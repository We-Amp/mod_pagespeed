// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

// The decision log writer thread is one per process and starts with the first
// line, so this runs in a binary of its own: no earlier test may start it.

#include <cstdint>
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
#include "pagespeed/system/daemon_html_report.h"
#include "pagespeed/system/rule_pack_decision_log.h"
#include "test/pagespeed/kernel/base/mock_timer.h"
#include "test/pagespeed/system/daemon_stub_test_util.h"

namespace net_instaweb {
namespace {

TEST(DaemonHtmlReportWriterStartTest, FailedStartDropsTheLineAndRetries) {
  std::unique_ptr<ThreadSystem> thread_system(Platform::CreateThreadSystem());
  SimpleStats stats(thread_system.get());
  RewriteStats::InitStats(&stats);
  MockTimer timer(new NullMutex, 1777777777000LL);
  GoogleMessageHandler handler;
  const std::filesystem::path dir =
      std::filesystem::path(testing::TempDir()) / "daemon_html_writer_start";
  std::filesystem::remove_all(dir);
  GoogleString error;
  std::unique_ptr<DaemonAbi> abi(
      LoadDaemonAbi(StubLibraryPath("libdaemon_stub_html_policy.so"), &error));
  ASSERT_TRUE(abi != nullptr) << error;
  unsetenv("PS_STUB_HTML_ACTION");
  unsetenv("PS_STUB_HTML_REASON");

  DaemonHtmlReporter reporter(&stats, &timer, dir.string(), &handler);
  auto run = [&](const char* path) {
    DaemonHtmlCapture capture(&reporter, abi.get(), 5 << 20);
    capture.SetRequest("GET", path, "www.example.com");
    capture.set_status(200);
    capture.AddHeader("Content-Type", "text/html");
    capture.AddBody("<html></html>", 13);
    capture.set_body_ended();
    capture.Finish(false);
  };
  auto lines = [&]() {
    DaemonHtmlReporter::WaitForLogWritesForTesting();
    std::vector<std::string> out;
    std::ifstream in(dir / "daemon-html" /
                     RulePackDecisionLog::FileName(
                         RulePackDecisionLog::UtcDate(timer.NowMs())));
    for (std::string line; std::getline(in, line);) out.push_back(line);
    return out;
  };

  DaemonHtmlReporter::FailLogWriterStartForTesting(true);
  run("/first.html");
  EXPECT_EQ(1, stats.GetVariable("daemon_html_evaluated")->Get());
  EXPECT_EQ(1, stats.GetVariable("daemon_html_would_store")->Get());
  EXPECT_EQ(1, stats.GetVariable("daemon_html_log_dropped")->Get());
  EXPECT_FALSE(std::filesystem::exists(dir / "daemon-html"));

  // The failed start left the writer unstarted, so the next line starts it.
  DaemonHtmlReporter::FailLogWriterStartForTesting(false);
  run("/second.html");
  const std::vector<std::string> written = lines();
  ASSERT_EQ(1u, written.size());
  EXPECT_NE(std::string::npos, written[0].find("path=/second.html"));
  EXPECT_EQ(1, stats.GetVariable("daemon_html_log_dropped")->Get());
  std::filesystem::remove_all(dir);
}

}  // namespace
}  // namespace net_instaweb
