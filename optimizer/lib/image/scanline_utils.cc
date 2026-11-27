// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.
//
// This file is derived from mod_pagespeed and has been substantially modified.
// Originally licensed under Apache License, Version 2.0.
// Copyright (c) 2010-2017 Google Inc.
// Copyright (c) 2018 The Apache Software Foundation.

// Scanline utility functions for pixel format expansion and stream input.
// Ported from mod_pagespeed's pagespeed/kernel/image/scanline_utils.cc

#include "lib/image/scanline_utils.h"

#include <cstddef>
#include <cstdint>
#include <cstring>

#include "lib/base/message_handler.h"

namespace pagespeed::image_compression {

size_t GetNumChannelsFromPixelFormat(PixelFormat format,
                                     MessageHandler* handler) {
  int num_channels = 0;
  switch (format) {
    case GRAY_8:
      num_channels = 1;
      break;
    case RGB_888:
      num_channels = 3;
      break;
    case RGBA_8888:
      num_channels = 4;
      break;
    default:
      if (handler != nullptr) {
        handler->Error("Invalid pixel format.");
      }
  }
  return num_channels;
}

bool ExpandPixelFormat(size_t num_pixels, PixelFormat src_format,
                       int src_offset, const uint8_t* src_data,
                       PixelFormat dst_format, int dst_offset,
                       uint8_t* dst_data, MessageHandler* handler) {
  const int src_num_channels =
      GetNumChannelsFromPixelFormat(src_format, handler);
  const int dst_num_channels =
      GetNumChannelsFromPixelFormat(dst_format, handler);
  const int rgb_num_channels = GetNumChannelsFromPixelFormat(RGB_888, handler);
  const int opaque_channel = rgb_num_channels;
  src_data += static_cast<ptrdiff_t>(src_offset * src_num_channels);
  dst_data += static_cast<ptrdiff_t>(dst_offset * dst_num_channels);

  bool is_ok = true;
  switch (dst_format) {
    case RGB_888:
      switch (src_format) {
        case GRAY_8:
          for (size_t i = 0; i < num_pixels; ++i) {
            memset(dst_data, *src_data, dst_num_channels);
            ++src_data;
            dst_data += dst_num_channels;
          }
          break;
        case RGB_888:
          memcpy(dst_data, src_data, num_pixels * src_num_channels);
          break;
        default:
          is_ok = false;
          if (handler != nullptr) {
            handler->Error("Unsupported pixel format conversion.");
          }
          break;
      }
      break;

    case RGBA_8888:
      switch (src_format) {
        case GRAY_8:
          for (size_t i = 0; i < num_pixels; ++i) {
            memset(dst_data, *src_data, rgb_num_channels);
            dst_data[opaque_channel] = kAlphaOpaque;
            ++src_data;
            dst_data += dst_num_channels;
          }
          break;
        case RGB_888:
          for (size_t i = 0; i < num_pixels; ++i) {
            memcpy(dst_data, src_data, src_num_channels);
            dst_data[opaque_channel] = kAlphaOpaque;
            src_data += src_num_channels;
            dst_data += dst_num_channels;
          }
          break;
        case RGBA_8888:
          memcpy(dst_data, src_data, num_pixels * src_num_channels);
          break;
        default:
          is_ok = false;
          if (handler != nullptr) {
            handler->Error("Unsupported pixel format conversion.");
          }
          break;
      }
      break;
    default:
      is_ok = false;
      if (handler != nullptr) {
        handler->Error("Unsupported pixel format conversion.");
      }
      break;
  }
  return is_ok;
}

}  // namespace pagespeed::image_compression
