// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

// from the template hash, using SentinelId::kBrowserProfile (0x5C).

#ifndef PAGESPEED_SRC_BROWSER_OPTIMIZATION_PROFILE_H_
#define PAGESPEED_SRC_BROWSER_OPTIMIZATION_PROFILE_H_

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "absl/status/statusor.h"
#include "lib/classify/capability_mask.h"

namespace pagespeed {

// Rendered image dimensions detected by browser analysis.
struct ImageDimension {
  std::string selector;  // CSS selector for the image element
  uint32_t rendered_width = 0;
  uint32_t rendered_height = 0;
  uint32_t natural_width = 0;
  uint32_t natural_height = 0;
};

// Resource preload hint from browser analysis.
struct PreloadHint {
  std::string url;
  std::string as;    // "image", "style", "script", "font"
  std::string type;  // MIME type (optional)
};

// Per-viewport optimization data from browser analysis.
struct ViewportProfile {
  std::string critical_css;
  std::string lcp_selector;
  std::string lcp_url;
  // Selector tokens for the elements a real browser measured above the fold in
  // THIS viewport, deduped, in document order. Two token shapes are meaningful
  // to the critical-CSS extractor: "#id" and ".class", carrying the RAW DOM
  // attribute value (not an escaped CSS identifier — `.dark:bg-stone-900`, not
  // `.dark\:bg-stone-900`). Anything else here — notably the tag-prefixed image
  // selectors this list has always carried, e.g. "img#hero" — is inert to the
  // extractor by construction.
  std::vector<std::string> above_fold_selectors;
  std::vector<std::string> below_fold_selectors;
  std::vector<ImageDimension> image_dimensions;
  float css_coverage_ratio = 0.0f;  // fraction of CSS bytes used
  size_t total_css_bytes = 0;
  size_t unused_css_bytes = 0;

  // Empirical sufficiency record for stylesheet deferral (async-CSS).
  //
  // The byte-ratio floor is a proxy: it can pass while the inlined block does
  // not in fact cover the fold, which is a flash of unstyled content. Deferral
  // therefore requires a POSITIVE record here, and the absence of one means
  // "keep the stylesheet render-blocking" — never "assume it is fine".
  bool critical_css_validated = false;
  // Differing-pixel ratio the validation measured; <0 means "never measured".
  float validation_diff_ratio = -1.0f;
  // Hex SHA-256 of the critical block the validation rendered. FORENSIC ONLY —
  // written here, never read by any gate. It answers "which bytes was this
  // record actually about?" when a page misbehaves; the block is re-derived per
  // page, so comparing against it at serve time would invalidate nearly
  // everything. The gate reads validated_combined_css_hash below.
  std::string validated_critical_css_hash;
  // Hex SHA-256 of the COMBINED stylesheet the validation was performed
  // against. The validated bit may only be honoured while the page still
  // serves that exact stylesheet: the template hash covers the tag tree and is
  // blind to CSS, so a CSS-only redeploy (new utility-framework build,
  // unchanged markup) would otherwise keep a stale validation alive for the
  // rest of the profile's TTL. Hashing the combined sheet rather than the
  // critical block is deliberate — per-page class variation changes the
  // critical block on nearly every page and would suppress everything.
  std::string validated_combined_css_hash;
};

// Content hash used to bind a validation record to the stylesheet it was made
// against. Hex-encoded SHA-256, the primitive the cache already
// content-addresses with (SentinelId::kContentHash).
//
// Empty input hashes to the empty string, which no stored hash can equal:
// "validated against nothing" must never satisfy the gate.
//
// ---------------------------------------------------------------------------
// INPUT CONTRACT — read before hashing anything
// ---------------------------------------------------------------------------
// The argument MUST be the bytes produced by `Worker::BuildCombinedCss`
// (src/worker/worker.h), and a producer of a validation record MUST CALL that
// function rather than reconstruct the bytes. A record is only meaningful
// against the exact sequence the serve path will hash, and the assembly has
// enough steps that an independent reimplementation will drift:
//
//   1. Seed: `scan_result.inline_css` verbatim — the concatenated <style>
//      bodies, exactly as HtmlScanner produced them. Since the
//      stylesheet-source rule that
//      leaves out <style> inside <noscript>, <template>, <noembed>, <noframes>
//      and <math> (and step 3 the links there), so a record made before it
//      for a page with such CSS no longer matches and must be re-validated.
//   2. @import flattening of that seed (relative URLs against the document's
//      base URL, i.e. the page URL or its <base href>, since the block-first
//      placement) is
//      ALL-OR-NOTHING: the flattened result
//      replaces the seed only when `imports_resolved > 0`; any unresolved
//      import leaves the ORIGINAL bytes. Disabled entirely under
//      `disable_css_import_flattening`.
//   3. Then each declared external <link rel=stylesheet>, in the order
//      HtmlScanner recorded them (document order), its href resolved against
//      the document's base URL (the page URL or its <base href>; under a
//      cross-host <base> a root-relative href is the base host's sheet)
//      and read from cache at the normalized URL, keyed by that
//      host. Hrefless sheets are skipped; a sheet that resolves with an EMPTY
//      body contributes nothing and does not set the missing flag.
//   4. Join: a single "\n" before each appended sheet, and only when the
//      accumulator is already non-empty. No trailing newline, no separator
//      before the first contribution.
//   5. Per-sheet @import flattening is all-or-nothing on the same rule.
//   6. Cap: 10 MiB. Exceeding it BREAKS out of the loop — later sheets are
//      silently absent from the bytes and `external_css_missing` is set.
//   7. No normalization of any kind: no minification, no whitespace collapsing,
//      no ordering fix-up. Bytes in, bytes hashed.
//   8. Media scoping (css::CombinedSheetMediaScope): a <style> or <link> whose
//      media is not empty, `all` or `screen` contributes its body or sheet
//      inside `@media <media>{...}`, so a print sheet stays print-only; a
//      print <link> with an onload handler (the print-media trick) counts as
//      unscoped; a media value that cannot be written as an @media prelude
//      contributes nothing and does not set the missing flag. Records made
//      before scoping, for a page with such media, no longer match and are
//      validated again once.
//
// KNOWN INSTABILITIES — accepted, and design inputs for the validator that
// will produce these records:
//   * A sheet whose cache entry flips between original and minified variants
//     changes these bytes without the stylesheet having been republished. The
//     record invalidates and the page reverts to render-blocking until it is
//     re-validated. Correct direction (fail toward render-blocking), but it
//     means "hash changed" does NOT imply "the site published new CSS".
//   * A transient cache miss on one sheet is indistinguishable here from a
//     redeploy: both simply produce different bytes. The validator must not
//     treat a single mismatch as evidence the site changed.
std::string CombinedCssValidationHash(std::string_view css);

// As above, bound to the page's cascade-layer order as well:
// `layer_order_binding` is CascadeLayerOrder::ValidationBinding(css)
// (src/worker/cascade_layer_order.h), which says whether the order was proven
// and, if so, the `@layer` statement put in front of a layered critical block;
// it is empty for a sheet that uses no cascade layer, so those records still
// hash the sheet alone.
// That decides where the block goes and what both validation documents carry,
// yet it is NOT in the combined sheet's bytes: a layer() / supports() @import
// is never flattened into them, but the order reads its sheet from cache. An
// import cached at analysis time and not at serve time (or the reverse, or
// changed in between) flips the placement while the sheet hash still matches,
// so the record binds to both. An empty binding hashes the sheet alone, as the
// one-argument form does. Empty `css` still hashes to "".
std::string CombinedCssValidationHash(std::string_view css,
                                      std::string_view layer_order_binding);

// The binding a validation record is made and checked under:
// `layer_order_binding` as above, salted when the page's <noscript> content
// changes what a render with script execution disabled shows
// (HtmlScanResult::noscript_affects_render). The analysis renders render such
// a page without that content now, so a record made while they rendered it is
// about another fold: the salt makes it mismatch, and the page is validated
// again once (BrowserAnalysisManager::RequestRevalidation). Pages without
// such content, the GTM snippet's hidden iframe or a tracking pixel included,
// keep their records. Both sides pass the result as `layer_order_binding`.
//
// Also salted when `combined_css` escapes a quote, a brace or a
// comma outside a string (CombinedCssHasStructuralEscape): until that was fixed
// the extractor misread such a sheet (an escaped quote opened a string that
// never closed, an escaped brace nested, an escaped comma split a selector),
// so the blocks validated for it lost most of their fold rules and its
// records are mostly negative. The salt makes them mismatch once, so the page
// is validated again with blocks derived from the whole sheet instead of
// staying render-blocking until the profile expires. Other sheets keep their
// records.
//
// And salted for the tablet viewport's records (`viewport` is
// kTablet): before that the tablet viewport was analysed and validated
// as a desktop window, so its blocks and verdicts are about a layout tablet
// visitors do not get. Every tablet record mismatches once and the analysed
// page is validated again, through the same rate-limited path. Mobile and
// desktop records keep theirs.
//
// And salted for the mobile and tablet viewports' records
// ("touch"): before touch emulation their renders emulated no touch screen, so
// `(hover: hover)` / `(pointer: fine)` rules applied in the coverage and
// validation renders and `(hover: none)` / `(pointer: coarse)` rules did not,
// the reverse of a phone visitor's browser, and the extractor now reads those
// features for the two classes (RetentionPointer). Every mobile and tablet
// record mismatches once; desktop records keep theirs.
//
// And salted for the mobile and tablet viewports' records
// ("ua"): before User-Agent emulation their renders reported the browser's
// desktop user agent, and the renders that write the document with
// Page.setDocumentContent wrote it into a window that predates the
// emulation (`'ontouchstart' in window` false). Since then they report a
// phone's or a tablet's UA (EmulateDevice) and write the document into a
// fresh window (LoadFreshBlankDocument). Every mobile and tablet record
// mismatches once, so the page is validated again by renders that are the
// device in those respects as well; desktop records keep theirs (the desktop
// window is sent no UA override).
//
// Also salted when the derivation of the block being served or
// validated left every anonymous-layer rule out of it
// (`anonymous_layers_dropped`, CriticalCssResult::anonymous_layers_dropped of
// the same DeriveDomMatchedCriticalCss / Extract call on both sides). The
// layer binding's `u2` marker says the same from the combined sheet alone;
// this salt comes from the derivation itself, so it also covers a block that
// only the extractor's check on the finished block moved after the sheets,
// which the sheet-keyed marker cannot see. Both sides pass the flag of their
// own derivation, made the same way from the same sheet and order. That
// finished-block check is the flag's one element-dependent leg: on a page
// whose markup differs between the analysis render and a later serve, with a
// proven order, an anonymous layer and a trailing `@import ... layer()`, the
// flag can differ between the two and the record mismatch, which takes the
// rate-limited revalidation path like any other mismatch.
std::string ValidationBindingFor(std::string_view layer_order_binding,
                                 bool noscript_affects_render,
                                 std::string_view combined_css,
                                 CapabilityMask::Viewport viewport,
                                 bool anonymous_layers_dropped);

// True when `css` has a backslash escaping `'`, `"`, `{`, `}` or `,` outside
// strings and comments (Tailwind v4 writes `'` in a class name as `\'`, as in
// shadcn/ui's `[&_svg:not([class*='size-'])]:size-4`). See ValidationBindingFor.
bool CombinedCssHasStructuralEscape(std::string_view css);

// The serve path's accept test for a validation record (issue #1056).
//
// The byte/coverage floor is a proxy for "the inlined block covers the fold",
// and a proxy can pass while the fold is left unstyled. Deferral therefore
// additionally requires a POSITIVE record on the viewport profile this page's
// critical block was derived from, still bound to the exact stylesheet being
// served:
//
//   - No profile (the heuristic path) -> no record and none obtainable, so
//     never defer. On a small sheet the byte gate's escape hatch would
//     otherwise say "sufficient" without measuring anything.
//   - Stylesheet-only redeploy -> the template hash covers the tag tree and is
//     blind to CSS, so the stale record survives the change that invalidates
//     it. The hash comparison is what notices.
//   - Nothing gathered (empty combined sheet) -> hashes to nothing matchable,
//     so "validated against no stylesheet" cannot satisfy the gate either.
//
// It lives here, beside the hash it consults, so the code that PRODUCES a
// record can be tested against the exact predicate that CONSUMES it rather
// than against a second copy that can drift into agreeing with itself.
//
// `layer_order_binding` is the served page's layer-order binding, compared the
// way the record was made (the two-argument CombinedCssValidationHash).
bool AsyncCssValidatedForServedSheet(const ViewportProfile* vp,
                                     std::string_view combined_css,
                                     std::string_view layer_order_binding = {});

// Which record, if any, the accept test above may be applied to (issue #1216).
//
// A record is only ever about ONE block: the output of
// `DeriveDomMatchedCriticalCss` for this profile, which is what the validation
// rendered and stamped. When that derivation comes back EMPTY for the page
// being served, whatever is inlined in its place — the heuristic extractor's
// substitute, or no block at all — is not what the record describes, while the
// record itself still reads as perfectly healthy: the validated bit is set and
// the stylesheet hash still matches, because neither of them says anything
// about WHICH block is being served. Handing it to the accept test then
// authorizes deferral on a confirmation made about different bytes, and the
// byte floor cannot catch that either — below `async_css_min_deferred_bytes`
// the small-sheet escape hatch clears whatever the coverage is.
//
// So: `vp` when the derivation produced the block, nullptr otherwise. Feeding
// the result straight into `AsyncCssValidatedForServedSheet` keeps the
// stylesheet render-blocking, and nothing is inlined next to it: a block is
// inlined only together with the deferral it makes possible.
const ViewportProfile* AsyncCssRecordForDerivedBlock(
    const ViewportProfile* vp, std::string_view derived_critical_css);

// Complete optimization profile for a template.
// Stored in cache at synthetic key:
//   URL:      __pagespeed_profile__/{hex(template_hash)}
//   Hostname: __internal__
//   Sentinel: kBrowserProfile (0x5C)
struct OptimizationProfile {
  std::string template_hash_hex;
  std::string analyzed_url;

  // Per-viewport profiles (standard breakpoints).
  ViewportProfile mobile;   // 375x667
  ViewportProfile tablet;   // 768x1024
  ViewportProfile desktop;  // 1440x900

  // Cross-viewport optimization data.
  std::vector<PreloadHint> preload_hints;
  std::vector<std::string> defer_safe_scripts;

  // Metadata.
  int64_t created_at = 0;  // Unix timestamp
  int64_t expires_at = 0;  // TTL for re-analysis

  // Serialize to JSON string.
  std::string ToJson() const;

  // Deserialize from JSON string.
  static absl::StatusOr<OptimizationProfile> FromJson(std::string_view json);

  // Build the synthetic cache URL for a template hash.
  static std::string CacheUrl(uint64_t template_hash);

  // Fixed hostname used for profile cache entries.
  static constexpr std::string_view kProfileHostname = "__internal__";
};

}  // namespace pagespeed

#endif  // PAGESPEED_SRC_BROWSER_OPTIMIZATION_PROFILE_H_
