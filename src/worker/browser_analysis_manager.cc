// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

// PageSpeed 2.0 - Browser Analysis Manager Implementation

#include "src/worker/browser_analysis_manager.h"

#include <sys/stat.h>
#include <sys/types.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <functional>
#include <optional>
#include <span>
#include <string_view>
#include <utility>

#include "absl/container/flat_hash_set.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/str_format.h"
#include "lib/base/message_handler.h"
#include "lib/base/url_util.h"
#include "lib/classify/alternate_id.h"
#include "lib/classify/alternate_metadata.h"
#include "lib/classify/capability_mask.h"
#include "lib/classify/content_type.h"
#include "lib/net/fetch_policy.h"
#include "lib/net/upstream_pin.h"
#include "src/browser/agent_fetcher.h"
#include "src/browser/cdp_types.h"
#include "src/worker/analysis_resource_map.h"
#include "src/worker/browser_analysis_manager_internal.h"
#include "src/worker/css_cache_inliner.h"
#include "src/worker/noscript_strip.h"
#include "src/worker/uv_helpers.h"

namespace pagespeed {

// ---------------------------------------------------------------------------
// Testable helper functions (browser_internal namespace)
// ---------------------------------------------------------------------------

namespace browser_internal {

int ViewportToIndex(CapabilityMask::Viewport vp) {
  switch (vp) {
    case CapabilityMask::Viewport::kMobile:
      return 0;
    case CapabilityMask::Viewport::kTablet:
      return 1;
    case CapabilityMask::Viewport::kDesktop:
    default:
      return 2;
  }
}

CapabilityMask::Viewport IndexToViewport(int index) {
  switch (index) {
    case 0:
      return CapabilityMask::Viewport::kMobile;
    case 1:
      return CapabilityMask::Viewport::kTablet;
    default:
      return CapabilityMask::Viewport::kDesktop;
  }
}

ViewportProfile* GetViewportProfile(OptimizationProfile& profile, int index) {
  ViewportProfile* profiles[] = {&profile.mobile, &profile.tablet,
                                 &profile.desktop};
  if (index >= 0 && index < 3) return profiles[index];
  return &profile.desktop;
}

void PopulateViewportFromPageAnalysis(PageAnalysisResult& result,
                                      ViewportProfile& vp) {
  vp.lcp_selector = std::move(result.lcp.selector);
  vp.lcp_url = std::move(result.lcp.url);

  for (auto& img : result.images) {
    if (img.above_fold) {
      vp.above_fold_selectors.push_back(img.selector);
    } else {
      vp.below_fold_selectors.push_back(img.selector);
    }
    vp.image_dimensions.push_back(ImageDimension{
        std::move(img.selector), img.rendered_width, img.rendered_height,
        img.natural_width, img.natural_height});
  }

  // Reduce 2000 per-element records to the distinct tokens that identify them.
  // On a utility-CSS page the same handful of classes repeats across the whole
  // fold, so the deduped set is a fraction of the descriptor list — and this
  // lands in a cached profile that is re-read on every request for the
  // template.
  //
  // ID and CLASS tokens only. A bare tag token (`div`, `span`) would match
  // essentially every element in the document and promote the entire page to
  // above-the-fold, which would re-inline most of a utility stylesheet. An
  // element that only a tag rule describes is left to the extractor's own
  // first-N-elements estimate (CriticalCssConfig::max_elements), i.e. to the
  // behaviour it had before anything was measured.
  size_t tokens = 0;
  absl::flat_hash_set<std::string> seen;
  auto add_token = [&](std::string token) {
    if (tokens >= kMaxAboveFoldSelectorTokens) return;
    // Count-bounded is not byte-bounded — see kMaxAboveFoldSelectorTokenBytes.
    if (token.size() > kMaxAboveFoldSelectorTokenBytes) return;
    if (!seen.insert(token).second) return;
    ++tokens;
    vp.above_fold_selectors.push_back(std::move(token));
  };
  for (const ElementDescriptor& el : result.elements) {
    // The fold, not the document: admitting below-fold classes here would undo
    // the point of measuring.
    if (!el.above_fold) continue;
    if (!el.id.empty()) add_token(absl::StrCat("#", el.id));
    for (const std::string& cls : el.classes) {
      if (!cls.empty()) add_token(absl::StrCat(".", cls));
    }
  }
}

bool AgentBindingStillLive(PageSpeedCache& cache,
                           const AnalysisQueue::Item& item) {
  if (!item.has_origin_html_hash) return false;
  auto live = cache.ReadAlternate(
      item.url, item.hostname, item.scheme,
      static_cast<AlternateId>(SentinelId::kContentHash), item.site);
  if (!live.has_value()) return false;
  auto sc = live->content();
  return sc.size() == AlternateMetadata::kHashSize &&
         std::memcmp(sc.data(), item.origin_html_hash.data(),
                     AlternateMetadata::kHashSize) == 0;
}

bool LooksLikeDegradedProfile(const OptimizationProfile& profile) {
  const ViewportProfile* viewports[] = {&profile.mobile, &profile.tablet,
                                        &profile.desktop};
  for (const ViewportProfile* vp : viewports) {
    // Only populated viewports carry the over-report signature; an empty
    // viewport is the separate all-empty re-analysis case.
    if (vp->critical_css.empty()) continue;
    if (vp->css_coverage_ratio >= kDegradedCoverageThreshold) return true;
  }
  return false;
}

bool ShouldInlineCriticalCss(float coverage_ratio) {
  // Single COVERAGE gate: the FCP "critical" CSS is really the bulk of the
  // sheet.  In production coverage_ratio == critical_bytes / total_css_bytes
  // (see the header's PRODUCTION INVARIANT note), so this gate alone makes the
  // suppression floor exactly kInlineCriticalCssMaxCoverage (0.60).
  return coverage_ratio < kInlineCriticalCssMaxCoverage;
}

bool MayRunAgentRender(const AnalysisQueue::Item& item, bool agent_optimize,
                       bool chrome_running) {
  return agent_optimize && chrome_running && item.host_vouched;
}

bool AgentPageAdmitted(SubresourceGate* gate, const AnalysisQueue::Item& item,
                       bool may_fetch,
                       const std::vector<PinnedUpstream>& private_origins) {
  if (gate == nullptr) return false;
  SubresourceRead read;
  read.site = item.site;
  read.scheme = item.scheme;
  read.host = item.hostname;
  read.url = item.url;
  read.page_host = item.hostname;
  read.host_fetch_allowed = item.host_vouched;
  read.shared_copy = true;
  read.private_address_origins = &private_origins;
  return gate->Admitted(read, may_fetch);
}

AnalysisQueue::Item BuildReanalysisItem(const AnalysisQueue::Item& src,
                                        int next_retry_count) {
  AnalysisQueue::Item retry;
  retry.url = src.url;
  retry.hostname = src.hostname;
  retry.scheme = src.scheme;
  retry.mask = src.mask;
  retry.template_hash = src.template_hash;
  retry.retry_count = next_retry_count;
  // Carry the content binding so the retry's agent render stays
  // bound (else AgentBindingStillLive fail-closes -> markdown amputated).
  retry.has_origin_html_hash = src.has_origin_html_hash;
  retry.origin_html_hash = src.origin_html_hash;
  // The retry stays in the item's keyspace and keeps its sender bits.
  retry.site = src.site;
  retry.host_vouched = src.host_vouched;
  retry.from_module = src.from_module;
  return retry;
}

bool ShouldFoldDeferRecommendation(std::string_view url,
                                   DeferralAdvice advice) {
  if (advice != DeferralAdvice::kSafeToDefer &&
      advice != DeferralAdvice::kCandidateForAsync) {
    return false;
  }
  return url.find("://") != std::string_view::npos;
}

int64_t ProfileExpiryFor(int64_t created_at, int64_t configured_ttl_seconds,
                         bool same_origin_scripts_uncached) {
  int64_t ttl = configured_ttl_seconds;
  if (same_origin_scripts_uncached) {
    ttl = std::min<int64_t>(ttl, kColdScriptProfileTtlSeconds);
  }
  return created_at + ttl;
}

AnalysisDocuments BuildAnalysisDocuments(std::string_view origin_html,
                                         std::string_view url,
                                         std::string_view hostname,
                                         std::string_view scheme,
                                         const css::CssLookupFn& lookup,
                                         CssInliningStats* stats) {
  AnalysisDocuments out;
  // Inline cached stylesheets for browser CSS coverage. The item's hostname
  // and scheme are the page's own: `url` is the cache-normalized path.
  out.document = InlineCachedStylesheets(origin_html, url, lookup, stats,
                                         hostname, scheme);

  // Absolute <base> so relative subresource URLs resolve inside the
  // about:blank analysis/agent documents and DOM .src reads are absolute —
  // the join key for the ANALYSIS-side joins (resource map + profiler
  // coverage).  The production suffix-matcher is a separate consumer: it only
  // ever matches absolute-authored srcs (an absolute profile entry cannot be
  // the suffix of a relative-authored src), so verdicts earned by
  // relative-authored scripts stay analysis-only for now.  Author-<base>
  // documents are left untouched.
  out.document = InjectBaseHrefIfAbsent(out.document, url, hostname, scheme);

  // The coverage render runs with script execution disabled, and Blink then
  // renders <noscript> content and applies its CSS, so its coverage, and the
  // fold it reflects, would be a no-JS client's. It gets the
  // document a browser running scripts parses: no <noscript> elements. Only
  // it: the renders that run scripts already read <noscript> as raw text and
  // keep the document as it is. Last, after the inliner: its stylesheet list
  // comes from the scanner, which needs a loadCSS preload's <noscript> twin
  // to count that sheet.
  NoscriptStripResult stripped = StripNoscriptElements(out.document);
  if (!stripped.reliable) {
    out.coverage_refusal = stripped.unreliable_reason;
    return out;
  }
  // The independent reading: the scanner's element count of the same
  // document must match what the removal left (NoscriptStripAgreesWithScan).
  // Nothing removed means nothing to vouch for: the document is the input.
  if (stripped.removed > 0) {
    HtmlScanner scanner;
    HtmlScanResult scan = scanner.Scan(url, out.document);
    if (!scan.success ||
        !NoscriptStripAgreesWithScan(stripped, scan.elements)) {
      out.coverage_refusal =
          "the <noscript> removal and the HTML scanner read the page "
          "differently";
      return out;
    }
  }
  out.coverage_document = std::move(stripped.html);
  return out;
}

ValidationInputs BuildValidationInputs(const CombinedCssBuilder& builder,
                                       const std::string& url,
                                       const std::string& hostname,
                                       const std::string& scheme,
                                       std::string_view pre_inline_html,
                                       const std::string& site,
                                       bool host_vouched, bool from_module) {
  ValidationInputs inputs;
  if (!builder) {
    inputs.skip = ValidationInputSkip::kNoSheetBuilder;
    return inputs;
  }
  if (pre_inline_html.empty()) {
    inputs.skip = ValidationInputSkip::kNoPageMarkup;
    return inputs;
  }

  HtmlScanner scanner;
  HtmlScanResult scan = scanner.Scan(url, pre_inline_html);
  if (!scan.success) {
    inputs.skip = ValidationInputSkip::kUnscannable;
    return inputs;
  }

  CombinedCssBytes combined =
      builder(scan, url, hostname, scheme, site, host_vouched, from_module);
  if (combined.external_css_missing) {
    inputs.skip = combined.revalidatable_css_missing
                      ? ValidationInputSkip::kStylesheetNotCachedYet
                      : ValidationInputSkip::kStylesheetNeverCacheable;
    return inputs;
  }
  if (combined.css.empty()) {
    inputs.skip = ValidationInputSkip::kEmptyStylesheet;
    return inputs;
  }

  inputs.elements = std::move(scan.elements);
  inputs.combined_css = std::move(combined.css);
  inputs.layer_order = std::move(combined.layer_order);
  inputs.noscript_affects_render = scan.noscript_affects_render;
  inputs.ready = true;
  return inputs;
}

const char* ValidationInputSkipMessage(ValidationInputSkip skip) {
  switch (skip) {
    case ValidationInputSkip::kNone:
      return "no skip";
    case ValidationInputSkip::kNoSheetBuilder:
      return "no stylesheet assembly is wired to this analysis";
    case ValidationInputSkip::kNoPageMarkup:
      return "the page markup was not carried into the analysis";
    case ValidationInputSkip::kUnscannable:
      return "the page could not be scanned";
    case ValidationInputSkip::kStylesheetNotCachedYet:
      return "a declared stylesheet was not in cache yet; this will be "
             "retried once it is";
    case ValidationInputSkip::kStylesheetNeverCacheable:
      return "a declared stylesheet is cross-origin and never enters this "
             "cache, so this page cannot be confirmed and its stylesheet "
             "stays render-blocking";
    case ValidationInputSkip::kEmptyStylesheet:
      return "the page has no stylesheet to defer";
  }
  return "unknown";
}

ValidationPlan PlanCriticalCssValidation(
    const ViewportProfile& vp, std::string_view combined_css,
    std::string_view candidate_critical_css, bool chrome_available) {
  ValidationPlan plan;
  if (!chrome_available) {
    plan.skip = ValidationSkip::kChromeUnavailable;
    return plan;
  }
  if (combined_css.empty()) {
    plan.skip = ValidationSkip::kNoCombinedStylesheet;
    return plan;
  }
  // The use-time inline budget already refuses to inline (and therefore to
  // defer) a near-whole-sheet profile, so nothing here could authorize
  // anything.  Judged on the same measured coverage the serve path judges on.
  if (!ShouldInlineCriticalCss(vp.css_coverage_ratio)) {
    plan.skip = ValidationSkip::kSuppressedByInlineBudget;
    return plan;
  }
  // LAST on purpose: this is the only question that costs anything to answer.
  // Producing the candidate means re-deriving the block from the whole sheet,
  // which is the most expensive synchronous step in the analysis (~580 ms on a
  // 118 KB sheet, measured — see PrepareCriticalCssValidation), so every
  // cheaper refusal above must get its chance first.
  if (candidate_critical_css.empty()) {
    plan.skip = ValidationSkip::kNoCriticalBlock;
    return plan;
  }
  plan.run = true;
  return plan;
}

const char* ValidationSkipMessage(ValidationSkip skip) {
  switch (skip) {
    case ValidationSkip::kNone:
      return "no skip";
    case ValidationSkip::kChromeUnavailable:
      return "the browser is not available";
    case ValidationSkip::kNoCriticalBlock:
      return "no above-the-fold block was produced for this viewport";
    case ValidationSkip::kNoCombinedStylesheet:
      return "the page's stylesheets were not all gathered";
    case ValidationSkip::kSuppressedByInlineBudget:
      return "the inline budget already declines this profile";
    case ValidationSkip::kFoldNotMeasured:
      return "this viewport's above-the-fold set has not been measured yet";
  }
  return "unknown";
}

ValidationRequest PrepareCriticalCssValidation(
    const ViewportProfile& vp, const std::vector<CollectedElement>& elements,
    std::string_view combined_css, std::string_view pre_inline_html,
    CapabilityMask::Viewport viewport, bool renderer_available,
    const CascadeLayerOrder& layer_order) {
  ValidationRequest request;

  // Answer every cheap refusal BEFORE deriving anything. The derivation below
  // re-parses the whole stylesheet and matches it against the page's DOM: it
  // measures ~580 ms on a 118 KB sheet and it runs on the event loop, three
  // times per template. A viewport that was never going to be validated — no
  // browser, no sheet, or a profile the inline budget already declines, which
  // is the common case — must not pay for it. Everything except "did the
  // derivation produce anything" is knowable first, so ask with an empty
  // candidate and let that one question through.
  ValidationPlan cheap = PlanCriticalCssValidation(
      vp, combined_css, /*candidate_critical_css=*/{}, renderer_available);
  if (!cheap.run && cheap.skip != ValidationSkip::kNoCriticalBlock) {
    request.skip = cheap.skip;
    return request;
  }
  if (vp.critical_css.empty()) {
    request.skip = ValidationSkip::kNoCriticalBlock;
    return request;
  }

  // The SAME derivation the serve path inlines, down to the measured fold it is
  // derived against. Rendering vp.critical_css — Chrome's raw coverage blob —
  // instead would confirm a block the visitor never receives, which is a false
  // confirmation wearing the right shape; so would deriving against a different
  // fold from the one `vp` will carry when the serve path reads it.
  //
  // That `vp.above_fold_selectors` is populated by the time this runs is a
  // PIPELINE ORDERING GUARANTEE, not an accident: within a viewport the manager
  // runs CSS extraction -> page analysis -> validation, precisely so the fold is
  // measured before the block that will be judged is derived. Reordering page
  // analysis after validation reintroduces the defect. It is not enough for the
  // served block to be a SUPERSET of the validated one: an override the
  // extractor can never admit (a bare attribute selector, say) can be masked in
  // the smaller block and visible in the larger, so the superset flashes while
  // the subset it was validated as does not.
  CriticalCssResult derived = DeriveDomMatchedCriticalCss(
      elements, combined_css, vp.critical_css, viewport,
      vp.above_fold_selectors, layer_order);
  std::string candidate = std::move(derived.critical_css);

  ValidationPlan plan = PlanCriticalCssValidation(vp, combined_css, candidate,
                                                  renderer_available);
  if (!plan.run) {
    request.skip = plan.skip;
    return request;
  }

  // `elements` is the scanner's reading of the same markup, the check on the
  // <noscript> removal the documents go through.
  request.docs = BuildValidationDocuments(
      pre_inline_html, combined_css, candidate, layer_order,
      kMaxValidationDocumentBytes, &elements);
  if (!request.docs.ok) return request;

  request.candidate_critical_css = std::move(candidate);
  request.anonymous_layers_dropped = derived.anonymous_layers_dropped;
  request.run = true;
  return request;
}

std::string ValidationRefusalMessage(const ValidationRequest& request) {
  if (request.skip != ValidationSkip::kNone) {
    return ValidationSkipMessage(request.skip);
  }
  return request.docs.error.empty() ? "the comparison could not be set up"
                                    : request.docs.error;
}

void ApplyValidationVerdict(ViewportProfile& vp,
                            const ValidationVerdict& verdict,
                            std::string_view candidate_critical_css,
                            std::string_view combined_css,
                            std::string_view layer_order_binding) {
  // Clear first: whatever else happens below, what is left behind must be a
  // record this run actually earned.
  vp.critical_css_validated = false;
  vp.validated_critical_css_hash.clear();
  vp.validated_combined_css_hash.clear();
  vp.validation_diff_ratio = verdict.diff_ratio;

  if (!verdict.validated) return;
  if (candidate_critical_css.empty()) return;

  std::string combined_hash =
      CombinedCssValidationHash(combined_css, layer_order_binding);
  // An empty sheet hashes to the empty string, which no stored hash can equal.
  // Recording the bit anyway would leave a record that reads as written wrong
  // rather than as never made.
  if (combined_hash.empty()) return;

  vp.critical_css_validated = true;
  vp.validated_critical_css_hash =
      CombinedCssValidationHash(candidate_critical_css);
  vp.validated_combined_css_hash = std::move(combined_hash);
}

std::string ChromeExitReason(int64_t exit_status, int term_signal) {
  if (term_signal != 0) return std::string();
  switch (exit_status) {
    case 21:
      return "profile directory locked by another Chrome instance (exit 21)";
    default:
      return std::string();
  }
}

}  // namespace browser_internal

// Bring browser_internal names into local scope for existing callers.
using browser_internal::AgentBindingStillLive;
using browser_internal::AgentPageAdmitted;
using browser_internal::BuildReanalysisItem;
using browser_internal::ChromeExitReason;
using browser_internal::GetViewportProfile;
using browser_internal::IndexToViewport;
using browser_internal::LooksLikeDegradedProfile;
using browser_internal::MayRunAgentRender;
using browser_internal::ViewportToIndex;

namespace {

// Restart delay after Chrome crash (milliseconds).
constexpr int kRestartDelayMs = 2000;
// A Chrome that exits within this long of being spawned did not crash on a
// page: it could not start at all (missing library, unwritable profile, a
// launch posture it refuses).  Such exits back off exponentially from
// kRestartDelayMs up to kMaxRestartDelayMs instead of respawning every few
// seconds forever; a Chrome that lived at least this long resets the series.
constexpr uint64_t kChromeHealthyUptimeMs = 60000;
constexpr uint64_t kMaxRestartDelayMs = 300000;  // 5 minutes
// Consecutive early exits after which the refusal is said out loud once.
constexpr uint64_t kEarlyExitWarnThreshold = 3;

}  // namespace

BrowserAnalysisManager::BrowserAnalysisManager(uv_loop_t* loop,
                                               BrowserAnalysisConfig config,
                                               PageSpeedCache* cache,
                                               MessageHandler* handler)
    : loop_(loop),
      config_(std::move(config)),
      cache_(cache),
      handler_(handler),
      queue_(config_.browser_queue_size) {}

BrowserAnalysisManager::~BrowserAnalysisManager() { Shutdown(); }

bool BrowserAnalysisManager::Initialize() {
  // Init uv_async_t for cross-thread signaling.
  int r = uv_async_init(loop_, &analysis_async_, OnAnalysisReady);
  if (r != 0) {
    if (handler_ != nullptr) {
      handler_->Error("Failed to init browser analysis async handle: %s",
                      uv_strerror(r));
    }
    return false;
  }
  analysis_async_.data = this;
  analysis_async_initialized_ = true;

  // Init restart timer once (reused via start/stop).
  uv_timer_init(loop_, &restart_timer_);
  restart_timer_.data = this;
  restart_timer_initialized_ = true;

  // Init the healthy-uptime timer (armed on every successful spawn).
  uv_timer_init(loop_, &healthy_timer_);
  healthy_timer_.data = this;
  healthy_timer_initialized_ = true;

  // Init re-analysis timer (for retrying CSS extraction on 0% coverage).
  uv_timer_init(loop_, &reanalysis_timer_);
  reanalysis_timer_.data = this;
  reanalysis_timer_initialized_ = true;

  StartChrome();

  // Chrome failure is non-fatal — we'll retry via restart timer.
  return true;
}

void BrowserAnalysisManager::Shutdown() {
  if (shutting_down_) return;
  shutting_down_ = true;

  queue_.Shutdown();
  current_analysis_.reset();
  analysis_in_progress_ = false;

  // Stop Chrome.  The validation gate is cancelled BEFORE the client goes —
  // see OnChromeExit for why the order is not cosmetic.
  validation_gate_.reset();
  if (chrome_) {
    chrome_->Stop();
    chrome_.reset();
  }
  retired_chrome_.clear();
  css_extractor_.reset();
  page_analyzer_.reset();

  // Stop and close restart timer (must close even if not active — the
  // handle was registered with the loop in Initialize).
  if (restart_timer_initialized_) {
    if (restart_timer_active_) {
      uv_timer_stop(&restart_timer_);
      restart_timer_active_ = false;
    }
    SafeClose(&restart_timer_);
    restart_timer_initialized_ = false;
  }
  if (healthy_timer_initialized_) {
    if (healthy_timer_active_) {
      uv_timer_stop(&healthy_timer_);
      healthy_timer_active_ = false;
    }
    SafeClose(&healthy_timer_);
    healthy_timer_initialized_ = false;
  }

  // Stop re-analysis timer.
  if (reanalysis_timer_initialized_) {
    if (reanalysis_timer_active_) {
      uv_timer_stop(&reanalysis_timer_);
      reanalysis_timer_active_ = false;
    }
    SafeClose(&reanalysis_timer_);
    reanalysis_timer_initialized_ = false;
  }
  pending_reanalysis_.reset();

  // Close async handle (guarded by async_send_mutex_ to prevent
  // EnqueueAnalysis from racing with the flag reset).
  {
    std::lock_guard<std::mutex> lock(async_send_mutex_);
    if (analysis_async_initialized_) {
      SafeClose(&analysis_async_);
      analysis_async_initialized_ = false;
    }
  }
}

std::string ResolveChromeUserDataDir(std::string_view runtime_directory_env,
                                     std::string_view socket_path,
                                     std::string_view cache_dir) {
  // systemd hands RuntimeDirectory as a colon-separated list; the unit
  // declares exactly one, but honour the contract rather than the instance.
  if (!runtime_directory_env.empty()) {
    const size_t colon = runtime_directory_env.find(':');
    std::string_view first = (colon == std::string_view::npos)
                                 ? runtime_directory_env
                                 : runtime_directory_env.substr(0, colon);
    if (!first.empty()) return absl::StrCat(first, "/chrome");
  }
  if (!socket_path.empty()) {
    const size_t slash = socket_path.find_last_of('/');
    if (slash != std::string_view::npos && slash > 0) {
      return absl::StrCat(socket_path.substr(0, slash), "/chrome");
    }
  }
  if (!cache_dir.empty()) return absl::StrCat(cache_dir, "/chrome");
  return std::string();
}

void BrowserAnalysisManager::StartChrome() {
  if (shutting_down_) return;
  // See retired_chrome_: safe to free now, their close callbacks have run.
  retired_chrome_.clear();

  ChromeProcessConfig chrome_config;
  chrome_config.chrome_path = config_.chrome_binary;
  chrome_config.max_rss_mb = config_.chrome_max_memory_mb;
  chrome_config.startup_timeout_ms = config_.chrome_startup_timeout_ms;
  chrome_config.recycle_after_pages = config_.chrome_recycle_interval;
  chrome_config.no_sandbox = config_.chrome_no_sandbox;
  chrome_config.user_data_dir = config_.chrome_user_data_dir;

  // The opt-out is loud on EVERY spawn, not only at startup —
  // a daemon that has been running unsandboxed for a month should still say
  // so in today's log.
  if (config_.chrome_no_sandbox && handler_ != nullptr) {
    handler_->Warning(
        "SECURITY: browser sandbox is OFF (--browser-sandbox=off) — headless "
        "Chrome is being started with --no-sandbox and untrusted page content "
        "is parsed with no kernel-enforced isolation");
  }

  // The profile directory must exist and be private before Chrome opens it.
  // 0700 explicitly: the daemon's umask is a backstop, never the mechanism
  // (the same rule the socket/volume modes follow).
#ifndef _WIN32
  if (!chrome_config.user_data_dir.empty()) {
    const char* path = chrome_config.user_data_dir.c_str();
    if (::mkdir(path, 0700) != 0 && errno != EEXIST) {
      if (handler_ != nullptr) {
        handler_->Warning(
            "Could not create the Chrome profile directory %s: %s — Chrome "
            "will pick its own, which may not be writable under the service "
            "sandbox",
            path, std::strerror(errno));
      }
      chrome_config.user_data_dir.clear();
    } else {
      // The packaged runtime directory is 0750 pagespeed:pagespeed — group
      // READ and traverse, not group WRITE — so a peer cannot normally plant
      // anything here.  This is defence in depth, because the profile's
      // parent follows an operator-chosen --socket path in ad-hoc runs and
      // carries no such guarantee there: chmod() follows symlinks, so lstat
      // first and confirm we are about to adjust a real directory rather than
      // whatever it points at.  Refusing costs a temporary profile; not
      // refusing is a chmod primitive.
      struct stat st{};
      if (::lstat(path, &st) != 0 || !S_ISDIR(st.st_mode)) {
        if (handler_ != nullptr) {
          handler_->Warning(
              "Chrome profile path %s is not a plain directory (it may be a "
              "symlink); refusing to use it — Chrome will pick its own",
              path);
        }
        chrome_config.user_data_dir.clear();
      } else if ((st.st_mode & 0777) != 0700 && ::chmod(path, 0700) != 0) {
        if (handler_ != nullptr) {
          handler_->Warning(
              "Could not chmod 0700 the Chrome profile directory %s: %s", path,
              std::strerror(errno));
        }
      }
    }
  }
#endif

  // A `SingletonLock` left behind by a Chrome that is no longer running (a
  // recreated container has a new hostname; a reboot recycles the pid) makes
  // every launch exit 21 (PROFILE_IN_USE) until the entries are removed.
  // Reconcile it here, before every spawn, so the condition clears itself.
  if (!chrome_config.user_data_dir.empty()) {
    const auto report =
        ChromeProcess::ReconcileSingletonLock(chrome_config.user_data_dir);
    if (handler_ != nullptr) {
      switch (report.verdict.action) {
        case SingletonLockAction::kRemoveStale:
          if (!report.error.empty()) {
            handler_->Warning(
                "Could not remove the stale Chrome profile lock in %s (owner "
                "%s pid %d is not running): %s — Chrome will refuse the "
                "profile (exit 21) until it is removed",
                chrome_config.user_data_dir.c_str(),
                report.verdict.owner_host.c_str(), report.verdict.owner_pid,
                report.error.c_str());
          } else {
            handler_->Info(
                "Removed a stale Chrome profile lock in %s: it was held by "
                "%s pid %d, which is not running here",
                chrome_config.user_data_dir.c_str(),
                report.verdict.owner_host.c_str(), report.verdict.owner_pid);
          }
          break;
        case SingletonLockAction::kKeepLive:
          handler_->Warning(
              "Chrome profile %s is locked by a live process (%s pid %d); "
              "leaving the lock for Chrome to arbitrate",
              chrome_config.user_data_dir.c_str(),
              report.verdict.owner_host.c_str(), report.verdict.owner_pid);
          break;
        case SingletonLockAction::kNoLock:
          break;
      }
    }
  }

  chrome_ = std::make_unique<ChromeProcess>(loop_, chrome_config);
  chrome_->SetExitCallback([this](int64_t exit_status, int term_signal) {
    OnChromeExit(exit_status, term_signal);
  });

  auto status = chrome_->Start();
  if (!status.ok()) {
    if (handler_ != nullptr) {
      handler_->Warning("Chrome failed to start: %s",
                        std::string(status.message()).c_str());
    }
    chrome_.reset();
    return;
  }

  // Problems a render reports without ending (a refused Fetch.failRequest)
  // go to the worker's log.
  if (handler_ != nullptr) {
    chrome_->cdp_client()->set_diagnostic_handler(
        [handler = handler_](std::string_view message) {
          handler->Warning("Browser analysis: %s",
                           std::string(message).c_str());
        });
  }

  // Create CDP components from the running Chrome instance.
  css_extractor_ = std::make_unique<BrowserCssExtractor>(chrome_->cdp_client());
  page_analyzer_ = std::make_unique<PageAnalyzer>(chrome_->cdp_client());
  script_analyzer_ =
      std::make_unique<ScriptCoverageAnalyzer>(chrome_->cdp_client());
  validation_gate_ =
      std::make_unique<VisualRegressionGate>(chrome_->cdp_client());

  chrome_started_at_ms_ = uv_now(loop_);
  if (handler_ != nullptr) {
    handler_->Info("Chrome started (PID %d) for browser analysis",
                   chrome_->pid());
  }
  // Once this launch has outlived the early-exit window it is healthy: clear
  // the failure series and the retry delay so /v1/health stops reporting a
  // running browser as failing (issue #1475).  OnChromeExit applies the same
  // rule on exit; this timer applies it while Chrome keeps running.
  if (healthy_timer_initialized_) {
    uv_timer_start(
        &healthy_timer_,
        [](uv_timer_t* timer) {
          auto* mgr = static_cast<BrowserAnalysisManager*>(timer->data);
          mgr->healthy_timer_active_ = false;
          mgr->stats_.chrome_consecutive_failures.store(
              0, std::memory_order_relaxed);
          mgr->stats_.chrome_restart_delay_ms.store(0,
                                                    std::memory_order_relaxed);
        },
        kChromeHealthyUptimeMs, 0);
    healthy_timer_active_ = true;
  }

  // If there's pending work, try to drain.
  DrainQueue();
}

void BrowserAnalysisManager::OnChromeExit(int64_t exit_status,
                                          int term_signal) {
  const std::string exit_reason = ChromeExitReason(exit_status, term_signal);
  if (handler_ != nullptr) {
    handler_->Warning("Chrome exited (status=%s, signal=%d)%s%s",
                      absl::StrCat(exit_status).c_str(), term_signal,
                      exit_reason.empty() ? "" : ": ", exit_reason.c_str());
  }
  if (healthy_timer_active_) {
    uv_timer_stop(&healthy_timer_);
    healthy_timer_active_ = false;
  }

  // Null out CDP components — they're invalid now.  The validation gate goes
  // FIRST and chrome_ (which owns the CdpClient) LAST: destroying the gate
  // cancels any in-flight comparison while the client it points at is still
  // alive.  Reverse that order and the comparison's timeout timer outlives the
  // client and dereferences it — and once the client is gone that timer is the
  // ONLY way the capture can ever finish, so it is the common path, not a race.
  validation_gate_.reset();
  css_extractor_.reset();
  page_analyzer_.reset();
  script_analyzer_.reset();
  // An exit the daemon asked for (RSS cap) is not a launch failure.
  const bool stop_requested = chrome_ && chrome_->stop_requested();
  // NOT chrome_.reset(): this runs inside the process handle's exit callback
  // and the object's handles are still closing -- see retired_chrome_.
  if (chrome_) retired_chrome_.push_back(std::move(chrome_));

  stats_.chrome_crashes.fetch_add(1, std::memory_order_relaxed);

  // Cancel in-progress analysis.
  if (analysis_in_progress_) {
    current_analysis_.reset();
    analysis_in_progress_ = false;
  }

  if (shutting_down_) return;

  // Bounded back-off for a Chrome that dies right after launch (issue #1467:
  // an unwritable HOME made every spawn SIGTRAP within a second, and the
  // daemon respawned it every few seconds indefinitely, one core dump each).
  const uint64_t now = uv_now(loop_);
  const uint64_t uptime_ms =
      chrome_started_at_ms_ != 0 && now >= chrome_started_at_ms_
          ? now - chrome_started_at_ms_
          : 0;
  chrome_started_at_ms_ = 0;
  uint64_t failures = 0;
  if (stop_requested || uptime_ms >= kChromeHealthyUptimeMs) {
    stats_.chrome_consecutive_failures.store(0, std::memory_order_relaxed);
  } else {
    failures = stats_.chrome_consecutive_failures.fetch_add(
                   1, std::memory_order_relaxed) +
               1;
  }
  uint64_t delay_ms = kRestartDelayMs;
  if (failures > 1) {
    const uint64_t shift = std::min<uint64_t>(failures - 1, 16);
    delay_ms = std::min<uint64_t>(
        static_cast<uint64_t>(kRestartDelayMs) << shift, kMaxRestartDelayMs);
  }
  stats_.chrome_restart_delay_ms.store(delay_ms, std::memory_order_relaxed);
  if (failures == kEarlyExitWarnThreshold && handler_ != nullptr) {
    handler_->Warning(
        "Browser analysis is UNAVAILABLE: Chrome (%s) has exited %llu times in "
        "a row within %llu s of being started%s%s, so it cannot run in this "
        "environment (check that the binary starts as the daemon's user and "
        "that its profile directory %s is writable by that user). Restart "
        "attempts now back off, up to one every %llu s; /v1/health reports "
        "browser.chrome_running=false until one succeeds.",
        config_.chrome_binary.c_str(),
        static_cast<unsigned long long>(failures),
        static_cast<unsigned long long>(kChromeHealthyUptimeMs / 1000),
        exit_reason.empty() ? "" : ", last exit: ", exit_reason.c_str(),
        config_.chrome_user_data_dir.empty()
            ? "(Chrome's default)"
            : config_.chrome_user_data_dir.c_str(),
        static_cast<unsigned long long>(kMaxRestartDelayMs / 1000));
  }

  // Schedule restart after delay (timer was init'd once in Initialize).
  stats_.chrome_restarts.fetch_add(1, std::memory_order_relaxed);
  uv_timer_start(
      &restart_timer_,
      [](uv_timer_t* timer) {
        auto* mgr = static_cast<BrowserAnalysisManager*>(timer->data);
        mgr->restart_timer_active_ = false;
        mgr->StartChrome();
      },
      delay_ms, 0);
  restart_timer_active_ = true;
}

std::function<std::optional<std::string>(std::string_view)>
BrowserAnalysisManager::GatedLookup(const AnalysisQueue::Item& item,
                                    bool may_fetch, bool shared_copy) {
  std::vector<PinnedUpstream> private_origins;
  if (shared_copy) {
    private_origins = ParseOperatorOrigins(config_.agent_render_private_origins)
                          .value_or(std::vector<PinnedUpstream>{});
  }
  return [this, &item, may_fetch, shared_copy,
          private_origins = std::move(private_origins)](
             std::string_view url) -> std::optional<std::string> {
    if (cache_ == nullptr || subresource_gate_ == nullptr) return std::nullopt;
    std::string_view url_host = UrlHostname(url);
    const std::string host =
        url_host.empty() ? item.hostname : std::string(url_host);
    SubresourceRead read;
    read.site = item.site;
    read.scheme = item.scheme;
    read.host = host;
    read.url = url;
    read.page_host = item.hostname;
    // An item is queued only for a sender that may ask for fetches
    // (Worker::MayRequestHostFetches); the host is asked only when vouched.
    read.host_fetch_allowed = item.host_vouched;
    read.shared_copy = shared_copy;
    read.private_address_origins = &private_origins;
    auto read_entry = [&] {
      return cache_->ReadBestAlternate(
          std::string(url), host, item.scheme, CapabilityMask(),
          /*agent_request_entitled=*/false, item.site);
    };
    std::optional<ReadResult> r;
    // Under a host the worker may not ask, a stylesheet or script the front
    // end recorded from an anonymous request is admitted as read.  Anything
    // else is asked about first, without holding a read.
    if (!read.host_fetch_allowed) {
      auto first = read_entry();
      if (first.has_value() &&
          (first->metadata.flags & AlternateMetadata::kFlagAnonymousRecorded) !=
              0) {
        read.entry_read = true;
        read.entry_flags = first->metadata.flags;
        read.entry_cc_flags = first->metadata.origin_cc_flags;
        read.entry_site = item.site;  // the read above is keyed by item.site
        if (subresource_gate_->Admitted(read, may_fetch)) {
          r.emplace(std::move(*first));
        }
        read.entry_read = false;
      }
    }
    if (!r.has_value()) {
      if (!subresource_gate_->Admitted(read, may_fetch)) return std::nullopt;
      auto again = read_entry();
      if (!again.has_value()) return std::nullopt;
      if (!CcFlagsAllowShared(again->metadata.origin_cc_flags)) {
        return std::nullopt;
      }
      r.emplace(std::move(*again));
    }
    auto span = r->content();
    return std::string(reinterpret_cast<const char*>(span.data()), span.size());
  };
}

std::vector<PinnedUpstream> BrowserAnalysisManager::AgentPrivateOrigins()
    const {
  return ParseOperatorOrigins(config_.agent_render_private_origins)
      .value_or(std::vector<PinnedUpstream>{});
}

void BrowserAnalysisManager::LogPrivateAddressRefusal(
    const std::string& message) {
  if (handler_ != nullptr) handler_->Warning("%s", message.c_str());
}

bool BrowserAnalysisManager::EnqueueAnalysis(
    const std::string& url, const std::string& hostname,
    const std::string& scheme, const std::string& site, bool host_vouched,
    bool from_module, uint32_t mask, uint64_t template_hash,
    std::string_view original_html,
    std::optional<std::array<std::byte, 32>> origin_html_hash,
    bool force_agent_render) {
  if (shutting_down_) return false;

  AnalysisQueue::Item item;
  item.url = url;
  item.hostname = hostname;
  item.scheme = scheme;
  item.mask = mask;
  item.site = site;
  item.host_vouched = host_vouched;
  item.from_module = from_module;
  item.force_agent_render = force_agent_render;
  // OWN a copy of the raw origin HTML the worker holds at enqueue time.  The
  // analysis runs ASYNCHRONOUSLY (later, on the Chrome pass), so the item must
  // own the bytes — a std::string_view into the worker's buffer would dangle.
  // RunAnalysis uses these bytes instead of re-reading the volatile cache slot
  // 0x08 (which the worker overwrites raw -> optimized, or a concurrent
  // origin-refresh purges, before the async pass runs).
  item.original_html = std::string(original_html);
  // A forced per-URL agent render dedups on the URL (its
  // markdown is per-URL), NOT the structural template hash — else distinct URLs
  // sharing a warm template would collapse in the queue's per-key dedup and
  // starve.  A normal (perf) analysis dedups on the template hash as before.
  // The site is part of that key: the same URL on two sites is two renders.
  item.template_hash =
      force_agent_render
          ? static_cast<uint64_t>(std::hash<std::string>{}(
                site.empty() ? absl::StrCat(scheme, "|", hostname, "|", url)
                             : absl::StrCat(scheme, "|", hostname, "|", url,
                                            "|s=", site)))
          : template_hash;
  // Carry the worker's raw-origin content hash so the
  // store-time re-validation can prove the rendered markdown still matches the
  // live origin (else discard — the v1/v2 race).
  if (origin_html_hash.has_value()) {
    item.has_origin_html_hash = true;
    item.origin_html_hash = *origin_html_hash;
  }

  // The analysis loads the page's cached stylesheets and scripts on the
  // event loop, where the sub-resource gate answers from its memo only.  Ask
  // it here, on the notification's thread, before the item can run: build
  // the same documents and map once with lookups that may ask the server.
  if (subresource_gate_ != nullptr && cache_ != nullptr &&
      !item.original_html.empty()) {
    auto warm = GatedLookup(item, /*may_fetch=*/true);
    browser_internal::AnalysisDocuments docs =
        browser_internal::BuildAnalysisDocuments(
            item.original_html, item.url, item.hostname, item.scheme, warm);
    if (!item.force_agent_render) {
      BuildAnalysisResourceMap(docs.document, item.url, item.hostname,
                               item.scheme, warm, config_.url_normalization);
    }
    // The agent render's document asks the gate in shared-copy mode, the
    // page itself included.
    if (config_.agent_optimize) {
      (void)AgentPageAdmitted(subresource_gate_.get(), item,
                              /*may_fetch=*/true, AgentPrivateOrigins());
      auto warm_shared =
          GatedLookup(item, /*may_fetch=*/true, /*shared_copy=*/true);
      (void)browser_internal::BuildAnalysisDocuments(item.original_html,
                                                     item.url, item.hostname,
                                                     item.scheme, warm_shared);
    }
  }

  bool enqueued = queue_.Enqueue(std::move(item));
  if (enqueued) {
    stats_.queue_enqueued.fetch_add(1, std::memory_order_relaxed);
    {
      std::lock_guard<std::mutex> lock(async_send_mutex_);
      if (analysis_async_initialized_) {
        uv_async_send(&analysis_async_);
      }
    }
  } else {
    stats_.queue_dropped.fetch_add(1, std::memory_order_relaxed);
  }
  return enqueued;
}

void BrowserAnalysisManager::OnAnalysisReady(uv_async_t* handle) {
  auto* mgr = static_cast<BrowserAnalysisManager*>(handle->data);
  if (mgr) mgr->DrainQueue();
}

void BrowserAnalysisManager::DrainQueue() {
  if (analysis_in_progress_ || !chrome_ || !chrome_->running() ||
      shutting_down_) {
    return;
  }

  auto item = queue_.Dequeue();
  if (!item) return;

  analysis_in_progress_ = true;
  RunAnalysis(std::move(*item));
}

void BrowserAnalysisManager::RunAnalysis(AnalysisQueue::Item item) {
  std::string html_content;
  if (!item.original_html.empty()) {
    // Preferred path: the worker handed us the RAW pre-rewrite origin HTML it
    // held at enqueue time (the same bytes kContentHash was computed over).
    // Use it directly and SKIP the cache re-read — by the time this async
    // Chrome pass runs, the worker has already overwritten cache slot 0x08
    // (raw -> optimized) or a concurrent origin-refresh has purged it, so a
    // re-read here would intermittently miss and amputate the markdown variant
    // ("HTML not in cache").  The store-time AgentBindingStillLive check and
    // the serve-time kContentHash equality gate still verify the binding, so
    // using these owned bytes is safe even though the slot may have moved on.
    html_content = std::move(item.original_html);
  } else {
    // Fallback (e.g. a re-analysis/retry path with no HTML supplied): re-read
    // the HTML from cache.  This preserves the prior behavior.
    auto html_read = cache_->ReadBestAlternate(
        item.url, item.hostname, item.scheme, CapabilityMask(),
        /*agent_request_entitled=*/false, item.site);
    if (!html_read.has_value()) {
      if (handler_ != nullptr) {
        handler_->Info("Browser analysis: HTML not in cache for %s",
                       item.url.c_str());
      }
      stats_.analysis_errors.fetch_add(1, std::memory_order_relaxed);
      OnAnalysisComplete();
      return;
    }
    auto html_span = html_read->content();
    html_content.assign(reinterpret_cast<const char*>(html_span.data()),
                        html_span.size());
  }

  if (html_content.empty()) {
    stats_.analysis_errors.fetch_add(1, std::memory_order_relaxed);
    OnAnalysisComplete();
    return;
  }

  // The validation documents are built from the page as the ORIGIN served it.
  // Both rewrites below are for the coverage render's benefit and would poison
  // the comparison: the inliner puts the stylesheet into the document, so a
  // "critical block only" candidate built afterwards would carry the whole
  // sheet as well and every page would validate.
  //
  // Only kept when something can actually use it — this is a second full copy
  // of the page.
  std::string pre_inline_html;
  if (combined_css_builder_) pre_inline_html = html_content;
  std::string agent_html;

  // The documents the renders load: the stylesheets inlined and an absolute
  // <base>; the coverage render's without <noscript> content (see
  // BuildAnalysisDocuments).
  std::string coverage_html;
  std::string coverage_refusal;
  {
    // Use the CSS URL's hostname for cache lookup, falling back to the page
    // hostname for relative URLs.  Nginx stores CSS keyed by the Host header
    // of the CSS request (the CSS URL's own hostname).  Only sheets the
    // server serves to any client are loaded (memo only on this loop).
    // The agent render's document, from shared-copy admitted sheets only.
    if (config_.agent_optimize) {
      auto shared_lookup =
          GatedLookup(item, /*may_fetch=*/false, /*shared_copy=*/true);
      agent_html =
          browser_internal::BuildAnalysisDocuments(
              html_content, item.url, item.hostname, item.scheme, shared_lookup)
              .document;
    }
    auto lookup = GatedLookup(item, /*may_fetch=*/false);
    CssInliningStats css_stats;
    browser_internal::AnalysisDocuments docs =
        browser_internal::BuildAnalysisDocuments(html_content, item.url,
                                                 item.hostname, item.scheme,
                                                 lookup, &css_stats);
    html_content = std::move(docs.document);
    coverage_html = std::move(docs.coverage_document);
    coverage_refusal = std::move(docs.coverage_refusal);
    stats_.css_inlining_attempted.fetch_add(1, std::memory_order_relaxed);
    stats_.css_inlining_stylesheets_found.fetch_add(css_stats.stylesheets_found,
                                                    std::memory_order_relaxed);
    stats_.css_inlining_stylesheets_cached.fetch_add(
        css_stats.stylesheets_cached, std::memory_order_relaxed);
    stats_.css_inlining_bytes_inlined.fetch_add(css_stats.bytes_inlined,
                                                std::memory_order_relaxed);
  }

  // Set up analysis context.
  current_analysis_ = std::make_unique<AnalysisContext>();
  current_analysis_->item = std::move(item);
  current_analysis_->html_content = std::move(html_content);
  current_analysis_->agent_html = std::move(agent_html);
  current_analysis_->coverage_html = std::move(coverage_html);
  current_analysis_->coverage_refusal = std::move(coverage_refusal);
  current_analysis_->pre_inline_html = std::move(pre_inline_html);
  if (!current_analysis_->coverage_refusal.empty()) {
    stats_.noscript_strip_refusals.fetch_add(1, std::memory_order_relaxed);
    if (handler_ != nullptr) {
      handler_->Info(
          "Browser analysis: CSS coverage skipped for %s: %s — the page is "
          "treated as if its CSS had not been analysed",
          current_analysis_->item.url.c_str(),
          current_analysis_->coverage_refusal.c_str());
    }
  }

  // A forced per-URL agent render (warm template — the
  // perf profile already exists) does ONLY the agent render, skipping the
  // per-viewport perf analysis AND the profile store (OnAgentRenderDone does
  // not StoreProfile).  This builds the markdown for a cold URL of a warm
  // template, which the perf-driven enqueue gate would otherwise never reach.
  if (current_analysis_->item.force_agent_render) {
    if (config_.agent_optimize && chrome_ && chrome_->running()) {
      RunAgentRender();  // -> OnAgentRenderDone -> WriteAgentAlternate -> done
    } else {
      OnAnalysisComplete();
    }
    return;
  }

  // Build the shared analysis resource map (same-origin cached script bytes)
  // once per item; both analysis passes serve from it.  Unmapped scripts stay
  // blocked in the offline browser and classify kNoCoverageData — never
  // deferred.  Skipped for forced agent renders above (their map stays empty
  // by contract).
  if (cache_ != nullptr) {
    const AnalysisQueue::Item& cur = current_analysis_->item;
    // The map builder hands over the cache-normalized path+query form, which
    // never carries a host, so the page hostname keys the read; the
    // UrlHostname branch is defensive only.  Only scripts the server serves
    // to any client are mapped (memo only on this loop).
    auto lookup = GatedLookup(cur, /*may_fetch=*/false);
    AnalysisResourceMapStats map_stats;
    current_analysis_->analysis_resources =
        std::make_shared<const PageAnalyzer::ResourceMap>(
            BuildAnalysisResourceMap(current_analysis_->html_content, cur.url,
                                     cur.hostname, cur.scheme, lookup,
                                     config_.url_normalization, &map_stats));
    current_analysis_->same_origin_scripts_uncached =
        map_stats.scripts_uncached_same_origin > 0;
    stats_.script_map_scripts_found.fetch_add(map_stats.scripts_found,
                                              std::memory_order_relaxed);
    stats_.script_map_scripts_cached.fetch_add(map_stats.scripts_cached,
                                               std::memory_order_relaxed);
    stats_.script_map_scripts_uncached_same_origin.fetch_add(
        map_stats.scripts_uncached_same_origin, std::memory_order_relaxed);
    stats_.script_map_scripts_empty_same_origin.fetch_add(
        map_stats.scripts_empty_same_origin, std::memory_order_relaxed);
    stats_.script_map_scripts_cross_origin.fetch_add(
        map_stats.scripts_cross_origin, std::memory_order_relaxed);
    stats_.script_map_scripts_oversize.fetch_add(map_stats.scripts_oversize,
                                                 std::memory_order_relaxed);
    stats_.script_map_bytes_mapped.fetch_add(map_stats.bytes_mapped,
                                             std::memory_order_relaxed);
    if (map_stats.scripts_uncached_same_origin > 0 && handler_ != nullptr) {
      handler_->Info(
          "Browser analysis: %zu same-origin script(s) not yet cached for %s "
          "(profile TTL shortened for re-analysis)",
          map_stats.scripts_uncached_same_origin, cur.url.c_str());
    }
  }

  // Determine first viewport from notification mask.
  CapabilityMask mask = CapabilityMask::Decode(current_analysis_->item.mask);
  // Build viewport schedule: requested viewport first, then the
  // remaining two in order (0=Mobile, 1=Tablet, 2=Desktop).
  int first_viewport = ViewportToIndex(mask.viewport());
  current_analysis_->viewport_order[0] = first_viewport;
  int slot = 1;
  for (int i = 0; i < AnalysisContext::kNumViewports; ++i) {
    if (i != first_viewport) {
      current_analysis_->viewport_order[slot++] = i;
    }
  }
  current_analysis_->viewports_done = 0;
  current_analysis_->viewport_index = current_analysis_->viewport_order[0];

  // Initialize profile metadata.
  current_analysis_->profile.template_hash_hex =
      absl::StrFormat("%016x", current_analysis_->item.template_hash);
  current_analysis_->profile.analyzed_url = current_analysis_->item.url;
  auto now = std::chrono::system_clock::now();
  current_analysis_->profile.created_at =
      std::chrono::duration_cast<std::chrono::seconds>(now.time_since_epoch())
          .count();
  // Cold-script TTL heal: shortened expiry re-analyzes the template once the
  // same-origin scripts land in cache (expiry -> LookupProfile drops the
  // entry -> next notification re-enqueues).
  current_analysis_->profile.expires_at = browser_internal::ProfileExpiryFor(
      current_analysis_->profile.created_at,
      config_.browser_profile_ttl_seconds,
      current_analysis_->same_origin_scripts_uncached);

  PrepareValidationInputs();

  RunViewportAnalysis();
}

void BrowserAnalysisManager::PrepareValidationInputs() {
  AnalysisContext& ctx = *current_analysis_;
  browser_internal::ValidationInputs inputs =
      browser_internal::BuildValidationInputs(
          combined_css_builder_, ctx.item.url, ctx.item.hostname,
          ctx.item.scheme, ctx.pre_inline_html, ctx.item.site,
          ctx.item.host_vouched, ctx.item.from_module);
  ctx.validation_inputs_ready = inputs.ready;
  ctx.pre_inline_elements = std::move(inputs.elements);
  ctx.combined_css = std::move(inputs.combined_css);
  ctx.layer_order = std::move(inputs.layer_order);
  ctx.noscript_affects_render = inputs.noscript_affects_render;

  if (!inputs.ready && handler_ != nullptr) {
    handler_->Info(
        "Critical-CSS validation skipped for %s: %s — the stylesheet stays "
        "render-blocking",
        ctx.item.url.c_str(),
        browser_internal::ValidationInputSkipMessage(inputs.skip));
  }
}

void BrowserAnalysisManager::RunViewportAnalysis() {
  if (!chrome_ || !chrome_->running() || !current_analysis_) {
    OnAnalysisComplete();
    return;
  }

  int idx = current_analysis_->viewport_index;
  uint32_t width = AnalysisContext::kViewportWidths[idx];
  uint32_t height = AnalysisContext::kViewportHeights[idx];

  // Per VIEWPORT, not per item: viewport N's measurement says nothing about
  // viewport N+1's fold, so this must fall back to "unmeasured" every time.
  current_analysis_->fold_measured = false;

  if (handler_ != nullptr) {
    handler_->Info("Browser analysis: extracting CSS at %ux%u for %s", width,
                   height, current_analysis_->item.url.c_str());
  }

  // Fail closed: a document the <noscript> removal could not
  // vouch for is not rendered; the viewport goes on as if CSS extraction had
  // failed, so nothing is derived from a coverage that may be of a page cut
  // short.
  if (!current_analysis_->coverage_refusal.empty()) {
    OnCssExtractionDone(
        absl::FailedPreconditionError(current_analysis_->coverage_refusal));
    return;
  }

  // Start with CSS extraction for this viewport.
  css_extractor_->Extract(
      current_analysis_->coverage_html, width, height,
      [this](absl::StatusOr<BrowserCssResult> result) {
        OnCssExtractionDone(std::move(result));
      },
      config_.chrome_page_timeout_ms);
}

void BrowserAnalysisManager::OnCssExtractionDone(
    absl::StatusOr<BrowserCssResult> result) {
  if (!current_analysis_) return;

  int idx = current_analysis_->viewport_index;
  ViewportProfile* vp = GetViewportProfile(current_analysis_->profile, idx);

  if (result.ok()) {
    vp->critical_css = std::move(result->critical_css);
    vp->css_coverage_ratio = result->coverage_ratio;
    vp->total_css_bytes = result->total_css_bytes;
    vp->unused_css_bytes =
        (result->total_css_bytes > result->critical_css_bytes)
            ? result->total_css_bytes - result->critical_css_bytes
            : 0;
    if (handler_) {
      handler_->Info(
          "Browser CSS extracted: %zu bytes critical (%.1f%% coverage)",
          result->critical_css_bytes, result->coverage_ratio * 100.0f);
    }
  } else {
    if (handler_) {
      handler_->Warning("Browser CSS extraction failed: %s",
                        std::string(result.status().message()).c_str());
    }
    // Continue with page analysis even if CSS extraction failed.
  }

  // A renderer, not necessarily a browser: under test the injected page-
  // analysis runner IS the renderer, which is what lets the step order below be
  // asserted without Chrome.
  if (!page_analysis_runner_ && (!chrome_ || !chrome_->running())) {
    // Chrome died during CSS extraction.
    OnAnalysisComplete();
    return;
  }

  // Page analysis BEFORE validation, deliberately: it is what measures which
  // elements are above this viewport's fold, and the validation must derive the
  // block it judges from that same fold — otherwise it confirms a block the
  // serve path will not produce.  See PrepareCriticalCssValidation.
  RunPageAnalysisForViewport();
}

void BrowserAnalysisManager::RunCriticalCssValidation() {
  // Every other step of the pipeline checks this; so does this one now.
  if (!current_analysis_) return;
  AnalysisContext& ctx = *current_analysis_;
  // No usable inputs for this item at all — PrepareValidationInputs said why,
  // once. Saying it again per viewport would be three copies of one fact.
  if (!ctx.validation_inputs_ready) {
    AdvanceToNextViewport();
    return;
  }

  // The ordering guarantee, enforced rather than assumed: this viewport's fold
  // must already be measured, because the block judged here has to be the block
  // the serve path derives from that same fold. If validation is ever moved
  // back ahead of page analysis, this refuses instead of confirming a block
  // nobody is served — the failure becomes a lost deferral, not a false
  // confirmation. See AnalysisContext::fold_measured.
  if (!ctx.fold_measured) {
    if (handler_ != nullptr) {
      handler_->Info(
          "Critical-CSS validation skipped for %s at %ux%u: %s — the "
          "stylesheet stays render-blocking",
          ctx.item.url.c_str(),
          AnalysisContext::kViewportWidths[ctx.viewport_index],
          AnalysisContext::kViewportHeights[ctx.viewport_index],
          browser_internal::ValidationSkipMessage(
              browser_internal::ValidationSkip::kFoldNotMeasured));
    }
    AdvanceToNextViewport();
    return;
  }

  const int idx = ctx.viewport_index;
  ViewportProfile* vp = GetViewportProfile(ctx.profile, idx);
  const uint32_t width = AnalysisContext::kViewportWidths[idx];
  const uint32_t height = AnalysisContext::kViewportHeights[idx];
  // A renderer, not necessarily a browser: under test the injected runner IS
  // the renderer, and the decisions this function makes are the same either
  // way.
  const bool renderer_available =
      static_cast<bool>(validation_runner_) ||
      (chrome_ && chrome_->running() && validation_gate_);

  browser_internal::ValidationRequest request =
      browser_internal::PrepareCriticalCssValidation(
          *vp, ctx.pre_inline_elements, ctx.combined_css, ctx.pre_inline_html,
          IndexToViewport(idx), renderer_available, ctx.layer_order);

  if (!request.run) {
    // Deliberately no record of any kind: the serve path reads the absence of
    // one as "keep the stylesheet render-blocking", which is what a viewport
    // nobody looked at deserves.
    if (handler_ != nullptr) {
      handler_->Info(
          "Critical-CSS validation skipped for %s at %ux%u: %s — the "
          "stylesheet stays render-blocking",
          ctx.item.url.c_str(), width, height,
          browser_internal::ValidationRefusalMessage(request).c_str());
    }
    AdvanceToNextViewport();
    return;
  }

  auto on_verdict = [this, candidate = request.candidate_critical_css,
                     anonymous_layers_dropped =
                         request.anonymous_layers_dropped](
                        ValidationVerdict verdict) mutable {
    OnCriticalCssValidationDone(std::move(candidate), anonymous_layers_dropped,
                                std::move(verdict));
  };

  if (validation_runner_) {
    validation_runner_(request.docs, width, height, std::move(on_verdict));
    return;
  }
  ValidateCriticalCss(validation_gate_.get(), request.docs, width, height,
                      kDefaultValidationDiffThreshold, std::move(on_verdict));
}

ViewportProfile BrowserAnalysisManager::TestValidateOneViewport(
    const AnalysisQueue::Item& item, const std::string& pre_inline_html,
    const ViewportProfile& seed, int viewport_index, bool fold_measured) {
  current_analysis_ = std::make_unique<AnalysisContext>();
  current_analysis_->item = item;
  current_analysis_->pre_inline_html = pre_inline_html;
  current_analysis_->viewport_index = viewport_index;
  *GetViewportProfile(current_analysis_->profile, viewport_index) = seed;
  // The seed IS this viewport's measured fold, so the production default says
  // page analysis has run.  A caller passing false is exercising the ordering
  // guard.
  current_analysis_->fold_measured = fold_measured;

  PrepareValidationInputs();

  validation_only_ = true;
  RunCriticalCssValidation();
  validation_only_ = false;

  ViewportProfile out =
      *GetViewportProfile(current_analysis_->profile, viewport_index);
  current_analysis_.reset();
  return out;
}

ViewportProfile BrowserAnalysisManager::TestRunViewportFromCssExtraction(
    const AnalysisQueue::Item& item, const std::string& pre_inline_html,
    const ViewportProfile& seed, int viewport_index) {
  current_analysis_ = std::make_unique<AnalysisContext>();
  current_analysis_->item = item;
  current_analysis_->pre_inline_html = pre_inline_html;
  current_analysis_->viewport_index = viewport_index;
  // Deliberately NOT set here: whether the fold has been measured by the time
  // validation runs is exactly what driving the real pipeline decides.
  current_analysis_->fold_measured = false;

  PrepareValidationInputs();

  single_viewport_only_ = true;
  // Through the REAL continuation, so which step it hands off to is under test.
  BrowserCssResult css;
  css.critical_css = seed.critical_css;
  css.coverage_ratio = seed.css_coverage_ratio;
  css.total_css_bytes = seed.total_css_bytes;
  css.critical_css_bytes = seed.critical_css.size();
  OnCssExtractionDone(std::move(css));
  single_viewport_only_ = false;

  ViewportProfile out =
      *GetViewportProfile(current_analysis_->profile, viewport_index);
  current_analysis_.reset();
  return out;
}

void BrowserAnalysisManager::OnCriticalCssValidationDone(
    std::string candidate_critical_css, bool anonymous_layers_dropped,
    ValidationVerdict verdict) {
  if (!current_analysis_) return;
  AnalysisContext& ctx = *current_analysis_;
  const int idx = ctx.viewport_index;
  ViewportProfile* vp = GetViewportProfile(ctx.profile, idx);

  browser_internal::ApplyValidationVerdict(
      *vp, verdict, candidate_critical_css, ctx.combined_css,
      ValidationBindingFor(ctx.layer_order.ValidationBinding(ctx.combined_css),
                           ctx.noscript_affects_render, ctx.combined_css,
                           browser_internal::IndexToViewport(idx),
                           anonymous_layers_dropped));

  if (handler_ != nullptr) {
    if (vp->critical_css_validated) {
      handler_->Info(
          "Critical-CSS confirmed for %s at %ux%u: above-the-fold appearance "
          "unchanged (%.4f of fold pixels differ)",
          ctx.item.url.c_str(), AnalysisContext::kViewportWidths[idx],
          AnalysisContext::kViewportHeights[idx], verdict.diff_ratio);
    } else {
      handler_->Info(
          "Critical-CSS NOT confirmed for %s at %ux%u: %s — the stylesheet "
          "stays render-blocking",
          ctx.item.url.c_str(), AnalysisContext::kViewportWidths[idx],
          AnalysisContext::kViewportHeights[idx],
          verdict.failure_reason.c_str());
    }
  }

  AdvanceToNextViewport();
}

void BrowserAnalysisManager::RunPageAnalysisForViewport() {
  // TestValidateOneViewport drives the validation step alone, with no browser
  // behind page_analyzer_.  Both this and the AdvanceToNextViewport guard are
  // needed: this one stops the seam from reaching a null renderer at all.
  if (validation_only_) return;
  if (!current_analysis_) return;

  const int idx = current_analysis_->viewport_index;
  uint32_t width = AnalysisContext::kViewportWidths[idx];
  uint32_t height = AnalysisContext::kViewportHeights[idx];

  if (page_analysis_runner_) {
    page_analysis_runner_(width, height,
                          [this](absl::StatusOr<PageAnalysisResult> result) {
                            OnPageAnalysisDone(std::move(result));
                          });
    return;
  }

  if (!chrome_ || !chrome_->running()) {
    OnAnalysisComplete();
    return;
  }

  // Serve the shared same-origin script map via Fetch interception — scripts
  // now execute during page analysis, so LCP/CLS/fold telemetry is collected
  // from a script-executed render (intended behavior change).  Image bytes
  // are not mapped yet (follow-up issue).
  std::shared_ptr<const PageAnalyzer::ResourceMap> resources =
      current_analysis_->analysis_resources;
  if (!resources) resources = std::make_shared<PageAnalyzer::ResourceMap>();

  // NOTE: the agent_optimize render is deliberately NOT run in the per-viewport
  // perf loop.  It is a separate one-shot pass (RunAgentRender, after the perf
  // loop completes) so the browser-perf profile is always collected from the
  // offline render — turning agent_optimize on must never change the perf
  // treatment (LCP/lazy-load/image-sizing) served to ordinary browsers, and the
  // viewport-independent markdown is rendered once, not 3x.
  page_analyzer_->Analyze(
      current_analysis_->html_content, resources, width, height,
      [this](absl::StatusOr<PageAnalysisResult> result) {
        OnPageAnalysisDone(std::move(result));
      },
      config_.chrome_page_timeout_ms);
}

void BrowserAnalysisManager::OnPageAnalysisDone(
    absl::StatusOr<PageAnalysisResult> result) {
  if (!current_analysis_) return;

  int idx = current_analysis_->viewport_index;
  ViewportProfile* vp = GetViewportProfile(current_analysis_->profile, idx);

  if (result.ok()) {
    // Surfaced, not swallowed: the descriptor list is capped, so on a very
    // large page the measured fold is a PREFIX of what the browser saw and the
    // critical block derived from it is correspondingly under-inclusive. That
    // degrades toward the old heuristic rather than breaking anything, but an
    // operator looking at why a page's above-the-fold CSS is thin needs to be
    // able to see it.
    if (result->elements_truncated && handler_ != nullptr) {
      handler_->Info(
          "Above-the-fold element list truncated at %zu descriptors for %s at "
          "%ux%u — the measured fold is partial and the critical CSS derived "
          "from it may be under-inclusive",
          PageAnalysisResult::kMaxElementDescriptors,
          current_analysis_->item.url.c_str(),
          AnalysisContext::kViewportWidths[idx],
          AnalysisContext::kViewportHeights[idx]);
    }
    browser_internal::PopulateViewportFromPageAnalysis(*result, *vp);
  } else {
    if (handler_) {
      handler_->Warning("Page analysis failed: %s",
                        std::string(result.status().message()).c_str());
    }
    stats_.analysis_errors.fetch_add(1, std::memory_order_relaxed);
  }

  // Increment page count for Chrome recycling (once per viewport,
  // after both CSS extraction and page analysis are complete).
  if (chrome_ && chrome_->IncrementPageCount()) {
    if (handler_) {
      handler_->Info("Chrome recycle threshold reached, will restart");
    }
  }

  // The fold for this viewport is now measured, so the block the serve path
  // will inline can be derived and put in front of a browser.  Set even when
  // the render FAILED: the serve path will then read the same empty fold from
  // the stored profile, so the two still agree — which is the property this
  // flag protects.  Continues to the next viewport on every path, including
  // every refusal.
  current_analysis_->fold_measured = true;
  RunCriticalCssValidation();
}

// Tail of one viewport: move the schedule on, or finish the item.
void BrowserAnalysisManager::AdvanceToNextViewport() {
  if (validation_only_) return;
  if (single_viewport_only_) return;
  if (!current_analysis_) return;

  current_analysis_->viewports_done++;

  if (current_analysis_->viewports_done < AnalysisContext::kNumViewports &&
      chrome_ && chrome_->running()) {
    current_analysis_->viewport_index =
        current_analysis_->viewport_order[current_analysis_->viewports_done];
    RunViewportAnalysis();
  } else {
    // All viewports done (or Chrome died).
    // Run script analysis if enabled (once, at desktop viewport).
    if (config_.enable_script_analysis && chrome_ && chrome_->running() &&
        script_analyzer_) {
      RunScriptAnalysis();
    } else {
      FinishAnalysis();
    }
  }
}

void BrowserAnalysisManager::RunScriptAnalysis() {
  if (!current_analysis_) return;

  // The shared same-origin script map: a mapped script is fetched+compiled in
  // the analysis browser and earns real coverage evidence; an unmapped one is
  // blocked and classifies kNoCoverageData.
  std::shared_ptr<const PageAnalyzer::ResourceMap> resources =
      current_analysis_->analysis_resources;
  if (!resources) resources = std::make_shared<PageAnalyzer::ResourceMap>();

  // Run at desktop viewport (1440x900) — script behavior is
  // viewport-independent, so we only need one pass.
  script_analyzer_->Analyze(
      current_analysis_->html_content, resources, 1440, 900,
      [this](absl::StatusOr<ScriptCoverageResult> result) {
        OnScriptAnalysisDone(std::move(result));
      },
      ScriptCoverageAnalyzer::kDefaultTimeoutMs);
}

void BrowserAnalysisManager::OnScriptAnalysisDone(
    absl::StatusOr<ScriptCoverageResult> result) {
  if (!current_analysis_) return;

  if (result.ok()) {
    // Extract deferrable script URLs.  kCandidateForAsync folds into the same
    // defer list as kSafeToDefer — a known async-vs-defer conflation, kept.
    uint64_t no_coverage = 0;
    for (const auto& [url, advice] : result->recommendations) {
      if (browser_internal::ShouldFoldDeferRecommendation(url, advice)) {
        current_analysis_->profile.defer_safe_scripts.push_back(url);
      } else if (advice == DeferralAdvice::kNoCoverageData) {
        ++no_coverage;
      }
    }
    stats_.scripts_analyzed.fetch_add(result->scripts.size(),
                                      std::memory_order_relaxed);
    stats_.scripts_deferrable.fetch_add(
        current_analysis_->profile.defer_safe_scripts.size(),
        std::memory_order_relaxed);
    stats_.scripts_no_coverage.fetch_add(no_coverage,
                                         std::memory_order_relaxed);
    stats_.script_fetches_served.fetch_add(result->fetches_served,
                                           std::memory_order_relaxed);
    stats_.script_fetches_blocked.fetch_add(result->fetches_blocked,
                                            std::memory_order_relaxed);

    if (handler_) {
      handler_->Info("Script analysis: %zu scripts, %zu deferrable",
                     result->scripts.size(),
                     current_analysis_->profile.defer_safe_scripts.size());
    }
  } else {
    // Script analysis failure is non-fatal — proceed with empty
    // defer_safe_scripts.
    if (handler_) {
      handler_->Warning("Script analysis failed: %s",
                        std::string(result.status().message()).c_str());
    }
  }

  // Increment page count for Chrome recycling.
  if (chrome_ && chrome_->IncrementPageCount()) {
    if (handler_) {
      handler_->Info("Chrome recycle threshold reached after script analysis");
    }
  }

  // Store the profile, run the one-shot agent render (if enabled), complete.
  FinishAnalysis();
}

// Common tail for the perf + (optional) script passes.  When a re-analysis is
// scheduled (empty critical CSS), skip both the profile store and the agent
// render — they re-run on the retry.  Otherwise store the perf profile, then
// run the one-shot agent_optimize render OUTSIDE the per-viewport
// loop so the perf profile is collected from the offline render (no egress) and
// is byte-identical whether or not agent_optimize is on.
void BrowserAnalysisManager::FinishAnalysis() {
  if (!current_analysis_) return;
  if (MaybeScheduleReanalysis()) {
    OnAnalysisComplete();
    return;
  }

  // Degraded-pass guard.  The critical-CSS profile and the agent markdown come
  // from the same Chrome pass; when it degrades the FCP coverage over-reports
  // and the "critical" CSS is really the whole sheet.  Don't let that clobber a
  // good profile we already have — keep the prior one and let the next analysis
  // win.  If no prior profile exists, store this one anyway (degraded > none for
  // a first-seen template).  Either way still run the agent render below: the
  // markdown variant is keyed per-URL and independent of the profile.
  bool skip_store = false;
  if (LooksLikeDegradedProfile(current_analysis_->profile) &&
      template_detector_.HasProfile(current_analysis_->item.template_hash)) {
    skip_store = true;
    if (handler_ != nullptr) {
      handler_->Info(
          "browser analysis: degraded profile (coverage>=%.2f), keeping prior "
          "profile for template %s",
          browser_internal::kDegradedCoverageThreshold,
          current_analysis_->profile.template_hash_hex.c_str());
    }
  }

  if (!skip_store) {
    StoreProfile(current_analysis_->profile);
  }
  // The agent render fetches from the item's hostname, so it runs only for a
  // notification whose sender vouched for that host.
  if (MayRunAgentRender(current_analysis_->item, config_.agent_optimize,
                        chrome_ && chrome_->running())) {
    RunAgentRender();  // → OnAgentRenderDone → OnAnalysisComplete
    return;
  }
  OnAnalysisComplete();
}

// One-shot, viewport-independent agent_optimize render.  Renders
// the page with the network-enabled, IP-pinned/SSRF-gated fetch policy and reads
// the rendered DOM → markdown (written as the kAgentMarkdown variant).  The G1
// upstream pin is THIS render's own origin (scheme://hostname from the queued
// item — never page-supplied); allow_hosts are operator config.  The injected
// <base href> makes relative subresources resolve here too, so they are now
// fetched via the AgentFetcher under the unchanged fail-closed FetchPolicy gate
// (own-origin requests pin to kProxyUpstream) — fetch VOLUME rises, the
// reachable surface does not.
void BrowserAnalysisManager::RunAgentRender() {
  if (!current_analysis_) {
    OnAnalysisComplete();
    return;
  }
  auto resources = std::make_shared<PageAnalyzer::ResourceMap>();
  auto policy = std::make_shared<FetchPolicy>();
  const AnalysisQueue::Item& item = current_analysis_->item;
  if (!item.hostname.empty()) {
    const std::string scheme = item.scheme.empty() ? "https" : item.scheme;
    std::optional<PinnedUpstream> up =
        ParseUpstream(absl::StrCat(scheme, "://", item.hostname));
    if (up.has_value()) policy->pinned_upstreams.push_back(*up);
  }
  policy->allow_hosts = config_.agent_render_allow_hosts;
  auto opts = std::make_shared<AgentRenderOptions>();
  opts->policy = std::move(policy);
  opts->deps.spawn = RealCurlSpawn();
  opts->deps.resolve = RealHostResolver();
  // The answers go into a copy served to other clients.
  opts->deps.shared_copy = true;
  opts->deps.private_address_origins =
      ParseOperatorOrigins(config_.agent_render_private_origins)
          .value_or(std::vector<PinnedUpstream>{});
  opts->deps.on_private_address_refusal = [this](const std::string& m) {
    LogPrivateAddressRefusal(m);
  };

  // Desktop viewport — the rendered-DOM markdown is viewport-independent
  // (mirrors RunScriptAnalysis).
  // The agent render loads only the shared-copy document, and only for a
  // page whose own anonymous answer may be shared (memo only on this loop;
  // warmed at enqueue).
  if (current_analysis_->agent_html.empty()) {
    OnAnalysisComplete();
    return;
  }
  if (!AgentPageAdmitted(subresource_gate_.get(), current_analysis_->item,
                         /*may_fetch=*/false, AgentPrivateOrigins())) {
    stats_.agent_render_page_refused.fetch_add(1, std::memory_order_relaxed);
    if (handler_ != nullptr) {
      handler_->Info(
          "agent_optimize: no Markdown copy for %s: its answer is not "
          "confirmed as shareable",
          current_analysis_->item.url.c_str());
    }
    OnAnalysisComplete();
    return;
  }
  page_analyzer_->Analyze(
      current_analysis_->agent_html, resources, 1440, 900,
      [this](absl::StatusOr<PageAnalysisResult> result) {
        OnAgentRenderDone(std::move(result));
      },
      config_.chrome_page_timeout_ms, std::move(opts));
}

void BrowserAnalysisManager::OnAgentRenderDone(
    absl::StatusOr<PageAnalysisResult> result) {
  if (!current_analysis_) return;

  // Write the rendered-DOM markdown as the kAgentMarkdown variant keyed on the
  // request's OWN url (NOT the template url — spec §2.3), so an entitled
  // `Accept: text/markdown` request HITs it at the same URL.  Best-effort: a
  // write failure is non-fatal (the request falls back to the normal best
  // variant).
  if (result.ok() && !result->agent_markdown.empty()) {
    const AnalysisQueue::Item& item = current_analysis_->item;
    // Store-time re-validation.  Refuse to store if the
    // origin advanced underneath the render (or no binding was captured) — the
    // v1-render-overwrites-v2 race.  Fail-closed: never bind rendered content
    // to a stale/wrong origin hash.
    if (!AgentPageAdmitted(subresource_gate_.get(), item, /*may_fetch=*/false,
                           AgentPrivateOrigins())) {
      // Checked again at the write: the answer must still be shareable.
      stats_.agent_render_page_refused.fetch_add(1, std::memory_order_relaxed);
      if (handler_) {
        handler_->Info(
            "agent_optimize: discarding markdown for %s (answer no longer "
            "confirmed as shareable)",
            item.url.c_str());
      }
    } else if (!AgentBindingStillLive(*cache_, item)) {
      if (handler_) {
        handler_->Info(
            "agent_optimize: discarding markdown for %s (origin advanced or "
            "unbound since render start)",
            item.url.c_str());
      }
    } else {
      AlternateMetadata meta;
      meta.full_mask =
          static_cast<uint32_t>(SentinelId::kAgentMarkdown);  // 0x7C
      meta.content_type = ContentType::kOther;  // served verbatim via origin ct
      meta.origin_content_type = "text/markdown";
      meta.flags |= AlternateMetadata::kFlagWorkerProcessed;
      // Stamp the binding the serve-time equality gate
      // checks against the live kContentHash.
      meta.origin_html_hash = item.origin_html_hash;
      // NOTE: origin_content_length is left 0, so the nginx serve-stats gate
      // (kFlagWorkerProcessed && origin_content_length>0) deliberately does NOT
      // count markdown serves yet — agent serve-stats parity is deferred
      // follow-up work, not a bug.
      auto wh = cache_->WriteAgentAlternate(
          item.url, item.hostname, item.scheme, result->agent_markdown.size(),
          meta, item.site);
      if (wh.has_value()) {
        (void)wh->write_sync(std::as_bytes(std::span(
            result->agent_markdown.data(), result->agent_markdown.size())));
        (void)wh->close_sync();
        // Write-around RAM tier (issue #1126): the worker reads this slot
        // in-process (AgentMarkdownNeedsBuild, the llms.txt variant_reader),
        // and this overwrite does NOT evict a previously admitted RAM copy.
        // Severity is low — overwrites require a live binding (same
        // origin_html_hash), so no rebuild loop is possible; the concrete
        // harm is the llms.txt builder synthesizing from stale markdown
        // until RAM LRU eviction — but the fix is the same one-liner.
        // Evict post-commit.
        cache_->EvictAlternateFromRamCache(
            item.url, item.hostname, item.scheme,
            static_cast<AlternateId>(SentinelId::kAgentMarkdown), item.site);
        if (handler_) {
          handler_->Info("agent_optimize: wrote %zu bytes of markdown for %s",
                         result->agent_markdown.size(), item.url.c_str());
        }
        // This kAgentMarkdown write bypasses HandleNotification's count guard,
        // so notify the worker to refresh the URL's cached alternate count
        // (otherwise /v1/cache/urls would under-report agent-optimized pages
        // by one until the next normal notification).
        if (on_agent_alternate_written_) {
          on_agent_alternate_written_(item.url, item.hostname, item.scheme,
                                      item.site);
        }
      } else if (handler_) {
        handler_->Warning("agent_optimize: markdown write failed for %s",
                          item.url.c_str());
      }
    }
  } else if (!result.ok() && handler_) {
    handler_->Warning("agent_optimize: render failed for %s: %s",
                      current_analysis_->item.url.c_str(),
                      std::string(result.status().message()).c_str());
  }

  // Chrome recycle bookkeeping (the agent render is another page load).
  if (chrome_ && chrome_->IncrementPageCount()) {
    if (handler_) {
      handler_->Info("Chrome recycle threshold reached after agent render");
    }
  }

  OnAnalysisComplete();
}

void BrowserAnalysisManager::StoreProfile(const OptimizationProfile& profile) {
  std::string json = profile.ToJson();
  if (json.empty()) return;

  std::string cache_url =
      OptimizationProfile::CacheUrl(current_analysis_->item.template_hash);
  const std::string& site = current_analysis_->item.site;
  auto wh = cache_->WriteSentinel(
      cache_url, std::string(OptimizationProfile::kProfileHostname), "https",
      SentinelId::kBrowserProfile, json.size(), site);
  if (!wh) return;
  // Profile storage is best-effort; write failure is non-fatal.
  (void)wh->write_sync(std::as_bytes(std::span(json.data(), json.size())));
  (void)wh->close_sync();
  // Cyclone's RAM tier is write-around (reads populate it, writes do NOT
  // evict): LookupProfile reads this slot in-process before a refresh
  // overwrites it, so without a post-commit eviction the stale RAM copy is
  // served indefinitely — and once its expires_at lapses LookupProfile even
  // RemoveProfile()s the template entry, so the fresh on-disk profile is
  // never used (issue #1126).  Evict the just-committed slot post-commit.
  cache_->EvictAlternateFromRamCache(
      cache_url, std::string(OptimizationProfile::kProfileHostname), "https",
      static_cast<AlternateId>(SentinelId::kBrowserProfile), site);

  // Record in template detector AFTER cache write is committed,
  // so LookupProfile() from worker threads never sees the template
  // entry before the data is available.
  template_detector_.RecordProfile(current_analysis_->item.template_hash,
                                   profile.analyzed_url);

  stats_.profiles_generated.fetch_add(1, std::memory_order_relaxed);
  stats_.queue_processed.fetch_add(1, std::memory_order_relaxed);

  if (handler_ != nullptr) {
    handler_->Info("Stored browser profile for template %s (%zu bytes)",
                   profile.template_hash_hex.c_str(), json.size());
  }
}

void BrowserAnalysisManager::TestStoreProfile(
    uint64_t template_hash, const OptimizationProfile& profile,
    std::string_view site) {
  // Mirror StoreProfile's persistence, but keyed on the supplied template_hash
  // (StoreProfile reads current_analysis_->item.template_hash, which only exists
  // mid-pipeline).  Test-only.
  if (cache_ == nullptr) return;
  std::string json = profile.ToJson();
  if (json.empty()) return;

  std::string cache_url = OptimizationProfile::CacheUrl(template_hash);
  auto wh = cache_->WriteSentinel(
      cache_url, std::string(OptimizationProfile::kProfileHostname), "https",
      SentinelId::kBrowserProfile, json.size(), site);
  if (!wh) return;
  (void)wh->write_sync(std::as_bytes(std::span(json.data(), json.size())));
  (void)wh->close_sync();
  // Same write-around RAM-tier post-commit eviction as StoreProfile
  // (issue #1126) — this mirror must behave identically under test.
  cache_->EvictAlternateFromRamCache(
      cache_url, std::string(OptimizationProfile::kProfileHostname), "https",
      static_cast<AlternateId>(SentinelId::kBrowserProfile), site);

  // Record AFTER the cache write commits, so LookupProfile never sees the
  // template entry before the data is available (matches StoreProfile ordering).
  template_detector_.RecordProfile(template_hash, profile.analyzed_url);
}

std::optional<OptimizationProfile> BrowserAnalysisManager::LookupProfile(
    uint64_t template_hash, std::string_view site) {
  if (!template_detector_.HasProfile(template_hash)) {
    return std::nullopt;
  }

  std::string cache_url = OptimizationProfile::CacheUrl(template_hash);
  auto read_result = cache_->ReadAlternate(
      cache_url, std::string(OptimizationProfile::kProfileHostname), "https",
      static_cast<AlternateId>(SentinelId::kBrowserProfile), site);
  if (!read_result.has_value()) {
    // Cache entry gone (evicted).  Remove stale template entry.  The template
    // record is not per site, so a sited miss only means this site has no
    // profile for the template yet; it leaves the record alone.
    if (site.empty()) template_detector_.RemoveProfile(template_hash);
    return std::nullopt;
  }

  auto content = read_result->content();
  if (content.empty()) return std::nullopt;

  std::string_view json(reinterpret_cast<const char*>(content.data()),
                        content.size());
  auto profile = OptimizationProfile::FromJson(json);
  if (!profile.ok()) {
    if (handler_ != nullptr) {
      handler_->Warning("Profile deserialization failed: %s",
                        std::string(profile.status().message()).c_str());
    }
    return std::nullopt;
  }

  // Check TTL.
  auto now = std::chrono::system_clock::now();
  int64_t now_secs =
      std::chrono::duration_cast<std::chrono::seconds>(now.time_since_epoch())
          .count();
  if (profile->expires_at > 0 && now_secs > profile->expires_at) {
    template_detector_.RemoveProfile(template_hash);
    return std::nullopt;
  }

  stats_.profiles_used.fetch_add(1, std::memory_order_relaxed);
  return std::move(*profile);
}

bool BrowserAnalysisManager::RequestRevalidation(
    const std::string& url, const std::string& hostname,
    const std::string& scheme, const std::string& site, bool host_vouched,
    bool from_module, uint32_t mask, uint64_t template_hash,
    std::string_view original_html,
    std::optional<std::array<std::byte, 32>> origin_html_hash,
    std::chrono::steady_clock::time_point now) {
  if (shutting_down_) return false;
  const auto window = std::chrono::seconds(
      std::max<int64_t>(config_.browser_profile_ttl_seconds, 1));
  const auto spacing = std::chrono::milliseconds(kReanalysisDelayMs);
  {
    std::lock_guard<std::mutex> lock(revalidation_mutex_);
    if (revalidation_budgets_.size() >= kMaxRevalidationBudgets &&
        !revalidation_budgets_.contains(template_hash)) {
      std::erase_if(revalidation_budgets_, [&](const auto& entry) {
        return now - entry.second.window_start >= window;
      });
      // Still full: evict the least recently used budget. Clearing the map
      // would reset every template's budget at once, a way around the cap.
      if (revalidation_budgets_.size() >= kMaxRevalidationBudgets) {
        auto oldest = std::min_element(revalidation_budgets_.begin(),
                                       revalidation_budgets_.end(),
                                       [](const auto& a, const auto& b) {
                                         return a.second.last < b.second.last;
                                       });
        revalidation_budgets_.erase(oldest);
      }
    }
    RevalidationBudget& budget = revalidation_budgets_[template_hash];
    if (budget.requests == 0 || now - budget.window_start >= window) {
      budget.window_start = now;
      budget.requests = 0;
    }
    if (budget.requests >= kMaxReanalysisRetries ||
        (budget.requests > 0 && now - budget.last < spacing)) {
      stats_.revalidations_rate_limited.fetch_add(1, std::memory_order_relaxed);
      return false;
    }
    // Spend the budget before queueing: a full queue is not a reason to try
    // again on the very next request.
    ++budget.requests;
    budget.last = now;
  }
  if (!EnqueueAnalysis(url, hostname, scheme, site, host_vouched, from_module,
                       mask, template_hash, original_html, origin_html_hash)) {
    return false;
  }
  stats_.revalidations_queued.fetch_add(1, std::memory_order_relaxed);
  if (handler_ != nullptr) {
    handler_->Info(
        "Browser re-analysis: validation record for %s does not match the "
        "served stylesheet, queued a new analysis",
        url.c_str());
  }
  return true;
}

bool BrowserAnalysisManager::MaybeScheduleReanalysis() {
  if (!current_analysis_ || shutting_down_) return false;

  // Two transient failure modes warrant another pass: all viewports produced
  // 0 bytes of critical CSS, OR the pass looks degraded (FCP coverage
  // over-report — see LooksLikeDegradedProfile).  The degraded trigger only
  // fires when there is NO prior good profile to fall back on: when one exists,
  // FinishAnalysis keeps it (no churn) rather than re-running Chrome.  Both
  // triggers share the kMaxReanalysisRetries cap (carried on item.retry_count),
  // so a pass that stays bad simply gives up — no infinite loop.
  const auto& profile = current_analysis_->profile;
  bool all_empty = profile.mobile.critical_css.empty() &&
                   profile.tablet.critical_css.empty() &&
                   profile.desktop.critical_css.empty();
  bool degraded_first_seen =
      LooksLikeDegradedProfile(profile) &&
      !template_detector_.HasProfile(current_analysis_->item.template_hash);
  if (!all_empty && !degraded_first_seen) return false;

  // Check retry limit.
  int retry = current_analysis_->item.retry_count;
  if (retry >= kMaxReanalysisRetries) {
    if (handler_ != nullptr) {
      handler_->Info(
          "Browser re-analysis: giving up after %d retries for %s (%s)", retry,
          current_analysis_->item.url.c_str(),
          all_empty ? "critical CSS still 0 bytes"
                    : "profile still looks degraded");
    }
    return false;
  }

  // Don't schedule if a re-analysis is already pending.
  if (reanalysis_timer_active_) {
    if (handler_ != nullptr) {
      handler_->Info(
          "Browser re-analysis: timer already active, skipping for %s",
          current_analysis_->item.url.c_str());
    }
    return false;
  }

  // Build the re-analysis item with incremented retry count.  Centralized in
  // BuildReanalysisItem so every required field (notably scheme + the content
  // binding) is carried forward — the retry re-enqueues directly, bypassing the
  // worker's EnqueueAnalysis.
  pending_reanalysis_ = BuildReanalysisItem(current_analysis_->item, retry + 1);

  if (handler_ != nullptr) {
    handler_->Info(
        "Browser re-analysis: %s, scheduling retry %d/%d in %dms for %s",
        all_empty ? "0% critical CSS coverage" : "profile looks degraded",
        retry + 1, kMaxReanalysisRetries, kReanalysisDelayMs,
        current_analysis_->item.url.c_str());
  }

  uv_timer_start(
      &reanalysis_timer_,
      [](uv_timer_t* timer) {
        auto* mgr = static_cast<BrowserAnalysisManager*>(timer->data);
        mgr->OnReanalysisTimer();
      },
      kReanalysisDelayMs, 0);
  reanalysis_timer_active_ = true;
  stats_.reanalyses_scheduled.fetch_add(1, std::memory_order_relaxed);
  return true;
}

void BrowserAnalysisManager::OnReanalysisTimer() {
  reanalysis_timer_active_ = false;

  if (shutting_down_ || !pending_reanalysis_) return;

  AnalysisQueue::Item item = std::move(*pending_reanalysis_);
  pending_reanalysis_.reset();

  if (handler_ != nullptr) {
    handler_->Info(
        "Browser re-analysis: timer fired, re-enqueuing %s (retry %d/%d)",
        item.url.c_str(), item.retry_count, kMaxReanalysisRetries);
  }

  // Enqueue for re-analysis. The queue dedup check uses template_hash;
  // since the item was dequeued earlier, it should accept re-enqueue.
  uint64_t hash = item.template_hash;
  std::string url_for_log = item.url;  // Save before move
  bool enqueued = queue_.Enqueue(std::move(item));
  if (enqueued) {
    // Remove the template from the detector AFTER successful enqueue,
    // so LookupProfile() falls back to heuristics until the retry
    // completes.  If enqueue fails, we leave any existing profile
    // intact rather than creating a gap.
    template_detector_.RemoveProfile(hash);
    stats_.queue_enqueued.fetch_add(1, std::memory_order_relaxed);
    if (analysis_async_initialized_) {
      uv_async_send(&analysis_async_);
    }
  } else if (handler_ != nullptr) {
    handler_->Info("Browser re-analysis: failed to re-enqueue %s (queue full)",
                   url_for_log.c_str());
  }
}

void BrowserAnalysisManager::OnAnalysisComplete() {
  current_analysis_.reset();
  analysis_in_progress_ = false;

  // Try to process next item.
  DrainQueue();
}

bool BrowserAnalysisManager::chrome_running() const {
  return chrome_ && chrome_->running();
}

CdpClient* BrowserAnalysisManager::cdp_client() const {
  if (!chrome_ || !chrome_->running()) return nullptr;
  return chrome_->cdp_client();
}

size_t BrowserAnalysisManager::queue_depth() const { return queue_.size(); }

std::string BrowserAnalysisManager::StatusJson() const {
  const auto& s = stats_;
  return absl::StrCat(
      "{\"enabled\":true,\"chrome_running\":",
      chrome_running() ? "true" : "false",
      ",\"chrome_pid\":", chrome_ ? chrome_->pid() : 0,
      ",\"chrome_rss_mb\":", chrome_ ? chrome_->rss_mb() : 0,
      ",\"queue_depth\":", queue_depth(),
      ",\"analysis_in_progress\":", analysis_in_progress_ ? "true" : "false",
      ",\"templates_tracked\":", template_detector_.size(),
      ",\"profiles_generated\":",
      s.profiles_generated.load(std::memory_order_relaxed),
      ",\"profiles_used\":", s.profiles_used.load(std::memory_order_relaxed),
      ",\"analysis_errors\":",
      s.analysis_errors.load(std::memory_order_relaxed),
      ",\"chrome_restarts\":",
      s.chrome_restarts.load(std::memory_order_relaxed),
      ",\"chrome_crashes\":", s.chrome_crashes.load(std::memory_order_relaxed),
      ",\"chrome_consecutive_failures\":",
      s.chrome_consecutive_failures.load(std::memory_order_relaxed),
      ",\"chrome_restart_delay_ms\":",
      s.chrome_restart_delay_ms.load(std::memory_order_relaxed),
      ",\"queue_enqueued\":", s.queue_enqueued.load(std::memory_order_relaxed),
      ",\"queue_dropped\":", s.queue_dropped.load(std::memory_order_relaxed),
      ",\"queue_processed\":",
      s.queue_processed.load(std::memory_order_relaxed),
      ",\"css_inlining_attempted\":",
      s.css_inlining_attempted.load(std::memory_order_relaxed),
      ",\"css_inlining_stylesheets_found\":",
      s.css_inlining_stylesheets_found.load(std::memory_order_relaxed),
      ",\"css_inlining_stylesheets_cached\":",
      s.css_inlining_stylesheets_cached.load(std::memory_order_relaxed),
      ",\"css_inlining_bytes_inlined\":",
      s.css_inlining_bytes_inlined.load(std::memory_order_relaxed),
      ",\"noscript_strip_refusals\":",
      s.noscript_strip_refusals.load(std::memory_order_relaxed),
      ",\"reanalyses_scheduled\":",
      s.reanalyses_scheduled.load(std::memory_order_relaxed),
      ",\"agent_render_page_refused\":",
      s.agent_render_page_refused.load(std::memory_order_relaxed),
      ",\"revalidations_queued\":",
      s.revalidations_queued.load(std::memory_order_relaxed),
      ",\"revalidations_rate_limited\":",
      s.revalidations_rate_limited.load(std::memory_order_relaxed),
      ",\"scripts_analyzed\":",
      s.scripts_analyzed.load(std::memory_order_relaxed),
      ",\"scripts_deferrable\":",
      s.scripts_deferrable.load(std::memory_order_relaxed),
      ",\"scripts_no_coverage\":",
      s.scripts_no_coverage.load(std::memory_order_relaxed),
      ",\"script_map_scripts_found\":",
      s.script_map_scripts_found.load(std::memory_order_relaxed),
      ",\"script_map_scripts_cached\":",
      s.script_map_scripts_cached.load(std::memory_order_relaxed),
      ",\"script_map_scripts_uncached_same_origin\":",
      s.script_map_scripts_uncached_same_origin.load(std::memory_order_relaxed),
      ",\"script_map_scripts_empty_same_origin\":",
      s.script_map_scripts_empty_same_origin.load(std::memory_order_relaxed),
      ",\"script_map_scripts_cross_origin\":",
      s.script_map_scripts_cross_origin.load(std::memory_order_relaxed),
      ",\"script_map_scripts_oversize\":",
      s.script_map_scripts_oversize.load(std::memory_order_relaxed),
      ",\"script_map_bytes_mapped\":",
      s.script_map_bytes_mapped.load(std::memory_order_relaxed),
      ",\"script_fetches_served\":",
      s.script_fetches_served.load(std::memory_order_relaxed),
      ",\"script_fetches_blocked\":",
      s.script_fetches_blocked.load(std::memory_order_relaxed), "}");
}

}  // namespace pagespeed
