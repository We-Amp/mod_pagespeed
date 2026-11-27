// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

// Agent markdown and content-hash idempotency tests
//
// - Content-hash self-purge and own-write-failure loops
// - Agent markdown variant durability across origin refreshes
//
// One of the topic files of //test/src/worker:worker_test; the shared fixture
// lives in worker_test_fixture.h.

// clang-format off
// Keep the fixture header first: it preserves the old worker_test.cc include
// order (worker.h before any Windows header), which Windows builds have proven.
// See lib/base/message_handler.h for the FormatMessage macro interaction.
#include "test/src/worker/worker_test_fixture.h"
// clang-format on

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <ctime>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "gtest/gtest.h"
#include "lib/base/message_handler.h"
#include "lib/cache/cache.h"
#include "lib/cache/freshness.h"
#include "lib/cache/html_withheld_marker.h"
#include "lib/classify/alternate_id.h"
#include "lib/classify/alternate_metadata.h"
#include "lib/classify/capability_mask.h"
#include "lib/net/upstream_pin.h"
#include "sha256.hpp"
#include "src/browser/analysis_queue.h"
#include "src/browser/page_analysis.h"
#include "src/proto/worker_ipc.h"
#include "src/worker/browser_analysis_manager.h"
#include "src/worker/browser_analysis_manager_internal.h"
#include "src/worker/html_scanner.h"
#include "src/worker/subresource_gate.h"
#include "src/worker/worker.h"
#include "test/test_util/worker_quiescence.h"

namespace pagespeed {
namespace {

// =============================================================================
// Issue B: content-hash self-purge + own-write-failure loop (agent_optimize)
//
// Root cause: the kContentHash sentinel is computed by re-hashing the worker's
// READ of slot 0x08, which flips raw<->worker-optimized over time.  When a
// re-notification reads the worker's OWN optimized bytes but the sentinel was
// written over raw bytes, content_changed falsely fires, the whole key is
// purged, and the in-flight HTML write is fenced by its now-stale dispatch
// baseline — failing its own write and double-counting write_failures+errors.
// =============================================================================

// Build an HTML/agent_optimize WorkerConfig that exercises the content-hash
// sentinel path.  browser_analysis.enabled=true with a nonexistent Chrome so
// the manager falls back to the heuristic critical-CSS extractor (no real
// render), while agent_optimize=true turns on the content-hash sentinel logic.
static WorkerConfig MakeAgentOptimizeConfig(const std::string& socket_path,
                                            const std::string& cache_path) {
  WorkerConfig config;
  config.socket_path = socket_path;
  config.cache_path = cache_path;
  config.cache_size_bytes = static_cast<uint64_t>(10 * 1024 * 1024);
  // The test binary runs as root in the build container,
  // where the Chrome sandbox is (correctly) unavailable and `require`
  // would refuse to construct the manager.  These tests seed profiles
  // directly and never spawn Chrome, so take the documented opt-out.
  config.browser_analysis.sandbox_mode = BrowserSandboxMode::kOff;
  config.browser_analysis.enabled = true;
  config.browser_analysis.chrome_binary = "/nonexistent/chrome";
  config.browser_analysis.agent_optimize = true;
  return config;
}

// HTML with inline CSS so the heuristic CriticalCssExtractor yields critical
// CSS, the worker writes an optimized identity variant (flags get
// kFlagWorkerProcessed), and the kContentHash sentinel is written.
static const char* const kAgentHtml =
    "<html><head><style>body { margin: 0; } "
    "h1 { color: red; }</style></head>"
    "<body><h1>Hello World</h1></body></html>";

TEST_F(WorkerTest, ContentHashIdempotency_OptimizedIdentityDoesNotSelfPurge) {
  // Two notifications for the same URL with NO origin change must not
  // self-purge.  Pre-fix the 2nd notification re-hashes the worker's own
  // optimized slot 0x08, mismatches the raw-origin sentinel, purges, and
  // fails its in-flight write — bumping content_hash_stale +
  // alternate_write_failures + errors in lockstep (the live prod bug).
  WorkerConfig config = MakeAgentOptimizeConfig(socket_path_, cache_path_);
  NullMessageHandler handler;
  config.unsafe_force_async_css =
      true;  // fixture page is not under test for the gate
  Worker worker(config, &handler);
  ASSERT_TRUE(worker.Initialize());

  std::string cache_url = "/test.html";
  std::string notification_url = "http://example.com/test.html";
  CacheOriginal(worker.cache(), cache_url, kAgentHtml);

  std::thread worker_thread([&worker]() { worker.Run(); });
  WorkerStopper stopper(worker, worker_thread);
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  CapabilityMask opt_mask;  // Desktop/Identity = 0x08
  CacheNotification notification;
  notification.url = notification_url;
  notification.scheme = "https";
  notification.content_type = ContentType::kHtml;
  notification.capability_mask = opt_mask.Encode();

  // First notification: worker writes the optimized identity + sentinel.
  SendNotification(notification);
  // Wait for the first work item to retire: the variant appearing is not
  // enough, because the URL stays marked in flight until the item is done and
  // the repeat below would be turned away.
  ASSERT_TRUE(pagespeed::test::WaitForNotificationsRetired(worker, 1,
                                                           kWorkerStallBudget));
  std::string variant = ReadVariant(worker.cache(), cache_url, opt_mask);
  ASSERT_FALSE(variant.empty()) << "First HTML variant not written";
  // Sanity: the identity slot now carries the worker-processed optimized
  // bytes (this is what the 2nd read re-hashes, exposing the bug).
  auto opt_meta = ReadVariantWithMeta(worker.cache(), cache_url, opt_mask);
  ASSERT_TRUE(opt_meta.has_value());
  EXPECT_NE(opt_meta->metadata.flags & AlternateMetadata::kFlagWorkerProcessed,
            0)
      << "Optimized identity must carry kFlagWorkerProcessed";

  uint64_t chs_before = worker.stats().content_hash_stale.load();
  uint64_t wf_before = worker.stats().alternate_write_failures.load();
  uint64_t err_before = worker.stats().errors.load();

  // Clear dedup/cooldown (exactly what origin-refresh does) so the SECOND
  // notification re-enters the content-hash block, reading the worker's own
  // optimized slot 0x08 — WITHOUT re-writing raw.
  worker.ClearDedupAndCooldown(notification_url, "", "https");
  SendNotification(notification);
  ASSERT_TRUE(pagespeed::test::WaitForNotificationsRetired(worker, 2,
                                                           kWorkerStallBudget));
  // The assertions below only mean something if the repeat was actually
  // processed: a notification turned away (in flight, or deduplicated) leaves
  // every counter they check unchanged.
  ASSERT_EQ(worker.stats().notifications_skipped_inflight.load(), 0u)
      << "repeat notification was rejected as already in flight";
  ASSERT_EQ(worker.stats().notifications_skipped_dedup.load(), 0u)
      << "repeat notification was skipped as a duplicate";
  ASSERT_EQ(worker.stats().html_processed.load(), 2u)
      << "repeat notification was not processed";

  EXPECT_EQ(worker.stats().content_hash_stale.load(), chs_before)
      << "No origin change: must NOT self-purge (content_hash_stale stable)";
  EXPECT_EQ(worker.stats().alternate_write_failures.load(), wf_before)
      << "No origin change: must NOT hard-fail its own write";
  EXPECT_EQ(worker.stats().errors.load(), err_before)
      << "No origin change: must NOT increment errors";
  EXPECT_TRUE(worker.cache()->AlternateExists(
      cache_url, "", "https",
      MaskToAlternateId(static_cast<uint8_t>(opt_mask.Encode() & 0xFF))))
      << "Variant set must survive a no-change reprocess";
}

// A markdown-* html_mode writes no optimized HTML variant for the front
// sender, while the page is still read and the content hash the Markdown copy
// binds to is still written.  `mode` full is the positive control: the same
// page and config do write the variant.
class HtmlModeVariantTest : public WorkerTest {
 protected:
  static constexpr const char* kCacheUrl = "/mode.html";

  static CacheNotification PageNotification() {
    CacheNotification notification;
    notification.url = "http://example.com/mode.html";
    notification.scheme = "https";
    notification.content_type = ContentType::kHtml;
    notification.capability_mask = CapabilityMask().Encode();
    return notification;
  }

  WorkerConfig ModeConfig(HtmlMode mode) {
    WorkerConfig config = MakeAgentOptimizeConfig(socket_path_, cache_path_);
    config.unsafe_force_async_css = true;
    config.html_mode = mode;
    return config;
  }

  void Run(HtmlMode mode, bool expect_variant) {
    NullMessageHandler handler;
    Worker worker(ModeConfig(mode), &handler);
    ASSERT_TRUE(worker.Initialize());
    CacheOriginal(worker.cache(), kCacheUrl, kAgentHtml);
    std::thread worker_thread([&worker]() { worker.Run(); });
    WorkerStopper stopper(worker, worker_thread);
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    SendNotification(PageNotification());
    ASSERT_TRUE(pagespeed::test::WaitForNotificationsRetired(
        worker, 1, kWorkerStallBudget));

    EXPECT_FALSE(
        ReadSentinel(worker.cache(), kCacheUrl, SentinelId::kContentHash)
            .empty())
        << HtmlModeName(mode) << ": the page was not read";
    // The analysis that renders the agent copy was still queued.
    EXPECT_TRUE(worker.TestBrowserManager()->TestDequeue().has_value())
        << HtmlModeName(mode) << ": no analysis was queued";
    // The identity slot holds the raw origin until the worker replaces it
    // with its own output, so "a variant" is an entry the worker wrote.
    auto slot =
        ReadVariantWithMeta(worker.cache(), kCacheUrl, CapabilityMask());
    ASSERT_TRUE(slot.has_value()) << HtmlModeName(mode);
    const bool has_variant =
        (slot->metadata.flags & AlternateMetadata::kFlagWorkerProcessed) != 0;
    EXPECT_EQ(has_variant, expect_variant) << HtmlModeName(mode);
    EXPECT_EQ(worker.stats().html_processed.load(), expect_variant ? 1u : 0u)
        << HtmlModeName(mode);
    EXPECT_EQ(worker.stats().variants_written.load() > 0, expect_variant)
        << HtmlModeName(mode);
    if (!expect_variant) {
      // The raw entry carries its source-binding stamp (the derived ETag for
      // a validator-less origin), with its bytes unchanged.
      static constexpr std::array<std::byte, 32> kZeroHash{};
      EXPECT_NE(slot->metadata.origin_html_hash, kZeroHash)
          << HtmlModeName(mode) << ": raw entry not stamped";
      EXPECT_EQ(slot->content, kAgentHtml) << HtmlModeName(mode);
    }
  }
};

// A markdown-* html_mode writes no optimized HTML variant for the front
// sender, while the page is still read, its content hash written, its raw
// entry stamped and its analysis queued.  Full is the positive control: the
// same page and config do write the variant.
TEST_F(HtmlModeVariantTest, FullWritesTheHtmlVariant) {
  Run(HtmlMode::kFull, /*expect_variant=*/true);
}

TEST_F(HtmlModeVariantTest, MarkdownStaticWritesNoHtmlVariant) {
  Run(HtmlMode::kMarkdownStatic, /*expect_variant=*/false);
}

TEST_F(HtmlModeVariantTest, MarkdownRenderedWritesNoHtmlVariant) {
  Run(HtmlMode::kMarkdownRendered, /*expect_variant=*/false);
}

// In a markdown-* mode no variant marks the page processed, so a repeat for
// an unchanged page is skipped by its origin hash; a changed origin is not.
TEST_F(HtmlModeVariantTest, MarkdownRepeatSkippedUntilTheOriginChanges) {
  NullMessageHandler handler;
  Worker worker(ModeConfig(HtmlMode::kMarkdownStatic), &handler);
  ASSERT_TRUE(worker.Initialize());
  CacheOriginal(worker.cache(), kCacheUrl, kAgentHtml);
  std::thread worker_thread([&worker]() { worker.Run(); });
  WorkerStopper stopper(worker, worker_thread);
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  SendNotification(PageNotification());
  ASSERT_TRUE(pagespeed::test::WaitForNotificationsRetired(worker, 1,
                                                           kWorkerStallBudget));
  ASSERT_TRUE(worker.TestBrowserManager()->TestDequeue().has_value());
  const std::string first_hash =
      ReadSentinel(worker.cache(), kCacheUrl, SentinelId::kContentHash);
  ASSERT_EQ(first_hash.size(), 32u);

  // Same bytes, repeated: skipped before the scan and the analysis lookup.
  const uint64_t skipped_before =
      worker.stats().notifications_skipped_dedup.load();
  for (int i = 0; i < 3; ++i) {
    SendNotification(PageNotification());
    ASSERT_TRUE(pagespeed::test::WaitForNotificationsRetired(
        worker, 2 + i, kWorkerStallBudget));
  }
  EXPECT_EQ(worker.stats().notifications_skipped_dedup.load(),
            skipped_before + 3);
  EXPECT_FALSE(worker.TestBrowserManager()->TestDequeue().has_value())
      << "an unchanged page was queued again";

  // A changed origin is processed: the content hash moves to the new bytes
  // and the page is queued again.
  const std::string changed =
      "<html><head><style>body { margin: 1px; }</style></head>"
      "<body><h1>Changed</h1></body></html>";
  CacheOriginal(worker.cache(), kCacheUrl, changed);
  SendNotification(PageNotification());
  ASSERT_TRUE(pagespeed::test::WaitForNotificationsRetired(worker, 5,
                                                           kWorkerStallBudget));
  EXPECT_EQ(worker.stats().notifications_skipped_dedup.load(),
            skipped_before + 3)
      << "a changed origin was skipped";
  const std::string second_hash =
      ReadSentinel(worker.cache(), kCacheUrl, SentinelId::kContentHash);
  EXPECT_EQ(second_hash.size(), 32u);
  EXPECT_NE(second_hash, first_hash);
  EXPECT_TRUE(worker.TestBrowserManager()->TestDequeue().has_value())
      << "the changed page was not queued";
}

TEST_F(WorkerTest, ContentHashIdempotency_RealOriginChangeStillPurges) {
  // A GENUINE origin change (raw bytes at slot 0x08 differ from the bound
  // sentinel) must still purge exactly once and rebuild — the fix must not
  // suppress real invalidation.
  WorkerConfig config = MakeAgentOptimizeConfig(socket_path_, cache_path_);
  NullMessageHandler handler;
  Worker worker(config, &handler);
  ASSERT_TRUE(worker.Initialize());

  std::string cache_url = "/test.html";
  std::string notification_url = "http://example.com/test.html";
  CacheOriginal(worker.cache(), cache_url, kAgentHtml);

  std::thread worker_thread([&worker]() { worker.Run(); });
  WorkerStopper stopper(worker, worker_thread);
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  CapabilityMask opt_mask;
  CacheNotification notification;
  notification.url = notification_url;
  notification.scheme = "https";
  notification.content_type = ContentType::kHtml;
  notification.capability_mask = opt_mask.Encode();

  SendNotification(notification);
  std::string variant;
  for (int i = 0; i < 60 * kSanitizerBudgetScale; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    variant = ReadVariant(worker.cache(), cache_url, opt_mask);
    if (worker.stats().html_assembly_complete.load() >= 1 && !variant.empty())
      break;
  }
  ASSERT_FALSE(variant.empty()) << "First HTML variant not written";

  uint64_t chs_before = worker.stats().content_hash_stale.load();

  // Overwrite slot 0x08 with DIFFERENT raw origin bytes (flags=0, i.e. a real
  // origin refresh re-recording fresh raw content), then re-notify.
  std::string new_html =
      "<html><head><style>body { padding: 8px; } "
      "h2 { color: blue; }</style></head>"
      "<body><h2>Brand New Origin</h2></body></html>";
  ASSERT_NE(new_html, std::string(kAgentHtml));
  CacheOriginal(worker.cache(), cache_url, new_html);  // flags default 0 (raw)
  // Evict THIS process's RAM-tier copy of the identity so the re-notification
  // reads the freshly-written RAW bytes (flags=0), not the stale optimized
  // identity left in the write-around RAM tier.  This is exactly what
  // HandleOriginRefreshed does before re-dispatching in production.
  CapabilityMask identity_mask;
  worker.cache()->EvictAlternateFromRamCache(
      cache_url, "", "https",
      MaskToAlternateId(static_cast<uint8_t>(identity_mask.Encode() & 0xFF)));

  worker.ClearDedupAndCooldown(notification_url, "", "https");
  SendNotification(notification);
  for (int i = 0; i < 60 * kSanitizerBudgetScale; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    if (worker.stats().content_hash_stale.load() > chs_before) break;
  }

  EXPECT_EQ(worker.stats().content_hash_stale.load(), chs_before + 1)
      << "A genuine origin change must purge exactly once";
}

TEST_F(WorkerTest, AgentMarkdownStaysServedAcrossReprocess) {
  // The complete-fix gate (forces durable raw-origin hash plumbing, not the
  // option-a stop-gap alone): a bound agent markdown variant must remain
  // SERVABLE after a no-change reprocess.  The serve gate
  // (ReadBestAlternateAgentByKey) requires the markdown's stamped
  // origin_html_hash to byte-equal the LIVE kContentHash sentinel.  Pre-fix
  // (and with option-a only) the reprocess rewrites the sentinel to the
  // optimized-content hash, so the markdown stamp != live sentinel -> the
  // serve gate refuses (NotFound) and HTML is served instead.
  WorkerConfig config = MakeAgentOptimizeConfig(socket_path_, cache_path_);
  NullMessageHandler handler;
  config.unsafe_force_async_css =
      true;  // fixture page is not under test for the gate
  Worker worker(config, &handler);
  ASSERT_TRUE(worker.Initialize());

  std::string cache_url = "/test.html";
  std::string notification_url = "http://example.com/test.html";
  CacheOriginal(worker.cache(), cache_url, kAgentHtml);

  std::thread worker_thread([&worker]() { worker.Run(); });
  WorkerStopper stopper(worker, worker_thread);
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  CapabilityMask opt_mask;
  CacheNotification notification;
  notification.url = notification_url;
  notification.scheme = "https";
  notification.content_type = ContentType::kHtml;
  notification.capability_mask = opt_mask.Encode();

  // First notification: worker writes optimized identity + the kContentHash
  // sentinel = SHA256(raw).
  SendNotification(notification);
  // Wait for the first work item to retire: the variant appearing is not
  // enough, because the URL stays marked in flight until the item is done and
  // the repeat below would be turned away.
  ASSERT_TRUE(pagespeed::test::WaitForNotificationsRetired(worker, 1,
                                                           kWorkerStallBudget));
  std::string variant = ReadVariant(worker.cache(), cache_url, opt_mask);
  ASSERT_FALSE(variant.empty()) << "First HTML variant not written";

  // Read the live kContentHash sentinel (SHA256 of the raw origin HTML) and
  // stamp a synthetic agent markdown variant (id=0x7C) with it — emulating a
  // completed render whose binding matches the current origin.  (The worker
  // test harness has no Chrome, so we write the markdown directly.)
  std::string sentinel =
      ReadSentinel(worker.cache(), cache_url, SentinelId::kContentHash);
  ASSERT_EQ(sentinel.size(), AlternateMetadata::kHashSize)
      << "kContentHash sentinel must be written after first processing";

  AlternateMetadata md_meta;
  md_meta.full_mask = static_cast<uint32_t>(SentinelId::kAgentMarkdown);
  md_meta.content_type = ContentType::kOther;
  md_meta.origin_content_type = "text/markdown";
  md_meta.flags |= AlternateMetadata::kFlagWorkerProcessed;
  std::memcpy(md_meta.origin_html_hash.data(), sentinel.data(),
              AlternateMetadata::kHashSize);
  std::string markdown = "# Hello World\n\nMarkdown body.\n";
  auto wh = worker.cache()->WriteAgentAlternate(cache_url, "", "https",
                                                markdown.size(), md_meta);
  ASSERT_TRUE(wh.has_value());
  ASSERT_TRUE(
      wh->write_sync(std::as_bytes(std::span(markdown.data(), markdown.size())))
          .has_value());
  ASSERT_TRUE(wh->close_sync().has_value());

  // Sanity: the markdown serves now (binding fresh).
  CapabilityMask agent_mask;
  auto served_before =
      worker.cache()->ReadBestAlternateAgent(cache_url, "", "https", agent_mask,
                                             /*agent_request_entitled=*/true);
  ASSERT_TRUE(served_before.has_value())
      << "Markdown must be servable before reprocess (binding fresh)";

  // Re-process WITHOUT an origin change.
  worker.ClearDedupAndCooldown(notification_url, "", "https");
  SendNotification(notification);
  ASSERT_TRUE(pagespeed::test::WaitForNotificationsRetired(worker, 2,
                                                           kWorkerStallBudget));
  // The assertions below only mean something if the repeat was actually
  // processed: a notification turned away (in flight, or deduplicated) leaves
  // every counter they check unchanged.
  ASSERT_EQ(worker.stats().notifications_skipped_inflight.load(), 0u)
      << "repeat notification was rejected as already in flight";
  ASSERT_EQ(worker.stats().notifications_skipped_dedup.load(), 0u)
      << "repeat notification was skipped as a duplicate";
  ASSERT_EQ(worker.stats().html_processed.load(), 2u)
      << "repeat notification was not processed";

  // The markdown variant must STILL exist and STILL be servable — meaning the
  // live sentinel was NOT rewritten to an optimized-content hash.
  EXPECT_TRUE(worker.cache()->AlternateExists(
      cache_url, "", "https",
      static_cast<AlternateId>(SentinelId::kAgentMarkdown)))
      << "Agent markdown variant must survive a no-change reprocess";
  auto served_after =
      worker.cache()->ReadBestAlternateAgent(cache_url, "", "https", agent_mask,
                                             /*agent_request_entitled=*/true);
  EXPECT_TRUE(served_after.has_value())
      << "Markdown binding must remain fresh after reprocess: the live "
         "kContentHash sentinel must stay SHA256(raw), not the optimized hash";
}

// =============================================================================
// Durability (#19/D2): the origin-refresh sentinel fires on every TTL
// expiry / 200 revalidation, even when the re-fetched body is byte-identical.
// The rendered markdown variant must be PRESERVED on an unchanged-content
// refresh (still bound to live content) and PURGED on a genuine content change
// (superseded — never served stale).  These integration tests drive the real
// HandleOriginRefreshed purge path end-to-end (the harness has no Chrome, so the
// markdown variant is stamped directly, as AgentMarkdownStaysServedAcrossReprocess
// does).
// =============================================================================

TEST_F(WorkerTest, AgentMarkdownPreservedOnUnchangedOriginRefresh) {
  WorkerConfig config = MakeAgentOptimizeConfig(socket_path_, cache_path_);
  NullMessageHandler handler;
  Worker worker(config, &handler);
  ASSERT_TRUE(worker.Initialize());

  std::string cache_url = "/test.html";
  std::string notification_url = "http://example.com/test.html";
  CacheOriginal(worker.cache(), cache_url, kAgentHtml);

  std::thread worker_thread([&worker]() { worker.Run(); });
  WorkerStopper stopper(worker, worker_thread);
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  CapabilityMask opt_mask;
  CacheNotification notification;
  notification.url = notification_url;
  notification.scheme = "https";
  notification.content_type = ContentType::kHtml;
  notification.capability_mask = opt_mask.Encode();

  // First processing: optimized identity + kContentHash sentinel = SHA256(raw).
  SendNotification(notification);
  std::string variant;
  for (int i = 0; i < 60 * kSanitizerBudgetScale; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    variant = ReadVariant(worker.cache(), cache_url, opt_mask);
    if (worker.stats().html_assembly_complete.load() >= 1 && !variant.empty())
      break;
  }
  ASSERT_FALSE(variant.empty()) << "First HTML variant not written";

  // Stamp a synthetic markdown variant bound to the live content hash.
  std::string sentinel =
      ReadSentinel(worker.cache(), cache_url, SentinelId::kContentHash);
  ASSERT_EQ(sentinel.size(), AlternateMetadata::kHashSize);
  AlternateMetadata md_meta;
  md_meta.full_mask = static_cast<uint32_t>(SentinelId::kAgentMarkdown);
  md_meta.content_type = ContentType::kOther;
  md_meta.origin_content_type = "text/markdown";
  md_meta.flags |= AlternateMetadata::kFlagWorkerProcessed;
  std::memcpy(md_meta.origin_html_hash.data(), sentinel.data(),
              AlternateMetadata::kHashSize);
  std::string markdown = "# Hello World\n\nMarkdown body.\n";
  auto wh = worker.cache()->WriteAgentAlternate(cache_url, "", "https",
                                                markdown.size(), md_meta);
  ASSERT_TRUE(wh.has_value());
  ASSERT_TRUE(
      wh->write_sync(std::as_bytes(std::span(markdown.data(), markdown.size())))
          .has_value());
  ASSERT_TRUE(wh->close_sync().has_value());

  // Simulate nginx's freshness-driven re-fetch with the SAME body (no content
  // change): drop the worker's optimized identity, then re-record the identical
  // RAW body so the slot holds raw origin bytes at sentinel time (nginx
  // overwrites the identity before sending the sentinel; the plain CacheOriginal
  // helper cannot replace an existing worker-processed slot).
  (void)worker.cache()->RemoveAlternate(
      cache_url, "", "https",
      MaskToAlternateId(static_cast<uint8_t>(opt_mask.Encode() & 0xFF)));
  CacheOriginal(worker.cache(), cache_url, kAgentHtml);

  // Send the origin-refresh sentinel.
  CacheNotification refresh = notification;
  refresh.capability_mask = kOriginRefreshedSentinel;
  SendNotification(refresh);
  for (int i = 0; i < 80 * kSanitizerBudgetScale; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    if (worker.stats().origin_refresh_purges.load() >= 1) break;
  }
  ASSERT_GE(worker.stats().origin_refresh_purges.load(), 1u)
      << "sentinel should have triggered a purge";
  std::this_thread::sleep_for(std::chrono::milliseconds(200));

  // D2: the markdown variant survives the unchanged-content refresh.
  EXPECT_TRUE(worker.cache()->AlternateExists(
      cache_url, "", "https",
      static_cast<AlternateId>(SentinelId::kAgentMarkdown)))
      << "markdown variant must be preserved across an unchanged-content "
         "origin refresh (D2): the rendering is still bound to live content";
  EXPECT_GE(worker.stats().agent_markdown_preserved.load(), 1u);
  EXPECT_EQ(worker.stats().agent_markdown_purged_on_change.load(), 0u);
  // Still servable (binding intact, sentinel never rewritten to a new hash).
  CapabilityMask agent_mask;
  EXPECT_TRUE(worker.cache()
                  ->ReadBestAlternateAgent(cache_url, "", "https", agent_mask,
                                           /*agent_request_entitled=*/true)
                  .has_value())
      << "preserved markdown must remain servable";
}

TEST_F(WorkerTest, AgentMarkdownPurgedOnChangedOriginRefresh) {
  WorkerConfig config = MakeAgentOptimizeConfig(socket_path_, cache_path_);
  NullMessageHandler handler;
  Worker worker(config, &handler);
  ASSERT_TRUE(worker.Initialize());

  std::string cache_url = "/test.html";
  std::string notification_url = "http://example.com/test.html";
  CacheOriginal(worker.cache(), cache_url, kAgentHtml);

  std::thread worker_thread([&worker]() { worker.Run(); });
  WorkerStopper stopper(worker, worker_thread);
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  CapabilityMask opt_mask;
  CacheNotification notification;
  notification.url = notification_url;
  notification.scheme = "https";
  notification.content_type = ContentType::kHtml;
  notification.capability_mask = opt_mask.Encode();

  SendNotification(notification);
  std::string variant;
  for (int i = 0; i < 60 * kSanitizerBudgetScale; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    variant = ReadVariant(worker.cache(), cache_url, opt_mask);
    if (worker.stats().html_assembly_complete.load() >= 1 && !variant.empty())
      break;
  }
  ASSERT_FALSE(variant.empty());

  std::string sentinel =
      ReadSentinel(worker.cache(), cache_url, SentinelId::kContentHash);
  ASSERT_EQ(sentinel.size(), AlternateMetadata::kHashSize);
  AlternateMetadata md_meta;
  md_meta.full_mask = static_cast<uint32_t>(SentinelId::kAgentMarkdown);
  md_meta.content_type = ContentType::kOther;
  md_meta.origin_content_type = "text/markdown";
  md_meta.flags |= AlternateMetadata::kFlagWorkerProcessed;
  std::memcpy(md_meta.origin_html_hash.data(), sentinel.data(),
              AlternateMetadata::kHashSize);
  std::string markdown = "# Hello World\n\nMarkdown body.\n";
  auto wh = worker.cache()->WriteAgentAlternate(cache_url, "", "https",
                                                markdown.size(), md_meta);
  ASSERT_TRUE(wh.has_value());
  ASSERT_TRUE(
      wh->write_sync(std::as_bytes(std::span(markdown.data(), markdown.size())))
          .has_value());
  ASSERT_TRUE(wh->close_sync().has_value());

  // Simulate nginx's re-fetch with a DIFFERENT body (genuine content change):
  // drop the worker's optimized identity, then record the fresh RAW body so the
  // slot holds the changed origin bytes at sentinel time.  The rendered markdown
  // is now bound to superseded content.
  std::string fresh_html = std::string(kAgentHtml) + "<p>changed body</p>";
  (void)worker.cache()->RemoveAlternate(
      cache_url, "", "https",
      MaskToAlternateId(static_cast<uint8_t>(opt_mask.Encode() & 0xFF)));
  CacheOriginal(worker.cache(), cache_url, fresh_html);

  CacheNotification refresh = notification;
  refresh.capability_mask = kOriginRefreshedSentinel;
  SendNotification(refresh);
  for (int i = 0; i < 80 * kSanitizerBudgetScale; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    if (worker.stats().origin_refresh_purges.load() >= 1) break;
  }
  ASSERT_GE(worker.stats().origin_refresh_purges.load(), 1u);
  std::this_thread::sleep_for(std::chrono::milliseconds(200));

  // D2 (negative): the markdown bound to the OLD content MUST be purged — never
  // preserved/served stale (D8 fail-closed binding).  Without a real Chrome the
  // inline rebuild cannot re-render, so the variant stays absent, which is the
  // correct "not stale-served" outcome.
  EXPECT_GE(worker.stats().agent_markdown_purged_on_change.load(), 1u);
  EXPECT_EQ(worker.stats().agent_markdown_preserved.load(), 0u);
  EXPECT_FALSE(worker.cache()->AlternateExists(
      cache_url, "", "https",
      static_cast<AlternateId>(SentinelId::kAgentMarkdown)))
      << "markdown bound to superseded origin content must be purged";
}

TEST_F(WorkerTest, AgentMarkdownWorkerProcessedIdentityPurgesConservatively) {
  // Durability (#19/D2) — fail-safe.  When the identity slot holds the
  // worker's OWN optimized output (kFlagWorkerProcessed) rather than the fresh
  // raw origin, FreshOriginHash returns nullopt and the variant is PURGED
  // conservatively — never preserved on a hash we cannot directly verify
  // against the raw origin.  Guards against re-introducing a durable-hash reuse
  // that would falsely preserve here.
  WorkerConfig config = MakeAgentOptimizeConfig(socket_path_, cache_path_);
  NullMessageHandler handler;
  config.unsafe_force_async_css =
      true;  // fixture page is not under test for the gate
  Worker worker(config, &handler);
  ASSERT_TRUE(worker.Initialize());

  std::string cache_url = "/test.html";
  std::string notification_url = "http://example.com/test.html";
  CacheOriginal(worker.cache(), cache_url, kAgentHtml);

  std::thread worker_thread([&worker]() { worker.Run(); });
  WorkerStopper stopper(worker, worker_thread);
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  CapabilityMask opt_mask;
  CacheNotification notification;
  notification.url = notification_url;
  notification.scheme = "https";
  notification.content_type = ContentType::kHtml;
  notification.capability_mask = opt_mask.Encode();

  SendNotification(notification);
  std::string variant;
  for (int i = 0; i < 60 * kSanitizerBudgetScale; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    variant = ReadVariant(worker.cache(), cache_url, opt_mask);
    if (worker.stats().html_assembly_complete.load() >= 1 && !variant.empty())
      break;
  }
  ASSERT_FALSE(variant.empty());

  std::string sentinel =
      ReadSentinel(worker.cache(), cache_url, SentinelId::kContentHash);
  ASSERT_EQ(sentinel.size(), AlternateMetadata::kHashSize);
  AlternateMetadata md_meta;
  md_meta.full_mask = static_cast<uint32_t>(SentinelId::kAgentMarkdown);
  md_meta.content_type = ContentType::kOther;
  md_meta.origin_content_type = "text/markdown";
  md_meta.flags |= AlternateMetadata::kFlagWorkerProcessed;
  std::memcpy(md_meta.origin_html_hash.data(), sentinel.data(),
              AlternateMetadata::kHashSize);
  std::string markdown = "# Hello World\n\nMarkdown body.\n";
  auto wh = worker.cache()->WriteAgentAlternate(cache_url, "", "https",
                                                markdown.size(), md_meta);
  ASSERT_TRUE(wh.has_value());
  ASSERT_TRUE(
      wh->write_sync(std::as_bytes(std::span(markdown.data(), markdown.size())))
          .has_value());
  ASSERT_TRUE(wh->close_sync().has_value());

  // Do NOT re-record a raw identity: the slot still holds the worker's optimized
  // output (kFlagWorkerProcessed).  FreshOriginHash -> nullopt -> conservative
  // purge, even though the origin content is in fact unchanged.
  CacheNotification refresh = notification;
  refresh.capability_mask = kOriginRefreshedSentinel;
  SendNotification(refresh);
  for (int i = 0; i < 80 * kSanitizerBudgetScale; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    if (worker.stats().origin_refresh_purges.load() >= 1) break;
  }
  ASSERT_GE(worker.stats().origin_refresh_purges.load(), 1u);
  std::this_thread::sleep_for(std::chrono::milliseconds(200));

  EXPECT_EQ(worker.stats().agent_markdown_preserved.load(), 0u)
      << "a worker-processed identity is unverifiable raw origin -> never "
         "preserve (fail-safe)";
  EXPECT_GE(worker.stats().agent_markdown_purged_on_change.load(), 1u);
}

// =============================================================================
// Issue C: rebuild the agent markdown (124) variant after an origin-refresh
// purge by propagating agent_request on the inline rebuild notification.
//
// HandleOriginRefreshed purges the whole non-identity set (incl. 124) then
// re-processes the URL at the default mask.  On a profile-present (warm) page,
// markdown is only rebuilt by the warm-template branch, which requires
// notification.agent_request==true.  Pre-fix the rebuild left agent_request at
// its default false, so 124 stayed absent ~70% of the time on landing pages
// and HTML was served to AI crawlers instead of markdown.
//
// Worker-level seam test (no Chrome needed — the harness has no real Chrome, so
// a full render-to-124 path is unsatisfiable in CI).  Asserts the rebuild-intent
// contract directly on the factored-out MakeOriginRefreshRebuild helper.
// =============================================================================

TEST_F(WorkerTest,
       OriginRefreshRebuildPropagatesAgentRequestWhenMarkdownExisted) {
  CacheNotification origin;
  origin.url = "http://example.com/index.html";
  origin.scheme = "https";
  origin.content_type = ContentType::kHtml;
  // The origin-refresh sentinel arrives at a reserved mask; the rebuild must
  // re-process at the DEFAULT capability mask regardless.
  origin.capability_mask = 0xFFFFFFFFu;
  origin.agent_request = false;

  // Positive case: the URL had an agent markdown variant before the purge —
  // the rebuild MUST carry agent_request=true so the warm-template branch
  // re-renders markdown (which AgentMarkdownNeedsBuild requests after the
  // purge dropped 124).
  CacheNotification rebuild_with_md = Worker::MakeOriginRefreshRebuild(
      origin, /*had_markdown=*/true, /*has_intent=*/false);
  EXPECT_TRUE(rebuild_with_md.agent_request)
      << "rebuild must request agent render when 124 pre-existed (else "
         "markdown "
         "is never rebuilt on a warm page and the URL serves HTML to crawlers)";
  EXPECT_EQ(rebuild_with_md.capability_mask, CapabilityMask().Encode())
      << "rebuild must re-process at the default capability mask";

  // Negative case (locks the demand-gate): no pre-existing markdown AND no
  // sticky intent -> the rebuild must NOT request a render, so a page that
  // never had markdown never forces a (wasted, possibly unwanted) agent render.
  CacheNotification rebuild_no_md = Worker::MakeOriginRefreshRebuild(
      origin, /*had_markdown=*/false, /*has_intent=*/false);
  EXPECT_FALSE(rebuild_no_md.agent_request)
      << "rebuild must NOT request agent render when the URL never had "
         "markdown";
  EXPECT_EQ(rebuild_no_md.capability_mask, CapabilityMask().Encode());

  // Durability (#19/D3): the variant was already lost in a prior churn
  // (had_markdown=false) but the URL EVER rendered markdown (has_intent=true) —
  // the rebuild MUST still request a render so the multi-churn demand-gating
  // race cannot strand the URL on HTML permanently.
  CacheNotification rebuild_intent = Worker::MakeOriginRefreshRebuild(
      origin, /*had_markdown=*/false, /*has_intent=*/true);
  EXPECT_TRUE(rebuild_intent.agent_request)
      << "rebuild must request agent render when the URL ever rendered "
         "markdown "
         "(sticky intent heals a variant lost in a prior multi-churn race)";
}

TEST_F(WorkerTest, ContentHashStaleSelfPurgeCountsAsFencedNotFailure) {
  // A content-hash invalidation purge bumps the URL's purge generation in the
  // SAME notification, so that notification's in-flight HTML write is correctly
  // purge-fenced.  This benign fence must count as alternate_writes_fenced —
  // NOT as alternate_write_failures + errors (the live prod lockstep
  // write_failures==content_hash_stale==errors that false-alarms the dashboard).
  WorkerConfig config = MakeAgentOptimizeConfig(socket_path_, cache_path_);
  NullMessageHandler handler;
  config.unsafe_force_async_css =
      true;  // fixture page is not under test for the gate
  Worker worker(config, &handler);
  ASSERT_TRUE(worker.Initialize());

  std::string cache_url = "/fenced.html";
  std::string notification_url = "http://example.com/fenced.html";
  CacheOriginal(worker.cache(), cache_url, kAgentHtml);

  std::thread worker_thread([&worker]() { worker.Run(); });
  WorkerStopper stopper(worker, worker_thread);
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  CapabilityMask opt_mask;
  CacheNotification notification;
  notification.url = notification_url;
  notification.scheme = "https";
  notification.content_type = ContentType::kHtml;
  notification.capability_mask = opt_mask.Encode();

  // First notification binds the kContentHash sentinel over the raw HTML.
  SendNotification(notification);
  std::string variant;
  for (int i = 0; i < 60 * kSanitizerBudgetScale; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    variant = ReadVariant(worker.cache(), cache_url, opt_mask);
    if (worker.stats().html_assembly_complete.load() >= 1 && !variant.empty())
      break;
  }
  ASSERT_FALSE(variant.empty()) << "First HTML variant not written";
  // The variant is readable before the first notification's work item has
  // left the worker.  Wait until it is gone: a second notification for the
  // same URL that arrives while the first is still running is turned away as
  // already in flight, and the content-hash purge below would never run.
  ASSERT_TRUE(pagespeed::test::WaitForNotificationsRetired(worker, 1,
                                                           kWorkerStallBudget));

  uint64_t chs_before = worker.stats().content_hash_stale.load();
  uint64_t wf_before = worker.stats().alternate_write_failures.load();
  uint64_t err_before = worker.stats().errors.load();
  uint64_t fenced_before = worker.stats().alternate_writes_fenced.load();

  // Genuine origin change: overwrite slot 0x08 with DIFFERENT raw bytes and
  // evict the RAM tier so the re-notification reads the fresh raw (exactly the
  // production origin-refresh path).  The content-hash check then fires, purges
  // (bumping the generation), and the in-flight HTML write is purge-fenced.
  std::string new_html =
      "<html><head><style>section { gap: 4px; } "
      "p { color: green; }</style></head>"
      "<body><p>Different Origin Body</p></body></html>";
  ASSERT_NE(new_html, std::string(kAgentHtml));
  CacheOriginal(worker.cache(), cache_url, new_html);
  CapabilityMask identity_mask;
  worker.cache()->EvictAlternateFromRamCache(
      cache_url, "", "https",
      MaskToAlternateId(static_cast<uint8_t>(identity_mask.Encode() & 0xFF)));

  worker.ClearDedupAndCooldown(notification_url, "", "https");
  SendNotification(notification);
  // Both notifications retired: the counters below are final.
  ASSERT_TRUE(pagespeed::test::WaitForNotificationsRetired(worker, 2,
                                                           kWorkerStallBudget));

  EXPECT_GE(worker.stats().content_hash_stale.load(), chs_before + 1)
      << "A genuine origin change must purge";
  EXPECT_GE(worker.stats().alternate_writes_fenced.load(), fenced_before + 1)
      << "The same-notification purge-fenced write must count as FENCED";
  EXPECT_EQ(worker.stats().alternate_write_failures.load(), wf_before)
      << "A benign purge-fence must NOT count as a hard write failure";
  EXPECT_EQ(worker.stats().errors.load(), err_before)
      << "A benign purge-fence must NOT increment errors";
}

// An origin-refreshed sentinel that arrives while another notification for the
// same URL is being processed must not be lost: forced reloads send it once,
// and nothing resends it.  Both frames go out in one write, so the sentinel is
// dispatched before the first item can finish.
TEST_F(WorkerTest, OriginRefreshedSentinelDuringInFlightNotificationIsKept) {
  WorkerConfig config = MakeAgentOptimizeConfig(socket_path_, cache_path_);
  NullMessageHandler handler;
  config.unsafe_force_async_css =
      true;  // fixture page is not under test for the gate
  Worker worker(config, &handler);
  ASSERT_TRUE(worker.Initialize());

  CacheOriginal(worker.cache(), "/held.html", kAgentHtml);

  std::thread worker_thread([&worker]() { worker.Run(); });
  WorkerStopper stopper(worker, worker_thread);

  CapabilityMask opt_mask;
  CacheNotification notification;
  notification.url = "http://example.com/held.html";
  notification.scheme = "https";
  notification.content_type = ContentType::kHtml;
  notification.capability_mask = opt_mask.Encode();
  CacheNotification refresh = notification;
  refresh.capability_mask = kOriginRefreshedSentinel;

  std::vector<char> frames = notification.Serialize();
  const std::vector<char> second = refresh.Serialize();
  frames.insert(frames.end(), second.begin(), second.end());
  SendRawNotificationBytes(frames);

  // The held sentinel is dispatched again as a notification of its own when
  // the first item retires, so wait for the worker to go quiet after both.
  ASSERT_TRUE(pagespeed::test::WaitForNotificationsRetired(worker, 2,
                                                           kWorkerStallBudget));
  ASSERT_EQ(worker.stats().html_processed.load(), 1u);
  ASSERT_EQ(worker.stats().notifications_skipped_inflight.load(), 1u)
      << "the sentinel was expected to meet the in-flight guard";

  const WorkerStats& s = worker.stats();
  const uint64_t handled =
      s.origin_refresh_purges.load() + s.origin_refresh_unchanged.load() +
      s.origin_refresh_deferred.load() + s.origin_refresh_rate_limited.load() +
      s.origin_refresh_rebuild_refused.load();
  EXPECT_GE(handled, 1u)
      << "origin-refreshed sentinel was dropped while a notification for the "
         "same URL was in flight";
}

// =============================================================================
// Agent render and /llms.txt need a sender that vouched for the host
// =============================================================================

namespace {

// Seeds a profile for `html`'s template so the worker takes the warm-template
// branch, where an agent request queues a per-URL render.
void SeedProfileFor(Worker& worker, const std::string& url,
                    const std::string& html) {
  HtmlScanner scanner;
  HtmlScanResult scan = scanner.Scan(url, html);
  ASSERT_TRUE(scan.success);
  OptimizationProfile profile;
  profile.template_hash_hex = "feed";
  profile.analyzed_url = url;
  profile.expires_at = 0;
  worker.TestBrowserManager()->TestStoreProfile(
      TemplateDetector::HashStructure(scan), profile);
}

CacheNotification AgentHtmlNotification(const std::string& url,
                                        bool host_vouched) {
  CacheNotification n;
  n.url = url;
  n.scheme = "https";
  n.content_type = ContentType::kHtml;
  n.capability_mask = CapabilityMask().Encode();
  n.agent_request = true;
  n.host_vouched = host_vouched;
  return n;
}

}  // namespace

TEST_F(WorkerTest, UnvouchedHtmlQueuesNoAgentRenderAndIsStillOptimized) {
  WorkerConfig config = MakeAgentOptimizeConfig(socket_path_, cache_path_);
  NullMessageHandler handler;
  Worker worker(config, &handler);
  ASSERT_TRUE(worker.Initialize());
  const std::string url = "http://example.com/unvouched.html";
  CacheOriginal(worker.cache(), "/unvouched.html", kAgentHtml);
  SeedProfileFor(worker, url, kAgentHtml);

  std::thread worker_thread([&worker]() { worker.Run(); });
  WorkerStopper stopper(worker, worker_thread);
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  SendNotification(AgentHtmlNotification(url, /*host_vouched=*/false));
  ASSERT_TRUE(pagespeed::test::WaitForNotificationsRetired(worker, 1,
                                                           kWorkerStallBudget));
  EXPECT_FALSE(worker.TestBrowserManager()->TestDequeue().has_value())
      << "no agent render was queued for an unvouched host";
  EXPECT_EQ(worker.stats().notifications_unvouched_render_skipped.load(), 1u);
  // The HTML pass still ran: the identity was rewritten, or, when the pass
  // changed nothing (an unconfirmed profile keeps the stylesheet
  // render-blocking and inlines no block), stamped with its source hash.
  auto identity =
      ReadVariantWithMeta(worker.cache(), "/unvouched.html", CapabilityMask());
  ASSERT_TRUE(identity.has_value());
  const bool rewritten =
      (identity->metadata.flags & AlternateMetadata::kFlagWorkerProcessed) != 0;
  bool stamped = false;
  for (std::byte b : identity->metadata.origin_html_hash) {
    stamped = stamped || b != std::byte{0};
  }
  EXPECT_TRUE(rewritten || stamped) << "the page itself was still optimized";
}

TEST_F(WorkerTest, VouchedHtmlStillQueuesTheAgentRender) {
  WorkerConfig config = MakeAgentOptimizeConfig(socket_path_, cache_path_);
  NullMessageHandler handler;
  Worker worker(config, &handler);
  ASSERT_TRUE(worker.Initialize());
  const std::string url = "http://example.com/vouched.html";
  CacheOriginal(worker.cache(), "/vouched.html", kAgentHtml);
  SeedProfileFor(worker, url, kAgentHtml);

  std::thread worker_thread([&worker]() { worker.Run(); });
  WorkerStopper stopper(worker, worker_thread);
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  SendNotification(AgentHtmlNotification(url, /*host_vouched=*/true));
  ASSERT_TRUE(pagespeed::test::WaitForNotificationsRetired(worker, 1,
                                                           kWorkerStallBudget));
  auto item = worker.TestBrowserManager()->TestDequeue();
  ASSERT_TRUE(item.has_value());
  EXPECT_TRUE(item->force_agent_render);
  EXPECT_TRUE(item->host_vouched);
  EXPECT_EQ(worker.stats().notifications_unvouched_render_skipped.load(), 0u);
}

TEST_F(WorkerTest, AnalysisOfAnUnvouchedPageMayNotRunTheAgentRender) {
  AnalysisQueue::Item item;
  item.url = "http://example.com/p.html";
  item.hostname = "example.com";
  item.host_vouched = false;
  EXPECT_FALSE(browser_internal::MayRunAgentRender(item, true, true));
  item.host_vouched = true;
  EXPECT_TRUE(browser_internal::MayRunAgentRender(item, true, true));
  EXPECT_FALSE(browser_internal::MayRunAgentRender(item, false, true));
  EXPECT_FALSE(browser_internal::MayRunAgentRender(item, true, false));
}

TEST_F(WorkerTest, UnvouchedLlmsTxtRequestStartsNoBuild) {
  WorkerConfig config = MakeAgentOptimizeConfig(socket_path_, cache_path_);
  config.browser_analysis.agent_optimize_llms_txt = true;
  WarningRecordingHandler handler;
  Worker worker(config, &handler);
  ASSERT_TRUE(worker.Initialize());
  std::thread worker_thread([&worker]() { worker.Run(); });
  WorkerStopper stopper(worker, worker_thread);
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  // An empty hostname: had the build started, it would stop at its first
  // check, before any fetch, and say so.
  CacheNotification n;
  n.url = "/llms.txt";
  n.scheme = "https";
  n.content_type = ContentType::kOther;
  n.capability_mask = kLlmsTxtSentinel;
  n.host_vouched = false;
  SendNotification(n);
  ASSERT_TRUE(pagespeed::test::WaitForNotificationsRetired(worker, 1,
                                                           kWorkerStallBudget));
  EXPECT_EQ(worker.stats().notifications_unvouched_render_skipped.load(), 1u);
  for (const std::string& w : handler.warnings()) {
    EXPECT_EQ(w.find("llms.txt: empty hostname"), std::string::npos)
        << "the build started: " << w;
  }

  // The same request, vouched, reaches the build as before.
  n.host_vouched = true;
  SendNotification(n);
  ASSERT_TRUE(pagespeed::test::WaitForNotificationsRetired(worker, 2,
                                                           kWorkerStallBudget));
  EXPECT_EQ(worker.stats().notifications_unvouched_render_skipped.load(), 1u);
  bool reached = false;
  for (const std::string& w : handler.warnings()) {
    reached |= w.find("llms.txt: empty hostname") != std::string::npos;
  }
  EXPECT_TRUE(reached) << "the vouched request reaches the build";
}

// The agent render uses the page only when the gate admits the page's own
// URL for a shared copy: an answer that sets a cookie, is private/no-store or
// varies on a request field is refused; an unconfirmed (memo-only) answer is
// not admitted either.
TEST(AgentPageAdmittedTest, AsksTheSharedCopyQuestionAboutThePage) {
  std::map<std::string, int> status;
  std::vector<std::string> asked;
  SubresourceGate gate(
      [](const std::string&, const std::string&) -> std::optional<int> {
        ADD_FAILURE() << "the ordinary HEAD must not answer for the page";
        return 200;
      },
      [] { return std::int64_t{0}; });
  gate.SetSharedCopyHead(
      [&status, &asked](
          const std::string&, const std::string& url,
          const std::vector<PinnedUpstream>&) -> std::optional<int> {
        asked.push_back(url);
        auto it = status.find(url);
        return it == status.end() ? std::optional<int>() : it->second;
      });
  status["https://example.com/open.html"] = 200;
  status["https://example.com/held.html"] = kRefusedStatus;
  AnalysisQueue::Item item;
  item.hostname = "example.com";
  item.scheme = "https";
  item.host_vouched = true;
  const std::vector<PinnedUpstream> none;

  item.url = "/open.html";
  EXPECT_FALSE(browser_internal::AgentPageAdmitted(&gate, item, false, none))
      << "memo only and never asked: not admitted";
  EXPECT_TRUE(browser_internal::AgentPageAdmitted(&gate, item, true, none));
  EXPECT_TRUE(browser_internal::AgentPageAdmitted(&gate, item, false, none))
      << "the warmed answer serves the render and the write";

  item.url = "/held.html";
  EXPECT_FALSE(browser_internal::AgentPageAdmitted(&gate, item, true, none));
  EXPECT_FALSE(browser_internal::AgentPageAdmitted(&gate, item, false, none));

  status["https://example.com/other.html"] = 200;
  item.url = "/other.html";  // never asked: an unvouched item is not asked
  item.host_vouched = false;
  EXPECT_FALSE(browser_internal::AgentPageAdmitted(&gate, item, true, none));
  EXPECT_FALSE(browser_internal::AgentPageAdmitted(nullptr, item, true, none));
  EXPECT_EQ(asked.size(), 2u);
}

// =============================================================================
// Pages stored by the module's HTML store: the Markdown copy is derived from
// the stored bytes, without a browser and without a request leaving the worker.
// =============================================================================

// A page with enough text to clear the fidelity floor.
constexpr char kModuleRichPage[] =
    "<html><head><title>Module page</title></head><body><main>"
    "<h1>Stored by the module</h1>"
    "<p>This paragraph is long enough that the Markdown copy derived from it "
    "clears the fidelity floor with room to spare. It talks about caching, "
    "about agents that ask for text instead of markup, and about the worker "
    "that turns one into the other without opening a browser.</p>"
    "<p>A second paragraph adds more words, so that no reasonable floor could "
    "call this page a shell: it has a heading, two paragraphs and a list.</p>"
    "<ul><li>first item</li><li>second item</li></ul>"
    "</main></body></html>";

// A script-rendered shell: nothing to read without running it.
constexpr char kModuleShellPage[] =
    "<html><head><title>App</title></head><body><div id=\"app\"></div>"
    "<script src=\"/app.js\"></script></body></html>";

std::array<std::byte, 32> Sha256Of(std::string_view s) {
  return cyclone::crypto::SHA256::hash(std::span<const std::byte>(
      reinterpret_cast<const std::byte*>(s.data()), s.size()));
}

// What the module's HTML store writes for a test page.
struct ModuleStoredPage {
  std::string content_type = "text/html; charset=utf-8";
  ContentType type = ContentType::kHtml;
  uint8_t flags = AlternateMetadata::kFlagHeadersClassified;
  uint16_t cc_flags = 0;
  std::optional<std::array<std::byte, 32>> stamp;  // default: SHA-256(body)
};

struct ModuleStoredEntry {
  std::string content;
  AlternateMetadata metadata;
};

class ModuleHtmlTest : public WorkerTest {
 protected:
  static constexpr const char* kPath = "/page.html";
  static constexpr const char* kHost = "example.com";
  const std::string site_ = std::string(kSiteIdChars, 'c');

  using StoredPage = ModuleStoredPage;
  using Entry = ModuleStoredEntry;

  void TearDown() override {
    stopper_.reset();
    worker_.reset();
    WorkerTest::TearDown();
  }

  WorkerConfig Config(HtmlMode mode = HtmlMode::kMarkdownStatic) {
    WorkerConfig config = MakeAgentOptimizeConfig(socket_path_, cache_path_);
    config.html_mode = mode;
    return config;
  }

  // `before_run` runs after Initialize and before the event loop starts.
  void Start(const WorkerConfig& config,
             const std::function<void(Worker&)>& before_run = nullptr) {
    worker_ = std::make_unique<Worker>(config, &handler_);
    ASSERT_TRUE(worker_->Initialize());
    if (before_run) before_run(*worker_);
    thread_ = std::thread([this]() { worker_->Run(); });
    stopper_ = std::make_unique<WorkerStopper>(*worker_, thread_);
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
  }

  PageSpeedCache* cache() { return worker_->cache(); }
  const WorkerStats& stats() { return worker_->stats(); }

  // What the module's HTML store writes: the page at the durable-original id,
  // in its site, classified, stamped with its content hash.
  void Store(std::string_view body, const StoredPage& page = {}) {
    AlternateMetadata meta;
    meta.full_mask = static_cast<uint32_t>(SentinelId::kOriginalContent);
    meta.content_type = page.type;
    meta.origin_content_type = page.content_type;
    meta.flags = page.flags;
    meta.origin_cc_flags = static_cast<uint16_t>(
        page.cc_flags | AlternateMetadata::kCCOriginHeaderPresent |
        AlternateMetadata::kCCOriginPublic);
    meta.origin_max_age = 3600;
    meta.cache_inserted_at = static_cast<uint32_t>(std::time(nullptr));
    meta.origin_html_hash = page.stamp.value_or(Sha256Of(body));
    auto wh = cache()->WriteOriginalAlternate(kPath, kHost, "https",
                                              body.size(), meta, site_);
    ASSERT_TRUE(wh.has_value());
    ASSERT_TRUE(wh->write_sync(std::as_bytes(std::span(body))).has_value());
    ASSERT_TRUE(wh->close_sync().has_value());
  }

  [[nodiscard]] CacheNotification Frame(bool agent_request) const {
    CacheNotification n;
    n.url = std::string("http://example.com") + kPath;
    n.hostname = kHost;
    n.scheme = "https";
    n.content_type = ContentType::kHtml;
    n.capability_mask = CapabilityMask().Encode();
    n.site = site_;
    n.from_module = true;
    n.host_vouched = true;
    n.html_stored = true;
    n.agent_request = agent_request;
    return n;
  }

  // Sends and waits for every frame sent so far to retire.
  void Send(const CacheNotification& n) {
    SendNotification(n);
    ++sent_;
    ASSERT_TRUE(pagespeed::test::WaitForNotificationsRetired(
        *worker_, sent_, kWorkerStallBudget));
  }

  std::optional<Entry> Read(SentinelId id) {
    const auto alt = static_cast<AlternateId>(id);
    cache()->EvictAlternateFromRamCache(kPath, kHost, "https", alt, site_);
    auto r = cache()->ReadAlternate(kPath, kHost, "https", alt, site_);
    if (!r.has_value()) return std::nullopt;
    auto c = r->content();
    return Entry{std::string(reinterpret_cast<const char*>(c.data()), c.size()),
                 r->metadata};
  }

  std::optional<HtmlWithheldMarker> Marker() {
    auto e = Read(SentinelId::kAgentMarkdownWithheld);
    if (!e.has_value()) return std::nullopt;
    return DecodeHtmlWithheldMarker(std::span<const uint8_t>(
        reinterpret_cast<const uint8_t*>(e->content.data()),
        e->content.size()));
  }

  void WriteMarker(const HtmlWithheldMarker& marker) {
    const std::string payload = EncodeHtmlWithheldMarker(marker);
    auto wh = cache()->WriteSentinel(kPath, kHost, "https",
                                     SentinelId::kAgentMarkdownWithheld,
                                     payload.size(), site_);
    ASSERT_TRUE(wh.has_value());
    ASSERT_TRUE(wh->write_sync(std::as_bytes(std::span(payload))).has_value());
    ASSERT_TRUE(wh->close_sync().has_value());
  }

  // The copy is present, bound to `body`, stamped with its own content hash
  // and actually Markdown of the page.
  void ExpectCopyOf(std::string_view body) {
    auto copy = Read(SentinelId::kAgentMarkdown);
    ASSERT_TRUE(copy.has_value()) << "no Markdown copy was written";
    EXPECT_EQ(copy->metadata.origin_html_hash, Sha256Of(body));
    EXPECT_EQ(copy->metadata.render_source_hash, Sha256Of(copy->content))
        << "the copy's content hash, which its entity tag is made from";
    EXPECT_NE(copy->content.find("Stored by the module"), std::string::npos);
    EXPECT_FALSE(MarkdownBelowFidelityFloor(copy->content));
  }

  // A permanent refusal: no copy, and a marker for this content, the current
  // deriver and `reason`, which the library's serve-side test applies.
  void ExpectWithheld(std::string_view stamp_of, HtmlWithheldReason reason) {
    EXPECT_FALSE(Read(SentinelId::kAgentMarkdown).has_value());
    auto marker = Marker();
    ASSERT_TRUE(marker.has_value()) << "no withheld marker";
    EXPECT_EQ(marker->reason, reason);
    EXPECT_EQ(marker->deriver_version, kHtmlMarkdownDeriverVersion);
    const auto h = Sha256Of(stamp_of);
    EXPECT_EQ(std::memcmp(marker->parent_hash.data(), h.data(), h.size()), 0);
    auto raw = Read(SentinelId::kAgentMarkdownWithheld);
    ASSERT_TRUE(raw.has_value());
    EXPECT_TRUE(HtmlWithheldMarkerApplies(
        std::span<const uint8_t>(
            reinterpret_cast<const uint8_t*>(raw->content.data()),
            raw->content.size()),
        std::span<const uint8_t>(reinterpret_cast<const uint8_t*>(h.data()),
                                 h.size())))
        << "the serve side would not read this marker as withheld";
  }

  NullMessageHandler handler_;
  std::unique_ptr<Worker> worker_;
  std::thread thread_;
  std::unique_ptr<WorkerStopper> stopper_;
  uint64_t sent_ = 0;
};

// A store frame records the content hash and builds nothing; the agent frame
// that follows (the lost-copy notification for a present page) builds the
// copy.  No HTML variant, no analysis.
TEST_F(ModuleHtmlTest, StoreFrameRecordsAndAgentFrameDerives) {
  ASSERT_NO_FATAL_FAILURE(Start(Config()));
  ASSERT_NO_FATAL_FAILURE(Store(kModuleRichPage));

  ASSERT_NO_FATAL_FAILURE(Send(Frame(/*agent_request=*/false)));
  auto recorded = Read(SentinelId::kContentHash);
  ASSERT_TRUE(recorded.has_value());
  const auto h = Sha256Of(kModuleRichPage);
  EXPECT_EQ(recorded->content,
            std::string(reinterpret_cast<const char*>(h.data()), h.size()));
  EXPECT_FALSE(Read(SentinelId::kAgentMarkdown).has_value())
      << "a copy nobody asked for was built";

  ASSERT_NO_FATAL_FAILURE(Send(Frame(/*agent_request=*/true)));
  ExpectCopyOf(kModuleRichPage);
  EXPECT_EQ(stats().module_markdown_derived.load(), 1u);
  EXPECT_EQ(stats().notifications_rejected_module_render.load(), 0u);
  EXPECT_EQ(stats().variants_written.load(), 0u) << "no HTML variant";
  EXPECT_EQ(stats().html_processed.load(), 0u);
  EXPECT_FALSE(worker_->TestBrowserManager()->TestDequeue().has_value());
  // The stored page is untouched.
  auto page = Read(SentinelId::kOriginalContent);
  ASSERT_TRUE(page.has_value());
  EXPECT_EQ(page->content, kModuleRichPage);
}

// The agent request that arrives while the store frame is still being
// processed is held, not dropped, and a later store frame that replaces it in
// the hold keeps its agent request: the copy is built once the first pass
// ends.  The first pass is held at a known point so both frames are certain to
// meet the in-flight guard.
TEST_F(ModuleHtmlTest, AgentFrameBehindAnInFlightStoreFrameIsHeldNotDropped) {
  std::atomic<bool> entered{false};
  std::atomic<bool> release{false};
  std::atomic<bool> first{true};
  ASSERT_NO_FATAL_FAILURE(Start(Config(), [&](Worker& worker) {
    worker.test_hook_module_html_after_copy_ = [&]() {
      if (!first.exchange(false)) return;
      entered = true;
      for (int i = 0; i < 2000 && !release; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
      }
    };
  }));
  ASSERT_NO_FATAL_FAILURE(Store(kModuleRichPage));
  SendNotification(Frame(/*agent_request=*/false));
  for (int i = 0; i < 2000 && !entered; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  ASSERT_TRUE(entered) << "the first pass never started";
  SendNotification(Frame(/*agent_request=*/true));
  SendNotification(Frame(/*agent_request=*/false));
  for (int i = 0; i < 2000 && stats().notifications_skipped_inflight.load() < 2;
       ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  EXPECT_EQ(stats().notifications_skipped_inflight.load(), 2u);
  EXPECT_FALSE(Read(SentinelId::kAgentMarkdown).has_value())
      << "a copy appeared before the held frame ran";
  release = true;
  // Three frames sent; the held one is dispatched again when the first ends.
  sent_ = 4;
  ASSERT_TRUE(pagespeed::test::WaitForNotificationsRetired(*worker_, sent_,
                                                           kWorkerStallBudget));
  ExpectCopyOf(kModuleRichPage);
  EXPECT_EQ(stats().module_markdown_derived.load(), 1u);
  EXPECT_FALSE(
      worker_->GetCooldownForUrl(kPath, kHost, "https", std::string_view(site_))
          .has_value())
      << "the page was left under a cooldown";
}

// A page stored again between the checks and the writes is checked again:
// the same bytes, now with no-transform, get neither a copy nor a marker.
TEST_F(ModuleHtmlTest, AnEntryThatTurnsNoTransformBeforeTheWriteGetsNothing) {
  for (const char* body : {kModuleRichPage, kModuleShellPage}) {
    SCOPED_TRACE(body == kModuleRichPage ? "copy" : "marker");
    TearDown();
    SetUp();
    sent_ = 0;
    ASSERT_NO_FATAL_FAILURE(Start(Config(), [this, body](Worker& worker) {
      worker.test_hook_module_html_after_copy_ = [this, body]() {
        StoredPage refusing;
        refusing.cc_flags = AlternateMetadata::kCCOriginNoTransform;
        Store(body, refusing);
      };
    }));
    ASSERT_NO_FATAL_FAILURE(Store(body));
    ASSERT_NO_FATAL_FAILURE(Send(Frame(/*agent_request=*/true)));
    EXPECT_FALSE(Read(SentinelId::kAgentMarkdown).has_value());
    EXPECT_FALSE(Read(SentinelId::kAgentMarkdownWithheld).has_value());
    EXPECT_EQ(stats().module_markdown_derived.load(), 0u);
    EXPECT_EQ(stats().module_markdown_withheld.load(), 0u);
  }
}

// Every condition of the stored-page shape and of this worker's mode is a
// refusal at the event loop, counted.
TEST_F(ModuleHtmlTest, RefusedUnlessStoredAndTheWorkerDerivesCopies) {
  struct Case {
    const char* name;
    HtmlMode mode;
    bool agent_optimize;
    bool html_stored;
    uint32_t mask;
  };
  const uint32_t plain = CapabilityMask().Encode();
  const Case cases[] = {
      {"full", HtmlMode::kFull, true, true, plain},
      {"off", HtmlMode::kOff, true, true, plain},
      {"agent_optimize off", HtmlMode::kMarkdownStatic, false, true, plain},
      {"not stored by the module", HtmlMode::kMarkdownStatic, true, false,
       plain},
      {"warmup mask", HtmlMode::kMarkdownStatic, true, true, kWarmupSentinel},
      {"origin-refresh mask", HtmlMode::kMarkdownStatic, true, true,
       kOriginRefreshedSentinel},
  };
  for (const Case& c : cases) {
    SCOPED_TRACE(c.name);
    TearDown();
    SetUp();
    sent_ = 0;
    WorkerConfig config = Config(c.mode);
    config.browser_analysis.agent_optimize = c.agent_optimize;
    ASSERT_NO_FATAL_FAILURE(Start(config));
    ASSERT_NO_FATAL_FAILURE(Store(kModuleRichPage));
    CacheNotification n = Frame(/*agent_request=*/true);
    n.capability_mask = c.mask;
    if (!c.html_stored) {
      // Without the stored-page flag the module may not vouch or ask for a
      // copy either (the wire refuses that shape).
      n.html_stored = false;
      n.host_vouched = false;
      n.agent_request = false;
    }
    ASSERT_NO_FATAL_FAILURE(Send(n));
    EXPECT_EQ(stats().notifications_rejected_module_render.load(), 1u);
    EXPECT_FALSE(Read(SentinelId::kContentHash).has_value());
    EXPECT_FALSE(Read(SentinelId::kAgentMarkdown).has_value());
    EXPECT_FALSE(Read(SentinelId::kAgentMarkdownWithheld).has_value());
  }
}

// What the stored entry says it is decides before a byte is copied: a
// missing page, one that is not HTML, one the HTML store did not write, one
// that forbids transformation and one without a content hash are refused,
// counted, and leave nothing behind.
TEST_F(ModuleHtmlTest, StoredEntryRefusedBeforeAnyCopy) {
  struct Case {
    const char* name;
    bool store;
    StoredPage page;
  };
  StoredPage not_html;
  not_html.content_type = "text/plain";
  StoredPage not_html_type;
  not_html_type.type = ContentType::kOther;
  StoredPage unflagged;
  unflagged.flags = 0;
  StoredPage no_transform;
  no_transform.cc_flags = AlternateMetadata::kCCOriginNoTransform;
  StoredPage unstamped;
  unstamped.stamp = std::array<std::byte, 32>{};
  const Case cases[] = {
      {"missing", false, {}},
      {"text/plain", true, not_html},
      {"not an HTML entry", true, not_html_type},
      {"not classified", true, unflagged},
      {"no-transform", true, no_transform},
      {"no content hash", true, unstamped},
  };
  for (const Case& c : cases) {
    SCOPED_TRACE(c.name);
    TearDown();
    SetUp();
    sent_ = 0;
    ASSERT_NO_FATAL_FAILURE(Start(Config()));
    if (c.store) ASSERT_NO_FATAL_FAILURE(Store(kModuleRichPage, c.page));
    ASSERT_NO_FATAL_FAILURE(Send(Frame(/*agent_request=*/true)));
    EXPECT_EQ(stats().notifications_rejected_module_render.load(), 1u);
    // The content-hash record is written after the copy and its checks, so
    // its absence shows the pass stopped before copying.
    EXPECT_FALSE(Read(SentinelId::kContentHash).has_value());
    EXPECT_FALSE(Read(SentinelId::kAgentMarkdown).has_value());
    EXPECT_FALSE(Read(SentinelId::kAgentMarkdownWithheld).has_value());
    EXPECT_EQ(stats().module_markdown_derived.load(), 0u);
  }
}

// New content drops the copies derived from the old content, keeps the
// module's stored page and header block, and derives again.
TEST_F(ModuleHtmlTest, ChangedContentKeepsTheStoredPageAndRederives) {
  ASSERT_NO_FATAL_FAILURE(Start(Config()));
  ASSERT_NO_FATAL_FAILURE(Store(kModuleRichPage));
  {
    const std::string block = std::string(1, '\x01') + "X-Kept: yes\r\n";
    auto wh = cache()->WriteSentinel(kPath, kHost, "https",
                                     SentinelId::kHeadersSidecar, block.size(),
                                     site_);
    ASSERT_TRUE(wh.has_value());
    ASSERT_TRUE(wh->write_sync(std::as_bytes(std::span(block))).has_value());
    ASSERT_TRUE(wh->close_sync().has_value());
  }
  ASSERT_NO_FATAL_FAILURE(Send(Frame(/*agent_request=*/true)));
  ExpectCopyOf(kModuleRichPage);

  std::string changed = kModuleRichPage;
  changed.replace(changed.find("first item"), 10, "other item");
  ASSERT_NO_FATAL_FAILURE(Store(changed));
  ASSERT_NO_FATAL_FAILURE(Send(Frame(/*agent_request=*/true)));
  ExpectCopyOf(changed);
  auto page = Read(SentinelId::kOriginalContent);
  ASSERT_TRUE(page.has_value()) << "the stored page was purged";
  EXPECT_EQ(page->content, changed);
  EXPECT_TRUE(Read(SentinelId::kHeadersSidecar).has_value())
      << "the header block was purged";
  EXPECT_EQ(stats().module_markdown_derived.load(), 2u);
}

// A copy left behind for older content (one written after the page changed,
// for example) is never bound to the new content: the next frame for the new
// content replaces it, even without an agent request.
TEST_F(ModuleHtmlTest, ACopyForOlderContentIsRebuiltNotServed) {
  ASSERT_NO_FATAL_FAILURE(Start(Config()));
  std::string changed = kModuleRichPage;
  changed.replace(changed.find("first item"), 10, "other item");
  ASSERT_NO_FATAL_FAILURE(Store(changed));
  {
    AlternateMetadata meta;
    meta.full_mask = static_cast<uint32_t>(SentinelId::kAgentMarkdown);
    meta.content_type = ContentType::kOther;
    meta.origin_content_type = "text/markdown";
    meta.flags = AlternateMetadata::kFlagWorkerProcessed;
    meta.origin_html_hash = Sha256Of(kModuleRichPage);
    const std::string old_copy = "# the old page\n";
    auto wh = cache()->WriteAgentAlternate(kPath, kHost, "https",
                                           old_copy.size(), meta, site_);
    ASSERT_TRUE(wh.has_value());
    ASSERT_TRUE(wh->write_sync(std::as_bytes(std::span(old_copy))).has_value());
    ASSERT_TRUE(wh->close_sync().has_value());
  }
  ASSERT_NO_FATAL_FAILURE(Send(Frame(/*agent_request=*/false)));
  ExpectCopyOf(changed);
}

// A purge (or a refresh) that leaves the same bytes behind is processed
// again: nothing about "same content" skips a page whose copy is gone.
TEST_F(ModuleHtmlTest, SameBytesAfterAPurgeStillDerive) {
  ASSERT_NO_FATAL_FAILURE(Start(Config()));
  ASSERT_NO_FATAL_FAILURE(Store(kModuleRichPage));
  ASSERT_NO_FATAL_FAILURE(Send(Frame(/*agent_request=*/true)));
  ExpectCopyOf(kModuleRichPage);

  // The module's purge removes the whole key in its site; it then stores the
  // same page again.
  ASSERT_TRUE(cache()->Remove(kPath, kHost, "https", site_).has_value());
  EXPECT_FALSE(Read(SentinelId::kAgentMarkdown).has_value());
  ASSERT_NO_FATAL_FAILURE(Store(kModuleRichPage));
  ASSERT_NO_FATAL_FAILURE(Send(Frame(/*agent_request=*/true)));
  ExpectCopyOf(kModuleRichPage);
  EXPECT_EQ(stats().module_markdown_derived.load(), 2u);

  // A refresh with the same bytes while the copy is in place keeps it.
  ASSERT_NO_FATAL_FAILURE(Store(kModuleRichPage));
  ASSERT_NO_FATAL_FAILURE(Send(Frame(/*agent_request=*/true)));
  ExpectCopyOf(kModuleRichPage);
  EXPECT_EQ(stats().module_markdown_derived.load(), 2u)
      << "a bound copy was rebuilt";
}

// Each permanent refusal writes no copy but a marker with its reason, and a
// second agent frame for the same content does no derivation.
TEST_F(ModuleHtmlTest, PermanentRefusalsWriteAMarkerAndAreNotRepeated) {
  struct Case {
    const char* name;
    std::string body;
    StoredPage page;
    size_t max_html_size;
    HtmlWithheldReason reason;
  };
  StoredPage mismatched;
  mismatched.stamp = Sha256Of("some other page");
  const std::string gzip_body =
      std::string("\x1f\x8b\x08\x00", 4) + std::string(64, 'x');
  const Case cases[] = {
      {"below floor",
       kModuleShellPage,
       {},
       1 << 20,
       HtmlWithheldReason::kBelowFloor},
      {"too large", kModuleRichPage, {}, 64, HtmlWithheldReason::kTooLarge},
      {"encoded", gzip_body, {}, 1 << 20, HtmlWithheldReason::kEncoded},
      {"hash mismatch", kModuleRichPage, mismatched, 1 << 20,
       HtmlWithheldReason::kHashMismatch},
  };
  for (const Case& c : cases) {
    SCOPED_TRACE(c.name);
    TearDown();
    SetUp();
    sent_ = 0;
    WorkerConfig config = Config();
    config.max_html_size = c.max_html_size;
    ASSERT_NO_FATAL_FAILURE(Start(config));
    ASSERT_NO_FATAL_FAILURE(Store(c.body, c.page));
    ASSERT_NO_FATAL_FAILURE(Send(Frame(/*agent_request=*/true)));
    const std::string stamp_of = c.reason == HtmlWithheldReason::kHashMismatch
                                     ? "some other page"
                                     : c.body;
    ExpectWithheld(stamp_of, c.reason);
    if (c.reason == HtmlWithheldReason::kTooLarge) {
      EXPECT_EQ(Marker()->size_cap, c.max_html_size);
    }
    EXPECT_EQ(stats().module_markdown_withheld.load(), 1u);
    const uint64_t floor_before = stats().agent_markdown_below_floor.load();

    ASSERT_NO_FATAL_FAILURE(Send(Frame(/*agent_request=*/true)));
    EXPECT_EQ(stats().module_markdown_withheld_skips.load(), 1u);
    EXPECT_EQ(stats().module_markdown_withheld.load(), 1u);
    EXPECT_EQ(stats().agent_markdown_below_floor.load(), floor_before)
        << "the second frame ran the extractor";
    EXPECT_EQ(stats().module_markdown_derived.load(), 0u);

    // A marker never reads as a page: neither a selection for an agent
    // request nor one for a browser returns it.
    CapabilityMask agent;
    auto best =
        cache()->ReadBestAlternateAgent(kPath, kHost, "https", agent,
                                        /*agent_request_entitled=*/true, site_);
    EXPECT_FALSE(best.has_value()) << "a selection read returned an entry";
    auto plain = cache()->ReadBestAlternate(kPath, kHost, "https",
                                            CapabilityMask(), false, site_);
    EXPECT_FALSE(plain.has_value()) << "a selection read returned an entry";
  }
}

// A marker from an older deriver is not obeyed: the page is derived and the
// marker goes, so the serve side stops withholding the copy.
TEST_F(ModuleHtmlTest, AMarkerFromAnOlderDeriverIsIgnored) {
  ASSERT_NO_FATAL_FAILURE(Start(Config()));
  ASSERT_NO_FATAL_FAILURE(Store(kModuleRichPage));
  ASSERT_NO_FATAL_FAILURE(Send(Frame(/*agent_request=*/false)));
  HtmlWithheldMarker old;
  old.reason = HtmlWithheldReason::kBelowFloor;
  old.deriver_version = kHtmlMarkdownDeriverVersion - 1;
  const auto h = Sha256Of(kModuleRichPage);
  std::memcpy(old.parent_hash.data(), h.data(), h.size());
  ASSERT_NO_FATAL_FAILURE(WriteMarker(old));

  ASSERT_NO_FATAL_FAILURE(Send(Frame(/*agent_request=*/true)));
  ExpectCopyOf(kModuleRichPage);
  EXPECT_FALSE(Read(SentinelId::kAgentMarkdownWithheld).has_value());
}

// A size refusal is about the cap it was made under: raise the cap and the
// page derives.
TEST_F(ModuleHtmlTest, ATooLargeMarkerIsIgnoredAfterTheCapIsRaised) {
  WorkerConfig config = Config();
  config.max_html_size = 64;
  ASSERT_NO_FATAL_FAILURE(Start(config));
  ASSERT_NO_FATAL_FAILURE(Store(kModuleRichPage));
  ASSERT_NO_FATAL_FAILURE(Send(Frame(/*agent_request=*/true)));
  ExpectWithheld(kModuleRichPage, HtmlWithheldReason::kTooLarge);

  config.max_html_size = 1 << 20;
  worker_->UpdateConfig(std::make_shared<const WorkerConfig>(config));
  ASSERT_NO_FATAL_FAILURE(Send(Frame(/*agent_request=*/true)));
  ExpectCopyOf(kModuleRichPage);
  EXPECT_FALSE(Read(SentinelId::kAgentMarkdownWithheld).has_value());
}

// A refusal for bytes that did not match their hash is undone when the
// module stores the page again: the store removes the marker, and the next
// agent request derives the copy.
TEST_F(ModuleHtmlTest, AHashMismatchMarkerIsClearedByTheNextStore) {
  ASSERT_NO_FATAL_FAILURE(Start(Config()));
  StoredPage mismatched;
  mismatched.stamp = Sha256Of("some other page");
  ASSERT_NO_FATAL_FAILURE(Store(kModuleRichPage, mismatched));
  ASSERT_NO_FATAL_FAILURE(Send(Frame(/*agent_request=*/true)));
  ExpectWithheld("some other page", HtmlWithheldReason::kHashMismatch);

  ASSERT_NO_FATAL_FAILURE(Store(kModuleRichPage));
  EXPECT_FALSE(Read(SentinelId::kAgentMarkdownWithheld).has_value())
      << "the store did not clear the marker";
  ASSERT_NO_FATAL_FAILURE(Send(Frame(/*agent_request=*/true)));
  ExpectCopyOf(kModuleRichPage);
}

// markdown-rendered derives module pages statically too: the module's
// frames may never start a browser.
TEST_F(ModuleHtmlTest, RenderedModeDerivesModulePagesStatically) {
  ASSERT_NO_FATAL_FAILURE(Start(Config(HtmlMode::kMarkdownRendered)));
  ASSERT_NO_FATAL_FAILURE(Store(kModuleRichPage));
  ASSERT_NO_FATAL_FAILURE(Send(Frame(/*agent_request=*/true)));
  ExpectCopyOf(kModuleRichPage);
  EXPECT_FALSE(worker_->TestBrowserManager()->TestDequeue().has_value());
}

// The network stub: with no browser, no sub-resource check allowed and no
// test override in place, a module page still gets its copy, and nothing
// that fetches, analyses or renders ran.
TEST_F(ModuleHtmlTest, DerivesWithoutAnyRequestLeavingTheWorker) {
  // The fixture's blanket "admitted" override would hide a gate call.
  SubresourceGate::SetVerdictOverrideForTest(nullptr);
  std::atomic<int> heads{0};
  ASSERT_NO_FATAL_FAILURE(Start(Config(), [&heads](Worker& worker) {
    worker.TestSubresourceGate()->SetHead(
        [&heads](const std::string&,
                 const std::string& url) -> std::optional<int> {
          ++heads;
          ADD_FAILURE() << "HEAD sent for " << url;
          return std::nullopt;
        });
    worker.TestSubresourceGate()->SetSharedCopyHead(
        [&heads](const std::string&, const std::string& url,
                 const std::vector<PinnedUpstream>&) -> std::optional<int> {
          ++heads;
          ADD_FAILURE() << "shared-copy HEAD sent for " << url;
          return std::nullopt;
        });
  }));
  ASSERT_NO_FATAL_FAILURE(Store(kModuleRichPage));
  ASSERT_NO_FATAL_FAILURE(Send(Frame(/*agent_request=*/false)));
  ASSERT_NO_FATAL_FAILURE(Send(Frame(/*agent_request=*/true)));
  ExpectCopyOf(kModuleRichPage);

  EXPECT_EQ(heads.load(), 0);
  const BrowserStats& bs = worker_->TestBrowserManager()->stats();
  EXPECT_EQ(bs.queue_enqueued.load(), 0u) << "an analysis was queued";
  EXPECT_EQ(bs.queue_processed.load(), 0u) << "an analysis ran";
  EXPECT_EQ(bs.profiles_generated.load(), 0u);
  EXPECT_EQ(bs.chrome_restarts.load(), 0u) << "a browser was started";
  EXPECT_EQ(bs.script_fetches_served.load(), 0u);
  EXPECT_EQ(bs.script_fetches_blocked.load(), 0u);
  EXPECT_FALSE(worker_->TestBrowserManager()->TestDequeue().has_value());
  EXPECT_EQ(stats().notifications_unvouched_render_skipped.load(), 0u);
  // Nothing on this path builds the /llms.txt index, the only user of the
  // agent fetcher.
  EXPECT_FALSE(Read(SentinelId::kLlmsTxt).has_value());
}

// The rendered path keeps no copy below the fidelity floor, and remembers it
// for that content so the page is not rendered again for every request.
TEST_F(WorkerTest, RenderedCopyBelowTheFloorIsNotKept) {
  WorkerConfig config = MakeAgentOptimizeConfig(socket_path_, cache_path_);
  NullMessageHandler handler;
  Worker worker(config, &handler);
  ASSERT_TRUE(worker.Initialize());
  BrowserAnalysisManager* manager = worker.TestBrowserManager();
  ASSERT_NE(manager, nullptr);

  AnalysisQueue::Item item;
  item.url = "/rendered.html";
  item.hostname = "example.com";
  item.scheme = "https";
  item.host_vouched = true;
  item.has_origin_html_hash = true;
  item.origin_html_hash = Sha256Of("the rendered page");
  {
    auto wh = worker.cache()->WriteSentinel(
        item.url, item.hostname, item.scheme, SentinelId::kContentHash, 32);
    ASSERT_TRUE(wh.has_value());
    ASSERT_TRUE(wh->write_sync(std::as_bytes(std::span(item.origin_html_hash)))
                    .has_value());
    ASSERT_TRUE(wh->close_sync().has_value());
  }
  const auto copy = [&]() {
    const auto id = static_cast<AlternateId>(SentinelId::kAgentMarkdown);
    worker.cache()->EvictAlternateFromRamCache(item.url, item.hostname,
                                               item.scheme, id);
    return worker.cache()
        ->ReadAlternate(item.url, item.hostname, item.scheme, id)
        .has_value();
  };

  PageAnalysisResult shell;
  shell.agent_markdown = "# App\n\nLoading...\n";
  manager->TestAgentRenderDone(item, shell);
  EXPECT_FALSE(copy()) << "a copy below the floor was kept";
  EXPECT_EQ(manager->stats().agent_markdown_below_floor.load(), 1u);
  EXPECT_TRUE(manager->AgentCopyBelowFloor(item.url, item.hostname, item.scheme,
                                           item.site, item.origin_html_hash));
  EXPECT_FALSE(manager->AgentCopyBelowFloor(item.url, item.hostname,
                                            item.scheme, item.site,
                                            Sha256Of("changed content")))
      << "a changed page must be rendered again";

  // Positive control: the same render with a real body is kept.
  PageAnalysisResult page;
  page.agent_markdown = "# Page\n\n" + std::string(300, 'w') + "\n";
  manager->TestAgentRenderDone(item, page);
  EXPECT_TRUE(copy());
}

// In full mode an agent request for a warm template queues a forced render
// whenever the page has no copy.  After a render of the same content fell
// below the floor it queues none, so a shell page costs one render per retry
// window, not one per request.
TEST_F(WorkerTest, BelowFloorRenderIsNotRepeatedInFullMode) {
  WorkerConfig config = MakeAgentOptimizeConfig(socket_path_, cache_path_);
  ASSERT_EQ(config.html_mode, HtmlMode::kFull);
  NullMessageHandler handler;
  Worker worker(config, &handler);
  ASSERT_TRUE(worker.Initialize());
  const std::string url = "http://example.com/shell.html";
  CacheOriginal(worker.cache(), "/shell.html", kAgentHtml);
  SeedProfileFor(worker, url, kAgentHtml);
  std::thread worker_thread([&worker]() { worker.Run(); });
  WorkerStopper stopper(worker, worker_thread);
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  SendNotification(AgentHtmlNotification(url, /*host_vouched=*/true));
  ASSERT_TRUE(pagespeed::test::WaitForNotificationsRetired(worker, 1,
                                                           kWorkerStallBudget));
  auto queued = worker.TestBrowserManager()->TestDequeue();
  ASSERT_TRUE(queued.has_value());
  ASSERT_TRUE(queued->force_agent_render);
  ASSERT_TRUE(queued->has_origin_html_hash);

  // Without a record, the next request queues the render again.
  worker.ClearDedupAndCooldown(queued->url, queued->hostname, queued->scheme);
  SendNotification(AgentHtmlNotification(url, /*host_vouched=*/true));
  ASSERT_TRUE(pagespeed::test::WaitForNotificationsRetired(worker, 2,
                                                           kWorkerStallBudget));
  ASSERT_TRUE(worker.TestBrowserManager()->TestDequeue().has_value());

  // The render fell below the floor: the next request queues nothing.
  worker.TestBrowserManager()->NoteAgentCopyBelowFloor(
      queued->url, queued->hostname, queued->scheme, queued->site,
      queued->origin_html_hash);
  worker.ClearDedupAndCooldown(queued->url, queued->hostname, queued->scheme);
  SendNotification(AgentHtmlNotification(url, /*host_vouched=*/true));
  ASSERT_TRUE(pagespeed::test::WaitForNotificationsRetired(worker, 3,
                                                           kWorkerStallBudget));
  EXPECT_FALSE(worker.TestBrowserManager()->TestDequeue().has_value())
      << "a render of content known to fall below the floor was queued";
}

}  // namespace
}  // namespace pagespeed
