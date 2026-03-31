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

#ifndef PAGESPEED_KERNEL_THREAD_STD_RW_LOCK_H_
#define PAGESPEED_KERNEL_THREAD_STD_RW_LOCK_H_

#include <condition_variable>
#include <mutex>

#include "pagespeed/kernel/base/basictypes.h"
#include "pagespeed/kernel/base/thread_system.h"

namespace net_instaweb {

// Implementation of RWLock using C++ standard library primitives.
// This provides cross-platform reader-writer lock support with writer priority
// to prevent writer starvation. Note that std::shared_mutex does NOT guarantee
// writer priority, so we implement a custom solution using std::mutex and
// std::condition_variable.
//
// Writer priority means: when a writer is waiting, new readers must wait for
// the writer to complete before they can acquire the lock. This prevents
// a continuous stream of readers from starving waiting writers.
class StdRWLock : public ThreadSystem::RWLock {
 public:
  StdRWLock();
  ~StdRWLock() override;

  // Writer methods (exclusive lock)
  bool TryLock() override;
  void Lock() override;
  void Unlock() override;

  // Reader methods (shared lock)
  bool ReaderTryLock() override;
  void ReaderLock() override;
  void ReaderUnlock() override;

 private:
  std::mutex mutex_;
  std::condition_variable readers_cv_;
  std::condition_variable writers_cv_;
  int readers_count_;
  int writers_waiting_;
  bool writer_active_;

  StdRWLock(const StdRWLock&) = delete;
  StdRWLock& operator=(const StdRWLock&) = delete;
};

}  // namespace net_instaweb

#endif  // PAGESPEED_KERNEL_THREAD_STD_RW_LOCK_H_
