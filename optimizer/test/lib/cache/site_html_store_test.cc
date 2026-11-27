// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

// The cache side of a front end's site-scoped HTML store: a sidecar paired
// with the durable original it belongs to, the withheld marker (never
// readable as a page, cleared by a re-store of the original), and the
// per-site purge epoch (a max-merge, clamped to the present, safe against
// racing writers on separate handles of one volume).

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <random>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include "gtest/gtest.h"
#include "lib/cache/cache.h"
#include "lib/cache/headers_sidecar.h"
#include "lib/cache/site_purge_epoch.h"
#include "lib/classify/alternate_id.h"
#include "lib/classify/alternate_metadata.h"
#include "lib/classify/capability_mask.h"
#include "test/test_util/temp_dir.h"

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#elif defined(__APPLE__)
#include <mach-o/dyld.h>
#include <sys/wait.h>
#include <unistd.h>
#else
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace pagespeed {
namespace {

const std::string kSiteA(64, 'a');
const std::string kSiteB = std::string(63, 'a') + "b";
constexpr std::string_view kUrl = "/page.html";
constexpr std::string_view kHost = "example.com";
constexpr std::string_view kScheme = "https";
constexpr AlternateId kMarkerId =
    static_cast<AlternateId>(SentinelId::kAgentMarkdownWithheld);
constexpr AlternateId kMarkdownId =
    static_cast<AlternateId>(SentinelId::kAgentMarkdown);

std::array<std::byte, AlternateMetadata::kHashSize> Hash(uint8_t seed) {
  std::array<std::byte, AlternateMetadata::kHashSize> h{};
  for (size_t i = 0; i < h.size(); ++i) {
    h[i] = static_cast<std::byte>(seed + i + 1);
  }
  return h;
}

class SiteHtmlStoreTest : public ::testing::Test {
 protected:
  void SetUp() override {
    temp_dir_ = pagespeed::test::MakeTempDir();
    cache_path_ = temp_dir_ + "/cache.vol";
    cache_ = Open();
    ASSERT_NE(cache_, nullptr);
  }

  void TearDown() override {
    cache_.reset();
    std::filesystem::remove_all(temp_dir_);
  }

  // Another handle on the same volume, the way a second process opens it.
  std::unique_ptr<PageSpeedCache> Open() {
    PageSpeedCacheConfig config;
    config.volume_path = cache_path_;
    config.volume_size = static_cast<uint64_t>(10 * 1024 * 1024);
    auto result = PageSpeedCache::Create(config);
    if (!result.has_value()) return nullptr;
    return std::move(*result);
  }

  void WriteOriginal(std::string_view site, std::string_view body,
                     const std::array<std::byte, 32>* hash = nullptr) {
    AlternateMetadata meta;
    meta.full_mask = static_cast<uint32_t>(SentinelId::kOriginalContent);
    meta.content_type = ContentType::kHtml;
    meta.origin_content_type = "text/html";
    meta.flags = AlternateMetadata::kFlagHeadersClassified;
    if (hash != nullptr) meta.origin_html_hash = *hash;
    auto wh = cache_->WriteOriginalAlternate(kUrl, kHost, kScheme, body.size(),
                                             meta, site);
    ASSERT_TRUE(wh.has_value());
    ASSERT_TRUE(wh->write_sync(std::as_bytes(std::span(body))).has_value());
    ASSERT_TRUE(wh->close_sync().has_value());
  }

  void WriteMarker(std::string_view site) {
    const std::string payload = "\x01marker";
    auto wh = cache_->WriteSentinel(kUrl, kHost, kScheme,
                                    SentinelId::kAgentMarkdownWithheld,
                                    payload.size(), site);
    ASSERT_TRUE(wh.has_value());
    ASSERT_TRUE(wh->write_sync(std::as_bytes(std::span(payload))).has_value());
    ASSERT_TRUE(wh->close_sync().has_value());
  }

  void WriteMarkdown(std::string_view site, std::string_view body) {
    AlternateMetadata meta;
    meta.full_mask = static_cast<uint32_t>(SentinelId::kAgentMarkdown);
    meta.content_type = ContentType::kOther;
    meta.origin_content_type = "text/markdown";
    auto wh = cache_->WriteAgentAlternate(kUrl, kHost, kScheme, body.size(),
                                          meta, site);
    ASSERT_TRUE(wh.has_value());
    ASSERT_TRUE(wh->write_sync(std::as_bytes(std::span(body))).has_value());
    ASSERT_TRUE(wh->close_sync().has_value());
  }

  bool Exists(std::string_view site, AlternateId id) {
    return cache_->AlternateExists(kUrl, kHost, kScheme, id, site);
  }

  std::string temp_dir_;
  std::string cache_path_;
  std::unique_ptr<PageSpeedCache> cache_;
};

// ------------------------------------------------------------ pairing

TEST_F(SiteHtmlStoreTest, APairedSidecarIsStoredInFormat2WithItsHash) {
  const auto hash = Hash(1);
  const std::vector<HeaderField> headers = {{"Content-Type", "text/html"},
                                            {"Referrer-Policy", "no-referrer"}};
  auto written =
      cache_->WriteHeadersSidecar(kUrl, kHost, kScheme, headers, kSiteA, hash);
  ASSERT_TRUE(written.has_value());
  EXPECT_TRUE(written->stored);

  auto read = cache_->ReadHeadersSidecar(kUrl, kHost, kScheme, kSiteA);
  ASSERT_TRUE(read.has_value());
  const auto payload = PageSpeedCache::HeadersSidecarPayload(*read);
  EXPECT_EQ(static_cast<unsigned char>(payload[0]),
            kHeadersSidecarPairedFormatVersion);
  auto block = ParseHeadersSidecarBlock(payload);
  ASSERT_TRUE(block.has_value());
  ASSERT_TRUE(block->pairing_hash.has_value());
  EXPECT_EQ(*block->pairing_hash, hash);
  ASSERT_EQ(block->fields.size(), 1u);
  EXPECT_EQ(block->fields[0].name, "Referrer-Policy");
}

TEST_F(SiteHtmlStoreTest, APairedWriteStoresABlockWithNothingToCarry) {
  // Content-Type is reproduced by the entry metadata, so there is nothing
  // for the block to carry. Unpaired, that is no entry at all; paired, it is
  // a recorded, provable "nothing extra" for this original.
  const std::vector<HeaderField> headers = {{"Content-Type", "text/html"}};
  auto unpaired =
      cache_->WriteHeadersSidecar(kUrl, kHost, kScheme, headers, kSiteA);
  ASSERT_TRUE(unpaired.has_value());
  EXPECT_FALSE(unpaired->stored);
  EXPECT_FALSE(
      cache_->ReadHeadersSidecar(kUrl, kHost, kScheme, kSiteA).has_value());

  auto paired = cache_->WriteHeadersSidecar(kUrl, kHost, kScheme, headers,
                                            kSiteA, Hash(2));
  ASSERT_TRUE(paired.has_value());
  EXPECT_TRUE(paired->stored);
  auto read = cache_->ReadHeadersSidecar(kUrl, kHost, kScheme, kSiteA);
  ASSERT_TRUE(read.has_value());
  auto block =
      ParseHeadersSidecarBlock(PageSpeedCache::HeadersSidecarPayload(*read));
  ASSERT_TRUE(block.has_value());
  EXPECT_TRUE(block->fields.empty());
  EXPECT_TRUE(block->pairing_hash.has_value());
}

TEST_F(SiteHtmlStoreTest, ARefusalWritesNothingEvenWhenPaired) {
  const std::vector<HeaderField> headers = {{"Set-Cookie", "a=b"}};
  auto written = cache_->WriteHeadersSidecar(kUrl, kHost, kScheme, headers,
                                             kSiteA, Hash(3));
  ASSERT_TRUE(written.has_value());
  EXPECT_FALSE(written->stored);
  EXPECT_FALSE(
      cache_->ReadHeadersSidecar(kUrl, kHost, kScheme, kSiteA).has_value());
}

TEST_F(SiteHtmlStoreTest, AMalformedPairingHashIsRefused) {
  const std::vector<HeaderField> headers = {{"Referrer-Policy", "x"}};
  const std::array<std::byte, 32> zero{};
  EXPECT_FALSE(
      cache_->WriteHeadersSidecar(kUrl, kHost, kScheme, headers, kSiteA, zero)
          .has_value());
  const std::array<std::byte, 31> short_hash{std::byte{1}};
  EXPECT_FALSE(cache_
                   ->WriteHeadersSidecar(kUrl, kHost, kScheme, headers, kSiteA,
                                         short_hash)
                   .has_value());
}

// ------------------------------------------------------------ withheld marker

TEST_F(SiteHtmlStoreTest, NoSelectionReadEverReturnsTheMarker) {
  WriteMarker(kSiteA);
  ASSERT_TRUE(Exists(kSiteA, kMarkerId));
  // Only the marker is stored: every selection read misses rather than
  // handing it out as a page, with and without the agent flag.
  for (bool agent : {false, true}) {
    SCOPED_TRACE(agent);
    EXPECT_FALSE(cache_
                     ->ReadBestAlternate(kUrl, kHost, kScheme, CapabilityMask(),
                                         agent, kSiteA)
                     .has_value());
    EXPECT_FALSE(cache_
                     ->ReadBestAlternateAgent(kUrl, kHost, kScheme,
                                              CapabilityMask(), agent, kSiteA)
                     .has_value());
  }
  // And no exact-id reader of another class returns it.
  EXPECT_FALSE(cache_->ReadAlternate(kUrl, kHost, kScheme, kMarkdownId, kSiteA)
                   .has_value());
  EXPECT_FALSE(
      cache_->ReadOriginalAlternate(kUrl, kHost, kScheme, kSiteA).has_value());
  EXPECT_FALSE(
      cache_->ReadHeadersSidecar(kUrl, kHost, kScheme, kSiteA).has_value());
  // Asked for by its own id, it is there.
  EXPECT_TRUE(cache_->ReadAlternate(kUrl, kHost, kScheme, kMarkerId, kSiteA)
                  .has_value());
}

TEST_F(SiteHtmlStoreTest, BesideAMarkdownCopyTheAgentReadGetsTheCopy) {
  WriteMarkdown(kSiteA, "# title");
  WriteMarker(kSiteA);
  auto read =
      cache_->ReadBestAlternate(kUrl, kHost, kScheme, CapabilityMask(),
                                /*agent_request_entitled=*/true, kSiteA);
  ASSERT_TRUE(read.has_value());
  EXPECT_EQ(read->metadata.full_mask & 0xFF,
            static_cast<uint32_t>(SentinelId::kAgentMarkdown));
}

TEST_F(SiteHtmlStoreTest, ARestoreOfTheOriginalClearsTheMarker) {
  const auto h = Hash(4);
  WriteOriginal(kSiteA, "<html>v1</html>", &h);
  WriteMarker(kSiteA);
  WriteMarker(kSiteB);
  ASSERT_TRUE(Exists(kSiteA, kMarkerId));

  // Same bytes, same hash: still cleared — the marker records a refusal of
  // the copy that was read, not a property of the page.
  WriteOriginal(kSiteA, "<html>v1</html>", &h);
  EXPECT_FALSE(Exists(kSiteA, kMarkerId));
  EXPECT_TRUE(
      Exists(kSiteA, static_cast<AlternateId>(SentinelId::kOriginalContent)));
  // Another site's marker is another key.
  EXPECT_TRUE(Exists(kSiteB, kMarkerId));
}

TEST_F(SiteHtmlStoreTest, TheOriginalStampSurvivesReadsOnEveryHandle) {
  const auto h = Hash(5);
  WriteOriginal(kSiteA, "<html>stamped</html>", &h);
  // Twice on the writing handle (the second read can come from its RAM
  // tier), then on a fresh handle that has only the volume.
  for (int i = 0; i < 2; ++i) {
    auto read = cache_->ReadOriginalAlternate(kUrl, kHost, kScheme, kSiteA);
    ASSERT_TRUE(read.has_value());
    EXPECT_EQ(read->metadata.origin_html_hash, h);
  }
  auto other = Open();
  ASSERT_NE(other, nullptr);
  auto read = other->ReadOriginalAlternate(kUrl, kHost, kScheme, kSiteA);
  ASSERT_TRUE(read.has_value());
  EXPECT_EQ(read->metadata.origin_html_hash, h);
}

// ------------------------------------------------------------ purge epoch

constexpr uint64_t kNow = 1'800'000'000;

TEST_F(SiteHtmlStoreTest, TheEpochRoundTripsPerSite) {
  auto none = cache_->ReadSitePurgeEpoch(kSiteA);
  ASSERT_TRUE(none.has_value());
  EXPECT_EQ(*none, 0u);

  ASSERT_TRUE(cache_->WriteSitePurgeEpoch(kSiteA, kNow - 100, kNow));
  EXPECT_EQ(cache_->ReadSitePurgeEpoch(kSiteA).value(), kNow - 100);
  EXPECT_EQ(cache_->ReadSitePurgeEpoch(kSiteB).value(), 0u)
      << "another site's epoch moved";

  ASSERT_TRUE(cache_->WriteSitePurgeEpoch(kSiteB, kNow - 7, kNow));
  EXPECT_EQ(cache_->ReadSitePurgeEpoch(kSiteA).value(), kNow - 100);
  EXPECT_EQ(cache_->ReadSitePurgeEpoch(kSiteB).value(), kNow - 7);

  auto other = Open();
  ASSERT_NE(other, nullptr);
  EXPECT_EQ(other->ReadSitePurgeEpoch(kSiteA).value(), kNow - 100);
}

TEST_F(SiteHtmlStoreTest, ALowerEpochIsASuccessfulNoOp) {
  ASSERT_TRUE(cache_->WriteSitePurgeEpoch(kSiteA, kNow - 10, kNow));
  EXPECT_TRUE(cache_->WriteSitePurgeEpoch(kSiteA, kNow - 500, kNow));
  EXPECT_TRUE(cache_->WriteSitePurgeEpoch(kSiteA, kNow - 10, kNow));
  EXPECT_EQ(cache_->ReadSitePurgeEpoch(kSiteA).value(), kNow - 10);
  ASSERT_TRUE(cache_->WriteSitePurgeEpoch(kSiteA, kNow, kNow));
  EXPECT_EQ(cache_->ReadSitePurgeEpoch(kSiteA).value(), kNow);
}

TEST_F(SiteHtmlStoreTest, AFutureEpochIsRefused) {
  const uint64_t skew = kSitePurgeEpochSkewSeconds;
  EXPECT_TRUE(cache_->WriteSitePurgeEpoch(kSiteA, kNow + skew, kNow));
  auto refused = cache_->WriteSitePurgeEpoch(kSiteA, kNow + skew + 1, kNow);
  ASSERT_FALSE(refused.has_value());
  EXPECT_EQ(refused.error(), cyclone::CacheError::InvalidArgument);
  EXPECT_EQ(cache_->ReadSitePurgeEpoch(kSiteA).value(), kNow + skew);
}

TEST_F(SiteHtmlStoreTest, OnlyASiteIdNamesAnEpoch) {
  const std::string upper(64, 'A');
  const std::string short_site = kSiteA.substr(1);
  for (std::string_view bad :
       {std::string_view(""), std::string_view("../../etc/passwd"),
        std::string_view(upper), std::string_view(short_site)}) {
    SCOPED_TRACE(std::string(bad));
    EXPECT_FALSE(cache_->WriteSitePurgeEpoch(bad, kNow, kNow).has_value());
    EXPECT_FALSE(cache_->ReadSitePurgeEpoch(bad).has_value());
  }
}

TEST_F(SiteHtmlStoreTest, TheEpochIsAFileBesideTheVolume) {
  ASSERT_TRUE(cache_->WriteSitePurgeEpoch(kSiteA, kNow - 3, kNow));
  const std::string path = SitePurgeEpochPath(cache_path_, kSiteA);
  EXPECT_EQ(path, cache_path_ + ".purge-epoch." + kSiteA);
  std::ifstream in(path, std::ios::binary);
  std::string content((std::istreambuf_iterator<char>(in)),
                      std::istreambuf_iterator<char>());
  EXPECT_EQ(content, RenderSitePurgeEpoch(kNow - 3));
  EXPECT_EQ(content.size(), 27u);
  EXPECT_EQ(content.substr(0, 6), "pspe1 ");
}

TEST_F(SiteHtmlStoreTest, RemovingTheUrlLeavesTheEpoch) {
  WriteOriginal(kSiteA, "<html>x</html>");
  ASSERT_TRUE(cache_->WriteSitePurgeEpoch(kSiteA, kNow - 1, kNow));
  ASSERT_TRUE(cache_->Remove(kUrl, kHost, kScheme, kSiteA));
  EXPECT_EQ(cache_->ReadSitePurgeEpoch(kSiteA).value(), kNow - 1);
}

TEST_F(SiteHtmlStoreTest, ATornRecordFailsClosedAndTheNextWriteRepairs) {
  ASSERT_TRUE(cache_->WriteSitePurgeEpoch(kSiteA, kNow - 100, kNow));
  const std::string path = SitePurgeEpochPath(cache_path_, kSiteA);
  const std::string whole = RenderSitePurgeEpoch(kNow - 10);
  // A writer that died part way through its in-place write: every prefix of
  // the record reads as an error, never as a value.
  for (size_t cut = 1; cut < whole.size(); ++cut) {
    SCOPED_TRACE(cut);
    {
      std::ofstream out(path, std::ios::binary | std::ios::trunc);
      out << whole.substr(0, cut);
    }
    auto read = cache_->ReadSitePurgeEpoch(kSiteA);
    ASSERT_FALSE(read.has_value());
    EXPECT_EQ(read.error(), cyclone::CacheError::Corrupted);
  }
  // An empty file is a creator that has not written yet: nothing recorded.
  {
    std::ofstream(path, std::ios::binary | std::ios::trunc);
  }
  EXPECT_EQ(cache_->ReadSitePurgeEpoch(kSiteA).value(), 0u);
  {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out << whole.substr(0, 9);
  }
  // The next write, even of a lower value, repairs a torn one fail-closed.
  ASSERT_TRUE(cache_->WriteSitePurgeEpoch(kSiteA, kNow - 500, kNow));
  EXPECT_EQ(cache_->ReadSitePurgeEpoch(kSiteA).value(), kNow);
  ASSERT_TRUE(cache_->WriteSitePurgeEpoch(kSiteA, kNow + 2, kNow + 2));
  EXPECT_EQ(cache_->ReadSitePurgeEpoch(kSiteA).value(), kNow + 2);
}

TEST_F(SiteHtmlStoreTest, ATornOverwriteNeverReadsBelowTheOldValue) {
  // A power loss part way through overwriting a valid record leaves the new
  // value's leading bytes over the old value's trailing ones.
  const uint64_t old_value = kNow - 1'000'001;
  const uint64_t new_value = kNow - 2;
  const std::string path = SitePurgeEpochPath(cache_path_, kSiteA);
  const std::string old_rec = RenderSitePurgeEpoch(old_value);
  const std::string new_rec = RenderSitePurgeEpoch(new_value);
  ASSERT_EQ(old_rec.size(), new_rec.size());
  for (size_t cut = 0; cut <= new_rec.size(); ++cut) {
    SCOPED_TRACE(cut);
    {
      std::ofstream out(path, std::ios::binary | std::ios::trunc);
      out << new_rec.substr(0, cut) << old_rec.substr(cut);
    }
    // Always a whole record: a value at or above the old one, never below
    // what was acknowledged.
    auto read = cache_->ReadSitePurgeEpoch(kSiteA);
    ASSERT_TRUE(read.has_value());
    EXPECT_GE(*read, old_value);
  }
}

TEST_F(SiteHtmlStoreTest, AStoredFutureValueIsKeptAndNeverLowered) {
  // A value an hour ahead (a write made while the clock ran fast, or a torn
  // overwrite) is a valid epoch: it reads as itself, and a lower write leaves
  // it alone. Lowering it would un-purge entries stored under the fast clock.
  const std::string path = SitePurgeEpochPath(cache_path_, kSiteA);
  {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out << RenderSitePurgeEpoch(kNow + 3600);
  }
  EXPECT_EQ(cache_->ReadSitePurgeEpoch(kSiteA).value(), kNow + 3600);
  ASSERT_TRUE(cache_->WriteSitePurgeEpoch(kSiteA, kNow - 100, kNow));
  ASSERT_TRUE(cache_->WriteSitePurgeEpoch(kSiteA, kNow, kNow));
  EXPECT_EQ(cache_->ReadSitePurgeEpoch(kSiteA).value(), kNow + 3600);
}

TEST_F(SiteHtmlStoreTest, AnUnreadableFileIsAnErrorNeverZero) {
  // A directory where the file should be: the open or the read fails.
  const std::string path = SitePurgeEpochPath(cache_path_, kSiteA);
  std::filesystem::create_directories(path);
  auto read = cache_->ReadSitePurgeEpoch(kSiteA);
  ASSERT_FALSE(read.has_value());
  EXPECT_FALSE(cache_->WriteSitePurgeEpoch(kSiteA, kNow, kNow).has_value());
}

TEST_F(SiteHtmlStoreTest, AReaderHoldingTheFileOpenDoesNotBlockAWriter) {
  ASSERT_TRUE(cache_->WriteSitePurgeEpoch(kSiteA, kNow - 100, kNow));
  const std::string path = SitePurgeEpochPath(cache_path_, kSiteA);
  std::ifstream held(path, std::ios::binary);
  ASSERT_TRUE(held.is_open());
  ASSERT_TRUE(cache_->WriteSitePurgeEpoch(kSiteA, kNow - 5, kNow));
  EXPECT_EQ(cache_->ReadSitePurgeEpoch(kSiteA).value(), kNow - 5);
  // The open handle sees the same, updated file (it was written in place).
  std::string content((std::istreambuf_iterator<char>(held)),
                      std::istreambuf_iterator<char>());
  EXPECT_EQ(content, RenderSitePurgeEpoch(kNow - 5));
}

TEST_F(SiteHtmlStoreTest, ANonAsciiVolumePathWorks) {
  // UTF-8 bytes for "caché-ñ": the path the API takes is UTF-8.
  const std::filesystem::path dir =
      std::filesystem::path(temp_dir_) /
      std::filesystem::path(u8"cach\u00e9-\u00f1");
  std::filesystem::create_directories(dir);
  const std::string volume = temp_dir_ + "/cach\xc3\xa9-\xc3\xb1/cache.vol";
  ASSERT_EQ(ReadSitePurgeEpochFile(volume, kSiteA).value(), 0u);
  ASSERT_TRUE(WriteSitePurgeEpochFile(volume, kSiteA, kNow - 9, kNow));
  ASSERT_TRUE(WriteSitePurgeEpochFile(volume, kSiteA, kNow - 99, kNow));
  EXPECT_EQ(ReadSitePurgeEpochFile(volume, kSiteA).value(), kNow - 9);
  EXPECT_TRUE(std::filesystem::exists(
      dir /
      std::filesystem::path(std::string("cache.vol.purge-epoch.") + kSiteA)));
}

TEST_F(SiteHtmlStoreTest, AnUnparseableFileIsAnErrorNeverZero) {
  const std::string path = SitePurgeEpochPath(cache_path_, kSiteA);
  for (std::string_view content :
       {std::string_view("pspe1 123"), std::string_view("pspe1 \n"),
        std::string_view("pspe1 12x\n"), std::string_view("pspe1 012\n"),
        std::string_view("pspe2 12\n"), std::string_view("pspe1 12\nextra"),
        std::string_view("pspe1 0000000000000000001x\n"),
        std::string_view("pspe1 00000000000000000001\nx")}) {
    SCOPED_TRACE(std::string(content));
    {
      std::ofstream out(path, std::ios::binary | std::ios::trunc);
      out << content;
    }
    auto read = cache_->ReadSitePurgeEpoch(kSiteA);
    ASSERT_FALSE(read.has_value()) << "an unreadable epoch read as a value";
    EXPECT_EQ(read.error(), cyclone::CacheError::Corrupted);
  }
  // A write heals it fail-closed: the larger of its value and now.
  ASSERT_TRUE(cache_->WriteSitePurgeEpoch(kSiteA, kNow - 500, kNow));
  EXPECT_EQ(cache_->ReadSitePurgeEpoch(kSiteA).value(), kNow);
}

TEST_F(SiteHtmlStoreTest, RacingThreadsOnSeparateHandlesLeaveTheLarger) {
  // Values 32 s apart: the pair a slot-per-residue design would lose.
  auto other = Open();
  ASSERT_NE(other, nullptr);
  constexpr int kRounds = 200;
  for (int round = 0; round < kRounds; ++round) {
    const uint64_t base = kNow - 100'000 + static_cast<uint64_t>(round) * 100;
    PageSpeedCache* low = (round % 2 == 0) ? cache_.get() : other.get();
    PageSpeedCache* high = (round % 2 == 0) ? other.get() : cache_.get();
    std::atomic<int> ready{0};
    auto run = [&ready](PageSpeedCache* c, uint64_t value) {
      ready.fetch_add(1);
      while (ready.load() < 2) {
      }
      EXPECT_TRUE(c->WriteSitePurgeEpoch(kSiteA, value, kNow).has_value());
    };
    std::thread t1(run, low, base);
    std::thread t2(run, high, base + 32);
    t1.join();
    t2.join();
    ASSERT_EQ(cache_->ReadSitePurgeEpoch(kSiteA).value(), base + 32)
        << "round " << round;
  }
}

// ------------------------------------------------------------ RAM tier
//
// The fixture's handles run with the RAM tier on (the cache's default), so
// a second read of an entry on the same handle is answered from it.

TEST_F(SiteHtmlStoreTest, RemoveEvictsThisProcesssRamCopies) {
  const auto h = Hash(6);
  WriteOriginal(kSiteA, "<html>ram</html>", &h);
  WriteMarkdown(kSiteA, "# ram");
  WriteMarker(kSiteA);
  for (int i = 0; i < 2; ++i) {  // populate the RAM tier
    ASSERT_TRUE(cache_->ReadOriginalAlternate(kUrl, kHost, kScheme, kSiteA));
    ASSERT_TRUE(
        cache_->ReadAlternate(kUrl, kHost, kScheme, kMarkdownId, kSiteA));
    ASSERT_TRUE(cache_->ReadAlternate(kUrl, kHost, kScheme, kMarkerId, kSiteA));
  }
  ASSERT_TRUE(cache_->Remove(kUrl, kHost, kScheme, kSiteA));
  EXPECT_FALSE(cache_->ReadOriginalAlternate(kUrl, kHost, kScheme, kSiteA));
  EXPECT_FALSE(
      cache_->ReadAlternate(kUrl, kHost, kScheme, kMarkdownId, kSiteA));
  EXPECT_FALSE(cache_->ReadAlternate(kUrl, kHost, kScheme, kMarkerId, kSiteA));
}

TEST_F(SiteHtmlStoreTest, StampPairingAndMarkerHoldThroughTheRamTier) {
  const auto h1 = Hash(7);
  const auto h2 = Hash(8);
  const std::vector<HeaderField> headers = {{"Referrer-Policy", "x"}};
  ASSERT_TRUE(
      cache_->WriteHeadersSidecar(kUrl, kHost, kScheme, headers, kSiteA, h1));
  WriteOriginal(kSiteA, "<html>1</html>", &h1);
  WriteMarker(kSiteA);
  for (int i = 0; i < 3; ++i) {  // the later reads come from RAM
    auto orig = cache_->ReadOriginalAlternate(kUrl, kHost, kScheme, kSiteA);
    ASSERT_TRUE(orig.has_value());
    EXPECT_EQ(orig->metadata.origin_html_hash, h1);
    auto side = cache_->ReadHeadersSidecar(kUrl, kHost, kScheme, kSiteA);
    ASSERT_TRUE(side.has_value());
    auto block =
        ParseHeadersSidecarBlock(PageSpeedCache::HeadersSidecarPayload(*side));
    ASSERT_TRUE(block.has_value());
    ASSERT_TRUE(block->pairing_hash.has_value());
    EXPECT_EQ(*block->pairing_hash, h1);
    ASSERT_TRUE(cache_->ReadAlternate(kUrl, kHost, kScheme, kMarkerId, kSiteA));
  }
  // Re-store on the same handle: the RAM copies of the old sidecar, the old
  // original and the marker must not answer afterwards.
  ASSERT_TRUE(
      cache_->WriteHeadersSidecar(kUrl, kHost, kScheme, headers, kSiteA, h2));
  WriteOriginal(kSiteA, "<html>2</html>", &h2);
  auto orig = cache_->ReadOriginalAlternate(kUrl, kHost, kScheme, kSiteA);
  ASSERT_TRUE(orig.has_value());
  EXPECT_EQ(orig->metadata.origin_html_hash, h2);
  auto side = cache_->ReadHeadersSidecar(kUrl, kHost, kScheme, kSiteA);
  ASSERT_TRUE(side.has_value());
  auto block =
      ParseHeadersSidecarBlock(PageSpeedCache::HeadersSidecarPayload(*side));
  ASSERT_TRUE(block.has_value());
  EXPECT_EQ(*block->pairing_hash, h2);
  EXPECT_FALSE(cache_->ReadAlternate(kUrl, kHost, kScheme, kMarkerId, kSiteA))
      << "the marker survived a re-store in this process's RAM tier";
}

// ------------------------------------------------------------ processes

// Real processes on every platform: the test binary runs itself as a child
// (SitePurgeEpochChild.Write, inert unless the spec variable is set), so the
// lock and replace path of each OS — fcntl and rename on POSIX, LockFileEx
// and MoveFileExW on Windows — is exercised across process boundaries.
constexpr char kChildSpecVar[] = "PS_EPOCH_CHILD_SPEC";

std::string SelfExecutable() {
#ifdef _WIN32
  char buf[MAX_PATH * 4];
  const DWORD n = GetModuleFileNameA(nullptr, buf, sizeof(buf));
  return std::string(buf, n);
#elif defined(__APPLE__)
  char buf[4096];
  uint32_t size = sizeof(buf);
  if (_NSGetExecutablePath(buf, &size) != 0) return std::string();
  return std::string(buf);
#else
  std::error_code ec;
  return std::filesystem::read_symlink("/proc/self/exe", ec).string();
#endif
}

// The child side. Spec file: volume path, now, go-file path, then one value
// per line.
TEST(SitePurgeEpochChild, Write) {
  const char* spec_path = std::getenv(kChildSpecVar);
  if (spec_path == nullptr) GTEST_SKIP() << "runs only as a child process";
  std::ifstream spec(spec_path);
  std::string volume;
  std::string go;
  uint64_t now = 0;
  ASSERT_TRUE(std::getline(spec, volume));
  ASSERT_TRUE(spec >> now);
  spec.ignore();
  ASSERT_TRUE(std::getline(spec, go));
  std::vector<uint64_t> values;
  for (uint64_t v = 0; spec >> v;) values.push_back(v);
  ASSERT_FALSE(values.empty());
  // Start together: wait for the parent's go file.
  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds(60);
  while (!std::filesystem::exists(go)) {
    ASSERT_LT(std::chrono::steady_clock::now(), deadline);
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  for (uint64_t v : values) {
    auto written = WriteSitePurgeEpochFile(volume, kSiteA, v, now);
    ASSERT_TRUE(written.has_value()) << "write of " << v << " failed";
  }
}

class SitePurgeEpochChildProcessTest : public ::testing::Test {
 protected:
  void SetUp() override {
    temp_dir_ = pagespeed::test::MakeTempDir();
    volume_ = temp_dir_ + "/cache.vol";
    self_ = SelfExecutable();
    ASSERT_FALSE(self_.empty());
  }
  void TearDown() override { std::filesystem::remove_all(temp_dir_); }

  // One child process per entry of `values`, all released at once; each
  // must exit 0.
  void RunChildren(const std::vector<std::vector<uint64_t>>& values,
                   int round) {
    const std::string go = temp_dir_ + "/go-" + std::to_string(round);
    std::vector<std::string> commands;
    for (size_t i = 0; i < values.size(); ++i) {
      const std::string spec = temp_dir_ + "/spec-" + std::to_string(round) +
                               "-" + std::to_string(i);
      {
        std::ofstream out(spec);
        out << volume_ << "\n" << kNow << "\n" << go << "\n";
        for (uint64_t v : values[i]) out << v << "\n";
      }
#ifdef _WIN32
      // std::system runs cmd.exe /c; `set X=y&& cmd` sets the variable for
      // that one child only. The test runner's own variables are cleared so a
      // child neither writes the parent's result file nor removes its
      // premature-exit marker.
      commands.push_back(std::string("set TEST_PREMATURE_EXIT_FILE=&& ") +
                         "set XML_OUTPUT_FILE=&& set GTEST_OUTPUT=&& set " +
                         kChildSpecVar + "=" + spec + "&& \"" + self_ +
                         "\" --gtest_filter=SitePurgeEpochChild.Write" +
                         " >nul 2>&1");
#else
      commands.push_back(
          std::string("env -u TEST_PREMATURE_EXIT_FILE -u XML_OUTPUT_FILE ") +
          "-u GTEST_OUTPUT " + kChildSpecVar + "='" + spec + "' '" + self_ +
          "' --gtest_filter=SitePurgeEpochChild.Write >/dev/null 2>&1");
#endif
    }
    std::vector<int> results(commands.size(), -1);
    std::vector<std::thread> threads;
    threads.reserve(commands.size());
    for (size_t i = 0; i < commands.size(); ++i) {
      threads.emplace_back([&results, &commands, i] {
        results[i] = std::system(commands[i].c_str());
      });
    }
    // Give every child time to start and reach the gate, then open it.
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    {
      std::ofstream(go) << "go";
    }
    for (auto& t : threads) t.join();
    for (size_t i = 0; i < results.size(); ++i) {
      EXPECT_EQ(results[i], 0) << "child " << i << " failed: " << commands[i];
    }
  }

  std::string temp_dir_;
  std::string volume_;
  std::string self_;
};

TEST_F(SitePurgeEpochChildProcessTest, TwoProcessesThirtyTwoSecondsApart) {
  for (int round = 0; round < 10; ++round) {
    SCOPED_TRACE(round);
    const uint64_t base = kNow - 100'000 + static_cast<uint64_t>(round) * 100;
    if (round % 2 == 0) {
      RunChildren({{base}, {base + 32}}, round);
    } else {
      RunChildren({{base + 32}, {base}}, round);
    }
    ASSERT_EQ(ReadSitePurgeEpochFile(volume_, kSiteA).value(), base + 32);
  }
}

TEST_F(SitePurgeEpochChildProcessTest, ManyProcessesOfRandomValuesLeaveTheMax) {
  std::mt19937_64 rng(777);
  std::uniform_int_distribution<uint64_t> dist(kNow - 50'000, kNow);
  std::vector<std::vector<uint64_t>> values(4);
  uint64_t max = 0;
  for (auto& mine : values) {
    for (int i = 0; i < 200; ++i) {
      mine.push_back(dist(rng));
      max = std::max(max, mine.back());
    }
  }
  RunChildren(values, 0);
  EXPECT_EQ(ReadSitePurgeEpochFile(volume_, kSiteA).value(), max);
}

#ifndef _WIN32
// Real processes, not handles: each child writes through its own lock and
// file descriptors, which is the case the OS advisory lock exists for.
class SitePurgeEpochProcessTest : public ::testing::Test {
 protected:
  void SetUp() override {
    temp_dir_ = pagespeed::test::MakeTempDir();
    volume_ = temp_dir_ + "/cache.vol";
  }
  void TearDown() override { std::filesystem::remove_all(temp_dir_); }

  // Forks one child per entry of `values`; every child waits on `gate`,
  // writes its values in order, and exits 0 only if every write succeeded.
  void RunChildren(const std::vector<std::vector<uint64_t>>& values) {
    int gate[2];
    ASSERT_EQ(::pipe(gate), 0);
    std::vector<pid_t> children;
    for (const auto& mine : values) {
      const pid_t pid = ::fork();
      ASSERT_GE(pid, 0);
      if (pid == 0) {
        ::close(gate[1]);
        char c;
        (void)::read(gate[0], &c, 1);  // released when the parent closes
        int rc = 0;
        for (uint64_t v : mine) {
          if (!WriteSitePurgeEpochFile(volume_, kSiteA, v, kNow).has_value()) {
            rc = 1;
          }
        }
        ::_exit(rc);
      }
      children.push_back(pid);
    }
    ::close(gate[0]);
    ::close(gate[1]);  // EOF releases every child at once
    for (pid_t pid : children) {
      int status = 0;
      ASSERT_EQ(::waitpid(pid, &status, 0), pid);
      ASSERT_TRUE(WIFEXITED(status));
      EXPECT_EQ(WEXITSTATUS(status), 0) << "a child's write failed";
    }
  }

  std::string temp_dir_;
  std::string volume_;
};

TEST_F(SitePurgeEpochProcessTest, TwoProcessesThirtyTwoSecondsApart) {
  for (int round = 0; round < 40; ++round) {
    SCOPED_TRACE(round);
    const uint64_t base = kNow - 100'000 + static_cast<uint64_t>(round) * 100;
    if (round % 2 == 0) {
      RunChildren({{base}, {base + 32}});
    } else {
      RunChildren({{base + 32}, {base}});
    }
    ASSERT_EQ(ReadSitePurgeEpochFile(volume_, kSiteA).value(), base + 32);
  }
}

TEST_F(SitePurgeEpochProcessTest, ManyProcessesOfRandomValuesLeaveTheMax) {
  std::mt19937_64 rng(12345);
  std::uniform_int_distribution<uint64_t> dist(kNow - 50'000, kNow);
  std::vector<std::vector<uint64_t>> values(6);
  uint64_t max = 0;
  for (auto& mine : values) {
    for (int i = 0; i < 150; ++i) {
      mine.push_back(dist(rng));
      max = std::max(max, mine.back());
    }
  }
  RunChildren(values);
  EXPECT_EQ(ReadSitePurgeEpochFile(volume_, kSiteA).value(), max);
}
#endif  // !_WIN32

}  // namespace
}  // namespace pagespeed
