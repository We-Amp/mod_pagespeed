/*
 * Licensed to the Apache Software Foundation (ASF) under one
 * or more contributor license agreements.  See the NOTICE file
 * distributed with this work for additional information
 * regarding copyright ownership.  The ASF licenses this file
 * to you under the Apache License, Version 2.0 (the
 * "License"); you may not use this file except in compliance
 * with the License.  You may obtain a copy of the License at
 *
 *   http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing,
 * software distributed under the License is distributed on an
 * "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
 * KIND, either express or implied.  See the License for the
 * specific language governing permissions and limitations
 * under the License.
 */

#include "pagespeed/nginx/ngx_daemon_serve_emit.h"

#include <algorithm>
#include <vector>

#include "pagespeed/kernel/base/timer.h"
#include "pagespeed/kernel/http/content_type.h"
#include "pagespeed/kernel/http/http_names.h"
#include "pagespeed/kernel/http/response_headers.h"
#include "pagespeed/system/daemon_serve_arm.h"

namespace net_instaweb {

PsDaemonServeEmit PsComposeDaemonServeHeaders(
    const DaemonServeDecision& decision, StringPiece request_media_type,
    int64 body_length, bool server_compresses_type, ResponseHeaders* out) {
  PsDaemonServeEmit emit;

  // The media type the response carries: the decision's own when it has
  // one, else the serving layer's type for the request.  The composed Vary
  // is evaluated against the SAME type the compressor would see.
  GoogleString request_type_storage(request_media_type);
  const char* request_mt =
      request_type_storage.empty() ? nullptr : request_type_storage.c_str();
  const char* media_type = ServeCompressorMediaType(decision, request_mt);
  if (media_type != nullptr) {
    emit.media_type.assign(media_type);
  }

  const ContentType* content_type = MimeTypeToContentType(emit.media_type);
  const bool compressible =
      server_compresses_type ||
      (content_type != nullptr && content_type->IsCompressible());
  // The both-legs composition: this server's compressor eligibility cannot
  // be read from here (nginx's core gzip filter decides after this module
  // has composed its headers, and its floor, type list and switches are the
  // operator's), so the arm states the encoding token on BOTH legs whenever
  // the served media type is compressible, with no size floor: the legs
  // agree and nothing an operator configures can narrow them.  The server
  // does not repeat the token (see the header).  See
  // ServeVaryFieldValueOnBothLegs in pagespeed/system/daemon_serve_arm.h.
  emit.vary = ServeVaryFieldValueOnBothLegs(decision, compressible);

  out->set_major_version(1);
  out->set_minor_version(1);
  if (decision.not_modified) {
    out->SetStatusAndReason(HttpStatus::kNotModified);
  } else {
    out->SetStatusAndReason(HttpStatus::kOK);
  }

  if (!decision.not_modified) {
    if (!emit.media_type.empty()) {
      out->Add(HttpAttributes::kContentType, emit.media_type);
    }
    out->Add(HttpAttributes::kContentLength, Integer64ToString(body_length));
    // A stored compressed copy is labelled with the coding its entry was
    // stored in -- the arm reads it off the entry, never off the request.
    // The header copy puts it in headers_out.content_encoding, where
    // nginx's gzip filter (and a brotli filter) see it and pass the body
    // through as it is.  The 304 carries no body and no label.
    if (!decision.content_encoding.empty()) {
      out->Add(HttpAttributes::kContentEncoding, decision.content_encoding);
    }
  }
  if (!decision.cache_control.empty()) {
    out->Add(HttpAttributes::kCacheControl, decision.cache_control);
  }
  if (!decision.etag.empty()) {
    out->Add(HttpAttributes::kEtag, decision.etag);
  }
  if (!emit.vary.empty()) {
    out->Add(HttpAttributes::kVary, emit.vary);
  }
  // Age rides on EVERY hit, including the 304: the Cache-Control carries
  // full remaining lifetime, so the entry's age must be stated with it.
  out->Add(HttpAttributes::kAge, Integer64ToString(decision.age_seconds));
  if (!decision.not_modified &&
      decision.verdict == DaemonServeVerdict::kServeOriginal &&
      decision.origin_last_modified != 0) {
    out->SetLastModified(decision.origin_last_modified * Timer::kSecondMs);
  }
  return emit;
}

namespace {

// Splits a Cache-Control value on the commas outside quoted strings.
void SplitCacheControlDirectives(StringPiece value,
                                 std::vector<StringPiece>* out) {
  size_t start = 0;
  bool quoted = false;
  for (size_t i = 0; i <= value.size(); ++i) {
    if (i == value.size() || (!quoted && value[i] == ',')) {
      // Only the leading whitespace is dropped here: a lifetime's value is
      // read exactly as sent, up to the comma.
      StringPiece directive = value.substr(start, i - start);
      while (!directive.empty() && IsHtmlSpace(directive[0])) {
        directive.remove_prefix(1);
      }
      StringPiece trimmed = directive;
      TrimWhitespace(&trimmed);
      if (!trimmed.empty()) {
        out->push_back(directive);
      }
      start = i + 1;
    } else if (value[i] == '\\' && quoted && i + 1 < value.size()) {
      ++i;
    } else if (value[i] == '"') {
      quoted = !quoted;
    }
  }
}

}  // namespace

GoogleString PsFallbackCacheControl(StringPiece existing, bool has_expires,
                                    int64 expires_seconds, int64 now_seconds) {
  if (has_expires && (expires_seconds < 0 || expires_seconds <= now_seconds)) {
    return GoogleString();
  }
  const int64 cap = static_cast<int64>(kServeDeviceFallbackMaxAgeSeconds);
  std::vector<StringPiece> directives;
  SplitCacheControlDirectives(existing, &directives);
  if (directives.empty()) {
    return GoogleString();
  }
  // The output, in first-appearance order.  A lifetime directive holds its
  // place and gets its value at the end, the smallest one stated.
  std::vector<GoogleString> kept;
  int64 max_age = -1;
  int64 s_maxage = -1;
  int max_age_at = -1;
  int s_maxage_at = -1;
  for (StringPiece raw : directives) {
    StringPiece directive = raw;
    TrimWhitespace(&directive);
    StringPiece name = raw;
    StringPiece value;
    const size_t eq = raw.find('=');
    if (eq != StringPiece::npos) {
      name = raw.substr(0, eq);
      value = raw.substr(eq + 1);
      TrimWhitespace(&name);
    }
    if (StringCaseEqual(name, "private") || StringCaseEqual(name, "no-store") ||
        StringCaseEqual(name, "no-cache")) {
      return GoogleString();
    }
    const bool is_max_age = StringCaseEqual(name, "max-age");
    if (is_max_age || StringCaseEqual(name, "s-maxage")) {
      // delta-seconds (RFC 9111 1.2.2): one or more ASCII digits and nothing
      // else.  A sign, whitespace or any other character is unreadable.
      bool digits = !value.empty();
      for (char ch : value) {
        digits = digits && ch >= '0' && ch <= '9';
      }
      int64 seconds = 0;
      if (!digits || !StringToInt64(value, &seconds)) {
        return GoogleString();
      }
      seconds = std::min(seconds, cap);
      int64* slot = is_max_age ? &max_age : &s_maxage;
      int* at = is_max_age ? &max_age_at : &s_maxage_at;
      if (*at < 0) {
        *at = static_cast<int>(kept.size());
        kept.push_back(GoogleString());
        *slot = seconds;
      } else {
        *slot = std::min(*slot, seconds);
      }
      continue;
    }
    bool duplicate = false;
    for (const GoogleString& k : kept) {
      if (StringCaseEqual(k, directive)) {
        duplicate = true;
        break;
      }
    }
    if (!duplicate) {
      kept.push_back(directive.as_string());
    }
  }
  if (max_age_at < 0) {
    max_age_at = static_cast<int>(kept.size());
    kept.push_back(GoogleString());
    max_age = cap;
  }
  kept[max_age_at] = StrCat("max-age=", Integer64ToString(max_age));
  if (s_maxage_at >= 0) {
    kept[s_maxage_at] = StrCat("s-maxage=", Integer64ToString(s_maxage));
  }
  GoogleString line;
  for (const GoogleString& k : kept) {
    if (!line.empty()) {
      StrAppend(&line, ", ");
    }
    StrAppend(&line, k);
  }
  return line;
}

}  // namespace net_instaweb
