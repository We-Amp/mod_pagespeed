// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.
//
// This file is derived from mod_pagespeed and has been substantially modified.
// Originally licensed under Apache License, Version 2.0.
// Copyright (c) 2010-2017 Google Inc.
// Copyright (c) 2018 The Apache Software Foundation.

#include "lib/image/read_image.h"

#include <csetjmp>
#include <cstdlib>
#include <cstring>
#include <memory>

#include "lib/base/message_handler.h"
#include "lib/image/exif_orientation.h"
#include "lib/image/frame_interface_optimizer.h"
#include "lib/image/gif_reader.h"
#include "lib/image/image_frame_interface.h"
#include "lib/image/jpeg_optimizer.h"
#include "lib/image/jpeg_reader.h"
#include "lib/image/png_optimizer.h"
#include "lib/image/scanline_interface.h"
#include "lib/image/scanline_interface_frame_adapter.h"
#include "lib/image/scanline_utils.h"
#include "lib/image/webp_optimizer.h"

namespace pagespeed::image_compression {

using net_instaweb::SCANLINE_STATUS_MEMORY_ERROR;
using net_instaweb::SCANLINE_STATUS_SUCCESS;
using net_instaweb::SCANLINE_STATUS_UNSUPPORTED_FORMAT;
using net_instaweb::SCANLINE_UTIL;
using net_instaweb::ScanlineReaderInterface;
using net_instaweb::ScanlineStatus;
using net_instaweb::ScanlineWriterInterface;

////////// Scanline API

// Forward declaration
MultipleFrameReader* InstantiateImageFrameReader(ImageFormat image_type,
                                                 MessageHandler* handler,
                                                 ScanlineStatus* status);

// Instantiates an uninitialized scanline image reader.
ScanlineReaderInterface* InstantiateScanlineReader(ImageFormat image_type,
                                                   MessageHandler* handler,
                                                   ScanlineStatus* status) {
  ScanlineReaderInterface* reader = nullptr;
  const char* which = nullptr;

  *status = ScanlineStatus(SCANLINE_STATUS_SUCCESS);
  switch (image_type) {
    case IMAGE_PNG:
      // Same orientation baking as JPEG, driven by the PNG eXIf chunk.
      reader = new ExifOrientedScanlineReader(new PngScanlineReaderRaw(handler),
                                              handler);
      which = "ExifOrientedScanlineReader(PngScanlineReaderRaw)";
      break;

    case IMAGE_JPEG:
      // Bake EXIF orientation into the pixels at the decode boundary
      // (issue #1005): every scanline consumer -- pixel decode, format
      // conversion, resize, analysis -- receives upright pixels and display
      // dimensions. Passthrough (no buffering) for orientation 1 / absent.
      reader = new ExifOrientedScanlineReader(new JpegScanlineReader(handler),
                                              handler);
      which = "ExifOrientedScanlineReader(JpegScanlineReader)";
      break;

    case IMAGE_WEBP:
      // Same orientation baking as JPEG, driven by the WebP EXIF chunk.
      reader = new ExifOrientedScanlineReader(new WebpScanlineReader(handler),
                                              handler);
      which = "ExifOrientedScanlineReader(WebpScanlineReader)";
      break;

    case IMAGE_GIF: {
      which = "FrameToScanlineReaderAdapter(GifFrameReader)";
      std::unique_ptr<MultipleFrameReader> mf_reader(
          InstantiateImageFrameReader(image_type, handler, status));
      if (!mf_reader->set_quirks_mode(QUIRKS_CHROME, status)) {
        return nullptr;
      }
      reader = new FrameToScanlineReaderAdapter(
          new MultipleFramePaddingReader(mf_reader.release()));
      break;
    }

    case IMAGE_UNKNOWN:
      break;

      // No default so compiler will complain if any enum is not
      // processed.
  }

  if (which == nullptr) {
    *status = PS_LOGGED_STATUS(
        PS_LOG_DFATAL, handler, SCANLINE_STATUS_UNSUPPORTED_FORMAT,
        SCANLINE_UTIL, "invalid image type for reader: %d", image_type);
  } else if (reader == nullptr) {
    *status =
        PS_LOGGED_STATUS(PS_LOG_ERROR, handler, SCANLINE_STATUS_MEMORY_ERROR,
                         SCANLINE_UTIL, "failed to allocate %s", which);
  }

  return reader;
}

// Returns an initialized scanline image reader.
ScanlineReaderInterface* CreateScanlineReader(ImageFormat image_type,
                                              const void* image_buffer,
                                              size_t buffer_length,
                                              MessageHandler* handler,
                                              ScanlineStatus* status) {
  std::unique_ptr<ScanlineReaderInterface> reader(
      InstantiateScanlineReader(image_type, handler, status));
  if (status->Success()) {
    *status = reader->InitializeWithStatus(image_buffer, buffer_length);
  }
  return status->Success() ? reader.release() : nullptr;
}

// Forward declaration.
MultipleFrameWriter* InstantiateImageFrameWriter(ImageFormat image_type,
                                                 MessageHandler* handler,
                                                 ScanlineStatus* status);

// Instantiates an uninitialized scanline image writer.
ScanlineWriterInterface* InstantiateScanlineWriter(ImageFormat image_type,
                                                   MessageHandler* handler,
                                                   ScanlineStatus* status) {
  ScanlineWriterInterface* writer = nullptr;
  const char* which = nullptr;

  *status = ScanlineStatus(SCANLINE_STATUS_SUCCESS);
  switch (image_type) {
    case IMAGE_JPEG:
      // JpegScanlineWriter methods handle libjpeg errors internally
      // via setjmp when no external jmp_buf is provided.
      writer = new JpegScanlineWriter(handler);
      which = "JpegScanlineWriter";
      break;

    case IMAGE_PNG:
      writer = new PngScanlineWriter(handler);
      which = "PngScanlineWriter";
      break;

    case IMAGE_WEBP:
      which = "FrameToScanlineWriterAdapter(WebpFrameWriter)";
      writer = new FrameToScanlineWriterAdapter(
          InstantiateImageFrameWriter(image_type, handler, status));
      break;

    case IMAGE_GIF:
      // GIF writing is not supported; intentional fall-through.
    case IMAGE_UNKNOWN:
      break;

      // No default so compiler will complain if any enum is not
      // processed.
  }

  if (which == nullptr) {
    *status = PS_LOGGED_STATUS(
        PS_LOG_DFATAL, handler, SCANLINE_STATUS_UNSUPPORTED_FORMAT,
        SCANLINE_UTIL, "invalid image type for writer: %d", image_type);
  } else if (writer == nullptr) {
    *status =
        PS_LOGGED_STATUS(PS_LOG_ERROR, handler, SCANLINE_STATUS_MEMORY_ERROR,
                         SCANLINE_UTIL, "failed to allocate %s", which);
  }

  return writer;
}

// Returns an initialized scanline image writer.
ScanlineWriterInterface* CreateScanlineWriter(
    ImageFormat image_type, PixelFormat pixel_format, size_t width,
    size_t height, const void* config, std::string* image_data,
    MessageHandler* handler, ScanlineStatus* status) {
  std::unique_ptr<ScanlineWriterInterface> writer(
      InstantiateScanlineWriter(image_type, handler, status));
  if (status->Success()) {
    *status = writer->InitWithStatus(width, height, pixel_format);
  }
  if (status->Success()) {
    *status = writer->InitializeWriteWithStatus(config, image_data);
  }
  return status->Success() ? writer.release() : nullptr;
}

////////// ImageFrame API

// Instantiates an uninitialized image frame reader.
MultipleFrameReader* InstantiateImageFrameReader(ImageFormat image_type,
                                                 MessageHandler* handler,
                                                 ScanlineStatus* status) {
  MultipleFrameReader* reader = nullptr;

  *status = ScanlineStatus(SCANLINE_STATUS_SUCCESS);
  if (image_type == IMAGE_GIF) {
    // Native ImageFrame implementation
    reader = new GifFrameReader(handler);
    if (reader == nullptr) {
      *status =
          PS_LOGGED_STATUS(PS_LOG_ERROR, handler, SCANLINE_STATUS_MEMORY_ERROR,
                           SCANLINE_UTIL, "failed to allocate GifFrameReader");
    }
  } else {
    // Image formats for which we do not have an ImageFrame
    // implementation result in a wrapper around the corresponding
    // Scanline object.
    std::unique_ptr<ScanlineReaderInterface> scanline_reader(
        InstantiateScanlineReader(image_type, handler, status));
    if (status->Success()) {
      reader =
          new ScanlineToFrameReaderAdapter(scanline_reader.release(), handler);
      if (reader == nullptr) {
        *status = PS_LOGGED_STATUS(
            PS_LOG_ERROR, handler, SCANLINE_STATUS_MEMORY_ERROR, SCANLINE_UTIL,
            "failed to allocate ScanlineToFrameReaderAdapter");
      }
    }
  }

  return reader;
}

// Returns an initialized image frame reader.
MultipleFrameReader* CreateImageFrameReader(
    ImageFormat image_type, const void* image_buffer, size_t buffer_length,
    QuirksMode quirks_mode, MessageHandler* handler, ScanlineStatus* status) {
  std::unique_ptr<MultipleFrameReader> reader(
      InstantiateImageFrameReader(image_type, handler, status));
  return (status->Success() && reader->set_quirks_mode(quirks_mode, status) &&
          reader->Initialize(image_buffer, buffer_length, status))
             ? reader.release()
             : nullptr;
}

// Instantiates an uninitialized image frame writer.
MultipleFrameWriter* InstantiateImageFrameWriter(ImageFormat image_type,
                                                 MessageHandler* handler,
                                                 ScanlineStatus* status) {
  MultipleFrameWriter* allocated_writer = nullptr;
  *status = ScanlineStatus(SCANLINE_STATUS_SUCCESS);

  if (image_type == IMAGE_WEBP) {
    // Native ImageFrame implementation
    allocated_writer = new WebpFrameWriter(handler);
    if (allocated_writer == nullptr) {
      *status =
          PS_LOGGED_STATUS(PS_LOG_ERROR, handler, SCANLINE_STATUS_MEMORY_ERROR,
                           SCANLINE_UTIL, "failed to allocate WebpFrameWriter");
    }
  } else {
    // Image formats for which we do not have an ImageFrame
    // implementation result in a wrapper around the corresponding
    // Scanline object.
    std::unique_ptr<ScanlineWriterInterface> scanline_writer(
        InstantiateScanlineWriter(image_type, handler, status));
    if (status->Success()) {
      allocated_writer =
          new ScanlineToFrameWriterAdapter(scanline_writer.release(), handler);
      if (allocated_writer == nullptr) {
        *status = PS_LOGGED_STATUS(
            PS_LOG_ERROR, handler, SCANLINE_STATUS_MEMORY_ERROR, SCANLINE_UTIL,
            "failed to allocate ScanlineToFrameWriterAdapter");
      }
    }
  }
  return allocated_writer;
}

// Returns an initialized image frame writer.
MultipleFrameWriter* CreateImageFrameWriter(ImageFormat image_type,
                                            const void* config,
                                            std::string* image_data,
                                            MessageHandler* handler,
                                            ScanlineStatus* status) {
  std::unique_ptr<MultipleFrameWriter> writer(
      InstantiateImageFrameWriter(image_type, handler, status));
  return (status->Success() && writer->Initialize(config, image_data, status))
             ? writer.release()
             : nullptr;
}

////////// Utilities

bool ReadImage(ImageFormat image_type, const void* image_buffer,
               size_t buffer_length, void** pixels, PixelFormat* pixel_format,
               size_t* width, size_t* height, size_t* stride,
               MessageHandler* handler) {
  // Instantiate and initialize the reader based on image type.
  std::unique_ptr<ScanlineReaderInterface> reader;
  reader.reset(
      CreateScanlineReader(image_type, image_buffer, buffer_length, handler));
  if (reader.get() == nullptr) {
    return false;
  }

  // The following information is available after the reader is
  // initialized. Copy them to the outputs if they are requested.
  if (pixel_format != nullptr) {
    *pixel_format = reader->GetPixelFormat();
  }
  if (width != nullptr) {
    *width = reader->GetImageWidth();
  }
  if (height != nullptr) {
    *height = reader->GetImageHeight();
  }

  // Round up stride to a multiplier of 4.
  size_t bytes_per_row4 = (((reader->GetBytesPerScanline() + 3) >> 2) << 2);
  if (stride != nullptr) {
    *stride = bytes_per_row4;
  }

  // Decode the image data (pixels) if it has been requested.
  if (pixels == nullptr) {
    return true;
  }
  *pixels = nullptr;
  const size_t image_height = reader->GetImageHeight();
  if (bytes_per_row4 > 0 && image_height > SIZE_MAX / bytes_per_row4) {
    return false;
  }
  const size_t data_length = image_height * bytes_per_row4;
  auto* image_data = static_cast<unsigned char*>(malloc(data_length));
  if (image_data == nullptr) {
    return false;
  }

  unsigned char* row_data = image_data;
  unsigned char* scanline = nullptr;
  while (reader->HasMoreScanLines()) {
    if (!reader->ReadNextScanline(reinterpret_cast<void**>(&scanline))) {
      free(image_data);
      return false;
    }
    memcpy(row_data, scanline, reader->GetBytesPerScanline());
    row_data += bytes_per_row4;
  }

  *pixels = static_cast<void*>(image_data);
  return true;
}

}  // namespace pagespeed::image_compression
