// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.
//
// This file is derived from mod_pagespeed and has been substantially modified.
// Originally licensed under Apache License, Version 2.0.
// Copyright (c) 2010-2017 Google Inc.
// Copyright (c) 2018 The Apache Software Foundation.

// Adapters between Scanline and MultipleFrame APIs.
// Ported from mod_pagespeed's
// pagespeed/kernel/image/scanline_interface_frame_adapter.h

#ifndef PAGESPEED_LIB_IMAGE_SCANLINE_INTERFACE_FRAME_ADAPTER_H_
#define PAGESPEED_LIB_IMAGE_SCANLINE_INTERFACE_FRAME_ADAPTER_H_

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

#include "lib/base/basictypes.h"
#include "lib/image/image_frame_interface.h"
#include "lib/image/image_util.h"
#include "lib/image/scanline_interface.h"
#include "lib/image/scanline_status.h"

namespace pagespeed {

class MessageHandler;

namespace image_compression {

using net_instaweb::ScanlineReaderInterface;
using net_instaweb::ScanlineStatus;
using net_instaweb::ScanlineWriterInterface;

////////// MultipleFrame API to Scanline API adapters.

// The class FrameToScanlineReaderAdapter takes ownership of a
// MultipleFrameReader and exposes ScanlineReaderInterface methods.
class FrameToScanlineReaderAdapter : public ScanlineReaderInterface {
 public:
  // Acquires ownership of 'frame_reader'.
  explicit FrameToScanlineReaderAdapter(MultipleFrameReader* frame_reader);
  ~FrameToScanlineReaderAdapter() override {}

  bool Reset() override;
  size_t GetBytesPerScanline() override;
  bool HasMoreScanLines() override;
  bool IsProgressive() override;

  // Will return an error status if the underlying MultipleFrameReader
  // is processing an animated image.
  ScanlineStatus InitializeWithStatus(const void* image_buffer,
                                      size_t buffer_length) override;
  ScanlineStatus ReadNextScanlineWithStatus(void** out_scanline_bytes) override;
  size_t GetImageHeight() override;
  size_t GetImageWidth() override;
  PixelFormat GetPixelFormat() override;

 private:
  std::unique_ptr<MultipleFrameReader> impl_;

  ImageSpec image_spec_;
  FrameSpec frame_spec_;

  DISALLOW_COPY_AND_ASSIGN(FrameToScanlineReaderAdapter);
};

// The class FrameToScanlineWriterAdapter takes ownership of a
// MultipleFrameWriter and exposes ScanlineWriterInterface methods.
class FrameToScanlineWriterAdapter : public ScanlineWriterInterface {
 public:
  // Acquires ownership of 'frame_writer'.
  explicit FrameToScanlineWriterAdapter(MultipleFrameWriter* frame_writer);
  ~FrameToScanlineWriterAdapter() override {}

  ScanlineStatus InitWithStatus(size_t width, size_t height,
                                PixelFormat pixel_format) override;
  ScanlineStatus InitializeWriteWithStatus(const void* config,
                                           std::string* out) override;
  ScanlineStatus WriteNextScanlineWithStatus(
      const void* scanline_bytes) override;
  ScanlineStatus FinalizeWriteWithStatus() override;

 private:
  std::unique_ptr<MultipleFrameWriter> impl_;

  bool init_done_;
  ImageSpec image_spec_;
  FrameSpec frame_spec_;

  DISALLOW_COPY_AND_ASSIGN(FrameToScanlineWriterAdapter);
};

////////// Scanline API to MultipleFrame API adapters.

// The class ScanlineToFrameReaderAdapter takes ownership of a
// ScanlineReaderInterface and exposes MultipleFrameReader methods.
class ScanlineToFrameReaderAdapter : public MultipleFrameReader {
 public:
  // Acquires ownership of 'scanline_reader'.
  ScanlineToFrameReaderAdapter(ScanlineReaderInterface* scanline_reader,
                               MessageHandler* message_handler);
  ScanlineStatus Reset() override;
  ScanlineStatus Initialize() override;
  bool HasMoreFrames() const override;
  bool HasMoreScanlines() const override;
  ScanlineStatus PrepareNextFrame() override;
  ScanlineStatus ReadNextScanline(const void** out_scanline_bytes) override;
  ScanlineStatus GetFrameSpec(FrameSpec* frame_spec) const override;
  ScanlineStatus GetImageSpec(ImageSpec* image_spec) const override;

 private:
  enum : std::uint8_t {
    UNINITIALIZED = 0,
    INITIALIZED,
    FRAME_PREPARED,

    ERROR
  } state_;

  ImageSpec image_spec_;
  FrameSpec frame_spec_;

  std::unique_ptr<ScanlineReaderInterface> impl_;

  DISALLOW_COPY_AND_ASSIGN(ScanlineToFrameReaderAdapter);
};

// The class ScanlineToFrameWriterAdapter takes ownership of a
// ScanlineWriterInterface and exposes MultipleFrameWriter methods.
class ScanlineToFrameWriterAdapter : public MultipleFrameWriter {
 public:
  // Acquires ownership of 'scanline_writer'.
  ScanlineToFrameWriterAdapter(ScanlineWriterInterface* scanline_writer,
                               MessageHandler* handler);

  ScanlineStatus Initialize(const void* config, std::string* out) override;
  ScanlineStatus PrepareImage(const ImageSpec* image_spec) override;
  ScanlineStatus PrepareNextFrame(const FrameSpec* frame_spec) override;
  ScanlineStatus WriteNextScanline(const void* scanline_bytes) override;
  ScanlineStatus FinalizeWrite() override;

 private:
  enum : std::uint8_t {
    UNINITIALIZED = 0,
    INITIALIZED,
    IMAGE_PREPARED,
    FRAME_PREPARED,

    ERROR
  } state_;
  const ImageSpec* image_spec_;
  const FrameSpec* frame_spec_;

  std::unique_ptr<ScanlineWriterInterface> impl_;

  const void* config_;
  std::string* out_;

  DISALLOW_COPY_AND_ASSIGN(ScanlineToFrameWriterAdapter);
};

}  // namespace image_compression

}  // namespace pagespeed

#endif  // PAGESPEED_LIB_IMAGE_SCANLINE_INTERFACE_FRAME_ADAPTER_H_
