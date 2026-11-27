// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

// The durable original-content entry class: the bytes the origin sent, kept
// under their own alternate id so that optimized variants can be rebuilt
// without going back to the origin.
//
// What these tests are actually protecting, in the order the class's
// properties depend on each other:
//
//   1. it is never selectable — no request can be served it by scoring;
//   2. it is readable by exact id, which is how it is served deliberately;
//   3. it dies with its origin, in the same removal event as the variants;
//   4. it is bounded — an original above the cap is not stored, and the
//      refusal happens while streaming, never after buffering the whole
//      thing;
//   5. its lifetime comes from the origin's own headers, not from a TTL of
//      its own;
//   6. every write path into the shared volume is alternate-scoped, because
//      the one that is not destroys the whole chain.

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "cyclone/cache.hpp"
#include "cyclone/key.hpp"
#include "gtest/gtest.h"
#include "lib/cache/cache.h"
#include "lib/cache/freshness.h"
#include "lib/cache/headers_sidecar.h"
#include "lib/classify/alternate_id.h"
#include "lib/classify/alternate_metadata.h"
#include "lib/classify/capability_mask.h"
#include "test/test_util/cache_test_peer.h"
#include "test/test_util/temp_dir.h"

namespace pagespeed {
namespace {

constexpr AlternateId kOriginalId =
    static_cast<AlternateId>(SentinelId::kOriginalContent);

// Metadata as a front end that just fetched from the origin would stamp it:
// the class it is stored under, plus the origin state that governs how long
// it may be used.
AlternateMetadata OriginalMetadata(uint32_t inserted_at = 1'700'000'000,
                                   uint32_t max_age = 600) {
  AlternateMetadata meta;
  meta.full_mask = static_cast<uint32_t>(SentinelId::kOriginalContent);
  meta.content_type = ContentType::kImage;
  meta.origin_content_type = "image/jpeg";
  meta.cache_inserted_at = inserted_at;
  meta.origin_max_age = max_age;
  meta.origin_cc_flags = AlternateMetadata::kCCOriginHeaderPresent |
                         AlternateMetadata::kCCOriginPublic;
  meta.origin_cache_control = "public, max-age=600, stale-while-revalidate=30";
  meta.origin_etag = "\"abc123\"";
  meta.origin_last_modified = 1'699'999'000;
  return meta;
}

class DurableOriginalsTest : public ::testing::Test {
 protected:
  void SetUp() override {
    temp_dir_ = pagespeed::test::MakeTempDir();
    cache_path_ = temp_dir_ + "/cache.vol";
  }

  void TearDown() override {
    cache_.reset();
    std::filesystem::remove_all(temp_dir_);
  }

  // cap == 0 leaves the config field zero-initialised, which is how the
  // shipped default is selected.
  void CreateCache(uint64_t cap = 0) {
    PageSpeedCacheConfig config;
    config.volume_path = cache_path_;
    config.volume_size = static_cast<uint64_t>(10 * 1024 * 1024);
    config.max_original_content_length = cap;
    auto result = PageSpeedCache::Create(config);
    ASSERT_TRUE(result.has_value()) << "Failed to create cache";
    cache_ = std::move(*result);
  }

  // Store a durable original; returns whether the whole write succeeded.
  bool WriteOriginalTo(std::string_view url, std::string_view body,
                       const AlternateMetadata& meta) {
    auto wh = cache_->WriteOriginalAlternate(url, "example.com", "https",
                                             body.size(), meta);
    if (!wh.has_value()) return false;
    auto written = wh->write_sync(std::as_bytes(std::span(body)));
    if (!written.has_value()) return false;
    return wh->close_sync().has_value();
  }

  // Store an ordinary content variant at the alternate id its mask implies.
  void WriteVariant(std::string_view url, const CapabilityMask& mask,
                    std::string_view body, uint8_t flags = 0) {
    AlternateMetadata meta;
    meta.full_mask = mask.Encode();
    meta.flags = flags;
    meta.content_type = ContentType::kImage;
    meta.origin_content_type = "image/jpeg";
    auto id = MaskToAlternateId(static_cast<uint8_t>(mask.Encode() & 0xFF));
    auto wh = cache_->WriteAlternate(url, "example.com", "https", id,
                                     body.size(), meta);
    ASSERT_TRUE(wh.has_value());
    ASSERT_TRUE(wh->write_sync(std::as_bytes(std::span(body))).has_value());
    ASSERT_TRUE(wh->close_sync().has_value());
  }

  // Re-points the chain head's successor field at the head itself, turning the
  // key's alternate chain into a cycle.  The cache must be CLOSED when this
  // runs (the volume is a persistent file; a second write handle on a mapped
  // file is not portable) and is re-opened by the caller afterwards.
  //
  // The successor is a stripe-relative offset in the document's fixed header,
  // outside the checksum, so a plain file write is what the cache
  // reads next.  The two constants are the storage layer's on-disk document
  // format; they are verified against the file before anything is written, so
  // a pin that moves them fails here rather than silently patching nothing.
  void SelfLinkChainHead(uint64_t head, uint64_t tail) {
    constexpr uint32_t kDocumentMagic = 0x5F129B14;
    constexpr std::streamoff kNextAlternateOffsetPos = 112;
    // Format v9: the link packs the successor's stripe-relative offset in the
    // low 48 bits and the successor's fill serial in the top 16; the document's
    // own fill serial sits at byte 88.
    constexpr std::streamoff kFillSerialPos = 88;
    constexpr unsigned kLinkSerialShift = 48;
    constexpr uint64_t kLinkOffsetMask = (uint64_t{1} << kLinkSerialShift) - 1;
    auto magic_at = [](std::fstream& f, uint64_t offset) {
      uint32_t magic = 0;
      f.clear();
      f.seekg(static_cast<std::streamoff>(offset));
      f.read(reinterpret_cast<char*>(&magic), sizeof(magic));
      return f.good() && magic == kDocumentMagic;
    };
    // The file is found by the document magic at both offsets: the storage
    // layer fingerprints the volume's file name and may keep a small-document
    // sibling, and disk_offset is relative to whichever file holds the entry.
    std::filesystem::path volume_file;
    std::string candidates;
    for (const auto& entry : std::filesystem::directory_iterator(temp_dir_)) {
      if (!entry.is_regular_file()) continue;
      candidates += " " + entry.path().filename().string();
      std::fstream probe(entry.path(), std::ios::in | std::ios::binary);
      if (probe.is_open() && magic_at(probe, head) && magic_at(probe, tail)) {
        volume_file = entry.path();
      }
    }
    ASSERT_FALSE(volume_file.empty())
        << "no volume file holds both documents; files:" << candidates;
    std::fstream volume(volume_file,
                        std::ios::in | std::ios::out | std::ios::binary);
    ASSERT_TRUE(volume.is_open());
    uint64_t link = 0;
    volume.seekg(static_cast<std::streamoff>(head) + kNextAlternateOffsetPos);
    volume.read(reinterpret_cast<char*>(&link), sizeof(link));
    ASSERT_TRUE(volume.good());
    const uint64_t tail_relative = link & kLinkOffsetMask;
    // The link's top 16 bits must be the successor's own fill serial; this
    // confirms the v9 layout assumed above before anything is written.
    uint32_t tail_fill_serial = 0;
    volume.seekg(static_cast<std::streamoff>(tail) + kFillSerialPos);
    volume.read(reinterpret_cast<char*>(&tail_fill_serial),
                sizeof(tail_fill_serial));
    ASSERT_TRUE(volume.good());
    ASSERT_EQ(link >> kLinkSerialShift,
              static_cast<uint64_t>(static_cast<uint16_t>(tail_fill_serial)))
        << "the link's serial must be the tail's fill serial";
    uint32_t head_fill_serial = 0;
    volume.seekg(static_cast<std::streamoff>(head) + kFillSerialPos);
    volume.read(reinterpret_cast<char*>(&head_fill_serial),
                sizeof(head_fill_serial));
    ASSERT_TRUE(volume.good());
    ASSERT_NE(tail_relative, 0u);
    ASSERT_LE(tail_relative, tail) << "the successor field is stripe-relative";
    const uint64_t stripe_base = tail - tail_relative;
    ASSERT_LT(stripe_base, head);
    const uint64_t head_relative =
        (head - stripe_base) |
        (uint64_t{static_cast<uint16_t>(head_fill_serial)} << kLinkSerialShift);
    volume.clear();
    volume.seekp(static_cast<std::streamoff>(head) + kNextAlternateOffsetPos);
    volume.write(reinterpret_cast<const char*>(&head_relative),
                 sizeof(head_relative));
    volume.flush();
    ASSERT_TRUE(volume.good());
  }

  std::string BodyOf(const ReadResult& r) {
    auto c = r.content();
    return std::string(reinterpret_cast<const char*>(c.data()), c.size());
  }

  cyclone::CacheKey KeyFor(std::string_view url) {
    return PageSpeedCache::ComposeKeyPreNormalized(url, "example.com", "https");
  }

  // Nodes on the URL's chain carrying the original's id, counted through the
  // SUBSTRATE rather than through a read.  This is the distinction the whole
  // re-record question turns on: an exact-id read returns the newest node and
  // says nothing about how many are behind it, so a read-back assertion stays
  // green while the chain grows without bound.
  size_t OriginalChainNodes(std::string_view url) {
    auto alts = PageSpeedCacheTestPeer::Cyclone(*cache_).list_alternates_sync(
        KeyFor(url));
    if (!alts.has_value()) return 0;
    size_t nodes = 0;
    for (const auto& alt : *alts) {
      if (static_cast<AlternateId>(alt.id) == kOriginalId) ++nodes;
    }
    return nodes;
  }

  // Plant an original at the substrate WITHOUT going through the cache's
  // writer, so no unlink happens and the node is prepended.  Two of these
  // reproduce, deterministically and without a race, the chain a concurrent
  // interleaving leaves behind.
  void PlantOriginalNode(std::string_view url, std::string_view body) {
    auto meta = OriginalMetadata();
    auto serialized = meta.Serialize();
    auto raw = PageSpeedCacheTestPeer::Cyclone(*cache_).write_alternate_sync(
        KeyFor(url), static_cast<cyclone::AlternateId>(kOriginalId),
        serialized.size() + body.size());
    ASSERT_TRUE(raw.has_value());
    raw->set_header(serialized);
    ASSERT_TRUE(raw->write_sync(serialized).has_value());
    ASSERT_TRUE(raw->write_sync(std::as_bytes(std::span(body))).has_value());
    ASSERT_TRUE(raw->close_sync().has_value());
    // Drop this process's RAM copy, which a raw substrate write does not.
    // The tier is write-around — reads populate it, writes do not evict — so
    // without this the next read can be answered from RAM with the node this
    // plant just superseded, and the test would be measuring the RAM tier
    // rather than the chain. (Discovered the hard way: the reads in the
    // growth demonstration below started returning a stale body from the
    // third plant onward. It is the same hazard the cache's own writers
    // handle by evicting, which is exactly why they do.)
    PageSpeedCacheTestPeer::Cyclone(*cache_).evict_from_ram_cache(
        KeyFor(url), static_cast<cyclone::AlternateId>(kOriginalId));
  }

  std::string temp_dir_;
  std::string cache_path_;
  std::unique_ptr<PageSpeedCache> cache_;
};

// ---------------------------------------------------------------------------
// The class round-trips, carrying the origin state it is governed by.
// ---------------------------------------------------------------------------

TEST_F(DurableOriginalsTest, RoundTripsThroughAnExactIdRead) {
  CreateCache();
  auto meta = OriginalMetadata();
  ASSERT_TRUE(WriteOriginalTo("/hero.jpg", "ORIGINALBYTES", meta));

  auto read =
      cache_->ReadOriginalAlternate("/hero.jpg", "example.com", "https");
  ASSERT_TRUE(read.has_value());
  EXPECT_EQ(BodyOf(*read), "ORIGINALBYTES");
  // The entry self-describes as its own class.
  EXPECT_EQ(read->metadata.full_mask & 0xFF, kOriginalId);
}

TEST_F(DurableOriginalsTest, EntryMetadataRoundTripsIncludingTheV8Fields) {
  CreateCache();
  auto meta = OriginalMetadata();
  meta.origin_epoch = 42;  // carried, not consumed
  ASSERT_TRUE(WriteOriginalTo("/hero.jpg", "ORIGINALBYTES", meta));

  auto read =
      cache_->ReadOriginalAlternate("/hero.jpg", "example.com", "https");
  ASSERT_TRUE(read.has_value());
  const auto& m = read->metadata;
  EXPECT_EQ(m.version, AlternateMetadata::kCurrentVersion);
  // The raw Cache-Control string is the reason a relaying consumer does not
  // have to re-synthesize the origin's directives: it must come back
  // byte-for-byte, extension directives and all.
  EXPECT_EQ(m.origin_cache_control,
            "public, max-age=600, stale-while-revalidate=30");
  EXPECT_EQ(m.origin_max_age, 600u);
  EXPECT_EQ(m.origin_s_maxage, 0u);
  EXPECT_EQ(m.origin_cc_flags, meta.origin_cc_flags);
  EXPECT_EQ(m.origin_etag, "\"abc123\"");
  EXPECT_EQ(m.origin_last_modified, 1'699'999'000u);
  EXPECT_EQ(m.cache_inserted_at, 1'700'000'000u);
  EXPECT_EQ(m.origin_epoch, 42u);
  EXPECT_EQ(m.origin_content_type, "image/jpeg");
}

// The `flags` byte round-trips, and specifically the Vary-Accept marker.
//
// Sibling of the field sweep above, which covers origin_cc_flags but never
// `flags` — a different byte with a different meaning.  It matters here
// because this exact read is what the worker's Accept-negotiation refusal
// consults on a URL whose only entry is a durable original: if the marker did
// not survive the round trip, the refusal would read clear and derive a
// variant family for an origin that negotiates on Accept.  Asserted rather
// than inferred from "the metadata round-trips generally".
TEST_F(DurableOriginalsTest, TheFlagsByteRoundTripsThroughTheExactIdRead) {
  CreateCache();

  auto marked = OriginalMetadata();
  marked.flags = AlternateMetadata::kFlagOriginVariesAccept;
  ASSERT_TRUE(WriteOriginalTo("/negotiated.jpg", "ORIGINAL", marked));

  auto read =
      cache_->ReadOriginalAlternate("/negotiated.jpg", "example.com", "https");
  ASSERT_TRUE(read.has_value());
  EXPECT_NE(read->metadata.flags & AlternateMetadata::kFlagOriginVariesAccept,
            0)
      << "the marker did not survive the exact-id read the refusal consults";

  // Carried, not synthesised: an unmarked original stays unmarked, so the
  // refusal cannot fire on every stored original.
  ASSERT_TRUE(WriteOriginalTo("/plain.jpg", "ORIGINAL", OriginalMetadata()));
  auto plain =
      cache_->ReadOriginalAlternate("/plain.jpg", "example.com", "https");
  ASSERT_TRUE(plain.has_value());
  EXPECT_EQ(plain->metadata.flags & AlternateMetadata::kFlagOriginVariesAccept,
            0);
}

TEST_F(DurableOriginalsTest, RefusesMetadataThatDoesNotSelfDescribe) {
  CreateCache();
  auto meta = OriginalMetadata();
  meta.full_mask = CapabilityMask().Encode();  // a variant's mask, not 0x0C

  auto wh = cache_->WriteOriginalAlternate("/hero.jpg", "example.com", "https",
                                           4, meta);
  ASSERT_FALSE(wh.has_value());
  EXPECT_EQ(wh.error(), cyclone::CacheError::InvalidArgument);
  // And nothing was stored under the id.
  EXPECT_FALSE(cache_->AlternateExists("/hero.jpg", "example.com", "https",
                                       kOriginalId));
}

TEST_F(DurableOriginalsTest, ReadRefusesABlobWithoutAMetadataPrefix) {
  CreateCache();
  // WriteSentinel writes NO prefix.  A blob like that at this id is not this
  // class — a peer speaking a format this build does not have, or a slot
  // somebody else wrote — and serving its bytes would hand the caller a
  // metadata header as if it were the origin's body.
  auto wh = cache_->WriteSentinel("/hero.jpg", "example.com", "https",
                                  SentinelId::kOriginalContent, 4);
  ASSERT_TRUE(wh.has_value());
  ASSERT_TRUE(wh->write_sync(std::as_bytes(std::span("junk", 4))).has_value());
  ASSERT_TRUE(wh->close_sync().has_value());

  EXPECT_TRUE(cache_->AlternateExists("/hero.jpg", "example.com", "https",
                                      kOriginalId));
  auto read =
      cache_->ReadOriginalAlternate("/hero.jpg", "example.com", "https");
  EXPECT_FALSE(read.has_value());
}

TEST_F(DurableOriginalsTest, ReadRefusesAPrefixedBlobThatIsNotThisClass) {
  // The case the write-side self-description check CANNOT cover: an entry that
  // arrives at this id without going through WriteOriginalAlternate — a peer
  // build, a future in-tree path, a raw alternate write.  Its prefix parses
  // fine, and every field behind it (content type, flags, the origin state
  // that decides how long it may be used) belongs to a different class.  The
  // reader has the one bit of evidence that settles it, so it uses it.
  CreateCache();

  AlternateMetadata foreign;
  foreign.full_mask = CapabilityMask().Encode();  // a variant, not this class
  foreign.content_type = ContentType::kImage;
  foreign.origin_content_type = "image/jpeg";
  foreign.cache_inserted_at = 1'700'000'000;
  foreign.origin_max_age = 600;
  auto serialized = foreign.Serialize();

  const std::string body = "NOT-AN-ORIGINAL";
  auto key = KeyFor("/imposter.jpg");
  {
    auto wh = PageSpeedCacheTestPeer::Cyclone(*cache_).write_alternate_sync(
        key, static_cast<cyclone::AlternateId>(kOriginalId),
        serialized.size() + body.size());
    ASSERT_TRUE(wh.has_value());
    wh->set_header(serialized);
    ASSERT_TRUE(wh->write_sync(serialized).has_value());
    ASSERT_TRUE(wh->write_sync(std::as_bytes(std::span(body))).has_value());
    ASSERT_TRUE(wh->close_sync().has_value());
  }

  // It is really there, and its prefix really does parse — so neither the
  // existence check nor the prefix check is what refuses it.
  EXPECT_TRUE(cache_->AlternateExists("/imposter.jpg", "example.com", "https",
                                      kOriginalId));
  auto raw = cache_->ReadAlternate("/imposter.jpg", "example.com", "https",
                                   kOriginalId);
  ASSERT_TRUE(raw.has_value());
  EXPECT_EQ(BodyOf(*raw), body);

  auto read =
      cache_->ReadOriginalAlternate("/imposter.jpg", "example.com", "https");
  EXPECT_FALSE(read.has_value())
      << "an entry that does not say it is this class must not be returned as "
         "one";
  if (!read.has_value()) {
    EXPECT_EQ(read.error(), cyclone::CacheError::NotFound);
  }
}

// ---------------------------------------------------------------------------
// Never selectable.
// ---------------------------------------------------------------------------

TEST_F(DurableOriginalsTest, NoRequestShapeCanSelectTheOriginal) {
  CreateCache();
  // A realistic family: the identity original slot, two optimized variants,
  // and the durable original beside them.
  WriteVariant("/hero.jpg", CapabilityMask(), "identity-bytes");
  WriteVariant("/hero.jpg",
               CapabilityMask(CapabilityMask::ImageFormat::kWebP,
                              CapabilityMask::Viewport::kMobile,
                              CapabilityMask::PixelDensity::k1x,
                              CapabilityMask::SaveData::kOff,
                              CapabilityMask::TransferEncoding::kIdentity),
               "webp-bytes");
  WriteVariant("/hero.jpg",
               CapabilityMask(CapabilityMask::ImageFormat::kAvif,
                              CapabilityMask::Viewport::kDesktop,
                              CapabilityMask::PixelDensity::k2xPlus,
                              CapabilityMask::SaveData::kOn,
                              CapabilityMask::TransferEncoding::kBrotli),
               "avif-bytes");
  ASSERT_TRUE(WriteOriginalTo("/hero.jpg", "ORIGINAL", OriginalMetadata()));

  // Every request shape the mask can express, and both entitlement values.
  // The markdown exception is the only sentinel exception there is, and it
  // must not extend to this class.
  auto key = KeyFor("/hero.jpg");
  int checked = 0;
  for (int fmt = 0; fmt < 4; ++fmt) {
    for (int vp = 0; vp < 3; ++vp) {
      for (int den = 0; den < 2; ++den) {
        for (int sd = 0; sd < 2; ++sd) {
          for (int enc = 0; enc < 4; ++enc) {
            auto mask = CapabilityMask(
                static_cast<CapabilityMask::ImageFormat>(fmt),
                static_cast<CapabilityMask::Viewport>(vp),
                static_cast<CapabilityMask::PixelDensity>(den),
                static_cast<CapabilityMask::SaveData>(sd),
                static_cast<CapabilityMask::TransferEncoding>(enc));
            for (bool entitled : {false, true}) {
              auto r = cache_->ReadBestAlternateByKey(key, mask, entitled);
              if (!r.has_value()) continue;
              ++checked;
              EXPECT_NE(r->metadata.full_mask & 0xFF, kOriginalId)
                  << "selection produced the durable original for mask 0x"
                  << std::hex << mask.Encode() << " entitled=" << entitled;
              EXPECT_NE(BodyOf(*r), "ORIGINAL");
            }
          }
        }
      }
    }
  }
  EXPECT_GT(checked, 0) << "the sweep proved nothing if nothing was served";
}

TEST_F(DurableOriginalsTest, ScoringNeverReachesTheOriginalEvenAlone) {
  // Not a duplicate of the sweep above: there, another alternate could have
  // won on score.  Here the durable original is the ONLY thing on the key, so
  // a selector that scored it at all would have to return it.  This is also
  // the population fence item 13 exists for — a resource with no optimized
  // original stored — and it is why the serve path needs an exact-id read.
  CreateCache();
  ASSERT_TRUE(WriteOriginalTo("/lonely.jpg", "ORIGINAL", OriginalMetadata()));

  auto key = KeyFor("/lonely.jpg");
  for (bool entitled : {false, true}) {
    auto r = cache_->ReadBestAlternateByKey(key, CapabilityMask(), entitled);
    EXPECT_FALSE(r.has_value())
        << "selection must find nothing, not the original (entitled="
        << entitled << ")";
  }

  // The exact-id read is what serves it: it bypasses scoring entirely, which
  // is why reading it deliberately does not weaken the invariant that makes
  // the class safe.
  auto exact = cache_->ReadOriginalAlternateByKey(key);
  ASSERT_TRUE(exact.has_value());
  EXPECT_EQ(BodyOf(*exact), "ORIGINAL");
}

// ---------------------------------------------------------------------------
// It dies with its origin.
// ---------------------------------------------------------------------------

TEST_F(DurableOriginalsTest, OriginRefreshDropsTheOriginalWithTheFamily) {
  CreateCache();
  const AlternateId identity =
      MaskToAlternateId(static_cast<uint8_t>(CapabilityMask().Encode() & 0xFF));

  WriteVariant("/hero.jpg", CapabilityMask(), "identity-bytes");
  WriteVariant("/hero.jpg",
               CapabilityMask(CapabilityMask::ImageFormat::kWebP,
                              CapabilityMask::Viewport::kMobile,
                              CapabilityMask::PixelDensity::k1x,
                              CapabilityMask::SaveData::kOff,
                              CapabilityMask::TransferEncoding::kIdentity),
               "webp-bytes");
  ASSERT_TRUE(WriteOriginalTo("/hero.jpg", "ORIGINAL", OriginalMetadata()));
  {
    auto hints = cache_->WriteSentinel("/hero.jpg", "example.com", "https",
                                       SentinelId::kEarlyHints, 4);
    ASSERT_TRUE(hints.has_value());
    ASSERT_TRUE(
        hints->write_sync(std::as_bytes(std::span("hint", 4))).has_value());
    ASSERT_TRUE(hints->close_sync().has_value());
  }

  const AlternateId preserve[] = {identity};
  auto removed = cache_->RemoveAlternatesExcept("/hero.jpg", "example.com",
                                                "https", preserve);
  ASSERT_TRUE(removed.has_value());
  EXPECT_EQ(*removed, 3u);  // webp variant, early hints, durable original

  // The refreshed identity survives; the original and everything derived from
  // the previous origin response are gone in that one event.
  EXPECT_TRUE(
      cache_->AlternateExists("/hero.jpg", "example.com", "https", identity));
  EXPECT_FALSE(cache_->AlternateExists("/hero.jpg", "example.com", "https",
                                       kOriginalId));
  EXPECT_FALSE(
      cache_->ReadOriginalAlternate("/hero.jpg", "example.com", "https")
          .has_value());
  auto alts = cache_->ListAlternates("/hero.jpg", "example.com", "https");
  ASSERT_TRUE(alts.has_value());
  ASSERT_EQ(alts->size(), 1u);
  EXPECT_EQ(static_cast<AlternateId>((*alts)[0].id), identity);
}

TEST_F(DurableOriginalsTest, TheOriginalCannotBePreservedAcrossARefresh) {
  CreateCache();
  const AlternateId identity =
      MaskToAlternateId(static_cast<uint8_t>(CapabilityMask().Encode() & 0xFF));
  WriteVariant("/hero.jpg", CapabilityMask(), "identity-bytes");
  WriteVariant("/hero.jpg",
               CapabilityMask(CapabilityMask::ImageFormat::kWebP,
                              CapabilityMask::Viewport::kMobile,
                              CapabilityMask::PixelDensity::k1x,
                              CapabilityMask::SaveData::kOff,
                              CapabilityMask::TransferEncoding::kIdentity),
               "webp-bytes");
  ASSERT_TRUE(WriteOriginalTo("/hero.jpg", "ORIGINAL", OriginalMetadata()));

  const AlternateId preserve[] = {identity, kOriginalId};
  auto removed = cache_->RemoveAlternatesExcept("/hero.jpg", "example.com",
                                                "https", preserve);
  ASSERT_FALSE(removed.has_value());
  EXPECT_EQ(removed.error(), cyclone::CacheError::InvalidArgument);

  // Refused WHOLE: a partially honoured purge — variants gone, original kept —
  // is precisely the state that rebuilds the new variant set from the old
  // bytes, so nothing may be removed on this path.
  auto alts = cache_->ListAlternates("/hero.jpg", "example.com", "https");
  ASSERT_TRUE(alts.has_value());
  EXPECT_EQ(alts->size(), 3u);
}

// ---------------------------------------------------------------------------
// RemoveDerivedAlternates: the mirror image, for a NEW original.
// ---------------------------------------------------------------------------

namespace {
CapabilityMask GzipMask() {
  return CapabilityMask(
      CapabilityMask::ImageFormat::kOriginal,
      CapabilityMask::Viewport::kDesktop, CapabilityMask::PixelDensity::k1x,
      CapabilityMask::SaveData::kOff, CapabilityMask::TransferEncoding::kGzip);
}
constexpr AlternateId kSentinelHash =
    static_cast<AlternateId>(SentinelId::kContentHash);
}  // namespace

TEST_F(DurableOriginalsTest, DerivedPurgeKeepsTheOriginalAndRemovesTheRest) {
  CreateCache();
  const AlternateId identity =
      MaskToAlternateId(static_cast<uint8_t>(CapabilityMask().Encode() & 0xFF));
  const AlternateId gzip =
      MaskToAlternateId(static_cast<uint8_t>(GzipMask().Encode() & 0xFF));
  ASSERT_NE(gzip, identity);

  ASSERT_TRUE(WriteOriginalTo("/logo.svg", "NEWORIGINAL", OriginalMetadata()));
  WriteVariant("/logo.svg", GzipMask(), "gzip-of-old");
  WriteVariant("/logo.svg", CapabilityMask(), "worker-identity",
               AlternateMetadata::kFlagWorkerProcessed);
  {
    auto oracle = cache_->WriteSentinel("/logo.svg", "example.com", "https",
                                        SentinelId::kContentHash, 4);
    ASSERT_TRUE(oracle.has_value());
    ASSERT_TRUE(
        oracle->write_sync(std::as_bytes(std::span("hash", 4))).has_value());
    ASSERT_TRUE(oracle->close_sync().has_value());
  }
  {
    auto tomb = cache_->WriteSentinel("/logo.svg", "example.com", "https",
                                      SentinelId::kDeclineTombstone, 4);
    ASSERT_TRUE(tomb.has_value());
    ASSERT_TRUE(
        tomb->write_sync(std::as_bytes(std::span("tomb", 4))).has_value());
    ASSERT_TRUE(tomb->close_sync().has_value());
    AlternateMetadata md_meta;
    md_meta.full_mask = static_cast<uint32_t>(SentinelId::kAgentMarkdown);
    md_meta.content_type = ContentType::kHtml;
    auto md = cache_->WriteAgentAlternate("/logo.svg", "example.com", "https",
                                          2, md_meta);
    ASSERT_TRUE(md.has_value());
    ASSERT_TRUE(md->write_sync(std::as_bytes(std::span("md", 2))).has_value());
    ASSERT_TRUE(md->close_sync().has_value());
  }
  // Warm this process's RAM tier so a stale RAM copy would show.
  ASSERT_TRUE(cache_->ReadAlternate("/logo.svg", "example.com", "https", gzip)
                  .has_value());

  auto removed =
      cache_->RemoveDerivedAlternates("/logo.svg", "example.com", "https");
  ASSERT_TRUE(removed.has_value());
  // gzip copy, worker identity, content-hash oracle, tombstone, markdown
  EXPECT_EQ(*removed, 5u);

  EXPECT_FALSE(
      cache_->AlternateExists("/logo.svg", "example.com", "https", gzip));
  EXPECT_FALSE(
      cache_->AlternateExists("/logo.svg", "example.com", "https", identity));
  EXPECT_FALSE(cache_->AlternateExists("/logo.svg", "example.com", "https",
                                       kSentinelHash));
  EXPECT_FALSE(cache_->AlternateExists(
      "/logo.svg", "example.com", "https",
      static_cast<AlternateId>(SentinelId::kDeclineTombstone)));
  EXPECT_FALSE(cache_->AlternateExists(
      "/logo.svg", "example.com", "https",
      static_cast<AlternateId>(SentinelId::kAgentMarkdown)));
  // The RAM copy went with the disk entry.
  EXPECT_FALSE(cache_->ReadAlternate("/logo.svg", "example.com", "https", gzip)
                   .has_value());
  auto original =
      cache_->ReadOriginalAlternate("/logo.svg", "example.com", "https");
  ASSERT_TRUE(original.has_value());
  EXPECT_EQ(BodyOf(*original), "NEWORIGINAL");
}

TEST_F(DurableOriginalsTest, DerivedPurgeKeepsAGenuineIdentityAndTheSidecar) {
  CreateCache();
  const AlternateId identity =
      MaskToAlternateId(static_cast<uint8_t>(CapabilityMask().Encode() & 0xFF));
  const AlternateId gzip =
      MaskToAlternateId(static_cast<uint8_t>(GzipMask().Encode() & 0xFF));

  ASSERT_TRUE(WriteOriginalTo("/site.css", "NEW", OriginalMetadata()));
  WriteVariant("/site.css", CapabilityMask(), "front-end-identity");  // flag 0
  WriteVariant("/site.css", GzipMask(), "gzip-of-old");
  const std::vector<HeaderField> headers = {
      {"X-Content-Type-Options", "nosniff"}};
  ASSERT_TRUE(
      cache_->WriteHeadersSidecar("/site.css", "example.com", "https", headers)
          .has_value());

  auto removed =
      cache_->RemoveDerivedAlternates("/site.css", "example.com", "https");
  ASSERT_TRUE(removed.has_value());
  EXPECT_EQ(*removed, 1u);  // only the gzip copy

  EXPECT_TRUE(
      cache_->AlternateExists("/site.css", "example.com", "https", identity));
  EXPECT_TRUE(cache_->AlternateExists("/site.css", "example.com", "https",
                                      kOriginalId));
  EXPECT_TRUE(cache_->AlternateExists(
      "/site.css", "example.com", "https",
      static_cast<AlternateId>(SentinelId::kHeadersSidecar)));
  EXPECT_FALSE(
      cache_->AlternateExists("/site.css", "example.com", "https", gzip));
}

TEST_F(DurableOriginalsTest, DerivedPurgeReadsTheIdentityFlagFromDisk) {
  // A stale RAM copy of a worker-processed identity must not make a genuine
  // re-recorded identity look derived, nor the reverse.
  CreateCache();
  const AlternateId identity =
      MaskToAlternateId(static_cast<uint8_t>(CapabilityMask().Encode() & 0xFF));
  ASSERT_TRUE(WriteOriginalTo("/a.svg", "O", OriginalMetadata()));
  WriteVariant("/a.svg", CapabilityMask(), "derived",
               AlternateMetadata::kFlagWorkerProcessed);
  ASSERT_TRUE(cache_->ReadAlternate("/a.svg", "example.com", "https", identity)
                  .has_value());  // RAM now holds the worker-processed entry

  // A peer handle re-records a genuine identity over it.
  PageSpeedCacheConfig config;
  config.volume_path = cache_path_;
  config.volume_size = static_cast<uint64_t>(10 * 1024 * 1024);
  auto peer = PageSpeedCache::Create(config);
  ASSERT_TRUE(peer.has_value());
  {
    AlternateMetadata meta;
    meta.full_mask = CapabilityMask().Encode();
    meta.content_type = ContentType::kImage;
    meta.origin_content_type = "image/svg+xml";
    auto wh = (*peer)->WriteAlternate("/a.svg", "example.com", "https",
                                      identity, 7, meta);
    ASSERT_TRUE(wh.has_value());
    ASSERT_TRUE(
        wh->write_sync(std::as_bytes(std::span("genuine", 7))).has_value());
    ASSERT_TRUE(wh->close_sync().has_value());
  }

  auto removed =
      cache_->RemoveDerivedAlternates("/a.svg", "example.com", "https");
  ASSERT_TRUE(removed.has_value());
  EXPECT_EQ(*removed, 0u);
  EXPECT_TRUE(
      cache_->AlternateExists("/a.svg", "example.com", "https", identity));
}

TEST_F(DurableOriginalsTest, DerivedPurgeOnAMissingKeyRemovesNothing) {
  CreateCache();
  auto removed =
      cache_->RemoveDerivedAlternates("/never.svg", "example.com", "https");
  ASSERT_TRUE(removed.has_value());
  EXPECT_EQ(*removed, 0u);
}

TEST_F(DurableOriginalsTest, DerivedPurgeWithOnlyThePreservedClassesIsANoOp) {
  CreateCache();
  ASSERT_TRUE(WriteOriginalTo("/only.svg", "O", OriginalMetadata()));
  auto removed =
      cache_->RemoveDerivedAlternates("/only.svg", "example.com", "https");
  ASSERT_TRUE(removed.has_value());
  EXPECT_EQ(*removed, 0u);
  EXPECT_TRUE(cache_->ReadOriginalAlternate("/only.svg", "example.com", "https")
                  .has_value());
}

// ---------------------------------------------------------------------------
// The purge must end the refresh loop whatever shape the chain is in.
// ---------------------------------------------------------------------------
//
// Issue #1566.  A URL whose optimized variants were rebuilt over and over
// reached a state where one listing of its alternate chain did not show the
// whole chain, the storage layer refused every further write to the key, and
// a purge that removed "everything one listing shows" left a stale variant
// servable.  The front end selected it, declined it on freshness and asked for
// the refresh again, while no re-record could land — the loop.
//
// The storage layer no longer ACCUMULATES its way into that state: a write
// unlinks the superseded document with the same alternate id as it goes, so a
// re-recorded variant no longer grows the chain, and a single-id chain that is
// already at the traversal boundary is reset by the next write to that id.
// What the storage layer does NOT do is guard its READ and REMOVAL walks: a
// chain whose links form a cycle — which its own write path documents as a
// post-wrap possibility, and which the reset does not cover once a second
// alternate id is on the key — still lists the same node over and over, still
// refuses writes, and still absorbs removals without changing.  That shape is
// what these tests build, directly, and it is what the purge below must end.
//
// Two earlier tests here built the same wedge by re-recording one alternate
// until the write was refused.  That construction is gone with the
// accumulation, and the assertions it carried — the purge clears the key and
// the key takes writes again; an ordinary chain keeps its preserved entry —
// are the ones the two tests below make, so they were retired rather than
// rebuilt around a shape that can no longer exist.

namespace {

const CapabilityMask kDesktopWebP(CapabilityMask::ImageFormat::kWebP,
                                  CapabilityMask::Viewport::kDesktop,
                                  CapabilityMask::PixelDensity::k1x,
                                  CapabilityMask::SaveData::kOff,
                                  CapabilityMask::TransferEncoding::kIdentity);

AlternateId IdOf(const CapabilityMask& mask) {
  return MaskToAlternateId(static_cast<uint8_t>(mask.Encode() & 0xFF));
}

}  // namespace

TEST_F(DurableOriginalsTest, OriginRefreshDropsAKeyWhoseChainCannotBeWalked) {
  // The reported shape: a chain whose head links back to itself.  At the
  // storage layer this issue was reported against, one listing returned the
  // traversal-cap number of copies of that node, every write to the key was
  // refused, and the per-alternate removal re-pointed the directory at the
  // head's successor — the head — and reported success without changing
  // anything.  The current storage layer rejects the self-link instead: the
  // listing stops there, writes land, and the removal refuses with
  // ChainCorrupted.  Either way no bounded number of per-alternate passes can
  // unlink the stale head; only dropping the whole key ends the loop.  This
  // test builds that chain and pins the drop.
  //
  // TWO ids, not one: the storage layer resets a single-id chain at the
  // traversal boundary when the next write carries that same id, so a
  // one-variant key heals itself.  A key that also holds an original — every
  // key the front end records — is past that heal, which is the case that
  // still needs this purge.
  CreateCache();
  const AlternateId identity = IdOf(CapabilityMask());
  const AlternateId webp_id = IdOf(kDesktopWebP);

  WriteVariant("/loop.jpg", CapabilityMask(), "identity-bytes");
  WriteVariant("/loop.jpg", kDesktopWebP, "webp-bytes");
  auto two = cache_->ListAlternates("/loop.jpg", "example.com", "https");
  ASSERT_TRUE(two.has_value());
  ASSERT_EQ(two->size(), 2u);
  ASSERT_EQ(static_cast<AlternateId>((*two)[0].id), webp_id)
      << "the newest write must be the head";
  const uint64_t head = (*two)[0].disk_offset;
  const uint64_t tail = (*two)[1].disk_offset;

  // The document's fixed header carries its successor as a stripe-relative
  // offset at a fixed position, outside the checksum.  Read the
  // head's successor field (it names the tail), derive the stripe base from
  // it, and re-point the head at itself.
  //
  // The cache is CLOSED for the patch and re-opened after: the volume is a
  // persistent file, and editing it through a second handle while the cache
  // holds it mapped is not portable (Windows sharing modes forbid it).  This
  // way the whole-key escalation is covered on every platform the suite runs
  // on, which matters because that path is the one that destroys data.
  cache_.reset();
  ASSERT_NO_FATAL_FAILURE(SelfLinkChainHead(head, tail));
  CreateCache();  // Same volume file, re-opened.

  // The storage layer rejects a link that points at or above its own node,
  // so the listing stops at the bad link: the head is listed once and the
  // identity variant behind it is unreachable.  (Before that check, this
  // listing returned cap-many copies of the head and the re-record below was
  // refused.)
  auto looped = cache_->ListAlternates("/loop.jpg", "example.com", "https");
  ASSERT_TRUE(looped.has_value());
  ASSERT_EQ(looped->size(), 1u);
  ASSERT_EQ(looped->front().disk_offset, head);
  ASSERT_EQ(static_cast<AlternateId>(looped->front().id), webp_id);
  // The re-record the front end performs after serving the origin now lands,
  // prepended ahead of the bad link — but the stale webp head is still
  // selectable, and removing it has to follow the link it cannot follow.
  ASSERT_TRUE(WriteOriginalTo("/loop.jpg", "REFRESHED", OriginalMetadata()));
  ASSERT_TRUE(
      cache_->AlternateExists("/loop.jpg", "example.com", "https", webp_id))
      << "the stale head must still be listed, or this test is not standing "
         "on the shape it describes";

  const AlternateId preserve[] = {identity};
  auto removed = cache_->RemoveAlternatesExcept("/loop.jpg", "example.com",
                                                "https", preserve);
  ASSERT_TRUE(removed.has_value()) << static_cast<int>(removed.error());

  EXPECT_FALSE(
      cache_->AlternateExists("/loop.jpg", "example.com", "https", webp_id))
      << "the stale head survived: the front end selects it, declines it on "
         "freshness and asks for the refresh again — the loop";
  auto after = cache_->ListAlternates("/loop.jpg", "example.com", "https");
  EXPECT_FALSE(after.has_value() && !after->empty());

  // The key is writable again: the next fall-through's re-record lands and
  // its notify rebuilds the URL from fresh bytes.
  ASSERT_TRUE(WriteOriginalTo("/loop.jpg", "REFRESHED", OriginalMetadata()));
  auto original =
      cache_->ReadOriginalAlternate("/loop.jpg", "example.com", "https");
  ASSERT_TRUE(original.has_value());
  EXPECT_EQ(BodyOf(*original), "REFRESHED");
}

TEST_F(DurableOriginalsTest, OriginRefreshLeavesAHealthyVariantSetIntact) {
  // The false-positive direction, which is what the whole-key escalation must
  // never reach: an ordinary variant matrix with a preserved identity.  Every
  // non-preserved entry goes, the preserved one stays, and the key survives.
  CreateCache();
  const AlternateId identity = IdOf(CapabilityMask());

  const CapabilityMask matrix[] = {
      CapabilityMask(CapabilityMask::ImageFormat::kWebP,
                     CapabilityMask::Viewport::kDesktop,
                     CapabilityMask::PixelDensity::k1x,
                     CapabilityMask::SaveData::kOff,
                     CapabilityMask::TransferEncoding::kIdentity),
      CapabilityMask(CapabilityMask::ImageFormat::kAvif,
                     CapabilityMask::Viewport::kDesktop,
                     CapabilityMask::PixelDensity::k1x,
                     CapabilityMask::SaveData::kOff,
                     CapabilityMask::TransferEncoding::kIdentity),
      CapabilityMask(
          CapabilityMask::ImageFormat::kWebP, CapabilityMask::Viewport::kMobile,
          CapabilityMask::PixelDensity::k2xPlus, CapabilityMask::SaveData::kOff,
          CapabilityMask::TransferEncoding::kIdentity),
      CapabilityMask(
          CapabilityMask::ImageFormat::kAvif, CapabilityMask::Viewport::kMobile,
          CapabilityMask::PixelDensity::k2xPlus, CapabilityMask::SaveData::kOn,
          CapabilityMask::TransferEncoding::kIdentity),
  };
  WriteVariant("/healthy.jpg", CapabilityMask(), "identity-bytes");
  for (const auto& mask : matrix) {
    WriteVariant("/healthy.jpg", mask, "variant-bytes");
  }
  ASSERT_TRUE(WriteOriginalTo("/healthy.jpg", "ORIGINAL", OriginalMetadata()));

  const AlternateId preserve[] = {identity};
  auto removed = cache_->RemoveAlternatesExcept("/healthy.jpg", "example.com",
                                                "https", preserve);
  ASSERT_TRUE(removed.has_value()) << static_cast<int>(removed.error());
  EXPECT_EQ(*removed, std::size(matrix) + 1);  // the matrix + the original

  auto after = cache_->ListAlternates("/healthy.jpg", "example.com", "https");
  ASSERT_TRUE(after.has_value()) << "the key must not have been dropped";
  ASSERT_EQ(after->size(), 1u);
  EXPECT_EQ(static_cast<AlternateId>(after->front().id), identity);
  for (const auto& mask : matrix) {
    EXPECT_FALSE(cache_->AlternateExists("/healthy.jpg", "example.com", "https",
                                         IdOf(mask)));
  }
}

TEST_F(DurableOriginalsTest, OriginRefreshDoesNotDropAKeyWhoseRemovalsFailed) {
  // The regression guard for the escalation trigger.  "Removals reported
  // success and the chain is unchanged" is the only signature that may drop a
  // key; "no removal succeeded" must NOT be, because the storage layer has
  // maybe-transient refusals — a concurrent wrap racing a middle-node removal
  // reports Busy — and a middle-node removal is exactly what a PRESERVED head
  // produces.  Escalating on that would destroy the fresh original the
  // preserve list exists to protect.
  //
  // A second handle onto the same volume that does not own the key's stripe
  // gives the same shape deterministically: its listings succeed and every
  // removal is refused.
  CreateCache();
  const AlternateId identity = IdOf(CapabilityMask());
  const AlternateId webp_id = IdOf(kDesktopWebP);
  WriteVariant("/nonowner.jpg", CapabilityMask(), "identity-bytes");
  WriteVariant("/nonowner.jpg", kDesktopWebP, "webp-bytes");
  ASSERT_TRUE(WriteOriginalTo("/nonowner.jpg", "ORIGINAL", OriginalMetadata()));

  PageSpeedCacheConfig other_config;
  other_config.volume_path = cache_path_;
  other_config.volume_size = static_cast<uint64_t>(10 * 1024 * 1024);
  other_config.multi_process.enabled = true;
  other_config.multi_process.process_index = 1;
  other_config.multi_process.total_processes = 2;
  auto other = PageSpeedCache::Create(other_config);
  ASSERT_TRUE(other.has_value()) << "second handle onto the volume failed";
  auto& non_owner = **other;

  // Precondition: this handle can READ the key but may not write it.  If it
  // happens to own the stripe the shape under test does not exist here.
  auto listed =
      non_owner.ListAlternates("/nonowner.jpg", "example.com", "https");
  ASSERT_TRUE(listed.has_value());
  ASSERT_EQ(listed->size(), 3u);
  {
    AlternateMetadata meta;
    meta.full_mask = kDesktopWebP.Encode();
    meta.content_type = ContentType::kImage;
    auto probe = non_owner.WriteAlternate("/nonowner.jpg", "example.com",
                                          "https", webp_id, 4, meta);
    if (probe.has_value() && probe->close_sync().has_value()) {
      GTEST_SKIP() << "this handle owns the key's stripe; the refusing-removal "
                      "shape is not reproducible here";
    }
  }

  const AlternateId preserve[] = {identity};
  auto removed = non_owner.RemoveAlternatesExcept(
      "/nonowner.jpg", "example.com", "https", preserve);
  // No escalation: the call reports what it removed (nothing) rather than
  // destroying a key it could not edit.
  ASSERT_TRUE(removed.has_value())
      << "a pass whose removals all failed must not escalate to a whole-key "
         "drop (error "
      << static_cast<int>(removed.error()) << ")";
  EXPECT_EQ(*removed, 0u);

  // Everything is exactly as it was — the next refresh retries.
  auto after = cache_->ListAlternates("/nonowner.jpg", "example.com", "https");
  ASSERT_TRUE(after.has_value());
  EXPECT_EQ(after->size(), 3u);
  EXPECT_TRUE(cache_->AlternateExists("/nonowner.jpg", "example.com", "https",
                                      identity));
  auto original =
      cache_->ReadOriginalAlternate("/nonowner.jpg", "example.com", "https");
  ASSERT_TRUE(original.has_value())
      << "the durable original the preserve contract protects was destroyed";
  EXPECT_EQ(BodyOf(*original), "ORIGINAL");
}

// ---------------------------------------------------------------------------
// The content cap.
// ---------------------------------------------------------------------------

TEST_F(DurableOriginalsTest, TheDefaultCapAppliesWhenTheConfigIsLeftUnset) {
  // The config field is left zero — the state a caller that never heard of it
  // is in, and the state EVERY shipped path is in today — and the library's
  // own ceiling applies anyway.
  CreateCache(0);
  EXPECT_EQ(kDefaultMaxOriginalContentLength, 16777216u);
  // The zero in the config resolves to the default everywhere it is asked,
  // including where a caller reads it back to explain a skip.
  EXPECT_EQ(cache_->MaxOriginalContentLength(),
            kDefaultMaxOriginalContentLength);

  auto wh = cache_->WriteOriginalAlternate("/huge.bin", "example.com", "https",
                                           kDefaultMaxOriginalContentLength + 1,
                                           OriginalMetadata());
  ASSERT_FALSE(wh.has_value());
  EXPECT_EQ(wh.error(), cyclone::CacheError::NoSpace);
  EXPECT_EQ(cache_->OriginalsOverCapSkipped(), 1u);
}

TEST_F(DurableOriginalsTest, UnderTheCapStores) {
  CreateCache(/*cap=*/1024);
  EXPECT_EQ(cache_->MaxOriginalContentLength(), 1024u);
  ASSERT_TRUE(WriteOriginalTo("/small.bin", std::string(1000, 'x'),
                              OriginalMetadata()));
  auto read =
      cache_->ReadOriginalAlternate("/small.bin", "example.com", "https");
  ASSERT_TRUE(read.has_value());
  EXPECT_EQ(read->content_length(), 1000u);
  EXPECT_EQ(cache_->OriginalsOverCapSkipped(), 0u);
}

TEST_F(DurableOriginalsTest, ExactlyAtTheCapStores) {
  // The boundary is inclusive: the cap is the largest size that IS stored.
  CreateCache(/*cap=*/1024);
  ASSERT_TRUE(WriteOriginalTo("/exact.bin", std::string(1024, 'x'),
                              OriginalMetadata()));
  auto read =
      cache_->ReadOriginalAlternate("/exact.bin", "example.com", "https");
  ASSERT_TRUE(read.has_value());
  EXPECT_EQ(read->content_length(), 1024u);
  EXPECT_EQ(cache_->OriginalsOverCapSkipped(), 0u);
}

TEST_F(DurableOriginalsTest, DeclaringOverTheCapSkipsTheStore) {
  CreateCache(/*cap=*/1024);
  auto wh = cache_->WriteOriginalAlternate("/big.bin", "example.com", "https",
                                           1025, OriginalMetadata());
  ASSERT_FALSE(wh.has_value());
  EXPECT_EQ(wh.error(), cyclone::CacheError::NoSpace);
  EXPECT_EQ(cache_->OriginalsOverCapSkipped(), 1u);
  EXPECT_FALSE(
      cache_->AlternateExists("/big.bin", "example.com", "https", kOriginalId));
}

TEST_F(DurableOriginalsTest, WritingPastTheCapIsCutOffMidStream) {
  // The declaration is honest-looking and the stream is not — a chunked
  // response whose real size only becomes known as it arrives.  This is the
  // case the streaming enforcement exists for: the store is abandoned at the
  // write that crosses the line, before the remaining bytes are accumulated.
  CreateCache(/*cap=*/1024);
  auto wh = cache_->WriteOriginalAlternate("/lying.bin", "example.com", "https",
                                           1024, OriginalMetadata());
  ASSERT_TRUE(wh.has_value());

  const std::string chunk(600, 'x');
  ASSERT_TRUE(wh->write_sync(std::as_bytes(std::span(chunk))).has_value());

  auto second = wh->write_sync(std::as_bytes(std::span(chunk)));
  ASSERT_FALSE(second.has_value());
  EXPECT_EQ(second.error(), cyclone::CacheError::NoSpace);
  EXPECT_EQ(cache_->OriginalsOverCapSkipped(), 1u);

  // Abandoned, not merely truncated: the handle knows WHY it is dead (the C
  // surface turns that into a message naming the cap), closing it fails, and
  // — the part that matters — no reader ever sees a partial original.
  EXPECT_TRUE(wh->over_content_cap());
  EXPECT_EQ(wh->content_cap(), 1024u);
  EXPECT_FALSE(wh->is_valid());
  EXPECT_FALSE(wh->close_sync().has_value());
  EXPECT_FALSE(cache_->AlternateExists("/lying.bin", "example.com", "https",
                                       kOriginalId));
  EXPECT_FALSE(
      cache_->ReadOriginalAlternate("/lying.bin", "example.com", "https")
          .has_value());
  auto alts = cache_->ListAlternates("/lying.bin", "example.com", "https");
  if (alts.has_value()) {
    for (const auto& alt : *alts) {
      EXPECT_NE(static_cast<AlternateId>(alt.id), kOriginalId);
    }
  }
}

TEST_F(DurableOriginalsTest, TheSkipCounterCountsBothRefusalPaths) {
  CreateCache(/*cap=*/1024);
  EXPECT_EQ(cache_->OriginalsOverCapSkipped(), 0u);

  ASSERT_FALSE(cache_
                   ->WriteOriginalAlternate("/a.bin", "example.com", "https",
                                            4096, OriginalMetadata())
                   .has_value());
  EXPECT_EQ(cache_->OriginalsOverCapSkipped(), 1u);

  auto wh = cache_->WriteOriginalAlternate("/b.bin", "example.com", "https",
                                           1024, OriginalMetadata());
  ASSERT_TRUE(wh.has_value());
  const std::string too_much(2000, 'x');
  ASSERT_FALSE(wh->write_sync(std::as_bytes(std::span(too_much))).has_value());
  EXPECT_EQ(cache_->OriginalsOverCapSkipped(), 2u);

  // A store that fits does not count, and the cap does not bleed into the
  // other entry classes: a variant far above it stores normally.
  ASSERT_TRUE(
      WriteOriginalTo("/c.bin", std::string(64, 'x'), OriginalMetadata()));
  WriteVariant("/d.bin", CapabilityMask(), std::string(4096, 'y'));
  EXPECT_EQ(cache_->OriginalsOverCapSkipped(), 2u);
}

// ---------------------------------------------------------------------------
// Lifetime comes from the origin, not from the class.
// ---------------------------------------------------------------------------

TEST_F(DurableOriginalsTest, FreshnessComesFromTheStampedOriginState) {
  CreateCache();
  const uint32_t inserted = 1'700'000'000;
  ASSERT_TRUE(WriteOriginalTo("/hero.jpg", "ORIGINAL",
                              OriginalMetadata(inserted, /*max_age=*/600)));

  auto read =
      cache_->ReadOriginalAlternate("/hero.jpg", "example.com", "https");
  ASSERT_TRUE(read.has_value());

  FreshnessConfig fc;
  // Inside the origin's own window.
  auto fresh = EvaluateFreshness(
      FreshnessInputFromMetadata(read->metadata, inserted + 100), fc);
  EXPECT_EQ(fresh.verdict, FreshnessVerdict::kFresh);
  EXPECT_EQ(fresh.effective_max_age, 600u);
  EXPECT_EQ(fresh.remaining_ttl, 500u);

  // Past it — and stale by AGE, which is the signal an origin refresh keys on.
  auto stale = EvaluateFreshness(
      FreshnessInputFromMetadata(read->metadata, inserted + 700), fc);
  EXPECT_EQ(stale.verdict, FreshnessVerdict::kRevalidate);
  EXPECT_TRUE(stale.is_stale);
  EXPECT_TRUE(stale.expired_by_age);

  // No TTL of its own: change only what the origin said, and the answer
  // changes with it.
  auto longer = OriginalMetadata(inserted, /*max_age=*/86'400);
  ASSERT_TRUE(WriteOriginalTo("/long.jpg", "ORIGINAL", longer));
  auto read2 =
      cache_->ReadOriginalAlternate("/long.jpg", "example.com", "https");
  ASSERT_TRUE(read2.has_value());
  auto still_fresh = EvaluateFreshness(
      FreshnessInputFromMetadata(read2->metadata, inserted + 700), fc);
  EXPECT_EQ(still_fresh.verdict, FreshnessVerdict::kFresh);
}

TEST_F(DurableOriginalsTest, AnUnstampedInsertTimeFailsTowardRevalidation) {
  // Zero is "no insertion time recorded".  It must read as old, never as new:
  // the failure direction is an extra fetch, not bytes served as fresher than
  // the origin allowed.
  CreateCache();
  auto meta = OriginalMetadata(/*inserted_at=*/0, /*max_age=*/600);
  ASSERT_TRUE(WriteOriginalTo("/nostamp.jpg", "ORIGINAL", meta));
  auto read =
      cache_->ReadOriginalAlternate("/nostamp.jpg", "example.com", "https");
  ASSERT_TRUE(read.has_value());

  FreshnessConfig fc;
  auto r = EvaluateFreshness(
      FreshnessInputFromMetadata(read->metadata, 1'700'000'000), fc);
  EXPECT_EQ(r.verdict, FreshnessVerdict::kRevalidate);
  EXPECT_TRUE(r.is_stale);
}

// ---------------------------------------------------------------------------
// Every write path is alternate-scoped.
// ---------------------------------------------------------------------------

TEST_F(DurableOriginalsTest, EveryCacheWritePathLeavesTheChainIntact) {
  // The disjointness that matters: each write entry point on this class
  // touches ONE alternate and leaves the rest of the chain reachable.  Proven
  // per entry point rather than argued, because the failure mode of the write
  // that is not alternate-scoped (next test) is total and silent.
  CreateCache();
  ASSERT_TRUE(WriteOriginalTo("/hero.jpg", "ORIGINAL", OriginalMetadata()));
  WriteVariant("/hero.jpg", CapabilityMask(), "identity-bytes");

  auto expect_intact = [&](const char* after) {
    SCOPED_TRACE(after);
    auto orig =
        cache_->ReadOriginalAlternate("/hero.jpg", "example.com", "https");
    ASSERT_TRUE(orig.has_value());
    EXPECT_EQ(BodyOf(*orig), "ORIGINAL");
    auto best = cache_->ReadBestAlternate("/hero.jpg", "example.com", "https",
                                          CapabilityMask());
    ASSERT_TRUE(best.has_value());
    EXPECT_EQ(BodyOf(*best), "identity-bytes");
  };

  // WriteAlternate (another variant).
  WriteVariant("/hero.jpg",
               CapabilityMask(CapabilityMask::ImageFormat::kWebP,
                              CapabilityMask::Viewport::kMobile,
                              CapabilityMask::PixelDensity::k1x,
                              CapabilityMask::SaveData::kOff,
                              CapabilityMask::TransferEncoding::kIdentity),
               "webp-bytes");
  expect_intact("WriteAlternate");

  // WriteSentinel (a prefix-less class).
  {
    auto wh = cache_->WriteSentinel("/hero.jpg", "example.com", "https",
                                    SentinelId::kEarlyHints, 4);
    ASSERT_TRUE(wh.has_value());
    ASSERT_TRUE(
        wh->write_sync(std::as_bytes(std::span("hint", 4))).has_value());
    ASSERT_TRUE(wh->close_sync().has_value());
  }
  expect_intact("WriteSentinel");

  // WriteAgentAlternate (the other sentinel-with-a-prefix class).
  {
    AlternateMetadata meta;
    meta.full_mask = static_cast<uint32_t>(SentinelId::kAgentMarkdown);
    meta.content_type = ContentType::kOther;
    auto wh = cache_->WriteAgentAlternate("/hero.jpg", "example.com", "https",
                                          2, meta);
    ASSERT_TRUE(wh.has_value());
    ASSERT_TRUE(wh->write_sync(std::as_bytes(std::span("md", 2))).has_value());
    ASSERT_TRUE(wh->close_sync().has_value());
  }
  expect_intact("WriteAgentAlternate");

  // WriteOriginalAlternate again (a re-record of the same class).
  ASSERT_TRUE(WriteOriginalTo("/hero.jpg", "ORIGINAL", OriginalMetadata()));
  expect_intact("WriteOriginalAlternate");

  // Read-back intactness is NOT the whole property, and this is the line that
  // says so: the chain must still carry ONE node of this class. Without it
  // the re-record above passes while the substrate links a superseded node
  // per store — reads keep finding the newest, so nothing looks wrong until
  // the chain ceiling stops the key from accepting writes at all.
  EXPECT_EQ(OriginalChainNodes("/hero.jpg"), 1u);
}

// ---------------------------------------------------------------------------
// Re-recording replaces rather than accumulates — as far as it can here.
// ---------------------------------------------------------------------------

TEST_F(DurableOriginalsTest, RepeatedReRecordsKeepTheChainAtOneNode) {
  // The quiet case, which is the one that has to hold: one writer, many
  // stores, one node. The counters must stay silent too — an instrument that
  // fires on the ordinary path is indistinguishable from noise exactly when
  // it matters.
  CreateCache();
  for (int i = 0; i < 8; ++i) {
    ASSERT_TRUE(WriteOriginalTo("/hero.jpg", "ORIGINAL-" + std::to_string(i),
                                OriginalMetadata()));
    EXPECT_EQ(OriginalChainNodes("/hero.jpg"), 1u) << "after store " << i;
  }

  auto read =
      cache_->ReadOriginalAlternate("/hero.jpg", "example.com", "https");
  ASSERT_TRUE(read.has_value());
  EXPECT_EQ(BodyOf(*read), "ORIGINAL-7");
  EXPECT_EQ(cache_->OriginalsSupersededObserved(), 0u);
  EXPECT_EQ(cache_->OriginalsUnlinkFailed(), 0u);
}

TEST_F(DurableOriginalsTest,
       TheSubstrateNoLongerAccumulatesAndReadsServeNewest) {
  // This used to demonstrate the defect this writer mitigates. Planting
  // bypasses the cache's writer — exactly what a second process racing this
  // one achieves by accident — and every store linked another node while
  // every read still returned the newest, which is why a read-back assertion
  // could never have caught it.
  //
  // The storage layer now unlinks the superseded same-id node as part of the
  // write itself, so the accumulation no longer happens even with this repo's
  // own writer-side mitigation bypassed. Depth stays at one, and reads still
  // serve the newest.
  CreateCache();
  for (int i = 1; i <= 4; ++i) {
    PlantOriginalNode("/hero.jpg", "PLANTED-" + std::to_string(i));
    ASSERT_EQ(OriginalChainNodes("/hero.jpg"), 1u) << "after plant " << i;
    auto read =
        cache_->ReadOriginalAlternate("/hero.jpg", "example.com", "https");
    ASSERT_TRUE(read.has_value());
    EXPECT_EQ(BodyOf(*read), "PLANTED-" + std::to_string(i));
  }
}

TEST_F(DurableOriginalsTest, DepthStaysAtOneAndReadsStillServeNewest) {
  // There used to be a fixed point here: a concurrent interleaving left a
  // chain at two, the writer unlinked ONE node and wrote ONE, so the depth
  // never came back down on its own and the cost was recorded as bounded
  // rather than self-healing.
  //
  // The storage layer now unlinks the superseded same-id node as part of the
  // write, so that two-node state cannot be built up in the first place — the
  // plants below already collapse to one. The property callers depend on is
  // unchanged: the read still serves the newest.
  CreateCache();
  PlantOriginalNode("/hero.jpg", "PLANTED-OLD");
  PlantOriginalNode("/hero.jpg", "PLANTED-NEW");
  ASSERT_EQ(OriginalChainNodes("/hero.jpg"), 1u);

  ASSERT_TRUE(WriteOriginalTo("/hero.jpg", "RE-RECORDED", OriginalMetadata()));

  EXPECT_EQ(OriginalChainNodes("/hero.jpg"), 1u);
  auto read =
      cache_->ReadOriginalAlternate("/hero.jpg", "example.com", "https");
  ASSERT_TRUE(read.has_value());
  EXPECT_EQ(BodyOf(*read), "RE-RECORDED");

  // The writer-side mitigation is now belt-and-braces: it finds no superseded
  // node to observe, so the counter that existed to keep the limitation
  // visible in the field reads zero rather than climbing.
  EXPECT_EQ(cache_->OriginalsSupersededObserved(), 0u);

  // And it stays that way across further re-records, rather than the depth
  // creeping up once the first store has been absorbed.
  ASSERT_TRUE(
      WriteOriginalTo("/hero.jpg", "RE-RECORDED-2", OriginalMetadata()));
  EXPECT_EQ(cache_->OriginalsSupersededObserved(), 0u);
  EXPECT_EQ(OriginalChainNodes("/hero.jpg"), 1u);
}

TEST_F(DurableOriginalsTest, AReRecordDoesNotDisturbTheRestOfTheChain) {
  // The unlink is alternate-scoped: it takes this class's node and nothing
  // else. Getting that wrong would be the orphaning hazard by another route,
  // so it is asserted rather than assumed.
  CreateCache();
  ASSERT_TRUE(WriteOriginalTo("/hero.jpg", "ORIGINAL", OriginalMetadata()));
  WriteVariant("/hero.jpg", CapabilityMask(), "identity-bytes");
  WriteVariant("/hero.jpg",
               CapabilityMask(CapabilityMask::ImageFormat::kWebP,
                              CapabilityMask::Viewport::kMobile,
                              CapabilityMask::PixelDensity::k1x,
                              CapabilityMask::SaveData::kOff,
                              CapabilityMask::TransferEncoding::kIdentity),
               "webp-bytes");
  auto before = cache_->ListAlternates("/hero.jpg", "example.com", "https");
  ASSERT_TRUE(before.has_value());

  ASSERT_TRUE(WriteOriginalTo("/hero.jpg", "ORIGINAL-2", OriginalMetadata()));

  auto after = cache_->ListAlternates("/hero.jpg", "example.com", "https");
  ASSERT_TRUE(after.has_value());
  EXPECT_EQ(after->size(), before->size());
  EXPECT_EQ(OriginalChainNodes("/hero.jpg"), 1u);
  // Every other alternate is still readable, and still itself.
  auto identity = cache_->ReadBestAlternate("/hero.jpg", "example.com", "https",
                                            CapabilityMask());
  ASSERT_TRUE(identity.has_value());
  EXPECT_EQ(BodyOf(*identity), "identity-bytes");
}

TEST_F(DurableOriginalsTest, AWholeKeyWriteOrphansTheEntireChain) {
  // The hazard, demonstrated rather than described, so that the rule above is
  // known to be worth something: a plain write on a key that carries an
  // alternate chain repoints the directory head at a document with no
  // alternate id, and EVERYTHING on that key — every variant and the durable
  // original — becomes unreachable at once.  It does not error, and from the
  // outside it is indistinguishable from a cold cache.
  //
  // This is why no method on PageSpeedCache takes this path, and why a second
  // writer sharing this volume must not either.
  CreateCache();
  ASSERT_TRUE(WriteOriginalTo("/hero.jpg", "ORIGINAL", OriginalMetadata()));
  WriteVariant("/hero.jpg", CapabilityMask(), "identity-bytes");
  ASSERT_TRUE(cache_->ReadOriginalAlternate("/hero.jpg", "example.com", "https")
                  .has_value());

  auto key = KeyFor("/hero.jpg");
  {
    auto raw = PageSpeedCacheTestPeer::Cyclone(*cache_).write_sync(key, 4);
    ASSERT_TRUE(raw.has_value());
    ASSERT_TRUE(
        raw->write_sync(std::as_bytes(std::span("junk", 4))).has_value());
    ASSERT_TRUE(raw->close_sync().has_value());
  }

  EXPECT_FALSE(
      cache_->ReadOriginalAlternate("/hero.jpg", "example.com", "https")
          .has_value())
      << "if this ever passes, the substrate changed and the invariant above "
         "should be re-derived rather than assumed";
  EXPECT_FALSE(cache_
                   ->ReadBestAlternate("/hero.jpg", "example.com", "https",
                                       CapabilityMask())
                   .has_value());
}

}  // namespace
}  // namespace pagespeed
