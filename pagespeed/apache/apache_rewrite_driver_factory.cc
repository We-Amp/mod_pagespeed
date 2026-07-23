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

#include "pagespeed/apache/apache_rewrite_driver_factory.h"

#include <unistd.h>

#include "ap_mpm.h"
#include "apr_pools.h"
#include "base/logging.h"
#include "net/instaweb/rewriter/public/rewrite_driver_factory.h"
#include "net/instaweb/rewriter/public/server_context.h"
#include "pagespeed/apache/apache_config.h"
#include "pagespeed/apache/apache_httpd_includes.h"
#include "pagespeed/apache/apache_message_handler.h"
#include "pagespeed/apache/apache_server_context.h"
#include "pagespeed/apache/apache_thread_system.h"
#include "pagespeed/apache/apr_timer.h"
#include "pagespeed/kernel/base/message_handler.h"
#include "pagespeed/kernel/base/null_shared_mem.h"
#include "pagespeed/kernel/base/stl_util.h"
#include "pagespeed/kernel/base/string.h"
#include "pagespeed/kernel/base/string_util.h"
#include "pagespeed/kernel/base/thread_system.h"
#include "pagespeed/kernel/base/timer.h"
#include "pagespeed/kernel/sharedmem/shared_circular_buffer.h"
#include "pagespeed/kernel/thread/event_scheduler.h"
#include "pagespeed/kernel/thread/libevent_dispatcher.h"
#include "pagespeed/kernel/thread/pthread_shared_mem.h"
#include "pagespeed/system/curl_url_async_fetcher.h"
#include "pagespeed/system/system_rewrite_options.h"

namespace net_instaweb {

class ProcessContext;

ApacheRewriteDriverFactory::ApacheRewriteDriverFactory(
    const ProcessContext& process_context, server_rec* server,
    const StringPiece& version)
    : SystemRewriteDriverFactory(process_context, new ApacheThreadSystem,
                                 nullptr, /* default shared memory runtime */
                                 server->server_hostname, server->port),
      server_rec_(server),
      version_(version.data(), version.size()),
      apache_message_handler_(new ApacheMessageHandler(
          server_rec_, version_, timer(), thread_system()->NewMutex())),
      apache_html_parse_message_handler_(new ApacheMessageHandler(
          server_rec_, version_, timer(), thread_system()->NewMutex())) {
  apr_pool_create(&pool_, nullptr);

  // Apache defaults UsePerVhostStatistics to false for historical reasons, but
  // more recent implementations default it to true.
  set_use_per_vhost_statistics(false);

  // Make sure the ownership of apache_message_handler_ and
  // apache_html_parse_message_handler_ is given to scoped pointer.
  // Otherwise may result in leak error in test.
  message_handler();
  html_parse_message_handler();
  InitializeDefaultOptions();
}

ApacheRewriteDriverFactory::~ApacheRewriteDriverFactory() {
  // We free all the resources before destroying the pool, because some of the
  // resource uses the sub-pool and will need that pool to be around to
  // clean up properly.
  ShutDown();

  // Quiesce the dispatcher's event loop, then detach the scheduler from it
  // BEFORE the dispatcher member is destroyed: the scheduler (owned by the
  // base factory) outlives the dispatcher, and detaching releases its pump
  // timers while the libevent base they reference is still alive.
  if (event_dispatcher_ != nullptr) {
    event_dispatcher_->InitiateShutdown();
    event_dispatcher_->WaitForShutdown();
    if (event_scheduler_ != nullptr) {
      event_scheduler_->DetachDispatcher();
    }
  }

  apr_pool_destroy(pool_);

  // We still have registered a pool deleter here, right?  This seems risky...
  STLDeleteElements(&uninitialized_server_contexts_);
}

Timer* ApacheRewriteDriverFactory::DefaultTimer() { return new AprTimer(); }

MessageHandler* ApacheRewriteDriverFactory::DefaultHtmlParseMessageHandler() {
  return apache_html_parse_message_handler_;
}

MessageHandler* ApacheRewriteDriverFactory::DefaultMessageHandler() {
  return apache_message_handler_;
}

void ApacheRewriteDriverFactory::SetupCaches(ServerContext* server_context) {
  SystemRewriteDriverFactory::SetupCaches(server_context);

  // TODO(jmarantz): It would make more sense to have the base ServerContext
  // own the ProxyFetchFactory, but that would create a cyclic directory
  // dependency.  This can be resolved minimally by moving proxy_fetch.cc
  // from automatic/ to rewriter/.  I think we should also think harder about
  // separating out rewriting infrastructure from rewriters.
  ApacheServerContext* apache_server_context =
      dynamic_cast<ApacheServerContext*>(server_context);
  CHECK(apache_server_context != nullptr);
  apache_server_context->InitProxyFetchFactory();
}

Scheduler* ApacheRewriteDriverFactory::CreateScheduler() {
  // Called lazily via RewriteDriverFactory::scheduler(), which owns the
  // result. Created unattached; SetNeedSchedulerThread() attaches the
  // libevent dispatcher when configuration requires driven alarms.
  DCHECK(event_scheduler_ == nullptr);
  event_scheduler_ = new EventScheduler(thread_system(), timer());
  return event_scheduler_;
}

void ApacheRewriteDriverFactory::SetNeedSchedulerThread() {
  if (event_dispatcher_ == nullptr) {
    // LibeventDispatcher runs libevent in a background thread, providing
    // the same functionality as SchedulerThread but with a unified
    // EventDispatcher abstraction. Attaching it to the EventScheduler makes
    // the event loop drive alarm delivery, so alarms fire on time even with
    // no thread blocked in the scheduler.
    event_dispatcher_ =
        std::make_unique<LibeventDispatcher>(thread_system(), timer());
    bool ok = event_dispatcher_->Start();
    CHECK(ok) << "Unable to start event dispatcher";
    scheduler();  // Ensure the scheduler exists (created via CreateScheduler).
    CHECK(event_scheduler_ != nullptr);
    event_scheduler_->AttachDispatcher(event_dispatcher_.get());
  }
}

bool ApacheRewriteDriverFactory::IsServerThreaded() {
  // Detect whether we're using a threaded MPM.
  apr_status_t status;
  int result = 0, threads = 1;
  status = ap_mpm_query(AP_MPMQ_IS_THREADED, &result);
  if (status == APR_SUCCESS &&
      (result == AP_MPMQ_STATIC || result == AP_MPMQ_DYNAMIC)) {
    // Number of configured threads.
    status = ap_mpm_query(AP_MPMQ_MAX_THREADS, &threads);
    if (status != APR_SUCCESS) {
      return false;  // Assume non-thready by default.
    }
  }

  return threads > 1;
}

int ApacheRewriteDriverFactory::LookupThreadLimit() {
  int thread_limit = 0;
  // The compiled maximum number of threads.
  ap_mpm_query(AP_MPMQ_HARD_LIMIT_THREADS, &thread_limit);
  return thread_limit;
}

void ApacheRewriteDriverFactory::ParentOrChildInit() {
  if (install_crash_handler()) {
    ApacheMessageHandler::InstallCrashHandler(server_rec_);
  }
  SystemRewriteDriverFactory::ParentOrChildInit();
}

void ApacheRewriteDriverFactory::ShutDownMessageHandlers() {
  // Reset SharedCircularBuffer to NULL, so that any shutdown warnings
  // (e.g. in ServerContext::ShutDownDrivers) don't reference
  // deleted objects as the base-class is deleted.
  //
  // TODO(jefftk): merge ApacheMessageHandler and NgxMessageHandler into
  // SystemMessageHandler and then move this into System.
  apache_message_handler_->set_buffer(nullptr);
  apache_html_parse_message_handler_->set_buffer(nullptr);
}

void ApacheRewriteDriverFactory::SetupMessageHandlers() {
  // TODO(jefftk): merge ApacheMessageHandler and NgxMessageHandler into
  // SystemMessageHandler and then move this into System.
  apache_message_handler_->SetPidString(static_cast<int64>(getpid()));
  apache_html_parse_message_handler_->SetPidString(
      static_cast<int64>(getpid()));
}

void ApacheRewriteDriverFactory::SetCircularBuffer(
    SharedCircularBuffer* buffer) {
  // TODO(jefftk): merge ApacheMessageHandler and NgxMessageHandler into
  // SystemMessageHandler and then move this into System.
  apache_message_handler_->set_buffer(buffer);
  apache_html_parse_message_handler_->set_buffer(buffer);
}

void ApacheRewriteDriverFactory::Initialize() {
  ApacheConfig::Initialize();
  RewriteDriverFactory::Initialize();
}

UrlAsyncFetcher* ApacheRewriteDriverFactory::AllocateFetcher(
    SystemRewriteOptions* config) {
  CurlUrlAsyncFetcher* fetcher = new CurlUrlAsyncFetcher(
      config->fetcher_proxy().c_str(), thread_system(), statistics(), timer(),
      config->blocking_fetch_timeout_ms(), message_handler());
  fetcher->set_list_outstanding_urls_on_error(list_outstanding_urls_on_error());
  fetcher->set_fetch_with_gzip(config->fetch_with_gzip());
  fetcher->set_track_original_content_length(track_original_content_length());
  fetcher->SetHttpsOptions(config->https_options());
  fetcher->SetSslCertificatesDir(config->ssl_cert_directory());
  fetcher->SetSslCertificatesFile(config->ssl_cert_file());
  return fetcher;
}

void ApacheRewriteDriverFactory::InitStats(Statistics* statistics) {
  // Init standard system stats.
  SystemRewriteDriverFactory::InitStats(statistics);

  // Init Apache-specific stats.
  CurlUrlAsyncFetcher::InitStats(statistics);
  ApacheServerContext::InitStats(statistics);
}

void ApacheRewriteDriverFactory::Terminate() {
  RewriteDriverFactory::Terminate();
  ApacheConfig::Terminate();
  PthreadSharedMem::Terminate();
}

ApacheServerContext* ApacheRewriteDriverFactory::MakeApacheServerContext(
    server_rec* server) {
  ApacheServerContext* server_context =
      new ApacheServerContext(this, server, version_);
  uninitialized_server_contexts_.insert(server_context);
  return server_context;
}

ServerContext* ApacheRewriteDriverFactory::NewDecodingServerContext() {
  ServerContext* sc = new ApacheServerContext(this, server_rec_, version_);
  InitStubDecodingServerContext(sc);
  return sc;
}

bool ApacheRewriteDriverFactory::PoolDestroyed(
    ApacheServerContext* server_context) {
  if (uninitialized_server_contexts_.erase(server_context) == 1) {
    delete server_context;
  }

  // Returns true if all the ServerContexts known by the factory and its
  // superclass are finished.  Then it's time to destroy the factory.  Note
  // that ApacheRewriteDriverFactory keeps track of ServerContexts that
  // are partially constructed.  RewriteDriverFactory keeps track of
  // ServerContexts that are already serving requests.  We need to clean
  // all of them out before we can terminate the driver.
  bool no_active_server_contexts = TerminateServerContext(server_context);
  return (no_active_server_contexts && uninitialized_server_contexts_.empty());
}

ApacheConfig* ApacheRewriteDriverFactory::NewRewriteOptions() {
  return new ApacheConfig(hostname_identifier(), thread_system());
}

ApacheConfig* ApacheRewriteDriverFactory::NewRewriteOptionsForQuery() {
  return new ApacheConfig("query", thread_system());
}

}  // namespace net_instaweb
