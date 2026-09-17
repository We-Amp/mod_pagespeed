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

#include "pagespeed/kernel/thread/dispatcher_sequence.h"

#ifdef _WIN32
#include <windows.h>
// Windows API defines InitiateShutdown as a macro in winreg.h (included by
// windows.h). We need to undefine it to avoid collision with our method.
#ifdef InitiateShutdown
#undef InitiateShutdown
#endif
#else
#include <unistd.h>
#endif

#include "pagespeed/kernel/base/abstract_mutex.h"
#include "pagespeed/kernel/base/function.h"
#include "pagespeed/kernel/thread/event_dispatcher.h"

namespace net_instaweb {

DispatcherSequence::DispatcherSequence(EventDispatcher* dispatcher,
                                       ThreadSystem* thread_system)
    : dispatcher_(dispatcher),
      mutex_(thread_system->NewMutex()),
      running_task_(false),
      shutting_down_(false),
      shutdown_complete_(false) {}

DispatcherSequence::~DispatcherSequence() {
  // Cancel any remaining tasks.
  ScopedMutex lock(mutex_.get());
  while (!queue_.empty()) {
    Function* f = queue_.front();
    queue_.pop_front();
    f->CallCancel();
  }
}

void DispatcherSequence::Add(Function* function) {
  ScopedMutex lock(mutex_.get());

  if (shutting_down_) {
    // During shutdown, immediately cancel new tasks.
    function->CallCancel();
    return;
  }

  queue_.push_back(function);

  // If no task is currently running, schedule the next one.
  if (!running_task_) {
    running_task_ = true;
    // Post a callback to run the next task.
    dispatcher_->Post(MakeFunction(this, &DispatcherSequence::RunNextTask));
  }
}

void DispatcherSequence::InitiateShutdown() {
  ScopedMutex lock(mutex_.get());
  shutting_down_ = true;

  // Cancel all queued tasks.
  while (!queue_.empty()) {
    Function* f = queue_.front();
    queue_.pop_front();
    f->CallCancel();
  }

  // If no task is running, shutdown is complete.
  if (!running_task_) {
    shutdown_complete_ = true;
  }
}

void DispatcherSequence::WaitForShutdown() {
  // Simple poll loop. In practice, callers should ensure
  // InitiateShutdown() is called and any in-flight tasks complete quickly.
  while (true) {
    {
      ScopedMutex lock(mutex_.get());
      if (shutdown_complete_) {
        return;
      }
    }
    // Release lock between checks to allow other threads to make progress.
    // Brief sleep to avoid spinning.
#ifdef _WIN32
    Sleep(0);  // Yield time slice.
#else
    usleep(100);  // 100 microseconds
#endif
  }
}

void DispatcherSequence::RunNextTask() {
  Function* function = nullptr;

  {
    ScopedMutex lock(mutex_.get());
    if (shutting_down_ || queue_.empty()) {
      running_task_ = false;
      if (shutting_down_) {
        shutdown_complete_ = true;
      }
      return;
    }

    function = queue_.front();
    queue_.pop_front();
  }

  // Run outside the lock to avoid deadlocks.
  function->CallRun();

  // After task completes, check for more work.
  TaskComplete();
}

void DispatcherSequence::TaskComplete() {
  ScopedMutex lock(mutex_.get());

  if (shutting_down_) {
    // Cancel remaining tasks.
    while (!queue_.empty()) {
      Function* f = queue_.front();
      queue_.pop_front();
      f->CallCancel();
    }
    running_task_ = false;
    shutdown_complete_ = true;
    return;
  }

  if (!queue_.empty()) {
    // More work to do - schedule the next task.
    dispatcher_->Post(MakeFunction(this, &DispatcherSequence::RunNextTask));
  } else {
    running_task_ = false;
  }
}

}  // namespace net_instaweb
