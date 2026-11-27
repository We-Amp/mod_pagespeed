// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

// 2. Fetch.enable + Fetch.failRequest for all requests — intercept layer
// 3. Emulation.setScriptExecutionDisabled({value:true}) — no JS
// 4. --host-resolver-rules="MAP * ~NOTFOUND" on Chrome — blocks DNS
// Content is loaded via Page.setDocumentContent with CSS inlined.

#ifndef PAGESPEED_SRC_BROWSER_BROWSER_CSS_EXTRACTOR_H_
#define PAGESPEED_SRC_BROWSER_BROWSER_CSS_EXTRACTOR_H_

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "absl/container/flat_hash_set.h"
#include "absl/status/statusor.h"
#include "src/worker/cascade_layer_order.h"
#include "src/worker/critical_css_extractor.h"  // for RuleIdentity

namespace pagespeed {

class CdpClient;

// Parses a critical-CSS blob into the set of rule identities it contains
// (selector identities only). The DOM-matched critical-CSS producer consumes
// this as a `force_include` augmentation so rules a static, JS-off, light-mode
// sample cannot match — but that Chrome DID exercise at first paint — are still
// retained.
//
// The input is raw byte slices reconstructed from Chrome CSS coverage ranges,
// so it may be brace-unbalanced and structurally partial. This parser is
// deliberately defensive: it never crashes, never hangs, and skips CSS strings
// and comments so braces in `content:"{"` or comments do not corrupt nesting.
// It tracks enclosing @layer/@media context best-effort — and usually there is
// none to track, because a coverage range covers the rule and never the
// `@layer ... {` / `@media ... {` prelude enclosing it, so most identities come
// out context-free. That is not inert: the producer relaxes both context fields
// on a primary-key miss, matching a context-free identity against the single
// (layer, media) context the sheet places that selector in, and against nothing
// at all when the sheet is ambiguous about it. This remains intentionally
// lossy; the DOM-matched producer is the primary fix and does not depend on
// this succeeding.
absl::flat_hash_set<RuleIdentity> BuildCoverageIdentities(
    std::string_view critical_css_text);

// The production derivation of the above-the-fold block a page is served: match
// the full combined stylesheet against THIS page's DOM, using the browser
// profile's observed coverage as the force-include augmentation for the
// matcher's blind spots.
//
// ONE implementation, deliberately. The serve path calls it to produce the
// block it inlines, and the validation that authorizes deferring calls it to
// produce the block it renders. A second derivation would mean the record says
// "this block covers the fold" about a block the visitor never receives — a
// validation that is worse than none, because the gate above it stops asking.
//
// `profile_critical_css` is the raw browser-coverage blob; an empty one simply
// means no force-include augmentation.
//
// `measured_above_fold_selectors` is the viewport profile's measured fold
// (ViewportProfile::above_fold_selectors). It has NO default: which fold a
// derivation was made against is exactly the thing that must not be decided by
// omission, because a caller that quietly derives against a different fold from
// the serve path produces a block about bytes no visitor receives — and when
// that caller is the deferral validation, the result is a confirmation of a
// block nobody is served. Pass `{}` to mean "no measurement", explicitly.
//
// `layer_order` is the page's cascade-layer order (Worker::BuildCombinedCss),
// the one the block's placement is decided with. It decides which @media
// blocks are kept (the device class's windows for a block that
// goes before the sheets, every width for one that goes after), so it has no
// default either: the serve path and the validation must pass the same order
// or they derive different blocks.
CriticalCssResult DeriveDomMatchedCriticalCss(
    const std::vector<CollectedElement>& elements,
    std::string_view combined_css, std::string_view profile_critical_css,
    CapabilityMask::Viewport viewport,
    const std::vector<std::string>& measured_above_fold_selectors,
    const CascadeLayerOrder& layer_order);

// Result of browser-based critical CSS extraction.
struct BrowserCssResult {
  std::string critical_css;  // CSS rules active at FCP
  std::string deferred_css;  // CSS rules active after load (below fold)
  std::string unused_css;    // CSS rules never matched
  size_t total_css_bytes = 0;
  size_t critical_css_bytes = 0;
  float coverage_ratio = 0.0f;  // critical / total
};

// Extracts critical CSS by rendering HTML in a headless Chrome tab
// and using the CSS Coverage API to identify which rules are active
// at First Contentful Paint.
//
// Usage:
//   BrowserCssExtractor extractor(cdp_client);
//   extractor.Extract(html, viewport_width, viewport_height,
//       [](absl::StatusOr<BrowserCssResult> result) {
//         if (result.ok()) { /* use result->critical_css */ }
//       });
//
// Each Extract() call creates a new browser tab, runs the analysis,
// and closes the tab. Only one extraction may run at a time per
// CdpClient (the event callback is single-subscriber).
class BrowserCssExtractor {
 public:
  using Callback = std::function<void(absl::StatusOr<BrowserCssResult>)>;

  explicit BrowserCssExtractor(CdpClient* client);
  ~BrowserCssExtractor() = default;

  BrowserCssExtractor(const BrowserCssExtractor&) = delete;
  BrowserCssExtractor& operator=(const BrowserCssExtractor&) = delete;

  // Default overall session timeout (60 seconds).
  static constexpr uint32_t kDefaultTimeoutMs = 60000;

  // Extract critical CSS for HTML content at a specific viewport.
  // html_content: full HTML with CSS inlined as <style> blocks.
  // The callback is invoked on the main event loop when extraction
  // completes or fails. timeout_ms is an overall session timeout
  // (0 = no timeout).
  void Extract(std::string_view html_content, uint32_t viewport_width,
               uint32_t viewport_height, Callback callback,
               uint32_t timeout_ms = kDefaultTimeoutMs);

 private:
  // Internal state machine for one extraction session.
  struct Session;

  CdpClient* client_;
};

}  // namespace pagespeed

#endif  // PAGESPEED_SRC_BROWSER_BROWSER_CSS_EXTRACTOR_H_
