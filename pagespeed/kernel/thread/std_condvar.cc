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

#include "pagespeed/kernel/thread/std_condvar.h"

#include <chrono>
#include <condition_variable>

#include "pagespeed/kernel/base/basictypes.h"
#include "pagespeed/kernel/thread/std_mutex.h"

namespace net_instaweb {

StdCondvar::StdCondvar(StdMutex* mutex) : mutex_(mutex) {}

StdCondvar::~StdCondvar() {}

void StdCondvar::Signal() { condvar_.notify_one(); }

void StdCondvar::Broadcast() { condvar_.notify_all(); }

void StdCondvar::Wait() {
  // The mutex must already be held by the caller. We use adopt_lock to tell
  // unique_lock that we already own the lock. After wait() returns, we must
  // release() to prevent unique_lock from unlocking in its destructor (since
  // the caller expects to still hold the lock).
  std::unique_lock<std::mutex> lock(mutex_->mutex_, std::adopt_lock);
  condvar_.wait(lock);
  lock.release();
}

void StdCondvar::TimedWait(int64 timeout_ms) {
  // Same pattern as Wait() - we already hold the lock.
  std::unique_lock<std::mutex> lock(mutex_->mutex_, std::adopt_lock);
  condvar_.wait_for(lock, std::chrono::milliseconds(timeout_ms));
  lock.release();
}

}  // namespace net_instaweb
