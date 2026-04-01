#pragma once

#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "envoy/server/filter_config.h"
#include "net/instaweb/rewriter/public/rewrite_driver.h"
#include "net/instaweb/rewriter/public/rewrite_query.h"
#include "net/instaweb/rewriter/public/rewrite_stats.h"
#include "pagespeed/automatic/proxy_fetch.h"
#include "pagespeed/envoy/envoy_async_fetch.h"
#include "pagespeed/envoy/envoy_base_fetch.h"
#include "pagespeed/envoy/envoy_metrics_collector.h"
#include "pagespeed/envoy/envoy_rewrite_options.h"
#include "pagespeed/envoy/envoy_server_context.h"
#include "pagespeed/envoy/header_utils.h"
#include "pagespeed/envoy/http_filter.pb.h"
#include "pagespeed/kernel/http/query_params.h"
#include "pagespeed/system/in_place_resource_recorder.h"
#include "pagespeed/system/system_request_context.h"
#include "pagespeed/system/system_rewrite_options.h"
#include "source/common/network/cidr_range.h"

namespace Envoy {
namespace Http {

// Token bucket rate limiter for admin endpoint protection.
// Thread-safe implementation using per-IP buckets with sharded mutexes
// to reduce contention under concurrent Envoy worker threads.
class AdminRateLimiter {
 public:
  explicit AdminRateLimiter(int32_t max_requests_per_minute)
      : max_requests_per_minute_(max_requests_per_minute) {}

  // Returns true if the request from this IP should be allowed.
  // Consumes a token if allowed.
  bool Allow(const std::string& client_ip) {
    if (max_requests_per_minute_ <= 0) {
      return true;  // Rate limiting disabled
    }

    // Shard by IP hash to reduce mutex contention across Envoy threads.
    size_t shard = std::hash<std::string>{}(client_ip) % kNumShards;
    std::lock_guard<std::mutex> lock(shard_mutexes_[shard]);

    auto now = std::chrono::steady_clock::now();
    auto& bucket = shard_buckets_[shard][client_ip];

    // Check if we need to refill the bucket (every minute).
    auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                          now - bucket.last_refill)
                          .count();
    if (elapsed_ms >= 60000 || bucket.tokens == -1) {
      // First request or minute elapsed - refill bucket.
      bucket.tokens = max_requests_per_minute_;
      bucket.last_refill = now;
    }

    if (bucket.tokens <= 0) {
      return false;  // Rate limit exceeded
    }

    bucket.tokens--;
    return true;
  }

 private:
  struct TokenBucket {
    int32_t tokens = -1;  // -1 indicates uninitialized
    std::chrono::steady_clock::time_point last_refill;
  };

  static constexpr size_t kNumShards = 16;
  int32_t max_requests_per_minute_;
  std::mutex shard_mutexes_[kNumShards];
  std::unordered_map<std::string, TokenBucket> shard_buckets_[kNumShards];
};

// Admin authentication configuration parsed from proto.
struct AdminAuthConfig {
  bool enabled = false;
  std::string token;
  std::vector<Network::Address::CidrRange> allowed_ips;
  int32_t rate_limit_rpm = 100;
};

class HttpPageSpeedDecoderFilterConfig {
 public:
  HttpPageSpeedDecoderFilterConfig(const pagespeed::Decoder& proto_config,
                                   Envoy::Stats::Scope& scope);

  const std::string& key() const { return key_; }
  const std::string& val() const { return val_; }

  // Admin authentication accessors.
  bool admin_auth_enabled() const { return admin_auth_.enabled; }
  const std::string& admin_auth_token() const { return admin_auth_.token; }
  const std::vector<Network::Address::CidrRange>& admin_auth_allowed_ips()
      const {
    return admin_auth_.allowed_ips;
  }
  int32_t admin_auth_rate_limit_rpm() const {
    return admin_auth_.rate_limit_rpm;
  }

  // Metrics collector accessor.
  net_instaweb::EnvoyMetricsCollectorSharedPtr metrics_collector() const {
    return metrics_collector_;
  }

  // Rate limiter accessor - returns mutable pointer since it tracks state.
  AdminRateLimiter* rate_limiter() { return rate_limiter_.get(); }

 private:
  const std::string key_;
  const std::string val_;
  AdminAuthConfig admin_auth_;
  net_instaweb::EnvoyMetricsCollectorSharedPtr metrics_collector_;
  std::unique_ptr<AdminRateLimiter> rate_limiter_;
};

using HttpPageSpeedDecoderFilterConfigSharedPtr =
    std::shared_ptr<HttpPageSpeedDecoderFilterConfig>;

class HttpPageSpeedDecoderFilter : public StreamFilter {
 public:
  HttpPageSpeedDecoderFilter(HttpPageSpeedDecoderFilterConfigSharedPtr,
                             net_instaweb::EnvoyServerContext*,
                             net_instaweb::ProxyFetchFactory*);
  ~HttpPageSpeedDecoderFilter() override;

  // Http::StreamFilterBase
  void onDestroy() override;

  // Http::StreamDecoderFilter
  FilterHeadersStatus decodeHeaders(RequestHeaderMap&, bool) override;
  FilterDataStatus decodeData(Buffer::Instance&, bool) override;
  FilterTrailersStatus decodeTrailers(RequestTrailerMap&) override;
  void setDecoderFilterCallbacks(StreamDecoderFilterCallbacks&) override;

  // Http::StreamEncoderFilter
  Filter1xxHeadersStatus encode1xxHeaders(ResponseHeaderMap& headers) override {
    return Filter1xxHeadersStatus::Continue;
  };

  FilterHeadersStatus encodeHeaders(ResponseHeaderMap& headers,
                                    bool end_stream) override;

  FilterDataStatus encodeData(Buffer::Instance& data, bool end_stream) override;

  FilterTrailersStatus encodeTrailers(ResponseTrailerMap& trailers) override {
    return FilterTrailersStatus::Continue;
  };
  FilterMetadataStatus encodeMetadata(MetadataMap& metadata_map) override {
    return FilterMetadataStatus::Continue;
  };

  void setEncoderFilterCallbacks(
      StreamEncoderFilterCallbacks& callbacks) override {
    encoder_callbacks_ = &callbacks;
  };
  void encodeComplete() override {}

  // HttpPageSpeedDecoderFilter
  void prepareForIproRecording();
  void sendReply(net_instaweb::ResponseHeaders* response_headers,
                 std::string body);

  // Called by EnvoyHtmlRewriter when HTML rewriting completes.
  // Records metrics for the rewrite operation.
  void RecordHtmlRewriteComplete(bool success, bool timeout);

  StreamDecoderFilterCallbacks* decoderCallbacks() {
    return decoder_callbacks_;
  };
  StreamEncoderFilterCallbacks* encoderCallbacks() {
    return encoder_callbacks_;
  };

 private:
  // Computes custom options based on query params and headers.
  // Returns true if custom options were found and merged.
  bool ComputeCustomOptions(net_instaweb::RequestContextPtr& request_context,
                            net_instaweb::RequestHeaders* request_headers);

  // Check if the path matches an admin endpoint and handle it.
  // Returns StopIteration if handled, Continue if not an admin path.
  // If auth is required and fails, returns StopIteration with 401 response.
  FilterHeadersStatus MaybeHandleAdminRequest(
      const RequestHeaderMap& headers, const net_instaweb::GoogleUrl& gurl,
      const net_instaweb::EnvoyRewriteOptions* options, bool end_stream);

  // Handler methods for admin endpoints
  FilterHeadersStatus HandleStatisticsRequest(
      bool is_global, const net_instaweb::QueryParams& query_params,
      const net_instaweb::RewriteOptions* options);
  FilterHeadersStatus HandleAdminRequest(
      bool is_global, const net_instaweb::GoogleUrl& stripped_gurl,
      const net_instaweb::QueryParams& query_params,
      const net_instaweb::RewriteOptions* options,
      StringPiece request_body = StringPiece());
  FilterHeadersStatus HandleConsoleRequest(
      const net_instaweb::QueryParams& query_params,
      const net_instaweb::SystemRewriteOptions* options);
  FilterHeadersStatus HandleMessagesRequest(
      const net_instaweb::RewriteOptions* options);
  FilterHeadersStatus HandleHealthRequest(
      const net_instaweb::EnvoyRewriteOptions* options);

  // Result of admin authentication validation.
  enum class AdminAuthResult {
    kAllowed,       // Request is allowed
    kUnauthorized,  // Authentication failed (401)
    kRateLimited    // Rate limit exceeded (429)
  };

  // Validate admin authentication for the current request.
  // Returns kAllowed if authentication succeeds or is not required.
  // Returns kUnauthorized or kRateLimited on failure.
  AdminAuthResult ValidateAdminAuth(const RequestHeaderMap& headers);

  // Send a 401 Unauthorized response.
  void SendUnauthorizedResponse();

  // Send a 429 Too Many Requests response.
  void SendRateLimitedResponse();

  // Check if the system is in degraded mode (circuit breaker open).
  bool IsInDegradedMode() const;

  // Add degradation headers to the response if in degraded mode.
  void MaybeAddDegradationHeaders(net_instaweb::ResponseHeaders* headers);

  // Check if IPRO (In-Place Resource Optimization) should be tried for this URL.
  // Returns true if the URL is eligible for IPRO lookup (images, CSS, JS).
  bool ShouldTryIpro(const net_instaweb::GoogleUrl& url,
                     const net_instaweb::RewriteOptions* options);

  const HttpPageSpeedDecoderFilterConfigSharedPtr config_;
  net_instaweb::EnvoyServerContext* server_context_{nullptr};
  StreamDecoderFilterCallbacks* decoder_callbacks_;
  StreamEncoderFilterCallbacks* encoder_callbacks_;

  const LowerCaseString headerKey() const;
  const std::string headerValue() const;
  net_instaweb::EnvoyBaseFetch* base_fetch_{nullptr};
  const net_instaweb::RewriteOptions* options_{nullptr};
  net_instaweb::RewriteDriver* rewrite_driver_{nullptr};
  net_instaweb::InPlaceResourceRecorder* recorder_{nullptr};
  net_instaweb::GoogleMessageHandler message_handler_;
  std::unique_ptr<net_instaweb::ResponseHeaders> response_headers_;
  std::unique_ptr<net_instaweb::GoogleUrl> pristine_url_;

  // Query parameter and header-based options support
  net_instaweb::RewriteQuery rewrite_query_;
  std::unique_ptr<net_instaweb::RewriteOptions> custom_options_;
  std::unique_ptr<net_instaweb::GoogleUrl> stripped_gurl_;
  // Response headers used for query option parsing - must be a member
  // because GetQueryOptions may store references to it.
  std::unique_ptr<net_instaweb::ResponseHeaders> query_response_headers_;

  // HTML rewriting support via ProxyFetch.
  // True if we're buffering an HTML response for rewriting.
  bool buffering_html_{false};
  // EnvoyAsyncFetch receives output from ProxyFetch and sends to client.
  net_instaweb::EnvoyAsyncFetchPtr envoy_async_fetch_;
  // ProxyFetch handles HTML parsing and rewriting on worker threads.
  net_instaweb::ProxyFetch* proxy_fetch_{nullptr};
  // Factory for creating ProxyFetch instances.
  net_instaweb::ProxyFetchFactory* proxy_fetch_factory_{nullptr};
  // Stored response headers from encodeHeaders for modification after rewriting.
  ResponseHeaderMap* stored_response_headers_{nullptr};

  // Metrics collector for Prometheus metrics.
  net_instaweb::EnvoyMetricsCollectorSharedPtr metrics_collector_;

  // Timestamp for HTML rewrite latency tracking.
  int64_t html_rewrite_start_time_ms_{0};

  // Pending admin POST request state: when a POST arrives at an admin endpoint,
  // we need to buffer the body before dispatching to HandleAdminRequest.
  bool pending_admin_dispatch_{false};
  bool pending_admin_is_global_{false};
  std::string pending_admin_body_;

  // Driver used for IPRO lookup. Owned by this filter until IPRO completes.
  net_instaweb::RewriteDriver* ipro_driver_{nullptr};
};

}  // namespace Http
}  // namespace Envoy
