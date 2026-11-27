// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

#ifndef LIB_NET_UPSTREAM_PIN_H_
#define LIB_NET_UPSTREAM_PIN_H_

#include <optional>
#include <string>
#include <string_view>
#include <vector>

// G1 — the same-origin/upstream carve-out classifier for the agent_optimize
// fetcher. It decides whether a subresource the render wants to
// load is the customer's OWN configured upstream (allowed back through the on-box
// proxy, even on loopback/RFC1918) versus a lateral pivot (which must be denied
// by default and only reach a third party through the G2 SSRF guard + allowlist).
//
// The keystone safety property (spec §3.3 G1): origin identity is pinned from the
// module's OWN configured upstream authority, NEVER from the page-supplied
// Host/Origin/Referer. The match is a canonicalized (scheme, host, port) BYTE
// equality — never a substring/prefix/suffix — so a page cannot enlarge the set
// (e.g. `backend.internal.evil.com` does not match `backend.internal`). A private
// IP is allowed ONLY when it byte-equals the exact configured upstream.
//
// What this is NOT: it does not resolve DNS or pin the connect IP. A classifier
// MATCH means "route this back to the configured upstream"; the fetcher (P1.3/P1.4)
// then resolves the name and pins that IP for the connection. The carve-out
// deliberately permits loopback/RFC1918 backends (the customer's own app server),
// so it does NOT require the resolved IP to be public — but it DOES reject a
// link-local/cloud-metadata pin (ssrf_guard IsLinkLocalIp), so a same-name rebind
// (split-horizon or attacker DNS for the render's own domain) cannot steer the pin
// at 169.254.169.254. A classifier MISS falls through to the G2 SSRF guard
// (deny-by-default; every resolved address must be public).

namespace pagespeed {

// A configured upstream authority in canonical form (host IP-normalized or
// lowercased+trailing-dot-stripped; port explicit). Compared byte-for-byte.
struct PinnedUpstream {
  std::string scheme;      // "http" or "https"
  std::string canon_host;  // canonicalized host
  int port = 0;            // explicit (scheme default applied)
  bool is_ip_literal =
      false;  // host is an IP literal (part of the identity key, so
              // a hostname can never string-equal an IP-literal pin)
};

// Parse + canonicalize a configured upstream from a "scheme://host[:port]" string
// (the module's proxy_pass target / worker origin; any path/query is ignored).
// Rejects userinfo, a non-http(s) scheme, and malformed input. nullopt on failure.
std::optional<PinnedUpstream> ParseUpstream(std::string_view config_url);

// G1 verdict: true iff `subresource_url` canonicalizes to a (scheme, host, port)
// that byte-equals one of `pinned`. A URL bearing userinfo is NEVER pinned. A miss
// (false) MUST send the request to the G2 SSRF guard; this function never allows a
// private IP except via an exact pinned-upstream match.
bool IsPinnedUpstream(std::string_view subresource_url,
                      const std::vector<PinnedUpstream>& pinned);

}  // namespace pagespeed

#endif  // LIB_NET_UPSTREAM_PIN_H_
