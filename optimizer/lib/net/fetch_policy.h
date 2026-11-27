// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

#ifndef LIB_NET_FETCH_POLICY_H_
#define LIB_NET_FETCH_POLICY_H_

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "lib/net/upstream_pin.h"

// The per-subresource routing decision for the agent_optimize fetcher (the
// policy table). Every request the headless render makes is adjudicated here
// BEFORE any egress: the CDP Fetch bridge (P1.4) calls DecideFetch() on each paused
// request and on every redirect hop, and acts on the verdict. This is the pure,
// hermetically-testable security core; it composes G1 (upstream_pin) + G2
// (ssrf_guard scheme/SSRF) + the resource-class rules, and does NO I/O itself.
//
// Fail-closed: anything not explicitly allowed is denied (BlockedByClient).

namespace pagespeed {

// CDP Fetch.requestPaused resourceType, classified to what the policy cares about.
// Only an explicit allowlist of classes may ever egress (see DecideFetch); every
// other class — including kOther for any unmapped/future resourceType — is denied.
enum class ResourceClass : std::uint8_t {
  // Fetchable classes (proceed to the G1/G2 origin checks):
  kDocument,  // subframe document (the TOP-level is setDocumentContent, never fetched)
  kScript,
  kStylesheet,
  kFont,
  kXhr,
  kFetch,
  // Keep-element classes (block the bytes, retain the element):
  kImage,
  kMedia,
  // Always-deny classes (no opt-in — spec §3.2/§3.5):
  kWebSocket,
  kEventSource,
  kPing,       // navigator.sendBeacon — exfil vector, DENY always
  kCspReport,  // CSP violation report POST
  kOther,      // anything unmapped/future — fail-closed to DENY
};

// Map a CDP resourceType string ("Document", "Script", "XHR", "WebSocket", ...) to
// a ResourceClass. Unknown/unrecognized → kOther.
ResourceClass ClassifyResource(std::string_view cdp_resource_type);

enum class FetchAction : std::uint8_t {
  // G1: route back to the customer's own configured upstream via the on-box proxy
  // (GET-only, credentials stripped — anonymous view, §3.4). Allowed on loopback/
  // RFC1918 only because it byte-equals a configured upstream.
  kProxyUpstream,
  // G2: an operator-allowlisted third-party host. The fetcher MUST still resolve and
  // run the SSRF guard (ssrf_guard::IsPublicUrl) on the resolved addresses, and pin
  // the validated IP, BEFORE any egress. This verdict only says "allowlisted".
  kFetchAllowlisted,
  // Image/media: never fetch the bytes (we extract text, not pixels); the cleaning
  // pass keeps the element's src/alt so markdown references the image by URL.
  kDenyKeepElement,
  // Everything else: Fetch.failRequest with reason BlockedByClient.
  kDenyBlockedByClient,
};

// The render-session policy: the config-pinned upstream set (G1) and the opt-in
// third-party host allowlist (G2). Both come from worker config; injected here so
// the decision stays pure and testable. `allow_hosts` entries are canonical hosts
// (lower-cased; comparison also lower-cases + strips a trailing dot defensively).
struct FetchPolicy {
  std::vector<PinnedUpstream> pinned_upstreams;
  std::vector<std::string> allow_hosts;
};

// Adjudicate one (sub)resource request. Pure: no DNS, no I/O. Fail-closed on the
// RESOURCE-CLASS axis (an allowlist, not a denylist), so any unmapped/future CDP
// resourceType denies rather than egressing. Order of precedence:
//   1. non-http(s) scheme / unparseable URL          -> kDenyBlockedByClient
//   2. image/media                                   -> kDenyKeepElement
//   3. class NOT in the fetchable allowlist
//      (websocket/eventsource/ping/csp-report/other) -> kDenyBlockedByClient
//   4. fetchable class + byte-equal a pinned upstream -> kProxyUpstream (G1)
//   5. fetchable class + host in the allowlist        -> kFetchAllowlisted (fetcher runs G2)
//   6. otherwise                                      -> kDenyBlockedByClient
FetchAction DecideFetch(std::string_view url, ResourceClass rc,
                        const FetchPolicy& policy);

}  // namespace pagespeed

#endif  // LIB_NET_FETCH_POLICY_H_
