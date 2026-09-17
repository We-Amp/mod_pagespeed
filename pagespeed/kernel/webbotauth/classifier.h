// Copyright 2026 We-Amp B.V.
// Licensed under the Apache License, Version 2.0 (the "License").
//
// Verdict enum and the static operator-curated keyid -> bot-name map used to
// promote a cryptographically-valid signed agent to "verified bot". The
// verified-bot tier is purely a presence check against an operator-owned map
// (NO additional cryptography). The map is intentionally empty by default and
// is populated by the operator; nothing is hardcoded as trusted.

#ifndef PAGESPEED_KERNEL_WEBBOTAUTH_CLASSIFIER_H_
#define PAGESPEED_KERNEL_WEBBOTAUTH_CLASSIFIER_H_

#include <map>

#include "pagespeed/kernel/base/string.h"
#include "pagespeed/kernel/base/string_util.h"

namespace net_instaweb {
namespace webbotauth {

enum class Verdict {
  // No web-bot-auth signature material: either the request carries no
  // Signature-Input/Signature headers at all, or it carries parseable
  // signature material with NO member tagged "web-bot-auth" (some other
  // signing scheme -- classified exactly as if unsigned, never invalid).
  // LOAD-BEARING INVARIANT (the nginx wiring depends on it): after the
  // wiring's no-material short-circuit, kHuman is reachable ONLY via the
  // untagged/other-tag path -- do not add other kHuman returns to
  // VerifyAndClassify without revisiting the wiring's label/counter logic.
  // (Upstreamed from the optimizer.)
  kHuman,
  kSignedAgent,  // valid Ed25519 signature; keyid NOT in verified-bot map
  kVerifiedBot,  // valid signature AND keyid present in operator verified-bot map
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
  void Register(StringPiece keyid, StringPiece bot_name);

  // Returns true and sets *bot_name if the keyid is a known verified bot.
  bool Lookup(StringPiece keyid, GoogleString* bot_name) const;

  // Iterate every registered (keyid, bot_name) pair. Used by the opt-in counter
  // endpoint to join operator-assigned names against the
  // per-signer slots by keyid hash. Read-only; order is the map's.
  template <typename Fn>
  void ForEach(Fn&& fn) const {
    for (const auto& entry : map_) {
      fn(StringPiece(entry.first), StringPiece(entry.second));
    }
  }

  bool empty() const { return map_.empty(); }

 private:
  std::map<GoogleString, GoogleString> map_;  // keyid -> bot name
};

// Given a successful signature verification for `keyid`, classify as
// kVerifiedBot (if registered) else kSignedAgent.
Verdict ClassifyVerified(StringPiece keyid, const VerifiedBotRegistry& registry,
                         GoogleString* bot_name_out);

}  // namespace webbotauth
}  // namespace net_instaweb

#endif  // PAGESPEED_KERNEL_WEBBOTAUTH_CLASSIFIER_H_
