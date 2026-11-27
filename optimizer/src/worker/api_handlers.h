// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

// PageSpeed 2.0 - Operational API Route Handlers
//
// Factory functions that create RouteHandler lambdas for the
// operational endpoints (/v1/health, /v1/stats, /v1/metrics,
// /v1/config).  Decoupled from the Worker class via ApiContext.

#ifndef PAGESPEED_SRC_WORKER_API_HANDLERS_H_
#define PAGESPEED_SRC_WORKER_API_HANDLERS_H_

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <utility>

#include "nlohmann/json.hpp"
#include "src/worker/http_server.h"
#include "src/worker/worker.h"

namespace pagespeed {

class ConfigPersister;

// Runtime context for operational API handlers.
// Provides access to worker state without coupling to the Worker class.
struct ApiContext {
  const WorkerStats& stats;
  std::function<std::shared_ptr<const WorkerConfig>()> get_config;
  std::function<void(std::shared_ptr<const WorkerConfig>)> update_config;
  std::function<int()> http_active_connections;
  std::function<int()> in_flight_work;
  std::function<uint64_t()> cache_entries;
  std::function<uint64_t()> cache_bytes;
  std::function<bool()>
      cache_degraded;  // true when cache was configured but failed
  int num_threads;
  int max_connections;
  std::chrono::steady_clock::time_point start_time;
  // Wall-clock epoch ms at worker construction (GET /v1/stats); the
  // monotonic start_time above stays the source for uptime_seconds.
  int64_t started_at_ms = 0;
  ServeStats* serve_stats = nullptr;  // mmap'd nginx bandwidth counters
  // Live read of the worker's browser analysis manager (nullptr when browser
  // analysis is disabled or failed to initialize), used by the shared
  // Prometheus builder for the pagespeed_browser_* series. May be unset
  // (treat as nullptr). The default initializer keeps existing designated
  // ApiContext constructions compiling under
  // -Wmissing-designated-field-initializers (-Werror on Linux).
  std::function<const BrowserAnalysisManager*()> browser_manager = nullptr;
  // The resolved headless-Chrome sandbox state as its wire
  // string ("disabled" | "on" | "unavailable" | "off").  Reported by
  // /v1/health and /v1/stats so a console or monitoring check can see that a
  // browser is running unsandboxed -- or refusing to run -- without reading
  // the journal.  May be unset (treated as "disabled").
  std::function<std::string()> browser_sandbox_state = nullptr;
  // Whether SOME kernel syscall filter is attached to this
  // process ("unknown" | "none" | "filtered"), read live from
  // /proc/self/status.  Read-only visibility, never a control: the daemon
  // does not install the filter (the service manager does, from the unit,
  // before execve; in a container the runtime's default profile does).  Note
  // "filtered" does NOT mean the optimizer's own hardening profile is in
  // force -- a stock container is already "filtered" by its runtime's default
  // profile.  May be unset (treated as "unknown").
  std::function<std::string()> syscall_filter_state = nullptr;
  // Reads a page of the worker's in-memory log ring for GET /v1/logs:
  // (since, has_since, limit) -> the response document.  Wired by the
  // Worker to WsManager::BuildLogsResponse; the /v1/logs route is registered
  // only when this is set.  The default initializer keeps existing
  // designated ApiContext constructions compiling under
  // -Wmissing-designated-field-initializers (-Werror on Linux), as with
  // browser_manager above.
  std::function<nlohmann::json(uint64_t since, bool has_since, size_t limit)>
      read_logs = nullptr;
  // Saves PATCH /v1/config changes on a dedicated thread so the event loop
  // never waits on the disk; the PATCH is answered once the change is saved.
  // When null the PATCH saves inline on the calling thread (unit tests that
  // build an ApiContext by hand).  Same default-initializer rationale as
  // browser_manager above.  Not owned; must outlive the registered routes.
  ConfigPersister* config_persister = nullptr;
};

// Builds the complete /v1/stats JSON document from live worker state.
//
// Single source of truth shared by the REST `GET /v1/stats` handler and the
// WebSocket `/v1/ws/stats` snapshot/delta provider, so the two payloads can
// never drift.  They previously diverged: the WS provider was a second,
// hand-maintained serializer that omitted the `svg`, `policy`, and
// `quality_baselining` groups plus several cache-reliability counters, and
// emitted `errors` as a scalar instead of an object.  Because the live
// dashboard is fed by the WS stream, those fields perpetually rendered 0
// (e.g. SVG "Candidates Evaluated") while the REST-fed Metrics page was
// correct.  Routing both paths through this one function removes the drift.
nlohmann::json BuildStatsJson(ApiContext& ctx);

// Inputs for the shared Prometheus builder below.  A plain value struct (not
// ApiContext) so the management-socket path can build metrics even when the
// HTTP API is disabled (api_port == 0, in which case ApiContext is never
// constructed).
struct PrometheusMetricsInputs {
  const WorkerStats& stats;
  uint64_t cache_entries = 0;
  uint64_t cache_bytes = 0;
  int active_connections = 0;
  int max_connections = 0;
  int in_flight_work = 0;
  // nullptr when browser analysis is disabled/failed to initialize: the
  // pagespeed_browser_* series are omitted.
  const BrowserAnalysisManager* browser_manager = nullptr;
  // nullptr when the serve-stats mmap is unavailable: the serve-time series
  // (bytes/hits served, webbotauth verdicts) are omitted.
  const ServeStats* serve_stats = nullptr;
  // Operator verified-bot registry ("keyid=name,keyid2=name2"), used to resolve
  // per-signer keyid hashes to names for the first-party
  // pagespeed_webbotauth per-signer series.  Empty = names
  // unresolved (signers appear by opaque keyid hash).
  std::string web_bot_auth_verified_bots;
};

// Builds the complete Prometheus text-exposition document from live worker
// state.
//
// Single source of truth shared by the REST `GET /v1/metrics` handler and the
// management-socket `METRICS` command (Worker::BuildPrometheusMetrics), so the
// two surfaces can never drift.  They previously were two hand-maintained
// near-duplicates that diverged in BOTH directions: #863/#864 added the
// pagespeed_webbotauth_signed_requests_total series to the socket builder
// only (invisible on the documented HTTP surface until #871 mirrored it),
// while the socket builder never gained a dozen newer series (per-type
// processed counts, cache auto-heal, origin refresh, learned quality, policy)
// and the HTTP handler lacked the html-assembly and pagespeed_browser_*
// series.  Routing both paths through this one function removes the drift
// (#877); both surfaces now emit the union.
std::string BuildPrometheusMetricsText(const PrometheusMetricsInputs& in);

// Register all operational routes on the server.
//   GET  /v1/health   - Health check (always unauthenticated)
//   GET  /v1/stats    - Full JSON statistics
//   GET  /v1/metrics  - Prometheus text exposition format
//   GET  /v1/config   - Current configuration
//   PATCH /v1/config  - Hot-reload configuration fields
//   GET  /v1/logs     - Recent log entries (registered when ctx.read_logs is set)
void RegisterOperationalRoutes(HttpServer& server, ApiContext& ctx);

}  // namespace pagespeed

#endif  // PAGESPEED_SRC_WORKER_API_HANDLERS_H_
