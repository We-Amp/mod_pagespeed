// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

// PageSpeed 2.0 - SVG Rasterizer Unit Tests

#include "lib/image/svg_rasterizer.h"

#include <climits>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "gtest/gtest.h"

namespace pagespeed {
namespace {

// -----------------------------------------------------------------
// RasterizeSvg tests
// -----------------------------------------------------------------

TEST(SvgRasterizerTest, RasterizeSolidRect) {
  // A simple red rectangle covering the full viewport.
  std::string svg =
      R"(<svg xmlns="http://www.w3.org/2000/svg" width="32" height="32">
    <rect x="0" y="0" width="32" height="32" fill="red"/>
  </svg>)";

  auto result = RasterizeSvg(svg, 32, 32);
  ASSERT_TRUE(result.success) << result.error;
  EXPECT_EQ(result.width, 32u);
  EXPECT_EQ(result.height, 32u);
  EXPECT_EQ(result.pixels.size(), 32u * 32u * 4u);

  // Center pixel should be red (R=255, G=0, B=0, A=255).
  size_t center = static_cast<size_t>((16 * 32 + 16) * 4);
  EXPECT_EQ(result.pixels[center + 0], 255);  // R
  EXPECT_EQ(result.pixels[center + 1], 0);    // G
  EXPECT_EQ(result.pixels[center + 2], 0);    // B
  EXPECT_EQ(result.pixels[center + 3], 255);  // A
}

TEST(SvgRasterizerTest, RasterizeInvalidSvg) {
  auto result = RasterizeSvg("not an svg", 32, 32);
  // nanosvg may parse garbage without error but produce empty output.
  // Either way, we shouldn't crash.
  // If it fails, error should be set.
  if (!result.success) {
    EXPECT_FALSE(result.error.empty());
  }
}

TEST(SvgRasterizerTest, RasterizeEmptyString) {
  auto result = RasterizeSvg("", 32, 32);
  EXPECT_FALSE(result.success);
  EXPECT_FALSE(result.error.empty());
}

TEST(SvgRasterizerTest, RasterizeDimensions) {
  std::string svg =
      R"(<svg xmlns="http://www.w3.org/2000/svg" width="100" height="50">
    <rect x="0" y="0" width="100" height="50" fill="blue"/>
  </svg>)";

  // Request different output dimensions.
  auto result = RasterizeSvg(svg, 64, 48);
  ASSERT_TRUE(result.success) << result.error;
  EXPECT_EQ(result.width, 64u);
  EXPECT_EQ(result.height, 48u);
  EXPECT_EQ(result.pixels.size(), 64u * 48u * 4u);
}

TEST(SvgRasterizerTest, RasterizeZeroDimensions) {
  std::string svg =
      R"(<svg xmlns="http://www.w3.org/2000/svg" width="32" height="32">
    <rect x="0" y="0" width="32" height="32" fill="green"/>
  </svg>)";

  auto result = RasterizeSvg(svg, 0, 32);
  EXPECT_FALSE(result.success);
  EXPECT_FALSE(result.error.empty());
}

// -----------------------------------------------------------------
// ConvertToRGBA tests
// -----------------------------------------------------------------

TEST(ConvertToRGBATest, RGBAPassthrough) {
  uint8_t pixels[] = {255, 0, 0, 128, 0, 255, 0, 64};
  auto rgba = ConvertToRGBA(pixels, 2, 1, 4);
  ASSERT_EQ(rgba.size(), 8u);
  EXPECT_EQ(rgba[0], 255);
  EXPECT_EQ(rgba[3], 128);
  EXPECT_EQ(rgba[4], 0);
  EXPECT_EQ(rgba[7], 64);
}

TEST(ConvertToRGBATest, RGBToRGBA) {
  uint8_t pixels[] = {255, 0, 0, 0, 255, 0};
  auto rgba = ConvertToRGBA(pixels, 2, 1, 3);
  ASSERT_EQ(rgba.size(), 8u);
  EXPECT_EQ(rgba[0], 255);  // R
  EXPECT_EQ(rgba[1], 0);    // G
  EXPECT_EQ(rgba[2], 0);    // B
  EXPECT_EQ(rgba[3], 255);  // A (added)
  EXPECT_EQ(rgba[4], 0);
  EXPECT_EQ(rgba[5], 255);
  EXPECT_EQ(rgba[6], 0);
  EXPECT_EQ(rgba[7], 255);
}

TEST(ConvertToRGBATest, GrayToRGBA) {
  uint8_t pixels[] = {128, 64};
  auto rgba = ConvertToRGBA(pixels, 2, 1, 1);
  ASSERT_EQ(rgba.size(), 8u);
  EXPECT_EQ(rgba[0], 128);
  EXPECT_EQ(rgba[1], 128);
  EXPECT_EQ(rgba[2], 128);
  EXPECT_EQ(rgba[3], 255);
  EXPECT_EQ(rgba[4], 64);
  EXPECT_EQ(rgba[5], 64);
  EXPECT_EQ(rgba[6], 64);
  EXPECT_EQ(rgba[7], 255);
}

TEST(ConvertToRGBATest, InvalidBpp) {
  uint8_t pixels[] = {0, 0, 0, 0};
  auto rgba = ConvertToRGBA(pixels, 2, 1, 2);
  EXPECT_TRUE(rgba.empty());
}

TEST(ConvertToRGBATest, NullPixels) {
  auto rgba = ConvertToRGBA(nullptr, 2, 1, 4);
  EXPECT_TRUE(rgba.empty());
}

TEST(ConvertToRGBATest, ZeroWidth) {
  uint8_t pixels[] = {0, 0, 0, 0};
  auto rgba = ConvertToRGBA(pixels, 0, 1, 4);
  EXPECT_TRUE(rgba.empty());
}

TEST(ConvertToRGBATest, ZeroHeight) {
  uint8_t pixels[] = {0, 0, 0, 0};
  auto rgba = ConvertToRGBA(pixels, 1, 0, 4);
  EXPECT_TRUE(rgba.empty());
}

TEST(ConvertToRGBATest, ExceedsMaxPixels) {
  // kMaxPixels = 50MB / 4 = 13,107,200.  Use dimensions that exceed it.
  uint8_t dummy = 0;
  auto rgba = ConvertToRGBA(&dummy, 4096, 4096, 4);  // 16M > 13.1M
  EXPECT_TRUE(rgba.empty());
}

// -----------------------------------------------------------------
// RasterizeSvg edge-case tests
// -----------------------------------------------------------------

TEST(SvgRasterizerTest, RasterizeZeroHeight) {
  std::string svg =
      R"(<svg xmlns="http://www.w3.org/2000/svg" width="32" height="32">
    <rect x="0" y="0" width="32" height="32" fill="green"/>
  </svg>)";

  auto result = RasterizeSvg(svg, 32, 0);
  EXPECT_FALSE(result.success);
  EXPECT_EQ(result.error, "Zero dimensions");
}

TEST(SvgRasterizerTest, RasterizeExceedsMaxPixels) {
  std::string svg =
      R"(<svg xmlns="http://www.w3.org/2000/svg" width="4096" height="4096">
    <rect x="0" y="0" width="4096" height="4096" fill="blue"/>
  </svg>)";

  auto result = RasterizeSvg(svg, 4096, 4096);  // 16M > 13.1M
  EXPECT_FALSE(result.success);
  EXPECT_EQ(result.error, "Dimensions too large for rasterization");
}

TEST(SvgRasterizerTest, RasterizeStrideOverflow) {
  std::string svg =
      R"(<svg xmlns="http://www.w3.org/2000/svg" width="32" height="32">
    <rect x="0" y="0" width="32" height="32" fill="red"/>
  </svg>)";

  // Width > INT_MAX/4 would overflow the stride calculation.
  uint32_t huge_width = static_cast<uint32_t>(INT_MAX) / 4 + 1;
  auto result = RasterizeSvg(svg, huge_width, 1);
  EXPECT_FALSE(result.success);
  // Either hits stride guard or kMaxPixels guard (both are acceptable).
  EXPECT_FALSE(result.error.empty());
}

TEST(SvgRasterizerTest, RasterizeSvgWithTransparency) {
  std::string svg =
      R"(<svg xmlns="http://www.w3.org/2000/svg" width="16" height="16">
    <rect x="0" y="0" width="16" height="16" fill="red" fill-opacity="0.5"/>
  </svg>)";

  auto result = RasterizeSvg(svg, 16, 16);
  ASSERT_TRUE(result.success) << result.error;
  // Center pixel should have partial alpha (not 255 and not 0).
  size_t center = static_cast<size_t>((8 * 16 + 8) * 4);
  EXPECT_GT(result.pixels[center + 3], 0);    // some alpha
  EXPECT_LT(result.pixels[center + 3], 255);  // not fully opaque
}

TEST(SvgRasterizerTest, RasterizeZeroViewBox) {
  // SVG with zero dimensions in the document.  nanosvg may or may not
  // report zero dimensions for this (behavior is implementation-defined).
  // The important thing is we don't crash.
  std::string svg =
      R"(<svg xmlns="http://www.w3.org/2000/svg" width="0" height="0">
    <rect x="0" y="0" width="32" height="32" fill="red"/>
  </svg>)";

  auto result = RasterizeSvg(svg, 32, 32);
  if (!result.success) {
    EXPECT_FALSE(result.error.empty());
  }
}

}  // namespace
}  // namespace pagespeed
