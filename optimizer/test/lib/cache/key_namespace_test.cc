// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 We-Amp B.V.

// Every entry this version stores lives under keys that an earlier version
// never computes, and the reverse: neither the versions that composed keys
// without a namespace nor those that used the earlier "ks2:" and "ks3:"
// namespaces.  One case per class of stored entry, in both directions: what
// this version writes cannot be found under the key an earlier version
// computes for the same URL, and what was stored under such a key cannot be
// read by this version.

#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "cyclone/cache.hpp"
#include "cyclone/key.hpp"
#include "gtest/gtest.h"
#include "lib/cache/cache.h"
#include "lib/cache/headers_sidecar.h"
#include "lib/classify/alternate_id.h"
#include "lib/classify/alternate_metadata.h"
#include "lib/classify/capability_mask.h"
#include "test/test_util/cache_test_peer.h"
#include "test/test_util/temp_dir.h"

namespace pagespeed {
namespace {

constexpr std::string_view kHost = "example.com";
constexpr std::string_view kScheme = "https";
// A fixed-width option context signature (64 hex characters).
const std::string kSignature(64, 'a');

// The namespaces earlier versions composed keys in: none at all, and the
// previous "ks2:".  An entry stored under either is never served.
constexpr std::string_view kEarlierNamespaces[] = {"", "ks2:", "ks3:"};

// The key an earlier version computes: the composed string in its namespace.
cyclone::CacheKey EarlierKey(std::string_view url, std::string_view ns) {
  return cyclone::CacheKey(std::string(ns) + "https://example.com" +
                           std::string(url));
}
cyclone::CacheKey EarlierContextKey(std::string_view url, std::string_view ns) {
  return cyclone::CacheKey(std::string(ns) + "oc1:" + kSignature +
                           "@https://example.com" + std::string(url));
}
// A site (64 lowercase hex characters) and the key a version composing
// site-scoped keys without the namespace computes for it.
const std::string kSite(64, 'c');
cyclone::CacheKey EarlierSiteKey(std::string_view url, std::string_view ns) {
  return cyclone::CacheKey(std::string(ns) + "st1:" + kSite +
                           "@https://example.com" + std::string(url));
}

std::vector<std::byte> Copy(std::span<const std::byte> bytes) {
  return std::vector<std::byte>(bytes.begin(), bytes.end());
}

class KeyNamespaceTest : public ::testing::TestWithParam<std::string_view> {
 protected:
  // The earlier key forms, in the namespace under test.
  static cyclone::CacheKey EarlierKey(std::string_view url) {
    return pagespeed::EarlierKey(url, GetParam());
  }
  static cyclone::CacheKey EarlierContextKey(std::string_view url) {
    return pagespeed::EarlierContextKey(url, GetParam());
  }
  static cyclone::CacheKey EarlierSiteKey(std::string_view url) {
    return pagespeed::EarlierSiteKey(url, GetParam());
  }

  void SetUp() override {
    temp_dir_ = pagespeed::test::MakeTempDir();
    PageSpeedCacheConfig config;
    config.volume_path = temp_dir_ + "/cache.vol";
    config.volume_size = static_cast<uint64_t>(10 * 1024 * 1024);
    auto result = PageSpeedCache::Create(config);
    ASSERT_TRUE(result.has_value()) << "Failed to create cache";
    cache_ = std::move(*result);
  }

  void TearDown() override {
    cache_.reset();
    std::filesystem::remove_all(temp_dir_);
  }

  cyclone::Cache& Raw() { return PageSpeedCacheTestPeer::Cyclone(*cache_); }

  // True when anything at all is stored under `key`.
  bool AnythingUnder(const cyclone::CacheKey& key) {
    auto listed = Raw().list_alternates_sync(key);
    return listed.has_value() && !listed->empty();
  }

  // Stores `blob` under `key` at `id` the way any writer would, bypassing key
  // composition.
  void StoreRaw(const cyclone::CacheKey& key, AlternateId id,
                const std::vector<std::byte>& blob) {
    auto wh = Raw().write_alternate_sync(
        key, static_cast<cyclone::AlternateId>(id), blob.size());
    ASSERT_TRUE(wh.has_value());
    ASSERT_TRUE(wh->write_sync(std::span(blob)).has_value());
    ASSERT_TRUE(wh->close_sync().has_value());
  }

  void WriteThrough(std::expected<WriteResult, cyclone::CacheError> wh,
                    std::string_view body) {
    ASSERT_TRUE(wh.has_value());
    ASSERT_TRUE(wh->write_sync(std::as_bytes(std::span(body))).has_value());
    ASSERT_TRUE(wh->close_sync().has_value());
  }

  std::string temp_dir_;
  std::unique_ptr<PageSpeedCache> cache_;
};

AlternateId DefaultId() {
  return MaskToAlternateId(
      static_cast<uint8_t>(CapabilityMask().Encode() & 0xFF));
}

AlternateMetadata ContentMetadata() {
  AlternateMetadata meta;
  meta.full_mask = CapabilityMask().Encode();
  meta.content_type = ContentType::kImage;
  meta.origin_content_type = "image/jpeg";
  return meta;
}

TEST(KeyNamespaceKeyTest, KeysCarryTheNamespace) {
  EXPECT_EQ(PageSpeedCache::ComposeKeyPreNormalized("/a", kHost, kScheme),
            cyclone::CacheKey("ks4:https://example.com/a"));
  EXPECT_EQ(
      PageSpeedCache::ComposeKeyPreNormalized("/a", kHost, kScheme, kSignature),
      cyclone::CacheKey("ks4:oc1:" + kSignature + "@https://example.com/a"));
  EXPECT_EQ(PageSpeedCache::ComposeSiteKeyPreNormalized("/a", kHost, kScheme,
                                                        "", kSite),
            cyclone::CacheKey("ks4:st1:" + kSite + "@https://example.com/a"));
  for (std::string_view ns : kEarlierNamespaces) {
    SCOPED_TRACE(std::string(ns));
    EXPECT_NE(PageSpeedCache::ComposeKeyPreNormalized("/a", kHost, kScheme),
              EarlierKey("/a", ns));
    EXPECT_NE(PageSpeedCache::ComposeKeyPreNormalized("/a", kHost, kScheme,
                                                      kSignature),
              EarlierContextKey("/a", ns));
    EXPECT_NE(PageSpeedCache::ComposeSiteKeyPreNormalized("/a", kHost, kScheme,
                                                          "", kSite),
              EarlierSiteKey("/a", ns));
  }
}

TEST_P(KeyNamespaceTest, ContentEntriesAreInTheirOwnKeyspace) {
  WriteThrough(cache_->WriteAlternate("/a", kHost, kScheme, DefaultId(), 4,
                                      ContentMetadata()),
               "body");
  EXPECT_FALSE(AnythingUnder(EarlierKey("/a")));

  auto stored = cache_->ReadAlternate("/a", kHost, kScheme, DefaultId());
  ASSERT_TRUE(stored.has_value());
  const auto blob = Copy(stored->handle.content());
  stored->release();
  ASSERT_NO_FATAL_FAILURE(StoreRaw(EarlierKey("/b"), DefaultId(), blob));
  EXPECT_FALSE(
      cache_->ReadAlternate("/b", kHost, kScheme, DefaultId()).has_value());
  EXPECT_FALSE(cache_->ReadBestAlternate("/b", kHost, kScheme, CapabilityMask())
                   .has_value());
}

TEST_P(KeyNamespaceTest, SitedEntriesAreInTheirOwnKeyspace) {
  WriteThrough(cache_->WriteAlternate("/a", kHost, kScheme, DefaultId(), 4,
                                      ContentMetadata(), kSite),
               "body");
  EXPECT_FALSE(AnythingUnder(EarlierSiteKey("/a")));

  auto stored = cache_->ReadAlternate("/a", kHost, kScheme, DefaultId(), kSite);
  ASSERT_TRUE(stored.has_value());
  const auto blob = Copy(stored->handle.content());
  stored->release();
  ASSERT_NO_FATAL_FAILURE(StoreRaw(EarlierSiteKey("/b"), DefaultId(), blob));
  EXPECT_FALSE(cache_->ReadAlternate("/b", kHost, kScheme, DefaultId(), kSite)
                   .has_value());
  EXPECT_FALSE(cache_
                   ->ReadBestAlternate("/b", kHost, kScheme, CapabilityMask(),
                                       /*agent_request_entitled=*/false, kSite)
                   .has_value());
}

TEST_P(KeyNamespaceTest, OptionContextEntriesAreInTheirOwnKeyspace) {
  const auto key =
      PageSpeedCache::ComposeKeyPreNormalized("/a", kHost, kScheme, kSignature);
  WriteThrough(
      cache_->WriteAlternateByKey(key, DefaultId(), 4, ContentMetadata(), "/a"),
      "body");
  EXPECT_FALSE(AnythingUnder(EarlierContextKey("/a")));

  auto stored = cache_->ReadAlternateByKey(key, DefaultId());
  ASSERT_TRUE(stored.has_value());
  const auto blob = Copy(stored->handle.content());
  stored->release();
  ASSERT_NO_FATAL_FAILURE(StoreRaw(EarlierContextKey("/b"), DefaultId(), blob));
  EXPECT_FALSE(cache_
                   ->ReadAlternateByKey(PageSpeedCache::ComposeKeyPreNormalized(
                                            "/b", kHost, kScheme, kSignature),
                                        DefaultId())
                   .has_value());
}

TEST_P(KeyNamespaceTest, SentinelsAreInTheirOwnKeyspace) {
  // Every class stored through WriteSentinel, none with a metadata prefix:
  // Early Hints, the content hash, the subresource manifest, browser
  // profiles, llms.txt and its freshness record, and decline tombstones.
  for (SentinelId sentinel :
       {SentinelId::kEarlyHints, SentinelId::kContentHash,
        SentinelId::kSubresourceManifest, SentinelId::kBrowserProfile,
        SentinelId::kLlmsTxt, SentinelId::kLlmsTxtMeta,
        SentinelId::kDeclineTombstone}) {
    SCOPED_TRACE(static_cast<int>(sentinel));
    const auto id = static_cast<AlternateId>(sentinel);
    const std::string a = "/a" + std::to_string(static_cast<int>(id));
    const std::string b = "/b" + std::to_string(static_cast<int>(id));
    WriteThrough(cache_->WriteSentinel(a, kHost, kScheme, sentinel, 7),
                 "payload");
    EXPECT_FALSE(AnythingUnder(EarlierKey(a)));

    auto stored = cache_->ReadAlternate(a, kHost, kScheme, id);
    ASSERT_TRUE(stored.has_value());
    const auto blob = Copy(stored->handle.content());
    stored->release();
    ASSERT_NO_FATAL_FAILURE(StoreRaw(EarlierKey(b), id, blob));
    EXPECT_FALSE(cache_->ReadAlternate(b, kHost, kScheme, id).has_value());
  }
}

TEST_P(KeyNamespaceTest, AgentMarkdownIsInItsOwnKeyspace) {
  AlternateMetadata meta;
  meta.full_mask = static_cast<uint32_t>(SentinelId::kAgentMarkdown);
  meta.content_type = ContentType::kOther;
  meta.origin_content_type = "text/markdown";
  meta.flags = AlternateMetadata::kFlagWorkerProcessed;
  const auto id = static_cast<AlternateId>(SentinelId::kAgentMarkdown);
  WriteThrough(cache_->WriteAgentAlternate("/a", kHost, kScheme, 3, meta),
               "# a");
  EXPECT_FALSE(AnythingUnder(EarlierKey("/a")));

  auto stored = cache_->ReadAlternate("/a", kHost, kScheme, id);
  ASSERT_TRUE(stored.has_value());
  const auto blob = Copy(stored->handle.content());
  stored->release();
  ASSERT_NO_FATAL_FAILURE(StoreRaw(EarlierKey("/b"), id, blob));
  EXPECT_FALSE(cache_->ReadAlternate("/b", kHost, kScheme, id).has_value());
}

TEST_P(KeyNamespaceTest, DurableOriginalsAreInTheirOwnKeyspace) {
  AlternateMetadata meta;
  meta.full_mask = static_cast<uint32_t>(SentinelId::kOriginalContent);
  meta.content_type = ContentType::kImage;
  meta.origin_content_type = "image/jpeg";
  WriteThrough(cache_->WriteOriginalAlternate("/a", kHost, kScheme, 4, meta),
               "orig");
  EXPECT_FALSE(AnythingUnder(EarlierKey("/a")));

  auto stored = cache_->ReadOriginalAlternate("/a", kHost, kScheme);
  ASSERT_TRUE(stored.has_value());
  const auto blob = Copy(stored->handle.content());
  stored->release();
  ASSERT_NO_FATAL_FAILURE(
      StoreRaw(EarlierKey("/b"),
               static_cast<AlternateId>(SentinelId::kOriginalContent), blob));
  EXPECT_FALSE(cache_->ReadOriginalAlternate("/b", kHost, kScheme).has_value());
}

TEST_P(KeyNamespaceTest, HeadersSidecarsAreInTheirOwnKeyspace) {
  const std::vector<HeaderField> headers = {
      {"X-Content-Type-Options", "nosniff"}};
  auto written = cache_->WriteHeadersSidecar("/a", kHost, kScheme, headers);
  ASSERT_TRUE(written.has_value());
  ASSERT_TRUE(written->stored);
  EXPECT_FALSE(AnythingUnder(EarlierKey("/a")));

  auto stored = cache_->ReadHeadersSidecar("/a", kHost, kScheme);
  ASSERT_TRUE(stored.has_value());
  const auto blob = Copy(stored->handle.content());
  stored->release();
  ASSERT_NO_FATAL_FAILURE(
      StoreRaw(EarlierKey("/b"),
               static_cast<AlternateId>(SentinelId::kHeadersSidecar), blob));
  EXPECT_FALSE(cache_->ReadHeadersSidecar("/b", kHost, kScheme).has_value());
}

INSTANTIATE_TEST_SUITE_P(
    EarlierNamespaces, KeyNamespaceTest,
    ::testing::ValuesIn(kEarlierNamespaces),
    [](const ::testing::TestParamInfo<std::string_view>& info) {
      if (info.param.empty()) return std::string("none");
      return std::string(info.param.substr(0, info.param.size() - 1));
    });

}  // namespace
}  // namespace pagespeed
