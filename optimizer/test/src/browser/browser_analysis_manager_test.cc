// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

// Tests for BrowserAnalysisManager lifecycle, queue management, and
// status reporting.  These tests do NOT require Chrome -- they exercise
// the manager's state machine, queue interaction, and JSON status output
// using a real libuv event loop and (where needed) a real PageSpeedCache.

#include "src/worker/browser_analysis_manager.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "gtest/gtest.h"
#include "lib/base/message_handler.h"
#include "lib/cache/cache.h"
#include "lib/classify/capability_mask.h"
#include "lib/classify/url_normalizer.h"
#include "src/browser/browser_css_extractor.h"
#include "src/browser/optimization_profile.h"
#include "src/browser/page_analysis.h"
#include "src/browser/script_coverage_analyzer.h"
#include "src/worker/analysis_resource_map.h"
#include "src/worker/browser_analysis_manager_internal.h"
#include "src/worker/css_cache_inliner.h"
#include "test/test_util/temp_dir.h"
#include "uv.h"

namespace pagespeed {
namespace {

// Run the event loop briefly to process pending I/O and timers.
void RunLoop(uv_loop_t* loop) {
  for (int i = 0; i < 10; ++i) {
    if (uv_run(loop, UV_RUN_NOWAIT) == 0) break;
  }
}

// Close all handles and drain the loop.
void DrainLoop(uv_loop_t* loop) {
  uv_walk(
      loop,
      [](uv_handle_t* h, void*) {
        if (!uv_is_closing(h)) uv_close(h, nullptr);
      },
      nullptr);
  uv_run(loop, UV_RUN_DEFAULT);
}

// ============================================================
// Test fixture WITHOUT a real cache (cache_ = nullptr).
// Good for testing construction, shutdown, queue behavior, etc.
// ============================================================
class BrowserAnalysisManagerTest : public ::testing::Test {
 protected:
  void SetUp() override {
    loop_ = new uv_loop_t;
    ASSERT_EQ(0, uv_loop_init(loop_));

    config_.enabled = true;
    // Use a non-existent binary so StartChrome fails gracefully.
    config_.chrome_binary = "/nonexistent/chrome";
    config_.browser_queue_size = 100;

    handler_ = std::make_unique<NullMessageHandler>();
    mgr_ = std::make_unique<BrowserAnalysisManager>(loop_, config_, nullptr,
                                                    handler_.get());
  }

  void TearDown() override {
    // Shutdown the manager first (closes most handles).
    if (mgr_) mgr_->Shutdown();
    // Close any remaining handles (e.g., timer initialized but never
    // started -- Shutdown only closes active timers).
    DrainLoop(loop_);
    // Now safe to destroy the manager -- all its handles are closed.
    mgr_.reset();
    uv_loop_close(loop_);
    delete loop_;
  }

  uv_loop_t* loop_ = nullptr;
  BrowserAnalysisConfig config_;
  std::unique_ptr<NullMessageHandler> handler_;
  std::unique_ptr<BrowserAnalysisManager> mgr_;
};

// --- Construction ---

TEST_F(BrowserAnalysisManagerTest, DefaultStateNotRunning) {
  EXPECT_FALSE(mgr_->chrome_running());
}

TEST_F(BrowserAnalysisManagerTest, DefaultQueueEmpty) {
  EXPECT_EQ(0u, mgr_->queue_depth());
}

TEST_F(BrowserAnalysisManagerTest, CdpClientNullWhenChromeNotRunning) {
  EXPECT_EQ(nullptr, mgr_->cdp_client());
}

TEST_F(BrowserAnalysisManagerTest, StatsInitiallyZero) {
  const auto& s = mgr_->stats();
  EXPECT_EQ(0u, s.profiles_generated.load());
  EXPECT_EQ(0u, s.profiles_used.load());
  EXPECT_EQ(0u, s.analysis_errors.load());
  EXPECT_EQ(0u, s.chrome_restarts.load());
  EXPECT_EQ(0u, s.chrome_crashes.load());
  EXPECT_EQ(0u, s.queue_enqueued.load());
  EXPECT_EQ(0u, s.queue_dropped.load());
  EXPECT_EQ(0u, s.queue_processed.load());
  EXPECT_EQ(0u, s.css_inlining_attempted.load());
  EXPECT_EQ(0u, s.css_inlining_stylesheets_found.load());
  EXPECT_EQ(0u, s.css_inlining_stylesheets_cached.load());
  EXPECT_EQ(0u, s.css_inlining_bytes_inlined.load());
  EXPECT_EQ(0u, s.reanalyses_scheduled.load());
  EXPECT_EQ(0u, s.script_map_scripts_found.load());
  EXPECT_EQ(0u, s.script_map_scripts_cached.load());
  EXPECT_EQ(0u, s.script_map_scripts_uncached_same_origin.load());
  EXPECT_EQ(0u, s.script_map_scripts_cross_origin.load());
  EXPECT_EQ(0u, s.script_map_scripts_oversize.load());
  EXPECT_EQ(0u, s.script_map_bytes_mapped.load());
  EXPECT_EQ(0u, s.scripts_no_coverage.load());
  EXPECT_EQ(0u, s.script_fetches_served.load());
  EXPECT_EQ(0u, s.script_fetches_blocked.load());
}

// --- Initialize / Shutdown lifecycle ---

TEST_F(BrowserAnalysisManagerTest, InitializeSucceeds) {
  // Initialize inits async handle + timer and attempts Chrome start.
  // Chrome will fail (non-existent binary) but Initialize still returns
  // true because Chrome failure is non-fatal.
  EXPECT_TRUE(mgr_->Initialize());
  RunLoop(loop_);

  // Chrome should NOT be running.
  EXPECT_FALSE(mgr_->chrome_running());
}

TEST_F(BrowserAnalysisManagerTest, ShutdownWithoutInitialize) {
  // Calling Shutdown without Initialize should not crash.
  mgr_->Shutdown();
  EXPECT_FALSE(mgr_->chrome_running());
}

TEST_F(BrowserAnalysisManagerTest, DoubleShutdownIsSafe) {
  EXPECT_TRUE(mgr_->Initialize());
  RunLoop(loop_);
  mgr_->Shutdown();
  RunLoop(loop_);
  // Second shutdown is a no-op.
  mgr_->Shutdown();
  RunLoop(loop_);
}

TEST_F(BrowserAnalysisManagerTest, DestructorCallsShutdown) {
  EXPECT_TRUE(mgr_->Initialize());
  RunLoop(loop_);
  // Explicitly shutdown + drain before destroy (timer handle is embedded
  // in the manager object, so it must be closed before the object is freed).
  mgr_->Shutdown();
  DrainLoop(loop_);
  mgr_.reset();
}

// --- Enqueue behavior ---

TEST_F(BrowserAnalysisManagerTest, EnqueueBeforeInitialize) {
  // EnqueueAnalysis should work (queue is created in constructor).
  // Without Initialize, analysis_async_ is not initialized so
  // uv_async_send is skipped, but the item is queued.
  bool enqueued = mgr_->EnqueueAnalysis(
      "http://example.com/", "example.com", "http", /*site=*/"",
      /*host_vouched=*/false, /*from_module=*/false, 0x08, 12345);
  EXPECT_TRUE(enqueued);
  EXPECT_EQ(1u, mgr_->queue_depth());
  EXPECT_EQ(1u, mgr_->stats().queue_enqueued.load());
}

TEST_F(BrowserAnalysisManagerTest, EnqueueAfterShutdownReturnsFalse) {
  EXPECT_TRUE(mgr_->Initialize());
  RunLoop(loop_);
  mgr_->Shutdown();
  RunLoop(loop_);

  bool enqueued = mgr_->EnqueueAnalysis(
      "http://example.com/", "example.com", "http", /*site=*/"",
      /*host_vouched=*/false, /*from_module=*/false, 0x08, 12345);
  EXPECT_FALSE(enqueued);
  EXPECT_EQ(0u, mgr_->queue_depth());
}

TEST_F(BrowserAnalysisManagerTest, EnqueueMultipleItems) {
  for (int i = 0; i < 5; ++i) {
    bool enqueued = mgr_->EnqueueAnalysis(
        "http://example.com/" + std::to_string(i), "example.com", "http",
        /*site=*/"", /*host_vouched=*/false, /*from_module=*/false, 0x08,
        static_cast<uint64_t>(i) + 1);
    EXPECT_TRUE(enqueued);
  }
  EXPECT_EQ(5u, mgr_->queue_depth());
  EXPECT_EQ(5u, mgr_->stats().queue_enqueued.load());
}

// A template whose validation record stopped matching the served
// stylesheet is queued for a fresh analysis instead of waiting out the profile
// TTL, at most kMaxReanalysisRetries (2) times per TTL window and at least
// kReanalysisDelayMs (30 s) apart, per template.
TEST_F(BrowserAnalysisManagerTest, RevalidationIsRateLimitedPerTemplate) {
  using std::chrono::hours;
  using std::chrono::seconds;
  const auto t0 = std::chrono::steady_clock::now();
  auto request = [&](uint64_t hash, std::chrono::steady_clock::time_point at) {
    bool queued = mgr_->RequestRevalidation(
        "http://example.com/p", "example.com", "http", /*site=*/"",
        /*host_vouched=*/false, /*from_module=*/false, 0x08, hash,
        "<html></html>", std::nullopt, at);
    // Drain so the queue's own per-template dedup is not what is measured.
    while (mgr_->TestDequeue().has_value()) {
    }
    return queued;
  };

  EXPECT_TRUE(request(7, t0));
  EXPECT_FALSE(request(7, t0 + seconds(5))) << "too soon after the first";
  EXPECT_TRUE(request(8, t0 + seconds(5))) << "budgets are per template";
  EXPECT_TRUE(request(7, t0 + seconds(31)));
  EXPECT_FALSE(request(7, t0 + seconds(120))) << "two per window";
  EXPECT_FALSE(request(7, t0 + hours(23)));
  // A new TTL window (browser_profile_ttl_seconds, 24 h by default).
  EXPECT_TRUE(request(7, t0 + hours(24) + seconds(1)));

  EXPECT_EQ(4u, mgr_->stats().revalidations_queued.load());
  EXPECT_EQ(3u, mgr_->stats().revalidations_rate_limited.load());

  // The queued item is an ordinary analysis of the template: the page's own
  // bytes, keyed on the template hash.
  ASSERT_TRUE(mgr_->RequestRevalidation(
      "http://example.com/q", "example.com", "https", /*site=*/"",
      /*host_vouched=*/false, /*from_module=*/false, 0x08, 9, "<p>q</p>"));
  auto item = mgr_->TestDequeue();
  ASSERT_TRUE(item.has_value());
  EXPECT_EQ(9u, item->template_hash);
  EXPECT_EQ("http://example.com/q", item->url);
  EXPECT_EQ("<p>q</p>", item->original_html);
  EXPECT_FALSE(item->force_agent_render);
}

// When the per-template budget map is full, only the least recently used
// budget is evicted; the other templates keep theirs (clearing the map would
// reset every budget at once, a way around the cap).
TEST_F(BrowserAnalysisManagerTest, RevalidationBudgetOverflowEvictsTheOldest) {
  using std::chrono::minutes;
  using std::chrono::seconds;
  const auto t0 = std::chrono::steady_clock::now();
  auto request = [&](uint64_t hash, std::chrono::steady_clock::time_point at) {
    bool queued = mgr_->RequestRevalidation(
        "http://example.com/p", "example.com", "http", /*site=*/"",
        /*host_vouched=*/false, /*from_module=*/false, 0x08, hash,
        "<html></html>", std::nullopt, at);
    while (mgr_->TestDequeue().has_value()) {
    }
    return queued;
  };
  // Template 1 spends its budget first; template 2 spends it later.
  ASSERT_TRUE(request(1, t0));
  ASSERT_TRUE(request(1, t0 + seconds(31)));
  ASSERT_TRUE(request(2, t0 + minutes(1)));
  ASSERT_TRUE(request(2, t0 + minutes(2)));
  // Fill the map (4096 budgets) with templates used after both.
  for (uint64_t h = 3; h <= 4096; ++h) {
    ASSERT_TRUE(request(1000 + h, t0 + minutes(3)));
  }
  // One more template: the map is full, template 1 is the oldest.
  ASSERT_TRUE(request(99999, t0 + minutes(4)));
  EXPECT_FALSE(request(2, t0 + minutes(5))) << "kept, still spent";
  EXPECT_TRUE(request(1, t0 + minutes(5))) << "evicted, so a fresh budget";
}

TEST_F(BrowserAnalysisManagerTest, ForceAgentRenderDedupsByUrlNotTemplate) {
  // Normal (perf) enqueue dedups on the template hash —
  // distinct URLs of one template collapse (perf is template-amortized).
  EXPECT_TRUE(mgr_->EnqueueAnalysis("http://x.com/a", "x.com", "http",
                                    /*site=*/"", /*host_vouched=*/false,
                                    /*from_module=*/false, 0x08,
                                    /*template_hash=*/42));
  EXPECT_FALSE(mgr_->EnqueueAnalysis("http://x.com/b", "x.com", "http",
                                     /*site=*/"", /*host_vouched=*/false,
                                     /*from_module=*/false, 0x08,
                                     /*template_hash=*/42));  // same template
  EXPECT_EQ(1u, mgr_->queue_depth());

  // FORCED agent renders for distinct URLs sharing the SAME template must BOTH
  // enqueue (markdown is per-URL) — they dedup on the URL, not the template, so
  // they never starve each other.
  std::array<std::byte, 32> h{};
  EXPECT_TRUE(
      mgr_->EnqueueAnalysis("http://y.com/a", "y.com", "http", /*site=*/"",
                            /*host_vouched=*/false, /*from_module=*/false, 0x08,
                            /*template_hash=*/99, /*original_html=*/{}, h,
                            /*force_agent_render=*/true));
  EXPECT_TRUE(
      mgr_->EnqueueAnalysis("http://y.com/b", "y.com", "http", /*site=*/"",
                            /*host_vouched=*/false, /*from_module=*/false, 0x08,
                            /*template_hash=*/99, /*original_html=*/{}, h,
                            /*force_agent_render=*/true));
  // Same forced URL again -> deduped on the URL.
  EXPECT_FALSE(
      mgr_->EnqueueAnalysis("http://y.com/a", "y.com", "http", /*site=*/"",
                            /*host_vouched=*/false, /*from_module=*/false, 0x08,
                            /*template_hash=*/99, /*original_html=*/{}, h,
                            /*force_agent_render=*/true));
  EXPECT_EQ(3u, mgr_->queue_depth());  // x/a + y/a + y/b
}

// The worker hands EnqueueAnalysis the RAW pre-rewrite origin HTML it holds at
// enqueue time.  The item must OWN a copy (analysis runs asynchronously, later,
// on the Chrome pass — a string_view into the worker buffer would dangle), so
// RunAnalysis can use it instead of re-reading the volatile cache slot 0x08
// (which the worker overwrites raw -> optimized, or a concurrent origin-refresh
// purges, before the async pass runs).
TEST_F(BrowserAnalysisManagerTest, EnqueueCarriesOwnedOriginalHtml) {
  const std::string raw_html = "<html><body>raw origin bytes</body></html>";
  // Pass a string_view that goes out of scope right after the call to prove the
  // item owns its own copy.
  {
    std::string scratch = raw_html;
    EXPECT_TRUE(
        mgr_->EnqueueAnalysis("http://example.com/", "example.com", "http",
                              /*site=*/"", /*host_vouched=*/false,
                              /*from_module=*/false, 0x08, /*template_hash=*/7,
                              /*original_html=*/scratch));
    scratch.assign(scratch.size(), 'X');  // clobber the source buffer
  }
  auto item = mgr_->TestDequeue();
  ASSERT_TRUE(item.has_value());
  EXPECT_EQ(item->original_html, raw_html);
}

// When the worker supplies no HTML (empty original_html — e.g. a path that
// relies on the cache), the item carries an empty original_html so RunAnalysis
// falls back to the cache re-read (preserving the prior behavior).
TEST_F(BrowserAnalysisManagerTest, EnqueueWithoutHtmlLeavesOriginalHtmlEmpty) {
  EXPECT_TRUE(mgr_->EnqueueAnalysis("http://example.com/", "example.com",
                                    "http", /*site=*/"", /*host_vouched=*/false,
                                    /*from_module=*/false, 0x08,
                                    /*template_hash=*/8));
  auto item = mgr_->TestDequeue();
  ASSERT_TRUE(item.has_value());
  EXPECT_TRUE(item->original_html.empty());
}

// The supplied original_html (not any cache value) is the buffer carried on the
// item, even when a different value would be read from cache.
TEST_F(BrowserAnalysisManagerTest, EnqueuedOriginalHtmlIsTheSuppliedBytes) {
  const std::string supplied = "SUPPLIED-RAW-HTML";
  EXPECT_TRUE(mgr_->EnqueueAnalysis(
      "http://example.com/", "example.com", "http", /*site=*/"",
      /*host_vouched=*/false, /*from_module=*/false, 0x08, /*template_hash=*/9,
      /*original_html=*/supplied));
  auto item = mgr_->TestDequeue();
  ASSERT_TRUE(item.has_value());
  EXPECT_EQ(item->original_html, supplied);
}

TEST_F(BrowserAnalysisManagerTest, QueueFullDropsItem) {
  // Create a manager with a tiny queue.
  BrowserAnalysisConfig tiny_config = config_;
  tiny_config.browser_queue_size = 2;
  auto tiny_mgr = std::make_unique<BrowserAnalysisManager>(
      loop_, tiny_config, nullptr, handler_.get());

  // Fill the queue.
  EXPECT_TRUE(tiny_mgr->EnqueueAnalysis("http://a.com/", "a.com", "http",
                                        /*site=*/"", /*host_vouched=*/false,
                                        /*from_module=*/false, 0x08, 1));
  EXPECT_TRUE(tiny_mgr->EnqueueAnalysis("http://b.com/", "b.com", "http",
                                        /*site=*/"", /*host_vouched=*/false,
                                        /*from_module=*/false, 0x08, 2));
  EXPECT_EQ(2u, tiny_mgr->queue_depth());

  // Third item: queue evicts the lowest-priority item (head-drop).
  // AnalysisQueue does head-drop so enqueue still succeeds.
  bool enqueued = tiny_mgr->EnqueueAnalysis("http://c.com/", "c.com", "http",
                                            /*site=*/"", /*host_vouched=*/false,
                                            /*from_module=*/false, 0x08, 3);
  // Depending on priority, it may enqueue (evicting old) or be dropped.
  // Either way, queue depth remains at most 2.
  EXPECT_LE(tiny_mgr->queue_depth(), 2u);
  (void)enqueued;

  // tiny_mgr was never initialized so no handles to close.
  tiny_mgr.reset();
}

// --- StatusJson ---

TEST_F(BrowserAnalysisManagerTest, StatusJsonContainsRequiredFields) {
  EXPECT_TRUE(mgr_->Initialize());
  RunLoop(loop_);

  std::string json = mgr_->StatusJson();

  // Verify all expected fields are present.
  EXPECT_NE(std::string::npos, json.find("\"enabled\":true"));
  EXPECT_NE(std::string::npos, json.find("\"chrome_running\":false"));
  EXPECT_NE(std::string::npos, json.find("\"chrome_pid\":0"));
  EXPECT_NE(std::string::npos, json.find("\"queue_depth\":0"));
  EXPECT_NE(std::string::npos, json.find("\"analysis_in_progress\":false"));
  EXPECT_NE(std::string::npos, json.find("\"templates_tracked\":0"));
  EXPECT_NE(std::string::npos, json.find("\"profiles_generated\":0"));
  EXPECT_NE(std::string::npos, json.find("\"profiles_used\":0"));
  EXPECT_NE(std::string::npos, json.find("\"analysis_errors\":0"));
  EXPECT_NE(std::string::npos, json.find("\"chrome_restarts\":"));
  EXPECT_NE(std::string::npos, json.find("\"chrome_crashes\":"));
  EXPECT_NE(std::string::npos, json.find("\"queue_enqueued\":0"));
  EXPECT_NE(std::string::npos, json.find("\"queue_dropped\":0"));
  EXPECT_NE(std::string::npos, json.find("\"queue_processed\":0"));
  EXPECT_NE(std::string::npos, json.find("\"css_inlining_attempted\":0"));
  EXPECT_NE(std::string::npos,
            json.find("\"css_inlining_stylesheets_found\":0"));
  EXPECT_NE(std::string::npos,
            json.find("\"css_inlining_stylesheets_cached\":0"));
  EXPECT_NE(std::string::npos, json.find("\"css_inlining_bytes_inlined\":0"));
  EXPECT_NE(std::string::npos, json.find("\"reanalyses_scheduled\":0"));
}

TEST_F(BrowserAnalysisManagerTest, StatusJsonContainsScriptEvidenceCounters) {
  std::string json = mgr_->StatusJson();
  EXPECT_NE(std::string::npos, json.find("\"scripts_no_coverage\":0"));
  EXPECT_NE(std::string::npos, json.find("\"script_map_scripts_found\":0"));
  EXPECT_NE(std::string::npos, json.find("\"script_map_scripts_cached\":0"));
  EXPECT_NE(std::string::npos,
            json.find("\"script_map_scripts_uncached_same_origin\":0"));
  EXPECT_NE(std::string::npos,
            json.find("\"script_map_scripts_cross_origin\":0"));
  EXPECT_NE(std::string::npos, json.find("\"script_map_scripts_oversize\":0"));
  EXPECT_NE(std::string::npos, json.find("\"script_map_bytes_mapped\":0"));
  EXPECT_NE(std::string::npos, json.find("\"script_fetches_served\":0"));
  EXPECT_NE(std::string::npos, json.find("\"script_fetches_blocked\":0"));
}

TEST_F(BrowserAnalysisManagerTest, StatusJsonReflectsQueueDepth) {
  mgr_->EnqueueAnalysis("http://example.com/", "example.com", "http",
                        /*site=*/"", /*host_vouched=*/false,
                        /*from_module=*/false, 0x08, 42);

  std::string json = mgr_->StatusJson();
  EXPECT_NE(std::string::npos, json.find("\"queue_depth\":1"));
}

TEST_F(BrowserAnalysisManagerTest, StatusJsonReflectsEnqueueStats) {
  mgr_->EnqueueAnalysis("http://a.com/", "a.com", "http", /*site=*/"",
                        /*host_vouched=*/false, /*from_module=*/false, 0x08, 1);
  mgr_->EnqueueAnalysis("http://b.com/", "b.com", "http", /*site=*/"",
                        /*host_vouched=*/false, /*from_module=*/false, 0x08, 2);
  mgr_->EnqueueAnalysis("http://c.com/", "c.com", "http", /*site=*/"",
                        /*host_vouched=*/false, /*from_module=*/false, 0x08, 3);

  std::string json = mgr_->StatusJson();
  EXPECT_NE(std::string::npos, json.find("\"queue_enqueued\":3"));
}

// --- Template detector access ---

TEST_F(BrowserAnalysisManagerTest, TemplateDetectorAccessible) {
  auto& td = mgr_->template_detector();
  // Initially, no profiles recorded.
  EXPECT_FALSE(td.HasProfile(42));
}

// --- set_handler ---

TEST_F(BrowserAnalysisManagerTest, SetHandlerDoesNotCrash) {
  auto other_handler = std::make_unique<NullMessageHandler>();
  mgr_->set_handler(other_handler.get());
  // Use the manager normally after swapping handler.
  mgr_->EnqueueAnalysis("http://x.com/", "x.com", "http", /*site=*/"",
                        /*host_vouched=*/false, /*from_module=*/false, 0x08, 1);
  EXPECT_EQ(1u, mgr_->queue_depth());
}

// --- Loop accessor ---

TEST_F(BrowserAnalysisManagerTest, LoopReturnsConstructedLoop) {
  EXPECT_EQ(loop_, mgr_->loop());
}

// ============================================================
// Test fixture WITH a real PageSpeedCache for LookupProfile tests.
// ============================================================
class BrowserAnalysisManagerCacheTest : public ::testing::Test {
 protected:
  void SetUp() override {
    loop_ = new uv_loop_t;
    ASSERT_EQ(0, uv_loop_init(loop_));

    // Create a temporary cache.
    tmp_dir_ = pagespeed::test::MakeTempDir();
    std::filesystem::create_directories(tmp_dir_);
    cache_path_ = (tmp_dir_ / "cache.vol").string();

    PageSpeedCacheConfig cache_config;
    cache_config.volume_path = cache_path_;
    cache_config.volume_size = static_cast<uint64_t>(16 * 1024 * 1024);  // 16MB
    cache_config.ram_cache_size = 0;
    auto cache_result = PageSpeedCache::Create(cache_config);
    ASSERT_TRUE(cache_result.has_value()) << "Cache creation failed";
    cache_ = std::move(*cache_result);

    config_.enabled = true;
    config_.chrome_binary = "/nonexistent/chrome";
    config_.browser_queue_size = 100;
    config_.browser_profile_ttl_seconds = 86400;

    handler_ = std::make_unique<NullMessageHandler>();
    mgr_ = std::make_unique<BrowserAnalysisManager>(
        loop_, config_, cache_.get(), handler_.get());
  }

  void TearDown() override {
    if (mgr_) mgr_->Shutdown();
    DrainLoop(loop_);
    mgr_.reset();
    cache_.reset();
    uv_loop_close(loop_);
    delete loop_;
    std::filesystem::remove_all(tmp_dir_);
  }

  uv_loop_t* loop_ = nullptr;
  BrowserAnalysisConfig config_;
  std::unique_ptr<NullMessageHandler> handler_;
  std::unique_ptr<PageSpeedCache> cache_;
  std::unique_ptr<BrowserAnalysisManager> mgr_;
  std::filesystem::path tmp_dir_;
  std::string cache_path_;
};

TEST_F(BrowserAnalysisManagerCacheTest, LookupProfileReturnsNulloptForUnknown) {
  // No profile recorded for this hash.
  auto result = mgr_->LookupProfile(99999);
  EXPECT_FALSE(result.has_value());
}

TEST_F(BrowserAnalysisManagerCacheTest,
       LookupProfileReturnsNulloptWhenNotInCache) {
  // Record the template hash in the detector but don't write to cache.
  mgr_->template_detector().RecordProfile(42, "http://example.com/");

  auto result = mgr_->LookupProfile(42);
  // Template entry exists but cache read fails -> removes stale entry.
  EXPECT_FALSE(result.has_value());
  EXPECT_FALSE(mgr_->template_detector().HasProfile(42));
}

TEST_F(BrowserAnalysisManagerCacheTest, StoreAndLookupProfile) {
  uint64_t template_hash = 0xDEADBEEF;

  // Build a profile.
  OptimizationProfile profile;
  profile.template_hash_hex = "00000000deadbeef";
  profile.analyzed_url = "http://example.com/page";
  auto now = std::chrono::system_clock::now();
  profile.created_at =
      std::chrono::duration_cast<std::chrono::seconds>(now.time_since_epoch())
          .count();
  profile.expires_at = profile.created_at + 86400;  // 24h from now

  // Serialize and write to cache at the sentinel slot.
  std::string json = profile.ToJson();
  ASSERT_FALSE(json.empty());

  std::string cache_url = OptimizationProfile::CacheUrl(template_hash);
  auto wh = cache_->WriteSentinel(
      cache_url, std::string(OptimizationProfile::kProfileHostname), "https",
      SentinelId::kBrowserProfile, json.size());
  ASSERT_TRUE(wh.has_value());
  (void)wh->write_sync(std::as_bytes(std::span(json.data(), json.size())));
  (void)wh->close_sync();

  // Record in template detector.
  mgr_->template_detector().RecordProfile(template_hash, profile.analyzed_url);

  // Lookup should succeed.
  auto result = mgr_->LookupProfile(template_hash);
  ASSERT_TRUE(result.has_value());
  EXPECT_EQ("http://example.com/page", result->analyzed_url);
  EXPECT_EQ("00000000deadbeef", result->template_hash_hex);
  EXPECT_EQ(1u, mgr_->stats().profiles_used.load());
}

// Degraded-pass guard decision (the FinishAnalysis predicate:
// LooksLikeDegradedProfile(profile) && template_detector_.HasProfile(hash)).
// A degraded pass must NOT clobber an existing good profile, but a first-seen
// degraded profile is still stored (better than none).  Driving the private
// FinishAnalysis directly needs a live Chrome, so the decision is asserted via
// the same public predicate it uses.
TEST_F(BrowserAnalysisManagerCacheTest, DegradedPassKeepsPriorGoodProfile) {
  uint64_t template_hash = 0xABCD;
  // A prior good profile already exists for this template.
  mgr_->template_detector().RecordProfile(template_hash,
                                          "http://example.com/page");
  ASSERT_TRUE(mgr_->template_detector().HasProfile(template_hash));

  // The just-finished pass looks degraded (full-stylesheet over-report).
  OptimizationProfile degraded;
  degraded.desktop.critical_css = "html{}body{}div{}";
  degraded.desktop.css_coverage_ratio = 0.99f;

  // FinishAnalysis would SKIP storing: degraded AND a prior profile exists.
  bool skip_store = browser_internal::LooksLikeDegradedProfile(degraded) &&
                    mgr_->template_detector().HasProfile(template_hash);
  EXPECT_TRUE(skip_store);
  // And the prior good profile survives.
  EXPECT_TRUE(mgr_->template_detector().HasProfile(template_hash));
}

TEST_F(BrowserAnalysisManagerCacheTest, DegradedFirstSeenProfileIsStored) {
  uint64_t template_hash = 0xBEEF;
  // No prior profile for this template.
  ASSERT_FALSE(mgr_->template_detector().HasProfile(template_hash));

  OptimizationProfile degraded;
  degraded.mobile.critical_css = "html{}body{}";
  degraded.mobile.css_coverage_ratio = 1.0f;

  // FinishAnalysis stores it (degraded > none for a first-seen template).
  bool skip_store = browser_internal::LooksLikeDegradedProfile(degraded) &&
                    mgr_->template_detector().HasProfile(template_hash);
  EXPECT_FALSE(skip_store);
}

TEST_F(BrowserAnalysisManagerCacheTest, TightProfileWithPriorIsStored) {
  uint64_t template_hash = 0xFEED;
  mgr_->template_detector().RecordProfile(template_hash,
                                          "http://example.com/page");

  OptimizationProfile tight;
  tight.desktop.critical_css = "div{color:red}";
  tight.desktop.css_coverage_ratio = 0.40f;

  // A tight (non-degraded) profile is stored as before — no regression.
  bool skip_store = browser_internal::LooksLikeDegradedProfile(tight) &&
                    mgr_->template_detector().HasProfile(template_hash);
  EXPECT_FALSE(skip_store);
}

// ---------------------------------------------------------------------------
// AgentBindingStillLive store-time re-validation predicate.
// ---------------------------------------------------------------------------

namespace {
// Write a kContentHash sentinel (32 bytes) for a key.
void WriteContentHash(PageSpeedCache& cache, const std::string& url,
                      const std::array<std::byte, 32>& hash) {
  auto wh = cache.WriteSentinel(url, "example.com", "https",
                                SentinelId::kContentHash, hash.size());
  ASSERT_TRUE(wh.has_value());
  ASSERT_TRUE(wh->write_sync(std::span<const std::byte>(hash)).has_value());
  ASSERT_TRUE(wh->close_sync().has_value());
}

AnalysisQueue::Item MakeAgentItem(const std::string& url, bool has_hash,
                                  const std::array<std::byte, 32>& hash) {
  AnalysisQueue::Item item;
  item.url = url;
  item.hostname = "example.com";
  item.scheme = "https";
  item.has_origin_html_hash = has_hash;
  item.origin_html_hash = hash;
  return item;
}
}  // namespace

TEST_F(BrowserAnalysisManagerCacheTest, AgentBindingStillLiveMatchingHash) {
  std::array<std::byte, 32> h{};
  for (size_t i = 0; i < h.size(); ++i) h[i] = static_cast<std::byte>(i + 1);
  WriteContentHash(*cache_, "/page.html", h);
  auto item = MakeAgentItem("/page.html", /*has_hash=*/true, h);
  EXPECT_TRUE(browser_internal::AgentBindingStillLive(*cache_, item));
}

TEST_F(BrowserAnalysisManagerCacheTest, AgentBindingMismatchDiscards) {
  std::array<std::byte, 32> live{};
  for (size_t i = 0; i < live.size(); ++i) live[i] = static_cast<std::byte>(i);
  WriteContentHash(*cache_, "/page.html", live);
  // Captured a DIFFERENT hash (origin advanced during the render).
  std::array<std::byte, 32> captured{};
  for (size_t i = 0; i < captured.size(); ++i)
    captured[i] = static_cast<std::byte>(0xFF - i);
  auto item = MakeAgentItem("/page.html", /*has_hash=*/true, captured);
  EXPECT_FALSE(browser_internal::AgentBindingStillLive(*cache_, item));
}

TEST_F(BrowserAnalysisManagerCacheTest, AgentBindingAbsentSentinelDiscards) {
  // No kContentHash written for this key.
  std::array<std::byte, 32> h{};
  for (size_t i = 0; i < h.size(); ++i) h[i] = static_cast<std::byte>(i + 1);
  auto item = MakeAgentItem("/no-sentinel.html", /*has_hash=*/true, h);
  EXPECT_FALSE(browser_internal::AgentBindingStillLive(*cache_, item));
}

TEST_F(BrowserAnalysisManagerCacheTest, AgentBindingNoCapturedHashDiscards) {
  std::array<std::byte, 32> h{};
  for (size_t i = 0; i < h.size(); ++i) h[i] = static_cast<std::byte>(i + 1);
  WriteContentHash(*cache_, "/page.html", h);
  // has_origin_html_hash=false -> cannot prove binding -> discard.
  auto item = MakeAgentItem("/page.html", /*has_hash=*/false, h);
  EXPECT_FALSE(browser_internal::AgentBindingStillLive(*cache_, item));
}

// The re-analysis retry MUST carry the content binding (and scheme)
// forward, else 0%-critical-CSS pages have their agent markdown amputated on
// retry (the re-analysis path bypasses EnqueueAnalysis where the hash is set).
TEST(BuildReanalysisItemTest, CarriesSchemeAndBindingForward) {
  std::array<std::byte, 32> h{};
  for (size_t i = 0; i < h.size(); ++i) h[i] = static_cast<std::byte>(i + 7);
  AnalysisQueue::Item src;
  src.url = "/p.html";
  src.hostname = "example.com";
  src.scheme = "http";  // non-default — must survive
  src.mask = 0x08;
  src.template_hash = 0xABCD;
  src.retry_count = 0;
  src.has_origin_html_hash = true;
  src.origin_html_hash = h;

  auto retry =
      browser_internal::BuildReanalysisItem(src, /*next_retry_count=*/1);
  EXPECT_EQ(retry.url, "/p.html");
  EXPECT_EQ(retry.hostname, "example.com");
  EXPECT_EQ(retry.scheme, "http");
  EXPECT_EQ(retry.mask, 0x08u);
  EXPECT_EQ(retry.template_hash, 0xABCDu);
  EXPECT_EQ(retry.retry_count, 1);
  EXPECT_TRUE(retry.has_origin_html_hash);
  EXPECT_EQ(retry.origin_html_hash, h);
}

TEST(BuildReanalysisItemTest, UnboundStaysUnbound) {
  AnalysisQueue::Item src;
  src.url = "/p.html";
  src.has_origin_html_hash = false;
  auto retry = browser_internal::BuildReanalysisItem(src, 2);
  EXPECT_FALSE(retry.has_origin_html_hash);
  EXPECT_EQ(retry.retry_count, 2);
}

// A retry intentionally does NOT carry original_html forward: it is a fresh
// analysis (not a warmup), so the retry path falls back to the cache re-read in
// RunAnalysis.  Resetting it keeps memory bounded and preserves the documented
// fallback contract this fix relies on.
TEST(BuildReanalysisItemTest, OriginalHtmlIsResetForRetry) {
  AnalysisQueue::Item src;
  src.url = "/p.html";
  src.original_html = "<html>raw origin bytes</html>";
  auto retry = browser_internal::BuildReanalysisItem(src, 1);
  EXPECT_TRUE(retry.original_html.empty());
}

// ---------------------------------------------------------------------------
// ShouldFoldDeferRecommendation — the OnScriptAnalysisDone fold gate.
// ---------------------------------------------------------------------------

TEST(ShouldFoldDeferRecommendationTest, SafeToDeferAbsoluteUrlFolds) {
  EXPECT_TRUE(browser_internal::ShouldFoldDeferRecommendation(
      "https://example.com/app.js", DeferralAdvice::kSafeToDefer));
}

TEST(ShouldFoldDeferRecommendationTest, CandidateForAsyncAbsoluteUrlFolds) {
  // The known async-vs-defer conflation, deliberately kept.
  EXPECT_TRUE(browser_internal::ShouldFoldDeferRecommendation(
      "https://example.com/widget.js", DeferralAdvice::kCandidateForAsync));
}

TEST(ShouldFoldDeferRecommendationTest, NoCoverageDataNeverFolds) {
  EXPECT_FALSE(browser_internal::ShouldFoldDeferRecommendation(
      "https://example.com/blocked.js", DeferralAdvice::kNoCoverageData));
}

TEST(ShouldFoldDeferRecommendationTest, KeepSynchronousNeverFolds) {
  EXPECT_FALSE(browser_internal::ShouldFoldDeferRecommendation(
      "https://example.com/critical.js", DeferralAdvice::kKeepSynchronous));
}

TEST(ShouldFoldDeferRecommendationTest, AlreadyAsyncNeverFolds) {
  EXPECT_FALSE(browser_internal::ShouldFoldDeferRecommendation(
      "https://example.com/a.js", DeferralAdvice::kAlreadyAsync));
}

// Belt-and-braces: a non-URL key must never reach the production
// suffix-matcher (a legacy "inline-3" would suffix-match a real src ending in
// "/inline-3").
TEST(ShouldFoldDeferRecommendationTest, NonUrlKeyNeverFolds) {
  EXPECT_FALSE(browser_internal::ShouldFoldDeferRecommendation(
      "inline-3", DeferralAdvice::kSafeToDefer));
  EXPECT_FALSE(browser_internal::ShouldFoldDeferRecommendation(
      "", DeferralAdvice::kSafeToDefer));
  EXPECT_FALSE(browser_internal::ShouldFoldDeferRecommendation(
      "/relative/app.js", DeferralAdvice::kSafeToDefer));
}

// ---------------------------------------------------------------------------
// ProfileExpiryFor — cold-script TTL heal.
// ---------------------------------------------------------------------------

TEST(ProfileExpiryForTest, WarmProfileGetsConfiguredTtl) {
  EXPECT_EQ(browser_internal::ProfileExpiryFor(1000, 86400, false),
            1000 + 86400);
}

TEST(ProfileExpiryForTest, ColdScriptsClampToHealTtl) {
  EXPECT_EQ(browser_internal::ProfileExpiryFor(1000, 86400, true),
            1000 + browser_internal::kColdScriptProfileTtlSeconds);
}

TEST(ProfileExpiryForTest, ColdNeverExtendsShortConfiguredTtl) {
  // min(): a configured TTL below the heal TTL wins either way.
  EXPECT_EQ(browser_internal::ProfileExpiryFor(1000, 600, true), 1000 + 600);
  EXPECT_EQ(browser_internal::ProfileExpiryFor(1000, 600, false), 1000 + 600);
}

// The TTL heal keys on HEALABLE misses only: a page whose scripts are all
// cross-origin has nothing a re-analysis could ever map (offline analysis),
// so its profile must keep the FULL configured TTL — not burn four Chrome
// sessions an hour forever.
TEST(ProfileExpiryForTest, CrossOriginOnlyPageKeepsConfiguredTtl) {
  AnalysisResourceMapStats stats;
  auto map = BuildAnalysisResourceMap(
      "<html><head>"
      "<script src=\"https://cdn.example.net/lib.js\"></script>"
      "<script src=\"//cdn.other.com/x.js\"></script>"
      "</head></html>",
      "/index.html", "example.com", "https",
      [](std::string_view) -> std::optional<std::string> {
        return std::nullopt;
      },
      UrlNormalizationConfig(), &stats);

  EXPECT_TRUE(map.empty());
  EXPECT_EQ(2u, stats.scripts_cross_origin);
  EXPECT_EQ(0u, stats.scripts_uncached_same_origin);
  EXPECT_EQ(browser_internal::ProfileExpiryFor(
                1000, 86400, stats.scripts_uncached_same_origin > 0),
            1000 + 86400);
}

// Same shape for the cached-but-empty class: an empty .js file (stub,
// feature-flag placeholder) is cached, so a re-analysis can never "heal" it
// into coverage evidence.  It must not trip the heal clamp, or every
// analysis would re-shorten the TTL and the template would burn a full
// multi-session analysis every hour forever.
TEST(ProfileExpiryForTest, CachedEmptyOnlyPageKeepsConfiguredTtl) {
  AnalysisResourceMapStats stats;
  auto map = BuildAnalysisResourceMap(
      "<html><head><script src=\"/stub.js\"></script></head></html>",
      "/index.html", "example.com", "https",
      [](std::string_view) -> std::optional<std::string> {
        return std::string();
      },
      UrlNormalizationConfig(), &stats);

  EXPECT_TRUE(map.empty());
  EXPECT_EQ(1u, stats.scripts_empty_same_origin);
  EXPECT_EQ(0u, stats.scripts_uncached_same_origin);
  EXPECT_EQ(browser_internal::ProfileExpiryFor(
                1000, 86400, stats.scripts_uncached_same_origin > 0),
            1000 + 86400);
}

// TTL heal round-trip: a profile built while same-origin scripts were
// uncached carries the clamped expiry through the store/lookup seam, so the
// template re-analyzes after ~1h instead of pinning keep-sync for 24h.
TEST_F(BrowserAnalysisManagerCacheTest, ColdScriptProfileTtlRoundTrip) {
  uint64_t template_hash = 0xC01D;

  OptimizationProfile profile;
  profile.template_hash_hex = "000000000000c01d";
  profile.analyzed_url = "http://example.com/cold";
  auto now = std::chrono::system_clock::now();
  profile.created_at =
      std::chrono::duration_cast<std::chrono::seconds>(now.time_since_epoch())
          .count();
  profile.expires_at = browser_internal::ProfileExpiryFor(
      profile.created_at, config_.browser_profile_ttl_seconds,
      /*same_origin_scripts_uncached=*/true);

  mgr_->TestStoreProfile(template_hash, profile);

  auto result = mgr_->LookupProfile(template_hash);
  ASSERT_TRUE(result.has_value());
  EXPECT_EQ(
      result->expires_at,
      profile.created_at + browser_internal::kColdScriptProfileTtlSeconds);
  EXPECT_LT(result->expires_at,
            profile.created_at + config_.browser_profile_ttl_seconds);
}

TEST_F(BrowserAnalysisManagerCacheTest, LookupExpiredProfileReturnsNullopt) {
  uint64_t template_hash = 0xCAFEBABE;

  // Build an expired profile.
  OptimizationProfile profile;
  profile.template_hash_hex = "00000000cafebabe";
  profile.analyzed_url = "http://example.com/expired";
  profile.created_at = 1000;
  profile.expires_at = 1001;  // Expired long ago.

  std::string json = profile.ToJson();
  ASSERT_FALSE(json.empty());

  std::string cache_url = OptimizationProfile::CacheUrl(template_hash);
  auto wh = cache_->WriteSentinel(
      cache_url, std::string(OptimizationProfile::kProfileHostname), "https",
      SentinelId::kBrowserProfile, json.size());
  ASSERT_TRUE(wh.has_value());
  (void)wh->write_sync(std::as_bytes(std::span(json.data(), json.size())));
  (void)wh->close_sync();

  mgr_->template_detector().RecordProfile(template_hash, profile.analyzed_url);

  // Lookup should fail (expired) and remove the template entry.
  auto result = mgr_->LookupProfile(template_hash);
  EXPECT_FALSE(result.has_value());
  EXPECT_FALSE(mgr_->template_detector().HasProfile(template_hash));
  EXPECT_EQ(0u, mgr_->stats().profiles_used.load());
}

TEST_F(BrowserAnalysisManagerCacheTest,
       RefreshedProfileOverwriteEvictsStaleRamCopy) {
  // Issue #1126: Cyclone's RAM tier is write-around (reads populate it,
  // writes do NOT evict).  LookupProfile reads the kBrowserProfile sentinel
  // in-process before a refresh overwrites it via StoreProfile/
  // TestStoreProfile — without the post-commit eviction the stale RAM copy
  // is served indefinitely (and once its expires_at lapses, LookupProfile
  // even RemoveProfile()s the template entry, so the fresh on-disk profile
  // is never used).  The fixture cache runs with the RAM tier disabled
  // (ram_cache_size = 0), so this test uses its own RAM-enabled cache and
  // manager — a true call-site negative control: it fails if the eviction
  // in TestStoreProfile is removed.
  PageSpeedCacheConfig cache_config;
  cache_config.volume_path = (tmp_dir_ / "cache-ram.vol").string();
  cache_config.volume_size = static_cast<uint64_t>(16 * 1024 * 1024);
  // ram_cache_size defaults to 64MB — the RAM tier is enabled.
  auto cache_result = PageSpeedCache::Create(cache_config);
  ASSERT_TRUE(cache_result.has_value()) << "RAM-enabled cache creation failed";
  auto ram_cache = std::move(*cache_result);
  auto ram_mgr = std::make_unique<BrowserAnalysisManager>(
      loop_, config_, ram_cache.get(), handler_.get());

  const uint64_t template_hash = 0x5A1E;
  auto make_profile = [](const std::string& url_suffix) {
    OptimizationProfile profile;
    profile.template_hash_hex = "0000000000005a1e";
    profile.analyzed_url = "http://example.com/" + url_suffix;
    auto now = std::chrono::system_clock::now();
    profile.created_at =
        std::chrono::duration_cast<std::chrono::seconds>(now.time_since_epoch())
            .count();
    profile.expires_at = profile.created_at + 86400;
    return profile;
  };

  ram_mgr->TestStoreProfile(template_hash, make_profile("v1"));

  // Two lookups: the first marks the slot seen, the second admits the v1
  // JSON into this process's RAM tier (CLFUS).
  ASSERT_TRUE(ram_mgr->LookupProfile(template_hash).has_value());
  ASSERT_TRUE(ram_mgr->LookupProfile(template_hash).has_value());

  // The refresh overwrites the same sentinel slot in-process.
  ram_mgr->TestStoreProfile(template_hash, make_profile("v2"));

  auto result = ram_mgr->LookupProfile(template_hash);
  ASSERT_TRUE(result.has_value());
  EXPECT_EQ("http://example.com/v2", result->analyzed_url)
      << "stale RAM copy of the pre-refresh profile was served (issue #1126)";

  ram_mgr->Shutdown();
}

// --- AnalysisQueue::Item retry_count ---

TEST_F(BrowserAnalysisManagerTest, ItemRetryCountDefaultsToZero) {
  AnalysisQueue::Item item;
  EXPECT_EQ(0, item.retry_count);
}

TEST_F(BrowserAnalysisManagerTest, ReanalysesScheduledInitiallyZero) {
  EXPECT_EQ(0u, mgr_->stats().reanalyses_scheduled.load());
}

TEST_F(BrowserAnalysisManagerTest, StatusJsonContainsReanalysesScheduled) {
  // StatusJson() works without Initialize() since it reads atomic stats.
  std::string json = mgr_->StatusJson();
  EXPECT_NE(std::string::npos, json.find("\"reanalyses_scheduled\":0"));
}

// ===========================================================================
// Direct unit tests for browser_internal helper functions
// ===========================================================================

using namespace browser_internal;

// ---------------------------------------------------------------------------
// ViewportToIndex tests
// ---------------------------------------------------------------------------

// Exit 21 is PROFILE_IN_USE; name it so an operator need not decode it.
TEST(ChromeExitReasonTest, ProfileLockExitIsNamed) {
  EXPECT_EQ(browser_internal::ChromeExitReason(21, 0),
            "profile directory locked by another Chrome instance (exit 21)");
}

TEST(ChromeExitReasonTest, UnknownStatusAndSignalsHaveNoReason) {
  EXPECT_TRUE(browser_internal::ChromeExitReason(0, 0).empty());
  EXPECT_TRUE(browser_internal::ChromeExitReason(1, 0).empty());
  EXPECT_TRUE(browser_internal::ChromeExitReason(21, 9).empty());
  EXPECT_TRUE(browser_internal::ChromeExitReason(0, 5).empty());
}

TEST(ViewportToIndexTest, MobileReturnsZero) {
  EXPECT_EQ(ViewportToIndex(CapabilityMask::Viewport::kMobile), 0);
}

TEST(ViewportToIndexTest, TabletReturnsOne) {
  EXPECT_EQ(ViewportToIndex(CapabilityMask::Viewport::kTablet), 1);
}

TEST(ViewportToIndexTest, DesktopReturnsTwo) {
  EXPECT_EQ(ViewportToIndex(CapabilityMask::Viewport::kDesktop), 2);
}

TEST(ViewportToIndexTest, InvalidViewportDefaultsToDesktop) {
  // Cast an out-of-range value to Viewport to test the default case.
  auto invalid = static_cast<CapabilityMask::Viewport>(99);
  EXPECT_EQ(ViewportToIndex(invalid), 2);
}

// ---------------------------------------------------------------------------
// GetViewportProfile tests
// ---------------------------------------------------------------------------

TEST(GetViewportProfileTest, IndexZeroReturnsMobile) {
  OptimizationProfile profile;
  profile.mobile.critical_css = "mobile-css";
  ViewportProfile* vp = GetViewportProfile(profile, 0);
  ASSERT_NE(vp, nullptr);
  EXPECT_EQ(vp->critical_css, "mobile-css");
  EXPECT_EQ(vp, &profile.mobile);
}

TEST(GetViewportProfileTest, IndexOneReturnsTablet) {
  OptimizationProfile profile;
  profile.tablet.critical_css = "tablet-css";
  ViewportProfile* vp = GetViewportProfile(profile, 1);
  ASSERT_NE(vp, nullptr);
  EXPECT_EQ(vp->critical_css, "tablet-css");
  EXPECT_EQ(vp, &profile.tablet);
}

TEST(GetViewportProfileTest, IndexTwoReturnsDesktop) {
  OptimizationProfile profile;
  profile.desktop.critical_css = "desktop-css";
  ViewportProfile* vp = GetViewportProfile(profile, 2);
  ASSERT_NE(vp, nullptr);
  EXPECT_EQ(vp->critical_css, "desktop-css");
  EXPECT_EQ(vp, &profile.desktop);
}

TEST(GetViewportProfileTest, InvalidIndexDefaultsToDesktop) {
  OptimizationProfile profile;
  profile.desktop.critical_css = "desktop-default";
  ViewportProfile* vp = GetViewportProfile(profile, 99);
  ASSERT_NE(vp, nullptr);
  EXPECT_EQ(vp->critical_css, "desktop-default");
  EXPECT_EQ(vp, &profile.desktop);
}

TEST(GetViewportProfileTest, NegativeIndexDefaultsToDesktop) {
  OptimizationProfile profile;
  profile.desktop.lcp_url = "desktop-lcp";
  ViewportProfile* vp = GetViewportProfile(profile, -1);
  ASSERT_NE(vp, nullptr);
  EXPECT_EQ(vp->lcp_url, "desktop-lcp");
  EXPECT_EQ(vp, &profile.desktop);
}

TEST(GetViewportProfileTest, ReturnsWritablePointer) {
  // Verify that the returned pointer allows modification.
  OptimizationProfile profile;
  ViewportProfile* vp = GetViewportProfile(profile, 0);
  vp->critical_css = "modified";
  EXPECT_EQ(profile.mobile.critical_css, "modified");
}

TEST(GetViewportProfileTest, ViewportToIndexAndGetViewportProfileRoundTrip) {
  // Verify that ViewportToIndex output can be used with GetViewportProfile.
  OptimizationProfile profile;
  profile.mobile.lcp_selector = "mobile-lcp";
  profile.tablet.lcp_selector = "tablet-lcp";
  profile.desktop.lcp_selector = "desktop-lcp";

  int mobile_idx = ViewportToIndex(CapabilityMask::Viewport::kMobile);
  int tablet_idx = ViewportToIndex(CapabilityMask::Viewport::kTablet);
  int desktop_idx = ViewportToIndex(CapabilityMask::Viewport::kDesktop);

  EXPECT_EQ(GetViewportProfile(profile, mobile_idx)->lcp_selector,
            "mobile-lcp");
  EXPECT_EQ(GetViewportProfile(profile, tablet_idx)->lcp_selector,
            "tablet-lcp");
  EXPECT_EQ(GetViewportProfile(profile, desktop_idx)->lcp_selector,
            "desktop-lcp");
}

// ---------------------------------------------------------------------------
// LooksLikeDegradedProfile tests (FCP coverage over-report detection)
// ---------------------------------------------------------------------------

TEST(LooksLikeDegradedProfileTest, FullCoverageIsDegraded) {
  OptimizationProfile profile;
  profile.desktop.critical_css = "div{color:red}";
  profile.desktop.css_coverage_ratio = 1.0f;
  EXPECT_TRUE(LooksLikeDegradedProfile(profile));
}

TEST(LooksLikeDegradedProfileTest, NearFullCoverageIsDegraded) {
  OptimizationProfile profile;
  profile.mobile.critical_css = "div{color:red}";
  profile.mobile.css_coverage_ratio = 0.97f;
  EXPECT_TRUE(LooksLikeDegradedProfile(profile));
}

TEST(LooksLikeDegradedProfileTest, AtThresholdIsDegraded) {
  OptimizationProfile profile;
  profile.tablet.critical_css = "div{color:red}";
  profile.tablet.css_coverage_ratio = kDegradedCoverageThreshold;
  EXPECT_TRUE(LooksLikeDegradedProfile(profile));
}

TEST(LooksLikeDegradedProfileTest, TightCoverageIsNotDegraded) {
  OptimizationProfile profile;
  profile.mobile.critical_css = "div{color:red}";
  profile.mobile.css_coverage_ratio = 0.30f;
  profile.desktop.critical_css = "div{color:red}";
  profile.desktop.css_coverage_ratio = 0.45f;
  EXPECT_FALSE(LooksLikeDegradedProfile(profile));
}

TEST(LooksLikeDegradedProfileTest, EmptyViewportsAreNotCounted) {
  // A viewport with a high ratio but EMPTY critical CSS must not trip the
  // guard — that is the all-empty re-analysis case, not the over-report case.
  OptimizationProfile profile;
  profile.mobile.css_coverage_ratio = 1.0f;  // but critical_css empty
  EXPECT_FALSE(LooksLikeDegradedProfile(profile));
}

TEST(LooksLikeDegradedProfileTest, DefaultProfileIsNotDegraded) {
  OptimizationProfile profile;  // all viewports empty, ratio 0.0
  EXPECT_FALSE(LooksLikeDegradedProfile(profile));
}

TEST(LooksLikeDegradedProfileTest, AnyDegradedViewportTrips) {
  // One tight viewport, one degraded viewport -> degraded.
  OptimizationProfile profile;
  profile.mobile.critical_css = "div{color:red}";
  profile.mobile.css_coverage_ratio = 0.20f;
  profile.desktop.critical_css = "div{color:red}";
  profile.desktop.css_coverage_ratio = 0.99f;
  EXPECT_TRUE(LooksLikeDegradedProfile(profile));
}

// ---------------------------------------------------------------------------
// ShouldInlineCriticalCss tests (Issue A: coverage budget before inlining)
// ---------------------------------------------------------------------------
//
// Gate the inline-or-defer decision: high-coverage profile CSS (the Tailwind
// near-whole-sheet over-report below the kDegradedCoverageThreshold=0.95 storage
// guard) must NOT be inlined+async-deferred — leave the original render-blocking
// external <link> untouched (correct on a small immutable sheet).
//
// PRODUCTION INVARIANT: the extractor reports coverage_ratio == critical_bytes /
// total_css_bytes, so the budget is a single coverage gate with an exact 0.60
// floor (an earlier draft's second `critical_bytes > 0.5*total` gate was
// mathematically redundant and silently lowered the effective floor to 0.50 —
// removed).  These tests pin the 0.60 floor precisely.

// The pathological prod case: 0.85 coverage (93363 critical of 110388 total =
// 85% of the sheet).  This is the bug — must NOT inline.
TEST(ShouldInlineCriticalCssTest, ShouldNotInlineHighCoverageCriticalCss) {
  EXPECT_FALSE(ShouldInlineCriticalCss(0.85f));
}

// A genuine small critical subset: low coverage -> inline (the win we must
// preserve; do not over-suppress).
TEST(ShouldInlineCriticalCssTest, InlinesLowCoverageCriticalCss) {
  EXPECT_TRUE(ShouldInlineCriticalCss(0.30f));
}

// The 0.60 floor must be EXACT: a coverage of 0.55 (which in production means
// ~55% of the sheet, e.g. 60713 of 110388 bytes) is below the 0.60 cap and is
// still a worthwhile inline — the removed redundant byte gate would have wrongly
// suppressed this (60713 > 0.5*110388), so this pins the floor at 0.60 not 0.50.
TEST(ShouldInlineCriticalCssTest, BetweenHalfAndCapStillInlines) {
  EXPECT_TRUE(ShouldInlineCriticalCss(0.55f));
}

// The extractor keeps its rules for wide replaced elements within what the
// worker will inline; its copy of the two limits must be these.
TEST(ShouldInlineCriticalCssTest, ExtractorInlineLimitsMatchTheWorkers) {
  const CriticalCssConfig config;
  EXPECT_EQ(config.inline_limit_bytes,
            browser_internal::kInlineCriticalCssMaxBytes);
  EXPECT_EQ(config.inline_limit_coverage,
            browser_internal::kInlineCriticalCssMaxCoverage);
}

// Coverage at/above the cap suppresses.
TEST(ShouldInlineCriticalCssTest, AtCoverageCapSuppresses) {
  EXPECT_FALSE(ShouldInlineCriticalCss(kInlineCriticalCssMaxCoverage));
}

// Just below the cap -> inline.
TEST(ShouldInlineCriticalCssTest, JustBelowCapInlines) {
  EXPECT_TRUE(ShouldInlineCriticalCss(0.59f));
}

// Just at/above the cap by the smallest representable margin -> suppress.
TEST(ShouldInlineCriticalCssTest, JustAboveCapSuppresses) {
  EXPECT_FALSE(ShouldInlineCriticalCss(0.601f));
}

// ---------------------------------------------------------------------------
// Empirical critical-CSS validation (issue #1056)
//
// The stamping half is asserted against the SERVE path's own accept predicate
// (AsyncCssValidatedForServedSheet), not against a re-implementation of it: a
// producer tested against a copy of its consumer only ever proves that the copy
// agrees with itself.
// ---------------------------------------------------------------------------

namespace {

ValidationVerdict PassingVerdict(float diff_ratio = 0.001f) {
  ValidationVerdict v;
  v.validated = true;
  v.diff_ratio = diff_ratio;
  return v;
}

ValidationVerdict FailingVerdict(float diff_ratio = 0.31f) {
  ValidationVerdict v;
  v.validated = false;
  v.diff_ratio = diff_ratio;
  v.failure_reason = "above-the-fold appearance changed";
  return v;
}

// The block must be a STRICT byte-distinct subset of the sheet. Made equal —
// as they briefly were — a record bound to the critical block instead of the
// served stylesheet passes every assertion here, and ships a feature whose
// records can never match at serve time: on at all, inert always.
constexpr char kSheet[] =
    "@layer base{.hero{display:flex}}@layer base{.footer{display:grid}}";
constexpr char kOtherSheet[] =
    "@layer base{.hero{display:grid}}@layer base{.footer{display:grid}}";
constexpr char kBlock[] = "@layer base{.hero{display:flex}}";
static_assert(std::string_view(kBlock) != std::string_view(kSheet),
              "the block must not be byte-identical to the sheet");

}  // namespace

TEST(CriticalCssValidationRecordTest,
     ConfirmedBlockYieldsARecordTheServePathAccepts) {
  ViewportProfile vp;
  browser_internal::ApplyValidationVerdict(vp, PassingVerdict(), kBlock,
                                           kSheet);

  // The gate that reads this record is the authority on whether it counts.
  EXPECT_TRUE(AsyncCssValidatedForServedSheet(&vp, kSheet));
  EXPECT_FLOAT_EQ(vp.validation_diff_ratio, 0.001f);
  EXPECT_FALSE(vp.validated_critical_css_hash.empty())
      << "the block that was actually rendered must be identifiable later";
  // The two hashes are over different things. Equal means the record was bound
  // to the critical block rather than the served stylesheet — which the serve
  // path then compares against the sheet, and never matches.
  EXPECT_NE(vp.validated_critical_css_hash, vp.validated_combined_css_hash);
}

TEST(CriticalCssValidationRecordTest, ARecordDoesNotTransferToADifferentSheet) {
  ViewportProfile vp;
  browser_internal::ApplyValidationVerdict(vp, PassingVerdict(), kBlock,
                                           kSheet);
  // The CSS-only redeploy: same markup, same template hash, new stylesheet.
  EXPECT_FALSE(AsyncCssValidatedForServedSheet(&vp, kOtherSheet));
}

TEST(CriticalCssValidationRecordTest, RejectedBlockLeavesNothingAcceptable) {
  ViewportProfile vp;
  browser_internal::ApplyValidationVerdict(vp, FailingVerdict(), kBlock,
                                           kSheet);

  EXPECT_FALSE(AsyncCssValidatedForServedSheet(&vp, kSheet));
  // The measurement is still kept: "we looked and it was 31% off" is a
  // different thing to go and look at than "we never looked".
  EXPECT_FLOAT_EQ(vp.validation_diff_ratio, 0.31f);
  EXPECT_TRUE(vp.validated_combined_css_hash.empty());
  EXPECT_TRUE(vp.validated_critical_css_hash.empty());
}

TEST(CriticalCssValidationRecordTest,
     ARejectionOverwritesAnEarlierConfirmation) {
  // Re-analysis reuses the profile object per viewport; a later failing run
  // must not leave the earlier run's record standing.
  ViewportProfile vp;
  browser_internal::ApplyValidationVerdict(vp, PassingVerdict(), kBlock,
                                           kSheet);
  ASSERT_TRUE(AsyncCssValidatedForServedSheet(&vp, kSheet));

  browser_internal::ApplyValidationVerdict(vp, FailingVerdict(), kBlock,
                                           kSheet);
  EXPECT_FALSE(AsyncCssValidatedForServedSheet(&vp, kSheet));
}

TEST(CriticalCssValidationRecordTest,
     ConfirmationAgainstNoStylesheetIsNotARecord) {
  // "Validated against nothing" must not be recordable at all — not even as a
  // bit with an empty hash, which reads as a record written wrong.
  ViewportProfile vp;
  browser_internal::ApplyValidationVerdict(vp, PassingVerdict(), kBlock, "");

  EXPECT_FALSE(vp.critical_css_validated);
  EXPECT_TRUE(vp.validated_combined_css_hash.empty());
  EXPECT_FALSE(AsyncCssValidatedForServedSheet(&vp, ""));
}

TEST(CriticalCssValidationRecordTest, ConfirmationOfAnEmptyBlockIsNotARecord) {
  ViewportProfile vp;
  browser_internal::ApplyValidationVerdict(vp, PassingVerdict(), "", kSheet);
  EXPECT_FALSE(AsyncCssValidatedForServedSheet(&vp, kSheet));
}

TEST(CriticalCssValidationRecordTest, ARecordSurvivesTheProfileRoundTrip) {
  // The record is written by this process and read by whichever process serves
  // the page. Everything above is worthless if it does not survive the cache.
  OptimizationProfile profile;
  profile.template_hash_hex = "00000000deadbeef";
  profile.analyzed_url = "http://example.com/page";
  profile.desktop.critical_css = kBlock;
  browser_internal::ApplyValidationVerdict(profile.desktop, PassingVerdict(),
                                           kBlock, kSheet);

  auto restored = OptimizationProfile::FromJson(profile.ToJson());
  ASSERT_TRUE(restored.ok()) << restored.status().message();
  EXPECT_TRUE(AsyncCssValidatedForServedSheet(&restored->desktop, kSheet));
  EXPECT_FALSE(
      AsyncCssValidatedForServedSheet(&restored->desktop, kOtherSheet));
  // A viewport that was never validated reads back unvalidated.
  EXPECT_FALSE(AsyncCssValidatedForServedSheet(&restored->mobile, kSheet));
}

// Issue #1216. A record speaks for ONE block — the one the derivation produced
// and the validation rendered. When the derivation comes back empty for the
// page being served, the serve path inlines something else (the heuristic
// extractor's substitute, or nothing), and the record must not travel with it.
// Nothing about the record itself changes in that case — the bit is set, the
// sheet hash still matches — which is exactly why the accept test on its own
// cannot see the problem, and why the record has to be dropped before it gets
// there.
TEST(CriticalCssValidationRecordTest, ARecordDoesNotCoverASubstitutedBlock) {
  ViewportProfile vp;
  browser_internal::ApplyValidationVerdict(vp, PassingVerdict(), kBlock,
                                           kSheet);
  ASSERT_TRUE(AsyncCssValidatedForServedSheet(&vp, kSheet))
      << "precondition: the record is intact and still bound to this sheet";

  // Derivation produced the block: the record does cover what is inlined.
  EXPECT_TRUE(AsyncCssValidatedForServedSheet(
      AsyncCssRecordForDerivedBlock(&vp, kBlock), kSheet));

  // Derivation produced nothing: refused, on a record that still looks healthy.
  EXPECT_FALSE(AsyncCssValidatedForServedSheet(
      AsyncCssRecordForDerivedBlock(&vp, ""), kSheet));

  // No profile at all stays no profile.
  EXPECT_EQ(AsyncCssRecordForDerivedBlock(nullptr, kBlock), nullptr);
}

// ---- The plan: which viewports are worth two extra renders ----

TEST(CriticalCssValidationPlanTest, RunsWhenEverythingIsInPlace) {
  ViewportProfile vp;
  vp.css_coverage_ratio = 0.21f;
  auto plan =
      browser_internal::PlanCriticalCssValidation(vp, kSheet, kBlock,
                                                  /*chrome_available=*/true);
  EXPECT_TRUE(plan.run);
  EXPECT_EQ(plan.skip, browser_internal::ValidationSkip::kNone);
}

TEST(CriticalCssValidationPlanTest, NoBrowserMeansNoRecord) {
  ViewportProfile vp;
  vp.css_coverage_ratio = 0.21f;
  auto plan =
      browser_internal::PlanCriticalCssValidation(vp, kSheet, kBlock,
                                                  /*chrome_available=*/false);
  EXPECT_FALSE(plan.run);
  EXPECT_EQ(plan.skip, browser_internal::ValidationSkip::kChromeUnavailable);
}

TEST(CriticalCssValidationPlanTest, NoCombinedSheetMeansNoRecordIsPossible) {
  // A record bound to no stylesheet can never match at serve time, so the two
  // renders would be spent producing something unusable.
  ViewportProfile vp;
  vp.css_coverage_ratio = 0.21f;
  auto plan = browser_internal::PlanCriticalCssValidation(vp, "", kBlock, true);
  EXPECT_FALSE(plan.run);
  EXPECT_EQ(plan.skip, browser_internal::ValidationSkip::kNoCombinedStylesheet);
}

TEST(CriticalCssValidationPlanTest, NoCriticalBlockMeansNothingToValidate) {
  ViewportProfile vp;
  vp.css_coverage_ratio = 0.21f;
  auto plan = browser_internal::PlanCriticalCssValidation(vp, kSheet, "", true);
  EXPECT_FALSE(plan.run);
  EXPECT_EQ(plan.skip, browser_internal::ValidationSkip::kNoCriticalBlock);
}

TEST(CriticalCssValidationPlanTest, InlineBudgetSuppressionSkipsTheRenders) {
  // The budget already refuses to inline this profile, so there is no deferral
  // for a record to authorize. Six renders per template saved.
  ViewportProfile vp;
  vp.css_coverage_ratio = browser_internal::kInlineCriticalCssMaxCoverage;
  ASSERT_FALSE(
      browser_internal::ShouldInlineCriticalCss(vp.css_coverage_ratio));
  auto plan =
      browser_internal::PlanCriticalCssValidation(vp, kSheet, kBlock, true);
  EXPECT_FALSE(plan.run);
  EXPECT_EQ(plan.skip,
            browser_internal::ValidationSkip::kSuppressedByInlineBudget);
}

TEST(CriticalCssValidationPlanTest, CheapRefusalsAreAnsweredWithoutACandidate) {
  // PrepareCriticalCssValidation asks the plan with an EMPTY candidate first,
  // so it can refuse a viewport without paying ~580 ms of event-loop time to
  // derive a block nobody was going to render. That only works while every
  // cheaper reason is decided before the candidate is looked at.
  ViewportProfile budgeted;
  budgeted.css_coverage_ratio = browser_internal::kInlineCriticalCssMaxCoverage;
  EXPECT_EQ(
      browser_internal::PlanCriticalCssValidation(budgeted, kSheet, "",
                                                  /*chrome_available=*/true)
          .skip,
      browser_internal::ValidationSkip::kSuppressedByInlineBudget);

  ViewportProfile fine;
  fine.css_coverage_ratio = 0.21f;
  EXPECT_EQ(
      browser_internal::PlanCriticalCssValidation(fine, "", "", true).skip,
      browser_internal::ValidationSkip::kNoCombinedStylesheet);
  EXPECT_EQ(
      browser_internal::PlanCriticalCssValidation(fine, kSheet, "", false).skip,
      browser_internal::ValidationSkip::kChromeUnavailable);
  // Only when nothing cheaper applies does the empty candidate decide.
  EXPECT_EQ(
      browser_internal::PlanCriticalCssValidation(fine, kSheet, "", true).skip,
      browser_internal::ValidationSkip::kNoCriticalBlock);
}

TEST(CriticalCssValidationPlanTest, EverySkipReasonSaysSomething) {
  using browser_internal::ValidationSkip;
  using browser_internal::ValidationSkipMessage;
  for (ValidationSkip s :
       {ValidationSkip::kChromeUnavailable, ValidationSkip::kNoCriticalBlock,
        ValidationSkip::kNoCombinedStylesheet,
        ValidationSkip::kSuppressedByInlineBudget}) {
    EXPECT_STRNE(ValidationSkipMessage(s), "unknown");
    EXPECT_GT(std::string(ValidationSkipMessage(s)).size(), 10u);
  }
}

// ---- The combined-stylesheet seam ----

namespace {
constexpr char kOriginPage[] =
    "<html><head><link rel=stylesheet href=/s.css></head>"
    "<body><div class=hero>x</div></body></html>";
}  // namespace

TEST(ValidationInputsTest, TheSheetBuilderSeesTheOriginDocumentAndItsIdentity) {
  // A record binds to the bytes the SERVE path hashes, so the manager takes
  // them from that path's own assembly. Two things have to be right: the
  // builder is handed the scan of the page as the ORIGIN served it (the
  // coverage render's copy has the stylesheet inlined into it, so its scan
  // declares no stylesheets at all), and it is handed this item's cache
  // identity, since that is what the sheet is resolved against.
  std::string seen_url, seen_host, seen_scheme;
  size_t seen_stylesheets = 0;
  int calls = 0;

  auto inputs = browser_internal::BuildValidationInputs(
      [&](const HtmlScanResult& scan, const std::string& url,
          const std::string& hostname, const std::string& scheme,
          const std::string& /*site*/, bool /*host_vouched*/,
          bool /*from_module*/) {
        ++calls;
        seen_url = url;
        seen_host = hostname;
        seen_scheme = scheme;
        seen_stylesheets = scan.stylesheets.size();
        return browser_internal::CombinedCssBytes{".hero{display:flex}", false};
      },
      "http://example.com/p", "example.com", "https", kOriginPage);

  EXPECT_EQ(calls, 1);
  EXPECT_EQ(seen_url, "http://example.com/p");
  EXPECT_EQ(seen_host, "example.com");
  EXPECT_EQ(seen_scheme, "https");
  EXPECT_EQ(seen_stylesheets, 1u)
      << "the builder was handed a document that had already been rewritten";

  EXPECT_TRUE(inputs.ready);
  EXPECT_EQ(inputs.combined_css, ".hero{display:flex}");
  // The DOM the candidate block is matched against is the origin's.
  ASSERT_FALSE(inputs.elements.empty());
  bool saw_hero = false;
  for (const auto& e : inputs.elements) {
    for (const auto& c : e.classes) {
      if (c == "hero") saw_hero = true;
    }
  }
  EXPECT_TRUE(saw_hero);
}

// The element list the candidate block is derived against is the
// page a browser running scripts renders, so a <noscript> banner's classes do
// not pull its rules into the block, while the sheet is still assembled from
// the page as the origin served it (the loadCSS <noscript> twin counts).
TEST(ValidationInputsTest, NoscriptElementsDoNotReachTheDerivation) {
  constexpr char kPage[] =
      "<html><head><link rel=preload as=style href=/s.css "
      "onload=\"this.rel='stylesheet'\">"
      "<noscript><link rel=stylesheet href=/s.css></noscript></head><body>"
      "<noscript><div class=nojs-banner id=nojs>Enable JS</div></noscript>"
      "<div class=hero>x</div></body></html>";
  size_t seen_stylesheets = 0;
  auto inputs = browser_internal::BuildValidationInputs(
      [&](const HtmlScanResult& scan, const std::string&, const std::string&,
          const std::string&, const std::string& /*site*/,
          bool /*host_vouched*/, bool /*from_module*/) {
        seen_stylesheets = scan.stylesheets.size();
        return browser_internal::CombinedCssBytes{
            ".nojs-banner{height:300px}#nojs{color:red}.hero{display:flex}",
            false};
      },
      "http://example.com/p", "example.com", "https", kPage);
  ASSERT_TRUE(inputs.ready);
  EXPECT_EQ(seen_stylesheets, 1u) << "the loadCSS twin must still count";

  for (const auto& e : inputs.elements) {
    EXPECT_NE(e.tag_name, "noscript");
    EXPECT_NE(e.id, "nojs");
    for (const auto& c : e.classes) EXPECT_NE(c, "nojs-banner");
  }

  CriticalCssResult derived = DeriveDomMatchedCriticalCss(
      inputs.elements, inputs.combined_css, /*profile_critical_css=*/"",
      CapabilityMask::Viewport::kMobile, /*measured_above_fold_selectors=*/{},
      inputs.layer_order);
  EXPECT_NE(derived.critical_css.find(".hero"), std::string::npos)
      << derived.critical_css;
  EXPECT_EQ(derived.critical_css.find("nojs"), std::string::npos)
      << derived.critical_css;
}

// The CSS coverage render runs with script execution disabled,
// so it loads the page without its <noscript> elements, stylesheets inlined.
// The removal comes after the inliner, which finds a loadCSS sheet through its
// <noscript> twin. The renders that run scripts (page analysis, script
// coverage, the agent render) load the page with them: a scripting browser
// already reads them as raw text, and the removal would edit script source.
namespace {
std::optional<std::string> SheetLookup(std::string_view url) {
  if (url.ends_with("/s.css")) return std::string(".hero{color:red}");
  return std::nullopt;
}
}  // namespace

TEST(AnalysisDocumentsTest, OnlyTheCoverageDocumentLosesNoscript) {
  constexpr char kPage[] =
      "<html><head><title>T</title>"
      "<link rel=preload as=style href=/s.css onload=\"this.rel='stylesheet'\">"
      "<noscript><link rel=stylesheet href=/s.css>"
      "<style>.hero{display:none}</style></noscript></head><body>"
      "<noscript><div class=nojs-banner>Enable JS</div></noscript>"
      "<div class=hero>x</div></body></html>";
  CssInliningStats stats;
  browser_internal::AnalysisDocuments docs =
      browser_internal::BuildAnalysisDocuments(kPage, "http://example.com/p",
                                               "example.com", "https",
                                               SheetLookup, &stats);
  EXPECT_EQ(stats.stylesheets_cached, 1u);
  ASSERT_TRUE(docs.coverage_refusal.empty()) << docs.coverage_refusal;
  for (const std::string* doc : {&docs.document, &docs.coverage_document}) {
    EXPECT_NE(
        doc->find("<style data-pagespeed-inlined>.hero{color:red}</style>"),
        std::string::npos)
        << *doc;
    EXPECT_NE(doc->find("<base href="), std::string::npos) << *doc;
    EXPECT_NE(doc->find("<div class=hero>x</div>"), std::string::npos) << *doc;
  }
  const std::string& coverage = docs.coverage_document;
  EXPECT_EQ(coverage.find("noscript"), std::string::npos) << coverage;
  EXPECT_EQ(coverage.find("Enable JS"), std::string::npos) << coverage;
  EXPECT_EQ(coverage.find("display:none"), std::string::npos) << coverage;
  // The document the scripting renders load keeps them.
  EXPECT_NE(docs.document.find("Enable JS"), std::string::npos)
      << docs.document;
  EXPECT_NE(docs.document.find("display:none"), std::string::npos)
      << docs.document;
}

// The analysis documents resolve a stylesheet href against the
// page's <base href>, as the serve path's combined-stylesheet gather does.
// Under a cross-host base a root-relative href is the base host's sheet: the
// inliner asks the cache for it at that absolute URL (the manager's lookup
// keys the read by that URL's host) and inlines it into both documents.
// Before the fix it asked for the page host's "/css/site.css", the sheet was
// never inlined, and the coverage render ran unstyled.
TEST(AnalysisDocumentsTest, CrossHostBaseHrefSheetIsInlinedFromTheBaseHost) {
  constexpr char kPage[] =
      "<html><head><title>T</title>"
      "<base href=\"https://cdn.example.com/assets/\">"
      "<link rel=stylesheet href=/css/site.css></head><body>"
      "<div class=hero>x</div></body></html>";
  std::vector<std::string> looked_up;
  auto lookup = [&](std::string_view url) -> std::optional<std::string> {
    looked_up.emplace_back(url);
    if (url == "https://cdn.example.com/css/site.css") {
      return std::string(".hero{color:red}");
    }
    return std::nullopt;
  };
  CssInliningStats stats;
  browser_internal::AnalysisDocuments docs =
      browser_internal::BuildAnalysisDocuments(kPage, "https://example.com/p",
                                               "example.com", "https", lookup,
                                               &stats);
  EXPECT_EQ((std::vector<std::string>{"https://cdn.example.com/css/site.css"}),
            looked_up);
  EXPECT_EQ(stats.stylesheets_cached, 1u);
  ASSERT_TRUE(docs.coverage_refusal.empty()) << docs.coverage_refusal;
  for (const std::string* doc : {&docs.document, &docs.coverage_document}) {
    EXPECT_NE(
        doc->find("<style data-pagespeed-inlined>.hero{color:red}</style>"),
        std::string::npos)
        << *doc;
    // The author's <base> is kept, and no second one is injected.
    EXPECT_NE(doc->find("<base href=\"https://cdn.example.com/assets/\">"),
              std::string::npos)
        << *doc;
    EXPECT_EQ(doc->find("<base href=\"https://example.com"), std::string::npos)
        << *doc;
  }
}

TEST(AnalysisDocumentsTest, ScriptSourceIsNeverEdited) {
  // A "<noscript>" inside a script string is script source, not an element.
  constexpr char kPage[] =
      "<html><head><title>T</title></head><body>"
      "<p title=it's>Hi</p><script>// don't\n"
      "if (a > b) { el.innerHTML = \"<noscript>\"; }</script>"
      "<div class=hero>HERO</div></body></html>";
  browser_internal::AnalysisDocuments docs =
      browser_internal::BuildAnalysisDocuments(
          kPage, "http://example.com/p", "example.com", "https", SheetLookup);
  ASSERT_TRUE(docs.coverage_refusal.empty()) << docs.coverage_refusal;
  EXPECT_NE(docs.coverage_document.find("el.innerHTML = \"<noscript>\"; }"),
            std::string::npos)
      << docs.coverage_document;
  EXPECT_NE(docs.coverage_document.find("<div class=hero>HERO</div>"),
            std::string::npos)
      << docs.coverage_document;
}

TEST(AnalysisDocumentsTest, AnUntrustedRemovalRefusesTheCoverageRender) {
  // An unclosed <noscript> with markup after it: rendering what is left
  // would measure a page cut short. The coverage render is skipped; the
  // scripting renders still get the page.
  constexpr char kPage[] =
      "<html><head><title>T</title></head><body>"
      "<noscript data-a=\"x\"<div class=hero>HERO</div><p>AFTER</p>"
      "</body></html>";
  browser_internal::AnalysisDocuments docs =
      browser_internal::BuildAnalysisDocuments(
          kPage, "http://example.com/p", "example.com", "https", SheetLookup);
  EXPECT_TRUE(docs.coverage_document.empty()) << docs.coverage_document;
  EXPECT_NE(docs.coverage_refusal.find("<noscript>"), std::string::npos)
      << docs.coverage_refusal;
  EXPECT_NE(docs.document.find("HERO"), std::string::npos);
}

TEST(ValidationInputsTest, NoscriptRenderEffectTravelsWithTheSheet) {
  auto builder = [](const HtmlScanResult&, const std::string&,
                    const std::string&, const std::string&,
                    const std::string& /*site*/, bool /*host_vouched*/,
                    bool /*from_module*/) {
    return browser_internal::CombinedCssBytes{".hero{display:flex}", false};
  };
  auto banner = browser_internal::BuildValidationInputs(
      builder, "http://example.com/p", "example.com", "https",
      "<html><head><link rel=stylesheet href=/s.css></head><body>"
      "<noscript><div>Enable JS</div></noscript><div class=hero>x</div>"
      "</body></html>");
  ASSERT_TRUE(banner.ready);
  EXPECT_TRUE(banner.noscript_affects_render);
  auto pixel = browser_internal::BuildValidationInputs(
      builder, "http://example.com/p", "example.com", "https",
      "<html><head><link rel=stylesheet href=/s.css></head><body>"
      "<noscript><img height=1 width=1 style=\"display:none\" "
      "src=\"https://t.example/p.gif\"></noscript><div class=hero>x</div>"
      "</body></html>");
  ASSERT_TRUE(pixel.ready);
  EXPECT_FALSE(pixel.noscript_affects_render);
}

TEST(ValidationInputsTest, TheLayerOrderTravelsWithTheSheet) {
  // The validator places a layered candidate with the page's layer order from
  // the same assembly as the sheet, so the order must reach the
  // inputs unchanged.
  auto inputs = browser_internal::BuildValidationInputs(
      [](const HtmlScanResult&, const std::string&, const std::string&,
         const std::string&, const std::string& /*site*/, bool /*host_vouched*/,
         bool /*from_module*/) {
        browser_internal::CombinedCssBytes bytes;
        bytes.css = "@layer base{.hero{display:flex}}";
        bytes.layer_order.proven = true;
        bytes.layer_order.reason.clear();
        bytes.layer_order.names = {"base"};
        return bytes;
      },
      "http://example.com/p", "example.com", "https", kOriginPage);
  ASSERT_TRUE(inputs.ready);
  EXPECT_TRUE(inputs.layer_order.proven);
  EXPECT_EQ(inputs.layer_order.Statement(), "@layer base;");
}

TEST(ValidationInputsTest, AnUncachedStylesheetProducesNoInputs) {
  bool called = false;
  auto inputs = browser_internal::BuildValidationInputs(
      [&](const HtmlScanResult&, const std::string&, const std::string&,
          const std::string&, const std::string& /*site*/,
          bool /*host_vouched*/, bool /*from_module*/) {
        called = true;
        // Whatever bytes came back, one declared sheet is missing from them.
        // Same-origin, so it can still land: transient.
        return browser_internal::CombinedCssBytes{".partial{}", true, true};
      },
      "http://example.com/p", "example.com", "https", kOriginPage);

  EXPECT_TRUE(called);
  EXPECT_FALSE(inputs.ready);
  EXPECT_EQ(inputs.skip,
            browser_internal::ValidationInputSkip::kStylesheetNotCachedYet);
  EXPECT_TRUE(inputs.combined_css.empty())
      << "a partial sheet must not be carried forward as if it were the sheet";
}

TEST(ValidationInputsTest, ACrossOriginSheetIsAPermanentRefusal) {
  // Distinct from "not cached yet": nothing retries a sheet that can never
  // enter this cache, so the page is permanently unconfirmable. An operator
  // reading "will be retried" about this page would wait forever.
  auto inputs = browser_internal::BuildValidationInputs(
      [](const HtmlScanResult&, const std::string&, const std::string&,
         const std::string&, const std::string& /*site*/, bool /*host_vouched*/,
         bool /*from_module*/) {
        return browser_internal::CombinedCssBytes{".partial{}", true, false};
      },
      "http://example.com/p", "example.com", "https", kOriginPage);

  EXPECT_FALSE(inputs.ready);
  EXPECT_EQ(inputs.skip,
            browser_internal::ValidationInputSkip::kStylesheetNeverCacheable);
  EXPECT_NE(
      std::string(browser_internal::ValidationInputSkipMessage(
          browser_internal::ValidationInputSkip::kStylesheetNeverCacheable)),
      std::string(browser_internal::ValidationInputSkipMessage(
          browser_internal::ValidationInputSkip::kStylesheetNotCachedYet)))
      << "the two refusals must not read the same in the log";
}

TEST(ValidationInputsTest, AnEmptySheetProducesNoInputs) {
  auto inputs = browser_internal::BuildValidationInputs(
      [](const HtmlScanResult&, const std::string&, const std::string&,
         const std::string&, const std::string& /*site*/, bool /*host_vouched*/,
         bool /*from_module*/) {
        return browser_internal::CombinedCssBytes{"", false};
      },
      "http://example.com/p", "example.com", "https", kOriginPage);
  EXPECT_FALSE(inputs.ready);
  EXPECT_EQ(inputs.skip,
            browser_internal::ValidationInputSkip::kEmptyStylesheet);
}

TEST(ValidationInputsTest, NoWiredBuilderMeansNoInputsAndNoCrash) {
  // Browser analysis can run without a worker behind it; it must not invent a
  // stylesheet, and it must not fall over.
  auto inputs = browser_internal::BuildValidationInputs(
      browser_internal::CombinedCssBuilder{}, "http://example.com/p",
      "example.com", "https", kOriginPage);
  EXPECT_FALSE(inputs.ready);
  EXPECT_EQ(inputs.skip,
            browser_internal::ValidationInputSkip::kNoSheetBuilder);
}

TEST(ValidationInputsTest, EmptyMarkupNeverReachesTheBuilder) {
  bool called = false;
  auto inputs = browser_internal::BuildValidationInputs(
      [&](const HtmlScanResult&, const std::string&, const std::string&,
          const std::string&, const std::string& /*site*/,
          bool /*host_vouched*/, bool /*from_module*/) {
        called = true;
        return browser_internal::CombinedCssBytes{".a{}", false};
      },
      "http://example.com/p", "example.com", "https", "");
  EXPECT_FALSE(called);
  EXPECT_FALSE(inputs.ready);
  EXPECT_EQ(inputs.skip, browser_internal::ValidationInputSkip::kNoPageMarkup);
}

TEST(ValidationInputsTest, EverySkipReasonSaysSomething) {
  using browser_internal::ValidationInputSkip;
  using browser_internal::ValidationInputSkipMessage;
  for (ValidationInputSkip s :
       {ValidationInputSkip::kNoSheetBuilder,
        ValidationInputSkip::kNoPageMarkup, ValidationInputSkip::kUnscannable,
        ValidationInputSkip::kStylesheetNotCachedYet,
        ValidationInputSkip::kStylesheetNeverCacheable,
        ValidationInputSkip::kEmptyStylesheet}) {
    EXPECT_STRNE(ValidationInputSkipMessage(s), "unknown");
    EXPECT_GT(std::string(ValidationInputSkipMessage(s)).size(), 10u);
  }
}

// ---------------------------------------------------------------------------
// The glue: what actually gets validated, and what actually gets stamped.
//
// Everything above tests pure pieces. These drive the production caller
// (RunCriticalCssValidation -> OnCriticalCssValidationDone) with only the
// render injected, because that caller is where a false confirmation would be
// manufactured and none of the pure tests can see it.
// ---------------------------------------------------------------------------

namespace {

// A page whose DOM matches only part of the sheet, so the derived block is a
// PROPER subset of both the sheet and the browser's raw coverage blob. Without
// that, "validated the wrong block" is unobservable.
constexpr char kGluePage[] =
    "<html><head><link rel=stylesheet href=/s.css></head>"
    "<body><div class=hero>x</div></body></html>";
constexpr char kGlueSheet[] =
    "@layer base{.hero{display:flex}}\n"
    "@layer base{.nowhere-on-this-page{display:grid}}";
// Chrome's raw coverage blob: byte ranges out of the sheet, so it carries no
// @layer prelude and is a DIFFERENT string from both the sheet and the derived
// block. All three must differ, or binding a record to the wrong one is
// unobservable.
constexpr char kGlueCoverageBlob[] = ".hero{display:flex}";
static_assert(std::string_view(kGlueCoverageBlob) !=
                  std::string_view(kGlueSheet),
              "the coverage blob must differ from the sheet");

AnalysisQueue::Item GlueItem() {
  AnalysisQueue::Item item;
  item.url = "http://example.com/p";
  item.hostname = "example.com";
  item.scheme = "https";
  return item;
}

BrowserAnalysisManager::CombinedCssBuilder GlueSheetBuilder() {
  return [](const HtmlScanResult&, const std::string&, const std::string&,
            const std::string&, const std::string& /*site*/,
            bool /*host_vouched*/, bool /*from_module*/) {
    return browser_internal::CombinedCssBytes{kGlueSheet, false};
  };
}

// The layer-order binding of a page whose order the builder did not prove
// (GlueSheetBuilder sets none), which the manager's records bind to.
std::string UnprovenBinding() {
  return CascadeLayerOrder{}.ValidationBinding(kGlueSheet);
}

}  // namespace

TEST_F(BrowserAnalysisManagerCacheTest, ValidatesTheBlockTheServePathInlines) {
  mgr_->set_combined_css_builder(GlueSheetBuilder());

  ValidationDocuments seen;
  mgr_->set_validation_runner(
      [&seen](const ValidationDocuments& docs, uint32_t, uint32_t,
              std::function<void(ValidationVerdict)> done) {
        seen = docs;
        ValidationVerdict v;
        v.validated = true;
        v.diff_ratio = 0.0f;
        done(std::move(v));
      });

  ViewportProfile seed;
  seed.css_coverage_ratio = 0.21f;
  seed.critical_css = kGlueCoverageBlob;

  mgr_->TestValidateOneViewport(GlueItem(), kGluePage, seed,
                                /*viewport_index=*/2);

  ASSERT_TRUE(seen.ok) << seen.error;
  const std::string open = "<style data-pagespeed-critical>";
  size_t a = seen.candidate.find(open);
  ASSERT_NE(a, std::string::npos);
  a += open.size();
  size_t b = seen.candidate.find("</style>", a);
  ASSERT_NE(b, std::string::npos);
  std::string rendered_block = seen.candidate.substr(a, b - a);

  // It must be the DOM-matched derivation the serve path inlines, NOT the raw
  // coverage blob. Validating the blob would produce a record about bytes no
  // visitor ever receives.
  HtmlScanner scanner;
  HtmlScanResult scan = scanner.Scan("http://example.com/p", kGluePage);
  ASSERT_TRUE(scan.success);
  // Same fold the validation path derives against — see the ordering note at
  // PrepareCriticalCssValidation.
  std::string expected =
      DeriveDomMatchedCriticalCss(scan.elements, kGlueSheet, seed.critical_css,
                                  CapabilityMask::Viewport::kDesktop,
                                  /*measured_above_fold_selectors=*/{},
                                  CascadeLayerOrder{})
          .critical_css;
  EXPECT_EQ(rendered_block, expected);
  EXPECT_NE(rendered_block, std::string(kGlueCoverageBlob))
      << "the raw coverage blob was rendered instead of the derived block";
  EXPECT_NE(rendered_block, std::string(kGlueSheet))
      << "the whole sheet was rendered instead of the derived block";
}

// On a layered page the layer order decides which @media blocks
// the block keeps, so the validation must derive with the order the serve
// path derives with, or it renders a different block from the one served. A
// Tailwind v4-shaped page at the mobile viewport: with the order proven the
// block goes first behind the statement and drops the `lg:` block; without it
// the block goes before </head> and keeps it. Either way the rendered block
// is the serve path's derivation under the same order.
namespace {

constexpr char kLayeredPage[] =
    "<html><head><link rel=stylesheet href=/s.css></head>"
    "<body><nav class=\"hidden lg:block\">x</nav></body></html>";
constexpr char kLayeredSheet[] =
    "@layer base,utilities;\n"
    "@layer base{nav{margin:0}}\n"
    "@layer utilities{.hidden{display:none}"
    "@media (width>=64rem){.lg\\:block{display:block}}}\n"
    "@layer utilities{.nowhere-on-this-page{display:grid}}";

CascadeLayerOrder LayeredOrder(bool proven) {
  CascadeLayerOrder order;
  if (proven) {
    order.proven = true;
    order.reason.clear();
    order.names = {"base", "utilities"};
  }
  return order;
}

}  // namespace

TEST_F(BrowserAnalysisManagerCacheTest,
       ValidatesTheServedBlockUnderThePagesLayerOrder) {
  for (bool proven : {true, false}) {
    SCOPED_TRACE(proven ? "order proven" : "order not proven");
    const CascadeLayerOrder order = LayeredOrder(proven);
    mgr_->set_combined_css_builder(
        [order](const HtmlScanResult&, const std::string&, const std::string&,
                const std::string&, const std::string& /*site*/,
                bool /*host_vouched*/, bool /*from_module*/) {
          return browser_internal::CombinedCssBytes{kLayeredSheet, false, false,
                                                    order};
        });
    ValidationDocuments seen;
    mgr_->set_validation_runner(
        [&seen](const ValidationDocuments& docs, uint32_t, uint32_t,
                std::function<void(ValidationVerdict)> done) {
          seen = docs;
          ValidationVerdict v;
          v.validated = true;
          v.diff_ratio = 0.0f;
          done(std::move(v));
        });

    ViewportProfile seed;
    seed.css_coverage_ratio = 0.21f;
    seed.critical_css = ".hidden{display:none}";
    mgr_->TestValidateOneViewport(GlueItem(), kLayeredPage, seed,
                                  /*viewport_index=*/0);  // mobile, 375 px
    ASSERT_TRUE(seen.ok) << seen.error;

    // The serve path's block for this page and order (worker.cc passes
    // CombinedCssResult::layer_order), placed by the serve path's rule.
    HtmlScanner scanner;
    HtmlScanResult scan = scanner.Scan("http://example.com/p", kLayeredPage);
    ASSERT_TRUE(scan.success);
    const std::string served =
        DeriveDomMatchedCriticalCss(scan.elements, kLayeredSheet,
                                    seed.critical_css,
                                    CapabilityMask::Viewport::kMobile,
                                    /*measured_above_fold_selectors=*/{}, order)
            .critical_css;
    const CriticalCssLayerPlacement placement =
        DecideCriticalCssLayerPlacement(served, order);
    const std::string open = "<style data-pagespeed-critical>";
    std::string expected_style = open;
    expected_style.append(placement.prefix).append(served).append("</style>");
    const size_t at = seen.candidate.find(expected_style);
    EXPECT_NE(at, std::string::npos)
        << "the validated block must be the block the visitor is served";

    const size_t link_or_head = seen.candidate.find("</head>");
    if (proven) {
      EXPECT_FALSE(placement.keep_fallback);
      EXPECT_EQ(placement.prefix, "@layer base,utilities;");
      EXPECT_EQ(served.find("64rem"), std::string::npos)
          << "a proven page's mobile block drops the lg: block";
    } else {
      EXPECT_TRUE(placement.keep_fallback);
      EXPECT_NE(served.find("64rem"), std::string::npos)
          << "a page whose order is not proven keeps it";
      EXPECT_EQ(
          at + open.size() + served.size() + std::string("</style>").size(),
          link_or_head)
          << "the fallback block sits right before </head>";
    }
  }
}

// On a page whose layer order is not proven, the block goes
// after the sheets and carries no anonymous-layer rule. The validation renders
// the block the serve path derives under the same order, so it renders that
// too.
TEST_F(BrowserAnalysisManagerCacheTest,
       ValidatesTheServedBlockWithoutAnonymousLayersWhenUnproven) {
  static constexpr char kSheet[] =
      "@layer base{nav{margin:0}}\n"
      "@layer { .hidden{display:none} .hidden.open{display:block} }\n"
      ".nowhere-on-this-page{display:grid}";
  static constexpr char kPage[] =
      "<html><head><link rel=stylesheet href=/s.css></head>"
      "<body><nav class=\"hidden\">x</nav></body></html>";
  const CascadeLayerOrder unproven;
  mgr_->set_combined_css_builder(
      [unproven](const HtmlScanResult&, const std::string&, const std::string&,
                 const std::string&, const std::string& /*site*/,
                 bool /*host_vouched*/, bool /*from_module*/) {
        return browser_internal::CombinedCssBytes{kSheet, false, false,
                                                  unproven};
      });
  ValidationDocuments seen;
  mgr_->set_validation_runner(
      [&seen](const ValidationDocuments& docs, uint32_t, uint32_t,
              std::function<void(ValidationVerdict)> done) {
        seen = docs;
        ValidationVerdict v;
        v.validated = true;
        v.diff_ratio = 0.0f;
        done(std::move(v));
      });
  ViewportProfile seed;
  seed.css_coverage_ratio = 0.21f;
  seed.critical_css = "nav{margin:0}";
  ViewportProfile out = mgr_->TestValidateOneViewport(GlueItem(), kPage, seed,
                                                      /*viewport_index=*/0);
  ASSERT_TRUE(seen.ok) << seen.error;

  HtmlScanner scanner;
  HtmlScanResult scan = scanner.Scan("http://example.com/p", kPage);
  ASSERT_TRUE(scan.success);
  CriticalCssResult served = DeriveDomMatchedCriticalCss(
      scan.elements, kSheet, seed.critical_css,
      CapabilityMask::Viewport::kMobile,
      /*measured_above_fold_selectors=*/{}, unproven);
  EXPECT_TRUE(served.anonymous_layers_dropped);
  EXPECT_EQ(served.critical_css.find("display:none"), std::string::npos)
      << served.critical_css;
  EXPECT_NE(served.critical_css.find("margin:0"), std::string::npos)
      << served.critical_css;
  std::string expected_style = "<style data-pagespeed-critical>";
  expected_style.append(served.critical_css).append("</style>");
  EXPECT_NE(seen.candidate.find(expected_style), std::string::npos)
      << "the validated block must be the block the visitor is served";
  // The record binds to that derivation: the sheet's `u2`
  // marker and the derivation's own salt, exactly what the serve path's
  // accept test computes for the block it inlines.
  ASSERT_TRUE(out.critical_css_validated);
  EXPECT_EQ(unproven.ValidationBinding(kSheet), "unproven u2");
  EXPECT_TRUE(AsyncCssValidatedForServedSheet(
      &out, kSheet,
      ValidationBindingFor(unproven.ValidationBinding(kSheet),
                           scan.noscript_affects_render, kSheet,
                           CapabilityMask::Viewport::kMobile,
                           served.anonymous_layers_dropped)));
  EXPECT_FALSE(AsyncCssValidatedForServedSheet(
      &out, kSheet,
      ValidationBindingFor(unproven.ValidationBinding(kSheet),
                           scan.noscript_affects_render, kSheet,
                           CapabilityMask::Viewport::kMobile,
                           /*anonymous_layers_dropped=*/false)))
      << "a record made before the salt existed is made again";
}

// The same with the anonymous layer written as `@l\61yer {`,
// which the placement side already read as a possible layer (the order is
// refused for the escape) while the drop path matched the literal `@layer`
// only. The validated block is the served one, without the escaped rules, and
// the record carries the derivation's salt.
TEST_F(BrowserAnalysisManagerCacheTest,
       ValidatesTheServedBlockWithoutEscapedAnonymousLayers) {
  static constexpr char kSheet[] =
      "@layer base{nav{margin:0}}\n"
      "@media screen { @l\\61yer { .hidden{display:none} } }\n"
      "@l\\61yer { .hidden.open{display:block} }";
  static constexpr char kPage[] =
      "<html><head><link rel=stylesheet href=/s.css></head>"
      "<body><nav class=\"hidden\">x</nav></body></html>";
  // Computed as BuildCombinedCss computes it: refused for the escape.
  std::vector<LayerOrderSource> sources(1);
  sources[0].available = true;
  sources[0].css = kSheet;
  const CascadeLayerOrder order = ComputeCascadeLayerOrder(sources, {});
  ASSERT_FALSE(order.proven);
  ASSERT_NE(order.reason.find("escape"), std::string::npos) << order.reason;
  mgr_->set_combined_css_builder(
      [order](const HtmlScanResult&, const std::string&, const std::string&,
              const std::string&, const std::string& /*site*/,
              bool /*host_vouched*/, bool /*from_module*/) {
        return browser_internal::CombinedCssBytes{kSheet, false, false, order};
      });
  ValidationDocuments seen;
  mgr_->set_validation_runner(
      [&seen](const ValidationDocuments& docs, uint32_t, uint32_t,
              std::function<void(ValidationVerdict)> done) {
        seen = docs;
        ValidationVerdict v;
        v.validated = true;
        v.diff_ratio = 0.0f;
        done(std::move(v));
      });
  ViewportProfile seed;
  seed.css_coverage_ratio = 0.21f;
  seed.critical_css = "nav{margin:0}";
  ViewportProfile out = mgr_->TestValidateOneViewport(GlueItem(), kPage, seed,
                                                      /*viewport_index=*/0);
  ASSERT_TRUE(seen.ok) << seen.error;

  HtmlScanner scanner;
  HtmlScanResult scan = scanner.Scan("http://example.com/p", kPage);
  ASSERT_TRUE(scan.success);
  CriticalCssResult served =
      DeriveDomMatchedCriticalCss(scan.elements, kSheet, seed.critical_css,
                                  CapabilityMask::Viewport::kMobile,
                                  /*measured_above_fold_selectors=*/{}, order);
  EXPECT_TRUE(served.anonymous_layers_dropped);
  EXPECT_EQ(served.critical_css.find("61yer"), std::string::npos)
      << served.critical_css;
  EXPECT_EQ(served.critical_css.find("display:"), std::string::npos)
      << served.critical_css;
  EXPECT_NE(served.critical_css.find("margin:0"), std::string::npos)
      << served.critical_css;
  std::string expected_style = "<style data-pagespeed-critical>";
  expected_style.append(served.critical_css).append("</style>");
  EXPECT_NE(seen.candidate.find(expected_style), std::string::npos)
      << "the validated block must be the block the visitor is served";
  ASSERT_TRUE(out.critical_css_validated);
  EXPECT_EQ(order.ValidationBinding(kSheet), "unproven u2");
  EXPECT_TRUE(AsyncCssValidatedForServedSheet(
      &out, kSheet,
      ValidationBindingFor(
          order.ValidationBinding(kSheet), scan.noscript_affects_render, kSheet,
          CapabilityMask::Viewport::kMobile, served.anonymous_layers_dropped)));
  EXPECT_FALSE(AsyncCssValidatedForServedSheet(
      &out, kSheet,
      ValidationBindingFor(order.ValidationBinding(kSheet),
                           scan.noscript_affects_render, kSheet,
                           CapabilityMask::Viewport::kMobile,
                           /*anonymous_layers_dropped=*/false)));
}

TEST_F(BrowserAnalysisManagerCacheTest, AConfirmationBindsToTheServedSheet) {
  mgr_->set_combined_css_builder(GlueSheetBuilder());
  mgr_->set_validation_runner([](const ValidationDocuments&, uint32_t, uint32_t,
                                 std::function<void(ValidationVerdict)> done) {
    ValidationVerdict v;
    v.validated = true;
    v.diff_ratio = 0.0f;
    done(std::move(v));
  });

  ViewportProfile seed;
  seed.css_coverage_ratio = 0.21f;
  seed.critical_css = kGlueCoverageBlob;

  ViewportProfile out =
      mgr_->TestValidateOneViewport(GlueItem(), kGluePage, seed, 2);

  // Asserted through the SERVE path's own accept test, against the sheet the
  // builder produced. Bound to anything else — the critical block, say — and
  // this is false while every "a record was written" assertion stays true.
  EXPECT_TRUE(
      AsyncCssValidatedForServedSheet(&out, kGlueSheet, UnprovenBinding()));
  EXPECT_FALSE(AsyncCssValidatedForServedSheet(&out, "@layer base{.hero{}}"));
  // The record binds to the layer order the validation placed the block with
  // too: the same sheet served with another order does not
  // match, and neither does the unbound hash of the sheet alone.
  CascadeLayerOrder proven;
  proven.proven = true;
  proven.names = {"base"};
  EXPECT_FALSE(AsyncCssValidatedForServedSheet(
      &out, kGlueSheet, proven.ValidationBinding(kGlueSheet)));
  EXPECT_FALSE(AsyncCssValidatedForServedSheet(&out, kGlueSheet));
  EXPECT_NE(out.validated_critical_css_hash, out.validated_combined_css_hash);
  // Bound to the SHEET, not to either block-shaped thing in play here.
  EXPECT_EQ(out.validated_combined_css_hash,
            CombinedCssValidationHash(kGlueSheet, UnprovenBinding()));
  EXPECT_NE(out.validated_combined_css_hash,
            CombinedCssValidationHash(kGlueCoverageBlob));
}

// The pipeline ORDER — CSS extraction, then page analysis, then validation —
// is what makes the validated block equal to the served one, and an ordering is
// not something the type system can hold. This pins it: a viewport whose fold
// has not been measured must be REFUSED outright, not validated against the
// empty fold it would see.
//
// Without this the ordering is unpinned, and moving validation back ahead of
// page analysis leaves every test green while every confirmation becomes a
// confirmation of bytes no visitor receives.
TEST_F(BrowserAnalysisManagerCacheTest, ValidationRefusesAnUnmeasuredFold) {
  mgr_->set_combined_css_builder(GlueSheetBuilder());

  bool renderer_invoked = false;
  mgr_->set_validation_runner(
      [&renderer_invoked](const ValidationDocuments&, uint32_t, uint32_t,
                          std::function<void(ValidationVerdict)> done) {
        renderer_invoked = true;
        ValidationVerdict v;
        v.validated = true;
        v.diff_ratio = 0.0f;
        done(std::move(v));
      });

  ViewportProfile seed;
  seed.css_coverage_ratio = 0.21f;
  seed.critical_css = kGlueCoverageBlob;

  ViewportProfile out = mgr_->TestValidateOneViewport(
      GlueItem(), kGluePage, seed, /*viewport_index=*/2,
      /*fold_measured=*/false);

  EXPECT_FALSE(renderer_invoked)
      << "an unmeasured fold must be refused before anything is rendered";
  // No record of any kind — the serve path reads the absence as "keep the
  // stylesheet render-blocking", which is the benign outcome.
  EXPECT_FALSE(out.critical_css_validated);
  EXPECT_TRUE(out.validated_combined_css_hash.empty());
  EXPECT_FALSE(
      AsyncCssValidatedForServedSheet(&out, kGlueSheet, UnprovenBinding()));

  // Control: the SAME inputs with the fold measured do validate, so the refusal
  // above is the flag's doing and not some other precondition failing.
  bool control_invoked = false;
  mgr_->set_validation_runner(
      [&control_invoked](const ValidationDocuments&, uint32_t, uint32_t,
                         std::function<void(ValidationVerdict)> done) {
        control_invoked = true;
        ValidationVerdict v;
        v.validated = true;
        v.diff_ratio = 0.0f;
        done(std::move(v));
      });
  ViewportProfile measured = mgr_->TestValidateOneViewport(
      GlueItem(), kGluePage, seed, /*viewport_index=*/2,
      /*fold_measured=*/true);
  EXPECT_TRUE(control_invoked);
  EXPECT_TRUE(AsyncCssValidatedForServedSheet(&measured, kGlueSheet,
                                              UnprovenBinding()));
}

// The ordering itself, driven through the real continuation.
//
// The two tests above pin the CONSEQUENCE of getting the order wrong (an
// unmeasured fold is refused). This pins the order: page analysis is what runs
// after CSS extraction, and validation runs after that — so the fold is measured
// before the block that gets judged is derived. Reverse the two and the fold is
// empty at validation time, every viewport confirms a block the serve path will
// not produce, and without this test nothing notices.
TEST_F(BrowserAnalysisManagerCacheTest, PageAnalysisRunsBeforeValidation) {
  mgr_->set_combined_css_builder(GlueSheetBuilder());

  std::vector<std::string> steps;

  mgr_->set_page_analysis_runner(
      [&steps](uint32_t, uint32_t,
               std::function<void(absl::StatusOr<PageAnalysisResult>)> done) {
        steps.emplace_back("page_analysis");
        PageAnalysisResult result;
        ElementDescriptor d;
        d.tag = "div";
        d.classes = {"hero"};
        d.above_fold = true;
        result.elements.push_back(std::move(d));
        done(std::move(result));
      });

  mgr_->set_validation_runner(
      [&steps](const ValidationDocuments&, uint32_t, uint32_t,
               std::function<void(ValidationVerdict)> done) {
        steps.emplace_back("validation");
        ValidationVerdict v;
        v.validated = true;
        v.diff_ratio = 0.0f;
        done(std::move(v));
      });

  ViewportProfile seed;
  seed.css_coverage_ratio = 0.21f;
  seed.critical_css = kGlueCoverageBlob;

  ViewportProfile out = mgr_->TestRunViewportFromCssExtraction(
      GlueItem(), kGluePage, seed, /*viewport_index=*/2);

  ASSERT_EQ(steps, (std::vector<std::string>{"page_analysis", "validation"}))
      << "the fold must be measured before the block to be judged is derived";

  // And the measurement actually reached the profile before validation used it.
  EXPECT_NE(std::find(out.above_fold_selectors.begin(),
                      out.above_fold_selectors.end(), ".hero"),
            out.above_fold_selectors.end());
  EXPECT_TRUE(
      AsyncCssValidatedForServedSheet(&out, kGlueSheet, UnprovenBinding()))
      << "a viewport that ran in the right order still gets confirmed";
}

TEST_F(BrowserAnalysisManagerCacheTest, ASkippedViewportIsLeftUnstamped) {
  // The incident class in miniature: a refusal that stamps a record anyway is
  // indistinguishable, downstream, from a confirmation.
  mgr_->set_combined_css_builder(GlueSheetBuilder());
  bool runner_called = false;
  mgr_->set_validation_runner(
      [&runner_called](const ValidationDocuments&, uint32_t, uint32_t,
                       std::function<void(ValidationVerdict)> done) {
        runner_called = true;
        ValidationVerdict v;
        v.validated = true;
        done(std::move(v));
      });

  // Coverage at the inline budget cap: nothing will be inlined, so there is no
  // deferral for a record to authorize and no render is worth spending.
  ViewportProfile seed;
  seed.css_coverage_ratio = browser_internal::kInlineCriticalCssMaxCoverage;
  seed.critical_css = kGlueCoverageBlob;

  ViewportProfile out =
      mgr_->TestValidateOneViewport(GlueItem(), kGluePage, seed, 2);

  EXPECT_FALSE(runner_called) << "a skipped viewport still rendered";
  EXPECT_FALSE(out.critical_css_validated);
  EXPECT_TRUE(out.validated_combined_css_hash.empty());
  EXPECT_TRUE(out.validated_critical_css_hash.empty());
  EXPECT_FALSE(
      AsyncCssValidatedForServedSheet(&out, kGlueSheet, UnprovenBinding()));
}

TEST_F(BrowserAnalysisManagerCacheTest, AnEmptyBlockSkipsAndStampsNothing) {
  mgr_->set_combined_css_builder(GlueSheetBuilder());
  bool runner_called = false;
  mgr_->set_validation_runner(
      [&runner_called](const ValidationDocuments&, uint32_t, uint32_t,
                       std::function<void(ValidationVerdict)> done) {
        runner_called = true;
        ValidationVerdict v;
        v.validated = true;
        done(std::move(v));
      });

  ViewportProfile seed;
  seed.css_coverage_ratio = 0.21f;
  seed.critical_css = "";  // extraction produced nothing for this viewport

  ViewportProfile out =
      mgr_->TestValidateOneViewport(GlueItem(), kGluePage, seed, 2);

  EXPECT_FALSE(runner_called);
  EXPECT_FALSE(
      AsyncCssValidatedForServedSheet(&out, kGlueSheet, UnprovenBinding()));
}

TEST_F(BrowserAnalysisManagerCacheTest,
       ARefusedVerdictLeavesNoRecordEitherWay) {
  mgr_->set_combined_css_builder(GlueSheetBuilder());
  mgr_->set_validation_runner([](const ValidationDocuments&, uint32_t, uint32_t,
                                 std::function<void(ValidationVerdict)> done) {
    ValidationVerdict v;
    v.validated = false;
    v.diff_ratio = 0.31f;
    v.failure_reason = "fold changed";
    done(std::move(v));
  });

  ViewportProfile seed;
  seed.css_coverage_ratio = 0.21f;
  seed.critical_css = kGlueCoverageBlob;

  ViewportProfile out =
      mgr_->TestValidateOneViewport(GlueItem(), kGluePage, seed, 2);

  EXPECT_FALSE(
      AsyncCssValidatedForServedSheet(&out, kGlueSheet, UnprovenBinding()));
  EXPECT_FLOAT_EQ(out.validation_diff_ratio, 0.31f)
      << "the measurement is still worth keeping; the confirmation is not";
}

TEST_F(BrowserAnalysisManagerCacheTest, TheViewportRenderedIsTheOneStamped) {
  mgr_->set_combined_css_builder(GlueSheetBuilder());
  uint32_t seen_width = 0;
  uint32_t seen_height = 0;
  mgr_->set_validation_runner([&](const ValidationDocuments&, uint32_t w,
                                  uint32_t h,
                                  std::function<void(ValidationVerdict)> done) {
    seen_width = w;
    seen_height = h;
    ValidationVerdict v;
    v.validated = true;
    done(std::move(v));
  });

  ViewportProfile seed;
  seed.css_coverage_ratio = 0.21f;
  seed.critical_css = kGlueCoverageBlob;

  mgr_->TestValidateOneViewport(GlueItem(), kGluePage, seed,
                                /*viewport_index=*/0);
  EXPECT_EQ(seen_width, 375u);
  EXPECT_EQ(seen_height, 667u);
}

// ---------------------------------------------------------------------------
// D-1 regression. The validation and the serve path MUST derive the block they
// judge/inline from the same measured fold, and it is not enough for the served
// block to be a superset of the validated one.
//
// Counterexample this pins: `[data-promo]` overrides `.promo`, and the extractor
// cannot resolve a bare attribute selector to any collected element, so it can
// NEVER admit that override. Derive the validation candidate against an empty
// fold and it contains only `.btn` — which renders correctly, so the viewport is
// CONFIRMED and deferral is authorized. The serve path then derives against the
// real fold, picks up `.promo` (red) but still not `[data-promo]` (black), and
// the visitor sees red text flip to black when the deferred sheet lands. A
// superset of a block that covers the fold does NOT necessarily cover the fold.
TEST(PrepareCriticalCssValidationTest, DerivesAgainstTheServedMeasuredFold) {
  // Ordering matters: `[data-promo]` comes last so it wins the cascade.
  const std::string sheet =
      ".btn{color:#000}\n.promo{color:#f00}\n[data-promo]{color:#000}\n";

  // A long page: the anchor lands past the 300-body-element estimate, so only
  // a measured fold can admit it.
  std::string html = "<html><head></head><body>";
  for (int i = 0; i < 300; ++i) html += "<p>x</p>";
  html += "<a class=\"btn promo\" data-promo>Buy</a></body></html>";

  HtmlScanner scanner;
  HtmlScanResult scan = scanner.Scan("http://example.com/p", html);
  ASSERT_TRUE(scan.success);

  ViewportProfile vp;
  vp.critical_css = ".btn{color:#000}";  // the browser's raw coverage blob
  vp.css_coverage_ratio = 0.21f;         // under the inline budget
  vp.total_css_bytes = sheet.size();
  vp.above_fold_selectors = {".promo"};  // what the render measured

  browser_internal::ValidationRequest request =
      browser_internal::PrepareCriticalCssValidation(
          vp, scan.elements, sheet, html, CapabilityMask::Viewport::kDesktop,
          /*renderer_available=*/true);
  ASSERT_TRUE(request.run) << browser_internal::ValidationRefusalMessage(
      request);

  // The block the SERVE path derives for this page, from the same profile.
  const std::string served =
      DeriveDomMatchedCriticalCss(scan.elements, sheet, vp.critical_css,
                                  CapabilityMask::Viewport::kDesktop,
                                  vp.above_fold_selectors, CascadeLayerOrder{})
          .critical_css;
  // The block an EMPTY fold would derive — what the bug validated instead.
  const std::string unmeasured =
      DeriveDomMatchedCriticalCss(scan.elements, sheet, vp.critical_css,
                                  CapabilityMask::Viewport::kDesktop, {},
                                  CascadeLayerOrder{})
          .critical_css;

  ASSERT_NE(served, unmeasured)
      << "fixture is inert: the measured fold must change the derived block";
  EXPECT_NE(served.find("#f00"), std::string::npos)
      << "precondition: the served block picks up the measured .promo rule";
  EXPECT_EQ(served.find("[data-promo]"), std::string::npos)
      << "precondition: the override can never be admitted — that is the FOUC";

  EXPECT_EQ(request.candidate_critical_css, served)
      << "the validated block must be the block the visitor is served";
  EXPECT_NE(request.candidate_critical_css, unmeasured)
      << "validating the empty-fold block confirms bytes nobody receives";
}

// Measured fold: what one viewport's render contributes to the profile.
// ---------------------------------------------------------------------------

namespace {

bool HasToken(const std::vector<std::string>& v, std::string_view token) {
  return std::find(v.begin(), v.end(), token) != v.end();
}

int CountToken(const std::vector<std::string>& v, std::string_view token) {
  return static_cast<int>(std::count(v.begin(), v.end(), token));
}

}  // namespace

// Previously only `img` elements reached above_fold_selectors, so the fold was
// described by whichever visible elements happened to carry an image — on a
// utility-CSS page, usually none of them.
TEST(PopulateViewportFromPageAnalysisTest,
     PopulatesAboveFoldSelectorsFromAllElements) {
  PageAnalysisResult result;
  result.lcp.selector = "h1.title";
  result.lcp.url = "";
  result.elements = {
      {"header", "top", {"flex", "h-16"}, 44, true},
      {"div", "", {"items-center", "flex"}, 45, true},
      {"section", "comments", {"mt-96"}, 300, false},
  };
  result.images = {
      {"img#hero", "https://cdn.example.com/h.jpg", true, 800, 400, 1600, 800},
      {"img.thumb", "https://cdn.example.com/t.jpg", false, 80, 80, 160, 160},
  };

  ViewportProfile vp;
  browser_internal::PopulateViewportFromPageAnalysis(result, vp);

  EXPECT_EQ(vp.lcp_selector, "h1.title");

  // Every above-the-fold element contributes, image or not.
  EXPECT_TRUE(HasToken(vp.above_fold_selectors, "#top"));
  EXPECT_TRUE(HasToken(vp.above_fold_selectors, ".flex"));
  EXPECT_TRUE(HasToken(vp.above_fold_selectors, ".h-16"));
  EXPECT_TRUE(HasToken(vp.above_fold_selectors, ".items-center"));

  // Below-fold elements contribute nothing: the set is the FOLD, and admitting
  // below-fold classes would re-inline most of a utility stylesheet.
  EXPECT_FALSE(HasToken(vp.above_fold_selectors, ".mt-96"));
  EXPECT_FALSE(HasToken(vp.above_fold_selectors, "#comments"));

  // Deduped — 2000 descriptors on a utility-CSS page repeat the same handful
  // of classes, and the profile is cached.
  EXPECT_EQ(CountToken(vp.above_fold_selectors, ".flex"), 1);

  // No bare tag tokens. A `div` or `span` token would match essentially every
  // element in the document and make the whole page "above the fold". An
  // element only a tag rule describes falls back to the extractor's
  // first-N-elements estimate, exactly as before anything was measured.
  EXPECT_FALSE(HasToken(vp.above_fold_selectors, "div"));
  EXPECT_FALSE(HasToken(vp.above_fold_selectors, "header"));
  EXPECT_FALSE(HasToken(vp.above_fold_selectors, "section"));

  // The pre-existing image bookkeeping is untouched.
  EXPECT_TRUE(HasToken(vp.above_fold_selectors, "img#hero"));
  EXPECT_TRUE(HasToken(vp.below_fold_selectors, "img.thumb"));
  ASSERT_EQ(vp.image_dimensions.size(), 2u);
  EXPECT_EQ(vp.image_dimensions[0].natural_width, 1600u);
}

// The profile is cached and carries three viewports, so one render's token set
// is bounded. Truncation loses fold evidence, which under-includes — the safe
// direction, back toward today's heuristic.
TEST(PopulateViewportFromPageAnalysisTest, AboveFoldSelectorTokensAreCapped) {
  PageAnalysisResult result;
  const size_t over_cap = browser_internal::kMaxAboveFoldSelectorTokens + 50;
  for (size_t i = 0; i < over_cap; ++i) {
    ElementDescriptor d;
    d.tag = "div";
    d.classes = {"u" + std::to_string(i)};
    d.index = static_cast<uint32_t>(i);
    d.above_fold = true;
    result.elements.push_back(std::move(d));
  }

  ViewportProfile vp;
  browser_internal::PopulateViewportFromPageAnalysis(result, vp);

  EXPECT_EQ(vp.above_fold_selectors.size(),
            browser_internal::kMaxAboveFoldSelectorTokens);
  EXPECT_TRUE(HasToken(vp.above_fold_selectors, ".u0"))
      << "the kept tokens are the document-order prefix";
}

// The derivation the serve path inlines and the one the deferral check renders
// are the same function, so it is that function that has to carry the measured
// fold through. A signature that accepts it and drops it would leave every
// extractor-level test green while the feature does nothing in production.
TEST(DeriveDomMatchedCriticalCssTest, ForwardsTheMeasuredFoldToTheExtractor) {
  // A long page: the elements of interest sit past the 300-body-element
  // estimate.
  std::string html = "<html><head></head><body>";
  for (int i = 0; i < 300; ++i) html += "<p>x</p>";
  html +=
      "<div class=\"flex\">a</div>"
      "<div class=\"mt-96\">b</div></body></html>";

  HtmlScanner scanner;
  HtmlScanResult scan = scanner.Scan("http://example.com/p", html);
  ASSERT_TRUE(scan.success);

  const std::string sheet = ".flex{display:flex}.mt-96{margin:24rem}";

  // No measured fold: both classes are outside the estimate and drop out.
  CriticalCssResult unmeasured = DeriveDomMatchedCriticalCss(
      scan.elements, sheet, /*profile_critical_css=*/"",
      CapabilityMask::Viewport::kDesktop,
      /*measured_above_fold_selectors=*/{}, CascadeLayerOrder{});
  EXPECT_EQ(unmeasured.critical_css.find("display:flex"), std::string::npos)
      << "precondition: the estimate alone excludes these elements";

  CriticalCssResult measured = DeriveDomMatchedCriticalCss(
      scan.elements, sheet, /*profile_critical_css=*/"",
      CapabilityMask::Viewport::kDesktop,
      /*measured_above_fold_selectors=*/{".flex"}, CascadeLayerOrder{});
  EXPECT_NE(measured.critical_css.find("display:flex"), std::string::npos)
      << "the measured token must reach the extractor";
  EXPECT_EQ(measured.critical_css.find("24rem"), std::string::npos)
      << "and only the measured element is admitted, not the whole document";
}

// The descriptor list is bounded by count, not by bytes. 2000 elements with
// megabyte-long ids would otherwise be copied verbatim into a cached profile
// and pushed back through CDP, where an oversized message trips the client's
// size cap. Long tokens are dropped, not truncated: a half-token would match
// the wrong elements.
TEST(PopulateViewportFromPageAnalysisTest, OverlongTokensAreDropped) {
  const std::string long_id(browser_internal::kMaxAboveFoldSelectorTokenBytes,
                            'x');
  const std::string long_class(
      browser_internal::kMaxAboveFoldSelectorTokenBytes + 4, 'y');
  // One byte under the bound once the "." prefix is added — still admitted.
  const std::string ok_class(
      browser_internal::kMaxAboveFoldSelectorTokenBytes - 1, 'z');

  PageAnalysisResult result;
  ElementDescriptor d;
  d.tag = "div";
  d.id = long_id;  // "#" + 128 bytes == 129, over the bound
  d.classes = {long_class, ok_class, "flex"};
  d.above_fold = true;
  result.elements.push_back(std::move(d));

  ViewportProfile vp;
  browser_internal::PopulateViewportFromPageAnalysis(result, vp);

  EXPECT_FALSE(HasToken(vp.above_fold_selectors, "#" + long_id));
  EXPECT_FALSE(HasToken(vp.above_fold_selectors, "." + long_class));
  EXPECT_TRUE(HasToken(vp.above_fold_selectors, "." + ok_class))
      << "the bound is on the token, and a token at the bound is fine";
  EXPECT_TRUE(HasToken(vp.above_fold_selectors, ".flex"))
      << "one pathological token must not discard its element's good ones";
  for (const std::string& token : vp.above_fold_selectors) {
    EXPECT_LE(token.size(), browser_internal::kMaxAboveFoldSelectorTokenBytes);
  }
}

// A render that measured nothing must leave the profile carrying no fold
// tokens at all, which is what makes the extractor fall back cleanly to its
// unmeasured behaviour instead of matching against a half-populated set.
TEST(PopulateViewportFromPageAnalysisTest, NoElementsLeavesTheFoldUnmeasured) {
  PageAnalysisResult result;
  ViewportProfile vp;
  browser_internal::PopulateViewportFromPageAnalysis(result, vp);

  EXPECT_TRUE(vp.above_fold_selectors.empty());
}

}  // namespace
}  // namespace pagespeed
