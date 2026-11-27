// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.
//
// This file is derived from mod_pagespeed and has been substantially modified.
// Originally licensed under Apache License, Version 2.0.
// Copyright (c) 2010-2017 Google Inc.
// Copyright (c) 2018 The Apache Software Foundation.

#include "lib/css/css_minify.h"

#include <cctype>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "lib/css/css_phases.h"

namespace pagespeed::css {
namespace {

// CSS identifier code point (letter, digit, '_', or non-ASCII byte —
// the high bit covers every byte of a multi-byte UTF-8 sequence).
// '-' is deliberately excluded: it needs context (a bare '-' starts a
// signed number, an ident-adjacent '-' continues a dashed ident).
bool IsIdentChar(char c) {
  return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
         (c >= '0' && c <= '9') || c == '_' ||
         static_cast<unsigned char>(c) >= 0x80;
}

bool IsHexDigit(char c) {
  return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') ||
         (c >= 'A' && c <= 'F');
}

// True if the logical character ending at emitted[pos] is identifier
// content, decoding escape *structure* rather than just raw bytes
// (#1175): a plain ident char, the second char of a `\<char>` pair
// escape, or the final char of a `\<hex>{1,6}<ws>?` hex escape — its
// last hex digit, or the single whitespace terminator, which the
// escape consumes (it is not a token separator: in ".a\35 0.5" the
// space terminates "\35", the decoded ident is "a50", and the '0' is
// ident content).  Escaped code points are always ident content, so
// the decoded value is never consulted.  Only the bytes ending at pos
// are examined; callers pass the output built so far, whose tail is
// verbatim input (a stripped byte inside the lookback window would
// leave a '.' behind, breaking the escape pattern by construction).
bool IsIdentContentAt(const std::string& emitted, size_t pos) {
  char c = emitted[pos];
  if (IsIdentChar(c)) return true;
  // An odd backslash run immediately before pos means c is the second
  // char of a `\<char>` pair escape (".a\'0.5", ".a\\-0.5").
  size_t bs = 0;
  while (bs < pos && emitted[pos - 1 - bs] == '\\') ++bs;
  if (bs % 2 != 0) return true;
  if (!IsCssWhitespace(c)) return false;
  // Whitespace directly after `\<hex>{1,6}` is the escape's consumed
  // terminator.  More than 6 hex digits means the escape closed
  // earlier and the whitespace is a real separator.
  size_t h = 0;
  while (h < pos && IsHexDigit(emitted[pos - 1 - h])) ++h;
  if (h < 1 || h > 6 || h + 1 > pos || emitted[pos - 1 - h] != '\\') {
    return false;
  }
  // The backslash introduces the escape unless it is itself escaped.
  size_t run = 0;
  while (run < pos - 1 - h && emitted[pos - 2 - h - run] == '\\') ++run;
  return run % 2 == 0;
}

// Ident/name constituent (property names and the property-name-ish run
// Phase 5 looks for, #1167): alphanumerics, '_', '-', non-ASCII bytes.
// Backslash escape pairs are handled by the callers' scanners.
bool IsNameChar(char c) {
  unsigned char u = static_cast<unsigned char>(c);
  return (u >= 'a' && u <= 'z') || (u >= 'A' && u <= 'Z') ||
         (u >= '0' && u <= '9') || c == '_' || c == '-' || u >= 0x80;
}

// True if a '{' at this point opens a brace group inside a declaration
// VALUE (#1167) rather than a real block — the group is opaque value
// content, to be preserved verbatim (custom-property values are
// observed verbatim by var()/getPropertyValue(); ordinary values
// corrupt the same way).  The current segment — text emitted since
// the last segment boundary (';', '}', or a completed block) — is
// declaration-like when it has a top-level ':' and its pre-colon run
// is property-name-ish (names, escapes, whitespace).  At top level a
// colon usually means a pseudo-class selector (":root", "a:hover"),
// so only custom properties qualify there; bare top-level declarations
// are invalid CSS anyway.  Shared by Phases 3 and 5 so both agree on
// where value groups live.
bool IsValueGroupBrace(const std::string& emitted, size_t seg_begin,
                       size_t seg_colon, bool top_level) {
  if (seg_colon == std::string::npos) return false;
  size_t name_begin = seg_begin;
  while (name_begin < seg_colon && IsCssWhitespace(emitted[name_begin]))
    ++name_begin;
  // An empty pre-colon run is a pseudo-class/element selector prelude
  // (":hover", "::before") — unambiguous CSS Nesting, never a value
  // group (review on PR-E).
  if (name_begin == seg_colon) return false;
  // An escaped char is name content whatever it is ("--\>p" is a valid
  // custom-property name).  Without the escape check, such a name
  // scanned as not-a-declaration and the value's brace group was
  // collapsed as a nested block, rewriting the opaque value.  Uses the
  // same escape predicate as AtCustomPropertyColon.
  for (size_t k = name_begin; k < seg_colon; ++k) {
    char sc = emitted[k];
    if (!IsNameChar(sc) && sc != '\\' && !IsCssWhitespace(sc) &&
        !IsEscapeContentAt(emitted, k)) {
      return false;
    }
  }
  if (top_level && emitted.compare(name_begin, 2, "--") != 0) return false;
  return true;
}

// Phase 3: Remove trailing semicolons before closing braces.
// Also handles consecutive semicolons before }.
std::string Phase3(std::string_view input) {
  std::string r;
  r.reserve(input.size());
  State state = State::kNormal;
  bool in_custom = false;
  bool custom_url = false;
  int custom_bd = 0;
  int pd = 0;  // Paren/bracket depth outside custom values.
  // #1167: the ';'-before-'}' trim must not fire inside a brace group
  // nested in a declaration value ("a{b:{L;}}" lost the group's ';' —
  // the group is value content, not a block).  Tracked like Phase 5
  // (same IsValueGroupBrace classification, so both phases agree):
  // bd = real block depth, value_bd = value-group nesting, and the
  // current segment's start/first-':' in r.
  int bd = 0;
  int value_bd = 0;
  size_t seg_begin = 0;
  size_t seg_colon = std::string::npos;
  int seg_group = -1;  // cached IsValueGroupBrace verdict (-1 = unknown)
  size_t i = 0;
  while (i < input.size()) {
    char c = input[i];
    switch (state) {
      case State::kNormal:
        if (c == '\\' && i + 1 < input.size()) {
          // Backslash escape (mirrors Phase 2): consume the pair
          // verbatim so an escaped quote does not open a phantom string
          // here and an escaped ';' is not trimmed.
          r += c;
          r += input[i + 1];
          i += 2;
          continue;
        }
        if (in_custom) {
          // Custom-property value: opaque token stream (mirrors Phase
          // 2's in_custom, including its custom_url shielding, so every
          // phase agrees on where the value ends — the narrow
          // depth-only version of this block diverged on "--:url({); }"
          // because Phase 1/2's custom_url does no depth tracking).
          // The trailing-';' trim must not fire inside the value
          // (a{--x:{;}} lost the ';' — valid-input corruption, audit
          // P3-d): the value ends at the ';' or '}' at its own nesting
          // depth, and the terminator itself is re-processed below so
          // a{--x:v;} still trims.
          if (custom_url) {
            // Inside an unquoted url() token: mirror Phase 1/2's
            // shielding — copy verbatim (no depth tracking, no string
            // states) until the closing ')'.
            if (c == ')') custom_url = false;
            r += c;
            ++i;
            continue;
          }
          if (c == '\'') {
            r += c;
            state = State::kInSingleStr;
            ++i;
            continue;
          }
          if (c == '"') {
            r += c;
            state = State::kInDoubleStr;
            ++i;
            continue;
          }
          if (c == '(' && r.size() >= 3 && CiMatch(r[r.size() - 3], 'u') &&
              CiMatch(r[r.size() - 2], 'r') && CiMatch(r[r.size() - 1], 'l')) {
            // url( inside a custom value: unquoted content enters the
            // shielded mode above, mirroring Phases 1/2 (which likewise
            // keep the whitespace after '(' verbatim here).
            size_t j = i + 1;
            while (j < input.size() && IsCssWhitespace(input[j])) ++j;
            if (j >= input.size() || (input[j] != '\'' && input[j] != '"')) {
              custom_url = true;
              r += c;
              ++i;
              continue;
            }
          }
          if (c == '{' || c == '(' || c == '[') {
            ++custom_bd;
          } else if (custom_bd > 0) {
            if (c == '}' || c == ')' || c == ']') --custom_bd;
          } else if (c == ';' || c == '}') {
            in_custom = false;
          }
          if (in_custom) {
            r += c;
            ++i;
            continue;
          }
          // Fall through: the terminating ';' or '}' is handled by the
          // normal rules below.
        }
        if (c == '\'') {
          r += c;
          state = State::kInSingleStr;
          ++i;
          continue;
        }
        if (c == '"') {
          r += c;
          state = State::kInDoubleStr;
          ++i;
          continue;
        }
        if (c == '(' && r.size() >= 3 && CiMatch(r[r.size() - 3], 'u') &&
            CiMatch(r[r.size() - 2], 'r') && CiMatch(r[r.size() - 1], 'l')) {
          // Unquoted url(): ';' and '}' are legal url code points, so
          // the trailing-semicolon trim must not fire inside one
          // (a{background:url(x;}y)} lost its ';' — valid-input
          // corruption, #1158).  Mirrors Phase 2's url entry
          // (whitespace-skip quote test after '(', quoted
          // url("...")/url('...') falls through to the string states).
          size_t j = i + 1;
          while (j < input.size() && IsCssWhitespace(input[j])) ++j;
          if (j >= input.size() || (input[j] != '\'' && input[j] != '"')) {
            r += c;
            // Unlike Phase 2's entry, do NOT skip the whitespace after
            // '(': custom values are intercepted by the in_custom block
            // above and can never reach this entry, and Phase 1 already
            // removed post-'(' whitespace from non-custom urls, so this
            // entry only ever sees whitespace-free input in-pipeline —
            // keep the bytes verbatim rather than skip for a case that
            // cannot occur.
            ++i;
            state = State::kInUrl;
            continue;
          }
        }
        if (c == ':' && AtCustomPropertyColon(r)) {
          // Entering a custom-property value (see AtCustomPropertyColon).
          r += c;
          in_custom = true;
          custom_bd = 0;
          ++i;
          continue;
        }
        if (value_bd > 0) {
          // Inside a value brace group (#1167): adjust nesting only —
          // nothing in this phase transforms group content, and the
          // trim below is gated on value_bd == 0.
          if (c == '{')
            ++value_bd;
          else if (c == '}')
            --value_bd;
          r += c;
          ++i;
          continue;
        }
        if (c == ':' && pd == 0 && seg_colon == std::string::npos) {
          seg_colon = r.size();
          r += c;
          ++i;
          continue;
        }
        if (c == '{') {
          // Cached per segment: the pre-colon run never changes once
          // seg_colon is set (r only appends), and re-scanning it per
          // '{' is quadratic on long values (review).
          if (seg_group < 0) {
            seg_group =
                IsValueGroupBrace(r, seg_begin, seg_colon, bd == 0) ? 1 : 0;
          }
          if (seg_group == 1) {
            ++value_bd;
            r += c;
            ++i;
            continue;  // Segment continues — still inside the value.
          }
          ++bd;
          r += c;
          ++i;
          seg_begin = r.size();
          seg_colon = std::string::npos;
          seg_group = -1;
          continue;
        }
        if (c == ';' && pd == 0) {
          // Look ahead past whitespace and additional semicolons for '}'.
          // Inside parens/brackets (pd > 0) a ';' is not a declaration
          // terminator — never trim there (audit P3-d).
          size_t j = i + 1;
          while (j < input.size() &&
                 (IsCssWhitespace(input[j]) || input[j] == ';')) {
            ++j;
          }
          if (j < input.size() && input[j] == '}') {
            ++i;
            seg_begin = r.size();
            seg_colon = std::string::npos;
            seg_group = -1;
            continue;
          }
          // Declaration terminator: ends the segment either way.
          r += c;
          ++i;
          seg_begin = r.size();
          seg_colon = std::string::npos;
          seg_group = -1;
          continue;
        }
        if (c == '}') {
          if (bd > 0) --bd;
          r += c;
          ++i;
          seg_begin = r.size();
          seg_colon = std::string::npos;
          seg_group = -1;
          continue;
        }
        if (c == '(' || c == '[') {
          ++pd;
        } else if (pd > 0 && (c == ')' || c == ']')) {
          --pd;
        }
        r += c;
        ++i;
        break;
      case State::kInSingleStr:
        r += c;
        if (c == '\\' && i + 1 < input.size()) {
          r += input[i + 1];
          i += 2;
          continue;
        }
        if (c == '\'') state = State::kNormal;
        ++i;
        break;
      case State::kInDoubleStr:
        r += c;
        if (c == '\\' && i + 1 < input.size()) {
          r += input[i + 1];
          i += 2;
          continue;
        }
        if (c == '"') state = State::kNormal;
        ++i;
        break;
      case State::kInUrl:
        // Verbatim to the closing ')' (#1158); escape pairs keep an
        // escaped ')' from closing the url early.
        r += c;
        if (c == '\\' && i + 1 < input.size()) {
          r += input[i + 1];
          i += 2;
          continue;
        }
        if (c == ')') state = State::kNormal;
        ++i;
        break;
      default:
        r += c;
        ++i;
        break;
    }
  }
  return r;
}

// Phase 4: Optimize decimal numbers (0.5 -> .5, -0.5 -> -.5).
std::string Phase4(std::string_view input) {
  std::string r;
  r.reserve(input.size());
  State state = State::kNormal;
  bool in_custom = false;
  bool custom_url = false;
  int custom_bd = 0;
  size_t i = 0;
  while (i < input.size()) {
    char c = input[i];
    switch (state) {
      case State::kNormal:
        if (c == '\\' && i + 1 < input.size()) {
          // Backslash escape (mirrors Phase 2): consume the pair
          // verbatim so an escaped '0' is not treated as a number start
          // (".a\0.5" lost the 0 — audit P4-a).
          r += c;
          r += input[i + 1];
          i += 2;
          continue;
        }
        if (in_custom) {
          // Custom-property value: opaque token stream — decimal
          // optimization must not fire inside it, the value is observed
          // verbatim ("--x:0.5" became "--x:.5"; audit P4-d, an
          // owner-approved behavior fix).  Mirrors Phase 2's in_custom
          // (including custom_url shielding) so every phase agrees on
          // where the value ends; strings stay shielded by the string
          // states.
          if (custom_url) {
            // Inside an unquoted url() token: mirror Phase 1/2's
            // shielding — copy verbatim (no depth tracking, no string
            // states) until the closing ')'.
            if (c == ')') custom_url = false;
            r += c;
            ++i;
            continue;
          }
          if (c == '\'') {
            r += c;
            state = State::kInSingleStr;
            ++i;
            continue;
          }
          if (c == '"') {
            r += c;
            state = State::kInDoubleStr;
            ++i;
            continue;
          }
          if (c == '(' && r.size() >= 3 && CiMatch(r[r.size() - 3], 'u') &&
              CiMatch(r[r.size() - 2], 'r') && CiMatch(r[r.size() - 1], 'l')) {
            // url( inside a custom value: unquoted content enters the
            // shielded mode above, mirroring Phases 1/2 (which likewise
            // keep the whitespace after '(' verbatim here).
            size_t j = i + 1;
            while (j < input.size() && IsCssWhitespace(input[j])) ++j;
            if (j >= input.size() || (input[j] != '\'' && input[j] != '"')) {
              custom_url = true;
              r += c;
              ++i;
              continue;
            }
          }
          if (c == '{' || c == '(' || c == '[') {
            ++custom_bd;
          } else if (custom_bd > 0) {
            if (c == '}' || c == ')' || c == ']') --custom_bd;
          } else if (c == ';' || c == '}') {
            in_custom = false;
          }
          if (in_custom) {
            r += c;
            ++i;
            continue;
          }
          // Fall through: the terminator is handled by the normal rules.
        }
        if (c == '\'') {
          r += c;
          state = State::kInSingleStr;
          ++i;
          continue;
        }
        if (c == '"') {
          r += c;
          state = State::kInDoubleStr;
          ++i;
          continue;
        }
        // Enter unquoted url() state — skip decimal optimization inside URLs.
        if (c == '(' && r.size() >= 3 && CiMatch(r[r.size() - 3], 'u') &&
            CiMatch(r[r.size() - 2], 'r') && CiMatch(r[r.size() - 1], 'l')) {
          r += c;
          ++i;
          // Skip whitespace before the quote test, matching Phases 1-3:
          // url( "x" ) is a quoted url handled by the string states, so
          // a ')' inside the string must not close a phantom url (audit
          // P4-c).  In-pipeline only custom values can still carry that
          // whitespace — and those never reach this entry (in_custom
          // above) — so the skip cannot drop served bytes.
          while (i < input.size() && IsCssWhitespace(input[i])) ++i;
          // If the URL content starts with a quote, the string states will
          // handle it — no need to enter kInUrl.
          if (i < input.size() && (input[i] == '\'' || input[i] == '"'))
            continue;
          state = State::kInUrl;
          continue;
        }
        if (c == ':' && AtCustomPropertyColon(r)) {
          // Entering a custom-property value (see AtCustomPropertyColon).
          r += c;
          in_custom = true;
          custom_bd = 0;
          ++i;
          continue;
        }
        // Drop a leading '0' immediately before '.' only at a number-
        // token start (#1163).  Inside an identifier "0.5" is ident
        // content, not a number — ".a0.5" became ".a.5" (valid-input
        // corruption).  So the strip is blocked when the '0' continues
        // an identifier, i.e. when the preceding output char is:
        // - an ident char (letter, digit, '_' or non-ASCII): ".a0.5",
        //   "#id0.5", and the pre-existing "10.5" digit case;
        // - a '-' that itself follows ident content: "a-0.5" is a
        //   dashed ident — while a bare '-' stays a number sign, so
        //   "-0.5" -> "-.5" keeps working;
        // - escape content (#1175): the second char of a `\<char>`
        //   pair escape (".a\'0.5", ".a\\-0.5" — an escaped char is
        //   ident content, and a '-' after it continues the ident),
        //   or the last char of a `\<hex>{1,6}<ws>?` hex escape —
        //   including its consumed whitespace terminator (".a\35 0.5":
        //   the space terminates "\35", the ident is "a50").  See
        //   IsIdentContentAt; the '-' lookback uses it too, so escape
        //   content before the dash (".a\26 -0.5") blocks the strip.
        // The strip is ALSO blocked when the preceding output char is
        // '.': a '0' between two dots never starts a valid number
        // token, so firing there rewrites bytes the minifier cannot
        // justify — ".0." became ".." (run 30913256122, 15 artifacts;
        // #1238 triage).  Invalid-input-only, but byte-stability on
        // garbage is what keeps the token oracle's number channel
        // intact (the #1163 philosophy, one guard clause over).
        // Examples: "0.5" -> ".5", "-0.5" -> "-.5", "10.5" and
        // ".a0.5" unchanged, ".0." unchanged.
        if (c == '0' && i + 1 < input.size() && input[i + 1] == '.') {
          // r.back() is always input[i-1] here: a stripped '0' would
          // make input[i] == '.', contradicting c == '0'.
          bool preceded_by_ident =
              !r.empty() &&
              (IsIdentContentAt(r, r.size() - 1) ||
               (r.back() == '-' && r.size() >= 2 &&
                (r[r.size() - 2] == '-' || IsIdentContentAt(r, r.size() - 2))));
          if (!preceded_by_ident && (r.empty() || r.back() != '.')) {
            // Skip the '0'; the next iteration will emit the '.'.
            ++i;
            continue;
          }
        }
        r += c;
        ++i;
        break;
      case State::kInSingleStr:
        r += c;
        if (c == '\\' && i + 1 < input.size()) {
          r += input[i + 1];
          i += 2;
          continue;
        }
        if (c == '\'') state = State::kNormal;
        ++i;
        break;
      case State::kInDoubleStr:
        r += c;
        if (c == '\\' && i + 1 < input.size()) {
          r += input[i + 1];
          i += 2;
          continue;
        }
        if (c == '"') state = State::kNormal;
        ++i;
        break;
      case State::kInUrl:
        r += c;
        if (c == ')') {
          state = State::kNormal;
          ++i;
          continue;
        }
        if (c == '\\' && i + 1 < input.size()) {
          r += input[i + 1];
          i += 2;
          continue;
        }
        ++i;
        break;
      default:
        r += c;
        ++i;
        break;
    }
  }
  return r;
}

// Phase 5: Collapse consecutive longhand properties into shorthand equivalents.
// Targets: padding, margin, border-{top,right,bottom,left}, overflow.

struct Decl5 {
  std::string_view prop;
  std::string_view value;
  std::string_view raw;
};

// The value class each longhand of a family accepts, for the collapse's
// value gate (#1378).  PER LONGHAND, not per family: "border-top-width:solid"
// is as invalid as "border-top-style:1px", and a gate that only asked whether
// a value is somewhere in the family's union would pass both.
enum class ValueClass : uint8_t {
  kUnused,      // padding of the fixed-size table; never consulted
  kLength,      // <length> | <percentage>            (padding)
  kLengthAuto,  // <length> | <percentage> | auto     (margin)
  kLineWidth,   // <length> | thin | medium | thick   (border-*-width)
  kLineStyle,   // <line-style> keyword               (border-*-style)
  kColor,       // <color>                            (border-*-color)
  kOverflow,    // <overflow> keyword                 (overflow-x/y)
};

struct ShorthandFamily {
  std::string_view shorthand;
  std::string_view longhands[4];
  int count;
  enum Kind : uint8_t { kBoxModel, kBorderSide, kOverflow } kind;
  ValueClass classes[4];
};

static constexpr ShorthandFamily kFamilies[] = {
    {"padding",
     {"padding-top", "padding-right", "padding-bottom", "padding-left"},
     4,
     ShorthandFamily::kBoxModel,
     {ValueClass::kLength, ValueClass::kLength, ValueClass::kLength,
      ValueClass::kLength}},
    {"margin",
     {"margin-top", "margin-right", "margin-bottom", "margin-left"},
     4,
     ShorthandFamily::kBoxModel,
     {ValueClass::kLengthAuto, ValueClass::kLengthAuto, ValueClass::kLengthAuto,
      ValueClass::kLengthAuto}},
    {"border-top",
     {"border-top-width", "border-top-style", "border-top-color", ""},
     3,
     ShorthandFamily::kBorderSide,
     {ValueClass::kLineWidth, ValueClass::kLineStyle, ValueClass::kColor,
      ValueClass::kUnused}},
    {"border-right",
     {"border-right-width", "border-right-style", "border-right-color", ""},
     3,
     ShorthandFamily::kBorderSide,
     {ValueClass::kLineWidth, ValueClass::kLineStyle, ValueClass::kColor,
      ValueClass::kUnused}},
    {"border-bottom",
     {"border-bottom-width", "border-bottom-style", "border-bottom-color", ""},
     3,
     ShorthandFamily::kBorderSide,
     {ValueClass::kLineWidth, ValueClass::kLineStyle, ValueClass::kColor,
      ValueClass::kUnused}},
    {"border-left",
     {"border-left-width", "border-left-style", "border-left-color", ""},
     3,
     ShorthandFamily::kBorderSide,
     {ValueClass::kLineWidth, ValueClass::kLineStyle, ValueClass::kColor,
      ValueClass::kUnused}},
    {"overflow",
     {"overflow-x", "overflow-y", "", ""},
     2,
     ShorthandFamily::kOverflow,
     {ValueClass::kOverflow, ValueClass::kOverflow, ValueClass::kUnused,
      ValueClass::kUnused}},
};

// ---------------------------------------------------------------------------
// Value gate for the shorthand collapse (#1378).
//
// Per CSS Cascading, an invalid LONGHAND value drops that one declaration; an
// invalid SHORTHAND value drops every longhand the shorthand would have set.
// So collapsing a family that contains one invalid member turns "one bad
// declaration ignored" into "the whole family lost" -- the minifier widening a
// stylesheet's own typo.  The guards above it are all minifier hazards
// (var()/!important/CSS-wide keywords, empty values, trimmable edges,
// unbalanced nesting, operator edges); none of them looks at whether the value
// is a legal value for the property at all.
//
// The gate is a conservative WHITELIST, not a grammar: every member value must
// be a plain number/dimension with a known unit, a percentage, or a keyword
// this family's longhand actually accepts.  Anything it cannot positively
// recognise refuses the collapse, and refusing is always sound -- the family
// is emitted as longhands, which is what the input already said.  In
// particular it does not attempt to validate function values (calc(), min(),
// max(), env(), ...): "calc(1px + )" is balanced and looks structurally fine
// but is invalid, and validating math expressions properly is a different
// problem, so a function value simply refuses the collapse.

bool EqualsCI(std::string_view a, std::string_view b) {
  if (a.size() != b.size()) return false;
  for (size_t i = 0; i < a.size(); ++i) {
    if (!CiMatch(a[i], b[i])) return false;
  }
  return true;
}

template <size_t N>
bool InSetCI(std::string_view v, const std::string_view (&set)[N]) {
  for (size_t i = 0; i < N; ++i) {
    if (EqualsCI(v, set[i])) return true;
  }
  return false;
}

// Every CSS distance unit, so a typo'd one ("4pxx") is not mistaken for a
// dimension.  '%' is handled separately -- it is legal for padding/margin but
// not for border-width.
constexpr std::string_view kLengthUnits[] = {
    "em",  "rem",   "ex",    "rex", "ch",    "rch",   "ic",  "ric",   "lh",
    "rlh", "cap",   "rcap",  "vw",  "vh",    "vi",    "vb",  "vmin",  "vmax",
    "svw", "svh",   "svi",   "svb", "svmin", "svmax", "lvw", "lvh",   "lvi",
    "lvb", "lvmin", "lvmax", "dvw", "dvh",   "dvi",   "dvb", "dvmin", "dvmax",
    "cqw", "cqh",   "cqi",   "cqb", "cqmin", "cqmax", "cm",  "mm",    "q",
    "in",  "pt",    "pc",    "px"};

constexpr std::string_view kLineWidthKeywords[] = {"thin", "medium", "thick"};

constexpr std::string_view kLineStyleKeywords[] = {
    "none",   "hidden", "dotted", "dashed", "solid",
    "double", "groove", "ridge",  "inset",  "outset"};

// The overflow keyword set.  "overlay" is deliberately absent: it is a legacy
// alias no longer in the specification, and admitting a keyword a browser may
// reject is exactly the direction this gate must not err in.
constexpr std::string_view kOverflowKeywords[] = {"visible", "hidden", "clip",
                                                  "scroll", "auto"};

// The CSS named colours (Color Level 4), plus the two keywords that are always
// legal in a colour position.  Functional colours (rgb(), hsl(), color-mix(),
// ...) are not whitelisted -- see the note above on function values.
constexpr std::string_view kColorKeywords[] = {"currentcolor",
                                               "transparent",
                                               "aliceblue",
                                               "antiquewhite",
                                               "aqua",
                                               "aquamarine",
                                               "azure",
                                               "beige",
                                               "bisque",
                                               "black",
                                               "blanchedalmond",
                                               "blue",
                                               "blueviolet",
                                               "brown",
                                               "burlywood",
                                               "cadetblue",
                                               "chartreuse",
                                               "chocolate",
                                               "coral",
                                               "cornflowerblue",
                                               "cornsilk",
                                               "crimson",
                                               "cyan",
                                               "darkblue",
                                               "darkcyan",
                                               "darkgoldenrod",
                                               "darkgray",
                                               "darkgreen",
                                               "darkgrey",
                                               "darkkhaki",
                                               "darkmagenta",
                                               "darkolivegreen",
                                               "darkorange",
                                               "darkorchid",
                                               "darkred",
                                               "darksalmon",
                                               "darkseagreen",
                                               "darkslateblue",
                                               "darkslategray",
                                               "darkslategrey",
                                               "darkturquoise",
                                               "darkviolet",
                                               "deeppink",
                                               "deepskyblue",
                                               "dimgray",
                                               "dimgrey",
                                               "dodgerblue",
                                               "firebrick",
                                               "floralwhite",
                                               "forestgreen",
                                               "fuchsia",
                                               "gainsboro",
                                               "ghostwhite",
                                               "gold",
                                               "goldenrod",
                                               "gray",
                                               "green",
                                               "greenyellow",
                                               "grey",
                                               "honeydew",
                                               "hotpink",
                                               "indianred",
                                               "indigo",
                                               "ivory",
                                               "khaki",
                                               "lavender",
                                               "lavenderblush",
                                               "lawngreen",
                                               "lemonchiffon",
                                               "lightblue",
                                               "lightcoral",
                                               "lightcyan",
                                               "lightgoldenrodyellow",
                                               "lightgray",
                                               "lightgreen",
                                               "lightgrey",
                                               "lightpink",
                                               "lightsalmon",
                                               "lightseagreen",
                                               "lightskyblue",
                                               "lightslategray",
                                               "lightslategrey",
                                               "lightsteelblue",
                                               "lightyellow",
                                               "lime",
                                               "limegreen",
                                               "linen",
                                               "magenta",
                                               "maroon",
                                               "mediumaquamarine",
                                               "mediumblue",
                                               "mediumorchid",
                                               "mediumpurple",
                                               "mediumseagreen",
                                               "mediumslateblue",
                                               "mediumspringgreen",
                                               "mediumturquoise",
                                               "mediumvioletred",
                                               "midnightblue",
                                               "mintcream",
                                               "mistyrose",
                                               "moccasin",
                                               "navajowhite",
                                               "navy",
                                               "oldlace",
                                               "olive",
                                               "olivedrab",
                                               "orange",
                                               "orangered",
                                               "orchid",
                                               "palegoldenrod",
                                               "palegreen",
                                               "paleturquoise",
                                               "palevioletred",
                                               "papayawhip",
                                               "peachpuff",
                                               "peru",
                                               "pink",
                                               "plum",
                                               "powderblue",
                                               "purple",
                                               "rebeccapurple",
                                               "red",
                                               "rosybrown",
                                               "royalblue",
                                               "saddlebrown",
                                               "salmon",
                                               "sandybrown",
                                               "seagreen",
                                               "seashell",
                                               "sienna",
                                               "silver",
                                               "skyblue",
                                               "slateblue",
                                               "slategray",
                                               "slategrey",
                                               "snow",
                                               "springgreen",
                                               "steelblue",
                                               "tan",
                                               "teal",
                                               "thistle",
                                               "tomato",
                                               "turquoise",
                                               "violet",
                                               "wheat",
                                               "white",
                                               "whitesmoke",
                                               "yellow",
                                               "yellowgreen"};

// Scans a CSS <number> at the start of v: optional sign, digits with at most
// one '.', at least one digit.  Scientific notation is deliberately not
// accepted -- it is legal CSS but rare in these families, and refusing is the
// safe direction.  Returns the number of bytes consumed (0 = not a number),
// and reports whether every digit was '0' and whether a '-' sign was present.
size_t ScanCssNumber(std::string_view v, bool* is_zero, bool* is_signed_neg) {
  size_t i = 0;
  *is_signed_neg = false;
  if (i < v.size() && (v[i] == '+' || v[i] == '-')) {
    *is_signed_neg = (v[i] == '-');
    ++i;
  }
  size_t digits = 0;
  bool all_zero = true;
  while (i < v.size() && v[i] >= '0' && v[i] <= '9') {
    if (v[i] != '0') all_zero = false;
    ++i;
    ++digits;
  }
  if (i < v.size() && v[i] == '.') {
    ++i;
    while (i < v.size() && v[i] >= '0' && v[i] <= '9') {
      if (v[i] != '0') all_zero = false;
      ++i;
      ++digits;
    }
  }
  if (digits == 0) return 0;
  *is_zero = all_zero;
  return i;
}

// <length> (and, when allowed, <percentage>).  A unitless number is a length
// only at zero -- "padding-top:5" is invalid CSS, and collapsing it would take
// the rest of the family down with it.
//
// |allow_negative| is the [0,inf] half of the property grammar, and it is not
// cosmetic: padding-* and border-*-width both refuse a negative length, so
// "padding-top:-1px" is an invalid longhand that costs one declaration and an
// invalid shorthand that costs four.  margin-* genuinely takes negatives and
// shares this helper.  A '-' sign is refused outright when negatives are not
// allowed, including "-0px": legal by arithmetic, vanishingly rare in practice,
// and refusing is the sound direction.
bool IsLengthValue(std::string_view v, bool allow_percent,
                   bool allow_negative) {
  bool is_zero = false;
  bool is_signed_neg = false;
  size_t n = ScanCssNumber(v, &is_zero, &is_signed_neg);
  if (n == 0) return false;
  if (is_signed_neg && !allow_negative) return false;
  std::string_view unit = v.substr(n);
  if (unit.empty()) return is_zero;
  if (unit == "%") return allow_percent;
  return InSetCI(unit, kLengthUnits);
}

// A bounded functional-colour form: rgb()/rgba()/hsl()/hsla() whose argument
// list contains nothing but digits and the separators/units this gate can
// account for -- commas, dots, percent signs and spaces -- and whose single
// paren pair closes at the very end.  Deliberately narrow, and deliberately
// NOT a generic "balanced parens" rule: `calc(1px + )` is balanced and ends
// in ')' too, and admitting it would re-admit #1378's own red case.  Nothing
// whose contents this cannot read is admitted -- no `/` alpha syntax, no
// <angle> hue, no color-mix()/oklch()/light-dark(), no nested function.
bool IsFunctionalColorValue(std::string_view v) {
  constexpr std::string_view kColorFunctions[] = {"rgba(", "rgb(", "hsla(",
                                                  "hsl("};
  std::string_view body;
  bool is_hsl = false;
  bool matched = false;
  for (std::string_view fn : kColorFunctions) {
    if (v.size() > fn.size() && EqualsCI(v.substr(0, fn.size()), fn)) {
      body = v.substr(fn.size());
      is_hsl = (fn[0] == 'h');
      matched = true;
      break;
    }
  }
  if (!matched || body.empty() || body.back() != ')') return false;
  body.remove_suffix(1);

  // Every remaining byte must be one this gate can account for.  A ')' or '('
  // in the body is rejected outright, which is what makes the single trailing
  // ')' the whole of the nesting and keeps the check bounded.
  for (char c : body) {
    const bool digit = c >= '0' && c <= '9';
    if (!digit && c != ',' && c != '.' && c != '%' && c != ' ') return false;
  }

  // A well-formed comma-separated argument list, and nothing else.  Reading
  // the bytes is not enough: "rgb(0)", "rgb(0,0)", "rgb(0,0,0,)",
  // "rgb(1.2.3,0,0)" and "hsl(50,50,50)" are all made only of accountable
  // bytes and are all invalid CSS -- and an invalid value inside a collapsed
  // shorthand is the very blast shape this gate exists to prevent, so it must
  // not be re-opened inside the class the gate re-admits.  The modern
  // space-separated form is deliberately NOT accepted: it is a separate
  // grammar, and refusing it only costs a collapse.
  std::string_view comps[5];
  size_t n = 0;
  size_t start = 0;
  for (size_t i = 0; i <= body.size(); ++i) {
    if (i == body.size() || body[i] == ',') {
      if (n >= 5) return false;
      comps[n++] = body.substr(start, i - start);
      start = i + 1;
    }
  }
  // 3 components (rgb/hsl) or 4 (rgba/hsla): commas == 2 or 3.
  if (n != 3 && n != 4) return false;

  bool all_percent = true;
  bool none_percent = true;
  for (size_t c = 0; c < n; ++c) {
    std::string_view comp = comps[c];
    // Surrounding spaces are formatting; the component itself must not be.
    while (!comp.empty() && comp.front() == ' ') comp.remove_prefix(1);
    while (!comp.empty() && comp.back() == ' ') comp.remove_suffix(1);
    if (comp.empty()) return false;

    const bool percent = comp.back() == '%';
    if (percent) comp.remove_suffix(1);
    if (comp.empty()) return false;

    // A '%' is a unit and may only sit at the end; a number carries at most
    // one '.'; and there has to be a digit.
    size_t dots = 0;
    bool has_digit = false;
    for (char ch : comp) {
      if (ch >= '0' && ch <= '9') {
        has_digit = true;
      } else if (ch == '.') {
        if (++dots > 1) return false;
      } else {
        return false;  // an interior '%' or a space
      }
    }
    if (!has_digit) return false;

    if (c < 3) {
      all_percent = all_percent && percent;
      none_percent = none_percent && !percent;
    }
  }

  if (is_hsl) {
    // hsl()/hsla(): the hue is a number, saturation and lightness are
    // percentages.  "hsl(50,50,50)" is the classic author error.
    if (comps[0].find('%') != std::string_view::npos) return false;
    if (comps[1].find('%') == std::string_view::npos) return false;
    if (comps[2].find('%') == std::string_view::npos) return false;
  } else if (!all_percent && !none_percent) {
    // rgb()/rgba() legacy syntax takes three numbers or three percentages,
    // never a mixture.
    return false;
  }
  return true;
}

bool IsHexColorValue(std::string_view v) {
  if (v.empty() || v[0] != '#') return false;
  size_t n = v.size() - 1;
  if (n != 3 && n != 4 && n != 6 && n != 8) return false;
  for (size_t i = 1; i < v.size(); ++i) {
    if (!IsHexDigit(v[i])) return false;
  }
  return true;
}

// True when v is a value the named longhand actually accepts, by the
// conservative whitelist described above.
bool IsFamilyLegalValue(std::string_view v, ValueClass cls) {
  switch (cls) {
    case ValueClass::kLength:
      // padding-*: <length-percentage [0,inf]> -- no keywords, no negatives.
      return IsLengthValue(v, /*allow_percent=*/true, /*allow_negative=*/false);
    case ValueClass::kLengthAuto:
      // margin-*: <length-percentage> | auto -- negatives are legal here.
      return EqualsCI(v, "auto") ||
             IsLengthValue(v, /*allow_percent=*/true, /*allow_negative=*/true);
    case ValueClass::kLineWidth:
      // border-*-width: <line-width> -- no percentages, no negatives.
      return InSetCI(v, kLineWidthKeywords) ||
             IsLengthValue(v, /*allow_percent=*/false,
                           /*allow_negative=*/false);
    case ValueClass::kLineStyle:
      return InSetCI(v, kLineStyleKeywords);
    case ValueClass::kColor:
      return IsHexColorValue(v) || InSetCI(v, kColorKeywords) ||
             IsFunctionalColorValue(v);
    case ValueClass::kOverflow:
      return InSetCI(v, kOverflowKeywords);
    case ValueClass::kUnused:
      break;
  }
  return false;
}

bool HasUnsafeValue(std::string_view v) {
  if (v.find("var(") != std::string_view::npos) return true;
  if (v.find("!important") != std::string_view::npos) return true;
  return v == "inherit" || v == "initial" || v == "unset" || v == "revert" ||
         v == "revert-layer";
}

// True if an unquoted url() token starts at position i of input
// (case-insensitive "url" immediately followed by '(', matching the
// other phases' suffix rule) — ';', '{' and '}' are legal unquoted-url
// code points, so Phase 5's scanners must not treat them as
// declaration or block boundaries (#1158).  On a match, *end is set
// past the closing ')' (or end of input) and the caller copies the
// range verbatim; escape pairs keep an escaped ')' from closing early.
// Quoted url("...")/url('...') returns false — the string handling
// covers that content, as in the other phases.
bool ScanUnquotedUrl(std::string_view input, size_t i, size_t* end) {
  if (i + 3 >= input.size() || !CiMatch(input[i], 'u') ||
      !CiMatch(input[i + 1], 'r') || !CiMatch(input[i + 2], 'l') ||
      input[i + 3] != '(') {
    return false;
  }
  size_t j = i + 4;
  while (j < input.size() && IsCssWhitespace(input[j])) ++j;
  if (j < input.size() && (input[j] == '\'' || input[j] == '"')) {
    return false;
  }
  j = i + 4;
  while (j < input.size()) {
    if (input[j] == '\\' && j + 1 < input.size()) {
      j += 2;
      continue;
    }
    if (input[j] == ')') {
      ++j;
      break;
    }
    ++j;
  }
  *end = j;
  return true;
}

// True if v is nesting-stable under reordering: parens balance and
// square brackets balance, each as its own depth-only counter (never
// negative, zero at the end), and no '{' or '}' occurs at all — under
// the same string/escape/url-aware scan the other scanners use.  The
// phases track these counters in different contexts (Phase 5's block
// matcher counts parens only; Phase 3's custom-value depth and
// paren/bracket depth count all of '([{'/')]}'), so each must be
// reorder-invariant on its own; a brace is never safe because its
// visibility to the block matcher depends on the paren depth the
// reordered arrangement happens to place before it.  Valid
// collapse-family values always pass — parens only occur inside
// functions, brackets and braces never occur, and Phases 1-2 never
// unbalance any of them — so a failure marks invalid input.
// Collapsing those is pass-unstable: the rejoin reorders longhand
// values, and a relocated bracket shifts the next pass's depth
// tracking, which moves brace/block boundaries and custom-value
// extents (and with them Phase 3's trim context and Phase 5's reparse
// extent) between passes (#1170's residual classes — nightly artifacts
// carry values like "/(pZ" or "-!(U -&y4"; the fork-mode sweeps carry
// mismatched '['/']', brace runs like "(}}})", and paren-shielded
// braces like "\x94{(])" relocating a custom value's opaque region or
// a block edge).
bool HasBalancedNesting(std::string_view v) {
  int pd = 0, bd = 0;
  size_t i = 0;
  while (i < v.size()) {
    char c = v[i];
    if (c == '\\' && i + 1 < v.size()) {
      i += 2;
      continue;
    }
    if (c == '\'' || c == '"') {
      char q = c;
      ++i;
      while (i < v.size()) {
        if (v[i] == '\\' && i + 1 < v.size()) {
          i += 2;
          continue;
        }
        if (v[i] == q) {
          ++i;
          break;
        }
        ++i;
      }
      continue;
    }
    size_t url_end;
    if (ScanUnquotedUrl(v, i, &url_end)) {
      // Unquoted url(): parens inside are url code points (#1158).
      i = url_end;
      continue;
    }
    if (c == '{' || c == '}') return false;
    if (c == '(') {
      ++pd;
    } else if (c == ')') {
      if (pd == 0) return false;
      --pd;
    } else if (c == '[') {
      ++bd;
    } else if (c == ']') {
      if (bd == 0) return false;
      --bd;
    }
    ++i;
  }
  return pd == 0 && bd == 0;
}

// Nesting of simple blocks opened by '(' or '[', shared by the
// three Phase 5 scanners (ParseBlockDecls, the top-level scan, the
// brace matcher) so they agree on where a rule's block and a
// declaration's end are.  CSS Syntax 3 consumes a simple block for
// every opener up to its matching closer, so inside an open (...) or
// [...]:
//   * a ';' is value content, not a declaration boundary
//     ("a{--x:(a;;b)}" was served "--x:(a;b)" — the empty segment
//     between the ';' parsed as an empty declaration and the rejoin
//     dropped it; a custom property reads its value back verbatim);
//   * a '{' opens a nested block, never a rule's block, and the ')' or
//     '}' met inside THAT block is content ("a{--x:(a{b)c;;d}e)}" had
//     the ')' close the paren and the ';;' collapse; "a{--x:[a}e{c:d;;}"
//     had the '}' inside the unclosed '[' close a's block).
// ')' and ']' are interchangeable (the shared-counter semantics Phase
// 3's pd has as well): a mismatched closer makes a declaration the
// browser drops anyway, and what matters is that the phases agree.
//
// Hot path: the three scanners call Feed once per character, and on a
// real sheet nearly every character arrives at depth 0 and is neither
// an opener nor a closer.  That case is answered in Feed, forced
// inline; the std::string stack work lives in FeedSlow, forced out of
// line (measured: with the whole thing out of line, large
// sheets minified 10-13% slower; left to the optimizer, clang folded
// the slow path INTO Feed and still called it per character, +5-8%).
#if defined(__GNUC__) || defined(__clang__)
#define CSS_ALWAYS_INLINE inline __attribute__((always_inline))
#define CSS_NOINLINE __attribute__((noinline))
#elif defined(_MSC_VER)
#define CSS_ALWAYS_INLINE __forceinline
#define CSS_NOINLINE __declspec(noinline)
#else
#define CSS_ALWAYS_INLINE inline
#define CSS_NOINLINE
#endif

class ValueNest {
 public:
  bool empty() const { return s_.empty(); }
  void clear() { s_.clear(); }
  // Feeds one char outside strings/url()/escapes.  Returns true when
  // the char is nesting bookkeeping or content inside an open block
  // (the caller copies it and moves on), false when it is structural
  // at depth 0 (';', ':', '{', '}' and everything else).
  CSS_ALWAYS_INLINE bool Feed(char c) {
    if (s_.empty() && c != '(' && c != '[' && c != ')' && c != ']') {
      return false;
    }
    return FeedSlow(c);
  }

 private:
  CSS_NOINLINE bool FeedSlow(char c);
  std::string s_;
};

bool ValueNest::FeedSlow(char c) {
  if (c == '(' || c == '[') {
    s_.push_back('(');
    return true;
  }
  if (c == ')' || c == ']') {
    if (!s_.empty() && s_.back() == '(') s_.pop_back();
    return true;  // A stray closer at depth 0 is content, as before.
  }
  // Inside an open block: everything is content; a '{' nests and the
  // '}' that matches it un-nests.
  if (c == '{') {
    s_.push_back('{');
  } else if (c == '}' && s_.back() == '{') {
    s_.pop_back();
  }
  return true;
}

std::vector<Decl5> ParseBlockDecls(std::string_view block) {
  std::vector<Decl5> decls;
  size_t i = 0;
  while (i < block.size()) {
    size_t start = i;
    size_t colon_pos = std::string_view::npos;
    bool in_sq = false, in_dq = false;
    // A ';' inside (...) or [...] is value content, not a
    // declaration boundary — CSS Syntax 3 splits declarations only at
    // TOP-LEVEL semicolons (see ValueNest).  The first ':' is likewise
    // only a name/value colon at depth 0.
    ValueNest nest;
    while (i < block.size()) {
      char c = block[i];
      if (in_sq) {
        if (c == '\\' && i + 1 < block.size()) {
          i += 2;
          continue;
        }
        if (c == '\'') in_sq = false;
      } else if (in_dq) {
        if (c == '\\' && i + 1 < block.size()) {
          i += 2;
          continue;
        }
        if (c == '"') in_dq = false;
      } else {
        size_t url_end;
        if (ScanUnquotedUrl(block, i, &url_end)) {
          // Unquoted url(): no declaration boundary inside (#1158).
          i = url_end;
          continue;
        }
        // Escape pair outside strings: the escaped char is value
        // content, never a terminator or a name/value colon — "\;"
        // splits nothing (#1164), mirroring every other scanner in
        // the file.
        if (c == '\\' && i + 1 < block.size()) {
          i += 2;
          continue;
        }
        if (c == '\'') {
          in_sq = true;
        } else if (c == '"') {
          in_dq = true;
        } else if (nest.Feed(c)) {
          // Nesting bookkeeping or content inside (...) / [...].
        } else if (c == ':' && colon_pos == std::string_view::npos) {
          colon_pos = i;
        } else if (c == ';') {
          break;
        }
      }
      ++i;
    }
    if (colon_pos != std::string_view::npos && colon_pos > start) {
      decls.push_back({block.substr(start, colon_pos - start),
                       block.substr(colon_pos + 1, i - colon_pos - 1),
                       block.substr(start, i - start)});
    } else if (i > start) {
      decls.push_back({"", "", block.substr(start, i - start)});
    }
    if (i < block.size() && block[i] == ';') ++i;
  }
  return decls;
}

void FormatBoxModelValues(const std::string_view values[4], std::string& out) {
  if (values[0] == values[1] && values[1] == values[2] &&
      values[2] == values[3]) {
    out += values[0];
  } else if (values[0] == values[2] && values[1] == values[3]) {
    out += values[0];
    out += ' ';
    out += values[1];
  } else if (values[1] == values[3]) {
    out += values[0];
    out += ' ';
    out += values[1];
    out += ' ';
    out += values[2];
  } else {
    out += values[0];
    out += ' ';
    out += values[1];
    out += ' ';
    out += values[2];
    out += ' ';
    out += values[3];
  }
}

void FormatBorderSideValues(const std::string_view values[4],
                            std::string& out) {
  out += values[0];
  out += ' ';
  out += values[1];
  out += ' ';
  out += values[2];
}

void FormatOverflowValues(const std::string_view values[4], std::string& out) {
  out += values[0];
  if (values[0] != values[1]) {
    out += ' ';
    out += values[1];
  }
}

int TryCollapseFamily(const Decl5* decls, size_t remaining,
                      const ShorthandFamily& family, std::string& out) {
  if (static_cast<int>(remaining) < family.count) return 0;
  std::string_view values[4] = {};
  bool matched[4] = {};

  for (int d = 0; d < family.count; ++d) {
    if (decls[d].prop.empty()) return 0;
    bool found = false;
    for (int l = 0; l < family.count; ++l) {
      if (!matched[l] && decls[d].prop == family.longhands[l]) {
        values[l] = decls[d].value;
        matched[l] = true;
        found = true;
        break;
      }
    }
    if (!found) return 0;
  }

  for (int l = 0; l < family.count; ++l) {
    // An empty longhand value is never a safe collapse: the separator
    // formatting would emit a dangling space ("overflow:: ") no phase
    // trims, converging one pass late (#1162).
    if (values[l].empty()) return 0;
    if (HasUnsafeValue(values[l])) return 0;
    // Nor is a value whose edge chars make an emitted separator space
    // Phase-2-trimmable ("overflow:v: w" -> pass 2 "overflow:v:w" —
    // #1162's broader face, found by the post-fix fuzz sweep).  '!' and
    // value-edge whitespace never legitimately appear (Phases 1-2 trim
    // the latter); the one deliberate casualty is SIGNED lengths —
    // "+1px" is valid and starts with '+', but collapsing those is
    // itself one-pass-late (pass 2 glues "+1px +2px" into
    // "+1px+2px"), so refusing is the correct direction there too.
    if (CanRemoveSpaceAfter(values[l].back()) ||
        CanRemoveSpaceBefore(values[l].front()) || values[l].front() == '!' ||
        IsCssWhitespace(values[l].front()) ||
        IsCssWhitespace(values[l].back())) {
      return 0;
    }
    // Nor is a value with unbalanced parens/brackets: those only occur
    // in invalid input, and the collapse reorders values, so a
    // relocated bracket shifts the next pass's block boundaries and
    // custom-value extent between passes (#1170 — same
    // conservative-refusal pattern as the #1162 guards).
    if (!HasBalancedNesting(values[l])) return 0;
    // Nor one whose edge char is a calc-mode operator: Phase 2 strips
    // whitespace adjacent to '*'/'/' inside math mode, and an
    // unterminated calc( earlier in the sheet can leave math mode on,
    // so an emitted separator space next to such an edge is
    // Phase-2-trimmable next pass (#1170's operator-space face; valid
    // longhand values never begin or end with an operator).
    if (values[l].front() == '*' || values[l].front() == '/' ||
        values[l].back() == '*' || values[l].back() == '/') {
      return 0;
    }
    // Nor a value this longhand does not actually accept.  An invalid
    // longhand costs one declaration; an invalid shorthand costs the whole
    // family, so the collapse may only fire on values the gate can positively
    // recognise as legal here (#1378).  Checked per longhand, so a value that
    // is legal for a SIBLING (border-*-style:1px, border-*-width:solid) is
    // refused too.
    if (!IsFamilyLegalValue(values[l], family.classes[l])) return 0;
  }

  if (!out.empty()) out += ';';
  out += family.shorthand;
  out += ':';

  switch (family.kind) {
    case ShorthandFamily::kBoxModel:
      FormatBoxModelValues(values, out);
      break;
    case ShorthandFamily::kBorderSide:
      FormatBorderSideValues(values, out);
      break;
    case ShorthandFamily::kOverflow:
      FormatOverflowValues(values, out);
      break;
  }

  return family.count;
}

std::string CollapseBlock(std::string_view block) {
  auto decls = ParseBlockDecls(block);
  if (decls.empty()) return std::string(block);
  std::string result;
  result.reserve(block.size());
  size_t i = 0;

  while (i < decls.size()) {
    int consumed = 0;
    for (const auto& family : kFamilies) {
      consumed =
          TryCollapseFamily(decls.data() + i, decls.size() - i, family, result);
      if (consumed > 0) break;
    }
    if (consumed == 0) {
      if (!result.empty()) result += ';';
      result += decls[i].raw;
      ++i;
    } else {
      i += consumed;
    }
  }

  return result;
}

std::string Phase5(std::string_view input, int recursion_depth = 0) {
  std::string result;
  result.reserve(input.size());
  size_t i = 0;
  // Declaration-segment tracking (#1167): a '{' that opens a brace group
  // inside a declaration VALUE must be copied verbatim — recursing or
  // collapsing into it rewrites opaque value content (custom-property
  // values are observed verbatim; ordinary values corrupted the same
  // way).  A segment runs from the last top-level ';' (or '}', or a
  // completed block) to the next; it is declaration-like when it has a
  // top-level ':' and everything before that colon is property-name-ish
  // (idents, escapes, whitespace).  At top level (depth 0), where a
  // colon usually means a pseudo-class selector (":root", "a:hover"),
  // the rule is restricted to custom properties (segment starts with
  // "--"); bare top-level declarations are invalid CSS otherwise.
  size_t seg_begin = 0;  // index into result: current segment start
  size_t seg_colon = std::string::npos;  // first top-level ':' in it
  ValueNest seg_nest;  // open (...) / [...] blocks within the segment
  int seg_group = -1;  // cached IsValueGroupBrace verdict (-1 = unknown)

  while (i < input.size()) {
    char c = input[i];

    // Escape pair outside strings: the escaped char is content, never
    // structural — in particular an escaped quote must not open a
    // phantom string (#1164's phantom-string face: the misaligned
    // string would skew block boundaries and CollapseBlock would
    // rewrite what is actually string content).
    if (c == '\\' && i + 1 < input.size()) {
      result += input.substr(i, 2);
      i += 2;
      continue;
    }

    // Handle strings at top level.
    if (c == '\'' || c == '"') {
      char q = c;
      result += q;
      ++i;
      while (i < input.size()) {
        result += input[i];
        if (input[i] == '\\' && i + 1 < input.size()) {
          result += input[i + 1];
          i += 2;
          continue;
        }
        if (input[i] == q) {
          ++i;
          break;
        }
        ++i;
      }
      continue;
    }

    // Unquoted url() at top level: '{' and '}' inside are url code
    // points, not block boundaries (#1158).
    size_t url_end;
    if (ScanUnquotedUrl(input, i, &url_end)) {
      result += input.substr(i, url_end - i);
      i = url_end;
      continue;
    }

    // Segment boundaries and the first top-level ':' (string/url/escape
    // content above never reaches these; the (...) / [...] nesting
    // keeps ';'/'}' inside functions — valid in declaration values —
    // from resetting the segment, and a '{' inside an open paren or
    // bracket from opening a rule block (see ValueNest: in CSS
    // Syntax 3 it opens a simple block nested in the (...) block, never
    // a rule's block — "(;--:{!a;;;y}", a fuzz artifact, had
    // its '{...}' collapsed as a declaration block, which dropped the
    // empty declarations between the ';'s).
    if (seg_nest.Feed(c)) {
      result += c;
      ++i;
      continue;
    }
    if (c == ';' || c == '}') {
      result += c;
      ++i;
      seg_begin = result.size();
      seg_colon = std::string::npos;
      seg_nest.clear();
      seg_group = -1;
      continue;
    }
    if (c == ':' && seg_colon == std::string::npos) {
      seg_colon = result.size();
      result += c;
      ++i;
      continue;
    }

    // Find opening brace.
    if (c != '{') {
      result += c;
      ++i;
      continue;
    }

    // Find matching closing brace.
    size_t j = i + 1;
    int depth = 1;
    // Braces inside (...) or [...] are not block edges (ValueNest,
    // "a{--x:[a}e{c:d;;}" closed a's block at the first '}' with
    // a paren-only count although the '[' block swallows it, and
    // "a{--x:(a{b)c;;d}e)}" had the ')' inside the nested '{' close the
    // paren).
    ValueNest nest;
    bool has_nested = false;
    while (j < input.size() && depth > 0) {
      char bc = input[j];
      // Escape pair outside strings: an escaped quote/brace is
      // content, never a string start or a block edge (#1164).
      if (bc == '\\' && j + 1 < input.size()) {
        j += 2;
        continue;
      }
      if (bc == '\'' || bc == '"') {
        char q = bc;
        ++j;
        while (j < input.size()) {
          if (input[j] == '\\' && j + 1 < input.size()) {
            j += 2;
            continue;
          }
          if (input[j] == q) {
            ++j;
            break;
          }
          ++j;
        }
        continue;
      }
      size_t url_end;
      if (ScanUnquotedUrl(input, j, &url_end)) {
        // Unquoted url(): braces inside are url code points (#1158).
        j = url_end;
        continue;
      }
      // Track paren nesting so a brace inside a function — e.g. a custom
      // property value like foo(a;}) — is not mistaken for the block
      // edge (audit P3-d companion fix; without it the matcher closed
      // the block early and the ';' inside the parens was lost).
      if (nest.Feed(bc)) {
        ++j;
        continue;
      }
      if (bc == '{') {
        ++depth;
        has_nested = true;
      } else if (bc == '}') {
        --depth;
      }
      if (depth > 0) ++j;
    }
    // If no matching '}' found (malformed CSS), emit remainder as-is.
    if (depth > 0) {
      result += input.substr(i);
      break;
    }
    // j points to the matching '}'.
    std::string_view block_content = input.substr(i + 1, j - i - 1);

    // #1167: a brace group inside a declaration value is opaque content,
    // not a nested block — copy it verbatim (the matcher above already
    // bounded it string/url/escape/paren-aware) and keep scanning the
    // same value.  The verdict is cached per segment: the pre-colon run
    // never changes once seg_colon is set (result only appends), and
    // re-scanning it per '{' is quadratic on long values (review).
    if (seg_colon != std::string::npos) {
      if (seg_group < 0) {
        seg_group = IsValueGroupBrace(result, seg_begin, seg_colon,
                                      recursion_depth == 0)
                        ? 1
                        : 0;
      }
      if (seg_group == 1) {
        result += input.substr(i, j - i + 1);
        i = j + 1;
        continue;
      }
    }

    if (has_nested) {
      result += '{';
      if (recursion_depth < 128) {
        result += Phase5(block_content, recursion_depth + 1);
      } else {
        result += block_content;
      }
      result += '}';
    } else {
      result += '{';
      result += CollapseBlock(block_content);
      result += '}';
    }
    i = j + 1;
    // A completed block ends the segment (e.g. selector prelude done).
    seg_begin = result.size();
    seg_colon = std::string::npos;
    seg_nest.clear();
    seg_group = -1;
  }

  return result;
}

}  // namespace

bool MinifyCss(std::string_view input, std::string* output) {
  if (output == nullptr) return false;
  if (input.empty()) {
    output->clear();
    return true;
  }
  PreservedComments preserved;
  std::string r = Phase1(input, &preserved);
  r = Phase2(r);
  r = Phase3(r);
  r = Phase4(r);
  r = Phase5(r);
  // Put the preserved comments' exact bytes back (#582): the phases
  // saw only their one-byte sentinel.  If a phase lost or duplicated a
  // sentinel the pairing cannot be trusted: fail safe (the documented
  // internal-error path, output untouched) rather than emit comments in
  // the wrong places.
  std::string restored;
  if (!RestorePreservedComments(r, preserved, &restored)) return false;
  r = std::move(restored);
  size_t s = 0;
  while (s < r.size() && IsCssWhitespace(r[s])) ++s;
  *output = r.substr(s);
  return true;
}

}  // namespace pagespeed::css
