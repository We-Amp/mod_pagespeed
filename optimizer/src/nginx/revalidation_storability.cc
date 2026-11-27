// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

#include "src/nginx/revalidation_storability.h"

#include "lib/cache/html_cache_policy.h"
#include "lib/classify/alternate_metadata.h"
#include "src/nginx/authz_cache_gate.h"

namespace pagespeed {

bool RefreshedHeadersForbidSharedStorage(uint16_t refreshed_cc_flags) {
  if ((refreshed_cc_flags & AlternateMetadata::kCCOriginNoStore) != 0) {
    return true;
  }
  if ((refreshed_cc_flags & AlternateMetadata::kCCOriginPrivate) == 0) {
    return false;
  }
  // A BARE `private` anywhere in the response forbids holding the
  // representation, and dominates a qualified occurrence elsewhere: flags
  // are OR-accumulated across header lines, so
  //     Cache-Control: private="Set-Cookie"
  //     Cache-Control: private
  // must evict on the strength of the second line.  "Bare seen" is the
  // monotone signal; keying on "qualified seen" instead would under-evict
  // exactly here — reintroducing the security bug this code exists to fix.
  if ((refreshed_cc_flags & AlternateMetadata::kCCOriginPrivateBare) != 0) {
    return true;
  }
  // Qualified only (`private="Set-Cookie"`, RFC 9111 §5.2.2.7): restricts
  // the named field-names, permits storing the remainder.  Evicting would
  // destroy the entry and every derived variant on every revalidation —
  // for an idiom origins pair with a perfectly cacheable `max-age`.
  if ((refreshed_cc_flags & AlternateMetadata::kCCOriginPrivateQualified) !=
      0) {
    return false;
  }
  // `private` with NEITHER form bit: metadata written before those bits
  // existed, so there is no per-occurrence information to reason from.
  // Fail safe — treat as blanket, matching the earlier blanket conservatism.
  return true;
}

RestampVerdict EvaluateRestampStorability(
    const RestampStorabilityInputs& inputs) {
  // Dominant: the origin now says a shared cache must not hold this
  // representation.  Declining the write-back would leave the offending
  // copy in place, so the stored entry is evicted.
  if (RefreshedHeadersForbidSharedStorage(inputs.refreshed_cc_flags)) {
    return RestampVerdict::kEvict;
  }
  // RFC 9111 §3.5 over the REFRESHED flags: an Authorization-bearing
  // exchange may only extend a shared-cache entry's life while the
  // response still carries an explicit permit.  A 304 that withdraws the
  // permit stops the life-extension without destroying an entry that was
  // lawfully stored for anonymous traffic.
  if (!AuthzCacheGateAllows(inputs.request_has_authorization,
                            inputs.refreshed_cc_flags)) {
    return RestampVerdict::kSkipRestamp;
  }
  // The same reasoning for a cookie-bearing exchange: it may extend the
  // entry's life only while the refreshed headers still satisfy the cookie
  // rule for the entry's class (cookie_cache_gate.h).
  if (!CookieRequestMayStore(inputs.request_has_cookie, inputs.entry_class,
                             HtmlExplicitlyShared(inputs.refreshed_cc_flags,
                                                  inputs.refreshed_s_maxage),
                             OriginStatesLifetime(inputs.refreshed_cc_flags,
                                                  inputs.refreshed_max_age,
                                                  inputs.refreshed_s_maxage))) {
    return RestampVerdict::kSkipRestamp;
  }
  // A 304 that sets a cookie marks the exchange as personalized; it must
  // not be the reason a shared entry stays fresh.
  if (inputs.response_has_set_cookie) {
    return RestampVerdict::kSkipRestamp;
  }
  return RestampVerdict::kRestamp;
}

}  // namespace pagespeed
