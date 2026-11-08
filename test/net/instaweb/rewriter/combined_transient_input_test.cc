// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 We-Amp B.V.

// Checks that a combined resource whose input was briefly unavailable is
// fetchable again once the failure's remembered window has passed, and not
// before.

#include "pagespeed/kernel/cache/lru_cache.h"
#include "pagespeed/kernel/base/string.h"
#include "pagespeed/kernel/base/timer.h"
#include "pagespeed/kernel/http/content_type.h"
#include "pagespeed/kernel/http/http_names.h"
#include "pagespeed/kernel/http/response_headers.h"
#include "test/net/instaweb/rewriter/rewrite_context_test_base.h"
#include "test/pagespeed/kernel/base/gtest.h"

namespace net_instaweb {

namespace {

class CombinedTransientInputTest : public RewriteContextTestBase {
 protected:
  GoogleString CombinedUrl() {
    return Encode(kTestDomain, CombiningFilter::kFilterId, "0",
                  MultiUrl("a.css", "b.css"), "css");
  }
  int64 TransientMs() {
    return http_cache()->failure_caching_ttl_sec(kFetchStatusTransientError) *
           Timer::kSecondMs;
  }
  void BreakB(HttpStatus::Code code) {
    ResponseHeaders h;
    SetDefaultLongCacheHeaders(&kContentTypeCss, &h);
    h.SetStatusAndReason(code);
    SetFetchResponse(AbsolutifyUrl("b.css"), h, "");
  }
  void FixB() {
    SetResponseWithDefaultHeaders("b.css", kContentTypeCss, "b", 100);
  }
  bool Fetch(GoogleString* content) {
    ResponseHeaders headers;
    content->clear();
    return FetchResourceUrl(CombinedUrl(), content, &headers);
  }
  // With the input already fixed: still refused just inside the window,
  // served just past it.
  void ExpectWindow(int64 window_ms) {
    GoogleString content;
    AdvanceTimeMs(window_ms - Timer::kSecondMs);
    EXPECT_FALSE(Fetch(&content));
    AdvanceTimeMs(2 * Timer::kSecondMs);
    EXPECT_TRUE(Fetch(&content));
    EXPECT_EQ(" a b", content);
  }
};

// The input answers 503 while the combined URL is requested.
TEST_F(CombinedTransientInputTest, Unavailable503ThenRecovers) {
  InitCombiningFilter(0);
  EnableDebug();
  InitResources();
  BreakB(HttpStatus::kUnavailable);
  GoogleString content;
  EXPECT_FALSE(Fetch(&content));
  FixB();
  ExpectWindow(TransientMs());
}

// The fetcher gave up: a made-up 404 plus Done(false). The fetch did not
// complete, so it is classified as transient rather than as a missing file.
TEST_F(CombinedTransientInputTest, TimeoutThenRecovers) {
  InitCombiningFilter(0);
  EnableDebug();
  InitResources();
  BreakB(HttpStatus::kNotFound);
  mock_url_fetcher()->SetResponseFailure(AbsolutifyUrl("b.css"));
  GoogleString content;
  EXPECT_FALSE(Fetch(&content));
  FixB();
  ExpectWindow(TransientMs());
}

// The combined URL was served before, the caches are gone, and the input is
// briefly down: the request then fails, a request after the window works.
TEST_F(CombinedTransientInputTest, PreviouslyServedThenInputBlips) {
  InitCombiningFilter(0);
  EnableDebug();
  InitResources();
  GoogleString content;
  EXPECT_TRUE(Fetch(&content));
  EXPECT_EQ(" a b", content);
  lru_cache()->Clear();
  BreakB(HttpStatus::kUnavailable);
  bool during = Fetch(&content);
  EXPECT_FALSE(during);
  FixB();
  ExpectWindow(TransientMs());
}

// A 404 the origin really sent. AddRecheckDependency applies per-status TTLs
// only for single-slot contexts, so for this two-input context the metadata
// input-error TTL governs; the HTTP cache's 4xx TTL is the other bound.
TEST_F(CombinedTransientInputTest, MissingThenAppears) {
  InitCombiningFilter(0);
  EnableDebug();
  InitResources();
  BreakB(HttpStatus::kNotFound);
  GoogleString content;
  EXPECT_FALSE(Fetch(&content));
  FixB();
  int64 http_4xx_ms =
      http_cache()->failure_caching_ttl_sec(kFetchStatus4xxError) *
      Timer::kSecondMs;
  ExpectWindow(
      std::max(http_4xx_ms, options()->metadata_input_errors_cache_ttl_ms()));
}

}  // namespace

}  // namespace net_instaweb
