// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.
//
// This file is derived from mod_pagespeed and has been substantially modified.
// Originally licensed under Apache License, Version 2.0.
// Copyright (c) 2010-2017 Google Inc.
// Copyright (c) 2018 The Apache Software Foundation.

// JPEG reader classes for PageSpeed 2.0.
// Ported from mod_pagespeed's pagespeed/kernel/image/jpeg_reader.h

#ifndef PAGESPEED_LIB_IMAGE_JPEG_READER_H_
#define PAGESPEED_LIB_IMAGE_JPEG_READER_H_

#include <cstddef>

#include "lib/base/basictypes.h"
#include "lib/image/image_util.h"
#include "lib/image/scanline_interface.h"
#include "lib/image/scanline_status.h"

struct jpeg_decompress_struct;
struct jpeg_error_mgr;

namespace pagespeed {
class MessageHandler;
}

namespace pagespeed {

namespace image_compression {

using ::net_instaweb::ScanlineStatus;

struct JpegEnv;

// A very thin wrapper that configures a jpeg_decompress_struct for
// reading from a string. The caller is responsible for
// configuring a jmp_buf and setting it as the client_data of the
// jpeg_decompress_struct, like so:
//
// jmp_buf env;
// if (setjmp(env)) {
//   // error handling
// }
// JpegReader reader;
// jpeg_decompress_struct* jpeg_decompress = reader.decompress_struct();
// jpeg_decompress->client_data = static_cast<void*>(&env);
// reader.PrepareForRead(src);
// // perform other operations on jpeg_decompress.
class JpegReader {
 public:
  explicit JpegReader(MessageHandler* handler);
  ~JpegReader();

  jpeg_decompress_struct* decompress_struct() const { return jpeg_decompress_; }

  // Returns true if both internal allocations succeeded. Callers must check
  // this before using decompress_struct() or PrepareForRead().
  bool is_valid() const;

  void PrepareForRead(const void* image_data, size_t image_length);

 private:
  jpeg_decompress_struct* jpeg_decompress_;
  jpeg_error_mgr* decompress_error_;
  [[maybe_unused]] MessageHandler* message_handler_;

  DISALLOW_COPY_AND_ASSIGN(JpegReader);
};

// JpegScanlineReader decodes JPEG image. It returns a scanline (a row
// of pixels) each time it is called. The output format is GRAY_8 if
// the input image has JCS_GRAYSCALE format, or RGB_888 otherwise.
class JpegScanlineReader : public ::net_instaweb::ScanlineReaderInterface {
 public:
  explicit JpegScanlineReader(MessageHandler* handler);
  ~JpegScanlineReader() override;
  bool Reset() override;

  // Initialize the reader with the given image stream. Note that
  // image_buffer must remain unchanged until the last call to
  // ReadNextScanline().
  ScanlineStatus InitializeWithStatus(const void* image_buffer,
                                      size_t buffer_length) override;

  // Return the next row of pixels.
  ScanlineStatus ReadNextScanlineWithStatus(void** out_scanline_bytes) override;

  // Return the number of bytes in a row (without padding).
  size_t GetBytesPerScanline() override { return bytes_per_row_; }

  bool HasMoreScanLines() override { return (row_ < height_); }
  PixelFormat GetPixelFormat() override { return pixel_format_; }
  size_t GetImageHeight() override { return height_; }
  size_t GetImageWidth() override { return width_; }
  bool IsProgressive() override { return is_progressive_; }

 private:
  JpegEnv* jpeg_env_;              // State of libjpeg
  unsigned char* row_pointer_[1];  // Pointer for a row buffer
  PixelFormat pixel_format_;
  size_t height_;
  size_t width_;
  size_t row_;
  size_t bytes_per_row_;
  bool was_initialized_;
  bool is_progressive_;
  MessageHandler* message_handler_;

  DISALLOW_COPY_AND_ASSIGN(JpegScanlineReader);
};

}  // namespace image_compression

}  // namespace pagespeed

#endif  // PAGESPEED_LIB_IMAGE_JPEG_READER_H_
