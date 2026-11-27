// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

// PageSpeed 2.0 - HTML Scanner for Worker
//
// Read-only HTML element collector. Scans HTML to collect elements,
// stylesheet links, and inline CSS without modifying the HTML.
// This is used for Critical CSS extraction.

#ifndef PAGESPEED_SRC_WORKER_HTML_SCANNER_H_
#define PAGESPEED_SRC_WORKER_HTML_SCANNER_H_

#include <string>
#include <string_view>
#include <vector>

#include "lib/base/string_util.h"

namespace net_instaweb {
class HtmlElement;
}  // namespace net_instaweb

namespace pagespeed {

// Information about a collected HTML element
struct CollectedElement {
  std::string tag_name;
  std::string id;
  std::vector<std::string> classes;
  int depth = 0;
  int element_index = 0;
  // The markup declares the element invisible (IsInvisibleElement below: the
  // `hidden` attribute, an inline display:none / visibility:hidden, 0/1 px
  // dimensions). Evidence-based; absent evidence means visible.
  bool hidden = false;
  // A replaced element that may be wider than a phone's viewport until author
  // CSS shrinks it (MayExceedPhoneViewport below). The extractor keeps such an
  // element's rules for the mobile and tablet blocks wherever it sits, because
  // without them it can widen the whole document.
  bool may_exceed_viewport = false;
};

// Information about an external stylesheet link
struct StylesheetLink {
  std::string href;
  std::string media;  // media attribute if present
  // True when the browser may reach first paint without waiting for this
  // sheet: it is applied by script (an author preload with an onload handler,
  // or `media=print` with an onload handler) or it stands inside <body>, after
  // content. Inlined critical CSS may then be the only styling of the fold.
  bool may_not_block_paint = false;
  // `media=print` with an onload handler that sets the media: the print-media
  // trick a page uses to load a screen sheet without blocking render (the
  // handler sets the media to `all` once the sheet loads), so its rules are
  // screen rules. A different handler does not make it a screen sheet.
  bool print_media_swapped_on_load = false;
};

// The media a <link>'s rules apply under, as the combined stylesheet the
// critical block is derived from scopes them (CombinedSheetMediaScope in
// lib/css/css_import_flattener.h): the media attribute, or none for the
// print-media trick, whose handler applies the sheet to every medium.
inline std::string_view CombinedSheetMediaOf(const StylesheetLink& link) {
  return link.print_media_swapped_on_load ? std::string_view() : link.media;
}

// A stylesheet source as the browser's cascade-layer order sees it, in
// document order. The sources are exactly the elements the combined-CSS
// assembly reads (one rule, one implementation in html_scanner.cc):
// each <style> collected into `inline_css`, and each <link> collected into
// `stylesheets`, at the position the browser applies it (a loadCSS preload at
// the preload's own position). So sources inside <noscript>, <template>,
// <noembed> or <noframes>, a <link> in <svg>/<math>, a <style> in <math>, a
// non-CSS `type` (text/tailwindcss, text/x-less) and a preload without an
// onload handler are not sources. Beyond that list, a link that a browser
// running scripts still applies but the gather does not read is recorded with
// no gathered sheet (stylesheet_index -1), so it leaves the order unproven: a
// loadCSS preload without a <noscript> twin, or a rel token list that names
// `stylesheet` in a spelling the gather does not match. The worker's own
// markup is read the way the serve path reads it (its previous critical
// <style> and its <noscript> copy of a deferred link are skipped, a deferred
// primary is its original sheet). Also recorded, in place: a <script> that
// may insert a stylesheet where it stands (may_insert_stylesheet). Feeds
// ComputeCascadeLayerOrder (cascade_layer_order.h) through
// Worker::BuildCombinedCss.
struct StylesheetSource {
  bool is_link = false;
  // For a link: its index in HtmlScanResult::stylesheets, the list the
  // combined-CSS gather reads; -1 when the gather does not read it.
  int stylesheet_index = -1;
  // For a link: a rel list with `alternate`, or a `disabled` attribute. Not a
  // plain active stylesheet, so what it contributes to the layer order is not
  // known.
  bool not_plain_stylesheet = false;
  // Has a `title`: part of a sheet set, of which a browser applies only the
  // preferred one.
  bool titled = false;
  // Not a stylesheet: a <script> that may insert one here (a parser-blocking
  // external classic script, or an inline script that writes, builds or
  // adopts a sheet or carries `@layer`). Every other field is unset.
  bool may_insert_stylesheet = false;
  std::string href;
  // The media attribute (for a deferred primary, the recorded author media).
  std::string media;
  // For a <style>: its body.
  std::string css;
  // The body went over the scanner's inline-CSS cap and is incomplete.
  bool truncated = false;
};

// True when a stylesheet's media attribute is exactly "print"
// (case-insensitive, whole-value).  Such a sheet is never render-blocking
// for screen, so it must not be preload-hinted.
inline bool StylesheetMediaIsPrint(std::string_view media) {
  return net_instaweb::StringCaseEqual(media, "print");
}

// Candidate Largest Contentful Paint image detected by heuristic: the first
// eligible <img> in a hero container, else the first eligible body <img>.
// Eligible means a non-data: src, not IsUnlikelyLcpImage, and not inside
// <noscript>, <template>, <noembed> or <noframes>, which a browser running
// scripts never creates (the same `inert` rule as `elements`).
// That scope is the lexer's markup parse of <noscript> (kSometimesLiteralTags
// in lib/html/html_lexer.cc), not the browser's RAWTEXT rule, which ends the
// element at the first `</noscript>` byte sequence; the two diverge on a
// nested <noscript>, a `</noscript>` inside a comment or script string within
// the noscript, and a <noscript> inside <svg>. Where they differ the scanner's
// scope is the longer one, so an image the browser does create is skipped
// and a later real image is the candidate, which is the less harmful side.
//
// One shape yields no candidate at all (`script_loaded_hero`):
// the first hero container holds a lazy-load placeholder, an <img> without a
// usable src that a loader fills in (`<img data-src=hero.jpg class=lazy>`),
// followed by its <noscript> copy, and no eligible image of its own. Nothing in
// the markup says which bytes the loader fetches, so there is nothing right to
// hint, and hinting the next image instead (probably below the fold) is worse
// than no hint. Such a placeholder outside a hero container, or without the
// <noscript> copy, changes nothing: the next eligible image is the candidate.
//
// What counts as the placeholder:
//   1. no usable src (absent, empty, or `data:`) plus a lazy data attribute
//      (HasLazySourceAttribute) or a lazy-loader class;
//   2. a lazy data attribute plus a src whose file name is a common stand-in
//      (IsLazyStandInSrc: blank, placeholder, pixel, spacer, 1x1, transparent,
//      loading, lazy), such as `<img src=/blank.gif data-src=/hero.jpg>`;
//   3. inside a hero container only: a lazy data attribute plus any other src,
//      once its <noscript> copy turns out to carry a DIFFERENT src. The copy is
//      the author saying what the real image is, so the <img> is a stand-in
//      (a low-quality preview, say) whatever its own src. The copy comes after
//      the <img>, so until it is seen such an <img> is the candidate as before,
//      and it stays the candidate when no copy follows, or the copy has the
//      same src. "Same" is the resolved reference (host, path and query;
//      scheme and fragment dropped), so `http://example.com/hero.jpg` is
//      `/hero.jpg` while `/hero.jpg?v=2` is not known to be. A lazy-loader
//      class alone does not make a real src a stand-in (`<img src=/hero.jpg
//      class=lazy>` is the hero, and so is `<img src=/blank.gif class=lazy>`
//      whatever its copy says).
// In every leg the copy has to be a plausible one: a <noscript> image the
// markup declares invisible (IsUnlikelyLcpImage: 0/1 px, hidden) is a tracking
// pixel's no-JS fallback, not the author's copy of the hero, and confirms
// nothing. Legs 1 and 2 are IsLazyLoadPlaceholder in html_scanner.cc; the
// transform filter's own "no usable source" test shares leg 2 (never promoted
// by the fetchpriority fallback). The transform filter does not read srcset
// for leg 1 the way the scanner does: `<img src="data:..." srcset=/hero.jpg
// data-src=/x>` is a placeholder for the scanner (no usable src) and a
// loading image for the filter (it has a srcset).
//
// Inside the placeholder's hero container, an eligible image whose declared
// size is small (IsSmallDeclaredImage: width x height under
// kSmallImageAreaPx2, a 48x48 icon in the call-to-action) neither cancels the
// pattern nor becomes the candidate: with the <noscript> copy seen, the page
// gets no candidate. Without the copy the pattern is not confirmed, and the
// icon is what today's first-eligible-image rule picks, exactly as before; a
// page without a placeholder is not touched by this at all. An image without
// a declared size, or at or above the threshold, cancels the pattern as
// before (unknown is not small). The tolerance applies while a placeholder is
// pending: an icon that PRECEDES the placeholder in the container is the
// first eligible image and the candidate, as before.
struct LcpCandidate {
  std::string src;
  std::string srcset;  // if present
  std::string sizes;   // sizes attribute if present
  int element_index = -1;
  // True when the <img> is a direct child of <picture>.  A <source> sibling
  // may win source selection, so preloading the img's src risks a double
  // download — preload emitters must skip such candidates.  fetchpriority on
  // the <img> element itself stays correct whichever source wins.
  bool in_picture = false;
  // True when the page's hero is loaded by script (see above). src is empty
  // then, and the no-candidate fallbacks must stay quiet too: no preload, no
  // Early Hint, and no fetchpriority="high" on a later image.
  bool script_loaded_hero = false;
};

// True for elements whose markup declares them invisible: tiny explicit
// dimensions (0/1 px — tracking beacons and 0x0 tracking iframes), the
// hidden attribute, or an inline style that removes the element from
// rendering (display:none / visibility:hidden).  Evidence-based (attributes
// only), deliberately no URL patterns; absent evidence means visible, so an
// element with no size info and no hiding markers is NOT invisible.
bool IsInvisibleElement(const net_instaweb::HtmlElement& element);

// The narrowest phone viewport, in CSS px. A width the markup states up to
// this fits every phone.
inline constexpr int kNarrowestPhoneViewportPx = 320;

// True for a rendered replaced element whose width, before author CSS
// applies, may exceed kNarrowestPhoneViewportPx: an <img>, <iframe>,
// <canvas>, <video>, <embed>, <object>, an <audio controls>, or an outermost
// inline <svg>, whose markup states no width that is known to fit. A missing
// width falls back to the element's own size (an image's pixels, a video's
// frames) or the UA's 300 x 150, and a stated `width="560"` (the YouTube
// embed snippet) is wider than a phone. A width fits when it is at most 320
// px (px, unitless, em/rem at 16 px) or relative (%, vw and its variants); an
// inline `max-width` that fits also counts. An empty or unreadable width
// does not. The inline style is read before the attribute, as the cascade
// does. Elements inside <noscript>, <template>, <noembed>, <noframes>, an
// <svg> or <math> are not rendered as such and never count.
bool MayExceedPhoneViewport(const net_instaweb::HtmlElement& element);

// True for images that cannot plausibly be the LCP element.  Today this is
// exactly the invisibility evidence above: a false "unlikely" merely skips
// an optimization hint, while promoting a beacon wastes the browser's
// high-priority fetch slot.
bool IsUnlikelyLcpImage(const net_instaweb::HtmlElement& element);

// True when the element carries a `data-*` attribute naming the source a lazy
// loader will fill in: any `data-*src*` (data-src, data-srcset, data-lazy-src,
// data-bg-src, ...), data-original or data-lazy. Attribute names only; the
// value is not read.
bool HasLazySourceAttribute(const net_instaweb::HtmlElement& element);

// True when `src` names a common stand-in image: its file name,
// without directory, query, fragment and extension, lower-cased and split on
// `-`, `_` and `.`, consists only of the tokens blank, placeholder, pixel,
// spacer, 1x1, transparent, loading, lazy (a trailing number allowed:
// `spacer2`), or a number or `<n>x<m>` size (`/blank.gif`,
// `/img/placeholder-300x200.png`, `/lazy_pixel.svg`). A name that only
// contains such a word (`/lazy-river.jpg`) does not match. The caller pairs
// this with HasLazySourceAttribute: a stand-in without a lazy data attribute
// is just an image named blank.
bool IsLazyStandInSrc(std::string_view src);

// An eligible image whose declared size is below this many CSS px^2 is small
// in the sense of IsSmallDeclaredImage: a 48x48 or 64x64 icon, a 96x96 badge,
// never a picture meant to be looked at, which is rarely under 100 px on both
// axes. At the narrowest phone viewport (kNarrowestPhoneViewportPx) such an
// image fills under a tenth of a 320x320 square, far from the largest paint.
// Strict: 100x100 (an avatar, a product thumbnail) is not small.
inline constexpr int kSmallImageAreaPx2 = 10000;

// True when the markup declares both a width and a height in px (inline
// `width:`/`height:` declarations first, as the cascade does, else the
// attributes; a plain integer, with or without `px`) and their product is
// under kSmallImageAreaPx2. A missing, relative or unreadable dimension means
// the size is unknown, and unknown is not small.
bool IsSmallDeclaredImage(const net_instaweb::HtmlElement& element);

// A third-party origin worth a preconnect hint.
struct PreconnectOrigin {
  std::string origin;
  // True when the resource that motivated this origin is fetched in CORS mode
  // (crossorigin attribute, ES module script, or a font preload).  Browsers
  // key connection reuse on the request mode, so a preconnect must warm the
  // SAME pool the resource will use: crossorigin for CORS-mode fetches, bare
  // for plain stylesheets/scripts/images.
  bool crossorigin = false;
};

// Result of scanning HTML
struct HtmlScanResult {
  bool success = false;
  std::string error_message;

  // Collected elements in document order: the elements a browser running
  // scripts renders. Not a <noscript>, <template>, <noembed> or <noframes>
  // element, nor anything inside one (see inert_elements).
  // Those still advance element_index, which keeps every index what it was
  // before they were left out. The cost: CriticalCssExtractor's "first N
  // elements in <body>" estimate counts them, so a large <noscript> early in
  // the body pushes real elements out of that estimate (the measured fold is
  // unaffected).
  std::vector<CollectedElement> elements;

  // The elements left out of `elements` for the rule above, each with the
  // position in `elements` it would have had. Kept only so that
  // TemplateDetector::HashStructure hashes the same tag tree it hashed when
  // they were collected: a changed template hash would send every template
  // with a <noscript> back to analysis at once.
  struct InertElement {
    size_t position = 0;
    int depth = 0;
    std::string tag_name;
  };
  std::vector<InertElement> inert_elements;

  // External stylesheet links (<link rel="stylesheet">), in document order.
  // Only sheets that apply to a client running scripts: a link inside
  // <noscript>, <template>, <noembed>, <noframes>, <svg> or <math> is not
  // collected, with one exception: the loadCSS pattern, where a
  // `<link rel=preload as=style onload=...>` outside those subtrees applies a
  // sheet by script and a <noscript> link declares the same sheet (matched by
  // resolved URL). That sheet is collected once, at the preload's position,
  // unless a link that applies already declares it.
  std::vector<StylesheetLink> stylesheets;

  // Combined inline CSS from the <style> tags that apply to a client running
  // scripts: not those inside <noscript>, <template>, <noembed>, <noframes> or
  // <math>. A <style> inside inline <svg> applies to the whole document and is
  // collected.
  std::string inline_css;

  // Every stylesheet source, in document order (see StylesheetSource).
  std::vector<StylesheetSource> stylesheet_sources;

  // LCP image candidate (empty src = no candidate found). Its element_index
  // is comparable with those in `elements`: an <img> inside <noscript> and
  // the like is never the candidate but advances the index like any other
  // inert element.
  LcpCandidate lcp_candidate;

  // Third-party origins found in the HTML (for preconnect hints).
  // Deduplicated, capped at 4, prioritized by resource type.  Each entry
  // records whether the motivating (first-seen) resource fetches in CORS
  // mode, so preconnect emitters warm the matching connection pool.
  std::vector<PreconnectOrigin> third_party_origins;

  // Raw URLs (href/src, unresolved) of subresources referenced with an
  // integrity attribute (SRI).  Optimized variants served at the same
  // URL would fail the browser's hash check, so these URLs must be
  // excluded from text-variant optimization.  Collected from <script src>
  // and any <link href> (stylesheet, preload, modulepreload).
  std::vector<std::string> integrity_pinned_urls;

  // Raw src attributes (unresolved) of <script src> elements, in document
  // order.  Feeds the analysis resource map (analysis_resource_map.cc).
  std::vector<std::string> script_srcs;

  // True when an author <noscript> holds something a render with script
  // execution disabled shows and a browser running scripts does not:
  // a <style>, a <meta http-equiv=refresh> (the JS-off render
  // navigates away), a stylesheet <link> whose sheet does not apply anyway
  // (declared by a link that applies, or the loadCSS twin of a script-loaded
  // preload), a visible element, or text. Not an element the markup declares
  // invisible (a 1x1 tracking pixel, the GTM snippet's hidden iframe; see
  // IsInvisibleElement), nor script, another meta or a non-stylesheet link. Not the
  // worker's own fallback copy, and not a <noscript> in <svg>, <math>,
  // <template>, <noembed> or <noframes>. Salts the validation binding
  // (ValidationBindingFor), so such a page is validated again once.
  bool noscript_affects_render = false;

  // True when the document carries an author <base href> — the analysis
  // pipeline must not inject a second base element.
  bool has_base_href = false;

  // The author <base href> value (raw attribute, possibly relative).  Per the
  // HTML spec only the FIRST base-with-href sets the document base URL, so
  // later ones are ignored.  Empty when has_base_href is false.
  std::string base_href;
};

// What a reference in a scanned document resolves against:
// the document's base URL is the page URL, or the first <base href>
// resolved against it when the scan saw one. The one rule for every consumer
// of a scan: the scanner's sheet and image identities, the combined-stylesheet
// gather and the CSS inliner (both sides of the critical-CSS validation), the
// stylesheet hints and the SRI pins all resolve through DocumentBaseOf and
// ResolveAgainstBase, so they name the same sheet.
struct DocumentBase {
  // The base URL. ResolvePath returns a root-relative <base href="/sub/"> as
  // it is, so this is hostless for such a base.
  std::string url;
  // What ResolvePath takes: `url` up to and including its last '/'.
  std::string dir;
  // The scheme of `url`, else of the page URL; empty when neither has one.
  std::string scheme;
  // The host a hostless reference fetches from, as the browser resolves it:
  // the base URL's own, else the page URL's (a root-relative <base href> has
  // none of its own). Lowercased. Empty only when neither has one.
  std::string host;
  // True when `host` is not the page's own (a cross-host or protocol-relative
  // <base href>); an explicit default port (":443" on https, ":80" on http)
  // does not make a host foreign. False when the page's own host is unknown.
  bool cross_host = false;
};
// `page_host` and `page_scheme` are what the caller knows about the page when
// `page_url` carries no host or scheme of its own: the worker's notification
// URL is the cache-normalized path, so its host is the Host header nginx put
// in notification.hostname and its scheme notification.scheme.
DocumentBase DocumentBaseOf(std::string_view page_url,
                            const HtmlScanResult& scan,
                            std::string_view page_host = {},
                            std::string_view page_scheme = {});

// `href` resolved against the document base, for fetching. A reference with a
// scheme (an absolute URL, a data: or javascript: href) is returned as it is.
// Otherwise ResolvePath on base.dir, then a protocol-relative result gets the
// document's scheme (with or without a <base>: "//cdn.example.com/c.css" is
// the CDN's "/c.css", where before it was looked up on the page host under a
// key nginx never stores), and under a cross-host base a hostless result gets
// the base's scheme and host, so the fetch goes to the <base> host as the
// browser's does. Any other hostless result stays hostless (the page's own
// root-relative path), and the caller fetches it from the page's own host as
// it always has.
std::string ResolveAgainstBase(const DocumentBase& base, std::string_view href);

// HTML Scanner that collects element information without modification.
//
// This scanner uses HtmlParse with a custom filter to collect elements
// as they are encountered. Unlike HtmlRewriter, it does NOT use
// HtmlWriterFilter and does NOT produce modified HTML output.
//
// Usage:
//   HtmlScanner scanner;
//   HtmlScanResult result = scanner.Scan("http://example.com/", html);
//   if (result.success) {
//     // Use result.elements, result.stylesheets, result.inline_css
//   }
class HtmlScanner {
 public:
  HtmlScanner();
  ~HtmlScanner() = default;

  // Scan HTML content and collect element information.
  // url: The URL of the page (used for base URL resolution)
  // html: The HTML content to scan
  // Returns the scan result with collected elements and CSS
  HtmlScanResult Scan(std::string_view url, std::string_view html);
};

}  // namespace pagespeed

#endif  // PAGESPEED_SRC_WORKER_HTML_SCANNER_H_
