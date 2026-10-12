// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

#include "pagespeed/kernel/packs/entity_decode.h"

#include <cstddef>
#include <string>
#include <string_view>

#include "pagespeed/kernel/html/html_keywords.h"

namespace pagespeed::packs {

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

}  // namespace

bool IsWellFormedUtf8(std::string_view s) {
  return WellFormedUtf8PrefixLen(s) == s.size();
}

void FoldLatin1Nbsp(std::string* s) {
  for (char& c : *s) {
    if (static_cast<unsigned char>(c) == 0xA0) c = ' ';
  }
}

std::string SafeDecodeHtmlEntities(std::string_view s) {
  // 1. Fast path: nothing to decode; preserve the bytes (still honoring the
  //    "result is always well-formed UTF-8" contract). Callers may byte-cap the
  //    raw value BEFORE this decode (a field byte limit),
  //    which can split a trailing multibyte sequence; trim that incomplete tail
  //    so a lone invalid byte is never returned.
  if (s.find('&') == std::string_view::npos) {
    return std::string(s.substr(0, WellFormedUtf8PrefixLen(s)));
  }
  std::string buf;
  bool decoding_error = false;
  StringPiece out =
      net_instaweb::HtmlKeywords::Unescape(s, &buf, &decoding_error);
  // 2. Decode failed (non-ASCII byte / multi-byte entity): keep the original.
  if (decoding_error) {
    return std::string(s);
  }
  // 3. Decode succeeded: fold NBSP, then validate UTF-8.
  std::string decoded(out.data(), out.size());
  FoldLatin1Nbsp(&decoded);
  if (!IsWellFormedUtf8(decoded)) {
    // A lone Latin-1 byte (e.g. &middot; -> 0xB7) survived the decode. Never
    // ship an invalid byte: fall back to the original entity text.
    return std::string(s);
  }
  return decoded;
}

}  // namespace pagespeed::packs
