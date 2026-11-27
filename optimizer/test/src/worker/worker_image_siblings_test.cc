// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

// Image sibling variant generation tests
//
// - Viewport, Save-Data and density sibling generation
// - Read leases and the dealias seam
// - Incomplete variant matrices
//
// One of the topic files of //test/src/worker:worker_test; the shared fixture
// lives in worker_test_fixture.h.

// clang-format off
// Keep the fixture header first: it preserves the old worker_test.cc include
// order (worker.h before any Windows header), which Windows builds have proven.
// See lib/base/message_handler.h for the FormatMessage macro interaction.
#include "test/src/worker/worker_test_fixture.h"
// clang-format on

#include <cstdarg>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <thread>
#include <vector>

#include "avif/avif.h"
#include "gtest/gtest.h"
#include "lib/base/message_handler.h"
#include "lib/cache/cache.h"
#include "lib/cache/freshness.h"
#include "lib/classify/alternate_id.h"
#include "lib/classify/alternate_metadata.h"
#include "lib/classify/capability_mask.h"
#include "src/proto/worker_ipc.h"
#include "src/worker/browser_analysis_manager.h"
#include "src/worker/critical_css_extractor.h"
#include "src/worker/worker.h"
#include "test/test_util/pipe_client.h"
#include "test/test_util/tcp_client.h"
#include "test/test_util/temp_dir.h"
#include "test/test_util/worker_quiescence.h"

namespace pagespeed {
namespace {

// =============================================================================
// Proactive Viewport Sibling Generation Tests
// =============================================================================

TEST_F(WorkerTest, ImageNotificationGeneratesViewportSiblings) {
  // When proactive viewport siblings are enabled, a Mobile/WebP
  // notification should also generate Tablet and Desktop variants.
  WorkerConfig config;
  config.socket_path = socket_path_;
  config.cache_path = cache_path_;
  config.cache_size_bytes = static_cast<uint64_t>(10 * 1024 * 1024);
  config.proactive_image_variants = true;
  config.proactive_viewport_variants = true;

  NullMessageHandler handler;
  Worker worker(config, &handler);
  ASSERT_TRUE(worker.Initialize());

  // Pre-populate cache with a real JPEG (large enough to benefit
  // from resize).
  std::string url = "http://example.com/photo.jpg";
  std::string jpeg = ReadTestFile("jpeg/quality100.jpg");
  ASSERT_FALSE(jpeg.empty()) << "Test JPEG not found";
  CacheOriginal(worker.cache(), url, jpeg);

  std::thread worker_thread([&worker]() { worker.Run(); });
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  // Send notification with Mobile/WebP mask.
  CapabilityMask mobile_webp(
      CapabilityMask::ImageFormat::kWebP, CapabilityMask::Viewport::kMobile,
      CapabilityMask::PixelDensity::k1x, CapabilityMask::SaveData::kOff,
      CapabilityMask::TransferEncoding::kIdentity);
  CacheNotification notification;
  notification.url = url;
  notification.scheme = "https";
  notification.content_type = ContentType::kImage;
  notification.capability_mask = mobile_webp.Encode();
  SendNotification(notification);

  // Poll for variants at all viewports.  Uses selector fallback so
  // content-deduped variants (identical bytes at different IDs) are
  // still discoverable.
  auto poll_variant = [&](CapabilityMask::Viewport vp,
                          CapabilityMask::ImageFormat fmt) -> bool {
    CapabilityMask mask(fmt, vp, CapabilityMask::PixelDensity::k1x,
                        CapabilityMask::SaveData::kOff,
                        CapabilityMask::TransferEncoding::kIdentity);
    for (int i = 0; i < kViewportSiblingPollIterations; ++i) {
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
      if (!ReadBestVariant(worker.cache(), url, mask).empty()) return true;
    }
    return false;
  };

  // The directly requested variant (Mobile/WebP) should exist.
  EXPECT_TRUE(poll_variant(CapabilityMask::Viewport::kMobile,
                           CapabilityMask::ImageFormat::kWebP))
      << "Mobile/WebP variant not written";

  // Tablet and Desktop WebP siblings should be servable.  Content-identical
  // copies are only deduplicated within a viewport, so each viewport holds
  // its own (the selector never serves another viewport's copy in place of
  // one at the client's own size).
  EXPECT_TRUE(poll_variant(CapabilityMask::Viewport::kTablet,
                           CapabilityMask::ImageFormat::kWebP))
      << "Tablet/WebP viewport sibling not servable";
  EXPECT_TRUE(poll_variant(CapabilityMask::Viewport::kDesktop,
                           CapabilityMask::ImageFormat::kWebP))
      << "Desktop/WebP viewport sibling not servable";

  // AVIF format siblings at other viewports should also be servable.
  EXPECT_TRUE(poll_variant(CapabilityMask::Viewport::kMobile,
                           CapabilityMask::ImageFormat::kAvif))
      << "Mobile/AVIF sibling not servable";
  EXPECT_TRUE(poll_variant(CapabilityMask::Viewport::kTablet,
                           CapabilityMask::ImageFormat::kAvif))
      << "Tablet/AVIF sibling not servable";

  worker.Shutdown();
  worker_thread.join();
}

TEST_F(WorkerTest, ViewportSiblingsDisabledOnlyGeneratesFormats) {
  // When viewport siblings are disabled, only format siblings at the
  // notification's viewport should be generated.
  WorkerConfig config;
  config.socket_path = socket_path_;
  config.cache_path = cache_path_;
  config.cache_size_bytes = static_cast<uint64_t>(10 * 1024 * 1024);
  config.proactive_image_variants = true;
  config.proactive_viewport_variants = false;

  NullMessageHandler handler;
  Worker worker(config, &handler);
  ASSERT_TRUE(worker.Initialize());

  std::string url = "http://example.com/no-vp.jpg";
  std::string jpeg = ReadTestFile("jpeg/quality100.jpg");
  ASSERT_FALSE(jpeg.empty());
  CacheOriginal(worker.cache(), url, jpeg);

  std::thread worker_thread([&worker]() { worker.Run(); });
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  // Send notification with Mobile/WebP mask.
  CapabilityMask mobile_webp(
      CapabilityMask::ImageFormat::kWebP, CapabilityMask::Viewport::kMobile,
      CapabilityMask::PixelDensity::k1x, CapabilityMask::SaveData::kOff,
      CapabilityMask::TransferEncoding::kIdentity);
  CacheNotification notification;
  notification.url = url;
  notification.scheme = "https";
  notification.content_type = ContentType::kImage;
  notification.capability_mask = mobile_webp.Encode();
  SendNotification(notification);

  // Wait for processing.
  CapabilityMask mobile_avif(
      CapabilityMask::ImageFormat::kAvif, CapabilityMask::Viewport::kMobile,
      CapabilityMask::PixelDensity::k1x, CapabilityMask::SaveData::kOff,
      CapabilityMask::TransferEncoding::kIdentity);
  std::string avif_variant;
  for (int i = 0; i < kViewportSiblingPollIterations; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    avif_variant = ReadVariant(worker.cache(), url, mobile_avif);
    if (!avif_variant.empty()) break;
  }
  EXPECT_FALSE(avif_variant.empty()) << "Mobile/AVIF format sibling missing";

  // Tablet variants should NOT exist (viewport siblings disabled).
  CapabilityMask tablet_webp(
      CapabilityMask::ImageFormat::kWebP, CapabilityMask::Viewport::kTablet,
      CapabilityMask::PixelDensity::k1x, CapabilityMask::SaveData::kOff,
      CapabilityMask::TransferEncoding::kIdentity);
  std::string tablet = ReadVariant(worker.cache(), url, tablet_webp);
  EXPECT_TRUE(tablet.empty()) << "Tablet variant written despite disabled";

  worker.Shutdown();
  worker_thread.join();
}

TEST_F(WorkerTest, CacheFullVariantWritesNotStarvedByOwnLease) {
  // Regression test for issue #934: WriteImageVariants used to renew the
  // ORIGIN borrow's read lease at the top of every transcode iteration,
  // so at a wrap-pinned volume every variant write — wrap-gated on that
  // very stripe's lease — was deferred and dropped: the worker starved
  // its own write phase.  The worker now copies the origin bytes out of
  // the borrow and releases it before transcoding, so only the initial
  // read-time stamp remains, and it decays while the matrix runs.
  //
  // The deterministic pin of the underlying mechanism (renewal defers
  // writes at a full stripe; release + expiry admits them) lives in
  // cache_test ReleaseUnpinsFullStripeForWrites — timing there is fully
  // controlled.  This test guards the end-to-end outcome: with the write
  // cursor parked AT the wrap boundary, the worker's image pipeline must
  // still land a variant before it has worked through its matrix.
  WorkerConfig config;
  config.socket_path = socket_path_;
  config.cache_path = cache_path_;
  config.cache_size_bytes = static_cast<uint64_t>(10 * 1024 * 1024);
  config.ram_cache_size = 0;  // the origin read must be a disk borrow
  // Short: the origin read's single stamp must decay before the matrix
  // finishes (pre-fix, the per-iteration renewals kept it fresh forever).
  config.read_lease_duration_ms = 500;
  // Park the anti-starvation valve out of reach: the default 60s
  // lease_wrap_ceiling force-admits one write per minute, which would
  // mask genuine lease starvation -- a write the ceiling forced through
  // looks exactly like one the decayed lease admitted.  Post-fix nothing
  // keeps the lease alive, so the ceiling is never needed; pre-fix (or with
  // a reintroduced pin) every write of the matrix is deferred and none
  // lands.  A day is far beyond any run of this matrix, on any host.
  config.lease_wrap_ceiling_ms = 24 * 60 * 60 * 1000;
  config.proactive_image_variants = true;
  // Trim the matrix to 6 combinations (viewport x density) to keep the
  // unoptimized-build runtime reasonable; still several transcode
  // iterations' worth of write opportunities after the lease decays.
  config.proactive_savedata_variants = false;
  // Under a sanitizer trim further: 3 viewport combinations, AVIF at its
  // fastest speed, no quality verification.  The property under test is
  // lease decay -- some variant write must land once the origin read's
  // single lease stamp has expired -- and 3 combinations still give several
  // write opportunities after the decay; the codec settings do not enter
  // it.  Untrimmed, the 6-combination encode matrix alone ran 2724 s under
  // TSan on a slower CI host (issue #436) and pushed its shard past the
  // 3600 s budget.
  if (pagespeed::test::kSanitizerBuild) {
    config.avif_speed = 10;
    config.quality_verify = false;
    config.proactive_density_variants = false;  // 3 combinations, not 6
  }

  NullMessageHandler handler;
  Worker worker(config, &handler);
  ASSERT_TRUE(worker.Initialize());

  std::string url = "http://example.com/starve.jpg";
  std::string jpeg = ReadTestFile("jpeg/sjpeg6.jpg");
  ASSERT_FALSE(jpeg.empty()) << "Test JPEG not found";
  CacheOriginal(worker.cache(), url, jpeg);

  // Park the write cursor AT the wrap boundary: hold a renewed borrow on
  // the origin (pinning the single 10MB stripe) and stream filler writes
  // until one is wrap-deferred.  A deferred wrap has no side effects, so
  // the cursor stays parked at the end of the data area — once the lease
  // decays, EVERY subsequent write must first win the lease-gated wrap,
  // which is exactly the cache-full contention from issue #934.
  {
    auto probe = worker.cache()->ReadBestAlternate(
        StripSchemeAuthority(url), "", "https", CapabilityMask());
    ASSERT_TRUE(probe.has_value());
    ASSERT_TRUE(probe->is_valid());
    CapabilityMask fill_mask;  // Desktop/Identity
    AlternateId fill_id =
        MaskToAlternateId(static_cast<uint8_t>(fill_mask.Encode() & 0xFF));
    // Any stage of the write can surface the lease-deferred NoSpace
    // (slot allocation happens when the buffered document is committed).
    auto try_fill = [&](int i, size_t sz) -> bool {
      const std::string filler(sz, 'f');
      AlternateMetadata meta;
      meta.full_mask = fill_mask.Encode();
      meta.content_type = ContentType::kOther;
      auto wh = worker.cache()->WriteAlternate(
          "/fill/" + std::to_string(i) + ".bin", "", "https", fill_id,
          filler.size(), meta);
      if (!wh.has_value()) return false;
      auto written = wh->write_sync(
          std::as_bytes(std::span(filler.data(), filler.size())));
      return written.has_value() && wh->close_sync().has_value();
    };
    // Shrinking ladder: when a write of the current size is deferred,
    // step down and keep filling, so the tail slack ends up smaller than
    // any variant the worker will write.  (A single 512KB probe can park
    // the cursor with almost 512KB of tail space still open — small
    // variant writes would then land without ever needing a wrap.)
    size_t fill_sz = 512 * 1024;
    bool parked = false;
    for (int i = 0; i < 200 && !parked; ++i) {
      ASSERT_TRUE(probe->renew_lease());
      if (!try_fill(i, fill_sz)) {
        if (fill_sz <= 512) {
          parked = true;  // even a sub-KB write needs a (deferred) wrap
        } else {
          fill_sz /= 8;
        }
      }
    }
    ASSERT_TRUE(parked) << "volume never reached the wrap boundary";
    EXPECT_GT(worker.cache()->Stats().writes_dropped_by_lease, 0u);
    probe->release();  // stop pinning; the probe's lease decays below
  }
  // Let the probe's last lease stamp expire so the only lease pressure
  // during processing comes from the worker's own origin read.
  std::this_thread::sleep_for(std::chrono::milliseconds(800));

  std::thread worker_thread([&worker]() { worker.Run(); });
  WorkerStopper stopper(worker, worker_thread);
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  CapabilityMask mobile_webp(
      CapabilityMask::ImageFormat::kWebP, CapabilityMask::Viewport::kMobile,
      CapabilityMask::PixelDensity::k1x, CapabilityMask::SaveData::kOff,
      CapabilityMask::TransferEncoding::kIdentity);
  CacheNotification notification;
  notification.url = url;
  notification.scheme = "https";
  notification.content_type = ContentType::kImage;
  notification.capability_mask = mobile_webp.Encode();
  SendNotification(notification);

  // Watch worker stats only — a cache read here would stamp a fresh lease
  // on the stripe and defer the very writes under test.
  //
  // Wait for the whole matrix to retire, then judge the final counters: by
  // then every combination has had its write attempt.  Pre-fix every write
  // was dropped behind the renewed lease and the counter stayed at zero.  How
  // long the encodes in front of those writes take is the host's business,
  // not this test's: an unoptimized build needs tens of seconds per
  // combination on an idle machine, and a fixed number of seconds for the
  // first write reported starvation whenever the first combination was
  // merely slow.  (Stopping at the first write would save nothing -- the
  // worker's teardown waits for the rest of the matrix anyway.)
  ASSERT_TRUE(pagespeed::test::WaitForNotificationsRetired(worker, 1,
                                                           kWorkerStallBudget));
  EXPECT_GE(worker.stats().variants_written.load(std::memory_order_relaxed), 1u)
      << "no image variant admitted at cache-full: worker starved its own "
         "writes (issue #934); variant write failures: "
      << worker.stats().alternate_write_failures.load(
             std::memory_order_relaxed);
  // Reported, not asserted: post-fix every write lands long after the origin
  // read's single lease stamp expired, so this is expected to be zero; a
  // non-zero value means some writes were still deferred.
  RecordProperty("alternate_write_failures",
                 std::to_string(worker.stats().alternate_write_failures.load(
                     std::memory_order_relaxed)));
}

TEST_F(WorkerTest, ReadLeaseDurationZeroDisablesLeases) {
  // Issue #934 escape hatch: read_lease_duration_ms = 0 must reach the
  // Cyclone cache config — with leases disabled, a disk-hit read has no
  // lease to renew (and, cache-side, wraps are never deferred).
  WorkerConfig config;
  config.socket_path = socket_path_;
  config.cache_path = cache_path_;
  config.cache_size_bytes = static_cast<uint64_t>(10 * 1024 * 1024);
  config.ram_cache_size = 0;  // force a disk borrow (RAM hits never lease)
  config.read_lease_duration_ms = 0;

  NullMessageHandler handler;
  Worker worker(config, &handler);
  ASSERT_TRUE(worker.Initialize());
  ASSERT_NE(worker.cache(), nullptr);

  CacheOriginal(worker.cache(), "/lease-knob.bin", std::string(4096, 'x'));
  {
    auto rr = worker.cache()->ReadBestAlternate("/lease-knob.bin", "", "https",
                                                CapabilityMask());
    ASSERT_TRUE(rr.has_value());
    EXPECT_FALSE(rr->renew_lease());
  }  // close the borrow before Shutdown tears the cache down

  worker.Shutdown();
}

TEST_F(WorkerTest, ReadLeaseDefaultForwardedToCache) {
  // Companion to ReadLeaseDurationZeroDisablesLeases: with the default
  // WorkerConfig (5s, mirroring Cyclone), a disk borrow has a live lease.
  WorkerConfig config;
  config.socket_path = socket_path_;
  config.cache_path = cache_path_;
  config.cache_size_bytes = static_cast<uint64_t>(10 * 1024 * 1024);
  config.ram_cache_size = 0;  // force a disk borrow (RAM hits never lease)

  NullMessageHandler handler;
  Worker worker(config, &handler);
  ASSERT_TRUE(worker.Initialize());
  ASSERT_NE(worker.cache(), nullptr);

  CacheOriginal(worker.cache(), "/lease-knob-on.bin", std::string(4096, 'x'));
  {
    auto rr = worker.cache()->ReadBestAlternate("/lease-knob-on.bin", "",
                                                "https", CapabilityMask());
    ASSERT_TRUE(rr.has_value());
    EXPECT_TRUE(rr->renew_lease());
  }  // close the borrow before Shutdown tears the cache down

  worker.Shutdown();
}

// ---------------------------------------------------------------------------
// Issue #934 de-alias-before-processing: every copy-out site (HTML, CSS, JS,
// image transcode, SVG/unsupported passthrough, warmup) copies the origin
// bytes out of the cache borrow and, BEFORE releasing, proves the copy is
// untorn through the single shared seam Worker::BorrowCopyUntorn.  These
// tests pin that seam's decision table directly (the sanctioned fallback: an
// end-to-end wrap during the worker's microsecond-wide read->copy->verify
// window cannot be forced deterministically, so we drive the seam's inputs at
// the cache boundary).
// ---------------------------------------------------------------------------

TEST_F(WorkerTest, DealiasSeamLiveDiskBorrowVerified) {
  // Disk borrow under a live lease whose epoch never moved: the seam confirms
  // the copy is untorn (renew_lease() == true), so processing proceeds.
  WorkerConfig config;
  config.socket_path = socket_path_;
  config.cache_path = cache_path_;
  config.cache_size_bytes = static_cast<uint64_t>(10 * 1024 * 1024);
  config.ram_cache_size = 0;  // force a disk borrow (RAM hits never lease)

  NullMessageHandler handler;
  Worker worker(config, &handler);
  ASSERT_TRUE(worker.Initialize());
  ASSERT_NE(worker.cache(), nullptr);

  CacheOriginal(worker.cache(), "/dealias-live.bin", std::string(4096, 'x'));
  {
    auto rr = worker.cache()->ReadBestAlternate("/dealias-live.bin", "",
                                                "https", CapabilityMask());
    ASSERT_TRUE(rr.has_value());
    ASSERT_NE(rr->content_file_offset(), ReadResult::kNoFileOffset)
        << "expected a disk borrow, not a RAM hit";
    EXPECT_TRUE(Worker::BorrowCopyUntorn(*rr, /*leases_enabled=*/true));
  }
  worker.Shutdown();
}

TEST_F(WorkerTest, DealiasSeamRamHitNeverDiscarded) {
  // A RAM-cache hit is served from a handle-owned buffer (no mmap alias),
  // so a write-buffer wrap cannot tear it — but renew_lease() legitimately
  // returns false for it (nothing to renew).  The seam keys on
  // is_ram_cache_hit() and MUST NOT mistake that false for a torn read
  // (regression guard: a naive "renew==false -> miss" would drop every RAM
  // hit on the hot path).  Cyclone's CLFUS RAM tier admits an entry on the
  // SECOND put (seen-filter admission), and each disk read of a <=32KB
  // alternate puts — so the third read is the RAM hit.
  WorkerConfig config;
  config.socket_path = socket_path_;
  config.cache_path = cache_path_;
  config.cache_size_bytes = static_cast<uint64_t>(10 * 1024 * 1024);
  config.ram_cache_size = static_cast<size_t>(4 * 1024 * 1024);  // RAM on

  NullMessageHandler handler;
  Worker worker(config, &handler);
  ASSERT_TRUE(worker.Initialize());
  ASSERT_NE(worker.cache(), nullptr);

  CacheOriginal(worker.cache(), "/dealias-ram.bin", std::string(4096, 'r'));
  // Read #1: disk borrow; put #1 only marks the CLFUS seen-filter.
  // Read #2: disk borrow; put #2 admits the entry into the RAM tier.
  for (int i = 0; i < 2; ++i) {
    auto warm = worker.cache()->ReadBestAlternate("/dealias-ram.bin", "",
                                                  "https", CapabilityMask());
    ASSERT_TRUE(warm.has_value());
    EXPECT_FALSE(warm->handle.is_ram_cache_hit());
  }
  {
    // Read #3: served from the RAM tier.
    auto rr = worker.cache()->ReadBestAlternate("/dealias-ram.bin", "", "https",
                                                CapabilityMask());
    ASSERT_TRUE(rr.has_value());
    ASSERT_TRUE(rr->handle.is_ram_cache_hit())
        << "expected the third read to be a RAM-tier hit (CLFUS admission)";
    EXPECT_EQ(rr->content_file_offset(), ReadResult::kNoFileOffset);
    EXPECT_FALSE(rr->renew_lease());  // no lease on a RAM hit
    // ...but the seam still treats the copy as safe (not a miss).
    EXPECT_TRUE(Worker::BorrowCopyUntorn(*rr, /*leases_enabled=*/true));
  }
  worker.Shutdown();
}

TEST_F(WorkerTest, DealiasSeamReleasedHandleTrustedAsVerified) {
  // A released handle (is_valid()==false) means the borrow was already
  // copied + verified + released upstream — this is exactly what
  // WriteTextVariant's belt-and-suspenders verify sees for CSS/JS, whose
  // callers de-alias before minify.  The seam must treat it as safe: there
  // is no alias left to tear, and discarding here would break every
  // already-de-aliased text write.
  WorkerConfig config;
  config.socket_path = socket_path_;
  config.cache_path = cache_path_;
  config.cache_size_bytes = static_cast<uint64_t>(10 * 1024 * 1024);
  config.ram_cache_size = 0;

  NullMessageHandler handler;
  Worker worker(config, &handler);
  ASSERT_TRUE(worker.Initialize());
  ASSERT_NE(worker.cache(), nullptr);

  CacheOriginal(worker.cache(), "/dealias-released.bin",
                std::string(4096, 'm'));
  {
    auto rr = worker.cache()->ReadBestAlternate("/dealias-released.bin", "",
                                                "https", CapabilityMask());
    ASSERT_TRUE(rr.has_value());
    // The upstream contract: copy, verify, THEN release.
    auto span = rr->content();
    const std::string owned(reinterpret_cast<const char*>(span.data()),
                            span.size());
    ASSERT_TRUE(Worker::BorrowCopyUntorn(*rr, /*leases_enabled=*/true));
    rr->release();
    ASSERT_FALSE(rr->is_valid());
    EXPECT_FALSE(rr->renew_lease());  // nothing left to renew
    // A downstream re-verify (WriteTextVariant) must not discard.
    EXPECT_TRUE(Worker::BorrowCopyUntorn(*rr, /*leases_enabled=*/true));
    EXPECT_EQ(owned.front(), 'm');
  }
  worker.Shutdown();
}

TEST_F(WorkerTest, DealiasSeamLeasesDisabledNeverDiscarded) {
  // read_lease_duration_ms == 0 is the pre-lease escape hatch: a disk borrow
  // has no lease, so renew_lease() returns false.  The seam must treat that as
  // "no protection configured, cannot tear" and proceed (not discard) — else
  // the escape-hatch config would drop every disk read.
  WorkerConfig config;
  config.socket_path = socket_path_;
  config.cache_path = cache_path_;
  config.cache_size_bytes = static_cast<uint64_t>(10 * 1024 * 1024);
  config.ram_cache_size = 0;
  config.read_lease_duration_ms = 0;  // leases OFF

  NullMessageHandler handler;
  Worker worker(config, &handler);
  ASSERT_TRUE(worker.Initialize());
  ASSERT_NE(worker.cache(), nullptr);

  CacheOriginal(worker.cache(), "/dealias-nolease.bin", std::string(4096, 'n'));
  {
    auto rr = worker.cache()->ReadBestAlternate("/dealias-nolease.bin", "",
                                                "https", CapabilityMask());
    ASSERT_TRUE(rr.has_value());
    EXPECT_FALSE(rr->renew_lease());  // leases disabled -> nothing to renew
    // leases_enabled=false: the seam short-circuits to "safe" without ever
    // consulting renew_lease().
    EXPECT_TRUE(Worker::BorrowCopyUntorn(*rr, /*leases_enabled=*/false));
  }
  worker.Shutdown();
}

TEST_F(WorkerTest, DealiasSeamForcedWrapTreatedAsMiss) {
  // The load-bearing case: a live-lease disk borrow whose stripe is
  // FORCE-wrapped past the ceiling.  The wrap moves the stripe's epoch, so the
  // borrowed region may have been overwritten in place; renew_lease() reports
  // the move (false) and the seam returns false — every copy-out site
  // (HTML/CSS/JS/image/warmup) then discards the copy and treats the read as a
  // miss instead of persisting torn bytes.  A single 10MB volume is one
  // stripe, so all fills contend on the borrowed stripe.
  WorkerConfig config;
  config.socket_path = socket_path_;
  config.cache_path = cache_path_;
  config.cache_size_bytes = static_cast<uint64_t>(10 * 1024 * 1024);
  config.ram_cache_size = 0;              // disk borrow (RAM hits never lease)
  config.read_lease_duration_ms = 60000;  // long: never decays during the test
  config.lease_wrap_ceiling_ms = 1;       // force wraps almost immediately

  NullMessageHandler handler;
  Worker worker(config, &handler);
  ASSERT_TRUE(worker.Initialize());
  ASSERT_NE(worker.cache(), nullptr);

  const std::string url = "/dealias-wrap.bin";
  CacheOriginal(worker.cache(), url, std::string(64 * 1024, 'v'));

  auto borrow =
      worker.cache()->ReadBestAlternate(url, "", "https", CapabilityMask());
  ASSERT_TRUE(borrow.has_value());
  ASSERT_TRUE(borrow->is_valid());
  ASSERT_NE(borrow->content_file_offset(), ReadResult::kNoFileOffset);

  // Copy the bytes out first, exactly as every de-alias site does.
  auto span = borrow->content();
  const std::string owned(reinterpret_cast<const char*>(span.data()),
                          span.size());
  // Untorn so far: the epoch has not moved.
  EXPECT_TRUE(Worker::BorrowCopyUntorn(*borrow, /*leases_enabled=*/true));

  // Let the 1ms ceiling elapse, then flood writes to force the write cursor to
  // wrap all the way around the single stripe and overwrite the borrowed
  // region (force-admitted past the ceiling despite the live lease).
  std::this_thread::sleep_for(std::chrono::milliseconds(20));
  auto fill = [&](int i) {
    const std::string filler(512 * 1024, 'f');
    CapabilityMask m;
    AlternateId id = MaskToAlternateId(static_cast<uint8_t>(m.Encode() & 0xFF));
    AlternateMetadata meta;
    meta.full_mask = m.Encode();
    meta.content_type = ContentType::kOther;
    auto wh =
        worker.cache()->WriteAlternate("/fill/" + std::to_string(i) + ".bin",
                                       "", "https", id, filler.size(), meta);
    if (!wh.has_value()) return;
    (void)wh->write_sync(
        std::as_bytes(std::span(filler.data(), filler.size())));
    (void)wh->close_sync();
  };
  for (int i = 0; i < 80; ++i) fill(i);
  ASSERT_GT(worker.cache()->Stats().wraps_forced_past_lease, 0u)
      << "the borrowed stripe was never force-wrapped past the ceiling";

  // The epoch moved under the still-held borrow: the seam now reports the copy
  // may be torn, so the caller must treat the read as a miss.
  EXPECT_FALSE(Worker::BorrowCopyUntorn(*borrow, /*leases_enabled=*/true));
  // Our earlier copy captured the pre-wrap bytes and is unaffected.
  EXPECT_EQ(owned.front(), 'v');

  borrow->release();
  worker.Shutdown();
}

TEST_F(WorkerTest, HtmlWrapDuringDealiasCopyPersistsNothing) {
  // The durable-corruption non-persistence guarantee, exercised at the REAL
  // HTML copy-out site: the test hook fires inside the worker's
  // read->copy->verify window and force-wraps the borrowed stripe past the
  // lease ceiling (moving its epoch), so the worker's own BorrowCopyUntorn
  // fails.  The worker must then treat the read as a cache miss: NO variant
  // or sentinel write for the URL, html_processed stays 0, and exactly one
  // read_borrow_wrap_discards is counted.  Pre-fix (aliased processing) the
  // pipeline would have persisted torn transform output with a fresh
  // checksum.
  WorkerConfig config;
  config.socket_path = socket_path_;
  config.cache_path = cache_path_;
  config.cache_size_bytes = static_cast<uint64_t>(10 * 1024 * 1024);
  config.ram_cache_size = 0;              // disk borrow (RAM hits never lease)
  config.read_lease_duration_ms = 60000;  // live for the whole test
  config.lease_wrap_ceiling_ms = 1;       // wraps force almost immediately

  Worker worker(config, nullptr);
  ASSERT_TRUE(worker.Initialize());

  const std::string html =
      "<html><head><style>body { margin: 0; } "
      "h1 { color: red; }</style></head>"
      "<body><h1>Hello World</h1></body></html>";
  const std::string cache_url = "/wrap-during-copy.html";
  CacheOriginal(worker.cache(), cache_url, html);

  // Runs on the worker-pool thread while the worker holds the origin
  // borrow (60s lease, stamped by its read).  Fill the single 10MB stripe
  // to the wrap boundary: the wrap defers on the live lease, and once the
  // 1ms ceiling elapses it is FORCED — moving the epoch under the borrow.
  std::atomic<uint64_t> forced_wraps{0};
  worker.test_hook_after_html_origin_read_ = [&worker, &forced_wraps]() {
    CapabilityMask fill_mask;  // Desktop/Identity
    AlternateId fill_id =
        MaskToAlternateId(static_cast<uint8_t>(fill_mask.Encode() & 0xFF));
    const std::string filler(512 * 1024, 'f');
    for (int i = 0; i < 200; ++i) {
      AlternateMetadata meta;
      meta.full_mask = fill_mask.Encode();
      meta.content_type = ContentType::kOther;
      auto wh = worker.cache()->WriteAlternate(
          "/fill/" + std::to_string(i) + ".bin", "", "https", fill_id,
          filler.size(), meta);
      if (wh.has_value()) {
        (void)wh->write_sync(
            std::as_bytes(std::span(filler.data(), filler.size())));
        (void)wh->close_sync();
      }
      forced_wraps = worker.cache()->Stats().wraps_forced_past_lease;
      if (forced_wraps > 0) break;
      // Let the deferral clock pass the 1ms ceiling before retrying.
      std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
  };

  std::thread worker_thread([&worker]() { worker.Run(); });
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  CacheNotification notification;
  notification.url = "http://example.com/wrap-during-copy.html";
  notification.scheme = "https";
  notification.content_type = ContentType::kHtml;
  notification.capability_mask = 0x00000001;
  SendNotification(notification);

  // Wait for the discard to register (the hook's fill loop runs inside the
  // notification, so allow generous time on a loaded runner).
  bool discarded = false;
  for (int i = 0; i < 600 * kSanitizerBudgetScale; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    if (worker.stats().read_borrow_wrap_discards.load() >= 1) {
      discarded = true;
      break;
    }
  }
  WaitForWorkerIdle(worker);

  EXPECT_GT(forced_wraps.load(), 0u)
      << "the hook never forced a wrap past the ceiling";
  EXPECT_TRUE(discarded) << "torn copy was not discarded";
  EXPECT_EQ(worker.stats().read_borrow_wrap_discards.load(), 1u);
  // Nothing persisted: no transform variant, no processing counted.
  EXPECT_EQ(worker.stats().html_processed.load(), 0u);
  EXPECT_EQ(worker.stats().variants_written.load(), 0u);
  CapabilityMask mask = CapabilityMask::Decode(0x00000001);
  mask.set_image_format(CapabilityMask::ImageFormat::kOriginal);
  EXPECT_TRUE(ReadVariant(worker.cache(), cache_url, mask).empty())
      << "a variant was persisted from a torn copy";

  worker.Shutdown();
  worker_thread.join();
}

TEST_F(WorkerTest, HtmlTransformHappyPathNoWrapDiscards) {
  // Happy-path companion to HtmlWrapDuringDealiasCopyPersistsNothing: with
  // no wrap racing the copy, the de-aliased pipeline (copy + verify +
  // release before scan/transform/write) must still produce a correct
  // transform variant and count zero torn-copy discards.
  WorkerConfig config;
  config.socket_path = socket_path_;
  config.cache_path = cache_path_;
  config.cache_size_bytes = static_cast<uint64_t>(10 * 1024 * 1024);
  config.ram_cache_size = 0;  // exercise the disk-borrow + live-lease path

  config.unsafe_force_async_css =
      true;  // fixture page is not under test for the gate
  Worker worker(config, nullptr);
  ASSERT_TRUE(worker.Initialize());

  const std::string html =
      "<html><head><style>body { margin: 0; } "
      "h1 { color: red; }</style></head>"
      "<body><h1>Hello World</h1></body></html>";
  const std::string cache_url = "/dealias-html.html";
  CacheOriginal(worker.cache(), cache_url, html);

  std::thread worker_thread([&worker]() { worker.Run(); });
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  CacheNotification notification;
  notification.url = "http://example.com/dealias-html.html";
  notification.scheme = "https";
  notification.content_type = ContentType::kHtml;
  notification.capability_mask = 0x00000001;
  SendNotification(notification);

  CapabilityMask mask = CapabilityMask::Decode(0x00000001);
  mask.set_image_format(CapabilityMask::ImageFormat::kOriginal);
  std::string variant;
  for (int i = 0; i < 40 * kSanitizerBudgetScale; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    variant = ReadVariant(worker.cache(), cache_url, mask);
    if (!variant.empty()) break;
  }

  EXPECT_FALSE(variant.empty()) << "HTML variant not written from owned copy";
  if (!variant.empty()) {
    EXPECT_NE(variant.find("data-pagespeed-critical"), std::string::npos);
    EXPECT_NE(variant.find("Hello World"), std::string::npos);
  }
  // The happy path must not have tripped the torn-copy discard.
  EXPECT_EQ(worker.stats().read_borrow_wrap_discards.load(), 0u);
  // The original disk-borrowed bytes survived intact behind the copy.
  CapabilityMask default_mask;
  EXPECT_EQ(ReadVariant(worker.cache(), cache_url, default_mask), html);

  worker.Shutdown();
  worker_thread.join();
}

TEST_F(WorkerTest, WarmupSentinelGeneratesViewportSiblings) {
  // When warmup is triggered with viewport siblings enabled, it
  // should generate variants at all viewports.
  WorkerConfig config;
  config.socket_path = socket_path_;
  config.cache_path = cache_path_;
  config.cache_size_bytes = static_cast<uint64_t>(10 * 1024 * 1024);
  config.enable_warmup = true;
  config.proactive_viewport_variants = true;

  NullMessageHandler handler;
  Worker worker(config, &handler);
  ASSERT_TRUE(worker.Initialize());

  std::string url = "http://example.com/warmup-vp.jpg";
  std::string jpeg = ReadTestFile("jpeg/quality100.jpg");
  ASSERT_FALSE(jpeg.empty());
  CacheOriginal(worker.cache(), url, jpeg);

  std::thread worker_thread([&worker]() { worker.Run(); });
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  // Send warmup sentinel.
  CacheNotification notification;
  notification.url = url;
  notification.scheme = "https";
  notification.content_type = ContentType::kImage;
  notification.capability_mask = kWarmupSentinel;
  SendNotification(notification);

  // Poll for Mobile/WebP variant (generated proactively by warmup).
  CapabilityMask mobile_webp(
      CapabilityMask::ImageFormat::kWebP, CapabilityMask::Viewport::kMobile,
      CapabilityMask::PixelDensity::k1x, CapabilityMask::SaveData::kOff,
      CapabilityMask::TransferEncoding::kIdentity);
  std::string variant;
  for (int i = 0; i < kViewportSiblingPollIterations; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    variant = ReadVariant(worker.cache(), url, mobile_webp);
    if (!variant.empty()) break;
  }
  EXPECT_FALSE(variant.empty()) << "Warmup: Mobile/WebP not generated";

  // Desktop/WebP should also exist.  Poll because AVIF encoding
  // for earlier viewports may still be completing.
  CapabilityMask desktop_webp;  // Default = Desktop/Identity
  desktop_webp.set_image_format(CapabilityMask::ImageFormat::kWebP);
  std::string dv;
  for (int i = 0; i < kViewportSiblingPollIterations; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    dv = ReadVariant(worker.cache(), url, desktop_webp);
    if (!dv.empty()) break;
  }
  EXPECT_FALSE(dv.empty()) << "Warmup: Desktop/WebP not generated";

  worker.Shutdown();
  worker_thread.join();
}

// =============================================================================
// Save-Data and Density Variant Generation (Features 2 & 3 worker-level)
// =============================================================================

TEST_F(WorkerTest, ImageNotificationGeneratesSaveDataSiblings) {
  WorkerConfig config;
  config.socket_path = socket_path_;
  config.cache_path = cache_path_;
  config.cache_size_bytes = static_cast<uint64_t>(10 * 1024 * 1024);
  config.proactive_image_variants = true;
  config.proactive_viewport_variants = false;  // Only format + save-data
  config.proactive_savedata_variants = true;
  config.proactive_density_variants = false;

  NullMessageHandler handler;
  Worker worker(config, &handler);
  ASSERT_TRUE(worker.Initialize());

  std::string url = "http://example.com/savedata.jpg";
  std::string jpeg = ReadTestFile("jpeg/quality100.jpg");
  ASSERT_FALSE(jpeg.empty());
  CacheOriginal(worker.cache(), url, jpeg);

  std::thread worker_thread([&worker]() { worker.Run(); });
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  // Send notification with SaveData=off, Mobile/WebP.
  CapabilityMask notif_mask(
      CapabilityMask::ImageFormat::kWebP, CapabilityMask::Viewport::kMobile,
      CapabilityMask::PixelDensity::k1x, CapabilityMask::SaveData::kOff,
      CapabilityMask::TransferEncoding::kIdentity);
  CacheNotification notification;
  notification.url = url;
  notification.scheme = "https";
  notification.content_type = ContentType::kImage;
  notification.capability_mask = notif_mask.Encode();
  SendNotification(notification);

  // The Save-Data=on sibling is written as part of the same notification's
  // proactive matrix, so wait for the matrix to retire (a condition wait,
  // not a fixed poll budget) and then read the sibling.  The old 6s poll
  // expired mid-transcode under TSan's ~10x slowdown.
  ASSERT_NO_FATAL_FAILURE(WaitForNotificationRetiredWithVariant(worker));
  CapabilityMask sd_mask(
      CapabilityMask::ImageFormat::kWebP, CapabilityMask::Viewport::kMobile,
      CapabilityMask::PixelDensity::k1x, CapabilityMask::SaveData::kOn,
      CapabilityMask::TransferEncoding::kIdentity);
  std::string sd_variant = ReadVariant(worker.cache(), url, sd_mask);
  EXPECT_FALSE(sd_variant.empty())
      << "Save-Data=on sibling variant not generated";

  worker.Shutdown();
  worker_thread.join();
}

TEST_F(WorkerTest, ImageNotificationGeneratesDensitySiblings) {
  WorkerConfig config;
  config.socket_path = socket_path_;
  config.cache_path = cache_path_;
  config.cache_size_bytes = static_cast<uint64_t>(10 * 1024 * 1024);
  config.proactive_image_variants = true;
  config.proactive_viewport_variants = false;
  config.proactive_savedata_variants = false;
  config.proactive_density_variants = true;

  NullMessageHandler handler;
  Worker worker(config, &handler);
  ASSERT_TRUE(worker.Initialize());

  std::string url = "http://example.com/density.jpg";
  std::string jpeg = ReadTestFile("jpeg/quality100.jpg");
  ASSERT_FALSE(jpeg.empty());
  CacheOriginal(worker.cache(), url, jpeg);

  std::thread worker_thread([&worker]() { worker.Run(); });
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  // Send notification with 1x density, Mobile/WebP.
  CapabilityMask notif_mask(
      CapabilityMask::ImageFormat::kWebP, CapabilityMask::Viewport::kMobile,
      CapabilityMask::PixelDensity::k1x, CapabilityMask::SaveData::kOff,
      CapabilityMask::TransferEncoding::kIdentity);
  CacheNotification notification;
  notification.url = url;
  notification.scheme = "https";
  notification.content_type = ContentType::kImage;
  notification.capability_mask = notif_mask.Encode();
  SendNotification(notification);

  // Poll for 2x density sibling.  Uses selector fallback since
  // content-identical 1x/2x variants may be deduped to a single write.
  CapabilityMask den2x_mask(
      CapabilityMask::ImageFormat::kWebP, CapabilityMask::Viewport::kMobile,
      CapabilityMask::PixelDensity::k2xPlus, CapabilityMask::SaveData::kOff,
      CapabilityMask::TransferEncoding::kIdentity);
  std::string den_variant;
  for (int i = 0; i < 60 * kSanitizerBudgetScale; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    den_variant = ReadBestVariant(worker.cache(), url, den2x_mask);
    if (!den_variant.empty()) break;
  }
  EXPECT_FALSE(den_variant.empty())
      << "2x density sibling variant not servable";

  worker.Shutdown();
  worker_thread.join();
}

TEST_F(WorkerTest, InvalidateUrlDeletesVariants) {
  WorkerConfig config;
  config.socket_path = socket_path_;
  config.cache_path = cache_path_;
  config.cache_size_bytes = static_cast<uint64_t>(10 * 1024 * 1024);

  NullMessageHandler handler;
  Worker worker(config, &handler);
  ASSERT_TRUE(worker.Initialize());

  // Manually write a couple of variants to the cache.
  // Use path-only URL for cache writes (InvalidateUrl normalizes internally).
  std::string url = "http://example.com/invalidate.jpg";
  std::string cache_url = StripSchemeAuthority(url);
  std::string data = "fake image data";

  CapabilityMask mask1(
      CapabilityMask::ImageFormat::kWebP, CapabilityMask::Viewport::kMobile,
      CapabilityMask::PixelDensity::k1x, CapabilityMask::SaveData::kOff,
      CapabilityMask::TransferEncoding::kIdentity);
  CapabilityMask mask2(
      CapabilityMask::ImageFormat::kAvif, CapabilityMask::Viewport::kDesktop,
      CapabilityMask::PixelDensity::k1x, CapabilityMask::SaveData::kOff,
      CapabilityMask::TransferEncoding::kIdentity);

  // Write two alternates via the PageSpeedCache API.
  auto write_alt = [&](const CapabilityMask& mask) {
    AlternateId id =
        MaskToAlternateId(static_cast<uint8_t>(mask.Encode() & 0xFF));
    AlternateMetadata meta;
    meta.full_mask = mask.Encode();
    meta.content_type = ContentType::kImage;
    auto wh = worker.cache()->WriteAlternate(cache_url, "", "https", id,
                                             data.size(), meta);
    ASSERT_TRUE(wh.has_value());
    (void)wh->write_sync(std::as_bytes(std::span(data.data(), data.size())));
    (void)wh->close_sync();
  };
  write_alt(mask1);
  write_alt(mask2);

  // Verify they exist.
  EXPECT_FALSE(ReadVariant(worker.cache(), url, mask1).empty());
  EXPECT_FALSE(ReadVariant(worker.cache(), url, mask2).empty());

  // Invalidate — Remove() deletes all alternates for the URL at once.
  int deleted = worker.InvalidateUrl(url);
  EXPECT_GE(deleted, 1);

  worker.Shutdown();
}

// Test: Warmup handler propagates content_class and SSIMULACRA2 scores
// to variant metadata (regression test for race condition where warmup
// overwrites proactive variants with stripped metadata).
TEST_F(WorkerTest, WarmupSentinelPropagatesMetadata) {
  WorkerConfig config;
  config.socket_path = socket_path_;
  config.cache_path = cache_path_;
  config.cache_size_bytes = static_cast<uint64_t>(10 * 1024 * 1024);
  config.enable_warmup = true;
  config.proactive_viewport_variants = false;  // Desktop only to speed up test
  config.proactive_density_variants = false;   // 1x only to speed up test
  config.proactive_savedata_variants = true;
  // Fixed qualities with learned prediction off, so the save-data WebP
  // variant stores deterministically: with learned prediction on, this
  // fixture's save-data WebP scores below the Save-Data floor (target
  // 70 - 15 = 55, floor 52) and is legitimately declined under the
  // binding verify verdict.  Decline mechanics are pinned in the
  // transcoder suite; this test is about metadata propagation.  Content
  // analysis stays on because the test asserts the propagated
  // content_class.  q60 clears the Save-Data band while staying well
  // below the normal-mode 75.
  config.learned_quality = false;
  config.savedata_webp_quality = 60;

  NullMessageHandler handler;
  Worker worker(config, &handler);
  ASSERT_TRUE(worker.Initialize());

  std::string url = "http://example.com/warmup-meta.jpg";
  // sjpeg6.jpg is 512x512, wider than mobile_width=480.
  std::string jpeg = ReadTestFile("jpeg/sjpeg6.jpg");
  ASSERT_FALSE(jpeg.empty()) << "Test JPEG not found";

  AlternateMetadata meta;
  meta.content_type = ContentType::kImage;
  meta.origin_content_type = "image/jpeg";
  CacheOriginalWithMeta(worker.cache(), url, jpeg, meta);

  std::thread worker_thread([&worker]() { worker.Run(); });
  WorkerStopper stopper(worker, worker_thread);
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  // Send warmup sentinel notification (no prior proactive processing).
  CacheNotification notification;
  notification.url = url;
  notification.scheme = "https";
  notification.content_type = ContentType::kImage;
  notification.capability_mask = kWarmupSentinel;
  SendNotification(notification);

  // Warmup writes its non-save-data and save-data variants within a single
  // notification's processing, so wait for that work item to retire (a
  // condition wait, not a fixed poll) and then read both variants directly.
  // The old fixed poll -- even its bumped sanitizer branch (50s) -- expired
  // mid-transcode under TSan's ~10x slowdown on the AVIF / SSIMULACRA2
  // verification path (it aborted the shard).
  ASSERT_NO_FATAL_FAILURE(WaitForNotificationRetiredWithVariant(worker));

  // Non-save-data Desktop/WebP variant (written early in the warmup loop,
  // before the save-data iterations).
  CapabilityMask desktop_webp;
  desktop_webp.set_image_format(CapabilityMask::ImageFormat::kWebP);
  std::optional<VariantWithMeta> desktop_meta =
      ReadVariantWithMeta(worker.cache(), url, desktop_webp);
  ASSERT_TRUE(desktop_meta.has_value())
      << "Warmup should write non-save-data Desktop/WebP variant";

  // Save-data Desktop/WebP variant (written after non-save-data, with the
  // full SSIMULACRA2 verification path).
  CapabilityMask sd_webp(
      CapabilityMask::ImageFormat::kWebP, CapabilityMask::Viewport::kDesktop,
      CapabilityMask::PixelDensity::k1x, CapabilityMask::SaveData::kOn,
      CapabilityMask::TransferEncoding::kIdentity);
  std::optional<VariantWithMeta> sd_meta =
      ReadVariantWithMeta(worker.cache(), url, sd_webp);
  ASSERT_TRUE(sd_meta.has_value())
      << "Warmup should write save-data WebP variant";

  // Content class should be populated (not kUnknown=4).
  EXPECT_NE(sd_meta->metadata.content_class, 4u)
      << "Warmup metadata should carry content_class from content analysis";

  // SSIMULACRA2 score should be valid (not N/A = 0xFFFF).
  EXPECT_NE(sd_meta->metadata.ssimulacra2_score_x100, 0xFFFF)
      << "Warmup save-data WebP should have SSIMULACRA2 score";
  if (sd_meta->metadata.ssimulacra2_score_x100 != 0xFFFF) {
    EXPECT_LE(sd_meta->metadata.ssimulacra2_score_x100, 10000u);
  }
}

TEST_F(WorkerTest, ImageIncompleteMatricesCounterInApiMetrics) {
  // Verify the new counter appears in the Prometheus metrics endpoint.
  WorkerConfig config;
  config.socket_path = socket_path_;
  config.cache_path = cache_path_;
  config.cache_size_bytes = static_cast<uint64_t>(10 * 1024 * 1024);

  NullMessageHandler handler;
  Worker worker(config, &handler);
  ASSERT_TRUE(worker.Initialize());

  std::thread worker_thread([&worker]() { worker.Run(); });
  WorkerStopper stopper(worker, worker_thread);
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  // Connect to management socket and issue METRICS command.
  std::string mgmt_path = socket_path_ + ".mgmt";
  intptr_t fd = ConnectToSocket(mgmt_path);
  ASSERT_GE(fd, static_cast<intptr_t>(0));

  const char* cmd = "METRICS\n";
  ASSERT_EQ(pagespeed::test::PipeWrite(fd, cmd, strlen(cmd)),
            static_cast<ssize_t>(strlen(cmd)));

  // Keep the retry loop (tolerates an empty first read), but read each
  // attempt to EOF: the metrics document no longer fits one bufferful
  // (see ReadMgmtResponseToEof).
  std::string response;
  for (int i = 0; i < 20; ++i) {
    response = ReadMgmtResponseToEof(fd);
    if (!response.empty()) {
      break;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
  }
  pagespeed::test::ClosePipe(fd);
  ASSERT_FALSE(response.empty()) << "No data from management socket";

  EXPECT_NE(response.find("pagespeed_image_incomplete_matrices_total"),
            std::string::npos)
      << "METRICS should include pagespeed_image_incomplete_matrices_total. "
         "Response: "
      << response;
}

TEST_F(WorkerTest, ImageIncompleteMatricesCounterInMgmtStats) {
  // Verify the new image_incomplete_matrices counter appears in MGMT STATS.
  WorkerConfig config;
  config.socket_path = socket_path_;
  config.cache_path = cache_path_;
  config.cache_size_bytes = static_cast<uint64_t>(10 * 1024 * 1024);

  NullMessageHandler handler;
  Worker worker(config, &handler);
  ASSERT_TRUE(worker.Initialize());

  std::thread worker_thread([&worker]() { worker.Run(); });
  WorkerStopper stopper(worker, worker_thread);
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  // Connect to management socket and issue STATS command.
  std::string mgmt_path = socket_path_ + ".mgmt";
  intptr_t fd = ConnectToSocket(mgmt_path);
  ASSERT_GE(fd, static_cast<intptr_t>(0));

  const char* cmd = "STATS\n";
  ASSERT_EQ(pagespeed::test::PipeWrite(fd, cmd, strlen(cmd)),
            static_cast<ssize_t>(strlen(cmd)));

  // Keep the retry loop (tolerates an empty first read), but read each
  // attempt to EOF: the STATS document grows with every counter the build
  // gains and the field asserted on sits about two-thirds in, so a
  // fixed-buffer single read truncates it silently as soon as the document
  // outgrows the buffer (see ReadMgmtResponseToEof).
  std::string response;
  for (int i = 0; i < 20; ++i) {
    response = ReadMgmtResponseToEof(fd);
    if (!response.empty()) {
      break;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
  }
  pagespeed::test::ClosePipe(fd);
  ASSERT_FALSE(response.empty()) << "No data from management socket";

  EXPECT_NE(response.find("\"image_incomplete_matrices\":"), std::string::npos)
      << "STATS should include image_incomplete_matrices field. Response: "
      << response;
}

TEST_F(WorkerTest, ProactiveWriteDoesNotDuplicate) {
  // Verify that the proactive image write loop does not re-write variants
  // that were already written within the same notification.  The fix
  // inserts written alternate IDs into existing_ids so later loop
  // iterations skip them.
  //
  // Matrix: 3 viewports × 3 formats × 2 save-data × 1 density = 18 variants.
  WorkerConfig config;
  config.socket_path = socket_path_;
  config.cache_path = cache_path_;
  config.cache_size_bytes = static_cast<uint64_t>(10 * 1024 * 1024);
  config.proactive_image_variants = true;
  config.proactive_viewport_variants = true;
  config.proactive_savedata_variants = true;
  config.proactive_density_variants = false;

  NullMessageHandler handler;
  Worker worker(config, &handler);
  ASSERT_TRUE(worker.Initialize());

  std::string url = "http://example.com/dedup-test.jpg";
  std::string jpeg = ReadTestFile("jpeg/quality100.jpg");
  ASSERT_FALSE(jpeg.empty()) << "Test JPEG not found";
  CacheOriginal(worker.cache(), url, jpeg);

  std::thread worker_thread([&worker]() { worker.Run(); });
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  // Send notification with Desktop/WebP mask.
  CapabilityMask mask(
      CapabilityMask::ImageFormat::kWebP, CapabilityMask::Viewport::kDesktop,
      CapabilityMask::PixelDensity::k1x, CapabilityMask::SaveData::kOff,
      CapabilityMask::TransferEncoding::kIdentity);
  CacheNotification notification;
  notification.url = url;
  notification.scheme = "https";
  notification.content_type = ContentType::kImage;
  notification.capability_mask = mask.Encode();
  SendNotification(notification);

  // Wait for the notification's whole proactive matrix to retire.  The old
  // "variants_written unchanged for 1s" heuristic was a slowdown trap: under
  // TSan a single slow transcode (AVIF via libaom) leaves the counter flat
  // for over a second mid-matrix, so the poll declared completion at 3 of the
  // expected variants.  Retiring the work item is the true completion signal.
  ASSERT_NO_FATAL_FAILURE(WaitForNotificationRetiredWithVariant(worker));

  uint64_t written = worker.stats().variants_written.load();
  uint64_t attempts = worker.stats().alternate_writes.load();
  uint64_t dedup_skipped = worker.stats().dedup_writes_skipped.load();

  // The proactive loop should produce a reasonable number of variants.
  // Theoretical max: 3 viewports × 3 formats × 2 save-data = 18,
  // but some combos may not succeed (e.g., optimized original not smaller,
  // AVIF/WebP size gate rejects output larger than original).  With high-quality
  // source JPEGs, the size gate rejects many format conversions.
  // Content-identical variants (e.g., same bytes across viewports when image
  // is small enough that no resize occurs) are deduped: written once, counted
  // in dedup_writes_skipped for the duplicates.
  // Windows MSVC image codecs have different compression characteristics,
  // causing more variants to be size-gated (output larger than original).
  // Use a lower threshold on Windows.
#ifdef _WIN32
  constexpr uint64_t kMinExpectedVariants = 3;
#else
  constexpr uint64_t kMinExpectedVariants = 6;
#endif
  EXPECT_GE(written + dedup_skipped, kMinExpectedVariants)
      << "Expected at least " << kMinExpectedVariants
      << " variants generated from proactive matrix, got " << written
      << " written + " << dedup_skipped << " dedup-skipped";

  // Write attempts (actual cache writes) should equal successful writes —
  // content-deduped variants are skipped before the write, not failed.
  EXPECT_EQ(attempts, written)
      << "Write attempts (" << attempts << ") should equal successful writes ("
      << written << "); mismatch suggests duplicate write attempts";

  // Content dedup tracking: dedup_skipped counts how many writes were
  // skipped because identical bytes were already written under another ID.
  // For small test images, viewports don't resize → many deduped writes.
  EXPECT_GE(dedup_skipped, 0u);  // May be 0 for large images

  // Note: second-notification idempotency is not asserted here because
  // image encoding is non-deterministic (SSIMULACRA2 binary search,
  // content-aware quality adjustments).  Transcodes that fail the size
  // gate on one run may succeed on the next, producing legitimately new
  // variants.  The within-notification dedup (attempts == written above)
  // is the meaningful invariant.

  worker.Shutdown();
  worker_thread.join();
}

TEST_F(WorkerTest, DISABLED_DeferredRetryEscalatesMultipleTimes) {
  WorkerConfig config;
  config.socket_path = socket_path_;
  config.cache_path = cache_path_;
  config.cache_size_bytes = static_cast<uint64_t>(10 * 1024 * 1024);
  config.proactive_image_variants = false;
  config.proactive_viewport_variants = false;
  config.proactive_savedata_variants = false;
  config.proactive_density_variants = false;

  NullMessageHandler handler;
  Worker worker(config, &handler);
  ASSERT_TRUE(worker.Initialize());

  std::thread worker_thread([&worker]() { worker.Run(); });
  WorkerStopper stopper(worker, worker_thread);
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  std::string url = "http://example.com/deferred-multi.jpg";
  CacheNotification notification;
  notification.url = url;
  notification.scheme = "https";
  notification.content_type = ContentType::kImage;
  notification.capability_mask =
      CapabilityMask(CapabilityMask::ImageFormat::kWebP,
                     CapabilityMask::Viewport::kDesktop,
                     CapabilityMask::PixelDensity::k1x,
                     CapabilityMask::SaveData::kOff,
                     CapabilityMask::TransferEncoding::kIdentity)
          .Encode();

  // Send notification — original not in cache, will trigger deferred retries.
  SendNotification(notification);

  // Wait for at least 2 deferred retries to be scheduled.
  // First deferred fires at ~5s, second at ~15s from first notification.
  // Wait up to 25s total for 2 deferred retries.
  for (int i = 0; i < 250 * kSanitizerBudgetScale; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    if (worker.stats().cache_read_deferred_retries.load() >= 2) break;
  }
  EXPECT_GE(worker.stats().cache_read_deferred_retries.load(), 2u)
      << "Should have scheduled at least 2 deferred retries";

  // Now populate the cache — the next deferred retry should succeed.
  std::string jpeg = ReadTestFile("jpeg/quality100.jpg");
  ASSERT_FALSE(jpeg.empty());
  CacheOriginal(worker.cache(), url, jpeg);

  // Wait for the image to be processed.
  for (int i = 0; i < 300 * kSanitizerBudgetScale; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    if (worker.stats().images_processed.load() >= 1) break;
  }
  EXPECT_GE(worker.stats().images_processed.load(), 1u)
      << "Image should be processed after deferred retry succeeds";
  EXPECT_GE(worker.stats().cache_read_deferred_successes.load(), 1u)
      << "Should have at least one successful deferred retry";
}

// Collects worker log lines so a test can check why a slot is empty.
class SlotLogHandler : public MessageHandler {
 public:
  void Message(MessageType type, const char* format, ...) override {
    va_list args;
    va_start(args, format);
    MessageV(type, format, args);
    va_end(args);
  }
  bool HasLine(const std::string& needle_a, const std::string& needle_b) {
    std::lock_guard<std::mutex> lock(mu_);
    for (const auto& l : lines_) {
      if (l.find(needle_a) != std::string::npos &&
          l.find(needle_b) != std::string::npos) {
        return true;
      }
    }
    return false;
  }

 protected:
  void MessageV(MessageType, const char* format, va_list args) override {
    std::string s = FormatMessage(format, args);
    std::lock_guard<std::mutex> lock(mu_);
    lines_.push_back(std::move(s));
  }

 private:
  std::mutex mu_;
  std::vector<std::string> lines_;
};

std::string SlotNeedle(CapabilityMask::ImageFormat fmt,
                       CapabilityMask::Viewport vp,
                       CapabilityMask::SaveData sd) {
  CapabilityMask m(fmt, vp, CapabilityMask::PixelDensity::k1x, sd,
                   CapabilityMask::TransferEncoding::kIdentity);
  char buf[32];
  std::snprintf(buf, sizeof(buf), "(slot 0x%02X)",
                static_cast<int>(m.Encode() & 0xFF));
  return buf;
}

// The full proactive matrix writes every normal slot, and each Save-Data slot
// is either strictly smaller than the normal slot of its format and viewport
// or was left unwritten with a logged reason.  A normal client must never
// depend on a Save-Data sibling to fill a missing normal slot.
TEST_F(WorkerTest, ImageMatrixWritesNormalSlotsAndSaveDataIsSmallerOrRefused) {
  WorkerConfig config;
  config.socket_path = socket_path_;
  config.cache_path = cache_path_;
  config.cache_size_bytes = static_cast<uint64_t>(64 * 1024 * 1024);
  config.proactive_image_variants = true;
  config.proactive_viewport_variants = true;
  config.proactive_savedata_variants = true;
  config.proactive_density_variants = false;

  SlotLogHandler handler;
  Worker worker(config, &handler);
  ASSERT_TRUE(worker.Initialize());

  std::string url = "http://example.com/matrix.jpg";
  std::string jpeg = ReadTestFile("jpeg/quality100.jpg");
  ASSERT_FALSE(jpeg.empty());
  CacheOriginal(worker.cache(), url, jpeg);

  std::thread worker_thread([&worker]() { worker.Run(); });
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  CapabilityMask notif_mask(
      CapabilityMask::ImageFormat::kWebP, CapabilityMask::Viewport::kDesktop,
      CapabilityMask::PixelDensity::k1x, CapabilityMask::SaveData::kOff,
      CapabilityMask::TransferEncoding::kIdentity);
  CacheNotification notification;
  notification.url = url;
  notification.scheme = "https";
  notification.content_type = ContentType::kImage;
  notification.capability_mask = notif_mask.Encode();
  SendNotification(notification);
  ASSERT_NO_FATAL_FAILURE(WaitForNotificationRetiredWithVariant(worker));

  const CapabilityMask::ImageFormat formats[] = {
      CapabilityMask::ImageFormat::kOriginal,
      CapabilityMask::ImageFormat::kWebP,
      CapabilityMask::ImageFormat::kAvif,
  };
  const CapabilityMask::Viewport viewports[] = {
      CapabilityMask::Viewport::kMobile,
      CapabilityMask::Viewport::kTablet,
      CapabilityMask::Viewport::kDesktop,
  };
  auto slot = [&](CapabilityMask::ImageFormat fmt, CapabilityMask::Viewport vp,
                  CapabilityMask::SaveData sd) {
    return ReadVariant(
        worker.cache(), url,
        CapabilityMask(fmt, vp, CapabilityMask::PixelDensity::k1x, sd,
                       CapabilityMask::TransferEncoding::kIdentity));
  };
  uint64_t sd_empty = 0;
  for (auto vp : viewports) {
    for (auto fmt : formats) {
      const std::string normal = slot(fmt, vp, CapabilityMask::SaveData::kOff);
      const std::string saved = slot(fmt, vp, CapabilityMask::SaveData::kOn);
      // An empty slot must have a logged "not written" line for exactly that
      // slot, giving a size-gate reason; the original-format normal slot is
      // never empty.
      if (normal.empty()) {
        EXPECT_NE(fmt, CapabilityMask::ImageFormat::kOriginal)
            << "original-format normal slot missing at viewport "
            << static_cast<int>(vp);
        EXPECT_TRUE(
            handler.HasLine(SlotNeedle(fmt, vp, CapabilityMask::SaveData::kOff),
                            "not smaller than the"))
            << "no logged reason for the empty normal slot, format "
            << static_cast<int>(fmt) << " viewport " << static_cast<int>(vp);
        continue;
      }
      if (saved.empty()) {
        ++sd_empty;
        EXPECT_TRUE(
            handler.HasLine(SlotNeedle(fmt, vp, CapabilityMask::SaveData::kOn),
                            "not smaller than the"))
            << "no logged reason for the empty Save-Data slot, format "
            << static_cast<int>(fmt) << " viewport " << static_cast<int>(vp);
      } else {
        EXPECT_LT(saved.size(), normal.size())
            << "format " << static_cast<int>(fmt) << " viewport "
            << static_cast<int>(vp);
      }
    }
  }
  // The Save-Data counter never exceeds the Save-Data slots left empty.
  EXPECT_LE(worker.stats().image_savedata_refused.load(), sd_empty);

  worker.Shutdown();
  worker_thread.join();
}

// On a source resized at mobile and tablet, the full normal matrix is
// written, or each empty normal slot was refused with a logged reason and
// counted.  The full-size desktop JPEG does not depend on the
// smaller viewports processed before it in the same pass: it is byte for byte
// the JPEG a desktop-only pass writes (#482).
TEST_F(WorkerTest, ResizedImageMatrixWritesNormalSlotsOrderIndependent) {
  const std::string url = "http://example.com/resized.jpg";
  const std::string jpeg = ReadTestFile("jpeg/sjpeg3.jpg");
  ASSERT_FALSE(jpeg.empty());
  const CapabilityMask::ImageFormat formats[] = {
      CapabilityMask::ImageFormat::kOriginal,
      CapabilityMask::ImageFormat::kWebP,
      CapabilityMask::ImageFormat::kAvif,
  };
  const CapabilityMask::Viewport viewports[] = {
      CapabilityMask::Viewport::kMobile,
      CapabilityMask::Viewport::kTablet,
      CapabilityMask::Viewport::kDesktop,
  };
  auto mask_of = [](CapabilityMask::ImageFormat fmt,
                    CapabilityMask::Viewport vp) {
    return CapabilityMask(fmt, vp, CapabilityMask::PixelDensity::k1x,
                          CapabilityMask::SaveData::kOff,
                          CapabilityMask::TransferEncoding::kIdentity);
  };
  const CapabilityMask desktop_jpeg =
      mask_of(CapabilityMask::ImageFormat::kOriginal,
              CapabilityMask::Viewport::kDesktop);

  // One worker pass over the image; returns the normal desktop JPEG.
  auto run_pass = [&](bool full_matrix, const std::string& dir) {
    cache_path_ = temp_dir_ + "/" + dir;
#ifndef _WIN32
    socket_path_ = temp_dir_ + "/" + dir + ".sock";
#else
    socket_path_ += dir;
#endif
    WorkerConfig config;
    config.socket_path = socket_path_;
    config.cache_path = cache_path_;
    config.cache_size_bytes = static_cast<uint64_t>(64 * 1024 * 1024);
    config.proactive_image_variants = true;
    config.proactive_viewport_variants = full_matrix;
    config.proactive_savedata_variants = false;
    config.proactive_density_variants = false;
    // Narrow widths keep mobile and tablet resized and the test fast.
    config.mobile_width = 128;
    config.tablet_width = 256;

    SlotLogHandler handler;
    Worker worker(config, &handler);
    EXPECT_TRUE(worker.Initialize());
    CacheOriginal(worker.cache(), url, jpeg);
    std::thread worker_thread([&worker]() { worker.Run(); });
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    CacheNotification notification;
    notification.url = url;
    notification.scheme = "https";
    notification.content_type = ContentType::kImage;
    notification.capability_mask = desktop_jpeg.Encode();
    SendNotification(notification);
    WaitForNotificationRetiredWithVariant(worker);

    std::string desktop;
    if (full_matrix) {
      uint64_t empty_converted = 0;
      for (auto vp : viewports) {
        for (auto fmt : formats) {
          const std::string normal =
              ReadVariant(worker.cache(), url, mask_of(fmt, vp));
          if (!normal.empty()) continue;
          EXPECT_NE(fmt, CapabilityMask::ImageFormat::kOriginal)
              << "original-format normal slot missing at viewport "
              << static_cast<int>(vp);
          ++empty_converted;
          EXPECT_TRUE(handler.HasLine(
              SlotNeedle(fmt, vp, CapabilityMask::SaveData::kOff),
              "not written"))
              << "no logged reason for the empty normal slot, format "
              << static_cast<int>(fmt) << " viewport " << static_cast<int>(vp);
        }
      }
      // Every empty normal WebP/AVIF slot was counted: as a size refusal, a
      // quality decline, or an unconverted fall-through.
      EXPECT_GE(worker.stats().image_size_refused_webp.load() +
                    worker.stats().image_size_refused_avif.load() +
                    worker.stats().ssimulacra2_declines.load() +
                    worker.stats().image_unconverted_fallthrough.load(),
                empty_converted);
    }
    desktop = ReadVariant(worker.cache(), url, desktop_jpeg);
    worker.Shutdown();
    worker_thread.join();
    return desktop;
  };

  const std::string from_matrix = run_pass(/*full_matrix=*/true, "matrix");
  const std::string desktop_only = run_pass(/*full_matrix=*/false, "desktop");
  ASSERT_FALSE(desktop_only.empty());
  ASSERT_FALSE(from_matrix.empty());
  EXPECT_EQ(from_matrix.size(), desktop_only.size());
  EXPECT_TRUE(from_matrix == desktop_only)
      << "the desktop JPEG depends on the viewports processed before it";
}

// A Save-Data variant that is not smaller than the stored normal variant of
// its slot is refused, counted exactly once, and not stored.
TEST_F(WorkerTest, SaveDataVariantNotSmallerThanNormalIsRefusedAndCounted) {
  WorkerConfig config;
  config.socket_path = socket_path_;
  config.cache_path = cache_path_;
  config.cache_size_bytes = static_cast<uint64_t>(64 * 1024 * 1024);
  config.proactive_image_variants = true;
  config.proactive_viewport_variants = false;
  config.proactive_savedata_variants = true;
  config.proactive_density_variants = false;

  NullMessageHandler handler;
  Worker worker(config, &handler);
  ASSERT_TRUE(worker.Initialize());

  std::string url = "http://example.com/gate.jpg";
  std::string jpeg = ReadTestFile("jpeg/quality100.jpg");
  ASSERT_FALSE(jpeg.empty());
  CacheOriginal(worker.cache(), url, jpeg);

  // A tiny stored normal desktop WebP: no Save-Data encode can be smaller.
  CapabilityMask normal_webp(
      CapabilityMask::ImageFormat::kWebP, CapabilityMask::Viewport::kDesktop,
      CapabilityMask::PixelDensity::k1x, CapabilityMask::SaveData::kOff,
      CapabilityMask::TransferEncoding::kIdentity);
  const std::string tiny = "tiny-normal-webp";
  AlternateMetadata meta;
  meta.full_mask = normal_webp.Encode();
  meta.content_type = ContentType::kImage;
  meta.flags = AlternateMetadata::kFlagWorkerProcessed;
  auto wh = worker.cache()->WriteAlternate(
      StripSchemeAuthority(url), "", "https",
      MaskToAlternateId(static_cast<uint8_t>(normal_webp.Encode() & 0xFF)),
      tiny.size(), meta);
  ASSERT_TRUE(wh.has_value());
  ASSERT_TRUE(wh->write_sync(std::as_bytes(std::span(tiny))).has_value());
  ASSERT_TRUE(wh->close_sync().has_value());

  std::thread worker_thread([&worker]() { worker.Run(); });
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  CacheNotification notification;
  notification.url = url;
  notification.scheme = "https";
  notification.content_type = ContentType::kImage;
  notification.capability_mask = normal_webp.Encode();
  SendNotification(notification);
  ASSERT_NO_FATAL_FAILURE(WaitForNotificationRetiredWithVariant(worker));

  EXPECT_EQ(worker.stats().image_savedata_refused.load(), 1u);
  CapabilityMask sd_webp(
      CapabilityMask::ImageFormat::kWebP, CapabilityMask::Viewport::kDesktop,
      CapabilityMask::PixelDensity::k1x, CapabilityMask::SaveData::kOn,
      CapabilityMask::TransferEncoding::kIdentity);
  EXPECT_TRUE(ReadVariant(worker.cache(), url, sd_webp).empty())
      << "the refused Save-Data WebP was stored";
  // The Save-Data AVIF and original-format slots, smaller than the normal
  // variants built in the same pass, are stored.
  EXPECT_FALSE(
      ReadVariant(worker.cache(), url,
                  CapabilityMask(CapabilityMask::ImageFormat::kAvif,
                                 CapabilityMask::Viewport::kDesktop,
                                 CapabilityMask::PixelDensity::k1x,
                                 CapabilityMask::SaveData::kOn,
                                 CapabilityMask::TransferEncoding::kIdentity))
          .empty());

  worker.Shutdown();
  worker_thread.join();
}

}  // namespace
}  // namespace pagespeed
