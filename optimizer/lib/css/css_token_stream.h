// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 We-Amp B.V.
//
// Token-stream equivalence oracle for the CSS minifier fuzz harness
// (css-syntax-3-LITE; harness-local tooling, NOT library code), after
// the GLUE-NORMALIZATION cutover (the glue-normalization spike,
// 2026-08-04, adopted — see css_minify_fuzz.cc's header for the
// nightly history that motivated it).
//
// Motivation: the strict idempotence oracle cannot see behavior-
// preserving corruption, and the first three oracle versions answered
// that with N hand-grown edge rules simulating the minifier's glue
// behavior (escape-glue absorption, chain links, unit liveness, %
// scoping, sign glue, comment spans, candidacy rules, function
// formation, the ws-only-diff adjudication).  The nightly walked the
// interaction surface adjacency-by-adjacency (needs-triage
// 9 -> 3 -> 3 -> 7, all invalid-input-only over-catches in that
// machinery; no real minifier bug since #1241).  This version
// retires the edge-rule dimension entirely:
//
//   Normalize(x) = Phase2(Phase1(x)) — the minifier's OWN
//   whitespace/comment engine, consumed VERBATIM from
//   lib/css/css_phases.h (factored, single source of truth, not a
//   re-implementation; preserved /*! and @license/@preserve
//   comments round-trip through their sentinel, byte for byte).
//   Both sides are canonicalized BEFORE
//   tokenizing, so whatever the glue behavior is — removal sets,
//   escape awareness, calc mode, comment strip, url/string/custom
//   opacity, sign handling — both sides get exactly it, and glued
//   forms are byte-identical whenever the minifier is
//   byte-identical (the pass-1/pass-2 idempotence invariant, which
//   the harness asserts as a counted diagnostic).
//
// Channels are then computed on the glued forms by ExtractGlued
// (~120 lines: the plain LITE tokenizer, NormalizeUrl,
// Phase4Canonical, simple adjacent-unit consumption, and custom-
// property region tracking): escape-absorbed numbers are already
// ident content, chains are already idents, signs are already
// glued, comments are already stripped — none of the retired rules
// is needed.  Phase4Canonical and SameChannelTokens survive because
// they model DOWNSTREAM phases (Phase 4's zero-strip and Phase 5's
// collapse dedup/reorder), which normalization deliberately does
// not cover.
//
// Retired (proven redundant on ~1,130 evidence inputs covering every
// over-catch class — the spike's measurement, re-verified by the
// shadow comparison on this PR): escape-glue absorption, chain
// links and chain state, unit_current/unit liveness, the '%'
// non-consumption rules (incl. the dot-led refinement), sign glue
// look-back and sign detachment, BackOverRemovable, ScanCommentSpans,
// number_joins_name and candidacy-through-glue, function-formation
// special cases, IsEscapeContent glue detection, and R1
// (IsSanctionedWhitespaceOnlyDiff — zero fires post-normalization
// across the whole matrix, so it is not carried as a fallback).
//
// Surviving zero-cost fallbacks for the non-glue disagreement axes:
//   R2  unterminated url( at EOF;
//   R3  garbage-input agreement (UTF-8-aware proxy — the token
//       oracle defers to strict idempotence + the sanitizer on
//       garbage, with the same no-interior-guard stance as v3);
//   the append guard (output extending the input byte-for-byte with
//   non-whitespace appended content flags on printable input).
// The strict idempotence oracle is untouched, as are the url/string
// collapse sanctions (reorder + identical-value dedup,
// SameChannelTokens) and the custom-region byte-for-byte channel.

#ifndef PAGESPEED_LIB_CSS_CSS_TOKEN_STREAM_H_
#define PAGESPEED_LIB_CSS_CSS_TOKEN_STREAM_H_

#include <algorithm>
#include <string>
#include <string_view>
#include <vector>

#include "lib/css/css_phases.h"

namespace pagespeed::css {
namespace token_stream {
namespace {

bool IsWs(char c) {
  return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f';
}

bool IsIdentChar(char c) {
  unsigned char u = static_cast<unsigned char>(c);
  return (u >= 'a' && u <= 'z') || (u >= 'A' && u <= 'Z') ||
         (u >= '0' && u <= '9') || c == '_' || c == '-' || u >= 0x80;
}

bool CiMatch(char c, char lower) { return c == lower || c == (lower - 32); }

enum class Kind : unsigned char {
  kIdent,
  kFunction,
  kString,
  kUrl,
  kNumber,
  kDelim,
  kBrace,  // text is one of { } ( ) [ ]
  kColon,
  kSemicolon,
};

struct Token {
  Kind kind;
  std::string_view text;
};

// css-syntax-3-LITE tokenizer.  Escape pairs (\X) are consumed
// everywhere and count as ident constituents; quoted url("...") is a
// function token plus a string token, unquoted url(...) a single
// url-token (matching the minifier's own tokenization decisions).
class Tokenizer {
 public:
  explicit Tokenizer(std::string_view in) : in_(in) {}

  bool Next(Token* t) {
    for (;;) {
      if (i_ >= in_.size()) return false;
      char c = in_[i_];
      if (IsWs(c)) {
        ++i_;
        continue;
      }
      if (c == '/' && i_ + 1 < in_.size() && in_[i_ + 1] == '*') {
        i_ += 2;
        while (i_ + 1 < in_.size() && !(in_[i_] == '*' && in_[i_ + 1] == '/'))
          ++i_;
        i_ = i_ + 2 <= in_.size() ? i_ + 2 : in_.size();
        continue;
      }
      break;
    }
    const size_t start = i_;
    const char c = in_[i_];
    switch (c) {
      case '{':
      case '}':
      case '(':
      case ')':
      case '[':
      case ']':
        ++i_;
        *t = {Kind::kBrace, in_.substr(start, 1)};
        return true;
      case ':':
        ++i_;
        *t = {Kind::kColon, in_.substr(start, 1)};
        return true;
      case ';':
        ++i_;
        *t = {Kind::kSemicolon, in_.substr(start, 1)};
        return true;
      case '\'':
      case '"': {
        ++i_;
        while (i_ < in_.size()) {
          if (in_[i_] == '\\' && i_ + 1 < in_.size()) {
            i_ += 2;
            continue;
          }
          if (in_[i_] == c) {
            ++i_;
            break;
          }
          ++i_;
        }
        *t = {Kind::kString, in_.substr(start, i_ - start)};
        return true;
      }
      default:
        break;
    }
    if ((c >= '0' && c <= '9') || c == '.' || c == '+' || c == '-') {
      // Number FIRST: css-syntax-3 starts a number here whenever digits
      // follow — idents never start with a digit, and a leading
      // '+'/'-' with digits is a signed number — so this must outrank
      // the ident path (v1 had it backwards: digits were ident
      // constituents, "5px" lexed as ONE ident, and no number channel
      // was possible — found by the v2 guard battery).  Anything
      // without digits ("+", "--x", ".-x") falls through to the
      // ident/delim paths below.
      size_t j = i_;
      if (in_[j] == '+' || in_[j] == '-') ++j;
      bool digits = false;
      while (j < in_.size() && in_[j] >= '0' && in_[j] <= '9') {
        ++j;
        digits = true;
      }
      if (j < in_.size() && in_[j] == '.') {
        size_t k = j + 1;
        bool frac = false;
        while (k < in_.size() && in_[k] >= '0' && in_[k] <= '9') {
          ++k;
          frac = true;
        }
        if (frac) {
          j = k;
          digits = true;
        }
      }
      if (digits) {
        i_ = j;
        *t = {Kind::kNumber, in_.substr(start, i_ - start)};
        return true;
      }
    }
    if (IsIdentChar(c) || (c == '\\' && i_ + 1 < in_.size())) {
      // Ident (or function / url-token).
      while (i_ < in_.size()) {
        if (in_[i_] == '\\' && i_ + 1 < in_.size()) {
          i_ += 2;
          continue;
        }
        if (!IsIdentChar(in_[i_])) break;
        ++i_;
      }
      if (i_ < in_.size() && in_[i_] == '(') {
        std::string_view name = in_.substr(start, i_ - start);
        // url(: the minifier matches "url" as a SUFFIX with no word
        // boundary (every phase, since #1133 — "05-url(" and "xhurl("
        // are url-tokens to it), so the oracle must match that quirk
        // exactly or every such input false-positives.
        if (name.size() >= 3 && CiMatch(name[name.size() - 3], 'u') &&
            CiMatch(name[name.size() - 2], 'r') &&
            CiMatch(name[name.size() - 1], 'l')) {
          // url(: quoted content is function + string; unquoted is a
          // single url-token consumed to the closing ')'.
          ++i_;
          size_t j = i_;
          while (j < in_.size() && IsWs(in_[j])) ++j;
          if (j < in_.size() && (in_[j] == '\'' || in_[j] == '"')) {
            *t = {Kind::kFunction, in_.substr(start, i_ - start)};
            return true;
          }
          while (i_ < in_.size()) {
            if (in_[i_] == '\\' && i_ + 1 < in_.size()) {
              i_ += 2;
              continue;
            }
            if (in_[i_] == ')') {
              ++i_;
              break;
            }
            ++i_;
          }
          *t = {Kind::kUrl, in_.substr(start, i_ - start)};
          return true;
        }
        ++i_;
        *t = {Kind::kFunction, in_.substr(start, i_ - start)};
        return true;
      }
      *t = {Kind::kIdent, in_.substr(start, i_ - start)};
      return true;
    }
    ++i_;
    *t = {Kind::kDelim, in_.substr(start, 1)};
    return true;
  }

 private:
  std::string_view in_;
  size_t i_ = 0;
};

// The four v2 channels extracted from one document.

struct Channels {
  std::vector<std::string_view> urls;
  std::vector<std::string_view> strings;
  // Number tokens outside custom-property value regions, as canonical
  // keys (CanonicalNumberKey).
  std::vector<std::string> numbers;
  // Custom-property value regions: each region is a flat token-text
  // sequence (kind+text pairs compared byte-for-byte).
  std::vector<std::vector<Token>> custom_values;
};

// Normalizes a url-token span for comparison: strips whitespace
// immediately after the opening '(' and immediately before the
// closing ')' — the two trims the minifier is allowed to make there
// (Phase 1's post-'(' skip and pre-')' trim).  Interior whitespace is
// significant and preserved.  The closing paren is escape-aware: a
// ')' preceded by an odd run of backslashes is url CONTENT, not the
// closer, so the token is unterminated — for those, trailing
// whitespace is stripped instead (the minifier's stylesheet-EOF trim
// fires inside an unterminated url() at end of input; sanctioned,
// and escape-naivety here was a disclosed over-catch).
std::string_view NormalizeUrl(std::string_view text) {
  // Anchor on the url-suffix paren — the '(' whose preceding three
  // chars are "url" (case-insensitive, the minifier's own suffix
  // rule) — NOT the first '(' in the token: an escape pair before the
  // name ("\(") is consumed into the token and would mis-anchor the
  // content ("\\(url(\\ru)" — the post-"url(" whitespace the minifier
  // trims is content at the wrong anchor, crash-22f52361 of nightly
  // 30895630048; the 878-era "\\url("/"\\(url(" family on printable
  // input).  A url-token always contains the suffix match by
  // construction; the first-'(' fallback below is unreachable in
  // practice but kept for safety.
  auto ci = [](char c, char lower) {
    return c == lower || (lower >= 'a' && lower <= 'z' && c == lower - 32);
  };
  size_t b = text.size();
  for (size_t i = 3; i < text.size(); ++i) {
    if (text[i] == '(' && ci(text[i - 3], 'u') && ci(text[i - 2], 'r') &&
        ci(text[i - 1], 'l')) {
      b = i;
      break;
    }
  }
  if (b == text.size()) {
    b = 0;
    while (b < text.size() && text[b] != '(') ++b;
  }
  if (b < text.size()) ++b;
  while (b < text.size() && IsWs(text[b])) ++b;
  size_t e = text.size();
  if (e > b && text[e - 1] == ')') {
    size_t slashes = 0;
    while (e - 1 - slashes > b && text[e - 2 - slashes] == '\\') ++slashes;
    if (slashes % 2 == 0) --e;  // Unescaped: the real closing paren.
  }
  while (e > b && IsWs(text[e - 1])) --e;
  return text.substr(b, e - b);
}

// Canonical numeric key for the number channel (v2), computed by
// SIMULATING Phase 4's actual zero-deletion rule on the token text —
// derived by probing the minifier, not assumed: a '0' is deleted iff
// it is NOT preceded by a digit AND is immediately followed by '.'
// (looking past the token into the document for trailing chars).
// Nothing after the dot matters — the rule reproduces every observed
// transform:
//
//   0.5 -> .5     -0.5 -> -.5    +0.5 -> +.5    0.0% -> .0%
//   0.x -> .x     0. -> .        0.] -> .]      0.* / 0.% / 0.- / 0.+
//   10.50 (kept: '0' preceded by digit)         00.5 (kept, both)
//   1.0 (kept: frac '0' not followed by '.')    5PX (unit case kept)
//   0px (kept)    +5 (kept: not followed by '.')
//   and on multi-dot garbage: 0.0.5 -> ..5, 1.0.5 -> 1..5,
//   0.50.0.50.5 -> .50..50.5 — found when the v2 sweep red-lined on
//   printable multi-dot garbage (736 needs-triage with a '.'+digit
//   rule; the unified rule covers it).
//
// A token whose digits are all deleted collapses to a delimiter (the
// minified output tokenizes it as such), so the caller drops it from
// the channel.  Everything the rule does NOT cover still FLAGs:
// digit changes (5->6, 0.5->0.05/0.6), '+' removal, trailing-zero
// removal (10.50->10.5), unit changes (px->em, case-SENSITIVE units),
// number drops.  Residual, documented: exponent parts are not number
// tokens here, but an ADJACENT exponent is captured as the
// dimension's unit ("1e3" = number "1" + unit "e3"), so "1e3"->"1e4"
// and "1e3px"->"1e4px" FLAG; only space-separated exponent changes
// ("1 e3"->"1 e4") are invisible.  The mantissa still flags
// ("1e3"->"2e3").

std::string Phase4Canonical(std::string_view doc, std::string_view tok,
                            std::string_view unit, bool* has_digits) {
  std::string key;
  const char* doc_begin = doc.data();
  const char* doc_end = doc.data() + doc.size();
  bool digits = false;
  for (size_t p = 0; p < tok.size(); ++p) {
    char c = tok[p];
    if (c == '0') {
      char prev =
          p > 0 ? tok[p - 1] : (tok.data() > doc_begin ? tok.data()[-1] : '\0');
      const char* after = tok.data() + p + 1;
      bool next_dot = after < doc_end && after[0] == '.';
      bool prev_digit = prev >= '0' && prev <= '9';
      if (!prev_digit && next_dot) continue;  // Phase 4 deletes this '0'.
    }
    if (c >= '0' && c <= '9') digits = true;
    key += c;
  }
  // A trailing '.' whose fraction digits were all deleted becomes a
  // delimiter in the output's tokenization ("1.0.5" -> "1..5": the
  // number is "1", the dots are delims) — strip it.
  if (!key.empty() && key.back() == '.') key.pop_back();
  key += '\x01';
  key += unit;
  *has_digits = digits;
  return key;
}

// The canonicalization pass: the minifier's own Phase1/Phase2
// whitespace/comment engine, consumed verbatim from
// lib/css/css_phases.h, with preserved comments (#582) round-tripped
// through their sentinel so normalized text still carries their exact
// bytes (the fixpoint invariant below must hold on output that
// contains them).  Both sides are normalized BEFORE
// tokenizing, so glued forms are byte-identical whenever the minifier
// is byte-identical (the pass-1/pass-2 idempotence invariant, which
// the harness asserts as a counted diagnostic).
std::string Normalize(std::string_view input) {
  pagespeed::css::PreservedComments preserved;
  std::string glued =
      pagespeed::css::Phase2(pagespeed::css::Phase1(input, &preserved));
  std::string restored;
  if (!pagespeed::css::RestorePreservedComments(glued, preserved, &restored)) {
    // Sentinel count mismatch: fall back to the text as the phases left
    // it, where each preserved comment is still its one-byte sentinel.
    return glued;
  }
  return restored;
}

// The glue-free extractor: the plain LITE tokenizer over NORMALIZED
// text.  Both sides arrive pre-glued, so escape-absorbed numbers are
// already ident content, chains are already idents, signs are already
// glued, and comments are already stripped — none of the retired
// edge rules is needed.  Numbers carry their adjacent ident/'%'
// units (dimension/percentage), canonicalized by Phase4Canonical
// (Phase 4's zero-strip, which is deliberately NOT part of
// normalization); custom-property value regions are tracked
// byte-for-byte (whitespace and comments skipped), with the
// declaration-boundary requirement from the v2 design: a region
// opens when a "--"-prefixed ident is followed by a ':' token AND
// sits at a declaration boundary (start of input or a preceding
// '{', ';', or '}' token — mirroring the minifier's
// AtCustomPropertyColon boundary rule, "'--' occurring mid-value
// never triggers"; on normalized text the custom name is already a
// single glued ident, so no glue rules are needed here either).
// One deliberate departure from that rule: a ';' inside an open '(',
// '[' or function bracket outside a custom region is NOT a boundary
// (nesting resets at '{' and '}').  A "--a:" inside a selector bracket
// ("[;--a:b]{;c}") is therefore not a custom property, so it cannot
// swallow the rule block's ';' and make Phase 5's sanctioned
// empty-declaration trim look like a custom-value change.
Channels ExtractGlued(std::string_view input) {
  Channels out;
  Tokenizer tz(input);
  Token t;
  bool pending_custom_colon = false;
  bool pending_at_boundary = false;
  bool at_boundary = true;  // Start of input counts as a boundary.
  bool in_region = false;
  int depth = 0;
  int outer_nest = 0;  // Open '(' '[' fn( outside custom regions.
  // Number-channel one-token delay: a kNumber is held until the next
  // token decides whether it carries a unit — an immediately adjacent
  // ident (dimension) or '%' delim (percentage), adjacency by text
  // contiguity since the tokenizer skips whitespace.  Numbers inside
  // custom-property value regions belong to the byte-for-byte custom
  // channel and are excluded here.
  std::string_view pending_number;
  bool have_pending_number = false;
  while (tz.Next(&t)) {
    if (have_pending_number) {
      std::string_view unit;
      if (pending_number.data() + pending_number.size() == t.text.data() &&
          (t.kind == Kind::kIdent ||
           (t.kind == Kind::kDelim && t.text == "%"))) {
        unit = t.text;
      }
      bool has_digits = false;
      const std::string key =
          Phase4Canonical(input, pending_number, unit, &has_digits);
      if (has_digits) out.numbers.push_back(key);
      have_pending_number = false;
      if (!unit.empty()) continue;  // The unit token is consumed.
    }
    if (in_region) {
      if (depth == 0 && (t.kind == Kind::kSemicolon ||
                         (t.kind == Kind::kBrace && t.text[0] == '}'))) {
        in_region = false;
      } else {
        out.custom_values.back().push_back(t);
        if (t.kind == Kind::kBrace) {
          char b = t.text[0];
          if (b == '{' || b == '(' || b == '[') ++depth;
          if (depth > 0 && (b == '}' || b == ')' || b == ']')) --depth;
        }
      }
    }
    if (!in_region) {
      if (t.kind == Kind::kFunction ||
          (t.kind == Kind::kBrace && (t.text[0] == '(' || t.text[0] == '['))) {
        ++outer_nest;
      } else if (t.kind == Kind::kBrace &&
                 (t.text[0] == ')' || t.text[0] == ']')) {
        if (outer_nest > 0) --outer_nest;
      } else if (t.kind == Kind::kBrace &&
                 (t.text[0] == '{' || t.text[0] == '}')) {
        outer_nest = 0;
      }
    }
    if (t.kind == Kind::kUrl) out.urls.push_back(NormalizeUrl(t.text));
    if (t.kind == Kind::kString) out.strings.push_back(t.text);
    if (t.kind == Kind::kNumber && !in_region) {
      pending_number = t.text;
      have_pending_number = true;
      // NO continue: the number must still reach the boundary
      // bookkeeping below (a no-op for it — not an ident, not a
      // boundary — but skipping it left at_boundary STALE and opened
      // bogus custom regions after numbers, e.g. "5\n--z:{;}}").
    }
    if (pending_custom_colon && pending_at_boundary && t.kind == Kind::kColon) {
      out.custom_values.emplace_back();
      in_region = true;
      depth = 0;
    }
    // A "--"-prefixed ident MIGHT be a custom property name; the
    // colon decides on the next token (anything in between resets
    // it).  The boundary check must be snapshotted BEFORE this
    // token — the ident itself is never a boundary.
    pending_custom_colon = t.kind == Kind::kIdent && t.text.size() >= 2 &&
                           t.text[0] == '-' && t.text[1] == '-';
    pending_at_boundary = at_boundary;
    at_boundary =
        (t.kind == Kind::kSemicolon && outer_nest == 0) ||
        (t.kind == Kind::kBrace && (t.text[0] == '{' || t.text[0] == '}'));
  }
  if (have_pending_number) {
    bool has_digits = false;
    const std::string key =
        Phase4Canonical(input, pending_number, "", &has_digits);
    if (has_digits) out.numbers.push_back(key);
  }
  return out;
}

bool SameTokens(const std::vector<Token>& a, const std::vector<Token>& b) {
  if (a.size() != b.size()) return false;
  for (size_t i = 0; i < a.size(); ++i) {
    if (a[i].kind != b[i].kind || a[i].text != b[i].text) return false;
  }
  return true;
}

// Multiset-with-dedup comparison for the url/string channels (PR
// over-catch fix, widened after review Finding 1): TWO sanctioned
// edits of the Phase 5 longhand→shorthand collapse must not flag —
//
//   1. REORDER: the collapse reorders declarations
//      ("overflow-y:'b';overflow-x:'a'" -> "overflow:'a' 'b'"),
//      reordering channel tokens without changing them.
//   2. IDENTICAL-VALUE DEDUP: a FULL family with identical values
//      collapses to one shorthand value ("overflow-x:'a';overflow-y:'a'"
//      -> "overflow:'a'"), dropping duplicate tokens.
//
// So the output channel must be a SUBMULTISET of the input channel
// with the SAME DISTINCT VALUES: reorder and duplicate-removal pass,
// while every other corruption shape still flags — gained or
// duplicated tokens (fails the submultiset half), dropped DISTINCT
// tokens (fails the distinct-values half), mutations and quote flips
// (both halves), and channel moves (channels stay separate).
//
// Residual hole, documented and accepted: dropping one of several
// IDENTICAL tokens where NO collapse happened is indistinguishable
// from sanctioned dedup without declaration-level evidence (the
// comparator is css-syntax-3-LITE; family-collapse evidence needs the
// planned full-sequence widening).  It stays acceptable while the
// minifier's only token-REMOVAL mechanism on valid input is the
// collapse dedup itself — empty-declaration elimination operates at
// declaration level, so a removed declaration's DISTINCT tokens
// vanish and the distinct-values half still flags; the classes that
// delete individual tokens on garbage (#1170 rejoin) are
// invalid-input-only.
template <typename T>
bool SameChannelTokens(std::vector<T> input_tokens,
                       std::vector<T> output_tokens) {
  std::sort(input_tokens.begin(), input_tokens.end());
  std::sort(output_tokens.begin(), output_tokens.end());
  // Output may not gain, duplicate, or mutate tokens (std::includes is
  // multiset inclusion on sorted ranges).
  if (!std::includes(input_tokens.begin(), input_tokens.end(),
                     output_tokens.begin(), output_tokens.end())) {
    return false;
  }
  // ... but every distinct token must survive (sanctioned dedup only
  // removes duplicates).
  input_tokens.erase(std::unique(input_tokens.begin(), input_tokens.end()),
                     input_tokens.end());
  output_tokens.erase(std::unique(output_tokens.begin(), output_tokens.end()),
                      output_tokens.end());
  return input_tokens == output_tokens;
}

// The v2 oracle core: the channels of the input must survive in the
// minified output — url/string per SameChannelTokens (sanctioned
// reorder + identical-value dedup under collapse), custom-property
// value regions sequence-strict (no family matches a "--" name, so
// collapse never reorders or dedups them).  Any other difference
// means the minifier changed the served token content of a url, a
// string, or a custom-property value.
bool ChannelsEquivalent(std::string_view input, std::string_view output) {
  Channels in = ExtractGlued(input);
  Channels out = ExtractGlued(output);
  if (!SameChannelTokens(in.urls, out.urls) ||
      !SameChannelTokens(in.strings, out.strings) ||
      !SameChannelTokens(in.numbers, out.numbers) ||
      in.custom_values.size() != out.custom_values.size()) {
    return false;
  }
  for (size_t i = 0; i < in.custom_values.size(); ++i) {
    if (!SameTokens(in.custom_values[i], out.custom_values[i])) return false;
  }
  return true;
}

// ------------------------------------------------------------------
// v3: garbage channel-agreement on invalid input.
//
// The three adjudication mechanisms below are EXACT ports of the
// reviewed known-bucket predicates in
// tools/ci/classify-css-fuzz-artifacts.sh (the nightly's classifier):
// the residual invalid-input token-oracle over-catches are adjudicated
// by the oracle itself instead of being bucketed after the fact.  Each

bool ValidUtf8Sequence(std::string_view s) {
  const unsigned char b0 = s[0];
  size_t need;
  uint32_t cp;
  if (b0 >= 0xc2 && b0 <= 0xdf) {
    need = 2;
    cp = b0 & 0x1f;
  } else if (b0 >= 0xe0 && b0 <= 0xef) {
    need = 3;
    cp = b0 & 0x0f;
  } else if (b0 >= 0xf0 && b0 <= 0xf4) {
    need = 4;
    cp = b0 & 0x07;
  } else {
    return false;
  }
  if (s.size() < need) return false;
  for (size_t k = 1; k < need; ++k) {
    const unsigned char bc = s[k];
    if ((bc & 0xc0) != 0x80) return false;
    cp = (cp << 6) | (bc & 0x3f);
  }
  // Reject overlongs, surrogates, and out-of-range, plus anything the
  // classifier's `ord(ch) >= 0x80` check would not count as text.
  if (cp < 0x80) return false;
  if (need == 2 && cp < 0x800) return false;
  if (need == 3 && cp < 0x10000) {
    if (cp < 0x800 || (cp >= 0xd800 && cp <= 0xdfff)) return false;
  }
  if (need == 4 && (cp < 0x10000 || cp > 0x10ffff)) return false;
  return true;
}

int GarbageByteCount(std::string_view in) {
  int bad = 0;
  size_t i = 0;
  while (i < in.size()) {
    const unsigned char b = in[i];
    if (b == 9 || b == 10 || b == 13 || (b >= 0x20 && b <= 0x7e)) {
      ++i;
      continue;
    }
    bool decoded = false;
    for (size_t width = 2; width <= 4; ++width) {
      if (i + width > in.size()) break;
      if (ValidUtf8Sequence(in.substr(i, width))) {
        i += width;
        decoded = true;
        break;
      }
    }
    if (!decoded) {
      ++bad;
      ++i;
    }
  }
  return bad;
}

// Mirrors the classifier's is_unterminated_url exactly: the input
// contains "url(" (case-SENSITIVE) with no ')' anywhere after the last
// one; a ')' before the last url( is irrelevant.
bool HasUnterminatedUrlAtEof(std::string_view in) {
  const size_t pos = in.rfind("url(");
  if (pos == std::string_view::npos) return false;
  return in.find(')', pos + 4) == std::string_view::npos;
}

// The oracle: canonicalize both sides with the minifier's own
// Phase1/Phase2 engine, then require the input's channels to survive
// in the output (urls/strings/numbers per SameChannelTokens —
// sanctioned collapse reorder and identical-value dedup; custom
// regions sequence-strict).  Fallbacks for the non-glue axes: the
// append guard, R2 (unterminated url at EOF), R3 (garbage-input
// agreement) — see the file header for the full retirement map and
// the survival rationale.
// True when the input ends inside a preserved comment (#582) that never
// closes.  The minifier closes such a comment with "*/" on output, so
// that exact two-byte tail is sanctioned.
bool EndsInUnterminatedPreservedComment(std::string_view input) {
  PreservedComments preserved;
  Phase1(input, &preserved);
  if (preserved.texts.empty()) return false;
  const std::string& last = preserved.texts.back();
  const bool closed = last.size() >= 4 && last[last.size() - 2] == '*' &&
                      last[last.size() - 1] == '/';
  return !closed && input.size() >= last.size() &&
         input.substr(input.size() - last.size()) == last;
}

bool Equivalent(std::string_view input, std::string_view output) {
  if (ChannelsEquivalent(Normalize(input), Normalize(output))) {
    // Append guard (carried from v3): an output that extends the
    // input BYTE-FOR-BYTE with appended NON-WHITESPACE content is
    // served-content gain and flags.  The channels cannot see it (an
    // ident-level gain touches no channel).  A whitespace-only tail
    // stays sanctioned, and garbage input is exempt (R3 defers there,
    // matching the classifier's bucket).
    if (output.size() > input.size() &&
        output.substr(0, input.size()) == input &&
        GarbageByteCount(input) == 0 &&
        !(output.substr(input.size()) == "*/" &&
          EndsInUnterminatedPreservedComment(input))) {
      for (const char c : output.substr(input.size())) {
        if (!(c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' ||
              c == '\v')) {
          return false;
        }
      }
    }
    return true;
  }
  if (HasUnterminatedUrlAtEof(input)) return true;  // R2 fallback.
  if (GarbageByteCount(input) > 0) return true;     // R3 fallback.
  return false;
}

}  // namespace
}  // namespace token_stream
}  // namespace pagespeed::css

#endif  // PAGESPEED_LIB_CSS_CSS_TOKEN_STREAM_H_
