// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.
//
// This file is derived from mod_pagespeed and has been substantially modified.
// Originally licensed under Apache License, Version 2.0.
// Copyright (c) 2010-2017 Google Inc.
// Copyright (c) 2018 The Apache Software Foundation.

// Implementation of the MultipleFrame API for reading
// and writing static and animated images.
// Ported from mod_pagespeed's pagespeed/kernel/image/image_frame_interface.cc

#include "lib/image/image_frame_interface.h"

#include <cassert>
#include <cstddef>
#include <cstring>
#include <string>

#include "absl/strings/str_format.h"
#include "lib/image/image_util.h"

namespace pagespeed::image_compression {

////////// ImageSpec

ImageSpec::ImageSpec() { Reset(); }

void ImageSpec::Reset() {
  width = 0;
  height = 0;
  num_frames = 0;
  loop_count = 1;
  memset(bg_color, 0, sizeof(bg_color));
  use_bg_color = true;
  image_size_adjusted = false;
}

size_px ImageSpec::TruncateXIndex(const size_px x) const {
  if (x > width) {
    return width;
  }
  return x;
}

size_px ImageSpec::TruncateYIndex(const size_px y) const {
  if (y > height) {
    return height;
  }
  return y;
}

bool ImageSpec::CanContainFrame(const FrameSpec& frame_spec) const {
  return ((frame_spec.left + frame_spec.width <= width) &&
          (frame_spec.top + frame_spec.height <= height));
}

std::string ImageSpec::ToString() const {
  return absl::StrFormat(
      "Image: %d x %d : %u frames, repeated %u times; "
      "bg_color: %s, RGBA: 0x%08X",
      width, height, num_frames, loop_count, use_bg_color ? "ON" : "OFF",
      RgbaToPackedArgb(bg_color));
}

bool ImageSpec::Equals(const ImageSpec& other) const {
  return ((width == other.width) && (height == other.height) &&
          (num_frames == other.num_frames) &&
          (loop_count == other.loop_count) &&
          (memcmp(bg_color, other.bg_color, sizeof(bg_color)) == 0) &&
          (use_bg_color == other.use_bg_color) &&
          (image_size_adjusted == other.image_size_adjusted));
}

////////// FrameSpec

FrameSpec::FrameSpec() { Reset(); }

void FrameSpec::Reset() {
  width = 0;
  height = 0;
  top = 0;
  left = 0;

  pixel_format = UNSUPPORTED;
  duration_ms = 0;
  disposal = DISPOSAL_NONE;
  hint_progressive = false;
}

std::string FrameSpec::ToString() const {
  return absl::StrFormat(
      "Frame: size %u x %u at (%u, %u) "
      "pixel_format: %s, duration_ms: %lu, disposal: %d, "
      "progressive: %s",
      width, height, top, left, GetPixelFormatString(pixel_format),
      static_cast<unsigned long>(duration_ms), disposal,
      (hint_progressive ? "yes" : "no"));
}

bool FrameSpec::Equals(const FrameSpec& other) const {
  return ((width == other.width) && (height == other.height) &&
          (top == other.top) && (left == other.left) &&
          (pixel_format == other.pixel_format) &&
          (duration_ms == other.duration_ms) && (disposal == other.disposal) &&
          (hint_progressive == other.hint_progressive));
}

////////// MultipleFrameReader

MultipleFrameReader::MultipleFrameReader(MessageHandler* const handler)
    : image_buffer_(nullptr),
      buffer_length_(0),
      message_handler_(handler),
      quirks_mode_(QUIRKS_NONE) {
  assert(handler != nullptr);
}

MultipleFrameReader::~MultipleFrameReader() = default;

////////// MultipleFrameWriter

MultipleFrameWriter::MultipleFrameWriter(MessageHandler* const handler)
    : message_handler_(handler) {
  assert(handler != nullptr);
}

MultipleFrameWriter::~MultipleFrameWriter() = default;

}  // namespace pagespeed::image_compression
