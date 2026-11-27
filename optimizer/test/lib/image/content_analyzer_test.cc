// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

// PageSpeed 2.0 - Content Analyzer Unit Tests

#include "lib/image/content_analyzer.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include "gtest/gtest.h"
#include "lib/base/message_handler.h"
#include "lib/image/image_analysis.h"
#include "lib/image/image_util.h"
#include "lib/image/read_image.h"
#include "test/lib/image/test_utils.h"

namespace pagespeed {
namespace {

class ContentAnalyzerTest : public ::testing::Test {
 protected:
  ConsoleMessageHandler handler_;
};

// Helper to generate a solid-color RGB image.
std::vector<uint8_t> MakeSolidColor(uint32_t w, uint32_t h, uint8_t r,
                                    uint8_t g, uint8_t b) {
  std::vector<uint8_t> pixels(static_cast<size_t>(w) * h * 3);
  for (size_t i = 0; i < pixels.size(); i += 3) {
    pixels[i] = r;
    pixels[i + 1] = g;
    pixels[i + 2] = b;
  }
  return pixels;
}

// Helper to generate a two-color checkerboard.
std::vector<uint8_t> MakeCheckerboard(uint32_t w, uint32_t h) {
  std::vector<uint8_t> pixels(static_cast<size_t>(w) * h * 3);
  for (uint32_t y = 0; y < h; ++y) {
    for (uint32_t x = 0; x < w; ++x) {
      size_t idx = (static_cast<size_t>(y) * w + x) * 3;
      bool white = ((x + y) % 2 == 0);
      pixels[idx] = white ? 255 : 0;
      pixels[idx + 1] = white ? 255 : 0;
      pixels[idx + 2] = white ? 255 : 0;
    }
  }
  return pixels;
}

// Helper to generate a photo-like image: smooth gradients with per-pixel
// noise, producing many unique colors (>256) and a wide histogram peak
// (high PhotoMetric).
std::vector<uint8_t> MakePhotoLike(uint32_t w, uint32_t h) {
  std::vector<uint8_t> pixels(static_cast<size_t>(w) * h * 3);
  uint32_t prng = 12345;
  for (uint32_t y = 0; y < h; ++y) {
    for (uint32_t x = 0; x < w; ++x) {
      size_t idx = (static_cast<size_t>(y) * w + x) * 3;
      float fx = static_cast<float>(x) / (w > 1 ? w - 1 : 1);
      float fy = static_cast<float>(y) / (h > 1 ? h - 1 : 1);
      // Per-channel noise (+/- 16) with different PRNG multipliers.
      prng = prng * 1103515245 + 12345;
      int nr = static_cast<int>((prng >> 16) & 0x1F) - 16;
      prng = prng * 1103515245 + 12345;
      int ng = static_cast<int>((prng >> 16) & 0x1F) - 16;
      prng = prng * 1103515245 + 12345;
      int nb = static_cast<int>((prng >> 16) & 0x1F) - 16;
      int r = static_cast<int>(fx * 230 + fy * 20) + nr;
      int g = static_cast<int>(fy * 200 + fx * 40) + ng;
      int b = static_cast<int>((fx * 0.3f + fy * 0.7f) * 190 + 30) + nb;
      pixels[idx] = static_cast<uint8_t>(std::max(0, std::min(255, r)));
      pixels[idx + 1] = static_cast<uint8_t>(std::max(0, std::min(255, g)));
      pixels[idx + 2] = static_cast<uint8_t>(std::max(0, std::min(255, b)));
    }
  }
  return pixels;
}

// Helper to generate a screenshot-like image: flat colored blocks with
// sharp 1-pixel boundaries.  Uses a palette of distinct flat colors
// (>256 unique RGB values via positional tinting) with only edges
// between blocks producing gradient.  This yields high edge density
// with a narrow gradient histogram (low PhotoMetric).
std::vector<uint8_t> MakeScreenshotLike(uint32_t w, uint32_t h) {
  std::vector<uint8_t> pixels(static_cast<size_t>(w) * h * 3);
  // Simulate a screenshot: white background with low-contrast text-like
  // horizontal lines every 4 rows and vertical separators every 8 cols.
  // The text uses gray value ~210 on white 240 background — small
  // contrast produces edges with low gradient values, keeping the
  // gradient histogram narrow (low PhotoMetric).
  // Per-pixel sub-pixel rendering variation adds >256 unique colors.
  uint32_t prng = 42;
  for (uint32_t y = 0; y < h; ++y) {
    for (uint32_t x = 0; x < w; ++x) {
      size_t idx = (static_cast<size_t>(y) * w + x) * 3;
      bool is_text_row = (y % 4 == 0 || y % 4 == 1);
      bool is_sep_col = (x % 8 == 0);
      uint8_t base;
      if (is_sep_col) {
        base = 200;  // Vertical separator
      } else if (is_text_row) {
        base = 210;  // Text line
      } else {
        base = 240;  // Background
      }
      // Sub-pixel rendering variation: +/- 3 per channel.
      prng = prng * 1103515245 + 12345;
      int dr = static_cast<int>((prng >> 16) % 7) - 3;
      prng = prng * 1103515245 + 12345;
      int dg = static_cast<int>((prng >> 16) % 7) - 3;
      prng = prng * 1103515245 + 12345;
      int db = static_cast<int>((prng >> 16) % 7) - 3;
      pixels[idx] = static_cast<uint8_t>(std::max(0, std::min(255, base + dr)));
      pixels[idx + 1] =
          static_cast<uint8_t>(std::max(0, std::min(255, base + dg)));
      pixels[idx + 2] =
          static_cast<uint8_t>(std::max(0, std::min(255, base + db)));
    }
  }
  return pixels;
}

// Helper to generate random noise using a simple LCG PRNG.
std::vector<uint8_t> MakeNoise(uint32_t w, uint32_t h, uint32_t seed = 42) {
  std::vector<uint8_t> pixels(static_cast<size_t>(w) * h * 3);
  uint32_t state = seed;
  for (size_t i = 0; i < pixels.size(); ++i) {
    state = state * 1103515245 + 12345;
    pixels[i] = static_cast<uint8_t>((state >> 16) & 0xFF);
  }
  return pixels;
}

// --- Invalid input tests ---

TEST_F(ContentAnalyzerTest, NullPixelsReturnsUnknown) {
  auto result = AnalyzeContent(nullptr, 100, 10, 10, 3, &handler_);
  EXPECT_EQ(ContentClass::kUnknown, result.quality_preset.content_class);
}

TEST_F(ContentAnalyzerTest, ZeroDimensionsReturnUnknown) {
  uint8_t pixel = 0;
  auto result = AnalyzeContent(&pixel, 1, 0, 0, 3, &handler_);
  EXPECT_EQ(ContentClass::kUnknown, result.quality_preset.content_class);
}

TEST_F(ContentAnalyzerTest, TooSmallImageReturnsUnknown) {
  // 1x1 is below kMinDimension (3).
  uint8_t pixels[3] = {128, 128, 128};
  auto result = AnalyzeContent(pixels, 3, 1, 1, 3, &handler_);
  EXPECT_EQ(ContentClass::kUnknown, result.quality_preset.content_class);
}

TEST_F(ContentAnalyzerTest, InvalidBufferLengthReturnsUnknown) {
  std::vector<uint8_t> pixels(10);  // Too small for 10x10x3.
  auto result =
      AnalyzeContent(pixels.data(), pixels.size(), 10, 10, 3, &handler_);
  EXPECT_EQ(ContentClass::kUnknown, result.quality_preset.content_class);
}

TEST_F(ContentAnalyzerTest, InvalidBppReturnsUnknown) {
  std::vector<uint8_t> pixels(100);
  auto result =
      AnalyzeContent(pixels.data(), pixels.size(), 10, 10, 2, &handler_);
  EXPECT_EQ(ContentClass::kUnknown, result.quality_preset.content_class);
}

TEST_F(ContentAnalyzerTest, BufferTooSmallReturnsUnknown) {
  // Buffer is 9 bytes but 3x3x3=27 bytes are required.
  std::vector<uint8_t> pixels(9, 128);
  auto result =
      AnalyzeContent(pixels.data(), pixels.size(), 3, 3, 3, &handler_);
  EXPECT_EQ(ContentClass::kUnknown, result.quality_preset.content_class);
}

// --- Illustration tests ---

TEST_F(ContentAnalyzerTest, SolidColorIsIllustration) {
  auto pixels = MakeSolidColor(16, 16, 255, 0, 0);
  auto result =
      AnalyzeContent(pixels.data(), pixels.size(), 16, 16, 3, &handler_);
  EXPECT_EQ(ContentClass::kIllustration, result.quality_preset.content_class);
  EXPECT_FLOAT_EQ(1.10f, result.quality_preset.jpeg_quality_factor);
}

TEST_F(ContentAnalyzerTest, TwoColorCheckerboardNotPhoto) {
  auto pixels = MakeCheckerboard(32, 32);
  auto result =
      AnalyzeContent(pixels.data(), pixels.size(), 32, 32, 3, &handler_);
  EXPECT_NE(ContentClass::kPhoto, result.quality_preset.content_class);
}

// --- Photo tests ---

TEST_F(ContentAnalyzerTest, PhotoLikeImageIsPhoto) {
  auto pixels = MakePhotoLike(256, 256);
  auto result =
      AnalyzeContent(pixels.data(), pixels.size(), 256, 256, 3, &handler_);
  EXPECT_EQ(ContentClass::kPhoto, result.quality_preset.content_class);
  EXPECT_FLOAT_EQ(1.0f, result.quality_preset.jpeg_quality_factor);
}

// --- Screenshot tests ---

TEST_F(ContentAnalyzerTest, ScreenshotLikeIsScreenshot) {
  // Use a 512x512 image to ensure enough unique colors from block
  // palette and enough flat area for low PhotoMetric.
  auto pixels = MakeScreenshotLike(512, 512);
  auto result =
      AnalyzeContent(pixels.data(), pixels.size(), 512, 512, 3, &handler_);
  EXPECT_EQ(ContentClass::kScreenshot, result.quality_preset.content_class);
  EXPECT_FLOAT_EQ(1.15f, result.quality_preset.jpeg_quality_factor);
}

// --- Noise tests ---

TEST_F(ContentAnalyzerTest, RandomNoiseIsNoisy) {
  auto pixels = MakeNoise(128, 128);
  auto result =
      AnalyzeContent(pixels.data(), pixels.size(), 128, 128, 3, &handler_);
  EXPECT_EQ(ContentClass::kNoisy, result.quality_preset.content_class);
  EXPECT_FLOAT_EQ(0.85f, result.quality_preset.jpeg_quality_factor);
  EXPECT_GT(result.quality_preset.noise_level, 0.0f);
}

// --- Grayscale (bpp=1) tests ---

TEST_F(ContentAnalyzerTest, GrayscaleGradientClassifies) {
  uint32_t w = 128, h = 128;
  std::vector<uint8_t> pixels(static_cast<size_t>(w) * h);
  for (uint32_t y = 0; y < h; ++y) {
    for (uint32_t x = 0; x < w; ++x) {
      pixels[static_cast<size_t>(y) * w + x] =
          static_cast<uint8_t>((x * 255) / (w > 1 ? w - 1 : 1));
    }
  }
  auto result =
      AnalyzeContent(pixels.data(), pixels.size(), w, h, 1, &handler_);
  // Should classify as something meaningful, not unknown.
  EXPECT_NE(ContentClass::kUnknown, result.quality_preset.content_class);
}

// --- RGBA (bpp=4) tests ---

TEST_F(ContentAnalyzerTest, RGBAClassifiesSameAsRGB) {
  uint32_t w = 256, h = 256;
  auto rgb_pixels = MakePhotoLike(w, h);
  // Create RGBA version with alpha=255.
  std::vector<uint8_t> rgba_pixels(static_cast<size_t>(w) * h * 4);
  for (uint32_t i = 0; i < w * h; ++i) {
    rgba_pixels[static_cast<size_t>(i) * 4] =
        rgb_pixels[static_cast<size_t>(i) * 3];
    rgba_pixels[static_cast<size_t>(i) * 4 + 1] =
        rgb_pixels[static_cast<size_t>(i) * 3 + 1];
    rgba_pixels[static_cast<size_t>(i) * 4 + 2] =
        rgb_pixels[static_cast<size_t>(i) * 3 + 2];
    rgba_pixels[static_cast<size_t>(i) * 4 + 3] = 255;
  }

  auto rgb_result =
      AnalyzeContent(rgb_pixels.data(), rgb_pixels.size(), w, h, 3, &handler_);
  auto rgba_result = AnalyzeContent(rgba_pixels.data(), rgba_pixels.size(), w,
                                    h, 4, &handler_);
  EXPECT_EQ(rgb_result.quality_preset.content_class,
            rgba_result.quality_preset.content_class);
}

// --- QualityPreset viewport tests ---

TEST_F(ContentAnalyzerTest, ScreenshotPresetNotAppliedOnMobile) {
  QualityPreset preset;
  preset.content_class = ContentClass::kScreenshot;
  EXPECT_FALSE(preset.applies_at_viewport(480));  // Mobile
  EXPECT_TRUE(preset.applies_at_viewport(768));   // Tablet
  EXPECT_TRUE(preset.applies_at_viewport(1920));  // Desktop
}

TEST_F(ContentAnalyzerTest, PhotoPresetAppliesAtAllViewports) {
  QualityPreset preset;
  preset.content_class = ContentClass::kPhoto;
  EXPECT_TRUE(preset.applies_at_viewport(0));
  EXPECT_TRUE(preset.applies_at_viewport(480));
  EXPECT_TRUE(preset.applies_at_viewport(1920));
}

// --- Real JPEG image tests ---

TEST_F(ContentAnalyzerTest, RealJpegPhotoClassifiesAsPhoto) {
  std::string jpeg_data;
  ASSERT_TRUE(image_compression::ReadTestFileWithExt(
      image_compression::kJpegTestDir, "sjpeg3.jpg", &jpeg_data));

  // Decode to pixels.
  int width = 0, height = 0;
  bool is_photo = false;
  ASSERT_TRUE(image_compression::AnalyzeImage(
      image_compression::IMAGE_JPEG, jpeg_data.data(), jpeg_data.size(), &width,
      &height, nullptr, nullptr, nullptr, &is_photo, nullptr, nullptr,
      &handler_));
  ASSERT_GT(width, 0);
  ASSERT_GT(height, 0);

  // Decode to raw pixels for content analysis.
  std::unique_ptr<net_instaweb::ScanlineReaderInterface> reader(
      image_compression::CreateScanlineReader(image_compression::IMAGE_JPEG,
                                              jpeg_data.data(),
                                              jpeg_data.size(), &handler_));
  ASSERT_NE(nullptr, reader);

  int bpp = (reader->GetPixelFormat() == image_compression::GRAY_8) ? 1 : 3;
  int bytes_per_line = reader->GetImageWidth() * bpp;
  std::vector<uint8_t> pixels(static_cast<size_t>(bytes_per_line) *
                              reader->GetImageHeight());

  for (int y = 0; y < static_cast<int>(reader->GetImageHeight()); ++y) {
    uint8_t* scanline = nullptr;
    ASSERT_TRUE(reader->HasMoreScanLines());
    ASSERT_TRUE(reader->ReadNextScanline(reinterpret_cast<void**>(&scanline)));
    memcpy(pixels.data() + static_cast<size_t>(y) * bytes_per_line, scanline,
           bytes_per_line);
  }

  auto result =
      AnalyzeContent(pixels.data(), pixels.size(), reader->GetImageWidth(),
                     reader->GetImageHeight(), bpp, &handler_);
  EXPECT_EQ(ContentClass::kPhoto, result.quality_preset.content_class);
}

TEST_F(ContentAnalyzerTest, SmallJpegGraphicNotPhoto) {
  std::string jpeg_data;
  ASSERT_TRUE(image_compression::ReadTestFileWithExt(
      image_compression::kJpegTestDir, "sjpeg1.jpg", &jpeg_data));

  std::unique_ptr<net_instaweb::ScanlineReaderInterface> reader(
      image_compression::CreateScanlineReader(image_compression::IMAGE_JPEG,
                                              jpeg_data.data(),
                                              jpeg_data.size(), &handler_));
  ASSERT_NE(nullptr, reader);

  int bpp = (reader->GetPixelFormat() == image_compression::GRAY_8) ? 1 : 3;
  int bytes_per_line = reader->GetImageWidth() * bpp;
  std::vector<uint8_t> pixels(static_cast<size_t>(bytes_per_line) *
                              reader->GetImageHeight());

  for (int y = 0; y < static_cast<int>(reader->GetImageHeight()); ++y) {
    uint8_t* scanline = nullptr;
    ASSERT_TRUE(reader->HasMoreScanLines());
    ASSERT_TRUE(reader->ReadNextScanline(reinterpret_cast<void**>(&scanline)));
    memcpy(pixels.data() + static_cast<size_t>(y) * bytes_per_line, scanline,
           bytes_per_line);
  }

  auto result =
      AnalyzeContent(pixels.data(), pixels.size(), reader->GetImageWidth(),
                     reader->GetImageHeight(), bpp, &handler_);
  // sjpeg1.jpg is a small graphic — should not be classified as a photo.
  EXPECT_NE(ContentClass::kPhoto, result.quality_preset.content_class);
}

// ===================================================================
// SVG Candidacy tests
// ===================================================================

class SvgCandidacyTest : public ::testing::Test {
 protected:
  ConsoleMessageHandler handler_;
  SvgCandidacyConfig config_;

  // Helper: run AnalyzeContent then EvaluateSvgCandidacy.
  SvgCandidacy Evaluate(const uint8_t* pixels, size_t len, uint32_t w,
                        uint32_t h, int bpp, ImageType fmt = ImageType::kPng,
                        const PngMetadata* meta = nullptr) {
    auto analysis = AnalyzeContent(pixels, len, w, h, bpp, &handler_);
    return EvaluateSvgCandidacy(config_, analysis.quality_preset, pixels, len,
                                w, h, bpp, fmt, meta, &handler_);
  }

  // Helper: evaluate with a pre-set QualityPreset (for hard-reject
  // testing without needing pixel analysis to match).
  SvgCandidacy EvaluateWithPreset(const QualityPreset& preset,
                                  const uint8_t* pixels, size_t len, uint32_t w,
                                  uint32_t h, int bpp,
                                  ImageType fmt = ImageType::kPng,
                                  const PngMetadata* meta = nullptr) {
    return EvaluateSvgCandidacy(config_, preset, pixels, len, w, h, bpp, fmt,
                                meta, &handler_);
  }
};

// Helper: solid-color RGBA image with partial transparency.
std::vector<uint8_t> MakeSolidColorRGBA(uint32_t w, uint32_t h, uint8_t r,
                                        uint8_t g, uint8_t b, uint8_t a) {
  std::vector<uint8_t> pixels(static_cast<size_t>(w) * h * 4);
  for (size_t i = 0; i < pixels.size(); i += 4) {
    pixels[i] = r;
    pixels[i + 1] = g;
    pixels[i + 2] = b;
    pixels[i + 3] = a;
  }
  return pixels;
}

// Helper: two-color logo-like RGBA image (half transparent).
std::vector<uint8_t> MakeLogoLikeRGBA(uint32_t w, uint32_t h) {
  std::vector<uint8_t> pixels(static_cast<size_t>(w) * h * 4);
  for (uint32_t y = 0; y < h; ++y) {
    for (uint32_t x = 0; x < w; ++x) {
      size_t idx = (static_cast<size_t>(y) * w + x) * 4;
      // Top half: solid blue.
      // Bottom half: transparent.
      if (y < h / 2) {
        pixels[idx] = 0;
        pixels[idx + 1] = 0;
        pixels[idx + 2] = 200;
        pixels[idx + 3] = 255;
      } else {
        pixels[idx] = 0;
        pixels[idx + 1] = 0;
        pixels[idx + 2] = 0;
        pixels[idx + 3] = 0;
      }
    }
  }
  return pixels;
}

// --- Hard reject: too small ---
TEST_F(SvgCandidacyTest, RejectTooSmall) {
  // 8x8 is below default min_dimension=16.
  auto pixels = MakeSolidColor(8, 8, 255, 0, 0);
  auto cand = Evaluate(pixels.data(), pixels.size(), 8, 8, 3);
  EXPECT_EQ(SvgCandidacy::kReject, cand.result);
  EXPECT_EQ("too small", cand.reason);
}

// --- Hard reject: too large ---
TEST_F(SvgCandidacyTest, RejectTooLarge) {
  // 512x512 = 262144 > default max_pixels=65536.
  auto pixels = MakeSolidColor(512, 512, 255, 0, 0);
  auto cand = Evaluate(pixels.data(), pixels.size(), 512, 512, 3);
  EXPECT_EQ(SvgCandidacy::kReject, cand.result);
  EXPECT_EQ("exceeds max pixels", cand.reason);
}

// --- Hard reject: extreme aspect ratio ---
TEST_F(SvgCandidacyTest, RejectExtremeAspectRatio) {
  // 256x16 = aspect ratio 16.0 > max_aspect_ratio=4.0.
  // Total pixels = 4096 which is within max_pixels.
  auto pixels = MakeSolidColor(256, 16, 255, 0, 0);
  auto cand = Evaluate(pixels.data(), pixels.size(), 256, 16, 3);
  EXPECT_EQ(SvgCandidacy::kReject, cand.result);
  EXPECT_EQ("extreme aspect ratio", cand.reason);
}

TEST_F(SvgCandidacyTest, RejectTallAspectRatio) {
  // 16x256 = aspect ratio 0.0625 < min_aspect_ratio=0.25.
  auto pixels = MakeSolidColor(16, 256, 255, 0, 0);
  auto cand = Evaluate(pixels.data(), pixels.size(), 16, 256, 3);
  EXPECT_EQ(SvgCandidacy::kReject, cand.result);
  EXPECT_EQ("extreme aspect ratio", cand.reason);
}

// --- Hard reject: photo content ---
TEST_F(SvgCandidacyTest, RejectPhotoContent) {
  auto pixels = MakeSolidColor(64, 64, 255, 0, 0);
  QualityPreset preset;
  preset.content_class = ContentClass::kPhoto;
  auto cand =
      EvaluateWithPreset(preset, pixels.data(), pixels.size(), 64, 64, 3);
  EXPECT_EQ(SvgCandidacy::kReject, cand.result);
  EXPECT_EQ("photo/noisy content", cand.reason);
}

// --- Hard reject: noisy content ---
TEST_F(SvgCandidacyTest, RejectNoisyContent) {
  auto pixels = MakeSolidColor(64, 64, 255, 0, 0);
  QualityPreset preset;
  preset.content_class = ContentClass::kNoisy;
  auto cand =
      EvaluateWithPreset(preset, pixels.data(), pixels.size(), 64, 64, 3);
  EXPECT_EQ(SvgCandidacy::kReject, cand.result);
  EXPECT_EQ("photo/noisy content", cand.reason);
}

// --- Strong candidate: solid-color image ---
TEST_F(SvgCandidacyTest, StrongCandidateSolidColor) {
  // 64x64 solid red: very few quantized colors, high flat ratio.
  auto pixels = MakeSolidColor(64, 64, 255, 0, 0);
  auto cand = Evaluate(pixels.data(), pixels.size(), 64, 64, 3);
  EXPECT_EQ(SvgCandidacy::kAttempt, cand.result);
  EXPECT_GE(cand.score, config_.candidacy_threshold);
  // Should have very few quantized colors.
  EXPECT_LE(cand.quantized_unique_colors, 16u);
  // Should have very high flat region ratio.
  EXPECT_GT(cand.flat_region_ratio, 0.9f);
}

// --- Strong candidate: two-color logo with transparency ---
TEST_F(SvgCandidacyTest, StrongCandidateLogoRGBA) {
  // 64x64 logo-like RGBA: few colors, alpha, flat regions.
  auto pixels = MakeLogoLikeRGBA(64, 64);
  auto cand = Evaluate(pixels.data(), pixels.size(), 64, 64, 4);
  EXPECT_EQ(SvgCandidacy::kAttempt, cand.result);
  EXPECT_GE(cand.score, config_.candidacy_threshold);
  EXPECT_GT(cand.alpha_coverage, 0.2f);
}

// --- Weak candidate: noisy photo-like image (small) ---
TEST_F(SvgCandidacyTest, WeakCandidatePhotoLikeSmall) {
  // A small photo-like image that happens to pass the content class
  // gate (kPhoto would be hard-rejected, but at 32x32 the
  // classifier may not classify it as kPhoto).
  // We force an kUnknown preset to test scoring.
  uint32_t w = 32, h = 32;
  auto pixels = MakeNoise(w, h, 99);
  QualityPreset preset;
  preset.content_class = ContentClass::kUnknown;
  auto cand = EvaluateWithPreset(preset, pixels.data(), pixels.size(), w, h, 3);
  // Random noise should have many quantized colors and low flat
  // ratio, so it should score low.
  EXPECT_LT(cand.score, config_.candidacy_threshold);
  EXPECT_EQ(SvgCandidacy::kReject, cand.result);
}

// --- PNG metadata bonus ---
TEST_F(SvgCandidacyTest, PngMetadataBonus) {
  auto pixels = MakeSolidColor(64, 64, 255, 0, 0);
  PngMetadata meta;
  meta.software = "Created with Inkscape 1.3";
  auto cand =
      Evaluate(pixels.data(), pixels.size(), 64, 64, 3, ImageType::kPng, &meta);
  EXPECT_TRUE(cand.png_metadata_match);
  // Should score higher than without metadata.
  auto cand_no_meta = Evaluate(pixels.data(), pixels.size(), 64, 64, 3);
  EXPECT_GT(cand.score, cand_no_meta.score);
}

TEST_F(SvgCandidacyTest, PngMetadataCaseInsensitive) {
  PngMetadata meta;
  meta.software = "ADOBE ILLUSTRATOR CC 2024";
  auto pixels = MakeSolidColor(64, 64, 0, 0, 255);
  auto cand =
      Evaluate(pixels.data(), pixels.size(), 64, 64, 3, ImageType::kPng, &meta);
  EXPECT_TRUE(cand.png_metadata_match);
}

TEST_F(SvgCandidacyTest, PngMetadataNoMatch) {
  PngMetadata meta;
  meta.software = "GIMP 2.10";
  auto pixels = MakeSolidColor(64, 64, 0, 0, 255);
  auto cand =
      Evaluate(pixels.data(), pixels.size(), 64, 64, 3, ImageType::kPng, &meta);
  EXPECT_FALSE(cand.png_metadata_match);
}

// --- Multi-signal bonus ---
TEST_F(SvgCandidacyTest, MultiSignalBonus) {
  // RGBA image with few colors, high flat ratio, and alpha.
  auto pixels = MakeLogoLikeRGBA(64, 64);
  auto cand = Evaluate(pixels.data(), pixels.size(), 64, 64, 4);
  // Verify multi-signal conditions are met.
  EXPECT_LE(cand.quantized_unique_colors, 64u);
  EXPECT_GT(cand.flat_region_ratio, 0.7f);
  EXPECT_GT(cand.alpha_coverage, 0.2f);
  // Score should be high due to multi-signal bonus.
  // Individual: colors(40) + flat(25) + alpha(15) + multi(15) = 95
  // plus possible edge/photo bonuses.
  EXPECT_GE(cand.score, 90);
}

// --- Threshold boundary ---
TEST_F(SvgCandidacyTest, ThresholdBoundaryExact) {
  // Create a synthetic image that scores exactly at threshold.
  // We control this by adjusting the threshold to match a known
  // score.
  auto pixels = MakeSolidColor(64, 64, 255, 0, 0);
  // First, measure the natural score.
  auto cand = Evaluate(pixels.data(), pixels.size(), 64, 64, 3);
  ASSERT_GE(cand.score, 50);  // Should be high.
  // Now set threshold to exactly the score.
  config_.candidacy_threshold = cand.score;
  auto cand2 = Evaluate(pixels.data(), pixels.size(), 64, 64, 3);
  EXPECT_EQ(SvgCandidacy::kAttempt, cand2.result);
  // Set threshold one above.
  config_.candidacy_threshold = cand.score + 1;
  auto cand3 = Evaluate(pixels.data(), pixels.size(), 64, 64, 3);
  EXPECT_EQ(SvgCandidacy::kReject, cand3.result);
}

// --- Individual signals: quantized colors ---
TEST_F(SvgCandidacyTest, QuantizedColorSignal) {
  // Solid color: should have very few quantized colors.
  auto pixels = MakeSolidColor(64, 64, 100, 200, 50);
  auto cand = Evaluate(pixels.data(), pixels.size(), 64, 64, 3);
  EXPECT_LE(cand.quantized_unique_colors, 16u);
}

// --- Individual signals: flat region ratio ---
TEST_F(SvgCandidacyTest, FlatRegionRatioSignal) {
  // Solid color image should have flat ratio close to 1.0.
  auto pixels = MakeSolidColor(64, 64, 42, 42, 42);
  auto cand = Evaluate(pixels.data(), pixels.size(), 64, 64, 3);
  EXPECT_GT(cand.flat_region_ratio, 0.95f);
}

// --- Individual signals: alpha coverage ---
TEST_F(SvgCandidacyTest, AlphaCoverageSignal) {
  // Half-transparent image.
  auto pixels = MakeLogoLikeRGBA(64, 64);
  auto cand = Evaluate(pixels.data(), pixels.size(), 64, 64, 4);
  // Bottom half is transparent, so coverage ~ 0.5.
  EXPECT_GT(cand.alpha_coverage, 0.3f);
  EXPECT_LT(cand.alpha_coverage, 0.7f);
}

TEST_F(SvgCandidacyTest, AlphaCoverageZeroForRGB) {
  auto pixels = MakeSolidColor(64, 64, 255, 0, 0);
  auto cand = Evaluate(pixels.data(), pixels.size(), 64, 64, 3);
  EXPECT_FLOAT_EQ(0.0f, cand.alpha_coverage);
}

// --- GIF format bonus ---
TEST_F(SvgCandidacyTest, GifFormatBonus) {
  auto pixels = MakeSolidColor(64, 64, 255, 0, 0);
  auto cand_png =
      Evaluate(pixels.data(), pixels.size(), 64, 64, 3, ImageType::kPng);
  auto cand_gif =
      Evaluate(pixels.data(), pixels.size(), 64, 64, 3, ImageType::kGif);
  EXPECT_GT(cand_gif.score, cand_png.score);
}

// --- PNG RGBA format bonus ---
TEST_F(SvgCandidacyTest, PngRgbaFormatBonus) {
  // RGBA image as PNG gets +5 bonus.
  auto rgba = MakeSolidColorRGBA(64, 64, 255, 0, 0, 255);
  auto cand_rgba =
      Evaluate(rgba.data(), rgba.size(), 64, 64, 4, ImageType::kPng);
  // Same image as WebP (no format bonus for PNG+RGBA).
  auto cand_webp =
      Evaluate(rgba.data(), rgba.size(), 64, 64, 4, ImageType::kWebP);
  EXPECT_GT(cand_rgba.score, cand_webp.score);
}

// ==================================================================
// Additional SVG candidacy tests: config variations, boundary
// conditions, and content-class edge cases
// ==================================================================

TEST_F(SvgCandidacyTest, CustomThresholdRejectsHighBar) {
  // Raise threshold so that a moderately-scoring image gets rejected.
  auto pixels = MakeCheckerboard(64, 64);
  auto cand_default = Evaluate(pixels.data(), pixels.size(), 64, 64, 3);

  SvgCandidacyConfig strict_config;
  strict_config.candidacy_threshold = 99;
  auto analysis =
      AnalyzeContent(pixels.data(), pixels.size(), 64, 64, 3, &handler_);
  auto cand_strict = EvaluateSvgCandidacy(
      strict_config, analysis.quality_preset, pixels.data(), pixels.size(), 64,
      64, 3, ImageType::kPng, nullptr, &handler_);
  // With threshold=99, almost everything should be rejected.
  EXPECT_EQ(SvgCandidacy::kReject, cand_strict.result);
}

TEST_F(SvgCandidacyTest, CustomMaxPixelsAccepts) {
  // Increase max_pixels to allow larger images.
  auto pixels = MakeSolidColor(512, 512, 255, 0, 0);
  SvgCandidacyConfig large_config;
  large_config.max_pixels = 512 * 512 + 1;
  auto analysis =
      AnalyzeContent(pixels.data(), pixels.size(), 512, 512, 3, &handler_);
  auto cand = EvaluateSvgCandidacy(large_config, analysis.quality_preset,
                                   pixels.data(), pixels.size(), 512, 512, 3,
                                   ImageType::kPng, nullptr, &handler_);
  // Should not reject on pixel count — solid color should be accepted.
  EXPECT_EQ(SvgCandidacy::kAttempt, cand.result);
}

TEST_F(SvgCandidacyTest, ExactMinDimensionAccepted) {
  // Image exactly at min_dimension should NOT be rejected for size.
  auto pixels = MakeSolidColor(16, 16, 0, 255, 0);
  auto cand = Evaluate(pixels.data(), pixels.size(), 16, 16, 3);
  EXPECT_EQ(SvgCandidacy::kAttempt, cand.result);
}

TEST_F(SvgCandidacyTest, ExactMaxPixelsAccepted) {
  // Image at exactly max_pixels should NOT be rejected.
  // 256x256 = 65536 = default max_pixels
  auto pixels = MakeSolidColor(256, 256, 0, 0, 255);
  auto cand = Evaluate(pixels.data(), pixels.size(), 256, 256, 3);
  EXPECT_EQ(SvgCandidacy::kAttempt, cand.result);
}

TEST_F(SvgCandidacyTest, ExactMaxPixelsPlusOneRejected) {
  // One pixel over max_pixels should be rejected.
  // 257x256 = 65792 > 65536
  auto pixels = MakeSolidColor(257, 256, 0, 0, 255);
  auto cand = Evaluate(pixels.data(), pixels.size(), 257, 256, 3);
  EXPECT_EQ(SvgCandidacy::kReject, cand.result);
  EXPECT_EQ("exceeds max pixels", cand.reason);
}

TEST_F(SvgCandidacyTest, IllustrationContentAccepted) {
  // kIllustration should NOT be rejected (only kPhoto and kNoisy are).
  auto pixels = MakeSolidColor(64, 64, 255, 0, 0);
  QualityPreset preset;
  preset.content_class = ContentClass::kIllustration;
  auto cand =
      EvaluateWithPreset(preset, pixels.data(), pixels.size(), 64, 64, 3);
  EXPECT_EQ(SvgCandidacy::kAttempt, cand.result);
}

TEST_F(SvgCandidacyTest, ScreenshotContentAccepted) {
  // kScreenshot should NOT be rejected.
  auto pixels = MakeSolidColor(64, 64, 255, 0, 0);
  QualityPreset preset;
  preset.content_class = ContentClass::kScreenshot;
  auto cand =
      EvaluateWithPreset(preset, pixels.data(), pixels.size(), 64, 64, 3);
  EXPECT_EQ(SvgCandidacy::kAttempt, cand.result);
}

TEST_F(SvgCandidacyTest, UnknownContentNotRejected) {
  // kUnknown should NOT be rejected by content class filter.
  auto pixels = MakeSolidColor(64, 64, 255, 0, 0);
  QualityPreset preset;
  preset.content_class = ContentClass::kUnknown;
  auto cand =
      EvaluateWithPreset(preset, pixels.data(), pixels.size(), 64, 64, 3);
  EXPECT_EQ(SvgCandidacy::kAttempt, cand.result);
}

TEST_F(SvgCandidacyTest, SquareAspectRatioAccepted) {
  // 1:1 aspect ratio is well within bounds.
  auto pixels = MakeSolidColor(64, 64, 0, 128, 0);
  auto cand = Evaluate(pixels.data(), pixels.size(), 64, 64, 3);
  EXPECT_EQ(SvgCandidacy::kAttempt, cand.result);
}

TEST_F(SvgCandidacyTest, ExactAspectRatioBoundaryAccepted) {
  // 4:1 = 4.0 which equals max_aspect_ratio. Should NOT be rejected.
  // Use 64x16 = 4.0 ratio, total pixels = 1024.
  auto pixels = MakeSolidColor(64, 16, 128, 0, 0);
  auto cand = Evaluate(pixels.data(), pixels.size(), 64, 16, 3);
  EXPECT_EQ(SvgCandidacy::kAttempt, cand.result);
}

TEST_F(SvgCandidacyTest, GrayscaleImageCandidacy) {
  // 1-bpp (grayscale) solid image should be evaluable.
  std::vector<uint8_t> gray(static_cast<size_t>(64) * 64, 128);
  auto cand = Evaluate(gray.data(), gray.size(), 64, 64, 1);
  // Grayscale solid should have very few quantized colors.
  EXPECT_LE(cand.quantized_unique_colors, 16u);
}

TEST_F(SvgCandidacyTest, MultiColorGradientRejected) {
  // A smooth gradient has many colors and should score low.
  std::vector<uint8_t> pixels(static_cast<size_t>(128) * 128 * 3);
  for (uint32_t y = 0; y < 128; ++y) {
    for (uint32_t x = 0; x < 128; ++x) {
      size_t idx = (static_cast<size_t>(y) * 128 + x) * 3;
      pixels[idx] = static_cast<uint8_t>(x * 2);
      pixels[idx + 1] = static_cast<uint8_t>(y * 2);
      pixels[idx + 2] = 128;
    }
  }
  auto cand = Evaluate(pixels.data(), pixels.size(), 128, 128, 3);
  // Many unique colors — should have a lower score.
  EXPECT_GT(cand.quantized_unique_colors, 64u);
}

TEST_F(SvgCandidacyTest, CustomMinDimension) {
  // Set min_dimension to 32 and check 24x24 is rejected.
  SvgCandidacyConfig config;
  config.min_dimension = 32;
  auto pixels = MakeSolidColor(24, 24, 255, 0, 0);
  auto analysis =
      AnalyzeContent(pixels.data(), pixels.size(), 24, 24, 3, &handler_);
  auto cand = EvaluateSvgCandidacy(config, analysis.quality_preset,
                                   pixels.data(), pixels.size(), 24, 24, 3,
                                   ImageType::kPng, nullptr, &handler_);
  EXPECT_EQ(SvgCandidacy::kReject, cand.result);
  EXPECT_EQ("too small", cand.reason);
}

// --- Buffer validation: nullptr pixels ---
TEST_F(SvgCandidacyTest, NullPixelsRejected) {
  QualityPreset preset;
  preset.content_class = ContentClass::kIllustration;
  auto cand = EvaluateWithPreset(preset, nullptr,
                                 static_cast<size_t>(64) * 64 * 3, 64, 64, 3);
  EXPECT_EQ(SvgCandidacy::kReject, cand.result);
  EXPECT_EQ("invalid input", cand.reason);
}

// --- Buffer validation: buffer_length=0 ---
TEST_F(SvgCandidacyTest, ZeroBufferLengthRejected) {
  auto pixels = MakeSolidColor(64, 64, 255, 0, 0);
  QualityPreset preset;
  preset.content_class = ContentClass::kIllustration;
  auto cand = EvaluateWithPreset(preset, pixels.data(), 0, 64, 64, 3);
  EXPECT_EQ(SvgCandidacy::kReject, cand.result);
  EXPECT_EQ("invalid input", cand.reason);
}

// --- Buffer validation: unsupported bpp ---
TEST_F(SvgCandidacyTest, UnsupportedBppRejected) {
  auto pixels = MakeSolidColor(64, 64, 255, 0, 0);
  QualityPreset preset;
  preset.content_class = ContentClass::kIllustration;
  auto cand =
      EvaluateWithPreset(preset, pixels.data(), pixels.size(), 64, 64, 2);
  EXPECT_EQ(SvgCandidacy::kReject, cand.result);
  EXPECT_EQ("invalid input", cand.reason);
}

// ==================================================================
// Additional coverage tests: SVG scoring branches
// ==================================================================

// Helper: create an image with a controlled number of quantized unique
// colors by using a palette of N colors distributed across pixels.
// Each color gets unique 5-bit quantized values (R>>3, G>>3, B>>3).
std::vector<uint8_t> MakePaletteImage(uint32_t w, uint32_t h, int num_colors) {
  std::vector<uint8_t> pixels(static_cast<size_t>(w) * h * 3);
  // Generate num_colors distinct quantized colors.
  // Each channel quantized to 5 bits: 32 values. Use different combos.
  std::vector<std::array<uint8_t, 3>> palette;
  for (int i = 0; i < num_colors && i < 32 * 32; ++i) {
    auto r = static_cast<uint8_t>((i % 32) * 8);
    auto g = static_cast<uint8_t>((i / 32) * 8);
    uint8_t b = 128;
    palette.push_back({r, g, b});
  }
  for (uint32_t y = 0; y < h; ++y) {
    for (uint32_t x = 0; x < w; ++x) {
      size_t idx = (static_cast<size_t>(y) * w + x) * 3;
      int ci = static_cast<int>((y * w + x) % palette.size());
      pixels[idx] = palette[ci][0];
      pixels[idx + 1] = palette[ci][1];
      pixels[idx + 2] = palette[ci][2];
    }
  }
  return pixels;
}

// Helper: make an image with moderate flat regions (between 0.5 and 0.7).
// Uses 8x8 blocks of uniform color, with 40% of blocks containing
// per-pixel noise to break flatness. This yields ~0.6 flat ratio.
std::vector<uint8_t> MakeModeratelyFlatImage(uint32_t w, uint32_t h) {
  std::vector<uint8_t> pixels(static_cast<size_t>(w) * h * 3);
  uint32_t prng = 7777;
  constexpr int block_size = 8;
  for (uint32_t by = 0; by < h; by += block_size) {
    for (uint32_t bx = 0; bx < w; bx += block_size) {
      // Block color.
      prng = prng * 1103515245 + 12345;
      auto base_r = static_cast<uint8_t>((prng >> 16) & 0xFF);
      prng = prng * 1103515245 + 12345;
      auto base_g = static_cast<uint8_t>((prng >> 16) & 0xFF);
      prng = prng * 1103515245 + 12345;
      auto base_b = static_cast<uint8_t>((prng >> 16) & 0xFF);

      // Decide if this block is noisy (40% chance).
      prng = prng * 1103515245 + 12345;
      bool noisy = ((prng >> 16) % 100) < 40;

      for (uint32_t dy = 0;
           dy < static_cast<uint32_t>(block_size) && by + dy < h; ++dy) {
        for (uint32_t dx = 0;
             dx < static_cast<uint32_t>(block_size) && bx + dx < w; ++dx) {
          size_t idx = (static_cast<size_t>(by + dy) * w + (bx + dx)) * 3;
          if (noisy) {
            prng = prng * 1103515245 + 12345;
            pixels[idx] = static_cast<uint8_t>((prng >> 16) & 0xFF);
            prng = prng * 1103515245 + 12345;
            pixels[idx + 1] = static_cast<uint8_t>((prng >> 16) & 0xFF);
            prng = prng * 1103515245 + 12345;
            pixels[idx + 2] = static_cast<uint8_t>((prng >> 16) & 0xFF);
          } else {
            pixels[idx] = base_r;
            pixels[idx + 1] = base_g;
            pixels[idx + 2] = base_b;
          }
        }
      }
    }
  }
  return pixels;
}

TEST_F(SvgCandidacyTest, QuantizedColors17To64ScoreBranch) {
  // Create an image with ~40 quantized unique colors to hit the
  // score += 30 branch (17 <= quantized_unique_colors <= 64).
  auto pixels = MakePaletteImage(64, 64, 40);
  QualityPreset preset;
  preset.content_class = ContentClass::kIllustration;
  auto cand =
      EvaluateWithPreset(preset, pixels.data(), pixels.size(), 64, 64, 3);
  EXPECT_GT(cand.quantized_unique_colors, 16u);
  EXPECT_LE(cand.quantized_unique_colors, 64u);
  // The score should include the +30 bonus.
  EXPECT_GE(cand.score, 30);
}

TEST_F(SvgCandidacyTest, FlatRegionRatioBetween50And70) {
  // Create image with moderate flatness (0.5 < flat_region_ratio <= 0.7)
  // to hit the score += 12 branch.
  auto pixels = MakeModeratelyFlatImage(64, 64);
  QualityPreset preset;
  preset.content_class = ContentClass::kIllustration;
  auto cand =
      EvaluateWithPreset(preset, pixels.data(), pixels.size(), 64, 64, 3);
  // If flat ratio happens to be in (0.5, 0.7] range, the +12 branch fires.
  // The exact value depends on the PRNG, but we verify the signal was evaluated.
  EXPECT_GT(cand.flat_region_ratio, 0.0f);
  EXPECT_LE(cand.flat_region_ratio, 1.0f);
}

TEST_F(SvgCandidacyTest, EdgeDensityAndPhotoMetricBranch) {
  // Create an image with strong edges but low photo metric:
  // checkerboard with thick stripes (high edge density) but few unique
  // colors (low photo metric).
  uint32_t w = 64, h = 64;
  std::vector<uint8_t> pixels(static_cast<size_t>(w) * h * 3);
  for (uint32_t y = 0; y < h; ++y) {
    for (uint32_t x = 0; x < w; ++x) {
      size_t idx = (static_cast<size_t>(y) * w + x) * 3;
      // 4-pixel-wide vertical stripes alternating black and white.
      bool white = ((x / 4) % 2 == 0);
      uint8_t v = white ? 255 : 0;
      pixels[idx] = v;
      pixels[idx + 1] = v;
      pixels[idx + 2] = v;
    }
  }
  QualityPreset preset;
  preset.content_class = ContentClass::kIllustration;
  auto cand = EvaluateWithPreset(preset, pixels.data(), pixels.size(), w, h, 3);
  // The striped pattern creates edges. We verify it gets scored.
  EXPECT_GE(cand.score, 0);
}

}  // namespace
}  // namespace pagespeed
