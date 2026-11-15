// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

// The site-scoped cache key: an empty site keeps the existing key, a site
// gets a keyspace of its own, and the url-based cache methods honour it.

#include <cstddef>
#include <filesystem>
#include <memory>
#include <set>
#include <span>
#include <string>
#include <string_view>

#include "cyclone/key.hpp"
#include "gtest/gtest.h"
#include "lib/cache/cache.h"
#include "lib/classify/alternate_id.h"
#include "lib/classify/alternate_metadata.h"
#include "lib/classify/capability_mask.h"
#include "lib/classify/option_context.h"
#include "test/test_util/temp_dir.h"

namespace pagespeed {
namespace {

constexpr std::string_view kUrl = "/style.css";
constexpr std::string_view kHost = "example.com";
constexpr std::string_view kScheme = "https";

// Two distinct sites, 64 lowercase hex characters each.
const std::string kSiteA(64, 'a');
const std::string kSiteB = std::string(63, 'a') + "b";

std::string ContextSignature() {
  return OptionContextSignature("psoc1\nf:hw\no:ImageInlineMaxBytes=3072\n");
}

TEST(SiteCacheKeyTest, EmptySiteGivesTheExistingKey) {
  EXPECT_EQ(PageSpeedCache::ComposeKeyPreNormalized(kUrl, kHost, kScheme, ""),
            PageSpeedCache::ComposeSiteKeyPreNormalized(kUrl, kHost, kScheme,
                                                        "", ""));
  const std::string ctx = ContextSignature();
  EXPECT_EQ(PageSpeedCache::ComposeKeyPreNormalized(kUrl, kHost, kScheme, ctx),
            PageSpeedCache::ComposeSiteKeyPreNormalized(kUrl, kHost, kScheme,
                                                        ctx, ""));
}

TEST(SiteCacheKeyTest, SitedKeyWrapsTheUnsitedStringUnderItsPrefix) {
  EXPECT_EQ(cyclone::CacheKey("ks4:st1:" + kSiteA + "@https://example.com" +
                              std::string(kUrl)),
            PageSpeedCache::ComposeSiteKeyPreNormalized(kUrl, kHost, kScheme,
                                                        "", kSiteA));
  const std::string ctx = ContextSignature();
  EXPECT_EQ(cyclone::CacheKey("ks4:st1:" + kSiteA + "@oc1:" + ctx +
                              "@https://example.com" + std::string(kUrl)),
            PageSpeedCache::ComposeSiteKeyPreNormalized(kUrl, kHost, kScheme,
                                                        ctx, kSiteA));
}

TEST(SiteCacheKeyTest, SitedKeysAreDisjointFromBothUnsitedShapes) {
  const std::string ctx = ContextSignature();
  std::set<std::string> hexes = {
      PageSpeedCache::ComposeKeyPreNormalized(kUrl, kHost, kScheme).to_hex(),
      PageSpeedCache::ComposeKeyPreNormalized(kUrl, kHost, kScheme, ctx)
          .to_hex(),
      PageSpeedCache::ComposeSiteKeyPreNormalized(kUrl, kHost, kScheme, "",
                                                  kSiteA)
          .to_hex(),
      PageSpeedCache::ComposeSiteKeyPreNormalized(kUrl, kHost, kScheme, ctx,
                                                  kSiteA)
          .to_hex(),
      PageSpeedCache::ComposeSiteKeyPreNormalized(kUrl, kHost, kScheme, "",
                                                  kSiteB)
          .to_hex(),
  };
  EXPECT_EQ(5u, hexes.size());
}

TEST(SiteCacheKeyTest, AUrlShapedLikeASitedKeyDoesNotCollide) {
  // A url and hostname chosen so that the unsited string contains the whole
  // sited string. The unsited string still starts with the scheme, so the
  // two cannot be equal.
  const std::string sited_text =
      "st1:" + kSiteA + "@https://example.com" + std::string(kUrl);
  const cyclone::CacheKey sited = PageSpeedCache::ComposeSiteKeyPreNormalized(
      kUrl, kHost, kScheme, "", kSiteA);
  EXPECT_NE(sited, PageSpeedCache::ComposeKeyPreNormalized("/" + sited_text,
                                                           kHost, kScheme));
  EXPECT_NE(sited, PageSpeedCache::ComposeKeyPreNormalized(
                       std::string(kUrl), "st1:" + kSiteA + "@https:", "http"));
}

class SiteCacheMethodsTest : public ::testing::Test {
 protected:
  void SetUp() override {
    temp_dir_ = pagespeed::test::MakeTempDir();
    PageSpeedCacheConfig config;
    config.volume_path = temp_dir_ + "/cache.vol";
    config.volume_size = static_cast<uint64_t>(16 * 1024 * 1024);
    auto result = PageSpeedCache::Create(config);
    ASSERT_TRUE(result.has_value());
    cache_ = std::move(*result);
  }

  void TearDown() override {
    cache_.reset();
    std::filesystem::remove_all(temp_dir_);
  }

  bool Write(std::string_view site, std::string_view body) {
    AlternateMetadata meta;
    meta.full_mask = 0;
    meta.content_type = ContentType::kCss;
    auto wh = cache_->WriteAlternate(kUrl, kHost, kScheme, AlternateId{0},
                                     body.size(), meta, site);
    if (!wh.has_value()) return false;
    auto written = wh->write_sync(std::span<const std::byte>(
        reinterpret_cast<const std::byte*>(body.data()), body.size()));
    if (!written.has_value()) return false;
    return wh->close_sync().has_value();
  }

  std::string Read(std::string_view site) {
    auto r = cache_->ReadBestAlternate(kUrl, kHost, kScheme, CapabilityMask(),
                                       /*agent_request_entitled=*/false, site);
    if (!r.has_value()) return "<miss>";
    auto c = r->content();
    return std::string(reinterpret_cast<const char*>(c.data()), c.size());
  }

  std::string temp_dir_;
  std::unique_ptr<PageSpeedCache> cache_;
};

TEST_F(SiteCacheMethodsTest, EachSiteReadsOnlyItsOwnEntries) {
  ASSERT_TRUE(Write(kSiteA, "body-a"));
  EXPECT_EQ("body-a", Read(kSiteA));
  EXPECT_EQ("<miss>", Read(kSiteB));
  EXPECT_EQ("<miss>", Read(""));

  ASSERT_TRUE(Write("", "body-unsited"));
  ASSERT_TRUE(Write(kSiteB, "body-b"));
  EXPECT_EQ("body-a", Read(kSiteA));
  EXPECT_EQ("body-b", Read(kSiteB));
  EXPECT_EQ("body-unsited", Read(""));

  // A per-URL remove acts on one keyspace.
  ASSERT_TRUE(cache_->Remove(kUrl, kHost, kScheme, kSiteA).has_value());
  EXPECT_EQ("<miss>", Read(kSiteA));
  EXPECT_EQ("body-b", Read(kSiteB));
  EXPECT_EQ("body-unsited", Read(""));
  EXPECT_TRUE(
      cache_->AlternateExists(kUrl, kHost, kScheme, AlternateId{0}, kSiteB));
  EXPECT_FALSE(
      cache_->AlternateExists(kUrl, kHost, kScheme, AlternateId{0}, kSiteA));
}

}  // namespace
}  // namespace pagespeed
