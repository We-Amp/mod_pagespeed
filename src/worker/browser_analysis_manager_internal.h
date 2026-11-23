// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

// Internal helpers for browser_analysis_manager.cc, exposed for unit testing.
//
// NOT part of the public API. Outside the manager itself, include this only
// from test code.

#ifndef PAGESPEED_SRC_WORKER_BROWSER_ANALYSIS_MANAGER_INTERNAL_H_
#define PAGESPEED_SRC_WORKER_BROWSER_ANALYSIS_MANAGER_INTERNAL_H_

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

#include "lib/cache/cache.h"
#include "lib/classify/capability_mask.h"
#include "src/browser/analysis_queue.h"
#include "src/browser/critical_css_validator.h"
#include "src/browser/optimization_profile.h"
#include "src/browser/page_analysis.h"
#include "src/browser/script_coverage_analyzer.h"
#include "src/worker/cascade_layer_order.h"
#include "src/worker/css_cache_inliner.h"
#include "src/worker/html_scanner.h"
#include "src/worker/subresource_gate.h"

namespace pagespeed {
namespace browser_internal {

// A readable reason for a Chrome exit status where one is known, so an
// operator does not have to decode the number.  Empty when there is nothing
// to add (a signal death, or a status this code does not recognise).
// Exit 21 is PROFILE_IN_USE: the profile directory is locked by another
// Chrome instance (see ChromeProcess::ReconcileSingletonLock).
std::string ChromeExitReason(int64_t exit_status, int term_signal);

// Map viewport enum to array index (0=Mobile, 1=Tablet, 2=Desktop).
int ViewportToIndex(CapabilityMask::Viewport vp);

// Inverse of the above.
CapabilityMask::Viewport IndexToViewport(int index);

// Return pointer to the viewport-specific profile for the given index.
ViewportProfile* GetViewportProfile(OptimizationProfile& profile, int index);

// The agent-render store-time binding check, factored out
// so it is unit-testable without driving a live Chrome render.  Returns true
// iff the markdown produced by a render bound to `item.origin_html_hash` may
// still be stored — i.e. the LIVE kContentHash sentinel for
// (item.url,hostname,scheme) still equals that captured hash (the origin has
// not advanced underneath the render — the v1-render-overwrites-v2 race).
// FAIL-CLOSED: !item.has_origin_html_hash, an absent/short live sentinel, or a
// byte mismatch all return false (discard the variant — never bind rendered
// content to a stale/wrong origin hash).
bool AgentBindingStillLive(PageSpeedCache& cache,
                           const AnalysisQueue::Item& item);

// Detect the FCP-coverage over-report failure mode.  The critical-CSS profile
// and the agent markdown variant come from the SAME Chrome analysis pass; when
// that pass degrades (e.g. under load) Chrome's FCP CSS-coverage delta
// over-reports, so the extractor's coverage_ratio (critical/total CSS bytes)
// balloons toward 1.0 and the stored profile's "critical" CSS is really the
// whole stylesheet — inlining it bloats the body (a real perf regression) and
// the same degraded pass typically yields no usable agent markdown.  Returns
// true iff a populated viewport reports a coverage_ratio at/above
// kDegradedCoverageThreshold; empty viewports are ignored (a 0-byte viewport is
// the all-empty re-analysis case, not the over-report case).
bool LooksLikeDegradedProfile(const OptimizationProfile& profile);

// Coverage ratio at/above which a populated viewport's critical CSS is treated
// as the degraded full-stylesheet over-report.  Conservative on purpose: a
// genuinely critical-heavy page rarely needs ~all of its CSS above the fold,
// and a false negative just stores a slightly-fat profile (today's behavior).
inline constexpr float kDegradedCoverageThreshold = 0.95f;

// Issue A: inline/async-defer budget for browser-profile critical CSS.
//
// The kDegradedCoverageThreshold storage guard above only fires at >=0.95 and
// only gates whether a profile is STORED — it never fires for the common
// Tailwind FCP coverage (~0.83-0.85) and does not gate USE.  Without a use-time
// budget the worker inlines ~85% of a 110 KB immutable sheet AND still async
// re-downloads the full sheet (the same CSS shipped twice, with the
// never-cacheable inline block re-sent on every HTML response — a real
// repeat-visit regression).  ShouldInlineCriticalCss is the use-time budget: it
// returns false (suppress inline+async-defer, keep the original render-blocking
// external <link> — correct on a small immutable sheet) when the critical CSS
// covers too much of the stylesheet, and true only for a genuine small critical
// subset (the win we keep).
//
// PRODUCTION INVARIANT: the extractor reports coverage_ratio as exactly
// critical_css_bytes / total_css_bytes (see BrowserCssExtractor), so coverage IS
// the byte ratio.  An earlier draft OR-ed a second `critical_bytes >
// 0.5 * total_css_bytes` gate, but because coverage == critical/total that gate
// is mathematically redundant and silently lowered the EFFECTIVE floor to 0.50
// rather than the intended 0.60.  The budget is therefore a single coverage gate
// — coverage_ratio >= kInlineCriticalCssMaxCoverage suppresses — which makes the
// production floor genuinely 0.60.
inline constexpr float kInlineCriticalCssMaxCoverage = 0.60f;

// Hard cap on the bytes of a critical block the worker inlines while the
// stylesheet STAYS render-blocking, whichever path derived it. The ratio gate
// above keeps the block a genuine subset of the sheet; this keeps it a
// genuine SMALL one when the sheet is huge. It is paid on every HTML response
// for the template (the sheet is fetched once and cached; the HTML is not),
// so 64 KiB — about half a typical utility-class sheet, and where the inline
// bytes start to cost more transfer than the render-blocking fetch they sit
// next to — is the ceiling. Above it the original render-blocking <link> is
// kept and nothing is inlined. A VALIDATED deferral is never vetoed by this
// cap: there the inline block replaces the render-blocking fetch rather than
// duplicating it, which is the whole win, and the validator rendered exactly
// that block (worker.cc applies the cap only when async_css_validated is
// false).
inline constexpr size_t kInlineCriticalCssMaxBytes = 64 * 1024;

// True when a heuristically derived block may be inlined: once the sheet is
// big enough for the ratio to mean anything, under the coverage gate measured
// on the bytes actually derived (critical / combined, the same quantity the
// profile path's coverage is). `small_sheet_bytes` is the sheet size below
// which the ratio is not applied: a page whose whole CSS is a 2 KB <style>
// block has always had that block inlined in full, and inlining all of a
// small sheet costs nothing that deferring it would save (it is the same
// threshold the deferral gate uses for its small-sheet escape,
// WorkerConfig::async_css_min_deferred_bytes). The byte cap above is applied
// separately, by the caller, once it knows whether the deferral is validated.
inline bool ShouldInlineHeuristicCriticalCss(size_t critical_css_bytes,
                                             size_t combined_css_bytes,
                                             size_t small_sheet_bytes) {
  if (combined_css_bytes < small_sheet_bytes) return true;
  return static_cast<float>(critical_css_bytes) /
             static_cast<float>(combined_css_bytes) <
         kInlineCriticalCssMaxCoverage;
}

bool ShouldInlineCriticalCss(float coverage_ratio);

// Build the re-analysis retry item from the current item.  Centralized + unit-
// tested because the retry re-enqueues directly (bypassing the worker's
// EnqueueAnalysis), so every field the downstream needs MUST be carried forward
// here — notably `scheme` (cache key) and the content binding
// (has_origin_html_hash/origin_html_hash), whose omission silently amputates the
// agent markdown for 0%-critical-CSS pages.  `frequency`/`is_warmup`/
// `original_html` are intentionally reset (a retry is not a warmup).
// Whether the agent render may follow this item's analysis.  The render
// fetches from the item's hostname, so it needs a notification that vouched
// for that host, besides agent_optimize and a running Chrome.
bool MayRunAgentRender(const AnalysisQueue::Item& item, bool agent_optimize,
                       bool chrome_running);

// Whether the agent render may use the page itself: the gate admits the
// item's own URL for a shared copy (an anonymous answer that may be shared,
// from an origin the shared-copy address rule allows).  `may_fetch` false:
// memo only.  False without a gate.
bool AgentPageAdmitted(SubresourceGate* gate, const AnalysisQueue::Item& item,
                       bool may_fetch,
                       const std::vector<PinnedUpstream>& private_origins);

AnalysisQueue::Item BuildReanalysisItem(const AnalysisQueue::Item& src,
                                        int next_retry_count);

// Fold gate for OnScriptAnalysisDone: only an evidence-backed defer/async
// verdict for a real absolute URL may enter defer_safe_scripts.
// kCandidateForAsync deliberately folds into the same defer list as
// kSafeToDefer (known async-vs-defer conflation; kept — verdicts are
// evidence-gated, so the conflation is strictly safer than classifying from
// the coverage default).  The
// `://` check is belt-and-braces: the production suffix-matcher must never
// see a non-URL key (an inline artifact or scriptId would suffix-match a
// real src ending in the same token).
bool ShouldFoldDeferRecommendation(std::string_view url, DeferralAdvice advice);

// Cold-script TTL heal: a profile analyzed while same-origin scripts were not
// yet cached carries only kNoCoverageData verdicts for them; the shortened
// TTL re-analyzes the template once the cache is warm instead of pinning
// keep-sync for the full configured TTL.  The trigger counts only HEALABLE
// misses (same-origin, mappable, cache-cold) — over-cap, cached-but-empty,
// and cross-origin scripts can never heal (a re-analysis re-reads the same
// bytes) and must not shorten the TTL.  Accepted residual: a
// same-origin script that is never fetched through the proxy (so never lands
// in cache) keeps its template on this shortened TTL indefinitely, not just
// during warmup — bounded at one re-analysis per template per hour.
inline constexpr int64_t kColdScriptProfileTtlSeconds = 3600;

// expires_at for a new profile: created_at + configured TTL, clamped to
// kColdScriptProfileTtlSeconds when same-origin scripts were uncached.
int64_t ProfileExpiryFor(int64_t created_at, int64_t configured_ttl_seconds,
                         bool same_origin_scripts_uncached);

// ---------------------------------------------------------------------------
// Measured above-the-fold set
// ---------------------------------------------------------------------------

// Cap on the distinct selector tokens ONE viewport's measured fold contributes
// to a profile. A profile carries three viewports and is cached and re-read on
// every request for the template, so this is bounded rather than proportional
// to page size. Truncation loses fold evidence, which makes the extractor
// include LESS — back toward the pre-measurement heuristic, never past it.
inline constexpr size_t kMaxAboveFoldSelectorTokens = 1024;

// Longest token one measured element may contribute. The descriptor list is
// bounded by COUNT, not by BYTES: a page is free to carry 2000 elements whose
// ids are each a megabyte, and the resulting profile is written to cache and
// re-read on every request for the template — and it would be pushed back
// through CDP, where an oversized message trips the client's own size cap. No
// real class or id is anywhere near this, so a token over it is evidence of a
// pathological or hostile page rather than of a fold worth describing.
inline constexpr size_t kMaxAboveFoldSelectorTokenBytes = 128;

// Fill one viewport profile from one viewport's page-analysis render.
//
// Pure, and factored out of the manager for one reason: this is where the fold
// stops being a measurement and becomes the thing the serve path inlines. What
// a render contributes — and, more importantly, what it does NOT contribute —
// has to be assertable without driving a browser.
//
// `result` is CONSUMED (its strings are moved out).
void PopulateViewportFromPageAnalysis(PageAnalysisResult& result,
                                      ViewportProfile& vp);

// The documents the analysis renders load, from the page as the origin
// served it: cached stylesheets inlined (InlineCachedStylesheets) and an
// absolute <base> when the author has none (InjectBaseHrefIfAbsent).
struct AnalysisDocuments {
  // For the renders that run scripts (page analysis, script coverage, the
  // agent render).
  std::string document;
  // For the CSS coverage render, which runs with script execution disabled:
  // `document` without its <noscript> elements (StripNoscriptElements),
  // so it renders what a browser running scripts renders. Empty when
  // that removal could not be trusted (it was unreliable, or the scanner read
  // the page differently, NoscriptStripAgreesWithScan); `coverage_refusal`
  // then says why, and the coverage render must not run.
  std::string coverage_document;
  std::string coverage_refusal;
};
AnalysisDocuments BuildAnalysisDocuments(std::string_view origin_html,
                                         std::string_view url,
                                         std::string_view hostname,
                                         std::string_view scheme,
                                         const css::CssLookupFn& lookup,
                                         CssInliningStats* stats = nullptr);

// ---------------------------------------------------------------------------
// Empirical critical-CSS validation (issue #1056)
// ---------------------------------------------------------------------------

// The stylesheet bytes a page is optimized and judged against, and the flag
// saying a declared sheet was not gathered.
struct CombinedCssBytes {
  std::string css;
  // ANY declared sheet was not gathered, cross-origin included.
  bool external_css_missing = false;
  // The missing sheet was one that could plausibly enter this cache, so its
  // absence self-heals on a re-notify. Distinguishes "not cached YET" from a
  // cross-origin sheet that will never cache here — the operator's next move is
  // "wait" in one case and "this page will never be confirmed" in the other.
  bool revalidatable_css_missing = false;
  // The page's cascade-layer order, as the serve path receives it: the
  // validator places a layered candidate with it exactly as the serve path
  // places the block it inlines.
  CascadeLayerOrder layer_order{};
};

// Injected, never reimplemented.  The byte sequence is a contract — it is the
// input to CombinedCssValidationHash, which binds a validation record to the
// stylesheet it was made against — and a second assembly of it would silently
// invalidate every record this manager produces.  The worker binds this to
// Worker::BuildCombinedCss, whose seven-point contract is written out beside
// that hash function in src/browser/optimization_profile.h.
//
// `site`, `host_vouched` and `from_module` are the analysed item's (wire v6),
// so the sheet is read from the page's own keyspace.
using CombinedCssBuilder = std::function<CombinedCssBytes(
    const HtmlScanResult& scan_result, const std::string& url,
    const std::string& hostname, const std::string& scheme,
    const std::string& site, bool host_vouched, bool from_module)>;

// Why an analysis item cannot produce validation records at all.
enum class ValidationInputSkip : std::uint8_t {
  kNone,
  // No worker behind this manager wired the sheet builder.
  kNoSheetBuilder,
  kNoPageMarkup,
  kUnscannable,
  // A declared sheet that CAN cache here was not in cache when the page was
  // analyzed. The bytes the serve path will hash are not the bytes we would
  // render against, so a record made now could never match. Self-heals: the
  // re-analysis TTL retries once the sheet lands.
  kStylesheetNotCachedYet,
  // A declared sheet that can NEVER enter this cache (cross-origin, e.g. a
  // font service) was not gathered. Nothing retries this: the page is
  // permanently unconfirmable and permanently render-blocking, which is a
  // different thing for an operator to read.
  kStylesheetNeverCacheable,
  kEmptyStylesheet,
};

// Everything a viewport's validation needs, assembled once per analysis item.
struct ValidationInputs {
  bool ready = false;
  ValidationInputSkip skip = ValidationInputSkip::kNone;
  // Elements the ORIGIN document declares — not the coverage render's copy,
  // which has the stylesheet inlined into it.
  std::vector<CollectedElement> elements;
  std::string combined_css;
  CascadeLayerOrder layer_order{};
  // HtmlScanResult::noscript_affects_render of the same scan; salts the
  // record's binding (ValidationBindingFor).
  bool noscript_affects_render = false;
};

// Scan the page as the origin served it and take the combined stylesheet from
// the injected builder.  Pure apart from the builder call, so the wiring that
// decides WHICH markup and WHICH stylesheet bytes a record is made against can
// be asserted without a browser.
ValidationInputs BuildValidationInputs(
    const CombinedCssBuilder& builder, const std::string& url,
    const std::string& hostname, const std::string& scheme,
    std::string_view pre_inline_html, const std::string& site = {},
    bool host_vouched = false, bool from_module = false);

// Human-readable reason an item produces no records, for the analysis log.
const char* ValidationInputSkipMessage(ValidationInputSkip skip);

// Why a viewport's critical block was not put in front of a browser.  Every
// value here means "no record", which the serve path reads as "keep the
// stylesheet render-blocking".
enum class ValidationSkip : std::uint8_t {
  kNone,
  kChromeUnavailable,
  // The extractor produced nothing to validate.
  kNoCriticalBlock,
  // Nothing was gathered, or a declared sheet was not in cache: the bytes the
  // serve path will hash are not the bytes we would render against, so a record
  // made now could never match anyway.
  kNoCombinedStylesheet,
  // The inline budget already refuses this profile, so there is no deferral to
  // authorize.  Two Chrome renders saved per viewport.
  kSuppressedByInlineBudget,
  // This viewport's fold has not been measured yet, so the block that would be
  // derived here is NOT the block the serve path will inline. Reaching this
  // means the pipeline ran validation before page analysis — see
  // AnalysisContext::fold_measured. Refusing costs a deferral; validating would
  // buy a confirmation of bytes no visitor receives.
  kFoldNotMeasured,
};

struct ValidationPlan {
  bool run = false;
  ValidationSkip skip = ValidationSkip::kNone;
};

// Decide whether a viewport is worth two extra renders.  Pure: the whole
// decision, so it can be asserted without a browser.
ValidationPlan PlanCriticalCssValidation(
    const ViewportProfile& vp, std::string_view combined_css,
    std::string_view candidate_critical_css, bool chrome_available);

// Human-readable reason for a skip, for the analysis log.
const char* ValidationSkipMessage(ValidationSkip skip);

// Everything decided about one viewport's validation before a browser is
// involved: WHICH block is judged, and WHICH two documents are rendered.
//
// Pure, and separated from the manager for one reason: this is where a false
// confirmation would be manufactured. Validate the wrong block — the browser's
// raw coverage blob instead of the block the serve path re-derives and inlines
// — and every record is about bytes no visitor receives, while every test that
// only checks "a record was written" stays green.
struct ValidationRequest {
  bool run = false;
  ValidationSkip skip = ValidationSkip::kNone;
  // The block that will be rendered, and that a resulting record describes.
  // Empty when !run.
  std::string candidate_critical_css;
  // CriticalCssResult::anonymous_layers_dropped of the derivation that made
  // the candidate; salts the record's binding (ValidationBindingFor).
  // False when !run.
  bool anonymous_layers_dropped = false;
  ValidationDocuments docs;
};

// `layer_order` is the page's cascade-layer order from the same assembly as
// `combined_css` (CombinedCssBytes::layer_order); it reaches
// BuildValidationDocuments so the validated placement is the served one.
ValidationRequest PrepareCriticalCssValidation(
    const ViewportProfile& vp, const std::vector<CollectedElement>& elements,
    std::string_view combined_css, std::string_view pre_inline_html,
    CapabilityMask::Viewport viewport, bool renderer_available,
    const CascadeLayerOrder& layer_order = {});

// Why `request` will not run, for the analysis log.
std::string ValidationRefusalMessage(const ValidationRequest& request);

// Record a verdict on `vp`.
//
// FAIL-CLOSED, and it clears before it writes: a positive record is only ever
// left behind when a real comparison was made, against a critical block that
// exists, against a stylesheet that exists.  "Validated against nothing" hashes
// to nothing the serve path can match, but leaving the bit set with an empty
// hash would still be a record that reads as a mistake rather than a refusal.
//
// `layer_order_binding` (CascadeLayerOrder::ValidationBinding of the page's
// order from the same assembly) is folded into the stylesheet hash, so the
// record also binds to the layer order the validation placed the block with.
void ApplyValidationVerdict(ViewportProfile& vp,
                            const ValidationVerdict& verdict,
                            std::string_view candidate_critical_css,
                            std::string_view combined_css,
                            std::string_view layer_order_binding = {});

}  // namespace browser_internal
}  // namespace pagespeed

#endif  // PAGESPEED_SRC_WORKER_BROWSER_ANALYSIS_MANAGER_INTERNAL_H_
