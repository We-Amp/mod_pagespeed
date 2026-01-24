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

// EventScheduler is a Scheduler implementation that uses an EventDispatcher
// for timer operations instead of condition variable waits. This allows the
// Scheduler to integrate with event-driven systems like Envoy without needing
// a separate SchedulerThread.
//
// The key insight is that Scheduler's ProcessAlarmsOrWaitUs() calls
// AwaitWakeupUntilUs() to block until a timeout. By overriding this method
// to use the dispatcher's timer system, we can eliminate the need for a
// separate thread and integrate directly with the event loop.

#ifndef PAGESPEED_KERNEL_THREAD_EVENT_SCHEDULER_H_
#define PAGESPEED_KERNEL_THREAD_EVENT_SCHEDULER_H_

#include <atomic>

#include "pagespeed/kernel/base/basictypes.h"
#include "pagespeed/kernel/thread/scheduler.h"

namespace net_instaweb {

class EventDispatcher;
class EventTimer;
class ThreadSystem;
class Timer;

// A Scheduler that uses an EventDispatcher for timer operations.
// Unlike the base Scheduler (which uses condition variable waits),
// EventScheduler integrates with an external event loop.
//
// Usage:
//   EventDispatcher* dispatcher = ...;  // EnvoyDispatcherAdapter or LibeventDispatcher
//   EventScheduler scheduler(thread_system, dispatcher);
//   // Now use scheduler normally - no SchedulerThread needed for Envoy.
//
// For event-driven systems (Envoy), the dispatcher provides the event loop,
// and the scheduler's alarms are handled via the dispatcher's timer system.
// For Apache with LibeventDispatcher, the dispatcher runs its own background
// thread with a libevent loop.
class EventScheduler : public Scheduler {
 public:
  // Creates an EventScheduler using the given dispatcher.
  // The dispatcher must outlive this scheduler.
  EventScheduler(ThreadSystem* thread_system, EventDispatcher* dispatcher);
  ~EventScheduler() override;

 protected:
  // Override to use the dispatcher's timer instead of condvar wait.
  // This is called from ProcessAlarmsOrWaitUs() when the scheduler needs
  // to wait for the next alarm.
  void AwaitWakeupUntilUs(int64 wakeup_time_us) override;

 private:
  // Callback invoked when the wakeup timer fires.
  // The generation parameter is used to ignore stale timer callbacks.
  void OnWakeupTimer(int64 generation);

  EventDispatcher* dispatcher_;

  // Generation counter to handle stale timer callbacks.
  // Each new timer gets the current generation; callbacks check if their
  // generation matches before signaling. This avoids races where an old
  // timer callback fires after we've moved on to a new timer.
  std::atomic<int64> timer_generation_;

  DISALLOW_COPY_AND_ASSIGN(EventScheduler);
};

}  // namespace net_instaweb

#endif  // PAGESPEED_KERNEL_THREAD_EVENT_SCHEDULER_H_
