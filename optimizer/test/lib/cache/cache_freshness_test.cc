// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

#include <cstdint>
#include <limits>

#include "gtest/gtest.h"
#include "lib/cache/freshness.h"
#include "lib/classify/alternate_metadata.h"

namespace pagespeed {
namespace {

using AM = AlternateMetadata;

// Fixed timestamp for deterministic, reproducible tests.
static constexpr uint32_t kTestNow = 1700000000;  // 2023-11-14

class CacheFreshnessTest : public ::testing::Test {
 protected:
  // Returns a default FreshnessInput with now_seconds set.
  // Tests set specific fields explicitly for clarity.
  FreshnessInput MakeInput() {
    FreshnessInput input{};
    input.now_seconds = kTestNow;
    input.cache_inserted_at = kTestNow;
    input.is_shared_cache = true;
    input.content_type = ContentType::kImage;  // Sensible default
    return input;
  }

  FreshnessConfig config_;  // Default config values
};

// --- Basic freshness verdicts ---

TEST_F(CacheFreshnessTest, FreshEntryServedDirectly) {
  auto input = MakeInput();
  input.origin_max_age = 3600;
  input.origin_cc_flags = AM::kCCOriginHeaderPresent;
  auto result = EvaluateFreshness(input, config_);
  EXPECT_EQ(result.verdict, FreshnessVerdict::kFresh);
  EXPECT_EQ(result.age_seconds, 0u);
  EXPECT_FALSE(result.is_stale);
  EXPECT_FALSE(result.expired_by_age);
}

TEST_F(CacheFreshnessTest, StaleWithMustRevalidateGoesToOrigin) {
  auto input = MakeInput();
  input.cache_inserted_at = kTestNow - 7200;
  input.origin_max_age = 3600;
  input.origin_cc_flags =
      AM::kCCOriginHeaderPresent | AM::kCCOriginMustRevalidate;
  auto result = EvaluateFreshness(input, config_);
  EXPECT_EQ(result.verdict, FreshnessVerdict::kRevalidate);
  EXPECT_TRUE(result.is_stale);
  // Genuine age expiry — eligible for the origin-refreshed purge.
  EXPECT_TRUE(result.expired_by_age);
}

// Issue #652: stale content without a revalidation directive used to be
// served indefinitely (kStaleServe); it now revalidates like all stale
// content (conditional re-fetch with validators, full re-fetch without).
TEST_F(CacheFreshnessTest, StaleWithoutRevalidationDirectiveRevalidates) {
  auto input = MakeInput();
  input.cache_inserted_at = kTestNow - 400;
  input.origin_max_age = 300;
  input.origin_cc_flags = AM::kCCOriginHeaderPresent;
  auto result = EvaluateFreshness(input, config_);
  EXPECT_EQ(result.verdict, FreshnessVerdict::kRevalidate);
  EXPECT_TRUE(result.is_stale);
  EXPECT_EQ(result.remaining_ttl, 0u);
  EXPECT_TRUE(result.expired_by_age);
}

// Issue #652 repro headers: HTML with "public, max-age=30" at age 79 — the
// exact configuration that produced the stale-serve in the WordPress
// integration test.  Must revalidate, not serve.
TEST_F(CacheFreshnessTest, Issue652HtmlMaxAge30Age79Revalidates) {
  auto input = MakeInput();
  input.content_type = ContentType::kHtml;
  input.cache_inserted_at = kTestNow - 79;
  input.origin_max_age = 30;
  input.origin_cc_flags = AM::kCCOriginHeaderPresent;
  auto result = EvaluateFreshness(input, config_);
  EXPECT_EQ(result.verdict, FreshnessVerdict::kRevalidate);
  EXPECT_TRUE(result.is_stale);
  EXPECT_EQ(result.age_seconds, 79u);
  EXPECT_TRUE(result.expired_by_age);
}

// Within-TTL counterpart of the issue #652 headers: unchanged behavior.
TEST_F(CacheFreshnessTest, Issue652HtmlMaxAge30WithinTtlFresh) {
  auto input = MakeInput();
  input.content_type = ContentType::kHtml;
  input.cache_inserted_at = kTestNow - 10;
  input.origin_max_age = 30;
  input.origin_cc_flags = AM::kCCOriginHeaderPresent;
  auto result = EvaluateFreshness(input, config_);
  EXPECT_EQ(result.verdict, FreshnessVerdict::kFresh);
  EXPECT_FALSE(result.is_stale);
  EXPECT_EQ(result.remaining_ttl, 20u);
}

TEST_F(CacheFreshnessTest, NoCacheAlwaysRevalidates) {
  auto input = MakeInput();
  input.origin_max_age = 3600;
  input.origin_cc_flags = AM::kCCOriginHeaderPresent | AM::kCCOriginNoCache;
  auto result = EvaluateFreshness(input, config_);
  EXPECT_EQ(result.verdict, FreshnessVerdict::kRevalidate);
  // no-cache is permanently "stale" by definition, NOT age-expired — it
  // must never trigger the origin-refreshed variant purge (issue #652
  // review: a very common CMS default would otherwise churn purge →
  // re-optimize every rate-limit window with zero client involvement).
  EXPECT_FALSE(result.expired_by_age);
}

// --- no-store / private / defensive ---

TEST_F(CacheFreshnessTest, NoCacheMaxAgeExpiredIsNotExpiredByAge) {
  // `no-cache, max-age=N` past N: revalidates, but expired_by_age stays
  // false, so the coalesced stale serve (which requires it) never applies.
  auto input = MakeInput();
  input.cache_inserted_at = kTestNow - 7200;
  input.origin_max_age = 3600;
  input.origin_cc_flags = AM::kCCOriginHeaderPresent | AM::kCCOriginNoCache;
  auto result = EvaluateFreshness(input, config_);
  EXPECT_EQ(result.verdict, FreshnessVerdict::kRevalidate);
  EXPECT_FALSE(result.expired_by_age);
}

TEST_F(CacheFreshnessTest, NoStoreServedAsNoCache) {
  auto input = MakeInput();
  input.origin_cc_flags = AM::kCCOriginHeaderPresent | AM::kCCOriginNoStore;
  auto result = EvaluateFreshness(input, config_);
  EXPECT_EQ(result.verdict, FreshnessVerdict::kServeNoCache);
}

TEST_F(CacheFreshnessTest, PrivateServedAsNoCacheInSharedCache) {
  auto input = MakeInput();
  input.origin_cc_flags = AM::kCCOriginHeaderPresent | AM::kCCOriginPrivate;
  input.is_shared_cache = true;
  auto result = EvaluateFreshness(input, config_);
  EXPECT_EQ(result.verdict, FreshnessVerdict::kServeNoCache);
}

TEST_F(CacheFreshnessTest, PrivateFreshInPrivateCache) {
  auto input = MakeInput();
  input.origin_max_age = 3600;
  input.origin_cc_flags = AM::kCCOriginHeaderPresent | AM::kCCOriginPrivate;
  input.is_shared_cache = false;
  auto result = EvaluateFreshness(input, config_);
  EXPECT_EQ(result.verdict, FreshnessVerdict::kFresh);
}

// --- s-maxage ---

TEST_F(CacheFreshnessTest, SMaxageTakesPrecedenceOverMaxAge) {
  auto input = MakeInput();
  input.cache_inserted_at = kTestNow - 120;
  input.origin_max_age = 3600;
  input.origin_s_maxage = 60;
  input.origin_cc_flags =
      AM::kCCOriginHeaderPresent | AM::kCCOriginSMaxagePresent;
  auto result = EvaluateFreshness(input, config_);
  // s-maxage=60, age=120 -> stale; s-maxage implies must-revalidate
  EXPECT_EQ(result.verdict, FreshnessVerdict::kRevalidate);
  EXPECT_EQ(result.effective_max_age, 60u);
}

TEST_F(CacheFreshnessTest, SMaxageIgnoredByPrivateCache) {
  auto input = MakeInput();
  input.cache_inserted_at = kTestNow - 120;
  input.origin_max_age = 3600;
  input.origin_s_maxage = 60;
  input.origin_cc_flags =
      AM::kCCOriginHeaderPresent | AM::kCCOriginSMaxagePresent;
  input.is_shared_cache = false;
  auto result = EvaluateFreshness(input, config_);
  // Private cache uses max-age=3600, not s-maxage=60
  EXPECT_EQ(result.verdict, FreshnessVerdict::kFresh);
}

TEST_F(CacheFreshnessTest, SMaxageZeroImmediatelyStale) {
  auto input = MakeInput();
  input.cache_inserted_at = kTestNow - 1;
  input.origin_max_age = 3600;
  input.origin_s_maxage = 0;
  input.origin_cc_flags =
      AM::kCCOriginHeaderPresent | AM::kCCOriginSMaxagePresent;
  auto result = EvaluateFreshness(input, config_);
  EXPECT_EQ(result.verdict, FreshnessVerdict::kRevalidate);
}

// --- Timestamp edge cases ---

TEST_F(CacheFreshnessTest, FutureTimestampTreatedAsRevalidate) {
  auto input = MakeInput();
  input.cache_inserted_at = kTestNow + 3600;  // Far future
  input.origin_max_age = 86400;
  input.origin_cc_flags =
      AM::kCCOriginHeaderPresent | AM::kCCOriginMustRevalidate;
  auto result = EvaluateFreshness(input, config_);
  // Corrupted timestamp -> age=UINT32_MAX -> stale + must-revalidate
  EXPECT_EQ(result.verdict, FreshnessVerdict::kRevalidate);
}

TEST_F(CacheFreshnessTest, ClockSkewSmallNegativeClampsToZero) {
  auto input = MakeInput();
  input.cache_inserted_at = kTestNow + 30;  // Within 60s tolerance
  input.origin_max_age = 3600;
  input.origin_cc_flags = AM::kCCOriginHeaderPresent;
  auto result = EvaluateFreshness(input, config_);
  EXPECT_EQ(result.age_seconds, 0u);
  EXPECT_EQ(result.verdict, FreshnessVerdict::kFresh);
}

TEST_F(CacheFreshnessTest, CacheInsertedAtZeroProducesLargeAge) {
  auto input = MakeInput();
  input.cache_inserted_at = 0;
  input.origin_max_age = 86400;
  input.origin_cc_flags = AM::kCCOriginHeaderPresent;
  auto result = EvaluateFreshness(input, config_);
  EXPECT_EQ(result.age_seconds, kTestNow);
  EXPECT_TRUE(result.is_stale);
}

// --- Content-type defaults ---

TEST_F(CacheFreshnessTest, DefaultTtlHtmlServesAsNoCache) {
  auto input = MakeInput();
  input.cache_inserted_at = kTestNow - 1;
  input.content_type = ContentType::kHtml;
  // No CC header, html_max_age defaults to 0 -> serve as no-cache
  auto result = EvaluateFreshness(input, config_);
  EXPECT_EQ(result.verdict, FreshnessVerdict::kServeNoCache);
}

TEST_F(CacheFreshnessTest, DefaultTtlCssExpiredRevalidates) {
  auto input = MakeInput();
  input.cache_inserted_at = kTestNow - 301;
  input.content_type = ContentType::kCss;
  // No CC header -> default css_max_age=300, age=301 -> stale -> revalidate
  // (issue #652: type-defaulted lifetimes expire like origin-set ones).
  auto result = EvaluateFreshness(input, config_);
  EXPECT_EQ(result.verdict, FreshnessVerdict::kRevalidate);
  EXPECT_TRUE(result.is_stale);
}

TEST_F(CacheFreshnessTest, DefaultTtlImageFresh) {
  auto input = MakeInput();
  input.cache_inserted_at = kTestNow - 100;
  input.content_type = ContentType::kImage;
  // No CC header -> default image_max_age=3600, age=100 -> fresh
  auto result = EvaluateFreshness(input, config_);
  EXPECT_EQ(result.verdict, FreshnessVerdict::kFresh);
}

TEST_F(CacheFreshnessTest, DefaultTtlImageExpiredRevalidates) {
  auto input = MakeInput();
  input.cache_inserted_at = kTestNow - 3601;
  input.content_type = ContentType::kImage;
  // Default image_max_age=3600, age=3601 -> stale -> revalidate (#652).
  auto result = EvaluateFreshness(input, config_);
  EXPECT_EQ(result.verdict, FreshnessVerdict::kRevalidate);
  EXPECT_TRUE(result.is_stale);
}

// --- proxy-revalidate ---

TEST_F(CacheFreshnessTest, ProxyRevalidateStaleGoesToOrigin) {
  auto input = MakeInput();
  input.cache_inserted_at = kTestNow - 400;
  input.origin_max_age = 300;
  input.origin_cc_flags =
      AM::kCCOriginHeaderPresent | AM::kCCOriginProxyRevalidate;
  auto result = EvaluateFreshness(input, config_);
  EXPECT_EQ(result.verdict, FreshnessVerdict::kRevalidate);
}

// --- Boundary conditions ---

TEST_F(CacheFreshnessTest, FreshUntilLifetime) {
  for (uint32_t n : {1u, 2u, 300u}) {
    auto input = MakeInput();
    input.origin_max_age = n;
    input.origin_cc_flags = AM::kCCOriginHeaderPresent;

    input.cache_inserted_at = kTestNow - (n - 1);
    auto fresh = EvaluateFreshness(input, config_);
    EXPECT_EQ(fresh.verdict, FreshnessVerdict::kFresh) << n;
    EXPECT_FALSE(fresh.is_stale);
    EXPECT_FALSE(fresh.expired_by_age);
    EXPECT_EQ(fresh.remaining_ttl, 1u);

    input.cache_inserted_at = kTestNow - n;
    auto stale = EvaluateFreshness(input, config_);
    EXPECT_EQ(stale.verdict, FreshnessVerdict::kRevalidate) << n;
    EXPECT_TRUE(stale.is_stale);
    EXPECT_TRUE(stale.expired_by_age);
    EXPECT_EQ(stale.remaining_ttl, 0u);
  }
}

TEST_F(CacheFreshnessTest, ZeroLifetimeIsNeverFresh) {
  for (uint32_t age : {0u, 1u, 5u}) {
    auto input = MakeInput();
    input.cache_inserted_at = kTestNow - age;
    input.origin_max_age = 0;
    input.origin_cc_flags =
        AM::kCCOriginHeaderPresent | AM::kCCOriginMustRevalidate;
    auto result = EvaluateFreshness(input, config_);
    EXPECT_EQ(result.verdict, FreshnessVerdict::kRevalidate) << age;
    EXPECT_TRUE(result.is_stale);
    EXPECT_EQ(result.remaining_ttl, 0u);
  }
}

TEST_F(CacheFreshnessTest, SharedZeroLifetimeIsNeverFresh) {
  for (uint32_t age : {0u, 1u}) {
    auto input = MakeInput();
    input.cache_inserted_at = kTestNow - age;
    input.origin_max_age = 3600;
    input.origin_s_maxage = 0;
    input.origin_cc_flags =
        AM::kCCOriginHeaderPresent | AM::kCCOriginSMaxagePresent;
    auto result = EvaluateFreshness(input, config_);
    EXPECT_EQ(result.verdict, FreshnessVerdict::kRevalidate) << age;
    EXPECT_EQ(result.remaining_ttl, 0u);
  }
}

// --- Immutable + caps ---

TEST_F(CacheFreshnessTest, ImmutableUsesHigherCap) {
  auto input = MakeInput();
  input.cache_inserted_at = kTestNow - 500000;
  input.origin_max_age = 700000;
  input.origin_cc_flags = AM::kCCOriginHeaderPresent | AM::kCCOriginImmutable;
  auto result = EvaluateFreshness(input, config_);
  // effective = min(700000, immutable_cap=604800) = 604800
  // age=500000 < 604800 -> fresh
  EXPECT_EQ(result.verdict, FreshnessVerdict::kFresh);
  EXPECT_EQ(result.effective_max_age, 604800u);
}

TEST_F(CacheFreshnessTest, ImmutableStillExpiresAtCap) {
  auto input = MakeInput();
  input.cache_inserted_at = kTestNow - 700000;
  input.origin_max_age = 900000;
  input.origin_cc_flags = AM::kCCOriginHeaderPresent | AM::kCCOriginImmutable;
  auto result = EvaluateFreshness(input, config_);
  // effective = min(900000, immutable_cap=604800) = 604800
  // age=700000 > 604800 -> stale -> revalidate (#652)
  EXPECT_EQ(result.verdict, FreshnessVerdict::kRevalidate);
  EXPECT_TRUE(result.is_stale);
}

TEST_F(CacheFreshnessTest, MaxAgeCapApplied) {
  auto input = MakeInput();
  input.cache_inserted_at = kTestNow - 90000;
  input.origin_max_age = 999999;
  input.origin_cc_flags = AM::kCCOriginHeaderPresent;
  auto result = EvaluateFreshness(input, config_);
  // effective = min(999999, max_age_cap=86400) = 86400
  // age=90000 > 86400 -> stale -> revalidate (#652)
  EXPECT_EQ(result.verdict, FreshnessVerdict::kRevalidate);
  EXPECT_EQ(result.effective_max_age, 86400u);
}

// --- kCCOriginHeaderPresent interactions ---

TEST_F(CacheFreshnessTest, MaxAgeZeroWithHeaderPresent) {
  auto input = MakeInput();
  input.cache_inserted_at = kTestNow - 1;
  input.origin_max_age = 0;
  input.origin_cc_flags = AM::kCCOriginHeaderPresent;
  input.content_type = ContentType::kCss;
  auto result = EvaluateFreshness(input, config_);
  // max_age=0 with header present: don't use type default (300).
  // age=1 > 0 -> stale -> revalidate (#652)
  EXPECT_EQ(result.verdict, FreshnessVerdict::kRevalidate);
  EXPECT_EQ(result.effective_max_age, 0u);
}

TEST_F(CacheFreshnessTest, HeaderPresentWithZeroMaxAgeAndMustRevalidate) {
  auto input = MakeInput();
  input.cache_inserted_at = kTestNow - 1;
  input.origin_max_age = 0;
  input.origin_cc_flags =
      AM::kCCOriginHeaderPresent | AM::kCCOriginMustRevalidate;
  auto result = EvaluateFreshness(input, config_);
  EXPECT_EQ(result.verdict, FreshnessVerdict::kRevalidate);
}

// --- Result field correctness ---

TEST_F(CacheFreshnessTest, ResultFieldsCorrect) {
  auto input = MakeInput();
  input.cache_inserted_at = kTestNow - 150;
  input.origin_max_age = 3600;
  input.origin_cc_flags = AM::kCCOriginHeaderPresent;
  auto result = EvaluateFreshness(input, config_);
  EXPECT_EQ(result.age_seconds, 150u);
  EXPECT_EQ(result.effective_max_age, 3600u);
  EXPECT_EQ(result.remaining_ttl, 3450u);
  EXPECT_FALSE(result.is_stale);
  EXPECT_EQ(result.verdict, FreshnessVerdict::kFresh);
}

// --- Fresh entries with revalidation directives ---

TEST_F(CacheFreshnessTest, MustRevalidateFreshEntryStillServed) {
  auto input = MakeInput();
  input.cache_inserted_at = kTestNow - 100;
  input.origin_max_age = 3600;
  input.origin_cc_flags =
      AM::kCCOriginHeaderPresent | AM::kCCOriginMustRevalidate;
  auto result = EvaluateFreshness(input, config_);
  // must-revalidate only triggers when stale, not when fresh
  EXPECT_EQ(result.verdict, FreshnessVerdict::kFresh);
  EXPECT_FALSE(result.is_stale);
}

TEST_F(CacheFreshnessTest, ProxyRevalidateFreshEntryStillServed) {
  auto input = MakeInput();
  input.cache_inserted_at = kTestNow - 100;
  input.origin_max_age = 3600;
  input.origin_cc_flags =
      AM::kCCOriginHeaderPresent | AM::kCCOriginProxyRevalidate;
  auto result = EvaluateFreshness(input, config_);
  EXPECT_EQ(result.verdict, FreshnessVerdict::kFresh);
}

TEST_F(CacheFreshnessTest, SMaxageFreshEntryStillServed) {
  auto input = MakeInput();
  input.cache_inserted_at = kTestNow - 30;
  input.origin_max_age = 3600;
  input.origin_s_maxage = 60;
  input.origin_cc_flags =
      AM::kCCOriginHeaderPresent | AM::kCCOriginSMaxagePresent;
  auto result = EvaluateFreshness(input, config_);
  // s-maxage=60, age=30 -> fresh
  EXPECT_EQ(result.verdict, FreshnessVerdict::kFresh);
  EXPECT_EQ(result.effective_max_age, 60u);
}

// --- JS content type default ---

TEST_F(CacheFreshnessTest, DefaultTtlJsFresh) {
  auto input = MakeInput();
  input.cache_inserted_at = kTestNow - 100;
  input.content_type = ContentType::kJs;
  // No CC header -> default css_max_age=300, age=100 -> fresh
  auto result = EvaluateFreshness(input, config_);
  EXPECT_EQ(result.verdict, FreshnessVerdict::kFresh);
  EXPECT_EQ(result.effective_max_age, 300u);
}

// --- no-store in private cache ---

TEST_F(CacheFreshnessTest, NoStoreInPrivateCacheStillServedAsNoCache) {
  auto input = MakeInput();
  input.origin_cc_flags = AM::kCCOriginHeaderPresent | AM::kCCOriginNoStore;
  input.is_shared_cache = false;
  auto result = EvaluateFreshness(input, config_);
  EXPECT_EQ(result.verdict, FreshnessVerdict::kServeNoCache);
}

// --- Conflicting directives ---

TEST_F(CacheFreshnessTest, NoCacheWithImmutableStillRevalidates) {
  auto input = MakeInput();
  input.origin_max_age = 3600;
  input.origin_cc_flags = AM::kCCOriginHeaderPresent | AM::kCCOriginNoCache |
                          AM::kCCOriginImmutable;
  auto result = EvaluateFreshness(input, config_);
  // no-cache wins over immutable: always revalidate
  EXPECT_EQ(result.verdict, FreshnessVerdict::kRevalidate);
}

// --- Age overflow / boundary tests ---

TEST_F(CacheFreshnessTest, AgeAtUint32MaxBoundary) {
  // With now_seconds = UINT32_MAX and cache_inserted_at = 0,
  // age_raw = UINT32_MAX which equals kMaxAge exactly.
  // This exercises the boundary of the age overflow check.
  FreshnessInput input{};
  input.now_seconds = std::numeric_limits<uint32_t>::max();
  input.cache_inserted_at = 0;
  input.origin_max_age = 86400;
  input.origin_cc_flags = AM::kCCOriginHeaderPresent;
  input.content_type = ContentType::kImage;
  input.is_shared_cache = true;
  auto result = EvaluateFreshness(input, config_);
  // age_raw = UINT32_MAX > any practical max-age -> stale -> revalidate
  EXPECT_EQ(result.age_seconds, std::numeric_limits<uint32_t>::max());
  EXPECT_TRUE(result.is_stale);
  EXPECT_EQ(result.verdict, FreshnessVerdict::kRevalidate);
}

TEST_F(CacheFreshnessTest, AgeOverflowCorruptedFarFutureTimestamp) {
  // cache_inserted_at far in the future (> now + 60) triggers age = UINT32_MAX.
  FreshnessInput input{};
  input.now_seconds = 1000;
  input.cache_inserted_at =
      2000;  // 1000 seconds in the future (> 60s tolerance)
  input.origin_max_age = 86400;
  input.origin_cc_flags =
      AM::kCCOriginHeaderPresent | AM::kCCOriginMustRevalidate;
  input.content_type = ContentType::kImage;
  input.is_shared_cache = true;
  auto result = EvaluateFreshness(input, config_);
  EXPECT_EQ(result.age_seconds, std::numeric_limits<uint32_t>::max());
  EXPECT_TRUE(result.is_stale);
  EXPECT_EQ(result.verdict, FreshnessVerdict::kRevalidate);
}

// --- age_raw > kMaxAge (lines 25-27 of freshness.cc) ---
// With uint32_t inputs, age_raw = int64_t(now) - int64_t(inserted).
// Maximum age_raw = UINT32_MAX - 0 = UINT32_MAX = kMaxAge.
// The condition age_raw > kMaxAge is unreachable with uint32_t inputs
// (it would require age_raw > 4294967295, but max is exactly 4294967295).
// This is defensive code for potential future type widening.
//
// The test below documents the boundary: age_raw == kMaxAge takes the
// else branch (line 28), NOT lines 26-27.

TEST_F(CacheFreshnessTest, AgeRawExactlyAtKMaxAgeTakesElseBranch) {
  // age_raw = UINT32_MAX - 0 = UINT32_MAX = kMaxAge.
  // This hits the else branch (line 28: age = static_cast<uint32_t>(age_raw))
  // because the condition is age_raw > kMaxAge (strictly greater).
  FreshnessInput input{};
  input.now_seconds = std::numeric_limits<uint32_t>::max();
  input.cache_inserted_at = 0;
  input.origin_max_age = 86400;
  input.origin_cc_flags = AM::kCCOriginHeaderPresent;
  input.content_type = ContentType::kImage;
  input.is_shared_cache = true;
  auto result = EvaluateFreshness(input, config_);
  // age = UINT32_MAX, which is > any practical max-age -> stale
  EXPECT_EQ(result.age_seconds, std::numeric_limits<uint32_t>::max());
  EXPECT_TRUE(result.is_stale);
}

TEST_F(CacheFreshnessTest, AgeBoundaryNowMaxInsertedOne) {
  // age_raw = UINT32_MAX - 1 = 4294967294. This is less than kMaxAge,
  // so it takes the normal else branch.
  FreshnessInput input{};
  input.now_seconds = std::numeric_limits<uint32_t>::max();
  input.cache_inserted_at = 1;
  input.origin_max_age = 86400;
  input.origin_cc_flags = AM::kCCOriginHeaderPresent;
  input.content_type = ContentType::kImage;
  input.is_shared_cache = true;
  auto result = EvaluateFreshness(input, config_);
  EXPECT_EQ(result.age_seconds, std::numeric_limits<uint32_t>::max() - 1);
  EXPECT_TRUE(result.is_stale);
}

TEST_F(CacheFreshnessTest, FarFutureTimestampBeyond60SecondTolerance) {
  // cache_inserted_at > now + 60: corrupted/far-future timestamp.
  // age is forced to kMaxAge (UINT32_MAX).
  FreshnessInput input{};
  input.now_seconds = 100;
  input.cache_inserted_at = 200;  // 100 seconds ahead (> 60s tolerance)
  input.origin_max_age = 86400;
  input.origin_cc_flags = AM::kCCOriginHeaderPresent;
  input.content_type = ContentType::kImage;
  input.is_shared_cache = true;
  auto result = EvaluateFreshness(input, config_);
  EXPECT_EQ(result.age_seconds, std::numeric_limits<uint32_t>::max());
  EXPECT_TRUE(result.is_stale);
}

TEST_F(CacheFreshnessTest, NowZeroInsertedAtMaxUint32) {
  // Edge case: now = 0, inserted = UINT32_MAX.
  // age_raw = int64_t(0) - int64_t(UINT32_MAX) = -4294967295.
  // cache_inserted_at(UINT32_MAX) > now(0) + 60 -> age = kMaxAge.
  FreshnessInput input{};
  input.now_seconds = 0;
  input.cache_inserted_at = std::numeric_limits<uint32_t>::max();
  input.origin_max_age = 86400;
  input.origin_cc_flags = AM::kCCOriginHeaderPresent;
  input.content_type = ContentType::kImage;
  input.is_shared_cache = true;
  auto result = EvaluateFreshness(input, config_);
  EXPECT_EQ(result.age_seconds, std::numeric_limits<uint32_t>::max());
  EXPECT_TRUE(result.is_stale);
}

TEST_F(CacheFreshnessTest, SmallClockSkewWithin60SecToleranceClampsToZero) {
  // cache_inserted_at = now + 59 (within tolerance window).
  // age_raw = -59, which is < 0 -> clamped to 0.
  FreshnessInput input{};
  input.now_seconds = 1000;
  input.cache_inserted_at = 1059;
  input.origin_max_age = 3600;
  input.origin_cc_flags = AM::kCCOriginHeaderPresent;
  input.content_type = ContentType::kImage;
  input.is_shared_cache = true;
  auto result = EvaluateFreshness(input, config_);
  EXPECT_EQ(result.age_seconds, 0u);
  EXPECT_EQ(result.verdict, FreshnessVerdict::kFresh);
}

TEST_F(CacheFreshnessTest, ExactlyAt60SecondToleranceBoundary) {
  // cache_inserted_at = now + 60 is NOT > now + 60, so it takes the
  // negative age_raw path (age_raw = -60 < 0 -> age = 0).
  FreshnessInput input{};
  input.now_seconds = 1000;
  input.cache_inserted_at = 1060;
  input.origin_max_age = 3600;
  input.origin_cc_flags = AM::kCCOriginHeaderPresent;
  input.content_type = ContentType::kImage;
  input.is_shared_cache = true;
  auto result = EvaluateFreshness(input, config_);
  EXPECT_EQ(result.age_seconds, 0u);
  EXPECT_EQ(result.verdict, FreshnessVerdict::kFresh);
}

TEST_F(CacheFreshnessTest, OneSecondPast60SecondToleranceIsFarFuture) {
  // cache_inserted_at = now + 61 is > now + 60, triggering far-future.
  FreshnessInput input{};
  input.now_seconds = 1000;
  input.cache_inserted_at = 1061;
  input.origin_max_age = 3600;
  input.origin_cc_flags = AM::kCCOriginHeaderPresent;
  input.content_type = ContentType::kImage;
  input.is_shared_cache = true;
  auto result = EvaluateFreshness(input, config_);
  EXPECT_EQ(result.age_seconds, std::numeric_limits<uint32_t>::max());
  EXPECT_TRUE(result.is_stale);
}

// --- ClampAge: direct tests for the overflow path (lines 26-27) ---
// With uint32_t inputs to EvaluateFreshness, age_raw can never exceed
// UINT32_MAX, making the age_raw > kMaxAge branch unreachable.
// The extracted ClampAge function accepts int64_t age_raw directly,
// allowing us to test the overflow clamping.

TEST_F(CacheFreshnessTest, ClampAgeOverflowClampsToMax) {
  // age_raw exceeds UINT32_MAX -> should clamp to kMaxAge.
  int64_t age_raw =
      static_cast<int64_t>(std::numeric_limits<uint32_t>::max()) + 1;
  uint32_t result =
      ClampAge(age_raw, /*cache_inserted_at=*/0, /*now_seconds=*/1000);
  EXPECT_EQ(result, std::numeric_limits<uint32_t>::max());
}

TEST_F(CacheFreshnessTest, ClampAgeFarOverflowClampsToMax) {
  // age_raw is INT64_MAX -> should clamp to kMaxAge.
  int64_t age_raw = std::numeric_limits<int64_t>::max();
  uint32_t result =
      ClampAge(age_raw, /*cache_inserted_at=*/0, /*now_seconds=*/1000);
  EXPECT_EQ(result, std::numeric_limits<uint32_t>::max());
}

TEST_F(CacheFreshnessTest, ClampAgeExactlyAtMaxTakesNormalPath) {
  // age_raw == UINT32_MAX -> NOT greater than kMaxAge, takes normal cast.
  auto age_raw = static_cast<int64_t>(std::numeric_limits<uint32_t>::max());
  uint32_t result =
      ClampAge(age_raw, /*cache_inserted_at=*/0, /*now_seconds=*/1000);
  EXPECT_EQ(result, std::numeric_limits<uint32_t>::max());
}

TEST_F(CacheFreshnessTest, ClampAgeNegativeClampsToZero) {
  int64_t age_raw = -100;
  uint32_t result =
      ClampAge(age_raw, /*cache_inserted_at=*/1000, /*now_seconds=*/1050);
  EXPECT_EQ(result, 0u);
}

TEST_F(CacheFreshnessTest, ClampAgeFarFutureTimestampClampsToMax) {
  // cache_inserted_at > now_seconds + 60 -> far-future -> kMaxAge.
  int64_t age_raw = -500;
  uint32_t result =
      ClampAge(age_raw, /*cache_inserted_at=*/600, /*now_seconds=*/100);
  EXPECT_EQ(result, std::numeric_limits<uint32_t>::max());
}

TEST_F(CacheFreshnessTest, ClampAgeNormalCast) {
  int64_t age_raw = 42;
  uint32_t result =
      ClampAge(age_raw, /*cache_inserted_at=*/958, /*now_seconds=*/1000);
  EXPECT_EQ(result, 42u);
}

// --- Force revalidate (client force-refresh) ---

TEST_F(CacheFreshnessTest, ForceRevalidateFreshEntryGoesToOrigin) {
  auto input = MakeInput();
  input.origin_max_age = 3600;
  input.origin_cc_flags = AM::kCCOriginHeaderPresent;
  input.force_revalidate = true;
  auto result = EvaluateFreshness(input, config_);
  EXPECT_EQ(result.verdict, FreshnessVerdict::kRevalidate);
  EXPECT_TRUE(result.is_stale);
  EXPECT_EQ(result.remaining_ttl, 0u);
  EXPECT_EQ(result.age_seconds, 0u);
  EXPECT_EQ(result.effective_max_age, 3600u);
  // Client-driven revalidation, not age expiry: must NOT be eligible for
  // the origin-refreshed purge (issue #652 review, blocker — a plain
  // browser reload sends Cache-Control: max-age=0 and would otherwise let
  // any anonymous client purge the URL's whole optimized variant set).
  EXPECT_FALSE(result.expired_by_age);
}

TEST_F(CacheFreshnessTest, ForceRevalidateStaleEntryGoesToOrigin) {
  auto input = MakeInput();
  input.cache_inserted_at = kTestNow - 7200;
  input.origin_max_age = 3600;
  input.origin_cc_flags = AM::kCCOriginHeaderPresent;
  input.force_revalidate = true;
  auto result = EvaluateFreshness(input, config_);
  EXPECT_EQ(result.verdict, FreshnessVerdict::kRevalidate);
  EXPECT_TRUE(result.is_stale);
  EXPECT_EQ(result.remaining_ttl, 0u);
  EXPECT_EQ(result.age_seconds, 7200u);
  EXPECT_EQ(result.effective_max_age, 3600u);
  // Even when ALSO age-expired, force_revalidate wins: the request header
  // is client-controlled, so expired_by_age stays false (issue #652
  // review).  The age-expiry purge fires on the next organic request.
  EXPECT_FALSE(result.expired_by_age);
}

TEST_F(CacheFreshnessTest, ForceRevalidateImmutableStillRevalidates) {
  auto input = MakeInput();
  input.origin_max_age = 3600;
  input.origin_cc_flags = AM::kCCOriginHeaderPresent | AM::kCCOriginImmutable;
  input.force_revalidate = true;
  auto result = EvaluateFreshness(input, config_);
  EXPECT_EQ(result.verdict, FreshnessVerdict::kRevalidate);
}

TEST_F(CacheFreshnessTest, ForceRevalidateNoStoreStillServedAsNoCache) {
  // no-store short-circuits before force_revalidate check
  auto input = MakeInput();
  input.origin_cc_flags = AM::kCCOriginHeaderPresent | AM::kCCOriginNoStore;
  input.force_revalidate = true;
  auto result = EvaluateFreshness(input, config_);
  EXPECT_EQ(result.verdict, FreshnessVerdict::kServeNoCache);
}

TEST_F(CacheFreshnessTest, ForceRevalidateDefaultIsFalse) {
  FreshnessInput input{};
  EXPECT_FALSE(input.force_revalidate);
}

TEST_F(CacheFreshnessTest, ForceRevalidateHtmlContent) {
  auto input = MakeInput();
  input.content_type = ContentType::kHtml;
  input.origin_max_age = 3600;
  input.origin_cc_flags = AM::kCCOriginHeaderPresent;
  input.force_revalidate = true;
  auto result = EvaluateFreshness(input, config_);
  EXPECT_EQ(result.verdict, FreshnessVerdict::kRevalidate);
}

TEST_F(CacheFreshnessTest, ForceRevalidatePrivateInPrivateCacheRevalidates) {
  auto input = MakeInput();
  input.origin_max_age = 3600;
  input.origin_cc_flags = AM::kCCOriginHeaderPresent | AM::kCCOriginPrivate;
  input.is_shared_cache = false;
  input.force_revalidate = true;
  auto result = EvaluateFreshness(input, config_);
  // Private flag passes through in private cache, force_revalidate triggers
  EXPECT_EQ(result.verdict, FreshnessVerdict::kRevalidate);
}

TEST_F(CacheFreshnessTest, NoForceRevalidateHtmlFreshStaysServed) {
  auto input = MakeInput();
  input.content_type = ContentType::kHtml;
  input.origin_max_age = 3600;
  input.origin_cc_flags = AM::kCCOriginHeaderPresent;
  input.force_revalidate = false;
  auto result = EvaluateFreshness(input, config_);
  EXPECT_EQ(result.verdict, FreshnessVerdict::kFresh);
}

// ---------------------------------------------------------------------------
// FreshnessInputFromMetadata vs the front stage's own build-ups
//
// The helper exists for callers that are not the nginx front stage, and the
// front stage — frozen — still builds this input inline in two places. Three
// copies of one mapping is exactly the shape that drifts silently: a field
// added to the metadata and wired into one copy, and a cache somewhere answers
// a freshness question with a field the others fed and this one did not.
//
// So both front-stage shapes are transcribed here as ORACLES, verbatim in
// effect, and compared to the helper field by field over a corpus that varies
// every input the mapping reads. This is the cheap half of the oracle pattern
// the Vary-storability extraction used: the transcription is the assertion,
// and editing one side without the other goes red here.
// ---------------------------------------------------------------------------

// Oracle 1 — ngx_http_pagespeed_stale_if_error_allowed: no force-refresh
// handling, shared cache hard-coded.
FreshnessInput OracleStaleIfErrorSite(const AlternateMetadata& meta,
                                      uint32_t now) {
  FreshnessInput fi;
  fi.now_seconds = now;
  fi.cache_inserted_at = meta.cache_inserted_at;
  fi.origin_max_age = meta.origin_max_age;
  fi.origin_s_maxage = meta.origin_s_maxage;
  fi.origin_cc_flags = meta.origin_cc_flags;
  fi.content_type = meta.content_type;
  fi.is_shared_cache = true;
  return fi;
}

// Oracle 2 — the HIT-path build-up. Note it feeds a LOCAL COPY of the flags
// (`uint16_t cc_flags = meta.origin_cc_flags;` at the top of the block, used
// for several later decisions) rather than the metadata field directly. Today
// that copy is unmodified between declaration and use, so the two agree; the
// copy is transcribed AS a copy because it is the seam where they could stop
// agreeing, and this test is what would notice.
FreshnessInput OracleHitPathSite(const AlternateMetadata& meta, uint32_t now,
                                 bool force_revalidate) {
  uint16_t cc_flags = meta.origin_cc_flags;
  FreshnessInput fi;
  fi.now_seconds = now;
  fi.cache_inserted_at = meta.cache_inserted_at;
  fi.origin_max_age = meta.origin_max_age;
  fi.origin_s_maxage = meta.origin_s_maxage;
  fi.origin_cc_flags = cc_flags;
  fi.content_type = meta.content_type;
  fi.is_shared_cache = true;  // nginx is always a shared cache
  fi.force_revalidate = force_revalidate;
  return fi;
}

void ExpectSameInput(const FreshnessInput& a, const FreshnessInput& b,
                     const char* which) {
  SCOPED_TRACE(which);
  EXPECT_EQ(a.now_seconds, b.now_seconds);
  EXPECT_EQ(a.cache_inserted_at, b.cache_inserted_at);
  EXPECT_EQ(a.origin_max_age, b.origin_max_age);
  EXPECT_EQ(a.origin_s_maxage, b.origin_s_maxage);
  EXPECT_EQ(a.origin_cc_flags, b.origin_cc_flags);
  EXPECT_EQ(a.content_type, b.content_type);
  EXPECT_EQ(a.is_shared_cache, b.is_shared_cache);
  EXPECT_EQ(a.force_revalidate, b.force_revalidate);
}

TEST(FreshnessInputFromMetadataTest, MatchesBothFrontStageBuildUps) {
  // Vary every field the mapping reads, including the ones whose defaults
  // would hide a missing assignment (0 / kOther / all-flags).
  const uint32_t inserted[] = {0, 1, kTestNow, 0xFFFFFFFFu};
  const uint32_t max_age[] = {0, 1, 600, 0xFFFFFFFFu};
  const uint32_t s_maxage[] = {0, 300};
  const uint16_t cc_flags[] = {
      0, AM::kCCOriginHeaderPresent,
      static_cast<uint16_t>(AM::kCCOriginNoStore | AM::kCCOriginPrivateBare),
      0xFFFFu};
  const ContentType cts[] = {ContentType::kHtml, ContentType::kCss,
                             ContentType::kJs, ContentType::kImage,
                             ContentType::kOther};

  int cases = 0;
  for (uint32_t ins : inserted) {
    for (uint32_t ma : max_age) {
      for (uint32_t sm : s_maxage) {
        for (uint16_t cc : cc_flags) {
          for (ContentType ct : cts) {
            AlternateMetadata meta;
            meta.cache_inserted_at = ins;
            meta.origin_max_age = ma;
            meta.origin_s_maxage = sm;
            meta.origin_cc_flags = cc;
            meta.content_type = ct;
            // Fields the mapping must NOT read, set to values that would show
            // up if it did.
            meta.origin_etag = "\"x\"";
            meta.origin_last_modified = 12345;
            meta.origin_cache_control = "public, max-age=1";

            ExpectSameInput(FreshnessInputFromMetadata(meta, kTestNow),
                            OracleStaleIfErrorSite(meta, kTestNow),
                            "stale-if-error site");
            for (bool force : {false, true}) {
              ExpectSameInput(
                  FreshnessInputFromMetadata(meta, kTestNow,
                                             /*is_shared_cache=*/true, force),
                  OracleHitPathSite(meta, kTestNow, force), "HIT-path site");
            }
            ++cases;
          }
        }
      }
    }
  }
  EXPECT_EQ(cases, 4 * 4 * 2 * 4 * 5);
}

TEST(FreshnessInputFromMetadataTest, VerdictsAgreeWithTheOracles) {
  // The mapping is only interesting because of what it decides, so compare the
  // decision too, not only the struct.
  FreshnessConfig fc;
  AlternateMetadata meta;
  meta.cache_inserted_at = kTestNow - 100;
  meta.origin_max_age = 600;
  meta.origin_cc_flags = AM::kCCOriginHeaderPresent | AM::kCCOriginPublic;
  meta.content_type = ContentType::kImage;

  for (uint32_t age : {0u, 100u, 599u, 600u, 601u, 100000u}) {
    const uint32_t now = meta.cache_inserted_at + age;
    SCOPED_TRACE(age);
    EXPECT_EQ(
        EvaluateFreshness(FreshnessInputFromMetadata(meta, now), fc).verdict,
        EvaluateFreshness(OracleStaleIfErrorSite(meta, now), fc).verdict);
    EXPECT_EQ(
        EvaluateFreshness(FreshnessInputFromMetadata(meta, now, true,
                                                     /*force_revalidate=*/true),
                          fc)
            .verdict,
        EvaluateFreshness(OracleHitPathSite(meta, now, true), fc).verdict);
  }
}

}  // namespace
}  // namespace pagespeed
