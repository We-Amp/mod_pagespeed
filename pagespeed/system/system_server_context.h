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

#ifndef PAGESPEED_SYSTEM_SYSTEM_SERVER_CONTEXT_H_
#define PAGESPEED_SYSTEM_SYSTEM_SERVER_CONTEXT_H_

#include <atomic>
#include <memory>
#include <mutex>

#include "net/instaweb/http/public/request_context.h"
#include "net/instaweb/rewriter/public/server_context.h"
#include "pagespeed/kernel/base/abstract_mutex.h"
#include "pagespeed/kernel/base/basictypes.h"
#include "pagespeed/kernel/base/string.h"
#include "pagespeed/kernel/base/string_util.h"
#include "pagespeed/kernel/base/writer.h"
#include "pagespeed/kernel/util/copy_on_write.h"
#include "pagespeed/system/admin_site.h"

namespace net_instaweb {

class AsyncFetch;
class GoogleUrl;
class Histogram;
class QueryParams;
class PurgeSet;
class RewriteDriver;
class RewriteDriverFactory;
class RewriteOptions;
class RewriteStats;
class SharedMemStatistics;
class Statistics;
class SystemCachePath;
class SystemCaches;
class SystemRewriteDriverFactory;
class SystemRewriteOptions;
class UpDownCounter;
class UrlAsyncFetcherStats;
class Variable;

// A server context with features specific to a PSOL port on a unix system.
class SystemServerContext : public ServerContext {
 public:
  // Identifies whether the user arrived at an admin page from a
  // /pagespeed_admin handler or a /*_pagespeed_statistics handler.
  // The main difference between these is that the _admin site might in the
  // future grant more privileges than the statistics site did, such as flushing
  // cache.  But it also affects the syntax of the links created to sub-pages
  // in the top navigation bar.

  SystemServerContext(RewriteDriverFactory* factory, StringPiece hostname,
                      int port);
  ~SystemServerContext() override;

  void SetCachePath(SystemCachePath* cache_path);

  // Implementations should call this method on every request, both for html and
  // resources, to avoid serving stale resources.
  //
  // TODO(jmarantz): allow a URL-based mechanism to flush cache, even if
  // we implement it by simply writing the cache.flush file so other
  // servers can see it.  Note that using shared-memory is not a great
  // plan because we need the cache-invalidation to persist across server
  // restart.
  void FlushCacheIfNecessary();

  SystemRewriteOptions* global_system_rewrite_options();
  GoogleString hostname_identifier() { return hostname_identifier_; }

  // Updates the PurgeSet with a new version.  This is called when the system
  // picks up (by polling or API) a new version of the cache.purge file.
  void UpdateCachePurgeSet(const CopyOnWrite<PurgeSet>& purge_set);

  // Initialize this SystemServerContext to set up its admin site.
  void PostInitHook() override;

  // SystemServerContext doesn't proxy HTML by default. Subclasses like
  // ApacheServerContext override this to return true when appropriate.
  bool ProxiesHtml() const override { return false; }

  static void InitStats(Statistics* statistics);

  // Called by SystemRewriteDriverFactory::ChildInit.  See documentation there.
  virtual void ChildInit(SystemRewriteDriverFactory* factory);

  // Initialize this ServerContext to have its own statistics domain.  Must be
  // called after global_statistics has been created and had ::InitStats called
  // on it.
  void CreateLocalStatistics(Statistics* global_statistics,
                             SystemRewriteDriverFactory* factory);

  // Whether ChildInit() has been called yet.  Exposed so debugging code can
  // verify initialization proceeded properly.
  bool initialized() const { return initialized_; }

  // Normally we just fetch with the default UrlAsyncFetcher, generally curl,
  // but there are some cases where we need to do something more complex:
  //  - Local requests: requests for resources on this host should go directly
  //    to the local IP.
  //  - Custom fetch headers: before continuing with the fetch we want to add
  //    request headers.
  // Session fetchers allow us to make these decisions.  Here we may update
  // driver->async_fetcher() to be a special fetcher just for this request.
  void ApplySessionFetchers(const RequestContextPtr& req,
                            RewriteDriver* driver) override;

  // Accumulate in a histogram the amount of time spent rewriting HTML.
  // TODO(sligocki): Remove in favor of RewriteStats::rewrite_latency_histogram.
  void AddHtmlRewriteTimeUs(int64 rewrite_time_us);

  SystemCachePath* cache_path() { return cache_path_; }

  // Hook called after all configuration parsing is done to support implementers
  // like ApacheServerContext that need to collapse configuration inside the
  // config overlays into actual RewriteOptions objects.  It will also compute
  // signatures when done, and by default that's the only thing it does.
  virtual void CollapseConfigOverlaysAndComputeSignatures();

  // Handler which serves PSOL console.
  // Note: ConsoleHandler always succeeds.
  void ConsoleHandler(const SystemRewriteOptions& options,
                      AdminSite::AdminSource source,
                      const QueryParams& query_params, AsyncFetch* fetch);

  // Displays recent Info/Warning/Error messages.
  void MessageHistoryHandler(const RewriteOptions& options,
                             AdminSite::AdminSource source, AsyncFetch* fetch);

  // Handle a request for /pagespeed_admin/*, which is a launching
  // point for all the administrator pages including stats,
  // message-histogram, console, etc.
  void AdminPage(bool is_global, const GoogleUrl& stripped_gurl,
                 const QueryParams& query_params, const RewriteOptions* options,
                 AsyncFetch* fetch, StringPiece request_body = StringPiece());

  // Handle a request for the legacy /*_pagespeed_statistics page, which also
  // serves as a launching point for a subset of the admin pages.  Because the
  // admin pages are not uniformly sensitive, an existing PageSpeed user might
  // have granted public access to /mod_pagespeed_statistics, but we don't
  // want that to automatically imply access to the server cache.
  void StatisticsPage(bool is_global, const QueryParams& query_params,
                      const RewriteOptions* options, AsyncFetch* fetch);

  AdminSite* admin_site() { return admin_site_.get(); }

  // Returns true if the license is active and optimization should proceed.
  // Lock-free (uses atomic bool) — safe to call on every request hot path.
  //
  // the design record: under soft enforcement this is now a pure SIGNAL only — callers
  // no longer gate core optimization on it; they emit the soft warn header
  // instead. The agent_optimize entitlement (IsAgentOptimizeEntitled) stays
  // hard-gated and is unaffected.
  bool ShouldOptimize() const {
    return license_active_.load(std::memory_order_relaxed);
  }

  // Update the license_active_ flag. Called by the license handler after
  // state changes (init, apply, renewal).
  void UpdateLicenseActive(bool active) {
    license_active_.store(active, std::memory_order_relaxed);
  }

  // the design record (R5, cold-start suppression): true once the first license
  // check/poll has completed. The soft "x-pagespeed-warn: unlicensed" header
  // is emitted iff (LicenseCheckedOnce() && !ShouldOptimize()), so it stays
  // suppressed until we have actually checked the license at least once.
  // Lock-free — safe on the request hot path. Default true (fail-open) so
  // paid installs never flash the warning during respawn / app-pool recycle.
  bool LicenseCheckedOnce() const {
    return license_checked_once_.load(std::memory_order_relaxed);
  }

  // Mark that the first license check/poll has completed. Called from
  // PostInitHook right after the initial UpdateLicenseActive() seed.
  void MarkLicenseChecked() {
    license_checked_once_.store(true, std::memory_order_relaxed);
  }

  // the design record: true only when the active license grants the agent_optimize
  // entitlement (license-valid AND the entitlement claim). Lock-free — safe on
  // the request hot path. Default false (opt-in), unlike license_active_.
  // Overrides ServerContext::IsAgentOptimizeEntitled (the base returns false).
  bool IsAgentOptimizeEntitled() const override {
    return agent_optimize_entitled_.load(std::memory_order_relaxed);
  }

  // Update the agent_optimize entitlement flag. Called by the license handler
  // on the same state-change path as UpdateLicenseActive (init, apply, renewal).
  void UpdateAgentOptimizeEntitled(bool entitled) {
    agent_optimize_entitled_.store(entitled, std::memory_order_relaxed);
  }

  // the design record: true once this install has been observed optimizing a host that is
  // OUTSIDE the licensed site of an active scope=="site" license (over-cap).
  // Lock-free — safe to read on the request hot path and from the status JSON.
  // A SOFT, display/telemetry-only signal: it NEVER gates optimization, never
  // feeds ShouldOptimize()/license_active_. Once latched it
  // stays set until the next license change (UpdateOverCapPolicy resets it).
  // Acquire-load: pairs with the release-stores in MaybeFlagOverCap (latch) and
  // UpdateOverCapPolicy (reset) so lock-free readers (the per-port warn header
  // and the status JSON getter) observe a latch/reset on weak-memory CPUs (ARM)
  // without taking over_cap_mutex_.
  bool IsOverCap() const { return over_cap_.load(std::memory_order_acquire); }

  // Per-request over-cap detection, called from each port's HTML-transform path
  // with the request Host. Cheap and idempotent: short-circuits in two relaxed
  // atomic loads for the common cases (already latched; or no active site-scope
  // policy — unlicensed / community / org / host / scopeless / no domain), and
  // only takes over_cap_mutex_ for an actively site-licensed install that has
  // not yet latched. Sets the latch + logs a rate-limited WARNING the first
  // time an external off-license host is seen. Never returns a value or alters
  // optimization — purely sets the display/telemetry flag.
  void MaybeFlagOverCap(StringPiece hostname);

  // Publish the over-cap policy from the applied/renewed license. |active| is
  // the live license validity; |scope|/|domain| are the design record token fields
  // (empty on legacy scopeless tokens). site-scope is armed ONLY for an active
  // scope=="site" license with a non-empty domain; every other case disarms
  // detection. Resets the latch so a re-pointed license is evaluated afresh.
  // Called on the same state-change path as UpdateLicenseActive (init, apply,
  // renewal).
  void UpdateOverCapPolicy(bool active, StringPiece scope, StringPiece domain);

 protected:
  // Flush the cache by updating the cache flush timestamp in the global
  // options.  This will change its signature, which is part of the cache key,
  // and so all previously cached entries will be unreachable.
  //
  // Returns true if it actually updated the timestamp, false if the existing
  // cache flush timestamp was newer or the same as the one provided.
  //
  // Subclasses which add additional configurations need to override this method
  // to additionally update the cache flush timestamp in those other
  // configurations.
  virtual bool UpdateCacheFlushTimestampMs(int64 timestamp_ms);

  // Returns JSON used by the PageSpeed Console JavaScript.
  void ConsoleJsonHandler(const QueryParams& params, AsyncFetch* fetch);

  // Handler for /mod_pagespeed_statistics and
  // /ngx_pagespeed_statistics, as well as
  // /...pagespeed__global_statistics.  If the latter,
  // is_global_request should be true.
  void StatisticsHandler(const RewriteOptions& options, bool is_global_request,
                         AdminSite::AdminSource source, AsyncFetch* fetch);

  // Print details for configuration.
  void PrintConfig(AdminSite::AdminSource source, AsyncFetch* fetch);

  // Print statistics about the caches.  In the future this will also
  // be a launching point for examining cache entries and purging them.
  void PrintCaches(bool is_global, AdminSite::AdminSource source,
                   const GoogleUrl& stripped_gurl,
                   const QueryParams& query_params,
                   const RewriteOptions* options, AsyncFetch* fetch);

  // Print histograms showing the dynamics of server activity.
  void PrintHistograms(bool is_global_request, AdminSite::AdminSource source,
                       AsyncFetch* fetch);

  Variable* statistics_404_count();

 private:
  // Checks the timestamp of cache.flush, updating the purge context as
  // needed.
  //
  // TODO(jmarantz): Consider removing this if we decide to turn
  // enable_cache_purge always-on.  If we do that, "touch cache.flush"
  // will no longer work, and that will force users to use the new purge API
  // instead.  We could consider checking both cache.flush and cache.purge,
  // but it would be confusing what our semantics should be if both are
  // present.
  void CheckLegacyGlobalCacheFlushFile();

  std::unique_ptr<AdminSite> admin_site_;

  // Lock-free license enforcement flag. Updated by AdminLicenseHandler,
  // checked by per-port request handlers on every request.
  std::atomic<bool> license_active_{true};  // Default true until Init
  // the design record (R5): true once the first license check/poll has completed.
  // Default true (fail-open) so paid installs never flash the soft warn header
  // during respawn / app-pool recycle; PostInitHook re-affirms it after the
  // initial seed. Separate atomic — never overloads license_active_.
  std::atomic<bool> license_checked_once_{true};
  // the design record: default false (opt-in) — the agent_optimize entitlement must be
  // explicitly granted by an applied license token.
  std::atomic<bool> agent_optimize_entitled_{false};

  // the design record over-cap state. over_cap_ is the latched display/telemetry flag
  // (read lock-free via IsOverCap / the status JSON). over_cap_site_scoped_ is
  // the cheap hot-path gate so non-site installs (the common case) exit
  // MaybeFlagOverCap without taking over_cap_mutex_. The licensed registrable
  // domain (lowercased) is guarded by over_cap_mutex_, which also serializes
  // the rare detect-transition (latch set) with UpdateOverCapPolicy (publish +
  // latch reset) — MaybeFlagOverCap re-checks the gate + domain under the lock
  // before latching so a racing license change can never latch a false
  // positive (the guard 2.0's adversarial review hardened). Default disarmed.
  std::atomic<bool> over_cap_{false};
  std::atomic<bool> over_cap_site_scoped_{false};
  std::atomic<int64_t> last_over_cap_warn_us_{0};
  GoogleString over_cap_domain_;  // guarded by over_cap_mutex_
  std::mutex over_cap_mutex_;

  bool initialized_;
  bool use_per_vhost_statistics_;

  // State used to implement periodic polling of $FILE_PREFIX/cache.flush.
  // last_cache_flush_check_sec_ is ctor-initialized to 0 so the first
  // time we Poll we will read the file.
  std::unique_ptr<AbstractMutex> cache_flush_mutex_;
  int64 last_cache_flush_check_sec_;  // seconds since 1970

  Variable* cache_flush_count_;
  UpDownCounter* cache_flush_timestamp_ms_;

  Histogram* html_rewrite_time_us_histogram_;

  // Non-NULL if we have per-vhost stats.
  std::unique_ptr<Statistics> split_statistics_;

  // May be NULL. Owned by *split_statistics_.
  SharedMemStatistics* local_statistics_;

  // These are non-NULL if we have per-vhost stats.
  std::unique_ptr<RewriteStats> local_rewrite_stats_;
  std::unique_ptr<UrlAsyncFetcherStats> stats_fetcher_;

  // hostname_identifier_ equals to "server_hostname:port" of the server.  It's
  // used to distinguish the name of shared memory so that each vhost has its
  // own SharedCircularBuffer.
  GoogleString hostname_identifier_;

  SystemCaches* system_caches_;

  SystemCachePath* cache_path_;

  SystemServerContext(const SystemServerContext&) = delete;
  SystemServerContext& operator=(const SystemServerContext&) = delete;
};

}  // namespace net_instaweb

#endif  // PAGESPEED_SYSTEM_SYSTEM_SERVER_CONTEXT_H_
