// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

// PageSpeed 2.0 - SVG Rasterizer (nanosvg wrapper)
//
// Re-rasterizes SVG output to pixel buffers for fidelity verification.
// Thread-safe (no mutable shared state).

#ifndef PAGESPEED_LIB_IMAGE_SVG_RASTERIZER_H_
#define PAGESPEED_LIB_IMAGE_SVG_RASTERIZER_H_

#include <cstdint>
#include <string>
#include <vector>

namespace pagespeed {

struct SvgRasterizeResult {
  bool success = false;
  std::vector<uint8_t> pixels;  // RGBA pixel data
  uint32_t width = 0;
  uint32_t height = 0;
  std::string error;
};

// Rasterize an SVG string to RGBA pixels at the given dimensions.
// The SVG data string is copied internally (nsvgParse modifies its input).
SvgRasterizeResult RasterizeSvg(const std::string& svg_data, uint32_t width,
                                uint32_t height);

// Convert a pixel buffer to RGBA format.
// Handles grayscale (1 bpp), RGB (3 bpp), and RGBA (4 bpp) input.
// Returns an empty vector on invalid bpp.
std::vector<uint8_t> ConvertToRGBA(const uint8_t* pixels, uint32_t width,
                                   uint32_t height, int bpp);

}  // namespace pagespeed

#endif  // PAGESPEED_LIB_IMAGE_SVG_RASTERIZER_H_
