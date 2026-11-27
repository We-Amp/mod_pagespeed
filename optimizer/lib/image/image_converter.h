// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.
//
// This file is derived from mod_pagespeed and has been substantially modified.
// Originally licensed under Apache License, Version 2.0.
// Copyright (c) 2010-2017 Google Inc.
// Copyright (c) 2018 The Apache Software Foundation.

// ImageConverter: format conversion between image formats.
// Ported from mod_pagespeed's
// pagespeed/kernel/image/image_converter.h
// optipng dependency has been removed.

#ifndef PAGESPEED_LIB_IMAGE_IMAGE_CONVERTER_H_
#define PAGESPEED_LIB_IMAGE_IMAGE_CONVERTER_H_

#include <cstddef>
#include <cstdint>
#include <string>

#include "lib/base/basictypes.h"
#include "lib/image/image_frame_interface.h"
#include "lib/image/jpeg_optimizer.h"
#include "lib/image/scanline_interface.h"
#include "lib/image/scanline_status.h"
#include "lib/image/webp_optimizer.h"

namespace pagespeed {

class MessageHandler;

namespace image_compression {

using net_instaweb::ScanlineReaderInterface;
using net_instaweb::ScanlineStatus;
using net_instaweb::ScanlineWriterInterface;

class PngReaderInterface;

class ImageConverter {
 public:
  enum ImageType : std::uint8_t {
    IMAGE_NONE = 0,
    IMAGE_PNG,
    IMAGE_JPEG,
    IMAGE_WEBP
  };

  // Converts image one line at a time, between different image
  // formats. Both 'reader' and 'writer' must be non-NULL.
  static ScanlineStatus ConvertImageWithStatus(ScanlineReaderInterface* reader,
                                               ScanlineWriterInterface* writer);

  inline static bool ConvertImage(ScanlineReaderInterface* reader,
                                  ScanlineWriterInterface* writer) {
    return ConvertImageWithStatus(reader, writer).Success();
  }

  // Converts image frame by frame, and then line by line within each
  // frame, between different image formats. Both 'reader' and
  // 'writer' must be non-NULL.
  static ScanlineStatus ConvertMultipleFrameImage(MultipleFrameReader* reader,
                                                  MultipleFrameWriter* writer);

  static bool ConvertPngToJpeg(const PngReaderInterface& png_struct_reader,
                               const std::string& in,
                               const JpegCompressionOptions& options,
                               std::string* out, MessageHandler* handler);

  // Reads the PNG encoded in 'in' with 'png_struct_reader', encodes
  // it in WebP format using the options in 'config', and writes the
  // resulting WebP in 'out'. Note that if config.alpha_quality==0,
  // this function will fail when attempting to convert an image with
  // transparent pixels. Returns is_opaque set to true iff the 'in'
  // image was opaque.
  static bool ConvertPngToWebp(const PngReaderInterface& png_struct_reader,
                               const std::string& in,
                               const WebpConfiguration& config,
                               std::string* out, bool* is_opaque,
                               MessageHandler* handler);

  // Overload that also returns the webp_writer used.
  static bool ConvertPngToWebp(const PngReaderInterface& png_struct_reader,
                               const std::string& in,
                               const WebpConfiguration& config,
                               std::string* out, bool* is_opaque,
                               ScanlineWriterInterface** webp_writer,
                               MessageHandler* handler);

  // Optimizes the given png image, also converts to jpeg and take
  // the one that has smaller size and set the output. Returns false
  // if both of them fails.
  static bool OptimizePngOrConvertToJpeg(
      const PngReaderInterface& png_struct_reader, const std::string& in,
      const JpegCompressionOptions& options, std::string* out, bool* is_out_png,
      MessageHandler* handler);

  // Convert a GIF image to WebP format. Handles both static and
  // animated GIFs. For animated GIFs, produces animated WebP using the
  // frame-based pipeline. For static GIFs, produces a static WebP
  // image.
  static bool ConvertGifToWebp(const std::string& in,
                               const WebpConfiguration& webp_config,
                               std::string* out, MessageHandler* handler);

  // Populates 'out' with a version of the input image 'in' resulting
  // in the smallest size, and returns the corresponding ImageType.
  static ImageType GetSmallestOfPngJpegWebp(
      const PngReaderInterface& png_struct_reader, const std::string& in,
      const JpegCompressionOptions* jpeg_options,
      const WebpConfiguration* webp_config, std::string* out,
      MessageHandler* handler);

 private:
  ImageConverter();
  ~ImageConverter();

  DISALLOW_COPY_AND_ASSIGN(ImageConverter);
};

// Returns a blank PNG image with specified size and transparency.
bool GenerateBlankImage(size_t width, size_t height, bool has_transparency,
                        std::string* output, MessageHandler* handler);

// Returns whether progressive format will result in a smaller JPEG
// image.
bool ShouldConvertToProgressive(int64_t quality, int threshold, int num_bytes,
                                int desired_width, int desired_height);

}  // namespace image_compression

}  // namespace pagespeed

#endif  // PAGESPEED_LIB_IMAGE_IMAGE_CONVERTER_H_
