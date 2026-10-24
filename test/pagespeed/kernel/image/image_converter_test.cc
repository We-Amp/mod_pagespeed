/*
 * Licensed to the Apache Software Foundation (ASF) under one
 * or more contributor license agreements.  See the NOTICE file
 * distributed with this work for additional information
 * regarding copyright ownership.  The ASF licenses this file
 * to you under the Apache License, Version 2.0 (the
 * "License"); you may not use this file except in compliance
 * with the License.  You may obtain a copy of the License at
 * 
 *   http://www.apache.org/licenses/LICENSE-2.0
 * 
 * Unless required by applicable law or agreed to in writing,
 * software distributed under the License is distributed on an
 * "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
 * KIND, either express or implied.  See the License for the
 * specific language governing permissions and limitations
 * under the License.
 */

#include "pagespeed/kernel/image/image_converter.h"

#include <cstddef>
#include <iterator>
#include <memory>

#include "pagespeed/kernel/base/null_mutex.h"
#include "pagespeed/kernel/base/string.h"
#include "pagespeed/kernel/image/gif_reader.h"
#include "pagespeed/kernel/image/image_util.h"
#include "pagespeed/kernel/image/png_optimizer.h"
#include "pagespeed/kernel/image/read_image.h"
#include "pagespeed/kernel/image/scanline_interface.h"
#include "test/pagespeed/kernel/base/gtest.h"
#include "test/pagespeed/kernel/base/mock_message_handler.h"
#include "test/pagespeed/kernel/image/test_utils.h"

namespace {

using net_instaweb::MockMessageHandler;
using net_instaweb::NullMutex;
using pagespeed::image_compression::GifReader;
using pagespeed::image_compression::IMAGE_GIF;
using pagespeed::image_compression::IMAGE_PNG;
using pagespeed::image_compression::IMAGE_WEBP;
using pagespeed::image_compression::ImageConverter;
using pagespeed::image_compression::JpegLossyOptions;
using pagespeed::image_compression::kGifTestDir;
using pagespeed::image_compression::kMessagePatternLibpngError;
using pagespeed::image_compression::kMessagePatternLibpngWarning;
using pagespeed::image_compression::kMessagePatternPixelFormat;
using pagespeed::image_compression::kMessagePatternStats;
using pagespeed::image_compression::kMessagePatternUnexpectedEOF;
using pagespeed::image_compression::kMessagePatternWritingToWebp;
using pagespeed::image_compression::kPngSuiteGifTestDir;
using pagespeed::image_compression::kPngSuiteTestDir;
using pagespeed::image_compression::kPngTestDir;
using pagespeed::image_compression::PngOptimizer;
using pagespeed::image_compression::PngReader;
using pagespeed::image_compression::PngReaderInterface;
using pagespeed::image_compression::ReadTestFile;
using pagespeed::image_compression::ScanlineReaderInterface;
using pagespeed::image_compression::ScanlineStatus;
using pagespeed::image_compression::ScanlineWriterInterface;
using pagespeed::image_compression::WebpConfiguration;

struct ImageCompressionInfo {
  const char* filename;
  size_t original_size;
  size_t compressed_size;
  bool is_png;
};

// These images were obtained from
// http://www.libpng.org/pub/png/pngsuite.html
ImageCompressionInfo kValidImages[] = {
    {"basi0g01", 217, 208, true},   {"basi0g02", 154, 154, true},
    {"basi0g04", 247, 145, true},   {"basi0g08", 254, 250, true},
    {"basi0g16", 299, 285, true},   {"basi2c08", 315, 313, true},
    {"basi2c16", 595, 419, false},  {"basi3p01", 132, 132, true},
    {"basi3p02", 193, 178, true},   {"basi3p04", 327, 312, true},
    {"basi4a08", 214, 209, true},   {"basi4a16", 2855, 1980, true},
    {"basi6a08", 361, 350, true},   {"basi6a16", 4180, 4148, true},
    {"basn0g01", 164, 164, true},   {"basn0g02", 104, 104, true},
    {"basn0g04", 145, 103, true},   {"basn0g08", 138, 132, true},
    {"basn0g16", 167, 152, true},   {"basn2c08", 145, 145, true},
    {"basn2c16", 302, 274, true},   {"basn3p01", 112, 112, true},
    {"basn3p02", 146, 131, true},   {"basn3p04", 216, 201, true},
    {"basn4a08", 126, 121, true},   {"basn4a16", 2206, 1185, true},
    {"basn6a08", 184, 177, true},   {"basn6a16", 3435, 3270, true},
    {"bgai4a08", 214, 209, true},   {"bgai4a16", 2855, 1980, true},
    {"bgan6a08", 184, 177, true},   {"bgan6a16", 3435, 3270, true},
    {"bgbn4a08", 140, 121, true},   {"bggn4a16", 2220, 1185, true},
    {"bgwn6a08", 202, 177, true},   {"bgyn6a16", 3453, 3270, true},
    {"cdfn2c08", 404, 498, true},   {"cdhn2c08", 344, 476, true},
    {"cdsn2c08", 232, 255, true},   {"cdun2c08", 724, 928, true},
    {"ch1n3p04", 258, 201, true},   {"cm0n0g04", 292, 271, true},
    {"cm7n0g04", 292, 271, true},   {"cm9n0g04", 292, 271, true},
    {"cs3n2c16", 214, 178, true},   {"cs3n3p08", 259, 244, true},
    {"cs5n2c08", 186, 226, true},   {"cs5n3p08", 271, 256, true},
    {"cs8n2c08", 149, 226, true},   {"cs8n3p08", 256, 256, true},
    {"ct0n0g04", 273, 271, true},   {"ct1n0g04", 792, 271, true},
    {"ctzn0g04", 753, 271, true},   {"f00n0g08", 319, 312, true},
    {"f01n0g08", 321, 246, true},   {"f02n0g08", 355, 289, true},
    {"f03n0g08", 389, 292, true},   {"f04n0g08", 269, 273, true},
    {"g03n0g16", 345, 273, true},   {"g03n2c08", 370, 396, true},
    {"g03n3p04", 214, 214, true},   {"g04n0g16", 363, 287, true},
    {"g04n2c08", 377, 399, true},   {"g04n3p04", 219, 219, true},
    {"g05n0g16", 339, 275, true},   {"g05n2c08", 350, 402, true},
    {"g05n3p04", 206, 206, true},   {"g07n0g16", 321, 261, true},
    {"g07n2c08", 340, 401, true},   {"g07n3p04", 207, 207, true},
    {"g10n0g16", 262, 210, true},   {"g10n2c08", 285, 403, true},
    {"g10n3p04", 214, 214, true},   {"g25n0g16", 383, 305, true},
    {"g25n2c08", 405, 399, true},   {"g25n3p04", 215, 215, true},
    {"oi1n0g16", 167, 152, true},   {"oi1n2c16", 302, 274, true},
    {"oi2n0g16", 179, 152, true},   {"oi2n2c16", 314, 274, true},
    {"oi4n0g16", 203, 152, true},   {"oi4n2c16", 338, 274, true},
    {"oi9n0g16", 1283, 152, true},  {"oi9n2c16", 3038, 274, true},
    {"pp0n2c16", 962, 419, false},  {"pp0n6a08", 818, 818, true},
    {"ps1n0g08", 1477, 132, true},  {"ps1n2c16", 1641, 274, true},
    {"ps2n0g08", 2341, 132, true},  {"ps2n2c16", 2505, 274, true},
    {"s01i3p01", 113, 98, true},    {"s01n3p01", 113, 98, true},
    {"s02i3p01", 114, 99, true},    {"s02n3p01", 115, 100, true},
    {"s03i3p01", 118, 103, true},   {"s03n3p01", 120, 105, true},
    {"s04i3p01", 126, 111, true},   {"s04n3p01", 121, 106, true},
    {"s05i3p02", 134, 121, true},   {"s05n3p02", 129, 114, true},
    {"s06i3p02", 143, 128, true},   {"s06n3p02", 131, 116, true},
    {"s07i3p02", 149, 136, true},   {"s07n3p02", 138, 123, true},
    {"s08i3p02", 149, 135, true},   {"s08n3p02", 139, 124, true},
    {"s09i3p02", 147, 133, true},   {"s09n3p02", 143, 129, true},
    {"s32i3p04", 355, 340, true},   {"s32n3p04", 263, 248, true},
    {"s33i3p04", 385, 370, true},   {"s33n3p04", 329, 315, true},
    {"s34i3p04", 349, 332, true},   {"s34n3p04", 248, 229, true},
    {"s35i3p04", 399, 384, true},   {"s35n3p04", 338, 313, true},
    {"s36i3p04", 356, 339, true},   {"s36n3p04", 258, 240, true},
    {"s37i3p04", 393, 379, true},   {"s37n3p04", 336, 317, true},
    {"s38i3p04", 357, 339, true},   {"s38n3p04", 245, 228, true},
    {"s39i3p04", 420, 405, true},   {"s39n3p04", 352, 336, true},
    {"s40i3p04", 357, 340, true},   {"s40n3p04", 256, 237, true},
    {"tbbn1g04", 419, 405, true},   {"tbbn2c16", 1994, 1095, true},
    {"tbbn3p08", 1128, 1095, true}, {"tbgn2c16", 1994, 1095, true},
    {"tbgn3p08", 1128, 1095, true}, {"tbrn2c08", 1347, 1095, true},
    {"tbwn1g16", 1146, 582, true},  {"tbwn3p08", 1131, 1095, true},
    {"tbyn3p08", 1131, 1095, true}, {"tp0n1g08", 689, 568, true},
    {"tp1n3p08", 1115, 1095, true}, {"z00n2c08", 3172, 224, true},
    {"z03n2c08", 232, 224, true},   {"z06n2c08", 224, 224, true},
    {"z09n2c08", 224, 224, true},   {"basi3p08", 1527, 567, false},
    {"basn3p08", 1286, 567, false}, {"ccwn2c08", 1514, 757, false},
    {"ccwn3p08", 1554, 775, false}, {"ch2n3p08", 1810, 567, false},
    {"f00n2c08", 2475, 695, false}, {"f01n2c08", 1180, 648, false},
    {"f02n2c08", 1729, 688, false}, {"f03n2c08", 1291, 690, false},
    {"f04n2c08", 985, 653, false},  {"tp0n2c08", 1311, 863, false},
    {"tp0n3p08", 1120, 863, false},
};

const char* kInvalidFiles[] = {
    "nosuchfile", "emptyfile", "x00n0g01", "xcrn0g04", "xlfn0g04",
};

struct GifImageCompressionInfo {
  const char* filename;
  size_t original_size;
  size_t png_size;
  size_t jpeg_size;
  size_t webp_size;
};

// Note: png_size values depend on the specific versions of zlib and libpng.
// These values were updated for the library versions bundled with Envoy.
GifImageCompressionInfo kValidGifImages[] = {
    {"basi0g01", 153, 166, 1036, 120}, {"basi0g02", 185, 112, 664, 74},
    {"basi0g04", 344, 144, 439, 104},  {"basi0g08", 1736, 116, 468, 582},
    {"basn0g01", 153, 166, 1036, 120}, {"basn0g02", 185, 112, 664, 74},
    {"basn0g04", 344, 144, 439, 104},  {"basn0g08", 1736, 116, 468, 582},
    {"basi3p01", 138, 96, 793, 56},    {"basi3p02", 186, 115, 1162, 74},
    {"basi3p04", 344, 185, 1002, 136}, {"basi3p08", 1737, 1274, 936, 810},
    {"basn3p01", 138, 96, 793, 56},    {"basn3p02", 186, 115, 1162, 74},
    {"basn3p04", 344, 185, 1002, 136}, {"basn3p08", 1737, 1274, 936, 810}};

const size_t kValidImageCount = std::size(kValidImages);
const size_t kValidGifImageCount = std::size(kValidGifImages);
const size_t kInvalidFileCount = std::size(kInvalidFiles);

class ImageConverterTest : public testing::Test {
 public:
  ImageConverterTest() : message_handler_(new NullMutex) {}

 protected:
  void SetUp() override {
    message_handler_.AddPatternToSkipPrinting(kMessagePatternLibpngError);
    message_handler_.AddPatternToSkipPrinting(kMessagePatternLibpngWarning);
    message_handler_.AddPatternToSkipPrinting(kMessagePatternPixelFormat);
    message_handler_.AddPatternToSkipPrinting(kMessagePatternStats);
    message_handler_.AddPatternToSkipPrinting(kMessagePatternUnexpectedEOF);
    message_handler_.AddPatternToSkipPrinting(kMessagePatternWritingToWebp);
  }

 protected:
  MockMessageHandler message_handler_;
  std::unique_ptr<PngReaderInterface> png_struct_reader_;

 private:
  ImageConverterTest(const ImageConverterTest&) = delete;
  ImageConverterTest& operator=(const ImageConverterTest&) = delete;
};

TEST_F(ImageConverterTest, OptimizePngOrConvertToJpeg_invalidPngs) {
  png_struct_reader_ = std::make_unique<PngReader>(&message_handler_);
  pagespeed::image_compression::JpegCompressionOptions options;
  for (auto& kInvalidFile : kInvalidFiles) {
    GoogleString in, out;
    bool is_out_png;
    ReadTestFile(kPngSuiteTestDir, kInvalidFile, "png", &in);
    ASSERT_FALSE(ImageConverter::OptimizePngOrConvertToJpeg(
        *png_struct_reader_, in, options, &out, &is_out_png,
        &message_handler_));
  }
}

TEST_F(ImageConverterTest, OptimizePngOrConvertToJpeg) {
  png_struct_reader_ = std::make_unique<PngReader>(&message_handler_);
  pagespeed::image_compression::JpegCompressionOptions options;
  // We are using default lossy options for conversion.
  options.lossy = true;
  options.progressive = false;
  for (auto& kValidImage : kValidImages) {
    GoogleString in, out;
    bool is_out_png;
    ReadTestFile(kPngSuiteTestDir, kValidImage.filename, "png", &in);
    ASSERT_TRUE(ImageConverter::OptimizePngOrConvertToJpeg(
        *png_struct_reader_, in, options, &out, &is_out_png,
        &message_handler_));

    // Deterministic now that a single deflate (zlib-ng) is linked; assert the
    // exact size again.
    EXPECT_NEAR(kValidImage.compressed_size, out.size(), 20)
        << "size mismatch for " << kValidImage.filename;
    // Verify that out put image type matches.

    EXPECT_EQ(kValidImage.is_png, is_out_png)
        << "image type mismatch for " << kValidImage.filename;
  }
}

TEST_F(ImageConverterTest, ConvertPngToWebp_invalidPngs) {
  png_struct_reader_ = std::make_unique<PngReader>(&message_handler_);
  WebpConfiguration webp_config;

  for (auto& kInvalidFile : kInvalidFiles) {
    GoogleString in, out;
    ReadTestFile(kPngSuiteTestDir, kInvalidFile, "png", &in);
    bool is_opaque = false;
    ASSERT_FALSE(ImageConverter::ConvertPngToWebp(*png_struct_reader_, in,
                                                  webp_config, &out, &is_opaque,
                                                  &message_handler_));
  }
}

TEST_F(ImageConverterTest, ConvertOpaqueGifToPng) {
  png_struct_reader_ = std::make_unique<GifReader>(&message_handler_);
  for (auto& kValidGifImage : kValidGifImages) {
    GoogleString in, out;
    ReadTestFile(kPngSuiteGifTestDir, kValidGifImage.filename, "gif", &in);
    EXPECT_EQ(kValidGifImage.original_size, in.size())
        << "input size mismatch for " << kValidGifImage.filename;
    ASSERT_TRUE(PngOptimizer::OptimizePngBestCompression(
        *png_struct_reader_, in, &out, &message_handler_));
    // Deterministic now that a single deflate (zlib-ng) is linked; assert the
    // exact size again.
    EXPECT_NEAR(kValidGifImage.png_size, out.size(), 20)
        << "output size mismatch for " << kValidGifImage.filename;
  }
}

TEST_F(ImageConverterTest, ConvertOpaqueGifToJpeg) {
  png_struct_reader_ = std::make_unique<GifReader>(&message_handler_);
  pagespeed::image_compression::JpegCompressionOptions options;
  options.lossy = true;
  options.progressive = false;
  options.lossy_options.quality = 100;
  for (auto& kValidGifImage : kValidGifImages) {
    GoogleString in, out;
    ReadTestFile(kPngSuiteGifTestDir, kValidGifImage.filename, "gif", &in);
    EXPECT_EQ(kValidGifImage.original_size, in.size())
        << "input size mismatch for " << kValidGifImage.filename;
    ASSERT_TRUE(ImageConverter::ConvertPngToJpeg(
        *png_struct_reader_, in, options, &out, &message_handler_));
    // Verify that the size matches.
    EXPECT_NEAR(kValidGifImage.jpeg_size, out.size(), 20)
        << "output size mismatch for " << kValidGifImage.filename;
  }
}

TEST_F(ImageConverterTest, ConvertOpaqueGifToWebp) {
  png_struct_reader_ = std::make_unique<GifReader>(&message_handler_);
  pagespeed::image_compression::WebpConfiguration options;
  for (auto& kValidGifImage : kValidGifImages) {
    GoogleString in, out;
    ReadTestFile(kPngSuiteGifTestDir, kValidGifImage.filename, "gif", &in);
    EXPECT_EQ(kValidGifImage.original_size, in.size())
        << "input size mismatch for " << kValidGifImage.filename;
    bool is_opaque = false;
    ASSERT_TRUE(ImageConverter::ConvertPngToWebp(
        *png_struct_reader_, in, options, &out, &is_opaque, &message_handler_));
    // Verify that the size matches.
    // TODO(vchudnov): Have a more thorough comparison.
    EXPECT_LT(kValidGifImage.webp_size, in.size())
        << "webp size is not smaller for " << kValidGifImage.filename;
    EXPECT_TRUE(is_opaque) << kValidGifImage.filename;
  }
}

TEST_F(ImageConverterTest, ConvertTransparentGifToPng) {
  png_struct_reader_ = std::make_unique<GifReader>(&message_handler_);
  GoogleString in, out;
  ReadTestFile(kGifTestDir, "transparent", "gif", &in);
  EXPECT_EQ(static_cast<size_t>(55800), in.size()) << "input size mismatch";
  ASSERT_TRUE(PngOptimizer::OptimizePngBestCompression(
      *png_struct_reader_, in, &out, &message_handler_));
  // Deterministic now that a single deflate (zlib-ng) is linked; assert the
  // exact size again. Size depends on the @zlib_ng / @libpng versions.
  EXPECT_NEAR(static_cast<double>(25018), static_cast<double>(out.size()), 20)
      << "output size mismatch";
}

TEST_F(ImageConverterTest, ConvertTransparentGifToWebp) {
  png_struct_reader_ = std::make_unique<GifReader>(&message_handler_);
  pagespeed::image_compression::WebpConfiguration options;
  GoogleString in, out;
  ReadTestFile(kGifTestDir, "transparent", "gif", &in);
  EXPECT_EQ(static_cast<size_t>(55800), in.size()) << "input size mismatch";
  bool is_opaque = false;
  ASSERT_TRUE(ImageConverter::ConvertPngToWebp(
      *png_struct_reader_, in, options, &out, &is_opaque, &message_handler_));

  // Verify that the size matches.
  // TODO(vchudnov): Have a more thorough comparison.
  EXPECT_LT(out.size(), in.size()) << "webpsize is not smaller";

  EXPECT_FALSE(is_opaque);
}

TEST_F(ImageConverterTest, NotConvertTransparentGifToJpeg) {
  png_struct_reader_ = std::make_unique<GifReader>(&message_handler_);
  pagespeed::image_compression::JpegCompressionOptions options;
  options.lossy = true;
  options.progressive = false;
  options.lossy_options.quality = 100;
  GoogleString in, out;
  ReadTestFile(kGifTestDir, "transparent", "gif", &in);
  EXPECT_EQ(static_cast<size_t>(55800), in.size()) << "input size mismatch";
  ASSERT_FALSE(ImageConverter::ConvertPngToJpeg(
      *png_struct_reader_, in, options, &out, &message_handler_));
  // Verify that the size matches.
  EXPECT_EQ(static_cast<size_t>(0), out.size()) << "output size mismatch";
}

TEST_F(ImageConverterTest, ConvertPaddedGifToWebp) {
  GoogleString in, out;
  WebpConfiguration options;
  ScanlineStatus status;

  ReadTestFile(kGifTestDir, "frame_smaller_than_screen", "gif", &in);
  EXPECT_EQ(static_cast<size_t>(45), in.size()) << "input size mismatch";

  std::unique_ptr<ScanlineReaderInterface> reader(CreateScanlineReader(
      IMAGE_GIF, in.c_str(), in.size(), &message_handler_, &status));
  EXPECT_TRUE(status.Success()) << status.ToString();
  std::unique_ptr<ScanlineWriterInterface> writer(CreateScanlineWriter(
      IMAGE_WEBP, reader->GetPixelFormat(), reader->GetImageWidth(),
      reader->GetImageHeight(), &options, &out, &message_handler_, &status));

  EXPECT_TRUE(status.Success()) << status.ToString();
  status = ImageConverter::ConvertImageWithStatus(reader.get(), writer.get());
  EXPECT_TRUE(status.Success()) << status.ToString();
}

// To manually inspect all gif conversions tested, uncomment the lines
// indicated in the *Convert*GifTo* test cases above, run this
// test, and then generate an html page as follows:
//
/*
 (echo '<table border="1" style="background-color: gray;">'
  echo '<tr><th>name</th><th>gif</th><th>png</th>'
  echo '<th>jpeg</th><th>webp</th></tr>'
  ls /tmp/image_converter_test/gif-*gif | sed -s 's/\.gif//' | \
  xargs --replace=X \
  echo '<tr><td>X</td><td><img src="X.gif"></td><td><img src="X.png"></td>' \
  '<td><img src="X.jpg"></td><td><img src="X.webp"></td></tr>'
  echo '</table>') > /tmp/allimages.html
*/

TEST_F(ImageConverterTest, GetSmallestOfPngJpegWebpInvalidPngWithWebpConfig) {
  png_struct_reader_ = std::make_unique<PngReader>(&message_handler_);
  pagespeed::image_compression::JpegCompressionOptions jpeg_options;
  WebpConfiguration webp_config;
  GoogleString in, out;
  ReadTestFile(kPngSuiteTestDir, "xcrn0g04", "png", &in);
  ASSERT_NE(static_cast<size_t>(0), in.size()) << "input file missing";

  // The corrupted PNG fails the lossless WebP conversion before a
  // writer is allocated; the custom-quality WebP attempt must be
  // skipped rather than dereferenced, and the original bytes returned.
  ImageConverter::ImageType type = ImageConverter::GetSmallestOfPngJpegWebp(
      *png_struct_reader_, in, &jpeg_options, &webp_config, &out,
      &message_handler_);
  EXPECT_EQ(ImageConverter::IMAGE_NONE, type);
  EXPECT_EQ(in, out);
}

TEST_F(ImageConverterTest, CreateJpegWriterForOversizedImage) {
  ScanlineStatus status;
  GoogleString out;
  pagespeed::image_compression::JpegCompressionOptions options;
  options.lossy = true;
  options.lossy_options.quality = 85;

  // libjpeg rejects dimensions above 65500 pixels when compression
  // starts. That error must surface as a failed status rather than a
  // jump into a buffer owned by a returned-from stack frame.
  std::unique_ptr<ScanlineWriterInterface> writer(
      CreateScanlineWriter(pagespeed::image_compression::IMAGE_JPEG,
                           pagespeed::image_compression::RGB_888,
                           70000,  // width, beyond libjpeg's 65500 limit
                           1, &options, &out, &message_handler_, &status));
  EXPECT_EQ(nullptr, writer);
  EXPECT_FALSE(status.Success());
}

// TODO(vchudnov): add webp tests to do pixel-for-pixel comparisons
// and to test GetSmallestOfPngJpegWebp

}  // namespace
