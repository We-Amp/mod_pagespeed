/*
 * Licensed to the Apache Software Foundation (ASF) under one
 * or more contributor license agreements.  See the NOTICE file
 * distributed with this work for additional information
 * regarding copyright ownership.  The ASF licenses this file
 * to you under the Apache License, Version 2.0 (the
 * "License"); you may not use this file except in compliance
 * with the License.  You may obtain a copy of the License at
 *
 *   http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing,
 * software distributed under the License is distributed on an
 * "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
 * KIND, either express or implied.  See the License for the
 * specific language governing permissions and limitations
 * under the License.
 */

// A byte-exact capture of the statistics JSON the admin console consumes,
// with the daemon-serve counter split and the statistics start-time gauge
// in it.  Deterministic by construction: DumpJson takes the timestamp as an
// argument, so the golden needs no textual substitution.  The console's
// fixtures copy this file verbatim.

#include <cstdio>
#include <fstream>
#include <iterator>
#include <memory>

#include "net/instaweb/rewriter/public/rewrite_stats.h"
#include "pagespeed/kernel/base/null_message_handler.h"
#include "pagespeed/kernel/base/string_writer.h"
#include "pagespeed/kernel/util/platform.h"
#include "pagespeed/kernel/util/simple_stats.h"
#include "test/pagespeed/kernel/base/gtest.h"

namespace net_instaweb {
namespace {

TEST(StatsJsonDumpTest, ShapeIsStable) {
  std::unique_ptr<ThreadSystem> thread_system(Platform::CreateThreadSystem());
  SimpleStats stats(thread_system.get());
  RewriteStats::InitStats(&stats);

  // The numbers tell the story the console explains: an optimizer that
  // served nearly every optimizable request, against a fall-through total
  // full of types it never handles.  Every seeded name is registered by
  // RewriteStats::InitStats — the dump covers exactly that set; per-filter
  // counters (the CSS filter's bytes-saved, say) are registered by their
  // own filters' InitStats and are NOT in it, so seeding one here would
  // CHECK-abort before the golden comparison ever runs.
  stats.GetVariable("ipro_daemon_served")->Add(1151);
  stats.GetVariable("ipro_daemon_fallthrough")->Add(8847);
  stats.GetVariable("ipro_daemon_served_css")->Add(345);
  stats.GetVariable("ipro_daemon_served_js")->Add(239);
  stats.GetVariable("ipro_daemon_served_image")->Add(567);
  stats.GetVariable("ipro_daemon_served_other")->Add(0);
  stats.GetVariable("ipro_daemon_fallthrough_css")->Add(2);
  stats.GetVariable("ipro_daemon_fallthrough_js")->Add(1);
  stats.GetVariable("ipro_daemon_fallthrough_image")->Add(3);
  stats.GetUpDownCounter(RewriteStats::kProcessStartMs)->Set(1759200000000);

  NullMessageHandler handler;
  GoogleString body;
  StringWriter writer(&body);
  stats.DumpJson("global", "example.com:80", 1759230000000, &writer, &handler);

  // The gauge rides in both the variables map and the gauges list.
  const size_t gauge = body.find("\"process_start_ms\": 1759200000000");
  ASSERT_NE(gauge, GoogleString::npos);
  const size_t gauges = body.find("\"gauges\": [");
  ASSERT_NE(gauges, GoogleString::npos);
  EXPECT_NE(GoogleString::npos, body.find("\"process_start_ms\"", gauges))
      << "the start-time gauge must be listed as a gauge, not differenced";

  std::ifstream golden_file(
      "test/pagespeed/system/testdata/stats_json.global.golden",
      std::ios::binary);
  ASSERT_TRUE(golden_file.is_open()) << "golden missing (BUILD data dep?)";
  const std::string golden((std::istreambuf_iterator<char>(golden_file)),
                           std::istreambuf_iterator<char>());
  EXPECT_EQ(golden, body + "\n") << "---- actual stats_json ----\n"
                                 << body << "\n---- end ----";
}

}  // namespace
}  // namespace net_instaweb
