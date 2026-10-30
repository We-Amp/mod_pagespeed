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

// Source pins for the nginx optimization thread counts. The port cannot be
// linked into a unit test (see test/pagespeed/nginx/BUILD), so these read the
// sources; the arithmetic itself is tested in
// test/pagespeed/system/optimization_thread_policy_test.cc.
//
// What they defend: nginx sizes its pools from the core budget divided by
// worker_processes. That only happens if the factory defers resolution past
// its Init() (worker_processes is not final until the whole configuration has
// been read), reports the worker count as its divisor, and ps_init_module
// runs the resolution. Lose any of the three and every worker silently falls
// back to one thread per pool.

#include <cctype>

#include "base/logging.h"
#include "pagespeed/kernel/base/file_system.h"
#include "pagespeed/kernel/base/null_message_handler.h"
#include "pagespeed/kernel/base/stdio_file_system.h"
#include "pagespeed/kernel/base/string.h"
#include "pagespeed/kernel/base/string_util.h"
#include "test/pagespeed/kernel/base/gtest.h"

namespace net_instaweb {
namespace {

GoogleString ReadStrippedSource(const GoogleString& relative_path) {
  StdioFileSystem file_system;
  NullMessageHandler handler;
  GoogleString source;
  const GoogleString path = StrCat(GTestSrcDir(), "/", relative_path);
  CHECK(file_system.ReadFile(path.c_str(), &source, &handler)) << path;
  CHECK(!source.empty()) << path;
  GoogleString out;
  for (const char c : source) {
    if (!isspace(static_cast<unsigned char>(c))) {
      out.push_back(c);
    }
  }
  return out;
}

TEST(NgxThreadCountsPinTest, FactoryDefersResolutionAndReportsWorkers) {
  const GoogleString h =
      ReadStrippedSource("pagespeed/nginx/ngx_rewrite_driver_factory.h");
  EXPECT_HAS_SUBSTR("boolThreadCountsKnownAtInit()override{returnfalse;}", h)
      << "the nginx factory must defer thread-count resolution past Init()";
  EXPECT_HAS_SUBSTR(
      "intConcurrentProcessCount()override{returnworker_process_count_;}", h)
      << "the nginx factory must report the worker count as the divisor";
}

TEST(NgxThreadCountsPinTest, InitModuleResolvesFromWorkerProcesses) {
  const GoogleString cc =
      ReadStrippedSource("pagespeed/nginx/ngx_pagespeed.cc");
  const size_t fn = cc.find("ngx_int_tps_init_module(ngx_cycle_t*cycle){");
  ASSERT_NE(GoogleString::npos, fn) << "ps_init_module was renamed";
  const size_t pinned = cc.find(
      "constboolworkers_pinned=ccf->cpu_affinity_auto||ccf->cpu_affinity_n>0;",
      fn);
  ASSERT_NE(GoogleString::npos, pinned)
      << "with worker_cpu_affinity each worker must keep the floor: the budget "
         "is read in the master, before the workers are pinned";
  const size_t set_count = cc.find(
      "set_worker_process_count(ForkedWorkerProcessCount(static_cast<int>("
      "ccf->worker_processes),ccf->master!=0,workers_pinned));",
      fn);
  ASSERT_NE(GoogleString::npos, set_count)
      << "ps_init_module must set the divisor from worker_processes";
  EXPECT_LT(pinned, set_count);
  const size_t finalize =
      cc.find("driver_factory->FinalizeThreadCounts();", fn);
  ASSERT_NE(GoogleString::npos, finalize)
      << "ps_init_module must resolve the thread counts";
  EXPECT_LT(set_count, finalize)
      << "the divisor must be set before the counts are resolved";
  const size_t limit = cc.find("caches()->set_thread_limit(", fn);
  ASSERT_NE(GoogleString::npos, limit)
      << "the cache thread limit must be updated with the resolved counts";
  EXPECT_LT(finalize, limit);
  // Resolution must come before the server contexts are post-configured and
  // the master forks workers that build their pools.
  const size_t post_config = cc.find("driver_factory->PostConfig(", fn);
  ASSERT_NE(GoogleString::npos, post_config);
  EXPECT_LT(limit, post_config);
}

}  // namespace
}  // namespace net_instaweb
