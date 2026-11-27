// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

// Tests for the shared-cache gate on cookie-bearing requests
// (src/nginx/cookie_cache_gate.h).  The module wiring (record path, HIT path,
// the Markdown copy, Early Hints and /llms.txt) is covered end to end by
// tools/http-compliance/test_cookie_request.py and t/111-cookie-request.t.

#include "src/nginx/cookie_cache_gate.h"

#include "gtest/gtest.h"
#include "lib/cache/html_cache_policy.h"
#include "lib/classify/alternate_metadata.h"
#include "lib/classify/content_type.h"

namespace pagespeed {
namespace {

using Meta = AlternateMetadata;

constexpr uint16_t kCc = Meta::kCCOriginHeaderPresent;
constexpr uint16_t kSMax = kCc | Meta::kCCOriginSMaxagePresent;

TEST(CookieCacheGateTest, RequestWithoutCookieIsUnaffected) {
  EXPECT_TRUE(CookieCacheGateAllowsStore(false, "text/html", 0, 0, 0));
  EXPECT_TRUE(CookieCacheGateAllowsStore(false, "text/css", 0, 0, 0));
  EXPECT_TRUE(
      CookieCacheGateAllowsServe(false, CookieShareClass::kPage, kCc, 0));
}

TEST(CookieCacheGateTest, CookieRequestWithoutPublicIsNotShared) {
  for (const char* ct :
       {"text/html", "Text/HTML", "text/html; charset=utf-8",
        "application/xhtml+xml", "application/json", "text/plain", ""}) {
    EXPECT_FALSE(CookieCacheGateAllowsStore(true, ct, kCc, 3600, 0)) << ct;
    EXPECT_TRUE(CookieCacheGateAllowsStore(
        true, ct, kCc | Meta::kCCOriginPublic, 3600, 0))
        << ct;
    EXPECT_TRUE(CookieCacheGateAllowsStore(true, ct, kSMax, 3600, 600)) << ct;
  }
  EXPECT_FALSE(
      CookieCacheGateAllowsServe(true, CookieShareClass::kPage, kCc, 0));
}

TEST(CookieCacheGateTest, SMaxageMustBeAboveZero) {
  // s-maxage=0 and a malformed value (parsed to 0) are not a mark.
  EXPECT_FALSE(CookieCacheGateAllowsStore(true, "text/html", kSMax, 3600, 0));
  EXPECT_FALSE(
      CookieCacheGateAllowsServe(true, CookieShareClass::kPage, kSMax, 0));
  EXPECT_TRUE(
      CookieCacheGateAllowsServe(true, CookieShareClass::kPage, kSMax, 60));
}

TEST(CookieCacheGateTest, RepeatedSMaxageIsNotAMark) {
  constexpr uint16_t kRepeated = kSMax | Meta::kCCOriginSMaxageRepeated;
  EXPECT_FALSE(
      CookieCacheGateAllowsStore(true, "text/html", kRepeated, 3600, 60));
  EXPECT_FALSE(
      CookieCacheGateAllowsServe(true, CookieShareClass::kPage, kRepeated, 60));
}

TEST(CookieCacheGateTest, MustRevalidateDoesNotMarkAPageShareable) {
  EXPECT_FALSE(CookieCacheGateAllowsStore(
      true, "text/html", kCc | Meta::kCCOriginMustRevalidate, 3600, 0));
}

TEST(CookieCacheGateTest, StaticAssetNeedsAStatedLifetime) {
  for (const char* ct : {"text/css", "application/javascript", "image/png",
                         "TEXT/JAVASCRIPT; charset=utf-8"}) {
    // No Cache-Control at all: the class default would apply; refused.
    EXPECT_FALSE(CookieCacheGateAllowsStore(true, ct, 0, 0, 0)) << ct;
    // Cache-Control without a lifetime.
    EXPECT_FALSE(CookieCacheGateAllowsStore(true, ct, kCc, 0, 0)) << ct;
    EXPECT_TRUE(CookieCacheGateAllowsStore(true, ct, kCc, 3600, 0)) << ct;
    EXPECT_TRUE(CookieCacheGateAllowsStore(true, ct, kSMax, 0, 60)) << ct;
  }
  // A stored asset is served to a cookie-bearing request as before.
  EXPECT_TRUE(
      CookieCacheGateAllowsServe(true, CookieShareClass::kStaticAsset, 0, 0));
}

TEST(CookieCacheGateTest, StoredEntryClass) {
  EXPECT_EQ(CookieShareClass::kStaticAsset,
            StoredEntryCookieClass("text/css", ContentType::kOther));
  EXPECT_EQ(CookieShareClass::kPage,
            StoredEntryCookieClass("application/json", ContentType::kOther));
  EXPECT_EQ(CookieShareClass::kPage,
            StoredEntryCookieClass("Text/HTML", ContentType::kOther));
  // Without a stored Content-Type the stored class decides.
  EXPECT_EQ(CookieShareClass::kStaticAsset,
            StoredEntryCookieClass("", ContentType::kImage));
  EXPECT_EQ(CookieShareClass::kPage,
            StoredEntryCookieClass("", ContentType::kHtml));
  EXPECT_EQ(CookieShareClass::kPage,
            StoredEntryCookieClass("", ContentType::kOther));
}

}  // namespace
}  // namespace pagespeed
