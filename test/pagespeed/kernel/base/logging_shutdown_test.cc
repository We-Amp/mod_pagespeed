// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.
//
// Test that LOG() calls are safe after ShutDownLogging() is called.
// This reproduces the crash where worker threads call LOG() during
// Apache process shutdown after spdlog's global logger is destroyed
// (static destruction order fiasco).
//
// The test calls ShutDownLogging() to simulate the shutdown state,
// then verifies that LOG() calls at all severity levels succeed
// without crashing. With the fix, they write to stderr instead of
// spdlog.

#include "base/logging.h"

#include "test/pagespeed/kernel/base/gtest.h"

namespace {

TEST(LoggingShutdownTest, LogAfterShutdownDoesNotCrash) {
  // Simulate the shutdown state — after this call, LOG() should
  // use fprintf(stderr) instead of spdlog.
  pagespeed_logging::ShutDownLogging();

  // These LOG() calls would crash (SIGSEGV in spdlog::logger::sink_it_)
  // without the fix, because ShutDownLogging sets the flag that makes
  // spdlog's static destruction order irrelevant.
  LOG(INFO) << "This should not crash (INFO after shutdown)";
  LOG(WARNING) << "This should not crash (WARNING after shutdown)";
  LOG(ERROR) << "This should not crash (ERROR after shutdown)";

  // If we get here without crashing, the test passes.
  SUCCEED();
}

}  // namespace
