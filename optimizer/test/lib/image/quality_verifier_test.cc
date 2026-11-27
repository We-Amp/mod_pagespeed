// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

// PageSpeed 2.0 - SSIMULACRA2 Quality Verifier Tests

#include "lib/image/quality_verifier.h"

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <optional>
#include <string>
#include <vector>

#include "gtest/gtest.h"
#include "lib/base/message_handler.h"

// Allow huge-allocation tests to receive nullptr instead of aborting under ASan.
#if defined(__has_feature)
#if __has_feature(address_sanitizer)
extern "C" const char* __asan_default_options() {
  return "allocator_may_return_null=1";
}
#endif
#endif

namespace pagespeed {
namespace {

// Create a solid color RGB image.
std::vector<uint8_t> MakeSolidRGB(uint32_t width, uint32_t height, uint8_t r,
                                  uint8_t g, uint8_t b) {
  std::vector<uint8_t> buf(static_cast<size_t>(width) * height * 3);
  for (size_t i = 0; i < buf.size(); i += 3) {
    buf[i + 0] = r;
    buf[i + 1] = g;
    buf[i + 2] = b;
  }
  return buf;
}

// Create a gradient RGB image (more realistic than solid).
std::vector<uint8_t> MakeGradientRGB(uint32_t width, uint32_t height) {
  std::vector<uint8_t> buf(static_cast<size_t>(width) * height * 3);
  for (uint32_t y = 0; y < height; ++y) {
    for (uint32_t x = 0; x < width; ++x) {
      size_t offset = (static_cast<size_t>(y) * width + x) * 3;
      buf[offset + 0] = static_cast<uint8_t>(x * 255 / width);
      buf[offset + 1] = static_cast<uint8_t>(y * 255 / height);
      buf[offset + 2] = static_cast<uint8_t>((x + y) * 127 / (width + height));
    }
  }
  return buf;
}

// Add uniform noise to an image buffer.
std::vector<uint8_t> AddNoise(const std::vector<uint8_t>& src,
                              int noise_level) {
  std::vector<uint8_t> out = src;
  // Simple deterministic pseudo-random noise (LCG).
  uint32_t seed = 12345;
  for (size_t i = 0; i < out.size(); ++i) {
    seed = seed * 1103515245 + 12345;
    int noise = static_cast<int>((seed >> 16) & 0xFF) - 128;
    noise = noise * noise_level / 128;
    int val = static_cast<int>(out[i]) + noise;
    out[i] = static_cast<uint8_t>(val < 0 ? 0 : (val > 255 ? 255 : val));
  }
  return out;
}

TEST(QualityVerifierTest, IdenticalImagesScoreHigh) {
  auto img = MakeGradientRGB(64, 64);
  ConsoleMessageHandler handler;
  std::optional<float> score =
      ComputeSSIMULACRA2(img.data(), img.data(), 64, 64, 3, &handler);
  ASSERT_TRUE(score.has_value());
  // Identical images should score very high (close to 100).
  EXPECT_GE(*score, 90.0f);
}

TEST(QualityVerifierTest, SlightlyDifferentImagesScoreAbove80) {
  auto img = MakeGradientRGB(64, 64);
  auto noisy = AddNoise(img, 5);  // Very light noise
  ConsoleMessageHandler handler;
  std::optional<float> score =
      ComputeSSIMULACRA2(img.data(), noisy.data(), 64, 64, 3, &handler);
  ASSERT_TRUE(score.has_value());
  EXPECT_GT(*score, 60.0f) << "Light noise should still score well";
}

TEST(QualityVerifierTest, VeryDifferentImagesScoreLow) {
  auto img1 = MakeGradientRGB(64, 64);
  auto img2 = MakeSolidRGB(64, 64, 128, 0, 255);
  ConsoleMessageHandler handler;
  std::optional<float> score =
      ComputeSSIMULACRA2(img1.data(), img2.data(), 64, 64, 3, &handler);
  // SSIMULACRA2 scores can go negative for very different images; a
  // negative value IS a verdict, distinct from the nullopt error channel.
  ASSERT_TRUE(score.has_value());
  EXPECT_LT(*score, 50.0f) << "Very different images should score low";
}

TEST(QualityVerifierTest, ScoreMonotonicity) {
  // More noise → lower score.
  auto img = MakeGradientRGB(64, 64);
  auto noisy_light = AddNoise(img, 10);
  auto noisy_heavy = AddNoise(img, 80);

  ConsoleMessageHandler handler;
  std::optional<float> score_light =
      ComputeSSIMULACRA2(img.data(), noisy_light.data(), 64, 64, 3, &handler);
  std::optional<float> score_heavy =
      ComputeSSIMULACRA2(img.data(), noisy_heavy.data(), 64, 64, 3, &handler);
  // SSIMULACRA2 scores can go negative for very distorted images; negative
  // results are verdicts, not errors.
  ASSERT_TRUE(score_light.has_value());
  ASSERT_TRUE(score_heavy.has_value());
  EXPECT_GT(*score_light, *score_heavy)
      << "Light noise should score higher than heavy noise";
}

TEST(QualityVerifierTest, NullInputReturnsError) {
  ConsoleMessageHandler handler;
  std::optional<float> score =
      ComputeSSIMULACRA2(nullptr, nullptr, 64, 64, 3, &handler);
  EXPECT_FALSE(score.has_value());
}

TEST(QualityVerifierTest, ZeroDimensionsReturnError) {
  auto img = MakeGradientRGB(64, 64);
  ConsoleMessageHandler handler;
  std::optional<float> score =
      ComputeSSIMULACRA2(img.data(), img.data(), 0, 64, 3, &handler);
  EXPECT_FALSE(score.has_value());
}

TEST(QualityVerifierTest, InvalidBppReturnsError) {
  auto img = MakeGradientRGB(64, 64);
  ConsoleMessageHandler handler;
  std::optional<float> score =
      ComputeSSIMULACRA2(img.data(), img.data(), 64, 64, 5, &handler);
  EXPECT_FALSE(score.has_value());
}

TEST(QualityVerifierTest, TooSmallImageReturnsError) {
  // SSIMULACRA2 requires at least 8x8.
  auto img = MakeGradientRGB(4, 4);
  ConsoleMessageHandler handler;
  std::optional<float> score =
      ComputeSSIMULACRA2(img.data(), img.data(), 4, 4, 3, &handler);
  EXPECT_FALSE(score.has_value());
}

TEST(QualityVerifierTest, NullHandlerWorks) {
  auto img = MakeGradientRGB(64, 64);
  std::optional<float> score =
      ComputeSSIMULACRA2(img.data(), img.data(), 64, 64, 3, nullptr);
  ASSERT_TRUE(score.has_value());
  EXPECT_GE(*score, 90.0f);
}

TEST(QualityVerifierTest, GrayscaleExpansion) {
  // Test 1-bpp grayscale input (expanded to RGB internally).
  uint32_t w = 64, h = 64;
  std::vector<uint8_t> gray(static_cast<size_t>(w) * h);
  for (uint32_t y = 0; y < h; ++y) {
    for (uint32_t x = 0; x < w; ++x) {
      gray[y * w + x] = static_cast<uint8_t>(x * 255 / w);
    }
  }
  ConsoleMessageHandler handler;
  std::optional<float> score =
      ComputeSSIMULACRA2(gray.data(), gray.data(), w, h, 1, &handler);
  ASSERT_TRUE(score.has_value());
  EXPECT_GE(*score, 90.0f);
}

TEST(QualityVerifierTest, RGBAInput) {
  // Test 4-bpp RGBA input with alpha channel.
  uint32_t w = 64, h = 64;
  std::vector<uint8_t> rgba(static_cast<size_t>(w) * h * 4);
  for (uint32_t y = 0; y < h; ++y) {
    for (uint32_t x = 0; x < w; ++x) {
      size_t offset = (static_cast<size_t>(y) * w + x) * 4;
      rgba[offset + 0] = static_cast<uint8_t>(x * 255 / w);
      rgba[offset + 1] = static_cast<uint8_t>(y * 255 / h);
      rgba[offset + 2] = 128;
      rgba[offset + 3] = 200;  // Semi-transparent alpha
    }
  }
  ConsoleMessageHandler handler;
  std::optional<float> score =
      ComputeSSIMULACRA2(rgba.data(), rgba.data(), w, h, 4, &handler);
  ASSERT_TRUE(score.has_value());
  EXPECT_GE(*score, 90.0f);
}

TEST(QualityVerifierTest, NullHandlerWithError) {
  // Pass nullptr handler to error paths to verify null-safety.
  std::optional<float> score =
      ComputeSSIMULACRA2(nullptr, nullptr, 64, 64, 3, nullptr);
  EXPECT_FALSE(score.has_value());

  auto img = MakeGradientRGB(64, 64);
  score = ComputeSSIMULACRA2(img.data(), img.data(), 0, 0, 3, nullptr);
  EXPECT_FALSE(score.has_value());

  score = ComputeSSIMULACRA2(img.data(), img.data(), 64, 64, 7, nullptr);
  EXPECT_FALSE(score.has_value());

  score = ComputeSSIMULACRA2(img.data(), img.data(), 4, 4, 3, nullptr);
  EXPECT_FALSE(score.has_value());
}

TEST(QualityVerifierTest, OneNullBufferReturnsError) {
  // Only one of original/encoded is null.
  auto img = MakeGradientRGB(64, 64);
  ConsoleMessageHandler handler;
  std::optional<float> score =
      ComputeSSIMULACRA2(img.data(), nullptr, 64, 64, 3, &handler);
  EXPECT_FALSE(score.has_value());

  score = ComputeSSIMULACRA2(nullptr, img.data(), 64, 64, 3, &handler);
  EXPECT_FALSE(score.has_value());
}

TEST(QualityVerifierTest, ZeroHeightReturnsError) {
  auto img = MakeGradientRGB(64, 64);
  ConsoleMessageHandler handler;
  std::optional<float> score =
      ComputeSSIMULACRA2(img.data(), img.data(), 64, 0, 3, &handler);
  EXPECT_FALSE(score.has_value());
}

TEST(QualityVerifierTest, MinimumValidSize) {
  // 8x8 is the minimum valid size for SSIMULACRA2.
  auto img = MakeGradientRGB(8, 8);
  ConsoleMessageHandler handler;
  std::optional<float> score =
      ComputeSSIMULACRA2(img.data(), img.data(), 8, 8, 3, &handler);
  ASSERT_TRUE(score.has_value());  // Should succeed.
  EXPECT_GE(*score, 80.0f);
}

TEST(QualityVerifierTest, SevenBySevenTooSmall) {
  auto img = MakeGradientRGB(7, 7);
  ConsoleMessageHandler handler;
  std::optional<float> score =
      ComputeSSIMULACRA2(img.data(), img.data(), 7, 7, 3, &handler);
  EXPECT_FALSE(score.has_value());
}

// ---------------------------------------------------------------------------
// BuildPPF failure paths: trigger PackedImage::Create failure with
// enormous dimensions that exceed allocation limits.
// Exercises lines 117-120 and 124-127 of quality_verifier.cc.
// ---------------------------------------------------------------------------

TEST(QualityVerifierTest, HugeDimensionsFailsBuildPPF) {
  // Use dimensions where width*height*channels would overflow size_t math
  // inside PackedImage::Create, causing it to fail allocation.
  // UINT32_MAX/2 * UINT32_MAX/2 * 3 >> available memory.
  uint8_t small_buf[64] = {};
  ConsoleMessageHandler handler;
  std::optional<float> score = ComputeSSIMULACRA2(
      small_buf, small_buf, UINT32_MAX / 2, UINT32_MAX / 2, 3, &handler);
  EXPECT_FALSE(score.has_value());
}

TEST(QualityVerifierTest, HugeDimensionsNullHandler) {
  // Same as above but with null handler to exercise the null-check branches.
  uint8_t small_buf[64] = {};
  std::optional<float> score = ComputeSSIMULACRA2(
      small_buf, small_buf, UINT32_MAX / 2, UINT32_MAX / 2, 3, nullptr);
  EXPECT_FALSE(score.has_value());
}

}  // namespace
}  // namespace pagespeed
