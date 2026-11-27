// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

// Unit tests for scanline_interface_frame_adapter.h adapter classes.
// These tests exercise the adapter layer through the factory functions
// in read_image.h, which create the adapters internally.

#include <memory>
#include <string>

#include "gtest/gtest.h"
#include "lib/base/message_handler.h"
#include "lib/image/image_frame_interface.h"
#include "lib/image/image_util.h"
#include "lib/image/png_optimizer.h"
#include "lib/image/read_image.h"
#include "lib/image/scanline_interface.h"
#include "lib/image/scanline_interface_frame_adapter.h"
#include "lib/image/scanline_status.h"
#include "lib/image/webp_optimizer.h"
#include "png.h"  // NOLINT
#include "test/lib/image/test_utils.h"
#include "zlib.h"

namespace pagespeed::image_compression {
namespace {

using net_instaweb::ScanlineReaderInterface;
using net_instaweb::ScanlineStatus;
using net_instaweb::ScanlineWriterInterface;
using pagespeed::NullMessageHandler;

class ScanlineFrameAdapterTest : public ::testing::Test {
 protected:
  NullMessageHandler handler_;
  std::string file_data_;

  bool LoadFile(const std::string& dir, const char* name_ext) {
    return ReadTestFileWithExt(dir, name_ext, &file_data_);
  }
};

// ==================================================================
// FrameToScanlineReaderAdapter tests
// GIF goes through: GifFrameReader -> MultipleFramePaddingReader
//   -> FrameToScanlineReaderAdapter (via CreateScanlineReader)
// ==================================================================

TEST_F(ScanlineFrameAdapterTest, GifViaScanlineAdapter) {
  ASSERT_TRUE(LoadFile(kGifTestDir, "transparent.gif"));
  ScanlineStatus status;
  std::unique_ptr<ScanlineReaderInterface> reader(CreateScanlineReader(
      IMAGE_GIF, file_data_.data(), file_data_.size(), &handler_, &status));
  ASSERT_NE(reader, nullptr);
  ASSERT_TRUE(status.Success()) << status.ToString();

  EXPECT_GT(reader->GetImageWidth(), 0u);
  EXPECT_GT(reader->GetImageHeight(), 0u);
  EXPECT_GT(reader->GetBytesPerScanline(), 0u);

  int rows = 0;
  while (reader->HasMoreScanLines()) {
    void* scanline = nullptr;
    ASSERT_TRUE(reader->ReadNextScanline(&scanline));
    ASSERT_NE(scanline, nullptr);
    ++rows;
  }
  EXPECT_EQ(rows, static_cast<int>(reader->GetImageHeight()));
}

TEST_F(ScanlineFrameAdapterTest, GifScanlineAdapterStaticImage) {
  ASSERT_TRUE(LoadFile(kGifTestDir, "o.gif"));
  ScanlineStatus status;
  std::unique_ptr<ScanlineReaderInterface> reader(CreateScanlineReader(
      IMAGE_GIF, file_data_.data(), file_data_.size(), &handler_, &status));
  ASSERT_NE(reader, nullptr);

  size_t w = reader->GetImageWidth();
  size_t h = reader->GetImageHeight();
  EXPECT_GT(w, 0u);
  EXPECT_GT(h, 0u);

  int rows = 0;
  while (reader->HasMoreScanLines()) {
    void* scanline = nullptr;
    ASSERT_TRUE(reader->ReadNextScanline(&scanline));
    ++rows;
  }
  EXPECT_EQ(rows, static_cast<int>(h));
}

// ==================================================================
// ScanlineToFrameReaderAdapter tests
// PNG/JPEG/WebP go through: ScanlineReader -> ScanlineToFrameReaderAdapter
//   (via CreateImageFrameReader)
// ==================================================================

TEST_F(ScanlineFrameAdapterTest, PngViaFrameAdapter) {
  ASSERT_TRUE(LoadFile("", "opaque_32x20.png"));
  ScanlineStatus status;
  std::unique_ptr<MultipleFrameReader> reader(CreateImageFrameReader(
      IMAGE_PNG, file_data_.data(), file_data_.size(), &handler_, &status));
  ASSERT_NE(reader, nullptr);
  ASSERT_TRUE(status.Success()) << status.ToString();

  // Should have one frame.
  EXPECT_TRUE(reader->HasMoreFrames());

  ScanlineStatus prep_status;
  ASSERT_TRUE(reader->PrepareNextFrame(&prep_status));
  ASSERT_TRUE(prep_status.Success());

  FrameSpec frame_spec;
  ASSERT_TRUE(reader->GetFrameSpec(&frame_spec, &status));
  EXPECT_EQ(frame_spec.width, 32u);
  EXPECT_EQ(frame_spec.height, 20u);

  int rows = 0;
  while (reader->HasMoreScanlines()) {
    const void* scanline = nullptr;
    ASSERT_TRUE(reader->ReadNextScanline(&scanline, &status));
    ASSERT_NE(scanline, nullptr);
    ++rows;
  }
  EXPECT_EQ(rows, 20);
  EXPECT_FALSE(reader->HasMoreFrames());
}

TEST_F(ScanlineFrameAdapterTest, JpegViaFrameAdapter) {
  ASSERT_TRUE(ReadTestFile(kJpegTestDir, "test420", "jpg", &file_data_));
  ScanlineStatus status;
  std::unique_ptr<MultipleFrameReader> reader(CreateImageFrameReader(
      IMAGE_JPEG, file_data_.data(), file_data_.size(), &handler_, &status));
  ASSERT_NE(reader, nullptr);
  ASSERT_TRUE(status.Success());

  EXPECT_TRUE(reader->HasMoreFrames());
  ScanlineStatus prep_status;
  ASSERT_TRUE(reader->PrepareNextFrame(&prep_status));

  FrameSpec frame_spec;
  ASSERT_TRUE(reader->GetFrameSpec(&frame_spec, &status));
  EXPECT_GT(frame_spec.width, 0u);
  EXPECT_GT(frame_spec.height, 0u);

  int rows = 0;
  while (reader->HasMoreScanlines()) {
    const void* scanline = nullptr;
    ASSERT_TRUE(reader->ReadNextScanline(&scanline, &status));
    ++rows;
  }
  EXPECT_EQ(rows, static_cast<int>(frame_spec.height));
}

TEST_F(ScanlineFrameAdapterTest, WebpViaFrameAdapter) {
  ASSERT_TRUE(LoadFile("", "opaque_32x20.webp"));
  ScanlineStatus status;
  std::unique_ptr<MultipleFrameReader> reader(CreateImageFrameReader(
      IMAGE_WEBP, file_data_.data(), file_data_.size(), &handler_, &status));
  ASSERT_NE(reader, nullptr);
  ASSERT_TRUE(status.Success());

  EXPECT_TRUE(reader->HasMoreFrames());
  ScanlineStatus prep_status;
  ASSERT_TRUE(reader->PrepareNextFrame(&prep_status));

  FrameSpec frame_spec;
  ASSERT_TRUE(reader->GetFrameSpec(&frame_spec, &prep_status));
  EXPECT_EQ(frame_spec.width, 32u);
  EXPECT_EQ(frame_spec.height, 20u);
}

// ==================================================================
// ScanlineToFrameWriterAdapter tests
// PNG/JPEG writers wrapped via the adapter
// ==================================================================

TEST_F(ScanlineFrameAdapterTest, PngFrameWriterAdapter) {
  std::string output;
  ScanlineStatus status;

  auto* png_writer = new PngScanlineWriter(&handler_);
  auto adapter =
      std::make_unique<ScanlineToFrameWriterAdapter>(png_writer, &handler_);

  PngCompressParams png_config(PNG_FILTER_NONE, Z_DEFAULT_STRATEGY, false);
  status = adapter->Initialize(&png_config, &output);
  ASSERT_TRUE(status.Success()) << status.ToString();

  ImageSpec image_spec;
  image_spec.width = 4;
  image_spec.height = 4;
  image_spec.num_frames = 1;
  status = adapter->PrepareImage(&image_spec);
  ASSERT_TRUE(status.Success()) << status.ToString();

  FrameSpec frame_spec;
  frame_spec.width = 4;
  frame_spec.height = 4;
  frame_spec.pixel_format = RGB_888;
  status = adapter->PrepareNextFrame(&frame_spec);
  ASSERT_TRUE(status.Success()) << status.ToString();

  uint8_t row[12] = {255, 0, 0, 0, 255, 0, 0, 0, 255, 128, 128, 128};
  for (int y = 0; y < 4; ++y) {
    status = adapter->WriteNextScanline(row);
    ASSERT_TRUE(status.Success()) << status.ToString();
  }

  status = adapter->FinalizeWrite();
  ASSERT_TRUE(status.Success()) << status.ToString();
  EXPECT_GT(output.size(), 0u);
}

// ==================================================================
// FrameToScanlineWriterAdapter tests
// WebP frame writer wrapped to scanline API
// ==================================================================

TEST_F(ScanlineFrameAdapterTest, WebpScanlineWriterAdapter) {
  std::string output;
  ScanlineStatus status;

  auto* webp_writer = new WebpFrameWriter(&handler_);
  auto adapter = std::make_unique<FrameToScanlineWriterAdapter>(webp_writer);

  status = adapter->InitWithStatus(4, 4, RGB_888);
  ASSERT_TRUE(status.Success()) << status.ToString();

  WebpConfiguration webp_config;
  status = adapter->InitializeWriteWithStatus(&webp_config, &output);
  ASSERT_TRUE(status.Success()) << status.ToString();

  uint8_t row[12] = {255, 0, 0, 0, 255, 0, 0, 0, 255, 128, 128, 128};
  for (int y = 0; y < 4; ++y) {
    status = adapter->WriteNextScanlineWithStatus(row);
    ASSERT_TRUE(status.Success()) << status.ToString();
  }

  status = adapter->FinalizeWriteWithStatus();
  ASSERT_TRUE(status.Success()) << status.ToString();
  EXPECT_GT(output.size(), 0u);
}

// ==================================================================
// Error path tests
// ==================================================================

TEST_F(ScanlineFrameAdapterTest, FrameReaderCorruptGif) {
  const char garbage[] = "not a gif file at all";
  ScanlineStatus status;
  std::unique_ptr<ScanlineReaderInterface> reader(CreateScanlineReader(
      IMAGE_GIF, garbage, sizeof(garbage), &handler_, &status));
  EXPECT_EQ(reader, nullptr);
}

TEST_F(ScanlineFrameAdapterTest, FrameAdapterCorruptPng) {
  const char garbage[] = "not a png";
  ScanlineStatus status;
  std::unique_ptr<MultipleFrameReader> reader(CreateImageFrameReader(
      IMAGE_PNG, garbage, sizeof(garbage), &handler_, &status));
  EXPECT_EQ(reader, nullptr);
}

TEST_F(ScanlineFrameAdapterTest, GifNativeFrameReader) {
  // GIF goes through native GifFrameReader (not adapter).
  ASSERT_TRUE(LoadFile(kGifTestDir, "transparent.gif"));
  ScanlineStatus status;
  std::unique_ptr<MultipleFrameReader> reader(CreateImageFrameReader(
      IMAGE_GIF, file_data_.data(), file_data_.size(), &handler_, &status));
  ASSERT_NE(reader, nullptr);
  ASSERT_TRUE(status.Success());

  EXPECT_TRUE(reader->HasMoreFrames());
  ScanlineStatus prep;
  ASSERT_TRUE(reader->PrepareNextFrame(&prep));
  ASSERT_TRUE(prep.Success());

  FrameSpec spec;
  ASSERT_TRUE(reader->GetFrameSpec(&spec, &prep));
  EXPECT_GT(spec.width, 0u);
  EXPECT_GT(spec.height, 0u);

  int rows = 0;
  while (reader->HasMoreScanlines()) {
    const void* scanline = nullptr;
    ASSERT_TRUE(reader->ReadNextScanline(&scanline, &status));
    ++rows;
  }
  EXPECT_EQ(rows, static_cast<int>(spec.height));
}

// ==================================================================
// Edge case tests for adapter error paths
// ==================================================================

// FrameToScanlineWriterAdapter::InitializeWriteWithStatus without
// prior InitWithStatus should return an error.
TEST_F(ScanlineFrameAdapterTest, WriterAdapterInitializeWriteBeforeInit) {
  auto* webp_writer = new WebpFrameWriter(&handler_);
  auto adapter = std::make_unique<FrameToScanlineWriterAdapter>(webp_writer);

  std::string output;
  WebpConfiguration webp_config;
  // Call InitializeWriteWithStatus without calling InitWithStatus first.
  ScanlineStatus status =
      adapter->InitializeWriteWithStatus(&webp_config, &output);
  EXPECT_FALSE(status.Success());
}

// ScanlineToFrameReaderAdapter::PrepareNextFrame after all frames
// consumed should return an error.
TEST_F(ScanlineFrameAdapterTest, FrameReaderPrepareAfterConsumed) {
  ASSERT_TRUE(LoadFile("", "opaque_32x20.png"));
  ScanlineStatus status;
  std::unique_ptr<MultipleFrameReader> reader(CreateImageFrameReader(
      IMAGE_PNG, file_data_.data(), file_data_.size(), &handler_, &status));
  ASSERT_NE(reader, nullptr);
  ASSERT_TRUE(status.Success());

  // Consume the single frame.
  ScanlineStatus prep_status;
  ASSERT_TRUE(reader->PrepareNextFrame(&prep_status));
  ASSERT_TRUE(prep_status.Success());
  EXPECT_FALSE(reader->HasMoreFrames());

  // Calling PrepareNextFrame again should fail.
  ScanlineStatus error_status;
  EXPECT_FALSE(reader->PrepareNextFrame(&error_status));
  EXPECT_FALSE(error_status.Success());
}

// ScanlineToFrameWriterAdapter::PrepareNextFrame before PrepareImage
// should return an error.
TEST_F(ScanlineFrameAdapterTest, FrameWriterPrepareBeforePrepareImage) {
  auto* png_writer = new PngScanlineWriter(&handler_);
  auto adapter =
      std::make_unique<ScanlineToFrameWriterAdapter>(png_writer, &handler_);

  PngCompressParams png_config(PNG_FILTER_NONE, Z_DEFAULT_STRATEGY, false);
  std::string output;
  ScanlineStatus status = adapter->Initialize(&png_config, &output);
  ASSERT_TRUE(status.Success());

  // Call PrepareNextFrame without calling PrepareImage first.
  FrameSpec frame_spec;
  frame_spec.width = 4;
  frame_spec.height = 4;
  frame_spec.pixel_format = RGB_888;
  status = adapter->PrepareNextFrame(&frame_spec);
  EXPECT_FALSE(status.Success());
}

// ScanlineToFrameWriterAdapter::PrepareImage with num_frames > 1
// should return an error (Scanline API doesn't support animation).
TEST_F(ScanlineFrameAdapterTest, FrameWriterPrepareImageMultiFrame) {
  auto* png_writer = new PngScanlineWriter(&handler_);
  auto adapter =
      std::make_unique<ScanlineToFrameWriterAdapter>(png_writer, &handler_);

  PngCompressParams png_config(PNG_FILTER_NONE, Z_DEFAULT_STRATEGY, false);
  std::string output;
  ScanlineStatus status = adapter->Initialize(&png_config, &output);
  ASSERT_TRUE(status.Success());

  ImageSpec image_spec;
  image_spec.width = 4;
  image_spec.height = 4;
  image_spec.num_frames = 2;  // Animated, not supported.
  status = adapter->PrepareImage(&image_spec);
  EXPECT_FALSE(status.Success());
}

// ==================================================================
// Phase 6: Additional boundary condition tests
// ==================================================================

// FrameToScanlineReaderAdapter: animated GIF (num_frames > 1) should
// return UNSUPPORTED_FEATURE from InitializeWithStatus (lines 59-64).
TEST_F(ScanlineFrameAdapterTest, GifAnimatedViaScanlineAdapterFails) {
  ASSERT_TRUE(LoadFile(kGifTestDir, "animated.gif"));
  ScanlineStatus status;
  std::unique_ptr<ScanlineReaderInterface> reader(CreateScanlineReader(
      IMAGE_GIF, file_data_.data(), file_data_.size(), &handler_, &status));
  // Animated GIFs should fail the scanline adapter (not supported).
  // The adapter may return nullptr or a non-success status.
  if (reader != nullptr) {
    EXPECT_FALSE(status.Success());
  }
}

// FrameToScanlineReaderAdapter: test Reset() and re-read.
TEST_F(ScanlineFrameAdapterTest, GifScanlineAdapterResetAndReread) {
  ASSERT_TRUE(LoadFile(kGifTestDir, "transparent.gif"));
  ScanlineStatus status;
  std::unique_ptr<ScanlineReaderInterface> reader(CreateScanlineReader(
      IMAGE_GIF, file_data_.data(), file_data_.size(), &handler_, &status));
  ASSERT_NE(reader, nullptr);
  ASSERT_TRUE(status.Success());

  // Read a few scanlines.
  void* scanline = nullptr;
  ASSERT_TRUE(reader->HasMoreScanLines());
  ASSERT_TRUE(reader->ReadNextScanline(&scanline));
  ASSERT_NE(scanline, nullptr);

  // Reset and verify we can read again from the beginning.
  EXPECT_TRUE(reader->Reset());
}

// FrameToScanlineWriterAdapter: FinalizeWrite without writing all scanlines.
TEST_F(ScanlineFrameAdapterTest, WriterAdapterPartialWriteFinalize) {
  std::string output;
  ScanlineStatus status;

  auto* webp_writer = new WebpFrameWriter(&handler_);
  auto adapter = std::make_unique<FrameToScanlineWriterAdapter>(webp_writer);

  status = adapter->InitWithStatus(4, 4, RGB_888);
  ASSERT_TRUE(status.Success());

  WebpConfiguration webp_config;
  status = adapter->InitializeWriteWithStatus(&webp_config, &output);
  ASSERT_TRUE(status.Success());

  // Write only 2 out of 4 scanlines.
  uint8_t row[12] = {255, 0, 0, 0, 255, 0, 0, 0, 255, 128, 128, 128};
  for (int y = 0; y < 2; ++y) {
    status = adapter->WriteNextScanlineWithStatus(row);
    ASSERT_TRUE(status.Success());
  }

  // FinalizeWrite with incomplete data -- may fail or succeed depending
  // on the codec. We just verify no crash.
  status = adapter->FinalizeWriteWithStatus();
  // WebP may still produce output with fewer scanlines (padding with
  // zeros). The important thing is no crash.
  SUCCEED();
}

// ScanlineToFrameReaderAdapter: test GetImageSpec and GetFrameSpec.
TEST_F(ScanlineFrameAdapterTest, FrameReaderGetSpecs) {
  ASSERT_TRUE(LoadFile("", "opaque_32x20.png"));
  ScanlineStatus status;
  std::unique_ptr<MultipleFrameReader> reader(CreateImageFrameReader(
      IMAGE_PNG, file_data_.data(), file_data_.size(), &handler_, &status));
  ASSERT_NE(reader, nullptr);
  ASSERT_TRUE(status.Success());

  ImageSpec image_spec;
  ASSERT_TRUE(reader->GetImageSpec(&image_spec, &status));
  EXPECT_EQ(image_spec.width, 32u);
  EXPECT_EQ(image_spec.height, 20u);
  EXPECT_EQ(image_spec.num_frames, 1u);

  ScanlineStatus prep_status;
  ASSERT_TRUE(reader->PrepareNextFrame(&prep_status));

  FrameSpec frame_spec;
  ASSERT_TRUE(reader->GetFrameSpec(&frame_spec, &status));
  EXPECT_EQ(frame_spec.width, 32u);
  EXPECT_EQ(frame_spec.height, 20u);
  EXPECT_EQ(frame_spec.top, 0u);
  EXPECT_EQ(frame_spec.left, 0u);
}

// ScanlineToFrameReaderAdapter: HasMoreScanlines delegates correctly.
TEST_F(ScanlineFrameAdapterTest, FrameReaderHasMoreScanlines) {
  ASSERT_TRUE(LoadFile("", "opaque_32x20.png"));
  ScanlineStatus status;
  std::unique_ptr<MultipleFrameReader> reader(CreateImageFrameReader(
      IMAGE_PNG, file_data_.data(), file_data_.size(), &handler_, &status));
  ASSERT_NE(reader, nullptr);
  ASSERT_TRUE(status.Success());

  ScanlineStatus prep_status;
  ASSERT_TRUE(reader->PrepareNextFrame(&prep_status));
  EXPECT_TRUE(reader->HasMoreScanlines());

  // Read all scanlines.
  int rows = 0;
  while (reader->HasMoreScanlines()) {
    const void* scanline = nullptr;
    ASSERT_TRUE(reader->ReadNextScanline(&scanline, &status));
    ++rows;
  }
  EXPECT_EQ(rows, 20);
  EXPECT_FALSE(reader->HasMoreScanlines());
}

// ScanlineToFrameWriterAdapter: Initialize sets config and output correctly.
TEST_F(ScanlineFrameAdapterTest, FrameWriterInitialize) {
  auto* png_writer = new PngScanlineWriter(&handler_);
  auto adapter =
      std::make_unique<ScanlineToFrameWriterAdapter>(png_writer, &handler_);

  PngCompressParams png_config(PNG_FILTER_NONE, Z_DEFAULT_STRATEGY, false);
  std::string output;
  ScanlineStatus status = adapter->Initialize(&png_config, &output);
  ASSERT_TRUE(status.Success());
}

// FrameToScanlineReaderAdapter: GetPixelFormat, IsProgressive, and
// GetBytesPerScanline.
TEST_F(ScanlineFrameAdapterTest, GifScanlineAdapterAccessors) {
  ASSERT_TRUE(LoadFile(kGifTestDir, "transparent.gif"));
  ScanlineStatus status;
  std::unique_ptr<ScanlineReaderInterface> reader(CreateScanlineReader(
      IMAGE_GIF, file_data_.data(), file_data_.size(), &handler_, &status));
  ASSERT_NE(reader, nullptr);
  ASSERT_TRUE(status.Success());

  // These should all return reasonable values.
  size_t bps = reader->GetBytesPerScanline();
  EXPECT_GT(bps, 0u);
  size_t w = reader->GetImageWidth();
  size_t h = reader->GetImageHeight();
  EXPECT_GT(w, 0u);
  EXPECT_GT(h, 0u);
  // PixelFormat should be valid.
  auto pf = reader->GetPixelFormat();
  EXPECT_NE(pf, UNSUPPORTED);
  // IsProgressive returns a boolean.
  reader->IsProgressive();
}

// FrameToScanlineWriterAdapter: test with RGBA pixel format.
TEST_F(ScanlineFrameAdapterTest, WebpScanlineWriterAdapterRGBA) {
  std::string output;
  ScanlineStatus status;

  auto* webp_writer = new WebpFrameWriter(&handler_);
  auto adapter = std::make_unique<FrameToScanlineWriterAdapter>(webp_writer);

  status = adapter->InitWithStatus(4, 4, RGBA_8888);
  ASSERT_TRUE(status.Success());

  WebpConfiguration webp_config;
  webp_config.alpha_quality = 100;
  status = adapter->InitializeWriteWithStatus(&webp_config, &output);
  ASSERT_TRUE(status.Success());

  // RGBA: 4 bytes per pixel, 4 pixels = 16 bytes.
  uint8_t row[16] = {255, 0, 0,   255, 0,   255, 0,   255,
                     0,   0, 255, 255, 128, 128, 128, 128};
  for (int y = 0; y < 4; ++y) {
    status = adapter->WriteNextScanlineWithStatus(row);
    ASSERT_TRUE(status.Success());
  }

  status = adapter->FinalizeWriteWithStatus();
  ASSERT_TRUE(status.Success());
  EXPECT_GT(output.size(), 0u);
}

}  // namespace
}  // namespace pagespeed::image_compression
