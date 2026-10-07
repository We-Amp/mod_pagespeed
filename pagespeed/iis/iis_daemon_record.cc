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

#include "pagespeed/iis/iis_daemon_record.h"


#include "net/instaweb/rewriter/public/option_context.h"
#include "net/instaweb/rewriter/public/rewrite_options.h"
#include "pagespeed/kernel/http/google_url.h"
#include "pagespeed/kernel/http/http_names.h"
#include "pagespeed/system/daemon_ipro_recorder.h"

#ifdef _WIN32
#define _WINSOCKAPI_
#include <httpserv.h>
#endif

namespace net_instaweb {

bool IisRequestIsAuthorized(const IisAuthFacts& facts) {
  return facts.has_authorization_header || facts.user_authenticated ||
         facts.has_remote_user || facts.has_client_certificate;
}

RequestHeaders::Properties IisBeginRequestProperties(bool has_cookie,
                                                     bool has_cookie2,
                                                     bool has_authorization) {
  return RequestHeaders::Properties(has_cookie, has_cookie2, has_authorization);
}

#ifdef _WIN32

namespace {

// A header counts as present when the raw request has it at all, even with
// an empty value -- the presence is the fact, not the content.
bool HasHeader(IHttpContext* http_context, const char* name) {
  USHORT length = 0;
  PCSTR value = http_context->GetRequest()->GetHeader(name, &length);
  return value != NULL;
}

// A server variable counts as naming a user when it is there and spells
// more than whitespace.  The narrow name / wide value spelling of the
// lookup is the one this port already uses elsewhere (the pristine-URL
// read).
bool ServerVariableNamesUser(IHttpContext* http_context, const char* name) {
  PCWSTR value = NULL;
  DWORD length = 0;
  if (http_context->GetRootContext()->GetServerVariable(name, &value,
                                                        &length) != S_OK) {
    return false;
  }
  if (value == NULL || length == 0) {
    return false;
  }
  for (DWORD i = 0; i < length; ++i) {
    if (value[i] != L' ' && value[i] != L'\t') {
      return true;
    }
  }
  return false;
}

// A wide string counts as naming a user when it spells more than
// whitespace.
bool NamesUser(PCWSTR value) {
  if (value == NULL) {
    return false;
  }
  for (PCWSTR at = value; *at != L'\0'; ++at) {
    if (*at != L' ' && *at != L'\t') {
      return true;
    }
  }
  return false;
}

}  // namespace

IisAuthFacts ReadIisAuthFacts(IHttpContext* http_context) {
  IisAuthFacts facts;
  facts.has_authorization_header =
      HasHeader(http_context, HttpAttributes::kAuthorization);
  IHttpUser* user = http_context->GetUser();
  if (user != NULL) {
    // Anonymous access reports an empty authentication type; a
    // connection-authenticated follow-up request reports the scheme
    // with no Authorization header anywhere.
    facts.user_authenticated = NamesUser(user->GetAuthenticationType());
    facts.has_remote_user = NamesUser(user->GetRemoteUserName());
  }
  if (!facts.has_remote_user) {
    facts.has_remote_user =
        ServerVariableNamesUser(http_context, "LOGON_USER") ||
        ServerVariableNamesUser(http_context, "REMOTE_USER");
  }
  // A negotiated handshake with no certificate (clientCertificate="Accept")
  // counts too: it is the safe direction, at the cost of not recording on
  // such a site.
  HTTP_SSL_CLIENT_CERT_INFO* certificate_info = NULL;
  BOOL certificate_negotiated = FALSE;
  http_context->GetRequest()->GetClientCertificate(&certificate_info,
                                                   &certificate_negotiated);
  facts.has_client_certificate =
      certificate_negotiated || certificate_info != NULL;
  return facts;
}

void IisCapabilityHeaders(IHttpContext* http_context, StringPiece* accept,
                          StringPiece* user_agent, StringPiece* save_data,
                          StringPiece* accept_encoding) {
  USHORT length = 0;
  PCSTR value =
      http_context->GetRequest()->GetHeader(HttpAttributes::kAccept, &length);
  *accept = StringPiece(value == NULL ? "" : value, value == NULL ? 0 : length);
  value = http_context->GetRequest()->GetHeader(HttpAttributes::kUserAgent,
                                                &length);
  *user_agent =
      StringPiece(value == NULL ? "" : value, value == NULL ? 0 : length);
  value =
      http_context->GetRequest()->GetHeader(HttpAttributes::kSaveData, &length);
  *save_data =
      StringPiece(value == NULL ? "" : value, value == NULL ? 0 : length);
  value = http_context->GetRequest()->GetHeader(HttpAttributes::kAcceptEncoding,
                                                &length);
  *accept_encoding =
      StringPiece(value == NULL ? "" : value, value == NULL ? 0 : length);
}

void BuildDaemonRecordRequest(IHttpContext* http_context, const GoogleUrl& url,
                              const RewriteOptions* options,
                              DaemonRecordRequest* request) {
  request->request_properties = IisBeginRequestProperties(
      HasHeader(http_context, HttpAttributes::kCookie),
      HasHeader(http_context, HttpAttributes::kCookie2),
      HasHeader(http_context, HttpAttributes::kAuthorization));
  url.PathAndLeaf().CopyToString(&request->url);
  url.Host().CopyToString(&request->hostname);
  url.Scheme().CopyToString(&request->scheme);

  StringPiece accept, user_agent, save_data, accept_encoding;
  IisCapabilityHeaders(http_context, &accept, &user_agent, &save_data,
                       &accept_encoding);
  accept.CopyToString(&request->accept);
  user_agent.CopyToString(&request->user_agent);
  save_data.CopyToString(&request->save_data);
  accept_encoding.CopyToString(&request->accept_encoding);

  // The resolved configuration for THIS request, stated as a value.  Both
  // halves or neither: a payload with no signature has no name, and a
  // refusal here is not an error -- the record arm responds by keeping
  // the original and asking for nothing, which the gate answers.
  if (options != NULL &&
      OptionContext::Compute(*options, &request->option_context,
                             &request->option_signature) !=
          OptionContextStatus::kOk) {
    request->option_context.clear();
    request->option_signature.clear();
  }
}

#else  // _WIN32

// The IIS runtime headers are unavailable off Windows; the pure builders
// above are what the unit tests drive, and the runtime functions exist only
// for the module, which builds on Windows.  The stub fails CLOSED: with no
// way to read the authorization facts, the request counts as authorized and
// nothing is recorded.
IisAuthFacts ReadIisAuthFacts(IHttpContext*) {
  IisAuthFacts facts;
  facts.has_authorization_header = true;
  facts.user_authenticated = true;
  facts.has_remote_user = true;
  facts.has_client_certificate = true;
  return facts;
}

void IisCapabilityHeaders(IHttpContext*, StringPiece*, StringPiece*,
                          StringPiece*, StringPiece*) {}

void BuildDaemonRecordRequest(IHttpContext*, const GoogleUrl&,
                              const RewriteOptions*, DaemonRecordRequest*) {}

#endif  // _WIN32

}  // namespace net_instaweb
