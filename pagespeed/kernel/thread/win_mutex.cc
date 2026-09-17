/*
 * Copyright 2010 Google Inc.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

// Author: jmarantz@google.com (Joshua Marantz)

#include "pagespeed/kernel/thread/win_mutex.h"

#include "pagespeed/kernel/base/thread_system.h"
#include "pagespeed/kernel/thread/win_condvar.h"

namespace net_instaweb {

WinMutex::WinMutex() { InitializeCriticalSection(&cs_); }

WinMutex::~WinMutex() { DeleteCriticalSection(&cs_); }

bool WinMutex::TryLock() { return TryEnterCriticalSection(&cs_); }

void WinMutex::Lock() { EnterCriticalSection(&cs_); }

void WinMutex::Unlock() { LeaveCriticalSection(&cs_); }

ThreadSystem::Condvar* WinMutex::NewCondvar() { return new WinCondvar(this); }

}  // namespace net_instaweb
