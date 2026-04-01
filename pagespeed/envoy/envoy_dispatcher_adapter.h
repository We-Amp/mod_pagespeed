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

// EnvoyDispatcherAdapter wraps Envoy's native Event::Dispatcher to implement
// PageSpeed's EventDispatcher interface. This allows PageSpeed to use Envoy's
// event loop directly without needing a separate SchedulerThread.
//
// Key benefits:
// - Eliminates the redundant event loop (PageSpeed was running two loops)
// - Proper integration with Envoy's threading model
// - Timers and callbacks run on Envoy's worker thread
// - Simplified shutdown handling

#ifndef PAGESPEED_ENVOY_ENVOY_DISPATCHER_ADAPTER_H_
#define PAGESPEED_ENVOY_ENVOY_DISPATCHER_ADAPTER_H_

#include "external/envoy/envoy/event/dispatcher.h"
#include "external/envoy/envoy/event/timer.h"
#include "pagespeed/kernel/base/basictypes.h"
#include "pagespeed/kernel/thread/event_dispatcher.h"

namespace net_instaweb {

class Function;
class Timer;

// Timer implementation that wraps Envoy's TimerPtr.
class EnvoyEventTimer : public EventTimer {
 public:
  EnvoyEventTimer(Envoy::Event::TimerPtr timer, Function* function);
  ~EnvoyEventTimer() override;

  void Cancel() override;

  // Enables the timer with the specified delay.
  void Enable(int64 delay_us);

 private:
  friend class EnvoyDispatcherAdapter;
  Envoy::Event::TimerPtr envoy_timer_;
  Function* function_;

  EnvoyEventTimer(const EnvoyEventTimer&) = delete;
  EnvoyEventTimer& operator=(const EnvoyEventTimer&) = delete;
};

// Adapter that wraps Envoy's Event::Dispatcher to implement EventDispatcher.
// This is a thin wrapper that delegates to Envoy's dispatcher.
class EnvoyDispatcherAdapter : public EventDispatcher {
 public:
  // Creates an adapter wrapping the given Envoy dispatcher.
  // The envoy_dispatcher and timer must outlive this adapter.
  EnvoyDispatcherAdapter(Envoy::Event::Dispatcher* envoy_dispatcher,
                         Timer* timer);
  ~EnvoyDispatcherAdapter() override;

  // EventDispatcher interface
  void Post(Function* function) override;
  EventTimer* CreateTimer(int64 delay_us, Function* function) override;
  EventTimer* CreateTimerAtUs(int64 wakeup_time_us,
                              Function* function) override;
  Timer* timer() const override { return timer_; }
  void InitiateShutdown() override;
  void WaitForShutdown() override;

  // Access to the underlying Envoy dispatcher for direct use if needed.
  Envoy::Event::Dispatcher* envoy_dispatcher() { return envoy_dispatcher_; }

 private:
  Envoy::Event::Dispatcher* envoy_dispatcher_;
  Timer* timer_;

  EnvoyDispatcherAdapter(const EnvoyDispatcherAdapter&) = delete;
  EnvoyDispatcherAdapter& operator=(const EnvoyDispatcherAdapter&) = delete;
};

}  // namespace net_instaweb

#endif  // PAGESPEED_ENVOY_ENVOY_DISPATCHER_ADAPTER_H_
