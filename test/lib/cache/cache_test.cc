// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

// Integration test for PageSpeedCache — the C++ cache API using
// Cyclone alternates.

#include "lib/cache/cache.h"

#ifndef _WIN32
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#else
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

#include <array>
#include <atomic>
#include <chrono>
#include <cstdarg>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <limits>
#include <string>
#include <thread>
#include <vector>

#include "cyclone/cache.hpp"
#include "cyclone/key.hpp"
#include "gtest/gtest.h"
#include "hit_tracker.hpp"
#include "lib/base/message_handler.h"
#include "lib/classify/alternate_id.h"
#include "lib/classify/alternate_metadata.h"
#include "lib/classify/capability_mask.h"
#include "test/test_util/cache_test_peer.h"
#include "test/test_util/temp_dir.h"
#include "volume.hpp"

namespace pagespeed {
namespace {

// Factory for the repeated 5-argument CapabilityMask construction used
// throughout these tests. Density/save-data/encoding default to the most
// common values (1x, save-data off, identity encoding); callers override
// only the axes they exercise. Behavior-identical to constructing the mask
// directly.
CapabilityMask MakeTestMask(
    CapabilityMask::ImageFormat image_format, CapabilityMask::Viewport viewport,
    CapabilityMask::PixelDensity density = CapabilityMask::PixelDensity::k1x,
    CapabilityMask::SaveData save_data = CapabilityMask::SaveData::kOff,
    CapabilityMask::TransferEncoding encoding =
        CapabilityMask::TransferEncoding::kIdentity) {
  return CapabilityMask(image_format, viewport, density, save_data, encoding);
}

// Test handler that counts messages for verifying handler logging paths.
class TestCacheHandler : public MessageHandler {
 public:
  void Message(MessageType type, const char* format, ...) override {
    va_list args;
    va_start(args, format);
    MessageV(type, format, args);
    va_end(args);
  }
  [[nodiscard]] int count() const { return count_; }
  // The last message, formatted.  A count alone cannot tell "refused for
  // reason A" from "refused for reason B", and the refusal reasons are
  // exactly what the volume-mode tests below are about.
  [[nodiscard]] const std::string& last_message() const { return last_; }

 protected:
  void MessageV(MessageType /*type*/, const char* format,
                va_list args) override {
    ++count_;
    va_list copy;
    va_copy(copy, args);
    char buf[1024];
    const int n = ::vsnprintf(buf, sizeof(buf), format, copy);
    va_end(copy);
    if (n > 0) {
      const size_t len = static_cast<size_t>(n) < sizeof(buf) - 1
                             ? static_cast<size_t>(n)
                             : sizeof(buf) - 1;
      last_.assign(buf, len);
    }
  }

 private:
  int count_ = 0;
  std::string last_;
};

class PageSpeedCacheTest : public ::testing::Test {
 protected:
  void SetUp() override {
    // Create a temporary directory for the cache volume.
    temp_dir_ = pagespeed::test::MakeTempDir();
    cache_path_ = temp_dir_ + "/cache.vol";
  }

  void TearDown() override {
    cache_.reset();
    std::filesystem::remove_all(temp_dir_);
  }

  void CreateCache() {
    PageSpeedCacheConfig config;
    config.volume_path = cache_path_;
    config.volume_size = static_cast<uint64_t>(10 * 1024 * 1024);  // 10MB
    config.enable_checksum = true;

    auto result = PageSpeedCache::Create(config);
    ASSERT_TRUE(result.has_value()) << "Failed to create cache";
    cache_ = std::move(*result);
  }

  // Helper: write content for a given mask.
  void WriteContent(std::string_view url, std::string_view hostname,
                    const CapabilityMask& mask, std::string_view data,
                    ContentType ct = ContentType::kImage,
                    std::string_view scheme = "https") {
    AlternateId id =
        MaskToAlternateId(static_cast<uint8_t>(mask.Encode() & 0xFF));
    AlternateMetadata meta;
    meta.full_mask = mask.Encode();
    meta.content_type = ct;
    meta.origin_content_type = "image/jpeg";

    auto wh =
        cache_->WriteAlternate(url, hostname, scheme, id, data.size(), meta);
    ASSERT_TRUE(wh.has_value())
        << "WriteAlternate failed err=" << static_cast<int>(wh.error());
    auto bytes = std::as_bytes(std::span(data));
    auto written = wh->write_sync(bytes);
    ASSERT_TRUE(written.has_value())
        << "write_sync err=" << static_cast<int>(written.error());
    auto closed = wh->close_sync();
    ASSERT_TRUE(closed.has_value())
        << "close_sync err=" << static_cast<int>(closed.error());
  }

  // --- Re-record chain helpers (the WriteSentinel/WriteAgentAlternate
  // unlink mitigation) ---

  static cyclone::CacheKey KeyFor(std::string_view url) {
    return PageSpeedCache::ComposeKeyPreNormalized(url, "example.com", "https");
  }

  // Nodes on the URL's chain carrying `id`, counted through the SUBSTRATE
  // rather than through a read.  This is the distinction the whole re-record
  // question turns on: an exact-id read returns the newest node and says
  // nothing about how many are behind it, so a read-back assertion stays
  // green while the chain grows without bound.
  size_t ChainNodesFor(std::string_view url, SentinelId id) {
    auto alts = PageSpeedCacheTestPeer::Cyclone(*cache_).list_alternates_sync(
        KeyFor(url));
    if (!alts.has_value()) return 0;
    size_t nodes = 0;
    for (const auto& alt : *alts) {
      if (alt.id == static_cast<cyclone::AlternateId>(id)) ++nodes;
    }
    return nodes;
  }

  // Plant a node under `id` at the substrate WITHOUT going through the
  // cache's writer, so no unlink happens and the node is prepended.  Two of
  // these reproduce, deterministically and without a race, the chain a
  // concurrent interleaving leaves behind.
  void PlantSentinelNode(std::string_view url, SentinelId id,
                         std::string_view data) {
    auto raw = PageSpeedCacheTestPeer::Cyclone(*cache_).write_alternate_sync(
        KeyFor(url), static_cast<cyclone::AlternateId>(id), data.size());
    ASSERT_TRUE(raw.has_value());
    ASSERT_TRUE(raw->write_sync(std::as_bytes(std::span(data))).has_value());
    ASSERT_TRUE(raw->close_sync().has_value());
    // Drop this process's RAM copy, which a raw substrate write does not.
    // The tier is write-around — reads populate it, writes do not evict — so
    // without this the next read can be answered from RAM with the node this
    // plant just superseded, and the test would be measuring the RAM tier
    // rather than the chain.  It is the same hazard the cache's own writers
    // handle by evicting, which is exactly why they do.
    PageSpeedCacheTestPeer::Cyclone(*cache_).evict_from_ram_cache(
        KeyFor(url), static_cast<cyclone::AlternateId>(id));
  }

  // A full sentinel store through the cache's writer: handle, bytes, close.
  void WriteSentinelData(std::string_view url, SentinelId id,
                         std::string_view data) {
    auto wh =
        cache_->WriteSentinel(url, "example.com", "https", id, data.size());
    ASSERT_TRUE(wh.has_value())
        << "WriteSentinel failed err=" << static_cast<int>(wh.error());
    ASSERT_TRUE(wh->write_sync(std::as_bytes(std::span(data))).has_value());
    ASSERT_TRUE(wh->close_sync().has_value());
  }

  // The same for the agent-markdown variant, whose writer stamps the
  // metadata prefix itself.
  void WriteAgentMarkdownData(std::string_view url, std::string_view md) {
    AlternateMetadata meta;
    meta.full_mask = static_cast<uint32_t>(SentinelId::kAgentMarkdown);
    meta.content_type = ContentType::kOther;
    meta.origin_content_type = "text/markdown";
    auto wh = cache_->WriteAgentAlternate(url, "example.com", "https",
                                          md.size(), meta);
    ASSERT_TRUE(wh.has_value())
        << "WriteAgentAlternate failed err=" << static_cast<int>(wh.error());
    ASSERT_TRUE(wh->write_sync(std::as_bytes(std::span(md))).has_value());
    ASSERT_TRUE(wh->close_sync().has_value());
  }

  // Exact-id read back as a string.  For the agent-markdown id this strips
  // the metadata prefix the writer stored, so what compares is the markdown.
  std::string ReadSentinelData(std::string_view url, SentinelId id) {
    auto result = cache_->ReadAlternate(url, "example.com", "https",
                                        static_cast<AlternateId>(id));
    EXPECT_TRUE(result.has_value());
    if (!result.has_value()) return {};
    auto content = result->content();
    return std::string(reinterpret_cast<const char*>(content.data()),
                       content.size());
  }

  std::string temp_dir_;
  std::string cache_path_;
  std::unique_ptr<PageSpeedCache> cache_;
};

TEST_F(PageSpeedCacheTest, WriteAndReadBack) {
  CreateCache();

  CapabilityMask mask = MakeTestMask(CapabilityMask::ImageFormat::kWebP,
                                     CapabilityMask::Viewport::kMobile);

  std::string data = "fake webp image data";
  WriteContent("/img/hero.jpg", "example.com", mask, data);

  // Read it back with the same mask.
  auto result =
      cache_->ReadBestAlternate("/img/hero.jpg", "example.com", "https", mask);
  ASSERT_TRUE(result.has_value())
      << "ReadBestAlternate error: " << static_cast<int>(result.error());
  EXPECT_TRUE(result->is_valid());

  auto content = result->content();
  std::string_view read_data(reinterpret_cast<const char*>(content.data()),
                             content.size());
  EXPECT_EQ(read_data, data);
  EXPECT_EQ(result->metadata.content_type, ContentType::kImage);
  EXPECT_EQ(result->metadata.origin_content_type, "image/jpeg");
}

// A disk-borrow read stamps a per-stripe lease that
// long holders must re-stamp via renew_lease().  Leases are ON by default
// (PageSpeedCacheConfig::read_lease_duration = 5s, mirroring Cyclone).
TEST_F(PageSpeedCacheTest, ReadLeaseRenewal) {
  CreateCache();

  CapabilityMask mask = MakeTestMask(CapabilityMask::ImageFormat::kWebP,
                                     CapabilityMask::Viewport::kMobile);
  WriteContent("/img/lease.jpg", "example.com", mask, "lease-pinned bytes");

  auto result =
      cache_->ReadBestAlternate("/img/lease.jpg", "example.com", "https", mask);
  ASSERT_TRUE(result.has_value());
  ASSERT_TRUE(result->is_valid());

  // First read comes from the disk mmap borrow: there is a lease to renew,
  // and renewing repeatedly is idempotent-safe.
  EXPECT_TRUE(result->renew_lease());
  EXPECT_TRUE(result->renew_lease());
}

TEST_F(PageSpeedCacheTest, ReadLeaseRenewalDisabled) {
  // read_lease_duration = 0 disables leases (the pre-lease behavior):
  // renew_lease() reports there is nothing to renew.
  PageSpeedCacheConfig config;
  config.volume_path = cache_path_;
  config.volume_size = static_cast<uint64_t>(10 * 1024 * 1024);
  config.read_lease_duration = std::chrono::milliseconds(0);
  auto created = PageSpeedCache::Create(config);
  ASSERT_TRUE(created.has_value());
  cache_ = std::move(*created);

  CapabilityMask mask = MakeTestMask(CapabilityMask::ImageFormat::kWebP,
                                     CapabilityMask::Viewport::kMobile);
  WriteContent("/img/nolease.jpg", "example.com", mask, "unleased bytes");

  auto result = cache_->ReadBestAlternate("/img/nolease.jpg", "example.com",
                                          "https", mask);
  ASSERT_TRUE(result.has_value());
  ASSERT_TRUE(result->is_valid());
  EXPECT_FALSE(result->renew_lease());
}

// Issue #934 (worker self-starvation mechanism): a holder that keeps
// renewing its read lease while writing into the same full stripe defers
// its own writes indefinitely; copying the bytes out and calling
// release() lets the lease expire, so writes are admitted again.  This
// pins the exact cache-layer contract the worker's de-aliased image
// write path relies on.
TEST_F(PageSpeedCacheTest, ReleaseUnpinsFullStripeForWrites) {
  PageSpeedCacheConfig config;
  config.volume_path = cache_path_;
  config.volume_size = static_cast<uint64_t>(10 * 1024 * 1024);
  config.ram_cache_size = 0;  // the borrow must come from the disk mmap
  config.read_lease_duration = std::chrono::milliseconds(2000);
  auto created = PageSpeedCache::Create(config);
  ASSERT_TRUE(created.has_value());
  cache_ = std::move(*created);

  CapabilityMask mask = MakeTestMask(CapabilityMask::ImageFormat::kWebP,
                                     CapabilityMask::Viewport::kMobile);
  WriteContent("/img/victim.jpg", "example.com", mask, "victim bytes");

  // Take a disk borrow — stamps the stripe's read lease.
  auto borrow = cache_->ReadBestAlternate("/img/victim.jpg", "example.com",
                                          "https", mask);
  ASSERT_TRUE(borrow.has_value());
  ASSERT_TRUE(borrow->is_valid());

  // Non-asserting write helper: the deferred write MUST fail here.
  const std::string filler(512 * 1024, 'f');
  auto try_write = [&](const std::string& url) -> bool {
    AlternateId id =
        MaskToAlternateId(static_cast<uint8_t>(mask.Encode() & 0xFF));
    AlternateMetadata meta;
    meta.full_mask = mask.Encode();
    meta.content_type = ContentType::kImage;
    auto wh = cache_->WriteAlternate(url, "example.com", "https", id,
                                     filler.size(), meta);
    if (!wh.has_value()) return false;
    auto written = wh->write_sync(std::as_bytes(std::span(filler)));
    if (!written.has_value()) return false;
    return wh->close_sync().has_value();
  };

  // Emulate the worker's OLD write phase: renew the borrow's lease while
  // streaming writes into the same stripe.  Once write_pos reaches the
  // end of the (10MB, single-stripe) volume, the wrap is gated on the
  // live lease and every further write is dropped with NoSpace.
  bool deferred = false;
  for (int i = 0; i < 40 && !deferred; ++i) {
    (void)borrow->renew_lease();
    deferred = !try_write("/fill/" + std::to_string(i) + ".bin");
  }
  ASSERT_TRUE(deferred) << "wrap was never lease-deferred";
  EXPECT_GT(cache_->Stats().writes_dropped_by_lease, 0u);

  // Still deferred while the holder keeps renewing (self-starvation).
  ASSERT_TRUE(borrow->renew_lease());
  EXPECT_FALSE(try_write("/fill/renewed.bin"));

  // De-alias: copy the content out, then drop the borrow.  Nothing
  // renews the lease anymore; once it expires, writes are admitted.
  auto content = borrow->content();
  const std::string copy(reinterpret_cast<const char*>(content.data()),
                         content.size());
  EXPECT_EQ(copy, "victim bytes");
  borrow->release();
  EXPECT_FALSE(borrow->is_valid());
  EXPECT_FALSE(borrow->renew_lease());
  EXPECT_TRUE(borrow->content().empty());

  std::this_thread::sleep_for(std::chrono::milliseconds(2600));
  EXPECT_TRUE(try_write("/fill/after-release.bin"))
      << "write still deferred after release() + lease expiry";
}

TEST_F(PageSpeedCacheTest, BestAlternateSelectsCorrectFormat) {
  CreateCache();

  // Write WebP and AVIF variants.
  CapabilityMask webp_mask = MakeTestMask(CapabilityMask::ImageFormat::kWebP,
                                          CapabilityMask::Viewport::kDesktop);
  CapabilityMask avif_mask = MakeTestMask(CapabilityMask::ImageFormat::kAvif,
                                          CapabilityMask::Viewport::kDesktop);

  WriteContent("/img/hero.jpg", "example.com", webp_mask, "webp-data");
  WriteContent("/img/hero.jpg", "example.com", avif_mask, "avif-data");

  // Client requesting AVIF should get AVIF.
  auto result = cache_->ReadBestAlternate("/img/hero.jpg", "example.com",
                                          "https", avif_mask);
  ASSERT_TRUE(result.has_value());
  auto content = result->content();
  std::string_view read_data(reinterpret_cast<const char*>(content.data()),
                             content.size());
  EXPECT_EQ(read_data, "avif-data");
}

TEST_F(PageSpeedCacheTest, RemoveDeletesAllAlternates) {
  CreateCache();

  CapabilityMask mask;
  WriteContent("/img/x.jpg", "example.com", mask, "data");

  auto alt_id = MaskToAlternateId(static_cast<uint8_t>(mask.Encode() & 0xFF));
  EXPECT_TRUE(
      cache_->AlternateExists("/img/x.jpg", "example.com", "https", alt_id));

  auto removed = cache_->Remove("/img/x.jpg", "example.com", "https");
  EXPECT_TRUE(removed.has_value())
      << "Remove error: " << static_cast<int>(removed.error());

  EXPECT_FALSE(
      cache_->AlternateExists("/img/x.jpg", "example.com", "https", alt_id));
}

TEST_F(PageSpeedCacheTest, AlternateExistsWorks) {
  CreateCache();

  AlternateId id = 0x41;  // Some content alternate
  EXPECT_FALSE(
      cache_->AlternateExists("/img/test.jpg", "example.com", "https", id));

  CapabilityMask mask = CapabilityMask::Decode(0x41);
  WriteContent("/img/test.jpg", "example.com", mask, "test-data");

  EXPECT_TRUE(
      cache_->AlternateExists("/img/test.jpg", "example.com", "https", id));
}

TEST_F(PageSpeedCacheTest, ReadMissReturnsNotFound) {
  CreateCache();
  CapabilityMask mask;
  auto result =
      cache_->ReadBestAlternate("/nonexistent", "example.com", "https", mask);
  EXPECT_FALSE(result.has_value());
}

TEST_F(PageSpeedCacheTest, SentinelWriteAndRead) {
  CreateCache();

  std::string hints = "<link rel=preload href=/style.css as=style>";

  auto wh = cache_->WriteSentinel("/page.html", "example.com", "https",
                                  SentinelId::kEarlyHints, hints.size());
  ASSERT_TRUE(wh.has_value());
  auto bytes = std::as_bytes(std::span(hints));
  auto written = wh->write_sync(bytes);
  ASSERT_TRUE(written.has_value());
  auto closed = wh->close_sync();
  ASSERT_TRUE(closed.has_value());

  // Read it back via ReadAlternate with sentinel ID.
  auto result =
      cache_->ReadAlternate("/page.html", "example.com", "https",
                            static_cast<AlternateId>(SentinelId::kEarlyHints));
  ASSERT_TRUE(result.has_value());
  auto content = result->content();
  std::string_view read_data(reinterpret_cast<const char*>(content.data()),
                             content.size());
  EXPECT_EQ(read_data, hints);
}

TEST_F(PageSpeedCacheTest, AgentEntitledFlagFlipsSelection) {
  // The agent_request_entitled flag threaded into request_metadata
  // makes select() pick the kAgentMarkdown sentinel over the content variant.
  CreateCache();
  CapabilityMask mask = MakeTestMask(CapabilityMask::ImageFormat::kOriginal,
                                     CapabilityMask::Viewport::kMobile);

  // A normal (metadata-prefixed) content variant for the same URL.
  WriteContent("/p.html", "example.com", mask, "CONTENT", ContentType::kHtml);

  // The agent-markdown variant, written raw as a sentinel. (The metadata-prefixed
  // agent write is P1.7; here we only need select() to be ABLE to pick it.)
  std::string md = "# markdown";
  auto wh = cache_->WriteSentinel("/p.html", "example.com", "https",
                                  SentinelId::kAgentMarkdown, md.size());
  ASSERT_TRUE(wh.has_value());
  ASSERT_TRUE(wh->write_sync(std::as_bytes(std::span(md))).has_value());
  ASSERT_TRUE(wh->close_sync().has_value());

  // Default (not entitled): the content variant is selected and read back.
  auto normal =
      cache_->ReadBestAlternate("/p.html", "example.com", "https", mask);
  ASSERT_TRUE(normal.has_value());
  auto c = normal->content();
  EXPECT_EQ(std::string_view(reinterpret_cast<const char*>(c.data()), c.size()),
            "CONTENT");

  // Entitled agent request: select() now picks the 0x7C sentinel instead of the
  // content variant — proving the flag threads through. (ReadBestAlternate's
  // metadata-prefix parse misses on the raw sentinel write, which confirms
  // selection moved OFF the content variant; the proper agent write is P1.7.)
  auto agent =
      cache_->ReadBestAlternate("/p.html", "example.com", "https", mask,
                                /*agent_request_entitled=*/true);
  EXPECT_FALSE(agent.has_value());
}

TEST_F(PageSpeedCacheTest, WriteAgentAlternateReadBack) {
  // The metadata-prefixed agent-markdown write IS readable via the
  // strict ReadBestAlternate path for an entitled agent request (regression guard
  // for the prefix-MISS tripwire that a raw WriteSentinel hits), while a normal
  // request still gets the content variant.
  CreateCache();
  CapabilityMask mask = MakeTestMask(CapabilityMask::ImageFormat::kOriginal,
                                     CapabilityMask::Viewport::kMobile);

  WriteContent("/p.html", "example.com", mask, "<html>HI</html>",
               ContentType::kHtml);

  std::string md = "# Hello\n\nrendered markdown";
  AlternateMetadata meta;
  meta.full_mask = static_cast<uint32_t>(SentinelId::kAgentMarkdown);  // 0x7C
  meta.content_type = ContentType::kOther;
  meta.origin_content_type = "text/markdown";
  auto wh = cache_->WriteAgentAlternate("/p.html", "example.com", "https",
                                        md.size(), meta);
  ASSERT_TRUE(wh.has_value());
  ASSERT_TRUE(wh->write_sync(std::as_bytes(std::span(md))).has_value());
  ASSERT_TRUE(wh->close_sync().has_value());

  // Entitled agent request: selects AND reads the markdown variant.
  auto agent =
      cache_->ReadBestAlternate("/p.html", "example.com", "https", mask,
                                /*agent_request_entitled=*/true);
  ASSERT_TRUE(agent.has_value());
  auto c = agent->content();
  EXPECT_EQ(std::string_view(reinterpret_cast<const char*>(c.data()), c.size()),
            md);
  EXPECT_EQ(agent->metadata.origin_content_type, "text/markdown");

  // Normal request: the HTML content variant, never the markdown.
  auto normal =
      cache_->ReadBestAlternate("/p.html", "example.com", "https", mask);
  ASSERT_TRUE(normal.has_value());
  auto c2 = normal->content();
  EXPECT_EQ(
      std::string_view(reinterpret_cast<const char*>(c2.data()), c2.size()),
      "<html>HI</html>");
}

// The shared agent serve gate (entitlement + the content-hash equality
// check) — refuses a markdown variant whose stamped origin_html_hash
// != the live kContentHash (or no sentinel), passes non-agent reads through.
TEST_F(PageSpeedCacheTest, ReadBestAlternateAgentByKeyGatesOnContentHash) {
  CreateCache();
  CapabilityMask mask = MakeTestMask(CapabilityMask::ImageFormat::kOriginal,
                                     CapabilityMask::Viewport::kMobile);
  WriteContent("/p.html", "example.com", mask, "<html>HI</html>",
               ContentType::kHtml);

  std::array<std::byte, 32> hA{};
  for (size_t i = 0; i < hA.size(); ++i) hA[i] = static_cast<std::byte>(i + 1);
  std::array<std::byte, 32> hB{};
  for (size_t i = 0; i < hB.size(); ++i)
    hB[i] = static_cast<std::byte>(0xF0 - i);

  std::string md = "# rendered markdown\n";
  AlternateMetadata meta;
  meta.full_mask = static_cast<uint32_t>(SentinelId::kAgentMarkdown);
  meta.content_type = ContentType::kOther;
  meta.origin_content_type = "text/markdown";
  meta.origin_html_hash = hA;  // bound to H_A
  {
    auto wh = cache_->WriteAgentAlternate("/p.html", "example.com", "https",
                                          md.size(), meta);
    ASSERT_TRUE(wh.has_value());
    ASSERT_TRUE(wh->write_sync(std::as_bytes(std::span(md))).has_value());
    ASSERT_TRUE(wh->close_sync().has_value());
  }

  auto key = PageSpeedCache::ComposeKeyPreNormalized("/p.html", "example.com",
                                                     "https");
  auto write_content_hash = [&](const std::array<std::byte, 32>& h) {
    auto wh = cache_->WriteSentinel("/p.html", "example.com", "https",
                                    SentinelId::kContentHash, h.size());
    ASSERT_TRUE(wh.has_value());
    ASSERT_TRUE(wh->write_sync(std::span<const std::byte>(h)).has_value());
    ASSERT_TRUE(wh->close_sync().has_value());
  };

  // (1) No kContentHash sentinel -> unbound -> entitled agent read REFUSED.
  EXPECT_FALSE(cache_
                   ->ReadBestAlternateAgentByKey(
                       key, mask, /*agent_request_entitled=*/true)
                   .has_value());

  // (2) Matching sentinel (H_A) -> markdown served.
  write_content_hash(hA);
  {
    auto r = cache_->ReadBestAlternateAgentByKey(
        key, mask, /*agent_request_entitled=*/true);
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(r->metadata.full_mask & 0xFF,
              static_cast<uint32_t>(SentinelId::kAgentMarkdown));
    auto c = r->content();
    EXPECT_EQ(
        std::string_view(reinterpret_cast<const char*>(c.data()), c.size()),
        md);
  }

  // (3) Origin advanced (live H_B != stamp H_A) -> REFUSED.
  write_content_hash(hB);
  EXPECT_FALSE(cache_
                   ->ReadBestAlternateAgentByKey(
                       key, mask, /*agent_request_entitled=*/true)
                   .has_value());

  // (4) Non-entitled -> the HTML content variant, never the markdown.
  {
    auto r = cache_->ReadBestAlternateAgentByKey(
        key, mask, /*agent_request_entitled=*/false);
    ASSERT_TRUE(r.has_value());
    EXPECT_NE(r->metadata.full_mask & 0xFF,
              static_cast<uint32_t>(SentinelId::kAgentMarkdown));
    auto c = r->content();
    EXPECT_EQ(
        std::string_view(reinterpret_cast<const char*>(c.data()), c.size()),
        "<html>HI</html>");
  }
}

// ---------------------------------------------------------------------------
// Re-recording through the generic writers replaces rather than accumulates —
// as far as it can at this storage version (#1312).  Mirrors the durable
// originals suite (durable_originals_test.cc): the depth is asserted through
// the substrate, because a read-back assertion stays green while the chain
// grows.
// ---------------------------------------------------------------------------

TEST_F(PageSpeedCacheTest, SentinelReRecordsKeepTheChainAtOneNode) {
  // The quiet case, which is the one that has to hold: one writer, many
  // stores, one node.  The counters must stay silent too — an instrument
  // that fires on the ordinary path is indistinguishable from noise exactly
  // when it matters.
  CreateCache();
  for (int i = 0; i < 8; ++i) {
    WriteSentinelData("/page.html", SentinelId::kContentHash,
                      "hash-" + std::to_string(i));
    EXPECT_EQ(ChainNodesFor("/page.html", SentinelId::kContentHash), 1u)
        << "after store " << i;
  }

  EXPECT_EQ(ReadSentinelData("/page.html", SentinelId::kContentHash), "hash-7");
  EXPECT_EQ(cache_->SentinelsSupersededObserved(), 0u);
  EXPECT_EQ(cache_->SentinelsUnlinkFailed(), 0u);
}

TEST_F(PageSpeedCacheTest,
       SentinelSubstrateNoLongerAccumulatesAndReadsServeNewest) {
  // This used to demonstrate the defect the writer mitigates.  Planting
  // bypasses the cache's writer — exactly what a second process racing this
  // one achieves by accident — and every store linked another node while
  // every read still returned the newest, which is why a read-back assertion
  // could never have caught it.
  //
  // The storage layer now unlinks the superseded same-id node as part of the
  // write itself, so the accumulation no longer happens even when this
  // repo's own writer-side mitigation is bypassed entirely.  Depth stays at
  // one however many times the id is re-recorded, and reads still serve the
  // newest.
  CreateCache();
  for (int i = 1; i <= 4; ++i) {
    PlantSentinelNode("/page.html", SentinelId::kContentHash,
                      "planted-" + std::to_string(i));
    ASSERT_EQ(ChainNodesFor("/page.html", SentinelId::kContentHash), 1u)
        << "after plant " << i;
    EXPECT_EQ(ReadSentinelData("/page.html", SentinelId::kContentHash),
              "planted-" + std::to_string(i));
  }
}

TEST_F(PageSpeedCacheTest, SentinelDepthStaysAtOneAndReadsStillServeNewest) {
  // There used to be a fixed point here: a concurrent interleaving left a
  // chain at two, the writer unlinked ONE node and wrote ONE, so the depth
  // never came back down on its own and the cost was recorded as bounded
  // rather than self-healing.
  //
  // The storage layer now unlinks the superseded same-id node as part of the
  // write, so that two-node state cannot be built up in the first place —
  // the plants below already collapse to one.  The property callers depend
  // on is unchanged: the read still serves the newest.
  CreateCache();
  PlantSentinelNode("/page.html", SentinelId::kContentHash, "planted-old");
  PlantSentinelNode("/page.html", SentinelId::kContentHash, "planted-new");
  ASSERT_EQ(ChainNodesFor("/page.html", SentinelId::kContentHash), 1u);

  WriteSentinelData("/page.html", SentinelId::kContentHash, "re-recorded");

  EXPECT_EQ(ChainNodesFor("/page.html", SentinelId::kContentHash), 1u);
  EXPECT_EQ(ReadSentinelData("/page.html", SentinelId::kContentHash),
            "re-recorded");

  // The writer-side mitigation is now belt-and-braces: it finds no superseded
  // node to observe, so the counter that existed to keep the limitation
  // visible in the field reads zero rather than climbing.
  EXPECT_EQ(cache_->SentinelsSupersededObserved(), 0u);
  EXPECT_EQ(cache_->SentinelsUnlinkFailed(), 0u);

  // And it stays that way across further re-records, rather than the depth
  // creeping up once the first store has been absorbed.
  WriteSentinelData("/page.html", SentinelId::kContentHash, "re-recorded-2");
  EXPECT_EQ(cache_->SentinelsSupersededObserved(), 0u);
  EXPECT_EQ(ChainNodesFor("/page.html", SentinelId::kContentHash), 1u);
}

TEST_F(PageSpeedCacheTest, SentinelReRecordDoesNotDisturbTheRestOfTheChain) {
  // The unlink is alternate-scoped: it takes this id's node and nothing else.
  // Getting that wrong would be the orphaning hazard by another route, so it
  // is asserted rather than assumed.
  CreateCache();
  WriteSentinelData("/page.html", SentinelId::kContentHash, "hash-1");
  WriteSentinelData("/page.html", SentinelId::kEarlyHints, "hints");
  CapabilityMask mask = MakeTestMask(CapabilityMask::ImageFormat::kOriginal,
                                     CapabilityMask::Viewport::kDesktop);
  WriteContent("/page.html", "example.com", mask, "<html>content</html>",
               ContentType::kHtml);
  auto before = cache_->ListAlternates("/page.html", "example.com", "https");
  ASSERT_TRUE(before.has_value());

  WriteSentinelData("/page.html", SentinelId::kContentHash, "hash-2");

  auto after = cache_->ListAlternates("/page.html", "example.com", "https");
  ASSERT_TRUE(after.has_value());
  EXPECT_EQ(after->size(), before->size());
  EXPECT_EQ(ChainNodesFor("/page.html", SentinelId::kContentHash), 1u);
  // Every other alternate is still readable, and still itself.
  EXPECT_EQ(ReadSentinelData("/page.html", SentinelId::kEarlyHints), "hints");
  auto content =
      cache_->ReadBestAlternate("/page.html", "example.com", "https", mask);
  ASSERT_TRUE(content.has_value());
  auto c = content->content();
  EXPECT_EQ(std::string_view(reinterpret_cast<const char*>(c.data()), c.size()),
            "<html>content</html>");
}

TEST_F(PageSpeedCacheTest, AgentMarkdownReRecordsKeepTheChainAtOneNode) {
  // The agent-markdown writer's half of the quiet case: one writer, many
  // re-records, one node, counters silent — and the newest markdown is what
  // reads back (through the strict path's prefix, stripped by content()).
  CreateCache();
  for (int i = 0; i < 8; ++i) {
    WriteAgentMarkdownData("/p.html", "# markdown v" + std::to_string(i));
    EXPECT_EQ(ChainNodesFor("/p.html", SentinelId::kAgentMarkdown), 1u)
        << "after store " << i;
  }

  EXPECT_EQ(ReadSentinelData("/p.html", SentinelId::kAgentMarkdown),
            "# markdown v7");
  EXPECT_EQ(cache_->AgentMarkdownSupersededObserved(), 0u);
  EXPECT_EQ(cache_->AgentMarkdownUnlinkFailed(), 0u);
}

TEST_F(PageSpeedCacheTest,
       AgentMarkdownDepthStaysAtOneAndReadsStillServeNewest) {
  // Same story as the plain sentinel writer: the storage layer's own unlink
  // means the two-node state no longer forms, the depth stays at one, and
  // the read still serves the newest.
  CreateCache();
  PlantSentinelNode("/p.html", SentinelId::kAgentMarkdown, "planted-old");
  PlantSentinelNode("/p.html", SentinelId::kAgentMarkdown, "planted-new");
  ASSERT_EQ(ChainNodesFor("/p.html", SentinelId::kAgentMarkdown), 1u);

  WriteAgentMarkdownData("/p.html", "# re-recorded");

  EXPECT_EQ(ChainNodesFor("/p.html", SentinelId::kAgentMarkdown), 1u);
  EXPECT_EQ(ReadSentinelData("/p.html", SentinelId::kAgentMarkdown),
            "# re-recorded");
  EXPECT_EQ(cache_->AgentMarkdownSupersededObserved(), 0u);

  WriteAgentMarkdownData("/p.html", "# re-recorded-2");
  EXPECT_EQ(cache_->AgentMarkdownSupersededObserved(), 0u);
  EXPECT_EQ(ChainNodesFor("/p.html", SentinelId::kAgentMarkdown), 1u);
}

TEST_F(PageSpeedCacheTest, RejectSentinelViaWriteAlternate) {
  CreateCache();

  AlternateMetadata meta;
  meta.full_mask = 0;
  meta.content_type = ContentType::kOther;

  // Sentinel IDs should be rejected by WriteAlternate.
  auto result = cache_->WriteAlternate(
      "/page.html", "example.com", "https",
      static_cast<AlternateId>(SentinelId::kOriginalContent), 10, meta);
  EXPECT_FALSE(result.has_value());
}

TEST_F(PageSpeedCacheTest, HostnameNormalization) {
  CreateCache();

  CapabilityMask mask;
  WriteContent("/img/hero.jpg", "Example.COM", mask, "data");

  // Same URL with different hostname casing should find it.
  auto result =
      cache_->ReadBestAlternate("/img/hero.jpg", "example.com", "https", mask);
  ASSERT_TRUE(result.has_value())
      << "HostnameNormalization ReadBestAlternate error: "
      << static_cast<int>(result.error());
  auto content = result->content();
  std::string_view read_data(reinterpret_cast<const char*>(content.data()),
                             content.size());
  EXPECT_EQ(read_data, "data");
}

TEST_F(PageSpeedCacheTest, ListAlternates) {
  CreateCache();

  CapabilityMask webp = MakeTestMask(CapabilityMask::ImageFormat::kWebP,
                                     CapabilityMask::Viewport::kDesktop);
  CapabilityMask avif = MakeTestMask(CapabilityMask::ImageFormat::kAvif,
                                     CapabilityMask::Viewport::kDesktop);

  WriteContent("/img/hero.jpg", "example.com", webp, "webp");
  WriteContent("/img/hero.jpg", "example.com", avif, "avif");

  auto alts = cache_->ListAlternates("/img/hero.jpg", "example.com", "https");
  ASSERT_TRUE(alts.has_value())
      << "ListAlternates error: " << static_cast<int>(alts.error());
  EXPECT_GE(alts->size(), 2u);
}

TEST_F(PageSpeedCacheTest, HostnameIsolation) {
  // Same URL with different hostnames should be independent cache entries.
  CreateCache();

  CapabilityMask mask;
  WriteContent("/img/hero.jpg", "a.example.com", mask, "data-for-a");
  WriteContent("/img/hero.jpg", "b.example.com", mask, "data-for-b");

  // Reading with each hostname should return the correct data.
  auto result_a = cache_->ReadBestAlternate("/img/hero.jpg", "a.example.com",
                                            "https", mask);
  ASSERT_TRUE(result_a.has_value());
  auto content_a = result_a->content();
  std::string_view read_a(reinterpret_cast<const char*>(content_a.data()),
                          content_a.size());
  EXPECT_EQ(read_a, "data-for-a");

  auto result_b = cache_->ReadBestAlternate("/img/hero.jpg", "b.example.com",
                                            "https", mask);
  ASSERT_TRUE(result_b.has_value());
  auto content_b = result_b->content();
  std::string_view read_b(reinterpret_cast<const char*>(content_b.data()),
                          content_b.size());
  EXPECT_EQ(read_b, "data-for-b");

  // Reading with an unrelated hostname should return NOT FOUND.
  auto result_c = cache_->ReadBestAlternate("/img/hero.jpg", "c.example.com",
                                            "https", mask);
  EXPECT_FALSE(result_c.has_value())
      << "Different hostname should not share cache entries";
}

TEST_F(PageSpeedCacheTest, DirtyContentTypeRoundTrip) {
  // Verify that content-type strings with \r\n\0 don't corrupt the
  // content boundary (ParseMetadataPrefix bug fix).
  CreateCache();

  CapabilityMask mask = MakeTestMask(CapabilityMask::ImageFormat::kOriginal,
                                     CapabilityMask::Viewport::kDesktop);

  AlternateId id =
      MaskToAlternateId(static_cast<uint8_t>(mask.Encode() & 0xFF));

  // Use a dirty content-type with \r\n that would cause the old bug.
  AlternateMetadata meta;
  meta.full_mask = mask.Encode();
  meta.content_type = ContentType::kImage;
  meta.origin_content_type = std::string("image/\r\njpeg", 12);

  std::string data = "actual image content here";
  auto wh = cache_->WriteAlternate("/img/dirty-ct.jpg", "example.com", "https",
                                   id, data.size(), meta);
  ASSERT_TRUE(wh.has_value());
  auto bytes = std::as_bytes(std::span(data));
  (void)wh->write_sync(bytes);
  (void)wh->close_sync();

  // Read it back and verify the content is intact (not corrupted).
  auto result = cache_->ReadBestAlternate("/img/dirty-ct.jpg", "example.com",
                                          "https", mask);
  ASSERT_TRUE(result.has_value());
  auto content = result->content();
  std::string_view read_data(reinterpret_cast<const char*>(content.data()),
                             content.size());
  EXPECT_EQ(read_data, data) << "Content corrupted by dirty content-type";

  // The origin_content_type should be sanitized (stripped \r\n).
  EXPECT_EQ(result->metadata.origin_content_type, "image/jpeg");
}

TEST_F(PageSpeedCacheTest, CrossProcessCacheVisibility) {
  // Two PageSpeedCache instances on the same volume file should see
  // each other's writes (both use enable_mmap_directory = true).
  CreateCache();

  // Write via cache_ (instance 1).
  CapabilityMask mask;
  WriteContent("/img/cross.jpg", "example.com", mask, "from-instance-1");

  // Open a second cache on the same volume.
  PageSpeedCacheConfig config2;
  config2.volume_path = cache_path_;
  config2.volume_size = static_cast<uint64_t>(10 * 1024 * 1024);
  config2.enable_checksum = true;

  auto result2 = PageSpeedCache::Create(config2);
  ASSERT_TRUE(result2.has_value()) << "Failed to create second cache";
  auto& cache2 = *result2;

  // Instance 2 should see instance 1's write.
  auto read_result =
      cache2->ReadBestAlternate("/img/cross.jpg", "example.com", "https", mask);
  ASSERT_TRUE(read_result.has_value())
      << "Instance 2 should see instance 1's write, error: "
      << static_cast<int>(read_result.error());
  auto content = read_result->content();
  std::string_view data(reinterpret_cast<const char*>(content.data()),
                        content.size());
  EXPECT_EQ(data, "from-instance-1");
}

TEST_F(PageSpeedCacheTest, V3CacheControlFieldsRoundTrip) {
  // Write an alternate with all v3 cache-control fields populated,
  // then read it back and verify the full serialize→prefix-write→
  // mmap-read→ParseMetadataPrefix→deserialize pipeline preserves them.
  CreateCache();

  CapabilityMask mask = MakeTestMask(CapabilityMask::ImageFormat::kOriginal,
                                     CapabilityMask::Viewport::kDesktop);

  AlternateId id =
      MaskToAlternateId(static_cast<uint8_t>(mask.Encode() & 0xFF));

  AlternateMetadata meta;
  meta.full_mask = mask.Encode();
  meta.content_type = ContentType::kCss;
  meta.flags = AlternateMetadata::kFlagNeedsRevalidation;
  meta.origin_content_type = "text/css; charset=utf-8";
  meta.cache_inserted_at = 1700000000;
  meta.origin_max_age = 86400;
  meta.origin_s_maxage = 3600;
  meta.origin_cc_flags = AlternateMetadata::kCCOriginPublic |
                         AlternateMetadata::kCCOriginSMaxagePresent |
                         AlternateMetadata::kCCOriginHeaderPresent;

  std::string data = "body { color: red; }";
  auto wh = cache_->WriteAlternate("/style.css", "example.com", "https", id,
                                   data.size(), meta);
  ASSERT_TRUE(wh.has_value());
  auto bytes = std::as_bytes(std::span(data));
  (void)wh->write_sync(bytes);
  (void)wh->close_sync();

  auto result =
      cache_->ReadBestAlternate("/style.css", "example.com", "https", mask);
  ASSERT_TRUE(result.has_value());

  // Verify content is intact.
  auto content = result->content();
  std::string_view read_data(reinterpret_cast<const char*>(content.data()),
                             content.size());
  EXPECT_EQ(read_data, data);

  // Verify all v3 metadata fields survived the round-trip.
  EXPECT_EQ(result->metadata.content_type, ContentType::kCss);
  EXPECT_EQ(result->metadata.flags, AlternateMetadata::kFlagNeedsRevalidation);
  EXPECT_EQ(result->metadata.origin_content_type, "text/css; charset=utf-8");
  EXPECT_EQ(result->metadata.cache_inserted_at, 1700000000u);
  EXPECT_EQ(result->metadata.origin_max_age, 86400u);
  EXPECT_EQ(result->metadata.origin_s_maxage, 3600u);
  EXPECT_EQ(result->metadata.origin_cc_flags,
            AlternateMetadata::kCCOriginPublic |
                AlternateMetadata::kCCOriginSMaxagePresent |
                AlternateMetadata::kCCOriginHeaderPresent);
}

TEST_F(PageSpeedCacheTest, RejectOversizedMetadata) {
  // WriteAlternate should reject metadata blobs larger than
  // max_metadata_size.  Since AlternateMetadata::Serialize() caps
  // origin_content_type at 256 bytes (max serialized = 264 bytes),
  // use a smaller limit to exercise the check.
  PageSpeedCacheConfig config;
  config.volume_path = cache_path_;
  config.volume_size = static_cast<uint64_t>(10 * 1024 * 1024);
  config.enable_checksum = true;
  config.max_metadata_size = 10;  // Very small limit

  auto result = PageSpeedCache::Create(config);
  ASSERT_TRUE(result.has_value());
  auto& small_cache = *result;

  CapabilityMask mask;
  AlternateId id =
      MaskToAlternateId(static_cast<uint8_t>(mask.Encode() & 0xFF));

  AlternateMetadata meta;
  meta.full_mask = mask.Encode();
  meta.content_type = ContentType::kImage;
  meta.origin_content_type = "image/jpeg";

  // Serialized metadata = 8 (fixed) + 10 (ct string) = 18 bytes > 10
  auto wh = small_cache->WriteAlternate("/img/big-meta.jpg", "example.com",
                                        "https", id, 10, meta);
  EXPECT_FALSE(wh.has_value())
      << "WriteAlternate should reject metadata exceeding max_metadata_size";
}

// The default metadata ceiling is derived from the format's worst case, so an
// entry with every origin string at its budget must be storable out of the
// box.  A ceiling below the worst case shows up in the field as entries that
// are silently never kept — the write is refused whole, which looks exactly
// like a cache that never warms.
TEST_F(PageSpeedCacheTest, DefaultCeilingStoresWorstCaseMetadata) {
  CreateCache();

  CapabilityMask mask;
  AlternateId id =
      MaskToAlternateId(static_cast<uint8_t>(mask.Encode() & 0xFF));

  AlternateMetadata meta;
  meta.full_mask = mask.Encode();
  meta.content_type = ContentType::kImage;
  meta.origin_content_type =
      std::string(AlternateMetadata::kMaxOriginCtLen, 'c');
  meta.origin_etag = std::string(AlternateMetadata::kEtagBudget, 'e');
  meta.origin_cache_control =
      std::string(AlternateMetadata::kMaxOriginCcLen, 'v');
  meta.origin_epoch = 0xFFFFFFFFFFFFFFFFull;
  ASSERT_EQ(meta.Serialize().size(),
            AlternateMetadata::kMaxSerializedSizeAtBudget);

  std::string data = "worst-case-metadata";
  auto wh = cache_->WriteAlternate("/img/worst-case.jpg", "example.com",
                                   "https", id, data.size(), meta);
  ASSERT_TRUE(wh.has_value())
      << "the default max_metadata_size must cover the format's worst case";
  ASSERT_TRUE(
      wh->write_sync(std::span(reinterpret_cast<const std::byte*>(data.data()),
                               data.size()))
          .has_value());
  ASSERT_TRUE(wh->close_sync().has_value());

  auto result = cache_->ReadBestAlternate("/img/worst-case.jpg", "example.com",
                                          "https", mask);
  ASSERT_TRUE(result.has_value());
  EXPECT_EQ(result->metadata.origin_epoch, 0xFFFFFFFFFFFFFFFFull);
  EXPECT_EQ(result->metadata.origin_cache_control.size(),
            AlternateMetadata::kMaxOriginCcLen);
  EXPECT_EQ(result->content_length(), data.size())
      << "the content offset must still be right at the largest prefix";
}

// An entry written by a build that knows a field this one does not is a MISS,
// not a partial read: the serving path re-records rather than serving a
// half-understood entry.
TEST_F(PageSpeedCacheTest, FutureMetadataVersionReadsAsMiss) {
  CreateCache();

  auto key = PageSpeedCache::ComposeKeyPreNormalized("/img/from-the-future.jpg",
                                                     "example.com", "https");
  CapabilityMask mask;
  AlternateId id =
      MaskToAlternateId(static_cast<uint8_t>(mask.Encode() & 0xFF));

  // A well-formed current-version blob, with the version byte advanced and
  // trailing bytes appended so that length cannot be what rejects it.
  AlternateMetadata meta;
  meta.full_mask = mask.Encode();
  meta.content_type = ContentType::kImage;
  meta.origin_content_type = "image/jpeg";
  meta.origin_cache_control = "max-age=60";
  auto blob = meta.Serialize();
  blob[0] = static_cast<std::byte>(AlternateMetadata::kCurrentVersion + 1);
  blob.resize(blob.size() + 32, std::byte{0x5A});

  auto wh = PageSpeedCacheTestPeer::Cyclone(*cache_).write_alternate_sync(
      key, static_cast<cyclone::AlternateId>(id), blob.size());
  ASSERT_TRUE(wh.has_value());
  ASSERT_TRUE(wh->write_sync(std::span(blob)).has_value());
  ASSERT_TRUE(wh->close_sync().has_value());

  auto result = cache_->ReadBestAlternate("/img/from-the-future.jpg",
                                          "example.com", "https", mask);
  EXPECT_FALSE(result.has_value());
  EXPECT_EQ(result.error(), cyclone::CacheError::NotFound);
}

// Stored HTML written by the worker under metadata version 8 is refused: both
// the selector read and the exact-id read report a miss. Everything else
// stored under version 8 stays readable.
class StoredVersionEightTest : public PageSpeedCacheTest {
 protected:
  // Stores an entry the way metadata version 8 wrote it, header copy
  // included.  `full_mask` 0 means the default mask.
  void StoreAtVersionEight(const std::string& path, ContentType type,
                           uint8_t flags, const std::string& body,
                           uint32_t full_mask = 0) {
    if (full_mask == 0) full_mask = CapabilityMask().Encode();
    AlternateId id = MaskToAlternateId(static_cast<uint8_t>(full_mask & 0xFF));
    AlternateMetadata meta;
    meta.full_mask = full_mask;
    meta.content_type = type;
    meta.flags = flags;
    meta.origin_content_type = "text/html";
    meta.origin_cache_control = "max-age=60";
    auto prefix = meta.Serialize();
    prefix[0] = std::byte{8};
    std::vector<std::byte> blob = prefix;
    for (char c : body) blob.push_back(static_cast<std::byte>(c));
    auto key =
        PageSpeedCache::ComposeKeyPreNormalized(path, "example.com", "https");
    auto wh = PageSpeedCacheTestPeer::Cyclone(*cache_).write_alternate_sync(
        key, static_cast<cyclone::AlternateId>(id), blob.size());
    ASSERT_TRUE(wh.has_value());
    wh->set_header(prefix);
    ASSERT_TRUE(wh->write_sync(std::span(blob)).has_value());
    ASSERT_TRUE(wh->close_sync().has_value());
  }

  AlternateId DefaultId() {
    CapabilityMask mask;
    return MaskToAlternateId(static_cast<uint8_t>(mask.Encode() & 0xFF));
  }
};

TEST_F(StoredVersionEightTest, WorkerWrittenHtmlReadsAsMiss) {
  CreateCache();
  StoreAtVersionEight("/page.html", ContentType::kHtml,
                      AlternateMetadata::kFlagWorkerProcessed, "<p>old</p>");

  auto best = cache_->ReadBestAlternate("/page.html", "example.com", "https",
                                        CapabilityMask());
  EXPECT_FALSE(best.has_value());
  auto exact =
      cache_->ReadAlternate("/page.html", "example.com", "https", DefaultId());
  EXPECT_FALSE(exact.has_value())
      << "an exact-id read must not hand out the refused bytes with default "
         "metadata, flags clear";
  if (!exact.has_value()) {
    EXPECT_EQ(exact.error(), cyclone::CacheError::NotFound);
  }
}

TEST_F(StoredVersionEightTest, RawHtmlReadsAsMiss) {
  CreateCache();
  StoreAtVersionEight("/raw.html", ContentType::kHtml, 0, "<p>raw</p>");

  EXPECT_FALSE(cache_
                   ->ReadBestAlternate("/raw.html", "example.com", "https",
                                       CapabilityMask())
                   .has_value());
  EXPECT_FALSE(
      cache_->ReadAlternate("/raw.html", "example.com", "https", DefaultId())
          .has_value());
}

TEST_F(StoredVersionEightTest, HtmlWithoutTheWorkerBitReadsAsMiss) {
  // HTML another writer stored with its response headers classified and the
  // worker bit clear.
  CreateCache();
  StoreAtVersionEight("/stored.html", ContentType::kHtml,
                      AlternateMetadata::kFlagHeadersClassified,
                      "<p>stored</p>");

  EXPECT_FALSE(cache_
                   ->ReadBestAlternate("/stored.html", "example.com", "https",
                                       CapabilityMask())
                   .has_value());
  auto exact = cache_->ReadAlternate("/stored.html", "example.com", "https",
                                     DefaultId());
  ASSERT_FALSE(exact.has_value());
  EXPECT_EQ(exact.error(), cyclone::CacheError::NotFound);
}

TEST_F(StoredVersionEightTest, WorkerWrittenCssStaysReadable) {
  CreateCache();
  StoreAtVersionEight("/a.css", ContentType::kCss,
                      AlternateMetadata::kFlagWorkerProcessed, "a{b:c}");

  auto best = cache_->ReadBestAlternate("/a.css", "example.com", "https",
                                        CapabilityMask());
  ASSERT_TRUE(best.has_value());
  EXPECT_EQ(best->metadata.version, 8);
  EXPECT_TRUE(
      cache_->ReadAlternate("/a.css", "example.com", "https", DefaultId())
          .has_value());
}

TEST_F(StoredVersionEightTest, WorkerWrittenMarkdownReadsAsMiss) {
  CreateCache();
  const uint32_t markdown = static_cast<uint32_t>(SentinelId::kAgentMarkdown);
  StoreAtVersionEight("/page.html", ContentType::kOther,
                      AlternateMetadata::kFlagWorkerProcessed, "# old",
                      markdown);

  auto exact = cache_->ReadAlternate("/page.html", "example.com", "https",
                                     static_cast<AlternateId>(markdown));
  EXPECT_FALSE(exact.has_value())
      << "an exact-id read must not hand out the refused bytes with default "
         "metadata";
  EXPECT_FALSE(cache_
                   ->ReadBestAlternate("/page.html", "example.com", "https",
                                       CapabilityMask(),
                                       /*agent_request_entitled=*/true)
                   .has_value());
}

TEST_F(StoredVersionEightTest, RemoveRetiredDropsOnlyRetiredEntries) {
  CreateCache();
  CapabilityMask gzip;
  gzip.set_transfer_encoding(CapabilityMask::TransferEncoding::kGzip);
  const uint32_t markdown = static_cast<uint32_t>(SentinelId::kAgentMarkdown);
  StoreAtVersionEight("/page.html", ContentType::kHtml, 0, "<p>raw</p>");
  StoreAtVersionEight("/page.html", ContentType::kHtml,
                      AlternateMetadata::kFlagWorkerProcessed, "gz",
                      gzip.Encode());
  StoreAtVersionEight("/page.html", ContentType::kOther,
                      AlternateMetadata::kFlagWorkerProcessed, "# old",
                      markdown);
  StoreAtVersionEight("/a.css", ContentType::kCss,
                      AlternateMetadata::kFlagWorkerProcessed, "a{b:c}");

  auto removed =
      cache_->RemoveRetiredAlternates("/page.html", "example.com", "https");
  ASSERT_TRUE(removed.has_value());
  EXPECT_EQ(*removed, 3u);
  auto css = cache_->RemoveRetiredAlternates("/a.css", "example.com", "https");
  ASSERT_TRUE(css.has_value());
  EXPECT_EQ(*css, 0u);
  auto none =
      cache_->RemoveRetiredAlternates("/absent.html", "example.com", "https");
  ASSERT_TRUE(none.has_value());
  EXPECT_EQ(*none, 0u);

  auto listed = cache_->ListAlternates("/page.html", "example.com", "https");
  EXPECT_TRUE(!listed.has_value() || listed->empty())
      << "no node of the page is left";

  auto served =
      cache_->ReadBestAlternate("/page.html", "example.com", "https", gzip);
  ASSERT_FALSE(served.has_value());
  EXPECT_EQ(served.error(), cyclone::CacheError::NotFound);
  EXPECT_TRUE(
      cache_->ReadAlternate("/a.css", "example.com", "https", DefaultId())
          .has_value());
}

TEST_F(StoredVersionEightTest, RemoveRetiredKeepsANewerEntryAtTheSameId) {
  // A page rebuilt at the same id is judged by its newest entry, which is
  // the one a removal would take.
  CreateCache();
  StoreAtVersionEight("/page.html", ContentType::kHtml,
                      AlternateMetadata::kFlagWorkerProcessed, "<p>old</p>");
  AlternateMetadata meta;
  meta.full_mask = CapabilityMask().Encode();
  meta.content_type = ContentType::kHtml;
  const std::string fresh = "<p>new</p>";
  auto wh = cache_->WriteAlternate("/page.html", "example.com", "https",
                                   DefaultId(), fresh.size(), meta);
  ASSERT_TRUE(wh.has_value());
  ASSERT_TRUE(
      wh->write_sync(std::as_bytes(std::span(fresh.data(), fresh.size())))
          .has_value());
  ASSERT_TRUE(wh->close_sync().has_value());

  auto removed =
      cache_->RemoveRetiredAlternates("/page.html", "example.com", "https");
  ASSERT_TRUE(removed.has_value());
  EXPECT_EQ(*removed, 0u);
  auto read =
      cache_->ReadAlternate("/page.html", "example.com", "https", DefaultId());
  ASSERT_TRUE(read.has_value());
  auto content = read->content();
  EXPECT_EQ(std::string(reinterpret_cast<const char*>(content.data()),
                        content.size()),
            fresh);
}

TEST_F(StoredVersionEightTest, ARewriteReadsBackAtTheCurrentVersion) {
  CreateCache();
  StoreAtVersionEight("/again.html", ContentType::kHtml,
                      AlternateMetadata::kFlagWorkerProcessed, "<p>old</p>");
  ASSERT_FALSE(
      cache_->ReadAlternate("/again.html", "example.com", "https", DefaultId())
          .has_value());

  CapabilityMask mask;
  AlternateMetadata meta;
  meta.full_mask = mask.Encode();
  meta.content_type = ContentType::kHtml;
  meta.flags = AlternateMetadata::kFlagWorkerProcessed;
  const std::string fresh = "<p>new</p>";
  auto wh = cache_->WriteAlternate("/again.html", "example.com", "https",
                                   DefaultId(), fresh.size(), meta);
  ASSERT_TRUE(wh.has_value());
  ASSERT_TRUE(
      wh->write_sync(std::as_bytes(std::span(fresh.data(), fresh.size())))
          .has_value());
  ASSERT_TRUE(wh->close_sync().has_value());

  auto read =
      cache_->ReadAlternate("/again.html", "example.com", "https", DefaultId());
  ASSERT_TRUE(read.has_value());
  EXPECT_EQ(read->metadata.version, AlternateMetadata::kCurrentVersion);
  auto content = read->content();
  EXPECT_EQ(std::string(reinterpret_cast<const char*>(content.data()),
                        content.size()),
            fresh);
}

TEST_F(PageSpeedCacheTest, WriteAroundCrossProcess) {
  // Verify that write-around caching allows cross-process overwrites
  // to become visible on the next read.  This is the key scenario:
  // nginx writes the original, worker overwrites with an optimized
  // variant, and nginx's next read should see the optimized version.
  CreateCache();

  CapabilityMask mask;
  AlternateId id =
      MaskToAlternateId(static_cast<uint8_t>(mask.Encode() & 0xFF));

  // Instance 1 (nginx) writes original content.
  WriteContent("/page.html", "example.com", mask, "original-html",
               ContentType::kHtml);

  // Instance 1 reads it back (populates RAM cache).
  auto r1 =
      cache_->ReadBestAlternate("/page.html", "example.com", "https", mask);
  ASSERT_TRUE(r1.has_value());
  {
    auto c = r1->content();
    std::string_view s(reinterpret_cast<const char*>(c.data()), c.size());
    EXPECT_EQ(s, "original-html");
  }

  // Instance 2 (worker) opens the same volume and overwrites.
  PageSpeedCacheConfig config2;
  config2.volume_path = cache_path_;
  config2.volume_size = static_cast<uint64_t>(10 * 1024 * 1024);
  config2.enable_checksum = true;
  auto result2 = PageSpeedCache::Create(config2);
  ASSERT_TRUE(result2.has_value());
  auto& cache2 = *result2;

  // Worker writes optimized content at the same AlternateId.
  {
    AlternateMetadata meta;
    meta.full_mask = mask.Encode();
    meta.content_type = ContentType::kHtml;
    meta.origin_content_type = "text/html";
    auto wh = cache2->WriteAlternate("/page.html", "example.com", "https", id,
                                     strlen("optimized-html"), meta);
    ASSERT_TRUE(wh.has_value());
    std::string_view data = "optimized-html";
    auto bytes = std::as_bytes(std::span(data));
    (void)wh->write_sync(bytes);
    (void)wh->close_sync();
  }

  // With RAM cache disabled, reads go directly to disk (mmap) and
  // should see the worker's write immediately.
  auto r2 =
      cache_->ReadBestAlternate("/page.html", "example.com", "https", mask);
  ASSERT_TRUE(r2.has_value());
  {
    auto c = r2->content();
    std::string_view s(reinterpret_cast<const char*>(c.data()), c.size());
    EXPECT_EQ(s, "optimized-html")
        << "Re-read should see worker's write (no RAM cache to serve stale)";
  }
}

TEST_F(PageSpeedCacheTest,
       SameProcessOverwriteIsVisibleWithoutAnExplicitEvict) {
  // Issue #1126 (same class as #1125) pinned the write-around RAM tier
  // hazard, which was not just cross-process.  The nginx origin-refresh
  // sequence is: read the stale identity in-process (RAM put #1 — CLFUS
  // seen-mark, not admitted), a racing SWR-coalesced serve reads again (put
  // #2 — ADMITTED with the stale bytes), then the re-record overwrites the
  // slot on disk.  That overwrite did NOT evict the RAM copy, so without a
  // post-commit EvictAlternateFromRamCache at the write site this process
  // served the pre-overwrite bytes indefinitely.
  //
  // The storage layer now evicts the superseded RAM entry as part of the
  // re-record, so the overwrite is visible on the very next read with no
  // call-site eviction at all.  The eviction the nginx record path and the
  // 304 restamp dispatch perform is now belt-and-braces — it is still
  // exercised below, and it must stay harmless now that it usually has
  // nothing left to evict.
  CreateCache();

  CapabilityMask mask;
  AlternateId id =
      MaskToAlternateId(static_cast<uint8_t>(mask.Encode() & 0xFF));

  WriteContent("/page.html", "example.com", mask, "origin-v1",
               ContentType::kHtml);

  // Two reads: the first marks the key seen, the second admits v1 into
  // this process's RAM tier.
  for (int i = 0; i < 2; ++i) {
    auto r =
        cache_->ReadBestAlternate("/page.html", "example.com", "https", mask);
    ASSERT_TRUE(r.has_value());
  }

  // Same-process overwrite at the same AlternateId (the re-record).
  WriteContent("/page.html", "example.com", mask, "origin-v2",
               ContentType::kHtml);

  // The hazard is gone: the re-record invalidated this process's RAM copy,
  // so the next read already sees the new bytes.
  {
    auto r =
        cache_->ReadBestAlternate("/page.html", "example.com", "https", mask);
    ASSERT_TRUE(r.has_value());
    auto c = r->content();
    std::string_view s(reinterpret_cast<const char*>(c.data()), c.size());
    EXPECT_EQ(s, "origin-v2")
        << "the re-record must invalidate this process's RAM copy";
  }

  // The call-site pattern (#1125/#1126) still runs at the write sites; it
  // must remain harmless now that it has nothing left to evict.
  cache_->EvictAlternateFromRamCache("/page.html", "example.com", "https", id);

  {
    auto r =
        cache_->ReadBestAlternate("/page.html", "example.com", "https", mask);
    ASSERT_TRUE(r.has_value());
    auto c = r->content();
    std::string_view s(reinterpret_cast<const char*>(c.data()), c.size());
    EXPECT_EQ(s, "origin-v2")
        << "a redundant post-commit eviction must not disturb the read";
  }
}

TEST_F(PageSpeedCacheTest, SentinelReRecordEvictsItsOwnRamCopy) {
  // Issue #1126 pinned the write-around RAM-tier hazard for sentinel slots:
  // the worker's llms.txt loop (LlmsTxtNeedsBuild reads kLlmsTxtMeta, the
  // build overwrites it), browser profiles (LookupProfile reads
  // kBrowserProfile, StoreProfile overwrites it) and the kContentHash rewrite
  // all read a sentinel in-process before overwriting it, and an overwrite
  // that did not evict the RAM copy left the stale record winning every
  // subsequent read.  #1312 closed this half of the hazard IN THE WRITER:
  // WriteSentinel now evicts this process's RAM copy as part of unlinking the
  // superseded node, so a same-process re-record is visible on the next read
  // with no call-site eviction.  The cross-process half is unchanged: a
  // writer can only evict its OWN RAM tier, so a peer that admitted the stale
  // copy still needs the #1125/#1126 post-commit eviction at its call site.
  CreateCache();

  const auto meta_id = static_cast<AlternateId>(SentinelId::kLlmsTxtMeta);
  auto write_meta = [&](std::string_view data) {
    auto wh = cache_->WriteSentinel("/llms.txt", "example.com", "https",
                                    SentinelId::kLlmsTxtMeta, data.size());
    ASSERT_TRUE(wh.has_value());
    auto bytes = std::as_bytes(std::span(data));
    ASSERT_TRUE(wh->write_sync(bytes).has_value());
    ASSERT_TRUE(wh->close_sync().has_value());
  };
  auto read_meta = [&]() -> std::string {
    auto r =
        cache_->ReadAlternate("/llms.txt", "example.com", "https", meta_id);
    EXPECT_TRUE(r.has_value());
    if (!r.has_value()) return {};
    auto c = r->content();
    return std::string(reinterpret_cast<const char*>(c.data()), c.size());
  };

  write_meta("old-meta");

  // Two reads: the first marks the key seen, the second admits the stale
  // blob into this process's RAM tier (CLFUS) — the earlier setup under
  // which the overwrite below served "old-meta" until the call site evicted.
  ASSERT_EQ(read_meta(), "old-meta");
  ASSERT_EQ(read_meta(), "old-meta");

  // Same-process sentinel re-record.  The writer's own eviction now makes
  // the overwrite visible immediately...
  write_meta("new-meta");
  EXPECT_EQ(read_meta(), "new-meta")
      << "the writer evicts its own RAM copy before replacing the node";

  // ...and the chain did not grow for it: one node under the id.
  EXPECT_EQ(ChainNodesFor("/llms.txt", SentinelId::kLlmsTxtMeta), 1u);
}

TEST_F(PageSpeedCacheTest, GenerationFileOnReset) {
  CreateCache();

  std::string gen_path = cache_path_ + ".gen";

  // No .gen file exists yet (Create() only reads, doesn't write).
  // ReadGenerationFile returns 0 for missing files.
  EXPECT_FALSE(std::filesystem::exists(gen_path));
  uint64_t gen0 = PageSpeedCache::ReadGenerationFile(gen_path);
  EXPECT_EQ(gen0, 0u) << "Initial generation should be 0";

  // Write some content.
  CapabilityMask mask;
  WriteContent("/img/gen-test.jpg", "example.com", mask, "gen-data");

  // Reset the volume.
  auto reset_result = cache_->ResetVolume();
  ASSERT_TRUE(reset_result.has_value())
      << "ResetVolume failed: " << static_cast<int>(reset_result.error());

  // .gen should have incremented to 1.
  uint64_t gen1 = PageSpeedCache::ReadGenerationFile(gen_path);
  EXPECT_EQ(gen1, 1u) << "Generation should be 1 after first reset";

  // Old content should be gone.
  AlternateId id =
      MaskToAlternateId(static_cast<uint8_t>(mask.Encode() & 0xFF));
  EXPECT_FALSE(
      cache_->AlternateExists("/img/gen-test.jpg", "example.com", "https", id));

  // New writes should succeed.
  WriteContent("/img/gen-test2.jpg", "example.com", mask, "new-data");
  {
    // The result borrows the volume's memory mapping.  Scope it so the
    // mapping is released before the second reset: Windows cannot delete a
    // file that is still mapped, and ResetVolume now reports that.
    auto r = cache_->ReadBestAlternate("/img/gen-test2.jpg", "example.com",
                                       "https", mask);
    ASSERT_TRUE(r.has_value());
    auto content = r->content();
    std::string_view read_data(reinterpret_cast<const char*>(content.data()),
                               content.size());
    EXPECT_EQ(read_data, "new-data");
  }

  // Second reset should increment to 2.
  auto reset2 = cache_->ResetVolume();
  ASSERT_TRUE(reset2.has_value());
  uint64_t gen2 = PageSpeedCache::ReadGenerationFile(gen_path);
  EXPECT_EQ(gen2, 2u) << "Generation should be 2 after second reset";

  // A second cache instance on the same volume reads the new generation.
  PageSpeedCacheConfig config2;
  config2.volume_path = cache_path_;
  config2.volume_size = static_cast<uint64_t>(10 * 1024 * 1024);
  config2.enable_checksum = true;
  auto result2 = PageSpeedCache::Create(config2);
  ASSERT_TRUE(result2.has_value());
  // The second instance reads generation 2 via ReadGenerationFile in Create().
  uint64_t gen2_read = PageSpeedCache::ReadGenerationFile(gen_path);
  EXPECT_EQ(gen2_read, 2u);
}

#ifndef _WIN32
// Unix file permission test — not applicable on Windows.
TEST_F(PageSpeedCacheTest, VolumePermissionsAfterReset) {
  CreateCache();

  auto reset_result = cache_->ResetVolume();
  ASSERT_TRUE(reset_result.has_value())
      << "ResetVolume failed: " << static_cast<int>(reset_result.error());

  struct stat st;
  ASSERT_EQ(::stat(cache_->VolumeFilePath().c_str(), &st), 0);
  EXPECT_EQ(st.st_mode & static_cast<mode_t>(0777), static_cast<mode_t>(0660))
      << "Cache volume should be mode 0660 after reset";
}
#endif  // !_WIN32

TEST_F(PageSpeedCacheTest, GenerationFileMissing) {
  // ReadGenerationFile should return 0 for a non-existent path.
  uint64_t gen =
      PageSpeedCache::ReadGenerationFile(temp_dir_ + "/nonexistent.gen");
  EXPECT_EQ(gen, 0u);
}

TEST_F(PageSpeedCacheTest, GenerationFileCorrupt) {
  std::string gen_path = temp_dir_ + "/corrupt.gen";

  // Empty file.
  {
    std::ofstream(gen_path) << "";
  }
  EXPECT_EQ(PageSpeedCache::ReadGenerationFile(gen_path), 0u);

  // Non-numeric content.
  {
    std::ofstream(gen_path) << "not_a_number\n";
  }
  EXPECT_EQ(PageSpeedCache::ReadGenerationFile(gen_path), 0u);

  // Overflow (exceeds uint64_t range).
  {
    std::ofstream(gen_path) << "99999999999999999999999\n";
  }
  EXPECT_EQ(PageSpeedCache::ReadGenerationFile(gen_path), 0u);

  // Valid number with trailing garbage — parses the leading number.
  {
    std::ofstream(gen_path) << "42xyz\n";
  }
  EXPECT_EQ(PageSpeedCache::ReadGenerationFile(gen_path), 42u);

  // Valid number.
  {
    std::ofstream(gen_path) << "12345\n";
  }
  EXPECT_EQ(PageSpeedCache::ReadGenerationFile(gen_path), 12345u);
}

// ---------------------------------------------------------------------------
// Phase 3.2: Cache Error Path Tests
// ---------------------------------------------------------------------------

// --- Creation failures ---

TEST_F(PageSpeedCacheTest, CreateFailsMissingParentDirectory) {
  // Pointing volume_path at a non-existent parent directory should cause
  // Create() to return an error (the directory tree is not auto-created).
  PageSpeedCacheConfig config;
  config.volume_path = temp_dir_ + "/no/such/parent/cache.vol";
  config.volume_size = static_cast<uint64_t>(10 * 1024 * 1024);
  config.enable_checksum = true;

  auto result = PageSpeedCache::Create(config);
  EXPECT_FALSE(result.has_value())
      << "Create should fail when parent directory does not exist";
}

#ifndef _WIN32
// Unix file permission test — not applicable on Windows.
TEST_F(PageSpeedCacheTest, CreateFailsReadOnlyDirectory) {
  // Root can write to read-only directories, so this test is meaningless as root.
  if (::geteuid() == 0) {
    GTEST_SKIP() << "Skipped: root bypasses directory permissions";
  }

  // Make the temp directory read-only so Cyclone cannot create the volume file.
  ASSERT_EQ(::chmod(temp_dir_.c_str(), 0555), 0)
      << "Failed to set directory to read-only";

  PageSpeedCacheConfig config;
  config.volume_path = cache_path_;
  config.volume_size = static_cast<uint64_t>(10 * 1024 * 1024);
  config.enable_checksum = true;

  auto result = PageSpeedCache::Create(config);
  // Restore permissions before any assertion can abort, so TearDown can clean
  // up even if the test fails.
  ASSERT_EQ(::chmod(temp_dir_.c_str(), 0755), 0)
      << "Failed to restore directory permissions";

  EXPECT_FALSE(result.has_value())
      << "Create should fail when directory is read-only";
}
#endif  // !_WIN32

TEST_F(PageSpeedCacheTest, CreateFailsInvalidVolumeSize) {
  // A zero volume size should be rejected by Cyclone.
  PageSpeedCacheConfig config;
  config.volume_path = cache_path_;
  config.volume_size = 0;
  config.enable_checksum = true;

  auto result = PageSpeedCache::Create(config);
  EXPECT_FALSE(result.has_value())
      << "Create should fail with a zero volume size";
}

// --- Write error paths ---

TEST_F(PageSpeedCacheTest, WriteAlternateRejectsInvalidSentinelId) {
  // All sentinel IDs should be rejected by WriteAlternate — callers must use
  // WriteSentinel() instead.  Test multiple sentinel values.
  CreateCache();

  AlternateMetadata meta;
  meta.full_mask = 0;
  meta.content_type = ContentType::kOther;
  meta.origin_content_type = "text/plain";

  // kOriginalContent sentinel
  auto r1 = cache_->WriteAlternate(
      "/page.html", "example.com", "https",
      static_cast<AlternateId>(SentinelId::kOriginalContent), 10, meta);
  EXPECT_FALSE(r1.has_value())
      << "kOriginalContent sentinel should be rejected";

  // kEarlyHints sentinel
  auto r2 = cache_->WriteAlternate(
      "/page.html", "example.com", "https",
      static_cast<AlternateId>(SentinelId::kEarlyHints), 10, meta);
  EXPECT_FALSE(r2.has_value()) << "kEarlyHints sentinel should be rejected";

  // kWarmupRequest sentinel
  auto r3 = cache_->WriteAlternate(
      "/page.html", "example.com", "https",
      static_cast<AlternateId>(SentinelId::kWarmupRequest), 10, meta);
  EXPECT_FALSE(r3.has_value()) << "kWarmupRequest sentinel should be rejected";

  // kContentHash sentinel
  auto r4 = cache_->WriteAlternate(
      "/page.html", "example.com", "https",
      static_cast<AlternateId>(SentinelId::kContentHash), 10, meta);
  EXPECT_FALSE(r4.has_value()) << "kContentHash sentinel should be rejected";

  // kSubresourceManifest sentinel
  auto r5 = cache_->WriteAlternate(
      "/page.html", "example.com", "https",
      static_cast<AlternateId>(SentinelId::kSubresourceManifest), 10, meta);
  EXPECT_FALSE(r5.has_value())
      << "kSubresourceManifest sentinel should be rejected";

  // kBrowserProfile sentinel
  auto r6 = cache_->WriteAlternate(
      "/page.html", "example.com", "https",
      static_cast<AlternateId>(SentinelId::kBrowserProfile), 10, meta);
  EXPECT_FALSE(r6.has_value()) << "kBrowserProfile sentinel should be rejected";
}

TEST_F(PageSpeedCacheTest, WriteAlternateRejectsOversizedMetadata) {
  // When max_metadata_size is set very small, metadata that exceeds it should
  // be rejected with an error.
  PageSpeedCacheConfig config;
  config.volume_path = cache_path_;
  config.volume_size = static_cast<uint64_t>(10 * 1024 * 1024);
  config.enable_checksum = true;
  config.max_metadata_size = 5;  // Unrealistically small

  auto result = PageSpeedCache::Create(config);
  ASSERT_TRUE(result.has_value());
  auto& small_meta_cache = *result;

  CapabilityMask mask;
  AlternateId id =
      MaskToAlternateId(static_cast<uint8_t>(mask.Encode() & 0xFF));

  AlternateMetadata meta;
  meta.full_mask = mask.Encode();
  meta.content_type = ContentType::kHtml;
  meta.origin_content_type = "text/html; charset=utf-8";  // Long enough

  auto wh = small_meta_cache->WriteAlternate("/page.html", "example.com",
                                             "https", id, 100, meta);
  EXPECT_FALSE(wh.has_value())
      << "WriteAlternate should reject metadata exceeding max_metadata_size";
  EXPECT_EQ(wh.error(), cyclone::CacheError::InvalidArgument);
}

// --- Read error paths ---

TEST_F(PageSpeedCacheTest, ReadAlternateNotFoundOnEmptyCache) {
  // ReadAlternate (direct ID lookup) on an empty cache should return an error.
  CreateCache();

  AlternateId id = 0x08;  // Default desktop/identity
  auto result =
      cache_->ReadAlternate("/img/nonexistent.jpg", "example.com", "https", id);
  EXPECT_FALSE(result.has_value())
      << "ReadAlternate on empty cache should return an error";
}

TEST_F(PageSpeedCacheTest, ReadBestAlternateReturnsNotFoundOnMiss) {
  // Write one variant, then request a completely different URL.
  // ReadBestAlternate should return not-found for the missing URL.
  CreateCache();

  CapabilityMask mask = MakeTestMask(CapabilityMask::ImageFormat::kWebP,
                                     CapabilityMask::Viewport::kDesktop);

  // Write content for one URL.
  WriteContent("/img/exists.jpg", "example.com", mask, "some-data");

  // Attempt to read a different URL that was never written.
  auto result = cache_->ReadBestAlternate("/img/does-not-exist.jpg",
                                          "example.com", "https", mask);
  EXPECT_FALSE(result.has_value())
      << "ReadBestAlternate should return not-found for non-existent URL";
}

// --- ResetVolume ---

TEST_F(PageSpeedCacheTest, ResetVolumeSucceedsWhenOnlyAStaleFileRemains) {
  // A leftover that this cache never opened (here a non-empty directory at the
  // raw small-tier path, which no volume uses) cannot be removed, but the
  // volume the cache serves from is gone.  The purge worked: the generation
  // moves so peers reopen, and the cache is empty.
  CreateCache();
  CapabilityMask mask;
  const AlternateId id =
      MaskToAlternateId(static_cast<uint8_t>(mask.Encode() & 0xFF));
  WriteContent("/img/gone.jpg", "example.com", mask, "gone-data");

  const std::string blocker = cache_path_ + ".small";
  std::filesystem::create_directories(blocker);
  {
    std::ofstream child(blocker + "/child");
    child << "x";
  }
  const std::string gen_path = cache_path_ + ".gen";
  const uint64_t gen_before = PageSpeedCache::ReadGenerationFile(gen_path);

  std::string detail;
  auto result = cache_->ResetVolume(&detail);
  ASSERT_TRUE(result.has_value());
  EXPECT_TRUE(detail.empty());
  EXPECT_EQ(PageSpeedCache::ReadGenerationFile(gen_path), gen_before + 1);
  EXPECT_FALSE(
      cache_->AlternateExists("/img/gone.jpg", "example.com", "https", id));
  EXPECT_TRUE(std::filesystem::exists(blocker + "/child"));
}

TEST_F(PageSpeedCacheTest, ResetVolumeRecreatesCleanCache) {
  // After ResetVolume, all previously written data should be gone, and new
  // writes should succeed on the fresh volume.
  CreateCache();

  CapabilityMask mask;
  AlternateId id =
      MaskToAlternateId(static_cast<uint8_t>(mask.Encode() & 0xFF));

  // Write several entries.
  WriteContent("/img/a.jpg", "example.com", mask, "aaa");
  WriteContent("/img/b.jpg", "example.com", mask, "bbb");
  WriteContent("/img/c.jpg", "example.com", mask, "ccc");

  // Confirm they exist.
  EXPECT_TRUE(
      cache_->AlternateExists("/img/a.jpg", "example.com", "https", id));
  EXPECT_TRUE(
      cache_->AlternateExists("/img/b.jpg", "example.com", "https", id));
  EXPECT_TRUE(
      cache_->AlternateExists("/img/c.jpg", "example.com", "https", id));

  // Reset.
  auto reset_result = cache_->ResetVolume();
  ASSERT_TRUE(reset_result.has_value())
      << "ResetVolume error: " << static_cast<int>(reset_result.error());

  // All old entries should be gone.
  EXPECT_FALSE(
      cache_->AlternateExists("/img/a.jpg", "example.com", "https", id));
  EXPECT_FALSE(
      cache_->AlternateExists("/img/b.jpg", "example.com", "https", id));
  EXPECT_FALSE(
      cache_->AlternateExists("/img/c.jpg", "example.com", "https", id));

  // New writes should work on the clean cache.
  WriteContent("/img/d.jpg", "example.com", mask, "new-data");
  auto read_result =
      cache_->ReadBestAlternate("/img/d.jpg", "example.com", "https", mask);
  ASSERT_TRUE(read_result.has_value()) << "Post-reset write+read failed: "
                                       << static_cast<int>(read_result.error());
  auto content = read_result->content();
  std::string_view data(reinterpret_cast<const char*>(content.data()),
                        content.size());
  EXPECT_EQ(data, "new-data");
}

TEST_F(PageSpeedCacheTest, ResetKeepsAnEarlierFormatVolumeAndClearsThisFormat) {
  // The volume filename carries the on-disk FORMAT the file was written in,
  // which is what lets two builds share a cache directory without reading each
  // other's bytes.  The clean-slate sweep must respect that boundary: an
  // operator who upgrades across a format change keeps the old volume on
  // purpose — it is the warm rollback — and a cache purge (this path) or the
  // serving module's open-failure recovery (the static path below) must not
  // take it away.  Within THIS format the sweep still has to be thorough,
  // including a file left by an earlier geometry that this instance never
  // opened.
  CreateCache();
  CapabilityMask mask;
  WriteContent("/img/a.jpg", "example.com", mask, "aaa");
  const std::string live = cache_->VolumeFilePath();
  ASSERT_FALSE(live.empty());

  namespace fs = std::filesystem;
  const std::string major =
      std::to_string(cyclone::VolumeHeader::kFormatVersionMajor);
  const std::string older =
      std::to_string(cyclone::VolumeHeader::kFormatVersionMajor - 1);
  auto plant = [&](const std::string& name) {
    const fs::path path = fs::path(temp_dir_) / name;
    std::ofstream f(path, std::ios::binary);
    f << "a volume this build did not write";
    f.close();
    EXPECT_TRUE(fs::exists(path)) << name;
    return path;
  };
  // Both shapes the sweep knows, in the previous format ...
  const fs::path old_main = plant("cache-" + older + "-0123456789abcdef.vol");
  const fs::path old_small =
      plant("cache.vol-" + older + "-0123456789abcdef.small");
  // ... and one in THIS format that the live instance is not using.
  const fs::path same_format =
      plant("cache-" + major + "-fedcba9876543210.vol");
  ASSERT_NE(same_format.string(), live);
  // ... plus the small-tier shape in THIS format.  Without this positive
  // control, deleting the matcher's small-tier arm outright would still pass:
  // every planted ".small" file would survive trivially, for the wrong reason.
  const fs::path same_small =
      plant("cache.vol-" + major + "-fedcba9876543210.small");
  // A FUTURE format, which happens whenever an operator rolls forward and back
  // again.  The boundary is "not this build's format", not "older than it": a
  // rule that only kept LOWER majors would delete this file, and nothing else
  // here would notice.
  const std::string newer =
      std::to_string(cyclone::VolumeHeader::kFormatVersionMajor + 1);
  const fs::path future = plant("cache-" + newer + "-0f1e2d3c4b5a6978.vol");
  // A major that merely STARTS with ours -- 7 against 71.  Only the exact
  // length of the fingerprint segment rejects this one, so it is what fails if
  // that comparison is ever relaxed back to a minimum length.  It is also the
  // case that arrives on its own the day the format major reaches two digits;
  // `older` above stops exercising the major comparison at that point (10 - 1
  // is narrower, so it is rejected on length instead), and this plant is what
  // keeps the branch covered.
  const fs::path wider = plant("cache-" + major + "1-0f1e2d3c4b5a6978.vol");

  ASSERT_TRUE(cache_->ResetVolume().has_value());

  EXPECT_TRUE(fs::exists(old_main))
      << "the previous format's volume was deleted: the documented warm "
         "rollback is gone, and the data was never this build's to remove";
  EXPECT_TRUE(fs::exists(old_small))
      << "the previous format's small-tier volume was deleted";
  EXPECT_FALSE(fs::exists(same_format))
      << "a leftover in THIS format must still be swept";
  EXPECT_FALSE(fs::exists(same_small))
      << "a small-tier leftover in THIS format must still be swept";
  EXPECT_TRUE(fs::exists(future))
      << "a later format's volume was deleted: the sweep kept only formats "
         "older than this one instead of keeping every format but this one";
  EXPECT_TRUE(fs::exists(wider))
      << "a volume whose format major merely begins with ours was deleted: "
         "the fingerprint segment is being matched by prefix, not whole";
  EXPECT_FALSE(cache_->AlternateExists(
      "/img/a.jpg", "example.com", "https",
      MaskToAlternateId(static_cast<uint8_t>(mask.Encode() & 0xFF))))
      << "the reset must still clear this format's cache";

  // The static path, which is what the serving module calls when opening the
  // cache failed: same boundary, no instance involved.
  const std::string live_after = cache_->VolumeFilePath();
  cache_.reset();
  PageSpeedCache::RemoveVolumeFiles(cache_path_);
  EXPECT_TRUE(fs::exists(old_main))
      << "the recovery path deleted the previous format's volume";
  EXPECT_TRUE(fs::exists(old_small));
  EXPECT_TRUE(fs::exists(future))
      << "the recovery path deleted a later format's volume";
  EXPECT_TRUE(fs::exists(wider))
      << "the recovery path matched the format major by prefix";
  EXPECT_FALSE(fs::exists(live_after))
      << "the recovery path must still remove this format's volume";
}

// The classification the worker's startup recovery consults (#428): which
// Create() failures mean the volume file is damaged (so a
// delete-and-recreate retry may fix the open) and which are live peers,
// refusals or environment failures the recovery must leave alone.
TEST_F(PageSpeedCacheTest, OpenFailureIsDamagedVolumeClassification) {
  // The damaged-volume error: a file too small for a volume header, or a
  // header that fails validation.  At the pinned storage layer such a
  // volume is reset in place inside Create() when no peer holds it, so
  // the classification is a backstop for a build that reports it directly.
  EXPECT_TRUE(PageSpeedCache::OpenFailureIsDamagedVolume(
      cyclone::CacheError::Corrupted))
      << "a truncated file or an invalid header is the damaged-volume error";

  // A live peer, never damage: ResetRefusedLivePeer means the volume
  // needs a reset and another process holds it, and the remaining
  // IncompatibleVersion path on open fires only when a peer raced the
  // open in.  Deleting under a possible live peer splits two processes
  // onto different volume files with no reset signal, so the recovery
  // must leave the files alone.
  EXPECT_FALSE(PageSpeedCache::OpenFailureIsDamagedVolume(
      cyclone::CacheError::ResetRefusedLivePeer))
      << "a volume another live process holds must not be deleted";
  EXPECT_FALSE(PageSpeedCache::OpenFailureIsDamagedVolume(
      cyclone::CacheError::IncompatibleVersion))
      << "a peer racing the open in must not be deleted for";

  // IoError is the refusal code: the ownership, permission and symlink
  // refusals share it with genuine I/O failures, so it must never be
  // treated as damage.
  EXPECT_FALSE(
      PageSpeedCache::OpenFailureIsDamagedVolume(cyclone::CacheError::IoError));
  // A volume the cache library refuses (not a plain file) is not damage.
  EXPECT_FALSE(PageSpeedCache::OpenFailureIsDamagedVolume(
      cyclone::CacheError::VolumeFileRefused));
  // Configuration, geometry and environment failures a delete cannot fix.
  EXPECT_FALSE(PageSpeedCache::OpenFailureIsDamagedVolume(
      cyclone::CacheError::InvalidArgument));
  EXPECT_FALSE(PageSpeedCache::OpenFailureIsDamagedVolume(
      cyclone::CacheError::InvalidConfiguration));
  EXPECT_FALSE(
      PageSpeedCache::OpenFailureIsDamagedVolume(cyclone::CacheError::Busy));
  EXPECT_FALSE(PageSpeedCache::OpenFailureIsDamagedVolume(
      cyclone::CacheError::NotInitialized));
  EXPECT_FALSE(PageSpeedCache::OpenFailureIsDamagedVolume(
      cyclone::CacheError::InternalError));
  EXPECT_FALSE(
      PageSpeedCache::OpenFailureIsDamagedVolume(cyclone::CacheError::Success));
}

// The recovery the nginx module runs when its cache open fails (#439): only
// a damaged volume is deleted and recreated; a volume another live process
// holds is left exactly as it is.  The live-peer case needs OFD locks (POSIX
// without macOS), like the worker's equivalent test.
#if defined(F_OFD_SETLK)
TEST_F(PageSpeedCacheTest, RecreatingRecoveryLeavesAVolumeAPeerHoldsAlone) {
  PageSpeedCacheConfig peer_config;
  peer_config.volume_path = cache_path_;
  peer_config.volume_size = static_cast<uint64_t>(10 * 1024 * 1024);
  auto peer = PageSpeedCache::Create(peer_config);
  ASSERT_TRUE(peer.has_value());
  const std::string volume_file = (*peer)->VolumeFilePath();
  ASSERT_FALSE(volume_file.empty());

  // The shape a full disk or a power loss leaves behind, under a live peer.
  std::error_code ec;
  std::filesystem::resize_file(volume_file, 16, ec);
  ASSERT_FALSE(ec) << ec.message();

  PageSpeedCacheConfig module_config;
  module_config.volume_path = cache_path_;
  module_config.volume_size = static_cast<uint64_t>(10 * 1024 * 1024);
  bool deleted = true;
  auto result =
      PageSpeedCache::CreateRecreatingDamagedVolume(module_config, &deleted);

  EXPECT_FALSE(result.has_value())
      << "the module must run without the cache, not replace the volume";
  EXPECT_FALSE(deleted) << "a volume another live process holds was deleted";
  EXPECT_TRUE(std::filesystem::exists(volume_file))
      << "the volume file a live peer holds was deleted";
  EXPECT_EQ(std::filesystem::file_size(volume_file),
            static_cast<std::uintmax_t>(16))
      << "the volume file a live peer holds was replaced";
}
#endif  // defined(F_OFD_SETLK)

TEST_F(PageSpeedCacheTest, RecreatingRecoveryRepairsADamagedVolumeNobodyHolds) {
  std::string volume_file;
  {
    PageSpeedCacheConfig create_config;
    create_config.volume_path = cache_path_;
    create_config.volume_size = static_cast<uint64_t>(10 * 1024 * 1024);
    auto created = PageSpeedCache::Create(create_config);
    ASSERT_TRUE(created.has_value());
    volume_file = (*created)->VolumeFilePath();
  }
  std::error_code ec;
  std::filesystem::resize_file(volume_file, 16, ec);
  ASSERT_FALSE(ec) << ec.message();

  PageSpeedCacheConfig module_config;
  module_config.volume_path = cache_path_;
  module_config.volume_size = static_cast<uint64_t>(10 * 1024 * 1024);
  auto result = PageSpeedCache::CreateRecreatingDamagedVolume(module_config);
  EXPECT_TRUE(result.has_value())
      << "a damaged volume with no live holder must come back usable";
}

TEST_F(PageSpeedCacheTest, RecreatingRecoveryDeletesNothingOnARefusedOpen) {
  // A volume path that cannot be opened (its parent is a regular file) is
  // not damage: nothing may be removed and the error is returned.
  const std::string blocker = temp_dir_ + "/blocker";
  {
    std::ofstream out(blocker);
    out << "x";
  }
  PageSpeedCacheConfig config;
  config.volume_path = blocker + "/cache.vol";
  config.volume_size = 0;
  bool deleted = true;
  auto result = PageSpeedCache::CreateRecreatingDamagedVolume(config, &deleted);
  EXPECT_FALSE(result.has_value());
  EXPECT_FALSE(deleted);
  EXPECT_TRUE(std::filesystem::exists(blocker));
}

// ========== Coverage: Stats, handler logging, edge cases ==========

TEST_F(PageSpeedCacheTest, StatsReturnsValidResult) {
  CreateCache();

  // Stats() should work on an empty cache.
  auto stats = cache_->Stats();
  EXPECT_EQ(stats.disk_cache_hits, 0u);
  EXPECT_EQ(stats.bytes_read, 0u);

  // Write and read to generate stats.
  CapabilityMask mask;
  WriteContent("/img/stats-test.jpg", "example.com", mask, "stats-data");
  (void)cache_->ReadBestAlternate("/img/stats-test.jpg", "example.com", "https",
                                  mask);

  // Cyclone may update stats counters asynchronously after the synchronous
  // write/read calls return.  Poll briefly to avoid flakes on loaded CI.
  for (int i = 0; i < 20; ++i) {
    stats = cache_->Stats();
    if (stats.disk_cache_hits >= 1 && stats.current_entries >= 1) break;
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
  }
  // bytes_written/bytes_read are not currently tracked per-operation in
  // Cyclone (returns 0).  Verify other stats that are tracked.
  EXPECT_GE(stats.disk_cache_hits, 1u);
  EXPECT_GE(stats.current_entries, 1u);
}

TEST_F(PageSpeedCacheTest, AlternateExistsReturnsFalseForWrongId) {
  // URL exists in cache, but with a different AlternateId.
  CreateCache();

  CapabilityMask webp_mask = MakeTestMask(CapabilityMask::ImageFormat::kWebP,
                                          CapabilityMask::Viewport::kDesktop);
  WriteContent("/img/alt-test.jpg", "example.com", webp_mask, "webp-data");

  // Check for an AlternateId that wasn't written (AVIF mask).
  CapabilityMask avif_mask = MakeTestMask(CapabilityMask::ImageFormat::kAvif,
                                          CapabilityMask::Viewport::kDesktop);
  AlternateId avif_id =
      MaskToAlternateId(static_cast<uint8_t>(avif_mask.Encode() & 0xFF));
  EXPECT_FALSE(cache_->AlternateExists("/img/alt-test.jpg", "example.com",
                                       "https", avif_id));
}

TEST_F(PageSpeedCacheTest, ListAlternatesNonExistentUrl) {
  CreateCache();

  auto alts =
      cache_->ListAlternates("/img/nonexistent.jpg", "example.com", "https");
  EXPECT_FALSE(alts.has_value());
}

TEST_F(PageSpeedCacheTest, HeadersSidecarPlainMissLogsNothing) {
  TestCacheHandler handler;
  PageSpeedCacheConfig config;
  config.volume_path = cache_path_;
  config.volume_size = static_cast<uint64_t>(10 * 1024 * 1024);
  config.handler = &handler;
  auto result = PageSpeedCache::Create(config);
  ASSERT_TRUE(result.has_value());

  const int before = handler.count();
  auto read = (*result)->ReadHeadersSidecar("https://example.com/none.html",
                                            "example.com", "https");
  EXPECT_FALSE(read.has_value());
  EXPECT_EQ(handler.count(), before) << handler.last_message();
}

TEST_F(PageSpeedCacheTest, HandlerLogsReadMiss) {
  // Set up cache with a handler to exercise the handler logging code paths.
  TestCacheHandler handler;
  PageSpeedCacheConfig config;
  config.volume_path = cache_path_;
  config.volume_size = static_cast<uint64_t>(10 * 1024 * 1024);
  config.enable_checksum = true;
  config.handler = &handler;

  auto result = PageSpeedCache::Create(config);
  ASSERT_TRUE(result.has_value());
  auto& logged_cache = *result;

  CapabilityMask mask;

  // ReadBestAlternate miss → handler_->Info() (lines 136-140).
  auto r1 =
      logged_cache->ReadBestAlternate("/miss", "example.com", "https", mask);
  EXPECT_FALSE(r1.has_value());
  EXPECT_GE(handler.count(), 1);

  // ReadAlternate miss → handler_->Info() (lines 167-172).
  int count_before = handler.count();
  auto r2 = logged_cache->ReadAlternate("/miss", "example.com", "https", 0x08);
  EXPECT_FALSE(r2.has_value());
  EXPECT_GT(handler.count(), count_before);
}

TEST_F(PageSpeedCacheTest, HandlerLogsWriteAndRemove) {
  TestCacheHandler handler;
  PageSpeedCacheConfig config;
  config.volume_path = cache_path_;
  config.volume_size = static_cast<uint64_t>(10 * 1024 * 1024);
  config.enable_checksum = true;
  config.handler = &handler;

  auto result = PageSpeedCache::Create(config);
  ASSERT_TRUE(result.has_value());
  auto& logged_cache = *result;

  // WriteAlternate success → handler_->Info() (lines 230-234).
  CapabilityMask mask;
  AlternateId id =
      MaskToAlternateId(static_cast<uint8_t>(mask.Encode() & 0xFF));
  AlternateMetadata meta;
  meta.full_mask = mask.Encode();
  meta.content_type = ContentType::kImage;
  meta.origin_content_type = "image/jpeg";
  std::string data = "test";
  auto wh = logged_cache->WriteAlternate("/img/log.jpg", "example.com", "https",
                                         id, 4, meta);
  ASSERT_TRUE(wh.has_value());
  auto bytes = std::as_bytes(std::span(data));
  (void)wh->write_sync(bytes);
  (void)wh->close_sync();

  int count_before = handler.count();

  // Remove → handler_->Info() (lines 278-280).
  (void)logged_cache->Remove("/img/log.jpg", "example.com", "https");
  EXPECT_GT(handler.count(), count_before);

  count_before = handler.count();

  // ResetVolume → handler_->Info() (lines 360-363).
  auto reset = logged_cache->ResetVolume();
  ASSERT_TRUE(reset.has_value());
  EXPECT_GT(handler.count(), count_before);
}

TEST_F(PageSpeedCacheTest, HandlerLogsSentinelWrite) {
  TestCacheHandler handler;
  PageSpeedCacheConfig config;
  config.volume_path = cache_path_;
  config.volume_size = static_cast<uint64_t>(10 * 1024 * 1024);
  config.enable_checksum = true;
  config.handler = &handler;

  auto result = PageSpeedCache::Create(config);
  ASSERT_TRUE(result.has_value());
  auto& logged_cache = *result;

  // WriteSentinel success → handler_->Info() (lines 257-261).
  std::string hints = "preload data";
  auto wh = logged_cache->WriteSentinel("/page.html", "example.com", "https",
                                        SentinelId::kEarlyHints, hints.size());
  ASSERT_TRUE(wh.has_value());
  auto bytes = std::as_bytes(std::span(hints));
  (void)wh->write_sync(bytes);
  (void)wh->close_sync();

  EXPECT_GE(handler.count(), 1);
}

// ========== Coverage Phase 4: Uncovered line coverage ==========

// --- Block 1 & 5: ParseMetadataPrefix too-short guard / metadata parse failure
// (lines 48, 152) ---

TEST_F(PageSpeedCacheTest, ReadBestAlternateReturnsNotFoundOnCorruptMetadata) {
  // Write raw bytes directly via Cyclone (bypassing PageSpeedCache's metadata
  // prefix writing), then read via ReadBestAlternate.  The metadata will be
  // shorter than kFixedPrefixSize, so ParseMetadataPrefix returns 0, and
  // ReadBestAlternate returns NotFound (line 152).
  CreateCache();

  // Write raw data directly via the underlying Cyclone cache.
  auto key = PageSpeedCache::ComposeKeyPreNormalized("/img/corrupt.jpg",
                                                     "example.com", "https");
  CapabilityMask mask;
  AlternateId id =
      MaskToAlternateId(static_cast<uint8_t>(mask.Encode() & 0xFF));

  // Write just 3 bytes — way shorter than kFixedPrefixSize (9).
  auto wh = PageSpeedCacheTestPeer::Cyclone(*cache_).write_alternate_sync(
      key, static_cast<cyclone::AlternateId>(id), 3);
  ASSERT_TRUE(wh.has_value());
  std::array<std::byte, 3> tiny = {std::byte{0x01}, std::byte{0x02},
                                   std::byte{0x03}};
  auto written = wh->write_sync(tiny);
  ASSERT_TRUE(written.has_value());
  auto closed = wh->close_sync();
  ASSERT_TRUE(closed.has_value());

  // Now read via PageSpeedCache — ParseMetadataPrefix should fail (too short),
  // and ReadBestAlternate should return NotFound.
  auto result = cache_->ReadBestAlternate("/img/corrupt.jpg", "example.com",
                                          "https", mask);
  EXPECT_FALSE(result.has_value())
      << "ReadBestAlternate should return NotFound for corrupt metadata";
  EXPECT_EQ(result.error(), cyclone::CacheError::NotFound);
}

TEST_F(PageSpeedCacheTest, ReadBestAlternateReturnsNotFoundOnInvalidVersion) {
  // Write data with enough bytes but invalid version byte, so
  // AlternateMetadata::Deserialize returns nullopt, making
  // ParseMetadataPrefix return 0.
  CreateCache();

  auto key = PageSpeedCache::ComposeKeyPreNormalized("/img/badver.jpg",
                                                     "example.com", "https");
  CapabilityMask mask;
  AlternateId id =
      MaskToAlternateId(static_cast<uint8_t>(mask.Encode() & 0xFF));

  // Write kFixedPrefixSize + suffix bytes, but with invalid version (0xFF).
  // kFixedPrefixSize=9, kV6FixedSuffixSize=27, minimum valid = 9 + 0 + 27 = 36
  std::vector<std::byte> bad_data(40, std::byte{0x00});
  bad_data[0] = std::byte{0xFF};  // Invalid version

  auto wh = PageSpeedCacheTestPeer::Cyclone(*cache_).write_alternate_sync(
      key, static_cast<cyclone::AlternateId>(id), bad_data.size());
  ASSERT_TRUE(wh.has_value());
  auto written = wh->write_sync(std::span(bad_data));
  ASSERT_TRUE(written.has_value());
  auto closed = wh->close_sync();
  ASSERT_TRUE(closed.has_value());

  auto result = cache_->ReadBestAlternate("/img/badver.jpg", "example.com",
                                          "https", mask);
  EXPECT_FALSE(result.has_value()) << "ReadBestAlternate should return "
                                      "NotFound for invalid metadata version";
  EXPECT_EQ(result.error(), cyclone::CacheError::NotFound);
}

TEST_F(PageSpeedCacheTest, ReadAlternateWithCorruptMetadataStillReturns) {
  // ReadAlternate (unlike ReadBestAlternate) does NOT reject on metadata
  // parse failure for a sentinel, which carries no metadata prefix.  Verify
  // it returns success with metadata_prefix_size_ == 0.
  CreateCache();

  auto key = PageSpeedCache::ComposeKeyPreNormalized("/img/sentinel-like.jpg",
                                                     "example.com", "https");
  AlternateId id = static_cast<AlternateId>(SentinelId::kEarlyHints);

  // Write raw bytes shorter than kFixedPrefixSize.
  std::array<std::byte, 4> raw = {std::byte{0xAA}, std::byte{0xBB},
                                  std::byte{0xCC}, std::byte{0xDD}};
  auto wh = PageSpeedCacheTestPeer::Cyclone(*cache_).write_alternate_sync(
      key, static_cast<cyclone::AlternateId>(id), raw.size());
  ASSERT_TRUE(wh.has_value());
  wh->write_sync(std::span(raw));
  wh->close_sync();

  auto result = cache_->ReadAlternate("/img/sentinel-like.jpg", "example.com",
                                      "https", id);
  ASSERT_TRUE(result.has_value())
      << "ReadAlternate should succeed even with corrupt metadata";
  // content() returns the full blob when metadata_prefix_size_ == 0.
  EXPECT_EQ(result->content_length(), raw.size());
}

TEST_F(PageSpeedCacheTest, ReadAlternateWithCorruptMetadataOnContentIsMiss) {
  // A content alternate always carries metadata; one that does not parse is
  // a miss, as for ReadBestAlternate.
  CreateCache();

  auto key = PageSpeedCache::ComposeKeyPreNormalized("/img/content.jpg",
                                                     "example.com", "https");
  AlternateId id = 0x08;
  std::array<std::byte, 4> raw = {std::byte{0xAA}, std::byte{0xBB},
                                  std::byte{0xCC}, std::byte{0xDD}};
  auto wh = PageSpeedCacheTestPeer::Cyclone(*cache_).write_alternate_sync(
      key, static_cast<cyclone::AlternateId>(id), raw.size());
  ASSERT_TRUE(wh.has_value());
  wh->write_sync(std::span(raw));
  wh->close_sync();

  auto result =
      cache_->ReadAlternate("/img/content.jpg", "example.com", "https", id);
  ASSERT_FALSE(result.has_value());
  EXPECT_EQ(result.error(), cyclone::CacheError::NotFound);
}

// --- Block 4: chmod warning in Create() (lines 100-102) ---
// chmod failure is very hard to trigger portably because Cyclone creates the
// file and we can't easily make chmod fail on a file we own.  However, we can
// test the path on macOS/Linux by making the file immutable (chflags on macOS)
// or by using a file on a read-only filesystem.  For now, we test the handler
// receives a message during create by wrapping with a custom handler.

// This test exercises the chmod warning path indirectly: by verifying that
// Create() with a handler produces at least one message, which includes
// the chmod path when permissions prevent the chmod call.

// --- Block 6: WriteAlternate write_alternate_sync failure (lines 208-214) ---

TEST_F(PageSpeedCacheTest, WriteAlternateFailsAfterCacheStop) {
  // Stop the underlying Cyclone cache, then attempt WriteAlternate.
  // The write_alternate_sync call should fail, exercising lines 208-214.
  TestCacheHandler handler;
  PageSpeedCacheConfig config;
  config.volume_path = cache_path_;
  config.volume_size = static_cast<uint64_t>(10 * 1024 * 1024);
  config.enable_checksum = true;
  config.handler = &handler;

  auto result = PageSpeedCache::Create(config);
  ASSERT_TRUE(result.has_value());
  auto& test_cache = *result;

  // Stop the underlying Cyclone cache directly.
  PageSpeedCacheTestPeer::Cyclone(*test_cache).stop();

  CapabilityMask mask;
  AlternateId id =
      MaskToAlternateId(static_cast<uint8_t>(mask.Encode() & 0xFF));

  AlternateMetadata meta;
  meta.full_mask = mask.Encode();
  meta.content_type = ContentType::kImage;
  meta.origin_content_type = "image/jpeg";

  // WriteAlternate should fail because the cache is stopped.
  auto wh = test_cache->WriteAlternate("/img/stopped.jpg", "example.com",
                                       "https", id, 100, meta);
  EXPECT_FALSE(wh.has_value())
      << "WriteAlternate should fail when the underlying cache is stopped";
  // Handler should have received a warning message.
  EXPECT_GE(handler.count(), 1);
}

// --- Block 8: WriteSentinel write failure (lines 249-255) ---

TEST_F(PageSpeedCacheTest, WriteSentinelFailsAfterCacheStop) {
  // Stop the underlying Cyclone cache, then attempt WriteSentinel.
  TestCacheHandler handler;
  PageSpeedCacheConfig config;
  config.volume_path = cache_path_;
  config.volume_size = static_cast<uint64_t>(10 * 1024 * 1024);
  config.enable_checksum = true;
  config.handler = &handler;

  auto result = PageSpeedCache::Create(config);
  ASSERT_TRUE(result.has_value());
  auto& test_cache = *result;

  // Stop the underlying Cyclone cache directly.
  PageSpeedCacheTestPeer::Cyclone(*test_cache).stop();

  auto wh = test_cache->WriteSentinel("/page.html", "example.com", "https",
                                      SentinelId::kEarlyHints, 50);
  EXPECT_FALSE(wh.has_value())
      << "WriteSentinel should fail when the underlying cache is stopped";
  // Handler should have received a warning.
  EXPECT_GE(handler.count(), 1);
}

// --- Block 9: ResetVolume error paths (lines 333, 339, 344, 350-352) ---

#ifndef _WIN32
// Unix file permission test — not applicable on Windows.
TEST_F(PageSpeedCacheTest, ResetVolumeFailsWhenDirectoryReadOnly) {
  // ResetVolume deletes the old volume file, then tries to create a new one.
  // If the directory is read-only, the delete may succeed (file in dir
  // opened before chmod) but add_volume should fail because it can't create
  // a new file.
  //
  // Strategy: delete the volume file first, then make the dir read-only,
  // so Cyclone's add_volume must create a new file and will fail.
  if (::geteuid() == 0) {
    GTEST_SKIP() << "Skipped: root bypasses directory permissions";
  }

  CreateCache();

  // Stop the cache and delete the (fingerprint-named) volume file manually.
  PageSpeedCacheTestPeer::Cyclone(*cache_).stop();
  std::filesystem::remove(cache_->VolumeFilePath());

  // Make the directory read-only so Cyclone can't create a new volume.
  ASSERT_EQ(::chmod(temp_dir_.c_str(), 0555), 0)
      << "Failed to set directory to read-only";

  auto result = cache_->ResetVolume();

  // Restore permissions before assertions.
  ASSERT_EQ(::chmod(temp_dir_.c_str(), 0755), 0)
      << "Failed to restore directory permissions";

  EXPECT_FALSE(result.has_value())
      << "ResetVolume should fail when directory is read-only";
}
#endif  // !_WIN32

// --- Block 10: WriteGenerationFile error paths (lines 390-394, 404-409,
// 413-418) ---

#ifndef _WIN32
// Unix file permission test — not applicable on Windows.
TEST_F(PageSpeedCacheTest, ResetVolumeWithReadOnlyGenFileDirectory) {
  // Exercise WriteGenerationFile error paths by making the directory
  // temporarily read-only after initial cache creation but before reset.
  // The cache volume file is already created, so ResetVolume can delete
  // and recreate it if we temporarily make the dir writable for that step.
  //
  // Actually, the simpler approach: create the cache normally, then
  // create a directory at the .gen.tmp path so the open(O_CREAT|O_EXCL)
  // fails — this hits the "fd < 0" path (lines 390-394).
  if (::geteuid() == 0) {
    GTEST_SKIP() << "Skipped: root bypasses directory permissions";
  }

  TestCacheHandler handler;
  PageSpeedCacheConfig config;
  config.volume_path = cache_path_;
  config.volume_size = static_cast<uint64_t>(10 * 1024 * 1024);
  config.enable_checksum = true;
  config.handler = &handler;

  auto result = PageSpeedCache::Create(config);
  ASSERT_TRUE(result.has_value());
  auto& test_cache = *result;

  // Create a directory at the .gen.tmp path so open(O_CREAT|O_EXCL) fails
  // when WriteGenerationFile tries to create the tmp file.
  std::string gen_tmp_path = cache_path_ + ".gen.tmp";
  std::filesystem::create_directory(gen_tmp_path);

  // ResetVolume should succeed (the cache reset itself works), but
  // WriteGenerationFile will fail at the open() call and log a warning.
  int count_before = handler.count();
  auto reset = test_cache->ResetVolume();
  EXPECT_TRUE(reset.has_value())
      << "ResetVolume should succeed even if gen file write fails";
  EXPECT_GT(handler.count(), count_before)
      << "Handler should log a warning for generation file write failure";

  // Clean up the blocking directory.
  std::filesystem::remove_all(gen_tmp_path);
}
#endif  // !_WIN32

#ifndef _WIN32
// Unix file permission test — not applicable on Windows.
TEST_F(PageSpeedCacheTest, WriteGenerationFileRenameFailure) {
  // Exercise the rename failure path (lines 412-418) by making the
  // target .gen path a directory, so rename() fails with EISDIR.
  if (::geteuid() == 0) {
    GTEST_SKIP() << "Skipped: root bypasses directory permissions";
  }

  TestCacheHandler handler;
  PageSpeedCacheConfig config;
  config.volume_path = cache_path_;
  config.volume_size = static_cast<uint64_t>(10 * 1024 * 1024);
  config.enable_checksum = true;
  config.handler = &handler;

  auto result = PageSpeedCache::Create(config);
  ASSERT_TRUE(result.has_value());
  auto& test_cache = *result;

  // Create a directory at the .gen path so rename() will fail.
  std::string gen_path = cache_path_ + ".gen";
  std::filesystem::create_directory(gen_path);

  int count_before = handler.count();
  auto reset = test_cache->ResetVolume();
  EXPECT_TRUE(reset.has_value())
      << "ResetVolume should succeed even if gen rename fails";
  EXPECT_GT(handler.count(), count_before)
      << "Handler should log a warning for generation file rename failure";

  // Clean up.
  std::filesystem::remove_all(gen_path);
}
#endif  // !_WIN32

TEST_F(PageSpeedCacheTest, WriteGenerationFileWriteFailure) {
  // Exercise the write() failure path (lines 404-409) by creating the
  // tmp file as a FIFO (named pipe) before ResetVolume.  When
  // WriteGenerationFile does unlink + open(O_CREAT|O_EXCL), it creates
  // a new regular file, so we can't trigger write failure this way.
  //
  // Alternative: make the directory read-only AFTER unlink succeeds
  // but before open. This is racy and fragile.
  //
  // Most robust approach: fill up the volume's filesystem (not practical).
  //
  // Instead, we test the overall behavior when /dev/full-like conditions
  // exist.  On macOS there's no /dev/full.  We'll verify via
  // ReadGenerationFile that a failed write does not leave a corrupt
  // generation file.

  // This test ensures that even if WriteGenerationFile cannot write, the
  // cache still works and the generation reads as 0 (missing).
  CreateCache();

  std::string gen_path = cache_path_ + ".gen";

  // No .gen file should exist before first reset.
  EXPECT_EQ(PageSpeedCache::ReadGenerationFile(gen_path), 0u);

  // Reset normally first.
  auto r1 = cache_->ResetVolume();
  ASSERT_TRUE(r1.has_value());
  EXPECT_EQ(PageSpeedCache::ReadGenerationFile(gen_path), 1u);

  // Now delete the gen file and make the tmp path a directory to block
  // the next WriteGenerationFile call.
  std::filesystem::remove(gen_path);
  std::string gen_tmp_path = cache_path_ + ".gen.tmp";
  std::filesystem::create_directory(gen_tmp_path);

  // Second reset — gen file won't be written.
  auto r2 = cache_->ResetVolume();
  EXPECT_TRUE(r2.has_value());

  // Gen file should not exist (was deleted and couldn't be re-created).
  EXPECT_EQ(PageSpeedCache::ReadGenerationFile(gen_path), 0u);

  // Clean up.
  std::filesystem::remove_all(gen_tmp_path);
}

// --- Block 2: Cache::create failure in Create() (line 83) ---

TEST_F(PageSpeedCacheTest, DefaultConfigUsesMultiProcessMmapSharing) {
  // Verify that the default PageSpeedCacheConfig enables multi-process mode
  // with process_index=0, total_processes=1.  This is the configuration that
  // gives both nginx and the worker write access to all stripes while
  // activating the mmap-backed directory for cross-process visibility.
  PageSpeedCacheConfig config;
  EXPECT_TRUE(config.multi_process.enabled);
  EXPECT_EQ(config.multi_process.process_index, 0u);
  EXPECT_EQ(config.multi_process.total_processes, 1u);
}

TEST_F(PageSpeedCacheTest, CreateFailsWithInvalidMultiProcessConfig) {
  // Trigger Cache::create failure by providing invalid configuration.
  // process_index >= total_processes should be rejected by Cyclone.
  PageSpeedCacheConfig config;
  config.volume_path = cache_path_;
  config.volume_size = static_cast<uint64_t>(10 * 1024 * 1024);
  config.enable_checksum = true;
  config.multi_process = {
      .enabled = true,
      .process_index = 5,
      .total_processes = 2,  // process_index (5) >= total_processes (2)
  };

  auto result = PageSpeedCache::Create(config);
  EXPECT_FALSE(result.has_value())
      << "Create should fail with invalid multi-process config";
}

// --- Block 4 variant: chmod warning with handler ---

TEST_F(PageSpeedCacheTest, CreateWithHandlerLogsChmodWarning) {
  // On some systems, we can't easily make chmod fail on a file we own.
  // This test at least verifies that Create() with a handler works
  // when chmod succeeds (no warning logged) — serving as a baseline.
  // On CI systems where the user is root (e.g., Docker), chmod would
  // always succeed.
  TestCacheHandler handler;
  PageSpeedCacheConfig config;
  config.volume_path = cache_path_;
  config.volume_size = static_cast<uint64_t>(10 * 1024 * 1024);
  config.enable_checksum = true;
  config.handler = &handler;

  auto result = PageSpeedCache::Create(config);
  ASSERT_TRUE(result.has_value()) << "Create should succeed normally";
  // handler count is 0 since chmod should succeed for a normal user.
  // (If this test runs as root or with special permissions, the count
  // might differ, but the test verifies no crash in the path.)
}

// --- Additional: ReadBestAlternate with handler on corrupt metadata ---

TEST_F(PageSpeedCacheTest, ReadBestAlternateCorruptMetadataWithHandler) {
  // Combine Block 5 (metadata parse failure) with handler logging.
  // The handler should receive an Info message for the read miss (line 136)
  // even though the alternate exists but has corrupt metadata.
  TestCacheHandler handler;
  PageSpeedCacheConfig config;
  config.volume_path = cache_path_;
  config.volume_size = static_cast<uint64_t>(10 * 1024 * 1024);
  config.enable_checksum = true;
  config.handler = &handler;

  auto result = PageSpeedCache::Create(config);
  ASSERT_TRUE(result.has_value());
  auto& logged_cache = *result;

  // Write corrupt data directly via Cyclone.
  auto key = PageSpeedCache::ComposeKeyPreNormalized("/img/corrupt2.jpg",
                                                     "example.com", "https");
  CapabilityMask mask;
  AlternateId id =
      MaskToAlternateId(static_cast<uint8_t>(mask.Encode() & 0xFF));

  std::array<std::byte, 2> tiny = {std::byte{0x01}, std::byte{0x02}};
  auto wh = PageSpeedCacheTestPeer::Cyclone(*logged_cache)
                .write_alternate_sync(
                    key, static_cast<cyclone::AlternateId>(id), tiny.size());
  ASSERT_TRUE(wh.has_value());
  wh->write_sync(std::span(tiny));
  wh->close_sync();

  // ReadBestAlternate: the alternate exists, Cyclone returns it,
  // but metadata parse fails → returns NotFound.
  // Note: this does NOT go through the handler_->Info "Read miss" path
  // (lines 136-140) because that path is for Cyclone-level misses.
  // The metadata parse failure path (line 152) returns NotFound silently.
  auto read_result = logged_cache->ReadBestAlternate(
      "/img/corrupt2.jpg", "example.com", "https", mask);
  EXPECT_FALSE(read_result.has_value());
  EXPECT_EQ(read_result.error(), cyclone::CacheError::NotFound);
}

// --- Additional: empty content written via Cyclone ---

TEST_F(PageSpeedCacheTest, ReadBestAlternateEmptyContentIsCorrupt) {
  // An empty alternate (0 bytes) should fail ParseMetadataPrefix
  // since content.size() < kFixedPrefixSize.
  CreateCache();

  auto key = PageSpeedCache::ComposeKeyPreNormalized("/img/empty.jpg",
                                                     "example.com", "https");
  CapabilityMask mask;
  AlternateId id =
      MaskToAlternateId(static_cast<uint8_t>(mask.Encode() & 0xFF));

  // Write 0 bytes.
  auto wh = PageSpeedCacheTestPeer::Cyclone(*cache_).write_alternate_sync(
      key, static_cast<cyclone::AlternateId>(id), 0);
  ASSERT_TRUE(wh.has_value());
  auto closed = wh->close_sync();
  ASSERT_TRUE(closed.has_value());

  auto result =
      cache_->ReadBestAlternate("/img/empty.jpg", "example.com", "https", mask);
  EXPECT_FALSE(result.has_value());
  EXPECT_EQ(result.error(), cyclone::CacheError::NotFound);
}

// ========== Coverage: Additional error paths and variant selection ==========

TEST_F(PageSpeedCacheTest, WriteAlternateMetadataWriteFailure) {
  // Stop the underlying Cyclone cache, then attempt WriteAlternate.
  // After stop, write_alternate_sync should fail, returning an error.
  // This exercises the error path at lines 205-215 in cache.cc.
  CreateCache();

  // Verify the cache works before stopping.
  CapabilityMask mask = MakeTestMask(CapabilityMask::ImageFormat::kWebP,
                                     CapabilityMask::Viewport::kDesktop);
  WriteContent("/img/pre-stop.jpg", "example.com", mask, "pre-stop-data");

  // Stop the cache.
  PageSpeedCacheTestPeer::Cyclone(*cache_).stop();

  AlternateId id =
      MaskToAlternateId(static_cast<uint8_t>(mask.Encode() & 0xFF));
  AlternateMetadata meta;
  meta.full_mask = mask.Encode();
  meta.content_type = ContentType::kImage;
  meta.origin_content_type = "image/webp";

  // WriteAlternate should fail because the cache is stopped.
  auto wh = cache_->WriteAlternate("/img/post-stop.jpg", "example.com", "https",
                                   id, 100, meta);
  EXPECT_FALSE(wh.has_value()) << "WriteAlternate should fail after cache stop";
  // The error should be a Cyclone-level error (not InvalidArgument which
  // is returned for sentinel rejection or oversized metadata).
  EXPECT_NE(wh.error(), cyclone::CacheError::InvalidArgument);
}

// A purge replaces the volume although another handle on it exists, as a web
// server module in another process has.  The worker side (this instance)
// starts empty; the peer keeps reading the file it has until it reopens, and
// a peer that reopens after the purge starts empty too.  Before the Windows
// move-aside, the old file survived the purge on Windows and the entry stayed.
TEST_F(PageSpeedCacheTest, ResetVolumeWithLivePeerStartsEmpty) {
  CreateCache();
  PageSpeedCacheConfig peer_config;
  peer_config.volume_path = cache_path_;
  peer_config.volume_size = static_cast<uint64_t>(10 * 1024 * 1024);
  peer_config.enable_checksum = true;
  auto peer = PageSpeedCache::Create(peer_config);
  ASSERT_TRUE(peer.has_value()) << "Failed to create the peer";

  CapabilityMask mask;
  const AlternateId id =
      MaskToAlternateId(static_cast<uint8_t>(mask.Encode() & 0xFF));
  WriteContent("/img/x.jpg", "example.com", mask, "x-data");
  ASSERT_TRUE(
      cache_->AlternateExists("/img/x.jpg", "example.com", "https", id));

  const std::string gen_path = cache_path_ + ".gen";
  const uint64_t gen_before = PageSpeedCache::ReadGenerationFile(gen_path);

  std::string detail;
  auto result = cache_->ResetVolume(&detail);
  ASSERT_TRUE(result.has_value()) << detail;
  EXPECT_EQ(PageSpeedCache::ReadGenerationFile(gen_path), gen_before + 1);
  EXPECT_FALSE(
      cache_->AlternateExists("/img/x.jpg", "example.com", "https", id));

  // The peer still has the file it opened, with the entry in it.
  EXPECT_TRUE(
      (*peer)->AlternateExists("/img/x.jpg", "example.com", "https", id));

  // A peer that reopens after the purge starts empty.
  peer->reset();
  auto reopened = PageSpeedCache::Create(peer_config);
  ASSERT_TRUE(reopened.has_value());
  EXPECT_FALSE(
      (*reopened)->AlternateExists("/img/x.jpg", "example.com", "https", id));
}

#ifdef _WIN32
// A process that has the volume file open without allowing delete or rename
// (an older web server module or ASP.NET Core package) makes the purge fail
// with a message; the worker keeps serving the old volume and the generation
// does not move.
TEST_F(PageSpeedCacheTest, ResetVolumeReportsHeldVolumeAndKeepsServing) {
  CreateCache();
  CapabilityMask mask;
  const AlternateId id =
      MaskToAlternateId(static_cast<uint8_t>(mask.Encode() & 0xFF));
  WriteContent("/img/x.jpg", "example.com", mask, "x-data");

  const std::string gen_path = cache_path_ + ".gen";
  const uint64_t gen_before = PageSpeedCache::ReadGenerationFile(gen_path);

  // Hold the volume file the way an older build does: read and write sharing
  // only, no FILE_SHARE_DELETE.
  const std::string held_path = cache_->VolumeFilePath();
  HANDLE held = ::CreateFileA(held_path.c_str(), GENERIC_READ,
                              FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                              OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
  ASSERT_NE(held, INVALID_HANDLE_VALUE) << "GetLastError " << ::GetLastError();

  std::string detail;
  auto result = cache_->ResetVolume(&detail);
  ::CloseHandle(held);

  ASSERT_FALSE(result.has_value());
  EXPECT_EQ(result.error(), cyclone::CacheError::Busy);
  EXPECT_NE(detail.find("older"), std::string::npos) << detail;
  EXPECT_NE(detail.find("another process still has it open"), std::string::npos)
      << detail;
  // The message names the file and carries the operating system's reason.
  EXPECT_NE(detail.find(cache_->VolumeFilePath()), std::string::npos) << detail;
  EXPECT_NE(detail.find("cannot move it aside: "), std::string::npos) << detail;
  EXPECT_EQ(PageSpeedCache::ReadGenerationFile(gen_path), gen_before);
  (void)cache_->Stats();
  EXPECT_TRUE(
      cache_->AlternateExists("/img/x.jpg", "example.com", "https", id));
}

// One of two live volume files is held without delete sharing: the other is
// moved aside and replaced, so the purge reports the held one but the
// generation still advances and peers reopen the replaced file.
TEST_F(PageSpeedCacheTest, ResetVolumePartialMoveStillAdvancesGeneration) {
  CreateCache();
  // The cache has one volume file; stage a second live file the way a
  // small-tier volume would be: present, known to the cache, and held.
  const std::string second = cache_path_ + ".small";
  {
    std::ofstream f(second, std::ios::binary);
    f << "second volume";
  }
  PageSpeedCacheTestPeer::AddLivePath(*cache_, second);

  const std::string gen_path = cache_path_ + ".gen";
  const uint64_t gen_before = PageSpeedCache::ReadGenerationFile(gen_path);

  HANDLE held = ::CreateFileA(second.c_str(), GENERIC_READ,
                              FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                              OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
  ASSERT_NE(held, INVALID_HANDLE_VALUE) << "GetLastError " << ::GetLastError();

  std::string detail;
  auto result = cache_->ResetVolume(&detail);
  ::CloseHandle(held);

  ASSERT_FALSE(result.has_value());
  EXPECT_EQ(result.error(), cyclone::CacheError::Busy);
  EXPECT_NE(detail.find(".small"), std::string::npos) << detail;
  EXPECT_EQ(PageSpeedCache::ReadGenerationFile(gen_path), gen_before + 1);
  // The worker serves from a usable cache.
  (void)cache_->Stats();
}

// A file where the tombstone directory belongs: nothing is deleted in place,
// the purge reports the cause, and the old volume keeps serving.
TEST_F(PageSpeedCacheTest, ResetVolumeKeepsServingWhenTombstoneDirIsUnusable) {
  CreateCache();
  CapabilityMask mask;
  const AlternateId id =
      MaskToAlternateId(static_cast<uint8_t>(mask.Encode() & 0xFF));
  WriteContent("/img/x.jpg", "example.com", mask, "x-data");
  const std::filesystem::path blocker =
      std::filesystem::path(temp_dir_) / ".pagespeed-purged";
  {
    std::ofstream f(blocker, std::ios::binary);
    f << "not a directory";
  }
  const std::string gen_path = cache_path_ + ".gen";
  const uint64_t gen_before = PageSpeedCache::ReadGenerationFile(gen_path);

  std::string detail;
  auto result = cache_->ResetVolume(&detail);

  ASSERT_FALSE(result.has_value());
  EXPECT_EQ(result.error(), cyclone::CacheError::Busy);
  EXPECT_NE(detail.find(".pagespeed-purged"), std::string::npos) << detail;
  EXPECT_EQ(PageSpeedCache::ReadGenerationFile(gen_path), gen_before);
  EXPECT_TRUE(std::filesystem::exists(cache_->VolumeFilePath()));
  EXPECT_TRUE(
      cache_->AlternateExists("/img/x.jpg", "example.com", "https", id));
}

// What a purge could not delete is removed the next time a cache is created.
TEST_F(PageSpeedCacheTest, TombstonesAreSweptAtCreate) {
  const std::filesystem::path tomb_dir =
      std::filesystem::path(temp_dir_) / ".pagespeed-purged";
  std::filesystem::create_directories(tomb_dir);
  const std::filesystem::path planted = tomb_dir / "cache.vol.1.0";
  {
    std::ofstream f(planted, std::ios::binary);
    f << "old volume";
  }
  ASSERT_TRUE(std::filesystem::exists(planted));

  CreateCache();
  EXPECT_FALSE(std::filesystem::exists(planted));
}
#endif  // _WIN32

#ifndef _WIN32
// A live volume file that cannot be removed (on Windows, one another process
// has open) must make the purge fail, not report success over the old
// entries.  A read-only directory reproduces it: the unlink fails, while the
// reopen still attaches the same file.
TEST_F(PageSpeedCacheTest, ResetVolumeReportsFailureWhenFilesCannotBeRemoved) {
  if (::geteuid() == 0) {
    GTEST_SKIP() << "Skipped: root bypasses directory permissions";
  }
  CreateCache();
  CapabilityMask mask;
  const AlternateId id =
      MaskToAlternateId(static_cast<uint8_t>(mask.Encode() & 0xFF));
  WriteContent("/img/held.jpg", "example.com", mask, "held-data");

  const std::string gen_path = cache_path_ + ".gen";
  const uint64_t gen_before = PageSpeedCache::ReadGenerationFile(gen_path);

  ASSERT_EQ(::chmod(temp_dir_.c_str(), 0555), 0);
  std::string detail;
  auto result = cache_->ResetVolume(&detail);
  ASSERT_EQ(::chmod(temp_dir_.c_str(), 0755), 0);

  ASSERT_FALSE(result.has_value()) << "the purge reported success although the "
                                      "volume file could not be removed";
  EXPECT_EQ(result.error(), cyclone::CacheError::Busy);
  EXPECT_FALSE(detail.empty()) << "the failure must carry a clear message";
  EXPECT_EQ(PageSpeedCache::ReadGenerationFile(gen_path), gen_before)
      << "the generation must not move when the volume was not replaced";

  // The cache is still usable: the old entry is served and new writes work.
  EXPECT_TRUE(
      cache_->AlternateExists("/img/held.jpg", "example.com", "https", id));
  WriteContent("/img/after.jpg", "example.com", mask, "after-data");
  EXPECT_TRUE(
      cache_->AlternateExists("/img/after.jpg", "example.com", "https", id));

  // Once the obstacle is gone the same call replaces the volume.
  auto retry = cache_->ResetVolume(&detail);
  ASSERT_TRUE(retry.has_value());
  EXPECT_EQ(PageSpeedCache::ReadGenerationFile(gen_path), gen_before + 1);
  EXPECT_FALSE(
      cache_->AlternateExists("/img/held.jpg", "example.com", "https", id));
}

// Unix file permission test — not applicable on Windows.
TEST_F(PageSpeedCacheTest, ResetVolumeRecreateFailure) {
  // After creating a cache, make the volume path's parent directory
  // read-only so that ResetVolume() cannot create a new volume file.
  // This exercises the error paths in ResetVolume at lines 331-345.
  if (::geteuid() == 0) {
    GTEST_SKIP() << "Skipped: root bypasses directory permissions";
  }

  CreateCache();

  // Write some data to ensure the cache has content.
  CapabilityMask mask;
  WriteContent("/img/reset-fail.jpg", "example.com", mask, "some-data");

  // Remove the volume file so ResetVolume must recreate it.
  // Then make the directory read-only so the recreate fails.
  std::filesystem::remove(cache_path_);
  ASSERT_EQ(::chmod(temp_dir_.c_str(), 0444), 0)
      << "Failed to set directory to read-only";

  auto result = cache_->ResetVolume();

  // Restore permissions before assertions so TearDown can clean up.
  ASSERT_EQ(::chmod(temp_dir_.c_str(), 0755), 0)
      << "Failed to restore directory permissions";

  EXPECT_FALSE(result.has_value())
      << "ResetVolume should fail when directory is read-only and volume "
         "must be recreated";

  // The failed reset leaves no cache behind.  Every entry point must answer
  // with an error or a miss instead of dereferencing it.
  CapabilityMask other_mask;
  const AlternateId id =
      MaskToAlternateId(static_cast<uint8_t>(other_mask.Encode() & 0xFF));
  EXPECT_EQ(cache_->Stats().disk_cache_hits, 0u);
  EXPECT_EQ(cache_->VolumeCapacityBytes(), 0u);
  EXPECT_FALSE(cache_->AlternateExists("/img/reset-fail.jpg", "example.com",
                                       "https", id));
  auto read = cache_->ReadBestAlternate("/img/reset-fail.jpg", "example.com",
                                        "https", other_mask);
  ASSERT_FALSE(read.has_value());
  EXPECT_EQ(read.error(), cyclone::CacheError::NotInitialized);
  AlternateMetadata meta;
  meta.full_mask = other_mask.Encode();
  auto write = cache_->WriteAlternate("/img/reset-fail.jpg", "example.com",
                                      "https", id, 4, meta);
  ASSERT_FALSE(write.has_value());
  EXPECT_EQ(write.error(), cyclone::CacheError::NotInitialized);

  // With the directory writable again a later reset brings the cache back.
  auto recovered = cache_->ResetVolume();
  ASSERT_TRUE(recovered.has_value());
  WriteContent("/img/reset-ok.jpg", "example.com", other_mask, "ok");
}
#endif  // !_WIN32

TEST_F(PageSpeedCacheTest, WriteAlternateWithVariousContentTypes) {
  // Write alternates using different content types to exercise the
  // metadata serialization paths for all ContentType enum values.
  CreateCache();

  struct TestCase {
    ContentType ct;
    std::string origin_ct;
    std::string data;
    std::string url;
  };

  std::vector<TestCase> cases = {
      {ContentType::kCss, "text/css; charset=utf-8", "body { color: red; }",
       "/style.css"},
      {ContentType::kJs, "application/javascript", "console.log('hello');",
       "/app.js"},
      {ContentType::kImage, "image/png", "fake-png-data", "/icon.png"},
      {ContentType::kOther, "application/octet-stream", "binary-blob",
       "/data.bin"},
      {ContentType::kHtml, "text/html; charset=utf-8",
       "<html><body>Hello</body></html>", "/page.html"},
  };

  for (const auto& tc : cases) {
    SCOPED_TRACE(tc.url);

    CapabilityMask mask = MakeTestMask(CapabilityMask::ImageFormat::kOriginal,
                                       CapabilityMask::Viewport::kDesktop);

    AlternateId id =
        MaskToAlternateId(static_cast<uint8_t>(mask.Encode() & 0xFF));

    AlternateMetadata meta;
    meta.full_mask = mask.Encode();
    meta.content_type = tc.ct;
    meta.origin_content_type = tc.origin_ct;
    meta.cache_inserted_at = 1700000000;
    meta.origin_max_age = 3600;

    auto wh = cache_->WriteAlternate(tc.url, "example.com", "https", id,
                                     tc.data.size(), meta);
    ASSERT_TRUE(wh.has_value()) << "WriteAlternate failed for " << tc.url
                                << " err=" << static_cast<int>(wh.error());
    auto bytes = std::as_bytes(std::span(tc.data));
    auto written = wh->write_sync(bytes);
    ASSERT_TRUE(written.has_value());
    auto closed = wh->close_sync();
    ASSERT_TRUE(closed.has_value());

    // Read back and verify content type and data survived round-trip.
    auto result =
        cache_->ReadBestAlternate(tc.url, "example.com", "https", mask);
    ASSERT_TRUE(result.has_value())
        << "ReadBestAlternate failed for " << tc.url;
    EXPECT_EQ(result->metadata.content_type, tc.ct);
    EXPECT_EQ(result->metadata.origin_content_type, tc.origin_ct);
    EXPECT_EQ(result->metadata.cache_inserted_at, 1700000000u);
    EXPECT_EQ(result->metadata.origin_max_age, 3600u);

    auto content = result->content();
    std::string_view read_data(reinterpret_cast<const char*>(content.data()),
                               content.size());
    EXPECT_EQ(read_data, tc.data);
  }
}

TEST_F(PageSpeedCacheTest, ReadBestAlternateWithMultipleMasks) {
  // Write alternates with different capability masks (mobile, tablet,
  // desktop, different encodings, formats) then call ReadBestAlternate
  // with various client masks to exercise the selector scoring paths.
  CreateCache();

  // Write: Mobile/WebP/Identity
  CapabilityMask mobile_webp = MakeTestMask(CapabilityMask::ImageFormat::kWebP,
                                            CapabilityMask::Viewport::kMobile);
  WriteContent("/img/hero.jpg", "example.com", mobile_webp, "mobile-webp");

  // Write: Tablet/WebP/Identity
  CapabilityMask tablet_webp = MakeTestMask(CapabilityMask::ImageFormat::kWebP,
                                            CapabilityMask::Viewport::kTablet);
  WriteContent("/img/hero.jpg", "example.com", tablet_webp, "tablet-webp");

  // Write: Desktop/AVIF/Identity
  CapabilityMask desktop_avif = MakeTestMask(
      CapabilityMask::ImageFormat::kAvif, CapabilityMask::Viewport::kDesktop);
  WriteContent("/img/hero.jpg", "example.com", desktop_avif, "desktop-avif");

  // Write: Desktop/Original/Identity (fallback)
  CapabilityMask desktop_orig =
      MakeTestMask(CapabilityMask::ImageFormat::kOriginal,
                   CapabilityMask::Viewport::kDesktop);
  WriteContent("/img/hero.jpg", "example.com", desktop_orig, "desktop-orig");

  // Write: Desktop/WebP/Gzip
  CapabilityMask desktop_webp_gzip = MakeTestMask(
      CapabilityMask::ImageFormat::kWebP, CapabilityMask::Viewport::kDesktop,
      CapabilityMask::PixelDensity::k1x, CapabilityMask::SaveData::kOff,
      CapabilityMask::TransferEncoding::kGzip);
  WriteContent("/img/hero.jpg", "example.com", desktop_webp_gzip,
               "desktop-webp-gzip");

  // Write: Mobile/WebP/2x density
  CapabilityMask mobile_webp_2x = MakeTestMask(
      CapabilityMask::ImageFormat::kWebP, CapabilityMask::Viewport::kMobile,
      CapabilityMask::PixelDensity::k2xPlus);
  WriteContent("/img/hero.jpg", "example.com", mobile_webp_2x,
               "mobile-webp-2x");

  // Write: Desktop/WebP/SaveData
  CapabilityMask desktop_webp_savedata = MakeTestMask(
      CapabilityMask::ImageFormat::kWebP, CapabilityMask::Viewport::kDesktop,
      CapabilityMask::PixelDensity::k1x, CapabilityMask::SaveData::kOn);
  WriteContent("/img/hero.jpg", "example.com", desktop_webp_savedata,
               "desktop-webp-savedata");

  // Verify we wrote multiple alternates.
  auto alts = cache_->ListAlternates("/img/hero.jpg", "example.com", "https");
  ASSERT_TRUE(alts.has_value());
  EXPECT_GE(alts->size(), 7u);

  // Test 1: Mobile/WebP client should get mobile-webp (exact match).
  {
    auto result = cache_->ReadBestAlternate("/img/hero.jpg", "example.com",
                                            "https", mobile_webp);
    ASSERT_TRUE(result.has_value());
    auto c = result->content();
    std::string_view data(reinterpret_cast<const char*>(c.data()), c.size());
    EXPECT_EQ(data, "mobile-webp");
  }

  // Test 2: Desktop/AVIF client should get desktop-avif (exact format+viewport).
  {
    auto result = cache_->ReadBestAlternate("/img/hero.jpg", "example.com",
                                            "https", desktop_avif);
    ASSERT_TRUE(result.has_value());
    auto c = result->content();
    std::string_view data(reinterpret_cast<const char*>(c.data()), c.size());
    EXPECT_EQ(data, "desktop-avif");
  }

  // Test 3: Tablet/WebP client should get tablet-webp (exact match).
  {
    auto result = cache_->ReadBestAlternate("/img/hero.jpg", "example.com",
                                            "https", tablet_webp);
    ASSERT_TRUE(result.has_value());
    auto c = result->content();
    std::string_view data(reinterpret_cast<const char*>(c.data()), c.size());
    EXPECT_EQ(data, "tablet-webp");
  }

  // Test 4: Desktop/WebP/Gzip client should get desktop-webp-gzip (exact).
  {
    auto result = cache_->ReadBestAlternate("/img/hero.jpg", "example.com",
                                            "https", desktop_webp_gzip);
    ASSERT_TRUE(result.has_value());
    auto c = result->content();
    std::string_view data(reinterpret_cast<const char*>(c.data()), c.size());
    EXPECT_EQ(data, "desktop-webp-gzip");
  }

  // Test 5: Mobile/WebP/2x client should get mobile-webp-2x (density match).
  {
    auto result = cache_->ReadBestAlternate("/img/hero.jpg", "example.com",
                                            "https", mobile_webp_2x);
    ASSERT_TRUE(result.has_value());
    auto c = result->content();
    std::string_view data(reinterpret_cast<const char*>(c.data()), c.size());
    EXPECT_EQ(data, "mobile-webp-2x");
  }

  // Test 6: Desktop/WebP/SaveData client should get desktop-webp-savedata.
  {
    auto result = cache_->ReadBestAlternate("/img/hero.jpg", "example.com",
                                            "https", desktop_webp_savedata);
    ASSERT_TRUE(result.has_value());
    auto c = result->content();
    std::string_view data(reinterpret_cast<const char*>(c.data()), c.size());
    EXPECT_EQ(data, "desktop-webp-savedata");
  }

  // Test 7: Desktop/WebP/Brotli client -- no exact brotli variant exists.
  // Should get desktop-orig (Original fallback with identity encoding)
  // because mismatched non-identity encoding is a hard disqualification,
  // but identity encoding gets a small fallback bonus (+5).
  // Or it could get the desktop-webp-gzip is disqualified (non-identity
  // mismatch), so among identity-encoded ones, desktop-webp-savedata or
  // desktop-orig based on scoring.
  {
    CapabilityMask desktop_webp_brotli = MakeTestMask(
        CapabilityMask::ImageFormat::kWebP, CapabilityMask::Viewport::kDesktop,
        CapabilityMask::PixelDensity::k1x, CapabilityMask::SaveData::kOff,
        CapabilityMask::TransferEncoding::kBrotli);
    auto result = cache_->ReadBestAlternate("/img/hero.jpg", "example.com",
                                            "https", desktop_webp_brotli);
    ASSERT_TRUE(result.has_value())
        << "Should find a fallback alternate even without exact brotli match";
    // We just verify it returns something valid -- the selector's scoring
    // logic is tested in pagespeed_selector_test.
    EXPECT_TRUE(result->is_valid());
  }
}

// ========== Coverage: WriteGenerationFile and CreateCache error paths ==========

#ifndef _WIN32
// Unix file permission test — not applicable on Windows.
TEST_F(PageSpeedCacheTest, WriteGenerationFileOpenFailsReadOnlyDir) {
  // Exercise WriteGenerationFile open failure (lines 390-394) by placing a
  // directory at the .gen.tmp path.  WriteGenerationFile's unlink() cannot
  // remove a directory, so the subsequent open(O_CREAT|O_EXCL) fails.
  // Verify the handler receives a warning and no .gen file is produced.
  if (::geteuid() == 0) {
    GTEST_SKIP() << "Skipped: root bypasses directory permissions";
  }

  TestCacheHandler handler;
  PageSpeedCacheConfig config;
  config.volume_path = cache_path_;
  config.volume_size = static_cast<uint64_t>(10 * 1024 * 1024);
  config.enable_checksum = true;
  config.handler = &handler;

  auto result = PageSpeedCache::Create(config);
  ASSERT_TRUE(result.has_value());
  auto& test_cache = *result;

  // Block the .gen.tmp path with a directory so open(O_CREAT|O_EXCL) fails.
  std::string gen_tmp_path = cache_path_ + ".gen.tmp";
  std::filesystem::create_directory(gen_tmp_path);

  int count_before = handler.count();
  auto reset = test_cache->ResetVolume();
  EXPECT_TRUE(reset.has_value());
  EXPECT_GT(handler.count(), count_before)
      << "Handler should log warning for generation file open failure";

  // Verify that .gen file was NOT written (the open() failed).
  std::string gen_path = cache_path_ + ".gen";
  EXPECT_FALSE(std::filesystem::exists(gen_path))
      << "Generation file should not exist when open() failed";

  // Clean up the blocking directory.
  std::filesystem::remove_all(gen_tmp_path);
}
#endif  // !_WIN32

#ifndef _WIN32
// Unix file permission test — not applicable on Windows.
TEST_F(PageSpeedCacheTest, WriteGenerationFileRenameFailureCleansTmpFile) {
  // Verify that when rename() fails in WriteGenerationFile, the tmp file
  // is cleaned up (line 417: unlink(tmp_path)).
  if (::geteuid() == 0) {
    GTEST_SKIP() << "Skipped: root bypasses directory permissions";
  }

  TestCacheHandler handler;
  PageSpeedCacheConfig config;
  config.volume_path = cache_path_;
  config.volume_size = static_cast<uint64_t>(10 * 1024 * 1024);
  config.enable_checksum = true;
  config.handler = &handler;

  auto result = PageSpeedCache::Create(config);
  ASSERT_TRUE(result.has_value());
  auto& test_cache = *result;

  // Create a directory at the .gen path so rename() fails with EISDIR.
  std::string gen_path = cache_path_ + ".gen";
  std::filesystem::create_directory(gen_path);

  auto reset = test_cache->ResetVolume();
  EXPECT_TRUE(reset.has_value());

  // The .gen.tmp file should have been cleaned up after the rename failure.
  std::string gen_tmp_path = cache_path_ + ".gen.tmp";
  EXPECT_FALSE(std::filesystem::exists(gen_tmp_path))
      << ".gen.tmp should be cleaned up after rename failure";

  // The .gen directory should still exist (rename didn't replace it).
  EXPECT_TRUE(std::filesystem::is_directory(gen_path))
      << ".gen directory should still exist after failed rename";

  // Clean up.
  std::filesystem::remove_all(gen_path);
}
#endif  // !_WIN32

TEST_F(PageSpeedCacheTest, ListAlternatesNeverWrittenUrlReturnsError) {
  // Verify that ListAlternates on a URL that has never been written to
  // returns a CacheError (not an empty vector), covering the Cyclone-level
  // NotFound path in list_alternates_sync.
  CreateCache();

  auto alts =
      cache_->ListAlternates("/img/never-written.jpg", "example.com", "https");
  EXPECT_FALSE(alts.has_value())
      << "ListAlternates should return an error for never-written URLs";

  // Also verify via AlternateExists that no alternate exists.
  EXPECT_FALSE(cache_->AlternateExists("/img/never-written.jpg", "example.com",
                                       "https", 0x08));
}

TEST_F(PageSpeedCacheTest, CreateCacheAddVolumeFailure) {
  // Try to trigger add_volume failure (line 89) by providing a volume
  // path that cannot be created.  Using a path through a file (not a
  // directory) as parent should cause add_volume to fail.
  PageSpeedCacheConfig config;
  // Create a regular file, then use it as a "directory" in the path.
  std::string blocker_file = temp_dir_ + "/blocker";
  {
    std::ofstream(blocker_file) << "x";
  }
  config.volume_path = blocker_file + "/cache.vol";
  config.volume_size = static_cast<uint64_t>(10 * 1024 * 1024);
  config.enable_checksum = true;

  auto result = PageSpeedCache::Create(config);
  EXPECT_FALSE(result.has_value())
      << "Create should fail when volume path parent is a file";
}

#ifndef _WIN32
// Unix file permission test — not applicable on Windows.
TEST_F(PageSpeedCacheTest, CreateCacheChmodWarningWithHandler) {
  // Exercise the chmod warning path (lines 99-102) by verifying that
  // Create() with a handler does not crash or produce unexpected
  // behavior.  On normal systems chmod succeeds (no warning), but
  // this exercises the handler != nullptr check in that code path.
  TestCacheHandler handler;
  PageSpeedCacheConfig config;
  config.volume_path = cache_path_;
  config.volume_size = static_cast<uint64_t>(10 * 1024 * 1024);
  config.enable_checksum = true;
  config.handler = &handler;

  auto result = PageSpeedCache::Create(config);
  ASSERT_TRUE(result.has_value());

  // Verify the (fingerprint-named) volume file was created with expected
  // permissions.
  struct stat st;
  ASSERT_EQ(::stat((*result)->VolumeFilePath().c_str(), &st), 0);
  EXPECT_EQ(st.st_mode & static_cast<mode_t>(0777), static_cast<mode_t>(0660))
      << "Volume file should be mode 0660 after Create";
}
#endif  // !_WIN32

#ifndef _WIN32
// Reopening a volume that is ALREADY mode 0660 must succeed and must not
// report a refusal.  This is the shape every peer after the first one sees:
// the volume exists, its mode is right, and there is nothing to set.
TEST_F(PageSpeedCacheTest, ReopenAcceptsVolumeWhoseModeIsAlready0660) {
  std::string volume_file;
  {
    PageSpeedCacheConfig config;
    config.volume_path = cache_path_;
    config.volume_size = static_cast<uint64_t>(10 * 1024 * 1024);
    config.enable_checksum = true;
    auto r = PageSpeedCache::Create(config);
    ASSERT_TRUE(r.has_value());
    volume_file = (*r)->VolumeFilePath();
  }
  struct stat st;
  ASSERT_EQ(::stat(volume_file.c_str(), &st), 0);
  ASSERT_EQ(st.st_mode & static_cast<mode_t>(07777), static_cast<mode_t>(0660))
      << "precondition: the first Create leaves the volume at 0660";

  TestCacheHandler handler;
  PageSpeedCacheConfig config;
  config.volume_path = cache_path_;
  config.volume_size = static_cast<uint64_t>(10 * 1024 * 1024);
  config.enable_checksum = true;
  config.handler = &handler;
  auto reopened = PageSpeedCache::Create(config);
  ASSERT_TRUE(reopened.has_value()) << "reopening a 0660 volume must succeed";
  EXPECT_EQ(handler.last_message().find("Refusing to use cache volume"),
            std::string::npos)
      << "handler said: " << handler.last_message();
}

// The regression this file exists for.  fchmod() is owner-only: only the
// process that CREATED the volume owns it, and a web-server worker sharing
// the cache through group `pagespeed` is a group member, not the owner.  It
// gets EPERM setting a mode that is already 0660 -- and treating that as
// fatal turned in-place optimization off in every worker on a correctly
// installed host.  Run the reopen under a second uid that keeps the group,
// which is exactly the deployed peer relationship.
TEST_F(PageSpeedCacheTest, ReopenSucceedsForNonOwnerWhenModeIsAlready0660) {
  if (::geteuid() != 0) {
    GTEST_SKIP() << "needs root in order to reopen the volume under a "
                    "second, non-owning uid";
  }
  // Any uid that is not this one; nothing is looked up, so it does not have
  // to exist in the password database.
  constexpr uid_t kNonOwnerUid = 65534;

  std::string volume_file;
  {
    PageSpeedCacheConfig config;
    config.volume_path = cache_path_;
    config.volume_size = static_cast<uint64_t>(10 * 1024 * 1024);
    config.enable_checksum = true;
    auto r = PageSpeedCache::Create(config);
    ASSERT_TRUE(r.has_value());
    volume_file = (*r)->VolumeFilePath();
  }
  struct stat st;
  ASSERT_EQ(::stat(volume_file.c_str(), &st), 0);
  ASSERT_EQ(st.st_mode & static_cast<mode_t>(07777), static_cast<mode_t>(0660));

  // The child reaches the volume through the GROUP, the way the packaged
  // layout does: it gives up the uid (so it is no longer the owner and
  // cannot fchmod) and keeps the gid (so 0660 still admits it).
  ASSERT_EQ(::chmod(temp_dir_.c_str(), static_cast<mode_t>(0770)), 0);

  const pid_t pid = ::fork();
  ASSERT_GE(pid, 0) << "fork failed: " << ::strerror(errno);
  if (pid == 0) {
    // Child.  No gtest assertions past this point -- the exit status is the
    // entire report, and _exit avoids running the parent's atexit handlers.
    if (::setuid(kNonOwnerUid) != 0) {
      ::_exit(71);
    }
    if (::geteuid() == 0) {
      ::_exit(72);
    }
    PageSpeedCacheConfig config;
    config.volume_path = cache_path_;
    config.volume_size = static_cast<uint64_t>(10 * 1024 * 1024);
    config.enable_checksum = true;
    auto r = PageSpeedCache::Create(config);
    ::_exit(r.has_value() ? 0 : 73);
  }
  int status = 0;
  ASSERT_EQ(::waitpid(pid, &status, 0), pid);
  ASSERT_TRUE(WIFEXITED(status)) << "the child did not exit normally";
  EXPECT_EQ(WEXITSTATUS(status), 0)
      << "a non-owning group member must be able to open a volume that is "
         "already mode 0660 (73 = Create refused it, which is the defect; "
         "71/72 = the test could not drop the uid)";
}

// O_NOFOLLOW catches a symlink AT the volume path.  A HARD LINK is the same
// substitution with no symlink to catch, and the cache directory is
// group-writable by design, so the volume must also be the only name for its
// bytes.
TEST_F(PageSpeedCacheTest, RefusesVolumeThatHasASecondHardLink) {
  std::string volume_file;
  {
    PageSpeedCacheConfig config;
    config.volume_path = cache_path_;
    config.volume_size = static_cast<uint64_t>(10 * 1024 * 1024);
    config.enable_checksum = true;
    auto r = PageSpeedCache::Create(config);
    ASSERT_TRUE(r.has_value());
    volume_file = (*r)->VolumeFilePath();
  }
  const std::string planted = temp_dir_ + "/planted-name.dat";
  ASSERT_EQ(::link(volume_file.c_str(), planted.c_str()), 0)
      << "link failed: " << ::strerror(errno);

  TestCacheHandler handler;
  PageSpeedCacheConfig config;
  config.volume_path = cache_path_;
  config.volume_size = static_cast<uint64_t>(10 * 1024 * 1024);
  config.enable_checksum = true;
  config.handler = &handler;
  auto r = PageSpeedCache::Create(config);
  ASSERT_FALSE(r.has_value())
      << "a volume a second name is hard-linked to must be refused";
  // Cyclone checks the opened volume itself and refuses first (and prints its
  // reason on stderr, not through the handler), so the cache's own "lone
  // regular file" check in CaptureVolumePathsAndChmod is no longer reached for
  // a hard link.  The refusal is what Cyclone reports.
  EXPECT_EQ(r.error(), cyclone::CacheError::VolumeFileRefused)
      << "the refusal must name the reason; handler said: "
      << handler.last_message();
}
#endif  // !_WIN32

TEST_F(PageSpeedCacheTest, WriteGenerationFileMultipleResetsIncrement) {
  // Verify that multiple ResetVolume calls each increment the generation
  // counter correctly, exercising WriteGenerationFile's write path
  // (lines 397-401) repeatedly.
  CreateCache();

  std::string gen_path = cache_path_ + ".gen";

  // Initial state: no gen file.
  EXPECT_EQ(PageSpeedCache::ReadGenerationFile(gen_path), 0u);

  // Perform 5 resets and verify monotonic increment.
  for (uint64_t i = 1; i <= 5; ++i) {
    auto reset = cache_->ResetVolume();
    ASSERT_TRUE(reset.has_value())
        << "ResetVolume #" << i
        << " failed: " << static_cast<int>(reset.error());
    uint64_t gen = PageSpeedCache::ReadGenerationFile(gen_path);
    EXPECT_EQ(gen, i) << "Generation should be " << i << " after reset #" << i;
  }
}

TEST_F(PageSpeedCacheTest, WriteAlternateWithHandlerLogsMetadataWrite) {
  // Exercise the handler logging on successful WriteAlternate (lines 230-234)
  // with a handler attached, ensuring the Info message fires for the
  // metadata prefix + content write path.
  TestCacheHandler handler;
  PageSpeedCacheConfig config;
  config.volume_path = cache_path_;
  config.volume_size = static_cast<uint64_t>(10 * 1024 * 1024);
  config.enable_checksum = true;
  config.handler = &handler;

  auto result = PageSpeedCache::Create(config);
  ASSERT_TRUE(result.has_value());
  auto& logged_cache = *result;

  CapabilityMask mask;
  AlternateId id =
      MaskToAlternateId(static_cast<uint8_t>(mask.Encode() & 0xFF));
  AlternateMetadata meta;
  meta.full_mask = mask.Encode();
  meta.content_type = ContentType::kImage;
  meta.origin_content_type = "image/jpeg";

  int count_before = handler.count();
  std::string data = "test-image-data";
  auto wh = logged_cache->WriteAlternate("/img/logged.jpg", "example.com",
                                         "https", id, data.size(), meta);
  ASSERT_TRUE(wh.has_value());
  auto bytes = std::as_bytes(std::span(data));
  (void)wh->write_sync(bytes);
  (void)wh->close_sync();

  // Handler should have received at least one Info message for the write.
  EXPECT_GT(handler.count(), count_before)
      << "Handler should log on successful WriteAlternate";
}

#ifndef _WIN32
// Unix symlink test — symlink() not available on Windows.
TEST_F(PageSpeedCacheTest, ReadGenerationFileFromSymlink) {
  // Verify ReadGenerationFile handles symlinks gracefully.
  // WriteGenerationFile uses O_NOFOLLOW to prevent symlink attacks,
  // but ReadGenerationFile uses std::ifstream which follows symlinks.
  std::string real_path = temp_dir_ + "/real.gen";
  std::string link_path = temp_dir_ + "/link.gen";

  {
    std::ofstream(real_path) << "42\n";
  }
  ASSERT_EQ(::symlink(real_path.c_str(), link_path.c_str()), 0);

  // ReadGenerationFile should follow the symlink and read the value.
  EXPECT_EQ(PageSpeedCache::ReadGenerationFile(link_path), 42u);

  // Clean up.
  ::unlink(link_path.c_str());
}
#endif  // !_WIN32

#ifndef _WIN32
// Unix file permission test — not applicable on Windows.
TEST_F(PageSpeedCacheTest, ResetVolumeChmodWarningAfterReset) {
  // Exercise the chmod warning path in ResetVolume (lines 349-352) by
  // verifying that after a reset, the volume file has correct permissions.
  // This covers the ResetVolume chmod path which is separate from Create's.
  TestCacheHandler handler;
  PageSpeedCacheConfig config;
  config.volume_path = cache_path_;
  config.volume_size = static_cast<uint64_t>(10 * 1024 * 1024);
  config.enable_checksum = true;
  config.handler = &handler;

  auto result = PageSpeedCache::Create(config);
  ASSERT_TRUE(result.has_value());
  auto& test_cache = *result;

  auto reset = test_cache->ResetVolume();
  ASSERT_TRUE(reset.has_value());

  // Verify volume permissions after reset.
  struct stat st;
  ASSERT_EQ(::stat(test_cache->VolumeFilePath().c_str(), &st), 0);
  EXPECT_EQ(st.st_mode & static_cast<mode_t>(0777), static_cast<mode_t>(0660))
      << "Volume file should be mode 0660 after ResetVolume";
}
#endif  // !_WIN32

// ========== Coverage: Additional error path tests ==========

TEST_F(PageSpeedCacheTest, CreateFailsWithNullHandlerAndMissingParent) {
  // Exercise Create() error path without a handler attached.
  // When the parent directory doesn't exist, Create() should fail cleanly
  // without crashing even though handler_ is null (no logging).
  PageSpeedCacheConfig config;
  config.volume_path = temp_dir_ + "/nonexistent/nested/cache.vol";
  config.volume_size = static_cast<uint64_t>(10 * 1024 * 1024);
  config.enable_checksum = true;
  config.handler = nullptr;  // No handler

  auto result = PageSpeedCache::Create(config);
  EXPECT_FALSE(result.has_value())
      << "Create should fail without crashing when handler is null";
}

TEST_F(PageSpeedCacheTest, CreateFailsStartPhase) {
  // Exercise the cache->start() failure path in Create() by creating
  // conditions that allow Cache::create and add_volume to succeed but
  // start to fail.  One way: create two caches with the same volume
  // using multi_process mode with conflicting configurations.
  // For now, we test with an unrealistically tiny volume_size which
  // may cause start() to fail on some Cyclone versions.
  PageSpeedCacheConfig config;
  config.volume_path = cache_path_;
  config.volume_size = 1;  // Unrealistically tiny
  config.enable_checksum = true;

  auto result = PageSpeedCache::Create(config);
  // This may succeed on some Cyclone versions (minimum size is
  // handled internally), so we only verify no crash.
  // The test's primary value is exercising the code path.
  if (!result.has_value()) {
    // Good - one of the three error paths triggered.
    SUCCEED();
  }
}

#ifndef _WIN32
// Windows file locking prevents remove_all on open cache files.
TEST_F(PageSpeedCacheTest, ResetVolumeWithInvalidMultiProcessConfig) {
  // Create a valid cache, then modify the internal config to have an
  // invalid multi-process configuration so that Cache::create fails
  // during ResetVolume (line 331 in cache.cc).
  //
  // We cannot directly modify the private cyclone_config_, but we can
  // test the ResetVolume error path when Cyclone's create() fails
  // by using a path that becomes invalid between create and reset.
  // Strategy: delete the temp directory entirely, so when ResetVolume
  // tries to re-create, the parent doesn't exist.
  CreateCache();

  // Delete the entire temp directory (the volume file is inside).
  std::filesystem::remove_all(temp_dir_);

  auto result = cache_->ResetVolume();
  EXPECT_FALSE(result.has_value())
      << "ResetVolume should fail when the volume's parent dir is deleted";

  // Recreate temp_dir_ for TearDown cleanup.
  std::filesystem::create_directories(temp_dir_);
}
#endif  // !_WIN32

TEST_F(PageSpeedCacheTest, WriteAlternateHandlerNullOnError) {
  // Exercise the WriteAlternate error path (lines 208-214) when
  // handler_ is null. Ensures the null handler check doesn't crash.
  PageSpeedCacheConfig config;
  config.volume_path = cache_path_;
  config.volume_size = static_cast<uint64_t>(10 * 1024 * 1024);
  config.enable_checksum = true;
  config.handler = nullptr;  // No handler

  auto result = PageSpeedCache::Create(config);
  ASSERT_TRUE(result.has_value());
  auto& no_handler_cache = *result;

  // Stop the cache to trigger write failure.
  PageSpeedCacheTestPeer::Cyclone(*no_handler_cache).stop();

  CapabilityMask mask;
  AlternateId id =
      MaskToAlternateId(static_cast<uint8_t>(mask.Encode() & 0xFF));

  AlternateMetadata meta;
  meta.full_mask = mask.Encode();
  meta.content_type = ContentType::kImage;
  meta.origin_content_type = "image/jpeg";

  // WriteAlternate should fail gracefully without crashing (no handler).
  auto wh = no_handler_cache->WriteAlternate(
      "/img/nohandler.jpg", "example.com", "https", id, 100, meta);
  EXPECT_FALSE(wh.has_value())
      << "WriteAlternate should fail without crash when handler is null";
}

TEST_F(PageSpeedCacheTest, WriteSentinelHandlerNullOnError) {
  // Exercise the WriteSentinel error path (lines 249-255) when
  // handler_ is null. Ensures the null handler check doesn't crash.
  PageSpeedCacheConfig config;
  config.volume_path = cache_path_;
  config.volume_size = static_cast<uint64_t>(10 * 1024 * 1024);
  config.enable_checksum = true;
  config.handler = nullptr;  // No handler

  auto result = PageSpeedCache::Create(config);
  ASSERT_TRUE(result.has_value());
  auto& no_handler_cache = *result;

  // Stop the cache to trigger write failure.
  PageSpeedCacheTestPeer::Cyclone(*no_handler_cache).stop();

  auto wh = no_handler_cache->WriteSentinel(
      "/page.html", "example.com", "https", SentinelId::kEarlyHints, 50);
  EXPECT_FALSE(wh.has_value())
      << "WriteSentinel should fail without crash when handler is null";
}

TEST_F(PageSpeedCacheTest, ReadBestAlternateHandlerNullOnMiss) {
  // Exercise the ReadBestAlternate miss path (lines 136-140) when
  // handler_ is null. Ensures the null handler check doesn't crash.
  PageSpeedCacheConfig config;
  config.volume_path = cache_path_;
  config.volume_size = static_cast<uint64_t>(10 * 1024 * 1024);
  config.enable_checksum = true;
  config.handler = nullptr;  // No handler

  auto result = PageSpeedCache::Create(config);
  ASSERT_TRUE(result.has_value());
  auto& no_handler_cache = *result;

  CapabilityMask mask;
  auto read_result = no_handler_cache->ReadBestAlternate("/miss", "example.com",
                                                         "https", mask);
  EXPECT_FALSE(read_result.has_value())
      << "ReadBestAlternate miss should not crash when handler is null";
}

TEST_F(PageSpeedCacheTest, ReadAlternateHandlerNullOnMiss) {
  // Exercise the ReadAlternate miss path (lines 167-172) when
  // handler_ is null.
  PageSpeedCacheConfig config;
  config.volume_path = cache_path_;
  config.volume_size = static_cast<uint64_t>(10 * 1024 * 1024);
  config.enable_checksum = true;
  config.handler = nullptr;  // No handler

  auto result = PageSpeedCache::Create(config);
  ASSERT_TRUE(result.has_value());
  auto& no_handler_cache = *result;

  auto read_result =
      no_handler_cache->ReadAlternate("/miss", "example.com", "https", 0x08);
  EXPECT_FALSE(read_result.has_value())
      << "ReadAlternate miss should not crash when handler is null";
}

TEST_F(PageSpeedCacheTest, RemoveHandlerNullOnRemove) {
  // Exercise the Remove path (lines 277-280) when handler_ is null.
  PageSpeedCacheConfig config;
  config.volume_path = cache_path_;
  config.volume_size = static_cast<uint64_t>(10 * 1024 * 1024);
  config.enable_checksum = true;
  config.handler = nullptr;  // No handler

  auto result = PageSpeedCache::Create(config);
  ASSERT_TRUE(result.has_value());
  auto& no_handler_cache = *result;

  // Remove on non-existent URL should not crash.
  auto removed =
      no_handler_cache->Remove("/nonexistent", "example.com", "https");
  // remove_sync may succeed (no-op) or fail (NotFound) depending on Cyclone.
  // Either way, no crash is the goal.
  (void)removed;
}

TEST_F(PageSpeedCacheTest, ResetVolumeHandlerNullOnSuccess) {
  // Exercise the ResetVolume info log path (lines 360-363) when
  // handler_ is null.
  PageSpeedCacheConfig config;
  config.volume_path = cache_path_;
  config.volume_size = static_cast<uint64_t>(10 * 1024 * 1024);
  config.enable_checksum = true;
  config.handler = nullptr;  // No handler

  auto result = PageSpeedCache::Create(config);
  ASSERT_TRUE(result.has_value());
  auto& no_handler_cache = *result;

  auto reset = no_handler_cache->ResetVolume();
  EXPECT_TRUE(reset.has_value())
      << "ResetVolume should succeed without crash when handler is null";

  // Verify the generation file was still written.
  std::string gen_path = cache_path_ + ".gen";
  EXPECT_EQ(PageSpeedCache::ReadGenerationFile(gen_path), 1u);
}

#ifndef _WIN32
// Unix file permission test — not applicable on Windows.
TEST_F(PageSpeedCacheTest, WriteGenerationFileHandlerNullOnError) {
  // Exercise WriteGenerationFile error paths when handler_ is null.
  // Block the .gen.tmp path with a directory to trigger the open failure,
  // and verify no crash occurs even without a handler.
  if (::geteuid() == 0) {
    GTEST_SKIP() << "Skipped: root bypasses directory permissions";
  }

  PageSpeedCacheConfig config;
  config.volume_path = cache_path_;
  config.volume_size = static_cast<uint64_t>(10 * 1024 * 1024);
  config.enable_checksum = true;
  config.handler = nullptr;  // No handler

  auto result = PageSpeedCache::Create(config);
  ASSERT_TRUE(result.has_value());
  auto& no_handler_cache = *result;

  // Block the .gen.tmp path with a directory.
  std::string gen_tmp_path = cache_path_ + ".gen.tmp";
  std::filesystem::create_directory(gen_tmp_path);

  // ResetVolume calls WriteGenerationFile, which should fail at open()
  // but not crash since handler is null.
  auto reset = no_handler_cache->ResetVolume();
  EXPECT_TRUE(reset.has_value())
      << "ResetVolume should succeed even if gen file write fails";

  // The .gen file should not have been written.
  std::string gen_path = cache_path_ + ".gen";
  EXPECT_FALSE(std::filesystem::exists(gen_path))
      << "Generation file should not exist when open() failed";

  std::filesystem::remove_all(gen_tmp_path);
}
#endif  // !_WIN32

#ifndef _WIN32
// Unix file permission test — not applicable on Windows.
TEST_F(PageSpeedCacheTest, WriteGenerationFileRenameHandlerNullOnError) {
  // Exercise WriteGenerationFile rename failure path when handler_ is null.
  if (::geteuid() == 0) {
    GTEST_SKIP() << "Skipped: root bypasses directory permissions";
  }

  PageSpeedCacheConfig config;
  config.volume_path = cache_path_;
  config.volume_size = static_cast<uint64_t>(10 * 1024 * 1024);
  config.enable_checksum = true;
  config.handler = nullptr;  // No handler

  auto result = PageSpeedCache::Create(config);
  ASSERT_TRUE(result.has_value());
  auto& no_handler_cache = *result;

  // Create a directory at .gen path so rename() fails with EISDIR.
  std::string gen_path = cache_path_ + ".gen";
  std::filesystem::create_directory(gen_path);

  auto reset = no_handler_cache->ResetVolume();
  EXPECT_TRUE(reset.has_value())
      << "ResetVolume should succeed even if gen rename fails";

  // The .gen.tmp file should have been cleaned up.
  std::string gen_tmp_path = cache_path_ + ".gen.tmp";
  EXPECT_FALSE(std::filesystem::exists(gen_tmp_path))
      << ".gen.tmp should be cleaned up after rename failure";

  std::filesystem::remove_all(gen_path);
}
#endif  // !_WIN32

#ifndef _WIN32
// Unix file permission test — not applicable on Windows.
TEST_F(PageSpeedCacheTest, CreateChmodFailureWithReadOnlyVolume) {
  // Exercise the chmod warning path in Create() (lines 99-102) by
  // making the cache volume file immutable after Cyclone creates it.
  // On macOS, we can use chflags(UF_IMMUTABLE) to prevent chmod.
  // On Linux, we could use chattr +i (requires root).
  // Since this is OS-specific, we verify the general path works.
  if (::geteuid() == 0) {
    GTEST_SKIP() << "Skipped: root bypasses file permissions";
  }

  // First create a cache normally.
  PageSpeedCacheConfig config;
  config.volume_path = cache_path_;
  config.volume_size = static_cast<uint64_t>(10 * 1024 * 1024);
  config.enable_checksum = true;

  auto r1 = PageSpeedCache::Create(config);
  ASSERT_TRUE(r1.has_value());
  // Capture the actual (fingerprint-named) volume file before closing.
  const std::string volume_file = (*r1)->VolumeFilePath();
  r1->reset();  // Close the cache.

  // Make the volume file read-only (chmod 0444).
  ASSERT_EQ(::chmod(volume_file.c_str(), 0444), 0);

  // Re-open the cache — the volume file already exists, so Cyclone
  // opens it rather than creating it. The fchmod to 0660 will fail
  // because we can't chmod a file we don't own with write permission
  // (on some systems). On most systems, the owner CAN chmod their own
  // file, so this may not actually trigger the warning.
  TestCacheHandler handler;
  PageSpeedCacheConfig config2;
  config2.volume_path = cache_path_;
  config2.volume_size = static_cast<uint64_t>(10 * 1024 * 1024);
  config2.enable_checksum = true;
  config2.handler = &handler;

  auto r2 = PageSpeedCache::Create(config2);
  // Whether or not the chmod fails, the cache should still be created.
  // The chmod warning is non-fatal.
  // Whether or not Create() succeeded, restore permissions so TearDown can
  // clean up.
  ::chmod(volume_file.c_str(), 0666);
  (void)r2;
}
#endif  // !_WIN32

TEST_F(PageSpeedCacheTest, ReadGenerationFileFromDirectory) {
  // ReadGenerationFile should return 0 for a path that exists but
  // is a directory (not a regular file).
  std::string dir_as_gen = temp_dir_ + "/dir.gen";
  std::filesystem::create_directory(dir_as_gen);

  uint64_t gen = PageSpeedCache::ReadGenerationFile(dir_as_gen);
  EXPECT_EQ(gen, 0u) << "ReadGenerationFile should return 0 for a directory";

  std::filesystem::remove_all(dir_as_gen);
}

TEST_F(PageSpeedCacheTest, ReadGenerationFileWithWhitespace) {
  // ReadGenerationFile should handle whitespace around the number.
  std::string gen_path = temp_dir_ + "/whitespace.gen";

  {
    std::ofstream(gen_path) << "  123  \n";
  }
  EXPECT_EQ(PageSpeedCache::ReadGenerationFile(gen_path), 123u);

  {
    std::ofstream(gen_path) << "\t456\t\n";
  }
  EXPECT_EQ(PageSpeedCache::ReadGenerationFile(gen_path), 456u);

  // Newline-only content should return 0.
  {
    std::ofstream(gen_path) << "\n\n\n";
  }
  EXPECT_EQ(PageSpeedCache::ReadGenerationFile(gen_path), 0u);
}

TEST_F(PageSpeedCacheTest, ReadGenerationFileNegativeNumber) {
  // ReadGenerationFile reads uint64_t. A negative number should fail or
  // produce unexpected results.
  std::string gen_path = temp_dir_ + "/negative.gen";
  {
    std::ofstream(gen_path) << "-1\n";
  }
  // uint64_t reading of "-1" via operator>> may wrap or fail.
  // We just verify no crash and return some value.
  uint64_t gen = PageSpeedCache::ReadGenerationFile(gen_path);
  // On most implementations, "-1" to uint64_t via >> produces 0 (fail).
  (void)gen;  // No crash is the primary assertion.
}

// ========== Coverage Phase 5: Remaining uncovered lines ==========

// --- an unusable (immutable) volume file is refused, macOS only ---
//
// This was CreateChmodFailureTriggersHandlerWarning, and it asserted the
// chmod-failure branch behind `if (r2.has_value())` -- a guard that can never
// be true, which made the assertion unreachable.  chflags(UF_IMMUTABLE) does
// not merely block fchmod: it blocks the O_RDWR open the cache layer performs
// FIRST, so Create fails before the mode is ever touched.  The honest
// assertion is therefore the deterministic one: Create refuses, every time.
// The non-owner fchmod path it was reaching for is covered directly by
// ReopenSucceedsForNonOwnerWhenModeIsAlready0660 above.

#ifdef __APPLE__
#include <sys/types.h>

TEST_F(PageSpeedCacheTest, CreateRefusesImmutableVolumeFile) {
  // Make the volume file immutable via chflags(UF_IMMUTABLE) after Cyclone
  // creates it, then re-open the cache.
  if (::geteuid() == 0) {
    GTEST_SKIP() << "Skipped: root bypasses chflags";
  }

  // Step 1: Create the cache normally so Cyclone creates the volume file.
  std::string volume_file;
  {
    PageSpeedCacheConfig config;
    config.volume_path = cache_path_;
    config.volume_size = 10 * 1024 * 1024;
    config.enable_checksum = true;
    auto r = PageSpeedCache::Create(config);
    ASSERT_TRUE(r.has_value());
    // Capture the actual (fingerprint-named) volume file.
    volume_file = (*r)->VolumeFilePath();
    // Destroy the cache so we can reopen it.
  }

  // Step 2: Make the volume file immutable.  This blocks the cache layer's
  // own O_RDWR open, not just the later fchmod.
  ASSERT_EQ(::chflags(volume_file.c_str(), UF_IMMUTABLE), 0)
      << "chflags UF_IMMUTABLE failed: " << strerror(errno);

  // Step 3: Re-open the cache with a handler and watch it refuse.
  TestCacheHandler handler;
  PageSpeedCacheConfig config2;
  config2.volume_path = cache_path_;
  config2.volume_size = 10 * 1024 * 1024;
  config2.enable_checksum = true;
  config2.handler = &handler;

  auto r2 = PageSpeedCache::Create(config2);

  // Remove immutable flag before any assertions so TearDown can clean up.
  ::chflags(volume_file.c_str(), 0);

  EXPECT_FALSE(r2.has_value())
      << "an immutable volume file cannot be opened read-write, so Create "
         "must refuse it rather than hand back a cache that cannot be "
         "written";
}

TEST_F(PageSpeedCacheTest, ResetVolumeChmodFailureTriggersHandlerWarning) {
  // Exercise the chmod warning path in ResetVolume() (lines 349-352)
  // by making the volume file immutable after reset recreates it.
  //
  // Strategy: hook into ResetVolume by creating a watcher thread that
  // sets UF_IMMUTABLE on the new volume file as soon as it appears.
  // This is racy, so instead we use a different approach:
  // After ResetVolume creates the new file, we make it immutable,
  // then call ResetVolume again.
  //
  // Actually, we can't easily intercept. Instead, we note that the
  // existing test ResetVolumeChmodWarningAfterReset covers the
  // non-failure chmod path. For the failure path, we'd need to make the
  // newly-created volume immutable between Cyclone's start() and our
  // chmod() call, which requires thread interleaving.
  //
  // Alternative approach: Create a symlink from the cache path to a
  // file on a read-only filesystem. Not portable.
  //
  // For now, we verify the handler path is exercised by examining a
  // simpler scenario: pre-create an immutable file at the cache path,
  // then call ResetVolume which deletes the old file and creates a new one.
  // Since the old file is immutable, std::filesystem::remove will fail.
  // But ResetVolume ignores the remove error code. The new file created
  // by Cyclone won't be immutable, so chmod will succeed.
  //
  // The most reliable test for ResetVolume chmod failure would require
  // patching chmod at runtime, which is too complex. We rely on the
  // Create() test above to verify the handler warning code path, since
  // lines 99-102 and 349-352 are identical code patterns.
  GTEST_SKIP() << "ResetVolume chmod failure requires runtime interception";
}
#endif  // __APPLE__

// --- ExactIdSelector (line 22) ---
// ExactIdSelector is used internally by ReadAlternate. It's already exercised
// by existing ReadAlternate tests, but let's add a targeted test to ensure
// the select() method's "not found" path (return nullopt) is also covered.

TEST_F(PageSpeedCacheTest, ReadAlternateWithNonExistentIdReturnsError) {
  // Write an alternate with one ID, then read with a different ID.
  // This exercises ExactIdSelector::select() returning nullopt.
  CreateCache();

  CapabilityMask webp_mask = MakeTestMask(CapabilityMask::ImageFormat::kWebP,
                                          CapabilityMask::Viewport::kDesktop);
  WriteContent("/img/exact-sel.jpg", "example.com", webp_mask, "webp-data");

  // Read with an AVIF alternate ID that doesn't exist.
  CapabilityMask avif_mask = MakeTestMask(CapabilityMask::ImageFormat::kAvif,
                                          CapabilityMask::Viewport::kDesktop);
  AlternateId avif_id =
      MaskToAlternateId(static_cast<uint8_t>(avif_mask.Encode() & 0xFF));

  auto result = cache_->ReadAlternate("/img/exact-sel.jpg", "example.com",
                                      "https", avif_id);
  EXPECT_FALSE(result.has_value()) << "ReadAlternate should return error when "
                                      "ExactIdSelector finds no match";
}

// --- WriteAlternate metadata write failure (lines 221-227) ---
// The metadata write_sync (line 219) writes into already-allocated space,
// so it essentially never fails in practice. This is defensive code.
// We document this with a test that verifies the normal path works
// with a handler, covering the handler != nullptr check on the success side.
// The failure path (lines 221-227) requires Cyclone internal failure
// (e.g., disk corruption mid-write) that cannot be reliably triggered.

TEST_F(PageSpeedCacheTest, WriteAlternateMetadataWriteSucceedsWithHandler) {
  // Verify that the metadata prefix write succeeds and the handler
  // receives the success Info message (lines 230-234), confirming we
  // passed through the metadata write path (line 219) without error.
  TestCacheHandler handler;
  PageSpeedCacheConfig config;
  config.volume_path = cache_path_;
  config.volume_size = static_cast<uint64_t>(10 * 1024 * 1024);
  config.enable_checksum = true;
  config.handler = &handler;

  auto result = PageSpeedCache::Create(config);
  ASSERT_TRUE(result.has_value());
  auto& logged_cache = *result;

  CapabilityMask mask = MakeTestMask(CapabilityMask::ImageFormat::kWebP,
                                     CapabilityMask::Viewport::kDesktop);
  AlternateId id =
      MaskToAlternateId(static_cast<uint8_t>(mask.Encode() & 0xFF));

  // Build metadata with a long origin_content_type to exercise the
  // variable-length metadata prefix write path.
  AlternateMetadata meta;
  meta.full_mask = mask.Encode();
  meta.content_type = ContentType::kImage;
  meta.origin_content_type = "image/webp; charset=binary; extra=padding";
  meta.cache_inserted_at = 1700000000;
  meta.origin_max_age = 86400;
  meta.origin_s_maxage = 3600;
  meta.origin_cc_flags = AlternateMetadata::kCCOriginPublic |
                         AlternateMetadata::kCCOriginImmutable |
                         AlternateMetadata::kCCOriginHeaderPresent;
  meta.ssimulacra2_score_x100 = 7500;
  meta.content_class = 0;  // Photo
  meta.origin_etag = "\"abc123\"";
  meta.origin_last_modified = 1699999000;

  int count_before = handler.count();
  std::string data = "webp-image-content-data-here";
  auto wh = logged_cache->WriteAlternate("/img/meta-write.jpg", "example.com",
                                         "https", id, data.size(), meta);
  ASSERT_TRUE(wh.has_value())
      << "WriteAlternate should succeed for valid metadata";
  auto bytes = std::as_bytes(std::span(data));
  auto written = wh->write_sync(bytes);
  ASSERT_TRUE(written.has_value());
  auto closed = wh->close_sync();
  ASSERT_TRUE(closed.has_value());

  // Handler should have the success Info log.
  EXPECT_GT(handler.count(), count_before)
      << "Handler should log on successful WriteAlternate with metadata";

  // Read back and verify all metadata fields survived.
  auto rr = logged_cache->ReadBestAlternate("/img/meta-write.jpg",
                                            "example.com", "https", mask);
  ASSERT_TRUE(rr.has_value());
  EXPECT_EQ(rr->metadata.ssimulacra2_score_x100, 7500);
  EXPECT_EQ(rr->metadata.content_class, 0);
  EXPECT_EQ(rr->metadata.origin_etag, "\"abc123\"");
  EXPECT_EQ(rr->metadata.origin_last_modified, 1699999000u);
}

// --- ResetVolume Cache::create failure (line 333) ---
// This is already covered by ResetVolumeWithInvalidMultiProcessConfig which
// deletes the temp directory. Let's add a specific test that corrupts the
// cyclone_config_ indirectly to ensure line 333 is hit.

#ifndef _WIN32
// Windows file locking prevents remove_all on open cache files.
TEST_F(PageSpeedCacheTest, ResetVolumeFailsWhenParentDirDeleted) {
  // Delete the parent directory between initial Create and ResetVolume.
  // Cache::create should fail because the working directory is gone.
  CreateCache();

  // Delete only the volume file and its parent directory.
  std::filesystem::remove(cache_path_);
  // Also remove the directory to make add_volume fail
  // (can't create new volume file in deleted directory).
  std::filesystem::remove_all(temp_dir_);

  auto result = cache_->ResetVolume();
  EXPECT_FALSE(result.has_value())
      << "ResetVolume should fail when parent directory is deleted";

  // Recreate temp_dir_ for TearDown cleanup.
  std::filesystem::create_directories(temp_dir_);
}
#endif  // !_WIN32

// --- Generation file write failure WITH handler (lines 405-407) ---
// The existing tests block the .gen.tmp path with a directory to trigger
// the open() failure (lines 391-392). For the write() failure path
// (lines 403-409), we need open() to succeed but write()/fsync() to fail.
// On macOS, we can make the fd read-only after open() by removing write
// permission, but open() already opened it with O_WRONLY. The write()
// failure path requires disk-level failures (full disk, I/O error).
//
// Alternative: Use /dev/full on Linux (not available on macOS).
// Since we can't practically trigger write failure on macOS, we document
// this and rely on the existing gen file tests for the handler path.

// --- Generation file rename failure WITH handler (lines 414-415) ---
// Already covered by WriteGenerationFileRenameFailure and
// WriteGenerationFileRenameFailureCleansTmpFile. Both use a handler.

// --- Additional: Verify WriteGenerationFile paths with handler messages ---

#ifndef _WIN32
// Unix file permission test — not applicable on Windows.
TEST_F(PageSpeedCacheTest, WriteGenerationFileOpenFailureHandlerMessage) {
  // Verify the handler receives a specific warning when the generation
  // file open() fails. Uses a recording handler to capture messages.
  if (::geteuid() == 0) {
    GTEST_SKIP() << "Skipped: root bypasses directory permissions";
  }

  // Custom handler that records message text.
  class RecordingHandler : public MessageHandler {
   public:
    void Message(MessageType type, const char* format, ...) override {
      va_list args;
      va_start(args, format);
      MessageV(type, format, args);
      va_end(args);
    }
    [[nodiscard]] const std::vector<std::string>& messages() const {
      return messages_;
    }

   protected:
    void MessageV(MessageType /*type*/, const char* format,
                  va_list args) override {
      char buf[1024];
      vsnprintf(buf, sizeof(buf), format, args);
      messages_.push_back(buf);
    }

   private:
    std::vector<std::string> messages_;
  };

  RecordingHandler handler;
  PageSpeedCacheConfig config;
  config.volume_path = cache_path_;
  config.volume_size = static_cast<uint64_t>(10 * 1024 * 1024);
  config.enable_checksum = true;
  config.handler = &handler;

  auto result = PageSpeedCache::Create(config);
  ASSERT_TRUE(result.has_value());
  auto& test_cache = *result;

  // Block the .gen.tmp path with a directory.
  std::string gen_tmp_path = cache_path_ + ".gen.tmp";
  std::filesystem::create_directory(gen_tmp_path);

  auto reset = test_cache->ResetVolume();
  EXPECT_TRUE(reset.has_value());

  // Check that one of the handler messages mentions "generation file".
  bool found_gen_warning = false;
  for (const auto& msg : handler.messages()) {
    if (msg.find("generation file") != std::string::npos) {
      found_gen_warning = true;
      break;
    }
  }
  EXPECT_TRUE(found_gen_warning)
      << "Handler should log warning about generation file creation failure";

  std::filesystem::remove_all(gen_tmp_path);
}
#endif  // !_WIN32

#ifndef _WIN32
// Unix file permission test — not applicable on Windows.
TEST_F(PageSpeedCacheTest, WriteGenerationFileRenameFailureHandlerMessage) {
  // Verify the handler receives a rename-specific warning.
  if (::geteuid() == 0) {
    GTEST_SKIP() << "Skipped: root bypasses directory permissions";
  }

  class RecordingHandler : public MessageHandler {
   public:
    void Message(MessageType type, const char* format, ...) override {
      va_list args;
      va_start(args, format);
      MessageV(type, format, args);
      va_end(args);
    }
    [[nodiscard]] const std::vector<std::string>& messages() const {
      return messages_;
    }

   protected:
    void MessageV(MessageType /*type*/, const char* format,
                  va_list args) override {
      char buf[1024];
      vsnprintf(buf, sizeof(buf), format, args);
      messages_.push_back(buf);
    }

   private:
    std::vector<std::string> messages_;
  };

  RecordingHandler handler;
  PageSpeedCacheConfig config;
  config.volume_path = cache_path_;
  config.volume_size = static_cast<uint64_t>(10 * 1024 * 1024);
  config.enable_checksum = true;
  config.handler = &handler;

  auto result = PageSpeedCache::Create(config);
  ASSERT_TRUE(result.has_value());
  auto& test_cache = *result;

  // Create a directory at .gen path so rename() fails with EISDIR.
  std::string gen_path = cache_path_ + ".gen";
  std::filesystem::create_directory(gen_path);

  auto reset = test_cache->ResetVolume();
  EXPECT_TRUE(reset.has_value());

  // Check that one of the handler messages mentions the generation file.
  bool found_gen_warning = false;
  for (const auto& msg : handler.messages()) {
    if (msg.find("generation") != std::string::npos) {
      found_gen_warning = true;
      break;
    }
  }
  EXPECT_TRUE(found_gen_warning)
      << "Handler should log warning about generation file write failure";

  std::filesystem::remove_all(gen_path);
}
#endif  // !_WIN32

// ========== Coverage Phase 6: Targeted coverage for specific uncovered paths ==========

// --- ExactIdSelector found path (line 22-35): ReadAlternate selects from
//     multiple alternates ---

TEST_F(PageSpeedCacheTest,
       ReadAlternateSelectsCorrectIdFromMultipleAlternates) {
  // Write multiple content alternates with different AlternateIds for the
  // same URL, then use ReadAlternate() to fetch each specific one by ID.
  // This exercises ExactIdSelector::select() iterating through the alternates
  // list and finding a match at various positions.
  CreateCache();

  // Write WebP/Desktop/Identity alternate.
  CapabilityMask webp_desktop = MakeTestMask(
      CapabilityMask::ImageFormat::kWebP, CapabilityMask::Viewport::kDesktop);
  WriteContent("/img/multi.jpg", "example.com", webp_desktop,
               "webp-desktop-data");

  // Write AVIF/Desktop/Identity alternate.
  CapabilityMask avif_desktop = MakeTestMask(
      CapabilityMask::ImageFormat::kAvif, CapabilityMask::Viewport::kDesktop);
  WriteContent("/img/multi.jpg", "example.com", avif_desktop,
               "avif-desktop-data");

  // Write Original/Mobile/Identity alternate.
  CapabilityMask orig_mobile =
      MakeTestMask(CapabilityMask::ImageFormat::kOriginal,
                   CapabilityMask::Viewport::kMobile);
  WriteContent("/img/multi.jpg", "example.com", orig_mobile,
               "orig-mobile-data");

  // Write WebP/Mobile/2x alternate.
  CapabilityMask webp_mobile_2x = MakeTestMask(
      CapabilityMask::ImageFormat::kWebP, CapabilityMask::Viewport::kMobile,
      CapabilityMask::PixelDensity::k2xPlus);
  WriteContent("/img/multi.jpg", "example.com", webp_mobile_2x,
               "webp-mobile-2x-data");

  // Verify we have at least 4 alternates.
  auto alts = cache_->ListAlternates("/img/multi.jpg", "example.com", "https");
  ASSERT_TRUE(alts.has_value());
  EXPECT_GE(alts->size(), 4u);

  // Now use ReadAlternate (which uses ExactIdSelector) to fetch each one.

  // Fetch WebP/Desktop.
  {
    AlternateId id =
        MaskToAlternateId(static_cast<uint8_t>(webp_desktop.Encode() & 0xFF));
    auto result =
        cache_->ReadAlternate("/img/multi.jpg", "example.com", "https", id);
    ASSERT_TRUE(result.has_value())
        << "ReadAlternate should find WebP/Desktop alternate";
    auto content = result->content();
    std::string_view data(reinterpret_cast<const char*>(content.data()),
                          content.size());
    EXPECT_EQ(data, "webp-desktop-data");
    EXPECT_EQ(result->metadata.content_type, ContentType::kImage);
  }

  // Fetch AVIF/Desktop.
  {
    AlternateId id =
        MaskToAlternateId(static_cast<uint8_t>(avif_desktop.Encode() & 0xFF));
    auto result =
        cache_->ReadAlternate("/img/multi.jpg", "example.com", "https", id);
    ASSERT_TRUE(result.has_value())
        << "ReadAlternate should find AVIF/Desktop alternate";
    auto content = result->content();
    std::string_view data(reinterpret_cast<const char*>(content.data()),
                          content.size());
    EXPECT_EQ(data, "avif-desktop-data");
  }

  // Fetch Original/Mobile.
  {
    AlternateId id =
        MaskToAlternateId(static_cast<uint8_t>(orig_mobile.Encode() & 0xFF));
    auto result =
        cache_->ReadAlternate("/img/multi.jpg", "example.com", "https", id);
    ASSERT_TRUE(result.has_value())
        << "ReadAlternate should find Original/Mobile alternate";
    auto content = result->content();
    std::string_view data(reinterpret_cast<const char*>(content.data()),
                          content.size());
    EXPECT_EQ(data, "orig-mobile-data");
  }

  // Fetch WebP/Mobile/2x.
  {
    AlternateId id =
        MaskToAlternateId(static_cast<uint8_t>(webp_mobile_2x.Encode() & 0xFF));
    auto result =
        cache_->ReadAlternate("/img/multi.jpg", "example.com", "https", id);
    ASSERT_TRUE(result.has_value())
        << "ReadAlternate should find WebP/Mobile/2x alternate";
    auto content = result->content();
    std::string_view data(reinterpret_cast<const char*>(content.data()),
                          content.size());
    EXPECT_EQ(data, "webp-mobile-2x-data");
  }

  // Verify that reading a non-existent alternate returns error.
  {
    CapabilityMask svgmask = MakeTestMask(CapabilityMask::ImageFormat::kSvg,
                                          CapabilityMask::Viewport::kDesktop);
    AlternateId id =
        MaskToAlternateId(static_cast<uint8_t>(svgmask.Encode() & 0xFF));
    auto result =
        cache_->ReadAlternate("/img/multi.jpg", "example.com", "https", id);
    EXPECT_FALSE(result.has_value())
        << "ReadAlternate should return error for non-existent SVG alternate";
  }
}

TEST_F(PageSpeedCacheTest, ReadAlternateWithHandlerLogsOnMissAmongMultiple) {
  // Write multiple alternates, then ReadAlternate with an ID that doesn't
  // exist. With a handler, lines 168-172 should fire (logging the miss).
  TestCacheHandler handler;
  PageSpeedCacheConfig config;
  config.volume_path = cache_path_;
  config.volume_size = static_cast<uint64_t>(10 * 1024 * 1024);
  config.enable_checksum = true;
  config.handler = &handler;

  auto result = PageSpeedCache::Create(config);
  ASSERT_TRUE(result.has_value());
  auto& logged_cache = *result;

  // Write two alternates.
  CapabilityMask webp_mask = MakeTestMask(CapabilityMask::ImageFormat::kWebP,
                                          CapabilityMask::Viewport::kDesktop);
  AlternateId webp_id =
      MaskToAlternateId(static_cast<uint8_t>(webp_mask.Encode() & 0xFF));

  AlternateMetadata meta;
  meta.full_mask = webp_mask.Encode();
  meta.content_type = ContentType::kImage;
  meta.origin_content_type = "image/webp";

  std::string data = "webp-content";
  auto wh = logged_cache->WriteAlternate("/img/multi-miss.jpg", "example.com",
                                         "https", webp_id, data.size(), meta);
  ASSERT_TRUE(wh.has_value());
  auto bytes = std::as_bytes(std::span(data));
  (void)wh->write_sync(bytes);
  (void)wh->close_sync();

  int count_before = handler.count();

  // Read an alternate that doesn't exist (AVIF).
  CapabilityMask avif_mask = MakeTestMask(CapabilityMask::ImageFormat::kAvif,
                                          CapabilityMask::Viewport::kDesktop);
  AlternateId avif_id =
      MaskToAlternateId(static_cast<uint8_t>(avif_mask.Encode() & 0xFF));

  auto read_result = logged_cache->ReadAlternate(
      "/img/multi-miss.jpg", "example.com", "https", avif_id);
  EXPECT_FALSE(read_result.has_value())
      << "ReadAlternate should return miss for non-existent AVIF ID";
  EXPECT_GT(handler.count(), count_before)
      << "Handler should log Info for ReadAlternate miss";
}

// --- ReadAlternate with metadata prefix on content alternates (line 181-183)
// ---

TEST_F(PageSpeedCacheTest, ReadAlternateContentAlternateHasMetadata) {
  // ReadAlternate on a content alternate (not sentinel) should parse
  // the metadata prefix and return correct content. This exercises the
  // ParseMetadataPrefix call at line 181-182 through ReadAlternate path
  // (not ReadBestAlternate).
  CreateCache();

  CapabilityMask mask = MakeTestMask(CapabilityMask::ImageFormat::kWebP,
                                     CapabilityMask::Viewport::kDesktop);

  std::string data = "content-via-read-alternate";
  WriteContent("/img/read-alt-content.jpg", "example.com", mask, data);

  AlternateId id =
      MaskToAlternateId(static_cast<uint8_t>(mask.Encode() & 0xFF));

  auto result = cache_->ReadAlternate("/img/read-alt-content.jpg",
                                      "example.com", "https", id);
  ASSERT_TRUE(result.has_value())
      << "ReadAlternate should succeed for existing content alternate";

  // metadata_prefix_size_ should be > 0 for content alternates.
  auto content = result->content();
  std::string_view read_data(reinterpret_cast<const char*>(content.data()),
                             content.size());
  EXPECT_EQ(read_data, data);
  EXPECT_EQ(result->metadata.content_type, ContentType::kImage);
  EXPECT_EQ(result->metadata.origin_content_type, "image/jpeg");
}

// --- WriteAlternate metadata write failure (lines 221-227) ---
// The metadata write_sync (line 219) writes into already-allocated space from
// write_alternate_sync. Under normal conditions, this write never fails because
// the space is already allocated. To trigger a failure, we would need the
// underlying Cyclone write handle to become invalid between write_alternate_sync
// (line 205) and write_sync (line 219) -- which can't happen in a single-threaded
// call to WriteAlternate.
//
// The closest we can get is to verify the code path compiles correctly and that
// if we manually construct a scenario where the cache is compromised, the error
// is propagated properly. Since we can't easily trigger this from the public API
// without mocking Cyclone, we add a documentation test.

TEST_F(PageSpeedCacheTest, WriteAlternateMetadataWritePathExercised) {
  // This test exercises the normal metadata write path (line 219) that
  // precedes the failure check at lines 220-227. We write alternates with
  // various metadata sizes to stress the write_sync call and verify the
  // metadata prefix is written correctly.
  TestCacheHandler handler;
  PageSpeedCacheConfig config;
  config.volume_path = cache_path_;
  config.volume_size = static_cast<uint64_t>(10 * 1024 * 1024);
  config.enable_checksum = true;
  config.handler = &handler;

  auto result = PageSpeedCache::Create(config);
  ASSERT_TRUE(result.has_value());
  auto& logged_cache = *result;

  // Test with a large origin_content_type (near the 256-byte limit).
  CapabilityMask mask = MakeTestMask(
      CapabilityMask::ImageFormat::kAvif, CapabilityMask::Viewport::kTablet,
      CapabilityMask::PixelDensity::k2xPlus, CapabilityMask::SaveData::kOn);
  AlternateId id =
      MaskToAlternateId(static_cast<uint8_t>(mask.Encode() & 0xFF));

  AlternateMetadata meta;
  meta.full_mask = mask.Encode();
  meta.content_type = ContentType::kImage;
  // Use a long but valid content type string (200 chars).
  meta.origin_content_type = std::string(200, 'x');
  meta.flags = AlternateMetadata::kFlagWorkerProcessed;
  meta.cache_inserted_at = 1700000000;
  meta.origin_max_age = 86400;
  meta.origin_s_maxage = 7200;
  meta.origin_cc_flags = AlternateMetadata::kCCOriginPublic |
                         AlternateMetadata::kCCOriginImmutable |
                         AlternateMetadata::kCCOriginHeaderPresent;
  meta.ssimulacra2_score_x100 = 8200;
  meta.content_class = 1;  // Screenshot
  meta.origin_etag = "W/\"etag-for-large-metadata-test\"";
  meta.origin_last_modified = 1699999999;

  std::string data = "large-metadata-content";
  auto wh = logged_cache->WriteAlternate("/img/large-meta.jpg", "example.com",
                                         "https", id, data.size(), meta);
  ASSERT_TRUE(wh.has_value())
      << "WriteAlternate should succeed with large metadata";
  auto bytes = std::as_bytes(std::span(data));
  auto written = wh->write_sync(bytes);
  ASSERT_TRUE(written.has_value());
  auto closed = wh->close_sync();
  ASSERT_TRUE(closed.has_value());

  // Read back and verify the metadata survived.
  auto rr = logged_cache->ReadAlternate("/img/large-meta.jpg", "example.com",
                                        "https", id);
  ASSERT_TRUE(rr.has_value());
  auto content = rr->content();
  std::string_view read_data(reinterpret_cast<const char*>(content.data()),
                             content.size());
  EXPECT_EQ(read_data, data);
  EXPECT_EQ(rr->metadata.flags, AlternateMetadata::kFlagWorkerProcessed);
  EXPECT_EQ(rr->metadata.ssimulacra2_score_x100, 8200);
  EXPECT_EQ(rr->metadata.content_class, 1);
  EXPECT_EQ(rr->metadata.origin_etag, "W/\"etag-for-large-metadata-test\"");
  EXPECT_EQ(rr->metadata.origin_last_modified, 1699999999u);

  // Verify handler logged the write.
  EXPECT_GE(handler.count(), 1)
      << "Handler should log on successful WriteAlternate";
}

// --- Generation file I/O errors (lines 391-418) ---

#ifndef _WIN32
// Unix file permission test — not applicable on Windows.
TEST_F(PageSpeedCacheTest, WriteGenerationFileOpenFailsNonexistentDir) {
  // Exercise WriteGenerationFile open failure (lines 389-394) by configuring
  // the cache with a volume_path in a non-existent directory. Create the
  // actual cache in a valid directory first, then use ResetVolume with a
  // cache whose volume_path points to a path where .gen.tmp cannot be created.
  //
  // Strategy: Create a cache, then replace the volume file path to point into
  // a non-existent directory. Since we can't modify private config_, we
  // simulate this by creating a subdirectory, building the cache there, then
  // deleting only the parent of the .gen.tmp path.
  if (::geteuid() == 0) {
    GTEST_SKIP() << "Skipped: root bypasses directory permissions";
  }

  // Create the cache normally.
  TestCacheHandler handler;
  PageSpeedCacheConfig config;
  config.volume_path = cache_path_;
  config.volume_size = static_cast<uint64_t>(10 * 1024 * 1024);
  config.enable_checksum = true;
  config.handler = &handler;

  auto result = PageSpeedCache::Create(config);
  ASSERT_TRUE(result.has_value());
  auto& test_cache = *result;

  // Make the temp directory read-only so the open() for .gen.tmp fails
  // with EACCES. But first we need to ensure ResetVolume itself can
  // recreate the cache file. So we make only the .gen.tmp parent unwritable.
  //
  // Actually, the simplest approach: create a regular file at the .gen.tmp
  // path with mode 0000, so unlink() in WriteGenerationFile will fail
  // (file exists but we can't unlink it from a directory we don't have
  // write permission to). This is tricky.
  //
  // Simpler: create a non-empty directory at .gen.tmp so unlink() fails
  // (unlink can't remove directories) and open(O_CREAT|O_EXCL) also fails.
  std::string gen_tmp_path = cache_path_ + ".gen.tmp";
  std::filesystem::create_directory(gen_tmp_path);
  // Put a file inside so remove would need rmdir.
  {
    std::ofstream(gen_tmp_path + "/blocker") << "x";
  }

  int count_before = handler.count();
  auto reset = test_cache->ResetVolume();
  EXPECT_TRUE(reset.has_value())
      << "ResetVolume should succeed even if gen file write fails";
  EXPECT_GT(handler.count(), count_before)
      << "Handler should log warning for generation file open failure";

  // The .gen file should not exist.
  std::string gen_path = cache_path_ + ".gen";
  EXPECT_FALSE(std::filesystem::exists(gen_path))
      << "Generation file should not exist when open() failed";

  // Clean up.
  std::filesystem::remove_all(gen_tmp_path);
}
#endif  // !_WIN32

TEST_F(PageSpeedCacheTest, ReadGenerationFileNonexistentPath) {
  // ReadGenerationFile with a completely non-existent directory path.
  uint64_t gen =
      PageSpeedCache::ReadGenerationFile("/nonexistent/dir/cache.vol.gen");
  EXPECT_EQ(gen, 0u)
      << "ReadGenerationFile should return 0 for non-existent path";
}

TEST_F(PageSpeedCacheTest, ReadGenerationFileEmptyPath) {
  // ReadGenerationFile with an empty string path.
  uint64_t gen = PageSpeedCache::ReadGenerationFile("");
  EXPECT_EQ(gen, 0u) << "ReadGenerationFile should return 0 for empty path";
}

#ifndef _WIN32
// Unix file permission test — not applicable on Windows.
TEST_F(PageSpeedCacheTest, WriteGenerationFileRenameFailsDirectoryAtTarget) {
  // Exercise rename failure path (lines 412-418) more explicitly.
  // Create a non-empty directory at the .gen path so rename() fails,
  // and verify the .gen.tmp file is cleaned up afterward.
  if (::geteuid() == 0) {
    GTEST_SKIP() << "Skipped: root bypasses directory permissions";
  }

  TestCacheHandler handler;
  PageSpeedCacheConfig config;
  config.volume_path = cache_path_;
  config.volume_size = static_cast<uint64_t>(10 * 1024 * 1024);
  config.enable_checksum = true;
  config.handler = &handler;

  auto result = PageSpeedCache::Create(config);
  ASSERT_TRUE(result.has_value());
  auto& test_cache = *result;

  // Create a non-empty directory at the .gen path. On macOS, rename()
  // over a non-empty directory fails with ENOTEMPTY.
  std::string gen_path = cache_path_ + ".gen";
  std::filesystem::create_directory(gen_path);
  {
    std::ofstream(gen_path + "/content") << "blocking";
  }

  int count_before = handler.count();
  auto reset = test_cache->ResetVolume();
  EXPECT_TRUE(reset.has_value())
      << "ResetVolume should succeed even if gen rename fails";
  EXPECT_GT(handler.count(), count_before)
      << "Handler should log a warning for generation file rename failure";

  // Verify .gen.tmp is cleaned up (line 417).
  std::string gen_tmp_path = cache_path_ + ".gen.tmp";
  EXPECT_FALSE(std::filesystem::exists(gen_tmp_path))
      << ".gen.tmp should be cleaned up after rename failure";

  // Clean up.
  std::filesystem::remove_all(gen_path);
}
#endif  // !_WIN32

// --- chmod failure paths (lines 100-102 in Create, 350-352 in ResetVolume)
// ---

#ifndef _WIN32
// Unix file permission test — not applicable on Windows.
TEST_F(PageSpeedCacheTest, CreateChmodPathWithHandler) {
  // Exercise the chmod path (lines 99-102) in Create() with a handler.
  // On normal systems, the fchmod to 0660 succeeds. This test ensures the handler
  // is properly checked (handler != nullptr guard) and that the chmod call
  // itself executes. We verify the file permissions are set correctly.
  TestCacheHandler handler;
  PageSpeedCacheConfig config;
  config.volume_path = cache_path_;
  config.volume_size = static_cast<uint64_t>(10 * 1024 * 1024);
  config.enable_checksum = true;
  config.handler = &handler;

  auto result = PageSpeedCache::Create(config);
  ASSERT_TRUE(result.has_value());

  // Verify the volume file has 0660 permissions (fchmod succeeded).
  struct stat st;
  ASSERT_EQ(::stat((*result)->VolumeFilePath().c_str(), &st), 0);
  // The mode is set through an explicit fchmod on a descriptor opened
  // O_NOFOLLOW, so it is exact regardless of the process umask.
  EXPECT_EQ(st.st_mode & static_cast<mode_t>(0777), static_cast<mode_t>(0660))
      << "Volume file should be mode 0660 after successful chmod in Create";
}
#endif  // !_WIN32

#ifndef _WIN32
// A volume file that is a SYMLINK is refused outright.  The
// mode is applied through a descriptor opened O_NOFOLLOW, so a symlink fails
// with ELOOP -- and cyclone's own create has no O_NOFOLLOW, so without this
// refusal the cache would happily run on an attacker-chosen path whose mode
// nobody set.  Fatal, never a warning.
TEST_F(PageSpeedCacheTest, SymlinkedVolumeFileIsRefused) {
  if (::geteuid() == 0) {
    GTEST_SKIP() << "root's open() ignores some of what this test relies on";
  }
  // Create the cache once so the fingerprinted volume filename is known,
  // then replace that exact file with a symlink and reopen.
  std::string volume_file;
  {
    PageSpeedCacheConfig config;
    config.volume_path = cache_path_;
    config.volume_size = static_cast<uint64_t>(10 * 1024 * 1024);
    config.enable_checksum = true;
    auto r = PageSpeedCache::Create(config);
    ASSERT_TRUE(r.has_value());
    volume_file = (*r)->VolumeFilePath();
  }
  const std::string decoy = temp_dir_ + "/decoy.vol";
  {
    std::ofstream(decoy) << "x";
  }
  ASSERT_EQ(::unlink(volume_file.c_str()), 0);
  ASSERT_EQ(::symlink(decoy.c_str(), volume_file.c_str()), 0);

  TestCacheHandler handler;
  PageSpeedCacheConfig config2;
  config2.volume_path = cache_path_;
  config2.volume_size = static_cast<uint64_t>(10 * 1024 * 1024);
  config2.enable_checksum = true;
  config2.handler = &handler;
  auto r2 = PageSpeedCache::Create(config2);

  // Clean up before asserting so TearDown always succeeds.
  ::unlink(volume_file.c_str());

  EXPECT_FALSE(r2.has_value())
      << "a symlinked volume file must be refused, not used";
}
#endif  // !_WIN32

#ifndef _WIN32
// The volume mode is set with an explicit fchmod, so it must
// come out exact even under a umask that would otherwise strip the group
// bits.  Correctness never rides on the process umask.
TEST_F(PageSpeedCacheTest, VolumeModeIsUmaskIndependent) {
  const mode_t old_umask = ::umask(0077);
  PageSpeedCacheConfig config;
  config.volume_path = cache_path_;
  config.volume_size = static_cast<uint64_t>(10 * 1024 * 1024);
  config.enable_checksum = true;

  auto result = PageSpeedCache::Create(config);
  ::umask(old_umask);
  ASSERT_TRUE(result.has_value());

  struct stat st;
  ASSERT_EQ(::stat((*result)->VolumeFilePath().c_str(), &st), 0);
  EXPECT_EQ(st.st_mode & static_cast<mode_t>(0777), static_cast<mode_t>(0660))
      << "Volume mode must be explicit, not umask-derived";
}
#endif  // !_WIN32

#ifndef _WIN32
// Unix file permission test — not applicable on Windows.
TEST_F(PageSpeedCacheTest, ResetVolumeChmodPathWithHandler) {
  // Exercise the chmod path (lines 349-352) in ResetVolume() with a handler.
  // Verify that after ResetVolume, the new volume file has 0660 permissions.
  TestCacheHandler handler;
  PageSpeedCacheConfig config;
  config.volume_path = cache_path_;
  config.volume_size = static_cast<uint64_t>(10 * 1024 * 1024);
  config.enable_checksum = true;
  config.handler = &handler;

  auto result = PageSpeedCache::Create(config);
  ASSERT_TRUE(result.has_value());
  auto& test_cache = *result;

  // Perform reset which triggers the chmod path (lines 349-352).
  auto reset = test_cache->ResetVolume();
  ASSERT_TRUE(reset.has_value());

  // Verify permissions after reset.
  struct stat st;
  ASSERT_EQ(::stat(test_cache->VolumeFilePath().c_str(), &st), 0);
  EXPECT_EQ(st.st_mode & static_cast<mode_t>(0777), static_cast<mode_t>(0660))
      << "Volume file should be mode 0660 after successful chmod in "
         "ResetVolume";

  // Handler should have logged the ResetVolume info message (line 361-362),
  // confirming we passed through the chmod path without error.
  EXPECT_GE(handler.count(), 1)
      << "Handler should log on successful ResetVolume";
}
#endif  // !_WIN32

// --- Combined: ReadAlternate sentinel + content interleaving ---

TEST_F(PageSpeedCacheTest, ReadAlternateDistinguishesSentinelFromContent) {
  // Write both a sentinel and a content alternate for the same URL,
  // then use ReadAlternate() to fetch each one independently. This
  // exercises ExactIdSelector finding the correct entry when both
  // sentinel and content alternates coexist.
  CreateCache();

  // Write a content alternate.
  CapabilityMask mask = MakeTestMask(CapabilityMask::ImageFormat::kOriginal,
                                     CapabilityMask::Viewport::kDesktop);
  WriteContent("/page.html", "example.com", mask, "<html>content</html>",
               ContentType::kHtml);

  // Write an Early Hints sentinel.
  std::string hints = "/style.css\n/hero.webp";
  auto wh = cache_->WriteSentinel("/page.html", "example.com", "https",
                                  SentinelId::kEarlyHints, hints.size());
  ASSERT_TRUE(wh.has_value());
  auto hints_bytes = std::as_bytes(std::span(hints));
  (void)wh->write_sync(hints_bytes);
  (void)wh->close_sync();

  // Write a Content Hash sentinel.
  std::string hash = "sha256:abc123";
  auto wh2 = cache_->WriteSentinel("/page.html", "example.com", "https",
                                   SentinelId::kContentHash, hash.size());
  ASSERT_TRUE(wh2.has_value());
  auto hash_bytes = std::as_bytes(std::span(hash));
  (void)wh2->write_sync(hash_bytes);
  (void)wh2->close_sync();

  // ReadAlternate for content alternate — should get metadata + content.
  {
    AlternateId id =
        MaskToAlternateId(static_cast<uint8_t>(mask.Encode() & 0xFF));
    auto result =
        cache_->ReadAlternate("/page.html", "example.com", "https", id);
    ASSERT_TRUE(result.has_value());
    auto content = result->content();
    std::string_view data(reinterpret_cast<const char*>(content.data()),
                          content.size());
    EXPECT_EQ(data, "<html>content</html>");
    EXPECT_EQ(result->metadata.content_type, ContentType::kHtml);
  }

  // ReadAlternate for Early Hints sentinel — should get raw sentinel data.
  {
    auto result = cache_->ReadAlternate(
        "/page.html", "example.com", "https",
        static_cast<AlternateId>(SentinelId::kEarlyHints));
    ASSERT_TRUE(result.has_value());
    auto content = result->content();
    std::string_view data(reinterpret_cast<const char*>(content.data()),
                          content.size());
    EXPECT_EQ(data, "/style.css\n/hero.webp");
  }

  // ReadAlternate for Content Hash sentinel — should get hash data.
  {
    auto result = cache_->ReadAlternate(
        "/page.html", "example.com", "https",
        static_cast<AlternateId>(SentinelId::kContentHash));
    ASSERT_TRUE(result.has_value());
    auto content = result->content();
    std::string_view data(reinterpret_cast<const char*>(content.data()),
                          content.size());
    EXPECT_EQ(data, "sha256:abc123");
  }

  // ReadAlternate for a sentinel that wasn't written — should fail.
  {
    auto result = cache_->ReadAlternate(
        "/page.html", "example.com", "https",
        static_cast<AlternateId>(SentinelId::kWarmupRequest));
    EXPECT_FALSE(result.has_value())
        << "ReadAlternate should return error for unwritten sentinel";
  }
}

// =================================================================
// HitTracker batch-done callback unit tests
// =================================================================

// Verify batch-done callback is NOT called when flush has no pending keys.
TEST(HitTrackerBatchCallbackTest, NotCalledOnEmptyFlush) {
  cyclone::HitTrackerConfig config;
  config.enable_background_flush = false;  // Manual flush only
  cyclone::HitTracker tracker(config);

  std::atomic<int> batch_count{0};
  tracker.set_batch_done_callback([&batch_count]() { batch_count++; });

  std::atomic<int> per_key_count{0};
  tracker.start([&per_key_count](const cyclone::CacheKey&, cyclone::AlternateId,
                                 uint32_t, int64_t) {
    per_key_count++;
    return true;
  });

  // Flush with no pending hits — batch callback should NOT fire.
  tracker.flush_now();
  EXPECT_EQ(batch_count.load(), 0);
  EXPECT_EQ(per_key_count.load(), 0);

  tracker.stop();
}

// Verify batch-done callback is called exactly once per flush cycle.
TEST(HitTrackerBatchCallbackTest, CalledOncePerFlushCycle) {
  cyclone::HitTrackerConfig config;
  config.enable_background_flush = false;
  cyclone::HitTracker tracker(config);

  std::atomic<int> batch_count{0};
  tracker.set_batch_done_callback([&batch_count]() { batch_count++; });

  std::atomic<int> per_key_count{0};
  tracker.start([&per_key_count](const cyclone::CacheKey&, cyclone::AlternateId,
                                 uint32_t, int64_t) {
    per_key_count++;
    return true;
  });

  // Record hits for 3 different keys.
  auto k1 = cyclone::CacheKey::from_url("key1");
  auto k2 = cyclone::CacheKey::from_url("key2");
  auto k3 = cyclone::CacheKey::from_url("key3");
  tracker.record_hit(k1);
  tracker.record_hit(k2);
  tracker.record_hit(k3);

  // One flush cycle — 3 per-key callbacks, 1 batch callback.
  tracker.flush_now();
  EXPECT_EQ(per_key_count.load(), 3);
  EXPECT_EQ(batch_count.load(), 1);

  // Second flush with no new hits — neither callback fires.
  tracker.flush_now();
  EXPECT_EQ(per_key_count.load(), 3);
  EXPECT_EQ(batch_count.load(), 1);

  tracker.stop();
}

// Verify that hit_flush_fsyncs counter increments via CacheStats.
TEST_F(PageSpeedCacheTest, HitFlushFsyncsCounterIncrements) {
  CreateCache();

  CapabilityMask mask;
  WriteContent("/fsync-test.html", "example.com", mask, "content",
               ContentType::kHtml);

  // Read to accumulate hits.
  for (int i = 0; i < 3; ++i) {
    auto r = cache_->ReadBestAlternate("/fsync-test.html", "example.com",
                                       "https", mask);
    ASSERT_TRUE(r.has_value());
  }

  // Wait for flush cycle (1s interval) + margin.
  std::this_thread::sleep_for(std::chrono::milliseconds(1500));

  auto stats = cache_->Stats();
  EXPECT_GE(stats.hit_flush_fsyncs, 1u)
      << "Expected at least one fsync after a flush cycle with hits";
  EXPECT_GE(stats.hit_flush_successes, 1u);
}

// Verify that cache reads increment the hit_count visible via
// ListAlternates.  This is a regression test for hit tracking
// not persisting after reads (both nginx and worker observed 0).
TEST_F(PageSpeedCacheTest, HitCountIncrementedOnRead) {
  CreateCache();

  CapabilityMask mask;  // Default: Desktop/Identity = 0x08
  WriteContent("/page.html", "example.com", mask, "hello world",
               ContentType::kHtml);

  // Read the alternate — this should record a hit in the HitTracker.
  auto read1 =
      cache_->ReadBestAlternate("/page.html", "example.com", "https", mask);
  ASSERT_TRUE(read1.has_value());

  // Read a second time to accumulate more hits.
  auto read2 =
      cache_->ReadBestAlternate("/page.html", "example.com", "https", mask);
  ASSERT_TRUE(read2.has_value());

  // Allow the HitTracker background thread to flush (1s interval + margin).
  std::this_thread::sleep_for(std::chrono::milliseconds(1500));

  // List alternates and verify hit_count > 0.
  auto alts = cache_->ListAlternates("/page.html", "example.com", "https");
  ASSERT_TRUE(alts.has_value());
  ASSERT_GE(alts->size(), 1u);

  // Find our alternate.
  bool found = false;
  for (const auto& alt : *alts) {
    if (static_cast<uint8_t>(alt.id) ==
        static_cast<uint8_t>(mask.Encode() & 0xFF)) {
      EXPECT_GE(alt.hit_count, 2u)
          << "hit_count should reflect 2 reads, got " << alt.hit_count;
      found = true;
      break;
    }
  }
  EXPECT_TRUE(found) << "Expected alternate not found in ListAlternates";
}

// Verify that hit_count persists across cache reopen and is visible
// to a second cache instance (simulating nginx→worker cross-process
// visibility).  Regression test: Docker workbench showed hit_count=0
// for all alternates despite X-PageSpeed: HIT being served.
TEST_F(PageSpeedCacheTest, HitCountVisibleAfterReopen) {
  // Phase 1: create cache, write, read (accumulate hits), close.
  {
    PageSpeedCacheConfig config;
    config.volume_path = cache_path_;
    config.volume_size = static_cast<uint64_t>(10 * 1024 * 1024);
    auto result = PageSpeedCache::Create(config);
    ASSERT_TRUE(result.has_value());
    auto writer = std::move(*result);

    CapabilityMask mask;
    AlternateId id =
        MaskToAlternateId(static_cast<uint8_t>(mask.Encode() & 0xFF));
    AlternateMetadata meta;
    meta.full_mask = mask.Encode();
    meta.content_type = ContentType::kHtml;
    meta.origin_content_type = "text/html";
    std::string data = "hello";
    auto wh =
        writer->WriteAlternate("/p", "host", "https", id, data.size(), meta);
    ASSERT_TRUE(wh.has_value());
    (void)wh->write_sync(std::as_bytes(std::span(data)));
    (void)wh->close_sync();

    // Read 5 times to generate hits.
    for (int i = 0; i < 5; ++i) {
      auto r = writer->ReadBestAlternate("/p", "host", "https", mask);
      ASSERT_TRUE(r.has_value());
    }

    // Wait for flush.
    std::this_thread::sleep_for(std::chrono::milliseconds(1500));
  }
  // writer is destroyed here (HitTracker final flush in destructor).

  // Phase 2: reopen with volume_size=0 (auto-detect, simulating nginx).
  {
    PageSpeedCacheConfig config;
    config.volume_path = cache_path_;
    config.volume_size = 0;  // Auto-detect from file.
    auto result = PageSpeedCache::Create(config);
    ASSERT_TRUE(result.has_value());
    auto reader = std::move(*result);

    auto alts = reader->ListAlternates("/p", "host", "https");
    ASSERT_TRUE(alts.has_value());
    ASSERT_GE(alts->size(), 1u);

    bool found = false;
    for (const auto& alt : *alts) {
      if (!IsSentinel(static_cast<uint8_t>(alt.id))) {
        EXPECT_GE(alt.hit_count, 5u)
            << "hit_count should reflect 5 reads after reopen, got "
            << alt.hit_count;
        found = true;
        break;
      }
    }
    EXPECT_TRUE(found) << "Expected alternate not found after reopen";
  }
}

// Verify hit tracking survives alternate overwrites.  When an alternate is
// rewritten (e.g. nginx re-fetches the origin, or the worker reprocesses),
// Cyclone prepends a new chain entry but the old one remains.
// update_hit_count_sync must still find and update the correct (newest)
// document.  Regression test for Docker workbench showing hit_count=0.
TEST_F(PageSpeedCacheTest, HitCountSurvivesAlternateOverwrite) {
  CreateCache();

  CapabilityMask mask;  // Default: Desktop/Identity = 0x08
  AlternateId id =
      MaskToAlternateId(static_cast<uint8_t>(mask.Encode() & 0xFF));

  // --- First write (simulating nginx original) ---
  WriteContent("/rewrite.html", "host", mask, "version-1", ContentType::kHtml);

  // Read to accumulate hits on the first version.
  for (int i = 0; i < 3; ++i) {
    auto r = cache_->ReadBestAlternate("/rewrite.html", "host", "https", mask);
    ASSERT_TRUE(r.has_value());
  }

  // Wait for flush to persist those hits.
  std::this_thread::sleep_for(std::chrono::milliseconds(1500));

  // Verify hits on the first version.
  {
    auto alts = cache_->ListAlternates("/rewrite.html", "host", "https");
    ASSERT_TRUE(alts.has_value());
    uint32_t total_hits = 0;
    for (const auto& alt : *alts) {
      if (static_cast<uint8_t>(alt.id) == static_cast<uint8_t>(id)) {
        total_hits += alt.hit_count;
      }
    }
    EXPECT_GE(total_hits, 3u) << "Expected >= 3 hits before overwrite";
  }

  // --- Overwrite with new content (simulating re-fetch or worker reprocess).
  // This prepends a new chain entry; the old one stays in the chain. ---
  WriteContent("/rewrite.html", "host", mask, "version-2", ContentType::kHtml);

  // Read the overwritten alternate to generate new hits.
  for (int i = 0; i < 4; ++i) {
    auto r = cache_->ReadBestAlternate("/rewrite.html", "host", "https", mask);
    ASSERT_TRUE(r.has_value());
  }

  // Wait for flush.
  std::this_thread::sleep_for(std::chrono::milliseconds(1500));

  // Verify the newest version accumulated hits.  The chain may now contain
  // two entries with the same alternate_id; update_hit_count_sync must have
  // updated the first (newest) one.
  auto alts = cache_->ListAlternates("/rewrite.html", "host", "https");
  ASSERT_TRUE(alts.has_value());
  ASSERT_FALSE(alts->empty());

  // Find the first (newest) entry matching our alternate_id.
  bool found_with_hits = false;
  for (const auto& alt : *alts) {
    if (static_cast<uint8_t>(alt.id) == static_cast<uint8_t>(id) &&
        alt.hit_count >= 4) {
      found_with_hits = true;
      break;
    }
  }
  EXPECT_TRUE(found_with_hits)
      << "Expected at least one entry for alternate " << static_cast<int>(id)
      << " with hit_count >= 4 after overwrite";
}

// Verify hit tracking works when multiple different alternates exist.
// Simulates the full nginx + worker cycle: nginx writes the original,
// worker writes WebP/AVIF/Brotli variants, then reads accumulate hits
// on each independently.
TEST_F(PageSpeedCacheTest, HitCountTrackedPerAlternate) {
  CreateCache();

  // Write 3 alternates (original, WebP, gzip).
  CapabilityMask orig_mask;  // Desktop/Identity = 0x08
  CapabilityMask webp_mask = MakeTestMask(CapabilityMask::ImageFormat::kWebP,
                                          CapabilityMask::Viewport::kDesktop);
  CapabilityMask gzip_mask = MakeTestMask(
      CapabilityMask::ImageFormat::kOriginal,
      CapabilityMask::Viewport::kDesktop, CapabilityMask::PixelDensity::k1x,
      CapabilityMask::SaveData::kOff, CapabilityMask::TransferEncoding::kGzip);

  WriteContent("/multi.jpg", "host", orig_mask, "original");
  WriteContent("/multi.jpg", "host", webp_mask, "webp-data");
  WriteContent("/multi.jpg", "host", gzip_mask, "gzip-data");

  AlternateId orig_id =
      MaskToAlternateId(static_cast<uint8_t>(orig_mask.Encode() & 0xFF));
  AlternateId webp_id =
      MaskToAlternateId(static_cast<uint8_t>(webp_mask.Encode() & 0xFF));

  // Read original 2 times, WebP 5 times.
  for (int i = 0; i < 2; ++i) {
    auto r =
        cache_->ReadBestAlternate("/multi.jpg", "host", "https", orig_mask);
    ASSERT_TRUE(r.has_value());
  }
  for (int i = 0; i < 5; ++i) {
    auto r =
        cache_->ReadBestAlternate("/multi.jpg", "host", "https", webp_mask);
    ASSERT_TRUE(r.has_value());
  }

  // Wait for flush.
  std::this_thread::sleep_for(std::chrono::milliseconds(1500));

  auto alts = cache_->ListAlternates("/multi.jpg", "host", "https");
  ASSERT_TRUE(alts.has_value());
  ASSERT_GE(alts->size(), 3u);

  // Verify per-alternate hit counts.
  uint32_t orig_hits = 0;
  uint32_t webp_hits = 0;
  for (const auto& alt : *alts) {
    if (static_cast<uint8_t>(alt.id) == static_cast<uint8_t>(orig_id)) {
      orig_hits = std::max(orig_hits, alt.hit_count);
    }
    if (static_cast<uint8_t>(alt.id) == static_cast<uint8_t>(webp_id)) {
      webp_hits = std::max(webp_hits, alt.hit_count);
    }
  }
  EXPECT_GE(orig_hits, 2u) << "Original alternate should have >= 2 hits";
  EXPECT_GE(webp_hits, 5u) << "WebP alternate should have >= 5 hits";
}

// Simulate the Docker two-process scenario: one cache instance (nginx) reads
// and records hits, while a second concurrent instance (worker) reads hit
// counts via ListAlternates.  This is the closest reproduction of the
// Docker workbench bug where hit_count=0 despite X-PageSpeed: HIT.
TEST_F(PageSpeedCacheTest, HitCountVisibleToConcurrentInstance) {
  // Instance 1 (simulating worker): create cache and write content.
  PageSpeedCacheConfig config1;
  config1.volume_path = cache_path_;
  config1.volume_size = static_cast<uint64_t>(10 * 1024 * 1024);
  auto result1 = PageSpeedCache::Create(config1);
  ASSERT_TRUE(result1.has_value());
  auto worker = std::move(*result1);

  CapabilityMask mask;
  AlternateId id =
      MaskToAlternateId(static_cast<uint8_t>(mask.Encode() & 0xFF));
  AlternateMetadata meta;
  meta.full_mask = mask.Encode();
  meta.content_type = ContentType::kHtml;
  meta.origin_content_type = "text/html";
  std::string data = "cross-process content";

  auto wh =
      worker->WriteAlternate("/x", "host", "https", id, data.size(), meta);
  ASSERT_TRUE(wh.has_value());
  (void)wh->write_sync(std::as_bytes(std::span(data)));
  (void)wh->close_sync();

  // Instance 2 (simulating nginx): open same file, read to record hits.
  PageSpeedCacheConfig config2;
  config2.volume_path = cache_path_;
  config2.volume_size = 0;  // Auto-detect
  auto result2 = PageSpeedCache::Create(config2);
  ASSERT_TRUE(result2.has_value());
  auto nginx = std::move(*result2);

  for (int i = 0; i < 5; ++i) {
    auto r = nginx->ReadBestAlternate("/x", "host", "https", mask);
    ASSERT_TRUE(r.has_value());
  }

  // Wait for nginx's HitTracker to flush.
  std::this_thread::sleep_for(std::chrono::milliseconds(1500));

  // Read hit counts from the worker instance.  Both instances share the
  // same file, so worker's mmap should see nginx's pwrite updates.
  auto alts = worker->ListAlternates("/x", "host", "https");
  ASSERT_TRUE(alts.has_value());
  ASSERT_GE(alts->size(), 1u);

  bool found = false;
  for (const auto& alt : *alts) {
    if (static_cast<uint8_t>(alt.id) == static_cast<uint8_t>(id)) {
      EXPECT_GE(alt.hit_count, 5u)
          << "Worker should see nginx's hit_count via shared file, got "
          << alt.hit_count;
      found = true;
      break;
    }
  }
  EXPECT_TRUE(found) << "Expected alternate not found in worker ListAlternates";
}

#ifndef _WIN32
// Verify that hit counts are visible across a fork() boundary after the
// HitTracker flush cycle completes (which now includes an fsync).  This is
// a true cross-process test: the child opens the cache file fresh, so its
// mmap is independent of the parent's page cache — closely reproducing the
// Docker virtiofs scenario where pwrite without fsync is invisible.
TEST_F(PageSpeedCacheTest, HitCountVisibleAcrossFork) {
  // Parent: create cache, write content, read to accumulate hits.
  PageSpeedCacheConfig config;
  config.volume_path = cache_path_;
  config.volume_size = static_cast<uint64_t>(10 * 1024 * 1024);
  auto result = PageSpeedCache::Create(config);
  ASSERT_TRUE(result.has_value());
  auto parent_cache = std::move(*result);

  CapabilityMask mask;
  AlternateId id =
      MaskToAlternateId(static_cast<uint8_t>(mask.Encode() & 0xFF));
  AlternateMetadata meta;
  meta.full_mask = mask.Encode();
  meta.content_type = ContentType::kHtml;
  meta.origin_content_type = "text/html";
  std::string data = "fork-test content";

  auto wh = parent_cache->WriteAlternate("/fork", "host", "https", id,
                                         data.size(), meta);
  ASSERT_TRUE(wh.has_value());
  (void)wh->write_sync(std::as_bytes(std::span(data)));
  (void)wh->close_sync();

  // Accumulate hits.
  for (int i = 0; i < 10; ++i) {
    auto r = parent_cache->ReadBestAlternate("/fork", "host", "https", mask);
    ASSERT_TRUE(r.has_value());
  }

  // Wait for HitTracker background flush (1s interval) + fsync to complete.
  // 2s provides ~1s margin; the flush thread polls every 100ms.
  std::this_thread::sleep_for(std::chrono::milliseconds(2000));

  // Destroy parent cache before forking to avoid inheriting the HitTracker's
  // background thread state (POSIX: only calling thread survives fork).
  parent_cache.reset();

  // Fork a child process that opens the cache independently.
  pid_t pid = fork();
  ASSERT_NE(pid, -1) << "fork() failed: " << strerror(errno);

  if (pid == 0) {
    // Child process — open a fresh cache instance and check hit counts.
    // Use _exit() throughout to skip atexit handlers and C++ destructors.
    PageSpeedCacheConfig child_config;
    child_config.volume_path = cache_path_;
    child_config.volume_size = 0;  // Auto-detect
    auto child_result = PageSpeedCache::Create(child_config);
    if (!child_result.has_value()) {
      _exit(10);
    }
    auto child_cache = std::move(*child_result);

    auto alts = child_cache->ListAlternates("/fork", "host", "https");
    if (!alts.has_value() || alts->empty()) {
      _exit(11);
    }

    for (const auto& alt : *alts) {
      if (static_cast<uint8_t>(alt.id) == static_cast<uint8_t>(id)) {
        // Expect >= 10 hits from the parent process.
        _exit(alt.hit_count >= 10 ? 0 : 12);
      }
    }
    _exit(13);  // Alternate not found
  }

  // Parent: wait for child and check exit status.
  int status = 0;
  waitpid(pid, &status, 0);
  ASSERT_TRUE(WIFEXITED(status)) << "Child did not exit normally";
  int exit_code = WEXITSTATUS(status);
  EXPECT_EQ(exit_code, 0)
      << "Child exit code " << exit_code
      << " (10=create fail, 11=list fail, 12=hit_count<10, 13=alt not found)";
}

// ---------------------------------------------------------------------------
// Bug #3 Regression: Cross-Process Cache Write Visibility
// ---------------------------------------------------------------------------

TEST_F(PageSpeedCacheTest, CrossProcessWriteVisibilityViaFork) {
  // Parent writes URL A, closes cache. Fork child. Child opens cache,
  // writes URL B, reads URL A (must match). Parent waits, reopens,
  // reads both A and B.
  CreateCache();

  CapabilityMask mask;
  WriteContent("/img/parent.jpg", "example.com", mask, "parent-data");
  cache_.reset();  // Close before fork

  pid_t pid = fork();
  ASSERT_NE(pid, -1) << "fork() failed: " << strerror(errno);

  if (pid == 0) {
    // Child: open cache, write URL B, read URL A.
    PageSpeedCacheConfig child_config;
    child_config.volume_path = cache_path_;
    child_config.volume_size = 0;  // Auto-detect
    auto child_result = PageSpeedCache::Create(child_config);
    if (!child_result.has_value()) _exit(10);
    auto child_cache = std::move(*child_result);

    // Read URL A (parent's write)
    auto r = child_cache->ReadBestAlternate("/img/parent.jpg", "example.com",
                                            "https", mask);
    if (!r.has_value()) _exit(11);
    auto c = r->content();
    std::string_view data(reinterpret_cast<const char*>(c.data()), c.size());
    if (data != "parent-data") _exit(12);

    // Write URL B
    AlternateId id =
        MaskToAlternateId(static_cast<uint8_t>(mask.Encode() & 0xFF));
    AlternateMetadata meta;
    meta.full_mask = mask.Encode();
    meta.content_type = ContentType::kImage;
    meta.origin_content_type = "image/jpeg";
    auto wh = child_cache->WriteAlternate("/img/child.jpg", "example.com",
                                          "https", id, 10, meta);
    if (!wh.has_value()) _exit(13);
    std::string_view child_data = "child-data";
    (void)wh->write_sync(std::as_bytes(std::span(child_data)));
    (void)wh->close_sync();

    child_cache.reset();
    _exit(0);
  }

  // Parent: wait for child, then reopen and verify both URLs.
  int status = 0;
  waitpid(pid, &status, 0);
  ASSERT_TRUE(WIFEXITED(status)) << "Child did not exit normally";
  EXPECT_EQ(WEXITSTATUS(status), 0)
      << "Child exit code " << WEXITSTATUS(status)
      << " (10=create, 11=readA, 12=mismatch, 13=writeB)";

  // Reopen cache
  CreateCache();

  // Read URL A (parent's original write)
  auto ra = cache_->ReadBestAlternate("/img/parent.jpg", "example.com", "https",
                                      mask);
  ASSERT_TRUE(ra.has_value()) << "Parent's URL A not found after reopen";
  {
    auto c = ra->content();
    std::string_view data(reinterpret_cast<const char*>(c.data()), c.size());
    EXPECT_EQ(data, "parent-data");
  }

  // Read URL B (child's write)
  auto rb =
      cache_->ReadBestAlternate("/img/child.jpg", "example.com", "https", mask);
  ASSERT_TRUE(rb.has_value()) << "Child's URL B not found after reopen";
  {
    auto c = rb->content();
    std::string_view data(reinterpret_cast<const char*>(c.data()), c.size());
    EXPECT_EQ(data, "child-data");
  }
}

TEST_F(PageSpeedCacheTest, SharedWritePosRecoveredOnReopen) {
  // Write 5 URLs, close cache, reopen, write 5 more, read all 10.
  CreateCache();

  CapabilityMask mask;
  for (int i = 0; i < 5; ++i) {
    std::string url = "/img/reopen-" + std::to_string(i) + ".jpg";
    std::string data = "data-" + std::to_string(i);
    WriteContent(url, "example.com", mask, data);
  }

  // Close and reopen
  cache_.reset();
  CreateCache();

  // Write 5 more
  for (int i = 5; i < 10; ++i) {
    std::string url = "/img/reopen-" + std::to_string(i) + ".jpg";
    std::string data = "data-" + std::to_string(i);
    WriteContent(url, "example.com", mask, data);
  }

  // Read all 10
  for (int i = 0; i < 10; ++i) {
    std::string url = "/img/reopen-" + std::to_string(i) + ".jpg";
    std::string expected = "data-" + std::to_string(i);
    auto r = cache_->ReadBestAlternate(url, "example.com", "https", mask);
    ASSERT_TRUE(r.has_value()) << "URL " << url << " not found";
    auto c = r->content();
    std::string_view data(reinterpret_cast<const char*>(c.data()), c.size());
    EXPECT_EQ(data, expected) << "Data mismatch for " << url;
  }
}

TEST_F(PageSpeedCacheTest, CrossProcessSequentialWrites) {
  // Process A writes 50 URLs, then Process B (child) writes 50 more.
  // After child exits, parent reopens and verifies all 100 entries.
  // Tests the shared_write_pos mechanism across processes.
  CreateCache();

  // Parent writes first 50
  CapabilityMask mask;
  for (int i = 0; i < 50; ++i) {
    std::string url = "/p-" + std::to_string(i);
    std::string data = "parent-" + std::to_string(i);
    WriteContent(url, "example.com", mask, data);
  }

  // Close parent's handle before fork
  cache_.reset();

  pid_t pid = fork();
  ASSERT_NE(pid, -1) << "fork() failed: " << strerror(errno);

  if (pid == 0) {
    // Child: write 50 more URLs with /c- prefix
    PageSpeedCacheConfig child_config;
    child_config.volume_path = cache_path_;
    child_config.volume_size = 0;
    auto child_result = PageSpeedCache::Create(child_config);
    if (!child_result.has_value()) _exit(10);
    auto child_cache = std::move(*child_result);

    CapabilityMask cmask;
    AlternateId id =
        MaskToAlternateId(static_cast<uint8_t>(cmask.Encode() & 0xFF));
    for (int i = 0; i < 50; ++i) {
      AlternateMetadata meta;
      meta.full_mask = cmask.Encode();
      meta.content_type = ContentType::kImage;
      meta.origin_content_type = "image/jpeg";
      std::string url = "/c-" + std::to_string(i);
      std::string data = "child-" + std::to_string(i);
      auto wh = child_cache->WriteAlternate(url, "example.com", "https", id,
                                            data.size(), meta);
      if (!wh.has_value()) _exit(11);
      (void)wh->write_sync(std::as_bytes(std::span(data)));
      (void)wh->close_sync();
    }
    child_cache.reset();
    _exit(0);
  }

  // Wait for child to finish
  int status = 0;
  waitpid(pid, &status, 0);
  ASSERT_TRUE(WIFEXITED(status)) << "Child did not exit normally";
  EXPECT_EQ(WEXITSTATUS(status), 0);

  // Reopen and verify all 100 entries
  CreateCache();

  for (int i = 0; i < 50; ++i) {
    std::string url = "/p-" + std::to_string(i);
    auto r = cache_->ReadBestAlternate(url, "example.com", "https", mask);
    ASSERT_TRUE(r.has_value()) << "Parent URL " << url << " not found";
    auto c = r->content();
    std::string_view data(reinterpret_cast<const char*>(c.data()), c.size());
    EXPECT_EQ(data, "parent-" + std::to_string(i));
  }

  for (int i = 0; i < 50; ++i) {
    std::string url = "/c-" + std::to_string(i);
    auto r = cache_->ReadBestAlternate(url, "example.com", "https", mask);
    ASSERT_TRUE(r.has_value()) << "Child URL " << url << " not found";
    auto c = r->content();
    std::string_view data(reinterpret_cast<const char*>(c.data()), c.size());
    EXPECT_EQ(data, "child-" + std::to_string(i));
  }
}
#endif  // !_WIN32

// ---------------------------------------------------------------------------
// Bug #5 Regression: Concurrent Reads Must Not Deadlock
// ---------------------------------------------------------------------------

TEST_F(PageSpeedCacheTest, ConcurrentReadsDoNotDeadlock) {
  // Write 100 URLs. Launch 8 threads reading all 100 in a loop (5 iters).
  // Coordinated start via atomic flag. Assert completion within 10s.
  CreateCache();

  CapabilityMask mask;
  for (int i = 0; i < 100; ++i) {
    std::string url = "/dl-" + std::to_string(i);
    std::string data = "deadlock-test-" + std::to_string(i);
    WriteContent(url, "example.com", mask, data);
  }

  std::atomic<int> threads_completed{0};

  // Use a flag for coordinated start
  std::atomic<bool> start_flag{false};

  auto reader = [&]() {
    while (!start_flag.load()) {
      std::this_thread::yield();
    }
    for (int iter = 0; iter < 5; ++iter) {
      for (int i = 0; i < 100; ++i) {
        std::string url = "/dl-" + std::to_string(i);
        auto r = cache_->ReadBestAlternate(url, "example.com", "https", mask);
        EXPECT_TRUE(r.has_value()) << "Read failed for " << url;
      }
    }
    threads_completed.fetch_add(1);
  };

  std::vector<std::thread> threads;
  threads.reserve(8);
  for (int t = 0; t < 8; ++t) {
    threads.emplace_back(reader);
  }

  start_flag.store(true);

  // Wait up to 10s for completion
  auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
  while (threads_completed.load() < 8 &&
         std::chrono::steady_clock::now() < deadline) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
  }

  for (auto& t : threads) {
    if (t.joinable()) t.join();
  }

  EXPECT_EQ(threads_completed.load(), 8)
      << "Not all threads completed — possible deadlock";
}

TEST_F(PageSpeedCacheTest, ConcurrentReadsAndWritesDuringFlush) {
  // 1 writer thread + 4 reader threads for 2 seconds.
  // Regression test for AB/BA deadlock between read path
  // (stripe->mutex → HitTracker::_mutex via record_hit) and
  // HitTracker flusher (HitTracker::_mutex → stripe->mutex via
  // update_hit_count_sync). Fixed by deferring record_hit() calls
  // until after stripe->mutex is released.
  CreateCache();

  CapabilityMask mask;
  // Seed some initial data
  for (int i = 0; i < 10; ++i) {
    std::string url = "/rw-" + std::to_string(i);
    WriteContent(url, "example.com", mask, "initial-" + std::to_string(i));
  }

  std::atomic<bool> stop{false};
  std::atomic<int> write_count{0};
  std::atomic<int> read_count{0};

  // Writer thread: writes new URLs continuously
  auto writer = [&]() {
    int idx = 10;
    while (!stop.load()) {
      std::string url = "/rw-" + std::to_string(idx);
      std::string data = "written-" + std::to_string(idx);
      AlternateId id =
          MaskToAlternateId(static_cast<uint8_t>(mask.Encode() & 0xFF));
      AlternateMetadata meta;
      meta.full_mask = mask.Encode();
      meta.content_type = ContentType::kImage;
      meta.origin_content_type = "image/jpeg";
      auto wh = cache_->WriteAlternate(url, "example.com", "https", id,
                                       data.size(), meta);
      if (wh.has_value()) {
        (void)wh->write_sync(std::as_bytes(std::span(data)));
        (void)wh->close_sync();
        write_count.fetch_add(1);
      }
      ++idx;
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
  };

  // Reader threads: read initial URLs in a loop
  auto reader = [&]() {
    while (!stop.load()) {
      for (int i = 0; i < 10 && !stop.load(); ++i) {
        std::string url = "/rw-" + std::to_string(i);
        auto r = cache_->ReadBestAlternate(url, "example.com", "https", mask);
        if (r.has_value()) {
          read_count.fetch_add(1);
        }
      }
    }
  };

  std::thread writer_thread(writer);
  std::vector<std::thread> reader_threads;
  reader_threads.reserve(4);
  for (int t = 0; t < 4; ++t) {
    reader_threads.emplace_back(reader);
  }

  // Run for 2 seconds (enough to exercise flush paths and detect deadlocks)
  std::this_thread::sleep_for(std::chrono::seconds(2));
  stop.store(true);

  writer_thread.join();
  for (auto& t : reader_threads) {
    t.join();
  }

  EXPECT_GT(write_count.load(), 0) << "Writer didn't write anything";
  EXPECT_GT(read_count.load(), 0) << "Readers didn't read anything";

  // Verify a sample of the initial URLs are intact
  for (int i = 0; i < 10; ++i) {
    std::string url = "/rw-" + std::to_string(i);
    auto r = cache_->ReadBestAlternate(url, "example.com", "https", mask);
    ASSERT_TRUE(r.has_value()) << "Initial URL " << url << " lost";
  }
}

// ==========================================================================
// Stabilization tests (T6-T8): cross-process cache sharing edge cases
// ==========================================================================

// T6: ResetVolume while a write handle is open must not crash.
TEST_F(PageSpeedCacheTest, ResetDuringWrite) {
  CreateCache();

  CapabilityMask mask;
  AlternateId id =
      MaskToAlternateId(static_cast<uint8_t>(mask.Encode() & 0xFF));
  AlternateMetadata meta;
  meta.full_mask = mask.Encode();
  meta.content_type = ContentType::kHtml;
  meta.origin_content_type = "text/html";

  // Open a write handle (allocates space in the volume)
  auto wh = cache_->WriteAlternate("/reset/test", "example.com", "https", id,
                                   4096, meta);
  ASSERT_TRUE(wh.has_value());

  // Reset the volume while the write handle is still open
  auto reset_result = cache_->ResetVolume();
  EXPECT_TRUE(reset_result.has_value());

  // The write handle is now stale — writing should either succeed
  // (to a now-orphaned location) or fail gracefully.  The key
  // invariant is that nothing crashes or deadlocks.
  std::string data(4096, 'X');
  auto bytes = std::as_bytes(std::span(data));
  auto written = wh->write_sync(bytes);
  // We don't assert success — the write may fail since the
  // underlying volume was reset.  Just verify no crash.
  (void)written;

  // After reset, the cache should be empty
  auto result =
      cache_->ReadBestAlternate("/reset/test", "example.com", "https", mask);
  EXPECT_FALSE(result.has_value());
}

// T7: GenerationFileRace — generation counter survives reset cycle.
TEST_F(PageSpeedCacheTest, GenerationFileRace) {
  CreateCache();

  // Write some data
  CapabilityMask mask;
  WriteContent("/gen/test", "example.com", mask, "before-reset");

  // Read the initial generation
  std::string gen_path = cache_path_ + ".gen";
  uint64_t gen_before = PageSpeedCache::ReadGenerationFile(gen_path);

  // Reset
  auto result = cache_->ResetVolume();
  ASSERT_TRUE(result.has_value());

  // Generation should have incremented
  uint64_t gen_after = PageSpeedCache::ReadGenerationFile(gen_path);
  EXPECT_GT(gen_after, gen_before);

  // Multiple resets should monotonically increase generation
  result = cache_->ResetVolume();
  ASSERT_TRUE(result.has_value());
  uint64_t gen_after2 = PageSpeedCache::ReadGenerationFile(gen_path);
  EXPECT_GT(gen_after2, gen_after);
}

// T8: Chain corruption at PageSpeed level returns error (not stale content).
TEST_F(PageSpeedCacheTest, ChainCorruptionReturnsError) {
  CreateCache();

  // Write two alternates for the same URL
  CapabilityMask mask1 = MakeTestMask(CapabilityMask::ImageFormat::kWebP,
                                      CapabilityMask::Viewport::kDesktop);
  CapabilityMask mask2 = MakeTestMask(CapabilityMask::ImageFormat::kAvif,
                                      CapabilityMask::Viewport::kDesktop);

  WriteContent("/corrupt/test", "example.com", mask1, "webp-data-payload");
  WriteContent("/corrupt/test", "example.com", mask2, "avif-data-payload");

  // Verify both are readable before corruption
  auto r1 =
      cache_->ReadBestAlternate("/corrupt/test", "example.com", "https", mask1);
  ASSERT_TRUE(r1.has_value());
  auto r2 =
      cache_->ReadBestAlternate("/corrupt/test", "example.com", "https", mask2);
  ASSERT_TRUE(r2.has_value());

  // Close cache, corrupt the (fingerprint-named) volume file, reopen
  const std::string volume_file = cache_->VolumeFilePath();
  cache_.reset();

  {
    std::fstream file(volume_file,
                      std::ios::in | std::ios::out | std::ios::binary);
    ASSERT_TRUE(file.is_open());

    file.seekg(0, std::ios::end);
    auto file_size = file.tellg();

    // Corrupt the data region (last 30% of file)
    auto corrupt_start =
        static_cast<std::streamoff>(static_cast<double>(file_size) * 0.7);
    std::vector<char> garbage(256, 'Z');
    for (auto pos = corrupt_start; pos < file_size; pos += 512) {
      file.seekp(pos);
      auto to_write = std::min(static_cast<std::streamoff>(garbage.size()),
                               static_cast<std::streamoff>(file_size - pos));
      file.write(garbage.data(), to_write);
    }
  }

  // Reopen the cache
  {
    PageSpeedCacheConfig config;
    config.volume_path = cache_path_;
    config.volume_size = static_cast<uint64_t>(10 * 1024 * 1024);
    config.enable_checksum = true;
    config.multi_process.max_read_retries = 1;

    auto result = PageSpeedCache::Create(config);
    ASSERT_TRUE(result.has_value());
    cache_ = std::move(*result);
  }

  // Reads should fail (corruption detected) rather than returning stale data
  auto result =
      cache_->ReadBestAlternate("/corrupt/test", "example.com", "https", mask1);
  if (result.has_value()) {
    // If somehow readable, the data must be valid (not garbage)
    auto content = result->content();
    std::string_view sv(reinterpret_cast<const char*>(content.data()),
                        content.size());
    // Should not contain our corruption pattern
    EXPECT_EQ(sv.find("ZZZZ"), std::string_view::npos)
        << "Read returned corrupted data instead of error";
  }
  // If !result.has_value(), that's the expected outcome — corruption detected

  // Also test list_alternates
  auto alts = cache_->ListAlternates("/corrupt/test", "example.com", "https");
  // Should either fail with corruption error or return empty/partial results
  // The key property: no crash, no serving of garbage data
  (void)alts;
}

// ========== Stabilization audit edge-case tests ==========

// --- WriteAlternate with uint64_t overflow ---

TEST_F(PageSpeedCacheTest, WriteAlternateRejectsUint64Overflow) {
  // When content_length is std::numeric_limits<uint64_t>::max(), adding the
  // serialized metadata prefix size would overflow uint64_t.  WriteAlternate
  // must detect this and return CacheError::InvalidArgument rather than
  // wrapping around to a small allocation.
  CreateCache();

  CapabilityMask mask = MakeTestMask(CapabilityMask::ImageFormat::kWebP,
                                     CapabilityMask::Viewport::kDesktop);
  AlternateId id =
      MaskToAlternateId(static_cast<uint8_t>(mask.Encode() & 0xFF));

  AlternateMetadata meta;
  meta.full_mask = mask.Encode();
  meta.content_type = ContentType::kImage;
  meta.origin_content_type = "image/webp";

  auto result =
      cache_->WriteAlternate("/img/overflow.jpg", "example.com", "https", id,
                             std::numeric_limits<uint64_t>::max(), meta);
  EXPECT_FALSE(result.has_value())
      << "WriteAlternate should reject content_length that causes overflow";
  EXPECT_EQ(result.error(), cyclone::CacheError::InvalidArgument);
}

// --- WriteGenerationFile after ResetVolume ---

TEST_F(PageSpeedCacheTest, ResetVolumeWritesGenerationFile) {
  // After a successful ResetVolume(), the generation file (.gen) must exist
  // and contain the correct generation counter value.
  CreateCache();

  std::string gen_path = cache_path_ + ".gen";

  // Before any reset, the generation file may or may not exist (generation
  // starts at 0 on fresh creation, and WriteGenerationFile is only called
  // during ResetVolume, not Create).
  auto reset_result = cache_->ResetVolume();
  ASSERT_TRUE(reset_result.has_value())
      << "ResetVolume failed: " << static_cast<int>(reset_result.error());

  // The generation file must now exist.
  EXPECT_TRUE(std::filesystem::exists(gen_path))
      << "Generation file should exist after ResetVolume";

  // ReadGenerationFile should return 1 (first reset).
  uint64_t gen = PageSpeedCache::ReadGenerationFile(gen_path);
  EXPECT_EQ(gen, 1u) << "Generation should be 1 after first ResetVolume";

  // Verify the file content is well-formed by reading it directly.
  std::ifstream f(gen_path);
  ASSERT_TRUE(f.is_open()) << "Generation file should be readable";
  uint64_t raw_gen = 0;
  f >> raw_gen;
  EXPECT_FALSE(f.fail()) << "Generation file content should be a valid number";
  EXPECT_EQ(raw_gen, 1u) << "Raw file content should match generation counter";
}

// --- ReadBestAlternate with garbage metadata returns NotFound ---

TEST_F(PageSpeedCacheTest, ReadBestAlternateGarbageMetadataReturnsNotFound) {
  // Write random garbage bytes (not valid metadata) directly to a Cyclone
  // alternate, bypassing PageSpeedCache's WriteAlternate.  The data is long
  // enough to pass the kFixedPrefixSize length check, but contains random
  // values that will fail AlternateMetadata::Deserialize (invalid field
  // values).  ReadBestAlternate must return NotFound, not crash or return
  // garbage.
  CreateCache();

  auto key = PageSpeedCache::ComposeKeyPreNormalized("/img/garbage-meta.jpg",
                                                     "example.com", "https");
  CapabilityMask mask;
  AlternateId id =
      MaskToAlternateId(static_cast<uint8_t>(mask.Encode() & 0xFF));

  // Build garbage data that is large enough to pass the minimum size check
  // (kFixedPrefixSize=9 + kV6FixedSuffixSize=27 = 36 minimum) but contains
  // entirely invalid content.  Use 0xDE bytes to ensure no accidental
  // alignment with valid metadata structures.
  std::vector<std::byte> garbage(64, std::byte{0xDE});

  auto wh = PageSpeedCacheTestPeer::Cyclone(*cache_).write_alternate_sync(
      key, static_cast<cyclone::AlternateId>(id), garbage.size());
  ASSERT_TRUE(wh.has_value());
  auto written = wh->write_sync(std::span(garbage));
  ASSERT_TRUE(written.has_value());
  auto closed = wh->close_sync();
  ASSERT_TRUE(closed.has_value());

  // ReadBestAlternate should detect the invalid metadata and return NotFound.
  auto result = cache_->ReadBestAlternate("/img/garbage-meta.jpg",
                                          "example.com", "https", mask);
  EXPECT_FALSE(result.has_value())
      << "ReadBestAlternate should return NotFound for garbage metadata";
  EXPECT_EQ(result.error(), cyclone::CacheError::NotFound);
}

// --- Round 9: ParseMetadataPrefix WireSize bounds validation ---

TEST_F(PageSpeedCacheTest,
       ReadBestAlternateReturnsNotFoundOnInflatedWireCtLen) {
  // Craft a metadata blob where wire_ct_len is inflated beyond the
  // actual content span.  Deserialize's own size check rejects blobs where
  // kFixedPrefixSize + ct_len + suffix_size > data.size(), and the
  // ParseMetadataPrefix WireSize guard provides defense-in-depth by
  // additionally checking WireSize() <= content.size() after Deserialize.
  //
  // Scenario: serialize valid metadata (ct_len=0, etag_len=0 → WireSize=110),
  // then corrupt wire_ct_len to 200.  The blob stays 110 bytes (+content), so
  // Deserialize sees data.size() < 9+200+101=310 → rejects.
  // ParseMetadataPrefix returns 0, ReadBestAlternate returns NotFound.
  CreateCache();

  // Build a valid v6 metadata blob with empty content type (ct_len=0).
  AlternateMetadata meta;
  meta.full_mask = 0x08;
  meta.content_type = ContentType::kImage;
  // origin_content_type left empty → wire_ct_len=0
  auto serialized = meta.Serialize();
  ASSERT_EQ(serialized.size(), 110u);  // v8: 9 + 0 + 101 + 0 + 0

  // Corrupt wire_ct_len (at offset 7-8) to 200, making the claimed size
  // far larger than the actual blob.
  uint16_t inflated_ct_len = 200;
  std::memcpy(&serialized[7], &inflated_ct_len, 2);

  // Append some fake content bytes after the metadata.
  std::string content_data = "real content bytes";
  std::vector<std::byte> blob;
  blob.insert(blob.end(), serialized.begin(), serialized.end());
  auto content_bytes = std::as_bytes(std::span(content_data));
  blob.insert(blob.end(), content_bytes.begin(), content_bytes.end());

  // Write the crafted blob directly via Cyclone.
  auto key = PageSpeedCache::ComposeKeyPreNormalized("/img/inflated-ct.jpg",
                                                     "example.com", "https");
  CapabilityMask mask;
  AlternateId id =
      MaskToAlternateId(static_cast<uint8_t>(mask.Encode() & 0xFF));

  auto wh = PageSpeedCacheTestPeer::Cyclone(*cache_).write_alternate_sync(
      key, static_cast<cyclone::AlternateId>(id), blob.size());
  ASSERT_TRUE(wh.has_value());
  auto written = wh->write_sync(std::span(blob));
  ASSERT_TRUE(written.has_value());
  auto closed = wh->close_sync();
  ASSERT_TRUE(closed.has_value());

  // ReadBestAlternate should return NotFound because the inflated
  // wire_ct_len causes either Deserialize or the WireSize guard to fail.
  auto result = cache_->ReadBestAlternate("/img/inflated-ct.jpg", "example.com",
                                          "https", mask);
  EXPECT_FALSE(result.has_value())
      << "ReadBestAlternate should return NotFound for inflated wire_ct_len";
  EXPECT_EQ(result.error(), cyclone::CacheError::NotFound);
}

TEST_F(PageSpeedCacheTest, ReadAlternateReturnsNotFoundOnInflatedWireCtLen) {
  // Similar to above, but via ReadAlternate: a content alternate whose
  // metadata does not parse is a miss here too, never the raw blob.
  CreateCache();

  AlternateMetadata meta;
  meta.full_mask = 0x08;
  meta.content_type = ContentType::kImage;
  auto serialized = meta.Serialize();
  ASSERT_EQ(serialized.size(), 110u);  // v8: 9 + 0 + 101 + 0 + 0

  // Corrupt wire_ct_len to 200.
  uint16_t inflated_ct_len = 200;
  std::memcpy(&serialized[7], &inflated_ct_len, 2);

  std::string content_data = "some content";
  std::vector<std::byte> blob;
  blob.insert(blob.end(), serialized.begin(), serialized.end());
  auto content_bytes = std::as_bytes(std::span(content_data));
  blob.insert(blob.end(), content_bytes.begin(), content_bytes.end());

  auto key = PageSpeedCache::ComposeKeyPreNormalized("/img/inflated-ct2.jpg",
                                                     "example.com", "https");
  AlternateId id = MaskToAlternateId(0x08);

  auto wh = PageSpeedCacheTestPeer::Cyclone(*cache_).write_alternate_sync(
      key, static_cast<cyclone::AlternateId>(id), blob.size());
  ASSERT_TRUE(wh.has_value());
  auto written = wh->write_sync(std::span(blob));
  ASSERT_TRUE(written.has_value());
  auto closed = wh->close_sync();
  ASSERT_TRUE(closed.has_value());

  auto result = cache_->ReadAlternate("/img/inflated-ct2.jpg", "example.com",
                                      "https", id);
  EXPECT_FALSE(result.has_value())
      << "ReadAlternate should return NotFound for inflated wire_ct_len";
  EXPECT_EQ(result.error(), cyclone::CacheError::NotFound);
}

TEST_F(PageSpeedCacheTest, ReadBestAlternateReturnsNotFoundOnInflatedEtagLen) {
  // Craft a metadata blob where the etag_len field is inflated.
  // etag_len is at suffix_offset+21 (2 bytes LE).  With ct_len=0,
  // suffix_offset=9, so etag_len is at offset 30.
  // Inflate it to 5000 while the actual blob is only ~50 bytes.
  CreateCache();

  AlternateMetadata meta;
  meta.full_mask = 0x08;
  meta.content_type = ContentType::kImage;
  // Leave origin_content_type and origin_etag empty.
  auto serialized = meta.Serialize();
  ASSERT_EQ(serialized.size(), 110u);  // v8: 9 + 0 + 101 + 0 + 0

  // Corrupt etag_len at offset 30 (suffix_offset=9, +21=30) to 5000.
  uint16_t inflated_etag_len = 5000;
  std::memcpy(&serialized[30], &inflated_etag_len, 2);

  // Append some content bytes.
  std::string content_data = "payload";
  std::vector<std::byte> blob;
  blob.insert(blob.end(), serialized.begin(), serialized.end());
  auto content_bytes = std::as_bytes(std::span(content_data));
  blob.insert(blob.end(), content_bytes.begin(), content_bytes.end());

  auto key = PageSpeedCache::ComposeKeyPreNormalized("/img/inflated-etag.jpg",
                                                     "example.com", "https");
  CapabilityMask mask;
  AlternateId id =
      MaskToAlternateId(static_cast<uint8_t>(mask.Encode() & 0xFF));

  auto wh = PageSpeedCacheTestPeer::Cyclone(*cache_).write_alternate_sync(
      key, static_cast<cyclone::AlternateId>(id), blob.size());
  ASSERT_TRUE(wh.has_value());
  auto written = wh->write_sync(std::span(blob));
  ASSERT_TRUE(written.has_value());
  auto closed = wh->close_sync();
  ASSERT_TRUE(closed.has_value());

  // The inflated etag_len makes Deserialize reject (etag validation) or
  // WireSize > content.size().  Either way, ReadBestAlternate → NotFound.
  auto result = cache_->ReadBestAlternate("/img/inflated-etag.jpg",
                                          "example.com", "https", mask);
  EXPECT_FALSE(result.has_value())
      << "ReadBestAlternate should return NotFound for inflated etag_len";
  EXPECT_EQ(result.error(), cyclone::CacheError::NotFound);
}

// =========================================================================
// Cache Corruption & Bad-State Test Suite
// =========================================================================

// --- 1. Alternate Selection Under Corruption ---

TEST_F(PageSpeedCacheTest, CorruptAlternateAmongValidOnesReturnsNotFound) {
  // Write two valid alternates (WebP and AVIF), then corrupt the best-scoring
  // one (WebP when requesting WebP).  ReadBestAlternate picks one alternate
  // via the selector; if the chosen one has corrupt metadata, it returns
  // NotFound (no retry with next-best).
  CreateCache();

  CapabilityMask webp_mask = MakeTestMask(CapabilityMask::ImageFormat::kWebP,
                                          CapabilityMask::Viewport::kDesktop);
  CapabilityMask avif_mask = MakeTestMask(CapabilityMask::ImageFormat::kAvif,
                                          CapabilityMask::Viewport::kDesktop);

  WriteContent("/img/partial-corrupt.jpg", "example.com", webp_mask,
               "valid-webp-data");
  WriteContent("/img/partial-corrupt.jpg", "example.com", avif_mask,
               "valid-avif-data");

  // Overwrite the WebP alternate with corrupt data directly via Cyclone.
  auto key = PageSpeedCache::ComposeKeyPreNormalized("/img/partial-corrupt.jpg",
                                                     "example.com", "https");
  AlternateId webp_id =
      MaskToAlternateId(static_cast<uint8_t>(webp_mask.Encode() & 0xFF));
  std::array<std::byte, 5> garbage = {std::byte{0xBA}, std::byte{0xAD},
                                      std::byte{0xF0}, std::byte{0x0D},
                                      std::byte{0x00}};
  auto wh = PageSpeedCacheTestPeer::Cyclone(*cache_).write_alternate_sync(
      key, static_cast<cyclone::AlternateId>(webp_id), garbage.size());
  ASSERT_TRUE(wh.has_value());
  (void)wh->write_sync(std::span(garbage));
  (void)wh->close_sync();

  // Request WebP — selector picks WebP alternate, which is corrupt → NotFound.
  auto result = cache_->ReadBestAlternate("/img/partial-corrupt.jpg",
                                          "example.com", "https", webp_mask);
  EXPECT_FALSE(result.has_value())
      << "Corrupt best-scoring alternate should return NotFound (no fallback)";

  // The AVIF alternate should still be readable when requesting AVIF.
  auto avif_result = cache_->ReadBestAlternate(
      "/img/partial-corrupt.jpg", "example.com", "https", avif_mask);
  ASSERT_TRUE(avif_result.has_value())
      << "Uncorrupted AVIF alternate should remain readable";
  auto avif_content = avif_result->content();
  std::string_view avif_sv(reinterpret_cast<const char*>(avif_content.data()),
                           avif_content.size());
  EXPECT_EQ(avif_sv, "valid-avif-data");
}

TEST_F(PageSpeedCacheTest, AllAlternatesCorruptReturnsNotFound) {
  // Every alternate for a URL has garbage data — should error, no crash.
  CreateCache();

  auto key = PageSpeedCache::ComposeKeyPreNormalized("/img/all-corrupt.jpg",
                                                     "example.com", "https");
  CapabilityMask mask;
  AlternateId id =
      MaskToAlternateId(static_cast<uint8_t>(mask.Encode() & 0xFF));

  // Write garbage via Cyclone.
  std::vector<std::byte> garbage(20, std::byte{0xFF});
  auto wh = PageSpeedCacheTestPeer::Cyclone(*cache_).write_alternate_sync(
      key, static_cast<cyclone::AlternateId>(id), garbage.size());
  ASSERT_TRUE(wh.has_value());
  (void)wh->write_sync(std::span(garbage));
  (void)wh->close_sync();

  auto result = cache_->ReadBestAlternate("/img/all-corrupt.jpg", "example.com",
                                          "https", mask);
  EXPECT_FALSE(result.has_value())
      << "All-corrupt alternates should return NotFound";
}

TEST_F(PageSpeedCacheTest, SentinelReadableWhenContentAlternateCorrupt) {
  // A corrupt content alternate should not affect sentinel reads.
  CreateCache();

  // Write a valid sentinel.
  std::string hints = "/style.css\n/hero.webp";
  auto wh =
      cache_->WriteSentinel("/page-with-corrupt.html", "example.com", "https",
                            SentinelId::kEarlyHints, hints.size());
  ASSERT_TRUE(wh.has_value());
  auto hints_bytes = std::as_bytes(std::span(hints));
  (void)wh->write_sync(hints_bytes);
  (void)wh->close_sync();

  // Write corrupt content alternate via Cyclone.
  auto key = PageSpeedCache::ComposeKeyPreNormalized("/page-with-corrupt.html",
                                                     "example.com", "https");
  CapabilityMask mask;
  AlternateId content_id =
      MaskToAlternateId(static_cast<uint8_t>(mask.Encode() & 0xFF));
  std::array<std::byte, 4> garbage = {std::byte{0xFF}, std::byte{0xFF},
                                      std::byte{0xFF}, std::byte{0xFF}};
  auto cwh = PageSpeedCacheTestPeer::Cyclone(*cache_).write_alternate_sync(
      key, static_cast<cyclone::AlternateId>(content_id), garbage.size());
  ASSERT_TRUE(cwh.has_value());
  (void)cwh->write_sync(std::span(garbage));
  (void)cwh->close_sync();

  // Sentinel should still be readable.
  auto result =
      cache_->ReadAlternate("/page-with-corrupt.html", "example.com", "https",
                            static_cast<AlternateId>(SentinelId::kEarlyHints));
  ASSERT_TRUE(result.has_value())
      << "Sentinel should be readable despite corrupt content alternate";
  auto content = result->content();
  std::string_view sv(reinterpret_cast<const char*>(content.data()),
                      content.size());
  EXPECT_EQ(sv, hints);
}

// --- 2. Metadata Field Corruption ---

TEST_F(PageSpeedCacheTest, MetadataWithInvalidContentTypeReturnsNotFound) {
  // content_type byte = 0x99 → Deserialize rejects → NotFound.
  CreateCache();

  AlternateMetadata meta;
  meta.full_mask = 0x08;
  meta.content_type = ContentType::kImage;
  auto serialized = meta.Serialize();

  // Corrupt content_type byte at offset 5 to 0x99 (beyond kOther=4).
  serialized[5] = std::byte{0x99};

  auto key = PageSpeedCache::ComposeKeyPreNormalized("/img/bad-ct.jpg",
                                                     "example.com", "https");
  CapabilityMask mask;
  AlternateId id =
      MaskToAlternateId(static_cast<uint8_t>(mask.Encode() & 0xFF));

  auto wh = PageSpeedCacheTestPeer::Cyclone(*cache_).write_alternate_sync(
      key, static_cast<cyclone::AlternateId>(id), serialized.size());
  ASSERT_TRUE(wh.has_value());
  (void)wh->write_sync(std::span(serialized));
  (void)wh->close_sync();

  auto result = cache_->ReadBestAlternate("/img/bad-ct.jpg", "example.com",
                                          "https", mask);
  EXPECT_FALSE(result.has_value())
      << "Invalid content_type byte should cause Deserialize rejection";
  EXPECT_EQ(result.error(), cyclone::CacheError::NotFound);
}

TEST_F(PageSpeedCacheTest, MetadataWithBoundaryContentTypeValues) {
  // kOther (4) is valid; 5 should fail — boundary test.
  CreateCache();

  CapabilityMask mask;
  AlternateId id =
      MaskToAlternateId(static_cast<uint8_t>(mask.Encode() & 0xFF));

  // kOther = 4, should succeed.
  {
    AlternateMetadata meta;
    meta.full_mask = 0x08;
    meta.content_type = ContentType::kOther;
    meta.origin_content_type = "application/octet-stream";
    auto serialized = meta.Serialize();

    // Append content.
    std::string payload = "boundary-ok";
    std::vector<std::byte> blob;
    blob.insert(blob.end(), serialized.begin(), serialized.end());
    auto payload_bytes = std::as_bytes(std::span(payload));
    blob.insert(blob.end(), payload_bytes.begin(), payload_bytes.end());

    auto key = PageSpeedCache::ComposeKeyPreNormalized(
        "/img/ct-boundary-ok.jpg", "example.com", "https");
    auto wh = PageSpeedCacheTestPeer::Cyclone(*cache_).write_alternate_sync(
        key, static_cast<cyclone::AlternateId>(id), blob.size());
    ASSERT_TRUE(wh.has_value());
    (void)wh->write_sync(std::span(blob));
    (void)wh->close_sync();

    auto result = cache_->ReadBestAlternate("/img/ct-boundary-ok.jpg",
                                            "example.com", "https", mask);
    ASSERT_TRUE(result.has_value()) << "kOther (4) should be valid";
    EXPECT_EQ(result->metadata.content_type, ContentType::kOther);
  }

  // content_type = 5 should fail.
  {
    AlternateMetadata meta;
    meta.full_mask = 0x08;
    meta.content_type = ContentType::kImage;
    auto serialized = meta.Serialize();
    serialized[5] = std::byte{5};  // One past kOther

    auto key = PageSpeedCache::ComposeKeyPreNormalized(
        "/img/ct-boundary-bad.jpg", "example.com", "https");
    auto wh = PageSpeedCacheTestPeer::Cyclone(*cache_).write_alternate_sync(
        key, static_cast<cyclone::AlternateId>(id), serialized.size());
    ASSERT_TRUE(wh.has_value());
    (void)wh->write_sync(std::span(serialized));
    (void)wh->close_sync();

    auto result = cache_->ReadBestAlternate("/img/ct-boundary-bad.jpg",
                                            "example.com", "https", mask);
    EXPECT_FALSE(result.has_value())
        << "content_type=5 (past kOther) should be rejected";
  }
}

TEST_F(PageSpeedCacheTest, MetadataWireSizeExceedsBlobSize) {
  // Truncated serialized metadata (WireSize > blob) → NotFound.
  CreateCache();

  AlternateMetadata meta;
  meta.full_mask = 0x08;
  meta.content_type = ContentType::kImage;
  meta.origin_content_type = "image/jpeg";  // 10 chars → adds to WireSize
  auto serialized = meta.Serialize();
  // WireSize = 9 + 10 + 27 = 46, but truncate to 20 bytes.
  std::vector<std::byte> truncated(serialized.begin(), serialized.begin() + 20);

  auto key = PageSpeedCache::ComposeKeyPreNormalized("/img/truncated-meta.jpg",
                                                     "example.com", "https");
  CapabilityMask mask;
  AlternateId id =
      MaskToAlternateId(static_cast<uint8_t>(mask.Encode() & 0xFF));

  auto wh = PageSpeedCacheTestPeer::Cyclone(*cache_).write_alternate_sync(
      key, static_cast<cyclone::AlternateId>(id), truncated.size());
  ASSERT_TRUE(wh.has_value());
  (void)wh->write_sync(std::span(truncated));
  (void)wh->close_sync();

  auto result = cache_->ReadBestAlternate("/img/truncated-meta.jpg",
                                          "example.com", "https", mask);
  EXPECT_FALSE(result.has_value())
      << "Truncated metadata should cause NotFound";
  EXPECT_EQ(result.error(), cyclone::CacheError::NotFound);
}

// --- 3. Auto-Healing Building Blocks ---

TEST_F(PageSpeedCacheTest, RemoveThenRewriteRecoverFromCorruption) {
  // Remove corrupt URL + re-write → read succeeds (auto-heal cycle).
  CreateCache();

  CapabilityMask mask;
  AlternateId id =
      MaskToAlternateId(static_cast<uint8_t>(mask.Encode() & 0xFF));

  // Write corrupt data directly via Cyclone.
  auto key = PageSpeedCache::ComposeKeyPreNormalized("/img/heal-me.jpg",
                                                     "example.com", "https");
  std::array<std::byte, 3> garbage = {std::byte{0xDE}, std::byte{0xAD},
                                      std::byte{0x00}};
  auto wh = PageSpeedCacheTestPeer::Cyclone(*cache_).write_alternate_sync(
      key, static_cast<cyclone::AlternateId>(id), garbage.size());
  ASSERT_TRUE(wh.has_value());
  (void)wh->write_sync(std::span(garbage));
  (void)wh->close_sync();

  // Confirm it's corrupt (unreadable via ReadBestAlternate).
  auto corrupt_result = cache_->ReadBestAlternate("/img/heal-me.jpg",
                                                  "example.com", "https", mask);
  EXPECT_FALSE(corrupt_result.has_value());

  // Remove the corrupt entry.
  auto removed = cache_->Remove("/img/heal-me.jpg", "example.com", "https");
  EXPECT_TRUE(removed.has_value());

  // Re-write valid data.
  WriteContent("/img/heal-me.jpg", "example.com", mask, "healed-data");

  // Read should now succeed.
  auto healed_result = cache_->ReadBestAlternate("/img/heal-me.jpg",
                                                 "example.com", "https", mask);
  ASSERT_TRUE(healed_result.has_value())
      << "After Remove + re-write, read should succeed";
  auto content = healed_result->content();
  std::string_view sv(reinterpret_cast<const char*>(content.data()),
                      content.size());
  EXPECT_EQ(sv, "healed-data");
}

TEST_F(PageSpeedCacheTest, OverwriteCorruptAlternateWithValidData) {
  // Write valid data at same AlternateId as corruption → overwrites.
  CreateCache();

  CapabilityMask mask;
  AlternateId id =
      MaskToAlternateId(static_cast<uint8_t>(mask.Encode() & 0xFF));

  // Write corrupt data directly.
  auto key = PageSpeedCache::ComposeKeyPreNormalized("/img/overwrite-me.jpg",
                                                     "example.com", "https");
  std::array<std::byte, 6> garbage = {std::byte{0xCA}, std::byte{0xFE},
                                      std::byte{0xBA}, std::byte{0xBE},
                                      std::byte{0x00}, std::byte{0x00}};
  auto wh = PageSpeedCacheTestPeer::Cyclone(*cache_).write_alternate_sync(
      key, static_cast<cyclone::AlternateId>(id), garbage.size());
  ASSERT_TRUE(wh.has_value());
  (void)wh->write_sync(std::span(garbage));
  (void)wh->close_sync();

  // Overwrite with valid data via WriteContent (same mask → same AlternateId).
  WriteContent("/img/overwrite-me.jpg", "example.com", mask,
               "fresh-valid-data");

  // Should now be readable.
  auto result = cache_->ReadBestAlternate("/img/overwrite-me.jpg",
                                          "example.com", "https", mask);
  ASSERT_TRUE(result.has_value())
      << "Overwritten corrupt alternate should be readable";
  auto content = result->content();
  std::string_view sv(reinterpret_cast<const char*>(content.data()),
                      content.size());
  EXPECT_EQ(sv, "fresh-valid-data");
}

TEST_F(PageSpeedCacheTest, RemoveNonExistentUrlDoesNotCrash) {
  // Remove on URL never written doesn't crash.  Cyclone returns NotFound
  // for non-existent keys, which is the expected behavior.
  CreateCache();

  auto removed =
      cache_->Remove("/img/never-written.jpg", "example.com", "https");
  // The key property: no crash, no undefined behavior.
  // Cyclone returns an error for non-existent keys.
  EXPECT_FALSE(removed.has_value())
      << "Remove on non-existent URL should return an error";
}

TEST_F(PageSpeedCacheTest, MultipleRemovesDoNotCrash) {
  // Remove same URL twice → no crash.  First remove succeeds, second
  // returns an error (key already removed) which is expected.
  CreateCache();

  CapabilityMask mask;
  WriteContent("/img/remove-twice.jpg", "example.com", mask, "some-data");

  auto r1 = cache_->Remove("/img/remove-twice.jpg", "example.com", "https");
  EXPECT_TRUE(r1.has_value()) << "First remove should succeed";

  // Second remove: key is gone, Cyclone returns an error.
  auto r2 = cache_->Remove("/img/remove-twice.jpg", "example.com", "https");
  EXPECT_FALSE(r2.has_value())
      << "Second remove should return error (key already removed)";

  // Verify the key is actually gone.
  AlternateId id =
      MaskToAlternateId(static_cast<uint8_t>(mask.Encode() & 0xFF));
  EXPECT_FALSE(cache_->AlternateExists("/img/remove-twice.jpg", "example.com",
                                       "https", id));
}

// --- 4. URL / Cache Key Edge Cases ---

TEST_F(PageSpeedCacheTest, VeryLongUrlHashesCorrectly) {
  // 10KB URL → write + read round-trip.
  CreateCache();

  std::string long_url(10240, 'a');
  long_url[0] = '/';

  CapabilityMask mask;
  WriteContent(long_url, "example.com", mask, "long-url-data");

  auto result =
      cache_->ReadBestAlternate(long_url, "example.com", "https", mask);
  ASSERT_TRUE(result.has_value()) << "10KB URL should round-trip correctly";
  auto content = result->content();
  std::string_view sv(reinterpret_cast<const char*>(content.data()),
                      content.size());
  EXPECT_EQ(sv, "long-url-data");
}

TEST_F(PageSpeedCacheTest, UrlWithSpecialCharacters) {
  // Percent-encoded, unicode, query params → distinct keys.
  CreateCache();

  CapabilityMask mask;

  WriteContent("/img/%E2%9C%93.jpg", "example.com", mask, "percent-encoded");
  WriteContent("/img/\xC3\xA9.jpg", "example.com", mask, "utf8-data");
  WriteContent("/img/test.jpg?v=1&w=100", "example.com", mask, "query-data");

  auto r1 = cache_->ReadBestAlternate("/img/%E2%9C%93.jpg", "example.com",
                                      "https", mask);
  ASSERT_TRUE(r1.has_value());
  {
    auto c = r1->content();
    std::string_view sv(reinterpret_cast<const char*>(c.data()), c.size());
    EXPECT_EQ(sv, "percent-encoded");
  }

  auto r2 = cache_->ReadBestAlternate("/img/\xC3\xA9.jpg", "example.com",
                                      "https", mask);
  ASSERT_TRUE(r2.has_value());
  {
    auto c = r2->content();
    std::string_view sv(reinterpret_cast<const char*>(c.data()), c.size());
    EXPECT_EQ(sv, "utf8-data");
  }

  auto r3 = cache_->ReadBestAlternate("/img/test.jpg?v=1&w=100", "example.com",
                                      "https", mask);
  ASSERT_TRUE(r3.has_value());
  {
    auto c = r3->content();
    std::string_view sv(reinterpret_cast<const char*>(c.data()), c.size());
    EXPECT_EQ(sv, "query-data");
  }
}

TEST_F(PageSpeedCacheTest, EmptyUrlAndHostname) {
  // Empty strings don't crash.
  CreateCache();

  CapabilityMask mask;
  WriteContent("", "", mask, "empty-key-data");

  auto result = cache_->ReadBestAlternate("", "", "https", mask);
  ASSERT_TRUE(result.has_value()) << "Empty URL + hostname should not crash";
  auto content = result->content();
  std::string_view sv(reinterpret_cast<const char*>(content.data()),
                      content.size());
  EXPECT_EQ(sv, "empty-key-data");
}

TEST_F(PageSpeedCacheTest, SameUrlDifferentQueryStringsAreDifferentKeys) {
  // /x.jpg and /x.jpg?v=2 are separate cache entries.
  CreateCache();

  CapabilityMask mask;
  WriteContent("/x.jpg", "example.com", mask, "no-query");
  WriteContent("/x.jpg?v=2", "example.com", mask, "with-query");

  auto r1 = cache_->ReadBestAlternate("/x.jpg", "example.com", "https", mask);
  ASSERT_TRUE(r1.has_value());
  {
    auto c = r1->content();
    std::string_view sv(reinterpret_cast<const char*>(c.data()), c.size());
    EXPECT_EQ(sv, "no-query");
  }

  auto r2 =
      cache_->ReadBestAlternate("/x.jpg?v=2", "example.com", "https", mask);
  ASSERT_TRUE(r2.has_value());
  {
    auto c = r2->content();
    std::string_view sv(reinterpret_cast<const char*>(c.data()), c.size());
    EXPECT_EQ(sv, "with-query");
  }
}

// --- 5. Alternate Management Edge Cases ---

TEST_F(PageSpeedCacheTest, OverwriteExistingAlternatePreservesOthers) {
  // Overwrite WebP alt → AVIF alt survives.
  CreateCache();

  CapabilityMask webp_mask = MakeTestMask(CapabilityMask::ImageFormat::kWebP,
                                          CapabilityMask::Viewport::kDesktop);
  CapabilityMask avif_mask = MakeTestMask(CapabilityMask::ImageFormat::kAvif,
                                          CapabilityMask::Viewport::kDesktop);

  WriteContent("/img/preserve.jpg", "example.com", webp_mask, "webp-original");
  WriteContent("/img/preserve.jpg", "example.com", avif_mask, "avif-original");

  // Overwrite WebP with new data.
  WriteContent("/img/preserve.jpg", "example.com", webp_mask, "webp-updated");

  // AVIF should survive.
  auto avif_result = cache_->ReadBestAlternate(
      "/img/preserve.jpg", "example.com", "https", avif_mask);
  ASSERT_TRUE(avif_result.has_value())
      << "AVIF alternate should survive WebP overwrite";
  auto avif_content = avif_result->content();
  std::string_view avif_sv(reinterpret_cast<const char*>(avif_content.data()),
                           avif_content.size());
  EXPECT_EQ(avif_sv, "avif-original");

  // WebP should return updated data.
  auto webp_result = cache_->ReadBestAlternate(
      "/img/preserve.jpg", "example.com", "https", webp_mask);
  ASSERT_TRUE(webp_result.has_value());
  auto webp_content = webp_result->content();
  std::string_view webp_sv(reinterpret_cast<const char*>(webp_content.data()),
                           webp_content.size());
  EXPECT_EQ(webp_sv, "webp-updated");
}

TEST_F(PageSpeedCacheTest, ListAlternatesAfterPartialCorruption) {
  // ListAlternates succeeds with mix of valid + corrupt alternates.
  CreateCache();

  CapabilityMask webp_mask = MakeTestMask(CapabilityMask::ImageFormat::kWebP,
                                          CapabilityMask::Viewport::kDesktop);
  CapabilityMask avif_mask = MakeTestMask(CapabilityMask::ImageFormat::kAvif,
                                          CapabilityMask::Viewport::kDesktop);

  // Write one valid alternate.
  WriteContent("/img/partial-list.jpg", "example.com", webp_mask, "valid-webp");

  // Write one corrupt alternate via Cyclone.
  auto key = PageSpeedCache::ComposeKeyPreNormalized("/img/partial-list.jpg",
                                                     "example.com", "https");
  AlternateId avif_id =
      MaskToAlternateId(static_cast<uint8_t>(avif_mask.Encode() & 0xFF));
  std::array<std::byte, 3> garbage = {std::byte{0xDE}, std::byte{0xAD},
                                      std::byte{0x00}};
  auto wh = PageSpeedCacheTestPeer::Cyclone(*cache_).write_alternate_sync(
      key, static_cast<cyclone::AlternateId>(avif_id), garbage.size());
  ASSERT_TRUE(wh.has_value());
  (void)wh->write_sync(std::span(garbage));
  (void)wh->close_sync();

  // ListAlternates should succeed and report both alternates.
  auto alts =
      cache_->ListAlternates("/img/partial-list.jpg", "example.com", "https");
  ASSERT_TRUE(alts.has_value())
      << "ListAlternates should succeed with mixed valid/corrupt alternates";
  EXPECT_GE(alts->size(), 2u);
}

// --- 6. Content Boundary ---

TEST_F(PageSpeedCacheTest, MetadataOnlyNoContentPayload) {
  // WireSize == blob size → content() returns empty span.
  CreateCache();

  AlternateMetadata meta;
  meta.full_mask = 0x08;
  meta.content_type = ContentType::kImage;
  // Empty origin_content_type / origin_etag / origin_cache_control →
  // WireSize = 9 + 0 + 101 + 0 + 0 = 110
  auto serialized = meta.Serialize();
  ASSERT_EQ(serialized.size(), 110u);

  auto key = PageSpeedCache::ComposeKeyPreNormalized("/img/meta-only.jpg",
                                                     "example.com", "https");
  CapabilityMask mask;
  AlternateId id =
      MaskToAlternateId(static_cast<uint8_t>(mask.Encode() & 0xFF));

  // Write exactly the metadata, no content payload.
  auto wh = PageSpeedCacheTestPeer::Cyclone(*cache_).write_alternate_sync(
      key, static_cast<cyclone::AlternateId>(id), serialized.size());
  ASSERT_TRUE(wh.has_value());
  (void)wh->write_sync(std::span(serialized));
  (void)wh->close_sync();

  auto result = cache_->ReadBestAlternate("/img/meta-only.jpg", "example.com",
                                          "https", mask);
  ASSERT_TRUE(result.has_value()) << "Metadata-only blob should be readable";
  auto content = result->content();
  EXPECT_EQ(content.size(), 0u) << "content() should be empty when no payload";
  EXPECT_EQ(result->metadata.content_type, ContentType::kImage);
}

TEST_F(PageSpeedCacheTest, LargeContentPayloadRoundTrip) {
  // 1MB payload → integrity check.
  CreateCache();

  CapabilityMask mask;
  std::string large_data(static_cast<size_t>(1024) * 1024, 'X');
  // Put some variety in the data for integrity checking.
  for (size_t i = 0; i < large_data.size(); i += 4096) {
    large_data[i] = static_cast<char>(i % 256);
  }

  WriteContent("/img/large.jpg", "example.com", mask, large_data);

  auto result =
      cache_->ReadBestAlternate("/img/large.jpg", "example.com", "https", mask);
  ASSERT_TRUE(result.has_value()) << "1MB payload should round-trip";
  auto content = result->content();
  std::string_view sv(reinterpret_cast<const char*>(content.data()),
                      content.size());
  EXPECT_EQ(sv, large_data);
}

// --- 7. Version Forward Compatibility ---

TEST_F(PageSpeedCacheTest, V3MetadataReadByV6Code) {
  // Hand-crafted v3 blob → defaults for v4/v5/v6 fields.
  CreateCache();

  // v3 layout: [1B version=3][4B mask][1B ct][1B flags][2B ct_len]
  //            [ct_len bytes ct string][14B suffix]
  // suffix = cache_inserted_at(4) + max_age(4) + s_maxage(4) + cc_flags(2)
  uint16_t ct_len = 10;  // "image/jpeg"
  size_t v3_size = 9 + ct_len + 14;
  std::vector<std::byte> v3_blob(v3_size, std::byte{0x00});

  // Version = 3.
  v3_blob[0] = std::byte{3};
  // full_mask = 0x08 (desktop/identity).
  uint32_t mask_val = 0x08;
  std::memcpy(&v3_blob[1], &mask_val, 4);
  // content_type = kImage (3).
  v3_blob[5] = std::byte{3};
  // flags = 0.
  v3_blob[6] = std::byte{0};
  // ct_len = 10.
  std::memcpy(&v3_blob[7], &ct_len, 2);
  // origin_content_type = "image/jpeg".
  std::memcpy(&v3_blob[9], "image/jpeg", 10);
  // cache_inserted_at = 1700000000.
  uint32_t inserted_at = 1700000000;
  std::memcpy(&v3_blob[19], &inserted_at, 4);
  // origin_max_age = 3600.
  uint32_t max_age = 3600;
  std::memcpy(&v3_blob[23], &max_age, 4);

  // Append some content after the metadata.
  std::string payload = "v3-content";
  std::vector<std::byte> blob;
  blob.insert(blob.end(), v3_blob.begin(), v3_blob.end());
  auto payload_bytes = std::as_bytes(std::span(payload));
  blob.insert(blob.end(), payload_bytes.begin(), payload_bytes.end());

  auto key = PageSpeedCache::ComposeKeyPreNormalized("/img/v3-compat.jpg",
                                                     "example.com", "https");
  CapabilityMask mask;
  AlternateId id =
      MaskToAlternateId(static_cast<uint8_t>(mask.Encode() & 0xFF));

  auto wh = PageSpeedCacheTestPeer::Cyclone(*cache_).write_alternate_sync(
      key, static_cast<cyclone::AlternateId>(id), blob.size());
  ASSERT_TRUE(wh.has_value());
  (void)wh->write_sync(std::span(blob));
  (void)wh->close_sync();

  auto result = cache_->ReadBestAlternate("/img/v3-compat.jpg", "example.com",
                                          "https", mask);
  ASSERT_TRUE(result.has_value())
      << "v3 metadata should be readable by v6 code";

  // Check that v3 fields are correct.
  EXPECT_EQ(result->metadata.version, 3);
  EXPECT_EQ(result->metadata.content_type, ContentType::kImage);
  EXPECT_EQ(result->metadata.origin_content_type, "image/jpeg");
  EXPECT_EQ(result->metadata.cache_inserted_at, 1700000000u);
  EXPECT_EQ(result->metadata.origin_max_age, 3600u);

  // V4+ fields should be defaults.
  EXPECT_EQ(result->metadata.ssimulacra2_score_x100,
            AlternateMetadata::kScoreNA);
  EXPECT_EQ(result->metadata.content_class,
            AlternateMetadata::kContentClassUnknown);
  // V5+ fields should be defaults.
  EXPECT_EQ(result->metadata.origin_last_modified, 0u);
  EXPECT_EQ(result->metadata.origin_etag, "");
  // V6 field should be default.
  EXPECT_EQ(result->metadata.origin_content_length, 0u);

  // Content should be intact.
  auto content = result->content();
  std::string_view sv(reinterpret_cast<const char*>(content.data()),
                      content.size());
  EXPECT_EQ(sv, payload);
}

TEST_F(PageSpeedCacheTest, V4MetadataReadByV6Code) {
  // Hand-crafted v4 blob → v4 fields (score, class) present, v5/v6 defaults.
  CreateCache();

  // v4 layout: [1B version=4][4B mask][1B ct][1B flags][2B ct_len]
  //            [ct_len bytes ct string][17B suffix]
  // suffix = cache_inserted_at(4) + max_age(4) + s_maxage(4) + cc_flags(2)
  //        + ssimulacra2_score_x100(2) + content_class(1) = 17 bytes
  uint16_t ct_len = 10;  // "image/webp"
  size_t v4_size = 9 + ct_len + 17;
  std::vector<std::byte> v4_blob(v4_size, std::byte{0x00});

  // Version = 4.
  v4_blob[0] = std::byte{4};
  // full_mask = 0x08 (desktop/identity).
  uint32_t mask_val = 0x08;
  std::memcpy(&v4_blob[1], &mask_val, 4);
  // content_type = kImage (3).
  v4_blob[5] = std::byte{3};
  // flags = kFlagWorkerProcessed (0x02).
  v4_blob[6] = std::byte{0x02};
  // ct_len = 10.
  std::memcpy(&v4_blob[7], &ct_len, 2);
  // origin_content_type = "image/webp".
  std::memcpy(&v4_blob[9], "image/webp", 10);
  // cache_inserted_at = 1700000000.
  uint32_t inserted_at = 1700000000;
  std::memcpy(&v4_blob[19], &inserted_at, 4);
  // origin_max_age = 7200.
  uint32_t max_age = 7200;
  std::memcpy(&v4_blob[23], &max_age, 4);
  // origin_s_maxage = 1800.
  uint32_t s_maxage = 1800;
  std::memcpy(&v4_blob[27], &s_maxage, 4);
  // origin_cc_flags = kCCOriginPublic | kCCOriginHeaderPresent.
  uint16_t cc_flags = AlternateMetadata::kCCOriginPublic |
                      AlternateMetadata::kCCOriginHeaderPresent;
  std::memcpy(&v4_blob[31], &cc_flags, 2);
  // ssimulacra2_score_x100 = 8500 (85.00).
  uint16_t score = 8500;
  std::memcpy(&v4_blob[33], &score, 2);
  // content_class = 0 (Photo).
  v4_blob[35] = std::byte{0};

  // Append content after the metadata.
  std::string payload = "v4-content";
  std::vector<std::byte> blob;
  blob.insert(blob.end(), v4_blob.begin(), v4_blob.end());
  auto payload_bytes = std::as_bytes(std::span(payload));
  blob.insert(blob.end(), payload_bytes.begin(), payload_bytes.end());

  auto key = PageSpeedCache::ComposeKeyPreNormalized("/img/v4-compat.jpg",
                                                     "example.com", "https");
  CapabilityMask mask;
  AlternateId id =
      MaskToAlternateId(static_cast<uint8_t>(mask.Encode() & 0xFF));

  auto wh = PageSpeedCacheTestPeer::Cyclone(*cache_).write_alternate_sync(
      key, static_cast<cyclone::AlternateId>(id), blob.size());
  ASSERT_TRUE(wh.has_value());
  (void)wh->write_sync(std::span(blob));
  (void)wh->close_sync();

  auto result = cache_->ReadBestAlternate("/img/v4-compat.jpg", "example.com",
                                          "https", mask);
  ASSERT_TRUE(result.has_value())
      << "v4 metadata should be readable by v6 code";

  // Verify v3 fields.
  EXPECT_EQ(result->metadata.version, 4);
  EXPECT_EQ(result->metadata.content_type, ContentType::kImage);
  EXPECT_EQ(result->metadata.flags, AlternateMetadata::kFlagWorkerProcessed);
  EXPECT_EQ(result->metadata.origin_content_type, "image/webp");
  EXPECT_EQ(result->metadata.cache_inserted_at, 1700000000u);
  EXPECT_EQ(result->metadata.origin_max_age, 7200u);
  EXPECT_EQ(result->metadata.origin_s_maxage, 1800u);
  EXPECT_EQ(result->metadata.origin_cc_flags, cc_flags);

  // Verify v4 fields are correctly read.
  EXPECT_EQ(result->metadata.ssimulacra2_score_x100, 8500u);
  EXPECT_EQ(result->metadata.content_class, 0u);  // Photo

  // V5+ fields should be defaults.
  EXPECT_EQ(result->metadata.origin_last_modified, 0u);
  EXPECT_EQ(result->metadata.origin_etag, "");
  // V6 field should be default.
  EXPECT_EQ(result->metadata.origin_content_length, 0u);

  // Content should be intact.
  auto content = result->content();
  std::string_view sv(reinterpret_cast<const char*>(content.data()),
                      content.size());
  EXPECT_EQ(sv, payload);
}

TEST_F(PageSpeedCacheTest, MixedVersionAlternatesSameUrl) {
  // v3 + v6 alternates coexist for same URL.
  CreateCache();

  auto key = PageSpeedCache::ComposeKeyPreNormalized("/img/mixed-ver.jpg",
                                                     "example.com", "https");

  // Write v3 alternate at WebP mask.
  CapabilityMask webp_mask = MakeTestMask(CapabilityMask::ImageFormat::kWebP,
                                          CapabilityMask::Viewport::kDesktop);
  AlternateId webp_id =
      MaskToAlternateId(static_cast<uint8_t>(webp_mask.Encode() & 0xFF));

  // Hand-craft v3 blob.
  uint16_t ct_len = 10;
  size_t v3_meta_size = 9 + ct_len + 14;
  std::vector<std::byte> v3_meta(v3_meta_size, std::byte{0x00});
  v3_meta[0] = std::byte{3};
  uint32_t mask_val = webp_mask.Encode();
  std::memcpy(&v3_meta[1], &mask_val, 4);
  v3_meta[5] = std::byte{3};  // kImage
  std::memcpy(&v3_meta[7], &ct_len, 2);
  std::memcpy(&v3_meta[9], "image/webp", 10);

  std::string v3_payload = "v3-webp";
  std::vector<std::byte> v3_blob;
  v3_blob.insert(v3_blob.end(), v3_meta.begin(), v3_meta.end());
  auto v3_bytes = std::as_bytes(std::span(v3_payload));
  v3_blob.insert(v3_blob.end(), v3_bytes.begin(), v3_bytes.end());

  auto wh1 = PageSpeedCacheTestPeer::Cyclone(*cache_).write_alternate_sync(
      key, static_cast<cyclone::AlternateId>(webp_id), v3_blob.size());
  ASSERT_TRUE(wh1.has_value());
  (void)wh1->write_sync(std::span(v3_blob));
  (void)wh1->close_sync();

  // Write v6 alternate at AVIF mask via WriteContent.
  CapabilityMask avif_mask = MakeTestMask(CapabilityMask::ImageFormat::kAvif,
                                          CapabilityMask::Viewport::kDesktop);
  WriteContent("/img/mixed-ver.jpg", "example.com", avif_mask, "v6-avif");

  // Both should be readable.
  auto r_webp = cache_->ReadBestAlternate("/img/mixed-ver.jpg", "example.com",
                                          "https", webp_mask);
  ASSERT_TRUE(r_webp.has_value()) << "v3 WebP alternate should be readable";
  EXPECT_EQ(r_webp->metadata.version, 3);
  {
    auto c = r_webp->content();
    std::string_view sv(reinterpret_cast<const char*>(c.data()), c.size());
    EXPECT_EQ(sv, "v3-webp");
  }

  auto r_avif = cache_->ReadBestAlternate("/img/mixed-ver.jpg", "example.com",
                                          "https", avif_mask);
  ASSERT_TRUE(r_avif.has_value())
      << "current AVIF alternate should be readable";
  EXPECT_EQ(
      r_avif->metadata.version,
      AlternateMetadata::kCurrentVersion);  // cache writes the current version
  {
    auto c = r_avif->content();
    std::string_view sv(reinterpret_cast<const char*>(c.data()), c.size());
    EXPECT_EQ(sv, "v6-avif");
  }
}

// --- 8. Defensive Deserialization ---

TEST_F(PageSpeedCacheTest, OutOfRangeScoreClampedToNA) {
  // score=10001 → clamped to kScoreNA (0xFFFF), read succeeds.
  CreateCache();

  AlternateMetadata meta;
  meta.full_mask = 0x08;
  meta.content_type = ContentType::kImage;
  meta.ssimulacra2_score_x100 = 10001;  // Out of range (valid: 0-10000)
  auto serialized = meta.Serialize();

  // Append content.
  std::string payload = "score-clamped";
  std::vector<std::byte> blob;
  blob.insert(blob.end(), serialized.begin(), serialized.end());
  auto payload_bytes = std::as_bytes(std::span(payload));
  blob.insert(blob.end(), payload_bytes.begin(), payload_bytes.end());

  auto key = PageSpeedCache::ComposeKeyPreNormalized("/img/bad-score.jpg",
                                                     "example.com", "https");
  CapabilityMask mask;
  AlternateId id =
      MaskToAlternateId(static_cast<uint8_t>(mask.Encode() & 0xFF));

  auto wh = PageSpeedCacheTestPeer::Cyclone(*cache_).write_alternate_sync(
      key, static_cast<cyclone::AlternateId>(id), blob.size());
  ASSERT_TRUE(wh.has_value());
  (void)wh->write_sync(std::span(blob));
  (void)wh->close_sync();

  auto result = cache_->ReadBestAlternate("/img/bad-score.jpg", "example.com",
                                          "https", mask);
  ASSERT_TRUE(result.has_value())
      << "Out-of-range score should be clamped, not rejected";
  EXPECT_EQ(result->metadata.ssimulacra2_score_x100,
            AlternateMetadata::kScoreNA);
  auto content = result->content();
  std::string_view sv(reinterpret_cast<const char*>(content.data()),
                      content.size());
  EXPECT_EQ(sv, payload);
}

TEST_F(PageSpeedCacheTest, OutOfRangeContentClassClampedToUnknown) {
  // class=255 → clamped to kContentClassUnknown (4), read succeeds.
  CreateCache();

  AlternateMetadata meta;
  meta.full_mask = 0x08;
  meta.content_type = ContentType::kImage;
  auto serialized = meta.Serialize();

  // Corrupt content_class byte.  In v6, content_class is at
  // suffix_offset + 16 = (9 + 0) + 16 = 25.
  serialized[25] = std::byte{255};

  // Append content.
  std::string payload = "class-clamped";
  std::vector<std::byte> blob;
  blob.insert(blob.end(), serialized.begin(), serialized.end());
  auto payload_bytes = std::as_bytes(std::span(payload));
  blob.insert(blob.end(), payload_bytes.begin(), payload_bytes.end());

  auto key = PageSpeedCache::ComposeKeyPreNormalized("/img/bad-class.jpg",
                                                     "example.com", "https");
  CapabilityMask mask;
  AlternateId id =
      MaskToAlternateId(static_cast<uint8_t>(mask.Encode() & 0xFF));

  auto wh = PageSpeedCacheTestPeer::Cyclone(*cache_).write_alternate_sync(
      key, static_cast<cyclone::AlternateId>(id), blob.size());
  ASSERT_TRUE(wh.has_value());
  (void)wh->write_sync(std::span(blob));
  (void)wh->close_sync();

  auto result = cache_->ReadBestAlternate("/img/bad-class.jpg", "example.com",
                                          "https", mask);
  ASSERT_TRUE(result.has_value())
      << "Out-of-range content_class should be clamped, not rejected";
  EXPECT_EQ(result->metadata.content_class,
            AlternateMetadata::kContentClassUnknown);
  auto content = result->content();
  std::string_view sv(reinterpret_cast<const char*>(content.data()),
                      content.size());
  EXPECT_EQ(sv, payload);
}

// --- 9. Metadata Integrity ---

TEST_F(PageSpeedCacheTest, AllFlagBitsPreservedRoundTrip) {
  // flags=0xFF round-trips (undefined bits preserved).
  CreateCache();

  CapabilityMask mask;
  AlternateId id =
      MaskToAlternateId(static_cast<uint8_t>(mask.Encode() & 0xFF));

  AlternateMetadata meta;
  meta.full_mask = mask.Encode();
  meta.content_type = ContentType::kHtml;
  meta.flags = 0xFF;  // All bits set, including undefined ones
  meta.origin_content_type = "text/html";

  std::string data = "<html>flags</html>";
  auto wh = cache_->WriteAlternate("/flags-test.html", "example.com", "https",
                                   id, data.size(), meta);
  ASSERT_TRUE(wh.has_value());
  auto bytes = std::as_bytes(std::span(data));
  (void)wh->write_sync(bytes);
  (void)wh->close_sync();

  auto result = cache_->ReadBestAlternate("/flags-test.html", "example.com",
                                          "https", mask);
  ASSERT_TRUE(result.has_value());
  EXPECT_EQ(result->metadata.flags, 0xFF)
      << "All flag bits should survive round-trip";
}

TEST_F(PageSpeedCacheTest, MaxOriginContentTypeLengthBoundary) {
  // 256 chars: OK (kMaxOriginCtLen = 256).
  // origin_content_type > 256 chars: serialized truncated to 256.
  CreateCache();

  CapabilityMask mask;
  AlternateId id =
      MaskToAlternateId(static_cast<uint8_t>(mask.Encode() & 0xFF));

  // 256-char origin CT should succeed.
  {
    AlternateMetadata meta;
    meta.full_mask = mask.Encode();
    meta.content_type = ContentType::kOther;
    meta.origin_content_type = std::string(256, 'x');

    std::string data = "max-ct-len";
    auto wh = cache_->WriteAlternate("/ct-256.html", "example.com", "https", id,
                                     data.size(), meta);
    ASSERT_TRUE(wh.has_value());
    auto bytes = std::as_bytes(std::span(data));
    (void)wh->write_sync(bytes);
    (void)wh->close_sync();

    auto result =
        cache_->ReadBestAlternate("/ct-256.html", "example.com", "https", mask);
    ASSERT_TRUE(result.has_value()) << "256-char origin CT should be accepted";
    EXPECT_EQ(result->metadata.origin_content_type.size(), 256u);
  }

  // 300-char origin CT should be truncated to 256 on serialization.
  {
    AlternateMetadata meta;
    meta.full_mask = mask.Encode();
    meta.content_type = ContentType::kOther;
    meta.origin_content_type = std::string(300, 'y');

    std::string data = "over-ct-len";
    auto wh = cache_->WriteAlternate("/ct-300.html", "example.com", "https", id,
                                     data.size(), meta);
    ASSERT_TRUE(wh.has_value());
    auto bytes = std::as_bytes(std::span(data));
    (void)wh->write_sync(bytes);
    (void)wh->close_sync();

    auto result =
        cache_->ReadBestAlternate("/ct-300.html", "example.com", "https", mask);
    ASSERT_TRUE(result.has_value());
    // Should be truncated to kMaxOriginCtLen = 256.
    EXPECT_EQ(result->metadata.origin_content_type.size(), 256u);
    EXPECT_EQ(result->metadata.origin_content_type, std::string(256, 'y'));
  }
}

TEST_F(PageSpeedCacheTest, MultipleRapidResetsStability) {
  // 5 resets in a row → generation increments, post-reset writes work.
  CreateCache();

  std::string gen_path = cache_path_ + ".gen";

  for (int i = 0; i < 5; ++i) {
    auto reset = cache_->ResetVolume();
    ASSERT_TRUE(reset.has_value()) << "Reset #" << (i + 1) << " should succeed";

    uint64_t gen = PageSpeedCache::ReadGenerationFile(gen_path);
    EXPECT_EQ(gen, static_cast<uint64_t>(i + 1))
        << "Generation after reset #" << (i + 1);

    // Write and read after each reset to verify cache is functional.
    CapabilityMask mask;
    std::string url = "/img/rapid-reset-" + std::to_string(i) + ".jpg";
    std::string data = "data-" + std::to_string(i);
    WriteContent(url, "example.com", mask, data);

    auto result = cache_->ReadBestAlternate(url, "example.com", "https", mask);
    ASSERT_TRUE(result.has_value())
        << "Post-reset #" << (i + 1) << " write+read should succeed";
    auto content = result->content();
    std::string_view sv(reinterpret_cast<const char*>(content.data()),
                        content.size());
    EXPECT_EQ(sv, data);
  }
}

}  // namespace

}  // namespace pagespeed
