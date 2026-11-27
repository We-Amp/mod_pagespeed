// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

#include "test/lib/image/test_utils.h"

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <fstream>

#include "gtest/gtest.h"
#include "lib/base/message_handler.h"
#include "lib/image/image_util.h"
#include "lib/image/read_image.h"
#include "lib/image/scanline_interface.h"
#include "lib/image/scanline_utils.h"

namespace pagespeed::image_compression {

using net_instaweb::ScanlineReaderInterface;

bool ReadFile(const std::string& file_name, std::string* content) {
  content->clear();
  std::ifstream file(file_name, std::ios::binary);
  if (!file.is_open()) {
    return false;
  }
  content->assign(std::istreambuf_iterator<char>(file),
                  std::istreambuf_iterator<char>());
  return !file.bad();
}

bool ReadTestFile(const std::string& path, const char* name,
                  const char* extension, std::string* content) {
  content->clear();
  std::string file_name =
      std::string(kTestRootDir) + path + name + "." + extension;
  return ReadFile(file_name, content);
}

bool ReadTestFileWithExt(const std::string& path,
                         const char* name_with_extension,
                         std::string* content) {
  std::string file_name =
      std::string(kTestRootDir) + path + name_with_extension;
  return ReadFile(file_name, content);
}

void CompareImageReaders(ScanlineReaderInterface* reader1,
                         ScanlineReaderInterface* reader2) {
  ASSERT_NE(nullptr, reader1);
  ASSERT_NE(nullptr, reader2);
  ASSERT_EQ(reader1->GetPixelFormat(), reader2->GetPixelFormat());
  ASSERT_EQ(reader1->GetImageHeight(), reader2->GetImageHeight());
  ASSERT_EQ(reader1->GetImageWidth(), reader2->GetImageWidth());
  ASSERT_EQ(reader1->GetBytesPerScanline(), reader2->GetBytesPerScanline());

  while (reader1->HasMoreScanLines() && reader2->HasMoreScanLines()) {
    uint8_t* scanline1 = nullptr;
    uint8_t* scanline2 = nullptr;
    ASSERT_TRUE(
        reader1->ReadNextScanline(reinterpret_cast<void**>(&scanline1)));
    ASSERT_TRUE(
        reader2->ReadNextScanline(reinterpret_cast<void**>(&scanline2)));
    EXPECT_EQ(0, memcmp(scanline1, scanline2, reader1->GetBytesPerScanline()));
  }

  // Make sure both readers have exhausted all of the scanlines.
  EXPECT_FALSE(reader1->HasMoreScanLines());
  EXPECT_FALSE(reader2->HasMoreScanLines());
}

void CompareImageRegions(const uint8_t* image1, PixelFormat format1,
                         int bytes_per_row1, int col1, int row1,
                         const uint8_t* image2, PixelFormat format2,
                         int bytes_per_row2, int col2, int row2, int num_cols,
                         int num_rows, MessageHandler* handler) {
  ASSERT_TRUE(format1 != UNSUPPORTED && format2 != UNSUPPORTED);
  const int num_channels1 = GetNumChannelsFromPixelFormat(format1, handler);
  const int num_channels2 = GetNumChannelsFromPixelFormat(format2, handler);

  PixelFormat format;
  int num_channels;
  if (num_channels1 >= num_channels2) {
    format = format1;
    num_channels = num_channels1;
  } else {
    format = format2;
    num_channels = num_channels2;
  }
  int bytes_per_line = num_cols * num_channels;

  int bytes_per_image = bytes_per_line * num_rows;
  std::unique_ptr<uint8_t[]> image_buffer1(new uint8_t[bytes_per_image]);
  std::unique_ptr<uint8_t[]> image_buffer2(new uint8_t[bytes_per_image]);
  ASSERT_TRUE(image_buffer1 != nullptr && image_buffer2 != nullptr);

  memset(image_buffer1.get(), 0, bytes_per_image);
  memset(image_buffer2.get(), 0, bytes_per_image);

  bool should_expand_colors = (format1 != format2);
  for (int row = 0; row < num_rows; ++row) {
    const uint8_t* src1 =
        image1 + static_cast<ptrdiff_t>((row + row1) * bytes_per_row1);
    uint8_t* dest1 =
        image_buffer1.get() + static_cast<ptrdiff_t>(row) * bytes_per_line;
    const uint8_t* src2 =
        image2 + static_cast<ptrdiff_t>((row + row2) * bytes_per_row2);
    uint8_t* dest2 =
        image_buffer2.get() + static_cast<ptrdiff_t>(row) * bytes_per_line;

    if (should_expand_colors) {
      ASSERT_TRUE(ExpandPixelFormat(num_cols, format1, col1, src1, format, 0,
                                    dest1, handler));
      ASSERT_TRUE(ExpandPixelFormat(num_cols, format2, col2, src2, format, 0,
                                    dest2, handler));
    } else {
      memcpy(dest1, src1 + static_cast<ptrdiff_t>(col1 * num_channels1),
             bytes_per_line);
      memcpy(dest2, src2 + static_cast<ptrdiff_t>(col2 * num_channels2),
             bytes_per_line);
    }
  }

  // Verify that all of the pixels are exactly the same.
  EXPECT_EQ(0,
            memcmp(image_buffer1.get(), image_buffer2.get(), bytes_per_image));
}

void SynthesizeImage(int width, int height, int bytes_per_line,
                     int num_channels, const uint8_t* seed_value,
                     const int* delta_x, const int* delta_y, uint8_t* image) {
  ASSERT_TRUE(image != nullptr);
  ASSERT_GT(width, 0);
  ASSERT_GT(height, 0);
  ASSERT_GE(bytes_per_line, width);

  std::unique_ptr<uint8_t[]> current_value(new uint8_t[num_channels]);
  memcpy(current_value.get(), seed_value, num_channels * sizeof(seed_value[0]));

  for (int y = 0; y < height; ++y) {
    uint8_t* pixel = image + static_cast<ptrdiff_t>(y * bytes_per_line);
    for (int x = 0; x < width; ++x) {
      for (int ch = 0; ch < num_channels; ++ch) {
        pixel[ch] = current_value[ch];
        current_value[ch] += delta_x[ch];
      }
      pixel += num_channels;
    }
    // Compute the value for the first pixel in the next line.
    for (int ch = 0; ch < num_channels; ++ch) {
      current_value[ch] = image[y * bytes_per_line + ch] + delta_y[ch];
    }
  }
}

// Golden GIF test image data - filenames, dimensions, and transparency.
const GoldImageCompressionInfo kValidGifImages[] = {
    {"basi0g01", 32, 32, false},   {"basi0g02", 32, 32, false},
    {"basi0g04", 32, 32, false},   {"basi0g08", 32, 32, false},
    {"basi3p01", 32, 32, false},   {"basi3p02", 32, 32, false},
    {"basi3p04", 32, 32, false},   {"basi3p08", 32, 32, false},
    {"basn0g01", 32, 32, false},   {"basn0g02", 32, 32, false},
    {"basn0g04", 32, 32, false},   {"basn0g08", 32, 32, false},
    {"basn3p01", 32, 32, false},   {"basn3p02", 32, 32, false},
    {"basn3p04", 32, 32, false},   {"basn3p08", 32, 32, false},
    {"tr-basi4a08", 32, 32, true}, {"tr-basn4a08", 32, 32, true},
};
const size_t kValidGifImageCount = arraysize(kValidGifImages);

void DecodeAndCompareImages(ImageFormat image_format1,
                            const void* image_buffer1, size_t buffer_length1,
                            ImageFormat image_format2,
                            const void* image_buffer2, size_t buffer_length2,
                            bool /*ignore_transparent_rgb*/,
                            MessageHandler* message_handler) {
  uint8_t* pixels1 = nullptr;
  uint8_t* pixels2 = nullptr;
  PixelFormat pixel_format1, pixel_format2;
  size_t width1, height1, stride1, width2, height2, stride2;

  // Decode the images.
  ASSERT_TRUE(ReadImage(image_format1, image_buffer1, buffer_length1,
                        reinterpret_cast<void**>(&pixels1), &pixel_format1,
                        &width1, &height1, &stride1, message_handler));
  ASSERT_TRUE(ReadImage(image_format2, image_buffer2, buffer_length2,
                        reinterpret_cast<void**>(&pixels2), &pixel_format2,
                        &width2, &height2, &stride2, message_handler));

  // Verify that the sizes are the same.
  ASSERT_EQ(width1, width2);
  ASSERT_EQ(height1, height2);

  // Compare with color expansion.
  CompareImageRegions(pixels1, pixel_format1, stride1, 0, 0, pixels2,
                      pixel_format2, stride2, 0, 0, width1, height1,
                      message_handler);

  free(pixels1);
  free(pixels2);
}

}  // namespace pagespeed::image_compression
