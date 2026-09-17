// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 We-Amp B.V.
//
// Fully-instrumented concurrent repro — an authoritative
// ThreadSanitizer verdict on the shutdown logging race.
//
// The other two shutdown tests cannot give TSan a verdict:
//   - logging_shutdown_test.cc is single-threaded (flag short-circuit only).
//   - logging_shutdown_race_test.cc synchronizes the fatal interleaving with
//     condvars (park-inside-sink), so every access is ordered by
//     happens-before edges and TSan sees no race BY CONSTRUCTION. It is an
//     ASan repro (use-after-free), not a race repro.
// The in-server evidence (stress harness under TSan) is low-confidence too:
// Apache forks after going multi-threaded (die_after_fork=0, best-effort
// tracking) and apache/APR/libc are uninstrumented.
//
// This test runs in a plain, fully-instrumented single process — no fork, no
// LD_PRELOAD — and genuinely RACES worker threads doing LOG() against the
// teardown that production performs during static destruction:
// spdlog::shutdown() (the registry dropping its strong logger references —
// what the registry's own static destructor does at exit) followed by
// ShutDownLogging(). There is deliberately NO synchronization between the
// workers and the teardown; the only ordering is the atomic
// g_logging_shutdown flag and spdlog's own internals, exactly as in prod.
//
// Before the fix (LogMessage re-fetched spdlog's registry default logger on
// every
// call): the registry teardown destroys the logger object while a worker is
// dereferencing it -> TSan reports a data race / heap-use-after-free.
// With the fix (immortal held logger + atomic flag): every worker access lands
// on process-immortal memory or the atomic -> TSan-clean.
//
// The CSS kClass/kId statics (the other half of that immortalization) are NOT
// exercised here: pre-fix they were destroyed only by real exit()-time
// static destruction, which an in-process test cannot race against without
// terminating itself. That path is covered by the ASan stress harness
// (tools/stress/).
//
// This is intentionally its OWN test binary: spdlog::shutdown() and
// ShutDownLogging() are sticky process-wide, so it must not share a process
// with tests that expect a live logging path (base_test runs shard_count=1).
//
// Run:
//   bazel test --config=clang-tsan \
//     //test/pagespeed/kernel/base:logging_shutdown_tsan_race_test \
//     --test_output=all
// CI runs this automatically: the tsan and asan jobs in the CI workflow both test
// //test/pagespeed/kernel/... and override TSAN_OPTIONS with a
// workspace-relative suppressions path. On a bare-metal manual run, note
// that .bazelrc's clang-tsan config hardcodes the build-image path
// /src/tools/tsan_suppressions.txt (TSan aborts at startup if it is
// missing) — override it, e.g.:
//   --test_env="TSAN_OPTIONS=report_atomic_races=0:detect_deadlocks=0:\
//     suppressions=$PWD/tools/tsan_suppressions.txt"
// (Also passes under ASan and plain builds.)

#include <atomic>
#include <cstdint>
#include <thread>
#include <vector>

#include "base/logging.h"
#include "spdlog/spdlog.h"
#include "test/pagespeed/kernel/base/gtest.h"

namespace {

TEST(LoggingShutdownTsanRaceTest, ConcurrentLogVsRegistryTeardown) {
  // Warmup: the first LOG() makes the fixed build capture (and leak) its
  // immortal held logger, modeling prod where the module logs long before
  // shutdown. Then mute the logger: should_log() still dereferences the
  // logger object on every call (the access TSan must see), but the test
  // log stays readable.
  LOG(INFO) << "tsan-race warmup";
  spdlog::default_logger()->set_level(spdlog::level::off);

  std::atomic<bool> stop{false};
  std::atomic<int64_t> iterations{0};

  constexpr int kWorkers = 4;
  std::vector<std::thread> workers;
  workers.reserve(kWorkers);
  for (int i = 0; i < kWorkers; ++i) {
    workers.emplace_back([&stop, &iterations] {
      while (!stop.load(std::memory_order_relaxed)) {
        // Models InPlaceRewriteContext::Harvest -> ~LogMessage -> spdlog.
        LOG(INFO) << "worker LOG in flight during teardown";
        iterations.fetch_add(1, std::memory_order_relaxed);
      }
    });
  }

  // Wait until the workers are demonstrably pumping the LOG() path.
  while (iterations.load(std::memory_order_relaxed) < 1000) {
    std::this_thread::yield();
  }

  // Teardown, unsynchronized with the workers. spdlog::shutdown() is what
  // the registry's static destructor performs at exit(): it drops the
  // registry's strong references. Pre-fix that destroys the logger under
  // the workers' feet; post-fix the held logger is immortal.
  spdlog::shutdown();

  // Keep racing on the torn-down registry state before the flag flips:
  // this is the exact prod crash window (flag not yet visible to a
  // worker mid-LOG).
  int64_t seen = iterations.load(std::memory_order_relaxed);
  while (iterations.load(std::memory_order_relaxed) < seen + 1000) {
    std::this_thread::yield();
  }

  pagespeed_logging::ShutDownLogging();

  // And a short tail through the post-flag stderr path.
  seen = iterations.load(std::memory_order_relaxed);
  while (iterations.load(std::memory_order_relaxed) < seen + 100) {
    std::this_thread::yield();
  }

  stop.store(true, std::memory_order_relaxed);
  for (auto& t : workers) {
    t.join();
  }
  SUCCEED();
}

}  // namespace
