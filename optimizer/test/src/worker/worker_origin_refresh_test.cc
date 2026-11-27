// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

// Origin refresh and headers sidecar tests
//
// - Headers sidecar flag bits on written variants
// - The origin-refresh sentinel: restamp, defer, purge and rebuild
//
// One of the topic files of //test/src/worker:worker_test; the shared fixture
// lives in worker_test_fixture.h.

// clang-format off
// Keep the fixture header first: it preserves the old worker_test.cc include
// order (worker.h before any Windows header), which Windows builds have proven.
// See lib/base/message_handler.h for the FormatMessage macro interaction.
#include "test/src/worker/worker_test_fixture.h"
// clang-format on

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <optional>
#include <span>
#include <string>
#include <thread>
#include <vector>

#include "avif/avif.h"
#include "gtest/gtest.h"
#include "lib/base/message_handler.h"
#include "lib/cache/cache.h"
#include "lib/classify/alternate_id.h"
#include "lib/classify/alternate_metadata.h"
#include "lib/classify/capability_mask.h"
#include "src/proto/worker_ipc.h"
#include "src/worker/browser_analysis_manager.h"
#include "src/worker/critical_css_extractor.h"
#include "src/worker/url_registry.h"
#include "src/worker/worker.h"
#include "test/test_util/worker_quiescence.h"

namespace pagespeed {
namespace {

// ---- Headers sidecar: the worker carries the record-time bits
// (kFlagHeadersClassified, kFlagHeadersSidecarRequired,
// kFlagAnonymousRecorded) from the entry it optimizes into every variant it
// builds, and keeps the URL's headers sidecar when it keeps the identity on
// an origin refresh.

namespace {
constexpr uint8_t kHeaderBits = AlternateMetadata::kFlagHeadersClassified |
                                AlternateMetadata::kFlagHeadersSidecarRequired |
                                AlternateMetadata::kFlagAnonymousRecorded;
}  // namespace

TEST_F(WorkerTest, CssVariantsCarryTheHeadersSidecarFlagBits) {
  WorkerConfig config;
  config.socket_path = socket_path_;
  config.cache_path = cache_path_;
  config.cache_size_bytes = static_cast<uint64_t>(10 * 1024 * 1024);

  NullMessageHandler handler;
  Worker worker(config, &handler);
  ASSERT_TRUE(worker.Initialize());

  const std::string css =
      "body  {  margin:  0;  padding:  0;  }\n"
      "h1  {  color:  red;  font-size:  24px;  }\n";
  // Both bits, only the classified bit, and neither (an entry recorded
  // before the bits existed): each must come out exactly as it went in.
  const std::vector<std::pair<std::string, uint8_t>> cases = {
      {"http://example.com/sidecar-both.css", kHeaderBits},
      {"http://example.com/sidecar-classified.css",
       AlternateMetadata::kFlagHeadersClassified},
      {"http://example.com/sidecar-none.css", 0}};
  for (const auto& c : cases) {
    CacheOriginal(worker.cache(), c.first, css, "", c.second);
  }

  std::thread worker_thread([&worker]() { worker.Run(); });
  WorkerStopper stopper(worker, worker_thread);
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  CapabilityMask gz_mask;
  gz_mask.set_transfer_encoding(CapabilityMask::TransferEncoding::kGzip);
  CapabilityMask br_mask;
  br_mask.set_transfer_encoding(CapabilityMask::TransferEncoding::kBrotli);

  uint64_t expected_processed = 0;
  for (const auto& c : cases) {
    SCOPED_TRACE(c.first);
    CacheNotification notification;
    notification.url = c.first;
    notification.scheme = "https";
    notification.content_type = ContentType::kCss;
    notification.capability_mask = CapabilityMask().Encode();
    SendNotification(notification);
    ++expected_processed;
    for (int i = 0; i < 80 * kSanitizerBudgetScale &&
                    worker.stats().css_processed.load() < expected_processed;
         ++i) {
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    ASSERT_GE(worker.stats().css_processed.load(), expected_processed);
    WaitForWorkerIdle(worker);

    for (const CapabilityMask& mask : {CapabilityMask(), gz_mask, br_mask}) {
      auto v = ReadVariantWithMeta(worker.cache(), c.first, mask);
      ASSERT_TRUE(v.has_value()) << "variant missing";
      EXPECT_NE(v->metadata.flags & AlternateMetadata::kFlagWorkerProcessed, 0);
      EXPECT_EQ(v->metadata.flags & kHeaderBits, c.second);
    }
  }
}

TEST_F(WorkerTest, HtmlVariantCarriesTheHeadersSidecarFlagBits) {
  WorkerConfig config;
  config.socket_path = socket_path_;
  config.cache_path = cache_path_;
  config.cache_size_bytes = static_cast<uint64_t>(10 * 1024 * 1024);

  NullMessageHandler handler;
  config.unsafe_force_async_css =
      true;  // fixture page is not under test for the gate
  Worker worker(config, &handler);
  ASSERT_TRUE(worker.Initialize());

  const std::string html =
      "<html><head><style>body { margin: 0; } "
      "h1 { color: red; }</style></head>"
      "<body><h1>Hello World</h1></body></html>";
  const std::string with_bits = "/sidecar-bits.html";
  const std::string without_bits = "/sidecar-nobits.html";
  CacheOriginal(worker.cache(), with_bits, html, "", kHeaderBits);
  CacheOriginal(worker.cache(), without_bits, html);

  std::thread worker_thread([&worker]() { worker.Run(); });
  WorkerStopper stopper(worker, worker_thread);
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  for (const auto& path : {with_bits, without_bits}) {
    SCOPED_TRACE(path);
    CacheNotification notification;
    notification.url = "http://example.com" + path;
    notification.scheme = "https";
    notification.content_type = ContentType::kHtml;
    notification.capability_mask = 0x00000001;
    SendNotification(notification);
    WaitForWorkerIdle(worker);

    CapabilityMask mask = CapabilityMask::Decode(0x00000001);
    mask.set_image_format(CapabilityMask::ImageFormat::kOriginal);
    std::optional<VariantWithMeta> v;
    for (int i = 0; i < 40 * kSanitizerBudgetScale; ++i) {
      v = ReadVariantWithMeta(worker.cache(), path, mask);
      if (v.has_value()) break;
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    ASSERT_TRUE(v.has_value()) << "HTML variant not written";
    EXPECT_NE(v->content.find("data-pagespeed-critical"), std::string::npos);
    EXPECT_EQ(v->metadata.flags & kHeaderBits,
              path == with_bits ? kHeaderBits : 0);
  }
}

TEST_F(WorkerTest, ImageVariantsCarryTheHeadersSidecarFlagBits) {
  WorkerConfig config;
  config.socket_path = socket_path_;
  config.cache_path = cache_path_;
  config.cache_size_bytes = static_cast<uint64_t>(10 * 1024 * 1024);
  config.proactive_image_variants = true;
  config.proactive_viewport_variants = true;
  // Under a sanitizer drop the density and Save-Data axes and encode AVIF
  // at its fastest speed: 3 viewport combinations still leave several
  // worker-written alternates to check the sidecar bits on (the assertion
  // wants >= 2; each viewport contributes WebP and AVIF besides the
  // optimized original).  Untrimmed this test ran 762 s under TSan on a
  // slower CI host (issue #436).
  if (pagespeed::test::kSanitizerBuild) {
    config.proactive_density_variants = false;
    config.proactive_savedata_variants = false;
    config.avif_speed = 10;
  }

  NullMessageHandler handler;
  Worker worker(config, &handler);
  ASSERT_TRUE(worker.Initialize());

  std::string jpeg = ReadTestFile("jpeg/quality100.jpg");
  ASSERT_FALSE(jpeg.empty()) << "Test JPEG not found";
  const std::string flagged = "/sidecar-flagged.jpg";
  const std::string plain = "/sidecar-plain.jpg";
  CacheOriginal(worker.cache(), flagged, jpeg, "", kHeaderBits);
  CacheOriginal(worker.cache(), plain, jpeg);

  std::thread worker_thread([&worker]() { worker.Run(); });
  WorkerStopper stopper(worker, worker_thread);
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  CapabilityMask mobile_webp(
      CapabilityMask::ImageFormat::kWebP, CapabilityMask::Viewport::kMobile,
      CapabilityMask::PixelDensity::k1x, CapabilityMask::SaveData::kOff,
      CapabilityMask::TransferEncoding::kIdentity);
  for (const auto& path : {flagged, plain}) {
    SCOPED_TRACE(path);
    CacheNotification notification;
    notification.url = "http://example.com" + path;
    notification.scheme = "https";
    notification.content_type = ContentType::kImage;
    notification.capability_mask = mobile_webp.Encode();
    SendNotification(notification);
    std::optional<VariantWithMeta> v;
    for (int i = 0; i < kViewportSiblingPollIterations; ++i) {
      v = ReadVariantWithMeta(worker.cache(), path, mobile_webp);
      if (v.has_value()) break;
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    ASSERT_TRUE(v.has_value()) << "Mobile/WebP variant not written";
    WaitForWorkerIdle(worker);

    // Every worker-written alternate of the URL (device, format and density
    // variants), not just the one that was asked for.
    auto listed = worker.cache()->ListAlternates(path, "", "https");
    ASSERT_TRUE(listed.has_value());
    int checked = 0;
    for (const auto& alt : *listed) {
      auto id = static_cast<AlternateId>(alt.id);
      if (IsSentinel(id)) continue;
      auto r = worker.cache()->ReadAlternate(path, "", "https", id);
      if (!r.has_value() ||
          (r->metadata.flags & AlternateMetadata::kFlagWorkerProcessed) == 0) {
        continue;
      }
      ++checked;
      EXPECT_EQ(r->metadata.flags & kHeaderBits,
                path == flagged ? kHeaderBits : 0)
          << "alternate id " << static_cast<int>(id);
    }
    EXPECT_GE(checked, 2) << "expected several worker-written variants";
  }
}

TEST_F(WorkerTest, OriginRefreshPurgeKeepsTheHeadersSidecarWithTheIdentity) {
  WorkerConfig config;
  config.socket_path = socket_path_;
  config.cache_path = cache_path_;
  config.cache_size_bytes = static_cast<uint64_t>(10 * 1024 * 1024);

  NullMessageHandler handler;
  Worker worker(config, &handler);
  ASSERT_TRUE(worker.Initialize());

  const std::string url = "http://example.com/sidecar-keep-sidecar.css";
  const std::string path = "/sidecar-keep-sidecar.css";
  CacheOriginal(worker.cache(), url, "body { margin: 0; } h1 { color: blue; }",
                "", kHeaderBits);
  const std::vector<HeaderField> fields = {
      {"X-Content-Type-Options", "nosniff"},
      {"Referrer-Policy", "no-referrer"}};
  auto written = worker.cache()->WriteHeadersSidecar(path, "", "https", fields);
  ASSERT_TRUE(written.has_value());
  ASSERT_TRUE(written->stored);
  ASSERT_TRUE(
      worker.cache()->ReadHeadersSidecar(path, "", "https").has_value());

  std::thread worker_thread([&worker]() { worker.Run(); });
  WorkerStopper stopper(worker, worker_thread);
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  CacheNotification refresh;
  refresh.url = url;
  refresh.scheme = "https";
  refresh.content_type = ContentType::kCss;
  refresh.capability_mask = kOriginRefreshedSentinel;
  SendNotification(refresh);
  bool purged = false;
  for (int i = 0; i < 80 * kSanitizerBudgetScale; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    if (worker.stats().origin_refresh_purges.load() >= 1) {
      purged = true;
      break;
    }
  }
  ASSERT_TRUE(purged) << "the sentinel should have triggered a purge";
  WaitForWorkerIdle(worker);

  EXPECT_TRUE(worker.cache()->ReadHeadersSidecar(path, "", "https").has_value())
      << "the purge must keep the headers record while it keeps the identity";
  // The rebuilt variants still carry the bits: read a copy the worker wrote
  // (the gzip slot), not the identity slot the test seeded itself.
  CapabilityMask gz_mask;
  gz_mask.set_transfer_encoding(CapabilityMask::TransferEncoding::kGzip);
  auto rebuilt = ReadVariantWithMeta(worker.cache(), url, gz_mask);
  ASSERT_TRUE(rebuilt.has_value()) << "gzip copy not rebuilt";
  EXPECT_NE(rebuilt->metadata.flags & AlternateMetadata::kFlagWorkerProcessed,
            0);
  EXPECT_EQ(rebuilt->metadata.flags & kHeaderBits, kHeaderBits);
}

TEST_F(WorkerTest, OriginRefreshPurgeRemovesTheHeadersSidecarWithTheIdentity) {
  // The identity slot holds the worker's own (stale) variant, so the purge
  // does not preserve it; the headers record goes with the rest of the family.
  WorkerConfig config;
  config.socket_path = socket_path_;
  config.cache_path = cache_path_;
  config.cache_size_bytes = static_cast<uint64_t>(10 * 1024 * 1024);

  NullMessageHandler handler;
  Worker worker(config, &handler);
  ASSERT_TRUE(worker.Initialize());

  const std::string url = "http://example.com/sidecar-drop-sidecar.css";
  const std::string path = "/sidecar-drop-sidecar.css";
  CacheOriginal(worker.cache(), url, "body { margin: 0; } h1 { color: blue; }",
                "", kHeaderBits);

  std::thread worker_thread([&worker]() { worker.Run(); });
  WorkerStopper stopper(worker, worker_thread);
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  CacheNotification notification;
  notification.url = url;
  notification.scheme = "https";
  notification.content_type = ContentType::kCss;
  notification.capability_mask = CapabilityMask().Encode();
  SendNotification(notification);
  for (int i = 0; i < 80 * kSanitizerBudgetScale &&
                  worker.stats().css_processed.load() < 1;
       ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
  }
  ASSERT_GE(worker.stats().css_processed.load(), 1u);
  WaitForWorkerIdle(worker);
  auto processed = ReadVariantWithMeta(worker.cache(), url, CapabilityMask());
  ASSERT_TRUE(processed.has_value());
  ASSERT_NE(processed->metadata.flags & AlternateMetadata::kFlagWorkerProcessed,
            0)
      << "the worker-processed variant must sit at the identity slot";

  const std::vector<HeaderField> fields = {
      {"X-Content-Type-Options", "nosniff"}};
  auto written = worker.cache()->WriteHeadersSidecar(path, "", "https", fields);
  ASSERT_TRUE(written.has_value());
  ASSERT_TRUE(written->stored);

  CacheNotification refresh = notification;
  refresh.capability_mask = kOriginRefreshedSentinel;
  SendNotification(refresh);
  bool refused = false;
  for (int i = 0; i < 80 * kSanitizerBudgetScale; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    if (worker.stats().origin_refresh_rebuild_refused.load() >= 1) {
      refused = true;
      break;
    }
  }
  ASSERT_TRUE(refused);
  WaitForWorkerIdle(worker);
  EXPECT_TRUE(ReadVariant(worker.cache(), url, CapabilityMask()).empty());
  EXPECT_FALSE(
      worker.cache()->ReadHeadersSidecar(path, "", "https").has_value())
      << "an identity that is not preserved takes the headers record with it";
}

TEST_F(WorkerTest, OriginRefreshedSentinelPreservesIdentityAndRebuilds) {
  // Issue #652 (fix B convergence + review): when nginx re-fetches expired
  // origin content (stale verdict, no validators), it re-records the fresh
  // body at the identity id and sends the origin-refreshed sentinel.  The
  // worker must purge the URL's stale NON-identity variants, PRESERVE the
  // fresh identity, clear dedup, and rebuild the variant set inline — no
  // second origin fetch, no unoptimized-MISS window.
  WorkerConfig config;
  config.socket_path = socket_path_;
  config.cache_path = cache_path_;
  config.cache_size_bytes = static_cast<uint64_t>(10 * 1024 * 1024);

  NullMessageHandler handler;
  Worker worker(config, &handler);
  ASSERT_TRUE(worker.Initialize());

  std::string url = "http://example.com/origin-refreshed.css";
  std::string css = "body { margin: 0; } h1 { color: blue; }";
  CacheOriginal(worker.cache(), url, css);

  std::thread worker_thread([&worker]() { worker.Run(); });
  WorkerStopper stopper(worker, worker_thread);
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  // Process the URL once so variants exist and dedup is populated.
  CacheNotification notification;
  notification.url = url;
  notification.scheme = "https";
  notification.content_type = ContentType::kCss;
  notification.capability_mask = CapabilityMask().Encode();
  SendNotification(notification);
  bool first_processed = false;
  for (int i = 0; i < 80 * kSanitizerBudgetScale; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    if (worker.stats().css_processed.load() >= 1) {
      first_processed = true;
      break;
    }
  }
  ASSERT_TRUE(first_processed) << "CSS should have been processed once";
  ASSERT_FALSE(ReadVariant(worker.cache(), url, CapabilityMask()).empty());

  // Simulate nginx's freshness-driven re-fetch: the fresh body is
  // re-recorded over the identity alternate BEFORE the sentinel is sent
  // (record_response bypasses the AlternateExists short-circuit when
  // origin_refreshed is set).
  std::string fresh_css = "body { margin: 4px; } h2 { color: red; }";
  CacheOriginal(worker.cache(), url, fresh_css);

  // Send the origin-refreshed sentinel — the worker purges the stale
  // variants, preserves the identity, and rebuilds inline (dedup was
  // cleared, so css_processed increments WITHOUT any further
  // notification).
  uint64_t css_before = worker.stats().css_processed.load();
  CacheNotification refresh = notification;
  refresh.capability_mask = kOriginRefreshedSentinel;
  SendNotification(refresh);
  bool purged = false;
  for (int i = 0; i < 80 * kSanitizerBudgetScale; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    if (worker.stats().origin_refresh_purges.load() >= 1) {
      purged = true;
      break;
    }
  }
  ASSERT_TRUE(purged) << "Sentinel should have triggered a purge";

  bool rebuilt = false;
  for (int i = 0; i < 80; ++i) {
    if (worker.stats().css_processed.load() > css_before) {
      rebuilt = true;
      break;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
  }
  EXPECT_TRUE(rebuilt) << "Worker must rebuild the variant set inline from "
                          "the preserved fresh identity (dedup cleared)";

  // The identity alternate survived the purge (fresh content, possibly
  // re-minified by the rebuild — but never empty, never a MISS).
  EXPECT_FALSE(ReadVariant(worker.cache(), url, CapabilityMask()).empty())
      << "Fresh identity must be preserved across the origin-refreshed "
         "purge — a whole-key Remove here means a second origin fetch and "
         "an unoptimized MISS on every TTL expiry";
}

TEST_F(WorkerTest,
       OriginRefreshedPurgesWorkerProcessedIdentityAndDefersRebuild) {
  // the dedup-heal work (reconciliation): on a substrate that
  // records originals ONLY at the durable-original id, the identity slot
  // holds the worker's OWN variant — stale at refresh time.  The
  // origin-refreshed purge must NOT preserve it, the inline rebuild must be
  // REFUSED (no genuine source, refusal counted), and the following
  // record+notify must rebuild the family from the genuinely fresh durable
  // original — the full convergence sequence.  Fails pre-reconciliation:
  // the stale variant is preserved and the family is "rebuilt" from
  // superseded bytes.
  WorkerConfig config;
  config.socket_path = socket_path_;
  config.cache_path = cache_path_;
  config.cache_size_bytes = static_cast<uint64_t>(10 * 1024 * 1024);

  NullMessageHandler handler;
  Worker worker(config, &handler);
  ASSERT_TRUE(worker.Initialize());

  std::string url = "http://example.com/origin-refresh-stale.css";
  std::string css_v1 = "body { margin: 0; } h1 { color: blue; }";
  CacheOriginal(worker.cache(), url, css_v1);

  std::thread worker_thread([&worker]() { worker.Run(); });
  WorkerStopper stopper(worker, worker_thread);
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  // Process once: the minified variant OVERWRITES the identity slot and
  // carries kFlagWorkerProcessed.  Nothing genuine replaces it — on this
  // substrate the origin's bytes live only at the durable-original id.
  CacheNotification notification;
  notification.url = url;
  notification.scheme = "https";
  notification.content_type = ContentType::kCss;
  notification.capability_mask = CapabilityMask().Encode();
  SendNotification(notification);
  bool first_processed = false;
  for (int i = 0; i < 80 * kSanitizerBudgetScale; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    if (worker.stats().css_processed.load() >= 1) {
      first_processed = true;
      break;
    }
  }
  ASSERT_TRUE(first_processed) << "CSS should have been processed once";
  ASSERT_NE(ReadVariant(worker.cache(), url, CapabilityMask()).find("margin"),
            std::string::npos)
      << "the stale variant must sit at the identity slot for this test";
  // Drain so every compressed-variant write of the first pass is counted
  // before variants_written is snapshotted below.
  WaitForWorkerIdle(worker);

  // The origin changed; the sentinel arrives BEFORE the fresh re-record
  // commits (pre-record ordering), so at sentinel time the identity slot
  // holds only the stale worker-processed variant.
  uint64_t variants_before = worker.stats().variants_written.load();
  CacheNotification refresh = notification;
  refresh.capability_mask = kOriginRefreshedSentinel;
  SendNotification(refresh);
  bool refused = false;
  for (int i = 0; i < 80 * kSanitizerBudgetScale; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    if (worker.stats().origin_refresh_rebuild_refused.load() >= 1) {
      refused = true;
      break;
    }
  }
  EXPECT_TRUE(refused)
      << "inline rebuild from a worker-processed (stale) identity must be "
         "refused and counted";
  EXPECT_EQ(1u, worker.stats().origin_refresh_purges.load())
      << "the purge itself still runs";
  EXPECT_EQ(variants_before, worker.stats().variants_written.load())
      << "no inline rebuild may run without a genuine source";
  EXPECT_TRUE(ReadVariant(worker.cache(), url, CapabilityMask()).empty())
      << "the stale worker-processed identity must be purged, not preserved";
  // Drain the sentinel's work item so the plain notify below is not
  // rejected by the in-flight guard.
  WaitForWorkerIdle(worker);

  // Convergence: the post-commit re-record (fresh durable original) plus
  // its plain notification rebuilds the family from genuinely fresh bytes.
  std::string css_v2 = "body { padding: 0; } h2 { color: green; }";
  CacheDurableOriginal(worker.cache(), url, css_v2, "text/css",
                       ContentType::kCss);
  uint64_t css_before = worker.stats().css_processed.load();
  SendNotification(notification);
  bool rebuilt = false;
  for (int i = 0; i < 80 * kSanitizerBudgetScale; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    if (worker.stats().css_processed.load() > css_before) {
      rebuilt = true;
      break;
    }
  }
  ASSERT_TRUE(rebuilt)
      << "the next record+notify must rebuild (dedup was cleared)";
  std::string rebuilt_variant =
      ReadVariant(worker.cache(), url, CapabilityMask());
  EXPECT_NE(rebuilt_variant.find("padding"), std::string::npos)
      << "rebuilt from the FRESH origin bytes";
  EXPECT_EQ(rebuilt_variant.find("margin"), std::string::npos)
      << "no trace of the superseded bytes may survive the convergence";
}

// Re-points a key's chain head at itself in a CLOSED cache volume, turning the
// alternate chain into a cycle: the shape the storage layer's read and removal
// walks do not guard against, in which a listing repeats one node up to the
// traversal cap, every write to the key is refused, and a per-alternate
// removal reports success without changing anything.  Returns false (with a
// gtest failure already recorded) if the volume file could not be found.
//
// The two constants are the storage layer's on-disk document format, verified
// against the file before anything is written, so a pin that moves them fails
// here rather than silently patching nothing.
static bool SelfLinkChainHeadInClosedVolume(const std::string& dir,
                                            uint64_t head, uint64_t tail) {
  constexpr uint32_t kDocumentMagic = 0x5F129B14;
  constexpr std::streamoff kNextAlternateOffsetPos = 112;
  // Format v9: the link packs the successor's stripe-relative offset in the
  // low 48 bits and the successor's fill serial in the top 16; a document's
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
  std::filesystem::path volume_file;
  for (const auto& entry : std::filesystem::directory_iterator(dir)) {
    if (!entry.is_regular_file()) continue;
    std::fstream probe(entry.path(), std::ios::in | std::ios::binary);
    if (probe.is_open() && magic_at(probe, head) && magic_at(probe, tail)) {
      volume_file = entry.path();
    }
  }
  if (volume_file.empty()) {
    ADD_FAILURE() << "no volume file holds both documents";
    return false;
  }
  std::fstream volume(volume_file,
                      std::ios::in | std::ios::out | std::ios::binary);
  if (!volume.is_open()) {
    ADD_FAILURE() << "cannot open the volume file for patching";
    return false;
  }
  uint64_t link = 0;
  volume.seekg(static_cast<std::streamoff>(head) + kNextAlternateOffsetPos);
  volume.read(reinterpret_cast<char*>(&link), sizeof(link));
  const uint64_t tail_relative = link & kLinkOffsetMask;
  if (!volume.good() || tail_relative == 0 || tail_relative > tail) {
    ADD_FAILURE() << "the head's successor field did not read as expected";
    return false;
  }
  uint32_t tail_fill_serial = 0;
  volume.seekg(static_cast<std::streamoff>(tail) + kFillSerialPos);
  volume.read(reinterpret_cast<char*>(&tail_fill_serial),
              sizeof(tail_fill_serial));
  uint32_t head_fill_serial = 0;
  volume.seekg(static_cast<std::streamoff>(head) + kFillSerialPos);
  volume.read(reinterpret_cast<char*>(&head_fill_serial),
              sizeof(head_fill_serial));
  if (!volume.good() ||
      (link >> kLinkSerialShift) !=
          static_cast<uint64_t>(static_cast<uint16_t>(tail_fill_serial))) {
    ADD_FAILURE() << "the link's serial is not the tail's fill serial";
    return false;
  }
  const uint64_t stripe_base = tail - tail_relative;
  if (stripe_base >= head) {
    ADD_FAILURE() << "the derived stripe base is not below the head";
    return false;
  }
  const uint64_t head_relative =
      (head - stripe_base) |
      (uint64_t{static_cast<uint16_t>(head_fill_serial)} << kLinkSerialShift);
  volume.clear();
  volume.seekp(static_cast<std::streamoff>(head) + kNextAlternateOffsetPos);
  volume.write(reinterpret_cast<const char*>(&head_relative),
               sizeof(head_relative));
  volume.flush();
  return volume.good();
}

TEST_F(WorkerTest, OriginRefreshedSentinelEndsTheLoopOnAnUneditableVariantSet) {
  // Issue #1566.  A URL whose alternate chain has become a cycle is wedged:
  // the front end selects the stale variant, declines it on freshness and asks
  // for the refresh again, the worker's purge cannot unlink anything, and the
  // re-record that would converge the URL is refused — so it is served
  // unoptimized and re-optimized over and over.  After the sentinel the key
  // must be CLEAR and WRITABLE again, so the next request's record + notify
  // converges it.
  //
  // The chain is planted before the worker opens the volume: the state is a
  // property of the stored key, and building it needs the cache closed.
  const std::string url = "http://example.com/refresh-loop.jpg";
  const std::string path = StripSchemeAuthority(url);
  const CapabilityMask webp(
      CapabilityMask::ImageFormat::kWebP, CapabilityMask::Viewport::kDesktop,
      CapabilityMask::PixelDensity::k1x, CapabilityMask::SaveData::kOff,
      CapabilityMask::TransferEncoding::kIdentity);
  const AlternateId webp_id =
      MaskToAlternateId(static_cast<uint8_t>(webp.Encode() & 0xFF));
  const AlternateId identity_id =
      MaskToAlternateId(static_cast<uint8_t>(CapabilityMask().Encode() & 0xFF));
  const uint64_t kVolumeSize = static_cast<uint64_t>(10 * 1024 * 1024);

  uint64_t head = 0;
  uint64_t tail = 0;
  {
    PageSpeedCacheConfig plant_config;
    plant_config.volume_path = cache_path_;
    plant_config.volume_size = kVolumeSize;
    auto planted = PageSpeedCache::Create(plant_config);
    ASSERT_TRUE(planted.has_value());
    auto& plant = **planted;
    for (const auto& [mask, body] :
         std::vector<std::pair<CapabilityMask, std::string>>{
             {CapabilityMask(), "identity-bytes"}, {webp, "stale-webp"}}) {
      AlternateMetadata meta;
      meta.full_mask = mask.Encode();
      meta.content_type = ContentType::kImage;
      meta.flags = AlternateMetadata::kFlagWorkerProcessed;
      meta.origin_content_type = "image/jpeg";
      auto wh = plant.WriteAlternate(
          path, "", "https",
          MaskToAlternateId(static_cast<uint8_t>(mask.Encode() & 0xFF)),
          body.size(), meta);
      ASSERT_TRUE(wh.has_value());
      ASSERT_TRUE(wh->write_sync(std::as_bytes(std::span(body))).has_value());
      ASSERT_TRUE(wh->close_sync().has_value());
    }
    auto two = plant.ListAlternates(path, "", "https");
    ASSERT_TRUE(two.has_value());
    ASSERT_EQ(two->size(), 2u);
    ASSERT_EQ(static_cast<AlternateId>((*two)[0].id), webp_id);
    head = (*two)[0].disk_offset;
    tail = (*two)[1].disk_offset;
  }
  ASSERT_TRUE(SelfLinkChainHeadInClosedVolume(temp_dir_, head, tail));

  WorkerConfig config;
  config.socket_path = socket_path_;
  config.cache_path = cache_path_;
  config.cache_size_bytes = kVolumeSize;

  NullMessageHandler handler;
  Worker worker(config, &handler);
  ASSERT_TRUE(worker.Initialize());

  // The worker opened the planted volume and sees the self-linked head.  At
  // the storage layer this issue was reported against, the listing returned
  // that one node over and over and every re-record was refused.  The
  // current storage layer rejects a link that points at or above its own
  // node, so the listing stops at the bad link: the stale webp head is listed
  // once and the identity variant behind it is unreachable.  Either way no
  // per-alternate removal can unlink the head; only dropping the whole key
  // ends the loop, and that is what the sentinel below must do.
  auto looped = worker.cache()->ListAlternates(path, "", "https");
  ASSERT_TRUE(looped.has_value());
  ASSERT_EQ(looped->size(), 1u);
  ASSERT_EQ(looped->front().disk_offset, head);
  ASSERT_EQ(static_cast<AlternateId>(looped->front().id), webp_id);
  ASSERT_TRUE(worker.cache()->AlternateExists(path, "", "https", webp_id))
      << "the stale head must still be selectable, or this test is not "
         "standing on the shape it describes";

  std::thread worker_thread([&worker]() { worker.Run(); });
  WorkerStopper stopper(worker, worker_thread);
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  CacheNotification refresh;
  refresh.url = url;
  refresh.scheme = "https";
  refresh.content_type = ContentType::kImage;
  refresh.capability_mask = kOriginRefreshedSentinel;
  SendNotification(refresh);
  bool purged = false;
  for (int i = 0; i < 80 * kSanitizerBudgetScale; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    if (worker.stats().origin_refresh_purges.load() >= 1) {
      purged = true;
      break;
    }
  }
  ASSERT_TRUE(purged) << "Sentinel should have triggered a purge";
  WaitForWorkerIdle(worker);

  // Nothing stale is left for the front end to select ...
  EXPECT_FALSE(worker.cache()->AlternateExists(path, "", "https", webp_id))
      << "a stale variant survived the origin-refreshed purge: the next "
         "request selects it, declines it on freshness and asks for the "
         "refresh again — the loop";
  EXPECT_FALSE(worker.cache()->AlternateExists(path, "", "https", identity_id));

  // ... and the key takes the re-record that the fall-through produces, so
  // the ordinary record + notify path can rebuild the URL.
  CacheDurableOriginal(worker.cache(), url, "REFRESHED-ORIGIN", "image/jpeg",
                       ContentType::kImage);
  auto original = worker.cache()->ReadOriginalAlternate(path, "", "https");
  ASSERT_TRUE(original.has_value())
      << "the key must accept writes again after the purge";
}

TEST_F(WorkerTest, OriginRefreshedPreservesGenuineIdentityAndRebuilds) {
  // The nginx shape — no regression from the reconciliation
  //: the front stage re-records the
  // fresh origin body at the identity id BEFORE sending the sentinel, so
  // the entry there has kFlagWorkerProcessed CLEAR.  It must be preserved
  // across the purge and the inline rebuild must proceed from it, with the
  // refusal counter unmoved.
  WorkerConfig config;
  config.socket_path = socket_path_;
  config.cache_path = cache_path_;
  config.cache_size_bytes = static_cast<uint64_t>(10 * 1024 * 1024);

  NullMessageHandler handler;
  Worker worker(config, &handler);
  ASSERT_TRUE(worker.Initialize());

  std::string url = "http://example.com/origin-refresh-genuine.css";
  std::string css_v1 = "body { margin: 0; } h1 { color: blue; }";
  CacheOriginal(worker.cache(), url, css_v1);

  std::thread worker_thread([&worker]() { worker.Run(); });
  WorkerStopper stopper(worker, worker_thread);
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  CacheNotification notification;
  notification.url = url;
  notification.scheme = "https";
  notification.content_type = ContentType::kCss;
  notification.capability_mask = CapabilityMask().Encode();
  SendNotification(notification);
  bool first_processed = false;
  for (int i = 0; i < 80 * kSanitizerBudgetScale; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    if (worker.stats().css_processed.load() >= 1) {
      first_processed = true;
      break;
    }
  }
  ASSERT_TRUE(first_processed) << "CSS should have been processed once";

  // nginx's freshness re-fetch: fresh GENUINE bytes overwrite the identity
  // alternate (flag clear) before the sentinel is sent.
  std::string css_v2 = "body { padding: 0; } h2 { color: green; }";
  CacheOriginal(worker.cache(), url, css_v2);

  uint64_t css_before = worker.stats().css_processed.load();
  CacheNotification refresh = notification;
  refresh.capability_mask = kOriginRefreshedSentinel;
  SendNotification(refresh);
  bool rebuilt = false;
  for (int i = 0; i < 80 * kSanitizerBudgetScale; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    if (worker.stats().css_processed.load() > css_before) {
      rebuilt = true;
      break;
    }
  }
  EXPECT_TRUE(rebuilt)
      << "a genuine re-recorded identity must still drive the inline rebuild";
  EXPECT_EQ(0u, worker.stats().origin_refresh_rebuild_refused.load())
      << "no refusal when the identity is a genuine original";
  std::string rebuilt_variant =
      ReadVariant(worker.cache(), url, CapabilityMask());
  EXPECT_NE(rebuilt_variant.find("padding"), std::string::npos)
      << "the rebuild must run from the fresh genuine identity";
}

TEST_F(WorkerTest, PurgeGenRotationPreservesMonotonicity) {
  // Issue #652 review: after the purge-generation map rotates
  // (> kMaxPurgeGenEntries distinct purged URLs), a key with generation N
  // survives only in the previous map.  A later purge must seed its bump
  // from there (N+1), not restart at 1 — otherwise a baseline of N
  // captured at dispatch time silently bypasses the write fence
  // (`1 > N` == false) until N more purges occur.
  WorkerConfig config;
  config.socket_path = socket_path_;
  config.cache_path = cache_path_;
  config.cache_size_bytes = static_cast<uint64_t>(10 * 1024 * 1024);

  NullMessageHandler handler;
  Worker worker(config, &handler);
  ASSERT_TRUE(worker.Initialize());

  std::string url = "http://example.com/rotation-fence-test";
  std::string hostname = "";

  // Purge to generation 3.
  worker.InvalidateUrl(url, hostname);
  worker.InvalidateUrl(url, hostname);
  worker.InvalidateUrl(url, hostname);

  // Force exactly one rotation: purge kMaxPurgeGenEntries + 1 distinct
  // other URLs (rotation triggers when the current map exceeds the cap),
  // so the test URL's generation now lives only in the previous map.
  for (size_t i = 0; i <= 10000; ++i) {
    worker.InvalidateUrl("http://example.com/rotate/" + std::to_string(i),
                         hostname);
  }

  // The baseline still sees generation 3 via the previous-map fallback.
  PurgeDispatchGen baseline = worker.CapturePurgeGen(url, hostname);
  EXPECT_EQ(baseline.url_gen, 3u)
      << "Rotation must not lose the per-URL generation";
  EXPECT_FALSE(worker.WasPurgedSinceDispatch(url, hostname, baseline));

  // Purge once more: the bump must be seeded from the previous map (-> 4),
  // not restart at 1.
  worker.InvalidateUrl(url, hostname);
  EXPECT_TRUE(worker.WasPurgedSinceDispatch(url, hostname, baseline))
      << "Post-rotation purge must fence a pre-purge baseline (else the "
         "#652 resurrection race is silently re-enabled)";
  EXPECT_EQ(worker.CapturePurgeGen(url, hostname).url_gen, 4u);
}

TEST_F(WorkerTest, OriginRefreshedSentinelRateLimited) {
  // Issue #652: a born-stale upstream (CDN serving Age > max-age) would
  // trigger the sentinel on every request pair.  Per-URL purges must be
  // rate-limited so the worker does not purge + re-optimize in a tight
  // loop.
  WorkerConfig config;
  config.socket_path = socket_path_;
  config.cache_path = cache_path_;
  config.cache_size_bytes = static_cast<uint64_t>(10 * 1024 * 1024);

  NullMessageHandler handler;
  Worker worker(config, &handler);
  ASSERT_TRUE(worker.Initialize());

  std::string url = "http://example.com/origin-refreshed-ratelimit.css";
  CacheOriginal(worker.cache(), url, "body { margin: 0; }");

  std::thread worker_thread([&worker]() { worker.Run(); });
  WorkerStopper stopper(worker, worker_thread);
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  CacheNotification refresh;
  refresh.url = url;
  refresh.scheme = "https";
  refresh.content_type = ContentType::kCss;
  refresh.capability_mask = kOriginRefreshedSentinel;

  SendNotification(refresh);
  bool purged = false;
  for (int i = 0; i < 80 * kSanitizerBudgetScale; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    if (worker.stats().origin_refresh_purges.load() >= 1) {
      purged = true;
      break;
    }
  }
  ASSERT_TRUE(purged);

  // origin_refresh_purges is bumped mid-handler, well before the sentinel's
  // in-flight entry is erased (the handler still rebuilds variants inline
  // after it).  Drain, or the second sentinel is rejected by the in-flight
  // guard and never reaches the rate-limit check.
  WaitForWorkerIdle(worker);

  // A second sentinel within the rate-limit window is dropped.
  SendNotification(refresh);
  bool rate_limited = false;
  for (int i = 0; i < 80 * kSanitizerBudgetScale; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    if (worker.stats().origin_refresh_rate_limited.load() >= 1) {
      rate_limited = true;
      break;
    }
  }
  EXPECT_TRUE(rate_limited)
      << "Second origin-refreshed sentinel within the interval must be "
         "rate-limited";
  EXPECT_EQ(worker.stats().origin_refresh_purges.load(), 1u);
}

TEST_F(WorkerTest, OriginRefreshedUnchangedImageRestampsWithoutPurge) {
  // Issue #1503 regression (nginx shape): an age-expired origin that
  // re-fetches BYTE-IDENTICAL must NOT purge the variant set — the worker
  // restamps the variants from the freshly re-recorded origin reference and
  // no re-optimization runs.  Pre-fix this purged every freshness cycle
  // (513 purges of unchanged origins observed in production).
  WorkerConfig config;
  config.socket_path = socket_path_;
  config.cache_path = cache_path_;
  config.cache_size_bytes = static_cast<uint64_t>(10 * 1024 * 1024);

  NullMessageHandler handler;
  Worker worker(config, &handler);
  ASSERT_TRUE(worker.Initialize());

  std::string url = "http://example.com/refresh-unchanged.jpg";
  std::string jpeg = ReadTestFile("jpeg/quality100.jpg");
  ASSERT_FALSE(jpeg.empty());
  CacheOriginal(worker.cache(), url, jpeg);

  std::thread worker_thread([&worker]() { worker.Run(); });
  WorkerStopper stopper(worker, worker_thread);
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  CacheNotification notification;
  notification.url = url;
  notification.scheme = "https";
  notification.content_type = ContentType::kImage;
  notification.capability_mask = CapabilityMask().Encode();
  SendNotification(notification);
  ASSERT_NO_FATAL_FAILURE(WaitForNotificationRetiredWithVariant(worker));

  // The first pass wrote the variant set, stamped the kContentHash oracle
  // over the raw origin, and (typically) poisoned the identity slot with
  // the worker's own recompressed variant.
  CapabilityMask webp_mask(
      CapabilityMask::ImageFormat::kWebP, CapabilityMask::Viewport::kDesktop,
      CapabilityMask::PixelDensity::k1x, CapabilityMask::SaveData::kOff,
      CapabilityMask::TransferEncoding::kIdentity);
  auto before = ReadVariantWithMeta(worker.cache(), url, webp_mask);
  ASSERT_TRUE(before.has_value()) << "webp variant from the first pass";

  // nginx's freshness re-fetch, origin UNCHANGED: the identical body is
  // re-recorded over the identity slot (flag-clear, freshly stamped) BEFORE
  // the sentinel is sent.
  AlternateMetadata fresh_meta;
  fresh_meta.content_type = ContentType::kImage;
  fresh_meta.origin_content_type = "image/jpeg";
  fresh_meta.cache_inserted_at = static_cast<uint32_t>(
      std::chrono::duration_cast<std::chrono::seconds>(
          std::chrono::system_clock::now().time_since_epoch())
          .count());
  fresh_meta.origin_max_age = 300;
  fresh_meta.origin_cc_flags = AlternateMetadata::kCCOriginHeaderPresent |
                               AlternateMetadata::kCCOriginPublic;
  CacheOriginalWithMeta(worker.cache(), url, jpeg, fresh_meta);

  const uint64_t images_before = worker.stats().images_processed.load();
  CacheNotification refresh = notification;
  refresh.capability_mask = kOriginRefreshedSentinel;
  SendNotification(refresh);
  bool unchanged_seen = false;
  for (int i = 0; i < 80 * kSanitizerBudgetScale; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    if (worker.stats().origin_refresh_unchanged.load() >= 1) {
      unchanged_seen = true;
      break;
    }
  }
  ASSERT_TRUE(unchanged_seen)
      << "unchanged origin across a freshness boundary must be recognised";
  WaitForWorkerIdle(worker);
  EXPECT_EQ(worker.stats().origin_refresh_purges.load(), 0u)
      << "an unchanged origin must NOT be purged";
  EXPECT_EQ(worker.stats().origin_refresh_deferred.load(), 0u)
      << "a fresh reference resolves in the sentinel handler itself";
  EXPECT_EQ(worker.stats().images_processed.load(), images_before)
      << "no re-transcode of identical bytes";

  // The variant set survives, byte-identical, with its freshness stamp
  // adopted from the re-fetched origin reference.
  auto after = ReadVariantWithMeta(worker.cache(), url, webp_mask);
  ASSERT_TRUE(after.has_value()) << "variant set must survive (no purge)";
  EXPECT_EQ(after->content, before->content);
  EXPECT_EQ(after->metadata.cache_inserted_at, fresh_meta.cache_inserted_at)
      << "variant freshness restamped from the re-fetched origin";
}

TEST_F(WorkerTest,
       OriginRefreshedBeginsVaryingByAcceptPurgesInsteadOfRestamping) {
  // An origin that was recorded without `Vary: Accept` and optimized, then
  // starts negotiating on Accept with BYTE-IDENTICAL content.  The content
  // hash matches, but the Vary set changed: the variant set is not restamped
  // in place, it is purged, and (the marked origin never being optimized)
  // the URL then serves the origin's own representation.
  WorkerConfig config;
  config.socket_path = socket_path_;
  config.cache_path = cache_path_;
  config.cache_size_bytes = static_cast<uint64_t>(10 * 1024 * 1024);

  NullMessageHandler handler;
  Worker worker(config, &handler);
  ASSERT_TRUE(worker.Initialize());

  std::string url = "http://example.com/refresh-begins-varying.jpg";
  std::string jpeg = ReadTestFile("jpeg/quality100.jpg");
  ASSERT_FALSE(jpeg.empty());
  CacheOriginal(worker.cache(), url, jpeg);

  std::thread worker_thread([&worker]() { worker.Run(); });
  WorkerStopper stopper(worker, worker_thread);
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  CacheNotification notification;
  notification.url = url;
  notification.scheme = "https";
  notification.content_type = ContentType::kImage;
  notification.capability_mask = CapabilityMask().Encode();
  SendNotification(notification);
  ASSERT_NO_FATAL_FAILURE(WaitForNotificationRetiredWithVariant(worker));

  CapabilityMask webp_mask(
      CapabilityMask::ImageFormat::kWebP, CapabilityMask::Viewport::kDesktop,
      CapabilityMask::PixelDensity::k1x, CapabilityMask::SaveData::kOff,
      CapabilityMask::TransferEncoding::kIdentity);
  ASSERT_TRUE(ReadVariantWithMeta(worker.cache(), url, webp_mask).has_value())
      << "webp variant from the first pass";

  // The freshness re-fetch re-records the identical body, now marked.
  AlternateMetadata fresh_meta;
  fresh_meta.content_type = ContentType::kImage;
  fresh_meta.origin_content_type = "image/jpeg";
  fresh_meta.flags = AlternateMetadata::kFlagOriginVariesAccept;
  fresh_meta.cache_inserted_at = static_cast<uint32_t>(
      std::chrono::duration_cast<std::chrono::seconds>(
          std::chrono::system_clock::now().time_since_epoch())
          .count());
  fresh_meta.origin_max_age = 300;
  fresh_meta.origin_cc_flags = AlternateMetadata::kCCOriginHeaderPresent |
                               AlternateMetadata::kCCOriginPublic;
  CacheOriginalWithMeta(worker.cache(), url, jpeg, fresh_meta);

  CacheNotification refresh = notification;
  refresh.capability_mask = kOriginRefreshedSentinel;
  SendNotification(refresh);
  bool purged = false;
  for (int i = 0; i < 80 * kSanitizerBudgetScale; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    if (worker.stats().origin_refresh_purges.load() >= 1) {
      purged = true;
      break;
    }
  }
  ASSERT_TRUE(purged) << "a flipped Vary set must purge the variant set";
  WaitForWorkerIdle(worker);
  EXPECT_EQ(worker.stats().origin_refresh_unchanged.load(), 0u)
      << "must not be treated as an unchanged origin";
  EXPECT_FALSE(ReadVariantWithMeta(worker.cache(), url, webp_mask).has_value())
      << "the pre-existing variant must not survive the flip";
}

TEST_F(WorkerTest, UnchangedRefreshRestampCarriesTheHeadersSidecarFlagBits) {
  // Variants built before the bits existed are restamped (not rebuilt) when
  // the origin is unchanged across a freshness boundary; the restamp must
  // adopt the bits from the freshly re-recorded reference, or the variant set
  // would stay unflagged until a purge.
  WorkerConfig config;
  config.socket_path = socket_path_;
  config.cache_path = cache_path_;
  config.cache_size_bytes = static_cast<uint64_t>(10 * 1024 * 1024);

  NullMessageHandler handler;
  Worker worker(config, &handler);
  ASSERT_TRUE(worker.Initialize());

  std::string url = "http://example.com/sidecar-restamp.jpg";
  std::string jpeg = ReadTestFile("jpeg/quality100.jpg");
  ASSERT_FALSE(jpeg.empty());
  CacheOriginal(worker.cache(), url, jpeg);  // unflagged source

  std::thread worker_thread([&worker]() { worker.Run(); });
  WorkerStopper stopper(worker, worker_thread);
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  CacheNotification notification;
  notification.url = url;
  notification.scheme = "https";
  notification.content_type = ContentType::kImage;
  notification.capability_mask = CapabilityMask().Encode();
  SendNotification(notification);
  ASSERT_NO_FATAL_FAILURE(WaitForNotificationRetiredWithVariant(worker));

  CapabilityMask webp_mask(
      CapabilityMask::ImageFormat::kWebP, CapabilityMask::Viewport::kDesktop,
      CapabilityMask::PixelDensity::k1x, CapabilityMask::SaveData::kOff,
      CapabilityMask::TransferEncoding::kIdentity);
  auto before = ReadVariantWithMeta(worker.cache(), url, webp_mask);
  ASSERT_TRUE(before.has_value()) << "webp variant from the first pass";
  ASSERT_EQ(before->metadata.flags & kHeaderBits, 0)
      << "the old variant must start unflagged";

  // The same bytes, re-recorded by a gate-aware front end: both bits set.
  AlternateMetadata fresh_meta;
  fresh_meta.content_type = ContentType::kImage;
  fresh_meta.origin_content_type = "image/jpeg";
  fresh_meta.cache_inserted_at = static_cast<uint32_t>(
      std::chrono::duration_cast<std::chrono::seconds>(
          std::chrono::system_clock::now().time_since_epoch())
          .count());
  fresh_meta.origin_max_age = 300;
  fresh_meta.origin_cc_flags = AlternateMetadata::kCCOriginHeaderPresent |
                               AlternateMetadata::kCCOriginPublic;
  fresh_meta.flags = kHeaderBits;
  CacheOriginalWithMeta(worker.cache(), url, jpeg, fresh_meta);

  CacheNotification refresh = notification;
  refresh.capability_mask = kOriginRefreshedSentinel;
  SendNotification(refresh);
  bool unchanged_seen = false;
  for (int i = 0; i < 80 * kSanitizerBudgetScale; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    if (worker.stats().origin_refresh_unchanged.load() >= 1) {
      unchanged_seen = true;
      break;
    }
  }
  ASSERT_TRUE(unchanged_seen) << "the test must take the restamp path";
  WaitForWorkerIdle(worker);
  EXPECT_EQ(worker.stats().origin_refresh_purges.load(), 0u);

  auto after = ReadVariantWithMeta(worker.cache(), url, webp_mask);
  ASSERT_TRUE(after.has_value()) << "variant set must survive (no purge)";
  EXPECT_EQ(after->content, before->content);
  EXPECT_EQ(after->metadata.flags & kHeaderBits, kHeaderBits)
      << "the restamp must adopt the bits from the re-recorded reference";
}

TEST_F(WorkerTest, OriginRefreshedAgeBackdatedReferenceRestampsWithoutDefer) {
  // Issue #1503 review (must-fix): nginx Age-adjusts cache_inserted_at on
  // the origin-refreshed re-record — a CDN-fronted origin re-records with a
  // stamp minutes BEHIND wall clock.  A wall-clock-only freshness test
  // takes the defer branch for such a reference, but on the nginx shape NO
  // notify follows the sentinel, so the URL would re-fetch on every
  // request and never serve optimized.  The reference must be recognised
  // as the re-record RELATIVELY: it postdates the variant set's own
  // stamps.  (Fails pre-fix: the unconditional purge fires instead.)
  WorkerConfig config;
  config.socket_path = socket_path_;
  config.cache_path = cache_path_;
  config.cache_size_bytes = static_cast<uint64_t>(10 * 1024 * 1024);

  NullMessageHandler handler;
  Worker worker(config, &handler);
  ASSERT_TRUE(worker.Initialize());

  std::string url = "http://example.com/refresh-age-backdated.jpg";
  std::string jpeg = ReadTestFile("jpeg/quality100.jpg");
  ASSERT_FALSE(jpeg.empty());
  CacheOriginal(worker.cache(), url, jpeg);

  std::thread worker_thread([&worker]() { worker.Run(); });
  WorkerStopper stopper(worker, worker_thread);
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  CacheNotification notification;
  notification.url = url;
  notification.scheme = "https";
  notification.content_type = ContentType::kImage;
  notification.capability_mask = CapabilityMask().Encode();
  SendNotification(notification);
  ASSERT_NO_FATAL_FAILURE(WaitForNotificationRetiredWithVariant(worker));

  CapabilityMask webp_mask(
      CapabilityMask::ImageFormat::kWebP, CapabilityMask::Viewport::kDesktop,
      CapabilityMask::PixelDensity::k1x, CapabilityMask::SaveData::kOff,
      CapabilityMask::TransferEncoding::kIdentity);
  auto before = ReadVariantWithMeta(worker.cache(), url, webp_mask);
  ASSERT_TRUE(before.has_value()) << "webp variant from the first pass";

  // nginx's freshness re-fetch of an UNCHANGED, CDN-fronted origin: the
  // identical body re-recorded over the identity slot, flag-clear, with
  // cache_inserted_at Age-backdated 300s behind wall clock.
  const uint32_t now_sec = static_cast<uint32_t>(
      std::chrono::duration_cast<std::chrono::seconds>(
          std::chrono::system_clock::now().time_since_epoch())
          .count());
  AlternateMetadata backdated_meta;
  backdated_meta.content_type = ContentType::kImage;
  backdated_meta.origin_content_type = "image/jpeg";
  backdated_meta.cache_inserted_at = now_sec - 300;
  backdated_meta.origin_max_age = 300;
  backdated_meta.origin_cc_flags = AlternateMetadata::kCCOriginHeaderPresent |
                                   AlternateMetadata::kCCOriginPublic;
  CacheOriginalWithMeta(worker.cache(), url, jpeg, backdated_meta);

  CacheNotification refresh = notification;
  refresh.capability_mask = kOriginRefreshedSentinel;
  SendNotification(refresh);
  bool unchanged_seen = false;
  for (int i = 0; i < 80 * kSanitizerBudgetScale; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    if (worker.stats().origin_refresh_unchanged.load() >= 1) {
      unchanged_seen = true;
      break;
    }
  }
  ASSERT_TRUE(unchanged_seen)
      << "an Age-backdated re-record is still the re-record: restamp, "
         "not defer and not purge";
  WaitForWorkerIdle(worker);
  EXPECT_EQ(worker.stats().origin_refresh_deferred.load(), 0u)
      << "on the nginx shape no notify follows — deferring here is a "
         "permanent re-fetch loop";
  EXPECT_EQ(worker.stats().origin_refresh_purges.load(), 0u);
  auto after = ReadVariantWithMeta(worker.cache(), url, webp_mask);
  ASSERT_TRUE(after.has_value());
  EXPECT_EQ(after->content, before->content);
  EXPECT_EQ(after->metadata.cache_inserted_at, now_sec - 300)
      << "variant freshness adopted the Age-backdated re-record's stamp";
}

TEST_F(WorkerTest, OriginRefreshedUnchangedImageDefersThenRestampsOnNotify) {
  // Issue #1503 regression (1.16/module shape): the sender records originals
  // ONLY at the durable-original id and sends the sentinel BEFORE the
  // re-record lands.  At sentinel time the durable original still holds the
  // pre-refresh bytes: hash-equal to the oracle, so the handler must DEFER
  // (no purge — the identity slot holds the worker's own variant, which was
  // the false-purge driver) and clear dedup; the record+notify that follows
  // then restamps the variant set from the fresh durable original instead of
  // re-transcoding identical bytes.
  WorkerConfig config;
  config.socket_path = socket_path_;
  config.cache_path = cache_path_;
  config.cache_size_bytes = static_cast<uint64_t>(10 * 1024 * 1024);

  NullMessageHandler handler;
  Worker worker(config, &handler);
  ASSERT_TRUE(worker.Initialize());

  std::string url = "http://example.com/refresh-deferred.jpg";
  std::string jpeg = ReadTestFile("jpeg/quality100.jpg");
  ASSERT_FALSE(jpeg.empty());
  // Durable-original-only substrate, stamped long enough ago to be past its
  // origin lifetime (the age-expired fall-through that triggers the refresh).
  CacheDurableOriginal(worker.cache(), url, jpeg, "image/jpeg",
                       ContentType::kImage,
                       /*cache_inserted_at=*/1'700'000'000,
                       /*origin_max_age=*/300);

  std::thread worker_thread([&worker]() { worker.Run(); });
  WorkerStopper stopper(worker, worker_thread);
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  CacheNotification notification;
  notification.url = url;
  notification.scheme = "https";
  notification.content_type = ContentType::kImage;
  notification.capability_mask = CapabilityMask().Encode();
  SendNotification(notification);
  ASSERT_NO_FATAL_FAILURE(WaitForNotificationRetiredWithVariant(worker));

  CapabilityMask webp_mask(
      CapabilityMask::ImageFormat::kWebP, CapabilityMask::Viewport::kDesktop,
      CapabilityMask::PixelDensity::k1x, CapabilityMask::SaveData::kOff,
      CapabilityMask::TransferEncoding::kIdentity);
  auto before = ReadVariantWithMeta(worker.cache(), url, webp_mask);
  ASSERT_TRUE(before.has_value()) << "webp variant from the first pass";
  EXPECT_EQ(before->metadata.cache_inserted_at, 1'700'000'000u)
      << "variants inherit the durable original's origin state";

  // The sentinel arrives BEFORE the re-record (the 1.16 ordering): the
  // durable original still holds the previous bytes with the old stamp.
  const uint64_t images_before = worker.stats().images_processed.load();
  CacheNotification refresh = notification;
  refresh.capability_mask = kOriginRefreshedSentinel;
  SendNotification(refresh);
  bool deferred = false;
  for (int i = 0; i < 80 * kSanitizerBudgetScale; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    if (worker.stats().origin_refresh_deferred.load() >= 1) {
      deferred = true;
      break;
    }
  }
  ASSERT_TRUE(deferred)
      << "a stale-stamped but hash-matching reference must defer, not purge";
  WaitForWorkerIdle(worker);
  EXPECT_EQ(worker.stats().origin_refresh_purges.load(), 0u);
  ASSERT_TRUE(ReadVariantWithMeta(worker.cache(), url, webp_mask).has_value())
      << "the variant set must survive the deferred refresh";

  // The re-record lands (same bytes, fresh stamp) and its plain notification
  // drives the restamp through the image branch's content-hash check.
  const uint32_t reinserted_at = static_cast<uint32_t>(
      std::chrono::duration_cast<std::chrono::seconds>(
          std::chrono::system_clock::now().time_since_epoch())
          .count());
  CacheDurableOriginal(worker.cache(), url, jpeg, "image/jpeg",
                       ContentType::kImage, reinserted_at, 300);
  const uint64_t hash_hits_before = worker.stats().content_hash_hits.load();
  SendNotification(notification);
  bool restamped = false;
  for (int i = 0; i < 80 * kSanitizerBudgetScale; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    if (worker.stats().content_hash_hits.load() > hash_hits_before) {
      restamped = true;
      break;
    }
  }
  ASSERT_TRUE(restamped)
      << "the record+notify convergence must recognise the unchanged origin";
  WaitForWorkerIdle(worker);
  EXPECT_EQ(worker.stats().origin_refresh_purges.load(), 0u);
  EXPECT_EQ(worker.stats().content_hash_stale.load(), 0u);
  EXPECT_EQ(worker.stats().images_processed.load(), images_before)
      << "no re-transcode of identical bytes";
  auto after = ReadVariantWithMeta(worker.cache(), url, webp_mask);
  ASSERT_TRUE(after.has_value());
  EXPECT_EQ(after->content, before->content);
  EXPECT_EQ(after->metadata.cache_inserted_at, reinserted_at)
      << "variant freshness adopted the re-recorded origin's stamp";
}

TEST_F(WorkerTest, OriginRefreshedDeferredThenMarkedReRecordPurgesNotRestamps) {
  // Module shape (sentinel before the re-record) for an image whose origin
  // begins negotiating on Accept.  The sentinel defers (the durable original
  // still holds the old, hash-equal bytes).  The re-record then lands MARKED
  // and is notified.  The identity slot holds the worker's own unmarked
  // variant, so the guard must read the marker from the durable original;
  // the set built before the flip is purged, never restamped.
  WorkerConfig config;
  config.socket_path = socket_path_;
  config.cache_path = cache_path_;
  config.cache_size_bytes = static_cast<uint64_t>(10 * 1024 * 1024);

  NullMessageHandler handler;
  Worker worker(config, &handler);
  ASSERT_TRUE(worker.Initialize());

  std::string url = "http://example.com/refresh-deferred-marked.jpg";
  std::string jpeg = ReadTestFile("jpeg/quality100.jpg");
  ASSERT_FALSE(jpeg.empty());
  CacheDurableOriginal(worker.cache(), url, jpeg, "image/jpeg",
                       ContentType::kImage,
                       /*cache_inserted_at=*/1'700'000'000,
                       /*origin_max_age=*/300);

  std::thread worker_thread([&worker]() { worker.Run(); });
  WorkerStopper stopper(worker, worker_thread);
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  CacheNotification notification;
  notification.url = url;
  notification.scheme = "https";
  notification.content_type = ContentType::kImage;
  notification.capability_mask = CapabilityMask().Encode();
  SendNotification(notification);
  ASSERT_NO_FATAL_FAILURE(WaitForNotificationRetiredWithVariant(worker));

  CapabilityMask webp_mask(
      CapabilityMask::ImageFormat::kWebP, CapabilityMask::Viewport::kDesktop,
      CapabilityMask::PixelDensity::k1x, CapabilityMask::SaveData::kOff,
      CapabilityMask::TransferEncoding::kIdentity);
  ASSERT_TRUE(ReadVariantWithMeta(worker.cache(), url, webp_mask).has_value())
      << "webp variant from the first pass";
  CapabilityMask identity_mask;
  auto identity = ReadVariantWithMeta(worker.cache(), url, identity_mask);
  ASSERT_TRUE(identity.has_value());
  ASSERT_NE(identity->metadata.flags & AlternateMetadata::kFlagWorkerProcessed,
            0)
      << "the identity slot must hold the worker's own unmarked variant";

  CacheNotification refresh = notification;
  refresh.capability_mask = kOriginRefreshedSentinel;
  SendNotification(refresh);
  bool deferred = false;
  for (int i = 0; i < 80 * kSanitizerBudgetScale; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    if (worker.stats().origin_refresh_deferred.load() >= 1) {
      deferred = true;
      break;
    }
  }
  ASSERT_TRUE(deferred) << "the sentinel must defer before the re-record";
  WaitForWorkerIdle(worker);
  ASSERT_TRUE(ReadVariantWithMeta(worker.cache(), url, webp_mask).has_value())
      << "the deferral itself leaves the set in place";

  // The re-record lands, same bytes, fresh stamp, now marked.
  const uint32_t reinserted_at = static_cast<uint32_t>(
      std::chrono::duration_cast<std::chrono::seconds>(
          std::chrono::system_clock::now().time_since_epoch())
          .count());
  CacheDurableOriginal(worker.cache(), url, jpeg, "image/jpeg",
                       ContentType::kImage, reinserted_at, 300, "",
                       AlternateMetadata::kFlagOriginVariesAccept);
  const uint64_t hash_hits_before = worker.stats().content_hash_hits.load();
  const uint64_t images_before = worker.stats().images_processed.load();
  SendNotification(notification);
  bool purged = false;
  for (int i = 0; i < 80 * kSanitizerBudgetScale; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    if (!ReadVariantWithMeta(worker.cache(), url, webp_mask).has_value()) {
      purged = true;
      break;
    }
  }
  ASSERT_TRUE(purged) << "the pre-flip variant must not survive the marked "
                         "re-record";
  WaitForWorkerIdle(worker);
  EXPECT_EQ(worker.stats().content_hash_hits.load(), hash_hits_before)
      << "the marked re-record must not reach the restamp";
  EXPECT_EQ(worker.stats().images_processed.load(), images_before)
      << "a marked origin is never re-optimized";
  EXPECT_FALSE(ReadVariantWithMeta(worker.cache(), url, webp_mask).has_value());
}

TEST_F(WorkerTest, OriginRefreshedChangedImageStillPurgesAndRebuilds) {
  // Issue #1503 control (1.16/module shape): a GENUINE origin change must
  // still purge and rebuild — via the deferred convergence: the sentinel
  // arrives before the re-record (no evidence of change yet), then the
  // re-recorded durable original carries DIFFERENT bytes and the notify
  // path's pristine-reference hash check fires the purge.  The rebuilt
  // variant set must derive from the NEW origin bytes — never from the
  // worker-processed identity slot (the poisoning this issue is about).
  WorkerConfig config;
  config.socket_path = socket_path_;
  config.cache_path = cache_path_;
  config.cache_size_bytes = static_cast<uint64_t>(10 * 1024 * 1024);

  NullMessageHandler handler;
  Worker worker(config, &handler);
  ASSERT_TRUE(worker.Initialize());

  std::string url = "http://example.com/refresh-changed.jpg";
  std::string jpeg_v1 = ReadTestFile("jpeg/quality100.jpg");
  std::string jpeg_v2 = ReadTestFile("jpeg/progressive.jpg");
  ASSERT_FALSE(jpeg_v1.empty());
  ASSERT_FALSE(jpeg_v2.empty());
  ASSERT_NE(jpeg_v1, jpeg_v2);
  CacheDurableOriginal(worker.cache(), url, jpeg_v1, "image/jpeg",
                       ContentType::kImage,
                       /*cache_inserted_at=*/1'700'000'000,
                       /*origin_max_age=*/300);

  std::thread worker_thread([&worker]() { worker.Run(); });
  WorkerStopper stopper(worker, worker_thread);
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  CacheNotification notification;
  notification.url = url;
  notification.scheme = "https";
  notification.content_type = ContentType::kImage;
  notification.capability_mask = CapabilityMask().Encode();
  SendNotification(notification);
  ASSERT_NO_FATAL_FAILURE(WaitForNotificationRetiredWithVariant(worker));

  CapabilityMask identity_mask;  // Desktop/Original/Identity = 0x08
  auto identity_v1 = ReadVariantWithMeta(worker.cache(), url, identity_mask);
  ASSERT_TRUE(identity_v1.has_value());
  ASSERT_NE(
      identity_v1->metadata.flags & AlternateMetadata::kFlagWorkerProcessed, 0)
      << "the identity slot must hold the worker's own variant here — the "
         "poisoned shape this fix operates on";

  // Sentinel first (pre-record ordering): nothing provably changed yet.
  CacheNotification refresh = notification;
  refresh.capability_mask = kOriginRefreshedSentinel;
  SendNotification(refresh);
  bool deferred = false;
  for (int i = 0; i < 80 * kSanitizerBudgetScale; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    if (worker.stats().origin_refresh_deferred.load() >= 1) {
      deferred = true;
      break;
    }
  }
  ASSERT_TRUE(deferred);
  WaitForWorkerIdle(worker);

  // The origin REALLY changed: the re-record carries different bytes.
  CacheDurableOriginal(
      worker.cache(), url, jpeg_v2, "image/jpeg", ContentType::kImage,
      static_cast<uint32_t>(
          std::chrono::duration_cast<std::chrono::seconds>(
              std::chrono::system_clock::now().time_since_epoch())
              .count()),
      300);
  const uint64_t stale_before = worker.stats().content_hash_stale.load();
  SendNotification(notification);
  bool change_detected = false;
  for (int i = 0; i < 80 * kSanitizerBudgetScale; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    if (worker.stats().content_hash_stale.load() > stale_before) {
      change_detected = true;
      break;
    }
  }
  ASSERT_TRUE(change_detected)
      << "a genuine origin change must fire the content-hash purge";
  WaitForWorkerIdle(worker);

  // The purge is whole-key (issue #652 discipline) and fences this pass's
  // own writes, so the convergence is the production one: the front end's
  // next request misses, re-records the durable original, and re-notifies —
  // and THAT pass rebuilds from the fresh pristine reference.
  CacheDurableOriginal(
      worker.cache(), url, jpeg_v2, "image/jpeg", ContentType::kImage,
      static_cast<uint32_t>(
          std::chrono::duration_cast<std::chrono::seconds>(
              std::chrono::system_clock::now().time_since_epoch())
              .count()),
      300);
  SendNotification(notification);
  WaitForWorkerIdle(worker);
  // Drain until the rebuild lands (a fenced pass neither writes nor marks,
  // so a repeat notify is the retry, exactly as in production).
  for (int i = 0; i < 80 * kSanitizerBudgetScale; ++i) {
    if (ReadVariantWithMeta(worker.cache(), url, identity_mask).has_value()) {
      break;
    }
    SendNotification(notification);
    WaitForWorkerIdle(worker);
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
  }
  auto identity_v2 = ReadVariantWithMeta(worker.cache(), url, identity_mask);
  ASSERT_TRUE(identity_v2.has_value())
      << "the variant set must be rebuilt after a genuine change";
  EXPECT_NE(identity_v2->content, identity_v1->content)
      << "the rebuild must derive from the NEW origin bytes";
  EXPECT_EQ(identity_v2->metadata.origin_content_length,
            static_cast<uint32_t>(jpeg_v2.size()))
      << "the rebuilt variant carries the new origin's length";
}

}  // namespace
}  // namespace pagespeed
