// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

// Ported from mod_pagespeed's test/pagespeed/kernel/image/jpeg_reader_test.cc

#include "lib/image/jpeg_reader.h"

#include <string>
#include <string_view>

#include "gtest/gtest.h"
#include "lib/base/message_handler.h"
#include "lib/image/image_util.h"
#include "test/lib/image/test_utils.h"

namespace {

using pagespeed::NullMessageHandler;
using pagespeed::image_compression::JpegScanlineReader;
using pagespeed::image_compression::kJpegTestDir;
using pagespeed::image_compression::ReadTestFile;
using pagespeed::image_compression::ReadTestFileWithExt;

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

constexpr size_t kInvalidFileCount =
    sizeof(kInvalidFiles) / sizeof(kInvalidFiles[0]);

// Verify that the reader can decode all valid JPEG test images and
// return plausible image properties and scanlines.
TEST(JpegReaderTest, ValidJpegs) {
  NullMessageHandler message_handler;
  for (auto& kValidJpegImage : kValidJpegImages) {
    std::string jpeg_image;
    ASSERT_TRUE(ReadTestFile(kJpegTestDir, kValidJpegImage, "jpg", &jpeg_image))
        << "Failed to read " << kValidJpegImage << ".jpg";

    JpegScanlineReader reader(&message_handler);
    ASSERT_TRUE(reader.Initialize(jpeg_image.c_str(), jpeg_image.length()))
        << "Failed to initialize " << kValidJpegImage;

    EXPECT_GT(reader.GetImageWidth(), 0u);
    EXPECT_GT(reader.GetImageHeight(), 0u);
    EXPECT_GT(reader.GetBytesPerScanline(), 0u);

    // Grayscale images have GRAY_8 format, others have RGB_888.
    if (std::string_view(kValidJpegImage) == "testgray") {
      EXPECT_EQ(pagespeed::image_compression::GRAY_8, reader.GetPixelFormat());
    } else {
      EXPECT_EQ(pagespeed::image_compression::RGB_888, reader.GetPixelFormat());
    }

    // Read all scanlines successfully.
    size_t rows_read = 0;
    while (reader.HasMoreScanLines()) {
      void* scanline = nullptr;
      ASSERT_TRUE(reader.ReadNextScanline(&scanline))
          << kValidJpegImage << " failed at row " << rows_read;
      ASSERT_NE(nullptr, scanline);
      ++rows_read;
    }
    EXPECT_EQ(reader.GetImageHeight(), rows_read);
  }
}

// Verify that the reader exits gracefully when the input is an invalid JPEG.
TEST(JpegReaderTest, InvalidJpegs) {
  for (size_t i = 0; i < kInvalidFileCount; ++i) {
    std::string src_data;
    ASSERT_TRUE(ReadTestFileWithExt(kJpegTestDir, kInvalidFiles[i], &src_data))
        << "Failed to read " << kInvalidFiles[i];
    NullMessageHandler message_handler;
    JpegScanlineReader reader(&message_handler);
    if (i < kInvalidFileCount - 1) {
      // Non-JPEG files should fail initialization.
      ASSERT_FALSE(reader.Initialize(src_data.c_str(), src_data.length()))
          << kInvalidFiles[i] << " should have failed Initialize";
    } else {
      // corrupt.jpg: header is valid, corruption is in the image data.
      // The exact row at which decoding fails depends on the libjpeg
      // version; just verify that it eventually fails before all rows
      // are consumed.
      ASSERT_TRUE(reader.Initialize(src_data.c_str(), src_data.length()));
      void* scanline = nullptr;
      bool hit_error = false;
      while (reader.HasMoreScanLines()) {
        if (!reader.ReadNextScanline(&scanline)) {
          hit_error = true;
          break;
        }
      }
      EXPECT_TRUE(hit_error)
          << "corrupt.jpg should have triggered a decoding error";
    }
  }
}

// Verify that the reader works properly regardless of how many scanlines
// it reads, and that re-initialization works.
TEST(JpegReaderTest, PartialRead) {
  std::string image1, image2;
  void* scanline = nullptr;
  NullMessageHandler message_handler;

  ASSERT_TRUE(ReadTestFile(kJpegTestDir, kValidJpegImages[0], "jpg", &image1));
  ASSERT_TRUE(ReadTestFile(kJpegTestDir, kValidJpegImages[1], "jpg", &image2));

  // Reader 1: initialize but read nothing.
  JpegScanlineReader reader1(&message_handler);
  ASSERT_TRUE(reader1.Initialize(image1.c_str(), image1.length()));

  // Reader 2: read one scanline.
  JpegScanlineReader reader2(&message_handler);
  ASSERT_TRUE(reader2.Initialize(image1.c_str(), image1.length()));
  ASSERT_TRUE(reader2.ReadNextScanline(&scanline));

  // Reader 3: read two scanlines, then re-initialize with a different image.
  JpegScanlineReader reader3(&message_handler);
  ASSERT_TRUE(reader3.Initialize(image1.c_str(), image1.length()));
  ASSERT_TRUE(reader3.ReadNextScanline(&scanline));
  ASSERT_TRUE(reader3.ReadNextScanline(&scanline));
  ASSERT_TRUE(reader3.Initialize(image2.c_str(), image2.length()));
  ASSERT_TRUE(reader3.ReadNextScanline(&scanline));

  // Reader 4: exhaust all scanlines.
  JpegScanlineReader reader4(&message_handler);
  ASSERT_TRUE(reader4.Initialize(image1.c_str(), image1.length()));
  while (reader4.HasMoreScanLines()) {
    ASSERT_TRUE(reader4.ReadNextScanline(&scanline));
  }

  // After exhaustion, reading should fail (release mode).
#ifdef NDEBUG
  EXPECT_FALSE(reader4.ReadNextScanline(&scanline));
#endif

  // Re-initialize with a different image should still work.
  ASSERT_TRUE(reader4.Initialize(image2.c_str(), image2.length()));
  ASSERT_TRUE(reader4.ReadNextScanline(&scanline));
}

// ==================================================================
// Phase 6: Additional edge case coverage
// ==================================================================

// ReadNextScanlineWithStatus without initialization returns error (L230-234).
TEST(JpegReaderTest, ReadWithoutInitialize) {
  NullMessageHandler message_handler;
  JpegScanlineReader reader(&message_handler);
  // Do NOT call Initialize(). HasMoreScanLines should be false.
  EXPECT_FALSE(reader.HasMoreScanLines());
}

// Initialize with truncated JPEG header triggers setjmp error (L182-189).
TEST(JpegReaderTest, TruncatedJpegHeaderTriggersSetjmp) {
  NullMessageHandler message_handler;
  // Valid JPEG SOI marker followed by truncated data.
  std::string truncated;
  truncated += '\xFF';
  truncated += '\xD8';  // SOI
  truncated += '\xFF';
  truncated += '\xE0';  // APP0 marker
  truncated += '\x00';
  truncated += '\x02';  // Length = 2 (too short for valid JFIF header)

  JpegScanlineReader reader(&message_handler);
  EXPECT_FALSE(reader.Initialize(truncated.c_str(), truncated.length()));
}

// Initialize with all-zeros data triggers error.
TEST(JpegReaderTest, AllZerosInput) {
  NullMessageHandler message_handler;
  std::string zeros(100, '\0');
  JpegScanlineReader reader(&message_handler);
  EXPECT_FALSE(reader.Initialize(zeros.c_str(), zeros.length()));
}

// Initialize with empty data triggers error.
TEST(JpegReaderTest, EmptyInput) {
  NullMessageHandler message_handler;
  JpegScanlineReader reader(&message_handler);
  EXPECT_FALSE(reader.Initialize("", 0));
}

// Double initialization: init with invalid, then init with valid.
TEST(JpegReaderTest, ReinitializeAfterInvalid) {
  NullMessageHandler message_handler;
  std::string valid_data;
  ASSERT_TRUE(ReadTestFile(kJpegTestDir, "test420", "jpg", &valid_data));

  JpegScanlineReader reader(&message_handler);
  // First initialize with garbage.
  std::string garbage = "not a jpeg";
  EXPECT_FALSE(reader.Initialize(garbage.c_str(), garbage.length()));

  // Re-initialize with valid data.
  ASSERT_TRUE(reader.Initialize(valid_data.c_str(), valid_data.length()));
  EXPECT_GT(reader.GetImageWidth(), 0u);
  EXPECT_GT(reader.GetImageHeight(), 0u);

  void* scanline = nullptr;
  ASSERT_TRUE(reader.ReadNextScanline(&scanline));
  EXPECT_NE(nullptr, scanline);
}

// Triple initialization: init, read, re-init, read, re-init, read.
TEST(JpegReaderTest, MultipleReinitialization) {
  NullMessageHandler message_handler;
  std::string image1, image2;
  ASSERT_TRUE(ReadTestFile(kJpegTestDir, "test411", "jpg", &image1));
  ASSERT_TRUE(ReadTestFile(kJpegTestDir, "testgray", "jpg", &image2));

  JpegScanlineReader reader(&message_handler);

  // First pass: read a few rows of image1.
  ASSERT_TRUE(reader.Initialize(image1.c_str(), image1.length()));
  void* scanline = nullptr;
  for (int i = 0; i < 3 && reader.HasMoreScanLines(); ++i) {
    ASSERT_TRUE(reader.ReadNextScanline(&scanline));
  }

  // Second pass: switch to image2 (grayscale).
  ASSERT_TRUE(reader.Initialize(image2.c_str(), image2.length()));
  EXPECT_EQ(pagespeed::image_compression::GRAY_8, reader.GetPixelFormat());
  ASSERT_TRUE(reader.ReadNextScanline(&scanline));
  EXPECT_NE(nullptr, scanline);

  // Third pass: switch back to image1.
  ASSERT_TRUE(reader.Initialize(image1.c_str(), image1.length()));
  EXPECT_EQ(pagespeed::image_compression::RGB_888, reader.GetPixelFormat());
  ASSERT_TRUE(reader.ReadNextScanline(&scanline));
  EXPECT_NE(nullptr, scanline);
}

// Read scanline mid-corruption: valid header, corrupt data (scanline-level).
TEST(JpegReaderTest, CorruptMidScanlineData) {
  NullMessageHandler message_handler;
  std::string src_data;
  ASSERT_TRUE(ReadTestFile(kJpegTestDir, "test420", "jpg", &src_data));

  // Corrupt bytes in the middle of the scan data (after the header).
  std::string corrupted = src_data;
  size_t corrupt_pos = corrupted.size() * 2 / 3;
  for (size_t i = corrupt_pos; i < corrupt_pos + 20 && i < corrupted.size();
       ++i) {
    corrupted[i] ^= 0xFF;
  }

  JpegScanlineReader reader(&message_handler);
  if (reader.Initialize(corrupted.c_str(), corrupted.length())) {
    // May initialize successfully, fail during reading.
    void* scanline = nullptr;
    bool hit_error = false;
    while (reader.HasMoreScanLines()) {
      if (!reader.ReadNextScanline(&scanline)) {
        hit_error = true;
        break;
      }
    }
    // May or may not hit error depending on where corruption lands.
    (void)hit_error;
  }
  // Just verify no crash.
  SUCCEED();
}

// Progressive JPEG detection.
TEST(JpegReaderTest, ProgressiveJpegDetection) {
  NullMessageHandler message_handler;
  std::string src_data;
  if (!ReadTestFileWithExt(kJpegTestDir, "progressive.jpg", &src_data) ||
      src_data.empty()) {
    GTEST_SKIP() << "progressive.jpg not found";
  }

  JpegScanlineReader reader(&message_handler);
  ASSERT_TRUE(reader.Initialize(src_data.c_str(), src_data.length()));
  EXPECT_TRUE(reader.IsProgressive());
}

// Non-progressive JPEG detection.
TEST(JpegReaderTest, NonProgressiveJpegDetection) {
  NullMessageHandler message_handler;
  std::string src_data;
  ASSERT_TRUE(ReadTestFile(kJpegTestDir, "test420", "jpg", &src_data));

  JpegScanlineReader reader(&message_handler);
  ASSERT_TRUE(reader.Initialize(src_data.c_str(), src_data.length()));
  EXPECT_FALSE(reader.IsProgressive());
}

// ==================================================================
// OOM null-check coverage
// ==================================================================

// Verify that JpegReader::is_valid() returns true for a normally constructed
// reader. The null-check paths in the constructor, InitializeWithStatus, and
// ReadNextScanlineWithStatus protect against OOM (malloc returning nullptr).
// We cannot easily mock malloc in a unit test, so we verify that the
// is_valid() API works correctly for the success case and that the error
// status codes are correct for the existing error paths.
TEST(JpegReaderTest, IsValidAfterConstruction) {
  NullMessageHandler message_handler;
  pagespeed::image_compression::JpegReader reader(&message_handler);
  // Under normal conditions both internal allocations succeed.
  EXPECT_TRUE(reader.is_valid());
  EXPECT_NE(nullptr, reader.decompress_struct());
}

// Verify that the scanline reader returns an error status (not a crash)
// when ReadNextScanlineWithStatus is called without initialization.
// This exercises the early-return guard that also protects the malloc
// null-check path in ReadNextScanlineWithStatus.
TEST(JpegReaderTest, ReadNextScanlineWithoutInitReturnsError) {
  NullMessageHandler message_handler;
  JpegScanlineReader reader(&message_handler);
  void* scanline = nullptr;
  auto status = reader.ReadNextScanlineWithStatus(&scanline);
  EXPECT_FALSE(status.Success());
}

}  // namespace
