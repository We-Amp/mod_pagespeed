// Copyright 2026 We-Amp B.V.
//
// Licensed under the Business Source License 1.1 (BUSL-1.1).
// See the LICENSE file in the repository root for terms.
//
// Author: agentpass scaffolding (A3 — RSL-CAP token enforcement)

#include "pagespeed/kernel/webbotauth/rsl_cap_validator.h"

#include <cstdint>

#include "ed25519.h"
#include "pagespeed/kernel/base/string.h"
#include "pagespeed/kernel/base/string_util.h"
#include "pagespeed/kernel/webbotauth/key_directory.h"
#include "pagespeed/kernel/webbotauth/rsl_cap_token.h"

namespace net_instaweb {
namespace webbotauth {

namespace {
// RFC 8032 Ed25519: 32-byte public key, 64-byte signature.
const size_t kEd25519PublicKeyBytes = 32;
const size_t kEd25519SignatureBytes = 64;

bool VectorContains(const StringVector& haystack, StringPiece needle) {
  for (const GoogleString& v : haystack) {
    if (StringPiece(v) == needle) {
      return true;
    }
  }
  return false;
}
}  // namespace

const char RslCapValidator::kSchemePrefix[] = "License ";

RslCapValidator::RslCapValidator(KeyDirectoryProvider* provider)
    : provider_(provider) {}

RslCapStatus RslCapValidator::Validate(StringPiece auth_header,
                                       StringPiece requested_license,
                                       StringPiece requested_scope,
                                       StringPiece directory_host,
                                       int64_t now_unix_sec,
                                       RslCapToken* out_token) const {
  RslCapToken local;
  RslCapToken* token = (out_token != nullptr) ? out_token : &local;
  *token = RslCapToken();

  // 1. Require the "License " scheme prefix; strip it to get the compact token.
  if (!HasPrefixString(auth_header, kSchemePrefix)) {
    return RslCapStatus::kNoToken;
  }
  StringPiece compact = auth_header;
  compact.remove_prefix(STATIC_STRLEN(kSchemePrefix));
  if (compact.empty()) {
    return RslCapStatus::kNoToken;
  }

  // 2-4. Parse + strict header check (typ=="RSL-CAP", alg=="EdDSA", kid). The
  //      parser also base64url-decodes all 3 segments and rejects alg-confusion
  //      (alg "none"/"HS256"/... never reaches any verify).
  if (ParseRslCapToken(compact, token) != RslCapParseStatus::kOk) {
    return RslCapStatus::kMalformed;
  }

  // 5. Resolve the issuer key by kid via the WS3 KeyDirectoryProvider. The
  //    provider may be a static trusted-key set and/or the SSRF-guarded
  //    NetFetchKeyDirectory (which only fetches when the operator-mapped host is
  //    on the DEFAULT-EMPTY allowlist; off-allowlist / empty => kError, NO
  //    network fetch). `directory_host` is operator-mapped, NOT request-derived
  //    (plane-split). Any non-kFound result (kError off-allowlist/SSRF, or
  //    kNotFound) => kUnknownIssuer.
  if (provider_ == nullptr) {
    return RslCapStatus::kUnknownIssuer;
  }
  KeyLookupResult key =
      provider_->GetKey(directory_host, token->kid, now_unix_sec);
  if (key.status != KeyLookupResult::kFound) {
    return RslCapStatus::kUnknownIssuer;
  }
  if (key.raw_key_32.size() != kEd25519PublicKeyBytes) {
    return RslCapStatus::kBadSignature;
  }

  // 6. Verify the Ed25519 signature over the ORIGINAL received signing input
  //    (never re-encoded). This runs BEFORE expiry/authorization so a tampered
  //    exp or lic[]/scope[] can never influence those checks. Same call shape
  //    as verifier.cc:115 (sig, msg, msg_len, pub) and license_v2.
  if (token->signature_bytes.size() != kEd25519SignatureBytes) {
    return RslCapStatus::kBadSignature;
  }
  int ok = ed25519_verify(
      reinterpret_cast<const unsigned char*>(token->signature_bytes.data()),
      reinterpret_cast<const unsigned char*>(token->signing_input.data()),
      token->signing_input.size(),
      reinterpret_cast<const unsigned char*>(key.raw_key_32.data()));
  if (ok != 1) {
    return RslCapStatus::kBadSignature;
  }

  // 7-8. Expiry. exp is unix seconds; compare against the supplied clock.
  if (token->exp <= now_unix_sec) {
    return RslCapStatus::kExpired;
  }

  // 9. Authorize: the requested license MUST be in lic[] AND the requested
  //    scope MUST be in scope[]. Empty requested fields are treated as
  //    "not granted" (fail-closed) to avoid an empty-request bypass.
  if (requested_license.empty() || requested_scope.empty()) {
    return RslCapStatus::kUnlicensed;
  }
  if (VectorContains(token->lic, requested_license) &&
      VectorContains(token->scope, requested_scope)) {
    return RslCapStatus::kAuthorized;
  }
  return RslCapStatus::kUnlicensed;
}

int RslCapStatusToHttpStatus(RslCapStatus status) {
  // No `default:` so a future enum value is caught by -Werror=switch; the
  // trailing return after the switch is the fail-closed safety net.
  switch (status) {
    case RslCapStatus::kAuthorized:
      return 0;  // allow: no enforcement response
    case RslCapStatus::kUnlicensed:
      return 402;  // valid identity, license/scope ungranted
    case RslCapStatus::kNoToken:
    case RslCapStatus::kMalformed:
    case RslCapStatus::kUnknownIssuer:
    case RslCapStatus::kBadSignature:
    case RslCapStatus::kExpired:
      return 401;  // no valid licensed identity
  }
  return 401;  // fail closed
}

}  // namespace webbotauth
}  // namespace net_instaweb
