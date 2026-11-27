// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

// HTML notification tests
//
// - HTML processing, early hints and preload hints
// - Speculation rules and hot URL tracking
// - HTML-specific skip, cooldown-write and logging paths
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
#include <optional>
#include <span>
#include <string>
#include <thread>
#include <vector>

#include "cyclone/cache.hpp"
#include "gtest/gtest.h"
#include "lib/base/message_handler.h"
#include "lib/cache/cache.h"
#include "lib/cache/freshness.h"
#include "lib/classify/alternate_id.h"
#include "lib/classify/alternate_metadata.h"
#include "lib/classify/capability_mask.h"
#include "src/nginx/early_hints_util.h"
#include "src/proto/worker_ipc.h"
#include "src/worker/browser_analysis_manager.h"
#include "src/worker/url_registry.h"
#include "src/worker/worker.h"
#include "test/test_util/cache_test_peer.h"
#include "test/test_util/temp_dir.h"
#include "test/test_util/worker_quiescence.h"

namespace pagespeed {
namespace {

// =============================================================================
// Early Hints: preload hints stored by HTML notification
// =============================================================================

TEST_F(WorkerTest, HtmlNotificationStoresPreloadHints) {
  // When the worker processes HTML with external stylesheet links,
  // it should store the stylesheet URLs as preload hints in cache
  // at mask=0xFFFFFFFF (reserved for hints).
  WorkerConfig config;
  config.socket_path = socket_path_;
  config.cache_path = cache_path_;
  config.cache_size_bytes = static_cast<uint64_t>(10 * 1024 * 1024);
  // Async CSS off: the sheets stay render-blocking links, they keep their
  // preload hints, and no critical block is inlined next to them. This test
  // pins all three.
  config.disable_async_css = true;

  Worker worker(config, nullptr);
  ASSERT_TRUE(worker.Initialize());

  // Pre-populate HTML that references external stylesheets.
  std::string url = "http://example.com/hints.html";
  std::string html =
      "<html><head>"
      "<link rel=\"stylesheet\" href=\"/base.css\">"
      "<link rel=\"stylesheet\" href=\"/theme.css\">"
      "<style>body { margin: 0; }</style>"
      "</head><body><h1>Hello</h1></body></html>";
  CacheOriginal(worker.cache(), url, html);

  // Pre-populate the external stylesheets so the worker can gather
  // CSS and produce a variant (otherwise it returns early).
  std::string css1 = "body { margin: 0; }";
  CacheOriginal(worker.cache(), "/base.css", css1);
  std::string css2 = "h1 { color: red; }";
  CacheOriginal(worker.cache(), "/theme.css", css2);

  // Run worker in background.
  std::thread worker_thread([&worker]() { worker.Run(); });
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  // Send HTML notification.
  CacheNotification notification;
  notification.url = url;
  notification.scheme = "https";
  notification.content_type = ContentType::kHtml;
  notification.capability_mask = 0;
  SendNotification(notification);

  // Poll for the hints sentinel entry.
  std::string hints;
  for (int i = 0; i < 40 * kSanitizerBudgetScale; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    hints = ReadSentinel(worker.cache(), url, SentinelId::kEarlyHints);
    if (!hints.empty()) break;
  }

  // Verify hints were stored.
  EXPECT_FALSE(hints.empty()) << "Preload hints not stored";
  if (!hints.empty()) {
    // Should contain the two stylesheet URLs, newline-separated.
    EXPECT_NE(hints.find("/base.css"), std::string::npos);
    EXPECT_NE(hints.find("/theme.css"), std::string::npos);
    // Verify newline separation.
    EXPECT_NE(hints.find('\n'), std::string::npos);
  }

  // The sheets stay blocking, so the pass has no critical block to write; the
  // page has nothing else to change, so it is skipped and no variant carries
  // a block.
  for (int i = 0; i < 40 * kSanitizerBudgetScale; ++i) {
    if (worker.stats().html_assembly_skipped.load() >= 1u) break;
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
  }
  EXPECT_GE(worker.stats().html_assembly_skipped.load(), 1u);
  EXPECT_EQ(worker.stats().html_assembly_complete.load(), 0u);
  CapabilityMask mask = CapabilityMask::Decode(0);
  EXPECT_EQ(
      ReadVariant(worker.cache(), url, mask).find("data-pagespeed-critical"),
      std::string::npos);

  worker.Shutdown();
  worker_thread.join();
}

TEST_F(WorkerTest, HtmlNotificationNoStylesheetsNoHints) {
  // HTML with no external stylesheets should NOT store hints.
  WorkerConfig config;
  config.socket_path = socket_path_;
  config.cache_path = cache_path_;
  config.cache_size_bytes = static_cast<uint64_t>(10 * 1024 * 1024);

  Worker worker(config, nullptr);
  ASSERT_TRUE(worker.Initialize());

  // Pre-populate HTML with only inline CSS (no external stylesheet links).
  std::string url = "http://example.com/inline-only.html";
  std::string html =
      "<html><head>"
      "<style>body { margin: 0; } h1 { color: red; }</style>"
      "</head><body><h1>Hello</h1></body></html>";
  CacheOriginal(worker.cache(), url, html);

  // Run worker in background.
  std::thread worker_thread([&worker]() { worker.Run(); });
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  // Send HTML notification.
  CacheNotification notification;
  notification.url = url;
  notification.scheme = "https";
  notification.content_type = ContentType::kHtml;
  notification.capability_mask = 0;
  SendNotification(notification);

  // Give worker time to process.
  std::this_thread::sleep_for(std::chrono::milliseconds(500));

  // Verify no hints sentinel was stored.
  std::string hints =
      ReadSentinel(worker.cache(), url, SentinelId::kEarlyHints);
  EXPECT_TRUE(hints.empty()) << "Hints stored for HTML without stylesheets";

  worker.Shutdown();
  worker_thread.join();
}

// =============================================================================
// SpeculationRules / Hot URL tracking (tested indirectly through HTML processing)
// =============================================================================

TEST_F(WorkerTest, SpeculationRulesDisabledByDefault) {
  // When enable_speculation_rules is false (default), HTML processing should
  // not inject speculation rules even when multiple URLs are processed.
  WorkerConfig config;
  config.socket_path = socket_path_;
  config.cache_path = cache_path_;
  config.cache_size_bytes = static_cast<uint64_t>(10 * 1024 * 1024);
  config.enable_speculation_rules = false;

  NullMessageHandler handler;
  Worker worker(config, &handler);
  ASSERT_TRUE(worker.Initialize());

  std::string html =
      "<html><head><style>body { margin: 0; }</style></head>"
      "<body><h1>Page</h1></body></html>";
  std::string url = "http://example.com/no-speculation.html";
  CacheOriginal(worker.cache(), url, html);

  std::thread worker_thread([&worker]() { worker.Run(); });
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  CacheNotification notification;
  notification.url = url;
  notification.scheme = "https";
  notification.content_type = ContentType::kHtml;
  notification.capability_mask = CapabilityMask().Encode();
  SendNotification(notification);

  // Wait for processing.
  for (int i = 0; i < 40 * kSanitizerBudgetScale; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    if (worker.stats().html_processed.load() >= 1) break;
  }

  // The variant should not contain speculation rules script tag.
  CapabilityMask mask = CapabilityMask::Decode(CapabilityMask().Encode());
  mask.set_image_format(CapabilityMask::ImageFormat::kOriginal);
  std::string variant = ReadVariant(worker.cache(), url, mask);
  if (!variant.empty()) {
    EXPECT_EQ(variant.find("speculationrules"), std::string::npos)
        << "Speculation rules should not be injected when disabled";
  }

  worker.Shutdown();
  worker_thread.join();
}

TEST_F(WorkerTest, WarmupWithSpeculationRulesTracksUrl) {
  // When enable_speculation_rules is true, a warmup notification should
  // track the URL for speculation rules.  We verify indirectly by checking
  // that the warmup request was handled without errors.
  WorkerConfig config;
  config.socket_path = socket_path_;
  config.cache_path = cache_path_;
  config.cache_size_bytes = static_cast<uint64_t>(10 * 1024 * 1024);
  config.enable_speculation_rules = true;
  config.enable_warmup = true;

  NullMessageHandler handler;
  Worker worker(config, &handler);
  ASSERT_TRUE(worker.Initialize());

  // Pre-populate CSS content for warmup to process.
  std::string url = "http://example.com/speculation-warmup.css";
  std::string css = "body { margin: 0; }";
  CacheOriginal(worker.cache(), url, css);

  std::thread worker_thread([&worker]() { worker.Run(); });
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  CacheNotification notification;
  notification.url = url;
  notification.scheme = "https";
  notification.content_type = ContentType::kCss;
  notification.capability_mask = 0xFFFFFFFE;  // kWarmupSentinel
  SendNotification(notification);

  // Poll for warmup processing (CSS processed counter).
  for (int i = 0; i < 40; ++i) {
    if (worker.stats().css_processed.load() > 0) break;
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
  }

  // Verify no errors during warmup processing.
  EXPECT_EQ(worker.stats().errors.load(), 0u)
      << "Warmup with speculation rules should not cause errors";

  worker.Shutdown();
  worker_thread.join();
}

TEST_F(WorkerTest, MultipleHtmlNotificationsWithSpeculationRules) {
  // Process multiple HTML pages with speculation rules enabled.
  // Each page should be tracked in the hot URL list (tested indirectly).
  WorkerConfig config;
  config.socket_path = socket_path_;
  config.cache_path = cache_path_;
  config.cache_size_bytes = static_cast<uint64_t>(10 * 1024 * 1024);
  config.enable_speculation_rules = true;

  NullMessageHandler handler;
  config.unsafe_force_async_css =
      true;  // fixture page is not under test for the gate
  Worker worker(config, &handler);
  ASSERT_TRUE(worker.Initialize());

  // Create multiple HTML pages.
  for (int i = 0; i < 3; ++i) {
    std::string url =
        "http://example.com/spec-page" + std::to_string(i) + ".html";
    std::string html =
        "<html><head><style>body { margin: 0; }</style></head>"
        "<body><h1>Page " +
        std::to_string(i) + "</h1></body></html>";
    CacheOriginal(worker.cache(), url, html);
  }

  std::thread worker_thread([&worker]() { worker.Run(); });
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  for (int i = 0; i < 3; ++i) {
    CacheNotification notification;
    notification.url =
        "http://example.com/spec-page" + std::to_string(i) + ".html";
    notification.scheme = "https";
    notification.content_type = ContentType::kHtml;
    notification.capability_mask = CapabilityMask().Encode();
    SendNotification(notification);
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
  }

  // Wait for all to process.
  for (int i = 0; i < 40 * kSanitizerBudgetScale; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    if (worker.stats().html_processed.load() >= 3) break;
  }

  EXPECT_GE(worker.stats().html_processed.load(), 3u)
      << "All three HTML pages should be processed with speculation rules";
  EXPECT_EQ(worker.stats().errors.load(), 0u);

  worker.Shutdown();
  worker_thread.join();
}

TEST_F(WorkerTest, HtmlNotificationGzipGuard) {
  // HTML content starting with gzip magic bytes should be skipped.
  WorkerConfig config;
  config.socket_path = socket_path_;
  config.cache_path = cache_path_;
  config.cache_size_bytes = static_cast<uint64_t>(10 * 1024 * 1024);

  NullMessageHandler handler;
  Worker worker(config, &handler);
  ASSERT_TRUE(worker.Initialize());

  std::string url = "http://example.com/gzip-guard.html";
  std::string gzip_html = "\x1f\x8b\x08...fake gzip html content";
  CacheOriginal(worker.cache(), url, gzip_html);

  std::thread worker_thread([&worker]() { worker.Run(); });
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  // Use Mobile mask (0x00) so the variant would be written at a
  // different AlternateId from the original (which is at Desktop 0x08).
  CacheNotification notification;
  notification.url = url;
  notification.scheme = "https";
  notification.content_type = ContentType::kHtml;
  notification.capability_mask = 0;  // Mobile/Identity
  SendNotification(notification);

  // Poll for the error counter to advance instead of a fixed sleep (slow
  // shards can sit at the pipe read past a fixed budget).  This also lets the
  // no-variant invariant below settle.
  for (int i = 0; i < 80 * kSanitizerBudgetScale; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    if (worker.stats().errors.load() >= 1u) break;
  }

  // No variant should be written at the Mobile mask for gzip content.
  CapabilityMask mask = CapabilityMask::Decode(0);
  std::string variant = ReadVariant(worker.cache(), url, mask);
  EXPECT_TRUE(variant.empty())
      << "Gzip-guarded HTML should not produce a variant";

  // Error counter should have incremented.
  EXPECT_GE(worker.stats().errors.load(), 1u)
      << "HTML gzip guard should increment error counter";

  worker.Shutdown();
  worker_thread.join();
}

TEST_F(WorkerTest, HtmlNotificationNoTransformsApplicable) {
  // Disable all HTML transforms and provide HTML with no critical CSS.
  // The worker should still process but may not produce a variant (since
  // no transforms produce output different from original).
  WorkerConfig config;
  config.socket_path = socket_path_;
  config.cache_path = cache_path_;
  config.cache_size_bytes = static_cast<uint64_t>(10 * 1024 * 1024);
  config.disable_lazy_load = true;
  config.disable_image_dimensions = true;
  config.disable_lcp_preload = true;
  config.disable_preconnect_injection = true;
  config.enable_speculation_rules = false;

  NullMessageHandler handler;
  Worker worker(config, &handler);
  ASSERT_TRUE(worker.Initialize());

  // HTML with no CSS, so critical CSS injection has nothing to inject.
  std::string url = "http://example.com/no-transforms.html";
  std::string html =
      "<html><head></head><body><p>Simple page</p></body></html>";
  CacheOriginal(worker.cache(), url, html);

  std::thread worker_thread([&worker]() { worker.Run(); });
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  CacheNotification notification;
  notification.url = url;
  notification.scheme = "https";
  notification.content_type = ContentType::kHtml;
  notification.capability_mask = CapabilityMask().Encode();
  SendNotification(notification);

  // Wait for processing.
  for (int i = 0; i < 40 * kSanitizerBudgetScale; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    if (worker.stats().notifications_received.load() >= 1) break;
  }

  // Notification should have been received without errors.
  EXPECT_GE(worker.stats().notifications_received.load(), 1u);
  EXPECT_EQ(worker.stats().errors.load(), 0u)
      << "No transforms applicable should not cause errors";

  worker.Shutdown();
  worker_thread.join();
}

TEST_F(WorkerTest, HtmlNotificationWithLcpAndPreconnect) {
  // HTML with an LCP image and third-party stylesheets should
  // produce Early Hints containing both image: and preconnect: entries.
  WorkerConfig config;
  config.socket_path = socket_path_;
  config.cache_path = cache_path_;
  config.cache_size_bytes = static_cast<uint64_t>(10 * 1024 * 1024);

  Worker worker(config, nullptr);
  ASSERT_TRUE(worker.Initialize());

  std::string url = "http://example.com/lcp-preconnect.html";
  std::string html =
      "<html><head>"
      "<link rel=\"stylesheet\" "
      "href=\"https://fonts.googleapis.com/css2?family=Roboto\">"
      "<style>body { margin: 0; } .hero { font-size: 24px; }</style>"
      "</head><body>"
      "<img src=\"https://cdn.example.com/hero.jpg\">"
      "<p>Content</p>"
      "</body></html>";
  CacheOriginal(worker.cache(), url, html);

  // Pre-populate the external stylesheet so critical CSS injection works.
  std::string font_url = "https://fonts.googleapis.com/css2?family=Roboto";
  std::string font_css = "@font-face { font-family: Roboto; }";
  CacheOriginal(worker.cache(), font_url, font_css, "fonts.googleapis.com");

  std::thread worker_thread([&worker]() { worker.Run(); });
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  CacheNotification notification;
  notification.url = url;
  notification.scheme = "https";
  notification.content_type = ContentType::kHtml;
  notification.capability_mask = CapabilityMask().Encode();
  SendNotification(notification);

  // Poll for Early Hints sentinel.
  std::string hints;
  for (int i = 0; i < 40 * kSanitizerBudgetScale; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    hints = ReadSentinel(worker.cache(), url, SentinelId::kEarlyHints);
    if (!hints.empty()) break;
  }

  EXPECT_FALSE(hints.empty()) << "Early Hints not stored";
  if (!hints.empty()) {
    // Should contain the LCP image with image: prefix.
    EXPECT_NE(hints.find("image:https://cdn.example.com/hero.jpg"),
              std::string::npos)
        << "Hints should contain LCP image. Got: " << hints;

    // Should contain preconnect for third-party origins.
    EXPECT_NE(hints.find("preconnect:https://fonts.googleapis.com"),
              std::string::npos)
        << "Hints should contain preconnect for fonts. Got: " << hints;
  }

  worker.Shutdown();
  worker_thread.join();
}

// =============================================================================
// Additional Coverage Tests: Uncovered Code Paths in worker.cc
// =============================================================================

// --- A. HTML Path Coverage ---

TEST_F(WorkerTest, HtmlOriginalNotInCache) {
  // Send HTML notification for URL not in cache — should not crash
  // and should not write any variant.  Covers the "original HTML not
  // found" early return (worker.cc ~1506-1512).
  WorkerConfig config;
  config.socket_path = socket_path_;
  config.cache_path = cache_path_;
  config.cache_size_bytes = static_cast<uint64_t>(10 * 1024 * 1024);

  NullMessageHandler handler;
  Worker worker(config, &handler);
  ASSERT_TRUE(worker.Initialize());

  std::thread worker_thread([&worker]() { worker.Run(); });
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  CacheNotification notification;
  notification.url = "http://example.com/html-not-cached.html";
  notification.scheme = "https";
  notification.content_type = ContentType::kHtml;
  notification.capability_mask = 0;
  SendNotification(notification);

  // Wait for notification to be received.
  for (int i = 0; i < 40 * kSanitizerBudgetScale; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    if (worker.stats().notifications_received.load() >= 1) break;
  }

  EXPECT_GE(worker.stats().notifications_received.load(), 1u);
  // No HTML should be processed since it's not in cache.
  EXPECT_EQ(worker.stats().html_processed.load(), 0u);
  EXPECT_EQ(worker.stats().variants_written.load(), 0u);

  worker.Shutdown();
  worker_thread.join();
}

TEST_F(WorkerTest, HtmlTransformNoChanges) {
  // Send HTML that has elements but no transforms apply.  All individual
  // transforms are enabled, but the HTML has no CSS (no critical CSS), no
  // images (no lazy load), no LCP candidate (no preload), no third-party
  // origins (no preconnect), and no speculation rules.  The HtmlTransformFilter
  // produces no modifications.  Covers line ~1759-1765.
  WorkerConfig config;
  config.socket_path = socket_path_;
  config.cache_path = cache_path_;
  config.cache_size_bytes = static_cast<uint64_t>(10 * 1024 * 1024);
  // Leave all transforms enabled (defaults) but provide HTML that
  // won't trigger any of them.
  config.enable_speculation_rules = false;

  NullMessageHandler handler;
  Worker worker(config, &handler);
  ASSERT_TRUE(worker.Initialize());

  // HTML with no CSS, no images, no external resources — transforms
  // are applicable in theory but produce no changes in practice.
  // We need at least one external stylesheet link for preconnect to have
  // something to work with, but since the stylesheet is on the same origin,
  // preconnect won't inject anything.  The key is that the final
  // transform_filter.modified() returns false.
  std::string url = "http://example.com/no-changes.html";
  std::string html =
      "<html><head></head><body><p>Plain text only</p></body></html>";
  CacheOriginal(worker.cache(), url, html);

  std::thread worker_thread([&worker]() { worker.Run(); });
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  CacheNotification notification;
  notification.url = url;
  notification.scheme = "https";
  notification.content_type = ContentType::kHtml;
  notification.capability_mask = 0;
  SendNotification(notification);

  // Wait for notification to be processed.
  for (int i = 0; i < 40 * kSanitizerBudgetScale; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    if (worker.stats().notifications_received.load() >= 1) break;
  }

  EXPECT_GE(worker.stats().notifications_received.load(), 1u);
  // No errors should have occurred.
  EXPECT_EQ(worker.stats().errors.load(), 0u);

  // No variant should be written at Mobile mask since transforms
  // produced no changes (html_assembly_skipped should increment).
  CapabilityMask mask = CapabilityMask::Decode(0);
  std::string variant = ReadVariant(worker.cache(), url, mask);
  EXPECT_TRUE(variant.empty())
      << "No variant should be written when transforms produce no changes";

  worker.Shutdown();
  worker_thread.join();
}

TEST_F(WorkerTest, HtmlCacheNotInitializedSkips) {
  // When cache is not initialized, HTML notifications should be skipped.
  // Covers worker.cc ~1494-1499.
  WorkerConfig config;
  config.socket_path = socket_path_;
  // No cache_path — cache_ will be null.

  NullMessageHandler handler;
  Worker worker(config, &handler);
  ASSERT_TRUE(worker.Initialize());

  std::thread worker_thread([&worker]() { worker.Run(); });
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  CacheNotification notification;
  notification.url = "http://example.com/no-cache-html.html";
  notification.scheme = "https";
  notification.content_type = ContentType::kHtml;
  notification.capability_mask = 0;
  SendNotification(notification);

  for (int i = 0; i < 40 * kSanitizerBudgetScale; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    if (worker.stats().notifications_received.load() >= 1) break;
  }

  EXPECT_GE(worker.stats().notifications_received.load(), 1u);
  EXPECT_EQ(worker.stats().html_processed.load(), 0u);

  worker.Shutdown();
  worker_thread.join();
}

// =============================================================================
// Additional Coverage: HTML Cooldown, Speculation Rules, SVG Pipeline,
// Proactive Transcoding, CSS @import Edge Cases
// =============================================================================

TEST_F(WorkerTest, HtmlWriteFailureCooldownSkipsWithMessage) {
  // When HTML processing sets a tentative cooldown and a second
  // notification arrives within the cooldown window, the worker should
  // skip processing and log the remaining cooldown time.
  // Covers lines 1458-1470 (cooldown expiry check with log message).
  WorkerConfig config;
  config.socket_path = socket_path_;
  config.cache_path = cache_path_;
  config.cache_size_bytes = static_cast<uint64_t>(10 * 1024 * 1024);

  NullMessageHandler handler;
  config.unsafe_force_async_css =
      true;  // fixture page is not under test for the gate
  Worker worker(config, &handler);
  ASSERT_TRUE(worker.Initialize());

  // Pre-populate HTML with inline CSS so a variant is produced.
  std::string url = "http://example.com/cooldown-msg.html";
  std::string html =
      "<html><head><style>body { margin: 0; }</style></head>"
      "<body><h1>Cooldown Message</h1></body></html>";
  CacheOriginal(worker.cache(), url, html);

  std::thread worker_thread([&worker]() { worker.Run(); });
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  CacheNotification notification;
  notification.url = url;
  notification.scheme = "https";
  notification.content_type = ContentType::kHtml;
  notification.capability_mask = 0;

  // First notification — sets the tentative cooldown.
  SendNotification(notification);

  // Wait for processing to complete.
  CapabilityMask mask = CapabilityMask::Decode(0);
  for (int i = 0; i < 40 * kSanitizerBudgetScale; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    if (!ReadVariant(worker.cache(), url, mask).empty()) break;
  }

  uint64_t skipped_before = worker.stats().notifications_skipped_dedup.load();
  uint64_t html_before = worker.stats().html_processed.load();

  // Second notification immediately — should be skipped by either dedup
  // or cooldown, exercising the cooldown remaining-time log path.  Poll for
  // the skip counter to advance instead of a fixed sleep (slow shards can sit
  // at the pipe read past a fixed budget).
  SendNotification(notification);
  for (int i = 0; i < 80 * kSanitizerBudgetScale; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    if (worker.stats().notifications_skipped_dedup.load() > skipped_before)
      break;
  }

  // Verify the second notification was skipped.
  uint64_t skipped_after = worker.stats().notifications_skipped_dedup.load();
  EXPECT_GT(skipped_after, skipped_before)
      << "Second notification should be skipped by dedup/cooldown";
  // HTML should not be processed again.
  EXPECT_EQ(worker.stats().html_processed.load(), html_before)
      << "HTML should not be reprocessed within cooldown window";

  worker.Shutdown();
  worker_thread.join();
}

TEST_F(WorkerTest, SpeculationRulesInjectedWhenHotUrlsExist) {
  // When enable_speculation_rules is true and warmup notifications have
  // populated hot_urls_, subsequent HTML pages should have speculation
  // rules injected.  Covers lines 1707-1710.
  // Hot URLs are populated via warmup notifications (kWarmupSentinel mask),
  // not regular HTML processing notifications.
  WorkerConfig config;
  config.socket_path = socket_path_;
  config.cache_path = cache_path_;
  config.cache_size_bytes = static_cast<uint64_t>(10 * 1024 * 1024);
  config.enable_speculation_rules = true;
  config.enable_warmup = true;

  NullMessageHandler handler;
  Worker worker(config, &handler);
  ASSERT_TRUE(worker.Initialize());

  std::string hostname = "spectest.example.com";

  // Helper to cache HTML with a specific hostname.
  // Normalizes URL to path-only to match worker's NormalizeCacheUrl.
  auto cache_html = [&](const std::string& url, const std::string& html) {
    std::string norm_url = StripSchemeAuthority(url);
    CapabilityMask default_mask;
    AlternateId id =
        MaskToAlternateId(static_cast<uint8_t>(default_mask.Encode() & 0xFF));
    AlternateMetadata meta;
    meta.full_mask = default_mask.Encode();
    meta.content_type = ContentType::kOther;
    auto wh = worker.cache()->WriteAlternate(norm_url, hostname, "https", id,
                                             html.size(), meta);
    ASSERT_TRUE(wh.has_value());
    (void)wh->write_sync(std::as_bytes(std::span(html.data(), html.size())));
    (void)wh->close_sync();
  };

  // Cache the target page (page4) that will receive speculation rules.
  std::string target_url = "http://spectest.example.com/page4.html";
  std::string target_html =
      "<html><head><style>body { margin: 0; }</style></head>"
      "<body><h1>Page 4</h1></body></html>";
  cache_html(target_url, target_html);

  // Also cache the pages that will be used as hot URLs (so warmup
  // can read them from cache).
  for (int i = 0; i < 4; ++i) {
    std::string page_url =
        "http://spectest.example.com/page" + std::to_string(i) + ".html";
    std::string page_html =
        "<html><head><style>body { margin: 0; }</style></head>"
        "<body><h1>Page " +
        std::to_string(i) + "</h1></body></html>";
    cache_html(page_url, page_html);
  }

  std::thread worker_thread([&worker]() { worker.Run(); });
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  // Send warmup notifications (kWarmupSentinel mask) for pages 0-3.
  // This populates the hot_urls_ list via RecordHotUrl().
  for (int i = 0; i < 4; ++i) {
    CacheNotification notification;
    notification.url =
        "http://spectest.example.com/page" + std::to_string(i) + ".html";
    notification.scheme = "https";
    notification.hostname = hostname;
    notification.content_type = ContentType::kHtml;
    notification.capability_mask = kWarmupSentinel;
    SendNotification(notification);
  }

  // Wait for warmup processing.
  std::this_thread::sleep_for(std::chrono::milliseconds(500));

  // Now send a regular HTML notification for page4.
  // The hot URL list should be populated, so speculation rules
  // should be injected into the output.
  CacheNotification final_notif;
  final_notif.url = target_url;
  final_notif.scheme = "https";
  final_notif.hostname = hostname;
  final_notif.content_type = ContentType::kHtml;
  final_notif.capability_mask = CapabilityMask().Encode();
  SendNotification(final_notif);

  // Wait for page4 to be processed.
  for (int i = 0; i < 60 * kSanitizerBudgetScale; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    if (worker.stats().html_processed.load() >= 1) break;
  }
  EXPECT_GE(worker.stats().html_processed.load(), 1u)
      << "Page4 should be processed";

  // Read the variant for page4 and check for speculation rules.
  CapabilityMask mask;
  mask.set_image_format(CapabilityMask::ImageFormat::kOriginal);
  std::string variant =
      ReadVariant(worker.cache(), final_notif.url, mask, hostname);
  EXPECT_FALSE(variant.empty()) << "HTML variant for page4 not written";
  if (!variant.empty()) {
    EXPECT_NE(variant.find("speculationrules"), std::string::npos)
        << "Speculation rules should be injected when hot URLs exist. "
        << "Variant: " << variant.substr(0, 500);
  }

  worker.Shutdown();
  worker_thread.join();
}

TEST_F(WorkerTest, HtmlDetectsIncompleteCssViaRevalidationFlag) {
  // Verifies that when CSS A imports B (which is not cached), the CSS
  // variant is still written as worker-processed, and HTML can use it
  // for critical CSS extraction despite the missing import.
  WorkerConfig config;
  config.socket_path = socket_path_;
  config.cache_path = cache_path_;
  config.cache_size_bytes = static_cast<uint64_t>(10 * 1024 * 1024);

  NullMessageHandler handler;
  Worker worker(config, &handler);
  ASSERT_TRUE(worker.Initialize());

  std::string hostname = "example.com";
  std::string css_a_url = "http://example.com/incomplete-a.css";
  std::string css_a =
      "@import url(\"http://example.com/incomplete-b.css\");\n"
      ".hero  {  font-size:  24px;  color:  blue;  }\n";
  CacheOriginal(worker.cache(), css_a_url, css_a, hostname);

  std::string html_url = "http://example.com/detect-incomplete.html";
  std::string html =
      "<html><head>"
      "<link rel=\"stylesheet\" href=\"" +
      css_a_url +
      "\">"
      "</head><body>"
      "<div class=\"hero\">Hello</div>"
      "<img src=\"hero.jpg\">"
      "<img src=\"photo1.jpg\">"
      "<img src=\"photo2.jpg\">"
      "<img src=\"photo3.jpg\">"
      "<img src=\"photo4.jpg\">"
      "</body></html>";
  CacheOriginal(worker.cache(), html_url, html, hostname);

  std::thread worker_thread([&worker]() { worker.Run(); });
  WorkerStopper stopper(worker, worker_thread);
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  // Process CSS A first (with B missing).
  CacheNotification css_notif;
  css_notif.url = css_a_url;
  css_notif.scheme = "https";
  css_notif.hostname = hostname;
  css_notif.content_type = ContentType::kCss;
  css_notif.capability_mask = CapabilityMask().Encode();
  SendNotification(css_notif);

  // Wait for CSS A variant to appear.
  CapabilityMask mask;
  std::optional<VariantWithMeta> css_result;
  for (int i = 0; i < 40 * kSanitizerBudgetScale; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    css_result = ReadVariantWithMeta(worker.cache(), css_a_url, mask, hostname);
    if (css_result.has_value()) break;
  }
  ASSERT_TRUE(css_result.has_value()) << "CSS A variant not written";
  EXPECT_NE(
      css_result->metadata.flags & AlternateMetadata::kFlagWorkerProcessed, 0)
      << "CSS A should have kFlagWorkerProcessed set";

  // Now process HTML that references CSS A.
  CacheNotification html_notif;
  html_notif.url = html_url;
  html_notif.scheme = "https";
  html_notif.hostname = hostname;
  html_notif.content_type = ContentType::kHtml;
  html_notif.capability_mask = CapabilityMask().Encode();
  SendNotification(html_notif);

  // Poll for HTML variant.
  CapabilityMask html_mask;
  std::optional<VariantWithMeta> html_result;
  for (int i = 0; i < 40 * kSanitizerBudgetScale; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    html_result =
        ReadVariantWithMeta(worker.cache(), html_url, html_mask, hostname);
    if (html_result.has_value()) break;
  }

  ASSERT_TRUE(html_result.has_value()) << "HTML variant not written";
}

TEST_F(WorkerTest, HtmlVariantWriteSuccessLogs) {
  // Verify that successful HTML variant write increments the
  // html_assembly_complete counter and produces compressed variants.
  // Covers lines 1802-1817 (HTML write success path).
  WorkerConfig config;
  config.socket_path = socket_path_;
  config.cache_path = cache_path_;
  config.cache_size_bytes = static_cast<uint64_t>(10 * 1024 * 1024);

  NullMessageHandler handler;
  config.unsafe_force_async_css =
      true;  // fixture page is not under test for the gate
  Worker worker(config, &handler);
  ASSERT_TRUE(worker.Initialize());

  std::string url = "http://example.com/write-success.html";
  std::string html =
      "<html><head>"
      "<link rel=\"stylesheet\" href=\"http://example.com/ws.css\">"
      "</head><body>"
      "<div class=\"hero\">Content</div>"
      "<img src=\"photo1.jpg\">"
      "<img src=\"photo2.jpg\">"
      "<img src=\"photo3.jpg\">"
      "<img src=\"photo4.jpg\">"
      "</body></html>";
  CacheOriginal(worker.cache(), url, html);

  // Cache the CSS so critical CSS injection works.
  std::string css_url = "http://example.com/ws.css";
  std::string css = ".hero { font-size: 24px; } .footer { display: none; }";
  CacheOriginal(worker.cache(), css_url, css, "example.com");

  std::thread worker_thread([&worker]() { worker.Run(); });
  WorkerStopper stopper(worker, worker_thread);
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  CacheNotification notification;
  notification.url = url;
  notification.scheme = "https";
  notification.content_type = ContentType::kHtml;
  notification.capability_mask = 0;  // Mobile/Identity
  SendNotification(notification);

  // Poll for the HTML variant AND wait for all in-flight work to drain.
  // Compressed variants are written synchronously after the identity variant,
  // but Cyclone's write-around RAM cache can cache "not found" results from
  // early reads.  Waiting for in_flight_work()==0 ensures all writes
  // (identity + gz + br) are committed before we read.
  CapabilityMask mask = CapabilityMask::Decode(0);
  std::string variant;
  for (int i = 0; i < 40 * kSanitizerBudgetScale; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    variant = ReadVariant(worker.cache(), url, mask);
    if (!variant.empty() && worker.in_flight_work() == 0) break;
  }

  ASSERT_FALSE(variant.empty()) << "HTML variant should be written";

  // html_assembly_complete should have incremented.
  EXPECT_GE(worker.stats().html_assembly_complete.load(), 1u)
      << "html_assembly_complete should increment on successful write";

  // Compressed HTML variants should exist.  Since we waited for
  // in_flight_work()==0 above, all writes are complete.
  CapabilityMask gz_mask = CapabilityMask::Decode(0);
  gz_mask.set_transfer_encoding(CapabilityMask::TransferEncoding::kGzip);
  std::string gz_variant = ReadVariant(worker.cache(), url, gz_mask);
  EXPECT_FALSE(gz_variant.empty())
      << "Gzip HTML variant should exist after successful write";

  CapabilityMask br_mask = CapabilityMask::Decode(0);
  br_mask.set_transfer_encoding(CapabilityMask::TransferEncoding::kBrotli);
  std::string br_variant = ReadVariant(worker.cache(), url, br_mask);
  EXPECT_FALSE(br_variant.empty())
      << "Brotli HTML variant should exist after successful write";

  // The variant should have critical CSS injected.
  EXPECT_NE(variant.find("data-pagespeed-critical"), std::string::npos);
  // Should also have fetchpriority/lazy-load transforms.
  EXPECT_NE(variant.find("loading=\"lazy\""), std::string::npos)
      << "Lazy load should be applied to later images";
}

TEST_F(WorkerTest, HtmlEarlyHintsSentinelWriteContent) {
  // Verify that the early hints sentinel contains stylesheet URLs,
  // LCP image with image: prefix, and preconnect origins — all in
  // the same sentinel entry.
  // Covers lines 1582-1622 (early hints assembly and sentinel write).
  WorkerConfig config;
  config.socket_path = socket_path_;
  config.cache_path = cache_path_;
  config.cache_size_bytes = static_cast<uint64_t>(10 * 1024 * 1024);
  // Async CSS off, so the stylesheets stay render-blocking links and keep
  // their preload hints; this test asserts all three hint classes appear
  // together.
  config.disable_async_css = true;

  Worker worker(config, nullptr);
  ASSERT_TRUE(worker.Initialize());

  std::string url = "http://example.com/full-hints.html";
  std::string html =
      "<html><head>"
      "<link rel=\"stylesheet\" href=\"/main.css\">"
      "<link rel=\"stylesheet\" "
      "href=\"https://cdn.example.com/vendor.css\">"
      "<style>body { margin: 0; }</style>"
      "</head><body>"
      "<img src=\"https://img.example.com/hero.jpg\" "
      "fetchpriority=\"high\">"
      "<script src=\"https://analytics.example.com/track.js\"></script>"
      "<p>Content</p>"
      "</body></html>";
  CacheOriginal(worker.cache(), url, html);

  // Cache the stylesheets so critical CSS can be extracted.
  CacheOriginal(worker.cache(), "/main.css", "body { margin: 0; }");
  CacheOriginal(worker.cache(), "https://cdn.example.com/vendor.css",
                ".hero { font-size: 24px; }", "cdn.example.com");

  std::thread worker_thread([&worker]() { worker.Run(); });
  WorkerStopper stopper(worker, worker_thread);
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  CacheNotification notification;
  notification.url = url;
  notification.scheme = "https";
  notification.content_type = ContentType::kHtml;
  notification.capability_mask = CapabilityMask().Encode();
  SendNotification(notification);

  // Poll for early hints sentinel.
  std::string hints;
  for (int i = 0; i < 40 * kSanitizerBudgetScale; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    hints = ReadSentinel(worker.cache(), url, SentinelId::kEarlyHints);
    if (!hints.empty()) break;
  }

  ASSERT_FALSE(hints.empty()) << "Early hints sentinel should be stored";

  // Should contain stylesheet URLs.
  EXPECT_NE(hints.find("/main.css"), std::string::npos)
      << "Hints should contain /main.css. Got: " << hints;
  EXPECT_NE(hints.find("https://cdn.example.com/vendor.css"), std::string::npos)
      << "Hints should contain vendor.css URL. Got: " << hints;

  // Should contain LCP image with image: prefix.
  EXPECT_NE(hints.find("image:https://img.example.com/hero.jpg"),
            std::string::npos)
      << "Hints should contain LCP image with image: prefix. Got: " << hints;

  // Should contain preconnect entries for third-party origins.
  // At minimum the CDN and analytics origins should appear.
  EXPECT_NE(hints.find("preconnect:"), std::string::npos)
      << "Hints should contain preconnect entries. Got: " << hints;
}

// A page whose hero is loaded by script (a src-less
// `<img data-src>` placeholder plus its <noscript> copy, alone in the hero
// container) gets no `image:` Early Hint at all. Before, the hint named the
// next image, which may be below the fold; the hero's bytes are unknown to
// the worker, so the right hint is none. The stylesheet hint is unaffected,
// and the variant promotes nothing: no fetchpriority on the placeholder or
// on /below.jpg, no image preload.
TEST_F(WorkerTest, ScriptLoadedHeroGetsNoImageHint) {
  WorkerConfig config;
  config.socket_path = socket_path_;
  config.cache_path = cache_path_;
  config.cache_size_bytes = static_cast<uint64_t>(10 * 1024 * 1024);
  config.disable_async_css = true;

  Worker worker(config, nullptr);
  ASSERT_TRUE(worker.Initialize());

  std::string url = "http://example.com/lazy-hero.html";
  std::string html =
      "<html><head>"
      "<link rel=\"stylesheet\" href=\"/main.css\">"
      "</head><body>"
      "<div class=\"hero\">"
      "<img data-src=\"/hero.jpg\" class=\"lazy\">"
      "<noscript><img src=\"/hero.jpg\" width=\"1200\" height=\"600\">"
      "</noscript>"
      "</div>"
      "<div><img src=\"/below.jpg\"></div>"
      "</body></html>";
  CacheOriginal(worker.cache(), url, html);
  CacheOriginal(worker.cache(), "/main.css", "body { margin: 0; }");

  std::thread worker_thread([&worker]() { worker.Run(); });
  WorkerStopper stopper(worker, worker_thread);
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  CacheNotification notification;
  notification.url = url;
  notification.scheme = "https";
  notification.content_type = ContentType::kHtml;
  notification.capability_mask = CapabilityMask().Encode();
  SendNotification(notification);

  std::string hints;
  for (int i = 0; i < 40 * kSanitizerBudgetScale; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    hints = ReadSentinel(worker.cache(), url, SentinelId::kEarlyHints);
    if (!hints.empty()) break;
  }
  ASSERT_FALSE(hints.empty()) << "Early hints sentinel should be stored";
  EXPECT_NE(hints.find("/main.css"), std::string::npos) << hints;
  EXPECT_EQ(hints.find("image:"), std::string::npos)
      << "a script-loaded hero yields no image hint. Got: " << hints;

  CapabilityMask mask = CapabilityMask::Decode(CapabilityMask().Encode());
  std::string variant;
  for (int i = 0; i < 40 * kSanitizerBudgetScale; ++i) {
    variant = ReadVariant(worker.cache(), url, mask);
    if (!variant.empty()) break;
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
  }
  ASSERT_FALSE(variant.empty()) << "the optimized variant should be stored";
  EXPECT_EQ(variant.find("fetchpriority"), std::string::npos) << variant;
  EXPECT_EQ(variant.find("as=\"image\""), std::string::npos) << variant;
  EXPECT_NE(variant.find("<img data-src=\"/hero.jpg\" class=\"lazy\">"),
            std::string::npos)
      << variant;
  EXPECT_NE(variant.find("<img src=\"/below.jpg\">"), std::string::npos)
      << variant;
}

TEST_F(WorkerTest, DeferredSheetIsPreloadHinted) {
  // The inverse of the rule that stood while the deferral primitive was a
  // low-priority media technique.  The primitive IS a preload now, so a 103
  // hint announces exactly what the transform announces — the same fetch, at
  // the same priority, started before the HTML body.  Suppressing the hint
  // would throw away the whole point of the switch.  The LCP image hint stays
  // alongside it.
  WorkerConfig config = BrowserProfileConfig();

  Worker worker(config, nullptr);
  ASSERT_TRUE(worker.Initialize());

  std::string url = "http://example.com/deferred-hints.html";
  std::string css_url = "http://example.com/site.css";
  std::string css = ".hero{color:red;font-size:24px}p{margin:0}";
  std::string html =
      "<html><head>"
      "<link rel=\"stylesheet\" href=\"" +
      css_url +
      "\">"
      "</head><body>"
      "<div class=\"hero\">Hello</div>"
      "<img src=\"/hero.jpg\">"
      "</body></html>";
  CacheOriginal(worker.cache(), url, html);
  // Cache the sheet so the async-CSS gate can measure it (a small cached
  // sheet is allowed to defer), and seed the validation record deferral now
  // requires. The page has no inline <style>, so the combined sheet IS these
  // bytes.
  CacheOriginal(worker.cache(), css_url, css, "example.com");
  StoreBrowserProfile(worker, url, html, ".hero{color:red;font-size:24px}",
                      /*validated=*/true, /*validated_against_css=*/css);

  std::thread worker_thread([&worker]() { worker.Run(); });
  WorkerStopper stopper(worker, worker_thread);
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  CacheNotification notification;
  notification.url = url;
  notification.scheme = "https";
  notification.content_type = ContentType::kHtml;
  notification.capability_mask = 0;
  SendNotification(notification);

  // Wait until the variant shows the deferral actually happened.
  CapabilityMask mask = CapabilityMask::Decode(0);
  std::string variant;
  for (int i = 0; i < 80 * kSanitizerBudgetScale; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    variant = ReadVariant(worker.cache(), url, mask);
    if (variant.find("as=\"style\"") != std::string::npos) break;
  }
  ASSERT_NE(variant.find("as=\"style\""), std::string::npos)
      << "Precondition: the stylesheet should be async-deferred";
  ASSERT_NE(variant.find("rel=\"preload\""), std::string::npos)
      << "Precondition: the deferral primitive should be a preload. Got: "
      << variant;

  std::string hints =
      ReadSentinel(worker.cache(), url, SentinelId::kEarlyHints);
  ASSERT_FALSE(hints.empty()) << "Hints sentinel should be stored";
  EXPECT_NE(hints.find("site.css"), std::string::npos)
      << "A deferred stylesheet must be preload-hinted — the transform and "
         "the hint have to promote the same thing. Got: "
      << hints;
  EXPECT_NE(hints.find("image:/hero.jpg"), std::string::npos)
      << "The LCP image hint must remain. Got: " << hints;
}

// The sentinel line format is the contract between the worker and every 103
// emitter (nginx module, .NET middleware).  A stylesheet is written as a bare
// URL — no prefix — which is what makes the emitter render it as
// `rel=preload; as=style`.  Asserting the parsed resource rather than the raw
// line is what pins the END of that chain: a deferred sheet promoted as
// anything other than a style would not be matched to the link the loader
// flips, and Chrome would download the sheet twice.
TEST_F(WorkerTest, PreloadHintForDeferredSheetCarriesStyleType) {
  WorkerConfig config = BrowserProfileConfig();

  Worker worker(config, nullptr);
  ASSERT_TRUE(worker.Initialize());

  std::string url = "http://example.com/hint-as-style.html";
  std::string css_url = "http://example.com/typed.css";
  std::string css = ".hero{color:red;font-size:24px}p{margin:0}";
  std::string html =
      "<html><head>"
      "<link rel=\"stylesheet\" href=\"" +
      css_url +
      "\">"
      "</head><body>"
      "<div class=\"hero\">Hello</div>"
      "</body></html>";
  CacheOriginal(worker.cache(), url, html);
  CacheOriginal(worker.cache(), css_url, css, "example.com");
  StoreBrowserProfile(worker, url, html, ".hero{color:red;font-size:24px}",
                      /*validated=*/true, /*validated_against_css=*/css);

  std::thread worker_thread([&worker]() { worker.Run(); });
  WorkerStopper stopper(worker, worker_thread);
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  CacheNotification notification;
  notification.url = url;
  notification.scheme = "https";
  notification.content_type = ContentType::kHtml;
  notification.capability_mask = 0;
  SendNotification(notification);

  std::string hints;
  for (int i = 0; i < 80 * kSanitizerBudgetScale; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    hints = ReadSentinel(worker.cache(), url, SentinelId::kEarlyHints);
    if (hints.find("typed.css") != std::string::npos) break;
  }
  ASSERT_NE(hints.find("typed.css"), std::string::npos)
      << "Precondition: the deferred sheet must be hinted. Got: " << hints;

  // Find the sheet's line and run it through the emitters' own parser.
  std::string sheet_line;
  size_t start = 0;
  while (start <= hints.size()) {
    size_t nl = hints.find('\n', start);
    std::string line = hints.substr(
        start, (nl == std::string::npos ? hints.size() : nl) - start);
    if (line.find("typed.css") != std::string::npos) {
      sheet_line = line;
      break;
    }
    if (nl == std::string::npos) break;
    start = nl + 1;
  }
  ASSERT_FALSE(sheet_line.empty()) << hints;

  PreloadResource res = ParseHintLine(sheet_line);
  EXPECT_EQ(res.rel, "preload") << sheet_line;
  EXPECT_EQ(res.as_type, "style")
      << "a deferred stylesheet must be hinted as a STYLE, or the preload "
         "will not match the link the loader flips: "
      << sheet_line;
  EXPECT_NE(res.url.find("typed.css"), std::string::npos) << sheet_line;
}

TEST_F(WorkerTest, EarlyHintsKeepStylesheetWhenAsyncCssSuppressed) {
  // The stylesheet hint gate keys on the RUNTIME async-CSS decision, not on
  // config: with async-CSS enabled (default) but the external sheet not yet
  // cached, the FOUC sufficiency gate suppresses deferral — the sheet stays
  // render-blocking, so it MUST be preload-hinted.
  WorkerConfig config;
  config.socket_path = socket_path_;
  config.cache_path = cache_path_;
  config.cache_size_bytes = static_cast<uint64_t>(10 * 1024 * 1024);

  Worker worker(config, nullptr);
  ASSERT_TRUE(worker.Initialize());

  std::string url = "http://example.com/suppressed-hints.html";
  std::string html =
      "<html><head>"
      "<link rel=\"stylesheet\" href=\"http://example.com/site.css\">"
      "</head><body>"
      "<div class=\"hero\">Hello</div>"
      "</body></html>";
  // Only the HTML is cached — the stylesheet is NOT, so the sufficiency
  // gate keeps it render-blocking.
  CacheOriginal(worker.cache(), url, html);

  std::thread worker_thread([&worker]() { worker.Run(); });
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  CacheNotification notification;
  notification.url = url;
  notification.scheme = "https";
  notification.content_type = ContentType::kHtml;
  notification.capability_mask = 0;
  SendNotification(notification);

  std::string hints;
  for (int i = 0; i < 80 * kSanitizerBudgetScale; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    hints = ReadSentinel(worker.cache(), url, SentinelId::kEarlyHints);
    if (!hints.empty()) break;
  }

  EXPECT_NE(hints.find("site.css"), std::string::npos)
      << "A render-blocking (suppression-kept) stylesheet must be "
         "preload-hinted. Got: "
      << hints;

  worker.Shutdown();
  worker_thread.join();
}

TEST_F(WorkerTest, EarlyHintsStaleSentinelClearedWhenHintsBecomeEmpty) {
  // A page can stop having anything worth hinting — its only stylesheet
  // becomes print-only, its LCP image goes away, a third-party origin is
  // dropped.  When that happens the sentinel from the earlier pass must be
  // REMOVED, not left behind: a missing sentinel reads as "no hints", a stale
  // one keeps the 103 path promoting a resource the page no longer wants.
  //
  // Driven by seeding the stale sentinel directly rather than by producing it
  // from a first pass.  It used to be produced by one — while the deferral
  // primitive demoted the sheet, deferring it emptied the hint list — but a
  // deferred sheet is now hinted like any other, so that no longer empties
  // anything.  Seeding is also the sharper test: it isolates the removal
  // branch from whatever made the list non-empty in the first place.
  WorkerConfig config = BrowserProfileConfig();
  // Disable RAM cache: same-process cache writes must be visible to the
  // worker's next disk read (in production nginx writes from another process).
  config.ram_cache_size = 0;

  Worker worker(config, nullptr);
  ASSERT_TRUE(worker.Initialize());

  std::string url = "http://example.com/hints-go-empty.html";
  // Nothing on this page is hintable: the one stylesheet is print-only (never
  // render-blocking for screen), there is no image, and the relative href
  // yields no third-party origin.  Processing it must produce an EMPTY hint
  // list — which is what sends the emitter down the removal branch.
  std::string html =
      "<html><head>"
      "<link rel=\"stylesheet\" href=\"/site.css\" media=\"print\">"
      "</head><body>"
      "<div class=\"hero\">Hello</div>"
      "</body></html>";
  CacheOriginal(worker.cache(), url, html);

  // The stale sentinel a previous pass would have left behind. Keyed the way
  // the cache helpers key everything here: scheme+authority stripped, which is
  // also what the worker's NormalizeCacheUrl does before it writes.
  std::string stale = "/site.css\nimage:/gone.jpg";
  {
    auto wh =
        worker.cache()->WriteSentinel(StripSchemeAuthority(url), "", "https",
                                      SentinelId::kEarlyHints, stale.size());
    ASSERT_TRUE(wh.has_value());
    ASSERT_TRUE(
        wh->write_sync(std::as_bytes(std::span(stale.data(), stale.size())))
            .has_value());
    ASSERT_TRUE(wh->close_sync().has_value());
  }
  ASSERT_EQ(ReadSentinel(worker.cache(), url, SentinelId::kEarlyHints), stale)
      << "Precondition: the stale sentinel should be seeded";

  std::thread worker_thread([&worker]() { worker.Run(); });
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  CacheNotification notification;
  notification.url = url;
  notification.scheme = "https";
  notification.content_type = ContentType::kHtml;
  notification.capability_mask = 0;
  SendNotification(notification);

  // Poll for the removal: the emitter runs as part of processing, and a
  // missing sentinel reads back as empty.
  std::string hints = stale;
  for (int i = 0; i < 80 * kSanitizerBudgetScale; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    hints = ReadSentinel(worker.cache(), url, SentinelId::kEarlyHints);
    if (hints.empty()) break;
  }

  EXPECT_TRUE(hints.empty())
      << "The stale sentinel must be cleared when processing leaves no "
         "hints. Got: "
      << hints;

  worker.Shutdown();
  worker_thread.join();
}

TEST_F(WorkerTest, EmptySentinelOverwriteReadsAsNoHints) {
  // The in-process (.NET) pipeline cannot remove a single alternate through
  // the C API, so it clears stale hints by overwriting the sentinel with
  // EMPTY content.  Pin the round-trip: a zero-length sentinel write reads
  // back as empty content, which every consumer treats as "no hints".
  WorkerConfig config;
  config.socket_path = socket_path_;
  config.cache_path = cache_path_;
  config.cache_size_bytes = static_cast<uint64_t>(10 * 1024 * 1024);
  config.ram_cache_size = 0;

  Worker worker(config, nullptr);
  ASSERT_TRUE(worker.Initialize());

  std::string url = "example.com/empty-sentinel.html";
  std::string hints = "style.css\nimage:/hero.jpg";
  {
    auto wh = worker.cache()->WriteSentinel(
        url, "", "https", SentinelId::kEarlyHints, hints.size());
    ASSERT_TRUE(wh.has_value());
    ASSERT_TRUE(
        wh->write_sync(std::as_bytes(std::span(hints.data(), hints.size())))
            .has_value());
    ASSERT_TRUE(wh->close_sync().has_value());
  }
  EXPECT_EQ(ReadSentinel(worker.cache(), url, SentinelId::kEarlyHints), hints);

  {
    auto wh = worker.cache()->WriteSentinel(url, "", "https",
                                            SentinelId::kEarlyHints, 0);
    ASSERT_TRUE(wh.has_value());
    ASSERT_TRUE(wh->close_sync().has_value());
  }
  EXPECT_TRUE(
      ReadSentinel(worker.cache(), url, SentinelId::kEarlyHints).empty())
      << "A zero-length sentinel overwrite must read back as no hints";
}

TEST_F(WorkerTest, HtmlExternalCssGatheringWithMeta) {
  // When HTML references external stylesheets and the CSS is cached,
  // the worker should gather CSS and use it for critical CSS extraction.
  // Verify via metadata that kFlagNeedsRevalidation is NOT set when
  // all CSS is available.
  // Covers lines 1635-1657 (external CSS gathering loop).
  WorkerConfig config;
  config.socket_path = socket_path_;
  config.cache_path = cache_path_;
  config.cache_size_bytes = static_cast<uint64_t>(10 * 1024 * 1024);

  config.unsafe_force_async_css =
      true;  // fixture page is not under test for the gate
  Worker worker(config, nullptr);
  ASSERT_TRUE(worker.Initialize());

  std::string url = "http://example.com/css-gathered.html";
  std::string css_url = "http://example.com/gathered.css";
  std::string html =
      "<html><head>"
      "<link rel=\"stylesheet\" href=\"" +
      css_url +
      "\">"
      "</head><body><div class=\"hero\">Gathered CSS</div></body></html>";
  CacheOriginal(worker.cache(), url, html);

  // Cache the external CSS (absolute href, so raw_url=true).
  std::string css = ".hero { font-size: 24px; color: green; }";
  CacheOriginal(worker.cache(), css_url, css, "example.com");

  std::thread worker_thread([&worker]() { worker.Run(); });
  WorkerStopper stopper(worker, worker_thread);
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  CacheNotification notification;
  notification.url = url;
  notification.scheme = "https";
  notification.content_type = ContentType::kHtml;
  notification.capability_mask = 0;
  SendNotification(notification);

  // Poll for variant with metadata.
  CapabilityMask mask = CapabilityMask::Decode(0);
  std::optional<VariantWithMeta> result;
  for (int i = 0; i < 40 * kSanitizerBudgetScale; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    result = ReadVariantWithMeta(worker.cache(), url, mask);
    if (result.has_value()) break;
  }

  ASSERT_TRUE(result.has_value()) << "HTML variant should be written";

  // Critical CSS should be injected (CSS was available).
  EXPECT_NE(result->content.find("data-pagespeed-critical"), std::string::npos)
      << "Critical CSS should be injected when external CSS is available";

  // The hero class should appear in the critical CSS.
  EXPECT_NE(result->content.find("hero"), std::string::npos)
      << "Hero class should be in critical CSS";

  // kFlagNeedsRevalidation should NOT be set (CSS was found).
  EXPECT_EQ(result->metadata.flags & AlternateMetadata::kFlagNeedsRevalidation,
            0)
      << "kFlagNeedsRevalidation should not be set when CSS is available";

  // kFlagWorkerProcessed should be SET.
  EXPECT_NE(result->metadata.flags & AlternateMetadata::kFlagWorkerProcessed, 0)
      << "kFlagWorkerProcessed should be set on worker-written variant";
}

// =============================================================================
// Pages stored by an earlier metadata version are rebuilt
// =============================================================================

// Writes worker-built HTML the way metadata version 8 stored it, header copy
// included, at the slot `mask` selects.
void StoreAtVersionEight(PageSpeedCache* cache, const std::string& path,
                         const CapabilityMask& mask, const std::string& body) {
  AlternateMetadata meta;
  meta.full_mask = mask.Encode();
  meta.content_type = ContentType::kHtml;
  meta.flags = AlternateMetadata::kFlagWorkerProcessed;
  meta.origin_content_type = "text/html";
  meta.origin_cache_control = "max-age=600";
  auto prefix = meta.Serialize();
  prefix[0] = std::byte{8};
  std::vector<std::byte> blob = prefix;
  for (char c : body) blob.push_back(static_cast<std::byte>(c));
  auto key = PageSpeedCache::ComposeKeyPreNormalized(path, "", "https");
  auto wh = PageSpeedCacheTestPeer::Cyclone(*cache).write_alternate_sync(
      key,
      static_cast<cyclone::AlternateId>(
          MaskToAlternateId(static_cast<uint8_t>(mask.Encode() & 0xFF))),
      blob.size());
  ASSERT_TRUE(wh.has_value());
  wh->set_header(prefix);
  ASSERT_TRUE(wh->write_sync(std::span(blob)).has_value());
  ASSERT_TRUE(wh->close_sync().has_value());
}

// What the front end does on the miss: record the origin's bytes at the
// identity slot.
void RecordOrigin(PageSpeedCache* cache, const std::string& path,
                  const std::string& html) {
  CapabilityMask identity;
  AlternateMetadata meta;
  meta.full_mask = identity.Encode();
  meta.content_type = ContentType::kHtml;
  meta.origin_content_type = "text/html";
  meta.origin_cache_control = "max-age=600";
  auto wh = cache->WriteAlternate(
      path, "", "https",
      MaskToAlternateId(static_cast<uint8_t>(identity.Encode() & 0xFF)),
      html.size(), meta);
  ASSERT_TRUE(wh.has_value());
  ASSERT_TRUE(wh->write_sync(std::as_bytes(std::span(html.data(), html.size())))
                  .has_value());
  ASSERT_TRUE(wh->close_sync().has_value());
}

CapabilityMask WithEncoding(CapabilityMask::TransferEncoding encoding) {
  CapabilityMask mask;
  mask.set_transfer_encoding(encoding);
  return mask;
}

TEST_F(WorkerTest, HtmlStoredByEarlierVersionIsRebuiltFromTheOrigin) {
  // A cache written before metadata version 9 holds worker-built HTML: the
  // page and its gzip and brotli copies.  Those read as a miss; the front end
  // records the origin's bytes again and notifies, and the worker must write
  // a fresh page and fresh copies.
  WorkerConfig config;
  config.socket_path = socket_path_;
  config.cache_path = cache_path_;
  config.cache_size_bytes = static_cast<uint64_t>(10 * 1024 * 1024);

  Worker worker(config, nullptr);
  ASSERT_TRUE(worker.Initialize());

  const std::string url = "http://example.com/earlier-version.html";
  const std::string path = StripSchemeAuthority(url);
  const CapabilityMask identity;
  const CapabilityMask gzip =
      WithEncoding(CapabilityMask::TransferEncoding::kGzip);
  const CapabilityMask brotli =
      WithEncoding(CapabilityMask::TransferEncoding::kBrotli);
  for (const auto& mask : {identity, gzip, brotli}) {
    ASSERT_NO_FATAL_FAILURE(
        StoreAtVersionEight(worker.cache(), path, mask, "EARLIER"));
  }
  for (const auto& mask : {identity, gzip, brotli}) {
    EXPECT_FALSE(
        worker.cache()->ReadBestAlternate(path, "", "https", mask).has_value());
  }

  const std::string html =
      "<html><head></head><body><h1>Rebuilt page</h1>"
      "<img src=\"photo1.jpg\"><img src=\"photo2.jpg\">"
      "<img src=\"photo3.jpg\"><img src=\"photo4.jpg\">"
      "</body></html>";
  ASSERT_NO_FATAL_FAILURE(RecordOrigin(worker.cache(), path, html));

  std::thread worker_thread([&worker]() { worker.Run(); });
  WorkerStopper stopper(worker, worker_thread);
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  CacheNotification notification;
  notification.url = url;
  notification.scheme = "https";
  notification.content_type = ContentType::kHtml;
  notification.capability_mask = identity.Encode();  // a desktop client
  SendNotification(notification);
  ASSERT_NO_FATAL_FAILURE(WaitForNotificationRetiredWithVariant(worker));

  // This test shares the worker's process and its RAM tier, which writes do
  // not refresh: drop what the reads above left there.
  for (const auto& mask : {identity, gzip, brotli}) {
    worker.cache()->EvictAlternateFromRamCache(
        path, "", "https",
        MaskToAlternateId(static_cast<uint8_t>(mask.Encode() & 0xFF)));
  }

  auto page = ReadVariantWithMeta(worker.cache(), url, identity);
  ASSERT_TRUE(page.has_value()) << "the page was not rebuilt";
  EXPECT_EQ(page->content.find("EARLIER"), std::string::npos);
  EXPECT_NE(page->content.find("Rebuilt page"), std::string::npos);
  EXPECT_NE(page->content.find("loading=\"lazy\""), std::string::npos);
  EXPECT_NE(page->metadata.flags & AlternateMetadata::kFlagWorkerProcessed, 0);
  EXPECT_EQ(page->metadata.version, AlternateMetadata::kCurrentVersion);

  for (const auto& mask : {gzip, brotli}) {
    auto copy = ReadVariantWithMeta(worker.cache(), url, mask);
    ASSERT_TRUE(copy.has_value()) << "a compressed copy was not rebuilt";
    EXPECT_EQ(copy->content.find("EARLIER"), std::string::npos);
    EXPECT_NE(copy->metadata.flags & AlternateMetadata::kFlagWorkerProcessed,
              0);
    EXPECT_EQ(copy->metadata.version, AlternateMetadata::kCurrentVersion);
  }
}

TEST_F(WorkerTest, HtmlStoredByEarlierVersionThatNowStaysUnchangedIsServed) {
  // The same starting cache, but the page the origin now returns gets no
  // transform, so the worker writes no copies over the earlier ones.  Those
  // must not keep every compressed request on a miss: the recorded page is
  // what every client is served.
  WorkerConfig config;
  config.socket_path = socket_path_;
  config.cache_path = cache_path_;
  config.cache_size_bytes = static_cast<uint64_t>(10 * 1024 * 1024);
  config.enable_speculation_rules = false;

  Worker worker(config, nullptr);
  ASSERT_TRUE(worker.Initialize());

  const std::string url = "http://example.com/earlier-unchanged.html";
  const std::string path = StripSchemeAuthority(url);
  const CapabilityMask identity;
  const CapabilityMask gzip =
      WithEncoding(CapabilityMask::TransferEncoding::kGzip);
  const CapabilityMask brotli =
      WithEncoding(CapabilityMask::TransferEncoding::kBrotli);
  for (const auto& mask : {identity, gzip, brotli}) {
    ASSERT_NO_FATAL_FAILURE(
        StoreAtVersionEight(worker.cache(), path, mask, "EARLIER"));
  }
  const std::string html =
      "<html><head></head><body><p>Plain text only</p></body></html>";
  ASSERT_NO_FATAL_FAILURE(RecordOrigin(worker.cache(), path, html));

  std::thread worker_thread([&worker]() { worker.Run(); });
  WorkerStopper stopper(worker, worker_thread);
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  CacheNotification notification;
  notification.url = url;
  notification.scheme = "https";
  notification.content_type = ContentType::kHtml;
  notification.capability_mask = identity.Encode();  // a desktop client
  SendNotification(notification);
  ASSERT_TRUE(pagespeed::test::WaitForNotificationsRetired(worker, 1,
                                                           kWorkerStallBudget));
  EXPECT_EQ(worker.stats().errors.load(), 0u);

  for (const auto& mask : {identity, gzip, brotli}) {
    auto served = worker.cache()->ReadBestAlternate(path, "", "https", mask);
    ASSERT_TRUE(served.has_value())
        << "encoding " << static_cast<int>(mask.transfer_encoding());
    auto content = served->content();
    EXPECT_EQ(std::string(reinterpret_cast<const char*>(content.data()),
                          content.size()),
              html);
  }
}

}  // namespace
}  // namespace pagespeed
