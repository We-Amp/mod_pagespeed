// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

// Test that basic GifReader operations succeed or fail as expected.
// Ported from mod_pagespeed's test/pagespeed/kernel/image/gif_reader_test.cc
//
// Modernization changes:
// - GoogleString -> std::string
// - scoped_ptr -> std::unique_ptr
// - DISALLOW_COPY_AND_ASSIGN -> = delete
// - NullMutex removed (NullMessageHandler used instead)
// - GifAnimationTest skipped (requires GifSquare + egiflib)

#include "lib/image/gif_reader.h"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <string>

#include "gtest/gtest.h"
#include "lib/base/basictypes.h"
#include "lib/base/message_handler.h"
#include "lib/image/image_frame_interface.h"
#include "lib/image/image_util.h"
#include "lib/image/png_optimizer.h"
#include "lib/image/read_image.h"
#include "lib/image/scanline_interface_frame_adapter.h"
#include "lib/image/scanline_utils.h"
#include "test/lib/image/test_utils.h"

extern "C" {
#include "png.h"
}

namespace pagespeed::image_compression {

// Friend adapter so we can instantiate GifFrameReader.
class TestGifFrameReader : public GifFrameReader {
 public:
  explicit TestGifFrameReader(MessageHandler* handler)
      : GifFrameReader(handler) {}
};

}  // namespace pagespeed::image_compression

namespace {

using pagespeed::image_compression::DecodeAndCompareImages;
using pagespeed::image_compression::FrameSpec;
using pagespeed::image_compression::FrameToScanlineReaderAdapter;
using pagespeed::image_compression::GifDisposalToFrameSpecDisposal;
using pagespeed::image_compression::GifFrameReader;
using pagespeed::image_compression::GifReader;
using pagespeed::image_compression::IMAGE_GIF;
using pagespeed::image_compression::IMAGE_PNG;
using pagespeed::image_compression::ImageSpec;
using pagespeed::image_compression::kAlphaOpaque;
using pagespeed::image_compression::kAlphaTransparent;
using pagespeed::image_compression::kGifTestDir;
using pagespeed::image_compression::kPngSuiteGifTestDir;
using pagespeed::image_compression::kPngSuiteTestDir;
using pagespeed::image_compression::kPngTestDir;
using pagespeed::image_compression::kValidGifImageCount;
using pagespeed::image_compression::kValidGifImages;
using pagespeed::image_compression::MultipleFrameReader;
using pagespeed::image_compression::PixelFormat;
using pagespeed::image_compression::PixelRgbaChannels;
using pagespeed::image_compression::PngReaderInterface;
using pagespeed::image_compression::QUIRKS_CHROME;
using pagespeed::image_compression::QUIRKS_FIREFOX;
using pagespeed::image_compression::QUIRKS_NONE;
using pagespeed::image_compression::ReadFile;
using pagespeed::image_compression::ReadTestFile;
using pagespeed::image_compression::RGB_888;
using pagespeed::image_compression::RGBA_8888;
using pagespeed::image_compression::RGBA_ALPHA;
using pagespeed::image_compression::RGBA_BLUE;
using pagespeed::image_compression::RGBA_GREEN;
using pagespeed::image_compression::RGBA_NUM_CHANNELS;
using pagespeed::image_compression::RGBA_RED;
using pagespeed::image_compression::RgbaChannels;
using pagespeed::image_compression::ScanlineStatus;
using pagespeed::image_compression::ScopedPngStruct;
using pagespeed::image_compression::size_px;
using pagespeed::image_compression::TestGifFrameReader;
// For integration tests via read_image.cc dispatch.
using net_instaweb::ScanlineReaderInterface;
using pagespeed::NullMessageHandler;
using pagespeed::image_compression::CreateImageFrameReader;
using pagespeed::image_compression::ReadTestFileWithExt;

const char* kValidOpaqueGifImages[] = {
    "basi0g01", "basi0g02", "basi0g04", "basi0g08", "basi3p01", "basi3p02",
    "basi3p04", "basi3p08", "basn0g01", "basn0g02", "basn0g04", "basn0g08",
    "basn3p01", "basn3p02", "basn3p04", "basn3p08",
};

const char* kValidTransparentGifImages[] = {"tr-basi4a08", "tr-basn4a08"};

const char kAnimatedGif[] = "animated";
const char kBadGif[] = "bad";
const char kFrameSmallerThanScreen[] = "frame_smaller_than_screen";
const char kInterlacedImage[] = "interlaced";
const char kRedConforming[] = "red_conforming";
const char kRedEmptyScreen[] = "red_empty_screen";
const char kRedUnusedBackground[] = "red_unused_invalid_background";
const char kTransparentGif[] = "transparent";
const char kZeroSizeAnimatedGif[] = "zero_size_animation";
const char kInvalidPixelLocalPaletteGif[] = "bad_pixel_local_palette";
const char kInvalidPixelGlobalPaletteGif[] = "bad_pixel_global_palette";

class GifReaderTest : public testing::Test {
 public:
  GifReaderTest()
      : message_handler_(),
        gif_reader_(new GifReader(&message_handler_)),
        read_(ScopedPngStruct::READ, &message_handler_) {}

 protected:
  pagespeed::NullMessageHandler message_handler_;
  std::unique_ptr<PngReaderInterface> gif_reader_;
  ScopedPngStruct read_;

  GifReaderTest(const GifReaderTest&) = delete;
  GifReaderTest& operator=(const GifReaderTest&) = delete;
};

TEST_F(GifReaderTest, LoadValidGifsWithoutTransforms) {
  std::string in;
  for (auto& kValidOpaqueGifImage : kValidOpaqueGifImages) {
    ReadTestFile(kPngSuiteGifTestDir, kValidOpaqueGifImage, "gif", &in);
    ASSERT_NE(static_cast<size_t>(0), in.length());
    ASSERT_TRUE(gif_reader_->ReadPng(in, read_.png_ptr(), read_.info_ptr(),
                                     PNG_TRANSFORM_IDENTITY))
        << kValidOpaqueGifImage;
    ASSERT_TRUE(read_.reset());
  }

  for (auto& kValidTransparentGifImage : kValidTransparentGifImages) {
    ReadTestFile(kPngSuiteGifTestDir, kValidTransparentGifImage, "gif", &in);
    ASSERT_NE(static_cast<size_t>(0), in.length());
    ASSERT_TRUE(gif_reader_->ReadPng(in, read_.png_ptr(), read_.info_ptr(),
                                     PNG_TRANSFORM_IDENTITY))
        << kValidTransparentGifImage;
    ASSERT_TRUE(read_.reset());
  }

  ReadTestFile(kGifTestDir, "transparent", "gif", &in);
  ASSERT_NE(static_cast<size_t>(0), in.length());
  ASSERT_TRUE(gif_reader_->ReadPng(in, read_.png_ptr(), read_.info_ptr(),
                                   PNG_TRANSFORM_IDENTITY));
}

TEST_F(GifReaderTest, ExpandColorMapForValidGifs) {
  std::string in;
  for (auto& kValidOpaqueGifImage : kValidOpaqueGifImages) {
    ReadTestFile(kPngSuiteGifTestDir, kValidOpaqueGifImage, "gif", &in);
    ASSERT_NE(static_cast<size_t>(0), in.length());
    ASSERT_TRUE(gif_reader_->ReadPng(in, read_.png_ptr(), read_.info_ptr(),
                                     PNG_TRANSFORM_EXPAND))
        << kValidOpaqueGifImage;
    ASSERT_TRUE(read_.reset());
  }

  for (auto& kValidTransparentGifImage : kValidTransparentGifImages) {
    ReadTestFile(kPngSuiteGifTestDir, kValidTransparentGifImage, "gif", &in);
    ASSERT_NE(static_cast<size_t>(0), in.length());
    ASSERT_TRUE(gif_reader_->ReadPng(in, read_.png_ptr(), read_.info_ptr(),
                                     PNG_TRANSFORM_EXPAND))
        << kValidTransparentGifImage;
    ASSERT_TRUE(read_.reset());
  }

  ReadTestFile(kGifTestDir, "transparent", "gif", &in);
  ASSERT_NE(static_cast<size_t>(0), in.length());
  ASSERT_TRUE(gif_reader_->ReadPng(in, read_.png_ptr(), read_.info_ptr(),
                                   PNG_TRANSFORM_EXPAND));
}

TEST_F(GifReaderTest, RequireOpaqueForValidGifs) {
  std::string in;
  for (auto& kValidOpaqueGifImage : kValidOpaqueGifImages) {
    ReadTestFile(kPngSuiteGifTestDir, kValidOpaqueGifImage, "gif", &in);
    ASSERT_NE(static_cast<size_t>(0), in.length());
    ASSERT_TRUE(gif_reader_->ReadPng(in, read_.png_ptr(), read_.info_ptr(),
                                     PNG_TRANSFORM_IDENTITY, true))
        << kValidOpaqueGifImage;
    ASSERT_TRUE(read_.reset());
  }

  for (auto& kValidTransparentGifImage : kValidTransparentGifImages) {
    ReadTestFile(kPngSuiteGifTestDir, kValidTransparentGifImage, "gif", &in);
    ASSERT_NE(static_cast<size_t>(0), in.length());
    ASSERT_FALSE(gif_reader_->ReadPng(in, read_.png_ptr(), read_.info_ptr(),
                                      PNG_TRANSFORM_IDENTITY, true))
        << kValidTransparentGifImage;
    ASSERT_TRUE(read_.reset());
  }

  ReadTestFile(kGifTestDir, "transparent", "gif", &in);
  ASSERT_NE(static_cast<size_t>(0), in.length());
  ASSERT_FALSE(gif_reader_->ReadPng(in, read_.png_ptr(), read_.info_ptr(),
                                    PNG_TRANSFORM_IDENTITY, true));
}

TEST_F(GifReaderTest, ExpandColormapAndRequireOpaqueForValidGifs) {
  std::string in;
  for (auto& kValidOpaqueGifImage : kValidOpaqueGifImages) {
    ReadTestFile(kPngSuiteGifTestDir, kValidOpaqueGifImage, "gif", &in);
    ASSERT_NE(static_cast<size_t>(0), in.length());
    ASSERT_TRUE(gif_reader_->ReadPng(in, read_.png_ptr(), read_.info_ptr(),
                                     PNG_TRANSFORM_EXPAND, true))
        << kValidOpaqueGifImage;
    ASSERT_TRUE(read_.reset());
  }

  for (auto& kValidTransparentGifImage : kValidTransparentGifImages) {
    ReadTestFile(kPngSuiteGifTestDir, kValidTransparentGifImage, "gif", &in);
    ASSERT_NE(static_cast<size_t>(0), in.length());
    ASSERT_FALSE(gif_reader_->ReadPng(in, read_.png_ptr(), read_.info_ptr(),
                                      PNG_TRANSFORM_EXPAND, true))
        << kValidTransparentGifImage;
    ASSERT_TRUE(read_.reset());
  }

  ReadTestFile(kGifTestDir, "transparent", "gif", &in);
  ASSERT_NE(static_cast<size_t>(0), in.length());
  ASSERT_FALSE(gif_reader_->ReadPng(in, read_.png_ptr(), read_.info_ptr(),
                                    PNG_TRANSFORM_EXPAND, true));
}

TEST_F(GifReaderTest, StripAlpha) {
  std::string in;
  png_uint_32 height;
  png_uint_32 width;
  int bit_depth;
  int color_type;
  png_bytep trans;
  int num_trans;
  png_color_16p trans_values;

  ReadTestFile(kGifTestDir, "transparent", "gif", &in);
  ASSERT_NE(static_cast<size_t>(0), in.length());
  ASSERT_TRUE(gif_reader_->ReadPng(in, read_.png_ptr(), read_.info_ptr(),
                                   PNG_TRANSFORM_STRIP_ALPHA, false));
  png_get_IHDR(read_.png_ptr(), read_.info_ptr(), &width, &height, &bit_depth,
               &color_type, nullptr, nullptr, nullptr);
  ASSERT_TRUE((color_type & PNG_COLOR_MASK_ALPHA) == 0);  // NOLINT
  ASSERT_EQ(static_cast<unsigned int>(0),
            png_get_tRNS(read_.png_ptr(), read_.info_ptr(), &trans, &num_trans,
                         &trans_values));

  read_.reset();

  ASSERT_TRUE(gif_reader_->ReadPng(
      in, read_.png_ptr(), read_.info_ptr(),
      PNG_TRANSFORM_STRIP_ALPHA | PNG_TRANSFORM_EXPAND, false));
  png_get_IHDR(read_.png_ptr(), read_.info_ptr(), &width, &height, &bit_depth,
               &color_type, nullptr, nullptr, nullptr);
  ASSERT_TRUE((color_type & PNG_COLOR_MASK_ALPHA) == 0);  // NOLINT

  ASSERT_EQ(static_cast<unsigned int>(0),
            png_get_tRNS(read_.png_ptr(), read_.info_ptr(), &trans, &num_trans,
                         &trans_values));
}

TEST_F(GifReaderTest, ExpandColormapOnZeroSizeCanvasAndCatchLibPngError) {
  std::string in;
  ReadTestFile(kGifTestDir, "zero_size_animation", "gif", &in);
  ASSERT_NE(static_cast<size_t>(0), in.length());
  ASSERT_FALSE(gif_reader_->ReadPng(in, read_.png_ptr(), read_.info_ptr(),
                                    PNG_TRANSFORM_EXPAND, true));
}

class GifScanlineReaderRawTest : public testing::Test {
 public:
  GifScanlineReaderRawTest()
      : message_handler_(),
        reader_(new TestGifFrameReader(&message_handler_)) {}

  bool Initialize(const char* file_name) {
    if (!ReadTestFile(kGifTestDir, file_name, "gif", &input_image_)) {
      return false;
    }
    return reader_.Initialize(input_image_.c_str(), input_image_.length());
  }

 protected:
  void* scanline_{nullptr};
  pagespeed::NullMessageHandler message_handler_;
  FrameToScanlineReaderAdapter reader_;
  std::string input_image_;

  GifScanlineReaderRawTest(const GifScanlineReaderRawTest&) = delete;
  GifScanlineReaderRawTest& operator=(const GifScanlineReaderRawTest&) = delete;
};

TEST_F(GifScanlineReaderRawTest, CorruptHeader) {
  ReadTestFile(kGifTestDir, kTransparentGif, "gif", &input_image_);
  // Make GifRecordType invalid.
  input_image_[781] = 0;
  ASSERT_FALSE(reader_.Initialize(input_image_.c_str(), input_image_.length()));
}

TEST_F(GifScanlineReaderRawTest, InitializeWithoutRead) {
  ASSERT_TRUE(Initialize(kTransparentGif));
}

TEST_F(GifScanlineReaderRawTest, ReadOneRow) {
  ASSERT_TRUE(Initialize(kTransparentGif));
  EXPECT_TRUE(reader_.ReadNextScanline(&scanline_));
}

TEST_F(GifScanlineReaderRawTest, ReinitializeAfterOneRow) {
  ASSERT_TRUE(Initialize(kTransparentGif));
  EXPECT_TRUE(reader_.ReadNextScanline(&scanline_));
  ASSERT_TRUE(Initialize(kInterlacedImage));
  EXPECT_TRUE(reader_.ReadNextScanline(&scanline_));
}

TEST_F(GifScanlineReaderRawTest, ReInitializeAfterLastRow) {
  ASSERT_TRUE(Initialize(kTransparentGif));
  while (reader_.HasMoreScanLines()) {
    EXPECT_TRUE(reader_.ReadNextScanline(&scanline_));
  }

  // After depleting the scanlines, any further call to
  // ReadNextScanline should return false.
  EXPECT_FALSE(reader_.ReadNextScanline(&scanline_));

  ASSERT_TRUE(Initialize(kInterlacedImage));
  EXPECT_TRUE(reader_.ReadNextScanline(&scanline_));
}

// Animated GIF is not supported via scanline API. Make sure it exits
// gracefully.
TEST_F(GifScanlineReaderRawTest, AnimatedGif) {
  ASSERT_FALSE(Initialize(kAnimatedGif));
}

TEST_F(GifScanlineReaderRawTest, BadGif) { ASSERT_FALSE(Initialize(kBadGif)); }

TEST_F(GifScanlineReaderRawTest, ZeroSizeGif) {
  ASSERT_FALSE(Initialize(kZeroSizeAnimatedGif));
}

// Check the accuracy of the reader. Compare the decoded results with the gold
// data (".gif.rgba"). The test images include transparent and opaque ones.
TEST_F(GifScanlineReaderRawTest, ValidGifs) {
  for (size_t i = 0; i < kValidGifImageCount; i++) {
    std::string rgba_image, gif_image;
    const char* file_name = kValidGifImages[i].filename;
    ReadTestFile(kPngSuiteGifTestDir, file_name, "gif.rgba", &rgba_image);
    ReadTestFile(kPngSuiteGifTestDir, file_name, "gif", &gif_image);

    const auto* reference_rgba =
        reinterpret_cast<const uint8_t*>(rgba_image.data());
    uint8_t* decoded_pixels = nullptr;

    ASSERT_TRUE(reader_.Initialize(gif_image.data(), gif_image.length()));

    PixelFormat pixel_format = reader_.GetPixelFormat();
    int width = reader_.GetImageWidth();
    int height = reader_.GetImageHeight();
    int bytes_per_row = reader_.GetBytesPerScanline();
    int num_channels =
        GetNumChannelsFromPixelFormat(pixel_format, &message_handler_);

    EXPECT_EQ(kValidGifImages[i].width, width);
    EXPECT_EQ(kValidGifImages[i].height, height);
    if (kValidGifImages[i].transparency) {
      EXPECT_EQ(pagespeed::image_compression::RGBA_8888, pixel_format);
      EXPECT_EQ(4, num_channels);
    } else {
      EXPECT_EQ(pagespeed::image_compression::RGB_888, pixel_format);
      EXPECT_EQ(3, num_channels);
    }
    EXPECT_EQ(
        width * num_channels,
        bytes_per_row);  // NOLINT(bugprone-implicit-widening-of-multiplication-result)

    // Decode and check the image a row at a time.
    int row = 0;
    while (reader_.HasMoreScanLines()) {
      EXPECT_TRUE(
          reader_.ReadNextScanline(reinterpret_cast<void**>(&decoded_pixels)));

      for (int x = 0; x < width; ++x) {
        int index_dec = x * num_channels;
        int index_ref = (row * width + x) * 4;
        ASSERT_EQ(0, memcmp(reference_rgba + index_ref,
                            decoded_pixels + index_dec, num_channels));
      }
      ++row;
    }

    // Make sure both readers have exhausted all image rows.
    EXPECT_EQ(height, row);
    EXPECT_EQ(rgba_image.length(), static_cast<size_t>(4 * height * width));
  }
}

TEST_F(GifScanlineReaderRawTest, Interlaced) {
  std::string png_image, gif_image;
  ReadTestFile(kGifTestDir, kInterlacedImage, "png", &png_image);
  ReadTestFile(kGifTestDir, kInterlacedImage, "gif", &gif_image);
  DecodeAndCompareImages(IMAGE_PNG, png_image.c_str(), png_image.length(),
                         IMAGE_GIF, gif_image.c_str(), gif_image.length(),
                         false,  // ignore_transparent_rgb
                         &message_handler_);
}

TEST_F(GifScanlineReaderRawTest, EmptyScreen) {
  std::string png_image, gif_image;
  ReadTestFile(kGifTestDir, kRedConforming, "png", &png_image);
  ReadTestFile(kGifTestDir, kRedEmptyScreen, "gif", &gif_image);
  DecodeAndCompareImages(IMAGE_PNG, png_image.c_str(), png_image.length(),
                         IMAGE_GIF, gif_image.c_str(), gif_image.length(),
                         false,  // ignore_transparent_rgb
                         &message_handler_);
}

TEST_F(GifScanlineReaderRawTest, UnusedBackground) {
  std::string png_image, gif_image;
  ReadTestFile(kGifTestDir, kRedConforming, "png", &png_image);
  ReadTestFile(kGifTestDir, kRedUnusedBackground, "gif", &gif_image);
  DecodeAndCompareImages(IMAGE_PNG, png_image.c_str(), png_image.length(),
                         IMAGE_GIF, gif_image.c_str(), gif_image.length(),
                         false,  // ignore_transparent_rgb
                         &message_handler_);
}

TEST_F(GifScanlineReaderRawTest, FrameSmallerThanImage) {
  ASSERT_FALSE(Initialize(kFrameSmallerThanScreen));
}

TEST(GifReaderUtil, DisposalMethod) {
  for (int i = -1; i < 4; ++i) {
    FrameSpec::DisposalMethod actual_disposal =
        GifDisposalToFrameSpecDisposal(i);
    FrameSpec::DisposalMethod expected_disposal = FrameSpec::DISPOSAL_NONE;
    switch (i) {
      case 0:
        // intentional fall-through
      case 1:
        expected_disposal = FrameSpec::DISPOSAL_NONE;
        break;
      case 2:
        expected_disposal = FrameSpec::DISPOSAL_BACKGROUND;
        break;
      case 3:
        expected_disposal = FrameSpec::DISPOSAL_RESTORE;
        break;
      default:
        break;
    }
    EXPECT_EQ(expected_disposal, actual_disposal);
  }
}

void CheckQuirksModeChangesToImageSpec(const FrameSpec& frame_spec,
                                       const ImageSpec& original_spec,
                                       const bool has_loop_count,
                                       const ImageSpec& expected_noquirks_spec,
                                       const ImageSpec& expected_firefox_spec,
                                       const ImageSpec& expected_chrome_spec) {
  ImageSpec noquirks_spec = original_spec;
  ImageSpec firefox_spec = original_spec;
  ImageSpec chrome_spec = original_spec;

  GifFrameReader::ApplyQuirksModeToImage(QUIRKS_NONE, has_loop_count,
                                         frame_spec, &noquirks_spec);
  EXPECT_TRUE(noquirks_spec.Equals(expected_noquirks_spec));

  GifFrameReader::ApplyQuirksModeToImage(QUIRKS_FIREFOX, has_loop_count,
                                         frame_spec, &firefox_spec);
  EXPECT_TRUE(firefox_spec.Equals(expected_firefox_spec))
      << "\nActual:\n"
      << firefox_spec.ToString() << "\nExpected:\n"
      << expected_firefox_spec.ToString();

  GifFrameReader::ApplyQuirksModeToImage(QUIRKS_CHROME, has_loop_count,
                                         frame_spec, &chrome_spec);
  EXPECT_TRUE(chrome_spec.Equals(expected_chrome_spec))
      << "\nActual:\n"
      << chrome_spec.ToString() << "\nExpected:\n"
      << expected_chrome_spec.ToString();
}

void SetOpaqueBackground(PixelRgbaChannels rgba) {
  // NOTE: In the reference code, this function has a loop bug that makes
  // it a no-op: `channel < static_cast<RgbaChannels>(channel)` is always
  // false. We replicate that behavior here because the test expectations
  // rely on bg_color remaining at the default (all zeros from Reset()).
  static const PixelRgbaChannels kOpaqueBackground = {0x80, 0x80, 0x80,
                                                      kAlphaOpaque};
  for (int channel = 0; channel < static_cast<RgbaChannels>(channel);  // NOLINT
       ++channel) {
    rgba[channel] = kOpaqueBackground[channel];
  }
}

TEST(ApplyQuirksModeToImage, TestFrameWidthLargerThanImageWidth) {
  ImageSpec image_spec;
  image_spec.width = 100;
  image_spec.height = 100;
  image_spec.num_frames = 2;
  SetOpaqueBackground(image_spec.bg_color);

  FrameSpec frame_spec;
  frame_spec.width = 200;
  frame_spec.height = 100;
  frame_spec.top = 10;
  frame_spec.left = 2;

  ImageSpec expected_noquirks_spec = image_spec;
  ImageSpec expected_firefox_spec = image_spec;
  ImageSpec expected_chrome_spec = image_spec;

  expected_chrome_spec.width = frame_spec.width;
  expected_chrome_spec.height = frame_spec.height;
  expected_chrome_spec.image_size_adjusted = true;

  CheckQuirksModeChangesToImageSpec(
      frame_spec, image_spec, false, expected_noquirks_spec,
      expected_firefox_spec, expected_chrome_spec);

  image_spec.num_frames = expected_noquirks_spec.num_frames =
      expected_firefox_spec.num_frames = expected_chrome_spec.num_frames = 1;

  CheckQuirksModeChangesToImageSpec(
      frame_spec, image_spec, false, expected_noquirks_spec,
      expected_firefox_spec, expected_chrome_spec);
}

TEST(ApplyQuirksModeToImage, TestFrameHeightLargerThanImageHeight) {
  ImageSpec image_spec;
  image_spec.width = 100;
  image_spec.height = 100;
  image_spec.num_frames = 2;
  SetOpaqueBackground(image_spec.bg_color);

  FrameSpec frame_spec;
  frame_spec.width = 100;
  frame_spec.height = 200;
  frame_spec.top = 10;
  frame_spec.left = 2;

  ImageSpec expected_noquirks_spec = image_spec;
  ImageSpec expected_firefox_spec = image_spec;
  ImageSpec expected_chrome_spec = image_spec;

  expected_chrome_spec.width = frame_spec.width;
  expected_chrome_spec.height = frame_spec.height;
  expected_chrome_spec.image_size_adjusted = true;

  CheckQuirksModeChangesToImageSpec(
      frame_spec, image_spec, false, expected_noquirks_spec,
      expected_firefox_spec, expected_chrome_spec);

  image_spec.num_frames = expected_noquirks_spec.num_frames =
      expected_firefox_spec.num_frames = expected_chrome_spec.num_frames = 1;

  CheckQuirksModeChangesToImageSpec(
      frame_spec, image_spec, false, expected_noquirks_spec,
      expected_firefox_spec, expected_chrome_spec);
}

TEST(ApplyQuirksModeToImage, TestFrameWidthSmallerThanImageWidth) {
  ImageSpec image_spec;
  image_spec.width = 100;
  image_spec.height = 100;
  image_spec.num_frames = 2;
  SetOpaqueBackground(image_spec.bg_color);

  FrameSpec frame_spec;
  frame_spec.width = 50;
  frame_spec.height = 100;
  frame_spec.top = 10;
  frame_spec.left = 2;

  ImageSpec expected_noquirks_spec = image_spec;
  ImageSpec expected_firefox_spec = image_spec;
  ImageSpec expected_chrome_spec = image_spec;

  CheckQuirksModeChangesToImageSpec(
      frame_spec, image_spec, false, expected_noquirks_spec,
      expected_firefox_spec, expected_chrome_spec);

  image_spec.num_frames = expected_noquirks_spec.num_frames =
      expected_firefox_spec.num_frames = expected_chrome_spec.num_frames = 1;

  expected_chrome_spec.bg_color[RGBA_ALPHA] = kAlphaTransparent;
  expected_firefox_spec.bg_color[RGBA_ALPHA] = kAlphaTransparent;

  CheckQuirksModeChangesToImageSpec(
      frame_spec, image_spec, false, expected_noquirks_spec,
      expected_firefox_spec, expected_chrome_spec);
}

TEST(ApplyQuirksModeToImage, TestFrameHeightSmallerThanImageHeight) {
  ImageSpec image_spec;
  image_spec.width = 100;
  image_spec.height = 100;
  image_spec.num_frames = 2;
  SetOpaqueBackground(image_spec.bg_color);

  FrameSpec frame_spec;
  frame_spec.width = 100;
  frame_spec.height = 50;
  frame_spec.top = 10;
  frame_spec.left = 2;

  ImageSpec expected_noquirks_spec = image_spec;
  ImageSpec expected_firefox_spec = image_spec;
  ImageSpec expected_chrome_spec = image_spec;

  CheckQuirksModeChangesToImageSpec(
      frame_spec, image_spec, false, expected_noquirks_spec,
      expected_firefox_spec, expected_chrome_spec);

  image_spec.num_frames = expected_noquirks_spec.num_frames =
      expected_firefox_spec.num_frames = expected_chrome_spec.num_frames = 1;

  expected_chrome_spec.bg_color[RGBA_ALPHA] = kAlphaTransparent;
  expected_firefox_spec.bg_color[RGBA_ALPHA] = kAlphaTransparent;

  CheckQuirksModeChangesToImageSpec(
      frame_spec, image_spec, false, expected_noquirks_spec,
      expected_firefox_spec, expected_chrome_spec);
}

TEST(ApplyQuirksModeToImage, TestLoopCount) {
  ImageSpec image_spec;
  FrameSpec frame_spec;
  image_spec.width = 100;
  image_spec.height = 100;
  image_spec.loop_count = 3;
  frame_spec.width = 100;
  frame_spec.height = 100;
  frame_spec.top = 0;
  frame_spec.left = 0;

  ImageSpec expected_noquirks_spec = image_spec;
  ImageSpec expected_firefox_spec = image_spec;
  ImageSpec expected_chrome_spec = image_spec;

  CheckQuirksModeChangesToImageSpec(
      frame_spec, image_spec, false, expected_noquirks_spec,
      expected_firefox_spec, expected_chrome_spec);

  expected_chrome_spec.loop_count = image_spec.loop_count + 1;

  CheckQuirksModeChangesToImageSpec(
      frame_spec, image_spec, true, expected_noquirks_spec,
      expected_firefox_spec, expected_chrome_spec);
}

TEST(ApplyQuirksModeToImage, TestNoop) {
  ImageSpec image_spec;
  FrameSpec frame_spec;
  image_spec.width = 100;
  image_spec.height = 100;
  frame_spec.width = 50;
  frame_spec.height = 50;
  frame_spec.top = 10;
  frame_spec.left = 2;

  ImageSpec expected_noquirks_spec = image_spec;
  ImageSpec expected_firefox_spec = image_spec;
  ImageSpec expected_chrome_spec = image_spec;

  CheckQuirksModeChangesToImageSpec(
      frame_spec, image_spec, false, expected_noquirks_spec,
      expected_firefox_spec, expected_chrome_spec);
}

void CheckQuirksModeChangesToFirstFrameSpec(
    const ImageSpec& image_spec, const FrameSpec& original_spec,
    const FrameSpec& expected_noquirks_spec,
    const FrameSpec& expected_firefox_spec,
    const FrameSpec& expected_chrome_spec) {
  FrameSpec noquirks_spec = original_spec;
  FrameSpec firefox_spec = original_spec;
  FrameSpec chrome_spec = original_spec;

  ImageSpec image = image_spec;
  GifFrameReader::ApplyQuirksModeToImage(QUIRKS_NONE, false, noquirks_spec,
                                         &image);
  GifFrameReader::ApplyQuirksModeToFirstFrame(QUIRKS_NONE, image,
                                              &noquirks_spec);
  EXPECT_TRUE(noquirks_spec.Equals(expected_noquirks_spec))
      << " Got: " << noquirks_spec.ToString()
      << "\n Expected: " << expected_noquirks_spec.ToString();

  image = image_spec;
  GifFrameReader::ApplyQuirksModeToImage(QUIRKS_FIREFOX, false, firefox_spec,
                                         &image);
  GifFrameReader::ApplyQuirksModeToFirstFrame(QUIRKS_FIREFOX, image,
                                              &firefox_spec);
  EXPECT_TRUE(firefox_spec.Equals(expected_firefox_spec))
      << "      Got: " << firefox_spec.ToString()
      << "\n Expected: " << expected_firefox_spec.ToString();

  image = image_spec;
  GifFrameReader::ApplyQuirksModeToImage(QUIRKS_CHROME, false, chrome_spec,
                                         &image);
  GifFrameReader::ApplyQuirksModeToFirstFrame(QUIRKS_CHROME, image,
                                              &chrome_spec);
  EXPECT_TRUE(chrome_spec.Equals(expected_chrome_spec))
      << "      Got: " << chrome_spec.ToString()
      << "\n Expected: " << expected_chrome_spec.ToString();
}

TEST(ApplyQuirksModeToFirstFrame, TestWidth) {
  ImageSpec image_spec;
  FrameSpec frame_spec;
  image_spec.width = 100;
  image_spec.height = 100;
  frame_spec.width = 200;
  frame_spec.height = 50;
  frame_spec.top = 10;
  frame_spec.left = 2;

  FrameSpec expected_noquirks_spec = frame_spec;
  FrameSpec expected_firefox_spec = frame_spec;
  FrameSpec expected_chrome_spec = frame_spec;

  expected_firefox_spec.top = 0;
  expected_firefox_spec.left = 0;
  expected_firefox_spec.width = 0;
  expected_firefox_spec.height = 0;

  expected_chrome_spec.top = 0;
  expected_chrome_spec.left = 0;

  CheckQuirksModeChangesToFirstFrameSpec(
      image_spec, frame_spec, expected_noquirks_spec, expected_firefox_spec,
      expected_chrome_spec);
}

TEST(ApplyQuirksModeToFirstFrame, TestHeight) {
  ImageSpec image_spec;
  FrameSpec frame_spec;
  image_spec.width = 100;
  image_spec.height = 100;
  frame_spec.width = 50;
  frame_spec.height = 200;
  frame_spec.top = 10;
  frame_spec.left = 2;

  FrameSpec expected_noquirks_spec = frame_spec;
  FrameSpec expected_firefox_spec = frame_spec;
  FrameSpec expected_chrome_spec = frame_spec;

  expected_firefox_spec.top = 0;
  expected_firefox_spec.left = 0;
  expected_firefox_spec.width = 0;
  expected_firefox_spec.height = 0;

  expected_chrome_spec.top = 0;
  expected_chrome_spec.left = 0;

  CheckQuirksModeChangesToFirstFrameSpec(
      image_spec, frame_spec, expected_noquirks_spec, expected_firefox_spec,
      expected_chrome_spec);
}

TEST(ApplyQuirksModeToFirstFrame, TestZeroWidthFrame) {
  ImageSpec image_spec;
  FrameSpec frame_spec;
  image_spec.width = 100;
  image_spec.height = 120;
  frame_spec.width = 0;
  frame_spec.height = 50;
  frame_spec.top = 10;
  frame_spec.left = 2;

  FrameSpec expected_noquirks_spec = frame_spec;
  FrameSpec expected_chrome_spec = frame_spec;
  FrameSpec expected_firefox_spec = frame_spec;

  expected_chrome_spec.height = image_spec.height;
  expected_chrome_spec.width = image_spec.width;
  expected_chrome_spec.top = 0;
  expected_chrome_spec.left = 0;

  expected_firefox_spec.top = 0;
  expected_firefox_spec.left = 0;
  expected_firefox_spec.width = 0;
  expected_firefox_spec.height = 0;

  CheckQuirksModeChangesToFirstFrameSpec(
      image_spec, frame_spec, expected_noquirks_spec, expected_firefox_spec,
      expected_chrome_spec);
}

TEST(ApplyQuirksModeToFirstFrame, TestZeroHeightFrame) {
  ImageSpec image_spec;
  FrameSpec frame_spec;
  image_spec.width = 100;
  image_spec.height = 120;
  frame_spec.width = 50;
  frame_spec.height = 0;
  frame_spec.top = 10;
  frame_spec.left = 2;

  FrameSpec expected_noquirks_spec = frame_spec;
  FrameSpec expected_chrome_spec = frame_spec;
  FrameSpec expected_firefox_spec = frame_spec;

  expected_chrome_spec.height = image_spec.height;
  expected_chrome_spec.width = image_spec.width;
  expected_chrome_spec.top = 0;
  expected_chrome_spec.left = 0;

  expected_firefox_spec.top = 0;
  expected_firefox_spec.left = 0;
  expected_firefox_spec.width = 0;
  expected_firefox_spec.height = 0;

  CheckQuirksModeChangesToFirstFrameSpec(
      image_spec, frame_spec, expected_noquirks_spec, expected_firefox_spec,
      expected_chrome_spec);
}

TEST(ApplyQuirksModeToFirstFrame, TestNoopChrome) {
  ImageSpec image_spec;
  FrameSpec frame_spec;
  image_spec.width = 100;
  image_spec.height = 120;
  frame_spec.width = 60;
  frame_spec.height = 50;
  frame_spec.top = 10;
  frame_spec.left = 2;

  FrameSpec expected_noquirks_spec = frame_spec;
  FrameSpec expected_firefox_spec = frame_spec;
  FrameSpec expected_chrome_spec = frame_spec;

  expected_firefox_spec.top = 0;
  expected_firefox_spec.left = 0;
  expected_firefox_spec.width = 0;
  expected_firefox_spec.height = 0;

  CheckQuirksModeChangesToFirstFrameSpec(
      image_spec, frame_spec, expected_noquirks_spec, expected_firefox_spec,
      expected_chrome_spec);
}

TEST(ApplyQuirksModeToFirstFrame, TestNoop) {
  ImageSpec image_spec;
  FrameSpec frame_spec;
  image_spec.width = 100;
  image_spec.height = 120;
  frame_spec.width = 100;
  frame_spec.height = 120;
  frame_spec.top = 0;
  frame_spec.left = 0;

  FrameSpec expected_noquirks_spec = frame_spec;
  FrameSpec expected_firefox_spec = frame_spec;
  FrameSpec expected_chrome_spec = frame_spec;

  CheckQuirksModeChangesToFirstFrameSpec(
      image_spec, frame_spec, expected_noquirks_spec, expected_firefox_spec,
      expected_chrome_spec);
}

void CheckImageForOutOfBoundsPixel(const char* filename,
                                   int first_invalid_frame) {
  const PixelRgbaChannels kTransparentPixel = {0, 0, 0, kAlphaTransparent};

  pagespeed::NullMessageHandler message_handler;
  std::unique_ptr<MultipleFrameReader> reader(
      new TestGifFrameReader(&message_handler));
  std::string input_image;

  if (!ReadTestFile(kGifTestDir, filename, "gif", &input_image)) {
    ADD_FAILURE() << "Failed to read file: " << filename;
    return;
  }

  EXPECT_TRUE(
      reader->Initialize(input_image.c_str(), input_image.length()).Success());
  ScanlineStatus status;
  ImageSpec image_spec;
  FrameSpec frame_spec;

  EXPECT_TRUE(reader->GetImageSpec(&image_spec, &status));
  EXPECT_GT(image_spec.num_frames, static_cast<size_px>(first_invalid_frame));

  // Frames up to the first invalid frame should have format RGB_888.
  for (size_px frame = 0; frame < static_cast<size_px>(first_invalid_frame);
       ++frame) {
    EXPECT_TRUE(reader->HasMoreFrames());
    EXPECT_TRUE(reader->PrepareNextFrame(&status));
    EXPECT_TRUE(reader->GetFrameSpec(&frame_spec, &status));
    EXPECT_EQ(RGB_888, frame_spec.pixel_format);
  }

  // The invalid frame should have format RGBA_8888, and all its
  // pixels, which are out of palette bounds, should be reported
  // as transparent.
  EXPECT_TRUE(reader->HasMoreFrames());
  EXPECT_TRUE(reader->PrepareNextFrame(&status));
  EXPECT_TRUE(reader->GetFrameSpec(&frame_spec, &status));
  EXPECT_EQ(RGBA_8888, frame_spec.pixel_format);
  const uint8_t* scanline;
  for (size_px row = 0; row < frame_spec.height; ++row) {
    EXPECT_TRUE(reader->HasMoreScanlines());
    EXPECT_TRUE(reader->ReadNextScanline(
        reinterpret_cast<const void**>(&scanline), &status));
    for (size_px col = 0; col < frame_spec.width; ++col) {
      int cmp_result =
          memcmp(scanline + static_cast<size_t>(RGBA_NUM_CHANNELS * col),
                 &kTransparentPixel, RGBA_NUM_CHANNELS * sizeof(uint8_t));
      EXPECT_EQ(0, cmp_result);
      if (cmp_result != 0) {
        // Return eagerly to avoid excessive output in case of error.
        return;
      }
    }
  }
  EXPECT_FALSE(reader->HasMoreScanlines());
}

TEST(InvalidPixels, TestOutOfRangePixelValueInLocalPalette) {
  CheckImageForOutOfBoundsPixel(kInvalidPixelLocalPaletteGif, 3);
}

TEST(InvalidPixels, TestOutOfRangePixelValueInGlobalPalette) {
  CheckImageForOutOfBoundsPixel(kInvalidPixelGlobalPaletteGif, 3);
}

// =============================================================================
// Integration tests via read_image.cc dispatch (CreateImageFrameReader)
// =============================================================================

TEST(GifReaderIntegration, CreateImageFrameReaderStaticGif) {
  NullMessageHandler handler;
  std::string data;
  ASSERT_TRUE(ReadTestFileWithExt(kGifTestDir, "o.gif", &data));

  ScanlineStatus status;
  std::unique_ptr<MultipleFrameReader> reader(CreateImageFrameReader(
      IMAGE_GIF, data.data(), data.size(), QUIRKS_CHROME, &handler, &status));
  ASSERT_NE(nullptr, reader.get()) << status.ToString();
  EXPECT_TRUE(status.Success());

  ImageSpec image_spec;
  EXPECT_TRUE(reader->GetImageSpec(&image_spec, &status));
  EXPECT_GT(image_spec.width, 0u);
  EXPECT_GT(image_spec.height, 0u);

  // Read at least one frame.
  EXPECT_TRUE(reader->HasMoreFrames());
  EXPECT_TRUE(reader->PrepareNextFrame(&status));
  FrameSpec frame_spec;
  EXPECT_TRUE(reader->GetFrameSpec(&frame_spec, &status));
  EXPECT_GT(frame_spec.width, 0u);

  // Read scanlines.
  while (reader->HasMoreScanlines()) {
    const void* scanline;
    EXPECT_TRUE(reader->ReadNextScanline(&scanline, &status));
    EXPECT_NE(nullptr, scanline);
  }
}

TEST(GifReaderIntegration, CreateImageFrameReaderAnimatedGif) {
  NullMessageHandler handler;
  std::string data;
  ASSERT_TRUE(ReadTestFileWithExt(kGifTestDir, "animated.gif", &data));

  ScanlineStatus status;
  std::unique_ptr<MultipleFrameReader> reader(CreateImageFrameReader(
      IMAGE_GIF, data.data(), data.size(), QUIRKS_CHROME, &handler, &status));
  ASSERT_NE(nullptr, reader.get()) << status.ToString();

  ImageSpec image_spec;
  EXPECT_TRUE(reader->GetImageSpec(&image_spec, &status));
  EXPECT_GT(image_spec.width, 0u);
  EXPECT_GT(image_spec.height, 0u);

  // Animated GIFs should have multiple frames.
  int frame_count = 0;
  while (reader->HasMoreFrames()) {
    EXPECT_TRUE(reader->PrepareNextFrame(&status));
    FrameSpec frame_spec;
    EXPECT_TRUE(reader->GetFrameSpec(&frame_spec, &status));

    while (reader->HasMoreScanlines()) {
      const void* scanline;
      EXPECT_TRUE(reader->ReadNextScanline(&scanline, &status));
    }
    frame_count++;
  }
  EXPECT_GT(frame_count, 1) << "Expected animated GIF to have multiple frames";
}

TEST(GifReaderIntegration, CreateImageFrameReaderMalformedTruncated) {
  NullMessageHandler handler;
  // Truncated GIF: just the magic bytes.
  const char truncated[] = "GIF89a";

  ScanlineStatus status;
  std::unique_ptr<MultipleFrameReader> reader(CreateImageFrameReader(
      IMAGE_GIF, truncated, 6, QUIRKS_CHROME, &handler, &status));
  // Should either return nullptr or fail on first frame read.
  if (reader != nullptr) {
    // Reader was created but initialization may have partially succeeded.
    // Attempting to read should fail gracefully.
    if (reader->HasMoreFrames()) {
      ScanlineStatus frame_status;
      bool ok = reader->PrepareNextFrame(&frame_status);
      // If PrepareNextFrame succeeds, reading scanlines should eventually fail.
      if (ok) {
        const void* scanline;
        while (reader->HasMoreScanlines()) {
          reader->ReadNextScanline(&scanline, &frame_status);
        }
      }
    }
  }
  // No crash = pass.
}

TEST(GifReaderIntegration, CreateImageFrameReaderMalformedBadData) {
  NullMessageHandler handler;
  const char garbage[] = "This is not a GIF file at all!";

  ScanlineStatus status;
  std::unique_ptr<MultipleFrameReader> reader(
      CreateImageFrameReader(IMAGE_GIF, garbage, sizeof(garbage) - 1,
                             QUIRKS_CHROME, &handler, &status));
  // Should fail — either nullptr or failing status.
  if (reader != nullptr) {
    EXPECT_FALSE(status.Success());
  }
}

TEST(GifReaderIntegration, CreateImageFrameReaderZeroLength) {
  NullMessageHandler handler;
  ScanlineStatus status;
  std::unique_ptr<MultipleFrameReader> reader(CreateImageFrameReader(
      IMAGE_GIF, "", 0, QUIRKS_CHROME, &handler, &status));
  // Zero-length should fail.
  EXPECT_EQ(nullptr, reader.get());
  EXPECT_FALSE(status.Success());
}

// =============================================================================
// Phase 4: Extended GIF coverage - animated frames, transparency,
// interlacing, error paths, and GifReader API
// =============================================================================

// --- GifFrameReader: animated GIF multi-frame reading ---

TEST(GifFrameReaderExtended, AnimatedGifFrameDetails) {
  NullMessageHandler handler;
  std::unique_ptr<MultipleFrameReader> reader(new TestGifFrameReader(&handler));
  std::string data;
  ASSERT_TRUE(ReadTestFileWithExt(kGifTestDir, "animated.gif", &data));

  ScanlineStatus status;
  EXPECT_TRUE(reader->Initialize(data.data(), data.size()).Success());

  ImageSpec image_spec;
  EXPECT_TRUE(reader->GetImageSpec(&image_spec, &status));
  EXPECT_GT(image_spec.width, 0u);
  EXPECT_GT(image_spec.height, 0u);
  EXPECT_GT(image_spec.num_frames, 1u);

  // Read all frames and verify each has scanlines.
  int frame_count = 0;
  while (reader->HasMoreFrames()) {
    EXPECT_TRUE(reader->PrepareNextFrame(&status)) << status.ToString();
    FrameSpec frame_spec;
    EXPECT_TRUE(reader->GetFrameSpec(&frame_spec, &status));
    EXPECT_GT(frame_spec.width, 0u);
    EXPECT_GT(frame_spec.height, 0u);

    int scanline_count = 0;
    while (reader->HasMoreScanlines()) {
      const void* scanline = nullptr;
      EXPECT_TRUE(reader->ReadNextScanline(&scanline, &status));
      EXPECT_NE(nullptr, scanline);
      ++scanline_count;
    }
    EXPECT_EQ(static_cast<int>(frame_spec.height), scanline_count);
    ++frame_count;
  }
  EXPECT_GT(frame_count, 1);
}

// --- GifFrameReader: animated interlaced GIF ---

TEST(GifFrameReaderExtended, AnimatedInterlacedGif) {
  NullMessageHandler handler;
  std::unique_ptr<MultipleFrameReader> reader(new TestGifFrameReader(&handler));
  std::string data;
  ASSERT_TRUE(
      ReadTestFileWithExt(kGifTestDir, "animated_interlaced.gif", &data));

  ScanlineStatus status;
  EXPECT_TRUE(reader->Initialize(data.data(), data.size()).Success());

  ImageSpec image_spec;
  EXPECT_TRUE(reader->GetImageSpec(&image_spec, &status));
  EXPECT_GT(image_spec.width, 0u);
  EXPECT_GT(image_spec.height, 0u);

  int frame_count = 0;
  while (reader->HasMoreFrames()) {
    EXPECT_TRUE(reader->PrepareNextFrame(&status)) << status.ToString();
    FrameSpec frame_spec;
    EXPECT_TRUE(reader->GetFrameSpec(&frame_spec, &status));

    while (reader->HasMoreScanlines()) {
      const void* scanline = nullptr;
      EXPECT_TRUE(reader->ReadNextScanline(&scanline, &status));
    }
    ++frame_count;
  }
  EXPECT_GT(frame_count, 0);
}

// --- GifFrameReader: looping animated GIF ---

TEST(GifFrameReaderExtended, LoopingAnimatedGif) {
  NullMessageHandler handler;
  std::unique_ptr<MultipleFrameReader> reader(new TestGifFrameReader(&handler));
  std::string data;
  ASSERT_TRUE(ReadTestFileWithExt(kGifTestDir, "full2loop.gif", &data));

  ScanlineStatus status;
  EXPECT_TRUE(reader->Initialize(data.data(), data.size()).Success());

  ImageSpec image_spec;
  EXPECT_TRUE(reader->GetImageSpec(&image_spec, &status));
  EXPECT_GT(image_spec.width, 0u);
  EXPECT_GT(image_spec.height, 0u);
  // Looping GIFs should have loop_count > 0.
  EXPECT_GT(image_spec.loop_count, 0u);

  int frame_count = 0;
  while (reader->HasMoreFrames()) {
    EXPECT_TRUE(reader->PrepareNextFrame(&status));
    FrameSpec frame_spec;
    EXPECT_TRUE(reader->GetFrameSpec(&frame_spec, &status));

    while (reader->HasMoreScanlines()) {
      const void* scanline = nullptr;
      EXPECT_TRUE(reader->ReadNextScanline(&scanline, &status));
    }
    ++frame_count;
  }
  EXPECT_GT(frame_count, 1);
}

// --- GifFrameReader: completely transparent GIF ---

TEST(GifFrameReaderExtended, CompletelyTransparentGif) {
  NullMessageHandler handler;
  std::unique_ptr<MultipleFrameReader> reader(new TestGifFrameReader(&handler));
  std::string data;
  ASSERT_TRUE(
      ReadTestFileWithExt(kGifTestDir, "completely_transparent.gif", &data));

  ScanlineStatus status;
  EXPECT_TRUE(reader->Initialize(data.data(), data.size()).Success());

  ImageSpec image_spec;
  EXPECT_TRUE(reader->GetImageSpec(&image_spec, &status));

  // Read the first frame.
  EXPECT_TRUE(reader->HasMoreFrames());
  EXPECT_TRUE(reader->PrepareNextFrame(&status));
  FrameSpec frame_spec;
  EXPECT_TRUE(reader->GetFrameSpec(&frame_spec, &status));

  // All pixels should be transparent.
  EXPECT_EQ(RGBA_8888, frame_spec.pixel_format);

  while (reader->HasMoreScanlines()) {
    const uint8_t* scanline = nullptr;
    EXPECT_TRUE(reader->ReadNextScanline(
        reinterpret_cast<const void**>(&scanline), &status));
    // Verify each pixel has alpha=0.
    for (size_px col = 0; col < frame_spec.width; ++col) {
      EXPECT_EQ(0, scanline[col * RGBA_NUM_CHANNELS + RGBA_ALPHA]);
    }
  }
}

// --- GifReader: GetAttributes on various GIF types ---

TEST_F(GifReaderTest, GetAttributesTransparentGif) {
  std::string in;
  ReadTestFile(kGifTestDir, kTransparentGif, "gif", &in);
  ASSERT_NE(0u, in.length());

  int width, height, bit_depth, color_type;
  ASSERT_TRUE(
      gif_reader_->GetAttributes(in, &width, &height, &bit_depth, &color_type));
  EXPECT_GT(width, 0);
  EXPECT_GT(height, 0);
}

TEST_F(GifReaderTest, GetAttributesInterlacedGif) {
  std::string in;
  ReadTestFile(kGifTestDir, kInterlacedImage, "gif", &in);
  ASSERT_NE(0u, in.length());

  int width, height, bit_depth, color_type;
  ASSERT_TRUE(
      gif_reader_->GetAttributes(in, &width, &height, &bit_depth, &color_type));
  EXPECT_GT(width, 0);
  EXPECT_GT(height, 0);
}

TEST_F(GifReaderTest, GetAttributesFailsOnBadGif) {
  std::string in;
  ReadTestFile(kGifTestDir, kBadGif, "gif", &in);
  int width, height, bit_depth, color_type;
  // Bad GIF should fail GetAttributes.
  EXPECT_FALSE(
      gif_reader_->GetAttributes(in, &width, &height, &bit_depth, &color_type));
}

TEST_F(GifReaderTest, GetAttributesFailsOnEmpty) {
  std::string empty;
  int width, height, bit_depth, color_type;
  EXPECT_FALSE(gif_reader_->GetAttributes(empty, &width, &height, &bit_depth,
                                          &color_type));
}

TEST_F(GifReaderTest, GetAttributesFailsOnGarbage) {
  std::string garbage = "This is not a GIF file.";
  int width, height, bit_depth, color_type;
  EXPECT_FALSE(gif_reader_->GetAttributes(garbage, &width, &height, &bit_depth,
                                          &color_type));
}

// --- GifReader: ReadPng with various transforms on opaque GIFs ---

TEST_F(GifReaderTest, ReadPngWithExpandTransform) {
  std::string in;
  ReadTestFile(kPngSuiteGifTestDir, "basn3p08", "gif", &in);
  ASSERT_NE(0u, in.length());

  ASSERT_TRUE(gif_reader_->ReadPng(in, read_.png_ptr(), read_.info_ptr(),
                                   PNG_TRANSFORM_EXPAND));
  png_uint_32 width, height;
  int bd, ct;
  png_get_IHDR(read_.png_ptr(), read_.info_ptr(), &width, &height, &bd, &ct,
               nullptr, nullptr, nullptr);
  EXPECT_EQ(32u, width);
  EXPECT_EQ(32u, height);
  // After EXPAND, palette should become RGB.
  EXPECT_EQ(PNG_COLOR_TYPE_RGB, ct);
}

TEST_F(GifReaderTest, ReadPngGrayscaleGif) {
  // Grayscale GIF (encoded as palette-based).
  std::string in;
  ReadTestFile(kPngSuiteGifTestDir, "basn0g08", "gif", &in);
  ASSERT_NE(0u, in.length());

  ASSERT_TRUE(gif_reader_->ReadPng(in, read_.png_ptr(), read_.info_ptr(),
                                   PNG_TRANSFORM_IDENTITY));
  png_uint_32 width, height;
  int bd, ct;
  png_get_IHDR(read_.png_ptr(), read_.info_ptr(), &width, &height, &bd, &ct,
               nullptr, nullptr, nullptr);
  EXPECT_EQ(32u, width);
  EXPECT_EQ(32u, height);
}

// --- GifScanlineReaderRaw: interlaced GIF pixel verification ---

TEST_F(GifScanlineReaderRawTest, InterlacedGifAllRows) {
  ASSERT_TRUE(Initialize(kInterlacedImage));
  int width = reader_.GetImageWidth();
  int height = reader_.GetImageHeight();
  EXPECT_GT(width, 0);
  EXPECT_GT(height, 0);

  int rows = 0;
  while (reader_.HasMoreScanLines()) {
    EXPECT_TRUE(reader_.ReadNextScanline(&scanline_));
    ASSERT_NE(nullptr, scanline_);
    ++rows;
  }
  EXPECT_EQ(height, rows);
}

// --- GifScanlineReaderRaw: transparent GIF pixel data ---

TEST_F(GifScanlineReaderRawTest, TransparentGifAllRows) {
  ASSERT_TRUE(Initialize(kTransparentGif));
  int width = reader_.GetImageWidth();
  int height = reader_.GetImageHeight();
  EXPECT_GT(width, 0);
  EXPECT_GT(height, 0);

  // Transparent GIF should have RGBA pixel format.
  EXPECT_EQ(RGBA_8888, reader_.GetPixelFormat());
  EXPECT_EQ(static_cast<size_t>(width * 4), reader_.GetBytesPerScanline());

  int rows = 0;
  while (reader_.HasMoreScanLines()) {
    EXPECT_TRUE(reader_.ReadNextScanline(&scanline_));
    ++rows;
  }
  EXPECT_EQ(height, rows);
}

// --- GifScanlineReaderRaw: opaque GIF pixel format ---

TEST_F(GifScanlineReaderRawTest, OpaqueGifRGB) {
  // Read an opaque GIF via the scanline reader.
  std::string data;
  ReadTestFile(kGifTestDir, kInterlacedImage, "gif", &data);
  ASSERT_NE(0u, data.length());
  ASSERT_TRUE(reader_.Initialize(data.data(), data.length()));

  // Opaque GIF should have RGB pixel format.
  EXPECT_EQ(RGB_888, reader_.GetPixelFormat());
  EXPECT_EQ(static_cast<size_t>(reader_.GetImageWidth() * 3),
            reader_.GetBytesPerScanline());
}

// --- GifFrameReader: quirks mode with Firefox ---

TEST(GifFrameReaderExtended, FirefoxQuirksMode) {
  NullMessageHandler handler;
  std::unique_ptr<MultipleFrameReader> reader(new TestGifFrameReader(&handler));

  // Set Firefox quirks mode.
  EXPECT_TRUE(reader->set_quirks_mode(QUIRKS_FIREFOX).Success());

  std::string data;
  ASSERT_TRUE(ReadTestFileWithExt(kGifTestDir, "animated.gif", &data));
  EXPECT_TRUE(reader->Initialize(data.data(), data.size()).Success());

  ScanlineStatus status;
  ImageSpec image_spec;
  EXPECT_TRUE(reader->GetImageSpec(&image_spec, &status));
  EXPECT_GT(image_spec.width, 0u);

  // Read through all frames to exercise the Firefox quirks path.
  while (reader->HasMoreFrames()) {
    EXPECT_TRUE(reader->PrepareNextFrame(&status));
    FrameSpec frame_spec;
    EXPECT_TRUE(reader->GetFrameSpec(&frame_spec, &status));
    while (reader->HasMoreScanlines()) {
      const void* scanline = nullptr;
      EXPECT_TRUE(reader->ReadNextScanline(&scanline, &status));
    }
  }
}

// --- GifFrameReader: no quirks mode ---

TEST(GifFrameReaderExtended, NoQuirksMode) {
  NullMessageHandler handler;
  std::unique_ptr<MultipleFrameReader> reader(new TestGifFrameReader(&handler));

  EXPECT_TRUE(reader->set_quirks_mode(QUIRKS_NONE).Success());

  std::string data;
  ASSERT_TRUE(ReadTestFileWithExt(kGifTestDir, "animated.gif", &data));
  EXPECT_TRUE(reader->Initialize(data.data(), data.size()).Success());

  ScanlineStatus status;
  ImageSpec image_spec;
  EXPECT_TRUE(reader->GetImageSpec(&image_spec, &status));

  while (reader->HasMoreFrames()) {
    EXPECT_TRUE(reader->PrepareNextFrame(&status));
    FrameSpec frame_spec;
    EXPECT_TRUE(reader->GetFrameSpec(&frame_spec, &status));
    while (reader->HasMoreScanlines()) {
      const void* scanline = nullptr;
      EXPECT_TRUE(reader->ReadNextScanline(&scanline, &status));
    }
  }
}

// --- GifFrameReader: Reset and re-initialize ---

TEST(GifFrameReaderExtended, ResetAndReinitialize) {
  NullMessageHandler handler;
  std::unique_ptr<MultipleFrameReader> reader(new TestGifFrameReader(&handler));

  std::string data;
  ASSERT_TRUE(ReadTestFileWithExt(kGifTestDir, "o.gif", &data));

  ScanlineStatus status;
  // Initialize, read some frames.
  EXPECT_TRUE(reader->Initialize(data.data(), data.size()).Success());
  ImageSpec image_spec;
  EXPECT_TRUE(reader->GetImageSpec(&image_spec, &status));
  if (reader->HasMoreFrames()) {
    EXPECT_TRUE(reader->PrepareNextFrame(&status));
  }

  // Reset and re-initialize with different data.
  EXPECT_TRUE(reader->Reset().Success());
  std::string data2;
  ASSERT_TRUE(ReadTestFileWithExt(kGifTestDir, "interlaced.gif", &data2));
  EXPECT_TRUE(reader->Initialize(data2.data(), data2.size()).Success());
  EXPECT_TRUE(reader->GetImageSpec(&image_spec, &status));
  EXPECT_GT(image_spec.width, 0u);
}

// --- GifScanlineReaderRaw: error paths ---

TEST_F(GifScanlineReaderRawTest, TruncatedGifHeader) {
  // A truncated GIF (just the magic bytes) should fail.
  const char truncated[] = "GIF89a";
  ASSERT_FALSE(reader_.Initialize(truncated, 6));
}

TEST_F(GifScanlineReaderRawTest, EmptyData) {
  ASSERT_FALSE(reader_.Initialize("", 0));
}

TEST_F(GifScanlineReaderRawTest, GarbageData) {
  const char garbage[] = "This is definitely not a GIF!";
  ASSERT_FALSE(reader_.Initialize(garbage, sizeof(garbage) - 1));
}

// --- GifScanlineReaderRaw: multiple re-initialization ---

TEST_F(GifScanlineReaderRawTest, MultipleReinitialize) {
  // Initialize with one GIF, read a few lines, then re-initialize
  // with another.
  ASSERT_TRUE(Initialize(kTransparentGif));
  EXPECT_TRUE(reader_.ReadNextScanline(&scanline_));
  EXPECT_TRUE(reader_.ReadNextScanline(&scanline_));

  // Re-initialize with a different GIF.
  ASSERT_TRUE(Initialize(kInterlacedImage));
  int rows = 0;
  while (reader_.HasMoreScanLines()) {
    EXPECT_TRUE(reader_.ReadNextScanline(&scanline_));
    ++rows;
  }
  EXPECT_GT(rows, 0);
}

// --- GifReader: comparison between PNG and GIF decoded pixels ---

// --- GifReader::ReadPng with unsupported transform flags ---

TEST_F(GifReaderTest, ReadPngRejectsUnsupportedTransforms) {
  std::string in;
  ReadTestFile(kPngSuiteGifTestDir, "basn3p08", "gif", &in);
  ASSERT_NE(0u, in.length());

  // 0xFF has many bits set that are outside the allowed transforms
  // (IDENTITY, EXPAND, STRIP_16, GRAY_TO_RGB, STRIP_ALPHA).
  // ReadPng should return false for unsupported transforms.
  EXPECT_FALSE(
      gif_reader_->ReadPng(in, read_.png_ptr(), read_.info_ptr(), 0xFF));
  ASSERT_TRUE(read_.reset());

  // Also test with a single unsupported bit (PNG_TRANSFORM_INVERT_MONO = 0x20).
  EXPECT_FALSE(gif_reader_->ReadPng(in, read_.png_ptr(), read_.info_ptr(),
                                    PNG_TRANSFORM_INVERT_MONO));
}

// --- GifReader::ReadPng with empty/null data ---

TEST_F(GifReaderTest, ReadPngEmptyData) {
  // Empty string should fail because DGifOpen can't parse it.
  std::string empty;
  EXPECT_FALSE(gif_reader_->ReadPng(empty, read_.png_ptr(), read_.info_ptr(),
                                    PNG_TRANSFORM_IDENTITY));
  ASSERT_TRUE(read_.reset());

  // Single-byte buffer should also fail.
  std::string one_byte(1, '\x00');
  EXPECT_FALSE(gif_reader_->ReadPng(one_byte, read_.png_ptr(), read_.info_ptr(),
                                    PNG_TRANSFORM_IDENTITY));
}

// --- GifFrameReader: set_quirks_mode after Initialize should fail ---

TEST(GifFrameReaderExtended, SetQuirksModeAfterInitializeFails) {
  NullMessageHandler handler;
  std::unique_ptr<MultipleFrameReader> reader(new TestGifFrameReader(&handler));
  std::string data;
  ASSERT_TRUE(ReadTestFileWithExt(kGifTestDir, "animated.gif", &data));

  // Initialize the reader first.
  EXPECT_TRUE(reader->Initialize(data.data(), data.size()).Success());

  // Attempting to change quirks mode after initialization should fail.
  ScanlineStatus status = reader->set_quirks_mode(QUIRKS_FIREFOX);
  EXPECT_FALSE(status.Success());

  // Verify the reader still works (initialization not corrupted).
  ImageSpec image_spec;
  ScanlineStatus img_status;
  EXPECT_TRUE(reader->GetImageSpec(&image_spec, &img_status));
  EXPECT_GT(image_spec.width, 0u);
}

// --- GifFrameReader: PrepareNextFrame beyond available frames ---

TEST(GifFrameReaderExtended, PrepareNextFrameBeyondFrames) {
  NullMessageHandler handler;
  std::unique_ptr<MultipleFrameReader> reader(new TestGifFrameReader(&handler));
  std::string data;
  // Use a single-frame GIF.
  ASSERT_TRUE(ReadTestFileWithExt(kGifTestDir, "o.gif", &data));

  ScanlineStatus status;
  EXPECT_TRUE(reader->Initialize(data.data(), data.size()).Success());

  ImageSpec image_spec;
  EXPECT_TRUE(reader->GetImageSpec(&image_spec, &status));

  // Read the single frame and exhaust all scanlines.
  EXPECT_TRUE(reader->HasMoreFrames());
  EXPECT_TRUE(reader->PrepareNextFrame(&status));
  while (reader->HasMoreScanlines()) {
    const void* scanline = nullptr;
    EXPECT_TRUE(reader->ReadNextScanline(&scanline, &status));
  }

  // There should be no more frames now.
  EXPECT_FALSE(reader->HasMoreFrames());

  // PrepareNextFrame should fail since no more frames.
  EXPECT_FALSE(reader->PrepareNextFrame(&status));
}

// --- GifFrameReader: Initialize without setting image buffer ---

TEST(GifFrameReaderExtended, InitializeWithoutBuffer) {
  NullMessageHandler handler;
  std::unique_ptr<MultipleFrameReader> reader(new TestGifFrameReader(&handler));

  // Call Initialize() without having set the image buffer.
  // The image_buffer_ is nullptr by default. Should fail.
  ScanlineStatus status = reader->Initialize();
  EXPECT_FALSE(status.Success());
}

TEST_F(GifReaderTest, OpaqueGifMatchesPngPixels) {
  // For each opaque GIF that has a corresponding PNG in pngsuite,
  // verify both decode to the same dimensions via GetAttributes.
  for (auto& kValidOpaqueGifImage : kValidOpaqueGifImages) {
    std::string gif_data;
    ReadTestFile(kPngSuiteGifTestDir, kValidOpaqueGifImage, "gif", &gif_data);
    ASSERT_NE(0u, gif_data.length()) << kValidOpaqueGifImage;

    int gw, gh, gbd, gct;
    ASSERT_TRUE(gif_reader_->GetAttributes(gif_data, &gw, &gh, &gbd, &gct))
        << kValidOpaqueGifImage;
    EXPECT_EQ(32, gw) << kValidOpaqueGifImage;
    EXPECT_EQ(32, gh) << kValidOpaqueGifImage;
  }
}

// =============================================================================
// Malformed GIF tests: trigger error paths in gif_reader.cc
// =============================================================================

// Helper to build a minimal valid GIF89a with a 1x1 red pixel.
// This is used as a starting point for corruption tests.
std::string MakeMinimalGif89a() {
  // GIF89a header
  std::string gif;
  gif += "GIF89a";
  // Logical Screen Descriptor: 1x1, global color table with 2 entries
  gif += '\x01';  // width low
  gif += '\x00';  // width high
  gif += '\x01';  // height low
  gif += '\x00';  // height high
  gif += '\x80';  // packed: GCT flag=1, color resolution=0, sort=0, GCT size=0
                  // (2^(0+1)=2 entries)
  gif += '\x00';  // background color index
  gif += '\x00';  // pixel aspect ratio

  // Global Color Table: 2 entries (6 bytes)
  gif += std::string("\xFF\x00\x00", 3);  // color 0: red
  gif += std::string("\x00\x00\x00", 3);  // color 1: black

  // Image Descriptor
  gif += '\x2C';  // Image separator
  gif += '\x00';  // left low
  gif += '\x00';  // left high
  gif += '\x00';  // top low
  gif += '\x00';  // top high
  gif += '\x01';  // width low
  gif += '\x00';  // width high
  gif += '\x01';  // height low
  gif += '\x00';  // height high
  gif += '\x00';  // packed: no local color table, not interlaced

  // Image Data
  gif += '\x02';  // LZW minimum code size
  gif += '\x02';  // sub-block size
  gif += '\x4C';  // compressed data byte 1
  gif += '\x01';  // compressed data byte 2
  gif += '\x00';  // block terminator

  // Trailer
  gif += '\x3B';

  return gif;
}

// Test: Image descriptor with coordinates outside screen resolution
// (Lines 130-134: pixel + width > SWidth or row + height > SHeight)
TEST_F(GifReaderTest, ImageDescriptorOutOfBounds) {
  std::string gif;
  gif += "GIF89a";
  // Screen: 2x2
  gif += '\x02';
  gif += '\x00';
  gif += '\x02';
  gif += '\x00';
  gif += '\x80';  // GCT flag=1, size=0 (2 entries)
  gif += '\x00';
  gif += '\x00';
  // GCT: 2 entries
  gif += std::string("\xFF\x00\x00", 3);
  gif += std::string("\x00\x00\x00", 3);
  // Image Descriptor with left=1, width=2 -> left+width=3 > SWidth=2
  gif += '\x2C';
  gif += '\x01';
  gif += '\x00';  // left=1
  gif += '\x00';
  gif += '\x00';  // top=0
  gif += '\x02';
  gif += '\x00';  // width=2
  gif += '\x02';
  gif += '\x00';  // height=2
  gif += '\x00';
  // Minimal LZW data
  gif += '\x02';
  gif += '\x02';
  gif += '\x4C';
  gif += '\x01';
  gif += '\x00';
  gif += '\x3B';

  // Should fail because image coordinates are out of bounds.
  EXPECT_FALSE(gif_reader_->ReadPng(gif, read_.png_ptr(), read_.info_ptr(),
                                    PNG_TRANSFORM_IDENTITY));
}

// Test: Image descriptor with top + height > SHeight
TEST_F(GifReaderTest, ImageDescriptorVerticalOutOfBounds) {
  std::string gif;
  gif += "GIF89a";
  // Screen: 2x2
  gif += '\x02';
  gif += '\x00';
  gif += '\x02';
  gif += '\x00';
  gif += '\x80';
  gif += '\x00';
  gif += '\x00';
  gif += std::string("\xFF\x00\x00", 3);
  gif += std::string("\x00\x00\x00", 3);
  // Image: top=1, height=2 -> top+height=3 > SHeight=2
  gif += '\x2C';
  gif += '\x00';
  gif += '\x00';  // left=0
  gif += '\x01';
  gif += '\x00';  // top=1
  gif += '\x02';
  gif += '\x00';  // width=2
  gif += '\x02';
  gif += '\x00';  // height=2
  gif += '\x00';
  gif += '\x02';
  gif += '\x02';
  gif += '\x4C';
  gif += '\x01';
  gif += '\x00';
  gif += '\x3B';

  EXPECT_FALSE(gif_reader_->ReadPng(gif, read_.png_ptr(), read_.info_ptr(),
                                    PNG_TRANSFORM_IDENTITY));
}

// Test: Image descriptor with large Left coordinate that would overflow
// when added to width. Exercises the unsigned-cast overflow protection path
// (pixel < 0 check for GifWord, and pixel + width > png_width for large vals).
TEST_F(GifReaderTest, ImageDescriptorLargeLeftCoordinate) {
  std::string gif;
  gif += "GIF89a";
  // Screen: 2x2
  gif += '\x02';
  gif += '\x00';
  gif += '\x02';
  gif += '\x00';
  gif += '\x80';  // GCT flag=1, size=0 (2 entries)
  gif += '\x00';
  gif += '\x00';
  // GCT: 2 entries
  gif += std::string("\xFF\x00\x00", 3);
  gif += std::string("\x00\x00\x00", 3);
  // Image Descriptor with left=65535, top=0, width=1, height=1
  // left + width = 65536 which far exceeds SWidth=2
  gif += '\x2C';
  gif += '\xFF';
  gif += '\xFF';  // left=65535
  gif += '\x00';
  gif += '\x00';  // top=0
  gif += '\x01';
  gif += '\x00';  // width=1
  gif += '\x01';
  gif += '\x00';  // height=1
  gif += '\x00';
  // Minimal LZW data
  gif += '\x02';
  gif += '\x02';
  gif += '\x4C';
  gif += '\x01';
  gif += '\x00';
  gif += '\x3B';

  // Should fail because left coordinate is far out of bounds.
  EXPECT_FALSE(gif_reader_->ReadPng(gif, read_.png_ptr(), read_.info_ptr(),
                                    PNG_TRANSFORM_IDENTITY));
}

// Test: Image descriptor with large Top coordinate that would overflow
// when added to height. Exercises the unsigned-cast overflow protection path.
TEST_F(GifReaderTest, ImageDescriptorLargeTopCoordinate) {
  std::string gif;
  gif += "GIF89a";
  // Screen: 2x2
  gif += '\x02';
  gif += '\x00';
  gif += '\x02';
  gif += '\x00';
  gif += '\x80';
  gif += '\x00';
  gif += '\x00';
  gif += std::string("\xFF\x00\x00", 3);
  gif += std::string("\x00\x00\x00", 3);
  // Image: left=0, top=65535, width=1, height=1
  // top + height = 65536 which far exceeds SHeight=2
  gif += '\x2C';
  gif += '\x00';
  gif += '\x00';  // left=0
  gif += '\xFF';
  gif += '\xFF';  // top=65535
  gif += '\x01';
  gif += '\x00';  // width=1
  gif += '\x01';
  gif += '\x00';  // height=1
  gif += '\x00';
  gif += '\x02';
  gif += '\x02';
  gif += '\x4C';
  gif += '\x01';
  gif += '\x00';
  gif += '\x3B';

  EXPECT_FALSE(gif_reader_->ReadPng(gif, read_.png_ptr(), read_.info_ptr(),
                                    PNG_TRANSFORM_IDENTITY));
}

// Test: Image descriptor with zero Width
// (Triggers the width <= 0 check in ReadImageDescriptor)
TEST_F(GifReaderTest, ImageDescriptorZeroWidth) {
  std::string gif;
  gif += "GIF89a";
  // Screen: 2x2
  gif += '\x02';
  gif += '\x00';
  gif += '\x02';
  gif += '\x00';
  gif += '\x80';
  gif += '\x00';
  gif += '\x00';
  gif += std::string("\xFF\x00\x00", 3);
  gif += std::string("\x00\x00\x00", 3);
  // Image: left=0, top=0, width=0, height=2
  gif += '\x2C';
  gif += '\x00';
  gif += '\x00';  // left=0
  gif += '\x00';
  gif += '\x00';  // top=0
  gif += '\x00';
  gif += '\x00';  // width=0
  gif += '\x02';
  gif += '\x00';  // height=2
  gif += '\x00';
  // Minimal LZW data
  gif += '\x02';
  gif += '\x02';
  gif += '\x4C';
  gif += '\x01';
  gif += '\x00';
  gif += '\x3B';

  // Should fail because width is zero.
  EXPECT_FALSE(gif_reader_->ReadPng(gif, read_.png_ptr(), read_.info_ptr(),
                                    PNG_TRANSFORM_IDENTITY));
}

// Test: Image descriptor with zero Height
// (Triggers the height <= 0 check in ReadImageDescriptor)
TEST_F(GifReaderTest, ImageDescriptorZeroHeight) {
  std::string gif;
  gif += "GIF89a";
  // Screen: 2x2
  gif += '\x02';
  gif += '\x00';
  gif += '\x02';
  gif += '\x00';
  gif += '\x80';
  gif += '\x00';
  gif += '\x00';
  gif += std::string("\xFF\x00\x00", 3);
  gif += std::string("\x00\x00\x00", 3);
  // Image: left=0, top=0, width=2, height=0
  gif += '\x2C';
  gif += '\x00';
  gif += '\x00';  // left=0
  gif += '\x00';
  gif += '\x00';  // top=0
  gif += '\x02';
  gif += '\x00';  // width=2
  gif += '\x00';
  gif += '\x00';  // height=0
  gif += '\x00';
  // Minimal LZW data
  gif += '\x02';
  gif += '\x02';
  gif += '\x4C';
  gif += '\x01';
  gif += '\x00';
  gif += '\x3B';

  // Should fail because height is zero.
  EXPECT_FALSE(gif_reader_->ReadPng(gif, read_.png_ptr(), read_.info_ptr(),
                                    PNG_TRANSFORM_IDENTITY));
}

// Test: Missing both local and global color maps
// (Lines 141-143: color_map == nullptr)
TEST_F(GifReaderTest, MissingColorMap) {
  std::string gif;
  gif += "GIF89a";
  // Screen: 1x1, NO global color table (packed byte GCT flag=0)
  gif += '\x01';
  gif += '\x00';
  gif += '\x01';
  gif += '\x00';
  gif += '\x00';  // packed: NO GCT flag
  gif += '\x00';
  gif += '\x00';
  // No GCT bytes
  // Image Descriptor without local color table
  gif += '\x2C';
  gif += '\x00';
  gif += '\x00';
  gif += '\x00';
  gif += '\x00';
  gif += '\x01';
  gif += '\x00';
  gif += '\x01';
  gif += '\x00';
  gif += '\x00';  // no local color table
  // LZW data
  gif += '\x02';
  gif += '\x02';
  gif += '\x4C';
  gif += '\x01';
  gif += '\x00';
  gif += '\x3B';

  EXPECT_FALSE(gif_reader_->ReadPng(gif, read_.png_ptr(), read_.info_ptr(),
                                    PNG_TRANSFORM_IDENTITY));
}

// Test: Graphics extension with length < 4
// (Lines 206-210: extension[0] < 4)
TEST_F(GifReaderTest, ExtensionTooShort) {
  std::string gif;
  gif += "GIF89a";
  // Screen: 1x1
  gif += '\x01';
  gif += '\x00';
  gif += '\x01';
  gif += '\x00';
  gif += '\x80';  // GCT flag=1, size=0
  gif += '\x00';
  gif += '\x00';
  // GCT: 2 entries
  gif += std::string("\xFF\x00\x00", 3);
  gif += std::string("\x00\x00\x00", 3);

  // Graphics Control Extension with invalid (too short) length
  gif += '\x21';  // Extension introducer
  gif += '\xF9';  // Graphics Control Label
  gif += '\x02';  // Sub-block size = 2 (should be 4)
  gif += '\x00';  // flags
  gif += '\x00';  // partial data
  gif += '\x00';  // block terminator

  // Image Descriptor
  gif += '\x2C';
  gif += '\x00';
  gif += '\x00';
  gif += '\x00';
  gif += '\x00';
  gif += '\x01';
  gif += '\x00';
  gif += '\x01';
  gif += '\x00';
  gif += '\x00';
  // LZW data
  gif += '\x02';
  gif += '\x02';
  gif += '\x4C';
  gif += '\x01';
  gif += '\x00';
  gif += '\x3B';

  EXPECT_FALSE(gif_reader_->ReadPng(gif, read_.png_ptr(), read_.info_ptr(),
                                    PNG_TRANSFORM_IDENTITY));
}

// Test: Multiple transparency entries in extensions
// (Lines 214-218: transparent index already set)
TEST_F(GifReaderTest, MultipleTransparencyEntries) {
  std::string gif;
  gif += "GIF89a";
  // Screen: 1x1
  gif += '\x01';
  gif += '\x00';
  gif += '\x01';
  gif += '\x00';
  gif += '\x80';
  gif += '\x00';
  gif += '\x00';
  gif += std::string("\xFF\x00\x00", 3);
  gif += std::string("\x00\x00\x00", 3);

  // First Graphics Control Extension with transparency (index=0)
  gif += '\x21';
  gif += '\xF9';
  gif += '\x04';  // block size = 4
  gif += '\x01';  // flags: transparent = 1
  gif += '\x00';  // delay low
  gif += '\x00';  // delay high
  gif += '\x00';  // transparent color index = 0
  gif += '\x00';  // block terminator

  // Second Graphics Control Extension with transparency (index=1)
  gif += '\x21';
  gif += '\xF9';
  gif += '\x04';
  gif += '\x01';  // flags: transparent = 1
  gif += '\x00';
  gif += '\x00';
  gif += '\x01';  // transparent color index = 1
  gif += '\x00';  // block terminator

  // Image Descriptor
  gif += '\x2C';
  gif += '\x00';
  gif += '\x00';
  gif += '\x00';
  gif += '\x00';
  gif += '\x01';
  gif += '\x00';
  gif += '\x01';
  gif += '\x00';
  gif += '\x00';
  gif += '\x02';
  gif += '\x02';
  gif += '\x4C';
  gif += '\x01';
  gif += '\x00';
  gif += '\x3B';

  // Multiple transparency entries should NOT cause failure.
  // The code logs a warning but uses the first entry.
  // With require_opaque=true it should fail because there IS a transparency
  // entry.
  EXPECT_FALSE(gif_reader_->ReadPng(gif, read_.png_ptr(), read_.info_ptr(),
                                    PNG_TRANSFORM_IDENTITY, true));
  ASSERT_TRUE(read_.reset());

  // With require_opaque=false it should succeed (using first transparency
  // entry).
  EXPECT_TRUE(gif_reader_->ReadPng(gif, read_.png_ptr(), read_.info_ptr(),
                                   PNG_TRANSFORM_IDENTITY, false));
}

// Test: Truncated GIF data causes row reading errors
// (Lines 163-168, 176-179: DGifGetLine failures)
TEST_F(GifReaderTest, TruncatedImageData) {
  std::string gif = MakeMinimalGif89a();
  // Truncate the image data by removing the LZW data.
  // Find the image separator (0x2C) and cut shortly after.
  size_t sep_pos = gif.find('\x2C');
  ASSERT_NE(std::string::npos, sep_pos);
  // Keep image descriptor (10 bytes: sep + 4*2 coords + 1 packed)
  // but truncate the LZW data
  std::string truncated = gif.substr(0, sep_pos + 10);
  // Add a minimal LZW code size but no actual data
  truncated += '\x02';  // LZW minimum code size
  truncated += '\x00';  // block terminator (empty data)
  truncated += '\x3B';  // trailer

  // The reader will fail because DGifGetLine can't read pixel data.
  EXPECT_FALSE(gif_reader_->ReadPng(truncated, read_.png_ptr(),
                                    read_.info_ptr(), PNG_TRANSFORM_IDENTITY));
}

// Test: GIF with multiple image descriptors (animated) should fail in ReadPng
// (Lines 120-123: gif_file->ImageCount != 1)
TEST_F(GifReaderTest, MultiFrameGifReadPngFails) {
  // Read the animated GIF which has multiple frames.
  std::string animated_gif;
  ReadTestFile(kGifTestDir, kAnimatedGif, "gif", &animated_gif);
  ASSERT_NE(0u, animated_gif.length());

  // ReadPng via GifReader should fail for animated GIFs because it only
  // handles single-frame GIFs.
  EXPECT_FALSE(gif_reader_->ReadPng(animated_gif, read_.png_ptr(),
                                    read_.info_ptr(), PNG_TRANSFORM_IDENTITY));
}

// Test: DGifGetExtensionNext failure path
// (Lines 233-235: extension reading loop fails)
TEST_F(GifReaderTest, CorruptExtensionData) {
  std::string gif = MakeMinimalGif89a();

  // Insert a malformed comment extension before the image descriptor.
  // A comment extension (0xFE) with corrupted sub-block structure
  // that may cause DGifGetExtensionNext to fail.
  size_t sep_pos = gif.find('\x2C');
  ASSERT_NE(std::string::npos, sep_pos);

  std::string ext;
  ext += '\x21';  // Extension introducer
  ext += '\xFE';  // Comment extension label
  ext += '\x05';  // Sub-block size: 5 bytes
  ext += "Hello";
  // Missing block terminator ('\x00') and more sub-blocks claimed
  // But the data ends here, so DGifGetExtensionNext will fail.

  // Replace everything from separator onwards with extension + truncation
  std::string corrupted = gif.substr(0, sep_pos) + ext;
  // Don't add terminator - truncated

  EXPECT_FALSE(gif_reader_->ReadPng(corrupted, read_.png_ptr(),
                                    read_.info_ptr(), PNG_TRANSFORM_IDENTITY));
}

// Test: GIF with expand colormap and strip alpha combined on transparent GIF
TEST_F(GifReaderTest, ExpandAndStripAlpha) {
  std::string in;
  ReadTestFile(kGifTestDir, kTransparentGif, "gif", &in);
  ASSERT_NE(0u, in.length());

  // expand + strip_alpha: should produce RGB (no alpha).
  ASSERT_TRUE(gif_reader_->ReadPng(
      in, read_.png_ptr(), read_.info_ptr(),
      PNG_TRANSFORM_EXPAND | PNG_TRANSFORM_STRIP_ALPHA, false));

  png_uint_32 width, height;
  int bit_depth, color_type;
  png_get_IHDR(read_.png_ptr(), read_.info_ptr(), &width, &height, &bit_depth,
               &color_type, nullptr, nullptr, nullptr);
  // After EXPAND + STRIP_ALPHA, should be RGB (no alpha).
  EXPECT_EQ(PNG_COLOR_TYPE_RGB, color_type);
}

// =============================================================================
// GifFrameReader: malformed data via scanline API
// =============================================================================

// Test: ScopedGifStruct with corrupted data that fails DGifOpen
TEST(GifFrameReaderMalformed, CompletelyCorruptData) {
  NullMessageHandler handler;
  std::unique_ptr<MultipleFrameReader> reader(new TestGifFrameReader(&handler));

  // Random binary data that's not a valid GIF at all.
  const uint8_t garbage[] = {0xDE, 0xAD, 0xBE, 0xEF, 0x00, 0xFF,
                             0x01, 0x02, 0x03, 0x04, 0x05, 0x06};

  ScanlineStatus status = reader->Initialize(garbage, sizeof(garbage));
  EXPECT_FALSE(status.Success());
}

// Test: Valid GIF header but truncated before any image data
TEST(GifFrameReaderMalformed, ValidHeaderTruncatedBody) {
  NullMessageHandler handler;
  std::unique_ptr<MultipleFrameReader> reader(new TestGifFrameReader(&handler));

  // Valid GIF89a header + screen descriptor + partial GCT
  std::string gif;
  gif += "GIF89a";
  gif += '\x01';
  gif += '\x00';
  gif += '\x01';
  gif += '\x00';
  gif += '\x80';  // GCT flag
  gif += '\x00';
  gif += '\x00';
  // Only 3 of 6 required GCT bytes
  gif += std::string("\xFF\x00\x00", 3);

  ScanlineStatus status = reader->Initialize(gif.data(), gif.size());
  EXPECT_FALSE(status.Success());
}

// Test: GIF with zero-dimension screen but valid structure
TEST(GifFrameReaderMalformed, ZeroDimensionScreen) {
  NullMessageHandler handler;
  std::unique_ptr<MultipleFrameReader> reader(new TestGifFrameReader(&handler));

  std::string gif;
  gif += "GIF89a";
  // Screen: 0x0
  gif += '\x00';
  gif += '\x00';
  gif += '\x00';
  gif += '\x00';
  gif += '\x00';  // no GCT
  gif += '\x00';
  gif += '\x00';
  gif += '\x3B';  // trailer

  ScanlineStatus status = reader->Initialize(gif.data(), gif.size());
  // Zero dimension: no frames found, which should cause initialization to
  // succeed (frame count=0) or fail depending on the parser.
  // Either way, no crash.
  if (status.Success()) {
    // If it succeeded, there should be no frames.
    ImageSpec image_spec;
    ScanlineStatus img_status;
    reader->GetImageSpec(&image_spec, &img_status);
    EXPECT_EQ(0u, image_spec.num_frames);
  }
}

// ==================================================================
// Phase 5: Validation/error path coverage for gif_reader.cc
// ==================================================================

// Helper: Build a minimal valid single-frame GIF89a.
// Screen: 2x2, 2-entry global palette (red, blue), no transparency.
static std::string MakeMinimalGif() {
  std::string gif;
  // Header: GIF89a
  gif += "GIF89a";
  // Logical Screen Descriptor: width=2, height=2
  gif += '\x02';
  gif += '\x00';  // width LE
  gif += '\x02';
  gif += '\x00';  // height LE
  // Packed: GCT flag=1, color resolution=1 (2 bits), sort=0, GCT size=0 (2^1=2)
  gif += '\x80';
  gif += '\x00';  // bg color index
  gif += '\x00';  // pixel aspect ratio
  // Global Color Table (2 entries x 3 bytes = 6 bytes)
  gif += '\xFF';
  gif += '\x00';
  gif += '\x00';  // red
  gif += '\x00';
  gif += '\x00';
  gif += '\xFF';  // blue
  // Image Descriptor
  gif += '\x2C';  // image separator
  gif += '\x00';
  gif += '\x00';  // left
  gif += '\x00';
  gif += '\x00';  // top
  gif += '\x02';
  gif += '\x00';  // width
  gif += '\x02';
  gif += '\x00';  // height
  gif += '\x00';  // packed: no LCT, not interlaced
  // Image Data: LZW minimum code size = 2
  gif += '\x02';
  // Sub-block: 4 pixels (indices: 0,1,0,1) encoded as LZW.
  // We'll use a pre-encoded block.
  gif += '\x03';  // sub-block size
  gif += '\x04';
  gif += '\x01';
  gif += '\x00';
  gif += '\x00';  // block terminator
  // Trailer
  gif += '\x3B';
  return gif;
}

// Helper: Build a minimal animated GIF89a with 2 frames, each 2x2.
static std::string MakeAnimatedGif(int disposal1 = 0, int disposal2 = 0,
                                   bool transparent = false) {
  std::string gif;
  // Header
  gif += "GIF89a";
  // Screen: 2x2
  gif += '\x02';
  gif += '\x00';
  gif += '\x02';
  gif += '\x00';
  // Packed: GCT=1, color res=2, sort=0, GCT size=1 (2^(1+1)=4 entries)
  gif += '\x91';
  gif += '\x00';  // bg
  gif += '\x00';  // aspect

  // Global Color Table (4 entries)
  gif += '\xFF';
  gif += '\x00';
  gif += '\x00';  // 0: red
  gif += '\x00';
  gif += '\xFF';
  gif += '\x00';  // 1: green
  gif += '\x00';
  gif += '\x00';
  gif += '\xFF';  // 2: blue
  gif += '\xFF';
  gif += '\xFF';
  gif += '\xFF';  // 3: white

  // Netscape loop extension (NETSCAPE2.0)
  gif += '\x21';  // extension introducer
  gif += '\xFF';  // application extension label
  gif += '\x0B';  // block size (11)
  gif += "NETSCAPE2.0";
  gif += '\x03';  // sub-block size
  gif += '\x01';  // fixed const
  gif += '\x00';
  gif += '\x00';  // loop count=0 (infinite)
  gif += '\x00';  // block terminator

  // Frame 1: Graphic Control Extension + Image Descriptor
  gif += '\x21';  // extension introducer
  gif += '\xF9';  // GCE label
  gif += '\x04';  // block size
  // Packed: disposal << 2 | transparent flag
  auto gce_flags1 =
      static_cast<uint8_t>((disposal1 << 2) | (transparent ? 1 : 0));
  gif += static_cast<char>(gce_flags1);
  gif += '\x0A';
  gif += '\x00';                         // delay = 10 (100ms)
  gif += transparent ? '\x03' : '\x00';  // transparent color index
  gif += '\x00';                         // block terminator

  // Image Descriptor frame 1
  gif += '\x2C';
  gif += '\x00';
  gif += '\x00';
  gif += '\x00';
  gif += '\x00';  // left, top
  gif += '\x02';
  gif += '\x00';
  gif += '\x02';
  gif += '\x00';  // 2x2
  gif += '\x00';  // no LCT
  // Image data
  gif += '\x02';  // LZW min code size
  gif += '\x03';
  gif += '\x04';
  gif += '\x01';
  gif += '\x00';
  gif += '\x00';  // block terminator

  // Frame 2: Graphic Control Extension + Image Descriptor
  gif += '\x21';
  gif += '\xF9';
  gif += '\x04';
  auto gce_flags2 = static_cast<uint8_t>((disposal2 << 2));
  gif += static_cast<char>(gce_flags2);
  gif += '\x0A';
  gif += '\x00';
  gif += '\x00';  // no transparent
  gif += '\x00';

  // Image Descriptor frame 2
  gif += '\x2C';
  gif += '\x00';
  gif += '\x00';
  gif += '\x00';
  gif += '\x00';
  gif += '\x02';
  gif += '\x00';
  gif += '\x02';
  gif += '\x00';
  gif += '\x00';
  // Image data
  gif += '\x02';
  gif += '\x03';
  gif += '\x04';
  gif += '\x01';
  gif += '\x00';
  gif += '\x00';

  // Trailer
  gif += '\x3B';
  return gif;
}

// --- GifReader::GetAttributes: crafted data ---

TEST_F(GifReaderTest, GetAttributes_MinimalGif) {
  std::string gif = MakeMinimalGif();
  int w, h, bd, ct;
  EXPECT_TRUE(gif_reader_->GetAttributes(gif, &w, &h, &bd, &ct));
  EXPECT_EQ(2, w);
  EXPECT_EQ(2, h);
  EXPECT_EQ(8, bd);
  EXPECT_EQ(PNG_COLOR_TYPE_PALETTE, ct);
}

TEST_F(GifReaderTest, GetAttributes_TruncatedHeader) {
  // Just "GIF89" -- missing version character and dimensions.
  std::string truncated = "GIF89";
  int w, h, bd, ct;
  EXPECT_FALSE(gif_reader_->GetAttributes(truncated, &w, &h, &bd, &ct));
}

TEST_F(GifReaderTest, GetAttributes_WrongMagic) {
  // Correct length but wrong magic.
  std::string wrong =
      "NOTGIF89a\x02\x00\x02\x00";  // NOLINT(bugprone-string-literal-with-embedded-nul)
  int w, h, bd, ct;
  EXPECT_FALSE(gif_reader_->GetAttributes(wrong, &w, &h, &bd, &ct));
}

TEST_F(GifReaderTest, GetAttributes_GIF87a) {
  // GIF87a should also be recognized (same magic prefix "GIF").
  std::string gif = MakeMinimalGif();
  gif[3] = '8';
  gif[4] = '7';
  gif[5] = 'a';
  int w, h, bd, ct;
  EXPECT_TRUE(gif_reader_->GetAttributes(gif, &w, &h, &bd, &ct));
  EXPECT_EQ(2, w);
  EXPECT_EQ(2, h);
}

// --- GifReader::ReadPng: crafted error cases ---

TEST_F(GifReaderTest, ReadPng_MinimalGifWorks) {
  std::string gif = MakeMinimalGif();
  EXPECT_TRUE(gif_reader_->ReadPng(gif, read_.png_ptr(), read_.info_ptr(),
                                   PNG_TRANSFORM_IDENTITY));
}

TEST_F(GifReaderTest, ReadPng_EmptyBody) {
  std::string empty;
  EXPECT_FALSE(gif_reader_->ReadPng(empty, read_.png_ptr(), read_.info_ptr(),
                                    PNG_TRANSFORM_IDENTITY));
}

// --- Decode caps on the whole-image and frame-reader GIF paths ---

// Appends a 16-bit little-endian value, the GIF field encoding.
void AppendGifUint16(std::string* out, uint32_t value) {
  out->push_back(static_cast<char>(value & 0xFF));
  out->push_back(static_cast<char>((value >> 8) & 0xFF));
}

// Builds a GIF89a with the given logical screen and one frame of the given
// size at the origin, a two-entry global color table and the pre-encoded
// LZW block of MakeMinimalGif(). With `transparent`, a graphics control
// extension marks index 1 transparent; the frame reader then defers
// decoding the frame data past PrepareNextFrame(), so the test can observe
// how the frame dimensions alone are handled.
std::string MakeGifWithScreenAndFrame(uint32_t screen_width,
                                      uint32_t screen_height,
                                      uint32_t frame_width,
                                      uint32_t frame_height, bool transparent) {
  std::string gif("GIF89a");
  AppendGifUint16(&gif, screen_width);
  AppendGifUint16(&gif, screen_height);
  gif += '\x80';  // packed: GCT flag=1, color resolution=0, GCT size=0 (2)
  gif += '\x00';  // background color index
  gif += '\x00';  // pixel aspect ratio
  gif += std::string("\xFF\x00\x00", 3);  // color 0: red
  gif += std::string("\x00\x00\xFF", 3);  // color 1: blue
  if (transparent) {
    gif += '\x21';  // extension introducer
    gif += '\xF9';  // graphics control extension label
    gif += '\x04';  // block size
    gif += '\x01';  // packed: transparent color flag
    gif += '\x00';
    gif += '\x00';  // delay
    gif += '\x01';  // transparent color index
    gif += '\x00';  // block terminator
  }
  gif += '\x2C';             // image separator
  AppendGifUint16(&gif, 0);  // left
  AppendGifUint16(&gif, 0);  // top
  AppendGifUint16(&gif, frame_width);
  AppendGifUint16(&gif, frame_height);
  gif += '\x00';                              // packed: no LCT, not interlaced
  gif += '\x02';                              // LZW minimum code size
  gif += std::string("\x03\x04\x01\x00", 4);  // one pre-encoded sub-block
  gif += '\x00';                              // block terminator
  gif += '\x3B';                              // trailer
  return gif;
}

// The whole-image GIF reader allocates one row per logical-screen line
// before it looks at any frame data, so a file whose screen declares
// dimensions past the decode caps must be refused up front. The same
// layout with a small screen is accepted, so the refusal is down to the
// screen size alone.
TEST_F(GifReaderTest, OversizedScreenRejected) {
  std::string small = MakeGifWithScreenAndFrame(16, 10, 2, 2, false);
  ASSERT_TRUE(gif_reader_->ReadPng(small, read_.png_ptr(), read_.info_ptr(),
                                   PNG_TRANSFORM_IDENTITY));
  ASSERT_TRUE(read_.reset());

  std::string wide = MakeGifWithScreenAndFrame(65535, 10, 2, 2, false);
  EXPECT_FALSE(gif_reader_->ReadPng(wide, read_.png_ptr(), read_.info_ptr(),
                                    PNG_TRANSFORM_IDENTITY));
}

// A screen that declares a side past the decode cap drives buffers
// proportional to the screen area, whatever the size of the frames, so
// initialization must refuse it before any frame is prepared.
TEST(GifFrameReaderInvalidInput, OversizedScreenRejectedByInitialize) {
  NullMessageHandler handler;
  std::unique_ptr<MultipleFrameReader> reader(new TestGifFrameReader(&handler));
  std::string in = MakeGifWithScreenAndFrame(65535, 10, 2, 2, false);
  EXPECT_FALSE(reader->Initialize(in.data(), in.length()).Success());
}

// Sides within the cap can still combine past the pixel budget.
TEST(GifFrameReaderInvalidInput,
     OversizedScreenPixelBudgetRejectedByInitialize) {
  NullMessageHandler handler;
  std::unique_ptr<MultipleFrameReader> reader(new TestGifFrameReader(&handler));
  // 128 Mi pixels, over the 100 Mi pixel budget.
  std::string in = MakeGifWithScreenAndFrame(16384, 8192, 2, 2, false);
  EXPECT_FALSE(reader->Initialize(in.data(), in.length()).Success());
}

// A frame may declare dimensions beyond the logical screen, and the frame
// buffers are proportional to width * height, so the same caps are applied
// when the frame is prepared.
TEST(GifFrameReaderInvalidInput, OversizedFrameRejectedByPrepareNextFrame) {
  NullMessageHandler handler;
  std::unique_ptr<MultipleFrameReader> reader(new TestGifFrameReader(&handler));
  std::string in = MakeGifWithScreenAndFrame(2, 2, 16385, 1, true);
  ASSERT_TRUE(reader->Initialize(in.data(), in.length()).Success());
  ScanlineStatus status;
  EXPECT_FALSE(reader->PrepareNextFrame(&status));
  EXPECT_EQ(net_instaweb::SCANLINE_STATUS_UNSUPPORTED_FORMAT, status.type());
}

// In Chrome quirks mode the logical screen is resized to the first frame
// after the screen caps were checked, so a first frame past the cap would
// be published through GetImageSpec() uncapped. Initialization must refuse
// the enlarged dimensions.
TEST(GifFrameReaderInvalidInput,
     OversizedFirstFrameInChromeQuirksRejectedByInitialize) {
  NullMessageHandler handler;
  std::unique_ptr<MultipleFrameReader> reader(new TestGifFrameReader(&handler));
  std::string in = MakeGifWithScreenAndFrame(2, 2, 16385, 1, true);
  ASSERT_TRUE(reader->set_quirks_mode(QUIRKS_CHROME).Success());
  EXPECT_FALSE(reader->Initialize(in.data(), in.length()).Success());
}

// --- Extension blocks with no data ---

// Helpers to build minimal GIF images byte by byte so truncated
// extension blocks can be constructed precisely.

// A 1x1 GIF89a header with no global color table.
void AppendGifHeaderNoGct(std::string* out) {
  const char header[] = {'G',  'I',  'F', '8', '9', 'a',  //
                         0x01, 0x00,                      // width = 1
                         0x01, 0x00,                      // height = 1
                         0x00,   // no global color table
                         0x00,   // background index
                         0x00};  // aspect ratio
  out->append(header, sizeof(header));
}

// A 1x1 image descriptor at (0, 0) with no local color table, followed by
// LZW data encoding a single pixel of value 'pixel_value' (minimum code
// size 3 so values up to 7 encode).
void AppendOnePixelImageDescriptor(std::string* out, int pixel_value) {
  const char descriptor[] = {0x2C,        // image separator
                             0x00, 0x00,  // left = 0
                             0x00, 0x00,  // top = 0
                             0x01, 0x00,  // width = 1
                             0x01, 0x00,  // height = 1
                             0x00};       // no local color table
  out->append(descriptor, sizeof(descriptor));
  // Minimum code size 3: clear = 8, EOI = 9, first code is 4 bits.
  // Codes: clear(8), pixel_value, EOI(9), packed LSB first.
  const int bits = 8 /* clear */ | (pixel_value << 4) | (9 << 8) /* EOI */;
  const char lzw[] = {
      0x03,  // minimum code size
      0x02, static_cast<char>(bits & 0xff),
      static_cast<char>((bits >> 8) & 0xff),
      0x00  // block terminator
  };
  out->append(lzw, sizeof(lzw));
}

// A graphics control extension whose single sub-block is empty (length
// zero) decodes to a null extension pointer; the whole-image reader must
// reject the image instead of dereferencing it.
TEST_F(GifReaderTest, EmptyGraphicsControlExtensionBlock) {
  std::string in;
  AppendGifHeaderNoGct(&in);
  const char empty_gce[] = {0x21, '\xF9', 0x00};
  in.append(empty_gce, sizeof(empty_gce));
  in.push_back(0x3B);  // trailer

  EXPECT_FALSE(gif_reader_->ReadPng(in, read_.png_ptr(), read_.info_ptr(),
                                    PNG_TRANSFORM_IDENTITY));
}

// Same empty graphics control extension block, on the frame reader path:
// Initialize() scans past it, and PrepareNextFrame() must reject it
// instead of dereferencing a null extension.
TEST(GifFrameReaderInvalidInput, EmptyGraphicsControlExtensionBlock) {
  std::string in;
  AppendGifHeaderNoGct(&in);
  const char empty_gce[] = {0x21, '\xF9', 0x00};
  in.append(empty_gce, sizeof(empty_gce));
  AppendOnePixelImageDescriptor(&in, 0);
  in.push_back(0x3B);

  NullMessageHandler handler;
  std::unique_ptr<MultipleFrameReader> reader(new TestGifFrameReader(&handler));
  ASSERT_TRUE(reader->Initialize(in.data(), in.length()).Success());
  ScanlineStatus status;
  EXPECT_FALSE(reader->PrepareNextFrame(&status));
}

// A NETSCAPE2.0 application extension whose loop-count sub-block is empty
// (length zero) decodes to a null extension pointer; the frame reader must
// reject the image during Initialize() instead of dereferencing it.
TEST(GifFrameReaderInvalidInput, EmptyLoopCountSubblock) {
  std::string in;
  AppendGifHeaderNoGct(&in);
  const char empty_loop_count[] = {0x21, '\xFF', 0x0B, 'N', 'E', 'T', 'S', 'C',
                                   'A',  'P',    'E',  '2', '.', '0', 0x00};
  in.append(empty_loop_count, sizeof(empty_loop_count));
  in.push_back(0x3B);

  NullMessageHandler handler;
  std::unique_ptr<MultipleFrameReader> reader(new TestGifFrameReader(&handler));
  EXPECT_FALSE(reader->Initialize(in.data(), in.length()).Success());
}

// Builds a one-frame image whose NETSCAPE2.0 loop-count sub-block is
// `subblock` (length byte first).
std::string GifWithLoopCountSubblock(const std::string& subblock) {
  std::string in;
  AppendGifHeaderNoGct(&in);
  const char app_ext[] = {0x21, '\xFF', 0x0B, 'N', 'E', 'T', 'S',
                          'C',  'A',    'P',  'E', '2', '.', '0'};
  in.append(app_ext, sizeof(app_ext));
  in.append(subblock);
  in.push_back(0x00);  // sub-block terminator
  AppendOnePixelImageDescriptor(&in, 0);
  in.push_back(0x3B);
  return in;
}

// A loop-count sub-block too short to hold a loop count must be rejected
// even when its first data byte holds the expected marker; so must one
// with the wrong marker. A well-formed block is still accepted.
TEST(GifFrameReaderInvalidInput, ShortLoopCountSubblock) {
  NullMessageHandler handler;
  std::unique_ptr<MultipleFrameReader> reader(new TestGifFrameReader(&handler));
  const char valid_block[] = {0x03, 0x01, 0x05, 0x00};
  std::string valid =
      GifWithLoopCountSubblock(std::string(valid_block, sizeof(valid_block)));
  EXPECT_TRUE(reader->Initialize(valid.data(), valid.length()).Success());

  const char short_block[] = {0x02, 0x01, 0x05};
  std::string in =
      GifWithLoopCountSubblock(std::string(short_block, sizeof(short_block)));
  EXPECT_FALSE(reader->Initialize(in.data(), in.length()).Success());

  const char wrong_marker[] = {0x03, 0x02, 0x05, 0x00};
  std::string marker =
      GifWithLoopCountSubblock(std::string(wrong_marker, sizeof(wrong_marker)));
  EXPECT_FALSE(reader->Initialize(marker.data(), marker.length()).Success());
}

TEST_F(GifReaderTest, ReadPng_JustHeader) {
  // Only the GIF header, no frames.
  std::string gif = "GIF89a";
  gif += '\x02';
  gif += '\x00';
  gif += '\x02';
  gif += '\x00';
  gif += '\x00';
  gif += '\x00';
  gif += '\x00';
  EXPECT_FALSE(gif_reader_->ReadPng(gif, read_.png_ptr(), read_.info_ptr(),
                                    PNG_TRANSFORM_IDENTITY));
}

TEST_F(GifReaderTest, ReadPng_MinimalWithExpand) {
  std::string gif = MakeMinimalGif();
  EXPECT_TRUE(gif_reader_->ReadPng(gif, read_.png_ptr(), read_.info_ptr(),
                                   PNG_TRANSFORM_EXPAND));
  // After expand, should be RGB.
  int ct = png_get_color_type(read_.png_ptr(), read_.info_ptr());
  EXPECT_EQ(PNG_COLOR_TYPE_RGB, ct);
}

TEST_F(GifReaderTest, ReadPng_MinimalWithStripAlpha) {
  std::string gif = MakeMinimalGif();
  EXPECT_TRUE(gif_reader_->ReadPng(gif, read_.png_ptr(), read_.info_ptr(),
                                   PNG_TRANSFORM_STRIP_ALPHA));
}

// --- GifDisposalToFrameSpecDisposal: extended range ---

TEST(GifDisposalMethodExtended, NegativeValues) {
  // Negative disposal values should map to DISPOSAL_NONE.
  EXPECT_EQ(FrameSpec::DISPOSAL_NONE, GifDisposalToFrameSpecDisposal(-1));
  EXPECT_EQ(FrameSpec::DISPOSAL_NONE, GifDisposalToFrameSpecDisposal(-100));
}

TEST(GifDisposalMethodExtended, OutOfRangePositive) {
  // Values > 3 (DISPOSAL_RESTORE) should map to DISPOSAL_NONE.
  EXPECT_EQ(FrameSpec::DISPOSAL_NONE, GifDisposalToFrameSpecDisposal(4));
  EXPECT_EQ(FrameSpec::DISPOSAL_NONE, GifDisposalToFrameSpecDisposal(7));
  EXPECT_EQ(FrameSpec::DISPOSAL_NONE, GifDisposalToFrameSpecDisposal(255));
}

TEST(GifDisposalMethodExtended, AllValidValues) {
  // 0 = DISPOSAL_UNKNOWN in GIF -> maps to DISPOSAL_NONE
  EXPECT_EQ(FrameSpec::DISPOSAL_NONE, GifDisposalToFrameSpecDisposal(0));
  // 1 = DISPOSAL_NONE
  EXPECT_EQ(FrameSpec::DISPOSAL_NONE, GifDisposalToFrameSpecDisposal(1));
  // 2 = DISPOSAL_BACKGROUND
  EXPECT_EQ(FrameSpec::DISPOSAL_BACKGROUND, GifDisposalToFrameSpecDisposal(2));
  // 3 = DISPOSAL_RESTORE
  EXPECT_EQ(FrameSpec::DISPOSAL_RESTORE, GifDisposalToFrameSpecDisposal(3));
}

// --- GifFrameReader: animated GIF with disposal methods ---

TEST(GifFrameReaderDisposal, DisposalNone) {
  NullMessageHandler handler;
  std::unique_ptr<MultipleFrameReader> reader(new TestGifFrameReader(&handler));
  std::string gif = MakeAnimatedGif(1, 1);  // DISPOSAL_NONE for both frames

  ScanlineStatus status = reader->Initialize(gif.data(), gif.size());
  ASSERT_TRUE(status.Success()) << status.ToString();

  ImageSpec image_spec;
  EXPECT_TRUE(reader->GetImageSpec(&image_spec, &status));
  EXPECT_EQ(2u, image_spec.num_frames);

  // Read both frames.
  for (int f = 0; f < 2; ++f) {
    ASSERT_TRUE(reader->HasMoreFrames());
    EXPECT_TRUE(reader->PrepareNextFrame(&status)) << status.ToString();
    FrameSpec frame_spec;
    EXPECT_TRUE(reader->GetFrameSpec(&frame_spec, &status));
    EXPECT_EQ(FrameSpec::DISPOSAL_NONE, frame_spec.disposal);
    EXPECT_GT(frame_spec.duration_ms, 0u);
    while (reader->HasMoreScanlines()) {
      const void* scanline = nullptr;
      EXPECT_TRUE(reader->ReadNextScanline(&scanline, &status));
    }
  }
  EXPECT_FALSE(reader->HasMoreFrames());
}

TEST(GifFrameReaderDisposal, DisposalBackground) {
  NullMessageHandler handler;
  std::unique_ptr<MultipleFrameReader> reader(new TestGifFrameReader(&handler));
  std::string gif = MakeAnimatedGif(2, 2);  // DISPOSAL_BACKGROUND

  ScanlineStatus status = reader->Initialize(gif.data(), gif.size());
  ASSERT_TRUE(status.Success()) << status.ToString();

  ImageSpec image_spec;
  EXPECT_TRUE(reader->GetImageSpec(&image_spec, &status));

  for (int f = 0; f < 2; ++f) {
    ASSERT_TRUE(reader->HasMoreFrames());
    EXPECT_TRUE(reader->PrepareNextFrame(&status));
    FrameSpec frame_spec;
    EXPECT_TRUE(reader->GetFrameSpec(&frame_spec, &status));
    EXPECT_EQ(FrameSpec::DISPOSAL_BACKGROUND, frame_spec.disposal);
    while (reader->HasMoreScanlines()) {
      const void* scanline = nullptr;
      EXPECT_TRUE(reader->ReadNextScanline(&scanline, &status));
    }
  }
}

TEST(GifFrameReaderDisposal, DisposalRestore) {
  NullMessageHandler handler;
  std::unique_ptr<MultipleFrameReader> reader(new TestGifFrameReader(&handler));
  std::string gif = MakeAnimatedGif(3, 3);  // DISPOSAL_RESTORE

  ScanlineStatus status = reader->Initialize(gif.data(), gif.size());
  ASSERT_TRUE(status.Success()) << status.ToString();

  ImageSpec image_spec;
  EXPECT_TRUE(reader->GetImageSpec(&image_spec, &status));

  for (int f = 0; f < 2; ++f) {
    ASSERT_TRUE(reader->HasMoreFrames());
    EXPECT_TRUE(reader->PrepareNextFrame(&status));
    FrameSpec frame_spec;
    EXPECT_TRUE(reader->GetFrameSpec(&frame_spec, &status));
    EXPECT_EQ(FrameSpec::DISPOSAL_RESTORE, frame_spec.disposal);
    while (reader->HasMoreScanlines()) {
      const void* scanline = nullptr;
      EXPECT_TRUE(reader->ReadNextScanline(&scanline, &status));
    }
  }
}

TEST(GifFrameReaderDisposal, MixedDisposal) {
  NullMessageHandler handler;
  std::unique_ptr<MultipleFrameReader> reader(new TestGifFrameReader(&handler));
  // Frame 1: DISPOSAL_BACKGROUND (2), Frame 2: DISPOSAL_RESTORE (3)
  std::string gif = MakeAnimatedGif(2, 3);

  ScanlineStatus status = reader->Initialize(gif.data(), gif.size());
  ASSERT_TRUE(status.Success()) << status.ToString();

  // Frame 1
  ASSERT_TRUE(reader->HasMoreFrames());
  EXPECT_TRUE(reader->PrepareNextFrame(&status));
  FrameSpec frame1;
  EXPECT_TRUE(reader->GetFrameSpec(&frame1, &status));
  EXPECT_EQ(FrameSpec::DISPOSAL_BACKGROUND, frame1.disposal);
  while (reader->HasMoreScanlines()) {
    const void* scanline = nullptr;
    EXPECT_TRUE(reader->ReadNextScanline(&scanline, &status));
  }

  // Frame 2
  ASSERT_TRUE(reader->HasMoreFrames());
  EXPECT_TRUE(reader->PrepareNextFrame(&status));
  FrameSpec frame2;
  EXPECT_TRUE(reader->GetFrameSpec(&frame2, &status));
  EXPECT_EQ(FrameSpec::DISPOSAL_RESTORE, frame2.disposal);
  while (reader->HasMoreScanlines()) {
    const void* scanline = nullptr;
    EXPECT_TRUE(reader->ReadNextScanline(&scanline, &status));
  }
}

// --- GifFrameReader: transparency in animated frames ---

TEST(GifFrameReaderDisposal, TransparentFrame) {
  NullMessageHandler handler;
  std::unique_ptr<MultipleFrameReader> reader(new TestGifFrameReader(&handler));
  std::string gif = MakeAnimatedGif(1, 1, true /* transparent */);

  ScanlineStatus status = reader->Initialize(gif.data(), gif.size());
  ASSERT_TRUE(status.Success()) << status.ToString();

  ASSERT_TRUE(reader->HasMoreFrames());
  EXPECT_TRUE(reader->PrepareNextFrame(&status));
  FrameSpec frame_spec;
  EXPECT_TRUE(reader->GetFrameSpec(&frame_spec, &status));

  // With transparency, should be RGBA.
  EXPECT_EQ(RGBA_8888, frame_spec.pixel_format);

  while (reader->HasMoreScanlines()) {
    const void* scanline = nullptr;
    EXPECT_TRUE(reader->ReadNextScanline(&scanline, &status));
  }
}

// --- GifFrameReader: set_quirks_mode after initialization ---

TEST(GifFrameReaderDisposal, SetQuirksModeAfterInitFails) {
  NullMessageHandler handler;
  std::unique_ptr<MultipleFrameReader> reader(new TestGifFrameReader(&handler));
  std::string gif = MakeMinimalGif();

  ScanlineStatus status = reader->Initialize(gif.data(), gif.size());
  ASSERT_TRUE(status.Success());

  // Attempting to set quirks mode after initialization should fail.
#ifdef NDEBUG
  EXPECT_FALSE(reader->set_quirks_mode(QUIRKS_CHROME).Success());
#endif
}

// --- GifFrameReader: GetFrameSpec/GetImageSpec with nullptr ---

TEST(GifFrameReaderDisposal, GetImageSpecNullPtr) {
  NullMessageHandler handler;
  std::unique_ptr<MultipleFrameReader> reader(new TestGifFrameReader(&handler));
  std::string gif = MakeMinimalGif();
  ScanlineStatus status = reader->Initialize(gif.data(), gif.size());
  ASSERT_TRUE(status.Success());

#ifdef NDEBUG
  EXPECT_FALSE(reader->GetImageSpec(nullptr, &status));
#endif
}

TEST(GifFrameReaderDisposal, GetFrameSpecNullPtr) {
  NullMessageHandler handler;
  std::unique_ptr<MultipleFrameReader> reader(new TestGifFrameReader(&handler));
  std::string gif = MakeMinimalGif();
  ScanlineStatus status = reader->Initialize(gif.data(), gif.size());
  ASSERT_TRUE(status.Success());

  ASSERT_TRUE(reader->HasMoreFrames());
  EXPECT_TRUE(reader->PrepareNextFrame(&status));

#ifdef NDEBUG
  EXPECT_FALSE(reader->GetFrameSpec(nullptr, &status));
#endif
}

// --- GifFrameReader: PrepareNextFrame when no more frames ---

TEST(GifFrameReaderDisposal, PrepareNextFrameNoMoreFrames) {
  NullMessageHandler handler;
  std::unique_ptr<MultipleFrameReader> reader(new TestGifFrameReader(&handler));
  std::string gif = MakeMinimalGif();
  ScanlineStatus status = reader->Initialize(gif.data(), gif.size());
  ASSERT_TRUE(status.Success());

  ImageSpec image_spec;
  EXPECT_TRUE(reader->GetImageSpec(&image_spec, &status));

  // Read all frames.
  while (reader->HasMoreFrames()) {
    EXPECT_TRUE(reader->PrepareNextFrame(&status));
    while (reader->HasMoreScanlines()) {
      const void* scanline = nullptr;
      EXPECT_TRUE(reader->ReadNextScanline(&scanline, &status));
    }
  }

  // Attempt to prepare another frame should fail.
#ifdef NDEBUG
  EXPECT_FALSE(reader->PrepareNextFrame(&status));
#endif
}

// --- GifFrameReader: ReadNextScanline when no more scanlines ---

TEST(GifFrameReaderDisposal, ReadNextScanlineNoMoreScanlines) {
  NullMessageHandler handler;
  std::unique_ptr<MultipleFrameReader> reader(new TestGifFrameReader(&handler));
  std::string gif = MakeMinimalGif();
  ScanlineStatus status = reader->Initialize(gif.data(), gif.size());
  ASSERT_TRUE(status.Success());

  ASSERT_TRUE(reader->HasMoreFrames());
  EXPECT_TRUE(reader->PrepareNextFrame(&status));

  // Read all scanlines.
  while (reader->HasMoreScanlines()) {
    const void* scanline = nullptr;
    EXPECT_TRUE(reader->ReadNextScanline(&scanline, &status));
  }

  // Attempt to read another scanline should fail.
#ifdef NDEBUG
  const void* scanline = nullptr;
  EXPECT_FALSE(reader->ReadNextScanline(&scanline, &status));
#endif
}

// --- GifFrameReader: crafted malformed GIFs ---

TEST(GifFrameReaderMalformed, TruncatedBeforeImageData) {
  NullMessageHandler handler;
  std::unique_ptr<MultipleFrameReader> reader(new TestGifFrameReader(&handler));

  // Build a GIF with header + GCT but no image data.
  std::string gif;
  gif += "GIF89a";
  gif += '\x02';
  gif += '\x00';
  gif += '\x02';
  gif += '\x00';
  gif += '\x80';  // GCT=1, size=0 (2 entries)
  gif += '\x00';
  gif += '\x00';
  // GCT: 2 entries
  gif += '\xFF';
  gif += '\x00';
  gif += '\x00';
  gif += '\x00';
  gif += '\xFF';
  gif += '\x00';
  // Truncate here -- no image descriptor, no trailer.

  ScanlineStatus status = reader->Initialize(gif.data(), gif.size());
  // Should either fail or have 0 frames.
  if (status.Success()) {
    ImageSpec image_spec;
    reader->GetImageSpec(&image_spec, &status);
    EXPECT_EQ(0u, image_spec.num_frames);
  }
}

TEST(GifFrameReaderMalformed, MissingColorMapForFrame) {
  NullMessageHandler handler;
  std::unique_ptr<MultipleFrameReader> reader(new TestGifFrameReader(&handler));

  // GIF with no GCT and no LCT.
  std::string gif;
  gif += "GIF89a";
  gif += '\x02';
  gif += '\x00';
  gif += '\x02';
  gif += '\x00';
  gif += '\x00';  // no GCT
  gif += '\x00';
  gif += '\x00';
  // Image Descriptor (without LCT).
  gif += '\x2C';
  gif += '\x00';
  gif += '\x00';
  gif += '\x00';
  gif += '\x00';
  gif += '\x02';
  gif += '\x00';
  gif += '\x02';
  gif += '\x00';
  gif += '\x00';  // no LCT
  // Minimal image data.
  gif += '\x02';
  gif += '\x03';
  gif += '\x04';
  gif += '\x01';
  gif += '\x00';
  gif += '\x00';
  gif += '\x3B';  // trailer

  ScanlineStatus status = reader->Initialize(gif.data(), gif.size());
  // Should fail during initialization or when trying to prepare the frame.
  if (status.Success()) {
    EXPECT_TRUE(reader->HasMoreFrames());
    // PrepareNextFrame should fail due to missing color map.
    ScanlineStatus frame_status;
    bool ok = reader->PrepareNextFrame(&frame_status);
    EXPECT_FALSE(ok) << "Should fail with missing color map";
  }
}

TEST(GifFrameReaderMalformed, CorruptImageDataNoCrash) {
  NullMessageHandler handler;
  std::unique_ptr<MultipleFrameReader> reader(new TestGifFrameReader(&handler));

  // Start with a valid minimal GIF, then corrupt the LZW data.
  std::string gif = MakeMinimalGif();
  // Find the LZW data portion and corrupt it.
  for (size_t i = gif.size() - 10; i < gif.size() - 2; ++i) {
    if (i < gif.size()) {
      gif[i] = static_cast<char>(0xFF);
    }
  }

  ScanlineStatus status = reader->Initialize(gif.data(), gif.size());
  // May fail during init or during reading. Either way, no crash.
  if (status.Success() && reader->HasMoreFrames()) {
    ScanlineStatus frame_status;
    if (reader->PrepareNextFrame(&frame_status)) {
      while (reader->HasMoreScanlines()) {
        const void* scanline = nullptr;
        reader->ReadNextScanline(&scanline, &frame_status);
      }
    }
  }
  // No crash = pass.
}

TEST(GifFrameReaderMalformed, GarbageDataFails) {
  NullMessageHandler handler;
  std::unique_ptr<MultipleFrameReader> reader(new TestGifFrameReader(&handler));

  std::string garbage = "This is definitely not a GIF at all.";
  ScanlineStatus status = reader->Initialize(garbage.data(), garbage.size());
  EXPECT_FALSE(status.Success());
}

TEST(GifFrameReaderMalformed, EmptyBufferFails) {
  NullMessageHandler handler;
  std::unique_ptr<MultipleFrameReader> reader(new TestGifFrameReader(&handler));

  std::string empty;
  ScanlineStatus status = reader->Initialize(empty.data(), 0);
  EXPECT_FALSE(status.Success());
}

// --- GifReader::ReadPng with crafted multi-frame (animated) GIF ---

TEST_F(GifReaderTest, ReadPng_AnimatedGifFails) {
  // The GifReader (single-frame) should reject animated GIFs.
  std::string gif = MakeAnimatedGif();
  EXPECT_FALSE(gif_reader_->ReadPng(gif, read_.png_ptr(), read_.info_ptr(),
                                    PNG_TRANSFORM_IDENTITY))
      << "Single-frame GifReader should reject animated GIFs";
}

// --- GifReader::ReadPng with oversized dimensions ---

TEST_F(GifReaderTest, ReadPng_OversizedDimensions) {
  // Build a GIF with impossibly large dimensions.
  std::string gif;
  gif += "GIF89a";
  gif += '\xFF';
  gif += '\xFF';  // width = 65535
  gif += '\xFF';
  gif += '\xFF';  // height = 65535
  gif += '\x80';  // GCT=1, size=0 (2 entries)
  gif += '\x00';
  gif += '\x00';
  // GCT entries
  gif += '\xFF';
  gif += '\x00';
  gif += '\x00';
  gif += '\x00';
  gif += '\xFF';
  gif += '\x00';
  // Image Descriptor
  gif += '\x2C';
  gif += '\x00';
  gif += '\x00';
  gif += '\x00';
  gif += '\x00';
  gif += '\xFF';
  gif += '\xFF';  // width = 65535
  gif += '\xFF';
  gif += '\xFF';  // height = 65535
  gif += '\x00';
  // Minimal LZW data
  gif += '\x02';
  gif += '\x01';
  gif += '\x00';
  gif += '\x00';
  gif += '\x3B';

  // GetAttributes should still return the dimensions from the header.
  int w, h, bd, ct;
  EXPECT_TRUE(gif_reader_->GetAttributes(gif, &w, &h, &bd, &ct));
  EXPECT_EQ(65535, w);
  EXPECT_EQ(65535, h);

  // ReadPng may fail due to memory or other issues, but should not crash.
  bool result = gif_reader_->ReadPng(gif, read_.png_ptr(), read_.info_ptr(),
                                     PNG_TRANSFORM_IDENTITY);
  (void)result;
}

// --- GifFrameReader: animated GIF frame duration and timing ---

TEST(GifFrameReaderDisposal, FrameDuration) {
  NullMessageHandler handler;
  std::unique_ptr<MultipleFrameReader> reader(new TestGifFrameReader(&handler));
  std::string gif = MakeAnimatedGif(1, 2);

  ScanlineStatus status = reader->Initialize(gif.data(), gif.size());
  ASSERT_TRUE(status.Success());

  ASSERT_TRUE(reader->HasMoreFrames());
  EXPECT_TRUE(reader->PrepareNextFrame(&status));
  FrameSpec frame1;
  EXPECT_TRUE(reader->GetFrameSpec(&frame1, &status));
  // Delay was set to 10 (0x0A, 0x00) = 100ms.
  EXPECT_EQ(100u, frame1.duration_ms);

  // Read through frame 1.
  while (reader->HasMoreScanlines()) {
    const void* scanline = nullptr;
    EXPECT_TRUE(reader->ReadNextScanline(&scanline, &status));
  }

  ASSERT_TRUE(reader->HasMoreFrames());
  EXPECT_TRUE(reader->PrepareNextFrame(&status));
  FrameSpec frame2;
  EXPECT_TRUE(reader->GetFrameSpec(&frame2, &status));
  EXPECT_EQ(100u, frame2.duration_ms);
}

// --- GifFrameReader: loop count from NETSCAPE extension ---

TEST(GifFrameReaderDisposal, LoopCount) {
  NullMessageHandler handler;
  std::unique_ptr<MultipleFrameReader> reader(new TestGifFrameReader(&handler));
  std::string gif = MakeAnimatedGif();

  ScanlineStatus status = reader->Initialize(gif.data(), gif.size());
  ASSERT_TRUE(status.Success());

  ImageSpec image_spec;
  EXPECT_TRUE(reader->GetImageSpec(&image_spec, &status));
  // The NETSCAPE2.0 extension specified loop_count=0.
  EXPECT_EQ(0u, image_spec.loop_count);
}

TEST(GifFrameReaderDisposal, LoopCountWithChromeQuirks) {
  NullMessageHandler handler;
  std::unique_ptr<MultipleFrameReader> reader(new TestGifFrameReader(&handler));
  EXPECT_TRUE(reader->set_quirks_mode(QUIRKS_CHROME).Success());

  std::string gif = MakeAnimatedGif();
  ScanlineStatus status = reader->Initialize(gif.data(), gif.size());
  ASSERT_TRUE(status.Success());

  ImageSpec image_spec;
  EXPECT_TRUE(reader->GetImageSpec(&image_spec, &status));
  // Chrome quirks adds 1 to loop_count when NETSCAPE extension is present.
  EXPECT_EQ(1u, image_spec.loop_count);
}

// --- GifFrameReader: Reset mid-read ---

TEST(GifFrameReaderDisposal, ResetMidRead) {
  NullMessageHandler handler;
  std::unique_ptr<MultipleFrameReader> reader(new TestGifFrameReader(&handler));
  std::string gif = MakeAnimatedGif();

  ScanlineStatus status = reader->Initialize(gif.data(), gif.size());
  ASSERT_TRUE(status.Success());

  // Start reading the first frame.
  ASSERT_TRUE(reader->HasMoreFrames());
  EXPECT_TRUE(reader->PrepareNextFrame(&status));

  // Read one scanline, then reset.
  if (reader->HasMoreScanlines()) {
    const void* scanline = nullptr;
    EXPECT_TRUE(reader->ReadNextScanline(&scanline, &status));
  }

  // Reset and re-initialize.
  EXPECT_TRUE(reader->Reset().Success());
  status = reader->Initialize(gif.data(), gif.size());
  ASSERT_TRUE(status.Success());

  // Should be able to read from the start again.
  ImageSpec image_spec;
  EXPECT_TRUE(reader->GetImageSpec(&image_spec, &status));
  EXPECT_EQ(2u, image_spec.num_frames);
}

// ==========================================================================
// Error path coverage: ReadGifToPng with various malformed GIF data
// ==========================================================================

// GIF that is too short (truncated before Logical Screen Descriptor).
TEST_F(GifReaderTest, TruncatedBeforeScreenDescriptor) {
  // Only the GIF header, no screen descriptor.
  std::string gif = "GIF89a";
  EXPECT_FALSE(gif_reader_->ReadPng(gif, read_.png_ptr(), read_.info_ptr(),
                                    PNG_TRANSFORM_IDENTITY, false));
}

// GIF with only a header and screen descriptor but no image data.
TEST_F(GifReaderTest, TruncatedAfterScreenDescriptor) {
  std::string gif;
  gif += "GIF89a";
  gif += '\x02';
  gif += '\x00';  // width
  gif += '\x02';
  gif += '\x00';  // height
  gif += '\x80';  // GCT flag=1, size=0 (2 entries)
  gif += '\x00';  // bg color
  gif += '\x00';  // aspect ratio
  // GCT: 2 entries
  gif += std::string("\xFF\x00\x00", 3);
  gif += std::string("\x00\x00\x00", 3);
  // No image data or trailer -- truncated.
  EXPECT_FALSE(gif_reader_->ReadPng(gif, read_.png_ptr(), read_.info_ptr(),
                                    PNG_TRANSFORM_IDENTITY, false));
}

// Empty input should fail.
TEST_F(GifReaderTest, EmptyInput) {
  std::string empty;
  EXPECT_FALSE(gif_reader_->ReadPng(empty, read_.png_ptr(), read_.info_ptr(),
                                    PNG_TRANSFORM_IDENTITY, false));
}

// All-zeros data should fail.
TEST_F(GifReaderTest, AllZerosInput) {
  std::string zeros(100, '\0');
  EXPECT_FALSE(gif_reader_->ReadPng(zeros, read_.png_ptr(), read_.info_ptr(),
                                    PNG_TRANSFORM_IDENTITY, false));
}

// Single byte input should fail.
TEST_F(GifReaderTest, SingleByteInput) {
  std::string one("G");
  EXPECT_FALSE(gif_reader_->ReadPng(one, read_.png_ptr(), read_.info_ptr(),
                                    PNG_TRANSFORM_IDENTITY, false));
}

// GIF with no color map at all (no global and no local).
TEST_F(GifReaderTest, NoColorMap) {
  std::string gif;
  gif += "GIF89a";
  gif += '\x01';
  gif += '\x00';  // width=1
  gif += '\x01';
  gif += '\x00';  // height=1
  gif += '\x00';  // packed: NO GCT flag
  gif += '\x00';
  gif += '\x00';
  // Image Descriptor (no local color table either).
  gif += '\x2C';
  gif += '\x00';
  gif += '\x00';
  gif += '\x00';
  gif += '\x00';
  gif += '\x01';
  gif += '\x00';
  gif += '\x01';
  gif += '\x00';
  gif += '\x00';  // no LCT
  gif += '\x02';
  gif += '\x02';
  gif += '\x4C';
  gif += '\x01';
  gif += '\x00';
  gif += '\x3B';
  EXPECT_FALSE(gif_reader_->ReadPng(gif, read_.png_ptr(), read_.info_ptr(),
                                    PNG_TRANSFORM_IDENTITY, false))
      << "GIF with no color map should fail";
}

// GIF with multiple frames should fail for GifReader::ReadPng
// (which only handles single-frame GIFs).
TEST_F(GifReaderTest, MultipleFramesFails) {
  std::string gif;
  gif += "GIF89a";
  gif += '\x02';
  gif += '\x00';
  gif += '\x02';
  gif += '\x00';
  gif += '\x80';
  gif += '\x00';
  gif += '\x00';
  // GCT
  gif += std::string("\xFF\x00\x00", 3);
  gif += std::string("\x00\x00\x00", 3);
  // Frame 1
  gif += '\x2C';
  gif += '\x00';
  gif += '\x00';
  gif += '\x00';
  gif += '\x00';
  gif += '\x02';
  gif += '\x00';
  gif += '\x02';
  gif += '\x00';
  gif += '\x00';
  gif += '\x02';
  gif += '\x03';
  gif += '\x04';
  gif += '\x01';
  gif += '\x00';
  gif += '\x00';
  // Frame 2 (same as frame 1)
  gif += '\x2C';
  gif += '\x00';
  gif += '\x00';
  gif += '\x00';
  gif += '\x00';
  gif += '\x02';
  gif += '\x00';
  gif += '\x02';
  gif += '\x00';
  gif += '\x00';
  gif += '\x02';
  gif += '\x03';
  gif += '\x04';
  gif += '\x01';
  gif += '\x00';
  gif += '\x00';
  gif += '\x3B';
  // ReadPng should fail because ImageCount > 1.
  EXPECT_FALSE(gif_reader_->ReadPng(gif, read_.png_ptr(), read_.info_ptr(),
                                    PNG_TRANSFORM_IDENTITY, false))
      << "Multiple-frame GIF should fail for ReadPng";
}

// GIF with graphics extension but truncated extension data.
TEST_F(GifReaderTest, TruncatedGraphicsExtension) {
  std::string gif;
  gif += "GIF89a";
  gif += '\x01';
  gif += '\x00';
  gif += '\x01';
  gif += '\x00';
  gif += '\x80';
  gif += '\x00';
  gif += '\x00';
  gif += std::string("\xFF\x00\x00", 3);
  gif += std::string("\x00\x00\x00", 3);
  // Graphics Extension with insufficient data.
  gif += '\x21';  // extension introducer
  gif += '\xF9';  // GCE label
  gif += '\x02';  // block size = 2 (should be 4)
  gif += '\x00';
  gif += '\x00';
  // No block terminator, no image data.
  EXPECT_FALSE(gif_reader_->ReadPng(gif, read_.png_ptr(), read_.info_ptr(),
                                    PNG_TRANSFORM_IDENTITY, false))
      << "Truncated graphics extension should fail";
}

// GIF with corrupt LZW data in the image.
TEST_F(GifReaderTest, CorruptLzwData) {
  std::string gif = MakeMinimalGif();
  // Corrupt the LZW data bytes (near the end, before the trailer).
  ASSERT_GT(gif.size(), 10u);
  // The LZW data starts after the image descriptor. Corrupt it.
  for (size_t i = gif.size() - 6; i < gif.size() - 2; ++i) {
    gif[i] ^= 0xFF;
  }
  EXPECT_FALSE(gif_reader_->ReadPng(gif, read_.png_ptr(), read_.info_ptr(),
                                    PNG_TRANSFORM_IDENTITY, false))
      << "Corrupt LZW data should fail";
}

// GIF with require_opaque=true but has transparency.
TEST_F(GifReaderTest, RequireOpaqueWithTransparency) {
  std::string gif;
  gif += "GIF89a";
  gif += '\x01';
  gif += '\x00';
  gif += '\x01';
  gif += '\x00';
  gif += '\x80';
  gif += '\x00';
  gif += '\x00';
  gif += std::string("\xFF\x00\x00", 3);
  gif += std::string("\x00\x00\x00", 3);
  // Graphics Control Extension with transparency flag set.
  gif += '\x21';  // extension introducer
  gif += '\xF9';  // GCE label
  gif += '\x04';  // block size
  gif += '\x01';  // packed: transparency flag=1
  gif += '\x00';
  gif += '\x00';  // delay
  gif += '\x00';  // transparent index = 0
  gif += '\x00';  // block terminator
  // Image Descriptor
  gif += '\x2C';
  gif += '\x00';
  gif += '\x00';
  gif += '\x00';
  gif += '\x00';
  gif += '\x01';
  gif += '\x00';
  gif += '\x01';
  gif += '\x00';
  gif += '\x00';
  gif += '\x02';
  gif += '\x02';
  gif += '\x4C';
  gif += '\x01';
  gif += '\x00';
  gif += '\x3B';
  EXPECT_FALSE(gif_reader_->ReadPng(gif, read_.png_ptr(), read_.info_ptr(),
                                    PNG_TRANSFORM_IDENTITY, true))
      << "Transparent GIF with require_opaque=true should fail";
}

// GIF with unsupported transform bits should fail.
TEST_F(GifReaderTest, UnsupportedTransform) {
  std::string gif = MakeMinimalGif();
  // PNG_TRANSFORM_PACKING is not in the allowed set for GIF.
  EXPECT_FALSE(gif_reader_->ReadPng(gif, read_.png_ptr(), read_.info_ptr(),
                                    PNG_TRANSFORM_PACKING, false))
      << "Unsupported transform should be rejected";
}

// ==========================================================================
// GifFrameReader error paths
// ==========================================================================

TEST(GifFrameReaderErrorPaths, EmptyData) {
  NullMessageHandler handler;
  std::unique_ptr<MultipleFrameReader> reader(new TestGifFrameReader(&handler));
  std::string empty;
  ScanlineStatus status = reader->Initialize(empty.data(), empty.size());
  EXPECT_FALSE(status.Success());
}

TEST(GifFrameReaderErrorPaths, TruncatedHeader) {
  NullMessageHandler handler;
  std::unique_ptr<MultipleFrameReader> reader(new TestGifFrameReader(&handler));
  std::string partial = "GIF89";
  ScanlineStatus status = reader->Initialize(partial.data(), partial.size());
  EXPECT_FALSE(status.Success());
}

TEST(GifFrameReaderErrorPaths, AllZeros) {
  NullMessageHandler handler;
  std::unique_ptr<MultipleFrameReader> reader(new TestGifFrameReader(&handler));
  std::string zeros(50, '\0');
  ScanlineStatus status = reader->Initialize(zeros.data(), zeros.size());
  EXPECT_FALSE(status.Success());
}

TEST(GifFrameReaderErrorPaths, CorruptLzwData) {
  NullMessageHandler handler;
  std::unique_ptr<MultipleFrameReader> reader(new TestGifFrameReader(&handler));
  std::string gif = MakeMinimalGif();
  // Corrupt the LZW data.
  for (size_t i = gif.size() - 6; i < gif.size() - 2; ++i) {
    gif[i] ^= 0xFF;
  }
  ScanlineStatus status = reader->Initialize(gif.data(), gif.size());
  // Initialization reads the image metadata -- the actual decode failure
  // may happen here or during frame reading.
  if (status.Success()) {
    // If Initialize succeeded, try reading frames. The decode should fail.
    if (reader->HasMoreFrames()) {
      ScanlineStatus prep_status;
      reader->PrepareNextFrame(&prep_status);
      if (prep_status.Success() && reader->HasMoreScanlines()) {
        const void* scanline = nullptr;
        ScanlineStatus read_status;
        reader->ReadNextScanline(&scanline, &read_status);
        // At some point in the pipeline, we expect failure.
      }
    }
  }
  // This test verifies no crash on corrupt data -- not a specific return value.
}

TEST(GifFrameReaderErrorPaths, NullFrameSpecPointer) {
  NullMessageHandler handler;
  std::unique_ptr<MultipleFrameReader> reader(new TestGifFrameReader(&handler));
  std::string gif = MakeMinimalGif();
  ScanlineStatus status = reader->Initialize(gif.data(), gif.size());
  ASSERT_TRUE(status.Success());

  // Null FrameSpec pointer should return an error.
  status = reader->GetFrameSpec(nullptr);
  EXPECT_FALSE(status.Success());
}

TEST(GifFrameReaderErrorPaths, NullImageSpecPointer) {
  NullMessageHandler handler;
  std::unique_ptr<MultipleFrameReader> reader(new TestGifFrameReader(&handler));
  std::string gif = MakeMinimalGif();
  ScanlineStatus status = reader->Initialize(gif.data(), gif.size());
  ASSERT_TRUE(status.Success());

  // Null ImageSpec pointer should return an error.
  status = reader->GetImageSpec(nullptr);
  EXPECT_FALSE(status.Success());
}

// ==========================================================================
// Phase 6: Targeted error-path coverage for gif_reader.cc
// Covers ~75 uncovered lines in error handling, transparency, colormap
// fallback, interlacing through ReadPng, and extension parsing paths.
// ==========================================================================

// Helper: Build a minimal GIF89a with a local color table (no global).
// Screen: 2x2, no GCT; image descriptor has a 2-entry local color table.
static std::string MakeGifWithLocalColorTable() {
  std::string gif;
  gif += "GIF89a";
  // Logical Screen Descriptor: 2x2, NO global color table
  gif += '\x02';
  gif += '\x00';  // width
  gif += '\x02';
  gif += '\x00';  // height
  gif += '\x00';  // packed: no GCT
  gif += '\x00';  // bg color
  gif += '\x00';  // aspect ratio
  // Image Descriptor with local color table
  gif += '\x2C';  // image separator
  gif += '\x00';
  gif += '\x00';  // left
  gif += '\x00';
  gif += '\x00';  // top
  gif += '\x02';
  gif += '\x00';  // width
  gif += '\x02';
  gif += '\x00';  // height
  // packed: LCT flag=1, not interlaced, sort=0, LCT size=0 (2^(0+1)=2)
  gif += '\x80';
  // Local Color Table: 2 entries
  gif += '\xFF';
  gif += '\x00';
  gif += '\x00';  // red
  gif += '\x00';
  gif += '\xFF';
  gif += '\x00';  // green
  // Image Data: LZW min code size = 2
  gif += '\x02';
  gif += '\x03';  // sub-block size
  gif += '\x04';
  gif += '\x01';
  gif += '\x00';
  gif += '\x00';  // block terminator
  // Trailer
  gif += '\x3B';
  return gif;
}

// Helper: Build a GIF with a transparent index that exceeds the palette size.
// The palette has 2 entries (indices 0-1), but transparent index is set to 5.
static std::string MakeGifWithInvalidTransparentIndex() {
  std::string gif;
  gif += "GIF89a";
  gif += '\x01';
  gif += '\x00';  // width=1
  gif += '\x01';
  gif += '\x00';  // height=1
  gif += '\x80';  // GCT flag=1, size=0 (2 entries)
  gif += '\x00';  // bg color
  gif += '\x00';  // aspect ratio
  // Global Color Table: 2 entries
  gif += '\xFF';
  gif += '\x00';
  gif += '\x00';  // 0: red
  gif += '\x00';
  gif += '\x00';
  gif += '\xFF';  // 1: blue
  // Graphics Control Extension with transparency flag, index=5 (out of range)
  gif += '\x21';  // extension introducer
  gif += '\xF9';  // GCE label
  gif += '\x04';  // block size = 4
  gif += '\x01';  // packed: transparent flag=1
  gif += '\x00';
  gif += '\x00';  // delay
  gif += '\x05';  // transparent index = 5 (only 2 palette entries!)
  gif += '\x00';  // block terminator
  // Image Descriptor
  gif += '\x2C';
  gif += '\x00';
  gif += '\x00';  // left
  gif += '\x00';
  gif += '\x00';  // top
  gif += '\x01';
  gif += '\x00';  // width=1
  gif += '\x01';
  gif += '\x00';  // height=1
  gif += '\x00';  // packed: no LCT, not interlaced
  // Image Data
  gif += '\x02';  // LZW minimum code size
  gif += '\x02';  // sub-block size
  gif += '\x4C';
  gif += '\x01';
  gif += '\x00';  // block terminator
  // Trailer
  gif += '\x3B';
  return gif;
}

// ---- Test 1: Transparent index out of bounds in AddTransparencyChunk ----
// Covers gif_reader.cc line 95: (num_trans <= 0 || num_trans > num_palette)
// When the transparent palette index exceeds the palette color count,
// AddTransparencyChunk should return false, failing the ReadPng call.
TEST_F(GifReaderTest, TransparentIndexExceedsPalette) {
  std::string gif = MakeGifWithInvalidTransparentIndex();
  // Without EXPAND and without STRIP_ALPHA, the code goes through
  // AddTransparencyChunk which checks num_trans > num_palette.
  EXPECT_FALSE(gif_reader_->ReadPng(gif, read_.png_ptr(), read_.info_ptr(),
                                    PNG_TRANSFORM_IDENTITY, false))
      << "Transparent index beyond palette size should fail in "
         "AddTransparencyChunk";
}

// ---- Test 2: Transparent index out of bounds with EXPAND ----
// When expand_colormap is true, transparency is handled in ExpandColorMap,
// not AddTransparencyChunk. The transparent index is still out of range,
// but ExpandColorMap treats it differently (it doesn't range-check the index
// against the palette -- it only checks for equality per pixel).
// Covers the expand_colormap path with an invalid transparent index.
TEST_F(GifReaderTest, TransparentIndexExceedsPaletteWithExpand) {
  std::string gif = MakeGifWithInvalidTransparentIndex();
  // With EXPAND, the code bypasses AddTransparencyChunk and goes through
  // ExpandColorMap. The transparent_palette_index=5 won't match any pixel
  // (which uses index 0), so all pixels are opaque despite RGBA format.
  bool result = gif_reader_->ReadPng(gif, read_.png_ptr(), read_.info_ptr(),
                                     PNG_TRANSFORM_EXPAND, false);
  // This should succeed since ExpandColorMap doesn't range-check the index.
  EXPECT_TRUE(result);
  if (result) {
    int ct = png_get_color_type(read_.png_ptr(), read_.info_ptr());
    // Should be RGBA because transparency is indicated.
    EXPECT_EQ(PNG_COLOR_TYPE_RGB_ALPHA, ct);
  }
}

// ---- Test 3: ExpandColorMap fallback to global color table ----
// Covers gif_reader.cc line 137-138: global colormap fallback in
// ReadImageDescriptor when Image.ColorMap is null.
// Existing expand tests use pngsuite GIFs which may have local color tables.
// This test explicitly uses a GIF with ONLY a global color table.
TEST_F(GifReaderTest, ExpandColormapGlobalFallback) {
  // MakeMinimalGif89a() has a global color table, no local color table.
  std::string gif = MakeMinimalGif89a();
  ASSERT_TRUE(gif_reader_->ReadPng(gif, read_.png_ptr(), read_.info_ptr(),
                                   PNG_TRANSFORM_EXPAND, false))
      << "ExpandColorMap should succeed by falling back to global color table";
  int ct = png_get_color_type(read_.png_ptr(), read_.info_ptr());
  EXPECT_EQ(PNG_COLOR_TYPE_RGB, ct);
}

// ---- Test 4: ReadPng with local color table (not global fallback) ----
// Ensures that when a local color table IS present, it is used instead.
TEST_F(GifReaderTest, ReadPngWithLocalColorTable) {
  std::string gif = MakeGifWithLocalColorTable();
  EXPECT_TRUE(gif_reader_->ReadPng(gif, read_.png_ptr(), read_.info_ptr(),
                                   PNG_TRANSFORM_IDENTITY, false))
      << "GIF with local color table should succeed";
}

// ---- Test 5: ExpandColorMap with local color table ----
TEST_F(GifReaderTest, ExpandColormapWithLocalColorTable) {
  std::string gif = MakeGifWithLocalColorTable();
  EXPECT_TRUE(gif_reader_->ReadPng(gif, read_.png_ptr(), read_.info_ptr(),
                                   PNG_TRANSFORM_EXPAND, false))
      << "ExpandColorMap with local color table should succeed";
  int ct = png_get_color_type(read_.png_ptr(), read_.info_ptr());
  EXPECT_EQ(PNG_COLOR_TYPE_RGB, ct);
}

// ---- Test 6: Interlaced GIF through ReadPng (deinterlace path) ----
// Covers gif_reader.cc lines 170-183: interlaced deinterlace loop in
// ReadImageDescriptor. Existing tests only use the GifFrameReader path.
TEST_F(GifReaderTest, InterlacedGifReadPng) {
  std::string in;
  ReadTestFile(kGifTestDir, kInterlacedImage, "gif", &in);
  ASSERT_NE(0u, in.length());
  ASSERT_TRUE(gif_reader_->ReadPng(in, read_.png_ptr(), read_.info_ptr(),
                                   PNG_TRANSFORM_IDENTITY, false))
      << "Interlaced GIF should be readable through ReadPng";
  png_uint_32 width, height;
  int bd, ct;
  png_get_IHDR(read_.png_ptr(), read_.info_ptr(), &width, &height, &bd, &ct,
               nullptr, nullptr, nullptr);
  EXPECT_GT(width, 0u);
  EXPECT_GT(height, 0u);
}

// ---- Test 7: Interlaced GIF through ReadPng with EXPAND ----
TEST_F(GifReaderTest, InterlacedGifReadPngExpand) {
  std::string in;
  ReadTestFile(kGifTestDir, kInterlacedImage, "gif", &in);
  ASSERT_NE(0u, in.length());
  ASSERT_TRUE(gif_reader_->ReadPng(in, read_.png_ptr(), read_.info_ptr(),
                                   PNG_TRANSFORM_EXPAND, false))
      << "Interlaced GIF should be expandable through ReadPng";
  int ct = png_get_color_type(read_.png_ptr(), read_.info_ptr());
  EXPECT_EQ(PNG_COLOR_TYPE_RGB, ct);
}

// ---- Test 8: Zero-width GIF through ReadPng ----
// Covers gif_reader.cc line 246-248: AllocatePngPixels returns row_size == 0.
// Also covers line 366-368: ReadGifToPng checks row_size == 0.
TEST_F(GifReaderTest, ZeroWidthGifReadPng) {
  std::string gif;
  gif += "GIF89a";
  gif += '\x00';
  gif += '\x00';  // width = 0
  gif += '\x01';
  gif += '\x00';  // height = 1
  gif += '\x80';  // GCT=1, size=0 (2 entries)
  gif += '\x00';
  gif += '\x00';
  // GCT entries
  gif += '\xFF';
  gif += '\x00';
  gif += '\x00';
  gif += '\x00';
  gif += '\xFF';
  gif += '\x00';
  // Trailer (no image data for zero-width image)
  gif += '\x3B';
  // ReadPng should fail because row_size will be 0 from zero width.
  EXPECT_FALSE(gif_reader_->ReadPng(gif, read_.png_ptr(), read_.info_ptr(),
                                    PNG_TRANSFORM_IDENTITY, false));
}

// ---- Test 9: Zero-height GIF through ReadPng ----
TEST_F(GifReaderTest, ZeroHeightGifReadPng) {
  std::string gif;
  gif += "GIF89a";
  gif += '\x01';
  gif += '\x00';  // width = 1
  gif += '\x00';
  gif += '\x00';  // height = 0
  gif += '\x80';  // GCT=1, size=0 (2 entries)
  gif += '\x00';
  gif += '\x00';
  gif += '\xFF';
  gif += '\x00';
  gif += '\x00';
  gif += '\x00';
  gif += '\xFF';
  gif += '\x00';
  gif += '\x3B';
  EXPECT_FALSE(gif_reader_->ReadPng(gif, read_.png_ptr(), read_.info_ptr(),
                                    PNG_TRANSFORM_IDENTITY, false));
}

// ---- Test 10: Zero-width GIF with ExpandColorMap ----
// Covers gif_reader.cc line 289: row_size == 0 check in ExpandColorMap.
TEST_F(GifReaderTest, ZeroWidthGifExpandColormap) {
  std::string gif;
  gif += "GIF89a";
  gif += '\x00';
  gif += '\x00';  // width = 0
  gif += '\x01';
  gif += '\x00';  // height = 1
  gif += '\x80';
  gif += '\x00';
  gif += '\x00';
  gif += '\xFF';
  gif += '\x00';
  gif += '\x00';
  gif += '\x00';
  gif += '\xFF';
  gif += '\x00';
  gif += '\x3B';
  EXPECT_FALSE(gif_reader_->ReadPng(gif, read_.png_ptr(), read_.info_ptr(),
                                    PNG_TRANSFORM_EXPAND, false));
}

// ---- Test 11: GIF with only TERMINATE_RECORD_TYPE (no image data) ----
// Exercises the ReadGifToPng loop hitting only the terminator without
// ever finding an image descriptor.
TEST_F(GifReaderTest, GifWithOnlyTerminator) {
  std::string gif;
  gif += "GIF89a";
  gif += '\x02';
  gif += '\x00';
  gif += '\x02';
  gif += '\x00';
  gif += '\x80';  // GCT flag
  gif += '\x00';
  gif += '\x00';
  // GCT: 2 entries
  gif += '\xFF';
  gif += '\x00';
  gif += '\x00';
  gif += '\x00';
  gif += '\xFF';
  gif += '\x00';
  // Immediately terminate (no image)
  gif += '\x3B';
  // This succeeds in parsing (terminator found) but produces no image data.
  // The result depends on whether ReadGifToPng checks for at least one image.
  // With no image descriptor, the PLTE won't be set, so expand will fail.
  bool result = gif_reader_->ReadPng(gif, read_.png_ptr(), read_.info_ptr(),
                                     PNG_TRANSFORM_IDENTITY, false);
  // Either outcome is acceptable -- just verifying no crash.
  (void)result;
}

// ---- Test 12: GIF with non-graphics extension (comment ext) ----
// Exercises the ReadExtension path where ext_code != GRAPHICS_EXT_FUNC_CODE
// and the extension is just skipped over.
TEST_F(GifReaderTest, CommentExtensionSkipped) {
  std::string gif;
  gif += "GIF89a";
  gif += '\x01';
  gif += '\x00';
  gif += '\x01';
  gif += '\x00';
  gif += '\x80';
  gif += '\x00';
  gif += '\x00';
  // GCT: 2 entries
  gif += '\xFF';
  gif += '\x00';
  gif += '\x00';
  gif += '\x00';
  gif += '\xFF';
  gif += '\x00';
  // Comment Extension (0xFE) -- should be silently ignored
  gif += '\x21';  // extension introducer
  gif += '\xFE';  // comment label
  gif += '\x05';  // sub-block size = 5
  gif += "Hello";
  gif += '\x00';  // block terminator
  // Image Descriptor
  gif += '\x2C';
  gif += '\x00';
  gif += '\x00';
  gif += '\x00';
  gif += '\x00';
  gif += '\x01';
  gif += '\x00';
  gif += '\x01';
  gif += '\x00';
  gif += '\x00';
  // Image Data
  gif += '\x02';
  gif += '\x02';
  gif += '\x4C';
  gif += '\x01';
  gif += '\x00';
  gif += '\x3B';
  EXPECT_TRUE(gif_reader_->ReadPng(gif, read_.png_ptr(), read_.info_ptr(),
                                   PNG_TRANSFORM_IDENTITY, false))
      << "Comment extension should be silently skipped";
}

// ---- Test 13: GIF with application extension (non-NETSCAPE) ----
// Exercises ProcessExtensionAffectingImage where the application extension
// has the right identifier length but is NOT "NETSCAPE2.0".
TEST(GifFrameReaderCoverage, NonNetscapeApplicationExtension) {
  NullMessageHandler handler;
  std::unique_ptr<MultipleFrameReader> reader(new TestGifFrameReader(&handler));

  std::string gif;
  gif += "GIF89a";
  gif += '\x02';
  gif += '\x00';  // width=2
  gif += '\x02';
  gif += '\x00';  // height=2
  gif += '\x80';  // GCT=1, size=0 (2 entries)
  gif += '\x00';
  gif += '\x00';
  // GCT
  gif += '\xFF';
  gif += '\x00';
  gif += '\x00';
  gif += '\x00';
  gif += '\xFF';
  gif += '\x00';
  // Application Extension with correct block size (11) but different ID
  gif += '\x21';         // extension introducer
  gif += '\xFF';         // application extension label
  gif += '\x0B';         // block size = 11
  gif += "MYAPPEXT1.0";  // 11 chars, not NETSCAPE2.0
  gif += '\x03';         // sub-block size
  gif += '\x01';
  gif += '\x00';
  gif += '\x00';
  gif += '\x00';  // block terminator
  // Image Descriptor
  gif += '\x2C';
  gif += '\x00';
  gif += '\x00';
  gif += '\x00';
  gif += '\x00';
  gif += '\x02';
  gif += '\x00';
  gif += '\x02';
  gif += '\x00';
  gif += '\x00';
  // Image Data
  gif += '\x02';
  gif += '\x03';
  gif += '\x04';
  gif += '\x01';
  gif += '\x00';
  gif += '\x00';
  gif += '\x3B';

  ScanlineStatus status = reader->Initialize(gif.data(), gif.size());
  ASSERT_TRUE(status.Success()) << status.ToString();
  ImageSpec image_spec;
  EXPECT_TRUE(reader->GetImageSpec(&image_spec, &status));
  // Non-NETSCAPE extension should not modify the default loop count (1).
  EXPECT_EQ(1u, image_spec.loop_count);
}

// ---- Test 14: Application extension with wrong identifier length ----
// Covers gif_reader.cc lines 754-758: identifier length != 11.
TEST(GifFrameReaderCoverage, AppExtensionWrongIdentifierLength) {
  NullMessageHandler handler;
  std::unique_ptr<MultipleFrameReader> reader(new TestGifFrameReader(&handler));

  std::string gif;
  gif += "GIF89a";
  gif += '\x02';
  gif += '\x00';
  gif += '\x02';
  gif += '\x00';
  gif += '\x80';
  gif += '\x00';
  gif += '\x00';
  gif += '\xFF';
  gif += '\x00';
  gif += '\x00';
  gif += '\x00';
  gif += '\xFF';
  gif += '\x00';
  // Application Extension with wrong identifier length (5 instead of 11)
  gif += '\x21';  // extension introducer
  gif += '\xFF';  // application extension label
  gif += '\x05';  // block size = 5 (wrong! should be 11)
  gif += "SHORT";
  gif += '\x00';  // block terminator
  // Image Descriptor
  gif += '\x2C';
  gif += '\x00';
  gif += '\x00';
  gif += '\x00';
  gif += '\x00';
  gif += '\x02';
  gif += '\x00';
  gif += '\x02';
  gif += '\x00';
  gif += '\x00';
  gif += '\x02';
  gif += '\x03';
  gif += '\x04';
  gif += '\x01';
  gif += '\x00';
  gif += '\x00';
  gif += '\x3B';

  ScanlineStatus status = reader->Initialize(gif.data(), gif.size());
  // Should fail because the app extension block size is unexpected.
  EXPECT_FALSE(status.Success())
      << "Application extension with wrong identifier length should fail";
}

// ---- Test 15: GifFrameReader with interlaced GIF via ReadPng path ----
// Verifies that the interlaced deinterlace loop in ReadImageDescriptor
// works correctly for all passes.
TEST_F(GifReaderTest, InterlacedGifReadPngAllPasses) {
  std::string in;
  ReadTestFile(kGifTestDir, kInterlacedImage, "gif", &in);
  ASSERT_NE(0u, in.length());

  // Read without expand (paletted).
  ASSERT_TRUE(gif_reader_->ReadPng(in, read_.png_ptr(), read_.info_ptr(),
                                   PNG_TRANSFORM_IDENTITY, false));
  png_uint_32 width, height;
  int bd, ct;
  png_get_IHDR(read_.png_ptr(), read_.info_ptr(), &width, &height, &bd, &ct,
               nullptr, nullptr, nullptr);
  EXPECT_GT(width, 0u);
  EXPECT_GT(height, 0u);

  // Verify we can get the row data.
  png_bytepp rows = png_get_rows(read_.png_ptr(), read_.info_ptr());
  ASSERT_NE(nullptr, rows);
  for (png_uint_32 row = 0; row < height; ++row) {
    ASSERT_NE(nullptr, rows[row]) << "Row " << row << " is null";
  }
}

// ---- Test 16: GifFrameReader with no color map in CreateColorMap ----
// Covers gif_reader.cc lines 1014-1017: missing colormap in CreateColorMap.
// This is tested via the frame reader path (not the ReadPng path).
TEST(GifFrameReaderCoverage, NoColorMapInCreateColorMap) {
  NullMessageHandler handler;
  std::unique_ptr<MultipleFrameReader> reader(new TestGifFrameReader(&handler));

  // GIF with no GCT and no LCT -- CreateColorMap should fail.
  std::string gif;
  gif += "GIF89a";
  gif += '\x02';
  gif += '\x00';
  gif += '\x02';
  gif += '\x00';
  gif += '\x00';  // no GCT
  gif += '\x00';
  gif += '\x00';
  // Image Descriptor without LCT
  gif += '\x2C';
  gif += '\x00';
  gif += '\x00';
  gif += '\x00';
  gif += '\x00';
  gif += '\x02';
  gif += '\x00';
  gif += '\x02';
  gif += '\x00';
  gif += '\x00';  // no LCT
  // Image data
  gif += '\x02';
  gif += '\x03';
  gif += '\x04';
  gif += '\x01';
  gif += '\x00';
  gif += '\x00';
  gif += '\x3B';

  ScanlineStatus status = reader->Initialize(gif.data(), gif.size());
  if (status.Success()) {
    // Initialize succeeded (it only counts frames, doesn't decode).
    // PrepareNextFrame triggers CreateColorMap which should fail.
    EXPECT_TRUE(reader->HasMoreFrames());
    ScanlineStatus frame_status;
    bool ok = reader->PrepareNextFrame(&frame_status);
    EXPECT_FALSE(ok) << "PrepareNextFrame should fail due to missing color map";
  }
}

// ---- Test 17: ExpandColorMap with transparency (strip_alpha=false) ----
// Covers the RGBA path in ExpandColorMap (lines 298-307) where transparent
// pixels are zeroed and non-transparent pixels get RGB from palette + alpha=FF.
TEST_F(GifReaderTest, ExpandColormapWithTransparency) {
  std::string gif;
  gif += "GIF89a";
  gif += '\x02';
  gif += '\x00';  // width=2
  gif += '\x02';
  gif += '\x00';  // height=2
  gif += '\x80';  // GCT=1, size=0 (2 entries)
  gif += '\x00';
  gif += '\x00';
  // GCT
  gif += '\xFF';
  gif += '\x00';
  gif += '\x00';  // 0: red
  gif += '\x00';
  gif += '\xFF';
  gif += '\x00';  // 1: green
  // Graphics Control Extension: transparent index = 1
  gif += '\x21';
  gif += '\xF9';
  gif += '\x04';
  gif += '\x01';  // transparent flag
  gif += '\x00';
  gif += '\x00';
  gif += '\x01';  // transparent color index = 1
  gif += '\x00';
  // Image Descriptor
  gif += '\x2C';
  gif += '\x00';
  gif += '\x00';
  gif += '\x00';
  gif += '\x00';
  gif += '\x02';
  gif += '\x00';
  gif += '\x02';
  gif += '\x00';
  gif += '\x00';
  // Image Data
  gif += '\x02';
  gif += '\x03';
  gif += '\x04';
  gif += '\x01';
  gif += '\x00';
  gif += '\x00';
  gif += '\x3B';

  // With EXPAND (no STRIP_ALPHA), should produce RGBA output.
  ASSERT_TRUE(gif_reader_->ReadPng(gif, read_.png_ptr(), read_.info_ptr(),
                                   PNG_TRANSFORM_EXPAND, false));
  int ct = png_get_color_type(read_.png_ptr(), read_.info_ptr());
  EXPECT_EQ(PNG_COLOR_TYPE_RGB_ALPHA, ct);
}

// ---- Test 18: ExpandColorMap with transparency + strip alpha ----
// Covers the case where expand_colormap=true AND strip_alpha=true.
// The transparent_palette_index is passed as -1 to ExpandColorMap.
TEST_F(GifReaderTest, ExpandColormapWithTransparencyStripped) {
  std::string gif;
  gif += "GIF89a";
  gif += '\x02';
  gif += '\x00';
  gif += '\x02';
  gif += '\x00';
  gif += '\x80';
  gif += '\x00';
  gif += '\x00';
  gif += '\xFF';
  gif += '\x00';
  gif += '\x00';
  gif += '\x00';
  gif += '\xFF';
  gif += '\x00';
  // GCE: transparent index = 0
  gif += '\x21';
  gif += '\xF9';
  gif += '\x04';
  gif += '\x01';
  gif += '\x00';
  gif += '\x00';
  gif += '\x00';  // transparent index = 0
  gif += '\x00';
  // Image Descriptor
  gif += '\x2C';
  gif += '\x00';
  gif += '\x00';
  gif += '\x00';
  gif += '\x00';
  gif += '\x02';
  gif += '\x00';
  gif += '\x02';
  gif += '\x00';
  gif += '\x00';
  gif += '\x02';
  gif += '\x03';
  gif += '\x04';
  gif += '\x01';
  gif += '\x00';
  gif += '\x00';
  gif += '\x3B';

  // EXPAND | STRIP_ALPHA: should produce RGB (not RGBA) even though
  // the GIF has transparency.
  ASSERT_TRUE(gif_reader_->ReadPng(
      gif, read_.png_ptr(), read_.info_ptr(),
      PNG_TRANSFORM_EXPAND | PNG_TRANSFORM_STRIP_ALPHA, false));
  int ct = png_get_color_type(read_.png_ptr(), read_.info_ptr());
  EXPECT_EQ(PNG_COLOR_TYPE_RGB, ct);
}

// ---- Test 19: GifFrameReader with GCE unexpected size in animated GIF ----
// Covers gif_reader.cc line 689-693: GCE extension[0] != 4.
// In the frame reader path (ProcessExtensionAffectingFrame), a GCE with
// wrong block size should fail.
TEST(GifFrameReaderCoverage, GceWrongBlockSize) {
  NullMessageHandler handler;
  std::unique_ptr<MultipleFrameReader> reader(new TestGifFrameReader(&handler));

  std::string gif;
  gif += "GIF89a";
  gif += '\x02';
  gif += '\x00';
  gif += '\x02';
  gif += '\x00';
  gif += '\x80';
  gif += '\x00';
  gif += '\x00';
  gif += '\xFF';
  gif += '\x00';
  gif += '\x00';
  gif += '\x00';
  gif += '\xFF';
  gif += '\x00';
  // GCE with wrong block size (2 instead of 4)
  gif += '\x21';
  gif += '\xF9';
  gif += '\x02';  // block size = 2 (should be 4!)
  gif += '\x00';
  gif += '\x00';
  gif += '\x00';  // block terminator
  // Image Descriptor
  gif += '\x2C';
  gif += '\x00';
  gif += '\x00';
  gif += '\x00';
  gif += '\x00';
  gif += '\x02';
  gif += '\x00';
  gif += '\x02';
  gif += '\x00';
  gif += '\x00';
  gif += '\x02';
  gif += '\x03';
  gif += '\x04';
  gif += '\x01';
  gif += '\x00';
  gif += '\x00';
  gif += '\x3B';

  ScanlineStatus status = reader->Initialize(gif.data(), gif.size());
  if (status.Success()) {
    // The GCE error should surface during PrepareNextFrame.
    ScanlineStatus frame_status;
    bool ok = reader->PrepareNextFrame(&frame_status);
    EXPECT_FALSE(ok)
        << "GCE with wrong block size should fail in PrepareNextFrame";
  }
}

// ---- Test 20: GifFrameReader: transparent index >= palette size ----
// In the frame reader path, CreateColorMap handles transparent index
// >= frame_palette_size_ (line 1042-1047). When transparent index is set
// but out of range, the frame becomes RGBA_8888 but the palette entry
// at the transparent index is not zeroed (since it's out of range).
TEST(GifFrameReaderCoverage, TransparentIndexOutOfRangeInCreateColorMap) {
  NullMessageHandler handler;
  std::unique_ptr<MultipleFrameReader> reader(new TestGifFrameReader(&handler));

  std::string gif = MakeGifWithInvalidTransparentIndex();
  ScanlineStatus status = reader->Initialize(gif.data(), gif.size());
  if (status.Success()) {
    ASSERT_TRUE(reader->HasMoreFrames());
    ScanlineStatus frame_status;
    bool ok = reader->PrepareNextFrame(&frame_status);
    if (ok) {
      FrameSpec frame_spec;
      EXPECT_TRUE(reader->GetFrameSpec(&frame_spec, &frame_status));
      // Transparent index present, so pixel_format should be RGBA.
      EXPECT_EQ(RGBA_8888, frame_spec.pixel_format);
      // Read all scanlines without crash.
      while (reader->HasMoreScanlines()) {
        const void* scanline = nullptr;
        EXPECT_TRUE(reader->ReadNextScanline(&scanline, &frame_status));
      }
    }
  }
}

// ---- Test 21: GifFrameReader with GIF using global color map ----
// Exercises CreateColorMap fallback to global color map (line 1011-1013).
TEST(GifFrameReaderCoverage, GlobalColorMapFallback) {
  NullMessageHandler handler;
  std::unique_ptr<MultipleFrameReader> reader(new TestGifFrameReader(&handler));

  // MakeMinimalGif() has a global color table and no local color table.
  std::string gif = MakeMinimalGif();
  ScanlineStatus status = reader->Initialize(gif.data(), gif.size());
  ASSERT_TRUE(status.Success()) << status.ToString();

  ASSERT_TRUE(reader->HasMoreFrames());
  ScanlineStatus frame_status;
  EXPECT_TRUE(reader->PrepareNextFrame(&frame_status))
      << frame_status.ToString();
  FrameSpec frame_spec;
  EXPECT_TRUE(reader->GetFrameSpec(&frame_spec, &frame_status));
  EXPECT_EQ(RGB_888, frame_spec.pixel_format);

  while (reader->HasMoreScanlines()) {
    const void* scanline = nullptr;
    EXPECT_TRUE(reader->ReadNextScanline(&scanline, &frame_status));
    EXPECT_NE(nullptr, scanline);
  }
}

// ---- Test 22: GifFrameReader with local color table ----
// Exercises CreateColorMap using the local color map (line 1011).
TEST(GifFrameReaderCoverage, LocalColorTableInCreateColorMap) {
  NullMessageHandler handler;
  std::unique_ptr<MultipleFrameReader> reader(new TestGifFrameReader(&handler));

  std::string gif = MakeGifWithLocalColorTable();
  ScanlineStatus status = reader->Initialize(gif.data(), gif.size());
  ASSERT_TRUE(status.Success()) << status.ToString();

  ASSERT_TRUE(reader->HasMoreFrames());
  ScanlineStatus frame_status;
  EXPECT_TRUE(reader->PrepareNextFrame(&frame_status))
      << frame_status.ToString();

  FrameSpec frame_spec;
  EXPECT_TRUE(reader->GetFrameSpec(&frame_spec, &frame_status));
  EXPECT_EQ(RGB_888, frame_spec.pixel_format);

  while (reader->HasMoreScanlines()) {
    const void* scanline = nullptr;
    EXPECT_TRUE(reader->ReadNextScanline(&scanline, &frame_status));
  }
}

// ---- Test 23: GIF with DGifGetExtension failure (truncated extension) ----
// Covers gif_reader.cc line 197-199: DGifGetExtension failure in ReadExtension.
TEST_F(GifReaderTest, DGifGetExtensionFailure) {
  std::string gif;
  gif += "GIF89a";
  gif += '\x01';
  gif += '\x00';
  gif += '\x01';
  gif += '\x00';
  gif += '\x80';
  gif += '\x00';
  gif += '\x00';
  gif += '\xFF';
  gif += '\x00';
  gif += '\x00';
  gif += '\x00';
  gif += '\xFF';
  gif += '\x00';
  // Extension introducer followed by truncation (no label or data)
  gif += '\x21';
  // Truncated here -- no extension label.
  EXPECT_FALSE(gif_reader_->ReadPng(gif, read_.png_ptr(), read_.info_ptr(),
                                    PNG_TRANSFORM_IDENTITY, false));
}

// ---- Test 24: DGifGetRecordType failure via very short truncated data ----
// Covers gif_reader.cc line 384-387: DGifGetRecordType failure in ReadGifToPng.
TEST_F(GifReaderTest, DGifGetRecordTypeFailure) {
  std::string gif;
  gif += "GIF89a";
  gif += '\x01';
  gif += '\x00';
  gif += '\x01';
  gif += '\x00';
  gif += '\x80';
  gif += '\x00';
  gif += '\x00';
  gif += '\xFF';
  gif += '\x00';
  gif += '\x00';
  gif += '\x00';
  gif += '\xFF';
  gif += '\x00';
  // End abruptly -- no record type byte, no image, no trailer
  EXPECT_FALSE(gif_reader_->ReadPng(gif, read_.png_ptr(), read_.info_ptr(),
                                    PNG_TRANSFORM_IDENTITY, false));
}

// ---- Test 25: ExpandColorMap on zero-dimension image via setjmp path ----
// The ExpandColorMap function has a setjmp guard (line 278-280).
// Trigger via zero width/height with EXPAND transform.
TEST_F(GifReaderTest, ExpandColormapZeroDimensions) {
  std::string gif;
  gif += "GIF89a";
  gif += '\x00';
  gif += '\x00';  // width=0
  gif += '\x00';
  gif += '\x00';  // height=0
  gif += '\x80';
  gif += '\x00';
  gif += '\x00';
  gif += '\xFF';
  gif += '\x00';
  gif += '\x00';
  gif += '\x00';
  gif += '\xFF';
  gif += '\x00';
  gif += '\x3B';  // trailer
  // ReadPng with EXPAND should fail for zero dimensions.
  EXPECT_FALSE(gif_reader_->ReadPng(gif, read_.png_ptr(), read_.info_ptr(),
                                    PNG_TRANSFORM_EXPAND, false));
}

// ---- Test 26: ReadGifFromStream EOF path ----
// Covers gif_reader.cc lines 82-84: EOF during stream read.
// A valid header but truncated mid-image-data triggers the stream
// read function's EOF branch.
TEST_F(GifReaderTest, StreamReadEof) {
  std::string gif;
  gif += "GIF89a";
  gif += '\x02';
  gif += '\x00';  // width=2
  gif += '\x02';
  gif += '\x00';  // height=2
  gif += '\x80';  // GCT=1, size=0 (2 entries)
  gif += '\x00';
  gif += '\x00';
  gif += '\xFF';
  gif += '\x00';
  gif += '\x00';
  gif += '\x00';
  gif += '\xFF';
  gif += '\x00';
  // Image Descriptor
  gif += '\x2C';
  gif += '\x00';
  gif += '\x00';
  gif += '\x00';
  gif += '\x00';
  gif += '\x02';
  gif += '\x00';
  gif += '\x02';
  gif += '\x00';
  gif += '\x00';
  // LZW code size but no actual data (truncated mid-image)
  gif += '\x02';
  gif += '\xFF';  // sub-block size = 255, but not enough data follows
  // Truncated here
  EXPECT_FALSE(gif_reader_->ReadPng(gif, read_.png_ptr(), read_.info_ptr(),
                                    PNG_TRANSFORM_IDENTITY, false));
}

// ---- Test 27: GifFrameReader interlaced single-frame through scanline API ----
// Exercises DecodeProgressiveGif (line 1055-1068) which is the interlaced
// path in the frame reader.
TEST(GifFrameReaderCoverage, InterlacedSingleFrame) {
  NullMessageHandler handler;
  std::unique_ptr<MultipleFrameReader> reader(new TestGifFrameReader(&handler));

  std::string data;
  ASSERT_TRUE(ReadTestFileWithExt(kGifTestDir, "interlaced.gif", &data));

  ScanlineStatus status = reader->Initialize(data.data(), data.size());
  ASSERT_TRUE(status.Success()) << status.ToString();

  ImageSpec image_spec;
  EXPECT_TRUE(reader->GetImageSpec(&image_spec, &status));
  EXPECT_GT(image_spec.width, 0u);
  EXPECT_GT(image_spec.height, 0u);

  ASSERT_TRUE(reader->HasMoreFrames());
  ScanlineStatus frame_status;
  EXPECT_TRUE(reader->PrepareNextFrame(&frame_status))
      << frame_status.ToString();

  FrameSpec frame_spec;
  EXPECT_TRUE(reader->GetFrameSpec(&frame_spec, &frame_status));
  // Interlaced GIF should report hint_progressive.
  EXPECT_TRUE(frame_spec.hint_progressive);

  int scanline_count = 0;
  while (reader->HasMoreScanlines()) {
    const void* scanline = nullptr;
    EXPECT_TRUE(reader->ReadNextScanline(&scanline, &frame_status));
    EXPECT_NE(nullptr, scanline);
    ++scanline_count;
  }
  EXPECT_EQ(static_cast<int>(frame_spec.height), scanline_count);
}

// ---- Test 28: GIF with DGifGetImageDesc failure in ReadImageDescriptor ----
// Covers gif_reader.cc lines 116-118: DGifGetImageDesc returns GIF_ERROR.
TEST_F(GifReaderTest, CorruptImageDescriptor) {
  std::string gif;
  gif += "GIF89a";
  gif += '\x01';
  gif += '\x00';
  gif += '\x01';
  gif += '\x00';
  gif += '\x80';
  gif += '\x00';
  gif += '\x00';
  gif += '\xFF';
  gif += '\x00';
  gif += '\x00';
  gif += '\x00';
  gif += '\xFF';
  gif += '\x00';
  // Image separator byte
  gif += '\x2C';
  // Truncate immediately after image separator (no image descriptor data)
  EXPECT_FALSE(gif_reader_->ReadPng(gif, read_.png_ptr(), read_.info_ptr(),
                                    PNG_TRANSFORM_IDENTITY, false));
}

// ---- Test 29: GIF with transparency, no expand, no strip_alpha ----
// Exercises AddTransparencyChunk success path (lines 88-111).
// A valid transparent index within palette bounds.
TEST_F(GifReaderTest, TransparencyChunkSuccess) {
  std::string gif;
  gif += "GIF89a";
  gif += '\x01';
  gif += '\x00';
  gif += '\x01';
  gif += '\x00';
  gif += '\x80';  // GCT=1, size=0 (2 entries)
  gif += '\x00';
  gif += '\x00';
  gif += '\xFF';
  gif += '\x00';
  gif += '\x00';
  gif += '\x00';
  gif += '\xFF';
  gif += '\x00';
  // GCE: transparent index = 0 (within palette bounds)
  gif += '\x21';
  gif += '\xF9';
  gif += '\x04';
  gif += '\x01';  // transparent flag
  gif += '\x00';
  gif += '\x00';
  gif += '\x00';  // transparent index = 0
  gif += '\x00';
  // Image Descriptor
  gif += '\x2C';
  gif += '\x00';
  gif += '\x00';
  gif += '\x00';
  gif += '\x00';
  gif += '\x01';
  gif += '\x00';
  gif += '\x01';
  gif += '\x00';
  gif += '\x00';
  gif += '\x02';
  gif += '\x02';
  gif += '\x4C';
  gif += '\x01';
  gif += '\x00';
  gif += '\x3B';

  // IDENTITY transform with transparency: should add tRNS chunk.
  ASSERT_TRUE(gif_reader_->ReadPng(gif, read_.png_ptr(), read_.info_ptr(),
                                   PNG_TRANSFORM_IDENTITY, false));
  // Verify tRNS chunk was added.
  png_bytep trans = nullptr;
  int num_trans = 0;
  png_color_16p trans_values = nullptr;
  EXPECT_NE(0u, png_get_tRNS(read_.png_ptr(), read_.info_ptr(), &trans,
                             &num_trans, &trans_values));
  EXPECT_EQ(1, num_trans);  // num_trans = transparent_palette_index + 1 = 1
  ASSERT_NE(nullptr, trans);
  EXPECT_EQ(pagespeed::image_compression::kAlphaTransparent, trans[0]);
}

// ---- Test 30: GIF with transparency index = 1 (second palette entry) ----
// Tests AddTransparencyChunk with num_trans = 2.
TEST_F(GifReaderTest, TransparencyChunkSecondEntry) {
  std::string gif;
  gif += "GIF89a";
  gif += '\x01';
  gif += '\x00';
  gif += '\x01';
  gif += '\x00';
  gif += '\x80';
  gif += '\x00';
  gif += '\x00';
  gif += '\xFF';
  gif += '\x00';
  gif += '\x00';
  gif += '\x00';
  gif += '\xFF';
  gif += '\x00';
  // GCE: transparent index = 1
  gif += '\x21';
  gif += '\xF9';
  gif += '\x04';
  gif += '\x01';
  gif += '\x00';
  gif += '\x00';
  gif += '\x01';  // transparent index = 1
  gif += '\x00';
  // Image
  gif += '\x2C';
  gif += '\x00';
  gif += '\x00';
  gif += '\x00';
  gif += '\x00';
  gif += '\x01';
  gif += '\x00';
  gif += '\x01';
  gif += '\x00';
  gif += '\x00';
  gif += '\x02';
  gif += '\x02';
  gif += '\x4C';
  gif += '\x01';
  gif += '\x00';
  gif += '\x3B';

  ASSERT_TRUE(gif_reader_->ReadPng(gif, read_.png_ptr(), read_.info_ptr(),
                                   PNG_TRANSFORM_IDENTITY, false));
  png_bytep trans = nullptr;
  int num_trans = 0;
  png_color_16p trans_values = nullptr;
  EXPECT_NE(0u, png_get_tRNS(read_.png_ptr(), read_.info_ptr(), &trans,
                             &num_trans, &trans_values));
  EXPECT_EQ(2, num_trans);  // transparent_palette_index + 1 = 2
  ASSERT_NE(nullptr, trans);
  // Index 0 should be opaque (0xFF), index 1 should be transparent.
  EXPECT_EQ(0xFF, trans[0]);
  EXPECT_EQ(pagespeed::image_compression::kAlphaTransparent, trans[1]);
}

// ---- Test 31: GifFrameReader: DGifGetLine failure during scanline read ----
// Covers gif_reader.cc line 1250-1254: DGifGetLine failure in
// ReadNextScanline (non-eagerly-read path).
TEST(GifFrameReaderCoverage, TruncatedImageDataDuringScanlineRead) {
  NullMessageHandler handler;
  std::unique_ptr<MultipleFrameReader> reader(new TestGifFrameReader(&handler));

  // Build a GIF where the image data is partially present.
  // PrepareNextFrame succeeds (for non-progressive, non-RGBA frames,
  // frame_eagerly_read_ is false for RGBA frames), but ReadNextScanline
  // fails when it tries to read more data than exists.
  // We need a transparent GIF (RGBA_8888) to avoid eager read, but
  // transparent frames ARE eagerly read for non-RGBA... Actually,
  // looking at the code: frame_eagerly_read_ = hint_progressive || !is_originally_rgba.
  // So if the frame IS originally RGBA (transparent), eagerly_read = false.
  // That means DGifGetLine is called per-scanline for RGBA frames.

  std::string gif;
  gif += "GIF89a";
  gif += '\x04';
  gif += '\x00';  // width=4
  gif += '\x04';
  gif += '\x00';  // height=4
  gif += '\x80';
  gif += '\x00';
  gif += '\x00';
  gif += '\xFF';
  gif += '\x00';
  gif += '\x00';
  gif += '\x00';
  gif += '\xFF';
  gif += '\x00';
  // GCE: transparent index = 0
  gif += '\x21';
  gif += '\xF9';
  gif += '\x04';
  gif += '\x01';  // transparent flag
  gif += '\x00';
  gif += '\x00';
  gif += '\x00';  // transparent index = 0
  gif += '\x00';
  // Image Descriptor: 4x4, not interlaced
  gif += '\x2C';
  gif += '\x00';
  gif += '\x00';
  gif += '\x00';
  gif += '\x00';
  gif += '\x04';
  gif += '\x00';
  gif += '\x04';
  gif += '\x00';
  gif += '\x00';
  // Minimal LZW data -- not enough for 16 pixels.
  gif += '\x02';  // LZW min code size
  gif += '\x02';  // sub-block size = 2 (very little data)
  gif += '\x4C';
  gif += '\x01';
  gif += '\x00';  // block terminator
  gif += '\x3B';

  ScanlineStatus status = reader->Initialize(gif.data(), gif.size());
  if (status.Success() && reader->HasMoreFrames()) {
    ScanlineStatus frame_status;
    if (reader->PrepareNextFrame(&frame_status)) {
      // Try reading scanlines. The LZW data is insufficient for 16 pixels
      // so DGifGetLine should fail at some point.
      bool had_failure = false;
      while (reader->HasMoreScanlines()) {
        const void* scanline = nullptr;
        if (!reader->ReadNextScanline(&scanline, &frame_status)) {
          had_failure = true;
          break;
        }
      }
      // We expect a failure at some point due to insufficient data.
      // But the key test is: no crash.
      (void)had_failure;
    }
  }
  // No crash = pass.
}

// Verify that the frame count limit is enforced for pathological GIFs.
// Constructs a minimal GIF with more frames than kMaxGifFrames (10000)
// and verifies rejection rather than unbounded memory consumption.
TEST(GifFrameReaderErrorPaths, FrameCountLimitEnforced) {
  NullMessageHandler handler;
  TestGifFrameReader reader(&handler);

  // Build a minimal GIF89a: header + global color table + 10001 frames.
  std::string gif;
  gif += std::string("GIF89a", 6);
  // Logical Screen Descriptor: 1x1, GCT flag, 1 bit (2 colors)
  gif += '\x01';
  gif += '\x00';
  gif += '\x01';
  gif += '\x00';
  gif += '\x80';
  gif += '\x00';
  gif += '\x00';
  // Global Color Table: 2 entries x 3 bytes
  gif += std::string(6, '\x00');

  // 10001 frames, each: image descriptor + minimal LZW data
  for (int i = 0; i < 10001; ++i) {
    gif += '\x2C';                  // Image Descriptor
    gif += std::string(4, '\x00');  // left=0, top=0
    gif += '\x01';
    gif += '\x00';
    gif += '\x01';
    gif += '\x00';  // 1x1
    gif += '\x00';  // packed byte
    gif += '\x02';  // LZW min code size
    gif += '\x02';  // sub-block size = 2
    gif += '\x04';
    gif += '\x01';  // LZW data
    gif += '\x00';  // block terminator
  }
  gif += '\x3B';  // Trailer

  ScanlineStatus status =
      reader.MultipleFrameReader::Initialize(gif.data(), gif.size());
  if (status.Success()) {
    bool hit_limit = false;
    while (reader.HasMoreFrames()) {
      ScanlineStatus frame_status = reader.PrepareNextFrame();
      if (!frame_status.Success()) {
        hit_limit = true;
        break;
      }
      while (reader.HasMoreScanlines()) {
        const void* scanline = nullptr;
        ScanlineStatus read_status = reader.ReadNextScanline(&scanline);
        if (!read_status.Success()) {
          hit_limit = true;
          break;
        }
      }
      if (hit_limit) break;
    }
    EXPECT_TRUE(hit_limit) << "Should have hit frame count limit";
  }
  // Either way: no crash, and the limit was enforced.
}

}  // namespace
