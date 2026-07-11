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
#include <utility>

#include "base/logging.h"
#include "pagespeed/kernel/base/abstract_mutex.h"
#include "pagespeed/kernel/base/function.h"
#include "pagespeed/kernel/base/timer.h"
#include "pagespeed/kernel/thread/event_dispatcher.h"

namespace net_instaweb {

// The pump callback, used both for dispatcher posts and as the pump timer's
// callback. Holds the alive token so a pump that was queued before
// DetachDispatcher()/destruction degrades to a no-op instead of touching a
// dead scheduler.
class EventScheduler::PumpFunction : public Function {
 public:
  PumpFunction(EventScheduler* scheduler,
               std::shared_ptr<std::atomic<bool>> alive)
      : scheduler_(scheduler), alive_(std::move(alive)) {}

 protected:
  void Run() override {
    if (alive_->load(std::memory_order_acquire)) {
      scheduler_->PumpOnDispatcherThread();
    }
  }
  void Cancel() override {}

 private:
  EventScheduler* scheduler_;
  std::shared_ptr<std::atomic<bool>> alive_;
};

EventScheduler::EventScheduler(ThreadSystem* thread_system,
                               EventDispatcher* dispatcher)
    : Scheduler(thread_system, dispatcher->timer()),
      dispatcher_(dispatcher),
      pump_posted_(false),
      alive_(std::make_shared<std::atomic<bool>>(true)) {}

EventScheduler::EventScheduler(ThreadSystem* thread_system, Timer* timer)
    : Scheduler(thread_system, timer),
      dispatcher_(nullptr),
      pump_posted_(false),
      alive_(std::make_shared<std::atomic<bool>>(true)) {}

EventScheduler::~EventScheduler() { DetachDispatcher(); }

void EventScheduler::AttachDispatcher(EventDispatcher* dispatcher) {
  CHECK(dispatcher != nullptr);
  CHECK(alive_->load(std::memory_order_acquire))
      << "AttachDispatcher after DetachDispatcher";
  EventDispatcher* previous =
      dispatcher_.exchange(dispatcher, std::memory_order_acq_rel);
  CHECK(previous == nullptr) << "AttachDispatcher called twice";
  // Catch up: alarms may have been scheduled while unattached, with nothing
  // driving them. A spurious pump (no alarms pending) is harmless.
  RequestPump();
}

void EventScheduler::DetachDispatcher() {
  alive_->store(false, std::memory_order_release);
  dispatcher_.store(nullptr, std::memory_order_release);
  // Contract: the dispatcher's loop is quiesced (or we're on its thread),
  // so cancelling the timers here cannot race their callbacks.
  pump_timer_.reset();
  retired_pump_timer_.reset();
}

void EventScheduler::EarliestWakeupChangedMutexHeld(int64 wakeup_time_us) {
  // Called with the scheduler mutex held, from any thread that inserts an
  // alarm. Post() is non-blocking for all EventDispatcher implementations,
  // so this cannot deadlock against the pump (which takes the mutex).
  RequestPump();
}

void EventScheduler::RequestPump() {
  EventDispatcher* dispatcher = dispatcher_.load(std::memory_order_acquire);
  if (dispatcher == nullptr) {
    return;  // Not attached: base Scheduler semantics apply.
  }
  if (!pump_posted_.exchange(true, std::memory_order_acq_rel)) {
    dispatcher->Post(new PumpFunction(this, alive_));
  }
}

void EventScheduler::PumpOnDispatcherThread() {
  // Clear the coalescing flag before running alarms, so a deadline change
  // that happens during RunAlarms() (alarm callbacks may drop the scheduler
  // mutex and insert new alarms) posts a fresh pump rather than being lost.
  pump_posted_.store(false, std::memory_order_release);

  EventDispatcher* dispatcher = dispatcher_.load(std::memory_order_acquire);
  if (dispatcher == nullptr || dispatcher->IsShuttingDown()) {
    return;
  }

  int64 next_wakeup_us = 0;
  {
    ScopedMutex lock(mutex());
    next_wakeup_us = RunAlarms(nullptr);
  }

  // Re-arm the one-shot timer at the next deadline. This pump may itself be
  // running from pump_timer_'s callback, so the current timer is cancelled
  // and retired rather than destroyed: destruction is deferred to the next
  // pump, by which point its callback frame has finished (pumps are
  // serialized on the dispatcher thread). Cancelling from within the timer's
  // own callback is safe in both backends, and a retired-but-unfired timer
  // that slips through only triggers a harmless extra pump.
  if (pump_timer_ != nullptr) {
    pump_timer_->Cancel();
  }
  retired_pump_timer_ = std::move(pump_timer_);
  if (next_wakeup_us != 0) {
    pump_timer_.reset(dispatcher->CreateTimerAtUs(
        next_wakeup_us, new PumpFunction(this, alive_)));
  }
}

}  // namespace net_instaweb
