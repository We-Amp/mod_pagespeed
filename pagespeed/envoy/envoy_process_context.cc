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

#include "pagespeed/envoy/envoy_process_context.h"

#include <memory>
#include <vector>

#include "net/instaweb/rewriter/public/domain_lawyer.h"
#include "pagespeed/automatic/proxy_fetch.h"
#include "pagespeed/envoy/envoy_message_handler.h"
#include "pagespeed/envoy/envoy_rewrite_driver_factory.h"
#include "pagespeed/envoy/envoy_rewrite_options.h"
#include "pagespeed/envoy/envoy_server_context.h"
#include "pagespeed/envoy/envoy_thread_system.h"
#include "pagespeed/kernel/base/string_util.h"
#include "pagespeed/kernel/thread/pthread_shared_mem.h"

namespace net_instaweb {

EnvoyProcessContext::EnvoyProcessContext()
    : EnvoyProcessContext(EnvoyCacheConfig()) {}

EnvoyProcessContext::EnvoyProcessContext(const EnvoyCacheConfig& cache_config)
    : ProcessContext(), threads_started_(false) {
  EnvoyRewriteOptions::Initialize();
  EnvoyRewriteDriverFactory::Initialize();
  // net_instaweb::log_message_handler::Install();

  EnvoyThreadSystem* ts = new EnvoyThreadSystem();
  message_handler_ = std::make_unique<GoogleMessageHandler>();
  driver_factory_ = std::make_unique<EnvoyRewriteDriverFactory>(
      *this, ts, "" /*hostname, not used*/, -1 /*port, not used*/);
  // Note: StartThreads() is NOT called here. It will be called either:
  // 1. When InitializeEnvoyDispatcher() is called with an Envoy dispatcher
  // 2. Lazily on first request via EnsureThreadsStarted()
  driver_factory_->Init();
  server_context_ = driver_factory()->MakeEnvoyServerContext("", -1);

  EnvoyRewriteOptions* root_options_ =
      dynamic_cast<EnvoyRewriteOptions*>(driver_factory_->default_options());
  EnvoyRewriteOptions* server_options = root_options_->Clone();
  EnvoyRewriteOptions* options =
      new EnvoyRewriteOptions(driver_factory_->thread_system());

  // Apply cache configuration from proto.
  if (!cache_config.redis_server.empty()) {
    options->set_redis_server(cache_config.redis_server);
    message_handler_->Message(kInfo, "Redis server configured: %s",
                              cache_config.redis_server.ToString().c_str());
  }
  if (!cache_config.memcached_servers.empty()) {
    options->set_memcached_servers(cache_config.memcached_servers);
    message_handler_->Message(
        kInfo, "Memcached servers configured: %s",
        cache_config.memcached_servers.ToString().c_str());
  }
  if (cache_config.memcached_threads > 0) {
    options->set_memcached_threads(cache_config.memcached_threads);
  }
  if (cache_config.memcached_timeout_us > 0) {
    options->set_memcached_timeout_us(
        static_cast<int>(cache_config.memcached_timeout_us));
  }
  if (!cache_config.file_cache_path.empty()) {
    options->set_file_cache_path(cache_config.file_cache_path);
  }
  if (!cache_config.log_dir.empty()) {
    options->set_log_dir(cache_config.log_dir);
  }
  if (cache_config.lru_cache_kb_per_process > 0) {
    options->set_lru_cache_kb_per_process(
        cache_config.lru_cache_kb_per_process);
  }
  if (cache_config.file_cache_size_kb > 0) {
    options->set_file_cache_clean_size_kb(cache_config.file_cache_size_kb);
  }
  if (!cache_config.blocking_rewrite_key.empty()) {
    options->set_blocking_rewrite_key(cache_config.blocking_rewrite_key);
    message_handler_->Message(kInfo, "Blocking rewrite key configured");
  }

  // Apply HTTPS fetch options (e.g., "enable,allow_self_signed").
  if (!cache_config.fetch_https_options.empty()) {
    GoogleString msg;
    RewriteOptions::OptionSettingResult result =
        options->ParseAndSetOptionFromName1(RewriteOptions::kFetchHttps,
                                            cache_config.fetch_https_options,
                                            &msg, message_handler_.get());
    if (result == RewriteOptions::kOptionOk) {
      message_handler_->Message(kInfo, "FetchHttps options configured: %s",
                                cache_config.fetch_https_options.c_str());
    } else {
      message_handler_->Message(
          kWarning, "Failed to set FetchHttps options '%s': %s",
          cache_config.fetch_https_options.c_str(), msg.c_str());
    }
  }

  // Apply domain configuration.
  DomainLawyer* domain_lawyer = options->WriteableDomainLawyer();

  // Add authorized domains.
  for (const auto& domain : cache_config.authorized_domains) {
    if (!domain_lawyer->AddDomain(domain, message_handler_.get())) {
      message_handler_->Message(kWarning, "Failed to add authorized domain: %s",
                                domain.c_str());
    } else {
      message_handler_->Message(kInfo, "Added authorized domain: %s",
                                domain.c_str());
    }
  }

  // Add rewrite domain mappings (for CDN).
  for (const auto& mapping : cache_config.rewrite_mappings) {
    if (!domain_lawyer->AddRewriteDomainMapping(
            mapping.to_domain, mapping.from_domains, message_handler_.get())) {
      message_handler_->Message(
          kWarning, "Failed to add rewrite domain mapping: %s -> %s",
          mapping.from_domains.c_str(), mapping.to_domain.c_str());
    } else {
      message_handler_->Message(kInfo, "Added rewrite domain mapping: %s -> %s",
                                mapping.from_domains.c_str(),
                                mapping.to_domain.c_str());
    }
  }

  // Add origin domain mappings (for fetch routing).
  for (const auto& mapping : cache_config.origin_mappings) {
    if (!domain_lawyer->AddOriginDomainMapping(
            mapping.to_domain, mapping.from_domains, mapping.host_header,
            message_handler_.get())) {
      message_handler_->Message(
          kWarning, "Failed to add origin domain mapping: %s -> %s",
          mapping.from_domains.c_str(), mapping.to_domain.c_str());
    } else {
      message_handler_->Message(kInfo, "Added origin domain mapping: %s -> %s",
                                mapping.from_domains.c_str(),
                                mapping.to_domain.c_str());
    }
  }

  // Add domain sharding.
  for (const auto& shard : cache_config.shards) {
    if (!domain_lawyer->AddShard(shard.domain, shard.shards,
                                 message_handler_.get())) {
      message_handler_->Message(kWarning,
                                "Failed to add domain shard: %s -> %s",
                                shard.domain.c_str(), shard.shards.c_str());
    } else {
      message_handler_->Message(kInfo, "Added domain shard: %s -> %s",
                                shard.domain.c_str(), shard.shards.c_str());
    }
  }

  server_options->Merge(*options);
  delete options;

  server_context_->global_options()->Merge(*server_options);
  delete server_options;

  // Register JavaScript libraries for canonicalization.
  // This must be done BEFORE PostConfig() which freezes options.
  if (!cache_config.libraries.empty()) {
    message_handler_->Message(kInfo, "Registering %zu JavaScript libraries",
                              cache_config.libraries.size());
    RewriteOptions* global_opts = server_context_->global_options();
    int registered = 0;
    for (const auto& entry : cache_config.libraries) {
      // Parse format: "size_bytes md5_hash canonical_url"
      StringPieceVector parts;
      SplitStringPieceToVector(entry, " ", &parts, true);
      if (parts.size() < 3) {
        message_handler_->Message(
            kWarning, "Invalid library entry (expected 'size hash url'): %s",
            entry.c_str());
        continue;
      }
      int64 size_bytes;
      if (!StringToInt64(parts[0], &size_bytes) || size_bytes <= 0) {
        message_handler_->Message(kWarning,
                                  "Invalid library size: %s in entry: %s",
                                  parts[0].as_string().c_str(), entry.c_str());
        continue;
      }
      StringPiece md5_hash = parts[1];
      // URL may contain spaces, so join remaining parts
      GoogleString canonical_url;
      for (size_t i = 2; i < parts.size(); ++i) {
        if (i > 2) canonical_url += " ";
        parts[i].AppendToString(&canonical_url);
      }
      if (global_opts->RegisterLibrary(size_bytes, md5_hash, canonical_url)) {
        message_handler_->Message(
            kInfo, "Registered library: size=%lld hash=%s url=%s",
            static_cast<long long>(size_bytes), md5_hash.as_string().c_str(),
            canonical_url.c_str());
        ++registered;
      } else {
        message_handler_->Message(
            kWarning, "Failed to register library: size=%lld hash=%s url=%s",
            static_cast<long long>(size_bytes), md5_hash.as_string().c_str(),
            canonical_url.c_str());
      }
    }
    message_handler_->Message(kInfo, "Successfully registered %d/%zu libraries",
                              registered, cache_config.libraries.size());
  }

  message_handler_->Message(
      kInfo, "Process context constructed:\r\n %s",
      driver_factory_->default_options()->OptionsToString().c_str());
  message_handler_->Message(
      kInfo, "Server context global options:\r\n %s",
      server_context_->global_options()->OptionsToString().c_str());

  std::vector<SystemServerContext*> server_contexts;
  server_contexts.push_back(server_context_);

  // Statistics* statistics =
  //    driver_factory_->MakeGlobalSharedMemStatistics(*(SystemRewriteOptions*)server_context_->global_options());
  GoogleString error_message;
  int error_index = -1;
  Statistics* global_statistics = nullptr;
  driver_factory_.get()->PostConfig(server_contexts, &error_message,
                                    &error_index, &global_statistics);
  if (error_index != -1) {
    server_contexts[error_index]->message_handler()->Message(
        kError, "pagespeed is enabled. %s", error_message.c_str());
    CHECK(false);
  }

  EnvoyRewriteDriverFactory::InitStats(global_statistics);

  driver_factory()->RootInit();
  driver_factory()->ChildInit();

  proxy_fetch_factory_ = std::make_unique<ProxyFetchFactory>(server_context_);
}

void EnvoyProcessContext::InitializeEnvoyDispatcher(
    Envoy::Event::Dispatcher* dispatcher) {
  if (threads_started_) {
    // Already started - can't change dispatcher after threads are running.
    message_handler_->Message(
        kWarning,
        "InitializeEnvoyDispatcher called after threads already started");
    return;
  }
  driver_factory_->SetEnvoyDispatcher(dispatcher);
  driver_factory_->StartThreads();
  threads_started_ = true;
  message_handler_->Message(kInfo, "Initialized with Envoy-native dispatcher");
}

void EnvoyProcessContext::EnsureThreadsStarted() {
  if (threads_started_) {
    return;
  }
  // Fallback: start threads without Envoy dispatcher (uses SchedulerThread).
  driver_factory_->StartThreads();
  threads_started_ = true;
  message_handler_->Message(kInfo,
                            "Started threads with fallback SchedulerThread");
}

void EnvoyProcessContext::ShutDown() {
  if (shut_down_) {
    return;
  }
  shut_down_ = true;
  message_handler_->Message(kInfo, "Shutting down EnvoyProcessContext");

  // Shut down the proxy fetch factory first (stops accepting new requests).
  proxy_fetch_factory_.reset();

  // Shut down the driver factory (stops threads, cleans up resources).
  if (driver_factory_) {
    driver_factory_->ShutDown();
  }
}

}  // namespace net_instaweb