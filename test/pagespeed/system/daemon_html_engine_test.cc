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
#include "openssl/sha.h"
#include "pagespeed/kernel/base/null_message_handler.h"
#include "pagespeed/kernel/base/null_mutex.h"
#include "pagespeed/kernel/base/string.h"
#include "pagespeed/kernel/base/string_writer.h"
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

// A URL purge the bounded map had to forget still fences everything stored
// at or before it, whatever its URL, and nothing newer.
TEST(DaemonHtmlPurgeOracleTest, AForgottenUrlPurgeStillFences) {
  MockTimer timer(new NullMutex, kNowMs);
  Source source;
  DaemonHtmlPurgeOracle oracle(&source, &timer);
  const uint32 before_s = static_cast<uint32>(kNowMs / 1000 - 100);
  oracle.NoteUrlPurge("https://a.test/first", kNowMs - 50000);
  for (size_t i = 0; i < DaemonHtmlPurgeOracle::kLocalUrlPurgesMax; ++i) {
    oracle.NoteUrlPurge(StrCat("https://a.test/", IntegerToString(i)),
                        kNowMs - 1000);
  }
  // "first" was the oldest and is forgotten: still fenced.
  EXPECT_TRUE(oracle.Purged("https://a.test/first", before_s));
  EXPECT_TRUE(oracle.Purged("https://a.test/never-purged", before_s));
  // Newer than the forgotten purge, and never purged: not fenced.
  EXPECT_FALSE(oracle.Purged("https://a.test/never-purged",
                             static_cast<uint32>(kNowMs / 1000 - 10)));
  EXPECT_TRUE(oracle.Purged("https://a.test/7",
                            static_cast<uint32>(kNowMs / 1000 - 10)));
}

// A URL purge time is never later than now.
TEST(DaemonHtmlPurgeOracleTest, AFutureUrlPurgeTimeIsClamped) {
  MockTimer timer(new NullMutex, kNowMs);
  Source source;
  DaemonHtmlPurgeOracle oracle(&source, &timer);
  oracle.NoteUrlPurge("https://a.test/", kNowMs + 3600 * 1000);
  EXPECT_TRUE(
      oracle.Purged("https://a.test/", static_cast<uint32>(kNowMs / 1000)));
  EXPECT_FALSE(
      oracle.Purged("https://a.test/", static_cast<uint32>(kNowMs / 1000 + 1)));
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

// The unsafe-method fence map is bounded: one fence more than it holds drops
// the oldest, which then has only the bound every other process has, and
// nothing else moves: no other key is fenced and the global purge time stays.
TEST(DaemonHtmlPurgeOracleTest, TheUnsafeFenceMapDropsItsOldestWhenFull) {
  MockTimer timer(new NullMutex, kNowMs);
  Source source;
  DaemonHtmlPurgeOracle oracle(&source, &timer);
  oracle.SetUnsafeRetentionCap(60);
  const uint32 before_s = static_cast<uint32>(kNowMs / 1000 - 10);
  const size_t max = DaemonHtmlPurgeOracle::kUnsafeFencesMax;
  for (size_t i = 0; i <= max; ++i) {
    oracle.NoteUnsafeInvalidation(StrCat("https://a.test/", IntegerToString(i)),
                                  kNowMs - 1000 + static_cast<int64>(i % 500));
  }
  EXPECT_EQ(max, oracle.unsafe_fences_size());
  EXPECT_FALSE(oracle.Purged("https://a.test/0", before_s));
  EXPECT_TRUE(oracle.Purged("https://a.test/1", before_s));
  EXPECT_TRUE(
      oracle.Purged(StrCat("https://a.test/", IntegerToString(max)), before_s));
  EXPECT_FALSE(oracle.Purged("https://a.test/never", before_s));
  EXPECT_LT(oracle.Observe(), 0);
  // At or before the fence: an entry stored in the second of the unsafe
  // request is fenced, the next second is not.
  oracle.NoteUnsafeInvalidation("https://a.test/now", kNowMs);
  EXPECT_TRUE(oracle.UnsafeFenced("https://a.test/now",
                                  static_cast<uint32>(kNowMs / 1000)));
  EXPECT_FALSE(oracle.UnsafeFenced("https://a.test/now",
                                   static_cast<uint32>(kNowMs / 1000 + 1)));
  // A fence time is never later than now.
  oracle.NoteUnsafeInvalidation("https://a.test/later", kNowMs + 3600 * 1000);
  EXPECT_FALSE(oracle.UnsafeFenced("https://a.test/later",
                                   static_cast<uint32>(kNowMs / 1000 + 1)));
}

// How long an unsafe fence lasts: twice the larger of the cap and 60 s; with
// no cap, until the bounded map drops it.
TEST(DaemonHtmlPurgeOracleTest, TheUnsafeFenceRetentionFollowsTheCap) {
  struct Case {
    uint32 cap_s;
    int64 retention_s;
  };
  const Case kCases[] = {{60, 120}, {30, 120}, {3600, 7200}};
  for (const Case& c : kCases) {
    SCOPED_TRACE(StrCat("cap ", IntegerToString(c.cap_s)));
    MockTimer timer(new NullMutex, kNowMs);
    Source source;
    DaemonHtmlPurgeOracle oracle(&source, &timer);
    oracle.SetUnsafeRetentionCap(c.cap_s);
    oracle.NoteUnsafeInvalidation("https://a.test/p", kNowMs);
    const uint32 stored_s = static_cast<uint32>(kNowMs / 1000 - 1);
    timer.AdvanceMs(c.retention_s * 1000 - 1);
    // Another fence's eviction pass leaves this one while it lasts.
    oracle.NoteUnsafeInvalidation("https://a.test/q", timer.NowMs());
    EXPECT_TRUE(oracle.UnsafeFenced("https://a.test/p", stored_s));
    timer.AdvanceMs(1);
    EXPECT_FALSE(oracle.UnsafeFenced("https://a.test/p", stored_s));
  }
}

// With no cap a fence is never dropped for its age: a stored page may stay
// fresh for longer than any fixed time.
TEST(DaemonHtmlPurgeOracleTest, WithoutACapAnUnsafeFenceIsNeverAged) {
  MockTimer timer(new NullMutex, kNowMs);
  Source source;
  DaemonHtmlPurgeOracle oracle(&source, &timer);
  oracle.SetUnsafeRetentionCap(0);
  oracle.NoteUnsafeInvalidation("https://a.test/p", kNowMs);
  const uint32 stored_s = static_cast<uint32>(kNowMs / 1000 - 1);
  timer.AdvanceMs(int64{400} * 24 * 3600 * 1000);
  oracle.NoteUnsafeInvalidation("https://a.test/q", timer.NowMs());
  EXPECT_TRUE(oracle.UnsafeFenced("https://a.test/p", stored_s));
  EXPECT_EQ(2u, oracle.unsafe_fences_size());
}

// A fence names the stored entry: every port spelling of its host, never
// another scheme or path.
TEST(DaemonHtmlPurgeOracleTest, AnUnsafeFenceCoversEveryPortSpelling) {
  MockTimer timer(new NullMutex, kNowMs);
  Source source;
  DaemonHtmlPurgeOracle oracle(&source, &timer);
  oracle.NoteUnsafeInvalidation("https://a.test:8443/p?x=1", kNowMs);
  const uint32 stored_s = static_cast<uint32>(kNowMs / 1000 - 1);
  EXPECT_TRUE(oracle.UnsafeFenced("https://a.test/p?x=1", stored_s));
  EXPECT_TRUE(oracle.UnsafeFenced("https://a.test:9/p?x=1", stored_s));
  EXPECT_FALSE(oracle.UnsafeFenced("http://a.test/p?x=1", stored_s));
  EXPECT_FALSE(oracle.UnsafeFenced("https://a.test/p", stored_s));
  EXPECT_FALSE(oracle.UnsafeFenced("https://b.test/p?x=1", stored_s));
  oracle.NoteUnsafeInvalidation("https://[::1]:8443/v6", kNowMs);
  EXPECT_TRUE(oracle.UnsafeFenced("https://[::1]/v6", stored_s));
}

// Location and Content-Location references, resolved as RFC 3986 section
// 5.2 says (its section 5.4 examples among them).
TEST(DaemonHtmlResolveReferenceTest, ResolvesEveryReferenceForm) {
  auto resolve = [](StringPiece ref) {
    return DaemonHtmlResolveReference("http", "a", "/b/c/d;p?q", ref);
  };
  EXPECT_EQ("http://a/b/c/g", resolve("g"));
  EXPECT_EQ("http://a/b/c/g", resolve("./g"));
  EXPECT_EQ("http://a/b/c/g/", resolve("g/"));
  EXPECT_EQ("http://a/g", resolve("/g"));
  EXPECT_EQ("http://g/", resolve("//g"));
  EXPECT_EQ("http://a/b/c/d;p?y", resolve("?y"));
  EXPECT_EQ("http://a/b/c/g?y", resolve("g?y"));
  EXPECT_EQ("http://a/b/c/d;p?q", resolve("#s"));
  EXPECT_EQ("http://a/b/c/d;p?q", resolve(""));
  EXPECT_EQ("http://a/b/c/", resolve("."));
  EXPECT_EQ("http://a/b/", resolve(".."));
  EXPECT_EQ("http://a/b/g", resolve("../g"));
  EXPECT_EQ("http://a/", resolve("../.."));
  EXPECT_EQ("http://a/g", resolve("../../../g"));
  EXPECT_EQ("http://a/g", resolve("/./g"));
  EXPECT_EQ("http://a/b/c/g.", resolve("g."));
  EXPECT_EQ("http://a/b/g", resolve("./../g"));
  EXPECT_EQ("http://a/b/c/y", resolve("g/../y"));
  EXPECT_EQ("https://h/x", resolve("HTTPS://h/x/../x"));
  EXPECT_EQ("", resolve("mailto:a@b"));
  EXPECT_EQ("", resolve("http:g"));
  EXPECT_EQ("http://a/b/c/g", resolve("  g  "));
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

TEST(DaemonHtmlAjaxMarkerTest, NamesTheMarkersTheServerPortsTreatAsAjax) {
  EXPECT_STREQ("X-Requested-With",
               DaemonHtmlAjaxMarker("X-Requested-With", "XMLHttpRequest"));
  EXPECT_STREQ("X-Requested-With",
               DaemonHtmlAjaxMarker("x-requested-with", " xmlhttprequest "));
  EXPECT_EQ(nullptr,
            DaemonHtmlAjaxMarker("X-Requested-With", "com.example.app"));
  EXPECT_EQ(nullptr, DaemonHtmlAjaxMarker("X-Requested-With", ""));
  // Repeated lines a server joined, and a list sent as one line.
  EXPECT_STREQ("X-Requested-With",
               DaemonHtmlAjaxMarker("X-Requested-With",
                                    "XMLHttpRequest, XMLHttpRequest"));
  EXPECT_STREQ("X-Requested-With",
               DaemonHtmlAjaxMarker("X-Requested-With", "foo, xmlhttprequest"));
  EXPECT_STREQ("X-Requested-With",
               DaemonHtmlAjaxMarker("X-Requested-With", "XMLHttpRequest,x"));
  EXPECT_EQ(nullptr,
            DaemonHtmlAjaxMarker("X-Requested-With", "com.example.app, other"));
  // Contained anywhere, as an origin matching the token anywhere reads it.
  EXPECT_STREQ("X-Requested-With",
               DaemonHtmlAjaxMarker("X-Requested-With", "XMLHttpRequestX"));
  EXPECT_STREQ("X-Requested-With",
               DaemonHtmlAjaxMarker("X-Requested-With", "my-xmlhttprequest"));
  EXPECT_EQ(nullptr, DaemonHtmlAjaxMarker("X-Requested-With", "XMLHttp"));
  EXPECT_EQ(nullptr, DaemonHtmlAjaxMarker("X-Requested-With", " , "));
  EXPECT_STREQ("X-MicrosoftAjax",
               DaemonHtmlAjaxMarker("X-MICROSOFTAJAX", "Delta=true"));
  EXPECT_EQ(nullptr, DaemonHtmlAjaxMarker("X-MicrosoftAjax", " "));
  EXPECT_STREQ("X-Prototype-Version",
               DaemonHtmlAjaxMarker("X-Prototype-Version", "1.7.3"));
  EXPECT_EQ(nullptr, DaemonHtmlAjaxMarker("X-Prototype-Version", ""));
  EXPECT_EQ(nullptr, DaemonHtmlAjaxMarker("Accept", "XMLHttpRequest"));
}

class InlinePort : public DaemonHtmlPort {
 public:
  bool ClearAndEmitAllowlisted(const DaemonHtmlServeResult&, bool) override {
    return true;
  }
  void MarkNoCompress() override {}
  void RemoveClientConditionals() override {}
  void RunBlocking(DaemonHtmlCommit commit) override { commit.Run(); }
  // A port that answers with the stored page, so the HTML lookup runs.
  bool ServesAsOrigin() const override { return true; }
};

// The injected-verdict library with store verdicts: each call of the store
// classifier takes the next action and reason (refuse, "mode" when none).
class StoreAbi : public InjectedVerdictDaemonAbi {
 public:
  explicit StoreAbi(const DaemonAbi* inner) : InjectedVerdictDaemonAbi(inner) {}

  void Queue(int action, const char* reason) {
    verdicts_.push_back({action, reason});
  }

  int HtmlStoreClassify(int mode, const char*, const char*,
                        uint32_t request_flags, int, const char* const*,
                        const char* const*, size_t, uint64_t, int, const char*,
                        size_t, uint64_t, uint32_t, int* out_reason,
                        uint32_t* out_lifetime_s, uint32_t* out_flags,
                        uint16_t* out_cc_flags, int* out_sidecar, char* detail,
                        size_t capacity,
                        size_t* out_detail_len) const override {
    store_modes.push_back(mode);
    store_request_flags.push_back(request_flags);
    std::pair<int, const char*> v = {kPsHtmlStoreActionRefuse, "mode"};
    if (!verdicts_.empty()) {
      v = verdicts_.front();
      verdicts_.erase(verdicts_.begin());
    }
    *out_reason = 0;
    for (size_t i = 0; i < std::size(kReasons); ++i) {
      if (GoogleString(kReasons[i]) == v.second) {
        *out_reason = static_cast<int>(i);
      }
    }
    *out_lifetime_s = 300;
    *out_flags = 0;
    *out_cc_flags = 0;
    *out_sidecar = 0;
    *out_detail_len = 0;
    if (capacity > 0) detail[0] = '\0';
    return v.first;
  }
  const char* HtmlStoreReasonName(int reason) const override {
    return reason >= 0 && static_cast<size_t>(reason) < std::size(kReasons)
               ? kReasons[reason]
               : nullptr;
  }

  // The mode and request flags of every store classification, in order.
  mutable std::vector<int> store_modes;
  mutable std::vector<uint32_t> store_request_flags;

 private:
  static constexpr const char* kReasons[] = {"none", "mode", "private"};
  mutable std::vector<std::pair<int, const char*>> verdicts_;
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

  std::unique_ptr<DaemonHtmlEngine> NewEngine(bool with_abi = true,
                                              bool with_cache = true) {
    DaemonHtmlEngine::Deps deps;
    deps.substrate.abi = with_abi ? &abi_ : nullptr;
    deps.substrate.cache = with_cache ? &volume_ : nullptr;
    deps.purge_source = &source_;
    deps.statistics = &stats_;
    deps.timer = &timer_;
    deps.handler = &handler_;
    deps.limiter = &limiter_;
    deps.budget = &budget_;
    // As a port's configuration load would (ApplyPolicySettings): with the
    // 1.15 set, the policy has a stamp unless a test says otherwise.
    DaemonHtmlEngine::Config config = config_;
    if (apply_stamp_ && with_abi && abi_.HasHtmlServe15Set() &&
        config.policy_stamp == 0) {
      config.policy_stamp = 0x5A5A;
    }
    return std::make_unique<DaemonHtmlEngine>(config, deps);
  }
  bool apply_stamp_ = true;

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
    f.capture_start_ms = kNowMs;
    return f;
  }

  // One captured 200 with the given origin lines and store-snapshot lines.
  void Capture(DaemonHtmlEngine* engine, DaemonHtmlPort* port,
               const DaemonHtmlHeaderLines& origin,
               const DaemonHtmlHeaderLines& at_store,
               const DaemonHtmlRequestFacts& facts) {
    std::unique_ptr<DaemonHtmlRecording> rec =
        engine->StartRecording(facts, port);
    ASSERT_NE(nullptr, rec);
    rec->set_status(200);
    rec->SetOriginHeaders(origin);
    rec->NoteStoreHeaders(at_store);
    rec->AddBody(kBody, sizeof(kBody) - 1);
    rec->Finish(false);
  }

  // Everything the handler was given, as one string.
  GoogleString Logged() {
    GoogleString out;
    StringWriter writer(&out);
    handler_.Dump(&writer);
    return out;
  }

  int CallsOf(const char* fn) const {
    int n = 0;
    for (const GoogleString& name : abi_.CallNames()) {
      if (name == fn) ++n;
    }
    return n;
  }

  // A request carrying only Cookie: _ga=GA1.2.3.
  DaemonHtmlRequestFacts AnalyticsCookieRequest() {
    DaemonHtmlRequestFacts facts = MarkdownRequest();
    facts.has_cookie = true;
    facts.cookie_lines = {"_ga=GA1.2.3"};
    return facts;
  }

  // Serves and captures one request carrying only Cookie: _ga=GA1.2.3, with
  // the library refusing the store, and checks HAS_COOKIE on the serve
  // request and on every store classification (`cookie_flag`), with
  // nothing written.
  void ExpectAnalyticsCookieFlag(DaemonHtmlEngine* engine, bool cookie_flag) {
    InlinePort port;
    const DaemonHtmlRequestFacts facts = AnalyticsCookieRequest();
    engine->Serve(facts, &port);
    ASSERT_EQ(1u, abi_.serve_requests().size());
    EXPECT_EQ(cookie_flag, (abi_.serve_requests()[0].request_flags &
                            kPsHtmlServeRequestHasCookie) != 0);
    abi_.Queue(kPsHtmlStoreActionRefuse, "private");
    abi_.Queue(kPsHtmlStoreActionRefuse, "private");
    Capture(engine, &port, kOrigin, kOrigin, facts);
    ASSERT_FALSE(abi_.store_request_flags.empty());
    for (uint32_t flags : abi_.store_request_flags) {
      EXPECT_EQ(cookie_flag, (flags & kStoreRequestHasCookieBit) != 0);
    }
    EXPECT_EQ(0, CallsOf("ps_cache_write_original_site"));
    EXPECT_EQ(0, CallsOf("ps_cache_write_headers_sidecar_site"));
  }

  // The store request's HAS_COOKIE bit (PS_HTML_STORE_REQUEST_HAS_COOKIE),
  // the engine's own kStoreRequestHasCookie; the binding names no constant
  // for it.
  static constexpr uint32_t kStoreRequestHasCookieBit = 4u;
  static constexpr char kBody[] = "<html><body>hello</body></html>";
  const DaemonHtmlHeaderLines kOrigin = {{"Content-Type", "text/html"},
                                         {"Cache-Control", "max-age=300"}};

  std::unique_ptr<ThreadSystem> thread_system_;
  MockTimer timer_;
  SimpleStats stats_;
  MockMessageHandler handler_{new NullMutex};
  DaemonHealNotifyLimiter limiter_;
  DaemonHtmlByteBudget budget_;
  Source source_;
  FakeDaemonVolume volume_;
  StoreAbi abi_;
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

// Holds every commit it is given, so a test can drop it unrun.
class DeferringPort : public InlinePort {
 public:
  void RunBlocking(DaemonHtmlCommit commit) override {
    held.push_back(std::move(commit));
  }
  std::vector<DaemonHtmlCommit> held;
};

TEST_F(DaemonHtmlEngineTest, ACommitDroppedUnrunReleasesItsBudget) {
  std::unique_ptr<DaemonHtmlEngine> engine = NewEngine();
  abi_.Queue(kPsHtmlStoreActionStore, "none");
  abi_.Queue(kPsHtmlStoreActionStore, "none");
  DeferringPort port;
  Capture(engine.get(), &port, kOrigin, kOrigin, MarkdownRequest());
  ASSERT_EQ(1u, port.held.size());
  // Held by the pending commit.
  EXPECT_EQ(static_cast<int64>(sizeof(kBody) - 1), budget_.in_use());
  port.held.clear();  // Dropped by a port that is shutting down.
  EXPECT_EQ(0, budget_.in_use());
  EXPECT_EQ(0, CallsOf("ps_cache_write_headers_sidecar_site"));
}

TEST_F(DaemonHtmlEngineTest, AnAjaxRequestIsNeitherStoredNorServed) {
  std::unique_ptr<DaemonHtmlEngine> engine = NewEngine();
  InlinePort port;
  DaemonHtmlRequestFacts facts = MarkdownRequest();
  facts.ajax_marker = "X-Requested-With";
  EXPECT_EQ(nullptr, engine->StartRecording(facts, &port));
  const DaemonHtmlServeResult result = engine->Serve(facts, &port);
  EXPECT_EQ(DaemonHtmlServeResult::kFallThrough, result.kind);
  EXPECT_EQ("request_header", result.reason);
  // Nothing read from the volume, nothing classified.
  EXPECT_EQ(0, CallsOf("ps_headers_sidecar_read"));
  EXPECT_EQ(0, CallsOf("ps_html_stored_facts_from_reads"));
  EXPECT_EQ(0, CallsOf("ps_html_serve_classify"));
  EXPECT_EQ(1, stats_.GetVariable("daemon_html_refused_request_header")->Get());
  EXPECT_EQ(
      1,
      stats_.GetVariable("daemon_markdown_fallthrough_request_header")->Get());
  EXPECT_EQ(0, budget_.in_use());
}

TEST_F(DaemonHtmlEngineTest, NoLibraryMeansNoRecordingAndNoCall) {
  std::unique_ptr<DaemonHtmlEngine> engine = NewEngine(/*with_abi=*/false);
  InlinePort port;
  EXPECT_EQ(nullptr, engine->StartRecording(MarkdownRequest(), &port));
  EXPECT_EQ(DaemonHtmlServeResult::kFallThrough,
            engine->Serve(MarkdownRequest(), &port).kind);
  engine->OnPurgePollTick();
  engine->OnStatisticsTick();
  bool ok = false;
  engine->OnAdminPurge("https://www.example.test/page", false, kNowMs,
                       [&ok](bool o, const GoogleString&) { ok = o; });
  EXPECT_TRUE(ok);
  EXPECT_TRUE(abi_.html_calls().empty());
}

TEST_F(DaemonHtmlEngineTest, NoCacheHandleStoresNothing) {
  std::unique_ptr<DaemonHtmlEngine> engine =
      NewEngine(/*with_abi=*/true, /*with_cache=*/false);
  abi_.Queue(kPsHtmlStoreActionStore, "none");
  abi_.Queue(kPsHtmlStoreActionStore, "none");
  InlinePort port;
  std::unique_ptr<DaemonHtmlRecording> rec =
      engine->StartRecording(MarkdownRequest(), &port);
  if (rec != nullptr) {
    rec->set_status(200);
    rec->SetOriginHeaders(kOrigin);
    rec->AddBody(kBody, sizeof(kBody) - 1);
    rec->Finish(false);
  }
  EXPECT_EQ(0, CallsOf("ps_cache_write_headers_sidecar_site"));
  EXPECT_EQ(0, CallsOf("ps_html_store_write_params"));
}

// A handle source is asked at each use: while it has no handle nothing is
// read, written or removed, and once it has one the engine uses it, without
// being created again.
TEST_F(DaemonHtmlEngineTest, AHandleSourceIsAskedAtEachUse) {
  void* handle = nullptr;
  int asked = 0;
  DaemonHtmlEngine::Deps deps;
  deps.substrate.abi = &abi_;
  deps.substrate.cache = &volume_;  // Ignored when a source is set.
  deps.substrate.cache_source = [&handle, &asked]() {
    ++asked;
    return handle;
  };
  deps.purge_source = &source_;
  deps.statistics = &stats_;
  deps.timer = &timer_;
  deps.handler = &handler_;
  deps.limiter = &limiter_;
  deps.budget = &budget_;
  DaemonHtmlEngine engine(config_, deps);
  InlinePort port;

  // No handle: the store and the serve make no volume call.
  abi_.Queue(kPsHtmlStoreActionStore, "none");
  abi_.Queue(kPsHtmlStoreActionStore, "none");
  std::unique_ptr<DaemonHtmlRecording> rec =
      engine.StartRecording(MarkdownRequest(), &port);
  if (rec != nullptr) {  // Report-only, then.
    rec->set_status(200);
    rec->SetOriginHeaders(kOrigin);
    rec->AddBody(kBody, sizeof(kBody) - 1);
    rec->Finish(false);
  }
  EXPECT_EQ(DaemonHtmlServeResult::kFallThrough,
            engine.Serve(MarkdownRequest(), &port).kind);
  EXPECT_TRUE(abi_.serve_facts().empty());
  EXPECT_EQ(0, CallsOf("ps_cache_write_headers_sidecar_site"));
  EXPECT_LT(0, asked);

  // A handle now: the next store writes through it.
  handle = &volume_;
  abi_.Queue(kPsHtmlStoreActionStore, "none");
  abi_.Queue(kPsHtmlStoreActionStore, "none");
  Capture(&engine, &port, kOrigin, kOrigin, MarkdownRequest());
  EXPECT_EQ(1, CallsOf("ps_cache_write_headers_sidecar_site"));
  engine.Serve(MarkdownRequest(), &port);
  EXPECT_EQ(1u, abi_.serve_facts().size());
}

// An admin URL purge fences this process at once: the purge set it reads
// purges from learns of the purge only at its next poll, and a commit whose
// capture predates the purge must not survive until then.
TEST_F(DaemonHtmlEngineTest, AnAdminUrlPurgeFencesThisProcessAtOnce) {
  std::unique_ptr<DaemonHtmlEngine> engine = NewEngine();
  class DeferringPort : public InlinePort {
   public:
    void RunBlocking(DaemonHtmlCommit commit) override {
      held = std::move(commit);
    }
    DaemonHtmlCommit held;
  } port;
  abi_.Queue(kPsHtmlStoreActionStore, "none");
  abi_.Queue(kPsHtmlStoreActionStore, "none");
  DaemonHtmlRequestFacts facts = MarkdownRequest();
  facts.capture_start_ms = kNowMs - 2000;
  Capture(engine.get(), &port, kOrigin, kOrigin, facts);
  ASSERT_FALSE(port.held.empty());
  bool ok = false;
  engine->OnAdminPurge("https://WWW.example.test:443/page", false, kNowMs,
                       [&ok](bool o, const GoogleString&) { ok = o; });
  EXPECT_TRUE(ok);
  // The source (the purge set) has not seen the purge yet.
  EXPECT_TRUE(source_.IsUrlValidIgnoringGlobal("https://www.example.test/page",
                                               kNowMs - 2000));
  port.held.Run();
  EXPECT_EQ(1, stats_.GetVariable("daemon_html_purge_race_refused")->Get());
  EXPECT_EQ(0, stats_.GetVariable("daemon_html_stored")->Get());
  // A capture after the purge stores.
  abi_.Queue(kPsHtmlStoreActionStore, "none");
  abi_.Queue(kPsHtmlStoreActionStore, "none");
  timer_.AdvanceMs(2000);
  facts.capture_start_ms = kNowMs + 2000;
  Capture(engine.get(), &port, kOrigin, kOrigin, facts);
  port.held.Run();
  EXPECT_EQ(1, stats_.GetVariable("daemon_html_stored")->Get());
}

TEST_F(DaemonHtmlEngineTest, TheInsertTimeTakesTheOriginsAge) {
  std::unique_ptr<DaemonHtmlEngine> engine = NewEngine();
  abi_.Queue(kPsHtmlStoreActionStore, "none");
  abi_.Queue(kPsHtmlStoreActionStore, "none");
  InlinePort port;
  DaemonHtmlHeaderLines origin = kOrigin;
  origin.emplace_back("Age", "20");
  // A server stage dropped the Age line before the store snapshot.
  Capture(engine.get(), &port, origin, kOrigin, MarkdownRequest());
  bool found = false;
  for (const InjectedHtmlCall& c : abi_.html_calls()) {
    if (c.fn != "ps_html_store_write_params") continue;
    found = true;
    EXPECT_NE(c.args.end(), std::find(c.args.begin(), c.args.end(), "age=20"));
  }
  EXPECT_TRUE(found);
}

TEST_F(DaemonHtmlEngineTest, ARequestWithoutASiteIsNeitherStoredNorServed) {
  std::unique_ptr<DaemonHtmlEngine> engine = NewEngine();
  InlinePort port;
  DaemonHtmlRequestFacts facts = MarkdownRequest();
  facts.site.clear();
  EXPECT_EQ(nullptr, engine->StartRecording(facts, &port));
  EXPECT_EQ(DaemonHtmlServeResult::kFallThrough,
            engine->Serve(facts, &port).kind);
  EXPECT_TRUE(abi_.serve_requests().empty());
}

TEST_F(DaemonHtmlEngineTest, ANullPortNeverStores) {
  std::unique_ptr<DaemonHtmlEngine> engine = NewEngine();
  ASSERT_TRUE(engine->serving());
  abi_.Queue(kPsHtmlStoreActionStore, "none");
  abi_.Queue(kPsHtmlStoreActionStore, "none");
  std::unique_ptr<DaemonHtmlRecording> rec =
      engine->StartRecording(MarkdownRequest(), /*port=*/nullptr);
  ASSERT_NE(nullptr, rec);  // Reported, as report-only would.
  rec->set_status(200);
  rec->SetOriginHeaders(kOrigin);
  rec->AddBody(kBody, sizeof(kBody) - 1);
  rec->Finish(false);
  EXPECT_EQ(0, budget_.in_use());
  EXPECT_EQ(0, CallsOf("ps_html_store_write_params"));
  EXPECT_EQ(0, CallsOf("ps_cache_write_headers_sidecar_site"));
  EXPECT_EQ(0, CallsOf("ps_cache_write_original_site"));
  EXPECT_EQ(0, stats_.GetVariable("daemon_html_serve_unavailable")->Get());
  EXPECT_EQ(1, stats_.GetVariable("daemon_html_evaluated")->Get());
}

TEST_F(DaemonHtmlEngineTest, AnOversizedBodyIsReportedRefusedForItsSize) {
  config_.max_bytes = 8;
  std::unique_ptr<DaemonHtmlEngine> engine = NewEngine();
  abi_.Queue(kPsHtmlStoreActionStore, "none");
  abi_.Queue(kPsHtmlStoreActionStore, "none");
  InlinePort port;
  Capture(engine.get(), &port, kOrigin, kOrigin, MarkdownRequest());
  EXPECT_EQ(0, CallsOf("ps_cache_write_original_site"));
  EXPECT_EQ(0, stats_.GetVariable("daemon_html_would_store")->Get());
  EXPECT_EQ(1, stats_.GetVariable("daemon_html_refused_size")->Get());
  EXPECT_EQ(0, budget_.in_use());
}

TEST_F(DaemonHtmlEngineTest, AnEntryStoredAgainIsRemovedOnTheNextRefusal) {
  // An entry this process removed, stored again by a peer process (the
  // injected removal leaves the volume as it is), then refused again.
  FakeVolumeEntry entry;
  entry.alternate_id = kPsSentinelOriginal;
  entry.body = kBody;
  entry.inserted_at = static_cast<uint32_t>(kNowMs / 1000 - 10);
  entry.max_age = 300;
  entry.origin_ct = "text/html";
  entry.has_hash = true;
  SHA256(reinterpret_cast<const uint8_t*>(kBody), sizeof(kBody) - 1,
         entry.hash);
  volume_
      .entries()[{FakeDaemonVolume::Key(kSite, "https://www.example.test/page"),
                  kPsSentinelOriginal}] = entry;
  abi_.mutable_injection()->sidecar_read_rc = kPsOk;  // An empty block.
  std::unique_ptr<DaemonHtmlEngine> engine = NewEngine();
  InlinePort port;
  DaemonHtmlRequestFacts facts = MarkdownRequest();
  facts.accept_lines.clear();

  abi_.Queue(kPsHtmlStoreActionStore, "none");
  abi_.Queue(kPsHtmlStoreActionRefuse, "private");
  Capture(engine.get(), &port, kOrigin, kOrigin, facts);
  EXPECT_EQ(1, CallsOf("ps_cache_remove_site"));

  // The same page, fresh and paired: the re-store is skipped.
  timer_.AdvanceMs(1000);
  abi_.Queue(kPsHtmlStoreActionStore, "none");
  abi_.Queue(kPsHtmlStoreActionStore, "none");
  Capture(engine.get(), &port, kOrigin, kOrigin, facts);
  EXPECT_EQ(0, CallsOf("ps_cache_write_headers_sidecar_site"));

  timer_.AdvanceMs(1000);
  abi_.Queue(kPsHtmlStoreActionStore, "none");
  abi_.Queue(kPsHtmlStoreActionRefuse, "private");
  Capture(engine.get(), &port, kOrigin, kOrigin, facts);
  EXPECT_EQ(2, CallsOf("ps_cache_remove_site"));
}

// One entry is shared by every port of a site: a store through one port
// clears what a refusal through another remembered, so the next refusal
// removes again.
TEST_F(DaemonHtmlEngineTest, TheRemovedSetIsKeyedWithoutThePort) {
  FakeVolumeEntry entry;
  entry.alternate_id = kPsSentinelOriginal;
  entry.body = kBody;
  entry.inserted_at = static_cast<uint32_t>(kNowMs / 1000 - 10);
  entry.max_age = 300;
  entry.origin_ct = "text/html";
  entry.has_hash = true;
  SHA256(reinterpret_cast<const uint8_t*>(kBody), sizeof(kBody) - 1,
         entry.hash);
  volume_
      .entries()[{FakeDaemonVolume::Key(kSite, "https://www.example.test/page"),
                  kPsSentinelOriginal}] = entry;
  abi_.mutable_injection()->sidecar_read_rc = kPsOk;
  std::unique_ptr<DaemonHtmlEngine> engine = NewEngine();
  InlinePort port;
  DaemonHtmlRequestFacts plain = MarkdownRequest();
  plain.accept_lines.clear();
  DaemonHtmlRequestFacts other_port = plain;
  other_port.host = "www.example.test:8443";

  abi_.Queue(kPsHtmlStoreActionStore, "none");
  abi_.Queue(kPsHtmlStoreActionRefuse, "private");
  Capture(engine.get(), &port, kOrigin, kOrigin, plain);
  EXPECT_EQ(1, CallsOf("ps_cache_remove_site"));

  timer_.AdvanceMs(1000);
  abi_.Queue(kPsHtmlStoreActionStore, "none");
  abi_.Queue(kPsHtmlStoreActionStore, "none");
  Capture(engine.get(), &port, kOrigin, kOrigin, other_port);

  timer_.AdvanceMs(1000);
  abi_.Queue(kPsHtmlStoreActionStore, "none");
  abi_.Queue(kPsHtmlStoreActionRefuse, "private");
  Capture(engine.get(), &port, kOrigin, kOrigin, plain);
  EXPECT_EQ(2, CallsOf("ps_cache_remove_site"));
}

// A path that does not start with "/" names no volume entry: nothing is
// read for it, and nothing is stored.
TEST_F(DaemonHtmlEngineTest, APathWithoutALeadingSlashNamesNoEntry) {
  std::unique_ptr<DaemonHtmlEngine> engine = NewEngine();
  InlinePort port;
  DaemonHtmlRequestFacts facts = MarkdownRequest();
  facts.path = "page";
  EXPECT_EQ(DaemonHtmlServeResult::kFallThrough,
            engine->Serve(facts, &port).kind);
  EXPECT_TRUE(abi_.serve_facts().empty());
  EXPECT_EQ(nullptr, engine->StartRecording(facts, &port));
}

// A URL purge is fenced even while no volume handle can be had.
TEST_F(DaemonHtmlEngineTest, AUrlPurgeWithoutAHandleIsStillFenced) {
  DaemonHtmlEngine::Deps deps;
  deps.substrate.abi = &abi_;
  deps.substrate.cache_source = []() -> void* { return nullptr; };
  deps.purge_source = &source_;
  deps.statistics = &stats_;
  deps.timer = &timer_;
  deps.handler = &handler_;
  deps.limiter = &limiter_;
  deps.budget = &budget_;
  DaemonHtmlEngine engine(config_, deps);
  bool ok = false;
  engine.OnAdminPurge("https://www.example.test/page", false, kNowMs,
                      [&ok](bool o, const GoogleString&) { ok = o; });
  EXPECT_TRUE(ok);
  EXPECT_TRUE(engine.oracle()->Purged("https://www.example.test/page",
                                      static_cast<uint32>(kNowMs / 1000)));
  EXPECT_FALSE(engine.oracle()->Purged("https://www.example.test/other",
                                       static_cast<uint32>(kNowMs / 1000)));
}

// A purge of a URL with no path purges the root: it is removed and fenced
// as "/".
TEST_F(DaemonHtmlEngineTest, APurgeWithoutAPathPurgesTheRoot) {
  std::unique_ptr<DaemonHtmlEngine> engine = NewEngine();
  bool ok = false;
  engine->OnAdminPurge("https://www.example.test", false, kNowMs,
                       [&ok](bool o, const GoogleString&) { ok = o; });
  EXPECT_TRUE(ok);
  EXPECT_EQ(1, CallsOf("ps_cache_remove_site"));
  EXPECT_TRUE(engine->oracle()->Purged("https://www.example.test/",
                                       static_cast<uint32>(kNowMs / 1000)));
}

// The epoch is written again to a volume that replaced the one it was
// written to.
TEST_F(DaemonHtmlEngineTest, AReplacedVolumeGetsTheEpochAgain) {
  source_.purge_set.UpdateGlobalInvalidationTimestampMs(kNowMs - 5000);
  FakeDaemonVolume replacement;
  void* handle = &volume_;
  DaemonHtmlEngine::Deps deps;
  deps.substrate.abi = &abi_;
  deps.substrate.cache_source = [&handle]() { return handle; };
  deps.purge_source = &source_;
  deps.statistics = &stats_;
  deps.timer = &timer_;
  deps.handler = &handler_;
  deps.limiter = &limiter_;
  deps.budget = &budget_;
  DaemonHtmlEngine engine(config_, deps);
  EXPECT_EQ(1, CallsOf("ps_cache_write_site_purge_epoch"));
  engine.OnPurgePollTick();
  EXPECT_EQ(1, CallsOf("ps_cache_write_site_purge_epoch"));
  handle = &replacement;
  engine.OnPurgePollTick();
  EXPECT_EQ(2, CallsOf("ps_cache_write_site_purge_epoch"));
}

// --- serve-cached-html and the 1.15 set -------------------------------

// The 1.15 fields of a sidecar write, by the struct size the engine stamped
// (the first argument the fake records).
GoogleString SidecarWriteSize(const InjectedVerdictDaemonAbi& abi) {
  for (const InjectedHtmlCall& c : abi.html_calls()) {
    if (c.fn == "ps_cache_write_headers_sidecar_site" && !c.args.empty()) {
      return c.args[0];
    }
  }
  return "";
}

// Without the 1.15 set, serve-cached-html runs as serve-variants: the library
// is sent mode 2 and every struct at its 1.14 size, each lookup and each HTML
// response counts daemon_html_serve_cached_html_unavailable, and one warning
// says why.
TEST_F(DaemonHtmlEngineTest, ServeCachedHtmlRunsAsServeVariantsBelowFifteen) {
  config_.mode = DaemonHtmlMode::kServeCachedHtml;
  abi_.mutable_injection()->unavailable_reason_15 =
      "the optimizer library reports C API 1.14";
  std::unique_ptr<DaemonHtmlEngine> engine = NewEngine();
  EXPECT_EQ(DaemonHtmlMode::kServeVariants, engine->effective_mode());
  EXPECT_TRUE(engine->serving());
  InlinePort port;
  DaemonHtmlRequestFacts facts = MarkdownRequest();
  facts.agent_class = 3;
  engine->Serve(facts, &port);
  ASSERT_EQ(1u, abi_.serve_requests().size());
  EXPECT_EQ(kPsHtmlCacheServeVariants, abi_.serve_requests()[0].mode);
  EXPECT_EQ(kPsHtmlServeRequestSize14, abi_.serve_requests()[0].struct_size);
  EXPECT_EQ(0u, abi_.serve_requests()[0].request_flags &
                    kPsHtmlServeRequestFreshOnly);
  ASSERT_EQ(1u, abi_.serve_facts().size());
  EXPECT_EQ(kPsHtmlStoredFactsSize14, abi_.serve_facts()[0].struct_size);

  abi_.Queue(kPsHtmlStoreActionStore, "none");
  abi_.Queue(kPsHtmlStoreActionStore, "none");
  Capture(engine.get(), &port, kOrigin, kOrigin, MarkdownRequest());
  // The lookup and the response.
  EXPECT_EQ(
      2,
      stats_.GetVariable("daemon_html_serve_cached_html_unavailable")->Get());
  ASSERT_FALSE(abi_.store_modes.empty());
  EXPECT_EQ(kPsHtmlCacheServeVariants, abi_.store_modes[0]);
  EXPECT_EQ(IntegerToString(static_cast<int>(kPsSidecarWriteParamsSize13)),
            SidecarWriteSize(abi_));
  EXPECT_EQ(1, handler_.MessagesOfType(kWarning));
  EXPECT_NE(GoogleString::npos,
            Logged().find("serve-cached-html is unavailable"));
  EXPECT_NE(GoogleString::npos, Logged().find("C API 1.14"));
  // Once per state: the next response counts, and logs nothing more.
  Capture(engine.get(), &port, kOrigin, kOrigin, MarkdownRequest());
  engine->OnPurgePollTick();
  EXPECT_EQ(
      3,
      stats_.GetVariable("daemon_html_serve_cached_html_unavailable")->Get());
  EXPECT_EQ(1, handler_.MessagesOfType(kWarning));

  // The set is read on every tick.  A real binding is fixed when the library
  // is loaded, so in a server only a restart changes it; the fake flips.
  abi_.mutable_injection()->has_set_15 = true;
  engine->OnPurgePollTick();
  EXPECT_EQ(DaemonHtmlMode::kServeCachedHtml, engine->effective_mode());
}

// With the 1.15 set, serve-cached-html sends mode 3 with the 1.15 request:
// the scope, the agent class, the cap, the stamp, always fresh-only, the
// conditional bit for a request that arrived with a validator, and the
// explicit-public bit; the structs at their 1.15 sizes.  A Markdown
// fall-through is classified once more for the page, with html_fallback.
TEST_F(DaemonHtmlEngineTest, ServeCachedHtmlSendsTheFifteenRequest) {
  config_.mode = DaemonHtmlMode::kServeCachedHtml;
  config_.serve_scope = kPsHtmlServeScopeAll;
  config_.max_age_s = 120;
  config_.credentialed = kPsHtmlCredentialedExplicitPublic;
  config_.policy_stamp = 0x77;
  abi_.mutable_injection()->has_set_15 = true;
  std::unique_ptr<DaemonHtmlEngine> engine = NewEngine();
  EXPECT_EQ(DaemonHtmlMode::kServeCachedHtml, engine->effective_mode());
  InlinePort port;
  DaemonHtmlRequestFacts facts = MarkdownRequest();
  facts.agent_class = 3;
  facts.has_if_modified_since = true;
  facts.if_modified_since = "Mon, 01 Jan 2024 00:00:00 GMT";
  engine->Serve(facts, &port);
  // The Markdown classification, then the page's.
  ASSERT_EQ(2u, abi_.serve_requests().size());
  EXPECT_NE(0u, abi_.serve_requests()[1].request_flags &
                    kPsHtmlServeRequestHtmlFallback);
  const RecordedServeRequest& r = abi_.serve_requests()[0];
  EXPECT_EQ(kPsHtmlCacheServeCachedHtml, r.mode);
  EXPECT_EQ(sizeof(PsHtmlServeRequest), r.struct_size);
  EXPECT_EQ(kPsHtmlServeRequestFreshOnly | kPsHtmlServeRequestConditional |
                kPsHtmlServeRequestExplicitPublic,
            r.request_flags &
                (kPsHtmlServeRequestFreshOnly | kPsHtmlServeRequestConditional |
                 kPsHtmlServeRequestExplicitPublic |
                 kPsHtmlServeRequestHtmlFallback));
  EXPECT_EQ(kPsHtmlServeScopeAll, r.serve_scope);
  EXPECT_EQ(3, r.agent_class);
  EXPECT_EQ(120u, r.html_max_lifetime_s);
  EXPECT_EQ(0x77u, r.policy_stamp);
  ASSERT_FALSE(abi_.serve_facts().empty());
  EXPECT_EQ(sizeof(PsHtmlStoredFacts), abi_.serve_facts()[0].struct_size);

  // No validator: no conditional bit; fresh-only stays.
  engine->Serve(MarkdownRequest(), &port);
  ASSERT_EQ(4u, abi_.serve_requests().size());
  EXPECT_EQ(0u, abi_.serve_requests()[2].request_flags &
                    kPsHtmlServeRequestConditional);
  EXPECT_NE(0u, abi_.serve_requests()[2].request_flags &
                    kPsHtmlServeRequestFreshOnly);

  // A verdict to serve the stored page on the Markdown classification (no
  // html_fallback) is a library defect: it falls through.
  abi_.mutable_injection()->verdict.action = kPsHtmlServeActionServeHtml;
  EXPECT_EQ(DaemonHtmlServeResult::kFallThrough,
            engine->Serve(MarkdownRequest(), &port).kind);

  // The store side: mode 3, the explicit-public bit, the 1.15 sidecar size.
  abi_.Queue(kPsHtmlStoreActionStore, "none");
  abi_.Queue(kPsHtmlStoreActionStore, "none");
  Capture(engine.get(), &port, kOrigin, kOrigin, MarkdownRequest());
  ASSERT_FALSE(abi_.store_modes.empty());
  EXPECT_EQ(kPsHtmlCacheServeCachedHtml, abi_.store_modes[0]);
  EXPECT_NE(0u,
            abi_.store_request_flags[0] & kPsHtmlStoreRequestExplicitPublic);
  EXPECT_EQ(IntegerToString(static_cast<int>(sizeof(PsSidecarWriteParams))),
            SidecarWriteSize(abi_));
  EXPECT_EQ(
      0,
      stats_.GetVariable("daemon_html_serve_cached_html_unavailable")->Get());
  EXPECT_EQ(0, handler_.MessagesOfType(kWarning));
}

// The split serve (a server that must not block its event thread): the
// reads may run on another thread between PrepareServe and FinishServe, and
// the purge fence and the clock are read when FinishServe runs, so a purge
// or the passing of time in between is seen.
TEST_F(DaemonHtmlEngineTest, FinishServeJudgesFenceAndFreshnessWhenItRuns) {
  config_.mode = DaemonHtmlMode::kServeCachedHtml;
  config_.serve_scope = kPsHtmlServeScopeAll;
  abi_.mutable_injection()->has_set_15 = true;
  abi_.mutable_injection()->wants_markdown = 0;
  std::unique_ptr<DaemonHtmlEngine> engine = NewEngine();
  InlinePort port;
  DaemonHtmlRequestFacts facts = MarkdownRequest();
  facts.accept_lines = {"text/html"};
  facts.agent_class = 3;
  DaemonHtmlEngine::PendingServe pending = engine->PrepareServe(facts, &port);
  ASSERT_TRUE(pending.needs_read());
  std::thread reader([&pending] { pending.Read(); });
  reader.join();
  EXPECT_FALSE(pending.needs_read());
  EXPECT_TRUE(abi_.serve_requests().empty());
  timer_.AdvanceMs(5000);
  engine->oracle()->NoteUrlPurge("https://www.example.test/page",
                                 timer_.NowMs());
  const DaemonHtmlServeResult result = engine->FinishServe(std::move(pending));
  EXPECT_EQ(DaemonHtmlServeResult::kFallThrough, result.kind);
  ASSERT_EQ(1u, abi_.serve_requests().size());
  EXPECT_EQ(static_cast<uint32_t>(kNowMs / 1000 + 5),
            abi_.serve_requests()[0].now_seconds);
  ASSERT_EQ(1u, abi_.serve_facts().size());
  EXPECT_NE(0u, abi_.serve_facts()[0].flags & kPsHtmlFactsPurged);

  // Dropped before it is finished (a client that went away): its reads are
  // freed, and nothing is classified.
  {
    DaemonHtmlEngine::PendingServe dropped = engine->PrepareServe(facts, &port);
    dropped.Read();
  }
  EXPECT_EQ(1u, abi_.serve_requests().size());

  // Decided at once: nothing to read.  Under scope agents an unknown agent
  // is pre-gated.
  config_.serve_scope = kPsHtmlServeScopeAgents;
  std::unique_ptr<DaemonHtmlEngine> agents = NewEngine();
  facts.agent_class = 0;
  DaemonHtmlEngine::PendingServe gated = agents->PrepareServe(facts, &port);
  EXPECT_FALSE(gated.needs_read());
  EXPECT_EQ("scope", agents->FinishServe(std::move(gated)).reason);
}

// serve-variants on a 1.15 library: mode 2 at the full size, never
// fresh-only, and the explicit-public bit only when configured.
TEST_F(DaemonHtmlEngineTest, ServeVariantsOnAFifteenLibrarySendsModeTwo) {
  abi_.mutable_injection()->has_set_15 = true;
  std::unique_ptr<DaemonHtmlEngine> engine = NewEngine();
  EXPECT_EQ(DaemonHtmlMode::kServeVariants, engine->effective_mode());
  InlinePort port;
  DaemonHtmlRequestFacts facts = MarkdownRequest();
  facts.has_if_none_match = true;
  facts.if_none_match = "\"x\"";
  engine->Serve(facts, &port);
  ASSERT_EQ(1u, abi_.serve_requests().size());
  const RecordedServeRequest& r = abi_.serve_requests()[0];
  EXPECT_EQ(kPsHtmlCacheServeVariants, r.mode);
  EXPECT_EQ(sizeof(PsHtmlServeRequest), r.struct_size);
  EXPECT_EQ(0u, r.request_flags & (kPsHtmlServeRequestFreshOnly |
                                   kPsHtmlServeRequestConditional |
                                   kPsHtmlServeRequestExplicitPublic));
  abi_.Queue(kPsHtmlStoreActionStore, "none");
  abi_.Queue(kPsHtmlStoreActionStore, "none");
  Capture(engine.get(), &port, kOrigin, kOrigin, MarkdownRequest());
  ASSERT_FALSE(abi_.store_modes.empty());
  EXPECT_EQ(kPsHtmlCacheServeVariants, abi_.store_modes[0]);
  EXPECT_EQ(0u,
            abi_.store_request_flags[0] & kPsHtmlStoreRequestExplicitPublic);
}

// serve-variants on a 1.14 library keeps the earlier cookie rule.  That is
// worth a warning only when the operator set a credentialed-policy setting
// that then has no effect.
TEST_F(DaemonHtmlEngineTest,
       ServeVariantsBelowFifteenWarnsOnlyForUnappliedPolicy) {
  {
    std::unique_ptr<DaemonHtmlEngine> engine = NewEngine();
    InlinePort port;
    Capture(engine.get(), &port, kOrigin, kOrigin, MarkdownRequest());
    engine->OnPurgePollTick();
    EXPECT_EQ(0, handler_.MessagesOfType(kWarning));
  }
  config_.credentialed = kPsHtmlCredentialedExplicitPublic;
  std::unique_ptr<DaemonHtmlEngine> engine = NewEngine();
  InlinePort port;
  Capture(engine.get(), &port, kOrigin, kOrigin, MarkdownRequest());
  engine->OnPurgePollTick();
  EXPECT_EQ(1, handler_.MessagesOfType(kWarning));
  EXPECT_NE(GoogleString::npos, Logged().find("DaemonHtmlCacheCredentialed"));
  // The 1.14 library is never told explicit-public.
  ASSERT_FALSE(abi_.store_request_flags.empty());
  for (uint32_t flags : abi_.store_request_flags) {
    EXPECT_EQ(0u, flags & kPsHtmlStoreRequestExplicitPublic);
  }
  EXPECT_EQ(
      0,
      stats_.GetVariable("daemon_html_serve_cached_html_unavailable")->Get());
}

// A request whose only cookie is an analytics one (Cookie: _ga=GA1.2.3).
// Below 1.15 the port's has_cookie reaches the library on both sides, as
// the earlier rule has it, and no cookie is classified.
TEST_F(DaemonHtmlEngineTest, AnAnalyticsCookieAloneStillCountsBelowFifteen) {
  config_.ignore_cookies = true;  // the default: the built-in list
  std::unique_ptr<DaemonHtmlEngine> engine = NewEngine();
  ASSERT_FALSE(abi_.HasHtmlServe15Set());
  EXPECT_EQ(DaemonHtmlMode::kServeVariants, engine->effective_mode());
  ExpectAnalyticsCookieFlag(engine.get(), /*cookie_flag=*/true);
  EXPECT_EQ(kPsHtmlServeRequestSize14, abi_.serve_requests()[0].struct_size);
  EXPECT_EQ(0, CallsOf("ps_html_classify_cookies"));
  EXPECT_EQ(0, CallsOf("ps_html_request_guard"));
}

// With 1.15, in serve-variants: the library sorts the Cookie lines, and an
// ignorable-only request is anonymous on both sides.
TEST_F(DaemonHtmlEngineTest, AnAnalyticsCookieAloneIsAnonymousOnFifteen) {
  abi_.mutable_injection()->has_set_15 = true;
  abi_.mutable_injection()->cookies.cookie_class = kPsHtmlCookiesIgnorableOnly;
  std::unique_ptr<DaemonHtmlEngine> engine = NewEngine();
  EXPECT_EQ(DaemonHtmlMode::kServeVariants, engine->effective_mode());
  ExpectAnalyticsCookieFlag(engine.get(), /*cookie_flag=*/false);
  EXPECT_EQ(sizeof(PsHtmlServeRequest), abi_.serve_requests()[0].struct_size);
  // Once for the serve, once for the capture, each with the Cookie line.
  EXPECT_EQ(2, CallsOf("ps_html_classify_cookies"));
  EXPECT_EQ(2, CallsOf("ps_html_request_guard"));
}

// With 1.15, a blocking cookie under anonymous-only: nothing is read,
// classified or stored, and the refusal is counted.
TEST_F(DaemonHtmlEngineTest, ABlockingCookieReadsAndStoresNothingOnFifteen) {
  abi_.mutable_injection()->has_set_15 = true;
  abi_.mutable_injection()->cookies.cookie_class = kPsHtmlCookiesBlocking;
  std::unique_ptr<DaemonHtmlEngine> engine = NewEngine();
  InlinePort port;
  const DaemonHtmlRequestFacts facts = AnalyticsCookieRequest();
  const DaemonHtmlServeResult served = engine->Serve(facts, &port);
  EXPECT_EQ(DaemonHtmlServeResult::kFallThrough, served.kind);
  EXPECT_EQ("cookie_request", served.reason);
  EXPECT_TRUE(abi_.serve_requests().empty());
  EXPECT_EQ(0, CallsOf("ps_cache_read_alternate_site"));
  EXPECT_EQ(nullptr, engine->StartRecording(facts, &port));
  EXPECT_TRUE(abi_.store_request_flags.empty());
  EXPECT_EQ(2, stats_.GetVariable("daemon_html_refused_cookie_request")->Get());
}

// A Cookie line the port could not show (a Cookie2 line only) is blocking.
TEST_F(DaemonHtmlEngineTest, ACookieWithoutLinesIsBlockingOnFifteen) {
  abi_.mutable_injection()->has_set_15 = true;
  std::unique_ptr<DaemonHtmlEngine> engine = NewEngine();
  InlinePort port;
  DaemonHtmlRequestFacts facts = MarkdownRequest();
  facts.has_cookie = true;
  EXPECT_EQ("cookie_request", engine->Serve(facts, &port).reason);
  EXPECT_EQ(0, CallsOf("ps_html_classify_cookies"));
}

// Either ignore-cookie setting away from its default needs 1.15: below it
// the setting has no effect, and the engine says so once.  With 1.15 it is
// applied and nothing is logged.
TEST_F(DaemonHtmlEngineTest, ANonDefaultIgnoreCookieSettingWarnsOnlyBelow15) {
  for (int which = 0; which < 2; ++which) {
    const int before = handler_.MessagesOfType(kWarning);
    const size_t mark = Logged().size();
    config_.ignore_cookies = which != 0;
    config_.ignore_cookie.clear();
    if (which != 0) config_.ignore_cookie.push_back("_my_ab");
    std::unique_ptr<DaemonHtmlEngine> engine = NewEngine();
    InlinePort port;
    Capture(engine.get(), &port, kOrigin, kOrigin, MarkdownRequest());
    Capture(engine.get(), &port, kOrigin, kOrigin, MarkdownRequest());
    engine->OnPurgePollTick();
    EXPECT_EQ(before + 1, handler_.MessagesOfType(kWarning)) << which;
    const GoogleString logged = Logged().substr(mark);
    EXPECT_NE(GoogleString::npos,
              logged.find("DaemonHtmlCacheIgnoreCookies and "
                          "DaemonHtmlCacheIgnoreCookie have no effect"))
        << which;
    EXPECT_NE(GoogleString::npos,
              logged.find("an analytics cookie included, counts as a cookie"))
        << which;
  }
  abi_.mutable_injection()->has_set_15 = true;
  const int before = handler_.MessagesOfType(kWarning);
  config_.ignore_cookies = false;
  config_.ignore_cookie = {"_my_ab"};
  std::unique_ptr<DaemonHtmlEngine> engine = NewEngine();
  InlinePort port;
  Capture(engine.get(), &port, kOrigin, kOrigin, MarkdownRequest());
  engine->OnPurgePollTick();
  EXPECT_EQ(before, handler_.MessagesOfType(kWarning));
}

// With the 1.15 set, an engine whose configuration has no policy stamp
// (its settings were never applied) stores and serves nothing.
TEST_F(DaemonHtmlEngineTest, NoPolicyStampStoresAndServesNothingOnFifteen) {
  abi_.mutable_injection()->has_set_15 = true;
  apply_stamp_ = false;
  std::unique_ptr<DaemonHtmlEngine> engine = NewEngine();
  EXPECT_FALSE(engine->serving());
  InlinePort port;
  engine->Serve(MarkdownRequest(), &port);
  EXPECT_TRUE(abi_.serve_requests().empty());
  abi_.Queue(kPsHtmlStoreActionStore, "none");
  abi_.Queue(kPsHtmlStoreActionStore, "none");
  Capture(engine.get(), &port, kOrigin, kOrigin, MarkdownRequest());
  EXPECT_EQ(0, CallsOf("ps_cache_write_original_site"));
  EXPECT_EQ(0, CallsOf("ps_cache_write_headers_sidecar_site"));
}

// Configuration load: each addition through the library's guard, the list
// kept as a sorted set, the stamp computed; a refused addition is named.
TEST_F(DaemonHtmlEngineTest, ApplyPolicySettingsGuardsAndStamps) {
  DaemonHtmlEngine::Config config;
  config.ignore_cookie = {"utm_src", "abtest_*", "utm_src"};
  GoogleString error;
  // Below 1.15: no effect, no call, no stamp.
  EXPECT_TRUE(DaemonHtmlEngine::ApplyPolicySettings(abi_, &config, &error));
  EXPECT_EQ(0u, config.policy_stamp);
  EXPECT_EQ(0, CallsOf("ps_html_ignore_cookie_guard"));
  EXPECT_EQ(0, CallsOf("ps_html_policy_stamp"));
  EXPECT_EQ((std::vector<GoogleString>{"abtest_*", "utm_src"}),
            config.ignore_cookie);

  abi_.mutable_injection()->has_set_15 = true;
  EXPECT_TRUE(DaemonHtmlEngine::ApplyPolicySettings(abi_, &config, &error))
      << error;
  EXPECT_EQ(0x5A5Au, config.policy_stamp);
  EXPECT_EQ(2, CallsOf("ps_html_ignore_cookie_guard"));
  EXPECT_EQ(1, CallsOf("ps_html_policy_stamp"));

  abi_.mutable_injection()->ignore_cookie_guard = 0;
  config.ignore_cookie = {"PHPSESSID"};
  EXPECT_FALSE(DaemonHtmlEngine::ApplyPolicySettings(abi_, &config, &error));
  EXPECT_NE(GoogleString::npos, error.find("PHPSESSID")) << error;

  // A stamp the library cannot compute fails closed too.
  abi_.mutable_injection()->ignore_cookie_guard = 1;
  abi_.mutable_injection()->policy_stamp_rc = -1;
  EXPECT_FALSE(DaemonHtmlEngine::ApplyPolicySettings(abi_, &config, &error));

  // The port's form drops the engine to report-only.
  config.mode = DaemonHtmlMode::kServeVariants;
  EXPECT_FALSE(ApplyDaemonHtmlPolicySettingsForPort(&abi_, &config, &handler_));
  EXPECT_EQ(DaemonHtmlMode::kReportOnly, config.mode);
  EXPECT_EQ(1, handler_.MessagesOfType(kError));
}

// The scheme nginx and IIS key a page on: the connection's, or with
// RespectXForwardedProto the first X-Forwarded-Proto line naming http or
// https, as the URL this module builds reads it.
TEST(DaemonHtmlKeyedSchemeTest, FollowsRespectXForwardedProto) {
  const DaemonHtmlHeaderLines https = {{"x-forwarded-proto", "HTTPS"}};
  EXPECT_EQ("http", DaemonHtmlKeyedScheme("http", https, false));
  EXPECT_EQ("https", DaemonHtmlKeyedScheme("http", https, true));
  EXPECT_EQ("http", DaemonHtmlKeyedScheme(
                        "https", {{"X-Forwarded-Proto", "http"}}, true));
  // Only the first line counts, and only http or https.
  EXPECT_EQ("http", DaemonHtmlKeyedScheme("http",
                                          {{"X-Forwarded-Proto", "ftp"},
                                           {"X-Forwarded-Proto", "https"}},
                                          true));
  EXPECT_EQ("http", DaemonHtmlKeyedScheme("http", {}, true));
}

TEST(DaemonHtmlModeTest, EveryOptionModeMapsAndOnlyLaterModesServe) {
  EXPECT_EQ(DaemonHtmlMode::kOff,
            DaemonHtmlModeFor(RewriteOptions::kDaemonHtmlCacheOff));
  EXPECT_EQ(DaemonHtmlMode::kReportOnly,
            DaemonHtmlModeFor(RewriteOptions::kDaemonHtmlCacheReportOnly));
  EXPECT_EQ(DaemonHtmlMode::kServeVariants,
            DaemonHtmlModeFor(RewriteOptions::kDaemonHtmlCacheServeVariants));
  EXPECT_EQ(DaemonHtmlMode::kServeCachedHtml,
            DaemonHtmlModeFor(RewriteOptions::kDaemonHtmlCacheServeCachedHtml));
  EXPECT_FALSE(DaemonHtmlCacheServes(RewriteOptions::kDaemonHtmlCacheOff));
  EXPECT_FALSE(
      DaemonHtmlCacheServes(RewriteOptions::kDaemonHtmlCacheReportOnly));
  EXPECT_TRUE(
      DaemonHtmlCacheServes(RewriteOptions::kDaemonHtmlCacheServeVariants));
  EXPECT_TRUE(
      DaemonHtmlCacheServes(RewriteOptions::kDaemonHtmlCacheServeCachedHtml));
}

TEST(DaemonHtmlModeTest, TheServeSettingsReachTheEngineConfig) {
  RewriteOptions::Initialize();
  {
    std::unique_ptr<ThreadSystem> threads(Platform::CreateThreadSystem());
    RewriteOptions options(threads.get());
    DaemonHtmlEngine::Config config;
    FillDaemonHtmlServeSettings(options, &config);
    EXPECT_EQ(kPsHtmlServeScopeAgents, config.serve_scope);
    EXPECT_EQ(3600u, config.max_age_s);
    EXPECT_EQ(kPsHtmlCredentialedAnonymousOnly, config.credentialed);
    EXPECT_TRUE(config.ignore_cookies);
    EXPECT_TRUE(config.ignore_cookie.empty());

    GoogleString msg;
    NullMessageHandler handler;
    for (const auto& [name, value] :
         std::vector<std::pair<const char*, const char*>>{
             {"DaemonHtmlCacheServeScope", "all"},
             {"DaemonHtmlCacheMaxAge", "0"},
             {"DaemonHtmlCacheCredentialed", "explicit-public"},
             {"DaemonHtmlCacheIgnoreCookies", "off"},
             {"DaemonHtmlCacheIgnoreCookie", "_my_ab"},
             {"DaemonHtmlCacheIgnoreCookie", "_my_pref*"}}) {
      ASSERT_EQ(RewriteOptions::kOptionOk,
                options.ParseAndSetOptionFromName1(name, value, &msg, &handler))
          << name;
    }
    FillDaemonHtmlServeSettings(options, &config);
    EXPECT_EQ(kPsHtmlServeScopeAll, config.serve_scope);
    EXPECT_EQ(0u, config.max_age_s);
    EXPECT_EQ(kPsHtmlCredentialedExplicitPublic, config.credentialed);
    EXPECT_FALSE(config.ignore_cookies);
    EXPECT_EQ(std::vector<GoogleString>({"_my_ab", "_my_pref*"}),
              config.ignore_cookie);
  }
  RewriteOptions::Terminate();
}

}  // namespace
}  // namespace net_instaweb
