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

#include <memory>

#include "ngx_message_handler.h"
#include "pagespeed/system/daemon_adapter.h"
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
  virtual ~NgxServerContext();

  // We don't allow ProxyFetch to fetch HTML via MapProxyDomain. We will call
  // set_trusted_input() on any ProxyFetches we use to transform internal HTML.
  virtual bool ProxiesHtml() const { return false; }

  // Creates the UDS-backed DaemonReader for the /v1/daemon/* admin
  // endpoints, from this server block's DaemonApiSocketPath, or
  // nullptr when that path is empty (daemon API disabled).  See
  // SystemServerContext::NewDaemonReader().
  virtual DaemonReader* NewDaemonReader();

  // Builds this server's optimizer-daemon adapter from its configuration and
  // resolves its health, exactly once, at module-init time.  Returns false
  // when the server must not start; the reason has already been logged.
  //
  // Must run after configuration is complete and before any worker is
  // forked: the serving path only ever reads the verdict, so that it
  // neither probes nor logs per request.
  bool RunDaemonStartupCheck();

  // This server's daemon health.  kNotConfigured until
  // RunDaemonStartupCheck() has run, which is also the correct answer for
  // every path that reaches serving without a daemon configured.
  DaemonHealth daemon_health() const {
    return daemon_adapter_ == nullptr ? DaemonHealth::kNotConfigured
                                      : daemon_adapter_->health();
  }

  // This server's daemon adapter, or nullptr before the startup check has
  // run.  Borrowed; the record arm asks it for the per-process cache handle.
  DaemonAdapter* daemon_adapter() { return daemon_adapter_.get(); }

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

  virtual GoogleString FormatOption(StringPiece option_name, StringPiece args);

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
  // it must be destroyed first.
  std::unique_ptr<DaemonAdapter> daemon_adapter_;
  std::unique_ptr<DaemonServeStats> daemon_serve_stats_;

  NgxServerContext(const NgxServerContext&) = delete;
  NgxServerContext& operator=(const NgxServerContext&) = delete;
};

}  // namespace net_instaweb

#endif  // NGX_SERVER_CONTEXT_H_
