// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

// Unit tests for read_image.h format dispatch factories.

#include "lib/image/read_image.h"

#include <cstdint>
#include <cstdlib>
#include <string>

#include "gtest/gtest.h"
#include "lib/base/message_handler.h"
#include "lib/image/image_frame_interface.h"
#include "lib/image/image_util.h"
#include "lib/image/jpeg_optimizer.h"
#include "lib/image/png_optimizer.h"
#include "lib/image/scanline_interface.h"
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

class ReadImageTest : public ::testing::Test {
 protected:
  bool LoadTestFile(const std::string& dir, const char* name, const char* ext) {
    return ReadTestFile(dir, name, ext, &file_data_);
  }

  bool LoadTestFileExt(const std::string& dir, const char* name_ext) {
    return ReadTestFileWithExt(dir, name_ext, &file_data_);
  }

  NullMessageHandler handler_;
  std::string file_data_;
};

// ------------------------------------------------------------------
// CreateScanlineReader tests
// ------------------------------------------------------------------

TEST_F(ReadImageTest, CreateScanlineReaderPng) {
  ASSERT_TRUE(LoadTestFileExt("", "opaque_32x20.png"));
  ScanlineStatus status;
  std::unique_ptr<ScanlineReaderInterface> reader(CreateScanlineReader(
      IMAGE_PNG, file_data_.data(), file_data_.size(), &handler_, &status));
  ASSERT_NE(reader, nullptr);
  EXPECT_TRUE(status.Success());
  EXPECT_EQ(reader->GetImageWidth(), 32u);
  EXPECT_EQ(reader->GetImageHeight(), 20u);
}

TEST_F(ReadImageTest, CreateScanlineReaderJpeg) {
  ASSERT_TRUE(LoadTestFile(kJpegTestDir, "test420", "jpg"));
  ScanlineStatus status;
  std::unique_ptr<ScanlineReaderInterface> reader(CreateScanlineReader(
      IMAGE_JPEG, file_data_.data(), file_data_.size(), &handler_, &status));
  ASSERT_NE(reader, nullptr);
  EXPECT_TRUE(status.Success());
  EXPECT_GT(reader->GetImageWidth(), 0u);
  EXPECT_GT(reader->GetImageHeight(), 0u);
}

TEST_F(ReadImageTest, CreateScanlineReaderWebp) {
  ASSERT_TRUE(LoadTestFileExt("", "opaque_32x20.webp"));
  ScanlineStatus status;
  std::unique_ptr<ScanlineReaderInterface> reader(CreateScanlineReader(
      IMAGE_WEBP, file_data_.data(), file_data_.size(), &handler_, &status));
  ASSERT_NE(reader, nullptr);
  EXPECT_TRUE(status.Success());
  EXPECT_EQ(reader->GetImageWidth(), 32u);
  EXPECT_EQ(reader->GetImageHeight(), 20u);
}

TEST_F(ReadImageTest, CreateScanlineReaderGif) {
  ASSERT_TRUE(LoadTestFileExt(kGifTestDir, "transparent.gif"));
  ScanlineStatus status;
  std::unique_ptr<ScanlineReaderInterface> reader(CreateScanlineReader(
      IMAGE_GIF, file_data_.data(), file_data_.size(), &handler_, &status));
  ASSERT_NE(reader, nullptr);
  EXPECT_TRUE(status.Success());
  EXPECT_GT(reader->GetImageWidth(), 0u);
}

TEST_F(ReadImageTest, CreateScanlineReaderUnknownFormat) {
  ScanlineStatus status;
  std::unique_ptr<ScanlineReaderInterface> reader(
      CreateScanlineReader(IMAGE_UNKNOWN, "x", 1, &handler_, &status));
  EXPECT_EQ(reader, nullptr);
  EXPECT_FALSE(status.Success());
}

TEST_F(ReadImageTest, CreateScanlineReaderCorruptData) {
  const char garbage[] = "not an image";
  ScanlineStatus status;
  std::unique_ptr<ScanlineReaderInterface> reader(CreateScanlineReader(
      IMAGE_PNG, garbage, sizeof(garbage), &handler_, &status));
  EXPECT_EQ(reader, nullptr);
}

// ------------------------------------------------------------------
// CreateScanlineWriter tests
// ------------------------------------------------------------------

TEST_F(ReadImageTest, CreateScanlineWriterPng) {
  std::string output;
  ScanlineStatus status;
  PngCompressParams png_config(PNG_FILTER_NONE, Z_DEFAULT_STRATEGY, false);
  std::unique_ptr<ScanlineWriterInterface> writer(CreateScanlineWriter(
      IMAGE_PNG, RGB_888, 2, 2, &png_config, &output, &handler_, &status));
  ASSERT_NE(writer, nullptr);
  EXPECT_TRUE(status.Success());
}

TEST_F(ReadImageTest, CreateScanlineWriterJpeg) {
  std::string output;
  ScanlineStatus status;
  JpegCompressionOptions jpeg_config;
  jpeg_config.lossy = true;
  jpeg_config.lossy_options.quality = 85;
  std::unique_ptr<ScanlineWriterInterface> writer(CreateScanlineWriter(
      IMAGE_JPEG, RGB_888, 2, 2, &jpeg_config, &output, &handler_, &status));
  ASSERT_NE(writer, nullptr);
  EXPECT_TRUE(status.Success());
}

TEST_F(ReadImageTest, CreateScanlineWriterWebp) {
  std::string output;
  ScanlineStatus status;
  WebpConfiguration webp_config;
  std::unique_ptr<ScanlineWriterInterface> writer(CreateScanlineWriter(
      IMAGE_WEBP, RGB_888, 2, 2, &webp_config, &output, &handler_, &status));
  ASSERT_NE(writer, nullptr);
  EXPECT_TRUE(status.Success());
}

TEST_F(ReadImageTest, CreateScanlineWriterGifUnsupported) {
  std::string output;
  ScanlineStatus status;
  std::unique_ptr<ScanlineWriterInterface> writer(CreateScanlineWriter(
      IMAGE_GIF, RGB_888, 2, 2, nullptr, &output, &handler_, &status));
  EXPECT_EQ(writer, nullptr);
  EXPECT_FALSE(status.Success());
}

TEST_F(ReadImageTest, CreateScanlineWriterUnknown) {
  std::string output;
  ScanlineStatus status;
  std::unique_ptr<ScanlineWriterInterface> writer(CreateScanlineWriter(
      IMAGE_UNKNOWN, RGB_888, 2, 2, nullptr, &output, &handler_, &status));
  EXPECT_EQ(writer, nullptr);
  EXPECT_FALSE(status.Success());
}

// ------------------------------------------------------------------
// CreateImageFrameReader tests
// ------------------------------------------------------------------

TEST_F(ReadImageTest, CreateImageFrameReaderPng) {
  ASSERT_TRUE(LoadTestFileExt("", "opaque_32x20.png"));
  ScanlineStatus status;
  std::unique_ptr<MultipleFrameReader> reader(CreateImageFrameReader(
      IMAGE_PNG, file_data_.data(), file_data_.size(), &handler_, &status));
  ASSERT_NE(reader, nullptr);
  EXPECT_TRUE(status.Success());
}

TEST_F(ReadImageTest, CreateImageFrameReaderJpeg) {
  ASSERT_TRUE(LoadTestFile(kJpegTestDir, "test420", "jpg"));
  ScanlineStatus status;
  std::unique_ptr<MultipleFrameReader> reader(CreateImageFrameReader(
      IMAGE_JPEG, file_data_.data(), file_data_.size(), &handler_, &status));
  ASSERT_NE(reader, nullptr);
  EXPECT_TRUE(status.Success());
}

TEST_F(ReadImageTest, CreateImageFrameReaderWebp) {
  ASSERT_TRUE(LoadTestFileExt("", "opaque_32x20.webp"));
  ScanlineStatus status;
  std::unique_ptr<MultipleFrameReader> reader(CreateImageFrameReader(
      IMAGE_WEBP, file_data_.data(), file_data_.size(), &handler_, &status));
  ASSERT_NE(reader, nullptr);
  EXPECT_TRUE(status.Success());
}

TEST_F(ReadImageTest, CreateImageFrameReaderGif) {
  ASSERT_TRUE(LoadTestFileExt(kGifTestDir, "transparent.gif"));
  ScanlineStatus status;
  std::unique_ptr<MultipleFrameReader> reader(CreateImageFrameReader(
      IMAGE_GIF, file_data_.data(), file_data_.size(), &handler_, &status));
  ASSERT_NE(reader, nullptr);
  EXPECT_TRUE(status.Success());
}

TEST_F(ReadImageTest, CreateImageFrameReaderUnknown) {
  ScanlineStatus status;
  std::unique_ptr<MultipleFrameReader> reader(
      CreateImageFrameReader(IMAGE_UNKNOWN, "x", 1, &handler_, &status));
  EXPECT_EQ(reader, nullptr);
}

// ------------------------------------------------------------------
// CreateImageFrameWriter tests
// ------------------------------------------------------------------

TEST_F(ReadImageTest, CreateImageFrameWriterPng) {
  std::string output;
  ScanlineStatus status;
  std::unique_ptr<MultipleFrameWriter> writer(
      CreateImageFrameWriter(IMAGE_PNG, nullptr, &output, &handler_, &status));
  ASSERT_NE(writer, nullptr);
  EXPECT_TRUE(status.Success());
}

TEST_F(ReadImageTest, CreateImageFrameWriterJpeg) {
  std::string output;
  ScanlineStatus status;
  std::unique_ptr<MultipleFrameWriter> writer(
      CreateImageFrameWriter(IMAGE_JPEG, nullptr, &output, &handler_, &status));
  ASSERT_NE(writer, nullptr);
  EXPECT_TRUE(status.Success());
}

TEST_F(ReadImageTest, CreateImageFrameWriterWebp) {
  std::string output;
  ScanlineStatus status;
  WebpConfiguration webp_config;
  std::unique_ptr<MultipleFrameWriter> writer(CreateImageFrameWriter(
      IMAGE_WEBP, &webp_config, &output, &handler_, &status));
  ASSERT_NE(writer, nullptr);
  EXPECT_TRUE(status.Success());
}

// ------------------------------------------------------------------
// ReadImage utility tests
// ------------------------------------------------------------------

TEST_F(ReadImageTest, ReadImagePngMetadataOnly) {
  ASSERT_TRUE(LoadTestFileExt("", "opaque_32x20.png"));
  size_t w = 0, h = 0, stride = 0;
  PixelFormat fmt;
  EXPECT_TRUE(ReadImage(IMAGE_PNG, file_data_.data(), file_data_.size(),
                        nullptr, &fmt, &w, &h, &stride, &handler_));
  EXPECT_EQ(w, 32u);
  EXPECT_EQ(h, 20u);
  EXPECT_GT(stride, 0u);
  // Stride is 4-byte aligned.
  EXPECT_EQ(stride % 4, 0u);
}

TEST_F(ReadImageTest, ReadImagePngWithPixels) {
  ASSERT_TRUE(LoadTestFileExt("", "opaque_32x20.png"));
  void* pixels = nullptr;
  size_t w = 0, h = 0;
  PixelFormat fmt;
  EXPECT_TRUE(ReadImage(IMAGE_PNG, file_data_.data(), file_data_.size(),
                        &pixels, &fmt, &w, &h, nullptr, &handler_));
  EXPECT_NE(pixels, nullptr);
  EXPECT_EQ(w, 32u);
  EXPECT_EQ(h, 20u);
  free(pixels);
}

TEST_F(ReadImageTest, ReadImageJpeg) {
  ASSERT_TRUE(LoadTestFile(kJpegTestDir, "test420", "jpg"));
  size_t w = 0, h = 0;
  PixelFormat fmt;
  EXPECT_TRUE(ReadImage(IMAGE_JPEG, file_data_.data(), file_data_.size(),
                        nullptr, &fmt, &w, &h, nullptr, &handler_));
  EXPECT_GT(w, 0u);
  EXPECT_GT(h, 0u);
}

TEST_F(ReadImageTest, ReadImageWebp) {
  ASSERT_TRUE(LoadTestFileExt("", "opaque_32x20.webp"));
  size_t w = 0, h = 0;
  EXPECT_TRUE(ReadImage(IMAGE_WEBP, file_data_.data(), file_data_.size(),
                        nullptr, nullptr, &w, &h, nullptr, &handler_));
  EXPECT_EQ(w, 32u);
  EXPECT_EQ(h, 20u);
}

TEST_F(ReadImageTest, ReadImageInvalidFormat) {
  EXPECT_FALSE(ReadImage(IMAGE_UNKNOWN, "x", 1, nullptr, nullptr, nullptr,
                         nullptr, nullptr, &handler_));
}

TEST_F(ReadImageTest, ReadImageCorruptData) {
  const char garbage[] = "not a png";
  EXPECT_FALSE(ReadImage(IMAGE_PNG, garbage, sizeof(garbage), nullptr, nullptr,
                         nullptr, nullptr, nullptr, &handler_));
}

TEST_F(ReadImageTest, ReadImageAlphaPng) {
  ASSERT_TRUE(LoadTestFileExt("", "alpha_32x32.png"));
  void* pixels = nullptr;
  size_t w = 0, h = 0;
  PixelFormat fmt;
  EXPECT_TRUE(ReadImage(IMAGE_PNG, file_data_.data(), file_data_.size(),
                        &pixels, &fmt, &w, &h, nullptr, &handler_));
  EXPECT_NE(pixels, nullptr);
  EXPECT_EQ(w, 32u);
  EXPECT_EQ(h, 32u);
  EXPECT_EQ(fmt, RGBA_8888);
  free(pixels);
}

// ------------------------------------------------------------------
// Additional coverage: GIF scanline reader with pixel decoding,
// ReadImage with GIF, JPEG writer full round-trip, frame writer
// unsupported formats
// ------------------------------------------------------------------

// Exercise ReadImage with a static GIF, decoding pixels.
// This exercises the GIF branch in InstantiateScanlineReader (lines 67-76
// of read_image.cc), including FrameToScanlineReaderAdapter instantiation.
TEST_F(ReadImageTest, ReadImageGifWithPixels) {
  ASSERT_TRUE(LoadTestFileExt(kGifTestDir, "transparent.gif"));
  void* pixels = nullptr;
  size_t w = 0, h = 0, stride = 0;
  PixelFormat fmt;
  EXPECT_TRUE(ReadImage(IMAGE_GIF, file_data_.data(), file_data_.size(),
                        &pixels, &fmt, &w, &h, &stride, &handler_));
  EXPECT_NE(pixels, nullptr);
  EXPECT_GT(w, 0u);
  EXPECT_GT(h, 0u);
  EXPECT_GT(stride, 0u);
  EXPECT_EQ(stride % 4, 0u);
  free(pixels);
}

// ReadImage with a static (non-animated) GIF, metadata only.
TEST_F(ReadImageTest, ReadImageGifMetadataOnly) {
  ASSERT_TRUE(LoadTestFileExt(kGifTestDir, "transparent.gif"));
  size_t w = 0, h = 0;
  PixelFormat fmt;
  EXPECT_TRUE(ReadImage(IMAGE_GIF, file_data_.data(), file_data_.size(),
                        nullptr, &fmt, &w, &h, nullptr, &handler_));
  EXPECT_GT(w, 0u);
  EXPECT_GT(h, 0u);
}

// ReadImage with a simple opaque GIF.
TEST_F(ReadImageTest, ReadImageGifOpaqueWithPixels) {
  ASSERT_TRUE(LoadTestFileExt(kGifTestDir, "o.gif"));
  void* pixels = nullptr;
  size_t w = 0, h = 0;
  PixelFormat fmt;
  EXPECT_TRUE(ReadImage(IMAGE_GIF, file_data_.data(), file_data_.size(),
                        &pixels, &fmt, &w, &h, nullptr, &handler_));
  EXPECT_NE(pixels, nullptr);
  EXPECT_GT(w, 0u);
  EXPECT_GT(h, 0u);
  free(pixels);
}

// ReadImage with corrupt GIF data should fail gracefully.
TEST_F(ReadImageTest, ReadImageGifCorruptData) {
  const char garbage[] = "GIF89a\x01\x00\x01\x00\x80\x00\x00";
  EXPECT_FALSE(ReadImage(IMAGE_GIF, garbage, sizeof(garbage), nullptr, nullptr,
                         nullptr, nullptr, nullptr, &handler_));
}

// CreateScanlineReader for GIF, then read all scanlines.
TEST_F(ReadImageTest, CreateScanlineReaderGifReadAllScanlines) {
  ASSERT_TRUE(LoadTestFileExt(kGifTestDir, "transparent.gif"));
  ScanlineStatus status;
  std::unique_ptr<ScanlineReaderInterface> reader(CreateScanlineReader(
      IMAGE_GIF, file_data_.data(), file_data_.size(), &handler_, &status));
  ASSERT_NE(reader, nullptr);
  EXPECT_TRUE(status.Success());

  int rows = 0;
  while (reader->HasMoreScanLines()) {
    void* scanline = nullptr;
    ASSERT_TRUE(reader->ReadNextScanline(&scanline));
    ASSERT_NE(scanline, nullptr);
    ++rows;
  }
  EXPECT_EQ(rows, static_cast<int>(reader->GetImageHeight()));
}

// CreateScanlineWriter for JPEG and perform a full write round-trip.
// This exercises lines 128-147 of read_image.cc (JPEG writer creation
// with setjmp/jmp_buf setup and JpegScanlineWriter allocation).
TEST_F(ReadImageTest, CreateScanlineWriterJpegRoundTrip) {
  std::string output;
  ScanlineStatus status;
  JpegCompressionOptions jpeg_config;
  jpeg_config.lossy = true;
  jpeg_config.lossy_options.quality = 85;
  std::unique_ptr<ScanlineWriterInterface> writer(CreateScanlineWriter(
      IMAGE_JPEG, RGB_888, 4, 4, &jpeg_config, &output, &handler_, &status));
  ASSERT_NE(writer, nullptr);
  EXPECT_TRUE(status.Success());

  // Write 4 rows of 4 RGB pixels each.
  uint8_t row[12] = {255, 0, 0, 0, 255, 0, 0, 0, 255, 128, 128, 128};
  for (int y = 0; y < 4; ++y) {
    ASSERT_TRUE(writer->WriteNextScanline(reinterpret_cast<void*>(row)));
  }
  ASSERT_TRUE(writer->FinalizeWrite());
  EXPECT_FALSE(output.empty());

  // Verify we can read it back as a JPEG.
  ScanlineStatus read_status;
  std::unique_ptr<ScanlineReaderInterface> reader(CreateScanlineReader(
      IMAGE_JPEG, output.data(), output.size(), &handler_, &read_status));
  ASSERT_NE(reader, nullptr);
  EXPECT_EQ(reader->GetImageWidth(), 4u);
  EXPECT_EQ(reader->GetImageHeight(), 4u);
}

// Exercise CreateImageFrameWriter with GIF (unsupported for writing).
// This exercises InstantiateImageFrameWriter with IMAGE_GIF, which falls
// through to InstantiateScanlineWriter -> IMAGE_GIF -> nullptr.
TEST_F(ReadImageTest, CreateImageFrameWriterGifUnsupported) {
  std::string output;
  ScanlineStatus status;
  std::unique_ptr<MultipleFrameWriter> writer(
      CreateImageFrameWriter(IMAGE_GIF, nullptr, &output, &handler_, &status));
  EXPECT_EQ(writer, nullptr);
  EXPECT_FALSE(status.Success());
}

// Exercise CreateImageFrameWriter with IMAGE_UNKNOWN (unsupported).
TEST_F(ReadImageTest, CreateImageFrameWriterUnknown) {
  std::string output;
  ScanlineStatus status;
  std::unique_ptr<MultipleFrameWriter> writer(CreateImageFrameWriter(
      IMAGE_UNKNOWN, nullptr, &output, &handler_, &status));
  EXPECT_EQ(writer, nullptr);
  EXPECT_FALSE(status.Success());
}

// Exercise CreateImageFrameReader with GIF, then read frames.
// This exercises the GIF-specific path in InstantiateImageFrameReader
// (lines 207-213 of read_image.cc).
TEST_F(ReadImageTest, CreateImageFrameReaderGifReadFrames) {
  ASSERT_TRUE(LoadTestFileExt(kGifTestDir, "transparent.gif"));
  ScanlineStatus status;
  std::unique_ptr<MultipleFrameReader> reader(CreateImageFrameReader(
      IMAGE_GIF, file_data_.data(), file_data_.size(), &handler_, &status));
  ASSERT_NE(reader, nullptr);
  EXPECT_TRUE(status.Success());

  // Should be able to get image spec.
  ImageSpec image_spec;
  ASSERT_TRUE(reader->GetImageSpec(&image_spec, &status));
  EXPECT_GT(image_spec.width, 0u);
  EXPECT_GT(image_spec.height, 0u);
}

// Exercise CreateImageFrameReader with non-default quirks mode.
TEST_F(ReadImageTest, CreateImageFrameReaderGifQuirksNone) {
  ASSERT_TRUE(LoadTestFileExt(kGifTestDir, "transparent.gif"));
  ScanlineStatus status;
  std::unique_ptr<MultipleFrameReader> reader(
      CreateImageFrameReader(IMAGE_GIF, file_data_.data(), file_data_.size(),
                             QUIRKS_NONE, &handler_, &status));
  ASSERT_NE(reader, nullptr);
  EXPECT_TRUE(status.Success());
}

// Exercise CreateScanlineWriter for WebP with full write round-trip.
// This exercises lines 154-158 of read_image.cc (WebP writer creation
// via FrameToScanlineWriterAdapter + InstantiateImageFrameWriter).
TEST_F(ReadImageTest, CreateScanlineWriterWebpRoundTrip) {
  std::string output;
  ScanlineStatus status;
  WebpConfiguration webp_config;
  std::unique_ptr<ScanlineWriterInterface> writer(CreateScanlineWriter(
      IMAGE_WEBP, RGB_888, 4, 4, &webp_config, &output, &handler_, &status));
  ASSERT_NE(writer, nullptr);
  EXPECT_TRUE(status.Success());

  uint8_t row[12] = {255, 0, 0, 0, 255, 0, 0, 0, 255, 128, 128, 128};
  for (int y = 0; y < 4; ++y) {
    ASSERT_TRUE(writer->WriteNextScanline(reinterpret_cast<void*>(row)));
  }
  ASSERT_TRUE(writer->FinalizeWrite());
  EXPECT_FALSE(output.empty());
}

// Exercise ReadImage with JPEG, including pixel decoding.
TEST_F(ReadImageTest, ReadImageJpegWithPixels) {
  ASSERT_TRUE(LoadTestFile(kJpegTestDir, "test420", "jpg"));
  void* pixels = nullptr;
  size_t w = 0, h = 0, stride = 0;
  PixelFormat fmt;
  EXPECT_TRUE(ReadImage(IMAGE_JPEG, file_data_.data(), file_data_.size(),
                        &pixels, &fmt, &w, &h, &stride, &handler_));
  EXPECT_NE(pixels, nullptr);
  EXPECT_GT(w, 0u);
  EXPECT_GT(h, 0u);
  EXPECT_GT(stride, 0u);
  EXPECT_EQ(stride % 4, 0u);
  free(pixels);
}

// Exercise ReadImage with WebP, including pixel decoding.
TEST_F(ReadImageTest, ReadImageWebpWithPixels) {
  ASSERT_TRUE(LoadTestFileExt("", "opaque_32x20.webp"));
  void* pixels = nullptr;
  size_t w = 0, h = 0, stride = 0;
  PixelFormat fmt;
  EXPECT_TRUE(ReadImage(IMAGE_WEBP, file_data_.data(), file_data_.size(),
                        &pixels, &fmt, &w, &h, &stride, &handler_));
  EXPECT_NE(pixels, nullptr);
  EXPECT_EQ(w, 32u);
  EXPECT_EQ(h, 20u);
  EXPECT_GT(stride, 0u);
  EXPECT_EQ(stride % 4, 0u);
  free(pixels);
}

// Exercise ReadImage with null optional output params.
TEST_F(ReadImageTest, ReadImagePngNullOptionalParams) {
  ASSERT_TRUE(LoadTestFileExt("", "opaque_32x20.png"));
  // Pass nullptr for pixel_format, width, height, stride.
  EXPECT_TRUE(ReadImage(IMAGE_PNG, file_data_.data(), file_data_.size(),
                        nullptr, nullptr, nullptr, nullptr, nullptr,
                        &handler_));
}

// ------------------------------------------------------------------
// Additional coverage: truncated image data, frame reader/writer
// edge cases
// ------------------------------------------------------------------

// ReadImage with a truncated PNG: initialization succeeds but scanline
// reading fails mid-way.  This exercises the ReadNextScanline failure
// path (lines 340-342 of read_image.cc) where the pixel data is freed
// on error.
TEST_F(ReadImageTest, ReadImageTruncatedPngScanlineFailure) {
  ASSERT_TRUE(LoadTestFileExt("", "opaque_32x20.png"));
  // Truncate the file data to about 60% -- enough to read the header
  // but not all scanlines.
  size_t truncated_size = file_data_.size() * 6 / 10;
  ASSERT_GT(truncated_size, 50u);
  void* pixels = nullptr;
  size_t w = 0, h = 0;
  PixelFormat fmt;
  // This may fail at init or during scanline read -- either way we
  // verify no crash and no memory leak.
  bool ok = ReadImage(IMAGE_PNG, file_data_.data(), truncated_size, &pixels,
                      &fmt, &w, &h, nullptr, &handler_);
  if (!ok) {
    EXPECT_EQ(pixels, nullptr);
  } else {
    // If it somehow succeeded, free the pixels.
    free(pixels);
  }
}

// ReadImage with a truncated JPEG: exercises JPEG scanline read failure.
TEST_F(ReadImageTest, ReadImageTruncatedJpegScanlineFailure) {
  ASSERT_TRUE(LoadTestFile(kJpegTestDir, "test420", "jpg"));
  size_t truncated_size = file_data_.size() / 2;
  ASSERT_GT(truncated_size, 50u);
  void* pixels = nullptr;
  size_t w = 0, h = 0;
  PixelFormat fmt;
  bool ok = ReadImage(IMAGE_JPEG, file_data_.data(), truncated_size, &pixels,
                      &fmt, &w, &h, nullptr, &handler_);
  if (!ok) {
    EXPECT_EQ(pixels, nullptr);
  } else {
    free(pixels);
  }
}

// ReadImage with a truncated WebP: exercises WebP scanline read failure.
TEST_F(ReadImageTest, ReadImageTruncatedWebpScanlineFailure) {
  ASSERT_TRUE(LoadTestFileExt("", "opaque_32x20.webp"));
  size_t truncated_size = file_data_.size() / 2;
  ASSERT_GT(truncated_size, 20u);
  void* pixels = nullptr;
  size_t w = 0, h = 0;
  PixelFormat fmt;
  bool ok = ReadImage(IMAGE_WEBP, file_data_.data(), truncated_size, &pixels,
                      &fmt, &w, &h, nullptr, &handler_);
  if (!ok) {
    EXPECT_EQ(pixels, nullptr);
  } else {
    free(pixels);
  }
}

// ReadImage with a truncated GIF: exercises GIF scanline read failure.
TEST_F(ReadImageTest, ReadImageTruncatedGifScanlineFailure) {
  ASSERT_TRUE(LoadTestFileExt(kGifTestDir, "transparent.gif"));
  size_t truncated_size = file_data_.size() * 7 / 10;
  ASSERT_GT(truncated_size, 20u);
  void* pixels = nullptr;
  size_t w = 0, h = 0;
  PixelFormat fmt;
  bool ok = ReadImage(IMAGE_GIF, file_data_.data(), truncated_size, &pixels,
                      &fmt, &w, &h, nullptr, &handler_);
  if (!ok) {
    EXPECT_EQ(pixels, nullptr);
  } else {
    free(pixels);
  }
}

// Exercise CreateScanlineReader with corrupt JPEG data: valid enough JPEG
// signature for initialization to potentially succeed but corrupt body.
TEST_F(ReadImageTest, CreateScanlineReaderJpegCorruptData) {
  // JPEG magic bytes followed by garbage.
  std::string corrupt;
  corrupt.push_back(static_cast<char>(0xFF));
  corrupt.push_back(static_cast<char>(0xD8));
  corrupt.push_back(static_cast<char>(0xFF));
  corrupt.push_back(static_cast<char>(0xE0));
  corrupt.append(100, 'X');
  ScanlineStatus status;
  std::unique_ptr<ScanlineReaderInterface> reader(CreateScanlineReader(
      IMAGE_JPEG, corrupt.data(), corrupt.size(), &handler_, &status));
  // May or may not succeed, but should not crash.
  if (reader == nullptr) {
    EXPECT_FALSE(status.Success());
  }
}

// Exercise CreateScanlineReader with corrupt WebP data.
TEST_F(ReadImageTest, CreateScanlineReaderWebpCorruptData) {
  const char garbage[] = "RIFF\x00\x00\x00\x00WEBPGARBAGE";
  ScanlineStatus status;
  std::unique_ptr<ScanlineReaderInterface> reader(CreateScanlineReader(
      IMAGE_WEBP, garbage, sizeof(garbage), &handler_, &status));
  EXPECT_EQ(reader, nullptr);
  EXPECT_FALSE(status.Success());
}

// Exercise InstantiateImageFrameWriter with IMAGE_PNG (non-WebP path),
// which wraps an InstantiateScanlineWriter.  The PNG writer is wrapped
// in ScanlineToFrameWriterAdapter.
TEST_F(ReadImageTest, CreateImageFrameWriterPngWrapped) {
  std::string output;
  ScanlineStatus status;
  PngCompressParams png_config(PNG_FILTER_NONE, Z_DEFAULT_STRATEGY, false);
  std::unique_ptr<MultipleFrameWriter> writer(CreateImageFrameWriter(
      IMAGE_PNG, &png_config, &output, &handler_, &status));
  ASSERT_NE(writer, nullptr);
  EXPECT_TRUE(status.Success());
}

// Exercise InstantiateImageFrameWriter with IMAGE_JPEG (non-WebP path).
TEST_F(ReadImageTest, CreateImageFrameWriterJpegWrapped) {
  std::string output;
  ScanlineStatus status;
  JpegCompressionOptions jpeg_config;
  jpeg_config.lossy = true;
  jpeg_config.lossy_options.quality = 85;
  std::unique_ptr<MultipleFrameWriter> writer(CreateImageFrameWriter(
      IMAGE_JPEG, &jpeg_config, &output, &handler_, &status));
  // JPEG frame writer goes through ScanlineToFrameWriterAdapter.
  ASSERT_NE(writer, nullptr);
  EXPECT_TRUE(status.Success());
}

// ------------------------------------------------------------------
// Additional coverage: inline overloads without explicit status,
// ImageFrame reader/writer with non-GIF/non-WebP formats, and
// ReadImage error paths
// ------------------------------------------------------------------

// Exercise the inline CreateScanlineReader overload (no ScanlineStatus*).
TEST_F(ReadImageTest, CreateScanlineReaderInlineOverload) {
  ASSERT_TRUE(LoadTestFileExt("", "opaque_32x20.png"));
  std::unique_ptr<ScanlineReaderInterface> reader(CreateScanlineReader(
      IMAGE_PNG, file_data_.data(), file_data_.size(), &handler_));
  ASSERT_NE(reader, nullptr);
  EXPECT_EQ(reader->GetImageWidth(), 32u);
  EXPECT_EQ(reader->GetImageHeight(), 20u);
}

// Exercise the inline CreateScanlineReader overload with unknown format.
TEST_F(ReadImageTest, CreateScanlineReaderInlineOverloadUnknown) {
  std::unique_ptr<ScanlineReaderInterface> reader(
      CreateScanlineReader(IMAGE_UNKNOWN, "x", 1, &handler_));
  EXPECT_EQ(reader, nullptr);
}

// Exercise the inline CreateScanlineWriter overload (no ScanlineStatus*).
TEST_F(ReadImageTest, CreateScanlineWriterInlineOverload) {
  std::string output;
  PngCompressParams png_config(PNG_FILTER_NONE, Z_DEFAULT_STRATEGY, false);
  std::unique_ptr<ScanlineWriterInterface> writer(CreateScanlineWriter(
      IMAGE_PNG, RGB_888, 2, 2, &png_config, &output, &handler_));
  ASSERT_NE(writer, nullptr);
}

// Exercise InstantiateImageFrameReader with IMAGE_PNG -- creates a
// ScanlineToFrameReaderAdapter wrapping a PngScanlineReaderRaw.
// Then verify it can read an actual image.
TEST_F(ReadImageTest, CreateImageFrameReaderPngReadFrames) {
  ASSERT_TRUE(LoadTestFileExt("", "opaque_32x20.png"));
  ScanlineStatus status;
  std::unique_ptr<MultipleFrameReader> reader(CreateImageFrameReader(
      IMAGE_PNG, file_data_.data(), file_data_.size(), &handler_, &status));
  ASSERT_NE(reader, nullptr);
  EXPECT_TRUE(status.Success());

  ImageSpec image_spec;
  ASSERT_TRUE(reader->GetImageSpec(&image_spec, &status));
  EXPECT_EQ(image_spec.width, 32u);
  EXPECT_EQ(image_spec.height, 20u);
}

// Exercise InstantiateImageFrameReader with IMAGE_JPEG.
TEST_F(ReadImageTest, CreateImageFrameReaderJpegReadFrames) {
  ASSERT_TRUE(LoadTestFile(kJpegTestDir, "test420", "jpg"));
  ScanlineStatus status;
  std::unique_ptr<MultipleFrameReader> reader(CreateImageFrameReader(
      IMAGE_JPEG, file_data_.data(), file_data_.size(), &handler_, &status));
  ASSERT_NE(reader, nullptr);
  EXPECT_TRUE(status.Success());

  ImageSpec image_spec;
  ASSERT_TRUE(reader->GetImageSpec(&image_spec, &status));
  EXPECT_GT(image_spec.width, 0u);
  EXPECT_GT(image_spec.height, 0u);
}

// Exercise ReadImage with GIF and all output params including stride.
TEST_F(ReadImageTest, ReadImageGifAllParams) {
  ASSERT_TRUE(LoadTestFileExt(kGifTestDir, "interlaced.gif"));
  void* pixels = nullptr;
  size_t w = 0, h = 0, stride = 0;
  PixelFormat fmt;
  EXPECT_TRUE(ReadImage(IMAGE_GIF, file_data_.data(), file_data_.size(),
                        &pixels, &fmt, &w, &h, &stride, &handler_));
  EXPECT_NE(pixels, nullptr);
  EXPECT_GT(w, 0u);
  EXPECT_GT(h, 0u);
  EXPECT_GT(stride, 0u);
  EXPECT_EQ(stride % 4, 0u);
  free(pixels);
}

// Exercise ReadImage with a WebP image and null handler.
TEST_F(ReadImageTest, ReadImageWebpNullHandler) {
  ASSERT_TRUE(LoadTestFileExt("", "opaque_32x20.webp"));
  size_t w = 0, h = 0;
  EXPECT_TRUE(ReadImage(IMAGE_WEBP, file_data_.data(), file_data_.size(),
                        nullptr, nullptr, &w, &h, nullptr, nullptr));
  EXPECT_EQ(w, 32u);
  EXPECT_EQ(h, 20u);
}

// Exercise CreateImageFrameWriter with IMAGE_WEBP explicitly, using the
// full Initialize path.
TEST_F(ReadImageTest, CreateImageFrameWriterWebpFull) {
  std::string output;
  ScanlineStatus status;
  WebpConfiguration webp_config;
  std::unique_ptr<MultipleFrameWriter> writer(CreateImageFrameWriter(
      IMAGE_WEBP, &webp_config, &output, &handler_, &status));
  ASSERT_NE(writer, nullptr);
  EXPECT_TRUE(status.Success());
}

// ------------------------------------------------------------------
// Additional error path coverage tests
// ------------------------------------------------------------------

// Exercise CreateImageFrameReader with QUIRKS_FIREFOX mode for GIF.
// This exercises the GIF frame reader with a different quirks mode.
TEST_F(ReadImageTest, CreateImageFrameReaderGifQuirksFirefox) {
  ASSERT_TRUE(LoadTestFileExt(kGifTestDir, "transparent.gif"));
  ScanlineStatus status;
  std::unique_ptr<MultipleFrameReader> reader(
      CreateImageFrameReader(IMAGE_GIF, file_data_.data(), file_data_.size(),
                             QUIRKS_FIREFOX, &handler_, &status));
  ASSERT_NE(reader, nullptr);
  EXPECT_TRUE(status.Success());

  ImageSpec image_spec;
  ASSERT_TRUE(reader->GetImageSpec(&image_spec, &status));
  EXPECT_GT(image_spec.width, 0u);
  EXPECT_GT(image_spec.height, 0u);
}

// Exercise CreateScanlineReader for GIF with minimal corrupt GIF data.
// The GIF has a valid header but truncated color table, which should fail
// during initialization within the GIF frame reader.
TEST_F(ReadImageTest, CreateScanlineReaderGifCorruptColorTable) {
  // Valid GIF89a header: 6 bytes signature, 4 bytes logical screen descriptor,
  // then a global color table flag is set but the table is truncated.
  const uint8_t corrupt_gif[] = {
      'G', 'I', 'F', '8', '9', 'a',  // GIF89a signature
      0x01, 0x00,                    // Width: 1
      0x01, 0x00,                    // Height: 1
      0x80,                          // GCT flag set, 2 colors
      0x00,                          // Background color index
      0x00,                          // Pixel aspect ratio
      // GCT should have 6 bytes (2 colors * 3 bytes), but we only provide 2.
      0xFF, 0x00};
  ScanlineStatus status;
  std::unique_ptr<ScanlineReaderInterface> reader(CreateScanlineReader(
      IMAGE_GIF, corrupt_gif, sizeof(corrupt_gif), &handler_, &status));
  // Should fail due to truncated GIF data.
  EXPECT_EQ(reader, nullptr);
  EXPECT_FALSE(status.Success());
}

// Exercise CreateScanlineReader for GIF with completely invalid magic bytes.
// The GIF reader should fail to open/parse this.
TEST_F(ReadImageTest, CreateScanlineReaderGifInvalidMagic) {
  const char not_gif[] = "NOT_A_GIF_FILE_AT_ALL";
  ScanlineStatus status;
  std::unique_ptr<ScanlineReaderInterface> reader(CreateScanlineReader(
      IMAGE_GIF, not_gif, sizeof(not_gif), &handler_, &status));
  EXPECT_EQ(reader, nullptr);
  EXPECT_FALSE(status.Success());
}

// Exercise CreateScanlineWriter for JPEG with GRAY_8 pixel format.
// JPEG supports grayscale natively; this exercises a different pixel format
// path in the JPEG writer.
TEST_F(ReadImageTest, CreateScanlineWriterJpegGrayscale) {
  std::string output;
  ScanlineStatus status;
  JpegCompressionOptions jpeg_config;
  jpeg_config.lossy = true;
  jpeg_config.lossy_options.quality = 75;
  std::unique_ptr<ScanlineWriterInterface> writer(CreateScanlineWriter(
      IMAGE_JPEG, GRAY_8, 4, 4, &jpeg_config, &output, &handler_, &status));
  ASSERT_NE(writer, nullptr);
  EXPECT_TRUE(status.Success());

  // Write 4 rows of 4 grayscale pixels.
  uint8_t row[4] = {128, 64, 192, 255};
  for (int y = 0; y < 4; ++y) {
    ASSERT_TRUE(writer->WriteNextScanline(reinterpret_cast<void*>(row)));
  }
  ASSERT_TRUE(writer->FinalizeWrite());
  EXPECT_FALSE(output.empty());
}

// Exercise CreateScanlineWriter for PNG with RGBA pixel format.
TEST_F(ReadImageTest, CreateScanlineWriterPngRgba) {
  std::string output;
  ScanlineStatus status;
  PngCompressParams png_config(PNG_FILTER_NONE, Z_DEFAULT_STRATEGY, false);
  std::unique_ptr<ScanlineWriterInterface> writer(CreateScanlineWriter(
      IMAGE_PNG, RGBA_8888, 2, 2, &png_config, &output, &handler_, &status));
  ASSERT_NE(writer, nullptr);
  EXPECT_TRUE(status.Success());

  // Write 2 rows of 2 RGBA pixels.
  uint8_t row[8] = {255, 0, 0, 255, 0, 255, 0, 128};
  for (int y = 0; y < 2; ++y) {
    ASSERT_TRUE(writer->WriteNextScanline(reinterpret_cast<void*>(row)));
  }
  ASSERT_TRUE(writer->FinalizeWrite());
  EXPECT_FALSE(output.empty());
}

// Exercise CreateScanlineWriter for WebP with RGBA pixel format.
TEST_F(ReadImageTest, CreateScanlineWriterWebpRgba) {
  std::string output;
  ScanlineStatus status;
  WebpConfiguration webp_config;
  std::unique_ptr<ScanlineWriterInterface> writer(CreateScanlineWriter(
      IMAGE_WEBP, RGBA_8888, 4, 4, &webp_config, &output, &handler_, &status));
  ASSERT_NE(writer, nullptr);
  EXPECT_TRUE(status.Success());

  // Write 4 rows of 4 RGBA pixels.
  uint8_t row[16] = {255, 0, 0,   255, 0,   255, 0,   255,
                     0,   0, 255, 255, 128, 128, 128, 128};
  for (int y = 0; y < 4; ++y) {
    ASSERT_TRUE(writer->WriteNextScanline(reinterpret_cast<void*>(row)));
  }
  ASSERT_TRUE(writer->FinalizeWrite());
  EXPECT_FALSE(output.empty());
}

// Exercise ReadImage with GIF using corrupt data that has a valid header
// but corrupt image data, forcing a scanline read failure.
TEST_F(ReadImageTest, ReadImageGifCorruptImageData) {
  // Minimal valid GIF89a header with global color table, but corrupt
  // image data following it.
  const uint8_t corrupt_gif[] = {
      'G',  'I',  'F',  '8',  '9', 'a',  // GIF89a
      0x02, 0x00,                        // Width: 2
      0x02, 0x00,                        // Height: 2
      0x80,                              // GCT flag, 2 colors
      0x00,                              // BG color
      0x00,                              // Aspect ratio
      0x00, 0x00, 0x00,                  // Color 0: black
      0xFF, 0xFF, 0xFF,                  // Color 1: white
      0x2C,                              // Image descriptor
      0x00, 0x00, 0x00, 0x00,            // Left, Top
      0x02, 0x00, 0x02, 0x00,            // Width, Height
      0x00,                              // Packed byte (no LCT)
      0x02,                              // LZW minimum code size
      0xFF,                              // Corrupt: invalid sub-block size
      0x00,                              // Block terminator
      0x3B                               // GIF trailer
  };
  void* pixels = nullptr;
  size_t w = 0, h = 0;
  PixelFormat fmt;
  // This should fail during scanline reading due to corrupt LZW data.
  bool ok = ReadImage(IMAGE_GIF, corrupt_gif, sizeof(corrupt_gif), &pixels,
                      &fmt, &w, &h, nullptr, &handler_);
  if (!ok) {
    EXPECT_EQ(pixels, nullptr);
  } else {
    free(pixels);
  }
}

// Exercise CreateImageFrameReader for WebP with corrupt data.
// The WebP frame reader wraps a ScanlineToFrameReaderAdapter around
// a WebpScanlineReader, exercising lines 219-228 of read_image.cc.
TEST_F(ReadImageTest, CreateImageFrameReaderWebpCorruptData) {
  const char garbage[] = "RIFF\x00\x00\x00\x00WEBPGARBAGE_DATA";
  ScanlineStatus status;
  std::unique_ptr<MultipleFrameReader> reader(CreateImageFrameReader(
      IMAGE_WEBP, garbage, sizeof(garbage), &handler_, &status));
  // Should fail during initialization.
  EXPECT_EQ(reader, nullptr);
  EXPECT_FALSE(status.Success());
}

// Exercise CreateImageFrameReader for PNG with corrupt data.
// This exercises the ScanlineToFrameReaderAdapter path (lines 219-228)
// when the underlying scanline reader fails to initialize.
TEST_F(ReadImageTest, CreateImageFrameReaderPngCorruptData) {
  const char garbage[] = "not a png at all";
  ScanlineStatus status;
  std::unique_ptr<MultipleFrameReader> reader(CreateImageFrameReader(
      IMAGE_PNG, garbage, sizeof(garbage), &handler_, &status));
  EXPECT_EQ(reader, nullptr);
  EXPECT_FALSE(status.Success());
}

// Exercise CreateImageFrameReader for JPEG with corrupt data.
TEST_F(ReadImageTest, CreateImageFrameReaderJpegCorruptData) {
  const char garbage[] = "not a jpeg";
  ScanlineStatus status;
  std::unique_ptr<MultipleFrameReader> reader(CreateImageFrameReader(
      IMAGE_JPEG, garbage, sizeof(garbage), &handler_, &status));
  EXPECT_EQ(reader, nullptr);
  EXPECT_FALSE(status.Success());
}

// Exercise CreateImageFrameReader for GIF with corrupt data.
// This goes through the GIF-native path (lines 207-213).
TEST_F(ReadImageTest, CreateImageFrameReaderGifCorruptData) {
  const char garbage[] = "not a gif";
  ScanlineStatus status;
  std::unique_ptr<MultipleFrameReader> reader(CreateImageFrameReader(
      IMAGE_GIF, garbage, sizeof(garbage), &handler_, &status));
  EXPECT_EQ(reader, nullptr);
  EXPECT_FALSE(status.Success());
}

// Exercise CreateImageFrameWriter with WebP and invalid config.
// Tests the error path in WebpFrameWriter when Initialize fails.
TEST_F(ReadImageTest, CreateImageFrameWriterWebpNullConfig) {
  std::string output;
  ScanlineStatus status;
  // Passing nullptr config for WebP -- WebpFrameWriter::Initialize
  // should handle this gracefully.
  std::unique_ptr<MultipleFrameWriter> writer(
      CreateImageFrameWriter(IMAGE_WEBP, nullptr, &output, &handler_, &status));
  // WebP requires a valid config; this should fail.
  EXPECT_EQ(writer, nullptr);
  EXPECT_FALSE(status.Success());
}

// Note: JPEG and PNG writers crash (longjmp/segfault) with zero dimensions
// due to libjpeg/libpng internal assertions. These are not tested here.

// Exercise ReadImage with an empty buffer for PNG.
TEST_F(ReadImageTest, ReadImageEmptyBuffer) {
  EXPECT_FALSE(ReadImage(IMAGE_PNG, "", 0, nullptr, nullptr, nullptr, nullptr,
                         nullptr, &handler_));
}

// Exercise ReadImage with a GIF that has a valid header but empty image data.
TEST_F(ReadImageTest, ReadImageGifEmptyImageData) {
  // Valid GIF89a header with global color table but no image descriptor.
  const uint8_t gif_no_image[] = {
      'G',  'I',  'F',  '8', '9', 'a',  // GIF89a
      0x01, 0x00,                       // Width: 1
      0x01, 0x00,                       // Height: 1
      0x80,                             // GCT flag, 2 colors
      0x00,                             // BG color
      0x00,                             // Aspect ratio
      0x00, 0x00, 0x00,                 // Color 0: black
      0xFF, 0xFF, 0xFF,                 // Color 1: white
      0x3B                              // Trailer (no image data)
  };
  void* pixels = nullptr;
  size_t w = 0, h = 0;
  PixelFormat fmt;
  // This may fail because there's no image data to decode.
  bool ok = ReadImage(IMAGE_GIF, gif_no_image, sizeof(gif_no_image), &pixels,
                      &fmt, &w, &h, nullptr, &handler_);
  if (!ok) {
    EXPECT_EQ(pixels, nullptr);
  } else {
    free(pixels);
  }
}

// ------------------------------------------------------------------
// Undecodable input must fail closed, with a null handler (#1371)
// ------------------------------------------------------------------
//
// ReadImage's contract says a caller may pass a null MessageHandler and
// must get false back for input the decoders cannot handle. Each shape
// below comes from the crash report in #1371; every one of them crashed
// the process on the error-reporting path instead of returning false.

// Builds a binary PGM (P5) 130x97 -- the form djpeg -ppm writes for a
// grayscale source.
static std::string MakeP5Pgm() {
  std::string pgm = "P5\n130 97\n255\n";
  for (int i = 0; i < 130 * 97; ++i) {
    pgm.push_back(static_cast<char>(i & 0xff));
  }
  return pgm;
}

// Builds a P4 bitmap: 16x16, one bit per pixel, 2 bytes per row.
static std::string MakeP4Pbm() {
  std::string pbm = "P4\n16 16\n";
  for (int i = 0; i < 2 * 16; ++i) {
    pbm.push_back(static_cast<char>(0xA5 ^ i));
  }
  return pbm;
}

// 200 pseudo-random but DETERMINISTIC bytes: fixed-seed LCG, so a failure
// is always reproducible (never read /dev/urandom in a test).
static std::string MakeDeterministicGarbage() {
  std::string bytes;
  uint32_t state = 0x12345678u;
  for (int i = 0; i < 200; ++i) {
    state = state * 1664525u + 1013904223u;
    bytes.push_back(static_cast<char>((state >> 16) & 0xff));
  }
  return bytes;
}

// Calls ReadImage exactly as the #1371 driver did -- all outputs
// requested, null handler -- and requires it to fail closed: false, no
// pixel buffer handed back, no crash.
void ExpectReadImageFailsClosed(ImageFormat image_type,
                                const std::string& contents) {
  void* pixels = nullptr;
  size_t w = 0, h = 0, stride = 0;
  PixelFormat fmt;
  EXPECT_FALSE(ReadImage(image_type, contents.data(), contents.size(), &pixels,
                         &fmt, &w, &h, &stride, nullptr /* handler */))
      << "undecodable input must be reported as false";
  EXPECT_EQ(pixels, nullptr);
}

TEST_F(ReadImageTest, ReadImageUndecodableP5AsPng) {
  ExpectReadImageFailsClosed(IMAGE_PNG, MakeP5Pgm());
}

TEST_F(ReadImageTest, ReadImageUndecodableP5AsJpeg) {
  ExpectReadImageFailsClosed(IMAGE_JPEG, MakeP5Pgm());
}

TEST_F(ReadImageTest, ReadImageUndecodableP4AsPng) {
  ExpectReadImageFailsClosed(IMAGE_PNG, MakeP4Pbm());
}

TEST_F(ReadImageTest, ReadImageUndecodableP4AsJpeg) {
  ExpectReadImageFailsClosed(IMAGE_JPEG, MakeP4Pbm());
}

TEST_F(ReadImageTest, ReadImageUndecodableRandomBytesAsPng) {
  ExpectReadImageFailsClosed(IMAGE_PNG, MakeDeterministicGarbage());
}

TEST_F(ReadImageTest, ReadImageUndecodableRandomBytesAsJpeg) {
  ExpectReadImageFailsClosed(IMAGE_JPEG, MakeDeterministicGarbage());
}

// A valid PNG truncated to its first 40 bytes: the signature is intact and
// IHDR parses, so caller-side magic sniffing cannot avoid this input.
// 40 = 8 (signature) + 25 (IHDR chunk: 4 len + 4 name + 13 data + 4 CRC) +
// 7 bytes of the next chunk header -- too few for libpng to read it.
TEST_F(ReadImageTest, ReadImageTruncatedPng40BytesAsPng) {
  ASSERT_TRUE(LoadTestFileExt("", "opaque_32x20.png"));
  ASSERT_GT(file_data_.size(), 40u);
  ExpectReadImageFailsClosed(IMAGE_PNG, file_data_.substr(0, 40));
}

// The same truncated PNG offered to the JPEG reader.
TEST_F(ReadImageTest, ReadImageTruncatedPng40BytesAsJpeg) {
  ASSERT_TRUE(LoadTestFileExt("", "opaque_32x20.png"));
  ASSERT_GT(file_data_.size(), 40u);
  ExpectReadImageFailsClosed(IMAGE_JPEG, file_data_.substr(0, 40));
}

// A PNG truncated so the header parses and initialization succeeds but the
// first scanline read hits end-of-buffer mid-row: the failure surfaces
// from ReadNextScanline, not from initialization. For this fixture the
// IDAT data starts at byte offset 41 (8 signature + 25 IHDR + 8 chunk
// header); keeping 16 data bytes lets decoding start and then starve.
TEST_F(ReadImageTest, ReadImageTruncatedPngMidScanlineAsPng) {
  ASSERT_TRUE(LoadTestFileExt("", "opaque_32x20.png"));
  const size_t kIdatDataOffset = 41;
  ASSERT_LT(kIdatDataOffset + 16, file_data_.size());
  ExpectReadImageFailsClosed(IMAGE_PNG,
                             file_data_.substr(0, kIdatDataOffset + 16));
}

}  // namespace
}  // namespace pagespeed::image_compression
