// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

// PageSpeed 2.0 - <noscript> removal for the browser-analysis renders.
//
// See the header for the rule this implements and why. The state names below
// are the HTML Standard's (13.2.5 Tokenization, 13.2.6 Tree construction).

#include "src/worker/noscript_strip.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <string>
#include <string_view>
#include <vector>

#include "src/worker/html_css_injector.h"
#include "src/worker/html_scanner.h"

namespace pagespeed {

namespace {

constexpr size_t npos = std::string_view::npos;

// Beyond this many open elements the tree model gives up and the result is
// marked unreliable. The implied end tags handled below keep ordinary pages
// far under it.
constexpr size_t kMaxOpenElements = 8192;

bool IsSpace(char c) {
  return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f';
}

bool IsAlpha(char c) {
  return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
}

char Lower(char c) {
  return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
}

bool EqualsIgnoreCase(std::string_view a, std::string_view b) {
  if (a.size() != b.size()) return false;
  for (size_t i = 0; i < a.size(); ++i) {
    if (Lower(a[i]) != Lower(b[i])) return false;
  }
  return true;
}

template <size_t N>
bool OneOf(std::string_view name, const std::string_view (&list)[N]) {
  return std::find(std::begin(list), std::end(list), name) != std::end(list);
}

struct Attr {
  std::string_view name;
  std::string_view value;
};

struct Tag {
  // Closed by its '>' before the end of the input.
  bool ok = false;
  bool end_tag = false;
  bool self_closing = false;
  std::string name;  // lowercase
  std::vector<Attr> attrs;
  size_t end = npos;  // one past the '>'

  [[nodiscard]] const Attr* Find(std::string_view attr) const {
    for (const Attr& a : attrs) {
      if (EqualsIgnoreCase(a.name, attr)) return &a;
    }
    return nullptr;
  }
};

// html[pos] is '<', followed by an ASCII letter or by '/' and one. The tag
// there, tokenized per the spec: tag name, then the attribute states. A quote
// opens a value only in the before-attribute-value state, right after `=`;
// anywhere else it is part of a name or of an unquoted value.
Tag ScanTag(std::string_view html, size_t pos) {
  Tag t;
  const size_t n = html.size();
  size_t i = pos + 1;
  if (i < n && html[i] == '/') {
    t.end_tag = true;
    ++i;
  }
  const size_t name_start = i;
  while (i < n && !IsSpace(html[i]) && html[i] != '/' && html[i] != '>') ++i;
  t.name.reserve(i - name_start);
  for (size_t k = name_start; k < i; ++k) t.name.push_back(Lower(html[k]));

  enum class S : uint8_t {
    kBeforeName,
    kName,
    kAfterName,
    kBeforeValue,
    kDoubleQuoted,
    kSingleQuoted,
    kUnquoted,
    kAfterQuoted,
    kSelfClosing,
  };
  S s = S::kBeforeName;
  size_t attr_start = 0;
  size_t value_start = 0;
  std::string_view attr_name;
  auto finish = [&](size_t at) {
    t.ok = true;
    t.end = at + 1;
  };
  for (; i < n; ++i) {
    const char c = html[i];
    switch (s) {
      case S::kBeforeName:
        if (IsSpace(c)) break;
        if (c == '/') {
          s = S::kSelfClosing;
          break;
        }
        if (c == '>') {
          finish(i);
          return t;
        }
        attr_start = i;  // a leading '=' is part of the name
        s = S::kName;
        break;
      case S::kName:
        if (IsSpace(c) || c == '/' || c == '>' || c == '=') {
          attr_name = html.substr(attr_start, i - attr_start);
          if (c == '=') {
            s = S::kBeforeValue;
          } else {
            s = S::kAfterName;
            --i;  // reconsume
          }
        }
        break;
      case S::kAfterName:
        if (IsSpace(c)) break;
        if (c == '=') {
          s = S::kBeforeValue;
          break;
        }
        t.attrs.push_back({attr_name, {}});
        if (c == '/') {
          s = S::kSelfClosing;
          break;
        }
        if (c == '>') {
          finish(i);
          return t;
        }
        attr_start = i;
        s = S::kName;
        break;
      case S::kBeforeValue:
        if (IsSpace(c)) break;
        if (c == '"' || c == '\'') {
          value_start = i + 1;
          s = c == '"' ? S::kDoubleQuoted : S::kSingleQuoted;
          break;
        }
        if (c == '>') {
          t.attrs.push_back({attr_name, {}});
          finish(i);
          return t;
        }
        value_start = i;
        s = S::kUnquoted;
        break;
      case S::kDoubleQuoted:
      case S::kSingleQuoted:
        if (c == (s == S::kDoubleQuoted ? '"' : '\'')) {
          t.attrs.push_back(
              {attr_name, html.substr(value_start, i - value_start)});
          s = S::kAfterQuoted;
        }
        break;
      case S::kUnquoted:
        if (IsSpace(c) || c == '>') {
          t.attrs.push_back(
              {attr_name, html.substr(value_start, i - value_start)});
          if (c == '>') {
            finish(i);
            return t;
          }
          s = S::kBeforeName;
        }
        break;
      case S::kAfterQuoted:
        if (IsSpace(c)) {
          s = S::kBeforeName;
          break;
        }
        if (c == '/') {
          s = S::kSelfClosing;
          break;
        }
        if (c == '>') {
          finish(i);
          return t;
        }
        s = S::kBeforeName;
        --i;  // reconsume
        break;
      case S::kSelfClosing:
        if (c == '>') {
          t.self_closing = true;
          finish(i);
          return t;
        }
        s = S::kBeforeName;
        --i;  // reconsume
        break;
    }
  }
  return t;  // the input ended inside the tag
}

// html[pos..] is "<!--". One past the comment's end as the HTML tokenizer
// reads it, or npos when it runs to the end of the input: the comment start
// states end `<!-->` and `<!--->` at once, and otherwise the first `-->` or
// `--!>` ends it. The serve path's HtmlLexer reads comments the same way, so
// this is html_scan::ScanComment; noscript_strip_test pins the browser's
// reading and html_css_injector_test the lexer's.
size_t CommentEnd(std::string_view html, size_t pos) {
  return html_scan::ScanComment(html, pos);
}

// At html[i]: `</name` followed by whitespace, `/` or `>`, the "appropriate
// end tag" that leaves raw text, RCDATA or script data.
bool AppropriateEndTagAt(std::string_view html, size_t i,
                         std::string_view name) {
  const size_t after = i + 2 + name.size();
  if (after >= html.size() || html[i] != '<' || html[i + 1] != '/') {
    return false;
  }
  if (!EqualsIgnoreCase(html.substr(i + 2, name.size()), name)) return false;
  const char c = html[after];
  return IsSpace(c) || c == '/' || c == '>';
}

// The offset of the end tag closing raw text or RCDATA content that starts at
// `from`, or npos.
size_t RawTextClose(std::string_view html, size_t from, std::string_view name) {
  for (size_t i = html.find("</", from); i != npos;
       i = html.find("</", i + 1)) {
    if (AppropriateEndTagAt(html, i, name)) return i;
  }
  return npos;
}

// The offset of the </script> closing script data that starts at `i`, or
// npos. Follows the script data states, including the escaped (after `<!--`)
// and double-escaped (after `<!--<script `) ones: in the latter a </script>
// only returns to escaped, and does not close the element.
size_t ScriptDataClose(std::string_view html, size_t i) {
  enum class S : uint8_t {
    kData,
    kEscaped,
    kEscapedDash,
    kEscapedDashDash,
    kDouble,
    kDoubleDash,
    kDoubleDashDash,
  };
  const size_t n = html.size();
  // Letters from html[j]: their end.
  auto letters_end = [&](size_t j) {
    while (j < n && IsAlpha(html[j])) ++j;
    return j;
  };
  auto is_script_then_delim = [&](size_t from, size_t to) {
    return to - from == 6 && EqualsIgnoreCase(html.substr(from, 6), "script") &&
           to < n && (IsSpace(html[to]) || html[to] == '/' || html[to] == '>');
  };
  S s = S::kData;
  for (; i < n; ++i) {
    const char c = html[i];
    switch (s) {
      case S::kData:
        if (c == '<') {
          if (AppropriateEndTagAt(html, i, "script")) return i;
          if (html.compare(i, 4, "<!--") == 0) {
            i += 3;
            s = S::kEscapedDashDash;
          }
        }
        break;
      case S::kEscaped:
      case S::kEscapedDash:
      case S::kEscapedDashDash:
        if (c == '-') {
          s = s == S::kEscaped ? S::kEscapedDash : S::kEscapedDashDash;
          break;
        }
        if (c == '>' && s == S::kEscapedDashDash) {
          s = S::kData;
          break;
        }
        if (c == '<') {
          if (AppropriateEndTagAt(html, i, "script")) return i;
          if (i + 1 < n && IsAlpha(html[i + 1])) {
            const size_t end = letters_end(i + 1);
            s = is_script_then_delim(i + 1, end) ? S::kDouble : S::kEscaped;
            i = end - 1;
            break;
          }
        }
        s = S::kEscaped;
        break;
      case S::kDouble:
      case S::kDoubleDash:
      case S::kDoubleDashDash:
        if (c == '-') {
          s = s == S::kDouble ? S::kDoubleDash : S::kDoubleDashDash;
          break;
        }
        if (c == '>' && s == S::kDoubleDashDash) {
          s = S::kData;
          break;
        }
        if (c == '<' && i + 1 < n && html[i + 1] == '/') {
          const size_t end = letters_end(i + 2);
          s = is_script_then_delim(i + 2, end) ? S::kEscaped : S::kDouble;
          i = end - 1;
          break;
        }
        s = S::kDouble;
        break;
    }
  }
  return npos;
}

// One past the end tag closing a raw-text-like element whose content starts
// at `from`, or npos when the content runs to the end of the input.
size_t TextElementEnd(std::string_view html, size_t from,
                      std::string_view name) {
  const size_t close = name == "script" ? ScriptDataClose(html, from)
                                        : RawTextClose(html, from, name);
  if (close == npos) return npos;
  const Tag end = ScanTag(html, close);
  return end.ok ? end.end : npos;
}

// True when html[from..] holds a start tag.
bool HasStartTagAfter(std::string_view html, size_t from) {
  for (size_t i = html.find('<', from); i != npos; i = html.find('<', i + 1)) {
    if (i + 1 < html.size() && IsAlpha(html[i + 1])) return true;
  }
  return false;
}

// RAWTEXT and RCDATA elements in HTML content, and script. <noscript> is
// handled on its own; <plaintext> runs to the end.
constexpr std::string_view kTextElements[] = {"script",  "style",   "textarea",
                                              "title",   "xmp",     "iframe",
                                              "noembed", "noframes"};

constexpr std::string_view kVoidElements[] = {
    "area",  "base",  "basefont", "bgsound", "br",    "col",    "embed",
    "frame", "hr",    "image",    "img",     "input", "keygen", "link",
    "meta",  "param", "source",   "track",   "wbr"};

// Start tags that leave foreign content (13.2.6.5); <font> does too when it
// has a color, face or size attribute.
constexpr std::string_view kBreakoutTags[] = {
    "b",      "big",    "blockquote", "body",    "br",    "center", "code",
    "dd",     "div",    "dl",         "dt",      "em",    "embed",  "h1",
    "h2",     "h3",     "h4",         "h5",      "h6",    "head",   "hr",
    "i",      "img",    "li",         "listing", "menu",  "meta",   "nobr",
    "ol",     "p",      "pre",        "ruby",    "s",     "small",  "span",
    "strong", "strike", "sub",        "sup",     "table", "tt",     "u",
    "ul",     "var"};

// Start tags that close an open <p> ("close a p element").
constexpr std::string_view kClosesP[] = {
    "address", "article",  "aside",   "blockquote", "center",   "details",
    "dialog",  "dir",      "div",     "dl",         "fieldset", "figcaption",
    "figure",  "footer",   "form",    "h1",         "h2",       "h3",
    "h4",      "h5",       "h6",      "header",     "hgroup",   "hr",
    "listing", "main",     "menu",    "nav",        "ol",       "p",
    "pre",     "search",   "section", "summary",    "table",    "ul",
    "xmp",     "plaintext"};

// The HTML elements in the spec's "special" category (13.2.4.2), headings
// aside (IsHeading).
constexpr std::string_view kSpecial[] = {
    "address",    "applet",   "area",       "article",  "aside",   "base",
    "basefont",   "bgsound",  "blockquote", "body",     "br",      "button",
    "caption",    "center",   "col",        "colgroup", "dd",      "details",
    "dir",        "div",      "dl",         "dt",       "embed",   "fieldset",
    "figcaption", "figure",   "footer",     "form",     "frame",   "frameset",
    "head",       "header",   "hgroup",     "hr",       "html",    "iframe",
    "img",        "input",    "keygen",     "li",       "link",    "listing",
    "main",       "marquee",  "menu",       "meta",     "nav",     "noembed",
    "noframes",   "noscript", "object",     "ol",       "p",       "param",
    "plaintext",  "pre",      "script",     "search",   "section", "select",
    "source",     "style",    "summary",    "table",    "tbody",   "td",
    "template",   "textarea", "tfoot",      "th",       "thead",   "title",
    "tr",         "track",    "ul",         "wbr",      "xmp"};

// HTML elements that end the default scope.
constexpr std::string_view kScopeBoundaries[] = {
    "applet", "caption", "html",   "table",   "td",
    "th",     "marquee", "object", "template"};

// End tags that close their element when it is in scope, and are ignored
// otherwise.
constexpr std::string_view kBlockEndTags[] = {
    "address",  "applet",     "article", "aside",   "blockquote", "button",
    "center",   "details",    "dialog",  "dir",     "div",        "dl",
    "fieldset", "figcaption", "figure",  "footer",  "header",     "hgroup",
    "listing",  "main",       "marquee", "menu",    "nav",        "object",
    "ol",       "pre",        "search",  "section", "summary",    "ul"};

// Formatting elements: their end tags run the adoption agency algorithm.
constexpr std::string_view kFormattingElements[] = {
    "a",    "b", "big",   "code",   "em",     "font", "i",
    "nobr", "s", "small", "strike", "strong", "tt",   "u"};

// Where the search for an open <li>, <dd> or <dt> to close stops.
constexpr std::string_view kListScopeStops[] = {
    "ul", "ol", "dl", "menu", "table", "td", "th", "body", "html", "template"};

// Attributes that mark a node the worker injected; HtmlScanner does not
// collect those.
constexpr std::string_view kWorkerMarkers[] = {
    "data-pagespeed-critical", "data-pagespeed-hint",
    "data-pagespeed-async-fallback", "data-pagespeed-async-loader"};

enum class Ns : uint8_t { kHtml, kSvg, kMath };

struct Open {
  std::string name;
  Ns ns = Ns::kHtml;
  bool html_ip = false;  // HTML integration point
  bool text_ip = false;  // MathML text integration point
  bool inert = false;    // HtmlScanner does not collect what is inside
};

class Stripper {
 public:
  explicit Stripper(std::string_view html) : html_(html) {
    out_.html.reserve(html.size());
  }

  NoscriptStripResult Run() {
    const size_t n = html_.size();
    while (pos_ < n) {
      const size_t lt = html_.find('<', pos_);
      if (lt == npos) break;
      CopyTo(lt);
      const char next = lt + 1 < n ? html_[lt + 1] : '\0';
      if (html_.compare(lt, 4, "<!--") == 0) {
        if (!CopyThrough(CommentEnd(html_, lt))) break;
        continue;
      }
      if (next == '!') {
        size_t end = npos;
        if (InForeign() && html_.compare(lt, 9, "<![CDATA[") == 0) {
          end = html_.find("]]>", lt + 9);
          if (end != npos) end += 3;
        } else {
          end = html_.find('>', lt + 2);  // doctype or bogus comment
          if (end != npos) ++end;
          if (!seen_tag_ &&
              EqualsIgnoreCase(html_.substr(lt, std::min<size_t>(9, n - lt)),
                               "<!doctype")) {
            quirks_ = false;
          }
        }
        if (!CopyThrough(end)) break;
        continue;
      }
      if (next == '?' ||
          (next == '/' && lt + 2 < n && !IsAlpha(html_[lt + 2]))) {
        // A bogus comment, or `</>` which is dropped: either way up to '>'.
        size_t end = html_.find('>', lt + 2);
        if (!CopyThrough(end == npos ? npos : end + 1)) break;
        continue;
      }
      if (!IsAlpha(next) && !(next == '/' && lt + 2 < n)) {
        CopyTo(lt + 1);  // a '<' that starts nothing is text
        continue;
      }
      Tag tag = ScanTag(html_, lt);
      seen_tag_ = true;
      if (tag.end_tag) {
        if (!CopyThrough(tag.ok ? tag.end : npos)) break;
        EndTag(tag.name);
        continue;
      }
      if (!StartTag(lt, tag)) break;
    }
    CopyTo(n);
    return std::move(out_);
  }

 private:
  // Copies the input up to `to`.
  void CopyTo(size_t to) {
    if (to > pos_) out_.html.append(html_.substr(pos_, to - pos_));
    pos_ = std::max(pos_, to);
  }
  // Copies the input up to `end`, or all of it when `end` is npos (then
  // returns false: nothing after it is markup).
  bool CopyThrough(size_t end) {
    if (end == npos) {
      CopyTo(html_.size());
      return false;
    }
    CopyTo(end);
    return true;
  }

  void Unreliable(std::string reason) {
    if (out_.reliable) {
      out_.reliable = false;
      out_.unreliable_reason = std::move(reason);
    }
  }

  [[nodiscard]] bool InForeign() const {
    return !stack_.empty() && stack_.back().ns != Ns::kHtml;
  }
  [[nodiscard]] bool ParentInert() const {
    return !stack_.empty() && stack_.back().inert;
  }

  // Which rules a start tag is processed with (the tree construction
  // dispatcher): the insertion mode's, i.e. HTML, or foreign content's.
  [[nodiscard]] bool HtmlRulesFor(const Tag& tag) const {
    if (stack_.empty()) return true;
    const Open& top = stack_.back();
    if (top.ns == Ns::kHtml || top.html_ip) return true;
    if (top.text_ip && tag.name != "mglyph" && tag.name != "malignmark") {
      return true;
    }
    return top.ns == Ns::kMath && top.name == "annotation-xml" &&
           tag.name == "svg";
  }

  void Count(const Tag& tag, bool self_inert) {
    if (self_inert || ParentInert()) return;
    for (std::string_view marker : kWorkerMarkers) {
      if (tag.Find(marker) != nullptr) return;
    }
    ++out_.rendered_start_tags;
    out_.rendered_tags.push_back(tag.name);
  }

  void Push(Open open) {
    if (stack_.size() >= kMaxOpenElements) {
      Unreliable("elements nested too deeply to follow");
      return;
    }
    open.inert = open.inert || ParentInert();
    stack_.push_back(std::move(open));
  }

  // Pops foreign elements until the current node is HTML or an integration
  // point (a breakout, 13.2.6.5).
  void PopForeign() {
    while (!stack_.empty() && stack_.back().ns != Ns::kHtml &&
           !stack_.back().html_ip && !stack_.back().text_ip) {
      stack_.pop_back();
    }
  }

  // The implied end tags that matter for keeping the stack from growing on
  // pages that leave <p>, <li>, <option> and table cells open.
  void ImplyEndTags(std::string_view name) {
    auto top_is = [&](std::string_view n) {
      return !stack_.empty() && stack_.back().ns == Ns::kHtml &&
             stack_.back().name == n;
    };
    // "Close a p element": a <p> in button scope closes, with everything
    // opened after it, for the start tags that close one, and
    // for <li>, <dd> and <dt>.
    // (In quirks mode, which a document without a doctype is in, <table>
    // does not.)
    if ((OneOf(name, kClosesP) && !(name == "table" && quirks_)) ||
        name == "li" || name == "dd" || name == "dt") {
      const size_t p = InScope(
          [](const Open& o) { return o.ns == Ns::kHtml && o.name == "p"; },
          Scope::kButton);
      if (p != npos) stack_.resize(p);
    }
    // An <a> or <nobr> inside an open one: the adoption agency closes the
    // first (13.2.6.4.7). A <button> inside an open one closes it.
    if (name == "a" || name == "nobr") {
      for (size_t i = stack_.size(); i-- > 0;) {
        const Open& o = stack_[i];
        if (o.ns == Ns::kHtml && o.name == name) {
          FormattingEndTag(std::string(name));
          break;
        }
        // Markers: a formatting element past one is not in the list.
        if (o.ns != Ns::kHtml || o.name == "td" || o.name == "th" ||
            o.name == "caption" || o.name == "template" || o.name == "applet" ||
            o.name == "object" || o.name == "marquee") {
          break;
        }
      }
    }
    if (name == "button") {
      const size_t b = InScope(
          [](const Open& o) { return o.ns == Ns::kHtml && o.name == "button"; },
          Scope::kDefault);
      if (b != npos) stack_.resize(b);
    }
    if (IsHeading(name) && !stack_.empty() && stack_.back().ns == Ns::kHtml &&
        IsHeading(stack_.back().name)) {
      stack_.pop_back();
    }
    if (name == "li" || name == "dd" || name == "dt") {
      for (size_t i = stack_.size(); i-- > 0;) {
        const Open& o = stack_[i];
        if (o.ns != Ns::kHtml) break;
        if (o.name == name ||
            (name != "li" && (o.name == "dd" || o.name == "dt"))) {
          stack_.resize(i);
          break;
        }
        if (OneOf(std::string_view(o.name), kListScopeStops)) break;
      }
    }
    if ((name == "option" || name == "optgroup") && top_is("option")) {
      stack_.pop_back();
    }
    if (name == "optgroup" && top_is("optgroup")) stack_.pop_back();
    if (name == "td" || name == "th" || name == "tr") {
      for (size_t i = stack_.size(); i-- > 0;) {
        const Open& o = stack_[i];
        if (o.ns != Ns::kHtml || o.name == "table") break;
        if (o.name == "td" || o.name == "th" ||
            (name == "tr" && o.name == "tr")) {
          stack_.resize(i);
          break;
        }
      }
    }
  }

  // Returns false when nothing after this tag is markup.
  bool StartTag(size_t lt, const Tag& tag) {
    if (!HtmlRulesFor(tag)) return ForeignStartTag(lt, tag);
    return HtmlStartTag(lt, tag);
  }

  bool HtmlStartTag(size_t lt, const Tag& tag) {
    const std::string& name = tag.name;
    if (name == "noscript") return RemoveNoscript(lt, tag);
    if (!tag.ok) return CopyThrough(npos);
    const bool text = OneOf(name, kTextElements);
    Count(tag, /*self_inert=*/name == "template" || name == "noembed" ||
                   name == "noframes");
    CopyTo(tag.end);
    // A <form> while the form element pointer is set is ignored outright:
    // it closes nothing either, not even an open <p>. The pointer outlives
    // the element when an ancestor's end tag pops it.
    if (name == "form" && form_pointer_ && !InTemplate()) return true;
    // Before the element is inserted, whatever kind it is: <hr>, <xmp>,
    // <plaintext> and <table> close an open <p> too.
    ImplyEndTags(name);
    if (name == "plaintext") return CopyThrough(npos);
    if (text) return CopyThrough(TextElementEnd(html_, tag.end, name));
    if (name == "svg" || name == "math") {
      if (!tag.self_closing) {
        Push({name, name == "svg" ? Ns::kSvg : Ns::kMath, false, false, false});
      }
      return true;
    }
    if (OneOf(name, kVoidElements)) return true;
    if (name == "form" && !InTemplate()) form_pointer_ = true;
    Push({name, Ns::kHtml, false, false, name == "template"});
    return true;
  }

  [[nodiscard]] bool InTemplate() const {
    for (const Open& o : stack_) {
      if (o.ns == Ns::kHtml && o.name == "template") return true;
    }
    return false;
  }

  bool ForeignStartTag(size_t lt, const Tag& tag) {
    const std::string& name = tag.name;
    const bool breakout = OneOf(name, kBreakoutTags) ||
                          (name == "font" && (tag.Find("color") != nullptr ||
                                              tag.Find("face") != nullptr ||
                                              tag.Find("size") != nullptr));
    if (breakout) {
      PopForeign();
      return StartTag(lt, tag);
    }
    if (!tag.ok) return CopyThrough(npos);
    const Ns ns = stack_.back().ns;
    // HtmlScanner (the HtmlLexer knows no namespaces) leaves these, and what
    // they hold, out of its elements in any namespace: an <svg><template> of
    // React/Next.js streaming markup included. Mirrored here for
    // the cross-check; it changes no removal.
    const bool inert = name == "noscript" || name == "template" ||
                       name == "noembed" || name == "noframes";
    Count(tag, /*self_inert=*/inert);
    CopyTo(tag.end);
    if (tag.self_closing) return true;
    Open open{name, ns, false, false, inert};
    if (ns == Ns::kSvg) {
      open.html_ip =
          name == "foreignobject" || name == "desc" || name == "title";
    } else {
      open.text_ip = name == "mi" || name == "mo" || name == "mn" ||
                     name == "ms" || name == "mtext";
      if (name == "annotation-xml") {
        const Attr* enc = tag.Find("encoding");
        open.html_ip = enc != nullptr &&
                       (EqualsIgnoreCase(enc->value, "text/html") ||
                        EqualsIgnoreCase(enc->value, "application/xhtml+xml"));
      }
    }
    Push(std::move(open));
    return true;
  }

  void EndTag(const std::string& name) {
    if (!InForeign()) {
      HtmlEndTag(name);
      return;
    }
    if (name == "br" || name == "p") {
      PopForeign();
      HtmlEndTag(name);
      return;
    }
    for (size_t i = stack_.size(); i-- > 0;) {
      if (stack_[i].ns == Ns::kHtml) {
        HtmlEndTag(name);
        return;
      }
      if (EqualsIgnoreCase(stack_[i].name, name)) {
        stack_.resize(i);
        return;
      }
    }
  }

  // The in-body rules for an end tag (13.2.6.4.7), as far as they decide
  // which elements are open: enough to know when an <svg> or <math> is
  // closed, and to keep the stack from drifting on misnested markup.
  void HtmlEndTag(const std::string& name) {
    // </body> and </html> switch the insertion mode; they pop nothing. </br>
    // is a <br>, and a </p> with no <p> open inserts an empty one.
    if (name == "body" || name == "html" || name == "br") return;
    if (IsHeading(name)) {
      // Any open heading in scope closes, whichever level: </h1> closes <h2>.
      const size_t i = InScope([](const Open& o) { return IsHeading(o.name); },
                               Scope::kDefault);
      if (i != npos) stack_.resize(i);
      return;
    }
    if (name == "p" || name == "li" || name == "dd" || name == "dt" ||
        OneOf(std::string_view(name), kBlockEndTags)) {
      const Scope scope = name == "p"    ? Scope::kButton
                          : name == "li" ? Scope::kListItem
                                         : Scope::kDefault;
      const size_t i =
          InScope([&name](const Open& o) { return o.name == name; }, scope);
      if (i != npos) stack_.resize(i);
      return;
    }
    if (name == "form") {
      // </form> removes the form element alone; what was opened inside it
      // stays open (13.2.6.4.7, "An end tag whose tag name is form"). It
      // clears the form element pointer first.
      if (!InTemplate()) form_pointer_ = false;
      const size_t i = InScope(
          [](const Open& o) { return o.ns == Ns::kHtml && o.name == "form"; },
          Scope::kDefault);
      if (i != npos)
        stack_.erase(stack_.begin() + static_cast<std::ptrdiff_t>(i));
      return;
    }
    if (OneOf(std::string_view(name), kFormattingElements)) {
      FormattingEndTag(name);
      return;
    }
    AnyOtherEndTag(name);
  }

  // "Any other end tag": from the current node down, the first HTML element
  // of that name closes, unless a special element comes first, in which case
  // the tag is ignored. So </span> across an open <div> is ignored.
  void AnyOtherEndTag(const std::string& name) {
    for (size_t i = stack_.size(); i-- > 0;) {
      const Open& o = stack_[i];
      if (o.ns == Ns::kHtml && o.name == name) {
        stack_.resize(i);
        return;
      }
      if (IsSpecial(o)) return;
    }
  }

  // The adoption agency algorithm, reduced to its effect on which elements
  // stay open. Without a special element after the formatting element (a
  // furthest block), the formatting element and everything after it close.
  // With one, the algorithm moves a new formatting element in after the
  // furthest block and runs again, so in the end the special elements after
  // the formatting element stay open and whatever follows the last of them
  // closes: <a><div><svg></a> leaves the <div> open and closes the <svg>.
  void FormattingEndTag(const std::string& name) {
    size_t f = npos;
    for (size_t i = stack_.size(); i-- > 0;) {
      if (stack_[i].ns == Ns::kHtml && stack_[i].name == name) {
        f = i;
        break;
      }
    }
    if (f == npos) {
      AnyOtherEndTag(name);
      return;
    }
    if (InScope([f, this](const Open& o) { return &o == &stack_[f]; },
                Scope::kDefault) == npos) {
      return;
    }
    size_t last_special = npos;
    for (size_t i = f + 1; i < stack_.size(); ++i) {
      if (IsSpecial(stack_[i])) last_special = i;
    }
    if (last_special == npos) {
      stack_.resize(f);
      return;
    }
    stack_.resize(last_special + 1);
    stack_.erase(stack_.begin() + static_cast<std::ptrdiff_t>(f));
  }

  enum class Scope : uint8_t { kDefault, kButton, kListItem };

  // The index of the topmost element `match` accepts, if it is "in scope"
  // (13.2.4.2): reached from the current node without passing a scope
  // boundary. npos otherwise.
  template <typename Match>
  [[nodiscard]] size_t InScope(Match match, Scope scope) const {
    for (size_t i = stack_.size(); i-- > 0;) {
      const Open& o = stack_[i];
      if (match(o)) return i;
      if (IsScopeBoundary(o)) return npos;
      if (o.ns == Ns::kHtml &&
          ((scope == Scope::kButton && o.name == "button") ||
           (scope == Scope::kListItem && (o.name == "ol" || o.name == "ul")))) {
        return npos;
      }
    }
    return npos;
  }

  static bool IsHeading(std::string_view name) {
    return name.size() == 2 && name[0] == 'h' && name[1] >= '1' &&
           name[1] <= '6';
  }

  static bool IsScopeBoundary(const Open& o) {
    switch (o.ns) {
      case Ns::kHtml:
        return OneOf(std::string_view(o.name), kScopeBoundaries);
      case Ns::kMath:
        return o.text_ip || o.name == "annotation-xml";
      case Ns::kSvg:
        return o.html_ip;
    }
    return false;
  }

  static bool IsSpecial(const Open& o) {
    if (o.ns != Ns::kHtml) return IsScopeBoundary(o);
    return IsHeading(o.name) || OneOf(std::string_view(o.name), kSpecial);
  }

  // A <noscript> in HTML content: raw text up to the first </noscript>, which
  // closes it. Unclosed, it runs to the end of the input; with markup after
  // its start tag that is either a page that is blank for scripting clients
  // as well, or a reading this tokenizer got wrong, and the result is marked
  // unreliable either way.
  bool RemoveNoscript(size_t lt, const Tag& tag) {
    size_t end = tag.ok ? TextElementEnd(html_, tag.end, "noscript") : npos;
    if (end == npos) {
      end = html_.size();
      if (HasStartTagAfter(html_, lt + 1)) {
        Unreliable(
            "a <noscript> runs to the end of the document with markup after "
            "it");
      }
    }
    out_.removed_ranges.emplace_back(lt, end);
    ++out_.removed;
    pos_ = end;
    return end < html_.size();
  }

  std::string_view html_;
  NoscriptStripResult out_;
  std::vector<Open> stack_;
  size_t pos_ = 0;
  // The form element pointer (13.2.4.4).
  bool form_pointer_ = false;
  // No doctype before the first tag: quirks mode. Any doctype counts as
  // no-quirks here; the difference this model reads (does <table> close an
  // open <p>?) is the same in limited-quirks mode.
  bool quirks_ = true;
  bool seen_tag_ = false;
};

}  // namespace

namespace {

// Myers' O((N+M)D) shortest edit script between the scanner's names `a` and
// the kept names `b`, bounded: false when it needs more than
// 2 * max(3, 1%) edits (capped at kMaxEdits, so a page whose two readings
// diverge costs bounded time), or when any run of edits between two matching
// elements deletes or inserts more than kMaxIsolatedRun elements.
bool DiffRunsAgree(const std::vector<CollectedElement>& scanned,
                   const std::vector<std::string>& kept) {
  constexpr size_t kMaxEdits = 1024;
  auto same = [&](size_t x, size_t y) {
    return EqualsIgnoreCase(scanned[x].tag_name, kept[y]);
  };
  // Common prefix and suffix are matches; diff what is between.
  size_t lo = 0;
  while (lo < scanned.size() && lo < kept.size() && same(lo, lo)) ++lo;
  size_t a_hi = scanned.size();
  size_t b_hi = kept.size();
  while (a_hi > lo && b_hi > lo && same(a_hi - 1, b_hi - 1)) {
    --a_hi;
    --b_hi;
  }
  const auto n = static_cast<std::ptrdiff_t>(a_hi - lo);
  const auto m = static_cast<std::ptrdiff_t>(b_hi - lo);
  if (n == 0 && m == 0) return true;
  const size_t tolerance =
      2 * std::max<size_t>(3, std::max(scanned.size(), kept.size()) / 100);
  const auto max_d =
      static_cast<std::ptrdiff_t>(std::min(tolerance, kMaxEdits));
  auto eq = [&](std::ptrdiff_t x, std::ptrdiff_t y) {
    return same(lo + static_cast<size_t>(x), lo + static_cast<size_t>(y));
  };
  const std::ptrdiff_t offset = max_d + 1;
  std::vector<std::ptrdiff_t> v(static_cast<size_t>(2 * max_d + 3), 0);
  auto at = [&](std::vector<std::ptrdiff_t>& vec,
                std::ptrdiff_t k) -> std::ptrdiff_t& {
    return vec[static_cast<size_t>(k + offset)];
  };
  // trace[d]: v as it was before round d.
  std::vector<std::vector<std::ptrdiff_t>> trace;
  std::ptrdiff_t found_d = -1;
  for (std::ptrdiff_t d = 0; d <= max_d && found_d < 0; ++d) {
    trace.push_back(v);
    for (std::ptrdiff_t k = -d; k <= d; k += 2) {
      std::ptrdiff_t x = (k == -d || (k != d && at(v, k - 1) < at(v, k + 1)))
                             ? at(v, k + 1)
                             : at(v, k - 1) + 1;
      std::ptrdiff_t y = x - k;
      while (x < n && y < m && eq(x, y)) {
        ++x;
        ++y;
      }
      at(v, k) = x;
      if (x >= n && y >= m) {
        found_d = d;
        break;
      }
    }
  }
  if (found_d < 0) return false;
  // Walk back, one edit per round, measuring each run of edits between
  // matches.
  std::ptrdiff_t x = n;
  std::ptrdiff_t y = m;
  size_t deleted = 0;
  size_t inserted = 0;
  size_t deletions = 0;
  // A removal that went wrong only ever deletes: a run may
  // delete at most kMaxDeletionRun elements, and kMaxDeletions in all.
  // Counted gross, not net of the run's insertions: insertions are cheap to
  // come by (a `<!-->` the scanner's lexer reads as one comment and the
  // removal as markup), and netting them would pass a deletion off as an
  // element read as another. Runs of insertions keep the isolated-difference
  // tolerance.
  auto run_ok = [&] {
    deletions += deleted;
    const bool ok = deleted <= kMaxIsolatedRun && inserted <= kMaxIsolatedRun &&
                    deleted <= kMaxDeletionRun && deletions <= kMaxDeletions;
    deleted = inserted = 0;
    return ok;
  };
  for (std::ptrdiff_t d = found_d; d > 0; --d) {
    std::vector<std::ptrdiff_t>& pv = trace[static_cast<size_t>(d)];
    const std::ptrdiff_t k = x - y;
    const std::ptrdiff_t prev_k =
        (k == -d || (k != d && at(pv, k - 1) < at(pv, k + 1))) ? k + 1 : k - 1;
    const std::ptrdiff_t prev_x = at(pv, prev_k);
    const std::ptrdiff_t prev_y = prev_x - prev_k;
    bool matched = false;
    while (x > prev_x && y > prev_y) {
      --x;
      --y;
      matched = true;
    }
    if (matched && !run_ok()) return false;
    if (x == prev_x) {
      ++inserted;  // kept[y-1]: the removal has it, the scanner does not
    } else {
      ++deleted;  // scanned[x-1]: the scanner has it, the removal does not
    }
    x = prev_x;
    y = prev_y;
  }
  return run_ok();
}

}  // namespace

size_t NoscriptStripResult::MapOffset(size_t pos) const {
  size_t shift = 0;
  for (const auto& [begin, end] : removed_ranges) {
    if (pos <= begin) break;
    if (pos < end) return begin - shift;
    shift += end - begin;
  }
  return pos - shift;
}

NoscriptStripResult StripNoscriptElements(std::string_view html) {
  return Stripper(html).Run();
}

bool NoscriptStripAgreesWithScan(const NoscriptStripResult& result,
                                 const std::vector<CollectedElement>& scanned) {
  const std::vector<std::string>& kept = result.rendered_tags;
  // From the first <body> on each side (or the start, without one), the
  // first kExactElementsAfterBody elements must be the same elements, in the
  // same order: that is where the fold is, and a reading that lost or gained
  // an element there has lost or gained part of the page.
  auto body_of = [](auto size, auto name_at) {
    for (size_t i = 0; i < size; ++i) {
      if (EqualsIgnoreCase(name_at(i), "body")) return i;
    }
    return size_t{0};
  };
  const size_t kb = body_of(
      kept.size(), [&](size_t i) -> std::string_view { return kept[i]; });
  const size_t sb = body_of(scanned.size(), [&](size_t i) -> std::string_view {
    return scanned[i].tag_name;
  });
  // <body> itself, then the kExactElementsAfterBody elements after it.
  for (size_t n = 0; n <= kExactElementsAfterBody; ++n) {
    const bool k_end = kb + n >= kept.size();
    const bool s_end = sb + n >= scanned.size();
    if (k_end || s_end) {
      if (k_end != s_end) return false;
      break;
    }
    if (!EqualsIgnoreCase(kept[kb + n], scanned[sb + n].tag_name)) {
      return false;
    }
  }
  // The whole document, aligned name by name: a shortest edit
  // script between the two lists (DiffRunsAgree). A removal that went wrong
  // deletes a contiguous run of elements, wherever the fold is: past the
  // window above too. Isolated differences, where the scanner's HtmlLexer and
  // the browser read a construct differently (an <svg> <title> with markup in
  // it, say), are runs of at most kMaxIsolatedRun elements between matching
  // ones; a longer run on either side is a disagreement, and so are more
  // edits than 2 * max(3, 1%) (an element read as another is two).
  return DiffRunsAgree(scanned, kept);
}

}  // namespace pagespeed
