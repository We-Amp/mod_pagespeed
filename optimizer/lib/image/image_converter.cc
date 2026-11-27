// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.
//
// This file is derived from mod_pagespeed and has been substantially modified.
// Originally licensed under Apache License, Version 2.0.
// Copyright (c) 2010-2017 Google Inc.
// Copyright (c) 2018 The Apache Software Foundation.

#include "lib/image/image_converter.h"

#include <cassert>
#include <csetjmp>
#include <cstddef>
#include <cstring>
#include <memory>

extern "C" {
#include <zlib.h>

#include "png.h"  // NOLINT
}  // extern "C"

#include "lib/base/message_handler.h"
#include "lib/image/image_frame_interface.h"
#include "lib/image/image_util.h"
#include "lib/image/jpeg_optimizer.h"
#include "lib/image/png_optimizer.h"
#include "lib/image/read_image.h"
#include "lib/image/scanline_interface.h"
#include "lib/image/scanline_interface_frame_adapter.h"
#include "lib/image/scanline_utils.h"

namespace {
// In some cases, converting a PNG to JPEG results in a smaller
// file. This is at the cost of switching from lossless to lossy, so
// we require that the savings are substantial before in order to do
// the conversion. We choose 80% size reduction as the minimum before
// we switch a PNG to JPEG.
const double kMinJpegSavingsRatio = 0.8;

// As above, but for use when comparing lossy WebPs to lossless
// formats.
const double kMinWebpSavingsRatio = 0.8;

// If 'new_image' and 'new_image_type' represent a valid image that is
// smaller than 'threshold_ratio' times the size of the current
// 'best_image' (if any), then updates 'best_image' and
// 'best_image_type' to point to the values of 'new_image' and
// 'new_image_type'.
void SelectSmallerImage(
    pagespeed::image_compression::ImageConverter::ImageType new_image_type,
    const std::string& new_image, const double threshold_ratio,
    pagespeed::image_compression::ImageConverter::ImageType* const
        best_image_type,
    const std::string** const best_image, pagespeed::MessageHandler* handler) {
  size_t new_image_size = new_image.size();
  if (new_image_size > 0 &&
      ((*best_image_type ==
        pagespeed::image_compression::ImageConverter::IMAGE_NONE) ||
       ((new_image_type !=
         pagespeed::image_compression::ImageConverter::IMAGE_NONE) &&
        (*best_image != nullptr) &&
        (new_image_size < (*best_image)->size() * threshold_ratio)))) {
    *best_image_type = new_image_type;
    *best_image = &new_image;

    PS_DLOG_INFO(handler, "%p best is now %d",
                 static_cast<void*>(best_image_type), new_image_type);
  }
}

// To estimate the number of bytes from the number of pixels, we
// divide by a magic ratio.
double JpegPixelToByteRatio(int compression_level) {
  if ((compression_level > 95) || (compression_level < 0)) {
    compression_level = 95;
  }
  double kSlope = 4.92865674;
  double kIntercept = 0.04177743;
  double ratio = kSlope / (110.0 - compression_level) + kIntercept;
  return ratio;
}

}  // namespace

namespace pagespeed::image_compression {

using net_instaweb::SCANLINE_STATUS_SUCCESS;
using net_instaweb::ScanlineStatus;

ScanlineStatus ImageConverter::ConvertImageWithStatus(
    ScanlineReaderInterface* reader, ScanlineWriterInterface* writer) {
  void* scan_row;
  while (reader->HasMoreScanLines()) {
    ScanlineStatus reader_status =
        reader->ReadNextScanlineWithStatus(&scan_row);
    if (!reader_status.Success()) {
      return reader_status;
    }
    ScanlineStatus writer_status =
        writer->WriteNextScanlineWithStatus(scan_row);
    if (!writer_status.Success()) {
      return writer_status;
    }
  }

  ScanlineStatus writer_status = writer->FinalizeWriteWithStatus();
  if (!writer_status.Success()) {
    return writer_status;
  }

  return ScanlineStatus(SCANLINE_STATUS_SUCCESS);
}

ScanlineStatus ImageConverter::ConvertMultipleFrameImage(
    MultipleFrameReader* reader, MultipleFrameWriter* writer) {
  ImageSpec image_spec;
  FrameSpec frame_spec;
  const void* scan_row = nullptr;

  ScanlineStatus status;
  if (reader->GetImageSpec(&image_spec, &status) &&
      writer->PrepareImage(&image_spec, &status)) {
    while (reader->HasMoreFrames() && reader->PrepareNextFrame(&status) &&
           reader->GetFrameSpec(&frame_spec, &status) &&
           writer->PrepareNextFrame(&frame_spec, &status)) {
      while (reader->HasMoreScanlines() &&
             reader->ReadNextScanline(&scan_row, &status) &&
             writer->WriteNextScanline(scan_row, &status)) {
        // intentional empty loop body
      }
    }
  }
  if (status.Success()) {
    writer->FinalizeWrite(&status);
  } else {
    // Don't overwrite the earlier error status from Read/Write failure.
    ScanlineStatus unused;
    writer->FinalizeWrite(&unused);
  }
  return status;
}

bool ImageConverter::ConvertPngToJpeg(
    const PngReaderInterface& png_struct_reader, const std::string& in,
    const JpegCompressionOptions& options, std::string* out,
    MessageHandler* handler) {
  assert(out->empty());
  out->clear();

  // Initialize the reader.
  PngScanlineReader png_reader(handler);

  // Since JPEG only support 8 bits/channels, we need convert PNG
  // having 1,2,4,16 bits/channel to 8 bits/channel.
  png_reader.set_transform(PNG_TRANSFORM_EXPAND | PNG_TRANSFORM_STRIP_16);

  // Since JPEGs can only support opaque images, require this in the
  // reader.
  png_reader.set_require_opaque(true);

  // Configure png reader error handlers.
  if (setjmp(*png_reader.GetJmpBuf())) {
    PS_LOG_INFO(handler, "libpng failed to decode the PNG image.");
    return false;
  }

  if (!png_reader.InitializeRead(png_struct_reader, in)) {
    return false;
  }

  // Try converting if the image is opaque. The flag lives across the
  // setjmp below and is READ on the longjmp path (the function returns
  // it), so it must be volatile: after a longjmp, the values of
  // non-volatile locals modified since setjmp are indeterminate.
  volatile bool jpeg_success = false;
  size_t width = png_reader.GetImageWidth();
  size_t height = png_reader.GetImageHeight();
  PixelFormat format = png_reader.GetPixelFormat();

  if (height > 0 && width > 0 && format != UNSUPPORTED) {
    JpegScanlineWriter jpeg_writer(handler);

    // libjpeg's error handling mechanism requires that longjmp be
    // used to get control after an error.
    jmp_buf env;
    if (setjmp(env)) {
      // This code is run only when libjpeg hit an error, and called
      // longjmp(env).
      jpeg_writer.AbortWrite();
    } else {
      jpeg_writer.SetJmpBufEnv(&env);
      if (jpeg_writer.Init(width, height, format)) {
        jpeg_writer.InitializeWrite(&options, out);
        jpeg_success = ConvertImage(&png_reader, &jpeg_writer);
      }
    }
  }
  return jpeg_success;
}

bool ImageConverter::OptimizePngOrConvertToJpeg(
    const PngReaderInterface& png_struct_reader, const std::string& in,
    const JpegCompressionOptions& options, std::string* out, bool* is_out_png,
    MessageHandler* handler) {
  bool jpeg_success =
      ConvertPngToJpeg(png_struct_reader, in, options, out, handler);

  // Try Optimizing the PNG.
  std::string optimized_png_out;
  bool png_success = PngOptimizer::OptimizePngBestCompression(
      png_struct_reader, in, &optimized_png_out, handler);

  // Consider using jpeg's only if it gives substantial amount of byte
  // savings.
  if (png_success &&
      (!jpeg_success ||
       out->size() > kMinJpegSavingsRatio * optimized_png_out.size())) {
    out->clear();
    out->assign(optimized_png_out);
    *is_out_png = true;
  } else {
    *is_out_png = false;
  }

  return jpeg_success || png_success;
}

bool ImageConverter::ConvertPngToWebp(
    const PngReaderInterface& png_struct_reader, const std::string& in,
    const WebpConfiguration& webp_config, std::string* const out,
    bool* is_opaque, MessageHandler* handler) {
  ScanlineWriterInterface* webp_writer = nullptr;
  bool success = ConvertPngToWebp(png_struct_reader, in, webp_config, out,
                                  is_opaque, &webp_writer, handler);
  delete webp_writer;
  return success;
}

bool ImageConverter::ConvertPngToWebp(
    const PngReaderInterface& png_struct_reader, const std::string& in,
    const WebpConfiguration& webp_config, std::string* const out,
    bool* is_opaque, ScanlineWriterInterface** webp_writer,
    MessageHandler* handler) {
  assert(out->empty());
  out->clear();

  if (*webp_writer != nullptr) {
    PS_LOG_DFATAL(handler, "Expected *webp_writer == NULL");
    return false;
  }

  // Initialize the reader.
  PngScanlineReader png_reader(handler);

  // Since the WebP API only support 8 bits/channels, we need convert
  // PNG having 1,2,4,16 bits/channel to 8 bits/channel.
  png_reader.set_transform(PNG_TRANSFORM_EXPAND | PNG_TRANSFORM_STRIP_16 |
                           PNG_TRANSFORM_GRAY_TO_RGB);

  // If alpha quality is zero, refuse to process transparent images.
  png_reader.set_require_opaque(webp_config.alpha_quality == 0);

  // Configure png reader error handlers.
  if (setjmp(*png_reader.GetJmpBuf())) {
    PS_LOG_INFO(handler, "libpng failed to decoded the PNG image.");
    return false;
  }
  if (!png_reader.InitializeRead(png_struct_reader, in, is_opaque)) {
    return false;
  }

  bool webp_success = false;
  size_t width = png_reader.GetImageWidth();
  size_t height = png_reader.GetImageHeight();
  PixelFormat format = png_reader.GetPixelFormat();

  (*webp_writer) =
      new FrameToScanlineWriterAdapter(new WebpFrameWriter(handler));

  if (height > 0 && width > 0 && format != UNSUPPORTED) {
    if ((*webp_writer)->Init(width, height, format) &&
        (*webp_writer)->InitializeWrite(&webp_config, out)) {
      webp_success = ConvertImage(&png_reader, *webp_writer);
    }
  }

  return webp_success;
}

bool ImageConverter::ConvertGifToWebp(const std::string& in,
                                      const WebpConfiguration& webp_config,
                                      std::string* out,
                                      MessageHandler* handler) {
  out->clear();
  ScanlineStatus status;

  // Create the GIF frame reader.
  std::unique_ptr<MultipleFrameReader> gif_reader(
      CreateImageFrameReader(pagespeed::image_compression::IMAGE_GIF, in.data(),
                             in.size(), QUIRKS_CHROME, handler, &status));
  if (gif_reader == nullptr || !status.Success()) {
    return false;
  }

  // Create the WebP frame writer.
  std::unique_ptr<MultipleFrameWriter> webp_writer(
      CreateImageFrameWriter(pagespeed::image_compression::IMAGE_WEBP,
                             &webp_config, out, handler, &status));
  if (webp_writer == nullptr || !status.Success()) {
    return false;
  }

  // Convert using the frame-based pipeline.
  status = ConvertMultipleFrameImage(gif_reader.get(), webp_writer.get());
  return status.Success();
}

ImageConverter::ImageType ImageConverter::GetSmallestOfPngJpegWebp(
    const PngReaderInterface& png_struct_reader, const std::string& in,
    const JpegCompressionOptions* jpeg_options,
    const WebpConfiguration* webp_config, std::string* out,
    MessageHandler* handler) {
  std::string jpeg_out, png_out, webp_lossless_out, webp_lossy_out;
  const std::string* best_lossless_image = nullptr;
  const std::string* best_lossy_image = nullptr;
  const std::string* best_image = nullptr;
  ImageType best_lossless_image_type = IMAGE_NONE;
  ImageType best_lossy_image_type = IMAGE_NONE;
  ImageType best_image_type = IMAGE_NONE;

  ScanlineWriterInterface* webp_writer = nullptr;
  WebpConfiguration webp_config_lossless;
  webp_config_lossless.lossless = 1;  // Explicit (also the default).
  bool is_opaque = false;
  if (!ConvertPngToWebp(png_struct_reader, in, webp_config_lossless,
                        &webp_lossless_out, &is_opaque, &webp_writer,
                        handler)) {
    PS_DLOG_INFO(handler, "Could not convert image to lossless WebP");
    webp_lossless_out.clear();
  }
  if ((webp_config != nullptr) && (webp_writer != nullptr) &&
      (!webp_writer->InitializeWrite(webp_config, &webp_lossy_out) ||
       !webp_writer->FinalizeWrite())) {
    PS_DLOG_INFO(handler, "Could not convert image to custom WebP");
    webp_lossy_out.clear();
  }
  delete webp_writer;

  if (!PngOptimizer::OptimizePngBestCompression(png_struct_reader, in, &png_out,
                                                handler)) {
    PS_DLOG_INFO(handler, "Could not optimize PNG");
    png_out.clear();
  }

  // If jpeg options are passed in and we haven't determined for sure
  // that the image has transparency, try jpeg conversion.
  if ((jpeg_options != nullptr) && (webp_lossy_out.empty() || is_opaque) &&
      !ConvertPngToJpeg(png_struct_reader, in, *jpeg_options, &jpeg_out,
                        handler)) {
    PS_DLOG_INFO(handler, "Could not convert image to JPEG");
    jpeg_out.clear();
  }

  SelectSmallerImage(IMAGE_NONE, in, 1, &best_lossless_image_type,
                     &best_lossless_image, handler);
  SelectSmallerImage(IMAGE_WEBP, webp_lossless_out, 1,
                     &best_lossless_image_type, &best_lossless_image, handler);
  SelectSmallerImage(IMAGE_PNG, png_out, 1, &best_lossless_image_type,
                     &best_lossless_image, handler);

  SelectSmallerImage(IMAGE_WEBP, webp_lossy_out, 1, &best_lossy_image_type,
                     &best_lossy_image, handler);
  SelectSmallerImage(IMAGE_JPEG, jpeg_out, 1, &best_lossy_image_type,
                     &best_lossy_image, handler);

  // To compensate for the lower quality, the lossy images must be
  // substantially smaller than the lossless images.
  double threshold_ratio =
      (best_lossy_image_type == IMAGE_WEBP ? kMinWebpSavingsRatio
                                           : kMinJpegSavingsRatio);
  best_image_type = best_lossless_image_type;
  best_image = best_lossless_image;
  if (best_lossy_image != nullptr) {
    SelectSmallerImage(best_lossy_image_type, *best_lossy_image,
                       threshold_ratio, &best_image_type, &best_image, handler);
  }

  out->clear();
  out->assign((best_image != nullptr) ? *best_image : in);

  return best_image_type;
}

bool GenerateBlankImage(size_t width, size_t height, bool has_transparency,
                        std::string* output, MessageHandler* handler) {
  // Create a PNG writer with no compression.
  PngCompressParams config(PNG_FILTER_NONE, Z_NO_COMPRESSION);
  PixelFormat pixel_format = (has_transparency ? RGBA_8888 : RGB_888);

  std::unique_ptr<ScanlineWriterInterface> png_writer(CreateScanlineWriter(
      IMAGE_PNG, pixel_format, width, height, &config, output, handler));
  if (png_writer == nullptr) {
    PS_LOG_ERROR(handler, "Failed to create an image writer.");
    return false;
  }

  // Create a transparent scanline.
  size_t num_channels = GetNumChannelsFromPixelFormat(pixel_format, handler);
  if (num_channels == 0 || width > SIZE_MAX / num_channels) {
    PS_LOG_ERROR(handler, "Image width too large for scanline buffer.");
    return false;
  }
  const size_t bytes_per_scanline = width * num_channels;
  std::unique_ptr<unsigned char[]> scanline(
      new unsigned char[bytes_per_scanline]);
  memset(scanline.get(), 0, bytes_per_scanline);

  // Fill the entire image with the blank scanline.
  for (int row = 0; row < static_cast<int>(height); ++row) {
    if (!png_writer->WriteNextScanline(
            reinterpret_cast<void*>(scanline.get()))) {
      return false;
    }
  }

  if (!png_writer->FinalizeWrite()) {
    return false;
  }
  return true;
}

bool ShouldConvertToProgressive(int64_t quality, int threshold, int num_bytes,
                                int desired_width, int desired_height) {
  bool progressive = false;

  if (num_bytes >= threshold) {
    progressive = true;
    int64_t num_pixels = static_cast<int64_t>(desired_width) * desired_height;

    double ratio = JpegPixelToByteRatio(quality);
    auto estimated_bytes =
        static_cast<int64_t>(static_cast<double>(num_pixels) * ratio);

    if (estimated_bytes < threshold) {
      progressive = false;
    }
  }
  return progressive;
}

}  // namespace pagespeed::image_compression
