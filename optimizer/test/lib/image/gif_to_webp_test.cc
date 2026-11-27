// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

#include <cstddef>
#include <string>

#include "gtest/gtest.h"
#include "lib/base/message_handler.h"
#include "lib/image/image_converter.h"
#include "lib/image/image_util.h"
#include "lib/image/webp_optimizer.h"
#include "test/lib/image/test_utils.h"

namespace {

using pagespeed::NullMessageHandler;
using pagespeed::image_compression::ImageConverter;
using pagespeed::image_compression::kGifTestDir;
using pagespeed::image_compression::ReadTestFileWithExt;
using pagespeed::image_compression::WebpConfiguration;

class GifToWebpTest : public ::testing::Test {
 protected:
  NullMessageHandler handler_;
};

// Test converting a static (single-frame) GIF to WebP.
TEST_F(GifToWebpTest, StaticGifToWebp) {
  std::string gif_data;
  ASSERT_TRUE(ReadTestFileWithExt(kGifTestDir, "interlaced.gif", &gif_data));
  ASSERT_GT(gif_data.size(), 0u);

  std::string webp_data;
  WebpConfiguration config;
  EXPECT_TRUE(ImageConverter::ConvertGifToWebp(gif_data, config, &webp_data,
                                               &handler_));
  EXPECT_GT(webp_data.size(), 0u);
  // WebP files start with "RIFF"
  ASSERT_GE(webp_data.size(), 4u);
  EXPECT_EQ('R', webp_data[0]);
  EXPECT_EQ('I', webp_data[1]);
  EXPECT_EQ('F', webp_data[2]);
  EXPECT_EQ('F', webp_data[3]);
}

// Test converting an animated GIF to WebP.
TEST_F(GifToWebpTest, AnimatedGifToWebp) {
  std::string gif_data;
  ASSERT_TRUE(ReadTestFileWithExt(kGifTestDir, "animated.gif", &gif_data));
  ASSERT_GT(gif_data.size(), 0u);

  std::string webp_data;
  WebpConfiguration config;
  EXPECT_TRUE(ImageConverter::ConvertGifToWebp(gif_data, config, &webp_data,
                                               &handler_));
  EXPECT_GT(webp_data.size(), 0u);
  // WebP files start with "RIFF"
  ASSERT_GE(webp_data.size(), 4u);
  EXPECT_EQ('R', webp_data[0]);
  EXPECT_EQ('I', webp_data[1]);
  EXPECT_EQ('F', webp_data[2]);
  EXPECT_EQ('F', webp_data[3]);
}

// Test converting a looping animated GIF.
TEST_F(GifToWebpTest, LoopingAnimatedGifToWebp) {
  std::string gif_data;
  ASSERT_TRUE(ReadTestFileWithExt(kGifTestDir, "full2loop.gif", &gif_data));
  ASSERT_GT(gif_data.size(), 0u);

  std::string webp_data;
  WebpConfiguration config;
  EXPECT_TRUE(ImageConverter::ConvertGifToWebp(gif_data, config, &webp_data,
                                               &handler_));
  EXPECT_GT(webp_data.size(), 0u);
}

// Test that invalid GIF data returns failure.
TEST_F(GifToWebpTest, InvalidGifData) {
  std::string bad_data = "not a gif";
  std::string webp_data;
  WebpConfiguration config;
  EXPECT_FALSE(ImageConverter::ConvertGifToWebp(bad_data, config, &webp_data,
                                                &handler_));
}

// Test that empty GIF data returns failure.
TEST_F(GifToWebpTest, EmptyGifData) {
  std::string empty_data;
  std::string webp_data;
  WebpConfiguration config;
  EXPECT_FALSE(ImageConverter::ConvertGifToWebp(empty_data, config, &webp_data,
                                                &handler_));
}

}  // namespace
