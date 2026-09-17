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

#pragma once

#include <memory>
#include <set>

#include "pagespeed/envoy/envoy_rewrite_driver_factory.h"
#include "pagespeed/kernel/base/md5_hasher.h"
#include "pagespeed/system/circuit_breaker.h"
#include "pagespeed/system/system_rewrite_driver_factory.h"

namespace Envoy {
namespace Event {
class Dispatcher;
}  // namespace Event
}  // namespace Envoy

namespace net_instaweb {

class EnvoyDispatcherAdapter;
class EnvoyMessageHandler;
class EnvoyRewriteOptions;
class EnvoyServerContext;
class EventScheduler;
class SharedCircularBuffer;
class SharedMemRefererStatistics;
class Statistics;
class SystemThreadSystem;

class EnvoyRewriteDriverFactory : public SystemRewriteDriverFactory {
 public:
  // We take ownership of the thread system.
  explicit EnvoyRewriteDriverFactory(const ProcessContext& process_context,
                                     SystemThreadSystem* system_thread_system,
                                     StringPiece hostname, int port);
  ~EnvoyRewriteDriverFactory() override;
  Hasher* NewHasher() override;
  UrlAsyncFetcher* AllocateFetcher(SystemRewriteOptions* config) override;
  MessageHandler* DefaultHtmlParseMessageHandler() override;
  MessageHandler* DefaultMessageHandler() override;
  FileSystem* DefaultFileSystem() override;
  Timer* DefaultTimer() override;
  NamedLockManager* DefaultLockManager() override;
  // Create a new RewriteOptions.  In this implementation it will be an
  // EnvoyRewriteOptions, and it will have CoreFilters explicitly set.
  RewriteOptions* NewRewriteOptions() override;
  RewriteOptions* NewRewriteOptionsForQuery() override;
  ServerContext* NewDecodingServerContext() override;

  // Returns an EventScheduler so that, once StartThreads() attaches the
  // Envoy dispatcher, alarms are driven by the event loop rather than
  // firing only opportunistically. Called lazily (possibly before
  // SetEnvoyDispatcher()), hence the attach-later pattern.
  Scheduler* CreateScheduler() override;

  // Initializes all the statistics objects created transitively by
  // EnvoyRewriteDriverFactory, including envoy-specific and
  // platform-independent statistics.
  static void InitStats(Statistics* statistics);
  EnvoyServerContext* MakeEnvoyServerContext(StringPiece hostname, int port);
  ServerContext* NewServerContext() override;
  void ShutDown() override;

  // Starts pagespeed threads if they've not been started already.
  // IMPORTANT: This must be called explicitly after construction.
  // Call SetEnvoyDispatcher() before this to use Envoy's native event loop.
  // If an Envoy dispatcher has been set via SetEnvoyDispatcher(), uses
  // EventScheduler with that dispatcher. Otherwise, falls back to
  // the traditional SchedulerThread approach.
  void StartThreads();

  // Sets the Envoy dispatcher to use for scheduling. Must be called before
  // StartThreads(). When set, PageSpeed will use Envoy's native event loop
  // for timer operations instead of running a separate SchedulerThread.
  void SetEnvoyDispatcher(Envoy::Event::Dispatcher* dispatcher);

  EnvoyMessageHandler* envoy_message_handler() {
    return envoy_message_handler_;
  }

  // Returns the start time in milliseconds since epoch.
  int64 start_time_ms() const { return start_time_ms_; }

  void NonStaticInitStats(Statistics* statistics) override {
    InitStats(statistics);
  }

  void SetMainConf(EnvoyRewriteOptions* main_conf);

  // Configure circuit breaker for resource fetching.
  // Must be called before StartThreads().
  void SetCircuitBreakerConfig(bool enabled, int failure_threshold,
                               int success_threshold, int64 timeout_ms);

  // Returns the current circuit breaker (may be nullptr if disabled).
  CircuitBreaker* circuit_breaker() { return circuit_breaker_.get(); }

  void LoggingInit(bool may_install_crash_handler);

  void SetServerContextMessageHandler(ServerContext* server_context);

  void ShutDownMessageHandlers() override;

  void SetCircularBuffer(SharedCircularBuffer* buffer) override;

 private:
  // Timer *timer_;

  bool threads_started_;
  EnvoyMessageHandler* envoy_message_handler_;
  EnvoyMessageHandler* envoy_html_parse_message_handler_;
  using EnvoyMessageHandlerSet = std::set<EnvoyMessageHandler*>;
  EnvoyMessageHandlerSet server_context_message_handlers_;
  SharedCircularBuffer* envoy_shared_circular_buffer_;
  GoogleString hostname_;
  int port_;
  bool shut_down_;

  // Envoy dispatcher integration. When envoy_dispatcher_ is set,
  // StartThreads() attaches the EnvoyDispatcherAdapter to the EventScheduler
  // (created by CreateScheduler and owned by the base factory) so the Envoy
  // event loop drives scheduler alarms instead of a SchedulerThread.
  Envoy::Event::Dispatcher* envoy_dispatcher_;
  std::unique_ptr<EnvoyDispatcherAdapter> event_dispatcher_;
  EventScheduler* event_scheduler_;  // Owned by RewriteDriverFactory.

  // Time when the factory was created, for uptime calculation.
  int64 start_time_ms_;

  // Circuit breaker for resource fetching reliability.
  std::unique_ptr<CircuitBreaker> circuit_breaker_;

  EnvoyRewriteDriverFactory(const EnvoyRewriteDriverFactory&) = delete;
  EnvoyRewriteDriverFactory& operator=(const EnvoyRewriteDriverFactory&) =
      delete;
};

}  // namespace net_instaweb
