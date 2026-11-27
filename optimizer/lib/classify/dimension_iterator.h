// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

#ifndef PAGESPEED_LIB_CLASSIFY_DIMENSION_ITERATOR_H_
#define PAGESPEED_LIB_CLASSIFY_DIMENSION_ITERATOR_H_

#include <cstddef>

#include "lib/classify/capability_mask.h"

namespace pagespeed {

// Configuration for which dimensions to iterate.
// When a dimension is disabled, only the base mask's value for that dimension
// is used. When enabled, all valid values for that dimension are iterated.
struct DimensionIteratorConfig {
  bool iterate_formats = true;    // 3 formats: Original, WebP, AVIF (skip SVG)
  bool iterate_viewports = true;  // 3 viewports: Mobile, Tablet, Desktop
  bool iterate_densities = true;  // 2 densities: 1x, 2x+
  bool iterate_savedata = true;   // 2 save-data: Off, On
  // Encoding is NOT iterated (identity variant only; gzip/brotli produced
  // by worker).
};

// Iterates over all combinations of enabled capability mask dimensions.
//
// Usage:
//   DimensionIteratorConfig config;
//   CapabilityMask base;  // The notification's original mask
//   DimensionIterator iter(config, base);
//   iter.ForEach([&](const CapabilityMask& mask) {
//     // Process this mask combination
//   });
class DimensionIterator {
 public:
  DimensionIterator(const DimensionIteratorConfig& config,
                    const CapabilityMask& base_mask)
      : config_(config),
        base_(base_mask),
        base_format_(base_mask.image_format()),
        base_viewport_(base_mask.viewport()),
        base_density_(base_mask.pixel_density()),
        base_savedata_(base_mask.save_data()) {}

  // Total number of combinations that will be produced.
  size_t Count() const {
    size_t count = 1;
    if (config_.iterate_formats) count *= 3;    // Original, WebP, AVIF
    if (config_.iterate_viewports) count *= 3;  // Mobile, Tablet, Desktop
    if (config_.iterate_densities) count *= 2;  // 1x, 2x+
    if (config_.iterate_savedata) count *= 2;   // Off, On
    return count;
  }

  // Iterate all combinations, calling fn with each mask.
  template <typename Fn>
  void ForEach(Fn&& fn) const {
    static constexpr CapabilityMask::ImageFormat kFormats[] = {
        CapabilityMask::ImageFormat::kOriginal,
        CapabilityMask::ImageFormat::kWebP,
        CapabilityMask::ImageFormat::kAvif,
    };
    static constexpr CapabilityMask::Viewport kViewports[] = {
        CapabilityMask::Viewport::kMobile,
        CapabilityMask::Viewport::kTablet,
        CapabilityMask::Viewport::kDesktop,
    };
    static constexpr CapabilityMask::PixelDensity kDensities[] = {
        CapabilityMask::PixelDensity::k1x,
        CapabilityMask::PixelDensity::k2xPlus,
    };
    static constexpr CapabilityMask::SaveData kSaveData[] = {
        CapabilityMask::SaveData::kOff,
        CapabilityMask::SaveData::kOn,
    };

    const CapabilityMask::ImageFormat* format_begin =
        config_.iterate_formats ? kFormats : &base_format_;
    const CapabilityMask::ImageFormat* format_end =
        config_.iterate_formats ? kFormats + 3 : &base_format_ + 1;

    const CapabilityMask::Viewport* viewport_begin =
        config_.iterate_viewports ? kViewports : &base_viewport_;
    const CapabilityMask::Viewport* viewport_end =
        config_.iterate_viewports ? kViewports + 3 : &base_viewport_ + 1;

    const CapabilityMask::PixelDensity* density_begin =
        config_.iterate_densities ? kDensities : &base_density_;
    const CapabilityMask::PixelDensity* density_end =
        config_.iterate_densities ? kDensities + 2 : &base_density_ + 1;

    const CapabilityMask::SaveData* savedata_begin =
        config_.iterate_savedata ? kSaveData : &base_savedata_;
    const CapabilityMask::SaveData* savedata_end =
        config_.iterate_savedata ? kSaveData + 2 : &base_savedata_ + 1;

    for (const auto* fmt = format_begin; fmt != format_end; ++fmt) {
      for (const auto* vp = viewport_begin; vp != viewport_end; ++vp) {
        for (const auto* den = density_begin; den != density_end; ++den) {
          for (const auto* sd = savedata_begin; sd != savedata_end; ++sd) {
            CapabilityMask mask(*fmt, *vp, *den, *sd,
                                base_.transfer_encoding());
            fn(mask);
          }
        }
      }
    }
  }

  // Factory: create an iterator for warmup requests.
  // Uses the default mask (0x08 = Desktop/Identity) as the base,
  // NOT a decoded sentinel mask.
  static DimensionIterator ForWarmup(const DimensionIteratorConfig& config) {
    return DimensionIterator(config, CapabilityMask());
  }

 private:
  DimensionIteratorConfig config_;
  CapabilityMask base_;

  // Stored base dimension values so we can take their address when
  // a dimension is not iterated (single-element iteration).
  CapabilityMask::ImageFormat base_format_;
  CapabilityMask::Viewport base_viewport_;
  CapabilityMask::PixelDensity base_density_;
  CapabilityMask::SaveData base_savedata_;
};

}  // namespace pagespeed

#endif  // PAGESPEED_LIB_CLASSIFY_DIMENSION_ITERATOR_H_
