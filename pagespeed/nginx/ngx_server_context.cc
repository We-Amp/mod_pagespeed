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

#include "ngx_server_context.h"

extern "C" {
#include <ngx_http.h>
}

#include <algorithm>
#include <utility>

#include "net/instaweb/rewriter/public/rewrite_driver.h"
#include "ngx_message_handler.h"
#include "ngx_pagespeed.h"
#include "ngx_rewrite_driver_factory.h"
#include "ngx_rewrite_options.h"
#include "pagespeed/system/add_headers_fetcher.h"
#include "pagespeed/system/daemon_html_purge_source.h"
#include "pagespeed/system/loopback_route_fetcher.h"
#include "pagespeed/system/site_cache_fragment.h"
#include "pagespeed/system/system_cache_path.h"
#include "pagespeed/system/system_request_context.h"
#include "pagespeed/system/uds_daemon_reader.h"

namespace net_instaweb {

NgxServerContext::NgxServerContext(NgxRewriteDriverFactory* factory,
                                   StringPiece hostname, int port)
    : SystemServerContext(factory, hostname, port),
      ngx_factory_(factory),
      ngx_http2_variable_index_(NGX_ERROR) {}

NgxServerContext::~NgxServerContext() { ShutDownDaemonHtmlEngine(); }

NgxRewriteOptions* NgxServerContext::config() {
  return NgxRewriteOptions::DynamicCast(global_options());
}

bool NgxServerContext::RunDaemonStartupCheck() {
  const NgxRewriteOptions* config = this->config();
  // The per-server message handler is wired in the worker at ChildInit; in
  // the master there is none yet, and the adapter's startup check announces
  // through the handler it is given. Use the factory's, the same handler the
  // factory itself logs module-init messages through.
  MessageHandler* handler = factory()->message_handler();
  // Shared with every other server block of this configuration that names
  // the same socket and volume: one adapter, so each worker opens ONE
  // handle on the volume (and starts its storage threads once) instead of
  // one per server block.  The check itself runs once per pair; later
  // server blocks reuse its verdict.
  const DaemonAdapterPool::Checked checked =
      ngx_factory_->daemon_adapter_pool()->GetOrCheck(
          config->daemon_socket_path(), config->daemon_volume_path(), handler);
  daemon_adapter_ = checked.adapter;
  const bool ok = checked.status == DaemonStartupStatus::kOk;
  // Built whatever the verdict, and holding nothing open.  The serve arm
  // records exactly one class per response through it, and a null object
  // here would make "the daemon was not usable" and "this serve was not
  // counted" the same absence in the counters.
  if (daemon_adapter_->abi() != nullptr) {
    daemon_serve_stats_ = std::make_unique<DaemonServeStats>(
        daemon_adapter_->abi(), daemon_adapter_->volume_path());
  }
  return ok;
}

DaemonReader* NgxServerContext::NewDaemonReader() {
  const GoogleString& socket_path = config()->daemon_api_socket_path();
  if (socket_path.empty()) {
    return nullptr;  // Daemon API disabled: the console says "not configured".
  }
  return new UdsDaemonReader(thread_system(), timer(), socket_path,
                             message_handler());
}

namespace {

DaemonHtmlMode DaemonHtmlModeFor(RewriteOptions::DaemonHtmlCacheMode mode) {
  switch (mode) {
    case RewriteOptions::kDaemonHtmlCacheOff:
      return DaemonHtmlMode::kOff;
    case RewriteOptions::kDaemonHtmlCacheReportOnly:
      return DaemonHtmlMode::kReportOnly;
    case RewriteOptions::kDaemonHtmlCacheServeVariants:
      return DaemonHtmlMode::kServeVariants;
  }
  return DaemonHtmlMode::kOff;
}

}  // namespace

void NgxServerContext::InitDaemonHtmlEngine(
    const ConfiguredHostNames& host_names,
    std::vector<GoogleString> epoch_sites, DaemonHtmlBlockingRunner* runner,
    DaemonHtmlByteBudget* budget) {
  std::call_once(daemon_html_engine_once_, [&] {
    const RewriteOptions* options = global_options();
    const SystemRewriteOptions* system_options =
        SystemRewriteOptions::DynamicCast(options);
    if (options->daemon_html_cache() == RewriteOptions::kDaemonHtmlCacheOff ||
        system_options == nullptr) {
      return;
    }
    const GoogleString& log_dir = system_options->log_dir();
    if (log_dir.empty()) {
      message_handler()->Message(
          kWarning, "DaemonHtmlCache decision log is off: LogDir is not set");
    }
    daemon_html_purge_source_ = std::make_unique<SystemDaemonHtmlPurgeSource>(
        options, file_system(),
        DaemonHtmlCacheFlushPath(system_options->cache_flush_filename(),
                                 system_options->file_cache_path()));
    DaemonHtmlEngine::Config config;
    config.mode = DaemonHtmlModeFor(options->daemon_html_cache());
    config.max_bytes = options->daemon_html_max_bytes();
    config.agent_optimize = options->agent_optimize();
    config.host_names = host_names;
    config.site = SiteIdFromServerFragment(options->cache_fragment());
    config.epoch_sites = std::move(epoch_sites);
    DaemonHtmlEngine::Deps deps;
    DaemonAdapter* adapter = daemon_adapter_.get();
    if (adapter != nullptr) {
      deps.substrate.abi = adapter->abi();
      // The handle this process holds now: none until the volume is open,
      // and the new one once the worker replaced its volume.  Never opens,
      // never waits (see DaemonAdapter::RecordCacheIfOpen).
      deps.substrate.cache_source = [adapter]() -> void* {
        return adapter->health() == DaemonHealth::kReady
                   ? adapter->RecordCacheIfOpen()
                   : nullptr;
      };
      deps.substrate.cache_path = adapter->volume_path();
      deps.substrate.notify_socket = adapter->socket_path();
    }
    deps.purge_source = daemon_html_purge_source_.get();
    deps.statistics = statistics();
    deps.timer = timer();
    deps.handler = message_handler();
    deps.limiter = ProcessDaemonHealNotifyLimiter();
    deps.budget = budget;
    deps.runner = runner;
    deps.log_dir = log_dir;
    daemon_html_engine_ =
        std::make_unique<DaemonHtmlEngine>(std::move(config), std::move(deps));
    if (cache_path() != nullptr) {
      cache_path()->AddPurgeObserver(daemon_html_engine_.get());
      daemon_html_observing_ = true;
    }
  });
}

void NgxServerContext::ShutDownDaemonHtmlEngine() {
  if (daemon_html_observing_ && cache_path() != nullptr) {
    cache_path()->RemovePurgeObserver(daemon_html_engine_.get());
  }
  daemon_html_observing_ = false;
  daemon_html_engine_.reset();
  daemon_html_purge_source_.reset();
}

void NgxServerContext::MaybeTickDaemonHtmlEngine(
    DaemonHtmlBlockingRunner* runner) {
  DaemonHtmlEngine* engine = daemon_html_engine_.get();
  if (engine == nullptr || runner == nullptr ||
      engine->config().mode < DaemonHtmlMode::kServeVariants) {
    return;
  }
  const int64 now_ms = timer()->NowMs();
  const int64 poll_ms =
      std::max<int64>(
          1, global_system_rewrite_options()->cache_flush_poll_interval_sec()) *
      Timer::kSecondMs;
  int64 last = daemon_html_last_poll_tick_ms_.load();
  if (now_ms - last >= poll_ms &&
      daemon_html_last_poll_tick_ms_.compare_exchange_strong(last, now_ms)) {
    runner->RunBlocking(
        DaemonHtmlCommit([engine] { engine->OnPurgePollTick(); }));
  }
  last = daemon_html_last_stats_tick_ms_.load();
  if (now_ms - last >= kDaemonHtmlStatisticsTickMs &&
      daemon_html_last_stats_tick_ms_.compare_exchange_strong(last, now_ms)) {
    runner->RunBlocking(
        DaemonHtmlCommit([engine] { engine->OnStatisticsTick(); }));
  }
}

DaemonHtmlEngine* NgxServerContext::daemon_html_engine() {
  std::call_once(daemon_html_engine_once_, [this] {
    const SystemRewriteOptions* options =
        SystemRewriteOptions::DynamicCast(global_options());
    daemon_html_engine_ = NewDaemonHtmlEngineForPort(
        global_options()->daemon_html_cache(),
        global_options()->daemon_html_max_bytes(),
        options == nullptr ? GoogleString() : options->log_dir(),
        daemon_adapter_ == nullptr ? nullptr : daemon_adapter_->abi(),
        statistics(), timer(), message_handler());
  });
  return daemon_html_engine_.get();
}

SystemRequestContext* NgxServerContext::NewRequestContext(
    ngx_http_request_t* r) {
  // Based on ngx_http_variable_server_port.
  bool port_set = false;
  int local_port = 0;
#if (NGX_HAVE_INET6)
  if (r->connection->local_sockaddr->sa_family == AF_INET6) {
    local_port = ntohs(
        reinterpret_cast<struct sockaddr_in6*>(r->connection->local_sockaddr)
            ->sin6_port);
    port_set = true;
  }
#endif
  if (!port_set) {
    local_port = ntohs(
        reinterpret_cast<struct sockaddr_in*>(r->connection->local_sockaddr)
            ->sin_port);
  }

  ngx_str_t local_ip;
  u_char addr[NGX_SOCKADDR_STRLEN];
  local_ip.len = NGX_SOCKADDR_STRLEN;
  local_ip.data = addr;
  ngx_int_t rc = ngx_connection_local_sockaddr(r->connection, &local_ip, 0);
  if (rc != NGX_OK) {
    local_ip.len = 0;
  }

  SystemRequestContext* ctx = new SystemRequestContext(
      thread_system()->NewMutex(), timer(), ps_determine_host(r), local_port,
      str_to_string_piece(local_ip));

  // Record the transport scheme of the incoming connection so loopback
  // fetches speak the right protocol to local_port (the X-Forwarded-Proto
  // scheme correction).
  // Same detection as ngx_http_variable_scheme / ps_is_https.
#if (NGX_HTTP_SSL)
  ctx->set_local_scheme(r->connection->ssl != nullptr ? "https" : "http");
#else
  ctx->set_local_scheme("http");
#endif

  // See if http2 is in use.
  if (ngx_http2_variable_index_ >= 0) {
    ngx_http_variable_value_t* val =
        ngx_http_get_indexed_variable(r, ngx_http2_variable_index_);
    if (val != nullptr && val->valid) {
      StringPiece str_val(reinterpret_cast<char*>(val->data), val->len);
      if (str_val == "h2" || str_val == "h2c") {
        ctx->set_using_http2(true);
      }
    }
  }

  return ctx;
}

GoogleString NgxServerContext::FormatOption(StringPiece option_name,
                                            StringPiece args) {
  return StrCat("pagespeed ", option_name, " ", args, ";");
}

}  // namespace net_instaweb
