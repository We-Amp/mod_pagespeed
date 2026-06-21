// Copyright 2026 We-Amp B.V.
//
// Licensed under the Business Source License 1.1 (BUSL-1.1).
// See the LICENSE file in the repository root for terms.
//
// Author: agentpass scaffolding (A3 — RSL-CAP token enforcement)

#ifndef PAGESPEED_KERNEL_WEBBOTAUTH_RSL_CAP_TOKEN_H_
#define PAGESPEED_KERNEL_WEBBOTAUTH_RSL_CAP_TOKEN_H_

#include <vector>

#include "pagespeed/kernel/base/basictypes.h"
#include "pagespeed/kernel/base/string.h"
#include "pagespeed/kernel/base/string_util.h"

namespace net_instaweb {
namespace webbotauth {

// A parsed RSL-CAP capability token.
//
// Wire format (compact, RSL-CAP-profiled JWT-like, carried in the request as
//   Authorization: License <token>):
//
//   base64url(header) "." base64url(payload) "." base64url(ed25519-sig)
//
//   header  = {"alg":"EdDSA","typ":"RSL-CAP","kid":"<issuer-key-id>"}
//   payload = {"iss":..,"sub":..,"exp":<unix>,"iat":<unix>,"lic":[..],"scope":[..]}
//   sig     = Ed25519 over the ASCII signing input
//             base64url(header) "." base64url(payload)   (byte-exact, no padding)
//
// This is a DEFENSIVE, verification-only artifact. It carries NO transaction
// code, NO settlement instruction, and NO payment data — it is purely a signed
// statement of licensed identity + granted licenses/scopes.
struct RslCapToken {
  // ---- header fields ----
  GoogleString typ;  // MUST equal "RSL-CAP"
  GoogleString alg;  // MUST equal "EdDSA"
  GoogleString kid;  // issuer key id, resolved via a KeyDirectory

  // ---- payload fields ----
  GoogleString iss;    // issuer
  GoogleString sub;    // subject
  int64 exp = 0;       // expiry, unix seconds; MUST be present (> 0)
  int64 iat = 0;       // issued-at, unix seconds (optional)
  StringVector lic;    // granted license ids
  StringVector scope;  // granted scopes

  // ---- raw decoded segments (for signature verification) ----
  // The ASCII signing input == the original received
  // base64url(header) "." base64url(payload) bytes, preserved verbatim to
  // avoid canonicalization drift (we never re-encode for verification).
  GoogleString signing_input;
  GoogleString signature_bytes;  // raw decoded Ed25519 signature (expect 64B)

  // True only after a successful Parse().
  bool parsed = false;
};

// Result of parsing the compact token string (before any crypto).
enum class RslCapParseStatus {
  kOk,
  kMalformed,  // not 3 dot-parts, bad base64url, bad header/payload shape,
               // typ != "RSL-CAP", alg != "EdDSA", or missing/invalid exp
};

// Parses `compact` (the value AFTER the "License " scheme prefix is stripped)
// into *out. Performs:
//   1. split on '.' into EXACTLY 3 parts (else kMalformed),
//   2. base64url-decode header/payload/sig (any failure => kMalformed),
//   3. strict header field extraction; require typ=="RSL-CAP" AND alg=="EdDSA"
//      (exact string match — defeats generic-JWT / alg-confusion incl.
//      "none"/"HS256"); extract kid,
//   4. strict payload field extraction (iss/sub/exp/iat/lic[]/scope[]); require
//      an integer exp present.
//
// On kOk, out->signing_input and out->signature_bytes are set so the caller can
// run ed25519_verify before trusting exp/lic/scope. Does NO crypto and NO
// network I/O.
RslCapParseStatus ParseRslCapToken(StringPiece compact, RslCapToken* out);

}  // namespace webbotauth
}  // namespace net_instaweb

#endif  // PAGESPEED_KERNEL_WEBBOTAUTH_RSL_CAP_TOKEN_H_
