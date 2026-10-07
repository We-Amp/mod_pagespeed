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

#ifndef PAGESPEED_IIS_IIS_DAEMON_RECORD_H_
#define PAGESPEED_IIS_IIS_DAEMON_RECORD_H_

#include <string>

#include "pagespeed/kernel/base/string.h"
#include "pagespeed/kernel/base/string_util.h"
#include "pagespeed/kernel/http/request_headers.h"

struct IHttpContext;

namespace net_instaweb {

class GoogleUrl;
class RewriteOptions;
struct DaemonRecordRequest;

// The authorization signals the record arm reads off an IIS request, as a
// plain value so the decision itself is testable without the IIS runtime.
// Every field is already the ANSWER to "is this signal present"; the
// conservative rule below is the only interpretation.
struct IisAuthFacts {
	// An Authorization header is on the request.
	bool has_authorization_header = false;
	// IIS has an authenticated user: GetUser() is non-null and its
	// authentication type is non-empty (anonymous access reports an empty
	// one).
	bool user_authenticated = false;
	// LOGON_USER / REMOTE_USER names a user.
	bool has_remote_user = false;
	// A TLS client certificate is present on the connection, or the
	// connection negotiated one (which IIS reports even when no certificate
	// arrived).
	bool has_client_certificate = false;
};

// Conservative by construction: any signal that says authenticated makes
// the request authorized, because a recorded response is placed in a cache
// other users' requests reach.  The one thing this must never do is read an
// authenticated request as anonymous.
bool IisRequestIsAuthorized(const IisAuthFacts& facts);

// The begin-time request properties: the header presence Apache builds its
// Properties from (Cookie, Cookie2, Authorization), taken from the raw
// request where they are visible from the first notification.
RequestHeaders::Properties IisBeginRequestProperties(bool has_cookie,
                                                    bool has_cookie2,
                                                    bool has_authorization);

// Reads the authorization signals off a request AFTER IIS authentication
// has run.  Connection-based Windows authentication sends no Authorization
// header on follow-up requests on one keep-alive connection, so the
// begin-time headers alone cannot answer; the caller takes this fact again
// at the point where recording is decided.
IisAuthFacts ReadIisAuthFacts(IHttpContext* http_context);

// The four request fields the peer's classifier derives a capability mask
// from.  ONE READER for both arms, and that is the point: the mask a request
// SELECTS with has to be the mask a recording of that same request would
// NOTIFY at; two independent readings of one client's headers would let the
// arms disagree about what the client can decode, and the disagreement would
// be invisible.  The cache key uses the pre-URL-rewrite URL both arms read
// (the request context's gurl()); nobody re-reads the raw request for the
// other arm.
void IisCapabilityHeaders(IHttpContext* http_context,
                          StringPiece* accept,
                          StringPiece* user_agent,
                          StringPiece* save_data,
                          StringPiece* accept_encoding);

// Fills the daemon record request from the request context's URL (the
// pre-URL-rewrite one), the raw request's headers, and the resolved
// options for this request.  Both halves or neither on the option context:
// a configuration that cannot be named records nothing and asks for
// nothing, which the gate answers.
void BuildDaemonRecordRequest(IHttpContext* http_context,
                              const GoogleUrl& url,
                              const RewriteOptions* options,
                              DaemonRecordRequest* request);

}  // namespace net_instaweb

#endif  // PAGESPEED_IIS_IIS_DAEMON_RECORD_H_
