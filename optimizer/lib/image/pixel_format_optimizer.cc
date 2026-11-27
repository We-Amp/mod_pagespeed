// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.
//
// This file is derived from mod_pagespeed and has been substantially modified.
// Originally licensed under Apache License, Version 2.0.
// Copyright (c) 2010-2017 Google Inc.
// Copyright (c) 2018 The Apache Software Foundation.

#include "lib/image/pixel_format_optimizer.h"

#include <cstdint>
#include <cstring>

#include "lib/base/message_handler.h"
#include "lib/image/scanline_utils.h"

namespace pagespeed::image_compression {

using net_instaweb::SCANLINE_PIXEL_FORMAT_OPTIMIZER;
using net_instaweb::SCANLINE_STATUS_INTERNAL_ERROR;
using net_instaweb::SCANLINE_STATUS_INVOCATION_ERROR;
using net_instaweb::SCANLINE_STATUS_SUCCESS;
using net_instaweb::SCANLINE_STATUS_UNINITIALIZED;
using net_instaweb::SCANLINE_STATUS_UNSUPPORTED_FORMAT;
using net_instaweb::ScanlineStatus;

static const uint8_t OPAQUE_ALPHA = 0xFF;

PixelFormatOptimizer::PixelFormatOptimizer(MessageHandler* handler)
    : message_handler_(handler) {
  Reset();
}

PixelFormatOptimizer::~PixelFormatOptimizer() = default;

bool PixelFormatOptimizer::Reset() {
  bytes_per_row_ = 0;
  pixel_format_ = UNSUPPORTED;
  output_row_ = 0;
  strip_alpha_ = false;
  was_initialized_ = false;
  input_lines_.reset();
  input_row_ = 0;
  output_line_.reset();
  return true;
}

ScanlineStatus PixelFormatOptimizer::InitializeWithStatus(
    const void* /* image_buffer */, size_t /* buffer_length */) {
  return PS_LOGGED_STATUS(PS_LOG_DFATAL, message_handler_,
                          SCANLINE_STATUS_INVOCATION_ERROR,
                          SCANLINE_PIXEL_FORMAT_OPTIMIZER,
                          "Unexpected call to InitializeWithStatus()");
}

// Initializes the object and determines whether the alpha channel
// should be removed (i.e., the image has a fully opaque alpha
// channel).
ScanlineStatus PixelFormatOptimizer::Initialize(
    ScanlineReaderInterface* reader) {
  Reset();

  if (reader == nullptr || reader->GetPixelFormat() == UNSUPPORTED ||
      reader->GetImageWidth() == 0 || reader->GetImageHeight() == 0) {
    return PS_LOGGED_STATUS(
        PS_LOG_INFO, message_handler_, SCANLINE_STATUS_UNINITIALIZED,
        SCANLINE_PIXEL_FORMAT_OPTIMIZER, "Invalid input image.");
  }

  reader_.reset(reader);
  pixel_format_ = reader_->GetPixelFormat();
  bytes_per_row_ = reader_->GetBytesPerScanline();

  // Only strip alpha for RGBA_8888 format.
  if (pixel_format_ != RGBA_8888) {
    strip_alpha_ = false;
    was_initialized_ = true;
    return ScanlineStatus(SCANLINE_STATUS_SUCCESS);
  }

  const size_t image_width = reader_->GetImageWidth();
  const size_t image_height = reader_->GetImageHeight();

  // The whole image is buffered below to avoid decoding it twice; refuse
  // dimensions implying a buffer beyond anything we would ever decode.
  if (static_cast<uint64_t>(image_width) * image_height >
      kScanlineMaxImagePixels) {
    Reset();
    return PS_LOGGED_STATUS(PS_LOG_INFO, message_handler_,
                            SCANLINE_STATUS_UNSUPPORTED_FORMAT,
                            SCANLINE_PIXEL_FORMAT_OPTIMIZER,
                            "Image dimensions %zux%zu exceed the supported "
                            "maximum.",
                            image_width, image_height);
  }

  // Guard against multiplication overflow for allocation size.
  if (bytes_per_row_ > 0 && image_height > SIZE_MAX / bytes_per_row_) {
    Reset();
    return PS_LOGGED_STATUS(PS_LOG_ERROR, message_handler_,
                            SCANLINE_STATUS_INVOCATION_ERROR,
                            SCANLINE_PIXEL_FORMAT_OPTIMIZER,
                            "image too large: overflow in "
                            "image_height * bytes_per_row_");
  }

  // Now check if the alpha channel is opaque. To avoid decoding the
  // image twice, the decoded scanlines will be stored in
  // 'input_lines_'.
  input_lines_.reset(new uint8_t[image_height * bytes_per_row_]);
  uint8_t* current_scanline = input_lines_.get();
  const size_t num_channels =
      GetNumChannelsFromPixelFormat(pixel_format_, message_handler_);

  input_row_ = 0;
  while (input_row_ < image_height) {
    void* in_scanline = nullptr;
    ScanlineStatus status = reader_->ReadNextScanlineWithStatus(&in_scanline);
    if (!status.Success()) {
      Reset();
      return status;
    }

    // Buffer the scanline.
    memcpy(current_scanline, in_scanline, bytes_per_row_);
    ++input_row_;

    // Check if the current scanline is opaque or not. Alpha is the
    // last channel.
    for (size_t ch = num_channels - 1; ch < image_width * num_channels;
         ch += num_channels) {
      if (current_scanline[ch] != OPAQUE_ALPHA) {
        strip_alpha_ = false;
        was_initialized_ = true;
        return ScanlineStatus(SCANLINE_STATUS_SUCCESS);
      }
    }
    current_scanline += bytes_per_row_;
  }

  // Now we know that the alpha channel is opaque. We will modify the
  // pixel format and allocate memory for the stripped scanlines.
  strip_alpha_ = true;
  pixel_format_ = RGB_888;
  bytes_per_row_ = image_width * GetNumChannelsFromPixelFormat(
                                     pixel_format_, message_handler_);
  output_line_.reset(new uint8_t[bytes_per_row_]);
  was_initialized_ = true;
  return ScanlineStatus(SCANLINE_STATUS_SUCCESS);
}

// Reads a scanline if that's available; or returns an error
// otherwise. This method is called after Initialize(), which already
// determined whether the alpha channel should be stripped. If it does,
// the returned scanline will have alpha removed; otherwise, the
// scanline will be copied without modification.
ScanlineStatus PixelFormatOptimizer::ReadNextScanlineWithStatus(
    void** out_scanline_bytes) {
  if (!was_initialized_) {
    return PS_LOGGED_STATUS(PS_LOG_DFATAL, message_handler_,
                            SCANLINE_STATUS_INVOCATION_ERROR,
                            SCANLINE_PIXEL_FORMAT_OPTIMIZER, "Uninitialized");
  }

  if (!HasMoreScanLines()) {
    return PS_LOGGED_STATUS(
        PS_LOG_INFO, message_handler_, SCANLINE_STATUS_INVOCATION_ERROR,
        SCANLINE_PIXEL_FORMAT_OPTIMIZER, "No more scanlines");
  }

  if (strip_alpha_) {
    const int bytes_per_in_pixel =
        GetNumChannelsFromPixelFormat(RGBA_8888, message_handler_);
    const int bytes_per_out_pixel =
        GetNumChannelsFromPixelFormat(RGB_888, message_handler_);

    // If we have decided to strip the alpha channel, the entire input
    // image should have already been copied to 'input_lines_'. We
    // will grab the corresponding line in 'input_lines_', filter the
    // alpha, and store the results in the 'output_line_'.
    uint8_t* in_pixel =
        input_lines_.get() + output_row_ * reader_->GetBytesPerScanline();
    uint8_t* out_pixel = output_line_.get();

    const size_t image_width = reader_->GetImageWidth();
    for (size_t pixel = 0; pixel < image_width; ++pixel) {
      memcpy(out_pixel, in_pixel, bytes_per_out_pixel);
      in_pixel += bytes_per_in_pixel;
      out_pixel += bytes_per_out_pixel;
    }
    *out_scanline_bytes = output_line_.get();
  } else {
    // If we have decided NOT to strip the alpha channel, we may have
    // decoded a portion of the input image. We will grab the decoded
    // lines from 'input_lines_', and then decode the rest of the
    // image.
    if (output_row_ < input_row_) {
      *out_scanline_bytes =
          input_lines_.get() + output_row_ * reader_->GetBytesPerScanline();
    } else {
      if (!reader_->ReadNextScanline(out_scanline_bytes)) {
        Reset();
        return PS_LOGGED_STATUS(
            PS_LOG_INFO, message_handler_, SCANLINE_STATUS_INTERNAL_ERROR,
            SCANLINE_PIXEL_FORMAT_OPTIMIZER, "Failed to read a scanline.");
      }
    }
  }
  ++output_row_;
  return ScanlineStatus(SCANLINE_STATUS_SUCCESS);
}

}  // namespace pagespeed::image_compression
