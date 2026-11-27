// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

#include "lib/net/fetch_policy.h"

#include <cctype>
#include <optional>
#include <string>
#include <string_view>

#include "lib/net/ssrf_guard.h"
#include "lib/net/upstream_pin.h"

namespace pagespeed {
namespace {

std::string LowerNoTrailingDot(std::string_view s) {
  std::string out(s);
  for (char& c : out) {
    c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  }
  if (!out.empty() && out.back() == '.') out.pop_back();
  return out;
}

}  // namespace

ResourceClass ClassifyResource(std::string_view t) {
  if (t == "Document") return ResourceClass::kDocument;
  if (t == "Script") return ResourceClass::kScript;
  if (t == "Stylesheet") return ResourceClass::kStylesheet;
  if (t == "Image") return ResourceClass::kImage;
  if (t == "Media") return ResourceClass::kMedia;
  if (t == "Font") return ResourceClass::kFont;
  if (t == "XHR") return ResourceClass::kXhr;
  if (t == "Fetch") return ResourceClass::kFetch;
  if (t == "Prefetch") return ResourceClass::kFetch;
  if (t == "WebSocket") return ResourceClass::kWebSocket;
  if (t == "EventSource") return ResourceClass::kEventSource;
  if (t == "Ping") return ResourceClass::kPing;
  if (t == "CSPViolationReport") return ResourceClass::kCspReport;
  // Everything else (Preflight, SignedExchange, Manifest, TextTrack, Other, and any
  // future type) maps to kOther, which DecideFetch denies — fail-closed.
  return ResourceClass::kOther;
}

// The ONLY resource classes permitted to egress (then still subject to G1/G2).
// Anything outside this set denies, so a new/unmapped CDP resourceType cannot
// silently open a hole (beacon/Ping, CSP reports, WS/SSE all fall through to deny).
bool IsFetchableClass(ResourceClass rc) {
  switch (rc) {
    case ResourceClass::kDocument:
    case ResourceClass::kScript:
    case ResourceClass::kStylesheet:
    case ResourceClass::kFont:
    case ResourceClass::kXhr:
    case ResourceClass::kFetch:
      return true;
    default:
      return false;
  }
}

FetchAction DecideFetch(std::string_view url, ResourceClass rc,
                        const FetchPolicy& policy) {
  // 1. Scheme allowlist + parseability. A non-http(s) or unparseable target never
  //    egresses (data:/blob: are handled inside Chrome and don't reach here as a
  //    network fetch; this is defense-in-depth + the WS/ws: case).
  std::optional<UrlAuthority> u = ParseHttpUrl(url);
  if (!u.has_value() || !IsAllowedScheme(u->scheme)) {
    return FetchAction::kDenyBlockedByClient;
  }

  // 2. Images/media: never fetch the bytes regardless of origin — agent_optimize
  //    extracts text, not pixels; the element (src/alt) is retained by the cleaner.
  if (rc == ResourceClass::kImage || rc == ResourceClass::kMedia) {
    return FetchAction::kDenyKeepElement;
  }

  // 3. Resource-class allowlist (fail-closed): WS/EventSource/Ping/CSP-report and
  //    every unmapped/future type deny here, BEFORE the origin checks — so a
  //    beacon to a pinned upstream or an allowlisted CDN can never egress.
  if (!IsFetchableClass(rc)) {
    return FetchAction::kDenyBlockedByClient;
  }

  // 4. The customer's own configured upstream (G1) — the same-origin hydration path.
  if (IsPinnedUpstream(url, policy.pinned_upstreams)) {
    return FetchAction::kProxyUpstream;
  }

  // 5. An operator-allowlisted third-party host (G2 still runs in the fetcher).
  //    Userinfo URLs are never allowlisted (ParseHttpUrl's host is post-'@', but we
  //    additionally refuse a credentialed authority here, mirroring G1).
  if (!u->had_userinfo) {
    const std::string host = LowerNoTrailingDot(u->host);
    for (const std::string& allowed : policy.allow_hosts) {
      if (LowerNoTrailingDot(allowed) == host) {
        return FetchAction::kFetchAllowlisted;
      }
    }
  }

  // 6. Everything else: deny.
  return FetchAction::kDenyBlockedByClient;
}

}  // namespace pagespeed
