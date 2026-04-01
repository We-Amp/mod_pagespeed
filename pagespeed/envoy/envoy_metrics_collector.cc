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

#include "pagespeed/envoy/envoy_metrics_collector.h"

namespace net_instaweb {

namespace {
// Stats prefix for all PageSpeed metrics.
constexpr char kStatsPrefix[] = "pagespeed.";
}  // namespace

EnvoyMetricsCollector::EnvoyMetricsCollector(Envoy::Stats::Scope& scope)
    : stats_(generateStats(kStatsPrefix, scope)) {}

PageSpeedStats EnvoyMetricsCollector::generateStats(
    const std::string& prefix, Envoy::Stats::Scope& scope) {
  return PageSpeedStats{PAGESPEED_STATS(POOL_COUNTER_PREFIX(scope, prefix),
                                        POOL_GAUGE_PREFIX(scope, prefix),
                                        POOL_HISTOGRAM_PREFIX(scope, prefix))};
}

void EnvoyMetricsCollector::RecordRequestStart() {
  stats_.requests_total_.inc();
}

void EnvoyMetricsCollector::RecordHtmlRewriteStart() {
  stats_.active_html_rewrites_.inc();
}

void EnvoyMetricsCollector::RecordHtmlRewriteEnd(int64_t latency_ms,
                                                 bool success) {
  stats_.active_html_rewrites_.dec();
  stats_.rewrite_latency_ms_.recordValue(static_cast<uint64_t>(latency_ms));
  if (success) {
    stats_.html_rewrites_total_.inc();
  } else {
    stats_.html_rewrites_failed_.inc();
  }
}

void EnvoyMetricsCollector::RecordHtmlRewriteTimeout() {
  stats_.html_rewrites_timeout_.inc();
}

void EnvoyMetricsCollector::RecordCacheHit(bool hit) {
  if (hit) {
    stats_.ipro_cache_hits_.inc();
  } else {
    stats_.ipro_cache_misses_.inc();
  }
}

void EnvoyMetricsCollector::RecordIproResult(bool served, bool in_cache,
                                             bool rewritable) {
  // Record if a resource was successfully served via IPRO.
  if (served) {
    stats_.ipro_served_total_.inc();
  }
  // Note: Cache hit/miss recording should be done via RecordCacheHit()
  // to avoid double-counting. This method only records served/rewritable.
  if (!rewritable) {
    stats_.ipro_not_rewritable_.inc();
  }
}

}  // namespace net_instaweb
