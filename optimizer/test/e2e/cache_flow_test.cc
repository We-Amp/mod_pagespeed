// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

// End-to-End Cache Flow Test
//
// Tests the complete flow from capability detection through cache lookup
// using the PageSpeedCache API (Cyclone native alternates):
// 1. Create capability mask from headers
// 2. WriteAlternate to cache (record-on-miss)
// 3. ReadBestAlternate (automatic best-fit scoring)
// 4. Fallback: ReadBestAlternate returns best available alternate
// 5. Cache miss handling
//
// This tests the same path the nginx module and worker use.

#include <cstddef>
#include <cstring>
#include <filesystem>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "gtest/gtest.h"
#include "lib/cache/cache.h"
#include "lib/classify/alternate_id.h"
#include "lib/classify/alternate_metadata.h"
#include "lib/classify/capability_mask.h"
#include "test/test_util/temp_dir.h"

using pagespeed::AlternateId;
using pagespeed::AlternateMetadata;
using pagespeed::CapabilityMask;
using pagespeed::ContentType;
using pagespeed::MaskToAlternateId;
using pagespeed::PageSpeedCache;
using pagespeed::PageSpeedCacheConfig;
using pagespeed::SentinelId;

namespace {

class CacheFlowTest : public ::testing::Test {
 protected:
  void SetUp() override {
    cache_dir_ = pagespeed::test::MakeTempDir();
    cache_path_ = cache_dir_ + "/cache.vol";

    PageSpeedCacheConfig config;
    config.volume_path = cache_path_;
    config.volume_size = static_cast<uint64_t>(10 * 1024 * 1024);

    auto result = PageSpeedCache::Create(config);
    ASSERT_TRUE(result.has_value()) << "Failed to open cache";
    cache_ = std::move(*result);
  }

  void TearDown() override {
    cache_.reset();
    if (!cache_dir_.empty()) {
      std::error_code ec;
      std::filesystem::remove_all(cache_dir_, ec);
    }
  }

  // Helper: write content at a given mask (what nginx does on record-on-miss).
  void WriteAtMask(std::string_view url, const CapabilityMask& mask,
                   std::string_view data, ContentType ct = ContentType::kHtml) {
    AlternateId id =
        MaskToAlternateId(static_cast<uint8_t>(mask.Encode() & 0xFF));
    AlternateMetadata meta;
    meta.full_mask = mask.Encode();
    meta.content_type = ct;

    auto wh = cache_->WriteAlternate(url, "", "", id, data.size(), meta);
    ASSERT_TRUE(wh.has_value());
    auto bytes = std::as_bytes(std::span(data));
    ASSERT_TRUE(wh->write_sync(bytes).has_value());
    ASSERT_TRUE(wh->close_sync().has_value());
  }

  std::string cache_dir_;
  std::string cache_path_;
  std::unique_ptr<PageSpeedCache> cache_;
};

// =============================================================================
// Basic E2E Flow
// =============================================================================

TEST_F(CacheFlowTest, FullFlowWithHit) {
  // Simulate a request with WebP support and desktop viewport
  CapabilityMask mask = CapabilityMask::FromHeaders(
      "image/webp,image/png,*/*", "Mozilla/5.0 (Windows NT 10.0; Win64; x64)",
      "", "4g");

  uint32_t encoded_mask = mask.Encode();
  EXPECT_NE(encoded_mask, 0u);

  std::string_view url = "https://example.com/page.html";

  // Step 1: Verify cache miss
  auto read_result = cache_->ReadBestAlternate(url, "", "", mask);
  EXPECT_FALSE(read_result.has_value());

  // Step 2: Write optimized content to cache
  const std::string content =
      "<html><head></head><body>Optimized!</body></html>";
  WriteAtMask(url, mask, content);

  // Step 3: Lookup again (hit)
  read_result = cache_->ReadBestAlternate(url, "", "", mask);
  ASSERT_TRUE(read_result.has_value());

  // Step 4: Verify cached data
  auto data = read_result->content();
  std::string_view read_data(reinterpret_cast<const char*>(data.data()),
                             data.size());
  EXPECT_EQ(read_data, content);
}

// =============================================================================
// Capability Fallback Flow
// =============================================================================

TEST_F(CacheFlowTest, CapabilityFallbackFlow) {
  // Scenario: User requests WebP, but cache only has JPEG (original) version.
  // ReadBestAlternate should return the best available alternate.

  std::string_view url = "https://example.com/image.jpg";
  const std::string jpeg_content = "JPEG image data";

  // Store only JPEG version (original at default mask)
  CapabilityMask default_mask;
  WriteAtMask(url, default_mask, jpeg_content, ContentType::kImage);

  // Client requests WebP — ReadBestAlternate finds the original as fallback
  CapabilityMask webp_mask =
      CapabilityMask::FromHeaders("image/webp,*/*", "Chrome/100", "", "4g");

  auto result = cache_->ReadBestAlternate(url, "", "", webp_mask);
  ASSERT_TRUE(result.has_value()) << "Fallback should find original";

  auto data = result->content();
  std::string_view read_data(reinterpret_cast<const char*>(data.data()),
                             data.size());
  EXPECT_EQ(read_data, jpeg_content);

  // Verify it returned the original (different mask), not exact match
  EXPECT_NE(result->metadata.full_mask, webp_mask.Encode());
  EXPECT_EQ(result->metadata.full_mask, default_mask.Encode());
}

// =============================================================================
// Purge → rebuild flow (issue #652)
// =============================================================================

TEST_F(CacheFlowTest, PurgeThenRebuildNeverServesPrePurgeStamp) {
  // Issue #652: after a single-URL purge, no read may return the pre-purge
  // variant set or its pre-purge cache_inserted_at provenance.  This is the
  // cache-API contract underlying the worker's purge fence: Remove() makes
  // the whole key vanish, and a rebuild stores fresh provenance.
  std::string_view url = "https://example.com/purged-page.html";
  constexpr uint32_t kPrePurgeStamp = 1000;
  constexpr uint32_t kPostPurgeStamp = 2000;

  auto write_with_stamp = [&](const CapabilityMask& mask,
                              std::string_view data, uint32_t stamp) {
    AlternateId id =
        MaskToAlternateId(static_cast<uint8_t>(mask.Encode() & 0xFF));
    AlternateMetadata meta;
    meta.full_mask = mask.Encode();
    meta.content_type = ContentType::kHtml;
    meta.cache_inserted_at = stamp;
    auto wh = cache_->WriteAlternate(url, "", "", id, data.size(), meta);
    ASSERT_TRUE(wh.has_value());
    ASSERT_TRUE(wh->write_sync(std::as_bytes(std::span(data))).has_value());
    ASSERT_TRUE(wh->close_sync().has_value());
  };

  // Original (identity) + a compressed variant, both pre-purge.
  CapabilityMask default_mask;
  CapabilityMask gzip_mask;
  gzip_mask.set_transfer_encoding(CapabilityMask::TransferEncoding::kGzip);
  write_with_stamp(default_mask, "<html>old</html>", kPrePurgeStamp);
  write_with_stamp(gzip_mask, "old-gzip-bytes", kPrePurgeStamp);

  // Purge (what Worker::InvalidateUrl does under the generation fence).
  ASSERT_TRUE(cache_->Remove(url, "", "").has_value());

  // Every read must miss — including the variant the purge was meant to
  // delete (the issue #652 symptom was the old variant surviving).
  EXPECT_FALSE(cache_->ReadBestAlternate(url, "", "", default_mask)
                   .has_value());
  EXPECT_FALSE(cache_->ReadBestAlternate(url, "", "", gzip_mask).has_value());

  // Rebuild with fresh provenance (what the next MISS fill does).
  write_with_stamp(default_mask, "<html>new</html>", kPostPurgeStamp);

  // Reads now return ONLY post-purge provenance and content.
  auto result = cache_->ReadBestAlternate(url, "", "", default_mask);
  ASSERT_TRUE(result.has_value());
  EXPECT_EQ(result->metadata.cache_inserted_at, kPostPurgeStamp);
  auto data = result->content();
  EXPECT_EQ(std::string_view(reinterpret_cast<const char*>(data.data()),
                             data.size()),
            "<html>new</html>");

  // A gzip-capable read must NOT resurrect the purged gzip variant; the
  // best alternate is the fresh identity original.
  auto gz_result = cache_->ReadBestAlternate(url, "", "", gzip_mask);
  ASSERT_TRUE(gz_result.has_value());
  EXPECT_EQ(gz_result->metadata.cache_inserted_at, kPostPurgeStamp);
  EXPECT_EQ(gz_result->metadata.full_mask, default_mask.Encode());
}

// =============================================================================
// Cache Miss Flow
// =============================================================================

TEST_F(CacheFlowTest, CacheMissAndSubsequentHit) {
  std::string_view url = "https://example.com/new-page.html";

  CapabilityMask mask = CapabilityMask::FromHeaders("*/*", "curl/7.0", "", "");

  // Lookup - should miss
  auto read_result = cache_->ReadBestAlternate(url, "", "", mask);
  EXPECT_FALSE(read_result.has_value());

  // Simulate: store optimized content after worker processes it
  const std::string optimized = "<html optimized>content</html>";
  WriteAtMask(url, mask, optimized);

  // Subsequent request should hit cache
  read_result = cache_->ReadBestAlternate(url, "", "", mask);
  ASSERT_TRUE(read_result.has_value());

  auto data = read_result->content();
  std::string_view read_data(reinterpret_cast<const char*>(data.data()),
                             data.size());
  EXPECT_EQ(read_data, optimized);
}

// =============================================================================
// Multiple Content Variants
// =============================================================================

TEST_F(CacheFlowTest, MultipleContentVariants) {
  std::string_view url = "https://example.com/resource";

  struct TestCase {
    CapabilityMask mask;
    std::string content;
  };

  TestCase cases[] = {
      {CapabilityMask(CapabilityMask::ImageFormat::kWebP,
                      CapabilityMask::Viewport::kDesktop,
                      CapabilityMask::PixelDensity::k1x,
                      CapabilityMask::SaveData::kOff,
                      CapabilityMask::TransferEncoding::kIdentity),
       "WebP image data"},
      {CapabilityMask(CapabilityMask::ImageFormat::kAvif,
                      CapabilityMask::Viewport::kDesktop,
                      CapabilityMask::PixelDensity::k1x,
                      CapabilityMask::SaveData::kOff,
                      CapabilityMask::TransferEncoding::kIdentity),
       "AVIF image data"},
      {CapabilityMask(), "Original JPEG data"},
  };

  // Write different variants
  for (const auto& tc : cases) {
    WriteAtMask(url, tc.mask, tc.content, ContentType::kImage);
  }

  // Verify each client mask gets the right variant
  for (const auto& tc : cases) {
    auto result = cache_->ReadBestAlternate(url, "", "", tc.mask);
    ASSERT_TRUE(result.has_value())
        << "Failed to find mask 0x" << std::hex << tc.mask.Encode();

    auto data = result->content();
    std::string_view read_data(reinterpret_cast<const char*>(data.data()),
                               data.size());
    EXPECT_EQ(read_data, tc.content);
    EXPECT_EQ(result->metadata.full_mask, tc.mask.Encode());
  }
}

// =============================================================================
// Default Mask Fallback Returns Original Content
// =============================================================================

TEST_F(CacheFlowTest, FallbackReturnsOriginal) {
  std::string_view url = "https://example.com/style.css";
  const std::string original_css = "body { margin: 0; padding: 0; }";
  const std::string minified_css = "body{margin:0;padding:0}";

  // Store original at default mask (what nginx record-on-miss does)
  CapabilityMask default_mask;
  WriteAtMask(url, default_mask, original_css, ContentType::kCss);

  // Client with different capabilities gets fallback to original
  CapabilityMask client_mask =
      CapabilityMask::FromHeaders("image/webp,*/*", "Chrome/100", "", "4g");
  ASSERT_NE(client_mask.Encode(), default_mask.Encode())
      << "Client mask should differ from default";

  auto result = cache_->ReadBestAlternate(url, "", "", client_mask);
  ASSERT_TRUE(result.has_value());

  auto data = result->content();
  std::string_view read_data(reinterpret_cast<const char*>(data.data()),
                             data.size());
  EXPECT_EQ(read_data, original_css);

  // Now store the minified variant at the client's exact mask
  WriteAtMask(url, client_mask, minified_css, ContentType::kCss);

  // Same client should now get the minified version (exact match)
  auto result2 = cache_->ReadBestAlternate(url, "", "", client_mask);
  ASSERT_TRUE(result2.has_value());

  auto data2 = result2->content();
  std::string_view read_data2(reinterpret_cast<const char*>(data2.data()),
                              data2.size());
  EXPECT_EQ(read_data2, minified_css);
  EXPECT_EQ(result2->metadata.full_mask, client_mask.Encode());
}

// =============================================================================
// Mask Consistency: same headers -> same result
// =============================================================================

TEST_F(CacheFlowTest, MaskConsistencyAcrossRequests) {
  std::string_view accept = "*/*";
  std::string_view user_agent = "python-requests/2.31.0";

  CapabilityMask mask1 =
      CapabilityMask::FromHeaders(accept, user_agent, "", "");
  CapabilityMask mask2 =
      CapabilityMask::FromHeaders(accept, user_agent, "", "");

  EXPECT_EQ(mask1.Encode(), mask2.Encode())
      << "Same headers must produce same mask";

  // Write with mask1, read with mask2 — should find it
  std::string_view url = "/style.css";
  WriteAtMask(url, mask1, "css-data", ContentType::kCss);

  auto result = cache_->ReadBestAlternate(url, "", "", mask2);
  ASSERT_TRUE(result.has_value());
  EXPECT_EQ(result->metadata.full_mask, mask1.Encode());
}

// =============================================================================
// Save-Data Variants Are Not Served To Clients Without Save-Data
// =============================================================================

TEST_F(CacheFlowTest, SaveDataVariantNotServedToNormalClient) {
  std::string_view url = "https://example.com/hero.jpg";
  auto mask = [](CapabilityMask::ImageFormat fmt, CapabilityMask::SaveData sd) {
    return CapabilityMask(fmt, CapabilityMask::Viewport::kDesktop,
                          CapabilityMask::PixelDensity::k1x, sd,
                          CapabilityMask::TransferEncoding::kIdentity);
  };
  using F = CapabilityMask::ImageFormat;
  using S = CapabilityMask::SaveData;

  // The normal WebP slot is empty; the Save-Data WebP and the normal JPEG
  // are stored.
  WriteAtMask(url, mask(F::kWebP, S::kOn), "sd-webp", ContentType::kImage);
  WriteAtMask(url, mask(F::kOriginal, S::kOff), "normal-jpeg",
              ContentType::kImage);

  auto read = [&](const CapabilityMask& client) {
    auto result = cache_->ReadBestAlternate(url, "", "", client);
    EXPECT_TRUE(result.has_value());
    if (!result.has_value()) return std::string();
    auto data = result->content();
    return std::string(reinterpret_cast<const char*>(data.data()), data.size());
  };

  // A normal WebP client gets the normal bytes, a Save-Data client its own.
  EXPECT_EQ(read(mask(F::kWebP, S::kOff)), "normal-jpeg");
  EXPECT_EQ(read(mask(F::kWebP, S::kOn)), "sd-webp");

  // Once the normal WebP slot is filled, the normal client takes it.
  WriteAtMask(url, mask(F::kWebP, S::kOff), "normal-webp", ContentType::kImage);
  EXPECT_EQ(read(mask(F::kWebP, S::kOff)), "normal-webp");
  EXPECT_EQ(read(mask(F::kWebP, S::kOn)), "sd-webp");
}

}  // namespace
