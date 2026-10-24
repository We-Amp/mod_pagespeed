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

// Tests for StdTimer, verifying microsecond precision and sleep accuracy.

#include "pagespeed/kernel/base/std_timer.h"

#include "pagespeed/kernel/base/basictypes.h"
#include "pagespeed/kernel/base/timer.h"
#include "test/pagespeed/kernel/base/gtest.h"

namespace net_instaweb {

namespace {

class StdTimerTest : public testing::Test {
 protected:
  StdTimer timer_;
};

// Test that NowUs returns reasonable values (since epoch)
TEST_F(StdTimerTest, NowUsReturnsReasonableValue) {
  int64 now_us = timer_.NowUs();
  // Should be a positive value representing microseconds since epoch
  EXPECT_GT(now_us, 0);

  // Should be at least year 2020 in microseconds since epoch
  // 2020-01-01 00:00:00 UTC is approximately 1577836800 seconds
  int64 year_2020_us = 1577836800LL * Timer::kSecondUs;
  EXPECT_GT(now_us, year_2020_us);
}

// Test that NowMs is consistent with NowUs
TEST_F(StdTimerTest, NowMsConsistentWithNowUs) {
  int64 now_us = timer_.NowUs();
  int64 now_ms = timer_.NowMs();

  // NowMs should be approximately NowUs / 1000
  // Allow for some time passing between calls
  EXPECT_NEAR(now_ms, now_us / Timer::kMsUs, 10);
}

// Test that time progresses monotonically
TEST_F(StdTimerTest, TimeProgressesMonotonically) {
  int64 prev = timer_.NowUs();
  for (int i = 0; i < 100; ++i) {
    int64 now = timer_.NowUs();
    EXPECT_GE(now, prev);
    prev = now;
  }
}

// Test that SleepUs does not return before the requested time has elapsed.
TEST_F(StdTimerTest, SleepUsAccuracy) {
  const int64 sleep_us = 10000;  // 10ms

  // Measure on the monotonic clock: the sleep itself runs on that clock
  // family, and a wall-clock step would break the bounds below.
  int64 start = timer_.NowMonotonicUs();
  timer_.SleepUs(sleep_us);
  int64 elapsed = timer_.NowMonotonicUs() - start;

  // Should have slept at least the requested time
  EXPECT_GE(elapsed, sleep_us);

  // The property under test is "not early": a sleep may legitimately
  // overrun by any amount on a loaded host, so the upper bound is only a
  // unit-error guard (it still catches a seconds-for-milliseconds bug).
  EXPECT_LT(elapsed, sleep_us + 5 * Timer::kSecondUs);
}

// Test that SleepMs does not return before the requested time has elapsed.
TEST_F(StdTimerTest, SleepMsAccuracy) {
  const int64 sleep_ms = 10;

  int64 start = timer_.NowMonotonicMs();
  timer_.SleepMs(sleep_ms);
  int64 elapsed = timer_.NowMonotonicMs() - start;

  // Should have slept at least the requested time
  EXPECT_GE(elapsed, sleep_ms);

  // As in SleepUsAccuracy: no upper bound tighter than a unit-error guard,
  // since a sleep may legitimately overrun on a loaded host.
  EXPECT_LT(elapsed, sleep_ms + 5 * Timer::kSecondMs);
}

// Test microsecond precision by measuring multiple short sleeps
TEST_F(StdTimerTest, MicrosecondPrecision) {
  // Measure resolution by taking many samples
  int64 min_delta = Timer::kSecondUs;  // Start with a large value
  int64 prev = timer_.NowUs();

  for (int i = 0; i < 1000; ++i) {
    int64 now = timer_.NowUs();
    int64 delta = now - prev;
    if (delta > 0 && delta < min_delta) {
      min_delta = delta;
    }
    prev = now;
  }

  // The minimum observable time delta should be in the microsecond range
  // (much less than 1ms = 1000us)
  EXPECT_LT(min_delta, 1000);
}

// Test that the monotonic clock is non-decreasing across many samples.
TEST_F(StdTimerTest, MonotonicClockNeverDecreases) {
  int64 prev = timer_.NowMonotonicUs();
  EXPECT_GT(prev, 0);
  for (int i = 0; i < 1000; ++i) {
    int64 now = timer_.NowMonotonicUs();
    EXPECT_GE(now, prev);
    prev = now;
  }
}

// Test that the monotonic clock actually advances across a sleep, so a latency
// delta computed from it is positive.
TEST_F(StdTimerTest, MonotonicClockAdvancesAcrossSleep) {
  const int64 sleep_us = 10000;  // 10ms
  int64 start = timer_.NowMonotonicUs();
  timer_.SleepUs(sleep_us);
  int64 delta = timer_.NowMonotonicUs() - start;
  EXPECT_GE(delta, sleep_us);
}

// Test that NowMonotonicMs is consistent with NowMonotonicUs.
TEST_F(StdTimerTest, MonotonicMsConsistentWithUs) {
  int64 now_us = timer_.NowMonotonicUs();
  int64 now_ms = timer_.NowMonotonicMs();
  EXPECT_NEAR(now_ms, now_us / Timer::kMsUs, 10);
}

// Test that timer constants are correct
TEST_F(StdTimerTest, TimerConstants) {
  EXPECT_EQ(1000, Timer::kSecondMs);
  EXPECT_EQ(1000, Timer::kMsUs);
  EXPECT_EQ(1000000, Timer::kSecondUs);
  EXPECT_EQ(1000000000, Timer::kSecondNs);
}

}  // namespace

}  // namespace net_instaweb
