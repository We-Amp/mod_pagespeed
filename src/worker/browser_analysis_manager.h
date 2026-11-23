// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

// PageSpeed 2.0 - Browser Analysis Manager
//
// Owns Chrome lifecycle, analysis queue, template detector, and the CDP
// analysis pipeline.  Runs on the main libuv event loop (where CDP must
// operate).  Worker thread pool enqueues work; event loop drains it.
//
// When --enable-browser-analysis is set and Chrome is available, the
// manager generates per-template OptimizationProfiles and makes them
// available for higher-quality HTML variants.  When disabled or Chrome
// is unavailable, zero overhead -- the heuristic path is unchanged.

#ifndef PAGESPEED_SRC_WORKER_BROWSER_ANALYSIS_MANAGER_H_
#define PAGESPEED_SRC_WORKER_BROWSER_ANALYSIS_MANAGER_H_

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "lib/cache/cache.h"
#include "lib/classify/url_normalizer.h"
#include "src/browser/analysis_queue.h"
#include "src/browser/browser_css_extractor.h"
#include "src/browser/browser_sandbox.h"
#include "src/browser/chrome_process.h"
#include "src/browser/critical_css_validator.h"
#include "src/browser/device_emulation.h"
#include "src/browser/optimization_profile.h"
#include "src/browser/page_analysis.h"
#include "src/browser/script_coverage_analyzer.h"
#include "src/browser/template_detector.h"
#include "src/browser/visual_regression_gate.h"
#include "src/worker/browser_analysis_manager_internal.h"
#include "src/worker/cascade_layer_order.h"
#include "src/worker/html_scanner.h"
#include "src/worker/subresource_gate.h"
#include "uv.h"

namespace pagespeed {

class MessageHandler;

// Configuration for browser analysis (populated from CLI flags).
struct BrowserAnalysisConfig {
  bool enabled = false;
  std::string chrome_binary = "/usr/bin/chrome-headless-shell";
  // `require` (the default) never degrades: if the startup
  // probe says the sandbox is unavailable, browser analysis refuses to
  // initialize and the daemon keeps serving.  `off` is the deliberate,
  // documented escape hatch and re-adds --no-sandbox with a loud banner.
  BrowserSandboxMode sandbox_mode = BrowserSandboxMode::kRequire;
  // Set by the resolved sandbox mode; the manager never decides this itself.
  bool chrome_no_sandbox = false;
  // Chrome profile directory.  Pinned by the daemon (see
  // ResolveChromeUserDataDir) rather than left to Chrome, which would pick a
  // path of its own choosing against a ProtectSystem=strict sandbox.
  std::string chrome_user_data_dir;
  int chrome_recycle_interval = 100;
  int chrome_page_timeout_ms = 60000;
  int chrome_max_memory_mb = 512;
  int chrome_startup_timeout_ms = 10000;
  bool enable_browser_critical_css = true;
  bool enable_browser_lazy_loading = true;
  bool enable_browser_lcp_preload = true;
  bool enable_browser_image_sizing = true;
  bool enable_script_analysis = true;
  size_t browser_queue_size = 1000;
  int64_t browser_profile_ttl_seconds = 86400;  // 24h

  // The worker's cache-key URL normalization, copied at startup (the worker
  // builds it once in Initialize() and never mutates it afterwards).  The
  // analysis resource map must normalize its cache lookups exactly as the
  // worker normalizes store keys — a query-versioned src ("app.js?v=123")
  // would otherwise miss permanently, pinning its template on the shortened
  // cold-script TTL.
  UrlNormalizationConfig url_normalization;

  // agent_optimize: when true, the headless render fetches subresources
  // out-of-Chrome via the IP-pinned, policy-gated fetcher instead of serving only
  // from cache. OFF by default. The per-render G1 upstream pin is derived from the
  // request's OWN origin (scheme://hostname), never page-supplied; the optional
  // allow-list names opt-in third-party hosts reachable through the G2 SSRF guard.
  bool agent_optimize = false;
  std::vector<std::string> agent_render_allow_hosts;
  // Own origins the operator allows to resolve to a loopback or private
  // address for the agent render and /llms.txt fetches (entries as
  // ParseOperatorOrigins reads them; validated at startup).  Any other own
  // origin that resolves there is not fetched.
  std::vector<std::string> agent_render_private_origins;

  // The off-by-default /llms.txt site-index. Synthesis is gated on
  // the operator flags agent_optimize && agent_optimize_llms_txt.
  // The sitemap + per-page summary fetches reuse the same G1 own-origin IP-pin as
  // the render (NEVER page/sitemap-supplied). All keys parse-reject on
  // render-incapable surfaces (§6.5 parity).
  bool agent_optimize_llms_txt = false;
  // Eligibility filter for index entries: comma path-pattern list ("/" or empty =
  // all). Also the page-source fallback when no sitemap is reachable (concrete
  // entries only).
  std::vector<std::string> agent_optimize_paths;
  bool agent_optimize_respect_ai_directives = true;
  std::string agent_optimize_sitemap_url =
      "/sitemap.xml";  // own-origin path only
  // Max cheap (non-Chrome) per-page HTML fetches per full build (D-OQ4).
  size_t agent_optimize_llms_summary_fetch_cap = 200;
  // /llms.txt regen TTL ceiling, seconds (subordinate to sitemap content-hash).
  int64_t agent_optimize_cache_ttl_seconds = 86400;  // 24h
};

// Resolves the Chrome profile directory.  Preference order:
// the unit's RuntimeDirectory (a 0750 tmpfs wiped per boot — the right home
// for a per-run profile), else the directory holding the notify socket, else
// the cache directory.  Returns "" only when all three are empty, in which
// case Chrome picks its own path as it always did.  Exposed for testing.
std::string ResolveChromeUserDataDir(std::string_view runtime_directory_env,
                                     std::string_view socket_path,
                                     std::string_view cache_dir);

// Atomic stats for browser analysis monitoring.
struct BrowserStats {
  std::atomic<uint64_t> profiles_generated{0};
  std::atomic<uint64_t> profiles_used{0};
  std::atomic<uint64_t> analysis_errors{0};
  std::atomic<uint64_t> chrome_restarts{0};
  std::atomic<uint64_t> chrome_crashes{0};
  // Exits in a row within kChromeHealthyUptimeMs of a launch — a browser that
  // cannot run in this environment rather than one that crashed on a page.
  // Drives the restart back-off; reported by /v1/health.
  std::atomic<uint64_t> chrome_consecutive_failures{0};
  std::atomic<uint64_t> chrome_restart_delay_ms{0};
  std::atomic<uint64_t> queue_enqueued{0};
  std::atomic<uint64_t> queue_dropped{0};
  std::atomic<uint64_t> queue_processed{0};
  std::atomic<uint64_t> css_inlining_attempted{0};
  // Analyses whose CSS coverage render was skipped because the <noscript>
  // removal for it could not be trusted.
  std::atomic<uint64_t> noscript_strip_refusals{0};
  std::atomic<uint64_t> css_inlining_stylesheets_found{0};
  std::atomic<uint64_t> css_inlining_stylesheets_cached{0};
  std::atomic<uint64_t> css_inlining_bytes_inlined{0};
  std::atomic<uint64_t> reanalyses_scheduled{0};
  // Agent renders (or their Markdown writes) skipped because the page's own
  // anonymous answer was not confirmed as shareable.
  std::atomic<uint64_t> agent_render_page_refused{0};
  // Analyses queued because a template's validation record no longer matched
  // the stylesheet its page is served with (RequestRevalidation), and requests
  // the per-template rate limit turned away.
  std::atomic<uint64_t> revalidations_queued{0};
  std::atomic<uint64_t> revalidations_rate_limited{0};
  std::atomic<uint64_t> scripts_analyzed{0};
  std::atomic<uint64_t> scripts_deferrable{0};
  // Analysis-resource-map + script-evidence-gate observability.
  std::atomic<uint64_t> script_map_scripts_found{0};
  std::atomic<uint64_t> script_map_scripts_cached{0};
  std::atomic<uint64_t> script_map_scripts_uncached_same_origin{0};
  std::atomic<uint64_t> script_map_scripts_empty_same_origin{0};
  std::atomic<uint64_t> script_map_scripts_cross_origin{0};
  std::atomic<uint64_t> script_map_scripts_oversize{0};
  std::atomic<uint64_t> script_map_bytes_mapped{0};
  std::atomic<uint64_t> scripts_no_coverage{0};
  std::atomic<uint64_t> script_fetches_served{0};
  std::atomic<uint64_t> script_fetches_blocked{0};
};

// Manages the browser analysis pipeline: Chrome lifecycle, analysis
// queue, CSS extraction, page analysis, and profile storage.
class BrowserAnalysisManager {
 public:
  BrowserAnalysisManager(uv_loop_t* loop, BrowserAnalysisConfig config,
                         PageSpeedCache* cache, MessageHandler* handler);
  ~BrowserAnalysisManager();

  BrowserAnalysisManager(const BrowserAnalysisManager&) = delete;
  BrowserAnalysisManager& operator=(const BrowserAnalysisManager&) = delete;

  // Start Chrome and init async handle.  Non-fatal if Chrome fails.
  bool Initialize();

  // Stop Chrome, close handles.
  void Shutdown();

  // Thread-safe.  Fires uv_async_send to wake event loop.
  // Returns false if queue is full (item dropped).
  //
  // `site`, `host_vouched` and `from_module` are the notification's (wire v6)
  // and travel with the item: see AnalysisQueue::Item.
  bool EnqueueAnalysis(
      const std::string& url, const std::string& hostname,
      const std::string& scheme, const std::string& site, bool host_vouched,
      bool from_module, uint32_t mask, uint64_t template_hash,
      std::string_view original_html = {},
      std::optional<std::array<std::byte, 32>> origin_html_hash = std::nullopt,
      bool force_agent_render = false);

  // A page's profile carries a validation record, but the record was made
  // against different stylesheet bytes than the page is served with now (a
  // stylesheet redeploy, or a change in how those bytes are assembled).
  // Deferral stays off until the template is validated again;
  // without this that would wait for the profile's TTL
  // (browser_profile_ttl_seconds). Queues a fresh analysis for the template,
  // leaving the current profile in place until the new one is stored.
  //
  // Rate-limited per template the way the degraded-profile retry is: at most
  // kMaxReanalysisRetries requests per profile-TTL window, at least
  // kReanalysisDelayMs apart, so bytes that never settle (a sheet whose cache
  // entry flips between variants) cost two analyses a day, not one per
  // request. Thread-safe. Returns true when an analysis was queued.
  bool RequestRevalidation(
      const std::string& url, const std::string& hostname,
      const std::string& scheme, const std::string& site, bool host_vouched,
      bool from_module, uint32_t mask, uint64_t template_hash,
      std::string_view original_html,
      std::optional<std::array<std::byte, 32>> origin_html_hash = std::nullopt,
      std::chrono::steady_clock::time_point now =
          std::chrono::steady_clock::now());

  // Thread-safe cache lookup for an existing profile, in `site`'s keyspace.
  std::optional<OptimizationProfile> LookupProfile(uint64_t template_hash,
                                                   std::string_view site = {});

  TemplateDetector& template_detector() { return template_detector_; }
  const BrowserStats& stats() const { return stats_; }
  bool chrome_running() const;
  size_t queue_depth() const;

  // Get the CDP client for direct browser interaction (e.g., capture
  // endpoints).  Returns nullptr when Chrome is not running.
  CdpClient* cdp_client() const;

  // Get the loop this manager runs on.
  uv_loop_t* loop() const { return loop_; }

  // Replace the message handler (e.g., after wrapping with
  // TeeMessageHandler).
  void set_handler(MessageHandler* handler) { handler_ = handler; }

  // Invoked (best-effort) after a kAgentMarkdown variant is written for a page
  // URL.  This write happens outside HandleNotification, so the worker uses
  // this hook to refresh that URL's cached alternate count in the URL registry.
  // Args: url, hostname, scheme, site.
  using AgentAlternateWrittenCallback =
      std::function<void(const std::string&, const std::string&,
                         const std::string&, const std::string&)>;
  void set_agent_alternate_written_callback(AgentAlternateWrittenCallback cb) {
    on_agent_alternate_written_ = std::move(cb);
  }

  // JSON status for the BROWSER-STATUS management command.
  std::string StatusJson() const;

  // Test-only seam: dequeue the highest-priority queued item so tests can
  // assert what EnqueueAnalysis stored on it (e.g. the owned original_html
  // copy) without driving a live Chrome render through RunAnalysis.
  std::optional<AnalysisQueue::Item> TestDequeue() { return queue_.Dequeue(); }

  // The stylesheet bytes a page is judged against, supplied by the serve path's
  // own assembly rather than reimplemented here.  See the alias definitions in
  // browser_analysis_manager_internal.h for why that matters.
  using CombinedCss = browser_internal::CombinedCssBytes;
  using CombinedCssBuilder = browser_internal::CombinedCssBuilder;
  // Test-only: the builder the analysis side assembles the combined stylesheet
  // with (the worker wires its own BuildCombinedCss here), so a test can seed
  // a record against exactly the bytes that side would hash and check the
  // serve path accepts it.
  const CombinedCssBuilder& TestCombinedCssBuilder() const {
    return combined_css_builder_;
  }
  void set_combined_css_builder(CombinedCssBuilder builder) {
    combined_css_builder_ = std::move(builder);
  }

  // The gate every cached sub-resource the analysis loads (stylesheets and
  // their @imports, scripts) must pass (SubresourceGate).  Without one the
  // analysis loads none.  The analysis runs on the event loop, where the gate
  // answers from its memo only; EnqueueAnalysis, which runs on the
  // notification's thread, asks it first.
  void set_subresource_gate(std::shared_ptr<SubresourceGate> gate) {
    subresource_gate_ = std::move(gate);
  }

  // Test-only seam: the async half of one viewport's validation — hand it the
  // two documents, get back a verdict. Unset in production, where it is the
  // real render on `validation_gate_`.
  //
  // The render is not the part worth testing; everything around it is. Which
  // block gets rendered, whether a refusal quietly stamps a record anyway, and
  // which stylesheet a record binds to are all decisions taken here, all
  // capable of manufacturing a false confirmation, and none reachable without
  // this seam — the analysis pipeline will not start without a live Chrome.
  using ValidationRunner = std::function<void(
      const ValidationDocuments& docs, uint32_t width, uint32_t height,
      std::function<void(ValidationVerdict)>)>;
  void set_validation_runner(ValidationRunner runner) {
    validation_runner_ = std::move(runner);
  }

  // Test-only seam: stands in for the per-viewport page-analysis render.
  //
  // It exists so the ORDER of the per-viewport pipeline is assertable. The
  // order — CSS extraction, then page analysis, then validation — is what makes
  // the validated block equal to the served one, and until page analysis could
  // be observed without a browser, reversing it broke nothing any test could
  // see. Same shape and same rationale as the validation runner above: under
  // test the injected runner IS the renderer.
  using PageAnalysisRunner = std::function<void(
      uint32_t width, uint32_t height,
      std::function<void(absl::StatusOr<PageAnalysisResult>)> done)>;
  void set_page_analysis_runner(PageAnalysisRunner runner) {
    page_analysis_runner_ = std::move(runner);
  }

  // Test-only seam: drive ONE viewport from the moment CSS extraction returns,
  // with both renders injected, and hand back the profile it left behind. This
  // is the entry point that makes the step ORDER observable — it goes through
  // the real OnCssExtractionDone, so which step that function hands off to is
  // part of what is under test.
  ViewportProfile TestRunViewportFromCssExtraction(
      const AnalysisQueue::Item& item, const std::string& pre_inline_html,
      const ViewportProfile& seed, int viewport_index);

  // Test-only seam: drive ONE viewport through the production validation path
  // and hand back the viewport profile it left behind. Same input assembly,
  // same derivation, same document synthesis, same stamping — only the render
  // is the injected runner.
  // `fold_measured` mirrors what the pipeline would have done by this point:
  // true is the production path (page analysis ran, and the seed carries the
  // fold it measured). Pass false to exercise the refusal that guards the
  // ordering.
  ViewportProfile TestValidateOneViewport(const AnalysisQueue::Item& item,
                                          const std::string& pre_inline_html,
                                          const ViewportProfile& seed,
                                          int viewport_index,
                                          bool fold_measured = true);

  // Test-only seam: persist `profile` under `template_hash` exactly as the real
  // analysis pipeline would (write the profile JSON to cache + record the
  // template entry) so a subsequent LookupProfile(template_hash) returns it.
  // Lets a test exercise the profile-driven critical-CSS path (e.g. the Issue A
  // coverage-budget suppression) without driving a live Chrome render.
  void TestStoreProfile(uint64_t template_hash,
                        const OptimizationProfile& profile,
                        std::string_view site = {});

 private:
  // Chrome lifecycle.
  void StartChrome();
  void OnChromeExit(int64_t exit_status, int term_signal);

  // Analysis pipeline (all on event loop).
  static void OnAnalysisReady(uv_async_t* handle);
  void DrainQueue();
  void RunAnalysis(AnalysisQueue::Item item);
  void RunViewportAnalysis();
  void OnCssExtractionDone(absl::StatusOr<BrowserCssResult> result);
  // Assemble, once per item, the validation inputs: the page as the origin
  // served it, the elements it declares, and the combined stylesheet the serve
  // path will judge it against.
  void PrepareValidationInputs();
  void RunPageAnalysisForViewport();
  void OnPageAnalysisDone(absl::StatusOr<PageAnalysisResult> result);
  // Render the candidate above-the-fold block against the full sheet at this
  // viewport, then advance the schedule either way.
  //
  // ORDERING IS LOAD-BEARING: this runs AFTER page analysis for the viewport,
  // because page analysis is what measures the fold, and the block validated
  // here must be derived from the same fold the serve path will derive from.
  // Run it earlier and it confirms a block no visitor receives.
  void RunCriticalCssValidation();
  // `anonymous_layers_dropped`: of the derivation that made the candidate
  // (ValidationRequest), for the record's binding.
  void OnCriticalCssValidationDone(std::string candidate_critical_css,
                                   bool anonymous_layers_dropped,
                                   ValidationVerdict verdict);
  // Tail of one viewport: advance the schedule, or finish the item.
  void AdvanceToNextViewport();
  void StoreProfile(const OptimizationProfile& profile);
  void RunScriptAnalysis();
  void OnScriptAnalysisDone(absl::StatusOr<ScriptCoverageResult> result);
  // Common completion tail: store the perf profile, then (if agent_optimize is
  // on) run the one-shot RunAgentRender before completing.  Decouples the
  // agent render from the per-viewport perf loop.
  void FinishAnalysis();
  void RunAgentRender();
  void OnAgentRenderDone(absl::StatusOr<PageAnalysisResult> result);
  bool MaybeScheduleReanalysis();
  void OnReanalysisTimer();
  void OnAnalysisComplete();

  uv_loop_t* loop_;  // Not owned
  BrowserAnalysisConfig config_;
  PageSpeedCache* cache_;    // Not owned
  MessageHandler* handler_;  // Not owned

  CombinedCssBuilder combined_css_builder_;
  std::shared_ptr<SubresourceGate> subresource_gate_;
  // A cache lookup for the analysis of `item` that returns only what the
  // gate admits; the lookup's url is a cache URL (or an absolute URL whose
  // host keys the read).  `may_fetch` false: memo only.  `shared_copy`: the
  // bytes go into the agent render's document, so the gate asks with its
  // shared-copy HEAD (SubresourceRead::shared_copy).
  std::function<std::optional<std::string>(std::string_view)> GatedLookup(
      const AnalysisQueue::Item& item, bool may_fetch,
      bool shared_copy = false);
  // The operator's private-address origins, parsed.
  std::vector<PinnedUpstream> AgentPrivateOrigins() const;
  // Logs a shared-copy refusal (private address not named by the operator).
  void LogPrivateAddressRefusal(const std::string& message);
  ValidationRunner validation_runner_;
  // Set only while TestValidateOneViewport is driving: stops the pipeline at
  // the end of the viewport rather than advancing the schedule, which would
  // need a live browser.
  bool validation_only_ = false;
  PageAnalysisRunner page_analysis_runner_;
  // Set while TestRunViewportFromCssExtraction is driving: stops the schedule
  // advancing past the one viewport under test.
  bool single_viewport_only_ = false;

  // Notifies the worker after an out-of-band kAgentMarkdown write so it can
  // refresh the URL's cached alternate count.  Empty when unset.
  AgentAlternateWrittenCallback on_agent_alternate_written_;

  std::unique_ptr<ChromeProcess> chrome_;
  // ChromeProcess objects whose Chrome has exited but whose libuv handles are
  // still being closed.  OnChromeExit runs INSIDE the process handle's exit
  // callback, and uv_close() is asynchronous: the handle (and the pipe/timer
  // handles closed with it) stay on the loop's closing list until the current
  // iteration ends.  Deleting the object there frees memory libuv is about to
  // walk -- a use-after-free that surfaces as a crash in uv_run once the heap
  // reuses the block (issue #1467 found it via the repeated-launch-failure
  // path).  Retired objects are freed on the next StartChrome and in Shutdown,
  // both well after the close callbacks have run.
  std::vector<std::unique_ptr<ChromeProcess>> retired_chrome_;
  std::unique_ptr<BrowserCssExtractor> css_extractor_;
  std::unique_ptr<PageAnalyzer> page_analyzer_;
  std::unique_ptr<ScriptCoverageAnalyzer> script_analyzer_;
  // The critical-CSS confirmation's renderer. Owned here, next to its siblings
  // and torn down with them, because an in-flight comparison holds a raw
  // CdpClient* and a 60 s timer: leave it beyond an owner's reach and Chrome
  // exiting turns that timer into a use-after-free. See VisualRegressionGate's
  // lifetime note.
  std::unique_ptr<VisualRegressionGate> validation_gate_;

  AnalysisQueue queue_;
  uv_async_t analysis_async_{};
  std::atomic<bool> analysis_async_initialized_{false};
  std::mutex async_send_mutex_;  // Guards async handle lifecycle vs send.

  uv_timer_t restart_timer_{};
  bool restart_timer_initialized_ = false;
  bool restart_timer_active_ = false;
  // uv_now() at the last successful spawn; 0 when Chrome is not running.
  uint64_t chrome_started_at_ms_ = 0;
  // Fires kChromeHealthyUptimeMs after a spawn that is still running, and
  // clears chrome_consecutive_failures / chrome_restart_delay_ms.
  uv_timer_t healthy_timer_{};
  bool healthy_timer_initialized_ = false;
  bool healthy_timer_active_ = false;

  // Re-analysis timer: fires after a delay to re-run browser analysis
  // when critical CSS extraction returned 0 bytes (external CSS not yet
  // cached).  Max kMaxReanalysisRetries attempts per URL.
  static constexpr int kReanalysisDelayMs = 30000;  // 30 seconds
  static constexpr int kMaxReanalysisRetries = 2;
  uv_timer_t reanalysis_timer_{};
  bool reanalysis_timer_initialized_ = false;
  bool reanalysis_timer_active_ = false;
  std::optional<AnalysisQueue::Item> pending_reanalysis_;

  // RequestRevalidation's per-template rate limit.
  struct RevalidationBudget {
    std::chrono::steady_clock::time_point window_start;
    std::chrono::steady_clock::time_point last;
    int requests = 0;
  };
  static constexpr size_t kMaxRevalidationBudgets = 4096;
  std::mutex revalidation_mutex_;
  std::unordered_map<uint64_t, RevalidationBudget> revalidation_budgets_;

  TemplateDetector template_detector_;

  bool analysis_in_progress_ = false;
  std::atomic<bool> shutting_down_{false};
  BrowserStats stats_;

  // Current analysis context (valid while analysis_in_progress_).
  struct AnalysisContext {
    AnalysisQueue::Item item;
    OptimizationProfile profile;
    // The analysis render's document: stylesheets inlined for CSS coverage,
    // plus an injected <base href>.  Unusable as a validation input — the sheet
    // is already in it, so a "critical only" candidate built from it would
    // carry the whole sheet as well.
    std::string html_content;
    // The agent render's document: as html_content, but built only from
    // sub-resources the gate admits for a shared copy.  Set when
    // agent_optimize is on.
    std::string agent_html;
    // The CSS coverage render's document: html_content without its <noscript>
    // elements (that render runs with script execution
    // disabled). Empty when `coverage_refusal` says why it may not be
    // rendered.
    std::string coverage_html;
    std::string coverage_refusal;
    // The page as the origin served it, captured before either rewrite above.
    std::string pre_inline_html;
    // Elements the origin document declares, for the DOM-matched derivation.
    std::vector<CollectedElement> pre_inline_elements;
    // The stylesheet bytes the serve path will judge this page against, from
    // the injected builder.  Empty when nothing was gathered or a declared
    // sheet was not in cache — in which case no record can usefully be made.
    std::string combined_css;
    // The page's cascade-layer order from the same assembly.
    CascadeLayerOrder layer_order{};
    // The page's <noscript> content changes a JS-off render; salts the
    // record's binding (ValidationBindingFor).
    bool noscript_affects_render = false;
    bool validation_inputs_ready = false;
    // Set when page analysis has completed for the CURRENT viewport, i.e. when
    // this viewport's above-the-fold set has been measured (or has definitively
    // failed to be). Reset at the start of every viewport.
    //
    // This exists to make the pipeline's ordering guarantee CHECKABLE rather
    // than merely documented. The validation must derive the block it judges
    // from the same fold the serve path will derive from; if a future edit moves
    // validation back ahead of page analysis, the fold would silently be empty
    // here and every viewport would be confirmed against a block no visitor
    // receives. RunCriticalCssValidation refuses outright when this is unset, so
    // that mistake costs deferral (benign) instead of buying a false
    // confirmation (a flash of unstyled content on a page we approved).
    bool fold_measured = false;
    // Shared per-item map (same-origin cached script bytes), served to BOTH
    // the page-analysis and script-analysis passes.  The agent render's map
    // stays empty (by contract).
    std::shared_ptr<const PageAnalyzer::ResourceMap> analysis_resources;
    // TTL-heal input: same-origin scripts existed that were not yet cached at
    // map-build time (their verdicts stay kNoCoverageData until re-analysis).
    bool same_origin_scripts_uncached = false;
    int viewport_index = 0;  // Current viewport being analyzed
    int viewports_done = 0;  // Number of viewports completed
    // Ordered viewport schedule: [first_from_mask, then remaining].
    int viewport_order[3] = {0, 1, 2};
    // Viewports to analyze: Mobile(375x667), Tablet(768x1024),
    // Desktop(1440x900), from src/browser/device_emulation.h, which also
    // decides what each one emulates (mobile device with touch, or a desktop
    // window) and which the critical-CSS extractor reads per class.
    static constexpr const uint32_t (&kViewportWidths)[3] =
        kAnalysisViewportWidths;
    static constexpr const uint32_t (&kViewportHeights)[3] =
        kAnalysisViewportHeights;
    static constexpr int kNumViewports = 3;
  };
  std::unique_ptr<AnalysisContext> current_analysis_;
};

}  // namespace pagespeed

#endif  // PAGESPEED_SRC_WORKER_BROWSER_ANALYSIS_MANAGER_H_
