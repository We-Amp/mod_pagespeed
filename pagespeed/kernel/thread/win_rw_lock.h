/*
 * Copyright 2011 Google Inc.
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

// Author: pulkitg@google.com (Pulkit Goyal)

#ifndef WIN_RW_LOCK_H
#define WIN_RW_LOCK_H

// clang-format off
#include <windows.h>  // Must precede WinBase.h
#include <WinBase.h>
// clang-format on

#include "pagespeed/kernel/base/basictypes.h"
#include "pagespeed/kernel/base/thread_system.h"

namespace net_instaweb {

// Implementation of RWLock for Pthread mutexes.
class WinRWLock : public ThreadSystem::RWLock {
 public:
  WinRWLock();
  virtual ~WinRWLock();
  virtual bool TryLock();
  virtual void Lock();
  virtual void Unlock();
  virtual bool ReaderTryLock();
  virtual void ReaderLock();
  virtual void ReaderUnlock();

 private:
  SRWLOCK srwlock_;
  //pthread_rwlock_t rwlock_;
  //pthread_rwlockattr_t attr_;

  WinRWLock(const WinRWLock&) = delete;
  WinRWLock& operator=(const WinRWLock&) = delete;
};

}  // namespace net_instaweb

#endif  // WIN_RW_LOCK_H
