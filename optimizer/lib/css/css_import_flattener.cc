// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

// PageSpeed 2.0 - CSS @import Flattener Implementation

#include "lib/css/css_import_flattener.h"

#include <cctype>
#include <cstddef>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "lib/base/string_util.h"
#include "lib/base/url_util.h"

namespace pagespeed::css {

namespace {

// ASCII-case-insensitive prefix match.  At-keywords (@import, @layer)
// match case-insensitively in browsers; @charset is the one exception
// (the spec requires the exact lowercase byte sequence).
bool StartsWithCi(std::string_view s, std::string_view lowercase_prefix) {
  if (s.size() < lowercase_prefix.size()) return false;
  for (size_t i = 0; i < lowercase_prefix.size(); ++i) {
    if (net_instaweb::LowerChar(s[i]) != lowercase_prefix[i]) return false;
  }
  return true;
}

// Skip whitespace and CSS comments, returning new position.
size_t SkipWhitespaceAndComments(std::string_view css, size_t pos) {
  while (pos < css.size()) {
    // Skip whitespace.
    if (std::isspace(static_cast<unsigned char>(css[pos])) != 0) {
      ++pos;
      continue;
    }
    // Skip /* ... */ comments.
    if (pos + 1 < css.size() && css[pos] == '/' && css[pos + 1] == '*') {
      size_t end = css.find("*/", pos + 2);
      if (end == std::string_view::npos) {
        return css.size();  // Unterminated comment.
      }
      pos = end + 2;
      continue;
    }
    break;
  }
  return pos;
}

// True for characters that continue a CSS ident: alphanumerics, '-',
// '_', '\\' (which starts an escape) and non-ASCII bytes (ident code
// points per css-syntax).  Used as the boundary after an at-keyword so
// "@importurl" or "@layerfoo" — unknown at-keywords browsers ignore
// entirely — is never mistaken for "@import"/"@layer".
bool IsIdentContinue(char c) {
  unsigned char uc = static_cast<unsigned char>(c);
  return std::isalnum(uc) != 0 || c == '-' || c == '_' || c == '\\' ||
         uc >= 0x80;
}

// ASCII-case-insensitive at-keyword match with an ident boundary after
// it: whitespace, comment, quote, '(', ';' (or end of input) all end
// the keyword, while an ident character extends it into a different,
// unknown at-keyword.
bool MatchesAtKeyword(std::string_view s, std::string_view lowercase_kw) {
  return StartsWithCi(s, lowercase_kw) &&
         (s.size() == lowercase_kw.size() ||
          !IsIdentContinue(s[lowercase_kw.size()]));
}

// Scan of a single at-statement prelude for its terminating ';'.
struct AtStatementScan {
  // Position of the terminating ';' outside comments and strings, or
  // npos when the statement is unterminated (including an unterminated
  // comment or string before any ';').
  size_t semi = std::string_view::npos;
  // Position of the first '{' outside comments and strings before the
  // ';', or npos.  A brace before the terminator means block form
  // (used to classify @layer).
  size_t brace = std::string_view::npos;
  // Prelude text from the scan start up to the ';', with each /*...*/
  // comment replaced by a single space — a comment is a token boundary
  // in CSS, so the token structure is preserved.  Meaningless when
  // semi is npos.
  std::string text;
};

// Scans `css` from `pos` for the ';' that terminates an at-statement.
// A ';' or '{' inside a /*...*/ comment or a string never counts:
// comment-blind scanning let a ';' inside a comment truncate the
// prelude (leaving an open comment to swallow a @media wrapper) and a
// '{' inside one misclassify an @layer statement as a block.
// ExtractImports and RemainderStartsWithImport both build on this
// helper; their prelude models must stay in sync.
AtStatementScan ScanAtStatement(std::string_view css, size_t pos) {
  AtStatementScan scan;
  while (pos < css.size()) {
    char c = css[pos];
    if (c == '/' && pos + 1 < css.size() && css[pos + 1] == '*') {
      size_t end = css.find("*/", pos + 2);
      if (end == std::string_view::npos) return scan;  // Unterminated.
      scan.text += ' ';
      pos = end + 2;
      continue;
    }
    if (c == '"' || c == '\'') {
      scan.text += c;
      ++pos;
      while (pos < css.size() && css[pos] != c) {
        if (css[pos] == '\\' && pos + 1 < css.size()) {
          scan.text += css[pos];
          ++pos;
        }
        scan.text += css[pos];
        ++pos;
      }
      if (pos >= css.size()) return scan;  // Unterminated string.
      scan.text += c;
      ++pos;  // Skip closing quote.
      continue;
    }
    if (c == '\\') {
      // Escape: the next code point is content — an escaped '{' or ';'
      // is an ident character (e.g. in a layer name), not structure.
      scan.text += c;
      ++pos;
      if (pos < css.size()) {
        scan.text += css[pos];
        ++pos;
      }
      continue;
    }
    if (c == ';') {
      scan.semi = pos;
      return scan;
    }
    if (c == '{' && scan.brace == std::string_view::npos) scan.brace = pos;
    scan.text += c;
    ++pos;
  }
  return scan;
}

// True for an ASCII hex digit.
bool IsHexDigit(char c) {
  char lc = net_instaweb::LowerChar(c);
  return (c >= '0' && c <= '9') || (lc >= 'a' && lc <= 'f');
}

// True for a css-syntax newline (CR/LF/FF; CRLF counts as one).
bool IsCssNewline(char c) { return c == '\n' || c == '\r' || c == '\f'; }

// True when a '\\' at `pos` begins a valid escape: css-syntax-3 §4.3.8
// says a backslash is a valid escape unless followed by a newline (or
// by end of input).
bool IsValidEscape(std::string_view css, size_t pos) {
  return pos + 1 < css.size() && !IsCssNewline(css[pos + 1]);
}

// Appends the UTF-8 encoding of a Unicode scalar value.
void AppendUtf8(std::string& out, unsigned int cp) {
  if (cp < 0x80) {
    out.push_back(static_cast<char>(cp));
  } else if (cp < 0x800) {
    out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
    out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
  } else if (cp < 0x10000) {
    out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
    out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
    out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
  } else {
    out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
    out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
    out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
    out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
  }
}

// Consumes one escape sequence and appends the code point it denotes to
// `out`.  `pos` points at the '\\' and is advanced past the sequence.
// Caller must have checked IsValidEscape.  Per css-syntax-3 §4.3.7
// ("Consume an escaped code point"): a run of up to 6 hex digits — with
// one following whitespace consumed as its terminator — denotes that
// code point, where zero, a surrogate, or an out-of-range value becomes
// U+FFFD; any other code point denotes itself.
void ConsumeEscape(std::string_view css, size_t& pos, std::string& out) {
  ++pos;  // Skip '\\'.
  if (pos >= css.size()) {
    AppendUtf8(out, 0xFFFD);
    return;
  }
  if (!IsHexDigit(css[pos])) {
    // The code point denotes itself.  Copy its whole UTF-8 sequence so a
    // multi-byte character is never split.
    size_t start = pos;
    ++pos;
    while (pos < css.size() &&
           (static_cast<unsigned char>(css[pos]) & 0xC0) == 0x80) {
      ++pos;
    }
    out.append(css.substr(start, pos - start));
    return;
  }
  unsigned int cp = 0;
  int digits = 0;
  while (pos < css.size() && digits < 6 && IsHexDigit(css[pos])) {
    char h = net_instaweb::LowerChar(css[pos]);
    cp = cp * 16 + static_cast<unsigned int>(h <= '9' ? h - '0' : h - 'a' + 10);
    ++pos;
    ++digits;
  }
  // A single following whitespace terminates the hex run and is consumed
  // with it (CRLF counts as one).
  if (pos < css.size()) {
    if (css[pos] == '\r' && pos + 1 < css.size() && css[pos + 1] == '\n') {
      pos += 2;
    } else if (css[pos] == ' ' || css[pos] == '\t' || IsCssNewline(css[pos])) {
      ++pos;
    }
  }
  if (cp == 0 || (cp >= 0xD800 && cp <= 0xDFFF) || cp > 0x10FFFF) cp = 0xFFFD;
  AppendUtf8(out, cp);
}

// Appends `value` as a CSS string delimited by `quote`.  Only three
// things need escaping in a string (css-syntax-3 §4.3.5): the delimiter,
// the backslash that starts an escape, and a newline (a raw one is a
// parse error, and a backslash-newline continuation would delete it, so
// the hex form with its terminating space is used).  Everything else —
// notably ')' and the other quote character — is literal.  Escaping
// exactly this set is what makes the writer the inverse of
// ConsumeEscape, so a URL that has been decoded on extraction leaves
// with one level of escaping no matter how many times it is rebased.
void AppendCssQuotedString(std::string& out, std::string_view value,
                           char quote) {
  out.push_back(quote);
  for (char c : value) {
    if (c == quote || c == '\\') {
      out.push_back('\\');
      out.push_back(c);
    } else if (c == '\n') {
      out.append("\\A ");
    } else if (c == '\r') {
      out.append("\\D ");
    } else if (c == '\f') {
      out.append("\\C ");
    } else {
      out.push_back(c);
    }
  }
  out.push_back(quote);
}

// Extract a quoted string starting at pos (pos points to the quote
// character).  Returns the string's VALUE — escape sequences decoded per
// css-syntax-3 §4.3.5 — and advances pos past the closing quote.
// Returns nullopt on error.  Returning the raw bytes while the writers
// re-escaped meant a URL containing a quote or backslash gained a
// backslash at every rebase level.
std::optional<std::string> ExtractQuotedString(std::string_view css,
                                               size_t& pos) {
  if (pos >= css.size()) return std::nullopt;
  char quote = css[pos];
  if (quote != '"' && quote != '\'') return std::nullopt;
  ++pos;
  std::string result;
  while (pos < css.size() && css[pos] != quote) {
    if (css[pos] == '\\') {
      if (pos + 1 >= css.size()) {
        pos = css.size();  // Trailing backslash: string is unterminated.
        break;
      }
      if (IsCssNewline(css[pos + 1])) {
        // Line continuation (§4.3.5): consumed, contributes nothing.
        pos += (css[pos + 1] == '\r' && pos + 2 < css.size() &&
                css[pos + 2] == '\n')
                   ? 3
                   : 2;
        continue;
      }
      ConsumeEscape(css, pos, result);
      continue;
    }
    result.push_back(css[pos]);
    ++pos;
  }
  if (pos >= css.size()) return std::nullopt;  // Unterminated string.
  ++pos;                                       // Skip closing quote.
  return result;
}

// Extract a URL from url(...) starting at pos, where pos points to
// the 'u' of 'url'. Advances pos past the closing ')'.
std::optional<std::string> ExtractUrlFunction(std::string_view css,
                                              size_t& pos) {
  // Expect "url("
  if (pos + 3 >= css.size()) return std::nullopt;
  if (net_instaweb::LowerChar(css[pos]) != 'u' ||
      net_instaweb::LowerChar(css[pos + 1]) != 'r' ||
      net_instaweb::LowerChar(css[pos + 2]) != 'l' || css[pos + 3] != '(') {
    return std::nullopt;
  }
  pos += 4;  // Skip "url("

  // Skip whitespace inside url().
  while (pos < css.size() &&
         (std::isspace(static_cast<unsigned char>(css[pos])) != 0)) {
    ++pos;
  }

  std::string url;
  if (pos < css.size() && (css[pos] == '"' || css[pos] == '\'')) {
    auto quoted = ExtractQuotedString(css, pos);
    if (!quoted) return std::nullopt;
    url = *quoted;
  } else {
    // Unquoted url token (css-syntax-3 §4.3.6): raw content up to the
    // closing ')' or whitespace, with backslash escapes decoded — an
    // escaped ')' is content, not the terminator.  A backslash before a
    // newline makes this a bad-url-token, so extraction fails and the
    // caller keeps the token verbatim.
    std::string decoded;
    while (pos < css.size() && css[pos] != ')' &&
           (std::isspace(static_cast<unsigned char>(css[pos])) == 0)) {
      if (css[pos] == '\\') {
        if (!IsValidEscape(css, pos)) return std::nullopt;
        ConsumeEscape(css, pos, decoded);
        continue;
      }
      decoded.push_back(css[pos]);
      ++pos;
    }
    url = std::move(decoded);
  }

  // Skip whitespace before ')'.
  while (pos < css.size() &&
         (std::isspace(static_cast<unsigned char>(css[pos])) != 0)) {
    ++pos;
  }
  if (pos >= css.size() || css[pos] != ')') return std::nullopt;
  ++pos;  // Skip ')'
  return url;
}

}  // namespace

using pagespeed::ResolvePath;
using pagespeed::UrlDirectory;

namespace {

// Maximum flattened output size (1MB). Prevents runaway expansion from
// deeply nested or diamond import patterns.
constexpr size_t kMaxFlattenedSize = static_cast<size_t>(1024 * 1024);

}  // namespace

// Returns true if a media query string is safe for embedding in
// @media <value> { ... }.  Rejects characters that enable CSS injection
// (braces, semicolons, backslash, @), HTML injection (angle brackets),
// or string breakout (quotes), and a comment opener ("/*"), which would
// swallow the wrapper.  Extraction strips comments from the media value,
// so the "/*" check is defense in depth against scanner drift.
// Unbalanced parentheses are rejected too: the statement scan is not
// component-value-aware, so a ';' hidden inside parens (or inside an
// unquoted url() token) mis-splits the condition and surfaces here as
// an unbalanced fragment; legitimate media queries always balance.
bool IsSafeMediaValue(std::string_view media) {
  int paren_depth = 0;
  for (size_t i = 0; i < media.size(); ++i) {
    char c = media[i];
    if (c == '{' || c == '}' || c == ';' || c == '<' || c == '>' || c == '\\' ||
        c == '\'' || c == '"' || c == '@') {
      return false;
    }
    if (c == '/' && i + 1 < media.size() && media[i + 1] == '*') {
      return false;
    }
    if (c == '(') ++paren_depth;
    if (c == ')') {
      if (paren_depth == 0) return false;
      --paren_depth;
    }
  }
  return paren_depth == 0;
}

std::optional<std::string_view> CombinedSheetMediaScope(
    std::string_view media) {
  const auto is_space = [](char c) {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f';
  };
  while (!media.empty() && is_space(media.front())) media.remove_prefix(1);
  while (!media.empty() && is_space(media.back())) media.remove_suffix(1);
  if (media.empty() || net_instaweb::StringCaseEqual(media, "all") ||
      net_instaweb::StringCaseEqual(media, "screen")) {
    return std::string_view();
  }
  if (!IsSafeMediaValue(media)) return std::nullopt;
  return media;
}

size_t MediaScopedCssSize(std::string_view scope, size_t css_size) {
  // "@media " + scope + "{" + css + "}"
  return scope.empty() ? css_size : css_size + scope.size() + 9;
}

bool AppendMediaScopedCss(std::string_view scope, std::string_view css,
                          std::string* out) {
  if (scope.empty()) {
    out->append(css);
    return true;
  }
  if (!IsStructurallyBalanced(css)) return false;
  out->reserve(out->size() + MediaScopedCssSize(scope, css.size()));
  out->append("@media ");
  out->append(scope);
  out->append("{");
  out->append(css);
  out->append("}");
  return true;
}

namespace {

// Returns true if an @import condition is not a media query: cascade
// layers (bare "layer" or "layer(...)") and feature queries
// ("supports(...)").  Re-emitting these as "@media <condition> {...}"
// evaluates to "not all" and silently drops the imported stylesheet.
// Per the @import grammar, layer()/supports() precede any media query,
// so checking the first token is sufficient.
bool IsNonMediaImportCondition(std::string_view condition) {
  if (StartsWithCi(condition, "layer")) {
    if (condition.size() == 5) return true;  // bare "layer"
    char next = condition[5];
    // '(' = layer(), whitespace = bare layer + media, '/' = comment
    // start (extraction strips comments to a space, so this is defense
    // in depth for e.g. "layer/*c*/screen").
    if (next == '(' || next == '/' ||
        (std::isspace(static_cast<unsigned char>(next)) != 0)) {
      return true;
    }
  }
  return StartsWithCi(condition, "supports") && condition.size() > 8 &&
         condition[8] == '(';
}

// Returns true if a live @import follows within `css`'s statement
// prelude: whitespace, comments, an optional @charset, @layer
// statements (which CSS Cascading L4/L5 allow between @imports), and
// unknown at-STATEMENTS (';' before any top-level '{'), which browsers
// drop as if absent — an @import behind one is still live.  Used on
// the remainder a sheet's import scan did not consume: a live @import
// there is one ExtractImports could not harvest, and inlining the
// sheet would embed it mid-parent.  An at-rule BLOCK or unterminated
// statement ends the import prelude for browsers too, so it returns
// false.  This model is deliberately a SUPERSET of ExtractImports'
// prelude (which stops at the first unknown at-keyword): recognizing
// more here can only convert a flatten into serve-original.  Keep the
// shared pieces — ScanAtStatement, keyword boundaries, the @charset
// guard — identical between the two.
bool RemainderStartsWithImport(std::string_view css) {
  size_t pos = SkipWhitespaceAndComments(css, 0);
  // @charset is byte-exact per spec; at-keywords below match
  // case-insensitively.
  if (pos + 8 <= css.size() && css.substr(pos, 8) == "@charset" &&
      (pos + 8 == css.size() || !IsIdentContinue(css[pos + 8]))) {
    AtStatementScan scan = ScanAtStatement(css, pos + 8);
    if (scan.semi == std::string_view::npos) return false;
    pos = SkipWhitespaceAndComments(css, scan.semi + 1);
  }
  while (pos < css.size() && css[pos] == '@') {
    if (MatchesAtKeyword(css.substr(pos), "@import")) return true;
    size_t after_kw;
    if (MatchesAtKeyword(css.substr(pos), "@layer")) {
      after_kw = pos + 6;
    } else {
      // Unknown at-keyword: skip its ident (escapes consume the next
      // code point) and classify statement vs block below.
      after_kw = pos + 1;
      while (after_kw < css.size() && IsIdentContinue(css[after_kw])) {
        after_kw += (css[after_kw] == '\\') ? 2 : 1;
      }
    }
    // ScanAtStatement stops at the ';', so a recorded brace precedes it.
    AtStatementScan scan = ScanAtStatement(css, after_kw);
    if (scan.semi == std::string_view::npos ||
        scan.brace != std::string_view::npos) {
      return false;  // Block form or unterminated — prelude over.
    }
    pos = SkipWhitespaceAndComments(css, scan.semi + 1);
  }
  return false;
}

// True when the identifier containing the escape at `esc` decodes to
// "url" and is followed by '(': CSS reads that as a url token, whose
// body is raw content, while the balance scan would count its braces.
bool EscapedIdentIsUrlFunction(std::string_view css, size_t esc) {
  // The longest spelling of "url" is three escapes of nine bytes each
  // (backslash, six hex digits, CR LF), so an identifier with more bytes
  // than that before this escape cannot be "url".  The bound also keeps
  // a long run of escapes linear.
  constexpr size_t kMaxUrlSpelling = 27;
  size_t i = esc;
  while (i > 0 && IsIdentContinue(css[i - 1])) {
    if (esc - i >= kMaxUrlSpelling) return false;
    --i;
  }
  std::string name;
  while (i < css.size() && IsIdentContinue(css[i]) && name.size() < 8) {
    if (css[i] != '\\') {
      name.push_back(net_instaweb::LowerChar(css[i]));
      ++i;
      continue;
    }
    ++i;
    if (i >= css.size()) break;
    if (std::isxdigit(static_cast<unsigned char>(css[i])) != 0) {
      unsigned code = 0;
      int digits = 0;
      while (i < css.size() && digits < 6 &&
             std::isxdigit(static_cast<unsigned char>(css[i])) != 0) {
        char h = css[i];
        code = code * 16 + static_cast<unsigned>(
                               h <= '9' ? h - '0' : (h | 0x20) - 'a' + 10);
        ++i;
        ++digits;
      }
      // One whitespace after a hex escape belongs to the escape.
      if (i < css.size() &&
          std::isspace(static_cast<unsigned char>(css[i])) != 0) {
        if (css[i] == '\r' && i + 1 < css.size() && css[i + 1] == '\n') ++i;
        ++i;
      }
      name.push_back(
          code < 0x80 ? net_instaweb::LowerChar(static_cast<char>(code)) : '?');
    } else {
      name.push_back(net_instaweb::LowerChar(css[i]));
      ++i;
    }
  }
  return name == "url" && i < css.size() && css[i] == '(';
}

}  // namespace

// Returns true if `css` can be embedded in another sheet without
// changing how the surrounding rules parse: every /* comment and every
// string is terminated, and braces are balanced (depth never negative,
// zero at the end).  An unclosed comment, string, or block swallows
// every rule the enclosing sheet appends after it (including a @media
// wrapper's closing brace, which would media-gate the rest of the
// parent), while a stray '}' closes a wrapper early so trailing rules
// escape their media condition.  Backslash escapes are honored so an
// escaped brace or quote in a selector is not miscounted.
bool IsStructurallyBalanced(std::string_view css) {
  size_t pos = 0;
  int depth = 0;
  while (pos < css.size()) {
    char c = css[pos];
    // An unquoted url(...) token is raw content per the tokenizer's
    // url-token rule: "/*" inside it is not a comment opener and braces
    // are not structural.  Requires an ident boundary before the 'u'
    // (any other ident + '(' is a normal function whose contents
    // tokenize normally) and a non-quote after the '(' (a quoted url(
    // is a normal function; the string branch below handles it).
    if (net_instaweb::LowerChar(c) == 'u' && pos + 3 < css.size() &&
        (pos == 0 || !IsIdentContinue(css[pos - 1])) &&
        net_instaweb::LowerChar(css[pos + 1]) == 'r' &&
        net_instaweb::LowerChar(css[pos + 2]) == 'l' && css[pos + 3] == '(') {
      size_t p = pos + 4;
      while (p < css.size() &&
             (std::isspace(static_cast<unsigned char>(css[p])) != 0)) {
        ++p;
      }
      if (p < css.size() && (css[p] == '"' || css[p] == '\'')) {
        pos += 4;  // Function form with a string argument.
        continue;
      }
      // Raw url token: consume to the unescaped ')'.
      while (p < css.size() && css[p] != ')') {
        p += (css[p] == '\\' && p + 1 < css.size()) ? 2 : 1;
      }
      if (p >= css.size()) return false;  // Unterminated url token.
      pos = p + 1;
      continue;
    }
    if (c == '/' && pos + 1 < css.size() && css[pos + 1] == '*') {
      size_t end = css.find("*/", pos + 2);
      if (end == std::string_view::npos) return false;  // Open comment.
      pos = end + 2;
      continue;
    }
    if (c == '"' || c == '\'') {
      ++pos;
      while (pos < css.size() && css[pos] != c) {
        // An unescaped newline ends a string early (a bad string), so
        // the quote this scan is looking for is not the one CSS sees.
        if (css[pos] == '\n' || css[pos] == '\r' || css[pos] == '\f') {
          return false;
        }
        if (css[pos] == '\\' && pos + 1 < css.size()) {
          // An escaped newline continues the string; CRLF is one newline.
          if (css[pos + 1] == '\r' && pos + 2 < css.size() &&
              css[pos + 2] == '\n') {
            ++pos;
          }
          ++pos;
        }
        ++pos;
      }
      if (pos >= css.size()) return false;  // Unterminated string.
      ++pos;                                // Skip closing quote.
      continue;
    }
    if (c == '\\') {
      if (pos + 1 >= css.size()) return false;  // Nothing left to escape.
      if (EscapedIdentIsUrlFunction(css, pos)) return false;
      pos += 2;  // Escaped code point outside a string.
      continue;
    }
    if (c == '{') ++depth;
    if (c == '}') {
      if (depth == 0) return false;  // Stray '}' escapes a wrapper.
      --depth;
    }
    ++pos;
  }
  return depth == 0;
}

namespace {

// Internal recursive flattener.  All-or-nothing per sheet: any @import
// that cannot be inlined would land mid-sheet after inlined rules — a
// position where browsers must ignore @import (CSS Cascading L4/L5,
// only @charset and @layer statements may precede one) — silently
// dropping its styles.  On the first such import the sheet is returned
// unchanged and skipped_unresolved_import propagates so every ancestor
// serves its original too.  Hoisting unresolved @imports to the prelude
// was rejected: imported rules must apply at the @import's position in
// the cascade, so hoisting reorders styles — and it makes the output
// bytes depend on cache state.
FlattenResult FlattenImportsRecursive(
    std::string_view css, std::string_view css_url, CssLookupFn& lookup_fn,
    int max_depth, std::string_view media_context,
    std::set<std::pair<std::string, std::string>>& visited) {
  FlattenResult result;

  auto imports = ExtractImports(css);
  if (imports.empty()) {
    result.css = std::string(css);
    // An import-like construct ExtractImports could not parse would be
    // inlined verbatim into the parent; flag so ancestors skip.
    if (RemainderStartsWithImport(css)) {
      result.skipped_unresolved_import = true;
      result.imports_unresolved = 1;
    }
    return result;
  }

  // Depth exhausted with imports remaining: inlining this sheet would
  // embed its @imports mid-parent.
  if (max_depth <= 0) {
    result.css = std::string(css);
    result.imports_unresolved = static_cast<int>(imports.size());
    result.skipped_unresolved_import = true;
    return result;
  }

  // Serve the sheet unchanged; the ancestor chain does the same.
  auto serve_original = [&css]() {
    FlattenResult skipped;
    skipped.css = std::string(css);
    skipped.imports_unresolved = 1;
    skipped.skipped_unresolved_import = true;
    return skipped;
  };

  // Build result by replacing @import statements.
  std::string output;
  output.reserve(css.size());
  size_t last_pos = 0;

  for (const auto& imp : imports) {
    // Non-media @import condition (cascade layer / supports()): flattening
    // cannot preserve semantics.  Skip the whole sheet.
    if (!imp.media.empty() && IsNonMediaImportCondition(imp.media)) {
      result = FlattenResult{};
      result.css = std::string(css);
      result.skipped_non_media_condition = true;
      return result;
    }

    // Unsafe media value (braces, quotes, etc.): cannot be re-emitted as
    // a @media wrapper (CSS injection), cannot survive mid-sheet either.
    if (!imp.media.empty() && !IsSafeMediaValue(imp.media)) {
      return serve_original();
    }

    // Append CSS before this @import.
    output.append(css.substr(last_pos, imp.start_pos - last_pos));
    last_pos = imp.end_pos;

    // Resolve the import URL relative to the current CSS file.
    std::string resolved_url = ResolvePath(UrlDirectory(css_url), imp.url);

    // Effective media condition the child is inlined under: the
    // ancestor wrappers plus this import's own media.  ';' terminates
    // an @import statement, so it cannot occur inside a media value —
    // and IsSafeMediaValue (already enforced above) rejects ';' plus
    // the quotes and escapes that could smuggle one — so ';' is an
    // unambiguous join separator.
    std::string media_key(media_context);
    if (!imp.media.empty()) {
      if (!media_key.empty()) media_key += ';';
      media_key += imp.media;
    }

    // Deduplication is keyed on (URL, effective media condition).  A
    // repeat URL under the SAME condition is a diamond or a cycle
    // back-edge — browsers apply a sheet once per condition and ignore
    // cyclic @imports, so dropping the repeat preserves semantics and
    // prevents exponential blowup (A→B→D, A→C→D would inline D twice).
    // A repeat under a DIFFERENT condition is a distinct application
    // (the same sheet imported for screen and again for print) —
    // browsers apply both, so it is inlined again under its own
    // wrapper.  Termination does not rest on this check: a
    // media-varying cycle exhausts max_depth and serves the original.
    if (visited.count({resolved_url, media_key}) > 0) {
      // Already inlined under this condition — skip the @import.
      ++result.imports_resolved;
      continue;
    }

    auto imported_css = lookup_fn(resolved_url);
    if (!imported_css) {
      // Not in cache.
      return serve_original();
    }

    // A structurally unbalanced child (open comment or string, brace
    // depth not returning to zero, stray '}') changes how this sheet
    // parses once the child is embedded in it.  Unresolvable.
    if (!IsStructurallyBalanced(*imported_css)) {
      return serve_original();
    }

    // Mark as visited permanently (never erase — prevents diamond
    // duplication).  Pollution from a later skip is harmless: the skip
    // discards the whole output.
    visited.insert({resolved_url, media_key});

    // Flatten the imported sheet in ITS OWN URL context first, then
    // rebase the fully flattened result into this sheet's context.
    // Rebasing before recursing (the previous order) rewrote url()-form
    // nested @imports into this sheet's context while the recursion
    // still resolved them against the child's URL — a double rebase
    // that made nested imports in sibling directories unresolvable.
    auto sub_result =
        FlattenImportsRecursive(*imported_css, resolved_url, lookup_fn,
                                max_depth - 1, media_key, visited);

    // A nested sheet carries a non-media @import condition — inlining it
    // would embed an @import mid-sheet (invalid).  Skip the whole sheet.
    if (sub_result.skipped_non_media_condition) {
      result = FlattenResult{};
      result.css = std::string(css);
      result.skipped_non_media_condition = true;
      return result;
    }
    if (sub_result.skipped_unresolved_import) {
      return serve_original();
    }

    // Resolve relative URLs of the (now import-free) flattened sheet.
    std::string rebased =
        ResolveUrlsInCss(sub_result.css, resolved_url, css_url);

    // Wrap in @media if the import had a media query (validated safe
    // above).
    if (!imp.media.empty()) {
      output.append("@media ");
      output.append(imp.media);
      output.append(" {\n");
      output.append(rebased);
      output.append("\n}\n");
    } else {
      output.append(rebased);
    }

    // Size cap: stopping partway would leave the remaining @imports
    // mid-sheet, so an oversized flatten serves the original instead.
    if (output.size() > kMaxFlattenedSize) {
      return serve_original();
    }

    result.imports_resolved += 1 + sub_result.imports_resolved;
  }

  // ExtractImports stops on an @import form it cannot parse; one at the
  // head of the unconsumed remainder would sit after the rules inlined
  // above.  Serve the original.
  if (RemainderStartsWithImport(css.substr(last_pos))) {
    return serve_original();
  }

  // Append remaining CSS after the last @import.
  output.append(css.substr(last_pos));

  result.css = std::move(output);
  return result;
}

}  // namespace

std::vector<CssImport> ExtractImports(std::string_view css) {
  std::vector<CssImport> imports;
  size_t pos = 0;

  // Skip @charset if present (byte-exact per spec; at-keywords below
  // match case-insensitively).
  pos = SkipWhitespaceAndComments(css, pos);
  if (pos + 8 <= css.size() && css.substr(pos, 8) == "@charset" &&
      (pos + 8 == css.size() || !IsIdentContinue(css[pos + 8]))) {
    AtStatementScan scan = ScanAtStatement(css, pos + 8);
    if (scan.semi != std::string_view::npos) {
      pos = scan.semi + 1;
    }
  }

  // This scan stops at the first construct it cannot harvest;
  // RemainderStartsWithImport recognizes a SUPERSET of this prelude
  // (it also scans past unknown at-statements) so a live @import
  // behind anything unharvested forces serve-original.  Keep the
  // shared pieces — ScanAtStatement, keyword boundaries, the @charset
  // guard — identical between the two.
  while (pos < css.size()) {
    pos = SkipWhitespaceAndComments(css, pos);
    if (pos >= css.size()) break;

    // Check for @import.
    if (css[pos] != '@') break;  // Non-@import rule → stop.
    if (pos + 7 >= css.size()) break;

    // Allow @layer STATEMENTS before/between @imports (CSS Cascading
    // L4/L5).  An @layer BLOCK ends the import prelude: browsers ignore
    // any @import after it, so harvesting one would inline styles the
    // original page never applied.
    if (MatchesAtKeyword(css.substr(pos), "@layer")) {
      // ScanAtStatement stops at the ';', so a recorded brace precedes
      // it.
      AtStatementScan scan = ScanAtStatement(css, pos + 6);
      if (scan.semi == std::string_view::npos ||
          scan.brace != std::string_view::npos) {
        break;  // Block form or unterminated — prelude over.
      }
      pos = scan.semi + 1;
      continue;
    }

    if (!MatchesAtKeyword(css.substr(pos), "@import")) break;

    size_t import_start = pos;
    pos += 7;  // Skip "@import"

    // Skip whitespace after @import.
    while (pos < css.size() &&
           (std::isspace(static_cast<unsigned char>(css[pos])) != 0)) {
      ++pos;
    }
    if (pos >= css.size()) break;

    CssImport imp;
    imp.start_pos = import_start;

    // Extract URL: either url(...) or "..." or '...'
    if (pos < css.size() && net_instaweb::LowerChar(css[pos]) == 'u') {
      auto url = ExtractUrlFunction(css, pos);
      if (!url) break;
      imp.url = *url;
    } else if (pos < css.size() && (css[pos] == '"' || css[pos] == '\'')) {
      auto url = ExtractQuotedString(css, pos);
      if (!url) break;
      imp.url = *url;
    } else {
      break;  // Invalid @import syntax.
    }

    // Skip whitespace and comments so a leading /*...*/ can't mask the
    // first condition token (layer/supports detection keys on it).
    pos = SkipWhitespaceAndComments(css, pos);

    // Extract optional media query (everything up to the comment-aware
    // ';' — a ';' inside a /*...*/ comment must not terminate the
    // statement).  Comments in the media value are token boundaries and
    // are replaced by a space, so the re-emitted @media prelude keeps
    // the original token structure.
    if (pos < css.size() && css[pos] != ';') {
      AtStatementScan scan = ScanAtStatement(css, pos);
      if (scan.semi == std::string_view::npos) break;
      std::string media = std::move(scan.text);
      // Trim trailing whitespace from media.
      while (!media.empty() &&
             (std::isspace(static_cast<unsigned char>(media.back())) != 0)) {
        media.pop_back();
      }
      imp.media = std::move(media);
      pos = scan.semi;
    }

    if (pos >= css.size() || css[pos] != ';') break;
    ++pos;  // Skip ';'

    imp.end_pos = pos;
    imports.push_back(std::move(imp));
  }

  return imports;
}

std::string ResolveUrlsInCss(std::string_view css, std::string_view import_url,
                             std::string_view parent_url) {
  std::string_view import_dir = UrlDirectory(import_url);
  std::string_view parent_dir = UrlDirectory(parent_url);

  // If directories are the same, no rewriting needed.
  if (import_dir == parent_dir) {
    return std::string(css);
  }

  std::string result;
  result.reserve(css.size());
  size_t pos = 0;

  while (pos < css.size()) {
    // String-form @import ("..."/'...') — needs the same rebase as the
    // url() form below: a kept child-relative URL would resolve against
    // the wrong base once the sheet is embedded in the parent's context.
    // The url() form falls through to the general url( branch.
    // Case-insensitive: browsers match at-keywords that way.
    if (css[pos] == '@' && StartsWithCi(css.substr(pos), "@import")) {
      size_t stmt_start = pos;
      size_t after = SkipWhitespaceAndComments(css, pos + 7);
      if (after < css.size() && (css[after] == '"' || css[after] == '\'')) {
        char quote = css[after];
        size_t str_pos = after;
        auto url = ExtractQuotedString(css, str_pos);
        if (url && !url->empty() && (*url)[0] != '/' && (*url)[0] != '#' &&
            url->find("://") == std::string::npos &&
            !url->starts_with("data:")) {
          std::string resolved = ResolvePath(import_dir, *url);
          if (resolved.starts_with(parent_dir)) {
            resolved = resolved.substr(parent_dir.size());
          }
          // "@import" plus any whitespace/comments, verbatim.
          result.append(css.substr(stmt_start, after - stmt_start));
          AppendCssQuotedString(result, resolved, quote);
          pos = str_pos;
          continue;
        }
      }
      // url() form, absolute/data: string, or malformed: emit the token
      // and let the general scanner handle what follows.
      result.append(css.substr(stmt_start, 7));
      pos = stmt_start + 7;
      continue;
    }

    // Look for url( -- case insensitive.
    if (pos + 3 < css.size() && net_instaweb::LowerChar(css[pos]) == 'u' &&
        net_instaweb::LowerChar(css[pos + 1]) == 'r' &&
        net_instaweb::LowerChar(css[pos + 2]) == 'l' && css[pos + 3] == '(') {
      size_t url_start = pos;
      auto url = ExtractUrlFunction(css, pos);

      if (url && !url->empty() && (*url)[0] != '/' && (*url)[0] != '#' &&
          url->find("://") == std::string::npos && !url->starts_with("data:")) {
        // Relative URL: resolve against import directory, then make
        // relative to parent directory.
        std::string resolved = ResolvePath(import_dir, *url);

        // If parent_dir is a prefix of resolved, make it relative.
        if (resolved.starts_with(parent_dir)) {
          resolved = resolved.substr(parent_dir.size());
        }

        // Emit the rewritten URL in the quoted form, escaped once.
        result.append("url(");
        AppendCssQuotedString(result, resolved, '"');
        result.push_back(')');
      } else {
        // Absolute URL, data: URL, or extraction failed: keep as-is.
        result.append(css.substr(url_start, pos - url_start));
      }
      continue;
    }

    // Skip over CSS comments to avoid rewriting url() inside comments.
    if (pos + 1 < css.size() && css[pos] == '/' && css[pos + 1] == '*') {
      size_t end = css.find("*/", pos + 2);
      if (end == std::string_view::npos) {
        result.append(css.substr(pos));
        pos = css.size();
      } else {
        result.append(css.substr(pos, end + 2 - pos));
        pos = end + 2;
      }
      continue;
    }

    // Skip over strings to avoid false url() matches.
    if (css[pos] == '"' || css[pos] == '\'') {
      char quote = css[pos];
      result.push_back(css[pos]);
      ++pos;
      while (pos < css.size() && css[pos] != quote) {
        if (css[pos] == '\\' && pos + 1 < css.size()) {
          result.push_back(css[pos]);
          ++pos;
        }
        result.push_back(css[pos]);
        ++pos;
      }
      if (pos < css.size()) {
        result.push_back(css[pos]);
        ++pos;
      }
      continue;
    }

    result.push_back(css[pos]);
    ++pos;
  }

  return result;
}

FlattenResult FlattenImports(std::string_view css, std::string_view css_url,
                             CssLookupFn lookup_fn, int max_depth) {
  std::set<std::pair<std::string, std::string>> visited;
  visited.insert({std::string(css_url), ""});
  return FlattenImportsRecursive(css, css_url, lookup_fn, max_depth, "",
                                 visited);
}

}  // namespace pagespeed::css
