// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.
//
// This file is derived from mod_pagespeed and has been substantially modified.
// Originally licensed under Apache License, Version 2.0.
// Copyright (c) 2010-2017 Google Inc.
// Copyright (c) 2018 The Apache Software Foundation.

// Factory functions for creating image readers and writers.
// Ported from mod_pagespeed's pagespeed/kernel/image/read_image.h

#ifndef PAGESPEED_LIB_IMAGE_READ_IMAGE_H_
#define PAGESPEED_LIB_IMAGE_READ_IMAGE_H_

#include <cstddef>
#include <string>

#include "lib/image/image_util.h"
#include "lib/image/scanline_interface.h"
#include "lib/image/scanline_status.h"

namespace pagespeed {

class MessageHandler;

namespace image_compression {

using net_instaweb::ScanlineStatus;

class MultipleFrameReader;
class MultipleFrameWriter;

using net_instaweb::ScanlineReaderInterface;
using net_instaweb::ScanlineWriterInterface;

////////// Scanline API

// Returns a scanline image reader. The following formats are
// supported: IMAGE_PNG, IMAGE_JPEG, IMAGE_WEBP, and IMAGE_GIF.
ScanlineReaderInterface* CreateScanlineReader(ImageFormat image_type,
                                              const void* image_buffer,
                                              size_t buffer_length,
                                              MessageHandler* handler,
                                              ScanlineStatus* status);

inline ScanlineReaderInterface* CreateScanlineReader(ImageFormat image_type,
                                                     const void* image_buffer,
                                                     size_t buffer_length,
                                                     MessageHandler* handler) {
  ScanlineStatus status;
  return CreateScanlineReader(image_type, image_buffer, buffer_length, handler,
                              &status);
}

// Returns a scanline image writer. The following formats are
// supported: IMAGE_PNG, IMAGE_JPEG, and IMAGE_WEBP. This function
// also calls the InitWithStatus() and InitializeWriteWithStatus()
// methods of the writer.
ScanlineWriterInterface* CreateScanlineWriter(
    ImageFormat image_type,    // Type of the image to write
    PixelFormat pixel_format,  // Pixel format, RGB_888 etc
    size_t width,              // Width, in pixels, of the image
    size_t height,             // Height, in pixels, of the image
    const void* config,        // Configuration for the output image
    std::string* image_data,   // Output image
    MessageHandler* handler,   // Message handler
    ScanlineStatus* status);   // Status code

inline ScanlineWriterInterface* CreateScanlineWriter(
    ImageFormat image_type, PixelFormat pixel_format, size_t width,
    size_t height, const void* config, std::string* image_data,
    MessageHandler* handler) {
  ScanlineStatus status;
  return CreateScanlineWriter(image_type, pixel_format, width, height, config,
                              image_data, handler, &status);
}

////////// ImageFrame API

// Returns a MultipleFrameReader after calling its Initialize()
// method. The following formats are supported: IMAGE_PNG,
// IMAGE_JPEG, IMAGE_WEBP, and IMAGE_GIF.
MultipleFrameReader* CreateImageFrameReader(
    ImageFormat image_type, const void* image_buffer, size_t buffer_length,
    QuirksMode quirks_mode, MessageHandler* handler, ScanlineStatus* status);

// This overloaded form defaults quirks_mode to QUIRKS_CHROME.
inline MultipleFrameReader* CreateImageFrameReader(ImageFormat image_type,
                                                   const void* image_buffer,
                                                   size_t buffer_length,
                                                   MessageHandler* handler,
                                                   ScanlineStatus* status) {
  return CreateImageFrameReader(image_type, image_buffer, buffer_length,
                                QUIRKS_CHROME, handler, status);
}

// Returns a new MultipleFrameWriter after calling its Initialize()
// method. The following formats are supported: IMAGE_PNG,
// IMAGE_JPEG, and IMAGE_WEBP.
MultipleFrameWriter* CreateImageFrameWriter(
    ImageFormat image_type,   // Type of the image to write
    const void* config,       // Configuration for the output image
    std::string* image_data,  // Image destination
    MessageHandler* handler,  // Message handler
    ScanlineStatus* status);  // Status code

////////// Utilities

// Decode the image stream and return the image information. Use
// non-null pointers to retrieve the information you need, and use
// null pointers to ignore other information.
//
// If the input "pixels" is set to a null pointer, the function will
// finish quicker because the pixel data will not be decoded. If
// "pixels" is set to a non-null pointer, the function will return a
// buffer containing the pixel data. You are responsible for
// destroying the buffer using free().
//
// Arguments "width" and "height" indicate the number of pixels along
// the horizontal and vertical directions, respectively. Argument
// "stride" indicates the number of bytes between the starting points
// of adjacent rows. Garbage bytes may be padded to the end of rows in
// order to make "stride" a multiplier of 4.
//
// This function uses the scanline API and supports non-animated
// images of the following formats: IMAGE_PNG, IMAGE_JPEG, and
// IMAGE_WEBP.
bool ReadImage(ImageFormat image_type, const void* image_buffer,
               size_t buffer_length, void** pixels, PixelFormat* pixel_format,
               size_t* width, size_t* height, size_t* stride,
               MessageHandler* handler);

}  // namespace image_compression

}  // namespace pagespeed

#endif  // PAGESPEED_LIB_IMAGE_READ_IMAGE_H_
