// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

// Dedup, cooldown and heal tests
//
// - Processing cooldown and the dedup sets
// - Healing of lost variant families and optimized-copy records
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
#include <iostream>
#include <span>
#include <string>
#include <thread>
#include <vector>

#include "absl/strings/str_cat.h"
#include "gtest/gtest.h"
#include "lib/base/message_handler.h"
#include "lib/cache/cache.h"
#include "lib/classify/alternate_id.h"
#include "lib/classify/capability_mask.h"
#include "src/proto/worker_ipc.h"
#include "src/worker/browser_analysis_manager.h"
#include "src/worker/critical_css_extractor.h"
#include "src/worker/url_registry.h"
#include "src/worker/worker.h"
#include "test/test_util/worker_quiescence.h"

namespace pagespeed {
namespace {

// --- Cooldown enforcement tests ---

TEST_F(WorkerTest, HtmlCooldownPreventsReprocessing) {
  // Send the same HTML notification twice quickly; the second should be
  // skipped due to cooldown enforcement.
  WorkerConfig config;
  config.socket_path = socket_path_;
  config.cache_path = cache_path_;
  config.cache_size_bytes = static_cast<uint64_t>(10 * 1024 * 1024);

  NullMessageHandler handler;
  config.unsafe_force_async_css =
      true;  // fixture page is not under test for the gate
  Worker worker(config, &handler);
  ASSERT_TRUE(worker.Initialize());

  std::string url = "http://example.com/cooldown.html";
  std::string html =
      "<html><head><style>body { margin: 0; }</style></head>"
      "<body><h1>Cooldown Test</h1></body></html>";
  CacheOriginal(worker.cache(), url, html);

  std::thread worker_thread([&worker]() { worker.Run(); });
  WorkerStopper stopper(worker, worker_thread);
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  CacheNotification notification;
  notification.url = url;
  notification.scheme = "https";
  notification.content_type = ContentType::kHtml;
  notification.capability_mask = 0;

  // First notification.
  SendNotification(notification);

  // Wait for first processing to complete (generous timeout for slow
  // machines, e.g. arm64 under emulation).
  CapabilityMask mask = CapabilityMask::Decode(0);
  bool variant_found = false;
  for (int i = 0; i < 80 * kSanitizerBudgetScale; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    std::string variant = ReadVariant(worker.cache(), url, mask);
    if (!variant.empty()) {
      variant_found = true;
      break;
    }
  }
  ASSERT_TRUE(variant_found)
      << "First notification must produce a variant before testing dedup";

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
      << "Second HTML notification should be skipped by dedup/cooldown";
}

TEST_F(WorkerTest, CooldownExpiresAllowsReprocessing) {
  // Verify that after the cooldown period expires, the same URL can be
  // reprocessed.  We use ClearDedupAndCooldown() to simulate expiry
  // rather than waiting the full 60-second default cooldown.
  WorkerConfig config;
  config.socket_path = socket_path_;
  config.cache_path = cache_path_;
  config.cache_size_bytes = static_cast<uint64_t>(10 * 1024 * 1024);

  NullMessageHandler handler;
  config.unsafe_force_async_css =
      true;  // fixture page is not under test for the gate
  Worker worker(config, &handler);
  ASSERT_TRUE(worker.Initialize());

  std::string url = "http://example.com/cooldown-expire.html";
  std::string html =
      "<html><head><style>body { margin: 0; }</style></head>"
      "<body><h1>Expire Test</h1></body></html>";
  CacheOriginal(worker.cache(), url, html);

  std::thread worker_thread([&worker]() { worker.Run(); });
  WorkerStopper stopper(worker, worker_thread);
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  CacheNotification notification;
  notification.url = url;
  notification.scheme = "https";
  notification.content_type = ContentType::kHtml;
  notification.capability_mask = 0;

  // First notification.
  SendNotification(notification);
  CapabilityMask mask = CapabilityMask::Decode(0);
  bool variant_found = false;
  for (int i = 0; i < 80 * kSanitizerBudgetScale; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    if (!ReadVariant(worker.cache(), url, mask).empty()) {
      variant_found = true;
      break;
    }
  }
  ASSERT_TRUE(variant_found) << "First notification must produce a variant "
                                "before testing cooldown expiry";

  uint64_t html_before = worker.stats().html_processed.load();

  // Clear dedup and cooldown to simulate expiry.
  worker.ClearDedupAndCooldown(url, "");

  // Invalidate cached variant so worker must reprocess.
  worker.InvalidateUrl(url);
  CacheOriginal(worker.cache(), url, html);

  // Re-send notification — should be processed again.
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
      << "After clearing cooldown, HTML should be reprocessed";
}

TEST_F(WorkerTest, ClearDedupAndCooldownResetsState) {
  // Verify that ClearDedupAndCooldown() allows an already-processed
  // URL to be reprocessed.
  WorkerConfig config;
  config.socket_path = socket_path_;
  config.cache_path = cache_path_;
  config.cache_size_bytes = static_cast<uint64_t>(10 * 1024 * 1024);

  NullMessageHandler handler;
  Worker worker(config, &handler);
  ASSERT_TRUE(worker.Initialize());

  std::string url = "http://example.com/clear-dedup.css";
  std::string css = "body { margin: 0; } h1 { color: red; }";
  CacheOriginal(worker.cache(), url, css);

  std::thread worker_thread([&worker]() { worker.Run(); });
  WorkerStopper stopper(worker, worker_thread);
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  CacheNotification notification;
  notification.url = url;
  notification.scheme = "https";
  notification.content_type = ContentType::kCss;
  notification.capability_mask = CapabilityMask().Encode();

  // First notification — poll for completion instead of fixed sleep.
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

  // css_processed moves before the in-flight entry is erased.  Drain first, or
  // the duplicate below is rejected by the in-flight guard and bumps
  // skipped_inflight instead of skipped_dedup.
  WaitForWorkerIdle(worker);

  uint64_t css_before = worker.stats().css_processed.load();

  // Second notification without clearing — should be skipped.  Poll for the
  // skip counter to advance instead of a fixed sleep (slow shards can sit at
  // the pipe read past a fixed budget).
  SendNotification(notification);
  for (int i = 0; i < 80 * kSanitizerBudgetScale; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    if (worker.stats().notifications_skipped_dedup.load() >= 1u) break;
  }

  uint64_t skipped = worker.stats().notifications_skipped_dedup.load();
  EXPECT_GE(skipped, 1u) << "Duplicate CSS notification should be skipped";

  // The dedup-skipped duplicate above still occupies the in-flight set until
  // the event loop retires it.  Drain before invalidating and re-sending, or
  // the third notification is rejected by the in-flight guard and never
  // reprocesses.
  WaitForWorkerIdle(worker);

  // Clear dedup state.
  worker.ClearDedupAndCooldown(url, "");

  // Invalidate so the worker reprocesses from original.
  worker.InvalidateUrl(url);
  CacheOriginal(worker.cache(), url, css);

  // Third notification — should now be processed.
  SendNotification(notification);
  bool reprocessed = false;
  for (int i = 0; i < 80 * kSanitizerBudgetScale; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    if (worker.stats().css_processed.load() > css_before) {
      reprocessed = true;
      break;
    }
  }

  EXPECT_TRUE(reprocessed)
      << "After ClearDedupAndCooldown, CSS should be reprocessed";
}

// --- Dedup tracking tests ---

TEST_F(WorkerTest, DuplicateImageNotificationSkipped) {
  // Send the same image notification twice; verify only one variant
  // is actually written (second is dedup-skipped).
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

  std::string url = "http://example.com/dedup.jpg";
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

  // First notification — should be processed.  Wait for the full processing
  // cycle to complete (all variant writes + dedup registration).  We detect
  // completion by waiting for variants_written to stabilize.
  SendNotification(notification);
  bool image_processed = false;
  for (int i = 0; i < 80 * kSanitizerBudgetScale; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    if (worker.stats().images_processed.load() >= 1) {
      image_processed = true;
      break;
    }
  }
  ASSERT_TRUE(image_processed)
      << "Image should be processed once before testing dedup";
  // Wait for variants_written to stabilize — MarkVariantProcessed (dedup
  // registration) runs after all variant writes, so once the counter stops
  // changing the dedup key is guaranteed to be registered.
  {
    uint64_t prev = 0;
    for (int i = 0; i < 40 * kSanitizerBudgetScale; ++i) {
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
      uint64_t cur = worker.stats().variants_written.load();
      if (cur > 0 && cur == prev) break;
      prev = cur;
    }
  }

  uint64_t variants_before = worker.stats().variants_written.load();
  uint64_t skipped_before = worker.stats().notifications_skipped_dedup.load();

  // Second notification for the same URL+mask — should be dedup-skipped.
  // Poll for the skip counter to advance instead of a fixed sleep (slow
  // shards can sit at the pipe read past a fixed budget).  This also lets
  // variants_written settle for the no-new-variants invariant below.
  SendNotification(notification);
  for (int i = 0; i < 80 * kSanitizerBudgetScale; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    if (worker.stats().notifications_skipped_dedup.load() > skipped_before)
      break;
  }

  uint64_t skipped_after = worker.stats().notifications_skipped_dedup.load();
  EXPECT_GT(skipped_after, skipped_before)
      << "Duplicate image notification should be dedup-skipped";
  // variants_written should not have increased (or at most marginally
  // from compressed variants of the first processing).
  EXPECT_EQ(worker.stats().variants_written.load(), variants_before)
      << "No new variants should be written for duplicate notification";
}

TEST_F(WorkerTest, InvalidateUrlClearsDedup) {
  // After InvalidateUrl(), the same URL should be re-processed on
  // the next notification.
  WorkerConfig config;
  config.socket_path = socket_path_;
  config.cache_path = cache_path_;
  config.cache_size_bytes = static_cast<uint64_t>(10 * 1024 * 1024);

  NullMessageHandler handler;
  Worker worker(config, &handler);
  ASSERT_TRUE(worker.Initialize());

  std::string url = "http://example.com/invalidate-dedup.css";
  std::string css = "body { margin: 0; } h1 { color: blue; }";
  CacheOriginal(worker.cache(), url, css);

  std::thread worker_thread([&worker]() { worker.Run(); });
  WorkerStopper stopper(worker, worker_thread);
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  CacheNotification notification;
  notification.url = url;
  notification.scheme = "https";
  notification.content_type = ContentType::kCss;
  notification.capability_mask = CapabilityMask().Encode();

  // First notification — poll for completion.
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

  // css_processed moves before the in-flight entry is erased, and InvalidateUrl
  // does not clear the in-flight set.  Drain first, or the re-send below is
  // rejected by the in-flight guard and never reprocesses.
  WaitForWorkerIdle(worker);

  uint64_t css_before = worker.stats().css_processed.load();

  // Invalidate the URL — this clears dedup state and cache entries.
  worker.InvalidateUrl(url);

  // Re-populate the cache so the worker has data to process.
  CacheOriginal(worker.cache(), url, css);

  // Re-send notification — should be processed again.
  SendNotification(notification);
  bool reprocessed = false;
  for (int i = 0; i < 80 * kSanitizerBudgetScale; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    if (worker.stats().css_processed.load() > css_before) {
      reprocessed = true;
      break;
    }
  }

  EXPECT_TRUE(reprocessed) << "After InvalidateUrl, CSS should be reprocessed";
}

TEST_F(WorkerTest, DedupHealReprocessesAfterVariantFamilyLoss) {
  // a processed-set entry must not
  // outlive the variant family it stands for.  Process a CSS URL (variant
  // written, dedup marked), remove the WHOLE family through the cache API —
  // simulating a Cyclone capacity eviction, which has no callback into the
  // worker — re-seed the source as a durable original (sentinel class, so
  // the family still holds no servable variant), and re-notify.  The stale
  // dedup entry must be healed (erased + counted) and the URL reprocessed
  // instead of skipped.  Fails on the un-fixed code, which skips.
  WorkerConfig config;
  config.socket_path = socket_path_;
  config.cache_path = cache_path_;
  config.cache_size_bytes = static_cast<uint64_t>(10 * 1024 * 1024);

  NullMessageHandler handler;
  Worker worker(config, &handler);
  ASSERT_TRUE(worker.Initialize());

  std::string url = "http://example.com/dedup-heal.css";
  std::string css = "body { margin: 0; } h1 { color: blue; }";
  CacheOriginal(worker.cache(), url, css);

  std::thread worker_thread([&worker]() { worker.Run(); });
  WorkerStopper stopper(worker, worker_thread);
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  CacheNotification notification;
  notification.url = url;
  notification.scheme = "https";
  notification.content_type = ContentType::kCss;
  notification.capability_mask = CapabilityMask().Encode();

  // First notification — processed, dedup marked.
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
  // css_processed moves before the in-flight entry is erased; drain, or the
  // re-send below is rejected by the in-flight guard and never reaches the
  // dedup check (see InvalidateUrlClearsDedup).
  WaitForWorkerIdle(worker);
  ASSERT_EQ(0u, worker.stats().notifications_dedup_healed.load())
      << "no heal before the family is lost";

  // Simulate capacity eviction at the cache layer only: the family
  // disappears while the in-memory dedup entry survives.  NOT
  // InvalidateUrl — that would also clear the dedup entry this test needs
  // to keep orphaned.
  (void)worker.cache()->Remove(StripSchemeAuthority(url), "", "https");
  // Re-seed the source as a durable original (SentinelId::kOriginalContent):
  // sentinel space, so the family still holds NO servable variant — the
  // state the heal is responsible for — while the re-processed notification
  // still has a source to optimize from (the durable-original fallback).
  CacheDurableOriginal(worker.cache(), url, css, "text/css", ContentType::kCss);

  uint64_t css_before = worker.stats().css_processed.load();
  uint64_t skipped_before = worker.stats().notifications_skipped_dedup.load();

  // Second notification — must heal the orphaned dedup entry and reprocess.
  SendNotification(notification);
  bool reprocessed = false;
  for (int i = 0; i < 80 * kSanitizerBudgetScale; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    if (worker.stats().css_processed.load() > css_before) {
      reprocessed = true;
      break;
    }
  }
  EXPECT_TRUE(reprocessed)
      << "URL must be reprocessed once its variant family is gone, not "
         "dedup-skipped forever";
  EXPECT_EQ(1u, worker.stats().notifications_dedup_healed.load())
      << "the heal counter moves exactly on the heal path";
  EXPECT_EQ(skipped_before, worker.stats().notifications_skipped_dedup.load())
      << "a healed notification is not counted as a dedup skip";
}

TEST_F(WorkerTest, DedupSkipStandsWhenVariantFamilyPresent) {
  // Companion of DedupHealReprocessesAfterVariantFamilyLoss: while a
  // servable (non-sentinel) alternate still exists for the key, the dedup
  // skip stands exactly as before and the heal counter does not move.
  WorkerConfig config;
  config.socket_path = socket_path_;
  config.cache_path = cache_path_;
  config.cache_size_bytes = static_cast<uint64_t>(10 * 1024 * 1024);

  NullMessageHandler handler;
  Worker worker(config, &handler);
  ASSERT_TRUE(worker.Initialize());

  std::string url = "http://example.com/dedup-stands.css";
  std::string css = "body { margin: 0; } h1 { color: blue; }";
  CacheOriginal(worker.cache(), url, css);

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
  WaitForWorkerIdle(worker);

  uint64_t skipped_before = worker.stats().notifications_skipped_dedup.load();

  // Second notification with the variant family intact — the skip stands.
  SendNotification(notification);
  for (int i = 0; i < 80 * kSanitizerBudgetScale; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    if (worker.stats().notifications_skipped_dedup.load() > skipped_before)
      break;
  }
  EXPECT_GT(worker.stats().notifications_skipped_dedup.load(), skipped_before)
      << "duplicate notification with a live variant family must be "
         "dedup-skipped";
  EXPECT_EQ(0u, worker.stats().notifications_dedup_healed.load())
      << "no heal when the family is present";
  EXPECT_EQ(1u, worker.stats().css_processed.load())
      << "no reprocessing when the family is present";
}

TEST_F(WorkerTest, DedupHealIsRateLimitedForTerminalDecisions) {
  // Pins the BOUND on the orphaned-entry heal
  //.
  //
  // A sentinel-only family is not always orphaned.  Several paths mark an
  // entry processed while writing nothing servable, and say so: a JS or CSS
  // minify failure is "deterministic for a given input, so retrying on the
  // next notification would loop forever".  On a substrate that records the
  // original only at the durable-original id, such a family is sentinel-only
  // FOREVER, so the heal's existence check sees "orphaned" on every dedup
  // hit and would re-pay the ladder per notification -- reintroducing the
  // loop those marks exist to prevent.
  //
  // The heal is therefore rate limited per URL.  This test drives a JS URL
  // that can never produce a variant and asserts the third notification is
  // deferred rather than re-processed.  Deleting the rate limiter makes the
  // third notification re-pay, and the parse-failure count moves.
  WorkerConfig config;
  config.socket_path = socket_path_;
  config.cache_path = cache_path_;
  config.cache_size_bytes = static_cast<uint64_t>(10 * 1024 * 1024);

  Worker worker(config, nullptr);
  ASSERT_TRUE(worker.Initialize());

  // Unterminated string literal: MinifyUtf8Js reliably returns false, so the
  // worker marks the entry processed and writes NO variant.  Recorded only
  // at the durable-original id, so the family is sentinel-only throughout.
  std::string url = "http://example.com/dedup-heal-bounded.js";
  std::string js = "var  a  =  1;\n\"not valid javascript";
  CacheDurableOriginal(worker.cache(), url, js, "application/javascript",
                       ContentType::kJs);

  std::thread worker_thread([&worker]() { worker.Run(); });
  WorkerStopper stopper(worker, worker_thread);
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  CacheNotification notification;
  notification.url = url;
  notification.scheme = "https";
  notification.content_type = ContentType::kJs;
  notification.capability_mask = CapabilityMask().Encode();

  // 1st notification: processed, parse fails, entry marked, nothing written.
  SendNotification(notification);
  for (int i = 0; i < 80 * kSanitizerBudgetScale; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    if (worker.stats().text_minify_parse_failures.load() >= 1) break;
  }
  ASSERT_GE(worker.stats().text_minify_parse_failures.load(), 1u)
      << "the JS parse failure should have been recorded once";
  ASSERT_EQ(0u, worker.stats().variants_written.load())
      << "the terminal path must not write a variant";
  WaitForWorkerIdle(worker);

  // 2nd notification: dedup hit on a sentinel-only family, and no heal has
  // run for this URL yet, so it heals once -- the behaviour #824 needs.
  SendNotification(notification);
  for (int i = 0; i < 80 * kSanitizerBudgetScale; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    if (worker.stats().notifications_dedup_healed.load() >= 1) break;
  }
  EXPECT_EQ(1u, worker.stats().notifications_dedup_healed.load())
      << "a sentinel-only family must still heal once";
  WaitForWorkerIdle(worker);

  const uint64_t failures_before =
      worker.stats().text_minify_parse_failures.load();
  const uint64_t healed_before =
      worker.stats().notifications_dedup_healed.load();

  // 3rd notification, inside the same window: THE BOUND.  It must be
  // deferred, not re-processed -- no second heal, and no fresh minify
  // attempt on content that deterministically cannot be minified.
  SendNotification(notification);
  bool rate_limited = false;
  for (int i = 0; i < 80 * kSanitizerBudgetScale; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    if (worker.stats().notifications_dedup_heal_rate_limited.load() >= 1) {
      rate_limited = true;
      break;
    }
  }
  EXPECT_TRUE(rate_limited)
      << "a repeat notification inside the window must be rate limited, not "
         "re-processed";
  WaitForWorkerIdle(worker);
  EXPECT_EQ(failures_before, worker.stats().text_minify_parse_failures.load())
      << "the rate-limited notification must not have re-paid the ladder";
  EXPECT_EQ(healed_before, worker.stats().notifications_dedup_healed.load())
      << "the rate-limited notification must not count as a heal";
  EXPECT_EQ(0u, worker.stats().variants_written.load());
}

TEST_F(WorkerTest, AMissingOptimizedCopyIsCreatedAgain) {
  // A stylesheet is optimized: the optimized copy and its gzip and brotli
  // siblings are written beside the stored original.  The optimized copy
  // then disappears from the cache while its siblings stay -- the entry
  // still looks optimized, and every request for it is answered with the
  // original.  The next notification for the URL must write the copy back
  // instead of being dropped as already done.
  WorkerConfig config;
  config.socket_path = socket_path_;
  config.cache_path = cache_path_;
  config.cache_size_bytes = static_cast<uint64_t>(10 * 1024 * 1024);

  NullMessageHandler handler;
  Worker worker(config, &handler);
  ASSERT_TRUE(worker.Initialize());

  std::string url = "http://example.com/missing-copy.css";
  std::string css = "body { margin: 0; } h1 { color: blue; }";
  CacheDurableOriginal(worker.cache(), url, css, "text/css", ContentType::kCss);

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
  WaitForWorkerIdle(worker);

  const std::string path = StripSchemeAuthority(url);
  const AlternateId optimized_id =
      MaskToAlternateId(static_cast<uint8_t>(CapabilityMask().Encode() & 0xFF));
  CapabilityMask gzip_mask;
  gzip_mask.set_transfer_encoding(CapabilityMask::TransferEncoding::kGzip);
  const AlternateId gzip_id =
      MaskToAlternateId(static_cast<uint8_t>(gzip_mask.Encode() & 0xFF));
  ASSERT_TRUE(worker.cache()->AlternateExists(path, "", "https", optimized_id));
  ASSERT_TRUE(worker.cache()->AlternateExists(path, "", "https", gzip_id));

  // Lose the optimized copy and nothing else.
  ASSERT_TRUE(worker.cache()
                  ->RemoveAlternate(path, "", "https", optimized_id)
                  .has_value());
  ASSERT_FALSE(
      worker.cache()->AlternateExists(path, "", "https", optimized_id));
  ASSERT_TRUE(worker.cache()->AlternateExists(path, "", "https", gzip_id));

  const uint64_t css_before = worker.stats().css_processed.load();
  const uint64_t skipped_before =
      worker.stats().notifications_skipped_dedup.load();

  SendNotification(notification);
  bool reprocessed = false;
  for (int i = 0; i < 80 * kSanitizerBudgetScale; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    if (worker.stats().css_processed.load() > css_before) {
      reprocessed = true;
      break;
    }
  }
  EXPECT_TRUE(reprocessed)
      << "a URL whose optimized copy is gone must be optimized again, not "
         "dropped as already processed";
  WaitForWorkerIdle(worker);
  EXPECT_TRUE(worker.cache()->AlternateExists(path, "", "https", optimized_id))
      << "the optimized copy was not written back";
  EXPECT_EQ(1u, worker.stats().notifications_missing_copy_healed.load());
  EXPECT_EQ(0u, worker.stats().notifications_dedup_healed.load())
      << "this is not the nothing-left-to-serve heal";
  EXPECT_EQ(skipped_before, worker.stats().notifications_skipped_dedup.load())
      << "a healed notification is not counted as a skip";
}

TEST_F(WorkerTest, AMissingCopyHealIsRateLimitedPerUrl) {
  // The bound: a copy that keeps disappearing costs one re-optimization per
  // URL per window, never one per notification.
  WorkerConfig config;
  config.socket_path = socket_path_;
  config.cache_path = cache_path_;
  config.cache_size_bytes = static_cast<uint64_t>(10 * 1024 * 1024);

  NullMessageHandler handler;
  Worker worker(config, &handler);
  ASSERT_TRUE(worker.Initialize());

  std::string url = "http://example.com/missing-copy-bounded.css";
  std::string css = "body { margin: 0; } h1 { color: blue; }";
  CacheDurableOriginal(worker.cache(), url, css, "text/css", ContentType::kCss);

  std::thread worker_thread([&worker]() { worker.Run(); });
  WorkerStopper stopper(worker, worker_thread);
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  CacheNotification notification;
  notification.url = url;
  notification.scheme = "https";
  notification.content_type = ContentType::kCss;
  notification.capability_mask = CapabilityMask().Encode();

  const std::string path = StripSchemeAuthority(url);
  const AlternateId optimized_id =
      MaskToAlternateId(static_cast<uint8_t>(CapabilityMask().Encode() & 0xFF));

  SendNotification(notification);
  for (int i = 0; i < 80 * kSanitizerBudgetScale; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    if (worker.stats().css_processed.load() >= 1) break;
  }
  ASSERT_EQ(1u, worker.stats().css_processed.load());
  WaitForWorkerIdle(worker);

  // First loss: healed.
  ASSERT_TRUE(worker.cache()
                  ->RemoveAlternate(path, "", "https", optimized_id)
                  .has_value());
  SendNotification(notification);
  for (int i = 0; i < 80 * kSanitizerBudgetScale; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    if (worker.stats().notifications_missing_copy_healed.load() >= 1) break;
  }
  ASSERT_EQ(1u, worker.stats().notifications_missing_copy_healed.load())
      << "the first loss must be healed";
  const auto healed_at = std::chrono::steady_clock::now();
  WaitForWorkerIdle(worker);
  ASSERT_EQ(2u, worker.stats().css_processed.load());

  // Second loss inside the same window: deferred, not re-optimized.
  ASSERT_TRUE(worker.cache()
                  ->RemoveAlternate(path, "", "https", optimized_id)
                  .has_value());
  const uint64_t limited_before =
      worker.stats().notifications_dedup_heal_rate_limited.load();
  SendNotification(notification);
  const auto sent_at = std::chrono::steady_clock::now();
  // Either outcome ends the wait: the notification was deferred (the bound
  // holds) or the URL was healed a second time (the bound is broken).
  bool rate_limited = false;
  for (int i = 0; i < 80 * kSanitizerBudgetScale; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    if (worker.stats().notifications_dedup_heal_rate_limited.load() >
        limited_before) {
      rate_limited = true;
      break;
    }
    if (worker.stats().notifications_missing_copy_healed.load() >= 2) {
      break;
    }
  }
  // The window is 10 s of real time.  A run slow enough to SEND the second
  // notification after it (an instrumented build on a loaded host) has not
  // tested the bound, and must not report that it failed.  Only the time up
  // to the send counts: how long the wait above took says nothing about
  // whether the notification fell inside the window.
  if (sent_at - healed_at > std::chrono::seconds(8)) {
    GTEST_SKIP() << "the second notification left too late to fall inside "
                    "the 10 s window on this run";
  }
  EXPECT_TRUE(rate_limited)
      << "a second heal for one URL inside the window must be deferred";
  WaitForWorkerIdle(worker);
  EXPECT_EQ(1u, worker.stats().notifications_missing_copy_healed.load());
  EXPECT_EQ(2u, worker.stats().css_processed.load())
      << "the deferred notification must not have re-optimized the URL";
}

TEST_F(WorkerTest, AnAlreadyMinimalStylesheetIsNeverHealed) {
  // The state that looks exactly like a lost copy and is not one: for a
  // stylesheet that is already minimal the optimizer stores compressed
  // copies of the original and no optimized copy at all.  Repeat
  // notifications must be dropped, every time, with nothing re-optimized.
  WorkerConfig config;
  config.socket_path = socket_path_;
  config.cache_path = cache_path_;
  config.cache_size_bytes = static_cast<uint64_t>(10 * 1024 * 1024);

  NullMessageHandler handler;
  Worker worker(config, &handler);
  ASSERT_TRUE(worker.Initialize());

  std::string url = "http://example.com/already-minimal-never-healed.css";
  std::string css = "body{margin:0}h1{color:red}";
  CacheDurableOriginal(worker.cache(), url, css, "text/css", ContentType::kCss);

  std::thread worker_thread([&worker]() { worker.Run(); });
  WorkerStopper stopper(worker, worker_thread);
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  CacheNotification notification;
  notification.url = url;
  notification.scheme = "https";
  notification.content_type = ContentType::kCss;
  notification.capability_mask = CapabilityMask().Encode();

  SendNotification(notification);
  for (int i = 0; i < 80 * kSanitizerBudgetScale; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    if (worker.stats().css_already_optimal_count.load() >= 1) break;
  }
  ASSERT_EQ(1u, worker.stats().css_already_optimal_count.load());
  WaitForWorkerIdle(worker);

  const std::string path = StripSchemeAuthority(url);
  const AlternateId optimized_id =
      MaskToAlternateId(static_cast<uint8_t>(CapabilityMask().Encode() & 0xFF));
  CapabilityMask gzip_mask;
  gzip_mask.set_transfer_encoding(CapabilityMask::TransferEncoding::kGzip);
  const AlternateId gzip_id =
      MaskToAlternateId(static_cast<uint8_t>(gzip_mask.Encode() & 0xFF));
  ASSERT_FALSE(worker.cache()->AlternateExists(path, "", "https", optimized_id))
      << "an already minimal stylesheet has no optimized copy";
  ASSERT_TRUE(worker.cache()->AlternateExists(path, "", "https", gzip_id))
      << "its compressed copies are what makes the entry look like a loss";

  for (int round = 0; round < 3; ++round) {
    const uint64_t skipped_before =
        worker.stats().notifications_skipped_dedup.load();
    SendNotification(notification);
    for (int i = 0; i < 80 * kSanitizerBudgetScale; ++i) {
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
      if (worker.stats().notifications_skipped_dedup.load() > skipped_before)
        break;
    }
    EXPECT_GT(worker.stats().notifications_skipped_dedup.load(), skipped_before)
        << "round " << round;
    WaitForWorkerIdle(worker);
  }
  EXPECT_EQ(0u, worker.stats().notifications_missing_copy_healed.load());
  EXPECT_EQ(0u, worker.stats().notifications_dedup_healed.load());
  EXPECT_EQ(1u, worker.stats().css_already_optimal_count.load())
      << "the stylesheet must not have been judged again";
}

TEST_F(WorkerTest, AnEarlierCopyRecordDoesNotSurviveAPurgeAndANewVerdict) {
  // A URL is optimized (a copy is written), purged, and comes back with
  // content that is already minimal (no copy is due).  The memory of the
  // earlier copy must not turn the new, copy-less entry into a heal.
  WorkerConfig config;
  config.socket_path = socket_path_;
  config.cache_path = cache_path_;
  config.cache_size_bytes = static_cast<uint64_t>(10 * 1024 * 1024);

  NullMessageHandler handler;
  Worker worker(config, &handler);
  ASSERT_TRUE(worker.Initialize());

  std::string url = "http://example.com/purged-then-minimal.css";
  std::string css_v1 = "body { margin: 0; } h1 { color: blue; }";
  std::string css_v2 = "body{margin:0}h1{color:red}";
  CacheDurableOriginal(worker.cache(), url, css_v1, "text/css",
                       ContentType::kCss);

  std::thread worker_thread([&worker]() { worker.Run(); });
  WorkerStopper stopper(worker, worker_thread);
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  CacheNotification notification;
  notification.url = url;
  notification.scheme = "https";
  notification.content_type = ContentType::kCss;
  notification.capability_mask = CapabilityMask().Encode();

  SendNotification(notification);
  for (int i = 0; i < 80 * kSanitizerBudgetScale; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    if (worker.stats().css_processed.load() >= 1) break;
  }
  ASSERT_EQ(1u, worker.stats().css_processed.load());
  WaitForWorkerIdle(worker);

  // Purge, then the URL is recorded again with already minimal content.
  worker.InvalidateUrl(url);
  CacheDurableOriginal(worker.cache(), url, css_v2, "text/css",
                       ContentType::kCss);
  SendNotification(notification);
  for (int i = 0; i < 80 * kSanitizerBudgetScale; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    if (worker.stats().css_already_optimal_count.load() >= 1) break;
  }
  ASSERT_EQ(1u, worker.stats().css_already_optimal_count.load());
  WaitForWorkerIdle(worker);

  // The entry now has compressed copies and no optimized copy -- by verdict.
  const uint64_t skipped_before =
      worker.stats().notifications_skipped_dedup.load();
  SendNotification(notification);
  for (int i = 0; i < 80 * kSanitizerBudgetScale; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    if (worker.stats().notifications_skipped_dedup.load() > skipped_before)
      break;
  }
  EXPECT_GT(worker.stats().notifications_skipped_dedup.load(), skipped_before);
  WaitForWorkerIdle(worker);
  EXPECT_EQ(0u, worker.stats().notifications_missing_copy_healed.load())
      << "the copy written before the purge must not count as missing now";
  EXPECT_EQ(1u, worker.stats().css_already_optimal_count.load());
  EXPECT_EQ(1u, worker.stats().css_processed.load());
}

TEST_F(WorkerTest, AMissingOptimizedScriptCopyIsCreatedAgain) {
  // The same rule for a script: its optimized copy disappears while the
  // compressed siblings stay, and the next notification writes it back.
  WorkerConfig config;
  config.socket_path = socket_path_;
  config.cache_path = cache_path_;
  config.cache_size_bytes = static_cast<uint64_t>(10 * 1024 * 1024);

  NullMessageHandler handler;
  Worker worker(config, &handler);
  ASSERT_TRUE(worker.Initialize());

  std::string url = "http://example.com/missing-copy.js";
  std::string js =
      "// Main application script\n"
      "function  hello( name )  {\n"
      "  var  greeting  =  'Hello, '  +  name;\n"
      "  return  greeting;\n"
      "}\n";
  CacheDurableOriginal(worker.cache(), url, js, "application/javascript",
                       ContentType::kJs);

  std::thread worker_thread([&worker]() { worker.Run(); });
  WorkerStopper stopper(worker, worker_thread);
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  CacheNotification notification;
  notification.url = url;
  notification.scheme = "https";
  notification.content_type = ContentType::kJs;
  notification.capability_mask = CapabilityMask().Encode();

  SendNotification(notification);
  for (int i = 0; i < 80 * kSanitizerBudgetScale; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    if (worker.stats().js_processed.load() >= 1) break;
  }
  ASSERT_EQ(1u, worker.stats().js_processed.load());
  WaitForWorkerIdle(worker);

  const std::string path = StripSchemeAuthority(url);
  const AlternateId optimized_id =
      MaskToAlternateId(static_cast<uint8_t>(CapabilityMask().Encode() & 0xFF));
  CapabilityMask gzip_mask;
  gzip_mask.set_transfer_encoding(CapabilityMask::TransferEncoding::kGzip);
  const AlternateId gzip_id =
      MaskToAlternateId(static_cast<uint8_t>(gzip_mask.Encode() & 0xFF));
  ASSERT_TRUE(worker.cache()->AlternateExists(path, "", "https", optimized_id));
  ASSERT_TRUE(worker.cache()->AlternateExists(path, "", "https", gzip_id));

  ASSERT_TRUE(worker.cache()
                  ->RemoveAlternate(path, "", "https", optimized_id)
                  .has_value());
  ASSERT_TRUE(worker.cache()->AlternateExists(path, "", "https", gzip_id));

  SendNotification(notification);
  for (int i = 0; i < 80 * kSanitizerBudgetScale; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    if (worker.stats().js_processed.load() >= 2) break;
  }
  EXPECT_EQ(2u, worker.stats().js_processed.load())
      << "a script whose optimized copy is gone must be optimized again";
  WaitForWorkerIdle(worker);
  EXPECT_TRUE(worker.cache()->AlternateExists(path, "", "https", optimized_id))
      << "the optimized copy was not written back";
  EXPECT_EQ(1u, worker.stats().notifications_missing_copy_healed.load());
  EXPECT_EQ(0u, worker.stats().notifications_dedup_healed.load());
}

TEST_F(WorkerTest, APurgedUrlKeepsNoCopyRecord) {
  // The record that the optimizer wrote a URL's optimized copy goes
  // wherever the URL's processed mark goes: a purge of the URL, or clearing
  // its processing state, leaves neither behind.
  WorkerConfig config;
  config.socket_path = socket_path_;
  config.cache_path = cache_path_;
  config.cache_size_bytes = static_cast<uint64_t>(10 * 1024 * 1024);

  NullMessageHandler handler;
  Worker worker(config, &handler);
  ASSERT_TRUE(worker.Initialize());

  const std::string purged_url = "http://example.com/record-purged.css";
  const std::string cleared_url = "http://example.com/record-cleared.css";
  const std::string css = "body { margin: 0; } h1 { color: blue; }";
  CacheDurableOriginal(worker.cache(), purged_url, css, "text/css",
                       ContentType::kCss);
  CacheDurableOriginal(worker.cache(), cleared_url, css, "text/css",
                       ContentType::kCss);

  std::thread worker_thread([&worker]() { worker.Run(); });
  WorkerStopper stopper(worker, worker_thread);
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  uint64_t expected_processed = 0;
  for (const std::string& url : {purged_url, cleared_url}) {
    CacheNotification notification;
    notification.url = url;
    notification.scheme = "https";
    notification.content_type = ContentType::kCss;
    notification.capability_mask = CapabilityMask().Encode();
    SendNotification(notification);
    ++expected_processed;
    for (int i = 0; i < 80 * kSanitizerBudgetScale; ++i) {
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
      if (worker.stats().css_processed.load() >= expected_processed) break;
    }
    ASSERT_EQ(expected_processed, worker.stats().css_processed.load());
    WaitForWorkerIdle(worker);
  }
  ASSERT_EQ(2u, worker.copy_record_count())
      << "each optimized stylesheet leaves one record";

  worker.InvalidateUrl(purged_url);
  EXPECT_EQ(1u, worker.copy_record_count())
      << "a purged URL must not keep its record";

  worker.ClearDedupAndCooldown(cleared_url, "", "https");
  EXPECT_EQ(0u, worker.copy_record_count())
      << "clearing a URL's processing state must drop its record too";
}

TEST_F(WorkerTest, CopyRecordsAgeOutWithTheirMarks) {
  // The records are bounded by the processed set: when a mark ages out, its
  // record goes with it, and a record whose mark was moved to the older
  // generation still heals.
  WorkerConfig config;
  config.socket_path = socket_path_;
  config.cache_path = cache_path_;
  config.cache_size_bytes = static_cast<uint64_t>(10 * 1024 * 1024);
  // Every second mark rotates the processed set.
  config.max_processed_entries = 1;

  NullMessageHandler handler;
  Worker worker(config, &handler);
  ASSERT_TRUE(worker.Initialize());

  const std::string css = "body { margin: 0; } h1 { color: blue; }";
  std::vector<std::string> urls;
  for (int i = 0; i < 4; ++i) {
    urls.push_back(absl::StrCat("http://example.com/record-aging-", i, ".css"));
    CacheDurableOriginal(worker.cache(), urls.back(), css, "text/css",
                         ContentType::kCss);
  }

  std::thread worker_thread([&worker]() { worker.Run(); });
  WorkerStopper stopper(worker, worker_thread);
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  // One at a time, so the order of the marks is the order of the URLs.
  CacheNotification notification;
  notification.scheme = "https";
  notification.content_type = ContentType::kCss;
  notification.capability_mask = CapabilityMask().Encode();
  for (size_t n = 0; n < urls.size(); ++n) {
    notification.url = urls[n];
    SendNotification(notification);
    for (int i = 0; i < 80 * kSanitizerBudgetScale; ++i) {
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
      if (worker.stats().css_processed.load() >= n + 1) break;
    }
    ASSERT_EQ(n + 1, worker.stats().css_processed.load());
    WaitForWorkerIdle(worker);
  }

  // Two rotations happened.  The marks of the first two URLs are gone, and
  // so are their records; the last two are in the older generation.
  EXPECT_EQ(2u, worker.copy_record_count())
      << "a record must not outlive its processed mark";

  // The third URL's mark and record are in the older generation: losing its
  // copy is still noticed.
  const std::string path = StripSchemeAuthority(urls[2]);
  const AlternateId optimized_id =
      MaskToAlternateId(static_cast<uint8_t>(CapabilityMask().Encode() & 0xFF));
  ASSERT_TRUE(worker.cache()
                  ->RemoveAlternate(path, "", "https", optimized_id)
                  .has_value());
  notification.url = urls[2];
  SendNotification(notification);
  for (int i = 0; i < 80 * kSanitizerBudgetScale; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    if (worker.stats().notifications_missing_copy_healed.load() >= 1) break;
  }
  EXPECT_EQ(1u, worker.stats().notifications_missing_copy_healed.load());
  WaitForWorkerIdle(worker);
  EXPECT_TRUE(worker.cache()->AlternateExists(path, "", "https", optimized_id))
      << "the optimized copy was not written back";
}

// --- begin ported adversarial probes ---
TEST_F(WorkerTest, AdvStaleTombstoneBlocksHealForChangedSource) {
  // ADVERSARIAL REVIEW PROBE, ported from the review of de600de8.  Against
  // the tombstone arm this FAILED deterministically: the arm counted the
  // tombstone as "family present" purely by ALTERNATE ID -- it never parsed
  // the payload, so it never compared the tombstone's bound source SHA-256
  // with the bytes now at the durable original, and a source changed after
  // the tombstone was written stayed unoptimized forever.  Under the bounded
  // heal the sentinel-only family heals (once per window), the changed
  // source is re-optimized, and this probe PASSES.  Body otherwise verbatim.
  //
  // Original review comment: identical in every respect to
  // DedupHealsEvictedFamilyWithNoTombstone except that a decline tombstone
  // survived the eviction, and the durable original re-recorded afterwards
  // holds DIFFERENT (v2) bytes than the ones the tombstone was written for.
  WorkerConfig config;
  config.socket_path = socket_path_;
  config.cache_path = cache_path_;
  config.cache_size_bytes = static_cast<uint64_t>(10 * 1024 * 1024);

  NullMessageHandler handler;
  Worker worker(config, &handler);
  ASSERT_TRUE(worker.Initialize());

  std::string url = "http://example.com/adv-stale-tombstone.css";
  std::string css_v1 = "body { margin: 0; } h1 { color: blue; }";
  std::string css_v2 = "body { padding: 0; } h2 { color: green; }";
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
  WaitForWorkerIdle(worker);

  // Capacity eviction takes the servable variants.  The tombstone (written
  // in a later phase) survives; the substrate then re-records the CHANGED
  // origin body at the durable-original id, which is the only place the
  // module substrate records originals.
  (void)worker.cache()->Remove(StripSchemeAuthority(url), "", "https");
  CacheDurableOriginal(worker.cache(), url, css_v2, "text/css",
                       ContentType::kCss);
  const std::string blob = "decline-tombstone-placeholder";
  auto wh =
      worker.cache()->WriteSentinel(StripSchemeAuthority(url), "", "https",
                                    SentinelId::kDeclineTombstone, blob.size());
  ASSERT_TRUE(wh.has_value()) << "Failed to open tombstone write handle";
  ASSERT_TRUE(wh->write_sync(std::as_bytes(std::span(blob.data(), blob.size())))
                  .has_value());
  ASSERT_TRUE(wh->close_sync().has_value());

  const uint64_t css_before = worker.stats().css_processed.load();

  SendNotification(notification);
  bool reprocessed = false;
  for (int i = 0; i < 80 * kSanitizerBudgetScale; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    if (worker.stats().css_processed.load() > css_before) {
      reprocessed = true;
      break;
    }
  }
  EXPECT_TRUE(reprocessed)
      << "the source CHANGED after the tombstone was written and every "
         "servable variant is gone -- the URL must be re-optimized, not "
         "skipped forever on the strength of a tombstone bound to bytes that "
         "no longer exist";
  EXPECT_EQ(0u, worker.stats().notifications_skipped_dedup.load())
      << "a stale tombstone must not answer the dedup check";
}

TEST_F(WorkerTest, AdvUnminifiableJsRepaysTheLadderOnEveryNotification) {
  // ADVERSARIAL REVIEW PROBE, ported from the review of de600de8 with the
  // assertions re-based on the bounded-heal design.  At de600de8 this probe
  // measured the re-pay as PER-NOTIFICATION: three notifications produced
  // skipped_dedup 0 / healed 2 / parse_failures 3, and since a family with
  // nothing servable makes the front end fall through and re-notify on every
  // request, that re-pay was per-request and permanent.  The bounded heal
  // converts it into one re-pay per URL per kDedupHealMinIntervalSecs: three
  // notifications inside one window must produce exactly TWO parse failures
  // (the initial attempt plus one bounded heal), ONE heal, and ONE
  // rate-limited deferral.
  //
  // worker.cc marks an unparseable JS URL processed with the comment "parse
  // failure is deterministic for a given input, so retrying on the next
  // notification would loop forever".  That mark writes NO servable
  // alternate.  On a substrate that records originals only at the
  // durable-original id (sentinel space -- the Apache module),
  // the family is therefore sentinel-only forever, and the existence check
  // reads it as "orphaned" on every dedup hit.
  WorkerConfig config;
  config.socket_path = socket_path_;
  config.cache_path = cache_path_;
  config.cache_size_bytes = static_cast<uint64_t>(10 * 1024 * 1024);

  NullMessageHandler handler;
  Worker worker(config, &handler);
  ASSERT_TRUE(worker.Initialize());

  std::string url = "http://example.com/adv-broken.js";
  std::string js = "var  a  =  1;\n\"not valid javascript";
  // Module-substrate shape: the origin bytes live ONLY at the durable
  // original (SentinelId::kOriginalContent), never at the identity id.
  CacheDurableOriginal(worker.cache(), url, js, "application/javascript",
                       ContentType::kJs);

  std::thread worker_thread([&worker]() { worker.Run(); });
  WorkerStopper stopper(worker, worker_thread);
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  CacheNotification notification;
  notification.url = url;
  notification.scheme = "https";
  notification.content_type = ContentType::kJs;
  notification.capability_mask = CapabilityMask().Encode();

  SendNotification(notification);
  for (int i = 0; i < 80 * kSanitizerBudgetScale; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    if (worker.stats().text_minify_parse_failures.load() >= 1) break;
  }
  ASSERT_EQ(1u, worker.stats().text_minify_parse_failures.load())
      << "the first notification must attempt and fail the minify once";
  WaitForWorkerIdle(worker);

  // Notifications 2 and 3, inside one rate-limit window.
  for (int n = 0; n < 2; ++n) {
    const uint64_t want = worker.stats().notifications_received.load() + 1;
    SendNotification(notification);
    for (int i = 0; i < 80 * kSanitizerBudgetScale; ++i) {
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
      if (worker.stats().notifications_received.load() >= want) break;
    }
    ASSERT_EQ(want, worker.stats().notifications_received.load())
        << "the worker did not receive notification " << (n + 2);
    WaitForWorkerIdle(worker);
  }

  // THE BOUND: notification 2 heals once (the property #824 needs);
  // notification 3 is deferred by the rate limiter.  At de600de8 the same
  // three notifications measured parse_failures 3 / healed 2 -- an
  // unbounded per-notification re-pay.
  EXPECT_EQ(2u, worker.stats().text_minify_parse_failures.load())
      << "the re-pay must be bounded to one per window, not one per "
         "notification";
  EXPECT_EQ(1u, worker.stats().notifications_dedup_healed.load());
  EXPECT_EQ(1u, worker.stats().notifications_dedup_heal_rate_limited.load());
  {
    auto alts =
        worker.cache()->ListAlternates(StripSchemeAuthority(url), "", "https");
    std::string ids;
    if (alts.has_value()) {
      for (const auto& al : *alts) {
        char buf[8];
        snprintf(buf, sizeof(buf), "%02x,", static_cast<unsigned>(al.id));
        ids += buf;
      }
    } else {
      ids = "<none>";
    }
    std::cerr << "ADV-DIAG recv="
              << worker.stats().notifications_received.load() << " skip_dedup="
              << worker.stats().notifications_skipped_dedup.load()
              << " skip_inflight="
              << worker.stats().notifications_skipped_inflight.load()
              << " healed=" << worker.stats().notifications_dedup_healed.load()
              << " rate_limited="
              << worker.stats().notifications_dedup_heal_rate_limited.load()
              << " parse_fail="
              << worker.stats().text_minify_parse_failures.load()
              << " js_proc=" << worker.stats().js_processed.load()
              << " variants=" << worker.stats().variants_written.load()
              << " alts=[" << ids << "]" << '\n';
  }
}

}  // namespace
}  // namespace pagespeed
