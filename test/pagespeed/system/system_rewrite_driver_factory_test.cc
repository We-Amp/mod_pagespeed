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

// Optimization thread-count resolution, at the factory level.
//
// optimization_thread_policy_test.cc pins the arithmetic.  This file pins how
// the factory feeds that arithmetic and how it reconciles the result with the
// operator's configuration:
//
//   * the process-concurrency divisor defaults to "unknown", and a port that
//     leaves it there gets one thread per pool -- never a larger guess;
//   * an explicitly configured count wins over the computed one in *both*
//     orderings of "detect" and "read the configuration" (ThreadCountsKnown-
//     AtInit() true, as on nginx/IIS/Envoy, and false, as on Apache, where the
//     MPM cannot be queried until post-config);
//   * `auto` and its alias `0` mean "apply the policy", also in both
//     orderings;
//   * a negative count is rejected at parse time, naming the directive.
//
// The deployment shapes below are the consequences table driven through the
// factory rather than the formula, so a change to either layer fails here.

#include "pagespeed/system/system_rewrite_driver_factory.h"

#include <algorithm>
#include <memory>

#include "net/instaweb/rewriter/public/process_context.h"
#include "net/instaweb/rewriter/public/rewrite_options.h"
#include "pagespeed/kernel/base/google_message_handler.h"
#include "pagespeed/kernel/base/message_handler.h"
#include "pagespeed/kernel/base/null_message_handler.h"
#include "pagespeed/kernel/base/string.h"
#include "pagespeed/kernel/base/string_util.h"
#include "pagespeed/system/optimization_thread_policy.h"
#include "pagespeed/system/system_thread_system.h"
#include "test/pagespeed/kernel/base/gtest.h"

namespace net_instaweb {
namespace {

// A SystemRewriteDriverFactory with just enough implemented to construct, so
// thread-count resolution can be exercised without a real server.  The
// deployment shape -- process concurrency, threading model, and whether the
// answers are available at Init() -- are test knobs.
class ThreadCountTestFactory : public SystemRewriteDriverFactory {
 public:
  ThreadCountTestFactory(const ProcessContext& process_context,
                         SystemThreadSystem* thread_system)
      : SystemRewriteDriverFactory(process_context, thread_system,
                                   nullptr /* shared_mem_runtime */,
                                   "localhost", 80),
        threaded_(false),
        known_at_init_(true),
        concurrent_processes_(kUnknownProcessConcurrency),
        override_divisor_(false) {}

  // Test knobs.
  void set_threaded(bool x) { threaded_ = x; }
  void set_known_at_init(bool x) { known_at_init_ = x; }
  void set_concurrent_processes(int x) {
    concurrent_processes_ = x;
    override_divisor_ = true;
  }

  bool IsServerThreaded() override { return threaded_; }
  bool ThreadCountsKnownAtInit() override { return known_at_init_; }
  int ConcurrentProcessCount() override {
    // Without set_concurrent_processes() this falls through to the base
    // class, which is what a port that has not implemented the divisor gets.
    return override_divisor_
               ? concurrent_processes_
               : SystemRewriteDriverFactory::ConcurrentProcessCount();
  }

  using SystemRewriteDriverFactory::thread_counts_finalized;

  // Unused, but pure virtual.
  UrlAsyncFetcher* DefaultAsyncUrlFetcher() override { return nullptr; }
  UrlAsyncFetcher* AllocateFetcher(SystemRewriteOptions* config) override {
    return nullptr;
  }
  MessageHandler* DefaultHtmlParseMessageHandler() override {
    return new GoogleMessageHandler;
  }
  MessageHandler* DefaultMessageHandler() override {
    return new GoogleMessageHandler;
  }
  ServerContext* NewDecodingServerContext() override { return nullptr; }
  void NonStaticInitStats(Statistics* statistics) override {
    InitStats(statistics);
  }

 private:
  bool threaded_;
  bool known_at_init_;
  int concurrent_processes_;
  bool override_divisor_;
};

class SystemThreadCountTest : public testing::Test {
 protected:
  void SetUp() override {
    RewriteOptions::Initialize();
    process_context_ = std::make_unique<ProcessContext>();
    // The factory takes ownership of the thread system.
    factory_ = std::make_unique<ThreadCountTestFactory>(*process_context_,
                                                        new SystemThreadSystem);
  }

  void TearDown() override {
    if (inited_) {
      // SystemCaches, created by Init(), DCHECKs that it was shut down.
      factory_->ShutDown();
    }
    factory_.reset();
    process_context_.reset();
    RewriteOptions::Terminate();
  }

  void Init() {
    factory_->Init();
    // RewriteDriverFactory::ShutDown() reads timer_ directly rather than
    // through the lazy timer() accessor, so a factory that never touched the
    // timer crashes on shutdown.  Real servers always have by then; force it
    // here so TearDown() is safe.
    factory_->timer();
    inited_ = true;
  }

  // Applies a process-scope directive the way a config file would.
  void SetDirective(const char* name, const char* value) {
    GoogleString msg;
    NullMessageHandler handler;
    EXPECT_EQ(RewriteOptions::kOptionOk,
              factory_->ParseAndSetOption1(name, value,
                                           true /* process_scope */, &msg,
                                           &handler))
        << name << " " << value << ": " << msg;
  }

  // Applies a directive that is expected to be rejected, returning the
  // operator-facing message.
  GoogleString SetInvalidDirective(const char* name, const char* value) {
    GoogleString msg;
    NullMessageHandler handler;
    EXPECT_EQ(RewriteOptions::kOptionValueInvalid,
              factory_->ParseAndSetOption1(name, value,
                                           true /* process_scope */, &msg,
                                           &handler))
        << name << " " << value << " was accepted";
    return msg;
  }

  // The shapes below are stated in effective cores, which the factory reads
  // from the machine.  Rather than pretend to control that, compute what the
  // policy says for the real host and compare the factory against it; the
  // per-shape numbers themselves are pinned in
  // optimization_thread_policy_test.cc.
  OptimizationThreadCounts Expected(int concurrent_processes) {
    return ComputeOptimizationThreadCounts(
        factory_->cpu_budget().effective_cores, concurrent_processes);
  }

  std::unique_ptr<ProcessContext> process_context_;
  std::unique_ptr<ThreadCountTestFactory> factory_;
  bool inited_ = false;
};

// ---------------------------------------------------------------------------
// The divisor defaults to "unknown", and unknown means one thread per pool.
// ---------------------------------------------------------------------------

// The default the base class ships.  Every port that has not implemented the
// divisor yet lands here, and lands on 1 + 1 -- deliberately, because
// oversubscribing a machine silently is the failure mode the policy exists to
// end.
TEST_F(SystemThreadCountTest, UnknownDivisorResolvesToOneOfEach) {
  Init();

  EXPECT_TRUE(factory_->thread_counts_finalized());
  EXPECT_EQ(kUnknownProcessConcurrency, factory_->concurrent_process_count());
  EXPECT_EQ(1, factory_->num_rewrite_threads());
  EXPECT_EQ(1, factory_->num_expensive_rewrite_threads());
}

// A threaded server with an unknown divisor is still 1 + 1: under this policy
// the threading model is not an input to the count, only to the log line.
TEST_F(SystemThreadCountTest, ThreadingModelDoesNotChangeTheCount) {
  factory_->set_threaded(true);
  Init();

  EXPECT_EQ(1, factory_->num_rewrite_threads());
  EXPECT_EQ(1, factory_->num_expensive_rewrite_threads());
}

// The CPU budget is read from the machine, never fabricated, and never zero.
TEST_F(SystemThreadCountTest, CpuBudgetIsPopulatedAtFinalization) {
  factory_->set_concurrent_processes(1);
  Init();

  EXPECT_GE(factory_->cpu_budget().effective_cores, 1);
  EXPECT_EQ(1, factory_->concurrent_process_count());
}

// ---------------------------------------------------------------------------
// The deployment shapes, driven through the factory.
// ---------------------------------------------------------------------------

TEST_F(SystemThreadCountTest, SingleProcessGetsTheWholeShare) {
  factory_->set_concurrent_processes(1);
  Init();

  OptimizationThreadCounts expected = Expected(1);
  EXPECT_EQ(expected.rewrite, factory_->num_rewrite_threads());
  EXPECT_EQ(expected.expensive, factory_->num_expensive_rewrite_threads());
  // The expensive pool is never larger than the rewrite pool: heavy image
  // compute must not starve the rest.
  EXPECT_LE(factory_->num_expensive_rewrite_threads(),
            factory_->num_rewrite_threads());
}

// Heavily forked servers: the per-process floor is what they get, on any
// machine this test could run on.
TEST_F(SystemThreadCountTest, HeavilyForkedServerFallsToTheFloor) {
  factory_->set_concurrent_processes(1024);
  Init();

  EXPECT_EQ(1, factory_->num_rewrite_threads());
  EXPECT_EQ(1, factory_->num_expensive_rewrite_threads());
}

// Doubling the process count can only lower the per-process budget.  This is
// the property that bounds the aggregate, expressed without depending on the
// test machine's core count.
TEST_F(SystemThreadCountTest, MoreProcessesNeverMeansMoreThreadsEach) {
  factory_->set_concurrent_processes(1);
  Init();
  const int one_process = factory_->num_rewrite_threads();

  EXPECT_LE(Expected(2).rewrite, one_process);
  EXPECT_LE(Expected(8).rewrite, Expected(2).rewrite);
}

// ---------------------------------------------------------------------------
// ThreadCountsKnownAtInit() == true: nginx, IIS and Envoy.  Resolution runs
// from Init(), before the configuration is read, so a directive arrives after
// the counts were already resolved.
// ---------------------------------------------------------------------------

TEST_F(SystemThreadCountTest, ExplicitCountsWinWhenSetAfterResolution) {
  factory_->set_concurrent_processes(1);
  Init();
  SetDirective("NumRewriteThreads", "7");
  SetDirective("NumExpensiveRewriteThreads", "3");

  EXPECT_EQ(7, factory_->num_rewrite_threads());
  EXPECT_EQ(3, factory_->num_expensive_rewrite_threads());
}

// `0` is documented as auto-detect; before this change, set after
// resolution it stuck as a literal 0 and produced a pool that accepts work and
// never runs it.  It must resolve to the policy's answer in this ordering too.
TEST_F(SystemThreadCountTest, ZeroMeansAutoWhenSetAfterResolution) {
  factory_->set_concurrent_processes(1);
  Init();
  OptimizationThreadCounts expected = Expected(1);
  SetDirective("NumRewriteThreads", "0");
  SetDirective("NumExpensiveRewriteThreads", "0");

  EXPECT_EQ(expected.rewrite, factory_->num_rewrite_threads());
  EXPECT_EQ(expected.expensive, factory_->num_expensive_rewrite_threads());
}

TEST_F(SystemThreadCountTest, AutoLiteralMeansAutoWhenSetAfterResolution) {
  factory_->set_concurrent_processes(1);
  Init();
  OptimizationThreadCounts expected = Expected(1);
  SetDirective("NumRewriteThreads", "auto");
  SetDirective("NumExpensiveRewriteThreads", "AUTO");

  EXPECT_EQ(expected.rewrite, factory_->num_rewrite_threads());
  EXPECT_EQ(expected.expensive, factory_->num_expensive_rewrite_threads());
}

// Setting a count and then putting it back to auto returns the computed
// value, rather than leaving the explicit one latched.
TEST_F(SystemThreadCountTest, ExplicitThenAutoReturnsToThePolicy) {
  factory_->set_concurrent_processes(1);
  Init();
  OptimizationThreadCounts expected = Expected(1);
  SetDirective("NumRewriteThreads", "9");
  EXPECT_EQ(9, factory_->num_rewrite_threads());
  SetDirective("NumRewriteThreads", "auto");

  EXPECT_EQ(expected.rewrite, factory_->num_rewrite_threads());
}

// ---------------------------------------------------------------------------
// ThreadCountsKnownAtInit() == false: Apache.  Init() leaves the counts
// unresolved, the configuration is read, and FinalizeThreadCounts() runs from
// post-config -- so a directive arrives *before* resolution.
// ---------------------------------------------------------------------------

TEST_F(SystemThreadCountTest, DeferredInitLeavesCountsUnresolved) {
  factory_->set_known_at_init(false);
  factory_->set_concurrent_processes(2);
  Init();

  EXPECT_FALSE(factory_->thread_counts_finalized());

  factory_->FinalizeThreadCounts();
  EXPECT_TRUE(factory_->thread_counts_finalized());
  OptimizationThreadCounts expected = Expected(2);
  EXPECT_EQ(expected.rewrite, factory_->num_rewrite_threads());
  EXPECT_EQ(expected.expensive, factory_->num_expensive_rewrite_threads());
}

TEST_F(SystemThreadCountTest, ExplicitCountsWinWhenSetBeforeResolution) {
  factory_->set_known_at_init(false);
  factory_->set_concurrent_processes(1);
  Init();
  SetDirective("NumRewriteThreads", "7");
  SetDirective("NumExpensiveRewriteThreads", "3");
  factory_->FinalizeThreadCounts();

  EXPECT_EQ(7, factory_->num_rewrite_threads());
  EXPECT_EQ(3, factory_->num_expensive_rewrite_threads());
}

// One directive set, the other left to the policy.
TEST_F(SystemThreadCountTest, PartiallyExplicitCountsMixWithThePolicy) {
  factory_->set_known_at_init(false);
  factory_->set_concurrent_processes(1);
  Init();
  SetDirective("NumRewriteThreads", "6");
  factory_->FinalizeThreadCounts();

  OptimizationThreadCounts expected = Expected(1);
  EXPECT_EQ(6, factory_->num_rewrite_threads());
  EXPECT_EQ(expected.expensive, factory_->num_expensive_rewrite_threads());
}

TEST_F(SystemThreadCountTest, ZeroMeansAutoWhenSetBeforeResolution) {
  factory_->set_known_at_init(false);
  factory_->set_concurrent_processes(1);
  Init();
  SetDirective("NumRewriteThreads", "0");
  SetDirective("NumExpensiveRewriteThreads", "auto");
  factory_->FinalizeThreadCounts();

  OptimizationThreadCounts expected = Expected(1);
  EXPECT_EQ(expected.rewrite, factory_->num_rewrite_threads());
  EXPECT_EQ(expected.expensive, factory_->num_expensive_rewrite_threads());
}

// ---------------------------------------------------------------------------
// Negative counts are rejected at parse time.
// ---------------------------------------------------------------------------

// A negative count used to convert to size_t as SIZE_MAX on its way to the
// worker pool -- unbounded thread creation, ending in a CHECK failure at child
// init in the rate controller.  It must not reach the pool at all, and the
// message must name the directive so the operator can find the line.
TEST_F(SystemThreadCountTest, NegativeRewriteThreadsIsRejected) {
  Init();
  GoogleString msg = SetInvalidDirective("NumRewriteThreads", "-1");
  EXPECT_TRUE(msg.find("NumRewriteThreads") != GoogleString::npos) << msg;

  // The rejected value never lands.
  EXPECT_EQ(1, factory_->num_rewrite_threads());
}

TEST_F(SystemThreadCountTest, NegativeExpensiveRewriteThreadsIsRejected) {
  Init();
  GoogleString msg =
      SetInvalidDirective("NumExpensiveRewriteThreads", "-1000000");
  EXPECT_TRUE(msg.find("NumExpensiveRewriteThreads") != GoogleString::npos)
      << msg;

  EXPECT_EQ(1, factory_->num_expensive_rewrite_threads());
}

TEST_F(SystemThreadCountTest, NonNumericThreadCountIsRejected) {
  Init();
  GoogleString msg = SetInvalidDirective("NumRewriteThreads", "many");
  EXPECT_TRUE(msg.find("NumRewriteThreads") != GoogleString::npos) << msg;
}

// ---------------------------------------------------------------------------
// An explicit count is an override, but not an unbounded one.
// ---------------------------------------------------------------------------

// A mistyped count -- an extra digit, a value meant for a byte size -- used to
// reach QueuedWorkerPool verbatim, which is precisely the silent
// oversubscription the policy exists to end.  It is clamped, and the operator
// is told, rather than rejected: an override that is merely too large is still
// an override.
TEST_F(SystemThreadCountTest, AbsurdlyLargeThreadCountIsClamped) {
  Init();
  SetDirective("NumRewriteThreads", "2000000000");
  EXPECT_EQ(kMaxOptimizationThreadsPerPool, factory_->num_rewrite_threads());

  SetDirective("NumExpensiveRewriteThreads", "2000000000");
  EXPECT_EQ(kMaxOptimizationThreadsPerPool,
            factory_->num_expensive_rewrite_threads());
}

TEST_F(SystemThreadCountTest, LargeButPlausibleThreadCountIsKept) {
  Init();
  SetDirective("NumRewriteThreads", "64");
  EXPECT_EQ(64, factory_->num_rewrite_threads());
}

// kAutoThreadCount must be usable as a value, not only as a compile-time
// literal: passing it to EXPECT_EQ (or to std::max) is an ODR use, which a
// `static const int` with no out-of-class definition fails to link.
TEST_F(SystemThreadCountTest, AutoThreadCountConstantIsLinkable) {
  Init();  // The fixture's teardown assumes a factory that was initialized.
  EXPECT_EQ(0, SystemRewriteDriverFactory::kAutoThreadCount);
  EXPECT_EQ(SystemRewriteDriverFactory::kAutoThreadCount,
            std::max(SystemRewriteDriverFactory::kAutoThreadCount, 0));
}

}  // namespace
}  // namespace net_instaweb
