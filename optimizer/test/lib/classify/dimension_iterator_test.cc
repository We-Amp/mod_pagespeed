// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

#include "lib/classify/dimension_iterator.h"

#include <cstdint>
#include <set>

#include "gtest/gtest.h"

namespace pagespeed {
namespace {

TEST(DimensionIteratorTest, AllEnabledProduces36) {
  DimensionIteratorConfig config;
  // All defaults are true
  DimensionIterator iter(config, CapabilityMask());
  EXPECT_EQ(iter.Count(), 36u);  // 3*3*2*2

  size_t actual = 0;
  std::set<uint32_t> seen_masks;
  iter.ForEach([&](const CapabilityMask& mask) {
    ++actual;
    seen_masks.insert(mask.Encode());
  });
  EXPECT_EQ(actual, 36u);
  EXPECT_EQ(seen_masks.size(), 36u);  // All unique
}

TEST(DimensionIteratorTest, FormatsOnlyProduces3) {
  DimensionIteratorConfig config;
  config.iterate_viewports = false;
  config.iterate_densities = false;
  config.iterate_savedata = false;
  DimensionIterator iter(config, CapabilityMask());
  EXPECT_EQ(iter.Count(), 3u);

  size_t actual = 0;
  iter.ForEach([&](const CapabilityMask& mask) {
    ++actual;
    // Encoding should be preserved from base (Identity)
    EXPECT_EQ(mask.transfer_encoding(),
              CapabilityMask::TransferEncoding::kIdentity);
  });
  EXPECT_EQ(actual, 3u);
}

TEST(DimensionIteratorTest, ViewportsOnlyProduces3) {
  DimensionIteratorConfig config;
  config.iterate_formats = false;
  config.iterate_densities = false;
  config.iterate_savedata = false;
  DimensionIterator iter(config, CapabilityMask());
  EXPECT_EQ(iter.Count(), 3u);
}

TEST(DimensionIteratorTest, DensitiesOnlyProduces2) {
  DimensionIteratorConfig config;
  config.iterate_formats = false;
  config.iterate_viewports = false;
  config.iterate_savedata = false;
  DimensionIterator iter(config, CapabilityMask());
  EXPECT_EQ(iter.Count(), 2u);
}

TEST(DimensionIteratorTest, SaveDataOnlyProduces2) {
  DimensionIteratorConfig config;
  config.iterate_formats = false;
  config.iterate_viewports = false;
  config.iterate_densities = false;
  DimensionIterator iter(config, CapabilityMask());
  EXPECT_EQ(iter.Count(), 2u);
}

TEST(DimensionIteratorTest, NoneEnabledProduces1) {
  DimensionIteratorConfig config;
  config.iterate_formats = false;
  config.iterate_viewports = false;
  config.iterate_densities = false;
  config.iterate_savedata = false;

  CapabilityMask base(
      CapabilityMask::ImageFormat::kWebP, CapabilityMask::Viewport::kMobile,
      CapabilityMask::PixelDensity::k2xPlus, CapabilityMask::SaveData::kOn,
      CapabilityMask::TransferEncoding::kBrotli);
  DimensionIterator iter(config, base);
  EXPECT_EQ(iter.Count(), 1u);

  size_t actual = 0;
  iter.ForEach([&](const CapabilityMask& mask) {
    ++actual;
    EXPECT_EQ(mask.Encode(), base.Encode());
  });
  EXPECT_EQ(actual, 1u);
}

TEST(DimensionIteratorTest, ForWarmupUsesDefaultMask) {
  DimensionIteratorConfig config;
  config.iterate_formats = false;
  config.iterate_viewports = false;
  config.iterate_densities = false;
  config.iterate_savedata = false;

  auto iter = DimensionIterator::ForWarmup(config);
  EXPECT_EQ(iter.Count(), 1u);

  iter.ForEach([&](const CapabilityMask& mask) {
    // Default mask = 0x08 (Desktop/Identity/Original/1x/Off)
    EXPECT_EQ(mask.Encode(), CapabilityMask().Encode());
    EXPECT_EQ(mask.Encode(), 0x08u);
  });
}

TEST(DimensionIteratorTest, ConnectionPreservedFromBase) {
  DimensionIteratorConfig config;
  // Iterate only formats
  config.iterate_viewports = false;
  config.iterate_densities = false;
  config.iterate_savedata = false;

  CapabilityMask base(
      CapabilityMask::ImageFormat::kOriginal,
      CapabilityMask::Viewport::kDesktop, CapabilityMask::PixelDensity::k1x,
      CapabilityMask::SaveData::kOff, CapabilityMask::TransferEncoding::kGzip);
  DimensionIterator iter(config, base);

  iter.ForEach([&](const CapabilityMask& mask) {
    EXPECT_EQ(mask.transfer_encoding(),
              CapabilityMask::TransferEncoding::kGzip);
  });
}

TEST(DimensionIteratorTest, FormatsAndViewportsProduces9) {
  DimensionIteratorConfig config;
  config.iterate_densities = false;
  config.iterate_savedata = false;
  DimensionIterator iter(config, CapabilityMask());
  EXPECT_EQ(iter.Count(), 9u);  // 3*3

  size_t actual = 0;
  std::set<uint32_t> seen_masks;
  iter.ForEach([&](const CapabilityMask& mask) {
    ++actual;
    seen_masks.insert(mask.Encode());
  });
  EXPECT_EQ(actual, 9u);
  EXPECT_EQ(seen_masks.size(), 9u);
}

TEST(DimensionIteratorTest, BaseViewportPreservedWhenNotIterated) {
  DimensionIteratorConfig config;
  config.iterate_formats = false;
  config.iterate_viewports = false;
  config.iterate_densities = false;
  config.iterate_savedata = false;

  CapabilityMask base(
      CapabilityMask::ImageFormat::kOriginal, CapabilityMask::Viewport::kTablet,
      CapabilityMask::PixelDensity::k1x, CapabilityMask::SaveData::kOff,
      CapabilityMask::TransferEncoding::kIdentity);
  DimensionIterator iter(config, base);

  iter.ForEach([&](const CapabilityMask& mask) {
    EXPECT_EQ(mask.viewport(), CapabilityMask::Viewport::kTablet);
  });
}

}  // namespace
}  // namespace pagespeed
