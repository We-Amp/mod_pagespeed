// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

// PageSpeed 2.0 - Content-Aware Image Quality Presets
//
// Analyzes decoded pixel buffers to classify image content (photo,
// screenshot, illustration, noisy) and select optimal per-image
// quality presets.  Built on top of the existing SobelGradient()
// and PhotoMetric() primitives from image_analysis.h.
//
// Also provides SVG candidacy evaluation for the auto-vectorization
// pipeline: scores raster images for potential SVG conversion.

#ifndef PAGESPEED_LIB_IMAGE_CONTENT_ANALYZER_H_
#define PAGESPEED_LIB_IMAGE_CONTENT_ANALYZER_H_

#include <cstddef>
#include <cstdint>
#include <string>

namespace pagespeed {

class MessageHandler;

enum class ContentClass : std::uint8_t {
  kPhoto,         // Natural photograph: default quality
  kScreenshot,    // Screenshot/UI: higher quality for sharp edges
  kIllustration,  // Vector-like art / low color count: higher quality
  kNoisy,         // High sensor noise: compress more aggressively
  kUnknown,       // Could not classify
};

struct QualityPreset {
  ContentClass content_class = ContentClass::kUnknown;
  float jpeg_quality_factor = 1.0f;
  float webp_quality_factor = 1.0f;
  float avif_quality_factor = 1.0f;
  float noise_level = 0.0f;

  bool applies_at_viewport(uint32_t viewport_width) const {
    if (content_class == ContentClass::kScreenshot) {
      return viewport_width >= 768;  // Tablet+
    }
    return true;
  }
};

// Result of content analysis, exposing intermediate metrics for downstream
// consumers (e.g., quality prediction models).
struct ContentAnalysisResult {
  ContentClass content_class = ContentClass::kUnknown;
  float edge_density = 0.0f;        // Sobel gradient fraction, 0.0-1.0
  float unique_color_ratio = 0.0f;  // unique_colors / kColorSampleCount
  float photo_metric = 0.0f;        // Histogram peak width
  float noise_level = 0.0f;         // MAD/40, 0.0-1.0
  QualityPreset quality_preset;     // Derived quality factors
};

// Analyze a decoded pixel buffer and return analysis results including
// intermediate metrics and a quality preset.
//
// |pixels| must point to a buffer of at least width * height * bpp bytes.
// |bpp| is bytes per pixel: 1 (grayscale), 3 (RGB), or 4 (RGBA).
// Returns kUnknown on invalid input.
ContentAnalysisResult AnalyzeContent(const uint8_t* pixels,
                                     size_t buffer_length, uint32_t width,
                                     uint32_t height, int bpp,
                                     MessageHandler* handler);

// ---------------------------------------------------------------------------
// SVG Candidacy Evaluation (auto-vectorization pipeline)
// ---------------------------------------------------------------------------

// Image type hint for SVG candidacy scoring.
enum class ImageType : std::uint8_t { kJpeg, kPng, kGif, kWebP, kUnknown };

// Optional PNG metadata for SVG-origin detection.
struct PngMetadata {
  std::string software;  // tEXt "Software" field
};

// Configuration knobs for SVG candidacy scoring.
struct SvgCandidacyConfig {
  int max_pixels = 65536;          // 256*256 default
  int candidacy_threshold = 50;    // Score threshold for kAttempt
  int min_dimension = 16;          // Minimum width/height
  float max_aspect_ratio = 4.0f;   // width/height upper bound
  float min_aspect_ratio = 0.25f;  // width/height lower bound
};

// Result of SVG candidacy evaluation.
struct SvgCandidacy {
  enum Result : std::uint8_t { kReject, kAttempt };
  Result result = kReject;
  int score = 0;
  std::string reason;

  // Individual signal values (for workbench display).
  size_t quantized_unique_colors = 0;
  float flat_region_ratio = 0.0f;
  float alpha_coverage = 0.0f;
  bool png_metadata_match = false;
};

// Evaluate whether a raster image is a good candidate for SVG
// conversion.  Requires the QualityPreset from a prior
// AnalyzeContent() call (for content class gating).
//
// |pixels| must point to a decoded buffer of at least
// width * height * bpp bytes.
// |bpp| is bytes per pixel: 1 (grayscale), 3 (RGB), or 4 (RGBA).
// |source_format| is the original encoded format (for format bonuses).
// |png_meta| is optional PNG metadata; may be nullptr.
SvgCandidacy EvaluateSvgCandidacy(const SvgCandidacyConfig& config,
                                  const QualityPreset& preset,
                                  const uint8_t* pixels, size_t buffer_length,
                                  uint32_t width, uint32_t height, int bpp,
                                  ImageType source_format,
                                  const PngMetadata* png_meta,
                                  MessageHandler* handler);

}  // namespace pagespeed

#endif  // PAGESPEED_LIB_IMAGE_CONTENT_ANALYZER_H_
