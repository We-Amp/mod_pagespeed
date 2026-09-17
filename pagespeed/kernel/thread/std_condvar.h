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

#ifndef PAGESPEED_KERNEL_THREAD_STD_CONDVAR_H_
#define PAGESPEED_KERNEL_THREAD_STD_CONDVAR_H_

#include <condition_variable>

#include "pagespeed/kernel/base/basictypes.h"
#include "pagespeed/kernel/base/condvar.h"
#include "pagespeed/kernel/base/thread_system.h"
#include "pagespeed/kernel/thread/std_mutex.h"

namespace net_instaweb {

// Implementation of ThreadSystem::Condvar using std::condition_variable.
// This provides cross-platform condition variable support using the C++
// standard library.
class StdCondvar : public ThreadSystem::Condvar {
 public:
  // The mutex is owned by the caller and must outlive the condvar.
  explicit StdCondvar(StdMutex* mutex);
  ~StdCondvar() override;

  StdMutex* mutex() const override { return mutex_; }

  void Signal() override;
  void Broadcast() override;
  void Wait() override;
  void TimedWait(int64 timeout_ms) override;

 private:
  StdMutex* mutex_;
  std::condition_variable condvar_;

  StdCondvar(const StdCondvar&) = delete;
  StdCondvar& operator=(const StdCondvar&) = delete;
};

}  // namespace net_instaweb

#endif  // PAGESPEED_KERNEL_THREAD_STD_CONDVAR_H_
