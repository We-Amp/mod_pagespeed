// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

// PageSpeed 2.0 - SVG Rasterizer Implementation

#include "lib/image/svg_rasterizer.h"

#include <climits>
#include <cstdlib>
#include <cstring>

#define NANOSVG_IMPLEMENTATION
#include "nanosvg.h"
#define NANOSVGRAST_IMPLEMENTATION
#include "nanosvgrast.h"

namespace pagespeed {

// Same limit as svg_vectorizer.cc: 50MB decoded / 4 bytes per RGBA pixel.
static constexpr uint64_t kMaxPixels = 50ULL * 1024 * 1024 / 4;

SvgRasterizeResult RasterizeSvg(const std::string& svg_data, uint32_t width,
                                uint32_t height) {
  SvgRasterizeResult result;

  if (svg_data.empty()) {
    result.error = "Empty SVG data";
    return result;
  }
  if (width == 0 || height == 0) {
    result.error = "Zero dimensions";
    return result;
  }

  // Guard against excessive allocation.
  if (static_cast<uint64_t>(width) * height > kMaxPixels) {
    result.error = "Dimensions too large for rasterization";
    return result;
  }

  // Guard against int overflow in nsvgRasterize stride parameter.
  if (width > static_cast<uint32_t>(INT_MAX) / 4) {
    result.error = "Width too large for stride calculation";
    return result;
  }

  // nsvgParse modifies the input string, so make a mutable copy.
  std::string svg_copy = svg_data;

  NSVGimage* image = nsvgParse(svg_copy.data(), "px", 96.0f);
  if (image == nullptr) {
    result.error = "nsvgParse failed";
    return result;
  }

  if (image->width <= 0.0f || image->height <= 0.0f) {
    result.error = "SVG has zero dimensions";
    nsvgDelete(image);
    return result;
  }

  NSVGrasterizer* rasterizer = nsvgCreateRasterizer();
  if (rasterizer == nullptr) {
    result.error = "nsvgCreateRasterizer failed";
    nsvgDelete(image);
    return result;
  }

  // Compute scale to fit requested dimensions.
  float scale_x = static_cast<float>(width) / image->width;
  float scale_y = static_cast<float>(height) / image->height;
  float scale = (scale_x < scale_y) ? scale_x : scale_y;

  // Guard resize() against allocation failure (std::bad_alloc) to prevent
  // leaking nsvg resources on the exception path.
  uint64_t buf_size = static_cast<uint64_t>(width) * height * 4;
  if (buf_size > 50ULL * 1024 * 1024) {
    result.error = "rasterization buffer too large";
    nsvgDeleteRasterizer(rasterizer);
    nsvgDelete(image);
    return result;
  }
  result.pixels.resize(static_cast<size_t>(buf_size));
  nsvgRasterize(rasterizer, image, 0.0f, 0.0f, scale, result.pixels.data(),
                static_cast<int>(width), static_cast<int>(height),
                static_cast<int>(width) * 4);

  nsvgDeleteRasterizer(rasterizer);
  nsvgDelete(image);

  result.success = true;
  result.width = width;
  result.height = height;
  return result;
}

std::vector<uint8_t> ConvertToRGBA(const uint8_t* pixels, uint32_t width,
                                   uint32_t height, int bpp) {
  if (pixels == nullptr || width == 0 || height == 0) {
    return {};
  }

  uint64_t pixel_count = static_cast<uint64_t>(width) * height;

  // Guard against overflow in allocation and loop indexing.
  if (pixel_count > kMaxPixels) {
    return {};
  }

  std::vector<uint8_t> rgba(pixel_count * 4);

  switch (bpp) {
    case 4:
      // Already RGBA, just copy.
      std::memcpy(rgba.data(), pixels, pixel_count * 4);
      break;
    case 3:
      // RGB -> RGBA: add alpha=255.
      for (uint64_t i = 0; i < pixel_count; ++i) {
        rgba[i * 4 + 0] = pixels[i * 3 + 0];
        rgba[i * 4 + 1] = pixels[i * 3 + 1];
        rgba[i * 4 + 2] = pixels[i * 3 + 2];
        rgba[i * 4 + 3] = 255;
      }
      break;
    case 1:
      // Grayscale -> RGBA: replicate gray to RGB, alpha=255.
      for (uint64_t i = 0; i < pixel_count; ++i) {
        rgba[i * 4 + 0] = pixels[i];
        rgba[i * 4 + 1] = pixels[i];
        rgba[i * 4 + 2] = pixels[i];
        rgba[i * 4 + 3] = 255;
      }
      break;
    default:
      return {};
  }

  return rgba;
}

}  // namespace pagespeed
