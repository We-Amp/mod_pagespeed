// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.
//
// This file is derived from mod_pagespeed and has been substantially modified.
// Originally licensed under Apache License, Version 2.0.
// Copyright (c) 2010-2017 Google Inc.
// Copyright (c) 2018 The Apache Software Foundation.

// JPEG quality estimation/detection utilities.
// Ported from mod_pagespeed's pagespeed/kernel/image/jpeg_utils.h

#ifndef PAGESPEED_LIB_IMAGE_JPEG_UTILS_H_
#define PAGESPEED_LIB_IMAGE_JPEG_UTILS_H_

#include <cstddef>

#include "lib/base/basictypes.h"

namespace pagespeed {
class MessageHandler;
}

namespace pagespeed {

namespace image_compression {

// Utility class that reads jpeg parameter from jpeg images.
class JpegUtils {
 public:
  // Get image quality with which the input jpeg image is compressed.
  // This method will return -1 if it is not able to either jpeg
  // image is invalid or image quality can't be determined.
  //
  // See comments in implementation for additional details on how
  // quality is computed.
  static int GetImageQualityFromImage(const void* image_data,
                                      size_t image_length,
                                      MessageHandler* handler);

 private:
  JpegUtils();
  DISALLOW_COPY_AND_ASSIGN(JpegUtils);
};

}  // namespace image_compression

}  // namespace pagespeed

#endif  // PAGESPEED_LIB_IMAGE_JPEG_UTILS_H_
