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

#pragma once

#include <map>
#include <memory>

#include "pagespeed/envoy/envoy_message_handler.h"
#include "pagespeed/envoy/envoy_vhost_config.h"
#include "pagespeed/system/system_server_context.h"

namespace net_instaweb {

class EnvoyRewriteDriverFactory;
class EnvoyRewriteOptions;
class SystemRequestContext;

class EnvoyServerContext : public SystemServerContext {
 public:
  EnvoyServerContext(EnvoyRewriteDriverFactory* factory, StringPiece hostname,
                     int port);

  // We don't allow ProxyFetch to fetch HTML via MapProxyDomain. We will call
  // set_trusted_input() on any ProxyFetches we use to transform internal HTML.
  bool ProxiesHtml() const override { return false; }

  // Creates the UDS-backed DaemonReader for the /v1/daemon/* admin
  // endpoints, from the configured DaemonApiSocketPath.  See
  // SystemServerContext::NewDaemonReader().
  DaemonReader* NewDaemonReader() override;

  // Call only when you need an EnvoyRewriteOptions.  If you don't need
  // Envoy-specific behavior, call global_options() instead which doesn't
  // downcast.
  EnvoyRewriteOptions* config();

  EnvoyRewriteDriverFactory* envoy_rewrite_driver_factory() {
    return envoy_factory_;
  }
  SystemRequestContext* NewRequestContext();
  SystemRequestContext* NewRequestContext(StringPiece hostname, int port);

  EnvoyMessageHandler* envoy_message_handler() {
    return dynamic_cast<EnvoyMessageHandler*>(message_handler());
  }

  GoogleString FormatOption(StringPiece option_name, StringPiece args) override;

  // VirtualHost configuration support.
  // Returns the VHost config manager for this server context.
  EnvoyVHostConfigManager* vhost_config_manager() {
    return &vhost_config_manager_;
  }

  // Find options for a given hostname. Returns nullptr if no VHost matches,
  // in which case global_options() should be used.
  const EnvoyRewriteOptions* FindOptionsForHost(StringPiece hostname) const;

  // Get options for a request, taking VHost configuration into account.
  // If a VHost matches the hostname, returns those options merged with global.
  // Otherwise returns global_options().
  // The returned pointer is valid for the lifetime of this server context.
  const RewriteOptions* GetOptionsForHost(StringPiece hostname);

 private:
  EnvoyRewriteDriverFactory* envoy_factory_;
  EnvoyVHostConfigManager vhost_config_manager_;
  // Cache of merged options per hostname (for VHost support).
  std::map<GoogleString, std::unique_ptr<EnvoyRewriteOptions>>
      merged_options_cache_;
  EnvoyServerContext(const EnvoyServerContext&) = delete;
  EnvoyServerContext& operator=(const EnvoyServerContext&) = delete;
};

}  // namespace net_instaweb
