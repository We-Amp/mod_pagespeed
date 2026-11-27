// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.
//
// This file is derived from mod_pagespeed and has been substantially modified.
// Originally licensed under Apache License, Version 2.0.
// Copyright (c) 2010-2017 Google Inc.
// Copyright (c) 2018 The Apache Software Foundation.

// String utilities providing functionality beyond C++23 and abseil.
// For standard operations, use std::string_view and absl::* directly.

#ifndef PAGESPEED_LIB_BASE_STRING_UTIL_H_
#define PAGESPEED_LIB_BASE_STRING_UTIL_H_

#include <cstdint>
#include <cstdio>
#include <string>
#include <string_view>
#include <vector>

namespace net_instaweb {

// =============================================================================
// HTML-specific utilities (not locale-dependent, HTML5 spec compliant)
// =============================================================================

// Check if character is an HTML space (not the same as isspace!).
// HTML spaces: space, tab, LF, FF, CR (notably excludes vertical tab \v).
// See: https://html.spec.whatwg.org/multipage/infrastructure.html#space-character
inline bool IsHtmlSpace(char c) {
  return c == ' ' || c == '\t' || c == '\n' || c == '\f' || c == '\r';
}

// Trim leading/trailing HTML whitespace (in-place modification of view).
// Returns true if any whitespace was trimmed.
bool TrimHtmlWhitespace(std::string_view* str);
bool TrimLeadingHtmlWhitespace(std::string_view* str);
bool TrimTrailingHtmlWhitespace(std::string_view* str);

// =============================================================================
// Case-insensitive operations (locale-independent, ASCII only)
// =============================================================================

// Locale-independent ASCII case conversion.
inline char UpperChar(char c) {
  return (c >= 'a' && c <= 'z') ? (c - 'a' + 'A') : c;
}

inline char LowerChar(char c) {
  return (c >= 'A' && c <= 'Z') ? (c - 'A' + 'a') : c;
}

// Convert a string to ASCII lowercase (returns a new string).
inline std::string AsciiToLower(std::string_view sv) {
  std::string result(sv);
  for (char& c : result) c = LowerChar(c);
  return result;
}

// Convert a string to ASCII lowercase in place.
inline void AsciiToLowerInPlace(std::string& s) {
  for (char& c : s) c = LowerChar(c);
}

// Case-insensitive comparison (locale-independent).
// Returns: <0 if s1<s2, 0 if equal, >0 if s1>s2
int StringCaseCompare(std::string_view s1, std::string_view s2);

inline bool StringCaseEqual(std::string_view s1, std::string_view s2) {
  return s1.size() == s2.size() && StringCaseCompare(s1, s2) == 0;
}

bool StringCaseStartsWith(std::string_view str, std::string_view prefix);
bool StringCaseEndsWith(std::string_view str, std::string_view suffix);

// Find needle in haystack, case-insensitive. Returns npos if not found.
std::string_view::size_type FindIgnoreCase(std::string_view haystack,
                                           std::string_view needle);

// =============================================================================
// String escaping
// =============================================================================

// C-style escape: converts \n, \r, \t, quotes, backslash, and non-printable
// characters to escape sequences (\n, \r, \t, \", \', \\, \ooo).
std::string CEscape(std::string_view src);

// Escape specified characters with backslash. Appends to dest.
void BackslashEscape(std::string_view src, std::string_view to_escape,
                     std::string* dest);

// =============================================================================
// String manipulation
// =============================================================================

// Replace all occurrences of substring. Returns count of replacements.
// Note: replacements are not subject to re-matching.
int GlobalReplaceSubstring(std::string_view substring,
                           std::string_view replacement, std::string* s);

// Erase substrings bracketed by left/right markers.
// Example: ("[", "]", "a[b]c[d]e") -> "ace", returns 2
int GlobalEraseBracketedSubstring(std::string_view left, std::string_view right,
                                  std::string* s);

// Count non-overlapping occurrences of substring in text.
int CountSubstring(std::string_view text, std::string_view substring);

// =============================================================================
// Security utilities
// =============================================================================

// Constant-time string comparison to prevent timing attacks.
// Always compares all bytes regardless of early mismatches.
// Use for comparing tokens, signatures, passwords, etc.
bool ConstantTimeCompare(std::string_view a, std::string_view b);

// Count character mismatches (for non-security timing-safe comparison stats).
int CountCharacterMismatches(std::string_view s1, std::string_view s2);

// =============================================================================
// Parsing utilities
// =============================================================================

// Parse shell-like string with quote handling.
// Example: 'a b "c d" e' -> ["a", "b", "c d", "e"]
// Used for HTML doctype parsing.
void ParseShellLikeString(std::string_view input,
                          std::vector<std::string>* output);

// Extract value after '=' sign, trimming whitespace.
// Example: "key = value" -> "value", "noequals" -> ""
std::string_view PieceAfterEquals(std::string_view piece);

// =============================================================================
// Numeric parsing helpers (for HTML entity decoding)
// =============================================================================

// Accumulate digit into value. Returns false if c is not a valid digit.
bool AccumulateDecimalValue(char c, uint32_t* value);
bool AccumulateHexValue(char c, uint32_t* value);

// Character classification (locale-independent).
inline bool IsHexDigit(char c) {
  return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') ||
         (c >= 'A' && c <= 'F');
}

inline bool IsDecimalDigit(char c) { return c >= '0' && c <= '9'; }

inline bool IsAsciiAlphaNumeric(char c) {
  return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
         (c >= '0' && c <= '9');
}

// RFC 8259-minimal JSON string escaping. Handles all required escapes:
// backslash, double-quote, \b, \f, \n, \r, \t, and \u00XX for other control
// chars. It passes '<', '>', and '&' through unescaped.
//
// NOT safe to embed inside an HTML <script> block: a value containing the
// literal "</script>" would break out of the script element. For any output
// that lands in an HTML document, use JsonEscapeHtmlSafe instead.
//
// This is the correct choice for pure-JSON sinks where the bytes never enter
// an HTML context: API/IPC response bodies, tokens, and log lines.
inline std::string JsonEscapeMinimal(std::string_view s) {
  std::string out;
  out.reserve(s.size());
  for (char c : s) {
    switch (c) {
      case '"':
        out.append("\\\"");
        break;
      case '\\':
        out.append("\\\\");
        break;
      case '\b':
        out.append("\\b");
        break;
      case '\f':
        out.append("\\f");
        break;
      case '\n':
        out.append("\\n");
        break;
      case '\r':
        out.append("\\r");
        break;
      case '\t':
        out.append("\\t");
        break;
      default:
        if (static_cast<unsigned char>(c) < 0x20) {
          char buf[8];
          std::snprintf(buf, sizeof(buf), "\\u%04x",
                        static_cast<unsigned>(static_cast<unsigned char>(c)));
          out.append(buf);
        } else {
          out.push_back(c);
        }
        break;
    }
  }
  return out;
}

// Like JsonEscapeMinimal but also escapes '<' and '>' (as \u003c / \u003e),
// making the result safe to embed inside an HTML <script> block (prevents
// </script> injection and --> comment breakout).
//
// This is NOT a general-purpose escaper: it is for JSON embedded in a <script>
// block only. It is not suitable for HTML attribute values, URLs, or CSS.
//
// It deliberately does not escape '&', nor either of the U+2028 LINE SEPARATOR
// and U+2029 PARAGRAPH SEPARATOR characters.  That passthrough is safe for the
// sink this escaper exists for: the sole caller emits JSON into a
// <script type="speculationrules"> block, which the browser parses as JSON, and
// U+2028/U+2029 are legal unescaped inside a JSON string per RFC 8259.  The
// historic separator hazard applies only when JSON bytes are inlined into JS
// SOURCE (and even there only on pre-ES2019 engines, which did not treat them
// as valid in string literals).  A caller that needs a JS-source sink must not
// use this function without adding U+2028/U+2029 escaping of its own.
inline std::string JsonEscapeHtmlSafe(std::string_view s) {
  std::string out;
  out.reserve(s.size());
  for (char c : s) {
    switch (c) {
      case '"':
        out.append("\\\"");
        break;
      case '\\':
        out.append("\\\\");
        break;
      case '<':
        out.append("\\u003c");
        break;
      case '>':
        out.append("\\u003e");
        break;
      case '\b':
        out.append("\\b");
        break;
      case '\f':
        out.append("\\f");
        break;
      case '\n':
        out.append("\\n");
        break;
      case '\r':
        out.append("\\r");
        break;
      case '\t':
        out.append("\\t");
        break;
      default:
        if (static_cast<unsigned char>(c) < 0x20) {
          char buf[8];
          std::snprintf(buf, sizeof(buf), "\\u%04x",
                        static_cast<unsigned>(static_cast<unsigned char>(c)));
          out.append(buf);
        } else {
          out.push_back(c);
        }
        break;
    }
  }
  return out;
}

// Return the numeric value of a hex digit (0-15), or -1 for non-hex chars.
inline int HexDigitValue(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

// Iterate over percent-encoded input, decoding %XX sequences.
// For each valid %XX, calls on_decoded(result, decoded_char, hi_nibble, lo_nibble).
// For all other characters, calls on_literal(result, char).
template <typename OnDecoded, typename OnLiteral>
std::string TransformPercentEncoded(std::string_view input,
                                    OnDecoded on_decoded,
                                    OnLiteral on_literal) {
  std::string result;
  result.reserve(input.size());
  for (size_t i = 0; i < input.size(); ++i) {
    if (input[i] == '%' && i + 2 < input.size()) {
      int hi = HexDigitValue(input[i + 1]);
      int lo = HexDigitValue(input[i + 2]);
      if (hi >= 0 && lo >= 0) {
        on_decoded(result, static_cast<char>((hi << 4) | lo), hi, lo);
        i += 2;
        continue;
      }
    }
    on_literal(result, input[i]);
  }
  return result;
}

}  // namespace net_instaweb

#endif  // PAGESPEED_LIB_BASE_STRING_UTIL_H_
