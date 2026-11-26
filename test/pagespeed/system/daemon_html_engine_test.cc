// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

// Unit tests of the HTML cache engine's own pieces that the conformance rows
// do not reach one by one: the purge-key normalisation, the purge oracle's
// fail-closed epoch and its clamp, and the byte budget under contention.
// The rows themselves run in daemon_html_conformance_test.cc.

#include "pagespeed/system/daemon_html_engine.h"

#include <atomic>
#include <memory>
#include <thread>
#include <vector>

#include "net/instaweb/rewriter/public/rewrite_stats.h"
#include "pagespeed/kernel/base/null_mutex.h"
#include "pagespeed/kernel/base/string.h"
#include "pagespeed/kernel/base/thread_system.h"
#include "pagespeed/kernel/cache/purge_set.h"
#include "pagespeed/kernel/util/platform.h"
#include "pagespeed/kernel/util/simple_stats.h"
#include "pagespeed/system/daemon_abi.h"
#include "pagespeed/system/daemon_html_purge_observer.h"
#include "pagespeed/system/daemon_serve_arm.h"
#include "test/pagespeed/kernel/base/gtest.h"
#include "test/pagespeed/kernel/base/mock_message_handler.h"
#include "test/pagespeed/kernel/base/mock_timer.h"
#include "test/pagespeed/system/fake_daemon_volume.h"
#include "test/pagespeed/system/injected_verdict_daemon_abi.h"

namespace net_instaweb {
namespace {

const int64 kNowMs = 1790000000000LL;
const char kSite[] =
    "2007f4b98af3e43f6c81272c8606ea77a35978dd7a5c0ee623f15036edac5831";

class Source : public DaemonHtmlPurgeSource {
 public:
  bool IsUrlValidIgnoringGlobal(const GoogleString& url,
                                int64 time_ms) const override {
    return purge_set.IsValidIgnoringGlobal(url, time_ms);
  }
  int64 GlobalInvalidationTimestampMs() const override {
    return purge_set.global_invalidation_timestamp_ms();
  }
  bool FlushFileMtimeMs(int64* mtime_ms) const override {
    if (flush_ms < 0) return false;
    *mtime_ms = flush_ms;
    return true;
  }
  PurgeSet purge_set{1 << 16};
  int64 flush_ms = -1;
};

TEST(DaemonHtmlPurgeKeysTest, NormalisedFormOnlyWhenItDiffers) {
  EXPECT_EQ(std::vector<GoogleString>({"https://www.example.test/page"}),
            DaemonHtmlPurgeKeys("https://www.example.test/page"));
  EXPECT_EQ(std::vector<GoogleString>({"https://WWW.Example.Test:443/Page",
                                       "https://www.example.test/Page"}),
            DaemonHtmlPurgeKeys("https://WWW.Example.Test:443/Page"));
  EXPECT_EQ("http://example.test/a?Q=1",
            DaemonHtmlNormalizePurgeUrl("http://Example.TEST:80/a?Q=1"));
  // A non-default port stays; so does the other scheme's default.
  EXPECT_EQ("https://example.test:80/a",
            DaemonHtmlNormalizePurgeUrl("https://example.test:80/a"));
  EXPECT_EQ("http://example.test:8080",
            DaemonHtmlNormalizePurgeUrl("http://Example.test:8080"));
  // Not an http(s) URL: only itself.
  EXPECT_EQ(std::vector<GoogleString>({"/relative/Path"}),
            DaemonHtmlPurgeKeys("/relative/Path"));
}

TEST(DaemonHtmlPurgeOracleTest, AFailedEpochReadFencesTheWholeSite) {
  MockTimer timer(new NullMutex, kNowMs);
  Source source;
  DaemonHtmlPurgeOracle oracle(&source, &timer);
  EXPECT_FALSE(oracle.Purged("https://a.test/", 1790000000));
  oracle.SetSiteEpoch(false, 0);
  EXPECT_TRUE(oracle.Purged("https://a.test/", 1790000000));
  EXPECT_TRUE(oracle.Purged("https://b.test/", 4000000000u));
  oracle.SetSiteEpoch(true, 1789999990);
  EXPECT_FALSE(oracle.Purged("https://a.test/", 1790000000));
  EXPECT_TRUE(oracle.Purged("https://a.test/", 1789999990));
  // A later successful read never lowers the epoch.
  oracle.SetSiteEpoch(true, 10);
  EXPECT_TRUE(oracle.Purged("https://a.test/", 1789999990));
}

TEST(DaemonHtmlPurgeOracleTest, AFutureGlobalTimeIsClampedWhenFirstSeen) {
  MockTimer timer(new NullMutex, kNowMs);
  Source source;
  DaemonHtmlPurgeOracle oracle(&source, &timer);
  source.flush_ms = kNowMs + 3600 * 1000;
  EXPECT_EQ(kNowMs, oracle.Observe());
  timer.AdvanceMs(10 * 1000);
  // Seen again unchanged: still the moment it was first observed.
  EXPECT_EQ(kNowMs, oracle.Observe());
  EXPECT_FALSE(oracle.Purged("https://a.test/", 1790000010));
  EXPECT_TRUE(oracle.Purged("https://a.test/", 1790000000));
  // The same for the process's own global timestamp.
  ASSERT_TRUE(source.purge_set.UpdateGlobalInvalidationTimestampMs(
      kNowMs + 3600 * 1000));
  EXPECT_FALSE(oracle.Purged("https://a.test/", 1790000011));
  EXPECT_TRUE(oracle.Purged("https://a.test/", 1790000010));
}

TEST(DaemonHtmlPurgeOracleTest, UrlPurgesAreSecondGranular) {
  MockTimer timer(new NullMutex, kNowMs);
  Source source;
  DaemonHtmlPurgeOracle oracle(&source, &timer);
  ASSERT_TRUE(source.purge_set.Put("https://a.test/p", kNowMs + 999));
  // Stored in the same second as the purge: purged.
  EXPECT_TRUE(oracle.Purged("https://a.test/p", 1790000000));
  EXPECT_FALSE(oracle.Purged("https://a.test/p", 1790000001));
  EXPECT_FALSE(oracle.Purged("https://a.test/other", 1790000000));
}

TEST(DaemonHtmlByteBudgetTest, NeverExceedsItsCapacityUnderContention) {
  DaemonHtmlByteBudget budget(1000);
  std::atomic<int64> peak{0};
  std::vector<std::thread> threads;
  for (int t = 0; t < 8; ++t) {
    threads.emplace_back([&budget, &peak] {
      for (int i = 0; i < 20000; ++i) {
        if (budget.TryReserve(7)) {
          const int64 used = budget.in_use();
          int64 seen = peak.load();
          while (used > seen && !peak.compare_exchange_weak(seen, used)) {
          }
          budget.Release(7);
        }
      }
    });
  }
  for (std::thread& t : threads) t.join();
  EXPECT_EQ(0, budget.in_use());
  EXPECT_LE(peak.load(), 1000);
  EXPECT_FALSE(budget.TryReserve(1001));
  EXPECT_TRUE(budget.TryReserve(1000));
  EXPECT_FALSE(budget.TryReserve(1));
  budget.Release(1000);
}

class InlinePort : public DaemonHtmlPort {
 public:
  bool ClearAndEmitAllowlisted(const DaemonHtmlServeResult&, bool) override {
    return true;
  }
  void MarkNoCompress() override {}
  void RemoveClientConditionals() override {}
  void RunBlocking(DaemonHtmlCommit commit) override { commit.Run(); }
};

class DaemonHtmlEngineTest : public testing::Test {
 protected:
  DaemonHtmlEngineTest()
      : thread_system_(Platform::CreateThreadSystem()),
        timer_(new NullMutex, kNowMs),
        stats_(thread_system_.get()),
        limiter_(1),
        abi_(&volume_) {
    RewriteStats::InitStats(&stats_);
    config_.mode = DaemonHtmlMode::kServeVariants;
    config_.agent_optimize = true;
    config_.host_names.primary = "www.example.test";
    config_.site = kSite;
    abi_.mutable_injection()->wants_markdown = 1;
    abi_.mutable_injection()->facts.flags = kPsHtmlFactsPresent;
    abi_.mutable_injection()->facts.stored_at = 1789999990;
  }

  std::unique_ptr<DaemonHtmlEngine> NewEngine() {
    DaemonHtmlEngine::Deps deps;
    deps.substrate.abi = &abi_;
    deps.substrate.cache = &volume_;
    deps.purge_source = &source_;
    deps.statistics = &stats_;
    deps.timer = &timer_;
    deps.handler = &handler_;
    deps.limiter = &limiter_;
    deps.budget = &budget_;
    return std::make_unique<DaemonHtmlEngine>(config_, deps);
  }

  DaemonHtmlRequestFacts MarkdownRequest() {
    DaemonHtmlRequestFacts f;
    f.method = "GET";
    f.scheme = "https";
    f.host = "www.example.test";
    f.path = "/page";
    f.site = kSite;
    f.url_agreed = true;
    f.options_enabled_and_allowed = true;
    f.accept_lines = {"text/markdown"};
    return f;
  }

  std::unique_ptr<ThreadSystem> thread_system_;
  MockTimer timer_;
  SimpleStats stats_;
  MockMessageHandler handler_{new NullMutex};
  DaemonHealNotifyLimiter limiter_;
  DaemonHtmlByteBudget budget_;
  Source source_;
  FakeDaemonVolume volume_;
  InjectedVerdictDaemonAbi abi_;
  DaemonHtmlEngine::Config config_;
};

TEST_F(DaemonHtmlEngineTest, AnEpochReadFailureFencesEveryServe) {
  abi_.mutable_injection()->epoch_read_rc = 2;  // PS_ERR_IO
  std::unique_ptr<DaemonHtmlEngine> engine = NewEngine();
  InlinePort port;
  engine->Serve(MarkdownRequest(), &port);
  ASSERT_EQ(1u, abi_.serve_facts().size());
  EXPECT_NE(0u, abi_.serve_facts()[0].flags & kPsHtmlFactsPurged);

  // Read again on the statistics tick: a good read lifts the fence.
  abi_.mutable_injection()->epoch_read_rc = kPsOk;
  abi_.mutable_injection()->epoch = 0;
  engine->OnStatisticsTick();
  engine->Serve(MarkdownRequest(), &port);
  ASSERT_EQ(2u, abi_.serve_facts().size());
  EXPECT_EQ(0u, abi_.serve_facts()[1].flags & kPsHtmlFactsPurged);

  // And a failing one puts it back.
  abi_.mutable_injection()->epoch_read_rc = 3;  // PS_ERR_CORRUPTED
  engine->OnStatisticsTick();
  engine->Serve(MarkdownRequest(), &port);
  ASSERT_EQ(3u, abi_.serve_facts().size());
  EXPECT_NE(0u, abi_.serve_facts()[2].flags & kPsHtmlFactsPurged);
}

TEST_F(DaemonHtmlEngineTest, AnOlderLibraryMakesNoVolumeCallAtAll) {
  abi_.mutable_injection()->has_set = false;
  abi_.mutable_injection()->unavailable_reason = "C API 1.12";
  std::unique_ptr<DaemonHtmlEngine> engine = NewEngine();
  InlinePort port;
  EXPECT_FALSE(engine->serving());
  EXPECT_EQ(DaemonHtmlServeResult::kFallThrough,
            engine->Serve(MarkdownRequest(), &port).kind);
  engine->OnPurgePollTick();
  engine->OnStatisticsTick();
  bool done = false;
  bool ok = false;
  engine->OnAdminPurge("https://www.example.test/*", true, kNowMs,
                       [&](bool o, const GoogleString&) {
                         done = true;
                         ok = o;
                       });
  EXPECT_TRUE(done);
  EXPECT_TRUE(ok);
  EXPECT_TRUE(abi_.html_calls().empty()) << abi_.CallNames().size();
  EXPECT_EQ(1, handler_.MessagesOfType(kWarning));
}

TEST_F(DaemonHtmlEngineTest, TheCommitRunsOnThePortAndReleasesTheBudget) {
  std::unique_ptr<DaemonHtmlEngine> engine = NewEngine();
  class DeferringPort : public InlinePort {
   public:
    void RunBlocking(DaemonHtmlCommit commit) override {
      held = std::move(commit);
    }
    DaemonHtmlCommit held;
  } port;
  DaemonHtmlRequestFacts facts = MarkdownRequest();
  facts.capture_start_ms = kNowMs;
  std::unique_ptr<DaemonHtmlRecording> rec =
      engine->StartRecording(facts, &port);
  ASSERT_NE(nullptr, rec);
  rec->set_status(200);
  rec->SetOriginHeaders({{"Content-Type", "text/html"}});
  rec->AddBody("<html>hello</html>", 18);
  EXPECT_EQ(18, budget_.in_use());
  rec->Finish(false);
  rec.reset();
  // The library refuses by default here (no store verdict is bound in the
  // plain injected fake), so nothing is held; a stored response would hold
  // its bytes until the commit ran.
  EXPECT_EQ(0, budget_.in_use());
  EXPECT_TRUE(port.held.empty());
}

}  // namespace
}  // namespace net_instaweb
