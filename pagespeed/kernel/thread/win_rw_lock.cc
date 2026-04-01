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

#include "pagespeed/kernel/thread/win_rw_lock.h"

namespace net_instaweb {

WinRWLock::WinRWLock() {
  InitializeSRWLock(&srwlock_);

  // POSIX does not provide any sort of guarantee that prevents writer
  // starvation for reader-writer locks. On, Linux one can avoid
  // writer starvation as long as readers are non-recursive via the
  // call below. (PTHREAD_RWLOCK_PREFER_WRITER_NP does not work).
  //
  // Other OS's (FreeBSD, Darwin, OpenSolaris) documentation suggests
  // that they prefer writers by default.
}

WinRWLock::~WinRWLock() {}

bool WinRWLock::TryLock() { return TryAcquireSRWLockExclusive(&srwlock_); }

void WinRWLock::Lock() { AcquireSRWLockExclusive(&srwlock_); }

void WinRWLock::Unlock() { ReleaseSRWLockExclusive(&srwlock_); }

bool WinRWLock::ReaderTryLock() { return TryAcquireSRWLockShared(&srwlock_); }

void WinRWLock::ReaderLock() { AcquireSRWLockShared(&srwlock_); }

void WinRWLock::ReaderUnlock() { ReleaseSRWLockShared(&srwlock_); }

}  // namespace net_instaweb
