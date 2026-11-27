// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

// PageSpeed 2.0 - SSIMULACRA2 Perceptual Quality Verification
//
// Wraps the SSIMULACRA2 metric from the JPEG XL project to measure
// perceptual similarity between original and encoded images.
// Score range: 90+ imperceptible, 70-90 high quality (web),
//              50-70 acceptable, <50 noticeable artifacts.

#ifndef PAGESPEED_LIB_IMAGE_QUALITY_VERIFIER_H_
#define PAGESPEED_LIB_IMAGE_QUALITY_VERIFIER_H_

#include <cstdint>
#include <optional>

namespace pagespeed {

class MessageHandler;

// Compute SSIMULACRA2 score between original and encoded images.
// Both buffers must be sRGB uint8 pixels (RGB or RGBA).
//
// Parameters:
//   original  - pointer to original pixel data
//   encoded   - pointer to encoded-then-decoded pixel data
//   width     - image width in pixels
//   height    - image height in pixels
//   bpp       - bytes per pixel (3=RGB, 4=RGBA, 1=Gray)
//   handler   - message handler for logging (may be null)
//
// Returns the SSIMULACRA2 score (higher=better), or nullopt when the
// metric could not run (null buffer, zero/unsupported dimensions or bpp,
// a sub-8x8 image, or an internal computation failure). Typical range:
// 90+ imperceptible, 70-90 high quality, <50 noticeable. Scores can go
// negative for very different images -- a negative return value IS a
// verdict, which is why the error channel has to be a separate sentinel
// rather than a reserved score (#1382): the caller must never read a
// metric failure as a catastrophic measurement, or vice versa.
std::optional<float> ComputeSSIMULACRA2(const uint8_t* original,
                                        const uint8_t* encoded, uint32_t width,
                                        uint32_t height, int bpp,
                                        MessageHandler* handler);

}  // namespace pagespeed

#endif  // PAGESPEED_LIB_IMAGE_QUALITY_VERIFIER_H_
