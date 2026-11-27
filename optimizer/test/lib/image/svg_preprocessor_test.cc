// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

// PageSpeed 2.0 - SVG Preprocessor Unit Tests

#include "lib/image/svg_preprocessor.h"

#include <cstdint>
#include <cstring>
#include <set>
#include <vector>

#include "gtest/gtest.h"

namespace pagespeed {
namespace {

// Helper: count unique RGB colors in a pixel buffer (ignores alpha).
int CountUniqueColors(const uint8_t* pixels, uint32_t width, uint32_t height,
                      int bpp) {
  std::set<uint32_t> colors;
  uint64_t total = static_cast<uint64_t>(width) * height;
  for (uint64_t i = 0; i < total; ++i) {
    size_t idx = static_cast<size_t>(i) * bpp;
    uint32_t packed = 0;
    if (bpp >= 3) {
      packed = (static_cast<uint32_t>(pixels[idx]) << 16) |
               (static_cast<uint32_t>(pixels[idx + 1]) << 8) |
               static_cast<uint32_t>(pixels[idx + 2]);
    } else {
      packed = pixels[idx];
    }
    colors.insert(packed);
  }
  return static_cast<int>(colors.size());
}

// Helper: create image with many distinct colors using LCG PRNG.
std::vector<uint8_t> MakeMultiColorImage(uint32_t w, uint32_t h, int bpp,
                                         uint32_t seed = 42) {
  std::vector<uint8_t> pixels(static_cast<size_t>(w) * h * bpp);
  uint32_t state = seed;
  for (size_t i = 0; i < pixels.size(); ++i) {
    state = state * 1103515245 + 12345;
    pixels[i] = static_cast<uint8_t>((state >> 16) & 0xFF);
  }
  return pixels;
}

// Helper: create a solid color image.
std::vector<uint8_t> MakeSolidImage(uint32_t w, uint32_t h, int bpp, uint8_t r,
                                    uint8_t g, uint8_t b, uint8_t a = 255) {
  std::vector<uint8_t> pixels(static_cast<size_t>(w) * h * bpp);
  uint64_t total = static_cast<uint64_t>(w) * h;
  for (uint64_t i = 0; i < total; ++i) {
    size_t idx = static_cast<size_t>(i) * bpp;
    if (bpp >= 3) {
      pixels[idx + 0] = r;
      pixels[idx + 1] = g;
      pixels[idx + 2] = b;
    }
    if (bpp == 4) {
      pixels[idx + 3] = a;
    }
    if (bpp == 1) {
      pixels[idx] = r;
    }
  }
  return pixels;
}

// --- QuantizeColors tests ---

TEST(SvgPreprocessorTest, QuantizeColorsReducesPalette) {
  // Create 10x10 image with many different colors.
  uint32_t w = 10, h = 10;
  auto pixels = MakeMultiColorImage(w, h, 4);

  int before = CountUniqueColors(pixels.data(), w, h, 4);
  EXPECT_GT(before, 8);

  QuantizeColors(pixels.data(), w, h, 4, 8);

  int after = CountUniqueColors(pixels.data(), w, h, 4);
  EXPECT_LE(after, 8);
}

TEST(SvgPreprocessorTest, QuantizeColorsPreservesAlpha) {
  uint32_t w = 8, h = 8;
  auto pixels = MakeMultiColorImage(w, h, 4);

  // Record original alpha values.
  uint64_t total = static_cast<uint64_t>(w) * h;
  std::vector<uint8_t> original_alpha(total);
  for (uint64_t i = 0; i < total; ++i) {
    original_alpha[i] = pixels[i * 4 + 3];
  }

  QuantizeColors(pixels.data(), w, h, 4, 4);

  // Verify alpha is unchanged.
  for (uint64_t i = 0; i < total; ++i) {
    EXPECT_EQ(original_alpha[i], pixels[i * 4 + 3])
        << "Alpha changed at pixel " << i;
  }
}

TEST(SvgPreprocessorTest, QuantizeColorsSingleColorUnchanged) {
  uint32_t w = 8, h = 8;
  auto pixels = MakeSolidImage(w, h, 4, 100, 150, 200, 255);

  QuantizeColors(pixels.data(), w, h, 4, 8);

  uint64_t total = static_cast<uint64_t>(w) * h;
  for (uint64_t i = 0; i < total; ++i) {
    size_t idx = static_cast<size_t>(i) * 4;
    // After quantization the single color should remain (or be very
    // close due to 5-bit bucketing).
    EXPECT_NEAR(100, pixels[idx + 0], 8);
    EXPECT_NEAR(150, pixels[idx + 1], 8);
    EXPECT_NEAR(200, pixels[idx + 2], 8);
    EXPECT_EQ(255, pixels[idx + 3]);
  }
}

TEST(SvgPreprocessorTest, QuantizeColorsMaxColorsLargerThanActual) {
  // Two colors only, max_colors=256 -- should be a no-op.
  uint32_t w = 4, h = 4;
  std::vector<uint8_t> pixels(static_cast<size_t>(w) * h * 4);
  for (uint32_t i = 0; i < w * h; ++i) {
    size_t idx = static_cast<size_t>(i) * 4;
    if (i % 2 == 0) {
      pixels[idx] = 0;
      pixels[idx + 1] = 0;
      pixels[idx + 2] = 0;
    } else {
      pixels[idx] = 255;
      pixels[idx + 1] = 255;
      pixels[idx + 2] = 255;
    }
    pixels[idx + 3] = 255;
  }

  auto original = pixels;
  QuantizeColors(pixels.data(), w, h, 4, 256);

  // Should remain unchanged since we have <= max_colors unique
  // bucket colors.
  EXPECT_EQ(original, pixels);
}

TEST(SvgPreprocessorTest, QuantizeColorsMaxColorsOne) {
  uint32_t w = 4, h = 4;
  auto pixels = MakeMultiColorImage(w, h, 4);

  QuantizeColors(pixels.data(), w, h, 4, 1);

  // All pixels should now be the same color.
  int unique = CountUniqueColors(pixels.data(), w, h, 4);
  EXPECT_EQ(1, unique);
}

TEST(SvgPreprocessorTest, QuantizeColorsRGB) {
  // Test with bpp=3.
  uint32_t w = 10, h = 10;
  auto pixels = MakeMultiColorImage(w, h, 3);

  int before = CountUniqueColors(pixels.data(), w, h, 3);
  EXPECT_GT(before, 4);

  QuantizeColors(pixels.data(), w, h, 3, 4);

  int after = CountUniqueColors(pixels.data(), w, h, 3);
  EXPECT_LE(after, 4);
}

// --- ThresholdAlpha tests ---

TEST(SvgPreprocessorTest, ThresholdAlphaProducesBinary) {
  uint32_t w = 5, h = 1;
  // RGBA pixels with varying alpha: 0, 64, 128, 192, 255.
  uint8_t pixels[] = {
      100, 100, 100, 0,    // alpha 0
      100, 100, 100, 64,   // alpha 64
      100, 100, 100, 128,  // alpha 128
      100, 100, 100, 192,  // alpha 192
      100, 100, 100, 255,  // alpha 255
  };

  ThresholdAlpha(pixels, w, h, 4, 128);

  EXPECT_EQ(0, pixels[3]);     // 0 < 128
  EXPECT_EQ(0, pixels[7]);     // 64 < 128
  EXPECT_EQ(255, pixels[11]);  // 128 >= 128
  EXPECT_EQ(255, pixels[15]);  // 192 >= 128
  EXPECT_EQ(255, pixels[19]);  // 255 >= 128
}

TEST(SvgPreprocessorTest, ThresholdAlphaPreservesRGB) {
  uint32_t w = 2, h = 1;
  uint8_t pixels[] = {
      10, 20, 30, 100, 40, 50, 60, 200,
  };

  ThresholdAlpha(pixels, w, h, 4, 128);

  // RGB channels unchanged.
  EXPECT_EQ(10, pixels[0]);
  EXPECT_EQ(20, pixels[1]);
  EXPECT_EQ(30, pixels[2]);
  EXPECT_EQ(40, pixels[4]);
  EXPECT_EQ(50, pixels[5]);
  EXPECT_EQ(60, pixels[6]);
}

TEST(SvgPreprocessorTest, ThresholdAlphaNoOpForRGB) {
  uint32_t w = 2, h = 1;
  uint8_t pixels[] = {10, 20, 30, 40, 50, 60};
  uint8_t original[] = {10, 20, 30, 40, 50, 60};

  ThresholdAlpha(pixels, w, h, 3, 128);

  // bpp=3: no-op, data unchanged.
  EXPECT_EQ(0, std::memcmp(pixels, original, sizeof(original)));
}

TEST(SvgPreprocessorTest, ThresholdAlphaCustomThreshold) {
  uint32_t w = 3, h = 1;
  uint8_t pixels[] = {
      0, 0, 0, 50,   // alpha 50
      0, 0, 0, 200,  // alpha 200
      0, 0, 0, 250,  // alpha 250
  };

  ThresholdAlpha(pixels, w, h, 4, 200);

  EXPECT_EQ(0, pixels[3]);     // 50 < 200
  EXPECT_EQ(255, pixels[7]);   // 200 >= 200
  EXPECT_EQ(255, pixels[11]);  // 250 >= 200
}

// --- MorphologicalClose tests ---

TEST(SvgPreprocessorTest, MorphologicalCloseFillsGap) {
  // 3x3 image: all alpha=255 except center pixel is 0.
  // Morphological close (dilate then erode) should fill the center.
  uint32_t w = 3, h = 3;
  std::vector<uint8_t> pixels(static_cast<size_t>(w) * h * 4);
  for (uint32_t i = 0; i < w * h; ++i) {
    size_t idx = static_cast<size_t>(i) * 4;
    pixels[idx + 0] = 128;
    pixels[idx + 1] = 128;
    pixels[idx + 2] = 128;
    pixels[idx + 3] = 255;  // All opaque.
  }
  // Set center pixel alpha to 0 (transparent gap).
  pixels[4 * 4 + 3] = 0;  // Pixel at (1,1).

  MorphologicalClose(pixels.data(), w, h, 4, 1);

  // After close, center should be opaque (255).
  EXPECT_EQ(255, pixels[4 * 4 + 3]);
}

TEST(SvgPreprocessorTest, MorphologicalClosePreservesRGB) {
  uint32_t w = 3, h = 3;
  std::vector<uint8_t> pixels(static_cast<size_t>(w) * h * 4);
  for (uint32_t i = 0; i < w * h; ++i) {
    size_t idx = static_cast<size_t>(i) * 4;
    pixels[idx + 0] = static_cast<uint8_t>(i * 10);
    pixels[idx + 1] = static_cast<uint8_t>(i * 20);
    pixels[idx + 2] = static_cast<uint8_t>(i * 30);
    pixels[idx + 3] = 255;
  }
  pixels[4 * 4 + 3] = 0;

  auto original_rgb = pixels;

  MorphologicalClose(pixels.data(), w, h, 4, 1);

  // RGB channels should be unchanged.
  for (uint32_t i = 0; i < w * h; ++i) {
    size_t idx = static_cast<size_t>(i) * 4;
    EXPECT_EQ(original_rgb[idx + 0], pixels[idx + 0])
        << "R changed at pixel " << i;
    EXPECT_EQ(original_rgb[idx + 1], pixels[idx + 1])
        << "G changed at pixel " << i;
    EXPECT_EQ(original_rgb[idx + 2], pixels[idx + 2])
        << "B changed at pixel " << i;
  }
}

TEST(SvgPreprocessorTest, MorphologicalCloseNoOpForRGB) {
  uint32_t w = 3, h = 3;
  std::vector<uint8_t> pixels(static_cast<size_t>(w) * h * 3, 128);
  auto original = pixels;

  MorphologicalClose(pixels.data(), w, h, 3, 1);

  EXPECT_EQ(original, pixels);
}

TEST(SvgPreprocessorTest, MorphologicalCloseRadiusZeroNoOp) {
  uint32_t w = 3, h = 3;
  std::vector<uint8_t> pixels(static_cast<size_t>(w) * h * 4);
  for (size_t i = 0; i < pixels.size(); ++i) pixels[i] = 128;
  pixels[4 * 4 + 3] = 0;

  auto original = pixels;

  MorphologicalClose(pixels.data(), w, h, 4, 0);

  // Radius 0 is a no-op.
  EXPECT_EQ(original, pixels);
}

// --- Full pipeline tests ---

TEST(SvgPreprocessorTest, PreprocessPixelsIntegration) {
  uint32_t w = 16, h = 16;
  auto pixels = MakeMultiColorImage(w, h, 4);

  PreprocessConfig config;
  config.max_colors = 8;
  config.alpha_threshold = 128;
  config.morphological_close = false;

  PreprocessResult result = PreprocessPixels(pixels.data(), w, h, 4, config);

  // Output should be RGBA.
  EXPECT_EQ(w, result.width);
  EXPECT_EQ(h, result.height);
  uint64_t expected_size = static_cast<uint64_t>(w) * h * 4;
  EXPECT_EQ(expected_size, result.pixel_buffer.size());

  // Colors should be reduced.
  EXPECT_LE(result.actual_colors, 8);
  EXPECT_GT(result.actual_colors, 0);

  // Alpha should be binary (0 or 255).
  auto* buf = reinterpret_cast<const uint8_t*>(result.pixel_buffer.data());
  for (uint64_t i = 0; i < static_cast<uint64_t>(w) * h; ++i) {
    uint8_t a = buf[i * 4 + 3];
    EXPECT_TRUE(a == 0 || a == 255)
        << "Non-binary alpha " << static_cast<int>(a) << " at pixel " << i;
  }
}

TEST(SvgPreprocessorTest, PreprocessPixelsRGBToRGBA) {
  uint32_t w = 4, h = 4;
  auto pixels = MakeSolidImage(w, h, 3, 100, 150, 200);

  PreprocessConfig config;
  config.max_colors = 32;

  PreprocessResult result = PreprocessPixels(pixels.data(), w, h, 3, config);

  // Output is RGBA.
  uint64_t expected_size = static_cast<uint64_t>(w) * h * 4;
  EXPECT_EQ(expected_size, result.pixel_buffer.size());

  // Check all pixels have alpha=255 (from RGB conversion).
  auto* buf = reinterpret_cast<const uint8_t*>(result.pixel_buffer.data());
  for (uint64_t i = 0; i < static_cast<uint64_t>(w) * h; ++i) {
    EXPECT_EQ(255, buf[i * 4 + 3]) << "Alpha not 255 at pixel " << i;
  }
}

TEST(SvgPreprocessorTest, PreprocessPixelsGrayscaleToRGBA) {
  uint32_t w = 4, h = 4;
  auto pixels = MakeSolidImage(w, h, 1, 128, 0, 0);

  PreprocessConfig config;
  config.max_colors = 32;

  PreprocessResult result = PreprocessPixels(pixels.data(), w, h, 1, config);

  uint64_t expected_size = static_cast<uint64_t>(w) * h * 4;
  EXPECT_EQ(expected_size, result.pixel_buffer.size());

  // Check that grayscale was expanded to RGBA correctly.
  auto* buf = reinterpret_cast<const uint8_t*>(result.pixel_buffer.data());
  for (uint64_t i = 0; i < static_cast<uint64_t>(w) * h; ++i) {
    size_t idx = static_cast<size_t>(i) * 4;
    // All channels should be the gray value (possibly quantized
    // slightly).
    EXPECT_NEAR(128, buf[idx + 0], 8);
    EXPECT_NEAR(128, buf[idx + 1], 8);
    EXPECT_NEAR(128, buf[idx + 2], 8);
    EXPECT_EQ(255, buf[idx + 3]);
  }
}

TEST(SvgPreprocessorTest, PreprocessPixelsWithMorphologicalClose) {
  // 5x5 RGBA: all opaque except center pixel.
  uint32_t w = 5, h = 5;
  std::vector<uint8_t> pixels(static_cast<size_t>(w) * h * 4);
  for (uint32_t i = 0; i < w * h; ++i) {
    size_t idx = static_cast<size_t>(i) * 4;
    pixels[idx + 0] = 128;
    pixels[idx + 1] = 128;
    pixels[idx + 2] = 128;
    pixels[idx + 3] = 255;
  }
  // Center pixel (2,2) = transparent.
  pixels[(2 * w + 2) * 4 + 3] = 0;

  PreprocessConfig config;
  config.max_colors = 32;
  config.alpha_threshold = 128;
  config.morphological_close = true;
  config.close_radius = 1;

  PreprocessResult result = PreprocessPixels(pixels.data(), w, h, 4, config);

  // After close, center pixel should be opaque.
  auto* buf = reinterpret_cast<const uint8_t*>(result.pixel_buffer.data());
  EXPECT_EQ(255, buf[(2 * w + 2) * 4 + 3]);
}

// --- Edge case tests ---

TEST(SvgPreprocessorTest, OneByOneImage) {
  uint8_t pixels[] = {42, 84, 126, 200};

  PreprocessConfig config;
  config.max_colors = 8;
  config.alpha_threshold = 128;

  PreprocessResult result = PreprocessPixels(pixels, 1, 1, 4, config);

  EXPECT_EQ(1u, result.width);
  EXPECT_EQ(1u, result.height);
  EXPECT_EQ(4u, result.pixel_buffer.size());

  // Alpha should be thresholded: 200 >= 128 -> 255.
  auto* buf = reinterpret_cast<const uint8_t*>(result.pixel_buffer.data());
  EXPECT_EQ(255, buf[3]);
  EXPECT_EQ(1, result.actual_colors);
}

TEST(SvgPreprocessorTest, NullPixelsReturnsEmpty) {
  PreprocessConfig config;
  PreprocessResult result = PreprocessPixels(nullptr, 10, 10, 4, config);
  EXPECT_EQ(0u, result.pixel_buffer.size());
  EXPECT_EQ(0, result.actual_colors);
}

TEST(SvgPreprocessorTest, ZeroDimensionsReturnsEmpty) {
  uint8_t pixels[4] = {1, 2, 3, 4};
  PreprocessConfig config;

  PreprocessResult result = PreprocessPixels(pixels, 0, 0, 4, config);
  EXPECT_EQ(0u, result.pixel_buffer.size());

  result = PreprocessPixels(pixels, 10, 0, 4, config);
  EXPECT_EQ(0u, result.pixel_buffer.size());

  result = PreprocessPixels(pixels, 0, 10, 4, config);
  EXPECT_EQ(0u, result.pixel_buffer.size());
}

TEST(SvgPreprocessorTest, InvalidBppReturnsEmpty) {
  uint8_t pixels[8] = {1, 2, 3, 4, 5, 6, 7, 8};
  PreprocessConfig config;

  PreprocessResult result = PreprocessPixels(pixels, 2, 1, 2, config);
  EXPECT_EQ(0u, result.pixel_buffer.size());

  result = PreprocessPixels(pixels, 2, 1, 5, config);
  EXPECT_EQ(0u, result.pixel_buffer.size());
}

TEST(SvgPreprocessorTest, QuantizeColorsZeroMaxColorsNoOp) {
  uint32_t w = 4, h = 4;
  auto pixels = MakeMultiColorImage(w, h, 4);
  auto original = pixels;

  QuantizeColors(pixels.data(), w, h, 4, 0);

  EXPECT_EQ(original, pixels);
}

TEST(SvgPreprocessorTest, QuantizeColorsNegativeMaxColorsNoOp) {
  uint32_t w = 4, h = 4;
  auto pixels = MakeMultiColorImage(w, h, 4);
  auto original = pixels;

  QuantizeColors(pixels.data(), w, h, 4, -5);

  EXPECT_EQ(original, pixels);
}

TEST(SvgPreprocessorTest, ThresholdAlphaZeroThreshold) {
  uint32_t w = 3, h = 1;
  uint8_t pixels[] = {
      0, 0, 0, 0,    // alpha 0
      0, 0, 0, 1,    // alpha 1
      0, 0, 0, 255,  // alpha 255
  };

  ThresholdAlpha(pixels, w, h, 4, 0);

  // All >= 0, so all should become 255.
  EXPECT_EQ(255, pixels[3]);
  EXPECT_EQ(255, pixels[7]);
  EXPECT_EQ(255, pixels[11]);
}

TEST(SvgPreprocessorTest, MorphologicalCloseFullyTransparent) {
  // All-transparent image: close should leave it transparent.
  uint32_t w = 5, h = 5;
  std::vector<uint8_t> pixels(static_cast<size_t>(w) * h * 4, 0);

  MorphologicalClose(pixels.data(), w, h, 4, 1);

  for (uint32_t i = 0; i < w * h; ++i) {
    EXPECT_EQ(0, pixels[i * 4 + 3])
        << "Pixel " << i << " should remain transparent";
  }
}

TEST(SvgPreprocessorTest, MorphologicalCloseFullyOpaque) {
  // All-opaque image: close should leave it opaque.
  uint32_t w = 5, h = 5;
  std::vector<uint8_t> pixels(static_cast<size_t>(w) * h * 4);
  for (uint32_t i = 0; i < w * h; ++i) {
    pixels[i * 4 + 0] = 128;
    pixels[i * 4 + 1] = 128;
    pixels[i * 4 + 2] = 128;
    pixels[i * 4 + 3] = 255;
  }

  MorphologicalClose(pixels.data(), w, h, 4, 1);

  for (uint32_t i = 0; i < w * h; ++i) {
    EXPECT_EQ(255, pixels[i * 4 + 3])
        << "Pixel " << i << " should remain opaque";
  }
}

}  // namespace
}  // namespace pagespeed
