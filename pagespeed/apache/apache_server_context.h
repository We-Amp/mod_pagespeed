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

#ifndef PAGESPEED_APACHE_APACHE_SERVER_CONTEXT_H_
#define PAGESPEED_APACHE_APACHE_SERVER_CONTEXT_H_

#include <memory>
#include <set>

#include "net/instaweb/http/public/request_context.h"
#include "net/instaweb/rewriter/public/rewrite_stats.h"
#include "pagespeed/apache/apache_config.h"
#include "pagespeed/kernel/base/basictypes.h"
#include "pagespeed/kernel/base/message_handler.h"
#include "pagespeed/kernel/base/string.h"
#include "pagespeed/kernel/base/string_util.h"
#include "pagespeed/system/daemon_adapter.h"
#include "pagespeed/system/daemon_serve_arm.h"
#include "pagespeed/system/serve_host_names.h"
#include "pagespeed/system/system_server_context.h"

struct ap_directive_t;
struct request_rec;
struct server_rec;

namespace net_instaweb {

class ApacheRewriteDriverFactory;
class ApacheRequestContext;
class MeasurementProxyUrlNamer;
class ProxyFetchFactory;
class RewriteDriverPool;
class RewriteDriver;
class Statistics;
class Variable;

// Creates an Apache-specific ServerContext.  This differs from base class
// that it incorporates by adding per-VirtualHost configuration, including:
//    - file-cache path & limits
//    - default RewriteOptions.
// Additionally, there are startup semantics for apache's prefork model
// that require a phased initialization.
class ApacheServerContext : public SystemServerContext {
 public:
  // Prefix for ProxyInterface stats (active in proxy_all_requests_mode() only).
  static const char kProxyInterfaceStatsPrefix[];

  ApacheServerContext(ApacheRewriteDriverFactory* factory, server_rec* server,
                      const StringPiece& version);
  ~ApacheServerContext() override;

  // This must be called for every statistics object in use before using this.
  static void InitStats(Statistics* statistics);

  ApacheRewriteDriverFactory* apache_factory() { return apache_factory_; }
  ApacheConfig* global_config();
  const ApacheConfig* global_config() const;
  bool InitPath(const GoogleString& path);

  // Builds this server's optimizer-daemon adapter from its configuration and
  // resolves its health, exactly once, at post-config time.  Returns false
  // when the server must not start; the reason has already been logged.
  //
  // Must run after directives are parsed and before any request is served:
  // the serving path only ever reads the verdict, so that it neither probes
  // nor logs per request.
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

  // Creates the UDS-backed DaemonReader for the /v1/daemon/* admin
  // endpoints, from this server's DaemonApiSocketPath, or
  // nullptr when that path is empty (daemon API disabled).  See
  // SystemServerContext::NewDaemonReader().
  DaemonReader* NewDaemonReader() override;

  // The peer's serve-stats mmap for this server, or nullptr when no daemon
  // is configured.  Borrowed; PROCESS-lifetime, and deliberately not opened
  // here -- the object is built at configuration time and opens the file on
  // its first recorded serve, in whichever child process makes it.  Opening
  // in the parent would hand every child an inherited mapping it never asked
  // for, which is the same fork problem the record cache solves the same way.
  DaemonServeStats* daemon_serve_stats() { return daemon_serve_stats_.get(); }

  // These return configuration objects that hold settings from
  // <ModPagespeedIf spdy> and <ModPagespeedIf !spdy> sections of configuration.
  // They initialize lazily, so are not thread-safe; however they are only
  // meant to be used during configuration parsing. These methods should be
  // called only if there is actually a need to put something in them, since
  // otherwise we may end up constructing separate SPDY vs. non-SPDY
  // configurations needlessly.
  ApacheConfig* SpdyConfigOverlay();
  ApacheConfig* NonSpdyConfigOverlay();

  // These return true if the given overlays were constructed (in response
  // to having something in config files to put in them).
  bool has_spdy_config_overlay() const {
    return spdy_config_overlay_.get() != NULL;
  }

  bool has_non_spdy_config_overlay() const {
    return non_spdy_config_overlay_.get() != NULL;
  }

  // These two take ownership of their parameters.
  void set_spdy_config_overlay(ApacheConfig* x) {
    spdy_config_overlay_.reset(x);
  }

  void set_non_spdy_config_overlay(ApacheConfig* x) {
    non_spdy_config_overlay_.reset(x);
  }

  // This should be called after all configuration parsing is done to collapse
  // configuration inside the config overlays into actual ApacheConfig objects.
  // It will also compute signatures when done.
  void CollapseConfigOverlaysAndComputeSignatures() override;

  // Called on notification from Apache on child exit. Returns true
  // if this is the last ServerContext that exists.
  bool PoolDestroyed();

  const server_rec* server() const { return server_rec_; }

  ProxyFetchFactory* proxy_fetch_factory() {
    return proxy_fetch_factory_.get();
  }

  void InitProxyFetchFactory();

  // We only proxy external HTML from mod_pagespeed in Apache using the
  // ProxyFetch flow if proxy_all_requests_mode() is on in config.  In the usual
  // case, we handle HTML as an Apache filter, letting something like mod_proxy
  // (or one of our own test modes like slurp) do the fetching.
  bool ProxiesHtml() const override {
    return global_config()->proxy_all_requests_mode();
  }

  ApacheRequestContext* NewApacheRequestContext(request_rec* request);

  // Reports an error status to the HTTP resource request, and logs
  // the error as a Warning to the log file, and bumps a stat as
  // needed.
  void ReportResourceNotFound(StringPiece message, request_rec* request) {
    ReportNotFoundHelper(kWarning, message, request,
                         rewrite_stats()->resource_404_count());
  }

  // Reports an error status to the HTTP statistics request, and logs
  // the error as a Warning to the log file, and bumps a stat as
  // needed.
  void ReportStatisticsNotFound(StringPiece message, request_rec* request) {
    ReportNotFoundHelper(kWarning, message, request, statistics_404_count());
  }

  // Reports an error status to the HTTP slurp request, and logs
  // the error as a Warning to the log file, and bumps a stat as
  // needed.
  void ReportSlurpNotFound(StringPiece message, request_rec* request) {
    ReportNotFoundHelper(kInfo, message, request,
                         rewrite_stats()->slurp_404_count());
  }

  GoogleString FormatOption(StringPiece option_name, StringPiece args) override;

 private:
  void ChildInit(SystemRewriteDriverFactory* factory) override;

  void ReportNotFoundHelper(MessageType message_type, StringPiece url,
                            request_rec* request, Variable* error_count);

  ApacheRewriteDriverFactory* apache_factory_;
  server_rec* server_rec_;
  GoogleString version_;

  // May be NULL. Constructed once we see things in config files that should
  // be stored in these.
  std::unique_ptr<ApacheConfig> spdy_config_overlay_;
  std::unique_ptr<ApacheConfig> non_spdy_config_overlay_;

  // May be NULL. Only constructed in measurement proxy mode.
  std::unique_ptr<MeasurementProxyUrlNamer> measurement_url_namer_;

  std::unique_ptr<ProxyFetchFactory> proxy_fetch_factory_;
  std::unique_ptr<DaemonAdapter> daemon_adapter_;
  std::unique_ptr<DaemonServeStats> daemon_serve_stats_;

  ApacheServerContext(const ApacheServerContext&) = delete;
  ApacheServerContext& operator=(const ApacheServerContext&) = delete;
};

// The names an Apache virtual host is configured with, from its own server
// record (`server`, the request's r->server), the main server's record
// (`main_server`) and the records whose configuration states a ServerName
// (`stated_server_names`, ApacheStatedServerNamesFromTree): ServerName as
// the primary name and the exact ServerAlias names (server->names).
// Wildcard aliases (server->wild_names) are left out -- a host one of them
// matched counts under the primary name.  There is NO primary name when
//  - the configuration does not state a ServerName for `server`: httpd
//    then derives one (the main server gets the machine's name, a virtual
//    host on its own address the reverse name of that address), and a
//    derived name can be another site's;
//  - `server` is a virtual host whose server_hostname is the main record's
//    pointer: httpd gives an unnamed virtual host on a default address the
//    main server's name (server/vhost.c, ap_fini_vhost_config), and such a
//    host must not record under -- or be shown -- the main site's row;
//  - the name is one of httpd's placeholders for a virtual host without a
//    usable address ("bogus_host_without_forward_dns",
//    "bogus_host_without_reverse_dns");
//  - the main record is unknown (nullptr, or itself a virtual host) and
//    `server` is a virtual host: inheritance cannot be told apart.
// Read per request, never stored on a server context: a virtual host
// without directives of its own shares the main server's context.  nullptr
// gives no names.
ConfiguredHostNames ApacheConfiguredHostNames(
    const server_rec* server, const server_rec* main_server,
    const std::set<const server_rec*>& stated_server_names);

// Whether httpd's parsed configuration (`tree`, the top level of the
// directive tree) states a ServerName for `server`:
//  - the main record (`server` == `main_server`, not a virtual host): a
//    top-level ServerName directive;
//  - a virtual host: a ServerName directive directly inside the top-level
//    <VirtualHost> node the record was created from (the node's file name
//    and line equal the record's defn_name and defn_line_number).
// Directive names compare case-insensitively.  Included files and
// conditional sections are already spliced into the tree where they were
// read.  Anything else -- no tree, no record, no main record, a record
// without defn_name, no matching node -- is false.
bool ApacheConfigStatesServerName(const ap_directive_t* tree,
                                  const server_rec* server,
                                  const server_rec* main_server);

// Every record -- the main record and the virtual hosts on its `next`
// chain -- whose configuration states a ServerName.  Computed once per
// configuration, while the tree and the records are those of the same
// configuration; empty without a tree or a (non-virtual) main record.
std::set<const server_rec*> ApacheStatedServerNamesFromTree(
    const ap_directive_t* tree, const server_rec* main_server);

}  // namespace net_instaweb

#endif  // PAGESPEED_APACHE_APACHE_SERVER_CONTEXT_H_
