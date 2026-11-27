// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

// Test coverage for lib/image/jpeg_utils.cc
// Tests GetImageQualityFromImage with valid, invalid, and edge-case inputs.

#include "lib/image/jpeg_utils.h"

#include <string>

#include "gtest/gtest.h"
#include "lib/base/message_handler.h"
#include "test/lib/image/test_utils.h"

namespace pagespeed::image_compression {
namespace {

class JpegUtilsTest : public ::testing::Test {
 protected:
  NullMessageHandler handler_;
};

TEST_F(JpegUtilsTest, GrayscaleJpegQuality) {
  std::string data;
  ASSERT_TRUE(ReadTestFile(kJpegTestDir, "testgray", "jpg", &data));
  int quality =
      JpegUtils::GetImageQualityFromImage(data.data(), data.size(), &handler_);
  // Grayscale test image was saved at quality 85.
  EXPECT_EQ(85, quality);
}

TEST_F(JpegUtilsTest, ColorJpegQuality) {
  std::string data;
  ASSERT_TRUE(ReadTestFile(kJpegTestDir, "sjpeg2", "jpg", &data));
  int quality =
      JpegUtils::GetImageQualityFromImage(data.data(), data.size(), &handler_);
  // Color test image was saved at quality 75.
  EXPECT_EQ(75, quality);
}

TEST_F(JpegUtilsTest, Quality100Jpeg) {
  std::string data;
  ASSERT_TRUE(ReadTestFile(kJpegTestDir, "quality100", "jpg", &data));
  int quality =
      JpegUtils::GetImageQualityFromImage(data.data(), data.size(), &handler_);
  EXPECT_EQ(100, quality);
}

TEST_F(JpegUtilsTest, EmptyFileReturnsMinusOne) {
  int quality = JpegUtils::GetImageQualityFromImage("", 0, &handler_);
  EXPECT_EQ(-1, quality);
}

TEST_F(JpegUtilsTest, InvalidDataReturnsMinusOne) {
  const char* garbage = "this is not a jpeg";
  int quality =
      JpegUtils::GetImageQualityFromImage(garbage, strlen(garbage), &handler_);
  EXPECT_EQ(-1, quality);
}

TEST_F(JpegUtilsTest, NotAJpegGifReturnsMinusOne) {
  std::string data;
  ASSERT_TRUE(ReadTestFileWithExt(kJpegTestDir, "notajpeg.gif", &data));
  int quality =
      JpegUtils::GetImageQualityFromImage(data.data(), data.size(), &handler_);
  EXPECT_EQ(-1, quality);
}

TEST_F(JpegUtilsTest, NotAJpegPngReturnsMinusOne) {
  std::string data;
  ASSERT_TRUE(ReadTestFileWithExt(kJpegTestDir, "notajpeg.png", &data));
  int quality =
      JpegUtils::GetImageQualityFromImage(data.data(), data.size(), &handler_);
  EXPECT_EQ(-1, quality);
}

TEST_F(JpegUtilsTest, CorruptJpegStillParsesQuantTables) {
  // corrupt.jpg has a broken image body but valid quantization tables,
  // so libjpeg can still extract a quality estimate.
  std::string data;
  ASSERT_TRUE(ReadTestFile(kJpegTestDir, "corrupt", "jpg", &data));
  int quality =
      JpegUtils::GetImageQualityFromImage(data.data(), data.size(), &handler_);
  EXPECT_GT(quality, 0);
}

TEST_F(JpegUtilsTest, TruncatedJpegHeader) {
  // A valid JPEG starts with FF D8, provide only 2 bytes.
  const unsigned char header[] = {0xFF, 0xD8};
  int quality = JpegUtils::GetImageQualityFromImage(header, 2, &handler_);
  EXPECT_EQ(-1, quality);
}

TEST_F(JpegUtilsTest, QualityInValidRange) {
  // Any valid JPEG should return quality in [1, 100].
  std::string data;
  ASSERT_TRUE(ReadTestFile(kJpegTestDir, "sjpeg1", "jpg", &data));
  int quality =
      JpegUtils::GetImageQualityFromImage(data.data(), data.size(), &handler_);
  EXPECT_GE(quality, 1);
  EXPECT_LE(quality, 100);
}

TEST_F(JpegUtilsTest, ProgressiveJpegQuality) {
  std::string data;
  ASSERT_TRUE(ReadTestFile(kJpegTestDir, "progressive", "jpg", &data));
  int quality =
      JpegUtils::GetImageQualityFromImage(data.data(), data.size(), &handler_);
  // Progressive JPEGs should still return a valid quality.
  EXPECT_GE(quality, 1);
  EXPECT_LE(quality, 100);
}

}  // namespace
}  // namespace pagespeed::image_compression
