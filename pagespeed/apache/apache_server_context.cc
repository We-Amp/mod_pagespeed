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

#include "pagespeed/apache/apache_server_context.h"

#include <cstring>
#include <memory>

// http_protocol.h includes httpd.h. We need to include httpd_includes.h, which
// captures Apache's OK macro as APACHE_OK and undefines it.
#include "http_protocol.h"  // NOLINT
// After http_protocol.h, which brings in httpd.h: the directive tree.
#include "http_config.h"  // NOLINT
#include "net/instaweb/rewriter/config/measurement_proxy_rewrite_options_manager.h"
#include "net/instaweb/rewriter/public/measurement_proxy_url_namer.h"
#include "pagespeed/apache/apache_config.h"
#include "pagespeed/apache/apache_httpd_includes.h"
#include "pagespeed/apache/apache_request_context.h"
#include "pagespeed/apache/apache_rewrite_driver_factory.h"
#include "pagespeed/automatic/proxy_fetch.h"
#include "pagespeed/automatic/proxy_interface.h"
#include "pagespeed/kernel/base/file_system.h"
#include "pagespeed/kernel/base/message_handler.h"
#include "pagespeed/kernel/base/statistics.h"
#include "pagespeed/kernel/base/string_util.h"
#include "pagespeed/kernel/base/thread_system.h"
#include "pagespeed/kernel/http/http_names.h"
#include "pagespeed/system/uds_daemon_reader.h"

namespace net_instaweb {

const char ApacheServerContext::kProxyInterfaceStatsPrefix[] =
    "proxy-all-mode-";

ApacheServerContext::ApacheServerContext(ApacheRewriteDriverFactory* factory,
                                         server_rec* server,
                                         const StringPiece& version)
    : SystemServerContext(factory, server->server_hostname, server->port),
      apache_factory_(factory),
      server_rec_(server),
      version_(version.data(), version.size()) {
  // We may need the message handler for error messages very early, before
  // we get to InitServerContext in ChildInit().
  set_message_handler(apache_factory_->message_handler());

  // Currently, mod_pagespeed always runs upstream of mod_headers when used as
  // an origin server.  Note that in a proxy application, this might not be the
  // case.  I'm not sure how we can detect this on a per-request basis so
  // that might require a small refactor.
  //
  // TODO(jmarantz): We'd like to change this for various reasons but are unsure
  // of the impact.
  set_response_headers_finalized(false);
}

ApacheServerContext::~ApacheServerContext() {}

void ApacheServerContext::InitStats(Statistics* statistics) {
  ProxyInterface::InitStats(kProxyInterfaceStatsPrefix, statistics);
  SystemServerContext::InitStats(statistics);
}

bool ApacheServerContext::InitPath(const GoogleString& path) {
  if (file_system()->IsDir(path.c_str(), message_handler()).is_true()) {
    return true;
  }
  bool ok = file_system()->RecursivelyMakeDir(path, message_handler());
  if (ok) {
    apache_factory_->AddCreatedDirectory(path);
  }
  return ok;
}

bool ApacheServerContext::RunDaemonStartupCheck() {
  const ApacheConfig* config = global_config();
  daemon_adapter_ = std::make_unique<DaemonAdapter>(
      config->daemon_socket_path(), config->daemon_volume_path(),
      message_handler());
  const bool ok = daemon_adapter_->StartupCheck() == DaemonStartupStatus::kOk;
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

ApacheConfig* ApacheServerContext::global_config() {
  return ApacheConfig::DynamicCast(global_options());
}

DaemonReader* ApacheServerContext::NewDaemonReader() {
  const GoogleString& socket_path = global_config()->daemon_api_socket_path();
  if (socket_path.empty()) {
    return nullptr;  // Daemon API disabled: the console says "not configured".
  }
  return new UdsDaemonReader(thread_system(), timer(), socket_path,
                             message_handler());
}

const ApacheConfig* ApacheServerContext::global_config() const {
  return ApacheConfig::DynamicCast(global_options());
}

ApacheConfig* ApacheServerContext::SpdyConfigOverlay() {
  // While we no longer actually use the spdy config overlay, it's still
  // useful for backwards compatibility during parsing.
  if (spdy_config_overlay_.get() == nullptr) {
    spdy_config_overlay_ =
        std::make_unique<ApacheConfig>("spdy_overlay", thread_system());
    // We want to copy any implicit rewrite level from the parent,
    // so we don't end up overriding it with passthrough. It's also OK
    // to forward explicit one to an implicit one here, since an implicit
    // will never override an explicit one (even if its different).
    spdy_config_overlay_->SetDefaultRewriteLevel(global_config()->level());
  }
  return spdy_config_overlay_.get();
}

ApacheConfig* ApacheServerContext::NonSpdyConfigOverlay() {
  if (non_spdy_config_overlay_.get() == nullptr) {
    non_spdy_config_overlay_ =
        std::make_unique<ApacheConfig>("non_spdy_overlay", thread_system());
    // See ::SpdyConfigOverlay for explanation.
    non_spdy_config_overlay_->SetDefaultRewriteLevel(global_config()->level());
  }
  return non_spdy_config_overlay_.get();
}

void ApacheServerContext::CollapseConfigOverlaysAndComputeSignatures() {
  // These days we ignore the spdy overlay and merge-in the non-spdy one
  // unconditionally. A non-null spdy overlay means a <ModPagespeedIf spdy>
  // block carried directives, which are now silently dropped; warn once per
  // config load so the operator can remove the dead block.
  if (spdy_config_overlay_.get() != nullptr) {
    message_handler()->Message(
        kWarning,
        "<ModPagespeedIf spdy> is deprecated; its directives are ignored. "
        "Please remove the block from your configuration.");
  }
  if (non_spdy_config_overlay_.get() != nullptr) {
    global_config()->Merge(*non_spdy_config_overlay_);
  }

  SystemServerContext::CollapseConfigOverlaysAndComputeSignatures();

  spdy_config_overlay_.reset();
  non_spdy_config_overlay_.reset();
}

bool ApacheServerContext::PoolDestroyed() {
  DCHECK_EQ(num_active_rewrite_drivers(), 0);
  return apache_factory_->PoolDestroyed(this);
}

void ApacheServerContext::InitProxyFetchFactory() {
  proxy_fetch_factory_ = std::make_unique<ProxyFetchFactory>(this);
}

ApacheRequestContext* ApacheServerContext::NewApacheRequestContext(
    request_rec* request) {
  return new ApacheRequestContext(thread_system()->NewMutex(), timer(),
                                  request);
}

void ApacheServerContext::ReportNotFoundHelper(MessageType message_type,
                                               StringPiece error_message,
                                               request_rec* request,
                                               Variable* error_count) {
  error_count->Add(1);
  request->status = HttpStatus::kNotFound;
  ap_send_error_response(request, 0);
  message_handler()->Message(
      message_type, "%s %s: not found (404)",
      (error_message.empty() ? "(null)" : error_message.as_string().c_str()),
      error_count->GetName().as_string().c_str());
}

GoogleString ApacheServerContext::FormatOption(StringPiece option_name,
                                               StringPiece args) {
  return StrCat("ModPagespeed", option_name, " ", args);
}

void ApacheServerContext::ChildInit(SystemRewriteDriverFactory* f) {
  if (global_config()->proxy_all_requests_mode()) {
    apache_factory_->SetNeedSchedulerThread();
    if (global_config()->measurement_proxy_mode()) {
      measurement_url_namer_ = std::make_unique<MeasurementProxyUrlNamer>(
          global_config()->measurement_proxy_root(),
          global_config()->measurement_proxy_password());
      set_url_namer(measurement_url_namer_.get());
      SetRewriteOptionsManager(new MeasurementProxyRewriteOptionsManager(
          this, global_config()->measurement_proxy_root(),
          global_config()->measurement_proxy_password()));
    }
  }
  SystemServerContext::ChildInit(f);
  // The optimizer-daemon start-up check ran in the parent, before the shared
  // message buffer existed; repeat a refusal so the admin console's message
  // history shows it.
  if (daemon_adapter_ != nullptr) daemon_adapter_->ReannounceStartupRefusal();
}

namespace {

// httpd's names for a virtual host it could not name (server/vhost.c,
// ap_fini_vhost_config): never a site's name.
bool IsHttpdPlaceholderName(StringPiece name) {
  return name == "bogus_host_without_forward_dns" ||
         name == "bogus_host_without_reverse_dns";
}

}  // namespace

namespace {

bool IsDirective(const ap_directive_t* node, const char* name) {
  return node->directive != nullptr && StringCaseEqual(node->directive, name);
}

// A ServerName directive among the siblings starting at `first`.
bool HasServerName(const ap_directive_t* first) {
  for (const ap_directive_t* node = first; node != nullptr; node = node->next) {
    if (IsDirective(node, "ServerName")) {
      return true;
    }
  }
  return false;
}

}  // namespace

bool ApacheConfigStatesServerName(const ap_directive_t* tree,
                                  const server_rec* server,
                                  const server_rec* main_server) {
  if (tree == nullptr || server == nullptr || main_server == nullptr ||
      main_server->is_virtual) {
    return false;
  }
  if (!server->is_virtual) {
    // Only the main record itself; any other non-virtual record is unknown.
    return server == main_server && HasServerName(tree);
  }
  if (server->defn_name == nullptr) {
    return false;
  }
  for (const ap_directive_t* node = tree; node != nullptr; node = node->next) {
    if (IsDirective(node, "<VirtualHost") && node->filename != nullptr &&
        node->line_num >= 0 &&
        static_cast<unsigned>(node->line_num) == server->defn_line_number &&
        strcmp(node->filename, server->defn_name) == 0) {
      return HasServerName(node->first_child);
    }
  }
  return false;
}

std::set<const server_rec*> ApacheStatedServerNamesFromTree(
    const ap_directive_t* tree, const server_rec* main_server) {
  std::set<const server_rec*> stated;
  if (tree == nullptr || main_server == nullptr || main_server->is_virtual) {
    return stated;
  }
  for (const server_rec* server = main_server; server != nullptr;
       server = server->next) {
    if (ApacheConfigStatesServerName(tree, server, main_server)) {
      stated.insert(server);
    }
  }
  return stated;
}

ConfiguredHostNames ApacheConfiguredHostNames(
    const server_rec* server, const server_rec* main_server,
    const std::set<const server_rec*>& stated_server_names) {
  ConfiguredHostNames names;
  if (server == nullptr) {
    return names;
  }
  // A main record that is itself a virtual host is not one: unknown.
  if (main_server != nullptr && main_server->is_virtual) {
    main_server = nullptr;
  }
  const bool is_main = server == main_server || !server->is_virtual;
  // A virtual host without ServerName on a default address shares the main
  // server's server_hostname POINTER; a configured ServerName is always its
  // own allocation, so pointer equality means "inherited".  The stated set
  // already leaves such a host out; this stays as a second check.
  const bool inherited =
      !is_main && (main_server == nullptr ||
                   server->server_hostname == main_server->server_hostname);
  const bool stated = stated_server_names.count(server) != 0;
  if (stated && server->server_hostname != nullptr && !inherited &&
      !IsHttpdPlaceholderName(server->server_hostname)) {
    names.primary = server->server_hostname;
  }
  // Exact ServerAlias names only -- always stated by the configuration;
  // Apache keeps the wildcard ones apart in server->wild_names.
  if (server->names != nullptr) {
    const char* const* aliases =
        reinterpret_cast<const char* const*>(server->names->elts);
    for (int i = 0; i < server->names->nelts; ++i) {
      if (aliases[i] != nullptr) {
        names.aliases.push_back(aliases[i]);
      }
    }
  }
  return names;
}

}  // namespace net_instaweb
