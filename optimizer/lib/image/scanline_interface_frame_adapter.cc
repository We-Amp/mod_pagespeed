// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.
//
// This file is derived from mod_pagespeed and has been substantially modified.
// Originally licensed under Apache License, Version 2.0.
// Copyright (c) 2010-2017 Google Inc.
// Copyright (c) 2018 The Apache Software Foundation.

#include "lib/image/scanline_interface_frame_adapter.h"

#include <cassert>

#include "lib/base/message_handler.h"

namespace pagespeed::image_compression {

using net_instaweb::FRAME_TO_SCANLINE_READER_ADAPTER;
using net_instaweb::FRAME_TO_SCANLINE_WRITER_ADAPTER;
using net_instaweb::SCANLINE_STATUS_INVOCATION_ERROR;
using net_instaweb::SCANLINE_STATUS_PARSE_ERROR;
using net_instaweb::SCANLINE_STATUS_SUCCESS;
using net_instaweb::SCANLINE_STATUS_UNSUPPORTED_FEATURE;
using net_instaweb::SCANLINE_TO_FRAME_READER_ADAPTER;
using net_instaweb::SCANLINE_TO_FRAME_WRITER_ADAPTER;
using net_instaweb::ScanlineStatus;

////////// FrameToScanlineReaderAdapter

FrameToScanlineReaderAdapter::FrameToScanlineReaderAdapter(
    MultipleFrameReader* frame_reader)
    : impl_(frame_reader) {
  assert(frame_reader != nullptr);
  Reset();
}

bool FrameToScanlineReaderAdapter::Reset() {
  image_spec_.Reset();
  frame_spec_.Reset();
  return impl_->Reset().Success();
}

size_t FrameToScanlineReaderAdapter::GetBytesPerScanline() {
  return GetBytesPerPixel(GetPixelFormat()) * GetImageWidth();
}

bool FrameToScanlineReaderAdapter::HasMoreScanLines() {
  return impl_->HasMoreScanlines();
}

bool FrameToScanlineReaderAdapter::IsProgressive() {
  return frame_spec_.hint_progressive;
}

ScanlineStatus FrameToScanlineReaderAdapter::InitializeWithStatus(
    const void* image_buffer, size_t const buffer_length) {
  ScanlineStatus status;
  if ((impl_->Initialize(image_buffer, buffer_length, &status) &&
       impl_->GetImageSpec(&image_spec_, &status) &&
       impl_->PrepareNextFrame(&status) &&
       impl_->GetFrameSpec(&frame_spec_, &status))) {
    if (image_spec_.num_frames > 1) {
      status = PS_LOGGED_STATUS(
          PS_DLOG_INFO, impl_->message_handler(),
          SCANLINE_STATUS_UNSUPPORTED_FEATURE, FRAME_TO_SCANLINE_READER_ADAPTER,
          "animated images not supported in Scanline interface. %s",
          image_spec_.ToString().c_str());
    } else if ((frame_spec_.width != image_spec_.width) ||
               (frame_spec_.height != image_spec_.height)) {
      status = PS_LOGGED_STATUS(PS_LOG_INFO, impl_->message_handler(),
                                SCANLINE_STATUS_PARSE_ERROR,
                                FRAME_TO_SCANLINE_READER_ADAPTER,
                                "frame must have same dimensions as image");
    }
  }
  return status;
}

ScanlineStatus FrameToScanlineReaderAdapter::ReadNextScanlineWithStatus(
    void** const out_scanline_bytes) {
  return impl_->ReadNextScanline(const_cast<const void**>(out_scanline_bytes));
}

size_t FrameToScanlineReaderAdapter::GetImageHeight() {
  return image_spec_.height;
}

size_t FrameToScanlineReaderAdapter::GetImageWidth() {
  return image_spec_.width;
}

PixelFormat FrameToScanlineReaderAdapter::GetPixelFormat() {
  return frame_spec_.pixel_format;
}

////////// FrameToScanlineWriterAdapter

FrameToScanlineWriterAdapter::FrameToScanlineWriterAdapter(
    MultipleFrameWriter* const frame_writer)
    : impl_(frame_writer), init_done_(false) {
  assert(frame_writer != nullptr);
}

ScanlineStatus FrameToScanlineWriterAdapter::InitWithStatus(
    const size_t width, const size_t height, const PixelFormat pixel_format) {
  image_spec_.width = width;
  image_spec_.height = height;

  // Non-animated images have only one frame.
  image_spec_.num_frames = 1;

  frame_spec_.width = image_spec_.width;
  frame_spec_.height = image_spec_.height;
  frame_spec_.top = 0;
  frame_spec_.left = 0;
  frame_spec_.pixel_format = pixel_format;
  frame_spec_.duration_ms = 0;
  frame_spec_.disposal = FrameSpec::DISPOSAL_NONE;

  init_done_ = true;
  return ScanlineStatus(SCANLINE_STATUS_SUCCESS);
}

ScanlineStatus FrameToScanlineWriterAdapter::InitializeWriteWithStatus(
    const void* const config, std::string* const out) {
  if (!init_done_) {
    return PS_LOGGED_STATUS(PS_LOG_DFATAL, impl_->message_handler(),
                            SCANLINE_STATUS_INVOCATION_ERROR,
                            FRAME_TO_SCANLINE_WRITER_ADAPTER,
                            "Initialize() called before Init()");
  }

  ScanlineStatus status;
  // We don't actually need a separate success variable, but want to
  // prevent future steps as soon as we encounter an error.
  bool success = (impl_->Initialize(config, out, &status) &&
                  impl_->PrepareImage(&image_spec_, &status) &&
                  impl_->PrepareNextFrame(&frame_spec_, &status));
  (void)success;
  return status;
}

ScanlineStatus FrameToScanlineWriterAdapter::WriteNextScanlineWithStatus(
    const void* const scanline_bytes) {
  return impl_->WriteNextScanline(scanline_bytes);
}

ScanlineStatus FrameToScanlineWriterAdapter::FinalizeWriteWithStatus() {
  return impl_->FinalizeWrite();
}

////////// ScanlineToFrameReaderAdapter

ScanlineToFrameReaderAdapter::ScanlineToFrameReaderAdapter(
    ScanlineReaderInterface* const scanline_reader,
    MessageHandler* const message_handler)
    : MultipleFrameReader(message_handler),
      state_(UNINITIALIZED),
      impl_(scanline_reader) {
  assert(scanline_reader != nullptr);
}

ScanlineStatus ScanlineToFrameReaderAdapter::Reset() {
  state_ = UNINITIALIZED;
  frame_spec_.Reset();
  image_spec_.Reset();

  return (impl_->Reset()
              ? ScanlineStatus(SCANLINE_STATUS_SUCCESS)
              : PS_LOGGED_STATUS(PS_LOG_ERROR, message_handler(),
                                 net_instaweb::SCANLINE_STATUS_INTERNAL_ERROR,
                                 SCANLINE_TO_FRAME_READER_ADAPTER,
                                 "Error in ScanlineReaderInterface::Reset()"));
}

ScanlineStatus ScanlineToFrameReaderAdapter::Initialize() {
  ScanlineStatus status =
      impl_->InitializeWithStatus(image_buffer_, buffer_length_);

  if (status.Success()) {
    image_spec_.width = impl_->GetImageWidth();
    image_spec_.height = impl_->GetImageHeight();

    // Non-animated images have only one frame.
    image_spec_.num_frames = 1;
  }

  state_ = status.Success() ? INITIALIZED : ERROR;
  return status;
}

ScanlineStatus ScanlineToFrameReaderAdapter::PrepareNextFrame() {
  if (!HasMoreFrames()) {
    state_ = ERROR;
    return PS_LOGGED_STATUS(
        PS_LOG_DFATAL, message_handler(), SCANLINE_STATUS_INVOCATION_ERROR,
        SCANLINE_TO_FRAME_READER_ADAPTER, "PrepareNextFrame()");
  }

  frame_spec_.Reset();
  frame_spec_.width = impl_->GetImageWidth();
  frame_spec_.height = impl_->GetImageHeight();
  frame_spec_.top = 0;
  frame_spec_.left = 0;
  frame_spec_.pixel_format = impl_->GetPixelFormat();
  frame_spec_.hint_progressive = impl_->IsProgressive();
  state_ = FRAME_PREPARED;

  return ScanlineStatus(SCANLINE_STATUS_SUCCESS);
}

bool ScanlineToFrameReaderAdapter::HasMoreScanlines() const {
  return impl_->HasMoreScanLines();
}

ScanlineStatus ScanlineToFrameReaderAdapter::ReadNextScanline(
    const void** const out_scanline_bytes) {
  return impl_->ReadNextScanlineWithStatus(
      const_cast<void**>(out_scanline_bytes));
}

bool ScanlineToFrameReaderAdapter::HasMoreFrames() const {
  // Note that once PrepareNextFrame is called, state_ will no longer
  // be INITIALIZED.
  return (state_ == INITIALIZED);
}

ScanlineStatus ScanlineToFrameReaderAdapter::GetImageSpec(
    ImageSpec* const spec) const {
  *spec = image_spec_;
  return ScanlineStatus(SCANLINE_STATUS_SUCCESS);
}

ScanlineStatus ScanlineToFrameReaderAdapter::GetFrameSpec(
    FrameSpec* const spec) const {
  *spec = frame_spec_;
  return ScanlineStatus(SCANLINE_STATUS_SUCCESS);
}

////////// ScanlineToFrameWriterAdapter

ScanlineToFrameWriterAdapter::ScanlineToFrameWriterAdapter(
    ScanlineWriterInterface* const scanline_writer,
    MessageHandler* const handler)
    : MultipleFrameWriter(handler),
      state_(UNINITIALIZED),
      impl_(scanline_writer),
      config_(nullptr),
      out_(nullptr) {
  assert(scanline_writer != nullptr);
}

ScanlineStatus ScanlineToFrameWriterAdapter::Initialize(
    const void* const config, std::string* const out) {
  config_ = config;
  out_ = out;
  state_ = INITIALIZED;
  return ScanlineStatus(SCANLINE_STATUS_SUCCESS);
}

ScanlineStatus ScanlineToFrameWriterAdapter::PrepareNextFrame(
    const FrameSpec* const spec) {
  if (state_ != IMAGE_PREPARED) {
    state_ = ERROR;
    return PS_LOGGED_STATUS(PS_LOG_DFATAL, message_handler(),
                            SCANLINE_STATUS_INVOCATION_ERROR,
                            SCANLINE_TO_FRAME_WRITER_ADAPTER,
                            "PrepareNextFrame should be called after "
                            "PrepareImage, and only once");
  }

  frame_spec_ = spec;
  ScanlineStatus status = impl_->InitWithStatus(
      image_spec_->width, image_spec_->height, frame_spec_->pixel_format);
  if (status.Success()) {
    status = impl_->InitializeWriteWithStatus(config_, out_);
  }
  state_ = (status.Success() ? FRAME_PREPARED : ERROR);
  return status;
}

ScanlineStatus ScanlineToFrameWriterAdapter::WriteNextScanline(
    const void* const scanline_bytes) {
  return impl_->WriteNextScanlineWithStatus(scanline_bytes);
}

ScanlineStatus ScanlineToFrameWriterAdapter::FinalizeWrite() {
  return impl_->FinalizeWriteWithStatus();
}

ScanlineStatus ScanlineToFrameWriterAdapter::PrepareImage(
    const ImageSpec* const spec) {
  image_spec_ = spec;
  if (spec->num_frames > 1) {
    state_ = ERROR;
    return PS_LOGGED_STATUS(PS_LOG_INFO, message_handler(),
                            SCANLINE_STATUS_UNSUPPORTED_FEATURE,
                            SCANLINE_TO_FRAME_WRITER_ADAPTER,
                            "animated images not supported in Scanline"
                            "interface");
  }
  state_ = IMAGE_PREPARED;
  return ScanlineStatus(SCANLINE_STATUS_SUCCESS);
}

}  // namespace pagespeed::image_compression
