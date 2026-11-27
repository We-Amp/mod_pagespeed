// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

// PageSpeed 2.0 - SVG Preprocessor Implementation

#include "lib/image/svg_preprocessor.h"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <limits>
#include <map>
#include <unordered_set>
#include <vector>

namespace pagespeed {

namespace {

// Packed RGB color for histogram and palette operations.
struct RGBColor {
  uint8_t r, g, b;

  bool operator<(const RGBColor& o) const {
    if (r != o.r) return r < o.r;
    if (g != o.g) return g < o.g;
    return b < o.b;
  }

  bool operator==(const RGBColor& o) const {
    return r == o.r && g == o.g && b == o.b;
  }
};

// A color box for median-cut: holds a set of histogram entries.
struct ColorBox {
  struct Entry {
    RGBColor color;
    uint64_t count;
  };

  std::vector<Entry> entries;

  // Range of each channel in this box.
  [[nodiscard]] int RangeR() const {
    uint8_t lo = 255, hi = 0;
    for (const auto& e : entries) {
      lo = std::min(lo, e.color.r);
      hi = std::max(hi, e.color.r);
    }
    return hi - lo;
  }
  [[nodiscard]] int RangeG() const {
    uint8_t lo = 255, hi = 0;
    for (const auto& e : entries) {
      lo = std::min(lo, e.color.g);
      hi = std::max(hi, e.color.g);
    }
    return hi - lo;
  }
  [[nodiscard]] int RangeB() const {
    uint8_t lo = 255, hi = 0;
    for (const auto& e : entries) {
      lo = std::min(lo, e.color.b);
      hi = std::max(hi, e.color.b);
    }
    return hi - lo;
  }

  // Maximum range across all channels.
  [[nodiscard]] int MaxRange() const {
    return std::max({RangeR(), RangeG(), RangeB()});
  }

  // Widest channel: 0=R, 1=G, 2=B.
  [[nodiscard]] int WidestChannel() const {
    int rr = RangeR(), rg = RangeG(), rb = RangeB();
    if (rr >= rg && rr >= rb) return 0;
    if (rg >= rb) return 1;
    return 2;
  }

  // Weighted average color of all entries.
  [[nodiscard]] RGBColor AverageColor() const {
    uint64_t sum_r = 0, sum_g = 0, sum_b = 0, total = 0;
    for (const auto& e : entries) {
      sum_r += static_cast<uint64_t>(e.color.r) * e.count;
      sum_g += static_cast<uint64_t>(e.color.g) * e.count;
      sum_b += static_cast<uint64_t>(e.color.b) * e.count;
      total += e.count;
    }
    if (total == 0) return {0, 0, 0};
    return {
        static_cast<uint8_t>(sum_r / total),
        static_cast<uint8_t>(sum_g / total),
        static_cast<uint8_t>(sum_b / total),
    };
  }

  // Total pixel count.
  [[nodiscard]] uint64_t TotalCount() const {
    uint64_t t = 0;
    for (const auto& e : entries) t += e.count;
    return t;
  }
};

// Sort entries by the given channel (0=R, 1=G, 2=B).
void SortByChannel(std::vector<ColorBox::Entry>& entries, int ch) {
  std::sort(entries.begin(), entries.end(),
            [ch](const ColorBox::Entry& a, const ColorBox::Entry& b) {
              switch (ch) {
                case 0:
                  return a.color.r < b.color.r;
                case 1:
                  return a.color.g < b.color.g;
                default:
                  return a.color.b < b.color.b;
              }
            });
}

// Split a box along the median of its widest channel.
// Returns the upper half; the input box is modified to be the lower
// half.
ColorBox SplitBox(ColorBox& box) {
  int ch = box.WidestChannel();
  SortByChannel(box.entries, ch);

  // Find median by pixel count.
  uint64_t total = box.TotalCount();
  uint64_t half = total / 2;
  uint64_t cumulative = 0;
  size_t split_idx = 0;
  for (size_t i = 0; i < box.entries.size(); ++i) {
    cumulative += box.entries[i].count;
    if (cumulative >= half) {
      // Split after this entry (but ensure both halves are
      // non-empty).
      split_idx = i + 1;
      break;
    }
  }

  // Ensure non-empty halves.
  if (split_idx == 0) split_idx = 1;
  if (split_idx >= box.entries.size()) {
    split_idx = box.entries.size() - 1;
  }

  ColorBox upper;
  upper.entries.assign(box.entries.begin() + split_idx, box.entries.end());
  box.entries.resize(split_idx);
  return upper;
}

// Squared Euclidean distance in RGB space.
inline int ColorDistSq(const RGBColor& a, const RGBColor& b) {
  int dr = static_cast<int>(a.r) - static_cast<int>(b.r);
  int dg = static_cast<int>(a.g) - static_cast<int>(b.g);
  int db = static_cast<int>(a.b) - static_cast<int>(b.b);
  return dr * dr + dg * dg + db * db;
}

// Find nearest palette color index.
int NearestPaletteIndex(const RGBColor& c,
                        const std::vector<RGBColor>& palette) {
  int best_idx = 0;
  int best_dist = std::numeric_limits<int>::max();
  for (size_t i = 0; i < palette.size(); ++i) {
    int d = ColorDistSq(c, palette[i]);
    if (d < best_dist) {
      best_dist = d;
      best_idx = static_cast<int>(i);
    }
  }
  return best_idx;
}

}  // namespace

void QuantizeColors(uint8_t* pixels, uint32_t width, uint32_t height, int bpp,
                    int max_colors) {
  if (max_colors <= 0 || width == 0 || height == 0 || pixels == nullptr) return;
  if (bpp != 3 && bpp != 4) return;

  uint64_t total = static_cast<uint64_t>(width) * height;

  // Step 1: Build color histogram (RGB only, ignore alpha).
  // Quantize to 5-bit per channel for bucketing to limit histogram
  // size.
  std::map<RGBColor, uint64_t> histogram;
  for (uint64_t i = 0; i < total; ++i) {
    size_t idx = static_cast<size_t>(i) * bpp;
    // Quantize to 5 bits (32 levels) per channel for bucketing.
    uint8_t r5 = (pixels[idx + 0] >> 3) << 3;
    uint8_t g5 = (pixels[idx + 1] >> 3) << 3;
    uint8_t b5 = (pixels[idx + 2] >> 3) << 3;
    histogram[{r5, g5, b5}]++;
  }

  // If we already have <= max_colors unique bucket colors, no need
  // to quantize.
  if (static_cast<int>(histogram.size()) <= max_colors) return;

  // Step 2: Build initial color box from histogram.
  ColorBox initial;
  initial.entries.reserve(histogram.size());
  for (const auto& [color, count] : histogram) {
    initial.entries.push_back({color, count});
  }

  // Step 3: Median-cut splitting.
  std::vector<ColorBox> boxes;
  boxes.push_back(std::move(initial));

  while (static_cast<int>(boxes.size()) < max_colors) {
    // Find the box with the largest color range.
    int best_box = -1;
    int best_range = 0;
    for (size_t i = 0; i < boxes.size(); ++i) {
      if (boxes[i].entries.size() < 2) continue;
      int r = boxes[i].MaxRange();
      if (r > best_range) {
        best_range = r;
        best_box = static_cast<int>(i);
      }
    }
    // No splittable box found.
    if (best_box < 0) break;

    ColorBox upper = SplitBox(boxes[best_box]);
    boxes.push_back(std::move(upper));
  }

  // Step 4: Build palette from box averages.
  std::vector<RGBColor> palette;
  palette.reserve(boxes.size());
  for (const auto& box : boxes) {
    palette.push_back(box.AverageColor());
  }

  // Step 5: Remap all pixels to nearest palette color.
  for (uint64_t i = 0; i < total; ++i) {
    size_t idx = static_cast<size_t>(i) * bpp;
    RGBColor pixel_color = {pixels[idx], pixels[idx + 1], pixels[idx + 2]};
    int nearest = NearestPaletteIndex(pixel_color, palette);
    pixels[idx + 0] = palette[nearest].r;
    pixels[idx + 1] = palette[nearest].g;
    pixels[idx + 2] = palette[nearest].b;
    // Alpha (if bpp==4) is preserved unchanged.
  }
}

void ThresholdAlpha(uint8_t* pixels, uint32_t width, uint32_t height, int bpp,
                    uint8_t threshold) {
  if (bpp != 4 || pixels == nullptr) return;
  uint64_t total = static_cast<uint64_t>(width) * height;
  for (uint64_t i = 0; i < total; ++i) {
    uint8_t& alpha = pixels[i * 4 + 3];
    alpha = (alpha >= threshold) ? 255 : 0;
  }
}

void MorphologicalClose(uint8_t* pixels, uint32_t width, uint32_t height,
                        int bpp, int radius) {
  if (bpp != 4 || radius <= 0 || pixels == nullptr) return;
  if (width == 0 || height == 0) return;

  uint64_t total = static_cast<uint64_t>(width) * height;
  int w = static_cast<int>(width);
  int h = static_cast<int>(height);

  // Temporary buffer for alpha channel.
  std::vector<uint8_t> temp(total);
  std::vector<uint8_t> result(total);

  // Extract alpha channel.
  for (uint64_t i = 0; i < total; ++i) {
    temp[i] = pixels[i * 4 + 3];
  }

  // Step 1: Dilate (max filter).
  for (int y = 0; y < h; ++y) {
    for (int x = 0; x < w; ++x) {
      uint8_t max_val = 0;
      for (int dy = -radius; dy <= radius; ++dy) {
        int ny = y + dy;
        if (ny < 0 || ny >= h) continue;
        for (int dx = -radius; dx <= radius; ++dx) {
          int nx = x + dx;
          if (nx < 0 || nx >= w) continue;
          uint8_t val = temp[static_cast<size_t>(ny) * w + nx];
          max_val = std::max(max_val, val);
        }
      }
      result[static_cast<size_t>(y) * w + x] = max_val;
    }
  }

  // Step 2: Erode (min filter) on the dilated result.
  // Reuse temp as output for erode.
  for (int y = 0; y < h; ++y) {
    for (int x = 0; x < w; ++x) {
      uint8_t min_val = 255;
      for (int dy = -radius; dy <= radius; ++dy) {
        int ny = y + dy;
        if (ny < 0 || ny >= h) continue;
        for (int dx = -radius; dx <= radius; ++dx) {
          int nx = x + dx;
          if (nx < 0 || nx >= w) continue;
          uint8_t val = result[static_cast<size_t>(ny) * w + nx];
          min_val = std::min(min_val, val);
        }
      }
      temp[static_cast<size_t>(y) * w + x] = min_val;
    }
  }

  // Step 3: Write eroded alpha back to pixel buffer.
  for (uint64_t i = 0; i < total; ++i) {
    pixels[i * 4 + 3] = temp[i];
  }
}

PreprocessResult PreprocessPixels(const uint8_t* pixels, uint32_t width,
                                  uint32_t height, int bpp,
                                  const PreprocessConfig& config) {
  PreprocessResult result;
  result.width = width;
  result.height = height;

  if (pixels == nullptr || width == 0 || height == 0) return result;
  if (bpp != 1 && bpp != 3 && bpp != 4) return result;

  // Copy pixels to mutable buffer (always as RGBA).
  uint64_t pixel_count = static_cast<uint64_t>(width) * height;
  // Cap at 50MB / 4 bytes-per-pixel to prevent OOM on huge images.
  constexpr uint64_t kMaxPreprocessPixels = 50ULL * 1024 * 1024 / 4;
  if (pixel_count > kMaxPreprocessPixels) return result;
  result.pixel_buffer.resize(pixel_count * 4);

  auto* buf = reinterpret_cast<uint8_t*>(result.pixel_buffer.data());

  if (bpp == 4) {
    std::memcpy(buf, pixels, pixel_count * 4);
  } else if (bpp == 3) {
    // Convert RGB to RGBA (alpha = 255).
    for (uint64_t i = 0; i < pixel_count; ++i) {
      buf[i * 4 + 0] = pixels[i * 3 + 0];
      buf[i * 4 + 1] = pixels[i * 3 + 1];
      buf[i * 4 + 2] = pixels[i * 3 + 2];
      buf[i * 4 + 3] = 255;
    }
  } else if (bpp == 1) {
    // Convert grayscale to RGBA.
    for (uint64_t i = 0; i < pixel_count; ++i) {
      uint8_t g = pixels[i];
      buf[i * 4 + 0] = g;
      buf[i * 4 + 1] = g;
      buf[i * 4 + 2] = g;
      buf[i * 4 + 3] = 255;
    }
  }

  // Step 1: Color quantization.
  QuantizeColors(buf, width, height, 4, config.max_colors);

  // Step 2: Alpha thresholding.
  ThresholdAlpha(buf, width, height, 4, config.alpha_threshold);

  // Step 3: Optional morphological close.
  if (config.morphological_close) {
    MorphologicalClose(buf, width, height, 4, config.close_radius);
  }

  // Count actual unique colors in the result.
  std::unordered_set<uint32_t> unique_colors;
  for (uint64_t i = 0; i < pixel_count; ++i) {
    uint32_t packed = (static_cast<uint32_t>(buf[i * 4 + 0]) << 16) |
                      (static_cast<uint32_t>(buf[i * 4 + 1]) << 8) |
                      static_cast<uint32_t>(buf[i * 4 + 2]);
    unique_colors.insert(packed);
  }
  result.actual_colors = static_cast<int>(unique_colors.size());

  return result;
}

}  // namespace pagespeed
