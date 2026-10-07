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

// Unit tests for the daemon record arm's request facts: the authorization
// decision and the begin-time properties, in their pure form.  The
// IHttpContext plumbing cannot be linked into a test (it sits behind the
// IIS runtime headers); the seam pin covers the call shape.

#include "pagespeed/iis/iis_daemon_record.h"

#include "gtest/gtest.h"
#include "pagespeed/kernel/base/string_util.h"

namespace net_instaweb {

namespace {

TEST(IisDaemonRecordTest, AnyAuthenticationSignalAuthorizesTheRequest) {
  // The decision is an OR over every signal, because a recorded response is
  // placed in a cache other users' requests reach: reading an authenticated
  // request as anonymous is the one failure that matters.
  IisAuthFacts anonymous;
  EXPECT_FALSE(IisRequestIsAuthorized(anonymous));

  IisAuthFacts authorization;
  authorization.has_authorization_header = true;
  EXPECT_TRUE(IisRequestIsAuthorized(authorization));

  IisAuthFacts authenticated_user;
  authenticated_user.user_authenticated = true;
  EXPECT_TRUE(IisRequestIsAuthorized(authenticated_user));

  IisAuthFacts remote_user;
  remote_user.has_remote_user = true;
  EXPECT_TRUE(IisRequestIsAuthorized(remote_user));

  IisAuthFacts client_certificate;
  client_certificate.has_client_certificate = true;
  EXPECT_TRUE(IisRequestIsAuthorized(client_certificate));
}

TEST(IisDaemonRecordTest, BeginPropertiesFollowHeaderPresence) {
  // The begin-time properties are the header presence Apache builds its
  // Properties from; the authorization AXIS is re-taken after IIS
  // authentication at the response site, so the header fact here is only
  // half of it by design.
  RequestHeaders::Properties none =
      IisBeginRequestProperties(false, false, false);
  EXPECT_FALSE(none.has_cookie);
  EXPECT_FALSE(none.has_cookie2);
  EXPECT_FALSE(none.has_authorization);

  RequestHeaders::Properties all = IisBeginRequestProperties(true, true, true);
  EXPECT_TRUE(all.has_cookie);
  EXPECT_TRUE(all.has_cookie2);
  EXPECT_TRUE(all.has_authorization);
}

}  // namespace

}  // namespace net_instaweb
