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

}  // namespace net_instaweb
