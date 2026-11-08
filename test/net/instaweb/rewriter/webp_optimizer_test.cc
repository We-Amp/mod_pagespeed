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

// Unit tests for the routines in webp_optimizer.cc.

#include "net/instaweb/rewriter/public/webp_optimizer.h"

#include "pagespeed/kernel/base/basictypes.h"
#include "pagespeed/kernel/base/null_mutex.h"
#include "pagespeed/kernel/base/string.h"
#include "pagespeed/kernel/base/string_util.h"  // for StrCat
#include "test/pagespeed/kernel/base/gtest.h"
#include "test/pagespeed/kernel/base/mock_message_handler.h"
#include "test/pagespeed/kernel/image/test_utils.h"

#ifdef USE_SYSTEM_LIBWEBP
#include "webp/decode.h"
#include "webp/encode.h"
#else
#include "external/libwebp/src/webp/decode.h"
#endif

namespace net_instaweb {

using pagespeed::image_compression::ReadFile;

const char kTestData[] = "/test/net/instaweb/rewriter/testdata/";
const char kTransparentWebP[] = "chromium-24.webp";

// A minimal JPEG stream declaring a frame of the given size.
GoogleString JpegDeclaring(int width, int height) {
  const char sof0[] = {'\xff',
                       '\xc0',
                       0,
                       11,
                       8,
                       static_cast<char>(height >> 8),
                       static_cast<char>(height),
                       static_cast<char>(width >> 8),
                       static_cast<char>(width),
                       1,
                       1,
                       0x11,
                       0};
  GoogleString jpeg("\xff\xd8", 2);
  jpeg.append(sof0, sizeof(sof0));
  jpeg.append("\xff\xdb\x00\x43\x00", 5);
  jpeg.append(GoogleString(64, 1));
  jpeg.append("\xff\xda\x00\x08\x01\x01\x00\x00\x3f\x00", 10);
  jpeg.append("\xff\xd9", 2);
  return jpeg;
}

TEST(WebpOptimizerTest, ReduceWebpImageQualityPreservesAlpha) {
  // This test verifies that ReduceWebpImageQuality preserves the
  // alpha channel.

  GoogleString input_image, output_image;
  EXPECT_TRUE(ReadFile(StrCat(GTestSrcDir(), kTestData, kTransparentWebP),
                       &input_image));

  WebPBitstreamFeatures features;
  EXPECT_EQ(
      VP8_STATUS_OK,
      WebPGetFeatures(reinterpret_cast<const uint8_t*>(input_image.c_str()),
                      input_image.length(), &features));
  EXPECT_TRUE(features.has_alpha);

  EXPECT_TRUE(ReduceWebpImageQuality(input_image, 50, &output_image));
  EXPECT_NE(input_image, output_image);

  EXPECT_EQ(
      VP8_STATUS_OK,
      WebPGetFeatures(reinterpret_cast<const uint8_t*>(output_image.c_str()),
                      output_image.length(), &features));
  EXPECT_TRUE(features.has_alpha);
}

// A frame over the pixel cap is refused before anything is decoded.
TEST(WebpOptimizerTest, OversizedJpegRejectedBeforeDecode) {
  MockMessageHandler handler(new NullMutex);
  handler.AddPatternToSkipPrinting("*exceed*");
  GoogleString output_image;
  // 128 Mi pixels, over the 100 Mi pixel budget, with each side below the
  // per-side cap and WebP's own 16383 limit.
  EXPECT_FALSE(OptimizeWebp(JpegDeclaring(16383, 8192), 75, nullptr, nullptr,
                            &output_image, &handler));
}
}  // namespace net_instaweb
