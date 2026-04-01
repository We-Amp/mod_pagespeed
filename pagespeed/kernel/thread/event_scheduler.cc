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

#include "pagespeed/kernel/thread/event_scheduler.h"

#include <memory>

#include "pagespeed/kernel/base/function.h"
#include "pagespeed/kernel/base/timer.h"
#include "pagespeed/kernel/thread/event_dispatcher.h"

namespace net_instaweb {

EventScheduler::EventScheduler(ThreadSystem* thread_system,
                               EventDispatcher* dispatcher)
    : Scheduler(thread_system, dispatcher->timer()),
      dispatcher_(dispatcher),
      timer_generation_(0) {}

EventScheduler::~EventScheduler() {
  // Increment generation to ignore any pending callbacks.
  timer_generation_.fetch_add(1, std::memory_order_release);
}

void EventScheduler::AwaitWakeupUntilUs(int64 wakeup_time_us) {
  // This is called with the scheduler mutex held. The base implementation
  // does a timed condvar wait. We create a timer that will signal the condvar
  // when it fires.
  //
  // Key insight: We still use the condvar wait from the base class. The timer
  // just provides an additional signal path. If the condvar times out naturally,
  // the timer callback will be a no-op (wrong generation). If the timer fires
  // first, it signals the condvar and we wake up early.
  //
  // This allows the EventScheduler to work with both single-threaded and
  // multi-threaded dispatchers.

  mutex()->DCheckLocked();

  int64 now_us = timer()->NowUs();
  if (wakeup_time_us <= now_us) {
    // No need to wait.
    return;
  }

  // Increment generation for this new timer.
  int64 generation =
      timer_generation_.fetch_add(1, std::memory_order_acq_rel) + 1;

  // Create a timer that will wake us up. We capture the generation so we can
  // ignore stale callbacks. The caller owns the returned EventTimer.
  std::unique_ptr<EventTimer> timer(dispatcher_->CreateTimerAtUs(
      wakeup_time_us,
      MakeFunction(this, &EventScheduler::OnWakeupTimer, generation)));

  // Do the actual condvar wait. This will return when either:
  // - The timeout expires
  // - OnWakeupTimer signals via Wakeup()
  // - Some other code calls Signal()
  Scheduler::AwaitWakeupUntilUs(wakeup_time_us);
  // Timer is auto-deleted here after the wait completes.
}

void EventScheduler::OnWakeupTimer(int64 generation) {
  // IMPORTANT: Do NOT acquire the scheduler mutex here!
  //
  // With evthread_use_pthreads(), event_del() blocks until any currently-
  // executing callback completes. The timer destructor in AwaitWakeupUntilUs
  // calls event_del() while holding the scheduler mutex. If this callback
  // also acquired the scheduler mutex, we'd deadlock when the condvar timeout
  // and the libevent timer fire simultaneously:
  //   Main thread: holds mutex → event_del() → waits for callback
  //   Dispatcher:  callback → waits for mutex
  //
  // This is safe without the mutex because:
  // 1. timer_generation_ is atomic - no lock needed for the check
  // 2. pthread_cond_broadcast() is safe to call without the mutex
  // 3. A "missed" broadcast (if no one is waiting) is harmless - the
  //    condvar timeout in AwaitWakeupUntilUs handles that case
  if (generation == timer_generation_.load(std::memory_order_acquire)) {
    Wakeup();
  }
}

}  // namespace net_instaweb
