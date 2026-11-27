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

// Manage pagespeed state across requests.  Compare to ApacheResourceManager.

#ifndef NGX_SERVER_CONTEXT_H_
#define NGX_SERVER_CONTEXT_H_

#include <atomic>
#include <memory>
#include <mutex>
#include <vector>

#include "ngx_message_handler.h"
#include "pagespeed/system/daemon_adapter.h"
#include "pagespeed/system/daemon_html_engine.h"
#include "pagespeed/system/daemon_serve_arm.h"
#include "pagespeed/system/system_server_context.h"

extern "C" {
#include <ngx_http.h>
}

namespace net_instaweb {

class NgxRewriteDriverFactory;
class NgxRewriteOptions;
class SystemRequestContext;

class NgxServerContext : public SystemServerContext {
 public:
  NgxServerContext(NgxRewriteDriverFactory* factory, StringPiece hostname,
                   int port);
  ~NgxServerContext() override;

  // We don't allow ProxyFetch to fetch HTML via MapProxyDomain. We will call
  // set_trusted_input() on any ProxyFetches we use to transform internal HTML.
  bool ProxiesHtml() const override { return false; }

  // Creates the UDS-backed DaemonReader for the /v1/daemon/* admin
  // endpoints, from this server block's DaemonApiSocketPath, or
  // nullptr when that path is empty (daemon API disabled).  See
  // SystemServerContext::NewDaemonReader().
  DaemonReader* NewDaemonReader() override;

  // Builds this server's optimizer-daemon adapter from its configuration and
  // resolves its health, exactly once, at module-init time.  Returns false
  // when the server must not start; the reason has already been logged.
  //
  // Must run after configuration is complete and before any worker is
  // forked: the serving path only ever reads the verdict, so that it
  // neither probes nor logs per request.
  bool RunDaemonStartupCheck();

  // This server's daemon health.  The start-up verdict, which may flip to
  // kReady after a recovery recheck posted off the event thread: the probe
  // blocks, so nginx -- unlike the Apache and IIS accessors -- never runs
  // it inline, and the flipped verdict lands here through the adapter's
  // published health once it does.  kNotConfigured until
  // RunDaemonStartupCheck() has run, which is also the correct answer for
  // every path that reaches serving without a daemon configured.
  DaemonHealth daemon_health() const {
    return daemon_adapter_ == nullptr ? DaemonHealth::kNotConfigured
                                      : daemon_adapter_->health();
  }

  // This server's daemon adapter, or nullptr before the startup check has
  // run.  Borrowed; the record arm asks it for the per-process cache handle.
  // SHARED by every server block of this configuration with the same
  // socket and volume: CloseRecordCache() on it closes the handle for all
  // of them, so only the worker-exit teardown, which closes every server's,
  // calls it.
  DaemonAdapter* daemon_adapter() { return daemon_adapter_.get(); }

  // The HTML cache engine of this server (DaemonHtmlCache).  A serving
  // worker builds it at its start (InitDaemonHtmlEngine); anywhere else it
  // is built on first use and only reports.  nullptr with the option off.
  DaemonHtmlEngine* daemon_html_engine();

  // Builds this server's engine in a serving worker, before any request:
  // `host_names` are the names this server block lists, `epoch_sites` every
  // site configured on this server's cache path, `runner` the record arm's
  // runner for the admin path's and the ticks' volume work, `budget` the
  // process's byte budget.  Registers the engine as a purge observer of the
  // cache path.  All pointers must outlive ShutDownDaemonHtmlEngine.
  void InitDaemonHtmlEngine(const ConfiguredHostNames& host_names,
                            std::vector<GoogleString> epoch_sites,
                            DaemonHtmlBlockingRunner* runner,
                            DaemonHtmlByteBudget* budget);

  // Unregisters and destroys the engine.  The caller has joined every
  // thread that runs the engine's work first (the record pool), so nothing
  // is in flight.  Idempotent.
  void ShutDownDaemonHtmlEngine();

  // The engine's ticks, called on the request path next to
  // FlushCacheIfNecessary: at most one purge-poll tick per poll interval and
  // one statistics tick per kDaemonHtmlStatisticsTickMs, each posted to
  // `runner` (a tick reads files), never run on the calling thread.
  void MaybeTickDaemonHtmlEngine(DaemonHtmlBlockingRunner* runner);
  static constexpr int64 kDaemonHtmlStatisticsTickMs = 10 * 1000;

  // The peer's serve-stats mmap for this server, or nullptr when no daemon
  // is configured.  Borrowed; PROCESS-lifetime, and deliberately not opened
  // here -- the object is built at module-init time and opens the file on
  // its first recorded serve, in whichever worker process makes it.  Opening
  // in the master would hand every worker an inherited mapping it never
  // asked for, which is the same fork problem the record cache solves the
  // same way.
  DaemonServeStats* daemon_serve_stats() { return daemon_serve_stats_.get(); }

  // Call only when you need an NgxRewriteOptions.  If you don't need
  // nginx-specific behavior, call global_options() instead which doesn't
  // downcast.
  NgxRewriteOptions* config();

  NgxRewriteDriverFactory* ngx_rewrite_driver_factory() { return ngx_factory_; }
  SystemRequestContext* NewRequestContext(ngx_http_request_t* r);

  NgxMessageHandler* ngx_message_handler() {
    return dynamic_cast<NgxMessageHandler*>(message_handler());
  }

  GoogleString FormatOption(StringPiece option_name, StringPiece args) override;

  void set_ngx_http2_variable_index(ngx_int_t idx) {
    ngx_http2_variable_index_ = idx;
  }

  ngx_int_t ngx_http2_variable_index() const {
    return ngx_http2_variable_index_;
  }

 private:
  NgxRewriteDriverFactory* ngx_factory_;
  // what index the "http2" var is, or NGX_ERROR.
  ngx_int_t ngx_http2_variable_index_;

  // In this order: the stats object borrows the adapter's bound library, so
  // it must be destroyed first.  Shared with the other server blocks of this
  // configuration that name the same socket and volume (see
  // NgxRewriteDriverFactory::daemon_adapter_pool()).
  std::shared_ptr<DaemonAdapter> daemon_adapter_;
  std::once_flag daemon_html_engine_once_;
  // What the serving engine reads purges from; declared before the engine,
  // which borrows it.
  std::unique_ptr<DaemonHtmlPurgeSource> daemon_html_purge_source_;
  std::unique_ptr<DaemonHtmlEngine> daemon_html_engine_;
  bool daemon_html_observing_ = false;
  std::atomic<int64> daemon_html_last_poll_tick_ms_{0};
  std::atomic<int64> daemon_html_last_stats_tick_ms_{0};
  std::unique_ptr<DaemonServeStats> daemon_serve_stats_;

  NgxServerContext(const NgxServerContext&) = delete;
  NgxServerContext& operator=(const NgxServerContext&) = delete;
};

}  // namespace net_instaweb

#endif  // NGX_SERVER_CONTEXT_H_
