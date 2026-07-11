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

#include "pagespeed/kernel/thread/event_scheduler.h"

#include <atomic>
#include <memory>
#include <vector>

#include "pagespeed/kernel/base/abstract_mutex.h"
#include "pagespeed/kernel/base/function.h"
#include "pagespeed/kernel/base/thread_system.h"
#include "pagespeed/kernel/base/timer.h"
#include "pagespeed/kernel/thread/libevent_dispatcher.h"
#include "pagespeed/kernel/util/platform.h"
#include "test/pagespeed/kernel/base/gtest.h"
#include "test/pagespeed/kernel/thread/worker_test_base.h"

namespace net_instaweb {
namespace {

// Helper function class for counting (with Cancel for proper testing).
class SchedulerCountFunction : public Function {
 public:
  explicit SchedulerCountFunction(int* counter) : counter_(counter) {}

 protected:
  void Run() override { ++(*counter_); }
  void Cancel() override { /* Do nothing - counter should stay unchanged */ }

 private:
  int* counter_;
};

class EventSchedulerTest : public WorkerTestBase {
 protected:
  EventSchedulerTest()
      : thread_system_(Platform::CreateThreadSystem()),
        timer_(thread_system_->NewTimer()),
        dispatcher_(new LibeventDispatcher(thread_system_.get(), timer_.get())),
        scheduler_(thread_system_.get(), dispatcher_.get()) {}

  void SetUp() override { ASSERT_TRUE(dispatcher_->Start()); }

  void TearDown() override {
    dispatcher_->InitiateShutdown();
    dispatcher_->WaitForShutdown();
  }

  // Process alarms until no more pending or timeout.
  void QuiesceAlarms(int64 timeout_us) {
    ScopedMutex lock(scheduler_.mutex());
    int64 now_us = timer_->NowUs();
    int64 end_us = now_us + timeout_us;
    while (now_us < end_us) {
      if (!scheduler_.ProcessAlarmsOrWaitUs(end_us - now_us)) {
        break;  // No more pending alarms.
      }
      now_us = timer_->NowUs();
    }
  }

  std::unique_ptr<ThreadSystem> thread_system_;
  std::unique_ptr<Timer> timer_;
  std::unique_ptr<LibeventDispatcher> dispatcher_;
  EventScheduler scheduler_;
};

TEST_F(EventSchedulerTest, AlarmsGetRun) {
  int64 start_us = timer_->NowUs();
  int counter = 0;

  // Add alarms with different delays.
  scheduler_.AddAlarmAtUs(start_us + 10 * Timer::kMsUs,
                          new SchedulerCountFunction(&counter));
  scheduler_.AddAlarmAtUs(start_us + 20 * Timer::kMsUs,
                          new SchedulerCountFunction(&counter));
  scheduler_.AddAlarmAtUs(start_us + 30 * Timer::kMsUs,
                          new SchedulerCountFunction(&counter));

  // Wait for all alarms to fire.
  QuiesceAlarms(1 * Timer::kSecondUs);

  EXPECT_EQ(3, counter);
}

// Helper function to push an int to a vector.
class PushIntFunction : public Function {
 public:
  PushIntFunction(int value, std::vector<int>* order)
      : value_(value), order_(order) {}

 protected:
  void Run() override { order_->push_back(value_); }

 private:
  int value_;
  std::vector<int>* order_;
};

TEST_F(EventSchedulerTest, AlarmsRunInOrder) {
  int64 start_us = timer_->NowUs();
  std::vector<int> order;

  // This test verifies that alarms are run in time order.
  // Add alarms out of order.
  scheduler_.AddAlarmAtUs(start_us + 30 * Timer::kMsUs,
                          new PushIntFunction(3, &order));
  scheduler_.AddAlarmAtUs(start_us + 10 * Timer::kMsUs,
                          new PushIntFunction(1, &order));
  scheduler_.AddAlarmAtUs(start_us + 20 * Timer::kMsUs,
                          new PushIntFunction(2, &order));

  // Wait for all alarms to fire.
  QuiesceAlarms(1 * Timer::kSecondUs);

  ASSERT_EQ(3u, order.size());
  EXPECT_EQ(1, order[0]);
  EXPECT_EQ(2, order[1]);
  EXPECT_EQ(3, order[2]);
}

TEST_F(EventSchedulerTest, CancelAlarm) {
  int64 start_us = timer_->NowUs();
  int counter = 0;

  // Add an alarm far in the future.
  Scheduler::Alarm* alarm = scheduler_.AddAlarmAtUs(
      start_us + 10 * Timer::kSecondUs, new SchedulerCountFunction(&counter));

  // Cancel it.
  {
    ScopedMutex lock(scheduler_.mutex());
    EXPECT_TRUE(scheduler_.CancelAlarm(alarm));
  }

  // Wait a bit and verify it didn't run.
  timer_->SleepUs(100 * Timer::kMsUs);
  EXPECT_EQ(0, counter);
}

TEST_F(EventSchedulerTest, BlockingTimedWait) {
  int64 start_us = timer_->NowUs();

  {
    ScopedMutex lock(scheduler_.mutex());
    scheduler_.BlockingTimedWaitMs(50);  // Wait 50ms.
  }

  int64 elapsed_us = timer_->NowUs() - start_us;
  // Should have waited approximately 50ms.
  EXPECT_GE(elapsed_us, 40 * Timer::kMsUs);   // Allow some slack.
  EXPECT_LE(elapsed_us, 200 * Timer::kMsUs);  // But not too much.
}

TEST_F(EventSchedulerTest, Signal) {
  int counter = 0;

  // TimedWait with a callback that will be run when Signal is called.
  {
    ScopedMutex lock(scheduler_.mutex());
    scheduler_.TimedWaitMs(
        10 * 1000, new SchedulerCountFunction(&counter));  // Long timeout.
    scheduler_.Signal();  // Signal immediately.
  }

  // The callback should have been invoked.
  EXPECT_EQ(1, counter);
}

TEST_F(EventSchedulerTest, TimerAccessor) {
  EXPECT_EQ(dispatcher_->timer(), scheduler_.timer());
}

// Alarm callback that records firing for a thread polling from outside the
// scheduler (alarms fire on the dispatcher thread in these tests).
class AtomicCountFunction : public Function {
 public:
  explicit AtomicCountFunction(std::atomic<int>* counter) : counter_(counter) {}

 protected:
  void Run() override { counter_->fetch_add(1, std::memory_order_acq_rel); }
  void Cancel() override {}

 private:
  std::atomic<int>* counter_;
};

// Polls until *counter >= expected or timeout_us elapses; returns the final
// counter value. Deliberately never calls into the scheduler: these tests
// verify the dispatcher drives alarms with NO waiting thread.
int AwaitCount(Timer* timer, std::atomic<int>* counter, int expected,
               int64 timeout_us) {
  int64 end_us = timer->NowUs() + timeout_us;
  while (counter->load(std::memory_order_acquire) < expected &&
         timer->NowUs() < end_us) {
    timer->SleepUs(2 * Timer::kMsUs);
  }
  return counter->load(std::memory_order_acquire);
}

// The core regression test for the half-wired integration: an alarm must
// fire on time even though no thread ever blocks in the scheduler (no
// ProcessAlarmsOrWaitUs poller, no SchedulerThread, no blocked waiter).
TEST_F(EventSchedulerTest, AlarmsFireWithoutWaiter) {
  std::atomic<int> counter(0);
  scheduler_.AddAlarmAtUs(timer_->NowUs() + 20 * Timer::kMsUs,
                          new AtomicCountFunction(&counter));
  EXPECT_EQ(1, AwaitCount(timer_.get(), &counter, 1, 2 * Timer::kSecondUs));
}

// Adding an alarm EARLIER than the currently-armed deadline must re-arm the
// pump: the early alarm fires promptly, not at the stale far deadline.
TEST_F(EventSchedulerTest, EarlierAlarmRearmsPump) {
  std::atomic<int> counter(0);
  Scheduler::Alarm* far_alarm =
      scheduler_.AddAlarmAtUs(timer_->NowUs() + 60 * Timer::kSecondUs,
                              new AtomicCountFunction(&counter));
  scheduler_.AddAlarmAtUs(timer_->NowUs() + 20 * Timer::kMsUs,
                          new AtomicCountFunction(&counter));
  EXPECT_EQ(1, AwaitCount(timer_.get(), &counter, 1, 2 * Timer::kSecondUs));
  {
    ScopedMutex lock(scheduler_.mutex());
    EXPECT_TRUE(scheduler_.CancelAlarm(far_alarm));
  }
}

// The attach-later pattern used by the Apache and Envoy factories: alarms
// scheduled while unattached (when the scheduler behaves like the base
// class) must be picked up and driven once a dispatcher is attached.
TEST(EventSchedulerAttachTest, AlarmsBeforeAttachFireAfterAttach) {
  std::unique_ptr<ThreadSystem> thread_system(Platform::CreateThreadSystem());
  std::unique_ptr<Timer> timer(thread_system->NewTimer());
  EventScheduler scheduler(thread_system.get(), timer.get());

  std::atomic<int> counter(0);
  scheduler.AddAlarmAtUs(timer->NowUs() + 10 * Timer::kMsUs,
                         new AtomicCountFunction(&counter));

  // Unattached and nothing ever blocks in the scheduler: past-due alarms
  // have no driver.
  timer->SleepUs(100 * Timer::kMsUs);
  EXPECT_EQ(0, counter.load(std::memory_order_acquire));

  LibeventDispatcher dispatcher(thread_system.get(), timer.get());
  ASSERT_TRUE(dispatcher.Start());
  scheduler.AttachDispatcher(&dispatcher);
  EXPECT_EQ(1, AwaitCount(timer.get(), &counter, 1, 2 * Timer::kSecondUs));

  dispatcher.InitiateShutdown();
  dispatcher.WaitForShutdown();
  scheduler.DetachDispatcher();
}

}  // namespace
}  // namespace net_instaweb
