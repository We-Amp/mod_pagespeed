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

#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <thread>
#include <vector>

#include "gtest/gtest.h"
#include "lib/base/message_handler.h"
#include "lib/cache/cache.h"
#include "lib/cache/freshness.h"
#include "lib/classify/alternate_id.h"
#include "lib/classify/alternate_metadata.h"
#include "lib/classify/capability_mask.h"
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

}  // namespace
}  // namespace pagespeed
