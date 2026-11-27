// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

// Tests for the PNG codec.
// Ported from mod_pagespeed's test/pagespeed/kernel/image/
// png_optimizer_test.cc
//
// Adaptations:
// - GoogleString -> std::string
// - MockMessageHandler -> NullMessageHandler
// - Removed GIF-related tests (no GifReader)
// - Removed optipng-dependent tests (no opng_reduce_image)
// - Removed ReadImage/CreateScanlineWriter dependent tests
// - Removed DecodeAndCompareImages dependent tests
// - Adapted optimizer size assertions (no optipng reduction)

#include "lib/image/png_optimizer.h"

#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <limits>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "gtest/gtest.h"
#include "lib/base/basictypes.h"
#include "lib/base/message_handler.h"
#include "lib/image/scanline_utils.h"

extern "C" {
#include "png.h"  // NOLINT
#include "zlib.h"
}

namespace {

using net_instaweb::ScanlineReaderInterface;
using net_instaweb::ScanlineStatus;
using net_instaweb::ScanlineWriterInterface;
using pagespeed::NullMessageHandler;
using pagespeed::image_compression::GetNumChannelsFromPixelFormat;
using pagespeed::image_compression::GRAY_8;
using pagespeed::image_compression::kScanlineMaxDimension;
using pagespeed::image_compression::PixelFormat;
using pagespeed::image_compression::PngCompressParams;
using pagespeed::image_compression::PngOptimizer;
using pagespeed::image_compression::PngReader;
using pagespeed::image_compression::PngReaderInterface;
using pagespeed::image_compression::PngScanlineReader;
using pagespeed::image_compression::PngScanlineReaderRaw;
using pagespeed::image_compression::PngScanlineWriter;
using pagespeed::image_compression::RGB_888;
using pagespeed::image_compression::ScopedPngStruct;

// Directories for test data, relative to TEST_SRCDIR.
const char kPngSuiteTestDir[] = "pngsuite/";
const char kPngTestDir[] = "png/";

// Read a file from the testdata directory.
bool ReadTestFile(const std::string& dir, const char* name, const char* ext,
                  std::string* content) {
  // Bazel provides TEST_SRCDIR and TEST_WORKSPACE for runfiles
  std::string base = "test/lib/image/testdata/";
  std::string path = base + dir + name + "." + ext;
  std::ifstream file(path, std::ios::binary);
  if (!file.is_open()) {
    return false;
  }
  content->assign((std::istreambuf_iterator<char>(file)),
                  std::istreambuf_iterator<char>());
  return !content->empty();
}

bool ReadTestFile(const std::string& dir, const char* name, const char* ext,
                  std::string* content, bool allow_empty) {
  std::string base = "test/lib/image/testdata/";
  std::string path = base + dir + name + "." + ext;
  std::ifstream file(path, std::ios::binary);
  if (!file.is_open()) {
    return false;
  }
  content->assign((std::istreambuf_iterator<char>(file)),
                  std::istreambuf_iterator<char>());
  return allow_empty || !content->empty();
}

// Structure for valid PNG images with metadata.
struct ImageCompressionInfo {
  const char* filename;
  size_t original_size;
  int width;
  int height;
  int original_bit_depth;
  int original_color_type;
};

// These images were obtained from
// http://www.libpng.org/pub/png/pngsuite.html
// (We only store the fields needed for basic validation; compressed
// size checks are omitted since we no longer have optipng reduction.)
ImageCompressionInfo kValidImages[] = {
    {"basi0g01", 217, 32, 32, 1, 0},   {"basi0g02", 154, 32, 32, 2, 0},
    {"basi0g04", 247, 32, 32, 4, 0},   {"basi0g08", 254, 32, 32, 8, 0},
    {"basi0g16", 299, 32, 32, 16, 0},  {"basi2c08", 315, 32, 32, 8, 2},
    {"basi2c16", 595, 32, 32, 16, 2},  {"basi3p01", 132, 32, 32, 1, 3},
    {"basi3p02", 193, 32, 32, 2, 3},   {"basi3p04", 327, 32, 32, 4, 3},
    {"basi3p08", 1527, 32, 32, 8, 3},  {"basi4a08", 214, 32, 32, 8, 4},
    {"basi4a16", 2855, 32, 32, 16, 4}, {"basi6a08", 361, 32, 32, 8, 6},
    {"basi6a16", 4180, 32, 32, 16, 6}, {"basn0g01", 164, 32, 32, 1, 0},
    {"basn0g02", 104, 32, 32, 2, 0},   {"basn0g04", 145, 32, 32, 4, 0},
    {"basn0g08", 138, 32, 32, 8, 0},   {"basn0g16", 167, 32, 32, 16, 0},
    {"basn2c08", 145, 32, 32, 8, 2},   {"basn2c16", 302, 32, 32, 16, 2},
    {"basn3p01", 112, 32, 32, 1, 3},   {"basn3p02", 146, 32, 32, 2, 3},
    {"basn3p04", 216, 32, 32, 4, 3},   {"basn3p08", 1286, 32, 32, 8, 3},
    {"basn4a08", 126, 32, 32, 8, 4},   {"basn4a16", 2206, 32, 32, 16, 4},
    {"basn6a08", 184, 32, 32, 8, 6},   {"basn6a16", 3435, 32, 32, 16, 6},
    {"bgai4a08", 214, 32, 32, 8, 4},   {"bgai4a16", 2855, 32, 32, 16, 4},
    {"bgan6a08", 184, 32, 32, 8, 6},   {"bgan6a16", 3435, 32, 32, 16, 6},
    {"bgbn4a08", 140, 32, 32, 8, 4},   {"bggn4a16", 2220, 32, 32, 16, 4},
    {"bgwn6a08", 202, 32, 32, 8, 6},   {"bgyn6a16", 3453, 32, 32, 16, 6},
    {"ccwn2c08", 1514, 32, 32, 8, 2},  {"ccwn3p08", 1554, 32, 32, 8, 3},
    {"cdfn2c08", 404, 8, 32, 8, 2},    {"cdhn2c08", 344, 32, 8, 8, 2},
    {"cdsn2c08", 232, 8, 8, 8, 2},     {"cdun2c08", 724, 32, 32, 8, 2},
    {"ch1n3p04", 258, 32, 32, 4, 3},   {"ch2n3p08", 1810, 32, 32, 8, 3},
    {"cm0n0g04", 292, 32, 32, 4, 0},   {"cm7n0g04", 292, 32, 32, 4, 0},
    {"cm9n0g04", 292, 32, 32, 4, 0},   {"cs3n2c16", 214, 32, 32, 16, 2},
    {"cs3n3p08", 259, 32, 32, 8, 3},   {"cs5n2c08", 186, 32, 32, 8, 2},
    {"cs5n3p08", 271, 32, 32, 8, 3},   {"cs8n2c08", 149, 32, 32, 8, 2},
    {"cs8n3p08", 256, 32, 32, 8, 3},   {"ct0n0g04", 273, 32, 32, 4, 0},
    {"ct1n0g04", 792, 32, 32, 4, 0},   {"ctzn0g04", 753, 32, 32, 4, 0},
    {"f00n0g08", 319, 32, 32, 8, 0},   {"f00n2c08", 2475, 32, 32, 8, 2},
    {"f01n0g08", 321, 32, 32, 8, 0},   {"f01n2c08", 1180, 32, 32, 8, 2},
    {"f02n0g08", 355, 32, 32, 8, 0},   {"f02n2c08", 1729, 32, 32, 8, 2},
    {"f03n0g08", 389, 32, 32, 8, 0},   {"f03n2c08", 1291, 32, 32, 8, 2},
    {"f04n0g08", 269, 32, 32, 8, 0},   {"f04n2c08", 985, 32, 32, 8, 2},
    {"g03n0g16", 345, 32, 32, 16, 0},  {"g03n2c08", 370, 32, 32, 8, 2},
    {"g03n3p04", 214, 32, 32, 4, 3},   {"g04n0g16", 363, 32, 32, 16, 0},
    {"g04n2c08", 377, 32, 32, 8, 2},   {"g04n3p04", 219, 32, 32, 4, 3},
    {"g05n0g16", 339, 32, 32, 16, 0},  {"g05n2c08", 350, 32, 32, 8, 2},
    {"g05n3p04", 206, 32, 32, 4, 3},   {"g07n0g16", 321, 32, 32, 16, 0},
    {"g07n2c08", 340, 32, 32, 8, 2},   {"g07n3p04", 207, 32, 32, 4, 3},
    {"g10n0g16", 262, 32, 32, 16, 0},  {"g10n2c08", 285, 32, 32, 8, 2},
    {"g10n3p04", 214, 32, 32, 4, 3},   {"g25n0g16", 383, 32, 32, 16, 0},
    {"g25n2c08", 405, 32, 32, 8, 2},   {"g25n3p04", 215, 32, 32, 4, 3},
    {"oi1n0g16", 167, 32, 32, 16, 0},  {"oi1n2c16", 302, 32, 32, 16, 2},
    {"oi2n0g16", 179, 32, 32, 16, 0},  {"oi2n2c16", 314, 32, 32, 16, 2},
    {"oi4n0g16", 203, 32, 32, 16, 0},  {"oi4n2c16", 338, 32, 32, 16, 2},
    {"oi9n0g16", 1283, 32, 32, 16, 0}, {"oi9n2c16", 3038, 32, 32, 16, 2},
    {"pp0n2c16", 962, 32, 32, 16, 2},  {"pp0n6a08", 818, 32, 32, 8, 6},
    {"ps1n0g08", 1477, 32, 32, 8, 0},  {"ps1n2c16", 1641, 32, 32, 16, 2},
    {"ps2n0g08", 2341, 32, 32, 8, 0},  {"ps2n2c16", 2505, 32, 32, 16, 2},
    {"s01i3p01", 113, 1, 1, 1, 3},     {"s01n3p01", 113, 1, 1, 1, 3},
    {"s02i3p01", 114, 2, 2, 1, 3},     {"s02n3p01", 115, 2, 2, 1, 3},
    {"s03i3p01", 118, 3, 3, 1, 3},     {"s03n3p01", 120, 3, 3, 1, 3},
    {"s04i3p01", 126, 4, 4, 1, 3},     {"s04n3p01", 121, 4, 4, 1, 3},
    {"s05i3p02", 134, 5, 5, 2, 3},     {"s05n3p02", 129, 5, 5, 2, 3},
    {"s06i3p02", 143, 6, 6, 2, 3},     {"s06n3p02", 131, 6, 6, 2, 3},
    {"s07i3p02", 149, 7, 7, 2, 3},     {"s07n3p02", 138, 7, 7, 2, 3},
    {"s08i3p02", 149, 8, 8, 2, 3},     {"s08n3p02", 139, 8, 8, 2, 3},
    {"s09i3p02", 147, 9, 9, 2, 3},     {"s09n3p02", 143, 9, 9, 2, 3},
    {"s32i3p04", 355, 32, 32, 4, 3},   {"s32n3p04", 263, 32, 32, 4, 3},
    {"s33i3p04", 385, 33, 33, 4, 3},   {"s33n3p04", 329, 33, 33, 4, 3},
    {"s34i3p04", 349, 34, 34, 4, 3},   {"s34n3p04", 248, 34, 34, 4, 3},
    {"s35i3p04", 399, 35, 35, 4, 3},   {"s35n3p04", 338, 35, 35, 4, 3},
    {"s36i3p04", 356, 36, 36, 4, 3},   {"s36n3p04", 258, 36, 36, 4, 3},
    {"s37i3p04", 393, 37, 37, 4, 3},   {"s37n3p04", 336, 37, 37, 4, 3},
    {"s38i3p04", 357, 38, 38, 4, 3},   {"s38n3p04", 245, 38, 38, 4, 3},
    {"s39i3p04", 420, 39, 39, 4, 3},   {"s39n3p04", 352, 39, 39, 4, 3},
    {"s40i3p04", 357, 40, 40, 4, 3},   {"s40n3p04", 256, 40, 40, 4, 3},
    {"tbbn1g04", 419, 32, 32, 4, 0},   {"tbbn2c16", 1994, 32, 32, 16, 2},
    {"tbbn3p08", 1128, 32, 32, 8, 3},  {"tbgn2c16", 1994, 32, 32, 16, 2},
    {"tbgn3p08", 1128, 32, 32, 8, 3},  {"tbrn2c08", 1347, 32, 32, 8, 2},
    {"tbwn1g16", 1146, 32, 32, 16, 0}, {"tbwn3p08", 1131, 32, 32, 8, 3},
    {"tbyn3p08", 1131, 32, 32, 8, 3},  {"tp0n1g08", 689, 32, 32, 8, 0},
    {"tp0n2c08", 1311, 32, 32, 8, 2},  {"tp0n3p08", 1120, 32, 32, 8, 3},
    {"tp1n3p08", 1115, 32, 32, 8, 3},  {"z00n2c08", 3172, 32, 32, 8, 2},
    {"z03n2c08", 232, 32, 32, 8, 2},   {"z06n2c08", 224, 32, 32, 8, 2},
    {"z09n2c08", 224, 32, 32, 8, 2},
};

const char* kInvalidFiles[] = {
    "emptyfile",
    "x00n0g01",
    "xcrn0g04",
    "xlfn0g04",
};

struct OpaqueImageInfo {
  const char* filename;
  bool is_opaque;
  int in_color_type;
  int out_color_type;
};

OpaqueImageInfo kOpaqueImagesWithAlpha[] = {{"rgba_opaque", true, 6, 2},
                                            {"grey_alpha_opaque", true, 4, 0},
                                            {"bgai4a16", false, 4, 4}};

const size_t kValidImageCount = arraysize(kValidImages);
const size_t kInvalidFileCount = arraysize(kInvalidFiles);
const size_t kOpaqueImagesWithAlphaCount = arraysize(kOpaqueImagesWithAlpha);

bool InitializeEntireReader(const std::string& image_string,
                            const PngReader& png_reader,
                            PngScanlineReader* entire_image_reader) {
  // Initialize entire_image_reader.
  if (!entire_image_reader->Reset()) {
    return false;
  }
  entire_image_reader->set_transform(PNG_TRANSFORM_EXPAND |
                                     PNG_TRANSFORM_STRIP_16);

  if (entire_image_reader->InitializeRead(png_reader, image_string) == false) {
    return false;
  }

  // Skip the images which are not supported by PngScanlineReader.
  return (entire_image_reader->GetPixelFormat() !=
          pagespeed::image_compression::UNSUPPORTED);
}

// If allow_expand_colors is set to false, both readers must have
// the same color values, dimension, and pixel format. If it is set
// to true, additionally reader1 can be GRAY_8 while reader2 can be
// RGB_888 and the colors will be expanded prior to comparison.
void AssertReadersMatch(ScanlineReaderInterface* reader1,
                        ScanlineReaderInterface* reader2,
                        bool allow_expand_colors) {
  NullMessageHandler message_handler;

  // Make sure the images sizes and the pixel formats are the same.
  ASSERT_EQ(reader1->GetImageWidth(),
            reader2->GetImageWidth());  // NOLINT(bugprone-branch-clone)
  ASSERT_EQ(reader1->GetImageHeight(),
            reader2->GetImageHeight());  // NOLINT(bugprone-branch-clone)
  bool expand_colors = false;
  if (!allow_expand_colors) {
    ASSERT_EQ(reader1->GetPixelFormat(),
              reader2->GetPixelFormat());  // NOLINT(bugprone-branch-clone)
  } else {
    expand_colors = (reader1->GetPixelFormat() == GRAY_8 &&
                     reader2->GetPixelFormat() == RGB_888);
    ASSERT_TRUE(expand_colors ||  // NOLINT(bugprone-branch-clone)
                (reader1->GetPixelFormat() == reader2->GetPixelFormat()));
  }

  const int width = reader1->GetImageWidth();
  const int num_channels = GetNumChannelsFromPixelFormat(
      reader1->GetPixelFormat(), &message_handler);
  uint8_t* pixels1 = nullptr;
  uint8_t* pixels2 = nullptr;

  // Decode and check the image a scanline at a time.
  while (reader1->HasMoreScanLines() && reader2->HasMoreScanLines()) {
    ASSERT_TRUE(reader1->ReadNextScanline(
        reinterpret_cast<void**>(&pixels1)));  // NOLINT(bugprone-branch-clone)
    ASSERT_TRUE(reader2->ReadNextScanline(
        reinterpret_cast<void**>(&pixels2)));  // NOLINT(bugprone-branch-clone)

    if (!expand_colors) {
      for (int i = 0; i < width * num_channels; ++i) {
        ASSERT_EQ(pixels1[i], pixels2[i]);  // NOLINT(bugprone-branch-clone)
      }
    } else {
      for (int i = 0; i < width; ++i) {
        ASSERT_EQ(pixels1[i], pixels2[static_cast<ptrdiff_t>(
                                  3 * i)]);  // NOLINT(bugprone-branch-clone)
        ASSERT_EQ(pixels1[i],
                  pixels2[3 * i + 1]);  // NOLINT(bugprone-branch-clone)
        ASSERT_EQ(pixels1[i],
                  pixels2[3 * i + 2]);  // NOLINT(bugprone-branch-clone)
      }
    }
  }

  // Make sure both readers have exhausted all scanlines.
  ASSERT_FALSE(reader1->HasMoreScanLines());  // NOLINT(bugprone-branch-clone)
  ASSERT_FALSE(reader2->HasMoreScanLines());  // NOLINT(bugprone-branch-clone)
}

class PngOptimizerTest : public testing::Test {
 public:
  PngOptimizerTest() = default;

 protected:
  NullMessageHandler message_handler_;
  std::unique_ptr<PngReaderInterface> reader_;

 private:
  DISALLOW_COPY_AND_ASSIGN(PngOptimizerTest);
};

class PngScanlineReaderRawTest : public testing::Test {
 public:
  PngScanlineReaderRawTest() = default;

 protected:
  NullMessageHandler message_handler_;

 private:
  DISALLOW_COPY_AND_ASSIGN(PngScanlineReaderRawTest);
};

class PngScanlineWriterTest : public testing::Test {
 public:
  PngScanlineWriterTest()
      : params_(PngCompressParams(PNG_FILTER_NONE, Z_DEFAULT_STRATEGY, false)) {
  }

  bool Initialize() {
    writer_ = std::make_unique<PngScanlineWriter>(&message_handler_);
    if (!writer_->Init(width_, height_, pixel_format_)) {
      return false;
    }
    output_.clear();
    if (!writer_->InitializeWrite(&params_, &output_)) {
      return false;
    }
    return true;
  }

  void TestRewritePng(bool best_compression, int* total_bytes);

 protected:
  std::unique_ptr<PngScanlineWriter> writer_;
  std::string output_;
  PngCompressParams params_;
  unsigned char scanline_[3];
  static const int width_ = 3;
  static const int height_ = 2;
  static const PixelFormat pixel_format_ = pagespeed::image_compression::GRAY_8;
  NullMessageHandler message_handler_;

 private:
  DISALLOW_COPY_AND_ASSIGN(PngScanlineWriterTest);
};

// Test that we can read valid PNG attributes.
TEST_F(PngOptimizerTest, ValidPngAttributes) {
  reader_ = std::make_unique<PngReader>(&message_handler_);
  for (auto& kValidImage : kValidImages) {
    std::string in;
    ASSERT_TRUE(
        ReadTestFile(kPngSuiteTestDir, kValidImage.filename, "png", &in))
        << kValidImage.filename;
    EXPECT_EQ(kValidImage.original_size, in.size()) << kValidImage.filename;

    int width, height, bit_depth, color_type;
    ASSERT_TRUE(
        reader_->GetAttributes(in, &width, &height, &bit_depth, &color_type))
        << kValidImage.filename;
    EXPECT_EQ(kValidImage.width, width) << kValidImage.filename;
    EXPECT_EQ(kValidImage.height, height) << kValidImage.filename;
    EXPECT_EQ(kValidImage.original_bit_depth, bit_depth)
        << kValidImage.filename;
    EXPECT_EQ(kValidImage.original_color_type, color_type)
        << kValidImage.filename;
  }
}

// Test that OptimizePng succeeds on valid PNGs (we just check that
// it produces output, not exact size since we don't have optipng).
TEST_F(PngOptimizerTest, ValidPngsOptimize) {
  reader_ = std::make_unique<PngReader>(&message_handler_);
  for (auto& kValidImage : kValidImages) {
    std::string in, out;
    ASSERT_TRUE(
        ReadTestFile(kPngSuiteTestDir, kValidImage.filename, "png", &in))
        << kValidImage.filename;
    ASSERT_TRUE(
        PngOptimizer::OptimizePng(*reader_, in, &out, &message_handler_))
        << kValidImage.filename;
    EXPECT_GT(out.size(), 0u) << kValidImage.filename;

    // Also test best compression.
    std::string out_best;
    ASSERT_TRUE(PngOptimizer::OptimizePngBestCompression(
        *reader_, in, &out_best, &message_handler_))
        << kValidImage.filename;
    EXPECT_GT(out_best.size(), 0u) << kValidImage.filename;
  }
}

TEST(PngScanlineReaderTest, InitializeRead_validPngs) {
  NullMessageHandler message_handler;
  PngScanlineReader scanline_reader(&message_handler);
  if (setjmp(*scanline_reader.GetJmpBuf())) {
    ASSERT_FALSE(true) << "Execution should never reach here";
  }
  for (auto& kValidImage : kValidImages) {
    std::string in;
    ASSERT_TRUE(
        ReadTestFile(kPngSuiteTestDir, kValidImage.filename, "png", &in));
    PngReader png_reader(&message_handler);
    ASSERT_TRUE(scanline_reader.Reset());

    int width, height, bit_depth, color_type;
    ASSERT_TRUE(
        png_reader.GetAttributes(in, &width, &height, &bit_depth, &color_type));

    EXPECT_EQ(kValidImage.original_color_type, color_type);
    ASSERT_TRUE(scanline_reader.InitializeRead(png_reader, in));
    EXPECT_EQ(kValidImage.original_color_type, scanline_reader.GetColorType());
  }

  for (auto& i : kOpaqueImagesWithAlpha) {
    std::string in;
    ASSERT_TRUE(ReadTestFile(kPngSuiteTestDir, i.filename, "png", &in));
    PngReader png_reader(&message_handler);
    ASSERT_TRUE(scanline_reader.Reset());

    int width, height, bit_depth, color_type;
    ASSERT_TRUE(
        png_reader.GetAttributes(in, &width, &height, &bit_depth, &color_type));

    EXPECT_EQ(i.in_color_type, color_type);
    ASSERT_TRUE(scanline_reader.InitializeRead(png_reader, in));
    EXPECT_EQ(i.out_color_type, scanline_reader.GetColorType());
  }
}

TEST_F(PngOptimizerTest, ValidPngs_isOpaque) {
  ScopedPngStruct read(ScopedPngStruct::READ, &message_handler_);

  for (size_t i = 0; i < kOpaqueImagesWithAlphaCount; i++) {
    std::string in;
    ASSERT_TRUE(ReadTestFile(kPngSuiteTestDir,
                             kOpaqueImagesWithAlpha[i].filename, "png", &in));
    reader_ = std::make_unique<PngReader>(&message_handler_);
    ASSERT_TRUE(reader_->ReadPng(in, read.png_ptr(), read.info_ptr(), 0));
    EXPECT_EQ(kOpaqueImagesWithAlpha[i].is_opaque,
              PngReaderInterface::IsAlphaChannelOpaque(
                  read.png_ptr(), read.info_ptr(), &message_handler_));
    ASSERT_TRUE(read.reset());
  }
}

TEST_F(PngOptimizerTest, LargerPng) {
  reader_ = std::make_unique<PngReader>(&message_handler_);
  std::string in, out;
  ASSERT_TRUE(ReadTestFile(kPngTestDir, "this_is_a_test", "png", &in));
  ASSERT_EQ(static_cast<size_t>(20316), in.length());
  ASSERT_TRUE(PngOptimizer::OptimizePng(*reader_, in, &out, &message_handler_));

  int width, height, bit_depth, color_type;
  ASSERT_TRUE(
      reader_->GetAttributes(in, &width, &height, &bit_depth, &color_type));
  EXPECT_EQ(640, width);
  EXPECT_EQ(400, height);
  EXPECT_EQ(8, bit_depth);
  EXPECT_EQ(2, color_type);

  // The output should be a valid PNG.
  ASSERT_TRUE(
      reader_->GetAttributes(out, &width, &height, &bit_depth, &color_type));
  EXPECT_EQ(640, width);
  EXPECT_EQ(400, height);
}

TEST_F(PngOptimizerTest, InvalidPngs) {
  reader_ = std::make_unique<PngReader>(&message_handler_);
  for (auto& kInvalidFile : kInvalidFiles) {
    std::string in, out;
    ReadTestFile(kPngSuiteTestDir, kInvalidFile, "png", &in, true);
    ASSERT_FALSE(PngOptimizer::OptimizePngBestCompression(*reader_, in, &out,
                                                          &message_handler_));
    ASSERT_FALSE(
        PngOptimizer::OptimizePng(*reader_, in, &out, &message_handler_));

    int width, height, bit_depth, color_type;
    const bool get_attributes_result =
        reader_->GetAttributes(in, &width, &height, &bit_depth, &color_type);
    bool expected_get_attributes_result = false;
    if (strcmp("x00n0g01", kInvalidFile) == 0) {
      // Special case: even though the image is invalid, it has a
      // valid IDAT chunk, so we can read its attributes.
      expected_get_attributes_result = true;
    }
    EXPECT_EQ(expected_get_attributes_result, get_attributes_result)
        << kInvalidFile;
  }
}

TEST_F(PngOptimizerTest, FixPngOutOfBoundReadCrash) {
  reader_ = std::make_unique<PngReader>(&message_handler_);
  std::string in, out;
  ASSERT_TRUE(ReadTestFile(kPngTestDir, "read_from_stream_crash", "png", &in));
  ASSERT_EQ(static_cast<size_t>(193), in.length());
  ASSERT_FALSE(
      PngOptimizer::OptimizePng(*reader_, in, &out, &message_handler_));

  int width, height, bit_depth, color_type;
  ASSERT_TRUE(
      reader_->GetAttributes(in, &width, &height, &bit_depth, &color_type));
  EXPECT_EQ(32, width);
  EXPECT_EQ(32, height);
  EXPECT_EQ(2, bit_depth);
  EXPECT_EQ(3, color_type);
}

TEST_F(PngOptimizerTest, PartialPng) {
  reader_ = std::make_unique<PngReader>(&message_handler_);
  std::string in, out;
  int width, height, bit_depth, color_type;
  ASSERT_TRUE(ReadTestFile(kPngTestDir, "pagespeed-128", "png", &in));
  ASSERT_NE(static_cast<size_t>(0), in.length());
  // Loop, removing the last byte repeatedly to generate every
  // possible partial version of the PNG.
  while (true) {
    if (in.size() == 0) {
      break;
    }
    // Remove the last byte.
    in.erase(in.length() - 1);
    EXPECT_FALSE(
        PngOptimizer::OptimizePng(*reader_, in, &out, &message_handler_));

    // See if we can extract image attributes. Doing so requires
    // that at least 33 bytes are available (signature plus full
    // IDAT chunk).
    bool png_header_available = (in.size() >= 33);
    bool get_attributes_result =
        reader_->GetAttributes(in, &width, &height, &bit_depth, &color_type);
    EXPECT_EQ(png_header_available, get_attributes_result) << in.size();
    if (get_attributes_result) {
      EXPECT_EQ(128, width);
      EXPECT_EQ(128, height);
      EXPECT_EQ(8, bit_depth);
      EXPECT_EQ(3, color_type);
    }
  }
}

// Make sure that after we fail, we're still able to successfully
// compress valid images.
TEST_F(PngOptimizerTest, SuccessAfterFailure) {
  reader_ = std::make_unique<PngReader>(&message_handler_);
  for (size_t i = 0; i < kInvalidFileCount; ++i) {
    {
      std::string in, out;
      ReadTestFile(kPngSuiteTestDir, kInvalidFiles[i], "png", &in, true);
      ASSERT_FALSE(
          PngOptimizer::OptimizePng(*reader_, in, &out, &message_handler_));
    }

    {
      std::string in, out;
      ASSERT_TRUE(
          ReadTestFile(kPngSuiteTestDir, kValidImages[i].filename, "png", &in));
      ASSERT_TRUE(
          PngOptimizer::OptimizePng(*reader_, in, &out, &message_handler_));
      int width, height, bit_depth, color_type;
      ASSERT_TRUE(
          reader_->GetAttributes(in, &width, &height, &bit_depth, &color_type));
    }
  }
}

TEST_F(PngOptimizerTest, ScopedPngStruct) {
  ScopedPngStruct read(ScopedPngStruct::READ, &message_handler_);
  ASSERT_TRUE(read.valid());
  ASSERT_NE(static_cast<png_structp>(nullptr), read.png_ptr());
  ASSERT_NE(static_cast<png_infop>(nullptr), read.info_ptr());

  ScopedPngStruct write(ScopedPngStruct::WRITE, &message_handler_);
  ASSERT_TRUE(write.valid());
  ASSERT_NE(static_cast<png_structp>(nullptr), write.png_ptr());
  ASSERT_NE(static_cast<png_infop>(nullptr), write.info_ptr());
}

TEST(PngReaderTest, ReadTransparentPng) {
  NullMessageHandler message_handler;
  PngReader reader(&message_handler);
  ScopedPngStruct read(ScopedPngStruct::READ, &message_handler);
  std::string in;
  ASSERT_TRUE(ReadTestFile(kPngSuiteTestDir, "basn4a16", "png", &in));
  // Don't require_opaque.
  ASSERT_TRUE(reader.ReadPng(in, read.png_ptr(), read.info_ptr(),
                             PNG_TRANSFORM_IDENTITY, false));
  ASSERT_FALSE(reader.IsAlphaChannelOpaque(read.png_ptr(), read.info_ptr(),
                                           &message_handler));
  ASSERT_TRUE(read.reset());

  // Don't transform but require opaque.
  ASSERT_FALSE(reader.ReadPng(in, read.png_ptr(), read.info_ptr(),
                              PNG_TRANSFORM_IDENTITY, true));
  ASSERT_TRUE(read.reset());

  // Strip the alpha channel and require opaque.
  ASSERT_TRUE(reader.ReadPng(in, read.png_ptr(), read.info_ptr(),
                             PNG_TRANSFORM_STRIP_ALPHA, true));
  ASSERT_TRUE(read.reset());

  // Strip the alpha channel and don't require opaque.
  ASSERT_TRUE(reader.ReadPng(in, read.png_ptr(), read.info_ptr(),
                             PNG_TRANSFORM_STRIP_ALPHA, false));
  ASSERT_TRUE(read.reset());
}

TEST_F(PngScanlineReaderRawTest, ValidPngsRow) {
  // Create a reader which tries to read a row of image at a time.
  PngScanlineReaderRaw per_row_reader(&message_handler_);

  // Create a reader which reads the entire image.
  PngReader png_reader(&message_handler_);
  PngScanlineReader entire_image_reader(&message_handler_);
  if (setjmp(*entire_image_reader.GetJmpBuf()) != 0) {
    FAIL();
  }

  for (size_t i = 0; i < kValidImageCount; i++) {
    std::string image_string;
    ASSERT_TRUE(ReadTestFile(kPngSuiteTestDir, kValidImages[i].filename, "png",
                             &image_string));

    // Initialize entire_image_reader (PngScanlineReader).
    if (!InitializeEntireReader(image_string, png_reader,
                                &entire_image_reader)) {
      // This image is not supported by PngScanlineReader. Skip it.
      continue;
    }

    // Initialize per_row_reader.
    ASSERT_TRUE(
        per_row_reader.Initialize(image_string.data(), image_string.length()));

    // Make sure the images sizes and the pixel formats are the same.
    ASSERT_EQ(entire_image_reader.GetImageWidth(),
              per_row_reader.GetImageWidth());
    ASSERT_EQ(entire_image_reader.GetImageHeight(),
              per_row_reader.GetImageHeight());
    ASSERT_EQ(entire_image_reader.GetPixelFormat(),
              per_row_reader.GetPixelFormat());

    const int width = per_row_reader.GetImageWidth();
    const int num_channels = GetNumChannelsFromPixelFormat(
        per_row_reader.GetPixelFormat(), &message_handler_);
    uint8_t* buffer_per_row = nullptr;
    uint8_t* buffer_entire = nullptr;

    // Decode and check the image a row at a time.
    while (per_row_reader.HasMoreScanLines() &&
           entire_image_reader.HasMoreScanLines()) {
      ASSERT_TRUE(entire_image_reader.ReadNextScanline(
          reinterpret_cast<void**>(&buffer_entire)));

      ASSERT_TRUE(per_row_reader.ReadNextScanline(
          reinterpret_cast<void**>(&buffer_per_row)));

      for (int j = 0; j < width * num_channels; ++j) {
        ASSERT_EQ(buffer_entire[j], buffer_per_row[j]);
      }
    }

    // Make sure both readers have exhausted all image rows.
    ASSERT_FALSE(per_row_reader.HasMoreScanLines());
    ASSERT_FALSE(entire_image_reader.HasMoreScanLines());
  }
}

TEST_F(PngScanlineReaderRawTest, PartialRead) {
  uint8_t* buffer = nullptr;
  std::string image_string;
  ASSERT_TRUE(ReadTestFile(kPngSuiteTestDir, kValidImages[0].filename, "png",
                           &image_string));

  // Initialize a reader but do not read any scanline.
  PngScanlineReaderRaw reader1(&message_handler_);
  ASSERT_TRUE(reader1.Initialize(image_string.data(), image_string.length()));

  // Initialize a reader and read one scanline.
  PngScanlineReaderRaw reader2(&message_handler_);
  ASSERT_TRUE(reader2.Initialize(image_string.data(), image_string.length()));
  ASSERT_TRUE(reader2.ReadNextScanline(reinterpret_cast<void**>(&buffer)));

  // Initialize a reader, and try to read a scanline after the image
  // has been depleted.
  PngScanlineReaderRaw reader3(&message_handler_);
  ASSERT_TRUE(reader3.Initialize(image_string.data(), image_string.length()));
  while (reader3.HasMoreScanLines()) {
    ASSERT_TRUE(reader3.ReadNextScanline(reinterpret_cast<void**>(&buffer)));
  }

  // After depleting the scanlines, any further call to
  // ReadNextScanline should fail.
#ifdef NDEBUG
  ASSERT_FALSE(reader3.ReadNextScanline(reinterpret_cast<void**>(&buffer)));
#endif
}

TEST_F(PngScanlineReaderRawTest, ReadAfterReset) {
  uint8_t* buffer = nullptr;
  std::string image_string;
  ASSERT_TRUE(ReadTestFile(kPngSuiteTestDir, kValidImages[0].filename, "png",
                           &image_string));

  // Initialize a reader and read one scanline.
  PngScanlineReaderRaw reader(&message_handler_);
  ASSERT_TRUE(reader.Initialize(image_string.data(), image_string.length()));
  ASSERT_TRUE(reader.ReadNextScanline(reinterpret_cast<void**>(&buffer)));
  // Now re-initialize the reader.
  ASSERT_TRUE(reader.Initialize(image_string.data(), image_string.length()));

  // Read all scanlines from the re-initialized reader.
  while (reader.HasMoreScanLines()) {
    uint8_t* buffer_row = nullptr;
    ASSERT_TRUE(reader.ReadNextScanline(reinterpret_cast<void**>(&buffer_row)));
  }
}

// Appends a 32-bit big-endian value to *out.
void AppendUint32BE(std::string* out, uint32_t value) {
  char buf[4] = {static_cast<char>(value >> 24), static_cast<char>(value >> 16),
                 static_cast<char>(value >> 8), static_cast<char>(value)};
  out->append(buf, sizeof(buf));
}

// Appends a PNG chunk with a correct CRC.
void AppendPngChunk(std::string* out, const char* type, std::string_view data) {
  AppendUint32BE(out, data.size());
  out->append(type, 4);
  out->append(data.data(), data.size());
  uLong crc = crc32(0, Z_NULL, 0);
  crc = crc32(crc, reinterpret_cast<const Bytef*>(type), 4);
  crc = crc32(crc, reinterpret_cast<const Bytef*>(data.data()), data.size());
  AppendUint32BE(out, static_cast<uint32_t>(crc));
}

// Appends an IHDR chunk for an 8-bit truecolor RGB, non-interlaced image
// of the given dimensions.
void AppendPngIhdrRgb8(std::string* out, uint32_t width, uint32_t height) {
  char ihdr[13];
  ihdr[0] = static_cast<char>(width >> 24);
  ihdr[1] = static_cast<char>(width >> 16);
  ihdr[2] = static_cast<char>(width >> 8);
  ihdr[3] = static_cast<char>(width);
  ihdr[4] = static_cast<char>(height >> 24);
  ihdr[5] = static_cast<char>(height >> 16);
  ihdr[6] = static_cast<char>(height >> 8);
  ihdr[7] = static_cast<char>(height);
  ihdr[8] = 8;   // Bit depth.
  ihdr[9] = 2;   // Color type: truecolor RGB.
  ihdr[10] = 0;  // Compression: deflate.
  ihdr[11] = 0;  // Filter method.
  ihdr[12] = 0;  // No interlace.
  AppendPngChunk(out, "IHDR", std::string_view(ihdr, sizeof(ihdr)));
}

// Builds a tiny PNG stream whose IHDR declares the given dimensions but
// which carries no pixel data: enough structure for the reader to reach
// and act on the header dimensions.
std::string MakePngDeclaringDimensions(uint32_t width, uint32_t height) {
  std::string png("\x89PNG\r\n\x1a\n", 8);
  AppendPngIhdrRgb8(&png, width, height);
  AppendPngChunk(&png, "IDAT", std::string_view());
  AppendPngChunk(&png, "IEND", std::string_view());
  return png;
}

// Builds a complete, valid PNG declaring the given dimensions: an opaque
// black image whose IDAT carries real pixel data, so a decoder that accepts
// the dimensions will happily decode the whole stream.
std::string MakeValidPngDeclaringDimensions(uint32_t width, uint32_t height) {
  // Each scanline is a filter byte followed by width * 3 zero bytes.
  std::string raw;
  for (uint32_t row = 0; row < height; ++row) {
    raw.push_back('\0');
    raw.append(static_cast<size_t>(width) * 3, '\0');
  }
  uLongf idat_size = compressBound(raw.size());
  std::string idat(idat_size, '\0');
  if (compress2(reinterpret_cast<Bytef*>(&idat[0]), &idat_size,
                reinterpret_cast<const Bytef*>(raw.data()), raw.size(),
                9 /* level */) != Z_OK) {
    return "";
  }
  idat.resize(idat_size);

  std::string png("\x89PNG\r\n\x1a\n", 8);
  AppendPngIhdrRgb8(&png, width, height);
  AppendPngChunk(&png, "IDAT", idat);
  AppendPngChunk(&png, "IEND", std::string_view());
  return png;
}

// A stream declaring dimensions far beyond any image we can decode must be
// rejected at Initialize time, before any buffer proportional to
// width * height is allocated.
TEST_F(PngScanlineReaderRawTest, OversizedDimensionsRejected) {
  PngScanlineReaderRaw reader(&message_handler_);
  std::string bomb = MakePngDeclaringDimensions(60000, 60000);
  ScanlineStatus status =
      reader.InitializeWithStatus(bomb.data(), bomb.length());
  EXPECT_FALSE(status.Success());
}

// A stream whose area fits the decode budget but whose width exceeds the
// per-side cap is rejected the same way.
TEST_F(PngScanlineReaderRawTest, OversizedWidthRejected) {
  PngScanlineReaderRaw reader(&message_handler_);
  std::string wide = MakePngDeclaringDimensions(16385, 1);
  ScanlineStatus status =
      reader.InitializeWithStatus(wide.data(), wide.length());
  EXPECT_FALSE(status.Success());
}

// The whole-image optimizer decodes via png_read_png, which buffers every
// row from the IHDR before any data is seen. A complete image one pixel
// wider than the per-side cap must be refused rather than decoded whole.
TEST_F(PngOptimizerTest, OversizedWholeImageDimensionsRejected) {
  reader_ = std::make_unique<PngReader>(&message_handler_);
  std::string in =
      MakeValidPngDeclaringDimensions(kScanlineMaxDimension + 1, 1);
  ASSERT_NE(static_cast<size_t>(0), in.length());
  std::string out;
  EXPECT_FALSE(PngOptimizer::OptimizePngBestCompression(*reader_, in, &out,
                                                        &message_handler_));
  EXPECT_TRUE(out.empty());
}

// The best-compression pass re-reads the just-written PNG in full via
// png_read_png, so the writer must apply the same per-side cap on that
// re-read as the readers apply on the first decode.
TEST_F(PngScanlineWriterTest, OversizedDimensionsRejectedByBestCompression) {
  const size_t width = static_cast<size_t>(kScanlineMaxDimension) + 1;
  PngCompressParams best(true /* best compression */, false /* progressive */);
  writer_ = std::make_unique<PngScanlineWriter>(&message_handler_);
  ASSERT_TRUE(writer_->Init(width, height_, pixel_format_));
  output_.clear();
  ASSERT_TRUE(writer_->InitializeWrite(&best, &output_));
  // pixel_format_ is GRAY_8, so each row is one byte per pixel.
  std::vector<unsigned char> row(width, 0);
  for (int i = 0; i < height_; ++i) {
    ASSERT_TRUE(writer_->WriteNextScanline(row.data()));
  }
  EXPECT_FALSE(writer_->FinalizeWrite());
}

TEST_F(PngScanlineReaderRawTest, InvalidPngs) {
  PngScanlineReaderRaw reader(&message_handler_);
  for (auto& kInvalidFile : kInvalidFiles) {
    std::string image_string;
    ReadTestFile(kPngSuiteTestDir, kInvalidFile, "png", &image_string, true);

    ASSERT_FALSE(reader.Initialize(image_string.data(), image_string.length()));
  }
}

// Make sure that PNG files are written correctly. We firstly
// decompress a PNG image; then compress it to a new PNG image;
// finally verify that the new PNG is the same as the original one.
void PngScanlineWriterTest::TestRewritePng(bool best_compression,
                                           int* total_bytes) {
  *total_bytes = 0;
  PngScanlineReaderRaw original_reader(&message_handler_);
  PngScanlineReaderRaw rewritten_reader(&message_handler_);

  // List of filters supported by libpng.
  const int png_filter_list[] = {
      PNG_FILTER_NONE,  // 0x08
      PNG_FILTER_SUB,   // 0x10
      PNG_FILTER_UP,    // 0x20
      PNG_FILTER_AVG,   // 0x40
      PNG_FILTER_PAETH  // 0x80
  };

  for (size_t i = 0; i < kValidImageCount; i++) {
    std::string original_image;
    std::string rewritten_image;

    ASSERT_TRUE(ReadTestFile(kPngSuiteTestDir, kValidImages[i].filename, "png",
                             &original_image));

    // Initialize a PNG reader for reading the original image.
    if (original_reader.Initialize(original_image.data(),
                                   original_image.length()) == false) {
      // Some images in kValidImages[] have unsupported formats, for
      // example, GRAY_ALPHA. These images are skipped.
      continue;
    }

    // Get the sizes and pixel format of the original image.
    const size_t width = original_reader.GetImageWidth();
    const size_t height = original_reader.GetImageHeight();
    const PixelFormat pixel_format = original_reader.GetPixelFormat();

    // Use a new combination of filter and compression level for
    // writing this image.
    const int num_z = Z_FIXED - Z_DEFAULT_STRATEGY + 1;
    int compression_strategy = Z_DEFAULT_STRATEGY + (i % num_z);
    int filter_level = png_filter_list[(i / num_z) % 5];
    std::unique_ptr<PngCompressParams> params =
        best_compression ? std::make_unique<PngCompressParams>(
                               true /*best compression*/, true /*progressive*/)
                         : std::make_unique<PngCompressParams>(
                               filter_level, compression_strategy, false);

    // Initialize the writer.
    auto writer = std::make_unique<PngScanlineWriter>(&message_handler_);
    ASSERT_TRUE(writer->Init(width, height, pixel_format));
    ASSERT_TRUE(writer->InitializeWrite(params.get(), &rewritten_image));

    // Read the scanlines from the original image and write them to
    // the new one.
    while (original_reader.HasMoreScanLines()) {
      uint8_t* scanline = nullptr;
      ASSERT_TRUE(original_reader.ReadNextScanline(
          reinterpret_cast<void**>(&scanline)));
      ASSERT_TRUE(writer->WriteNextScanline(reinterpret_cast<void*>(scanline)));
    }

    // Make sure that the readers has exhausted the original image.
    ASSERT_FALSE(original_reader.HasMoreScanLines());
    // Make sure that the writer has received all of the image data,
    // and finalize it.
    ASSERT_TRUE(writer->FinalizeWrite());

    // Now create readers for reading the original and the rewritten
    // images.
    ASSERT_TRUE(original_reader.Initialize(original_image.data(),
                                           original_image.length()));
    ASSERT_TRUE(rewritten_reader.Initialize(rewritten_image.data(),
                                            rewritten_image.length()));

    // Now make sure that the original and rewritten images have the
    // same dimensions, types, and pixel values. When
    // "best_compression" is true, the pixel format (i.e., number
    // of color channels) may change, so we allow expanding colors.
    AssertReadersMatch(&original_reader, &rewritten_reader,
                       best_compression /* allow expanding colors */);

    *total_bytes += rewritten_image.length();
  }
}

TEST_F(PngScanlineWriterTest, RewritePng) {
  int total_bytes = 0;
  int total_bytes_best = 0;
  TestRewritePng(false /* no best compression */, &total_bytes);
  TestRewritePng(true /* best compression */, &total_bytes_best);
  // For a large corpus of images, "best compression" should be
  // better overall.
  EXPECT_GT(total_bytes, total_bytes_best);
}

// Attempt to finalize without writing all of the scanlines.
TEST_F(PngScanlineWriterTest, EarlyFinalize) {
  ASSERT_TRUE(Initialize());
  ASSERT_TRUE(writer_->WriteNextScanline(reinterpret_cast<void*>(scanline_)));
#ifdef NDEBUG
  ASSERT_FALSE(writer_->FinalizeWrite());
#endif
}

// Write insufficient number of scanlines and do not finalize at the
// end.
TEST_F(PngScanlineWriterTest, MissingScanlines) {
  ASSERT_TRUE(Initialize());
  ASSERT_TRUE(writer_->WriteNextScanline(reinterpret_cast<void*>(scanline_)));
}

// Write too many scanlines.
TEST_F(PngScanlineWriterTest, TooManyScanlines) {
  ASSERT_TRUE(Initialize());
  ASSERT_TRUE(writer_->WriteNextScanline(reinterpret_cast<void*>(scanline_)));
  ASSERT_TRUE(writer_->WriteNextScanline(reinterpret_cast<void*>(scanline_)));
#ifdef NDEBUG
  ASSERT_FALSE(writer_->WriteNextScanline(reinterpret_cast<void*>(scanline_)));
#endif
}

// Write a scanline, and then re-initialize and write too many
// scanlines.
TEST_F(PngScanlineWriterTest, ReinitializeAndTooManyScanlines) {
  ASSERT_TRUE(Initialize());
  ASSERT_TRUE(writer_->WriteNextScanline(reinterpret_cast<void*>(scanline_)));

  ASSERT_TRUE(Initialize());
  ASSERT_TRUE(writer_->WriteNextScanline(reinterpret_cast<void*>(scanline_)));
  ASSERT_TRUE(writer_->WriteNextScanline(reinterpret_cast<void*>(scanline_)));
#ifdef NDEBUG
  ASSERT_FALSE(writer_->WriteNextScanline(reinterpret_cast<void*>(scanline_)));
#endif
}

// ---------------------------------------------------------------------------
// Phase 3.4: PNG Optimizer Internals
// ---------------------------------------------------------------------------

// --- Grayscale Detection ---

TEST_F(PngOptimizerTest, GrayscaleSavedAsRGBGetsReduced) {
  // A grayscale image saved as RGB should be optimized to a smaller file
  // because the optimizer detects that all channels are equal and reduces
  // RGB to Grayscale.
  reader_ = std::make_unique<PngReader>(&message_handler_);
  std::string in, out;
  ASSERT_TRUE(ReadTestFile("", "gray_saved_as_rgb", "png", &in));
  ASSERT_TRUE(PngOptimizer::OptimizePng(*reader_, in, &out, &message_handler_));
  EXPECT_GT(out.size(), 0u);
  // The optimized output should be strictly smaller because RGB→Gray
  // strips two redundant channels.
  EXPECT_LT(out.size(), in.size())
      << "Optimizer should reduce RGB-encoded grayscale to a smaller file";
}

TEST_F(PngOptimizerTest, TrueGrayscaleStaysGrayscale) {
  // A true grayscale image should remain grayscale after optimization.
  reader_ = std::make_unique<PngReader>(&message_handler_);
  std::string in, out;
  ASSERT_TRUE(ReadTestFile("", "gray_saved_as_gray", "png", &in));
  ASSERT_TRUE(PngOptimizer::OptimizePng(*reader_, in, &out, &message_handler_));
  EXPECT_GT(out.size(), 0u);

  // Verify that the output is still a grayscale image (color_type 0).
  int width, height, bit_depth, color_type;
  ASSERT_TRUE(
      reader_->GetAttributes(out, &width, &height, &bit_depth, &color_type));
  EXPECT_EQ(0, color_type)
      << "True grayscale input should remain grayscale after optimization";
}

// --- Alpha Stripping ---

TEST_F(PngOptimizerTest, OpaqueAlphaChannelStripped) {
  // An RGBA image where all alpha values are 255 (fully opaque) should
  // have the alpha channel stripped, resulting in a smaller file.
  reader_ = std::make_unique<PngReader>(&message_handler_);
  std::string in, out;
  ASSERT_TRUE(ReadTestFile(kPngSuiteTestDir, "rgba_opaque", "png", &in));
  ASSERT_TRUE(PngOptimizer::OptimizePng(*reader_, in, &out, &message_handler_));
  EXPECT_GT(out.size(), 0u);

  // Verify that the optimized output has no alpha channel.
  // color_type 4 (gray+alpha) and 6 (RGBA) carry alpha; the optimizer
  // should strip it. The result may be RGB (2), grayscale (0), or
  // palette (3) depending on what the optimizer determines is best.
  int width, height, bit_depth, color_type;
  ASSERT_TRUE(
      reader_->GetAttributes(out, &width, &height, &bit_depth, &color_type));
  EXPECT_NE(4, color_type)
      << "Opaque alpha should be stripped (got gray+alpha)";
  EXPECT_NE(6, color_type) << "Opaque alpha should be stripped (got RGBA)";
  // Output should be smaller due to removed alpha channel (and possibly
  // palette conversion).
  EXPECT_LT(out.size(), in.size());
}

TEST_F(PngOptimizerTest, TransparentAlphaPreserved) {
  // A PNG with actual (non-opaque) alpha transparency should preserve
  // its alpha channel through optimization.
  reader_ = std::make_unique<PngReader>(&message_handler_);
  std::string in, out;
  // basn4a08 is a gray+alpha image with non-trivial transparency.
  ASSERT_TRUE(ReadTestFile(kPngSuiteTestDir, "basn4a08", "png", &in));
  ASSERT_TRUE(PngOptimizer::OptimizePng(*reader_, in, &out, &message_handler_));
  EXPECT_GT(out.size(), 0u);

  // Verify that alpha is preserved: color_type should still include alpha.
  // For gray+alpha input, the output color_type should be 4 (gray+alpha).
  int width, height, bit_depth, color_type;
  ASSERT_TRUE(
      reader_->GetAttributes(out, &width, &height, &bit_depth, &color_type));
  // color_type 4 = gray+alpha, color_type 6 = RGBA; both preserve alpha.
  EXPECT_TRUE(color_type == 4 || color_type == 6)
      << "Alpha should be preserved for images with actual transparency, "
         "got color_type="
      << color_type;
}

// --- Optimization Reduces Size ---

TEST_F(PngOptimizerTest, OptimizePngReducesSize) {
  // A typical PNG should be optimized to a smaller (or at least not larger
  // in the pathological case) size.
  reader_ = std::make_unique<PngReader>(&message_handler_);
  std::string in, out;
  ASSERT_TRUE(ReadTestFile(kPngTestDir, "this_is_a_test", "png", &in));
  ASSERT_TRUE(PngOptimizer::OptimizePng(*reader_, in, &out, &message_handler_));
  EXPECT_GT(out.size(), 0u);
  // The optimizer should produce output no larger than the input.
  // For a typical PNG, it should actually reduce size.
  EXPECT_LE(out.size(), in.size())
      << "OptimizePng should not increase file size for a typical image";
}

TEST_F(PngOptimizerTest, BestCompressionSmallerThanDefault) {
  // For a corpus of images, best compression should produce smaller total
  // output than default compression.
  reader_ = std::make_unique<PngReader>(&message_handler_);
  size_t total_default = 0;
  size_t total_best = 0;
  int count = 0;
  for (size_t i = 0; i < kValidImageCount; i++) {
    std::string in, out_default, out_best;
    ASSERT_TRUE(
        ReadTestFile(kPngSuiteTestDir, kValidImages[i].filename, "png", &in))
        << kValidImages[i].filename;
    if (!PngOptimizer::OptimizePng(*reader_, in, &out_default,
                                   &message_handler_)) {
      continue;
    }
    if (!PngOptimizer::OptimizePngBestCompression(*reader_, in, &out_best,
                                                  &message_handler_)) {
      continue;
    }
    total_default += out_default.size();
    total_best += out_best.size();
    ++count;
  }
  ASSERT_GT(count, 0) << "At least some images should optimize successfully";
  EXPECT_LE(total_best, total_default)
      << "Best compression should produce smaller or equal total output "
         "compared to default compression across the corpus";
}

// --- Interlace Handling ---

TEST_F(PngOptimizerTest, InterlacedPngOptimizes) {
  // Interlaced PNGs (basi* prefix = basic interlaced) should optimize
  // without crashing or returning errors.
  reader_ = std::make_unique<PngReader>(&message_handler_);
  // Test several interlaced images from the pngsuite.
  const char* interlaced_files[] = {
      "basi0g08",  // grayscale 8-bit interlaced
      "basi2c08",  // RGB 8-bit interlaced
      "basi4a08",  // gray+alpha 8-bit interlaced
      "basi6a08",  // RGBA 8-bit interlaced
      "basi3p08",  // palette 8-bit interlaced
  };
  for (const char* name : interlaced_files) {
    std::string in, out;
    ASSERT_TRUE(ReadTestFile(kPngSuiteTestDir, name, "png", &in)) << name;
    ASSERT_TRUE(
        PngOptimizer::OptimizePng(*reader_, in, &out, &message_handler_))
        << "Interlaced PNG should optimize without error: " << name;
    EXPECT_GT(out.size(), 0u) << name;

    // Verify the output is a valid PNG with correct dimensions.
    int width, height, bit_depth, color_type;
    ASSERT_TRUE(
        reader_->GetAttributes(out, &width, &height, &bit_depth, &color_type))
        << name;
    EXPECT_EQ(32, width) << name;
    EXPECT_EQ(32, height) << name;
  }
}

// --- Reader Attributes ---

TEST_F(PngOptimizerTest, GetAttributesReportsCorrectDimensions) {
  // Load a PNG with known dimensions and verify GetAttributes returns
  // the correct values.
  reader_ = std::make_unique<PngReader>(&message_handler_);
  std::string in;
  ASSERT_TRUE(ReadTestFile(kPngTestDir, "this_is_a_test", "png", &in));

  int width, height, bit_depth, color_type;
  ASSERT_TRUE(
      reader_->GetAttributes(in, &width, &height, &bit_depth, &color_type));
  EXPECT_EQ(640, width);
  EXPECT_EQ(400, height);
  EXPECT_EQ(8, bit_depth);
  EXPECT_EQ(2, color_type);  // RGB
}

TEST_F(PngOptimizerTest, GetAttributesFailsOnInvalidData) {
  // Passing garbage data to GetAttributes should return false without
  // crashing.
  reader_ = std::make_unique<PngReader>(&message_handler_);
  std::string garbage = "This is not a PNG file at all!";
  int width, height, bit_depth, color_type;
  EXPECT_FALSE(
      reader_->GetAttributes(garbage, &width, &height, &bit_depth, &color_type))
      << "GetAttributes should fail on non-PNG data";
}

// --- Edge Cases ---

TEST_F(PngOptimizerTest, EmptyInputRejected) {
  // An empty input should be rejected gracefully.
  reader_ = std::make_unique<PngReader>(&message_handler_);
  std::string empty_input;
  std::string out;
  EXPECT_FALSE(
      PngOptimizer::OptimizePng(*reader_, empty_input, &out, &message_handler_))
      << "OptimizePng should fail on empty input";
  EXPECT_FALSE(PngOptimizer::OptimizePngBestCompression(
      *reader_, empty_input, &out, &message_handler_))
      << "OptimizePngBestCompression should fail on empty input";

  // GetAttributes should also fail on empty input.
  int width, height, bit_depth, color_type;
  EXPECT_FALSE(reader_->GetAttributes(empty_input, &width, &height, &bit_depth,
                                      &color_type))
      << "GetAttributes should fail on empty input";
}

TEST_F(PngOptimizerTest, TruncatedPngRejected) {
  // A truncated PNG (just the first 10 bytes, which includes part of the
  // PNG signature but no complete chunks) should be rejected gracefully.
  reader_ = std::make_unique<PngReader>(&message_handler_);
  std::string in;
  ASSERT_TRUE(
      ReadTestFile(kPngSuiteTestDir, kValidImages[0].filename, "png", &in));
  ASSERT_GT(in.size(), 10u);

  std::string truncated = in.substr(0, 10);
  std::string out;
  EXPECT_FALSE(
      PngOptimizer::OptimizePng(*reader_, truncated, &out, &message_handler_))
      << "OptimizePng should fail on truncated PNG";
  EXPECT_FALSE(PngOptimizer::OptimizePngBestCompression(
      *reader_, truncated, &out, &message_handler_))
      << "OptimizePngBestCompression should fail on truncated PNG";
}

// ==================================================================
// Additional coverage: writer pixel formats, reader color types,
// and scanline reader edge cases
// ==================================================================

TEST_F(PngOptimizerTest, WriteGrayscaleImage) {
  // Write a 4x4 GRAY_8 image and verify it produces valid PNG output.
  auto writer = std::make_unique<PngScanlineWriter>(&message_handler_);
  std::string output;
  PngCompressParams params(PNG_FILTER_NONE, Z_DEFAULT_STRATEGY, false);
  ASSERT_TRUE(writer->Init(4, 4, GRAY_8));
  ASSERT_TRUE(writer->InitializeWrite(&params, &output));

  uint8_t row[4] = {0, 85, 170, 255};
  for (int y = 0; y < 4; ++y) {
    ASSERT_TRUE(writer->WriteNextScanline(reinterpret_cast<void*>(row)));
  }
  ASSERT_TRUE(writer->FinalizeWrite());
  EXPECT_GT(output.size(), 0u);

  // Verify the output is valid grayscale PNG.
  int w, h, bd, ct;
  reader_ = std::make_unique<PngReader>(&message_handler_);
  ASSERT_TRUE(reader_->GetAttributes(output, &w, &h, &bd, &ct));
  EXPECT_EQ(4, w);
  EXPECT_EQ(4, h);
  EXPECT_EQ(0, ct);  // PNG_COLOR_TYPE_GRAY
}

TEST_F(PngOptimizerTest, WriteRGBAImage) {
  // Write a 4x4 RGBA_8888 image and verify it produces valid PNG output.
  using pagespeed::image_compression::RGBA_8888;
  auto writer = std::make_unique<PngScanlineWriter>(&message_handler_);
  std::string output;
  PngCompressParams params(PNG_FILTER_NONE, Z_DEFAULT_STRATEGY, false);
  ASSERT_TRUE(writer->Init(4, 4, RGBA_8888));
  ASSERT_TRUE(writer->InitializeWrite(&params, &output));

  uint8_t row[16] = {255, 0, 0,   128, 0,   255, 0,   128,
                     0,   0, 255, 128, 128, 128, 128, 255};
  for (int y = 0; y < 4; ++y) {
    ASSERT_TRUE(writer->WriteNextScanline(reinterpret_cast<void*>(row)));
  }
  ASSERT_TRUE(writer->FinalizeWrite());
  EXPECT_GT(output.size(), 0u);

  int w, h, bd, ct;
  reader_ = std::make_unique<PngReader>(&message_handler_);
  ASSERT_TRUE(reader_->GetAttributes(output, &w, &h, &bd, &ct));
  EXPECT_EQ(4, w);
  EXPECT_EQ(4, h);
  // Should be RGBA (6) since alpha values are non-trivial.
  EXPECT_EQ(6, ct);
}

TEST_F(PngOptimizerTest, ScanlineReaderRawGrayAlpha) {
  // Gray+alpha images (color_type 4) should be readable.
  PngScanlineReaderRaw reader(&message_handler_);
  std::string in;
  ASSERT_TRUE(ReadTestFile(kPngSuiteTestDir, "basn4a08", "png", &in));
  // basn4a08 is gray+alpha, which PngScanlineReaderRaw may or may not
  // support. If it initializes successfully, verify we can read all rows.
  if (reader.Initialize(in.data(), in.length())) {
    EXPECT_EQ(reader.GetImageWidth(), 32u);
    EXPECT_EQ(reader.GetImageHeight(), 32u);
    int rows = 0;
    while (reader.HasMoreScanLines()) {
      void* scanline = nullptr;
      ASSERT_TRUE(reader.ReadNextScanline(&scanline));
      ASSERT_NE(scanline, nullptr);
      ++rows;
    }
    EXPECT_EQ(rows, 32);
  }
}

TEST_F(PngOptimizerTest, ScanlineReaderRawPalette) {
  // Palette images (color_type 3) should be expanded to RGB.
  PngScanlineReaderRaw reader(&message_handler_);
  std::string in;
  ASSERT_TRUE(ReadTestFile(kPngSuiteTestDir, "basn3p08", "png", &in));
  ASSERT_TRUE(reader.Initialize(in.data(), in.length()));
  EXPECT_EQ(reader.GetImageWidth(), 32u);
  EXPECT_EQ(reader.GetImageHeight(), 32u);
  // After expansion, palette should become RGB (3 bytes per pixel).
  EXPECT_EQ(reader.GetPixelFormat(), RGB_888);

  int rows = 0;
  while (reader.HasMoreScanLines()) {
    void* scanline = nullptr;
    ASSERT_TRUE(reader.ReadNextScanline(&scanline));
    ++rows;
  }
  EXPECT_EQ(rows, 32);
}

TEST_F(PngOptimizerTest, ScanlineReaderRaw16Bit) {
  // 16-bit images should be scaled down to 8-bit by the reader.
  PngScanlineReaderRaw reader(&message_handler_);
  std::string in;
  ASSERT_TRUE(ReadTestFile(kPngSuiteTestDir, "basn0g16", "png", &in));
  ASSERT_TRUE(reader.Initialize(in.data(), in.length()));
  EXPECT_EQ(reader.GetImageWidth(), 32u);
  EXPECT_EQ(reader.GetImageHeight(), 32u);
  // 16-bit grayscale should be scaled to 8-bit GRAY_8.
  EXPECT_EQ(reader.GetPixelFormat(), GRAY_8);

  int rows = 0;
  while (reader.HasMoreScanLines()) {
    void* scanline = nullptr;
    ASSERT_TRUE(reader.ReadNextScanline(&scanline));
    ++rows;
  }
  EXPECT_EQ(rows, 32);
}

TEST_F(PngOptimizerTest, ScanlineReaderRaw1Bit) {
  // 1-bit grayscale should be expanded to 8-bit.
  PngScanlineReaderRaw reader(&message_handler_);
  std::string in;
  ASSERT_TRUE(ReadTestFile(kPngSuiteTestDir, "basn0g01", "png", &in));
  ASSERT_TRUE(reader.Initialize(in.data(), in.length()));
  EXPECT_EQ(reader.GetImageWidth(), 32u);
  EXPECT_EQ(reader.GetPixelFormat(), GRAY_8);

  int rows = 0;
  while (reader.HasMoreScanLines()) {
    void* scanline = nullptr;
    ASSERT_TRUE(reader.ReadNextScanline(&scanline));
    ++rows;
  }
  EXPECT_EQ(rows, 32);
}

TEST_F(PngOptimizerTest, ScanlineReaderRawInterlaced) {
  // Interlaced PNGs should be deinterlaced by the reader.
  PngScanlineReaderRaw reader(&message_handler_);
  std::string in;
  ASSERT_TRUE(ReadTestFile(kPngSuiteTestDir, "basi2c08", "png", &in));
  ASSERT_TRUE(reader.Initialize(in.data(), in.length()));
  EXPECT_EQ(reader.GetImageWidth(), 32u);
  EXPECT_EQ(reader.GetImageHeight(), 32u);
  EXPECT_EQ(reader.GetPixelFormat(), RGB_888);

  int rows = 0;
  while (reader.HasMoreScanLines()) {
    void* scanline = nullptr;
    ASSERT_TRUE(reader.ReadNextScanline(&scanline));
    ++rows;
  }
  EXPECT_EQ(rows, 32);
}

TEST_F(PngOptimizerTest, ScanlineReaderCorruptData) {
  PngScanlineReaderRaw reader(&message_handler_);
  const char garbage[] = "not a png at all";
  EXPECT_FALSE(reader.Initialize(garbage, sizeof(garbage)));
}

TEST_F(PngOptimizerTest, GreyAlphaOpaqueStripped) {
  // A grayscale+alpha image where all alpha = 255 should have alpha stripped.
  reader_ = std::make_unique<PngReader>(&message_handler_);
  std::string in, out;
  ASSERT_TRUE(ReadTestFile(kPngSuiteTestDir, "grey_alpha_opaque", "png", &in));
  ASSERT_TRUE(PngOptimizer::OptimizePng(*reader_, in, &out, &message_handler_));
  EXPECT_GT(out.size(), 0u);

  int width, height, bit_depth, color_type;
  ASSERT_TRUE(
      reader_->GetAttributes(out, &width, &height, &bit_depth, &color_type));
  // Should NOT be gray+alpha (4) since alpha was all-opaque.
  EXPECT_NE(4, color_type);
}

TEST_F(PngOptimizerTest, WriterProgressiveMode) {
  // Write a progressive (interlaced) PNG.
  auto writer = std::make_unique<PngScanlineWriter>(&message_handler_);
  std::string output;
  PngCompressParams params(true /*best compression*/, true /*progressive*/);
  ASSERT_TRUE(writer->Init(4, 4, RGB_888));
  ASSERT_TRUE(writer->InitializeWrite(&params, &output));

  uint8_t row[12] = {255, 0, 0, 0, 255, 0, 0, 0, 255, 128, 128, 128};
  for (int y = 0; y < 4; ++y) {
    ASSERT_TRUE(writer->WriteNextScanline(reinterpret_cast<void*>(row)));
  }
  ASSERT_TRUE(writer->FinalizeWrite());
  EXPECT_GT(output.size(), 0u);

  // Verify the output is a valid PNG.
  int w, h, bd, ct;
  reader_ = std::make_unique<PngReader>(&message_handler_);
  ASSERT_TRUE(reader_->GetAttributes(output, &w, &h, &bd, &ct));
  EXPECT_EQ(4, w);
  EXPECT_EQ(4, h);
}

TEST_F(PngOptimizerTest, GetAttributesDifferentSizes) {
  // Verify GetAttributes for non-square images.
  reader_ = std::make_unique<PngReader>(&message_handler_);

  // 8x32 image
  std::string in;
  ASSERT_TRUE(ReadTestFile(kPngSuiteTestDir, "cdfn2c08", "png", &in));
  int w, h, bd, ct;
  ASSERT_TRUE(reader_->GetAttributes(in, &w, &h, &bd, &ct));
  EXPECT_EQ(8, w);
  EXPECT_EQ(32, h);

  // 32x8 image
  ASSERT_TRUE(ReadTestFile(kPngSuiteTestDir, "cdhn2c08", "png", &in));
  ASSERT_TRUE(reader_->GetAttributes(in, &w, &h, &bd, &ct));
  EXPECT_EQ(32, w);
  EXPECT_EQ(8, h);

  // 8x8 image
  ASSERT_TRUE(ReadTestFile(kPngSuiteTestDir, "cdsn2c08", "png", &in));
  ASSERT_TRUE(reader_->GetAttributes(in, &w, &h, &bd, &ct));
  EXPECT_EQ(8, w);
  EXPECT_EQ(8, h);
}

// ==================================================================
// Phase 4: Extended PNG coverage - writer paths, reader internals,
// opngreduc, and error handling
// ==================================================================

// --- PngScanlineWriter: round-trip through all pixel formats ---

TEST_F(PngScanlineWriterTest, RoundTripRGB) {
  // Write a 4x4 RGB image, read it back, verify pixel values match.
  auto writer = std::make_unique<PngScanlineWriter>(&message_handler_);
  std::string output;
  PngCompressParams params(PNG_FILTER_NONE, Z_DEFAULT_STRATEGY, false);
  ASSERT_TRUE(writer->Init(4, 4, RGB_888));
  ASSERT_TRUE(writer->InitializeWrite(&params, &output));

  uint8_t row[12] = {255, 0, 0, 0, 255, 0, 0, 0, 255, 128, 128, 128};
  for (int y = 0; y < 4; ++y) {
    ASSERT_TRUE(writer->WriteNextScanline(reinterpret_cast<void*>(row)));
  }
  ASSERT_TRUE(writer->FinalizeWrite());
  EXPECT_GT(output.size(), 0u);

  // Read back and verify dimensions and pixel format.
  PngScanlineReaderRaw reader(&message_handler_);
  ASSERT_TRUE(reader.Initialize(output.data(), output.length()));
  EXPECT_EQ(4u, reader.GetImageWidth());
  EXPECT_EQ(4u, reader.GetImageHeight());
  EXPECT_EQ(RGB_888, reader.GetPixelFormat());
  EXPECT_EQ(12u, reader.GetBytesPerScanline());

  int rows_read = 0;
  while (reader.HasMoreScanLines()) {
    void* scanline = nullptr;
    ASSERT_TRUE(reader.ReadNextScanline(&scanline));
    auto* pixels = static_cast<uint8_t*>(scanline);
    // All rows were written with the same data.
    for (int i = 0; i < 12; ++i) {
      EXPECT_EQ(row[i], pixels[i]) << "mismatch at byte " << i;
    }
    ++rows_read;
  }
  EXPECT_EQ(4, rows_read);
}

TEST_F(PngScanlineWriterTest, RoundTripRGBA) {
  // Write a 2x2 RGBA image, read it back.
  using pagespeed::image_compression::RGBA_8888;
  auto writer = std::make_unique<PngScanlineWriter>(&message_handler_);
  std::string output;
  PngCompressParams params(PNG_FILTER_NONE, Z_DEFAULT_STRATEGY, false);
  ASSERT_TRUE(writer->Init(2, 2, RGBA_8888));
  ASSERT_TRUE(writer->InitializeWrite(&params, &output));

  uint8_t row[8] = {255, 0, 0, 200, 0, 255, 0, 100};
  for (int y = 0; y < 2; ++y) {
    ASSERT_TRUE(writer->WriteNextScanline(reinterpret_cast<void*>(row)));
  }
  ASSERT_TRUE(writer->FinalizeWrite());
  EXPECT_GT(output.size(), 0u);

  PngScanlineReaderRaw reader(&message_handler_);
  ASSERT_TRUE(reader.Initialize(output.data(), output.length()));
  EXPECT_EQ(2u, reader.GetImageWidth());
  EXPECT_EQ(2u, reader.GetImageHeight());
  EXPECT_EQ(RGBA_8888, reader.GetPixelFormat());
  EXPECT_EQ(8u, reader.GetBytesPerScanline());

  int rows_read = 0;
  while (reader.HasMoreScanLines()) {
    void* scanline = nullptr;
    ASSERT_TRUE(reader.ReadNextScanline(&scanline));
    auto* pixels = static_cast<uint8_t*>(scanline);
    for (int i = 0; i < 8; ++i) {
      EXPECT_EQ(row[i], pixels[i]) << "mismatch at byte " << i;
    }
    ++rows_read;
  }
  EXPECT_EQ(2, rows_read);
}

TEST_F(PngScanlineWriterTest, RoundTripGrayscale) {
  // Write a 3x3 GRAY_8 image, read it back.
  auto writer = std::make_unique<PngScanlineWriter>(&message_handler_);
  std::string output;
  PngCompressParams params(PNG_FILTER_NONE, Z_DEFAULT_STRATEGY, false);
  ASSERT_TRUE(writer->Init(3, 3, GRAY_8));
  ASSERT_TRUE(writer->InitializeWrite(&params, &output));

  uint8_t row[3] = {0, 128, 255};
  for (int y = 0; y < 3; ++y) {
    ASSERT_TRUE(writer->WriteNextScanline(reinterpret_cast<void*>(row)));
  }
  ASSERT_TRUE(writer->FinalizeWrite());
  EXPECT_GT(output.size(), 0u);

  PngScanlineReaderRaw reader(&message_handler_);
  ASSERT_TRUE(reader.Initialize(output.data(), output.length()));
  EXPECT_EQ(3u, reader.GetImageWidth());
  EXPECT_EQ(3u, reader.GetImageHeight());
  EXPECT_EQ(GRAY_8, reader.GetPixelFormat());

  int rows_read = 0;
  while (reader.HasMoreScanLines()) {
    void* scanline = nullptr;
    ASSERT_TRUE(reader.ReadNextScanline(&scanline));
    auto* pixels = static_cast<uint8_t*>(scanline);
    for (int i = 0; i < 3; ++i) {
      EXPECT_EQ(row[i], pixels[i]);
    }
    ++rows_read;
  }
  EXPECT_EQ(3, rows_read);
}

// --- Writer: best compression path ---

TEST_F(PngScanlineWriterTest, BestCompressionProducesValidPng) {
  // Test the DoBestCompression() path in PngScanlineWriter via
  // the best_compression PngCompressParams.
  auto writer = std::make_unique<PngScanlineWriter>(&message_handler_);
  std::string output;
  PngCompressParams params(true /*best compression*/, false /*progressive*/);
  ASSERT_TRUE(writer->Init(8, 8, RGB_888));
  ASSERT_TRUE(writer->InitializeWrite(&params, &output));

  // Write a simple gradient.
  uint8_t row[24];
  for (int y = 0; y < 8; ++y) {
    for (int x = 0; x < 8; ++x) {
      row[x * 3 + 0] = static_cast<uint8_t>(y * 32);
      row[x * 3 + 1] = static_cast<uint8_t>(x * 32);
      row[x * 3 + 2] = 128;
    }
    ASSERT_TRUE(writer->WriteNextScanline(reinterpret_cast<void*>(row)));
  }
  ASSERT_TRUE(writer->FinalizeWrite());
  EXPECT_GT(output.size(), 0u);

  // Verify the output is a valid PNG.
  PngReader reader(&message_handler_);
  int w, h, bd, ct;
  ASSERT_TRUE(reader.GetAttributes(output, &w, &h, &bd, &ct));
  EXPECT_EQ(8, w);
  EXPECT_EQ(8, h);
}

TEST_F(PngScanlineWriterTest, BestCompressionProgressiveProducesValidPng) {
  // Test the DoBestCompression() path with progressive mode.
  auto writer = std::make_unique<PngScanlineWriter>(&message_handler_);
  std::string output;
  PngCompressParams params(true /*best compression*/, true /*progressive*/);
  ASSERT_TRUE(writer->Init(8, 8, GRAY_8));
  ASSERT_TRUE(writer->InitializeWrite(&params, &output));

  uint8_t row[8];
  for (int y = 0; y < 8; ++y) {
    for (int x = 0; x < 8; ++x) {
      row[x] = static_cast<uint8_t>((y + x) * 16);
    }
    ASSERT_TRUE(writer->WriteNextScanline(reinterpret_cast<void*>(row)));
  }
  ASSERT_TRUE(writer->FinalizeWrite());
  EXPECT_GT(output.size(), 0u);

  // Verify the output is a valid PNG.
  PngReader reader(&message_handler_);
  int w, h, bd, ct;
  ASSERT_TRUE(reader.GetAttributes(output, &w, &h, &bd, &ct));
  EXPECT_EQ(8, w);
  EXPECT_EQ(8, h);
}

// --- Writer: various compression strategies ---

TEST_F(PngScanlineWriterTest, AllFilterTypes) {
  // Test that all PNG filter types produce valid output.
  const int filters[] = {PNG_FILTER_NONE, PNG_FILTER_SUB,   PNG_FILTER_UP,
                         PNG_FILTER_AVG,  PNG_FILTER_PAETH, PNG_ALL_FILTERS};
  for (int filter : filters) {
    auto writer = std::make_unique<PngScanlineWriter>(&message_handler_);
    std::string output;
    PngCompressParams params(filter, Z_DEFAULT_STRATEGY, false);
    ASSERT_TRUE(writer->Init(4, 4, RGB_888));
    ASSERT_TRUE(writer->InitializeWrite(&params, &output));

    uint8_t row[12] = {255, 0, 0, 0, 255, 0, 0, 0, 255, 128, 128, 128};
    for (int y = 0; y < 4; ++y) {
      ASSERT_TRUE(writer->WriteNextScanline(reinterpret_cast<void*>(row)));
    }
    ASSERT_TRUE(writer->FinalizeWrite());
    EXPECT_GT(output.size(), 0u) << "filter=" << filter;

    PngReader reader(&message_handler_);
    int w, h, bd, ct;
    ASSERT_TRUE(reader.GetAttributes(output, &w, &h, &bd, &ct))
        << "filter=" << filter;
    EXPECT_EQ(4, w);
    EXPECT_EQ(4, h);
  }
}

TEST_F(PngScanlineWriterTest, AllCompressionStrategies) {
  // Test that all zlib compression strategies produce valid output.
  const int strategies[] = {Z_DEFAULT_STRATEGY, Z_FILTERED, Z_HUFFMAN_ONLY,
                            Z_RLE, Z_FIXED};
  for (int strategy : strategies) {
    auto writer = std::make_unique<PngScanlineWriter>(&message_handler_);
    std::string output;
    PngCompressParams params(PNG_FILTER_NONE, strategy, false);
    ASSERT_TRUE(writer->Init(4, 4, GRAY_8));
    ASSERT_TRUE(writer->InitializeWrite(&params, &output));

    uint8_t row[4] = {0, 85, 170, 255};
    for (int y = 0; y < 4; ++y) {
      ASSERT_TRUE(writer->WriteNextScanline(reinterpret_cast<void*>(row)));
    }
    ASSERT_TRUE(writer->FinalizeWrite());
    EXPECT_GT(output.size(), 0u) << "strategy=" << strategy;
  }
}

// --- PngScanlineReaderRaw: bit depth expansion ---

TEST_F(PngScanlineReaderRawTest, TwoBitGrayscaleExpanded) {
  // 2-bit grayscale should be expanded to 8-bit GRAY_8.
  PngScanlineReaderRaw reader(&message_handler_);
  std::string in;
  ASSERT_TRUE(ReadTestFile(kPngSuiteTestDir, "basn0g02", "png", &in));
  ASSERT_TRUE(reader.Initialize(in.data(), in.length()));
  EXPECT_EQ(32u, reader.GetImageWidth());
  EXPECT_EQ(32u, reader.GetImageHeight());
  EXPECT_EQ(GRAY_8, reader.GetPixelFormat());

  int rows = 0;
  while (reader.HasMoreScanLines()) {
    void* scanline = nullptr;
    ASSERT_TRUE(reader.ReadNextScanline(&scanline));
    ASSERT_NE(nullptr, scanline);
    ++rows;
  }
  EXPECT_EQ(32, rows);
}

TEST_F(PngScanlineReaderRawTest, FourBitGrayscaleExpanded) {
  // 4-bit grayscale should be expanded to 8-bit GRAY_8.
  PngScanlineReaderRaw reader(&message_handler_);
  std::string in;
  ASSERT_TRUE(ReadTestFile(kPngSuiteTestDir, "basn0g04", "png", &in));
  ASSERT_TRUE(reader.Initialize(in.data(), in.length()));
  EXPECT_EQ(32u, reader.GetImageWidth());
  EXPECT_EQ(32u, reader.GetImageHeight());
  EXPECT_EQ(GRAY_8, reader.GetPixelFormat());

  int rows = 0;
  while (reader.HasMoreScanLines()) {
    void* scanline = nullptr;
    ASSERT_TRUE(reader.ReadNextScanline(&scanline));
    ++rows;
  }
  EXPECT_EQ(32, rows);
}

TEST_F(PngScanlineReaderRawTest, SixteenBitRGBStrippedTo8Bit) {
  // 16-bit RGB should be stripped to 8-bit RGB.
  PngScanlineReaderRaw reader(&message_handler_);
  std::string in;
  ASSERT_TRUE(ReadTestFile(kPngSuiteTestDir, "basn2c16", "png", &in));
  ASSERT_TRUE(reader.Initialize(in.data(), in.length()));
  EXPECT_EQ(32u, reader.GetImageWidth());
  EXPECT_EQ(32u, reader.GetImageHeight());
  EXPECT_EQ(RGB_888, reader.GetPixelFormat());

  int rows = 0;
  while (reader.HasMoreScanLines()) {
    void* scanline = nullptr;
    ASSERT_TRUE(reader.ReadNextScanline(&scanline));
    ++rows;
  }
  EXPECT_EQ(32, rows);
}

TEST_F(PngScanlineReaderRawTest, PaletteWithTransparencyExpandedToRGBA) {
  // Palette images with tRNS chunk should be expanded to RGBA.
  PngScanlineReaderRaw reader(&message_handler_);
  std::string in;
  // tbbn3p08 has transparency in a palette image.
  ASSERT_TRUE(ReadTestFile(kPngSuiteTestDir, "tbbn3p08", "png", &in));
  ASSERT_TRUE(reader.Initialize(in.data(), in.length()));
  EXPECT_EQ(32u, reader.GetImageWidth());
  EXPECT_EQ(32u, reader.GetImageHeight());
  // After expansion, palette with tRNS should become RGBA.
  PixelFormat fmt = reader.GetPixelFormat();
  EXPECT_EQ(fmt, pagespeed::image_compression::RGBA_8888)
      << "Palette with tRNS must expand to RGBA_8888";

  int rows = 0;
  while (reader.HasMoreScanLines()) {
    void* scanline = nullptr;
    ASSERT_TRUE(reader.ReadNextScanline(&scanline));
    ++rows;
  }
  EXPECT_EQ(32, rows);
}

TEST_F(PngScanlineReaderRawTest, SixteenBitGrayAlphaExpandedToRGBA) {
  // 16-bit gray+alpha should be expanded to RGBA after strip_16 + gray_to_rgb.
  PngScanlineReaderRaw reader(&message_handler_);
  std::string in;
  ASSERT_TRUE(ReadTestFile(kPngSuiteTestDir, "basn4a16", "png", &in));
  ASSERT_TRUE(reader.Initialize(in.data(), in.length()));
  EXPECT_EQ(32u, reader.GetImageWidth());
  EXPECT_EQ(32u, reader.GetImageHeight());
  // Gray_Alpha is expanded to RGBA by the reader.
  EXPECT_EQ(pagespeed::image_compression::RGBA_8888, reader.GetPixelFormat());

  int rows = 0;
  while (reader.HasMoreScanLines()) {
    void* scanline = nullptr;
    ASSERT_TRUE(reader.ReadNextScanline(&scanline));
    ++rows;
  }
  EXPECT_EQ(32, rows);
}

TEST_F(PngScanlineReaderRawTest, IsProgressiveDetectsInterlaced) {
  // Interlaced PNGs should be detected as progressive.
  PngScanlineReaderRaw reader(&message_handler_);
  std::string in;
  // basi2c08 is an interlaced RGB image.
  ASSERT_TRUE(ReadTestFile(kPngSuiteTestDir, "basi2c08", "png", &in));
  ASSERT_TRUE(reader.Initialize(in.data(), in.length()));
  EXPECT_TRUE(reader.IsProgressive());

  // Non-interlaced should not be progressive.
  PngScanlineReaderRaw reader2(&message_handler_);
  std::string in2;
  ASSERT_TRUE(ReadTestFile(kPngSuiteTestDir, "basn2c08", "png", &in2));
  ASSERT_TRUE(reader2.Initialize(in2.data(), in2.length()));
  EXPECT_FALSE(reader2.IsProgressive());
}

TEST_F(PngScanlineReaderRawTest, InterlacedImageDecoded) {
  // Interlaced PNG should be fully decodable via the raw reader.
  PngScanlineReaderRaw reader(&message_handler_);
  std::string in;
  // basi0g08 is interlaced grayscale 8-bit.
  ASSERT_TRUE(ReadTestFile(kPngSuiteTestDir, "basi0g08", "png", &in));
  ASSERT_TRUE(reader.Initialize(in.data(), in.length()));
  EXPECT_TRUE(reader.IsProgressive());
  EXPECT_EQ(32u, reader.GetImageWidth());
  EXPECT_EQ(32u, reader.GetImageHeight());

  int rows = 0;
  while (reader.HasMoreScanLines()) {
    void* scanline = nullptr;
    ASSERT_TRUE(reader.ReadNextScanline(&scanline));
    ASSERT_NE(nullptr, scanline);
    ++rows;
  }
  EXPECT_EQ(32, rows);
}

TEST_F(PngScanlineReaderRawTest, InterlacedRGBADecoded) {
  // Interlaced RGBA should be fully decodable.
  PngScanlineReaderRaw reader(&message_handler_);
  std::string in;
  // basi6a08 is interlaced RGBA 8-bit.
  ASSERT_TRUE(ReadTestFile(kPngSuiteTestDir, "basi6a08", "png", &in));
  ASSERT_TRUE(reader.Initialize(in.data(), in.length()));
  EXPECT_TRUE(reader.IsProgressive());
  EXPECT_EQ(pagespeed::image_compression::RGBA_8888, reader.GetPixelFormat());

  int rows = 0;
  while (reader.HasMoreScanLines()) {
    void* scanline = nullptr;
    ASSERT_TRUE(reader.ReadNextScanline(&scanline));
    ++rows;
  }
  EXPECT_EQ(32, rows);
}

// --- PngScanlineReaderRaw: palette images of various bit depths ---

TEST_F(PngScanlineReaderRawTest, Palette1Bit) {
  PngScanlineReaderRaw reader(&message_handler_);
  std::string in;
  ASSERT_TRUE(ReadTestFile(kPngSuiteTestDir, "basn3p01", "png", &in));
  ASSERT_TRUE(reader.Initialize(in.data(), in.length()));
  EXPECT_EQ(32u, reader.GetImageWidth());
  EXPECT_EQ(RGB_888, reader.GetPixelFormat());
  int rows = 0;
  while (reader.HasMoreScanLines()) {
    void* scanline = nullptr;
    ASSERT_TRUE(reader.ReadNextScanline(&scanline));
    ++rows;
  }
  EXPECT_EQ(32, rows);
}

TEST_F(PngScanlineReaderRawTest, Palette2Bit) {
  PngScanlineReaderRaw reader(&message_handler_);
  std::string in;
  ASSERT_TRUE(ReadTestFile(kPngSuiteTestDir, "basn3p02", "png", &in));
  ASSERT_TRUE(reader.Initialize(in.data(), in.length()));
  EXPECT_EQ(32u, reader.GetImageWidth());
  EXPECT_EQ(RGB_888, reader.GetPixelFormat());
  int rows = 0;
  while (reader.HasMoreScanLines()) {
    void* scanline = nullptr;
    ASSERT_TRUE(reader.ReadNextScanline(&scanline));
    ++rows;
  }
  EXPECT_EQ(32, rows);
}

TEST_F(PngScanlineReaderRawTest, Palette4Bit) {
  PngScanlineReaderRaw reader(&message_handler_);
  std::string in;
  ASSERT_TRUE(ReadTestFile(kPngSuiteTestDir, "basn3p04", "png", &in));
  ASSERT_TRUE(reader.Initialize(in.data(), in.length()));
  EXPECT_EQ(32u, reader.GetImageWidth());
  EXPECT_EQ(RGB_888, reader.GetPixelFormat());
  int rows = 0;
  while (reader.HasMoreScanLines()) {
    void* scanline = nullptr;
    ASSERT_TRUE(reader.ReadNextScanline(&scanline));
    ++rows;
  }
  EXPECT_EQ(32, rows);
}

// --- PngOptimizer: palette optimization via opng_reduce ---

TEST_F(PngOptimizerTest, PaletteOptimization) {
  // Images with a large palette should be reduced by opng_reduce.
  reader_ = std::make_unique<PngReader>(&message_handler_);

  // basn3p08 has 256-color palette.
  std::string in, out;
  ASSERT_TRUE(ReadTestFile(kPngSuiteTestDir, "basn3p08", "png", &in));
  ASSERT_TRUE(PngOptimizer::OptimizePngBestCompression(*reader_, in, &out,
                                                       &message_handler_));
  EXPECT_GT(out.size(), 0u);

  // Verify the output is valid.
  int w, h, bd, ct;
  ASSERT_TRUE(reader_->GetAttributes(out, &w, &h, &bd, &ct));
  EXPECT_EQ(32, w);
  EXPECT_EQ(32, h);
}

TEST_F(PngOptimizerTest, TransparentPaletteOptimization) {
  // Palette image with transparency chunk should optimize correctly.
  reader_ = std::make_unique<PngReader>(&message_handler_);

  // tbbn3p08 has palette with transparency.
  std::string in, out;
  ASSERT_TRUE(ReadTestFile(kPngSuiteTestDir, "tbbn3p08", "png", &in));
  ASSERT_TRUE(PngOptimizer::OptimizePng(*reader_, in, &out, &message_handler_));
  EXPECT_GT(out.size(), 0u);

  int w, h, bd, ct;
  ASSERT_TRUE(reader_->GetAttributes(out, &w, &h, &bd, &ct));
  EXPECT_EQ(32, w);
  EXPECT_EQ(32, h);
}

// --- PngOptimizer: gamma preservation ---

TEST_F(PngOptimizerTest, GammaChunkPreserved) {
  // PNG files with gamma chunks should be optimized without losing the gamma.
  reader_ = std::make_unique<PngReader>(&message_handler_);

  // g03n0g16 has gamma 0.35.
  std::string in, out;
  ASSERT_TRUE(ReadTestFile(kPngSuiteTestDir, "g03n0g16", "png", &in));
  ASSERT_TRUE(PngOptimizer::OptimizePng(*reader_, in, &out, &message_handler_));
  EXPECT_GT(out.size(), 0u);

  // Test several gamma images to exercise the gamma copy path.
  const char* gamma_images[] = {"g04n0g16", "g05n0g16", "g07n0g16", "g10n0g16",
                                "g25n0g16"};
  for (const char* name : gamma_images) {
    std::string gin, gout;
    ASSERT_TRUE(ReadTestFile(kPngSuiteTestDir, name, "png", &gin)) << name;
    ASSERT_TRUE(
        PngOptimizer::OptimizePng(*reader_, gin, &gout, &message_handler_))
        << name;
    EXPECT_GT(gout.size(), 0u) << name;
  }
}

// --- PngScanlineReader: direct API ---

TEST(PngScanlineReaderDirectTest, GetBytesPerScanlineRGB) {
  NullMessageHandler handler;
  PngScanlineReader reader(&handler);
  PngReader png_reader(&handler);
  if (setjmp(*reader.GetJmpBuf())) {
    FAIL() << "libpng error";
  }

  std::string in;
  ASSERT_TRUE(ReadTestFile(kPngSuiteTestDir, "basn2c08", "png", &in));
  reader.set_transform(PNG_TRANSFORM_IDENTITY);
  ASSERT_TRUE(reader.InitializeRead(png_reader, in));

  EXPECT_EQ(32u, reader.GetImageWidth());
  EXPECT_EQ(32u, reader.GetImageHeight());
  // RGB 8-bit: 3 bytes per pixel * 32 pixels = 96.
  EXPECT_EQ(96u, reader.GetBytesPerScanline());
  EXPECT_FALSE(reader.IsProgressive());

  // Read all scanlines.
  int rows = 0;
  while (reader.HasMoreScanLines()) {
    void* scanline = nullptr;
    ASSERT_TRUE(reader.ReadNextScanline(&scanline));
    ++rows;
  }
  EXPECT_EQ(32, rows);
}

TEST(PngScanlineReaderDirectTest, InterlacedImageIsProgressive) {
  NullMessageHandler handler;
  PngScanlineReader reader(&handler);
  PngReader png_reader(&handler);
  if (setjmp(*reader.GetJmpBuf())) {
    FAIL() << "libpng error";
  }

  std::string in;
  ASSERT_TRUE(ReadTestFile(kPngSuiteTestDir, "basi2c08", "png", &in));
  reader.set_transform(PNG_TRANSFORM_EXPAND | PNG_TRANSFORM_STRIP_16);
  ASSERT_TRUE(reader.InitializeRead(png_reader, in));

  EXPECT_TRUE(reader.IsProgressive());
}

TEST(PngScanlineReaderDirectTest, GetPixelFormatGray) {
  NullMessageHandler handler;
  PngScanlineReader reader(&handler);
  PngReader png_reader(&handler);
  if (setjmp(*reader.GetJmpBuf())) {
    FAIL() << "libpng error";
  }

  std::string in;
  ASSERT_TRUE(ReadTestFile(kPngSuiteTestDir, "basn0g08", "png", &in));
  reader.set_transform(PNG_TRANSFORM_IDENTITY);
  ASSERT_TRUE(reader.InitializeRead(png_reader, in));

  EXPECT_EQ(GRAY_8, reader.GetPixelFormat());
  EXPECT_EQ(32u, reader.GetBytesPerScanline());
}

TEST(PngScanlineReaderDirectTest, GetPixelFormatRGBA) {
  NullMessageHandler handler;
  PngScanlineReader reader(&handler);
  PngReader png_reader(&handler);
  if (setjmp(*reader.GetJmpBuf())) {
    FAIL() << "libpng error";
  }

  std::string in;
  ASSERT_TRUE(ReadTestFile(kPngSuiteTestDir, "basn6a08", "png", &in));
  reader.set_transform(PNG_TRANSFORM_IDENTITY);
  ASSERT_TRUE(reader.InitializeRead(png_reader, in));

  EXPECT_EQ(pagespeed::image_compression::RGBA_8888, reader.GetPixelFormat());
  // 4 channels * 32 pixels = 128 bytes per scanline.
  EXPECT_EQ(128u, reader.GetBytesPerScanline());
}

// --- PngScanlineReader: opaque detection in InitializeRead ---

TEST(PngScanlineReaderDirectTest, OpaqueAlphaDetectedAndStripped) {
  // When not requiring opaque, the reader should detect opaque alpha
  // and strip it, reporting is_opaque=true.
  NullMessageHandler handler;
  PngScanlineReader reader(&handler);
  PngReader png_reader(&handler);
  if (setjmp(*reader.GetJmpBuf())) {
    FAIL() << "libpng error";
  }

  std::string in;
  // rgba_opaque is an RGBA image where all alpha values are 255.
  ASSERT_TRUE(ReadTestFile(kPngSuiteTestDir, "rgba_opaque", "png", &in));
  reader.set_transform(PNG_TRANSFORM_EXPAND | PNG_TRANSFORM_STRIP_16);
  bool is_opaque = false;
  ASSERT_TRUE(reader.InitializeRead(png_reader, in, &is_opaque));
  EXPECT_TRUE(is_opaque);

  // The reader should have stripped the alpha, resulting in RGB.
  int ct = reader.GetColorType();
  EXPECT_EQ(ct, PNG_COLOR_TYPE_RGB)
      << "Opaque RGBA should be stripped to RGB, got color_type=" << ct;
}

// --- Writer: re-initialization ---

TEST_F(PngScanlineWriterTest, ReinitializeAndWrite) {
  // Initialize, write one line, re-initialize, then write full image.
  ASSERT_TRUE(Initialize());
  ASSERT_TRUE(writer_->WriteNextScanline(reinterpret_cast<void*>(scanline_)));

  // Re-initialize.
  ASSERT_TRUE(Initialize());
  ASSERT_TRUE(writer_->WriteNextScanline(reinterpret_cast<void*>(scanline_)));
  ASSERT_TRUE(writer_->WriteNextScanline(reinterpret_cast<void*>(scanline_)));
  ASSERT_TRUE(writer_->FinalizeWrite());
  EXPECT_GT(output_.size(), 0u);

  // Verify the output is a valid PNG.
  PngReader reader(&message_handler_);
  int w, h, bd, ct;
  ASSERT_TRUE(reader.GetAttributes(output_, &w, &h, &bd, &ct));
  EXPECT_EQ(3, w);
  EXPECT_EQ(2, h);
}

// --- PngOptimizer: various small image sizes ---

TEST_F(PngOptimizerTest, SmallSquareImages) {
  // Test optimization of tiny square images (1x1 through 9x9).
  reader_ = std::make_unique<PngReader>(&message_handler_);
  const char* small_images[] = {"s01n3p01", "s02n3p01", "s03n3p01",
                                "s04n3p01", "s05n3p02", "s06n3p02",
                                "s07n3p02", "s08n3p02", "s09n3p02"};
  for (const char* name : small_images) {
    std::string in, out;
    ASSERT_TRUE(ReadTestFile(kPngSuiteTestDir, name, "png", &in)) << name;
    ASSERT_TRUE(
        PngOptimizer::OptimizePng(*reader_, in, &out, &message_handler_))
        << name;
    EXPECT_GT(out.size(), 0u) << name;
  }
}

// --- PngOptimizer: interlaced with best compression ---

TEST_F(PngOptimizerTest, InterlacedBestCompression) {
  // Interlaced PNGs should be reducible with best compression.
  reader_ = std::make_unique<PngReader>(&message_handler_);
  const char* interlaced[] = {"basi0g01", "basi2c08", "basi3p08", "basi4a08",
                              "basi6a08"};
  for (const char* name : interlaced) {
    std::string in, out;
    ASSERT_TRUE(ReadTestFile(kPngSuiteTestDir, name, "png", &in)) << name;
    ASSERT_TRUE(PngOptimizer::OptimizePngBestCompression(*reader_, in, &out,
                                                         &message_handler_))
        << name;
    EXPECT_GT(out.size(), 0u) << name;
  }
}

// --- PngReader: ReadPng with transforms ---

TEST(PngReaderTransformTest, ExpandAndStrip16) {
  NullMessageHandler handler;
  PngReader reader(&handler);
  ScopedPngStruct read(ScopedPngStruct::READ, &handler);

  // 16-bit RGB image: expand + strip_16 should give 8-bit RGB.
  std::string in;
  ASSERT_TRUE(ReadTestFile(kPngSuiteTestDir, "basn2c16", "png", &in));
  ASSERT_TRUE(reader.ReadPng(in, read.png_ptr(), read.info_ptr(),
                             PNG_TRANSFORM_EXPAND | PNG_TRANSFORM_STRIP_16,
                             false));
  int ct = png_get_color_type(read.png_ptr(), read.info_ptr());
  EXPECT_EQ(PNG_COLOR_TYPE_RGB, ct);
  int bd = png_get_bit_depth(read.png_ptr(), read.info_ptr());
  EXPECT_EQ(8, bd);
}

TEST(PngReaderTransformTest, StripAlphaOnTransparent) {
  NullMessageHandler handler;
  PngReader reader(&handler);
  ScopedPngStruct read(ScopedPngStruct::READ, &handler);

  // basn6a08 is RGBA; stripping alpha should give RGB.
  std::string in;
  ASSERT_TRUE(ReadTestFile(kPngSuiteTestDir, "basn6a08", "png", &in));
  ASSERT_TRUE(reader.ReadPng(in, read.png_ptr(), read.info_ptr(),
                             PNG_TRANSFORM_STRIP_ALPHA, false));
  int ct = png_get_color_type(read.png_ptr(), read.info_ptr());
  EXPECT_EQ(PNG_COLOR_TYPE_RGB, ct);
}

// --- GetAttributes: corrupted IHDR CRC ---

TEST_F(PngOptimizerTest, GetAttributesCorruptedIHDRCRC) {
  // If the IHDR chunk CRC is corrupted, GetAttributes should fail.
  reader_ = std::make_unique<PngReader>(&message_handler_);
  std::string in;
  ASSERT_TRUE(ReadTestFile(kPngSuiteTestDir, "basn0g08", "png", &in));

  // Corrupt the CRC (last 4 bytes of the first chunk).
  // PNG header: 8 byte sig + 4 byte len + 4 byte name + 13 byte IHDR + 4 byte CRC.
  // CRC starts at offset 8+4+4+13 = 29.
  ASSERT_GT(in.size(), 33u);
  std::string corrupted = in;
  corrupted[29] ^= 0xFF;
  corrupted[30] ^= 0xFF;

  int w, h, bd, ct;
  EXPECT_FALSE(reader_->GetAttributes(corrupted, &w, &h, &bd, &ct));
}

// --- GetAttributes: truncated just before CRC ---

TEST_F(PngOptimizerTest, GetAttributesTruncatedBeforeCRC) {
  reader_ = std::make_unique<PngReader>(&message_handler_);
  std::string in;
  ASSERT_TRUE(ReadTestFile(kPngSuiteTestDir, "basn0g08", "png", &in));
  ASSERT_GT(in.size(), 29u);

  // Truncate to exactly 29 bytes (missing CRC entirely).
  std::string truncated = in.substr(0, 29);
  int w, h, bd, ct;
  EXPECT_FALSE(reader_->GetAttributes(truncated, &w, &h, &bd, &ct));
}

// --- GetAttributes: wrong IHDR chunk length ---

TEST_F(PngOptimizerTest, GetAttributesBadIHDRLength) {
  reader_ = std::make_unique<PngReader>(&message_handler_);
  std::string in;
  ASSERT_TRUE(ReadTestFile(kPngSuiteTestDir, "basn0g08", "png", &in));
  ASSERT_GT(in.size(), 33u);

  // Corrupt the chunk length (at offset 8) so it doesn't equal 13.
  std::string corrupted = in;
  corrupted[8] = 0;
  corrupted[9] = 0;
  corrupted[10] = 0;
  corrupted[11] = 99;

  int w, h, bd, ct;
  EXPECT_FALSE(reader_->GetAttributes(corrupted, &w, &h, &bd, &ct));
}

// --- GetAttributes: bad PNG signature ---

TEST_F(PngOptimizerTest, GetAttributesBadSignature) {
  reader_ = std::make_unique<PngReader>(&message_handler_);
  std::string in;
  ASSERT_TRUE(ReadTestFile(kPngSuiteTestDir, "basn0g08", "png", &in));
  ASSERT_GT(in.size(), 33u);

  // Corrupt the PNG signature.
  std::string corrupted = in;
  corrupted[0] = 'X';

  int w, h, bd, ct;
  EXPECT_FALSE(reader_->GetAttributes(corrupted, &w, &h, &bd, &ct));
}

// ============================================================
// Coverage Batch: PNG Writer error paths, GetBackgroundColor,
// Reset, FinalizeWrite, validation, best compression
// ============================================================

// --- PngScanlineWriter: Init with invalid dimensions ---

TEST_F(PngScanlineWriterTest, InitZeroDimensions) {
  auto writer = std::make_unique<PngScanlineWriter>(&message_handler_);
  // Width = 0 should fail at Init().
  EXPECT_FALSE(writer->Init(0, 10, GRAY_8));
}

TEST_F(PngScanlineWriterTest, InitZeroHeight) {
  auto writer = std::make_unique<PngScanlineWriter>(&message_handler_);
  // Height = 0 should fail at Init().
  EXPECT_FALSE(writer->Init(10, 0, GRAY_8));
}

// --- PngScanlineWriter: Init with unsupported pixel format ---

TEST_F(PngScanlineWriterTest, InitUnsupportedPixelFormat) {
  auto writer = std::make_unique<PngScanlineWriter>(&message_handler_);
  // UNSUPPORTED is not a valid pixel format for writing.
  EXPECT_FALSE(writer->Init(10, 10, pagespeed::image_compression::UNSUPPORTED));
}

// --- PngScanlineWriter: Validate with bad filter level ---

TEST_F(PngScanlineWriterTest, ValidateBadFilterLevel) {
  auto writer = std::make_unique<PngScanlineWriter>(&message_handler_);
  ASSERT_TRUE(writer->Init(3, 2, GRAY_8));
  std::string output;
  // Invalid filter level (0xFF has bits outside PNG_ALL_FILTERS).
  // Validate logs DFATAL but doesn't return false for filter_level alone.
  PngCompressParams bad_params(0xFF, Z_DEFAULT_STRATEGY, false);
  // The filter level warning is logged but doesn't fail — it proceeds.
  // But an invalid compression_strategy DOES fail.
  PngCompressParams bad_strategy(PNG_FILTER_NONE, 99, false);
  EXPECT_FALSE(writer->InitializeWrite(&bad_strategy, &output));
}

// --- PngScanlineWriter: WriteNextScanline after all rows written ---

TEST_F(PngScanlineWriterTest, WriteNextScanlinePastEnd) {
  ASSERT_TRUE(Initialize());
  // Write all rows.
  memset(scanline_, 0x80, sizeof(scanline_));
  for (int i = 0; i < height_; ++i) {
    ASSERT_TRUE(writer_->WriteNextScanline(scanline_));
  }
  // Extra write should fail.
  EXPECT_FALSE(writer_->WriteNextScanline(scanline_));
}

// --- PngScanlineWriter: FinalizeWrite without writing all rows ---

TEST_F(PngScanlineWriterTest, FinalizeWriteIncomplete) {
  ASSERT_TRUE(Initialize());
  // Write only 1 of 2 rows, then finalize.
  memset(scanline_, 0x80, sizeof(scanline_));
  ASSERT_TRUE(writer_->WriteNextScanline(scanline_));
  // FinalizeWrite should fail (row_ != height_).
  EXPECT_FALSE(writer_->FinalizeWrite());
}

// --- PngScanlineWriter: FinalizeWrite without initialization ---

TEST_F(PngScanlineWriterTest, FinalizeWriteWithoutInit) {
  auto writer = std::make_unique<PngScanlineWriter>(&message_handler_);
  // Never called Init/InitializeWrite. was_initialized_ is false.
  EXPECT_FALSE(writer->FinalizeWrite());
}

// --- PngScanlineWriter: Reset after initialization ---

TEST_F(PngScanlineWriterTest, ResetViaReinitAfterPartialWrite) {
  ASSERT_TRUE(Initialize());
  // Write one row (partial).
  memset(scanline_, 0x80, sizeof(scanline_));
  ASSERT_TRUE(writer_->WriteNextScanline(scanline_));
  // Re-init triggers internal Reset(). Should succeed and produce valid output.
  ASSERT_TRUE(writer_->Init(width_, height_, pixel_format_));
  std::string output2;
  ASSERT_TRUE(writer_->InitializeWrite(&params_, &output2));
  for (int i = 0; i < height_; ++i) {
    ASSERT_TRUE(writer_->WriteNextScanline(scanline_));
  }
  ASSERT_TRUE(writer_->FinalizeWrite());
  EXPECT_FALSE(output2.empty());
}

// --- PngScanlineWriter: Re-initialize after previous Init ---

TEST_F(PngScanlineWriterTest, ReinitializeWriter) {
  ASSERT_TRUE(Initialize());
  // Write all rows and finalize.
  memset(scanline_, 0x80, sizeof(scanline_));
  for (int i = 0; i < height_; ++i) {
    ASSERT_TRUE(writer_->WriteNextScanline(scanline_));
  }
  ASSERT_TRUE(writer_->FinalizeWrite());
  EXPECT_FALSE(output_.empty());

  // Re-initialize (exercises the Reset-on-reinit path in Init()).
  ASSERT_TRUE(writer_->Init(width_, height_, pixel_format_));
  std::string output2;
  ASSERT_TRUE(writer_->InitializeWrite(&params_, &output2));
  for (int i = 0; i < height_; ++i) {
    ASSERT_TRUE(writer_->WriteNextScanline(scanline_));
  }
  ASSERT_TRUE(writer_->FinalizeWrite());
  EXPECT_FALSE(output2.empty());
}

// --- PngScanlineWriter: Best compression path ---

TEST_F(PngScanlineWriterTest, BestCompressionRoundtrip) {
  PngCompressParams best_params(true, false);
  auto writer = std::make_unique<PngScanlineWriter>(&message_handler_);
  ASSERT_TRUE(writer->Init(width_, height_, pixel_format_));
  std::string output;
  ASSERT_TRUE(writer->InitializeWrite(&best_params, &output));
  unsigned char scanline[3];
  memset(scanline, 0x42, sizeof(scanline));
  for (int i = 0; i < height_; ++i) {
    ASSERT_TRUE(writer->WriteNextScanline(scanline));
  }
  ASSERT_TRUE(writer->FinalizeWrite());
  EXPECT_FALSE(output.empty());

  // Verify result is valid PNG by reading it back.
  PngScanlineReaderRaw reader(&message_handler_);
  ASSERT_TRUE(reader.Initialize(output.data(), output.length()));
  EXPECT_EQ(static_cast<size_t>(width_), reader.GetImageWidth());
  EXPECT_EQ(static_cast<size_t>(height_), reader.GetImageHeight());
}

// --- PngScanlineWriter: Progressive (interlaced) output ---

TEST_F(PngScanlineWriterTest, ProgressiveOutput) {
  PngCompressParams prog_params(PNG_FILTER_NONE, Z_DEFAULT_STRATEGY, true);
  auto writer = std::make_unique<PngScanlineWriter>(&message_handler_);
  ASSERT_TRUE(writer->Init(width_, height_, pixel_format_));
  std::string output;
  ASSERT_TRUE(writer->InitializeWrite(&prog_params, &output));
  unsigned char scanline[3];
  memset(scanline, 0x99, sizeof(scanline));
  for (int i = 0; i < height_; ++i) {
    ASSERT_TRUE(writer->WriteNextScanline(scanline));
  }
  ASSERT_TRUE(writer->FinalizeWrite());
  EXPECT_FALSE(output.empty());
}

// --- PngScanlineWriter: RGBA_8888 output ---

TEST_F(PngScanlineWriterTest, WriteRgba) {
  auto writer = std::make_unique<PngScanlineWriter>(&message_handler_);
  const int w = 2, h = 2;
  ASSERT_TRUE(writer->Init(w, h, pagespeed::image_compression::RGBA_8888));
  std::string output;
  PngCompressParams params(PNG_FILTER_NONE, Z_DEFAULT_STRATEGY, false);
  ASSERT_TRUE(writer->InitializeWrite(&params, &output));
  unsigned char scanline[8] = {0xFF, 0x00, 0x00, 0xFF, 0x00, 0xFF, 0x00, 0xFF};
  for (int i = 0; i < h; ++i) {
    ASSERT_TRUE(writer->WriteNextScanline(scanline));
  }
  ASSERT_TRUE(writer->FinalizeWrite());
  EXPECT_FALSE(output.empty());
}

// --- PngScanlineWriter: RGB_888 output ---

TEST_F(PngScanlineWriterTest, WriteRgb) {
  auto writer = std::make_unique<PngScanlineWriter>(&message_handler_);
  const int w = 2, h = 2;
  ASSERT_TRUE(writer->Init(w, h, RGB_888));
  std::string output;
  PngCompressParams params(PNG_FILTER_NONE, Z_DEFAULT_STRATEGY, false);
  ASSERT_TRUE(writer->InitializeWrite(&params, &output));
  unsigned char scanline[6] = {0xFF, 0x00, 0x00, 0x00, 0xFF, 0x00};
  for (int i = 0; i < h; ++i) {
    ASSERT_TRUE(writer->WriteNextScanline(scanline));
  }
  ASSERT_TRUE(writer->FinalizeWrite());
  EXPECT_FALSE(output.empty());
}

// --- GetBackgroundColor: 8-bit RGB with bKGD chunk ---
// Helper to create PNGs with bKGD chunk programmatically via libpng.

void TestWritePngCallback(png_structp write_ptr, png_bytep data,
                          png_size_t length) {
  auto* out = static_cast<std::string*>(png_get_io_ptr(write_ptr));
  out->append(reinterpret_cast<char*>(data), length);
}

std::string CreatePngWithBkgd(int bit_depth, int color_type,
                              const png_color_16& bg, int width = 1,
                              int height = 1) {
  std::string result;
  png_structp write_ptr =
      png_create_write_struct(PNG_LIBPNG_VER_STRING, nullptr, nullptr, nullptr);
  png_infop info_ptr = png_create_info_struct(write_ptr);

  if (setjmp(png_jmpbuf(write_ptr))) {
    png_destroy_write_struct(&write_ptr, &info_ptr);
    return "";
  }

  png_set_write_fn(write_ptr, &result, TestWritePngCallback, nullptr);
  png_set_IHDR(write_ptr, info_ptr, width, height, bit_depth, color_type,
               PNG_INTERLACE_NONE, PNG_COMPRESSION_TYPE_DEFAULT,
               PNG_FILTER_TYPE_DEFAULT);

  png_color_16 bg_copy = bg;
  png_set_bKGD(write_ptr, info_ptr, &bg_copy);

  png_write_info(write_ptr, info_ptr);
  int row_bytes = png_get_rowbytes(write_ptr, info_ptr);
  std::vector<unsigned char> row(row_bytes, 0);
  for (int y = 0; y < height; ++y) {
    png_write_row(write_ptr, row.data());
  }
  png_write_end(write_ptr, info_ptr);
  png_destroy_write_struct(&write_ptr, &info_ptr);
  return result;
}

TEST(PngBackgroundColorTest, Get8bitBackgroundColor) {
  NullMessageHandler handler;

  png_color_16 bg;
  memset(&bg, 0, sizeof(bg));
  bg.red = 200;
  bg.green = 100;
  bg.blue = 50;
  std::string png_data = CreatePngWithBkgd(8, PNG_COLOR_TYPE_RGB, bg);
  ASSERT_FALSE(png_data.empty());

  PngReader reader(&handler);
  PngScanlineReader scanline_reader(&handler);
  if (setjmp(*scanline_reader.GetJmpBuf()) != 0) {
    FAIL() << "libpng error";
  }
  ASSERT_TRUE(scanline_reader.InitializeRead(reader, png_data));

  unsigned char r = 0, g = 0, b = 0;
  EXPECT_TRUE(scanline_reader.GetBackgroundColor(&r, &g, &b));
  EXPECT_EQ(200, r);
  EXPECT_EQ(100, g);
  EXPECT_EQ(50, b);
}

// --- GetBackgroundColor: 16-bit image with bKGD ---

TEST(PngBackgroundColorTest, Get16bitBackgroundColor) {
  NullMessageHandler handler;

  png_color_16 bg;
  memset(&bg, 0, sizeof(bg));
  bg.red = 0xAB00;    // >> 8 = 0xAB = 171
  bg.green = 0xCD00;  // >> 8 = 0xCD = 205
  bg.blue = 0xEF00;   // >> 8 = 0xEF = 239
  std::string png_data = CreatePngWithBkgd(16, PNG_COLOR_TYPE_RGB, bg);
  ASSERT_FALSE(png_data.empty());

  PngReader reader(&handler);
  PngScanlineReader scanline_reader(&handler);
  if (setjmp(*scanline_reader.GetJmpBuf()) != 0) {
    FAIL() << "libpng error";
  }
  ASSERT_TRUE(scanline_reader.InitializeRead(reader, png_data));

  unsigned char r = 0, g = 0, b = 0;
  EXPECT_TRUE(scanline_reader.GetBackgroundColor(&r, &g, &b));
  EXPECT_EQ(0xAB, r);
  EXPECT_EQ(0xCD, g);
  EXPECT_EQ(0xEF, b);
}

// --- GetBackgroundColor: grayscale <8-bit with bKGD ---

TEST(PngBackgroundColorTest, GetLowBitDepthGrayBackgroundColor) {
  NullMessageHandler handler;

  png_color_16 bg;
  memset(&bg, 0, sizeof(bg));
  bg.gray = 3;  // Small value to avoid overflow in upsampling formula.
  std::string png_data = CreatePngWithBkgd(4, PNG_COLOR_TYPE_GRAY, bg);
  ASSERT_FALSE(png_data.empty());

  PngReader reader(&handler);
  PngScanlineReader scanline_reader(&handler);
  if (setjmp(*scanline_reader.GetJmpBuf()) != 0) {
    FAIL() << "libpng error";
  }
  ASSERT_TRUE(scanline_reader.InitializeRead(reader, png_data));

  unsigned char r = 0, g = 0, b = 0;
  EXPECT_TRUE(scanline_reader.GetBackgroundColor(&r, &g, &b));
  // 4-bit: scale = 255 / ((4 << 1) - 1) = 255 / 7 = 36
  // gray_8bit = 3 * 36 = 108
  EXPECT_EQ(108, r);
  EXPECT_EQ(r, g);
  EXPECT_EQ(g, b);
}

// --- GetBackgroundColor: no bKGD chunk ---

TEST(PngBackgroundColorTest, NoBkgdChunk) {
  NullMessageHandler handler;
  std::string image;
  ASSERT_TRUE(ReadTestFile(kPngSuiteTestDir, "basn0g08", "png", &image));

  PngReader reader(&handler);
  PngScanlineReader scanline_reader(&handler);
  if (setjmp(*scanline_reader.GetJmpBuf()) != 0) {
    FAIL() << "libpng error";
  }
  ASSERT_TRUE(scanline_reader.InitializeRead(reader, image));

  unsigned char r, g, b;
  // No bKGD chunk -> should return false.
  EXPECT_FALSE(scanline_reader.GetBackgroundColor(&r, &g, &b));
}

// --- PngScanlineReader: InitializeWithStatus is a no-op error ---

TEST(PngScanlineReaderTest, InitializeWithStatusReturnsError) {
  NullMessageHandler handler;
  PngScanlineReader reader(&handler);
  ScanlineStatus status = reader.InitializeWithStatus(nullptr, 0);
  EXPECT_FALSE(status.Success());
}

// --- ScopedPngStruct: reset (WRITE) ---

TEST(ScopedPngStructTest, ResetWriteStruct) {
  NullMessageHandler handler;
  ScopedPngStruct png(ScopedPngStruct::WRITE, &handler);
  ASSERT_TRUE(png.valid());
  // Reset destroys and recreates the write struct.
  EXPECT_TRUE(png.reset());
  // Should still be valid after reset.
  EXPECT_TRUE(png.valid());
}

// --- ScopedPngStruct: reset (READ) ---

TEST(ScopedPngStructTest, ResetReadStruct) {
  NullMessageHandler handler;
  ScopedPngStruct png(ScopedPngStruct::READ, &handler);
  ASSERT_TRUE(png.valid());
  EXPECT_TRUE(png.reset());
  EXPECT_TRUE(png.valid());
}

// ============================================================
// Additional coverage: PngScanlineReaderRaw::Reset, corrupt PNG,
// GRAY_8 round-trip verification
// ============================================================

// --- PngScanlineReaderRaw::Reset ---

TEST_F(PngScanlineReaderRawTest, ResetAfterInitialize) {
  // Initialize a PngScanlineReaderRaw with a valid PNG, then call Reset().
  PngScanlineReaderRaw reader(&message_handler_);
  std::string in;
  ASSERT_TRUE(ReadTestFile(kPngSuiteTestDir, "basn2c08", "png", &in));
  ASSERT_TRUE(reader.Initialize(in.data(), in.length()));

  // Read a few scanlines to advance state.
  void* scanline = nullptr;
  ASSERT_TRUE(reader.ReadNextScanline(&scanline));
  ASSERT_TRUE(reader.ReadNextScanline(&scanline));

  // Reset should succeed.
  EXPECT_TRUE(reader.Reset());

  // After reset, re-initialize and verify we can read again from scratch.
  ASSERT_TRUE(reader.Initialize(in.data(), in.length()));
  EXPECT_EQ(32u, reader.GetImageWidth());
  EXPECT_EQ(32u, reader.GetImageHeight());

  int rows = 0;
  while (reader.HasMoreScanLines()) {
    ASSERT_TRUE(reader.ReadNextScanline(&scanline));
    ++rows;
  }
  EXPECT_EQ(32, rows);
}

// --- PngScanlineReaderRaw with corrupt (truncated body) PNG ---

TEST_F(PngScanlineReaderRawTest, CorruptTruncatedBody) {
  // Read a valid PNG, then truncate it so the header is intact but
  // the compressed data is incomplete.
  PngScanlineReaderRaw reader(&message_handler_);
  std::string in;
  ASSERT_TRUE(ReadTestFile(kPngSuiteTestDir, "basn2c08", "png", &in));
  ASSERT_GT(in.size(), 50u);

  // Truncate to 50 bytes: enough for signature + IHDR but not the full IDAT.
  std::string truncated = in.substr(0, 50);
  EXPECT_FALSE(reader.Initialize(truncated.data(), truncated.length()));
}

// --- GRAY_8 write and round-trip pixel verification ---

TEST_F(PngScanlineWriterTest, RoundTripGrayscalePixelVerification) {
  // Write a 4x4 GRAY_8 image with known pixel values, read it back,
  // and verify each pixel matches exactly.
  auto writer = std::make_unique<PngScanlineWriter>(&message_handler_);
  std::string output;
  PngCompressParams params(PNG_FILTER_NONE, Z_DEFAULT_STRATEGY, false);
  ASSERT_TRUE(writer->Init(4, 4, GRAY_8));
  ASSERT_TRUE(writer->InitializeWrite(&params, &output));

  // Each row has distinct pixel values.
  uint8_t rows[4][4] = {
      {0, 64, 128, 255},
      {10, 20, 30, 40},
      {200, 201, 202, 203},
      {255, 128, 64, 0},
  };
  for (auto& row : rows) {
    ASSERT_TRUE(writer->WriteNextScanline(reinterpret_cast<void*>(row)));
  }
  ASSERT_TRUE(writer->FinalizeWrite());
  EXPECT_GT(output.size(), 0u);

  // Read back and verify pixel values.
  PngScanlineReaderRaw reader(&message_handler_);
  ASSERT_TRUE(reader.Initialize(output.data(), output.length()));
  EXPECT_EQ(4u, reader.GetImageWidth());
  EXPECT_EQ(4u, reader.GetImageHeight());
  EXPECT_EQ(GRAY_8, reader.GetPixelFormat());
  EXPECT_EQ(4u, reader.GetBytesPerScanline());

  int row_idx = 0;
  while (reader.HasMoreScanLines()) {
    void* scanline = nullptr;
    ASSERT_TRUE(reader.ReadNextScanline(&scanline));
    auto* pixels = static_cast<uint8_t*>(scanline);
    for (int x = 0; x < 4; ++x) {
      EXPECT_EQ(rows[row_idx][x], pixels[x])
          << "mismatch at row=" << row_idx << " col=" << x;
    }
    ++row_idx;
  }
  EXPECT_EQ(4, row_idx);
}

// ============================================================
// Coverage: PngReader::ReadPng require_opaque + alpha stripping
// via opng_reduce_image (lines 346-354 of png_optimizer.cc)
// ============================================================

// When require_opaque=true and no PNG_TRANSFORM_STRIP_ALPHA, ReadPng
// should check IsAlphaChannelOpaque and strip alpha via opng_reduce_image
// if the alpha channel is fully opaque.
TEST(PngReaderRequireOpaqueTest, OpaqueRGBAStrippedViaOpngReduce) {
  NullMessageHandler handler;
  PngReader reader(&handler);
  ScopedPngStruct read(ScopedPngStruct::READ, &handler);

  // rgba_opaque.png is an RGBA image where all alpha values are 255.
  std::string in;
  ASSERT_TRUE(ReadTestFile(kPngSuiteTestDir, "rgba_opaque", "png", &in));

  // Call ReadPng with require_opaque=true and NO PNG_TRANSFORM_STRIP_ALPHA.
  // This exercises lines 339-354: the code detects alpha is opaque
  // via IsAlphaChannelOpaque, then calls opng_reduce_image(STRIP_ALPHA)
  // to strip the alpha channel.
  ASSERT_TRUE(reader.ReadPng(in, read.png_ptr(), read.info_ptr(),
                             PNG_TRANSFORM_IDENTITY, true));

  // After stripping alpha, the color type should no longer include alpha.
  int color_type = png_get_color_type(read.png_ptr(), read.info_ptr());
  EXPECT_EQ(0, (color_type & PNG_COLOR_MASK_ALPHA))
      << "Alpha should have been stripped for opaque RGBA image, got "
         "color_type="
      << color_type;
}

TEST(PngReaderRequireOpaqueTest, OpaqueGrayAlphaStrippedViaOpngReduce) {
  NullMessageHandler handler;
  PngReader reader(&handler);
  ScopedPngStruct read(ScopedPngStruct::READ, &handler);

  // grey_alpha_opaque.png is a gray+alpha image where all alpha values are 255.
  std::string in;
  ASSERT_TRUE(ReadTestFile(kPngSuiteTestDir, "grey_alpha_opaque", "png", &in));

  // Call ReadPng with require_opaque=true and NO PNG_TRANSFORM_STRIP_ALPHA.
  ASSERT_TRUE(reader.ReadPng(in, read.png_ptr(), read.info_ptr(),
                             PNG_TRANSFORM_IDENTITY, true));

  // After stripping alpha, the color type should not include alpha.
  int color_type = png_get_color_type(read.png_ptr(), read.info_ptr());
  EXPECT_EQ(0, (color_type & PNG_COLOR_MASK_ALPHA))
      << "Alpha should have been stripped for opaque gray+alpha image, got "
         "color_type="
      << color_type;
}

TEST(PngReaderRequireOpaqueTest, TransparentImageRejectedWhenOpaqueRequired) {
  NullMessageHandler handler;
  PngReader reader(&handler);
  ScopedPngStruct read(ScopedPngStruct::READ, &handler);

  // bgai4a16 is a gray+alpha image with actual transparency.
  std::string in;
  ASSERT_TRUE(ReadTestFile(kPngSuiteTestDir, "bgai4a16", "png", &in));

  // require_opaque=true without STRIP_ALPHA transform, on a transparent
  // image. IsAlphaChannelOpaque returns false, so ReadPng should fail.
  EXPECT_FALSE(reader.ReadPng(in, read.png_ptr(), read.info_ptr(),
                              PNG_TRANSFORM_IDENTITY, true));
}

TEST(PngReaderRequireOpaqueTest, OpaqueNotCheckedWhenStripAlphaTransformSet) {
  NullMessageHandler handler;
  PngReader reader(&handler);
  ScopedPngStruct read(ScopedPngStruct::READ, &handler);

  // basn6a08 is RGBA with actual transparency. When PNG_TRANSFORM_STRIP_ALPHA
  // is set, the require_opaque code path (lines 339-354) is skipped entirely.
  std::string in;
  ASSERT_TRUE(ReadTestFile(kPngSuiteTestDir, "basn6a08", "png", &in));

  // With STRIP_ALPHA transform, require_opaque check is bypassed.
  // ReadPng should succeed even though the image has non-opaque alpha.
  ASSERT_TRUE(reader.ReadPng(in, read.png_ptr(), read.info_ptr(),
                             PNG_TRANSFORM_STRIP_ALPHA, true));
}

// When require_opaque=false, alpha channel is not checked or stripped.
TEST(PngReaderRequireOpaqueTest, AlphaPreservedWhenOpaqueNotRequired) {
  NullMessageHandler handler;
  PngReader reader(&handler);
  ScopedPngStruct read(ScopedPngStruct::READ, &handler);

  std::string in;
  ASSERT_TRUE(ReadTestFile(kPngSuiteTestDir, "basn6a08", "png", &in));

  // require_opaque=false: alpha is left alone.
  ASSERT_TRUE(reader.ReadPng(in, read.png_ptr(), read.info_ptr(),
                             PNG_TRANSFORM_IDENTITY, false));

  int color_type = png_get_color_type(read.png_ptr(), read.info_ptr());
  EXPECT_NE(0, (color_type & PNG_COLOR_MASK_ALPHA))
      << "Alpha should be preserved when require_opaque=false";
}

// ============================================================
// Coverage: PngScanlineReaderRaw with palette+tRNS (GIF
// transparency), testing the expansion path in InitializeWithStatus
// ============================================================

TEST_F(PngScanlineReaderRawTest, GrayscaleWithTrnsExpandedToRGBA) {
  // A grayscale image with tRNS chunk should be expanded to RGBA
  // via the gray_to_rgb + expand transforms in InitializeWithStatus.
  // This exercises the GRAY + tRNS path at lines 898-903 of png_optimizer.cc.
  PngScanlineReaderRaw reader(&message_handler_);
  std::string in;
  // tbwn1g16 is a 16-bit grayscale image with tRNS.
  ASSERT_TRUE(ReadTestFile(kPngSuiteTestDir, "tbwn1g16", "png", &in));
  ASSERT_TRUE(reader.Initialize(in.data(), in.length()));
  EXPECT_EQ(32u, reader.GetImageWidth());
  EXPECT_EQ(32u, reader.GetImageHeight());
  // After expansion: gray+tRNS -> gray_alpha -> RGBA via gray_to_rgb.
  EXPECT_EQ(pagespeed::image_compression::RGBA_8888, reader.GetPixelFormat());

  int rows = 0;
  while (reader.HasMoreScanLines()) {
    void* scanline = nullptr;
    ASSERT_TRUE(reader.ReadNextScanline(&scanline));
    ASSERT_NE(nullptr, scanline);
    ++rows;
  }
  EXPECT_EQ(32, rows);
}

// ============================================================
// Coverage: PngScanlineReader with require_opaque via
// InitializeRead (lines 673-707 of png_optimizer.cc)
// ============================================================

TEST(PngScanlineReaderDirectTest, RequireOpaqueOnOpaqueGrayAlpha) {
  NullMessageHandler handler;
  PngScanlineReader reader(&handler);
  PngReader png_reader(&handler);
  if (setjmp(*reader.GetJmpBuf())) {
    FAIL() << "libpng error";
  }

  std::string in;
  ASSERT_TRUE(ReadTestFile(kPngSuiteTestDir, "grey_alpha_opaque", "png", &in));
  reader.set_transform(PNG_TRANSFORM_IDENTITY);
  reader.set_require_opaque(true);

  bool is_opaque = false;
  ASSERT_TRUE(reader.InitializeRead(png_reader, in, &is_opaque));
  EXPECT_TRUE(is_opaque);
}

// ==================================================================
// Phase 5: Validation/error path coverage for png_optimizer.cc
// ==================================================================

// Helper: Build a valid minimal PNG with a given IHDR (for crafting tests).
// Returns a valid PNG with an 8-bit 1x1 RGB image.
static std::string MakeMinimalPng() {
  // Use PngScanlineWriter to create a tiny valid PNG.
  NullMessageHandler handler;
  PngScanlineWriter writer(&handler);
  std::string output;
  PngCompressParams params(PNG_FILTER_NONE, Z_DEFAULT_STRATEGY, false);
  writer.Init(1, 1, RGB_888);
  writer.InitializeWrite(&params, &output);
  uint8_t row[3] = {255, 0, 0};
  writer.WriteNextScanline(reinterpret_cast<void*>(row));
  writer.FinalizeWrite();
  return output;
}

// --- GetAttributes: header validation ---

TEST_F(PngOptimizerTest, GetAttributes_TruncatedBeforeIHDR) {
  // A PNG with just the signature (8 bytes) but no IHDR chunk.
  reader_ = std::make_unique<PngReader>(&message_handler_);
  // Valid PNG signature.
  std::string sig("\x89PNG\r\n\x1a\n", 8);
  int w, h, bd, ct;
  EXPECT_FALSE(reader_->GetAttributes(sig, &w, &h, &bd, &ct))
      << "Should fail with only PNG signature, no IHDR";
}

TEST_F(PngOptimizerTest, GetAttributes_InvalidSignature) {
  // Build data that has a valid length but wrong PNG signature.
  reader_ = std::make_unique<PngReader>(&message_handler_);
  std::string valid_png = MakeMinimalPng();
  ASSERT_GE(valid_png.size(), 33u);

  // Corrupt the PNG signature (first byte).
  std::string corrupted = valid_png;
  corrupted[0] = 0x00;
  int w, h, bd, ct;
  EXPECT_FALSE(reader_->GetAttributes(corrupted, &w, &h, &bd, &ct))
      << "Should fail with invalid PNG signature";
}

TEST_F(PngOptimizerTest, GetAttributes_BadIHDRChunkLength) {
  // A PNG where the IHDR chunk length field is wrong (not 13).
  reader_ = std::make_unique<PngReader>(&message_handler_);
  std::string valid_png = MakeMinimalPng();
  ASSERT_GE(valid_png.size(), 33u);

  std::string corrupted = valid_png;
  // The IHDR chunk length is at bytes 8-11 (big-endian).
  // Set it to 14 instead of 13.
  corrupted[8] = 0;
  corrupted[9] = 0;
  corrupted[10] = 0;
  corrupted[11] = 14;
  int w, h, bd, ct;
  EXPECT_FALSE(reader_->GetAttributes(corrupted, &w, &h, &bd, &ct))
      << "Should fail when IHDR chunk length != 13";
}

TEST_F(PngOptimizerTest, GetAttributes_NonIHDRFirstChunk) {
  // A PNG where the first chunk type is not "IHDR".
  reader_ = std::make_unique<PngReader>(&message_handler_);
  std::string valid_png = MakeMinimalPng();
  ASSERT_GE(valid_png.size(), 33u);

  std::string corrupted = valid_png;
  // The chunk type is at bytes 12-15. Replace "IHDR" with "XXXX".
  corrupted[12] = 'X';
  corrupted[13] = 'X';
  corrupted[14] = 'X';
  corrupted[15] = 'X';
  int w, h, bd, ct;
  EXPECT_FALSE(reader_->GetAttributes(corrupted, &w, &h, &bd, &ct))
      << "Should fail when first chunk is not IHDR";
}

TEST_F(PngOptimizerTest, GetAttributes_BadCRC) {
  // A PNG with a corrupted CRC for the IHDR chunk.
  reader_ = std::make_unique<PngReader>(&message_handler_);
  std::string valid_png = MakeMinimalPng();
  ASSERT_GE(valid_png.size(), 33u);

  std::string corrupted = valid_png;
  // The CRC is at bytes 29-32 (8 sig + 4 len + 4 name + 13 IHDR data = 29).
  corrupted[29] ^= 0xFF;
  int w, h, bd, ct;
  EXPECT_FALSE(reader_->GetAttributes(corrupted, &w, &h, &bd, &ct))
      << "Should fail with corrupted IHDR CRC";
}

TEST_F(PngOptimizerTest, GetAttributes_ExactMinSize33Bytes) {
  // Exactly 33 bytes: 8 sig + 4 len + 4 name + 13 IHDR + 4 CRC.
  // This should succeed if the data is valid.
  reader_ = std::make_unique<PngReader>(&message_handler_);
  std::string valid_png = MakeMinimalPng();
  ASSERT_GE(valid_png.size(), 33u);

  // Truncate to exactly 33 bytes.
  std::string exact = valid_png.substr(0, 33);
  int w, h, bd, ct;
  EXPECT_TRUE(reader_->GetAttributes(exact, &w, &h, &bd, &ct))
      << "Should succeed with exactly 33 bytes of valid PNG header";
  EXPECT_EQ(1, w);
  EXPECT_EQ(1, h);
}

TEST_F(PngOptimizerTest, GetAttributes_32BytesFails) {
  // 32 bytes (one less than minimum) should fail.
  reader_ = std::make_unique<PngReader>(&message_handler_);
  std::string valid_png = MakeMinimalPng();
  ASSERT_GE(valid_png.size(), 33u);

  std::string truncated = valid_png.substr(0, 32);
  int w, h, bd, ct;
  EXPECT_FALSE(reader_->GetAttributes(truncated, &w, &h, &bd, &ct))
      << "Should fail with 32 bytes (one less than minimum)";
}

// --- OptimizePng: feed corrupt data ---

TEST_F(PngOptimizerTest, OptimizePng_CorruptedIDAT) {
  // Create a valid PNG, then corrupt the IDAT data.
  reader_ = std::make_unique<PngReader>(&message_handler_);
  std::string valid_png = MakeMinimalPng();
  ASSERT_GT(valid_png.size(), 40u);

  // Corrupt some bytes in the middle of the file (likely IDAT area).
  std::string corrupted = valid_png;
  size_t mid = corrupted.size() / 2;
  for (size_t i = mid; i < mid + 5 && i < corrupted.size(); ++i) {
    corrupted[i] ^= 0xFF;
  }
  std::string out;
  EXPECT_FALSE(
      PngOptimizer::OptimizePng(*reader_, corrupted, &out, &message_handler_))
      << "Should fail on corrupted IDAT data";
}

TEST_F(PngOptimizerTest, OptimizePng_AllZeros) {
  // A buffer of all zeros should fail gracefully.
  reader_ = std::make_unique<PngReader>(&message_handler_);
  std::string zeros(100, '\0');
  std::string out;
  EXPECT_FALSE(
      PngOptimizer::OptimizePng(*reader_, zeros, &out, &message_handler_));
}

TEST_F(PngOptimizerTest, OptimizePng_ValidSignatureThenGarbage) {
  // Valid PNG signature followed by garbage data.
  reader_ = std::make_unique<PngReader>(&message_handler_);
  std::string data("\x89PNG\r\n\x1a\n", 8);
  data += std::string(100, 'X');
  std::string out;
  EXPECT_FALSE(
      PngOptimizer::OptimizePng(*reader_, data, &out, &message_handler_))
      << "Should fail with valid sig + garbage";
}

// --- PngScanlineReaderRaw: various corrupt input ---

TEST_F(PngScanlineReaderRawTest, ValidSignatureThenGarbage) {
  PngScanlineReaderRaw reader(&message_handler_);
  std::string data("\x89PNG\r\n\x1a\n", 8);
  data += std::string(50, '\xAB');
  EXPECT_FALSE(reader.Initialize(data.data(), data.size()));
}

TEST_F(PngScanlineReaderRawTest, TruncatedIHDR) {
  PngScanlineReaderRaw reader(&message_handler_);
  // Valid PNG sig + partial IHDR (only chunk length, no type/data).
  std::string data("\x89PNG\r\n\x1a\n", 8);
  data += std::string({0, 0, 0, 13});  // length
  EXPECT_FALSE(reader.Initialize(data.data(), data.size()));
}

TEST_F(PngScanlineReaderRawTest, ZeroLengthBuffer) {
  PngScanlineReaderRaw reader(&message_handler_);
  EXPECT_FALSE(reader.Initialize(nullptr, 0));
}

TEST_F(PngScanlineReaderRawTest, SingleByteBuffer) {
  PngScanlineReaderRaw reader(&message_handler_);
  uint8_t byte = 0x89;
  EXPECT_FALSE(reader.Initialize(&byte, 1));
}

// --- PngScanlineWriter: validation paths ---

TEST_F(PngScanlineWriterTest, InitWithZeroDimensions) {
  auto writer = std::make_unique<PngScanlineWriter>(&message_handler_);
  // Width=0 should fail.
#ifdef NDEBUG
  EXPECT_FALSE(writer->Init(0, 4, RGB_888));
#endif
}

TEST_F(PngScanlineWriterTest, InitWithZeroHeight) {
  auto writer = std::make_unique<PngScanlineWriter>(&message_handler_);
#ifdef NDEBUG
  EXPECT_FALSE(writer->Init(4, 0, RGB_888));
#endif
}

TEST_F(PngScanlineWriterTest, InitWithUnsupportedPixelFormat) {
  auto writer = std::make_unique<PngScanlineWriter>(&message_handler_);
#ifdef NDEBUG
  EXPECT_FALSE(writer->Init(4, 4, pagespeed::image_compression::UNSUPPORTED));
#endif
}

TEST_F(PngScanlineWriterTest, ValidateRejectsNullOutput) {
  auto writer = std::make_unique<PngScanlineWriter>(&message_handler_);
  ASSERT_TRUE(writer->Init(4, 4, RGB_888));
#ifdef NDEBUG
  EXPECT_FALSE(writer->InitializeWrite(&params_, nullptr));
#endif
}

TEST_F(PngScanlineWriterTest, ValidateRejectsInvalidCompressionStrategy) {
  auto writer = std::make_unique<PngScanlineWriter>(&message_handler_);
  ASSERT_TRUE(writer->Init(4, 4, RGB_888));
  PngCompressParams bad_params(PNG_FILTER_NONE, 999, false);
#ifdef NDEBUG
  std::string output;
  EXPECT_FALSE(writer->InitializeWrite(&bad_params, &output));
#endif
}

TEST_F(PngScanlineWriterTest, FinalizeWithoutInit) {
  auto writer = std::make_unique<PngScanlineWriter>(&message_handler_);
#ifdef NDEBUG
  EXPECT_FALSE(writer->FinalizeWrite());
#endif
}

// --- PngScanlineReader: InitializeWithStatus (the no-op path) ---

TEST(PngScanlineReaderDirectTest, InitializeWithStatusFails) {
  NullMessageHandler handler;
  PngScanlineReader reader(&handler);
  // InitializeWithStatus is a no-op and should fail.
  ScanlineStatus status = reader.InitializeWithStatus(nullptr, 0);
#ifdef NDEBUG
  EXPECT_FALSE(status.Success());
#endif
}

// --- PngReader::ReadPng: EOF during read ---

TEST(PngReaderValidation, ReadPngTruncatedAfterIHDR) {
  NullMessageHandler handler;
  PngReader reader(&handler);
  ScopedPngStruct read(ScopedPngStruct::READ, &handler);

  // Create a valid PNG and truncate it after the IHDR chunk (33 bytes).
  std::string valid_png = MakeMinimalPng();
  ASSERT_GT(valid_png.size(), 33u);
  std::string truncated = valid_png.substr(0, 34);

  EXPECT_FALSE(reader.ReadPng(truncated, read.png_ptr(), read.info_ptr(),
                              PNG_TRANSFORM_IDENTITY, false));
}

TEST(PngReaderValidation, ReadPngEmptyBody) {
  NullMessageHandler handler;
  PngReader reader(&handler);
  ScopedPngStruct read(ScopedPngStruct::READ, &handler);

  std::string empty;
  EXPECT_FALSE(reader.ReadPng(empty, read.png_ptr(), read.info_ptr(),
                              PNG_TRANSFORM_IDENTITY, false));
}

TEST(PngReaderValidation, ReadPngAllZeros) {
  NullMessageHandler handler;
  PngReader reader(&handler);
  ScopedPngStruct read(ScopedPngStruct::READ, &handler);

  std::string zeros(200, '\0');
  EXPECT_FALSE(reader.ReadPng(zeros, read.png_ptr(), read.info_ptr(),
                              PNG_TRANSFORM_IDENTITY, false));
}

// --- IsAlphaChannelOpaque: edge cases ---

TEST(PngReaderValidation, IsAlphaChannelOpaque_RGBA16bit) {
  // Test the 16-bit RGBA path. basn6a16 is a 16-bit RGBA image.
  NullMessageHandler handler;
  PngReader reader(&handler);
  ScopedPngStruct read(ScopedPngStruct::READ, &handler);

  std::string in;
  ASSERT_TRUE(ReadTestFile(kPngSuiteTestDir, "basn6a16", "png", &in));
  ASSERT_TRUE(reader.ReadPng(in, read.png_ptr(), read.info_ptr(),
                             PNG_TRANSFORM_IDENTITY, false));

  // basn6a16 has non-trivial transparency -- should NOT be opaque.
  EXPECT_FALSE(PngReaderInterface::IsAlphaChannelOpaque(
      read.png_ptr(), read.info_ptr(), &handler));
}

TEST(PngReaderValidation, IsAlphaChannelOpaque_GrayAlpha16bit) {
  // Test the 16-bit gray+alpha path. basn4a16 is a 16-bit gray+alpha image.
  NullMessageHandler handler;
  PngReader reader(&handler);
  ScopedPngStruct read(ScopedPngStruct::READ, &handler);

  std::string in;
  ASSERT_TRUE(ReadTestFile(kPngSuiteTestDir, "basn4a16", "png", &in));
  ASSERT_TRUE(reader.ReadPng(in, read.png_ptr(), read.info_ptr(),
                             PNG_TRANSFORM_IDENTITY, false));

  // basn4a16 has non-trivial transparency -- should NOT be opaque.
  EXPECT_FALSE(PngReaderInterface::IsAlphaChannelOpaque(
      read.png_ptr(), read.info_ptr(), &handler));
}

// --- GetBackgroundColor edge cases ---

TEST(PngReaderValidation, GetBackgroundColor_NoBKGD) {
  // An image without bKGD chunk should return false.
  NullMessageHandler handler;
  PngReader reader(&handler);
  ScopedPngStruct read(ScopedPngStruct::READ, &handler);

  std::string in;
  ASSERT_TRUE(ReadTestFile(kPngSuiteTestDir, "basn0g08", "png", &in));
  ASSERT_TRUE(reader.ReadPng(in, read.png_ptr(), read.info_ptr(),
                             PNG_TRANSFORM_IDENTITY, false));

  unsigned char r, g, b;
  EXPECT_FALSE(PngReaderInterface::GetBackgroundColor(
      read.png_ptr(), read.info_ptr(), &r, &g, &b, &handler));
}

TEST(PngReaderValidation, GetBackgroundColor_WithBKGD) {
  // tbbn1g04 is a grayscale image with a bKGD chunk.
  NullMessageHandler handler;
  PngReader reader(&handler);
  ScopedPngStruct read(ScopedPngStruct::READ, &handler);

  std::string in;
  ASSERT_TRUE(ReadTestFile(kPngSuiteTestDir, "tbbn1g04", "png", &in));
  ASSERT_TRUE(reader.ReadPng(in, read.png_ptr(), read.info_ptr(),
                             PNG_TRANSFORM_IDENTITY, false));

  unsigned char r, g, b;
  bool has_bg = PngReaderInterface::GetBackgroundColor(
      read.png_ptr(), read.info_ptr(), &r, &g, &b, &handler);
  // tbbn1g04 has a bKGD chunk, should succeed.
  EXPECT_TRUE(has_bg);
}

TEST(PngReaderValidation, GetBackgroundColor_16bitWithBKGD) {
  // tbbn2c16 is a 16-bit RGB image with a bKGD chunk.
  NullMessageHandler handler;
  PngReader reader(&handler);
  ScopedPngStruct read(ScopedPngStruct::READ, &handler);

  std::string in;
  ASSERT_TRUE(ReadTestFile(kPngSuiteTestDir, "tbbn2c16", "png", &in));
  ASSERT_TRUE(reader.ReadPng(in, read.png_ptr(), read.info_ptr(),
                             PNG_TRANSFORM_IDENTITY, false));

  unsigned char r, g, b;
  bool has_bg = PngReaderInterface::GetBackgroundColor(
      read.png_ptr(), read.info_ptr(), &r, &g, &b, &handler);
  // Should succeed and downsample from 16 to 8 bits.
  EXPECT_TRUE(has_bg);
}

TEST(PngReaderValidation, GetBackgroundColor_PaletteWithBKGD) {
  // tbbn3p08 is a paletted image with a bKGD chunk.
  NullMessageHandler handler;
  PngReader reader(&handler);
  ScopedPngStruct read(ScopedPngStruct::READ, &handler);

  std::string in;
  ASSERT_TRUE(ReadTestFile(kPngSuiteTestDir, "tbbn3p08", "png", &in));
  ASSERT_TRUE(reader.ReadPng(in, read.png_ptr(), read.info_ptr(),
                             PNG_TRANSFORM_IDENTITY, false));

  unsigned char r, g, b;
  bool has_bg = PngReaderInterface::GetBackgroundColor(
      read.png_ptr(), read.info_ptr(), &r, &g, &b, &handler);
  // Palette images with bKGD have 8-bit depth.
  EXPECT_TRUE(has_bg);
}

// --- ScopedPngStruct: multiple reset cycle ---

TEST(ScopedPngStructTest, MultipleResets) {
  NullMessageHandler handler;
  ScopedPngStruct read(ScopedPngStruct::READ, &handler);
  for (int i = 0; i < 5; ++i) {
    ASSERT_TRUE(read.reset());
    ASSERT_TRUE(read.valid());
  }
}

// --- PngCompressParams constructors ---

TEST(PngCompressParamsTest, FilterStrategyConstructor) {
  PngCompressParams params(PNG_ALL_FILTERS, Z_FILTERED, true);
  EXPECT_EQ(PNG_ALL_FILTERS, params.filter_level);
  EXPECT_EQ(Z_FILTERED, params.compression_strategy);
  EXPECT_FALSE(params.try_best_compression);
  EXPECT_TRUE(params.is_progressive);
}

TEST(PngCompressParamsTest, BestCompressionConstructor) {
  PngCompressParams params(true, false);
  EXPECT_TRUE(params.try_best_compression);
  EXPECT_FALSE(params.is_progressive);
  // Default filter/strategy for this constructor.
  EXPECT_EQ(PNG_FILTER_NONE, params.filter_level);
  EXPECT_EQ(Z_NO_COMPRESSION, params.compression_strategy);
}

// --- CopyPngStructs ---

TEST(PngOptimizerUtils, CopyPngStructsPreservesMetadata) {
  NullMessageHandler handler;
  // Read a valid PNG into a read struct.
  PngReader reader(&handler);
  ScopedPngStruct read(ScopedPngStruct::READ, &handler);
  std::string in;
  ASSERT_TRUE(ReadTestFile(kPngSuiteTestDir, "basn2c08", "png", &in));
  ASSERT_TRUE(reader.ReadPng(in, read.png_ptr(), read.info_ptr(),
                             PNG_TRANSFORM_IDENTITY, false));

  // Copy to a write struct.
  ScopedPngStruct write(ScopedPngStruct::WRITE, &handler);
  ASSERT_TRUE(PngOptimizer::CopyPngStructs(read, &write));

  // Verify dimensions were preserved.
  png_uint_32 w, h;
  int bd, ct, il;
  png_get_IHDR(write.png_ptr(), write.info_ptr(), &w, &h, &bd, &ct, &il,
               nullptr, nullptr);
  EXPECT_EQ(32u, w);
  EXPECT_EQ(32u, h);
  EXPECT_EQ(8, bd);
  EXPECT_EQ(PNG_COLOR_TYPE_RGB, ct);
}

// ==========================================================================
// Error path coverage: OptimizePngBestCompression with corrupt data
// ==========================================================================

TEST_F(PngOptimizerTest, OptimizePngBestCompression_CorruptedIDAT) {
  reader_ = std::make_unique<PngReader>(&message_handler_);
  std::string valid_png = MakeMinimalPng();
  ASSERT_GT(valid_png.size(), 40u);

  // Corrupt bytes in the IDAT region.
  std::string corrupted = valid_png;
  size_t mid = corrupted.size() / 2;
  for (size_t i = mid; i < mid + 5 && i < corrupted.size(); ++i) {
    corrupted[i] ^= 0xFF;
  }
  std::string out;
  EXPECT_FALSE(PngOptimizer::OptimizePngBestCompression(
      *reader_, corrupted, &out, &message_handler_))
      << "Best-compression path should also fail on corrupted IDAT data";
}

TEST_F(PngOptimizerTest, OptimizePngBestCompression_EmptyInput) {
  reader_ = std::make_unique<PngReader>(&message_handler_);
  std::string empty;
  std::string out;
  EXPECT_FALSE(PngOptimizer::OptimizePngBestCompression(*reader_, empty, &out,
                                                        &message_handler_))
      << "Best-compression path should fail on empty input";
}

TEST_F(PngOptimizerTest, OptimizePngBestCompression_AllZeros) {
  reader_ = std::make_unique<PngReader>(&message_handler_);
  std::string zeros(200, '\0');
  std::string out;
  EXPECT_FALSE(PngOptimizer::OptimizePngBestCompression(*reader_, zeros, &out,
                                                        &message_handler_))
      << "Best-compression path should fail on all-zeros data";
}

// ==========================================================================
// Error path coverage: OptimizePng with various levels of truncation
// ==========================================================================

TEST_F(PngOptimizerTest, OptimizePng_TruncatedAtSignature) {
  reader_ = std::make_unique<PngReader>(&message_handler_);
  // Only the PNG signature, no chunks at all.
  std::string sig("\x89PNG\r\n\x1a\n", 8);
  std::string out;
  EXPECT_FALSE(
      PngOptimizer::OptimizePng(*reader_, sig, &out, &message_handler_))
      << "Should fail with just a PNG signature";
}

TEST_F(PngOptimizerTest, OptimizePng_TruncatedAfterIHDR) {
  reader_ = std::make_unique<PngReader>(&message_handler_);
  std::string valid_png = MakeMinimalPng();
  ASSERT_GT(valid_png.size(), 33u);
  // Keep only the IHDR chunk (no IDAT).
  std::string truncated = valid_png.substr(0, 34);
  std::string out;
  EXPECT_FALSE(
      PngOptimizer::OptimizePng(*reader_, truncated, &out, &message_handler_))
      << "Should fail when truncated after IHDR";
}

TEST_F(PngOptimizerTest, OptimizePng_SingleByte) {
  reader_ = std::make_unique<PngReader>(&message_handler_);
  std::string one_byte(1, 0x89);
  std::string out;
  EXPECT_FALSE(
      PngOptimizer::OptimizePng(*reader_, one_byte, &out, &message_handler_))
      << "Should fail with a single byte of data";
}

// ==========================================================================
// Error path coverage: IsAlphaChannelOpaque called on non-alpha image
// ==========================================================================

TEST(PngReaderValidation, IsAlphaChannelOpaque_NoAlphaImage) {
  // basn0g08 is 8-bit grayscale with NO alpha. Calling IsAlphaChannelOpaque
  // on it should return false (and log an error about missing alpha).
  NullMessageHandler handler;
  PngReader reader(&handler);
  ScopedPngStruct read(ScopedPngStruct::READ, &handler);

  std::string in;
  ASSERT_TRUE(ReadTestFile(kPngSuiteTestDir, "basn0g08", "png", &in));
  ASSERT_TRUE(reader.ReadPng(in, read.png_ptr(), read.info_ptr(),
                             PNG_TRANSFORM_IDENTITY, false));

  // This image has no alpha channel; the function should return false.
  EXPECT_FALSE(PngReaderInterface::IsAlphaChannelOpaque(
      read.png_ptr(), read.info_ptr(), &handler));
}

TEST(PngReaderValidation, IsAlphaChannelOpaque_RGBNoAlpha) {
  // basn2c08 is 8-bit RGB (no alpha). Should fail.
  NullMessageHandler handler;
  PngReader reader(&handler);
  ScopedPngStruct read(ScopedPngStruct::READ, &handler);

  std::string in;
  ASSERT_TRUE(ReadTestFile(kPngSuiteTestDir, "basn2c08", "png", &in));
  ASSERT_TRUE(reader.ReadPng(in, read.png_ptr(), read.info_ptr(),
                             PNG_TRANSFORM_IDENTITY, false));

  EXPECT_FALSE(PngReaderInterface::IsAlphaChannelOpaque(
      read.png_ptr(), read.info_ptr(), &handler));
}

// ==========================================================================
// Error path coverage: PngReader::ReadPng with require_opaque on transparent
// ==========================================================================

TEST(PngReaderValidation, ReadPngRequireOpaque_TransparentImage) {
  // bgai4a16 has transparency. Requiring opaque should fail.
  NullMessageHandler handler;
  PngReader reader(&handler);
  ScopedPngStruct read(ScopedPngStruct::READ, &handler);

  std::string in;
  ASSERT_TRUE(ReadTestFile(kPngSuiteTestDir, "bgai4a16", "png", &in));
  EXPECT_FALSE(reader.ReadPng(in, read.png_ptr(), read.info_ptr(),
                              PNG_TRANSFORM_IDENTITY, true))
      << "Transparent image should fail with require_opaque=true";
}

TEST(PngReaderValidation, ReadPngRequireOpaque_OpaqueRGBAImage) {
  // rgba_opaque has alpha channel but all pixels are fully opaque.
  // require_opaque should succeed (strip alpha).
  NullMessageHandler handler;
  PngReader reader(&handler);
  ScopedPngStruct read(ScopedPngStruct::READ, &handler);

  std::string in;
  ASSERT_TRUE(ReadTestFile(kPngSuiteTestDir, "rgba_opaque", "png", &in));
  EXPECT_TRUE(reader.ReadPng(in, read.png_ptr(), read.info_ptr(),
                             PNG_TRANSFORM_IDENTITY, true))
      << "Opaque RGBA image should succeed with require_opaque=true";
}

// ==========================================================================
// Error path coverage: PngScanlineReaderRaw with progressive corrupt PNG
// ==========================================================================

TEST_F(PngScanlineReaderRawTest, CorruptedIdatData) {
  // Read a valid PNG, corrupt the IDAT data, and try to read scanlines.
  // libpng may defer decompression errors until scanline reading.
  std::string valid;
  ASSERT_TRUE(ReadTestFile(kPngSuiteTestDir, "basn2c08", "png", &valid));
  // Corrupt in the IDAT area.
  std::string corrupted = valid;
  for (size_t i = corrupted.size() / 2;
       i < corrupted.size() / 2 + 10 && i < corrupted.size(); ++i) {
    corrupted[i] ^= 0xFF;
  }
  PngScanlineReaderRaw reader(&message_handler_);
  bool init_ok = reader.Initialize(corrupted.data(), corrupted.size());
  if (init_ok) {
    // If Init succeeded, the error should surface during scanline reading.
    while (reader.HasMoreScanLines()) {
      void* scanline = nullptr;
      ScanlineStatus status = reader.ReadNextScanlineWithStatus(&scanline);
      if (!status.Success()) {
        break;
      }
    }
  }
  // The key assertion is that we don't crash on corrupt data.
}

TEST_F(PngScanlineReaderRawTest, TruncatedBeforeIDAT) {
  // A valid PNG truncated to only the IHDR (no IDAT data).
  std::string valid;
  ASSERT_TRUE(ReadTestFile(kPngSuiteTestDir, "basn2c08", "png", &valid));
  ASSERT_GT(valid.size(), 33u);
  std::string truncated = valid.substr(0, 35);
  PngScanlineReaderRaw reader(&message_handler_);
  EXPECT_FALSE(reader.Initialize(truncated.data(), truncated.size()));
}

// ==========================================================================
// Error path coverage: PngScanlineReader InitializeRead with corrupt data
// ==========================================================================

TEST(PngScanlineReaderErrorPaths, CorruptPngData) {
  NullMessageHandler handler;
  PngScanlineReader reader(&handler);
  PngReader png_reader(&handler);

  // Valid PNG signature followed by garbage.
  std::string data("\x89PNG\r\n\x1a\n", 8);
  data += std::string(100, 'Z');
  EXPECT_FALSE(reader.InitializeRead(png_reader, data));
}

TEST(PngScanlineReaderErrorPaths, EmptyStringInput) {
  NullMessageHandler handler;
  PngScanlineReader reader(&handler);
  PngReader png_reader(&handler);

  std::string empty;
  EXPECT_FALSE(reader.InitializeRead(png_reader, empty));
}

TEST(PngScanlineReaderErrorPaths, InitializeReadWithIsOpaqueOutput) {
  NullMessageHandler handler;
  PngScanlineReader reader(&handler);
  PngReader png_reader(&handler);

  std::string data("\x89PNG\r\n\x1a\n", 8);
  data += std::string(100, 'Z');
  bool is_opaque = true;
  EXPECT_FALSE(reader.InitializeRead(png_reader, data, &is_opaque));
}

// ==========================================================================
// Error path coverage: CopyPngStructs with corrupt source
// ==========================================================================

TEST(PngOptimizerUtils, CopyPngStructsFromCorruptSource) {
  NullMessageHandler handler;
  PngReader reader(&handler);
  ScopedPngStruct read(ScopedPngStruct::READ, &handler);

  // Read corrupted PNG data into the read struct.
  std::string garbage("\x89PNG\r\n\x1a\n", 8);
  garbage += std::string(100, 'X');
  // ReadPng will fail, leaving the read struct in an indeterminate state.
  reader.ReadPng(garbage, read.png_ptr(), read.info_ptr(),
                 PNG_TRANSFORM_IDENTITY, false);

  // Attempting to copy from a read struct that never got valid data
  // should not crash (it may or may not fail depending on libpng state).
  ScopedPngStruct write(ScopedPngStruct::WRITE, &handler);
  // We just verify it doesn't crash -- the return value is not guaranteed.
  PngOptimizer::CopyPngStructs(read, &write);
}

// ==========================================================================
// Phase 6: Additional error path coverage for png_optimizer.cc
// ==========================================================================

// --- ScopedPngStruct::reset() paths ---

TEST(ScopedPngStructCoverage, ResetReadStruct) {
  // Exercise reset() on a READ struct. After reset, the struct should
  // still be valid and usable.
  NullMessageHandler handler;
  ScopedPngStruct read(ScopedPngStruct::READ, &handler);
  ASSERT_TRUE(read.valid());
  ASSERT_TRUE(read.reset());
  ASSERT_TRUE(read.valid());
  EXPECT_NE(nullptr, read.png_ptr());
  EXPECT_NE(nullptr, read.info_ptr());
}

TEST(ScopedPngStructCoverage, ResetWriteStruct) {
  // Exercise reset() on a WRITE struct.
  NullMessageHandler handler;
  ScopedPngStruct write(ScopedPngStruct::WRITE, &handler);
  ASSERT_TRUE(write.valid());
  ASSERT_TRUE(write.reset());
  ASSERT_TRUE(write.valid());
  EXPECT_NE(nullptr, write.png_ptr());
  EXPECT_NE(nullptr, write.info_ptr());
}

TEST(ScopedPngStructCoverage, ResetMultipleTimes) {
  // Verify that resetting multiple times is safe.
  NullMessageHandler handler;
  ScopedPngStruct read(ScopedPngStruct::READ, &handler);
  for (int i = 0; i < 5; ++i) {
    ASSERT_TRUE(read.reset()) << "reset() failed on iteration " << i;
    EXPECT_TRUE(read.valid());
  }
}

TEST(ScopedPngStructCoverage, ResetAfterReadOperation) {
  // Read valid PNG data, then reset and verify struct is reusable.
  NullMessageHandler handler;
  PngReader reader(&handler);
  ScopedPngStruct read(ScopedPngStruct::READ, &handler);

  std::string in;
  ASSERT_TRUE(ReadTestFile(kPngSuiteTestDir, "basn2c08", "png", &in));
  ASSERT_TRUE(reader.ReadPng(in, read.png_ptr(), read.info_ptr(),
                             PNG_TRANSFORM_IDENTITY, false));

  // After reading, reset should work.
  ASSERT_TRUE(read.reset());
  EXPECT_TRUE(read.valid());

  // After reset, should be able to read again.
  ASSERT_TRUE(reader.ReadPng(in, read.png_ptr(), read.info_ptr(),
                             PNG_TRANSFORM_IDENTITY, false));
}

TEST(ScopedPngStructCoverage, ResetAfterCorruptRead) {
  // Attempt to read corrupt data (which fails), then reset and verify
  // the struct is reusable for a valid read.
  NullMessageHandler handler;
  PngReader reader(&handler);
  ScopedPngStruct read(ScopedPngStruct::READ, &handler);

  // Feed garbage that looks like PNG but is corrupt.
  std::string corrupt("\x89PNG\r\n\x1a\n", 8);
  corrupt += std::string(200, '\xff');
  EXPECT_FALSE(reader.ReadPng(corrupt, read.png_ptr(), read.info_ptr(),
                              PNG_TRANSFORM_IDENTITY, false));

  // Reset should succeed.
  ASSERT_TRUE(read.reset());
  EXPECT_TRUE(read.valid());

  // Now read a valid PNG.
  std::string valid;
  ASSERT_TRUE(ReadTestFile(kPngSuiteTestDir, "basn2c08", "png", &valid));
  EXPECT_TRUE(reader.ReadPng(valid, read.png_ptr(), read.info_ptr(),
                             PNG_TRANSFORM_IDENTITY, false));
}

// --- CreateOptimizedPng setjmp error paths (lines 232-239) ---

TEST_F(PngOptimizerTest, OptimizePng_CorruptedPngSignatureWithIDAT) {
  // A file that starts with a valid PNG signature but has corrupted
  // compressed data. This should trigger the setjmp path during decode
  // (line 232-234).
  reader_ = std::make_unique<PngReader>(&message_handler_);

  // Construct data: valid PNG sig + valid IHDR chunk + corrupted IDAT.
  std::string valid;
  ASSERT_TRUE(ReadTestFile(kPngSuiteTestDir, "basn2c08", "png", &valid));
  ASSERT_GT(valid.size(), 50u);

  // Keep the header and IHDR, corrupt everything after byte 40.
  std::string corrupted = valid;
  for (size_t i = 40; i < corrupted.size(); ++i) {
    corrupted[i] = static_cast<char>(i & 0xFF);
  }

  std::string out;
  EXPECT_FALSE(
      PngOptimizer::OptimizePng(*reader_, corrupted, &out, &message_handler_))
      << "OptimizePng should fail with corrupted IDAT data";
}

TEST_F(PngOptimizerTest, OptimizePng_SinglePixelCorrupt) {
  // Start with a valid 1x1 PNG and corrupt the IDAT data to trigger
  // the decode setjmp path.
  reader_ = std::make_unique<PngReader>(&message_handler_);
  std::string valid = MakeMinimalPng();
  ASSERT_GT(valid.size(), 40u);

  // Flip bits in the middle of the file.
  std::string corrupted = valid;
  for (size_t i = corrupted.size() / 2; i < corrupted.size(); ++i) {
    corrupted[i] ^= 0xFF;
  }

  std::string out;
  EXPECT_FALSE(
      PngOptimizer::OptimizePng(*reader_, corrupted, &out, &message_handler_))
      << "Should fail on corrupted single-pixel PNG";
}

TEST_F(PngOptimizerTest, OptimizePngBestCompression_SinglePixelCorrupt) {
  // Same as above but with best compression path.
  reader_ = std::make_unique<PngReader>(&message_handler_);
  std::string valid = MakeMinimalPng();
  ASSERT_GT(valid.size(), 40u);

  std::string corrupted = valid;
  for (size_t i = corrupted.size() / 2; i < corrupted.size(); ++i) {
    corrupted[i] ^= 0xFF;
  }

  std::string out;
  EXPECT_FALSE(PngOptimizer::OptimizePngBestCompression(
      *reader_, corrupted, &out, &message_handler_))
      << "Should fail on corrupted single-pixel PNG in best compression mode";
}

// --- opng_validate_image failure (line 248) ---
// This is triggered when libpng successfully reads the PNG but the image
// data violates optipng validation rules. Crafting such data is difficult,
// but we can try various malformed PNG files.

TEST_F(PngOptimizerTest, OptimizePng_TruncatedIDATMidStream) {
  // Truncate a valid PNG at a point where the IDAT is partially present.
  // This exercises the ReadPngFromStream EOF path which triggers
  // png_longjmp, hitting the setjmp in ReadPng (line 333).
  reader_ = std::make_unique<PngReader>(&message_handler_);
  std::string valid;
  ASSERT_TRUE(ReadTestFile(kPngTestDir, "this_is_a_test", "png", &valid));
  ASSERT_GT(valid.size(), 100u);

  // Truncate at various points in the IDAT region.
  for (size_t cut : {50u, 100u, 200u, 500u}) {
    if (cut >= valid.size()) continue;
    std::string truncated = valid.substr(0, cut);
    std::string out;
    EXPECT_FALSE(
        PngOptimizer::OptimizePng(*reader_, truncated, &out, &message_handler_))
        << "Should fail with PNG truncated at byte " << cut;
  }
}

// --- CopyReadToWrite error path (line 254) ---
// CopyReadToWrite calls CopyPngStructs which uses setjmp. We test this
// by feeding data that reads but yields corrupt internal state.

TEST(PngOptimizerCopyStructs, CopyFromValidReadToWrite) {
  NullMessageHandler handler;
  PngReader reader(&handler);
  ScopedPngStruct read(ScopedPngStruct::READ, &handler);

  std::string in;
  ASSERT_TRUE(ReadTestFile(kPngSuiteTestDir, "basn2c08", "png", &in));
  ASSERT_TRUE(reader.ReadPng(in, read.png_ptr(), read.info_ptr(),
                             PNG_TRANSFORM_IDENTITY, false));

  ScopedPngStruct write(ScopedPngStruct::WRITE, &handler);
  EXPECT_TRUE(PngOptimizer::CopyPngStructs(read, &write));
}

TEST(PngOptimizerCopyStructs, CopyFromResetReadStruct) {
  // Create a read struct, reset it (so it has no data), and try to copy.
  NullMessageHandler handler;
  ScopedPngStruct read(ScopedPngStruct::READ, &handler);

  // Don't read anything into it -- just reset.
  ASSERT_TRUE(read.reset());
  ScopedPngStruct write(ScopedPngStruct::WRITE, &handler);

  // Attempting to copy from an empty/reset read struct: we just verify
  // this doesn't crash. The return value depends on libpng internals.
  PngOptimizer::CopyPngStructs(read, &write);
}

// --- ReadPng with various corrupt patterns ---

TEST(PngReaderErrorPaths, ReadPngWithOnlySignature) {
  // Only the 8-byte PNG signature, nothing else.
  NullMessageHandler handler;
  PngReader reader(&handler);
  ScopedPngStruct read(ScopedPngStruct::READ, &handler);

  std::string sig("\x89PNG\r\n\x1a\n", 8);
  EXPECT_FALSE(reader.ReadPng(sig, read.png_ptr(), read.info_ptr(),
                              PNG_TRANSFORM_IDENTITY, false));
}

TEST(PngReaderErrorPaths, ReadPngWith4Bytes) {
  // Just 4 bytes -- not even a complete signature.
  NullMessageHandler handler;
  PngReader reader(&handler);
  ScopedPngStruct read(ScopedPngStruct::READ, &handler);

  std::string data("\x89PNG", 4);
  EXPECT_FALSE(reader.ReadPng(data, read.png_ptr(), read.info_ptr(),
                              PNG_TRANSFORM_IDENTITY, false));
}

TEST(PngReaderErrorPaths, ReadPngWithZeroBytes) {
  NullMessageHandler handler;
  PngReader reader(&handler);
  ScopedPngStruct read(ScopedPngStruct::READ, &handler);

  std::string empty;
  EXPECT_FALSE(reader.ReadPng(empty, read.png_ptr(), read.info_ptr(),
                              PNG_TRANSFORM_IDENTITY, false));
}

TEST(PngReaderErrorPaths, ReadPngValidSigCorruptIHDR) {
  // Valid PNG signature followed by an invalid IHDR chunk (wrong CRC).
  NullMessageHandler handler;
  PngReader reader(&handler);
  ScopedPngStruct read(ScopedPngStruct::READ, &handler);

  std::string data("\x89PNG\r\n\x1a\n", 8);
  // IHDR chunk: length=13
  data += std::string("\x00\x00\x00\x0d", 4);
  // Chunk type "IHDR"
  data += "IHDR";
  // IHDR data: width=1, height=1, bit_depth=8, color_type=2(RGB),
  // compression=0, filter=0, interlace=0
  data += std::string(
      "\x00\x00\x00\x01"  // width=1
      "\x00\x00\x00\x01"  // height=1
      "\x08"              // bit_depth=8
      "\x02"              // color_type=RGB
      "\x00"              // compression
      "\x00"              // filter
      "\x00",             // interlace
      13);
  // Bad CRC (all zeros).
  data += std::string("\x00\x00\x00\x00", 4);

  EXPECT_FALSE(reader.ReadPng(data, read.png_ptr(), read.info_ptr(),
                              PNG_TRANSFORM_IDENTITY, false));
}

TEST(PngReaderErrorPaths, ReadPngCorruptIDAT) {
  // Valid IHDR but corrupted IDAT chunk that triggers decode longjmp.
  NullMessageHandler handler;
  PngReader reader(&handler);

  std::string in;
  ASSERT_TRUE(ReadTestFile(kPngSuiteTestDir, "basn2c08", "png", &in));
  ASSERT_GT(in.size(), 50u);

  // Corrupt from byte 40 onward.
  std::string corrupted = in;
  for (size_t i = 40; i < corrupted.size(); ++i) {
    corrupted[i] = '\xDE';
  }

  ScopedPngStruct read(ScopedPngStruct::READ, &handler);
  EXPECT_FALSE(reader.ReadPng(corrupted, read.png_ptr(), read.info_ptr(),
                              PNG_TRANSFORM_IDENTITY, false));

  // After failure, reset and verify reuse.
  ASSERT_TRUE(read.reset());
  EXPECT_TRUE(read.valid());
}

// --- ReadPng with require_opaque on non-opaque images ---

TEST(PngReaderErrorPaths, RequireOpaqueOnTransparentImage) {
  NullMessageHandler handler;
  PngReader reader(&handler);
  ScopedPngStruct read(ScopedPngStruct::READ, &handler);

  // basn6a08 is RGBA with non-trivial alpha.
  std::string in;
  ASSERT_TRUE(ReadTestFile(kPngSuiteTestDir, "basn6a08", "png", &in));
  EXPECT_FALSE(reader.ReadPng(in, read.png_ptr(), read.info_ptr(),
                              PNG_TRANSFORM_IDENTITY, true));
}

// --- PngScanlineReader: InitializeRead setjmp path coverage ---

TEST(PngScanlineReaderSetjmpPaths, CorruptDataTriggersDecodeError) {
  NullMessageHandler handler;
  PngScanlineReader reader(&handler);
  PngReader png_reader(&handler);

  // Valid PNG signature but corrupt content.
  std::string valid;
  ASSERT_TRUE(ReadTestFile(kPngSuiteTestDir, "basn2c08", "png", &valid));
  ASSERT_GT(valid.size(), 60u);

  // Truncate after IHDR to cause decode failure.
  std::string corrupted = valid.substr(0, 35);
  EXPECT_FALSE(reader.InitializeRead(png_reader, corrupted));
}

TEST(PngScanlineReaderSetjmpPaths, ResetAndReuseAfterFailure) {
  NullMessageHandler handler;
  PngScanlineReader reader(&handler);
  PngReader png_reader(&handler);

  // Try to read corrupt data.
  std::string corrupt("\x89PNG\r\n\x1a\n", 8);
  corrupt += std::string(50, '\xAB');
  EXPECT_FALSE(reader.InitializeRead(png_reader, corrupt));

  // Reset should succeed.
  ASSERT_TRUE(reader.Reset());

  // Now read valid data.
  std::string valid;
  ASSERT_TRUE(ReadTestFile(kPngSuiteTestDir, "basn2c08", "png", &valid));
  EXPECT_TRUE(reader.InitializeRead(png_reader, valid));
}

// --- PngScanlineReaderRaw: more error path coverage ---

TEST(PngScanlineReaderRawErrors, CorruptedIDATTriggersSetjmp) {
  NullMessageHandler handler;
  PngScanlineReaderRaw reader(&handler);

  std::string valid;
  ASSERT_TRUE(ReadTestFile(kPngSuiteTestDir, "basn2c08", "png", &valid));
  ASSERT_GT(valid.size(), 50u);

  // Corrupt the IDAT data region.
  std::string corrupted = valid;
  for (size_t i = 40; i < corrupted.size(); ++i) {
    corrupted[i] ^= 0xFF;
  }

  EXPECT_FALSE(reader.Initialize(corrupted.data(), corrupted.length()));
}

TEST(PngScanlineReaderRawErrors, TruncatedAfterIHDR) {
  NullMessageHandler handler;
  PngScanlineReaderRaw reader(&handler);

  std::string valid;
  ASSERT_TRUE(ReadTestFile(kPngSuiteTestDir, "basn2c08", "png", &valid));
  ASSERT_GT(valid.size(), 40u);

  // Keep only header + IHDR, no IDAT.
  std::string truncated = valid.substr(0, 34);
  EXPECT_FALSE(reader.Initialize(truncated.data(), truncated.length()));
}

TEST(PngScanlineReaderRawErrors, ReinitializeAfterCorrupt) {
  NullMessageHandler handler;
  PngScanlineReaderRaw reader(&handler);

  // First, fail with corrupt data.
  std::string garbage(100, '\xFF');
  EXPECT_FALSE(reader.Initialize(garbage.data(), garbage.size()));

  // Then succeed with valid data.
  std::string valid;
  ASSERT_TRUE(ReadTestFile(kPngSuiteTestDir, "basn2c08", "png", &valid));
  EXPECT_TRUE(reader.Initialize(valid.data(), valid.length()));

  // Read all scanlines to verify the reader works.
  int rows = 0;
  while (reader.HasMoreScanLines()) {
    void* scanline = nullptr;
    ASSERT_TRUE(reader.ReadNextScanline(&scanline));
    ++rows;
  }
  EXPECT_EQ(32, rows);
}

// --- PngScanlineWriter: error handling ---

TEST_F(PngScanlineWriterTest, SmallImageWriteAndFinalize) {
  // Write a tiny 2x2 GRAY_8 image and verify round-trip.
  writer_ = std::make_unique<PngScanlineWriter>(&message_handler_);
  std::string output;
  PngCompressParams params(PNG_FILTER_NONE, Z_DEFAULT_STRATEGY, false);
  ASSERT_TRUE(writer_->Init(2, 2, GRAY_8));
  ASSERT_TRUE(writer_->InitializeWrite(&params, &output));
  unsigned char row[2] = {128, 128};
  ASSERT_TRUE(writer_->WriteNextScanline(reinterpret_cast<void*>(row)));
  ASSERT_TRUE(writer_->WriteNextScanline(reinterpret_cast<void*>(row)));
  ASSERT_TRUE(writer_->FinalizeWrite());
  EXPECT_GT(output.size(), 0u);
}

// --- OptimizePng roundtrip after failure ---

TEST_F(PngOptimizerTest, OptimizeAfterMultipleFailures) {
  // Multiple sequential failures followed by a success, verifying that
  // the optimizer cleanly handles each failure without state leaks.
  reader_ = std::make_unique<PngReader>(&message_handler_);

  // 3 consecutive failures.
  for (int i = 0; i < 3; ++i) {
    std::string garbage(50 + i * 10, static_cast<char>(i));
    std::string out;
    EXPECT_FALSE(
        PngOptimizer::OptimizePng(*reader_, garbage, &out, &message_handler_));
  }

  // Then a success.
  std::string valid;
  ASSERT_TRUE(ReadTestFile(kPngSuiteTestDir, "basn2c08", "png", &valid));
  std::string out;
  EXPECT_TRUE(
      PngOptimizer::OptimizePng(*reader_, valid, &out, &message_handler_));
  EXPECT_GT(out.size(), 0u);
}

// Regression tests for the decoded-size guard added after the 2026-05-29 audit
// (worker image): an interlaced PNG within libpng's own (1M x 1M)
// dimension limits could otherwise drive a multi-terabyte up-front allocation
// in ReadNextScanlineWithStatus.
using pagespeed::image_compression::PngScanlineReaderRaw;

TEST(PngDecodedSizeGuardTest, AcceptsNormalImage) {
  size_t bpr = 0, total = 0;
  EXPECT_TRUE(
      PngScanlineReaderRaw::ValidateDecodedSize(100, 50, 3, &bpr, &total));
  EXPECT_EQ(300u, bpr);
  EXPECT_EQ(15000u, total);
}

TEST(PngDecodedSizeGuardTest, RejectsZeroChannels) {
  size_t bpr = 0, total = 0;
  EXPECT_FALSE(
      PngScanlineReaderRaw::ValidateDecodedSize(100, 50, 0, &bpr, &total));
}

TEST(PngDecodedSizeGuardTest, RejectsInterlacedAllocationBomb) {
  // 1,000,000 x 1,000,000 x 4 bytes ~= 4 TB: within libpng's dimension limits
  // but far above the decode cap.
  size_t bpr = 0, total = 0;
  EXPECT_FALSE(PngScanlineReaderRaw::ValidateDecodedSize(1000000, 1000000, 4,
                                                         &bpr, &total));
}

TEST(PngDecodedSizeGuardTest, RejectsMultiplicationOverflow) {
  size_t bpr = 0, total = 0;
  EXPECT_FALSE(PngScanlineReaderRaw::ValidateDecodedSize(
      std::numeric_limits<size_t>::max(), 1, 4, &bpr, &total));
}

TEST(PngDecodedSizeGuardTest, EnforcesAbsoluteByteCap) {
  const size_t cap = PngScanlineReaderRaw::kMaxDecodedImageBytes;
  size_t bpr = 0, total = 0;
  EXPECT_FALSE(PngScanlineReaderRaw::ValidateDecodedSize(cap / 4 + 1, 1, 4,
                                                         &bpr, &total));
  EXPECT_TRUE(
      PngScanlineReaderRaw::ValidateDecodedSize(cap / 4, 1, 4, &bpr, &total));
}

}  // namespace
