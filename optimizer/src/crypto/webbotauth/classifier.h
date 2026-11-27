// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.
//
// In sync with mod_pagespeed pagespeed/kernel/webbotauth/classifier.h @
// 626ddfa95 (2026-07-03). Keep logic in sync manually; the RFC 9421/8941 core
// is standards-frozen.
//
// Verdict enum and the static operator-curated keyid -> bot-name map used to
// promote a cryptographically-valid signed agent to "verified bot". The
// verified-bot tier is purely a presence check against an operator-owned map
// (NO additional cryptography). The map is intentionally empty by default and
// is populated by the operator; nothing is hardcoded as trusted.

#ifndef PAGESPEED_SRC_CRYPTO_WEBBOTAUTH_CLASSIFIER_H_
#define PAGESPEED_SRC_CRYPTO_WEBBOTAUTH_CLASSIFIER_H_

#include <cstdint>
#include <map>
#include <string>
#include <string_view>

namespace pagespeed {
namespace webbotauth {

enum class Verdict : std::uint8_t {
  // No web-bot-auth signature material: either the request carries no
  // Signature-Input/Signature headers at all, or it carries parseable
  // signature material with NO member tagged "web-bot-auth" (some other
  // signing scheme -- classified exactly as if unsigned, never invalid).
  // LOAD-BEARING INVARIANT (the nginx wiring depends on it): after the
  // wiring's no-material short-circuit, kHuman is reachable ONLY via the
  // untagged/other-tag path -- do not add other kHuman returns to
  // VerifyAndClassify without revisiting the wiring's label/counter logic.
  // (2.0 extension; 1.15's kHuman is still headers-absent-only.)
  kHuman,
  kSignedAgent,  // valid Ed25519 signature; keyid NOT in verified-bot map
  kVerifiedBot,  // valid signature AND keyid in operator verified-bot map
  kUnknown,      // any failure: malformed, unsupported, bad sig, fetch/SSRF
                 //   refusal, expired, etc. NEVER throws.
};

// Stable short token for logging / nginx variable surfacing.
const char* VerdictToken(Verdict v);

// Operator-curated verified-bot registry: keyid -> human-readable bot name.
// Default-empty. Injectable so the unit test can register a test keyid without
// touching global state across tests.
class VerifiedBotRegistry {
 public:
  VerifiedBotRegistry() = default;

  // Register a keyid as a verified bot (operator config).
  void Register(std::string_view keyid, std::string_view bot_name);

  // Returns true and sets *bot_name if the keyid is a known verified bot.
  bool Lookup(std::string_view keyid, std::string* bot_name) const;

  // Iterate every registered (keyid, bot_name) pair.  Used by the opt-in
  // counter endpoint to join operator-assigned names against
  // the per-signer slots by keyid hash.  Read-only; order is the map's.
  template <typename Fn>
  void ForEach(Fn&& fn) const {
    for (const auto& [keyid, name] : map_) {
      fn(std::string_view(keyid), std::string_view(name));
    }
  }

  bool empty() const { return map_.empty(); }

 private:
  // keyid -> bot name (transparent comparator: lookup by string_view).
  std::map<std::string, std::string, std::less<>> map_;
};

// Given a successful signature verification for `keyid`, classify as
// kVerifiedBot (if registered) else kSignedAgent.
Verdict ClassifyVerified(std::string_view keyid,
                         const VerifiedBotRegistry& registry,
                         std::string* bot_name_out);

}  // namespace webbotauth
}  // namespace pagespeed

#endif  // PAGESPEED_SRC_CRYPTO_WEBBOTAUTH_CLASSIFIER_H_
