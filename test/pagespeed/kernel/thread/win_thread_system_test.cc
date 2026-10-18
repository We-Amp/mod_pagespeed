// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

// WinThreadSystem: the shared start/join and synchronization tests, plus the
// lifetime rule for a joinable thread whose implementation object goes away
// while the thread is still running.

#include "pagespeed/kernel/thread/win_thread_system.h"

#include <windows.h>

#include <atomic>
#include <memory>

#include "pagespeed/kernel/base/thread.h"
#include "pagespeed/kernel/base/thread_system.h"
#include "test/pagespeed/kernel/base/gtest.h"
#include "test/pagespeed/kernel/thread/thread_system_test_base.h"

namespace net_instaweb {

// ThreadSystem grants this class access to NewThreadImpl, which lets the test
// drive a thread implementation without the Thread wrapper's own checks.
class MockThreadSystem {
 public:
  static ThreadSystem::ThreadImpl* NewImpl(ThreadSystem* system,
                                           ThreadSystem::Thread* wrapper,
                                           ThreadSystem::ThreadFlags flags) {
    return system->NewThreadImpl(wrapper, flags);
  }
};

namespace {

INSTANTIATE_TYPED_TEST_SUITE_P(WinThread, ThreadSystemTestTemplate,
                               WinThreadSystem);

// Runs until released. The final store to |state_| is the last access the
// thread makes to this object.
class GatedThread : public ThreadSystem::Thread {
 public:
  explicit GatedThread(ThreadSystem* system)
      : Thread(system, "gated", ThreadSystem::kJoinable) {}

  void Run() override {
    state_.store(1);
    while (!release_.load()) {
      Sleep(1);
    }
    state_.store(2);
  }

  std::atomic<int> state_{0};
  std::atomic<bool> release_{false};
};

TEST(WinThreadSystemTest, ImplReleasedWhileJoinableThreadRuns) {
  WinThreadSystem system;
  GatedThread wrapper(&system);
  std::unique_ptr<ThreadSystem::ThreadImpl> impl(
      MockThreadSystem::NewImpl(&system, &wrapper, ThreadSystem::kJoinable));
  ASSERT_TRUE(impl->StartImpl());
  while (wrapper.state_.load() == 0) {
    Sleep(1);
  }
  // Destroying the implementation of a started, never-joined thread must
  // leave the thread running on its own rather than end the process.
  impl.reset();
  wrapper.release_.store(true);
  while (wrapper.state_.load() != 2) {
    Sleep(1);
  }
  // Give the detached thread time to return from Run.
  Sleep(50);
}

}  // namespace

}  // namespace net_instaweb
