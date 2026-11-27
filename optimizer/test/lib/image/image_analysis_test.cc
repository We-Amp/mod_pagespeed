// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

// Tests for image analysis: gradient, histogram, photo detection.
// Ported from mod_pagespeed's
// test/pagespeed/kernel/image/image_analysis_test.cc
// GIF-related tests have been removed.

#include "lib/image/image_analysis.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>

#include "gtest/gtest.h"
#include "lib/base/basictypes.h"
#include "lib/base/message_handler.h"
#include "lib/image/image_util.h"
#include "lib/image/read_image.h"
#include "lib/image/scanline_interface.h"
#include "lib/image/scanline_utils.h"
#include "test/lib/image/test_utils.h"

namespace {

using net_instaweb::ScanlineReaderInterface;
using pagespeed::NullMessageHandler;
using pagespeed::image_compression::AnalyzeImage;
using pagespeed::image_compression::CreateScanlineReader;
using pagespeed::image_compression::GRAY_8;
using pagespeed::image_compression::Histogram;
using pagespeed::image_compression::IMAGE_GIF;
using pagespeed::image_compression::IMAGE_JPEG;
using pagespeed::image_compression::IMAGE_PNG;
using pagespeed::image_compression::IMAGE_WEBP;
using pagespeed::image_compression::ImageFormat;
using pagespeed::image_compression::kJpegTestDir;
using pagespeed::image_compression::kNumColorHistogramBins;
using pagespeed::image_compression::kPngSuiteTestDir;
using pagespeed::image_compression::PhotoMetric;
using pagespeed::image_compression::PixelFormat;
using pagespeed::image_compression::ReadImage;
using pagespeed::image_compression::ReadTestFile;
using pagespeed::image_compression::RGB_888;
using pagespeed::image_compression::RGBA_8888;
using pagespeed::image_compression::SobelGradient;
using pagespeed::image_compression::SynthesizeImage;

struct ImageInfo {
  const char* file_name;
  int width;
  int height;
  bool is_progressive;
  bool is_animated;
  bool is_photo;
  bool has_transparency;
  int quality;
};

const ImageInfo kJpegImages[] = {
    {"quality100", 200, 200, false, false, false, false, 100},
    {"progressive", 200, 200, true, false, false, false, 100},
    {"sjpeg1", 120, 90, false, false, false, false, 93},
    {"sjpeg6", 512, 512, false, false, false, false, 100},
    {"sjpeg3", 512, 384, false, false, true, false, 89},
    {"sjpeg4", 512, 384, false, false, true, false, 100},
    {"already_optimized", 130, 97, false, false, true, false, 85},
    {"test444", 130, 97, false, false, true, false, 85},
};
const size_t kJpegImageCount = arraysize(kJpegImages);
// In kJpegImages[], the images at position kJpegImageFirstPhotoIdx
// and later are photos.
const size_t kJpegImageFirstPhotoIdx = 4;

const ImageInfo kPngImages[] = {
    {"basi0g04", 32, 32, true, false, false, false, -1},
    {"basi3p02", 32, 32, true, false, false, false, -1},
    {"basn6a16", 32, 32, false, false, false, true, -1},
};
const size_t kPngImageCount = arraysize(kPngImages);

class ImageAnalysisTest : public testing::Test {
 public:
  ImageAnalysisTest() {
    // Initialize the expected histogram.
    for (float& i : expected_hist_) {
      i = 0.0f;
    }
  }

 protected:
  // Synthesize an image; compute its gradient; and verify that the
  // gradient has the expected value.
  void TestGradient(int width, int height, PixelFormat pixel_format,
                    int bytes_per_line, const uint8_t* seed_value,
                    const int* delta_x, const int* delta_y,
                    const uint8_t* expected_gradient) {
    const int num_channels =
        GetNumChannelsFromPixelFormat(pixel_format, &message_handler_);

    // Synthesize the image.
    std::unique_ptr<uint8_t[]> image(
        new uint8_t[static_cast<unsigned long>(bytes_per_line * height)]);
    SynthesizeImage(width, height, bytes_per_line, num_channels, seed_value,
                    delta_x, delta_y, image.get());

    // Compute gradient.
    std::unique_ptr<uint8_t[]> gradient(
        new uint8_t[static_cast<unsigned long>(width * height)]);
    ASSERT_TRUE(SobelGradient(image.get(), width, height, bytes_per_line,
                              pixel_format, &message_handler_, gradient.get()));

    // Verify the gradient.
    EXPECT_EQ(0, memcmp(gradient.get(), expected_gradient,
                        static_cast<unsigned long>(width * height) *
                            sizeof(gradient[0])));
  }

  void VerifyKeyInformation(ImageFormat image_format, const char* dir,
                            const char* ext, const ImageInfo* images,
                            int num_images) {
    for (size_t i = 0; i < static_cast<size_t>(num_images); ++i) {
      std::string image_string;
      ASSERT_TRUE(ReadTestFile(dir, images[i].file_name, ext, &image_string));

      int width = -1;
      int height = -1;
      bool is_progressive = false;
      bool is_animated = false;
      bool has_transparency = false;
      bool is_photo = false;
      int quality = -1;
      ScanlineReaderInterface* reader = nullptr;

      EXPECT_TRUE(AnalyzeImage(
          image_format, image_string.data(), image_string.length(), &width,
          &height, &is_progressive, &is_animated, &has_transparency, &is_photo,
          &quality, &reader, &message_handler_));
      EXPECT_EQ(images[i].width, width);
      EXPECT_EQ(images[i].height, height);
      EXPECT_EQ(images[i].is_progressive, is_progressive);
      EXPECT_EQ(images[i].is_animated, is_animated);
      EXPECT_EQ(images[i].has_transparency, has_transparency);
      EXPECT_EQ(images[i].quality, quality);

      bool expected_is_photo = images[i].is_photo;
      if (image_format == IMAGE_JPEG) {
        expected_is_photo = true;
      }
      EXPECT_EQ(expected_is_photo, is_photo);

      if (is_animated || image_format != IMAGE_JPEG) {
        EXPECT_EQ(nullptr, reader);
      } else {
        EXPECT_NE(nullptr, reader);
      }
      delete reader;
    }
  }

 protected:
  NullMessageHandler message_handler_;
  float expected_hist_[kNumColorHistogramBins];

 private:
  DISALLOW_COPY_AND_ASSIGN(ImageAnalysisTest);
};

TEST_F(ImageAnalysisTest, GradientOfWhiteImage) {
  int width = 9;
  int height = 5;
  int bytes_per_line = 12;
  const uint8_t seed_value[] = {255};
  const int delta_x[] = {0};
  const int delta_y[] = {0};
  const PixelFormat pixel_format = GRAY_8;
  const int num_channels =
      GetNumChannelsFromPixelFormat(pixel_format, &message_handler_);

  uint8_t seed[] = {0};
  std::unique_ptr<uint8_t[]> expected_gradient(
      new uint8_t[static_cast<unsigned long>(width * height)]);
  SynthesizeImage(width, height, width, num_channels, seed, delta_x, delta_y,
                  expected_gradient.get());

  TestGradient(width, height, GRAY_8, bytes_per_line, seed_value, delta_x,
               delta_y, expected_gradient.get());
}

TEST_F(ImageAnalysisTest, GradientOfIncreasingPixelValues) {
  int width = 11;
  int height = 6;
  const uint8_t seed_value[] = {0, 20, 40, 100};
  const int delta_x[] = {1, 2, 3, 24};
  const int delta_y[] = {10, 20, 30, 123};
  const PixelFormat pixel_format[] = {RGB_888, RGBA_8888};
  const int bytes_per_line[] = {36, 44};

  // Generate ground truth. The synthesized image has RGB channels,
  // so the gradient is equal to
  //   max( 2 * (delta_x[0] + delta_x[1] + delta_x[2]) / 3,
  //        2 * (delta_y[0] + delta_y[1] + delta_y[2]) / 3 )
  // which is 40.
  std::unique_ptr<uint8_t[]> expected_gradient(
      new uint8_t[static_cast<size_t>(width) * height]);
  memset(expected_gradient.get(), 0, static_cast<size_t>(width) * height);
  for (int y = 1; y < height - 1; ++y) {
    memset(expected_gradient.get() + static_cast<size_t>(y) * width + 1, 40,
           width - 2);
  }

  // Test the gradient computed for two pixel formats: RGB_888 and
  // RGBA_8888. Since the alpha channel is ignored, both formats
  // have the same gradient.
  for (int format = 0; format < 2; ++format) {
    TestGradient(width, height, pixel_format[format], bytes_per_line[format],
                 seed_value, delta_x, delta_y, expected_gradient.get());
  }
}

TEST_F(ImageAnalysisTest, GradientOfFluctuatingPixelValues) {
  int width = 6;
  int height = 5;
  const uint8_t seed_value[] = {128, 128, 128, 100};
  const int delta_x[] = {-30, 45, -51, 24};
  const int delta_y[] = {-42, -20, 50, 123};
  const int bytes_per_line[] = {18, 24};
  const PixelFormat pixel_format[] = {RGB_888, RGBA_8888};

  const uint8_t expected_gradient[] = {0,  0,  0,  0,  0,  0,  0,  14, 69, 56,
                                       14, 0,  0,  70, 69, 47, 54, 0,  0,  104,
                                       20, 47, 89, 0,  0,  0,  0,  0,  0,  0};

  // Test the gradient computed for two pixel formats: RGB_888 and
  // RGBA_8888. Since the alpha channel is ignored, both formats
  // have the same gradient.
  for (int format = 0; format < 2; ++format) {
    TestGradient(width, height, pixel_format[format], bytes_per_line[format],
                 seed_value, delta_x, delta_y, expected_gradient);
  }
}

TEST_F(ImageAnalysisTest, HistogramOfBlankImage) {
  int width = 9;
  int height = 5;
  int bytes_per_line = 12;
  int num_channels = 1;
  const uint8_t seed_value[] = {123};
  const int delta_x[] = {0};
  const int delta_y[] = {0};

  std::unique_ptr<uint8_t[]> image(
      new uint8_t[static_cast<unsigned long>(bytes_per_line * height)]);
  SynthesizeImage(width, height, bytes_per_line, num_channels, seed_value,
                  delta_x, delta_y, image.get());

  float hist[kNumColorHistogramBins];
  const int x0 = 1;
  const int y0 = 2;

  // Ground truth. All of the pixels have value of 123, so only one
  // bin has non-zero value.
  expected_hist_[seed_value[0]] = (width - x0) * (height - y0);

  Histogram(image.get(), width - x0, height - y0, bytes_per_line, x0, y0, hist);
  EXPECT_EQ(
      0,
      memcmp(expected_hist_,  // NOLINT(bugprone-suspicious-memory-comparison)
             hist, kNumColorHistogramBins * sizeof(hist[0])));
}

TEST_F(ImageAnalysisTest, HistogramOfIncreasingPixelValues) {
  int width = 9;
  int height = 5;
  int bytes_per_line = 12;
  int num_channels = 1;
  const uint8_t seed_value[] = {123};
  const int delta_x[] = {1};
  const int delta_y[] = {9};

  std::unique_ptr<uint8_t[]> image(
      new uint8_t[static_cast<unsigned long>(bytes_per_line * height)]);
  SynthesizeImage(width, height, bytes_per_line, num_channels, seed_value,
                  delta_x, delta_y, image.get());

  // Generate ground truth. The pixels have contiguous values, so
  // the histogram has a flat region.
  for (int i = seed_value[0]; i < seed_value[0] + width * height; ++i) {
    expected_hist_[i] = 1.0f;
  }

  float hist[kNumColorHistogramBins];
  Histogram(image.get(), width, height, bytes_per_line, 0, 0, hist);
  EXPECT_EQ(
      0,
      memcmp(expected_hist_,  // NOLINT(bugprone-suspicious-memory-comparison)
             hist, kNumColorHistogramBins * sizeof(hist[0])));
}

TEST_F(ImageAnalysisTest, PhotoMetric) {
  const float thresholds[] = {
      0.005f,
      0.01f,
      0.02f,
      0.03f,
  };

  float metric[kJpegImageCount];
  for (float thr : thresholds) {
    for (size_t j = 0; j < kJpegImageCount; ++j) {
      std::string image_string;
      ASSERT_TRUE(ReadTestFile(kJpegTestDir, kJpegImages[j].file_name, "jpg",
                               &image_string));

      size_t width, height, bytes_per_line;
      uint8_t* image;
      PixelFormat pixel_format;
      ASSERT_TRUE(
          ReadImage(IMAGE_JPEG, image_string.data(), image_string.length(),
                    reinterpret_cast<void**>(&image), &pixel_format, &width,
                    &height, &bytes_per_line, &message_handler_));

      metric[j] = pagespeed::image_compression::PhotoMetric(
          image, width, height, bytes_per_line, pixel_format, thr,
          &message_handler_);
      free(image);
    }

    // Verify that the metrics for all graphics are smaller than
    // those for photos.
    float max_graphics_metric =
        *std::max_element(metric, metric + kJpegImageFirstPhotoIdx);
    float min_photo_metric = *std::min_element(metric + kJpegImageFirstPhotoIdx,
                                               metric + kJpegImageCount);
    EXPECT_LT(max_graphics_metric, min_photo_metric);
  }
}

TEST_F(ImageAnalysisTest, KeyInformation_JPEG) {
  VerifyKeyInformation(IMAGE_JPEG, kJpegTestDir, "jpg", kJpegImages,
                       kJpegImageCount);
}

TEST_F(ImageAnalysisTest, KeyInformation_PNG) {
  VerifyKeyInformation(IMAGE_PNG, kPngSuiteTestDir, "png", kPngImages,
                       kPngImageCount);
}

// ==========================================================================
// Error path coverage: SobelGradient
// ==========================================================================

TEST_F(ImageAnalysisTest, SobelGradient_TooSmallWidth) {
  // Width < 3 should return false.
  std::vector<uint8_t> image(static_cast<size_t>(2) * 10, 128);
  std::vector<uint8_t> gradient(static_cast<size_t>(2) * 10);
  EXPECT_FALSE(SobelGradient(image.data(), 2, 10, 2, GRAY_8, &message_handler_,
                             gradient.data()));
}

TEST_F(ImageAnalysisTest, SobelGradient_TooSmallHeight) {
  // Height < 3 should return false.
  std::vector<uint8_t> image(static_cast<size_t>(10) * 2, 128);
  std::vector<uint8_t> gradient(static_cast<size_t>(10) * 2);
  EXPECT_FALSE(SobelGradient(image.data(), 10, 2, 10, GRAY_8, &message_handler_,
                             gradient.data()));
}

TEST_F(ImageAnalysisTest, SobelGradient_InvalidPixelFormat) {
  // UNSUPPORTED pixel format should return false.
  std::vector<uint8_t> image(static_cast<size_t>(10) * 10, 128);
  std::vector<uint8_t> gradient(static_cast<size_t>(10) * 10);
  EXPECT_FALSE(SobelGradient(image.data(), 10, 10, 10,
                             pagespeed::image_compression::UNSUPPORTED,
                             &message_handler_, gradient.data()));
}

TEST_F(ImageAnalysisTest, SobelGradient_Gray8Works) {
  // 4x4 GRAY_8 image with a vertical edge: left half dark, right half bright.
  // Sobel should detect the edge in the interior pixels.
  const int w = 4, h = 4;
  uint8_t image[16] = {
      0, 0, 255, 255,  // row 0
      0, 0, 255, 255,  // row 1
      0, 0, 255, 255,  // row 2
      0, 0, 255, 255,  // row 3
  };
  uint8_t gradient[16] = {};
  EXPECT_TRUE(
      SobelGradient(image, w, h, w, GRAY_8, &message_handler_, gradient));
  // Interior pixels at the edge boundary should have non-zero gradient.
  // Pixel (1,1) is at the edge between dark(0) and bright(255).
  EXPECT_GT(gradient[1 * w + 1], 0u);
  EXPECT_GT(gradient[2 * w + 1], 0u);
}

TEST_F(ImageAnalysisTest, SobelGradient_RGB888Works) {
  // 4x4 RGB_888 image: should succeed.
  const int w = 4, h = 4;
  std::vector<uint8_t> image(static_cast<size_t>(w) * h * 3, 128);
  // Put a bright pixel in the center.
  image[static_cast<size_t>(1 * w + 1) * 3] = 255;
  image[static_cast<size_t>(1 * w + 1) * 3 + 1] = 255;
  image[static_cast<size_t>(1 * w + 1) * 3 + 2] = 255;
  std::vector<uint8_t> gradient(static_cast<size_t>(w) * h);
  EXPECT_TRUE(SobelGradient(image.data(), w, h, w * 3, RGB_888,
                            &message_handler_, gradient.data()));
}

TEST_F(ImageAnalysisTest, SobelGradient_RGBA8888Works) {
  // 4x4 RGBA_8888 image: should succeed.
  const int w = 4, h = 4;
  std::vector<uint8_t> image(static_cast<size_t>(w) * h * 4, 128);
  std::vector<uint8_t> gradient(static_cast<size_t>(w) * h);
  EXPECT_TRUE(SobelGradient(image.data(), w, h, w * 4, RGBA_8888,
                            &message_handler_, gradient.data()));
}

TEST_F(ImageAnalysisTest, SobelGradient_UniformImageProducesZeroGradient) {
  // A uniform 5x5 GRAY_8 image should produce zero gradient everywhere.
  const int w = 5, h = 5;
  std::vector<uint8_t> image(static_cast<size_t>(w) * h, 100);
  std::vector<uint8_t> gradient(static_cast<size_t>(w) * h, 255);
  EXPECT_TRUE(SobelGradient(image.data(), w, h, w, GRAY_8, &message_handler_,
                            gradient.data()));
  // Interior pixels (1..3, 1..3) should all be 0.
  for (int y = 1; y < h - 1; ++y) {
    for (int x = 1; x < w - 1; ++x) {
      EXPECT_EQ(gradient[y * w + x], 0u)
          << "Uniform image should have zero gradient at (" << x << "," << y
          << ")";
    }
  }
}

// ==========================================================================
// Coverage: AnalyzeImage reader output path without optimizer (line 308)
// ==========================================================================

TEST_F(ImageAnalysisTest, AnalyzeImageReaderWithoutOptimizer) {
  // When has_transparency=null and is_photo=null, the optimizer is not
  // created. If reader is non-null, the sf_reader should be returned
  // directly (line 308: *reader = sf_reader.release()).
  std::string jpeg_data;
  ASSERT_TRUE(ReadTestFile(kJpegTestDir, "sjpeg3", "jpg", &jpeg_data));

  int width = 0, height = 0;
  int quality = -1;
  ScanlineReaderInterface* reader = nullptr;

  // Pass null for has_transparency and is_photo but non-null for reader.
  EXPECT_TRUE(AnalyzeImage(IMAGE_JPEG, jpeg_data.data(), jpeg_data.length(),
                           &width, &height, nullptr, nullptr,
                           /*has_transparency=*/nullptr,
                           /*is_photo=*/nullptr, &quality, &reader,
                           &message_handler_));
  EXPECT_GT(width, 0);
  EXPECT_GT(height, 0);
  // reader should be the raw sf_reader (not optimizer) since we skipped
  // the optimizer path.
  EXPECT_NE(nullptr, reader);
  delete reader;
}

// ==========================================================================
// Coverage: AnalyzeImage with null optional output params
// ==========================================================================

TEST_F(ImageAnalysisTest, AnalyzeImageMinimalOutputParams) {
  // Pass non-null only for width/height, everything else null.
  // This covers the branches where is_animated, is_progressive, quality,
  // has_transparency, is_photo, and reader are all null.
  std::string jpeg_data;
  ASSERT_TRUE(ReadTestFile(kJpegTestDir, "sjpeg3", "jpg", &jpeg_data));

  int width = 0, height = 0;
  EXPECT_TRUE(AnalyzeImage(IMAGE_JPEG, jpeg_data.data(), jpeg_data.length(),
                           &width, &height, nullptr, nullptr, nullptr, nullptr,
                           nullptr, nullptr, &message_handler_));
  EXPECT_GT(width, 0);
  EXPECT_GT(height, 0);
}

// ==========================================================================
// Dimension probes read the header only and are not subject to the decode
// caps; anything that needs pixels still is.
// ==========================================================================

// A header-only PNG declaring width 99999, height 1, with a valid IHDR CRC:
// the same bytes the worker's dimension-inference boundary test feeds in.
// clang-format off
static constexpr uint8_t kBoundaryWidthPng[] = {
    0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A,  // PNG signature
    0x00, 0x00, 0x00, 0x0D, 0x49, 0x48, 0x44, 0x52,  // IHDR chunk
    0x00, 0x01, 0x86, 0x9F, 0x00, 0x00, 0x00, 0x01,  // width=99999, height=1
    0x08, 0x02, 0x00, 0x00, 0x00, 0x4B, 0xE4, 0x58,  // 8bit RGB, CRC
    0x37, 0x00, 0x00, 0x00, 0x0C, 0x49, 0x44, 0x41,  // IDAT chunk
    0x54, 0x08, 0xD7, 0x63, 0xF8, 0xCF, 0xC0, 0x00,  // compressed data
    0x00, 0x00, 0x04, 0x00, 0x01, 0x3B, 0x7E, 0x03,  // checksum
    0x8E, 0x00, 0x00, 0x00, 0x00, 0x49, 0x45, 0x4E,  // IEND chunk
    0x44, 0xAE, 0x42, 0x60, 0x82,                     // IEND CRC
};
// clang-format on

// Appends a 32-bit little-endian value to *out.
void AppendUint32LE(std::string* out, uint32_t value) {
  char buf[4] = {static_cast<char>(value), static_cast<char>(value >> 8),
                 static_cast<char>(value >> 16),
                 static_cast<char>(value >> 24)};
  out->append(buf, sizeof(buf));
}

// Builds a WebP stream whose VP8X chunk declares the given canvas size and
// carries no frame data.
std::string MakeWebpDeclaringCanvas(int width, int height) {
  std::string payload;
  payload.append(1, '\0');  // Flags: no alpha, no animation, no EXIF/XMP.
  payload.append(3, '\0');  // Reserved.
  const uint32_t w = width - 1;
  const uint32_t h = height - 1;
  const char wbuf[3] = {static_cast<char>(w), static_cast<char>(w >> 8),
                        static_cast<char>(w >> 16)};
  const char hbuf[3] = {static_cast<char>(h), static_cast<char>(h >> 8),
                        static_cast<char>(h >> 16)};
  payload.append(wbuf, sizeof(wbuf));
  payload.append(hbuf, sizeof(hbuf));

  std::string body("WEBP");
  body.append("VP8X", 4);
  AppendUint32LE(&body, payload.size());
  body.append(payload);

  std::string webp("RIFF");
  AppendUint32LE(&webp, body.size());
  webp.append(body);
  return webp;
}

TEST_F(ImageAnalysisTest, HeaderOnlyPngDimensionsAreNotCapped) {
  // Only width and height requested: the header is read and nothing is
  // decoded, so the declared width past the per-side decode cap comes back.
  int width = 0, height = 0;
  EXPECT_TRUE(AnalyzeImage(
      IMAGE_PNG, kBoundaryWidthPng, sizeof(kBoundaryWidthPng), &width, &height,
      nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, &message_handler_));
  EXPECT_EQ(99999, width);
  EXPECT_EQ(1, height);

  // A request that needs the pixels goes through the capped reader, which
  // refuses the same bytes.
  bool has_transparency = false;
  EXPECT_FALSE(AnalyzeImage(IMAGE_PNG, kBoundaryWidthPng,
                            sizeof(kBoundaryWidthPng), &width, &height, nullptr,
                            nullptr, &has_transparency, nullptr, nullptr,
                            nullptr, &message_handler_));
  std::unique_ptr<ScanlineReaderInterface> reader(
      CreateScanlineReader(IMAGE_PNG, kBoundaryWidthPng,
                           sizeof(kBoundaryWidthPng), &message_handler_));
  EXPECT_EQ(nullptr, reader);
}

TEST_F(ImageAnalysisTest, HeaderOnlyGifAndWebpDimensionsAreNotCapped) {
  // GIF: the logical screen descriptor alone, 65535x1.
  const char gif[] = {'G',    'I',    'F',  '8',  '9',  'a',  '\xFF',
                      '\xFF', '\x01', 0x00, 0x00, 0x00, 0x00, 0x3B};
  int width = 0, height = 0;
  EXPECT_TRUE(AnalyzeImage(IMAGE_GIF, gif, sizeof(gif), &width, &height,
                           nullptr, nullptr, nullptr, nullptr, nullptr, nullptr,
                           &message_handler_));
  EXPECT_EQ(65535, width);
  EXPECT_EQ(1, height);

  // WebP: the VP8X canvas alone, past both decode caps; the decoder still
  // refuses it.
  std::string webp = MakeWebpDeclaringCanvas(60000, 60000);
  EXPECT_TRUE(AnalyzeImage(IMAGE_WEBP, webp.data(), webp.size(), &width,
                           &height, nullptr, nullptr, nullptr, nullptr, nullptr,
                           nullptr, &message_handler_));
  EXPECT_EQ(60000, width);
  EXPECT_EQ(60000, height);
  std::unique_ptr<ScanlineReaderInterface> reader(CreateScanlineReader(
      IMAGE_WEBP, webp.data(), webp.size(), &message_handler_));
  EXPECT_EQ(nullptr, reader);
}

}  // namespace
