// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

// PageSpeed 2.0 - SVG Vectorizer (C++ Wrapper around VTracer FFI)
//
// Orchestrates the full raster-to-SVG pipeline: preprocessing,
// VTracer conversion, sanitization, and quality/size gating.
// Thread-safe (no mutable shared state).

#ifndef PAGESPEED_LIB_IMAGE_SVG_VECTORIZER_H_
#define PAGESPEED_LIB_IMAGE_SVG_VECTORIZER_H_

#include <cstddef>
#include <cstdint>
#include <string>

namespace pagespeed {

struct SvgVectorizerConfig {
  int color_precision = 6;            // VTracer color precision (1-8)
  int filter_speckle = 4;             // Minimum region size in pixels
  int max_paths = 500;                // Reject SVGs with more paths
  size_t max_svg_bytes = 100 * 1024;  // Reject SVGs larger than this
  int coordinate_precision = 1;       // Decimal places in SVG paths
  int preset = 1;                     // 0=bw, 1=poster, 2=photo
};

struct SvgVectorizeResult {
  bool success = false;
  std::string svg_data;       // Sanitized SVG output
  std::string error_message;  // On failure
  size_t path_count = 0;      // Number of <path> elements
  int actual_colors = 0;      // Unique colors after preprocessing
};

// Convert a raster pixel buffer to a sanitized SVG.
//
// Pipeline:
//   1. Input validation
//   2. Preprocess pixels (quantize, threshold alpha)
//   3. Call VTracer FFI
//   4. Sanitize SVG output (allowlist validation)
//   5. Path count gate
//   6. Size gate
//
// Parameters:
//   pixels - Raw pixel data, must not be NULL.
//   width  - Image width in pixels (>0).
//   height - Image height in pixels (>0).
//   bpp    - Bytes per pixel: 1 (grayscale), 3 (RGB), or 4 (RGBA).
//   config - Vectorization configuration.
//
// Returns result with success=true on success, or error_message set.
SvgVectorizeResult VectorizeImage(const uint8_t* pixels, uint32_t width,
                                  uint32_t height, int bpp,
                                  const SvgVectorizerConfig& config);

namespace detail {

// Adapt VTracer's color_precision based on the actual number of
// unique colors found after preprocessing.  Fewer colors allow
// coarser quantization, which produces simpler paths.
// Exposed in this namespace for unit testing.
int AdaptiveColorPrecision(int actual_colors, int default_precision);

}  // namespace detail

}  // namespace pagespeed

#endif  // PAGESPEED_LIB_IMAGE_SVG_VECTORIZER_H_
