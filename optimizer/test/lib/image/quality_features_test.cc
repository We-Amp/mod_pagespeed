// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

// PageSpeed 2.0 - ImageFeatures extraction tests.

#include "lib/image/quality_features.h"

#include <cmath>
#include <cstdint>
#include <vector>

#include "gtest/gtest.h"

namespace pagespeed {
namespace {

// Helper: create a ContentAnalysisResult with known values.
ContentAnalysisResult MakeAnalysis(ContentClass cc, float edge, float noise,
                                   float unique_ratio, float photo) {
  ContentAnalysisResult a;
  a.content_class = cc;
  a.edge_density = edge;
  a.noise_level = noise;
  a.unique_color_ratio = unique_ratio;
  a.photo_metric = photo;
  return a;
}

// Helper: fill a pixel buffer with a solid RGB color.
std::vector<uint8_t> SolidRgb(uint32_t w, uint32_t h, uint8_t r, uint8_t g,
                              uint8_t b) {
  std::vector<uint8_t> buf(static_cast<size_t>(w) * h * 3);
  for (size_t i = 0; i < buf.size(); i += 3) {
    buf[i] = r;
    buf[i + 1] = g;
    buf[i + 2] = b;
  }
  return buf;
}

// Helper: fill a pixel buffer with a grayscale gradient (left to right).
std::vector<uint8_t> GrayscaleGradient(uint32_t w, uint32_t h) {
  std::vector<uint8_t> buf(static_cast<size_t>(w) * h);
  for (uint32_t y = 0; y < h; ++y) {
    for (uint32_t x = 0; x < w; ++x) {
      buf[y * w + x] = static_cast<uint8_t>((x * 255) / (w - 1));
    }
  }
  return buf;
}

// Helper: checkerboard pattern (two alternating RGB colors per pixel).
std::vector<uint8_t> Checkerboard(uint32_t w, uint32_t h, uint8_t r1,
                                  uint8_t g1, uint8_t b1, uint8_t r2,
                                  uint8_t g2, uint8_t b2) {
  std::vector<uint8_t> buf(static_cast<size_t>(w) * h * 3);
  for (uint32_t y = 0; y < h; ++y) {
    for (uint32_t x = 0; x < w; ++x) {
      size_t off = (static_cast<size_t>(y) * w + x) * 3;
      if ((x + y) % 2 == 0) {
        buf[off] = r1;
        buf[off + 1] = g1;
        buf[off + 2] = b1;
      } else {
        buf[off] = r2;
        buf[off + 1] = g2;
        buf[off + 2] = b2;
      }
    }
  }
  return buf;
}

// Helper: RGBA solid color.
std::vector<uint8_t> SolidRgba(uint32_t w, uint32_t h, uint8_t r, uint8_t g,
                               uint8_t b, uint8_t a) {
  std::vector<uint8_t> buf(static_cast<size_t>(w) * h * 4);
  for (size_t i = 0; i < buf.size(); i += 4) {
    buf[i] = r;
    buf[i + 1] = g;
    buf[i + 2] = b;
    buf[i + 3] = a;
  }
  return buf;
}

// ---------------------------------------------------------------------------
// Basic extraction: metadata fields are correctly set.
// ---------------------------------------------------------------------------

TEST(QualityFeaturesTest, MetadataCopiedFromAnalysis) {
  auto analysis = MakeAnalysis(ContentClass::kPhoto, 0.25f, 0.1f, 0.8f, 22.0f);
  auto pixels = SolidRgb(64, 64, 128, 128, 128);
  auto f = ExtractImageFeatures(analysis, pixels.data(), 64, 64, 3, 0);

  EXPECT_EQ(f.width, 64u);
  EXPECT_EQ(f.height, 64u);
  EXPECT_EQ(f.content_class, static_cast<uint8_t>(ContentClass::kPhoto));
  EXPECT_EQ(f.source_format, 0);  // JPEG
  EXPECT_FALSE(f.has_alpha);
  EXPECT_FALSE(f.is_grayscale);
  EXPECT_FLOAT_EQ(f.edge_density, 0.25f);
  EXPECT_FLOAT_EQ(f.noise_level, 0.1f);
  EXPECT_FLOAT_EQ(f.unique_color_ratio, 0.8f);
  EXPECT_FLOAT_EQ(f.photo_metric, 22.0f);
}

TEST(QualityFeaturesTest, GrayscaleDetected) {
  auto analysis = MakeAnalysis(ContentClass::kPhoto, 0.1f, 0.05f, 0.5f, 20.0f);
  auto pixels = GrayscaleGradient(64, 64);
  auto f = ExtractImageFeatures(analysis, pixels.data(), 64, 64, 1, 0);

  EXPECT_TRUE(f.is_grayscale);
  EXPECT_FALSE(f.has_alpha);
}

TEST(QualityFeaturesTest, AlphaDetected) {
  auto analysis = MakeAnalysis(ContentClass::kPhoto, 0.1f, 0.05f, 0.5f, 20.0f);
  auto pixels = SolidRgba(64, 64, 128, 128, 128, 255);
  auto f = ExtractImageFeatures(analysis, pixels.data(), 64, 64, 4, 3);

  EXPECT_TRUE(f.has_alpha);
  EXPECT_FALSE(f.is_grayscale);
  EXPECT_EQ(f.source_format, 3);  // WebP
}

// ---------------------------------------------------------------------------
// Color entropy tests.
// ---------------------------------------------------------------------------

TEST(QualityFeaturesTest, SolidColorHasLowEntropy) {
  auto analysis = MakeAnalysis(ContentClass::kPhoto, 0.0f, 0.0f, 0.01f, 0.0f);
  auto pixels = SolidRgb(64, 64, 200, 50, 50);
  auto f = ExtractImageFeatures(analysis, pixels.data(), 64, 64, 3, 0);

  // Solid color → all samples land in one hue bin → entropy = 0.
  EXPECT_NEAR(f.color_entropy, 0.0f, 0.01f);
}

TEST(QualityFeaturesTest, CheckerboardHasHigherEntropy) {
  auto analysis = MakeAnalysis(ContentClass::kPhoto, 0.0f, 0.0f, 0.5f, 0.0f);
  // Red vs cyan — different hue bins.
  auto pixels = Checkerboard(64, 64, 255, 0, 0, 0, 255, 255);
  auto f = ExtractImageFeatures(analysis, pixels.data(), 64, 64, 3, 0);

  // Two distinct hue bins → entropy > 0.
  EXPECT_GT(f.color_entropy, 0.3f);
}

TEST(QualityFeaturesTest, GrayscaleEntropyIsZero) {
  auto analysis = MakeAnalysis(ContentClass::kPhoto, 0.0f, 0.0f, 0.01f, 0.0f);
  auto pixels = GrayscaleGradient(64, 64);
  auto f = ExtractImageFeatures(analysis, pixels.data(), 64, 64, 1, 0);

  // Grayscale has no hue → all bin 0 → entropy = 0.
  EXPECT_NEAR(f.color_entropy, 0.0f, 0.01f);
}

// ---------------------------------------------------------------------------
// Spatial frequency tests.
// ---------------------------------------------------------------------------

TEST(QualityFeaturesTest, SolidColorHasLowSpatialFrequency) {
  auto analysis = MakeAnalysis(ContentClass::kPhoto, 0.0f, 0.0f, 0.01f, 0.0f);
  auto pixels = SolidRgb(64, 64, 128, 128, 128);
  auto f = ExtractImageFeatures(analysis, pixels.data(), 64, 64, 3, 0);

  // Constant luminance → all AC coefficients near zero.
  EXPECT_NEAR(f.spatial_freq_low, 0.0f, 1.0f);
  EXPECT_NEAR(f.spatial_freq_high, 0.0f, 1.0f);
}

TEST(QualityFeaturesTest, CheckerboardHasHighFrequency) {
  auto analysis = MakeAnalysis(ContentClass::kPhoto, 0.0f, 0.0f, 0.5f, 0.0f);
  // Black/white checkerboard — max high-frequency content.
  auto pixels = Checkerboard(64, 64, 0, 0, 0, 255, 255, 255);
  auto f = ExtractImageFeatures(analysis, pixels.data(), 64, 64, 3, 0);

  // High-freq energy should dominate.
  EXPECT_GT(f.spatial_freq_high, 100.0f);
  EXPECT_GT(f.spatial_freq_high, f.spatial_freq_low);
}

TEST(QualityFeaturesTest, SmallImageNoBlocks) {
  // 3x3 image: no complete 8x8 blocks → spatial freq stays at 0.
  auto analysis = MakeAnalysis(ContentClass::kPhoto, 0.0f, 0.0f, 0.5f, 0.0f);
  auto pixels = SolidRgb(3, 3, 128, 128, 128);
  auto f = ExtractImageFeatures(analysis, pixels.data(), 3, 3, 3, 0);

  EXPECT_FLOAT_EQ(f.spatial_freq_low, 0.0f);
  EXPECT_FLOAT_EQ(f.spatial_freq_high, 0.0f);
}

// ---------------------------------------------------------------------------
// Luminance statistics tests.
// ---------------------------------------------------------------------------

TEST(QualityFeaturesTest, SolidGrayLuminance) {
  auto analysis = MakeAnalysis(ContentClass::kPhoto, 0.0f, 0.0f, 0.01f, 0.0f);
  auto pixels = SolidRgb(64, 64, 128, 128, 128);
  auto f = ExtractImageFeatures(analysis, pixels.data(), 64, 64, 3, 0);

  // BT.601: 0.299*128 + 0.587*128 + 0.114*128 = 128.0
  EXPECT_NEAR(f.mean_luminance, 128.0f, 1.0f);
  EXPECT_NEAR(f.luminance_variance, 0.0f, 1.0f);
}

TEST(QualityFeaturesTest, GradientHasVariance) {
  auto analysis = MakeAnalysis(ContentClass::kPhoto, 0.0f, 0.0f, 0.5f, 0.0f);
  auto pixels = GrayscaleGradient(256, 256);
  auto f = ExtractImageFeatures(analysis, pixels.data(), 256, 256, 1, 0);

  // Mean should be near 127-128.
  EXPECT_NEAR(f.mean_luminance, 127.5f, 2.0f);
  // Variance of 0..255 uniform is ~5460 — allow wide margin for discrete sampling.
  EXPECT_GT(f.luminance_variance, 1000.0f);
}

// ---------------------------------------------------------------------------
// Null / edge case handling.
// ---------------------------------------------------------------------------

TEST(QualityFeaturesTest, NullPixelsReturnsDefaults) {
  auto analysis = MakeAnalysis(ContentClass::kUnknown, 0.0f, 0.0f, 0.0f, 0.0f);
  auto f = ExtractImageFeatures(analysis, nullptr, 100, 100, 3, 0);

  // Should return defaults without crashing.
  EXPECT_FLOAT_EQ(f.color_entropy, 0.0f);
  EXPECT_FLOAT_EQ(f.spatial_freq_low, 0.0f);
  EXPECT_FLOAT_EQ(f.spatial_freq_high, 0.0f);
  EXPECT_FLOAT_EQ(f.mean_luminance, 0.0f);
  EXPECT_FLOAT_EQ(f.luminance_variance, 0.0f);
}

TEST(QualityFeaturesTest, ZeroDimensionsReturnsDefaults) {
  auto analysis = MakeAnalysis(ContentClass::kUnknown, 0.0f, 0.0f, 0.0f, 0.0f);
  uint8_t dummy = 0;
  auto f = ExtractImageFeatures(analysis, &dummy, 0, 0, 3, 0);

  EXPECT_FLOAT_EQ(f.color_entropy, 0.0f);
  EXPECT_FLOAT_EQ(f.mean_luminance, 0.0f);
}

TEST(QualityFeaturesTest, InvalidBppReturnsDefaults) {
  auto analysis = MakeAnalysis(ContentClass::kUnknown, 0.0f, 0.0f, 0.0f, 0.0f);
  auto pixels = SolidRgb(16, 16, 128, 128, 128);
  auto f = ExtractImageFeatures(analysis, pixels.data(), 16, 16, 2, 0);

  EXPECT_FLOAT_EQ(f.color_entropy, 0.0f);
  EXPECT_FLOAT_EQ(f.mean_luminance, 0.0f);
}

// ---------------------------------------------------------------------------
// ToFloatArray ordering matches documented index mapping.
// ---------------------------------------------------------------------------

TEST(QualityFeaturesTest, ToFloatArrayOrdering) {
  ImageFeatures f;
  f.edge_density = 0.1f;
  f.noise_level = 0.2f;
  f.unique_color_ratio = 0.3f;
  f.photo_metric = 0.4f;
  f.color_entropy = 0.5f;
  f.spatial_freq_low = 0.6f;
  f.spatial_freq_high = 0.7f;
  f.mean_luminance = 0.8f;
  f.luminance_variance = 0.9f;
  f.width = 100;
  f.height = 200;
  f.content_class = 1;
  f.source_format = 2;
  f.has_alpha = true;
  f.is_grayscale = false;
  f.source_quality = 75;

  float out[kNumFeatures];
  f.ToFloatArray(out, 70.0f);

  EXPECT_FLOAT_EQ(out[0], 0.1f);       // edge_density
  EXPECT_FLOAT_EQ(out[1], 0.2f);       // noise_level
  EXPECT_FLOAT_EQ(out[2], 0.3f);       // unique_color_ratio
  EXPECT_FLOAT_EQ(out[3], 0.4f);       // photo_metric
  EXPECT_FLOAT_EQ(out[4], 0.5f);       // color_entropy
  EXPECT_FLOAT_EQ(out[5], 0.6f);       // spatial_freq_low
  EXPECT_FLOAT_EQ(out[6], 0.7f);       // spatial_freq_high
  EXPECT_FLOAT_EQ(out[7], 0.8f);       // mean_luminance
  EXPECT_FLOAT_EQ(out[8], 0.9f);       // luminance_variance
  EXPECT_FLOAT_EQ(out[9], 100.0f);     // width
  EXPECT_FLOAT_EQ(out[10], 200.0f);    // height
  EXPECT_FLOAT_EQ(out[11], 1.0f);      // content_class
  EXPECT_FLOAT_EQ(out[12], 2.0f);      // source_format
  EXPECT_FLOAT_EQ(out[13], 1.0f);      // has_alpha = true
  EXPECT_FLOAT_EQ(out[14], 0.0f);      // is_grayscale = false
  EXPECT_FLOAT_EQ(out[15], 70.0f);     // target_ssimulacra2
  EXPECT_FLOAT_EQ(out[16], 20000.0f);  // pixel_count = 100*200
  EXPECT_FLOAT_EQ(out[17], 75.0f);     // source_quality
}

// ---------------------------------------------------------------------------
// Determinism: seeded RNG produces identical output on repeated calls.
// ---------------------------------------------------------------------------

TEST(QualityFeaturesTest, Deterministic) {
  auto analysis = MakeAnalysis(ContentClass::kPhoto, 0.3f, 0.15f, 0.6f, 18.0f);
  auto pixels = Checkerboard(128, 128, 255, 0, 0, 0, 255, 0);

  auto f1 = ExtractImageFeatures(analysis, pixels.data(), 128, 128, 3, 0);
  auto f2 = ExtractImageFeatures(analysis, pixels.data(), 128, 128, 3, 0);

  EXPECT_FLOAT_EQ(f1.color_entropy, f2.color_entropy);
  EXPECT_FLOAT_EQ(f1.spatial_freq_low, f2.spatial_freq_low);
  EXPECT_FLOAT_EQ(f1.spatial_freq_high, f2.spatial_freq_high);
  EXPECT_FLOAT_EQ(f1.mean_luminance, f2.mean_luminance);
  EXPECT_FLOAT_EQ(f1.luminance_variance, f2.luminance_variance);
}

// =================================================================
// Quality Baselining: kNumFeatures and source_quality
// =================================================================

TEST(QualityBaselineFeaturesTest, KNumFeaturesIs18) {
  EXPECT_EQ(kNumFeatures, 18);
}

TEST(QualityBaselineFeaturesTest, SourceQualityPopulated) {
  auto analysis = MakeAnalysis(ContentClass::kPhoto, 0.5f, 0.1f, 0.3f, 12.0f);
  auto pixels = SolidRgb(64, 64, 128, 128, 128);

  auto features = ExtractImageFeatures(analysis, pixels.data(), 64, 64, 3,
                                       /*source_format=*/0,
                                       /*source_quality=*/75);
  EXPECT_EQ(features.source_quality, 75);
}

TEST(QualityBaselineFeaturesTest, SourceQualityDefaultNegativeOne) {
  auto analysis = MakeAnalysis(ContentClass::kPhoto, 0.5f, 0.1f, 0.3f, 12.0f);
  auto pixels = SolidRgb(64, 64, 128, 128, 128);

  // Default source_quality parameter is -1.
  auto features = ExtractImageFeatures(analysis, pixels.data(), 64, 64, 3,
                                       /*source_format=*/0);
  EXPECT_EQ(features.source_quality, -1);
}

TEST(QualityBaselineFeaturesTest, ToFloatArraySize) {
  ImageFeatures f;
  float arr[kNumFeatures];
  f.ToFloatArray(arr, 70.0f);
  // Verify that pixel_count (index 16) is width*height.
  f.width = 100;
  f.height = 200;
  f.ToFloatArray(arr, 70.0f);
  EXPECT_FLOAT_EQ(arr[16], 20000.0f);
}

TEST(QualityBaselineFeaturesTest, SourceQualityInToFloatArray) {
  ImageFeatures f;
  f.width = 64;
  f.height = 64;
  f.source_quality = 85;
  float arr[kNumFeatures];
  f.ToFloatArray(arr, 70.0f);
  EXPECT_FLOAT_EQ(arr[17], 85.0f);
}

TEST(QualityBaselineFeaturesTest, SourceQualityNanWhenUnavailable) {
  ImageFeatures f;
  f.width = 64;
  f.height = 64;
  f.source_quality = -1;  // Non-JPEG or unknown.
  float arr[kNumFeatures];
  f.ToFloatArray(arr, 70.0f);
  EXPECT_TRUE(std::isnan(arr[17]))
      << "source_quality should be NaN when unavailable, got " << arr[17];
}

}  // namespace
}  // namespace pagespeed
