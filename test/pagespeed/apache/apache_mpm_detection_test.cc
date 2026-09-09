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

// Unit tests for the pure part of Apache MPM threading detection.
//
// These pin the decision IsThreadedFromMpmInfo() makes about a given
// ap_mpm_query() answer.  They deliberately do not exercise
// QueryMpmThreadInfo(), which needs a real Apache runtime -- the tests here
// cover the predicate, not when it is evaluated, and "when" is the harder
// half.  Under a threaded MPM the query only reports the operator's
// ThreadsPerChild once configuration has been processed; the
// queried-too-early row below is what that looks like.

#include "pagespeed/apache/apache_mpm_detection.h"

#include <type_traits>

#include "ap_mpm.h"
#include "pagespeed/apache/apache_rewrite_driver_factory.h"
#include "pagespeed/kernel/base/google_message_handler.h"
#include "pagespeed/kernel/base/stdio_file_system.h"
#include "pagespeed/kernel/base/string.h"
#include "pagespeed/kernel/base/string_util.h"
#include "pagespeed/system/optimization_thread_policy.h"
#include "pagespeed/system/system_rewrite_driver_factory.h"
#include "test/pagespeed/kernel/base/gtest.h"

namespace net_instaweb {
namespace {

MpmThreadInfo MakeInfo(bool query_ok, int is_threaded, int max_threads) {
  MpmThreadInfo info = {query_ok, is_threaded, max_threads};
  return info;
}

MpmProcessInfo MakeProcessInfo(bool query_ok, int max_daemons) {
  MpmProcessInfo info = {query_ok, max_daemons};
  return info;
}

// The event or worker MPM, queried once ThreadsPerChild is known.
TEST(ApacheMpmDetectionTest, ThreadedMpmWithConfiguredThreads) {
  EXPECT_TRUE(IsThreadedFromMpmInfo(MakeInfo(true, AP_MPMQ_STATIC, 25)));
}

// A threaded MPM configured with a single thread per child buys us nothing.
TEST(ApacheMpmDetectionTest, ThreadedMpmWithOneThreadPerChild) {
  EXPECT_FALSE(IsThreadedFromMpmInfo(MakeInfo(true, AP_MPMQ_STATIC, 1)));
}

// The bug in: asked before configuration was processed, a threaded
// MPM reports zero threads.  The predicate is right to call that
// non-threaded -- the fix is to ask later, not to answer differently.
TEST(ApacheMpmDetectionTest, ThreadedMpmQueriedTooEarly) {
  EXPECT_FALSE(IsThreadedFromMpmInfo(MakeInfo(true, AP_MPMQ_STATIC, 0)));
}

// The prefork MPM.
TEST(ApacheMpmDetectionTest, PreforkMpm) {
  EXPECT_FALSE(IsThreadedFromMpmInfo(MakeInfo(true, AP_MPMQ_NOT_SUPPORTED, 1)));
}

// mod_pagespeed loaded before the MPM module: ap_mpm_query() cannot answer.
TEST(ApacheMpmDetectionTest, MpmNotLoadedYet) {
  EXPECT_FALSE(IsThreadedFromMpmInfo(MakeInfo(false, AP_MPMQ_NOT_SUPPORTED, 1)));
}

// An MPM that varies its thread count at runtime.
TEST(ApacheMpmDetectionTest, DynamicallyThreadedMpm) {
  EXPECT_TRUE(IsThreadedFromMpmInfo(MakeInfo(true, AP_MPMQ_DYNAMIC, 25)));
}

// ---------------------------------------------------------------------------
// The process-concurrency divisor Apache supplies.
//
// AP_MPMQ_MAX_DAEMONS is the divisor directly, with no arithmetic of ours:
// prefork answers with MaxRequestWorkers, worker and event answer with
// MaxRequestWorkers / ThreadsPerChild.  What these tests pin is the mapping
// from that answer to the divisor -- in particular that an absent or zero
// answer becomes "unknown", which the policy turns into one thread per pool,
// and never a larger guess.
// ---------------------------------------------------------------------------

// Apache event with the distro default: three children.
TEST(ApacheProcessConcurrencyTest, ConfiguredChildCount) {
  EXPECT_EQ(3, ProcessConcurrencyFromMpmInfo(MakeProcessInfo(true, 3)));
}

// Apache prefork, where the child count is MaxRequestWorkers and is large.
// The divisor is used as given; it is the formula's floor, not a clamp here,
// that keeps such a server at one thread per pool.
TEST(ApacheProcessConcurrencyTest, LargePreforkChildCount) {
  EXPECT_EQ(256, ProcessConcurrencyFromMpmInfo(MakeProcessInfo(true, 256)));
}

TEST(ApacheProcessConcurrencyTest, SingleChild) {
  EXPECT_EQ(1, ProcessConcurrencyFromMpmInfo(MakeProcessInfo(true, 1)));
}

// Asked before httpd's check_config phase computed the child count, the MPM
// answers zero.  Zero is not a number to divide by, and guessing is exactly
// what the policy forbids: an undeterminable divisor must resolve to the
// floor of one thread per pool.
TEST(ApacheProcessConcurrencyTest, ZeroChildrenIsUnknown) {
  EXPECT_EQ(kUnknownProcessConcurrency,
            ProcessConcurrencyFromMpmInfo(MakeProcessInfo(true, 0)));
}

TEST(ApacheProcessConcurrencyTest, NegativeChildCountIsUnknown) {
  EXPECT_EQ(kUnknownProcessConcurrency,
            ProcessConcurrencyFromMpmInfo(MakeProcessInfo(true, -1)));
}

// mod_pagespeed loaded before the MPM module: ap_mpm_query() cannot answer.
TEST(ApacheProcessConcurrencyTest, MpmNotLoadedYetIsUnknown) {
  EXPECT_EQ(kUnknownProcessConcurrency,
            ProcessConcurrencyFromMpmInfo(MakeProcessInfo(false, 8)));
}

// The key itself is the decision -- the divisor is AP_MPMQ_MAX_DAEMONS
// exactly, not a ratio computed from it -- and not an implementation detail
// of QueryMpmProcessInfo().  Every alternative below
// compiles and returns a number, and swapping one in would change what the
// whole policy divides by without failing anything -- so pin it.
TEST(ApacheProcessConcurrencyTest, DivisorComesFromMaxDaemons) {
  EXPECT_EQ(AP_MPMQ_MAX_DAEMONS, kMpmProcessConcurrencyQuery);

  // Named individually so a failure says which one was substituted.
  EXPECT_NE(AP_MPMQ_MAX_DAEMON_USED, kMpmProcessConcurrencyQuery)
      << "the high-water mark of children started, which is 0 at post-config";
  EXPECT_NE(AP_MPMQ_HARD_LIMIT_DAEMONS, kMpmProcessConcurrencyQuery)
      << "the compile-time ceiling, unrelated to the configuration";
  EXPECT_NE(AP_MPMQ_MAX_THREADS, kMpmProcessConcurrencyQuery)
      << "threads per child, not processes -- a different quantity";
}

// ---------------------------------------------------------------------------
// The wiring, not just the mapping.
//
// A previous round of this work removed the Apache-specific overrides and
// every test still passed, because all of them exercised free functions.
// These static assertions fail to compile if ApacheRewriteDriverFactory stops
// declaring the two hooks that make the policy Apache-aware -- deleting either
// one would otherwise leave Apache silently on the base class's "unknown"
// divisor, which is 1 + 1 on every machine.
//
// The trick is that `&Apache::f` has type `int (Apache::*)()` only when Apache
// declares f itself; if it merely inherits, name lookup yields the base
// member and the type is `int (SystemRewriteDriverFactory::*)()`.
// ---------------------------------------------------------------------------

static_assert(
    !std::is_same<decltype(&ApacheRewriteDriverFactory::ConcurrentProcessCount),
                  int (SystemRewriteDriverFactory::*)()>::value,
    "ApacheRewriteDriverFactory must override ConcurrentProcessCount(): "
    "without it Apache falls back to the unknown-divisor floor of one "
    "optimization thread per pool on every machine.");

static_assert(
    !std::is_same<decltype(&ApacheRewriteDriverFactory::ThreadCountsKnownAtInit),
                  bool (SystemRewriteDriverFactory::*)()>::value,
    "ApacheRewriteDriverFactory must override ThreadCountsKnownAtInit(): "
    "ap_mpm_query() cannot report the child count or the threading model "
    "until post-config, so resolving at Init() reads zeroes.");

static_assert(
    !std::is_same<decltype(&ApacheRewriteDriverFactory::IsServerThreaded),
                  bool (SystemRewriteDriverFactory::*)()>::value,
    "ApacheRewriteDriverFactory must override IsServerThreaded().");

// ---------------------------------------------------------------------------
// The call site, not just the hooks.
//
// The overrides above are useless unless something actually runs resolution.
// On Apache that is one hand-written line in the post-config hook, and it is
// the weakest link in the chain: delete it and every unit test still passes,
// all three static assertions still compile, both pools quietly fall back to
// one worker, and -- because resolution never ran -- not even the log line
// that would have said so is emitted.
//
// There is no way to reach pagespeed_post_config() from a unit test (it wants
// a live server_rec chain and an apr pool), so pin the source instead.  It is
// a blunt test, and it is deliberately blunt: what it defends is that the
// call exists at all.
// ---------------------------------------------------------------------------

TEST(ApachePostConfigWiringTest, PostConfigFinalizesThreadCounts) {
  StdioFileSystem file_system;
  GoogleMessageHandler handler;
  GoogleString source;
  const GoogleString path =
      StrCat(GTestSrcDir(), "/pagespeed/apache/mod_instaweb.cc");
  ASSERT_TRUE(file_system.ReadFile(path.c_str(), &source, &handler)) << path;

  // Resolution must run from post-config.
  EXPECT_HAS_SUBSTR("factory->FinalizeThreadCounts();", source)
      << "pagespeed_post_config() must call FinalizeThreadCounts(): "
         "ap_mpm_query() cannot report the MPM's threading model or the "
         "configured child count until the configuration has been processed "
         "(), so nothing else resolves the counts on Apache. "
         "Without this call both optimization pools silently fall back to a "
         "single worker and no resolution is logged at all.";

  // ...and the cache thread limit has to be corrected with the counts that
  // Init() could not know, or SystemCaches keeps the pre-resolution estimate.
  EXPECT_HAS_SUBSTR("caches()->set_thread_limit(", source)
      << "pagespeed_post_config() must update the cache thread limit once the "
         "thread counts are resolved; Init() ran before they were known.";
}

}  // namespace
}  // namespace net_instaweb
