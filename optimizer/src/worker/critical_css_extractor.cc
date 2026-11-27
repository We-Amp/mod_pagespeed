// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

// PageSpeed 2.0 - Critical CSS Extractor Implementation

#include "src/worker/critical_css_extractor.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "absl/container/flat_hash_map.h"
#include "absl/container/flat_hash_set.h"
#include "absl/strings/ascii.h"
#include "absl/strings/match.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/strip.h"
#include "lib/base/string_util.h"
#include "lib/classify/capability_mask.h"
#include "src/browser/device_emulation.h"
#include "src/worker/html_css_injector.h"

namespace pagespeed {

struct CssRule {
  std::string selector;
  std::string body;  // includes braces
};

// Depth bound shared by every recursive walk over the rule tree (ProcessRules,
// CollectSelectorContexts). They must agree: a walk that gives up earlier than
// ProcessRules reports a rule as absent that ProcessRules will happily emit.
inline constexpr int kMaxRuleRecursionDepth = 5;

namespace {

// Skip whitespace and comments in CSS
size_t SkipWhitespaceAndComments(std::string_view css, size_t pos) {
  while (pos < css.size()) {
    // Skip whitespace
    while (pos < css.size() &&
           (std::isspace(static_cast<unsigned char>(css[pos])) != 0)) {
      ++pos;
    }

    // Check for comment start
    if (pos + 1 < css.size() && css[pos] == '/' && css[pos + 1] == '*') {
      pos += 2;
      // Find comment end
      while (pos + 1 < css.size() &&
             !(css[pos] == '*' && css[pos + 1] == '/')) {
        ++pos;
      }
      if (pos + 1 < css.size()) {
        pos += 2;  // Skip */
      } else {
        return std::string_view::npos;  // Unterminated comment
      }
    } else {
      break;
    }
  }
  return pos;
}

bool IsCssWhitespace(char c) {
  return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f';
}

// One past the escape that starts at css[i] (a backslash), per CSS Syntax 3
// §4.3.7: one to six hex digits and a single whitespace after them (CRLF
// counts as one), or else any one code point, quotes and braces included. A
// backslash at the end of input escapes nothing. Outside a string a backslash
// before a newline is not an escape, but the newline is whitespace either way,
// so skipping it changes nothing for the scanners here. The bytes of a
// multi-byte code point after the first are never ASCII, so skipping one
// byte is enough to step over any delimiter.
size_t CssEscapeEnd(std::string_view css, size_t i) {
  ++i;  // the backslash
  if (i >= css.size()) return i;
  if (std::isxdigit(static_cast<unsigned char>(css[i])) == 0) return i + 1;
  const size_t hex_start = i;
  while (i < css.size() && i - hex_start < 6 &&
         std::isxdigit(static_cast<unsigned char>(css[i])) != 0) {
    ++i;
  }
  if (i + 1 < css.size() && css[i] == '\r' && css[i + 1] == '\n') return i + 2;
  if (i < css.size() && IsCssWhitespace(css[i])) return i + 1;
  return i;
}

// One past the string whose opening quote is css[i] (CSS Syntax 3 §4.3.5):
// escapes inside it are skipped, the matching quote ends it, and an unescaped
// newline ends it as a bad string (the newline is not part of it), as does the
// end of input.
size_t CssStringEnd(std::string_view css, size_t i) {
  const char quote = css[i];
  ++i;
  while (i < css.size()) {
    const char c = css[i];
    if (c == '\\') {
      // Inside a string an escaped newline is a line continuation; CRLF is
      // one newline.
      i += (i + 2 < css.size() && css[i + 1] == '\r' && css[i + 2] == '\n') ? 3
                                                                            : 2;
      continue;
    }
    if (c == quote) return i + 1;
    if (c == '\n' || c == '\r' || c == '\f') return i;
    ++i;
  }
  return css.size();
}

// When css[i] starts an unquoted `url(` token (CSS Syntax 3 §4.3.6), one
// past its closing ')'; npos otherwise. Such a URL is raw text up to the ')':
// a quote in it (`url(it's.png)`) makes it a bad URL, which browsers skip up
// to the ')' as well, so it opens no string, and an escaped ')' does not end
// it. `url("...")` is an ordinary function whose argument is a string, so
// npos.
size_t CssUnquotedUrlEnd(std::string_view css, size_t i) {
  if (i + 4 > css.size() || (css[i] != 'u' && css[i] != 'U') ||
      (css[i + 1] != 'r' && css[i + 1] != 'R') ||
      (css[i + 2] != 'l' && css[i + 2] != 'L') || css[i + 3] != '(') {
    return std::string_view::npos;
  }
  if (i > 0) {
    // `url(` must be a whole ident, not the tail of `foo-url(`.
    const unsigned char before = static_cast<unsigned char>(css[i - 1]);
    if (std::isalnum(before) != 0 || before == '-' || before == '_' ||
        before == '\\' || before >= 0x80) {
      return std::string_view::npos;
    }
  }
  size_t j = i + 4;
  while (j < css.size() && IsCssWhitespace(css[j])) ++j;
  if (j < css.size() && (css[j] == '"' || css[j] == '\'')) {
    return std::string_view::npos;
  }
  while (j < css.size()) {
    if (css[j] == '\\') {
      j += 2;
      continue;
    }
    if (css[j] == ')') return j + 1;
    ++j;
  }
  return css.size();
}

// One past the comment that starts at css[i] ("/*"), or the end of input.
size_t CssCommentEnd(std::string_view css, size_t i) {
  size_t end = css.find("*/", i + 2);
  return end == std::string_view::npos ? css.size() : end + 2;
}

bool StartsCssComment(std::string_view css, size_t i) {
  return i + 1 < css.size() && css[i] == '/' && css[i + 1] == '*';
}

// Find the end of a CSS block (matching braces). Braces inside comments,
// strings and escapes (`.a\{`, `.a\'b`) do not count.
size_t FindBlockEnd(std::string_view css, size_t start) {
  int depth = 0;
  size_t i = start;
  while (i < css.size()) {
    const char c = css[i];
    if (StartsCssComment(css, i)) {
      i = CssCommentEnd(css, i);
      continue;
    }
    if (c == '\\') {
      i = CssEscapeEnd(css, i);
      continue;
    }
    if (c == '"' || c == '\'') {
      i = CssStringEnd(css, i);
      continue;
    }
    if (c == 'u' || c == 'U') {
      const size_t url_end = CssUnquotedUrlEnd(css, i);
      if (url_end != std::string_view::npos) {
        i = url_end;
        continue;
      }
    }
    if (c == '{') {
      ++depth;
    } else if (c == '}') {
      if (depth <= 0) {
        // Unmatched closing brace — treat as end of block.
        return i + 1;
      }
      --depth;
      if (depth == 0) {
        return i + 1;
      }
    }
    ++i;
  }
  return css.size();
}

// Find the first '{' at or after `start` that is not inside a comment, a CSS
// string, an escape or a [...] attribute selector. Returns npos if none found.
size_t FindOpenBrace(std::string_view css, size_t start) {
  bool in_bracket = false;  // inside [...]
  size_t i = start;
  while (i < css.size()) {
    const char c = css[i];
    if (StartsCssComment(css, i)) {
      i = CssCommentEnd(css, i);
      continue;
    }
    if (c == '\\') {
      i = CssEscapeEnd(css, i);
      continue;
    }
    if (c == '"' || c == '\'') {
      i = CssStringEnd(css, i);
      continue;
    }
    if (c == 'u' || c == 'U') {
      const size_t url_end = CssUnquotedUrlEnd(css, i);
      if (url_end != std::string_view::npos) {
        i = url_end;
        continue;
      }
    }
    if (c == '[') {
      in_bracket = true;
    } else if (c == ']') {
      in_bracket = false;
    } else if (c == '{' && !in_bracket) {
      return i;
    }
    ++i;
  }
  return std::string_view::npos;
}

size_t FindMatchingParen(std::string_view s, size_t open);

// Find the first top-level ';' in [start, limit) that terminates a statement
// at-rule (no block), skipping strings, comments, escapes, [...] attribute
// selectors and balanced (...) groups (`@supports (a;b) {`, an at-rule
// prelude with a `;` in parentheses, is one block rule, not a statement and
// a rule with the selector `b)`). Returns npos if none, and
// when a `(` is never closed: a browser reads to the end of the sheet
// there. Used to recognize block-less at-rules such as the Tailwind v4
// layer-order declaration `@layer theme,base,...;` and `@import url(...);`,
// which the brace-oriented parser would otherwise fuse with the next rule's
// selector.
size_t FindTopLevelSemicolon(std::string_view css, size_t start, size_t limit) {
  bool in_bracket = false;
  limit = std::min(limit, css.size());
  size_t i = start;
  while (i < limit) {
    const char c = css[i];
    if (StartsCssComment(css, i)) {
      i = CssCommentEnd(css, i);
      continue;
    }
    if (c == '\\') {
      i = CssEscapeEnd(css, i);
      continue;
    }
    if (c == '"' || c == '\'') {
      i = CssStringEnd(css, i);
      continue;
    }
    if (c == 'u' || c == 'U') {
      const size_t url_end = CssUnquotedUrlEnd(css, i);
      if (url_end != std::string_view::npos) {
        i = url_end;
        continue;
      }
    }
    if (c == '(') {
      const size_t close = FindMatchingParen(css, i);
      if (close == std::string_view::npos) return std::string_view::npos;
      i = close + 1;
      continue;
    }
    if (c == '[') {
      in_bracket = true;
    } else if (c == ']') {
      in_bracket = false;
    } else if (c == ';' && !in_bracket) {
      return i;
    }
    ++i;
  }
  return std::string_view::npos;
}

// Parse CSS rules from input
std::vector<CssRule> ParseCssRules(std::string_view css) {
  std::vector<CssRule> rules;
  size_t pos = 0;

  while (pos < css.size()) {
    pos = SkipWhitespaceAndComments(css, pos);
    if (pos >= css.size()) {
      break;
    }

    // Find the start of the block, skipping braces inside strings and
    // attribute selectors (e.g., div[data-x="{"] { ... }).
    size_t brace_pos = FindOpenBrace(css, pos);

    // A block-less statement AT-rule (only '@...;' — never bare non-at text,
    // which stays malformed garbage) ends at a top-level ';' before the next
    // '{'. Capture it with an empty body so ProcessRules can preserve it (a
    // leading `@layer a,b,c;` establishes cascade order for the layers below).
    size_t limit =
        (brace_pos == std::string_view::npos) ? css.size() : brace_pos;
    size_t semi_pos = FindTopLevelSemicolon(css, pos, limit);
    if (semi_pos != std::string_view::npos) {
      std::string_view stmt_sv =
          absl::StripAsciiWhitespace(css.substr(pos, semi_pos - pos));
      if (!stmt_sv.empty() && stmt_sv.front() == '@') {
        rules.push_back({std::string(stmt_sv), std::string()});
        pos = semi_pos + 1;
        continue;
      }
    }

    if (brace_pos == std::string_view::npos) {
      break;
    }

    // Extract selector
    std::string_view selector_sv = css.substr(pos, brace_pos - pos);
    std::string selector(absl::StripAsciiWhitespace(selector_sv));

    // Find block end
    size_t block_end = FindBlockEnd(css, brace_pos);

    // Extract body including braces
    std::string body(css.substr(brace_pos, block_end - brace_pos));

    if (!selector.empty()) {
      rules.push_back({std::move(selector), std::move(body)});
    }

    pos = block_end;
  }

  return rules;
}

// Convert string to lowercase for case-insensitive comparison
std::string ToLower(std::string_view str) {
  return net_instaweb::AsciiToLower(str);
}

// Match an at-rule by NAME, not by prefix: `@import` must not match
// `@importantly`. The boundary is "next char cannot continue a CSS ident"
// rather than the container helper's whitespace-or-brace set, because a
// minifier legally emits `@import"x.css"` and `@charset"utf-8"` with no
// separator at all.
bool AtRuleNameIs(std::string_view selector, std::string_view name) {
  if (!absl::StartsWithIgnoreCase(selector, name)) return false;
  if (selector.size() == name.size()) return true;
  unsigned char next = static_cast<unsigned char>(selector[name.size()]);
  if (next >= 0x80) return false;  // non-ASCII continues an ident
  return (std::isalnum(next) == 0) && next != '_' && next != '-' &&
         next != '\\';
}

// Check if the rule is an @-rule that should always be included
bool IsAlwaysIncludedAtRule(std::string_view selector) {
  // Always include @charset, @import, @font-face, @keyframes.
  //
  // @property / @counter-style / @font-palette-values / @font-feature-values /
  // @position-try are registrations, not styling: a RETAINED rule referencing
  // an unregistered custom property, counter style, palette, feature set or
  // position-try tactic is invalid at computed-value time, so dropping the
  // registration makes the rules that survived compute the WRONG value rather
  // than merely omitting a rule. @namespace likewise re-interprets the type
  // selectors of every rule that survives. None may be filtered out.
  return AtRuleNameIs(selector, "@charset") ||
         AtRuleNameIs(selector, "@import") ||
         AtRuleNameIs(selector, "@namespace") ||
         AtRuleNameIs(selector, "@font-face") ||
         AtRuleNameIs(selector, "@font-feature-values") ||
         AtRuleNameIs(selector, "@font-palette-values") ||
         AtRuleNameIs(selector, "@property") ||
         AtRuleNameIs(selector, "@position-try") ||
         AtRuleNameIs(selector, "@counter-style") ||
         AtRuleNameIs(selector, "@keyframes") ||
         AtRuleNameIs(selector, "@-webkit-keyframes") ||
         AtRuleNameIs(selector, "@-moz-keyframes");
}

}  // namespace

// ---------------------------------------------------------------------------
// Media-query width evaluation (public: see critical_css_extractor.h)
// ---------------------------------------------------------------------------

MediaWidthRange RetentionWidthRange(CapabilityMask::Viewport viewport) {
  // Rule retention asks "can this @media block apply to a window this
  // variant is served to?", and the variant is chosen by the User-Agent
  // (ParseViewport in lib/classify/capability_mask.cc), not by the window.
  // So each class gets the whole range of window widths its User-Agents
  // plausibly render at, and the classes overlap. They are deliberately NOT
  // CapabilityMask::ViewportWidthRange (a partition, 0-479 / 480-1279 /
  // 1280+, used to pick a class from a known width) nor the single widths
  // the browser analysis renders at (375 / 768 / 1440 in
  // BrowserAnalysisManager::AnalysisContext): a block dropped for a width
  // the class can still have is missing for that window until the full
  // stylesheet applies (a desktop UA in a 375 px window lost
  // the `(width<=420px)` overrides, a 0.02 fold-pixel flash).
  //
  // Lower bound, every class: 0. Real windows narrower than the narrowest
  // phone exist under every User-Agent class: KaiOS phones are 240 wide (their
  // UA says "Mobile"), Galaxy Fold cover screens 280 (`max-width: 280px`
  // rules are common), and page zoom shrinks any window's CSS width. A
  // desktop User-Agent can be any width at all (a narrow window, zoom,
  // devtools device mode), which is why its range is everything.
  //
  // Mobile upper bound: 980. The widest phone viewport is a large phone in
  // landscape (iPhone 16 Pro Max: 956; Pixel and Galaxy flagships 915 or
  // less), and a page with no `<meta name="viewport">` is laid out 980 px
  // wide on iOS and Android whatever the device.
  //
  // Tablet upper bound: 1480. The widest tablet viewports are landscape
  // (iPad Pro 13": 1376; Galaxy Tab S9 Ultra: 1480 x 924 CSS px at DPR 2).
  //
  // These upper bounds hold for a page's own layout, not for every window a
  // User-Agent can have (page zoom out, `<meta name="viewport"
  // content="width=1024">`, initial-scale below 1). That is tolerable only
  // where the block precedes the sheet; see RetentionWidthRangeForCss.
  switch (viewport) {
    case CapabilityMask::Viewport::kMobile:
      return {0, 980};
    case CapabilityMask::Viewport::kTablet:
      return {0, 1480};
    case CapabilityMask::Viewport::kDesktop:
      break;
  }
  return {0, kMediaWidthUnbounded};
}

MediaWidthRange RetentionWidthRangeForCss(CapabilityMask::Viewport viewport,
                                          std::string_view css,
                                          const CascadeLayerOrder& order,
                                          bool* class_range) {
  // A block that goes AFTER the sheets (DecideCriticalCssLayerPlacement keeps
  // the </head> fallback for a block that uses a cascade layer when the page's
  // layer order is not proven, or does not list every layer the block names)
  // wins every same-specificity tie against the sheet. Dropping an override
  // there while keeping its base rule is permanent for any window the
  // override matches, not a flash: a mobile UA zoomed out past 980 px would
  // keep `lg:hidden` content visible, or `hidden lg:block` content hidden,
  // for as long as the page is open. So retention keeps every block that can
  // apply to ANY window, and drops only blocks that never apply (print).
  //
  // A block that goes FIRST (no layer, or a layered block behind the page's
  // proven `@layer` statement) loses every tie to the sheet, so
  // a window outside the class range lacks a dropped override only until the
  // sheet applies: the class range is enough.
  //
  // Known edge, as for placement (HtmlTransformFilter::TrackCriticalCssAnchor):
  // a sheet that comes before `<meta charset>`, a Content-Type/CSP
  // `<meta http-equiv>` or `<base>` resets the anchor, so the block lands
  // after THAT sheet and wins its ties; an override in it that the narrowed
  // block dropped stays lost for a window outside the class range. Rare (a
  // stylesheet ahead of the charset declaration), and accepted there too.
  //
  // Not with an anonymous layer, though: the block's anonymous layer is a
  // different, earlier layer than the sheet's, and an earlier layer WINS for
  // `!important` (CriticalCssRetentionMayNarrow).
  //
  // Decided on the combined CSS rather than on the finished block: placement
  // reads the block and retention shapes it, and a block cannot name a layer
  // the CSS it came from does not. Extract() checks the finished block too.
  const bool narrow = CriticalCssRetentionMayNarrow(css, order);
  if (class_range != nullptr) *class_range = narrow;
  if (!narrow) return {0, kMediaWidthUnbounded};
  return RetentionWidthRange(viewport);
}

MediaPointer RetentionPointer(CapabilityMask::Viewport viewport) {
  // The phone and tablet analysis renders emulate a touch screen
  // (src/browser/device_emulation.h), and so do the browsers of
  // every visitor those classes stand for: a User-Agent with "Mobile",
  // "Android", "iPhone", "iPod", "iPad" or "Tablet" (ParseViewport in
  // lib/classify/capability_mask.cc) is a phone or a tablet, with
  // `hover: none` and `pointer: coarse`. Reading the
  // features the same way here keeps the derived block and its validation
  // render in agreement: a `(hover: none)` block the render applies is kept,
  // a `(hover: hover)` block the render never applies is dropped.
  //
  // The desktop class is NOT the reverse (`hover: hover`), for the reason its
  // retention range is every width: the class comes from the User-Agent, and
  // a desktop User-Agent can be a touch device. iPadOS Safari sends a desktop
  // "Macintosh" UA by default and lands here, as do 2-in-1 laptops in tablet
  // mode. Dropping `(hover: none)` / `(pointer: coarse)` blocks from the
  // desktop block would leave those visitors without them until the sheet
  // applies, while keeping them costs the desktop render nothing: a block it
  // does not match is inert in the validation render. So the desktop keeps
  // both, as before.
  return EmulationForViewport(viewport).has_touch ? MediaPointer::kTouch
                                                  : MediaPointer::kUnknown;
}

namespace {

// A set of integral window widths in CSS px, as sorted, disjoint, inclusive
// intervals within [0, kMediaWidthUnbounded].
using WidthSet = std::vector<std::pair<int64_t, int64_t>>;

constexpr int64_t kWidthMax = kMediaWidthUnbounded;

WidthSet AllWidths() { return {{0, kWidthMax}}; }

WidthSet WidthInterval(int64_t lo, int64_t hi) {
  lo = std::max<int64_t>(lo, 0);
  hi = std::min<int64_t>(hi, kWidthMax);
  if (lo > hi) return {};
  return {{lo, hi}};
}

WidthSet Intersect(const WidthSet& a, const WidthSet& b) {
  WidthSet out;
  size_t i = 0;
  size_t j = 0;
  while (i < a.size() && j < b.size()) {
    int64_t lo = std::max(a[i].first, b[j].first);
    int64_t hi = std::min(a[i].second, b[j].second);
    if (lo <= hi) out.emplace_back(lo, hi);
    if (a[i].second < b[j].second) {
      ++i;
    } else {
      ++j;
    }
  }
  return out;
}

WidthSet Union(const WidthSet& a, const WidthSet& b) {
  WidthSet all = a;
  all.insert(all.end(), b.begin(), b.end());
  std::sort(all.begin(), all.end());
  WidthSet out;
  for (const auto& iv : all) {
    if (!out.empty() && iv.first <= out.back().second + 1) {
      out.back().second = std::max(out.back().second, iv.second);
    } else {
      out.push_back(iv);
    }
  }
  return out;
}

WidthSet Complement(const WidthSet& a) {
  WidthSet out;
  int64_t next = 0;
  for (const auto& iv : a) {
    if (iv.first > next) out.emplace_back(next, iv.first - 1);
    next = iv.second + 1;
  }
  if (next <= kWidthMax) out.emplace_back(next, kWidthMax);
  return out;
}

// What a media condition says about the window width, bracketed from both
// sides: `may` holds every width at which the condition can be true, `must`
// every width at which it is certainly true. A width feature is exact
// (may == must), and so is a hover/pointer feature under a known pointer
// (true or false at every width); a feature the evaluator does not read
// (orientation, prefers-*, height, hover/pointer with an unknown pointer, ...)
// can be true or false at any width (may = all, must = none). `not` swaps
// and complements the two, which keeps both brackets sound through any
// nesting.
struct WidthVerdict {
  WidthSet may;
  WidthSet must;
};

WidthVerdict Exact(WidthSet s) { return {s, s}; }
WidthVerdict Unknown() { return {AllWidths(), {}}; }
WidthVerdict Not(const WidthVerdict& v) {
  return {Complement(v.must), Complement(v.may)};
}
WidthVerdict And(const WidthVerdict& a, const WidthVerdict& b) {
  return {Intersect(a.may, b.may), Intersect(a.must, b.must)};
}
WidthVerdict Or(const WidthVerdict& a, const WidthVerdict& b) {
  return {Union(a.may, b.may), Union(a.must, b.must)};
}

bool IsIdentChar(char c) {
  return (std::isalnum(static_cast<unsigned char>(c)) != 0) || c == '-' ||
         c == '_';
}

// Parse a CSS length at the front of `s` (whitespace already stripped) into
// CSS px, consuming it. Accepts a bare `0` and the absolute units plus
// em/rem, which in a media query are relative to the initial font size
// (16 px), never to the page's own root font size.
std::optional<double> ConsumeLength(std::string_view& s) {
  size_t pos = 0;
  if (pos < s.size() && s[pos] == '+') ++pos;
  size_t digits_start = pos;
  while (pos < s.size() &&
         ((std::isdigit(static_cast<unsigned char>(s[pos])) != 0) ||
          s[pos] == '.')) {
    ++pos;
  }
  if (pos == digits_start || pos - digits_start > 12) return std::nullopt;
  std::string number(s.substr(digits_start, pos - digits_start));
  char* end = nullptr;
  double value = std::strtod(number.c_str(), &end);
  if (end != number.c_str() + number.size()) return std::nullopt;
  size_t unit_start = pos;
  while (pos < s.size() && IsIdentChar(s[pos])) ++pos;
  std::string_view unit = s.substr(unit_start, pos - unit_start);
  double px = 0;
  if (unit == "px") {
    px = value;
  } else if (unit == "em" || unit == "rem") {
    px = value * 16.0;
  } else if (unit == "in") {
    px = value * 96.0;
  } else if (unit == "cm") {
    px = value * 96.0 / 2.54;
  } else if (unit == "mm") {
    px = value * 96.0 / 25.4;
  } else if (unit == "q") {
    px = value * 96.0 / 101.6;
  } else if (unit == "pt") {
    px = value * 96.0 / 72.0;
  } else if (unit == "pc") {
    px = value * 16.0;
  } else if (unit.empty() && value == 0.0) {
    px = 0;
  } else {
    return std::nullopt;
  }
  s.remove_prefix(pos);
  return px;
}

// The integral widths w with `w OP x` (x in CSS px). Windows are laid out at
// whole CSS px, so strict bounds fold to the next whole pixel: `w < 768` is
// at most 767, `w > 767.98` at least 768.
WidthSet Compare(std::string_view op, double x) {
  auto floor_px = static_cast<int64_t>(std::floor(x));
  auto ceil_px = static_cast<int64_t>(std::ceil(x));
  if (op == ">=") return WidthInterval(ceil_px, kWidthMax);
  if (op == ">") return WidthInterval(floor_px + 1, kWidthMax);
  if (op == "<=") return WidthInterval(0, floor_px);
  if (op == "<") return WidthInterval(0, ceil_px - 1);
  if (op == "=") return WidthInterval(ceil_px, floor_px);
  return {};
}

// The mirror of an operator, for `x OP width` read as `width OP' x`.
std::string_view FlipOp(std::string_view op) {
  if (op == ">=") return "<=";
  if (op == ">") return "<";
  if (op == "<=") return ">=";
  if (op == "<") return ">";
  return op;
}

std::string_view ConsumeOp(std::string_view& s) {
  for (std::string_view op : {">=", "<=", ">", "<", "="}) {
    if (absl::StartsWith(s, op)) {
      s.remove_prefix(op.size());
      return op;
    }
  }
  return {};
}

// The verdict on `name: value` for the hover/pointer features under a known
// pointer (Media Queries Level 4 §7; `value` empty for the boolean form,
// which is true when the feature is not `none`). nullopt for a value the
// feature cannot take.
std::optional<WidthVerdict> EvaluatePointerFeature(std::string_view name,
                                                   std::string_view value,
                                                   MediaPointer pointer) {
  // kTouch: hover and any-hover are `none`, pointer and any-pointer `coarse`.
  // A touch emulation sets both the primary and the any-* features, as a
  // phone with only its screen reports them.
  if (pointer != MediaPointer::kTouch) return Unknown();
  const bool is_hover = name == "hover" || name == "any-hover";
  std::string_view actual = is_hover ? "none" : "coarse";
  bool applies = false;
  if (value.empty()) {
    applies = actual != "none";
  } else if (is_hover
                 ? (value == "none" || value == "hover")
                 : (value == "none" || value == "coarse" || value == "fine")) {
    applies = value == actual;
  } else {
    return std::nullopt;
  }
  return Exact(applies ? AllWidths() : WidthSet{});
}

bool IsPointerFeature(std::string_view name) {
  return name == "hover" || name == "any-hover" || name == "pointer" ||
         name == "any-pointer";
}

// Evaluate the text inside one `( ... )` media feature. Returns nullopt for
// malformed syntax; a well-formed feature that is not about width (nor, with
// a known pointer, about hover/pointer) yields Unknown().
std::optional<WidthVerdict> EvaluateFeature(std::string_view feature,
                                            MediaPointer pointer) {
  // Whitespace is insignificant around ':' and the range operators.
  std::string compact;
  compact.reserve(feature.size());
  for (char c : feature) {
    if (std::isspace(static_cast<unsigned char>(c)) == 0) compact += c;
  }
  std::string_view s = compact;
  if (s.empty()) return std::nullopt;

  // Plain form: `name: value`.
  size_t colon = s.find(':');
  if (colon != std::string_view::npos) {
    std::string_view name = s.substr(0, colon);
    std::string_view value = s.substr(colon + 1);
    if (IsPointerFeature(name)) {
      return EvaluatePointerFeature(name, value, pointer);
    }
    if (name != "width" && name != "min-width" && name != "max-width") {
      return Unknown();
    }
    std::optional<double> px = ConsumeLength(value);
    if (!px.has_value() || !value.empty()) return std::nullopt;
    if (name == "min-width") return Exact(Compare(">=", *px));
    if (name == "max-width") return Exact(Compare("<=", *px));
    return Exact(Compare("=", *px));
  }

  // Boolean form of a pointer feature: `(hover)`, `(any-pointer)`.
  if (IsPointerFeature(s)) return EvaluatePointerFeature(s, "", pointer);

  // Range form: `width OP v`, `v OP width`, `v1 OP width OP v2`.
  size_t w = s.find("width");
  bool is_width = w != std::string_view::npos &&
                  (w == 0 || !IsIdentChar(s[w - 1])) &&
                  (w + 5 >= s.size() || !IsIdentChar(s[w + 5]));
  if (!is_width) {
    // `(height >= 600px)`, `(min-device-width ...)`, `(orientation)`, ...
    return Unknown();
  }
  std::string_view left = s.substr(0, w);
  std::string_view right = s.substr(w + 5);
  if (left.empty() && right.empty()) {
    // Boolean `(width)`: true for any non-zero width.
    return Exact(WidthInterval(1, kWidthMax));
  }
  WidthSet result = AllWidths();
  if (!left.empty()) {
    std::optional<double> px = ConsumeLength(left);
    if (!px.has_value()) return std::nullopt;
    std::string_view op = ConsumeOp(left);
    if (op.empty() || !left.empty()) return std::nullopt;
    result = Intersect(result, Compare(FlipOp(op), *px));
  }
  if (!right.empty()) {
    std::string_view op = ConsumeOp(right);
    if (op.empty()) return std::nullopt;
    std::optional<double> px = ConsumeLength(right);
    if (!px.has_value() || !right.empty()) return std::nullopt;
    result = Intersect(result, Compare(op, *px));
  }
  return Exact(std::move(result));
}

// Recursive-descent reader for one media query (Media Queries Level 4 §3):
//   query     := condition | [not|only]? type [and condition-without-or]?
//   condition := not in-parens | in-parens [and in-parens]* |
//                in-parens [or in-parens]*
//   in-parens := '(' condition ')' | '(' feature ')'
// Input is lowercase. Any syntax it cannot follow makes the whole query
// unknown, never excluded.
class MediaQueryReader {
 public:
  MediaQueryReader(std::string_view text, MediaPointer pointer)
      : s_(text), pointer_(pointer) {}

  std::optional<WidthVerdict> ReadQuery() {
    SkipSpace();
    std::optional<WidthVerdict> v;
    if (Peek() == '(' || (PeekWord() == "not" && PeekAfterWordIsParen())) {
      v = ReadCondition(/*allow_or=*/true);
    } else {
      bool negate = false;
      std::string_view word = PeekWord();
      if (word == "not" || word == "only") {
        negate = word == "not";
        ConsumeWord();
      }
      std::string_view type = ConsumeWord();
      if (type.empty()) return std::nullopt;
      WidthVerdict type_verdict;
      if (type == "all" || type == "screen") {
        type_verdict = Exact(AllWidths());
      } else if (type == "print") {
        type_verdict = Exact({});  // never matches a screen render
      } else {
        type_verdict = Unknown();
      }
      v = type_verdict;
      SkipSpace();
      if (PeekWord() == "and") {
        ConsumeWord();
        std::optional<WidthVerdict> cond = ReadCondition(/*allow_or=*/false);
        if (!cond.has_value()) return std::nullopt;
        v = And(*v, *cond);
      }
      if (negate) v = Not(*v);
    }
    SkipSpace();
    if (!v.has_value() || pos_ != s_.size()) return std::nullopt;
    return v;
  }

 private:
  std::optional<WidthVerdict> ReadCondition(bool allow_or) {
    SkipSpace();
    if (PeekWord() == "not") {
      ConsumeWord();
      std::optional<WidthVerdict> inner = ReadInParens();
      if (!inner.has_value()) return std::nullopt;
      return Not(*inner);
    }
    std::optional<WidthVerdict> v = ReadInParens();
    if (!v.has_value()) return std::nullopt;
    std::string_view joiner;
    while (true) {
      SkipSpace();
      std::string_view word = PeekWord();
      if (word != "and" && word != "or") break;
      if (word == "or" && !allow_or) return std::nullopt;
      if (!joiner.empty() && word != joiner) return std::nullopt;  // mixed
      joiner = word;
      ConsumeWord();
      std::optional<WidthVerdict> next = ReadInParens();
      if (!next.has_value()) return std::nullopt;
      v = joiner == "and" ? And(*v, *next) : Or(*v, *next);
    }
    return v;
  }

  std::optional<WidthVerdict> ReadInParens() {
    SkipSpace();
    if (Peek() != '(') return std::nullopt;
    ++pos_;
    SkipSpace();
    if (Peek() == '(' || (PeekWord() == "not" && PeekAfterWordIsParen())) {
      std::optional<WidthVerdict> v = ReadCondition(/*allow_or=*/true);
      SkipSpace();
      if (!v.has_value() || Peek() != ')') return std::nullopt;
      ++pos_;
      return v;
    }
    // A feature (or general-enclosed text): up to the matching ')'.
    size_t start = pos_;
    int depth = 0;
    while (pos_ < s_.size()) {
      char c = s_[pos_];
      if (c == '(') ++depth;
      if (c == ')') {
        if (depth == 0) break;
        --depth;
      }
      ++pos_;
    }
    if (pos_ >= s_.size()) return std::nullopt;
    std::string_view feature = s_.substr(start, pos_ - start);
    ++pos_;  // ')'
    if (feature.find('(') != std::string_view::npos) {
      return Unknown();  // calc(), general-enclosed functions, ...
    }
    std::optional<WidthVerdict> v = EvaluateFeature(feature, pointer_);
    // A feature we cannot read is unknown, not malformed: the rest of the
    // query can still exclude.
    return v.has_value() ? *v : Unknown();
  }

  void SkipSpace() {
    while (pos_ < s_.size() &&
           (std::isspace(static_cast<unsigned char>(s_[pos_])) != 0)) {
      ++pos_;
    }
  }
  char Peek() const { return pos_ < s_.size() ? s_[pos_] : '\0'; }
  std::string_view PeekWord() const {
    size_t end = pos_;
    while (end < s_.size() && IsIdentChar(s_[end])) ++end;
    return s_.substr(pos_, end - pos_);
  }
  bool PeekAfterWordIsParen() const {
    size_t end = pos_ + PeekWord().size();
    while (end < s_.size() &&
           (std::isspace(static_cast<unsigned char>(s_[end])) != 0)) {
      ++end;
    }
    return end < s_.size() && s_[end] == '(';
  }
  std::string_view ConsumeWord() {
    SkipSpace();
    std::string_view word = PeekWord();
    pos_ += word.size();
    return word;
  }

  std::string_view s_;
  size_t pos_ = 0;
  MediaPointer pointer_;
};

}  // namespace

MediaWidthMatch EvaluateMediaWidth(std::string_view media_rule,
                                   MediaWidthRange range,
                                   MediaPointer pointer) {
  std::string lower = net_instaweb::AsciiToLower(media_rule);
  std::string_view text = absl::StripAsciiWhitespace(lower);
  if (absl::StartsWith(text, "@media")) text.remove_prefix(6);

  // A media query list applies if ANY query applies. Split on commas at
  // parenthesis depth 0.
  WidthVerdict list{{}, {}};
  int depth = 0;
  size_t start = 0;
  for (size_t i = 0; i <= text.size(); ++i) {
    char c = i < text.size() ? text[i] : ',';
    if (c == '(') ++depth;
    if (c == ')' && depth > 0) --depth;
    // The end of the list closes the last query even when a parenthesis was
    // left open (its reader then reports it unreadable).
    if (i < text.size() && (c != ',' || depth != 0)) continue;
    std::string_view query =
        absl::StripAsciiWhitespace(text.substr(start, i - start));
    start = i + 1;
    std::optional<WidthVerdict> v;
    if (!query.empty()) v = MediaQueryReader(query, pointer).ReadQuery();
    list = Or(list, v.has_value() ? *v : Unknown());
  }

  WidthSet window = WidthInterval(range.min_px, range.max_px);
  if (Intersect(list.may, window).empty()) return MediaWidthMatch::kNever;
  if (!Intersect(list.must, window).empty()) return MediaWidthMatch::kApplies;
  return MediaWidthMatch::kUnknown;
}

namespace {

// Rule RETENTION: drop an @media block only when no window in `range`
// (RetentionWidthRangeForCss) with `pointer` (RetentionPointer under the
// class range, unknown otherwise) satisfies it. A query that cannot be read
// is kept: dropping a rule the fold needs is the failure this extractor
// exists to avoid.
//
// What that guarantees, by placement of the block:
// - The block precedes the sheets (CSS that uses no cascade layer, or a
//   layered page whose order is proven, CriticalCssBlockGoesFirst): the
//   range is the device class's plausible windows. A window inside it never
//   needs a dropped block. A window outside it (a phone UA zoomed out past
//   980 px) lacks the dropped overrides only until the sheet applies; the
//   sheet then wins every tie against the block.
// - The block follows the sheets (a layered page whose order is not proven):
//   the range is every width, so only blocks that can never apply (print)
//   are dropped, and no window loses an override it matches.
bool ShouldExcludeMediaForViewport(std::string_view media_rule,
                                   MediaWidthRange range,
                                   MediaPointer pointer) {
  return EvaluateMediaWidth(media_rule, range, pointer) ==
         MediaWidthMatch::kNever;
}

// True for an @media block that no screen at any width can match: `print`,
// `only print`, `not screen`, `print and (...)`, a list whose every query is
// one of those. `not print` and `screen, print` apply on screen and are kept.
// Not an @media rule: false.
bool MediaNeverAppliesOnScreen(std::string_view selector) {
  if (!absl::StartsWithIgnoreCase(selector, "@media")) return false;
  return EvaluateMediaWidth(selector, {0, kMediaWidthUnbounded}) ==
         MediaWidthMatch::kNever;
}

// The same question for ANCHORING, where the conservative default runs the
// other way: promoting an element (and its subtree) to above-the-fold on the
// strength of a media query nobody could evaluate over-includes, so only a
// query known to apply at this viewport anchors. Anchoring asks about the
// window the fold is estimated for, so it keeps the class's own partition
// range (CapabilityMask::ViewportWidthRange) rather than the wider retention
// range: a banner fixed only from 1024 px up is not on a phone's fold. The
// class's pointer (RetentionPointer) is read as well: a bar fixed under
// `(hover: none)` is on a phone's fold.
bool MediaKnownToApplyForViewport(std::string_view media_rule,
                                  CapabilityMask::Viewport viewport) {
  CapabilityMask::ViewportRange vp =
      CapabilityMask::ViewportWidthRange(viewport);
  return EvaluateMediaWidth(media_rule, {vp.min_px, vp.max_px},
                            RetentionPointer(viewport)) ==
         MediaWidthMatch::kApplies;
}

// Check if a selector represents a container at-rule whose inner rules
// should be recursed into (@layer, @supports).
bool AtRuleKeywordIs(std::string_view selector, std::string_view keyword);

bool IsContainerAtRule(std::string_view selector) {
  // Same name-boundary set as every other at-rule check: whitespace, '{', '/'
  // (`@layer/**/{`) or '(' (a minifier emits `@supports(display:grid){`), and
  // end-of-string, so "@layerX" does not match.
  return AtRuleKeywordIs(selector, "@layer") ||
         AtRuleKeywordIs(selector, "@supports");
}

using net_instaweb::HexDigitValue;

// Append a Unicode codepoint as UTF-8 bytes.
void AppendUtf8(std::string& out, char32_t cp) {
  if (cp < 0x80) {
    out += static_cast<char>(cp);
  } else if (cp < 0x800) {
    out += static_cast<char>(0xC0 | (cp >> 6));
    out += static_cast<char>(0x80 | (cp & 0x3F));
  } else if (cp < 0x10000) {
    out += static_cast<char>(0xE0 | (cp >> 12));
    out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
    out += static_cast<char>(0x80 | (cp & 0x3F));
  } else if (cp <= 0x10FFFF) {
    out += static_cast<char>(0xF0 | (cp >> 18));
    out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
    out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
    out += static_cast<char>(0x80 | (cp & 0x3F));
  }
}

// Advance pos past a CSS escape sequence (backslash already consumed, pos
// points at the character after the backslash).  Handles both hex escapes
// (\3a, \00003a) and literal escapes (\:, \[).
void SkipCssEscape(std::string_view s, size_t& pos) {
  if (pos >= s.size()) return;
  if (std::isxdigit(static_cast<unsigned char>(s[pos])) != 0) {
    // Hex escape: consume 1-6 hex digits.
    size_t hex_start = pos;
    while (pos < s.size() && pos - hex_start < 6 &&
           (std::isxdigit(static_cast<unsigned char>(s[pos])) != 0)) {
      ++pos;
    }
    // Consume optional trailing whitespace (part of the escape).
    if (pos < s.size() && s[pos] == ' ') ++pos;
  } else {
    // Literal escape: skip one character.
    ++pos;
  }
}

// Unescape a CSS identifier: handles both backslash-literal escapes
// (e.g., `lg\:grid` → `lg:grid`, `w-\[100px\]` → `w-[100px]`) and
// CSS hex escapes per CSS Syntax Module Level 3 §4.3.11
// (e.g., `\3a` → `:`, `\00003a` → `:`, `\5b` → `[`).
// A double backslash (`\\`) produces a literal backslash.
std::string UnescapeCssIdent(std::string_view s) {
  std::string result;
  result.reserve(s.size());
  for (size_t i = 0; i < s.size(); ++i) {
    if (s[i] == '\\' && i + 1 < s.size()) {
      ++i;
      if (std::isxdigit(static_cast<unsigned char>(s[i])) != 0) {
        // CSS hex escape: consume 1-6 hex digits.
        size_t start = i;
        while (i < s.size() && i - start < 6 &&
               (std::isxdigit(static_cast<unsigned char>(s[i])) != 0)) {
          ++i;
        }
        // Optional trailing whitespace is consumed as part of the escape.
        if (i < s.size() && s[i] == ' ') ++i;
        // Parse the hex value.
        char32_t codepoint = 0;
        for (size_t j = start; j < i && s[j] != ' '; ++j) {
          codepoint = codepoint * 16 + HexDigitValue(s[j]);
        }
        // Per spec: 0 and values > 0x10FFFF map to U+FFFD.
        if (codepoint == 0 || codepoint > 0x10FFFF) {
          codepoint = 0xFFFD;
        }
        AppendUtf8(result, codepoint);
        --i;  // loop will ++i
      } else {
        // Literal escape: backslash + next char.
        result += s[i];
      }
    } else {
      result += s[i];
    }
  }
  return result;
}

// Strip the outer braces from a block body: "{ ... }" → " ... ".
std::string_view StripOuterBraces(std::string_view body) {
  if (body.size() >= 2 && body.front() == '{' && body.back() == '}') {
    return body.substr(1, body.size() - 2);
  }
  return body;
}

// A style rule's declaration block (`body`, braces included)
// without its `!important` declarations (CssTextHasImportantBang, which is
// over-inclusive). Empty when nothing is left, or when the block nests rules
// (CSS nesting) and may hold an `!important` anywhere: the whole rule is then
// left out rather than parsed further.
std::string StripImportantDeclarations(std::string_view body) {
  const std::string_view inner = StripOuterBraces(body);
  if (!CssTextHasImportantBang(inner)) return std::string(body);
  std::vector<std::string_view> declarations;
  size_t start = 0;
  int parens = 0;
  // The same CSS Syntax 3 scanning as the rule parser: escapes,
  // strings (a bad string ends at an unescaped newline), unquoted url()s and
  // comments never split a declaration or open a block.
  for (size_t i = 0; i < inner.size();) {
    const char c = inner[i];
    if (c == '\\') {
      i = CssEscapeEnd(inner, i);
      continue;
    }
    if (c == '"' || c == '\'') {
      i = CssStringEnd(inner, i);
      continue;
    }
    if (StartsCssComment(inner, i)) {
      i = CssCommentEnd(inner, i);
      continue;
    }
    if (const size_t url_end = CssUnquotedUrlEnd(inner, i);
        url_end != std::string_view::npos) {
      i = url_end;
      continue;
    }
    if (c == '(' || c == '[') {
      ++parens;
    } else if ((c == ')' || c == ']') && parens > 0) {
      --parens;
    } else if (c == '{') {
      return {};  // nested rules: leave the whole rule out
    } else if (c == ';' && parens == 0) {
      declarations.push_back(inner.substr(start, i - start));
      start = i + 1;
    }
    ++i;
  }
  if (start < inner.size()) declarations.push_back(inner.substr(start));
  std::string out;
  for (std::string_view d : declarations) {
    if (CssTextHasImportantBang(d)) continue;
    if (absl::StripAsciiWhitespace(d).empty()) continue;
    if (!out.empty()) out += ';';
    out.append(d);
  }
  if (out.empty()) return {};
  return absl::StrCat("{", out, "}");
}

std::string ExtractLayerName(std::string_view selector);

// A block of rules (`body`, braces included, e.g. an @media
// block copied whole) with every `!important` declaration that sits inside an
// anonymous layer left out, recursively through group rules. `in_anonymous`:
// `body` itself is already inside one. A style rule outside any anonymous
// layer keeps its declarations, but one that nests an anonymous layer (CSS
// nesting) and may hold an `!important` is left out whole, and so is an
// at-rule whose keyword is written with an escape (`@l\61yer {` is an
// anonymous @layer to a browser). Empty when no rule is left.
std::string StripImportantFromRules(std::string_view body, int depth,
                                    bool in_anonymous) {
  const std::string_view inner = StripOuterBraces(body);
  if (!CssTextHasImportantBang(inner)) return std::string(body);
  if (!in_anonymous && !CssTextHasAnonymousLayer(inner)) {
    return std::string(body);
  }
  if (depth <= 0) return {};
  std::string out;
  for (const CssRule& rule : ParseCssRules(inner)) {
    std::string kept;
    if (CssAtRuleKeywordHasEscape(rule.selector)) {
      // Not decoded here, as on the placement side, so it may be an anonymous
      // layer holding an `!important`: left out whole.
    } else if (rule.body.empty()) {
      // A statement; an anonymous `@import ... layer` cannot sit in here.
      kept = absl::StrCat(rule.selector, ";");
    } else if (!absl::StartsWith(rule.selector, "@") ||
               AtRuleKeywordIs(rule.selector, "@page")) {
      std::string decls;
      if (in_anonymous) {
        decls = StripImportantDeclarations(rule.body);
      } else if (!(CssTextHasAnonymousLayer(rule.body) &&
                   CssTextHasImportantBang(rule.body))) {
        decls = rule.body;
      }
      if (!decls.empty()) kept = absl::StrCat(rule.selector, " ", decls);
    } else if (AtRuleKeywordIs(rule.selector, "@media") ||
               AtRuleKeywordIs(rule.selector, "@supports") ||
               AtRuleKeywordIs(rule.selector, "@layer") ||
               AtRuleKeywordIs(rule.selector, "@container") ||
               AtRuleKeywordIs(rule.selector, "@scope") ||
               AtRuleKeywordIs(rule.selector, "@starting-style")) {
      const bool anonymous =
          in_anonymous || (AtRuleKeywordIs(rule.selector, "@layer") &&
                           ExtractLayerName(rule.selector).empty());
      std::string nested =
          StripImportantFromRules(rule.body, depth - 1, anonymous);
      if (!nested.empty()) kept = absl::StrCat(rule.selector, " ", nested);
    } else {
      // @font-face, @keyframes, @property and the like: `!important` is
      // invalid or ignored inside them.
      kept = absl::StrCat(rule.selector, " ", rule.body);
    }
    if (kept.empty()) continue;
    if (!out.empty()) out += '\n';
    out += kept;
  }
  if (out.empty()) return {};
  return absl::StrCat("{\n", out, "\n}");
}

// A block of rules (`body`, braces included) without any
// anonymous layer in it: `@layer { }` blocks and anonymous `@import ... layer`
// statements are left out, group rules are filtered recursively, and a style
// rule that nests an anonymous layer (CSS nesting) is left out whole. So is
// any at-rule whose keyword is written with an escape (`@l\61yer {` is an
// anonymous @layer to a browser, and the keyword is not decoded here, as the
// placement side does not). Empty when no rule is left.
std::string DropAnonymousLayers(std::string_view body, int depth) {
  const std::string_view inner = StripOuterBraces(body);
  if (!CssTextHasAnonymousLayer(inner)) return std::string(body);
  if (depth <= 0) return {};
  std::string out;
  for (const CssRule& rule : ParseCssRules(inner)) {
    std::string kept;
    if (CssAtRuleKeywordHasEscape(rule.selector)) {
      // left out: may be an anonymous layer
    } else if (rule.body.empty()) {
      if (!CssTextHasAnonymousLayer(rule.selector)) {
        kept = absl::StrCat(rule.selector, ";");
      }
    } else if (AtRuleKeywordIs(rule.selector, "@layer") &&
               ExtractLayerName(rule.selector).empty()) {
      // left out
    } else if (AtRuleKeywordIs(rule.selector, "@media") ||
               AtRuleKeywordIs(rule.selector, "@supports") ||
               AtRuleKeywordIs(rule.selector, "@layer") ||
               AtRuleKeywordIs(rule.selector, "@container") ||
               AtRuleKeywordIs(rule.selector, "@scope") ||
               AtRuleKeywordIs(rule.selector, "@starting-style")) {
      std::string nested = DropAnonymousLayers(rule.body, depth - 1);
      if (!nested.empty()) kept = absl::StrCat(rule.selector, " ", nested);
    } else if (!CssTextHasAnonymousLayer(rule.body) ||
               (absl::StartsWith(rule.selector, "@") &&
                !AtRuleKeywordIs(rule.selector, "@page"))) {
      // @font-face, @keyframes and the like cannot hold a layer; their text
      // only looks like one inside a string or a comment.
      kept = absl::StrCat(rule.selector, " ", rule.body);
    }
    if (kept.empty()) continue;
    if (!out.empty()) out += '\n';
    out += kept;
  }
  if (out.empty()) return {};
  return absl::StrCat("{\n", out, "\n}");
}

bool AtRuleKeywordIs(std::string_view selector, std::string_view keyword) {
  if (!absl::StartsWithIgnoreCase(selector, keyword)) return false;
  if (selector.size() == keyword.size()) return true;
  const char next = selector[keyword.size()];
  return next == ' ' || next == '\t' || next == '\n' || next == '\r' ||
         next == '{' || next == '(' || next == '/';
}

// Extract the @layer name from a `@layer <name> {` selector: the trimmed text
// between "@layer" and end-of-string. Anonymous layers (`@layer {`) yield "".
std::string ExtractLayerName(std::string_view selector) {
  if (!absl::StartsWithIgnoreCase(selector, "@layer")) return "";
  std::string_view rest = selector.substr(6);  // past "@layer"
  // Comments are whitespace here: `@layer/**/{` is anonymous.
  std::string name;
  for (size_t i = 0; i < rest.size(); ++i) {
    if (rest[i] == '/' && i + 1 < rest.size() && rest[i + 1] == '*') {
      const size_t end = rest.find("*/", i + 2);
      if (end == std::string_view::npos) break;
      i = end + 1;
      name.push_back(' ');
      continue;
    }
    name.push_back(rest[i]);
  }
  return std::string(absl::StripAsciiWhitespace(name));
}

// Extract the normalized @media condition from a `@media <cond> {` selector:
// the text after "@media", whitespace-collapsed and lowercased (media features
// are case-insensitive) so the same query keys identically regardless of
// source spacing/case.
std::string ExtractMediaCondition(std::string_view selector) {
  if (!absl::StartsWithIgnoreCase(selector, "@media")) return "";
  std::string_view rest = selector.substr(6);  // past "@media"
  std::string collapsed;
  collapsed.reserve(rest.size());
  bool in_ws = false;
  for (char c : absl::StripAsciiWhitespace(rest)) {
    if (std::isspace(static_cast<unsigned char>(c)) != 0) {
      if (!in_ws) {
        collapsed += ' ';
        in_ws = true;
      }
    } else {
      collapsed +=
          static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
      in_ws = false;
    }
  }
  return collapsed;
}

// Append a cascade-layer segment to a ">"-joined layer path (outermost first).
std::string JoinLayer(const std::string& path, const std::string& name) {
  if (path.empty()) return name;
  return absl::StrCat(path, ">", name);
}

// Combine nested @media conditions with " and " (a rule inside two nested
// @media blocks applies only when both hold).
std::string JoinMedia(const std::string& outer, const std::string& inner) {
  if (inner.empty()) return outer;
  if (outer.empty()) return inner;
  return absl::StrCat(outer, " and ", inner);
}

// Census of the contexts the sheet places each normalized selector in.
// Mirrors ProcessRules' context tracking, but walks EVERY @media block rather
// than only the ones ProcessRules recurses into, so a responsive override of a
// selector is seen even when ProcessRules would emit its @media block
// wholesale. Only consulted by the relaxed force-include fallback.
//
// @supports is transparent for identity (as it is everywhere else in this
// file), so a rule that appears both bare and inside an @supports probe
// collapses to ONE context and both copies are force-included. That is
// accepted, bounded over-inclusion: the two copies are alternatives the
// cascade already resolves.
void CollectSelectorContexts(
    const std::vector<CssRule>& rules, MediaWidthRange range,
    MediaPointer pointer, int max_depth, const std::string& layer_path,
    const std::string& media_condition,
    absl::flat_hash_map<std::string, SelectorContexts>& out) {
  for (const auto& rule : rules) {
    if (rule.body.empty()) continue;
    if (IsContainerAtRule(rule.selector)) {
      if (max_depth <= 0) continue;
      bool is_layer = absl::StartsWithIgnoreCase(rule.selector, "@layer");
      std::string inner_layer_path =
          is_layer ? JoinLayer(layer_path, ExtractLayerName(rule.selector))
                   : layer_path;
      CollectSelectorContexts(ParseCssRules(StripOuterBraces(rule.body)), range,
                              pointer, max_depth - 1, inner_layer_path,
                              media_condition, out);
      continue;
    }
    if (MediaNeverAppliesOnScreen(rule.selector)) continue;
    if (absl::StartsWithIgnoreCase(rule.selector, "@media")) {
      if (max_depth <= 0) continue;
      if (ShouldExcludeMediaForViewport(rule.selector, range, pointer)) {
        continue;
      }
      CollectSelectorContexts(
          ParseCssRules(StripOuterBraces(rule.body)), range, pointer,
          max_depth - 1, layer_path,
          JoinMedia(media_condition, ExtractMediaCondition(rule.selector)),
          out);
      continue;
    }
    if (absl::StartsWith(rule.selector, "@")) continue;
    SelectorContexts& entry = out[NormalizeSelector(rule.selector)];
    entry.layer_paths.insert(layer_path);
    entry.media_conditions.insert(media_condition);
  }
}

}  // namespace

std::string NormalizeSelector(std::string_view selector) {
  std::string_view trimmed = absl::StripAsciiWhitespace(selector);
  std::string out;
  out.reserve(trimmed.size());
  bool in_ws = false;
  for (char c : trimmed) {
    if (std::isspace(static_cast<unsigned char>(c)) != 0) {
      if (!in_ws) {
        out += ' ';
        in_ws = true;
      }
    } else {
      out += c;
      in_ws = false;
    }
  }
  return out;
}

CriticalCssExtractor::CriticalCssExtractor() { BuildMeasuredFoldIndex(); }

CriticalCssExtractor::CriticalCssExtractor(CriticalCssConfig config)
    : config_(std::move(config)) {
  BuildMeasuredFoldIndex();
}

CriticalCssExtractor::~CriticalCssExtractor() = default;

void CriticalCssExtractor::BuildMeasuredFoldIndex() {
  for (const std::string& token : config_.measured_above_fold_selectors) {
    // Rejects "", "*", and a bare "#"/"." in one condition. A wildcard token
    // would promote every element in the document to above-the-fold, which is
    // the failure mode this whole change exists to avoid overshooting into.
    if (token.size() < 2) continue;
    std::string value = token.substr(1);
    if (token[0] == '#') {
      measured_fold_ids_.insert(std::move(value));
    } else if (token[0] == '.') {
      measured_fold_classes_.insert(std::move(value));
    }
    // Every other shape — a bare tag, a compound, an image selector such as
    // "img#hero" — is inert on purpose. See measured_above_fold_selectors.
  }
}

bool CriticalCssExtractor::MeasuredFoldContains(
    const CollectedElement& element) const {
  if (!element.id.empty() && measured_fold_ids_.contains(element.id)) {
    return true;
  }
  if (measured_fold_classes_.empty()) return false;
  for (const std::string& cls : element.classes) {
    if (measured_fold_classes_.contains(cls)) return true;
  }
  return false;
}

CriticalCssResult CriticalCssExtractor::Extract(
    const std::vector<CollectedElement>& elements, std::string_view css,
    CapabilityMask::Viewport viewport,
    const absl::flat_hash_set<RuleIdentity>* force_include,
    const CascadeLayerOrder* layer_order) {
  CriticalCssResult result;

  if (css.empty()) {
    result.success = true;
    return result;
  }

  // The element budget starts at <body>: nothing before it paints. Elements
  // arrive in document order, so the first <body> is the document's.
  fold_index_base_ = 0;
  for (const auto& element : elements) {
    if (net_instaweb::StringCaseEqual(element.tag_name, "body")) {
      fold_index_base_ = element.element_index;
      break;
    }
  }

  // Parse CSS into rules
  std::vector<CssRule> rules = ParseCssRules(css);
  result.total_rules = static_cast<int>(rules.size());

  BuildFixedAnchors(rules, elements, viewport);
  BuildWideReplacedElements(elements, viewport);
  BuildPageTokens(elements);

  const CascadeLayerOrder unknown_order;
  const CascadeLayerOrder& order =
      layer_order != nullptr ? *layer_order : unknown_order;
  auto extract_for = [&](MediaWidthRange range, MediaPointer pointer) {
    retention_range_ = range;
    retention_pointer_ = pointer;
    anonymous_layer_depth_ = 0;
    std::optional<ForceIncludeIndex> index;
    if (force_include != nullptr) {
      index.emplace(
          BuildForceIncludeIndex(rules, range, pointer, *force_include));
    }
    auto run = [&](size_t wide_budget) {
      wide_replaced_budget_ = wide_budget;
      wide_replaced_bytes_ = 0;
      pending_wrapper_bytes_ = 0;
      wide_replaced_closed_ = wide_budget == 0;
      fold_admissions_ = 0;
      int critical_count = 0;
      result.critical_css = ProcessRules(
          rules, elements, viewport, critical_count, kMaxRuleRecursionDepth,
          /*layer_path=*/"", /*media_condition=*/"",
          index.has_value() ? &index.value() : nullptr);
      result.critical_rules = critical_count;
    };
    run(kMaxWideReplacedBytes);
    // Rules kept for wide replaced elements never push a block past what the
    // worker will inline (config_.inline_limit_bytes / _coverage): a block
    // over either limit is not inlined at all, which would turn a fix for an
    // overflow into a full flash. When the first pass lands past the limit,
    // the budget shrinks to the room the fold's own rules leave, less a small
    // margin (the budget counts a separator the first rule of a block does not
    // emit, so the fold's share is estimated a few bytes low), and the block
    // is derived again.
    if (wide_replaced_bytes_ > 0) {
      size_t limit = config_.inline_limit_bytes;
      if (css.size() >= config_.inline_limit_min_sheet_bytes) {
        // Strictly under the share: the worker inlines only below it.
        const size_t share = static_cast<size_t>(
            config_.inline_limit_coverage * static_cast<float>(css.size()));
        limit = std::min(limit, share > 0 ? share - 1 : 0);
      }
      if (result.critical_css.size() > limit) {
        constexpr size_t kMargin = 64;
        const size_t fold_bytes =
            result.critical_css.size() - wide_replaced_bytes_;
        run(limit > fold_bytes + kMargin ? limit - fold_bytes - kMargin : 0);
      }
    }
  };

  // A block that goes AFTER the sheets registers its anonymous
  // layers after theirs, and a later layer wins every NORMAL declaration over
  // any specificity, so its base rules beat the sheet's state overrides for
  // good (and `!important` is already kept out of them, which the earlier
  // layer would win anyway). No placement is safe for anonymous-layer rules
  // there, so such a block carries none. Decided from the combined sheet
  // first (CriticalCssBlockGoesFirst), then checked on the finished block
  // below. An at-rule keyword written with an escape (`@l\61yer {`) counts
  // as a possible anonymous layer throughout (CssTextHasAnonymousLayer,
  // CssAtRuleKeywordHasEscape), as it does on the placement
  // side, where it always sends the block after the sheets.
  drop_anonymous_layers_ = !CriticalCssBlockGoesFirst(css, order);
  const MediaWidthRange every_width{0, kMediaWidthUnbounded};
  const MediaWidthRange range = RetentionWidthRangeForCss(
      viewport, css, order, &result.class_range_retention);
  // The hover/pointer features are read for the class only under its own
  // range: a block that goes after the sheets keeps every
  // @media block any visitor could match, whatever they point with.
  const MediaPointer pointer = result.class_range_retention
                                   ? RetentionPointer(viewport)
                                   : MediaPointer::kUnknown;
  MediaWidthRange used = range;
  MediaPointer used_pointer = pointer;
  extract_for(range, pointer);
  // The class range is only sound for a block that goes first and has no
  // anonymous layer. It was chosen from the combined sheet; confirm it on the
  // block that will be placed and fall back to every width when the two
  // disagree (an @import layer() the block promotes to the front, a block the
  // layer walker cannot read with certainty). The serve path strips NUL bytes
  // before it decides placement (HtmlTransformFilter::InjectCriticalCss), and
  // `@la\0yer` is `@layer` after that, so the check reads the same text.
  // CriticalCssRetentionMayNarrow implies the block goes first
  // (DecideCriticalCssLayerPlacement).
  std::string placed;
  placed.reserve(result.critical_css.size());
  for (char c : result.critical_css) {
    if (c != '\0') placed.push_back(c);
  }
  if (result.class_range_retention &&
      !CriticalCssRetentionMayNarrow(placed, order)) {
    result.class_range_retention = false;
    if (range.min_px != every_width.min_px ||
        range.max_px != every_width.max_px ||
        pointer != MediaPointer::kUnknown) {
      used = every_width;
      used_pointer = MediaPointer::kUnknown;
      extract_for(every_width, MediaPointer::kUnknown);
    }
  }
  // On the block that will be placed: one that still carries an
  // anonymous layer and takes the fallback after the sheets (the combined
  // sheet went first, the block does not, e.g. an @import layer() it promotes
  // to the front) is derived again without any. Without anonymous layers the
  // block's placement can only become first, which is safe either way.
  if (!drop_anonymous_layers_) {
    placed.clear();
    for (char c : result.critical_css) {
      if (c != '\0') placed.push_back(c);
    }
    if (CssTextHasAnonymousLayer(placed) &&
        DecideCriticalCssLayerPlacement(placed, order).keep_fallback) {
      drop_anonymous_layers_ = true;
      extract_for(used, used_pointer);
    }
  }
  result.anonymous_layers_dropped =
      drop_anonymous_layers_ && CssTextHasAnonymousLayer(css);
  result.success = true;
  return result;
}

bool CriticalCssExtractor::ForceIncludeIndex::MatchesRelaxed(
    const std::string& normalized_selector, const std::string& layer_path,
    const std::string& media_condition) const {
  if (!relaxable.contains(normalized_selector)) return false;
  auto it = sheet_contexts.find(normalized_selector);
  // An absent census entry resolves to no match: the walks bound recursion
  // independently, so the census can stop short of a rule ProcessRules reaches.
  if (it == sheet_contexts.end()) return false;
  const SelectorContexts& contexts = it->second;
  // Cross-layer occurrences are genuine ambiguity. The sole layer must also BE
  // this rule's layer: the two walks bound recursion independently, so the
  // census can have recorded a different single occurrence than the one in
  // hand.
  if (contexts.layer_paths.size() != 1) return false;
  if (*contexts.layer_paths.begin() != layer_path) return false;
  // With an unconditional occurrence, the identity names it, and its @media
  // occurrences in the same layer are that rule's responsive overrides: they
  // come along (`.container` with its breakpoint max-widths), since the base
  // rule alone would style the element for no viewport. @media blocks are
  // filtered per rule (ProcessRules), so they no longer ride along whole.
  // Without one, only an unambiguous single occurrence matches.
  if (contexts.media_conditions.contains("")) return true;
  if (contexts.media_conditions.size() != 1) return false;
  return media_condition == *contexts.media_conditions.begin();
}

CriticalCssExtractor::ForceIncludeIndex
CriticalCssExtractor::BuildForceIncludeIndex(
    const std::vector<CssRule>& rules, MediaWidthRange retention_range,
    MediaPointer retention_pointer,
    const absl::flat_hash_set<RuleIdentity>& force_include) {
  ForceIncludeIndex index(force_include);
  for (const auto& id : force_include) {
    if (id.layer_path.empty() && id.media_condition.empty()) {
      index.relaxable.insert(id.normalized_selector);
    }
  }
  if (!index.relaxable.empty()) {
    CollectSelectorContexts(rules, retention_range, retention_pointer,
                            kMaxRuleRecursionDepth, /*layer_path=*/"",
                            /*media_condition=*/"", index.sheet_contexts);
  }
  return index;
}

namespace {
bool RuleHasMatchableToken(std::string_view selector);
}  // namespace

std::string CriticalCssExtractor::ProcessRules(
    const std::vector<CssRule>& rules,
    const std::vector<CollectedElement>& elements,
    CapabilityMask::Viewport viewport, int& critical_count, int max_depth,
    const std::string& layer_path, const std::string& media_condition,
    const ForceIncludeIndex* force_index) {
  std::string critical_css;

  for (const auto& rule : rules) {
    // An at-rule whose keyword is written with an escape
    // (`@l\61yer {` is an anonymous @layer to a browser) is not decoded here.
    // The placement side treats it as a possible layer already
    // (CriticalCssMayUseCascadeLayer), so a block holding one goes after the
    // sheets, and such a block carries none of them, whatever they are (see
    // drop_anonymous_layers_). Before the statement branch: a statement with
    // an escaped keyword (`@\69mport ... layer;`) is left out the same way.
    if (drop_anonymous_layers_ && CssAtRuleKeywordHasEscape(rule.selector)) {
      continue;
    }
    // Statement at-rules (no block body): a bare `@layer a,b,c;` layer-order
    // declaration, `@import`, or `@charset`. These carry cascade-ordering /
    // load semantics, not a matchable selector, so preserve them as-is. Emitted
    // in source order, so a leading layer-order statement lands before the rules
    // that reference the layers.
    if (rule.body.empty()) {
      // An anonymous `@import ... layer` is left out: its sheet
      // would land in a new anonymous layer ahead of the page's, where its
      // `!important` declarations win, and it is fetched again from a URL
      // that resolves against the page rather than the sheet.
      if (AtRuleKeywordIs(rule.selector, "@import") &&
          CssTextHasAnonymousLayer(rule.selector)) {
        continue;
      }
      if (absl::StartsWithIgnoreCase(rule.selector, "@layer") ||
          IsAlwaysIncludedAtRule(rule.selector)) {
        if (!critical_css.empty()) {
          critical_css += '\n';
        }
        critical_css += rule.selector;
        critical_css += ";";
      }
      continue;
    }

    // Handle container at-rules (@layer, @supports): recurse into inner rules.
    if (IsContainerAtRule(rule.selector)) {
      if (max_depth <= 0) continue;
      std::string_view inner = StripOuterBraces(rule.body);
      std::vector<CssRule> inner_rules = ParseCssRules(inner);
      // Extend the identity layer path when entering a @layer block.
      // @supports is transparent for identity.
      bool is_layer = absl::StartsWithIgnoreCase(rule.selector, "@layer");
      std::string inner_layer_path =
          is_layer ? JoinLayer(layer_path, ExtractLayerName(rule.selector))
                   : layer_path;
      const bool anonymous =
          is_layer && ExtractLayerName(rule.selector).empty();
      // A block that goes after the sheets carries no anonymous
      // layer at all (drop_anonymous_layers_).
      if (anonymous && drop_anonymous_layers_) continue;
      const size_t outer_pending = EnterWrapper(rule.selector);
      if (anonymous) ++anonymous_layer_depth_;
      std::string inner_critical = ProcessRules(
          inner_rules, elements, viewport, critical_count, max_depth - 1,
          inner_layer_path, media_condition, force_index);
      if (anonymous) --anonymous_layer_depth_;
      LeaveWrapper(outer_pending);
      if (!inner_critical.empty()) {
        if (!critical_css.empty()) {
          critical_css += '\n';
        }
        critical_css += rule.selector;
        critical_css += " {\n";
        critical_css += inner_critical;
        critical_css += "\n}";
      }
      continue;
    }

    bool include = false;

    // Skip @media print rules
    if (MediaNeverAppliesOnScreen(rule.selector)) {
      continue;
    }

    // Always include certain @-rules
    if (IsAlwaysIncludedAtRule(rule.selector)) {
      include = true;
    }

    // Check if we should include this selector
    if (!include) {
      include = ShouldIncludeSelector(rule.selector, elements);
    }

    // Inside an @media block being filtered per rule, a rule the matcher
    // cannot match on (no tag, id or class token: `[data-theme=dark]`,
    // `:root:not(...)`, `::selection`, a nested at-rule) is kept rather than
    // dropped. It may apply to the page although nothing in the scan matches
    // it, and before per-rule filtering its block was copied whole. Such rules
    // are rare, so keeping them costs little.
    if (!include && !media_condition.empty() &&
        !RuleHasMatchableToken(rule.selector)) {
      include = true;
    }

    // Force-include augmentation: a rule the DOM matcher cannot capture (an
    // attribute selector, a state-conditional variant that never renders under
    // a static JS-off sample) is still emitted when its identity — keyed by the
    // enclosing @layer path + @media condition + normalized selector — is in
    // the force_include set. @-rules never carry a DOM selector, so they are
    // excluded from the identity lookup.
    //
    // A coverage-derived identity has no wrapper context to key on (see
    // ForceIncludeIndex), so a primary-key miss falls back to the uniqueness-
    // guarded relaxed match.
    if (!include && force_index != nullptr &&
        !absl::StartsWith(rule.selector, "@")) {
      std::string normalized = NormalizeSelector(rule.selector);
      RuleIdentity id{layer_path, media_condition, normalized};
      if (force_index->exact.contains(id) ||
          force_index->MatchesRelaxed(normalized, layer_path,
                                      media_condition)) {
        include = true;
      }
    }

    // A replaced element outside the fold that may be wider than a phone
    // (wide_replaced_): its rules are kept while the budget lasts.
    bool wide_only = false;
    if (!include && !wide_replaced_.empty() && !wide_replaced_closed_ &&
        !absl::StartsWith(rule.selector, "@") &&
        SelectorMatchesWideReplacedElement(rule.selector, elements)) {
      // The rule, and the enclosing @layer / @media wrappers it would be the
      // first to bring into the block.
      const size_t cost =
          rule.selector.size() + rule.body.size() + 2 + pending_wrapper_bytes_;
      if (wide_replaced_bytes_ + cost <= wide_replaced_budget_) {
        wide_replaced_bytes_ += cost;
        pending_wrapper_bytes_ = 0;
        include = true;
        wide_only = true;
      } else {
        wide_replaced_closed_ = true;
      }
    }
    if (include && !wide_only && !absl::StartsWith(rule.selector, "@")) {
      ++fold_admissions_;
      // The fold's own rule brings the enclosing wrappers in.
      pending_wrapper_bytes_ = 0;
    }

    if (include) {
      // An @media block is filtered per rule, inside a @layer or not: its
      // rules are matched against the fold like any others. A block none of
      // whose rules the fold needs is left out, and a large one keeps only
      // the rules the fold needs. Only a small block with at least one such
      // rule is copied whole (cheaper than per-rule wrappers, and Tailwind's
      // responsive blocks hold little else). Copying every surviving block
      // whole let a framework's mobile-first breakpoints (Bootstrap's ten
      // `(min-width:576px)` blocks) push the mobile block past
      // kInlineCriticalCssMaxBytes once retention kept blocks up to 980 px
      // for phones.
      if (absl::StartsWithIgnoreCase(rule.selector, "@media") &&
          max_depth > 0) {
        std::string_view inner = StripOuterBraces(rule.body);
        std::vector<CssRule> inner_rules = ParseCssRules(inner);
        std::string inner_media =
            JoinMedia(media_condition, ExtractMediaCondition(rule.selector));
        int inner_count = 0;
        const int fold_admissions_before = fold_admissions_;
        const size_t outer_pending = EnterWrapper(rule.selector);
        std::string inner_critical =
            ProcessRules(inner_rules, elements, viewport, inner_count,
                         max_depth - 1, layer_path, inner_media, force_index);
        LeaveWrapper(outer_pending);
        // Copied whole only for a rule the fold needs: a block holding nothing
        // but rules kept for a wide replaced element keeps just those, so the
        // budget counts every byte they add.
        const bool small = static_cast<int>(rule.body.size()) <=
                               config_.max_wholesale_media_bytes &&
                           (inner_critical.empty() ||
                            fold_admissions_ != fold_admissions_before);
        if (inner_critical.empty()) {
          // Dropping is sound only when the matcher could have matched every
          // rule. A rule with no tag, id or class token (`:root:not(...)`,
          // `[data-theme=auto]`, `::selection`, `*`) may match nothing in the
          // scan and still apply, so a small block holding one is copied
          // whole, as it always was.
          bool all_matchable = true;
          for (const auto& inner_rule : inner_rules) {
            if (!RuleHasMatchableToken(inner_rule.selector)) {
              all_matchable = false;
              break;
            }
          }
          if (all_matchable || !small) continue;
        }
        // Copied whole: without the `!important` declarations that sit
        // inside an anonymous layer, whether the block itself is inside one
        // or holds one.
        // Or without any anonymous layer, for a block that
        // goes after the sheets.
        std::string whole;
        if (small) {
          whole =
              drop_anonymous_layers_
                  ? DropAnonymousLayers(rule.body, kMaxRuleRecursionDepth)
                  : StripImportantFromRules(rule.body, kMaxRuleRecursionDepth,
                                            anonymous_layer_depth_ > 0);
          if (whole.empty()) continue;
        }
        if (!critical_css.empty()) {
          critical_css += '\n';
        }
        critical_css += rule.selector;
        if (!small) {
          critical_css += " {\n";
          critical_css += inner_critical;
          critical_css += "\n}";
          critical_count += inner_count;
        } else {
          critical_css += " ";
          critical_css += whole;
          ++critical_count;
        }
      } else {
        // Without the `!important` declarations that sit inside an anonymous
        // layer, whether the rule is inside one or nests one:
        //   - a style rule or @page inside one loses them one by one (the
        //     rule goes when none is left); outside one, a rule that nests an
        //     anonymous layer (CSS nesting) and may hold an `!important` goes
        //     whole;
        //   - a group rule copied whole loses them in every anonymous layer
        //     inside it (or everywhere, when it is inside one itself);
        //   - @font-face and the like keep their text.
        //   - for a block that goes after the sheets, no
        //     anonymous layer stays at all (DropAnonymousLayers).
        std::string body = rule.body;
        const bool in_anonymous = anonymous_layer_depth_ > 0;
        if (drop_anonymous_layers_ && CssTextHasAnonymousLayer(rule.body)) {
          if (!absl::StartsWith(rule.selector, "@") ||
              AtRuleKeywordIs(rule.selector, "@page")) {
            body.clear();  // nests an anonymous layer
          } else if (AtRuleKeywordIs(rule.selector, "@container") ||
                     AtRuleKeywordIs(rule.selector, "@scope") ||
                     AtRuleKeywordIs(rule.selector, "@starting-style") ||
                     AtRuleKeywordIs(rule.selector, "@media")) {
            body = DropAnonymousLayers(rule.body, kMaxRuleRecursionDepth);
          }
        } else if (!absl::StartsWith(rule.selector, "@") ||
                   AtRuleKeywordIs(rule.selector, "@page")) {
          if (in_anonymous) {
            body = StripImportantDeclarations(rule.body);
          } else if (CssTextHasAnonymousLayer(rule.body) &&
                     CssTextHasImportantBang(rule.body)) {
            body.clear();
          }
        } else if (AtRuleKeywordIs(rule.selector, "@container") ||
                   AtRuleKeywordIs(rule.selector, "@scope") ||
                   AtRuleKeywordIs(rule.selector, "@starting-style") ||
                   AtRuleKeywordIs(rule.selector, "@media") ||
                   AtRuleKeywordIs(rule.selector, "@supports") ||
                   AtRuleKeywordIs(rule.selector, "@layer")) {
          body = StripImportantFromRules(rule.body, kMaxRuleRecursionDepth,
                                         in_anonymous);
        }
        if (body.empty()) continue;
        if (!critical_css.empty()) {
          critical_css += "\n";
        }
        critical_css += rule.selector;
        critical_css += " ";
        critical_css += body;
        ++critical_count;
      }
    }
  }

  return critical_css;
}

bool CriticalCssExtractor::ShouldIncludeSelector(
    std::string_view selector, const std::vector<CollectedElement>& elements) {
  // Handle @media rules
  if (absl::StartsWithIgnoreCase(selector, "@media")) {
    if (MediaNeverAppliesOnScreen(selector)) return false;
    if (ShouldExcludeMediaForViewport(selector, retention_range_,
                                      retention_pointer_)) {
      return false;
    }
    return true;
  }

  // Check against always-include selectors
  std::string selector_lower = ToLower(selector);
  for (const auto& always_include : config_.always_include_selectors) {
    std::string pattern_lower = ToLower(always_include);
    // Match exactly or as part of a selector list
    if (selector_lower == pattern_lower ||
        selector_lower.find(pattern_lower + ",") != std::string::npos ||
        selector_lower.find(", " + pattern_lower) != std::string::npos ||
        selector_lower.find("," + pattern_lower) != std::string::npos) {
      return true;
    }
  }

  // For universal selector, always include
  if (selector == "*" || selector.find('*') == 0) {
    return true;
  }

  // Check if selector matches any collected element
  return SelectorMatchesAnyElement(selector, elements);
}

namespace {

// The first occurrence at or after `pos` of a character in `stops` that is at
// the top level of a selector: not inside a string, an escape (`\,`, `\(`),
// an attribute selector or parentheses. npos when there is none.
size_t FindTopLevelInSelector(std::string_view s, size_t pos,
                              std::string_view stops) {
  int depth = 0;  // () and [] together: they nest properly in a selector
  size_t i = pos;
  while (i < s.size()) {
    const char c = s[i];
    if (c == '\\') {
      i = CssEscapeEnd(s, i);
      continue;
    }
    if (c == '"' || c == '\'') {
      i = CssStringEnd(s, i);
      continue;
    }
    if (depth == 0 && stops.find(c) != std::string_view::npos) return i;
    if (c == '(' || c == '[') {
      ++depth;
    } else if ((c == ')' || c == ']') && depth > 0) {
      --depth;
    }
    ++i;
  }
  return std::string_view::npos;
}

// The ')' matching the '(' at s[open], skipping strings and escapes; npos
// when it is never closed.
size_t FindMatchingParen(std::string_view s, size_t open) {
  int depth = 0;
  size_t i = open;
  while (i < s.size()) {
    const char c = s[i];
    if (c == '\\') {
      i = CssEscapeEnd(s, i);
      continue;
    }
    if (c == '"' || c == '\'') {
      i = CssStringEnd(s, i);
      continue;
    }
    if (c == '(') {
      ++depth;
    } else if (c == ')' && --depth == 0) {
      return i;
    }
    ++i;
  }
  return std::string_view::npos;
}

// The compounds of a complex selector, left to right: the text between its
// top-level combinators (whitespace, `>`, `+`, `~`). A combinator character
// that is escaped (`.\[\&\>svg\]`, `.a\ b`, the space that ends a hex escape
// like `\3a `), quoted, or inside parentheses or an attribute selector
// (`:nth-child(2n+1)`, `:not(.a > .b)`, `[class~=c]`, `[type=text i]`) does
// not split.
std::vector<std::string_view> CompoundsOf(std::string_view complex) {
  std::vector<std::string_view> out;
  complex = absl::StripAsciiWhitespace(complex);
  size_t start = 0;
  size_t pos = 0;
  while (true) {
    const size_t at = FindTopLevelInSelector(complex, pos, " \t\n\r\f>+~");
    std::string_view piece = absl::StripAsciiWhitespace(complex.substr(
        start,
        at == std::string_view::npos ? std::string_view::npos : at - start));
    if (!piece.empty()) out.push_back(piece);
    if (at == std::string_view::npos) break;
    start = at + 1;
    pos = at + 1;
  }
  return out;
}

// The rightmost compound of a complex selector.
std::string_view FinalCompoundOf(std::string_view complex) {
  std::vector<std::string_view> compounds = CompoundsOf(complex);
  return compounds.empty() ? std::string_view() : compounds.back();
}

bool CompoundHasMatchableToken(std::string_view compound);

// The compound the matcher matches a complex selector on: the rightmost one
// with a tag, id or class (CompoundHasMatchableToken), else the rightmost.
// The matcher asks whether an element of the selector exists in the fold,
// not whether its ancestors do; when the subject itself carries nothing to
// match (`.space-y-4>:not(:last-child)`, Tailwind v4's `space-y-*` and
// `divide-y`, or `.menu > *`), the nearest compound that does stands in for
// it, so the rule is kept when the element it styles the children of is in
// the fold instead of never.
std::string_view MatchCompoundOf(std::string_view complex) {
  std::vector<std::string_view> compounds = CompoundsOf(complex);
  if (compounds.empty()) return std::string_view();
  for (auto it = compounds.rbegin(); it != compounds.rend(); ++it) {
    if (CompoundHasMatchableToken(*it)) return *it;
  }
  return compounds.back();
}

// The final compound of each alternative in a selector list: for
// `header nav > a:hover, .cta` that is `a:hover` and `.cta`. The matcher is a
// simplification — it asks whether the LAST element of a complex selector
// exists, not whether its ancestors do. Commas inside functional
// pseudo-classes (`:is(h1, h2)`, `:where()`, `:not()`) do not split, and a
// space consumed by a CSS hex escape (`\3a `) is not a descendant combinator.
std::vector<std::string_view> FinalCompounds(std::string_view selector) {
  std::vector<std::string_view> out;
  size_t pos = 0;
  while (pos < selector.size()) {
    size_t comma_pos = FindTopLevelInSelector(selector, pos, ",");
    std::string_view single_selector;
    if (comma_pos == std::string_view::npos) {
      single_selector = selector.substr(pos);
      pos = selector.size();
    } else {
      single_selector = selector.substr(pos, comma_pos - pos);
      pos = comma_pos + 1;
    }
    std::string_view final_selector = FinalCompoundOf(single_selector);
    if (final_selector.empty()) {
      continue;
    }
    out.push_back(final_selector);
  }
  return out;
}

// What the matcher matches one alternative of a selector list on: the
// stand-in compound (MatchCompoundOf), and the compounds before it, which
// name the ancestors the rule needs.
struct MatchTarget {
  std::string_view compound;
  std::vector<std::string_view> ancestors;
};

std::vector<MatchTarget> MatchTargets(std::string_view selector) {
  std::vector<MatchTarget> out;
  size_t pos = 0;
  while (pos < selector.size()) {
    const size_t comma = FindTopLevelInSelector(selector, pos, ",");
    std::string_view alternative = selector.substr(
        pos,
        comma == std::string_view::npos ? std::string_view::npos : comma - pos);
    pos = comma == std::string_view::npos ? selector.size() : comma + 1;
    std::vector<std::string_view> compounds = CompoundsOf(alternative);
    if (compounds.empty()) continue;
    size_t stand_in = compounds.size() - 1;
    for (size_t k = compounds.size(); k > 0; --k) {
      if (CompoundHasMatchableToken(compounds[k - 1])) {
        stand_in = k - 1;
        break;
      }
    }
    MatchTarget target;
    target.compound = compounds[stand_in];
    target.ancestors.assign(compounds.begin(), compounds.begin() + stand_in);
    out.push_back(std::move(target));
  }
  return out;
}

// MatchCompoundOf for each alternative of a selector list.
std::vector<std::string_view> MatchCompounds(std::string_view selector) {
  std::vector<std::string_view> out;
  for (const MatchTarget& target : MatchTargets(selector)) {
    out.push_back(target.compound);
  }
  return out;
}

// Calls fn(is_class, unescaped_name) for each class and id a compound names
// outside parentheses and attribute selectors: `.prose` and `#nav` in
// `.prose#nav:where(.dark)`, not `.dark`. Returns false without calling fn
// when the compound's type selector is `html` or `body` or it starts with
// `:root`: the document element's state classes (`html.dark`, `html.js`) are
// often added by script, so they are not looked for in the markup.
template <typename Fn>
bool ForEachCompoundToken(std::string_view compound, Fn fn) {
  auto tag_is = [&compound](std::string_view tag) {
    if (!absl::StartsWithIgnoreCase(compound, tag)) return false;
    return compound.size() == tag.size() || !IsIdentChar(compound[tag.size()]);
  };
  if (tag_is("html") || tag_is("body") || tag_is(":root")) return false;
  size_t i = 0;
  while (i < compound.size()) {
    const char c = compound[i];
    if (c == '\\') {
      i = CssEscapeEnd(compound, i);
      continue;
    }
    if (c == '"' || c == '\'') {
      i = CssStringEnd(compound, i);
      continue;
    }
    if (c == '(') {
      const size_t close = FindMatchingParen(compound, i);
      i = close == std::string_view::npos ? compound.size() : close + 1;
      continue;
    }
    if (c == '[') {
      const size_t close = FindTopLevelInSelector(compound, i + 1, "]");
      i = close == std::string_view::npos ? compound.size() : close + 1;
      continue;
    }
    if (c == '.' || c == '#') {
      size_t end = i + 1;
      while (end < compound.size() &&
             (IsIdentChar(compound[end]) || compound[end] == '\\' ||
              static_cast<unsigned char>(compound[end]) >= 0x80)) {
        end = compound[end] == '\\' ? CssEscapeEnd(compound, end) : end + 1;
      }
      if (end > i + 1) {
        fn(c == '.', UnescapeCssIdent(compound.substr(i + 1, end - i - 1)));
      }
      i = end;
      continue;
    }
    ++i;
  }
  return true;
}

// True when `compound` carries a token the matcher can match an element on: a
// tag, an id or a class, directly or in every alternative of a
// `:where()`/`:is()`. Attribute selectors and pseudos alone
// (`[data-theme=auto]`, `:not(.x)`, `::selection`, `*`) are none, and neither
// is `:root`: the matcher reads it as `html`, but a page whose <html> tag is
// omitted has no such element in the scan, and the rule still applies.
bool CompoundHasMatchableToken(std::string_view compound) {
  compound = absl::StripAsciiWhitespace(compound);
  if (compound.empty()) return false;
  if (IsIdentChar(compound[0]) || compound[0] == '\\') return true;  // tag
  size_t i = 0;
  while (i < compound.size()) {
    char c = compound[i];
    if (c == '.' || c == '#') return true;
    if (c == '\\') {
      // An escaped character is part of whatever ident it is in.
      i = CssEscapeEnd(compound, i);
      continue;
    }
    if (c == '[') {
      // Skip the attribute selector, quoted values and escapes included.
      size_t close = FindTopLevelInSelector(compound, i + 1, "]");
      i = close == std::string_view::npos ? compound.size() : close + 1;
      continue;
    }
    if (c == ':') {
      size_t name_start = i + 1;
      if (name_start < compound.size() && compound[name_start] == ':') {
        ++name_start;
      }
      size_t p = name_start;
      while (p < compound.size() && IsIdentChar(compound[p])) ++p;
      std::string_view name = compound.substr(name_start, p - name_start);
      if (p < compound.size() && compound[p] == '(') {
        const size_t close = FindMatchingParen(compound, p);
        if (close == std::string_view::npos) return false;
        if (absl::EqualsIgnoreCase(name, "where") ||
            absl::EqualsIgnoreCase(name, "is")) {
          std::vector<std::string_view> alts =
              MatchCompounds(compound.substr(p + 1, close - p - 1));
          bool all = !alts.empty();
          for (std::string_view alt : alts) {
            if (!CompoundHasMatchableToken(alt)) all = false;
          }
          if (all) return true;
        }
        i = close + 1;
        continue;
      }
      i = p;
      continue;
    }
    ++i;
  }
  return false;
}

// True when every alternative of a style rule's selector list has a matchable
// token (CompoundHasMatchableToken). An at-rule has none.
bool RuleHasMatchableToken(std::string_view selector) {
  if (absl::StartsWith(selector, "@")) return false;
  std::vector<std::string_view> compounds = FinalCompounds(selector);
  if (compounds.empty()) return false;
  for (std::string_view compound : compounds) {
    if (!CompoundHasMatchableToken(compound)) return false;
  }
  return true;
}

// The declarations a style rule applies to ITS OWN element: the rule body
// with every nested block (CSS nesting, a nested @media) removed, compacted to
// lower case with no whitespace so a minifier's and an author's spelling read
// the same.
std::string OwnDeclarationsCompact(std::string_view body) {
  std::string compact;
  compact.reserve(body.size());
  int depth = 0;
  size_t i = 0;
  while (i < body.size()) {
    const char c = body[i];
    // A brace inside a string or an escape (`content: "{"`) opens nothing.
    size_t end = i + 1;
    const size_t url_end = (c == 'u' || c == 'U') ? CssUnquotedUrlEnd(body, i)
                                                  : std::string_view::npos;
    if (url_end != std::string_view::npos) {
      end = url_end;
    } else if (c == '"' || c == '\'') {
      end = CssStringEnd(body, i);
    } else if (c == '\\') {
      end = CssEscapeEnd(body, i);
    } else if (c == '{') {
      ++depth;
      ++i;
      continue;
    } else if (c == '}') {
      if (depth > 0) --depth;
      ++i;
      continue;
    }
    // depth 1 is the rule's own block; anything deeper is a nested rule and
    // styles some other element (or the same one under a condition this pass
    // does not evaluate).
    if (depth == 1) {
      for (size_t k = i; k < end; ++k) {
        if (std::isspace(static_cast<unsigned char>(body[k])) == 0) {
          compact += static_cast<char>(
              std::tolower(static_cast<unsigned char>(body[k])));
        }
      }
    }
    i = end;
  }
  return compact;
}

// True when a rule's own declarations pin its element to the viewport AND do
// not also take it out of rendering: a `position:fixed` modal that ships
// `display:none` is not on screen, and neither is a fixed box at opacity 0.
bool AnchorsToViewport(std::string_view body) {
  const std::string decls = OwnDeclarationsCompact(body);
  if (decls.find("position:fixed") == std::string::npos) return false;
  return decls.find("display:none") == std::string::npos &&
         decls.find("visibility:hidden") == std::string::npos &&
         decls.find("opacity:0;") == std::string::npos &&
         !absl::EndsWith(decls, "opacity:0");
}

// True when a compound selector only applies under an interaction state, so
// its element is not fixed at first paint: `.focus\:fixed:focus` is the skip
// link, on screen only while focused.
bool CompoundIsStateConditional(std::string_view compound) {
  const std::string lower = ToLower(compound);
  for (std::string_view pseudo :
       {":focus", ":hover", ":active", ":target", ":checked"}) {
    // ":focus" also covers ":focus-within" and ":focus-visible".
    if (lower.find(pseudo) != std::string::npos) return true;
  }
  return false;
}

// Selector lists of every style rule that anchors its element to the
// viewport, wherever the rule sits in the @layer/@supports/@media nesting. An
// @media block that cannot apply at `viewport` is skipped, so a
// `@media (min-width: 48rem) { .cookie-bar { position: fixed } }` anchors
// nothing on mobile. Bounded by the same recursion depth as ProcessRules.
void CollectFixedSelectors(const std::vector<CssRule>& rules,
                           CapabilityMask::Viewport viewport, int max_depth,
                           std::vector<std::string>& out) {
  for (const auto& rule : rules) {
    if (rule.body.empty()) continue;
    if (IsContainerAtRule(rule.selector) ||
        absl::StartsWithIgnoreCase(rule.selector, "@media")) {
      if (max_depth <= 0) continue;
      if (MediaNeverAppliesOnScreen(rule.selector)) continue;
      if (absl::StartsWithIgnoreCase(rule.selector, "@media") &&
          !MediaKnownToApplyForViewport(rule.selector, viewport)) {
        continue;
      }
      CollectFixedSelectors(ParseCssRules(StripOuterBraces(rule.body)),
                            viewport, max_depth - 1, out);
      continue;
    }
    if (absl::StartsWith(rule.selector, "@")) continue;
    if (AnchorsToViewport(rule.body)) out.push_back(rule.selector);
  }
}

}  // namespace

// Bound on the elements one anchor pulls in (the anchor plus its subtree). A
// launcher or a cookie banner is a handful of elements; a fixed wrapper that
// an unclosed tag turns into an ancestor of the rest of the document must not
// promote the rest of the document.
inline constexpr int kMaxFixedAnchorSubtree = 50;

void CriticalCssExtractor::BuildFixedAnchors(
    const std::vector<CssRule>& rules,
    const std::vector<CollectedElement>& elements,
    CapabilityMask::Viewport viewport) {
  fixed_anchored_.clear();
  std::vector<std::string> selectors;
  CollectFixedSelectors(rules, viewport, kMaxRuleRecursionDepth, selectors);
  if (selectors.empty()) return;

  std::vector<std::string_view> compounds;
  for (const std::string& selector : selectors) {
    for (std::string_view compound : FinalCompounds(selector)) {
      // A universal compound would anchor the whole document. Nothing
      // legitimately fixes every element; refuse rather than inline the sheet.
      if (compound == "*") continue;
      if (CompoundIsStateConditional(compound)) continue;
      compounds.push_back(compound);
    }
  }

  // Elements arrive in document order with their depth, so the subtree of an
  // anchor is the run of deeper elements that follows it. A hidden element
  // (IsInvisibleElement: the `hidden` attribute, display:none inline, a 1 px
  // beacon) is never an anchor, and a hidden subtree inside an anchor — the
  // launcher's closed panel — is not on screen either.
  for (size_t i = 0; i < elements.size(); ++i) {
    const CollectedElement& anchor = elements[i];
    if (anchor.hidden) continue;
    if (fixed_anchored_.contains(anchor.element_index)) continue;
    bool matches = false;
    for (std::string_view compound : compounds) {
      if (SimpleSelectorMatchesElement(compound, anchor)) {
        matches = true;
        break;
      }
    }
    if (!matches) continue;
    fixed_anchored_.insert(anchor.element_index);
    int taken = 1;
    for (size_t j = i + 1;
         j < elements.size() && elements[j].depth > anchor.depth; ++j) {
      if (elements[j].hidden) {
        const int hidden_depth = elements[j].depth;
        while (j + 1 < elements.size() &&
               elements[j + 1].depth > hidden_depth) {
          ++j;
        }
        continue;
      }
      if (taken >= kMaxFixedAnchorSubtree) break;
      fixed_anchored_.insert(elements[j].element_index);
      ++taken;
    }
  }
}

size_t CriticalCssExtractor::EnterWrapper(std::string_view prelude) {
  const size_t outer = pending_wrapper_bytes_;
  // `prelude {\n` ... `\n}` plus the newline that separates it.
  pending_wrapper_bytes_ += prelude.size() + 6;
  return outer;
}

void CriticalCssExtractor::LeaveWrapper(size_t outer_pending) {
  // Zero means a rule inside brought this wrapper (and so every enclosing one)
  // into the block; otherwise nothing was emitted and the outer wrappers are
  // still pending exactly as before.
  if (pending_wrapper_bytes_ != 0) pending_wrapper_bytes_ = outer_pending;
}

void CriticalCssExtractor::BuildWideReplacedElements(
    const std::vector<CollectedElement>& elements,
    CapabilityMask::Viewport viewport) {
  wide_replaced_.clear();
  if (viewport != CapabilityMask::Viewport::kMobile &&
      viewport != CapabilityMask::Viewport::kTablet) {
    return;
  }
  // Elements arrive in document order with their depth, so "inside" is the run
  // of deeper elements that follows.
  int hidden_depth = -1;
  for (size_t i = 0; i < elements.size(); ++i) {
    const CollectedElement& element = elements[i];
    if (hidden_depth >= 0 && element.depth <= hidden_depth) hidden_depth = -1;
    if (hidden_depth >= 0) continue;
    if (element.hidden) {
      hidden_depth = element.depth;
      continue;
    }
    if (element.may_exceed_viewport) wide_replaced_.push_back(i);
  }
}

bool CriticalCssExtractor::SelectorMatchesWideReplacedElement(
    std::string_view selector, const std::vector<CollectedElement>& elements) {
  for (std::string_view final_selector : FinalCompounds(selector)) {
    for (size_t i : wide_replaced_) {
      if (SimpleSelectorMatchesElement(final_selector, elements[i])) {
        return true;
      }
    }
  }
  return false;
}

void CriticalCssExtractor::BuildPageTokens(
    const std::vector<CollectedElement>& elements) {
  page_classes_.clear();
  page_ids_.clear();
  for (const CollectedElement& element : elements) {
    if (!element.id.empty()) page_ids_.insert(element.id);
    for (const std::string& cls : element.classes) page_classes_.insert(cls);
  }
}

// Classes that script commonly puts on the document root before first
// paint, so a page's markup need not carry them for a rule scoped to them to
// apply: Tailwind's class-strategy dark mode (`.dark .dark\:bg-black`, the v3
// default spelling) and its `light` counterpart, and the `js` flag that
// scripts and Modernizr-style loaders set.
static bool IsScriptRootStateClass(std::string_view name) {
  return name == "dark" || name == "light" || name == "js";
}

bool CriticalCssExtractor::AncestorsOnPage(
    const std::vector<std::string_view>& ancestors) const {
  for (std::string_view compound : ancestors) {
    bool on_page = true;
    ForEachCompoundToken(compound, [&](bool is_class, const std::string& name) {
      if (is_class && IsScriptRootStateClass(name)) return;
      if (!(is_class ? page_classes_ : page_ids_).contains(name)) {
        on_page = false;
      }
    });
    if (!on_page) return false;
  }
  return true;
}

bool CriticalCssExtractor::SelectorMatchesAnyElement(
    std::string_view selector, const std::vector<CollectedElement>& elements) {
  for (const MatchTarget& target : MatchTargets(selector)) {
    // The rule needs its ancestors too: one whose ancestor compound names a
    // class or id the page never uses (Tailwind Typography's `.prose :where(p)`
    // on a page without `.prose`) cannot apply anywhere on it.
    if (!AncestorsOnPage(target.ancestors)) continue;
    const std::string_view final_selector = target.compound;
    // Check each element
    for (const auto& element : elements) {
      // Skip excluded elements.
      //
      // NOTE (known interaction, deliberately unchanged here): this runs BEFORE
      // the measured-fold check below, so `max_depth` and the footer/lazy/defer
      // pattern lists still veto an element a browser reported inside the
      // viewport — e.g. a deeply-nested visible element, or a visible one whose
      // class merely contains "defer". The result is under-inclusion, which is
      // the safe direction (that element's rules are simply not treated as
      // critical, exactly as today), so measured evidence is not given the power
      // to overrule an explicit exclusion in this change. A follow-up issue
      // tracks whether it should.
      //
      // A position:fixed anchor is exempt from the PATTERN veto only: a
      // floating "defer-banner" is on screen whatever its class is called. The
      // depth veto still applies to it.
      if (element.depth > config_.max_depth) {
        continue;
      }
      const bool anchored = fixed_anchored_.contains(element.element_index);
      if (!anchored && IsElementExcluded(element)) {
        continue;
      }

      // Check if element is within our "above the fold" criteria. An element
      // is critical if it's among the first N elements inside <body> OR
      // explicitly included (a measured above-the-fold descriptor, the
      // header/nav/hero heuristics, or a position:fixed anchor and its
      // subtree; replaced elements that may overflow a phone are matched
      // separately, see wide_replaced_). Elements before <body> never paint
      // and are not in the
      // budget — except the root <html> itself, which is a painted box and
      // carries page-wide state classes (Tailwind puts `dark` on <html>, and
      // the `.dark { --color-... }` rule keyed on it is the whole dark
      // palette).
      const int fold_rank = element.element_index - fold_index_base_;
      const bool in_estimate = fold_rank >= 0 ? fold_rank < config_.max_elements
                                              : net_instaweb::StringCaseEqual(
                                                    element.tag_name, "html");
      bool is_critical = in_estimate || anchored || IsElementIncluded(element);

      if (!is_critical) {
        continue;
      }

      if (SimpleSelectorMatchesElement(final_selector, element)) {
        return true;
      }
    }
  }

  return false;
}

bool CriticalCssExtractor::SimpleSelectorMatchesElement(
    std::string_view selector, const CollectedElement& element) {
  if (selector.empty()) {
    return false;
  }

  // Universal selector matches everything
  if (selector == "*") {
    return true;
  }

  std::string selector_str(selector);
  size_t pos = 0;

  // Parse the simple selector into components
  std::string tag_name;
  std::string id;
  std::vector<std::string> classes;

  // Tailwind v4 (and any framework using `@custom-variant`) scopes variant
  // rules with zero-specificity `:where()`/`:is()` wrappers, e.g.
  // `.dark\:bg-stone-900:where(.dark, .dark *)`. Per CSS semantics `:where()`
  // has zero specificity and is a scoping hint, not a gating predicate — so we
  // must treat it as non-constraining for retention. Two cases:
  //   1. `A:where(B)` — when the outer part `A` matches, `:where(B)` is ignored
  //      (`saw_where_is` lets the final check skip the broken-off pseudo).
  //   2. `:where(B)` / `:is(B)` with no matching outer part — desugar by
  //      evaluating the inner alternatives so the rule is retained when any
  //      inner alternative matches (rescues leading-pseudo selectors).
  bool saw_where_is = false;
  bool where_is_inner_matched = false;

  // A leading `:root` is the document element, i.e. `html`:
  // `:root:not([data-theme=light])` (a theme toggle's dark palette) matches
  // <html> like `html:not(...)` would.
  if (absl::StartsWithIgnoreCase(selector_str, ":root") &&
      (selector_str.size() == 5 || !IsIdentChar(selector_str[5]))) {
    tag_name = "html";
    pos = 5;
  }

  while (pos < selector_str.size()) {
    char c = selector_str[pos];

    if (c == '#') {
      // ID selector
      size_t end = pos + 1;
      while (
          end < selector_str.size() &&
          ((std::isalnum(static_cast<unsigned char>(selector_str[end])) != 0) ||
           selector_str[end] == '-' || selector_str[end] == '_' ||
           selector_str[end] == '\\')) {
        if (selector_str[end] == '\\' && end + 1 < selector_str.size()) {
          ++end;  // skip backslash
          SkipCssEscape(selector_str, end);
        } else {
          ++end;
        }
      }
      id = UnescapeCssIdent(selector_str.substr(pos + 1, end - pos - 1));
      pos = end;
    } else if (c == '.') {
      // Class selector
      size_t end = pos + 1;
      while (
          end < selector_str.size() &&
          ((std::isalnum(static_cast<unsigned char>(selector_str[end])) != 0) ||
           selector_str[end] == '-' || selector_str[end] == '_' ||
           selector_str[end] == '\\')) {
        if (selector_str[end] == '\\' && end + 1 < selector_str.size()) {
          ++end;  // skip backslash
          SkipCssEscape(selector_str, end);
        } else {
          ++end;
        }
      }
      classes.push_back(
          UnescapeCssIdent(selector_str.substr(pos + 1, end - pos - 1)));
      pos = end;
    } else if (c == ':') {
      // Pseudo-class/element. `:where(...)`/`:is(...)` are zero-specificity
      // scoping wrappers (CSS Selectors Level 4); desugar them rather than
      // dropping the rule.
      std::string_view rest(selector_str);
      rest = rest.substr(pos);
      bool is_where = absl::StartsWithIgnoreCase(rest, ":where(");
      bool is_is = !is_where && absl::StartsWithIgnoreCase(rest, ":is(");
      if (!is_where && !is_is) {
        // Any other pseudo (`:hover`, `:focus`, `::before`, `:nth-child(2)`,
        // ...): drop its constraint but keep parsing the rest of the compound,
        // so a following `:where()`/`:is()` wrapper — which may hold the only
        // DOM-matchable token (e.g. `:hover:where(.foo)`) — is still desugared.
        // Skip the pseudo name and any functional `(...)` argument; the outer
        // tag/id/class decision below is unchanged for the common `a:hover`
        // case (the pseudo simply contributes nothing).
        size_t p = pos + 1;  // past ':'
        if (p < selector_str.size() && selector_str[p] == ':') {
          ++p;  // pseudo-element `::`
        }
        while (
            p < selector_str.size() &&
            ((std::isalnum(static_cast<unsigned char>(selector_str[p])) != 0) ||
             selector_str[p] == '-' || selector_str[p] == '_')) {
          ++p;
        }
        if (p < selector_str.size() && selector_str[p] == '(') {
          // Skip the argument (`:not(.a > .b)`, `:nth-child(2n+1)`), strings
          // and escapes in it included; an unclosed one ends the compound.
          const size_t close = FindMatchingParen(selector_str, p);
          p = close == std::string::npos ? selector_str.size() : close + 1;
        }
        pos = p;
        continue;
      }
      // Locate the matching close paren for the `:where(` / `:is(` opener.
      size_t open_paren = selector_str.find('(', pos);
      size_t close_paren = FindMatchingParen(selector_str, open_paren);
      if (close_paren == std::string::npos) {
        // Malformed (unbalanced) — give up on this pseudo conservatively.
        break;
      }
      saw_where_is = true;
      // Evaluate the inner selector list. `:where()`/`:is()` accept a
      // forgiving selector list; the rule applies if ANY alternative matches,
      // so recurse on the last compound part of each alternative.
      std::string_view inner = selector_str;
      inner = inner.substr(open_paren + 1, close_paren - open_paren - 1);
      size_t inner_pos = 0;
      while (inner_pos < inner.size()) {
        size_t comma = FindTopLevelInSelector(inner, inner_pos, ",");
        std::string_view alt;
        if (comma == std::string_view::npos) {
          alt = inner.substr(inner_pos);
          inner_pos = inner.size();
        } else {
          alt = inner.substr(inner_pos, comma - inner_pos);
          inner_pos = comma + 1;
        }
        alt = absl::StripAsciiWhitespace(alt);
        if (alt.empty()) {
          continue;
        }
        // For a complex inner alternative (descendant/combinator, e.g.
        // `.dark *`), match on its rightmost compound part, mirroring
        // SelectorMatchesAnyElement.
        alt = MatchCompoundOf(alt);
        // A bare universal (`*`) inside :where()/:is() matches any element.
        if (alt == "*" || SimpleSelectorMatchesElement(alt, element)) {
          where_is_inner_matched = true;
        }
      }
      // Continue parsing after the wrapper (e.g. `A:where(B):is(C)`), so the
      // outer part decision below still sees any tag/id/class on either side.
      pos = close_paren + 1;
    } else if (c == '[') {
      // Attribute selector: not matched (the scan does not keep attributes),
      // so it adds no constraint, but the compound goes on after it
      // (`input[type=text i].wide`); quotes and escapes in it are skipped.
      const size_t close = FindTopLevelInSelector(selector_str, pos + 1, "]");
      pos = close == std::string::npos ? selector_str.size() : close + 1;
    } else if ((std::isalnum(static_cast<unsigned char>(c)) != 0) || c == '-' ||
               c == '_' || c == '\\') {
      // Tag name
      size_t end = pos;
      while (
          end < selector_str.size() &&
          ((std::isalnum(static_cast<unsigned char>(selector_str[end])) != 0) ||
           selector_str[end] == '-' || selector_str[end] == '_' ||
           selector_str[end] == '\\')) {
        if (selector_str[end] == '\\' && end + 1 < selector_str.size()) {
          ++end;  // skip backslash
          SkipCssEscape(selector_str, end);
        } else {
          ++end;
        }
      }
      tag_name = UnescapeCssIdent(selector_str.substr(pos, end - pos));
      pos = end;
    } else {
      ++pos;
    }
  }

  // Match against element
  // Tag name must match (case-insensitive)
  if (!tag_name.empty()) {
    if (!absl::EqualsIgnoreCase(tag_name, element.tag_name)) {
      return false;
    }
  }

  // ID must match exactly
  if (!id.empty()) {
    if (id != element.id) {
      return false;
    }
  }

  // All classes must be present
  for (const auto& cls : classes) {
    bool found = false;
    for (const auto& elem_cls : element.classes) {
      if (cls == elem_cls) {
        found = true;
        break;
      }
    }
    if (!found) {
      return false;
    }
  }

  // At least one component must have matched. The outer simple selector
  // (tag/id/classes) gated above; if it is present and non-empty it matched,
  // so the rule is retained regardless of any `:where()`/`:is()` wrapper
  // (zero-specificity scoping is non-constraining for retention).
  if (!tag_name.empty() || !id.empty() || !classes.empty()) {
    return true;
  }

  // No matchable outer part (e.g. a selector that leads with `:where(...)` /
  // `:is(...)`): retain the rule if any inner alternative of the desugared
  // `:where()`/`:is()` matched. This is what rescues Tailwind v4 dark-mode
  // rules whose only matchable token lives inside the scoping wrapper.
  return saw_where_is && where_is_inner_matched;
}

bool CriticalCssExtractor::ContainsPattern(
    std::string_view str, const std::vector<std::string>& patterns) {
  std::string str_lower = ToLower(str);
  for (const auto& pattern : patterns) {
    std::string pattern_lower = ToLower(pattern);
    if (str_lower.find(pattern_lower) != std::string::npos) {
      return true;
    }
  }
  return false;
}

bool CriticalCssExtractor::IsElementExcluded(const CollectedElement& element) {
  // Check depth
  if (element.depth > config_.max_depth) {
    return true;
  }

  // Check tag name patterns
  if (ContainsPattern(element.tag_name, config_.exclude_tag_patterns)) {
    return true;
  }

  // Check ID patterns
  if (!element.id.empty() &&
      ContainsPattern(element.id, config_.exclude_id_patterns)) {
    return true;
  }

  // Check class patterns
  for (const auto& cls : element.classes) {
    if (ContainsPattern(cls, config_.exclude_class_patterns)) {
      return true;
    }
  }

  return false;
}

bool CriticalCssExtractor::IsElementIncluded(const CollectedElement& element) {
  // Measured evidence first. The patterns below are guesses about which markup
  // tends to sit at the top of a page; this is a browser reporting that it laid
  // this element out inside the viewport.
  if (MeasuredFoldContains(element)) {
    return true;
  }

  // Check tag name patterns
  if (ContainsPattern(element.tag_name, config_.include_tag_patterns)) {
    return true;
  }

  // Check ID patterns
  if (!element.id.empty() &&
      ContainsPattern(element.id, config_.include_id_patterns)) {
    return true;
  }

  // Check class patterns
  for (const auto& cls : element.classes) {
    if (ContainsPattern(cls, config_.include_class_patterns)) {
      return true;
    }
  }

  return false;
}

bool CriticalCssIsSufficient(float coverage_ratio, size_t critical_css_bytes,
                             size_t deferred_css_bytes,
                             bool external_css_unresolved,
                             const AsyncCssSufficiencyConfig& cfg) {
  // Fail-safe (dominant): a declared external stylesheet was not resolved from
  // cache, so the bytes we would defer are UNMEASURED and the inlined critical
  // CSS was derived WITHOUT that sheet. `deferred_css_bytes` here is the cold,
  // inline-only blob (not the real sheet), which would otherwise trip the
  // small-sheet escape hatch below and defer an unmeasured sheet -> FOUC.
  // Never defer a sheet we could not measure; keep it render-blocking. The
  // caller marks the variant for revalidation, so async re-enables once the
  // sheet caches and a real decision can be made.
  if (external_css_unresolved) return false;
  // Gate disabled -> legacy behavior (always defer when critical CSS exists).
  if (cfg.min_coverage_ratio <= 0.0f) return true;
  // A small sheet has a trivial FOUC window even with thin critical CSS.
  if (deferred_css_bytes < cfg.min_deferred_css_bytes) return true;
  if (deferred_css_bytes == 0) return true;
  // The byte ratio is a hard floor: it measures what was ACTUALLY inlined
  // against what would ACTUALLY be deferred. A browser profile's rule-level
  // coverage_ratio is measured pre-extraction and can wildly contradict the
  // extracted bytes (live incident: coverage=0.39 claimed while 830 B of
  // critical CSS deferred a 115 KB sheet -> FOUC). Gate on the pessimistic of
  // the two; unknown (negative) or NaN coverage leaves the byte ratio alone.
  float ratio = static_cast<float>(critical_css_bytes) /
                static_cast<float>(deferred_css_bytes);
  if (coverage_ratio >= 0.0f && coverage_ratio < ratio) {
    ratio = coverage_ratio;
  }
  return ratio >= cfg.min_coverage_ratio;
}

}  // namespace pagespeed
