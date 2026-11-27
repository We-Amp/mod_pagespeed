// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

// PageSpeed 2.0 — Critical-CSS validator implementation.
//
// See the header for what this is for and what it deliberately cannot see.

#include "src/browser/critical_css_validator.h"

#include <cctype>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <iterator>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "absl/status/statusor.h"
#include "absl/strings/str_cat.h"
#include "lib/base/string_util.h"
#include "src/browser/visual_regression_gate.h"
#include "src/worker/cascade_layer_order.h"
#include "src/worker/html_css_injector.h"
#include "src/worker/noscript_strip.h"

namespace pagespeed {

using net_instaweb::LowerChar;

namespace {

constexpr std::string_view kStyleOpen = "<style data-pagespeed-critical>";
constexpr std::string_view kStyleClose = "</style>";

bool EqualsIgnoreCase(std::string_view a, std::string_view b) {
  if (a.size() != b.size()) return false;
  for (size_t i = 0; i < a.size(); ++i) {
    if (LowerChar(a[i]) != LowerChar(b[i])) return false;
  }
  return true;
}

bool IsHtmlSpace(char c) {
  return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f';
}

// html[pos] is '<'. True when it opens a <link ...> tag (word boundary
// enforced, so <linkbox> is not one).
bool StartsLinkTag(std::string_view html, size_t pos) {
  static constexpr std::string_view kLink = "link";
  size_t after = pos + 1 + kLink.size();
  if (after > html.size()) return false;
  if (!EqualsIgnoreCase(html.substr(pos + 1, kLink.size()), kLink)) {
    return false;
  }
  if (after == html.size()) return true;
  char next = html[after];
  return next == '>' || next == '/' || IsHtmlSpace(next);
}

// One past the '>' that closes the tag starting at `pos`, honouring quoted
// attribute values so `title="a>b"` does not end it early. npos when the tag is
// never closed.
size_t FindTagEnd(std::string_view html, size_t pos) {
  char quote = '\0';
  for (size_t i = pos + 1; i < html.size(); ++i) {
    char c = html[i];
    if (quote != '\0') {
      if (c == quote) quote = '\0';
      continue;
    }
    if (c == '"' || c == '\'') {
      quote = c;
      continue;
    }
    if (c == '>') return i + 1;
  }
  return std::string_view::npos;
}

// Calls fn(name, value) for each attribute of the start-tag text `tag`
// (`<name attr=... >`), value unquoted and empty when the attribute has none.
// Stops early when fn returns true; returns whether it did.
template <typename Fn>
bool ForEachAttribute(std::string_view tag, Fn fn) {
  size_t i = 0;
  // Skip "<name".
  while (i < tag.size() && tag[i] != '>' && !IsHtmlSpace(tag[i])) ++i;

  while (i < tag.size()) {
    while (i < tag.size() && (IsHtmlSpace(tag[i]) || tag[i] == '/')) ++i;
    if (i >= tag.size() || tag[i] == '>') break;

    size_t name_start = i;
    while (i < tag.size() && !IsHtmlSpace(tag[i]) && tag[i] != '=' &&
           tag[i] != '>' && tag[i] != '/') {
      ++i;
    }
    std::string_view name = tag.substr(name_start, i - name_start);

    while (i < tag.size() && IsHtmlSpace(tag[i])) ++i;
    std::string_view value;
    if (i < tag.size() && tag[i] == '=') {
      ++i;
      while (i < tag.size() && IsHtmlSpace(tag[i])) ++i;
      if (i < tag.size() && (tag[i] == '"' || tag[i] == '\'')) {
        char quote = tag[i++];
        size_t value_start = i;
        while (i < tag.size() && tag[i] != quote) ++i;
        value = tag.substr(value_start, i - value_start);
        if (i < tag.size()) ++i;  // closing quote
      } else {
        size_t value_start = i;
        while (i < tag.size() && !IsHtmlSpace(tag[i]) && tag[i] != '>') ++i;
        value = tag.substr(value_start, i - value_start);
      }
    }
    if (fn(name, value)) return true;
  }
  return false;
}

// The first attribute called `name` (case-insensitive): whether it is present,
// and its value.
struct TagAttr {
  bool present = false;
  std::string_view value;
};
TagAttr FindTagAttribute(std::string_view tag, std::string_view name) {
  TagAttr out;
  ForEachAttribute(tag, [&](std::string_view n, std::string_view v) {
    if (!EqualsIgnoreCase(n, name)) return false;
    out.present = true;
    out.value = v;
    return true;
  });
  return out;
}

// True when the tag text (`<link ...>`) has `token` in its rel token list,
// matched per token and case-insensitively.  Parsed rather than
// substring-matched: `href="/stylesheet.css"` on a rel=preload is not a
// stylesheet link, and `rel="alternate stylesheet"` is.  The same rule as
// HtmlTransformFilter's RelHasToken.
bool LinkTagHasRelToken(std::string_view tag, std::string_view token) {
  TagAttr rel = FindTagAttribute(tag, "rel");
  if (!rel.present) return false;
  // rel is a space-separated token list.
  std::string_view value = rel.value;
  size_t t = 0;
  while (t < value.size()) {
    while (t < value.size() && IsHtmlSpace(value[t])) ++t;
    size_t token_start = t;
    while (t < value.size() && !IsHtmlSpace(value[t])) ++t;
    if (t > token_start &&
        EqualsIgnoreCase(value.substr(token_start, t - token_start), token)) {
      return true;
    }
  }
  return false;
}

bool LinkTagIsStylesheet(std::string_view tag) {
  return LinkTagHasRelToken(tag, "stylesheet");
}

// An author's loadCSS preload (`<link rel=preload as=style onload=...>`): a
// browser running scripts applies its sheet at this position,
// so it anchors the block like a stylesheet source. It is kept in the
// document: the validator renders without scripts, where it applies nothing,
// and its sheet is part of the inlined full stylesheet. The same rule as
// HtmlTransformFilter's IsLoadCssPreload.
bool LinkTagIsLoadCssPreload(std::string_view tag) {
  if (!LinkTagHasRelToken(tag, "preload")) return false;
  TagAttr as = FindTagAttribute(tag, "as");
  std::string_view as_value = as.value;
  while (!as_value.empty() && IsHtmlSpace(as_value.front())) {
    as_value.remove_prefix(1);
  }
  while (!as_value.empty() && IsHtmlSpace(as_value.back())) {
    as_value.remove_suffix(1);
  }
  return as.present && EqualsIgnoreCase(as_value, "style") &&
         FindTagAttribute(tag, "onload").present &&
         !FindTagAttribute(tag, "data-pagespeed-hint").present;
}

// html[pos] is '<'. The element name of the start or end tag there (and
// whether it is an end tag), or an empty name when it is neither.
struct TagName {
  std::string_view name;
  bool end_tag = false;
};
TagName ReadTagName(std::string_view html, size_t pos) {
  TagName out;
  size_t i = pos + 1;
  if (i < html.size() && html[i] == '/') {
    out.end_tag = true;
    ++i;
  }
  size_t start = i;
  while (i < html.size() &&
         (std::isalnum(static_cast<unsigned char>(html[i])) != 0 ||
          html[i] == '-')) {
    ++i;
  }
  if (i == start ||
      std::isalpha(static_cast<unsigned char>(html[start])) == 0) {
    return {};
  }
  out.name = html.substr(start, i - start);
  return out;
}

// Subtrees where an inserted <style> is not a document stylesheet for every
// client, so a source inside one never anchors the block. The same list as
// HtmlTransformFilter's IsNonDocumentSubtreeRoot, <select> and <plaintext>
// included: the serve path injects no block while a page is in select or
// after a <plaintext>, and a source inside a <select> that has closed again
// does not anchor it either.
constexpr std::string_view kNonDocumentSubtrees[] = {
    "noscript", "noembed", "noframes", "template",
    "svg",      "math",    "select",   "plaintext"};

// A head-prelude element the block must never move ahead of: <base>, <meta
// charset>, <meta http-equiv="Content-Type" | "Content-Security-Policy">.
// The same rule as HtmlTransformFilter::TrackCriticalCssAnchor.
bool IsHeadPrelude(std::string_view name, std::string_view tag) {
  if (EqualsIgnoreCase(name, "base")) return true;
  if (!EqualsIgnoreCase(name, "meta")) return false;
  if (FindTagAttribute(tag, "charset").present) return true;
  TagAttr http_equiv = FindTagAttribute(tag, "http-equiv");
  return http_equiv.present &&
         (EqualsIgnoreCase(http_equiv.value, "content-type") ||
          EqualsIgnoreCase(http_equiv.value, "content-security-policy"));
}

template <size_t N>
bool NameIsOneOf(std::string_view name, const std::string_view (&names)[N]) {
  for (std::string_view n : names) {
    if (EqualsIgnoreCase(name, n)) return true;
  }
  return false;
}

// The serve path's parser (the vendored HtmlLexer) reads inline SVG and MathML
// as browsers do (HTML Standard section 13.2.6), and gives their elements no
// keyword: an SVG or MathML <style> or <link> is neither a stylesheet source
// nor raw text there. OpenElements follows the open elements with the parser's
// rules, so the scanner below can tell such an element from an HTML one.
// The name sets are the parser's.

// Start tags that end SVG and MathML content; <font> does too with a color,
// face or size attribute.
constexpr std::string_view kForeignBreakoutStartTags[] = {
    "b",      "big",    "blockquote", "body",    "br",    "center", "code",
    "dd",     "div",    "dl",         "dt",      "em",    "embed",  "h1",
    "h2",     "h3",     "h4",         "h5",      "h6",    "head",   "hr",
    "i",      "img",    "li",         "listing", "menu",  "meta",   "nobr",
    "ol",     "p",      "pre",        "ruby",    "s",     "small",  "span",
    "strong", "strike", "sub",        "sup",     "table", "tt",     "u",
    "ul",     "var"};
constexpr std::string_view kForeignBreakoutFontAttributes[] = {"color", "face",
                                                               "size"};
constexpr std::string_view kSvgHtmlIntegrationPoints[] = {
    "desc", "foreignobject", "title"};
constexpr std::string_view kMathMlTextIntegrationPoints[] = {"mi", "mn", "mo",
                                                             "ms", "mtext"};
constexpr std::string_view kMathMlTextIntegrationPointExceptions[] = {
    "malignmark", "mglyph"};
constexpr std::string_view kAnnotationXmlHtmlEncodings[] = {
    "application/xhtml+xml", "text/html"};
// End tags handled by the HTML rules that walk past SVG or MathML content.
constexpr std::string_view kEndTagsThatDoNotPop[] = {"body", "form", "head",
                                                     "html"};
constexpr std::string_view kFormattingElements[] = {
    "a",    "b", "big",   "code",   "em",     "font", "i",
    "nobr", "s", "small", "strike", "strong", "tt",   "u"};
constexpr std::string_view kEndTagsClosingInScope[] = {
    "address", "applet",  "article",  "aside",      "blockquote", "button",
    "center",  "dd",      "details",  "dialog",     "dir",        "div",
    "dl",      "dt",      "fieldset", "figcaption", "figure",     "footer",
    "h1",      "h2",      "h3",       "h4",         "h5",         "h6",
    "header",  "hgroup",  "li",       "listing",    "main",       "marquee",
    "menu",    "nav",     "object",   "ol",         "pre",        "search",
    "section", "summary", "ul"};
constexpr std::string_view kScopeBoundaries[] = {"applet",  "caption",  "html",
                                                 "marquee", "object",   "table",
                                                 "td",      "template", "th"};
constexpr std::string_view kListItemScopeBoundaries[] = {"ol", "ul"};
constexpr std::string_view kTableFamilyEndTags[] = {
    "caption", "col", "colgroup", "optgroup", "option", "select", "table",
    "tbody",   "td",  "template", "tfoot",    "th",     "thead",  "tr"};
constexpr std::string_view kSpecialHtmlElements[] = {
    "address",    "applet",   "area",       "article",  "aside",   "base",
    "basefont",   "bgsound",  "blockquote", "body",     "br",      "button",
    "caption",    "center",   "col",        "colgroup", "dd",      "details",
    "dir",        "div",      "dl",         "dt",       "embed",   "fieldset",
    "figcaption", "figure",   "footer",     "form",     "frame",   "frameset",
    "h1",         "h2",       "h3",         "h4",       "h5",      "h6",
    "head",       "header",   "hgroup",     "hr",       "html",    "iframe",
    "img",        "input",    "keygen",     "li",       "link",    "listing",
    "main",       "marquee",  "menu",       "meta",     "nav",     "noembed",
    "noframes",   "noscript", "object",     "ol",       "p",       "param",
    "plaintext",  "pre",      "script",     "search",   "section", "select",
    "source",     "style",    "summary",    "table",    "tbody",   "td",
    "template",   "textarea", "tfoot",      "th",       "thead",   "title",
    "tr",         "track",    "ul",         "wbr",      "xmp"};
// HTML elements without an end tag.
constexpr std::string_view kVoidElements[] = {
    "area",  "base",  "basefont", "bgsound", "br",    "col",    "embed",
    "frame", "hr",    "image",    "img",     "input", "keygen", "link",
    "meta",  "param", "source",   "track",   "wbr"};

class OpenElements {
 public:
  enum class Ns : std::uint8_t { kHtml, kSvg, kMath };

  // A start tag `name` (the tag text is `tag`): ends foreign content first
  // when it is a breakout tag, and returns the namespace of the element it
  // opens. Call Push for it afterwards when it stays open.
  Ns StartTag(std::string_view name, std::string_view tag) {
    bool html_rules = StartTagUsesHtmlRules(name);
    if (!html_rules && IsBreakout(name, tag)) {
      PopForeignForBreakout();
      html_rules = true;
    }
    if (!html_rules) return Current().ns;
    if (EqualsIgnoreCase(name, "svg")) return Ns::kSvg;
    if (EqualsIgnoreCase(name, "math")) return Ns::kMath;
    return Ns::kHtml;
  }

  void Push(std::string_view name, std::string_view tag, Ns ns) {
    Open open;
    open.name = std::string(name);
    open.ns = ns;
    if (ns == Ns::kSvg) {
      open.html_ip = NameIsOneOf(name, kSvgHtmlIntegrationPoints);
      open.special = open.html_ip;
    } else if (ns == Ns::kMath) {
      open.text_ip = NameIsOneOf(name, kMathMlTextIntegrationPoints);
      open.annotation_xml = EqualsIgnoreCase(name, "annotation-xml");
      if (open.annotation_xml) {
        TagAttr encoding = FindTagAttribute(tag, "encoding");
        open.html_ip = encoding.present &&
                       NameIsOneOf(encoding.value, kAnnotationXmlHtmlEncodings);
      }
      open.special = open.annotation_xml || open.text_ip;
    }
    stack_.push_back(std::move(open));
  }

  // An HTML element that stays open after its start tag.
  static bool HtmlElementStaysOpen(std::string_view name) {
    return !NameIsOneOf(name, kVoidElements);
  }

  void EndTag(std::string_view name) {
    if (InForeignContent()) {
      if (EqualsIgnoreCase(name, "br") || EqualsIgnoreCase(name, "p")) {
        PopForeignForBreakout();
      } else {
        for (size_t i = stack_.size(); i-- > 0;) {
          if (EqualsIgnoreCase(stack_[i].name, name)) {
            stack_.resize(i);
            return;
          }
          if (i > 0 && stack_[i - 1].ns == Ns::kHtml) break;
        }
      }
    }
    bool crossed_foreign = false;
    bool crossed_special_html = false;
    const bool do_not_pop = NameIsOneOf(name, kEndTagsThatDoNotPop);
    const bool formatting = NameIsOneOf(name, kFormattingElements);
    const bool closes_in_scope = NameIsOneOf(name, kEndTagsClosingInScope);
    const bool table_family = NameIsOneOf(name, kTableFamilyEndTags);
    const bool list_item = EqualsIgnoreCase(name, "li");
    for (size_t i = stack_.size(); i-- > 0;) {
      const Open& o = stack_[i];
      if (o.ns != Ns::kHtml) {
        if (o.special) return;
        crossed_foreign = true;
      } else if (EqualsIgnoreCase(o.name, name)) {
        if (crossed_foreign &&
            (do_not_pop || (formatting && crossed_special_html))) {
          return;
        }
        stack_.resize(i);
        return;
      } else if (crossed_foreign && NameIsOneOf(o.name, kSpecialHtmlElements)) {
        crossed_special_html = true;
        if (closes_in_scope) {
          if (NameIsOneOf(o.name, kScopeBoundaries) ||
              (list_item && NameIsOneOf(o.name, kListItemScopeBoundaries))) {
            return;
          }
        } else if (!formatting && !table_family && !do_not_pop) {
          return;
        }
      }
    }
  }

  // Whether the current node is an SVG or MathML element.
  bool InForeignContent() const { return Current().ns != Ns::kHtml; }

 private:
  struct Open {
    std::string name;
    Ns ns = Ns::kHtml;
    bool html_ip = false;  // an HTML integration point
    bool text_ip = false;  // a MathML text integration point
    bool annotation_xml = false;
    bool special = false;  // an integration point or annotation-xml
  };

  const Open& Current() const {
    static const Open kDocument;
    return stack_.empty() ? kDocument : stack_.back();
  }

  bool StartTagUsesHtmlRules(std::string_view name) const {
    const Open& current = Current();
    if (current.ns == Ns::kHtml || current.html_ip) return true;
    if (current.text_ip &&
        !NameIsOneOf(name, kMathMlTextIntegrationPointExceptions)) {
      return true;
    }
    return current.annotation_xml && EqualsIgnoreCase(name, "svg");
  }

  static bool IsBreakout(std::string_view name, std::string_view tag) {
    if (NameIsOneOf(name, kForeignBreakoutStartTags)) return true;
    if (!EqualsIgnoreCase(name, "font")) return false;
    return ForEachAttribute(tag, [](std::string_view n, std::string_view) {
      return NameIsOneOf(n, kForeignBreakoutFontAttributes);
    });
  }

  void PopForeignForBreakout() {
    while (!stack_.empty()) {
      const Open& o = stack_.back();
      if (o.ns == Ns::kHtml || o.html_ip || o.text_ip) break;
      stack_.pop_back();
    }
  }

  std::vector<Open> stack_;
};

// True when html[pos] opens a CDATA section.
bool StartsCdata(std::string_view html, size_t pos) {
  return html.substr(pos, 9) == "<![CDATA[";
}

struct StripResult {
  bool ok = false;
  std::string error;
  std::string html;
  size_t links_removed = 0;
  size_t styles_removed = 0;
  // Where the serve path would put the block, as an offset into `html` (the
  // stripped output). See PlaceBlock.
  size_t injection_pos = 0;
};

// One thing the placement rule reads, at its offset in the stripped output.
struct PlacementEvent {
  size_t pos;
  bool prelude;  // else a stylesheet source that may anchor
};

// The serve path's placement (HtmlTransformFilter::InjectCriticalCss),
// replayed on the stripped document: the decision is taken at the
// </head> (else </body>) the string injector would use, and the block goes
// before the first stylesheet source seen since the last head-prelude element
// before that point, else at that point.
size_t PlaceBlock(std::string_view stripped,
                  const std::vector<PlacementEvent>& events) {
  const size_t fallback = CriticalCssFallbackOffset(stripped);
  size_t anchor = std::string_view::npos;
  for (const PlacementEvent& e : events) {
    // A removed source sits AT the offset of whatever followed it, so one that
    // stood right before </head> has pos == fallback and still counts; a
    // prelude element is kept, and cannot start where </head> does.
    if (e.pos > fallback) break;
    if (e.prelude) {
      anchor = std::string_view::npos;
    } else if (anchor == std::string_view::npos) {
      anchor = e.pos;
    }
  }
  return anchor != std::string_view::npos ? anchor : fallback;
}

// Remove every stylesheet <link> and every <style> element, leaving everything
// else — including markup that only LOOKS like one because it sits inside a
// comment or a <script> string — exactly where it was, and work out where the
// serve path would put the critical block (PlaceBlock).
//
// The worker's own markup is recognised the way the serve path recognises it
// on a revalidation pass: a previous <style data-pagespeed-critical> is removed
// and never anchors (the filter deletes it first), and a deferred primary
// <link data-pagespeed-async> (rel=preload) is a stylesheet source again. An
// author's loadCSS preload is kept, and anchors like a source.
StripResult StripStylesheetSources(
    std::string_view html, const std::vector<size_t>& noscript_preludes) {
  StripResult out;
  out.html.reserve(html.size());
  std::vector<PlacementEvent> events;
  size_t next_prelude = 0;
  // A head-prelude element the serve path saw inside a <noscript> that is no
  // longer here, at the input offset where that <noscript> stood.
  auto emit_noscript_preludes = [&](size_t upto) {
    while (next_prelude < noscript_preludes.size() &&
           noscript_preludes[next_prelude] <= upto) {
      events.push_back({out.html.size(), /*prelude=*/true});
      ++next_prelude;
    }
  };
  size_t subtree_depth[std::size(kNonDocumentSubtrees)] = {};
  auto in_non_document_subtree = [&subtree_depth] {
    for (size_t d : subtree_depth) {
      if (d > 0) return true;
    }
    return false;
  };
  OpenElements open;
  // Tags before this offset have been given to `open` (or are the content of
  // an <iframe>, which the parser reads as text).
  size_t tracked_until = 0;

  size_t pos = 0;
  while (pos < html.size()) {
    emit_noscript_preludes(pos);
    if (html_scan::StartsComment(html, pos)) {
      size_t end = html_scan::ScanComment(html, pos);
      if (end == std::string_view::npos) {
        out.error = "unterminated HTML comment";
        return out;
      }
      // As the serve path writes it: `<!-->` goes out as `<!---->`, and a
      // comment ended by `--!>` with `-->`.
      absl::StrAppend(&out.html, "<!--", html_scan::CommentText(html, pos, end),
                      "-->");
      pos = end;
      continue;
    }

    if (html[pos] == '<') {
      // In SVG and MathML content a CDATA section is text up to "]]>".
      if (open.InForeignContent() && StartsCdata(html, pos)) {
        size_t end = html.find("]]>", pos + 9);
        end = end == std::string_view::npos ? html.size() : end + 3;
        out.html.append(html.substr(pos, end - pos));
        pos = end;
        continue;
      }
      TagName tn = ReadTagName(html, pos);
      // An SVG or MathML element: never raw text, never a stylesheet source,
      // never a head prelude. Copied through as it is.
      bool foreign_tag = false;
      if (!tn.name.empty() && pos >= tracked_until) {
        size_t end = FindTagEnd(html, pos);
        if (end == std::string_view::npos) end = html.size();
        std::string_view tag = html.substr(pos, end - pos);
        tracked_until = end;
        if (tn.end_tag) {
          open.EndTag(tn.name);
        } else {
          const OpenElements::Ns ns = open.StartTag(tn.name, tag);
          if (ns != OpenElements::Ns::kHtml) {
            foreign_tag = true;
            const bool self_closing = tag.size() >= 2 && tag.back() == '>' &&
                                      tag[tag.size() - 2] == '/';
            if (!self_closing) open.Push(tn.name, tag, ns);
          } else if (!html_scan::ScanRawTextElement(html, pos).matched &&
                     OpenElements::HtmlElementStaysOpen(tn.name)) {
            open.Push(tn.name, tag, ns);
            if (EqualsIgnoreCase(tn.name, "iframe")) {
              // Its content is text up to </iframe.
              size_t close = end;
              while (
                  close < html.size() &&
                  !(html[close] == '<' &&
                    EqualsIgnoreCase(html.substr(close + 1, 7), "/iframe"))) {
                ++close;
              }
              tracked_until = close;
            }
          }
        }
      }

      html_scan::RawTextElement rt;
      if (!foreign_tag) rt = html_scan::ScanRawTextElement(html, pos);
      if (rt.matched) {
        if (!rt.closed) {
          // The rest of the document is inside it. Neither keeping nor dropping
          // the remainder produces a document that means anything.
          out.error = absl::StrCat("unterminated <", rt.tag, "> element");
          return out;
        }
        if (EqualsIgnoreCase(rt.tag, "style")) {
          ++out.styles_removed;
          size_t open_end = FindTagEnd(html, pos);
          std::string_view open_tag = html.substr(
              pos, (open_end == std::string_view::npos || open_end > rt.end)
                       ? rt.end - pos
                       : open_end - pos);
          if (!FindTagAttribute(open_tag, "data-pagespeed-critical").present &&
              !in_non_document_subtree()) {
            events.push_back({out.html.size(), /*prelude=*/false});
          }
        } else {
          out.html.append(html.substr(pos, rt.end - pos));
        }
        pos = rt.end;
        continue;
      }

      if (!foreign_tag && StartsLinkTag(html, pos)) {
        size_t end = FindTagEnd(html, pos);
        if (end == std::string_view::npos) {
          out.error = "unterminated <link> tag";
          return out;
        }
        std::string_view tag = html.substr(pos, end - pos);
        const bool fallback_copy =
            FindTagAttribute(tag, "data-pagespeed-async-fallback").present;
        const bool deferred_primary =
            !fallback_copy &&
            FindTagAttribute(tag, "data-pagespeed-async").present;
        if (deferred_primary || LinkTagIsStylesheet(tag)) {
          ++out.links_removed;
          if (!fallback_copy && !in_non_document_subtree()) {
            events.push_back({out.html.size(), /*prelude=*/false});
          }
        } else {
          if (LinkTagIsLoadCssPreload(tag) && !in_non_document_subtree()) {
            events.push_back({out.html.size(), /*prelude=*/false});
          }
          out.html.append(tag);
        }
        pos = end;
        continue;
      }

      // Any other tag is copied through below, character by character, as
      // before; these few are only looked at on the way.
      // Every other tag is copied character by character below, as before. A
      // few are looked at on the way: the non-document subtree boundaries and
      // the head prelude, which the placement rule reads.
      size_t subtree = std::size(kNonDocumentSubtrees);
      for (size_t k = 0; k < std::size(kNonDocumentSubtrees); ++k) {
        if (EqualsIgnoreCase(tn.name, kNonDocumentSubtrees[k])) subtree = k;
      }
      const bool maybe_prelude = !foreign_tag && !tn.end_tag &&
                                 (EqualsIgnoreCase(tn.name, "meta") ||
                                  EqualsIgnoreCase(tn.name, "base"));
      if (subtree < std::size(kNonDocumentSubtrees) || maybe_prelude) {
        size_t end = FindTagEnd(html, pos);
        std::string_view tag = html.substr(
            pos, end == std::string_view::npos ? html.size() - pos : end - pos);
        if (subtree < std::size(kNonDocumentSubtrees)) {
          // `/>` closes only in foreign content; <noscript/> still opens.
          const bool foreign = EqualsIgnoreCase(tn.name, "svg") ||
                               EqualsIgnoreCase(tn.name, "math");
          const bool self_closed = foreign && tag.size() >= 2 &&
                                   tag.back() == '>' &&
                                   tag[tag.size() - 2] == '/';
          if (tn.end_tag) {
            if (subtree_depth[subtree] > 0) --subtree_depth[subtree];
          } else if (!self_closed) {
            ++subtree_depth[subtree];
          }
        } else if (IsHeadPrelude(tn.name, tag)) {
          events.push_back({out.html.size(), /*prelude=*/true});
        }
      }
    }

    out.html.push_back(html[pos]);
    ++pos;
  }

  emit_noscript_preludes(html.size());
  out.injection_pos = PlaceBlock(out.html, events);
  out.ok = true;
  return out;
}

// True when the markup of a removed <noscript> holds a head-prelude element,
// read the way the serve path's parser reads it: <noscript> content as markup
// (the HtmlLexer parses it, kSometimesLiteralTags), comments skipped.
bool NoscriptHoldsPrelude(std::string_view markup) {
  size_t pos = 1;  // past the <noscript>'s own '<'
  while ((pos = markup.find('<', pos)) != std::string_view::npos) {
    if (html_scan::StartsComment(markup, pos)) {
      const size_t end = html_scan::ScanComment(markup, pos);
      if (end == std::string_view::npos) return false;
      pos = end;
      continue;
    }
    const TagName tn = ReadTagName(markup, pos);
    if (!tn.end_tag && (EqualsIgnoreCase(tn.name, "meta") ||
                        EqualsIgnoreCase(tn.name, "base"))) {
      const size_t end = FindTagEnd(markup, pos);
      const std::string_view tag = markup.substr(
          pos, end == std::string_view::npos ? markup.size() - pos : end - pos);
      if (IsHeadPrelude(tn.name, tag)) return true;
    }
    ++pos;
  }
  return false;
}

// The comments of `html` as the serve path's HtmlLexer reads them
// (html_scan::ScanComment; an unterminated one runs to the end), outside
// raw-text elements, <noscript> content read as markup as the lexer does.
// [begin, end) in document order.
std::vector<std::pair<size_t, size_t>> LexerCommentExtents(
    std::string_view html) {
  std::vector<std::pair<size_t, size_t>> out;
  size_t pos = 0;
  while ((pos = html.find('<', pos)) != std::string_view::npos) {
    if (html_scan::StartsComment(html, pos)) {
      size_t end = html_scan::ScanComment(html, pos);
      if (end == std::string_view::npos) end = html.size();
      out.emplace_back(pos, end);
      pos = end;
      continue;
    }
    const html_scan::RawTextElement rt =
        html_scan::ScanRawTextElement(html, pos);
    if (rt.matched && rt.closed) {
      pos = rt.end;
      continue;
    }
    ++pos;
  }
  return out;
}

// True when a removed <noscript> range and a lexer-rule comment overlap
// without one holding the other. The removal reads comments as a browser
// does and the placement replay as the serve path does; a comment whose
// `-->` the lexer finds inside a removed <noscript> (or one that starts in
// it and ends past it) would make the replay see markup the serve path took
// for comment text, or the reverse.
bool NoscriptCrossesALexerComment(
    const std::vector<std::pair<size_t, size_t>>& removed,
    const std::vector<std::pair<size_t, size_t>>& comments) {
  size_t c = 0;
  for (const auto& [rb, re] : removed) {
    while (c < comments.size() && comments[c].second <= rb) ++c;
    for (size_t k = c; k < comments.size() && comments[k].first < re; ++k) {
      const auto& [cb, ce] = comments[k];
      const bool comment_inside = cb >= rb && ce <= re;
      const bool noscript_inside = rb >= cb && re <= ce;
      if (!comment_inside && !noscript_inside) return true;
    }
  }
  return false;
}

ValidationDocuments Refuse(std::string error) {
  ValidationDocuments out;
  out.ok = false;
  out.error = std::move(error);
  return out;
}

ValidationVerdict NotValidated(std::string reason) {
  ValidationVerdict v;
  v.validated = false;
  v.diff_ratio = -1.0f;
  v.failure_reason = std::move(reason);
  return v;
}

}  // namespace

bool DocumentsDifferOnlyInStyleBody(std::string_view reference,
                                    std::string_view candidate) {
  size_t ref_open = reference.find(kStyleOpen);
  size_t cand_open = candidate.find(kStyleOpen);
  if (ref_open == std::string_view::npos ||
      cand_open == std::string_view::npos) {
    return false;
  }
  // Exactly one injected block per document, or "the body" is ambiguous.
  if (reference.find(kStyleOpen, ref_open + 1) != std::string_view::npos ||
      candidate.find(kStyleOpen, cand_open + 1) != std::string_view::npos) {
    return false;
  }
  if (reference.substr(0, ref_open) != candidate.substr(0, cand_open)) {
    return false;
  }
  size_t ref_close = reference.find(kStyleClose, ref_open + kStyleOpen.size());
  size_t cand_close =
      candidate.find(kStyleClose, cand_open + kStyleOpen.size());
  if (ref_close == std::string_view::npos ||
      cand_close == std::string_view::npos) {
    return false;
  }
  return reference.substr(ref_close) == candidate.substr(cand_close);
}

ValidationDocuments BuildValidationDocuments(
    std::string_view pre_inline_html, std::string_view full_css,
    std::string_view critical_css, const CascadeLayerOrder& layer_order,
    size_t max_document_bytes,
    const std::vector<CollectedElement>* scanned_elements) {
  if (pre_inline_html.empty()) return Refuse("empty page markup");
  // An empty reference stylesheet renders the same unstyled fold as an empty
  // candidate: the diff would be zero and the page would validate on nothing.
  if (full_css.empty())
    return Refuse("no combined stylesheet to validate against");
  if (critical_css.empty()) return Refuse("no critical block to validate");

  // The serve path strips NUL bytes from the block BEFORE it decides where the
  // block goes (HtmlTransformFilter::InjectCriticalCss), and `@la\0yer` is
  // `@layer` after that. Decide on the same text here, or the two can place
  // the block differently. (The injector strips them again; that is a no-op
  // now.)
  std::string block_without_nul;
  block_without_nul.reserve(critical_css.size());
  for (char c : critical_css) {
    if (c != '\0') block_without_nul.push_back(c);
  }
  critical_css = block_without_nul;
  if (critical_css.empty()) return Refuse("no critical block to validate");

  // Both documents render with script execution disabled, where Blink renders
  // <noscript> content; the served page's fold is a scripting browser's, which
  // never renders it. So the documents leave the <noscript>
  // elements out, contents and all, and that comes FIRST: a <noscript>'s
  // content is raw text to a scripting browser, and reading it as markup (a
  // <style> in it whose </style> comes after the </noscript>) would let the
  // stylesheet removal below run through the page.
  //
  // FAIL CLOSED. A removal that could not be trusted (NoscriptStripResult::
  // reliable), or that the scanner's independent reading of the same page
  // disagrees with, could have cut the page short, and a blank reference
  // compares equal to a blank candidate. No comparison is made.
  NoscriptStripResult rendered = StripNoscriptElements(pre_inline_html);
  if (!rendered.reliable) {
    return Refuse(absl::StrCat("the <noscript> removal could not be trusted: ",
                               rendered.unreliable_reason));
  }
  if (rendered.removed > 0 && scanned_elements != nullptr &&
      !NoscriptStripAgreesWithScan(rendered, *scanned_elements)) {
    return Refuse(
        "the <noscript> removal and the HTML scanner read the page "
        "differently");
  }

  if (rendered.removed > 0 &&
      NoscriptCrossesALexerComment(rendered.removed_ranges,
                                   LexerCommentExtents(pre_inline_html))) {
    return Refuse(
        "a removed <noscript> and an HTML comment, as the serve path reads "
        "it, overlap; the block placement cannot be replayed");
  }

  // The placement is still the serve path's, decided on the page with its
  // <noscript> elements: a head-prelude element inside one counts there (the
  // parity test pins it), so it counts here, where that <noscript> stood.
  std::vector<size_t> noscript_preludes;
  for (const auto& [begin, end] : rendered.removed_ranges) {
    if (NoscriptHoldsPrelude(pre_inline_html.substr(begin, end - begin))) {
      noscript_preludes.push_back(rendered.MapOffset(begin));
    }
  }

  StripResult stripped =
      StripStylesheetSources(rendered.html, noscript_preludes);
  if (!stripped.ok) return Refuse(std::move(stripped.error));

  // Where the serve path puts it (StripStylesheetSources / PlaceBlock), so both
  // documents carry their one block exactly where the served page carries the
  // inlined one.
  // The serve path decides on the block it inlines — the candidate — so both
  // documents take the candidate's answer (DecideCriticalCssLayerPlacement): a
  // layered block goes first only with the page's proven layer order in front
  // of it, and otherwise keeps the </head> fallback. The statement goes in
  // front of BOTH documents' CSS, so the reference carries the author's layer
  // order as well, and the two still differ only in the style body.
  const CriticalCssLayerPlacement layers =
      DecideCriticalCssLayerPlacement(critical_css, layer_order);
  const size_t pos = layers.keep_fallback
                         ? CriticalCssFallbackOffset(stripped.html)
                         : stripped.injection_pos;

  const std::string reference_css = absl::StrCat(layers.prefix, full_css);
  const std::string candidate_css = absl::StrCat(layers.prefix, critical_css);

  // The serve path refuses a block holding, outside its strings and comments,
  // a `<` that could open markup (HtmlTransformFilter::InjectCriticalCss), so
  // there is no served block to validate. Only the candidate: the reference
  // is never served, and a page's own sheets may carry `<!--` from an old
  // inline <style>.
  if (CssOpensMarkupOutsideStrings(candidate_css)) {
    return Refuse(
        "critical CSS injection refused: the block holds text a browser "
        "could read as markup");
  }

  const size_t largest_css = reference_css.size() > candidate_css.size()
                                 ? reference_css.size()
                                 : candidate_css.size();
  if (stripped.html.size() + largest_css + kStyleOpen.size() +
          kStyleClose.size() >
      max_document_bytes) {
    return Refuse("validation document too large to render");
  }

  CssInjectionResult reference =
      InjectCriticalCssAt(stripped.html, pos, reference_css);
  CssInjectionResult candidate =
      InjectCriticalCssAt(stripped.html, pos, candidate_css);

  // The injector writes each `<` in a CSS string or comment as a four-byte
  // escape, as the serve path does, so the documents can come out larger than
  // the estimate above.
  for (const auto* r : {&reference, &candidate}) {
    if (r->html.size() > max_document_bytes) {
      return Refuse("validation document too large to render");
    }
  }

  // The injector reports its `</style` refusal as success=TRUE with an EMPTY
  // document (html_css_injector.cc). Checking `success` alone would synthesize
  // a blank page and diff it against a real one.
  for (const auto* r : {&reference, &candidate}) {
    if (!r->success || !r->injected || r->html.empty()) {
      return Refuse(absl::StrCat("critical CSS injection refused: ",
                                 r->error_message.empty()
                                     ? "no injection performed"
                                     : r->error_message));
    }
  }

  if (!DocumentsDifferOnlyInStyleBody(reference.html, candidate.html)) {
    return Refuse(
        "reference and candidate differ outside the injected style body");
  }

  ValidationDocuments out;
  out.ok = true;
  out.reference = std::move(reference.html);
  out.candidate = std::move(candidate.html);
  out.stylesheet_links_removed = stripped.links_removed;
  out.style_blocks_removed = stripped.styles_removed;
  out.noscript_elements_removed = rendered.removed;
  return out;
}

void ValidateCriticalCss(VisualRegressionGate* gate,
                         const ValidationDocuments& docs,
                         uint32_t viewport_width, uint32_t viewport_height,
                         float threshold,
                         std::function<void(ValidationVerdict)> callback) {
  if (!docs.ok) {
    callback(NotValidated(
        absl::StrCat("no comparable documents: ",
                     docs.error.empty() ? "not built" : docs.error)));
    return;
  }
  if (gate == nullptr) {
    callback(NotValidated("no browser available to validate against"));
    return;
  }

  // The gate is BORROWED, and deliberately NOT kept alive by this callback.
  // Its owner tears it down together with the CDP client it was built on; a
  // validation that outlived that teardown would hold a freed client through a
  // 60 s timer, and once the client is gone that timer is the only thing left
  // that can complete the capture. Cancellation arrives here as a refusal,
  // which is the correct answer: a browser that went away confirmed nothing.
  gate->Compare(
      docs.reference, docs.candidate, viewport_width, viewport_height,
      [callback =
           std::move(callback)](absl::StatusOr<RegressionResult> result) {
        if (!result.ok()) {
          callback(NotValidated(
              absl::StrCat("validation render failed: ",
                           std::string(result.status().message()))));
          return;
        }
        // "Nothing differed because nothing was compared" is not a
        // measurement. CompareScreenshots reports passed=true with
        // total_pixels==0 for a degenerate compare region.
        if (result->total_pixels == 0) {
          callback(NotValidated("validation compared no pixels"));
          return;
        }
        // A blank reference (the whole fold one colour) matches a blank
        // candidate: two documents cut short, or a page whose fold paints
        // nothing without its subresources, would confirm any block. Not a
        // measurement either.
        if (result->reference_uniform) {
          callback(NotValidated(
              "the reference render is a single uniform colour, so a "
              "matching candidate proves nothing"));
          return;
        }
        ValidationVerdict verdict;
        verdict.diff_ratio = result->diff_ratio;
        verdict.validated = result->passed;
        if (!verdict.validated) {
          verdict.failure_reason =
              "above-the-fold appearance changed beyond the allowed threshold";
        }
        callback(std::move(verdict));
      },
      threshold);
}

}  // namespace pagespeed
