// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

// PageSpeed 2.0 - SVG Sanitizer (Allowlist-Based)
//
// Validates and sanitizes SVG output from VTracer auto-vectorization.
// Uses an allowlist approach: only known-safe elements and attributes
// pass through.  Everything else is stripped.  This is security-critical
// code -- XSS vectors, XXE attacks, and script injection must all be
// blocked.

#ifndef PAGESPEED_LIB_IMAGE_SVG_SANITIZER_H_
#define PAGESPEED_LIB_IMAGE_SVG_SANITIZER_H_

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace pagespeed {

struct SvgSanitizeConfig {
  uint32_t source_width = 0;       // Original raster width (for svg dims)
  uint32_t source_height = 0;      // Original raster height
  bool crisp_edges = false;        // Add shape-rendering="crispEdges"
  int coordinate_precision = 1;    // Decimal places for path rounding
  uint32_t max_depth = 256;        // Max element nesting depth (SVG bomb guard)
  uint32_t max_elements = 100000;  // Max total elements (SVG bomb guard)
};

struct SvgSanitizeResult {
  bool success = false;
  std::string sanitized_svg;
  std::string error_message;
  size_t path_count = 0;         // Number of <path> elements found
  size_t elements_stripped = 0;  // Disallowed elements removed
};

// Sanitize SVG via allowlist-based structural validation.
// Strips all disallowed elements, attributes, and declarations.
// Returns sanitized SVG or error.
SvgSanitizeResult SanitizeSvg(std::string_view svg_input,
                              const SvgSanitizeConfig& config);

}  // namespace pagespeed

#endif  // PAGESPEED_LIB_IMAGE_SVG_SANITIZER_H_
