// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.
//
// This file is derived from mod_pagespeed and has been substantially modified.
// Originally licensed under Apache License, Version 2.0.
// Copyright (c) 2010-2017 Google Inc.
// Copyright (c) 2018 The Apache Software Foundation.

// String utilities providing functionality beyond C++23 and abseil.

#include "lib/base/string_util.h"

#include <algorithm>
#include <cassert>
#include <cstdio>
#include <vector>

namespace net_instaweb {

// =============================================================================
// HTML whitespace trimming
// =============================================================================

bool TrimLeadingHtmlWhitespace(std::string_view* str) {
  size_t trim = 0;
  while (trim < str->size() && IsHtmlSpace((*str)[trim])) {
    ++trim;
  }
  str->remove_prefix(trim);
  return trim > 0;
}

bool TrimTrailingHtmlWhitespace(std::string_view* str) {
  size_t size = str->size();
  while (size > 0 && IsHtmlSpace((*str)[size - 1])) {
    --size;
  }
  if (size != str->size()) {
    str->remove_suffix(str->size() - size);
    return true;
  }
  return false;
}

bool TrimHtmlWhitespace(std::string_view* str) {
  // Use bitwise OR to ensure both functions are always called.
  return (static_cast<int>(TrimLeadingHtmlWhitespace(str)) |
          static_cast<int>(TrimTrailingHtmlWhitespace(str))) != 0;
}

// =============================================================================
// Case-insensitive operations
// =============================================================================

int StringCaseCompare(std::string_view s1, std::string_view s2) {
  size_t n = std::min(s1.size(), s2.size());
  for (size_t i = 0; i < n; ++i) {
    unsigned char c1 = UpperChar(s1[i]);
    unsigned char c2 = UpperChar(s2[i]);
    if (c1 < c2) return -1;
    if (c1 > c2) return 1;
  }
  if (s1.size() < s2.size()) return -1;
  if (s1.size() > s2.size()) return 1;
  return 0;
}

bool StringCaseStartsWith(std::string_view str, std::string_view prefix) {
  return str.size() >= prefix.size() &&
         StringCaseCompare(str.substr(0, prefix.size()), prefix) == 0;
}

bool StringCaseEndsWith(std::string_view str, std::string_view suffix) {
  return str.size() >= suffix.size() &&
         StringCaseCompare(str.substr(str.size() - suffix.size()), suffix) == 0;
}

std::string_view::size_type FindIgnoreCase(std::string_view haystack,
                                           std::string_view needle) {
  if (needle.empty()) return 0;
  if (needle.size() > haystack.size()) return std::string_view::npos;

  for (size_t i = 0; i <= haystack.size() - needle.size(); ++i) {
    if (StringCaseStartsWith(haystack.substr(i), needle)) {
      return i;
    }
  }
  return std::string_view::npos;
}

// =============================================================================
// String escaping
// =============================================================================

std::string CEscape(std::string_view src) {
  std::string result;
  result.reserve(src.size() * 2);  // Reasonable estimate

  for (unsigned char ch : src) {
    switch (ch) {
      case '\n':
        result += "\\n";
        break;
      case '\r':
        result += "\\r";
        break;
      case '\t':
        result += "\\t";
        break;
      case '\"':
        result += "\\\"";
        break;
      case '\'':
        result += "\\'";
        break;
      case '\\':
        result += "\\\\";
        break;
      default:
        if (ch < 32 || ch >= 127) {
          char buf[5];
          snprintf(buf, sizeof(buf), "\\%03o", ch);
          result += buf;
        } else {
          result += static_cast<char>(ch);
        }
        break;
    }
  }
  return result;
}

void BackslashEscape(std::string_view src, std::string_view to_escape,
                     std::string* dest) {
  dest->reserve(dest->size() + src.size());
  for (char c : src) {
    if (to_escape.find(c) != std::string_view::npos) {
      dest->push_back('\\');
    }
    dest->push_back(c);
  }
}

// =============================================================================
// String manipulation
// =============================================================================

int GlobalReplaceSubstring(std::string_view substring,
                           std::string_view replacement, std::string* s) {
  assert(s != nullptr);
  if (s->empty() || substring.empty()) return 0;

  std::string result;
  int count = 0;
  size_t pos = 0;

  while (true) {
    size_t found = s->find(substring, pos);
    if (found == std::string::npos) {
      result.append(*s, pos, std::string::npos);
      break;
    }
    result.append(*s, pos, found - pos);
    result.append(replacement);
    pos = found + substring.size();
    ++count;
  }

  if (count > 0) {
    s->swap(result);
  }
  return count;
}

int GlobalEraseBracketedSubstring(std::string_view left, std::string_view right,
                                  std::string* s) {
  assert(s != nullptr);
  if (s->empty() || left.empty() || right.empty()) return 0;
  size_t pos = s->find(left);
  if (pos == std::string::npos) return 0;

  std::string result;
  result.reserve(s->size());
  int count = 0;
  size_t keep_start = 0;

  while (pos != std::string::npos) {
    result.append(*s, keep_start, pos - keep_start);
    size_t end_pos = s->find(right, pos + left.size());
    if (end_pos == std::string::npos) {
      keep_start = pos;
      break;
    }
    keep_start = end_pos + right.size();
    ++count;
    pos = s->find(left, keep_start);
  }

  result.append(*s, keep_start, std::string::npos);
  s->swap(result);
  return count;
}

int CountSubstring(std::string_view text, std::string_view substring) {
  if (substring.empty()) return 0;
  int count = 0;
  size_t pos = 0;
  while ((pos = text.find(substring, pos)) != std::string_view::npos) {
    ++count;
    ++pos;  // Allow overlapping matches
  }
  return count;
}

// =============================================================================
// Security utilities
// =============================================================================

bool ConstantTimeCompare(std::string_view a, std::string_view b) {
  // Use size_t for the result accumulator to avoid truncation.  The old
  // unsigned-char accumulator silently truncated the size XOR, producing
  // false equality when lengths differed by a multiple of 256.
  volatile size_t result = a.size() ^ b.size();
  size_t max_len = std::max(a.size(), b.size());
  for (size_t i = 0; i < max_len; ++i) {
    unsigned char ca = i < a.size() ? static_cast<unsigned char>(a[i]) : 0;
    unsigned char cb = i < b.size() ? static_cast<unsigned char>(b[i]) : 0;
    result |= ca ^ cb;
  }
  return result == 0;
}

int CountCharacterMismatches(std::string_view s1, std::string_view s2) {
  int mismatches = 0;
  size_t n = std::min(s1.size(), s2.size());
  for (size_t i = 0; i < n; ++i) {
    mismatches += static_cast<int>(s1[i] != s2[i]);
  }
  return mismatches + static_cast<int>(std::max(s1.size(), s2.size()) - n);
}

// =============================================================================
// Parsing utilities
// =============================================================================

void ParseShellLikeString(std::string_view input,
                          std::vector<std::string>* output) {
  output->clear();
  size_t i = 0;

  while (i < input.size()) {
    char c = input[i];

    if (c == '"' || c == '\'') {
      // Quoted string
      char quote = c;
      ++i;
      std::string part;
      while (i < input.size() && input[i] != quote) {
        if (input[i] == '\\' && i + 1 < input.size()) {
          ++i;  // Skip backslash
        }
        part.push_back(input[i]);
        ++i;
      }
      if (i < input.size()) ++i;  // Skip closing quote.
      output->push_back(std::move(part));
    } else if (!IsHtmlSpace(c)) {
      // Unquoted token
      std::string part;
      while (i < input.size() && !IsHtmlSpace(input[i])) {
        part.push_back(input[i]);
        ++i;
      }
      output->push_back(std::move(part));
    } else {
      ++i;  // Skip whitespace
    }
  }
}

std::string_view PieceAfterEquals(std::string_view piece) {
  size_t pos = piece.find('=');
  if (pos == std::string_view::npos) {
    return {};
  }
  std::string_view result = piece.substr(pos + 1);
  TrimHtmlWhitespace(&result);
  return result;
}

// =============================================================================
// Numeric parsing helpers
// =============================================================================

bool AccumulateDecimalValue(char c, uint32_t* value) {
  if (c >= '0' && c <= '9') {
    *value = (*value * 10) + (c - '0');
    if (*value > 0x110000) *value = 0x110000;  // Clamp above Unicode max.
    return true;
  }
  return false;
}

bool AccumulateHexValue(char c, uint32_t* value) {
  if (c >= '0' && c <= '9') {
    *value = (*value << 4) | (c - '0');
  } else if (c >= 'a' && c <= 'f') {
    *value = (*value << 4) | (c - 'a' + 10);
  } else if (c >= 'A' && c <= 'F') {
    *value = (*value << 4) | (c - 'A' + 10);
  } else {
    return false;
  }
  if (*value > 0x110000) *value = 0x110000;  // Clamp above Unicode max.
  return true;
}

}  // namespace net_instaweb
