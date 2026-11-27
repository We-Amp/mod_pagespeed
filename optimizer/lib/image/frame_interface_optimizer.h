// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.
//
// This file is derived from mod_pagespeed and has been substantially modified.
// Originally licensed under Apache License, Version 2.0.
// Copyright (c) 2010-2017 Google Inc.
// Copyright (c) 2018 The Apache Software Foundation.

// MultipleFramePaddingReader: adapts frame dimensions to image
// dimensions with background color padding.
// Ported from mod_pagespeed's
// pagespeed/kernel/image/frame_interface_optimizer.h

#ifndef PAGESPEED_LIB_IMAGE_FRAME_INTERFACE_OPTIMIZER_H_
#define PAGESPEED_LIB_IMAGE_FRAME_INTERFACE_OPTIMIZER_H_

#include <cstddef>
#include <cstdint>
#include <memory>

#include "lib/base/basictypes.h"
#include "lib/image/image_frame_interface.h"
#include "lib/image/image_util.h"
#include "lib/image/scanline_status.h"

namespace pagespeed {

class MessageHandler;

namespace image_compression {

using net_instaweb::ScanlineStatus;

// This class is an adapter that makes the frame size be the same as
// the image size, padding the frame with background color pixels.
class MultipleFramePaddingReader : public MultipleFrameReader {
 public:
  // Takes ownership of reader.
  explicit MultipleFramePaddingReader(MultipleFrameReader* reader);
  ~MultipleFramePaddingReader() override;

  ScanlineStatus Reset() override;
  ScanlineStatus Initialize() override;
  bool HasMoreFrames() const override;
  bool HasMoreScanlines() const override;
  ScanlineStatus PrepareNextFrame() override;
  ScanlineStatus ReadNextScanline(const void** out_scanline_bytes) override;
  ScanlineStatus GetFrameSpec(FrameSpec* frame_spec) const override;
  ScanlineStatus GetImageSpec(ImageSpec* image_spec) const override;
  MessageHandler* message_handler() const;
  ScanlineStatus set_quirks_mode(QuirksMode quirks_mode) override;
  QuirksMode quirks_mode() const override;

 private:
  std::unique_ptr<MultipleFrameReader> impl_;

  // The ImageSpec as fetched from impl_.
  ImageSpec image_spec_;

  // The FrameSpec returned by impl_ for the current frame.
  FrameSpec impl_frame_spec_;

  // The padded FrameSpec we return for the current frame. Its
  // dimensions are those of the image.
  FrameSpec padded_frame_spec_;

  // Whether the frame is as tall as the image.
  bool frame_is_full_height_;

  // Whether the frame is as wide as the image.
  bool frame_is_full_width_;

  // Whether the frame has exactly the same dimensions as the
  // image. This is simply frame_is_full_width_ &&
  // frame_is_full_height_, and is used to shortcut the expensive
  // operations in ReadNextScanline.
  bool frame_needs_no_padding_;

  // The index of the current scanline being read in the current
  // (padded) frame.
  size_px current_scanline_idx_;

  // The current scanline being read in the current (padded) frame.
  std::unique_ptr<uint8_t[]> current_scanline_;

  // A template scanline consisting of purely the padding background
  // color. We copy this to current_scanline_ and overwrite the
  // appropriate locations with the contents of the non-padded frame.
  std::unique_ptr<uint8_t[]> scanline_template_;

  // The number of bytes per pixel in the current frame.
  size_t bytes_per_pixel_;

  // Pointer to the byte in current_scanline_ that marks the start
  // location of where the non-padded frame will be copied, in rows
  // which contain the frame.
  uint8_t* foreground_scanline_start_byte_;

  DISALLOW_COPY_AND_ASSIGN(MultipleFramePaddingReader);
};

}  // namespace image_compression

}  // namespace pagespeed

#endif  // PAGESPEED_LIB_IMAGE_FRAME_INTERFACE_OPTIMIZER_H_
