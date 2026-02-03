#include "http_filter.h"

#include <string>

#include "absl/strings/str_format.h"
#include "openssl/crypto.h"  // For CRYPTO_memcmp (timing-safe comparison)

#include "envoy/server/filter_config.h"
#include "pagespeed/system/circuit_breaker.h"
#include "net/instaweb/http/public/async_fetch.h"
#include "net/instaweb/http/public/cache_url_async_fetcher.h"
#include "net/instaweb/http/public/request_context.h"
#include "net/instaweb/public/global_constants.h"
#include "net/instaweb/public/version.h"
#include "net/instaweb/rewriter/public/experiment_matcher.h"
#include "net/instaweb/rewriter/public/experiment_util.h"
#include "net/instaweb/rewriter/public/process_context.h"
#include "net/instaweb/rewriter/public/resource_fetch.h"
#include "net/instaweb/rewriter/public/rewrite_driver.h"
#include "net/instaweb/rewriter/public/rewrite_options.h"
#include "net/instaweb/rewriter/public/rewrite_query.h"
#include "net/instaweb/rewriter/public/rewrite_stats.h"
#include "net/instaweb/rewriter/public/static_asset_manager.h"
#include "net/instaweb/util/public/fallback_property_page.h"
#include "pagespeed/automatic/proxy_fetch.h"
#include "pagespeed/envoy/envoy_admin_fetch.h"
#include "pagespeed/envoy/envoy_async_fetch.h"
#include "pagespeed/envoy/envoy_rewrite_options.h"
#include "pagespeed/kernel/base/google_message_handler.h"
#include "pagespeed/kernel/base/null_message_handler.h"
#include "pagespeed/kernel/base/posix_timer.h"
#include "pagespeed/kernel/base/stack_buffer.h"
#include "pagespeed/kernel/base/stdio_file_system.h"
#include "pagespeed/kernel/base/string.h"
#include "pagespeed/kernel/base/string_writer.h"
#include "pagespeed/kernel/base/time_util.h"
#include "pagespeed/kernel/html/html_keywords.h"
#include "pagespeed/kernel/http/content_type.h"
#include "pagespeed/kernel/http/google_url.h"
#include "pagespeed/kernel/http/query_params.h"
#include "pagespeed/kernel/thread/pthread_shared_mem.h"
#include "pagespeed/kernel/util/gzip_inflater.h"
#include "pagespeed/kernel/util/statistics_logger.h"
#include "pagespeed/system/admin_site.h"
#include "pagespeed/system/in_place_resource_recorder.h"
#include "pagespeed/system/system_caches.h"
#include "pagespeed/system/system_request_context.h"
#include "pagespeed/system/system_rewrite_options.h"
#include "pagespeed/system/system_server_context.h"
#include "pagespeed/system/system_thread_system.h"
#include "net/instaweb/http/public/http_cache.h"
#include "pagespeed/envoy/envoy_rewrite_driver_factory.h"
#include "source/common/network/utility.h"

namespace Envoy {
namespace Http {

// Forward declaration: adds security headers to admin responses.
// Defined later in this file with other security header functions.
void AddAdminSecurityHeadersToResponse(net_instaweb::ResponseHeaders* headers);

HttpPageSpeedDecoderFilterConfig::HttpPageSpeedDecoderFilterConfig(
    const pagespeed::Decoder& proto_config, Envoy::Stats::Scope& scope)
    : key_(proto_config.key()),
      val_(proto_config.val()),
      metrics_collector_(
          std::make_shared<net_instaweb::EnvoyMetricsCollector>(scope)),
      rate_limiter_(std::make_unique<AdminRateLimiter>(100)) {  // Default 100 rpm
  // Parse admin authentication configuration from proto.
  if (proto_config.has_admin_auth()) {
    const auto& auth_config = proto_config.admin_auth();
    admin_auth_.enabled = auth_config.enabled();
    admin_auth_.token = auth_config.token();
    admin_auth_.rate_limit_rpm =
        auth_config.rate_limit_rpm() > 0 ? auth_config.rate_limit_rpm() : 100;

    // Parse CIDR ranges for IP whitelist.
    for (const auto& ip_range : auth_config.allowed_ips()) {
      auto cidr_or = Network::Address::CidrRange::create(ip_range);
      if (cidr_or.ok()) {
        admin_auth_.allowed_ips.push_back(std::move(cidr_or.value()));
      } else {
        // Log warning about invalid CIDR range - this will be visible
        // during Envoy startup.
        ENVOY_LOG_MISC(warn, "Invalid CIDR range in admin_auth.allowed_ips: {}",
                       ip_range);
      }
    }

    // Create rate limiter with configured rate limit.
    rate_limiter_ =
        std::make_unique<AdminRateLimiter>(admin_auth_.rate_limit_rpm);
  }
}

HttpPageSpeedDecoderFilter::HttpPageSpeedDecoderFilter(
    HttpPageSpeedDecoderFilterConfigSharedPtr config,
    net_instaweb::EnvoyServerContext* server_context,
    net_instaweb::ProxyFetchFactory* proxy_fetch_factory)
    : config_(config),
      server_context_(server_context),
      proxy_fetch_factory_(proxy_fetch_factory),
      metrics_collector_(config->metrics_collector()) {}

HttpPageSpeedDecoderFilter::~HttpPageSpeedDecoderFilter() {
  if (rewrite_driver_ != nullptr) {
    rewrite_driver_->Cleanup();
    rewrite_driver_ = nullptr;
  }
  if (recorder_ != nullptr) {
    // Recording was not completed - mark as failed
    recorder_->DoneAndSetHeaders(nullptr, false);
    recorder_ = nullptr;
  }
  if (base_fetch_ != nullptr) {
    // Detach from the base_fetch before destroying this filter.
    // This prevents use-after-free in async callbacks that may still
    // hold a reference to base_fetch_.
    base_fetch_->DetachDecoder();
    base_fetch_->DecrementRefCount();
    base_fetch_ = nullptr;
  }
  // Clean up ProxyFetch-based HTML rewriting.
  if (envoy_async_fetch_ != nullptr) {
    // Mark the filter as destroyed so EnvoyAsyncFetch knows not to
    // access filter APIs in pending callbacks.
    envoy_async_fetch_->MarkFilterDestroyed();
    envoy_async_fetch_.reset();
  }
  // Note: proxy_fetch_ is owned by ProxyFetchFactory and will clean itself up
  // when Done() is called. We don't delete it here.
  proxy_fetch_ = nullptr;
}

void HttpPageSpeedDecoderFilter::onDestroy() {}

const LowerCaseString HttpPageSpeedDecoderFilter::headerKey() const {
  return LowerCaseString(config_->key());
}

const std::string HttpPageSpeedDecoderFilter::headerValue() const {
  return config_->val();
}

// Computes custom options based on VHost config, query params, and headers.
// This follows Apache's three-level hierarchy: global -> VHost -> query/headers.
// Returns true if custom options were created (either from VHost or query/headers).
bool HttpPageSpeedDecoderFilter::ComputeCustomOptions(
    net_instaweb::RequestContextPtr& request_context,
    net_instaweb::RequestHeaders* request_headers) {
  // Initialize stripped_gurl_ with a copy of the pristine URL.
  // GetQueryOptions will modify this to remove PageSpeed query parameters.
  if (pristine_url_ == nullptr || !pristine_url_->IsWebValid()) {
    return false;
  }
  stripped_gurl_ = std::make_unique<net_instaweb::GoogleUrl>(
      pristine_url_->Spec());

  // Get host from URL for VHost lookup.
  GoogleString hostname = pristine_url_->Host().as_string();

  // Look up VHost-specific options. This returns global options merged with
  // VHost options, or just global options if no VHost matches.
  const net_instaweb::RewriteOptions* base_options =
      server_context_->GetOptionsForHost(hostname);

  // Create response headers for query option parsing.
  // GetQueryOptions may store references to headers during parsing.
  query_response_headers_ = std::make_unique<net_instaweb::ResponseHeaders>(
      base_options->ComputeHttpOptions());

  // Parse query params and headers for PageSpeed options.
  // This modifies stripped_gurl_ (removes PS params) and request_headers
  // (removes PS headers), and populates rewrite_query_ with parsed options.
  if (!server_context_->GetQueryOptions(
          request_context, base_options, stripped_gurl_.get(),
          request_headers, query_response_headers_.get(), &rewrite_query_)) {
    // Invalid query params/headers - log warning and use defaults
    server_context_->message_handler()->Message(
        net_instaweb::kWarning,
        "Invalid PageSpeed query params or headers for request %s. "
        "Serving with default options.",
        stripped_gurl_->spec_c_str());
    // Even with invalid query params, we may have VHost options.
    if (base_options != server_context_->global_options()) {
      custom_options_.reset(base_options->Clone());
      return true;
    }
    return false;
  }

  // Check if we got any query options to merge
  const net_instaweb::RewriteOptions* query_options = rewrite_query_.options();
  if (query_options != nullptr) {
    // Create custom options by merging base options (global + VHost) with query options
    custom_options_.reset(base_options->Clone());
    custom_options_->Merge(*query_options);
    return true;
  }

  // No query options, but we may have VHost-specific options.
  if (base_options != server_context_->global_options()) {
    custom_options_.reset(base_options->Clone());
    return true;
  }

  return false;
}

// Check if the path matches an admin endpoint and handle it.
FilterHeadersStatus HttpPageSpeedDecoderFilter::MaybeHandleAdminRequest(
    const RequestHeaderMap& headers, const net_instaweb::GoogleUrl& gurl,
    const net_instaweb::EnvoyRewriteOptions* options) {
  StringPiece path = gurl.PathSansQuery();

  // Parse query params from URL
  net_instaweb::QueryParams query_params;
  query_params.ParseFromUntrustedString(gurl.Query());

  // Determine if this is an admin path that requires authentication.
  bool is_admin_path =
      (path == options->statistics_path()) ||
      (path == options->global_statistics_path()) ||
      (path == options->admin_path()) ||
      (path == options->global_admin_path()) ||
      (path == options->console_path()) ||
      (path == options->messages_path());

  // Health path is intentionally excluded from auth - it's used for
  // load balancer health checks which typically don't support auth.

  // Validate authentication for admin paths.
  if (is_admin_path) {
    AdminAuthResult auth_result = ValidateAdminAuth(headers);
    if (auth_result == AdminAuthResult::kUnauthorized) {
      SendUnauthorizedResponse();
      return FilterHeadersStatus::StopIteration;
    } else if (auth_result == AdminAuthResult::kRateLimited) {
      SendRateLimitedResponse();
      return FilterHeadersStatus::StopIteration;
    }
  }

  // Check statistics path
  if (path == options->statistics_path()) {
    return HandleStatisticsRequest(false /* local */, query_params, options);
  }
  if (path == options->global_statistics_path()) {
    return HandleStatisticsRequest(true /* global */, query_params, options);
  }

  // Check admin path
  if (path == options->admin_path()) {
    return HandleAdminRequest(false /* local */, gurl, query_params, options);
  }
  if (path == options->global_admin_path()) {
    return HandleAdminRequest(true /* global */, gurl, query_params, options);
  }

  // Check console path
  if (path == options->console_path()) {
    return HandleConsoleRequest(query_params, options);
  }

  // Check messages path
  if (path == options->messages_path()) {
    return HandleMessagesRequest(options);
  }

  // Check health path (no auth required for health checks).
  if (path == options->health_path()) {
    return HandleHealthRequest(options);
  }

  // Not an admin path
  return FilterHeadersStatus::Continue;
}

FilterHeadersStatus HttpPageSpeedDecoderFilter::HandleStatisticsRequest(
    bool is_global, const net_instaweb::QueryParams& query_params,
    const net_instaweb::RewriteOptions* options) {
  auto* fetch = new net_instaweb::EnvoyAdminFetch(server_context_, this);
  server_context_->StatisticsPage(is_global, query_params, options, fetch);
  return FilterHeadersStatus::StopIteration;
}

FilterHeadersStatus HttpPageSpeedDecoderFilter::HandleAdminRequest(
    bool is_global, const net_instaweb::GoogleUrl& stripped_gurl,
    const net_instaweb::QueryParams& query_params,
    const net_instaweb::RewriteOptions* options) {
  auto* fetch = new net_instaweb::EnvoyAdminFetch(server_context_, this);
  server_context_->AdminPage(is_global, stripped_gurl, query_params, options,
                             fetch);
  return FilterHeadersStatus::StopIteration;
}

FilterHeadersStatus HttpPageSpeedDecoderFilter::HandleConsoleRequest(
    const net_instaweb::QueryParams& query_params,
    const net_instaweb::SystemRewriteOptions* options) {
  auto* fetch = new net_instaweb::EnvoyAdminFetch(server_context_, this);
  server_context_->ConsoleHandler(*options, net_instaweb::AdminSite::kOther,
                                  query_params, fetch);
  return FilterHeadersStatus::StopIteration;
}

FilterHeadersStatus HttpPageSpeedDecoderFilter::HandleMessagesRequest(
    const net_instaweb::RewriteOptions* options) {
  auto* fetch = new net_instaweb::EnvoyAdminFetch(server_context_, this);
  server_context_->MessageHistoryHandler(*options,
                                         net_instaweb::AdminSite::kOther,
                                         fetch);
  return FilterHeadersStatus::StopIteration;
}

FilterHeadersStatus HttpPageSpeedDecoderFilter::HandleHealthRequest(
    const net_instaweb::EnvoyRewriteOptions* options) {
  // Get uptime from the driver factory.
  net_instaweb::EnvoyRewriteDriverFactory* factory =
      server_context_->envoy_rewrite_driver_factory();
  int64 start_time_ms = factory->start_time_ms();
  int64 now_ms = factory->timer()->NowMs();
  int64 uptime_seconds = (now_ms - start_time_ms) / 1000;

  // Get cache statistics from the HTTP cache.
  net_instaweb::HTTPCache* http_cache = server_context_->http_cache();
  int64 cache_hits = 0;
  int64 cache_misses = 0;
  double hit_rate = 0.0;
  int64 cache_size_bytes = 0;

  if (http_cache != nullptr) {
    cache_hits = http_cache->cache_hits()->Get();
    cache_misses = http_cache->cache_misses()->Get();
    int64 total = cache_hits + cache_misses;
    if (total > 0) {
      hit_rate = static_cast<double>(cache_hits) / static_cast<double>(total);
    }
    // Note: cache_size_bytes is not directly available from HTTPCache.
    // We use cache_inserts as a proxy for cache activity.
    // A more accurate size would require querying the underlying cache backend.
    cache_size_bytes = http_cache->cache_inserts()->Get() * 1024;  // Estimate
  }

  // Build JSON response.
  GoogleString json_body = absl::StrCat(
      "{\n"
      "  \"status\": \"healthy\",\n"
      "  \"version\": \"", net_instaweb::kModPagespeedVersion, "\",\n"
      "  \"uptime_seconds\": ", absl::StrCat(uptime_seconds), ",\n"
      "  \"cache\": {\n"
      "    \"hit_rate\": ", absl::StrFormat("%.2f", hit_rate), ",\n"
      "    \"hits\": ", absl::StrCat(cache_hits), ",\n"
      "    \"misses\": ", absl::StrCat(cache_misses), ",\n"
      "    \"size_bytes\": ", absl::StrCat(cache_size_bytes), "\n"
      "  }\n"
      "}\n");

  // Create response headers with JSON content type.
  auto response_headers = std::make_unique<net_instaweb::ResponseHeaders>();
  response_headers->SetStatusAndReason(net_instaweb::HttpStatus::kOK);
  response_headers->set_major_version(1);
  response_headers->set_minor_version(1);
  response_headers->Add(net_instaweb::HttpAttributes::kContentType,
                        "application/json");
  response_headers->ComputeCaching();

  // Add security headers for admin endpoint.
  AddAdminSecurityHeadersToResponse(response_headers.get());

  sendReply(response_headers.get(), json_body);
  return FilterHeadersStatus::StopIteration;
}

HttpPageSpeedDecoderFilter::AdminAuthResult
HttpPageSpeedDecoderFilter::ValidateAdminAuth(const RequestHeaderMap& headers) {
  // If admin auth is not enabled, allow all requests.
  if (!config_->admin_auth_enabled()) {
    return AdminAuthResult::kAllowed;
  }

  // Get the client's remote address from the stream info.
  const auto& stream_info = decoder_callbacks_->streamInfo();
  const auto& remote_address =
      stream_info.downstreamAddressProvider().remoteAddress();
  std::string client_ip =
      remote_address != nullptr ? remote_address->asString() : "unknown";

  // Check IP whitelist first if configured.
  const auto& allowed_ips = config_->admin_auth_allowed_ips();
  if (!allowed_ips.empty()) {
    bool ip_allowed = false;
    if (remote_address != nullptr) {
      for (const auto& cidr : allowed_ips) {
        if (cidr.isInRange(*remote_address)) {
          ip_allowed = true;
          break;
        }
      }
    }

    if (!ip_allowed) {
      server_context_->message_handler()->Message(
          net_instaweb::kWarning, "Admin request from unauthorized IP: %s",
          client_ip.c_str());
      return AdminAuthResult::kUnauthorized;
    }
  }

  // Check rate limit.
  AdminRateLimiter* rate_limiter = config_->rate_limiter();
  if (rate_limiter != nullptr && !rate_limiter->Allow(client_ip)) {
    server_context_->message_handler()->Message(
        net_instaweb::kWarning, "Admin request rate limit exceeded for IP: %s",
        client_ip.c_str());
    return AdminAuthResult::kRateLimited;
  }

  // Check Bearer token in Authorization header.
  const std::string& expected_token = config_->admin_auth_token();
  if (expected_token.empty()) {
    // No token configured - IP whitelist was sufficient (or not configured).
    return AdminAuthResult::kAllowed;
  }

  auto auth_header = headers.get(LowerCaseString("authorization"));
  if (auth_header.empty()) {
    server_context_->message_handler()->Message(
        net_instaweb::kWarning, "Admin request missing Authorization header");
    return AdminAuthResult::kUnauthorized;
  }

  absl::string_view auth_value = auth_header[0]->value().getStringView();

  // Check for "Bearer " prefix (case-insensitive for "Bearer").
  static const absl::string_view kBearerPrefix = "Bearer ";
  if (!absl::StartsWithIgnoreCase(auth_value, kBearerPrefix)) {
    server_context_->message_handler()->Message(
        net_instaweb::kWarning,
        "Admin request has invalid Authorization header format");
    return AdminAuthResult::kUnauthorized;
  }

  // Extract the token after "Bearer ".
  absl::string_view provided_token = auth_value.substr(kBearerPrefix.size());

  // Use timing-safe comparison to prevent timing attacks.
  // CRYPTO_memcmp returns 0 if the buffers are equal.
  if (provided_token.size() != expected_token.size()) {
    server_context_->message_handler()->Message(
        net_instaweb::kWarning, "Admin request has invalid auth token");
    return AdminAuthResult::kUnauthorized;
  }

  if (CRYPTO_memcmp(provided_token.data(), expected_token.data(),
                    expected_token.size()) != 0) {
    server_context_->message_handler()->Message(
        net_instaweb::kWarning, "Admin request has invalid auth token");
    return AdminAuthResult::kUnauthorized;
  }

  return AdminAuthResult::kAllowed;
}

void HttpPageSpeedDecoderFilter::RecordHtmlRewriteComplete(bool success,
                                                            bool timeout) {
  if (metrics_collector_ && html_rewrite_start_time_ms_ > 0) {
    int64_t latency_ms =
        server_context_->timer()->NowMs() - html_rewrite_start_time_ms_;
    metrics_collector_->RecordHtmlRewriteEnd(latency_ms, success);
    if (timeout) {
      metrics_collector_->RecordHtmlRewriteTimeout();
    }
    html_rewrite_start_time_ms_ = 0;
  }
}

void HttpPageSpeedDecoderFilter::SendUnauthorizedResponse() {
  auto response_headers = std::make_unique<net_instaweb::ResponseHeaders>();
  response_headers->SetStatusAndReason(net_instaweb::HttpStatus::kUnauthorized);
  response_headers->set_major_version(1);
  response_headers->set_minor_version(1);
  response_headers->Add(net_instaweb::HttpAttributes::kContentType,
                        "text/plain");
  // Add WWW-Authenticate header to indicate Bearer token authentication.
  response_headers->Add("WWW-Authenticate",
                        "Bearer realm=\"PageSpeed Admin\"");
  response_headers->ComputeCaching();

  // Add security headers for admin endpoint.
  AddAdminSecurityHeadersToResponse(response_headers.get());

  sendReply(response_headers.get(), "401 Unauthorized\n");
}

void HttpPageSpeedDecoderFilter::SendRateLimitedResponse() {
  auto response_headers = std::make_unique<net_instaweb::ResponseHeaders>();
  // 429 Too Many Requests
  response_headers->SetStatusAndReason(
      static_cast<net_instaweb::HttpStatus::Code>(429));
  response_headers->set_reason_phrase("Too Many Requests");
  response_headers->set_major_version(1);
  response_headers->set_minor_version(1);
  response_headers->Add(net_instaweb::HttpAttributes::kContentType,
                        "text/plain");
  // Retry-After header suggests client wait 60 seconds before retrying.
  response_headers->Add("Retry-After", "60");
  response_headers->ComputeCaching();

  // Add security headers for admin endpoint.
  AddAdminSecurityHeadersToResponse(response_headers.get());

  sendReply(response_headers.get(), "429 Too Many Requests\n");
}

bool HttpPageSpeedDecoderFilter::IsInDegradedMode() const {
  // Check if the circuit breaker is in OPEN state.
  net_instaweb::EnvoyRewriteDriverFactory* factory =
      server_context_->envoy_rewrite_driver_factory();
  if (factory == nullptr) {
    return false;
  }

  net_instaweb::CircuitBreaker* cb = factory->circuit_breaker();
  if (cb == nullptr) {
    return false;
  }

  return cb->GetState() == net_instaweb::CircuitBreaker::State::OPEN;
}

void HttpPageSpeedDecoderFilter::MaybeAddDegradationHeaders(
    net_instaweb::ResponseHeaders* headers) {
  if (headers == nullptr) {
    return;
  }

  if (IsInDegradedMode()) {
    headers->Add("X-PageSpeed-Degraded", "circuit-breaker-open");
    server_context_->message_handler()->Message(
        net_instaweb::kWarning,
        "PageSpeed is in degraded mode (circuit breaker open) for %s",
        pristine_url_ != nullptr ? pristine_url_->spec_c_str() : "unknown");
  }
}

bool HttpPageSpeedDecoderFilter::ShouldTryIpro(
    const net_instaweb::GoogleUrl& url,
    const net_instaweb::RewriteOptions* options) {
  if (options == nullptr || !options->enabled()) {
    return false;
  }

  // Check if IPRO is enabled in options.
  if (!options->in_place_rewriting_enabled()) {
    return false;
  }

  // Check if this URL is allowed by the configuration.
  if (!options->IsAllowed(url.Spec())) {
    return false;
  }

  // Check file extension for IPRO eligibility.
  // IPRO works on images, CSS, and JavaScript - not HTML.
  StringPiece path = url.PathSansQuery();

  // Image extensions
  if (path.ends_with(".png") || path.ends_with(".jpg") ||
      path.ends_with(".jpeg") || path.ends_with(".gif") ||
      path.ends_with(".webp") || path.ends_with(".ico")) {
    return true;
  }

  // CSS and JavaScript
  if (path.ends_with(".css") || path.ends_with(".js")) {
    return true;
  }

  // PDF (if configured)
  if (path.ends_with(".pdf")) {
    return true;
  }

  return false;
}

// decode = client side request
FilterHeadersStatus HttpPageSpeedDecoderFilter::decodeHeaders(
    RequestHeaderMap& headers, bool end_response) {
  RELEASE_ASSERT(base_fetch_ == nullptr, "Base fetch not null");

  // Record request start for metrics.
  if (metrics_collector_) {
    metrics_collector_->RecordRequestStart();
  }

  // Construct URL from Host header and path to preserve the correct port.
  // This is important for sub-resource fetches to work correctly.
  std::string host = "127.0.0.1";
  if (headers.Host() != nullptr) {
    host = std::string(headers.Host()->value().getStringView());
  }
  const std::string url =
      absl::StrCat("http://", host, headers.Path()->value().getStringView());
  pristine_url_ = std::make_unique<net_instaweb::GoogleUrl>(url);

  // Check for admin endpoints first
  auto* envoy_options = net_instaweb::EnvoyRewriteOptions::DynamicCast(
      server_context_->global_options());
  if (envoy_options != nullptr) {
    FilterHeadersStatus admin_status =
        MaybeHandleAdminRequest(headers, *pristine_url_, envoy_options);
    if (admin_status == FilterHeadersStatus::StopIteration) {
      return admin_status;
    }
  }

  // Create request context with actual host from request headers.
  // The NewRequestContext method parses the port from hostname if present.
  net_instaweb::RequestContextPtr request_context(
      server_context_->NewRequestContext(host, 80));

  // Convert Envoy headers to PageSpeed RequestHeaders for query option parsing
  std::unique_ptr<net_instaweb::RequestHeaders> ps_request_headers =
      net_instaweb::HeaderUtils::toPageSpeedRequestHeaders(headers);

  // Start with global options, then potentially merge query/header options
  options_ = server_context_->global_options();

  // Try to compute custom options from query params and headers
  if (ComputeCustomOptions(request_context, ps_request_headers.get())) {
    options_ = custom_options_.get();
  }

  request_context->set_options(options_->ComputeHttpOptions());
  RELEASE_ASSERT(options_ != nullptr, "server context global options not set!");

  // Use stripped URL if available (PageSpeed query params removed),
  // otherwise use the pristine URL
  const net_instaweb::GoogleUrl& fetch_url =
      (stripped_gurl_ != nullptr && stripped_gurl_->IsWebValid())
          ? *stripped_gurl_
          : *pristine_url_;

  base_fetch_ = new net_instaweb::EnvoyBaseFetch(
      fetch_url.Spec(), server_context_, request_context,
      net_instaweb::kDontPreserveHeaders, options_, this);

  // Copy request headers to base_fetch (use already parsed headers)
  for (int i = 0; i < ps_request_headers->NumAttributes(); ++i) {
    base_fetch_->request_headers()->Add(ps_request_headers->Name(i),
                                        ps_request_headers->Value(i));
  }

  // Check if this is a request for a static asset (e.g., js_defer.js,
  // lazyload.js). These are served from /pagespeed_static/ by default.
  const GoogleString& static_prefix =
      server_context_->envoy_rewrite_driver_factory()->static_asset_prefix();
  StringPiece path = fetch_url.PathSansQuery();
  if (path.starts_with(static_prefix)) {
    // Extract file name by stripping the prefix
    StringPiece file_name = path.substr(static_prefix.length());
    StringPiece file_contents;
    StringPiece cache_header;
    net_instaweb::ContentType content_type;

    if (server_context_->static_asset_manager()->GetAsset(
            file_name, &file_contents, &content_type, &cache_header)) {
      // Create response headers for the static asset
      auto response_headers =
          std::make_unique<net_instaweb::ResponseHeaders>();
      response_headers->SetStatusAndReason(net_instaweb::HttpStatus::kOK);
      response_headers->Add(net_instaweb::HttpAttributes::kContentType,
                            content_type.mime_type());
      response_headers->Add(net_instaweb::HttpAttributes::kCacheControl,
                            cache_header.as_string());
      response_headers->ComputeCaching();

      // Send the static asset response
      sendReply(response_headers.get(), file_contents.as_string());
      return FilterHeadersStatus::StopIteration;
    } else {
      // Static asset not found - return 404
      auto response_headers =
          std::make_unique<net_instaweb::ResponseHeaders>();
      response_headers->SetStatusAndReason(net_instaweb::HttpStatus::kNotFound);
      sendReply(response_headers.get(), "Static asset not found");
      return FilterHeadersStatus::StopIteration;
    }
  }

  // Check if this is a PageSpeed-rewritten resource (e.g., combined CSS/JS,
  // optimized images). These URLs have ".pagespeed." in them and must be
  // served from the PageSpeed cache via IPRO, not proxied to upstream.
  if (server_context_->IsPagespeedResource(fetch_url)) {
    // This is a PageSpeed-generated resource (combined CSS/JS, optimized
    // images, etc.). Use ResourceFetch::Start to decode the URL, fetch the
    // original resources, apply optimizations, and return the result.
    // Note: ResourceFetch handles its own driver lifecycle.
    net_instaweb::ResourceFetch::Start(
        fetch_url, custom_options_.release(), server_context_, base_fetch_);
    return FilterHeadersStatus::StopIteration;
  }

  // Check if this URL is eligible for IPRO (In-Place Resource Optimization).
  // For eligible URLs (images, CSS, JS), we first check if an optimized version
  // is already in the cache. If found, we serve it directly. If not found,
  // FetchInPlaceResource will return kNotInCacheStatus and we'll record the
  // response for future optimization in the encode path.
  if (ShouldTryIpro(fetch_url, options_)) {
    // Create a RewriteDriver for the IPRO lookup.
    ipro_driver_ = server_context_->NewRewriteDriver(request_context);

    // Copy request headers to the driver (required by FetchInPlaceResource).
    ipro_driver_->SetRequestHeaders(*base_fetch_->request_headers());

    server_context_->message_handler()->Message(
        net_instaweb::kInfo, "Trying IPRO lookup for %s",
        fetch_url.spec_c_str());

    // FetchInPlaceResource will:
    // 1. Check HTTPCache for an optimized version
    // 2. If found, call base_fetch_->HeadersComplete/Write/Done with the data
    // 3. If not found, call HeadersComplete with kNotInCacheStatus
    //
    // The EnvoyBaseFetch::HandleHeadersComplete() handles both cases:
    // - Cache hit (status 200): Sends optimized response to client
    // - Cache miss (kNotInCacheStatus): Calls prepareForIproRecording()
    //   and continueDecoding() to let the request go to upstream
    ipro_driver_->FetchInPlaceResource(fetch_url, false /* proxy_mode */,
                                       base_fetch_);

    // Stop iteration - we'll handle the response in base_fetch_ callbacks.
    // For cache hits, sendReply() sends the optimized response.
    // For cache misses, continueDecoding() lets the request proceed to upstream.
    return FilterHeadersStatus::StopIteration;
  }

  // For regular requests (HTML, non-IPRO resources), let the request flow
  // through Envoy's proxy path so we can intercept the response in
  // encodeHeaders/encodeData. This allows us to:
  // 1. Rewrite HTML responses via ProxyFetch
  // 2. Record and optimize resources via IPRO recording
  //
  // The rewrite_driver_ will be created lazily in encodeHeaders when
  // we know the Content-Type of the response.

  return FilterHeadersStatus::Continue;
}

FilterDataStatus HttpPageSpeedDecoderFilter::decodeData(Buffer::Instance&,
                                                        bool) {
  return FilterDataStatus::Continue;
}

FilterTrailersStatus HttpPageSpeedDecoderFilter::decodeTrailers(
    RequestTrailerMap&) {
  return FilterTrailersStatus::Continue;
}

void HttpPageSpeedDecoderFilter::setDecoderFilterCallbacks(
    StreamDecoderFilterCallbacks& callbacks) {
  decoder_callbacks_ = &callbacks;
}

void HttpPageSpeedDecoderFilter::prepareForIproRecording() {
  // Use stripped URL (without PageSpeed query params) for consistency with
  // the IPRO lookup. This matches the Apache implementation which uses
  // stripped_gurl_.Spec() for the InPlaceResourceRecorder.
  const net_instaweb::GoogleUrl& recording_url =
      (stripped_gurl_ != nullptr && stripped_gurl_->IsWebValid())
          ? *stripped_gurl_
          : *pristine_url_;

  server_context_->rewrite_stats()->ipro_not_in_cache()->Add(1);
  server_context_->message_handler()->Message(
      net_instaweb::kInfo,
      "Could not rewrite resource in-place "
      "because URL is not in cache: %s",
      recording_url.spec_c_str());

  // Use options_ directly instead of rewrite_driver_->options() because
  // this callback may run on a worker thread, while rewrite_driver_ was
  // created on the main Envoy thread.
  const net_instaweb::SystemRewriteOptions* options =
      net_instaweb::SystemRewriteOptions::DynamicCast(options_);
  if (options == nullptr) {
    server_context_->message_handler()->Message(
        net_instaweb::kWarning,
        "prepareForIproRecording: options not available for %s",
        recording_url.spec_c_str());
    return;
  }

  // Extract host and port from the recording URL for proper request context.
  GoogleString host_str = recording_url.HostAndPort().as_string();
  net_instaweb::RequestContextPtr request_context(
      server_context_->NewRequestContext(host_str, 80));
  request_context->set_options(options->ComputeHttpOptions());

  // Get cache fragment from server context's global options if rewrite_driver_
  // is not available on this thread.
  GoogleString cache_fragment;
  if (rewrite_driver_ != nullptr) {
    cache_fragment = rewrite_driver_->CacheFragment();
  } else {
    // Use the hostname as cache fragment (matches typical behavior)
    cache_fragment = recording_url.Host().as_string();
  }

  // This URL was not found in cache (neither the input resource nor
  // a ResourceNotCacheable entry) so we need to get it into cache
  // (or at least a note that it cannot be cached stored there).
  recorder_ = new net_instaweb::InPlaceResourceRecorder(
      request_context, recording_url.Spec(),
      cache_fragment,
      base_fetch_->request_headers()->GetProperties(),
      options->ipro_max_response_bytes(),
      options->ipro_max_concurrent_recordings(), server_context_->http_cache(),
      server_context_->statistics(), &message_handler_);
}

// Adds security headers to HTTP responses for defense in depth.
// These headers protect against common web vulnerabilities:
// - X-Frame-Options: Prevents clickjacking attacks
// - X-Content-Type-Options: Prevents MIME type sniffing
// - X-XSS-Protection: Legacy XSS protection for older browsers
// - Content-Security-Policy: Restricts resource loading
//
// Note: Cache-Control is NOT set here because user-facing cached resources
// (IPRO cache hits) should keep their cache headers set by PageSpeed.
// For admin endpoints, use AddAdminSecurityHeaders() instead.
void AddSecurityHeaders(Http::HeaderMap& headers) {
  headers.addCopy(LowerCaseString("x-frame-options"), "SAMEORIGIN");
  headers.addCopy(LowerCaseString("x-content-type-options"), "nosniff");
  headers.addCopy(LowerCaseString("x-xss-protection"), "1; mode=block");
  headers.addCopy(LowerCaseString("content-security-policy"),
                  "default-src 'self'; script-src 'self' 'unsafe-inline'");
}

// Adds security headers for admin/internal endpoints that should not be cached.
// This includes health checks, statistics, auth failures, etc.
// Note: This function operates on Envoy HeaderMap - use
// AddAdminSecurityHeadersToResponse() for PageSpeed ResponseHeaders.
void AddAdminSecurityHeaders(Http::HeaderMap& headers) {
  AddSecurityHeaders(headers);
  headers.addCopy(LowerCaseString("cache-control"),
                  "private, no-store, no-cache, must-revalidate");
}

// Adds security headers to PageSpeed ResponseHeaders for admin endpoints.
// This should be called BEFORE sendReply() for admin responses only.
// User-facing content (HTML rewrites, IPRO cache hits) should NOT call this.
void AddAdminSecurityHeadersToResponse(net_instaweb::ResponseHeaders* headers) {
  headers->Add("X-Frame-Options", "SAMEORIGIN");
  headers->Add("X-Content-Type-Options", "nosniff");
  headers->Add("X-XSS-Protection", "1; mode=block");
  headers->Add("Content-Security-Policy",
               "default-src 'self'; script-src 'self' 'unsafe-inline'");
  // Set restrictive caching for admin endpoints by replacing any existing
  // Cache-Control header.
  headers->Replace(net_instaweb::HttpAttributes::kCacheControl,
                   "private, no-store, no-cache, must-revalidate");
}

void HttpPageSpeedDecoderFilter::sendReply(
    net_instaweb::ResponseHeaders* response_headers, std::string body) {
  CHECK(response_headers != nullptr);

  // Add degradation headers if the system is in degraded mode.
  MaybeAddDegradationHeaders(response_headers);

  // When called from the encode path (HTML rewriting), we need to:
  // 1. Modify the stored response headers in place
  // 2. Add the rewritten body via addEncodedData
  // 3. Continue encoding to send the response

  if (stored_response_headers_ != nullptr) {
    // Modify the stored Envoy headers in place.
    // First, clear existing headers and copy from PageSpeed headers.
    stored_response_headers_->setStatus(response_headers->status_code());

    // Remove all existing headers except :status, then add PageSpeed headers.
    // This is needed because PageSpeed may have modified headers.
    std::vector<LowerCaseString> to_remove;
    stored_response_headers_->iterate(
        [&to_remove](const HeaderEntry& entry) -> HeaderMap::Iterate {
          if (entry.key().getStringView() != ":status") {
            to_remove.push_back(LowerCaseString(std::string(entry.key().getStringView())));
          }
          return HeaderMap::Iterate::Continue;
        });
    for (const auto& key : to_remove) {
      stored_response_headers_->remove(key);
    }

    // Copy PageSpeed headers to Envoy headers.
    for (uint32_t i = 0, n = response_headers->NumAttributes(); i < n; ++i) {
      const GoogleString& name = response_headers->Name(i);
      const GoogleString& value = response_headers->Value(i);
      // Skip status header (already set above).
      if (name != ":status") {
        stored_response_headers_->addCopy(LowerCaseString(name), value);
      }
    }

    // Note: Security headers are NOT added for HTML rewrites (user-facing content).
    // Only admin endpoints should have restrictive security headers.

    // Update content-length for the rewritten body.
    stored_response_headers_->setContentLength(body.size());

    // Add body via encoder callbacks and continue encoding.
    // The body will be sent with end_stream=true.
    if (!body.empty()) {
      Buffer::OwnedImpl buffer(body);
      encoder_callbacks_->addEncodedData(buffer, false /* streaming_filter */);
    }

    // Continue encoding to send the modified response.
    encoder_callbacks_->continueEncoding();
    stored_response_headers_ = nullptr;
  } else {
    // Fallback: use decoder sendLocalReply (for non-HTML paths).
    // This handles IPRO cache hits, admin endpoints, errors, etc.
    // IMPORTANT: Security headers are NOT added here because this path is also
    // used by IPRO cache hits which should NOT have restrictive cache headers.
    // Admin endpoints that need security headers should add them to their
    // ResponseHeaders before calling sendReply() (see AddAdminSecurityHeaders).
    std::function<void(Http::HeaderMap&)> modify_headers =
        [response_headers](Http::HeaderMap& envoy_headers) {
          for (uint32_t i = 0, n = response_headers->NumAttributes(); i < n;
               ++i) {
            const GoogleString& name = response_headers->Name(i);
            const GoogleString& value = response_headers->Value(i);
            auto lcase_key = Envoy::Http::LowerCaseString(name);
            envoy_headers.remove(lcase_key);
            envoy_headers.addCopy(lcase_key, value);
          }
          // Note: No security headers added here - admin endpoints add their own
          // via AddAdminSecurityHeadersToResponse() before calling sendReply().
        };
    decoder_callbacks_->sendLocalReply(
        static_cast<Envoy::Http::Code>(response_headers->status_code()), body,
        modify_headers, absl::nullopt, "details");
  }
}

FilterHeadersStatus HttpPageSpeedDecoderFilter::encodeHeaders(
    ResponseHeaderMap& headers, bool end_stream) {
  // TODO(oschaaf): Add X-Page-Speed header to indicate PageSpeed is active.
  // Currently disabled due to crashes during local reply processing.
  // The header should be added for proxied responses when options_ is set.
  // See issue tracking this: adding X-Page-Speed header crashes during
  // sendLocalReply for admin endpoints.

  // Convert headers to PageSpeed format for processing.
  auto ps_response_headers =
      net_instaweb::HeaderUtils::toPageSpeedResponseHeaders(headers);

  // Check if this is an HTML response that we should rewrite.
  // Only rewrite HTML if:
  // 1. Options are set (not an admin/local response)
  // 2. PageSpeed is enabled (not PageSpeed=off)
  // 3. HTML rewriting is enabled in options
  // This matches Apache behavior where HTML is rewritten by default.
  // IPRO requests go through a separate path (recorder_ handling below).
  if (options_ == nullptr) {
    // No options set - this is likely an admin response or local error.
    return FilterHeadersStatus::Continue;
  }
  auto* envoy_options = net_instaweb::EnvoyRewriteOptions::DynamicCast(options_);
  bool pagespeed_enabled = options_->enabled();
  bool html_rewriting_enabled =
      envoy_options != nullptr && envoy_options->enable_html_rewriting();

  // HTML rewriting activates by default when PageSpeed is enabled and HTML
  // rewriting is enabled (matching Apache behavior). IPRO requests go through
  // a different path (recorder_ is set when IPRO recording is active).
  if (pagespeed_enabled && html_rewriting_enabled) {
    // Check Content-Type to see if this is HTML.
    const net_instaweb::ContentType* content_type =
        ps_response_headers->DetermineContentType();
    bool is_html = content_type != nullptr && content_type->IsHtmlLike();

    // Only rewrite successful HTML responses.
    int status_code = ps_response_headers->status_code();
    bool is_success = status_code >= 200 && status_code < 300;

    if (is_html && is_success && !end_stream) {
      // This is an HTML response - set up HTML rewriting via ProxyFetch.
      // Cancel any IPRO recording that may have been started, since we're
      // going to rewrite the HTML instead.
      if (recorder_ != nullptr) {
        recorder_->Fail();
        recorder_ = nullptr;
      }

      server_context_->message_handler()->Message(
          net_instaweb::kInfo, "Starting HTML rewriting for %s",
          pristine_url_->spec_c_str());

      // Check if we have a ProxyFetchFactory.
      if (proxy_fetch_factory_ == nullptr) {
        server_context_->message_handler()->Message(
            net_instaweb::kWarning,
            "No ProxyFetchFactory available for HTML rewriting");
        return FilterHeadersStatus::Continue;
      }

      // Create request context for HTML rewriting.
      // Use the actual port from the URL for correct resource fetching.
      int port = pristine_url_->EffectiveIntPort();
      net_instaweb::RequestContextPtr request_context(
          server_context_->NewRequestContext(
              pristine_url_->Host().as_string(), port));
      request_context->set_options(options_->ComputeHttpOptions());

      // Create EnvoyAsyncFetch to receive ProxyFetch output.
      envoy_async_fetch_ = std::make_shared<net_instaweb::EnvoyAsyncFetch>(
          request_context, this, encoder_callbacks_->dispatcher(),
          server_context_);

      // Copy request headers to the async fetch (required by ProxyFetch).
      if (decoder_callbacks_ != nullptr) {
        auto envoy_request_headers = decoder_callbacks_->requestHeaders();
        if (envoy_request_headers.has_value()) {
          auto ps_request_headers =
              net_instaweb::HeaderUtils::toPageSpeedRequestHeaders(
                  envoy_request_headers.ref());
          for (int i = 0; i < ps_request_headers->NumAttributes(); ++i) {
            envoy_async_fetch_->request_headers()->Add(
                ps_request_headers->Name(i), ps_request_headers->Value(i));
          }
        }
      }

      // Copy origin response headers to the async fetch.
      for (int i = 0; i < ps_response_headers->NumAttributes(); ++i) {
        envoy_async_fetch_->response_headers()->Add(
            ps_response_headers->Name(i), ps_response_headers->Value(i));
      }
      envoy_async_fetch_->response_headers()->set_status_code(
          ps_response_headers->status_code());
      // ComputeCaching() must be called after modifying headers, before
      // ProxyFetch accesses caching-related fields like date_ms().
      envoy_async_fetch_->response_headers()->ComputeCaching();

      // Get a RewriteDriver for HTML rewriting.
      // Use NewCustomRewriteDriver when there are custom options from query params
      // or VHost config. This ensures ApplySessionFetchers (which may create
      // LoopbackRouteFetcher) sees the correct options - not options that would
      // be replaced later, causing use-after-free in the session fetcher.
      net_instaweb::RewriteDriver* driver;
      if (custom_options_ != nullptr) {
        driver = server_context_->NewCustomRewriteDriver(
            custom_options_.release(), request_context);
      } else {
        driver = server_context_->NewRewriteDriver(request_context);
      }

      // Set request headers on the driver (required by ProxyFetch).
      driver->SetRequestHeaders(*envoy_async_fetch_->request_headers());

      // Initiate property cache lookup (for critical CSS, etc.)
      // Note: InitiatePropertyCacheLookup needs non-const options, but the
      // options are only used for reading configuration - the const_cast is
      // safe here as the function doesn't modify them.
      net_instaweb::RewriteOptions* mutable_options =
          const_cast<net_instaweb::RewriteOptions*>(driver->options());
      net_instaweb::ProxyFetchPropertyCallbackCollector* property_callback =
          net_instaweb::ProxyFetchFactory::InitiatePropertyCacheLookup(
              false /* is_resource_fetch */,
              *pristine_url_,
              server_context_,
              mutable_options,
              envoy_async_fetch_.get());

      // Create ProxyFetch to handle HTML parsing and rewriting.
      // This does NOT start a fetch - we feed data via HeadersComplete/Write/Done.
      proxy_fetch_ = proxy_fetch_factory_->CreateNewProxyFetch(
          pristine_url_->Spec().as_string(),
          envoy_async_fetch_.get(),
          driver,
          property_callback,
          nullptr /* original_content_fetch */);

      if (proxy_fetch_ == nullptr) {
        server_context_->message_handler()->Message(
            net_instaweb::kWarning,
            "Failed to create ProxyFetch for %s",
            pristine_url_->spec_c_str());
        envoy_async_fetch_.reset();
        return FilterHeadersStatus::Continue;
      }

      // Mark this as trusted internal HTML since we're rewriting responses
      // from our upstream server. This prevents ProxyFetch from returning 403
      // when it sees HTML content and ProxiesHtml() returns false.
      proxy_fetch_->set_trusted_input(true);

      // Start HTML rewriting - this creates a self-reference that keeps
      // envoy_async_fetch_ alive until HandleDone() is called. This prevents
      // use-after-free if the filter is destroyed while worker threads are
      // still processing HTML.
      envoy_async_fetch_->StartHtmlRewriting();

      // Signal that headers are complete. Note: We don't need to copy headers
      // again here because ProxyFetch shares the same response_headers object
      // as envoy_async_fetch_ (via SharedAsyncFetch). Headers were already
      // copied to envoy_async_fetch_->response_headers() above.
      proxy_fetch_->HeadersComplete();

      // HandleHeadersComplete() may have modified headers (e.g., domain rewrite,
      // redirect handling, cookie operations). Ensure cache fields are computed
      // before Write() is called, which may access caching-related fields.
      envoy_async_fetch_->response_headers()->ComputeCaching();

      buffering_html_ = true;

      // Store reference to original response headers for modification later.
      stored_response_headers_ = &headers;

      // Record HTML rewrite start for metrics.
      if (metrics_collector_) {
        metrics_collector_->RecordHtmlRewriteStart();
        html_rewrite_start_time_ms_ =
            server_context_->timer()->NowMs();
      }

      // Stop iteration - we'll buffer the body and send our own response.
      return FilterHeadersStatus::StopIteration;
    }
  }

  // Not rewriting HTML - handle IPRO recording if active.
  if (recorder_) {
    response_headers_ = std::move(ps_response_headers);
    recorder_->ConsiderResponseHeaders(
        net_instaweb::InPlaceResourceRecorder::kPreliminaryHeaders,
        response_headers_.get());

    // If end_stream is true, there's no body - complete the recording now.
    if (end_stream) {
      recorder_->DoneAndSetHeaders(response_headers_.get(), true);
      recorder_ = nullptr;
    }
  }

  return FilterHeadersStatus::Continue;
}

FilterDataStatus HttpPageSpeedDecoderFilter::encodeData(Buffer::Instance& data,
                                                        bool end_stream) {
  // Handle ProxyFetch-based HTML rewriting if active.
  if (buffering_html_ && proxy_fetch_ != nullptr) {
    // Feed data to ProxyFetch.
    std::string data_str = data.toString();
    if (!data_str.empty()) {
      proxy_fetch_->Write(
          StringPiece(data_str.data(), data_str.size()),
          server_context_->message_handler());
    }

    // Consume the data - ProxyFetch is buffering it.
    data.drain(data.length());

    if (end_stream) {
      // All data received - tell ProxyFetch we're done.
      // ProxyFetch will parse HTML, rewrite it, and call
      // EnvoyAsyncFetch::HandleDone() which posts sendReply to dispatcher.
      proxy_fetch_->Done(true);
      proxy_fetch_ = nullptr;  // ProxyFetch deletes itself after Done
      buffering_html_ = false;
      return FilterDataStatus::StopIterationNoBuffer;
    }

    return FilterDataStatus::StopIterationAndBuffer;
  }

  // Handle IPRO recording if active.
  if (recorder_ != nullptr) {
    recorder_->Write(data.toString(), recorder_->handler());
    if (end_stream) {
      // Before completing the recording, save the original Cache-Control header
      // if we're going to modify it with s-maxage. This is critical for
      // preserving no-cache and other directives from the origin response.
      // See mod_instaweb.cc lines 804-819 for Apache's implementation.
      const net_instaweb::SystemRewriteOptions* sys_options =
          net_instaweb::SystemRewriteOptions::DynamicCast(options_);
      if (sys_options != nullptr && response_headers_ != nullptr) {
        int s_maxage_sec = sys_options->EffectiveInPlaceSMaxAgeSec();
        if (s_maxage_sec != -1) {
          const char* existing_cache_control =
              response_headers_->Lookup1(net_instaweb::HttpAttributes::kCacheControl);
          GoogleString updated_cache_control;
          if (net_instaweb::ResponseHeaders::ApplySMaxAge(
                  s_maxage_sec, existing_cache_control, &updated_cache_control)) {
            // We're modifying the cache control header; save the original first.
            // This preserves directives like no-cache in the cached copy.
            recorder_->SaveCacheControl(existing_cache_control);
          }
        }
      }

      recorder_->DoneAndSetHeaders(response_headers_.get(), true);
      recorder_ = nullptr;
    }
  }

  return FilterDataStatus::Continue;
}

}  // namespace Http
}  // namespace Envoy
