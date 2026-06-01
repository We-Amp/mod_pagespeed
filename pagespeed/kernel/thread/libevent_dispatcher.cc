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

#include "pagespeed/kernel/thread/libevent_dispatcher.h"

#include <event2/event.h>
#include <event2/thread.h>
#include <fcntl.h>
#include <unistd.h>

#include "base/logging.h"
#include "pagespeed/kernel/base/abstract_mutex.h"
#include "pagespeed/kernel/base/function.h"
#include "pagespeed/kernel/base/timer.h"

namespace net_instaweb {

// Background thread that runs the libevent loop.
class LibeventDispatcher::DispatcherThread : public ThreadSystem::Thread {
 public:
  DispatcherThread(ThreadSystem* thread_system, LibeventDispatcher* dispatcher)
      : Thread(thread_system, "libevent_dispatcher", ThreadSystem::kJoinable),
        dispatcher_(dispatcher) {}

 protected:
  void Run() override {
    // Run the event loop until shutdown.
    event_base_dispatch(dispatcher_->event_base_);
  }

 private:
  LibeventDispatcher* dispatcher_;
};

// LibeventTimer implementation

LibeventTimer::LibeventTimer(struct event_base* base, int64 delay_us,
                             Function* function)
    : event_(nullptr), function_(function), delay_us_(delay_us) {
  // Create a one-shot timer event.
  event_ = event_new(base, -1, 0, &LibeventTimer::OnTimer, this);
  CHECK(event_ != nullptr) << "Failed to create libevent timer";
}

LibeventTimer::~LibeventTimer() {
  if (event_ != nullptr) {
    event_del(event_);
    event_free(event_);
  }
  if (function_ != nullptr) {
    function_->CallCancel();
  }
}

void LibeventTimer::Cancel() {
  bool was_cancelled = cancelled_.exchange(true, std::memory_order_acq_rel);
  if (!was_cancelled) {
    if (event_ != nullptr) {
      event_del(event_);
    }
    if (function_ != nullptr) {
      function_->CallCancel();
      function_ = nullptr;
    }
  }
}

void LibeventTimer::Enable() {
  struct timeval tv;
  tv.tv_sec = delay_us_ / Timer::kSecondUs;
  tv.tv_usec = delay_us_ % Timer::kSecondUs;
  event_add(event_, &tv);
}

// static
void LibeventTimer::OnTimer(evutil_socket_t /*fd*/, short /*what*/, void* arg) {
  LibeventTimer* timer = static_cast<LibeventTimer*>(arg);

  // Check if cancelled before running.
  if (!timer->IsCancelled() && timer->function_ != nullptr) {
    Function* f = timer->function_;
    timer->function_ = nullptr;
    f->CallRun();
  }

  // Timer has fired; delete it.
  // Note: Caller is responsible for timer lifetime.
}

// LibeventDispatcher implementation

LibeventDispatcher::LibeventDispatcher(ThreadSystem* thread_system,
                                       Timer* timer)
    : thread_system_(thread_system),
      timer_(timer),
      event_base_(nullptr),
      wakeup_event_(nullptr),
      started_(false),
      shutdown_complete_(false) {
  wakeup_pipe_[0] = -1;
  wakeup_pipe_[1] = -1;
  mutex_.reset(thread_system->NewMutex());
}

LibeventDispatcher::~LibeventDispatcher() {
  if (started_.load()) {
    WaitForShutdown();
  }

  if (wakeup_event_ != nullptr) {
    event_del(wakeup_event_);
    event_free(wakeup_event_);
  }

  if (event_base_ != nullptr) {
    event_base_free(event_base_);
  }

  if (wakeup_pipe_[0] >= 0) {
    close(wakeup_pipe_[0]);
  }
  if (wakeup_pipe_[1] >= 0) {
    close(wakeup_pipe_[1]);
  }

  // Cancel any remaining posted functions.
  ScopedMutex lock(mutex_.get());
  while (!posted_functions_.empty()) {
    Function* f = posted_functions_.front();
    posted_functions_.pop_front();
    f->CallCancel();
  }
}

bool LibeventDispatcher::Start() {
  // Initialize libevent thread support (only once globally).
  static std::atomic<bool> pthreads_initialized(false);
  bool expected = false;
  if (pthreads_initialized.compare_exchange_strong(expected, true)) {
    evthread_use_pthreads();
  }

  // Create the event base.
  event_base_ = event_base_new();
  if (event_base_ == nullptr) {
    LOG(ERROR) << "Failed to create libevent event_base";
    return false;
  }

  // Create the wakeup pipe (non-blocking).
  if (pipe(wakeup_pipe_) < 0) {
    LOG(ERROR) << "Failed to create wakeup pipe";
    return false;
  }

  // Make the read end non-blocking.
  int flags = fcntl(wakeup_pipe_[0], F_GETFL);
  if (flags >= 0) {
    fcntl(wakeup_pipe_[0], F_SETFL, flags | O_NONBLOCK);
  }

  // Create the wakeup event.
  wakeup_event_ = event_new(event_base_, wakeup_pipe_[0], EV_READ | EV_PERSIST,
                            &LibeventDispatcher::OnWakeup, this);
  if (wakeup_event_ == nullptr) {
    LOG(ERROR) << "Failed to create wakeup event";
    return false;
  }
  event_add(wakeup_event_, nullptr);

  // Start the background thread.
  thread_ = std::make_unique<DispatcherThread>(thread_system_, this);
  if (!thread_->Start()) {
    LOG(ERROR) << "Failed to start dispatcher thread";
    return false;
  }

  started_.store(true, std::memory_order_release);
  return true;
}

void LibeventDispatcher::Post(Function* function) {
  if (IsShuttingDown()) {
    function->CallCancel();
    return;
  }

  {
    ScopedMutex lock(mutex_.get());
    posted_functions_.push_back(function);
  }

  WakeupEventLoop();
}

EventTimer* LibeventDispatcher::CreateTimer(int64 delay_us,
                                            Function* function) {
  if (IsShuttingDown() || event_base_ == nullptr) {
    function->CallCancel();
    return new BasicEventTimer(nullptr);  // Return a cancelled timer.
  }

  LibeventTimer* timer = new LibeventTimer(event_base_, delay_us, function);
  timer->Enable();
  return timer;
}

EventTimer* LibeventDispatcher::CreateTimerAtUs(int64 wakeup_time_us,
                                                Function* function) {
  int64 now_us = timer_->NowUs();
  int64 delay_us = wakeup_time_us - now_us;
  if (delay_us < 0) {
    delay_us = 0;  // Fire immediately.
  }
  return CreateTimer(delay_us, function);
}

void LibeventDispatcher::InitiateShutdown() {
  EventDispatcher::InitiateShutdown();

  // Break out of the event loop.
  if (event_base_ != nullptr) {
    event_base_loopbreak(event_base_);
  }

  // Wake up to process the shutdown.
  WakeupEventLoop();
}

void LibeventDispatcher::WaitForShutdown() {
  if (!started_.load(std::memory_order_acquire)) {
    return;
  }

  // Already completed shutdown - don't try to join again.
  if (shutdown_complete_.load(std::memory_order_acquire)) {
    return;
  }

  if (!IsShuttingDown()) {
    InitiateShutdown();
  }

  if (thread_ != nullptr) {
    thread_->Join();
  }

  shutdown_complete_.store(true, std::memory_order_release);
}

void LibeventDispatcher::WakeupEventLoop() {
  // Write a byte to the wakeup pipe.
  if (wakeup_pipe_[1] >= 0) {
    char c = 1;
    // Ignore write errors - the event loop will wake up eventually.
    (void)write(wakeup_pipe_[1], &c, 1);
  }
}

// static
void LibeventDispatcher::OnWakeup(evutil_socket_t fd, short /*what*/,
                                  void* arg) {
  LibeventDispatcher* dispatcher = static_cast<LibeventDispatcher*>(arg);

  // Drain the pipe.
  char buf[256];
  while (read(fd, buf, sizeof(buf)) > 0) {
  }

  // Check for shutdown first - call loopbreak from within the event loop
  // to ensure it takes effect.
  if (dispatcher->IsShuttingDown()) {
    event_base_loopbreak(dispatcher->event_base_);
    return;
  }

  // Process posted functions.
  dispatcher->ProcessPostedFunctions();
}

void LibeventDispatcher::ProcessPostedFunctions() {
  // Extract all pending functions while holding the lock.
  std::deque<Function*> functions;
  {
    ScopedMutex lock(mutex_.get());
    functions.swap(posted_functions_);
  }

  // Run them outside the lock.
  while (!functions.empty()) {
    Function* f = functions.front();
    functions.pop_front();

    if (IsShuttingDown()) {
      f->CallCancel();
    } else {
      f->CallRun();
    }
  }
}

}  // namespace net_instaweb
