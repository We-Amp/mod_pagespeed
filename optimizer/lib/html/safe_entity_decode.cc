// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

#include "lib/html/safe_entity_decode.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <string>
#include <string_view>

namespace net_instaweb {

namespace {

// Length (in bytes) of the longest well-formed-UTF-8 PREFIX of `s`. `s` is
// well-formed iff this equals s.size(); otherwise the returned length is the
// byte offset of the first invalid/incomplete sequence, so s.substr(0, len) is
// always well-formed. Rejects lone continuation bytes, invalid lead bytes (e.g.
// a lone Latin-1 0xB7 from &middot;), truncated/overlong sequences, surrogates,
// and out-of-range code points.
std::size_t WellFormedUtf8PrefixLen(std::string_view s) {
  std::size_t i = 0;
  const std::size_t n = s.size();
  while (i < n) {
    const unsigned char c = static_cast<unsigned char>(s[i]);
    std::size_t cont;  // number of trailing continuation bytes expected
    unsigned int cp;
    if (c < 0x80) {
      ++i;
      continue;
    } else if ((c & 0xE0) == 0xC0) {
      cont = 1;
      cp = c & 0x1F;
    } else if ((c & 0xF0) == 0xE0) {
      cont = 2;
      cp = c & 0x0F;
    } else if ((c & 0xF8) == 0xF0) {
      cont = 3;
      cp = c & 0x07;
    } else {
      return i;  // lone continuation byte or invalid lead byte.
    }
    if (i + cont >= n) return i;  // truncated sequence (e.g. byte-cap split).
    for (std::size_t k = 1; k <= cont; ++k) {
      const unsigned char cc = static_cast<unsigned char>(s[i + k]);
      if ((cc & 0xC0) != 0x80) return i;  // not a continuation byte.
      cp = (cp << 6) | (cc & 0x3F);
    }
    if ((cont == 1 && cp < 0x80) || (cont == 2 && cp < 0x800) ||
        (cont == 3 && cp < 0x10000)) {
      return i;  // overlong encoding.
    }
    if (cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) {
      return i;  // out of range or UTF-16 surrogate.
    }
    i += cont + 1;
  }
  return n;
}

// Length of the well-formed UTF-8 sequence starting at s[i], 0 when s[i]
// does not start one. *truncated is set when the bytes from s[i] to the end
// are a correct but incomplete start of a sequence.
std::size_t Utf8SequenceLen(std::string_view s, std::size_t i,
                            bool* truncated) {
  *truncated = false;
  const unsigned char c = static_cast<unsigned char>(s[i]);
  if (c < 0x80) return 1;
  std::size_t cont;
  unsigned int lo = 0x80;  // allowed range of the first continuation byte
  unsigned int hi = 0xBF;
  if (c >= 0xC2 && c <= 0xDF) {
    cont = 1;
  } else if (c >= 0xE0 && c <= 0xEF) {
    cont = 2;
    if (c == 0xE0) lo = 0xA0;  // no overlong form
    if (c == 0xED) hi = 0x9F;  // no UTF-16 surrogate
  } else if (c >= 0xF0 && c <= 0xF4) {
    cont = 3;
    if (c == 0xF0) lo = 0x90;  // no overlong form
    if (c == 0xF4) hi = 0x8F;  // nothing above U+10FFFF
  } else {
    return 0;  // continuation byte, C0/C1 or F5..FF: never a lead byte
  }
  for (std::size_t k = 1; k <= cont; ++k) {
    if (i + k >= s.size()) {
      *truncated = true;
      return 0;
    }
    const unsigned int cc = static_cast<unsigned char>(s[i + k]);
    const unsigned int min = k == 1 ? lo : 0x80;
    const unsigned int max = k == 1 ? hi : 0xBF;
    if (cc < min || cc > max) return 0;
  }
  return cont + 1;
}

struct NamedEntity {
  std::string_view name;  // without the leading '&' and the trailing ';'
  std::uint32_t cp1;
  std::uint32_t cp2;  // 0 when the reference names one code point
  int legacy;         // 1 when the name is also recognized without ';'
};

constexpr NamedEntity kNamedEntities[] = {
#include "lib/html/html5_named_entities.inc"
};

// Longer than the longest name in the table ("CounterClockwiseContourIntegral",
// 31 bytes).
constexpr std::size_t kMaxEntityNameLen = 32;

// Longest name the standard accepts without a ';' ("frac12", "middot"),
// derived from the table at compile time.
constexpr std::size_t MaxLegacyNameLen() {
  std::size_t n = 0;
  for (const NamedEntity& e : kNamedEntities) {
    if (e.legacy != 0 && e.name.size() > n) n = e.name.size();
  }
  return n;
}
constexpr std::size_t kMaxLegacyNameLen = MaxLegacyNameLen();
static_assert(kMaxLegacyNameLen == 6, "the HTML Standard's legacy names");

const NamedEntity* FindEntity(std::string_view name) {
  const auto* it = std::lower_bound(
      std::begin(kNamedEntities), std::end(kNamedEntities), name,
      [](const NamedEntity& e, std::string_view n) { return e.name < n; });
  if (it == std::end(kNamedEntities) || it->name != name) return nullptr;
  return it;
}

void AppendUtf8(std::uint32_t cp, std::string* out) {
  if (cp < 0x80) {
    out->push_back(static_cast<char>(cp));
  } else if (cp < 0x800) {
    out->push_back(static_cast<char>(0xC0 | (cp >> 6)));
    out->push_back(static_cast<char>(0x80 | (cp & 0x3F)));
  } else if (cp < 0x10000) {
    out->push_back(static_cast<char>(0xE0 | (cp >> 12)));
    out->push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
    out->push_back(static_cast<char>(0x80 | (cp & 0x3F)));
  } else {
    out->push_back(static_cast<char>(0xF0 | (cp >> 18)));
    out->push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
    out->push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
    out->push_back(static_cast<char>(0x80 | (cp & 0x3F)));
  }
}

// The code point a numeric character reference stands for, as the HTML
// Standard maps it: NUL, surrogates and values past U+10FFFF become U+FFFD,
// and 0x80..0x9F are read as windows-1252. Control characters other than tab,
// line feed, form feed and carriage return also become U+FFFD.
std::uint32_t NumericReferenceCodePoint(std::uint32_t v) {
  static constexpr std::uint32_t kWindows1252[32] = {
      0x20AC, 0x81,   0x201A, 0x0192, 0x201E, 0x2026, 0x2020, 0x2021,
      0x02C6, 0x2030, 0x0160, 0x2039, 0x0152, 0x8D,   0x017D, 0x8F,
      0x90,   0x2018, 0x2019, 0x201C, 0x201D, 0x2022, 0x2013, 0x2014,
      0x02DC, 0x2122, 0x0161, 0x203A, 0x0153, 0x9D,   0x017E, 0x0178};
  if (v == 0 || v > 0x10FFFF || (v >= 0xD800 && v <= 0xDFFF)) return 0xFFFD;
  if (v >= 0x80 && v <= 0x9F) v = kWindows1252[v - 0x80];
  if ((v < 0x20 && v != '\t' && v != '\n' && v != '\f' && v != '\r') ||
      v == 0x7F || (v >= 0x80 && v <= 0x9F)) {
    return 0xFFFD;
  }
  return v;
}

bool IsAsciiAlnum(char c) {
  return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
         (c >= '0' && c <= '9');
}

// Decode the character reference at s[i] (s[i] == '&'). Appends its text to
// *out and returns the number of bytes it used, or 0 when s[i] does not start
// a reference this decoder knows (the '&' is then ordinary text).
std::size_t DecodeReferenceAt(std::string_view s, std::size_t i,
                              std::string* out) {
  const std::size_t n = s.size();
  if (i + 1 < n && s[i + 1] == '#') {
    std::size_t j = i + 2;
    const bool hex = j < n && (s[j] == 'x' || s[j] == 'X');
    if (hex) ++j;
    const std::size_t digits = j;
    std::uint32_t value = 0;
    for (; j < n; ++j) {
      const char c = s[j];
      std::uint32_t d;
      if (c >= '0' && c <= '9') {
        d = static_cast<std::uint32_t>(c - '0');
      } else if (hex && c >= 'a' && c <= 'f') {
        d = static_cast<std::uint32_t>(c - 'a' + 10);
      } else if (hex && c >= 'A' && c <= 'F') {
        d = static_cast<std::uint32_t>(c - 'A' + 10);
      } else {
        break;
      }
      // Saturate past the Unicode range: the result is U+FFFD either way.
      value = value > 0x10FFFF ? value : value * (hex ? 16 : 10) + d;
    }
    if (j == digits) return 0;  // "&#" or "&#x" with no digits: plain text
    if (j < n && s[j] == ';') ++j;
    AppendUtf8(NumericReferenceCodePoint(value), out);
    return j - i;
  }
  std::size_t j = i + 1;
  while (j < n && j - (i + 1) < kMaxEntityNameLen + 1 && IsAsciiAlnum(s[j])) {
    ++j;
  }
  const std::string_view name = s.substr(i + 1, j - (i + 1));
  if (name.empty()) return 0;
  if (j < n && s[j] == ';') {
    if (const NamedEntity* e = FindEntity(name)) {
      AppendUtf8(e->cp1, out);
      if (e->cp2 != 0) AppendUtf8(e->cp2, out);
      return j + 1 - i;
    }
  }
  // Without a ';' only the historical names are references, matched as the
  // longest one that prefixes the name ("&copy2026" is "(c)2026").
  for (std::size_t len = std::min(name.size(), kMaxLegacyNameLen); len >= 2;
       --len) {
    const NamedEntity* e = FindEntity(name.substr(0, len));
    if (e != nullptr && e->legacy != 0) {
      AppendUtf8(e->cp1, out);
      if (e->cp2 != 0) AppendUtf8(e->cp2, out);
      return len + 1;
    }
  }
  return 0;
}

}  // namespace

bool IsWellFormedUtf8(std::string_view s) {
  return WellFormedUtf8PrefixLen(s) == s.size();
}

bool IsHtml5EntityName(std::string_view name) {
  return FindEntity(name) != nullptr;
}

std::string ToWellFormedUtf8(std::string_view s, bool drop_cut_tail) {
  if (IsWellFormedUtf8(s)) return std::string(s);
  std::string out;
  out.reserve(s.size() + 8);
  std::size_t i = 0;
  while (i < s.size()) {
    bool truncated = false;
    const std::size_t len = Utf8SequenceLen(s, i, &truncated);
    if (len > 0) {
      out.append(s.substr(i, len));
      i += len;
    } else if (truncated && drop_cut_tail) {
      break;  // a sequence cut off at the end by a byte cap: dropped
    } else {
      out.append("\xEF\xBF\xBD");  // U+FFFD REPLACEMENT CHARACTER
      ++i;
    }
  }
  return out;
}

void FoldNbsp(std::string* s) {
  std::size_t w = 0;
  for (std::size_t r = 0; r < s->size(); ++r) {
    const unsigned char c = static_cast<unsigned char>((*s)[r]);
    if (c == 0xC2 && r + 1 < s->size() &&
        static_cast<unsigned char>((*s)[r + 1]) == 0xA0) {
      (*s)[w++] = ' ';
      ++r;
    } else {
      (*s)[w++] = static_cast<char>(c);
    }
  }
  s->resize(w);
}

std::string SafeDecodeHtmlEntities(std::string_view s, bool value_was_capped) {
  std::string out;
  if (s.find('&') == std::string_view::npos) {
    out = ToWellFormedUtf8(s, value_was_capped);
  } else {
    // Reference by reference: a reference this decoder does not know stays as
    // written, and never stops the rest of the text from being decoded.
    std::string decoded;
    decoded.reserve(s.size());
    std::size_t i = 0;
    while (i < s.size()) {
      if (s[i] == '&') {
        const std::size_t used = DecodeReferenceAt(s, i, &decoded);
        if (used > 0) {
          i += used;
          continue;
        }
      }
      decoded.push_back(s[i]);
      ++i;
    }
    // Decoded references are well-formed; input bytes that are not UTF-8
    // are repaired here.
    out = ToWellFormedUtf8(decoded, value_was_capped);
  }
  FoldNbsp(&out);
  return out;
}

}  // namespace net_instaweb
