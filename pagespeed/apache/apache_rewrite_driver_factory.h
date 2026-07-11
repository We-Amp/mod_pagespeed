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

#ifndef PAGESPEED_APACHE_APACHE_REWRITE_DRIVER_FACTORY_H_
#define PAGESPEED_APACHE_APACHE_REWRITE_DRIVER_FACTORY_H_

#include <memory>

// Note: We must include apache_config.h to allow using ApacheConfig*
// return-types for functions that return RewriteOptions* in base class.
#include "pagespeed/apache/apache_config.h"
#include "pagespeed/kernel/base/basictypes.h"
#include "pagespeed/kernel/base/statistics.h"
#include "pagespeed/kernel/base/string.h"
#include "pagespeed/kernel/base/string_util.h"
#include "pagespeed/system/system_rewrite_driver_factory.h"

struct apr_pool_t;
struct server_rec;

namespace net_instaweb {

class ApacheMessageHandler;
class ApacheServerContext;
class EventScheduler;
class LibeventDispatcher;
class MessageHandler;
class ProcessContext;
class ServerContext;
class SharedCircularBuffer;
class SystemRewriteOptions;
class Timer;
class UrlAsyncFetcher;

// Creates an Apache RewriteDriver.
class ApacheRewriteDriverFactory : public SystemRewriteDriverFactory {
 public:
  ApacheRewriteDriverFactory(const ProcessContext& process_context,
                             server_rec* server, const StringPiece& version);
  ~ApacheRewriteDriverFactory() override;

  // Give access to apache_message_handler_ for the cases we need
  // to use ApacheMessageHandler rather than MessageHandler.
  // e.g. Use ApacheMessageHandler::Dump()
  // This is a better choice than cast from MessageHandler.
  ApacheMessageHandler* apache_message_handler() {
    return apache_message_handler_;
  }

  void NonStaticInitStats(Statistics* statistics) override {
    InitStats(statistics);
  }

  ApacheServerContext* MakeApacheServerContext(server_rec* server);

  // Notification of apache tearing down a context (vhost or top-level)
  // corresponding to given ApacheServerContext. Returns true if it was
  // the last context.
  bool PoolDestroyed(ApacheServerContext* rm);

  ApacheConfig* NewRewriteOptions() override;

  // As above, but set a name on the ApacheConfig noting that it came from
  // a query.
  ApacheConfig* NewRewriteOptionsForQuery() override;

  // Initializes all the statistics objects created transitively by
  // ApacheRewriteDriverFactory, including apache-specific and
  // platform-independent statistics.
  static void InitStats(Statistics* statistics);
  static void Initialize();
  static void Terminate();

  // Called by any ApacheServerContext whose configuration requires alarms
  // to fire without a waiting thread (proxy_all_requests_mode). Starts the
  // libevent dispatcher and attaches it to the EventScheduler so the event
  // loop drives alarm delivery. Starts a thread, so should only be called
  // from child processes.
  void SetNeedSchedulerThread();

  // Needed by mod_instaweb.cc:ParseDirective().
  void set_message_buffer_size(int x) override {
    SystemRewriteDriverFactory::set_message_buffer_size(x);
  }

  bool IsServerThreaded() override;
  int LookupThreadLimit() override;

 protected:
  UrlAsyncFetcher* AllocateFetcher(SystemRewriteOptions* config) override;

  // Provide defaults.
  MessageHandler* DefaultHtmlParseMessageHandler() override;
  MessageHandler* DefaultMessageHandler() override;
  Timer* DefaultTimer() override;
  void SetupCaches(ServerContext* server_context) override;

  // Disable the Resource Manager's filesystem since we have a
  // write-through http_cache.
  virtual bool ShouldWriteResourcesToFileSystem() { return false; }

  void ParentOrChildInit() override;

  // Returns an EventScheduler so that SetNeedSchedulerThread() can attach
  // the libevent dispatcher to it; until (and unless) that happens it
  // behaves exactly like the base Scheduler.
  Scheduler* CreateScheduler() override;

  void SetupMessageHandlers() override;
  void ShutDownMessageHandlers() override;

  void SetCircularBuffer(SharedCircularBuffer* buffer) override;

  ServerContext* NewDecodingServerContext() override;

 private:
  apr_pool_t* pool_;
  server_rec* server_rec_;
  // Event-based scheduling using LibeventDispatcher.
  // LibeventDispatcher runs its own background event loop thread; once
  // SetNeedSchedulerThread() attaches it, it drives the EventScheduler's
  // alarms (created by CreateScheduler, owned by the base factory).
  std::unique_ptr<LibeventDispatcher> event_dispatcher_;
  EventScheduler* event_scheduler_ = nullptr;  // Owned by RewriteDriverFactory.

  // TODO(jmarantz): These options could be consolidated in a protobuf or
  // some other struct, which would keep them distinct from the rest of the
  // state.  Note also that some of the options are in the base class,
  // RewriteDriverFactory, so we'd have to sort out how that worked.
  GoogleString version_;

  // This will be assigned to message_handler_ when message_handler() or
  // html_parse_message_handler is invoked for the first time.
  // We keep an extra link because we need to refer them as
  // ApacheMessageHandlers rather than just MessageHandler in initialization
  // process.
  ApacheMessageHandler* apache_message_handler_;
  // This will be assigned to html_parse_message_handler_ when
  // html_parse_message_handler() is invoked for the first time.
  // Note that apache_message_handler_ and apache_html_parse_message_handler
  // writes to the same shared memory which is owned by the factory.
  ApacheMessageHandler* apache_html_parse_message_handler_;

  ApacheRewriteDriverFactory(const ApacheRewriteDriverFactory&) = delete;
  ApacheRewriteDriverFactory& operator=(const ApacheRewriteDriverFactory&) =
      delete;
};

}  // namespace net_instaweb

#endif  // PAGESPEED_APACHE_APACHE_REWRITE_DRIVER_FACTORY_H_
