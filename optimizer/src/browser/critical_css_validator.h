// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

// PageSpeed 2.0 — Critical-CSS validator
//
// Answers, empirically, the one question the async-CSS byte-ratio floor can
// only guess at: does the page's fold look the same when ONLY the inlined
// above-the-fold block is applied, as it does with the whole stylesheet?
//
// It builds two documents from the same page markup — one carrying the full
// combined stylesheet, one carrying the candidate critical block, and NOTHING
// else different — renders both, and diffs the fold. A positive answer is what
// the serve path's deferral gate requires (ViewportProfile::
// critical_css_validated); the absence of one means "keep the stylesheet
// render-blocking".
//
// FAIL-CLOSED IS THE WHOLE POINT. Every path that cannot produce a measurement
// — no browser, a refused injection, a screenshot that never arrived, a
// document too large to render, a comparison that compared no pixels — is "not
// validated", never "probably fine". A validator that guesses is worse than no
// validator, because the gate above it stops asking.
//
// THREE PERMANENT BLIND SPOTS. The first two are properties of the render's
// SSRF defense (src/browser/visual_regression_gate.cc) and therefore not
// tunable; the third is a property of what a record is attached to:
//   1. Scripts do not run. A fold whose layout is established by JavaScript is
//      validated against a DOM the visitor never sees. (What the HTML parser
//      does differently with scripting on IS reproduced: <noscript> content
//      is left out of both documents.)
//   2. Every subresource fails. Images, web fonts and @import-ed sheets load in
//      NEITHER document, so a fold that depends on a dropped background-image
//      or @font-face renders identically blank in both and diffs to zero.
//   3. Confirmation is per TEMPLATE, not per page. The record hangs off the
//      profile, and the profile is keyed on a tag-tree hash that ignores
//      classes and ids — so page B, which shares A's template, defers on A's
//      record even though B's critical block is re-derived against B's own DOM
//      and may drop a rule B's fold needs. Deliberate: hashing per page would
//      invalidate nearly everything and the feature would never apply. The
//      exposure is real and bounded by how alike a template's pages are.
// The rendered probe lane (tools/async-css-probe) is the only instrument that
// covers the second class; nothing in-product can. Nothing covers the third —
// an operator who sees a fold problem on one page of a template should turn
// deferral off for the site (--no-async-css) rather than expect the gate to
// have caught it.
//
// WHICH PAGES REACH THIS AT ALL. Deferral is only inlined-and-deferred below
// the inline budget (browser_internal::kInlineCriticalCssMaxCoverage, 0.60), so
// a page whose critical block covers more of its sheet than that is suppressed
// before validation and never validated — by design, since there is no
// deferral to authorize. The measured fixture below sits at 0.83 and is one of
// them: its numbers exercise the mechanism, not a page production would
// confirm.
//
// CASCADE ORDER. The synthesized documents carry ONE block, placed where the
// serve path puts the inlined block (HtmlTransformFilter::InjectCriticalCss):
// where the page's first stylesheet source stood (an author's
// loadCSS preload counts, though it is kept), unless that
// source is inside <noscript> and the like, comes before a head-prelude element
// (<meta charset>, a meta CSP, <base>) or after </head>; else at the </head>
// (or </body>) fallback — and always at that fallback when the candidate block
// names a cascade layer and the page's layer order is not proven (see
// BuildValidationDocuments). On the served page the block precedes every sheet
// it duplicates, so the sheets win their ties once they apply. The
// reference block is the combined sheet in the combined order (inline <style>
// bodies first, then the external sheets — see Worker::BuildCombinedCss), which
// is not the page's document order when a <style> follows a <link>: two rules
// of equal specificity can then resolve differently here than in the served
// page. The direction is conservative — a difference this introduces shows up
// as a diff and refuses — but it is a reason a page can fail to be confirmed
// that has nothing to do with its fold.

#ifndef PAGESPEED_SRC_BROWSER_CRITICAL_CSS_VALIDATOR_H_
#define PAGESPEED_SRC_BROWSER_CRITICAL_CSS_VALIDATOR_H_

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

#include "src/worker/cascade_layer_order.h"
#include "src/worker/html_scanner.h"

namespace pagespeed {

class VisualRegressionGate;

// The two documents a validation renders. `ok == false` means no comparison
// may be attempted — `error` says why, and the caller must record "not
// validated".
struct ValidationDocuments {
  bool ok = false;
  std::string error;
  // Page markup with every stylesheet <link> and every <style> removed, plus
  // ONE injected <style> carrying the full combined sheet, standing where the
  // serve path puts the inlined block (see CASCADE ORDER above).
  std::string reference;
  // The same markup, the same injection point, carrying the candidate critical
  // block instead.
  std::string candidate;
  size_t stylesheet_links_removed = 0;
  size_t style_blocks_removed = 0;
  // <noscript> elements left out (see BuildValidationDocuments).
  size_t noscript_elements_removed = 0;
};

// Outcome of one viewport's validation.
struct ValidationVerdict {
  bool validated = false;
  // Differing-pixel ratio of the fold; <0 means "no measurement was made".
  float diff_ratio = -1.0f;
  // Why not, when !validated. Empty on success.
  std::string failure_reason;
};

// Above this, the document is not rendered at all. Page.setDocumentContent
// carries the whole document in one CDP frame under a 15 s timeout, and a page
// this size is not the shape this optimization is for. Well above the combined
// -CSS assembly's own 10 MiB cap plus a large page.
inline constexpr size_t kMaxValidationDocumentBytes = 8 * 1024 * 1024;

// Fold diff ratio at or below which the candidate is considered to render the
// same fold.
//
// Q3, OWNER DECISION PENDING. This is the value VisualRegressionGate shipped
// with, and it had never been run against a real page. Measured over 8 runs on
// the byte-frozen modpagespeed.com capture with
// tools/async-css-probe:measure_validation_threshold, against the block the
// extractor really produces and against the same block with its layout
// DECLARATIONS stripped out (the pre-fix extraction shape that caused the
// incident). Sizes matter as much as ratios: a 118,024 B combined sheet, a
// 107,602 B "good" block, a 93,400 B "gutted" one.
//
//   viewport         good      gutted
//   375x667       0.00000     0.62956     (good: 0.00040 in 3 of 8 runs)
//   768x1024      0.00000     0.48308
//   1440x900      0.00000     0.35016
//
// Both candidate thresholds separate those cleanly; the decision is about
// headroom, not about this page. The good case is not a stable zero: the
// mobile viewport measures 0.00040 (100 of 250,125 pixels) in 3 runs of 8 —
// rasterization noise, not a difference anyone can see, but noise a threshold
// has to sit above, and intermittent enough that a single run will not show
// it. That leaves 0.005 with ~12x margin over observed noise and 0.002 with
// ~5x. Nothing measured here argues for the tighter value, and the looser one
// is what a noisy render is least likely to trip. It stays at 0.005 until the
// owner picks; changing it is this line.
inline constexpr float kDefaultValidationDiffThreshold = 0.005f;

// Build the reference/candidate pair from the page's PRE-INLINE markup.
//
// `pre_inline_html` must be the page as the origin served it — NOT a copy that
// has already had its stylesheets inlined for coverage analysis, which would
// leave the sheet in the document twice and make the comparison meaningless.
//
// Strips every <link rel=stylesheet> and every <style> element, comment- and
// raw-text-aware (it reuses html_css_injector's scanner, so "where does this
// <style> end" is answered once in the codebase), then injects exactly one
// <style> per document at the same point: the serve path's placement,
// replayed on the stripped markup (see CASCADE ORDER above; the rule is
// HtmlTransformFilter::TrackCriticalCssAnchor's, mirrored on the string
// level), via InjectCriticalCssAt.
//
// Both documents also leave out every <noscript> element, contents and all
// (StripNoscriptElements), the worker's own deferred-stylesheet
// fallback copy included. The render runs with script execution disabled, in
// which Blink renders <noscript> content, while the fold being judged is the
// one a browser running scripts sees, where that content is raw text. That
// removal runs first, so no <noscript> content is ever read as markup by the
// stylesheet removal; a head-prelude element the serve path sees inside a
// <noscript> still moves the placement, where that <noscript> stood. Also
// fails closed when the removal is not reliable, or disagrees with the
// scanner's reading (`scanned_elements`), or when a removed <noscript> and a
// comment as the serve path's lexer reads it overlap without one holding the
// other (the placement replay would read comment text differently from the
// serve path): a page cut short renders blank in both
// documents, and two blank renders compare equal.
//
// Fails closed — `ok == false` — when: either CSS is empty (an empty reference
// would diff to zero against anything); the markup is empty; the resulting
// document exceeds `max_document_bytes`; the injector refuses (notably the
// `</style` abort, which reports success with an EMPTY document, so a caller
// checking only `success` would synthesize a blank reference); or the two
// documents end up differing anywhere outside the injected style body.
//
// `layer_order` is the page's cascade-layer order, as the serve path receives
// it (Worker::BuildCombinedCss). The candidate block decides the placement for
// both documents with DecideCriticalCssLayerPlacement, as the serve path does:
// a layered candidate goes first only when the order is proven, and then BOTH
// documents carry the order's `@layer` statement in front of their CSS, so the
// reference renders the full sheet in the author's layer order too. The default
// (not proven) keeps a layered candidate at the </head> fallback.
//
// `scanned_elements` is HtmlScanner's `elements` for the same
// `pre_inline_html`, the independent reading the <noscript> removal is checked
// against (NoscriptStripAgreesWithScan); null skips that check.
ValidationDocuments BuildValidationDocuments(
    std::string_view pre_inline_html, std::string_view full_css,
    std::string_view critical_css, const CascadeLayerOrder& layer_order = {},
    size_t max_document_bytes = kMaxValidationDocumentBytes,
    const std::vector<CollectedElement>* scanned_elements = nullptr);

// True iff `reference` and `candidate` are byte-identical apart from the body
// of the one injected <style data-pagespeed-critical> element. The fairness
// invariant: anything else that differs is a difference the diff would blame on
// the CSS.
bool DocumentsDifferOnlyInStyleBody(std::string_view reference,
                                    std::string_view candidate);

// Render both documents at `viewport_width` x `viewport_height` and diff the
// fold. `callback` is always invoked exactly once.
//
// `gate` is BORROWED and must be owned by something that outlives neither more
// nor less than the CDP client under it — see VisualRegressionGate's lifetime
// note. Destroying it cancels an in-flight validation, which arrives here as a
// refusal, which is the right answer: a browser that went away confirmed
// nothing.
//
// A null `gate` (Chrome not running) is not an error to report upward — it is
// simply "not validated", delivered through the same callback so the caller has
// one path.
void ValidateCriticalCss(VisualRegressionGate* gate,
                         const ValidationDocuments& docs,
                         uint32_t viewport_width, uint32_t viewport_height,
                         float threshold,
                         std::function<void(ValidationVerdict)> callback);

}  // namespace pagespeed

#endif  // PAGESPEED_SRC_BROWSER_CRITICAL_CSS_VALIDATOR_H_
