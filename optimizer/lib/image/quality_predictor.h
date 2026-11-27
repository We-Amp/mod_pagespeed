// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

// PageSpeed 2.0 - Learned Quality Prediction
//
// Wraps per-format LightGBM models (compiled to C) to predict the
// encoder quality parameter for a target SSIMULACRA2 score.

#ifndef PAGESPEED_LIB_IMAGE_QUALITY_PREDICTOR_H_
#define PAGESPEED_LIB_IMAGE_QUALITY_PREDICTOR_H_

#include <cstdint>

#include "lib/image/quality_features.h"

namespace pagespeed {

// Image format enum matching CapabilityMask format bits.
// Duplicated here to avoid pulling in the full CapabilityMask header.
enum class PredictorFormat : uint8_t {
  kJpeg = 0,
  kWebP = 1,
  kAvif = 2,
};

// Predict the encoder quality parameter for the given format and target score.
//
// Returns the predicted quality (JPEG: 1-100, WebP: 0-100, AVIF: 0-100),
// or -1 if the model is not available or the prediction is invalid.
// Callers should check IsValidPrediction() before using the result.
int PredictQuality(PredictorFormat format, const ImageFeatures& features,
                   float target_ssimulacra2);

// Returns true if the predicted quality is within the valid range for the format.
bool IsValidPrediction(PredictorFormat format, int predicted_quality);

}  // namespace pagespeed

#endif  // PAGESPEED_LIB_IMAGE_QUALITY_PREDICTOR_H_
