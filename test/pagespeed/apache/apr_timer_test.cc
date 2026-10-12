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

// AprTimer has no APR monotonic clock; it must still provide one that does
// not follow the wall clock.

#include "pagespeed/apache/apr_timer.h"

#include "pagespeed/kernel/base/timer.h"
#include "test/pagespeed/kernel/base/gtest.h"

namespace net_instaweb {
namespace {

TEST(AprTimerTest, MonotonicClockIsIndependentOfWallClock) {
  AprTimer timer;
  const int64 wall_us = timer.NowUs();
  const int64 mono_us = timer.NowMonotonicUs();
  // The wall clock is microseconds since 1970; the steady clock's epoch is
  // not, so the two readings differ unless the fallback to NowUs() is used.
  EXPECT_NE(wall_us / Timer::kSecondUs, mono_us / Timer::kSecondUs);
}

TEST(AprTimerTest, MonotonicClockAdvancesWithSleep) {
  AprTimer timer;
  const int64 before_us = timer.NowMonotonicUs();
  timer.SleepUs(20 * Timer::kMsUs);
  const int64 after_us = timer.NowMonotonicUs();
  EXPECT_LE(before_us + 20 * Timer::kMsUs, after_us);
  EXPECT_GE(after_us, before_us);
}

}  // namespace
}  // namespace net_instaweb
