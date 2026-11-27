// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

#include "lib/html/markdown_extractor_filter.h"

#include <algorithm>
#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

#include "lib/base/string_util.h"
#include "lib/html/compat/message_handler.h"
#include "lib/html/html_element.h"
#include "lib/html/html_metadata_extractor.h"
#include "lib/html/html_name.h"
#include "lib/html/html_node.h"
#include "lib/html/html_parse.h"
#include "lib/html/safe_entity_decode.h"

namespace net_instaweb {

namespace {

// ASCII whitespace only. Text reaching the collapse loop has been through
// SafeDecodeHtmlEntities, which already folded every no-break space (decoded
// or the literal UTF-8 C2 A0) to ' ', so no non-ASCII byte is ever treated as
// whitespace here: doing so would split a multi-byte sequence.
bool IsAsciiWs(char c) {
  return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' ||
         c == '\v';
}

// Entity decode, UTF-8 repair and the NBSP fold live in
// lib/html/safe_entity_decode.h, the single source of truth shared by the
// body-text path here and the <meta description> path in
// html_metadata_extractor.cc.

std::string Trim(std::string_view s) {
  size_t b = 0;
  size_t e = s.size();
  while (b < e && IsAsciiWs(s[b])) ++b;
  while (e > b && IsAsciiWs(s[e - 1])) --e;
  return std::string(s.substr(b, e - b));
}

// Lowercase + strip spaces/tabs, for a substring scan of a style attribute.
std::string StyleNorm(const char* style) {
  std::string out;
  for (const char* p = style; *p != '\0'; ++p) {
    char c = *p;
    if (c == ' ' || c == '\t') continue;
    if (c >= 'A' && c <= 'Z') c = static_cast<char>(c + 32);
    out.push_back(c);
  }
  return out;
}

// Truly-content-free subtrees: always dropped (absolute strip).
bool IsHardStripKeyword(HtmlName::Keyword kw) {
  switch (kw) {
    case HtmlName::kScript:
    case HtmlName::kStyle:
    case HtmlName::kNoscript:
    case HtmlName::kTemplate:
    case HtmlName::kHead:
    // A <select> is interactive form-control UI, never citable prose. The 1.1
    // docs ship a server-rendered mobile-nav <select id="docs-nav"> INSIDE
    // <main> whose ~40 <option> page names would otherwise leak into the feed
    // (the content-root gate admits it because it sits in <main>).
    case HtmlName::kSelect:
      return true;
    default:
      return false;
  }
}

// Hard-strip by element NAME, for tags that have no HtmlName keyword: <svg>
// (graphics, never text) and <label> (form-control label — the "Navigate docs"
// label of the mobile-nav <select> above). Matched like <summary> elsewhere.
bool IsHardStripByName(HtmlElement* el) {
  return StringCaseEqual(el->name_str(), "svg") ||
         StringCaseEqual(el->name_str(), "label");
}

// Soft chrome: dropped as boilerplate by default, but a descendant content root
// (<main>/<article>/role=main) triggers the fail-open re-extract.
bool IsSoftStripKeyword(HtmlName::Keyword kw) {
  switch (kw) {
    case HtmlName::kNav:
    case HtmlName::kHeader:
    case HtmlName::kFooter:
      return true;
    default:
      return false;
  }
}

bool RoleIs(HtmlElement* el, std::string_view want) {
  const char* role = el->AttributeValue(HtmlName::kRole);
  return role != nullptr && StringCaseEqual(role, want);
}

// <main>/<article> or role=main/article — protected content the strip must never
// silently drop (the §4 kill-metric invariant).
bool IsContentRoot(HtmlElement* el) {
  HtmlName::Keyword kw = el->keyword();
  return kw == HtmlName::kMain || kw == HtmlName::kArticle ||
         RoleIs(el, "main") || RoleIs(el, "article");
}

// <article> or role=article — an <article>-scoped <header>/<footer> is content
// (blog title/date/byline), not chrome, so the soft-strip must not drop it
// (Issue F (3)).
bool IsArticleRoot(HtmlElement* el) {
  return el->keyword() == HtmlName::kArticle || RoleIs(el, "article");
}

// Sectioning content or the main content root: a <header> inside one of these
// is that section's heading block (its title, eyebrow and intro text), not the
// page banner, so the soft strip keeps it.
// On a page without <main>/<article>, a template that wraps the whole body in
// a <section> therefore keeps its banner header text (a <nav> in it is still
// dropped); with a content root, text outside it is not emitted at all.
bool IsSectioningScope(HtmlElement* el) {
  const HtmlName::Keyword kw = el->keyword();
  return kw == HtmlName::kSection || kw == HtmlName::kAside ||
         IsArticleRoot(el) || IsContentRoot(el);
}

// Block-level keywords whose structural emission must be flattened away when the
// element is wrapped inside an open <a> (Issue F (5)).
bool IsBlockKeyword(HtmlName::Keyword kw) {
  switch (kw) {
    case HtmlName::kH1:
    case HtmlName::kH2:
    case HtmlName::kH3:
    case HtmlName::kH4:
    case HtmlName::kH5:
    case HtmlName::kH6:
    case HtmlName::kP:
    case HtmlName::kDiv:
    case HtmlName::kSection:
    case HtmlName::kArticle:
    case HtmlName::kMain:
    case HtmlName::kAside:
    case HtmlName::kAddress:
    case HtmlName::kBlockquote:
    case HtmlName::kUl:
    case HtmlName::kOl:
    case HtmlName::kLi:
    case HtmlName::kDl:
    case HtmlName::kDt:
    case HtmlName::kDd:
    case HtmlName::kHr:
      return true;
    default:
      return false;
  }
}

// Block keywords whose child, when it sits directly inside an open <li>, is
// flattened into the item line instead of starting its own block (icon-badge /
// card feature lists). Deliberately EXCLUDES kUl/kOl/kLi (a nested real list
// must still detach) and kPre/kBlockquote/kTable/kHr/headings (genuinely nested
// code/quotes/tables/headings keep their own block).
bool IsListItemFoldKeyword(HtmlName::Keyword kw) {
  switch (kw) {
    case HtmlName::kP:
    case HtmlName::kDiv:
    case HtmlName::kSection:
    case HtmlName::kAside:
      return true;
    default:
      return false;
  }
}

// Inline-hidden via the element's OWN attributes only (SAX has no computed style;
// class-driven hiding is a documented limit). Treated as a hard strip.
bool IsInlineHidden(HtmlElement* el) {
  const char* style = el->AttributeValue(HtmlName::kStyle);
  if (style != nullptr) {
    std::string s = StyleNorm(style);
    if (s.find("display:none") != std::string::npos ||
        s.find("visibility:hidden") != std::string::npos) {
      return true;
    }
  }
  if (el->FindAttribute("hidden") != nullptr) return true;
  const HtmlElement::Attribute* aria = el->FindAttribute("aria-hidden");
  if (aria != nullptr) {
    const char* v = aria->DecodedValueOrNull();
    if (v != nullptr && StringCaseEqual(v, "true")) return true;
  }
  return false;
}

// True when t holds a character a reader can see (not ASCII whitespace or the
// UTF-8 form of NBSP).
bool HasVisibleText(std::string_view t) {
  for (std::size_t i = 0; i < t.size(); ++i) {
    const unsigned char c = static_cast<unsigned char>(t[i]);
    if (c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\f') continue;
    if (c == 0xC2 && i + 1 < t.size() &&
        static_cast<unsigned char>(t[i + 1]) == 0xA0) {
      ++i;
      continue;
    }
    return true;
  }
  return false;
}

// True when the '&' at t[i] begins text a Markdown renderer reads as a
// character reference ("&lt;", "&#60;", "&#x3C;"). Such text came from the
// page as escaped text (for example "&amp;lt;"), so its '&' is escaped to keep
// it literal.
bool StartsMarkdownReference(std::string_view t, std::size_t i) {
  std::size_t j = i + 1;
  if (j < t.size() && t[j] == '#') {
    ++j;
    const bool hex = j < t.size() && (t[j] == 'x' || t[j] == 'X');
    if (hex) ++j;
    const std::size_t start = j;
    while (j < t.size() && j - start < 8 &&
           ((t[j] >= '0' && t[j] <= '9') ||
            (hex &&
             ((t[j] >= 'a' && t[j] <= 'f') || (t[j] >= 'A' && t[j] <= 'F'))))) {
      ++j;
    }
    return j > start && j < t.size() && t[j] == ';';
  }
  const std::size_t start = j;
  while (j < t.size() && j - start < 33 &&
         ((t[j] >= 'a' && t[j] <= 'z') || (t[j] >= 'A' && t[j] <= 'Z') ||
          (t[j] >= '0' && t[j] <= '9'))) {
    ++j;
  }
  // CommonMark reads only the standard's names as references ("&bogus;" is
  // text as it stands).
  return j > start && j < t.size() && t[j] == ';' &&
         IsHtml5EntityName(t.substr(start, j - start));
}

std::string EscapeCell(std::string_view s) {
  std::string out;
  out.reserve(s.size());
  for (char c : s) {
    if (c == '\n' || c == '\r') {
      out.push_back(' ');
    } else if (c == '|') {
      out += "\\|";
    } else {
      out.push_back(c);
    }
  }
  return out;
}

// Percent-encode the markdown-structural bytes in a URL so a hostile (entity-
// decoded) href/src can never break out of `(...)` and forge a new block/link.
// A control byte (CR/LF/etc.) is dropped.
std::string SanitizeUrl(const char* url) {
  std::string out;
  for (const char* p = url; *p != '\0'; ++p) {
    unsigned char c = static_cast<unsigned char>(*p);
    switch (c) {
      case '(':
        out += "%28";
        break;
      case ')':
        out += "%29";
        break;
      case ' ':
        out += "%20";
        break;
      case '<':
        out += "%3C";
        break;
      case '>':
        out += "%3E";
        break;
      case '"':
        out += "%22";
        break;
      case '\\':
        out += "%5C";
        break;
      default:
        if (c >= 0x20 && c != 0x7f) out.push_back(static_cast<char>(c));
        // else: drop control byte
    }
  }
  return out;
}

// Backslash-escape link/image structural chars in attacker text (alt text, etc.)
// and fold CR/LF to a space. The caller passes text already decoded by
// SafeDecodeHtmlEntities (well-formed UTF-8, no-break spaces folded).
std::string EscapeLinkText(std::string_view s) {
  std::string out;
  out.reserve(s.size());
  for (char c : s) {
    switch (c) {
      case '[':
      case ']':
      case '(':
      case ')':
      case '<':   // decoded text, never an HTML tag or autolink
      case '\\':  // a literal backslash, never an escape
        out.push_back('\\');
        out.push_back(c);
        break;
      case '\n':
      case '\r':
        out.push_back(' ');
        break;
      default:
        out.push_back(c);
    }
  }
  return out;
}

int LongestBacktickRun(std::string_view s) {
  int max_run = 0;
  int run = 0;
  for (char c : s) {
    if (c == '`') {
      ++run;
      if (run > max_run) max_run = run;
    } else {
      run = 0;
    }
  }
  return max_run;
}

// Reduce an attacker-influenced <pre data-language> value to a safe info-string
// for a fenced code block's opening line: keep only the leading run of
// language-token chars (letters/digits and the few punctuation chars real Shiki
// grammars use: + # . _ -) and cap the length, so a value like "\n# Injected"
// or "js`)" can never inject a newline or markdown marker past the fence line.
std::string SanitizeFenceLanguage(std::string_view v) {
  constexpr std::size_t kMaxLangLen = 32;
  std::string out;
  for (char c : v) {
    const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                    (c >= '0' && c <= '9') || c == '+' || c == '#' ||
                    c == '.' || c == '_' || c == '-';
    if (!ok || out.size() >= kMaxLangLen) break;
    out.push_back(c);
  }
  return out;
}

// Extract the fence language from a <code class="..."> value: hand-authored
// (non-Shiki) code blocks carry it as a `language-<lang>` class token (the
// CommonMark/highlight.js convention) rather than a <pre data-language>
// attribute. Returns the sanitized <lang> of the first such token, or "".
std::string LanguageFromCodeClass(const char* class_attr) {
  if (class_attr == nullptr) return std::string();
  constexpr std::string_view kPrefix = "language-";
  std::string_view cls(class_attr);
  std::size_t i = 0;
  while (i < cls.size()) {
    while (i < cls.size() && (cls[i] == ' ' || cls[i] == '\t')) ++i;
    std::size_t start = i;
    while (i < cls.size() && cls[i] != ' ' && cls[i] != '\t') ++i;
    std::string_view token = cls.substr(start, i - start);
    if (token.size() > kPrefix.size() &&
        token.substr(0, kPrefix.size()) == kPrefix) {
      return SanitizeFenceLanguage(token.substr(kPrefix.size()));
    }
  }
  return std::string();
}

// Escape a leading block marker in attacker paragraph text so it cannot forge a
// heading/list/quote/table when emitted as a standalone block. (review SF-3.) NOTE:
// '*' is intentionally NOT escaped here — the extractor itself emits '*'/'**' for
// <em>/<strong> at a block start, and FlushBlock cannot tell an emphasis marker
// from literal text. A leading literal '*' (emphasis forgery) is the documented
// residual; '#'/'>'/'|'/'-'/'+' are never emitted inline so a leading one is text.
std::string EscapeLeadingMarker(std::string body) {
  if (body.empty()) return body;
  char c = body[0];
  if (c == '#' || c == '>' || c == '|' || c == '-' || c == '+') {
    body.insert(body.begin(), '\\');
  }
  return body;
}

// Escape a leading block marker at the start of each INTERIOR physical line of a
// list-item body — the char right after an embedded '\n'. A list item's body can
// contain a raw newline (a <br>, or a folded block child carrying a <br>); the
// text after it sits at column 0 and an unescaped '#'/'-'/'>'/'|'/'+' there would
// forge a heading/list/quote/table into the agent feed (the list-item FlushBlock
// branch is the only one that does not prefix every line, so it must escape per
// line). The first line is preceded by the "- "/"N. " marker and is escaped by
// the caller's own logic, so it is left untouched here.
std::string EscapeInteriorLineMarkers(std::string_view body) {
  std::string out;
  out.reserve(body.size() + 4);
  for (std::size_t i = 0; i < body.size(); ++i) {
    const char c = body[i];
    out.push_back(c);
    if (c == '\n' && i + 1 < body.size()) {
      const char n = body[i + 1];
      if (n == '#' || n == '>' || n == '|' || n == '-' || n == '+') {
        out.push_back('\\');
      }
    }
  }
  return out;
}

// A fenced code block around `body`, with a fence longer than any backtick run
// in it so no content line can close the block early.
std::string FencedBlock(std::string_view body, std::string_view lang) {
  const int n = LongestBacktickRun(body);
  const std::string fence(std::max(3, n + 1), '`');
  std::string out = fence;
  out.append(lang);
  out.push_back('\n');
  out.append(body);
  out.push_back('\n');
  out += fence;
  return out;
}

// Tidy a <pre> whose lines are block children (a diff view renders one <div>
// per line): drop the whitespace-only lines the source indentation leaves
// between the children, strip trailing blanks, and remove the indentation the
// remaining lines share.
std::string TidyBlockLinePre(std::string_view body) {
  std::vector<std::string_view> lines;
  std::size_t start = 0;
  while (start <= body.size()) {
    std::size_t nl = body.find('\n', start);
    if (nl == std::string_view::npos) nl = body.size();
    std::string_view line = body.substr(start, nl - start);
    while (!line.empty() &&
           (line.back() == ' ' || line.back() == '\t' || line.back() == '\r')) {
      line.remove_suffix(1);
    }
    if (!line.empty()) lines.push_back(line);
    start = nl + 1;
  }
  std::size_t indent = std::string_view::npos;
  for (std::string_view line : lines) {
    std::size_t i = 0;
    while (i < line.size() && (line[i] == ' ' || line[i] == '\t')) ++i;
    indent = std::min(indent, i);
  }
  std::string out;
  for (std::string_view line : lines) {
    if (!out.empty()) out.push_back('\n');
    out.append(line.substr(indent));
  }
  return out;
}

// Trim leading and trailing whitespace-only lines (and trailing blanks) from a
// code block's text, keeping the indentation of its first content line.
std::string TrimCodeLines(std::string_view s) {
  std::size_t b = 0;
  for (std::size_t i = 0; i < s.size(); ++i) {
    if (s[i] == '\n') {
      b = i + 1;
    } else if (s[i] != ' ' && s[i] != '\t' && s[i] != '\r') {
      break;
    }
  }
  std::size_t e = s.size();
  while (e > b && IsAsciiWs(s[e - 1])) --e;
  return std::string(s.substr(b, e - b));
}

}  // namespace

MarkdownExtractorFilter::MarkdownExtractorFilter(bool strip_soft_chrome,
                                                 bool content_root_only)
    : strip_soft_chrome_(strip_soft_chrome),
      content_root_only_(content_root_only) {}
MarkdownExtractorFilter::~MarkdownExtractorFilter() = default;

void MarkdownExtractorFilter::StartDocument() {
  markdown_.clear();
  cur_.clear();
  cell_.clear();
  code_buf_.clear();
  pre_language_.clear();
  inline_code_lang_.clear();
  pre_has_block_lines_ = false;
  table_.clear();
  lists_.clear();
  row_cells_.clear();
  fm_date_.clear();
  fm_chrome_date_.clear();
  anchor_text_start_ = 0;
  blockquote_depth_ = 0;
  pre_depth_ = 0;
  heading_level_ = 0;
  hard_skip_depth_ = 0;
  soft_skip_depth_ = 0;
  time_depth_ = 0;
  content_root_depth_ = 0;
  article_depth_ = 0;
  sectioning_depth_ = 0;
  anchor_depth_ = 0;
  anchor_open_pos_.clear();
  item_open_ = false;
  in_table_ = false;
  in_cell_ = false;
  in_inline_code_ = false;
  in_dt_ = false;
  table_header_done_ = false;
  last_was_list_item_ = false;
  truncated_ = false;
  dropped_content_root_ = false;
}

std::string* MarkdownExtractorFilter::Out() {
  return in_cell_ ? &cell_ : &cur_;
}

bool MarkdownExtractorFilter::AtOutputCap() {
  if (truncated_) return true;
  if (markdown_.size() > kMaxMarkdownBytes) {
    truncated_ = true;
    return true;
  }
  return false;
}

void MarkdownExtractorFilter::AppendText(std::string_view raw_text) {
  // Post-cap: stop accumulating once markdown_ crossed the output cap (mirrors
  // the AtOutputCap() guard in AppendBlock / AppendListItem).
  if (AtOutputCap()) return;
  // Content-root gate (Issue F (4)): outside a content root, drop bare-body
  // chrome text the nav/header/footer denylist misses.
  if (OutsideContentRoot()) return;
  // Issue F (1): entity-decode the text node BEFORE whitespace-collapse and
  // BEFORE the leading-marker structural escape (which runs at FlushBlock). The
  // decode-then-escape order keeps the markdown-structure security contract.
  const std::string decoded = SafeDecodeHtmlEntities(raw_text);
  std::string_view text(decoded);
  if (HasVisibleText(text)) MarkAnchorText();
  std::string* o = Out();
  // Pre-cap bound on the intermediate buffer itself: a single never-flushed
  // block (or one giant text node) must not grow cur_/cell_ past the output cap
  // before markdown_ crosses it. Without this, the 8 MiB cap leaks via the
  // staging buffers (review: unbounded intermediate buffer growth).
  if (o->size() >= kMaxMarkdownBytes) {
    truncated_ = true;
    return;
  }
  if (pre_depth_ > 0) {  // preformatted: preserve verbatim (bounded)
    std::size_t room = kMaxMarkdownBytes - o->size();
    // The decoded text is well-formed UTF-8 with no-break spaces already
    // folded, so it is appended verbatim; a cut at the cap is repaired once in
    // EndDocument.
    o->append(text.substr(0, std::min(text.size(), room)));
    if (text.size() > room) truncated_ = true;
    return;
  }
  bool prev_space = o->empty() || o->back() == ' ' || o->back() == '\n';
  for (std::size_t i = 0; i < text.size(); ++i) {
    const char c = text[i];
    if (o->size() >= kMaxMarkdownBytes) {  // bound the staging buffer per char
      truncated_ = true;
      break;
    }
    if (IsAsciiWs(c)) {
      if (!prev_space) {
        o->push_back(' ');
        prev_space = true;
      }
      continue;
    }
    // Decoded text stays text: a '<' (from "&lt;" or a literal one) is
    // escaped so no Markdown renderer reads it as an HTML tag or autolink, and
    // an '&' that would read as a character reference is escaped likewise. A
    // literal backslash is always doubled, so it can never cancel one of these
    // escapes (or a table cell's "\|"), even across text nodes.
    // Brackets are escaped too, so link, image and reference-definition
    // syntax written as text never forms a live link.
    if (c == '\\' || c == '<' || c == '[' || c == ']' ||
        (c == '&' && StartsMarkdownReference(text, i))) {
      o->push_back('\\');
    }
    o->push_back(c);
    prev_space = false;
  }
}

void MarkdownExtractorFilter::AppendBlock(const std::string& body) {
  if (AtOutputCap()) return;
  if (!markdown_.empty()) markdown_ += "\n\n";
  markdown_ += body;
  last_was_list_item_ = false;
}

void MarkdownExtractorFilter::AppendListItem(const std::string& body) {
  if (AtOutputCap()) return;
  if (!markdown_.empty()) markdown_ += last_was_list_item_ ? "\n" : "\n\n";
  markdown_ += body;
  last_was_list_item_ = true;
}

void MarkdownExtractorFilter::FlushBlock() {
  std::string body = Trim(cur_);
  cur_.clear();
  if (body.empty()) return;

  if (heading_level_ > 0) {
    AppendBlock(std::string(heading_level_, '#') + " " + body);
    return;
  }
  if (item_open_ && !lists_.empty()) {
    // Clamp the emitted indent depth (MF-1: a 1000-deep list must not emit a
    // 1000-space prefix per item).
    int levels =
        std::min<int>(static_cast<int>(lists_.size()), kMaxListIndentLevels);
    int indent = (levels - 1) * 2;
    if (indent < 0) indent = 0;
    ListCtx& top = lists_.back();
    std::string marker =
        top.ordered ? std::to_string(++top.counter) + ". " : "- ";
    // A folded list item can carry an interior '\n' (a <br>, or a folded block
    // child with a <br>); escape any block marker at the start of those interior
    // lines so it cannot forge structure at column 0 (security contract).
    // EscapeLeadingMarker covers the FIRST line (the char right after the
    // "- "/"N. " marker): like every other block branch, a leading '#'/'>'/'|'
    // /'-'/'+' there must be literal text, not a forged heading/quote/table/list.
    std::string line = std::string(indent, ' ') + marker +
                       EscapeInteriorLineMarkers(EscapeLeadingMarker(body));
    AppendListItem(line);
    item_open_ = false;
    return;
  }
  if (blockquote_depth_ > 0) {
    int depth = std::min(blockquote_depth_, kMaxBlockquoteDepth);  // MF-1 clamp
    std::string bq;
    for (int i = 0; i < depth; ++i) bq += "> ";
    AppendBlock(bq + EscapeLeadingMarker(body));
    return;
  }
  if (in_dt_) {
    // A <dt> definition term: emit as a bold standalone block so the following
    // <dd> reads as its definition. EscapeLeadingMarker still applies (the term
    // is attacker-influenced) — it cannot forge a heading/list/quote. But only
    // add the outer '**' when the term carries no '*' of its own: a term with an
    // inline <strong>/<b> (or a literal '*') already has emphasis markers in the
    // body, and wrapping it would emit malformed nesting ("**Hello **World****").
    // In that case keep the term's own emphasis intact, unwrapped.
    const std::string esc = EscapeLeadingMarker(body);
    AppendBlock(esc.find('*') == std::string::npos ? "**" + esc + "**" : esc);
    return;
  }
  AppendBlock(EscapeLeadingMarker(body));
}

void MarkdownExtractorFilter::AnchorBlockJoin() {
  // Insert the ": " segment joiner before a new flattened block inside an open
  // <a>, but only once content already follows the '[' (so the first block does
  // not get a leading joiner), and never doubling an existing separator.
  std::string* o = Out();
  // Mark the open anchors as cards (block content), so each one ends its own
  // block at </a> and consecutive cards stay apart.
  for (OpenAnchor& a : anchor_open_pos_) a.has_block = true;
  // Source whitespace between the blocks is not a separator: drop it so the
  // joiner is applied consistently ("Title 34 filters: description").
  while (o->size() > anchor_text_start_ && o->back() == ' ') o->pop_back();
  if (o->size() <= anchor_text_start_) return;  // nothing emitted yet
  if (o->back() == ':') return;                 // already separated
  o->append(": ");
}

bool MarkdownExtractorFilter::TrimLinkText(std::string* o, std::size_t open) {
  // Whitespace at the end of the label moves after the link.
  bool trailing = false;
  while (o->size() > open + 1 && IsAsciiWs(o->back())) {
    o->pop_back();
    trailing = true;
  }
  // Whitespace at the start moves before it, so "Go<a> here</a>" still reads
  // "Go [here](...)".
  std::size_t b = open + 1;
  while (b < o->size() && IsAsciiWs((*o)[b])) ++b;
  if (b > open + 1) {
    o->erase(open + 1, b - (open + 1));
    if (open > 0 && !IsAsciiWs((*o)[open - 1])) o->insert(open, 1, ' ');
  }
  return trailing;
}

void MarkdownExtractorFilter::CaptureFrontMatterDate(HtmlElement* element,
                                                     std::string* date) {
  if (!date->empty()) return;
  const HtmlElement::Attribute* dt = element->FindAttribute("datetime");
  const char* v = dt != nullptr ? dt->DecodedValueOrNull() : nullptr;
  if (v != nullptr && *v != '\0') date->assign(v);
}

void MarkdownExtractorFilter::PreLineBreak() {
  std::string* o = Out();
  while (!o->empty() && (o->back() == ' ' || o->back() == '\t')) o->pop_back();
  if (!o->empty() && o->back() != '\n') o->push_back('\n');
}

bool MarkdownExtractorFilter::CodeSpanIsBlock() const {
  if (in_cell_ || anchor_depth_ > 0 || heading_level_ > 0 || item_open_ ||
      blockquote_depth_ > 0 || in_dt_) {
    return false;
  }
  if (HasVisibleText(cur_)) return false;  // text before it: an inline span
  const std::string body = TrimCodeLines(code_buf_);
  return body.find('\n') != std::string::npos;
}

void MarkdownExtractorFilter::ListItemBlockJoin() {
  std::string* o = Out();
  // Separate folded block segments by exactly one space. Trim any trailing
  // inter-block whitespace first (pretty-printed HTML puts a newline between
  // sibling blocks, which collapses to a space) so the join is consistent
  // regardless of source formatting; then add a single space unless we are at
  // the very start of the item (a block as the item's first child gets no
  // leading separator). A plain ": " joiner is intentionally NOT used: it would
  // be inconsistent here (an icon-badge "✓ Title" must not become "✓: Title")
  // and the segments already read as one citable line.
  while (!o->empty() && o->back() == ' ') o->pop_back();
  if (o->empty() || o->back() == '\n') return;
  o->push_back(' ');
}

void MarkdownExtractorFilter::StartElement(HtmlElement* element) {
  if (Skipping()) {  // inside a dropped subtree: track strip nesting only
    if (soft_skip_depth_ > 0 && hard_skip_depth_ == 0 &&
        IsContentRoot(element)) {
      dropped_content_root_ = true;  // soft chrome is hiding a content island
    }
    if (IsHardStripKeyword(element->keyword()) || IsInlineHidden(element) ||
        IsHardStripByName(element)) {
      ++hard_skip_depth_;
    } else if (element->keyword() == HtmlName::kTime) {
      // A "Last updated <time>" line in a <footer> inside <main> is page
      // chrome; its date is the page's date only when the content itself
      // carries none (see PrependFrontMatter).
      if (hard_skip_depth_ == 0 && content_root_depth_ > 0) {
        CaptureFrontMatterDate(element, &fm_chrome_date_);
      }
      ++time_depth_;  // each <time> tracks its own counter (balanced nesting)
    } else if (strip_soft_chrome_ && IsSoftStripKeyword(element->keyword())) {
      ++soft_skip_depth_;
    }
    return;
  }

  // While buffering an inline <code> span, ignore nested element markers; their
  // text still reaches code_buf_ via Characters.
  if (in_inline_code_) return;

  const HtmlName::Keyword kw = element->keyword();

  if (IsHardStripKeyword(kw) || IsInlineHidden(element) ||
      IsHardStripByName(element)) {
    ++hard_skip_depth_;
    return;
  }
  // Issue F (3): an <article>-scoped <header>/<footer> is content (blog
  // title/date/byline), not chrome — do NOT soft-strip it. A <header> inside
  // <main>, <section> or <aside> is likewise that section's heading block (a
  // docs page's H1, a landing section's H2 and intro). Only the page-level
  // banner <header> and page or section <footer>s are stripped, and <nav> is
  // always chrome.
  const bool article_scoped_chrome =
      (article_depth_ > 0 &&
       (kw == HtmlName::kHeader || kw == HtmlName::kFooter)) ||
      (sectioning_depth_ > 0 && kw == HtmlName::kHeader);
  if (strip_soft_chrome_ && IsSoftStripKeyword(kw) && !article_scoped_chrome) {
    ++soft_skip_depth_;
    return;
  }

  // Content-root depth (Issue F (4)) and article depth (Issue F (3)) must be
  // tracked BEFORE the OutsideContentRoot() emission gate so entering <main>
  // from outside opens the root.
  if (IsContentRoot(element)) ++content_root_depth_;
  if (IsArticleRoot(element)) ++article_depth_;
  if (IsSectioningScope(element)) ++sectioning_depth_;

  // Issue F (3): capture the first <time datetime> as the front-matter date and
  // SUPPRESS its visible text (a timestamp is metadata, not citation body).
  if (kw == HtmlName::kTime) {
    CaptureFrontMatterDate(element, &fm_date_);
    ++time_depth_;  // drop the visible stamp text (Skipping covers descendants)
    return;
  }

  // Content-root gate (Issue F (4)): outside any content root, drop bare-body
  // chrome — text is already gated in AppendText/Characters; also suppress the
  // structural-only emitters (hr/table markers) here. Inline anchor open/close
  // is gated symmetrically by this same predicate in EndElement.
  if (OutsideContentRoot()) return;

  // Block-in-anchor (Issue F (5)): a block element (heading/p/li/…) wrapped in
  // an open <a> must NOT start a new markdown block — it flattens inline, joined
  // by ": ", so the whole card emits one '[text](href)'.
  const bool is_summary = StringCaseEqual(element->name_str(), "summary");
  if (InAnchorBlock() && (IsBlockKeyword(kw) || is_summary)) {
    AnchorBlockJoin();
    return;
  }

  // Inside <pre>, a block child (a <div> per line in a diff view, a <p>) is a
  // line of the preformatted text, not a Markdown block: it only starts a new
  // line, so the fence stays whole.
  if (pre_depth_ > 0 && kw != HtmlName::kPre &&
      (IsBlockKeyword(kw) || is_summary)) {
    PreLineBreak();
    pre_has_block_lines_ = true;
    return;
  }

  // A <details>/<summary> disclosure (the FAQ pattern on pricing/features/1.1):
  // the <summary> is the question label — emit it as a heading so each Q is a
  // citable anchor, not a bare paragraph indistinguishable from its answer.
  // <summary> is not an HtmlName keyword, so match by name (like <svg>). Guarded
  // by InAnchorBlock above so a <summary> wrapped in an <a> flattens inline
  // instead of forging a heading and shattering the link (Issue F (5) parity).
  if (is_summary) {
    FlushBlock();
    heading_level_ = 3;
    return;
  }

  // Block-in-list-item fold (icon-badge / card feature lists): a kP/kDiv/kSection/
  // kAside child of an open <li> folds inline into the item line (joined like the
  // anchor flatten) instead of flushing the item early and detaching the rest.
  if (InListItemBlock() && IsListItemFoldKeyword(kw)) {
    ListItemBlockJoin();
    return;
  }

  switch (kw) {
    case HtmlName::kH1:
    case HtmlName::kH2:
    case HtmlName::kH3:
    case HtmlName::kH4:
    case HtmlName::kH5:
    case HtmlName::kH6:
      FlushBlock();
      heading_level_ = kw - HtmlName::kH1 + 1;
      break;
    case HtmlName::kP:
    case HtmlName::kDiv:
    case HtmlName::kSection:
    case HtmlName::kArticle:
    case HtmlName::kMain:
    case HtmlName::kAside:
    case HtmlName::kAddress:
    case HtmlName::kDl:
    case HtmlName::kDd:
      FlushBlock();
      break;
    case HtmlName::kDt:
      // A <dt> term emits as a bold block (**term**) so an agent binds the
      // following <dd> definition to it (the home FAQ <dl> shape, where dt/dd
      // otherwise concatenate into one undifferentiated paragraph).
      FlushBlock();
      in_dt_ = true;
      break;
    case HtmlName::kBlockquote:
      FlushBlock();
      ++blockquote_depth_;
      break;
    case HtmlName::kPre:
      FlushBlock();
      // Capture the outermost <pre>'s code language (Astro/Shiki emits
      // <pre data-language="nginx">) so the closing fence is tagged ```nginx.
      // The label is an ASCII token, so the decoded attribute path is safe.
      if (pre_depth_ == 0) {
        pre_language_.clear();
        pre_has_block_lines_ = false;
        const HtmlElement::Attribute* lang =
            element->FindAttribute("data-language");
        const char* v = lang != nullptr ? lang->DecodedValueOrNull() : nullptr;
        if (v != nullptr && *v != '\0') pre_language_.assign(v);
      }
      ++pre_depth_;
      break;
    case HtmlName::kUl:
      FlushBlock();
      lists_.push_back({false, 0});
      break;
    case HtmlName::kOl:
      FlushBlock();
      lists_.push_back({true, 0});
      break;
    case HtmlName::kLi:
      FlushBlock();
      item_open_ = true;
      break;
    case HtmlName::kHr:
      FlushBlock();
      AppendBlock("---");
      break;
    case HtmlName::kTable:
      FlushBlock();
      in_table_ = true;
      table_.clear();
      row_cells_.clear();
      table_header_done_ = false;
      break;
    case HtmlName::kTr:
      row_cells_.clear();
      break;
    case HtmlName::kTd:
    case HtmlName::kTh:
      in_cell_ = true;
      cell_.clear();
      break;
    // ---- inline (markers suppressed inside <pre>) ----
    case HtmlName::kBr:
      // A <br> inside <pre> is a line break of the preformatted text; inside
      // a heading it is a space, since an ATX heading is one line.
      if (pre_depth_ > 0) {
        Out()->append("\n");
      } else if (heading_level_ > 0) {
        if (!Out()->empty() && Out()->back() != ' ') Out()->push_back(' ');
      } else {
        Out()->append("  \n");
      }
      break;
    case HtmlName::kStrong:
    case HtmlName::kB:
      if (pre_depth_ == 0) Out()->append("**");
      break;
    case HtmlName::kEm:
    case HtmlName::kI:
      if (pre_depth_ == 0) Out()->append("*");
      break;
    case HtmlName::kCode:
      if (pre_depth_ == 0) {  // start buffering an inline code span (MF-4/SF-2)
        in_inline_code_ = true;
        code_buf_.clear();
        inline_code_lang_ =
            LanguageFromCodeClass(element->AttributeValue(HtmlName::kClass));
      } else if (pre_language_.empty()) {
        // Hand-authored <pre><code class="language-bash"> blocks carry the fence
        // language on the <code> class, not a <pre data-language> attribute
        // (Astro/Shiki). Capture it so those fences are tagged too.
        pre_language_ =
            LanguageFromCodeClass(element->AttributeValue(HtmlName::kClass));
      }
      break;
    case HtmlName::kA:
      if (pre_depth_ == 0) {
        std::string* o = Out();
        anchor_open_pos_.push_back({o->size(), false, false});
        o->append("[");
        // Issue F (5): record where the link text begins so a block element
        // wrapped in this anchor can be flattened into one '[text](href)'. Only
        // the OUTERMOST anchor anchors the join (nested <a> is degenerate HTML).
        if (anchor_depth_ == 0) anchor_text_start_ = o->size();
        ++anchor_depth_;
      }
      break;
    case HtmlName::kImg: {
      if (pre_depth_ != 0) break;
      const char* src = element->AttributeValue(HtmlName::kSrc);
      if (src != nullptr && *src != '\0') {
        // Decode the alt text from its source form: the parser's own decoded
        // value is Latin-1 and is absent for any non-ASCII alt text.
        const HtmlElement::Attribute* alt =
            element->FindAttribute(HtmlName::kAlt);
        std::string* o = Out();
        MarkAnchorText();
        o->append("![");
        if (alt != nullptr && alt->escaped_value() != nullptr) {
          o->append(
              EscapeLinkText(SafeDecodeHtmlEntities(alt->escaped_value())));
        }
        o->append("](");
        o->append(SanitizeUrl(src));
        o->append(")");
      }
      break;
    }
    default:
      break;  // unknown / pass-through container: its text still flows
  }
}

void MarkdownExtractorFilter::EndElement(HtmlElement* element) {
  if (Skipping()) {
    if (IsHardStripKeyword(element->keyword()) || IsInlineHidden(element) ||
        IsHardStripByName(element)) {
      if (hard_skip_depth_ > 0) --hard_skip_depth_;
    } else if (element->keyword() == HtmlName::kTime) {
      if (time_depth_ > 0) --time_depth_;
    } else if (strip_soft_chrome_ && IsSoftStripKeyword(element->keyword())) {
      if (soft_skip_depth_ > 0) --soft_skip_depth_;
    }
    return;
  }

  // Inline <code> span: only the matching </code> ends it; emit with an adaptive
  // backtick fence so a backtick in the content cannot close the span early.
  if (in_inline_code_) {
    if (element->keyword() == HtmlName::kCode && pre_depth_ == 0) {
      // A multi-line <code> that starts its own block (a command shown with
      // white-space: pre outside any <pre>) is a code block: its line breaks
      // are part of the command, so it becomes a fenced block instead of a
      // span that would join the lines.
      if (CodeSpanIsBlock()) {
        const std::string body = TrimCodeLines(code_buf_);
        FlushBlock();
        AppendBlock(FencedBlock(body, inline_code_lang_));
        code_buf_.clear();
        in_inline_code_ = false;
        return;
      }
      // An inline span is single-line: fold CR/LF to a space so a newline
      // cannot end the paragraph and let the text after the orphaned
      // backtick forge a heading or list.
      for (char& c : code_buf_) {
        if (c == '\n' || c == '\r') c = ' ';
      }
      // Leading and trailing whitespace is source formatting, not code.
      code_buf_ = Trim(code_buf_);
      if (code_buf_.empty()) {
        in_inline_code_ = false;
        return;
      }
      int n = LongestBacktickRun(code_buf_);
      std::string fence(std::max(1, n + 1), '`');
      bool pad = !code_buf_.empty() &&
                 (code_buf_.front() == '`' || code_buf_.back() == '`');
      std::string* o = Out();
      if (HasVisibleText(code_buf_)) MarkAnchorText();
      o->append(fence);
      if (pad) o->push_back(' ');
      o->append(code_buf_);
      if (pad) o->push_back(' ');
      o->append(fence);
      code_buf_.clear();
      in_inline_code_ = false;
    }
    return;
  }

  const HtmlName::Keyword kw = element->keyword();

  // Close content-root / article scopes (symmetric with StartElement). Done
  // AFTER the OutsideContentRoot gate below would matter, but BEFORE structural
  // close handling so a </main> still flushes its own block while "inside".
  const bool closing_content_root = IsContentRoot(element);
  const bool closing_article_root = IsArticleRoot(element);
  const bool closing_sectioning = IsSectioningScope(element);

  // Content-root gate (Issue F (4)): outside a content root, suppress the
  // structural close handling too (its text was already dropped). Still close
  // the scope counters so a re-entered root works.
  if (OutsideContentRoot()) {
    if (closing_content_root && content_root_depth_ > 0) --content_root_depth_;
    if (closing_article_root && article_depth_ > 0) --article_depth_;
    if (closing_sectioning && sectioning_depth_ > 0) --sectioning_depth_;
    return;
  }

  // Block-in-anchor (Issue F (5)): a block element (or an anchor-wrapped
  // <summary>, flattened symmetrically with StartElement) wrapped in an open <a>
  // flattened inline at StartElement — its close emits no block boundary.
  const bool is_summary = StringCaseEqual(element->name_str(), "summary");
  if (InAnchorBlock() && (IsBlockKeyword(kw) || is_summary) &&
      kw != HtmlName::kA) {
    if (closing_content_root && content_root_depth_ > 0) --content_root_depth_;
    if (closing_article_root && article_depth_ > 0) --article_depth_;
    if (closing_sectioning && sectioning_depth_ > 0) --sectioning_depth_;
    return;
  }

  // A block child inside <pre> ends a line of the preformatted text
  // (symmetric with StartElement); it emits no Markdown block boundary.
  if (pre_depth_ > 0 && kw != HtmlName::kPre &&
      (IsBlockKeyword(kw) || is_summary)) {
    PreLineBreak();
    if (closing_content_root && content_root_depth_ > 0) --content_root_depth_;
    if (closing_article_root && article_depth_ > 0) --article_depth_;
    if (closing_sectioning && sectioning_depth_ > 0) --sectioning_depth_;
    return;
  }

  // Close a <summary> heading (FAQ disclosure label) — mirror the heading close.
  if (is_summary) {
    FlushBlock();
    heading_level_ = 0;
    return;
  }

  // Block-in-list-item fold (symmetric with StartElement): a folded kP/kDiv/
  // kSection/kAside child emits no block boundary — the item stays open and is
  // flushed once at </li> (so EscapeLeadingMarker still runs once on the line).
  // Still balance the content-root/article scopes (a kSection/kAside could be a
  // role=main/article root), exactly as the anchor-flatten guard does.
  if (InListItemBlock() && IsListItemFoldKeyword(kw)) {
    if (closing_content_root && content_root_depth_ > 0) --content_root_depth_;
    if (closing_article_root && article_depth_ > 0) --article_depth_;
    if (closing_sectioning && sectioning_depth_ > 0) --sectioning_depth_;
    return;
  }

  switch (kw) {
    case HtmlName::kH1:
    case HtmlName::kH2:
    case HtmlName::kH3:
    case HtmlName::kH4:
    case HtmlName::kH5:
    case HtmlName::kH6:
      FlushBlock();
      heading_level_ = 0;
      break;
    case HtmlName::kP:
    case HtmlName::kDiv:
    case HtmlName::kSection:
    case HtmlName::kArticle:
    case HtmlName::kMain:
    case HtmlName::kAside:
    case HtmlName::kAddress:
    case HtmlName::kDl:
    case HtmlName::kDd:
      FlushBlock();
      break;
    case HtmlName::kDt:
      FlushBlock();  // emits the bold term (in_dt_ still set, see FlushBlock)
      in_dt_ = false;
      break;
    case HtmlName::kBlockquote:
      FlushBlock();
      if (blockquote_depth_ > 0) --blockquote_depth_;
      break;
    case HtmlName::kPre: {
      if (pre_depth_ > 0) --pre_depth_;
      std::string body = cur_;
      cur_.clear();
      while (!body.empty() && (body.back() == '\n' || body.back() == '\r')) {
        body.pop_back();
      }
      // Tag the opening fence with the captured code language on the outermost
      // close only. Sanitize to a leading run of language-token chars (the
      // attribute is attacker-influenced rendered DOM) so it cannot inject a
      // newline/marker past the fence line.
      std::string lang;
      if (pre_depth_ == 0) {
        lang = SanitizeFenceLanguage(pre_language_);
        pre_language_.clear();
        if (pre_has_block_lines_) body = TidyBlockLinePre(body);
        pre_has_block_lines_ = false;
      }
      // MF-4: fence longer than any backtick run in body.
      if (!body.empty()) AppendBlock(FencedBlock(body, lang));
      break;
    }
    case HtmlName::kLi:
      FlushBlock();
      item_open_ = false;
      break;
    case HtmlName::kUl:
    case HtmlName::kOl:
      FlushBlock();
      if (!lists_.empty()) lists_.pop_back();
      break;
    case HtmlName::kTd:
    case HtmlName::kTh:
      in_cell_ = false;
      row_cells_.push_back(EscapeCell(Trim(cell_)));
      cell_.clear();
      break;
    case HtmlName::kTr: {
      if (!in_table_) break;
      // Bound table_ to the output cap: a table with a huge number of rows must
      // not grow the staging buffer without limit before the </table> flush.
      if (table_.size() < kMaxMarkdownBytes) {
        std::string row = "|";
        for (const std::string& c : row_cells_) row += " " + c + " |";
        table_ += row + "\n";
        if (!table_header_done_) {
          std::string sep = "|";
          for (size_t i = 0; i < row_cells_.size(); ++i) sep += " --- |";
          table_ += sep + "\n";
          table_header_done_ = true;
        }
      } else {
        truncated_ = true;
      }
      row_cells_.clear();
      break;
    }
    case HtmlName::kTable: {
      in_table_ = false;
      std::string t = Trim(table_);
      table_.clear();
      if (!t.empty()) AppendBlock(t);
      break;
    }
    case HtmlName::kStrong:
    case HtmlName::kB:
      if (pre_depth_ == 0) Out()->append("**");
      break;
    case HtmlName::kEm:
    case HtmlName::kI:
      if (pre_depth_ == 0) Out()->append("*");
      break;
    case HtmlName::kA:
      if (pre_depth_ == 0) {
        if (anchor_depth_ > 0) --anchor_depth_;  // Issue F (5)
        const char* href = element->AttributeValue(HtmlName::kHref);
        std::string* o = Out();
        // A link with no visible text (an empty heading permalink, an icon-only
        // link whose svg / aria-hidden child was stripped) is dropped rather
        // than emitted as '[](href)'; empty emphasis markers from an icon-font
        // <i> do not count as text. aria-label is deliberately not used as text.
        // An
        // image inside the link counts as content, so image links are kept.
        bool space_after = false;
        bool card = false;
        if (!anchor_open_pos_.empty()) {
          const OpenAnchor open = anchor_open_pos_.back();
          anchor_open_pos_.pop_back();
          if (!open.has_text && open.pos < o->size() && (*o)[open.pos] == '[') {
            o->resize(open.pos);
            break;
          }
          if (open.pos < o->size() && (*o)[open.pos] == '[') {
            space_after = TrimLinkText(o, open.pos);
          }
          card = open.has_block;
        }
        o->append("](");
        if (href != nullptr) o->append(SanitizeUrl(href));
        o->append(")");
        if (space_after) o->push_back(' ');
        // A card link (an <a> wrapping blocks) is a block of its own, so the
        // next card does not run into it.
        if (card && anchor_depth_ == 0 && !in_cell_ && heading_level_ == 0) {
          FlushBlock();
        }
      }
      break;
    default:
      break;
  }

  // Close the content-root / article scopes after structural handling so a
  // closing main/article still flushed its own block while "inside" the root.
  if (closing_content_root && content_root_depth_ > 0) --content_root_depth_;
  if (closing_article_root && article_depth_ > 0) --article_depth_;
  if (closing_sectioning && sectioning_depth_ > 0) --sectioning_depth_;
}

void MarkdownExtractorFilter::Characters(HtmlCharactersNode* characters) {
  if (Skipping()) return;
  if (AtOutputCap()) return;  // post-cap: stop accumulating any text
  if (parser_ != nullptr && parser_->passing_through()) {
    truncated_ = true;  // the rest of the page is raw markup: not copied
    return;
  }
  if (OutsideContentRoot()) return;  // Issue F (4): drop bare-body chrome text
  if (in_inline_code_) {             // buffered raw, adaptive-fenced at </code>
    // Bound code_buf_ to the output cap too (a giant <code> span must not grow
    // it without limit before the </code> flush).
    if (code_buf_.size() >= kMaxMarkdownBytes) {
      truncated_ = true;
      return;
    }
    // v2.0.27 audit: markdown does NOT unescape inside backticks, so a raw
    // '&lt;style&gt;' would reach the agent literally instead of '<style>'.
    // Issue F decoded body text in AppendText but this code-span path bypasses
    // it; decode here too via the same UTF-8-safe helper (a decoded backtick is
    // handled by LongestBacktickRun's adaptive fence at </code>).
    // Line breaks are kept until </code>, which either emits a fenced block
    // (a multi-line command) or folds them to spaces for an inline span.
    std::string t = SafeDecodeHtmlEntities(characters->contents());
    std::size_t room = kMaxMarkdownBytes - code_buf_.size();
    if (t.size() > room) {
      code_buf_.append(t.substr(0, room));
      truncated_ = true;
    } else {
      code_buf_.append(t);
    }
    return;
  }
  AppendText(characters->contents());
}

namespace {
// Make a metadata value safe to put after 'key: ' on one YAML line: fold CR/LF
// to spaces and double-quote (with " and \ escaped) when the value contains a
// YAML-significant char so the front-matter can never be misparsed or used to
// forge extra keys (an attacker controls <title>/<meta>). Plain values stay
// unquoted to match the common, clean case.
std::string YamlScalar(std::string_view v) {
  std::string flat;
  flat.reserve(v.size());
  for (char c : v) flat.push_back((c == '\n' || c == '\r') ? ' ' : c);
  bool needs_quote = flat.empty();
  // YAML plain-scalar hazards: a ':' is only significant before a space or at
  // end of value (so a URL's '://' stays unquoted); a '#' only starts a comment
  // after a space; quotes/backslash always force quoting.
  for (std::size_t i = 0; i < flat.size() && !needs_quote; ++i) {
    char c = flat[i];
    if (c == '"' || c == '\'' || c == '\\') {
      needs_quote = true;
    } else if (c == ':' && (i + 1 == flat.size() || flat[i + 1] == ' ')) {
      needs_quote = true;
    } else if (c == '#' && i > 0 && flat[i - 1] == ' ') {
      needs_quote = true;
    }
  }
  // A leading char that YAML treats specially also forces quoting.
  if (!needs_quote && !flat.empty()) {
    char f = flat.front();
    if (f == ' ' || f == '-' || f == '?' || f == ':' || f == ',' || f == '[' ||
        f == ']' || f == '{' || f == '}' || f == '#' || f == '&' || f == '*' ||
        f == '!' || f == '|' || f == '>' || f == '%' || f == '@' || f == '`' ||
        f == '"' || f == '\'') {
      needs_quote = true;
    }
  }
  if (!needs_quote) return flat;
  std::string out = "\"";
  for (char c : flat) {
    if (c == '"' || c == '\\') out.push_back('\\');
    out.push_back(c);
  }
  out.push_back('"');
  return out;
}
}  // namespace

void MarkdownExtractorFilter::PrependFrontMatter() {
  // Issue F (2): emit a YAML front-matter block only when head metadata
  // (title/description/canonical) exists. A bare <time> date alone does NOT
  // start a block (it is dropped); it only joins an existing block as a line.
  if (fm_title_.empty() && fm_description_.empty() && fm_canonical_.empty()) {
    return;
  }
  if (markdown_.empty()) return;  // no body -> no front-matter-only document
  std::string fm = "---\n";
  if (!fm_title_.empty()) fm += "title: " + YamlScalar(fm_title_) + "\n";
  if (!fm_description_.empty()) {
    fm += "description: " + YamlScalar(fm_description_) + "\n";
  }
  // A date from the content wins; one from chrome inside the content root is
  // the fallback.
  const std::string& date = fm_date_.empty() ? fm_chrome_date_ : fm_date_;
  if (!date.empty()) fm += "date: " + YamlScalar(date) + "\n";
  if (!fm_canonical_.empty()) {
    fm += "canonical: " + YamlScalar(fm_canonical_) + "\n";
  }
  fm += "---\n\n";
  markdown_.insert(0, fm);
}

void MarkdownExtractorFilter::EndDocument() {
  FlushBlock();
  while (!markdown_.empty() && IsAsciiWs(markdown_.back()))
    markdown_.pop_back();
  PrependFrontMatter();
  // Every piece is well-formed UTF-8 already; a cut at the output cap or a
  // URL or date byte from a non-UTF-8 page is the remaining way to break
  // that, so the whole copy is checked once and repaired if needed.
  if (!IsWellFormedUtf8(markdown_)) {
    markdown_ = ToWellFormedUtf8(markdown_, /*drop_cut_tail=*/truncated_);
  }
}

namespace {

// Cheap detector: does the rendered DOM contain a <main>/<article>/role=main/
// role=article content root? When it does, the markdown pass runs in
// content-root-only mode so bare-<body> chrome (skip-links, promo banners) the
// nav/header/footer denylist misses is dropped (Issue F (4)). Landmark-free
// pages skip that mode and keep the fail-open two-pass behavior.
class ContentRootDetector : public EmptyHtmlFilter {
 public:
  const char* Name() const override { return "ContentRootDetector"; }
  void StartElement(HtmlElement* element) override {
    if (found_) return;
    if (IsContentRoot(element)) found_ = true;
  }
  bool found() const { return found_; }

 private:
  bool found_ = false;
};

bool HasContentRoot(std::string_view html, const std::string& parse_url) {
  ContentRootDetector detector;
  NullMessageHandler message_handler;
  HtmlParse parser(&message_handler);
  parser.AddFilter(&detector);
  if (!parser.StartParse(parse_url)) return false;
  parser.ParseText(html);
  parser.FinishParse();
  return detector.found();
}

// Runs one extraction over `html`. The page is fed in pieces with a Flush after
// each, so the filter sees the parser switch to passing the rest of the page
// through (nesting past the depth limit) and stops copying there. Pieces are
// kPiece bytes up to `fine_from`, then one byte at a time, so the copy ends at
// the exact point of the switch. If the switch is seen in a coarse piece,
// *switch_at is set to where that piece began and the caller runs again with
// fine_from there (rare: only pages that pass the rest through).
std::string RunExtractorOnce(std::string_view html,
                             const std::string& parse_url,
                             bool strip_soft_chrome, bool content_root_only,
                             const HtmlMetadata& meta, size_t fine_from,
                             size_t* switch_at) {
  MarkdownExtractorFilter filter(strip_soft_chrome, content_root_only);
  filter.SetFrontMatter(meta.title, meta.meta_description, meta.canonical);
  NullMessageHandler message_handler;
  HtmlParse parser(&message_handler);  // ctor auto-inits HtmlKeywords
  parser.AddFilter(&filter);
  filter.SetParser(&parser);
  if (!parser.StartParse(parse_url)) return std::string();
  constexpr size_t kPiece = 4096;
  *switch_at = std::string_view::npos;
  for (size_t pos = 0; pos < html.size();) {
    const size_t n = pos < fine_from ? kPiece : 1;
    parser.ParseText(html.substr(pos, n));
    parser.Flush();
    if (parser.passing_through()) {
      if (pos < fine_from) *switch_at = pos;
      break;
    }
    pos += n;
  }
  parser.FinishParse();
  // dropped_content_root signals a fail-open re-extract is needed; encode it by
  // returning empty so the caller re-runs (only meaningful on the strip pass).
  if (strip_soft_chrome && filter.dropped_content_root()) return std::string();
  return filter.markdown();
}

std::string RunExtractor(std::string_view html, const std::string& parse_url,
                         bool strip_soft_chrome, bool content_root_only,
                         const HtmlMetadata& meta) {
  size_t switch_at = std::string_view::npos;
  std::string md = RunExtractorOnce(
      html, parse_url, strip_soft_chrome, content_root_only, meta,
      /*fine_from=*/std::string_view::npos, &switch_at);
  if (switch_at == std::string_view::npos) return md;
  size_t unused;
  return RunExtractorOnce(html, parse_url, strip_soft_chrome, content_root_only,
                          meta, switch_at, &unused);
}
}  // namespace

std::string ExtractAgentMarkdown(std::string_view html, std::string_view url) {
  const std::string parse_url =
      url.empty() ? std::string("http://localhost/") : std::string(url);
  // Issue F (2): collect <title>/<meta description>/<link canonical> once (this
  // pass already entity-decodes) so both extractor passes prepend identical
  // front-matter.
  const HtmlMetadata meta = ExtractHtmlMetadata(html, parse_url);
  // Issue F (4): only enable content-root-only mode when a content root exists,
  // so landmark-free pages keep the fail-open two-pass behavior.
  const bool content_root_only = HasContentRoot(html, parse_url);
  // Pass 1: strip boilerplate chrome (and, when a content root exists, emit only
  // its subtree). RunExtractor returns "" if it dropped a <main>/<article>
  // content root into the soft-chrome strip (kill-guard).
  std::string md = RunExtractor(html, parse_url, /*strip_soft_chrome=*/true,
                                content_root_only, meta);
  if (!md.empty()) return md;
  // Pass 2 (fail-open): keep chrome AND drop content-root-only so content nested
  // in nav/header/footer — or a page whose entire content sits in chrome — is
  // still extracted (content-complete, boilerplate-included; P2's Readability
  // pass refines precision).
  return RunExtractor(html, parse_url, /*strip_soft_chrome=*/false,
                      /*content_root_only=*/false, meta);
}

}  // namespace net_instaweb
