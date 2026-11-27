// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

// Tests for the OptimizationPolicy engine.

#include "src/worker/optimization_policy.h"

#include <string>

#include "gtest/gtest.h"
#include "src/browser/optimization_profile.h"

namespace pagespeed {
namespace {

TEST(OptimizationPolicyTest, ComputeFromEmptyProfile) {
  OptimizationProfile profile;
  auto policy = OptimizationPolicy::Compute(profile);

  EXPECT_FALSE(policy.async_css_recommended);
  EXPECT_FLOAT_EQ(policy.css_unused_ratio, 0.0f);
  EXPECT_FALSE(policy.script_deferral_recommended);
  EXPECT_EQ(policy.scripts_deferrable, 0);
  EXPECT_FALSE(policy.lcp_is_image);
  EXPECT_EQ(policy.above_fold_image_count, 0);
  EXPECT_EQ(policy.optimization_score, 0);
  // Empty profile has no data, but expires_at is 0 so +0 for that.
  EXPECT_EQ(policy.confidence, 0);
}

TEST(OptimizationPolicyTest, ComputeHighCssWaste) {
  OptimizationProfile profile;
  // 80% unused CSS across all viewports.
  profile.mobile.total_css_bytes = 10000;
  profile.mobile.unused_css_bytes = 8000;
  profile.mobile.css_coverage_ratio = 0.20f;
  profile.tablet.total_css_bytes = 10000;
  profile.tablet.unused_css_bytes = 8000;
  profile.tablet.css_coverage_ratio = 0.20f;
  profile.desktop.total_css_bytes = 10000;
  profile.desktop.unused_css_bytes = 8000;
  profile.desktop.css_coverage_ratio = 0.20f;

  auto policy = OptimizationPolicy::Compute(profile);

  EXPECT_TRUE(policy.async_css_recommended);
  EXPECT_NEAR(policy.css_unused_ratio, 0.80f, 0.01f);
}

TEST(OptimizationPolicyTest, ComputeLowCssWaste) {
  OptimizationProfile profile;
  // 20% unused CSS — not enough to recommend async.
  profile.mobile.total_css_bytes = 10000;
  profile.mobile.unused_css_bytes = 2000;
  profile.mobile.css_coverage_ratio = 0.80f;
  profile.tablet.total_css_bytes = 10000;
  profile.tablet.unused_css_bytes = 2000;
  profile.tablet.css_coverage_ratio = 0.80f;
  profile.desktop.total_css_bytes = 10000;
  profile.desktop.unused_css_bytes = 2000;
  profile.desktop.css_coverage_ratio = 0.80f;

  auto policy = OptimizationPolicy::Compute(profile);

  EXPECT_FALSE(policy.async_css_recommended);
  EXPECT_NEAR(policy.css_unused_ratio, 0.20f, 0.01f);
}

TEST(OptimizationPolicyTest, ComputeWithDeferrableScripts) {
  OptimizationProfile profile;
  profile.defer_safe_scripts = {"analytics.js", "tracker.js", "chat.js"};

  auto policy = OptimizationPolicy::Compute(profile);

  EXPECT_TRUE(policy.script_deferral_recommended);
  EXPECT_EQ(policy.scripts_deferrable, 3);
}

TEST(OptimizationPolicyTest, ComputeLcpIsImage) {
  OptimizationProfile profile;
  profile.desktop.lcp_url = "https://example.com/hero.jpg";

  auto policy = OptimizationPolicy::Compute(profile);

  EXPECT_TRUE(policy.lcp_is_image);
}

TEST(OptimizationPolicyTest, ComputeConfidenceScoring) {
  OptimizationProfile profile;
  // All 3 viewports with CSS data: +30 (browser CSS) + 30 (3 viewports).
  profile.mobile.total_css_bytes = 5000;
  profile.mobile.css_coverage_ratio = 0.60f;
  profile.tablet.total_css_bytes = 5000;
  profile.tablet.css_coverage_ratio = 0.60f;
  profile.desktop.total_css_bytes = 5000;
  profile.desktop.css_coverage_ratio = 0.60f;
  // Script analysis: +20.
  profile.defer_safe_scripts = {"app.js"};
  // Expires set: +20.
  profile.expires_at = 1000;

  auto policy = OptimizationPolicy::Compute(profile);

  EXPECT_EQ(policy.confidence, 100);
}

TEST(OptimizationPolicyTest, ComputeOverallScore) {
  OptimizationProfile profile;
  // High CSS waste: 80% unused → 0.8 * 30 = 24 points.
  profile.mobile.total_css_bytes = 10000;
  profile.mobile.css_coverage_ratio = 0.20f;
  profile.tablet.total_css_bytes = 10000;
  profile.tablet.css_coverage_ratio = 0.20f;
  profile.desktop.total_css_bytes = 10000;
  profile.desktop.css_coverage_ratio = 0.20f;
  // 3 deferrable scripts: min(3,5)*4 = 12 points.
  profile.defer_safe_scripts = {"a.js", "b.js", "c.js"};
  // LCP is image: 15 points.
  profile.mobile.lcp_url = "https://example.com/hero.jpg";
  // Preload hints present: 15 points.
  profile.preload_hints = {{"https://example.com/style.css", "style", ""}};
  // 2 above-fold images: min(2,5)*4 = 8 points.
  profile.desktop.image_dimensions = {{"/img1.jpg", 100, 200, 100, 200},
                                      {"/img2.jpg", 300, 400, 300, 400}};

  auto policy = OptimizationPolicy::Compute(profile);

  // Total: 24 + 12 + 8 + 15 + 15 = 74.
  EXPECT_EQ(policy.optimization_score, 74);
}

TEST(OptimizationPolicyTest, JsonRoundTrip) {
  OptimizationPolicy original;
  original.async_css_recommended = true;
  original.css_unused_ratio = 0.65f;
  original.script_deferral_recommended = true;
  original.scripts_deferrable = 3;
  original.lcp_is_image = true;
  original.above_fold_image_count = 4;
  original.optimization_score = 72;
  original.confidence = 85;

  std::string json = original.ToJson();
  auto result = OptimizationPolicy::FromJson(json);
  ASSERT_TRUE(result.ok());
  const auto& roundtripped = *result;

  EXPECT_EQ(roundtripped.async_css_recommended, original.async_css_recommended);
  EXPECT_FLOAT_EQ(roundtripped.css_unused_ratio, original.css_unused_ratio);
  EXPECT_EQ(roundtripped.script_deferral_recommended,
            original.script_deferral_recommended);
  EXPECT_EQ(roundtripped.scripts_deferrable, original.scripts_deferrable);
  EXPECT_EQ(roundtripped.lcp_is_image, original.lcp_is_image);
  EXPECT_EQ(roundtripped.above_fold_image_count,
            original.above_fold_image_count);
  EXPECT_EQ(roundtripped.optimization_score, original.optimization_score);
  EXPECT_EQ(roundtripped.confidence, original.confidence);
}

TEST(OptimizationPolicyTest, FromJsonInvalidJson) {
  auto result = OptimizationPolicy::FromJson("not json at all");
  EXPECT_FALSE(result.ok());
}

TEST(OptimizationPolicyTest, FromJsonWrongTypes) {
  // String where int expected — should return error, not crash.
  auto result =
      OptimizationPolicy::FromJson(R"({"optimization_score":"not_a_number"})");
  EXPECT_FALSE(result.ok());
}

TEST(OptimizationPolicyTest, ComputeCssBoundaryAt50Percent) {
  OptimizationProfile profile;
  // Exactly 50% coverage (50% unused) — should NOT recommend async CSS
  // because the threshold is < 50%, not <= 50%.
  profile.mobile.total_css_bytes = 10000;
  profile.mobile.css_coverage_ratio = 0.50f;
  profile.tablet.total_css_bytes = 10000;
  profile.tablet.css_coverage_ratio = 0.50f;
  profile.desktop.total_css_bytes = 10000;
  profile.desktop.css_coverage_ratio = 0.50f;

  auto policy = OptimizationPolicy::Compute(profile);

  EXPECT_FALSE(policy.async_css_recommended);
  EXPECT_NEAR(policy.css_unused_ratio, 0.50f, 0.01f);
}

TEST(OptimizationPolicyTest, ComputeScoreClampedAt100) {
  OptimizationProfile profile;
  // Max out every scoring dimension.
  profile.mobile.total_css_bytes = 10000;
  profile.mobile.css_coverage_ratio = 0.0f;  // 100% unused → 30 points
  profile.tablet.total_css_bytes = 10000;
  profile.tablet.css_coverage_ratio = 0.0f;
  profile.desktop.total_css_bytes = 10000;
  profile.desktop.css_coverage_ratio = 0.0f;
  // 10 deferrable scripts: capped at 5 → 20 points.
  profile.defer_safe_scripts = {"a.js", "b.js", "c.js", "d.js", "e.js",
                                "f.js", "g.js", "h.js", "i.js", "j.js"};
  // LCP is image → 15 points.
  profile.mobile.lcp_url = "https://example.com/hero.jpg";
  // Preload hints → 15 points.
  profile.preload_hints = {{"https://example.com/style.css", "style", ""}};
  // 10 above-fold images: capped at 5 → 20 points.
  profile.desktop.image_dimensions = {
      {"/1.jpg", 1, 1, 1, 1}, {"/2.jpg", 1, 1, 1, 1}, {"/3.jpg", 1, 1, 1, 1},
      {"/4.jpg", 1, 1, 1, 1}, {"/5.jpg", 1, 1, 1, 1}, {"/6.jpg", 1, 1, 1, 1},
      {"/7.jpg", 1, 1, 1, 1}, {"/8.jpg", 1, 1, 1, 1}, {"/9.jpg", 1, 1, 1, 1},
      {"/10.jpg", 1, 1, 1, 1}};

  auto policy = OptimizationPolicy::Compute(profile);

  // Total: 30 + 20 + 20 + 15 + 15 = 100, clamped at 100.
  EXPECT_EQ(policy.optimization_score, 100);
}

}  // namespace
}  // namespace pagespeed
