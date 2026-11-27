// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

// PageSpeed 2.0 - Image Dimension Reader
//
// Lightweight utility to read image width/height from header bytes
// without performing a full decode. Supports JPEG, PNG, GIF, and WebP.
// Uses AnalyzeImage() internally.

#ifndef PAGESPEED_LIB_IMAGE_IMAGE_DIMENSIONS_H_
#define PAGESPEED_LIB_IMAGE_IMAGE_DIMENSIONS_H_

#include <cstdint>
#include <span>

namespace pagespeed {

struct ImageDimensions {
  int width = 0;
  int height = 0;
  bool valid = false;
};

// Read dimensions from image header bytes without full decode.
// Supports JPEG (SOF marker), PNG (IHDR chunk), GIF (header),
// WebP (VP8/VP8L/VP8X header).
ImageDimensions ReadImageDimensions(std::span<const uint8_t> data);

}  // namespace pagespeed

#endif  // PAGESPEED_LIB_IMAGE_IMAGE_DIMENSIONS_H_
