// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

// The response-header sidecar as a STORED entry class: what reaches the
// volume, what comes back, and what never gets there at all.
//
// The gate's own decisions are tested next door, without a cache
// (headers_sidecar_test.cc). What is only testable here:
//
//   1. the block survives a real store/read round trip byte for byte;
//   2. ONE entry per URL — a re-store replaces, nothing accumulates, and the
//      process-local RAM tier does not keep answering with the old block;
//   3. a refused response leaves NOTHING behind, and is counted;
//   4. the entry is never selectable — a block of headers is not a
//      representation and no request can be served it by scoring;
//   5. a blob at this id that is not this class reads as a MISS.

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <ios>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

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

constexpr AlternateId kSidecarId =
    static_cast<AlternateId>(SentinelId::kHeadersSidecar);
constexpr std::string_view kUrl = "https://example.com/style.css";
constexpr std::string_view kHost = "example.com";
constexpr std::string_view kScheme = "https";

class HeadersSidecarStoreTest : public ::testing::Test {
 protected:
  void SetUp() override {
    temp_dir_ = pagespeed::test::MakeTempDir();
    cache_path_ = temp_dir_ + "/cache.vol";
    CreateCache();
  }

  void TearDown() override {
    cache_.reset();
    std::filesystem::remove_all(temp_dir_);
  }

  void CreateCache() {
    PageSpeedCacheConfig config;
    config.volume_path = cache_path_;
    config.volume_size = static_cast<uint64_t>(10 * 1024 * 1024);
    auto result = PageSpeedCache::Create(config);
    ASSERT_TRUE(result.has_value()) << "Failed to create cache";
    cache_ = std::move(*result);
  }

  // The headers read back out of the stored block, as strings.
  std::vector<std::pair<std::string, std::string>> ReadBlock() {
    std::vector<std::pair<std::string, std::string>> out;
    auto read = cache_->ReadHeadersSidecar(kUrl, kHost, kScheme);
    if (!read.has_value()) return out;
    auto parsed =
        ParseHeadersSidecar(PageSpeedCache::HeadersSidecarPayload(*read));
    if (!parsed.has_value()) return out;
    for (const HeaderField& field : *parsed) {
      out.emplace_back(std::string(field.name), std::string(field.value));
    }
    return out;
  }

  // How many alternates the URL's chain carries under this class's id.
  size_t CountSidecarAlternates() {
    auto alternates = cache_->ListAlternates(kUrl, kHost, kScheme);
    if (!alternates.has_value()) return 0;
    size_t count = 0;
    for (const auto& alt : *alternates) {
      if (static_cast<AlternateId>(alt.id) == kSidecarId) ++count;
    }
    return count;
  }

  // Plant a node under this class's id at the SUBSTRATE, bypassing the
  // cache's writers, so no unlink happens and the node is prepended.  Two of
  // these reproduce, deterministically and without a race, the chain a
  // concurrent-writer interleaving leaves behind.  (Planted through the raw
  // handle rather than WriteSentinel for exactly that reason: since #1312 the
  // sentinel writer unlinks too, so a second WriteSentinel would REPLACE the
  // first plant instead of piling onto it.)
  void PlantSidecarNode(std::string_view payload) {
    auto& raw = PageSpeedCacheTestPeer::Cyclone(*cache_);
    // The same key the writer composes for (kUrl, kHost, kScheme) — the
    // fixture's hostname is already normalized.
    auto key = PageSpeedCache::ComposeKeyPreNormalized(kUrl, kHost, kScheme);
    auto wh = raw.write_alternate_sync(
        key, static_cast<cyclone::AlternateId>(kSidecarId), payload.size());
    ASSERT_TRUE(wh.has_value());
    ASSERT_TRUE(wh->write_sync(std::as_bytes(std::span(payload))).has_value());
    ASSERT_TRUE(wh->close_sync().has_value());
    // Drop this process's RAM copy, which a raw substrate write does not.
    // The tier is write-around — reads populate it, writes do not evict — so
    // without this a later read can be answered from RAM with the node this
    // plant just superseded, and the test would be measuring the RAM tier
    // rather than the chain.
    raw.evict_from_ram_cache(key,
                             static_cast<cyclone::AlternateId>(kSidecarId));
  }

  std::string temp_dir_;
  std::string cache_path_;
  std::unique_ptr<PageSpeedCache> cache_;
};

TEST_F(HeadersSidecarStoreTest, StoredBlockRoundTripsThroughTheVolume) {
  const std::vector<HeaderField> headers = {
      {"Cache-Control", "max-age=600"},
      {"Content-Type", "text/css"},
      {"Content-Security-Policy", "default-src 'self';  img-src *"},
      {"X-Content-Type-Options", "nosniff"},
      {"Access-Control-Allow-Origin", "*"},
      {"Referrer-Policy", ""},
      {"Vary", "Accept-Encoding,  ACCEPT-ENCODING "},
  };
  auto write = cache_->WriteHeadersSidecar(kUrl, kHost, kScheme, headers);
  ASSERT_TRUE(write.has_value());
  EXPECT_EQ(write->verdict, SidecarVerdict::kSwapEligible);
  EXPECT_TRUE(write->stored);
  EXPECT_EQ(write->fields, 5u);  // the two metadata-reproduced ones are not

  const auto block = ReadBlock();
  ASSERT_EQ(block.size(), 5u);
  EXPECT_EQ(block[0].first, "Content-Security-Policy");
  EXPECT_EQ(block[0].second, "default-src 'self';  img-src *");
  EXPECT_EQ(block[1].first, "X-Content-Type-Options");
  EXPECT_EQ(block[2].first, "Access-Control-Allow-Origin");
  EXPECT_EQ(block[3].first, "Referrer-Policy");
  EXPECT_EQ(block[3].second, "");
  // F4's point: the origin's ACTUAL Vary string, spacing and case included.
  EXPECT_EQ(block[4].first, "Vary");
  EXPECT_EQ(block[4].second, "Accept-Encoding,  ACCEPT-ENCODING ");
}

TEST_F(HeadersSidecarStoreTest, OneEntryPerUrlAndAReStoreReplaces) {
  const std::vector<HeaderField> first = {
      {"Referrer-Policy", "no-referrer"},
      {"X-Content-Type-Options", "nosniff"}};
  const std::vector<HeaderField> second = {
      {"Referrer-Policy", "strict-origin-when-cross-origin"}};

  ASSERT_TRUE(
      cache_->WriteHeadersSidecar(kUrl, kHost, kScheme, first).has_value());
  ASSERT_EQ(ReadBlock().size(), 2u);

  ASSERT_TRUE(
      cache_->WriteHeadersSidecar(kUrl, kHost, kScheme, second).has_value());
  const auto block = ReadBlock();
  ASSERT_EQ(block.size(), 1u) << "the blocks accumulated instead of replacing";
  EXPECT_EQ(block[0].second, "strict-origin-when-cross-origin");

  // And exactly one entry carries the class, not one per store.
  //
  // This is the half that does not hold for free. At the pinned storage layer
  // a write to an existing id LINKS ANOTHER alternate under it — reads find
  // the newest, so a read-only check passes while the chain grows by one
  // superseded entry per store, against a budget that is not unlimited. For a
  // block re-offered on every origin response for its URL that is the whole
  // ballgame, so the writer unlinks first. Counting the chain is how that
  // stays true.
  EXPECT_EQ(CountSidecarAlternates(), 1u);

  // Ten more stores, same URL: still one.
  for (int i = 0; i < 10; ++i) {
    ASSERT_TRUE(
        cache_->WriteHeadersSidecar(kUrl, kHost, kScheme, second).has_value());
  }
  EXPECT_EQ(CountSidecarAlternates(), 1u);
  EXPECT_EQ(ReadBlock().size(), 1u);
}

TEST_F(HeadersSidecarStoreTest, ASupersededNodeIsNoLongerLeftOnTheChain) {
  // This used to reproduce the fixed point without a race: plant two nodes
  // under the id (each raw substrate write prepended), then store once. The
  // writer unlinked ONE and wrote ONE, so the chain ended at two again —
  // exactly what a concurrent-writer interleaving left behind, and exactly
  // why the depth did not come back down on its own.
  //
  // The storage layer now unlinks the superseded same-id node on the write
  // itself, so the plants no longer stack up and the chain holds one node
  // throughout.
  PlantSidecarNode("x");
  PlantSidecarNode("y");
  ASSERT_EQ(CountSidecarAlternates(), 1u);

  const std::vector<HeaderField> headers = {{"Referrer-Policy", "no-referrer"}};
  ASSERT_TRUE(
      cache_->WriteHeadersSidecar(kUrl, kHost, kScheme, headers).has_value());

  EXPECT_EQ(CountSidecarAlternates(), 1u);
  // The writer finds no superseded node to observe, so the counter that kept
  // the limitation visible in the field reads zero.
  EXPECT_EQ(cache_->HeadersSidecarSupersededObserved(), 0u);
  // And the property callers actually depend on survives it — the read still
  // serves the newest block, not the superseded one.
  const auto block = ReadBlock();
  ASSERT_EQ(block.size(), 1u);
  EXPECT_EQ(block[0].first, "Referrer-Policy");
}

TEST_F(HeadersSidecarStoreTest, ANonceBearingCspStoresNothingAtAll) {
  const std::vector<HeaderField> headers = {
      {"X-Content-Type-Options", "nosniff"},
      {"Content-Security-Policy", "script-src 'nonce-r4nd0m'"}};
  auto write = cache_->WriteHeadersSidecar(kUrl, kHost, kScheme, headers);
  ASSERT_TRUE(write.has_value());
  EXPECT_EQ(write->verdict, SidecarVerdict::kNeverOptimized);
  EXPECT_FALSE(write->stored);

  EXPECT_FALSE(cache_->AlternateExists(kUrl, kHost, kScheme, kSidecarId));
  EXPECT_FALSE(cache_->ReadHeadersSidecar(kUrl, kHost, kScheme).has_value());
  // The two counters are DISJOINT: this response is in the permanent class,
  // so it must NOT also appear in the class that later work can close.
  EXPECT_EQ(cache_->HeadersSidecarNeverOptimized(), 1u);
  EXPECT_EQ(cache_->HeadersSidecarFallThrough(), 0u);
}

TEST_F(HeadersSidecarStoreTest, TheRefusalCountersPartitionTheRefusals) {
  // One of each, offered in both orders, and neither counter sees the other's
  // response. Anything reporting "how large the closable class is" from the
  // fall-through counter has to be able to trust that.
  const std::vector<HeaderField> nonce = {
      {"Content-Security-Policy", "script-src 'nonce-a'"}};
  const std::vector<HeaderField> custom = {{"X-Custom", "1"}};
  ASSERT_TRUE(
      cache_->WriteHeadersSidecar(kUrl, kHost, kScheme, nonce).has_value());
  ASSERT_TRUE(
      cache_->WriteHeadersSidecar(kUrl, kHost, kScheme, custom).has_value());
  ASSERT_TRUE(
      cache_->WriteHeadersSidecar(kUrl, kHost, kScheme, custom).has_value());

  EXPECT_EQ(cache_->HeadersSidecarNeverOptimized(), 1u);
  EXPECT_EQ(cache_->HeadersSidecarFallThrough(), 2u);
}

TEST_F(HeadersSidecarStoreTest, ANonceWithALineTerminatorIsStillPermanent) {
  // A CSP carrying both a nonce and something that makes the field
  // unstorable: refused either way, but the nonce is the decisive fact, so it
  // belongs in the permanent bucket rather than the one later work closes.
  const std::vector<HeaderField> both = {
      {"Content-Security-Policy", "script-src 'nonce-a'\r\nX-Injected: 1"}};
  auto write = cache_->WriteHeadersSidecar(kUrl, kHost, kScheme, both);
  ASSERT_TRUE(write.has_value());
  EXPECT_EQ(write->verdict, SidecarVerdict::kNeverOptimized);
  EXPECT_FALSE(write->stored);
  EXPECT_EQ(cache_->HeadersSidecarNeverOptimized(), 1u);
  EXPECT_EQ(cache_->HeadersSidecarFallThrough(), 0u);
  EXPECT_FALSE(cache_->AlternateExists(kUrl, kHost, kScheme, kSidecarId));
}

TEST_F(HeadersSidecarStoreTest, StillLinkedIsExactlyTheNonNotFoundErrors) {
  // The unlink's failure modes need opposite handling and only two of them
  // are benign, so the classification is a predicate over the error rather
  // than a branch at the call site.
  //
  // The list below is every code this call plausibly returns, not every
  // enumerator the storage layer defines — there are 25, and enumerating the
  // ones that cannot reach this path would age badly for no gain. What makes
  // the omission safe is not the list: it is the `default:` arm, proven by
  // the unknown-code probe at the end, which is also what a future storage
  // version's new enumerator will land on.
  for (auto gone : {cyclone::CacheError::NotFound,
                    cyclone::CacheError::AlternateNotFound}) {
    EXPECT_FALSE(UnlinkLeftEntryLinked(gone))
        << "error " << static_cast<int>(gone);
  }
  // Every other code means the node may still be there — including the three
  // a middle-of-chain node, a corrupt chain and a multi-process deployment
  // reach in normal operation.
  for (auto linked :
       {cyclone::CacheError::Busy, cyclone::CacheError::ChainCorrupted,
        cyclone::CacheError::NotOwned, cyclone::CacheError::IoError,
        cyclone::CacheError::Corrupted, cyclone::CacheError::NotInitialized,
        cyclone::CacheError::InvalidKey, cyclone::CacheError::InvalidArgument,
        cyclone::CacheError::Closed, cyclone::CacheError::Timeout,
        cyclone::CacheError::TooManyAlternates,
        cyclone::CacheError::InternalError}) {
    EXPECT_TRUE(UnlinkLeftEntryLinked(linked))
        << "error " << static_cast<int>(linked);
  }
  // And the default arm is what makes that list a floor rather than a
  // snapshot: a code this build has never seen reads as still-linked.
  EXPECT_TRUE(UnlinkLeftEntryLinked(static_cast<cyclone::CacheError>(250)));
}

TEST_F(HeadersSidecarStoreTest, TheSingleWriterPathObservesNoSupersededNodes) {
  // The counter is the instrument for the limitation this class cannot close
  // from here, so its quiet case has to be pinned too: repeated stores from
  // ONE writer must never trip it. A counter that fires on the normal path
  // would be indistinguishable from noise exactly when it mattered.
  const std::vector<HeaderField> headers = {{"Referrer-Policy", "no-referrer"}};
  for (int i = 0; i < 5; ++i) {
    ASSERT_TRUE(
        cache_->WriteHeadersSidecar(kUrl, kHost, kScheme, headers).has_value());
  }
  EXPECT_EQ(CountSidecarAlternates(), 1u);
  EXPECT_EQ(cache_->HeadersSidecarSupersededObserved(), 0u);
  EXPECT_EQ(cache_->HeadersSidecarUnlinkFailed(), 0u);
}

TEST_F(HeadersSidecarStoreTest, ANonceNeverOverwritesAnExistingBlock) {
  // The dangerous shape is not the first store, it is the SECOND: a URL whose
  // origin starts sending a nonce must not end up with a stored nonce, and
  // must not end up half-updated either.
  const std::vector<HeaderField> clean = {{"Referrer-Policy", "no-referrer"}};
  ASSERT_TRUE(
      cache_->WriteHeadersSidecar(kUrl, kHost, kScheme, clean).has_value());
  ASSERT_EQ(ReadBlock().size(), 1u);

  const std::vector<HeaderField> with_nonce = {
      {"Referrer-Policy", "no-referrer"},
      {"Content-Security-Policy", "script-src 'nonce-x'"}};
  auto write = cache_->WriteHeadersSidecar(kUrl, kHost, kScheme, with_nonce);
  ASSERT_TRUE(write.has_value());
  EXPECT_FALSE(write->stored);

  const auto block = ReadBlock();
  ASSERT_EQ(block.size(), 1u);
  EXPECT_EQ(block[0].first, "Referrer-Policy");
  for (const auto& [name, value] : block) {
    EXPECT_EQ(value.find("nonce-"), std::string::npos);
  }
}

TEST_F(HeadersSidecarStoreTest, AFallThroughResponseStoresNothingAndIsCounted) {
  const std::vector<HeaderField> headers = {{"Cache-Control", "max-age=600"},
                                            {"Set-Cookie", "session=abc"}};
  auto write = cache_->WriteHeadersSidecar(kUrl, kHost, kScheme, headers);
  ASSERT_TRUE(write.has_value());
  EXPECT_EQ(write->verdict, SidecarVerdict::kFallThrough);
  EXPECT_FALSE(write->stored);
  EXPECT_FALSE(cache_->AlternateExists(kUrl, kHost, kScheme, kSidecarId));
  EXPECT_EQ(cache_->HeadersSidecarFallThrough(), 1u);
  // Not the never-optimized class: this one is a gap, not a permanent one.
  EXPECT_EQ(cache_->HeadersSidecarNeverOptimized(), 0u);
}

TEST_F(HeadersSidecarStoreTest, NothingToCarryIsNotAFallThrough) {
  // Every header reproduced by the entry metadata: swap-eligible, no entry,
  // and NOT counted as fall-through — counting it would make the instrument
  // report the ordinary case as the exposed class.
  const std::vector<HeaderField> headers = {
      {"Cache-Control", "max-age=600"},
      {"Content-Type", "text/css"},
      {"Date", "Mon, 10 Aug 2026 00:00:00 GMT"}};
  auto write = cache_->WriteHeadersSidecar(kUrl, kHost, kScheme, headers);
  ASSERT_TRUE(write.has_value());
  EXPECT_EQ(write->verdict, SidecarVerdict::kSwapEligible);
  EXPECT_FALSE(write->stored);
  EXPECT_FALSE(cache_->AlternateExists(kUrl, kHost, kScheme, kSidecarId));
  EXPECT_EQ(cache_->HeadersSidecarFallThrough(), 0u);
}

TEST_F(HeadersSidecarStoreTest, TheBlockIsNeverSelectable) {
  const std::vector<HeaderField> headers = {{"Referrer-Policy", "no-referrer"}};
  ASSERT_TRUE(
      cache_->WriteHeadersSidecar(kUrl, kHost, kScheme, headers).has_value());

  // With ONLY the sidecar present, EVERY client capability misses: a block of
  // headers is not a representation and must never be handed to a client as
  // one. The whole axis product is walked, not a sample, because "selectable"
  // is a property of the scoring function and one mask proves nothing.
  using CM = CapabilityMask;
  for (auto format : {CM::ImageFormat::kOriginal, CM::ImageFormat::kWebP,
                      CM::ImageFormat::kAvif, CM::ImageFormat::kSvg}) {
    for (auto viewport : {CM::Viewport::kMobile, CM::Viewport::kTablet,
                          CM::Viewport::kDesktop}) {
      for (auto density : {CM::PixelDensity::k1x, CM::PixelDensity::k2xPlus}) {
        for (auto save_data : {CM::SaveData::kOff, CM::SaveData::kOn}) {
          for (auto encoding :
               {CM::TransferEncoding::kIdentity, CM::TransferEncoding::kGzip,
                CM::TransferEncoding::kBrotli}) {
            CapabilityMask mask(format, viewport, density, save_data, encoding);
            auto best = cache_->ReadBestAlternate(kUrl, kHost, kScheme, mask);
            EXPECT_FALSE(best.has_value())
                << "selected at mask 0x" << std::hex << mask.Encode();
          }
        }
      }
    }
  }

  // And with a real variant present, that variant is what selection returns.
  AlternateMetadata meta;
  CapabilityMask content_mask;
  meta.full_mask = content_mask.Encode();
  meta.content_type = ContentType::kCss;
  const std::string body = "body{}";
  auto wh = cache_->WriteAlternate(
      kUrl, kHost, kScheme, static_cast<AlternateId>(meta.full_mask & 0xFF),
      body.size(), meta);
  ASSERT_TRUE(wh.has_value());
  ASSERT_TRUE(wh->write_sync(std::as_bytes(std::span(body))).has_value());
  ASSERT_TRUE(wh->close_sync().has_value());

  auto best = cache_->ReadBestAlternate(kUrl, kHost, kScheme, content_mask);
  ASSERT_TRUE(best.has_value());
  auto content = best->content();
  EXPECT_EQ(std::string_view(reinterpret_cast<const char*>(content.data()),
                             content.size()),
            body);
}

TEST_F(HeadersSidecarStoreTest, AForeignBlobAtThisIdReadsAsAMiss) {
  // A peer build, or an in-tree path that did not go through the writer, can
  // leave bytes at this id. They are not this class: the read stops rather
  // than handing back somebody else's payload as a header block.
  const std::string foreign = "not a sidecar payload";
  auto wh = cache_->WriteSentinel(kUrl, kHost, kScheme,
                                  SentinelId::kHeadersSidecar, foreign.size());
  ASSERT_TRUE(wh.has_value());
  ASSERT_TRUE(wh->write_sync(std::as_bytes(std::span(foreign))).has_value());
  ASSERT_TRUE(wh->close_sync().has_value());

  EXPECT_TRUE(cache_->AlternateExists(kUrl, kHost, kScheme, kSidecarId));
  EXPECT_FALSE(cache_->ReadHeadersSidecar(kUrl, kHost, kScheme).has_value());
}

TEST_F(HeadersSidecarStoreTest, APayloadFromAFutureVersionReadsAsAMiss) {
  std::string payload =
      ClassifyForHeadersSidecar(
          std::vector<HeaderField>{{"Referrer-Policy", "no-referrer"}})
          .payload;
  ASSERT_FALSE(payload.empty());
  payload[0] = static_cast<char>(kHeadersSidecarPairedFormatVersion + 1);
  auto wh = cache_->WriteSentinel(kUrl, kHost, kScheme,
                                  SentinelId::kHeadersSidecar, payload.size());
  ASSERT_TRUE(wh.has_value());
  ASSERT_TRUE(wh->write_sync(std::as_bytes(std::span(payload))).has_value());
  ASSERT_TRUE(wh->close_sync().has_value());

  EXPECT_FALSE(cache_->ReadHeadersSidecar(kUrl, kHost, kScheme).has_value());
}

TEST_F(HeadersSidecarStoreTest, AMissingBlockIsAMissNotAnError) {
  EXPECT_FALSE(cache_->ReadHeadersSidecar(kUrl, kHost, kScheme).has_value());
  EXPECT_EQ(cache_->HeadersSidecarFallThrough(), 0u);
}

TEST_F(HeadersSidecarStoreTest, ThePurgeDropsTheBlockWithTheVariants) {
  // Pinned as behaviour rather than asserted as policy: the block belongs to
  // the origin response that a refresh has just replaced, and the purge's
  // default is to drop everything it is not asked to keep. Whether asking to
  // keep it should be REFUSED outright — as it is for the durable original —
  // is not decided here.
  //
  // THE DECISION MUST NOT OUTLIVE THE SERVE LANE. It is safe to leave open
  // only while nothing reads the block: a caller CAN preserve this id across
  // an origin refresh that dropped every variant, and the moment a serve path
  // exists that reads the block, that is the previous origin's headers beside
  // the next origin's bytes.
  const std::vector<HeaderField> headers = {{"Referrer-Policy", "no-referrer"}};
  ASSERT_TRUE(
      cache_->WriteHeadersSidecar(kUrl, kHost, kScheme, headers).has_value());
  ASSERT_TRUE(cache_->AlternateExists(kUrl, kHost, kScheme, kSidecarId));

  const AlternateId preserve[] = {static_cast<AlternateId>(0x08)};
  auto removed = cache_->RemoveAlternatesExcept(kUrl, kHost, kScheme, preserve);
  ASSERT_TRUE(removed.has_value());
  EXPECT_FALSE(cache_->AlternateExists(kUrl, kHost, kScheme, kSidecarId));
}

}  // namespace
}  // namespace pagespeed
