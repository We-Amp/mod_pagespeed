// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

// PageSpeed 2.0 - HTML Critical CSS Injector Implementation

#include "src/worker/html_css_injector.h"

#include <cctype>
#include <cstddef>
#include <string>
#include <string_view>
#include <utility>

#include "lib/base/string_util.h"

namespace pagespeed {

using net_instaweb::LowerChar;

namespace {

// Case-insensitive comparison of two string segments.
bool CaseInsensitiveEqual(std::string_view a, std::string_view b) {
  if (a.size() != b.size()) return false;
  for (size_t i = 0; i < a.size(); ++i) {
    if (LowerChar(a[i]) != LowerChar(b[i])) {
      return false;
    }
  }
  return true;
}

// Find a case-insensitive tag (like "<head" or "<head>") in the HTML.
// Returns position of the end of the tag (after the '>').
// Enforces word boundary: <head> matches but <header> does not.
size_t FindOpenTagEnd(std::string_view html, std::string_view tag_name) {
  for (size_t i = 0; i + tag_name.size() + 1 <= html.size(); ++i) {
    if (html[i] == '<' &&
        CaseInsensitiveEqual(html.substr(i + 1, tag_name.size()), tag_name)) {
      // Verify word boundary after tag name.
      size_t after_name = i + 1 + tag_name.size();
      if (after_name < html.size()) {
        char next = html[after_name];
        if (next != '>' && next != ' ' && next != '\t' && next != '\n' &&
            next != '\r' && next != '/') {
          continue;
        }
      }
      // Found "<tag_name" — now find the closing '>'.
      size_t close = html.find('>', after_name);
      if (close != std::string_view::npos) {
        return close + 1;
      }
    }
  }
  return std::string_view::npos;
}

bool IsCssNewline(char c) { return c == '\n' || c == '\r' || c == '\f'; }

bool OpensMarkup(char next) {
  return (next >= 'a' && next <= 'z') || (next >= 'A' && next <= 'Z') ||
         next == '/' || next == '!' || next == '?';
}

constexpr std::string_view kEscapedLessThan = "\\3c ";

// One pass over `css` with the CSS tokenizer's string, comment and escape
// rules (see the header). Appends the rewritten text to `escaped` when it is
// not null, and sets `*opens_markup` (when not null) if a `<` outside strings
// and comments opens markup; such a `<` is copied as it is.
void ScanCssLessThan(std::string_view css, std::string* escaped,
                     bool* opens_markup) {
  enum class State : unsigned char { kNormal, kString, kComment };
  State state = State::kNormal;
  char quote = '\0';
  const size_t n = css.size();
  auto put = [escaped](std::string_view text) {
    if (escaped != nullptr) escaped->append(text);
  };
  for (size_t i = 0; i < n; ++i) {
    const char c = css[i];
    const char next = i + 1 < n ? css[i + 1] : '\0';
    if (c == '<') {
      if (state == State::kNormal && i + 1 < n && OpensMarkup(next) &&
          opens_markup != nullptr) {
        *opens_markup = true;
        if (escaped == nullptr) return;
      }
      if (state != State::kNormal) {
        put(kEscapedLessThan);
        continue;
      }
    } else if (c == '\\' && state != State::kComment) {
      // An escape: the backslash and the character it escapes go together,
      // so an escaped quote does not open or close a string.
      if (next == '<') {
        put(kEscapedLessThan);
        ++i;
        continue;
      }
      if (i + 1 < n && (state == State::kString || !IsCssNewline(next))) {
        // In a string a newline after the backslash continues it; CR LF is
        // one newline.
        size_t len = 2;
        if (next == '\r' && i + 2 < n && css[i + 2] == '\n') len = 3;
        put(css.substr(i, len));
        i += len - 1;
        continue;
      }
    } else if (state == State::kComment) {
      if (c == '*' && next == '/') {
        put("*/");
        ++i;
        state = State::kNormal;
        continue;
      }
    } else if (state == State::kString) {
      // A string ends at its quote, or (a bad string) at a newline.
      if (c == quote || IsCssNewline(c)) state = State::kNormal;
    } else if (c == '/' && next == '*') {
      put("/*");
      ++i;
      state = State::kComment;
      continue;
    } else if (c == '"' || c == '\'') {
      quote = c;
      state = State::kString;
    }
    put(css.substr(i, 1));
  }
}

// Build the style tag with critical CSS.
// Returns empty string if the CSS contains a style-closing sequence
// (XSS prevention: attacker-controlled CSS must not break out of the
// <style> element).
std::string BuildStyleTag(std::string_view critical_css) {
  // Strip null bytes, then write every `<` in a CSS string or comment as an
  // escape, as the serve path does (HtmlTransformFilter::InjectCriticalCss):
  // the validator's candidate document must carry the served block's text.
  std::string without_nul;
  without_nul.reserve(critical_css.size());
  for (char c : critical_css) {
    if (c != '\0') {
      without_nul.push_back(c);
    }
  }
  const std::string sanitized = EscapeLessThanForStyleElement(without_nul);

  // Abort if CSS contains </style (case-insensitive).  Without the
  // closing '>' requirement, this also catches </style\t, </style ,
  // and other variants that browsers accept as closing the element.
  for (size_t i = 0; i + 7 <= sanitized.size(); ++i) {
    if (CaseInsensitiveEqual(std::string_view(sanitized.data() + i, 7),
                             "</style")) {
      return {};
    }
  }

  std::string tag;
  tag.reserve(sanitized.size() + 50);
  tag.append("<style data-pagespeed-critical>");
  tag.append(sanitized);
  tag.append("</style>");
  return tag;
}

}  // namespace

bool CssOpensMarkupOutsideStrings(std::string_view css) {
  bool opens_markup = false;
  ScanCssLessThan(css, nullptr, &opens_markup);
  return opens_markup;
}

std::string EscapeLessThanForStyleElement(std::string_view css) {
  std::string out;
  out.reserve(css.size());
  ScanCssLessThan(css, &out, nullptr);
  return out;
}

namespace html_scan {

RawTextElement ScanRawTextElement(std::string_view html, size_t pos) {
  RawTextElement out;
  if (pos >= html.size() || html[pos] != '<') return out;

  static constexpr std::string_view kRawTextTags[] = {
      "script", "style", "textarea", "title", "xmp"};

  for (const auto& tag : kRawTextTags) {
    size_t tag_end = pos + 1 + tag.size();
    if (tag_end > html.size()) continue;
    if (!CaseInsensitiveEqual(html.substr(pos + 1, tag.size()), tag)) {
      continue;
    }

    // Verify word boundary after tag name, so <styles> is not <style>.
    if (tag_end < html.size()) {
      char next = html[tag_end];
      if (next != '>' && next != ' ' && next != '\t' && next != '\n' &&
          next != '\r' && next != '/') {
        continue;
      }
    }

    out.matched = true;
    out.tag = tag;

    // Find the case-insensitive closing tag.
    for (size_t j = tag_end; j + 2 + tag.size() <= html.size(); ++j) {
      if (html[j] == '<' && html[j + 1] == '/' &&
          CaseInsensitiveEqual(html.substr(j + 2, tag.size()), tag)) {
        size_t close_end = html.find('>', j + 2 + tag.size());
        if (close_end == std::string_view::npos) return out;  // unterminated
        out.closed = true;
        out.end = close_end + 1;
        return out;
      }
    }

    // Unclosed raw text element — everything after it is inside it.
    return out;
  }

  return out;
}

bool StartsComment(std::string_view html, size_t pos) {
  return pos + 3 < html.size() && html[pos] == '<' && html[pos + 1] == '!' &&
         html[pos + 2] == '-' && html[pos + 3] == '-';
}

size_t ScanComment(std::string_view html, size_t pos) {
  // The serve path's HtmlLexer (lib/html, synced from mod_pagespeed and not
  // edited here) and the HTML tokenizer read comments alike: the comment
  // start states end `<!-->` and `<!--->` at once, and otherwise the first
  // `-->` or `--!>` ends the comment.
  const size_t i = pos + 4;
  if (i < html.size() && html[i] == '>') return i + 1;
  if (i + 1 < html.size() && html[i] == '-' && html[i + 1] == '>') {
    return i + 2;
  }
  const size_t a = html.find("-->", i);
  const size_t b = html.find("--!>", i);
  if (a == std::string_view::npos && b == std::string_view::npos) {
    return std::string_view::npos;
  }
  return a <= b ? a + 3 : b + 4;
}

std::string_view CommentText(std::string_view html, size_t pos, size_t end) {
  // `<!-->` and `<!--->` are the only comments shorter than `<!---->`.
  if (end - pos < 7) return {};
  const size_t close = html.substr(end - 4, 4) == "--!>" ? 4 : 3;
  return html.substr(pos + 4, end - close - (pos + 4));
}

size_t FindOutsideCommentsAndRawText(std::string_view html,
                                     std::string_view needle) {
  size_t pos = 0;
  while (pos + needle.size() <= html.size()) {
    // Skip HTML comments.
    if (StartsComment(html, pos)) {
      size_t end = ScanComment(html, pos);
      if (end == std::string_view::npos) return std::string_view::npos;
      pos = end;
      continue;
    }

    // Skip raw text elements.  An UNCLOSED one is deliberately not skipped
    // here: this scanner's job is to find an injection point, and refusing to
    // scan the rest of a document because it contains an unclosed <script>
    // would refuse injection on pages browsers render fine.
    if (html[pos] == '<') {
      RawTextElement rt = ScanRawTextElement(html, pos);
      if (rt.matched && rt.closed) {
        pos = rt.end;
        continue;
      }
    }

    // Check for needle match.
    if (CaseInsensitiveEqual(html.substr(pos, needle.size()), needle)) {
      return pos;
    }

    ++pos;
  }
  return std::string_view::npos;
}

}  // namespace html_scan

bool CriticalCssNamesCascadeLayer(std::string_view css) {
  auto at_keyword = [&css](size_t i, std::string_view kw) {
    return i + kw.size() <= css.size() &&
           CaseInsensitiveEqual(css.substr(i, kw.size()), kw);
  };
  for (size_t i = 0; i < css.size(); ++i) {
    if (css[i] != '@') continue;
    if (at_keyword(i, "@layer")) {
      size_t j = i + 6;
      // `@layers` or `@layer-x` is some other at-keyword.
      if (j < css.size() &&
          (std::isalnum(static_cast<unsigned char>(css[j])) != 0 ||
           css[j] == '-' || css[j] == '_')) {
        continue;
      }
      while (j < css.size() &&
             (std::isspace(static_cast<unsigned char>(css[j])) != 0 ||
              css[j] == '/')) {
        // Skip whitespace and comments between the keyword and what follows.
        if (css[j] == '/' && j + 1 < css.size() && css[j + 1] == '*') {
          size_t end = css.find("*/", j + 2);
          j = end == std::string_view::npos ? css.size() : end + 2;
          continue;
        }
        if (css[j] == '/') break;
        ++j;
      }
      // Anonymous `@layer {` names nothing; anything else might.
      if (j < css.size() && css[j] == '{') continue;
      return true;
    }
    if (at_keyword(i, "@import")) {
      size_t end = css.find(';', i);
      std::string_view rule = css.substr(
          i, end == std::string_view::npos ? css.size() - i : end - i);
      // The `layer` / `layer(name)` import condition, as a word: after
      // whitespace or a closing paren, before `(`, `;`, whitespace or the end.
      for (size_t k = 1; k + 5 <= rule.size(); ++k) {
        if (!CaseInsensitiveEqual(rule.substr(k, 5), "layer")) continue;
        const char before = rule[k - 1];
        const char after = k + 5 < rule.size() ? rule[k + 5] : ';';
        if ((std::isspace(static_cast<unsigned char>(before)) != 0 ||
             before == ')' || before == '"' || before == '\'') &&
            (after == '(' || after == ';' ||
             std::isspace(static_cast<unsigned char>(after)) != 0)) {
          return true;
        }
      }
    }
  }
  return false;
}

size_t CriticalCssFallbackOffset(std::string_view html) {
  // Strategy 1: before </head>, skipping matches inside comments and raw text
  // elements (script, style, etc.).
  size_t pos = html_scan::FindOutsideCommentsAndRawText(html, "</head>");
  if (pos != std::string_view::npos) return pos;
  // Strategy 2: before </body>.
  pos = html_scan::FindOutsideCommentsAndRawText(html, "</body>");
  if (pos != std::string_view::npos) return pos;
  // Strategy 3: after <head> or <head ...>.
  pos = FindOpenTagEnd(html, "head");
  if (pos != std::string_view::npos) return pos;
  // Strategy 4: at document start.
  return 0;
}

CssInjectionResult InjectCriticalCss(std::string_view html,
                                     std::string_view critical_css) {
  if (html.empty()) {
    return {false, "Empty HTML input", {}, false};
  }
  return InjectCriticalCssAt(html, CriticalCssFallbackOffset(html),
                             critical_css);
}

CssInjectionResult InjectCriticalCssAt(std::string_view html, size_t pos,
                                       std::string_view critical_css) {
  if (html.empty()) {
    return {false, "Empty HTML input", {}, false};
  }
  if (pos > html.size()) {
    return {false, "Injection offset past the end of the document", {}, false};
  }

  if (critical_css.empty()) {
    return {true, {}, std::string(html), false};
  }

  std::string style_tag = BuildStyleTag(critical_css);
  if (style_tag.empty()) {
    return {
        true, "CSS contains </style sequence; injection aborted", {}, false};
  }

  std::string result;
  result.reserve(html.size() + style_tag.size());
  result.append(html.substr(0, pos));
  result.append(style_tag);
  result.append(html.substr(pos));
  return {true, {}, std::move(result), true};
}

}  // namespace pagespeed
