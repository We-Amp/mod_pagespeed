// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

// PageSpeed 2.0 - Font URL Extractor Implementation

#include "src/worker/font_url_extractor.h"

#include <cctype>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

#include "absl/strings/match.h"
#include "lib/base/string_util.h"

namespace pagespeed {

namespace {

constexpr size_t kMaxFontUrls = 10;

// Skip whitespace at pos, return new pos.
size_t SkipWs(std::string_view s, size_t pos) {
  while (pos < s.size() &&
         (std::isspace(static_cast<unsigned char>(s[pos])) != 0)) {
    ++pos;
  }
  return pos;
}

// Extract a URL from url(...) at pos. Advances pos past ')'. Returns empty
// string on failure.
std::string ExtractUrl(std::string_view css, size_t& pos) {
  // Expect "url("
  if (pos + 3 >= css.size()) return {};
  if (!absl::StartsWithIgnoreCase(css.substr(pos, 4), "url(")) return {};
  pos += 4;

  pos = SkipWs(css, pos);
  if (pos >= css.size()) return {};

  std::string url;
  if (css[pos] == '"' || css[pos] == '\'') {
    char quote = css[pos++];
    size_t start = pos;
    while (pos < css.size() && css[pos] != quote) {
      if (css[pos] == '\\' && pos + 1 < css.size()) {
        pos += 2;
      } else {
        ++pos;
      }
    }
    url = std::string(css.substr(start, pos - start));
    if (pos < css.size()) ++pos;  // skip closing quote
  } else {
    // Unquoted URL
    size_t start = pos;
    while (pos < css.size() && css[pos] != ')' &&
           (std::isspace(static_cast<unsigned char>(css[pos])) == 0)) {
      ++pos;
    }
    url = std::string(css.substr(start, pos - start));
  }

  pos = SkipWs(css, pos);
  if (pos < css.size() && css[pos] == ')') ++pos;  // skip ')'
  return url;
}

// Check if a URL looks like a woff2 file (by extension).
bool LooksLikeWoff2(std::string_view url) {
  // Strip query string and fragment for extension check.
  auto qpos = url.find('?');
  if (qpos != std::string_view::npos) url = url.substr(0, qpos);
  auto fpos = url.find('#');
  if (fpos != std::string_view::npos) url = url.substr(0, fpos);
  return url.size() >= 6 && absl::EndsWithIgnoreCase(url, ".woff2");
}

// Parse a single @font-face src: value and extract the best woff2 URL.
// Only returns woff2 URLs (by format() hint or extension). Returns empty
// string if no woff2 URL found.
std::string ParseFontSrcValue(std::string_view src_value) {
  size_t pos = 0;
  while (pos < src_value.size()) {
    pos = SkipWs(src_value, pos);
    if (pos >= src_value.size()) break;

    if (absl::StartsWithIgnoreCase(src_value.substr(pos), "url(")) {
      std::string url = ExtractUrl(src_value, pos);
      if (url.empty() || absl::StartsWithIgnoreCase(url, "data:")) {
        // Skip data: URLs — continue to next source.
      } else {
        // Check for format('woff2') hint after the url().
        pos = SkipWs(src_value, pos);
        bool format_woff2 = false;
        if (pos < src_value.size() &&
            absl::StartsWithIgnoreCase(src_value.substr(pos), "format(")) {
          size_t paren_end = src_value.find(')', pos + 7);
          if (paren_end != std::string_view::npos) {
            std::string_view fmt_str =
                src_value.substr(pos + 7, paren_end - pos - 7);
            // Strip quotes around format value.
            size_t qs = fmt_str.find_first_of("'\"");
            if (qs != std::string_view::npos) {
              char q = fmt_str[qs];
              size_t qe = fmt_str.find(q, qs + 1);
              if (qe != std::string_view::npos) {
                fmt_str = fmt_str.substr(qs + 1, qe - qs - 1);
              }
            }
            format_woff2 = absl::StartsWithIgnoreCase(fmt_str, "woff2");
            pos = paren_end + 1;
          }
        }

        bool is_woff2 = format_woff2 || LooksLikeWoff2(url);
        if (is_woff2) {
          return url;  // Found a woff2 URL — return immediately.
        }
      }
    }

    // Advance past current source entry (skip to comma or end).
    while (pos < src_value.size() && src_value[pos] != ',') {
      if (src_value[pos] == '(') {
        // Skip past balanced parens (e.g., format(...), local(...)).
        size_t pe = src_value.find(')', pos);
        pos = (pe != std::string_view::npos) ? pe + 1 : src_value.size();
      } else {
        ++pos;
      }
    }
    if (pos < src_value.size() && src_value[pos] == ',') ++pos;
  }

  return {};  // No woff2 URL found.
}

}  // namespace

std::vector<std::string> ExtractFontUrls(std::string_view css) {
  std::vector<std::string> urls;
  std::unordered_set<std::string> seen;
  if (css.empty()) return urls;

  size_t pos = 0;
  while (pos < css.size() && urls.size() < kMaxFontUrls) {
    // Find @font-face (case-insensitive) by searching for '@' first.
    size_t ff_pos = std::string_view::npos;
    for (size_t at = css.find('@', pos); at != std::string_view::npos;
         at = css.find('@', at + 1)) {
      if (at + 10 <= css.size() &&
          absl::StartsWithIgnoreCase(css.substr(at, 10), "@font-face")) {
        ff_pos = at;
        break;
      }
    }
    if (ff_pos == std::string_view::npos) break;

    // Find the opening brace.
    size_t brace_start = css.find('{', ff_pos + 10);
    if (brace_start == std::string_view::npos) break;

    // Find the matching closing brace (simple: no nesting in @font-face).
    size_t brace_end = css.find('}', brace_start + 1);
    if (brace_end == std::string_view::npos) break;

    std::string_view block =
        css.substr(brace_start + 1, brace_end - brace_start - 1);

    // Find "src:" property within the block.
    size_t src_pos = 0;
    while (src_pos < block.size()) {
      // Look for "src" as a property name (case-insensitive).
      // Search for 's'/'S' first to avoid per-char StartsWithIgnoreCase.
      size_t si = std::string_view::npos;
      for (size_t i = src_pos; i + 3 <= block.size(); ++i) {
        char c = net_instaweb::LowerChar(block[i]);
        if (c == 's' && absl::StartsWithIgnoreCase(block.substr(i, 3), "src") &&
            (i == 0 ||
             !std::isalnum(static_cast<unsigned char>(block[i - 1])))) {
          si = i;
          break;
        }
      }
      if (si == std::string_view::npos) break;

      size_t after_src = si + 3;
      // Skip whitespace after "src".
      after_src = SkipWs(block, after_src);
      if (after_src < block.size() && block[after_src] == ':') {
        ++after_src;
        // Find value end (semicolon or end of block).
        size_t val_end = block.find(';', after_src);
        if (val_end == std::string_view::npos) val_end = block.size();
        std::string_view src_value =
            block.substr(after_src, val_end - after_src);

        std::string url = ParseFontSrcValue(src_value);
        if (!url.empty()) {
          // Deduplicate.
          if (seen.insert(url).second) {
            urls.push_back(std::move(url));
          }
        }
        src_pos = val_end;
      } else {
        src_pos = after_src;
      }
    }

    pos = brace_end + 1;
  }

  return urls;
}

}  // namespace pagespeed
