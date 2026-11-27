// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

// PageSpeed 2.0 - VTracer FFI C Header
//
// C-compatible declarations for the VTracer raster-to-SVG vectorization
// library.  The implementation is a Rust staticlib (vtracer_ffi crate)
// linked as a Bazel cc_library for C++ consumption.
//
// Usage:
//   VTracerConfig config;
//   vtracer_default_config(&config);
//   config.color_precision = 4;
//
//   VTracerResult result;
//   int err = vtracer_convert(pixels, width, height, 4, &config, &result);
//   if (err == VTRACER_OK) {
//     // result.svg_data contains result.svg_len bytes of SVG
//     use_svg(result.svg_data, result.svg_len);
//     vtracer_free(&result);
//   }

#ifndef PAGESPEED_LIB_IMAGE_VTRACER_FFI_H_
#define PAGESPEED_LIB_IMAGE_VTRACER_FFI_H_

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Error codes.
#define VTRACER_OK 0
#define VTRACER_INVALID_INPUT 1
#define VTRACER_TOO_MANY_PATHS 2
#define VTRACER_INTERNAL_ERROR 3

// Conversion modes.
#define VTRACER_MODE_SPLINE 0
#define VTRACER_MODE_POLYGON 1
#define VTRACER_MODE_PIXEL 2

// Configuration for VTracer conversion.
typedef struct VTracerConfig {
  uint32_t color_precision;   // 0-8, number of significant bits per channel
  uint32_t filter_speckle;    // Minimum area of speckle to discard (px)
  uint32_t corner_threshold;  // 0-180 degrees, corner detection angle
  double segment_length;      // Minimum segment length
  uint32_t splice_threshold;  // 0-180 degrees, splice angle threshold
  uint32_t mode;              // 0=spline, 1=polygon, 2=pixel
} VTracerConfig;

// Result from VTracer conversion.  Must be freed via vtracer_free().
typedef struct VTracerResult {
  uint8_t* svg_data;    // SVG output (UTF-8).  NULL on error.
  size_t svg_len;       // Length of svg_data in bytes.
  uint32_t path_count;  // Number of <path> elements in output.
  int32_t error_code;   // 0=success, see VTRACER_* error codes.
} VTracerResult;

// Populate |config| with default values.
// Safe to call with NULL (no-op).
void vtracer_default_config(VTracerConfig* config);

// Convert a raster pixel buffer to SVG.
//
// Parameters:
//   pixels   - Raw pixel data (RGB or RGBA), must not be NULL.
//   width    - Image width in pixels (1..65535).
//   height   - Image height in pixels (1..65535).
//   channels - Bytes per pixel: 3 (RGB) or 4 (RGBA).
//   config   - Conversion config, or NULL for defaults.
//   result   - Output struct, must not be NULL.
//
// Returns 0 on success, non-zero error code on failure.
// On success, result->svg_data must be freed via vtracer_free().
int32_t vtracer_convert(const uint8_t* pixels, uint32_t width, uint32_t height,
                        uint32_t channels, const VTracerConfig* config,
                        VTracerResult* result);

// Free memory allocated by vtracer_convert().
// Safe to call with NULL (no-op).  Must not be called twice on the
// same result.
void vtracer_free(VTracerResult* result);

#ifdef __cplusplus
}  // extern "C"
#endif

#endif  // PAGESPEED_LIB_IMAGE_VTRACER_FFI_H_
