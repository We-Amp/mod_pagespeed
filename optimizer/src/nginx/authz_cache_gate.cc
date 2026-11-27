// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

#include "src/nginx/authz_cache_gate.h"

#include "lib/cache/cache_control_header.h"
#include "lib/classify/alternate_metadata.h"

namespace pagespeed {

bool AuthzCacheGateAllows(bool request_has_authorization,
                          uint16_t origin_cc_flags) {
  if (!request_has_authorization) {
    return true;
  }
  // RFC 9111 §3.5: only these response directives explicitly permit a
  // shared cache to store/serve despite the request's Authorization.  The
  // s-maxage permit is its Present flag (which also covers s-maxage=0); a
  // stored s-maxage value without the flag cannot occur (see header), so
  // origin_cc_flags == 0 — no Cache-Control at store time, or corrupt
  // metadata — carries no permission signal and fails closed.
  constexpr uint16_t kPermits = AlternateMetadata::kCCOriginPublic |
                                AlternateMetadata::kCCOriginMustRevalidate |
                                AlternateMetadata::kCCOriginSMaxagePresent;
  return (origin_cc_flags & kPermits) != 0;
}

bool AuthzCacheGateAllowsStale(bool request_has_authorization,
                               uint16_t origin_cc_flags) {
  if (!request_has_authorization) {
    return true;
  }
  // Once stale, must-revalidate/s-maxage demand revalidation before reuse
  // (RFC 9111 §4.2.4, §5.2.2.10), so of §3.5's permits only `public`
  // still allows a stale serve (coalesced-SWR and stale-if-error alike).
  return (origin_cc_flags & AlternateMetadata::kCCOriginPublic) != 0;
}

bool StaleServePermitted(bool request_has_authorization,
                         uint16_t origin_cc_flags) {
  // nginx is always a shared cache: must-revalidate, proxy-revalidate and
  // s-maxage (RFC 9111 §5.2.2.2, §5.2.2.8, §5.2.2.10) all forbid a stale
  // serve without validation.  Same predicate the freshness and
  // stale-if-error paths use.
  if (ComputeSharedRevalidationRequired(origin_cc_flags,
                                        /*is_shared_cache=*/true)) {
    return false;
  }
  return AuthzCacheGateAllowsStale(request_has_authorization, origin_cc_flags);
}

}  // namespace pagespeed
