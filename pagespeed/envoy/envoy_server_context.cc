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

#include "pagespeed/envoy/envoy_server_context.h"

#include <memory>

#include "net/instaweb/rewriter/public/rewrite_driver.h"
#include "pagespeed/envoy/envoy_message_handler.h"
#include "pagespeed/envoy/envoy_rewrite_driver_factory.h"
#include "pagespeed/envoy/envoy_rewrite_options.h"
#include "pagespeed/system/add_headers_fetcher.h"
#include "pagespeed/system/loopback_route_fetcher.h"
#include "pagespeed/system/system_request_context.h"
#include "pagespeed/system/uds_daemon_reader.h"

namespace net_instaweb {

EnvoyServerContext::EnvoyServerContext(EnvoyRewriteDriverFactory* factory,
                                       StringPiece hostname, int port)
    : SystemServerContext(factory, hostname, port), envoy_factory_(factory) {}

EnvoyRewriteOptions* EnvoyServerContext::config() {
  return EnvoyRewriteOptions::DynamicCast(global_options());
}

DaemonReader* EnvoyServerContext::NewDaemonReader() {
  return new UdsDaemonReader(thread_system(), timer(),
                             config()->daemon_api_socket_path(),
                             message_handler());
}

SystemRequestContext* EnvoyServerContext::NewRequestContext() {
  // Default to localhost:80 for backward compatibility.
  return NewRequestContext("localhost", 80);
}

SystemRequestContext* EnvoyServerContext::NewRequestContext(
    StringPiece hostname, int port) {
  // Parse hostname to extract port if present (e.g., "localhost:8080").
  GoogleString host_str(hostname.data(), hostname.size());
  int actual_port = port;
  size_t colon_pos = host_str.find(':');
  if (colon_pos != GoogleString::npos) {
    GoogleString port_str = host_str.substr(colon_pos + 1);
    actual_port = atoi(port_str.c_str());
    host_str = host_str.substr(0, colon_pos);
  }
  SystemRequestContext* ctx = new SystemRequestContext(
      thread_system()->NewMutex(), timer(), host_str, actual_port, host_str);
  ctx->set_using_http2(false);
  // No connection info is available here, so local_scheme stays unset and
  // LoopbackRouteFetcher keeps the resource URL's scheme when munging
  // (the X-Forwarded-Proto scheme correction is not wired for this port
  // yet).
  return ctx;
}

GoogleString EnvoyServerContext::FormatOption(StringPiece option_name,
                                              StringPiece args) {
  return StrCat("pagespeed ", option_name, " ", args, ";");
}

const EnvoyRewriteOptions* EnvoyServerContext::FindOptionsForHost(
    StringPiece hostname) const {
  return vhost_config_manager_.FindOptionsForHost(hostname);
}

const RewriteOptions* EnvoyServerContext::GetOptionsForHost(
    StringPiece hostname) {
  // Check if we have a cached merged options for this host.
  GoogleString hostname_str = hostname.as_string();
  auto it = merged_options_cache_.find(hostname_str);
  if (it != merged_options_cache_.end()) {
    return it->second.get();
  }

  // Look up VHost-specific options.
  const EnvoyRewriteOptions* vhost_options = FindOptionsForHost(hostname);
  if (vhost_options == nullptr) {
    // No VHost match - use global options.
    return global_options();
  }

  // Create merged options: global + VHost.
  auto* merged = EnvoyRewriteOptions::DynamicCast(global_options()->Clone());
  merged->Merge(*vhost_options);

  // Cache the merged options.
  merged_options_cache_[hostname_str] =
      std::unique_ptr<EnvoyRewriteOptions>(merged);
  return merged_options_cache_[hostname_str].get();
}

}  // namespace net_instaweb
