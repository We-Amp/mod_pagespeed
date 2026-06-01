/** @file

    A brief file description

    @section license License

    Licensed to the Apache Software Foundation (ASF) under one
    or more contributor license agreements.  See the NOTICE file
    distributed with this work for additional information
    regarding copyright ownership.  The ASF licenses this file
    to you under the Apache License, Version 2.0 (the
    "License"); you may not use this file except in compliance
    with the License.  You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

    Unless required by applicable law or agreed to in writing, software
    distributed under the License is distributed on an "AS IS" BASIS,
    WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
    See the License for the specific language governing permissions and
    limitations under the License.
*/

#pragma once

#include <vector>

#include "net/instaweb/rewriter/public/process_context.h"
#include "pagespeed/kernel/base/google_message_handler.h"
#include "pagespeed/kernel/base/message_handler.h"
#include "pagespeed/kernel/base/scoped_ptr.h"
#include "pagespeed/kernel/base/string.h"
#include "pagespeed/system/external_server_spec.h"

namespace Envoy {
namespace Event {
class Dispatcher;
}  // namespace Event
}  // namespace Envoy

namespace net_instaweb {

class EnvoyRewriteDriverFactory;
class ProxyFetchFactory;
class EnvoyServerContext;

// Rewrite domain mapping configuration.
struct EnvoyRewriteDomainMapping {
  GoogleString to_domain;
  GoogleString from_domains;  // comma-separated
};

// Origin domain mapping configuration.
struct EnvoyOriginDomainMapping {
  GoogleString to_domain;
  GoogleString from_domains;  // comma-separated
  GoogleString host_header;
};

// Domain sharding configuration.
struct EnvoyDomainShard {
  GoogleString domain;
  GoogleString shards;  // comma-separated
};

// Configuration for PageSpeed caches.
struct EnvoyCacheConfig {
  // Redis configuration
  ExternalServerSpec redis_server;
  int64 redis_timeout_us = 0;  // 0 = use default
  int64 redis_reconnection_delay_ms = 0;  // 0 = use default
  int redis_database_index = -1;  // -1 = not set
  int redis_ttl_sec = -1;  // -1 = not set

  // Memcached configuration
  ExternalClusterSpec memcached_servers;
  int memcached_threads = 0;  // 0 = use default
  int64 memcached_timeout_us = 0;  // 0 = use default

  // File cache configuration
  GoogleString file_cache_path;
  GoogleString log_dir;
  int64 lru_cache_kb_per_process = 0;  // 0 = use default
  int64 file_cache_size_kb = 0;  // 0 = use default

  // Domain configuration
  std::vector<GoogleString> authorized_domains;
  std::vector<EnvoyRewriteDomainMapping> rewrite_mappings;
  std::vector<EnvoyOriginDomainMapping> origin_mappings;
  std::vector<EnvoyDomainShard> shards;

  // JavaScript library canonicalization entries.
  // Format: "size_bytes md5_hash canonical_url"
  std::vector<GoogleString> libraries;

  // Key for X-PSA-Blocking-Rewrite header support.
  GoogleString blocking_rewrite_key;

  // HTTPS fetch options (e.g., "enable,allow_self_signed").
  // Empty string means use default ("enable").
  GoogleString fetch_https_options;
};

class EnvoyProcessContext : public ProcessContext {
 public:
  explicit EnvoyProcessContext();
  explicit EnvoyProcessContext(const EnvoyCacheConfig& cache_config);
  ~EnvoyProcessContext() override{};

  MessageHandler* message_handler() { return message_handler_.get(); }
  EnvoyRewriteDriverFactory* driver_factory() { return driver_factory_.get(); }
  ProxyFetchFactory* proxy_fetch_factory() {
    return proxy_fetch_factory_.get();
  }
  EnvoyServerContext* server_context() { return server_context_; }

  // Initializes the Envoy dispatcher for native scheduling. Must be called
  // exactly once before any requests are processed. When called, PageSpeed
  // will use Envoy's event loop for scheduling instead of a separate thread.
  // If not called before the first request, falls back to SchedulerThread.
  void InitializeEnvoyDispatcher(Envoy::Event::Dispatcher* dispatcher);

  // Returns true if threads have been started (either via dispatcher or fallback).
  bool threads_started() const { return threads_started_; }

  // Shuts down the process context, stopping all threads and releasing resources.
  // After calling this, the process context cannot be reused.
  void ShutDown();

  // Returns true if ShutDown() has been called.
  bool is_shut_down() const { return shut_down_; }

 private:
  // Ensures threads are started (with fallback if no dispatcher was set).
  void EnsureThreadsStarted();

  std::unique_ptr<GoogleMessageHandler> message_handler_;
  std::unique_ptr<EnvoyRewriteDriverFactory> driver_factory_;
  std::unique_ptr<ProxyFetchFactory> proxy_fetch_factory_;
  EnvoyServerContext* server_context_;
  bool threads_started_;
  bool shut_down_ = false;
};

}  // namespace net_instaweb
