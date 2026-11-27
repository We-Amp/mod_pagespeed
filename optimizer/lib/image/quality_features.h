// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

// PageSpeed 2.0 - Image Feature Extraction for Quality Prediction
//
// Extracts numerical features from decoded pixel buffers and content
// analysis results, suitable for use by learned quality prediction models.

#ifndef PAGESPEED_LIB_IMAGE_QUALITY_FEATURES_H_
#define PAGESPEED_LIB_IMAGE_QUALITY_FEATURES_H_

#include <cstdint>

#include "lib/image/content_analyzer.h"

namespace pagespeed {

// Number of features in the float array for ML prediction.
// 18 features: 17 original + source_quality at index 17.
// Existing models (trained on 17) ignore index 17 harmlessly.
static constexpr int kNumFeatures = 18;

// 18 numerical features extracted from a decoded image.
struct ImageFeatures {
  // From ContentAnalysisResult (reused, no recomputation).
  float edge_density = 0.0f;
  float noise_level = 0.0f;
  float unique_color_ratio = 0.0f;
  float photo_metric = 0.0f;

  // Color entropy: Shannon entropy of an 8-bin hue histogram on sampled pixels.
  float color_entropy = 0.0f;

  // Spatial frequency: 8x8 DCT energy split into low-freq and high-freq.
  float spatial_freq_low = 0.0f;
  float spatial_freq_high = 0.0f;

  // Luminance statistics (single-pass Y channel).
  float mean_luminance = 0.0f;
  float luminance_variance = 0.0f;

  // Metadata.
  uint32_t width = 0;
  uint32_t height = 0;
  uint8_t content_class = 4;  // ContentClass enum value (4=kUnknown)
  uint8_t source_format = 0;  // 0=JPEG, 1=PNG, 2=GIF, 3=WebP
  bool has_alpha = false;
  bool is_grayscale = false;

  // Source JPEG quality from DQT tables (-1 if unavailable/non-JPEG).
  // Included in ToFloatArray at index 17 (NaN when unavailable).
  int source_quality = -1;

  // Feature index mapping for cross-reference with Python training:
  // [0]=edge_density, [1]=noise_level, [2]=unique_color_ratio,
  // [3]=photo_metric, [4]=color_entropy, [5]=spatial_freq_low,
  // [6]=spatial_freq_high, [7]=mean_luminance, [8]=luminance_variance,
  // [9]=width, [10]=height, [11]=content_class, [12]=source_format,
  // [13]=has_alpha, [14]=is_grayscale, [15]=target_ssimulacra2,
  // [16]=pixel_count (width*height, derived),
  // [17]=source_quality (NaN if unavailable)
  void ToFloatArray(float out[kNumFeatures], float target_ssimulacra2) const;
};

// Extract features from a decoded pixel buffer and analysis result.
//
// |analysis| is the result from a prior AnalyzeContent() call.
// |pixels| must point to a buffer of at least width * height * bpp bytes.
// |bpp| is bytes per pixel: 1 (grayscale), 3 (RGB), or 4 (RGBA).
// |source_format| encodes the original format: 0=JPEG, 1=PNG, 2=GIF, 3=WebP.
ImageFeatures ExtractImageFeatures(const ContentAnalysisResult& analysis,
                                   const uint8_t* pixels, uint32_t width,
                                   uint32_t height, int bpp,
                                   uint8_t source_format,
                                   int source_quality = -1);

}  // namespace pagespeed

#endif  // PAGESPEED_LIB_IMAGE_QUALITY_FEATURES_H_
