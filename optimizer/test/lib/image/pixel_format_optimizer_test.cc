// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

// Tests for PixelFormatOptimizer.
// Ported from mod_pagespeed's
// test/pagespeed/kernel/image/pixel_format_optimizer_test.cc

#include "lib/image/pixel_format_optimizer.h"

#include <cstddef>
#include <memory>
#include <string>

#include "gtest/gtest.h"
#include "lib/base/basictypes.h"
#include "lib/base/message_handler.h"
#include "lib/image/image_util.h"
#include "lib/image/png_optimizer.h"
#include "lib/image/scanline_interface.h"
#include "lib/image/scanline_status.h"
#include "test/lib/image/test_utils.h"

namespace {

using net_instaweb::SCANLINE_STATUS_INVOCATION_ERROR;
using net_instaweb::SCANLINE_STATUS_SUCCESS;
using net_instaweb::ScanlineReaderInterface;
using net_instaweb::ScanlineStatus;
using pagespeed::NullMessageHandler;
using pagespeed::image_compression::CompareImageReaders;
using pagespeed::image_compression::kWebpTestDir;
using pagespeed::image_compression::PixelFormat;
using pagespeed::image_compression::PixelFormatOptimizer;
using pagespeed::image_compression::PngScanlineReaderRaw;
using pagespeed::image_compression::ReadTestFileWithExt;
using pagespeed::image_compression::RGBA_8888;

const char kOpaqueAlphaImage[] = "completely_opaque_32x20.png";
const char kNoAlphaImage[] = "opaque_32x20.png";

const char* kUnoptimizableImages[] = {
    "pagespeed_32x32_gray.png",    // no alpha, gray scale
    "opaque_32x20.png",            // no alpha, RGB
    "alpha_32x32.png",             // alpha, first few pixels are
                                   // transparent
    "partially_opaque_32x20.png",  // alpha, only last pixel is
                                   // transparent
};
class PixelFormatOptimizerTest : public testing::Test {
 public:
  PixelFormatOptimizerTest()
      : optimizer_(&message_handler_), gold_reader_(&message_handler_) {}

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
  NullMessageHandler message_handler_;
  PixelFormatOptimizer optimizer_;
  PngScanlineReaderRaw gold_reader_;
  std::string input_image_;
  std::string gold_image_;
  void* scanline_;

 private:
  DISALLOW_COPY_AND_ASSIGN(PixelFormatOptimizerTest);
};

// A minimal RGBA reader that reports the given dimensions without holding
// any image data, so the optimizer's handling of header dimensions alone
// can be exercised. Counts scanline reads into *read_calls so a test can
// tell whether the optimizer tried to buffer the image.
class FakeRgbaReader : public ScanlineReaderInterface {
 public:
  FakeRgbaReader(size_t width, size_t height, int* read_calls)
      : width_(width), height_(height), read_calls_(read_calls) {}

  bool Reset() override { return true; }
  size_t GetBytesPerScanline() override { return width_ * 4; }
  bool HasMoreScanLines() override { return false; }
  ScanlineStatus InitializeWithStatus(const void* /* image_buffer */,
                                      size_t /* buffer_length */) override {
    return ScanlineStatus(SCANLINE_STATUS_SUCCESS);
  }
  ScanlineStatus ReadNextScanlineWithStatus(
      void** /* out_scanline_bytes */) override {
    ++*read_calls_;
    return ScanlineStatus(SCANLINE_STATUS_INVOCATION_ERROR);
  }
  size_t GetImageHeight() override { return height_; }
  size_t GetImageWidth() override { return width_; }
  PixelFormat GetPixelFormat() override { return RGBA_8888; }
  bool IsProgressive() override { return false; }

 private:
  size_t width_;
  size_t height_;
  int* read_calls_;
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
// allocated and before a single scanline is requested.
TEST_F(PixelFormatOptimizerTest, OversizedImageRejected) {
  int read_calls = 0;
  // Initialize() takes ownership of the reader it accepts.
  EXPECT_FALSE(
      optimizer_.Initialize(new FakeRgbaReader(60000, 60000, &read_calls))
          .Success());
  EXPECT_EQ(0, read_calls);
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
  ASSERT_TRUE(InitializeOptimizer(kOpaqueAlphaImage));
  while (optimizer_.HasMoreScanLines()) {
    ASSERT_TRUE(optimizer_.ReadNextScanline(&scanline_));
  }
  ASSERT_FALSE(optimizer_.ReadNextScanline(&scanline_));
  // Initialize and use the object again.
  ASSERT_TRUE(InitializeOptimizer(kOpaqueAlphaImage));
  ASSERT_TRUE(InitializeGoldReader(kNoAlphaImage));
  CompareImageReaders(&gold_reader_, &optimizer_);
}

// The truncated image leads to a bad reader, which consequently
// causes the optimizer fail to initialize.
TEST_F(PixelFormatOptimizerTest, TruncatedImage) {
  ASSERT_TRUE(
      ReadTestFileWithExt(kWebpTestDir, kOpaqueAlphaImage, &input_image_));
  int truncated_length = input_image_.length() * 0.8;

  std::unique_ptr<PngScanlineReaderRaw> input_reader(
      new PngScanlineReaderRaw(&message_handler_));
  ASSERT_TRUE(input_reader->Initialize(input_image_.data(), truncated_length));
  ASSERT_FALSE(optimizer_.Initialize(input_reader.release()).Success());
}

// Calling InitializeWithStatus directly should return an error.
TEST_F(PixelFormatOptimizerTest, InitializeWithStatusReturnsError) {
  auto status = optimizer_.InitializeWithStatus(nullptr, 0);
  EXPECT_FALSE(status.Success());
}

// Calling Initialize with nullptr reader should return an error.
TEST_F(PixelFormatOptimizerTest, InitializeNullReaderReturnsError) {
  auto status = optimizer_.Initialize(nullptr);
  EXPECT_FALSE(status.Success());
}

// ReadNextScanlineWithStatus before Initialize should return an error.
TEST_F(PixelFormatOptimizerTest, ReadBeforeInitializeReturnsError) {
  void* scanline = nullptr;
  auto status = optimizer_.ReadNextScanlineWithStatus(&scanline);
  EXPECT_FALSE(status.Success());
}

}  // namespace
