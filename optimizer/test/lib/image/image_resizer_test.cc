// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

#include "lib/image/image_resizer.h"

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <memory>
#include <string>

#include "absl/strings/str_format.h"
#include "gtest/gtest.h"
#include "lib/base/basictypes.h"
#include "lib/base/message_handler.h"
#include "lib/image/image_util.h"
#include "lib/image/jpeg_optimizer.h"
#include "lib/image/png_optimizer.h"
#include "lib/image/read_image.h"
#include "lib/image/scanline_interface.h"
#include "lib/image/webp_optimizer.h"
#include "test/lib/image/test_utils.h"

namespace {

// Pixel formats
using pagespeed::image_compression::GRAY_8;
using pagespeed::image_compression::PixelFormat;
using pagespeed::image_compression::RGB_888;
using pagespeed::image_compression::RGBA_8888;
// Readers and writers
using pagespeed::NullMessageHandler;
using pagespeed::image_compression::JpegCompressionOptions;
using pagespeed::image_compression::kMessagePatternPixelFormat;
using pagespeed::image_compression::kMessagePatternStats;
using pagespeed::image_compression::kMessagePatternUnexpectedEOF;
using pagespeed::image_compression::kMessagePatternWritingToWebp;
using pagespeed::image_compression::kPngSuiteTestDir;
using pagespeed::image_compression::kPngTestDir;
using pagespeed::image_compression::kResizedTestDir;
using pagespeed::image_compression::PngScanlineReaderRaw;
using pagespeed::image_compression::ReadTestFile;
using pagespeed::image_compression::ScanlineResizer;
using pagespeed::image_compression::WebpConfiguration;

using net_instaweb::ScanlineWriterInterface;

const size_t kPreserveAspectRatio =
    pagespeed::image_compression::ScanlineResizer::kPreserveAspectRatio;

// Three testing images: GRAY_8, RGB_888, and RGBA_8888.
// Size of these images is 32-by-32 pixels.
const char* kValidImages[] = {
    "basi0g04",
    "basi3p02",
    "basn6a16",
};

// Image of RGBA_8888 format. Size is 128-by-128 pixels.
const char kImagePagespeed[] = "pagespeed-128";
// Same content as pagespeed-128, but resized to 33-by-34 pixels.
const char kImagePageSpeed33x34[] = "pagespeed-33x34";
// Image with 4096-by-2048 pixels.
const char kLarge4096x2048[] = "large";

// Size of the output image [width, height]. The size of the input image
// is 32-by-32. We would like to test resizing ratios of both integers
// and non-integers.
const size_t kOutputSize[][2] = {
    {16, kPreserveAspectRatio},  // Shrink image by 2x in both directions.
    {kPreserveAspectRatio, 8},   // Shrink image by 4x in both directions.
    {3, 3},    // Shrink image by 32/3 times in both directions.
    {16, 25},  // Shrink image by [2, 32/25] times.
    {32, 5},   // Shrink image by [1, 32/5] times.
    {3, 32},   // Shrink image by [32/3, 1] times.
    {31, 31},  // Shrink image by [32/31, 32/31] times.
    {32, 32},  // Although the image did not shrink, the algorithm is exercised.
};

class ScanlineResizerTest : public testing::Test {
 public:
  ScanlineResizerTest()
      : message_handler_(),
        reader_(&message_handler_),
        resizer_(&message_handler_) {}

 protected:
  void InitializeReader(const char* file_name) {
    ASSERT_TRUE(
        ReadTestFile(kPngSuiteTestDir, file_name, "png", &input_image_));
    ASSERT_TRUE(reader_.Initialize(input_image_.data(), input_image_.length()));
  }

  void ResizeAndValidateImage(const char* file_name, const std::string& image);

  NullMessageHandler message_handler_;
  PngScanlineReaderRaw reader_;
  ScanlineResizer resizer_;
  std::string input_image_;
  void* scanline_{nullptr};

 private:
  ScanlineResizerTest(const ScanlineResizerTest&) = delete;
  ScanlineResizerTest& operator=(const ScanlineResizerTest&) = delete;
};

// Read the gold file. Size of the gold file is embedded in its name.
// For example 'testdata/resized/basi0g04_w16_h16.png'.
bool ReadGoldImageToString(const char* file_name, size_t width, size_t height,
                           std::string* image_data) {
  std::string gold_file_name =
      absl::StrFormat("%s_w%d_h%d", file_name, static_cast<int>(width),
                      static_cast<int>(height));
  return ReadTestFile(kResizedTestDir, gold_file_name.c_str(), "png",
                      image_data);
}

// Return JPEG writer for Gray_8, or WebP writer for RGB_888 or RGBA_8888.
ScanlineWriterInterface* CreateWriter(PixelFormat pixel_format, size_t width,
                                      size_t height, std::string* image_data,
                                      std::string* file_ext,
                                      pagespeed::MessageHandler* handler) {
  if (pixel_format == GRAY_8) {
    *file_ext = "jpg";
    JpegCompressionOptions jpeg_config;
    jpeg_config.lossy = true;
    jpeg_config.lossy_options.quality = 100;
    return reinterpret_cast<ScanlineWriterInterface*>(
        pagespeed::image_compression::CreateScanlineWriter(
            pagespeed::image_compression::IMAGE_JPEG, pixel_format, width,
            height, &jpeg_config, image_data, handler));
  } else {
    *file_ext = "webp";
    WebpConfiguration webp_config;  // Use lossless by default
    return reinterpret_cast<ScanlineWriterInterface*>(
        pagespeed::image_compression::CreateScanlineWriter(
            pagespeed::image_compression::IMAGE_WEBP, pixel_format, width,
            height, &webp_config, image_data, handler));
  }
}

// Make sure the resized results, include image size, pixel format, and pixel
// values, match the gold data. The gold data has the image size coded in the
// file name. For example, an image resized to 16-by-16 is
// test/lib/image/testdata/resized/basi0g04_w16_h16.png
void ScanlineResizerTest::ResizeAndValidateImage(const char* file_name,
                                                 const std::string& image) {
  PngScanlineReaderRaw gold_reader(&message_handler_);
  for (auto index_size : kOutputSize) {
    size_t width = index_size[0];
    size_t height = index_size[1];
    ASSERT_TRUE(reader_.Initialize(image.data(), image.length()));
    ASSERT_TRUE(resizer_.Initialize(&reader_, width, height));

    if (width == 0) width = height;
    if (height == 0) height = width;
    std::string gold_image;
    ASSERT_TRUE(ReadGoldImageToString(file_name, width, height, &gold_image));
    ASSERT_TRUE(gold_reader.Initialize(gold_image.data(), gold_image.length()));

    // Make sure the images sizes and the pixel formats are the same.
    ASSERT_EQ(gold_reader.GetImageWidth(), resizer_.GetImageWidth());
    ASSERT_EQ(gold_reader.GetImageHeight(), resizer_.GetImageHeight());
    ASSERT_EQ(gold_reader.GetPixelFormat(), resizer_.GetPixelFormat());

    while (resizer_.HasMoreScanLines() && gold_reader.HasMoreScanLines()) {
      uint8_t* resized_scanline = nullptr;
      uint8_t* gold_scanline = nullptr;
      ASSERT_TRUE(resizer_.ReadNextScanline(
          reinterpret_cast<void**>(&resized_scanline)));
      ASSERT_TRUE(gold_reader.ReadNextScanline(
          reinterpret_cast<void**>(&gold_scanline)));

      for (size_t i = 0; i < resizer_.GetBytesPerScanline(); ++i) {
        // Allow off-by-one tolerance for platform-specific floating point
        // rounding differences (e.g., ARM vs x86).
        ASSERT_LE(std::abs(static_cast<int>(gold_scanline[i]) -
                           static_cast<int>(resized_scanline[i])),
                  1)
            << "Pixel mismatch at byte " << i;
      }
    }

    // Make sure both the resizer and the reader have exhausted
    // scanlines.
    ASSERT_FALSE(resizer_.HasMoreScanLines());
    ASSERT_FALSE(gold_reader.HasMoreScanLines());
  }
}

TEST_F(ScanlineResizerTest, Accuracy) {
  // Test accuracy of resizing for some images in PNG Suite. All images
  // in this suite have 32-by-32 pixels.
  for (auto file_name : kValidImages) {
    ASSERT_TRUE(
        ReadTestFile(kPngSuiteTestDir, file_name, "png", &input_image_));
    ResizeAndValidateImage(file_name, input_image_);
  }

  // Test accuracy of resizing for an image with 33-by-34 pixels.
  ASSERT_TRUE(
      ReadTestFile(kPngTestDir, kImagePageSpeed33x34, "png", &input_image_));
  ResizeAndValidateImage(kImagePageSpeed33x34, input_image_);
}

// Resize the image and write the result to a JPEG or a WebP image.
TEST_F(ScanlineResizerTest, ResizeAndWrite) {
  for (auto file_name : kValidImages) {
    ASSERT_TRUE(
        ReadTestFile(kPngSuiteTestDir, file_name, "png", &input_image_));

    for (auto index_size : kOutputSize) {
      const size_t width = index_size[0];
      const size_t height = index_size[1];
      ASSERT_TRUE(
          reader_.Initialize(input_image_.data(), input_image_.length()));
      ASSERT_TRUE(resizer_.Initialize(&reader_, width, height));

      std::string output_image;
      std::string file_ext;
      std::unique_ptr<ScanlineWriterInterface> writer(
          CreateWriter(resizer_.GetPixelFormat(), resizer_.GetImageWidth(),
                       resizer_.GetImageHeight(), &output_image, &file_ext,
                       &message_handler_));

      while (resizer_.HasMoreScanLines()) {
        ASSERT_TRUE(resizer_.ReadNextScanline(&scanline_));
        ASSERT_TRUE(writer->WriteNextScanline(scanline_));
      }

      ASSERT_TRUE(writer->FinalizeWrite());
    }
  }
}

// Both width and height are specified.
TEST_F(ScanlineResizerTest, InitializeWidthHeight) {
  size_t width = 20;
  size_t height = 10;
  InitializeReader(kValidImages[0]);
  ASSERT_TRUE(resizer_.Initialize(&reader_, width, height));
  EXPECT_EQ(width, resizer_.GetImageWidth());
  EXPECT_EQ(height, resizer_.GetImageHeight());
  EXPECT_EQ(reader_.GetPixelFormat(), resizer_.GetPixelFormat());
}

// Only height is specified.
TEST_F(ScanlineResizerTest, InitializeHeight) {
  size_t width = kPreserveAspectRatio;
  size_t height = 10;
  InitializeReader(kValidImages[0]);
  ASSERT_TRUE(resizer_.Initialize(&reader_, width, height));
  EXPECT_EQ(height, resizer_.GetImageWidth());
  EXPECT_EQ(height, resizer_.GetImageHeight());
}

// Only width is specified.
TEST_F(ScanlineResizerTest, InitializeWidth) {
  size_t width = 12;
  size_t height = kPreserveAspectRatio;
  InitializeReader(kValidImages[0]);
  ASSERT_TRUE(resizer_.Initialize(&reader_, width, height));
  EXPECT_EQ(width, resizer_.GetImageWidth());
  EXPECT_EQ(width, resizer_.GetImageHeight());
}

// The resizer is not initialized, so ReadNextScanline returns false.
TEST_F(ScanlineResizerTest, ReadNullScanline) {
  ASSERT_FALSE(resizer_.ReadNextScanline(&scanline_));
}

// The resizer has only one scanline, so ReadNextScanline returns false
// at the second call.
TEST_F(ScanlineResizerTest, ReadNextScanline) {
  InitializeReader(kValidImages[1]);
  ASSERT_TRUE(resizer_.Initialize(&reader_, 10, 1));
  ASSERT_TRUE(resizer_.ReadNextScanline(&scanline_));
  ASSERT_FALSE(resizer_.ReadNextScanline(&scanline_));
}

// The original image is truncated. Only 100 bytes are passed to the reader.
// The reader is able decode the image header, but not the pixels.
// The resizer should return false when the reader fails.
TEST_F(ScanlineResizerTest, BadReader) {
  ASSERT_TRUE(
      ReadTestFile(kPngSuiteTestDir, kValidImages[0], "png", &input_image_));
  ASSERT_TRUE(reader_.Initialize(input_image_.data(), 100));
  ASSERT_TRUE(resizer_.Initialize(&reader_, 10, 20));
  ASSERT_FALSE(resizer_.ReadNextScanline(&scanline_));
}

// The resizer is initialized twice and only a portion of scanlines are
// read. The resizer should not have any error.
TEST_F(ScanlineResizerTest, PartialRead) {
  InitializeReader(kValidImages[0]);

  ASSERT_TRUE(resizer_.Initialize(&reader_, 10, 20));
  // Read only 1 scanline, although there are 20.
  EXPECT_TRUE(resizer_.ReadNextScanline(&scanline_));

  ASSERT_TRUE(resizer_.Initialize(&reader_, 10, 20));
  // Read only 2 scanlines, although there are 20.
  EXPECT_TRUE(resizer_.ReadNextScanline(&scanline_));
  EXPECT_TRUE(resizer_.ReadNextScanline(&scanline_));
}

// Resize the image by non-integer ratios.
TEST_F(ScanlineResizerTest, ResizeFractionalRatio) {
  const int new_width = 11;
  const int new_height = 19;
  ASSERT_TRUE(ReadTestFile(kPngTestDir, kImagePagespeed, "png", &input_image_));

  ASSERT_TRUE(reader_.Initialize(input_image_.data(), input_image_.length()));
  ASSERT_TRUE(resizer_.Initialize(&reader_, new_width, new_height));

  int num_rows = 0;
  while (resizer_.HasMoreScanLines()) {
    ASSERT_TRUE(resizer_.ReadNextScanline(&scanline_));
    ++num_rows;
  }
  EXPECT_EQ(new_height, num_rows);
}

TEST_F(ScanlineResizerTest, LargeImage) {
  ASSERT_TRUE(ReadTestFile(kPngTestDir, kLarge4096x2048, "png", &input_image_));
  ResizeAndValidateImage(kLarge4096x2048, input_image_);
}

// ==================================================================
// PixelBufferReader adapter for testing resizer with raw pixels.
// ==================================================================

using net_instaweb::SCANLINE_STATUS_INTERNAL_ERROR;
using net_instaweb::SCANLINE_STATUS_SUCCESS;
using net_instaweb::SCANLINE_UTIL;
using net_instaweb::ScanlineReaderInterface;
using net_instaweb::ScanlineStatus;

class PixelBufferReader : public ScanlineReaderInterface {
 public:
  PixelBufferReader(const uint8_t* pixels, size_t width, size_t height,
                    PixelFormat format)
      : pixels_(pixels), width_(width), height_(height), format_(format) {
    switch (format) {
      case GRAY_8:
        bpp_ = 1;
        break;
      case RGB_888:
        bpp_ = 3;
        break;
      case RGBA_8888:
        bpp_ = 4;
        break;
      default:
        bpp_ = 3;
        break;
    }
  }

  bool Reset() override {
    current_row_ = 0;
    return true;
  }

  size_t GetBytesPerScanline() override { return width_ * bpp_; }

  bool HasMoreScanLines() override { return current_row_ < height_; }

  ScanlineStatus InitializeWithStatus(const void* /*image_buffer*/,
                                      size_t /*buffer_length*/) override {
    current_row_ = 0;
    return ScanlineStatus(SCANLINE_STATUS_SUCCESS);
  }

  ScanlineStatus ReadNextScanlineWithStatus(
      void** out_scanline_bytes) override {
    if (current_row_ >= height_) {
      return ScanlineStatus(SCANLINE_STATUS_INTERNAL_ERROR, SCANLINE_UTIL,
                            "no more scanlines");
    }
    *out_scanline_bytes =
        const_cast<uint8_t*>(pixels_ + current_row_ * width_ * bpp_);
    ++current_row_;
    return ScanlineStatus(SCANLINE_STATUS_SUCCESS);
  }

  size_t GetImageHeight() override { return height_; }
  size_t GetImageWidth() override { return width_; }
  PixelFormat GetPixelFormat() override { return format_; }
  bool IsProgressive() override { return false; }

 private:
  const uint8_t* pixels_;
  size_t width_;
  size_t height_;
  PixelFormat format_;
  size_t bpp_;
  size_t current_row_{0};
};

// ==================================================================
// PixelBufferReader-based resizer tests.
// ==================================================================

class PixelBufferResizerTest : public testing::Test {
 protected:
  NullMessageHandler handler_;
  ScanlineResizer resizer_{&handler_};
};

// Solid gray image: resize from 8x8 to 4x4 via GRAY_8.
TEST_F(PixelBufferResizerTest, ResizeGray8) {
  const size_t w = 8, h = 8;
  std::vector<uint8_t> pixels(w * h, 128);
  PixelBufferReader reader(pixels.data(), w, h, GRAY_8);
  ASSERT_TRUE(resizer_.Initialize(&reader, 4, 4));
  EXPECT_EQ(resizer_.GetImageWidth(), 4u);
  EXPECT_EQ(resizer_.GetImageHeight(), 4u);
  EXPECT_EQ(resizer_.GetPixelFormat(), GRAY_8);

  int rows_read = 0;
  while (resizer_.HasMoreScanLines()) {
    void* scanline = nullptr;
    ASSERT_TRUE(resizer_.ReadNextScanline(&scanline));
    ASSERT_NE(scanline, nullptr);
    auto* data = static_cast<uint8_t*>(scanline);
    for (size_t x = 0; x < 4; ++x) {
      // Solid gray should stay close to 128.
      EXPECT_NEAR(data[x], 128, 2);
    }
    ++rows_read;
  }
  EXPECT_EQ(rows_read, 4);
}

// Solid RGB image: resize from 16x16 to 4x4.
TEST_F(PixelBufferResizerTest, ResizeRgb888) {
  const size_t w = 16, h = 16;
  std::vector<uint8_t> pixels(w * h * 3);
  for (size_t i = 0; i < pixels.size(); i += 3) {
    pixels[i] = 255;      // R
    pixels[i + 1] = 0;    // G
    pixels[i + 2] = 128;  // B
  }
  PixelBufferReader reader(pixels.data(), w, h, RGB_888);
  ASSERT_TRUE(resizer_.Initialize(&reader, 4, 4));
  EXPECT_EQ(resizer_.GetPixelFormat(), RGB_888);

  while (resizer_.HasMoreScanLines()) {
    void* scanline = nullptr;
    ASSERT_TRUE(resizer_.ReadNextScanline(&scanline));
    auto* data = static_cast<uint8_t*>(scanline);
    for (size_t x = 0; x < 4; ++x) {
      EXPECT_NEAR(data[x * 3], 255, 2);
      EXPECT_NEAR(data[x * 3 + 1], 0, 2);
      EXPECT_NEAR(data[x * 3 + 2], 128, 2);
    }
  }
}

// Solid RGBA: resize from 16x16 to 8x8.
TEST_F(PixelBufferResizerTest, ResizeRgba8888) {
  const size_t w = 16, h = 16;
  std::vector<uint8_t> pixels(w * h * 4);
  for (size_t i = 0; i < pixels.size(); i += 4) {
    pixels[i] = 100;
    pixels[i + 1] = 200;
    pixels[i + 2] = 50;
    pixels[i + 3] = 255;
  }
  PixelBufferReader reader(pixels.data(), w, h, RGBA_8888);
  ASSERT_TRUE(resizer_.Initialize(&reader, 8, 8));
  EXPECT_EQ(resizer_.GetPixelFormat(), RGBA_8888);

  int rows = 0;
  while (resizer_.HasMoreScanLines()) {
    void* scanline = nullptr;
    ASSERT_TRUE(resizer_.ReadNextScanline(&scanline));
    auto* data = static_cast<uint8_t*>(scanline);
    for (size_t x = 0; x < 8; ++x) {
      EXPECT_NEAR(data[x * 4], 100, 2);
      EXPECT_NEAR(data[x * 4 + 1], 200, 2);
      EXPECT_NEAR(data[x * 4 + 2], 50, 2);
      EXPECT_NEAR(data[x * 4 + 3], 255, 2);
    }
    ++rows;
  }
  EXPECT_EQ(rows, 8);
}

// 1x1 image: resize to 1x1 (identity).
TEST_F(PixelBufferResizerTest, Resize1x1) {
  uint8_t pixel[] = {42, 84, 126};
  PixelBufferReader reader(pixel, 1, 1, RGB_888);
  ASSERT_TRUE(resizer_.Initialize(&reader, 1, 1));
  EXPECT_EQ(resizer_.GetImageWidth(), 1u);
  EXPECT_EQ(resizer_.GetImageHeight(), 1u);

  void* scanline = nullptr;
  ASSERT_TRUE(resizer_.ReadNextScanline(&scanline));
  auto* data = static_cast<uint8_t*>(scanline);
  EXPECT_NEAR(data[0], 42, 1);
  EXPECT_NEAR(data[1], 84, 1);
  EXPECT_NEAR(data[2], 126, 1);
  EXPECT_FALSE(resizer_.HasMoreScanLines());
}

// Same-size resize (no-op).
TEST_F(PixelBufferResizerTest, SameSizeNoop) {
  const size_t w = 4, h = 4;
  std::vector<uint8_t> pixels(w * h * 3, 77);
  PixelBufferReader reader(pixels.data(), w, h, RGB_888);
  ASSERT_TRUE(resizer_.Initialize(&reader, w, h));
  EXPECT_EQ(resizer_.GetImageWidth(), w);
  EXPECT_EQ(resizer_.GetImageHeight(), h);

  int rows = 0;
  while (resizer_.HasMoreScanLines()) {
    void* scanline = nullptr;
    ASSERT_TRUE(resizer_.ReadNextScanline(&scanline));
    ++rows;
  }
  EXPECT_EQ(rows, static_cast<int>(h));
}

// Fractional resize ratio.
TEST_F(PixelBufferResizerTest, FractionalRatio) {
  const size_t w = 10, h = 10;
  std::vector<uint8_t> pixels(w * h * 3, 200);
  PixelBufferReader reader(pixels.data(), w, h, RGB_888);
  ASSERT_TRUE(resizer_.Initialize(&reader, 3, 7));
  EXPECT_EQ(resizer_.GetImageWidth(), 3u);
  EXPECT_EQ(resizer_.GetImageHeight(), 7u);

  int rows = 0;
  while (resizer_.HasMoreScanLines()) {
    void* scanline = nullptr;
    ASSERT_TRUE(resizer_.ReadNextScanline(&scanline));
    ++rows;
  }
  EXPECT_EQ(rows, 7);
}

// Resize with aspect ratio preservation (height=0).
TEST_F(PixelBufferResizerTest, PreserveAspectRatioWidth) {
  const size_t w = 20, h = 10;
  std::vector<uint8_t> pixels(w * h, 50);
  PixelBufferReader reader(pixels.data(), w, h, GRAY_8);
  ASSERT_TRUE(resizer_.Initialize(&reader, 10, kPreserveAspectRatio));
  EXPECT_EQ(resizer_.GetImageWidth(), 10u);
  EXPECT_EQ(resizer_.GetImageHeight(), 5u);
}

// Gradient image: verify resized values are averaged.
TEST_F(PixelBufferResizerTest, GradientAveraging) {
  // 4x1 GRAY_8: values 0, 100, 200, 255. Resize to 2x1.
  const size_t w = 4, h = 1;
  uint8_t pixels[] = {0, 100, 200, 255};
  PixelBufferReader reader(pixels, w, h, GRAY_8);
  ASSERT_TRUE(resizer_.Initialize(&reader, 2, 1));

  void* scanline = nullptr;
  ASSERT_TRUE(resizer_.ReadNextScanline(&scanline));
  auto* data = static_cast<uint8_t*>(scanline);
  // First pixel: avg(0, 100) ≈ 50
  EXPECT_NEAR(data[0], 50, 3);
  // Second pixel: avg(200, 255) ≈ 227
  EXPECT_NEAR(data[1], 227, 3);
}

// Null reader should fail initialization.
TEST_F(PixelBufferResizerTest, NullReaderFails) {
  EXPECT_FALSE(resizer_.Initialize(nullptr, 4, 4));
}

// Both dimensions kPreserveAspectRatio should fail.
TEST_F(PixelBufferResizerTest, BothDimensionsPreserveAspectRatio) {
  const size_t w = 10, h = 10;
  std::vector<uint8_t> pixels(w * h * 3, 128);
  PixelBufferReader reader(pixels.data(), w, h, RGB_888);
  EXPECT_FALSE(
      resizer_.Initialize(&reader, kPreserveAspectRatio, kPreserveAspectRatio));
}

// Calling InitializeWithStatus directly should return an error
// (the resizer expects Initialize() with a reader, not raw buffer).
TEST_F(PixelBufferResizerTest, InitializeWithStatusReturnsError) {
  auto status = resizer_.InitializeWithStatus(nullptr, 0);
  EXPECT_FALSE(status.Success());
}

// =============================================================================
// Coverage gap tests: invalid dimensions in CreateTableForAreaMethod and
// ComputeResizedSizeRatio
// =============================================================================

// Test: Both output_width and output_height are zero.
// This triggers the else branch in ComputeResizedSizeRatio (lines 171-175)
// where both resized_width and resized_height <= 0, causing ratio_x=0,
// ratio_y=0. This then cascades to CreateTableForAreaMethod with out_size=0
// and ratio=0, which hits the guard at lines 97-99.
TEST_F(PixelBufferResizerTest, BothOutputDimensionsZero) {
  const size_t w = 10, h = 10;
  std::vector<uint8_t> pixels(w * h * 3, 128);
  PixelBufferReader reader(pixels.data(), w, h, RGB_888);
  // Both dimensions zero (not kPreserveAspectRatio, just 0).
  EXPECT_FALSE(resizer_.Initialize(&reader, 0, 0));
}

// Test: Output width is zero, output height is kPreserveAspectRatio.
// Both are clamped: width to min(0,10)=0, height to min(kPreserveAspectRatio,10)=10.
// ComputeResizedSizeRatio with output_width=0 and output_height=10 follows
// the "resized_height > 0" path, computing ratio from height only. But since
// output_width=0 is not treated as kPreserveAspectRatio (it's literally 0),
// the ratio and resized_width become 0, which fails in CreateTableForAreaMethod.
TEST_F(PixelBufferResizerTest, ZeroWidthPreserveHeight) {
  const size_t w = 10, h = 10;
  std::vector<uint8_t> pixels(w * h * 3, 128);
  PixelBufferReader reader(pixels.data(), w, h, RGB_888);
  // Width=0 with height=kPreserveAspectRatio: this should fail because
  // output_width=0 leads to a zero-size table.
  EXPECT_FALSE(resizer_.Initialize(&reader, 0, kPreserveAspectRatio));
}

// Test: Request size larger than input. The resizer truncates to input size.
TEST_F(PixelBufferResizerTest, RequestSizeLargerThanInput) {
  const size_t w = 8, h = 8;
  std::vector<uint8_t> pixels(w * h * 3, 100);
  PixelBufferReader reader(pixels.data(), w, h, RGB_888);
  // Request 100x100 but input is 8x8 -> clamped to 8x8.
  ASSERT_TRUE(resizer_.Initialize(&reader, 100, 100));
  EXPECT_EQ(resizer_.GetImageWidth(), w);
  EXPECT_EQ(resizer_.GetImageHeight(), h);

  // Should produce correct output since it's effectively a no-op resize.
  int rows = 0;
  while (resizer_.HasMoreScanLines()) {
    void* scanline = nullptr;
    ASSERT_TRUE(resizer_.ReadNextScanline(&scanline));
    ++rows;
  }
  EXPECT_EQ(rows, static_cast<int>(h));
}

// Test: Only width larger than input (height ok).
TEST_F(PixelBufferResizerTest, WidthLargerHeightSmaller) {
  const size_t w = 10, h = 10;
  std::vector<uint8_t> pixels(w * h * 3, 150);
  PixelBufferReader reader(pixels.data(), w, h, RGB_888);
  // Width 20 (clamped to 10), height 5.
  ASSERT_TRUE(resizer_.Initialize(&reader, 20, 5));
  EXPECT_EQ(resizer_.GetImageWidth(), 10u);
  EXPECT_EQ(resizer_.GetImageHeight(), 5u);

  int rows = 0;
  while (resizer_.HasMoreScanLines()) {
    void* scanline = nullptr;
    ASSERT_TRUE(resizer_.ReadNextScanline(&scanline));
    ++rows;
  }
  EXPECT_EQ(rows, 5);
}

// Test: Only height larger than input (width ok).
TEST_F(PixelBufferResizerTest, HeightLargerWidthSmaller) {
  const size_t w = 10, h = 10;
  std::vector<uint8_t> pixels(w * h, 200);
  PixelBufferReader reader(pixels.data(), w, h, GRAY_8);
  // Width 3, height 100 (clamped to 10).
  ASSERT_TRUE(resizer_.Initialize(&reader, 3, 100));
  EXPECT_EQ(resizer_.GetImageWidth(), 3u);
  EXPECT_EQ(resizer_.GetImageHeight(), 10u);

  int rows = 0;
  while (resizer_.HasMoreScanLines()) {
    void* scanline = nullptr;
    ASSERT_TRUE(resizer_.ReadNextScanline(&scanline));
    ++rows;
  }
  EXPECT_EQ(rows, 10);
}

// Test: Resize with only vertical reduction (width=same, height=half).
// This exercises the code path where ratio_x == 1.0 but ratio_y != 1.0
// (uses uint8_t ResizeCol but skips ResizeRow).
TEST_F(PixelBufferResizerTest, OnlyVerticalResize) {
  const size_t w = 8, h = 16;
  std::vector<uint8_t> pixels(w * h * 3, 99);
  PixelBufferReader reader(pixels.data(), w, h, RGB_888);
  ASSERT_TRUE(resizer_.Initialize(&reader, w, h / 2));
  EXPECT_EQ(resizer_.GetImageWidth(), w);
  EXPECT_EQ(resizer_.GetImageHeight(), h / 2);

  int rows = 0;
  while (resizer_.HasMoreScanLines()) {
    void* scanline = nullptr;
    ASSERT_TRUE(resizer_.ReadNextScanline(&scanline));
    auto* data = static_cast<uint8_t*>(scanline);
    for (size_t x = 0; x < w * 3; ++x) {
      EXPECT_NEAR(data[x], 99, 2);
    }
    ++rows;
  }
  EXPECT_EQ(rows, static_cast<int>(h / 2));
}

// Test: Resize with only horizontal reduction (width=half, height=same).
// This exercises the code path where ratio_y == 1.0 but ratio_x != 1.0
// (only_scale_outputs_ path in ResizeCol).
TEST_F(PixelBufferResizerTest, OnlyHorizontalResize) {
  const size_t w = 16, h = 8;
  std::vector<uint8_t> pixels(w * h * 4);
  for (size_t i = 0; i < pixels.size(); i += 4) {
    pixels[i] = 80;
    pixels[i + 1] = 160;
    pixels[i + 2] = 240;
    pixels[i + 3] = 255;
  }
  PixelBufferReader reader(pixels.data(), w, h, RGBA_8888);
  ASSERT_TRUE(resizer_.Initialize(&reader, w / 2, h));
  EXPECT_EQ(resizer_.GetImageWidth(), w / 2);
  EXPECT_EQ(resizer_.GetImageHeight(), h);

  int rows = 0;
  while (resizer_.HasMoreScanLines()) {
    void* scanline = nullptr;
    ASSERT_TRUE(resizer_.ReadNextScanline(&scanline));
    auto* data = static_cast<uint8_t*>(scanline);
    for (size_t x = 0; x < w / 2; ++x) {
      EXPECT_NEAR(data[x * 4], 80, 2);
      EXPECT_NEAR(data[x * 4 + 1], 160, 2);
      EXPECT_NEAR(data[x * 4 + 2], 240, 2);
      EXPECT_NEAR(data[x * 4 + 3], 255, 2);
    }
    ++rows;
  }
  EXPECT_EQ(rows, static_cast<int>(h));
}

// Test: Non-integer resize ratio for GRAY_8 (exercises ResizeRowAreaGray).
TEST_F(PixelBufferResizerTest, GrayNonIntegerRatio) {
  const size_t w = 9, h = 9;
  std::vector<uint8_t> pixels(w * h, 64);
  PixelBufferReader reader(pixels.data(), w, h, GRAY_8);
  ASSERT_TRUE(resizer_.Initialize(&reader, 4, 4));
  EXPECT_EQ(resizer_.GetImageWidth(), 4u);
  EXPECT_EQ(resizer_.GetImageHeight(), 4u);
  EXPECT_EQ(resizer_.GetPixelFormat(), GRAY_8);

  int rows = 0;
  while (resizer_.HasMoreScanLines()) {
    void* scanline = nullptr;
    ASSERT_TRUE(resizer_.ReadNextScanline(&scanline));
    auto* data = static_cast<uint8_t*>(scanline);
    for (size_t x = 0; x < 4; ++x) {
      EXPECT_NEAR(data[x], 64, 2);
    }
    ++rows;
  }
  EXPECT_EQ(rows, 4);
}

// ==========================================================================
// Error path coverage: CreateTableForAreaMethod via Initialize
// ==========================================================================

// Upscale request: request_width > input_width. ScanlineResizer should
// truncate the request to the input size (not fail), but this exercises
// the boundary where ratio approaches 1.0 and the truncation path.
TEST_F(PixelBufferResizerTest, UpscaleRequestTruncatedToInput) {
  const size_t w = 4, h = 4;
  std::vector<uint8_t> pixels(w * h * 3, 100);
  PixelBufferReader reader(pixels.data(), w, h, RGB_888);
  // Request larger than input: should be clamped to 4x4 (no-op resize).
  ASSERT_TRUE(resizer_.Initialize(&reader, 8, 8));
  // Effective output should be capped at input dimensions.
  EXPECT_LE(resizer_.GetImageWidth(), w);
  EXPECT_LE(resizer_.GetImageHeight(), h);

  int rows = 0;
  while (resizer_.HasMoreScanLines()) {
    void* scanline = nullptr;
    ASSERT_TRUE(resizer_.ReadNextScanline(&scanline));
    ++rows;
  }
  EXPECT_EQ(rows, static_cast<int>(resizer_.GetImageHeight()));
}

// Height-only preserve aspect ratio: set width=kPreserve, height=some value.
TEST_F(PixelBufferResizerTest, PreserveAspectRatioHeight) {
  const size_t w = 10, h = 20;
  std::vector<uint8_t> pixels(w * h, 50);
  PixelBufferReader reader(pixels.data(), w, h, GRAY_8);
  ASSERT_TRUE(resizer_.Initialize(&reader, kPreserveAspectRatio, 10));
  EXPECT_EQ(resizer_.GetImageHeight(), 10u);
  // Width should be proportionally halved (5).
  EXPECT_EQ(resizer_.GetImageWidth(), 5u);
}

// Very large downscale ratio: exercises CreateTableForAreaMethod with
// large ratio values.
TEST_F(PixelBufferResizerTest, LargeDownscaleRatio) {
  const size_t w = 100, h = 100;
  std::vector<uint8_t> pixels(w * h, 128);
  PixelBufferReader reader(pixels.data(), w, h, GRAY_8);
  ASSERT_TRUE(resizer_.Initialize(&reader, 1, 1));
  EXPECT_EQ(resizer_.GetImageWidth(), 1u);
  EXPECT_EQ(resizer_.GetImageHeight(), 1u);

  void* scanline = nullptr;
  ASSERT_TRUE(resizer_.ReadNextScanline(&scanline));
  EXPECT_FALSE(resizer_.HasMoreScanLines());
}

// Asymmetric resize: wide input to narrow output.
TEST_F(PixelBufferResizerTest, AsymmetricResize) {
  const size_t w = 100, h = 4;
  std::vector<uint8_t> pixels(w * h * 4, 200);
  PixelBufferReader reader(pixels.data(), w, h, RGBA_8888);
  ASSERT_TRUE(resizer_.Initialize(&reader, 3, 2));
  EXPECT_EQ(resizer_.GetImageWidth(), 3u);
  EXPECT_EQ(resizer_.GetImageHeight(), 2u);

  int rows = 0;
  while (resizer_.HasMoreScanLines()) {
    void* scanline = nullptr;
    ASSERT_TRUE(resizer_.ReadNextScanline(&scanline));
    ++rows;
  }
  EXPECT_EQ(rows, 2);
}

// Same dimensions: 1:1 ratio exercises the shortcut path in Initialize
// where both ratios are 1.0.
TEST_F(PixelBufferResizerTest, SameDimensionsShortcut) {
  const size_t w = 8, h = 8;
  std::vector<uint8_t> pixels(w * h * 3);
  for (size_t i = 0; i < pixels.size(); i += 3) {
    pixels[i] = 10;
    pixels[i + 1] = 20;
    pixels[i + 2] = 30;
  }
  PixelBufferReader reader(pixels.data(), w, h, RGB_888);
  ASSERT_TRUE(resizer_.Initialize(&reader, w, h));
  EXPECT_EQ(resizer_.GetImageWidth(), w);
  EXPECT_EQ(resizer_.GetImageHeight(), h);

  int rows = 0;
  while (resizer_.HasMoreScanLines()) {
    void* scanline = nullptr;
    ASSERT_TRUE(resizer_.ReadNextScanline(&scanline));
    auto* data = static_cast<uint8_t*>(scanline);
    for (size_t x = 0; x < w; ++x) {
      EXPECT_EQ(data[x * 3], 10);
      EXPECT_EQ(data[x * 3 + 1], 20);
      EXPECT_EQ(data[x * 3 + 2], 30);
    }
    ++rows;
  }
  EXPECT_EQ(rows, static_cast<int>(h));
}

// Prime number dimensions: tests non-trivial fractional ratios
// that exercise accumulated rounding in CreateTableForAreaMethod.
TEST_F(PixelBufferResizerTest, PrimeDimensionResize) {
  const size_t w = 17, h = 13;
  std::vector<uint8_t> pixels(w * h, 100);
  PixelBufferReader reader(pixels.data(), w, h, GRAY_8);
  ASSERT_TRUE(resizer_.Initialize(&reader, 7, 5));
  EXPECT_EQ(resizer_.GetImageWidth(), 7u);
  EXPECT_EQ(resizer_.GetImageHeight(), 5u);

  int rows = 0;
  while (resizer_.HasMoreScanLines()) {
    void* scanline = nullptr;
    ASSERT_TRUE(resizer_.ReadNextScanline(&scanline));
    ++rows;
  }
  EXPECT_EQ(rows, 5);
}

// ==========================================================================
// Error path coverage: invalid channel count via UNSUPPORTED pixel format.
// This exercises line 373 of image_resizer.cc where num_channels is not
// 1, 3, or 4 (GetNumChannelsFromPixelFormat returns 0 for UNSUPPORTED).
// ==========================================================================

TEST_F(PixelBufferResizerTest, InvalidChannelCountUnsupportedFormat) {
  const size_t w = 8, h = 8;
  // Use UNSUPPORTED pixel format -- GetNumChannelsFromPixelFormat returns 0.
  std::vector<uint8_t> pixels(w * h * 3, 128);
  PixelBufferReader reader(pixels.data(), w, h,
                           pagespeed::image_compression::UNSUPPORTED);
  // Initialize should fail because 0 channels is not valid for ResizeRowArea.
  EXPECT_FALSE(resizer_.Initialize(&reader, 4, 4));
}

// ==========================================================================
// Error path coverage: Zero reader dimensions.
// A reader with width=0 or height=0 should cause Initialize to fail
// (lines 694-697 of image_resizer.cc).
// ==========================================================================

class ZeroDimensionReader : public ScanlineReaderInterface {
 public:
  ZeroDimensionReader(size_t w, size_t h, PixelFormat fmt)
      : w_(w), h_(h), fmt_(fmt) {}
  bool Reset() override { return true; }
  size_t GetBytesPerScanline() override { return w_ * 3; }
  bool HasMoreScanLines() override { return false; }
  ScanlineStatus InitializeWithStatus(const void*, size_t) override {
    return ScanlineStatus(SCANLINE_STATUS_SUCCESS);
  }
  ScanlineStatus ReadNextScanlineWithStatus(void**) override {
    return ScanlineStatus(SCANLINE_STATUS_INTERNAL_ERROR, SCANLINE_UTIL,
                          "no data");
  }
  size_t GetImageHeight() override { return h_; }
  size_t GetImageWidth() override { return w_; }
  PixelFormat GetPixelFormat() override { return fmt_; }
  bool IsProgressive() override { return false; }

 private:
  size_t w_;
  size_t h_;
  PixelFormat fmt_;
};

// Reader with zero width should fail initialization.
TEST_F(PixelBufferResizerTest, ZeroWidthReader) {
  ZeroDimensionReader reader(0, 10, RGB_888);
  EXPECT_FALSE(resizer_.Initialize(&reader, 0, 5));
}

// Reader with zero height should fail initialization.
TEST_F(PixelBufferResizerTest, ZeroHeightReader) {
  ZeroDimensionReader reader(10, 0, RGB_888);
  EXPECT_FALSE(resizer_.Initialize(&reader, 5, 0));
}

// Reader with both zero dimensions should fail initialization.
TEST_F(PixelBufferResizerTest, ZeroWidthAndHeightReader) {
  ZeroDimensionReader reader(0, 0, RGB_888);
  EXPECT_FALSE(resizer_.Initialize(&reader, 0, 0));
}

// ==========================================================================
// Error path coverage: extreme resize ratios that stress the table builder.
// Very large input with very small output exercises accumulated rounding
// in CreateTableForAreaMethod (lines 108-143 of image_resizer.cc).
// ==========================================================================

// Large downscale: 10000 -> 1 in one dimension (ratio = 10000.0).
TEST_F(PixelBufferResizerTest, ExtremeDownscaleRatio) {
  const size_t w = 10000, h = 2;
  std::vector<uint8_t> pixels(w * h, 64);
  PixelBufferReader reader(pixels.data(), w, h, GRAY_8);
  ASSERT_TRUE(resizer_.Initialize(&reader, 1, 1));
  EXPECT_EQ(resizer_.GetImageWidth(), 1u);
  EXPECT_EQ(resizer_.GetImageHeight(), 1u);

  void* scanline = nullptr;
  ASSERT_TRUE(resizer_.ReadNextScanline(&scanline));
  EXPECT_FALSE(resizer_.HasMoreScanLines());
}

// Large downscale in both dimensions with RGB.
TEST_F(PixelBufferResizerTest, ExtremeDownscaleRgb) {
  const size_t w = 5000, h = 5000;
  // Allocate a large buffer -- uniform color.
  std::vector<uint8_t> pixels(w * h * 3, 200);
  PixelBufferReader reader(pixels.data(), w, h, RGB_888);
  ASSERT_TRUE(resizer_.Initialize(&reader, 1, 1));
  EXPECT_EQ(resizer_.GetImageWidth(), 1u);
  EXPECT_EQ(resizer_.GetImageHeight(), 1u);

  void* scanline = nullptr;
  ASSERT_TRUE(resizer_.ReadNextScanline(&scanline));
  EXPECT_FALSE(resizer_.HasMoreScanLines());
  // Verify the single pixel is close to 200 for all channels.
  auto* data = static_cast<uint8_t*>(scanline);
  EXPECT_NEAR(data[0], 200, 3);
  EXPECT_NEAR(data[1], 200, 3);
  EXPECT_NEAR(data[2], 200, 3);
}

// Non-trivial resize with ratio producing near-integer boundaries.
// This stresses the IsApproximatelyInteger check in CreateTableForAreaMethod.
TEST_F(PixelBufferResizerTest, NearIntegerBoundaryRatio) {
  // 1000 -> 333 gives ratio ~3.003003..., very close to integer boundaries.
  const size_t w = 1000, h = 4;
  std::vector<uint8_t> pixels(w * h, 100);
  PixelBufferReader reader(pixels.data(), w, h, GRAY_8);
  ASSERT_TRUE(resizer_.Initialize(&reader, 333, 2));
  EXPECT_EQ(resizer_.GetImageWidth(), 333u);
  EXPECT_EQ(resizer_.GetImageHeight(), 2u);

  int rows = 0;
  while (resizer_.HasMoreScanLines()) {
    void* scanline = nullptr;
    ASSERT_TRUE(resizer_.ReadNextScanline(&scanline));
    ++rows;
  }
  EXPECT_EQ(rows, 2);
}

// ==========================================================================
// Error path coverage: UNSUPPORTED pixel format with non-trivial resize.
// This ensures the ResizeRowArea::Initialize returns false (line 373)
// even when tables would otherwise be valid.
// ==========================================================================

TEST_F(PixelBufferResizerTest, UnsupportedFormatWithValidDimensions) {
  const size_t w = 16, h = 16;
  std::vector<uint8_t> pixels(w * h * 3, 128);
  PixelBufferReader reader(pixels.data(), w, h,
                           pagespeed::image_compression::UNSUPPORTED);
  // Both horizontal and vertical resize requested; fails at channel validation.
  EXPECT_FALSE(resizer_.Initialize(&reader, 8, 8));
}

// UNSUPPORTED format with only vertical resize (ratio_x == 1.0).
// This exercises the uint8_t ResizeCol path with bad channel count.
TEST_F(PixelBufferResizerTest, UnsupportedFormatVerticalOnly) {
  const size_t w = 8, h = 16;
  std::vector<uint8_t> pixels(w * h * 3, 128);
  PixelBufferReader reader(pixels.data(), w, h,
                           pagespeed::image_compression::UNSUPPORTED);
  EXPECT_FALSE(resizer_.Initialize(&reader, w, 8));
}

// ==========================================================================
// Error path coverage: Large output dimensions with aspect ratio.
// When only one dimension is specified via kPreserveAspectRatio, the
// computed output for the other dimension could be very small, exercising
// edge cases in ComputeResizedSizeRatio.
// ==========================================================================

TEST_F(PixelBufferResizerTest, PreserveAspectRatioVeryWide) {
  const size_t w = 10000, h = 10;
  std::vector<uint8_t> pixels(w * h, 150);
  PixelBufferReader reader(pixels.data(), w, h, GRAY_8);
  // Only specify height=5; width computed proportionally: 10000 * 5/10 = 5000.
  ASSERT_TRUE(resizer_.Initialize(&reader, kPreserveAspectRatio, 5));
  EXPECT_EQ(resizer_.GetImageHeight(), 5u);
  EXPECT_EQ(resizer_.GetImageWidth(), 5000u);

  int rows = 0;
  while (resizer_.HasMoreScanLines()) {
    void* scanline = nullptr;
    ASSERT_TRUE(resizer_.ReadNextScanline(&scanline));
    ++rows;
  }
  EXPECT_EQ(rows, 5);
}

TEST_F(PixelBufferResizerTest, PreserveAspectRatioVeryTall) {
  const size_t w = 10, h = 10000;
  std::vector<uint8_t> pixels(w * h, 150);
  PixelBufferReader reader(pixels.data(), w, h, GRAY_8);
  // Only specify width=5; height computed proportionally: 10000 * 5/10 = 5000.
  ASSERT_TRUE(resizer_.Initialize(&reader, 5, kPreserveAspectRatio));
  EXPECT_EQ(resizer_.GetImageWidth(), 5u);
  EXPECT_EQ(resizer_.GetImageHeight(), 5000u);

  int rows = 0;
  while (resizer_.HasMoreScanLines()) {
    void* scanline = nullptr;
    ASSERT_TRUE(resizer_.ReadNextScanline(&scanline));
    ++rows;
  }
  EXPECT_EQ(rows, 5000);
}

}  // namespace
