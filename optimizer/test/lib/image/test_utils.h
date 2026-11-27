// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

// Test utilities for image tests.
// Ported from mod_pagespeed's test/pagespeed/kernel/image/test_utils.h

#ifndef PAGESPEED_TEST_LIB_IMAGE_TEST_UTILS_H_
#define PAGESPEED_TEST_LIB_IMAGE_TEST_UTILS_H_

#include <cstddef>
#include <cstdint>
#include <string>

#include "lib/base/basictypes.h"
#include "lib/image/image_util.h"
#include "lib/image/scanline_interface.h"

namespace pagespeed {

class MessageHandler;

namespace image_compression {

using net_instaweb::ScanlineReaderInterface;

const char kTestRootDir[] = "test/lib/image/testdata/";

// Directory for test data (relative to kTestRootDir).
const char kGifTestDir[] = "gif/";
const char kJpegTestDir[] = "jpeg/";
const char kPngSuiteGifTestDir[] = "pngsuite/gif/";
const char kPngSuiteTestDir[] = "pngsuite/";
const char kPngTestDir[] = "png/";
const char kWebpTestDir[] = "webp/";
const char kResizedTestDir[] = "resized/";

// Message patterns for tests that need to suppress expected warnings.
const char kMessagePatternFailedToOpen[] = "*Failed to open*";
const char kMessagePatternFailedToRead[] = "*Failed to read*";
const char kMessagePatternLibpngError[] = "*libpng error:*";
const char kMessagePatternLibpngWarning[] = "*libpng warning:*";
const char kMessagePatternUnexpectedEOF[] = "*Unexpected EOF*";
const char kMessagePatternPixelFormat[] = "*Pixel format:*";
const char kMessagePatternStats[] = "*Stats:*";
const char kMessagePatternWritingToWebp[] = "*Writing to webp:*";

// Information about a GIF test image for golden-file comparison tests.
struct GoldImageCompressionInfo {
  const char* filename;
  int width;
  int height;
  bool transparency;
};

extern const GoldImageCompressionInfo kValidGifImages[];
extern const size_t kValidGifImageCount;

// Read a file from disk.
bool ReadFile(const std::string& file_name, std::string* content);

// Read a test file given directory, name, and extension.
bool ReadTestFile(const std::string& path, const char* name,
                  const char* extension, std::string* content);

// Read a test file given directory and name with extension.
bool ReadTestFileWithExt(const std::string& path,
                         const char* name_with_extension, std::string* content);

// Check whether the readers decode to exactly the same pixels.
void CompareImageReaders(ScanlineReaderInterface* reader1,
                         ScanlineReaderInterface* reader2);

// Check whether the images have the same content in the specified
// regions. Here "same content" means that the image regions "look"
// the same.
void CompareImageRegions(const uint8_t* image1, PixelFormat format1,
                         int bytes_per_row1, int col1, int row1,
                         const uint8_t* image2, PixelFormat format2,
                         int bytes_per_row2, int col2, int row2, int num_cols,
                         int num_rows, MessageHandler* handler);

// Return a synthesized image, each channel with the following
// pattern:
//   1st row: seed_value, seed_value + delta_x, ...
//   2nd row: 1st row + delta_y
//   ...
// Values will be wrapped around if they are greater than 255.
void SynthesizeImage(int width, int height, int bytes_per_line,
                     int num_channels, const uint8_t* seed_value,
                     const int* delta_x, const int* delta_y, uint8_t* image);

// Decode two images of given formats and compare their pixels.
// Uses ReadImage() internally to decode. If ignore_transparent_rgb is
// true, RGB values of fully transparent pixels are not compared.
void DecodeAndCompareImages(ImageFormat image_format1,
                            const void* image_buffer1, size_t buffer_length1,
                            ImageFormat image_format2,
                            const void* image_buffer2, size_t buffer_length2,
                            bool ignore_transparent_rgb,
                            MessageHandler* message_handler);

// Returns a string with a hex representation of the RGBA bytes.
inline std::string PixelRgbaChannelsToString(const uint8_t* const channels) {
  char buf[9];
  snprintf(buf, sizeof(buf), "%02x%02x%02x%02x", channels[RGBA_RED],
           channels[RGBA_GREEN], channels[RGBA_BLUE], channels[RGBA_ALPHA]);
  return std::string(buf);
}

// Packs the given A, R, G, B values into a single RGBA uint32.
inline uint32_t PackAsRgba(uint8_t alpha, uint8_t red, uint8_t green,
                           uint8_t blue) {
  return PackHiToLo(red, green, blue, alpha);
}

// Packs a pixel's color channel data in RGBA format to a single
// uint32_t.
inline uint32_t RgbaToPackedRgba(const PixelRgbaChannels rgba) {
  return PackAsRgba(rgba[RGBA_ALPHA], rgba[RGBA_RED], rgba[RGBA_GREEN],
                    rgba[RGBA_BLUE]);
}

}  // namespace image_compression

}  // namespace pagespeed

#endif  // PAGESPEED_TEST_LIB_IMAGE_TEST_UTILS_H_
