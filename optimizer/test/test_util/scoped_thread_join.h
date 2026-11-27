// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

#ifndef TEST_TEST_UTIL_SCOPED_THREAD_JOIN_H_
#define TEST_TEST_UTIL_SCOPED_THREAD_JOIN_H_

#include <functional>
#include <thread>
#include <utility>

namespace pagespeed::test {

// Asks a helper thread to stop and joins it on scope exit.
//
// Load-bearing wherever a test body uses a FATAL assertion (ASSERT_*) while a
// helper std::thread is running: a fatal assertion returns from the test body
// immediately, so a trailing stop/join never executes and a still-joinable
// std::thread is destroyed -- which calls std::terminate and takes the whole
// test binary, and every test after it, down with one failed expectation.
// With this, a failing assertion reports as a failing assertion.
//
// Declare it right after the thread starts, and after everything the thread
// and `request_stop` use, so it is destroyed (and the thread joined) first.
// `request_stop` must make the thread function return and must not throw; it
// runs on the thread that calls StopAndJoin() or destroys the guard, and only
// while the thread is still joinable.
// A test that stops and joins its thread by hand on the normal path can keep
// doing so: the guard then finds nothing to do.
class ScopedThreadJoin {
 public:
  ScopedThreadJoin(std::thread& thread, std::function<void()> request_stop)
      : thread_(thread), request_stop_(std::move(request_stop)) {}
  ScopedThreadJoin(const ScopedThreadJoin&) = delete;
  ScopedThreadJoin& operator=(const ScopedThreadJoin&) = delete;
  ~ScopedThreadJoin() { StopAndJoin(); }

  void StopAndJoin() {
    if (!thread_.joinable()) return;
    if (request_stop_) request_stop_();
    thread_.join();
  }

 private:
  std::thread& thread_;
  std::function<void()> request_stop_;
};

}  // namespace pagespeed::test

#endif  // TEST_TEST_UTIL_SCOPED_THREAD_JOIN_H_
