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

#include "pagespeed/kernel/image/image_distortion.h"

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <vector>

#include "pagespeed/kernel/base/null_mutex.h"
#include "pagespeed/kernel/base/string.h"
#include "pagespeed/kernel/image/image_converter.h"
#include "pagespeed/kernel/image/image_util.h"
#include "pagespeed/kernel/image/png_optimizer.h"
#include "pagespeed/kernel/image/webp_optimizer.h"
#include "test/pagespeed/kernel/base/gtest.h"
#include "test/pagespeed/kernel/base/mock_message_handler.h"

namespace {

using net_instaweb::MockMessageHandler;
using net_instaweb::NullMutex;
using pagespeed::image_compression::ImageConverter;
using pagespeed::image_compression::kIdenticalTilePsnrDb;
using pagespeed::image_compression::PngCompressParams;
using pagespeed::image_compression::PngReader;
using pagespeed::image_compression::PngScanlineWriter;
using pagespeed::image_compression::RGB_888;
using pagespeed::image_compression::WebpConfiguration;
using pagespeed::image_compression::WorstTilePsnr;

class ImageDistortionTest : public testing::Test {
 protected:
  ImageDistortionTest() : message_handler_(new NullMutex) {}

  // A smooth RGB gradient.
  std::vector<uint8_t> Gradient(size_t width, size_t height) {
    std::vector<uint8_t> pixels(width * height * 3);
    for (size_t y = 0; y < height; ++y) {
      for (size_t x = 0; x < width; ++x) {
        uint8_t* p = &pixels[(y * width + x) * 3];
        p[0] = static_cast<uint8_t>(x * 255 / width);
        p[1] = static_cast<uint8_t>(y * 255 / height);
        p[2] = static_cast<uint8_t>(128);
      }
    }
    return pixels;
  }

  GoogleString EncodePng(const std::vector<uint8_t>& pixels, size_t width,
                         size_t height) {
    GoogleString png;
    PngScanlineWriter writer(&message_handler_);
    EXPECT_TRUE(writer.Init(width, height, RGB_888));
    PngCompressParams params(false /* try_best_compression */,
                             false /* progressive */);
    EXPECT_TRUE(writer.InitializeWrite(&params, &png));
    for (size_t y = 0; y < height; ++y) {
      EXPECT_TRUE(writer.WriteNextScanline(&pixels[y * width * 3]));
    }
    EXPECT_TRUE(writer.FinalizeWrite());
    return png;
  }

  // Lossless WebP of a PNG: decodes back to exactly the PNG's pixels, so the
  // tests control the damage themselves.
  GoogleString LosslessWebp(const GoogleString& png) {
    PngReader reader(&message_handler_);
    WebpConfiguration config;
    config.lossless = true;
    GoogleString webp;
    bool is_opaque = false;
    EXPECT_TRUE(ImageConverter::ConvertPngToWebp(
        reader, png, config, &webp, &is_opaque, &message_handler_));
    return webp;
  }

  MockMessageHandler message_handler_;
};

TEST_F(ImageDistortionTest, WorstTilePsnrIdentical) {
  const GoogleString png = EncodePng(Gradient(100, 70), 100, 70);
  PngReader reader(&message_handler_);
  double worst = 0;
  ASSERT_TRUE(WorstTilePsnr(reader, png, LosslessWebp(png), 32, &worst,
                            &message_handler_));
  EXPECT_EQ(kIdenticalTilePsnrDb, worst);
}

// One damaged tile in an otherwise clean frame drives the minimum, while the
// image-wide PSNR stays high; this is the failure the local measure exists for.
TEST_F(ImageDistortionTest, WorstTilePsnrLocalizedDamage) {
  const size_t width = 256, height = 256;
  std::vector<uint8_t> clean = Gradient(width, height);
  std::vector<uint8_t> damaged = clean;
  // Tile (3, 2) of the 32x32 grid: every channel off by 40.
  for (size_t y = 64; y < 96; ++y) {
    for (size_t x = 96; x < 128; ++x) {
      for (size_t c = 0; c < 3; ++c) {
        damaged[(y * width + x) * 3 + c] += 40;
      }
    }
  }
  const GoogleString png = EncodePng(clean, width, height);
  const GoogleString webp = LosslessWebp(EncodePng(damaged, width, height));
  PngReader reader(&message_handler_);
  double worst = 0;
  ASSERT_TRUE(WorstTilePsnr(reader, png, webp, 32, &worst, &message_handler_));
  // A uniform error of 40 is an MSE of 1600: 10*log10(255^2/1600) = 16.09 dB.
  EXPECT_NEAR(16.09, worst, 0.01);

  // The same damage averaged over the whole frame would pass almost any
  // image-wide threshold: 1/64 of the pixels at MSE 1600 is an MSE of 25.
  const double global = 10 * std::log10(255.0 * 255.0 / (1600.0 / 64.0));
  EXPECT_GT(global, 34.0);

  // Smaller tiles localise the same damage no worse.
  double worst_16 = 0;
  ASSERT_TRUE(
      WorstTilePsnr(reader, png, webp, 16, &worst_16, &message_handler_));
  EXPECT_NEAR(16.09, worst_16, 0.01);
}

// Edge slivers smaller than a quarter of a tile are not scored, so a sliver of
// damage there does not decide the result; a full-size edge tile is.
TEST_F(ImageDistortionTest, WorstTilePsnrSkipsEdgeSlivers) {
  const size_t width = 66, height = 64;  // columns 64..65: a 2x32 sliver
  std::vector<uint8_t> clean = Gradient(width, height);
  std::vector<uint8_t> damaged = clean;
  for (size_t y = 0; y < height; ++y) {
    for (size_t x = 64; x < width; ++x) {
      damaged[(y * width + x) * 3] += 60;
    }
  }
  const GoogleString png = EncodePng(clean, width, height);
  PngReader reader(&message_handler_);
  double worst = 0;
  ASSERT_TRUE(WorstTilePsnr(reader, png,
                            LosslessWebp(EncodePng(damaged, width, height)), 32,
                            &worst, &message_handler_));
  EXPECT_EQ(kIdenticalTilePsnrDb, worst);
}

TEST_F(ImageDistortionTest, WorstTilePsnrMismatchedDims) {
  const GoogleString png = EncodePng(Gradient(64, 64), 64, 64);
  const GoogleString other = EncodePng(Gradient(64, 96), 64, 96);
  PngReader reader(&message_handler_);
  double worst = 12.5;
  EXPECT_FALSE(WorstTilePsnr(reader, png, LosslessWebp(other), 32, &worst,
                             &message_handler_));
  EXPECT_EQ(12.5, worst);
}

TEST_F(ImageDistortionTest, WorstTilePsnrRejectsBadInput) {
  const GoogleString png = EncodePng(Gradient(64, 64), 64, 64);
  const GoogleString webp = LosslessWebp(png);
  PngReader reader(&message_handler_);
  double worst = 0;
  EXPECT_FALSE(
      WorstTilePsnr(reader, png, "not a webp", 32, &worst, &message_handler_));
  EXPECT_FALSE(
      WorstTilePsnr(reader, "not a png", webp, 32, &worst, &message_handler_));
  EXPECT_FALSE(WorstTilePsnr(reader, png, webp, 1, &worst, &message_handler_));
  // Too small to hold a scored tile at this tile size.
  const GoogleString tiny = EncodePng(Gradient(8, 8), 8, 8);
  EXPECT_FALSE(WorstTilePsnr(reader, tiny, LosslessWebp(tiny), 32, &worst,
                             &message_handler_));
}

}  // namespace
