// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

// PageSpeed 2.0 - Image Dimensions Reader Tests

#include "lib/image/image_dimensions.h"

#include <cstdint>
#include <fstream>
#include <span>
#include <string>
#include <vector>

#include "gtest/gtest.h"

namespace pagespeed {
namespace {

// Read a test file into a byte vector.
std::vector<uint8_t> ReadTestFile(const std::string& path) {
  std::ifstream file(path, std::ios::binary);
  if (!file.is_open()) return {};
  return std::vector<uint8_t>(std::istreambuf_iterator<char>(file),
                              std::istreambuf_iterator<char>());
}

TEST(ImageDimensionsTest, EmptyData) {
  ImageDimensions dims = ReadImageDimensions({});
  EXPECT_FALSE(dims.valid);
}

TEST(ImageDimensionsTest, InvalidData) {
  uint8_t garbage[] = {0x00, 0x01, 0x02, 0x03};
  ImageDimensions dims =
      ReadImageDimensions(std::span<const uint8_t>(garbage, sizeof(garbage)));
  EXPECT_FALSE(dims.valid);
}

TEST(ImageDimensionsTest, PngDimensions) {
  auto data = ReadTestFile("test/lib/image/testdata/opaque_32x20.png");
  if (data.empty()) {
    GTEST_SKIP() << "opaque_32x20.png not found in testdata";
  }
  ImageDimensions dims =
      ReadImageDimensions(std::span<const uint8_t>(data.data(), data.size()));
  EXPECT_TRUE(dims.valid);
  EXPECT_EQ(dims.width, 32);
  EXPECT_EQ(dims.height, 20);
}

TEST(ImageDimensionsTest, PngAlphaDimensions) {
  auto data = ReadTestFile("test/lib/image/testdata/alpha_32x32.png");
  if (data.empty()) {
    GTEST_SKIP() << "alpha_32x32.png not found in testdata";
  }
  ImageDimensions dims =
      ReadImageDimensions(std::span<const uint8_t>(data.data(), data.size()));
  EXPECT_TRUE(dims.valid);
  EXPECT_EQ(dims.width, 32);
  EXPECT_EQ(dims.height, 32);
}

TEST(ImageDimensionsTest, WebpDimensions) {
  auto data = ReadTestFile("test/lib/image/testdata/opaque_32x20.webp");
  if (data.empty()) {
    GTEST_SKIP() << "opaque_32x20.webp not found in testdata";
  }
  ImageDimensions dims =
      ReadImageDimensions(std::span<const uint8_t>(data.data(), data.size()));
  EXPECT_TRUE(dims.valid);
  EXPECT_EQ(dims.width, 32);
  EXPECT_EQ(dims.height, 20);
}

TEST(ImageDimensionsTest, CorruptWebpHeader) {
  auto data = ReadTestFile("test/lib/image/testdata/corrupt_header.webp");
  if (data.empty()) {
    GTEST_SKIP() << "corrupt_header.webp not found in testdata";
  }
  ImageDimensions dims =
      ReadImageDimensions(std::span<const uint8_t>(data.data(), data.size()));
  // Corrupt headers may or may not return valid dims; just don't crash.
  (void)dims;
}

TEST(ImageDimensionsTest, JpegDimensions) {
  // Look for a JPEG test file.
  auto data =
      ReadTestFile("test/lib/image/testdata/jpeg/already_optimized.jpg");
  if (data.empty()) {
    GTEST_SKIP() << "No JPEG test file found";
  }
  ImageDimensions dims =
      ReadImageDimensions(std::span<const uint8_t>(data.data(), data.size()));
  EXPECT_TRUE(dims.valid);
  EXPECT_GT(dims.width, 0);
  EXPECT_GT(dims.height, 0);
}

TEST(ImageDimensionsTest, JpegExifOrientationReportsDisplayDimensions) {
  // Orientation=6 fixture: stored 16x32, displayed (and served after the
  // pipeline bakes the orientation) as 32x16. The probe must report display
  // dimensions so injected width/height attributes match the rendered image
  // (issue #1005).
  auto data =
      ReadTestFile("test/lib/image/testdata/jpeg/exif_orientation_6.jpg");
  ASSERT_FALSE(data.empty());
  ImageDimensions dims =
      ReadImageDimensions(std::span<const uint8_t>(data.data(), data.size()));
  EXPECT_TRUE(dims.valid);
  EXPECT_EQ(dims.width, 32);
  EXPECT_EQ(dims.height, 16);
}

TEST(ImageDimensionsTest, TooShortForMagic) {
  // Only 3 bytes — not enough for any format magic.
  uint8_t tiny[] = {0x89, 0x50, 0x4E};
  ImageDimensions dims =
      ReadImageDimensions(std::span<const uint8_t>(tiny, sizeof(tiny)));
  EXPECT_FALSE(dims.valid);
}

// ---------------------------------------------------------------------------
// Codec error paths must report, not crash.
//
// The readers log via PS_LOGGED_STATUS/PS_LOG_*, which tolerate a null
// MessageHandler (#1371), but the GIF reader still asserts the handler is
// non-null.  ReadImageDimensions therefore supplies a real NullMessageHandler
// rather than nullptr; before #1371 the logging macros dereferenced the
// handler unconditionally and nullptr made every case below a SIGSEGV (or,
// for GIF, a SIGABRT) rather than a clean valid=false.
//
// These inputs must all get PAST ComputeImageType -- a few garbage bytes
// return before any codec runs and would not exercise the reader at all.
// ---------------------------------------------------------------------------

// A PNG whose IHDR is well-formed but whose CRC is wrong, so libpng rejects
// the chunk and takes a logging error path.
TEST(ImageDimensionsTest, PngCorruptIhdrCrcDoesNotCrash) {
  // clang-format off
  static constexpr uint8_t kBadCrcPng[] = {
      0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A,  // PNG signature
      0x00, 0x00, 0x00, 0x0D, 0x49, 0x48, 0x44, 0x52,  // IHDR chunk
      0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x01,  // width=1, height=1
      0x08, 0x02, 0x00, 0x00, 0x00, 0xDE, 0xAD, 0xBE,  // 8bit RGB, BAD CRC
      0xEF, 0x00, 0x00, 0x00, 0x00, 0x49, 0x45, 0x4E,  // IEND chunk
      0x44, 0xAE, 0x42, 0x60, 0x82,                    // IEND CRC
  };
  // clang-format on
  ImageDimensions dims = ReadImageDimensions(
      std::span<const uint8_t>(kBadCrcPng, sizeof(kBadCrcPng)));
  EXPECT_FALSE(dims.valid);
}

// A PNG cut off in the middle of the IHDR chunk data.
TEST(ImageDimensionsTest, PngTruncatedMidIhdrDoesNotCrash) {
  // clang-format off
  static constexpr uint8_t kTruncatedPng[] = {
      0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A,  // PNG signature
      0x00, 0x00, 0x00, 0x0D, 0x49, 0x48, 0x44, 0x52,  // IHDR chunk header
      0x00, 0x00, 0x00, 0x01, 0x00, 0x00,              // width, partial height
  };
  // clang-format on
  ImageDimensions dims = ReadImageDimensions(
      std::span<const uint8_t>(kTruncatedPng, sizeof(kTruncatedPng)));
  EXPECT_FALSE(dims.valid);
}

// A JPEG with a valid SOI/APP0 opening that ends before any frame header.
TEST(ImageDimensionsTest, TruncatedJpegDoesNotCrash) {
  // clang-format off
  static constexpr uint8_t kTruncatedJpeg[] = {
      0xFF, 0xD8,                                      // SOI
      0xFF, 0xE0, 0x00, 0x10,                          // APP0, length 16
      0x4A, 0x46, 0x49, 0x46, 0x00,                    // "JFIF\0"
      0x01, 0x01, 0x00, 0x00, 0x01,                    // version, units, Xdens
      // truncated here: APP0 payload incomplete, no SOF, no EOI
  };
  // clang-format on
  ImageDimensions dims = ReadImageDimensions(
      std::span<const uint8_t>(kTruncatedJpeg, sizeof(kTruncatedJpeg)));
  EXPECT_FALSE(dims.valid);
}

// A minimal but fully VALID 1x1 GIF89a.  This is the case that used to abort
// on the GIF reader's assert(handler != nullptr) even though nothing about
// the input is malformed.
TEST(ImageDimensionsTest, MinimalValidGifDoesNotCrash) {
  // clang-format off
  static constexpr uint8_t kMinimalGif[] = {
      0x47, 0x49, 0x46, 0x38, 0x39, 0x61,              // "GIF89a"
      0x01, 0x00, 0x01, 0x00,                          // width=1, height=1
      0xF0, 0x00, 0x00,                                // GCT flag, bg, aspect
      0x00, 0x00, 0x00, 0xFF, 0xFF, 0xFF,              // 2-entry GCT
      0x2C, 0x00, 0x00, 0x00, 0x00,                    // image descriptor
      0x01, 0x00, 0x01, 0x00, 0x00,                    // 1x1, no local table
      0x02, 0x02, 0x4C, 0x01, 0x00,                    // LZW image data
      0x3B,                                            // trailer
  };
  // clang-format on
  ImageDimensions dims = ReadImageDimensions(
      std::span<const uint8_t>(kMinimalGif, sizeof(kMinimalGif)));
  EXPECT_TRUE(dims.valid);
  EXPECT_EQ(dims.width, 1);
  EXPECT_EQ(dims.height, 1);
}

// ---------------------------------------------------------------------------
// AVIF data: ComputeImageType returns IMAGE_AVIF which is not handled in
// the switch statement in ReadImageDimensions, hitting the default branch
// (return result with valid=false).
// ---------------------------------------------------------------------------

TEST(ImageDimensionsTest, AvifFormatHitsDefaultBranch) {
  // Construct minimal AVIF-like data: ISO BMFF container with ftyp box.
  // Bytes: [size(4)][ftyp(4)][avif(4)] + padding to >= 12 bytes.
  uint8_t avif_header[] = {
      0x00, 0x00, 0x00, 0x1C,  // box size = 28
      'f',  't',  'y',  'p',   // box type = ftyp
      'a',  'v',  'i',  'f',   // major brand = avif
      0x00, 0x00, 0x00, 0x00,  // minor version
      'a',  'v',  'i',  'f',   // compatible brand
      'm',  'i',  'f',  '1',   // compatible brand
      'i',  's',  'o',  'm',   // compatible brand
  };
  ImageDimensions dims = ReadImageDimensions(
      std::span<const uint8_t>(avif_header, sizeof(avif_header)));
  // AVIF is not handled in the switch, so valid should be false.
  EXPECT_FALSE(dims.valid);
}

// ---------------------------------------------------------------------------
// WebP lossless/alpha variant: exercises the IMAGE_WEBP_LOSSLESS_OR_ALPHA
// branch which maps to IMAGE_WEBP in ReadImageDimensions.
// ---------------------------------------------------------------------------

TEST(ImageDimensionsTest, WebpLosslessDimensions) {
  auto data = ReadTestFile("test/lib/image/testdata/alpha_32x32.webp");
  if (data.empty()) {
    GTEST_SKIP() << "alpha_32x32.webp not found in testdata";
  }
  ImageDimensions dims =
      ReadImageDimensions(std::span<const uint8_t>(data.data(), data.size()));
  EXPECT_TRUE(dims.valid);
  EXPECT_EQ(dims.width, 32);
  EXPECT_EQ(dims.height, 32);
}

// Orientation-6 PNG and WebP fixtures are stored 16x32 and displayed 32x16;
// the probe reports display dimensions, as it does for JPEG.
TEST(ImageDimensionsTest, PngExifOrientationReportsDisplayDimensions) {
  auto data =
      ReadTestFile("test/lib/image/testdata/exif/exif_orientation_6.png");
  ASSERT_FALSE(data.empty());
  ImageDimensions dims =
      ReadImageDimensions(std::span<const uint8_t>(data.data(), data.size()));
  EXPECT_TRUE(dims.valid);
  EXPECT_EQ(dims.width, 32);
  EXPECT_EQ(dims.height, 16);
}

TEST(ImageDimensionsTest, WebpExifOrientationReportsDisplayDimensions) {
  auto data =
      ReadTestFile("test/lib/image/testdata/exif/exif_orientation_6.webp");
  ASSERT_FALSE(data.empty());
  ImageDimensions dims =
      ReadImageDimensions(std::span<const uint8_t>(data.data(), data.size()));
  EXPECT_TRUE(dims.valid);
  EXPECT_EQ(dims.width, 32);
  EXPECT_EQ(dims.height, 16);
}

}  // namespace
}  // namespace pagespeed
