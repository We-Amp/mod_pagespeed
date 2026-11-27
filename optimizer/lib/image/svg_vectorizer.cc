// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

// PageSpeed 2.0 - SVG Vectorizer Implementation

#include "lib/image/svg_vectorizer.h"

#include <cstddef>
#include <cstdint>
#include <string>

#include "lib/image/svg_preprocessor.h"
#include "lib/image/svg_sanitizer.h"
#include "lib/image/vtracer_ffi.h"

namespace pagespeed {

namespace detail {

int AdaptiveColorPrecision(int actual_colors, int default_precision) {
  if (actual_colors <= 16) return 4;
  if (actual_colors <= 64) return 5;
  if (actual_colors <= 256) return 6;
  return default_precision;
}

}  // namespace detail

SvgVectorizeResult VectorizeImage(const uint8_t* pixels, uint32_t width,
                                  uint32_t height, int bpp,
                                  const SvgVectorizerConfig& config) {
  SvgVectorizeResult result;

  // Step (a): Input validation.
  if (pixels == nullptr) {
    result.error_message = "Null pixel buffer";
    return result;
  }
  if (width == 0 || height == 0) {
    result.error_message = "Zero dimensions";
    return result;
  }
  if (bpp != 1 && bpp != 3 && bpp != 4) {
    result.error_message = "Invalid bpp (must be 1, 3, or 4)";
    return result;
  }
  // Guard against excessive memory allocation in the preprocessor.
  // 50MB decoded pixel limit / 4 bytes per RGBA pixel.
  constexpr uint64_t kMaxPixels = 50ULL * 1024 * 1024 / 4;
  if (static_cast<uint64_t>(width) * height > kMaxPixels) {
    result.error_message = "Image too large for vectorization";
    return result;
  }

  // Step (b): Preprocess pixels (quantize colors, threshold alpha).
  PreprocessConfig preprocess_config;
  PreprocessResult preprocessed =
      PreprocessPixels(pixels, width, height, bpp, preprocess_config);
  if (preprocessed.pixel_buffer.empty()) {
    result.error_message = "Preprocessing failed";
    return result;
  }
  result.actual_colors = preprocessed.actual_colors;

  // Determine adaptive color precision.
  int color_precision = detail::AdaptiveColorPrecision(
      preprocessed.actual_colors, config.color_precision);

  // Step (c): Call VTracer FFI.
  // PreprocessPixels always outputs RGBA (4 channels).
  const auto* preprocessed_pixels =
      reinterpret_cast<const uint8_t*>(preprocessed.pixel_buffer.data());

  VTracerConfig vtracer_config;
  vtracer_default_config(&vtracer_config);

  // Apply preset-specific geometry parameters.
  switch (config.preset) {
    case 0:  // bw: polygon mode, coarser geometry
      vtracer_config.mode = 1;
      vtracer_config.corner_threshold = 120;
      vtracer_config.segment_length = 6.0;
      vtracer_config.splice_threshold = 90;
      break;
    case 1:  // poster (default): spline mode, balanced
      // vtracer_default_config already sets these values.
      break;
    case 2:  // photo: spline mode, finer geometry
      vtracer_config.corner_threshold = 45;
      vtracer_config.segment_length = 2.0;
      vtracer_config.splice_threshold = 30;
      break;
    default:  // Out-of-range falls back to poster.
      break;
  }

  vtracer_config.color_precision = static_cast<uint32_t>(color_precision);
  vtracer_config.filter_speckle = static_cast<uint32_t>(config.filter_speckle);

  VTracerResult vtracer_result;
  int32_t err =
      vtracer_convert(preprocessed_pixels, preprocessed.width,
                      preprocessed.height, 4, &vtracer_config, &vtracer_result);

  if (err != VTRACER_OK || vtracer_result.svg_data == nullptr) {
    result.error_message = "VTracer conversion failed (error code: ";
    result.error_message += std::to_string(err);
    result.error_message += ")";
    // Step (d): Free even on error (safe per API contract).
    vtracer_free(&vtracer_result);
    return result;
  }

  // Capture raw SVG output before freeing.
  std::string raw_svg(reinterpret_cast<const char*>(vtracer_result.svg_data),
                      vtracer_result.svg_len);

  // Step (d): Free FFI result.
  vtracer_free(&vtracer_result);

  // Step (e): Sanitize the raw SVG.
  SvgSanitizeConfig sanitize_config;
  sanitize_config.source_width = width;
  sanitize_config.source_height = height;
  sanitize_config.coordinate_precision = config.coordinate_precision;
  // Enable crisp edges for low-color images.
  sanitize_config.crisp_edges = (preprocessed.actual_colors <= 16);

  SvgSanitizeResult sanitized = SanitizeSvg(raw_svg, sanitize_config);
  if (!sanitized.success) {
    result.error_message = "SVG sanitization failed: ";
    result.error_message += sanitized.error_message;
    return result;
  }

  result.path_count = sanitized.path_count;

  // Step (f): Path count gate.
  if (static_cast<int>(result.path_count) > config.max_paths) {
    result.error_message = "Path count ";
    result.error_message += std::to_string(result.path_count);
    result.error_message += " exceeds limit ";
    result.error_message += std::to_string(config.max_paths);
    return result;
  }

  // Step (g): Size gate.
  if (sanitized.sanitized_svg.size() > config.max_svg_bytes) {
    result.error_message = "SVG size ";
    result.error_message += std::to_string(sanitized.sanitized_svg.size());
    result.error_message += " exceeds limit ";
    result.error_message += std::to_string(config.max_svg_bytes);
    return result;
  }

  // Step (h): Success.
  result.success = true;
  result.svg_data = std::move(sanitized.sanitized_svg);
  return result;
}

}  // namespace pagespeed
