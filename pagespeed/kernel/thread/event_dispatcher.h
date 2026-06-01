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

// Defines an abstract EventDispatcher interface that unifies event loop
// handling across different backends (Envoy dispatcher, libevent for Apache).
//
// This abstraction allows mod_pagespeed to use Envoy's native dispatcher when
// running as an Envoy filter, eliminating the need for a separate
// SchedulerThread, while providing a standalone libevent-based backend for
// Apache deployments.

#ifndef PAGESPEED_KERNEL_THREAD_EVENT_DISPATCHER_H_
#define PAGESPEED_KERNEL_THREAD_EVENT_DISPATCHER_H_

#include <atomic>

#include "pagespeed/kernel/base/basictypes.h"

namespace net_instaweb {

class Function;
class Timer;

// Represents a timer that can be cancelled. Timers are one-shot: they fire
// once and then must be recreated if needed again.
class EventTimer {
 public:
  EventTimer() : cancelled_(false) {}
  virtual ~EventTimer() = default;

  // Cancels the timer, preventing the callback from being invoked.
  // If the callback is already running, this has no effect.
  // Thread-safe: can be called from any thread.
  virtual void Cancel() = 0;

  // Returns true if the timer has been cancelled.
  bool IsCancelled() const {
    return cancelled_.load(std::memory_order_acquire);
  }

 protected:
  // Implementations should set this when Cancel() is called.
  std::atomic<bool> cancelled_;

 private:
  EventTimer(const EventTimer&) = delete;
  EventTimer& operator=(const EventTimer&) = delete;
};

// Abstract interface for an event dispatcher that can post callbacks and
// create timers. This unifies Envoy's native dispatcher with a standalone
// libevent backend.
//
// Thread-safety: Post() and CreateTimer() are thread-safe and may be called
// from any thread. The callbacks will execute in the dispatcher's thread.
class EventDispatcher {
 public:
  EventDispatcher() : shutting_down_(false) {}
  virtual ~EventDispatcher() = default;

  // Posts a function to be run on the dispatcher's event loop.
  // The function will be called at some point in the future when the event
  // loop processes pending callbacks.
  // Ownership of the function passes to the dispatcher.
  // Thread-safe: can be called from any thread.
  virtual void Post(Function* function) = 0;

  // Creates a timer that will fire after delay_us microseconds.
  // Ownership of the function passes to the timer (and will be deleted
  // when the timer fires or is cancelled).
  // Returns ownership of the timer to the caller.
  // Thread-safe: can be called from any thread.
  virtual EventTimer* CreateTimer(int64 delay_us, Function* function) = 0;

  // Creates a timer that will fire at the specified absolute time.
  // wakeup_time_us is microseconds since epoch.
  // If wakeup_time_us is in the past, the timer fires immediately.
  // Ownership of the function passes to the timer.
  // Returns ownership of the timer to the caller.
  // Thread-safe: can be called from any thread.
  virtual EventTimer* CreateTimerAtUs(int64 wakeup_time_us,
                                      Function* function) = 0;

  // Returns the timer used by this dispatcher for time measurements.
  virtual Timer* timer() const = 0;

  // Initiates shutdown of the dispatcher. After this call:
  // - New timers will fire their Cancel callbacks immediately
  // - Posted functions may or may not run (implementation-defined)
  // - WaitForShutdown() can be called to block until shutdown completes
  virtual void InitiateShutdown() {
    shutting_down_.store(true, std::memory_order_release);
  }

  // Returns true if shutdown has been initiated.
  bool IsShuttingDown() const {
    return shutting_down_.load(std::memory_order_acquire);
  }

  // Blocks until shutdown is complete. Must be called after InitiateShutdown().
  // After this returns, no more callbacks will be invoked.
  virtual void WaitForShutdown() = 0;

 protected:
  std::atomic<bool> shutting_down_;

 private:
  EventDispatcher(const EventDispatcher&) = delete;
  EventDispatcher& operator=(const EventDispatcher&) = delete;
};

// Simple EventTimer implementation that wraps a Function and tracks
// cancellation state. Useful as a base for concrete implementations.
class BasicEventTimer : public EventTimer {
 public:
  explicit BasicEventTimer(Function* function);
  ~BasicEventTimer() override;

  void Cancel() override;

  // Returns the function, or nullptr if cancelled/fired.
  // After calling this, the timer no longer owns the function.
  Function* ReleaseFunction();

 protected:
  Function* function_;

 private:
  BasicEventTimer(const BasicEventTimer&) = delete;
  BasicEventTimer& operator=(const BasicEventTimer&) = delete;
};

}  // namespace net_instaweb

#endif  // PAGESPEED_KERNEL_THREAD_EVENT_DISPATCHER_H_
