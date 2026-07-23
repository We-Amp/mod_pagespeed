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

#ifndef PAGESPEED_KERNEL_BASE_TIMER_H_
#define PAGESPEED_KERNEL_BASE_TIMER_H_

#include "pagespeed/kernel/base/basictypes.h"

namespace net_instaweb {

// Timer interface, made virtual so it can be mocked for tests.
class Timer {
 public:
  // Note: it's important that these stay compile-time constants, to
  // avoid weird init order surprises. (Noticed in the wild on Mac).
  // If you get a link error due to one of these missing, you're trying
  // to pass it in by a const reference. You can do something like * 1 to
  // sidestep the issue.
  static const int64 kSecondMs = 1000;
  static const int64 kMsUs = 1000;
  static const int64 kSecondUs = kMsUs * kSecondMs;
  static const int64 kSecondNs = 1000 * kSecondUs;
  static const int64 kMinuteMs = 60 * kSecondMs;
  static const int64 kMinuteUs = 60 * kSecondUs;
  static const int64 kHourMs = 60 * kMinuteMs;
  static const int64 kDayMs = 24 * kHourMs;
  static const int64 kWeekMs = 7 * kDayMs;
  static const int64 kMonthMs = 31 * kDayMs;
  static const int64 kYearMs = 365 * kDayMs;

  virtual ~Timer();

  // Returns number of milliseconds since 1970.
  virtual int64 NowMs() const;

  // Returns number of microseconds since 1970.
  virtual int64 NowUs() const = 0;

  // Returns a monotonically non-decreasing time in microseconds from an
  // unspecified epoch. Unlike NowUs(), this is immune to wall-clock steps
  // (NTP, hypervisor time-sync, a misbehaving timesync daemon), so it is the
  // correct source for elapsed-time / latency / duration measurements.
  //
  // It must NOT be used for absolute timestamps (HTTP Date headers, cache
  // freshness/expiry), which require wall-clock time from NowUs()/NowMs().
  //
  // The base implementation falls back to NowUs() for Timer subclasses that
  // have no independent monotonic source; subclasses backed by a real
  // monotonic clock (PosixTimer, StdTimer) override it.
  virtual int64 NowMonotonicUs() const;

  // Monotonic counterpart of NowMs(); see NowMonotonicUs().
  virtual int64 NowMonotonicMs() const;

  // Sleep for given number of milliseconds.
  virtual void SleepMs(int64 ms);

  // Sleep for given number of microseconds.
  virtual void SleepUs(int64 us) = 0;
};

}  // namespace net_instaweb

#endif  // PAGESPEED_KERNEL_BASE_TIMER_H_
