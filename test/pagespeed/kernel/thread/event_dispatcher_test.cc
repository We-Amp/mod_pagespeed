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

#include "pagespeed/kernel/thread/event_dispatcher.h"

#include "pagespeed/kernel/base/function.h"
#include "pagespeed/kernel/base/thread_system.h"
#include "pagespeed/kernel/base/timer.h"
#include "pagespeed/kernel/util/platform.h"
#include "test/pagespeed/kernel/base/gtest.h"

namespace net_instaweb {
namespace {

// Test the BasicEventTimer class.
class BasicEventTimerTest : public testing::Test {
 protected:
  void SetUp() override { call_count_ = 0; }

  static void IncrementCounter() { ++call_count_; }

  static int call_count_;
};

int BasicEventTimerTest::call_count_ = 0;

// Helper function class for testing.
class CountFunction : public Function {
 public:
  explicit CountFunction(int* counter) : counter_(counter) {}

 protected:
  void Run() override { ++(*counter_); }
  void Cancel() override { *counter_ -= 100; }

 private:
  int* counter_;
};

TEST_F(BasicEventTimerTest, CancelCallsCancel) {
  int counter = 0;
  BasicEventTimer timer(new CountFunction(&counter));

  EXPECT_FALSE(timer.IsCancelled());
  EXPECT_EQ(0, counter);

  timer.Cancel();

  EXPECT_TRUE(timer.IsCancelled());
  // Cancel should have been called on the function.
  EXPECT_EQ(-100, counter);
}

TEST_F(BasicEventTimerTest, DestructorCancels) {
  int counter = 0;
  {
    BasicEventTimer timer(new CountFunction(&counter));
    EXPECT_EQ(0, counter);
  }
  // Destructor should have called Cancel.
  EXPECT_EQ(-100, counter);
}

TEST_F(BasicEventTimerTest, ReleaseFunctionPreventsCancel) {
  int counter = 0;
  BasicEventTimer timer(new CountFunction(&counter));

  Function* f = timer.ReleaseFunction();
  EXPECT_NE(nullptr, f);

  // Destructor should not cancel since we released the function.
  // The counter should still be 0.
  // Note: We need to manually call the function and delete it.
  f->CallRun();
  EXPECT_EQ(1, counter);
}

TEST_F(BasicEventTimerTest, DoubleCancelIsNoOp) {
  int counter = 0;
  BasicEventTimer timer(new CountFunction(&counter));

  timer.Cancel();
  EXPECT_EQ(-100, counter);

  // Second cancel should be no-op.
  timer.Cancel();
  EXPECT_EQ(-100, counter);
}

}  // namespace
}  // namespace net_instaweb
