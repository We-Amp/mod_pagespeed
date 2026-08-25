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

#include "pagespeed/system/system_server_context.h"

#include <algorithm>
#include <cctype>
#include <memory>
#include <mutex>
#include <string>

#include "base/logging.h"
#include "net/instaweb/http/public/url_async_fetcher.h"
#include "net/instaweb/http/public/url_async_fetcher_stats.h"
#include "net/instaweb/rewriter/public/rewrite_driver.h"
#include "net/instaweb/rewriter/public/rewrite_driver_factory.h"
#include "net/instaweb/rewriter/public/rewrite_options.h"
#include "net/instaweb/rewriter/public/rewrite_stats.h"
#include "pagespeed/kernel/base/abstract_mutex.h"
#include "pagespeed/kernel/base/file_system.h"
#include "pagespeed/kernel/base/message_handler.h"
#include "pagespeed/kernel/base/null_message_handler.h"
#include "pagespeed/kernel/base/split_statistics.h"
#include "pagespeed/kernel/base/statistics.h"
#include "pagespeed/kernel/base/string.h"
#include "pagespeed/kernel/base/thread_system.h"
#include "pagespeed/kernel/base/timer.h"
#include "pagespeed/kernel/http/google_url.h"
#include "pagespeed/kernel/sharedmem/shared_mem_statistics.h"
#include "pagespeed/kernel/util/statistics_logger.h"
#include "pagespeed/system/add_headers_fetcher.h"
#include "pagespeed/system/admin_license_handler.h"
#include "pagespeed/system/loopback_route_fetcher.h"
#include "pagespeed/system/over_cap_match.h"
#include "pagespeed/system/system_cache_path.h"
#include "pagespeed/system/system_caches.h"
#include "pagespeed/system/system_request_context.h"
#include "pagespeed/system/system_rewrite_driver_factory.h"
#include "pagespeed/system/system_rewrite_options.h"

namespace net_instaweb {

class PurgeSet;

namespace {

const char kHtmlRewriteTimeUsHistogram[] = "Html Time us Histogram";
const char kLocalFetcherStatsPrefix[] = "http";
const char kCacheFlushCount[] = "cache_flush_count";
const char kCacheFlushTimestampMs[] = "cache_flush_timestamp_ms";
const char kStatistics404Count[] = "statistics_404_count";

}  // namespace

SystemServerContext::SystemServerContext(RewriteDriverFactory* factory,
                                         StringPiece hostname, int port)
    : ServerContext(factory),
      initialized_(false),
      use_per_vhost_statistics_(false),
      cache_flush_mutex_(thread_system()->NewMutex()),
      last_cache_flush_check_sec_(0),
      cache_flush_count_(nullptr),         // Lazy-initialized under mutex.
      cache_flush_timestamp_ms_(nullptr),  // Lazy-initialized under mutex.
      html_rewrite_time_us_histogram_(nullptr),
      local_statistics_(nullptr),
      hostname_identifier_(StrCat(hostname, ":", IntegerToString(port))),
      system_caches_(nullptr),
      cache_path_(nullptr) {
  global_system_rewrite_options()->set_description(hostname_identifier_);
}

SystemServerContext::~SystemServerContext() {
  if (cache_path_ != nullptr) {
    cache_path_->RemoveServerContext(this);
  }
}

void SystemServerContext::SetCachePath(SystemCachePath* cache_path) {
  DCHECK(cache_path_ == nullptr);
  cache_path_ = cache_path;
  cache_path->AddServerContext(this);
}

// If we haven't checked the timestamp of $FILE_PREFIX/cache.flush in the past
// cache_flush_poll_interval_sec_ seconds do so, and if the timestamp has
// expired then update the cache_invalidation_timestamp in global_options,
// thus flushing the cache.
void SystemServerContext::FlushCacheIfNecessary() {
  if (global_system_rewrite_options()->enable_cache_purge()) {
    cache_path_->FlushCacheIfNecessary();
  } else {
    CheckLegacyGlobalCacheFlushFile();
  }

  // Advance the statistics log on a wall-clock cadence that does not depend on
  // this request having driven an HTML rewrite or a pagespeed-resource fetch.
  // Those are the only paths that otherwise call UpdateAndDumpIfRequired(), so
  // under pass-through/static/cached traffic the log never grows and the
  // /pagespeed_admin Graphs page flatlines even though the live Statistics
  // counters keep moving. This hook runs on every request across all ports
  // (nginx, Apache, IIS); UpdateAndDumpIfRequired() is internally throttled to
  // StatisticsLoggingIntervalMs via a non-blocking TryLock on the logger's own
  // timestamp mutex, so on all but ~one request per interval it is a cheap
  // no-op and can neither block nor contend with the cache-flush mutex above.
  StatisticsLogger* stats_logger = statistics()->console_logger();
  if (stats_logger != nullptr) {
    stats_logger->UpdateAndDumpIfRequired();
  }
}

void SystemServerContext::CheckLegacyGlobalCacheFlushFile() {
  int64 cache_flush_poll_interval_sec =
      global_system_rewrite_options()->cache_flush_poll_interval_sec();
  if (cache_flush_poll_interval_sec > 0) {
    int64 now_sec = timer()->NowMs() / Timer::kSecondMs;
    bool check_cache_file = false;
    {
      ScopedMutex lock(cache_flush_mutex_.get());
      if (now_sec >=
          (last_cache_flush_check_sec_ + cache_flush_poll_interval_sec)) {
        last_cache_flush_check_sec_ = now_sec;
        check_cache_file = true;
      }
      if (cache_flush_count_ == nullptr) {
        cache_flush_count_ = statistics()->GetVariable(kCacheFlushCount);
      }
      if (cache_flush_timestamp_ms_ == nullptr) {
        cache_flush_timestamp_ms_ =
            statistics()->GetUpDownCounter(kCacheFlushTimestampMs);
      }
    }

    if (check_cache_file) {
      GoogleString cache_flush_filename =
          global_system_rewrite_options()->cache_flush_filename();
      if (cache_flush_filename.empty()) {
        cache_flush_filename = "cache.flush";
      }
      if (cache_flush_filename[0] != '/'
#ifdef _WIN32
          &&
          !(cache_flush_filename.size() >= 3 &&
            cache_flush_filename[1] == ':' &&
            (cache_flush_filename[2] == '\\' || cache_flush_filename[2] == '/'))
#endif
      ) {
        // cache_flush_filename is relative — prepend file_cache_path.
        // The path must be absolute and at least one subdirectory deep
        // (not empty or filesystem root).
        const GoogleString& fcp =
            global_system_rewrite_options()->file_cache_path();
#ifdef _WIN32
        // Windows absolute: "X:\subdir" — drive letter + colon + sep + name.
        DCHECK(fcp.size() >= 4 && fcp[1] == ':' &&
               (fcp[2] == '\\' || fcp[2] == '/'))
            << "file_cache_path must be an absolute path at least one "
               "subdirectory deep, got: "
            << fcp;
        cache_flush_filename = StrCat(fcp, "/", cache_flush_filename);
#else
        // Unix absolute: "/subdir" — starts with / and has more after it.
        DCHECK(fcp.size() >= 2 && fcp[0] == '/')
            << "file_cache_path must be an absolute path at least one "
               "subdirectory deep, got: "
            << fcp;
        cache_flush_filename = StrCat(fcp, "/", cache_flush_filename);
#endif
      }
      int64 cache_flush_timestamp_sec;
      NullMessageHandler null_handler;
      if (file_system()->Mtime(cache_flush_filename, &cache_flush_timestamp_sec,
                               &null_handler)) {
        int64 timestamp_ms = cache_flush_timestamp_sec * Timer::kSecondMs;

        bool flushed = UpdateCacheFlushTimestampMs(timestamp_ms);

        // The multiple child processes each must independently
        // discover a fresh cache.flush and update the options. However,
        // as shown in
        //     http://github.com/apache/incubator-pagespeed-mod/issues/568
        // we should only bump the flush-count and print a warning to
        // the log once per new timestamp.
        if (flushed && (timestamp_ms !=
                        cache_flush_timestamp_ms_->SetReturningPreviousValue(
                            timestamp_ms))) {
          int count = cache_flush_count_->Add(1);
          message_handler()->Message(kWarning, "Cache Flush %d", count);
        }
      }
    } else {
      // Check on every request whether another child process has updated the
      // statistic.
      int64 timestamp_ms = cache_flush_timestamp_ms_->Get();

      // Do the difference-check first because that involves only a
      // reader-lock, so we have zero contention risk when the cache is not
      // being flushed.
      if ((timestamp_ms > 0) &&
          global_options()->has_cache_invalidation_timestamp_ms() &&
          (global_options()->cache_invalidation_timestamp() < timestamp_ms)) {
        UpdateCacheFlushTimestampMs(timestamp_ms);
      }
    }
  }
}

void SystemServerContext::UpdateCachePurgeSet(
    const CopyOnWrite<PurgeSet>& purge_set) {
  global_options()->UpdateCachePurgeSet(purge_set);
  if (cache_flush_count_ == nullptr) {
    cache_flush_count_ = statistics()->GetVariable(kCacheFlushCount);
  }
  cache_flush_count_->Add(1);
}

bool SystemServerContext::UpdateCacheFlushTimestampMs(int64 timestamp_ms) {
  return global_options()->UpdateCacheInvalidationTimestampMs(timestamp_ms);
}

void SystemServerContext::AddHtmlRewriteTimeUs(int64 rewrite_time_us) {
  if (html_rewrite_time_us_histogram_ != nullptr) {
    html_rewrite_time_us_histogram_->Add(rewrite_time_us);
  }
}

SystemRewriteOptions* SystemServerContext::global_system_rewrite_options() {
  SystemRewriteOptions* out =
      dynamic_cast<SystemRewriteOptions*>(global_options());
  CHECK(out != nullptr);
  return out;
}

void SystemServerContext::PostInitHook() {
  ServerContext::PostInitHook();
  if (is_decoding_stub()) {
    // The stub decoding context exists only to back the shared decoding
    // driver: it runs on default options (no FileCachePath) and never
    // serves requests, so admin/license setup here would always come up
    // unlicensed and emit a spurious per-process UNLICENSED warning on
    // licensed installs.
    return;
  }
  admin_site_ = std::make_unique<AdminSite>(
      timer(), thread_system(), message_handler(), DefaultSystemFetcher(),
      global_system_rewrite_options()->file_cache_path());
  // Set initial license state from what Init() loaded from disk,
  // and register a callback to keep it updated.
  if (admin_site_->license_handler() != nullptr) {
    UpdateLicenseActive(admin_site_->license_handler()->IsLicenseValid());
    // the design record (R5): the first license check has now completed — from here on
    // the soft warn header is allowed to emit when unlicensed. Re-affirms the
    // fail-open default so the predicate is deterministic.
    MarkLicenseChecked();
    // the design record: seed + track the agent_optimize entitlement on the same path.
    UpdateAgentOptimizeEntitled(
        admin_site_->license_handler()->IsAgentOptimizeEntitled());
    // the design record: seed + track the over-cap policy on the same state-change path.
    // The callback re-reads the (just-updated) scope/domain from the handler,
    // so a single source of truth (the license handler) feeds both the gate
    // and the over-cap policy.
    UpdateOverCapPolicy(admin_site_->license_handler()->IsLicenseValid(),
                        admin_site_->license_handler()->license_scope(),
                        admin_site_->license_handler()->license_domain());
    admin_site_->license_handler()->set_license_state_callback(
        [this](bool active, bool agent_optimize) {
          UpdateLicenseActive(active);
          UpdateAgentOptimizeEntitled(agent_optimize);
          UpdateOverCapPolicy(active,
                              admin_site_->license_handler()->license_scope(),
                              admin_site_->license_handler()->license_domain());
        });
    // the design record: the status JSON reads the over-cap flag LIVE from this context
    // (single source of truth) rather than mirroring it into the handler.
    admin_site_->license_handler()->set_over_cap_getter(
        [this]() { return IsOverCap(); });
    // the design record (D4): one-time startup WARNING when running unlicensed at init.
    // Emitted here (the single license-check site) to avoid double-logging.
    if (!admin_site_->license_handler()->IsLicenseValid()) {
      message_handler()->Message(
          kWarning,
          "WeAmp PageSpeed is running UNLICENSED. Optimization is active; "
          "responses carry X-PageSpeed-Warn: unlicensed. Activate a license "
          "to remove the warning and unlock support + premium features.");
    }
  }
}

void SystemServerContext::UpdateOverCapPolicy(bool active, StringPiece scope,
                                              StringPiece domain) {
  // the design record: arm over-cap detection ONLY for an active scope=="site" license
  // with a non-empty domain. Every other case (unlicensed, community, org,
  // host, scopeless legacy, or a site token without a domain) disarms it.
  const bool site_scoped = active && scope == "site" && !domain.empty();
  GoogleString lowered;
  domain.CopyToString(&lowered);
  LowerString(&lowered);  // hot-path compare is case-stable
  {
    std::lock_guard<std::mutex> lock(over_cap_mutex_);
    over_cap_domain_.swap(lowered);
    over_cap_site_scoped_.store(site_scoped, std::memory_order_relaxed);
    // Re-evaluate from scratch on every license change so a re-pointed or
    // downgraded license can never carry a stale latch. Release-store so a
    // lock-free IsOverCap() acquire-load observes the reset (the mutex unlock
    // alone does not synchronize with lock-free readers).
    over_cap_.store(false, std::memory_order_release);
  }
}

void SystemServerContext::MaybeFlagOverCap(StringPiece hostname) {
  // the design record — over-cap is a SOFT, display/telemetry-only signal. This never
  // gates optimization, never alters the response, and never touches
  // license_active_.
  //
  // Hot-path short-circuits, cheapest first:
  //   1. already latched          → one relaxed atomic load, done.
  //   2. no active site-scope policy → one relaxed atomic load, done (the
  //      common case: unlicensed / community / org / host / scopeless / no
  //      domain). Neither path takes over_cap_mutex_.
  if (over_cap_.load(std::memory_order_relaxed)) return;
  if (!over_cap_site_scoped_.load(std::memory_order_relaxed)) return;

  // Normalize this request's host: strip any ":port", lowercase.
  GoogleString host;
  hostname.CopyToString(&host);
  if (const size_t colon = host.find(':'); colon != GoogleString::npos) {
    host.resize(colon);
  }
  LowerString(&host);

  // Only real external FQDNs count, and the licensed domain + its sub-domains
  // are exempt. Conservative: anything ambiguous is treated as in-scope.
  if (!IsExternalFqdn(host)) return;

  // Rare transition: first off-license host under the current policy. Serialize
  // with UpdateOverCapPolicy (which publishes a new domain + resets the latch
  // under the same lock) and RE-CHECK the policy under the lock — the gate read
  // above may be stale if a license change raced in. This is the guard 2.0's
  // adversarial review hardened: never latch a false positive against a policy
  // that has since changed (e.g. site → org, or re-pointed to this very host).
  std::lock_guard<std::mutex> lock(over_cap_mutex_);
  if (!over_cap_site_scoped_.load(std::memory_order_relaxed)) return;
  if (HostnameMatchesLicensedDomain(host, over_cap_domain_)) return;
  if (over_cap_.load(std::memory_order_relaxed)) return;  // already latched
  // Release-store so a lock-free IsOverCap() acquire-load (the per-port warn
  // header + status JSON) observes the latch on weak-memory CPUs without taking
  // over_cap_mutex_.
  over_cap_.store(true, std::memory_order_release);

  // Rate-limited WARNING (also effectively one-shot via the latch above). Mirror
  // the design record unlicensed-warning cadence for log-shape consistency.
  const int64 now_us = timer()->NowUs();
  int64_t prev_us = last_over_cap_warn_us_.load(std::memory_order_relaxed);
  constexpr int64_t kWarnIntervalUs = int64_t{60} * 1000 * 1000;  // 1 minute
  if (now_us - prev_us >= kWarnIntervalUs &&
      last_over_cap_warn_us_.compare_exchange_strong(
          prev_us, now_us, std::memory_order_relaxed)) {
    message_handler()->Message(
        kWarning,
        "License scope=site for domain '%s' but this install is optimizing "
        "'%s', which is outside the licensed site. Optimization continues "
        "(soft flag, the design record/092/105) — claim a license slot for each "
        "additional site to clear this notice.",
        over_cap_domain_.c_str(), host.c_str());
  }
}

void SystemServerContext::CreateLocalStatistics(
    Statistics* global_statistics, SystemRewriteDriverFactory* factory) {
  local_statistics_ = factory->AllocateAndInitSharedMemStatistics(
      true /* local */, hostname_identifier(),
      *global_system_rewrite_options());
  split_statistics_ = std::make_unique<SplitStatistics>(
      factory->thread_system(), local_statistics_, global_statistics);
  // local_statistics_ was ::InitStat'd by AllocateAndInitSharedMemStatistics,
  // but we need to take care of split_statistics_.
  factory->NonStaticInitStats(split_statistics_.get());
}

void SystemServerContext::InitStats(Statistics* statistics) {
  statistics->AddVariable(kCacheFlushCount);
  statistics->AddUpDownCounter(kCacheFlushTimestampMs);
  statistics->AddVariable(kStatistics404Count);
  Histogram* html_rewrite_time_us_histogram =
      statistics->AddHistogram(kHtmlRewriteTimeUsHistogram);
  // We set the boundary at 2 seconds which is about 2 orders of magnitude worse
  // than anything we have reasonably seen, to make sure we don't cut off actual
  // samples.
  html_rewrite_time_us_histogram->SetMaxValue(2 * Timer::kSecondUs);
  UrlAsyncFetcherStats::InitStats(kLocalFetcherStatsPrefix, statistics);
}

Variable* SystemServerContext::statistics_404_count() {
  return statistics()->GetVariable(kStatistics404Count);
}

void SystemServerContext::ChildInit(SystemRewriteDriverFactory* factory) {
  DCHECK(!initialized_);
  use_per_vhost_statistics_ = factory->use_per_vhost_statistics();
  if (!initialized_ && !global_options()->unplugged()) {
    initialized_ = true;
    system_caches_ = factory->caches();
    set_lock_manager(
        factory->caches()->GetLockManager(global_system_rewrite_options()));
    UrlAsyncFetcher* fetcher =
        factory->GetFetcher(global_system_rewrite_options());
    set_default_system_fetcher(fetcher);

    if (split_statistics_.get() != nullptr) {
      // Readjust the SHM stuff for the new process
      local_statistics_->Init(false, message_handler());

      // Create local stats for the ServerContext, and fill in its
      // statistics() and rewrite_stats() using them; if we didn't do this here
      // they would get set to the factory's by the InitServerContext call
      // below.
      set_statistics(split_statistics_.get());
      local_rewrite_stats_ = std::make_unique<RewriteStats>(
          factory->HasWaveforms(), split_statistics_.get(),
          factory->thread_system(), factory->timer());
      set_rewrite_stats(local_rewrite_stats_.get());

      // In case of gzip fetching, we will have the UrlAsyncFetcherStats take
      // care of decompression rather than the original fetcher, so we get
      // correct numbers for bytes fetched.
      bool fetch_with_gzip = global_system_rewrite_options()->fetch_with_gzip();
      if (fetch_with_gzip) {
        fetcher->set_fetch_with_gzip(false);
      }
      stats_fetcher_ = std::make_unique<UrlAsyncFetcherStats>(
          kLocalFetcherStatsPrefix, fetcher, factory->timer(),
          split_statistics_.get());
      if (fetch_with_gzip) {
        stats_fetcher_->set_fetch_with_gzip(true);
      }
      set_default_system_fetcher(stats_fetcher_.get());
    }

    // To allow Flush to come in while multiple threads might be
    // referencing the signature, we must be able to mutate the
    // timestamp and signature atomically.  RewriteOptions supports
    // an optional read/writer lock for this purpose.
    global_options()->set_cache_invalidation_timestamp_mutex(
        thread_system()->NewRWLock());
    factory->InitServerContext(this);

    html_rewrite_time_us_histogram_ =
        statistics()->GetHistogram(kHtmlRewriteTimeUsHistogram);
    html_rewrite_time_us_histogram_->SetMaxValue(2 * Timer::kSecondUs);
  }
}

void SystemServerContext::ApplySessionFetchers(const RequestContextPtr& request,
                                               RewriteDriver* driver) {
  const SystemRewriteOptions* conf =
      SystemRewriteOptions::DynamicCast(driver->options());
  CHECK(conf != nullptr);

  SystemRequestContext* system_request =
      SystemRequestContext::DynamicCast(request.get());
  if (system_request == nullptr) {
    return;  // decoding_driver has a null RequestContext.
  }

  // Note that these fetchers are applied in the opposite order of how they are
  // added: the last one added here is the first one applied and vice versa.
  //
  // Currently, we want AddHeadersFetcher running first, then
  // LoopbackRouteFetcher (and then the base fetcher).
  SystemRewriteOptions* options = global_system_rewrite_options();
  if (!options->disable_loopback_routing() && !options->slurping_enabled() &&
      !options->test_proxy()) {
    // Note the port here is our port, not from the request, since
    // LoopbackRouteFetcher may decide we should be talking to ourselves.
    driver->SetSessionFetcher(new LoopbackRouteFetcher(
        driver->options(), system_request->local_ip(),
        system_request->local_port(), system_request->local_scheme(),
        driver->async_fetcher()));
  }

  if (driver->options()->num_custom_fetch_headers() > 0) {
    driver->SetSessionFetcher(
        new AddHeadersFetcher(driver->options(), driver->async_fetcher()));
  }
}

void SystemServerContext::CollapseConfigOverlaysAndComputeSignatures() {
  ComputeSignature(global_system_rewrite_options());
}

// Handler which serves PSOL console.
void SystemServerContext::ConsoleHandler(const SystemRewriteOptions& options,
                                         AdminSite::AdminSource source,
                                         const QueryParams& query_params,
                                         AsyncFetch* fetch) {
  admin_site_->ConsoleHandler(*global_system_rewrite_options(), options, source,
                              query_params, fetch, statistics());
}

void SystemServerContext::StatisticsHandler(const RewriteOptions& options,
                                            bool is_global_request,
                                            AdminSite::AdminSource source,
                                            AsyncFetch* fetch) {
  if (!use_per_vhost_statistics_) {
    is_global_request = true;
  }
  Statistics* stats =
      is_global_request ? factory()->statistics() : statistics();
  admin_site_->StatisticsHandler(options, source, fetch, stats);
}

void SystemServerContext::ConsoleJsonHandler(const QueryParams& params,
                                             AsyncFetch* fetch) {
  admin_site_->ConsoleJsonHandler(params, fetch, statistics());
}

void SystemServerContext::PrintHistograms(bool is_global_request,
                                          AdminSite::AdminSource source,
                                          AsyncFetch* fetch) {
  Statistics* stats =
      is_global_request ? factory()->statistics() : statistics();
  admin_site_->PrintHistograms(source, fetch, stats);
}

void SystemServerContext::PrintCaches(bool is_global,
                                      AdminSite::AdminSource source,
                                      const GoogleUrl& stripped_gurl,
                                      const QueryParams& query_params,
                                      const RewriteOptions* options,
                                      AsyncFetch* fetch) {
  admin_site_->PrintCaches(is_global, source, stripped_gurl, query_params,
                           options, cache_path(), fetch, system_caches_,
                           filesystem_metadata_cache(), http_cache(),
                           metadata_cache(), page_property_cache(), this);
}

void SystemServerContext::PrintConfig(AdminSite::AdminSource source,
                                      AsyncFetch* fetch) {
  admin_site_->PrintConfig(source, fetch, global_system_rewrite_options());
}

void SystemServerContext::MessageHistoryHandler(const RewriteOptions& options,
                                                AdminSite::AdminSource source,
                                                AsyncFetch* fetch) {
  admin_site_->MessageHistoryHandler(options, source, fetch);
}

void SystemServerContext::AdminPage(bool is_global,
                                    const GoogleUrl& stripped_gurl,
                                    const QueryParams& query_params,
                                    const RewriteOptions* options,
                                    AsyncFetch* fetch,
                                    StringPiece request_body) {
  Statistics* stats = is_global ? factory()->statistics() : statistics();
  admin_site_->AdminPage(
      is_global, stripped_gurl, query_params, options, cache_path(), fetch,
      system_caches_, filesystem_metadata_cache(), http_cache(),
      metadata_cache(), page_property_cache(), this, statistics(), stats,
      global_system_rewrite_options(), request_body);
}

void SystemServerContext::StatisticsPage(bool is_global,
                                         const QueryParams& query_params,
                                         const RewriteOptions* options,
                                         AsyncFetch* fetch) {
  Statistics* stats = is_global ? factory()->statistics() : statistics();
  admin_site_->StatisticsPage(is_global, query_params, options, fetch,
                              system_caches_, filesystem_metadata_cache(),
                              http_cache(), metadata_cache(),
                              page_property_cache(), this, statistics(), stats,
                              global_system_rewrite_options());
}

}  // namespace net_instaweb
