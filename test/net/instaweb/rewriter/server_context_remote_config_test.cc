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

// Unit tests for remote configuration fetching in ServerContext
// (RemoteConfigurationUrl): which responses are applied, and that the last
// good configuration is kept when a refetch fails.

#include <memory>

#include "net/instaweb/http/public/async_fetch.h"
#include "net/instaweb/http/public/counting_url_async_fetcher.h"
#include "net/instaweb/http/public/wait_url_async_fetcher.h"
#include "net/instaweb/rewriter/public/rewrite_options.h"
#include "net/instaweb/rewriter/public/server_context.h"
#include "pagespeed/kernel/base/basictypes.h"
#include "pagespeed/kernel/base/string_util.h"
#include "pagespeed/kernel/base/timer.h"
#include "pagespeed/kernel/http/content_type.h"
#include "pagespeed/kernel/http/http_names.h"
#include "pagespeed/kernel/http/response_headers.h"
#include "test/net/instaweb/http/mock_url_fetcher.h"
#include "test/net/instaweb/rewriter/rewrite_test_base.h"
#include "test/net/instaweb/rewriter/test_rewrite_driver_factory.h"
#include "test/pagespeed/kernel/base/gtest.h"

namespace net_instaweb {

namespace {

const char kConfigUrl[] = "http://remote-config.test/pagespeed.conf";
const char kConfigBody[] =
    "EnableFilters remove_comments,collapse_whitespace\n"
    "EndRemoteConfig\n";
// What a failing configuration server answers with (not a configuration).
const char kGoneBody[] = "This resource is gone\n";
const int64 kConfigTtlSec = 5;
const int64 kLastModifiedMs = 1000000000000LL;  // Fixed, in the past.

class ServerContextRemoteConfigTest : public RewriteTestBase {
 protected:
  void SetUp() override {
    RewriteTestBase::SetUp();
    // Stamp each served response with the (mock) time it was fetched.
    FetcherUpdateDateHeaders();
  }

  // Makes the remote configuration URL answer with `status` and `body`,
  // cacheable for kConfigTtlSec seconds.
  void SetConfigResponse(HttpStatus::Code status, StringPiece body) {
    ResponseHeaders headers;
    headers.set_major_version(1);
    headers.set_minor_version(1);
    headers.SetStatusAndReason(status);
    headers.Add(HttpAttributes::kContentType, kContentTypeText.mime_type());
    headers.SetDateAndCaching(timer()->NowMs(),
                              kConfigTtlSec * Timer::kSecondMs);
    headers.ComputeCaching();
    SetFetchResponse(kConfigUrl, headers, body);
  }

  // Like SetConfigResponse(kOK, ...) but with a Last-Modified validator, so a
  // conditional refetch after expiry is answered with 304 Not Modified.
  void SetConditionalConfigResponse(StringPiece body) {
    ResponseHeaders headers;
    headers.set_major_version(1);
    headers.set_minor_version(1);
    headers.SetStatusAndReason(HttpStatus::kOK);
    headers.Add(HttpAttributes::kContentType, kContentTypeText.mime_type());
    headers.SetDateAndCaching(timer()->NowMs(),
                              kConfigTtlSec * Timer::kSecondMs);
    headers.SetLastModified(kLastModifiedMs);
    headers.ComputeCaching();
    mock_url_fetcher()->SetConditionalResponse(kConfigUrl, kLastModifiedMs, "",
                                               headers, body);
  }

  // Runs a per-request remote configuration lookup, as the server ports do,
  // and reports whether the fetched configuration was applied.
  bool RemoteConfigApplied(bool serve_stale_if_fetch_error = true) {
    std::unique_ptr<RewriteOptions> remote_options(
        server_context()->NewOptions());
    remote_options->set_serve_stale_if_fetch_error(serve_stale_if_fetch_error);
    remote_options->set_remote_configuration_url(kConfigUrl);
    remote_options->set_remote_configuration_timeout_ms(1000);
    EXPECT_FALSE(remote_options->Enabled(RewriteOptions::kRemoveComments));
    server_context()->GetRemoteOptions(remote_options.get(),
                                       false /* on_startup */);
    return remote_options->Enabled(RewriteOptions::kRemoveComments);
  }

  int64 FetchCount() { return counting_url_async_fetcher()->fetch_count(); }

  // Past the freshness lifetime of the cached configuration.
  void ExpireCachedConfig() {
    AdvanceTimeMs((kConfigTtlSec + 1) * Timer::kSecondMs);
  }
};

TEST_F(ServerContextRemoteConfigTest, SuccessResponseIsApplied) {
  SetConfigResponse(HttpStatus::kOK, kConfigBody);
  EXPECT_TRUE(RemoteConfigApplied());
  EXPECT_EQ(1, FetchCount());
  // A second request within the lifetime is served from cache.
  EXPECT_TRUE(RemoteConfigApplied());
  EXPECT_EQ(1, FetchCount());
}

TEST_F(ServerContextRemoteConfigTest, NotModifiedRevalidationIsApplied) {
  SetConditionalConfigResponse(kConfigBody);
  EXPECT_TRUE(RemoteConfigApplied());
  EXPECT_EQ(1, FetchCount());
  ExpireCachedConfig();
  // The refetch is conditional and answered with 304 Not Modified; the
  // cached configuration is revalidated and applied.
  EXPECT_TRUE(RemoteConfigApplied());
  EXPECT_EQ(2, FetchCount());
}

TEST_F(ServerContextRemoteConfigTest, ForbiddenResponseIsNotApplied) {
  SetConfigResponse(HttpStatus::kForbidden, kConfigBody);
  EXPECT_FALSE(RemoteConfigApplied());
  EXPECT_FALSE(RemoteConfigApplied());
}

TEST_F(ServerContextRemoteConfigTest, GoneResponseIsNotApplied) {
  SetConfigResponse(HttpStatus::kGone, kConfigBody);
  EXPECT_FALSE(RemoteConfigApplied());
}

TEST_F(ServerContextRemoteConfigTest, NotFoundResponseIsNotApplied) {
  SetConfigResponse(HttpStatus::kNotFound, kConfigBody);
  EXPECT_FALSE(RemoteConfigApplied());
}

TEST_F(ServerContextRemoteConfigTest, ServerErrorResponseIsNotApplied) {
  SetConfigResponse(HttpStatus::kInternalServerError, kConfigBody);
  EXPECT_FALSE(RemoteConfigApplied());
}

TEST_F(ServerContextRemoteConfigTest, RedirectResponseIsNotApplied) {
  SetConfigResponse(HttpStatus::kMovedPermanently, kConfigBody);
  EXPECT_FALSE(RemoteConfigApplied());
  EXPECT_FALSE(RemoteConfigApplied());
}

TEST_F(ServerContextRemoteConfigTest, RedirectRefetchKeepsLastGoodConfig) {
  SetConfigResponse(HttpStatus::kOK, kConfigBody);
  EXPECT_TRUE(RemoteConfigApplied());

  // A 301 is cacheable; it must not replace the last good copy.
  SetConfigResponse(HttpStatus::kMovedPermanently, "EndRemoteConfig\n");
  ExpireCachedConfig();
  EXPECT_TRUE(RemoteConfigApplied());
  EXPECT_EQ(2, FetchCount());
  EXPECT_TRUE(RemoteConfigApplied());
  EXPECT_EQ(3, FetchCount());
}

TEST_F(ServerContextRemoteConfigTest, OriginStaleWarningIsNotTakenAsStale) {
  // An uncacheable 200 that carries the same Warning value this module uses
  // to mark a stale copy is still a fresh configuration.
  ResponseHeaders headers;
  headers.set_major_version(1);
  headers.set_minor_version(1);
  headers.SetStatusAndReason(HttpStatus::kOK);
  headers.Add(HttpAttributes::kContentType, kContentTypeText.mime_type());
  headers.Add(HttpAttributes::kCacheControl, "no-cache");
  headers.Add(HttpAttributes::kWarning,
              FallbackSharedAsyncFetch::kStaleWarningHeaderValue);
  headers.ComputeCaching();
  SetFetchResponse(kConfigUrl, headers, kConfigBody);
  EXPECT_TRUE(RemoteConfigApplied());
}

TEST_F(ServerContextRemoteConfigTest, StartupFetchCompletingLaterIsCached) {
  // The startup fetch returns at once; its completion arrives after
  // FetchRemoteConfig has returned and still populates the cache.
  SetupWaitFetcher();
  SetConfigResponse(HttpStatus::kOK, kConfigBody);
  std::unique_ptr<RewriteOptions> startup_options(
      server_context()->NewOptions());
  startup_options->set_remote_configuration_url(kConfigUrl);
  server_context()->GetRemoteOptions(startup_options.get(),
                                     true /* on_startup */);
  EXPECT_FALSE(startup_options->Enabled(RewriteOptions::kRemoveComments));
  // The counting fetcher counts completed fetches: this one is still held.
  EXPECT_EQ(0, FetchCount());

  factory()->wait_url_async_fetcher()->CallCallbacks();
  factory()->wait_url_async_fetcher()->SetPassThroughMode(true);
  EXPECT_TRUE(RemoteConfigApplied());
  EXPECT_EQ(1, FetchCount());
}

TEST_F(ServerContextRemoteConfigTest, FailedRefetchKeepsLastGoodConfig) {
  SetConfigResponse(HttpStatus::kOK, kConfigBody);
  EXPECT_TRUE(RemoteConfigApplied());
  EXPECT_EQ(1, FetchCount());

  // The configuration server starts failing; the cached copy expires.
  SetConfigResponse(HttpStatus::kGone, kGoneBody);
  ExpireCachedConfig();
  EXPECT_TRUE(RemoteConfigApplied());
  EXPECT_EQ(2, FetchCount());
  // Still kept on the next request, which refetches again.
  AdvanceTimeMs(Timer::kMinuteMs);
  EXPECT_TRUE(RemoteConfigApplied());
  EXPECT_EQ(3, FetchCount());

  // Just under a day past its expiry it still applies ...
  AdvanceTimeMs(Timer::kDayMs - 2 * Timer::kMinuteMs);
  EXPECT_TRUE(RemoteConfigApplied());
  // ... and once it is stale beyond the one-day bound, it is dropped.
  AdvanceTimeMs(2 * Timer::kMinuteMs);
  EXPECT_FALSE(RemoteConfigApplied());
}

TEST_F(ServerContextRemoteConfigTest,
       NetworkFailureOnRefetchKeepsLastGoodConfig) {
  SetConfigResponse(HttpStatus::kOK, kConfigBody);
  EXPECT_TRUE(RemoteConfigApplied());

  // The configuration server becomes unreachable; the cached copy expires.
  mock_url_fetcher()->Disable();
  ExpireCachedConfig();
  EXPECT_TRUE(RemoteConfigApplied());
  EXPECT_EQ(2, FetchCount());

  AdvanceTimeMs(Timer::kDayMs);
  EXPECT_FALSE(RemoteConfigApplied());
}

TEST_F(ServerContextRemoteConfigTest, FailedRefetchRecoversWhenServerIsBack) {
  SetConfigResponse(HttpStatus::kOK, kConfigBody);
  EXPECT_TRUE(RemoteConfigApplied());
  SetConfigResponse(HttpStatus::kGone, kGoneBody);
  ExpireCachedConfig();
  EXPECT_TRUE(RemoteConfigApplied());

  // A new configuration from the recovered server replaces the kept copy.
  SetConfigResponse(HttpStatus::kOK, "EndRemoteConfig\n");
  EXPECT_FALSE(RemoteConfigApplied());
}

TEST_F(ServerContextRemoteConfigTest,
       FailedRefetchDropsConfigWithoutServeStaleIfFetchError) {
  SetConfigResponse(HttpStatus::kOK, kConfigBody);
  EXPECT_TRUE(RemoteConfigApplied(false));
  SetConfigResponse(HttpStatus::kGone, kGoneBody);
  ExpireCachedConfig();
  EXPECT_FALSE(RemoteConfigApplied(false));
}

TEST_F(ServerContextRemoteConfigTest, NoConfigWithoutAnyGoodFetch) {
  mock_url_fetcher()->Disable();
  EXPECT_FALSE(RemoteConfigApplied());
}

}  // namespace

}  // namespace net_instaweb
