// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 We-Amp B.V.

#include "pagespeed/kernel/html/inline_style_text.h"

#include <algorithm>
#include <cstddef>
#include <iterator>

namespace net_instaweb {

const char kInlineStyleScanVersion[] = "s1";

namespace {

const char kEscapedLessThan[] = "\\3c ";
const char kRefusedReason[] = "CSS could be read as markup";
const char kRefusedEncodingReason[] =
    "CSS holds a less-than sign and is not valid UTF-8";

// The bytes a kept '<' may be followed by.
bool AllowedAfterLessThan(char c) {
  switch (c) {
    case ' ':
    case '\t':
    case '\n':
    case '\r':
    case '\f':
    case '=':
    case '.':
      return true;
    default:
      return c >= '0' && c <= '9';
  }
}

// ASCII whitespace as the Encoding Standard trims it from a label.
bool IsAsciiWhitespace(char c) {
  return c == ' ' || c == '\t' || c == '\n' || c == '\f' || c == '\r';
}

bool IsCssNewline(char c) { return c == '\n' || c == '\r' || c == '\f'; }

bool IsCssWhitespace(char c) {
  return c == ' ' || c == '\t' || IsCssNewline(c);
}

// A byte that can be part of a CSS identifier (or an escape in one), so that
// "url(" right after it is not the start of a url token.
bool IsIdentifierByte(char c) {
  const unsigned char u = static_cast<unsigned char>(c);
  return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
         (c >= '0' && c <= '9') || c == '_' || c == '-' || c == '\\' ||
         u >= 0x80;
}

bool HasNonAscii(StringPiece text) {
  for (char c : text) {
    if (static_cast<unsigned char>(c) >= 0x80) {
      return true;
    }
  }
  return false;
}

// Well-formed UTF-8 (RFC 3629: no overlong forms, no surrogates, nothing
// above U+10FFFF).
bool IsValidUtf8(StringPiece text) {
  const size_t n = text.size();
  size_t i = 0;
  while (i < n) {
    const unsigned char c = static_cast<unsigned char>(text[i]);
    if (c < 0x80) {
      ++i;
      continue;
    }
    size_t len;
    unsigned char lo = 0x80, hi = 0xBF;  // bounds of the second byte
    if (c >= 0xC2 && c <= 0xDF) {
      len = 2;
    } else if (c >= 0xE0 && c <= 0xEF) {
      len = 3;
      if (c == 0xE0) lo = 0xA0;
      if (c == 0xED) hi = 0x9F;
    } else if (c >= 0xF0 && c <= 0xF4) {
      len = 4;
      if (c == 0xF0) lo = 0x90;
      if (c == 0xF4) hi = 0x8F;
    } else {
      return false;
    }
    if (n - i < len) {
      return false;
    }
    const unsigned char c1 = static_cast<unsigned char>(text[i + 1]);
    if (c1 < lo || c1 > hi) {
      return false;
    }
    for (size_t k = 2; k < len; ++k) {
      const unsigned char ck = static_cast<unsigned char>(text[i + k]);
      if (ck < 0x80 || ck > 0xBF) {
        return false;
      }
    }
    i += len;
  }
  return true;
}

// Whether "url(" starts at css[i] as the start of an unquoted url token.
bool StartsUnquotedUrl(StringPiece css, size_t i) {
  const size_t n = css.size();
  if (n - i < 4 || !StringCaseEqual(StringPiece(css.data() + i, 4), "url(")) {
    return false;
  }
  if (i > 0 && IsIdentifierByte(css[i - 1])) {
    return false;
  }
  size_t j = i + 4;
  while (j < n && IsCssWhitespace(css[j])) {
    ++j;
  }
  return j >= n || (css[j] != '"' && css[j] != '\'');
}

// The kHtml rewrite. Returns false when the CSS is refused.
bool PrepareHtml(StringPiece css, GoogleString* out, bool* changed) {
  enum class State { kNormal, kString, kComment, kUrl };
  State state = State::kNormal;
  char quote = '\0';
  const size_t n = css.size();
  out->reserve(n);
  for (size_t i = 0; i < n; ++i) {
    const char c = css[i];
    const bool has_next = i + 1 < n;
    const char next = has_next ? css[i + 1] : '\0';

    if (state == State::kComment) {
      if (c == '*' && next == '/') {
        out->append("*/");
        ++i;
        state = State::kNormal;
      } else if (c == '<') {
        out->append(kEscapedLessThan);
        *changed = true;
      } else {
        out->push_back(c);
      }
      continue;
    }

    if (c == '\\') {
      // An escape: the backslash and the byte after it go together, so an
      // escaped quote or parenthesis neither opens nor closes anything.
      if (next == '<' && has_next) {
        out->append(kEscapedLessThan);
        *changed = true;
        ++i;
        continue;
      }
      if (has_next && (state == State::kString || !IsCssNewline(next))) {
        // In a string a backslash before a newline continues the string;
        // CR LF is one newline.
        size_t len = 2;
        if (next == '\r' && i + 2 < n && css[i + 2] == '\n') {
          len = 3;
        }
        out->append(css.data() + i, len);
        i += len - 1;
        continue;
      }
      out->push_back(c);
      continue;
    }

    if (c == '<') {
      if (state != State::kNormal) {
        out->append(kEscapedLessThan);
        *changed = true;
        continue;
      }
      if (!has_next || !AllowedAfterLessThan(next)) {
        return false;
      }
      out->push_back(c);
      continue;
    }

    switch (state) {
      case State::kString:
        // A string ends at its quote or, as a bad string, at a newline.
        if (c == quote || IsCssNewline(c)) {
          state = State::kNormal;
        }
        break;
      case State::kUrl:
        if (c == ')') {
          state = State::kNormal;
        }
        break;
      case State::kNormal:
        if (c == '/' && next == '*') {
          out->append("/*");
          ++i;
          state = State::kComment;
          continue;
        }
        if (c == '"' || c == '\'') {
          quote = c;
          state = State::kString;
        } else if ((c == 'u' || c == 'U') && StartsUnquotedUrl(css, i)) {
          out->append(css.data() + i, 4);
          i += 3;
          state = State::kUrl;
          continue;
        }
        break;
      case State::kComment:
        break;  // Handled above.
    }
    out->push_back(c);
  }
  return true;
}

// The labels the Encoding Standard (encoding.spec.whatwg.org) gives the
// encodings UTF-8, windows-1250 to windows-1258 and ISO-8859-x, sorted.
// Any other label, including one no browser knows, gets kStrict.
const char* const kCharsetLabels[] = {
    "ansi_x3.4-1968",
    "arabic",
    "ascii",
    "asmo-708",
    "cp1250",
    "cp1251",
    "cp1252",
    "cp1253",
    "cp1254",
    "cp1255",
    "cp1256",
    "cp1257",
    "cp1258",
    "cp819",
    "csiso88596e",
    "csiso88596i",
    "csiso88598e",
    "csiso88598i",
    "csisolatin1",
    "csisolatin2",
    "csisolatin3",
    "csisolatin4",
    "csisolatin5",
    "csisolatin6",
    "csisolatin9",
    "csisolatinarabic",
    "csisolatincyrillic",
    "csisolatingreek",
    "csisolatinhebrew",
    "cyrillic",
    "ecma-114",
    "ecma-118",
    "elot_928",
    "greek",
    "greek8",
    "hebrew",
    "ibm819",
    "iso-8859-1",
    "iso-8859-10",
    "iso-8859-13",
    "iso-8859-14",
    "iso-8859-15",
    "iso-8859-16",
    "iso-8859-2",
    "iso-8859-3",
    "iso-8859-4",
    "iso-8859-5",
    "iso-8859-6",
    "iso-8859-6-e",
    "iso-8859-6-i",
    "iso-8859-7",
    "iso-8859-8",
    "iso-8859-8-e",
    "iso-8859-8-i",
    "iso-8859-9",
    "iso-ir-100",
    "iso-ir-101",
    "iso-ir-109",
    "iso-ir-110",
    "iso-ir-126",
    "iso-ir-127",
    "iso-ir-138",
    "iso-ir-144",
    "iso-ir-148",
    "iso-ir-157",
    "iso8859-1",
    "iso8859-10",
    "iso8859-13",
    "iso8859-14",
    "iso8859-15",
    "iso8859-2",
    "iso8859-3",
    "iso8859-4",
    "iso8859-5",
    "iso8859-6",
    "iso8859-7",
    "iso8859-8",
    "iso8859-9",
    "iso88591",
    "iso885910",
    "iso885913",
    "iso885914",
    "iso885915",
    "iso88592",
    "iso88593",
    "iso88594",
    "iso88595",
    "iso88596",
    "iso88597",
    "iso88598",
    "iso88599",
    "iso_8859-1",
    "iso_8859-15",
    "iso_8859-1:1987",
    "iso_8859-2",
    "iso_8859-2:1987",
    "iso_8859-3",
    "iso_8859-3:1988",
    "iso_8859-4",
    "iso_8859-4:1988",
    "iso_8859-5",
    "iso_8859-5:1988",
    "iso_8859-6",
    "iso_8859-6:1987",
    "iso_8859-7",
    "iso_8859-7:1987",
    "iso_8859-8",
    "iso_8859-8:1988",
    "iso_8859-9",
    "iso_8859-9:1989",
    "l1",
    "l2",
    "l3",
    "l4",
    "l5",
    "l6",
    "l9",
    "latin1",
    "latin2",
    "latin3",
    "latin4",
    "latin5",
    "latin6",
    "logical",
    "sun_eu_greek",
    "unicode-1-1-utf-8",
    "unicode11utf8",
    "unicode20utf8",
    "us-ascii",
    "utf-8",
    "utf8",
    "visual",
    "windows-1250",
    "windows-1251",
    "windows-1252",
    "windows-1253",
    "windows-1254",
    "windows-1255",
    "windows-1256",
    "windows-1257",
    "windows-1258",
    "x-cp1250",
    "x-cp1251",
    "x-cp1252",
    "x-cp1253",
    "x-cp1254",
    "x-cp1255",
    "x-cp1256",
    "x-cp1257",
    "x-cp1258",
    "x-unicode20utf8",
};

}  // namespace

const char* InlineStyleModeKey(InlineStyleMode mode) {
  return mode == InlineStyleMode::kHtml ? "h" : "x";
}

InlineStyleMode InlineStyleModeForCharset(StringPiece charset) {
  // As the Encoding Standard reads a label: without leading and trailing ASCII
  // whitespace, and ASCII case-insensitively.
  while (!charset.empty() && IsAsciiWhitespace(charset.front())) {
    charset.remove_prefix(1);
  }
  while (!charset.empty() && IsAsciiWhitespace(charset.back())) {
    charset.remove_suffix(1);
  }
  GoogleString label(charset.data(), charset.size());
  LowerString(&label);
  const bool known = std::binary_search(
      std::begin(kCharsetLabels), std::end(kCharsetLabels), label,
      [](StringPiece a, StringPiece b) { return a < b; });
  return known ? InlineStyleMode::kHtml : InlineStyleMode::kStrict;
}

StringPieceVector InlineStyleCharsetLabelsForTesting() {
  return StringPieceVector(std::begin(kCharsetLabels),
                           std::end(kCharsetLabels));
}

bool IsSafeInlineStyleText(StringPiece text, InlineStyleMode mode) {
  const size_t n = text.size();
  for (size_t i = 0; i < n; ++i) {
    if (text[i] != '<') {
      continue;
    }
    if (mode != InlineStyleMode::kHtml || i + 1 >= n ||
        !AllowedAfterLessThan(text[i + 1])) {
      return false;
    }
  }
  return true;
}

bool InlineStyleText::Prepare(StringPiece css, InlineStyleMode mode,
                              InlineStyleText* out, const char** reason) {
  out->text_.clear();
  out->changed_ = false;
  const char* why = kRefusedReason;
  GoogleString text;
  bool changed = false;
  bool ok;
  if (css.find('<') == StringPiece::npos) {
    // Nothing to check or rewrite in either mode.
    css.CopyToString(&text);
    ok = true;
  } else if (mode != InlineStyleMode::kHtml) {
    ok = false;
  } else if (HasNonAscii(css) && !IsValidUtf8(css)) {
    // CSS that holds a '<' is written only when it is ASCII or valid UTF-8:
    // the rewrite reads the text one byte at a time, which matches how a
    // browser reads text in those encodings.
    why = kRefusedEncodingReason;
    ok = false;
  } else {
    ok = PrepareHtml(css, &text, &changed);
  }
  // The postcondition, checked in every build: whatever the rewrite above
  // did, text that fails it is never returned.
  if (ok && !IsSafeInlineStyleText(text, mode)) {
    ok = false;
  }
  const PrepareObserver observer =
      prepare_observer_.load(std::memory_order_relaxed);
  if (observer != nullptr) {
    observer(css, mode, ok ? &text : nullptr);
  }
  if (!ok) {
    if (reason != nullptr) {
      *reason = why;
    }
    return false;
  }
  out->text_.swap(text);
  out->changed_ = changed;
  return true;
}

std::atomic<InlineStyleText::PrepareObserver>
    InlineStyleText::prepare_observer_{nullptr};

void InlineStyleText::set_prepare_observer_for_testing(
    PrepareObserver observer) {
  prepare_observer_.store(observer, std::memory_order_relaxed);
}

}  // namespace net_instaweb
