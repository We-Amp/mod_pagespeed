// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.
//
// This file is derived from mod_pagespeed and has been substantially modified.
// Originally licensed under Apache License, Version 2.0.
// Copyright (c) 2010-2017 Google Inc.
// Copyright (c) 2018 The Apache Software Foundation.

// PixelFormatOptimizer: strips unused alpha channel
// (RGBA_8888 -> RGB_888 when alpha is all opaque).
// Ported from mod_pagespeed's
// pagespeed/kernel/image/pixel_format_optimizer.h

#ifndef PAGESPEED_LIB_IMAGE_PIXEL_FORMAT_OPTIMIZER_H_
#define PAGESPEED_LIB_IMAGE_PIXEL_FORMAT_OPTIMIZER_H_

#include <cstddef>
#include <cstdint>
#include <memory>

#include "lib/base/basictypes.h"
#include "lib/image/image_util.h"
#include "lib/image/scanline_interface.h"
#include "lib/image/scanline_status.h"

namespace pagespeed {

class MessageHandler;

namespace image_compression {

using net_instaweb::ScanlineReaderInterface;
using net_instaweb::ScanlineStatus;

// PixelFormatOptimizer removes an unused channel from the image. This
// corresponds to changing the pixel format to a more compact one.
// Currently it only removes opaque alpha channel and changes RGBA_8888
// to RGB_888.
//
// To determine if a channel is unused, PixelFormatOptimizer has to
// examine every pixel in the image. Thus, the entire image may be
// buffered before the first output scanline can be retrieved. However,
// as soon as PixelFormatOptimizer finds a pixel with all channels used,
// it will stop buffering and become ready to serve the first scanline.
class PixelFormatOptimizer : public ScanlineReaderInterface {
 public:
  explicit PixelFormatOptimizer(MessageHandler* handler);
  ~PixelFormatOptimizer() override;

  // PixelFormatOptimizer acquires ownership of reader, even in case
  // of failure.
  ScanlineStatus Initialize(ScanlineReaderInterface* reader);

  ScanlineStatus ReadNextScanlineWithStatus(void** out_scanline_bytes) override;

  // Resets the resizer to its initial state. Always returns true.
  bool Reset() override;

  // Returns number of bytes required to store a scanline.
  size_t GetBytesPerScanline() override { return bytes_per_row_; }

  // Returns true if there are more scanlines to read. Returns false
  // if the object has not been initialized or all of the scanlines
  // have been read.
  bool HasMoreScanLines() override { return (output_row_ < GetImageHeight()); }

  // Returns the height of the image.
  size_t GetImageHeight() override { return reader_->GetImageHeight(); }

  // Returns the width of the image.
  size_t GetImageWidth() override { return reader_->GetImageWidth(); }

  // Returns the pixel format of the image.
  PixelFormat GetPixelFormat() override { return pixel_format_; }

  // Returns true if the image is encoded in progressive / interlacing
  // format.
  bool IsProgressive() override { return reader_->IsProgressive(); }

  // This method should not be called. If it does get called, in DEBUG
  // mode it will throw a FATAL error and in RELEASE mode it does
  // nothing.
  ScanlineStatus InitializeWithStatus(const void* image_buffer,
                                      size_t buffer_length) override;

 private:
  std::unique_ptr<ScanlineReaderInterface> reader_;
  size_t bytes_per_row_;
  PixelFormat pixel_format_;
  size_t output_row_;
  bool strip_alpha_;
  bool was_initialized_;

  // Buffer for storing decoded scanlines.
  std::unique_ptr<uint8_t[]> input_lines_;

  // Number of rows which have been examined and buffered.
  size_t input_row_;

  // Buffer for storing a single converted scanline.
  std::unique_ptr<uint8_t[]> output_line_;

  MessageHandler* message_handler_;

  DISALLOW_COPY_AND_ASSIGN(PixelFormatOptimizer);
};

}  // namespace image_compression

}  // namespace pagespeed

#endif  // PAGESPEED_LIB_IMAGE_PIXEL_FORMAT_OPTIMIZER_H_
