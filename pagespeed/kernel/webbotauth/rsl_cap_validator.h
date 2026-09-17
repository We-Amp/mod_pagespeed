// Copyright 2026 We-Amp B.V.
//
// SPDX-License-Identifier: Apache-2.0
//
// Author: agentpass scaffolding (A3 — RSL-CAP token enforcement)
//
// RSL-CAP capability-token validator. This is the PAID enforcement sibling of
// the FREE WS3 verifier (verifier.h): the verifier only CLASSIFIES an RFC-9421
// request signature; this validator decides whether an Authorization: License
// capability token grants a requested license/scope, and the nginx layer maps
// the verdict to an inline 401 / 402. It NEVER blocks/redirects/modifies
// content beyond the status, and NEVER clears, settles, meters, escrows, or
// custodies money. Default-OFF and non-shipping (gated by the engine option +
// the nginx ps_srv_conf_t flag, both default disabled).

#ifndef PAGESPEED_KERNEL_WEBBOTAUTH_RSL_CAP_VALIDATOR_H_
#define PAGESPEED_KERNEL_WEBBOTAUTH_RSL_CAP_VALIDATOR_H_

#include <cstdint>

#include "pagespeed/kernel/base/string.h"
#include "pagespeed/kernel/base/string_util.h"
#include "pagespeed/kernel/webbotauth/key_directory.h"
#include "pagespeed/kernel/webbotauth/rsl_cap_token.h"

namespace net_instaweb {
namespace webbotauth {

// Outcome of validating an RSL-CAP capability token. The nginx layer maps these
// to HTTP status:
//
//   kAuthorized   -> allow (NGX_DECLINED)
//   kNoToken      -> 401   (no Authorization: License header)
//   kMalformed    -> 401   (not 3 parts / bad b64url / header not RSL-CAP /
//                           alg != EdDSA / bad payload shape)
//   kUnknownIssuer-> 401   (kid unresolvable via the KeyDirectoryProvider)
//   kBadSignature -> 401   (ed25519_verify != 1 / key != 32B / sig != 64B)
//   kExpired      -> 401   (exp <= now)
//   kUnlicensed   -> 402   (valid signed identity, but requested license/scope
//                           NOT in the token's grant set — settle externally)
//
// 401 = "no valid licensed identity" (token problem). 402 = "identity
// cryptographically valid, but this license/scope is not granted". The engine
// ONLY emits a status; it NEVER clears, settles, meters, escrows, or custodies
// money.
enum class RslCapStatus {
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
// pure, nginx-free function so the mapping is unit-testable.
int RslCapStatusToHttpStatus(RslCapStatus status);

// Stateless validator for RSL-CAP tokens. Reuses the WS3 KeyDirectoryProvider
// abstraction for issuer-key resolution (a static trusted-key provider and/or
// the SSRF-guarded NetFetchKeyDirectory introspection fetcher behind the same
// interface — no parallel key path). Time is supplied as an explicit
// `now_unix_sec` (same shape as VerifyAndClassify), so tests are deterministic
// and there is no hidden clock dependency. Does NO content modification, NO
// settlement, NO payment work.
//
// `directory_host` is the operator-mapped issuer directory host passed to the
// provider's GetKey(host, keyid, now) — it comes from operator config, NOT from
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
  // supplied OUT-OF-BAND by operator config / route — never derived from
  // attacker-controlled request data beyond the token itself.
  //
  // Algorithm (signature is verified BEFORE expiry/authorization, so a tampered
  // exp or lic[] never reaches those steps — they run only on a
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
  RslCapStatus Validate(StringPiece auth_header, StringPiece requested_license,
                        StringPiece requested_scope, StringPiece directory_host,
                        int64_t now_unix_sec, RslCapToken* out_token) const;

  // Convenience overload that discards the parsed token.
  RslCapStatus Validate(StringPiece auth_header, StringPiece requested_license,
                        StringPiece requested_scope, StringPiece directory_host,
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
}  // namespace net_instaweb

#endif  // PAGESPEED_KERNEL_WEBBOTAUTH_RSL_CAP_VALIDATOR_H_
