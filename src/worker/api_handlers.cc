// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

// PageSpeed 2.0 - Operational API Route Handlers Implementation

#include "src/worker/api_handlers.h"

#include <cerrno>
#include <cstdint>
#include <exception>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <unordered_set>
#include <utility>
#include <vector>

#include "absl/strings/numbers.h"
#include "absl/strings/str_cat.h"
#include "lib/base/message_handler.h"
#include "lib/classify/url_normalizer.h"
#include "nlohmann/json.hpp"
#include "src/browser/agent_fetcher.h"
#include "src/product_version/build_commit.h"
#include "src/product_version/version.h"
#include "src/worker/config_file.h"
#include "src/worker/config_persister.h"
#include "src/worker/json_dump.h"
#include "src/worker/serve_stats.h"
#include "src/worker/shared_config.h"
#include "src/worker/worker.h"

namespace pagespeed {

using json = nlohmann::json;

// ---------------------------------------------------------------------------
// GET /v1/health
// ---------------------------------------------------------------------------

// Records a single named health check into `checks`.  On failure it attaches
// the supplied detail and downgrades `overall` to "degraded".  Factored out of
// HandleHealth so each check is a one-liner instead of a nested scope block.
static void AddHealthCheck(json& checks, std::string& overall, const char* name,
                           bool pass, const std::string& detail) {
  checks[name] = {{"pass", pass}};
  if (!pass) {
    checks[name]["detail"] = detail;
    overall = "degraded";
  }
}

static HttpResponse HandleHealth(ApiContext& ctx,
                                 const HttpRequest& /*request*/) {
  auto now = std::chrono::steady_clock::now();
  int64_t uptime_s =
      std::chrono::duration_cast<std::chrono::seconds>(now - ctx.start_time)
          .count();

  int active = ctx.http_active_connections();
  int inflight = ctx.in_flight_work();
  bool ready = inflight < ctx.num_threads;

  auto config = ctx.get_config();

  // Health checks — each produces a pass/fail with optional detail.
  json checks;
  std::string overall = "ok";

  // Check: cache not degraded (configured but failed to open).
  AddHealthCheck(checks, overall, "cache_open",
                 !ctx.cache_degraded || !ctx.cache_degraded(),
                 "cache failed to open");

  // Check: cache path configured.
  AddHealthCheck(checks, overall, "cache_configured",
                 !config->cache_path.empty(), "no cache path configured");

  // Check: the shared-config schema version this build reads matches what is
  // on disk.  Read from the sticky process-wide record rather than by
  // re-parsing here, so the check reports the state that actually produced the
  // running configuration.  A mismatch means every setting in that file was
  // discarded in favour of compiled-in defaults — the install is running, but
  // not as configured — which is a degradation and is reported as one.  This
  // is the surface a support bundle carries, and it exists because a
  // mismatched install otherwise presents only as "the cache never warms".
  {
    const SharedConfigVersionSkew skew = SharedConfigVersionSkewState();
    std::string detail;
    if (skew.mismatch) {
      detail = absl::StrCat(
          "shared config declares schema version ", skew.observed_version,
          " but this build reads version ", skew.supported_version,
          "; running on compiled-in defaults (observed ", skew.observations,
          " time(s)). Align the versions of the components sharing this cache "
          "directory.");
    }
    AddHealthCheck(checks, overall, "shared_config_version", !skew.mismatch,
                   detail);
  }

  // There is deliberately no "license" check and no license object below:
  // since 2.1 the daemon holds no license state at all.

  json j;
  j["status"] = overall;
  j["checks"] = checks;
  j["version"] = kPageSpeedVersion;
  j["git_commit"] = kBuildCommitShort;
  j["uptime_seconds"] = uptime_s;
  j["connections"] = {{"active", active}, {"max", ctx.max_connections}};
  j["inflight"] = inflight;
  j["ready"] = ready;

  // Always present, so "no browser_sandbox key" means "an old
  // daemon", never "we forgot to look".  "unavailable" is a REFUSAL (browser
  // analysis is off and the daemon is serving without it), not a degrade to
  // an unsandboxed browser.
  j["browser_sandbox"] =
      ctx.browser_sandbox_state ? ctx.browser_sandbox_state() : "disabled";

  // #1467: whether the headless browser is actually up, not just configured.
  // A browser that keeps dying at launch shows here as chrome_running=false
  // with a climbing failure count and a growing retry delay, so an operator
  // (and the image gate) can tell "browser analysis unavailable" from
  // "enabled and idle" without reading the log.
  if (const BrowserAnalysisManager* bm =
          ctx.browser_manager ? ctx.browser_manager() : nullptr) {
    const auto& bs = bm->stats();
    j["browser"] = {
        {"enabled", true},
        {"chrome_running", bm->chrome_running()},
        {"chrome_consecutive_failures",
         bs.chrome_consecutive_failures.load(std::memory_order_relaxed)},
        {"chrome_restart_delay_ms",
         bs.chrome_restart_delay_ms.load(std::memory_order_relaxed)},
    };
  } else {
    j["browser"] = {{"enabled", false}};
  }

  // Read-only, from /proc/self/status.  Always present for
  // the same reason as browser_sandbox -- a missing key means an old daemon,
  // never "we did not look".  "filtered" means A filter is attached, not that
  // the optimizer's own hardening profile is: a stock container is already
  // "filtered" by its runtime's default profile.
  j["syscall_filter"] =
      ctx.syscall_filter_state ? ctx.syscall_filter_state() : "unknown";

  return HttpResponse().Json(DumpJson(j));
}

// ---------------------------------------------------------------------------
// GET /v1/stats
// ---------------------------------------------------------------------------

// Builds the /v1/stats JSON document.  Shared verbatim by the REST handler
// (HandleStats, below) and the WebSocket /v1/ws/stats provider in worker.cc,
// so the two payloads can never drift.  See api_handlers.h for the rationale.
json BuildStatsJson(ApiContext& ctx) {
  const auto& s = ctx.stats;
  // Same value as GET /v1/health, on the surface the console
  // and the WebSocket stats stream both read.
  const std::string browser_sandbox =
      ctx.browser_sandbox_state ? ctx.browser_sandbox_state() : "disabled";
  // Same value as GET /v1/health.
  const std::string syscall_filter =
      ctx.syscall_filter_state ? ctx.syscall_filter_state() : "unknown";

  uint64_t ssim_checks = s.ssimulacra2_checks.load();
  uint64_t avg_ssim =
      ssim_checks > 0 ? s.ssimulacra2_total_score_x100.load() / ssim_checks : 0;

  json j;
  j["browser_sandbox"] = browser_sandbox;
  j["syscall_filter"] = syscall_filter;
  j["notifications"] = {
      {"received", s.notifications_received.load()},
      {"skipped_dedup", s.notifications_skipped_dedup.load()},
      {"skipped_inflight", s.notifications_skipped_inflight.load()},
      // Dedup entries erased because their variant family is gone from the
      // cache (eviction has no worker callback); the URL is reprocessed.
      {"dedup_healed", s.notifications_dedup_healed.load()},
      // Notifications dropped by the heal window, which is kept per URL
      // and client class: the URL was re-processed by one of the two heals
      // less than 10 seconds ago, or the table that tracks those windows
      // was full.  That covers a family that looked orphaned, a missing
      // optimized copy, and any repeat notification arriving right after
      // either heal.  A steadily climbing value with no heals beside it
      // means a URL is empty by decision (nothing servable will ever be
      // produced for it), not that a heal is being lost.
      {"dedup_heal_rate_limited",
       s.notifications_dedup_heal_rate_limited.load()},
      // Processed-set hits erased because the optimized copy this optimizer
      // wrote for the URL is gone from the cache while other copies remain;
      // the URL is processed again.  Counted when the re-processing starts,
      // whether or not it ends with the copy written back.  Sustained
      // growth means single copies of URLs keep going missing while their
      // other copies stay, whatever removes them.
      {"missing_copy_healed", s.notifications_missing_copy_healed.load()},
      // Refusals, split by cause.  A peer on another wire version and a peer
      // sending corrupt frames both show up as "nothing is being optimized";
      // these are what tell them apart without reading logs.
      {"rejected_version", s.notifications_rejected_version.load()},
      {"rejected_malformed", s.notifications_rejected_malformed.load()},
      {"rejected_sentinel", s.notifications_rejected_sentinel.load()},
      {"rejected_option_context",
       s.notifications_rejected_option_context.load()},
      {"rejected_empty_site", s.notifications_rejected_empty_site.load()},
      {"rejected_module_render", s.notifications_rejected_module_render.load()},
      // Not refusals: frames from a caller that predates site identity
      // (processed unsited), and page work withheld from an unvouched host.
      {"legacy_ex_caller", s.notifications_legacy_ex_caller.load()},
      {"unvouched_render_skipped",
       s.notifications_unvouched_render_skipped.load()},
      // Not a refusal: a valid non-default per-request options context was
      // accepted and its work stored under the default context.
      {"accepted_non_default_option_context",
       s.notifications_accepted_non_default_option_context.load()}};
  // Answers not used in shared output (sub-resource checks, agent render,
  // /llms.txt), by reason, since start.
  {
    const SharedCopyRefusalCounts r = GetSharedCopyRefusalCounts();
    j["subresource_refusals"] = {{"cookie", r.cookie},
                                 {"no_store", r.no_store},
                                 {"private", r.private_answer},
                                 {"vary", r.vary},
                                 {"private_address", r.private_address}};
  }
  j["known_sites"] = {{"overflow", s.known_sites_overflow.load()},
                      {"incomplete", s.known_sites_incomplete.load() != 0}};
  j["variants"] = {{"written", s.variants_written.load()},
                   {"proactive", s.proactive_variants_written.load()},
                   {"gzip", s.gzip_variants_written.load()},
                   {"brotli", s.brotli_variants_written.load()}};
  j["errors"] = {
      {"total", s.errors.load()},
      {"origin_misconfiguration", s.errors_origin_misconfiguration.load()},
      {"text_minify_parse_failures", s.text_minify_parse_failures.load()}};
  j["cache"] = {{"entries", ctx.cache_entries()},
                {"size_bytes", ctx.cache_bytes()}};
  j["by_type"] = {{"html",
                   {{"count", s.html_processed.load()},
                    {"time_us", s.html_processing_time_us.load()}}},
                  {"css",
                   {{"count", s.css_processed.load()},
                    {"time_us", s.css_processing_time_us.load()}}},
                  {"js",
                   {{"count", s.js_processed.load()},
                    {"time_us", s.js_processing_time_us.load()}}},
                  {"image",
                   {{"count", s.images_processed.load()},
                    {"time_us", s.image_processing_time_us.load()}}}};
  j["by_format"] = {{"webp", s.webp_generated.load()},
                    {"avif", s.avif_generated.load()},
                    {"jpeg", s.jpeg_optimized.load()},
                    {"png", s.png_optimized.load()}};
  j["content_analysis"] = {
      {"photo", s.content_photo.load()},
      {"screenshot", s.content_screenshot.load()},
      {"illustration", s.content_illustration.load()},
      {"noisy", s.content_noisy.load()},
      {"denoised", 0}};  // removed feature; kept at 0 for consumers
  j["ssimulacra2"] = {
      {"checks", ssim_checks},
      {"reencodes", s.ssimulacra2_reencodes.load()},
      {"declines", s.ssimulacra2_declines.load()},
      {"tombstone_hits", s.ssimulacra2_decline_tombstone_hits.load()},
      {"avg_score_x100", avg_ssim}};
  j["html_assembly"] = {{"complete", s.html_assembly_complete.load()},
                        {"skipped", s.html_assembly_skipped.load()},
                        {"css_aborted", s.critical_css_aborted.load()},
                        {"critical_css_skipped_high_coverage",
                         s.critical_css_skipped_high_coverage.load()},
                        {"critical_css_skipped_byte_cap",
                         s.critical_css_skipped_byte_cap.load()}};
  j["alternates"] = {
      {"writes", s.alternate_writes.load()},
      {"write_failures", s.alternate_write_failures.load()},
      // Issue E: benign purge-fenced writes, split from hard write_failures.
      {"writes_fenced", s.alternate_writes_fenced.load()}};
  // Issue E: origin-refresh activity (incremented in HandleOriginRefreshed but
  // never serialized before).
  j["origin_refresh"] = {
      {"purges", s.origin_refresh_purges.load()},
      {"rate_limited", s.origin_refresh_rate_limited.load()},
      // Inline rebuilds refused: the identity slot held a worker-processed
      // (stale) variant, not the refreshed origin; the next record+notify
      // rebuilds from fresh bytes instead.
      {"rebuild_refused", s.origin_refresh_rebuild_refused.load()},
      // Issue #1503: refreshes where the pristine origin reference matched
      // the content-hash oracle — the variant set was restamped in place
      // rather than purged — and refreshes deferred to the record+notify
      // convergence because the re-recorded origin had not landed yet.
      {"unchanged", s.origin_refresh_unchanged.load()},
      {"deferred", s.origin_refresh_deferred.load()},
      // A re-recorded original seen on a dedup hit: looked at, rebuilt
      // (new bytes), restamped (same bytes).
      {"rechecked", s.notifications_origin_rechecked.load()},
      {"change_rebuilt", s.origin_change_rebuilt.load()},
      {"unchanged_restamped", s.origin_unchanged_restamped.load()}};
  // Durability (#19): agent_markdown coverage telemetry — preserved
  // counts unchanged-content refreshes that kept the variant alive (the lift),
  // purged_on_change counts genuine content changes that dropped it, and
  // rebuild_forced counts multi-churn-race heals driven by the sticky intent
  // flag.  A healthy stable site shows preserved >> purged_on_change.
  //
  // below_floor counts copies not kept because their body fell below the
  // fidelity floor, on both paths (static and rendered).  The module_* keys
  // cover pages the module's HTML store wrote, derived without a browser:
  // copies written, permanent refusals recorded as a withheld marker, and
  // frames that found such a marker for the content and did no derivation.
  uint64_t below_floor = s.agent_markdown_below_floor.load();
  if (const BrowserAnalysisManager* bm =
          ctx.browser_manager ? ctx.browser_manager() : nullptr) {
    below_floor += bm->stats().agent_markdown_below_floor.load();
  }
  j["agent_markdown"] = {
      {"preserved", s.agent_markdown_preserved.load()},
      {"purged_on_change", s.agent_markdown_purged_on_change.load()},
      {"rebuild_forced", s.agent_markdown_rebuild_forced.load()},
      {"below_floor", below_floor},
      {"module_derived", s.module_markdown_derived.load()},
      {"module_withheld", s.module_markdown_withheld.load()},
      {"module_withheld_skips", s.module_markdown_withheld_skips.load()}};
  j["selector_invocations"] = s.selector_invocations.load();
  j["source_reads_from_durable_original"] =
      s.source_reads_from_durable_original.load();
  j["cache_read_retries"] = s.cache_read_retries.load();
  j["cache_read_failures"] = s.cache_read_failures.load();
  j["cache_read_deferred_retries"] = s.cache_read_deferred_retries.load();
  j["cache_read_deferred_successes"] = s.cache_read_deferred_successes.load();
  j["read_borrow_wrap_discards"] = s.read_borrow_wrap_discards.load();
  j["cache_auto_heals"] = s.cache_auto_heals.load();
  j["cache_auto_heal_exhausted"] = s.cache_auto_heal_exhausted.load();
  j["image_incomplete_matrices"] = s.image_incomplete_matrices.load();
  j["image_no_savings_skipped"] = s.image_no_savings_skipped.load();
  j["image_unconverted_fallthrough"] = s.image_unconverted_fallthrough.load();
  j["image_size_refused"] = {{"webp", s.image_size_refused_webp.load()},
                             {"avif", s.image_size_refused_avif.load()}};
  j["image_savedata_refused"] = s.image_savedata_refused.load();
  j["image_budget_rescued"] = {{"webp", s.image_budget_rescued_webp.load()},
                               {"avif", s.image_budget_rescued_avif.load()}};
  j["dedup"] = {{"writes_skipped", s.dedup_writes_skipped.load()},
                {"content_hash_hits", s.content_hash_hits.load()},
                {"content_hash_stale", s.content_hash_stale.load()}};
  j["learned_quality"] = {{"predictions", s.learned_quality_predictions.load()},
                          {"fallbacks", s.learned_quality_fallbacks.load()}};
  j["quality_baselining"] = {{"capped_jpeg", s.quality_capped_jpeg.load()},
                             {"skip_reencode", s.quality_skip_reencode.load()}};
  j["svg"] = {
      {"candidates_evaluated", s.svg_candidates_evaluated.load()},
      {"candidates_rejected", s.svg_candidates_rejected.load()},
      {"vectorized", s.svg_vectorized.load()},
      {"fidelity_rejected", s.svg_fidelity_rejected.load()},
      {"timeout_exceeded", s.svg_timeout_exceeded.load()},
      {"size_rejected", s.svg_size_rejected.load()},
      {"path_count_rejected", s.svg_path_count_rejected.load()},
      {"written", s.svg_written.load()},
      {"bytes_saved", s.svg_bytes_saved.load()},
      {"vectorize_time_us", s.svg_vectorize_time_us.load()},
      // served = serve-time SVG HITs, read from the cross-process ServeStats
      // mmap (the WorkerStats field was unwritable / always 0 — the
      // follow-up in #455). 0 when the serve-stats file isn't mapped yet.
      {"served", ctx.serve_stats != nullptr
                     ? *reinterpret_cast<const volatile uint64_t*>(
                           &ctx.serve_stats->svg_optimized_hits)
                     : 0},
  };
  j["policy"] = {
      {"computed", s.policy_computed.load()},
      {"async_css_enabled", s.policy_async_css_enabled.load()},
      {"async_css_suppressed_low_coverage",
       s.async_css_suppressed_low_coverage.load()},
      {"async_css_suppressed_unvalidated",
       s.async_css_suppressed_unvalidated.load()},
      {"async_css_record_dropped_empty_derivation",
       s.async_css_record_dropped_empty_derivation.load()},
      {"script_deferral_enabled", s.policy_script_deferral_enabled.load()}};
  j["thread_pool"] = {{"inflight", ctx.in_flight_work()},
                      {"size", ctx.num_threads}};
  j["connections"] = {{"active", ctx.http_active_connections()},
                      {"max", ctx.max_connections}};

  // Serve-time bandwidth savings from the serving front end (shared mmap
  // counters; nginx 2.0, the ASP.NET middleware, or the 1.16 module).
  if (ctx.serve_stats != nullptr) {
    auto* ss = ctx.serve_stats;
    // Aligned 64-bit loads are atomic on x86-64 (both GCC/Clang and MSVC).
    auto load = [](const uint64_t& field) -> uint64_t {
      return *reinterpret_cast<const volatile uint64_t*>(&field);
    };
    // v9: the same serves, split by the transfer encoding actually served
    // (the variant's capability mask, recorded per hit).  An all-identity
    // row on a front end that stores compressed variants is the "compressed
    // copies not served here" signal a stats consumer can see.
    auto by_encoding = [&load](const ServeStats* s, size_t type_index) {
      return json{
          {"identity",
           {{"hits", load(s->serve_hits_by_encoding[type_index][0])},
            {"bytes", load(s->serve_bytes_by_encoding[type_index][0])}}},
          {"gzip",
           {{"hits", load(s->serve_hits_by_encoding[type_index][1])},
            {"bytes", load(s->serve_bytes_by_encoding[type_index][1])}}},
          {"br",
           {{"hits", load(s->serve_hits_by_encoding[type_index][2])},
            {"bytes", load(s->serve_bytes_by_encoding[type_index][2])}}}};
    };
    // The saving divided into what the served encoding saves and what the
    // rewrite saves, over the serves whose encoded-origin size was known.
    // original_bytes - compressed_original_bytes is the encoding's share and
    // compressed_original_bytes - optimized_bytes the rewrite's (negative when
    // the served bytes exceed the encoded origin).  All three are 0 when no
    // serve carried the size; the share of original_bytes they cover is the
    // coverage.
    auto split_json = [ss](ContentType type) {
      const ServeStats::SplitCounters sp = ReadServeSplit(ss, type);
      return json{{"original_bytes", sp.original_bytes},
                  {"compressed_original_bytes", sp.compressed_original_bytes},
                  {"optimized_bytes", sp.optimized_bytes}};
    };
    j["serve_savings"] = {
        {"html",
         {{"original_bytes", load(ss->html_original_bytes)},
          {"optimized_bytes", load(ss->html_optimized_bytes)},
          {"hits", load(ss->html_optimized_hits)},
          {"by_encoding", by_encoding(ss, 0)},
          {"split", split_json(ContentType::kHtml)}}},
        {"css",
         {{"original_bytes", load(ss->css_original_bytes)},
          {"optimized_bytes", load(ss->css_optimized_bytes)},
          {"hits", load(ss->css_optimized_hits)},
          {"by_encoding", by_encoding(ss, 1)},
          {"split", split_json(ContentType::kCss)}}},
        {"js",
         {{"original_bytes", load(ss->js_original_bytes)},
          {"optimized_bytes", load(ss->js_optimized_bytes)},
          {"hits", load(ss->js_optimized_hits)},
          {"by_encoding", by_encoding(ss, 2)},
          {"split", split_json(ContentType::kJs)}}},
        {"image",
         {{"original_bytes", load(ss->image_original_bytes)},
          {"optimized_bytes", load(ss->image_optimized_bytes)},
          {"hits", load(ss->image_optimized_hits)},
          {"by_encoding", by_encoding(ss, 3)},
          {"split", split_json(ContentType::kImage)}}},
    };
    // The same serves attributed to the host the front end served them for:
    // up to 32 hosts (of the first 64 distinct hosts seen since the optimizer
    // started), most serves first, and everything else -- the remaining
    // hosts and serves recorded without a host -- under "other".  Host names
    // passed NormalizeServeHost, so they are plain host names.
    const ServeSavingsByHost by_host =
        ReadServeSavingsByHost(ss, kServeHostReportLimit);
    json hosts = json::array();
    for (const ServeHostRow& row : by_host.hosts) {
      json entry;
      entry["host"] = row.host;
      entry["hits"] = row.hits;
      entry["original_bytes"] = row.original_bytes;
      entry["optimized_bytes"] = row.optimized_bytes;
      hosts.push_back(std::move(entry));
    }
    json other;
    other["hits"] = by_host.other.hits;
    other["original_bytes"] = by_host.other.original_bytes;
    other["optimized_bytes"] = by_host.other.optimized_bytes;
    json serve_by_host;
    serve_by_host["hosts"] = std::move(hosts);
    serve_by_host["limit"] = kServeHostReportLimit;
    serve_by_host["other"] = std::move(other);
    j["serve_savings_by_host"] = std::move(serve_by_host);
    // Zero-copy serve-barrier verdicts (from the same nginx mmap).
    j["zerocopy"] = {
        {"torn_aborts", load(ss->zerocopy_torn_aborts)},
        {"proactive_copyouts", load(ss->zerocopy_proactive_copyouts)},
        {"copy_then_verify_discards",
         load(ss->zerocopy_copy_then_verify_discards)}};
    // Bounded stale-serve telemetry (issue #652).
    j["stale_serves"] = {{"swr_coalesced", load(ss->swr_coalesced_serves)},
                         {"stale_if_error", load(ss->stale_if_error_serves)}};
    // The v8 serve-class partition (same mmap): exactly one of the five
    // classes per serve the front end classified as optimizable, plus the
    // notify-suppression counter (orthogonal to the partition) and the two
    // unrecognized-write counters that keep a dropped or newer-than-us write
    // visible.  Live where the 1.16 module fronts the cache (its writer lands
    // with the serve-stats record_hit wiring, mpp #891); no in-tree front end
    // classifies serves yet, so all-zero means "not instrumented", not
    // "nothing happened" — see the field comments in serve_stats.h.
    j["serve_classes"] = {
        {"optimized", load(ss->serve_optimized_total)},
        {"original_cold", load(ss->serve_original_cold_total)},
        {"original_pending", load(ss->serve_original_pending_total)},
        {"original_declined", load(ss->serve_original_declined_total)},
        {"original_skew", load(ss->serve_original_skew_total)},
        {"notify_suppressed", load(ss->notify_suppressed_total)},
        {"class_unrecognized", load(ss->serve_class_unrecognized_total)},
        {"flags_unrecognized", load(ss->serve_flags_unrecognized_total)}};
  }

  // Entries judged already optimal at processing time: no smaller identity
  // variant was produced.  Per processed entry (not per serve) — CSS/JS at
  // the "already minimal" verdict, images at the no-savings skip — over
  // this worker process's lifetime.
  j["verdicts"] = {
      {"css",
       {{"already_optimal",
         {{"count", ctx.stats.css_already_optimal_count.load()},
          {"bytes", ctx.stats.css_already_optimal_bytes.load()}}}}},
      {"js",
       {{"already_optimal",
         {{"count", ctx.stats.js_already_optimal_count.load()},
          {"bytes", ctx.stats.js_already_optimal_bytes.load()}}}}},
      {"image",
       {{"already_optimal",
         {{"count", ctx.stats.image_already_optimal_count.load()},
          {"bytes", ctx.stats.image_already_optimal_bytes.load()}}}}}};

  // The worker's own time base: when this process started (wall clock) and
  // how long it has been up (monotonic).  One stats read answers "since
  // when" for every counter in this document without a second health read.
  const auto now = std::chrono::steady_clock::now();
  j["uptime_seconds"] =
      std::chrono::duration_cast<std::chrono::seconds>(now - ctx.start_time)
          .count();
  j["started_at_ms"] = ctx.started_at_ms;

  return j;
}

static HttpResponse HandleStats(ApiContext& ctx,
                                const HttpRequest& /*request*/) {
  return HttpResponse().Json(DumpJson(BuildStatsJson(ctx)));
}

// ---------------------------------------------------------------------------
// GET /v1/metrics
// ---------------------------------------------------------------------------

std::string BuildPrometheusMetricsText(const PrometheusMetricsInputs& in) {
  const auto& s = in.stats;
  std::string m;

  auto counter = [&m](const char* name, const char* help, uint64_t val) {
    absl::StrAppend(&m, "# HELP ", name, " ", help, "\n");
    absl::StrAppend(&m, "# TYPE ", name, " counter\n");
    absl::StrAppend(&m, name, " ", val, "\n");
  };

  auto gauge = [&m](const char* name, const char* help, uint64_t val) {
    absl::StrAppend(&m, "# HELP ", name, " ", help, "\n");
    absl::StrAppend(&m, "# TYPE ", name, " gauge\n");
    absl::StrAppend(&m, name, " ", val, "\n");
  };

  counter("pagespeed_notifications_total", "Total notifications received.",
          s.notifications_received.load());
  counter("pagespeed_notifications_skipped_total",
          "Notifications skipped (dedup).",
          s.notifications_skipped_dedup.load());
  counter("pagespeed_notifications_skipped_inflight_total",
          "Notifications skipped (in-flight).",
          s.notifications_skipped_inflight.load());
  counter("pagespeed_notifications_dedup_heal_rate_limited_total",
          "Notifications dropped because the URL was re-processed by a heal "
          "(orphaned entry or missing optimized copy) less than 10 seconds "
          "before for the same client class, or because the table tracking "
          "those heals was full; the notification was skipped instead of "
          "re-processed.",
          s.notifications_dedup_heal_rate_limited.load());
  counter("pagespeed_notifications_dedup_healed_total",
          "Dedup-set hits erased because no servable variant remains in the "
          "cache for the key; the notification is reprocessed, not skipped.",
          s.notifications_dedup_healed.load());
  counter("pagespeed_notifications_missing_copy_healed_total",
          "Notifications for an already optimized URL whose optimized copy "
          "was no longer in the cache while its other copies were; the URL "
          "is processed again, at most once per URL and client class per 10 "
          "seconds. Counted when the re-processing starts.",
          s.notifications_missing_copy_healed.load());
  counter("pagespeed_notifications_rejected_version_total",
          "Notifications refused because the sending peer speaks another "
          "wire protocol version.",
          s.notifications_rejected_version.load());
  counter("pagespeed_notifications_rejected_malformed_total",
          "Notifications refused as truncated or corrupt (version matched).",
          s.notifications_rejected_malformed.load());
  counter("pagespeed_notifications_rejected_sentinel_total",
          "Notifications refused for naming a reserved entry class.",
          s.notifications_rejected_sentinel.load());
  counter("pagespeed_notifications_rejected_option_context_total",
          "Notifications refused because their per-request options context "
          "failed validation: unknown payload format, malformed signature, or "
          "a signature that does not match its payload. An oversized payload "
          "is refused earlier as a malformed frame and counted in "
          "pagespeed_notifications_rejected_malformed_total instead.",
          s.notifications_rejected_option_context.load());
  counter("pagespeed_notifications_accepted_non_default_option_context_total",
          "Notifications accepted while carrying a valid non-default "
          "per-request options context; the work is stored under the default "
          "context.",
          s.notifications_accepted_non_default_option_context.load());
  counter("pagespeed_variants_written_total", "Total variants written.",
          s.variants_written.load());
  counter("pagespeed_proactive_variants_total", "Proactive variants written.",
          s.proactive_variants_written.load());
  counter("pagespeed_errors_total", "Total processing errors.",
          s.errors.load());
  counter("pagespeed_errors_origin_misconfiguration_total",
          "Origin misconfiguration errors (pre-compressed responses).",
          s.errors_origin_misconfiguration.load());

  // Per-type processing counts
  absl::StrAppend(
      &m, "# HELP pagespeed_processed_total Items processed by type.\n");
  absl::StrAppend(&m, "# TYPE pagespeed_processed_total counter\n");
  absl::StrAppend(&m, "pagespeed_processed_total{type=\"html\"} ",
                  s.html_processed.load(), "\n");
  absl::StrAppend(&m, "pagespeed_processed_total{type=\"css\"} ",
                  s.css_processed.load(), "\n");
  absl::StrAppend(&m, "pagespeed_processed_total{type=\"js\"} ",
                  s.js_processed.load(), "\n");
  absl::StrAppend(&m, "pagespeed_processed_total{type=\"image\"} ",
                  s.images_processed.load(), "\n");

  // Per-type already-optimal verdicts (entries, original bytes).
  absl::StrAppend(
      &m, "# HELP pagespeed_already_optimal_total ",
      "Entries whose optimization produced no smaller identity variant.\n");
  absl::StrAppend(&m, "# TYPE pagespeed_already_optimal_total counter\n");
  absl::StrAppend(&m, "pagespeed_already_optimal_total{type=\"css\"} ",
                  s.css_already_optimal_count.load(), "\n");
  absl::StrAppend(&m, "pagespeed_already_optimal_total{type=\"js\"} ",
                  s.js_already_optimal_count.load(), "\n");
  absl::StrAppend(&m, "pagespeed_already_optimal_total{type=\"image\"} ",
                  s.image_already_optimal_count.load(), "\n");
  absl::StrAppend(&m, "# HELP pagespeed_already_optimal_bytes_total ",
                  "Original bytes of entries already optimal.\n");
  absl::StrAppend(&m, "# TYPE pagespeed_already_optimal_bytes_total counter\n");
  absl::StrAppend(&m, "pagespeed_already_optimal_bytes_total{type=\"css\"} ",
                  s.css_already_optimal_bytes.load(), "\n");
  absl::StrAppend(&m, "pagespeed_already_optimal_bytes_total{type=\"js\"} ",
                  s.js_already_optimal_bytes.load(), "\n");
  absl::StrAppend(&m, "pagespeed_already_optimal_bytes_total{type=\"image\"} ",
                  s.image_already_optimal_bytes.load(), "\n");

  // Per-type processing time
  absl::StrAppend(
      &m,
      "# HELP pagespeed_processing_seconds_total Processing time by type.\n");
  absl::StrAppend(&m, "# TYPE pagespeed_processing_seconds_total counter\n");
  absl::StrAppend(&m, "pagespeed_processing_seconds_total{type=\"html\"} ",
                  static_cast<double>(s.html_processing_time_us.load()) / 1e6,
                  "\n");
  absl::StrAppend(&m, "pagespeed_processing_seconds_total{type=\"css\"} ",
                  static_cast<double>(s.css_processing_time_us.load()) / 1e6,
                  "\n");
  absl::StrAppend(&m, "pagespeed_processing_seconds_total{type=\"js\"} ",
                  static_cast<double>(s.js_processing_time_us.load()) / 1e6,
                  "\n");
  absl::StrAppend(&m, "pagespeed_processing_seconds_total{type=\"image\"} ",
                  static_cast<double>(s.image_processing_time_us.load()) / 1e6,
                  "\n");

  // Per-format generation
  absl::StrAppend(
      &m, "# HELP pagespeed_format_generated_total Images by output format.\n");
  absl::StrAppend(&m, "# TYPE pagespeed_format_generated_total counter\n");
  absl::StrAppend(&m, "pagespeed_format_generated_total{format=\"webp\"} ",
                  s.webp_generated.load(), "\n");
  absl::StrAppend(&m, "pagespeed_format_generated_total{format=\"avif\"} ",
                  s.avif_generated.load(), "\n");
  absl::StrAppend(&m, "pagespeed_format_generated_total{format=\"jpeg\"} ",
                  s.jpeg_optimized.load(), "\n");
  absl::StrAppend(&m, "pagespeed_format_generated_total{format=\"png\"} ",
                  s.png_optimized.load(), "\n");

  // Compressed variants
  absl::StrAppend(&m,
                  "# HELP pagespeed_compressed_variants_total "
                  "Pre-compressed variants.\n");
  absl::StrAppend(&m, "# TYPE pagespeed_compressed_variants_total counter\n");
  absl::StrAppend(&m, "pagespeed_compressed_variants_total{encoding=\"gzip\"} ",
                  s.gzip_variants_written.load(), "\n");
  absl::StrAppend(&m,
                  "pagespeed_compressed_variants_total{encoding=\"brotli\"} ",
                  s.brotli_variants_written.load(), "\n");

  // Gauges
  gauge("pagespeed_cache_entries", "Cache entry count.", in.cache_entries);
  gauge("pagespeed_cache_bytes", "Cache size in bytes.", in.cache_bytes);
  gauge("pagespeed_connections_active", "Active connections.",
        in.active_connections);
  gauge("pagespeed_connections_max", "Max connections.", in.max_connections);
  gauge("pagespeed_thread_pool_inflight", "In-flight thread pool items.",
        in.in_flight_work);

  // SSIMULACRA2
  counter("pagespeed_ssimulacra2_checks_total", "SSIMULACRA2 checks.",
          s.ssimulacra2_checks.load());
  counter("pagespeed_ssimulacra2_reencodes_total", "SSIMULACRA2 re-encodes.",
          s.ssimulacra2_reencodes.load());
  counter("pagespeed_ssimulacra2_declines_total",
          "Variants refused by SSIMULACRA2 verification.",
          s.ssimulacra2_declines.load());
  counter("pagespeed_ssimulacra2_decline_tombstone_hits_total",
          "Variant recomputes skipped by a decline tombstone for the same "
          "source.",
          s.ssimulacra2_decline_tombstone_hits.load());

  counter("pagespeed_selector_invocations_total", "ReadBestAlternate calls.",
          s.selector_invocations.load());
  counter("pagespeed_source_reads_from_durable_original_total",
          "Optimization-source reads answered by the durable original "
          "(exact-id fallback) after the selector found nothing.",
          s.source_reads_from_durable_original.load());

  // Cache read retries (cross-process mmap propagation)
  counter("pagespeed_cache_read_retries_total",
          "Cache reads that succeeded after retry.",
          s.cache_read_retries.load());
  counter("pagespeed_cache_read_failures_total",
          "Cache reads where all retries were exhausted.",
          s.cache_read_failures.load());
  counter("pagespeed_cache_read_deferred_retries_total",
          "Deferred re-notifications scheduled after cache read failure.",
          s.cache_read_deferred_retries.load());
  counter("pagespeed_cache_read_deferred_successes_total",
          "Deferred re-notifications that succeeded on retry.",
          s.cache_read_deferred_successes.load());
  counter("pagespeed_read_borrow_wrap_discards_total",
          "Disk-borrow copies discarded because a write-buffer wrap raced "
          "the copy (read treated as a cache miss; issue #934).",
          s.read_borrow_wrap_discards.load());
  counter("pagespeed_cache_auto_heals_total",
          "URLs auto-purged after persistent cache read failures.",
          s.cache_auto_heals.load());
  counter("pagespeed_cache_auto_heal_exhausted_total",
          "Auto-heal attempts blocked by per-URL rate limit.",
          s.cache_auto_heal_exhausted.load());
  counter("pagespeed_image_incomplete_matrices_total",
          "Image variant matrices that were incomplete after processing.",
          s.image_incomplete_matrices.load());
  counter(
      "pagespeed_image_no_savings_skipped_total",
      "Image variants skipped because optimization produced no size savings.",
      s.image_no_savings_skipped.load());
  counter("pagespeed_image_unconverted_fallthrough_total",
          "Converted-format variants refused because the fall-through "
          "produced no conversion.",
          s.image_unconverted_fallthrough.load());
  counter("pagespeed_image_savedata_refused_total",
          "Save-Data image variants that were not smaller than the same "
          "variant without Save-Data and so were not stored.",
          s.image_savedata_refused.load());
  counter("pagespeed_image_size_refused_webp_total",
          "WebP images that were not smaller than the original-format image "
          "of their slot, including refusals against a stored image.",
          s.image_size_refused_webp.load());
  counter("pagespeed_image_size_refused_avif_total",
          "AVIF images that were not smaller than the original-format image "
          "of their slot, including refusals against a stored image.",
          s.image_size_refused_avif.load());
  counter("pagespeed_image_budget_rescued_webp_total",
          "Refused WebP images a smaller encode at the same quality floor "
          "recovered.",
          s.image_budget_rescued_webp.load());
  counter("pagespeed_image_budget_rescued_avif_total",
          "Refused AVIF images a smaller encode at the same quality floor "
          "recovered.",
          s.image_budget_rescued_avif.load());

  // Content dedup
  counter("pagespeed_dedup_writes_skipped_total",
          "Variant writes skipped due to identical content under another ID.",
          s.dedup_writes_skipped.load());
  counter("pagespeed_content_hash_hits_total",
          "Image notifications skipped due to unchanged origin content hash.",
          s.content_hash_hits.load());
  counter("pagespeed_content_hash_stale_total",
          "Image notifications where origin content changed (stale hash).",
          s.content_hash_stale.load());

  // Alternate write outcomes (Issue E: fenced split out from hard failures).
  counter("pagespeed_alternate_writes_total", "Alternate write attempts.",
          s.alternate_writes.load());
  counter("pagespeed_alternate_write_failures_total",
          "Hard alternate write failures (mmap/cache errors only).",
          s.alternate_write_failures.load());
  counter("pagespeed_alternate_writes_fenced_total",
          "Alternate writes benignly dropped by a post-dispatch purge fence.",
          s.alternate_writes_fenced.load());

  // Origin-refresh activity (Issue E: previously unexported).
  counter("pagespeed_origin_refresh_purges_total",
          "Origin-refresh sentinels that purged a URL's stale variant set.",
          s.origin_refresh_purges.load());
  counter("pagespeed_origin_refresh_rate_limited_total",
          "Origin-refresh sentinels rate-limited away (per-URL interval).",
          s.origin_refresh_rate_limited.load());
  counter("pagespeed_origin_refresh_rebuild_refused_total",
          "Origin-refresh inline rebuilds refused because the identity slot "
          "held a worker-processed (stale) variant, not the refreshed "
          "origin; the next record+notify rebuilds from fresh bytes.",
          s.origin_refresh_rebuild_refused.load());
  counter("pagespeed_notifications_origin_rechecked_total",
          "Notifications whose recorded original differed in origin state "
          "from the stored variant set and were checked by content hash.",
          s.notifications_origin_rechecked.load());
  counter("pagespeed_origin_change_rebuilt_total",
          "Variant sets rebuilt because the front end recorded changed "
          "origin content.",
          s.origin_change_rebuilt.load());
  counter("pagespeed_origin_unchanged_restamped_total",
          "Re-recorded originals with identical content whose variant set "
          "was only restamped.",
          s.origin_unchanged_restamped.load());

  // Learned quality prediction
  counter("pagespeed_learned_quality_predictions_total",
          "ML quality predictions used.", s.learned_quality_predictions.load());
  counter("pagespeed_learned_quality_fallbacks_total",
          "ML quality prediction fallbacks to heuristic.",
          s.learned_quality_fallbacks.load());

  // Quality baselining
  counter("pagespeed_quality_capped_jpeg_total",
          "JPEG quality capped to source quality.",
          s.quality_capped_jpeg.load());
  counter("pagespeed_quality_skip_reencode_total",
          "JPEG re-encodes skipped (source <= target).",
          s.quality_skip_reencode.load());

  // Content analysis
  absl::StrAppend(
      &m, "# HELP pagespeed_content_analysis_total Content classifications.\n");
  absl::StrAppend(&m, "# TYPE pagespeed_content_analysis_total counter\n");
  absl::StrAppend(&m, "pagespeed_content_analysis_total{class=\"photo\"} ",
                  s.content_photo.load(), "\n");
  absl::StrAppend(&m, "pagespeed_content_analysis_total{class=\"screenshot\"} ",
                  s.content_screenshot.load(), "\n");
  absl::StrAppend(&m,
                  "pagespeed_content_analysis_total{class=\"illustration\"} ",
                  s.content_illustration.load(), "\n");
  absl::StrAppend(&m, "pagespeed_content_analysis_total{class=\"noisy\"} ",
                  s.content_noisy.load(), "\n");
  counter("pagespeed_images_denoised_total",
          "Images denoised (always 0; the denoise step was removed).", 0);

  // SVG auto-vectorization
  counter("pagespeed_svg_candidates_evaluated_total",
          "SVG vectorization candidates evaluated.",
          s.svg_candidates_evaluated.load());
  counter("pagespeed_svg_candidates_rejected_total",
          "SVG vectorization candidates rejected.",
          s.svg_candidates_rejected.load());
  counter("pagespeed_svg_vectorized_total", "Images vectorized to SVG.",
          s.svg_vectorized.load());
  counter("pagespeed_svg_written_total", "SVG variants written to cache.",
          s.svg_written.load());
  // served = serve-time SVG HITs from the cross-process ServeStats mmap (the
  // WorkerStats counter was unwritable / always 0 — follow-up #455).
  counter("pagespeed_svg_served_total", "SVG variants served.",
          in.serve_stats != nullptr
              ? *reinterpret_cast<const volatile uint64_t*>(
                    &in.serve_stats->svg_optimized_hits)
              : 0);
  counter("pagespeed_svg_bytes_saved_total",
          "Total bytes saved by SVG vectorization.", s.svg_bytes_saved.load());
  counter("pagespeed_svg_fidelity_rejected_total",
          "SVG candidates rejected for fidelity.",
          s.svg_fidelity_rejected.load());
  counter("pagespeed_svg_timeout_exceeded_total",
          "SVG candidates discarded for exceeding timeout.",
          s.svg_timeout_exceeded.load());
  counter("pagespeed_svg_size_rejected_total",
          "SVG candidates rejected for size.", s.svg_size_rejected.load());
  counter("pagespeed_svg_path_count_rejected_total",
          "SVG candidates rejected for path count.",
          s.svg_path_count_rejected.load());
  absl::StrAppend(&m,
                  "# HELP pagespeed_svg_vectorize_time_seconds_total "
                  "Cumulative SVG vectorization time in seconds.\n"
                  "# TYPE pagespeed_svg_vectorize_time_seconds_total counter\n"
                  "pagespeed_svg_vectorize_time_seconds_total ",
                  static_cast<double>(s.svg_vectorize_time_us.load()) / 1e6,
                  "\n");

  // Optimization policy
  counter("pagespeed_policy_computed_total", "Optimization policies computed.",
          s.policy_computed.load());
  counter("pagespeed_policy_async_css_enabled_total",
          "Times async CSS was enabled by policy.",
          s.policy_async_css_enabled.load());
  counter("pagespeed_async_css_suppressed_low_coverage_total",
          "Times async CSS deferral was suppressed by the FOUC sufficiency "
          "gate (critical CSS too thin to bridge first paint).",
          s.async_css_suppressed_low_coverage.load());
  counter("pagespeed_async_css_suppressed_unvalidated_total",
          "Times async CSS deferral was suppressed because the page has no "
          "validation record bound to the stylesheet being served.",
          s.async_css_suppressed_unvalidated.load());
  counter("pagespeed_async_css_record_dropped_empty_derivation_total",
          "Times a browser profile's validation record was dropped because no "
          "critical block could be derived for the page being served.",
          s.async_css_record_dropped_empty_derivation.load());
  counter("pagespeed_policy_script_deferral_enabled_total",
          "Times script deferral was enabled by policy.",
          s.policy_script_deferral_enabled.load());

  // HTML critical CSS assembly outcomes.
  absl::StrAppend(&m,
                  "# HELP pagespeed_html_assembly_total "
                  "HTML critical CSS assembly outcomes.\n"
                  "# TYPE pagespeed_html_assembly_total counter\n"
                  "pagespeed_html_assembly_total{result=\"complete\"} ",
                  s.html_assembly_complete.load(),
                  "\npagespeed_html_assembly_total{result=\"skipped\"} ",
                  s.html_assembly_skipped.load(),
                  "\npagespeed_html_assembly_total{result=\"css_aborted\"} ",
                  s.critical_css_aborted.load(), "\n");

  // Browser analysis (omitted when disabled/failed to initialize).
  if (in.browser_manager != nullptr) {
    const auto& bs = in.browser_manager->stats();
    absl::StrAppend(
        &m,
        "# HELP pagespeed_browser_profiles_generated_total "
        "Browser optimization profiles generated.\n"
        "# TYPE pagespeed_browser_profiles_generated_total counter\n"
        "pagespeed_browser_profiles_generated_total ",
        bs.profiles_generated.load(),
        "\n# HELP pagespeed_browser_profiles_used_total "
        "Browser profiles used for HTML optimization.\n"
        "# TYPE pagespeed_browser_profiles_used_total counter\n"
        "pagespeed_browser_profiles_used_total ",
        bs.profiles_used.load(),
        "\n# HELP pagespeed_browser_analysis_errors_total "
        "Browser analysis errors.\n"
        "# TYPE pagespeed_browser_analysis_errors_total counter\n"
        "pagespeed_browser_analysis_errors_total ",
        bs.analysis_errors.load(),
        "\n# HELP pagespeed_browser_chrome_crashes_total "
        "Chrome process crashes.\n"
        "# TYPE pagespeed_browser_chrome_crashes_total counter\n"
        "pagespeed_browser_chrome_crashes_total ",
        bs.chrome_crashes.load(),
        "\n# HELP pagespeed_browser_queue_depth "
        "Current browser analysis queue depth.\n"
        "# TYPE pagespeed_browser_queue_depth gauge\n"
        "pagespeed_browser_queue_depth ",
        in.browser_manager->queue_depth(),
        "\n# HELP pagespeed_browser_css_inlining_attempted_total "
        "CSS inlining attempts during browser analysis.\n"
        "# TYPE pagespeed_browser_css_inlining_attempted_total counter\n"
        "pagespeed_browser_css_inlining_attempted_total ",
        bs.css_inlining_attempted.load(),
        "\n# HELP pagespeed_browser_css_inlining_stylesheets_found_total "
        "Stylesheet link tags found during CSS inlining.\n"
        "# TYPE pagespeed_browser_css_inlining_stylesheets_found_total "
        "counter\n"
        "pagespeed_browser_css_inlining_stylesheets_found_total ",
        bs.css_inlining_stylesheets_found.load(),
        "\n# HELP pagespeed_browser_css_inlining_stylesheets_cached_total "
        "Stylesheets successfully inlined from cache.\n"
        "# TYPE pagespeed_browser_css_inlining_stylesheets_cached_total "
        "counter\n"
        "pagespeed_browser_css_inlining_stylesheets_cached_total ",
        bs.css_inlining_stylesheets_cached.load(),
        "\n# HELP pagespeed_browser_css_inlining_bytes_total "
        "Total CSS bytes inlined into HTML.\n"
        "# TYPE pagespeed_browser_css_inlining_bytes_total counter\n"
        "pagespeed_browser_css_inlining_bytes_total ",
        bs.css_inlining_bytes_inlined.load(),
        "\n# HELP pagespeed_browser_reanalyses_scheduled_total "
        "Re-analyses scheduled due to 0% critical CSS coverage.\n"
        "# TYPE pagespeed_browser_reanalyses_scheduled_total counter\n"
        "pagespeed_browser_reanalyses_scheduled_total ",
        bs.reanalyses_scheduled.load(), "\n");
  }

  // Serve-time bandwidth savings (from nginx shared mmap).
  if (in.serve_stats != nullptr) {
    const auto* ss = in.serve_stats;
    // Aligned 64-bit loads are atomic on x86-64 (both GCC/Clang and MSVC).
    auto load = [](const uint64_t& field) -> uint64_t {
      return *reinterpret_cast<const volatile uint64_t*>(&field);
    };
    absl::StrAppend(
        &m,
        "# HELP pagespeed_original_bytes_served_total "
        "Cumulative original content bytes for worker-optimized cache HITs.\n"
        "# TYPE pagespeed_original_bytes_served_total counter\n"
        "pagespeed_original_bytes_served_total{type=\"html\"} ",
        load(ss->html_original_bytes),
        "\npagespeed_original_bytes_served_total{type=\"css\"} ",
        load(ss->css_original_bytes),
        "\npagespeed_original_bytes_served_total{type=\"js\"} ",
        load(ss->js_original_bytes),
        "\npagespeed_original_bytes_served_total{type=\"image\"} ",
        load(ss->image_original_bytes),
        "\n# HELP pagespeed_optimized_bytes_served_total "
        "Cumulative optimized content bytes served for worker-optimized cache "
        "HITs.\n"
        "# TYPE pagespeed_optimized_bytes_served_total counter\n"
        "pagespeed_optimized_bytes_served_total{type=\"html\"} ",
        load(ss->html_optimized_bytes),
        "\npagespeed_optimized_bytes_served_total{type=\"css\"} ",
        load(ss->css_optimized_bytes),
        "\npagespeed_optimized_bytes_served_total{type=\"js\"} ",
        load(ss->js_optimized_bytes),
        "\npagespeed_optimized_bytes_served_total{type=\"image\"} ",
        load(ss->image_optimized_bytes),
        "\n# HELP pagespeed_optimized_hits_served_total "
        "Cumulative cache HIT count for worker-optimized variants.\n"
        "# TYPE pagespeed_optimized_hits_served_total counter\n"
        "pagespeed_optimized_hits_served_total{type=\"html\"} ",
        load(ss->html_optimized_hits),
        "\npagespeed_optimized_hits_served_total{type=\"css\"} ",
        load(ss->css_optimized_hits),
        "\npagespeed_optimized_hits_served_total{type=\"js\"} ",
        load(ss->js_optimized_hits),
        "\npagespeed_optimized_hits_served_total{type=\"image\"} ",
        load(ss->image_optimized_hits), "\n");
    // The split of the saving, over the serves whose encoded-origin size was
    // known: origin bytes, those bytes encoded as served, bytes served.
    struct SplitSeries {
      const char* name;
      const char* help;
      uint64_t ServeStats::SplitCounters::* field;
    };
    for (const SplitSeries& series :
         {SplitSeries{"pagespeed_split_original_bytes_served_total",
                      "Original content bytes of the HITs whose encoded size "
                      "was known.",
                      &ServeStats::SplitCounters::original_bytes},
          SplitSeries{"pagespeed_split_compressed_original_bytes_served_total",
                      "Those original bytes encoded the way each HIT was "
                      "served.",
                      &ServeStats::SplitCounters::compressed_original_bytes},
          SplitSeries{"pagespeed_split_optimized_bytes_served_total",
                      "Bytes served for those HITs.",
                      &ServeStats::SplitCounters::optimized_bytes}}) {
      absl::StrAppend(&m, "# HELP ", series.name, " ", series.help, "\n",
                      "# TYPE ", series.name, " counter\n");
      for (const auto& [label, type] :
           {std::pair{"html", ContentType::kHtml},
            std::pair{"css", ContentType::kCss},
            std::pair{"js", ContentType::kJs},
            std::pair{"image", ContentType::kImage}}) {
        absl::StrAppend(&m, series.name, "{type=\"", label, "\"} ",
                        ReadServeSplit(ss, type).*series.field, "\n");
      }
    }
    // Serve savings by host: the hosts /v1/stats names, the rest under
    // host="(other)" -- a value no host name can take, so it never collides.
    const ServeSavingsByHost by_host =
        ReadServeSavingsByHost(ss, kServeHostReportLimit);
    struct HostSeries {
      const char* name;
      const char* help;
      uint64_t ServeHostRow::* field;
    };
    for (const HostSeries& series :
         {HostSeries{"pagespeed_host_hits_served_total",
                     "Optimized cache HITs served, per host.",
                     &ServeHostRow::hits},
          HostSeries{"pagespeed_host_original_bytes_served_total",
                     "Original content bytes of those HITs, per host.",
                     &ServeHostRow::original_bytes},
          HostSeries{"pagespeed_host_optimized_bytes_served_total",
                     "Bytes served from cache for those HITs, per host.",
                     &ServeHostRow::optimized_bytes}}) {
      absl::StrAppend(&m, "# HELP ", series.name, " ", series.help,
                      " Up to 32 hosts, drawn from the first 64 distinct "
                      "hosts seen since the optimizer started, most HITs "
                      "first; the rest, and HITs recorded without a host "
                      "name, under host=\"(other)\". A host name is "
                      "lowercase, without a port or a trailing dot: "
                      "letters, digits, '.', '-' and '_', or a bracketed "
                      "IPv6 literal, at most 127 bytes.\n",
                      "# TYPE ", series.name, " counter\n");
      for (const ServeHostRow& row : by_host.hosts) {
        absl::StrAppend(&m, series.name, "{host=\"", row.host, "\"} ",
                        row.*series.field, "\n");
      }
      absl::StrAppend(&m, series.name, "{host=\"(other)\"} ",
                      by_host.other.*series.field, "\n");
    }
    // Web Bot Auth (observe-only): request-time verdicts for
    // signed requests, incremented by the front-end at classify time.
    absl::StrAppend(
        &m,
        "# HELP pagespeed_webbotauth_signed_requests_total "
        "Requests carrying an RFC 9421 signature, by verification result.\n"
        "# TYPE pagespeed_webbotauth_signed_requests_total counter\n"
        "pagespeed_webbotauth_signed_requests_total{result=\"verified\"} ",
        load(ss->webbotauth_signed_verified),
        "\npagespeed_webbotauth_signed_requests_total{result=\"invalid\"} ",
        load(ss->webbotauth_signed_invalid),
        // result="other": signature material with no web-bot-auth-tagged
        // member (classified as if unsigned, never "invalid").
        "\npagespeed_webbotauth_signed_requests_total{result=\"other\"} ",
        load(ss->webbotauth_other_signature), "\n");
    // Web Bot Auth opt-in counter (experimental): per-signer
    // decomposition of the verified total and a coarse verify-latency
    // histogram.  FIRST-PARTY ONLY (this /v1/metrics surface) — never on the
    // public well-known counter doc.  The per-signer slots key on an unsalted,
    // reproducible FNV-1a-64 keyid hash; resolve names from the operator
    // verified-bot registry, else fall back to the opaque hash.  Parse
    // "keyid=name,keyid2=name2".
    {
      // hash -> name from the operator registry.
      std::vector<std::pair<uint64_t, std::string>> name_by_hash;
      std::string_view spec(in.web_bot_auth_verified_bots);
      size_t p = 0;
      while (p < spec.size()) {
        size_t comma = spec.find(',', p);
        std::string_view entry = spec.substr(p, comma == std::string_view::npos
                                                    ? std::string_view::npos
                                                    : comma - p);
        size_t eq = entry.find('=');
        if (eq != std::string_view::npos) {
          std::string_view kid = entry.substr(0, eq);
          std::string_view nm = entry.substr(eq + 1);
          if (!kid.empty()) {
            name_by_hash.emplace_back(HashWebBotAuthKeyid(kid),
                                      std::string(nm));
          }
        }
        if (comma == std::string_view::npos) break;
        p = comma + 1;
      }
      auto resolve = [&](uint64_t h) -> std::string {
        for (const auto& [hh, nm] : name_by_hash) {
          if (hh == h) return nm;
        }
        // Opaque fallback: 16 lowercase hex of the unsalted keyid hash.
        static const char* kHex = "0123456789abcdef";
        std::string s = "hash:";
        for (int shift = 60; shift >= 0; shift -= 4) {
          s += kHex[(h >> shift) & 0x0F];
        }
        return s;
      };
      // Sanitize a signer name for a Prometheus label value: escape \\, ", and
      // drop control chars; cap length.  Operator free text -> defensive.
      auto label = [](std::string_view v) -> std::string {
        std::string out;
        size_t n = v.size() > 64 ? 64 : v.size();
        for (size_t i = 0; i < n; ++i) {
          unsigned char c = static_cast<unsigned char>(v[i]);
          if (c == '\\') {
            out += "\\\\";
          } else if (c == '"') {
            out += "\\\"";
          } else if (c == '\n') {
            out += "\\n";
          } else if (c < 0x20 || c == 0x7f) {
            continue;
          } else {
            out += static_cast<char>(c);
          }
        }
        return out;
      };
      absl::StrAppend(&m,
                      "# HELP pagespeed_webbotauth_verified_signer_total "
                      "Verified Web Bot Auth requests by signer.\n"
                      "# TYPE pagespeed_webbotauth_verified_signer_total "
                      "counter\n");
      // Sum counts across slots that resolve to the SAME label before emitting:
      // an operator can map two keyids to one friendly name (common during key
      // rotation), and two lines sharing an identical label set would make
      // Prometheus reject the whole scrape as a duplicate series.  One line per
      // distinct label; preserve first-seen order for stable output.
      std::vector<std::pair<std::string, uint64_t>> by_label;
      for (const auto& slot : ss->webbotauth_signers) {
        uint64_t h = load(slot.kid_hash);
        if (h == 0) continue;
        std::string lbl = label(resolve(h));
        uint64_t cnt = load(slot.count);
        bool merged = false;
        for (auto& [existing, total] : by_label) {
          if (existing == lbl) {
            total += cnt;
            merged = true;
            break;
          }
        }
        if (!merged) by_label.emplace_back(std::move(lbl), cnt);
      }
      for (const auto& [lbl, total] : by_label) {
        absl::StrAppend(&m,
                        "pagespeed_webbotauth_verified_signer_total{signer=\"",
                        lbl, "\"} ", total, "\n");
      }
      absl::StrAppend(
          &m, "pagespeed_webbotauth_verified_signer_total{signer=\"other\"} ",
          load(ss->webbotauth_other_verified_bots), "\n");

      // Coarse verify-latency histogram (cumulative buckets; _sum omitted — the
      // engine stores disjoint bucket counts, not the latency sum).
      uint64_t l1 = load(ss->webbotauth_verify_latency_lt100us);
      uint64_t l2 = load(ss->webbotauth_verify_latency_lt1ms);
      uint64_t l3 = load(ss->webbotauth_verify_latency_lt10ms);
      uint64_t l4 = load(ss->webbotauth_verify_latency_ge10ms);
      uint64_t c1 = l1;
      uint64_t c2 = c1 + l2;
      uint64_t c3 = c2 + l3;
      uint64_t c4 = c3 + l4;
      absl::StrAppend(
          &m,
          "# HELP pagespeed_webbotauth_verify_latency_seconds "
          "Web Bot Auth signature verify latency.\n"
          "# TYPE pagespeed_webbotauth_verify_latency_seconds histogram\n"
          "pagespeed_webbotauth_verify_latency_seconds_bucket{le=\"0.0001\"} ",
          c1,
          "\npagespeed_webbotauth_verify_latency_seconds_bucket{le=\"0.001\"} ",
          c2,
          "\npagespeed_webbotauth_verify_latency_seconds_bucket{le=\"0.01\"} ",
          c3,
          "\npagespeed_webbotauth_verify_latency_seconds_bucket{le=\"+Inf\"} ",
          c4, "\npagespeed_webbotauth_verify_latency_seconds_count ", c4, "\n");
    }
    // RSL-CAP enforcement (experimental): request-time verdicts,
    // incremented by the front-end when the enforcement handler is enabled.
    absl::StrAppend(
        &m,
        "# HELP pagespeed_rslcap_requests_total "
        "Requests evaluated by the RSL-CAP enforcement handler, by verdict.\n"
        "# TYPE pagespeed_rslcap_requests_total counter\n"
        "pagespeed_rslcap_requests_total{verdict=\"authorized\"} ",
        load(ss->rslcap_authorized),
        "\npagespeed_rslcap_requests_total{verdict=\"denied_401\"} ",
        load(ss->rslcap_denied_401),
        "\npagespeed_rslcap_requests_total{verdict=\"denied_402\"} ",
        load(ss->rslcap_denied_402), "\n");
    // Zero-copy serve-barrier verdicts (nginx shared mmap).
    counter("pagespeed_zerocopy_torn_aborts_total",
            "Aliased zero-copy HIT serves failed closed (RST) because a strict "
            "lease renew saw a committed write-buffer wrap (torn borrow); no "
            "foreign bytes were served.",
            load(ss->zerocopy_torn_aborts));
    counter("pagespeed_zerocopy_proactive_copyouts_total",
            "Aliased HIT tails de-aliased into owned memory ahead of a wrap "
            "(wrap-pressure copy-out).",
            load(ss->zerocopy_proactive_copyouts));
    counter("pagespeed_zerocopy_copy_then_verify_discards_total",
            "De-alias copies discarded because a wrap raced the copy and the "
            "post-copy re-check saw a torn borrow (serve failed closed).",
            load(ss->zerocopy_copy_then_verify_discards));
    // Bounded stale-serve telemetry (issue #652).
    counter("pagespeed_swr_coalesced_serves_total",
            "Stale-while-revalidate serves coalesced onto an in-flight "
            "re-fetch.",
            load(ss->swr_coalesced_serves));
    counter("pagespeed_stale_if_error_serves_total",
            "Stale entries served because the upstream returned >= 500.",
            load(ss->stale_if_error_serves));
    // Response-header sidecar (nginx shared mmap, v11).
    counter("pagespeed_headers_sidecar_fall_through_total",
            "Responses not cached because an origin header cannot be "
            "reproduced on a cache hit.",
            load(ss->headers_sidecar_fall_through));
    counter("pagespeed_headers_sidecar_never_optimized_total",
            "Responses not cached because their Content-Security-Policy "
            "carries a nonce.",
            load(ss->headers_sidecar_never_optimized));
    counter("pagespeed_headers_sidecar_replayed_total",
            "Cache hits that replayed stored origin headers.",
            load(ss->headers_sidecar_replayed));
    counter("pagespeed_headers_sidecar_missing_total",
            "Cache hits served as a miss because the stored origin headers "
            "could not be read.",
            load(ss->headers_sidecar_missing));
    counter("pagespeed_headers_sidecar_write_failed_total",
            "Records abandoned because the origin headers could not be "
            "stored.",
            load(ss->headers_sidecar_write_failed));
    counter("pagespeed_headers_sidecar_stored_total",
            "Origin header blocks stored with a cache entry.",
            load(ss->headers_sidecar_stored));
    // Busy cache reads (shared mmap, busy-read block).
    counter("pagespeed_cache_busy_reads_total",
            "Cache reads that found the entry locked by a concurrent write "
            "and were retried once.",
            load(ss->cache_busy_reads));
    counter("pagespeed_cache_busy_retry_hits_total",
            "Retried busy cache reads that then found the entry.",
            load(ss->cache_busy_retry_hits));
  }

  return m;
}

static HttpResponse HandleMetrics(ApiContext& ctx,
                                  const HttpRequest& /*request*/) {
  std::string verified_bots;
  if (ctx.get_config) {
    if (auto cfg = ctx.get_config()) {
      verified_bots = cfg->web_bot_auth_verified_bots;
    }
  }
  PrometheusMetricsInputs in{
      .stats = ctx.stats,
      .cache_entries = ctx.cache_entries(),
      .cache_bytes = ctx.cache_bytes(),
      .active_connections = ctx.http_active_connections(),
      .max_connections = ctx.max_connections,
      .in_flight_work = ctx.in_flight_work(),
      .browser_manager = ctx.browser_manager ? ctx.browser_manager() : nullptr,
      .serve_stats = ctx.serve_stats,
      .web_bot_auth_verified_bots = verified_bots,
  };
  return HttpResponse()
      .ContentType("text/plain; version=0.0.4; charset=utf-8")
      .Body(BuildPrometheusMetricsText(in));
}

// ---------------------------------------------------------------------------
// GET /v1/config
// ---------------------------------------------------------------------------

static json ConfigToJson(const WorkerConfig& c) {
  return ConfigToJsonPersistable(c);
}

static HttpResponse HandleConfigGet(ApiContext& ctx,
                                    const HttpRequest& /*request*/) {
  auto config = ctx.get_config();
  return HttpResponse().Json(DumpJson(ConfigToJson(*config)));
}

// ---------------------------------------------------------------------------
// PATCH /v1/config
// ---------------------------------------------------------------------------

// Thread-safe text for an errno value (the cause named in a save warning).
static std::string ErrnoMessage(int err) {
  if (err == 0) return "unknown error";
  return std::error_code(err, std::generic_category()).message();
}

// Everything a PATCH computes before any disk work.  Built on the event-loop
// thread; the config it applied is already live in memory.
struct ConfigPatchState {
  // Set when the request was rejected outright (bad JSON, wrong type).
  std::optional<HttpResponse> error;
  std::shared_ptr<const WorkerConfig> current;
  std::shared_ptr<const WorkerConfig> updated;
  json applied;
  json rejected;
  json warnings;
  // Disk work is needed: something was applied and there is a cache path.
  bool persist = false;
  // The change touches a field the shared (module-read) file carries.
  bool shared_dirty = false;
};

// Writes pagespeed-shared.conf for `updated`.  READ-MODIFY-WRITE: it starts
// from the file's current contents so fields owned by OTHER writers/startup
// (web_bot_auth*, rsl_cap*) survive an unrelated runtime PATCH instead of
// being silently reset to defaults until the next worker restart.  Touches
// disk, so it runs on the persister thread when there is one.
// Returns 0 on success, else the errno-style cause, read right at the failing
// call (before this function's locals are destroyed).
static int WriteSharedConfigFor(const WorkerConfig& updated) {
  SharedConfig shared =
      ReadSharedConfigFile(SharedConfigFilePath(updated.cache_path));
  // socket_path is non-reloadable so it is not checked for a change, but it
  // is always written to the file since nginx needs the current value.
  shared.socket_path = updated.socket_path;
  shared.html_mode = updated.html_mode;
  // agent_optimize: the serve-side toggle is the operator flag.
  // agent_optimize is a startup flag (not PATCH-able), so this carries it
  // forward unchanged.
  shared.agent_optimize_entitled = updated.browser_analysis.agent_optimize;
  // Recompute the /llms.txt serve gate alongside it (previously this writer
  // dropped it to false).
  shared.agent_optimize_llms_txt_enabled =
      shared.agent_optimize_entitled &&
      updated.browser_analysis.agent_optimize_llms_txt;
  shared.cache_mode = updated.cache_mode;
  // Rebuild expanded extensions (non-reloadable, but must be preserved).
  {
    std::unordered_set<std::string> exts;
    for (const auto& ext : updated.strip_query_extensions) {
      exts.insert(ext);
    }
    for (const auto& group : updated.strip_query_groups) {
      for (const auto& ext : ExpandExtensionGroup(group)) {
        exts.insert(ext);
      }
    }
    std::string csv;
    for (const auto& ext : exts) {
      if (!csv.empty()) csv += ',';
      csv += ext;
    }
    shared.strip_query_extensions = std::move(csv);
  }
  {
    std::string csv;
    for (const auto& param : updated.strip_query_params) {
      if (!csv.empty()) csv += ',';
      csv += param;
    }
    shared.strip_query_params = std::move(csv);
  }
  if (WriteSharedConfigFile(SharedConfigFilePath(updated.cache_path), shared)) {
    return 0;
  }
  return errno != 0 ? errno : EIO;
}

// Steps that never touch the disk: parse, validate, apply in memory.
static ConfigPatchState PrepareConfigPatch(ApiContext& ctx,
                                           const HttpRequest& request) {
  // Serialise read-copy-update so concurrent PATCH calls never lose
  // updates.  Currently single-threaded (libuv event loop), but the
  // lock makes the invariant explicit for future threading changes.
  static std::mutex mu;
  std::lock_guard<std::mutex> lock(mu);

  ConfigPatchState st;
  if (JsonNestingTooDeep(request.body)) {
    st.error =
        HttpResponse::Error(ApiErrorCode::kBadRequest, "JSON nesting too deep");
    return st;
  }

  json body;
  try {
    body = json::parse(request.body);
  } catch (const json::exception&) {
    st.error =
        HttpResponse::Error(ApiErrorCode::kBadRequest, "Invalid JSON body");
    return st;
  }

  if (!body.is_object()) {
    st.error = HttpResponse::Error(ApiErrorCode::kBadRequest,
                                   "Request body must be a JSON object");
    return st;
  }

  st.current = ctx.get_config();
  auto updated = std::make_shared<WorkerConfig>(*st.current);

  // Apply all hot-reloadable fields via shared validation logic.
  auto cr = ApplyConfigJson(body, updated.get());
  st.applied = std::move(cr.applied);
  st.rejected = std::move(cr.rejected);
  st.warnings = std::move(cr.warnings);

  // Retired licensing keys (license_key, license_renewal_url, the storefront
  // pair) arrive in `rejected` from ApplyConfigJson with a reason naming the
  // 2.1 change; nothing else to do for them here.

  // Apply the config if any fields changed.
  if (!st.applied.empty()) {
    ctx.update_config(updated);
  }

  st.persist = !st.applied.empty() && !updated->cache_path.empty();
  st.shared_dirty = st.persist && (st.applied.contains("html_mode") ||
                                   st.applied.contains("disable_html") ||
                                   st.applied.contains("cache_mode"));
  st.updated = std::move(updated);
  return st;
}

static HttpResponse FinishConfigPatch(ConfigPatchState& st) {
  json result;
  result["applied"] = st.applied;
  result["rejected"] = st.rejected;
  result["warnings"] = st.warnings;
  // Use the updated config directly to avoid a redundant lock acquisition.
  result["config"] =
      ConfigToJson(st.applied.empty() ? *st.current : *st.updated);
  return HttpResponse().Json(DumpJson(result));
}

// Disk-failure warnings, in the response's `warnings` array.
static void AddPersistWarnings(ConfigPatchState& st,
                               const ConfigPersister::Result& r) {
  if (!r.json_ok) {
    st.warnings.push_back("failed to write pagespeed.json: " + r.json_error);
  }
  if (r.shared_attempted && !r.shared_ok) {
    st.warnings.push_back("failed to write pagespeed-shared.conf: " +
                          r.shared_error);
  }
}

// Without a persister (no worker thread to hand the disk work to): save on the
// calling thread, as before.
static HttpResponse HandleConfigPatch(ApiContext& ctx,
                                      const HttpRequest& request) {
  ConfigPatchState st = PrepareConfigPatch(ctx, request);
  if (st.error.has_value()) return std::move(*st.error);

  ConfigPersister::Result r;
  if (st.persist) {
    const json snapshot = ConfigToJsonPersistable(*st.updated);
    const std::string json_path = ConfigFilePath(st.updated->cache_path);
    if (!WriteConfigFile(json_path, snapshot)) {
      const int err = errno != 0 ? errno : EIO;
      r.json_ok = false;
      r.json_error = ErrnoMessage(err);
    }
    if (st.shared_dirty) {
      r.shared_attempted = true;
      if (const int err = WriteSharedConfigFor(*st.updated); err != 0) {
        r.shared_ok = false;
        r.shared_error = ErrnoMessage(err);
      }
    }
  }
  AddPersistWarnings(st, r);
  return FinishConfigPatch(st);
}

// With a persister: the change is live as soon as PrepareConfigPatch returns,
// the disk work runs on the persister thread, and the response is sent only
// once the files are saved, so a 200 still means "saved".  The event loop is
// free in the meantime.
static void HandleConfigPatchAsync(ApiContext& ctx, const HttpRequest& request,
                                   Responder respond) {
  auto st =
      std::make_shared<ConfigPatchState>(PrepareConfigPatch(ctx, request));
  if (st->error.has_value()) {
    respond(std::move(*st->error));
    return;
  }
  if (!st->persist) {
    respond(FinishConfigPatch(*st));
    return;
  }

  ConfigPersister::Job job;
  const std::string json_path = ConfigFilePath(st->updated->cache_path);
  job.write_json = [json_path,
                    snapshot = ConfigToJsonPersistable(*st->updated)] {
    if (WriteConfigFile(json_path, snapshot)) return 0;
    return errno != 0 ? errno : EIO;
  };
  job.shared_dirty = st->shared_dirty;
  job.write_shared = [updated = st->updated] {
    return WriteSharedConfigFor(*updated);
  };
  MessageHandler* log = ctx.config_persister->handler();
  ctx.config_persister->Submit(
      std::move(job), [st, log, respond = std::move(respond)](
                          const ConfigPersister::Result& r) {
        // This runs inside a libuv callback: an exception must not escape.
        HttpResponse response;
        try {
          AddPersistWarnings(*st, r);
          response = FinishConfigPatch(*st);
        } catch (const std::exception& e) {
          if (log != nullptr) {
            log->Warning("config: building the PATCH response failed: %s",
                         e.what());
          }
          response = HttpResponse::Error(ApiErrorCode::kInternalError,
                                         "Internal server error");
        }
        respond(std::move(response));
      });
}

// ---------------------------------------------------------------------------
// GET /v1/logs?since=<seq>&limit=<n>
// ---------------------------------------------------------------------------

// Default and maximum entries per log page.  The ring owner also stops a
// page at kMaxLogsPageBytes, so a page of long lines is shorter.
static constexpr size_t kMaxLogReadLimit = 500;

// Reads a page of the worker's in-memory log ring (WsManager).  `since` is a
// sequence cursor: entries with seq > since, oldest first, so a burst larger
// than a page is paged through on later reads; without it, the newest
// entries.  Malformed numbers are a 400; an out-of-range limit clamps,
// mirroring /v1/cache/urls.  (absl::SimpleAtoi also accepts a leading '+'
// and surrounding whitespace, and an empty value reads as absent -- lenient
// like the other endpoints; the module proxy in front is strict.)
static HttpResponse HandleLogs(ApiContext& ctx, const HttpRequest& request) {
  const std::string_view since_str = request.QueryParam("since");
  const std::string_view limit_str = request.QueryParam("limit");

  uint64_t since = 0;
  bool has_since = false;
  if (!since_str.empty()) {
    if (!absl::SimpleAtoi(since_str, &since)) {
      return HttpResponse::Error(ApiErrorCode::kBadRequest,
                                 "Invalid 'since' parameter");
    }
    has_since = true;
  }

  size_t limit = kMaxLogReadLimit;
  if (!limit_str.empty()) {
    if (!absl::SimpleAtoi(limit_str, &limit)) {
      return HttpResponse::Error(ApiErrorCode::kBadRequest,
                                 "Invalid 'limit' parameter");
    }
  }
  if (limit == 0 || limit > kMaxLogReadLimit) limit = kMaxLogReadLimit;

  return HttpResponse().Json(DumpJson(ctx.read_logs(since, has_since, limit)));
}

// ---------------------------------------------------------------------------
// Route registration
// ---------------------------------------------------------------------------

void RegisterOperationalRoutes(HttpServer& server, ApiContext& ctx) {
  server.AddRoute("GET", "/v1/health", [&ctx](const HttpRequest& req) {
    return HandleHealth(ctx, req);
  });

  // These three are open under --api-read-open today (RouteAuth::kReadOpenOk
  // is the explicit allow-list entry preserving that); --api-read-open's
  // whole purpose is a public read-only console, and stats/metrics/config
  // are exactly its data.
  server.AddRoute(
      "GET", "/v1/stats",
      [&ctx](const HttpRequest& req) { return HandleStats(ctx, req); },
      RouteAuth::kReadOpenOk);

  server.AddRoute(
      "GET", "/v1/metrics",
      [&ctx](const HttpRequest& req) { return HandleMetrics(ctx, req); },
      RouteAuth::kReadOpenOk);

  server.AddRoute(
      "GET", "/v1/config",
      [&ctx](const HttpRequest& req) { return HandleConfigGet(ctx, req); },
      RouteAuth::kReadOpenOk);

  if (ctx.config_persister != nullptr) {
    server.AddAsyncRoute("PATCH", "/v1/config",
                         [&ctx](const HttpRequest& req, Responder respond) {
                           HandleConfigPatchAsync(ctx, req, std::move(respond));
                         });
  } else {
    server.AddRoute("PATCH", "/v1/config", [&ctx](const HttpRequest& req) {
      return HandleConfigPatch(ctx, req);
    });
  }

  // The log ring read.  Registered only when the Worker wired a ring
  // reader.  Log lines carry every host's URLs and error text, so on TCP
  // the token stays required even under --api-read-open -- this is simply
  // RouteAuth::kToken, the default, named explicitly here because it is the
  // one GET route that deliberately does NOT join the allow-list (the
  // /v1/ws/logs stream keeps the same rule, enforced inside the WS layer);
  // the unix socket and HEAD handling are the shared dispatch's, like every
  // GET /v1/* route.
  if (ctx.read_logs) {
    server.AddRoute(
        "GET", "/v1/logs",
        [&ctx](const HttpRequest& req) { return HandleLogs(ctx, req); },
        RouteAuth::kToken);
  }
}

}  // namespace pagespeed
