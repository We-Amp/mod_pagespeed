// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

#include "pagespeed/system/daemon_html_report.h"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

#include "gtest/gtest.h"
#include "net/instaweb/rewriter/public/rewrite_stats.h"
#include "pagespeed/kernel/base/google_message_handler.h"
#include "pagespeed/kernel/base/null_mutex.h"
#include "pagespeed/kernel/base/thread_system.h"
#include "pagespeed/kernel/util/platform.h"
#include "pagespeed/kernel/util/simple_stats.h"
#include "pagespeed/system/daemon_abi.h"
#include "pagespeed/system/rule_pack_decision_log.h"
#include "test/pagespeed/kernel/base/mock_timer.h"
#include "test/pagespeed/system/daemon_stub_test_util.h"

namespace net_instaweb {
namespace {

// Against the stub library flavour that exports the policy, whose classifier
// answers what PS_STUB_HTML_ACTION and PS_STUB_HTML_REASON say and logs what
// it was given to PS_STUB_LOG. The policy's own rules are the library's, and
// are tested there; this tests what the module does with its answers.
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
    GoogleString error;
    abi_.reset(LoadDaemonAbi(StubLibraryPath("libdaemon_stub_html_policy.so"),
                             &error));
    EXPECT_TRUE(abi_ != nullptr) << error;
    unsetenv("PS_STUB_HTML_ACTION");
    unsetenv("PS_STUB_HTML_REASON");
  }

  ~DaemonHtmlReportTest() override {
    // A queued line holds its log; let it go before the next test can reuse
    // this directory name.
    DaemonHtmlReporter::WaitForLogWritesForTesting();
    std::filesystem::remove_all(dir_);
  }

  int64_t Count(const char* name) { return stats_.GetVariable(name)->Get(); }

  std::vector<std::string> LogLines() {
    DaemonHtmlReporter::WaitForLogWritesForTesting();
    std::vector<std::string> lines;
    std::ifstream in(dir_ / "daemon-html" /
                     RulePackDecisionLog::FileName(
                         RulePackDecisionLog::UtcDate(timer_.NowMs())));
    for (std::string line; std::getline(in, line);) lines.push_back(line);
    return lines;
  }

  void Run(DaemonHtmlReporter* reporter, const char* path = "/a.html",
           bool end = true) {
    DaemonHtmlCapture capture(reporter, abi_.get(), 5 << 20);
    capture.SetRequest("GET", path, "www.example.com");
    capture.set_status(200);
    capture.AddHeader("Content-Type", "text/html");
    capture.AddBody("<html></html>", 13);
    if (end) capture.set_body_ended();
    capture.Finish(false);
  }

  std::unique_ptr<ThreadSystem> thread_system_;
  SimpleStats stats_;
  MockTimer timer_;
  GoogleMessageHandler handler_;
  std::filesystem::path dir_;
  std::unique_ptr<DaemonAbi> abi_;
};

TEST_F(DaemonHtmlReportTest, BodyWindowIsBounded) {
  DaemonHtmlReporter reporter(&stats_, &timer_, "", &handler_);
  DaemonHtmlCapture capture(&reporter, abi_.get(), 5 << 20);
  const std::string chunk(40 * 1024, 'x');
  capture.AddBody(chunk.data(), chunk.size());
  capture.AddBody(chunk.data(), chunk.size());
  capture.AddBody(chunk.data(), chunk.size());
  capture.set_body_ended();
  capture.Finish(false);
  EXPECT_EQ(DaemonHtmlCapture::kBodyWindowBytes, capture.body_prefix_size());
  EXPECT_EQ(1, Count("daemon_html_evaluated"));
  EXPECT_EQ(120 * 1024, Count("daemon_html_would_store_bytes"));
}

TEST_F(DaemonHtmlReportTest, WouldStoreCountsAndLogs) {
  DaemonHtmlReporter reporter(&stats_, &timer_, dir_.string(), &handler_);
  Run(&reporter);
  EXPECT_EQ(1, Count("daemon_html_evaluated"));
  EXPECT_EQ(1, Count("daemon_html_would_store"));
  EXPECT_EQ(13, Count("daemon_html_would_store_bytes"));
  EXPECT_EQ(0, Count("daemon_html_refused"));
  const std::vector<std::string> lines = LogLines();
  ASSERT_EQ(1u, lines.size());
  EXPECT_NE(std::string::npos, lines[0].find("action=would_store"));
  EXPECT_NE(std::string::npos, lines[0].find("path=/a.html"));
  EXPECT_NE(std::string::npos, lines[0].find("mode=report-only"));
}

TEST_F(DaemonHtmlReportTest, PersonalisationIsARefusalWithItsReason) {
  setenv("PS_STUB_HTML_ACTION", "0", 1);
  setenv("PS_STUB_HTML_REASON", "18", 1);
  DaemonHtmlReporter reporter(&stats_, &timer_, dir_.string(), &handler_);
  Run(&reporter);
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
  DaemonHtmlReporter reporter(&stats_, &timer_, "", &handler_);
  Run(&reporter);
  EXPECT_EQ(1, Count("daemon_html_refused"));
  EXPECT_EQ(1, Count("daemon_html_refused_other"));
}

TEST_F(DaemonHtmlReportTest, MalformedCallCountsAndLogsNothing) {
  setenv("PS_STUB_HTML_ACTION", "-1", 1);
  DaemonHtmlReporter reporter(&stats_, &timer_, dir_.string(), &handler_);
  Run(&reporter);
  EXPECT_EQ(1, Count("daemon_html_classify_failed"));
  EXPECT_EQ(0, Count("daemon_html_would_store"));
  EXPECT_TRUE(LogLines().empty());
}

TEST_F(DaemonHtmlReportTest, DuplicateDecisionIsOneLineButCountsTwice) {
  DaemonHtmlReporter reporter(&stats_, &timer_, dir_.string(), &handler_);
  Run(&reporter);
  Run(&reporter);
  EXPECT_EQ(2, Count("daemon_html_would_store"));
  EXPECT_EQ(1u, LogLines().size());
}

TEST_F(DaemonHtmlReportTest, QueryStringNeverReachesTheLog) {
  DaemonHtmlReporter reporter(&stats_, &timer_, dir_.string(), &handler_);
  Run(&reporter, "/a.html?token=secret");
  const std::vector<std::string> lines = LogLines();
  ASSERT_EQ(1u, lines.size());
  EXPECT_EQ(std::string::npos, lines[0].find("secret"));
  EXPECT_NE(std::string::npos, lines[0].find("path=/a.html"));
}

TEST_F(DaemonHtmlReportTest, PercentEscapedPathsAreLoggedWhole) {
  DaemonHtmlReporter reporter(&stats_, &timer_, dir_.string(), &handler_);
  Run(&reporter, "/caf%C3%A9.html");
  Run(&reporter, "/caf%C3%A8.html");
  Run(&reporter, "/b.html;jsessionid=secret");
  const std::vector<std::string> lines = LogLines();
  ASSERT_EQ(3u, lines.size());
  EXPECT_NE(std::string::npos, lines[0].find("path=/caf%C3%A9.html "));
  EXPECT_NE(std::string::npos, lines[1].find("path=/caf%C3%A8.html "));
  EXPECT_NE(std::string::npos, lines[2].find("path=/b.html "));
  EXPECT_EQ(std::string::npos, lines[2].find("secret"));
}

TEST_F(DaemonHtmlReportTest, ReportersOfOneLogDirShareTheLog) {
  // Two server configurations of one process with the same LogDir: one dedup
  // set and one per-second limit between them.
  DaemonHtmlReporter first(&stats_, &timer_, dir_.string(), &handler_);
  DaemonHtmlReporter second(&stats_, &timer_, dir_.string(), &handler_);
  Run(&first, "/same.html");
  Run(&second, "/same.html");
  EXPECT_EQ(1u, LogLines().size());
  for (int i = 0; i < DaemonHtmlReporter::kMaxLinesPerSecond; ++i) {
    const std::string path = "/p" + std::to_string(i) + ".html";
    Run(i % 2 == 0 ? &first : &second, path.c_str());
  }
  const std::vector<std::string> lines = LogLines();
  EXPECT_EQ(static_cast<size_t>(DaemonHtmlReporter::kMaxLinesPerSecond),
            lines.size());
  // The writer's drops reach the counter with the next report.
  Run(&first, "/after.html");
  DaemonHtmlReporter::WaitForLogWritesForTesting();
  Run(&first, "/after2.html");
  EXPECT_EQ(2, Count("daemon_html_log_dropped"));
}

TEST_F(DaemonHtmlReportTest, LateHeadersApplyWhatChangedSinceTheBaseline) {
  DaemonHtmlReporter reporter(&stats_, &timer_, "", &handler_);
  DaemonHtmlCapture capture(&reporter, abi_.get(), 5 << 20);
  // The origin's lines, as captured early.
  capture.AddHeader("Content-Type", "text/html");
  capture.AddHeader("Cache-Control", "max-age=60");
  capture.AddHeader("Content-Length", "13");
  capture.AddHeader("X-Origin", "a");
  capture.AddHeader("X-Origin", "a");
  // After the module's own changes: its signature added, the length gone.
  capture.SetBaselineHeaders({{"Cache-Control", "max-age=60"},
                              {"X-Origin", "a"},
                              {"X-Origin", "a"},
                              {"X-Mod-Pagespeed", "1"}});
  EXPECT_TRUE(capture.needs_late_headers());
  // Later: the header stage replaced Cache-Control, set a cookie and removed
  // one of the two repeated lines; compression added Vary and an encoding.
  capture.ApplyLateHeaders(
      {{"cache-control", "private, no-store"},
       {"X-Origin", "a"},
       {"Set-Cookie", "lane=1"},
       {"X-Mod-Pagespeed", "1"},
       {"Vary", "Accept-Encoding"},
       {"Content-Encoding", "gzip"}},
      {"Content-Encoding", "Content-Length", "X-Mod-Pagespeed"});
  EXPECT_FALSE(capture.needs_late_headers());
  const DaemonHtmlCapture::HeaderLines expected = {
      {"Content-Type", "text/html"},
      {"Content-Length", "13"},
      {"X-Origin", "a"},
      {"cache-control", "private, no-store"},
      {"Set-Cookie", "lane=1"}};
  EXPECT_EQ(expected, capture.headers());
  // Only once.
  capture.ApplyLateHeaders({}, {});
  EXPECT_EQ(expected, capture.headers());
}

TEST_F(DaemonHtmlReportTest, LateVaryIsATokenSetAndEtagLosesTheGzipSuffix) {
  DaemonHtmlReporter reporter(&stats_, &timer_, "", &handler_);
  {
    // What compression adds is no change.
    DaemonHtmlCapture capture(&reporter, abi_.get(), 5 << 20);
    capture.AddHeader("Vary", "User-Agent");
    capture.AddHeader("ETag", "\"abc\"");
    capture.SetBaselineHeaders({{"Vary", "User-Agent"}, {"ETag", "\"abc\""}});
    capture.ApplyLateHeaders(
        {{"Vary", "user-agent,Accept-Encoding"}, {"ETag", "\"abc-gzip\""}}, {});
    const DaemonHtmlCapture::HeaderLines expected = {{"Vary", "User-Agent"},
                                                     {"ETag", "\"abc\""}};
    EXPECT_EQ(expected, capture.headers());
  }
  {
    // Header append Vary Cookie, and a replaced ETag, both count.
    DaemonHtmlCapture capture(&reporter, abi_.get(), 5 << 20);
    capture.AddHeader("Content-Type", "text/html");
    capture.AddHeader("Vary", "User-Agent");
    capture.AddHeader("ETag", "\"abc\"");
    capture.SetBaselineHeaders(
        {{"Vary", "User-Agent, Accept-Encoding"}, {"ETag", "\"abc\""}});
    capture.ApplyLateHeaders({{"Vary", "User-Agent,Accept-Encoding,Cookie"},
                              {"ETag", "\"xyz-gzip\""}},
                             {});
    const DaemonHtmlCapture::HeaderLines expected = {
        {"Content-Type", "text/html"},
        {"ETag", "\"xyz\""},
        {"Vary", "User-Agent, Cookie"}};
    EXPECT_EQ(expected, capture.headers());
  }
  {
    // A token the server took away goes from the origin's Vary.
    DaemonHtmlCapture capture(&reporter, abi_.get(), 5 << 20);
    capture.AddHeader("Vary", "Cookie");
    capture.AddHeader("Vary", "User-Agent");
    capture.SetBaselineHeaders({{"Vary", "Cookie"}, {"Vary", "User-Agent"}});
    capture.ApplyLateHeaders({{"Vary", "User-Agent"}}, {});
    const DaemonHtmlCapture::HeaderLines expected = {{"Vary", "User-Agent"}};
    EXPECT_EQ(expected, capture.headers());
  }
}

TEST_F(DaemonHtmlReportTest, FinishIsOnce) {
  DaemonHtmlReporter reporter(&stats_, &timer_, "", &handler_);
  DaemonHtmlCapture capture(&reporter, abi_.get(), 5 << 20);
  capture.SetRequest("GET", "/", "h");
  capture.Finish(false);
  capture.Finish(false);
  EXPECT_EQ(1, Count("daemon_html_evaluated"));
}

TEST_F(DaemonHtmlReportTest, UnavailableIsItsOwnCounter) {
  DaemonHtmlReporter reporter(&stats_, &timer_, "", &handler_);
  reporter.CountUnavailable();
  EXPECT_EQ(1, Count("daemon_html_unavailable"));
  EXPECT_EQ(0, Count("daemon_html_evaluated"));
}

}  // namespace
}  // namespace net_instaweb
