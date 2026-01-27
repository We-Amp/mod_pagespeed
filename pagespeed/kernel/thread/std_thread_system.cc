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

#include "pagespeed/kernel/thread/std_thread_system.h"

#include <thread>

#include "pagespeed/kernel/base/basictypes.h"
#include "pagespeed/kernel/base/thread.h"
#include "pagespeed/kernel/base/thread_system.h"
#include "pagespeed/kernel/base/timer.h"
#include "pagespeed/kernel/base/std_timer.h"
#include "pagespeed/kernel/thread/std_mutex.h"
#include "pagespeed/kernel/thread/std_rw_lock.h"

namespace net_instaweb {

namespace {

// Thread ID wrapper using std::thread::id
class StdId : public ThreadSystem::ThreadId {
 public:
  StdId() : id_(std::this_thread::get_id()) {}
  ~StdId() override {}

  bool IsEqual(const ThreadId& that) const override {
    return id_ == dynamic_cast<const StdId&>(that).id_;
  }

  bool IsCurrentThread() const override {
    return id_ == std::this_thread::get_id();
  }

 private:
  std::thread::id id_;

  DISALLOW_COPY_AND_ASSIGN(StdId);
};

}  // namespace

// Thread implementation using std::thread
class StdThreadImpl : public ThreadSystem::ThreadImpl {
 public:
  StdThreadImpl(StdThreadSystem* thread_system, ThreadSystem::Thread* wrapper,
                ThreadSystem::ThreadFlags flags)
      : thread_system_(thread_system), wrapper_(wrapper), flags_(flags) {}

  ~StdThreadImpl() override {
    // If thread is joinable and still running, we need to detach it to avoid
    // std::terminate being called when the thread object is destroyed.
    if (thread_.joinable()) {
      thread_.detach();
    }
  }

  bool StartImpl() override {
    try {
      thread_ = std::thread(&StdThreadImpl::InvokeRun, this);
      if ((flags_ & ThreadSystem::kJoinable) == 0) {
        thread_.detach();
      }
      return true;
    } catch (...) {
      return false;
    }
  }

  void JoinImpl() override {
    if (thread_.joinable()) {
      thread_.join();
    }
  }

 private:
  void InvokeRun() {
    thread_system_->BeforeThreadRunHook();
    // Note: Unlike pthreads, std::thread doesn't have a portable way to set
    // the thread name. On Linux, we could use pthread_setname_np, but that
    // defeats the purpose of using std::thread for portability. Thread naming
    // is optional and used mainly for debugging, so we skip it here.
    wrapper_->Run();
  }

  StdThreadSystem* thread_system_;
  ThreadSystem::Thread* wrapper_;
  ThreadSystem::ThreadFlags flags_;
  std::thread thread_;

  DISALLOW_COPY_AND_ASSIGN(StdThreadImpl);
};

StdThreadSystem::StdThreadSystem() {}

StdThreadSystem::~StdThreadSystem() {}

ThreadSystem::CondvarCapableMutex* StdThreadSystem::NewMutex() {
  return new StdMutex;
}

ThreadSystem::RWLock* StdThreadSystem::NewRWLock() { return new StdRWLock; }

void StdThreadSystem::BeforeThreadRunHook() {}

ThreadSystem::ThreadImpl* StdThreadSystem::NewThreadImpl(
    ThreadSystem::Thread* wrapper, ThreadSystem::ThreadFlags flags) {
  return new StdThreadImpl(this, wrapper, flags);
}

Timer* StdThreadSystem::NewTimer() { return new StdTimer; }

ThreadSystem::ThreadId* StdThreadSystem::GetThreadId() const {
  return new StdId;
}

}  // namespace net_instaweb
