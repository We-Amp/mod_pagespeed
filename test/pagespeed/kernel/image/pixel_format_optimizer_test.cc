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

#include "pagespeed/kernel/image/pixel_format_optimizer.h"

#include <iterator>
#include <memory>

#include "pagespeed/kernel/base/basictypes.h"
#include "pagespeed/kernel/base/null_mutex.h"
#include "pagespeed/kernel/base/string.h"
#include "pagespeed/kernel/image/png_optimizer.h"
#include "test/pagespeed/kernel/base/gtest.h"
#include "test/pagespeed/kernel/base/mock_message_handler.h"
#include "test/pagespeed/kernel/image/test_utils.h"

namespace {

using net_instaweb::MessageHandler;
using net_instaweb::MockMessageHandler;
using net_instaweb::NullMutex;
using pagespeed::image_compression::kMessagePatternUnexpectedEOF;
using pagespeed::image_compression::kWebpTestDir;
using pagespeed::image_compression::PixelFormat;
using pagespeed::image_compression::PixelFormatOptimizer;
using pagespeed::image_compression::PngScanlineReaderRaw;
using pagespeed::image_compression::ReadTestFileWithExt;
using pagespeed::image_compression::RGBA_8888;
using pagespeed::image_compression::SCANLINE_STATUS_INVOCATION_ERROR;
using pagespeed::image_compression::SCANLINE_STATUS_SUCCESS;
using pagespeed::image_compression::ScanlineReaderInterface;
using pagespeed::image_compression::ScanlineStatus;

const char kOpaqueAlphaImage[] = "completely_opaque_32x20.png";
const char kNoAlphaImage[] = "opaque_32x20.png";

const char* kUnoptimizableImages[] = {
    "pagespeed_32x32_gray.png",    // no alpha, gray scale
    "opaque_32x20.png",            // no alpha, RGB
    "alpha_32x32.png",             // alpha, first few pixels are transparent
    "partially_opaque_32x20.png",  // alpha, only last pixel is transparent
};
const size_t kUnoptimizableImageCount = std::size(kUnoptimizableImages);

// Message to ignore.
const char kMessagePatternScanline[] = "*scanline*";
const char kMessagePatternLongJmp[] = "*longjmp()*";

class PixelFormatOptimizerTest : public testing::Test {
 public:
  PixelFormatOptimizerTest()
      : message_handler_(new NullMutex),
        optimizer_(&message_handler_),
        gold_reader_(&message_handler_) {}

  bool InitializeOptimizer(const char* file_name) {
    if (!ReadTestFileWithExt(kWebpTestDir, file_name, &input_image_)) {
      return false;
    }

    std::unique_ptr<PngScanlineReaderRaw> input_reader(
        new PngScanlineReaderRaw(&message_handler_));

    if (!input_reader->Initialize(input_image_.data(), input_image_.length())) {
      return false;
    }
    return optimizer_.Initialize(input_reader.release()).Success();
  }

  bool InitializeGoldReader(const char* file_name) {
    if (!ReadTestFileWithExt(kWebpTestDir, file_name, &gold_image_)) {
      return false;
    }
    return gold_reader_.Initialize(gold_image_.data(), gold_image_.length());
  }

 protected:
  MockMessageHandler message_handler_;
  PixelFormatOptimizer optimizer_;
  PngScanlineReaderRaw gold_reader_;
  GoogleString input_image_;
  GoogleString gold_image_;
  void* scanline_;

 private:
  PixelFormatOptimizerTest(const PixelFormatOptimizerTest&) = delete;
  PixelFormatOptimizerTest& operator=(const PixelFormatOptimizerTest&) = delete;
};

// A minimal RGBA reader that reports the given dimensions without holding
// any image data, so the optimizer's handling of header dimensions alone
// can be exercised.
class FakeRgbaReader : public ScanlineReaderInterface {
 public:
  FakeRgbaReader(size_t width, size_t height)
      : width_(width), height_(height) {}

  bool Reset() override { return true; }
  size_t GetBytesPerScanline() override { return width_ * 4; }
  bool HasMoreScanLines() override { return false; }
  ScanlineStatus InitializeWithStatus(const void* /* image_buffer */,
                                      size_t /* buffer_length */) override {
    return ScanlineStatus(SCANLINE_STATUS_SUCCESS);
  }
  ScanlineStatus ReadNextScanlineWithStatus(
      void** /* out_scanline_bytes */) override {
    return ScanlineStatus(SCANLINE_STATUS_INVOCATION_ERROR);
  }
  size_t GetImageHeight() override { return height_; }
  size_t GetImageWidth() override { return width_; }
  PixelFormat GetPixelFormat() override { return RGBA_8888; }
  bool IsProgressive() override { return false; }

 private:
  size_t width_;
  size_t height_;
};

// The optimizable image will be converted to a new image.
TEST_F(PixelFormatOptimizerTest, Optimizable) {
  ASSERT_TRUE(InitializeOptimizer(kOpaqueAlphaImage));
  ASSERT_TRUE(InitializeGoldReader(kNoAlphaImage));
  CompareImageReaders(&gold_reader_, &optimizer_);
}

// The un-optimizable images will stay the same after conversion.
TEST_F(PixelFormatOptimizerTest, Unoptimizable) {
  for (auto file_name : kUnoptimizableImages) {
    ASSERT_TRUE(InitializeOptimizer(file_name));
    ASSERT_TRUE(InitializeGoldReader(file_name));
    CompareImageReaders(&gold_reader_, &optimizer_);
  }
}

// Test that we don't have memory leakage if the object is initialized
// but no scanline is read.
TEST_F(PixelFormatOptimizerTest, InitializeWithoutRead) {
  ASSERT_TRUE(InitializeOptimizer(kOpaqueAlphaImage));
}

// An image whose dimensions imply a whole-image buffer far beyond anything
// we can decode must be refused by Initialize(), before that buffer is
// allocated.
TEST_F(PixelFormatOptimizerTest, OversizedImageRejected) {
  message_handler_.AddPatternToSkipPrinting("*dimensions*");
  // Initialize() takes ownership of the reader it accepts.
  EXPECT_FALSE(
      optimizer_.Initialize(new FakeRgbaReader(60000, 60000)).Success());
}

// Test that we don't have memory leakage if we don't read all of the
// scanlines.
TEST_F(PixelFormatOptimizerTest, ReadOneRow) {
  ASSERT_TRUE(InitializeOptimizer(kOpaqueAlphaImage));
  ASSERT_TRUE(optimizer_.ReadNextScanline(&scanline_));
}

TEST_F(PixelFormatOptimizerTest, ReinitializeAfterOneRow) {
  ASSERT_TRUE(InitializeOptimizer(kOpaqueAlphaImage));
  ASSERT_TRUE(optimizer_.ReadNextScanline(&scanline_));
  ASSERT_TRUE(InitializeOptimizer(kOpaqueAlphaImage));
  ASSERT_TRUE(InitializeGoldReader(kNoAlphaImage));
  CompareImageReaders(&gold_reader_, &optimizer_);
}

TEST_F(PixelFormatOptimizerTest, ReInitializeAfterLastRow) {
  message_handler_.AddPatternToSkipPrinting(kMessagePatternScanline);
  ASSERT_TRUE(InitializeOptimizer(kOpaqueAlphaImage));
  while (optimizer_.HasMoreScanLines()) {
    ASSERT_TRUE(optimizer_.ReadNextScanline(&scanline_));
  }
  ASSERT_FALSE(optimizer_.ReadNextScanline(&scanline_));
  // Initalize and use the object again.
  ASSERT_TRUE(InitializeOptimizer(kOpaqueAlphaImage));
  ASSERT_TRUE(InitializeGoldReader(kNoAlphaImage));
  CompareImageReaders(&gold_reader_, &optimizer_);
}

// The truncated image leads to a bad reader, which consequently causes
// the optimizer fail to initialize.
TEST_F(PixelFormatOptimizerTest, TruncatedImage) {
  message_handler_.AddPatternToSkipPrinting(kMessagePatternLongJmp);
  message_handler_.AddPatternToSkipPrinting(kMessagePatternScanline);
  message_handler_.AddPatternToSkipPrinting(kMessagePatternUnexpectedEOF);

  ASSERT_TRUE(
      ReadTestFileWithExt(kWebpTestDir, kOpaqueAlphaImage, &input_image_));
  int truncated_length = input_image_.length() * 0.8;

  std::unique_ptr<PngScanlineReaderRaw> input_reader(
      new PngScanlineReaderRaw(&message_handler_));
  ASSERT_TRUE(input_reader->Initialize(input_image_.data(), truncated_length));
  ASSERT_FALSE(optimizer_.Initialize(input_reader.release()).Success());
}

}  // namespace
