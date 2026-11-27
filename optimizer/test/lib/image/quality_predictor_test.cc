// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

// PageSpeed 2.0 - Quality Predictor tests.

#include "lib/image/quality_predictor.h"

#include <cstdint>

#include "gtest/gtest.h"
#include "lib/image/quality_features.h"

namespace pagespeed {
namespace {

// Helper: create features with known values.
ImageFeatures MakeFeatures(uint32_t w, uint32_t h, uint8_t content_class,
                           uint8_t source_format) {
  ImageFeatures f;
  f.edge_density = 0.2f;
  f.noise_level = 0.1f;
  f.unique_color_ratio = 0.5f;
  f.photo_metric = 18.0f;
  f.color_entropy = 2.0f;
  f.spatial_freq_low = 500.0f;
  f.spatial_freq_high = 300.0f;
  f.mean_luminance = 128.0f;
  f.luminance_variance = 2000.0f;
  f.width = w;
  f.height = h;
  f.content_class = content_class;
  f.source_format = source_format;
  f.has_alpha = false;
  f.is_grayscale = false;
  return f;
}

// ---------------------------------------------------------------------------
// All trained models return valid predictions.
// ---------------------------------------------------------------------------

TEST(QualityPredictorTest, JpegReturnsValidPrediction) {
  auto f = MakeFeatures(640, 480, 0, 0);  // Photo, JPEG source.
  int q = PredictQuality(PredictorFormat::kJpeg, f, 70.0f);
  EXPECT_TRUE(IsValidPrediction(PredictorFormat::kJpeg, q))
      << "JPEG prediction " << q << " should be in valid range [1, 100]";
}

TEST(QualityPredictorTest, WebpReturnsValidPrediction) {
  auto f = MakeFeatures(640, 480, 0, 0);
  int q = PredictQuality(PredictorFormat::kWebP, f, 70.0f);
  EXPECT_TRUE(IsValidPrediction(PredictorFormat::kWebP, q))
      << "WebP prediction " << q << " should be in valid range [0, 100]";
}

TEST(QualityPredictorTest, AvifReturnsValidPrediction) {
  auto f = MakeFeatures(640, 480, 0, 0);
  int q = PredictQuality(PredictorFormat::kAvif, f, 70.0f);
  EXPECT_TRUE(IsValidPrediction(PredictorFormat::kAvif, q))
      << "AVIF prediction " << q << " should be in valid range [0, 100]";
}

// ---------------------------------------------------------------------------
// IsValidPrediction range checks.
// ---------------------------------------------------------------------------

TEST(QualityPredictorTest, JpegValidRange) {
  EXPECT_FALSE(IsValidPrediction(PredictorFormat::kJpeg, -1));
  EXPECT_FALSE(IsValidPrediction(PredictorFormat::kJpeg, 0));
  EXPECT_TRUE(IsValidPrediction(PredictorFormat::kJpeg, 1));
  EXPECT_TRUE(IsValidPrediction(PredictorFormat::kJpeg, 50));
  EXPECT_TRUE(IsValidPrediction(PredictorFormat::kJpeg, 100));
  EXPECT_FALSE(IsValidPrediction(PredictorFormat::kJpeg, 101));
}

TEST(QualityPredictorTest, WebpValidRange) {
  EXPECT_FALSE(IsValidPrediction(PredictorFormat::kWebP, -1));
  EXPECT_TRUE(IsValidPrediction(PredictorFormat::kWebP, 0));
  EXPECT_TRUE(IsValidPrediction(PredictorFormat::kWebP, 50));
  EXPECT_TRUE(IsValidPrediction(PredictorFormat::kWebP, 100));
  EXPECT_FALSE(IsValidPrediction(PredictorFormat::kWebP, 101));
}

TEST(QualityPredictorTest, AvifValidRange) {
  EXPECT_FALSE(IsValidPrediction(PredictorFormat::kAvif, -1));
  EXPECT_TRUE(IsValidPrediction(PredictorFormat::kAvif, 0));
  EXPECT_TRUE(IsValidPrediction(PredictorFormat::kAvif, 50));
  EXPECT_TRUE(IsValidPrediction(PredictorFormat::kAvif, 100));
  EXPECT_FALSE(IsValidPrediction(PredictorFormat::kAvif, 101));
}

// ---------------------------------------------------------------------------
// Determinism: same input → same output.
// ---------------------------------------------------------------------------

TEST(QualityPredictorTest, Deterministic) {
  auto f = MakeFeatures(800, 600, 0, 0);

  int q1 = PredictQuality(PredictorFormat::kJpeg, f, 70.0f);
  int q2 = PredictQuality(PredictorFormat::kJpeg, f, 70.0f);
  EXPECT_EQ(q1, q2);
}

// ---------------------------------------------------------------------------
// Higher target SSIMULACRA2 should produce higher quality settings.
// ---------------------------------------------------------------------------

TEST(QualityPredictorTest, HigherTargetGivesHigherQuality) {
  auto f = MakeFeatures(640, 480, 0, 0);

  int q_low = PredictQuality(PredictorFormat::kJpeg, f, 50.0f);
  int q_high = PredictQuality(PredictorFormat::kJpeg, f, 90.0f);

  EXPECT_TRUE(IsValidPrediction(PredictorFormat::kJpeg, q_low));
  EXPECT_TRUE(IsValidPrediction(PredictorFormat::kJpeg, q_high));
  EXPECT_LT(q_low, q_high) << "JPEG quality for target 50 (" << q_low
                           << ") should be less than for target 90 (" << q_high
                           << ")";
}

TEST(QualityPredictorTest, AvifHigherTargetGivesHigherQuality) {
  auto f = MakeFeatures(640, 480, 0, 0);

  int q_low = PredictQuality(PredictorFormat::kAvif, f, 50.0f);
  int q_high = PredictQuality(PredictorFormat::kAvif, f, 90.0f);

  EXPECT_TRUE(IsValidPrediction(PredictorFormat::kAvif, q_low));
  EXPECT_TRUE(IsValidPrediction(PredictorFormat::kAvif, q_high));
  EXPECT_LT(q_low, q_high) << "AVIF quality for target 50 (" << q_low
                           << ") should be less than for target 90 (" << q_high
                           << ")";
}

// ---------------------------------------------------------------------------
// Invalid PredictorFormat: exercises default branch in PredictQuality (line 30)
// and IsValidPrediction (line 49).
// ---------------------------------------------------------------------------

TEST(QualityPredictorTest, InvalidFormatReturnsMinusOne) {
  auto f = MakeFeatures(640, 480, 0, 0);
  // Cast an out-of-range value to trigger the default branch.
  auto bad_format = static_cast<PredictorFormat>(99);
  int q = PredictQuality(bad_format, f, 70.0f);
  EXPECT_EQ(q, -1);
}

TEST(QualityPredictorTest, IsValidPredictionInvalidFormatReturnsFalse) {
  auto bad_format = static_cast<PredictorFormat>(99);
  // Even a "good" quality value should return false for an invalid format.
  EXPECT_FALSE(IsValidPrediction(bad_format, 50));
  EXPECT_FALSE(IsValidPrediction(bad_format, 0));
  EXPECT_FALSE(IsValidPrediction(bad_format, 100));
}

// =================================================================
// Quality Baselining: kNumFeatures consistency
// =================================================================

TEST(QualityPredictorBaselineTest, FeatureArraySizeMatchesKNumFeatures) {
  // Verify the predictor uses kNumFeatures-sized arrays.
  ImageFeatures f = MakeFeatures(512, 512, 0, 0);
  float arr[kNumFeatures];
  f.ToFloatArray(arr, 70.0f);
  // If this compiles and runs, kNumFeatures is consistent.
  EXPECT_EQ(kNumFeatures, 18);
}

}  // namespace
}  // namespace pagespeed
