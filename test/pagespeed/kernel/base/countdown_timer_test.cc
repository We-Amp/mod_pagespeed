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

#include "pagespeed/kernel/base/countdown_timer.h"

#include "pagespeed/kernel/base/null_mutex.h"
#include "pagespeed/kernel/base/timer.h"
#include "test/pagespeed/kernel/base/gtest.h"
#include "test/pagespeed/kernel/base/mock_timer.h"

namespace net_instaweb {

class CountdownTimerTest : public testing::Test {
 protected:
  CountdownTimerTest() : timer_(new NullMutex, 0) {}

  MockTimer timer_;
};

TEST_F(CountdownTimerTest, SetTimeNegative) {
  const char* data = "Some fake data";
  CountdownTimer countdown_timer(&timer_, &data, -1);
  EXPECT_TRUE(countdown_timer.HaveTimeLeft());
  EXPECT_EQ(&data, countdown_timer.user_data());
  timer_.AdvanceMs(100);
  EXPECT_TRUE(countdown_timer.HaveTimeLeft());
  EXPECT_EQ(&data, countdown_timer.user_data());

  countdown_timer.Reset(-57);
  EXPECT_TRUE(countdown_timer.HaveTimeLeft());
  EXPECT_EQ(&data, countdown_timer.user_data());
  timer_.SetTimeMs(30);
  EXPECT_TRUE(countdown_timer.HaveTimeLeft());
  EXPECT_EQ(&data, countdown_timer.user_data());
}

TEST_F(CountdownTimerTest, SetTimeZero) {
  const char* data = "Nothing real";
  timer_.SetTimeUs(10);
  CountdownTimer countdown_timer(&timer_, &data, 0);
  EXPECT_FALSE(countdown_timer.HaveTimeLeft());
  EXPECT_EQ(&data, countdown_timer.user_data());
  timer_.AdvanceMs(100);
  EXPECT_FALSE(countdown_timer.HaveTimeLeft());
  EXPECT_EQ(&data, countdown_timer.user_data());

  countdown_timer.Reset(0);
  EXPECT_FALSE(countdown_timer.HaveTimeLeft());
  EXPECT_EQ(&data, countdown_timer.user_data());
}

TEST_F(CountdownTimerTest, SetTimePositive) {
  const char* data = "Something for later";
  CountdownTimer countdown_timer(&timer_, &data, 1);
  EXPECT_TRUE(countdown_timer.HaveTimeLeft());
  EXPECT_EQ(&data, countdown_timer.user_data());
  timer_.AdvanceMs(100);
  EXPECT_FALSE(countdown_timer.HaveTimeLeft());
  EXPECT_EQ(&data, countdown_timer.user_data());

  countdown_timer.Reset(10);
  EXPECT_TRUE(countdown_timer.HaveTimeLeft());
  EXPECT_EQ(&data, countdown_timer.user_data());
  timer_.AdvanceMs(9);
  EXPECT_TRUE(countdown_timer.HaveTimeLeft());
  EXPECT_EQ(&data, countdown_timer.user_data());
  timer_.AdvanceUs(999);
  EXPECT_TRUE(countdown_timer.HaveTimeLeft());
  EXPECT_EQ(&data, countdown_timer.user_data());
  timer_.AdvanceUs(1);
  EXPECT_FALSE(countdown_timer.HaveTimeLeft());
  EXPECT_EQ(&data, countdown_timer.user_data());
}

// A backward wall-clock step must neither extend nor shorten the budget:
// the countdown follows the monotonic clock.
TEST_F(CountdownTimerTest, BackwardWallClockStepDoesNotExtendBudget) {
  const char* data = "Step back";
  CountdownTimer countdown_timer(&timer_, &data, 1000);
  timer_.AdvanceMs(600);
  EXPECT_TRUE(countdown_timer.HaveTimeLeft());
  timer_.StepWallClockBackUs(10 * Timer::kSecondUs);
  EXPECT_TRUE(countdown_timer.HaveTimeLeft());
  EXPECT_EQ(600, countdown_timer.TimeElapsedMs());
  timer_.AdvanceMs(500);
  EXPECT_FALSE(countdown_timer.HaveTimeLeft());
  EXPECT_EQ(1100, countdown_timer.TimeElapsedMs());
}

// Microsecond readings beyond 2^31 (about 36 minutes) must not overflow the
// stored start/limit values (clock_t is 32 bits on Windows).
TEST_F(CountdownTimerTest, ElapsedBeyond32BitMicroseconds) {
  const char* data = "Long uptime";
  const int64 kMinuteMs = 60 * 1000;
  // Uptime of 36 minutes, i.e. just past 2^31 us.
  timer_.SetTimeUs((int64{1} << 31) + 1);
  CountdownTimer countdown_timer(&timer_, &data, 10 * kMinuteMs);
  EXPECT_TRUE(countdown_timer.HaveTimeLeft());
  timer_.AdvanceMs(9 * kMinuteMs);
  EXPECT_TRUE(countdown_timer.HaveTimeLeft());
  EXPECT_EQ(1 * kMinuteMs, countdown_timer.TimeLeftMs());
  EXPECT_EQ(9 * kMinuteMs, countdown_timer.TimeElapsedMs());
  timer_.AdvanceMs(1 * kMinuteMs);
  EXPECT_FALSE(countdown_timer.HaveTimeLeft());
}

// Start just below 2^31 us so the budget crosses the 32-bit boundary.
TEST_F(CountdownTimerTest, BudgetCrossesSigned32BitBoundary) {
  const char* data = "Crossing";
  timer_.SetTimeUs((int64{1} << 31) - 500 * 1000);
  CountdownTimer countdown_timer(&timer_, &data, 1000);
  EXPECT_TRUE(countdown_timer.HaveTimeLeft());
  timer_.AdvanceMs(999);
  EXPECT_TRUE(countdown_timer.HaveTimeLeft());
  EXPECT_EQ(1, countdown_timer.TimeLeftMs());
  timer_.AdvanceMs(1);
  EXPECT_FALSE(countdown_timer.HaveTimeLeft());
}

// Over 25 days of uptime (past 2^31 ms) and across the unsigned 32-bit
// microsecond boundary (2^32 us, about 71 minutes).
TEST_F(CountdownTimerTest, ElapsedBeyond25Days) {
  const char* data = "Very long uptime";
  const int64 kDayMs = 24LL * 60 * 60 * 1000;
  timer_.SetTimeMs(26 * kDayMs);
  CountdownTimer countdown_timer(&timer_, &data, 2 * kDayMs);
  EXPECT_TRUE(countdown_timer.HaveTimeLeft());
  timer_.AdvanceMs(kDayMs);
  EXPECT_TRUE(countdown_timer.HaveTimeLeft());
  EXPECT_EQ(kDayMs, countdown_timer.TimeLeftMs());
  EXPECT_EQ(kDayMs, countdown_timer.TimeElapsedMs());
  countdown_timer.Reset(1000);
  timer_.AdvanceMs(1000);
  EXPECT_FALSE(countdown_timer.HaveTimeLeft());
  EXPECT_EQ(1000, countdown_timer.TimeElapsedMs());
}

}  // namespace net_instaweb
