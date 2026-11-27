// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.
//
// This file is derived from mod_pagespeed and has been substantially modified.
// Originally licensed under Apache License, Version 2.0.
// Copyright (c) 2010-2017 Google Inc.
// Copyright (c) 2018 The Apache Software Foundation.

// Abstract interfaces for scanline-based image reading and writing.
// Ported from mod_pagespeed's pagespeed/kernel/image/scanline_interface.h

#ifndef PAGESPEED_LIB_IMAGE_SCANLINE_INTERFACE_H_
#define PAGESPEED_LIB_IMAGE_SCANLINE_INTERFACE_H_

#include <cstddef>
#include <string>

#include "lib/base/basictypes.h"
#include "lib/image/image_util.h"
#include "lib/image/scanline_status.h"

// Forward declaration for ScanlineWriterInterface. This must sit at global
// scope: nesting it inside net_instaweb would declare a distinct
// net_instaweb::pagespeed::image_compression::ScanlineWriterConfig that
// shadows the real type defined in image_util.h.
namespace pagespeed {
namespace image_compression {
struct ScanlineWriterConfig;
}  // namespace image_compression
}  // namespace pagespeed

namespace net_instaweb {

// Abstract interface for reading image data one scanline at a time.
// Implementations exist for JPEG, PNG, GIF, and WebP formats.
class ScanlineReaderInterface {
 public:
  ScanlineReaderInterface() = default;
  virtual ~ScanlineReaderInterface() = default;

  // Reset the ScanlineReaderInterface to its initial state. This
  // will only return false as a result of an unhandled error
  // condition, such as a longjmp due to a libpng error.
  virtual bool Reset() = 0;

  // Returns number of bytes required to store a scanline.
  virtual size_t GetBytesPerScanline() = 0;

  // Returns true if there are more scanlines to read.
  virtual bool HasMoreScanLines() = 0;

  // Initialize the reader with an image buffer. Returns status.
  virtual ScanlineStatus InitializeWithStatus(const void* image_buffer,
                                              size_t buffer_length) = 0;

  // Initialize the reader with an image buffer. Returns true on success.
  bool Initialize(const void* image_buffer, size_t buffer_length) {
    return InitializeWithStatus(image_buffer, buffer_length).Success();
  }

  // Reads the next available scanline. Returns the ScanlineStatus of
  // the conversion.
  virtual ScanlineStatus ReadNextScanlineWithStatus(
      void** out_scanline_bytes) = 0;

  // Reads the next available scanline. Returns false if the
  // scan fails.
  bool ReadNextScanline(void** out_scanline_bytes) {
    return ReadNextScanlineWithStatus(out_scanline_bytes).Success();
  }

  // Returns the height of the image.
  virtual size_t GetImageHeight() = 0;

  // Returns the width of the image.
  virtual size_t GetImageWidth() = 0;

  // Returns the pixel format that needs to be used by writer.
  virtual ::pagespeed::image_compression::PixelFormat GetPixelFormat() = 0;

  // Returns true if the original image was encoded progressively by the origin
  // site, so that it could be progressively rendered while the image contents
  // were being transferred.
  virtual bool IsProgressive() = 0;

 private:
  DISALLOW_COPY_AND_ASSIGN(ScanlineReaderInterface);
};

// Abstract interface for writing image data one scanline at a time.
// Implementations exist for JPEG, PNG, and WebP formats.
class ScanlineWriterInterface {
 public:
  ScanlineWriterInterface() = default;
  virtual ~ScanlineWriterInterface() = default;

  // Initialize the basic parameters for writing the image.
  virtual ScanlineStatus InitWithStatus(
      size_t width, size_t height,
      ::pagespeed::image_compression::PixelFormat pixel_format) = 0;

  // Initialize the basic parameters for writing the image. Returns true on
  // success.
  bool Init(size_t width, size_t height,
            ::pagespeed::image_compression::PixelFormat pixel_format) {
    return InitWithStatus(width, height, pixel_format).Success();
  }

  // Initialize the writer with format-specific configuration.
  // The config parameter is format-specific (e.g., JpegWriterConfig).
  virtual ScanlineStatus InitializeWriteWithStatus(const void* config,
                                                   std::string* out) = 0;

  // Initialize the writer. Returns true on success.
  bool InitializeWrite(const void* config, std::string* out) {
    return InitializeWriteWithStatus(config, out).Success();
  }

  // Writes the current scanline with data provided. Returns the
  // ScanlineStatus of the operation.
  virtual ScanlineStatus WriteNextScanlineWithStatus(
      const void* scanline_bytes) = 0;

  // Writes the current scanline with data provided. Returns false
  // if the write fails.
  bool WriteNextScanline(const void* scanline_bytes) {
    return WriteNextScanlineWithStatus(scanline_bytes).Success();
  }

  // Finalizes write structure once all scanlines are written.
  virtual ScanlineStatus FinalizeWriteWithStatus() = 0;

  // Finalizes write structure. Returns true on success.
  bool FinalizeWrite() { return FinalizeWriteWithStatus().Success(); }

 private:
  DISALLOW_COPY_AND_ASSIGN(ScanlineWriterInterface);
};

}  // namespace net_instaweb

#endif  // PAGESPEED_LIB_IMAGE_SCANLINE_INTERFACE_H_
