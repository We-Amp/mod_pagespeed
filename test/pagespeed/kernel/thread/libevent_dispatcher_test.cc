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

#include "pagespeed/kernel/thread/libevent_dispatcher.h"

#include <atomic>
#include <memory>

#include "pagespeed/kernel/base/abstract_mutex.h"
#include "pagespeed/kernel/base/condvar.h"
#include "pagespeed/kernel/base/function.h"
#include "pagespeed/kernel/base/thread_system.h"
#include "pagespeed/kernel/base/timer.h"
#include "pagespeed/kernel/util/platform.h"
#include "test/pagespeed/kernel/base/gtest.h"

namespace net_instaweb {
namespace {

// Helper function class for counting.
class CountFunction : public Function {
 public:
  explicit CountFunction(std::atomic<int>* counter) : counter_(counter) {}

 protected:
  void Run() override { ++(*counter_); }
  void Cancel() override { counter_->fetch_sub(100); }

 private:
  std::atomic<int>* counter_;
};

// Helper function class that signals a condvar.
class SignalFunction : public Function {
 public:
  SignalFunction(bool* done, ThreadSystem::CondvarCapableMutex* mutex,
                 ThreadSystem::Condvar* condvar)
      : done_(done), mutex_(mutex), condvar_(condvar) {}

 protected:
  void Run() override {
    ScopedMutex lock(mutex_);
    *done_ = true;
    condvar_->Signal();
  }

 private:
  bool* done_;
  ThreadSystem::CondvarCapableMutex* mutex_;
  ThreadSystem::Condvar* condvar_;
};

class LibeventDispatcherTest : public testing::Test {
 protected:
  void SetUp() override {
    thread_system_.reset(Platform::CreateThreadSystem());
    timer_.reset(thread_system_->NewTimer());
    dispatcher_.reset(new LibeventDispatcher(thread_system_.get(), timer_.get()));
  }

  void TearDown() override {
    if (dispatcher_ != nullptr) {
      dispatcher_->InitiateShutdown();
      dispatcher_->WaitForShutdown();
    }
  }

  // Wait for a posted function to complete, with timeout.
  bool WaitForCompletion(int64 timeout_us) {
    std::unique_ptr<ThreadSystem::CondvarCapableMutex> mutex(
        thread_system_->NewMutex());
    std::unique_ptr<ThreadSystem::Condvar> condvar(mutex->NewCondvar());
    bool done = false;

    dispatcher_->Post(new SignalFunction(&done, mutex.get(), condvar.get()));

    ScopedMutex lock(mutex.get());
    int64 start_us = timer_->NowUs();
    while (!done && (timer_->NowUs() - start_us) < timeout_us) {
      condvar->TimedWait(10);  // 10ms
    }
    return done;
  }

  std::unique_ptr<ThreadSystem> thread_system_;
  std::unique_ptr<Timer> timer_;
  std::unique_ptr<LibeventDispatcher> dispatcher_;
};

TEST_F(LibeventDispatcherTest, StartAndShutdown) {
  ASSERT_TRUE(dispatcher_->Start());
  EXPECT_FALSE(dispatcher_->IsShuttingDown());

  dispatcher_->InitiateShutdown();
  EXPECT_TRUE(dispatcher_->IsShuttingDown());

  dispatcher_->WaitForShutdown();
  // No crash means success.
}

TEST_F(LibeventDispatcherTest, PostRunsFunction) {
  ASSERT_TRUE(dispatcher_->Start());

  std::atomic<int> counter(0);
  dispatcher_->Post(new CountFunction(&counter));

  // Wait for the function to run.
  int64 start_us = timer_->NowUs();
  while (counter.load() == 0 &&
         (timer_->NowUs() - start_us) < 5 * Timer::kSecondUs) {
    timer_->SleepUs(Timer::kMsUs);
  }

  EXPECT_EQ(1, counter.load());
}

TEST_F(LibeventDispatcherTest, MultiplePostsRunInOrder) {
  ASSERT_TRUE(dispatcher_->Start());

  std::atomic<int> counter(0);
  const int kNumPosts = 100;

  for (int i = 0; i < kNumPosts; ++i) {
    dispatcher_->Post(new CountFunction(&counter));
  }

  // Wait for all functions to run.
  int64 start_us = timer_->NowUs();
  while (counter.load() < kNumPosts &&
         (timer_->NowUs() - start_us) < 5 * Timer::kSecondUs) {
    timer_->SleepUs(Timer::kMsUs);
  }

  EXPECT_EQ(kNumPosts, counter.load());
}

TEST_F(LibeventDispatcherTest, TimerFires) {
  ASSERT_TRUE(dispatcher_->Start());

  std::atomic<int> counter(0);
  EventTimer* timer = dispatcher_->CreateTimer(
      50 * Timer::kMsUs,  // 50ms delay
      new CountFunction(&counter));

  // Wait for the timer to fire.
  int64 start_us = timer_->NowUs();
  while (counter.load() == 0 &&
         (timer_->NowUs() - start_us) < 5 * Timer::kSecondUs) {
    timer_->SleepUs(Timer::kMsUs);
  }

  EXPECT_EQ(1, counter.load());
  delete timer;
}

TEST_F(LibeventDispatcherTest, TimerCancel) {
  ASSERT_TRUE(dispatcher_->Start());

  std::atomic<int> counter(0);
  EventTimer* timer = dispatcher_->CreateTimer(
      1 * Timer::kSecondUs,  // 1 second delay - long enough to cancel
      new CountFunction(&counter));

  // Cancel immediately.
  timer->Cancel();

  // Wait a bit to make sure the timer doesn't fire.
  timer_->SleepUs(100 * Timer::kMsUs);

  // Counter should be negative (cancelled).
  EXPECT_EQ(-100, counter.load());
  delete timer;
}

TEST_F(LibeventDispatcherTest, CreateTimerAtUs) {
  ASSERT_TRUE(dispatcher_->Start());

  std::atomic<int> counter(0);
  int64 now_us = timer_->NowUs();
  EventTimer* timer = dispatcher_->CreateTimerAtUs(
      now_us + 50 * Timer::kMsUs,  // 50ms from now
      new CountFunction(&counter));

  // Wait for the timer to fire.
  int64 start_us = timer_->NowUs();
  while (counter.load() == 0 &&
         (timer_->NowUs() - start_us) < 5 * Timer::kSecondUs) {
    timer_->SleepUs(Timer::kMsUs);
  }

  EXPECT_EQ(1, counter.load());
  delete timer;
}

TEST_F(LibeventDispatcherTest, PostDuringShutdownCancels) {
  ASSERT_TRUE(dispatcher_->Start());
  dispatcher_->InitiateShutdown();

  std::atomic<int> counter(0);
  dispatcher_->Post(new CountFunction(&counter));

  // The function should have been cancelled.
  EXPECT_EQ(-100, counter.load());
}

TEST_F(LibeventDispatcherTest, TimerAccessor) {
  EXPECT_EQ(timer_.get(), dispatcher_->timer());
}

}  // namespace
}  // namespace net_instaweb
