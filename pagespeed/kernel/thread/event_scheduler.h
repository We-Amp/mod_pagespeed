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

// EventScheduler is a Scheduler whose alarms are driven by an
// EventDispatcher's event loop, so they fire on time even when no thread is
// blocked in the scheduler. The base Scheduler runs alarms only
// opportunistically: when some thread happens to call into it (e.g. a
// SchedulerBlockingFunction waiter) or when a dedicated SchedulerThread
// polls ProcessAlarmsOrWaitUs(). EventScheduler replaces the dedicated
// thread: whenever the earliest alarm deadline moves earlier
// (EarliestWakeupChangedMutexHeld), it posts a pump to the dispatcher; the
// pump runs due alarms via RunAlarms() on the dispatcher thread and keeps a
// one-shot dispatcher timer armed at the next deadline.
//
// The dispatcher may be attached after construction (AttachDispatcher) to
// accommodate factories whose scheduler is created lazily, before the event
// loop exists. Until a dispatcher is attached, behavior is identical to the
// base Scheduler (alarms fire opportunistically); attaching pumps any
// already-scheduled alarms.
//
// Shutdown contract: call DetachDispatcher() before the dispatcher stops
// being able to run posted callbacks, at a point where no dispatcher
// callback is concurrently executing (i.e. after the dispatcher's event
// loop has quiesced, or from the dispatcher thread itself). After detach,
// pump callbacks still queued in the dispatcher become no-ops. Detach is
// terminal: re-attach is not supported.
//
// Alarm callbacks execute on the dispatcher thread, so they must honor the
// Scheduler contract of being lightweight and short-lived; anything
// expensive must be handed off to a worker.

#ifndef PAGESPEED_KERNEL_THREAD_EVENT_SCHEDULER_H_
#define PAGESPEED_KERNEL_THREAD_EVENT_SCHEDULER_H_

#include <atomic>
#include <memory>

#include "pagespeed/kernel/base/basictypes.h"
#include "pagespeed/kernel/thread/scheduler.h"

namespace net_instaweb {

class EventDispatcher;
class EventTimer;
class ThreadSystem;
class Timer;

class EventScheduler : public Scheduler {
 public:
  // Creates an EventScheduler already attached to the given dispatcher.
  // The dispatcher must outlive this scheduler (see shutdown contract above).
  EventScheduler(ThreadSystem* thread_system, EventDispatcher* dispatcher);

  // Creates an EventScheduler with no dispatcher yet; behaves exactly like
  // the base Scheduler until AttachDispatcher() is called. Use this when the
  // scheduler must be created before the event loop exists.
  EventScheduler(ThreadSystem* thread_system, Timer* timer);

  ~EventScheduler() override;

  // Attaches the dispatcher that will drive alarms, and pumps any alarms
  // scheduled before the attach. May be called at most once, and only if
  // this scheduler was constructed without a dispatcher.
  void AttachDispatcher(EventDispatcher* dispatcher);

  // Stops driving alarms; queued pump callbacks become no-ops and the armed
  // pump timer is cancelled. Must be called while the dispatcher object is
  // still alive but with its event loop quiesced (or from the dispatcher
  // thread). Safe to call when never attached, and idempotent.
  void DetachDispatcher();

 protected:
  // Scheduler override: called with the scheduler mutex held whenever the
  // earliest alarm deadline moves earlier. Requests a pump; must not block.
  void EarliestWakeupChangedMutexHeld(int64 wakeup_time_us) override;

 private:
  class PumpFunction;

  // Posts a (coalesced) pump to the dispatcher, if one is attached.
  void RequestPump();

  // Runs on the dispatcher thread: runs due alarms and re-arms the pump
  // timer for the next deadline.
  void PumpOnDispatcherThread();

  // Null until attached, null again after detach.
  std::atomic<EventDispatcher*> dispatcher_;

  // Coalesces RequestPump() posts: set when a pump has been posted and not
  // yet started, cleared at pump start so later deadline changes re-post.
  std::atomic<bool> pump_posted_;

  // Cleared on detach/destruction; pump callbacks that were already queued
  // in the dispatcher check it before touching this object.
  std::shared_ptr<std::atomic<bool>> alive_;

  // The one-shot timer armed at the next alarm deadline. Created, replaced
  // and destroyed on the dispatcher thread only (except in DetachDispatcher,
  // whose contract guarantees the loop is quiesced).
  std::unique_ptr<EventTimer> pump_timer_;

  // The previously armed timer, kept alive for one extra pump: a pump may be
  // executing from pump_timer_'s own callback, and destroying an EventTimer
  // from inside its own callback is not guaranteed safe by every backend.
  // Pumps are serialized on the dispatcher thread, so by the time the NEXT
  // pump destroys this, its callback frame has finished.
  std::unique_ptr<EventTimer> retired_pump_timer_;

  EventScheduler(const EventScheduler&) = delete;
  EventScheduler& operator=(const EventScheduler&) = delete;
};

}  // namespace net_instaweb

#endif  // PAGESPEED_KERNEL_THREAD_EVENT_SCHEDULER_H_
