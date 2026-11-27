// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

#include "lib/net/fetch_policy.h"

#include "gtest/gtest.h"
#include "lib/net/upstream_pin.h"

// Fetch-policy routing battery. The dangerous direction is an
// ALLOW that should have been a DENY (egress to a lateral host / a third party
// that wasn't allowlisted / a non-http scheme). Every deny case below is a real
// pivot the render must not make.

namespace pagespeed {
namespace {

FetchPolicy MakePolicy() {
  FetchPolicy p;
  auto up = ParseUpstream("http://127.0.0.1:3000");  // the SPA's own API
  if (up.has_value()) p.pinned_upstreams.push_back(*up);
  p.allow_hosts = {"cdn.example.com"};  // one opt-in third party
  return p;
}

TEST(FetchPolicy, ClassifyResourceMapping) {
  EXPECT_EQ(ClassifyResource("Document"), ResourceClass::kDocument);
  EXPECT_EQ(ClassifyResource("Script"), ResourceClass::kScript);
  EXPECT_EQ(ClassifyResource("XHR"), ResourceClass::kXhr);
  EXPECT_EQ(ClassifyResource("Fetch"), ResourceClass::kFetch);
  EXPECT_EQ(ClassifyResource("Image"), ResourceClass::kImage);
  EXPECT_EQ(ClassifyResource("WebSocket"), ResourceClass::kWebSocket);
  EXPECT_EQ(ClassifyResource("EventSource"), ResourceClass::kEventSource);
  EXPECT_EQ(ClassifyResource("Ping"), ResourceClass::kPing);
  EXPECT_EQ(ClassifyResource("CSPViolationReport"), ResourceClass::kCspReport);
  EXPECT_EQ(ClassifyResource("Manifest"), ResourceClass::kOther);
  EXPECT_EQ(ClassifyResource("totally-unknown"), ResourceClass::kOther);
}

// H3/S1 — beacon (sendBeacon → Ping) and every unmapped/future resource type deny
// even when the target is the pinned upstream OR an allowlisted CDN. http(s) URLs
// throughout, so the deny is attributable to the resource class, not the scheme.
TEST(FetchPolicy, BeaconAndUnknownTypesDeniedEvenToTrustedHosts) {
  const auto p = MakePolicy();
  EXPECT_EQ(DecideFetch("https://cdn.example.com/collect",
                        ClassifyResource("Ping"), p),
            FetchAction::kDenyBlockedByClient);
  EXPECT_EQ(
      DecideFetch("http://127.0.0.1:3000/beacon", ClassifyResource("Ping"), p),
      FetchAction::kDenyBlockedByClient);
  EXPECT_EQ(DecideFetch("https://cdn.example.com/r",
                        ClassifyResource("CSPViolationReport"), p),
            FetchAction::kDenyBlockedByClient);
  EXPECT_EQ(
      DecideFetch("http://127.0.0.1:3000/x", ClassifyResource("Preflight"), p),
      FetchAction::kDenyBlockedByClient);
  // an unknown/future CDP type (→ kOther) must NOT egress to a pinned/allowlisted host
  EXPECT_EQ(DecideFetch("http://127.0.0.1:3000/x",
                        ClassifyResource("FutureTypeXYZ"), p),
            FetchAction::kDenyBlockedByClient);
  EXPECT_EQ(DecideFetch("https://cdn.example.com/x",
                        ClassifyResource("FutureTypeXYZ"), p),
            FetchAction::kDenyBlockedByClient);
}

// The single most important ALLOW: the SPA hydrating from its own-origin API.
TEST(FetchPolicy, SameOriginXhrAndScriptProxied) {
  const auto p = MakePolicy();
  EXPECT_EQ(
      DecideFetch("http://127.0.0.1:3000/api/data", ResourceClass::kXhr, p),
      FetchAction::kProxyUpstream);
  EXPECT_EQ(
      DecideFetch("http://127.0.0.1:3000/app.js", ResourceClass::kScript, p),
      FetchAction::kProxyUpstream);
  EXPECT_EQ(DecideFetch("http://127.0.0.1:3000/x", ResourceClass::kFetch, p),
            FetchAction::kProxyUpstream);
}

// Lateral pivots on the same private host (wrong port) must be denied.
TEST(FetchPolicy, SameHostWrongPortDenied) {
  const auto p = MakePolicy();
  EXPECT_EQ(DecideFetch("http://127.0.0.1:6379/", ResourceClass::kXhr, p),
            FetchAction::kDenyBlockedByClient);  // Redis
  EXPECT_EQ(DecideFetch("http://127.0.0.1:22/", ResourceClass::kFetch, p),
            FetchAction::kDenyBlockedByClient);  // SSH
}

TEST(FetchPolicy, UnlistedThirdPartyScriptDenied) {
  const auto p = MakePolicy();
  EXPECT_EQ(DecideFetch("https://evil.example/x.js", ResourceClass::kScript, p),
            FetchAction::kDenyBlockedByClient);
  EXPECT_EQ(DecideFetch("https://tracker.io/t.js", ResourceClass::kFetch, p),
            FetchAction::kDenyBlockedByClient);
}

TEST(FetchPolicy, AllowlistedThirdPartyRoutedToG2) {
  const auto p = MakePolicy();
  EXPECT_EQ(
      DecideFetch("https://cdn.example.com/lib.js", ResourceClass::kScript, p),
      FetchAction::kFetchAllowlisted);
  // case + trailing-dot are normalized for the allowlist match
  EXPECT_EQ(
      DecideFetch("https://CDN.EXAMPLE.COM./lib.js", ResourceClass::kScript, p),
      FetchAction::kFetchAllowlisted);
  // a sibling host is NOT covered by the allowlist entry (no suffix match)
  EXPECT_EQ(DecideFetch("https://evil.cdn.example.com/x.js",
                        ResourceClass::kScript, p),
            FetchAction::kDenyBlockedByClient);
}

// Images/media never fetch bytes — regardless of origin (even same-origin).
TEST(FetchPolicy, ImagesAndMediaKeepElement) {
  const auto p = MakePolicy();
  EXPECT_EQ(
      DecideFetch("http://127.0.0.1:3000/logo.png", ResourceClass::kImage, p),
      FetchAction::kDenyKeepElement);
  EXPECT_EQ(
      DecideFetch("https://cdn.example.com/hero.jpg", ResourceClass::kImage, p),
      FetchAction::kDenyKeepElement);
  EXPECT_EQ(
      DecideFetch("https://evil.example/track.gif", ResourceClass::kImage, p),
      FetchAction::kDenyKeepElement);
  EXPECT_EQ(
      DecideFetch("http://127.0.0.1:3000/v.mp4", ResourceClass::kMedia, p),
      FetchAction::kDenyKeepElement);
}

// WebSocket / EventSource always denied, even to the pinned upstream.
TEST(FetchPolicy, WebSocketAndEventSourceDenied) {
  const auto p = MakePolicy();
  EXPECT_EQ(
      DecideFetch("http://127.0.0.1:3000/ws", ResourceClass::kWebSocket, p),
      FetchAction::kDenyBlockedByClient);
  EXPECT_EQ(
      DecideFetch("http://127.0.0.1:3000/sse", ResourceClass::kEventSource, p),
      FetchAction::kDenyBlockedByClient);
}

TEST(FetchPolicy, NonHttpSchemeDenied) {
  const auto p = MakePolicy();
  EXPECT_EQ(DecideFetch("ws://127.0.0.1:3000/", ResourceClass::kWebSocket, p),
            FetchAction::kDenyBlockedByClient);
  EXPECT_EQ(DecideFetch("file:///etc/passwd", ResourceClass::kOther, p),
            FetchAction::kDenyBlockedByClient);
  EXPECT_EQ(DecideFetch("not a url", ResourceClass::kScript, p),
            FetchAction::kDenyBlockedByClient);
}

// Userinfo in a third-party URL must not pin and must not allowlist.
TEST(FetchPolicy, UserinfoNeverAllowed) {
  const auto p = MakePolicy();
  // post-@ host is the upstream, but userinfo present → not pinned (G1) and the
  // allowlist branch is skipped on userinfo → deny → falls to the G2 fetcher path.
  EXPECT_EQ(DecideFetch("http://x@127.0.0.1:3000/", ResourceClass::kXhr, p),
            FetchAction::kDenyBlockedByClient);
  // userinfo whose post-@ host is an allowlisted CDN is still refused here.
  EXPECT_EQ(DecideFetch("https://evil@cdn.example.com/x.js",
                        ResourceClass::kScript, p),
            FetchAction::kDenyBlockedByClient);
}

// A redirect hop is re-adjudicated by the same function (the bridge calls
// DecideFetch on each Location): a public→loopback redirect target denies.
TEST(FetchPolicy, RedirectTargetReDecided) {
  const auto p = MakePolicy();
  // the original allowlisted CDN would be kFetchAllowlisted, but a Location that
  // points at a non-upstream loopback port re-decides to deny.
  EXPECT_EQ(DecideFetch("http://127.0.0.1:6379/", ResourceClass::kScript, p),
            FetchAction::kDenyBlockedByClient);
  EXPECT_EQ(DecideFetch("http://127.0.0.1:3000/redirected",
                        ResourceClass::kScript, p),
            FetchAction::kProxyUpstream);
}

TEST(FetchPolicy, EmptyPolicyDeniesEverythingExceptKeepElement) {
  FetchPolicy empty;
  EXPECT_EQ(DecideFetch("http://127.0.0.1:3000/x", ResourceClass::kXhr, empty),
            FetchAction::kDenyBlockedByClient);
  EXPECT_EQ(DecideFetch("https://cdn.example.com/x.js", ResourceClass::kScript,
                        empty),
            FetchAction::kDenyBlockedByClient);
  EXPECT_EQ(DecideFetch("https://x/y.png", ResourceClass::kImage, empty),
            FetchAction::kDenyKeepElement);
}

}  // namespace
}  // namespace pagespeed
