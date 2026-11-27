// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

// PageSpeed 2.0 - Learned Quality Prediction Implementation

#include "lib/image/quality_predictor.h"

#include <cmath>
#include <cstdio>

#include "lib/image/generated/avif_predictor.h"
#include "lib/image/generated/jpeg_predictor.h"
#include "lib/image/generated/webp_predictor.h"

namespace pagespeed {

int PredictQuality(PredictorFormat format, const ImageFeatures& features,
                   float target_ssimulacra2) {
  float feature_array[kNumFeatures];
  features.ToFloatArray(feature_array, target_ssimulacra2);

  float raw;
  switch (format) {
    case PredictorFormat::kJpeg:
      raw = predict_jpeg_quality(feature_array);
      break;
    case PredictorFormat::kWebP:
      raw = predict_webp_quality(feature_array);
      break;
    case PredictorFormat::kAvif:
      raw = predict_avif_quality(feature_array);
      break;
    default:
      return -1;
  }

  if (std::isnan(raw) || std::isinf(raw) || raw < 0.0f) {
    fprintf(stderr,
            "[pagespeed] PredictQuality: model returned %s for format %d\n",
            std::isnan(raw)   ? "NaN"
            : std::isinf(raw) ? "Inf"
                              : "negative",
            static_cast<int>(format));
    return -1;
  }

  return static_cast<int>(std::round(raw));
}

bool IsValidPrediction(PredictorFormat format, int predicted_quality) {
  if (predicted_quality < 0) return false;
  switch (format) {
    case PredictorFormat::kJpeg:
      return predicted_quality >= 1 && predicted_quality <= 100;
    case PredictorFormat::kWebP:
    case PredictorFormat::kAvif:
      return predicted_quality >= 0 && predicted_quality <= 100;
    default:
      return false;
  }
}

}  // namespace pagespeed
