// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.
//
// Ported from mod_pagespeed pagespeed/kernel/webbotauth/base64.cc
// @ 8e15a19ce (2026-07-02). Keep logic in sync manually; the RFC 9421/8941
// core is standards-frozen.

#include "src/crypto/webbotauth/base64.h"

#include <cstdint>
#include <string>
#include <string_view>

namespace pagespeed {
namespace webbotauth {

namespace {

// Returns the 6-bit value of a base64 character, or -1 if not in the alphabet.
// `url_safe` selects the '-'/'_' (62/63) variant vs '+'/'/'.
inline int DecodeChar(char c, bool url_safe) {
  if (c >= 'A' && c <= 'Z') return c - 'A';
  if (c >= 'a' && c <= 'z') return c - 'a' + 26;
  if (c >= '0' && c <= '9') return c - '0' + 52;
  if (url_safe) {
    if (c == '-') return 62;
    if (c == '_') return 63;
  } else {
    if (c == '+') return 62;
    if (c == '/') return 63;
  }
  return -1;
}

bool DecodeImpl(std::string_view in, bool url_safe, std::string* out) {
  out->clear();
  out->reserve(in.size() * 3 / 4 + 3);

  uint32_t buffer = 0;
  int bits = 0;
  int sextets = 0;  // count of significant (non-pad) base64 chars
  for (size_t i = 0; i < in.size(); ++i) {
    char c = in[i];
    if (c == '=') {
      // Padding: only valid at the tail; once seen, the rest must be padding.
      for (size_t j = i; j < in.size(); ++j) {
        if (in[j] != '=') return false;
      }
      break;
    }
    int v = DecodeChar(c, url_safe);
    if (v < 0) return false;
    buffer = (buffer << 6) | static_cast<uint32_t>(v);
    bits += 6;
    ++sextets;
    if (bits >= 8) {
      bits -= 8;
      out->push_back(static_cast<char>((buffer >> bits) & 0xFF));
    }
  }

  // A base64 group of length 1 (mod 4) is invalid (no whole byte encoded).
  if ((sextets % 4) == 1) return false;

  // Any leftover bits must be zero (canonical encoding); be lenient and accept
  // non-zero trailing bits rather than reject, but require they not encode an
  // extra dropped byte (the loop above already consumed all full bytes).
  return true;
}

}  // namespace

bool Base64Decode(std::string_view in, std::string* out) {
  return DecodeImpl(in, /*url_safe=*/false, out);
}

bool Base64UrlDecode(std::string_view in, std::string* out) {
  return DecodeImpl(in, /*url_safe=*/true, out);
}

}  // namespace webbotauth
}  // namespace pagespeed
