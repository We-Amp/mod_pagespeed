#include <string>

#include "absl/status/statusor.h"
#include "envoy/registry/registry.h"
#include "http_filter.h"
#include "net/instaweb/rewriter/public/domain_lawyer.h"
#include "net/instaweb/rewriter/public/process_context.h"
#include "pagespeed/automatic/proxy_fetch.h"
#include "pagespeed/envoy/envoy_process_context.h"
#include "pagespeed/envoy/envoy_rewrite_driver_factory.h"
#include "pagespeed/envoy/envoy_rewrite_options.h"
#include "pagespeed/envoy/envoy_server_context.h"
#include "pagespeed/envoy/envoy_vhost_config.h"
#include "pagespeed/envoy/http_filter.pb.h"
#include "pagespeed/envoy/http_filter.pb.validate.h"
#include "pagespeed/system/system_thread_system.h"

using namespace net_instaweb;

// XXX(oschaaf): use in-proc shared mem?
// #define PAGESPEED_SUPPORT_POSIX_SHARED_MEM

namespace Envoy {
namespace Server {
namespace Configuration {

namespace {
EnvoyProcessContext* g_process_context = nullptr;

// Convert proto config to cache config struct.
EnvoyCacheConfig protoToCacheConfig(const pagespeed::Decoder& proto_config) {
  EnvoyCacheConfig config;

  // Redis configuration
  if (proto_config.has_redis()) {
    const auto& redis = proto_config.redis();
    if (redis.has_server() && !redis.server().host().empty()) {
      config.redis_server.host = redis.server().host();
      config.redis_server.port =
          redis.server().port() > 0 ? redis.server().port() : 6379;
    }
    if (redis.timeout_us() > 0) {
      config.redis_timeout_us = redis.timeout_us();
    }
    if (redis.reconnection_delay_ms() > 0) {
      config.redis_reconnection_delay_ms = redis.reconnection_delay_ms();
    }
    if (redis.database_index() >= 0) {
      config.redis_database_index = redis.database_index();
    }
    if (redis.ttl_sec() > 0) {
      config.redis_ttl_sec = redis.ttl_sec();
    }
  }

  // Memcached configuration
  if (proto_config.has_memcached()) {
    const auto& memcached = proto_config.memcached();
    for (const auto& server : memcached.servers()) {
      if (!server.host().empty()) {
        ExternalServerSpec spec;
        spec.host = server.host();
        spec.port = server.port() > 0 ? server.port() : 11211;
        config.memcached_servers.servers.push_back(spec);
      }
    }
    if (memcached.threads() > 0) {
      config.memcached_threads = memcached.threads();
    }
    if (memcached.timeout_us() > 0) {
      config.memcached_timeout_us = memcached.timeout_us();
    }
  }

  // File cache configuration
  if (!proto_config.file_cache_path().empty()) {
    config.file_cache_path = proto_config.file_cache_path();
  }
  if (!proto_config.log_dir().empty()) {
    config.log_dir = proto_config.log_dir();
  }
  if (proto_config.lru_cache_kb_per_process() > 0) {
    config.lru_cache_kb_per_process = proto_config.lru_cache_kb_per_process();
  }
  if (proto_config.file_cache_size_kb() > 0) {
    config.file_cache_size_kb = proto_config.file_cache_size_kb();
  }

  // Domain configuration
  if (proto_config.has_domains()) {
    const auto& domains = proto_config.domains();

    // Authorized domains
    for (const auto& domain : domains.authorized_domains()) {
      if (!domain.empty()) {
        config.authorized_domains.push_back(domain);
      }
    }

    // Rewrite domain mappings
    for (const auto& mapping : domains.rewrite_mappings()) {
      if (!mapping.to_domain().empty() && !mapping.from_domains().empty()) {
        EnvoyRewriteDomainMapping m;
        m.to_domain = mapping.to_domain();
        m.from_domains = mapping.from_domains();
        config.rewrite_mappings.push_back(m);
      }
    }

    // Origin domain mappings
    for (const auto& mapping : domains.origin_mappings()) {
      if (!mapping.to_domain().empty() && !mapping.from_domains().empty()) {
        EnvoyOriginDomainMapping m;
        m.to_domain = mapping.to_domain();
        m.from_domains = mapping.from_domains();
        m.host_header = mapping.host_header();
        config.origin_mappings.push_back(m);
      }
    }

    // Domain shards
    for (const auto& shard : domains.shards()) {
      if (!shard.domain().empty() && !shard.shards().empty()) {
        EnvoyDomainShard s;
        s.domain = shard.domain();
        s.shards = shard.shards();
        config.shards.push_back(s);
      }
    }
  }

  // JavaScript library canonicalization entries
  for (const auto& library_entry : proto_config.libraries()) {
    if (!library_entry.empty()) {
      config.libraries.push_back(library_entry);
    }
  }

  return config;
}

// Apply domain configuration to rewrite options.
void applyDomainConfig(const pagespeed::DomainConfig& domains,
                       EnvoyRewriteOptions* options,
                       MessageHandler* message_handler) {
  DomainLawyer* domain_lawyer = options->WriteableDomainLawyer();

  // Add authorized domains.
  for (const auto& domain : domains.authorized_domains()) {
    if (!domain.empty()) {
      if (!domain_lawyer->AddDomain(domain, message_handler)) {
        message_handler->Message(kWarning,
                                 "VHost: Failed to add authorized domain: %s",
                                 domain.c_str());
      }
    }
  }

  // Add rewrite domain mappings.
  for (const auto& mapping : domains.rewrite_mappings()) {
    if (!mapping.to_domain().empty() && !mapping.from_domains().empty()) {
      domain_lawyer->AddRewriteDomainMapping(mapping.to_domain(),
                                              mapping.from_domains(),
                                              message_handler);
    }
  }

  // Add origin domain mappings.
  for (const auto& mapping : domains.origin_mappings()) {
    if (!mapping.to_domain().empty() && !mapping.from_domains().empty()) {
      domain_lawyer->AddOriginDomainMapping(
          mapping.to_domain(), mapping.from_domains(), mapping.host_header(),
          message_handler);
    }
  }

  // Add domain shards.
  for (const auto& shard : domains.shards()) {
    if (!shard.domain().empty() && !shard.shards().empty()) {
      domain_lawyer->AddShard(shard.domain(), shard.shards(), message_handler);
    }
  }
}

// Create EnvoyRewriteOptions from VirtualHostConfig proto.
std::unique_ptr<EnvoyRewriteOptions> createVHostOptions(
    const pagespeed::VirtualHostConfig& vhost_config,
    ThreadSystem* thread_system, MessageHandler* message_handler) {
  auto options =
      std::make_unique<EnvoyRewriteOptions>(thread_system);

  const auto& vhost_options = vhost_config.options();

  // Set rewrite level if specified.
  if (!vhost_options.rewrite_level().empty()) {
    RewriteOptions::RewriteLevel level;
    if (RewriteOptions::ParseRewriteLevel(vhost_options.rewrite_level(),
                                           &level)) {
      options->SetRewriteLevel(level);
      message_handler->Message(kInfo, "VHost %s: rewrite_level set to %s",
                               vhost_config.host_pattern().c_str(),
                               vhost_options.rewrite_level().c_str());
    } else {
      message_handler->Message(kWarning,
                               "VHost %s: Invalid rewrite_level: %s",
                               vhost_config.host_pattern().c_str(),
                               vhost_options.rewrite_level().c_str());
    }
  }

  // Note: 'enabled' field is handled separately in the filter by checking
  // if options->enabled(). The default is true. If set to false, we disable
  // all filters by setting level to PassThrough.
  // Actually, RewriteOptions has an 'enabled_' flag we should use instead.
  // For now, we'll set the level to PassThrough if disabled.
  // TODO(oschaaf): Add proper enabled flag support.

  // Enable specific filters.
  if (!vhost_options.enabled_filters().empty()) {
    StringPieceVector filters;
    SplitStringPieceToVector(vhost_options.enabled_filters(), ",", &filters,
                              true);
    for (const auto& filter_name : filters) {
      RewriteOptions::Filter filter =
          RewriteOptions::LookupFilter(filter_name);
      if (filter != RewriteOptions::kEndOfFilters) {
        options->EnableFilter(filter);
        message_handler->Message(kInfo, "VHost %s: Enabled filter: %s",
                                 vhost_config.host_pattern().c_str(),
                                 filter_name.as_string().c_str());
      } else {
        message_handler->Message(kWarning, "VHost %s: Unknown filter: %s",
                                 vhost_config.host_pattern().c_str(),
                                 filter_name.as_string().c_str());
      }
    }
  }

  // Disable specific filters.
  if (!vhost_options.disabled_filters().empty()) {
    StringPieceVector filters;
    SplitStringPieceToVector(vhost_options.disabled_filters(), ",", &filters,
                              true);
    for (const auto& filter_name : filters) {
      RewriteOptions::Filter filter =
          RewriteOptions::LookupFilter(filter_name);
      if (filter != RewriteOptions::kEndOfFilters) {
        options->DisableFilter(filter);
        message_handler->Message(kInfo, "VHost %s: Disabled filter: %s",
                                 vhost_config.host_pattern().c_str(),
                                 filter_name.as_string().c_str());
      } else {
        message_handler->Message(kWarning, "VHost %s: Unknown filter: %s",
                                 vhost_config.host_pattern().c_str(),
                                 filter_name.as_string().c_str());
      }
    }
  }

  // Apply custom options as key-value pairs.
  for (const auto& [key, value] : vhost_options.custom_options()) {
    GoogleString msg;
    RewriteOptions::OptionSettingResult result =
        options->ParseAndSetOptionFromName1(key, value, &msg,
                                            message_handler);
    if (result == RewriteOptions::kOptionOk) {
      message_handler->Message(kInfo, "VHost %s: Set option %s = %s",
                               vhost_config.host_pattern().c_str(),
                               key.c_str(), value.c_str());
    } else {
      message_handler->Message(kWarning, "VHost %s: Failed to set option %s: %s",
                               vhost_config.host_pattern().c_str(),
                               key.c_str(), msg.c_str());
    }
  }

  // Apply domain configuration if present.
  if (vhost_options.has_domains()) {
    applyDomainConfig(vhost_options.domains(), options.get(), message_handler);
  }

  return options;
}

// Initialize VirtualHost configurations from proto.
void initializeVHosts(const pagespeed::Decoder& proto_config,
                      EnvoyServerContext* server_context,
                      ThreadSystem* thread_system,
                      MessageHandler* message_handler) {
  if (proto_config.virtual_hosts_size() == 0) {
    return;
  }

  message_handler->Message(kInfo, "Initializing %d virtual host configurations",
                           proto_config.virtual_hosts_size());

  EnvoyVHostConfigManager* vhost_manager = server_context->vhost_config_manager();

  for (const auto& vhost_config : proto_config.virtual_hosts()) {
    if (vhost_config.host_pattern().empty()) {
      message_handler->Message(kWarning,
                               "Skipping VHost with empty host_pattern");
      continue;
    }

    auto options = createVHostOptions(vhost_config, thread_system,
                                       message_handler);

    message_handler->Message(kInfo, "Adding VHost: pattern='%s', priority=%d",
                             vhost_config.host_pattern().c_str(),
                             vhost_config.priority());

    vhost_manager->AddVHost(vhost_config.host_pattern(), vhost_config.priority(),
                            std::move(options));
  }

  // Sort VHosts by priority for efficient lookup.
  vhost_manager->SortByPriority();

  message_handler->Message(kInfo, "VirtualHost configuration complete: %zu hosts",
                           vhost_manager->size());
}

}  // namespace

EnvoyProcessContext& getProcessContext(const pagespeed::Decoder& proto_config) {
  if (g_process_context == nullptr) {
    EnvoyCacheConfig cache_config = protoToCacheConfig(proto_config);
    g_process_context = new EnvoyProcessContext(cache_config);

    // Initialize VirtualHost configurations.
    initializeVHosts(proto_config, g_process_context->server_context(),
                     g_process_context->driver_factory()->thread_system(),
                     g_process_context->message_handler());

    // Configure circuit breaker for resource fetching reliability.
    EnvoyRewriteDriverFactory* factory = g_process_context->driver_factory();
    if (proto_config.has_circuit_breaker()) {
      const auto& cb = proto_config.circuit_breaker();
      // Default to enabled unless explicitly disabled.
      bool enabled = !proto_config.has_circuit_breaker() || cb.enabled();
      factory->SetCircuitBreakerConfig(enabled, cb.failure_threshold(),
                                        cb.success_threshold(), cb.timeout_ms());
    } else {
      // Circuit breaker disabled by default if not configured.
      factory->SetCircuitBreakerConfig(false, 0, 0, 0);
    }
  }
  return *g_process_context;
}

// Initialize the Envoy dispatcher for native scheduling on first filter config.
// Must be called before creating filters.
void initializeDispatcher(FactoryContext& context,
                          const pagespeed::Decoder& proto_config) {
  auto& process_ctx = getProcessContext(proto_config);
  if (!process_ctx.threads_started()) {
    // Use Envoy's main thread dispatcher for PageSpeed scheduling.
    // This eliminates the need for a separate SchedulerThread.
    process_ctx.InitializeEnvoyDispatcher(
        &context.serverFactoryContext().mainThreadDispatcher());
  }
}

class HttpPageSpeedDecoderFilterConfig : public NamedHttpFilterConfigFactory {
 public:
  absl::StatusOr<Http::FilterFactoryCb> createFilterFactoryFromProto(
      const Protobuf::Message& proto_config, const std::string&,
      FactoryContext& context) override {
    return createFilter(
        Envoy::MessageUtil::downcastAndValidate<const pagespeed::Decoder&>(
            proto_config, context.messageValidationVisitor()),
        context);
  }

  /**
   *  Return the Protobuf Message that represents your config incase you have
   * config proto
   */
  ProtobufTypes::MessagePtr createEmptyConfigProto() override {
    return ProtobufTypes::MessagePtr{new pagespeed::Decoder()};
  }

  std::string name() const override { return "pagespeed"; };

 private:
  Http::FilterFactoryCb createFilter(const pagespeed::Decoder& proto_config,
                                     FactoryContext& context) {
    // Initialize the Envoy dispatcher on first filter config.
    // This enables native event loop integration.
    initializeDispatcher(context, proto_config);

    // Create the filter config with the stats scope for Prometheus metrics.
    Http::HttpPageSpeedDecoderFilterConfigSharedPtr config =
        std::make_shared<Http::HttpPageSpeedDecoderFilterConfig>(
            proto_config, context.scope());

    // Capture proto_config by value for the lambda since getProcessContext
    // needs it to initialize (on first call) or retrieve the singleton.
    return [config, proto_config](
               Http::FilterChainFactoryCallbacks& callbacks) -> void {
      auto filter = new Http::HttpPageSpeedDecoderFilter(
          config, getProcessContext(proto_config).server_context(),
          getProcessContext(proto_config).proxy_fetch_factory());
      callbacks.addStreamFilter(Http::StreamFilterSharedPtr{filter});
    };
  }
};
/**
 * Static registration for this PageSpeed filter. @see RegisterFactory.
 */
static Registry::RegisterFactory<HttpPageSpeedDecoderFilterConfig,
                                 NamedHttpFilterConfigFactory>
    register_;

}  // namespace Configuration
}  // namespace Server
}  // namespace Envoy
