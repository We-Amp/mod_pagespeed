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

// Contains SchedulerThread, used to run the Scheduler dispatch loop for
// non-blocking servers.
//
// DEPRECATED: This class is deprecated. Use LibeventDispatcher with
// EventScheduler instead, which provides the same functionality with a
// unified EventDispatcher abstraction:
//
//   // Old way (deprecated):
//   SchedulerThread* thread = new SchedulerThread(thread_system, scheduler);
//   thread->Start();
//
//   // New way:
//   auto dispatcher = std::make_unique<LibeventDispatcher>(thread_system, timer);
//   auto scheduler = std::make_unique<EventScheduler>(thread_system, dispatcher.get());
//   dispatcher->Start();
//
// The new approach allows sharing scheduling infrastructure between different
// deployment modes (Apache uses LibeventDispatcher, Envoy uses
// EnvoyDispatcherAdapter wrapping Envoy's native dispatcher).

#ifndef PAGESPEED_KERNEL_THREAD_SCHEDULER_THREAD_H_
#define PAGESPEED_KERNEL_THREAD_SCHEDULER_THREAD_H_

#include "pagespeed/kernel/base/basictypes.h"
#include "pagespeed/kernel/base/thread.h"
#include "pagespeed/kernel/base/thread_system.h"

namespace net_instaweb {

class Function;
class Scheduler;

// This class is a helper used to dispatch events on a scheduler in a thread
// in case where the server infrastructure is non-blocking and therefore does
// not provide a natural way to do it.
class SchedulerThread : public ThreadSystem::Thread {
 public:
  // Creates the thread. The user still needs to call Start() manually.
  SchedulerThread(ThreadSystem* thread_system, Scheduler* scheduler);

  // Returns a function that, when run, will properly synchronize with this
  // thread and shut it down cleanly, deleting the object as well.
  // It is suggested for use with RewriteDriverFactory::defer_cleanup(); as it
  // needs to be run after it's OK if scheduler timeouts no longer work.
  Function* MakeDeleter();

 protected:
  void Run() override;

 private:
  class CleanupFunction;
  friend class CleanupFunction;

  ~SchedulerThread() override;

  bool quit_;
  Scheduler* scheduler_;

  SchedulerThread(const SchedulerThread&) = delete;
  SchedulerThread& operator=(const SchedulerThread&) = delete;
};

}  // namespace net_instaweb

#endif  // PAGESPEED_KERNEL_THREAD_SCHEDULER_THREAD_H_
