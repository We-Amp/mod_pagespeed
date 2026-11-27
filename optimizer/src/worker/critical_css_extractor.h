// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

// PageSpeed 2.0 - Critical CSS Extractor
//
// Extracts critical CSS rules that apply to above-the-fold content
// based on collected HTML elements and heuristics.

#ifndef PAGESPEED_SRC_WORKER_CRITICAL_CSS_EXTRACTOR_H_
#define PAGESPEED_SRC_WORKER_CRITICAL_CSS_EXTRACTOR_H_

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "absl/container/flat_hash_map.h"
#include "absl/container/flat_hash_set.h"
#include "lib/classify/capability_mask.h"
#include "src/worker/cascade_layer_order.h"
#include "src/worker/html_scanner.h"

namespace pagespeed {

// Media-query width evaluation, shared by the extractor's rule retention and
// position:fixed anchoring (exposed for tests).
//
// A closed range of window widths in CSS px. kMediaWidthUnbounded stands for
// "no upper bound".
inline constexpr uint32_t kMediaWidthUnbounded = 1U << 24;
struct MediaWidthRange {
  uint32_t min_px;
  uint32_t max_px;
};

// The window widths a variant of this device class can be rendered at, for
// rule retention. The classes overlap: the class comes from the User-Agent,
// and a desktop User-Agent can be any width. See the definition for how the
// bounds were chosen.
MediaWidthRange RetentionWidthRange(CapabilityMask::Viewport viewport);

// The range rule retention starts from for `css` (the combined stylesheet)
// under the page's cascade-layer order: RetentionWidthRange when a block
// derived from `css` goes before the page's sheets and the CSS has no
// anonymous layer (CriticalCssRetentionMayNarrow), and every width otherwise.
// `class_range`, when given, is set to whether the
// class range was chosen (for desktop both ranges are every width).
// CriticalCssExtractor::Extract decides with this.
MediaWidthRange RetentionWidthRangeForCss(CapabilityMask::Viewport viewport,
                                          std::string_view css,
                                          const CascadeLayerOrder& order,
                                          bool* class_range = nullptr);

// What the visitor points with, for the `hover`, `any-hover`, `pointer` and
// `any-pointer` media features.
enum class MediaPointer : std::uint8_t {
  // Not known: those features neither exclude nor apply (the evaluator's
  // default for every feature it does not read).
  kUnknown,
  // A touch screen, as the phone and tablet analysis renders emulate
  // (src/browser/device_emulation.h): `hover: none`, `pointer: coarse`,
  // `any-hover: none` and `any-pointer: coarse` apply at every width;
  // `hover: hover`, `pointer: fine` and their any-* forms never do.
  kTouch,
};

// The pointer rule retention and position:fixed anchoring evaluate the
// hover/pointer features for a device class: kTouch for the phone and the
// tablet, whose analysis renders emulate a touch screen, kUnknown for the
// desktop. See the definition for why the desktop is not the reverse.
MediaPointer RetentionPointer(CapabilityMask::Viewport viewport);

enum class MediaWidthMatch : std::uint8_t {
  kNever,    // no width in the range satisfies any query in the list
  kApplies,  // some width in the range is known to satisfy it
  kUnknown,  // it may apply, but a feature other than width decides
};

// Evaluate an `@media ...` prelude (or a bare media query list) against a
// width range and a pointer. Reads `min-width` / `max-width` / `width`, the
// Media Queries Level 4 range forms (`width >= 48rem`, `40rem <= width <
// 64rem`, `>`/`<`, value on either side), `not`, `and`, `or`, `only`, the
// media types all / screen / print, comma lists and nested parentheses.
// Lengths in px, em, rem (16 px), in, cm, mm, q, pt and pc. Strict bounds
// fold to whole pixels. With a known `pointer` it also reads `hover`,
// `any-hover`, `pointer` and `any-pointer` (plain and boolean forms), which
// then apply or exclude at every width. Syntax it cannot follow yields
// kUnknown, never kNever.
MediaWidthMatch EvaluateMediaWidth(
    std::string_view media_rule, MediaWidthRange range,
    MediaPointer pointer = MediaPointer::kUnknown);

// Stable identity of a single style rule, independent of surrounding byte
// layout. Two rules share an identity iff they carry the same normalized
// selector under the same cascade-layer path AND @media condition. This is
// what lets the DOM-matched producer (which knows the enclosing @layer/@media
// during recursion) agree with a set of "must-retain" identities derived
// elsewhere (browser coverage, force-include augmentation) — a raw-selector
// match would collapse `.flex` in `@layer utilities` with `.flex` in a
// `@media` block and mis-key state-conditional Tailwind v4 variants.
struct RuleIdentity {
  // ">"-joined enclosing @layer names, outermost first (empty at top level;
  // anonymous @layer contributes an empty segment).
  std::string layer_path;
  // Normalized enclosing @media condition text (empty when unconditional;
  // nested @media conditions are joined with " and ").
  std::string media_condition;
  // Selector text after NormalizeSelector (trim + whitespace collapse).
  std::string normalized_selector;

  bool operator==(const RuleIdentity& other) const {
    return layer_path == other.layer_path &&
           media_condition == other.media_condition &&
           normalized_selector == other.normalized_selector;
  }

  template <typename H>
  friend H AbslHashValue(H h, const RuleIdentity& id) {
    return H::combine(std::move(h), id.layer_path, id.media_condition,
                      id.normalized_selector);
  }
};

// Where a stylesheet places one normalized selector: the distinct cascade-layer
// paths, and the distinct @media conditions, it occurs under. Internal to the
// relaxed force-include fallback (see CriticalCssExtractor::ForceIncludeIndex);
// the two axes are kept apart because they are adjudicated differently — a
// selector spanning two LAYERS is ambiguous, whereas a selector spanning two
// MEDIA conditions within one layer has a well-defined base occurrence.
struct SelectorContexts {
  absl::flat_hash_set<std::string> layer_paths;
  absl::flat_hash_set<std::string> media_conditions;
};

// Canonicalize a selector for identity comparison: trim leading/trailing
// whitespace and collapse every internal run of ASCII whitespace to a single
// space. Escapes are left intact (they are load-bearing in selector text, e.g.
// `dark\:bg-stone-900`), so this is deliberately NOT a wholesale
// UnescapeCssIdent — only the parts that would otherwise be unescaped per
// compound (id/class/tag) are unescaped by the matcher, and both the producer
// and any coverage parser run selectors through THIS function so their
// identities collapse identically.
std::string NormalizeSelector(std::string_view selector);

// The most bytes of rules the extractor adds for replaced elements outside
// the fold that may be wider than a phone (CriticalCssExtractor's
// wide_replaced_). 8 KiB: the case it exists for is a handful of sizing
// utilities (the async-CSS fixture needs 480 B), it keeps a typical 40-45 KB
// block well under kInlineCriticalCssMaxBytes (64 KiB), and past it the
// rules are per-element styling of a large icon or image set rather than
// the few sizing rules that stop an overflow.
inline constexpr size_t kMaxWideReplacedBytes = 8 * 1024;

// Configuration for Critical CSS extraction heuristics
struct CriticalCssConfig {
  // How many elements, counted from the first one inside <body>, to treat as
  // "above the fold" when nothing measured where the fold is.
  //
  // Counted from <body>, not from the start of the document: nothing in <head>
  // ever paints, and a modern page carries dozens of <meta>/<link>/<script>
  // elements there. The old count started at <html>, so on such a page the
  // budget was spent before the first visible element and the block covered
  // nothing of the fold (the async-CSS rendered probe measured a 0.42 flash
  // ratio against the modpagespeed.com fixture, whose <body> is element 42).
  // A document with no <body> element counts from element 0, as before.
  //
  // The default is sized for markup as it is written today: utility-class
  // pages spend hundreds of elements on a header, its (hidden) menus and a
  // hero. Measured on that fixture with the rendered probe's own comparison
  // (mobile / tablet / desktop fold pixels still unstyled): 25 body elements
  // 0.25 / 0.34 / 0.21, 150 unchanged, 200 0.0001 / 0.021 / 0.005, 250 and
  // 300 clean. The header's hidden dropdown menus occupy body ranks ~15-160
  // and the hero's own content ranks ~165-236, so the fold's rules only
  // start entering the block past 150. Per-rule matching keeps the block
  // bounded by what those elements actually use (44.6 KB of a 118 KB sheet
  // there), the worker caps it (kInlineCriticalCssMaxBytes), and a
  // browser-measured fold (measured_above_fold_selectors) is still the
  // precise channel.
  int max_elements = 300;

  // Maximum depth to consider critical (elements deeper are below fold)
  int max_depth = 10;

  // Always include these selectors regardless of element matching
  // (universal selectors that affect page layout)
  std::vector<std::string> always_include_selectors = {"*", "html", "body",
                                                       ":root"};

  // Tag name patterns to always include (header/nav elements)
  std::vector<std::string> include_tag_patterns = {"header", "nav", "main"};

  // Class patterns to always include (hero sections, etc.)
  std::vector<std::string> include_class_patterns = {"hero", "banner",
                                                     "masthead", "above-fold"};

  // ID patterns to always include
  std::vector<std::string> include_id_patterns = {"hero", "header", "nav",
                                                  "masthead"};

  // Class patterns to always exclude (below-fold content)
  std::vector<std::string> exclude_class_patterns = {"footer", "lazy", "defer",
                                                     "below-fold", "lazyload"};

  // ID patterns to always exclude
  std::vector<std::string> exclude_id_patterns = {"footer", "below-fold"};

  // Tag names to always exclude
  std::vector<std::string> exclude_tag_patterns = {"footer"};

  // ---------------------------------------------------------------------
  // Measured fold — a real browser render replaces the estimate above.
  // ---------------------------------------------------------------------
  //
  // `max_elements` is an ESTIMATE of where the fold is, and on a page with a
  // substantial <head> it is exhausted before the first visible body element:
  // the utility classes that lay out the header sit at element index ~45 and
  // are judged below the fold, so their rules never enter the critical block.
  //
  // Selector tokens for elements a browser measured above the fold. Two shapes
  // are honoured — "#id" and ".class" — each carrying the RAW DOM attribute
  // value, compared against CollectedElement::id / ::classes, which are likewise
  // raw HTML attribute values. (Raw, deliberately: a DOM class of
  // `dark:bg-stone-900` is written `.dark\:bg-stone-900` in the stylesheet, and
  // routing the token through a selector parser would only re-introduce that
  // escaping asymmetry.)
  //
  // Every other token shape is INERT, and that is load-bearing rather than
  // laziness: a bare tag token such as `div` would match essentially every
  // element in the document and promote the whole page to above-the-fold,
  // re-inlining most of a utility stylesheet. `*` and the empty string are
  // inert for the same reason.
  //
  // This is the ONLY measured channel, by design. An earlier revision also
  // carried the browser's COUNT of above-the-fold elements and used it to raise
  // `max_elements`. That is unsound and was removed: the count is consumed as an
  // index bound, but it counts elements with no layout box — the whole <head>,
  // and every `display:none` subtree — because getBoundingClientRect() returns
  // all-zeros for them and `top < viewportHeight` is therefore true. The
  // analyzed document is also not the served one (the analysis render has the
  // stylesheet inlined and a <base> injected, and scripts can add nodes), so the
  // two element orderings do not line up. Between them a count could promote an
  // entire document to critical. An element that only a tag rule describes
  // therefore still falls back to the `max_elements` estimate — i.e. exactly
  // today's behaviour, which is no regression.
  //
  // Empty = nothing measured: the heuristics above are then the only thing in
  // play, exactly as before.
  std::vector<std::string> measured_above_fold_selectors;

  // Maximum size (bytes) for wholesale inclusion of an @media block (inside a
  // @layer or not). Blocks larger than this are recursed into and filtered
  // per rule.
  // Default 4096 bytes (covers typical Tailwind responsive utility blocks).
  int max_wholesale_media_bytes = 4096;

  // What the worker will inline for a heuristic block: at most this many
  // bytes, and, once the combined stylesheet is at least
  // inline_limit_min_sheet_bytes, less than this share of it. Mirrors
  // kInlineCriticalCssMaxBytes / kInlineCriticalCssMaxCoverage
  // (browser_analysis_manager_internal.h; a test pins them together) and
  // WorkerConfig::async_css_min_deferred_bytes (the worker passes its own).
  // Rules kept for wide replaced elements outside the fold never take a block
  // past these: a block over them is not inlined at all.
  size_t inline_limit_bytes = 64 * 1024;
  float inline_limit_coverage = 0.60f;
  size_t inline_limit_min_sheet_bytes = 15000;
};

// Result of CSS extraction
struct CriticalCssResult {
  bool success = false;
  std::string error_message;
  std::string critical_css;

  // Statistics
  int total_rules = 0;
  int critical_rules = 0;

  // @media blocks were retained for the device class's windows
  // (RetentionWidthRange; for desktop that is every width) because the block
  // goes before the page's sheets. False when it goes after them and every
  // width was kept.
  bool class_range_retention = false;

  // The CSS has an anonymous layer and the block goes after the page's
  // sheets, so every anonymous-layer rule was left out of it.
  bool anonymous_layers_dropped = false;
};

// Thresholds for the async-CSS FOUC sufficiency gate (see WorkerConfig).
struct AsyncCssSufficiencyConfig {
  float min_coverage_ratio = 0.10f;
  size_t min_deferred_css_bytes = 15000;
};

// Returns true when the inlined critical CSS is sufficient to bridge first
// paint, so the full stylesheet may safely be DEFERRED (switched to
// rel="preload" as="style" by async-CSS). Returns false when the critical CSS
// is too thin relative to the sheet it would defer — deferring then produces a
// flash of unstyled content, so the caller must keep the stylesheet
// render-blocking.
//
// `coverage_ratio` is the browser profile's rule-level css_coverage_ratio when
// known; pass a negative value when unknown (the heuristic path). It is only
// ever trusted DOWNWARD: the gate always computes the extracted-bytes ratio
// (critical_css_bytes / deferred_css_bytes) and gates on the pessimistic of
// the two. A profile's coverage is measured pre-extraction and can wildly
// contradict the bytes actually inlined (a claimed 0.39 coverage next to
// 830 B critical / 115 KB deferred caused a live FOUC); an optimistic
// coverage number must never authorize deferral on its own. Semantics:
//   - external_css_unresolved (a declared external <link> stylesheet was NOT
//     resolved from cache) -> ALWAYS insufficient. This is the dominant
//     fail-safe: when an external sheet is missing, `deferred_css_bytes` does
//     not include it (the cold sheet was never gathered) AND the critical CSS
//     was derived WITHOUT it, so both the byte ratio and the small-sheet escape
//     hatch below are measuring the wrong, shrunken denominator. Never defer a
//     sheet we could not measure — keep it render-blocking (no FOUC). The caller
//     marks the variant for revalidation, so this self-heals once the sheet
//     caches and a real sufficiency decision can be made.
//   - min_coverage_ratio <= 0  -> always sufficient (gate disabled / legacy).
//   - deferred_css_bytes < min_deferred_css_bytes -> always sufficient (a small
//     sheet has a trivial FOUC window; avoids false-positives on light pages).
//   - otherwise sufficient iff min(byte ratio, known coverage_ratio) >=
//     min_coverage_ratio (the byte ratio alone when coverage is unknown/NaN).
bool CriticalCssIsSufficient(float coverage_ratio, size_t critical_css_bytes,
                             size_t deferred_css_bytes,
                             bool external_css_unresolved,
                             const AsyncCssSufficiencyConfig& cfg);

// Extracts critical CSS rules based on collected HTML elements.
//
// This extractor uses heuristics to determine which CSS rules are needed
// for above-the-fold content rendering:
//
// Always included:
// - Universal selectors (*, html, body, :root)
// - The first N elements inside <body> (configurable, see max_elements) and
//   the root <html> element
// - Elements matching header/nav/hero patterns in tag/class/id
// - Elements a `position:fixed` rule selects, and everything inside them: a
//   fixed box is in the viewport wherever it sits in the document, and the
//   chat launcher / cookie banner / floating CTA it usually is tends to be
//   the last thing in <body>, past any document-order estimate of the fold
// - For the mobile and tablet blocks, the rules of replaced elements that may
//   be wider than a phone until CSS shrinks them (an <img>, <iframe>,
//   <video>, an unsized <svg>, ... wherever they sit): one can widen the
//   document past the viewport, which shifts the whole fold. Bounded by
//   kMaxWideReplacedBytes
// - Non-selector at-rules that the retained rules DEPEND on, wherever they
//   sit in the layer/supports nesting: @charset, @import, @namespace,
//   @font-face, @keyframes, and the registrations (@property,
//   @counter-style, @font-palette-values, @font-feature-values,
//   @position-try). These are not styling and are not optional — a retained
//   rule referencing a registration that was dropped is invalid at
//   computed-value time, so omitting one changes what the SURVIVING rules
//   compute rather than merely removing a rule.
//
// Always excluded:
// - Elements with depth > max_depth (default 10)
// - Elements matching footer/lazy/defer patterns
// - @media print rules
//
// Usage:
//   CriticalCssExtractor extractor;
//   auto result = extractor.Extract(scan_result.elements, css);
//   if (result.success) {
//     // Use result.critical_css
//   }
class CriticalCssExtractor {
 public:
  CriticalCssExtractor();
  explicit CriticalCssExtractor(CriticalCssConfig config);
  ~CriticalCssExtractor();

  // Extract critical CSS rules from the given CSS that apply to the
  // collected elements.
  //
  // elements: Elements collected from HTML by HtmlScanner
  // css: Full CSS content to filter
  // viewport: Target viewport class for @media query filtering
  //           (default: kDesktop for backward compatibility)
  //
  // force_include: optional set of rule identities to emit even when they do
  //   NOT match any collected element. This augments the DOM matcher's blind
  //   spots (attribute selectors, state-conditional variants that cannot be
  //   captured from a static, JS-off, light-mode sample) without falling back
  //   to raw text: a forced rule is still emitted THROUGH the layered
  //   serializer, inside its own @layer/@media wrapper. nullptr disables it.
  //
  // layer_order: the page's cascade-layer order (Worker::BuildCombinedCss),
  //   which decides where the block goes and therefore which @media blocks it
  //   keeps (RetentionWidthRangeForCss). nullptr means not known: a block
  //   that uses a cascade layer then goes after the sheets and keeps every
  //   width. When the class range was used and the finished block (NUL bytes
  //   stripped, as the serve path places it) fails
  //   CriticalCssRetentionMayNarrow, the block is extracted again for every
  //   width, so a narrowed block is always one that goes first and has no
  //   anonymous layer.
  //
  // Returns the result with critical CSS rules
  CriticalCssResult Extract(
      const std::vector<CollectedElement>& elements, std::string_view css,
      CapabilityMask::Viewport viewport = CapabilityMask::Viewport::kDesktop,
      const absl::flat_hash_set<RuleIdentity>* force_include = nullptr,
      const CascadeLayerOrder* layer_order = nullptr);

  // Get the current configuration
  const CriticalCssConfig& config() const { return config_; }

 private:
  // Force-include lookup state, built once per Extract().
  //
  // Chrome reports CSS coverage as byte ranges over the stylesheet, and the
  // critical blob is the plain concatenation of those ranges: a used range
  // covers the rule, never the enclosing `@layer ... {` / `@media ... {`
  // prelude. So an identity derived from coverage arrives as {"", "", selector}
  // and can never key-match a rule the producer sees inside a wrapper. On a
  // primary-key miss the lookup therefore retries with both context fields
  // relaxed — but only for force-include entries that carry no context of
  // their own, and only against ONE resolved occurrence, chosen as follows:
  //
  //   - Two distinct cascade LAYERS is genuine ambiguity: the same selector in
  //     `@layer base` and `@layer utilities` are different rules with different
  //     precedence and nothing in a context-free identity says which the
  //     browser used. Matches NOTHING.
  //   - Within a single layer, several @media conditions are NOT ambiguity.
  //     A context-free coverage identity means the browser used the selector
  //     with no media qualification in play, which is the UNCONDITIONAL
  //     occurrence — so that one is chosen and the responsive overrides are
  //     left to whatever the normal @media handling does with them. Choosing
  //     "ambiguous" here would drop the base rule while its own breakpoint
  //     overrides ride along, leaving e.g. `.container` with a `max-width` and
  //     no `width` — worse than either alternative.
  //   - A single layer with exactly one occurrence resolves to that occurrence,
  //     media-qualified or not; that is the responsive-variant case where the
  //     browser used a rule that only exists inside a breakpoint.
  //   - A single layer, several media conditions, none of them unconditional:
  //     no base occurrence exists to prefer, so this matches NOTHING.
  //
  // Relaxation therefore never crosses a layer boundary, and within a layer it
  // resolves to at most one occurrence. Over-inclusion stays bounded and the
  // choice is deterministic.
  struct ForceIncludeIndex {
    explicit ForceIncludeIndex(
        const absl::flat_hash_set<RuleIdentity>& exact_ids)
        : exact(exact_ids) {}

    // Full identities, matched exactly.
    const absl::flat_hash_set<RuleIdentity>& exact;
    // Normalized selectors of the context-free force-include entries.
    absl::flat_hash_set<std::string> relaxable;
    // Normalized selector -> where the sheet places it.
    absl::flat_hash_map<std::string, SelectorContexts> sheet_contexts;

    // True when the rule currently being processed — identified by its own
    // enclosing layer path and media condition — is the occurrence the relaxed
    // key resolves to.
    bool MatchesRelaxed(const std::string& normalized_selector,
                        const std::string& layer_path,
                        const std::string& media_condition) const;
  };

  // Builds the index for one Extract(). `relaxable` and `sheet_contexts` stay
  // empty (and the census walk is skipped) when no force-include entry is
  // context-free, so the exact-match path costs nothing extra.
  static ForceIncludeIndex BuildForceIncludeIndex(
      const std::vector<struct CssRule>& rules, MediaWidthRange retention_range,
      MediaPointer retention_pointer,
      const absl::flat_hash_set<RuleIdentity>& force_include);

  // Recursively process CSS rules, handling @layer/@supports container
  // blocks by recursing into their inner rules. Returns the critical CSS
  // for the given rules. max_depth bounds recursion (default 5).
  // A @media block larger than config_.max_wholesale_media_bytes is recursed
  // into and filtered per rule rather than included wholesale.
  // layer_path / media_condition: the enclosing cascade-layer path and @media
  //   condition accumulated during recursion; used to build each rule's
  //   RuleIdentity for the force_include lookup.
  // force_index: identities to emit even without a DOM match (nullptr = off).
  std::string ProcessRules(const std::vector<struct CssRule>& rules,
                           const std::vector<CollectedElement>& elements,
                           CapabilityMask::Viewport viewport,
                           int& critical_count, int max_depth,
                           const std::string& layer_path,
                           const std::string& media_condition,
                           const ForceIncludeIndex* force_index);

  // Check if a selector should be included based on heuristics
  bool ShouldIncludeSelector(std::string_view selector,
                             const std::vector<CollectedElement>& elements);

  // Check if selector matches any of the collected elements
  bool SelectorMatchesAnyElement(std::string_view selector,
                                 const std::vector<CollectedElement>& elements);

  // Parse a simple selector and check if it matches an element
  bool SimpleSelectorMatchesElement(std::string_view selector,
                                    const CollectedElement& element);

  // Check if a string contains any of the patterns (case-insensitive)
  bool ContainsPattern(std::string_view str,
                       const std::vector<std::string>& patterns);

  // Check if element should be excluded based on patterns
  bool IsElementExcluded(const CollectedElement& element);

  // Check if element should be included based on patterns
  bool IsElementIncluded(const CollectedElement& element);

  // Index config_.measured_above_fold_selectors for lookup. Called from every
  // constructor; config_ is fixed after construction, so this runs once per
  // extractor and never inside the rule walk.
  void BuildMeasuredFoldIndex();

  // True when a real browser render measured THIS element above the fold.
  //
  // Deliberately a raw set lookup rather than a trip through
  // SimpleSelectorMatchesElement: the tokens hold DOM attribute values and so
  // do CollectedElement::id / ::classes, whereas the selector parser expects
  // CSS identifier syntax and would need `dark\:bg-stone-900` where the DOM
  // says `dark:bg-stone-900`. Comparing the raw values is both correct and
  // O(1) — and this runs per element, per selector, over the whole sheet.
  bool MeasuredFoldContains(const CollectedElement& element) const;

  CriticalCssConfig config_;

  // element_index of the <body> element of the document being extracted (0
  // when it has none), set at the top of every Extract(). The
  // config_.max_elements budget is counted from here.
  int fold_index_base_ = 0;

  // How many anonymous `@layer { }` blocks enclose the rules ProcessRules is
  // emitting. Inside one, `!important` declarations are left out of the block
  // (CriticalCssMayCarryImportantInAnonymousLayer): the block's
  // anonymous layer is a different, earlier layer than the sheet's, and the
  // earlier layer wins for `!important`. Reset by every Extract() pass.
  int anonymous_layer_depth_ = 0;

  // The block goes after the page's sheets, so it carries no
  // anonymous layer at all (an anonymous layer there is registered after the
  // sheet's and wins every normal declaration). Set by every Extract().
  bool drop_anonymous_layers_ = false;

  // The window widths rule retention keeps @media blocks for, set at the top
  // of every Extract() (RetentionWidthRangeForCss), and the pointer it reads
  // the hover/pointer features with: the class's (RetentionPointer) under the
  // class range, unknown under every width.
  MediaWidthRange retention_range_{0, kMediaWidthUnbounded};
  MediaPointer retention_pointer_ = MediaPointer::kUnknown;

  // element_index of every element a position:fixed rule selects, and of
  // every element inside one. A fixed box is in the viewport wherever it sits
  // in the document, and the things authors fix — chat launchers, cookie
  // banners, floating CTAs — are typically the last children of <body>, past
  // any document-order estimate of the fold. Built once per Extract().
  // Only a rule whose OWN declarations (nested blocks excluded) say
  // `position:fixed` without `display:none` / `visibility:hidden` /
  // `opacity:0` anchors; a rule under an @media that cannot apply at the
  // viewport, or whose compound needs an interaction state (`:focus`,
  // `:hover`, ...), does not. Hidden elements (CollectedElement::hidden) are
  // neither anchors nor part of an anchored subtree, and one anchor pulls in
  // at most kMaxFixedAnchorSubtree elements. Every exclusion is towards
  // under-inclusion, the safe direction.
  //
  // Both fields above are per-call state: Extract() is not reentrant, so one
  // extractor must not serve concurrent Extract() calls (the worker builds
  // one per call).
  absl::flat_hash_set<int> fixed_anchored_;

  // Every class and id anywhere in the document (not only in the fold), so a
  // rule whose ancestor compounds name one the page never uses can be left
  // out (`.prose :where(p)` on a page without `.prose`). Only
  // tokens outside :where()/:is()/:not()/:has() arguments are checked;
  // html / body / :root compounds are exempt, and so are the root state
  // classes script commonly adds before first paint (`dark`, `light`, `js`),
  // so Tailwind's `.dark .dark\:x` dark variant is kept on a page whose
  // markup does not carry `dark`. Built once per Extract().
  absl::flat_hash_set<std::string> page_classes_;
  absl::flat_hash_set<std::string> page_ids_;
  void BuildPageTokens(const std::vector<CollectedElement>& elements);
  bool AncestorsOnPage(const std::vector<std::string_view>& ancestors) const;
  void BuildFixedAnchors(const std::vector<struct CssRule>& rules,
                         const std::vector<CollectedElement>& elements,
                         CapabilityMask::Viewport viewport);

  // Replaced elements that may be wider than a phone until CSS shrinks them
  // (CollectedElement::may_exceed_viewport: an <img>, <iframe>, <video>, an
  // unsized outermost <svg>, ... whose markup states no width that fits 320
  // px), and the budget their rules draw on. Mobile and tablet variants only.
  //
  // Such an element widens the document wherever it sits: one 300 px icon
  // 75 px from the left edge of a 375 px phone, or a `width="560"` video
  // embed, and the phone grows its layout viewport to the content width, so
  // the whole fold shifts and its fixed buttons move off-screen
  // (the FAQ chevron icon of the async-CSS probe fixture, 13,000 px
  // down). So a rule matching one of them is kept although the element is
  // outside the fold, and outside the depth and pattern vetoes, which only
  // estimate the fold. Only the element itself, not its subtree; hidden
  // elements and everything inside one are left out. A desktop window is
  // wide enough that this almost never happens there, and the block is not
  // grown for it.
  //
  // A rule admitted ONLY this way costs its bytes against the budget,
  // kMaxWideReplacedBytes, in sheet order; the first rule that does not fit
  // closes it and nothing further is admitted this way. The budget shrinks to
  // whatever keeps the block within the worker's inline limits
  // (CriticalCssConfig::inline_limit_bytes / _coverage). An icon-heavy
  // page with per-icon utility classes would otherwise double the block (43
  // KB -> 99 KB measured), past kInlineCriticalCssMaxBytes, which loses the
  // inlining altogether. A small @media block is copied whole only for a rule
  // the fold itself needs, never for one admitted this way.
  //
  // Per-call state, reset for every ProcessRules pass.
  std::vector<size_t> wide_replaced_;  // indices into the elements vector
  size_t wide_replaced_budget_ = 0;
  size_t wide_replaced_bytes_ = 0;
  // Bytes of the enclosing @layer / @media wrappers not yet in the block: the
  // first budget-only rule inside them pays for them, so the budget bounds the
  // emitted bytes, wrappers included. A rule the fold needs brings them in
  // for free.
  size_t pending_wrapper_bytes_ = 0;
  size_t EnterWrapper(std::string_view prelude);  // returns the outer value
  void LeaveWrapper(size_t outer_pending);
  bool wide_replaced_closed_ = false;
  int fold_admissions_ = 0;  // style rules admitted by any other path
  void BuildWideReplacedElements(const std::vector<CollectedElement>& elements,
                                 CapabilityMask::Viewport viewport);
  bool SelectorMatchesWideReplacedElement(
      std::string_view selector, const std::vector<CollectedElement>& elements);

  // Raw DOM id / class values from config_.measured_above_fold_selectors.
  // Tokens of any other shape are dropped here — see the config field.
  absl::flat_hash_set<std::string> measured_fold_ids_;
  absl::flat_hash_set<std::string> measured_fold_classes_;
};

}  // namespace pagespeed

#endif  // PAGESPEED_SRC_WORKER_CRITICAL_CSS_EXTRACTOR_H_
