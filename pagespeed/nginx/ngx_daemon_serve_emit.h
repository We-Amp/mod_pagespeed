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

#ifndef PAGESPEED_NGINX_NGX_DAEMON_SERVE_EMIT_H_
#define PAGESPEED_NGINX_NGX_DAEMON_SERVE_EMIT_H_

#include "pagespeed/kernel/base/basictypes.h"
#include "pagespeed/kernel/base/string.h"
#include "pagespeed/kernel/base/string_util.h"

namespace net_instaweb {

struct DaemonServeDecision;
class ResponseHeaders;

// The one composed response for the daemon substrate.  Deliberately
// nginx-free so the whole shape is unit tested
// (//test/pagespeed/nginx:ngx_daemon_serve_emit_test); nothing here knows
// what an ngx_http_request_t is.
//
// WHAT IT OWNS: everything the seam emits is computed here from the
// decision -- the status, the media type the response carries, and the
// `Vary` composition.  The composition is the BOTH-LEGS one
// (ServeVaryFieldValueOnBothLegs): nginx's compressor eligibility cannot be
// read from this module (the core gzip header filter decides after this
// module has composed its headers, and its floor, type list and switches are
// the operator's), so the arm states the encoding token on the 200 AND the
// 304 whenever the served media type is compressible, with no size floor.
// The arm's line is the only one on the wire: the module's filter that runs
// after the compressor withdraws the compressor's own `Vary` stamp whenever
// the module's value already lists the token, so the two legs agree by
// construction whatever the operator configures.  See
// ServeVaryFieldValueOnBothLegs in pagespeed/system/daemon_serve_arm.h.
// The caller owns the body (one request-pool buffer) and the send.

struct PsDaemonServeEmit {
  // The media type the response carries: the decision's own when it has
  // one, else the serving layer's type for the request (the caller's, from
  // ngx_http_set_content_type when it had to invent one).  A 200 must never
  // leave without a Content-Type; a 304 carries none.
  GoogleString media_type;
  // The composed `Vary` field value for the arm's own line, identical on
  // both legs; empty means the arm emits no `Vary` of its own (the served
  // type is not one the arm calls compressible and the body is not a
  // stored compressed copy; if the operator's configuration compresses it
  // anyway, the server states the token itself).  A stored compressed copy
  // states `Accept-Encoding` whatever its media type.
  GoogleString vary;
};

// Composes the response for the leg `decision.not_modified` selects.
//
// The 200: Content-Type (the resolved media type, never empty unless the
// caller passed none), Content-Encoding when the decision serves a stored
// compressed copy (its own coding), Cache-Control and ETag from the decision
// when present, Vary as composed, Age always, Content-Length as passed (the
// body the caller will send), and Last-Modified ONLY on an ORIGINAL serve
// with a stored origin date -- never on an optimized variant, whose bytes
// the origin never produced.
//
// The 304: status, and exactly the same Cache-Control/ETag/Vary/Age as the
// 200 would carry -- and nothing else.  No Content-Type, no Content-Encoding,
// no Last-Modified, no Content-Length, no body (the caller sends headers
// only).
//
// `server_compresses_type` is the serving layer's answer to "does the
// server's compressor handle this media type" -- on nginx, the module's own
// gzip setter's list (NgxGZipSetterCompressesType).  It is OR'd with
// ContentType::IsCompressible() on the resolved media type, and the result
// drives the encoding token on both legs.
PsDaemonServeEmit PsComposeDaemonServeHeaders(
    const DaemonServeDecision& decision, StringPiece request_media_type,
    int64 body_length, bool server_compresses_type, ResponseHeaders* out);

}  // namespace net_instaweb

#endif  // PAGESPEED_NGINX_NGX_DAEMON_SERVE_EMIT_H_
