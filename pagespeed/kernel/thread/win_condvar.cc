// Copyright 2011 Google Inc.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
//
// Author: jmaessen@google.com (Jan Maessen)

#include <Windows.h>
#include <winbase.h>

// KS: gettimeofday etc..
#ifdef WIN32
#endif

#include <cerrno>
#include <ctime>

#include "base/logging.h"
#include "pagespeed/kernel/base/basictypes.h"
#include "pagespeed/kernel/base/timer.h"
#include "pagespeed/kernel/thread/win_condvar.h"
#include "pagespeed/kernel/thread/win_mutex.h"

namespace net_instaweb {

WinCondvar::~WinCondvar() {
  // Does not need destroy
}

void WinCondvar::Signal() { ::WakeConditionVariable(&condvar_); }

void WinCondvar::Broadcast() { WakeAllConditionVariable(&condvar_); }

void WinCondvar::Wait() {
  SleepConditionVariableCS(&condvar_, &(mutex_->cs_), INFINITE);
}

void WinCondvar::TimedWait(int64 timeout_ms) {
  SleepConditionVariableCS(&condvar_, &(mutex_->cs_), timeout_ms);
}

void WinCondvar::Init() { InitializeConditionVariable(&condvar_); }
}  // namespace net_instaweb
