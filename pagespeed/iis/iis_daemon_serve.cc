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


#include "pagespeed/iis/iis_daemon_serve.h"

#include "net/instaweb/rewriter/public/rewrite_options.h"
#include "pagespeed/iis/iis_daemon_record.h"
#include "pagespeed/kernel/base/time_util.h"
#include "pagespeed/kernel/http/content_type.h"
#include "pagespeed/kernel/http/google_url.h"
#include "pagespeed/kernel/http/http_names.h"
#include "pagespeed/system/daemon_serve_arm.h"

#ifdef _WIN32
#define _WINSOCKAPI_
#include <windows.h>
#include <httpserv.h>
#endif

namespace net_instaweb {

// Does IIS compress this media type?  The module already answers a version of
// this question where it decides whether a response may be handed on without
// copying (iis_module_base_fetch.cpp): the raster image types are the ones
// IIS leaves alone, and everything else -- notably the stylesheets and
// scripts this arm serves -- is a candidate for dynamic compression.
//
// The answer is deliberately fail-OPEN: an unknown type counts as
// compressible.  Stating the encoding axis for a response IIS then does not
// compress costs a cache one extra key; omitting it for one IIS does
// compress hands two different bodies out under one key.
bool IisCompressibleMediaType(StringPiece media_type) {
  const ContentType* type = MimeTypeToContentType(media_type);
  if (type == NULL) {
    return true;
  }
  switch (type->type()) {
    case ContentType::kPng:
    case ContentType::kGif:
    case ContentType::kJpeg:
    case ContentType::kWebp:
    case ContentType::kAvif:
      return false;
    default:
      return true;
  }
}

void ComposeIisServeResponse(const DaemonServeDecision& decision,
                             bool compressible_media_type, bool head_request,
                             IisServeResponse* response) {
  response->headers.clear();
  response->body = StringPiece();
  response->has_body = false;
  response->content_length = 0;
  response->status = decision.not_modified ? 304 : 200;

  if (!decision.cache_control.empty()) {
    response->headers.push_back(
        std::make_pair(GoogleString(HttpAttributes::kCacheControl),
                       decision.cache_control));
  }
  if (!decision.etag.empty()) {
    response->headers.push_back(
        std::make_pair(GoogleString(HttpAttributes::kEtag), decision.etag));
  }
  // THE ENCODING AXIS IS STATED ON THE 304 LEG ONLY, and that is not an
  // arrangement of the field but the whole of it on this port.  The server's
  // own compressor states `Vary: Accept-Encoding` on a 200 it compresses,
  // and it appends that to whatever this arm wrote -- measured: a response
  // this arm sent with the axis already named went out naming it twice, and
  // no notification this module can register runs late enough to take the
  // duplicate back out.  A 304 carries no body, so no compressor touches it
  // and the arm is the only possible emitter.
  //
  // Which leaves two cases and no third: the server compresses, and both
  // legs name the axis exactly once; or it does not, and the 304 names an
  // axis its 200 does not.  A cache updates what it stored from the 304
  // (RFC 9111 s4.3.4), so the second case widens the key it had -- the safe
  // direction, and the reason this is not mirrored per leg.
  GoogleString vary;
  if (decision.emit_vary_accept) {
    vary.assign(HttpAttributes::kAccept);
  }
  if (decision.not_modified && compressible_media_type) {
    if (!vary.empty()) {
      StrAppend(&vary, ", ");
    }
    StrAppend(&vary, HttpAttributes::kAcceptEncoding);
  }
  if (!vary.empty()) {
    response->headers.push_back(
        std::make_pair(GoogleString(HttpAttributes::kVary), vary));
  }
  // `Age` on EVERY hit, the 304 included: what goes out carries the entry's
  // remaining lifetime, and a cache told `max-age` without it keeps the
  // response for longer than the entry has left.
  response->headers.push_back(
      std::make_pair(GoogleString(HttpAttributes::kAge),
                     IntegerToString(static_cast<int>(decision.age_seconds))));

  if (decision.not_modified) {
    // A 304 carries no body and states no length: the caller writes headers
    // only.
    return;
  }
  if (!decision.content_type.empty()) {
    response->headers.push_back(std::make_pair(
        GoogleString(HttpAttributes::kContentType), decision.content_type));
  }
  // `Last-Modified` is the ORIGIN's validator, so it goes on origin bytes and
  // on nothing else: on an optimized variant it would advertise a validator
  // for a representation the origin never produced.
  if (decision.verdict == DaemonServeVerdict::kServeOriginal &&
      decision.origin_last_modified != 0) {
    GoogleString date;
    if (ConvertTimeToString(
            static_cast<int64>(decision.origin_last_modified) * 1000, &date)) {
      response->headers.push_back(std::make_pair(
          GoogleString(HttpAttributes::kLastModified), date));
    }
  }
  // A HEAD is answered with the headers its GET would carry -- the entity's
  // length included -- and no body.
  response->content_length = decision.body.size();
  if (!head_request) {
    response->body = decision.body;
    response->has_body = true;
  }
}

#ifdef _WIN32

void BuildDaemonServeRequest(IHttpContext* http_context, const GoogleUrl& url,
                             DaemonServeRequest* request) {
  request->url = url.PathAndLeaf();
  request->hostname = url.Host();
  request->scheme = url.Scheme();
  IisCapabilityHeaders(http_context, &request->accept, &request->user_agent,
                       &request->save_data, &request->accept_encoding);

  USHORT length = 0;
  PCSTR value = http_context->GetRequest()->GetHeader(
      HttpAttributes::kIfNoneMatch, &length);
  request->if_none_match = StringPiece(value == NULL ? "" : value,
                                       value == NULL ? 0 : length);
  value = http_context->GetRequest()->GetHeader(HttpAttributes::kIfModifiedSince,
                                                &length);
  request->if_modified_since =
      StringPiece(value == NULL ? "" : value, value == NULL ? 0 : length);

  // A forced reload, in both spellings a client still sends. It is handed to
  // the peer's freshness evaluator rather than acted on here: the peer
  // distinguishes "the client asked" from "the bytes are old", and only the
  // second means the entry expired.
  value = http_context->GetRequest()->GetHeader(HttpAttributes::kCacheControl,
                                                &length);
  const StringPiece cache_control(value == NULL ? "" : value,
                                  value == NULL ? 0 : length);
  value = http_context->GetRequest()->GetHeader(HttpAttributes::kPragma,
                                                &length);
  const StringPiece pragma(value == NULL ? "" : value,
                           value == NULL ? 0 : length);
  request->force_revalidate =
      cache_control.find("no-cache") != StringPiece::npos ||
      StringCaseEqual(pragma, "no-cache");
}

#endif  // _WIN32

}  // namespace net_instaweb
