// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

// Tests for the RFC 9111 §3.5 Authorization shared-cache gate
// (src/nginx/authz_cache_gate.h): responses to Authorization-bearing
// requests are stored/served only on an explicit permit, and once stale
// any reuse (coalesced-SWR serve, stale-if-error serve) additionally
// requires `public`.
//
// NOT covered here (module wiring, integration-only): the RSL-CAP
// exemption's internal-request restriction (the `!r->internal` condition
// in ngx_http_pagespeed_authz_gate_has_auth), the cache_miss Early Hints
// suppression for Authorization-bearing requests, and the stale-on-error
// AND-wiring itself (ngx_http_pagespeed_stale_on_error_permitted =
// window check && AuthzCacheGateAllowsStale).  All live purely in
// ngx_pagespeed_module.cc — the kernel surface carries no request/ctx
// state — so they have no pure-function seam to test against.

#include "src/nginx/authz_cache_gate.h"

#include "gtest/gtest.h"
#include "lib/classify/alternate_metadata.h"

namespace pagespeed {
namespace {

using Meta = AlternateMetadata;

TEST(AuthzCacheGateTest, NoAuthorizationAlwaysAllowed) {
  // Without Authorization on the request the gate is a no-op, whatever the
  // response's Cache-Control looks like (including no CC at all).
  EXPECT_TRUE(AuthzCacheGateAllows(false, 0));
  EXPECT_TRUE(AuthzCacheGateAllows(false, Meta::kCCOriginHeaderPresent));
  EXPECT_TRUE(AuthzCacheGateAllows(
      false, Meta::kCCOriginHeaderPresent | Meta::kCCOriginPrivate));
  EXPECT_TRUE(AuthzCacheGateAllows(
      false, Meta::kCCOriginHeaderPresent | Meta::kCCOriginNoStore));
}

TEST(AuthzCacheGateTest, AuthorizationWithPublicAllowed) {
  EXPECT_TRUE(AuthzCacheGateAllows(
      true, Meta::kCCOriginHeaderPresent | Meta::kCCOriginPublic));
}

TEST(AuthzCacheGateTest, AuthorizationWithMustRevalidateAllowed) {
  EXPECT_TRUE(AuthzCacheGateAllows(
      true, Meta::kCCOriginHeaderPresent | Meta::kCCOriginMustRevalidate));
}

TEST(AuthzCacheGateTest, AuthorizationWithSMaxagePresentAllowed) {
  // s-maxage=0 still sets the Present flag — an explicit shared-cache
  // directive permits storage/serving (the entry is just immediately stale
  // and must revalidate).  The flag alone IS the permit: a stored s-maxage
  // value never travels without it (see authz_cache_gate.h).
  EXPECT_TRUE(AuthzCacheGateAllows(
      true, Meta::kCCOriginHeaderPresent | Meta::kCCOriginSMaxagePresent));
}

TEST(AuthzCacheGateTest, SMaxageValueWithoutFlagCannotOccurAndIsRefused) {
  // A stored s-maxage VALUE without kCCOriginSMaxagePresent cannot occur:
  // flag and value were introduced atomically in metadata v3, the parser
  // sets both together, every writer copies them wholesale, and
  // Deserialize rejects pre-v3 metadata.  Metadata without the flag
  // therefore carries no s-maxage permit and is refused — corrupt
  // metadata fails CLOSED, not open.
  EXPECT_FALSE(AuthzCacheGateAllows(true, 0));
}

TEST(AuthzCacheGateTest, AuthorizationWithMaxAgeOnlyBypassed) {
  // max-age alone is NOT one of RFC 9111 §3.5's permitting directives.
  // (max-age travels out-of-band of the flags, so flags-wise this is
  // HeaderPresent only.)
  EXPECT_FALSE(AuthzCacheGateAllows(true, Meta::kCCOriginHeaderPresent));
}

TEST(AuthzCacheGateTest, AuthorizationWithNoCCSignalBypassed) {
  // origin_cc_flags == 0: the origin sent no Cache-Control at store time
  // (or the metadata is corrupt).  No permission signal, so the gate fails
  // closed for Authorization-bearing requests.
  EXPECT_FALSE(AuthzCacheGateAllows(true, /*origin_cc_flags=*/0));
}

TEST(AuthzCacheGateTest, AuthorizationWithPrivateBypassed) {
  EXPECT_FALSE(AuthzCacheGateAllows(
      true, Meta::kCCOriginHeaderPresent | Meta::kCCOriginPrivate));
}

TEST(AuthzCacheGateTest, ProxyRevalidateIsNotAPermit) {
  // §3.5's enumeration (public / must-revalidate / s-maxage) is exhaustive;
  // proxy-revalidate is deliberately not treated as a permit.
  EXPECT_FALSE(AuthzCacheGateAllows(
      true, Meta::kCCOriginHeaderPresent | Meta::kCCOriginProxyRevalidate));
}

TEST(AuthzCacheGateTest, PermitCombinesWithOtherDirectives) {
  // A permitting directive keeps permitting alongside unrelated directives.
  EXPECT_TRUE(AuthzCacheGateAllows(true, Meta::kCCOriginHeaderPresent |
                                             Meta::kCCOriginMustRevalidate |
                                             Meta::kCCOriginNoCache));
  EXPECT_TRUE(AuthzCacheGateAllows(
      true, Meta::kCCOriginHeaderPresent | Meta::kCCOriginPublic |
                Meta::kCCOriginNoTransform | Meta::kCCOriginImmutable));
}

TEST(AuthzCacheGateTest, PublicWinsOverPrivateInThePureFunction) {
  // Contract pin: in the pure function a permit wins even alongside
  // `private`.  The store path never reaches this input — `private` sets
  // ctx->uncacheable before the gate matters — but the function's contract
  // is pinned so a future caller can't be surprised.
  EXPECT_TRUE(AuthzCacheGateAllows(true, Meta::kCCOriginHeaderPresent |
                                             Meta::kCCOriginPublic |
                                             Meta::kCCOriginPrivate));
}

TEST(AuthzCacheGateTest, AllThreePermitsTogetherAllowed) {
  EXPECT_TRUE(AuthzCacheGateAllows(
      true, Meta::kCCOriginHeaderPresent | Meta::kCCOriginPublic |
                Meta::kCCOriginMustRevalidate | Meta::kCCOriginSMaxagePresent));
}

TEST(AuthzCacheGateTest, MustRevalidateWithProxyRevalidateStillPermitted) {
  // Permitted via must-revalidate; proxy-revalidate itself contributes
  // nothing to the decision.
  EXPECT_TRUE(AuthzCacheGateAllows(true, Meta::kCCOriginHeaderPresent |
                                             Meta::kCCOriginMustRevalidate |
                                             Meta::kCCOriginProxyRevalidate));
}

// --- AuthzCacheGateAllowsStale: every stale reuse of a stored entry ---
// Two call sites consume this decision: the bounded-SWR coalesced stale
// serve (content handler) and the stale-if-error serve (header filter,
// via ngx_http_pagespeed_stale_on_error_permitted).  Same rule for both:
// once stale, Authorization requires `public`.

TEST(AuthzCacheGateStaleTest, NoAuthorizationAlwaysAllowed) {
  // Anonymous requests keep the pre-existing bounded-stale behavior.
  EXPECT_TRUE(AuthzCacheGateAllowsStale(false, 0));
  EXPECT_TRUE(AuthzCacheGateAllowsStale(false, Meta::kCCOriginHeaderPresent));
  EXPECT_TRUE(AuthzCacheGateAllowsStale(
      false, Meta::kCCOriginHeaderPresent | Meta::kCCOriginMustRevalidate));
}

TEST(AuthzCacheGateStaleTest, AuthorizationWithPublicAllowed) {
  EXPECT_TRUE(AuthzCacheGateAllowsStale(
      true, Meta::kCCOriginHeaderPresent | Meta::kCCOriginPublic));
}

TEST(AuthzCacheGateStaleTest, AuthorizationWithMustRevalidateOnlyRefused) {
  // Once stale, must-revalidate requires revalidation before reuse
  // (§4.2.4): the §3.5 store/serve permit does not extend to the
  // coalesced stale serve.
  EXPECT_FALSE(AuthzCacheGateAllowsStale(
      true, Meta::kCCOriginHeaderPresent | Meta::kCCOriginMustRevalidate));
}

TEST(AuthzCacheGateStaleTest, AuthorizationWithSMaxageOnlyRefused) {
  // Once stale, s-maxage requires revalidation before reuse (§5.2.2.10).
  EXPECT_FALSE(AuthzCacheGateAllowsStale(
      true, Meta::kCCOriginHeaderPresent | Meta::kCCOriginSMaxagePresent));
}

TEST(AuthzCacheGateStaleTest, AuthorizationWithNoCCSignalRefused) {
  EXPECT_FALSE(AuthzCacheGateAllowsStale(true, /*origin_cc_flags=*/0));
}

TEST(AuthzCacheGateStaleTest, AuthorizationWithMaxAgeOnlyRefused) {
  // max-age alone is no §3.5 permit at all (it travels out-of-band of the
  // flags), so it cannot permit stale reuse either.
  EXPECT_FALSE(AuthzCacheGateAllowsStale(true, Meta::kCCOriginHeaderPresent));
}

TEST(AuthzCacheGateStaleTest, PublicAlongsideRevalidateDirectivesAllowed) {
  // `public` keeps permitting alongside must-revalidate/s-maxage.  (The
  // stale-if-error window check refuses revalidate-carrying entries
  // before this function runs; the kernel contract is pinned regardless.)
  EXPECT_TRUE(AuthzCacheGateAllowsStale(
      true, Meta::kCCOriginHeaderPresent | Meta::kCCOriginPublic |
                Meta::kCCOriginMustRevalidate | Meta::kCCOriginSMaxagePresent));
}

TEST(AuthzCacheGateStaleTest, CombinedRevalidatePermitsWithoutPublicRefused) {
  // must-revalidate and s-maxage are STORE permits, not stale-reuse
  // permits — combining them does not add up to one.
  EXPECT_FALSE(AuthzCacheGateAllowsStale(
      true, Meta::kCCOriginHeaderPresent | Meta::kCCOriginMustRevalidate |
                Meta::kCCOriginSMaxagePresent));
}

TEST(StaleServePermittedTest, AnonymousMustRevalidateRefused) {
  EXPECT_FALSE(StaleServePermitted(
      false, Meta::kCCOriginHeaderPresent | Meta::kCCOriginMustRevalidate));
}

TEST(StaleServePermittedTest, AnonymousSMaxageRefused) {
  EXPECT_FALSE(StaleServePermitted(
      false, Meta::kCCOriginHeaderPresent | Meta::kCCOriginSMaxagePresent));
}

TEST(StaleServePermittedTest, AnonymousProxyRevalidateRefused) {
  EXPECT_FALSE(StaleServePermitted(
      false, Meta::kCCOriginHeaderPresent | Meta::kCCOriginProxyRevalidate));
}

TEST(StaleServePermittedTest, AnonymousPublicAllowed) {
  EXPECT_TRUE(StaleServePermitted(
      false, Meta::kCCOriginHeaderPresent | Meta::kCCOriginPublic));
}

TEST(StaleServePermittedTest, AuthorizedPublicAllowed) {
  EXPECT_TRUE(StaleServePermitted(
      true, Meta::kCCOriginHeaderPresent | Meta::kCCOriginPublic));
}

TEST(StaleServePermittedTest, AuthorizedPublicMustRevalidateRefused) {
  EXPECT_FALSE(StaleServePermitted(true, Meta::kCCOriginHeaderPresent |
                                             Meta::kCCOriginPublic |
                                             Meta::kCCOriginMustRevalidate));
}

}  // namespace
}  // namespace pagespeed
