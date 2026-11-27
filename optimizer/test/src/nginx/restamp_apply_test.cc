// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

// Cache-effect tests for the 304-restamp dispatch (issue #1016).
//
// The kernel tests next door assert which VERDICT a set of headers yields.
// These assert what the verdict DOES to a real cache — that kEvict actually
// removes the entry and every derived alternate, and that kSkipRestamp
// actually leaves the stored bytes and timestamp alone.  Those two effects
// previously had no test at all, and two defects (a dropped no-cache and a
// contradictory stale-if-error) survived a full review cycle in exactly
// this untested layer, so it is worth a real cache rather than a fake.
//
// ApplyRestampVerdict is a template over the cache type purely so this can
// instantiate it; production instantiates the same code with the same
// PageSpeedCache, no indirection.

#include <filesystem>
#include <span>
#include <string>
#include <string_view>
#include <system_error>

#include "gtest/gtest.h"
#include "lib/cache/cache.h"
#include "lib/classify/alternate_id.h"
#include "lib/classify/alternate_metadata.h"
#include "src/nginx/revalidation_storability.h"
#include "test/test_util/temp_dir.h"

namespace pagespeed {
namespace {

constexpr std::string_view kUrl = "/index.html";
constexpr std::string_view kBody = "<html>original</html>";

class RestampApplyTest : public ::testing::Test {
 protected:
  void SetUp() override {
    cache_dir_ = pagespeed::test::MakeTempDir();
    PageSpeedCacheConfig config;
    config.volume_path = cache_dir_ + "/cache.vol";
    config.volume_size = static_cast<uint64_t>(10 * 1024 * 1024);
    auto result = PageSpeedCache::Create(config);
    ASSERT_TRUE(result.has_value()) << "failed to open cache";
    cache_ = std::move(*result);
  }

  void TearDown() override {
    cache_.reset();
    if (!cache_dir_.empty()) {
      std::error_code ec;
      std::filesystem::remove_all(cache_dir_, ec);
    }
  }

  static AlternateId Id(uint8_t mask) { return MaskToAlternateId(mask); }

  // Seed an entry the way the record-on-miss path would.
  void Seed(uint8_t mask, std::string_view body, uint32_t inserted_at) {
    AlternateMetadata meta;
    meta.full_mask = mask;
    meta.content_type = ContentType::kHtml;
    meta.cache_inserted_at = inserted_at;
    auto wh = cache_->WriteAlternate(kUrl, "", "", Id(mask), body.size(), meta);
    ASSERT_TRUE(wh.has_value());
    ASSERT_TRUE(wh->write_sync(std::as_bytes(std::span(body))).has_value());
    ASSERT_TRUE(wh->close_sync().has_value());
  }

  bool Exists(uint8_t mask) {
    return cache_->AlternateExists(kUrl, "", "", Id(mask));
  }

  AlternateMetadata RestampedMeta(uint8_t mask, uint32_t inserted_at) {
    AlternateMetadata meta;
    meta.full_mask = mask;
    meta.content_type = ContentType::kHtml;
    meta.cache_inserted_at = inserted_at;
    return meta;
  }

  RestampAction Apply(RestampVerdict verdict, uint8_t mask,
                      const AlternateMetadata& meta, std::string_view body) {
    return ApplyRestampVerdict(*cache_, verdict, kUrl, "", "", Id(mask), meta,
                               std::as_bytes(std::span(body)));
  }

  std::string cache_dir_;
  std::unique_ptr<PageSpeedCache> cache_;
};

// --- kEvict actually removes ----------------------------------------------

TEST_F(RestampApplyTest, EvictRemovesTheStoredEntry) {
  Seed(0x01, kBody, 1000);
  ASSERT_TRUE(Exists(0x01));

  EXPECT_EQ(RestampAction::kEvicted, Apply(RestampVerdict::kEvict, 0x01,
                                           RestampedMeta(0x01, 2000), kBody));

  EXPECT_FALSE(Exists(0x01))
      << "a response the origin marked non-storable is still cached";
}

TEST_F(RestampApplyTest, EvictRemovesEveryDerivedAlternate) {
  // The worker derives variants (WebP/AVIF/compressed) from the same origin
  // body, so the origin's new directive covers all of them.  Evicting only
  // the revalidated alternate would leave the others served.
  Seed(0x01, kBody, 1000);
  Seed(0x02, "<html>webp variant</html>", 1000);
  Seed(0x04, "<html>avif variant</html>", 1000);
  ASSERT_TRUE(Exists(0x01));
  ASSERT_TRUE(Exists(0x02));
  ASSERT_TRUE(Exists(0x04));

  EXPECT_EQ(RestampAction::kEvicted, Apply(RestampVerdict::kEvict, 0x01,
                                           RestampedMeta(0x01, 2000), kBody));

  EXPECT_FALSE(Exists(0x01));
  EXPECT_FALSE(Exists(0x02)) << "derived variant survived the eviction";
  EXPECT_FALSE(Exists(0x04)) << "derived variant survived the eviction";
}

// --- kSkipRestamp actually writes nothing ---------------------------------

TEST_F(RestampApplyTest, SkipLeavesEntryAndTimestampUntouched) {
  Seed(0x01, kBody, 1000);

  EXPECT_EQ(RestampAction::kSkipped, Apply(RestampVerdict::kSkipRestamp, 0x01,
                                           RestampedMeta(0x01, 999999), kBody));

  ASSERT_TRUE(Exists(0x01)) << "skip must not destroy a lawfully stored entry";
  auto read = cache_->ReadAlternate(kUrl, "", "", Id(0x01));
  ASSERT_TRUE(read.has_value());
  // The whole point of the skip: the freshness stamp is NOT extended, so
  // the entry revalidates again next request instead of living on.
  EXPECT_EQ(1000u, read->metadata.cache_inserted_at)
      << "skip extended the entry's life anyway";
}

// --- kRestamp still does what it always did -------------------------------

TEST_F(RestampApplyTest, RestampWritesBackWithTheNewTimestamp) {
  Seed(0x01, kBody, 1000);

  EXPECT_EQ(
      RestampAction::kWrittenBack,
      Apply(RestampVerdict::kRestamp, 0x01, RestampedMeta(0x01, 2000), kBody));

  auto read = cache_->ReadAlternate(kUrl, "", "", Id(0x01));
  ASSERT_TRUE(read.has_value());
  EXPECT_EQ(2000u, read->metadata.cache_inserted_at);
}

TEST_F(RestampApplyTest, RestampKeepsTheContentBinding) {
  // The write-back carries the stashed entry's metadata with only the
  // freshness fields refreshed, so the origin_html_hash stamp the entry was
  // stored with is still on it afterwards.
  AlternateMetadata stored = RestampedMeta(0x01, 1000);
  for (size_t i = 0; i < stored.origin_html_hash.size(); ++i) {
    stored.origin_html_hash[i] = static_cast<std::byte>(0xA0 + i);
  }
  auto wh =
      cache_->WriteAlternate(kUrl, "", "", Id(0x01), kBody.size(), stored);
  ASSERT_TRUE(wh.has_value());
  ASSERT_TRUE(wh->write_sync(std::as_bytes(std::span(kBody))).has_value());
  ASSERT_TRUE(wh->close_sync().has_value());

  auto stale = cache_->ReadAlternate(kUrl, "", "", Id(0x01));
  ASSERT_TRUE(stale.has_value());
  AlternateMetadata updated = stale->metadata;
  updated.cache_inserted_at = 2000;
  stale->release();
  EXPECT_EQ(RestampAction::kWrittenBack,
            Apply(RestampVerdict::kRestamp, 0x01, updated, kBody));

  auto read = cache_->ReadAlternate(kUrl, "", "", Id(0x01));
  ASSERT_TRUE(read.has_value());
  EXPECT_EQ(2000u, read->metadata.cache_inserted_at);
  EXPECT_EQ(read->metadata.origin_html_hash, stored.origin_html_hash);
}

TEST_F(RestampApplyTest, RestampHonoursThePurgeFence) {
  // Entry purged mid-revalidation: the write-back must not resurrect it.
  EXPECT_EQ(RestampAction::kEntryGone, Apply(RestampVerdict::kRestamp, 0x01,
                                             RestampedMeta(0x01, 2000), kBody));
  EXPECT_FALSE(Exists(0x01)) << "write-back resurrected a purged entry";
}

TEST_F(RestampApplyTest, RestampEvictsStaleRamCopy) {
  // Issue #1126 (same class as #1125): Cyclone's RAM tier is write-around —
  // reads populate it, writes do NOT evict.  The stale read that triggered
  // this revalidation already populated this process's RAM tier (first put:
  // CLFUS seen-mark, not admitted; a racing second read admits), and the
  // restamp write-back — same bytes, new cache_inserted_at — does not
  // displace it.  Without the post-commit eviction in ApplyRestampVerdict
  // every later same-process read computes freshness from the PRE-restamp
  // stamp and the entry revalidates to origin on every request, forever.
  Seed(0x01, kBody, 1000);

  // Two reads: the first marks the key seen, the second admits the
  // pre-restamp document into this process's RAM tier.
  for (int i = 0; i < 2; ++i) {
    auto poison = cache_->ReadAlternate(kUrl, "", "", Id(0x01));
    ASSERT_TRUE(poison.has_value());
    ASSERT_EQ(1000u, poison->metadata.cache_inserted_at);
  }

  EXPECT_EQ(
      RestampAction::kWrittenBack,
      Apply(RestampVerdict::kRestamp, 0x01, RestampedMeta(0x01, 2000), kBody));

  auto read = cache_->ReadAlternate(kUrl, "", "", Id(0x01));
  ASSERT_TRUE(read.has_value());
  EXPECT_EQ(2000u, read->metadata.cache_inserted_at)
      << "RAM tier served the pre-restamp document — the write-back must "
         "evict the stale RAM copy (issue #1126)";
}

TEST_F(RestampApplyTest, EvictOnAnAlreadyGoneEntryIsNotAnError) {
  // Racing purge + eviction: removing nothing is the desired end state.
  EXPECT_EQ(RestampAction::kEvicted, Apply(RestampVerdict::kEvict, 0x01,
                                           RestampedMeta(0x01, 2000), kBody));
  EXPECT_FALSE(Exists(0x01));
}

}  // namespace
}  // namespace pagespeed
