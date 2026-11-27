// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

// PageSpeed 2.0 - the page's cascade-layer order, for the critical block
//
// WHY THIS EXISTS. The inlined critical <style> goes before the
// page's first stylesheet source, so the full sheets win every same-specificity
// tie once they apply. A cascade layer's position, though, is fixed by where it
// is FIRST mentioned, and a block placed first mentions its layers before any
// sheet does, in the order the block happens to list them: the combined sheet's
// order (inline <style> bodies first, then the external sheets), limited to
// what was gathered. That differs from the author's order when an inline
// <style> follows a <link>, or when a layered sheet is missing from the
// combined sheet (a cross-origin sheet, a cold cache).
//
// THE FIX. Put an `@layer a, b, a.c;` statement at the top of the block that
// names every layer in the order the browser first meets it in the original
// document. The statement is then the first mention of every layer, and the
// order it declares is the author's order, so everything after it (the block's
// own @layer rules, the page's sheets, sheets the async-CSS loader activates
// later) only mentions layers that already exist.
//
// Which sources count is the scanner's business (StylesheetSource,
// html_scanner.h): exactly the sheets the combined CSS reads, by the one
// stylesheet-source rule, so sources a browser running scripts never applies
// (in <noscript> or <template>, a non-CSS `type`, a preload without onload)
// are not sources at all, and a loadCSS preload is its sheet at the preload's
// own position.
//
// That is only right when the order is PROVEN. ComputeCascadeLayerOrder walks
// every stylesheet source in document order and refuses (proven == false, with
// a reason) whenever it cannot know what the browser would do:
//   - a source's content is not known: a <link> the combined-CSS gather did not
//     read (cross-origin, not cached yet, over the size cap, a loadCSS preload
//     the scan did not pair with its sheet), a link that is not a plain active
//     stylesheet (`alternate`, `disabled`), a titled sheet (a sheet set, of
//     which a browser applies only the preferred one), a truncated <style>
//     body, or an @import whose sheet is not in cache;
//   - a layer is first mentioned under a condition: inside @media, @supports
//     or any other block that is not @layer itself (a nested rule included),
//     under a <link>/<style> media attribute other than `all`, or an @import
//     with a media or supports() condition. Chromium does not
//     register a layer declared under a condition that does not match (checked
//     in a real browser for @media, @supports and a link's media attribute),
//     and the window the page will be shown in is unknown here, so the
//     position of such a layer depends on the visitor;
//   - an anonymous layer (`@layer { }`, `@import ... layer`) comes before the
//     first mention of a named layer with the same parent: the statement would
//     move the named layer ahead of the anonymous one;
//   - a layer name or an at-rule keyword this parser does not read with
//     certainty (an escape, a CSS-wide keyword, something that is not an
//     identifier), or a stray `}` / `;` where a rule may start, which makes a
//     browser drop the rule after it;
//   - a sheet declares a new layer after a script that may insert a stylesheet
//     (LayerOrderSource::is_script). Scripts that insert sheets without any of
//     the recognised signs (an external async/defer/module script, or an
//     inline one that does it indirectly) are a residual risk this cannot see:
//     the statement overrides whatever order such a sheet would have set. That
//     covers a layer only the inserted sheet declares (it lands after every
//     layer the statement lists), and also a sheet inserted ahead of the
//     page's first stylesheet that declares known layers in another order
//     (say `@layer b, a;` before a <link> that declares a then b: the page
//     orders b first, the statement a first). A sheet inserted at the very
//     start of <head>, before the block, is unaffected: the statement is not
//     the first mention there.
// A later mention of a layer that is already known changes nothing, whatever
// its condition, so it never refuses.
//
// DecideCriticalCssLayerPlacement is the one rule the serve path
// (HtmlTransformFilter::InjectCriticalCss) and the critical-CSS validator
// (BuildValidationDocuments) both apply to the order: an unlayered block goes
// first as before; a layered block goes first with the statement in front of
// it when the order is proven and covers every layer the block names; any other
// layered block keeps the end-of-head placement before </head>.

#ifndef PAGESPEED_SRC_WORKER_CASCADE_LAYER_ORDER_H_
#define PAGESPEED_SRC_WORKER_CASCADE_LAYER_ORDER_H_

#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace pagespeed {

// One stylesheet source of the page, in document order.
struct LayerOrderSource {
  // The source's CSS is known. When false, `unavailable_reason` says why, and
  // the order cannot be proven.
  bool available = false;
  std::string unavailable_reason;
  // The source's own CSS: a <style> body, or a <link>'s sheet as fetched (NOT
  // with its @imports flattened: they are resolved through the lookup below).
  std::string css;
  // What the source's relative @import URLs resolve against: the sheet's URL,
  // or the page's URL for a <style>.
  std::string base_url;
  // The source applies only under a condition that cannot be evaluated here
  // (a media attribute other than `all`).
  bool conditional = false;
  // Not a stylesheet but a script that may insert one where it stands
  // (document.write from a parser-blocking script, or an inline script that
  // builds a <style>/<link>, calls insertRule, sets adoptedStyleSheets or
  // carries `@layer`). No sheet after it may declare a new layer: the order
  // relative to what the script inserts is unknown. `available` and the CSS
  // fields are ignored.
  bool is_script = false;
};

// True when a <link>/<style> media attribute always applies: absent (empty) or
// `all`. Anything else is a condition this code does not evaluate.
bool LayerOrderMediaIsUnconditional(std::string_view media);

// An @import target: its CSS and its absolute URL (for its own @imports).
struct LayerOrderImport {
  std::string css;
  std::string url;
};

// Resolves `href` (as written in the @import) against `base_url` and returns
// the imported sheet, or nullopt when it is not available.
using LayerOrderImportLookup = std::function<std::optional<LayerOrderImport>(
    std::string_view base_url, std::string_view href)>;

// The page's cascade-layer order, or why it is not known.
struct CascadeLayerOrder {
  // Default-constructed: not proven, so a layered block keeps the fallback.
  bool proven = false;
  // Why the order is not proven. Empty when proven.
  std::string reason = "the page's layer order was not computed";
  // Every named layer, fully qualified (`a.b` for `b` inside `a`), in the
  // order the browser first registers it. Parents come before children.
  std::vector<std::string> names;
  // Not proven because an @import's sheet is not in cache. Like a declared
  // sheet missing from cache (external_css_missing), that can change once the
  // import is fetched, so the serve path does not ask for a revalidation on
  // it: a record made now would bind to `unproven` and mismatch again.
  bool import_missing = false;

  // `@layer a,b,a.c;`, or empty when there are no named layers.
  [[nodiscard]] std::string Statement() const;

  // What a validation record binds to (CombinedCssValidationHash) on a page
  // whose combined sheet is `combined_css`. Empty when that sheet uses no
  // cascade layer (CriticalCssMayUseCascadeLayer): a block derived from it
  // uses none either, so it goes first without a statement whatever the
  // order is, and the record keeps hashing the sheet alone, as before the
  // block-first placement.
  // Records of pages without layers therefore stay valid; records of layered
  // pages, whose placement this order now decides, are made again. Otherwise
  // the binding reads
  //
  //   unproven[ a2][ u2]
  //   proven [r2 ][a2 ][u2 ]<statement>
  //
  // where <statement> is Statement() (empty when the order has no named
  // layer, hence the trailing space of a bare "proven ") and each marker, in
  // this fixed order, says the block differs from what records made before
  // the marker existed had validated:
  //   - r2: @media retention narrows to the device class's windows
  //     (CriticalCssRetentionMayNarrow);
  //   - a2: the sheet may carry an `!important` inside an anonymous layer,
  //     which the block leaves out
  //     (CriticalCssMayCarryImportantInAnonymousLayer);
  //   - u2: the sheet has an anonymous layer and the block goes after the
  //     sheets, so it carries no anonymous-layer rule at all
  //     (CssTextHasAnonymousLayer and not CriticalCssBlockGoesFirst).
  // r2 never appears with u2: narrowing requires a block that goes first.
  // The record's binding salts this further (ValidationBindingFor,
  // src/browser/optimization_profile.h), among others with the derivation's
  // CriticalCssResult::anonymous_layers_dropped, which also covers a block
  // that only the check on the finished block moved after the sheets.
  [[nodiscard]] std::string ValidationBinding(
      std::string_view combined_css) const;
};

// Walk `sources` in document order and compute the order, refusing whenever it
// cannot be proven (see the file comment). `lookup` may be empty: every @import
// is then unavailable.
CascadeLayerOrder ComputeCascadeLayerOrder(
    const std::vector<LayerOrderSource>& sources,
    const LayerOrderImportLookup& lookup);

// Where the critical block goes relative to the page's stylesheets.
struct CriticalCssLayerPlacement {
  // Keep the end-of-head placement (before </head>, after every head sheet),
  // with no statement.
  bool keep_fallback = false;
  // Text to put in front of the block when it goes first. Empty when the block
  // names no layer or the page has none.
  std::string prefix;
};

// True when `css` may use a cascade layer at all: it names one
// (CriticalCssNamesCascadeLayer), has an anonymous `@layer {`, or has an
// at-rule keyword written with an escape (`@l\61yer` is @layer to a browser).
// Textual and over-inclusive, like that detector.
bool CriticalCssMayUseCascadeLayer(std::string_view css);

// The rule both the serve path and the validator apply (see the file comment):
//   - the block uses no layer (CriticalCssMayUseCascadeLayer): first, no
//     prefix;
//   - it does, the order is proven and lists every named layer the block
//     mentions: first, prefix = order.Statement(). An anonymous layer counts
//     too: placed first without the statement, the block's anonymous layer
//     would be registered ahead of every named layer, where its `!important`
//     declarations win against them;
//   - otherwise: keep the fallback.
CriticalCssLayerPlacement DecideCriticalCssLayerPlacement(
    std::string_view block, const CascadeLayerOrder& order);

// Whether a critical block derived from `combined_css` will go before the
// page's stylesheets: DecideCriticalCssLayerPlacement applied to
// the combined sheet itself. True for a sheet that uses no cascade layer, and
// for a layered sheet whose order is proven and lists every layer the sheet
// names; false otherwise. Rule retention (RetentionWidthRangeForCss) narrows
// @media blocks to the device class's windows only when this holds, because
// only a block that goes first loses every tie to the sheet.
//
// Deciding on the combined sheet rather than on the finished block breaks the
// cycle (placement reads the block, retention shapes it) and is a superset
// test: a block names only layers the sheet it came from names. The extractor
// still checks the finished block against DecideCriticalCssLayerPlacement and
// extracts again for every width when a narrowed block would not go first.
bool CriticalCssBlockGoesFirst(std::string_view combined_css,
                               const CascadeLayerOrder& order);

// Whether @media retention may narrow to the device class's windows for a
// block derived from `css`: the block goes first
// (CriticalCssBlockGoesFirst) AND `css` has no anonymous layer at all
// (`@layer { }`, `@import ... layer`, nested ones included).
//
// Why anonymous layers are excluded: every `@layer { }` creates a NEW layer,
// so the block's anonymous layer is registered before the sheet's and is a
// different layer. For `!important` declarations the EARLIER layer wins, so a
// block placed first beats the sheet's `!important` override in its own
// anonymous layer: `.x{display:none!important}` kept from the block wins over
// the sheet's `@media (min-width:64rem){.x{display:block!important}}` for as
// long as the page is open. Keeping every width keeps the block's own copy of
// that override. (Named layers are shared by the block and the sheet, where
// the later sheet wins either way.) Tailwind v4 uses no anonymous layer.
//
// Also what the validation binding keys on (ValidationBinding: "proven r2").
// Applied to the finished block too, after NUL bytes are stripped as the
// serve path does, so a narrowed block is always one this holds for.
bool CriticalCssRetentionMayNarrow(std::string_view css,
                                   const CascadeLayerOrder& order);

// True when `css` may hold an `!important`: a `!` followed, after whitespace
// and comments, by the keyword `important` in any case, or by a keyword with
// a backslash anywhere in it (Chromium decodes `!\69mportant`,
// `!i\6dportant` and `!imp\ortant` as `!important`). Textual and
// over-inclusive: a `!` in a comment or a string counts.
bool CssTextHasImportantBang(std::string_view css);

// True when `rule` starts with an at-rule whose keyword, as written, has a
// backslash in it (`@l\61yer`, `@\6c ayer`). A browser decodes the escape
// (both spell @layer); nothing here does, so such a rule may be anything, an
// anonymous layer included. The placement side already treats it as a
// possible layer (CriticalCssMayUseCascadeLayer), and the extractor leaves it
// out of a block that goes after the sheets.
bool CssAtRuleKeywordHasEscape(std::string_view rule);

// True when `css` may contain an anonymous layer: `@layer` followed, after
// whitespace and comments, by `{`; an `@import` whose prelude carries the
// word `layer` without a name; or an at-rule with a block whose keyword is
// written with an escape (`@l\61yer {` is `@layer {` to a browser, and the
// keyword is not decoded here, so `@l\61yer a {` counts too).
// Textual and over-inclusive (comments and strings count).
bool CssTextHasAnonymousLayer(std::string_view css);

// True when `css` may carry an `!important` declaration inside an anonymous
// layer (`@layer { }`, nested ones included), or has an anonymous
// `@import ... layer` whose sheet is not read. Over-inclusive. Why:
// every `@layer { }` is a new layer, and the block's anonymous layer, placed
// first, comes before the sheet's; for `!important` the EARLIER layer wins,
// so such a declaration in the block would beat the sheet's own `!important`
// override for good. CriticalCssExtractor leaves those declarations out of the
// block; the validation binding marks pages whose combined sheet has one
// ("a2").
bool CriticalCssMayCarryImportantInAnonymousLayer(std::string_view css);

}  // namespace pagespeed

#endif  // PAGESPEED_SRC_WORKER_CASCADE_LAYER_ORDER_H_
