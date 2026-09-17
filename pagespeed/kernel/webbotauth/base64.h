// Copyright 2026 We-Amp B.V.
// Licensed under the Apache License, Version 2.0 (the "License").
//
// Self-contained, dependency-free base64 decoders for the Web-Bot-Auth
// verifier.  This is plain RFC 4648 encoding (NOT cryptography -- the Ed25519
// primitive is reused from @ed25519, see verifier.cc).  We implement it here,
// rather than depending on pagespeed/kernel/util:base64_util, to keep the
// hermetic unit-test dependency closure minimal (the util base64 pulls a
// Chromium transitive) and to have an unambiguous, locally-tested URL-safe
// decoder for JWK `x` values and structured-field byte sequences.
//
// Both decoders are tolerant of missing padding (JWK base64url is unpadded).
// They reject any out-of-alphabet character and never read out of bounds.

#ifndef PAGESPEED_KERNEL_WEBBOTAUTH_BASE64_H_
#define PAGESPEED_KERNEL_WEBBOTAUTH_BASE64_H_

#include "pagespeed/kernel/base/string.h"
#include "pagespeed/kernel/base/string_util.h"

namespace net_instaweb {
namespace webbotauth {

// Decode standard base64 (alphabet '+' '/'), padding optional. Returns true on
// success, false on any invalid character. `out` receives raw bytes.
bool Base64Decode(StringPiece in, GoogleString* out);

// Decode URL-safe base64url (alphabet '-' '_'), padding optional (JWK style).
// Returns true on success, false on any invalid character.
bool Base64UrlDecode(StringPiece in, GoogleString* out);

}  // namespace webbotauth
}  // namespace net_instaweb

#endif  // PAGESPEED_KERNEL_WEBBOTAUTH_BASE64_H_
