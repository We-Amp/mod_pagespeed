// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

// PageSpeed 2.0 - Image Feature Extraction Implementation

#include "lib/image/quality_features.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>

namespace pagespeed {

namespace {

// Number of pixel samples for color entropy computation.
constexpr size_t kEntropySampleCount = 4096;

// Number of 8x8 blocks to sample for spatial frequency.
constexpr size_t kDctBlockSamples = 1000;

// Convert RGB to luminance using BT.601 coefficients.
inline float RgbToLuminance(uint8_t r, uint8_t g, uint8_t b) {
  return 0.299f * r + 0.587f * g + 0.114f * b;
}

// Convert RGB to hue (0-7 bin index for 8-bin histogram).
// Uses a simple max-component hue mapping.
int RgbToHueBin(uint8_t r, uint8_t g, uint8_t b) {
  int max_c = std::max({r, g, b});
  int min_c = std::min({r, g, b});
  int delta = max_c - min_c;
  if (delta < 8) return 0;  // Near-achromatic → bin 0.

  float hue;
  if (max_c == r) {
    hue = static_cast<float>(g - b) / delta;
    if (hue < 0) hue += 6.0f;
  } else if (max_c == g) {
    hue = 2.0f + static_cast<float>(b - r) / delta;
  } else {
    hue = 4.0f + static_cast<float>(r - g) / delta;
  }
  // Map 0-6 range to 0-7 bins.
  int bin = static_cast<int>(hue * (8.0f / 6.0f));
  return std::clamp(bin, 0, 7);
}

// Compute Shannon entropy from a histogram (normalized).
float ShannonEntropy(const float hist[], int n) {
  float entropy = 0.0f;
  for (int i = 0; i < n; ++i) {
    if (hist[i] > 0.0f) {
      entropy -= hist[i] * std::log2(hist[i]);
    }
  }
  return entropy;
}

// Simple 8x8 DCT Type II (no FFTW dependency).
// Computes one row/column of DCT coefficients at a time.
void Dct8(const float in[8], float out[8]) {
  for (int k = 0; k < 8; ++k) {
    float sum = 0.0f;
    for (int n = 0; n < 8; ++n) {
      sum += in[n] * std::cos(static_cast<float>(M_PI) * (n + 0.5f) * k / 8.0f);
    }
    out[k] = sum;
  }
}

}  // namespace

void ImageFeatures::ToFloatArray(float out[kNumFeatures],
                                 float target_ssimulacra2) const {
  out[0] = edge_density;
  out[1] = noise_level;
  out[2] = unique_color_ratio;
  out[3] = photo_metric;
  out[4] = color_entropy;
  out[5] = spatial_freq_low;
  out[6] = spatial_freq_high;
  out[7] = mean_luminance;
  out[8] = luminance_variance;
  out[9] = static_cast<float>(width);
  out[10] = static_cast<float>(height);
  out[11] = static_cast<float>(content_class);
  out[12] = static_cast<float>(source_format);
  out[13] = has_alpha ? 1.0f : 0.0f;
  out[14] = is_grayscale ? 1.0f : 0.0f;
  out[15] = target_ssimulacra2;
  out[16] = static_cast<float>(static_cast<uint64_t>(width) * height);
  out[17] = source_quality >= 0 ? static_cast<float>(source_quality)
                                : std::numeric_limits<float>::quiet_NaN();
}

ImageFeatures ExtractImageFeatures(const ContentAnalysisResult& analysis,
                                   const uint8_t* pixels, uint32_t width,
                                   uint32_t height, int bpp,
                                   uint8_t source_format, int source_quality) {
  ImageFeatures f;

  // Copy metrics from content analysis.
  f.edge_density = analysis.edge_density;
  f.noise_level = analysis.noise_level;
  f.unique_color_ratio = analysis.unique_color_ratio;
  f.photo_metric = analysis.photo_metric;

  // Metadata.
  f.width = width;
  f.height = height;
  f.content_class = static_cast<uint8_t>(analysis.content_class);
  f.source_format = source_format;
  f.has_alpha = (bpp == 4);
  f.is_grayscale = (bpp == 1);
  f.source_quality = source_quality;

  uint64_t total_pixels = static_cast<uint64_t>(width) * height;
  if (pixels == nullptr || total_pixels == 0 ||
      (bpp != 1 && bpp != 3 && bpp != 4)) {
    return f;
  }

  // --- Color entropy: 8-bin hue histogram on sampled pixels ---
  {
    float hist[8] = {};
    size_t num_samples = total_pixels < kEntropySampleCount
                             ? static_cast<size_t>(total_pixels)
                             : kEntropySampleCount;
    uint32_t prng = 0xABCD1234;
    for (size_t s = 0; s < num_samples; ++s) {
      prng = prng * 1103515245 + 12345;
      uint64_t idx = (static_cast<uint64_t>(prng >> 8) * total_pixels) >> 24;
      if (idx >= total_pixels) idx = total_pixels - 1;
      const uint8_t* p = pixels + idx * bpp;
      int bin;
      if (bpp == 1) {
        bin = 0;  // Grayscale has no hue.
      } else {
        bin = RgbToHueBin(p[0], p[1], p[2]);
      }
      hist[bin] += 1.0f;
    }
    // Normalize.
    float inv_n = 1.0f / static_cast<float>(num_samples);
    for (float& h : hist) h *= inv_n;
    f.color_entropy = ShannonEntropy(hist, 8);
  }

  // --- Spatial frequency: 8x8 DCT energy on sampled blocks ---
  {
    uint32_t blocks_x = width / 8;
    uint32_t blocks_y = height / 8;
    uint64_t total_blocks = static_cast<uint64_t>(blocks_x) * blocks_y;

    if (total_blocks > 0) {
      size_t num_blocks = total_blocks < kDctBlockSamples
                              ? static_cast<size_t>(total_blocks)
                              : kDctBlockSamples;

      double low_energy = 0.0;
      double high_energy = 0.0;
      uint32_t prng = 0x87654321;

      for (size_t s = 0; s < num_blocks; ++s) {
        prng = prng * 1103515245 + 12345;
        uint64_t block_idx =
            (static_cast<uint64_t>(prng >> 8) * total_blocks) >> 24;
        if (block_idx >= total_blocks) block_idx = total_blocks - 1;

        auto bx = static_cast<uint32_t>(block_idx % blocks_x);
        auto by = static_cast<uint32_t>(block_idx / blocks_x);

        // Extract 8x8 luminance block.
        float block[8][8];
        for (int dy = 0; dy < 8; ++dy) {
          for (int dx = 0; dx < 8; ++dx) {
            uint32_t px = bx * 8 + dx;
            uint32_t py = by * 8 + dy;
            const uint8_t* p =
                pixels + (static_cast<uint64_t>(py) * width + px) * bpp;
            if (bpp == 1) {
              block[dy][dx] = static_cast<float>(p[0]);
            } else {
              block[dy][dx] = RgbToLuminance(p[0], p[1], p[2]);
            }
          }
        }

        // Row DCT.
        float temp[8][8];
        for (int row = 0; row < 8; ++row) {
          Dct8(block[row], temp[row]);
        }

        // Column DCT.
        float dct[8][8];
        for (int col = 0; col < 8; ++col) {
          float col_in[8], col_out[8];
          for (int row = 0; row < 8; ++row) col_in[row] = temp[row][col];
          Dct8(col_in, col_out);
          for (int row = 0; row < 8; ++row) dct[row][col] = col_out[row];
        }

        // Split energy: low-freq = top-left 4x4, high-freq = rest.
        // Skip DC coefficient (0,0).
        for (int u = 0; u < 8; ++u) {
          for (int v = 0; v < 8; ++v) {
            if (u == 0 && v == 0) continue;  // Skip DC.
            double coeff_sq = static_cast<double>(dct[u][v]) * dct[u][v];
            if (u < 4 && v < 4) {
              low_energy += coeff_sq;
            } else {
              high_energy += coeff_sq;
            }
          }
        }
      }

      double inv_blocks = 1.0 / num_blocks;
      f.spatial_freq_low = static_cast<float>(low_energy * inv_blocks);
      f.spatial_freq_high = static_cast<float>(high_energy * inv_blocks);
    }
  }

  // --- Luminance statistics: single-pass Welford's algorithm ---
  {
    // Sample up to 65536 pixels for efficiency on large images.
    constexpr size_t kLumSampleMax = 65536;
    size_t num_samples = total_pixels < kLumSampleMax
                             ? static_cast<size_t>(total_pixels)
                             : kLumSampleMax;

    double mean = 0.0;
    double m2 = 0.0;

    if (num_samples == static_cast<size_t>(total_pixels)) {
      // Full scan.
      for (size_t i = 0; i < num_samples; ++i) {
        const uint8_t* p = pixels + i * bpp;
        float lum;
        if (bpp == 1) {
          lum = static_cast<float>(p[0]);
        } else {
          lum = RgbToLuminance(p[0], p[1], p[2]);
        }
        double delta = lum - mean;
        mean += delta / (i + 1);
        double delta2 = lum - mean;
        m2 += delta * delta2;
      }
    } else {
      // Sampled scan with LCG.
      uint32_t prng = 0x11223344;
      for (size_t s = 0; s < num_samples; ++s) {
        prng = prng * 1103515245 + 12345;
        uint64_t idx = (static_cast<uint64_t>(prng >> 8) * total_pixels) >> 24;
        if (idx >= total_pixels) idx = total_pixels - 1;
        const uint8_t* p = pixels + idx * bpp;
        float lum;
        if (bpp == 1) {
          lum = static_cast<float>(p[0]);
        } else {
          lum = RgbToLuminance(p[0], p[1], p[2]);
        }
        double delta = lum - mean;
        mean += delta / (s + 1);
        double delta2 = lum - mean;
        m2 += delta * delta2;
      }
    }

    f.mean_luminance = static_cast<float>(mean);
    f.luminance_variance =
        num_samples > 1 ? static_cast<float>(m2 / (num_samples - 1)) : 0.0f;
  }

  return f;
}

}  // namespace pagespeed
