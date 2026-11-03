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

#include "pagespeed/system/ipro_recorder.h"

#include <utility>

#include "pagespeed/kernel/base/basictypes.h"
#include "pagespeed/kernel/base/string_util.h"
#include "pagespeed/kernel/http/http_names.h"

namespace net_instaweb {

const char* const IproRecorder::kOriginCachingHeaderNames[] = {
    HttpAttributes::kCacheControl, HttpAttributes::kExpires,
    HttpAttributes::kPragma,       HttpAttributes::kSetCookie,
    HttpAttributes::kSetCookie2,   HttpAttributes::kVary,
};
const int IproRecorder::kNumOriginCachingHeaderNames =
    static_cast<int>(arraysize(kOriginCachingHeaderNames));

namespace {

bool IsOriginCachingHeader(StringPiece name, bool* is_vary) {
  for (int i = 0; i < IproRecorder::kNumOriginCachingHeaderNames; ++i) {
    if (StringCaseEqual(name, IproRecorder::kOriginCachingHeaderNames[i])) {
      *is_vary = StringCaseEqual(name, HttpAttributes::kVary);
      return true;
    }
  }
  return false;
}

// Every token of every Vary line, lower-cased.
void VaryTokens(const ResponseHeaders& headers, StringSet* tokens) {
  for (int i = 0, n = headers.NumAttributes(); i < n; ++i) {
    if (!StringCaseEqual(headers.Name(i), HttpAttributes::kVary)) {
      continue;
    }
    StringPieceVector parts;
    SplitStringPieceToVector(headers.Value(i), ",", &parts,
                             true /* omit_empty_strings */);
    for (StringPiece part : parts) {
      TrimWhitespace(&part);
      if (!part.empty()) {
        GoogleString token = part.as_string();
        LowerString(&token);
        tokens->insert(token);
      }
    }
  }
}

// Whether `headers` state a freshness lifetime of their own: an Expires
// line, or a max-age or s-maxage directive.
bool StatesLifetime(const ResponseHeaders& headers) {
  if (headers.Has(HttpAttributes::kExpires)) {
    return true;
  }
  ConstStringStarVector values;
  if (headers.Lookup(HttpAttributes::kCacheControl, &values)) {
    for (const GoogleString* value : values) {
      if (value != nullptr && (StringCaseStartsWith(*value, "max-age") ||
                               StringCaseStartsWith(*value, "s-maxage"))) {
        return true;
      }
    }
  }
  return false;
}

}  // namespace

// Out of line on purpose, and not merely as style: this is the interface's
// key function, so it is what anchors the vtable in one translation unit
// instead of emitting a copy into every one that sees the class.
IproRecorder::~IproRecorder() = default;

void IproRecorder::SetOriginCachingHeaders(const ResponseHeaders& origin) {
  // Raw lines, copied as they are: a parsed view would split an Expires date
  // or a cookie at its commas.
  std::unique_ptr<ResponseHeaders> copy(new ResponseHeaders);
  bool states_caching = false;
  for (int i = 0, n = origin.NumAttributes(); i < n; ++i) {
    bool is_vary = false;
    if (IsOriginCachingHeader(origin.Name(i), &is_vary)) {
      copy->Add(origin.Name(i), origin.Value(i));
      states_caching =
          states_caching ||
          StringCaseEqual(origin.Name(i), HttpAttributes::kCacheControl) ||
          StringCaseEqual(origin.Name(i), HttpAttributes::kExpires) ||
          StringCaseEqual(origin.Name(i), HttpAttributes::kPragma);
    }
  }
  // An origin that said nothing about caching (a static file, most often)
  // has no statement to compare: the server's caching headers are then the
  // only statement there is, and the final headers decide alone.
  if (states_caching) {
    origin_caching_headers_ = std::move(copy);
  } else {
    origin_caching_headers_.reset();
  }
}

bool IproRecorder::ApplyOriginCachingHeaders(
    ResponseHeaders* headers, const GoogleString* saved_cache_control) const {
  if (origin_caching_headers_ == nullptr || headers == nullptr) {
    return false;
  }
  // The server's lifetime, from a copy: computing caching may add fields.
  int64 server_ttl_ms = -1;
  {
    ResponseHeaders server(headers->http_options());
    server.CopyFrom(*headers);
    if (saved_cache_control != nullptr) {
      server.RemoveAll(HttpAttributes::kCacheControl);
      if (!saved_cache_control->empty()) {
        server.Add(HttpAttributes::kCacheControl, *saved_cache_control);
      }
    }
    if (StatesLifetime(server)) {
      server.ComputeCaching();
      server_ttl_ms = server.cache_ttl_ms();
    }
  }
  StringSet final_vary;
  VaryTokens(*headers, &final_vary);
  for (int i = 0; i < kNumOriginCachingHeaderNames; ++i) {
    if (!StringCaseEqual(kOriginCachingHeaderNames[i], HttpAttributes::kVary)) {
      headers->RemoveAll(kOriginCachingHeaderNames[i]);
    }
  }
  StringSet origin_vary;
  VaryTokens(*origin_caching_headers_, &origin_vary);
  for (int i = 0, n = origin_caching_headers_->NumAttributes(); i < n; ++i) {
    bool is_vary = false;
    IsOriginCachingHeader(origin_caching_headers_->Name(i), &is_vary);
    if (!is_vary) {
      headers->Add(origin_caching_headers_->Name(i),
                   origin_caching_headers_->Value(i));
    }
  }
  for (const GoogleString& token : origin_vary) {
    if (final_vary.find(token) == final_vary.end()) {
      headers->Add(HttpAttributes::kVary, token);
    }
  }
  headers->ComputeCaching();
  if (server_ttl_ms >= 0 && server_ttl_ms < headers->cache_ttl_ms()) {
    headers->SetCacheControlMaxAge(server_ttl_ms);
    headers->ComputeCaching();
  }
  return true;
}

}  // namespace net_instaweb
