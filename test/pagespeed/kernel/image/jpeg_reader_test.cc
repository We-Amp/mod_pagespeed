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

#include "pagespeed/kernel/image/jpeg_reader.h"

#include <iterator>

#include "pagespeed/kernel/base/null_mutex.h"
#include "pagespeed/kernel/base/string.h"
#include "pagespeed/kernel/image/jpeg_optimizer.h"
#include "pagespeed/kernel/image/read_image.h"
#include "test/pagespeed/kernel/base/gtest.h"
#include "test/pagespeed/kernel/base/mock_message_handler.h"
#include "test/pagespeed/kernel/image/jpeg_optimizer_test_helper.h"
#include "test/pagespeed/kernel/image/test_utils.h"

// DO NOT INCLUDE LIBJPEG HEADERS HERE. Doing so causes build errors
// on Windows. If you need to call out to libjpeg, please add helper
// methods in jpeg_optimizer_test_helper.h.

namespace {

using net_instaweb::MockMessageHandler;
using net_instaweb::NullMutex;
using pagespeed::image_compression::IMAGE_JPEG;
using pagespeed::image_compression::IMAGE_PNG;
using pagespeed::image_compression::JpegCompressionOptions;
using pagespeed::image_compression::JpegScanlineReader;
using pagespeed::image_compression::kJpegTestDir;
using pagespeed::image_compression::kMessagePatternLibJpegFailure;
using pagespeed::image_compression::OptimizeJpegWithOptions;
using pagespeed::image_compression::ReadImage;
using pagespeed::image_compression::ReadTestFile;
using pagespeed::image_compression::ReadTestFileWithExt;
using pagespeed::image_compression::SCANLINE_STATUS_INTERNAL_ERROR;
using pagespeed_testing::image_compression::EncodeGrayJpegWithScans;

const char* kValidJpegImages[] = {
    "test411",   // RGB color space with 4:1:1 chroma sub-sampling.
    "test420",   // RGB color space with 4:2:0 chroma sub-sampling.
    "test422",   // RGB color space with 4:2:2 chroma sub-sampling.
    "test444",   // RGB color space with full chroma information.
    "testgray",  // Grayscale color space.
};

const char* kInvalidFiles[] = {
    "notajpeg.png",   // A png.
    "notajpeg.gif",   // A gif.
    "emptyfile.jpg",  // A zero-byte file.
    "corrupt.jpg",    // Invalid huffman code in the image data section.
};

const size_t kInvalidFileCount = std::size(kInvalidFiles);

// The number of scans the reader accepts; jpeg_reader.cc refuses more.
const int kScanLimit = 100;

// Counts the start-of-scan markers in a JPEG stream. Entropy-coded data
// stuffs a zero byte after every 0xff, so a marker cannot occur inside it.
int CountStartOfScanMarkers(const GoogleString& jpeg) {
  int count = 0;
  for (size_t i = 0; i + 1 < jpeg.size(); ++i) {
    if (jpeg[i] == '\xff' && jpeg[i + 1] == '\xda') {
      ++count;
    }
  }
  return count;
}

// Verify that decoded image is accurate for each pixel.
TEST(JpegReaderTest, ValidJpegs) {
  MockMessageHandler message_handler(new NullMutex);
  for (auto& kValidJpegImage : kValidJpegImages) {
    GoogleString jpeg_image, png_image;
    ReadTestFile(kJpegTestDir, kValidJpegImage, "jpg", &jpeg_image);
    ReadTestFile(kJpegTestDir, kValidJpegImage, "png", &png_image);
    DecodeAndCompareImages(IMAGE_PNG, png_image.c_str(), png_image.length(),
                           IMAGE_JPEG, jpeg_image.c_str(), jpeg_image.length(),
                           false,  // ignore_transparent_rgb
                           &message_handler);
  }
}

// Verify that the reader exit gracefully when the input is an invalid JPEG.
TEST(JpegReaderTest, InvalidJpegs) {
  for (size_t i = 0; i < kInvalidFileCount; ++i) {
    GoogleString src_data;
    ReadTestFileWithExt(kJpegTestDir, kInvalidFiles[i], &src_data);
    MockMessageHandler message_handler(new NullMutex);
    JpegScanlineReader reader(&message_handler);
    message_handler.AddPatternToSkipPrinting(kMessagePatternLibJpegFailure);
    if (i < kInvalidFileCount - 1) {
      ASSERT_FALSE(reader.Initialize(src_data.c_str(), src_data.length()));
    } else {
      ASSERT_TRUE(reader.Initialize(src_data.c_str(), src_data.length()));
      void* scanline = nullptr;
      // The image data section of this image is corrupted. The first 89 rows
      // can be decoded correctly, but not for the 90th or later rows.
      for (int row = 0; row < 89; ++row) {
        ASSERT_TRUE(reader.ReadNextScanline(&scanline));
      }
      ASSERT_FALSE(reader.ReadNextScanline(&scanline));
    }
  }
}

// Verify that the reader work properly no matter how many scalines it reads.
TEST(JpegReaderTest, PartialRead) {
  GoogleString image1, image2;
  void* scanline = nullptr;
  MockMessageHandler message_handler(new NullMutex);

  ReadTestFile(kJpegTestDir, kValidJpegImages[0], "jpg", &image1);
  ReadTestFile(kJpegTestDir, kValidJpegImages[1], "jpg", &image2);

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
  // ReadNextScanline leads to death in debugging mode, or a
  // false in release mode.
#ifdef NDEBUG
  EXPECT_FALSE(reader4.ReadNextScanline(&scanline));
#else
  EXPECT_DEATH(reader4.ReadNextScanline(&scanline),
               "The reader was not initialized or does not "
               "have any more scanlines.");
#endif

  ASSERT_TRUE(reader4.Initialize(image2.c_str(), image2.length()));
  ASSERT_TRUE(reader4.ReadNextScanline(&scanline));
}

// A real image, re-encoded with a scan script of spectral bands and
// successive-approximation refinements, decodes with as many scans as the
// limit allows; one scan more stops the decode through libjpeg's error exit,
// in the scanline reader and in both optimizer paths alike.
TEST(JpegReaderTest, DecodeStopsAfterScanLimit) {
  GoogleString source;
  ASSERT_TRUE(ReadTestFile(kJpegTestDir, "testgray", "jpg", &source));
  GoogleString at_limit, past_limit;
  ASSERT_TRUE(EncodeGrayJpegWithScans(source, kScanLimit, &at_limit));
  ASSERT_TRUE(EncodeGrayJpegWithScans(source, kScanLimit + 1, &past_limit));
  ASSERT_EQ(kScanLimit, CountStartOfScanMarkers(at_limit));
  ASSERT_EQ(kScanLimit + 1, CountStartOfScanMarkers(past_limit));

  MockMessageHandler message_handler(new NullMutex);
  message_handler.AddPatternToSkipPrinting(kMessagePatternLibJpegFailure);
  void* scanline = nullptr;

  JpegScanlineReader reader(&message_handler);
  ASSERT_TRUE(reader.Initialize(at_limit.data(), at_limit.length()));
  EXPECT_TRUE(reader.IsProgressive());
  while (reader.HasMoreScanLines()) {
    ASSERT_TRUE(reader.ReadNextScanline(&scanline));
  }

  ASSERT_TRUE(reader.Initialize(past_limit.data(), past_limit.length()));
  EXPECT_EQ(SCANLINE_STATUS_INTERNAL_ERROR,
            reader.ReadNextScanlineWithStatus(&scanline).type());

  auto recompresses = [&message_handler](const GoogleString& jpeg, bool lossy) {
    JpegCompressionOptions options;
    options.lossy = lossy;
    GoogleString output;
    return OptimizeJpegWithOptions(jpeg, &output, options, &message_handler);
  };
  EXPECT_TRUE(recompresses(at_limit, /*lossy=*/false));
  EXPECT_TRUE(recompresses(at_limit, /*lossy=*/true));
  EXPECT_FALSE(recompresses(past_limit, /*lossy=*/false));
  EXPECT_FALSE(recompresses(past_limit, /*lossy=*/true));
}

}  // namespace
