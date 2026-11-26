// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

// DaemonHtmlCache report-only through the HTML cache engine: what the engine
// counts and logs for a recording it may not store, and the vhost_added
// field of its decision line.

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

#include "gtest/gtest.h"
#include "net/instaweb/rewriter/public/rewrite_options.h"
#include "net/instaweb/rewriter/public/rewrite_stats.h"
#include "pagespeed/kernel/base/google_message_handler.h"
#include "pagespeed/kernel/base/null_mutex.h"
#include "pagespeed/kernel/base/thread_system.h"
#include "pagespeed/kernel/util/platform.h"
#include "pagespeed/kernel/util/simple_stats.h"
#include "pagespeed/system/daemon_abi.h"
#include "pagespeed/system/daemon_html_decision_log.h"
#include "pagespeed/system/daemon_html_engine.h"
#include "pagespeed/system/rule_pack_decision_log.h"
#include "test/pagespeed/kernel/base/mock_message_handler.h"
#include "test/pagespeed/kernel/base/mock_timer.h"
#include "test/pagespeed/system/daemon_stub_test_util.h"
#include "test/pagespeed/system/recording_daemon_abi.h"

namespace net_instaweb {
namespace {

// The stub library, with every call of the store classifier recorded: the
// exact header view and body facts the engine hands to the policy.
class ClassifyRecordingAbi : public RecordingDaemonAbi {
 public:
  struct Call {
    int mode = 0;
    DaemonHtmlHeaderLines headers;
    uint64_t body_bytes = 0;
    int body_complete = -1;
    size_t prefix_len = 0;
  };

  explicit ClassifyRecordingAbi(const DaemonAbi* stub)
      : RecordingDaemonAbi(stub), stub_(stub) {}

  bool PublishesHtmlStorePolicy() const override {
    return stub_->PublishesHtmlStorePolicy();
  }
  int HtmlStoreClassify(int mode, const char* method, const char* url_path,
                        uint32_t request_flags, int status,
                        const char* const* names, const char* const* values,
                        size_t count, uint64_t body_bytes, int body_complete,
                        const char* body_prefix, size_t body_prefix_len,
                        uint64_t max_body_bytes, uint32_t now_seconds,
                        int* out_reason, uint32_t* out_lifetime_s,
                        uint32_t* out_flags, uint16_t* out_cc_flags,
                        int* out_sidecar, char* detail_buf,
                        size_t detail_capacity,
                        size_t* out_detail_len) const override {
    Call call;
    call.mode = mode;
    for (size_t i = 0; i < count; ++i) {
      call.headers.emplace_back(names[i], values[i]);
    }
    call.body_bytes = body_bytes;
    call.body_complete = body_complete;
    call.prefix_len = body_prefix_len;
    calls.push_back(call);
    return stub_->HtmlStoreClassify(
        mode, method, url_path, request_flags, status, names, values, count,
        body_bytes, body_complete, body_prefix, body_prefix_len, max_body_bytes,
        now_seconds, out_reason, out_lifetime_s, out_flags, out_cc_flags,
        out_sidecar, detail_buf, detail_capacity, out_detail_len);
  }
  const char* HtmlStoreReasonName(int reason) const override {
    return stub_->HtmlStoreReasonName(reason);
  }
  int HtmlDecisionFormat(const char* const* keys, const char* const* values,
                         size_t count, char* buf, size_t capacity,
                         size_t* out_len) const override {
    return stub_->HtmlDecisionFormat(keys, values, count, buf, capacity,
                                     out_len);
  }

  // Two per recording: the store view, then the final view.
  mutable std::vector<Call> calls;

 private:
  const DaemonAbi* stub_;
};

DaemonHtmlHeaderLines Sorted(DaemonHtmlHeaderLines lines) {
  std::sort(lines.begin(), lines.end());
  return lines;
}

// Against the stub library flavour that exports the policy, whose classifier
// answers what PS_STUB_HTML_ACTION and PS_STUB_HTML_REASON say and whose
// formatter writes `key=value ` pairs. The policy's own rules are the
// library's, and are tested there; this tests what the module does with its
// answers.
class DaemonHtmlReportTest : public ::testing::Test {
 protected:
  DaemonHtmlReportTest()
      : thread_system_(Platform::CreateThreadSystem()),
        stats_(thread_system_.get()),
        timer_(new NullMutex, 1777777777000LL) {
    RewriteStats::InitStats(&stats_);
    dir_ = std::filesystem::path(testing::TempDir()) /
           ("daemon_html_" + std::to_string(reinterpret_cast<uintptr_t>(this)));
    std::filesystem::remove_all(dir_);
    unsetenv("PS_STUB_HTML_ACTION");
    unsetenv("PS_STUB_HTML_REASON");
    unsetenv("PS_STUB_MINOR");
    abi_ = Load();
  }

  ~DaemonHtmlReportTest() override {
    // A queued line holds its log; let it go before the next test can reuse
    // this directory name.
    DaemonHtmlDecisionLog::WaitForWritesForTesting();
    std::filesystem::remove_all(dir_);
    unsetenv("PS_STUB_MINOR");
  }

  std::unique_ptr<DaemonAbi> Load(
      const char* library = "libdaemon_stub_html_policy.so") {
    GoogleString error;
    std::unique_ptr<DaemonAbi> abi(
        LoadDaemonAbi(StubLibraryPath(library), &error));
    EXPECT_TRUE(abi != nullptr) << error;
    return abi;
  }

  std::unique_ptr<DaemonHtmlEngine> NewEngine(
      bool with_log = true, DaemonHtmlMode mode = DaemonHtmlMode::kReportOnly,
      const DaemonAbi* abi = nullptr) {
    DaemonHtmlEngine::Config config;
    config.mode = mode;
    config.max_bytes = 5 << 20;
    DaemonHtmlEngine::Deps deps;
    deps.substrate.abi = abi == nullptr ? abi_.get() : abi;
    deps.statistics = &stats_;
    deps.timer = &timer_;
    deps.handler = &handler_;
    if (with_log) deps.log_dir = dir_.string();
    return std::make_unique<DaemonHtmlEngine>(std::move(config),
                                              std::move(deps));
  }

  int64_t Count(const char* name) { return stats_.GetVariable(name)->Get(); }

  std::vector<std::string> LogLines() {
    DaemonHtmlDecisionLog::WaitForWritesForTesting();
    std::vector<std::string> lines;
    std::ifstream in(dir_ / "daemon-html" /
                     RulePackDecisionLog::FileName(
                         RulePackDecisionLog::UtcDate(timer_.NowMs())));
    for (std::string line; std::getline(in, line);) lines.push_back(line);
    return lines;
  }

  static DaemonHtmlRequestFacts Request(const char* path) {
    DaemonHtmlRequestFacts facts;
    facts.method = "GET";
    facts.path = path;
    facts.host = "www.example.com";
    return facts;
  }

  void Run(DaemonHtmlEngine* engine, const char* path = "/a.html",
           bool end = true) {
    std::unique_ptr<DaemonHtmlRecording> rec =
        engine->StartRecording(Request(path), nullptr);
    ASSERT_NE(nullptr, rec);
    rec->set_status(200);
    rec->SetOriginHeaders({{"Content-Type", "text/html"}});
    rec->AddBody("<html></html>", 13);
    if (end) rec->set_body_ended();
    rec->Finish(false);
  }

  std::unique_ptr<ThreadSystem> thread_system_;
  SimpleStats stats_;
  MockTimer timer_;
  MockMessageHandler handler_{new NullMutex};
  std::filesystem::path dir_;
  std::unique_ptr<DaemonAbi> abi_;
};

TEST_F(DaemonHtmlReportTest, WouldStoreCountsAndLogs) {
  std::unique_ptr<DaemonHtmlEngine> engine = NewEngine();
  Run(engine.get());
  EXPECT_EQ(1, Count("daemon_html_evaluated"));
  EXPECT_EQ(1, Count("daemon_html_would_store"));
  EXPECT_EQ(13, Count("daemon_html_would_store_bytes"));
  EXPECT_EQ(0, Count("daemon_html_refused"));
  // Report-only stores and serves nothing, so it is not "unavailable".
  EXPECT_EQ(0, Count("daemon_html_serve_unavailable"));
  EXPECT_EQ(0, Count("daemon_html_stored"));
  const std::vector<std::string> lines = LogLines();
  ASSERT_EQ(1u, lines.size());
  EXPECT_NE(std::string::npos, lines[0].find("action=would_store "));
  EXPECT_NE(std::string::npos, lines[0].find("path=/a.html"));
  EXPECT_NE(std::string::npos, lines[0].find("mode=report-only"));
  EXPECT_NE(std::string::npos, lines[0].find("host=www.example.com"));
}

TEST_F(DaemonHtmlReportTest, EveryHostAndUrlIsReported) {
  // Report-only reads what the policy says of every HTML response, whether
  // or not the engine could store it: no site, no URL agreement, a host the
  // configuration does not list.
  std::unique_ptr<DaemonHtmlEngine> engine = NewEngine(/*with_log=*/false);
  DaemonHtmlRequestFacts facts = Request("/a.html");
  facts.url_agreed = false;
  facts.site.clear();
  facts.host = "unlisted.example";
  std::unique_ptr<DaemonHtmlRecording> rec =
      engine->StartRecording(facts, nullptr);
  ASSERT_NE(nullptr, rec);
  rec->set_status(200);
  rec->SetOriginHeaders({{"Content-Type", "text/html"}});
  rec->set_body_ended();
  rec->Finish(false);
  EXPECT_EQ(1, Count("daemon_html_evaluated"));
  EXPECT_EQ(0, Count("daemon_html_refused_url_disagree"));
}

TEST_F(DaemonHtmlReportTest, BodyWindowIsBounded) {
  std::unique_ptr<DaemonHtmlEngine> engine = NewEngine(/*with_log=*/false);
  std::unique_ptr<DaemonHtmlRecording> rec =
      engine->StartRecording(Request("/a.html"), nullptr);
  ASSERT_NE(nullptr, rec);
  rec->set_status(200);
  const std::string chunk(40 * 1024, 'x');
  for (int i = 0; i < 3; ++i) rec->AddBody(chunk.data(), chunk.size());
  rec->set_body_ended();
  rec->Finish(false);
  EXPECT_EQ(1, Count("daemon_html_evaluated"));
  // Every byte counts; only the scan window is held, outside the budget.
  EXPECT_EQ(120 * 1024, Count("daemon_html_would_store_bytes"));
}

TEST_F(DaemonHtmlReportTest, AnUnendedOrAbortedBodyIsClassifiedIncomplete) {
  ClassifyRecordingAbi abi(abi_.get());
  std::unique_ptr<DaemonHtmlEngine> engine = NewEngine(
      /*with_log=*/false, DaemonHtmlMode::kReportOnly, &abi);
  auto run = [&](bool ended, bool aborted) {
    std::unique_ptr<DaemonHtmlRecording> rec =
        engine->StartRecording(Request("/a.html"), nullptr);
    ASSERT_NE(nullptr, rec);
    rec->set_status(200);
    rec->SetOriginHeaders({{"Content-Type", "text/html"}});
    rec->AddBody("<html></html>", 13);
    if (ended) rec->set_body_ended();
    rec->Finish(aborted);
  };
  run(/*ended=*/true, /*aborted=*/false);
  run(/*ended=*/false, /*aborted=*/false);
  run(/*ended=*/true, /*aborted=*/true);
  ASSERT_EQ(6u, abi.calls.size());
  for (size_t i = 0; i < abi.calls.size(); ++i) {
    EXPECT_EQ(kPsHtmlCacheReportOnly, abi.calls[i].mode) << i;
    EXPECT_EQ(13u, abi.calls[i].body_bytes) << i;
    EXPECT_EQ(13u, abi.calls[i].prefix_len) << i;
    // Complete only for the body that ended and was not aborted.
    EXPECT_EQ(i < 2 ? 1 : 0, abi.calls[i].body_complete) << i;
  }
  EXPECT_EQ(3, Count("daemon_html_evaluated"));
}

TEST_F(DaemonHtmlReportTest, PersonalisationIsARefusalWithItsReason) {
  setenv("PS_STUB_HTML_ACTION", "0", 1);
  setenv("PS_STUB_HTML_REASON", "18", 1);
  std::unique_ptr<DaemonHtmlEngine> engine = NewEngine();
  Run(engine.get());
  EXPECT_EQ(0, Count("daemon_html_would_store"));
  EXPECT_EQ(1, Count("daemon_html_refused"));
  EXPECT_EQ(1, Count("daemon_html_refused_personalisation_signal"));
  const std::vector<std::string> lines = LogLines();
  ASSERT_EQ(1u, lines.size());
  EXPECT_NE(std::string::npos, lines[0].find("action=refuse"));
  EXPECT_NE(std::string::npos, lines[0].find("reason=personalisation_signal"));
  EXPECT_NE(std::string::npos,
            lines[0].find("personalisation=meta_csrf_token"));
}

TEST_F(DaemonHtmlReportTest, UnknownReasonCountsAsOther) {
  setenv("PS_STUB_HTML_ACTION", "0", 1);
  setenv("PS_STUB_HTML_REASON", "99", 1);
  std::unique_ptr<DaemonHtmlEngine> engine = NewEngine(/*with_log=*/false);
  Run(engine.get());
  EXPECT_EQ(1, Count("daemon_html_refused"));
  EXPECT_EQ(1, Count("daemon_html_refused_other"));
}

TEST_F(DaemonHtmlReportTest, MalformedCallCountsAndLogsNothing) {
  setenv("PS_STUB_HTML_ACTION", "-1", 1);
  std::unique_ptr<DaemonHtmlEngine> engine = NewEngine();
  Run(engine.get());
  EXPECT_EQ(1, Count("daemon_html_classify_failed"));
  EXPECT_EQ(0, Count("daemon_html_would_store"));
  EXPECT_TRUE(LogLines().empty());
}

TEST_F(DaemonHtmlReportTest, DuplicateDecisionIsOneLineButCountsTwice) {
  std::unique_ptr<DaemonHtmlEngine> engine = NewEngine();
  Run(engine.get());
  Run(engine.get());
  EXPECT_EQ(2, Count("daemon_html_would_store"));
  EXPECT_EQ(1u, LogLines().size());
}

TEST_F(DaemonHtmlReportTest, QueryStringNeverReachesTheLog) {
  std::unique_ptr<DaemonHtmlEngine> engine = NewEngine();
  Run(engine.get(), "/a.html?token=secret");
  const std::vector<std::string> lines = LogLines();
  ASSERT_EQ(1u, lines.size());
  EXPECT_EQ(std::string::npos, lines[0].find("secret"));
  EXPECT_NE(std::string::npos, lines[0].find("path=/a.html"));
}

TEST_F(DaemonHtmlReportTest, PercentEscapedPathsAreLoggedWhole) {
  std::unique_ptr<DaemonHtmlEngine> engine = NewEngine();
  Run(engine.get(), "/caf%C3%A9.html");
  Run(engine.get(), "/caf%C3%A8.html");
  Run(engine.get(), "/b.html;jsessionid=secret");
  const std::vector<std::string> lines = LogLines();
  ASSERT_EQ(3u, lines.size());
  EXPECT_NE(std::string::npos, lines[0].find("path=/caf%C3%A9.html "));
  EXPECT_NE(std::string::npos, lines[1].find("path=/caf%C3%A8.html "));
  EXPECT_NE(std::string::npos, lines[2].find("path=/b.html "));
  EXPECT_EQ(std::string::npos, lines[2].find("secret"));
}

TEST_F(DaemonHtmlReportTest, EnginesOfOneLogDirShareTheLog) {
  // Two server configurations of one process with the same LogDir: one dedup
  // set and one per-second limit between them.
  std::unique_ptr<DaemonHtmlEngine> first = NewEngine();
  std::unique_ptr<DaemonHtmlEngine> second = NewEngine();
  Run(first.get(), "/same.html");
  Run(second.get(), "/same.html");
  EXPECT_EQ(1u, LogLines().size());
  for (int i = 0; i < DaemonHtmlDecisionLog::kMaxLinesPerSecond; ++i) {
    const std::string path = "/p" + std::to_string(i) + ".html";
    Run(i % 2 == 0 ? first.get() : second.get(), path.c_str());
  }
  const std::vector<std::string> lines = LogLines();
  EXPECT_EQ(static_cast<size_t>(DaemonHtmlDecisionLog::kMaxLinesPerSecond),
            lines.size());
  // The writer's drops reach the counter with the next report.
  Run(first.get(), "/after.html");
  DaemonHtmlDecisionLog::WaitForWritesForTesting();
  Run(first.get(), "/after2.html");
  EXPECT_EQ(2, Count("daemon_html_log_dropped"));
}

// The exact final view the policy reads, from the origin's lines, the
// baseline (after the module's own changes) and the lines at the final
// position.
class DaemonHtmlReportViewTest : public DaemonHtmlReportTest {
 protected:
  DaemonHtmlHeaderLines FinalView(const DaemonHtmlHeaderLines& origin,
                                  const DaemonHtmlHeaderLines& baseline,
                                  const DaemonHtmlHeaderLines& at_final) {
    ClassifyRecordingAbi abi(abi_.get());
    std::unique_ptr<DaemonHtmlEngine> engine = NewEngine(
        /*with_log=*/false, DaemonHtmlMode::kReportOnly, &abi);
    std::unique_ptr<DaemonHtmlRecording> rec =
        engine->StartRecording(Request("/v.html"), nullptr);
    EXPECT_NE(nullptr, rec);
    if (rec == nullptr) return {};
    rec->set_status(200);
    rec->SetOriginHeaders(origin);
    rec->SetBaselineHeaders(baseline);
    EXPECT_TRUE(rec->needs_final_headers());
    rec->NoteFinalHeaders(at_final);
    EXPECT_FALSE(rec->needs_final_headers());
    // Only the first final view counts.
    rec->NoteFinalHeaders({{"X-Late", "1"}});
    rec->set_body_ended();
    rec->Finish(false);
    EXPECT_EQ(2u, abi.calls.size());
    if (abi.calls.size() != 2) return {};
    // Without a store snapshot the store view is the origin's lines.
    EXPECT_EQ(origin, abi.calls[0].headers);
    return abi.calls[1].headers;
  }
};

TEST_F(DaemonHtmlReportViewTest, LateHeadersApplyWhatChangedSinceTheBaseline) {
  // The origin's lines, as captured early; then the module's own changes
  // (its signature added, the length gone); later the header stage replaced
  // Cache-Control, set a cookie and removed one of two repeated lines, and
  // compression added Vary and an encoding.
  const DaemonHtmlHeaderLines view =
      FinalView({{"Content-Type", "text/html"},
                 {"Cache-Control", "max-age=60"},
                 {"Content-Length", "13"},
                 {"X-Origin", "a"},
                 {"X-Origin", "a"}},
                {{"Cache-Control", "max-age=60"},
                 {"X-Origin", "a"},
                 {"X-Origin", "a"},
                 {"X-Mod-Pagespeed", "1"}},
                {{"cache-control", "private, no-store"},
                 {"X-Origin", "a"},
                 {"Set-Cookie", "lane=1"},
                 {"X-Mod-Pagespeed", "1"},
                 {"Vary", "Accept-Encoding"},
                 {"Content-Encoding", "gzip"}});
  const DaemonHtmlHeaderLines expected = {
      {"Content-Type", "text/html"},
      {"Content-Length", "13"},
      {"X-Origin", "a"},
      {"cache-control", "private, no-store"},
      {"Set-Cookie", "lane=1"}};
  EXPECT_EQ(expected, view);
}

TEST_F(DaemonHtmlReportViewTest, WhatCompressionAddsIsNoChange) {
  const DaemonHtmlHeaderLines view = FinalView(
      {{"Vary", "User-Agent"}, {"ETag", "\"abc\""}},
      {{"Vary", "User-Agent"}, {"ETag", "\"abc\""}},
      {{"Vary", "user-agent,Accept-Encoding"}, {"ETag", "\"abc-gzip\""}});
  EXPECT_EQ(Sorted({{"Vary", "User-Agent"}, {"ETag", "\"abc\""}}),
            Sorted(view));
}

TEST_F(DaemonHtmlReportViewTest, AnAppendedVaryTokenAndAReplacedEtagCount) {
  // Header append Vary Cookie counts; so does an ETag the server replaced
  // (kept as sent, compression's suffix included: it is not the origin's).
  const DaemonHtmlHeaderLines view =
      FinalView({{"Content-Type", "text/html"},
                 {"Vary", "User-Agent"},
                 {"ETag", "\"abc\""}},
                {{"Vary", "User-Agent, Accept-Encoding"}, {"ETag", "\"abc\""}},
                {{"Vary", "User-Agent,Accept-Encoding,Cookie"},
                 {"ETag", "\"xyz-gzip\""}});
  EXPECT_EQ(Sorted({{"Content-Type", "text/html"},
                    {"ETag", "\"xyz-gzip\""},
                    {"Vary", "User-Agent, Cookie"}}),
            Sorted(view));
}

TEST_F(DaemonHtmlReportViewTest, AVaryTokenTheServerRemovedGoes) {
  const DaemonHtmlHeaderLines view = FinalView(
      {{"Vary", "Cookie"}, {"Vary", "User-Agent"}},
      {{"Vary", "Cookie"}, {"Vary", "User-Agent"}}, {{"Vary", "User-Agent"}});
  EXPECT_EQ(DaemonHtmlHeaderLines({{"Vary", "User-Agent"}}), view);
}

TEST_F(DaemonHtmlReportTest, TheFinalViewIsWhatIsReported) {
  // The origin's lines; the module's own changes (its signature added, the
  // length gone); later the server's header stage replaced Cache-Control,
  // set a cookie and removed one of two repeated lines, and compression
  // added Vary and an encoding.  The report reads the final view.
  setenv("PS_STUB_HTML_ACTION", "0", 1);
  std::unique_ptr<DaemonHtmlEngine> engine = NewEngine();
  std::unique_ptr<DaemonHtmlRecording> rec =
      engine->StartRecording(Request("/late.html"), nullptr);
  ASSERT_NE(nullptr, rec);
  rec->set_status(200);
  rec->SetOriginHeaders({{"Content-Type", "text/html"},
                         {"Cache-Control", "max-age=60"},
                         {"Content-Length", "13"},
                         {"X-Origin", "a"},
                         {"X-Origin", "a"},
                         {"Vary", "User-Agent"}});
  rec->SetBaselineHeaders({{"Cache-Control", "max-age=60"},
                           {"X-Origin", "a"},
                           {"X-Origin", "a"},
                           {"Vary", "User-Agent"}});
  EXPECT_TRUE(rec->needs_final_headers());
  rec->NoteFinalHeaders({{"cache-control", "private, no-store"},
                         {"X-Origin", "a"},
                         {"Set-Cookie", "lane=1"},
                         {"Vary", "User-Agent, Accept-Encoding, Cookie"},
                         {"Content-Encoding", "gzip"}});
  EXPECT_FALSE(rec->needs_final_headers());
  rec->set_body_ended();
  rec->Finish(false);
  const std::vector<std::string> lines = LogLines();
  ASSERT_EQ(1u, lines.size());
  // The token mod_headers appended counts; the one compression added does
  // not.
  EXPECT_NE(std::string::npos, lines[0].find("vary=User-Agent, Cookie "))
      << lines[0];
  // The library is 1.12 here: the line carries no vhost_added.
  EXPECT_EQ(std::string::npos, lines[0].find("vhost_added")) << lines[0];
}

TEST_F(DaemonHtmlReportTest, VhostAddedNamesWhatTheServerAddedWithA114Library) {
  setenv("PS_STUB_MINOR", "14", 1);
  std::unique_ptr<DaemonAbi> abi = Load();
  ASSERT_EQ(14, abi->VersionMinor());
  std::unique_ptr<DaemonHtmlEngine> engine =
      NewEngine(/*with_log=*/true, DaemonHtmlMode::kReportOnly, abi.get());
  std::unique_ptr<DaemonHtmlRecording> rec =
      engine->StartRecording(Request("/added.html"), nullptr);
  ASSERT_NE(nullptr, rec);
  rec->set_status(200);
  rec->SetOriginHeaders(
      {{"Content-Type", "text/html"}, {"Cache-Control", "max-age=60"}});
  rec->SetBaselineHeaders(
      {{"Content-Type", "text/html"}, {"Cache-Control", "max-age=60"}});
  rec->NoteFinalHeaders({{"Content-Type", "text/html"},
                         {"Cache-Control", "max-age=60"},
                         {"X-Frame-Options", "DENY"},
                         {"X-T", "1"},
                         {"X-T", "2"},
                         {"Content-Encoding", "gzip"}});
  rec->set_body_ended();
  rec->Finish(false);
  const std::vector<std::string> lines = LogLines();
  ASSERT_EQ(1u, lines.size());
  // Names only, lower-case, each once; what compression adds is not the
  // server's configuration.
  EXPECT_NE(std::string::npos,
            lines[0].find("vhost_added=x-frame-options,x-t "))
      << lines[0];
  EXPECT_EQ(std::string::npos, lines[0].find("DENY")) << lines[0];
}

TEST_F(DaemonHtmlReportTest, NothingAddedLeavesVhostAddedOut) {
  setenv("PS_STUB_MINOR", "14", 1);
  std::unique_ptr<DaemonAbi> abi = Load();
  std::unique_ptr<DaemonHtmlEngine> engine =
      NewEngine(/*with_log=*/true, DaemonHtmlMode::kReportOnly, abi.get());
  Run(engine.get());
  const std::vector<std::string> lines = LogLines();
  ASSERT_EQ(1u, lines.size());
  EXPECT_EQ(std::string::npos, lines[0].find("vhost_added")) << lines[0];
}

TEST(DaemonHtmlVhostAddedTest, NamesOnlyEachOnceInOrder) {
  const DaemonHtmlLogHeaderLines store = {{"Content-Type", "text/html"},
                                          {"X-T", "1"},
                                          {"Cache-Control", "max-age=60"}};
  EXPECT_EQ("", DaemonHtmlVhostAdded(store, store));
  // A changed value is a line the store view did not hold.
  EXPECT_EQ("cache-control",
            DaemonHtmlVhostAdded(store, {{"Content-Type", "text/html"},
                                         {"X-T", "1"},
                                         {"Cache-Control", "private"}}));
  // A second copy of a line counts; a name is listed once, lower-cased, in
  // the order the final view has them.
  EXPECT_EQ("x-t,expires",
            DaemonHtmlVhostAdded(store, {{"Content-Type", "text/html"},
                                         {"X-T", "1"},
                                         {"x-t", "1"},
                                         {"Cache-Control", "max-age=60"},
                                         {"Expires", "0"},
                                         {"X-T", "2"}}));
  // A line removed after the snapshot is not "added".
  EXPECT_EQ("", DaemonHtmlVhostAdded(store, {{"Content-Type", "text/html"}}));
  // Never a name the line could not carry.
  EXPECT_EQ("x-ok", DaemonHtmlVhostAdded({}, {{"X Bad", "1"}, {"X-Ok", "1"}}));
}

TEST(DaemonHtmlVhostAddedTest, AListTooLongForTheLineIsLeftOut) {
  DaemonHtmlLogHeaderLines added;
  for (int i = 0; i < 200; ++i) {
    added.emplace_back("X-Long-Header-Name-" + std::to_string(i), "1");
  }
  EXPECT_EQ("", DaemonHtmlVhostAdded({}, added));
}

TEST_F(DaemonHtmlReportTest, FinishIsOnce) {
  std::unique_ptr<DaemonHtmlEngine> engine = NewEngine(/*with_log=*/false);
  std::unique_ptr<DaemonHtmlRecording> rec =
      engine->StartRecording(Request("/"), nullptr);
  ASSERT_NE(nullptr, rec);
  rec->Finish(false);
  rec->Finish(false);
  EXPECT_EQ(1, Count("daemon_html_evaluated"));
}

TEST_F(DaemonHtmlReportTest, NoLibraryIsUnavailable) {
  DaemonHtmlEngine::Config config;
  config.mode = DaemonHtmlMode::kReportOnly;
  DaemonHtmlEngine::Deps deps;
  deps.statistics = &stats_;
  deps.timer = &timer_;
  deps.handler = &handler_;
  DaemonHtmlEngine engine(config, deps);
  bool unavailable = false;
  EXPECT_EQ(nullptr,
            engine.StartRecording(Request("/a.html"), nullptr, &unavailable));
  EXPECT_TRUE(unavailable);
  // Counted by the port, once it knows it handles the response.
  EXPECT_EQ(0, Count("daemon_html_unavailable"));
  engine.CountUnavailable();
  EXPECT_EQ(1, Count("daemon_html_unavailable"));
  EXPECT_EQ(0, Count("daemon_html_evaluated"));
}

TEST_F(DaemonHtmlReportTest, ALibraryWithoutThePolicyIsUnavailable) {
  std::unique_ptr<DaemonAbi> old = Load("libdaemon_stub.so");
  ASSERT_FALSE(old->PublishesHtmlStorePolicy());
  std::unique_ptr<DaemonHtmlEngine> engine =
      NewEngine(/*with_log=*/false, DaemonHtmlMode::kReportOnly, old.get());
  bool unavailable = false;
  EXPECT_EQ(nullptr,
            engine->StartRecording(Request("/a.html"), nullptr, &unavailable));
  EXPECT_TRUE(unavailable);
  EXPECT_EQ(0, Count("daemon_html_unavailable"));
  EXPECT_EQ(0, Count("daemon_html_evaluated"));

  // A recording made says nothing is unavailable.
  std::unique_ptr<DaemonHtmlEngine> good = NewEngine(/*with_log=*/false);
  unavailable = true;
  EXPECT_NE(nullptr,
            good->StartRecording(Request("/a.html"), nullptr, &unavailable));
  EXPECT_FALSE(unavailable);
}

TEST_F(DaemonHtmlReportTest, ServeVariantsOnAPortReportsExactlyAsReportOnly) {
  // What a port builds from its options: serve-variants runs as report-only
  // until the port stores and serves, so the line says report-only and no
  // volume is ever touched (there is none here to touch).
  std::unique_ptr<DaemonHtmlEngine> engine = NewDaemonHtmlEngineForPort(
      RewriteOptions::kDaemonHtmlCacheServeVariants, 5 << 20, dir_.string(),
      abi_.get(), &stats_, &timer_, &handler_);
  ASSERT_NE(nullptr, engine);
  EXPECT_EQ(DaemonHtmlMode::kReportOnly, engine->config().mode);
  EXPECT_FALSE(engine->serving());
  Run(engine.get());
  EXPECT_EQ(1, Count("daemon_html_would_store"));
  EXPECT_EQ(0, Count("daemon_html_serve_unavailable"));
  const std::vector<std::string> lines = LogLines();
  ASSERT_EQ(1u, lines.size());
  EXPECT_NE(std::string::npos, lines[0].find("mode=report-only"));
  DaemonHtmlRequestFacts markdown = Request("/a.html");
  markdown.accept_lines = {"text/markdown"};
  EXPECT_EQ(DaemonHtmlServeResult::kFallThrough,
            engine->Serve(markdown, nullptr).kind);

  // Off builds nothing; an empty LogDir warns once.
  EXPECT_EQ(nullptr, NewDaemonHtmlEngineForPort(
                         RewriteOptions::kDaemonHtmlCacheOff, 5 << 20, "",
                         abi_.get(), &stats_, &timer_, &handler_));
  EXPECT_EQ(0, handler_.MessagesOfType(kWarning));
  EXPECT_NE(nullptr, NewDaemonHtmlEngineForPort(
                         RewriteOptions::kDaemonHtmlCacheReportOnly, 5 << 20,
                         "", abi_.get(), &stats_, &timer_, &handler_));
  EXPECT_EQ(1, handler_.MessagesOfType(kWarning));
}

}  // namespace
}  // namespace net_instaweb
