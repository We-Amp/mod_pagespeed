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

// Unit tests for HttpCacheFailure: how a fetch result is classified, and how
// long each kind of failure is remembered. The point pinned here is the line
// between a failure that describes the resource (a 4xx the origin sent, kept
// for minutes) and one that describes the origin's condition right now (a
// fetch that did not complete, or a 5xx, kept for seconds).

#include "net/instaweb/http/public/http_cache_failure.h"

#include "test/pagespeed/kernel/base/gtest.h"
#include "pagespeed/kernel/base/string_util.h"
#include "pagespeed/kernel/http/http_names.h"
#include "pagespeed/kernel/http/response_headers.h"

namespace net_instaweb {

namespace {

class HttpCacheFailureTest : public testing::Test {
 protected:
  // A completed fetch (Done(true)) of a cacheable, non-empty response.
  static FetchResponseStatus Completed(int status_code) {
    ResponseHeaders headers;
    headers.set_status_code(status_code);
    return HttpCacheFailure::ClassifyFailure(headers, "body",
                                             true /* physical success */,
                                             true /* cacheable */);
  }

  // A fetch that did not complete (Done(false)), carrying whatever status
  // the fetcher left in the headers: 0 when it never got any, a code it made
  // up when it gave up before it had headers, or the origin's own code when
  // the body stalled after the headers.
  static FetchResponseStatus Incomplete(int status_code) {
    ResponseHeaders headers;
    headers.set_status_code(status_code);
    return HttpCacheFailure::ClassifyFailure(headers, "",
                                             false /* physical success */,
                                             false /* cacheable */);
  }
};

TEST_F(HttpCacheFailureTest, CompletedResponses) {
  EXPECT_EQ(kFetchStatusOK, Completed(HttpStatus::kOK));
  EXPECT_EQ(kFetchStatusOtherError, Completed(HttpStatus::kFound));
  // A 4xx the origin actually sent is about the resource.
  EXPECT_EQ(kFetchStatus4xxError, Completed(HttpStatus::kNotFound));
  EXPECT_EQ(kFetchStatus4xxError, Completed(HttpStatus::kGone));
  EXPECT_EQ(kFetchStatus4xxError, Completed(HttpStatus::kForbidden));
  // A 5xx is about the origin's condition right now.
  EXPECT_EQ(kFetchStatusTransientError,
            Completed(HttpStatus::kInternalServerError));
  EXPECT_EQ(kFetchStatusTransientError, Completed(HttpStatus::kBadGateway));
  EXPECT_EQ(kFetchStatusTransientError, Completed(HttpStatus::kUnavailable));
  EXPECT_EQ(kFetchStatusTransientError,
            Completed(HttpStatus::kGatewayTimeout));
}

TEST_F(HttpCacheFailureTest, IncompleteFetchesAreTransient) {
  // Never got headers at all.
  EXPECT_EQ(kFetchStatusTransientError, Incomplete(0));
  // The curl fetcher writes a 404 when it times out before it has headers;
  // that made-up status must not be remembered as a missing resource.
  EXPECT_EQ(kFetchStatusTransientError, Incomplete(HttpStatus::kNotFound));
  // The body stalled after a 200 came through.
  EXPECT_EQ(kFetchStatusTransientError, Incomplete(HttpStatus::kOK));
  EXPECT_EQ(kFetchStatusTransientError,
            Incomplete(HttpStatus::kInternalServerError));
}

TEST_F(HttpCacheFailureTest, LoadShedIsDropped) {
  ResponseHeaders headers;
  headers.set_status_code(HttpStatus::kUnavailable);
  headers.Add(HttpAttributes::kXPsaLoadShed, "1");
  EXPECT_EQ(kFetchStatusDropped,
            HttpCacheFailure::ClassifyFailure(headers, "", false, false));
}

TEST_F(HttpCacheFailureTest, UncacheableAndEmpty) {
  ResponseHeaders headers;
  headers.set_status_code(HttpStatus::kOK);
  EXPECT_EQ(kFetchStatusUncacheable200,
            HttpCacheFailure::ClassifyFailure(headers, "body", true, false));
  EXPECT_EQ(kFetchStatusEmpty,
            HttpCacheFailure::ClassifyFailure(headers, "", true, true));
}

TEST_F(HttpCacheFailureTest, PolicyKeepsResourceFailuresLongerThanTransient) {
  HttpCacheFailurePolicy policy;
  const int transient_sec = policy.ttl_sec_for_status[kFetchStatusTransientError];
  EXPECT_EQ(10, transient_sec);
  EXPECT_EQ(transient_sec, policy.ttl_sec_for_status[kFetchStatusDropped]);
  EXPECT_EQ(300, policy.ttl_sec_for_status[kFetchStatus4xxError]);
  EXPECT_EQ(300, policy.ttl_sec_for_status[kFetchStatusOtherError]);
  EXPECT_EQ(300, policy.ttl_sec_for_status[kFetchStatusUncacheable200]);
  EXPECT_EQ(300, policy.ttl_sec_for_status[kFetchStatusEmpty]);
}

TEST_F(HttpCacheFailureTest, FailureStatusCodesRoundTrip) {
  const FetchResponseStatus kFailures[] = {
      kFetchStatusUncacheable200, kFetchStatusUncacheableError,
      kFetchStatus4xxError,       kFetchStatusOtherError,
      kFetchStatusDropped,        kFetchStatusEmpty,
      kFetchStatusTransientError,
  };
  for (FetchResponseStatus status : kFailures) {
    HttpStatus::Code code = HttpCacheFailure::EncodeFailureCachingStatus(status);
    EXPECT_TRUE(HttpCacheFailure::IsFailureCachingStatus(code)) << status;
    EXPECT_EQ(status, HttpCacheFailure::DecodeFailureCachingStatus(code));
  }
  EXPECT_FALSE(HttpCacheFailure::IsFailureCachingStatus(HttpStatus::kOK));
  EXPECT_FALSE(HttpCacheFailure::IsFailureCachingStatus(HttpStatus::kNotFound));
}

}  // namespace

}  // namespace net_instaweb
