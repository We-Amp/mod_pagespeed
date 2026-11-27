// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.
//
// This file is derived from mod_pagespeed and has been substantially modified.
// Originally licensed under Apache License, Version 2.0.
// Copyright (c) 2010-2017 Google Inc.
// Copyright (c) 2018 The Apache Software Foundation.

// Scanline utility functions for pixel format expansion and stream input.
// Ported from mod_pagespeed's pagespeed/kernel/image/scanline_utils.h

#ifndef PAGESPEED_LIB_IMAGE_SCANLINE_UTILS_H_
#define PAGESPEED_LIB_IMAGE_SCANLINE_UTILS_H_

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

#include "lib/image/image_util.h"

namespace pagespeed {

class MessageHandler;

namespace image_compression {

// Decode-side dimension caps. Scanline readers decode bytes fetched from
// origin servers, which are not trusted: a tiny stream can declare
// dimensions implying a giant decode buffer. A stream whose declared
// dimensions exceed these caps is rejected at Initialize time, before any
// buffer proportional to width * height is allocated.
// Per-side pixel cap.
inline constexpr uint32_t kScanlineMaxDimension = 16384;
// Total pixel cap.
inline constexpr uint64_t kScanlineMaxImagePixels = 100ull << 20;

// Return the number of channels, including color channels and alpha channel,
// for the input pixel format.
//   GRAY_8:    1
//   RGB_888:   3
//   RGBA_8888: 4
//
size_t GetNumChannelsFromPixelFormat(PixelFormat format,
                                     MessageHandler* handler);

// Palette for RGBA_8888.
//
struct PaletteRGBA {
  uint8_t red_;
  uint8_t green_;
  uint8_t blue_;
  uint8_t alpha_;
};

// ScanlineStreamInput stores the data stream that will be used by a scanline
// reader. It also stores the position of the stream that the scanline reader
// should start to read.
//
class ScanlineStreamInput {
 public:
  explicit ScanlineStreamInput(MessageHandler* handler)
      : data_(nullptr), length_(0), offset_(0), message_handler_(handler) {}

  void Reset() {
    data_ = nullptr;
    length_ = 0;
    offset_ = 0;
  }

  void Initialize(const void* image_buffer, size_t buffer_length) {
    data_ = static_cast<const char*>(image_buffer);
    length_ = buffer_length;
    offset_ = 0;
  }

  void Initialize(const std::string& image_string) {
    data_ = static_cast<const char*>(image_string.data());
    length_ = image_string.length();
    offset_ = 0;
  }

  const char* data() { return data_; }
  size_t length() { return length_; }
  size_t offset() { return offset_; }
  void set_offset(size_t val) { offset_ = val; }
  MessageHandler* message_handler() { return message_handler_; }

 private:
  const char* data_;
  size_t length_;
  size_t offset_;
  MessageHandler* message_handler_;
};

// Expand pixel format for a scanline and change its offset in the memory.
// Supported expansions:
//   - GRAY_8    -> RGB_888
//   - RGB_888   -> RGB_888
//   - GRAY_8    -> RGBA_8888
//   - RGB_888   -> RGBA_8888
//   - RGBA_8888 -> RGBA_8888
bool ExpandPixelFormat(size_t num_pixels, PixelFormat src_format,
                       int src_offset, const uint8_t* src_data,
                       PixelFormat dst_format, int dst_offset,
                       uint8_t* dst_data, MessageHandler* handler);

}  // namespace image_compression

}  // namespace pagespeed

#endif  // PAGESPEED_LIB_IMAGE_SCANLINE_UTILS_H_
