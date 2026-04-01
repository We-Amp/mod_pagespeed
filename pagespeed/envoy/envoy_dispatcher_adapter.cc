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

#include "pagespeed/envoy/envoy_dispatcher_adapter.h"

#include <chrono>

#include "pagespeed/kernel/base/function.h"
#include "pagespeed/kernel/base/timer.h"

namespace net_instaweb {

// EnvoyEventTimer implementation

EnvoyEventTimer::EnvoyEventTimer(Envoy::Event::TimerPtr timer,
                                 Function* function)
    : envoy_timer_(std::move(timer)), function_(function) {}

EnvoyEventTimer::~EnvoyEventTimer() {
  if (envoy_timer_ != nullptr) {
    envoy_timer_->disableTimer();
  }
  if (function_ != nullptr) {
    function_->CallCancel();
  }
}

void EnvoyEventTimer::Cancel() {
  bool was_cancelled = cancelled_.exchange(true, std::memory_order_acq_rel);
  if (!was_cancelled) {
    if (envoy_timer_ != nullptr) {
      envoy_timer_->disableTimer();
    }
    if (function_ != nullptr) {
      function_->CallCancel();
      function_ = nullptr;
    }
  }
}

void EnvoyEventTimer::Enable(int64 delay_us) {
  if (envoy_timer_ != nullptr) {
    // Envoy's timer API uses milliseconds.
    int64 delay_ms = (delay_us + 999) / 1000;  // Round up
    envoy_timer_->enableTimer(std::chrono::milliseconds(delay_ms));
  }
}

// EnvoyDispatcherAdapter implementation

EnvoyDispatcherAdapter::EnvoyDispatcherAdapter(
    Envoy::Event::Dispatcher* envoy_dispatcher, Timer* timer)
    : envoy_dispatcher_(envoy_dispatcher), timer_(timer) {}

EnvoyDispatcherAdapter::~EnvoyDispatcherAdapter() {
  // Don't own the Envoy dispatcher or timer - they're owned by Envoy.
}

void EnvoyDispatcherAdapter::Post(Function* function) {
  if (IsShuttingDown()) {
    function->CallCancel();
    return;
  }

  // Capture the function pointer and call it on the Envoy dispatcher thread.
  envoy_dispatcher_->post([function]() { function->CallRun(); });
}

EventTimer* EnvoyDispatcherAdapter::CreateTimer(int64 delay_us,
                                                Function* function) {
  if (IsShuttingDown()) {
    function->CallCancel();
    return new BasicEventTimer(nullptr);
  }

  // Create the wrapper timer first, passing the function.
  // The timer takes ownership of the function for cancellation purposes.
  EnvoyEventTimer* timer = new EnvoyEventTimer(nullptr, function);

  // Create an Envoy timer with a callback that runs the function.
  // When the timer fires:
  // - We mark the timer's function as nullptr (so destructor won't cancel it)
  // - We call the function
  Envoy::Event::TimerPtr envoy_timer =
      envoy_dispatcher_->createTimer([timer]() {
        // Timer fired - take the function from the timer.
        Function* f = timer->function_;
        timer->function_ = nullptr;  // Prevent double-call on destruction.
        if (f != nullptr && !timer->IsCancelled()) {
          f->CallRun();
        }
      });

  // Move the envoy timer into our wrapper.
  timer->envoy_timer_ = std::move(envoy_timer);

  // Enable the timer.
  timer->Enable(delay_us);

  return timer;
}

EventTimer* EnvoyDispatcherAdapter::CreateTimerAtUs(int64 wakeup_time_us,
                                                    Function* function) {
  int64 now_us = timer_->NowUs();
  int64 delay_us = wakeup_time_us - now_us;
  if (delay_us < 0) {
    delay_us = 0;  // Fire immediately.
  }
  return CreateTimer(delay_us, function);
}

void EnvoyDispatcherAdapter::InitiateShutdown() {
  EventDispatcher::InitiateShutdown();
  // Note: We don't break out of the Envoy event loop here because
  // we don't own it. Envoy manages its own lifecycle.
}

void EnvoyDispatcherAdapter::WaitForShutdown() {
  // For Envoy, we don't block waiting for shutdown.
  // Envoy's lifecycle is managed by Envoy itself.
  // Our shutdown is complete once InitiateShutdown() is called.
}

}  // namespace net_instaweb
