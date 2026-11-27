// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

// PageSpeed 2.0 - Pixel Preprocessing Pipeline for SVG Vectorization
//
// Prepares raster pixel data for auto-vectorization (VTracer).
// Reduces color count via median-cut quantization, binarizes alpha,
// and optionally applies morphological close to fill anti-aliasing
// gaps in thin features.  All operations are overflow-safe.

#ifndef PAGESPEED_LIB_IMAGE_SVG_PREPROCESSOR_H_
#define PAGESPEED_LIB_IMAGE_SVG_PREPROCESSOR_H_

#include <cstdint>
#include <string>

namespace pagespeed {

struct PreprocessConfig {
  int max_colors = 32;               // Max dominant colors for quantization
  uint8_t alpha_threshold = 128;     // Binary alpha threshold
  bool morphological_close = false;  // Enable morphological close
  int close_radius = 1;              // Morphological close radius
};

struct PreprocessResult {
  std::string pixel_buffer;  // Preprocessed RGBA pixels
  uint32_t width = 0;
  uint32_t height = 0;
  int actual_colors = 0;  // Number of colors after quantization
};

// Quantize image colors to N dominant colors using median-cut
// algorithm.  Input: RGBA pixel buffer.  Output: quantized RGBA
// pixel buffer.  Each pixel is snapped to its nearest dominant
// color.  Alpha channel is preserved unchanged.
void QuantizeColors(uint8_t* pixels, uint32_t width, uint32_t height, int bpp,
                    int max_colors);

// Threshold alpha channel to binary (0 or 255).
// Only operates on RGBA (bpp=4) images.  No-op for RGB/grayscale.
void ThresholdAlpha(uint8_t* pixels, uint32_t width, uint32_t height, int bpp,
                    uint8_t threshold = 128);

// Morphological close (dilate then erode) on the alpha channel.
// Fills anti-aliasing gaps in thin features.
// Only for RGBA (bpp=4) images.  No-op for others.
void MorphologicalClose(uint8_t* pixels, uint32_t width, uint32_t height,
                        int bpp, int radius = 1);

// Run the full preprocessing pipeline:
// 1. Color quantization (median-cut to max_colors)
// 2. Alpha thresholding (binary at threshold)
// 3. Optional morphological close
//
// |pixels| must be RGBA (bpp=4), RGB (bpp=3), or grayscale (bpp=1).
// Returns preprocessed pixel buffer (always RGBA, bpp=4).
PreprocessResult PreprocessPixels(const uint8_t* pixels, uint32_t width,
                                  uint32_t height, int bpp,
                                  const PreprocessConfig& config);

}  // namespace pagespeed

#endif  // PAGESPEED_LIB_IMAGE_SVG_PREPROCESSOR_H_
