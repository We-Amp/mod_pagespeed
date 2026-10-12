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

#ifndef PAGESPEED_KERNEL_BASE_COUNTDOWN_TIMER_H_
#define PAGESPEED_KERNEL_BASE_COUNTDOWN_TIMER_H_

#include "pagespeed/kernel/base/basictypes.h"

namespace net_instaweb {

class Timer;

// Once initialized with a non-negative interval 'allowed_time_ms' via
// the ctor or Reset(), repeated calls to HaveTimeLeft() will return
// true only until the interval has elapsed. If the interval is
// negative number, HaveTimeLeft() always returns true.
class CountdownTimer {
 public:
  CountdownTimer(Timer* timer, void* user_data, int64 allowed_time_ms);

  void Reset(int64 allowed_time_ms);

  bool HaveTimeLeft() const;

  void* user_data() const { return user_data_; }

  int64 TimeLeftMs() const;

  int64 TimeElapsedMs() const;

 private:
  Timer* timer_;
  void* user_data_;
  // 64-bit: clock_t is a 32-bit long on Windows, which holds only ~36 minutes
  // of microseconds.
  int64 limit_time_us_;
  int64 start_time_us_;
  static_assert(sizeof(limit_time_us_) == 8 && sizeof(start_time_us_) == 8,
                "microsecond values need 64 bits");
};

}  // namespace net_instaweb

#endif  // PAGESPEED_KERNEL_BASE_COUNTDOWN_TIMER_H_
