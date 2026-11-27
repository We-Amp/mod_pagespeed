// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

// Tests for JPEG reader, utils, and optimizer.
// Ported from mod_pagespeed's test/pagespeed/kernel/image/
// jpeg_optimizer_test.cc, jpeg_reader_test.cc, and
// jpeg_utils_test.cc.

#include "lib/image/jpeg_optimizer.h"

#include <csetjmp>
#include <cstddef>
#include <cstdint>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

#include "gtest/gtest.h"
#include "lib/base/basictypes.h"
#include "lib/base/message_handler.h"
#include "lib/image/exif_orientation.h"
#include "lib/image/jpeg_reader.h"
#include "lib/image/jpeg_utils.h"
#include "lib/image/read_image.h"
#include "lib/image/scanline_interface.h"
#include "lib/image/scanline_utils.h"

// DO NOT INCLUDE LIBJPEG HEADERS HERE. Doing so causes build errors
// on Windows. If you need to call out to libjpeg, please add helper
// methods in a separate file.

namespace {

using pagespeed::NullMessageHandler;
using pagespeed::image_compression::ColorSampling;
using pagespeed::image_compression::GRAY_8;
using pagespeed::image_compression::JpegCompressionOptions;
using pagespeed::image_compression::JpegLossyOptions;
using pagespeed::image_compression::JpegReader;
using pagespeed::image_compression::JpegScanlineReader;
using pagespeed::image_compression::JpegUtils;
using pagespeed::image_compression::kScanlineMaxDimension;
using pagespeed::image_compression::OptimizeJpeg;
using pagespeed::image_compression::OptimizeJpegWithOptions;

// Helper to read a test file into a string.
std::string ReadTestFile(const std::string& filename) {
  // Bazel sets the runfiles directory. test data is accessed via
  // the path relative to the workspace root.
  std::string path = "test/lib/image/testdata/jpeg/" + filename;
  std::ifstream file(path, std::ios::binary);
  if (!file.is_open()) {
    return "";
  }
  return std::string((std::istreambuf_iterator<char>(file)),
                     std::istreambuf_iterator<char>());
}

// C2PA / Content Credentials provenance is carried in JPEG
// APP11 / JUMBF segments. These helpers synthesize a recognizable APP11 segment
// so a test can verify it survives (or is stripped by) optimization without any
// external C2PA tooling. libjpeg saves/rewrites APP11 bytes verbatim, so an
// opaque payload with a sentinel is sufficient.
const char kC2paSentinel[] = "C2PA-PRESERVE-SENTINEL";

// Insert a synthetic APP11/JUMBF segment carrying kC2paSentinel right after the
// SOI marker (FF D8).
std::string InjectApp11C2paSegment(const std::string& jpeg) {
  if (jpeg.size() < 2) {
    return jpeg;
  }
  std::string payload;
  payload.append("JP", 2);        // APP11 common identifier.
  payload.append("\0\0", 2);      // Box-instance number.
  payload.append("jumbc2pa");     // Pseudo JUMBF/C2PA box label.
  payload.append(kC2paSentinel);  // Recognizable opaque blob.
  // APP11 segment length field includes the 2 length bytes themselves.
  const size_t seg_len = payload.size() + 2;
  std::string segment;
  segment.push_back(static_cast<char>(0xFF));
  segment.push_back(static_cast<char>(0xEB));  // APP11 marker.
  segment.push_back(static_cast<char>((seg_len >> 8) & 0xFF));
  segment.push_back(static_cast<char>(seg_len & 0xFF));
  segment += payload;
  return jpeg.substr(0, 2) + segment + jpeg.substr(2);
}

bool ContainsC2paSentinel(const std::string& data) {
  return data.find(kC2paSentinel) != std::string::npos;
}

struct ImageCompressionInfo {
  const char* filename;
  size_t original_size;
  size_t compressed_size;
  size_t lossy_compressed_size;
  size_t progressive_size;
  size_t progressive_and_lossy_compressed_size;
};

ImageCompressionInfo kValidImages[] = {
    // Golden sizes for Jpegli encoder (google/jpegli).
    {"sjpeg1.jpg", 1552, 1481, 962, 1592, 1102},
    {"sjpeg3.jpg", 44084, 41646, 22789, 40772, 22399},
    {"sjpeg6.jpg", 149600, 147151, 88422, 146177, 85514},
    {"testgray.jpg", 5014, 3053, 2871, 3022, 2844},
    {"sjpeg2.jpg", 3612, 3235, 3398, 3333, 3513},
    {"sjpeg4.jpg", 168895, 168248, 42087, 162116, 40740},
    {"test411.jpg", 6883, 4328, 3159, 4436, 3260},
    {"test420.jpg", 6173, 3640, 3369, 3665, 3424},
    {"test422.jpg", 6501, 3970, 3174, 4075, 3268},
};

const char* kInvalidFiles[] = {
    "notajpeg.png",   // A png.
    "notajpeg.gif",   // A gif.
    "emptyfile.jpg",  // A zero-byte file.
    "corrupt.jpg",    // Invalid huffman code in the image data section.
};

const size_t kValidImageCount = arraysize(kValidImages);
const size_t kInvalidFileCount = arraysize(kInvalidFiles);

// ============================================================
// JpegOptimizer tests
// ============================================================

class JpegOptimizerTest : public testing::Test {
 public:
  JpegOptimizerTest() = default;

 protected:
  NullMessageHandler message_handler_;

 private:
  DISALLOW_COPY_AND_ASSIGN(JpegOptimizerTest);
};

TEST_F(JpegOptimizerTest, ValidJpegs) {
  for (auto& kValidImage : kValidImages) {
    std::string src_data = ReadTestFile(kValidImage.filename);
    ASSERT_FALSE(src_data.empty()) << kValidImage.filename;
    std::string dest_data;
    ASSERT_TRUE(OptimizeJpeg(src_data, &dest_data, &message_handler_));
    EXPECT_EQ(kValidImage.original_size, src_data.size())
        << kValidImage.filename;
    EXPECT_EQ(kValidImage.compressed_size, dest_data.size())
        << kValidImage.filename;
    ASSERT_LE(dest_data.size(), src_data.size());
  }
}

TEST_F(JpegOptimizerTest, ValidJpegsLossy) {
  for (auto& kValidImage : kValidImages) {
    std::string src_data = ReadTestFile(kValidImage.filename);
    ASSERT_FALSE(src_data.empty()) << kValidImage.filename;
    JpegCompressionOptions options;
    options.lossy = true;
    std::string dest_data;
    ASSERT_TRUE(OptimizeJpegWithOptions(src_data, &dest_data, options,
                                        &message_handler_))
        << kValidImage.filename;
    EXPECT_EQ(kValidImage.original_size, src_data.size())
        << kValidImage.filename;
    EXPECT_EQ(kValidImage.lossy_compressed_size, dest_data.size())
        << kValidImage.filename;
  }
}

TEST_F(JpegOptimizerTest, ValidJpegsProgressive) {
  for (auto& kValidImage : kValidImages) {
    std::string src_data = ReadTestFile(kValidImage.filename);
    ASSERT_FALSE(src_data.empty()) << kValidImage.filename;
    JpegCompressionOptions options;
    options.progressive = true;
    std::string dest_data;
    ASSERT_TRUE(OptimizeJpegWithOptions(src_data, &dest_data, options,
                                        &message_handler_))
        << kValidImage.filename;
    EXPECT_EQ(kValidImage.original_size, src_data.size())
        << kValidImage.filename;
    EXPECT_EQ(kValidImage.progressive_size, dest_data.size())
        << kValidImage.filename;
  }
}

TEST_F(JpegOptimizerTest, ValidJpegsProgressiveAndLossy) {
  for (auto& kValidImage : kValidImages) {
    std::string src_data = ReadTestFile(kValidImage.filename);
    ASSERT_FALSE(src_data.empty()) << kValidImage.filename;
    JpegCompressionOptions options;
    options.lossy = true;
    options.progressive = true;
    std::string dest_data;
    ASSERT_TRUE(OptimizeJpegWithOptions(src_data, &dest_data, options,
                                        &message_handler_))
        << kValidImage.filename;
    EXPECT_EQ(kValidImage.original_size, src_data.size())
        << kValidImage.filename;
    EXPECT_EQ(kValidImage.progressive_and_lossy_compressed_size,
              dest_data.size())
        << kValidImage.filename;
  }
}

// C2PA/Content-Credentials provenance (APP11/JUMBF) must be
// preserved by default through JPEG optimization, stripped only when
// preserve_c2pa is explicitly false, and independent of EXIF retention.
TEST_F(JpegOptimizerTest, PreserveC2pa) {
  std::string clean = ReadTestFile("sjpeg3.jpg");
  ASSERT_FALSE(clean.empty());
  const std::string src = InjectApp11C2paSegment(clean);
  ASSERT_TRUE(ContainsC2paSentinel(src));

  JpegCompressionOptions options;
  std::string dest;

  // Lossless: preserve on (the default) keeps it; off strips it.
  options.preserve_c2pa = true;
  ASSERT_TRUE(OptimizeJpegWithOptions(src, &dest, options, &message_handler_));
  EXPECT_TRUE(ContainsC2paSentinel(dest));

  options.preserve_c2pa = false;
  dest.clear();
  ASSERT_TRUE(OptimizeJpegWithOptions(src, &dest, options, &message_handler_));
  EXPECT_FALSE(ContainsC2paSentinel(dest));

  // Lossy: same behavior.
  options.lossy = true;
  options.preserve_c2pa = true;
  dest.clear();
  ASSERT_TRUE(OptimizeJpegWithOptions(src, &dest, options, &message_handler_));
  EXPECT_TRUE(ContainsC2paSentinel(dest));

  options.preserve_c2pa = false;
  dest.clear();
  ASSERT_TRUE(OptimizeJpegWithOptions(src, &dest, options, &message_handler_));
  EXPECT_FALSE(ContainsC2paSentinel(dest));

  // Independence from EXIF stripping: stripping EXIF must NOT drop C2PA.
  options.lossy = false;
  options.preserve_c2pa = true;
  options.retain_exif_data = false;
  dest.clear();
  ASSERT_TRUE(OptimizeJpegWithOptions(src, &dest, options, &message_handler_));
  EXPECT_TRUE(ContainsC2paSentinel(dest));
}

TEST_F(JpegOptimizerTest, InvalidJpegs) {
  for (auto& kInvalidFile : kInvalidFiles) {
    std::string src_data = ReadTestFile(kInvalidFile);
    // emptyfile.jpg may be empty; others should have content
    std::string dest_data;
    ASSERT_FALSE(OptimizeJpeg(src_data, &dest_data, &message_handler_));
  }
}

TEST_F(JpegOptimizerTest, InvalidJpegsLossy) {
  for (auto& kInvalidFile : kInvalidFiles) {
    std::string src_data = ReadTestFile(kInvalidFile);
    JpegCompressionOptions options;
    options.lossy = true;
    std::string dest_data;
    ASSERT_FALSE(OptimizeJpegWithOptions(src_data, &dest_data, options,
                                         &message_handler_));
  }
}

TEST_F(JpegOptimizerTest, InvalidJpegsProgressive) {
  for (auto& kInvalidFile : kInvalidFiles) {
    std::string src_data = ReadTestFile(kInvalidFile);
    JpegCompressionOptions options;
    options.progressive = true;
    std::string dest_data;
    ASSERT_FALSE(OptimizeJpegWithOptions(src_data, &dest_data, options,
                                         &message_handler_));
  }
}

TEST_F(JpegOptimizerTest, InvalidJpegsProgressiveAndLossy) {
  for (auto& kInvalidFile : kInvalidFiles) {
    std::string src_data = ReadTestFile(kInvalidFile);
    JpegCompressionOptions options;
    options.lossy = true;
    options.progressive = true;
    std::string dest_data;
    ASSERT_FALSE(OptimizeJpegWithOptions(src_data, &dest_data, options,
                                         &message_handler_));
  }
}

// Test that after reading an invalid jpeg, the reader cleans its
// state so that it can read a correct jpeg again.
TEST_F(JpegOptimizerTest, CleanupAfterReadingInvalidJpeg) {
  // Compress each input image with a reinitialized JpegOptimizer.
  // We will compare these files with the output we get from
  // a JpegOptimizer that had an error.
  std::vector<std::string> correctly_compressed;
  for (auto& kValidImage : kValidImages) {
    std::string src_data = ReadTestFile(kValidImage.filename);
    ASSERT_FALSE(src_data.empty()) << kValidImage.filename;
    correctly_compressed.emplace_back("");
    std::string& dest_data = correctly_compressed.back();
    ASSERT_TRUE(OptimizeJpeg(src_data, &dest_data, &message_handler_));
  }

  // The invalid files are all invalid in different ways, and we
  // want to cover all the ways jpeg decoding can fail. So, we want
  // at least as many valid images as invalid ones.
  ASSERT_GE(kValidImageCount, kInvalidFileCount);

  for (size_t i = 0; i < kInvalidFileCount; ++i) {
    std::string invalid_src_data = ReadTestFile(kInvalidFiles[i]);
    std::string invalid_dest_data;

    std::string valid_src_data = ReadTestFile(kValidImages[i].filename);
    ASSERT_FALSE(valid_src_data.empty()) << kValidImages[i].filename;
    std::string valid_dest_data;

    ASSERT_FALSE(
        OptimizeJpeg(invalid_src_data, &invalid_dest_data, &message_handler_));
    ASSERT_TRUE(
        OptimizeJpeg(valid_src_data, &valid_dest_data, &message_handler_));

    // Diff the jpeg created by CreateOptimizedJpeg() with the one
    // created with a reinitialized JpegOptimizer.
    ASSERT_EQ(valid_dest_data, correctly_compressed.at(i));
  }
}

// ============================================================
// JpegScanlineReader tests
// ============================================================

const char* kValidJpegImages[] = {
    "test411.jpg",   // RGB color space with 4:1:1 chroma.
    "test420.jpg",   // RGB color space with 4:2:0 chroma.
    "test422.jpg",   // RGB color space with 4:2:2 chroma.
    "test444.jpg",   // RGB color space with full chroma.
    "testgray.jpg",  // Grayscale color space.
};

TEST(JpegReaderTest, InvalidJpegs) {
  for (size_t i = 0; i < kInvalidFileCount; ++i) {
    std::string src_data = ReadTestFile(kInvalidFiles[i]);
    NullMessageHandler message_handler;
    JpegScanlineReader reader(&message_handler);
    if (i < kInvalidFileCount - 1) {
      ASSERT_FALSE(reader.Initialize(src_data.c_str(), src_data.length()));
    } else {
      // corrupt.jpg: can initialize but fails during reading.
      // The exact row where corruption is detected varies by
      // decoder (libjpeg-turbo vs jpegli), so just verify that
      // reading eventually fails before all rows are consumed.
      ASSERT_TRUE(reader.Initialize(src_data.c_str(), src_data.length()));
      void* scanline = nullptr;
      bool hit_error = false;
      while (reader.HasMoreScanLines()) {
        if (!reader.ReadNextScanline(&scanline)) {
          hit_error = true;
          break;
        }
      }
      ASSERT_TRUE(hit_error) << "corrupt.jpg should fail during reading";
    }
  }
}

// Verify that the reader works properly no matter how many
// scanlines it reads.
TEST(JpegReaderTest, PartialRead) {
  std::string image1 = ReadTestFile(kValidJpegImages[0]);
  std::string image2 = ReadTestFile(kValidJpegImages[1]);
  ASSERT_FALSE(image1.empty());
  ASSERT_FALSE(image2.empty());
  void* scanline = nullptr;
  NullMessageHandler message_handler;

  JpegScanlineReader reader1(&message_handler);
  ASSERT_TRUE(reader1.Initialize(image1.c_str(), image1.length()));

  JpegScanlineReader reader2(&message_handler);
  ASSERT_TRUE(reader2.Initialize(image1.c_str(), image1.length()));
  ASSERT_TRUE(reader2.ReadNextScanline(&scanline));

  JpegScanlineReader reader3(&message_handler);
  ASSERT_TRUE(reader3.Initialize(image1.c_str(), image1.length()));
  ASSERT_TRUE(reader3.ReadNextScanline(&scanline));
  ASSERT_TRUE(reader3.ReadNextScanline(&scanline));
  ASSERT_TRUE(reader3.Initialize(image2.c_str(), image2.length()));
  ASSERT_TRUE(reader3.ReadNextScanline(&scanline));

  JpegScanlineReader reader4(&message_handler);
  ASSERT_TRUE(reader4.Initialize(image1.c_str(), image1.length()));
  while (reader4.HasMoreScanLines()) {
    ASSERT_TRUE(reader4.ReadNextScanline(&scanline));
  }

  // After depleting the scanlines, any further call to
  // ReadNextScanline should return false.
  EXPECT_FALSE(reader4.ReadNextScanline(&scanline));

  ASSERT_TRUE(reader4.Initialize(image2.c_str(), image2.length()));
  ASSERT_TRUE(reader4.ReadNextScanline(&scanline));
}

// Verify basic properties of valid JPEG scanline reading.
TEST(JpegReaderTest, ValidJpegs) {
  NullMessageHandler message_handler;
  for (auto& kValidJpegImage : kValidJpegImages) {
    std::string jpeg_image = ReadTestFile(kValidJpegImage);
    ASSERT_FALSE(jpeg_image.empty()) << kValidJpegImage;
    JpegScanlineReader reader(&message_handler);
    ASSERT_TRUE(reader.Initialize(jpeg_image.c_str(), jpeg_image.length()))
        << kValidJpegImage;
    EXPECT_GT(reader.GetImageWidth(), 0u);
    EXPECT_GT(reader.GetImageHeight(), 0u);
    EXPECT_GT(reader.GetBytesPerScanline(), 0u);

    // Read all scanlines
    size_t rows_read = 0;
    while (reader.HasMoreScanLines()) {
      void* scanline = nullptr;
      ASSERT_TRUE(reader.ReadNextScanline(&scanline));
      ASSERT_NE(nullptr, scanline);
      ++rows_read;
    }
    EXPECT_EQ(reader.GetImageHeight(), rows_read);
  }
}

// ============================================================
// JpegUtils tests
// ============================================================

TEST(JpegUtilsTest, GetImageQualityFromImage) {
  NullMessageHandler message_handler;

  std::string src_data = ReadTestFile("testgray.jpg");
  ASSERT_FALSE(src_data.empty());
  EXPECT_EQ(85, JpegUtils::GetImageQualityFromImage(
                    src_data.data(), src_data.size(), &message_handler));

  src_data = ReadTestFile("sjpeg2.jpg");
  ASSERT_FALSE(src_data.empty());
  EXPECT_EQ(75, JpegUtils::GetImageQualityFromImage(
                    src_data.data(), src_data.size(), &message_handler));

  src_data = ReadTestFile("emptyfile.jpg");
  // emptyfile.jpg is a zero-byte file
  EXPECT_EQ(-1, JpegUtils::GetImageQualityFromImage(
                    src_data.data(), src_data.size(), &message_handler));

  src_data = ReadTestFile("quality100.jpg");
  ASSERT_FALSE(src_data.empty());
  EXPECT_EQ(100, JpegUtils::GetImageQualityFromImage(
                     src_data.data(), src_data.size(), &message_handler));
}

// ============================================================
// Color sampling tests (L155-172 in jpeg_optimizer.cc)
// ============================================================

TEST_F(JpegOptimizerTest, LossyYUV444ColorSampling) {
  // Use a YCbCr JPEG (not grayscale) so color sampling path is exercised.
  std::string src_data = ReadTestFile("test420.jpg");
  ASSERT_FALSE(src_data.empty());
  JpegCompressionOptions options;
  options.lossy = true;
  options.lossy_options.color_sampling = ColorSampling::YUV444;
  std::string dest_data;
  ASSERT_TRUE(OptimizeJpegWithOptions(src_data, &dest_data, options,
                                      &message_handler_));
  EXPECT_FALSE(dest_data.empty());
  // YUV444 output should differ from default YUV420 lossy output.
  JpegCompressionOptions default_opts;
  default_opts.lossy = true;
  std::string default_data;
  ASSERT_TRUE(OptimizeJpegWithOptions(src_data, &default_data, default_opts,
                                      &message_handler_));
  EXPECT_NE(dest_data, default_data);
}

TEST_F(JpegOptimizerTest, LossyYUV422ColorSampling) {
  std::string src_data = ReadTestFile("test420.jpg");
  ASSERT_FALSE(src_data.empty());
  JpegCompressionOptions options;
  options.lossy = true;
  options.lossy_options.color_sampling = ColorSampling::YUV422;
  std::string dest_data;
  ASSERT_TRUE(OptimizeJpegWithOptions(src_data, &dest_data, options,
                                      &message_handler_));
  EXPECT_FALSE(dest_data.empty());
  // YUV422 output should differ from default YUV420 lossy output.
  JpegCompressionOptions default_opts;
  default_opts.lossy = true;
  std::string default_data;
  ASSERT_TRUE(OptimizeJpegWithOptions(src_data, &default_data, default_opts,
                                      &message_handler_));
  EXPECT_NE(dest_data, default_data);
}

TEST_F(JpegOptimizerTest, LossyRetainColorSampling) {
  // test422.jpg has 4:2:2 chroma subsampling. RETAIN should preserve it.
  std::string src_data = ReadTestFile("test422.jpg");
  ASSERT_FALSE(src_data.empty());
  JpegCompressionOptions options;
  options.lossy = true;
  options.lossy_options.color_sampling = ColorSampling::RETAIN;
  std::string dest_data;
  ASSERT_TRUE(OptimizeJpegWithOptions(src_data, &dest_data, options,
                                      &message_handler_));
  EXPECT_FALSE(dest_data.empty());
  // RETAIN with a 4:2:2 source should differ from default YUV420 lossy.
  JpegCompressionOptions yuv420_opts;
  yuv420_opts.lossy = true;
  yuv420_opts.lossy_options.color_sampling = ColorSampling::YUV420;
  std::string yuv420_data;
  ASSERT_TRUE(OptimizeJpegWithOptions(src_data, &yuv420_data, yuv420_opts,
                                      &message_handler_));
  EXPECT_NE(dest_data, yuv420_data);
}

// ============================================================
// Progressive with num_scans limit (L183-184)
// ============================================================

TEST_F(JpegOptimizerTest, ProgressiveWithNumScansLimit) {
  std::string src_data = ReadTestFile("sjpeg3.jpg");
  ASSERT_FALSE(src_data.empty());
  JpegCompressionOptions options;
  options.progressive = true;
  options.lossy = true;
  options.lossy_options.num_scans = 3;
  std::string dest_data;
  ASSERT_TRUE(OptimizeJpegWithOptions(src_data, &dest_data, options,
                                      &message_handler_));
  EXPECT_FALSE(dest_data.empty());
  // With fewer scans the output should be smaller than full progressive+lossy.
  JpegCompressionOptions full_opts;
  full_opts.progressive = true;
  full_opts.lossy = true;
  std::string full_data;
  ASSERT_TRUE(OptimizeJpegWithOptions(src_data, &full_data, full_opts,
                                      &message_handler_));
  EXPECT_LE(dest_data.size(), full_data.size());
}

// ============================================================
// EXIF data retention (L195-206, L389-391)
// ============================================================

TEST_F(JpegOptimizerTest, RetainExifData) {
  // app_segments.jpg is expected to contain APP1 (EXIF) markers.
  std::string src_data = ReadTestFile("app_segments.jpg");
  ASSERT_FALSE(src_data.empty());

  // Lossy path with retain_exif_data=true exercises L389-391 (jpeg_save_markers
  // for EXIF) and L195-206 (marker copy loop).
  JpegCompressionOptions options;
  options.lossy = true;
  options.retain_exif_data = true;
  std::string dest_with_exif;
  ASSERT_TRUE(OptimizeJpegWithOptions(src_data, &dest_with_exif, options,
                                      &message_handler_));
  EXPECT_FALSE(dest_with_exif.empty());

  // Without retaining EXIF, the output should be smaller (markers stripped).
  JpegCompressionOptions no_exif_opts;
  no_exif_opts.lossy = true;
  no_exif_opts.retain_exif_data = false;
  std::string dest_no_exif;
  ASSERT_TRUE(OptimizeJpegWithOptions(src_data, &dest_no_exif, no_exif_opts,
                                      &message_handler_));
  EXPECT_GT(dest_with_exif.size(), dest_no_exif.size());
}

TEST_F(JpegOptimizerTest, RetainExifDataLossless) {
  // Also exercise the lossless path with retain_exif_data to ensure
  // jpeg_save_markers is set up before jpeg_read_header.
  std::string src_data = ReadTestFile("app_segments.jpg");
  ASSERT_FALSE(src_data.empty());

  JpegCompressionOptions options;
  options.lossy = false;
  options.retain_exif_data = true;
  std::string dest_data;
  ASSERT_TRUE(OptimizeJpegWithOptions(src_data, &dest_data, options,
                                      &message_handler_));
  EXPECT_FALSE(dest_data.empty());
}

// ============================================================
// ICC color profile retention (L385-387)
// ============================================================

TEST_F(JpegOptimizerTest, RetainColorProfile) {
  // app_segments.jpg may contain ICC profile in APP2 marker.
  std::string src_data = ReadTestFile("app_segments.jpg");
  ASSERT_FALSE(src_data.empty());

  JpegCompressionOptions options;
  options.lossy = true;
  options.retain_color_profile = true;
  std::string dest_with_icc;
  ASSERT_TRUE(OptimizeJpegWithOptions(src_data, &dest_with_icc, options,
                                      &message_handler_));
  EXPECT_FALSE(dest_with_icc.empty());

  // Without retaining color profile, the output may be smaller.
  JpegCompressionOptions no_icc_opts;
  no_icc_opts.lossy = true;
  no_icc_opts.retain_color_profile = false;
  std::string dest_no_icc;
  ASSERT_TRUE(OptimizeJpegWithOptions(src_data, &dest_no_icc, no_icc_opts,
                                      &message_handler_));
  // The output with ICC retained should be >= the output without.
  EXPECT_GE(dest_with_icc.size(), dest_no_icc.size());
}

TEST_F(JpegOptimizerTest, RetainBothColorProfileAndExif) {
  std::string src_data = ReadTestFile("app_segments.jpg");
  ASSERT_FALSE(src_data.empty());

  JpegCompressionOptions options;
  options.lossy = true;
  options.retain_color_profile = true;
  options.retain_exif_data = true;
  std::string dest_data;
  ASSERT_TRUE(OptimizeJpegWithOptions(src_data, &dest_data, options,
                                      &message_handler_));
  EXPECT_FALSE(dest_data.empty());
}

// ============================================================
// JpegScanlineWriter tests (L467-552)
// ============================================================

using pagespeed::image_compression::JpegScanlineWriter;
using pagespeed::image_compression::PixelFormat;

TEST_F(JpegOptimizerTest, ScanlineWriterRGBA8888Rejection) {
  // RGBA_8888 is not supported for JPEG (no alpha channel). L482-485.
  JpegScanlineWriter writer(&message_handler_);
  jmp_buf env;
  writer.SetJmpBufEnv(&env);
  if (setjmp(env) == 0) {
    auto status = writer.InitWithStatus(100, 100, PixelFormat::RGBA_8888);
    EXPECT_FALSE(status.Success());
    // Should be UNSUPPORTED_FEATURE from SCANLINE_JPEGWRITER.
    EXPECT_EQ(status.type(), net_instaweb::SCANLINE_STATUS_UNSUPPORTED_FEATURE);
  }
}

TEST_F(JpegOptimizerTest, ScanlineWriterNullParams) {
  // InitializeWriteWithStatus with nullptr params should return error. L516-519.
  JpegScanlineWriter writer(&message_handler_);
  jmp_buf env;
  writer.SetJmpBufEnv(&env);
  if (setjmp(env) == 0) {
    auto init_status = writer.InitWithStatus(100, 100, PixelFormat::RGB_888);
    ASSERT_TRUE(init_status.Success());

    std::string output;
    auto status = writer.InitializeWriteWithStatus(nullptr, &output);
    EXPECT_FALSE(status.Success());
    EXPECT_EQ(status.type(), net_instaweb::SCANLINE_STATUS_INVOCATION_ERROR);
  }
}

TEST_F(JpegOptimizerTest, ScanlineWriterLosslessWarning) {
  // SetJpegCompressParams with lossy=false logs a DFATAL warning. L505-510.
  // With NullMessageHandler, the Fatal call is silently absorbed.
  JpegScanlineWriter writer(&message_handler_);
  jmp_buf env;
  writer.SetJmpBufEnv(&env);
  if (setjmp(env) == 0) {
    auto status = writer.InitWithStatus(100, 100, PixelFormat::RGB_888);
    ASSERT_TRUE(status.Success());

    // Create options with lossy=false -- this triggers the warning path.
    JpegCompressionOptions options;
    options.lossy = false;

    std::string output;
    // InitializeWriteWithStatus calls SetJpegCompressParams internally.
    auto write_status = writer.InitializeWriteWithStatus(&options, &output);
    // Despite the warning, it should still proceed with default encoding.
    EXPECT_TRUE(write_status.Success());
    writer.AbortWrite();
  }
}

TEST_F(JpegOptimizerTest, ScanlineWriterAbortWrite) {
  // Start a write, abort, verify cleanup. L549-552.
  JpegScanlineWriter writer(&message_handler_);
  jmp_buf env;
  writer.SetJmpBufEnv(&env);
  if (setjmp(env) == 0) {
    auto init_status = writer.InitWithStatus(10, 10, PixelFormat::RGB_888);
    ASSERT_TRUE(init_status.Success());

    JpegCompressionOptions options;
    options.lossy = true;
    std::string output;
    auto write_status = writer.InitializeWriteWithStatus(&options, &output);
    ASSERT_TRUE(write_status.Success());

    // Write one scanline (10 pixels * 3 bytes = 30 bytes).
    std::vector<uint8_t> row(30, 128);
    auto scanline_status = writer.WriteNextScanlineWithStatus(row.data());
    EXPECT_TRUE(scanline_status.Success());

    // Abort without writing all scanlines or finalizing.
    writer.AbortWrite();
    // After abort, the writer should be in a clean state. We verify it
    // doesn't crash. The output string may contain partial data.
  }
}

TEST_F(JpegOptimizerTest, ScanlineWriterGrayscale) {
  // Verify the GRAY_8 pixel format path (L479-480) works end-to-end.
  JpegScanlineWriter writer(&message_handler_);
  jmp_buf env;
  writer.SetJmpBufEnv(&env);
  if (setjmp(env) == 0) {
    const size_t width = 8;
    const size_t height = 8;
    auto init_status =
        writer.InitWithStatus(width, height, PixelFormat::GRAY_8);
    ASSERT_TRUE(init_status.Success());

    JpegCompressionOptions options;
    options.lossy = true;
    options.lossy_options.quality = 75;
    std::string output;
    auto write_status = writer.InitializeWriteWithStatus(&options, &output);
    ASSERT_TRUE(write_status.Success());

    // Write all scanlines (8 pixels * 1 byte per pixel).
    std::vector<uint8_t> row(width, 200);
    for (size_t i = 0; i < height; ++i) {
      auto scanline_status = writer.WriteNextScanlineWithStatus(row.data());
      ASSERT_TRUE(scanline_status.Success());
    }

    auto finalize_status = writer.FinalizeWriteWithStatus();
    EXPECT_TRUE(finalize_status.Success());
    EXPECT_FALSE(output.empty());
  }
}

// ============================================================
// ErrorExit path (L102-106): corrupt/truncated JPEG data
// ============================================================

TEST_F(JpegOptimizerTest, TruncatedJpegTriggersErrorExit) {
  // Read a valid JPEG and truncate it to trigger ErrorExit during
  // decompression via longjmp.
  std::string src_data = ReadTestFile("sjpeg3.jpg");
  ASSERT_FALSE(src_data.empty());
  // Keep only the first 100 bytes (valid header, but truncated image data).
  std::string truncated = src_data.substr(0, 100);

  std::string dest_data;
  EXPECT_FALSE(OptimizeJpeg(truncated, &dest_data, &message_handler_));
}

TEST_F(JpegOptimizerTest, TruncatedJpegLossyTriggersErrorExit) {
  std::string src_data = ReadTestFile("sjpeg3.jpg");
  ASSERT_FALSE(src_data.empty());
  std::string truncated = src_data.substr(0, 100);

  JpegCompressionOptions options;
  options.lossy = true;
  std::string dest_data;
  EXPECT_FALSE(OptimizeJpegWithOptions(truncated, &dest_data, options,
                                       &message_handler_));
}

TEST_F(JpegOptimizerTest, CorruptJpegDataTriggersErrorExit) {
  // Use corrupt.jpg which has invalid huffman codes in the data section.
  std::string src_data = ReadTestFile("corrupt.jpg");
  ASSERT_FALSE(src_data.empty());
  std::string dest_data;
  EXPECT_FALSE(OptimizeJpeg(src_data, &dest_data, &message_handler_));
}

TEST_F(JpegOptimizerTest, CorruptJpegLossyTriggersErrorExit) {
  std::string src_data = ReadTestFile("corrupt.jpg");
  ASSERT_FALSE(src_data.empty());
  JpegCompressionOptions options;
  options.lossy = true;
  std::string dest_data;
  EXPECT_FALSE(OptimizeJpegWithOptions(src_data, &dest_data, options,
                                       &message_handler_));
}

// ==========================================================================
// Error path coverage: JPEG with various types of corruption
// ==========================================================================

TEST_F(JpegOptimizerTest, EmptyInputLossless) {
  std::string empty;
  std::string dest_data;
  EXPECT_FALSE(OptimizeJpeg(empty, &dest_data, &message_handler_));
}

TEST_F(JpegOptimizerTest, EmptyInputLossy) {
  std::string empty;
  JpegCompressionOptions options;
  options.lossy = true;
  std::string dest_data;
  EXPECT_FALSE(
      OptimizeJpegWithOptions(empty, &dest_data, options, &message_handler_));
}

TEST_F(JpegOptimizerTest, SingleByteInput) {
  std::string single(1, '\xFF');
  std::string dest_data;
  EXPECT_FALSE(OptimizeJpeg(single, &dest_data, &message_handler_));
}

TEST_F(JpegOptimizerTest, ValidHeaderThenGarbage) {
  // Start with the JPEG SOI marker followed by garbage.
  std::string data;
  data += '\xFF';
  data += '\xD8';  // SOI
  data += std::string(100, 'X');
  std::string dest_data;
  EXPECT_FALSE(OptimizeJpeg(data, &dest_data, &message_handler_));
}

TEST_F(JpegOptimizerTest, ValidHeaderThenGarbageLossy) {
  std::string data;
  data += '\xFF';
  data += '\xD8';  // SOI
  data += std::string(100, 'X');
  JpegCompressionOptions options;
  options.lossy = true;
  std::string dest_data;
  EXPECT_FALSE(
      OptimizeJpegWithOptions(data, &dest_data, options, &message_handler_));
}

TEST_F(JpegOptimizerTest, AllZerosInput) {
  std::string zeros(200, '\0');
  std::string dest_data;
  EXPECT_FALSE(OptimizeJpeg(zeros, &dest_data, &message_handler_));
}

TEST_F(JpegOptimizerTest, TruncatedJpegMidScanlines) {
  // Read a valid JPEG, truncate it at about 60% -- deep enough to get
  // past the header but mid-scanline data.
  std::string src_data = ReadTestFile("sjpeg3.jpg");
  ASSERT_FALSE(src_data.empty());
  std::string truncated = src_data.substr(0, src_data.size() * 6 / 10);
  std::string dest_data;
  EXPECT_FALSE(OptimizeJpeg(truncated, &dest_data, &message_handler_));
}

TEST_F(JpegOptimizerTest, TruncatedJpegMidScanlineLoossy) {
  std::string src_data = ReadTestFile("sjpeg3.jpg");
  ASSERT_FALSE(src_data.empty());
  std::string truncated = src_data.substr(0, src_data.size() * 6 / 10);
  JpegCompressionOptions options;
  options.lossy = true;
  std::string dest_data;
  EXPECT_FALSE(OptimizeJpegWithOptions(truncated, &dest_data, options,
                                       &message_handler_));
}

// Progressive+lossy on corrupt data.
TEST_F(JpegOptimizerTest, CorruptJpegProgressiveLossy) {
  std::string src_data = ReadTestFile("corrupt.jpg");
  ASSERT_FALSE(src_data.empty());
  JpegCompressionOptions options;
  options.lossy = true;
  options.progressive = true;
  std::string dest_data;
  EXPECT_FALSE(OptimizeJpegWithOptions(src_data, &dest_data, options,
                                       &message_handler_));
}

// Progressive lossless on truncated data.
TEST_F(JpegOptimizerTest, TruncatedJpegProgressiveLossless) {
  std::string src_data = ReadTestFile("sjpeg3.jpg");
  ASSERT_FALSE(src_data.empty());
  std::string truncated = src_data.substr(0, 100);
  JpegCompressionOptions options;
  options.progressive = true;
  std::string dest_data;
  EXPECT_FALSE(OptimizeJpegWithOptions(truncated, &dest_data, options,
                                       &message_handler_));
}

// Retain exif data with corrupt input.
TEST_F(JpegOptimizerTest, RetainExifWithCorruptInput) {
  std::string src_data = ReadTestFile("corrupt.jpg");
  ASSERT_FALSE(src_data.empty());
  JpegCompressionOptions options;
  options.retain_exif_data = true;
  std::string dest_data;
  EXPECT_FALSE(OptimizeJpegWithOptions(src_data, &dest_data, options,
                                       &message_handler_));
}

// Retain color profile with truncated input.
TEST_F(JpegOptimizerTest, RetainColorProfileWithTruncatedInput) {
  std::string src_data = ReadTestFile("sjpeg3.jpg");
  ASSERT_FALSE(src_data.empty());
  std::string truncated = src_data.substr(0, 100);
  JpegCompressionOptions options;
  options.retain_color_profile = true;
  std::string dest_data;
  EXPECT_FALSE(OptimizeJpegWithOptions(truncated, &dest_data, options,
                                       &message_handler_));
}

// ============================================================
// Phase 6: Additional coverage for remaining gaps
// ============================================================

// --- JpegScanlineWriter: UNSUPPORTED pixel format triggers default case
// (L487-491) ---

TEST_F(JpegOptimizerTest, ScanlineWriterUnsupportedPixelFormat) {
  JpegScanlineWriter writer(&message_handler_);
  jmp_buf env;
  writer.SetJmpBufEnv(&env);
  if (setjmp(env) == 0) {
    // UNSUPPORTED is not RGB_888, GRAY_8, or RGBA_8888 -- hits the default
    // case.
    auto status = writer.InitWithStatus(100, 100, PixelFormat::UNSUPPORTED);
    EXPECT_FALSE(status.Success());
    EXPECT_EQ(status.type(), net_instaweb::SCANLINE_STATUS_INTERNAL_ERROR);
  }
}

// --- JpegScanlineWriter: full end-to-end RGB write cycle ---

TEST_F(JpegOptimizerTest, ScanlineWriterEndToEndRGB) {
  JpegScanlineWriter writer(&message_handler_);
  jmp_buf env;
  writer.SetJmpBufEnv(&env);
  if (setjmp(env) == 0) {
    const size_t width = 16;
    const size_t height = 16;
    auto init_status =
        writer.InitWithStatus(width, height, PixelFormat::RGB_888);
    ASSERT_TRUE(init_status.Success());

    JpegCompressionOptions options;
    options.lossy = true;
    options.lossy_options.quality = 90;
    std::string output;
    auto write_status = writer.InitializeWriteWithStatus(&options, &output);
    ASSERT_TRUE(write_status.Success());

    // Write all scanlines with gradient data.
    std::vector<uint8_t> row(width * 3);
    for (size_t y = 0; y < height; ++y) {
      for (size_t x = 0; x < width; ++x) {
        row[x * 3 + 0] = static_cast<uint8_t>((x * 255) / width);
        row[x * 3 + 1] = static_cast<uint8_t>((y * 255) / height);
        row[x * 3 + 2] = 128;
      }
      auto scanline_status = writer.WriteNextScanlineWithStatus(row.data());
      ASSERT_TRUE(scanline_status.Success());
    }

    auto finalize_status = writer.FinalizeWriteWithStatus();
    EXPECT_TRUE(finalize_status.Success());
    EXPECT_FALSE(output.empty());
    // Valid JPEG starts with 0xFF 0xD8.
    ASSERT_GE(output.size(), 2u);
    EXPECT_EQ(static_cast<uint8_t>(0xFF), static_cast<uint8_t>(output[0]));
    EXPECT_EQ(static_cast<uint8_t>(0xD8), static_cast<uint8_t>(output[1]));
  }
}

// --- Already-optimized JPEG ---

TEST_F(JpegOptimizerTest, AlreadyOptimizedJpeg) {
  std::string src_data = ReadTestFile("already_optimized.jpg");
  if (src_data.empty()) {
    // File may not exist in all test data sets.
    GTEST_SKIP() << "already_optimized.jpg not found";
  }
  std::string dest_data;
  ASSERT_TRUE(OptimizeJpeg(src_data, &dest_data, &message_handler_));
  EXPECT_FALSE(dest_data.empty());
  // Already optimized: output should be same or similar size.
  EXPECT_LE(dest_data.size(), src_data.size());
}

// --- Progressive JPEG input ---

TEST_F(JpegOptimizerTest, ProgressiveJpegInput) {
  std::string src_data = ReadTestFile("progressive.jpg");
  if (src_data.empty()) {
    GTEST_SKIP() << "progressive.jpg not found";
  }
  std::string dest_data;
  ASSERT_TRUE(OptimizeJpeg(src_data, &dest_data, &message_handler_));
  EXPECT_FALSE(dest_data.empty());
}

TEST_F(JpegOptimizerTest, ProgressiveJpegInputLossy) {
  std::string src_data = ReadTestFile("progressive.jpg");
  if (src_data.empty()) {
    GTEST_SKIP() << "progressive.jpg not found";
  }
  JpegCompressionOptions options;
  options.lossy = true;
  std::string dest_data;
  ASSERT_TRUE(OptimizeJpegWithOptions(src_data, &dest_data, options,
                                      &message_handler_));
  EXPECT_FALSE(dest_data.empty());
}

// --- Quality 100 JPEG ---

TEST_F(JpegOptimizerTest, Quality100JpegLossy) {
  std::string src_data = ReadTestFile("quality100.jpg");
  ASSERT_FALSE(src_data.empty());
  JpegCompressionOptions options;
  options.lossy = true;
  options.lossy_options.quality = 100;
  std::string dest_data;
  ASSERT_TRUE(OptimizeJpegWithOptions(src_data, &dest_data, options,
                                      &message_handler_));
  EXPECT_FALSE(dest_data.empty());
}

// --- Quality 1 (very low) JPEG ---

TEST_F(JpegOptimizerTest, VeryLowQualityLossy) {
  std::string src_data = ReadTestFile("sjpeg3.jpg");
  ASSERT_FALSE(src_data.empty());
  JpegCompressionOptions options;
  options.lossy = true;
  options.lossy_options.quality = 1;
  std::string dest_data;
  ASSERT_TRUE(OptimizeJpegWithOptions(src_data, &dest_data, options,
                                      &message_handler_));
  EXPECT_FALSE(dest_data.empty());
  // Very low quality should produce smaller output.
  EXPECT_LT(dest_data.size(), src_data.size());
}

// --- Grayscale JPEG lossy with color sampling (should be ignored) ---

TEST_F(JpegOptimizerTest, GrayscaleJpegLossyYUV444) {
  // testgray.jpg is grayscale; color sampling path should be skipped
  // (jpeg_color_space != JCS_YCbCr).
  std::string src_data = ReadTestFile("testgray.jpg");
  ASSERT_FALSE(src_data.empty());
  JpegCompressionOptions options;
  options.lossy = true;
  options.lossy_options.color_sampling = ColorSampling::YUV444;
  std::string dest_data;
  ASSERT_TRUE(OptimizeJpegWithOptions(src_data, &dest_data, options,
                                      &message_handler_));
  EXPECT_FALSE(dest_data.empty());
}

// --- Lossless with retain both EXIF and ICC ---

TEST_F(JpegOptimizerTest, LosslessRetainBothMarkers) {
  std::string src_data = ReadTestFile("app_segments.jpg");
  ASSERT_FALSE(src_data.empty());
  JpegCompressionOptions options;
  options.lossy = false;
  options.retain_color_profile = true;
  options.retain_exif_data = true;
  std::string dest_data;
  ASSERT_TRUE(OptimizeJpegWithOptions(src_data, &dest_data, options,
                                      &message_handler_));
  EXPECT_FALSE(dest_data.empty());
}

// --- YUV420 explicit color sampling ---

TEST_F(JpegOptimizerTest, LossyYUV420ColorSampling) {
  std::string src_data = ReadTestFile("test444.jpg");
  ASSERT_FALSE(src_data.empty());
  JpegCompressionOptions options;
  options.lossy = true;
  options.lossy_options.color_sampling = ColorSampling::YUV420;
  std::string dest_data;
  ASSERT_TRUE(OptimizeJpegWithOptions(src_data, &dest_data, options,
                                      &message_handler_));
  EXPECT_FALSE(dest_data.empty());
}

// --- Bit-flipped JPEG to exercise ErrorExit longjmp more thoroughly ---

TEST_F(JpegOptimizerTest, BitFlippedJpegLossless) {
  std::string src_data = ReadTestFile("sjpeg3.jpg");
  ASSERT_FALSE(src_data.empty());
  // Flip bits in the middle of the image data.
  size_t flip_pos = src_data.size() / 2;
  src_data[flip_pos] ^= 0xFF;
  src_data[flip_pos + 1] ^= 0xFF;
  src_data[flip_pos + 2] ^= 0xFF;
  std::string dest_data;
  // May succeed or fail depending on where the corruption lands.
  OptimizeJpeg(src_data, &dest_data, &message_handler_);
  // Just verify no crash.
  SUCCEED();
}

TEST_F(JpegOptimizerTest, BitFlippedJpegLossy) {
  std::string src_data = ReadTestFile("sjpeg3.jpg");
  ASSERT_FALSE(src_data.empty());
  size_t flip_pos = src_data.size() / 2;
  src_data[flip_pos] ^= 0xFF;
  src_data[flip_pos + 1] ^= 0xFF;
  JpegCompressionOptions options;
  options.lossy = true;
  std::string dest_data;
  OptimizeJpegWithOptions(src_data, &dest_data, options, &message_handler_);
  // Just verify no crash.
  SUCCEED();
}

// --- JpegScanlineReader: re-initialize after error ---

TEST(JpegReaderTest, ReinitializeAfterCorruption) {
  NullMessageHandler message_handler;
  std::string corrupt_data = ReadTestFile("corrupt.jpg");
  ASSERT_FALSE(corrupt_data.empty());
  std::string valid_data = ReadTestFile("test420.jpg");
  ASSERT_FALSE(valid_data.empty());

  JpegScanlineReader reader(&message_handler);
  // Initialize with corrupt data.
  if (reader.Initialize(corrupt_data.c_str(), corrupt_data.length())) {
    // Try to read until error.
    void* scanline = nullptr;
    while (reader.HasMoreScanLines()) {
      if (!reader.ReadNextScanline(&scanline)) break;
    }
  }

  // Reinitialize with valid data -- should work.
  ASSERT_TRUE(reader.Initialize(valid_data.c_str(), valid_data.length()));
  EXPECT_GT(reader.GetImageWidth(), 0u);
  EXPECT_GT(reader.GetImageHeight(), 0u);

  void* scanline = nullptr;
  EXPECT_TRUE(reader.ReadNextScanline(&scanline));
  EXPECT_NE(nullptr, scanline);
}

// --- JpegScanlineReader: read grayscale scanlines ---

TEST(JpegReaderTest, GrayscaleScanlineRead) {
  NullMessageHandler message_handler;
  std::string src_data = ReadTestFile("testgray.jpg");
  ASSERT_FALSE(src_data.empty());

  JpegScanlineReader reader(&message_handler);
  ASSERT_TRUE(reader.Initialize(src_data.c_str(), src_data.length()));
  EXPECT_GT(reader.GetImageWidth(), 0u);
  EXPECT_GT(reader.GetImageHeight(), 0u);
  // Grayscale: 1 byte per pixel.
  EXPECT_EQ(reader.GetBytesPerScanline(), reader.GetImageWidth());

  size_t rows = 0;
  while (reader.HasMoreScanLines()) {
    void* scanline = nullptr;
    ASSERT_TRUE(reader.ReadNextScanline(&scanline));
    ASSERT_NE(nullptr, scanline);
    ++rows;
  }
  EXPECT_EQ(reader.GetImageHeight(), rows);
}

// --- JpegScanlineReader: progressive detection ---

TEST(JpegReaderTest, ProgressiveDetection) {
  NullMessageHandler message_handler;
  std::string src_data = ReadTestFile("progressive.jpg");
  if (src_data.empty()) {
    GTEST_SKIP() << "progressive.jpg not found";
  }
  JpegScanlineReader reader(&message_handler);
  ASSERT_TRUE(reader.Initialize(src_data.c_str(), src_data.length()));
  EXPECT_TRUE(reader.IsProgressive());
}

TEST(JpegReaderTest, NonProgressiveDetection) {
  NullMessageHandler message_handler;
  std::string src_data = ReadTestFile("test420.jpg");
  ASSERT_FALSE(src_data.empty());
  JpegScanlineReader reader(&message_handler);
  ASSERT_TRUE(reader.Initialize(src_data.c_str(), src_data.length()));
  EXPECT_FALSE(reader.IsProgressive());
}

// ============================================================
// EXIF orientation baking through the JPEG->JPEG recompress (issue #1005)
// ============================================================
//
// Fixtures: exif_orientation_{1..8}.jpg share one upright 32x16 quadrant
// scene (TL=red, TR=green, BL=blue, BR=yellow), stored pre-transformed with
// the matching Orientation tag. See exif_orientation_test.cc.

// Decodes a JPEG through the scanline decode boundary (which bakes any EXIF
// orientation) into an interleaved RGB buffer.
bool DecodeThroughBoundary(const std::string& jpeg, std::string* pixels,
                           size_t* width, size_t* height,
                           pagespeed::MessageHandler* handler) {
  std::unique_ptr<net_instaweb::ScanlineReaderInterface> reader(
      pagespeed::image_compression::CreateScanlineReader(
          pagespeed::image_compression::IMAGE_JPEG, jpeg.data(), jpeg.size(),
          handler));
  if (reader == nullptr) {
    return false;
  }
  *width = reader->GetImageWidth();
  *height = reader->GetImageHeight();
  pixels->clear();
  while (reader->HasMoreScanLines()) {
    void* scanline = nullptr;
    if (!reader->ReadNextScanline(&scanline)) {
      return false;
    }
    pixels->append(static_cast<const char*>(scanline),
                   reader->GetBytesPerScanline());
  }
  return true;
}

void ExpectUprightQuadrantScene(const std::string& jpeg,
                                const std::string& what,
                                pagespeed::MessageHandler* handler) {
  std::string pixels;
  size_t width = 0;
  size_t height = 0;
  ASSERT_TRUE(DecodeThroughBoundary(jpeg, &pixels, &width, &height, handler))
      << what;
  ASSERT_EQ(width, 32u) << what;
  ASSERT_EQ(height, 16u) << what;
  struct Quadrant {
    size_t x, y;
    int r, g, b;
    const char* name;
  };
  const Quadrant quadrants[] = {
      {8, 4, 255, 0, 0, "TL"},
      {24, 4, 0, 255, 0, "TR"},
      {8, 12, 0, 0, 255, "BL"},
      {24, 12, 255, 255, 0, "BR"},
  };
  for (const Quadrant& q : quadrants) {
    const size_t offset = (q.y * width + q.x) * 3;
    ASSERT_LE(offset + 3, pixels.size()) << what;
    EXPECT_NEAR(static_cast<uint8_t>(pixels[offset]), q.r, 48)
        << what << " " << q.name;
    EXPECT_NEAR(static_cast<uint8_t>(pixels[offset + 1]), q.g, 48)
        << what << " " << q.name;
    EXPECT_NEAR(static_cast<uint8_t>(pixels[offset + 2]), q.b, 48)
        << what << " " << q.name;
  }
}

TEST_F(JpegOptimizerTest, LossyBakesExifOrientation) {
  // The lossy recompress must rotate/mirror the pixels upright and emit no
  // Orientation tag, for every orientation value.
  for (int orientation = 1; orientation <= 8; ++orientation) {
    std::string src_data = ReadTestFile("exif_orientation_" +
                                        std::to_string(orientation) + ".jpg");
    ASSERT_FALSE(src_data.empty()) << "fixture " << orientation;

    JpegCompressionOptions options;
    options.lossy = true;
    std::string dest_data;
    ASSERT_TRUE(OptimizeJpegWithOptions(src_data, &dest_data, options,
                                        &message_handler_))
        << "fixture " << orientation;
    // The output must not carry any Orientation tag.
    EXPECT_EQ(pagespeed::image_compression::ReadJpegExifOrientation(
                  dest_data.data(), dest_data.size()),
              1)
        << "fixture " << orientation;
    ExpectUprightQuadrantScene(
        dest_data, "lossy orientation " + std::to_string(orientation),
        &message_handler_);
  }
}

TEST_F(JpegOptimizerTest, LosslessKeepsAccurateOrientationTag) {
  // The lossless coefficient copy cannot rotate pixels; it must keep the
  // image rendering upright by emitting an accurate minimal Orientation tag
  // even though EXIF is otherwise stripped.
  std::string src_data = ReadTestFile("exif_orientation_6.jpg");
  ASSERT_FALSE(src_data.empty());

  JpegCompressionOptions options;
  options.lossy = false;
  std::string dest_data;
  ASSERT_TRUE(OptimizeJpegWithOptions(src_data, &dest_data, options,
                                      &message_handler_));
  EXPECT_EQ(pagespeed::image_compression::ReadJpegExifOrientation(
                dest_data.data(), dest_data.size()),
            6);
  // Decoding through the boundary re-applies the tag: still upright.
  ExpectUprightQuadrantScene(dest_data, "lossless orientation 6",
                             &message_handler_);
}

TEST_F(JpegOptimizerTest, LossyOversizedRasterFallsBackToMinimalTag) {
  // When the raster exceeds the bake ceiling, the lossy recompress cannot
  // buffer it for rotation: it must leave the pixels in their stored layout
  // and emit an accurate minimal Orientation tag so the image still renders
  // upright.
  std::string src_data = ReadTestFile("exif_orientation_6.jpg");
  ASSERT_FALSE(src_data.empty());

  // RAII override so the default is restored even if an assertion fails.
  struct BakeLimitOverride {
    explicit BakeLimitOverride(size_t limit) {
      pagespeed::image_compression::SetOrientationBakeByteLimitForTesting(
          limit);
    }
    ~BakeLimitOverride() {
      pagespeed::image_compression::SetOrientationBakeByteLimitForTesting(0);
    }
  } limit(64);

  JpegCompressionOptions options;
  options.lossy = true;
  std::string dest_data;
  ASSERT_TRUE(OptimizeJpegWithOptions(src_data, &dest_data, options,
                                      &message_handler_));
  // Accurate tag kept, pixels untransformed (raw reader sees the stored
  // portrait layout).
  EXPECT_EQ(pagespeed::image_compression::ReadJpegExifOrientation(
                dest_data.data(), dest_data.size()),
            6);
  JpegScanlineReader raw_reader(&message_handler_);
  ASSERT_TRUE(raw_reader.Initialize(dest_data.data(), dest_data.size()));
  EXPECT_EQ(raw_reader.GetImageWidth(), 16u);
  EXPECT_EQ(raw_reader.GetImageHeight(), 32u);
}

TEST_F(JpegOptimizerTest, RetainExifDataKeepsTagAndStoredPixels) {
  // With retain_exif_data the original EXIF (including its Orientation tag)
  // survives verbatim and the pixels stay untransformed -- a consistent
  // output that still renders upright via the tag.
  std::string src_data = ReadTestFile("exif_orientation_6.jpg");
  ASSERT_FALSE(src_data.empty());

  JpegCompressionOptions options;
  options.lossy = true;
  options.retain_exif_data = true;
  std::string dest_data;
  ASSERT_TRUE(OptimizeJpegWithOptions(src_data, &dest_data, options,
                                      &message_handler_));
  EXPECT_EQ(pagespeed::image_compression::ReadJpegExifOrientation(
                dest_data.data(), dest_data.size()),
            6);
  // The stored raster is untransformed: the raw (unwrapped) reader sees the
  // original portrait layout.
  JpegScanlineReader raw_reader(&message_handler_);
  ASSERT_TRUE(raw_reader.Initialize(dest_data.data(), dest_data.size()));
  EXPECT_EQ(raw_reader.GetImageWidth(), 16u);
  EXPECT_EQ(raw_reader.GetImageHeight(), 32u);
}

// The lossless path buffers the DCT coefficients of the whole image, an
// allocation proportional to the declared dimensions, so a complete image
// one pixel wider than the per-side cap must be refused before the
// coefficients are read.
TEST_F(JpegOptimizerTest, OversizedDimensionsRejectedByLosslessPath) {
  // Encode a valid grayscale strip wider than the decode cap; the bytes
  // are a real JPEG, only the dimensions are unusual.
  const size_t width = static_cast<size_t>(kScanlineMaxDimension) + 1;
  std::string src_data;
  {
    jmp_buf env;
    JpegScanlineWriter writer(&message_handler_);
    if (setjmp(env)) {
      writer.AbortWrite();
      FAIL() << "writing the test image failed";
    }
    writer.SetJmpBufEnv(&env);
    ASSERT_TRUE(writer.InitWithStatus(width, 1, GRAY_8).Success());
    JpegCompressionOptions options;
    options.lossy = true;
    ASSERT_TRUE(
        writer.InitializeWriteWithStatus(&options, &src_data).Success());
    const std::vector<unsigned char> row(width, 0);
    ASSERT_TRUE(writer.WriteNextScanlineWithStatus(row.data()).Success());
    ASSERT_TRUE(writer.FinalizeWriteWithStatus().Success());
  }
  ASSERT_LT(static_cast<size_t>(0), src_data.size());

  std::string dest_data;
  EXPECT_FALSE(OptimizeJpeg(src_data, &dest_data, &message_handler_));
  EXPECT_TRUE(dest_data.empty());
}

}  // namespace
