// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.
//
// Ported from mod_pagespeed
// pagespeed/kernel/webbotauth/rsl_cap_token.h @
// f6c24e8d34ba0c58acb22d626f6213d666d73e41 (2026-07-04; AgentPass A3).
// Keep logic in sync manually; the token wire format is frozen. Part of the
// same manual-sync discipline as the rest of src/crypto/webbotauth.
//
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
// code, NO settlement instruction, and NO payment data -- it is purely a signed
// statement of licensed identity + granted licenses/scopes.

#ifndef PAGESPEED_SRC_CRYPTO_WEBBOTAUTH_RSL_CAP_TOKEN_H_
#define PAGESPEED_SRC_CRYPTO_WEBBOTAUTH_RSL_CAP_TOKEN_H_

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace pagespeed {
namespace webbotauth {

// A parsed RSL-CAP capability token.
struct RslCapToken {
  // ---- header fields ----
  std::string typ;  // MUST equal "RSL-CAP"
  std::string alg;  // MUST equal "EdDSA"
  std::string kid;  // issuer key id, resolved via a KeyDirectory

  // ---- payload fields ----
  std::string iss;               // issuer
  std::string sub;               // subject
  int64_t exp = 0;               // expiry, unix seconds; MUST be present (> 0)
  int64_t iat = 0;               // issued-at, unix seconds (optional)
  std::vector<std::string> lic;  // granted license ids
  std::vector<std::string> scope;  // granted scopes

  // ---- raw decoded segments (for signature verification) ----
  // The ASCII signing input == the original received
  // base64url(header) "." base64url(payload) bytes, preserved verbatim to
  // avoid canonicalization drift (we never re-encode for verification).
  std::string signing_input;
  std::string signature_bytes;  // raw decoded Ed25519 signature (expect 64B)

  // True only after a successful Parse().
  bool parsed = false;
};

// Result of parsing the compact token string (before any crypto).
enum class RslCapParseStatus : std::uint8_t {
  kOk,
  kMalformed,  // not 3 dot-parts, bad base64url, bad header/payload shape,
               // typ != "RSL-CAP", alg != "EdDSA", or missing/invalid exp
};

// Parses `compact` (the value AFTER the "License " scheme prefix is stripped)
// into *out. Performs:
//   1. split on '.' into EXACTLY 3 parts (else kMalformed),
//   2. base64url-decode header/payload/sig (any failure => kMalformed),
//   3. strict header field extraction; require typ=="RSL-CAP" AND alg=="EdDSA"
//      (exact string match -- defeats generic-JWT / alg-confusion incl.
//      "none"/"HS256"); extract kid,
//   4. strict payload field extraction (iss/sub/exp/iat/lic[]/scope[]); require
//      an integer exp present.
//
// On kOk, out->signing_input and out->signature_bytes are set so the caller can
// run ed25519_verify before trusting exp/lic/scope. Does NO crypto and NO
// network I/O.
RslCapParseStatus ParseRslCapToken(std::string_view compact, RslCapToken* out);

}  // namespace webbotauth
}  // namespace pagespeed

#endif  // PAGESPEED_SRC_CRYPTO_WEBBOTAUTH_RSL_CAP_TOKEN_H_
