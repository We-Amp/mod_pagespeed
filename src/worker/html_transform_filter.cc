// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

// PageSpeed 2.0 - HTML Transform Filter Implementation

#include "src/worker/html_transform_filter.h"

#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "lib/base/string_util.h"
#include "lib/cache/cache.h"
#include "lib/cache/headers_sidecar.h"
#include "lib/classify/alternate_metadata.h"
#include "lib/classify/capability_mask.h"
#include "lib/html/csp_inline_policy.h"
#include "lib/html/html_element.h"
#include "lib/html/html_keywords.h"
#include "lib/html/html_name.h"
#include "lib/html/html_node.h"
#include "lib/html/html_parse.h"
#include "lib/image/image_dimensions.h"
#include "src/worker/async_css_loader.h"
#include "src/worker/cascade_layer_order.h"
#include "src/worker/html_css_injector.h"

namespace pagespeed {

namespace {

// Decimal digits allowed in a width/height pixel dimension, and the largest
// value that many digits can express.  Both the author-input validator and
// the header-inference path are held to this same limit.
//
// Tradeoff, accepted deliberately: a genuine image wider than this (a
// 150000x80 panorama, say) loses its inferred dimensions entirely.  That is
// the pre-existing no-dimensions behavior for that image, not corruption, and
// it is the price of the self-consistency rule -- we never emit via inference
// a value we would have rejected as author input.
constexpr size_t kMaxPixelDimensionDigits = 5;

// Derived, not asserted: 10^digits - 1 (== 99999).  Computing it from the
// digit count makes an inconsistent pair unrepresentable.
constexpr int kMaxPixelDimension = [] {
  int v = 1;
  for (size_t i = 0; i < kMaxPixelDimensionDigits; ++i) v *= 10;
  return v - 1;
}();

// HTML whitespace, for splitting a rel token list.
bool IsHtmlSpace(char c) {
  return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f';
}

// True when `rel` (a space-separated token list) has `token`, matched per
// token and case-insensitively: `alternate stylesheet` names a stylesheet, a
// `rel="preload"` whose href merely contains "stylesheet" does not. The same
// rule as the validator's LinkTagHasRelToken.
bool RelHasToken(std::string_view rel, std::string_view token) {
  size_t i = 0;
  while (i < rel.size()) {
    while (i < rel.size() && IsHtmlSpace(rel[i])) ++i;
    size_t start = i;
    while (i < rel.size() && !IsHtmlSpace(rel[i])) ++i;
    if (i > start &&
        net_instaweb::StringCaseEqual(rel.substr(start, i - start), token)) {
      return true;
    }
  }
  return false;
}

// An author's loadCSS preload: `<link rel=preload as=style onload=...>`, whose
// handler turns this element into the stylesheet, so the browser applies the
// sheet HERE (the scanner counts it at this position). The
// worker's own preload hints carry data-pagespeed-hint and are not sheets.
// The same rule as the validator's LinkTagIsLoadCssPreload.
bool IsLoadCssPreload(const net_instaweb::HtmlElement* element) {
  using net_instaweb::HtmlName;
  const char* rel = element->AttributeValue(HtmlName::kRel);
  const char* as = element->AttributeValue(HtmlName::kAs);
  std::string_view as_value = as != nullptr ? as : "";
  while (!as_value.empty() && IsHtmlSpace(as_value.front())) {
    as_value.remove_prefix(1);
  }
  while (!as_value.empty() && IsHtmlSpace(as_value.back())) {
    as_value.remove_suffix(1);
  }
  return rel != nullptr && RelHasToken(rel, "preload") &&
         net_instaweb::StringCaseEqual(as_value, "style") &&
         element->FindAttribute("onload") != nullptr &&
         element->FindAttribute("data-pagespeed-hint") == nullptr;
}

// True when `element` sits inside a subtree a browser running scripts never
// creates elements from: <noscript> (raw text whenever scripting is on, which
// is every client the transforms are for), <noembed>/<noframes> (raw text in
// current browsers; the lexer parses all three as markup, see
// kSometimesLiteralTags in html_lexer.cc, and asks filters not to insert into
// them) and <template> (inert). HtmlScanner's `inert` rule names the same
// four.
bool InsideInertSubtree(const net_instaweb::HtmlElement* element) {
  using net_instaweb::HtmlName;
  for (const net_instaweb::HtmlElement* p = element->parent(); p != nullptr;
       p = p->parent()) {
    switch (p->keyword()) {
      case HtmlName::kNoscript:
      case HtmlName::kNoembed:
      case HtmlName::kNoframes:
      case HtmlName::kTemplate:
        return true;
      default:
        break;
    }
  }
  return false;
}

// True when `element` opens a subtree where an inserted <style> is not a
// document stylesheet for every client: the inert subtrees above, foreign
// content (<svg>/<math>, where <style> is not a raw-text element), <select>
// (whose parser ignores a <style> start tag, so the CSS would be text) and
// <plaintext> (everything after it is text).
bool IsNonDocumentSubtreeRoot(const net_instaweb::HtmlElement* element) {
  using net_instaweb::HtmlName;
  switch (element->keyword()) {
    case HtmlName::kNoscript:
    case HtmlName::kNoembed:
    case HtmlName::kNoframes:
    case HtmlName::kTemplate:
    case HtmlName::kSelect:
      return true;
    default:
      break;
  }
  std::string_view name(element->name_str().data(), element->name_str().size());
  return net_instaweb::StringCaseEqual(name, "svg") ||
         net_instaweb::StringCaseEqual(name, "math") ||
         net_instaweb::StringCaseEqual(name, "plaintext");
}

// True when `element` sits inside such a subtree. A stylesheet source in there
// cannot be the critical block's anchor. The validator's
// StripStylesheetSources applies the same list (kNonDocumentSubtrees,
// critical_css_validator.cc); change both together.
bool InsideNonDocumentSubtree(const net_instaweb::HtmlElement* element) {
  for (const net_instaweb::HtmlElement* p = element->parent(); p != nullptr;
       p = p->parent()) {
    if (IsNonDocumentSubtreeRoot(p)) return true;
  }
  return false;
}

}  // namespace

HtmlTransformFilter::HtmlTransformFilter(
    net_instaweb::HtmlParse* parser, const HtmlTransformConfig& config,
    std::string_view critical_css, PageSpeedCache* cache,
    std::string_view hostname, std::string_view scheme,
    LcpCandidate lcp_candidate,
    const std::vector<PreconnectOrigin>& preconnect_origins,
    const std::vector<std::string>& speculation_urls,
    const std::vector<std::string>& defer_scripts,
    const std::vector<std::string>& font_urls)
    : parser_(parser),
      config_(config),
      critical_css_(critical_css),
      cache_(cache),
      hostname_(hostname),
      scheme_(scheme),
      lcp_candidate_(std::move(lcp_candidate)),
      preconnect_origins_(preconnect_origins),
      speculation_urls_(speculation_urls),
      defer_scripts_(defer_scripts),
      font_urls_(font_urls) {}

void HtmlTransformFilter::StartDocument() {
  modified_ = false;
  critical_css_injected_ = false;
  critical_css_context_refused_ = false;
  in_select_ = false;
  select_sticky_ = false;
  plaintext_seen_ = false;
  frameset_seen_ = false;
  injected_critical_style_ = nullptr;
  critical_css_anchor_ = nullptr;
  in_head_ = false;
  in_body_ = false;
  fallback_priority_applied_ = false;
  picture_has_source_srcset_ = false;
  body_img_count_ = 0;
  body_iframe_count_ = 0;
  lcp_preload_injected_ = false;
  preconnect_injected_ = false;
  font_preload_injected_ = false;
  speculation_rules_injected_ = false;
  async_css_loader_injected_ = false;
  deferred_css_links_.clear();
  async_css_injected_nodes_.clear();
  meta_csps_.clear();
}

void HtmlTransformFilter::StartElement(net_instaweb::HtmlElement* element) {
  using net_instaweb::HtmlName;

  HtmlName::Keyword keyword = element->keyword();

  // Contexts in which a browser would not read an inserted <style> as a
  // style element; InjectCriticalCss refuses while (or after) they apply.
  // In select mode, any start tag but <option>, <optgroup>, <hr> or
  // <script> is one where the parser's tree and a browser's can part (a
  // <template>, foreign content, a nested <select>, a <div> the parser
  // nests): stay in select for the rest of the document.
  if (in_select_ && keyword != HtmlName::kOption &&
      keyword != HtmlName::kOptgroup && keyword != HtmlName::kHr &&
      keyword != HtmlName::kScript) {
    select_sticky_ = true;
  }
  if (keyword == HtmlName::kSelect) in_select_ = true;
  {
    const std::string_view name(element->name_str().data(),
                                element->name_str().size());
    if (net_instaweb::StringCaseEqual(name, "plaintext")) {
      plaintext_seen_ = true;
    } else if (net_instaweb::StringCaseEqual(name, "frameset")) {
      frameset_seen_ = true;
      // A frameset document gets no block at all: take back one injected
      // at </head> (the whole document is one flush window, so the node is
      // still rewritable); EndDocument then reverts the deferral.
      if (critical_css_injected_ && injected_critical_style_ != nullptr &&
          parser_->IsRewritable(injected_critical_style_)) {
        parser_->DeleteNode(injected_critical_style_);
        injected_critical_style_ = nullptr;
        critical_css_injected_ = false;
        critical_css_context_refused_ = true;
      }
    }
  }

  if (keyword == HtmlName::kHead) {
    in_head_ = true;
  } else if (keyword == HtmlName::kBody) {
    in_body_ = true;
  }

  // A <picture>'s <source> siblings come before its <img>; remember whether
  // one names a real source, so a src-less <img> in there is not taken for a
  // lazy-load placeholder (ApplyLazyLoad).
  if (keyword == HtmlName::kPicture) {
    picture_has_source_srcset_ = false;
  } else if (keyword == HtmlName::kSource && element->parent() != nullptr &&
             element->parent()->keyword() == HtmlName::kPicture) {
    const char* srcset = element->AttributeValue(HtmlName::kSrcset);
    if (srcset != nullptr && srcset[0] != '\0') {
      picture_has_source_srcset_ = true;
    }
  }

  // Collect an enforcing <meta http-equiv="Content-Security-Policy"> so the
  // inline-injection sites can gate on it. Meta always precedes </head>/</body>.
  if (keyword == HtmlName::kMeta) {
    CollectMetaCsp(element);
  }

  // Remove previously-injected critical CSS to avoid duplication
  // on revalidation passes (the worker re-processes its own output).
  if (keyword == HtmlName::kStyle &&
      (element->FindAttribute("data-pagespeed-critical") != nullptr)) {
    parser_->DeleteNode(element);
    modified_ = true;
    return;
  }

  // Remove previously-injected <link> and <script> elements marked with
  // data-pagespeed-hint to avoid duplication on revalidation passes.
  if ((keyword == HtmlName::kLink || keyword == HtmlName::kScript) &&
      (element->FindAttribute("data-pagespeed-hint") != nullptr)) {
    parser_->DeleteNode(element);
    modified_ = true;
    return;
  }

  // Remove the previously-injected <noscript> async-CSS fallback (and its
  // child <link>) to avoid duplication on revalidation passes.
  if (keyword == HtmlName::kNoscript &&
      (element->FindAttribute("data-pagespeed-async-fallback") != nullptr)) {
    parser_->DeleteNode(element);
    modified_ = true;
    return;
  }

  // Remove the previously-injected async-CSS loader <script> on revalidation;
  // it is re-injected below if any stylesheet is deferred again.
  if (keyword == HtmlName::kScript &&
      (element->FindAttribute("data-pagespeed-async-loader") != nullptr)) {
    parser_->DeleteNode(element);
    modified_ = true;
    return;
  }

  // Revalidation cleanup for async CSS: put the link back to the author's
  // rel="stylesheet" + media and strip our markers so ApplyAsyncCss can
  // re-apply cleanly. Deliberately NOT gated on config_.enable_async_css —
  // turning the feature off must hand the customer their original markup back,
  // not leave a preload nothing consumes.
  if (keyword == HtmlName::kLink &&
      (element->FindAttribute("data-pagespeed-async") != nullptr)) {
    RemoveAsyncCssMarkers(element);
  }

  // After the revalidation cleanup above (a deferred link is a stylesheet
  // again by now, and our own previous block is gone) and before ApplyAsyncCss
  // rewrites rel: does this element move the critical block's insertion point?
  TrackCriticalCssAnchor(element);

  // Apply async CSS loading to <link rel="stylesheet">. Never re-process the
  // <noscript> fallback link we inject ourselves.
  //
  // Gate on meta CSP too (MetaCspAllowsInlineStyle): async conversion only pays
  // off when the inlined critical CSS renders above-the-fold while the full
  // sheet loads non-render-blocking. Under a restrictive style CSP the inline
  // critical CSS is suppressed (see InjectCriticalCss), so deferring the sheet
  // here would leave the page UNSTYLED (FOUC) until the loader flips the rel.
  // Skipping the conversion keeps the sheet render-blocking = safe baseline,
  // consistent with the critical-CSS gate.
  //
  // Ordering: this runs at StartElement, so a <link rel=stylesheet> appearing
  // in source order BEFORE the CSP <meta> is converted without seeing that
  // policy — yet the meta still governs the inline <style> whose injection is
  // decided at </head>.
  // The check here is therefore an early-out, not the guarantee; RevertAsyncCss
  // (EndDocument) undoes any conversion whose inline block turned out not to
  // ship.
  //
  // Never a link inside <noscript> and the like: it does not
  // load for a client running scripts, so there is nothing to defer, the
  // scanner does not count it as a source the block was measured against,
  // and converting it would hand no-JS clients a preload plus a nested
  // <noscript> copy instead of the author's plain stylesheet.
  if (config_.enable_async_css && !critical_css_.empty() &&
      MetaCspAllowsInlineStyle() && keyword == HtmlName::kLink &&
      element->FindAttribute("data-pagespeed-async-fallback") == nullptr &&
      !InSelect() && !InsideNonDocumentSubtree(element)) {
    ApplyAsyncCss(element);
  }

  // Revalidation cleanup for script deferral: strip marker and defer.
  if (keyword == HtmlName::kScript &&
      (element->FindAttribute("data-pagespeed-defer") != nullptr)) {
    element->DeleteAttribute("data-pagespeed-defer");
    element->DeleteAttribute(HtmlName::kDefer);
    modified_ = true;
  }

  // Apply script deferral to <script> elements.
  if (config_.enable_script_deferral && keyword == HtmlName::kScript) {
    ApplyScriptDeferral(element);
  }

  // Apply lazy loading to <img> and <iframe>.
  if (config_.enable_lazy_load) {
    if (keyword == HtmlName::kImg || keyword == HtmlName::kIframe) {
      ApplyLazyLoad(element);
    }
  }

  // Apply image dimensions to <img>.
  if (config_.enable_image_dimensions && keyword == HtmlName::kImg) {
    ApplyImageDimensions(element);
  }
}

void HtmlTransformFilter::EndElement(net_instaweb::HtmlElement* element) {
  using net_instaweb::HtmlName;

  HtmlName::Keyword keyword = element->keyword();

  // Only a </select> in the source ends a browser's "in select" mode; a
  // select the parser closes for its tree (an ancestor's end tag, </head>,
  // the end of input) leaves the browser reading what follows inside it.
  // The parser's explicit </select> is the browser's only when it closed
  // nothing but <option> and <optgroup>: the elements it skips end first,
  // without an end tag of their own. Any other element ending that way while
  // in select (a <div>, a <template>) keeps the document in select for good.
  if (in_select_ && keyword != HtmlName::kSelect &&
      keyword != HtmlName::kOption && keyword != HtmlName::kOptgroup &&
      element->style() != net_instaweb::HtmlElement::EXPLICIT_CLOSE &&
      element->style() != net_instaweb::HtmlElement::IMPLICIT_CLOSE &&
      element->style() != net_instaweb::HtmlElement::BRIEF_CLOSE) {
    select_sticky_ = true;
  }
  if (keyword == HtmlName::kSelect &&
      element->style() == net_instaweb::HtmlElement::EXPLICIT_CLOSE &&
      !select_sticky_) {
    bool select_above = false;
    for (const net_instaweb::HtmlElement* p = element->parent(); p != nullptr;
         p = p->parent()) {
      if (p->keyword() == HtmlName::kSelect) select_above = true;
    }
    if (!select_above) in_select_ = false;
  }

  // Past the parser's page limit the rest of the page is passed through as
  // text, and an element still open there ends with the input: its real end
  // tag is in that text. There is no "before </head>" or "before </body>" to
  // inject at then, and anything inserted would follow the end of the page.
  const bool end_tag_passed_through =
      parser_->passing_through() &&
      element->style() == net_instaweb::HtmlElement::UNCLOSED;

  if (keyword == HtmlName::kHead && end_tag_passed_through) {
    in_head_ = false;
  } else if (keyword == HtmlName::kBody && end_tag_passed_through) {
    in_body_ = false;
  } else if (keyword == HtmlName::kHead) {
    if (config_.enable_font_preload && !font_preload_injected_) {
      InjectFontPreload(element);
    }
    if (config_.enable_preconnect_injection && !preconnect_injected_) {
      InjectPreconnectLinks(element);
    }
    if (config_.enable_lcp_preload && !lcp_preload_injected_) {
      InjectLcpPreload(element);
    }
    if (config_.enable_critical_css && !critical_css_injected_) {
      InjectCriticalCss(element);
    }
    in_head_ = false;
  } else if (keyword == HtmlName::kBody) {
    // Inject speculation rules before </body>.
    if (config_.enable_speculation_rules && !speculation_rules_injected_) {
      InjectSpeculationRules(element);
    }
    // Fallback: inject before </body> if not injected yet.
    if (config_.enable_critical_css && !critical_css_injected_ &&
        !critical_css_.empty()) {
      InjectCriticalCss(element);
    }
    in_body_ = false;
  }
}

void HtmlTransformFilter::EndDocument() {
  // Whether the inline block was REFUSED (meta CSP, markup guard) or simply
  // never reached (a fragment with no </head> and no </body>) makes no
  // difference to the visitor: a deferred stylesheet with no inlined block to
  // paint meanwhile is a guaranteed flash of unstyled content. Gate on the
  // outcome, not the reason.
  if (!critical_css_injected_) {
    RevertAsyncCss();
  }
}

void HtmlTransformFilter::InjectCriticalCss(
    net_instaweb::HtmlElement* element) {
  if (critical_css_.empty()) return;

  // Only into a document known to be text/html (see set_document_is_text_html):
  // in XHTML the block's text is parsed as XML, where `<` opens markup and `&`
  // an entity reference (CSS nesting uses `&`), so the guard below says
  // nothing about it there. A refused block leaves the stylesheets
  // render-blocking (EndDocument reverts any deferral).
  if (!document_is_text_html_) return;
  if (critical_css_context_refused_) return;

  // Gate on the page's CSP (header policies from the headers sidecar and
  // <meta> ones): if inline <style> would be dropped, do not inject. The SAME
  // policies also gate ApplyAsyncCss in
  // StartElement, so the origin stylesheet is NOT converted to async — it stays
  // render-blocking (the safe baseline, no FOUC), instead of substituting a
  // dead inline copy for a deferred sheet.
  if (!MetaCspAllowsInlineStyle()) return;

  // Strip null bytes first (before XSS check to prevent bypass).
  std::string sanitized;
  sanitized.reserve(critical_css_.size());
  for (char c : critical_css_) {
    if (c != '\0') sanitized.push_back(c);
  }

  // Create <style data-pagespeed-critical>...</style>. It goes BEFORE the
  // first stylesheet source seen (see the header: the block must lose every
  // same-specificity tie against the sheets it duplicates, which source order
  // decides), else before the end tag of <head> (or <body> as fallback) — a
  // source that only appears later in the document still follows it.
  //
  // The anchor was queued earlier in this pass. Every entry point drives the
  // filter with the whole document as one flush window, so it is rewritable;
  // the check is the same guard RevertAsyncCss applies, and a node that has
  // left the window is not touched (InsertNodeBeforeNode Checks on it).
  //
  // A block that names a cascade layer goes first only with the page's layer
  // order in front of it, when that order is proven; otherwise it keeps the old
  // placement before the end tag, where the page's own sheets mention every
  // layer first (DecideCriticalCssLayerPlacement, cascade_layer_order.h). The
  // statement goes in front wherever the block lands ahead of the sheets,
  // including before the end tag when every sheet is in <body>.
  const CriticalCssLayerPlacement layers =
      DecideCriticalCssLayerPlacement(sanitized, layer_order_);
  const bool before_anchor = !layers.keep_fallback &&
                             critical_css_anchor_ != nullptr &&
                             parser_->IsRewritable(critical_css_anchor_);

  // Markup safety: the block's text must hold no `<` that could open a tag, a
  // closing tag, a comment or a processing instruction (`<` then an ASCII
  // letter, `/`, `!` or `?`), so it is markup-free even where a browser would
  // not read it as a style's raw text (a tokenizer state the parser does not
  // model). Such a `<` inside a CSS string or comment, as in Tailwind CSS v4's
  // `syntax: "<color>"`, is written as the CSS escape `\3c `, which means the
  // same to CSS (EscapeLessThanForStyleElement). One outside them is not
  // valid CSS: refuse the block. A bare `<`, as in a range media query, stays
  // as it is. Decided on the text as written, layer statement included.
  std::string block_text = layers.prefix;
  block_text.append(sanitized);
  if (CssOpensMarkupOutsideStrings(block_text)) return;  // Abort injection.
  block_text = EscapeLessThanForStyleElement(block_text);

  // In an HTML <style> element the raw text ends at the first `</style`
  // (ASCII case-insensitive) followed by whitespace, `/` or `>`. After the
  // two steps above the text holds no `</` at all; this names the end tag on
  // its own, as defense in depth. Only sound for text/html; see
  // document_is_text_html_.
  constexpr std::string_view kStyleEndTag = "</style";
  const std::string_view css_view(block_text);
  for (size_t i = 0; i + kStyleEndTag.size() <= css_view.size(); ++i) {
    if (css_view[i] == '<' &&
        net_instaweb::StringCaseEqual(css_view.substr(i, kStyleEndTag.size()),
                                      kStyleEndTag)) {
      return;  // Abort injection.
    }
  }

  // The block must land where an inserted <style> is a document stylesheet:
  // never inside <noscript>/<noembed>/<noframes>/<template> or foreign content
  // (<svg>/<math>), where it would not apply, or not as raw text. Before the
  // anchor it becomes the anchor's sibling; before the current end tag, the
  // last child of `element`. TrackCriticalCssAnchor already skips sources in
  // those subtrees, and a <head> or <body> start tag ends foreign content
  // (html_lexer.cc), so this refuses a document that nests <head> or <body>
  // in <noscript>, <noembed>, <noframes>, <template> or <select>; the rest is
  // defense in depth. The refusal is final for the document (no second try at
  // </body>).
  // So does a document that is "in select" (a <select> start tag not yet
  // matched by an explicit </select>), or has had a <plaintext> or a
  // <frameset>, wherever the block would go (conservative: before an earlier
  // anchor it would be safe).
  if (InSelect() || plaintext_seen_ || frameset_seen_ ||
      (before_anchor ? InsideNonDocumentSubtree(critical_css_anchor_)
                     : (IsNonDocumentSubtreeRoot(element) ||
                        InsideNonDocumentSubtree(element)))) {
    critical_css_context_refused_ = true;
    return;  // Refuse; EndDocument reverts any deferral.
  }

  auto* style_element =
      parser_->NewElement(nullptr, net_instaweb::HtmlName::kStyle);
  parser_->AddAttribute(style_element, "data-pagespeed-critical", "");
  if (before_anchor) {
    parser_->InsertNodeBeforeNode(critical_css_anchor_, style_element);
  } else {
    parser_->InsertNodeBeforeCurrent(style_element);
  }
  injected_critical_style_ = style_element;
  auto* css_text = parser_->NewCharactersNode(style_element, block_text);
  parser_->AppendChild(style_element, css_text);

  critical_css_injected_ = true;
  modified_ = true;
}

void HtmlTransformFilter::TrackCriticalCssAnchor(
    net_instaweb::HtmlElement* element) {
  using net_instaweb::HtmlName;

  // The injection point is decided at </head> (or </body>); anything after
  // that cannot move it.
  if (critical_css_injected_) return;

  const HtmlName::Keyword keyword = element->keyword();

  // Head prelude: the block must not move ahead of these, so a stylesheet
  // source seen before one of them stops being the anchor. Known, accepted
  // edge: a sheet that precedes the prelude still precedes the block, so the
  // block wins its ties against THAT sheet as it did before the block-first
  // placement (a page with
  // a stylesheet ahead of its charset declaration is rare, and every fix
  // would move the block ahead of the prelude).
  if (keyword == HtmlName::kBase) {
    critical_css_anchor_ = nullptr;
    return;
  }
  if (keyword == HtmlName::kMeta) {
    if (element->FindAttribute(HtmlName::kCharset) != nullptr) {
      critical_css_anchor_ = nullptr;
      return;
    }
    const char* http_equiv = element->AttributeValue(HtmlName::kHttpEquiv);
    if (http_equiv != nullptr &&
        (net_instaweb::StringCaseEqual(http_equiv, "Content-Type") ||
         net_instaweb::StringCaseEqual(http_equiv,
                                       "Content-Security-Policy"))) {
      critical_css_anchor_ = nullptr;
    }
    return;
  }

  if (critical_css_anchor_ != nullptr) return;  // first source wins

  // Stylesheet sources: any <style> (our own previous block was deleted before
  // this runs, so it cannot anchor its replacement), a <link> whose rel token
  // list names a stylesheet (a deferred link has been put back to
  // rel="stylesheet" by the time this runs), and an author's loadCSS preload,
  // which a browser running scripts applies as a sheet at its own position.
  // Never one inside <noscript> and the like — which also rules
  // out the async fallback copy we inject.
  const bool is_source =
      keyword == HtmlName::kStyle ||
      (keyword == HtmlName::kLink &&
       element->FindAttribute("data-pagespeed-async-fallback") == nullptr &&
       ((element->AttributeValue(HtmlName::kRel) != nullptr &&
         RelHasToken(element->AttributeValue(HtmlName::kRel), "stylesheet")) ||
        IsLoadCssPreload(element)));
  if (is_source && !InSelect() && !InsideNonDocumentSubtree(element)) {
    critical_css_anchor_ = element;
  }
}

void HtmlTransformFilter::ApplyLazyLoad(net_instaweb::HtmlElement* element) {
  using net_instaweb::HtmlName;

  // Skip if element already has a "loading" attribute.
  if (element->FindAttribute("loading") != nullptr) return;

  // An <img> or <iframe> inside <noscript>, <template>, <noembed> or
  // <noframes> does not exist for a browser running scripts
  // (the lazy-load fallback `<noscript><img src=hero.jpg></noscript>`), so it
  // gets no transform and consumes no above-fold slot: promoting it would
  // spend the fetchpriority fallback on an element the page never creates,
  // and counting it would push a real image out of the window. For a client
  // without scripts it is the author's fallback, left as written.
  if (InsideInertSubtree(element)) return;

  bool is_img = (element->keyword() == HtmlName::kImg);

  if (is_img && in_body_) {
    ++body_img_count_;  // Count ALL body images.

    // An implausible-LCP image (1x1 beacon, hidden element) gets NO
    // transform at any position: promotion would waste the browser's
    // high-priority fetch slot, and loading="lazy" on an element with no
    // layout box (display:none beacons) makes browsers skip the load
    // entirely — silently breaking the analytics the beacon exists for.
    if (IsUnlikelyLcpImage(*element)) return;

    const char* src = element->AttributeValue(HtmlName::kSrc);
    std::string_view src_view = (src != nullptr) ? src : "";
    // An <img> with no usable source is a lazy-load placeholder the author's
    // loader fills in (`<img data-src=hero.jpg class=lazy>`). It is never
    // promoted: fetchpriority="high" on an element with no fetch
    // of its own hints nothing, and the slot it took kept the fallback from
    // the first image that does load. Inside the above-fold window it is
    // left as the author wrote it (it may well be the hero); past the window
    // it is lazy-loaded like any other image. An <img> that loads from its
    // srcset without a src, or from a <source srcset> sibling in <picture>,
    // has a source and is not a placeholder. A src that names a stand-in
    // (`/blank.gif`, IsLazyStandInSrc) next to a lazy data attribute is no
    // source either: the fetch it makes is not the picture.
    const char* srcset = element->AttributeValue(HtmlName::kSrcset);
    const bool has_srcset = srcset != nullptr && srcset[0] != '\0';
    const bool in_picture = element->parent() != nullptr &&
                            element->parent()->keyword() == HtmlName::kPicture;
    const bool stand_in_src =
        !src_view.empty() && !src_view.starts_with("data:") &&
        HasLazySourceAttribute(*element) && IsLazyStandInSrc(src_view);
    const bool placeholder =
        (src_view.empty() || src_view.starts_with("data:") || stand_in_src) &&
        !has_srcset && !(in_picture && picture_has_source_srcset_);
    const bool has_candidate =
        !lcp_candidate_.src.empty() && IsAllowedPreloadUrl(lcp_candidate_.src);

    // If we have a valid LCP candidate, match by src URL.
    if (has_candidate) {
      if (src_view == lcp_candidate_.src) {
        // This is the LCP image: fetchpriority="high", no lazy.
        if (element->FindAttribute("fetchpriority") == nullptr) {
          parser_->AddAttribute(element, "fetchpriority", "high");
          modified_ = true;
        }
        return;
      }
    } else if (!placeholder && !lcp_candidate_.script_loaded_hero &&
               !fallback_priority_applied_ &&
               body_img_count_ <= kAboveFoldImgWindow) {
      // No valid LCP candidate: the first plausible body img gets
      // fetchpriority (implausible images already returned above), and if
      // none qualifies within the above-fold window, no image is promoted
      // (fail-safe). With a script-loaded hero the scanner has
      // decided that nothing on the page is right to promote: the hero's
      // bytes are unknown and the next image is probably below the fold.
      if (element->FindAttribute("fetchpriority") == nullptr) {
        parser_->AddAttribute(element, "fetchpriority", "high");
        modified_ = true;
      }
      fallback_priority_applied_ = true;
      return;
    }

    // Guard: when an LCP candidate is set, don't lazy-load the first
    // few body images (likely above-fold) in case the heuristic picked
    // the wrong image. The same goes for a script-loaded hero, which is a
    // candidate the worker cannot name, and for a placeholder in the window.
    // Without an LCP candidate, the first-plausible-img heuristic already
    // protected one image above.
    if ((has_candidate || lcp_candidate_.script_loaded_hero || placeholder) &&
        body_img_count_ <= kAboveFoldImgWindow) {
      return;
    }
  }

  // Above-fold guard for iframes, mirroring the img window: a top-of-page
  // embed (video player etc.) is usually the FIRST iframe, and
  // lazy-loading it delays the page's main content.
  if (!is_img && in_body_) {
    // An invisible iframe (0x0 tracking frame, hidden, display:none) gets NO
    // transform at any position, for the same reason as invisible images:
    // it has no layout box, so loading="lazy" makes browsers skip the load
    // entirely — silently breaking the no-JS tracking the frame exists for.
    // It also must not count toward the above-fold window: that exemption
    // slot is for VISIBLE viewport embeds, and letting a hidden tracking
    // frame consume it would push the real hero embed into lazy-loading.
    // (Invisible imgs DO still count toward their window: with three img
    // slots a beacon rarely displaces the real candidates, while this
    // single iframe slot would be lost to the first tracking frame.)
    if (IsInvisibleElement(*element)) return;
    ++body_iframe_count_;
    if (body_iframe_count_ <= kAboveFoldIframeWindow) return;
  }

  // All other <img> and <iframe> get loading="lazy".
  parser_->AddAttribute(element, "loading", "lazy");
  // A lazy image is below the fold: decode it off the main thread so a late
  // scroll-in does not stall rendering. An author's decoding attribute wins.
  if (is_img && element->FindAttribute("decoding") == nullptr) {
    parser_->AddAttribute(element, "decoding", "async");
  }
  modified_ = true;
}

void HtmlTransformFilter::InjectFontPreload(
    net_instaweb::HtmlElement* /*element*/) {
  if (font_urls_.empty()) return;

  for (const auto& url : font_urls_) {
    if (!IsAllowedPreloadUrl(url)) continue;

    auto* link_element =
        parser_->NewElement(nullptr, net_instaweb::HtmlName::kLink);
    parser_->AddAttribute(link_element, net_instaweb::HtmlName::kRel,
                          "preload");
    parser_->AddAttribute(link_element, "as", "font");
    parser_->AddAttribute(link_element, net_instaweb::HtmlName::kType,
                          "font/woff2");
    parser_->AddAttribute(link_element, "crossorigin", "");
    parser_->AddAttribute(link_element, net_instaweb::HtmlName::kHref, url);
    parser_->AddAttribute(link_element, "data-pagespeed-hint", "");
    parser_->InsertNodeBeforeCurrent(link_element);
    modified_ = true;
  }
  font_preload_injected_ = true;
}

void HtmlTransformFilter::InjectPreconnectLinks(
    net_instaweb::HtmlElement* /*element*/) {
  if (preconnect_origins_.empty()) return;

  for (const auto& entry : preconnect_origins_) {
    if (!IsAllowedPreloadUrl(entry.origin)) continue;

    auto* link_element =
        parser_->NewElement(nullptr, net_instaweb::HtmlName::kLink);
    parser_->AddAttribute(link_element, net_instaweb::HtmlName::kRel,
                          "preconnect");
    parser_->AddAttribute(link_element, net_instaweb::HtmlName::kHref,
                          entry.origin);
    // Warm the pool the motivating resource will actually use: browsers key
    // connection reuse on the request mode, so crossorigin belongs on the
    // preconnect exactly when the resource fetches in CORS mode (fonts,
    // crossorigin scripts, ES modules).  Plain stylesheets/scripts/images
    // use the no-cors pool — a crossorigin preconnect would warm a
    // connection they never touch.
    if (entry.crossorigin) {
      parser_->AddAttribute(link_element, "crossorigin", "");
    }
    parser_->AddAttribute(link_element, "data-pagespeed-hint", "");
    parser_->InsertNodeBeforeCurrent(link_element);
    modified_ = true;
  }
  preconnect_injected_ = true;
}

void HtmlTransformFilter::InjectSpeculationRules(
    net_instaweb::HtmlElement* /*element*/) {
  if (speculation_urls_.empty()) return;

  // Gate on meta-delivered CSP: an inline <script type="speculationrules">
  // is dropped by the browser unless script-src permits inline, so skip
  // injection when the page's CSP would neutralize it.
  if (!MetaCspAllowsInlineScript()) return;

  // Build JSON with proper escaping (M4 security finding).
  // Format: {"prefetch":[{"source":"list","urls":["url1","url2",...]}]}
  std::string json = R"({"prefetch":[{"source":"list","urls":[)";
  bool first = true;
  for (const auto& url : speculation_urls_) {
    if (!IsAllowedPreloadUrl(url)) continue;
    if (!first) json += ',';
    first = false;
    json += '"';
    json += net_instaweb::JsonEscapeHtmlSafe(url);
    json += '"';
  }

  // If all URLs were filtered out, don't inject.
  if (first) return;

  json += "]}]}";

  // Create <script type="speculationrules">JSON</script> and insert
  // before </body>.
  auto* script_element =
      parser_->NewElement(nullptr, net_instaweb::HtmlName::kScript);
  parser_->AddAttribute(script_element, net_instaweb::HtmlName::kType,
                        "speculationrules");
  parser_->AddAttribute(script_element, "data-pagespeed-hint", "");
  parser_->InsertNodeBeforeCurrent(script_element);

  auto* json_text = parser_->NewCharactersNode(script_element, json);
  parser_->AppendChild(script_element, json_text);

  speculation_rules_injected_ = true;
  modified_ = true;
}

void HtmlTransformFilter::InjectLcpPreload(
    net_instaweb::HtmlElement* /*element*/) {
  if (lcp_candidate_.src.empty()) return;
  if (!IsAllowedPreloadUrl(lcp_candidate_.src)) return;
  // Inside <picture> a <source> sibling may win source selection, so
  // preloading the img's src risks downloading an image the browser never
  // uses.  Skip the preload; the fetchpriority="high" applied to the <img>
  // element itself (ApplyLazyLoad) stays correct whichever source wins.
  if (lcp_candidate_.in_picture) return;

  // Create <link rel="preload" as="image" href="..." fetchpriority="high">
  auto* link_element =
      parser_->NewElement(nullptr, net_instaweb::HtmlName::kLink);
  parser_->AddAttribute(link_element, net_instaweb::HtmlName::kRel, "preload");
  parser_->AddAttribute(link_element, "as", "image");
  parser_->AddAttribute(link_element, net_instaweb::HtmlName::kHref,
                        lcp_candidate_.src);
  parser_->AddAttribute(link_element, "fetchpriority", "high");
  parser_->AddAttribute(link_element, "data-pagespeed-hint", "");

  // Add srcset if available (validate each URL in the srcset).
  if (!lcp_candidate_.srcset.empty()) {
    // Validate srcset URLs: each comma-separated entry starts with a URL.
    bool srcset_safe = true;
    std::string_view srcset_view = lcp_candidate_.srcset;
    size_t pos = 0;
    while (pos < srcset_view.size()) {
      // Skip leading whitespace.
      while (pos < srcset_view.size() && srcset_view[pos] == ' ') ++pos;
      if (pos >= srcset_view.size()) break;
      // Find the end of this entry (next comma or end of string).
      size_t comma = srcset_view.find(',', pos);
      std::string_view entry = (comma != std::string_view::npos)
                                   ? srcset_view.substr(pos, comma - pos)
                                   : srcset_view.substr(pos);
      // The URL is everything up to the first space in the entry.
      size_t space = entry.find(' ');
      std::string_view url =
          (space != std::string_view::npos) ? entry.substr(0, space) : entry;
      if (!url.empty() && !IsAllowedPreloadUrl(url)) {
        srcset_safe = false;
        break;
      }
      pos = (comma != std::string_view::npos) ? comma + 1 : srcset_view.size();
    }
    if (srcset_safe) {
      parser_->AddAttribute(link_element, "imagesrcset", lcp_candidate_.srcset);
    }
  }

  // Add imagesizes if available.
  if (!lcp_candidate_.sizes.empty()) {
    parser_->AddAttribute(link_element, "imagesizes", lcp_candidate_.sizes);
  }

  parser_->InsertNodeBeforeCurrent(link_element);
  lcp_preload_injected_ = true;
  modified_ = true;
}

void HtmlTransformFilter::ApplyImageDimensions(
    net_instaweb::HtmlElement* element) {
  using net_instaweb::HtmlName;

  // Check existing width and height attributes.
  bool has_valid_width = false;
  bool has_valid_height = false;

  const auto* width_attr = element->FindAttribute(HtmlName::kWidth);
  if (width_attr != nullptr) {
    has_valid_width = IsValidPixelDimension(width_attr->DecodedValueOrNull());
  }

  const auto* height_attr = element->FindAttribute(HtmlName::kHeight);
  if (height_attr != nullptr) {
    has_valid_height = IsValidPixelDimension(height_attr->DecodedValueOrNull());
  }

  if (has_valid_width && has_valid_height) return;

  // Need at least one dimension. Get image URL from src.
  const char* src = element->AttributeValue(HtmlName::kSrc);
  if (src == nullptr || src[0] == '\0') return;

  // Skip data: URLs and inline images.
  std::string_view src_view(src);
  if (src_view.starts_with("data:")) return;

  if (cache_ == nullptr) return;

  // Only an image the server serves to any client may size this page.
  if (subresource_gate_ == nullptr) return;

  // Look up the image in cache to read its dimensions.
  auto read_image = [&] {
    return cache_->ReadBestAlternate(std::string(src), hostname_, scheme_,
                                     CapabilityMask(),
                                     /*agent_request_entitled=*/false, site_);
  };
  std::optional<ReadResult> read_result;
  {
    SubresourceRead read;
    read.site = site_;
    read.scheme = scheme_;
    read.host = hostname_;
    read.url = src_view;
    read.page_host = hostname_;
    read.host_fetch_allowed = host_fetch_allowed_;
    // Under a host the worker may not ask, an image the front end recorded
    // from an anonymous request is admitted as read.  Anything else is asked
    // about first, without holding a read.
    if (!read.host_fetch_allowed) {
      auto first = read_image();
      if (first.has_value() &&
          (first->metadata.flags & AlternateMetadata::kFlagAnonymousRecorded) !=
              0) {
        read.entry_read = true;
        read.entry_flags = first->metadata.flags;
        read.entry_site = site_;  // the read above is keyed by site_
        if (subresource_gate_->Admitted(read, /*may_fetch=*/true)) {
          read_result.emplace(std::move(*first));
        }
        read.entry_read = false;
      }
    }
    if (!read_result.has_value()) {
      if (!subresource_gate_->Admitted(read, /*may_fetch=*/true)) return;
      auto again = read_image();
      if (!again.has_value()) return;
      read_result.emplace(std::move(*again));
    }
  }

  auto content_span = read_result->content();
  if (content_span.empty()) return;

  ImageDimensions dims = ReadImageDimensions(std::span<const uint8_t>(
      reinterpret_cast<const uint8_t*>(content_span.data()),
      content_span.size()));
  // Hold header-inferred dimensions to the SAME range we demand of author
  // input: without this the filter would emit, via inference, a value it
  // would have rejected had the page supplied it (a crafted or corrupt PNG
  // can declare dimensions up to libpng's default 1,000,000 user limit).
  // Out-of-range means SKIP, never clamp -- a clamped value is a confidently
  // wrong aspect ratio, which is worse than emitting nothing and letting the
  // browser fall back to its normal behavior.  Both dimensions drop together
  // because they come from the same distrusted header.
  if (!dims.valid || dims.width <= 0 || dims.height <= 0 ||
      dims.width > kMaxPixelDimension || dims.height > kMaxPixelDimension) {
    return;
  }

  if (!has_valid_width) {
    parser_->AddAttribute(element, HtmlName::kWidth,
                          std::to_string(dims.width));
    modified_ = true;
  }
  if (!has_valid_height) {
    parser_->AddAttribute(element, HtmlName::kHeight,
                          std::to_string(dims.height));
    modified_ = true;
  }
}

void HtmlTransformFilter::ApplyAsyncCss(net_instaweb::HtmlElement* element) {
  using net_instaweb::HtmlName;

  // Only convert ACTIVE <link rel="stylesheet" href="..."> to non-blocking.
  // Held as the Attribute* (not the decoded value) because the conversion below
  // rewrites it in place: one lookup, and the pointer is provably non-null past
  // this guard.
  net_instaweb::HtmlElement::Attribute* rel_attr =
      element->FindAttribute(HtmlName::kRel);
  const char* rel =
      (rel_attr != nullptr) ? rel_attr->DecodedValueOrNull() : nullptr;
  if (rel == nullptr || std::string_view(rel) != "stylesheet") return;
  const char* href = element->AttributeValue(HtmlName::kHref);
  if (href == nullptr || href[0] == '\0') return;

  // Skip if already processed by us.
  if (element->FindAttribute("data-pagespeed-async") != nullptr) return;
  // Skip disabled stylesheets: they are inactive, so deferring them is
  // pointless, and the <noscript> copy (which cannot carry the live `disabled`
  // DOM state) would wrongly ACTIVATE them for no-JS clients.
  if (element->FindAttribute("disabled") != nullptr) return;

  // Make the full stylesheet load WITHOUT blocking first paint. The inlined
  // critical CSS (InjectCriticalCss) renders above-the-fold immediately; the
  // full sheet is applied once it finishes loading. Pattern:
  //   <link rel="preload" as="style" href
  //         data-pagespeed-async data-pagespeed-media="<original>">
  //   <noscript><link rel="stylesheet" href [media] [integrity...]></noscript>
  //   ...plus one <script defer src="{loader}"> per document.
  // A preload is not a stylesheet, so it does not block first paint; `as=style`
  // is what makes the browser fetch it at the priority a stylesheet deserves,
  // and what makes it eligible for Early-Hints promotion (worker.cc emits the
  // hint for exactly these sheets). The external loader (InjectAsyncCssLoader)
  // flips rel back to "stylesheet" and restores the recorded media — it runs
  // under a `script-src 'self'` CSP, which an inline onload= handler cannot
  // (see async_css_loader.h for the nonce + strict-dynamic caveat). Because the
  // loader mutates THIS element rather than creating a second <link>, the
  // preload's consumer matches its `as` and the sheet is fetched once. The
  // <noscript> copy keeps the stylesheet for clients without JavaScript.
  const char* media = element->AttributeValue(HtmlName::kMedia);
  const bool has_media_attr = (media != nullptr);
  // Case-insensitive whole-value match (media here is a simple attribute value).
  auto media_eq = [media](std::string_view kw) {
    if (media == nullptr) return false;
    std::string_view m(media);
    if (m.size() != kw.size()) return false;
    for (size_t i = 0; i < m.size(); ++i) {
      if (static_cast<char>(m[i] | 0x20) != kw[i]) return false;
    }
    return true;
  };
  // A media="print" sheet is already non-render-blocking for screen — deferring
  // it is pointless, so leave it alone.
  if (media_eq("print")) return;
  // Empty media="" and media="all" both mean the default (render-blocking) "all"
  // media — treat them as NOT meaningful so the transform is a FIXED POINT
  // across revalidation (otherwise the noscript media churns "all" <-> absent)
  // and we don't pin a redundant media attribute on the fallback copy.
  const bool media_meaningful =
      has_media_attr && media[0] != '\0' && !media_eq("all");
  const std::string original_media = media_meaningful ? media : "all";
  // Copy values before mutating the element's attribute list — the mutations
  // below may invalidate the pointers returned by the element's accessors.
  const std::string href_str = href;
  // Attributes that must survive onto the <noscript> fallback copy: SRI
  // integrity (+ the crossorigin it requires) and other load-affecting attrs.
  static constexpr const char* kCarryAttrs[] = {
      "integrity", "crossorigin", "referrerpolicy", "type", "title"};
  // Carry on PRESENCE, not on having a value: a bare valueless attribute like
  // `<link ... crossorigin>` (equivalent to crossorigin="anonymous", and
  // required for SRI on a cross-origin sheet) has a NULL decoded value. Keying
  // on DecodedValueOrNull would drop it, so the <noscript> fallback would lose
  // crossorigin and SRI verification would fail for no-JS clients. nullopt
  // records "present but valueless" so it is re-emitted as a bare attribute.
  std::vector<std::pair<std::string, std::optional<std::string>>> carry;
  for (const char* name : kCarryAttrs) {
    const net_instaweb::HtmlElement::Attribute* a =
        element->FindAttribute(name);
    if (a == nullptr) continue;
    const char* value = a->DecodedValueOrNull();
    if (value != nullptr) {
      carry.emplace_back(name, std::string(value));
    } else {
      carry.emplace_back(name, std::nullopt);
    }
  }

  // rel="stylesheet" -> rel="preload": mutate the existing attribute in place
  // rather than delete + re-add, so `rel` keeps its authored position and the
  // transform stays a byte-level fixed point across revalidation.
  rel_attr->SetValue("preload");
  // `as` tells the browser what it is fetching: it sets the request's
  // destination (and with it the priority), and it is what Chrome matches the
  // preload against when the loader flips this same element to a stylesheet.
  // A mismatched or missing `as` downloads the sheet twice. Delete first: an
  // author `as` on a stylesheet link is meaningless but must not be duplicated.
  element->DeleteAttribute(HtmlName::kAs);
  parser_->AddAttribute(element, HtmlName::kAs, "style");

  // Remember the original media so the loader can restore it and revalidation
  // cleanup (RemoveAsyncCssMarkers) can put it back before re-processing.
  parser_->AddAttribute(element, "data-pagespeed-media", original_media);

  // Drop the author's media attribute (even an empty one). On a preload, media
  // is a fetch CONDITION, not the sheet's applicability — leaving it would let
  // a narrow media query suppress the very download we are trying to promote.
  // The real value lives in data-pagespeed-media and the loader puts it back
  // at the instant the link becomes a stylesheet again.
  if (has_media_attr) {
    element->DeleteAttribute(HtmlName::kMedia);
  }
  parser_->AddAttribute(element, "data-pagespeed-async", "");

  // <noscript> fallback so non-JS clients still load the full stylesheet.
  // Insert the element into the tree BEFORE AppendChild so it has a valid
  // position in the parser's event queue (matches InjectCriticalCss order).
  auto* noscript = parser_->NewElement(nullptr, HtmlName::kNoscript);
  parser_->AddAttribute(noscript, "data-pagespeed-async-fallback", "");
  parser_->InsertNodeAfterCurrent(noscript);
  auto* fallback_link = parser_->NewElement(noscript, HtmlName::kLink);
  parser_->AddAttribute(fallback_link, HtmlName::kRel, "stylesheet");
  parser_->AddAttribute(fallback_link, HtmlName::kHref, href_str);
  if (media_meaningful) {
    parser_->AddAttribute(fallback_link, HtmlName::kMedia, original_media);
  }
  for (const auto& [name, value] : carry) {
    // A null-data string_view emits the attribute with no value at all
    // (html_parse.h: "Pass in NULL for value"), faithfully reproducing a bare
    // valueless attribute; passing "" would wrongly emit name="" (and an empty
    // integrity="" would actively DISABLE SRI).
    parser_->AddAttribute(
        fallback_link, name,
        value.has_value() ? std::string_view(*value) : std::string_view());
  }
  parser_->AddAttribute(fallback_link, "data-pagespeed-async-fallback", "");
  parser_->AppendChild(noscript, fallback_link);

  // Inject the CSP-safe loader once, co-located with the FIRST deferred link,
  // so it is present even in documents with no <head>/<body> end tags.
  if (!async_css_loader_injected_) {
    InjectAsyncCssLoader(element);
  }

  // Recorded so RevertAsyncCss can undo this conversion if the inline critical
  // block turns out not to ship (see the CSP-ordering note in StartElement).
  deferred_css_links_.push_back(element);
  async_css_injected_nodes_.push_back(noscript);

  modified_ = true;
}

void HtmlTransformFilter::InjectAsyncCssLoader(
    net_instaweb::HtmlElement* /*element*/) {
  using net_instaweb::HtmlName;
  // One external, same-origin <script defer src="{loader}"> per document. An
  // external `self` script runs under a `script-src 'self'` CSP (unlike an
  // inline onload= handler; see async_css_loader.h for the strict-dynamic
  // caveat). `defer` runs it after parse, when most deferred sheets
  // have already loaded, so it restores their media synchronously. Inserted
  // right after the current (first deferred) link — InsertNodeAfterCurrent is
  // reliable mid-callback, whereas InsertNodeAfterNode on a just-queued sibling
  // is not.
  auto* script = parser_->NewElement(nullptr, HtmlName::kScript);
  parser_->AddAttribute(script, HtmlName::kSrc, kAsyncCssLoaderPath);
  parser_->AddAttribute(script, HtmlName::kDefer, "");
  parser_->AddAttribute(script, "data-pagespeed-async-loader", "");
  parser_->InsertNodeAfterCurrent(script);
  async_css_injected_nodes_.push_back(script);
  async_css_loader_injected_ = true;
  modified_ = true;
}

void HtmlTransformFilter::RestoreDeferredLink(
    net_instaweb::HtmlElement* element) {
  using net_instaweb::HtmlName;

  // rel="preload" -> rel="stylesheet" and drop the `as` we added. This must
  // happen on BOTH paths (revalidation cleanup and the same-pass revert): a
  // link left at rel="preload" with the loader gone downloads the sheet and
  // then never applies it. Mutate in place so `rel` keeps its position, which
  // is what makes re-applying the transform a byte-level fixed point; a
  // deferred link always has a rel, but fall back to adding one rather than
  // dereference null if some other path ever strips it.
  net_instaweb::HtmlElement::Attribute* rel =
      element->FindAttribute(HtmlName::kRel);
  if (rel != nullptr) {
    rel->SetValue("stylesheet");
  } else {
    parser_->AddAttribute(element, HtmlName::kRel, "stylesheet");
  }
  element->DeleteAttribute(HtmlName::kAs);

  // Restore the original media attribute (recorded in data-pagespeed-media),
  // then strip our markers so ApplyAsyncCss can re-apply cleanly. If the
  // original had no media (or "all"), leave it unset (default is all media).
  const net_instaweb::HtmlElement::Attribute* saved =
      element->FindAttribute("data-pagespeed-media");
  const char* saved_media =
      (saved != nullptr) ? saved->DecodedValueOrNull() : nullptr;
  element->DeleteAttribute(HtmlName::kMedia);
  if (saved_media != nullptr && std::string_view(saved_media) != "all" &&
      saved_media[0] != '\0') {
    parser_->AddAttribute(element, HtmlName::kMedia, saved_media);
  }
  element->DeleteAttribute("data-pagespeed-media");
  element->DeleteAttribute("data-pagespeed-async");
}

void HtmlTransformFilter::RemoveAsyncCssMarkers(
    net_instaweb::HtmlElement* element) {
  using net_instaweb::HtmlName;

  RestoreDeferredLink(element);
  // Older output formats also left these behind on the deferred link; strip
  // them on the revalidation path only. On the same-pass revert they would be
  // the AUTHOR's attributes, never ours.
  // onload is a known HTML attribute interned under HtmlName::kOnload, so it
  // must be deleted by keyword — the string overload would not match it.
  element->DeleteAttribute(HtmlName::kOnload);
  element->DeleteAttribute("fetchpriority");  // legacy format cleanup
  modified_ = true;
}

void HtmlTransformFilter::RevertAsyncCss() {
  // Both loops are guarded on rewritability, symmetrically: the explicit
  // IsRewritable check below and DeleteNode's own internal one (html_parse.cc)
  // are the same test. A node flushed out of the window cannot be touched, and
  // silently skipping it is the only option — which is why the filter must
  // never be driven with a mid-document Flush. Every shipped entry point does
  // StartParse -> ParseText(whole document) -> FinishParse, so the document is
  // one flush window and neither guard fires in practice.
  for (net_instaweb::HtmlElement* link : deferred_css_links_) {
    if (!parser_->IsRewritable(link)) continue;
    RestoreDeferredLink(link);
  }
  for (net_instaweb::HtmlNode* node : async_css_injected_nodes_) {
    parser_->DeleteNode(node);
  }
  deferred_css_links_.clear();
  async_css_injected_nodes_.clear();
  async_css_loader_injected_ = false;
}

void HtmlTransformFilter::ApplyScriptDeferral(
    net_instaweb::HtmlElement* element) {
  using net_instaweb::HtmlName;

  // Only defer scripts with a src attribute (not inline scripts).
  const char* src = element->AttributeValue(HtmlName::kSrc);
  if (src == nullptr || src[0] == '\0') return;

  // Skip if already async, defer, or type="module".
  if (element->FindAttribute(HtmlName::kAsync) != nullptr) return;
  if (element->FindAttribute(HtmlName::kDefer) != nullptr) return;
  const char* type_val = element->AttributeValue(HtmlName::kType);
  if (type_val != nullptr && std::string_view(type_val) == "module") return;

  // Skip if already marked by us.
  if (element->FindAttribute("data-pagespeed-defer") != nullptr) return;

  // Match src URL against defer_scripts_ using path-boundary suffix matching.
  // Pattern "analytics.js" matches ".../analytics.js" but not ".../xanalytics.js".
  std::string_view src_sv(src);
  bool matched = false;
  for (const auto& pattern : defer_scripts_) {
    if (pattern.empty()) continue;
    if (src_sv == pattern) {
      matched = true;
      break;
    }
    if (src_sv.ends_with(pattern) && src_sv.size() > pattern.size()) {
      char preceding = src_sv[src_sv.size() - pattern.size() - 1];
      if (preceding == '/' || preceding == '?' || preceding == '=') {
        matched = true;
        break;
      }
    }
  }
  if (!matched) return;

  // Add defer + marker attribute.
  parser_->AddAttribute(element, HtmlName::kDefer, "");
  parser_->AddAttribute(element, "data-pagespeed-defer", "");
  modified_ = true;
}

void HtmlTransformFilter::CollectMetaCsp(net_instaweb::HtmlElement* element) {
  using net_instaweb::HtmlName;

  const char* http_equiv = element->AttributeValue(HtmlName::kHttpEquiv);
  if (http_equiv == nullptr) return;
  // Enforcing policy only. Content-Security-Policy-Report-Only never blocks
  // rendering, so it must NOT gate our injections. The exact (case-insensitive)
  // match on "Content-Security-Policy" excludes the "-Report-Only" variant.
  if (!net_instaweb::StringCaseEqual(std::string_view(http_equiv),
                                     "Content-Security-Policy")) {
    return;
  }
  const char* content = element->AttributeValue(HtmlName::kContent);
  if (content == nullptr) return;
  meta_csps_.emplace_back(content);
}

// static
std::vector<std::string> HtmlTransformFilter::ReadHeaderCsps(
    PageSpeedCache* cache, std::string_view url, std::string_view hostname,
    std::string_view scheme, std::string_view site) {
  std::vector<std::string> out;
  if (cache == nullptr) return out;
  auto read = cache->ReadHeadersSidecar(url, hostname, scheme, site);
  if (!read.has_value()) return out;
  auto fields =
      ParseHeadersSidecar(PageSpeedCache::HeadersSidecarPayload(*read));
  if (!fields.has_value()) return out;
  for (const HeaderField& field : *fields) {
    // Enforcing policy only; Report-Only is not an exact match and is not
    // in the sidecar allowlist anyway.
    if (net_instaweb::StringCaseEqual(field.name, "Content-Security-Policy")) {
      out.emplace_back(field.value);
    }
  }
  return out;
}

bool HtmlTransformFilter::MetaCspAllowsInlineStyle() const {
  for (const std::string& csp : header_csps_) {
    if (!net_instaweb::InlineStyleAllowed(csp)) return false;
  }
  for (const std::string& csp : meta_csps_) {
    if (!net_instaweb::InlineStyleAllowed(csp)) return false;
  }
  return true;
}

bool HtmlTransformFilter::MetaCspAllowsInlineScript() const {
  for (const std::string& csp : header_csps_) {
    if (!net_instaweb::InlineScriptAllowed(csp)) return false;
  }
  for (const std::string& csp : meta_csps_) {
    if (!net_instaweb::InlineScriptAllowed(csp)) return false;
  }
  return true;
}

// static
bool HtmlTransformFilter::IsValidPixelDimension(const char* value) {
  if (value == nullptr || value[0] == '\0') return false;

  // Reject percentage values.
  std::string_view sv(value);
  if (sv.find('%') != std::string_view::npos) return false;

  // Must parse as a positive integer of at most kMaxPixelDimensionDigits
  // digits.  The length check is what enforces the range, and it bounds the
  // accumulation below so it cannot overflow.  (It does NOT canonicalize:
  // a zero-padded form longer than the cap, like "099999", is rejected by
  // length alone, but a shorter one like "0999" parses to 999 and is
  // accepted -- validated strings are not guaranteed canonical decimal.)
  // Given it, v can never exceed kMaxPixelDimension, so no numeric upper
  // bound is tested here -- see ApplyImageDimensions for the range check on
  // the header-inferred path, which has no such length to lean on.
  if (sv.size() > kMaxPixelDimensionDigits) return false;
  int v = 0;
  for (char c : sv) {
    if (c < '0' || c > '9') return false;
    v = v * 10 + (c - '0');
  }
  return v > 0;
}

// static
bool HtmlTransformFilter::IsAllowedPreloadUrl(std::string_view url) {
  return url.starts_with("https://") || url.starts_with("http://") ||
         (url.starts_with("/") && !url.starts_with("//"));
}

}  // namespace pagespeed
