// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

// Unit tests for scanline utility functions.

#include "lib/image/scanline_utils.h"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>

#include "gtest/gtest.h"
#include "lib/base/message_handler.h"
#include "lib/image/image_util.h"

namespace pagespeed::image_compression {
namespace {

// =============================================================================
// GetNumChannelsFromPixelFormat tests
// =============================================================================

TEST(GetNumChannelsFromPixelFormatTest, Gray8) {
  NullMessageHandler handler;
  EXPECT_EQ(1u, GetNumChannelsFromPixelFormat(GRAY_8, &handler));
}

TEST(GetNumChannelsFromPixelFormatTest, Rgb888) {
  NullMessageHandler handler;
  EXPECT_EQ(3u, GetNumChannelsFromPixelFormat(RGB_888, &handler));
}

TEST(GetNumChannelsFromPixelFormatTest, Rgba8888) {
  NullMessageHandler handler;
  EXPECT_EQ(4u, GetNumChannelsFromPixelFormat(RGBA_8888, &handler));
}

TEST(GetNumChannelsFromPixelFormatTest, Unsupported) {
  NullMessageHandler handler;
  // UNSUPPORTED format should return 0 (falls through to default)
  EXPECT_EQ(0u, GetNumChannelsFromPixelFormat(UNSUPPORTED, &handler));
}

TEST(GetNumChannelsFromPixelFormatTest, NullHandler) {
  // Should not crash with a null handler
  EXPECT_EQ(1u, GetNumChannelsFromPixelFormat(GRAY_8, nullptr));
  EXPECT_EQ(3u, GetNumChannelsFromPixelFormat(RGB_888, nullptr));
  EXPECT_EQ(4u, GetNumChannelsFromPixelFormat(RGBA_8888, nullptr));
  EXPECT_EQ(0u, GetNumChannelsFromPixelFormat(UNSUPPORTED, nullptr));
}

// =============================================================================
// PaletteRGBA tests
// =============================================================================

TEST(PaletteRGBATest, BasicStructure) {
  PaletteRGBA palette;
  palette.red_ = 255;
  palette.green_ = 128;
  palette.blue_ = 64;
  palette.alpha_ = 200;
  EXPECT_EQ(255, palette.red_);
  EXPECT_EQ(128, palette.green_);
  EXPECT_EQ(64, palette.blue_);
  EXPECT_EQ(200, palette.alpha_);
}

// =============================================================================
// ScanlineStreamInput tests
// =============================================================================

TEST(ScanlineStreamInputTest, ConstructWithHandler) {
  NullMessageHandler handler;
  ScanlineStreamInput input(&handler);
  EXPECT_EQ(nullptr, input.data());
  EXPECT_EQ(0u, input.length());
  EXPECT_EQ(0u, input.offset());
  EXPECT_EQ(&handler, input.message_handler());
}

TEST(ScanlineStreamInputTest, InitializeWithBuffer) {
  NullMessageHandler handler;
  ScanlineStreamInput input(&handler);

  const char buffer[] = "test data";
  input.Initialize(buffer, sizeof(buffer));
  EXPECT_EQ(buffer, input.data());
  EXPECT_EQ(sizeof(buffer), input.length());
  EXPECT_EQ(0u, input.offset());
}

TEST(ScanlineStreamInputTest, InitializeWithString) {
  NullMessageHandler handler;
  ScanlineStreamInput input(&handler);

  std::string data = "test string data";
  input.Initialize(data);
  EXPECT_EQ(data.data(), input.data());
  EXPECT_EQ(data.length(), input.length());
  EXPECT_EQ(0u, input.offset());
}

TEST(ScanlineStreamInputTest, SetOffset) {
  NullMessageHandler handler;
  ScanlineStreamInput input(&handler);

  const char buffer[] = "test data";
  input.Initialize(buffer, sizeof(buffer));

  input.set_offset(5);
  EXPECT_EQ(5u, input.offset());

  input.set_offset(0);
  EXPECT_EQ(0u, input.offset());
}

TEST(ScanlineStreamInputTest, Reset) {
  NullMessageHandler handler;
  ScanlineStreamInput input(&handler);

  const char buffer[] = "test data";
  input.Initialize(buffer, sizeof(buffer));
  input.set_offset(3);

  input.Reset();
  EXPECT_EQ(nullptr, input.data());
  EXPECT_EQ(0u, input.length());
  EXPECT_EQ(0u, input.offset());

  // Message handler should still be accessible after Reset
  EXPECT_EQ(&handler, input.message_handler());
}

TEST(ScanlineStreamInputTest, ReInitialize) {
  NullMessageHandler handler;
  ScanlineStreamInput input(&handler);

  const char buffer1[] = "first buffer";
  input.Initialize(buffer1, sizeof(buffer1));
  input.set_offset(5);

  const char buffer2[] = "second buffer";
  input.Initialize(buffer2, sizeof(buffer2));
  EXPECT_EQ(buffer2, input.data());
  EXPECT_EQ(sizeof(buffer2), input.length());
  EXPECT_EQ(0u, input.offset());  // offset should be reset
}

// =============================================================================
// ExpandPixelFormat tests
// =============================================================================

TEST(ExpandPixelFormatTest, Gray8ToRgb888) {
  NullMessageHandler handler;
  const size_t num_pixels = 3;
  const uint8_t src[] = {100, 150, 200};
  uint8_t dst[9] = {0};

  EXPECT_TRUE(
      ExpandPixelFormat(num_pixels, GRAY_8, 0, src, RGB_888, 0, dst, &handler));

  // Each gray value should be replicated across R, G, B channels
  EXPECT_EQ(100, dst[0]);
  EXPECT_EQ(100, dst[1]);
  EXPECT_EQ(100, dst[2]);
  EXPECT_EQ(150, dst[3]);
  EXPECT_EQ(150, dst[4]);
  EXPECT_EQ(150, dst[5]);
  EXPECT_EQ(200, dst[6]);
  EXPECT_EQ(200, dst[7]);
  EXPECT_EQ(200, dst[8]);
}

TEST(ExpandPixelFormatTest, Rgb888ToRgb888) {
  NullMessageHandler handler;
  const size_t num_pixels = 2;
  const uint8_t src[] = {10, 20, 30, 40, 50, 60};
  uint8_t dst[6] = {0};

  EXPECT_TRUE(ExpandPixelFormat(num_pixels, RGB_888, 0, src, RGB_888, 0, dst,
                                &handler));

  // Should be a straight copy
  EXPECT_EQ(0, memcmp(src, dst, 6));
}

TEST(ExpandPixelFormatTest, Gray8ToRgba8888) {
  NullMessageHandler handler;
  const size_t num_pixels = 2;
  const uint8_t src[] = {100, 200};
  uint8_t dst[8] = {0};

  EXPECT_TRUE(ExpandPixelFormat(num_pixels, GRAY_8, 0, src, RGBA_8888, 0, dst,
                                &handler));

  // First pixel: gray=100, alpha=255
  EXPECT_EQ(100, dst[0]);
  EXPECT_EQ(100, dst[1]);
  EXPECT_EQ(100, dst[2]);
  EXPECT_EQ(kAlphaOpaque, dst[3]);

  // Second pixel: gray=200, alpha=255
  EXPECT_EQ(200, dst[4]);
  EXPECT_EQ(200, dst[5]);
  EXPECT_EQ(200, dst[6]);
  EXPECT_EQ(kAlphaOpaque, dst[7]);
}

TEST(ExpandPixelFormatTest, Rgb888ToRgba8888) {
  NullMessageHandler handler;
  const size_t num_pixels = 2;
  const uint8_t src[] = {10, 20, 30, 40, 50, 60};
  uint8_t dst[8] = {0};

  EXPECT_TRUE(ExpandPixelFormat(num_pixels, RGB_888, 0, src, RGBA_8888, 0, dst,
                                &handler));

  // First pixel
  EXPECT_EQ(10, dst[0]);
  EXPECT_EQ(20, dst[1]);
  EXPECT_EQ(30, dst[2]);
  EXPECT_EQ(kAlphaOpaque, dst[3]);

  // Second pixel
  EXPECT_EQ(40, dst[4]);
  EXPECT_EQ(50, dst[5]);
  EXPECT_EQ(60, dst[6]);
  EXPECT_EQ(kAlphaOpaque, dst[7]);
}

TEST(ExpandPixelFormatTest, Rgba8888ToRgba8888) {
  NullMessageHandler handler;
  const size_t num_pixels = 2;
  const uint8_t src[] = {10, 20, 30, 40, 50, 60, 70, 80};
  uint8_t dst[8] = {0};

  EXPECT_TRUE(ExpandPixelFormat(num_pixels, RGBA_8888, 0, src, RGBA_8888, 0,
                                dst, &handler));

  // Should be a straight copy
  EXPECT_EQ(0, memcmp(src, dst, 8));
}

TEST(ExpandPixelFormatTest, WithSrcOffset) {
  NullMessageHandler handler;
  // Source has 3 pixels, but we start from offset 1
  const uint8_t src[] = {10, 20, 30, 40, 50, 60, 70, 80, 90};
  uint8_t dst[6] = {0};

  EXPECT_TRUE(ExpandPixelFormat(2, RGB_888, 1, src, RGB_888, 0, dst, &handler));

  // Should have copied pixels starting from offset 1 (bytes 3-8)
  EXPECT_EQ(40, dst[0]);
  EXPECT_EQ(50, dst[1]);
  EXPECT_EQ(60, dst[2]);
  EXPECT_EQ(70, dst[3]);
  EXPECT_EQ(80, dst[4]);
  EXPECT_EQ(90, dst[5]);
}

TEST(ExpandPixelFormatTest, WithDstOffset) {
  NullMessageHandler handler;
  const uint8_t src[] = {10, 20, 30};
  uint8_t dst[9] = {0};

  EXPECT_TRUE(ExpandPixelFormat(1, RGB_888, 0, src, RGB_888, 1, dst, &handler));

  // First pixel in dst should be zeros (untouched)
  EXPECT_EQ(0, dst[0]);
  EXPECT_EQ(0, dst[1]);
  EXPECT_EQ(0, dst[2]);

  // Second pixel in dst should have the source data
  EXPECT_EQ(10, dst[3]);
  EXPECT_EQ(20, dst[4]);
  EXPECT_EQ(30, dst[5]);
}

TEST(ExpandPixelFormatTest, UnsupportedSrcFormatForRgb) {
  NullMessageHandler handler;
  const uint8_t src[] = {10, 20, 30, 40};
  uint8_t dst[3] = {0};

  // RGBA_8888 -> RGB_888 is not supported
  EXPECT_FALSE(
      ExpandPixelFormat(1, RGBA_8888, 0, src, RGB_888, 0, dst, &handler));
}

TEST(ExpandPixelFormatTest, UnsupportedDstFormat) {
  NullMessageHandler handler;
  const uint8_t src[] = {10, 20, 30};
  uint8_t dst[1] = {0};

  // RGB_888 -> GRAY_8 is not supported
  EXPECT_FALSE(ExpandPixelFormat(1, RGB_888, 0, src, GRAY_8, 0, dst, &handler));
}

TEST(ExpandPixelFormatTest, ZeroPixels) {
  NullMessageHandler handler;
  const uint8_t src[] = {10, 20, 30};
  uint8_t dst[3] = {99, 99, 99};

  // Converting zero pixels should succeed and not modify dst
  EXPECT_TRUE(ExpandPixelFormat(0, RGB_888, 0, src, RGB_888, 0, dst, &handler));
  EXPECT_EQ(99, dst[0]);
  EXPECT_EQ(99, dst[1]);
  EXPECT_EQ(99, dst[2]);
}

TEST(ExpandPixelFormatTest, NullHandler) {
  // Should work with null handler for valid conversions
  const uint8_t src[] = {100};
  uint8_t dst[3] = {0};

  EXPECT_TRUE(ExpandPixelFormat(1, GRAY_8, 0, src, RGB_888, 0, dst, nullptr));
  EXPECT_EQ(100, dst[0]);
  EXPECT_EQ(100, dst[1]);
  EXPECT_EQ(100, dst[2]);
}

TEST(ExpandPixelFormatTest, SinglePixelGray8ToRgba8888) {
  NullMessageHandler handler;
  const uint8_t src[] = {0};  // black pixel
  uint8_t dst[4] = {0};

  EXPECT_TRUE(
      ExpandPixelFormat(1, GRAY_8, 0, src, RGBA_8888, 0, dst, &handler));
  EXPECT_EQ(0, dst[0]);
  EXPECT_EQ(0, dst[1]);
  EXPECT_EQ(0, dst[2]);
  EXPECT_EQ(kAlphaOpaque, dst[3]);
}

TEST(ExpandPixelFormatTest, WhitePixelGray8ToRgba8888) {
  NullMessageHandler handler;
  const uint8_t src[] = {255};  // white pixel
  uint8_t dst[4] = {0};

  EXPECT_TRUE(
      ExpandPixelFormat(1, GRAY_8, 0, src, RGBA_8888, 0, dst, &handler));
  EXPECT_EQ(255, dst[0]);
  EXPECT_EQ(255, dst[1]);
  EXPECT_EQ(255, dst[2]);
  EXPECT_EQ(kAlphaOpaque, dst[3]);
}

TEST(ExpandPixelFormatTest, UnsupportedSrcFormatForRgba) {
  NullMessageHandler handler;
  const uint8_t src[] = {10};
  uint8_t dst[4] = {0};

  // UNSUPPORTED -> RGBA_8888 is not supported.
  EXPECT_FALSE(
      ExpandPixelFormat(1, UNSUPPORTED, 0, src, RGBA_8888, 0, dst, &handler));
}

TEST(ExpandPixelFormatTest, UnsupportedSrcFormatForRgbaNullHandler) {
  const uint8_t src[] = {10};
  uint8_t dst[4] = {0};

  // Should not crash with null handler for unsupported conversion.
  EXPECT_FALSE(
      ExpandPixelFormat(1, UNSUPPORTED, 0, src, RGBA_8888, 0, dst, nullptr));
}

TEST(ExpandPixelFormatTest, UnsupportedDstFormatNullHandler) {
  const uint8_t src[] = {10, 20, 30};
  uint8_t dst[1] = {0};

  // Should not crash with null handler for unsupported dst format.
  EXPECT_FALSE(ExpandPixelFormat(1, RGB_888, 0, src, GRAY_8, 0, dst, nullptr));
}

}  // namespace
}  // namespace pagespeed::image_compression
