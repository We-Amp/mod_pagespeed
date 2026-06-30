// Copyright (c) 2026 We-Amp B.V.
//
// TDD reproduction for the RESIDUAL shutdown logging race.
//
// logging_shutdown_test.cc only exercises the g_logging_shutdown==true
// short-circuit. This test reproduces the ACTUAL production crash window, where
// the flag is STILL FALSE and a worker thread is in-flight inside
// spdlog::logger::sink_it_() when spdlog's logger registry is torn down
// (static-destruction-order fiasco). It models the prod stack:
//   InPlaceRewriteContext::Harvest -> ~LogMessage -> spdlog::logger::sink_it_ -> UAF
//
// Mechanism (no sleeps; two one-shot condvar sync points):
//   1. WARMUP: main does one LOG(INFO) so a FIXED build captures/holds its
//      spdlog logger reference (models prod's first-LOG-constructs-the-singleton).
//   2. Install a BlockingSink onto the EXISTING default logger's sink vector via
//      a TEMPORARY default_logger() handle (never retained -> does not mask the
//      pre-fix UAF). We keep a strong ref to the SINK ONLY, so the use-after-free
//      lands on the logger object (matching prod), not the sink.
//   3. A worker thread calls LOG(INFO) (flag stays FALSE). It enters ~LogMessage,
//      calls spdlog::info() -> default_logger_raw()->log() -> sink_it_, signals
//      `entered`, and PARKS holding the raw default-logger pointer.
//   4. Main waits for `entered` (worker provably mid-spdlog), then calls
//      spdlog::shutdown() -> registry drops the only strong refs -> the logger
//      object is freed underneath the parked worker. g_logging_shutdown stays FALSE.
//   5. Main releases the worker. On resume it returns up through
//      spdlog::logger::sink_it_(), reading freed logger memory -> ASan
//      heap-use-after-free (PRE-fix).
//
// With the fix (base/logging.cc holds an immortal leaked logger and routes
// ~LogMessage through it) the logger object outlives spdlog::shutdown(), the
// worker resumes on live memory, joins cleanly, and the test SUCCEEDs.
//
// Run:
//   bazel test --config=clang-asan //test/pagespeed/kernel/base:base_test \
//     --test_filter='LoggingShutdownRaceTest.*' --test_output=all

#include <condition_variable>
#include <memory>
#include <mutex>
#include <thread>

#include "base/logging.h"
#include "spdlog/sinks/base_sink.h"
#include "spdlog/spdlog.h"
#include "test/pagespeed/kernel/base/gtest.h"

namespace {

// Dependency-free one-shot sync point (mutex + condvar + bool).
struct SyncPoint {
  std::mutex mu;
  std::condition_variable cv;
  bool fired = false;
  void Notify() {
    std::lock_guard<std::mutex> l(mu);
    fired = true;
    cv.notify_all();
  }
  void Wait() {
    std::unique_lock<std::mutex> l(mu);
    cv.wait(l, [this] { return fired; });
  }
};

// Parks the calling thread INSIDE spdlog::logger::sink_it_() until released, so
// the main thread can free the logger underneath it.
class BlockingSink : public spdlog::sinks::base_sink<std::mutex> {
 public:
  BlockingSink(SyncPoint* entered, SyncPoint* release)
      : entered_(entered), release_(release) {}

 protected:
  void sink_it_(const spdlog::details::log_msg&) override {
    entered_->Notify();
    release_->Wait();
    // Touch nothing after the wait: control returns up into
    // spdlog::logger::sink_it_(), which is where the freed-logger UAF lands.
  }
  void flush_() override {}

 private:
  SyncPoint* entered_;
  SyncPoint* release_;
};

TEST(LoggingShutdownRaceTest, WorkerSpdlogCallSurvivesRegistryTeardown) {
  // (1) Warmup so a fixed build captures its held logger reference.
  LOG(INFO) << "warmup";

  SyncPoint entered;
  SyncPoint release;

  // (2) Install the blocking sink on the EXISTING default logger. Keep a strong
  // ref to the SINK only; NEVER retain the default LOGGER shared_ptr (that would
  // keep the logger alive and mask the pre-fix UAF).
  auto blocking_sink = std::make_shared<BlockingSink>(&entered, &release);
  spdlog::default_logger()->sinks().push_back(blocking_sink);

  // (3) Worker logs while g_logging_shutdown stays FALSE; parks inside sink_it_.
  std::thread worker([] { LOG(INFO) << "in-flight during teardown"; });

  // (4) Worker is provably inside spdlog; tear down the registry, freeing the
  // logger object (pre-fix: while the worker still holds its raw pointer).
  entered.Wait();
  spdlog::shutdown();

  // (5) Release the parked worker; pre-fix it returns into a freed logger.
  release.Notify();
  worker.join();

  SUCCEED();
}

}  // namespace
