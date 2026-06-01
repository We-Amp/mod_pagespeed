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

#include "pagespeed/envoy/envoy_rewrite_driver_factory.h"

#include <cstdio>

#include "net/instaweb/http/public/rate_controller.h"
#include "net/instaweb/http/public/rate_controlling_url_async_fetcher.h"
#include "net/instaweb/http/public/wget_url_fetcher.h"
#include "net/instaweb/rewriter/public/rewrite_driver.h"
#include "net/instaweb/rewriter/public/rewrite_driver_factory.h"
#include "net/instaweb/rewriter/public/server_context.h"
#include "net/instaweb/util/public/property_cache.h"
#include "pagespeed/envoy/envoy_dispatcher_adapter.h"
#include "pagespeed/envoy/envoy_message_handler.h"
#include "pagespeed/envoy/envoy_rewrite_options.h"
#include "pagespeed/envoy/envoy_server_context.h"
#include "pagespeed/envoy/log_message_handler.h"
#include "pagespeed/kernel/base/google_message_handler.h"
#include "pagespeed/kernel/base/null_shared_mem.h"
#include "pagespeed/kernel/base/stdio_file_system.h"
#include "pagespeed/kernel/base/string.h"
#include "pagespeed/kernel/base/string_util.h"
#include "pagespeed/kernel/base/thread_system.h"
#include "pagespeed/kernel/http/content_type.h"
#include "pagespeed/kernel/sharedmem/shared_circular_buffer.h"
#include "pagespeed/kernel/sharedmem/shared_mem_statistics.h"
#include "pagespeed/kernel/thread/event_scheduler.h"
#include "pagespeed/kernel/thread/pthread_shared_mem.h"
#include "pagespeed/kernel/thread/scheduler_thread.h"
#include "pagespeed/kernel/util/platform.h"
#include "pagespeed/kernel/util/threadsafe_lock_manager.h"
#include "pagespeed/system/circuit_breaker_fetcher.h"
#include "pagespeed/system/curl_url_async_fetcher.h"
#include "pagespeed/system/in_place_resource_recorder.h"
#include "pagespeed/system/system_caches.h"
#include "pagespeed/system/system_rewrite_options.h"

namespace net_instaweb {

class FileSystem;
class Hasher;
class MessageHandler;
class Statistics;
class Timer;
class UrlAsyncFetcher;
class UrlFetcher;
class Writer;

class SharedCircularBuffer;

EnvoyRewriteDriverFactory::EnvoyRewriteDriverFactory(
    const ProcessContext& process_context,
    SystemThreadSystem* system_thread_system, StringPiece hostname, int port)
    : SystemRewriteDriverFactory(
          process_context, system_thread_system,
          new PthreadSharedMem() /* default shared memory runtime */, hostname,
          port),
      threads_started_(false),
      envoy_message_handler_(
          new EnvoyMessageHandler(timer(), thread_system()->NewMutex())),
      envoy_html_parse_message_handler_(
          new EnvoyMessageHandler(timer(), thread_system()->NewMutex())),
      envoy_shared_circular_buffer_(nullptr),
      hostname_(hostname.as_string()),
      port_(port),
      shut_down_(false),
      envoy_dispatcher_(nullptr),
      start_time_ms_(0) {
  // Record start time before any other initialization.
  start_time_ms_ = timer()->NowMs();
  InitializeDefaultOptions();
  default_options()->set_beacon_url("/envoy_pagespeed_beacon");
  default_options()->set_enabled(RewriteOptions::kEnabledOn);
  default_options()->SetRewriteLevel(RewriteOptions::kCoreFilters);

  SystemRewriteOptions* system_options =
      dynamic_cast<SystemRewriteOptions*>(default_options());
  system_options->set_log_dir("/tmp/envoy_pagespeed_log/");
  system_options->set_statistics_logging_enabled(true);
  // ExternalClusterSpec spec = {{ExternalServerSpec("127.0.0.1", 11211)}};
  // system_options->set_memcached_servers(spec);

  system_options->set_file_cache_clean_size_kb(1024 * 10000);  // 10 GB
  system_options->set_avoid_renaming_introspective_javascript(true);
  system_options->set_file_cache_path("/tmp/envoy_pagespeed_cache/");
  system_options->set_lru_cache_byte_limit(163840);
  system_options->set_lru_cache_kb_per_process(1024 * 500);  // 500 MB

  system_options->set_flush_html(true);

  // EnvoyRewriteOptions *options = (EnvoyRewriteOptions *)system_options;
  // std::vector<std::string> args;
  // args.push_back("RateLimitBackgroundFetches");
  // args.push_back("on");
  // global_settings settings;
  // const char *msg = options->ParseAndSetOptions(args, envoy_message_handler_,
  // settings); CHECK(!msg);

  set_message_buffer_size(1024 * 128);
  set_message_handler(envoy_message_handler_);
  set_html_parse_message_handler(envoy_html_parse_message_handler_);
  // Note: StartThreads() is NOT called here. It must be called explicitly
  // after construction, optionally after SetEnvoyDispatcher() is called.
  // This allows using Envoy's native scheduler when a dispatcher is available.
}

EnvoyRewriteDriverFactory::~EnvoyRewriteDriverFactory() {
  ShutDown();
  envoy_shared_circular_buffer_ = nullptr;
  // message handlers are owned by RewriteDriverFactory
  envoy_message_handler_ = nullptr;
  envoy_html_parse_message_handler_ = nullptr;
  STLDeleteElements(&uninitialized_server_contexts_);
}

Hasher* EnvoyRewriteDriverFactory::NewHasher() { return new MD5Hasher; }

UrlAsyncFetcher* EnvoyRewriteDriverFactory::AllocateFetcher(
    SystemRewriteOptions* config) {
  // Use curl-based fetcher for all resource fetching.
  // Curl operates independently of Envoy's ClusterManager, avoiding
  // TLS initialization issues that previously blocked HTML rewriting.
  CurlUrlAsyncFetcher* curl_fetcher = new CurlUrlAsyncFetcher(
      config->fetcher_proxy().c_str(), thread_system(), statistics(), timer(),
      config->blocking_fetch_timeout_ms(), message_handler());
  curl_fetcher->set_track_original_content_length(
      track_original_content_length());
  curl_fetcher->set_fetch_with_gzip(config->fetch_with_gzip());
  curl_fetcher->SetHttpsOptions(config->https_options());
  curl_fetcher->SetSslCertificatesDir(config->ssl_cert_directory());
  curl_fetcher->SetSslCertificatesFile(config->ssl_cert_file());
  LOG(INFO) << "Using curl-based fetcher for resource fetching";

  // Wrap with circuit breaker if enabled.
  if (circuit_breaker_ != nullptr) {
    LOG(INFO) << "Wrapping fetcher with circuit breaker";
    return new CircuitBreakerFetcher(curl_fetcher, circuit_breaker_.get(),
                                     message_handler());
  }

  return curl_fetcher;
}

MessageHandler* EnvoyRewriteDriverFactory::DefaultHtmlParseMessageHandler() {
  return envoy_html_parse_message_handler_;
}

MessageHandler* EnvoyRewriteDriverFactory::DefaultMessageHandler() {
  return envoy_message_handler_;
}

FileSystem* EnvoyRewriteDriverFactory::DefaultFileSystem() {
  return new StdioFileSystem();
}

Timer* EnvoyRewriteDriverFactory::DefaultTimer() {
  return Platform::CreateTimer();
}

NamedLockManager* EnvoyRewriteDriverFactory::DefaultLockManager() {
  // Envoy is single-process, multi-threaded. ThreadSafeLockManager provides
  // in-process thread coordination which is appropriate for this deployment.
  return new ThreadSafeLockManager(scheduler());
}

RewriteOptions* EnvoyRewriteDriverFactory::NewRewriteOptions() {
  EnvoyRewriteOptions* options = new EnvoyRewriteOptions(thread_system());
  // TODO(jefftk): figure out why using SetDefaultRewriteLevel like
  // mod_pagespeed does in mod_instaweb.cc:create_dir_config() isn't enough here
  // -- if you use that instead then envoy_pagespeed doesn't actually end up
  // defaulting CoreFilters.
  // See: https://github.com/apache/incubator-pagespeed-envoy/issues/1190
  options->SetRewriteLevel(RewriteOptions::kCoreFilters);
  return options;
}

RewriteOptions* EnvoyRewriteDriverFactory::NewRewriteOptionsForQuery() {
  return new EnvoyRewriteOptions(thread_system());
}

EnvoyServerContext* EnvoyRewriteDriverFactory::MakeEnvoyServerContext(
    StringPiece hostname, int port) {
  EnvoyServerContext* server_context =
      new EnvoyServerContext(this, hostname, port);
  uninitialized_server_contexts_.insert(server_context);
  return server_context;
}

ServerContext* EnvoyRewriteDriverFactory::NewDecodingServerContext() {
  ServerContext* sc = new EnvoyServerContext(this, hostname_, port_);
  InitStubDecodingServerContext(sc);
  return sc;
}

ServerContext* EnvoyRewriteDriverFactory::NewServerContext() {
  LOG(DFATAL) << "MakeEnvoyServerContext should be used instead";
  return nullptr;
}

void EnvoyRewriteDriverFactory::ShutDown() {
  if (!shut_down_) {
    shut_down_ = true;

    // Shut down the event dispatcher if we're using Envoy-native scheduling.
    if (event_dispatcher_ != nullptr) {
      event_dispatcher_->InitiateShutdown();
    }

    SystemRewriteDriverFactory::ShutDown();

    // Clean up event scheduler and dispatcher after base class shutdown.
    event_scheduler_.reset();
    event_dispatcher_.reset();
  }
}

void EnvoyRewriteDriverFactory::ShutDownMessageHandlers() {
  envoy_message_handler_->set_buffer(nullptr);
  envoy_html_parse_message_handler_->set_buffer(nullptr);
  for (EnvoyMessageHandlerSet::iterator p =
           server_context_message_handlers_.begin();
       p != server_context_message_handlers_.end(); ++p) {
    (*p)->set_buffer(nullptr);
  }
  server_context_message_handlers_.clear();
}

void EnvoyRewriteDriverFactory::SetEnvoyDispatcher(
    Envoy::Event::Dispatcher* dispatcher) {
  CHECK(!threads_started_)
      << "SetEnvoyDispatcher must be called before StartThreads";
  envoy_dispatcher_ = dispatcher;
}

void EnvoyRewriteDriverFactory::StartThreads() {
  if (threads_started_) {
    return;
  }

  if (envoy_dispatcher_ != nullptr) {
    // Use Envoy's native dispatcher for scheduling.
    // This eliminates the need for a separate SchedulerThread.
    event_dispatcher_ =
        std::make_unique<EnvoyDispatcherAdapter>(envoy_dispatcher_, timer());
    event_scheduler_ = std::make_unique<EventScheduler>(
        thread_system(), event_dispatcher_.get());
    // Note: EventScheduler uses dispatcher timers for wakeups, but the
    // scheduler's ProcessAlarmsOrWaitUs() is still driven by calls from
    // PageSpeed code. The dispatcher provides the timer backend.
    LOG(INFO) << "Using Envoy-native scheduling via EventScheduler";
  } else {
    // Fallback: Use traditional SchedulerThread approach.
    // This is used when no Envoy dispatcher is available (e.g., unit tests).
    SchedulerThread* thread = new SchedulerThread(thread_system(), scheduler());
    bool ok = thread->Start();
    CHECK(ok) << "Unable to start scheduler thread";
    defer_cleanup(thread->MakeDeleter());
    LOG(INFO) << "Using traditional SchedulerThread for scheduling";
  }

  threads_started_ = true;
}

void EnvoyRewriteDriverFactory::SetMainConf(EnvoyRewriteOptions* main_options) {
  // Propagate process-scope options from the copy we had during Envoy option
  // parsing to our own.
  if (main_options != nullptr) {
    default_options()->MergeOnlyProcessScopeOptions(*main_options);
  }
}

void EnvoyRewriteDriverFactory::SetCircuitBreakerConfig(bool enabled,
                                                        int failure_threshold,
                                                        int success_threshold,
                                                        int64 timeout_ms) {
  if (!enabled) {
    circuit_breaker_.reset();
    LOG(INFO) << "Circuit breaker disabled";
    return;
  }

  CircuitBreaker::Config config;
  config.failure_threshold = failure_threshold > 0 ? failure_threshold : 5;
  config.success_threshold = success_threshold > 0 ? success_threshold : 2;
  config.timeout_ms = timeout_ms > 0 ? timeout_ms : 30000;

  circuit_breaker_ = std::make_unique<CircuitBreaker>(config, timer());
  LOG(INFO) << "Circuit breaker enabled: failure_threshold="
            << config.failure_threshold
            << ", success_threshold=" << config.success_threshold
            << ", timeout_ms=" << config.timeout_ms;
}

void EnvoyRewriteDriverFactory::LoggingInit(bool may_install_crash_handler) {
  /*
  log_ = log;
  net_instaweb::log_message_handler::Install(log);
  if (may_install_crash_handler && install_crash_handler())
  {
      EnvoyMessageHandler::InstallCrashHandler(log);
  }
  envoy_message_handler_->set_log(log);
  envoy_html_parse_message_handler_->set_log(log);
  */
}

void EnvoyRewriteDriverFactory::SetServerContextMessageHandler(
    ServerContext* server_context) {
  EnvoyMessageHandler* handler =
      new EnvoyMessageHandler(timer(), thread_system()->NewMutex());
  handler->set_buffer(envoy_shared_circular_buffer_);
  server_context_message_handlers_.insert(handler);
  defer_cleanup(new Deleter<EnvoyMessageHandler>(handler));
  server_context->set_message_handler(handler);
}

void EnvoyRewriteDriverFactory::SetCircularBuffer(
    SharedCircularBuffer* buffer) {
  envoy_shared_circular_buffer_ = buffer;
  envoy_message_handler_->set_buffer(buffer);
  envoy_html_parse_message_handler_->set_buffer(buffer);
}

void EnvoyRewriteDriverFactory::InitStats(Statistics* statistics) {
  // Init standard PSOL stats.
  SystemRewriteDriverFactory::InitStats(statistics);
  RewriteDriverFactory::InitStats(statistics);
  RateController::InitStats(statistics);

  // Init Envoy-specific stats.
  EnvoyServerContext::InitStats(statistics);
  InPlaceResourceRecorder::InitStats(statistics);
  CurlUrlAsyncFetcher::InitStats(statistics);
}

void EnvoyRewriteDriverFactory::PrepareForkedProcess(const char* name) {
  // envoy_pid = envoy_getpid(); // Needed for logging to have the right PIDs.
  SystemRewriteDriverFactory::PrepareForkedProcess(name);
}

void EnvoyRewriteDriverFactory::NameProcess(const char* name) {
  SystemRewriteDriverFactory::NameProcess(name);
  // char name_for_setproctitle[32];
  // snprintf(name_for_setproctitle, sizeof(name_for_setproctitle),
  //         "pagespeed %s", name);
  // envoy_setproctitle(name_for_setproctitle);
}

}  // namespace net_instaweb