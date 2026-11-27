// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.
//
// Shared whitespace/comment phases for the streaming CSS minifier —
// the SINGLE SOURCE OF TRUTH for Phase 1 (comment
// strip + whitespace collapse + string/url preservation, keeping
// /*! and @license/@preserve comments for later restoration) and Phase 2
// (whitespace removal around operators), factored verbatim out of
// lib/css/css_minify.cc so the minifier and the token-stream fuzz
// oracle (lib/css/css_token_stream.h) consume the SAME code (the
// glue-normalization cutover: the oracle canonicalizes with
// Phase2(Phase1(x)) instead of simulating glue behavior with
// hand-grown edge rules — divergence-by-construction is
// impossible).  The header carries ONLY what Phase1/Phase2 need; the
// downstream-only predicates (ident/hex/escape-structure and
// IsValueGroupBrace for Phases 3-5) stayed in css_minify.cc.
//
// Anonymous namespace at pagespeed::css scope, mirroring the
// pre-extraction layout in css_minify.cc: call sites keep their
// unqualified names, and each including TU gets its own copy
// (the css_token_stream.h pattern).

#ifndef PAGESPEED_LIB_CSS_CSS_PHASES_H_
#define PAGESPEED_LIB_CSS_CSS_PHASES_H_

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace pagespeed::css {
namespace {

bool IsCssWhitespace(char c) {
  return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f';
}

bool CanRemoveSpaceBefore(char c) {
  return c == '{' || c == '}' || c == ';' || c == ',' || c == '>' || c == '+' ||
         c == '~';
}

bool CanRemoveSpaceAfter(char c) {
  return c == '{' || c == '}' || c == ';' || c == ':' || c == ',' || c == '>' ||
         c == '+' || c == '~';
}

enum class State : uint8_t {
  kNormal,
  kInComment,
  kInSingleStr,
  kInDoubleStr,
  kInUrl
};

bool CiMatch(char c, char lower) { return c == lower || c == (lower - 32); }

// ----------------------------------------------------------------------
// Preserved comments (#582): a comment starting with /*! or
// containing @license or @preserve (case-insensitive) is kept in the
// minified output byte for byte, in position.  Phases 2-5 have no
// comment state, so Phase 1 cannot leave the bytes in the stream —
// Phase 2 would strip spaces inside them, Phase 4 rewrite their
// decimals, Phase 3 trim their ';'s, Phase 5 read their braces as
// block edges.  Instead Phase 1 replaces each preserved comment with
// a one-byte SENTINEL and records the exact bytes; the caller restores
// them after the pipeline (MinifyCss) and after normalization (the
// token-stream oracle's Normalize, so its pass-1/pass-2 fixpoint
// invariant keeps holding on output that contains preserved
// comments).  The sentinel is the first candidate byte the input does
// not contain — control bytes never occur in valid CSS, so it cannot
// collide with real content; an input containing every candidate is
// pathological, and preservation degrades to stripping there.

struct PreservedComments {
  // Exact bytes of each preserved comment (from "/*" through "*/" or
  // end of input), in order.  Filled by Phase 1.
  std::vector<std::string> texts;
  char sentinel = '\0';  // '\0' = preservation disabled this call.
};

// One past the "*/" closing the comment that starts at i, or end of
// input when the comment never closes.  Matches the kInComment scan:
// the FIRST "*/" wins and backslashes carry no meaning inside
// comments.
size_t CommentEnd(std::string_view input, size_t i) {
  for (size_t j = i + 2; j + 1 < input.size(); ++j) {
    if (input[j] == '*' && input[j + 1] == '/') return j + 2;
  }
  return input.size();
}

// Case-insensitive substring match.  Case folds only where the needle
// char is an ASCII lowercase letter, so '@' matches '@' alone.
bool ContainsCi(std::string_view text, std::string_view lower) {
  if (text.size() < lower.size()) return false;
  for (size_t i = 0; i + lower.size() <= text.size(); ++i) {
    bool matched = true;
    for (size_t j = 0; j < lower.size(); ++j) {
      char t = text[i + j];
      char n = lower[j];
      if (t != n && !(n >= 'a' && n <= 'z' && t == n - 32)) {
        matched = false;
        break;
      }
    }
    if (matched) return true;
  }
  return false;
}

// True for the comment spanning [begin, end) of input that must
// survive minification.
bool CommentIsPreserved(std::string_view input, size_t begin, size_t end) {
  const std::string_view comment = input.substr(begin, end - begin);
  if (comment.size() > 2 && comment[2] == '!') return true;
  return ContainsCi(comment, "@license") || ContainsCi(comment, "@preserve");
}

// Sentinel for this input: the first candidate byte it does not
// contain, or '\0' when preservation must be disabled.
char ChooseCommentSentinel(std::string_view input) {
  for (const char c :
       {'\x01', '\x02', '\x03', '\x04', '\x05', '\x06', '\x07'}) {
    if (input.find(c) == std::string_view::npos) return c;
  }
  return '\0';
}

// Replaces the k-th sentinel occurrence with the k-th recorded
// comment.  Every phase copies the sentinel through verbatim (it is
// not whitespace, not an operator, not a block/declaration delimiter,
// and never enters strings or urls), so occurrence order equals
// recording order.  Nothing enforces that, so the count is checked:
// when the output carries a different number of sentinels than there
// are recorded comments, pairing them would put comments in the wrong
// places, and the function returns false with *out left empty.
//
// A preserved comment that never closed (it ran to the end of the
// input) is closed with "*/" when restored, so the output stays
// well-formed if it is later concatenated or inlined with other CSS.
bool RestorePreservedComments(std::string_view input,
                              const PreservedComments& preserved,
                              std::string* out) {
  out->clear();
  if (preserved.sentinel == '\0' || preserved.texts.empty()) {
    out->assign(input);
    return true;
  }
  size_t count = 0;
  for (char c : input) {
    if (c == preserved.sentinel) ++count;
  }
  if (count != preserved.texts.size()) return false;
  out->reserve(input.size());
  size_t k = 0;
  for (char c : input) {
    if (c == preserved.sentinel) {
      const std::string& text = preserved.texts[k++];
      *out += text;
      const bool closed = text.size() >= 4 && text[text.size() - 2] == '*' &&
                          text[text.size() - 1] == '/';
      if (!closed) *out += "*/";
      continue;
    }
    *out += c;
  }
  return true;
}

// True iff text[pos] is ESCAPE CONTENT: preceded by an odd-length run
// of backslashes, so it is part of an identifier, never a syntactic
// character.  The single escape predicate for the property-name scans
// (AtCustomPropertyColon here, IsValueGroupBrace in css_minify.cc), so
// every phase agrees on what a custom-property name contains.
bool IsEscapeContentAt(std::string_view text, size_t pos) {
  size_t slashes = 0;
  while (pos > slashes && text[pos - 1 - slashes] == '\\') ++slashes;
  return slashes % 2 == 1;
}

// True if a ':' about to be emitted sits in a declaration's property
// position and the property name starts with "--" (a CSS custom
// property).  Custom-property values are opaque token streams whose
// whitespace is significant (calc() operands, selector fragments,
// arbitrary strings read back via getPropertyValue()), so minification
// must leave them verbatim.  Scans backwards over the output built so
// far: skips spaces, matches the property identifier, then requires a
// declaration boundary ('{', ';', '}', or start of output) before it —
// so "--" occurring mid-value (calc(a - -b)) or inside parens
// (@supports (--x: y)) never triggers.  Combinators ('+', '>', '~')
// also stop the identifier scan: a property name can never contain
// them, and Phase 2 removes whitespace after combinators before this
// check runs, so without the stop they could glue a phantom "--" name
// across the removed space ("--+ y: }", #1156).  The set covers every
// character whose adjacent whitespace Phase 2 can remove — the
// operator sets, the '!' special case ("-- !: 3"), and calc-mode's
// '*' and '/' ("a{b:calc(1*2; --* y: 2)}") — so the leak is closed by
// construction, not case by case.
//
// Escape-awareness (issue #1238): a stop-set char that is ESCAPE
// CONTENT — preceded by an odd-length backslash run — is part of the
// name, not a boundary (an escaped char is a valid custom-property
// name char per CSS Syntax 3: `--\ \>` is the name `--`, space, `>`).
// Without this, a name containing e.g. an escaped space scanned as
// non-custom and the value got ordinary-CSS processing (Phase 4's
// decimal rule rewrote `a{--\ \>:0.}` to `a{--\ \>:.}` — valid-CSS
// served-value corruption, found by the token-oracle nightly).
bool AtCustomPropertyColon(const std::string& r) {
  size_t e = r.size();
  while (e > 0 && r[e - 1] == ' ' && !IsEscapeContentAt(r, e - 1)) --e;
  size_t s = e;
  while (s > 0) {
    char c = r[s - 1];
    // Glue-awareness (pass-ordering idempotence fix, #1238 triage): a
    // whitespace run inside the name is skipped exactly when Phase 2
    // would remove it — the char before the run is in
    // CanRemoveSpaceAfter's set, or, single char, the char after it is
    // in CanRemoveSpaceBefore's set — so the name is evaluated on its
    // POST-GLUE form ("--\> p:" scans as the custom "--\>p:").  A run
    // preceded by a declaration boundary ('{', ';', '}', or start of
    // output, unescaped) still breaks the scan — that case is a real
    // separator on every pass ("{ --x"), not name content.  Without
    // this, Phase 1 detected "not custom" while the name was still
    // unglued and processed the value as ordinary CSS, Phase 2 glued
    // the name, and the NEXT pass saw a custom property from the
    // start — a kept-then-dropped value-leading space broke
    // minify∘minify = minify.
    if (!IsEscapeContentAt(r, s - 1) && c == ' ') {
      size_t s2 = s;
      while (s2 > 0 && r[s2 - 1] == ' ') --s2;
      if (s2 == 0) break;
      const char before = r[s2 - 1];
      const bool boundary = !IsEscapeContentAt(r, s2 - 1) &&
                            (before == '{' || before == ';' || before == '}');
      if (boundary) break;
      const bool removable = CanRemoveSpaceAfter(before) ||
                             (s2 == s - 1 && CanRemoveSpaceBefore(r[s]));
      if (!removable) break;
      s = s2;
      continue;
    }
    if (!IsEscapeContentAt(r, s - 1) &&
        (c == '{' || c == '}' || c == ';' || c == ':' || c == ',' || c == '(' ||
         c == ')' || c == '\'' || c == '"' || c == '+' || c == '>' ||
         c == '~' || c == '!' || c == '*' || c == '/')) {
      break;
    }
    --s;
  }
  if (e - s < 2 || r[s] != '-' || r[s + 1] != '-') return false;
  while (s > 0 && r[s - 1] == ' ' && !IsEscapeContentAt(r, s - 1)) --s;
  return s == 0 || r[s - 1] == '{' || r[s - 1] == ';' || r[s - 1] == '}';
}

// Phase 1: Strip comments, collapse whitespace, preserve strings and
// url().  Preserved comments (see PreservedComments) are replaced by
// preserved->sentinel with their exact bytes recorded in *preserved;
// restore them with RestorePreservedComments.  All other comments are
// stripped as before.
std::string Phase1(std::string_view input, PreservedComments* preserved) {
  std::string r;
  r.reserve(input.size());
  State state = State::kNormal;
  bool psp = false;
  bool in_custom = false;
  bool custom_url = false;
  bool custom_started = false;
  int custom_bd = 0;
  size_t i = 0;
  preserved->sentinel = ChooseCommentSentinel(input);
  while (i < input.size()) {
    char c = input[i];
    switch (state) {
      case State::kNormal:
        if (c == '\\' && i + 1 < input.size()) {
          // Backslash escape: consume the pair verbatim in every
          // normal-context sub-mode (plain, custom value, custom url()),
          // mirroring Phase 2's kNormal guard (01d14490a).  Without this,
          // Phase 1 tokenizes differently from Phase 2: an escaped quote
          // opens a string here but not there (non-idempotence, #1133),
          // and an escaped '/' starts a phantom comment that eats real
          // content (a{--x:\/*y*/;b:c} lost its custom-property value).
          if (psp) {
            // No separator before the first custom-property value
            // token (see the custom_started guard below, #1133).
            if (!in_custom || custom_started) r += ' ';
            psp = false;
          }
          if (in_custom) custom_started = true;  // An escape pair is a token.
          r += c;
          r += input[i + 1];
          i += 2;
          continue;
        }
        if (in_custom) {
          // Custom-property value: opaque token stream — preserve it
          // verbatim (including whitespace runs).  Comments are still
          // stripped, but replaced by a space so adjacent tokens do not
          // merge.  The value ends at the ';' or '}' at its own
          // brace/paren/bracket nesting level.
          if (!custom_started) {
            // Value not started: the leading-whitespace trim at the ':'
            // entry runs before any comment is seen, so a comment
            // immediately after the colon ("--x:/*c*/v") would otherwise
            // be replaced by a separator space, re-introducing leading
            // whitespace the trim just removed — and the next pass,
            // seeing no comment, trims it, breaking idempotence
            // (#1133).  Skip whitespace and comments until the first
            // value token.
            psp = false;
            if (IsCssWhitespace(c)) {
              ++i;
              continue;
            }
            if (c == '/' && i + 1 < input.size() && input[i + 1] == '*') {
              size_t end = CommentEnd(input, i);
              if (preserved->sentinel != '\0' &&
                  CommentIsPreserved(input, i, end)) {
                // A preserved comment is itself a value token (its
                // bytes stay in the served value), so the value has
                // started — same as the escape-pair token above.
                preserved->texts.emplace_back(input.substr(i, end - i));
                r += preserved->sentinel;
                custom_started = true;
                i = end;
                continue;
              }
              state = State::kInComment;
              i += 2;
              continue;
            }
            custom_started = true;
          }
          if (custom_url) {
            // Inside an unquoted url() token: per CSS syntax "/*" here
            // is URL content, not a comment start — mirror the
            // non-custom path's kInUrl shielding and copy verbatim
            // (no comment detection, no depth tracking) until the
            // closing ')'.  Escapes are already consumed as pairs by
            // the kNormal guard above, which fires first in every
            // sub-mode.
            if (c == ')') custom_url = false;
            r += c;
            ++i;
            continue;
          }
          if (c == '/' && i + 1 < input.size() && input[i + 1] == '*') {
            size_t end = CommentEnd(input, i);
            if (preserved->sentinel != '\0' &&
                CommentIsPreserved(input, i, end)) {
              // No separator space follows the sentinel: the restored
              // comment's own bytes keep the surrounding tokens apart
              // (the tokenizer skips comments).  A separator pending
              // from a preceding stripped comment still flushes, as it
              // would before any token.
              if (psp) {
                r += ' ';
                psp = false;
              }
              preserved->texts.emplace_back(input.substr(i, end - i));
              r += preserved->sentinel;
              i = end;
              continue;
            }
            state = State::kInComment;
            i += 2;
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
          if (psp) {
            // A comment was just stripped: keep a token separator
            // unless whitespace or a terminator follows anyway.
            psp = false;
            if (!IsCssWhitespace(c) && c != ';' && c != '}') r += ' ';
          }
          if (c == '(' && r.size() >= 3 && CiMatch(r[r.size() - 3], 'u') &&
              CiMatch(r[r.size() - 2], 'r') && CiMatch(r[r.size() - 1], 'l')) {
            // url( opening: if the content is unquoted, enter the
            // verbatim url mode above.  Quoted url("...")/url('...')
            // content is already covered by the string states, so
            // leave that paren to the normal depth tracking below.
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
            // Declaration terminator: trim the value's trailing
            // whitespace and leave opaque mode.  Never trim an escaped
            // whitespace char ("\ " is a token character, preceded by
            // an odd number of backslashes): popping it would glue the
            // backslash to the terminator, turning it into "\;" or
            // "\}" and destroying the declaration.
            while (!r.empty() && IsCssWhitespace(r.back())) {
              size_t bs = 0;
              while (bs + 1 < r.size() && r[r.size() - 2 - bs] == '\\') ++bs;
              if (bs % 2 != 0) break;
              r.pop_back();
            }
            in_custom = false;
          }
          r += c;
          ++i;
          continue;
        }
        if (c == '/' && i + 1 < input.size() && input[i + 1] == '*') {
          size_t end = CommentEnd(input, i);
          if (preserved->sentinel != '\0' &&
              CommentIsPreserved(input, i, end)) {
            // Flush any pending separator first, exactly as the quote
            // entries below do; none is added after the sentinel — the
            // restored comment's own bytes separate it from the token
            // that follows.
            if (psp) {
              r += ' ';
              psp = false;
            }
            preserved->texts.emplace_back(input.substr(i, end - i));
            r += preserved->sentinel;
            i = end;
            continue;
          }
          state = State::kInComment;
          i += 2;
          continue;
        }
        if (c == '\'') {
          if (psp) {
            r += ' ';
            psp = false;
          }
          r += c;
          state = State::kInSingleStr;
          ++i;
          continue;
        }
        if (c == '"') {
          if (psp) {
            r += ' ';
            psp = false;
          }
          r += c;
          state = State::kInDoubleStr;
          ++i;
          continue;
        }
        if (c == '(' && !psp && r.size() >= 3 &&
            CiMatch(r[r.size() - 3], 'u') && CiMatch(r[r.size() - 2], 'r') &&
            CiMatch(r[r.size() - 1], 'l')) {
          r += c;
          ++i;
          while (i < input.size() && IsCssWhitespace(input[i])) ++i;
          if (i < input.size() && (input[i] == '\'' || input[i] == '"'))
            continue;
          state = State::kInUrl;
          continue;
        }
        if (c == ':' && AtCustomPropertyColon(r)) {
          if (psp) {
            r += ' ';
            psp = false;
          }
          r += c;
          in_custom = true;
          custom_bd = 0;
          custom_started = false;
          ++i;
          // Trim the value's leading whitespace (safe: neither var()
          // substitution nor serialization depends on it).
          while (i < input.size() && IsCssWhitespace(input[i])) ++i;
          continue;
        }
        if (IsCssWhitespace(c)) {
          psp = true;
          ++i;
          continue;
        }
        if (psp) {
          r += ' ';
          psp = false;
        }
        r += c;
        ++i;
        break;
      case State::kInComment:
        if (c == '*' && i + 1 < input.size() && input[i + 1] == '/') {
          state = State::kNormal;
          i += 2;
          psp = true;
          continue;
        }
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
        if (c == ')') {
          // Never trim an escaped whitespace char ("\ " is a URL
          // character, preceded by an odd number of backslashes):
          // popping it would glue the backslash to the paren, turning
          // it into "\)" and changing the URL (#1154).
          while (!r.empty() && r.back() == ' ') {
            size_t bs = 0;
            while (bs + 1 < r.size() && r[r.size() - 2 - bs] == '\\') ++bs;
            if (bs % 2 != 0) break;
            r.pop_back();
          }
          r += c;
          state = State::kNormal;
          ++i;
          continue;
        }
        if (c == '\\' && i + 1 < input.size()) {
          r += c;
          r += input[i + 1];
          i += 2;
          continue;
        }
        r += c;
        ++i;
        break;
    }
  }
  if (state == State::kInUrl) {
    // Unterminated url() at end of input: trim trailing spaces, mirroring
    // the ')' handler above.  Without this the spaces survive Phase 1
    // verbatim but are trimmed on the NEXT pass through this same code,
    // so minify would not be idempotent (#1133).  String states are
    // exempt: their trailing whitespace is significant content.  Never
    // trim an escaped whitespace char ("\ " is a URL character, preceded
    // by an odd number of backslashes): popping it would leave the
    // backslash dangling at end of input, deleting URL content.  (The
    // two pre-existing url trims without this guard are issue #1154.)
    while (!r.empty() && r.back() == ' ') {
      size_t bs = 0;
      while (bs + 1 < r.size() && r[r.size() - 2 - bs] == '\\') ++bs;
      if (bs % 2 != 0) break;
      r.pop_back();
    }
  }
  return r;
}

// Phase 2: Remove unnecessary whitespace around operators.
// Inside calc(), only preserve spaces around binary + and -.
std::string Phase2(std::string_view input) {
  std::string r;
  r.reserve(input.size());
  State state = State::kNormal;
  int pd = 0, cpd = 0;
  bool in_custom = false;
  bool custom_url = false;
  int custom_bd = 0;
  size_t i = 0;
  while (i < input.size()) {
    char c = input[i];
    switch (state) {
      case State::kNormal: {
        if (c == '\\') {
          // Backslash escape: emit the pair verbatim so an escaped
          // space in an identifier (".a\ {") is not treated as
          // removable whitespace, and escaped delimiters carry no
          // structural meaning.
          r += c;
          if (i + 1 < input.size()) {
            r += input[i + 1];
            i += 2;
          } else {
            ++i;
          }
          continue;
        }
        if (in_custom) {
          // Custom-property value: opaque token stream — preserve its
          // whitespace verbatim, exactly like string literals.  The
          // value ends at the ';' or '}' at its own nesting level.
          if (custom_url) {
            // Inside an unquoted url() token: mirror Phase 1's
            // shielding — copy verbatim (no depth tracking, no string
            // states) until the closing ')'.  Without this, braces in
            // url content unbalanced custom_bd against Phase 1, so the
            // two phases disagreed on where the custom value ends
            // (#1133).
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
            // shielded mode above, mirroring Phase 1 (which likewise
            // keeps the whitespace after '(' verbatim here, unlike the
            // non-custom path).  Quoted url("...")/url('...') content
            // falls through to the normal depth tracking, covered by
            // the string states.
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
          // Unquoted url(): enter the url state, mirroring Phase 1.  Phase
          // 2 previously had no url concept, so url content was scanned as
          // ordinary CSS: a space before an operator inside a url
          // (url(a ;b)) was stripped, corrupting the URL, and an
          // unterminated url( at end of input — which Phase 1 preserves
          // verbatim — collapsed one whitespace-run tail per pass
          // (#1133).  A preceding space ("url (") leaves r ending in ' ',
          // so the match fails here exactly as Phase 1's !psp guard
          // intends.
          size_t j = i + 1;
          while (j < input.size() && IsCssWhitespace(input[j])) ++j;
          if (j >= input.size() || (input[j] != '\'' && input[j] != '"')) {
            r += c;
            i = j;  // Skip the whitespace after '(', as Phase 1 does.
            state = State::kInUrl;
            continue;
          }
          // Quoted url("...")/url('...'): fall through to the generic
          // '(' path so paren-depth accounting and math detection stay
          // balanced; the string states cover the quoted content.
        }
        if (c == '(') {
          ++pd;
          // CSS math functions require spaces around + and - operators.
          // Match case-insensitively.  The list is the complete CSS Values 4
          // math-function set plus the CSS Values 5 functions whose arguments
          // are calc-sums (calc-size, progress, random; "progress" also covers
          // media-progress()/container-progress() since '-' is a boundary).
          // Inclusion is one-sided-safe for the +/- rule: preserving
          // spaces around + and - is always valid CSS, while a missing
          // name invalidates declarations, so new calc-sum-accepting
          // functions belong here.  Math mode also tightens spaces
          // around '*' and '/' — the same treatment regular values get —
          // and with '-' as a boundary, dashed idents like --my-random()
          // enter math mode too; that false positive is accepted.
          // Match a CSS math function name at the end of r, requiring a
          // word boundary before the name (so "localcalc" doesn't match
          // "calc").  Alphanumeric chars and '_' are not boundaries; '-'
          // is (vendor prefixes like -webkit-calc are legitimate).
          auto is_math_fn = [&](std::string_view suffix) -> bool {
            if (r.size() < suffix.size()) return false;
            size_t start = r.size() - suffix.size();
            for (size_t j = 0; j < suffix.size(); ++j) {
              char a = r[start + j] | 0x20;
              if (a != suffix[j]) return false;
            }
            if (start > 0) {
              char before = r[start - 1];
              if (std::isalnum(static_cast<unsigned char>(before)) ||
                  before == '_') {
                return false;
              }
            }
            return true;
          };
          static constexpr std::string_view kMathFunctions[] = {
              "calc",  "min",  "max",  "clamp",     "round",    "abs",
              "sign",  "mod",  "rem",  "sin",       "cos",      "tan",
              "asin",  "acos", "atan", "atan2",     "pow",      "sqrt",
              "hypot", "log",  "exp",  "calc-size", "progress", "random"};
          if (cpd == 0) {
            for (std::string_view fn : kMathFunctions) {
              if (is_math_fn(fn)) {
                cpd = pd;
                break;
              }
            }
          }
          r += c;
          ++i;
          continue;
        }
        if (c == ')') {
          if (pd == cpd) cpd = 0;
          if (pd > 0) --pd;
          r += c;
          ++i;
          continue;
        }
        if (c == ':' && AtCustomPropertyColon(r)) {
          // Entering a custom-property value (see AtCustomPropertyColon).
          in_custom = true;
          custom_bd = 0;
          r += c;
          ++i;
          continue;
        }
        if (c == ' ') {
          if (i + 1 >= input.size()) {
            ++i;
            continue;
          }
          char pc = r.empty() ? '\0' : r.back();
          char nc = input[i + 1];
          if (cpd > 0) {
            // Inside calc(): only preserve spaces around binary + and -.
            // Spaces around * and / can be removed.
            if (nc == '+' || nc == '-') {
              if (pc != '(' && pc != ',') {
                r += c;
                ++i;
                continue;
              }
            }
            if (pc == '+' || pc == '-') {
              if (r.size() >= 2 && r[r.size() - 2] != '(' &&
                  r[r.size() - 2] != ',') {
                r += c;
                ++i;
                continue;
              }
            }
            // Inside calc(), remove spaces around * and / — but never
            // when the removal would glue them into a comment token
            // ('/' + '*' or '*' + '/'): a manufactured "/*" is honored
            // by pass 2's comment stripping and truncates the sheet
            // (#1159).
            if (nc == '*' || nc == '/' || pc == '*' || pc == '/') {
              if (!((pc == '/' && nc == '*') || (pc == '*' && nc == '/'))) {
                ++i;
                continue;
              }
            }
          }
          // Never strip spaces before ':'.  In selector context the space is a
          // descendant combinator (.prose :where(h2)); removing it changes
          // semantics.  In declaration context the space is harmless but
          // virtually never emitted by build tools, so preserving it costs
          // nothing in practice.
          if (nc == ':') {
            r += c;
            ++i;
            continue;
          }
          if (CanRemoveSpaceBefore(nc)) {
            ++i;
            continue;
          }
          if (!r.empty() && CanRemoveSpaceAfter(pc)) {
            ++i;
            continue;
          }
          if (nc == '!') {
            ++i;
            continue;
          }
          r += c;
          ++i;
          continue;
        }
        r += c;
        ++i;
        break;
      }
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
        // Mirrors Phase 1's url state, including the parity-guarded
        // trailing-space trim before ')' (#1154).  The trim is
        // defensive here: Phase 1 already removed every genuine
        // trailing space Phase 2 can see in-pipeline, so the pop never
        // fires — the guard exists to keep the two phases byte-aligned.
        if (c == ')') {
          while (!r.empty() && r.back() == ' ') {
            size_t bs = 0;
            while (bs + 1 < r.size() && r[r.size() - 2 - bs] == '\\') ++bs;
            if (bs % 2 != 0) break;
            r.pop_back();
          }
          r += c;
          state = State::kNormal;
          ++i;
          continue;
        }
        if (c == '\\' && i + 1 < input.size()) {
          r += c;
          r += input[i + 1];
          i += 2;
          continue;
        }
        r += c;
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

}  // namespace
}  // namespace pagespeed::css

#endif  // PAGESPEED_LIB_CSS_CSS_PHASES_H_
