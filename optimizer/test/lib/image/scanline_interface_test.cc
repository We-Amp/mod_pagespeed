// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

// Unit tests for ScanlineStatus and scanline interface definitions.

#include "lib/image/scanline_interface.h"

#include <cstddef>
#include <string>

#include "gtest/gtest.h"
#include "lib/image/scanline_status.h"

namespace net_instaweb {
namespace {

// =============================================================================
// ScanlineStatus tests
// =============================================================================

TEST(ScanlineStatusTest, DefaultConstruction) {
  ScanlineStatus status;
  EXPECT_TRUE(status.Success());
  EXPECT_EQ(SCANLINE_STATUS_SUCCESS, status.type());
  EXPECT_EQ(SCANLINE_UNKNOWN, status.source());
  EXPECT_TRUE(status.details().empty());
}

TEST(ScanlineStatusTest, TypeOnlyConstruction) {
  ScanlineStatus status(SCANLINE_STATUS_PARSE_ERROR);
  EXPECT_FALSE(status.Success());
  EXPECT_EQ(SCANLINE_STATUS_PARSE_ERROR, status.type());
  EXPECT_EQ(SCANLINE_UNKNOWN, status.source());
  EXPECT_TRUE(status.details().empty());
}

TEST(ScanlineStatusTest, FullConstruction) {
  ScanlineStatus status(SCANLINE_STATUS_MEMORY_ERROR, SCANLINE_PNGREADER,
                        "allocation failed");
  EXPECT_FALSE(status.Success());
  EXPECT_EQ(SCANLINE_STATUS_MEMORY_ERROR, status.type());
  EXPECT_EQ(SCANLINE_PNGREADER, status.source());
  EXPECT_EQ("allocation failed", status.details());
}

TEST(ScanlineStatusTest, SuccessStatus) {
  ScanlineStatus status(SCANLINE_STATUS_SUCCESS, SCANLINE_JPEGREADER,
                        "completed");
  EXPECT_TRUE(status.Success());
}

TEST(ScanlineStatusTest, NewWithFormat) {
  ScanlineStatus status =
      ScanlineStatus::New(SCANLINE_STATUS_PARSE_ERROR, SCANLINE_GIFREADER,
                          "invalid header at offset %d", 42);
  EXPECT_FALSE(status.Success());
  EXPECT_EQ(SCANLINE_STATUS_PARSE_ERROR, status.type());
  EXPECT_EQ(SCANLINE_GIFREADER, status.source());
  EXPECT_EQ("invalid header at offset 42", status.details());
}

TEST(ScanlineStatusTest, NewWithMultipleFormatArgs) {
  ScanlineStatus status = ScanlineStatus::New(
      SCANLINE_STATUS_INTERNAL_ERROR, SCANLINE_WEBPREADER,
      "expected %d bytes, got %d at position %lu", 100, 50, 1024UL);
  EXPECT_EQ("expected 100 bytes, got 50 at position 1024", status.details());
}

TEST(ScanlineStatusTest, TypeStr) {
  ScanlineStatus status(SCANLINE_STATUS_UNSUPPORTED_FORMAT);
  EXPECT_STREQ("SCANLINE_STATUS_UNSUPPORTED_FORMAT", status.TypeStr());

  ScanlineStatus success_status(SCANLINE_STATUS_SUCCESS);
  EXPECT_STREQ("SCANLINE_STATUS_SUCCESS", success_status.TypeStr());

  ScanlineStatus timeout_status(SCANLINE_STATUS_TIMEOUT_ERROR);
  EXPECT_STREQ("SCANLINE_STATUS_TIMEOUT_ERROR", timeout_status.TypeStr());
}

TEST(ScanlineStatusTest, SourceStr) {
  ScanlineStatus png_status(SCANLINE_STATUS_SUCCESS, SCANLINE_PNGREADER, "");
  EXPECT_STREQ("SCANLINE_PNGREADER", png_status.SourceStr());

  ScanlineStatus jpeg_status(SCANLINE_STATUS_SUCCESS, SCANLINE_JPEGREADER, "");
  EXPECT_STREQ("SCANLINE_JPEGREADER", jpeg_status.SourceStr());

  ScanlineStatus webp_status(SCANLINE_STATUS_SUCCESS, SCANLINE_WEBPWRITER, "");
  EXPECT_STREQ("SCANLINE_WEBPWRITER", webp_status.SourceStr());
}

TEST(ScanlineStatusTest, ToString) {
  ScanlineStatus status(SCANLINE_STATUS_PARSE_ERROR, SCANLINE_GIFREADER,
                        "bad frame data");
  std::string str = status.ToString();
  EXPECT_NE(str.find("SCANLINE_GIFREADER"), std::string::npos);
  EXPECT_NE(str.find("SCANLINE_STATUS_PARSE_ERROR"), std::string::npos);
  EXPECT_NE(str.find("bad frame data"), std::string::npos);
}

TEST(ScanlineStatusTest, ComesFromReader) {
  // Reader sources
  EXPECT_TRUE(ScanlineStatus(SCANLINE_STATUS_SUCCESS, SCANLINE_PNGREADER, "")
                  .ComesFromReader());
  EXPECT_TRUE(ScanlineStatus(SCANLINE_STATUS_SUCCESS, SCANLINE_PNGREADERRAW, "")
                  .ComesFromReader());
  EXPECT_TRUE(ScanlineStatus(SCANLINE_STATUS_SUCCESS, SCANLINE_GIFREADER, "")
                  .ComesFromReader());
  EXPECT_TRUE(ScanlineStatus(SCANLINE_STATUS_SUCCESS, SCANLINE_GIFREADERRAW, "")
                  .ComesFromReader());
  EXPECT_TRUE(ScanlineStatus(SCANLINE_STATUS_SUCCESS, SCANLINE_JPEGREADER, "")
                  .ComesFromReader());
  EXPECT_TRUE(ScanlineStatus(SCANLINE_STATUS_SUCCESS, SCANLINE_WEBPREADER, "")
                  .ComesFromReader());
  EXPECT_TRUE(ScanlineStatus(SCANLINE_STATUS_SUCCESS,
                             FRAME_TO_SCANLINE_READER_ADAPTER, "")
                  .ComesFromReader());
  EXPECT_TRUE(ScanlineStatus(SCANLINE_STATUS_SUCCESS,
                             SCANLINE_TO_FRAME_READER_ADAPTER, "")
                  .ComesFromReader());
  EXPECT_TRUE(ScanlineStatus(SCANLINE_STATUS_SUCCESS, FRAME_GIFREADER, "")
                  .ComesFromReader());
  EXPECT_TRUE(ScanlineStatus(SCANLINE_STATUS_SUCCESS, FRAME_PADDING_READER, "")
                  .ComesFromReader());

  // Non-reader sources
  EXPECT_FALSE(ScanlineStatus(SCANLINE_STATUS_SUCCESS, SCANLINE_UNKNOWN, "")
                   .ComesFromReader());
  EXPECT_FALSE(ScanlineStatus(SCANLINE_STATUS_SUCCESS, SCANLINE_PNGWRITER, "")
                   .ComesFromReader());
  EXPECT_FALSE(ScanlineStatus(SCANLINE_STATUS_SUCCESS, SCANLINE_JPEGWRITER, "")
                   .ComesFromReader());
  EXPECT_FALSE(ScanlineStatus(SCANLINE_STATUS_SUCCESS, SCANLINE_WEBPWRITER, "")
                   .ComesFromReader());
  EXPECT_FALSE(ScanlineStatus(SCANLINE_STATUS_SUCCESS, SCANLINE_RESIZER, "")
                   .ComesFromReader());
  EXPECT_FALSE(ScanlineStatus(SCANLINE_STATUS_SUCCESS, SCANLINE_UTIL, "")
                   .ComesFromReader());
}

TEST(ScanlineStatusTest, CopyConstruction) {
  ScanlineStatus original(SCANLINE_STATUS_MEMORY_ERROR, SCANLINE_WEBPREADER,
                          "out of memory");
  const ScanlineStatus& copy(original);

  EXPECT_EQ(original.type(), copy.type());
  EXPECT_EQ(original.source(), copy.source());
  EXPECT_EQ(original.details(), copy.details());
  EXPECT_FALSE(copy.Success());
}

TEST(ScanlineStatusTest, Assignment) {
  ScanlineStatus original(SCANLINE_STATUS_TIMEOUT_ERROR, SCANLINE_JPEGWRITER,
                          "encode timeout");
  ScanlineStatus assigned;
  assigned = original;

  EXPECT_EQ(original.type(), assigned.type());
  EXPECT_EQ(original.source(), assigned.source());
  EXPECT_EQ(original.details(), assigned.details());
}

// =============================================================================
// ScanlineStatusType enum tests
// =============================================================================

TEST(ScanlineStatusTypeTest, AllStatusTypesExist) {
  // Verify all expected status types exist and have distinct values
  EXPECT_EQ(0, SCANLINE_STATUS_UNINITIALIZED);
  EXPECT_EQ(1, SCANLINE_STATUS_SUCCESS);
  EXPECT_EQ(2, SCANLINE_STATUS_UNSUPPORTED_FORMAT);
  EXPECT_EQ(3, SCANLINE_STATUS_UNSUPPORTED_FEATURE);
  EXPECT_EQ(4, SCANLINE_STATUS_PARSE_ERROR);
  EXPECT_EQ(5, SCANLINE_STATUS_MEMORY_ERROR);
  EXPECT_EQ(6, SCANLINE_STATUS_INTERNAL_ERROR);
  EXPECT_EQ(7, SCANLINE_STATUS_TIMEOUT_ERROR);
  EXPECT_EQ(8, SCANLINE_STATUS_INVOCATION_ERROR);
  EXPECT_EQ(9, NUM_SCANLINE_STATUS);
}

// =============================================================================
// ScanlineStatusSource enum tests
// =============================================================================

TEST(ScanlineStatusSourceTest, AllSourcesExist) {
  // Verify key sources exist (not exhaustive, just sampling)
  EXPECT_EQ(0, SCANLINE_UNKNOWN);
  EXPECT_LT(SCANLINE_PNGREADER, NUM_SCANLINE_SOURCE);
  EXPECT_LT(SCANLINE_JPEGREADER, NUM_SCANLINE_SOURCE);
  EXPECT_LT(SCANLINE_GIFREADER, NUM_SCANLINE_SOURCE);
  EXPECT_LT(SCANLINE_WEBPREADER, NUM_SCANLINE_SOURCE);
  EXPECT_LT(SCANLINE_PNGWRITER, NUM_SCANLINE_SOURCE);
  EXPECT_LT(SCANLINE_JPEGWRITER, NUM_SCANLINE_SOURCE);
  EXPECT_LT(SCANLINE_WEBPWRITER, NUM_SCANLINE_SOURCE);
}

// =============================================================================
// Mock implementations for testing interface contracts
// =============================================================================

// A minimal mock reader for testing the interface contract
class MockScanlineReader : public ScanlineReaderInterface {
 public:
  MockScanlineReader()

  {}

  bool Reset() override {
    current_row_ = 0;
    return true;
  }

  size_t GetBytesPerScanline() override {
    return width_ * GetBytesPerPixelForFormat();
  }

  bool HasMoreScanLines() override { return current_row_ < height_; }

  ScanlineStatus InitializeWithStatus(const void* /*image_buffer*/,
                                      size_t /*buffer_length*/) override {
    initialized_ = true;
    // Only set default dimensions if not already configured
    if (width_ == 0) width_ = 100;
    if (height_ == 0) height_ = 100;
    return ScanlineStatus(SCANLINE_STATUS_SUCCESS);
  }

  ScanlineStatus ReadNextScanlineWithStatus(
      void** out_scanline_bytes) override {
    if (!initialized_) {
      return ScanlineStatus(SCANLINE_STATUS_INVOCATION_ERROR, SCANLINE_UNKNOWN,
                            "not initialized");
    }
    if (!HasMoreScanLines()) {
      return ScanlineStatus(SCANLINE_STATUS_INVOCATION_ERROR, SCANLINE_UNKNOWN,
                            "no more scanlines");
    }
    // Return a pointer to our internal buffer
    *out_scanline_bytes = scanline_buffer_;
    ++current_row_;
    return ScanlineStatus(SCANLINE_STATUS_SUCCESS);
  }

  size_t GetImageHeight() override { return height_; }
  size_t GetImageWidth() override { return width_; }

  ::pagespeed::image_compression::PixelFormat GetPixelFormat() override {
    return pixel_format_;
  }

  bool IsProgressive() override { return progressive_; }

  // Test helpers
  void SetDimensions(size_t width, size_t height) {
    width_ = width;
    height_ = height;
  }

  void SetPixelFormat(::pagespeed::image_compression::PixelFormat format) {
    pixel_format_ = format;
  }

  void SetProgressive(bool progressive) { progressive_ = progressive; }

 private:
  [[nodiscard]] size_t GetBytesPerPixelForFormat() const {
    switch (pixel_format_) {
      case ::pagespeed::image_compression::GRAY_8:
        return 1;
      case ::pagespeed::image_compression::RGB_888:
        return 3;
      case ::pagespeed::image_compression::RGBA_8888:
        return 4;
      default:
        return 0;
    }
  }

  bool initialized_{false};
  size_t width_{0};
  size_t height_{0};
  size_t current_row_{0};
  ::pagespeed::image_compression::PixelFormat pixel_format_{
      ::pagespeed::image_compression::RGB_888};
  bool progressive_{false};
  uint8_t scanline_buffer_[1024];  // Simple fixed buffer for testing
};

// A minimal mock writer for testing the interface contract
class MockScanlineWriter : public ScanlineWriterInterface {
 public:
  MockScanlineWriter()

  {}

  ScanlineStatus InitWithStatus(
      size_t width, size_t height,
      ::pagespeed::image_compression::PixelFormat pixel_format) override {
    width_ = width;
    height_ = height;
    pixel_format_ = pixel_format;
    return ScanlineStatus(SCANLINE_STATUS_SUCCESS);
  }

  ScanlineStatus InitializeWriteWithStatus(const void* /*config*/,
                                           std::string* out) override {
    output_ = out;
    current_row_ = 0;
    finalized_ = false;
    return ScanlineStatus(SCANLINE_STATUS_SUCCESS);
  }

  ScanlineStatus WriteNextScanlineWithStatus(
      const void* /*scanline_bytes*/) override {
    if (output_ == nullptr) {
      return ScanlineStatus(SCANLINE_STATUS_INVOCATION_ERROR, SCANLINE_UNKNOWN,
                            "not initialized");
    }
    if (current_row_ >= height_) {
      return ScanlineStatus(SCANLINE_STATUS_INVOCATION_ERROR, SCANLINE_UNKNOWN,
                            "too many scanlines");
    }
    ++current_row_;
    return ScanlineStatus(SCANLINE_STATUS_SUCCESS);
  }

  ScanlineStatus FinalizeWriteWithStatus() override {
    finalized_ = true;
    return ScanlineStatus(SCANLINE_STATUS_SUCCESS);
  }

  // Test accessors
  [[nodiscard]] size_t width() const { return width_; }
  [[nodiscard]] size_t height() const { return height_; }
  [[nodiscard]] ::pagespeed::image_compression::PixelFormat pixel_format()
      const {
    return pixel_format_;
  }
  [[nodiscard]] size_t rows_written() const { return current_row_; }
  [[nodiscard]] bool finalized() const { return finalized_; }

 private:
  size_t width_{0};
  size_t height_{0};
  ::pagespeed::image_compression::PixelFormat pixel_format_{
      ::pagespeed::image_compression::UNSUPPORTED};
  size_t current_row_{0};
  std::string* output_{nullptr};
  bool finalized_{false};
};

// =============================================================================
// ScanlineReaderInterface tests
// =============================================================================

TEST(ScanlineReaderInterfaceTest, BasicReadFlow) {
  MockScanlineReader reader;

  // Initialize with dummy data
  const char dummy_data[] = "dummy image data";
  EXPECT_TRUE(reader.Initialize(dummy_data, sizeof(dummy_data)));

  // Check dimensions
  EXPECT_EQ(100u, reader.GetImageWidth());
  EXPECT_EQ(100u, reader.GetImageHeight());

  // Check pixel format
  EXPECT_EQ(::pagespeed::image_compression::RGB_888, reader.GetPixelFormat());

  // Check bytes per scanline (100 pixels * 3 bytes for RGB)
  EXPECT_EQ(300u, reader.GetBytesPerScanline());

  // Read scanlines
  void* scanline = nullptr;
  int rows_read = 0;
  while (reader.HasMoreScanLines()) {
    EXPECT_TRUE(reader.ReadNextScanline(&scanline));
    EXPECT_NE(nullptr, scanline);
    ++rows_read;
  }
  EXPECT_EQ(100, rows_read);

  // Reset and read again
  EXPECT_TRUE(reader.Reset());
  EXPECT_TRUE(reader.HasMoreScanLines());
}

TEST(ScanlineReaderInterfaceTest, InitializeWithStatus) {
  MockScanlineReader reader;

  const char dummy_data[] = "dummy image data";
  ScanlineStatus status =
      reader.InitializeWithStatus(dummy_data, sizeof(dummy_data));

  EXPECT_TRUE(status.Success());
  EXPECT_EQ(SCANLINE_STATUS_SUCCESS, status.type());
}

TEST(ScanlineReaderInterfaceTest, ReadWithStatus) {
  MockScanlineReader reader;

  const char dummy_data[] = "dummy";
  EXPECT_TRUE(reader.Initialize(dummy_data, sizeof(dummy_data)));

  // Set dimensions after initialize to override the defaults
  reader.SetDimensions(10, 5);

  void* scanline = nullptr;
  for (int i = 0; i < 5; ++i) {
    ScanlineStatus status = reader.ReadNextScanlineWithStatus(&scanline);
    EXPECT_TRUE(status.Success());
  }

  // Reading past end should fail
  ScanlineStatus status = reader.ReadNextScanlineWithStatus(&scanline);
  EXPECT_FALSE(status.Success());
  EXPECT_EQ(SCANLINE_STATUS_INVOCATION_ERROR, status.type());
}

TEST(ScanlineReaderInterfaceTest, PixelFormats) {
  MockScanlineReader reader;

  reader.SetPixelFormat(::pagespeed::image_compression::GRAY_8);
  EXPECT_EQ(::pagespeed::image_compression::GRAY_8, reader.GetPixelFormat());

  reader.SetPixelFormat(::pagespeed::image_compression::RGBA_8888);
  EXPECT_EQ(::pagespeed::image_compression::RGBA_8888, reader.GetPixelFormat());
}

TEST(ScanlineReaderInterfaceTest, Progressive) {
  MockScanlineReader reader;

  EXPECT_FALSE(reader.IsProgressive());
  reader.SetProgressive(true);
  EXPECT_TRUE(reader.IsProgressive());
}

// =============================================================================
// ScanlineWriterInterface tests
// =============================================================================

TEST(ScanlineWriterInterfaceTest, BasicWriteFlow) {
  MockScanlineWriter writer;

  // Initialize dimensions
  EXPECT_TRUE(writer.Init(200, 150, ::pagespeed::image_compression::RGBA_8888));
  EXPECT_EQ(200u, writer.width());
  EXPECT_EQ(150u, writer.height());
  EXPECT_EQ(::pagespeed::image_compression::RGBA_8888, writer.pixel_format());

  // Initialize write
  std::string output;
  EXPECT_TRUE(writer.InitializeWrite(nullptr, &output));

  // Write all scanlines
  uint8_t dummy_scanline[800] = {0};  // 200 pixels * 4 bytes
  for (size_t i = 0; i < 150; ++i) {
    EXPECT_TRUE(writer.WriteNextScanline(dummy_scanline));
  }
  EXPECT_EQ(150u, writer.rows_written());

  // Finalize
  EXPECT_FALSE(writer.finalized());
  EXPECT_TRUE(writer.FinalizeWrite());
  EXPECT_TRUE(writer.finalized());
}

TEST(ScanlineWriterInterfaceTest, InitWithStatus) {
  MockScanlineWriter writer;

  ScanlineStatus status =
      writer.InitWithStatus(100, 100, ::pagespeed::image_compression::RGB_888);
  EXPECT_TRUE(status.Success());
  EXPECT_EQ(::pagespeed::image_compression::RGB_888, writer.pixel_format());
}

TEST(ScanlineWriterInterfaceTest, WriteWithStatus) {
  MockScanlineWriter writer;

  writer.Init(50, 2, ::pagespeed::image_compression::GRAY_8);
  std::string output;
  writer.InitializeWrite(nullptr, &output);

  uint8_t dummy_scanline[50] = {0};

  // Write two scanlines
  ScanlineStatus status = writer.WriteNextScanlineWithStatus(dummy_scanline);
  EXPECT_TRUE(status.Success());

  status = writer.WriteNextScanlineWithStatus(dummy_scanline);
  EXPECT_TRUE(status.Success());

  // Third write should fail
  status = writer.WriteNextScanlineWithStatus(dummy_scanline);
  EXPECT_FALSE(status.Success());
  EXPECT_EQ(SCANLINE_STATUS_INVOCATION_ERROR, status.type());
}

TEST(ScanlineWriterInterfaceTest, FinalizeWithStatus) {
  MockScanlineWriter writer;

  writer.Init(10, 10, ::pagespeed::image_compression::RGB_888);
  std::string output;
  writer.InitializeWrite(nullptr, &output);

  ScanlineStatus status = writer.FinalizeWriteWithStatus();
  EXPECT_TRUE(status.Success());
  EXPECT_TRUE(writer.finalized());
}

}  // namespace
}  // namespace net_instaweb
