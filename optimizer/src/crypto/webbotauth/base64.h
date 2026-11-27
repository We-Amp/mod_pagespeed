// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.
//
// Ported from mod_pagespeed pagespeed/kernel/webbotauth/base64.h
// @ 8e15a19ce (2026-07-02). Keep logic in sync manually; the RFC 9421/8941
// core is standards-frozen.
//
// Self-contained, dependency-free base64 decoders for the Web-Bot-Auth
// verifier.  This is plain RFC 4648 encoding (NOT cryptography -- the Ed25519
// primitive is reused from @ed25519, see verifier.cc).  We implement it here
// to keep the hermetic unit-test dependency closure minimal and to have an
// unambiguous, locally-tested URL-safe decoder for JWK `x` values and
// structured-field byte sequences.
//
// Both decoders are tolerant of missing padding (JWK base64url is unpadded).
// They reject any out-of-alphabet character and never read out of bounds.

#ifndef PAGESPEED_SRC_CRYPTO_WEBBOTAUTH_BASE64_H_
#define PAGESPEED_SRC_CRYPTO_WEBBOTAUTH_BASE64_H_

#include <string>
#include <string_view>

namespace pagespeed {
namespace webbotauth {

// Decode standard base64 (alphabet '+' '/'), padding optional. Returns true on
// success, false on any invalid character. `out` receives raw bytes.
bool Base64Decode(std::string_view in, std::string* out);

// Decode URL-safe base64url (alphabet '-' '_'), padding optional (JWK style).
// Returns true on success, false on any invalid character.
bool Base64UrlDecode(std::string_view in, std::string* out);

}  // namespace webbotauth
}  // namespace pagespeed

#endif  // PAGESPEED_SRC_CRYPTO_WEBBOTAUTH_BASE64_H_
