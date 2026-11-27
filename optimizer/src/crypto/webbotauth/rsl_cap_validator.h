// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.
//
// Ported from mod_pagespeed
// pagespeed/kernel/webbotauth/rsl_cap_validator.h @
// f6c24e8d34ba0c58acb22d626f6213d666d73e41 (2026-07-04; AgentPass A3).
// Keep logic in sync manually; reviewers diff this against the 1.15 source.
//
// RSL-CAP capability-token validator. This is the operator-gated ENFORCEMENT
// sibling of the observe-only WS3 verifier (verifier.h): the verifier only
// CLASSIFIES an RFC-9421 request signature; this validator decides whether an
// Authorization: License capability token grants a requested license/scope, and
// the front-end maps the verdict to an inline 401 / 402. It NEVER
// blocks/redirects/modifies content beyond the status, and NEVER clears,
// settles, meters, escrows, or custodies money. Default-OFF and inert until an
// operator explicitly enables enforcement.

#ifndef PAGESPEED_SRC_CRYPTO_WEBBOTAUTH_RSL_CAP_VALIDATOR_H_
#define PAGESPEED_SRC_CRYPTO_WEBBOTAUTH_RSL_CAP_VALIDATOR_H_

#include <cstdint>
#include <string_view>

#include "src/crypto/webbotauth/key_directory.h"
#include "src/crypto/webbotauth/rsl_cap_token.h"

namespace pagespeed {
namespace webbotauth {

// Outcome of validating an RSL-CAP capability token. The front-end maps these
// to HTTP status:
//
//   kAuthorized   -> allow (pass the request through)
//   kNoToken      -> 401   (no Authorization: License header)
//   kMalformed    -> 401   (not 3 parts / bad b64url / header not RSL-CAP /
//                           alg != EdDSA / bad payload shape)
//   kUnknownIssuer-> 401   (kid unresolvable via the KeyDirectoryProvider)
//   kBadSignature -> 401   (ed25519_verify != 1 / key != 32B / sig != 64B)
//   kExpired      -> 401   (exp <= now)
//   kUnlicensed   -> 402   (valid signed identity, but requested license/scope
//                           NOT in the token's grant set)
//
// 401 = "no valid licensed identity" (token problem). 402 = "identity
// cryptographically valid, but this license/scope is not granted". The engine
// ONLY emits a status; it NEVER clears, settles, meters, escrows, or custodies
// money.
enum class RslCapStatus : std::uint8_t {
  kAuthorized,
  kNoToken,
  kMalformed,
  kUnknownIssuer,
  kBadSignature,
  kExpired,
  kUnlicensed,
};

// Maps an RSL-CAP verdict to the HTTP status the enforcement handler should
// apply: 0 = allow (no enforcement response), 402 = valid identity but
// license/scope ungranted, 401 = no valid licensed identity. Kept as a
// pure, front-end-free function so the mapping is unit-testable.
int RslCapStatusToHttpStatus(RslCapStatus status);

// Stateless validator for RSL-CAP tokens. Reuses the WS3 KeyDirectoryProvider
// abstraction for issuer-key resolution (a static/warmed trusted-key provider
// behind the same interface -- no parallel key path). Time is supplied as an
// explicit `now_unix_sec` (same shape as VerifyAndClassify), so tests are
// deterministic and there is no hidden clock dependency. Does NO content
// modification, NO settlement, NO payment work.
//
// `directory_host` is the operator-mapped issuer directory host passed to the
// provider's GetKey(host, keyid, now) -- it comes from operator config, NOT from
// any request-controlled header (plane-split). For static-key providers it is
// ignored.
//
// The KeyDirectoryProvider is NOT owned by the validator.
class RslCapValidator {
 public:
  explicit RslCapValidator(KeyDirectoryProvider* provider);

  // Validates the raw Authorization header value `auth_header` (e.g.
  // "License <token>") and authorizes it against the requested license id +
  // scope. `requested_license`, `requested_scope`, and `directory_host` are all
  // supplied OUT-OF-BAND by operator config / route -- never derived from
  // attacker-controlled request data beyond the token itself.
  //
  // Algorithm (signature is verified BEFORE expiry/authorization, so a tampered
  // exp or lic[] never reaches those steps -- they run only on a
  // cryptographically intact payload):
  //   1. strip "License " prefix; absent/empty => kNoToken
  //   2-4. parse + strict header check (typ/alg/kid)  => kMalformed on failure
  //   5. resolve key by kid via the provider          => kUnknownIssuer / kBadSignature
  //   6. ed25519_verify over the original signing input => kBadSignature
  //   7-8. expiry vs now_unix_sec                       => kExpired
  //   9. requested license in lic[] AND scope in scope[] => kAuthorized
  //      else                                            => kUnlicensed
  //
  // If `out_token` is non-null it receives the parsed token (populated as far
  // as parsing got).
  RslCapStatus Validate(std::string_view auth_header,
                        std::string_view requested_license,
                        std::string_view requested_scope,
                        std::string_view directory_host, int64_t now_unix_sec,
                        RslCapToken* out_token) const;

  // Convenience overload that discards the parsed token.
  RslCapStatus Validate(std::string_view auth_header,
                        std::string_view requested_license,
                        std::string_view requested_scope,
                        std::string_view directory_host,
                        int64_t now_unix_sec) const {
    return Validate(auth_header, requested_license, requested_scope,
                    directory_host, now_unix_sec, nullptr);
  }

  // The scheme prefix carried in the Authorization header.
  static const char kSchemePrefix[];  // "License "

 private:
  KeyDirectoryProvider* provider_;  // not owned
};

}  // namespace webbotauth
}  // namespace pagespeed

#endif  // PAGESPEED_SRC_CRYPTO_WEBBOTAUTH_RSL_CAP_VALIDATOR_H_
