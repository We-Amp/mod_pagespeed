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

// EnvoyMetricsCollector provides Prometheus metrics integration for the
// PageSpeed Envoy filter. It wraps Envoy's Stats::Scope to record PageSpeed
// performance metrics that are exported via Envoy's /stats endpoint.
//
// Metrics exported:
// - pagespeed.requests_total: Total requests processed by PageSpeed filter
// - pagespeed.html_rewrites_total: Successful HTML rewrites
// - pagespeed.html_rewrites_failed: Failed HTML rewrites (timeout, error)
// - pagespeed.ipro_cache_hits: IPRO cache hits
// - pagespeed.ipro_cache_misses: IPRO cache misses
// - pagespeed.ipro_served_total: Resources served via IPRO
// - pagespeed.ipro_not_rewritable: Resources not rewritable by IPRO
// - pagespeed.rewrite_latency_ms: HTML rewrite latency histogram
// - pagespeed.active_html_rewrites: Currently active HTML rewrites (gauge)
//
// Usage:
//   auto collector = std::make_shared<EnvoyMetricsCollector>(scope);
//   collector->RecordRequestStart();
//   // ... process request ...
//   collector->RecordHtmlRewriteEnd(latency_ms, success);

#ifndef PAGESPEED_ENVOY_ENVOY_METRICS_COLLECTOR_H_
#define PAGESPEED_ENVOY_ENVOY_METRICS_COLLECTOR_H_

#include <cstdint>
#include <memory>
#include <string>

#include "envoy/stats/histogram.h"
#include "envoy/stats/scope.h"
#include "envoy/stats/stats.h"
#include "envoy/stats/stats_macros.h"

namespace net_instaweb {

/**
 * PageSpeed filter stats. @see stats_macros.h
 */
#define PAGESPEED_STATS(COUNTER, GAUGE, HISTOGRAM) \
  COUNTER(requests_total)                          \
  COUNTER(html_rewrites_total)                     \
  COUNTER(html_rewrites_failed)                    \
  COUNTER(html_rewrites_timeout)                   \
  COUNTER(ipro_cache_hits)                         \
  COUNTER(ipro_cache_misses)                       \
  COUNTER(ipro_served_total)                       \
  COUNTER(ipro_not_rewritable)                     \
  GAUGE(active_html_rewrites, NeverImport)         \
  HISTOGRAM(rewrite_latency_ms, Milliseconds)

/**
 * Struct definition for PageSpeed stats. @see stats_macros.h
 */
struct PageSpeedStats {
  PAGESPEED_STATS(GENERATE_COUNTER_STRUCT, GENERATE_GAUGE_STRUCT,
                  GENERATE_HISTOGRAM_STRUCT)
};

/**
 * EnvoyMetricsCollector wraps Envoy's Stats::Scope and provides methods to
 * record PageSpeed metrics. All metrics are prefixed with "pagespeed." and
 * are exported via Envoy's standard /stats and /stats/prometheus endpoints.
 *
 * Thread Safety:
 * - Counter/Gauge operations are atomic and thread-safe.
 * - Histogram recordValue is thread-safe.
 * - This class can be shared across filter instances.
 */
class EnvoyMetricsCollector {
 public:
  /**
   * Creates a metrics collector with stats in the given scope.
   * @param scope The stats scope to create metrics in. Metrics will be
   *              prefixed with "pagespeed." under this scope.
   */
  explicit EnvoyMetricsCollector(Envoy::Stats::Scope& scope);

  /**
   * Records that a request has started processing by the PageSpeed filter.
   * Increments requests_total counter.
   */
  void RecordRequestStart();

  /**
   * Records that an HTML rewrite operation has completed.
   * Updates html_rewrites_total or html_rewrites_failed counter based on
   * success, records latency in the histogram, and decrements
   * active_html_rewrites gauge.
   *
   * @param latency_ms Time taken for the HTML rewrite in milliseconds.
   * @param success True if rewrite completed successfully, false otherwise.
   */
  void RecordHtmlRewriteEnd(int64_t latency_ms, bool success);

  /**
   * Records that an HTML rewrite operation has started.
   * Increments active_html_rewrites gauge.
   */
  void RecordHtmlRewriteStart();

  /**
   * Records that an HTML rewrite timed out.
   * Increments html_rewrites_timeout counter.
   */
  void RecordHtmlRewriteTimeout();

  /**
   * Records a cache lookup result for IPRO.
   * @param hit True if the resource was found in cache.
   */
  void RecordCacheHit(bool hit);

  /**
   * Records the result of an IPRO (In-Place Resource Optimization) request.
   * Note: Use RecordCacheHit() separately to record cache hit/miss.
   *
   * @param served True if a resource was served (either from cache or origin).
   * @param in_cache Unused - use RecordCacheHit() instead to avoid double counting.
   * @param rewritable True if the resource type is rewritable by PageSpeed.
   */
  void RecordIproResult(bool served, bool in_cache, bool rewritable);

  /**
   * Returns the current stats for testing/inspection.
   */
  const PageSpeedStats& stats() const { return stats_; }

 private:
  /**
   * Generates the PageSpeedStats struct from the given scope.
   */
  static PageSpeedStats generateStats(const std::string& prefix,
                                      Envoy::Stats::Scope& scope);

  PageSpeedStats stats_;
};

using EnvoyMetricsCollectorSharedPtr = std::shared_ptr<EnvoyMetricsCollector>;

}  // namespace net_instaweb

#endif  // PAGESPEED_ENVOY_ENVOY_METRICS_COLLECTOR_H_
