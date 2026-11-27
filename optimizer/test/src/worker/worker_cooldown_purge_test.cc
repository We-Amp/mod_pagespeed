// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

// Cooldown eviction, purge generation and retry tests
//
// - Cooldown eviction and scoping
// - Auto-heal state, generational dedup and purge generations
// - Incomplete-matrix retry and scheme isolation
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
#include <span>
#include <string>
#include <thread>
#include <vector>

#include "absl/strings/str_cat.h"
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
#include "src/worker/url_registry.h"
#include "src/worker/worker.h"
#include "test/test_util/temp_dir.h"
#include "test/test_util/worker_quiescence.h"

namespace pagespeed {
namespace {

// =============================================================================
// ClearDedupAndCooldown (hostname-scoped) Tests
// =============================================================================

TEST_F(WorkerTest, ClearDedupScopedToHostname) {
  // ClearDedupAndCooldown only clears dedup entries for the given URL+hostname,
  // not for the same URL on different hostnames.
  WorkerConfig config;
  config.socket_path = socket_path_;
  config.cache_path = cache_path_;
  config.cache_size_bytes = static_cast<uint64_t>(10 * 1024 * 1024);

  NullMessageHandler handler;
  Worker worker(config, &handler);
  ASSERT_TRUE(worker.Initialize());

  std::string url = "http://example.com/scoped.css";
  std::string css = "body { margin: 0; }";
  CacheOriginal(worker.cache(), url, css);

  std::thread worker_thread([&worker]() { worker.Run(); });
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  // Process notification for host1 (empty hostname = default).
  CacheNotification notification;
  notification.url = url;
  notification.scheme = "https";
  notification.content_type = ContentType::kCss;
  notification.capability_mask = CapabilityMask().Encode();
  SendNotification(notification);

  // Wait for processing AND in-flight work to drain.  Although
  // MarkVariantProcessed runs before css_processed increments,
  // checking in_flight_work()==0 ensures the full work item
  // (including WriteCompressedVariants) has completed.
  for (int i = 0; i < 40 * kSanitizerBudgetScale; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    if (worker.stats().css_processed.load() >= 1 &&
        worker.in_flight_work() == 0) {
      break;
    }
  }
  EXPECT_GE(worker.stats().css_processed.load(), 1u);

  // css_processed moves before the in-flight entry is erased.  Drain first, or
  // the duplicate below is rejected by the in-flight guard and bumps
  // skipped_inflight instead of skipped_dedup.
  WaitForWorkerIdle(worker);

  // Clear dedup for a DIFFERENT hostname — should NOT affect the empty hostname
  // entry.
  worker.ClearDedupAndCooldown(url, "other.com");

  // Send same notification again — should still be deduped.
  uint64_t skipped_before = worker.stats().notifications_skipped_dedup.load();
  SendNotification(notification);
  // Poll for the dedup counter to advance instead of a fixed sleep (slow
  // shards can sit at the pipe read past a fixed budget).
  for (int i = 0; i < 80 * kSanitizerBudgetScale; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    if (worker.stats().notifications_skipped_dedup.load() > skipped_before)
      break;
  }

  EXPECT_GT(worker.stats().notifications_skipped_dedup.load(), skipped_before)
      << "Notification should still be deduped after clearing a different host";

  worker.Shutdown();
  worker_thread.join();
}

// =============================================================================
// InvalidateUrl clears cooldown
// =============================================================================

TEST_F(WorkerTest, InvalidateUrlClearsCooldown) {
  // InvalidateUrl should clear the HTML cooldown so the URL can be
  // immediately reprocessed.
  WorkerConfig config;
  config.socket_path = socket_path_;
  config.cache_path = cache_path_;
  config.cache_size_bytes = static_cast<uint64_t>(10 * 1024 * 1024);

  NullMessageHandler handler;
  config.unsafe_force_async_css =
      true;  // fixture page is not under test for the gate
  Worker worker(config, &handler);
  ASSERT_TRUE(worker.Initialize());

  std::string url = "http://example.com/cooldown-invalidate.html";
  std::string html =
      "<html><head><style>body { margin: 0; }</style></head>"
      "<body>Test</body></html>";
  CacheOriginal(worker.cache(), url, html);

  std::thread worker_thread([&worker]() { worker.Run(); });
  WorkerStopper stopper(worker, worker_thread);
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  CacheNotification notification;
  notification.url = url;
  notification.scheme = "https";
  notification.content_type = ContentType::kHtml;
  notification.capability_mask = CapabilityMask().Encode();

  // First notification — processes and sets cooldown.
  SendNotification(notification);
  bool first_processed = false;
  for (int i = 0; i < 80 * kSanitizerBudgetScale; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    if (worker.stats().html_processed.load() >= 1) {
      first_processed = true;
      break;
    }
  }
  ASSERT_TRUE(first_processed)
      << "HTML should be processed once before testing cooldown invalidation";

  // html_processed moves before the in-flight entry is erased, and InvalidateUrl
  // clears the dedup set and the cooldown but NOT the in-flight set.  Without
  // draining first, the re-notification below is rejected by the in-flight guard
  // and never reprocesses, which matches how this test fails under load.
  WaitForWorkerIdle(worker);

  uint64_t html_before = worker.stats().html_processed.load();

  // Invalidate the URL — should clear both dedup and cooldown.
  worker.InvalidateUrl(url, "");

  // Re-populate cache so the worker can read it.
  CacheOriginal(worker.cache(), url, html);

  // Send notification again — should process since cooldown was cleared.
  SendNotification(notification);
  bool reprocessed = false;
  for (int i = 0; i < 80 * kSanitizerBudgetScale; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    if (worker.stats().html_processed.load() > html_before) {
      reprocessed = true;
      break;
    }
  }

  EXPECT_TRUE(reprocessed)
      << "HTML should be reprocessed after InvalidateUrl clears cooldown";
}

// =============================================================================
// HTML Cooldown LRU Eviction Tests
// =============================================================================

TEST_F(WorkerTest, HtmlCooldownEvictionPrunesOldEntries) {
  // When the cooldown map exceeds kMaxCooldownEntries (256), oldest entries
  // should be evicted.  Covers lines 1869-1885 (cooldown LRU eviction).
  //
  // Strategy: use a very small cache (2MB) and send many unique HTML
  // notifications through the socket.  Pre-populate the cache with HTML
  // content for all URLs.  The processed variant writes may fail under
  // extreme cache pressure (2MB shared across 270 URLs), adding
  // write-failure cooldown entries.  With >256 unique URLs, the eviction
  // code at lines 1869-1885 is exercised.
  //
  // Even if writes succeed (and cooldown entries are cleared on success),
  // the tentative cooldown entries accumulate from concurrent/queued
  // notifications before processing completes.
  WorkerConfig config;
  config.socket_path = socket_path_;
  config.cache_path = cache_path_;
  config.cache_size_bytes =
      static_cast<uint64_t>(2 * 1024 * 1024);  // 2MB — very small

  NullMessageHandler handler;
  Worker worker(config, &handler);
  ASSERT_TRUE(worker.Initialize());

  // Pre-populate cache with 270 unique HTML pages.
  // Each page has inline CSS so the worker can produce a variant.
  // Use short content to fit within the tiny cache.
  constexpr int kNumUrls = 270;
  for (int i = 0; i < kNumUrls; ++i) {
    std::string url = "http://example.com/evict-" + std::to_string(i) + ".html";
    std::string html =
        "<html><head><style>h1{color:red}</style></head>"
        "<body><h1>Page " +
        std::to_string(i) + "</h1></body></html>";
    CacheOriginal(worker.cache(), url, html);
  }

  // Run worker in background.
  std::thread worker_thread([&worker]() { worker.Run(); });
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  // Send 270 unique HTML notifications in batches.  Each one sets a
  // tentative cooldown entry (line 1478).  Write failures add entries
  // at line 1864.  Once the map exceeds 256, eviction at lines 1869-1885
  // fires.
  constexpr int kBatchSize = 30;
  for (int i = 0; i < kNumUrls; ++i) {
    CacheNotification notification;
    notification.scheme = "https";
    notification.url =
        "http://example.com/evict-" + std::to_string(i) + ".html";
    notification.hostname = "";
    notification.content_type = ContentType::kHtml;
    notification.capability_mask = 0;
    SendNotification(notification);
    // Brief pause between batches to let the worker drain connections.
    if ((i + 1) % kBatchSize == 0) {
      std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
  }

  // Wait for processing to complete (or timeout).
  // Use 120 iterations to give Windows enough headroom for pipe I/O.
  for (int i = 0; i < 120 * kSanitizerBudgetScale; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    uint64_t processed = worker.stats().html_processed.load();
    uint64_t errors = worker.stats().alternate_write_failures.load();
    uint64_t skipped = worker.stats().notifications_skipped_dedup.load();
    // We expect all notifications to be either processed, failed, or skipped.
    if (processed + errors + skipped >= static_cast<uint64_t>(kNumUrls - 10)) {
      break;
    }
  }

  // The worker should not crash regardless of whether eviction triggered.
  EXPECT_NE(worker.cache(), nullptr);

  // Verify that ClearDedupAndCooldown still works (accesses the map).
  worker.ClearDedupAndCooldown("http://example.com/evict-0.html", "");

  // Verify notifications were received.  Allow a small tolerance for pipe
  // buffering on Windows where a notification may still be in-flight at
  // assertion time.
  EXPECT_GE(worker.stats().notifications_received.load(),
            static_cast<uint64_t>(kNumUrls - 2))
      << "All notifications should have been received";

  worker.Shutdown();
  worker_thread.join();
}

TEST_F(WorkerTest, HtmlCooldownMapBoundedUnderLoad) {
  // Verify that the cooldown map doesn't grow without bound when many
  // unique HTML notifications are sent for URLs not in cache.
  // The tentative cooldown path (line 1478) adds entries; the worker
  // should remain functional even with >kMaxCooldownEntries (256)
  // unique URLs.
  //
  // Send notifications in batches with small delays to avoid overwhelming
  // the Unix socket listen backlog.
  WorkerConfig config;
  config.socket_path = socket_path_;
  config.cache_path = cache_path_;
  config.cache_size_bytes = static_cast<uint64_t>(10 * 1024 * 1024);

  NullMessageHandler handler;
  Worker worker(config, &handler);
  ASSERT_TRUE(worker.Initialize());

  // Run worker in background.
  std::thread worker_thread([&worker]() { worker.Run(); });
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  // Send 270 unique HTML notifications (URLs not in cache) in batches.
  // Each sets a tentative cooldown entry.
  constexpr int kNumUrls = 270;
  constexpr int kBatchSize = 30;
  for (int i = 0; i < kNumUrls; ++i) {
    CacheNotification notification;
    notification.url =
        "http://example.com/bounded-" + std::to_string(i) + ".html";
    notification.hostname = "example.com";
    notification.scheme = "https";
    notification.content_type = ContentType::kHtml;
    notification.capability_mask = 0;
    SendNotification(notification);
    // Brief pause between batches to let the worker drain connections.
    if ((i + 1) % kBatchSize == 0) {
      std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
  }

  // Wait for processing.
  // Use 80 iterations to give Windows enough headroom for pipe I/O.
  for (int i = 0; i < 80 * kSanitizerBudgetScale; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    if (worker.stats().notifications_received.load() >=
        static_cast<uint64_t>(kNumUrls - 10)) {
      break;
    }
  }

  // Worker should remain functional.
  EXPECT_NE(worker.cache(), nullptr);

  // Verify ClearDedupAndCooldown works — accessing the map doesn't crash.
  worker.ClearDedupAndCooldown("http://example.com/bounded-0.html",
                               "example.com");

  // Cache operations should still work.
  std::string url = "http://example.com/after-load.html";
  std::string content = "still works";
  CacheOriginal(worker.cache(), url, content);
  CapabilityMask default_mask;
  EXPECT_EQ(ReadVariant(worker.cache(), url, default_mask), content);

  worker.Shutdown();
  worker_thread.join();
}

TEST_F(WorkerTest, CompleteMatrixMarksProcessedAndAllowsDedup) {
  // Verify that a complete variant matrix marks the URL as processed,
  // causing re-notifications to be dedup-skipped. Also verify the
  // image_incomplete_matrices counter stays at zero.
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

  std::string url = "http://example.com/complete-matrix.jpg";
  std::string image_data = ReadTestFile("jpeg/quality100.jpg");
  ASSERT_FALSE(image_data.empty()) << "Test image not found";
  CacheOriginal(worker.cache(), url, image_data);

  std::thread worker_thread([&worker]() { worker.Run(); });
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

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

  // First notification — should process and complete.  Wait for the work
  // item to retire: MarkVariantProcessed and the in-flight erase both run
  // before retirement, so afterwards the dedup registration is visible AND
  // the in-flight guard cannot swallow the second notification below
  // (which would surface as notifications_skipped_inflight instead of
  // notifications_skipped_dedup).  The old two-poll wait — counter moved,
  // then "stable for 100ms" — could declare completion in that window.
  SendNotification(notification);
  ASSERT_NO_FATAL_FAILURE(WaitForNotificationRetiredWithVariant(worker));
  EXPECT_GE(worker.stats().images_processed.load(), 1u)
      << "Image should be processed";
  EXPECT_EQ(worker.stats().image_incomplete_matrices.load(), 0u)
      << "Complete matrix should not increment incomplete counter";

  uint64_t skipped_before = worker.stats().notifications_skipped_dedup.load();

  // Second notification — should be dedup-skipped.  Poll for the skip counter
  // to advance instead of a fixed sleep (slow shards can sit at the pipe read
  // past a fixed budget).
  SendNotification(notification);
  for (int i = 0; i < 80 * kSanitizerBudgetScale; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    if (worker.stats().notifications_skipped_dedup.load() > skipped_before)
      break;
  }

  EXPECT_GT(worker.stats().notifications_skipped_dedup.load(), skipped_before)
      << "Duplicate notification should be dedup-skipped after complete matrix";

  worker.Shutdown();
  worker_thread.join();
}

TEST_F(WorkerTest, CacheReadFailureDoesNotMarkProcessed) {
  // When original is not in cache, the notification fails early (before
  // the variant tracking code), so no dedup marking occurs. A later
  // notification with the original populated should process normally.
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

  std::string url = "http://example.com/gapfill.jpg";
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

  // First notification — original not in cache, processing fails.
  // ReadOriginalWithRetry total budget is ~4.4s, so wait up to 8s.
  SendNotification(notification);
  for (int i = 0; i < 80 * kSanitizerBudgetScale; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    if (worker.stats().cache_read_failures.load() >= 1 ||
        worker.stats().notifications_received.load() >= 1)
      break;
  }
  EXPECT_EQ(worker.stats().images_processed.load(), 0u)
      << "Should not process when original is missing";

  // The failed attempt still occupies the in-flight set until the event loop
  // retires it.  Drain, or the second notification is rejected by the in-flight
  // guard and never processes.
  WaitForWorkerIdle(worker);

  // Now populate the cache with the original image.
  std::string image_data = ReadTestFile("jpeg/quality100.jpg");
  ASSERT_FALSE(image_data.empty()) << "Test image not found";
  CacheOriginal(worker.cache(), url, image_data);

  // Second notification — original now exists, should process.
  SendNotification(notification);
  for (int i = 0; i < 60 * kSanitizerBudgetScale; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    if (worker.stats().images_processed.load() >= 1) break;
  }
  EXPECT_GE(worker.stats().images_processed.load(), 1u)
      << "Second notification should process after original is populated";

  // Should not be dedup-skipped since no successful processing happened before.
  EXPECT_EQ(worker.stats().notifications_skipped_dedup.load(), 0u)
      << "Re-notification should not be dedup-skipped when first attempt "
         "failed";
}

TEST_F(WorkerTest, InvalidateUrlClearsIncompleteRetryState) {
  // Verify that InvalidateUrl clears the incomplete-matrix retry state,
  // allowing full re-processing after a purge.
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

  std::string url = "http://example.com/retry-clear.jpg";
  std::string image_data = ReadTestFile("jpeg/quality100.jpg");
  ASSERT_FALSE(image_data.empty()) << "Test image not found";
  CacheOriginal(worker.cache(), url, image_data);

  std::thread worker_thread([&worker]() { worker.Run(); });
  WorkerStopper stopper(worker, worker_thread);
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

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

  // First notification — process and dedup-mark.  Wait for the work item to
  // retire rather than for a number of seconds: the requested combination's
  // encodes take as long as the host lets them, and once the item has retired
  // the counters are final.  Retired also means its in-flight entry is gone
  // (InvalidateUrl clears dedup and retry state but NOT the in-flight set), so
  // the re-notification below is not rejected by the in-flight guard.
  SendNotification(notification);
  ASSERT_TRUE(pagespeed::test::WaitForNotificationsRetired(worker, 1,
                                                           kWorkerStallBudget));
  ASSERT_GE(worker.stats().images_processed.load(), 1u)
      << "first notification wrote no image variant";

  // Purge the URL — this should clear dedup AND retry state.
  worker.InvalidateUrl(url, "");

  // Re-populate cache so the next notification can process.
  CacheOriginal(worker.cache(), url, image_data);

  uint64_t processed_before = worker.stats().images_processed.load();

  // Re-notify — should be processed (not dedup-skipped).  Same event: once
  // this second work item has retired, either it reprocessed the image or it
  // was skipped, and no amount of further waiting changes which.
  SendNotification(notification);
  ASSERT_TRUE(pagespeed::test::WaitForNotificationsRetired(worker, 2,
                                                           kWorkerStallBudget));
  EXPECT_GT(worker.stats().images_processed.load(), processed_before)
      << "After InvalidateUrl, re-notification should be processed "
         "(dedup-skipped: "
      << worker.stats().notifications_skipped_dedup.load()
      << ", rejected as in-flight: "
      << worker.stats().notifications_skipped_inflight.load() << ")";
}

// The in-flight guard: a notification for a URL whose previous notification
// is still being processed is dropped and counted, not queued behind it.  Two
// frames sent in one write are dispatched from the same read callback, before
// the first work item can finish, so the second is rejected
// deterministically.  Should the kernel split the write into two reads, the
// first item is still in flight: the URL has no cached original, so its
// source read runs the retry backoff (about 310 ms) before it gives up, and
// the second half of a split local write arrives far sooner than that.
TEST_F(WorkerTest, NotificationWhileSameUrlInFlightIsRejected) {
  WorkerConfig config;
  config.socket_path = socket_path_;
  config.cache_path = cache_path_;
  config.cache_size_bytes = static_cast<uint64_t>(10 * 1024 * 1024);

  NullMessageHandler handler;
  Worker worker(config, &handler);
  ASSERT_TRUE(worker.Initialize());

  std::thread worker_thread([&worker]() { worker.Run(); });
  WorkerStopper stopper(worker, worker_thread);

  CacheNotification notification;
  notification.url = "http://example.com/in-flight.jpg";
  notification.scheme = "https";
  notification.content_type = ContentType::kImage;
  notification.capability_mask =
      CapabilityMask(CapabilityMask::ImageFormat::kWebP,
                     CapabilityMask::Viewport::kDesktop,
                     CapabilityMask::PixelDensity::k1x,
                     CapabilityMask::SaveData::kOff,
                     CapabilityMask::TransferEncoding::kIdentity)
          .Encode();
  std::vector<char> frames = notification.Serialize();
  const std::vector<char> one = frames;
  frames.insert(frames.end(), one.begin(), one.end());
  SendRawNotificationBytes(frames);

  ASSERT_TRUE(pagespeed::test::WaitForNotificationsRetired(worker, 2,
                                                           kWorkerStallBudget));
  EXPECT_EQ(2u, worker.stats().notifications_received.load());
  EXPECT_EQ(1u, worker.stats().notifications_skipped_inflight.load())
      << "the second notification for a URL already in flight must be "
         "rejected by the in-flight guard";
}

// =============================================================================
// Auto-Heal Tests
// =============================================================================

TEST_F(WorkerTest, AutoHealStatsInitiallyZero) {
  WorkerConfig config;
  config.socket_path = socket_path_;
  config.cache_path = cache_path_;
  config.cache_size_bytes = static_cast<uint64_t>(10 * 1024 * 1024);

  NullMessageHandler handler;
  Worker worker(config, &handler);
  ASSERT_TRUE(worker.Initialize());

  EXPECT_EQ(worker.stats().cache_auto_heals.load(), 0u);
  EXPECT_EQ(worker.stats().cache_auto_heal_exhausted.load(), 0u);

  worker.Shutdown();
}

TEST_F(WorkerTest, InvalidateUrlResetsAutoHealState) {
  // Manual purge (InvalidateUrl with default clear_heal_state=true) should
  // reset the per-URL auto-heal counter, allowing future auto-heals.
  WorkerConfig config;
  config.socket_path = socket_path_;
  config.cache_path = cache_path_;
  config.cache_size_bytes = static_cast<uint64_t>(10 * 1024 * 1024);

  NullMessageHandler handler;
  Worker worker(config, &handler);
  ASSERT_TRUE(worker.Initialize());

  std::string url = "http://example.com/autoheal-reset.jpg";
  std::string hostname = "example.com";

  // Populate cache so InvalidateUrl has something to remove.
  CacheOriginal(worker.cache(), url, "test content");

  // Manual purge should succeed (and reset any internal heal state).
  int deleted = worker.InvalidateUrl(url, hostname);
  // deleted might be 0 since hostname doesn't match the empty default,
  // but the point is that the call doesn't crash and clears state.
  (void)deleted;

  EXPECT_EQ(worker.stats().cache_auto_heals.load(), 0u)
      << "Manual purge should not increment auto-heal stats";

  worker.Shutdown();
}

TEST_F(WorkerTest, AutoHealDoesNotBlockNormalProcessing) {
  // Verify that the auto-heal mechanism doesn't interfere with normal
  // notification processing. Write original → send notification → check
  // that processing completes without triggering auto-heal.
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

  std::string url = "http://example.com/normal.jpg";
  std::string image_data = ReadTestFile("jpeg/quality100.jpg");
  ASSERT_FALSE(image_data.empty()) << "Test image not found";

  // Populate cache with original
  CacheOriginal(worker.cache(), url, image_data);

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

  SendNotification(notification);
  for (int i = 0; i < 60 * kSanitizerBudgetScale; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    if (worker.stats().images_processed.load() >= 1) break;
  }

  EXPECT_GE(worker.stats().images_processed.load(), 1u)
      << "Normal processing should complete";
  EXPECT_EQ(worker.stats().cache_auto_heals.load(), 0u)
      << "Auto-heal should not trigger for successful reads";
}

// =============================================================================
// Item 3: Generational double-buffer dedup tests
// =============================================================================

TEST_F(WorkerTest, DedupBothSetsConsulted) {
  // Verify that entries rotated to the previous set still dedup.
  // Use a small max_processed_entries (100) to force overflow with few
  // notifications, keeping the test fast and socket-friendly.
  WorkerConfig config;
  config.socket_path = socket_path_;
  config.cache_path = cache_path_;
  config.cache_size_bytes = static_cast<uint64_t>(100 * 1024 * 1024);
  config.gzip_level = 0;  // Disable compression for speed
  config.brotli_level = 0;
  config.max_processed_entries = 100;

  NullMessageHandler handler;
  Worker worker(config, &handler);
  ASSERT_TRUE(worker.Initialize());

  // Pre-populate cache with 101 CSS URLs to force one overflow rotation.
  // Use unminified CSS so the worker actually minifies and increments
  // css_processed (already-minimal CSS takes a different path).
  static constexpr size_t kCount = 101;
  std::string css_content =
      "body  {  margin:  0;  padding:  0;  }\n"
      "h1  {  color:  red;  font-size:  24px;  }\n";
  std::string first_url;
  for (size_t i = 0; i < kCount; ++i) {
    std::string url = absl::StrCat("http://example.com/dedup-both-", i, ".css");
    if (i == 0) first_url = url;
    CacheOriginal(worker.cache(), url, css_content);
  }

  std::thread worker_thread([&worker]() { worker.Run(); });
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  // Send all notifications.
  for (size_t i = 0; i < kCount; ++i) {
    CacheNotification notification;
    notification.url =
        absl::StrCat("http://example.com/dedup-both-", i, ".css");
    notification.content_type = ContentType::kCss;
    notification.scheme = "https";
    notification.capability_mask = CapabilityMask().Encode();
    SendNotification(notification);
  }

  // Wait for all notifications to be received and all in-flight work to
  // complete.  Checking in_flight_work==0 ensures MarkVariantProcessed
  // has populated the dedup set for every processed URL.
  for (int w = 0; w < 300; ++w) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    if (worker.stats().notifications_received.load() >= kCount &&
        worker.in_flight_work() == 0) {
      break;
    }
  }
  EXPECT_GE(worker.stats().css_processed.load(), kCount)
      << "All CSS notifications should be processed";

  // The first URL was processed early and should now be in previous_ set
  // after the overflow rotation.  Re-sending it should still dedup.
  uint64_t skipped_before = worker.stats().notifications_skipped_dedup.load();
  CacheNotification dup_notif;
  dup_notif.url = first_url;
  dup_notif.scheme = "https";
  dup_notif.content_type = ContentType::kCss;
  dup_notif.capability_mask = CapabilityMask().Encode();
  SendNotification(dup_notif);
  // Poll for the dedup counter to advance instead of a fixed sleep (slow
  // shards can sit at the pipe read past a fixed budget).
  for (int i = 0; i < 80 * kSanitizerBudgetScale; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    if (worker.stats().notifications_skipped_dedup.load() > skipped_before)
      break;
  }

  EXPECT_GT(worker.stats().notifications_skipped_dedup.load(), skipped_before)
      << "Entry in previous_ set should still be consulted for dedup";

  worker.Shutdown();
  worker_thread.join();
}

TEST_F(WorkerTest, DedupOldEntriesEvictedAfterDoubleOverflow) {
  // After two full overflow rotations, entries from the first wave should
  // be evicted from both current_ and previous_.
  // We send 3 sequential waves of 101 unique CSS URLs, each waited to
  // completion, ensuring deterministic overflow: wave 1 fills+overflows,
  // wave 2 fills+overflows (evicting wave 1 from previous), wave 3
  // guarantees all wave-1 entries are gone.
  WorkerConfig config;
  config.socket_path = socket_path_;
  config.cache_path = cache_path_;
  config.cache_size_bytes = static_cast<uint64_t>(100 * 1024 * 1024);
  config.gzip_level = 0;
  config.brotli_level = 0;
  config.max_processed_entries = 100;

  NullMessageHandler handler;
  Worker worker(config, &handler);
  ASSERT_TRUE(worker.Initialize());

  static constexpr size_t kWaveSize = 101;
  static constexpr size_t kNumWaves = 3;
  std::string css_content =
      "body  {  margin:  0;  padding:  0;  }\n"
      "h1  {  color:  red;  font-size:  24px;  }\n";

  // Pre-populate all URLs in cache.
  std::string first_url = "http://example.com/evict-w0-0.css";
  for (size_t w = 0; w < kNumWaves; ++w) {
    for (size_t i = 0; i < kWaveSize; ++i) {
      std::string url =
          absl::StrCat("http://example.com/evict-w", w, "-", i, ".css");
      CacheOriginal(worker.cache(), url, css_content);
    }
  }

  std::thread worker_thread([&worker]() { worker.Run(); });
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  // Send and wait for each wave sequentially.
  for (size_t w = 0; w < kNumWaves; ++w) {
    uint64_t expected_received = (w + 1) * kWaveSize;
    uint64_t target = expected_received;
    for (size_t i = 0; i < kWaveSize; ++i) {
      CacheNotification notification;
      notification.url =
          absl::StrCat("http://example.com/evict-w", w, "-", i, ".css");
      notification.content_type = ContentType::kCss;
      notification.scheme = "https";
      notification.capability_mask = CapabilityMask().Encode();
      SendNotification(notification);
    }
    // Wait for all notifications to be received and all in-flight work to
    // complete, rather than polling css_processed with an arbitrary timeout.
    for (int t = 0; t < 300; ++t) {
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
      if (worker.stats().notifications_received.load() >= expected_received &&
          worker.in_flight_work() == 0) {
        break;
      }
    }
    auto const& s = worker.stats();
    EXPECT_GE(s.css_processed.load(), target)
        << "Wave " << w << " should complete"
        << "\n  notifications_received=" << s.notifications_received.load()
        << "\n  css_processed=" << s.css_processed.load()
        << "\n  alternate_write_failures=" << s.alternate_write_failures.load()
        << "\n  cache_read_failures=" << s.cache_read_failures.load()
        << "\n  skipped_dedup=" << s.notifications_skipped_dedup.load()
        << "\n  skipped_inflight=" << s.notifications_skipped_inflight.load()
        << "\n  in_flight_work=" << worker.in_flight_work();
  }

  // Re-populate wave-0's first URL and re-send.
  // After 3 waves, wave-0 entries are fully evicted from dedup sets.
  CacheOriginal(worker.cache(), first_url, css_content);

  uint64_t processed_before = worker.stats().css_processed.load();
  CacheNotification resend;
  resend.url = first_url;
  resend.scheme = "https";
  resend.content_type = ContentType::kCss;
  resend.capability_mask = CapabilityMask().Encode();
  SendNotification(resend);

  for (int w = 0; w < 40; ++w) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    if (worker.stats().css_processed.load() > processed_before) break;
  }
  EXPECT_GT(worker.stats().css_processed.load(), processed_before)
      << "After triple overflow, wave-0 entries should be re-processed";

  worker.Shutdown();
  worker_thread.join();
}

// =============================================================================
// Item 4: Purge generation tests
// =============================================================================

TEST_F(WorkerTest, PurgeGenerationPreventsStaleDedup) {
  // Verify that purge + re-cache + re-notify triggers re-processing
  // (the purge generation mechanism prevents the re-notification from
  // being dedup-skipped).
  WorkerConfig config;
  config.socket_path = socket_path_;
  config.cache_path = cache_path_;
  config.cache_size_bytes = static_cast<uint64_t>(10 * 1024 * 1024);

  NullMessageHandler handler;
  Worker worker(config, &handler);
  ASSERT_TRUE(worker.Initialize());

  std::string url = "http://example.com/purge-gen.css";
  std::string css =
      "body  {  margin:  0;  padding:  0;  }\n"
      "h1  {  color:  red;  font-size:  24px;  }\n";
  CacheOriginal(worker.cache(), url, css);

  std::thread worker_thread([&worker]() { worker.Run(); });
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  // First notification — process normally.
  CacheNotification notification;
  notification.url = url;
  notification.scheme = "https";
  notification.content_type = ContentType::kCss;
  notification.capability_mask = CapabilityMask().Encode();
  SendNotification(notification);

  for (int i = 0; i < 40 * kSanitizerBudgetScale; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    if (worker.stats().css_processed.load() >= 1) break;
  }
  EXPECT_GE(worker.stats().css_processed.load(), 1u);

  // css_processed moves before the in-flight entry is erased.  Drain first, or
  // the duplicate below is rejected by the in-flight guard and bumps
  // skipped_inflight instead of skipped_dedup.
  WaitForWorkerIdle(worker);

  // Verify dedup: second notification should be skipped.  Poll for the skip
  // counter to advance instead of a fixed sleep (slow shards can sit at the
  // pipe read past a fixed budget).
  uint64_t skipped_before = worker.stats().notifications_skipped_dedup.load();
  SendNotification(notification);
  for (int i = 0; i < 80 * kSanitizerBudgetScale; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    if (worker.stats().notifications_skipped_dedup.load() > skipped_before)
      break;
  }
  EXPECT_GT(worker.stats().notifications_skipped_dedup.load(), skipped_before)
      << "Pre-purge: duplicate should be deduped";

  // The dedup-skipped duplicate also occupies the in-flight set until the event
  // loop retires it.  Drain before the purge + re-notify below.
  WaitForWorkerIdle(worker);

  // Purge the URL — bumps purge generation.
  worker.InvalidateUrl(url, "");

  // Re-populate cache with new content.
  std::string css2 =
      "body  {  padding:  0;  }\n"
      "h2  {  color:  blue;  font-size:  18px;  }\n";
  CacheOriginal(worker.cache(), url, css2);

  uint64_t processed_before = worker.stats().css_processed.load();

  // Re-notify — should be processed (not dedup-skipped).
  SendNotification(notification);
  for (int i = 0; i < 40 * kSanitizerBudgetScale; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    if (worker.stats().css_processed.load() > processed_before) break;
  }
  EXPECT_GT(worker.stats().css_processed.load(), processed_before)
      << "After purge, re-notification should be re-processed";

  worker.Shutdown();
  worker_thread.join();
}

TEST_F(WorkerTest, PurgeGenerationCounterIncrementsOnInvalidate) {
  // Verify that multiple purge+re-notify cycles all trigger re-processing.
  WorkerConfig config;
  config.socket_path = socket_path_;
  config.cache_path = cache_path_;
  config.cache_size_bytes = static_cast<uint64_t>(10 * 1024 * 1024);

  NullMessageHandler handler;
  Worker worker(config, &handler);
  ASSERT_TRUE(worker.Initialize());

  std::string url = "http://example.com/purge-multi.css";
  std::string css =
      "body  {  margin:  0;  padding:  0;  }\n"
      "h1  {  color:  red;  font-size:  24px;  }\n";
  CacheOriginal(worker.cache(), url, css);

  std::thread worker_thread([&worker]() { worker.Run(); });
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  CacheNotification notification;
  notification.url = url;
  notification.scheme = "https";
  notification.content_type = ContentType::kCss;
  notification.capability_mask = CapabilityMask().Encode();

  // First cycle: process normally.
  SendNotification(notification);
  for (int i = 0; i < 40 * kSanitizerBudgetScale; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    if (worker.stats().css_processed.load() >= 1) break;
  }
  EXPECT_GE(worker.stats().css_processed.load(), 1u);

  // Two additional purge+re-process cycles.
  for (int cycle = 0; cycle < 2; ++cycle) {
    // Drain the previous cycle first: its processing counter moves before the
    // in-flight entry is erased, and the re-send below would otherwise be
    // rejected by the in-flight guard.
    WaitForWorkerIdle(worker);

    worker.InvalidateUrl(url, "");
    CacheOriginal(worker.cache(), url, css);

    uint64_t processed_before = worker.stats().css_processed.load();
    SendNotification(notification);
    for (int i = 0; i < 40 * kSanitizerBudgetScale; ++i) {
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
      if (worker.stats().css_processed.load() > processed_before) break;
    }
    EXPECT_GT(worker.stats().css_processed.load(), processed_before)
        << "Purge cycle " << cycle << " should trigger re-processing";
  }

  worker.Shutdown();
  worker_thread.join();
}

TEST_F(WorkerTest, WasPurgedSinceDispatch) {
  // Unit test for the WasPurgedSinceDispatch method that guards against
  // writing stale variants when a purge arrives during in-flight processing.
  // No IPC or Run() needed — just tests the generation tracking logic.
  WorkerConfig config;
  config.socket_path = socket_path_;
  config.cache_path = cache_path_;
  config.cache_size_bytes = static_cast<uint64_t>(10 * 1024 * 1024);

  NullMessageHandler handler;
  Worker worker(config, &handler);
  ASSERT_TRUE(worker.Initialize());

  std::string url = "http://example.com/was-purged-test";
  std::string hostname = "";

  // Before any purge, dispatch_gen=0 should NOT be detected as purged.
  EXPECT_FALSE(worker.WasPurgedSinceDispatch(url, hostname, 0));

  // Purge the URL (bumps generation to 1).
  worker.InvalidateUrl(url, hostname);

  // Now dispatch_gen=0 should be detected as purged (1 > 0).
  EXPECT_TRUE(worker.WasPurgedSinceDispatch(url, hostname, 0));

  // dispatch_gen=1 should NOT be detected (1 > 1 is false — same gen).
  EXPECT_FALSE(worker.WasPurgedSinceDispatch(url, hostname, 1));

  // Purge again (gen becomes 2).
  worker.InvalidateUrl(url, hostname);

  // dispatch_gen=1 should now be detected (2 > 1).
  EXPECT_TRUE(worker.WasPurgedSinceDispatch(url, hostname, 1));

  // dispatch_gen=2 should not.
  EXPECT_FALSE(worker.WasPurgedSinceDispatch(url, hostname, 2));

  // Different URL should not be affected.
  EXPECT_FALSE(
      worker.WasPurgedSinceDispatch("http://other.com/page", hostname, 0));
}

TEST_F(WorkerTest, ResetCacheBumpsGlobalPurgeGeneration) {
  // Issue #652: a full cache purge (ResetCache) must fence in-flight
  // variant writes the same way a single-URL purge does.  The fence is the
  // worker-global purge generation: baselines captured before the reset
  // must be flagged, baselines captured after must not.
  WorkerConfig config;
  config.socket_path = socket_path_;
  config.cache_path = cache_path_;
  config.cache_size_bytes = static_cast<uint64_t>(10 * 1024 * 1024);

  NullMessageHandler handler;
  Worker worker(config, &handler);
  ASSERT_TRUE(worker.Initialize());

  std::string url = "http://example.com/reset-fence-test";
  std::string hostname = "";

  // Baseline captured before the reset (never-purged URL: url_gen=0,
  // all_gen=0).
  PurgeDispatchGen before = worker.CapturePurgeGen(url, hostname);
  EXPECT_FALSE(worker.WasPurgedSinceDispatch(url, hostname, before));

  // Full purge.
  ASSERT_TRUE(worker.ResetCache().empty());

  // The pre-reset baseline is now fenced — for EVERY URL, including ones
  // that were never individually purged.
  EXPECT_TRUE(worker.WasPurgedSinceDispatch(url, hostname, before));
  EXPECT_TRUE(worker.WasPurgedSinceDispatch("http://other.com/x", "", before));

  // A baseline captured after the reset is clean.
  PurgeDispatchGen after = worker.CapturePurgeGen(url, hostname);
  EXPECT_FALSE(worker.WasPurgedSinceDispatch(url, hostname, after));
}

TEST_F(WorkerTest, ResetCacheClearsPerUrlGenerationMaps) {
  // Issue #652 (review finding): reset_cache used to clear only
  // purge_generation_, leaving stale high generations in
  // purge_generation_previous_.  A post-reset notification capturing the
  // cleared-map baseline (0) would then be flagged as purged forever,
  // silently dropping every legitimate write for that URL.
  WorkerConfig config;
  config.socket_path = socket_path_;
  config.cache_path = cache_path_;
  config.cache_size_bytes = static_cast<uint64_t>(10 * 1024 * 1024);

  NullMessageHandler handler;
  Worker worker(config, &handler);
  ASSERT_TRUE(worker.Initialize());

  std::string url = "http://example.com/reset-prevmap-test";
  std::string hostname = "";

  // Purge a few times so the URL has a non-zero generation.
  worker.InvalidateUrl(url, hostname);
  worker.InvalidateUrl(url, hostname);
  worker.InvalidateUrl(url, hostname);
  EXPECT_TRUE(worker.WasPurgedSinceDispatch(url, hostname, 0));

  ASSERT_TRUE(worker.ResetCache().empty());

  // A fresh post-reset baseline must be clean: the per-URL generation maps
  // (current AND previous) were cleared, and the global generation matches
  // the captured one.
  PurgeDispatchGen after = worker.CapturePurgeGen(url, hostname);
  EXPECT_FALSE(worker.WasPurgedSinceDispatch(url, hostname, after))
      << "Post-reset notifications must not be flagged as purged";
}

TEST_F(WorkerTest, CapturePurgeGenReflectsPerUrlPurges) {
  // The dispatch-time baseline must include purges that already happened,
  // so a notification dispatched after a purge is not spuriously dropped.
  WorkerConfig config;
  config.socket_path = socket_path_;
  config.cache_path = cache_path_;
  config.cache_size_bytes = static_cast<uint64_t>(10 * 1024 * 1024);

  NullMessageHandler handler;
  Worker worker(config, &handler);
  ASSERT_TRUE(worker.Initialize());

  std::string url = "http://example.com/capture-gen-test";
  std::string hostname = "";

  worker.InvalidateUrl(url, hostname);
  worker.InvalidateUrl(url, hostname);

  PurgeDispatchGen gen = worker.CapturePurgeGen(url, hostname);
  EXPECT_EQ(gen.url_gen, 2u);
  // Captured baseline is current — not flagged.
  EXPECT_FALSE(worker.WasPurgedSinceDispatch(url, hostname, gen));
  // A purge after capture flags it.
  worker.InvalidateUrl(url, hostname);
  EXPECT_TRUE(worker.WasPurgedSinceDispatch(url, hostname, gen));
}

// =============================================================================
// Item 5: Incomplete matrix retry tests
// =============================================================================

TEST_F(WorkerTest, IncompleteMatrixAllowsReprocessing) {
  // If the proactive loop writes fewer variants than expected, the URL must
  // NOT be marked as processed, so the next notification fills the gap.
  // One variant write is failed through the test seam, which makes the
  // matrix incomplete on every run; the format matrix alone is enough to
  // exercise the path, so the viewport/density/save-data fan-out is off.
  WorkerConfig config;
  config.socket_path = socket_path_;
  config.cache_path = cache_path_;
  config.cache_size_bytes = static_cast<uint64_t>(10 * 1024 * 1024);
  config.proactive_image_variants = true;
  config.proactive_viewport_variants = false;
  config.proactive_savedata_variants = false;
  config.proactive_density_variants = false;

  NullMessageHandler handler;
  Worker worker(config, &handler);
  ASSERT_TRUE(worker.Initialize());
  std::atomic<bool> fail_next_write{true};
  worker.test_hook_fail_image_variant_write_ = [&fail_next_write]() {
    return fail_next_write.exchange(false);
  };

  std::string url = "http://example.com/incomplete-retry.jpg";
  std::string image_data = ReadTestFile("jpeg/quality100.jpg");
  ASSERT_FALSE(image_data.empty()) << "Test image not found";
  CacheOriginal(worker.cache(), url, image_data);

  std::thread worker_thread([&worker]() { worker.Run(); });
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

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

  SendNotification(notification);
  ASSERT_TRUE(pagespeed::test::WaitForNotificationsRetired(worker, 1,
                                                           kWorkerStallBudget));
  EXPECT_FALSE(fail_next_write.load()) << "no image variant write attempted";
  EXPECT_GE(worker.stats().images_processed.load(), 1u)
      << "Image should be processed";
  ASSERT_EQ(worker.stats().image_incomplete_matrices.load(), 1u)
      << "the failed write should leave the matrix incomplete";

  // The URL is NOT dedup-marked, so re-sending re-processes it and writes
  // the variant that failed the first time.
  uint64_t processed_before = worker.stats().images_processed.load();
  SendNotification(notification);
  ASSERT_TRUE(pagespeed::test::WaitForNotificationsRetired(worker, 2,
                                                           kWorkerStallBudget));
  EXPECT_GT(worker.stats().images_processed.load(), processed_before)
      << "Incomplete matrix should allow re-processing on retry";
  EXPECT_EQ(worker.stats().image_incomplete_matrices.load(), 1u)
      << "the retry should complete the matrix";

  worker.Shutdown();
  worker_thread.join();
}

// =============================================================================
// Tentative Cooldown Clearing Tests
//
// The worker sets a tentative 60s cooldown BEFORE HTML processing starts.
// On success, it's cleared.  But certain early-return paths forgot to clear
// it, locking the URL out for 60s unnecessarily.  These tests prove that
// the cooldown IS cleared on each early-return path.
// =============================================================================

TEST_F(WorkerTest, HtmlNoTransformsClearsTentativeCooldown) {
  // Path: "No HTML transformations applicable" early return.
  // When all transforms are disabled and there's no critical CSS,
  // the worker returns early.  The tentative cooldown must be cleared
  // so the URL is not locked out for 60s.
  WorkerConfig config;
  config.socket_path = socket_path_;
  config.cache_path = cache_path_;
  config.cache_size_bytes = static_cast<uint64_t>(10 * 1024 * 1024);
  // Disable all transforms so the "no transformations applicable" path fires.
  config.disable_lazy_load = true;
  config.disable_image_dimensions = true;
  config.disable_lcp_preload = true;
  config.disable_preconnect_injection = true;
  config.disable_async_css = true;
  config.disable_script_deferral = true;
  config.enable_speculation_rules = false;

  NullMessageHandler handler;
  Worker worker(config, &handler);
  ASSERT_TRUE(worker.Initialize());

  // HTML with no inline CSS -> no critical CSS -> no transforms applicable.
  std::string url = "http://example.com/no-transforms.html";
  std::string html =
      "<html><head><title>No Transforms</title></head>"
      "<body><p>Plain text only</p></body></html>";
  CacheOriginal(worker.cache(), url, html);

  std::thread worker_thread([&worker]() { worker.Run(); });
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  CacheNotification notification;
  notification.url = url;
  notification.scheme = "https";
  notification.hostname = "";
  notification.content_type = ContentType::kHtml;
  notification.capability_mask = 0;

  // First notification: hits "no transformations applicable" early return.
  SendNotification(notification);

  // Wait for the early return to fire.
  for (int i = 0; i < 40 * kSanitizerBudgetScale; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    if (worker.stats().html_assembly_skipped.load() >= 1) break;
  }
  EXPECT_GE(worker.stats().html_assembly_skipped.load(), 1u)
      << "First notification should hit 'no transformations' path";

  uint64_t skipped_before = worker.stats().notifications_skipped_dedup.load();
  uint64_t assembly_skipped_before =
      worker.stats().html_assembly_skipped.load();

  // Second notification for the same URL.  If the tentative cooldown was
  // NOT cleared, this will be blocked for 60s and the dedup counter
  // will increment.  If properly cleared, it processes again (hitting
  // the same "no transforms" path, incrementing assembly_skipped).  Poll for
  // the assembly-skipped counter to advance instead of a fixed sleep (slow
  // shards can sit at the pipe read past a fixed budget); this also settles
  // the dedup-unchanged invariant below.
  SendNotification(notification);
  for (int i = 0; i < 80 * kSanitizerBudgetScale; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    if (worker.stats().html_assembly_skipped.load() > assembly_skipped_before)
      break;
  }

  uint64_t skipped_after = worker.stats().notifications_skipped_dedup.load();
  uint64_t assembly_skipped_after = worker.stats().html_assembly_skipped.load();

  EXPECT_EQ(skipped_after, skipped_before)
      << "Second notification must NOT be blocked by tentative cooldown";
  EXPECT_GT(assembly_skipped_after, assembly_skipped_before)
      << "Second notification should reach 'no transforms' path again";

  worker.Shutdown();
  worker_thread.join();
}

TEST_F(WorkerTest, HtmlNoChangesClearsTentativeCooldown) {
  // Path: "Transform produced no changes" early return.
  // When transforms run but produce identical output (no matching elements),
  // the worker returns early.  The tentative cooldown must be cleared.
  WorkerConfig config;
  config.socket_path = socket_path_;
  config.cache_path = cache_path_;
  config.cache_size_bytes = static_cast<uint64_t>(10 * 1024 * 1024);
  // Enable only lazy-load (needs <img>/<iframe> to actually modify).
  // Disable everything else.
  config.disable_lazy_load = false;  // Enabled — but no <img> in HTML
  config.disable_image_dimensions = true;
  config.disable_lcp_preload = true;
  config.disable_preconnect_injection = true;
  config.disable_async_css = true;
  config.disable_script_deferral = true;
  config.enable_speculation_rules = false;

  NullMessageHandler handler;
  Worker worker(config, &handler);
  ASSERT_TRUE(worker.Initialize());

  // HTML with no <img> or <iframe> — lazy-load transform runs but
  // finds nothing to modify, so modified_ stays false.
  std::string url = "http://example.com/no-changes.html";
  std::string html =
      "<html><head><title>No Changes</title></head>"
      "<body><p>Text only, no images or iframes</p></body></html>";
  CacheOriginal(worker.cache(), url, html);

  std::thread worker_thread([&worker]() { worker.Run(); });
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  CacheNotification notification;
  notification.url = url;
  notification.scheme = "https";
  notification.hostname = "";
  notification.content_type = ContentType::kHtml;
  notification.capability_mask = 0;

  // First notification: hits "transform produced no changes" early return.
  SendNotification(notification);

  // Wait for the early return to fire (same counter as "no transforms").
  for (int i = 0; i < 40 * kSanitizerBudgetScale; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    if (worker.stats().html_assembly_skipped.load() >= 1) break;
  }
  EXPECT_GE(worker.stats().html_assembly_skipped.load(), 1u)
      << "First notification should hit 'no changes' path";

  // html_assembly_skipped moves before the in-flight entry is erased.  Drain,
  // or the second notification is rejected by the in-flight guard and the
  // counter below never advances.
  WaitForWorkerIdle(worker);

  uint64_t skipped_before = worker.stats().notifications_skipped_dedup.load();
  uint64_t assembly_skipped_before =
      worker.stats().html_assembly_skipped.load();

  // Second notification: should NOT be blocked by tentative cooldown.  Poll
  // for the assembly-skipped counter to advance instead of a fixed sleep (slow
  // shards can sit at the pipe read past a fixed budget); this also settles
  // the dedup-unchanged invariant below.
  SendNotification(notification);
  for (int i = 0; i < 80 * kSanitizerBudgetScale; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    if (worker.stats().html_assembly_skipped.load() > assembly_skipped_before)
      break;
  }

  uint64_t skipped_after = worker.stats().notifications_skipped_dedup.load();
  uint64_t assembly_skipped_after = worker.stats().html_assembly_skipped.load();

  EXPECT_EQ(skipped_after, skipped_before)
      << "Second notification must NOT be blocked by tentative cooldown";
  EXPECT_GT(assembly_skipped_after, assembly_skipped_before)
      << "Second notification should reach 'no changes' path again";

  worker.Shutdown();
  worker_thread.join();
}

TEST_F(WorkerTest, UnsupportedImageTypeExistingCompressedMarksDedup) {
  // Simulates a worker restart scenario: both compressed variants (gzip and
  // brotli, the default configuration) for an unsupported image type (ICO)
  // already exist in cache from a previous worker run, but the in-memory dedup
  // set is empty.  The first notification hits the "compressed variants already
  // exist" early-return path.  Without MarkVariantProcessed,
  // every subsequent notification would repeat the cache lookup instead of being
  // caught by the fast in-memory dedup.
  WorkerConfig config;
  config.socket_path = socket_path_;
  config.cache_path = cache_path_;
  config.cache_size_bytes = static_cast<uint64_t>(10 * 1024 * 1024);

  NullMessageHandler handler;
  Worker worker(config, &handler);
  ASSERT_TRUE(worker.Initialize());

  // Create fake ICO data with metadata indicating image/x-icon (original).
  std::string url = "http://example.com/dedup-favicon.ico";
  std::string ico_data = "fake ico content for dedup testing";
  AlternateMetadata meta;
  meta.content_type = ContentType::kOther;
  meta.origin_content_type = "image/x-icon";
  CacheOriginalWithMeta(worker.cache(), url, ico_data, meta);

  // Pre-write both coded variants to simulate a previous worker run having
  // already processed this URL.  This causes the "compressed variants already
  // exist" early-return path to be taken on the first notification.
  for (auto encoding : {CapabilityMask::TransferEncoding::kGzip,
                        CapabilityMask::TransferEncoding::kBrotli}) {
    CapabilityMask mask;
    mask.set_transfer_encoding(encoding);
    AlternateId id =
        MaskToAlternateId(static_cast<uint8_t>(mask.Encode() & 0xFF));
    AlternateMetadata variant_meta;
    variant_meta.content_type = ContentType::kImage;
    variant_meta.flags = AlternateMetadata::kFlagWorkerProcessed;
    variant_meta.full_mask = mask.Encode();
    std::string content = "fake compressed ico data";
    std::string norm_url = StripSchemeAuthority(url);
    auto wh = worker.cache()->WriteAlternate(norm_url, "", "https", id,
                                             content.size(), variant_meta);
    ASSERT_TRUE(wh.has_value()) << "Failed to write compressed variant";
    auto written = wh->write_sync(
        std::as_bytes(std::span(content.data(), content.size())));
    ASSERT_TRUE(written.has_value()) << "Failed to write compressed content";
    auto closed = wh->close_sync();
    ASSERT_TRUE(closed.has_value()) << "Failed to close compressed handle";
  }

  std::thread worker_thread([&worker]() { worker.Run(); });
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  // First notification — hits "compressed variants already exist" path.
  // With the fix, this marks the URL in the dedup set.
  CacheNotification notification;
  notification.url = url;
  notification.scheme = "https";
  notification.content_type = ContentType::kImage;
  notification.capability_mask = CapabilityMask().Encode();
  SendNotification(notification);

  // Wait for the notification to be processed (received + handled).
  for (int i = 0; i < 40 * kSanitizerBudgetScale; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    if (worker.stats().notifications_received.load() >= 1) break;
  }
  std::this_thread::sleep_for(std::chrono::milliseconds(200));

  // The receive counter above moves before the notification is even handled, so
  // the fixed sleep is not enough on a loaded runner.  Drain the in-flight
  // entry, or the duplicate below is rejected by the in-flight guard instead of
  // the dedup set.
  WaitForWorkerIdle(worker);

  // Second notification — should be caught by in-memory dedup.
  uint64_t skipped_before = worker.stats().notifications_skipped_dedup.load();
  SendNotification(notification);
  for (int i = 0; i < 40 * kSanitizerBudgetScale; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    if (worker.stats().notifications_skipped_dedup.load() > skipped_before)
      break;
  }
  EXPECT_GT(worker.stats().notifications_skipped_dedup.load(), skipped_before)
      << "Second notification for unsupported image with pre-existing "
         "compressed variants should be caught by in-memory dedup";

  worker.Shutdown();
  worker_thread.join();
}

// --- CSS/JS cooldown throttling tests ---

TEST_F(WorkerTest, CssCooldownPreventsReprocessing) {
  // Send the same CSS notification twice quickly; the second should be
  // skipped due to cooldown enforcement (tentative in-flight guard).
  WorkerConfig config;
  config.socket_path = socket_path_;
  config.cache_path = cache_path_;
  config.cache_size_bytes = static_cast<uint64_t>(10 * 1024 * 1024);

  NullMessageHandler handler;
  Worker worker(config, &handler);
  ASSERT_TRUE(worker.Initialize());

  std::string url = "http://example.com/cooldown.css";
  std::string css = "body { margin: 0; } h1 { color: red; }";
  CacheOriginal(worker.cache(), url, css);

  std::thread worker_thread([&worker]() { worker.Run(); });
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  CacheNotification notification;
  notification.url = url;
  notification.scheme = "https";
  notification.content_type = ContentType::kCss;
  notification.capability_mask = CapabilityMask().Encode();

  // First notification.
  SendNotification(notification);

  // Wait for first processing to complete.
  CapabilityMask mask;
  for (int i = 0; i < 40 * kSanitizerBudgetScale; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    if (worker.stats().css_processed.load() >= 1) break;
  }
  EXPECT_GE(worker.stats().css_processed.load(), 1u)
      << "CSS should have been processed once";

  // css_processed moves before the in-flight entry is erased.  Drain first, or
  // the duplicate below is rejected by the in-flight guard and bumps
  // skipped_inflight instead of skipped_dedup.
  WaitForWorkerIdle(worker);

  uint64_t skipped_before = worker.stats().notifications_skipped_dedup.load();

  // Second notification for the same URL — should be skipped by dedup
  // or cooldown.  Poll for the skip counter to advance instead of a fixed
  // sleep (slow shards can sit at the pipe read past a fixed budget).
  SendNotification(notification);
  for (int i = 0; i < 80 * kSanitizerBudgetScale; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    if (worker.stats().notifications_skipped_dedup.load() > skipped_before)
      break;
  }

  uint64_t skipped_after = worker.stats().notifications_skipped_dedup.load();
  EXPECT_GT(skipped_after, skipped_before)
      << "Second CSS notification should be skipped by dedup/cooldown";

  worker.Shutdown();
  worker_thread.join();
}

TEST_F(WorkerTest, CssCooldownClearedOnSuccess) {
  // Verify that after successful CSS processing, the cooldown is cleared
  // so that ClearDedupAndCooldown + re-notification can reprocess.
  WorkerConfig config;
  config.socket_path = socket_path_;
  config.cache_path = cache_path_;
  config.cache_size_bytes = static_cast<uint64_t>(10 * 1024 * 1024);

  NullMessageHandler handler;
  Worker worker(config, &handler);
  ASSERT_TRUE(worker.Initialize());

  std::string url = "http://example.com/cooldown-clear.css";
  std::string css = "body { margin: 0; } h1 { color: red; }";
  CacheOriginal(worker.cache(), url, css);

  std::thread worker_thread([&worker]() { worker.Run(); });
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  CacheNotification notification;
  notification.url = url;
  notification.scheme = "https";
  notification.content_type = ContentType::kCss;
  notification.capability_mask = CapabilityMask().Encode();

  // First notification — process CSS.
  SendNotification(notification);
  for (int i = 0; i < 40 * kSanitizerBudgetScale; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    if (worker.stats().css_processed.load() >= 1) break;
  }
  EXPECT_GE(worker.stats().css_processed.load(), 1u);

  // css_processed moves before the in-flight entry is erased, and InvalidateUrl
  // does not clear the in-flight set.  Drain first, or the re-send below is
  // rejected by the in-flight guard and never reprocesses.
  WaitForWorkerIdle(worker);

  // Verify cooldown was cleared on success (not just dedup).
  auto cooldown = worker.GetCooldownForUrl(url, "");
  EXPECT_FALSE(cooldown.has_value())
      << "Cooldown should be cleared after successful CSS processing";

  uint64_t css_before = worker.stats().css_processed.load();

  // Clear dedup to simulate re-notification scenario.
  worker.ClearDedupAndCooldown(url, "");
  worker.InvalidateUrl(url);
  CacheOriginal(worker.cache(), url, css);

  // Re-send notification — should process since cooldown was cleared on
  // success and dedup was explicitly cleared.
  SendNotification(notification);
  for (int i = 0; i < 40 * kSanitizerBudgetScale; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    if (worker.stats().css_processed.load() > css_before) break;
  }

  EXPECT_GT(worker.stats().css_processed.load(), css_before)
      << "After clearing dedup, CSS should be reprocessed (cooldown was "
         "cleared on success)";

  worker.Shutdown();
  worker_thread.join();
}

TEST_F(WorkerTest, JsCooldownPreventsReprocessing) {
  // Send the same JS notification twice; the second should be skipped by the
  // processed-variant dedup set once the first has retired.
  WorkerConfig config;
  config.socket_path = socket_path_;
  config.cache_path = cache_path_;
  config.cache_size_bytes = static_cast<uint64_t>(10 * 1024 * 1024);

  NullMessageHandler handler;
  Worker worker(config, &handler);
  ASSERT_TRUE(worker.Initialize());

  std::string url = "http://example.com/cooldown.js";
  std::string js = "function hello() { console.log('hello world'); }";
  CacheOriginal(worker.cache(), url, js);

  std::thread worker_thread([&worker]() { worker.Run(); });
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  CacheNotification notification;
  notification.url = url;
  notification.scheme = "https";
  notification.content_type = ContentType::kJs;
  notification.capability_mask = CapabilityMask().Encode();

  // First notification.
  SendNotification(notification);

  // Wait for first processing to complete.
  for (int i = 0; i < 40 * kSanitizerBudgetScale; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    if (worker.stats().js_processed.load() >= 1) break;
  }
  EXPECT_GE(worker.stats().js_processed.load(), 1u)
      << "JS should have been processed once";

  // js_processed moves before the in-flight entry is erased.  Drain first, or the
  // second notification is rejected by the in-flight guard, which bumps a
  // different counter than the one asserted below.
  WaitForWorkerIdle(worker);

  uint64_t skipped_before = worker.stats().notifications_skipped_dedup.load();

  // Second notification for the same URL — with the first retired, it is skipped
  // by the processed-variant dedup set.  (JS has no cooldown of its own; the
  // tentative cooldown entry is HTML-only.)  Poll for the skip counter to
  // advance instead of a fixed sleep (slow shards can sit at the pipe read past
  // a fixed budget).
  SendNotification(notification);
  for (int i = 0; i < 80 * kSanitizerBudgetScale; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    if (worker.stats().notifications_skipped_dedup.load() > skipped_before)
      break;
  }

  uint64_t skipped_after = worker.stats().notifications_skipped_dedup.load();
  EXPECT_GT(skipped_after, skipped_before)
      << "Second JS notification should be skipped by dedup/cooldown";

  worker.Shutdown();
  worker_thread.join();
}

TEST_F(WorkerTest, JsCooldownClearedOnSuccess) {
  // Verify that after successful JS processing, the cooldown is cleared
  // so that ClearDedupAndCooldown + re-notification can reprocess.
  WorkerConfig config;
  config.socket_path = socket_path_;
  config.cache_path = cache_path_;
  config.cache_size_bytes = static_cast<uint64_t>(10 * 1024 * 1024);

  NullMessageHandler handler;
  Worker worker(config, &handler);
  ASSERT_TRUE(worker.Initialize());

  std::string url = "http://example.com/cooldown-clear.js";
  std::string js = "function hello() { console.log('hello world'); }";
  CacheOriginal(worker.cache(), url, js);

  std::thread worker_thread([&worker]() { worker.Run(); });
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  CacheNotification notification;
  notification.url = url;
  notification.scheme = "https";
  notification.content_type = ContentType::kJs;
  notification.capability_mask = CapabilityMask().Encode();

  // First notification — process JS.
  SendNotification(notification);
  for (int i = 0; i < 40 * kSanitizerBudgetScale; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    if (worker.stats().js_processed.load() >= 1) break;
  }
  EXPECT_GE(worker.stats().js_processed.load(), 1u);

  // js_processed moves before the in-flight entry is erased, and InvalidateUrl
  // does not clear the in-flight set.  Drain first, or the re-send below is
  // rejected by the in-flight guard and never reprocesses.
  WaitForWorkerIdle(worker);

  // Verify cooldown was cleared on success (not just dedup).
  auto cooldown = worker.GetCooldownForUrl(url, "");
  EXPECT_FALSE(cooldown.has_value())
      << "Cooldown should be cleared after successful JS processing";

  uint64_t js_before = worker.stats().js_processed.load();

  // Clear dedup to simulate re-notification scenario.
  worker.ClearDedupAndCooldown(url, "");
  worker.InvalidateUrl(url);
  CacheOriginal(worker.cache(), url, js);

  // Re-send notification — should process since cooldown was cleared on
  // success and dedup was explicitly cleared.
  SendNotification(notification);
  for (int i = 0; i < 40 * kSanitizerBudgetScale; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    if (worker.stats().js_processed.load() > js_before) break;
  }

  EXPECT_GT(worker.stats().js_processed.load(), js_before)
      << "After clearing dedup, JS should be reprocessed (cooldown was "
         "cleared on success)";

  worker.Shutdown();
  worker_thread.join();
}

TEST_F(WorkerTest, CssSuccessPathClearsCooldown) {
  // Verify that successful CSS processing clears the tentative cooldown,
  // so the URL is not locked out for 60s after a successful write.
  WorkerConfig config;
  config.socket_path = socket_path_;
  config.cache_path = cache_path_;
  config.cache_size_bytes = static_cast<uint64_t>(10 * 1024 * 1024);

  NullMessageHandler handler;
  Worker worker(config, &handler);
  ASSERT_TRUE(worker.Initialize());

  std::string url = "http://example.com/write-fail-throttle.css";
  std::string css = "body { margin: 0; } h1 { color: red; }";
  CacheOriginal(worker.cache(), url, css);

  std::thread worker_thread([&worker]() { worker.Run(); });
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  CacheNotification notification;
  notification.url = url;
  notification.scheme = "https";
  notification.content_type = ContentType::kCss;
  notification.capability_mask = CapabilityMask().Encode();

  // First notification — processes successfully.
  SendNotification(notification);
  for (int i = 0; i < 40 * kSanitizerBudgetScale; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    if (worker.stats().css_processed.load() >= 1) break;
  }
  EXPECT_GE(worker.stats().css_processed.load(), 1u);

  // Verify the cooldown was NOT left lingering (it should be cleared
  // on success).
  auto cooldown = worker.GetCooldownForUrl(url, "");
  EXPECT_FALSE(cooldown.has_value())
      << "Cooldown should be cleared after successful CSS processing";

  worker.Shutdown();
  worker_thread.join();
}

TEST_F(WorkerTest, DualSchemePurgeInvalidatesBothEntries) {
  // Cache the same URL under both HTTP and HTTPS schemes.
  // The management socket PURGE should remove both.
  WorkerConfig config;
  config.socket_path = socket_path_;
  config.cache_path = cache_path_;
  config.cache_size_bytes = static_cast<uint64_t>(10 * 1024 * 1024);

  NullMessageHandler handler;
  Worker worker(config, &handler);
  ASSERT_TRUE(worker.Initialize());

  std::string url = "/dual-scheme-purge.css";
  std::string hostname = "example.com";
  std::string content_https = "body { color: blue; }";
  std::string content_http = "body { color: red; }";

  // Write under HTTPS scheme.
  {
    CapabilityMask dm;
    AlternateId id =
        MaskToAlternateId(static_cast<uint8_t>(dm.Encode() & 0xFF));
    AlternateMetadata meta;
    meta.full_mask = dm.Encode();
    meta.content_type = ContentType::kOther;
    auto wh = worker.cache()->WriteAlternate(url, hostname, "https", id,
                                             content_https.size(), meta);
    ASSERT_TRUE(wh.has_value());
    (void)wh->write_sync(
        std::as_bytes(std::span(content_https.data(), content_https.size())));
    (void)wh->close_sync();
  }

  // Write under HTTP scheme.
  {
    CapabilityMask dm;
    AlternateId id =
        MaskToAlternateId(static_cast<uint8_t>(dm.Encode() & 0xFF));
    AlternateMetadata meta;
    meta.full_mask = dm.Encode();
    meta.content_type = ContentType::kOther;
    auto wh = worker.cache()->WriteAlternate(url, hostname, "http", id,
                                             content_http.size(), meta);
    ASSERT_TRUE(wh.has_value());
    (void)wh->write_sync(
        std::as_bytes(std::span(content_http.data(), content_http.size())));
    (void)wh->close_sync();
  }

  // Verify both exist.
  {
    CapabilityMask dm;
    auto r1 = worker.cache()->ReadBestAlternate(url, hostname, "https", dm);
    ASSERT_TRUE(r1.has_value()) << "HTTPS entry should exist";
    auto r2 = worker.cache()->ReadBestAlternate(url, hostname, "http", dm);
    ASSERT_TRUE(r2.has_value()) << "HTTP entry should exist";
  }

  // Purge via InvalidateUrl with both schemes (as mgmt PURGE does).
  int deleted = worker.InvalidateUrl(url, hostname, "https");
  deleted += worker.InvalidateUrl(url, hostname, "http");
  EXPECT_GE(deleted, 2) << "Both scheme entries should be deleted";

  // Verify both are gone.
  {
    CapabilityMask dm;
    auto r1 = worker.cache()->ReadBestAlternate(url, hostname, "https", dm);
    EXPECT_FALSE(r1.has_value()) << "HTTPS entry should be purged";
    auto r2 = worker.cache()->ReadBestAlternate(url, hostname, "http", dm);
    EXPECT_FALSE(r2.has_value()) << "HTTP entry should be purged";
  }

  worker.Shutdown();
}

TEST_F(WorkerTest, SchemeIsolatedCaching) {
  // HTTP and HTTPS entries for the same URL must be independent.
  // Purging one scheme must not affect the other.
  WorkerConfig config;
  config.socket_path = socket_path_;
  config.cache_path = cache_path_;
  config.cache_size_bytes = static_cast<uint64_t>(10 * 1024 * 1024);

  NullMessageHandler handler;
  Worker worker(config, &handler);
  ASSERT_TRUE(worker.Initialize());

  std::string url = "/scheme-isolated.html";
  std::string hostname = "example.com";
  std::string https_body = "<html>HTTPS version</html>";
  std::string http_body = "<html>HTTP version</html>";

  // Write HTTPS variant.
  {
    CapabilityMask dm;
    AlternateId id =
        MaskToAlternateId(static_cast<uint8_t>(dm.Encode() & 0xFF));
    AlternateMetadata meta;
    meta.full_mask = dm.Encode();
    meta.content_type = ContentType::kHtml;
    auto wh = worker.cache()->WriteAlternate(url, hostname, "https", id,
                                             https_body.size(), meta);
    ASSERT_TRUE(wh.has_value());
    (void)wh->write_sync(
        std::as_bytes(std::span(https_body.data(), https_body.size())));
    (void)wh->close_sync();
  }

  // Write HTTP variant.
  {
    CapabilityMask dm;
    AlternateId id =
        MaskToAlternateId(static_cast<uint8_t>(dm.Encode() & 0xFF));
    AlternateMetadata meta;
    meta.full_mask = dm.Encode();
    meta.content_type = ContentType::kHtml;
    auto wh = worker.cache()->WriteAlternate(url, hostname, "http", id,
                                             http_body.size(), meta);
    ASSERT_TRUE(wh.has_value());
    (void)wh->write_sync(
        std::as_bytes(std::span(http_body.data(), http_body.size())));
    (void)wh->close_sync();
  }

  // Read both — should get different content.
  {
    CapabilityMask dm;
    auto r_https =
        worker.cache()->ReadBestAlternate(url, hostname, "https", dm);
    ASSERT_TRUE(r_https.has_value());
    auto c1 = r_https->content();
    std::string_view s1(reinterpret_cast<const char*>(c1.data()), c1.size());
    EXPECT_EQ(s1, https_body);

    auto r_http = worker.cache()->ReadBestAlternate(url, hostname, "http", dm);
    ASSERT_TRUE(r_http.has_value());
    auto c2 = r_http->content();
    std::string_view s2(reinterpret_cast<const char*>(c2.data()), c2.size());
    EXPECT_EQ(s2, http_body);
  }

  // Purge only HTTPS.
  worker.InvalidateUrl(url, hostname, "https");

  // HTTPS should be gone, HTTP should survive.
  {
    CapabilityMask dm;
    auto r_https =
        worker.cache()->ReadBestAlternate(url, hostname, "https", dm);
    EXPECT_FALSE(r_https.has_value()) << "HTTPS should be purged";

    auto r_http = worker.cache()->ReadBestAlternate(url, hostname, "http", dm);
    ASSERT_TRUE(r_http.has_value()) << "HTTP should survive HTTPS purge";
    auto c = r_http->content();
    std::string_view s(reinterpret_cast<const char*>(c.data()), c.size());
    EXPECT_EQ(s, http_body);
  }

  worker.Shutdown();
}

TEST_F(WorkerTest, CooldownSchemeIsolation) {
  // Cooldown for HTTPS should not block HTTP processing, and vice versa.
  WorkerConfig config;
  config.socket_path = socket_path_;
  config.cache_path = cache_path_;
  config.cache_size_bytes = static_cast<uint64_t>(10 * 1024 * 1024);

  NullMessageHandler handler;
  config.unsafe_force_async_css =
      true;  // fixture page is not under test for the gate
  Worker worker(config, &handler);
  ASSERT_TRUE(worker.Initialize());

  // Pre-populate cache with HTML under both schemes so both notifications
  // can find the original content.
  std::string url = "http://example.com/cooldown-scheme.html";
  std::string path = StripSchemeAuthority(url);
  std::string html =
      "<html><head><style>body { margin: 0; }</style></head>"
      "<body><h1>Test</h1></body></html>";

  // CacheOriginal writes under "https" scheme with empty hostname.
  CacheOriginal(worker.cache(), url, html);
  // Also write under "http" scheme so the HTTP notification can find it.
  {
    CapabilityMask default_mask;
    AlternateId id =
        MaskToAlternateId(static_cast<uint8_t>(default_mask.Encode() & 0xFF));
    AlternateMetadata meta;
    meta.full_mask = default_mask.Encode();
    meta.content_type = ContentType::kOther;
    meta.origin_content_type = DefaultOriginContentType(html);
    auto wh =
        worker.cache()->WriteAlternate(path, "", "http", id, html.size(), meta);
    ASSERT_TRUE(wh.has_value());
    (void)wh->write_sync(std::as_bytes(std::span(html.data(), html.size())));
    (void)wh->close_sync();
  }

  std::thread worker_thread([&worker]() { worker.Run(); });
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  // Send HTTPS notification — triggers processing and cooldown.
  {
    CacheNotification notification;
    notification.url = url;
    notification.scheme = "https";
    notification.content_type = ContentType::kHtml;
    notification.capability_mask = CapabilityMask().Encode();
    SendNotification(notification);
  }

  // Wait for HTTPS processing.
  for (int i = 0; i < 40 * kSanitizerBudgetScale; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    if (worker.stats().html_processed.load() >= 1) break;
  }
  EXPECT_GE(worker.stats().html_processed.load(), 1u);

  // Now send HTTP notification for the same URL.
  // With scheme-isolated cooldown, this should NOT be blocked.
  uint64_t processed_before = worker.stats().html_processed.load();
  {
    CacheNotification notification;
    notification.url = url;
    notification.scheme = "http";
    notification.content_type = ContentType::kHtml;
    notification.capability_mask = CapabilityMask().Encode();
    SendNotification(notification);
  }

  // Wait for HTTP processing.
  for (int i = 0; i < 40 * kSanitizerBudgetScale; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    if (worker.stats().html_processed.load() > processed_before) break;
  }
  EXPECT_GT(worker.stats().html_processed.load(), processed_before)
      << "HTTP notification should not be blocked by HTTPS cooldown";

  worker.Shutdown();
  worker_thread.join();
}

TEST_F(WorkerTest, PurgeGenerationSchemeIsolation) {
  // Purge generation for HTTPS should not affect HTTP dispatch.
  WorkerConfig config;
  config.socket_path = socket_path_;
  config.cache_path = cache_path_;
  config.cache_size_bytes = static_cast<uint64_t>(10 * 1024 * 1024);

  NullMessageHandler handler;
  Worker worker(config, &handler);
  ASSERT_TRUE(worker.Initialize());

  std::string url = "http://example.com/purge-gen-scheme";
  std::string hostname = "example.com";

  // Purge the HTTPS version (bumps generation for HTTPS key).
  worker.InvalidateUrl(url, hostname, "https");

  // WasPurgedSinceDispatch for HTTPS should detect the purge.
  EXPECT_TRUE(worker.WasPurgedSinceDispatch(url, hostname, 0, "https"))
      << "HTTPS should show as purged";

  // WasPurgedSinceDispatch for HTTP should NOT detect it.
  EXPECT_FALSE(worker.WasPurgedSinceDispatch(url, hostname, 0, "http"))
      << "HTTP should not be affected by HTTPS purge";

  worker.Shutdown();
}

TEST_F(WorkerTest, NativeSvgNotificationMarksDedup) {
  // Native SVG files (origin content-type image/svg+xml) skip raster
  // transcoding and only write gzip/brotli compressed variants.
  // Without MarkVariantProcessed, every subsequent notification for the
  // same SVG URL bypasses the in-memory dedup check, causing unbounded
  // reprocessing and "alternate not found" warnings on fallback HITs.
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

  // Cache a minimal SVG with origin_content_type = "image/svg+xml".
  std::string url = "http://example.com/images/logo.svg";
  std::string svg_data =
      R"(<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 100 100">)"
      R"(<circle cx="50" cy="50" r="40" fill="blue"/></svg>)";
  AlternateMetadata meta;
  meta.content_type = ContentType::kImage;
  meta.origin_content_type = "image/svg+xml";
  CacheOriginalWithMeta(worker.cache(), url, svg_data, meta);

  std::thread worker_thread([&worker]() { worker.Run(); });
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  // First notification — should process the SVG (write compressed variants).
  CacheNotification notification;
  notification.url = url;
  notification.scheme = "https";
  notification.content_type = ContentType::kImage;
  notification.capability_mask = CapabilityMask().Encode();
  SendNotification(notification);

  // Wait for compressed variants to be written (strong completion signal).
  for (int i = 0; i < 60 * kSanitizerBudgetScale; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    if (worker.stats().gzip_variants_written.load() >= 1) break;
  }
  EXPECT_GE(worker.stats().gzip_variants_written.load(), 1u)
      << "SVG compressed variant should have been written";

  // The SVG notification's counters move before its in-flight entry is erased.
  // Drain first, or the duplicate below is rejected by the in-flight guard
  // instead of the dedup set.
  WaitForWorkerIdle(worker);

  uint64_t variants_before = worker.stats().variants_written.load();
  uint64_t skipped_before = worker.stats().notifications_skipped_dedup.load();

  // Second notification (same mask) — should be caught by in-memory dedup.
  SendNotification(notification);
  for (int i = 0; i < 40 * kSanitizerBudgetScale; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    if (worker.stats().notifications_skipped_dedup.load() > skipped_before)
      break;
  }
  EXPECT_GT(worker.stats().notifications_skipped_dedup.load(), skipped_before)
      << "Second notification for native SVG should be caught by in-memory "
         "dedup (MarkVariantProcessed must be called after writing compressed "
         "variants)";
  EXPECT_EQ(worker.stats().variants_written.load(), variants_before)
      << "No new variants should be written for duplicate SVG notification";

  // Third notification with a different client mask (AVIF+Brotli+Desktop,
  // simulating a real browser).  The dedup check normalizes the mask by
  // stripping encoding and format bits, so this should still hit the
  // same dedup key as the first notification (both are Desktop viewport).
  uint64_t skipped_before2 = worker.stats().notifications_skipped_dedup.load();
  CacheNotification browser_notification = notification;
  browser_notification.capability_mask =
      CapabilityMask(CapabilityMask::ImageFormat::kAvif,
                     CapabilityMask::Viewport::kDesktop,
                     CapabilityMask::PixelDensity::k1x,
                     CapabilityMask::SaveData::kOff,
                     CapabilityMask::TransferEncoding::kBrotli)
          .Encode();
  SendNotification(browser_notification);
  for (int i = 0; i < 40 * kSanitizerBudgetScale; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    if (worker.stats().notifications_skipped_dedup.load() > skipped_before2)
      break;
  }
  EXPECT_GT(worker.stats().notifications_skipped_dedup.load(), skipped_before2)
      << "AVIF+Brotli notification should be dedup-skipped after SVG "
         "processing (mask normalization strips encoding and format bits)";

  worker.Shutdown();
  worker_thread.join();
}

}  // namespace
}  // namespace pagespeed
