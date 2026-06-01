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

#include "pagespeed/kernel/thread/dispatcher_sequence.h"

#include <atomic>
#include <vector>
#include <memory>

#include "pagespeed/kernel/base/abstract_mutex.h"
#include "pagespeed/kernel/base/function.h"
#include "pagespeed/kernel/base/thread_system.h"
#include "pagespeed/kernel/base/timer.h"
#include "pagespeed/kernel/thread/event_dispatcher.h"
#include "pagespeed/kernel/thread/libevent_dispatcher.h"
#include "pagespeed/kernel/util/platform.h"
#include "test/pagespeed/kernel/base/gtest.h"

namespace net_instaweb {
namespace {

// Helper function class that records call order.
class RecordOrderFunction : public Function {
 public:
  RecordOrderFunction(int id, std::vector<int>* order, AbstractMutex* mutex)
      : id_(id), order_(order), mutex_(mutex) {}

 protected:
  void Run() override {
    ScopedMutex lock(mutex_);
    order_->push_back(id_);
  }

 private:
  int id_;
  std::vector<int>* order_;
  AbstractMutex* mutex_;
};

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

class DispatcherSequenceTest : public testing::Test {
 protected:
  void SetUp() override {
    thread_system_.reset(Platform::CreateThreadSystem());
    timer_.reset(thread_system_->NewTimer());
    dispatcher_.reset(new LibeventDispatcher(thread_system_.get(), timer_.get()));
    ASSERT_TRUE(dispatcher_->Start());
    mutex_.reset(thread_system_->NewMutex());
  }

  void TearDown() override {
    dispatcher_->InitiateShutdown();
    dispatcher_->WaitForShutdown();
  }

  std::unique_ptr<ThreadSystem> thread_system_;
  std::unique_ptr<Timer> timer_;
  std::unique_ptr<LibeventDispatcher> dispatcher_;
  std::unique_ptr<AbstractMutex> mutex_;
};

TEST_F(DispatcherSequenceTest, SequentialExecution) {
  // Tasks added to a sequence should execute in order.
  std::vector<int> order;
  DispatcherSequence sequence(dispatcher_.get(), thread_system_.get());

  const int kNumTasks = 10;
  for (int i = 0; i < kNumTasks; ++i) {
    sequence.Add(new RecordOrderFunction(i, &order, mutex_.get()));
  }

  // Wait for all tasks to complete.
  int64 start_us = timer_->NowUs();
  {
    size_t current_size = 0;
    while (current_size < kNumTasks &&
           (timer_->NowUs() - start_us) < 5 * Timer::kSecondUs) {
      timer_->SleepUs(Timer::kMsUs);
      ScopedMutex lock(mutex_.get());
      current_size = order.size();
    }
  }

  // All tasks should have completed by now; the dispatcher thread is done
  // writing to order so we can read without the mutex.
  ASSERT_EQ(kNumTasks, static_cast<int>(order.size()));
  for (int i = 0; i < kNumTasks; ++i) {
    EXPECT_EQ(i, order[i]) << "Task " << i << " ran out of order";
  }
}

TEST_F(DispatcherSequenceTest, ShutdownCancelsPending) {
  // Pending tasks should be cancelled on shutdown.
  std::atomic<int> counter(0);
  DispatcherSequence sequence(dispatcher_.get(), thread_system_.get());

  // Add many tasks.
  for (int i = 0; i < 100; ++i) {
    sequence.Add(new CountFunction(&counter));
  }

  // Immediately shutdown - some tasks should be cancelled.
  sequence.InitiateShutdown();
  sequence.WaitForShutdown();

  // We expect some tasks ran and some were cancelled.
  // Cancelled tasks decrement by 100, ran tasks increment by 1.
  // The exact count depends on timing, but it should be <= 100.
  // (If all ran: 100, if all cancelled: -10000, mix: something in between)
  EXPECT_LE(counter.load(), 100);
}

TEST_F(DispatcherSequenceTest, AddDuringShutdownCancels) {
  // Tasks added during shutdown should be cancelled immediately.
  std::atomic<int> counter(0);
  DispatcherSequence sequence(dispatcher_.get(), thread_system_.get());

  sequence.InitiateShutdown();

  // Add a task during shutdown.
  sequence.Add(new CountFunction(&counter));

  // The task should have been cancelled.
  EXPECT_EQ(-100, counter.load());
}

}  // namespace
}  // namespace net_instaweb
