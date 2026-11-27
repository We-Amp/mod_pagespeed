// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 We-Amp B.V.
//
// The per-request cost of the HTML cache's agent class on the unknown path
// (a browser User-Agent: every table token is scanned for, none matches),
// against the same work with an empty table.  Both include the counter
// increments such a request pays: its class counter, and one more increment
// standing in for the serve scope's pre-gate counter.

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <memory>
#include <vector>

#include "net/instaweb/rewriter/public/rewrite_stats.h"
#include "pagespeed/kernel/base/statistics.h"
#include "pagespeed/kernel/base/string.h"
#include "pagespeed/kernel/base/thread_system.h"
#include "pagespeed/kernel/http/known_agent_table.h"
#include "pagespeed/kernel/util/platform.h"
#include "pagespeed/kernel/util/simple_stats.h"
#include "pagespeed/system/daemon_html_agent_class.h"
#include "test/pagespeed/kernel/base/gtest.h"

namespace net_instaweb {
namespace {

const char* const kBrowsers[] = {
    "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, "
    "like Gecko) Chrome/131.0.0.0 Safari/537.36",
    "Mozilla/5.0 (Macintosh; Intel Mac OS X 10_15_7) AppleWebKit/605.1.15 "
    "(KHTML, like Gecko) Version/18.1 Safari/605.1.15",
    "Mozilla/5.0 (X11; Linux x86_64; rv:133.0) Gecko/20100101 Firefox/133.0",
    "Mozilla/5.0 (iPhone; CPU iPhone OS 18_1 like Mac OS X) AppleWebKit/"
    "605.1.15 (KHTML, like Gecko) Version/18.1 Mobile/15E148 Safari/604.1",
    "Mozilla/5.0 (Linux; Android 14; Pixel 8) AppleWebKit/537.36 (KHTML, like "
    "Gecko) Chrome/131.0.0.0 Mobile Safari/537.36",
    "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, "
    "like Gecko) Chrome/131.0.0.0 Safari/537.36 Edg/131.0.0.0",
    "curl/8.5.0",
    "",
};

constexpr int kRounds = 2000000;

double NsPerRequest(const KnownAgentMatcher* matcher, Statistics* stats,
                    Variable* pregate, int* sink) {
  const size_t n = sizeof(kBrowsers) / sizeof(kBrowsers[0]);
  const auto start = std::chrono::steady_clock::now();
  for (int i = 0; i < kRounds; ++i) {
    const DaemonHtmlAgentClass result = ClassifyDaemonHtmlAgent(
        kBrowsers[i % n], DaemonHtmlAgentSignature::kNone, matcher);
    CountDaemonHtmlAgentClass(result, stats);
    pregate->Add(1);
    *sink += result.agent_class;
  }
  const auto end = std::chrono::steady_clock::now();
  return std::chrono::duration<double, std::nano>(end - start).count() /
         kRounds;
}

TEST(DaemonHtmlAgentClassSpeedTest, UnknownPathAgainstAnEmptyClassifier) {
  std::unique_ptr<ThreadSystem> threads(Platform::CreateThreadSystem());
  SimpleStats stats(threads.get());
  RewriteStats::InitStats(&stats);
  Variable* pregate = stats.AddVariable("pregate_stand_in");
  const KnownAgent unused[] = {{"x", 1, KnownAgentKind::kOther}};
  KnownAgentMatcher empty(unused, 0);
  const KnownAgentMatcher* table = &KnownAgentMatcher::Default();
  int sink = 0;
  // Warm both, then alternate so neither run gets the warmer cache.
  NsPerRequest(&empty, &stats, pregate, &sink);
  NsPerRequest(table, &stats, pregate, &sink);
  std::vector<double> empty_ns, table_ns;
  for (int run = 0; run < 5; ++run) {
    empty_ns.push_back(NsPerRequest(&empty, &stats, pregate, &sink));
    table_ns.push_back(NsPerRequest(table, &stats, pregate, &sink));
  }
  std::sort(empty_ns.begin(), empty_ns.end());
  std::sort(table_ns.begin(), table_ns.end());
  printf("agent class, unknown path, %zu tokens, median of 5 x %d requests:\n"
         "  empty classifier + counters: %.1f ns/request\n"
         "  table classifier + counters: %.1f ns/request\n"
         "  classifier cost:             %.1f ns/request\n",
         table->size(), kRounds, empty_ns[2], table_ns[2],
         table_ns[2] - empty_ns[2]);
  EXPECT_EQ(0, sink);
}

}  // namespace
}  // namespace net_instaweb
