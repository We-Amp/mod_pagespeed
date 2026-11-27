// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

// PageSpeed 2.0 - Content-Aware Image Quality Presets Implementation

#include "lib/image/content_analyzer.h"

#include <algorithm>
#include <climits>
#include <cmath>
#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <set>
#include <string>
#include <string_view>
#include <vector>

#include "lib/base/message_handler.h"
#include "lib/base/string_util.h"
#include "lib/image/image_analysis.h"
#include "lib/image/image_util.h"

namespace pagespeed {

namespace {

// Minimum image dimension for meaningful analysis.
constexpr uint32_t kMinDimension = 3;

// Number of pixel samples for unique color counting.
constexpr size_t kColorSampleCount = 4096;

// Thresholds for illustration detection by unique color count.
constexpr size_t kStrongIllustrationColors = 64;
constexpr size_t kWeakIllustrationColors = 256;

// PhotoMetric threshold (matching image_analysis.cc kHistogramThreshold).
constexpr float kHistogramThreshold = 0.01f;

// Edge density thresholds for screenshot detection.
// High edge density with low PhotoMetric indicates screenshots/UI.
constexpr float kScreenshotEdgeDensityMin = 0.15f;
constexpr float kScreenshotPhotoMetricMax = 12.0f;

// PhotoMetric threshold for photo classification.
constexpr float kPhotoMetricMin = 16.0f;

// Noise estimation: MAD threshold above which a photo is noisy.
constexpr float kNoiseMADThreshold = 15.0f;

// Maximum noise_level value.
constexpr float kMaxNoiseLevel = 1.0f;

// Noise MAD value for max noise_level.
constexpr float kNoiseMADSaturation = 40.0f;

// Compute the fraction of gradient pixels above a threshold.
float ComputeEdgeDensity(const uint8_t* gradient, uint32_t width,
                         uint32_t height) {
  // Use the inner region (skip 1-pixel border zeroed by Sobel).
  if (width < 3 || height < 3) return 0.0f;
  uint32_t inner_w = width - 2;
  uint32_t inner_h = height - 2;
  uint64_t count = 0;
  uint64_t total = static_cast<uint64_t>(inner_w) * inner_h;
  for (uint32_t y = 1; y <= inner_h; ++y) {
    for (uint32_t x = 1; x <= inner_w; ++x) {
      if (gradient[y * width + x] > 30) {
        ++count;
      }
    }
  }
  return total > 0 ? static_cast<float>(count) / total : 0.0f;
}

// Count unique RGB colors by sampling ~kColorSampleCount pixels using
// a pseudo-random pattern (LCG) to avoid aliasing with regular image
// structures like grids or tile boundaries.
// For grayscale, packs as (v, v, v).  Alpha channel is ignored.
size_t CountUniqueColorsSampled(const uint8_t* pixels, uint32_t width,
                                uint32_t height, int bpp) {
  uint64_t total_pixels = static_cast<uint64_t>(width) * height;
  size_t num_samples =
      total_pixels < kColorSampleCount ? total_pixels : kColorSampleCount;

  // LCG pseudo-random sampling to avoid aliasing with regular patterns.
  uint32_t prng = 0x12345678;
  std::set<uint32_t> colors;
  for (size_t s = 0; s < num_samples; ++s) {
    prng = prng * 1103515245 + 12345;
    uint64_t i = (static_cast<uint64_t>(prng >> 8) * total_pixels) >> 24;
    if (i >= total_pixels) i = total_pixels - 1;
    const uint8_t* p = pixels + i * bpp;
    uint32_t rgb;
    if (bpp == 1) {
      rgb = (static_cast<uint32_t>(p[0]) << 16) |
            (static_cast<uint32_t>(p[0]) << 8) | p[0];
    } else {
      rgb = (static_cast<uint32_t>(p[0]) << 16) |
            (static_cast<uint32_t>(p[1]) << 8) | p[2];
    }
    colors.insert(rgb);
    // Early exit: once we have enough colors, it's not an illustration.
    if (colors.size() > kWeakIllustrationColors) break;
  }
  return colors.size();
}

// Estimate noise via median absolute deviation (MAD) of 8x8 block
// variances.  Returns the MAD value.
float EstimateNoiseMAD(const uint8_t* pixels, uint32_t width, uint32_t height,
                       int bpp) {
  // Work on luminance.  For RGB/RGBA, average R+G+B.
  // For grayscale, use the pixel value directly.
  constexpr uint32_t kBlockSize = 8;
  uint32_t blocks_x = width / kBlockSize;
  uint32_t blocks_y = height / kBlockSize;

  if (blocks_x == 0 || blocks_y == 0) return 0.0f;

  std::vector<float> variances;
  variances.reserve(static_cast<size_t>(blocks_x) * blocks_y);

  for (uint32_t by = 0; by < blocks_y; ++by) {
    for (uint32_t bx = 0; bx < blocks_x; ++bx) {
      float sum = 0.0f;
      float sum_sq = 0.0f;
      for (uint32_t dy = 0; dy < kBlockSize; ++dy) {
        for (uint32_t dx = 0; dx < kBlockSize; ++dx) {
          uint32_t px = bx * kBlockSize + dx;
          uint32_t py = by * kBlockSize + dy;
          const uint8_t* p =
              pixels + static_cast<size_t>((py * width + px) * bpp);
          float lum;
          if (bpp == 1) {
            lum = static_cast<float>(p[0]);
          } else {
            lum = (static_cast<float>(p[0]) + p[1] + p[2]) / 3.0f;
          }
          sum += lum;
          sum_sq += lum * lum;
        }
      }
      constexpr float n = kBlockSize * kBlockSize;
      float mean = sum / n;
      float var = (sum_sq / n) - (mean * mean);
      variances.push_back(var);
    }
  }

  if (variances.empty()) return 0.0f;

  // Compute median.
  std::sort(variances.begin(), variances.end());
  float median = variances[variances.size() / 2];

  // Compute MAD.
  std::vector<float> abs_devs;
  abs_devs.reserve(variances.size());
  for (float v : variances) {
    abs_devs.push_back(std::fabs(v - median));
  }
  std::sort(abs_devs.begin(), abs_devs.end());
  return abs_devs[abs_devs.size() / 2];
}

// Convert PixelFormat from bpp.
image_compression::PixelFormat BppToPixelFormat(int bpp) {
  switch (bpp) {
    case 1:
      return image_compression::GRAY_8;
    case 3:
      return image_compression::RGB_888;
    case 4:
      return image_compression::RGBA_8888;
    default:
      return image_compression::UNSUPPORTED;
  }
}

// -----------------------------------------------------------------------
// SVG candidacy helpers
// -----------------------------------------------------------------------

// Count unique RGB colors after quantizing each channel to 5 bits
// (>> 3).  Uses the same LCG PRNG sampling as CountUniqueColorsSampled.
size_t CountQuantizedUniqueColors(const uint8_t* pixels, uint32_t width,
                                  uint32_t height, int bpp) {
  uint64_t total_pixels = static_cast<uint64_t>(width) * height;
  size_t num_samples =
      total_pixels < kColorSampleCount ? total_pixels : kColorSampleCount;

  uint32_t prng = 0x12345678;
  std::set<uint32_t> colors;
  for (size_t s = 0; s < num_samples; ++s) {
    prng = prng * 1103515245 + 12345;
    uint64_t i = (static_cast<uint64_t>(prng >> 8) * total_pixels) >> 24;
    if (i >= total_pixels) i = total_pixels - 1;
    const uint8_t* p = pixels + i * bpp;
    uint8_t r5, g5, b5;
    if (bpp == 1) {
      r5 = g5 = b5 = p[0] >> 3;
    } else {
      r5 = p[0] >> 3;
      g5 = p[1] >> 3;
      b5 = p[2] >> 3;
    }
    uint32_t key = (static_cast<uint32_t>(r5) << 10) |
                   (static_cast<uint32_t>(g5) << 5) | b5;
    colors.insert(key);
  }
  return colors.size();
}

// Single-pass flat region ratio: for each pixel, quantize to 5-bit
// per channel and compare with the right and bottom neighbors (also
// quantized).  Returns fraction of matching comparisons.
float ComputeFlatRegionRatio(const uint8_t* pixels, uint32_t width,
                             uint32_t height, int bpp) {
  if (width < 2 || height < 2) return 0.0f;

  uint64_t matches = 0;
  uint64_t comparisons = 0;

  for (uint32_t y = 0; y < height; ++y) {
    for (uint32_t x = 0; x < width; ++x) {
      const uint8_t* p = pixels + (static_cast<uint64_t>(y) * width + x) * bpp;
      uint8_t r5, g5, b5;
      if (bpp == 1) {
        r5 = g5 = b5 = p[0] >> 3;
      } else {
        r5 = p[0] >> 3;
        g5 = p[1] >> 3;
        b5 = p[2] >> 3;
      }

      // Compare with right neighbor.
      if (x + 1 < width) {
        const uint8_t* pr =
            pixels + (static_cast<uint64_t>(y) * width + x + 1) * bpp;
        uint8_t rr, gr, br;
        if (bpp == 1) {
          rr = gr = br = pr[0] >> 3;
        } else {
          rr = pr[0] >> 3;
          gr = pr[1] >> 3;
          br = pr[2] >> 3;
        }
        if (r5 == rr && g5 == gr && b5 == br) {
          ++matches;
        }
        ++comparisons;
      }

      // Compare with bottom neighbor.
      if (y + 1 < height) {
        const uint8_t* pb =
            pixels + (static_cast<uint64_t>(y + 1) * width + x) * bpp;
        uint8_t rb, gb, bb;
        if (bpp == 1) {
          rb = gb = bb = pb[0] >> 3;
        } else {
          rb = pb[0] >> 3;
          gb = pb[1] >> 3;
          bb = pb[2] >> 3;
        }
        if (r5 == rb && g5 == gb && b5 == bb) {
          ++matches;
        }
        ++comparisons;
      }
    }
  }
  return comparisons > 0 ? static_cast<float>(matches) / comparisons : 0.0f;
}

// For RGBA images (bpp==4): fraction of pixels with alpha < 255.
// Returns 0.0 for non-RGBA images.
float ComputeAlphaCoverage(const uint8_t* pixels, uint32_t width,
                           uint32_t height, int bpp) {
  if (bpp != 4) return 0.0f;
  uint64_t total = static_cast<uint64_t>(width) * height;
  if (total == 0) return 0.0f;
  uint64_t transparent = 0;
  for (uint64_t i = 0; i < total; ++i) {
    if (pixels[i * 4 + 3] < 255) {
      ++transparent;
    }
  }
  return static_cast<float>(transparent) / total;
}

// Case-insensitive substring search.
bool ContainsCaseInsensitive(std::string_view haystack,
                             std::string_view needle) {
  if (needle.empty()) return true;
  if (haystack.size() < needle.size()) return false;
  for (size_t i = 0; i <= haystack.size() - needle.size(); ++i) {
    bool match = true;
    for (size_t j = 0; j < needle.size(); ++j) {
      if (net_instaweb::LowerChar(haystack[i + j]) !=
          net_instaweb::LowerChar(needle[j])) {
        match = false;
        break;
      }
    }
    if (match) return true;
  }
  return false;
}

// Check PngMetadata::software for known SVG-origin tools.
bool CheckPngMetadataFingerprint(const PngMetadata* meta) {
  if (meta == nullptr) return false;
  static constexpr std::string_view kSvgTools[] = {
      "Inkscape",          "Figma", "Adobe Illustrator", "Sketch",
      "Affinity Designer", "Canva",
  };
  for (auto tool : kSvgTools) {
    if (ContainsCaseInsensitive(meta->software, tool)) {
      return true;
    }
  }
  return false;
}

}  // namespace

ContentAnalysisResult AnalyzeContent(const uint8_t* pixels,
                                     size_t buffer_length, uint32_t width,
                                     uint32_t height, int bpp,
                                     MessageHandler* handler) {
  ContentAnalysisResult result;

  // Bounds validation (overflow-safe).
  if (pixels == nullptr || width < kMinDimension || height < kMinDimension ||
      (bpp != 1 && bpp != 3 && bpp != 4)) {
    return result;  // kUnknown
  }

  uint64_t required = static_cast<uint64_t>(width) * height * bpp;
  if (buffer_length < required) {
    return result;  // kUnknown
  }

  auto pixel_format = BppToPixelFormat(bpp);
  if (pixel_format == image_compression::UNSUPPORTED) {
    return result;  // kUnknown
  }

  // Stage 1: Unique color sampling.
  size_t unique_colors = CountUniqueColorsSampled(pixels, width, height, bpp);
  result.unique_color_ratio =
      std::min(static_cast<float>(unique_colors) / kColorSampleCount, 1.0f);

  if (unique_colors <= kStrongIllustrationColors) {
    result.content_class = ContentClass::kIllustration;
    result.quality_preset.content_class = ContentClass::kIllustration;
    result.quality_preset.jpeg_quality_factor = 1.10f;
    result.quality_preset.webp_quality_factor = 1.10f;
    result.quality_preset.avif_quality_factor = 1.10f;
    result.quality_preset.noise_level = 0.0f;
    return result;
  }

  bool candidate_illustration = (unique_colors <= kWeakIllustrationColors);

  // Stage 2: Edge density via SobelGradient + PhotoMetric.
  // Use size_t to prevent overflow when width * bpp exceeds INT_MAX.
  size_t bytes_per_line = static_cast<size_t>(width) * bpp;
  auto gradient_buf =
      static_cast<uint8_t*>(malloc(static_cast<size_t>(width) * height));
  if (gradient_buf == nullptr) {
    return result;  // kUnknown
  }

  if (bytes_per_line > static_cast<size_t>(INT_MAX)) {
    free(gradient_buf);
    return result;  // kUnknown — bytes_per_line too large for int stride.
  }
  int bpl_int = static_cast<int>(bytes_per_line);
  bool gradient_ok = image_compression::SobelGradient(
      pixels, width, height, bpl_int, pixel_format, handler, gradient_buf);

  if (gradient_ok) {
    result.edge_density = ComputeEdgeDensity(gradient_buf, width, height);
  }
  free(gradient_buf);

  result.photo_metric = image_compression::PhotoMetric(
      pixels, width, height, bpl_int, pixel_format, kHistogramThreshold,
      handler);

  // Screenshot: high edge density + low photo metric.
  if (result.edge_density >= kScreenshotEdgeDensityMin &&
      result.photo_metric < kScreenshotPhotoMetricMax) {
    result.content_class = ContentClass::kScreenshot;
    result.quality_preset.content_class = ContentClass::kScreenshot;
    result.quality_preset.jpeg_quality_factor = 1.15f;
    result.quality_preset.webp_quality_factor = 1.15f;
    result.quality_preset.avif_quality_factor = 1.15f;
    result.quality_preset.noise_level = 0.0f;
    return result;
  }

  // Illustration (weak candidate): low color count + low photo metric.
  if (candidate_illustration && result.photo_metric < kPhotoMetricMin) {
    result.content_class = ContentClass::kIllustration;
    result.quality_preset.content_class = ContentClass::kIllustration;
    result.quality_preset.jpeg_quality_factor = 1.10f;
    result.quality_preset.webp_quality_factor = 1.10f;
    result.quality_preset.avif_quality_factor = 1.10f;
    result.quality_preset.noise_level = 0.0f;
    return result;
  }

  // Stage 3: Noise estimation.
  float noise_mad = EstimateNoiseMAD(pixels, width, height, bpp);
  result.noise_level =
      std::min(noise_mad / kNoiseMADSaturation, kMaxNoiseLevel);

  if (result.photo_metric >= kPhotoMetricMin &&
      noise_mad > kNoiseMADThreshold) {
    result.content_class = ContentClass::kNoisy;
    result.quality_preset.content_class = ContentClass::kNoisy;
    result.quality_preset.jpeg_quality_factor = 0.85f;
    result.quality_preset.webp_quality_factor = 0.85f;
    result.quality_preset.avif_quality_factor = 0.85f;
    result.quality_preset.noise_level = result.noise_level;
    return result;
  }

  // Photo: high photo metric.
  if (result.photo_metric >= kPhotoMetricMin) {
    result.content_class = ContentClass::kPhoto;
    result.quality_preset.content_class = ContentClass::kPhoto;
    result.quality_preset.jpeg_quality_factor = 1.0f;
    result.quality_preset.webp_quality_factor = 1.0f;
    result.quality_preset.avif_quality_factor = 1.0f;
    result.quality_preset.noise_level = result.noise_level;
    return result;
  }

  // Fallback: kUnknown (default preset).
  return result;
}

SvgCandidacy EvaluateSvgCandidacy(const SvgCandidacyConfig& config,
                                  const QualityPreset& preset,
                                  const uint8_t* pixels, size_t buffer_length,
                                  uint32_t width, uint32_t height, int bpp,
                                  ImageType source_format,
                                  const PngMetadata* png_meta,
                                  MessageHandler* handler) {
  SvgCandidacy result;

  // --- Hard reject: content class ---
  if (preset.content_class == ContentClass::kPhoto ||
      preset.content_class == ContentClass::kNoisy) {
    result.reason = "photo/noisy content";
    return result;
  }

  // --- Hard reject: pixel count (overflow-safe) ---
  uint64_t total_pixels = static_cast<uint64_t>(width) * height;
  if (total_pixels > static_cast<uint64_t>(config.max_pixels)) {
    result.reason = "exceeds max pixels";
    return result;
  }

  // --- Hard reject: minimum dimension ---
  if (width < static_cast<uint32_t>(config.min_dimension) ||
      height < static_cast<uint32_t>(config.min_dimension) || width == 0 ||
      height == 0) {
    result.reason = "too small";
    return result;
  }

  // --- Hard reject: extreme aspect ratio ---
  float aspect_ratio = static_cast<float>(width) / height;
  if (aspect_ratio < config.min_aspect_ratio ||
      aspect_ratio > config.max_aspect_ratio) {
    result.reason = "extreme aspect ratio";
    return result;
  }

  // --- Buffer validation ---
  uint64_t required = total_pixels * bpp;
  if (pixels == nullptr || buffer_length < required ||
      (bpp != 1 && bpp != 3 && bpp != 4)) {
    result.reason = "invalid input";
    return result;
  }

  // --- Compute signals ---
  result.quantized_unique_colors =
      CountQuantizedUniqueColors(pixels, width, height, bpp);
  result.flat_region_ratio = ComputeFlatRegionRatio(pixels, width, height, bpp);
  result.alpha_coverage = ComputeAlphaCoverage(pixels, width, height, bpp);
  result.png_metadata_match = CheckPngMetadataFingerprint(png_meta);

  // Compute edge density and photo metric internally.
  auto pixel_format = BppToPixelFormat(bpp);
  // Use size_t to prevent overflow when width * bpp exceeds INT_MAX.
  size_t bytes_per_line_sz = static_cast<size_t>(width) * bpp;
  float edge_density = 0.0f;
  if (pixel_format != image_compression::UNSUPPORTED && width >= 3 &&
      height >= 3 && bytes_per_line_sz <= static_cast<size_t>(INT_MAX)) {
    auto* gradient_buf = static_cast<uint8_t*>(malloc(total_pixels));
    if (gradient_buf != nullptr) {
      int bytes_per_line = static_cast<int>(bytes_per_line_sz);
      if (image_compression::SobelGradient(pixels, width, height,
                                           bytes_per_line, pixel_format,
                                           handler, gradient_buf)) {
        edge_density = ComputeEdgeDensity(gradient_buf, width, height);
      }
      free(gradient_buf);
    }
  }

  float photo_metric = 0.0f;
  if (pixel_format != image_compression::UNSUPPORTED &&
      bytes_per_line_sz <= static_cast<size_t>(INT_MAX)) {
    int bytes_per_line = static_cast<int>(bytes_per_line_sz);
    photo_metric = image_compression::PhotoMetric(pixels, width, height,
                                                  bytes_per_line, pixel_format,
                                                  kHistogramThreshold, handler);
  }

  // --- Soft scoring ---
  int score = 0;

  // Quantized unique colors.
  if (result.quantized_unique_colors <= 16) {
    score += 40;
  } else if (result.quantized_unique_colors <= 64) {
    score += 30;
  } else if (result.quantized_unique_colors <= 256) {
    score += 15;
  }

  // Flat region ratio.
  if (result.flat_region_ratio > 0.7f) {
    score += 25;
  } else if (result.flat_region_ratio > 0.5f) {
    score += 12;
  }

  // Alpha coverage.
  if (result.alpha_coverage > 0.2f) {
    score += 15;
  }

  // Edge density + photo metric combination.
  if (edge_density > 0.15f && photo_metric < 12.0f) {
    score += 10;
  }

  // Low photo metric.
  if (photo_metric < 8.0f) {
    score += 10;
  }

  // Source format bonuses.
  if (source_format == ImageType::kPng && bpp == 4) {
    score += 5;
  }
  if (source_format == ImageType::kGif) {
    score += 3;
  }

  // PNG metadata SVG-origin tool.
  if (result.png_metadata_match) {
    score += 20;
  }

  // Multi-signal bonus.
  if (result.quantized_unique_colors <= 64 && result.flat_region_ratio > 0.7f &&
      result.alpha_coverage > 0.2f) {
    score += 15;
  }

  result.score = score;

  if (score >= config.candidacy_threshold) {
    result.result = SvgCandidacy::kAttempt;
    result.reason = "candidate";
  } else {
    result.reason = "score below threshold";
  }
  return result;
}

}  // namespace pagespeed
