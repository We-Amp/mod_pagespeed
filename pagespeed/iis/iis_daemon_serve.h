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


#ifndef PAGESPEED_IIS_IIS_DAEMON_SERVE_H_
#define PAGESPEED_IIS_IIS_DAEMON_SERVE_H_

#include <utility>
#include <vector>

#include "pagespeed/kernel/base/string.h"
#include "pagespeed/kernel/base/string_util.h"

struct IHttpContext;

namespace net_instaweb {

class GoogleUrl;
class RewriteOptions;
struct DaemonServeDecision;
struct DaemonServeRequest;

// The response this arm is about to write, as plain values: the status and
// the header fields in the order Apache's arm states them, so the whole
// composition can be read -- and tested -- without an IIS request to hand.
// `body` points into the bytes the peer's READER owns, so it is valid only
// while that reader is: a caller copies it out before releasing the reader.
struct IisServeResponse {
	int status = 0;
	std::vector<std::pair<GoogleString, GoogleString> > headers;
	StringPiece body;
	bool has_body = false;
	// What the entity's `Content-Length` states. A HEAD carries the length
	// its GET would return and no body, which is what a client asking a HEAD
	// is asking for.
	size_t content_length = 0;
};

// Does IIS compress this media type?  Fail-OPEN: an unknown type counts as
// compressible, because stating the encoding axis for a response IIS does
// not compress costs a cache one extra key, while omitting it for one IIS
// does compress hands two different bodies out under one key.
bool IisCompressibleMediaType(StringPiece media_type);

// Composes that response from the peer's decision.
//
// `compressible_media_type` is this port's answer for the media type the
// response will carry, which is what decides whether the `Vary` the arm
// states names the encoding axis. `head_request` drops the body and nothing
// else: a HEAD is answered with the headers its GET would carry.
//
// The `Vary` field names the encoding axis on the 304 leg ONLY: on a 200 the
// server's own compressor states it when it compresses, and appends that to
// whatever this arm wrote, which no notification of this module runs late
// enough to undo. See the comment beside the composition for the two cases
// that leaves and why the remaining one is the safe direction.
void ComposeIisServeResponse(const DaemonServeDecision& decision,
                             bool compressible_media_type, bool head_request,
                             IisServeResponse* response);

// The request facts the peer needs, read from the IIS request through the
// SAME capability reader the record arm uses, so the two arms cannot
// disagree about what this client asked for.
void BuildDaemonServeRequest(IHttpContext* http_context, const GoogleUrl& url,
                             DaemonServeRequest* request);

}  // namespace net_instaweb

#endif  // PAGESPEED_IIS_IIS_DAEMON_SERVE_H_
