// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

// PageSpeed 2.0 - Unused CSS Removal Implementation
//
// Given a complete stylesheet and the byte ranges that Chrome's CSS
// Coverage API reports as used during full page load, extract only the
// used portions. Always-needed at-rules (@charset, @import, @font-face,
// @keyframes) are preserved regardless of coverage data.

#include "src/browser/unused_css_remover.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "absl/strings/match.h"
#include "absl/strings/str_cat.h"

namespace pagespeed {

namespace {

// Sort and merge overlapping/adjacent ranges into non-overlapping set.
std::vector<std::pair<size_t, size_t>> SortAndMerge(
    std::vector<std::pair<size_t, size_t>> ranges) {
  if (ranges.size() < 2) return ranges;

  std::sort(ranges.begin(), ranges.end());

  std::vector<std::pair<size_t, size_t>> merged;
  merged.push_back(ranges[0]);

  for (size_t i = 1; i < ranges.size(); ++i) {
    auto& last = merged.back();
    if (ranges[i].first <= last.second) {
      last.second = std::max(last.second, ranges[i].second);
    } else {
      merged.push_back(ranges[i]);
    }
  }
  return merged;
}

// Check if a byte range overlaps with any used range.
// Both inputs must be sorted and non-overlapping.
bool OverlapsUsedRange(
    size_t start, size_t end,
    const std::vector<std::pair<size_t, size_t>>& used_ranges) {
  if (start >= end) return false;
  // Find the first range whose end > start.
  auto it = std::lower_bound(used_ranges.begin(), used_ranges.end(), start,
                             [](const std::pair<size_t, size_t>& range,
                                size_t val) { return range.second <= val; });
  return it != used_ranges.end() && it->first < end;
}

// Skip whitespace characters.
size_t SkipWhitespace(std::string_view css, size_t pos) {
  while (pos < css.size() && (css[pos] == ' ' || css[pos] == '\t' ||
                              css[pos] == '\n' || css[pos] == '\r')) {
    ++pos;
  }
  return pos;
}

// Lexer state for brace-balanced CSS scanning.
struct CssLexerState {
  int depth = 0;
  bool in_string = false;
  char string_char = 0;
};

enum class CssCharAction : uint8_t {
  kContinue,    // Consumed character, keep scanning.
  kFoundClose,  // Closing brace found at depth 0.
  kEndOfInput,  // Unterminated comment -- treat as end of input.
};

// Process one CSS character for brace-balance tracking.
// Updates `state` and may advance `pos` past escapes or comments.
CssCharAction ParseCssChar(std::string_view css, size_t& pos,
                           CssLexerState& state) {
  char ch = css[pos];
  if (state.in_string) {
    if (ch == '\\' && pos + 1 < css.size()) {
      // Skip the escaped character; an escaped newline continues the string,
      // and CRLF is one newline.
      pos +=
          (pos + 2 < css.size() && css[pos + 1] == '\r' && css[pos + 2] == '\n')
              ? 2
              : 1;
      return CssCharAction::kContinue;
    }
    // The closing quote ends the string, and so does an unescaped newline (a
    // bad string, CSS Syntax 3 §4.3.5), which is not part of it.
    if (ch == state.string_char || ch == '\n' || ch == '\r' || ch == '\f') {
      state.in_string = false;
    }
    return CssCharAction::kContinue;
  }
  if (ch == '/' && pos + 1 < css.size() && css[pos + 1] == '*') {
    size_t end = css.find("*/", pos + 2);
    if (end == std::string_view::npos) return CssCharAction::kEndOfInput;
    pos = end + 1;  // Loop increment brings us past '*/'.
    return CssCharAction::kContinue;
  }
  if (ch == '\\' && pos + 1 < css.size()) {
    // An escape outside a string (`.a\'b`, `.a\{`) is part of an ident: the
    // escaped quote opens no string and the escaped brace no block (CSS
    // Syntax 3 §4.3.7).
    ++pos;
    return CssCharAction::kContinue;
  }
  if (ch == '\'' || ch == '"') {
    state.in_string = true;
    state.string_char = ch;
    return CssCharAction::kContinue;
  }
  if (ch == '{') {
    ++state.depth;
  } else if (ch == '}') {
    --state.depth;
    if (state.depth == 0) return CssCharAction::kFoundClose;
  }
  return CssCharAction::kContinue;
}

// Find the end of a balanced brace block starting at the opening '{'.
// Returns position after the closing '}'.
size_t FindClosingBrace(std::string_view css, size_t pos) {
  if (pos >= css.size() || css[pos] != '{') return pos;
  CssLexerState state;
  for (size_t i = pos; i < css.size(); ++i) {
    CssCharAction action = ParseCssChar(css, i, state);
    if (action == CssCharAction::kFoundClose) return i + 1;
    if (action == CssCharAction::kEndOfInput) return css.size();
  }
  return css.size();
}

// Check if a rule starting at `pos` is a preserved at-rule.
// Preserved at-rules: @charset, @import, @font-face, @keyframes.
bool IsPreservedAtRule(std::string_view css, size_t pos) {
  if (pos >= css.size() || css[pos] != '@') return false;
  std::string_view remaining = css.substr(pos);
  return absl::StartsWithIgnoreCase(remaining, "@charset") ||
         absl::StartsWithIgnoreCase(remaining, "@import") ||
         absl::StartsWithIgnoreCase(remaining, "@font-face") ||
         absl::StartsWithIgnoreCase(remaining, "@keyframes") ||
         absl::StartsWithIgnoreCase(remaining, "@-webkit-keyframes") ||
         absl::StartsWithIgnoreCase(remaining, "@-moz-keyframes");
}

// Find the next '{' starting at pos, skipping over CSS comments.
// Returns npos if not found.
size_t FindOpenBrace(std::string_view css, size_t pos) {
  while (pos < css.size()) {
    if (css[pos] == '/' && pos + 1 < css.size() && css[pos + 1] == '*') {
      size_t end = css.find("*/", pos + 2);
      if (end == std::string_view::npos) return std::string_view::npos;
      pos = end + 2;
      continue;
    }
    if (css[pos] == '{') return pos;
    ++pos;
  }
  return std::string_view::npos;
}

// Find the end of a top-level rule or at-rule block starting at `pos`.
// Returns the byte position past the end of this rule.
size_t FindRuleEnd(std::string_view css, size_t pos) {
  if (pos >= css.size()) return pos;

  // At-rules without blocks (@charset, @import) end at ';'.
  if (css[pos] == '@') {
    std::string_view remaining = css.substr(pos);
    if (remaining.starts_with("@charset") || remaining.starts_with("@import")) {
      // These end at the next semicolon.
      size_t semi = css.find(';', pos);
      if (semi == std::string_view::npos) return css.size();
      return semi + 1;
    }
    // Other at-rules (@font-face, @keyframes, @media) have blocks.
    size_t brace = FindOpenBrace(css, pos);
    if (brace == std::string_view::npos) {
      // Malformed -- consume to end or semicolon.
      size_t semi = css.find(';', pos);
      if (semi == std::string_view::npos) return css.size();
      return semi + 1;
    }
    return FindClosingBrace(css, brace);
  }

  // Regular rule: selector { declarations }.
  size_t brace = FindOpenBrace(css, pos);
  if (brace == std::string_view::npos) {
    // No block found -- might be trailing text.
    return css.size();
  }
  return FindClosingBrace(css, brace);
}

// Skip a CSS comment at pos. Returns position after the comment.
size_t SkipComment(std::string_view css, size_t pos) {
  if (pos + 1 < css.size() && css[pos] == '/' && css[pos + 1] == '*') {
    size_t end = css.find("*/", pos + 2);
    if (end == std::string_view::npos) return css.size();
    return end + 2;
  }
  return pos;
}

}  // namespace

UnusedCssRemovalResult UnusedCssRemover::Remove(
    std::string_view full_css,
    const std::vector<std::pair<size_t, size_t>>& used_ranges) {
  UnusedCssRemovalResult result;
  result.original_bytes = full_css.size();

  if (full_css.empty()) {
    return result;
  }

  // Sort and merge used ranges for efficient lookup.
  auto merged = SortAndMerge(used_ranges);

  std::string output;
  output.reserve(full_css.size());

  size_t pos = 0;
  size_t rules_total = 0;
  size_t rules_kept = 0;

  while (pos < full_css.size()) {
    // Skip whitespace between rules.
    size_t rule_start = SkipWhitespace(full_css, pos);
    if (rule_start >= full_css.size()) break;

    // Skip comments (preserve them -- they're cheap).
    if (rule_start + 1 < full_css.size() && full_css[rule_start] == '/' &&
        full_css[rule_start + 1] == '*') {
      size_t comment_end = SkipComment(full_css, rule_start);
      // Include the comment in output.
      if (!output.empty()) output += '\n';
      output.append(full_css.substr(rule_start, comment_end - rule_start));
      pos = comment_end;
      continue;
    }

    // Find the extent of this rule.
    size_t rule_end = FindRuleEnd(full_css, rule_start);
    ++rules_total;

    // Always preserve @charset, @import, @font-face, @keyframes.
    bool preserve = IsPreservedAtRule(full_css, rule_start);

    // Check if any byte in this rule's range overlaps with used ranges.
    bool used = preserve || OverlapsUsedRange(rule_start, rule_end, merged);

    if (used) {
      if (!output.empty()) output += '\n';
      output.append(full_css.substr(rule_start, rule_end - rule_start));
      ++rules_kept;
    }

    pos = rule_end;
  }

  result.rules_removed = rules_total - rules_kept;
  result.cleaned_css = std::move(output);
  result.cleaned_bytes = result.cleaned_css.size();
  if (result.original_bytes > 0) {
    result.removal_ratio =
        static_cast<float>(result.original_bytes - result.cleaned_bytes) /
        static_cast<float>(result.original_bytes);
  }

  return result;
}

std::vector<std::pair<size_t, size_t>> UnusedCssRemover::MergeViewportCoverage(
    const std::vector<std::vector<std::pair<size_t, size_t>>>&
        per_viewport_ranges) {
  // Collect all ranges from all viewports.
  std::vector<std::pair<size_t, size_t>> all_ranges;
  for (const auto& viewport_ranges : per_viewport_ranges) {
    all_ranges.insert(all_ranges.end(), viewport_ranges.begin(),
                      viewport_ranges.end());
  }

  return SortAndMerge(std::move(all_ranges));
}

}  // namespace pagespeed
