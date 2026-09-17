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

// Adapted from pthreads implementation.
// pthread_create replaced with std::thread: this is the Windows build, which
// has no pthreads.

#include "pagespeed/kernel/thread/win_thread_system.h"

#include <thread>

#include "pagespeed/kernel/base/basictypes.h"
#include "pagespeed/kernel/base/std_timer.h"
#include "pagespeed/kernel/base/thread.h"
#include "pagespeed/kernel/base/thread_system.h"
#include "pagespeed/kernel/thread/win_mutex.h"
#include "pagespeed/kernel/thread/win_rw_lock.h"

namespace net_instaweb {

class Timer;

namespace {

class WinThreadId : public ThreadSystem::ThreadId {
 public:
  WinThreadId() : id_(std::this_thread::get_id()) {}
  ~WinThreadId() override {}

  bool IsEqual(const ThreadId& that) const override {
    return id_ == dynamic_cast<const WinThreadId&>(that).id_;
  }

  bool IsCurrentThread() const override {
    return id_ == std::this_thread::get_id();
  }

 private:
  std::thread::id id_;

  WinThreadId(const WinThreadId&) = delete;
  WinThreadId& operator=(const WinThreadId&) = delete;
};

}  // namespace

class WinThreadImpl : public ThreadSystem::ThreadImpl {
 public:
  WinThreadImpl(WinThreadSystem* thread_system, ThreadSystem::Thread* wrapper,
                ThreadSystem::ThreadFlags flags)
      : thread_system_(thread_system), wrapper_(wrapper), flags_(flags) {}

  virtual ~WinThreadImpl() {
    // If the thread was detached and is still joinable, detach it now.
    if (thread_.joinable() && (flags_ & ThreadSystem::kJoinable) == 0) {
      thread_.detach();
    }
  }

  virtual bool StartImpl() {
    try {
      thread_ = std::thread(&WinThreadImpl::InvokeRun, this);
      if ((flags_ & ThreadSystem::kJoinable) == 0) {
        thread_.detach();
      }
      return true;
    } catch (...) {
      return false;
    }
  }

  virtual void JoinImpl() {
    if (thread_.joinable()) {
      thread_.join();
    }
  }

 private:
  static void InvokeRun(WinThreadImpl* self) {
    self->thread_system_->BeforeThreadRunHook();
    self->wrapper_->Run();
  }

  WinThreadSystem* thread_system_;
  ThreadSystem::Thread* wrapper_;
  ThreadSystem::ThreadFlags flags_;
  std::thread thread_;

  WinThreadImpl(const WinThreadImpl&) = delete;
  WinThreadImpl& operator=(const WinThreadImpl&) = delete;
};

WinThreadSystem::WinThreadSystem() {}

WinThreadSystem::~WinThreadSystem() {}

ThreadSystem::CondvarCapableMutex* WinThreadSystem::NewMutex() {
  return new WinMutex;
}

ThreadSystem::RWLock* WinThreadSystem::NewRWLock() { return new WinRWLock; }

void WinThreadSystem::BeforeThreadRunHook() {}

ThreadSystem::ThreadImpl* WinThreadSystem::NewThreadImpl(
    ThreadSystem::Thread* wrapper, ThreadSystem::ThreadFlags flags) {
  return new WinThreadImpl(this, wrapper, flags);
}

Timer* WinThreadSystem::NewTimer() { return new StdTimer; }

ThreadSystem::ThreadId* WinThreadSystem::GetThreadId() const {
  return new WinThreadId;
}

}  // namespace net_instaweb
