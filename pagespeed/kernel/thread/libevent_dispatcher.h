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

// LibeventDispatcher is a standalone EventDispatcher implementation using
// libevent. It runs its own background thread with a libevent event loop,
// suitable for Apache deployments where there's no external event loop
// to integrate with.

#ifndef PAGESPEED_KERNEL_THREAD_LIBEVENT_DISPATCHER_H_
#define PAGESPEED_KERNEL_THREAD_LIBEVENT_DISPATCHER_H_

#include <atomic>
#include <deque>
#include <memory>

#include "pagespeed/kernel/base/abstract_mutex.h"
#include "pagespeed/kernel/base/basictypes.h"
#include "pagespeed/kernel/base/thread.h"
#include "pagespeed/kernel/base/thread_system.h"
#include "pagespeed/kernel/thread/event_dispatcher.h"

// Forward declarations for libevent types
struct event_base;
struct event;

// evutil_socket_t is typically int on Unix
using evutil_socket_t = int;

namespace net_instaweb {

class Function;
class Timer;

// Timer implementation using libevent events.
class LibeventTimer : public EventTimer {
 public:
  LibeventTimer(struct event_base* base, int64 delay_us, Function* function);
  ~LibeventTimer() override;

  void Cancel() override;

  // Starts the timer. Must be called after construction.
  void Enable();

 private:
  // Callback invoked by libevent when the timer fires.
  static void OnTimer(evutil_socket_t fd, short what, void* arg);

  struct event* event_;
  Function* function_;
  int64 delay_us_;

  LibeventTimer(const LibeventTimer&) = delete;
  LibeventTimer& operator=(const LibeventTimer&) = delete;
};

// EventDispatcher implementation using libevent.
// Runs a background thread with a libevent event loop.
class LibeventDispatcher : public EventDispatcher {
 public:
  // Creates a dispatcher. Call Start() to begin the event loop.
  LibeventDispatcher(ThreadSystem* thread_system, Timer* timer);
  ~LibeventDispatcher() override;

  // Starts the background event loop thread.
  // Returns true on success, false on failure.
  bool Start();

  // EventDispatcher interface
  void Post(Function* function) override;
  EventTimer* CreateTimer(int64 delay_us, Function* function) override;
  EventTimer* CreateTimerAtUs(int64 wakeup_time_us,
                              Function* function) override;
  Timer* timer() const override { return timer_; }
  void InitiateShutdown() override;
  void WaitForShutdown() override;

 private:
  // Background thread that runs the libevent loop.
  class DispatcherThread;

  // Processes posted functions. Called from libevent callback.
  void ProcessPostedFunctions();

  // Wakes up the event loop to process posted functions.
  void WakeupEventLoop();

  // Callback for the wakeup event.
  static void OnWakeup(evutil_socket_t fd, short what, void* arg);

  ThreadSystem* thread_system_;
  Timer* timer_;
  struct event_base* event_base_;
  struct event* wakeup_event_;

  // Pipe used to wake up the event loop for posted functions.
  int wakeup_pipe_[2];

  std::unique_ptr<AbstractMutex> mutex_;
  std::deque<Function*> posted_functions_;  // GUARDED_BY(mutex_)

  std::unique_ptr<DispatcherThread> thread_;
  std::atomic<bool> started_;
  std::atomic<bool> shutdown_complete_;

  LibeventDispatcher(const LibeventDispatcher&) = delete;
  LibeventDispatcher& operator=(const LibeventDispatcher&) = delete;
};

}  // namespace net_instaweb

#endif  // PAGESPEED_KERNEL_THREAD_LIBEVENT_DISPATCHER_H_
