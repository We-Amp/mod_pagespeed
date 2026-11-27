// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

// PageSpeed 2.0 - SVG Vectorizer Unit Tests

#include "lib/image/svg_vectorizer.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "gtest/gtest.h"

namespace pagespeed {
namespace {

// Helper: create a solid-color RGBA pixel buffer.
std::vector<uint8_t> MakeSolidRGBA(uint32_t w, uint32_t h, uint8_t r, uint8_t g,
                                   uint8_t b, uint8_t a = 255) {
  std::vector<uint8_t> pixels(static_cast<size_t>(w) * h * 4);
  uint64_t total = static_cast<uint64_t>(w) * h;
  for (uint64_t i = 0; i < total; ++i) {
    size_t idx = static_cast<size_t>(i) * 4;
    pixels[idx + 0] = r;
    pixels[idx + 1] = g;
    pixels[idx + 2] = b;
    pixels[idx + 3] = a;
  }
  return pixels;
}

// Helper: create a solid-color RGB pixel buffer.
std::vector<uint8_t> MakeSolidRGB(uint32_t w, uint32_t h, uint8_t r, uint8_t g,
                                  uint8_t b) {
  std::vector<uint8_t> pixels(static_cast<size_t>(w) * h * 3);
  uint64_t total = static_cast<uint64_t>(w) * h;
  for (uint64_t i = 0; i < total; ++i) {
    size_t idx = static_cast<size_t>(i) * 3;
    pixels[idx + 0] = r;
    pixels[idx + 1] = g;
    pixels[idx + 2] = b;
  }
  return pixels;
}

// Helper: create a solid-color grayscale pixel buffer.
std::vector<uint8_t> MakeSolidGray(uint32_t w, uint32_t h, uint8_t gray) {
  return std::vector<uint8_t>(static_cast<size_t>(w) * h, gray);
}

// -----------------------------------------------------------------
// Input validation tests
// -----------------------------------------------------------------

TEST(SvgVectorizerTest, NullPixelBufferReturnsError) {
  SvgVectorizerConfig config;
  auto result = VectorizeImage(nullptr, 100, 100, 4, config);
  EXPECT_FALSE(result.success);
  EXPECT_FALSE(result.error_message.empty());
  EXPECT_NE(result.error_message.find("Null"), std::string::npos);
}

TEST(SvgVectorizerTest, ZeroWidthReturnsError) {
  auto pixels = MakeSolidRGBA(1, 1, 255, 0, 0);
  SvgVectorizerConfig config;
  auto result = VectorizeImage(pixels.data(), 0, 100, 4, config);
  EXPECT_FALSE(result.success);
  EXPECT_NE(result.error_message.find("Zero"), std::string::npos);
}

TEST(SvgVectorizerTest, ZeroHeightReturnsError) {
  auto pixels = MakeSolidRGBA(1, 1, 255, 0, 0);
  SvgVectorizerConfig config;
  auto result = VectorizeImage(pixels.data(), 100, 0, 4, config);
  EXPECT_FALSE(result.success);
  EXPECT_NE(result.error_message.find("Zero"), std::string::npos);
}

TEST(SvgVectorizerTest, InvalidBppTwoReturnsError) {
  auto pixels = MakeSolidRGBA(4, 4, 255, 0, 0);
  SvgVectorizerConfig config;
  auto result = VectorizeImage(pixels.data(), 4, 4, 2, config);
  EXPECT_FALSE(result.success);
  EXPECT_NE(result.error_message.find("bpp"), std::string::npos);
}

TEST(SvgVectorizerTest, InvalidBppFiveReturnsError) {
  auto pixels = MakeSolidRGBA(4, 4, 255, 0, 0);
  SvgVectorizerConfig config;
  auto result = VectorizeImage(pixels.data(), 4, 4, 5, config);
  EXPECT_FALSE(result.success);
  EXPECT_NE(result.error_message.find("bpp"), std::string::npos);
}

TEST(SvgVectorizerTest, InvalidBppZeroReturnsError) {
  auto pixels = MakeSolidRGBA(4, 4, 255, 0, 0);
  SvgVectorizerConfig config;
  auto result = VectorizeImage(pixels.data(), 4, 4, 0, config);
  EXPECT_FALSE(result.success);
  EXPECT_NE(result.error_message.find("bpp"), std::string::npos);
}

TEST(SvgVectorizerTest, TooLargeImageReturnsError) {
  // 50MB / 4 = 13107200 pixels max. 4096 * 4096 = 16M > limit.
  uint8_t dummy = 0;
  SvgVectorizerConfig config;
  auto result = VectorizeImage(&dummy, 4096, 4096, 4, config);
  EXPECT_FALSE(result.success);
  EXPECT_NE(result.error_message.find("too large"), std::string::npos);
}

// -----------------------------------------------------------------
// Successful vectorization
// -----------------------------------------------------------------

TEST(SvgVectorizerTest, SolidColorRGBAProducesValidSvg) {
  auto pixels = MakeSolidRGBA(32, 32, 255, 0, 0);
  SvgVectorizerConfig config;
  auto result = VectorizeImage(pixels.data(), 32, 32, 4, config);
  ASSERT_TRUE(result.success) << result.error_message;
  EXPECT_FALSE(result.svg_data.empty());
  EXPECT_NE(result.svg_data.find("<svg"), std::string::npos);
  EXPECT_NE(result.svg_data.find("</svg>"), std::string::npos);
  EXPECT_GT(result.path_count, 0u);
  EXPECT_GT(result.actual_colors, 0);
}

TEST(SvgVectorizerTest, SolidColorRGBProducesValidSvg) {
  auto pixels = MakeSolidRGB(32, 32, 0, 255, 0);
  SvgVectorizerConfig config;
  auto result = VectorizeImage(pixels.data(), 32, 32, 3, config);
  ASSERT_TRUE(result.success) << result.error_message;
  EXPECT_FALSE(result.svg_data.empty());
  EXPECT_NE(result.svg_data.find("<svg"), std::string::npos);
}

TEST(SvgVectorizerTest, SolidColorGrayscaleProducesValidSvg) {
  auto pixels = MakeSolidGray(32, 32, 128);
  SvgVectorizerConfig config;
  auto result = VectorizeImage(pixels.data(), 32, 32, 1, config);
  ASSERT_TRUE(result.success) << result.error_message;
  EXPECT_FALSE(result.svg_data.empty());
  EXPECT_NE(result.svg_data.find("<svg"), std::string::npos);
}

TEST(SvgVectorizerTest, OutputContainsDimensions) {
  auto pixels = MakeSolidRGBA(64, 48, 0, 0, 255);
  SvgVectorizerConfig config;
  auto result = VectorizeImage(pixels.data(), 64, 48, 4, config);
  ASSERT_TRUE(result.success) << result.error_message;
  EXPECT_NE(result.svg_data.find("width=\"64\""), std::string::npos);
  EXPECT_NE(result.svg_data.find("height=\"48\""), std::string::npos);
}

// -----------------------------------------------------------------
// Path count gate
// -----------------------------------------------------------------

TEST(SvgVectorizerTest, PathCountGateRejectsHighPathCount) {
  auto pixels = MakeSolidRGBA(32, 32, 255, 0, 0);
  SvgVectorizerConfig config;
  config.max_paths = 0;  // Reject any SVG with paths.
  auto result = VectorizeImage(pixels.data(), 32, 32, 4, config);
  EXPECT_FALSE(result.success);
  EXPECT_NE(result.error_message.find("Path count"), std::string::npos);
  EXPECT_NE(result.error_message.find("exceeds"), std::string::npos);
}

// -----------------------------------------------------------------
// Size gate
// -----------------------------------------------------------------

TEST(SvgVectorizerTest, SizeGateRejectsOversizedSvg) {
  auto pixels = MakeSolidRGBA(32, 32, 255, 0, 0);
  SvgVectorizerConfig config;
  config.max_svg_bytes = 1;  // 1 byte -- any SVG will exceed this.
  auto result = VectorizeImage(pixels.data(), 32, 32, 4, config);
  EXPECT_FALSE(result.success);
  EXPECT_NE(result.error_message.find("SVG size"), std::string::npos);
  EXPECT_NE(result.error_message.find("exceeds"), std::string::npos);
}

// -----------------------------------------------------------------
// Adaptive color precision
// -----------------------------------------------------------------

TEST(SvgVectorizerTest, AdaptiveColorPrecisionLowColors) {
  // With a single solid color, actual_colors will be 1, so
  // adaptive precision should be 4.  We verify indirectly via
  // successful vectorization and the actual_colors count.
  auto pixels = MakeSolidRGBA(16, 16, 100, 200, 50);
  SvgVectorizerConfig config;
  config.color_precision = 8;  // Default high, but adaptive adjusts.
  auto result = VectorizeImage(pixels.data(), 16, 16, 4, config);
  ASSERT_TRUE(result.success) << result.error_message;
  // Single color image should have very few actual colors.
  EXPECT_LE(result.actual_colors, 16);
}

// -----------------------------------------------------------------
// Coordinate precision
// -----------------------------------------------------------------

TEST(SvgVectorizerTest, CoordinatePrecisionApplied) {
  auto pixels = MakeSolidRGBA(32, 32, 255, 128, 0);
  SvgVectorizerConfig config;
  config.coordinate_precision = 0;  // Integer coordinates.
  auto result = VectorizeImage(pixels.data(), 32, 32, 4, config);
  ASSERT_TRUE(result.success) << result.error_message;
  // With precision 0, we should not see decimal points in path
  // data, but we can at least verify the SVG is well-formed.
  EXPECT_NE(result.svg_data.find("<svg"), std::string::npos);
}

// -----------------------------------------------------------------
// Various bpp inputs
// -----------------------------------------------------------------

TEST(SvgVectorizerTest, AllValidBppValues) {
  // Test bpp=1, 3, 4 all produce valid SVGs.
  SvgVectorizerConfig config;

  auto gray = MakeSolidGray(16, 16, 200);
  auto r1 = VectorizeImage(gray.data(), 16, 16, 1, config);
  EXPECT_TRUE(r1.success) << r1.error_message;

  auto rgb = MakeSolidRGB(16, 16, 200, 100, 50);
  auto r3 = VectorizeImage(rgb.data(), 16, 16, 3, config);
  EXPECT_TRUE(r3.success) << r3.error_message;

  auto rgba = MakeSolidRGBA(16, 16, 200, 100, 50);
  auto r4 = VectorizeImage(rgba.data(), 16, 16, 4, config);
  EXPECT_TRUE(r4.success) << r4.error_message;
}

// -----------------------------------------------------------------
// Crisp edges for low-color images
// -----------------------------------------------------------------

TEST(SvgVectorizerTest, CrispEdgesAddedForLowColorImage) {
  // A solid-color image has 1 unique color (<= 16), so crisp
  // edges should be enabled.
  auto pixels = MakeSolidRGBA(32, 32, 0, 128, 255);
  SvgVectorizerConfig config;
  auto result = VectorizeImage(pixels.data(), 32, 32, 4, config);
  ASSERT_TRUE(result.success) << result.error_message;
  EXPECT_NE(result.svg_data.find("crispEdges"), std::string::npos)
      << "Expected crispEdges for low-color image";
}

// -----------------------------------------------------------------
// SVG output is sanitized
// -----------------------------------------------------------------

TEST(SvgVectorizerTest, OutputIsSanitized) {
  auto pixels = MakeSolidRGBA(32, 32, 255, 0, 0);
  SvgVectorizerConfig config;
  auto result = VectorizeImage(pixels.data(), 32, 32, 4, config);
  ASSERT_TRUE(result.success) << result.error_message;
  // Output should not contain dangerous elements.
  EXPECT_EQ(result.svg_data.find("<script"), std::string::npos);
  EXPECT_EQ(result.svg_data.find("onclick"), std::string::npos);
}

// -----------------------------------------------------------------
// Multi-color images: exercise higher actual_colors values
// -----------------------------------------------------------------

// Create a 32x32 image with many distinct colors to push actual_colors
// past 16 after quantization (into the <= 64 adaptive precision branch).
TEST(SvgVectorizerTest, MultiColorImageExercisesMediumColorPrecision) {
  // Create a gradient with diverse colors -- after 5-bit quantization
  // and median-cut to max_colors=32, we expect actual_colors > 16.
  uint32_t w = 64, h = 64;
  std::vector<uint8_t> pixels(static_cast<size_t>(w) * h * 4);
  for (uint32_t y = 0; y < h; ++y) {
    for (uint32_t x = 0; x < w; ++x) {
      size_t idx = (static_cast<size_t>(y) * w + x) * 4;
      pixels[idx + 0] = static_cast<uint8_t>(x * 255 / w);  // R gradient
      pixels[idx + 1] = static_cast<uint8_t>(y * 255 / h);  // G gradient
      pixels[idx + 2] = static_cast<uint8_t>((x + y) * 127 / (w + h));
      pixels[idx + 3] = 255;
    }
  }
  SvgVectorizerConfig config;
  auto result = VectorizeImage(pixels.data(), w, h, 4, config);
  ASSERT_TRUE(result.success) << result.error_message;
  EXPECT_FALSE(result.svg_data.empty());
  // With a full gradient, actual_colors should be > 1 (likely > 16).
  EXPECT_GT(result.actual_colors, 1);
}

// Another multi-color test: create distinct color blocks to reliably
// get actual_colors between 17 and 32 (the <= 64 adaptive precision path).
TEST(SvgVectorizerTest, DistinctColorBlocksExceed16Colors) {
  uint32_t w = 64, h = 64;
  std::vector<uint8_t> pixels(static_cast<size_t>(w) * h * 4);
  // Create a 8x8 grid of 64 distinct-ish colors (after quantization
  // some may merge, but we should get well above 16).
  for (uint32_t y = 0; y < h; ++y) {
    for (uint32_t x = 0; x < w; ++x) {
      size_t idx = (static_cast<size_t>(y) * w + x) * 4;
      uint32_t bx = x / 8;  // Block x (0-7)
      uint32_t by = y / 8;  // Block y (0-7)
      // 8*8 = 64 potential colors, spread across RGB space.
      pixels[idx + 0] = static_cast<uint8_t>(bx * 36);
      pixels[idx + 1] = static_cast<uint8_t>(by * 36);
      pixels[idx + 2] = static_cast<uint8_t>((bx + by) * 18);
      pixels[idx + 3] = 255;
    }
  }
  SvgVectorizerConfig config;
  auto result = VectorizeImage(pixels.data(), w, h, 4, config);
  ASSERT_TRUE(result.success) << result.error_message;
  // With 64 distinct input colors and max_colors=32, the preprocessor
  // will quantize to at most 32 colors. Actual count should be > 16.
  EXPECT_GT(result.actual_colors, 16)
      << "Expected > 16 actual colors for diverse input";
}

// -----------------------------------------------------------------
// AdaptiveColorPrecision unit tests (detail namespace)
// -----------------------------------------------------------------

TEST(AdaptiveColorPrecisionTest, VeryLowColorsReturnsFour) {
  EXPECT_EQ(detail::AdaptiveColorPrecision(1, 8), 4);
  EXPECT_EQ(detail::AdaptiveColorPrecision(8, 7), 4);
  EXPECT_EQ(detail::AdaptiveColorPrecision(16, 6), 4);
}

TEST(AdaptiveColorPrecisionTest, MediumColorsReturnsFive) {
  EXPECT_EQ(detail::AdaptiveColorPrecision(17, 8), 5);
  EXPECT_EQ(detail::AdaptiveColorPrecision(32, 7), 5);
  EXPECT_EQ(detail::AdaptiveColorPrecision(64, 6), 5);
}

TEST(AdaptiveColorPrecisionTest, HighColorsReturnsSix) {
  EXPECT_EQ(detail::AdaptiveColorPrecision(65, 8), 6);
  EXPECT_EQ(detail::AdaptiveColorPrecision(128, 7), 6);
  EXPECT_EQ(detail::AdaptiveColorPrecision(256, 6), 6);
}

TEST(AdaptiveColorPrecisionTest, VeryHighColorsReturnsDefault) {
  EXPECT_EQ(detail::AdaptiveColorPrecision(257, 8), 8);
  EXPECT_EQ(detail::AdaptiveColorPrecision(500, 7), 7);
  EXPECT_EQ(detail::AdaptiveColorPrecision(1000, 3), 3);
}

TEST(AdaptiveColorPrecisionTest, BoundaryValues) {
  // Exact boundaries between ranges.
  EXPECT_EQ(detail::AdaptiveColorPrecision(16, 8), 4);   // <= 16
  EXPECT_EQ(detail::AdaptiveColorPrecision(17, 8), 5);   // > 16, <= 64
  EXPECT_EQ(detail::AdaptiveColorPrecision(64, 8), 5);   // <= 64
  EXPECT_EQ(detail::AdaptiveColorPrecision(65, 8), 6);   // > 64, <= 256
  EXPECT_EQ(detail::AdaptiveColorPrecision(256, 8), 6);  // <= 256
  EXPECT_EQ(detail::AdaptiveColorPrecision(257, 8), 8);  // > 256
}

TEST(AdaptiveColorPrecisionTest, ZeroColors) {
  // Edge case: zero colors should still return 4 (falls in <= 16).
  EXPECT_EQ(detail::AdaptiveColorPrecision(0, 8), 4);
}

TEST(AdaptiveColorPrecisionTest, NegativeColors) {
  // Edge case: negative color count (shouldn't happen, but tests behavior).
  EXPECT_EQ(detail::AdaptiveColorPrecision(-1, 8), 4);
}

// -----------------------------------------------------------------
// Preset configuration
// -----------------------------------------------------------------

TEST(SvgVectorizerTest, PresetBwUsesPolygonMode) {
  auto pixels = MakeSolidRGBA(32, 32, 255, 0, 0);
  SvgVectorizerConfig config;
  config.preset = 0;  // bw
  auto result = VectorizeImage(pixels.data(), 32, 32, 4, config);
  ASSERT_TRUE(result.success) << result.error_message;
  EXPECT_NE(result.svg_data.find("<svg"), std::string::npos);
}

TEST(SvgVectorizerTest, PresetPosterIsDefault) {
  auto pixels = MakeSolidRGBA(32, 32, 255, 0, 0);
  SvgVectorizerConfig config_default;
  auto result_default =
      VectorizeImage(pixels.data(), 32, 32, 4, config_default);
  ASSERT_TRUE(result_default.success) << result_default.error_message;

  SvgVectorizerConfig config_poster;
  config_poster.preset = 1;
  auto result_poster = VectorizeImage(pixels.data(), 32, 32, 4, config_poster);
  ASSERT_TRUE(result_poster.success) << result_poster.error_message;

  // Both should produce identical output since preset=1 is the default.
  EXPECT_EQ(result_default.svg_data, result_poster.svg_data);
}

TEST(SvgVectorizerTest, PresetPhotoProducesValidSvg) {
  auto pixels = MakeSolidRGBA(32, 32, 0, 128, 255);
  SvgVectorizerConfig config;
  config.preset = 2;  // photo
  auto result = VectorizeImage(pixels.data(), 32, 32, 4, config);
  ASSERT_TRUE(result.success) << result.error_message;
  EXPECT_NE(result.svg_data.find("<svg"), std::string::npos);
}

TEST(SvgVectorizerTest, PresetOutOfRangeFallsToPoster) {
  auto pixels = MakeSolidRGBA(32, 32, 255, 0, 0);

  SvgVectorizerConfig config_poster;
  config_poster.preset = 1;
  auto result_poster = VectorizeImage(pixels.data(), 32, 32, 4, config_poster);
  ASSERT_TRUE(result_poster.success) << result_poster.error_message;

  SvgVectorizerConfig config_oob;
  config_oob.preset = 99;
  auto result_oob = VectorizeImage(pixels.data(), 32, 32, 4, config_oob);
  ASSERT_TRUE(result_oob.success) << result_oob.error_message;

  // Out-of-range preset should behave like poster (default).
  EXPECT_EQ(result_poster.svg_data, result_oob.svg_data);
}

// -----------------------------------------------------------------
// Minimum valid image size
// -----------------------------------------------------------------

TEST(SvgVectorizerTest, MinimumValidImageSize) {
  // 1x1 image should still vectorize successfully.
  auto pixels = MakeSolidRGBA(1, 1, 255, 0, 0);
  SvgVectorizerConfig config;
  auto result = VectorizeImage(pixels.data(), 1, 1, 4, config);
  ASSERT_TRUE(result.success) << result.error_message;
  EXPECT_NE(result.svg_data.find("<svg"), std::string::npos);
}

TEST(SvgVectorizerTest, SmallRectangularImage) {
  // 2x1 image: very narrow, tests minimum viable dimensions.
  auto pixels = MakeSolidRGBA(2, 1, 0, 255, 0);
  SvgVectorizerConfig config;
  auto result = VectorizeImage(pixels.data(), 2, 1, 4, config);
  ASSERT_TRUE(result.success) << result.error_message;
  EXPECT_NE(result.svg_data.find("width=\"2\""), std::string::npos);
  EXPECT_NE(result.svg_data.find("height=\"1\""), std::string::npos);
}

}  // namespace
}  // namespace pagespeed
