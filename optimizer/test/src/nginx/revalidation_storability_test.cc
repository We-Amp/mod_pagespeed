// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

// Tests for the conditional-revalidation (304) restamp storability gate
// (src/nginx/revalidation_storability.h), issue #1016: a 304 whose
// refreshed Cache-Control makes the representation non-storable in a
// shared cache must evict the stored entry rather than restamp it, and a
// 304 that merely withdraws this exchange's permission to extend the
// entry's life must decline the restamp without destroying the entry.
//
// The downstream Cache-Control emission on the evict path is covered by
// the forward_origin_restrictions cases in
// test/lib/cache/cache_control_header_test.cc.
//
// NOT covered anywhere (module wiring, integration-only): the actual
// cache->Remove() call and the write-back suppression.  Both live in
// ngx_pagespeed_module.cc, whose 304 branch carries no pure-function seam
// beyond this kernel — so the eviction ACTUALLY firing is asserted by no
// test at present.  That is the residual risk in this change.

#include "src/nginx/revalidation_storability.h"

#include "gtest/gtest.h"
#include "lib/classify/alternate_metadata.h"

namespace pagespeed {
namespace {

using Meta = AlternateMetadata;

// A plain, storable public response: the baseline every case varies from.
constexpr uint16_t kPublicFresh =
    Meta::kCCOriginHeaderPresent | Meta::kCCOriginPublic;

RestampStorabilityInputs Anonymous(uint16_t flags) {
  RestampStorabilityInputs in;
  in.refreshed_cc_flags = flags;
  return in;
}

RestampStorabilityInputs Authorized(uint16_t flags) {
  RestampStorabilityInputs in;
  in.refreshed_cc_flags = flags;
  in.request_has_authorization = true;
  return in;
}

// --- The core defect: no-store / private on the 304 -----------------------

TEST(RevalidationStorabilityTest, NoStoreOn304Evicts) {
  // The origin now says this must not be stored at all.  Declining the
  // write-back is insufficient — the previously stored copy is exactly
  // what may not be retained.
  EXPECT_EQ(RestampVerdict::kEvict,
            EvaluateRestampStorability(Anonymous(Meta::kCCOriginHeaderPresent |
                                                 Meta::kCCOriginNoStore)));
}

TEST(RevalidationStorabilityTest, PrivateOn304Evicts) {
  EXPECT_EQ(RestampVerdict::kEvict,
            EvaluateRestampStorability(Anonymous(Meta::kCCOriginHeaderPresent |
                                                 Meta::kCCOriginPrivate |
                                                 Meta::kCCOriginPrivateBare)));
}

TEST(RevalidationStorabilityTest, NoStoreOn304EvictsForAuthorizedToo) {
  EXPECT_EQ(RestampVerdict::kEvict,
            EvaluateRestampStorability(Authorized(Meta::kCCOriginHeaderPresent |
                                                  Meta::kCCOriginNoStore)));
}

TEST(RevalidationStorabilityTest, PublicTurningPrivateEvicts) {
  // The transition that matters operationally: an entry stored while the
  // resource was public, revalidated after the origin made it private.
  // `public` alongside `private` must not rescue it — the pure function
  // has no ordering preference to fall back on, so no-store/private wins.
  EXPECT_EQ(RestampVerdict::kEvict, EvaluateRestampStorability(Anonymous(
                                        kPublicFresh | Meta::kCCOriginPrivate |
                                        Meta::kCCOriginPrivateBare)));
}

TEST(RevalidationStorabilityTest, NoStoreDominatesEveryPermit) {
  EXPECT_EQ(RestampVerdict::kEvict,
            EvaluateRestampStorability(Authorized(
                Meta::kCCOriginHeaderPresent | Meta::kCCOriginPublic |
                Meta::kCCOriginMustRevalidate | Meta::kCCOriginSMaxagePresent |
                Meta::kCCOriginNoStore)));
}

TEST(RevalidationStorabilityTest, NoStoreDominatesSetCookie) {
  // Both non-storable signals present: the destructive verdict wins, so a
  // personalization signal can never downgrade an eviction to a skip.
  RestampStorabilityInputs in =
      Anonymous(Meta::kCCOriginHeaderPresent | Meta::kCCOriginNoStore);
  in.response_has_set_cookie = true;
  EXPECT_EQ(RestampVerdict::kEvict, EvaluateRestampStorability(in));
}

// --- Still storable: the restamp proceeds unchanged ------------------------

TEST(RevalidationStorabilityTest, PublicResponseRestamps) {
  EXPECT_EQ(RestampVerdict::kRestamp,
            EvaluateRestampStorability(Anonymous(kPublicFresh)));
}

TEST(RevalidationStorabilityTest, PlainMaxAgeAnonymousRestamps) {
  // No permit signal at all is fine for an anonymous request — §3.5 does
  // not apply, and nothing forbids storage.
  EXPECT_EQ(
      RestampVerdict::kRestamp,
      EvaluateRestampStorability(Anonymous(Meta::kCCOriginHeaderPresent)));
}

TEST(RevalidationStorabilityTest, NoCacheStillRestamps) {
  // `no-cache` means revalidate-before-reuse, NOT do-not-store: the entry
  // stays, and the pre-existing serve path already forces revalidation.
  EXPECT_EQ(RestampVerdict::kRestamp,
            EvaluateRestampStorability(Anonymous(Meta::kCCOriginHeaderPresent |
                                                 Meta::kCCOriginNoCache)));
}

TEST(RevalidationStorabilityTest, ThreeOhFourWithoutCacheControlRestamps) {
  // A 304 carrying NO Cache-Control leaves the stored flags in place, and
  // the caller passes those through unchanged.  Distinct from
  // PublicResponseRestamps: no kCCOriginHeaderPresent, which is exactly
  // what "the origin sent no Cache-Control" looks like in this vocabulary.
  EXPECT_EQ(RestampVerdict::kRestamp, EvaluateRestampStorability(Anonymous(0)));
  // ...and with only a stored max-age, still no CC header on the 304.
  EXPECT_EQ(RestampVerdict::kRestamp,
            EvaluateRestampStorability(Anonymous(Meta::kCCOriginPublic)));
}

// --- Qualified private/no-cache: restrict fields, do NOT destroy ----------

TEST(RevalidationStorabilityTest, QualifiedPrivateDoesNotEvict) {
  // RFC 9111 §5.2.2.7: `private="Set-Cookie"` restricts only the named
  // field-names and explicitly permits storing the remainder.  Evicting on
  // it would destroy the entry plus every worker-derived variant on EVERY
  // revalidation — permanent cache thrash for a common CDN idiom.
  const uint16_t qualified_private = Meta::kCCOriginHeaderPresent |
                                     Meta::kCCOriginPrivate |
                                     Meta::kCCOriginPrivateQualified;
  EXPECT_FALSE(RefreshedHeadersForbidSharedStorage(qualified_private));
  EXPECT_EQ(RestampVerdict::kRestamp,
            EvaluateRestampStorability(Anonymous(qualified_private)));
}

TEST(RevalidationStorabilityTest, QualifiedPrivateWithMaxAgeRestamps) {
  // The exact shape that drives the thrash loop:
  // `Cache-Control: private="Set-Cookie", max-age=600`.
  //
  // Note the real sequence this stands in for — the MISS path refuses to
  // store ANY response flagged private, qualified or not, so the entry at
  // risk is never one stored under this header.  It is an entry stored
  // earlier (or a worker-derived variant) that the origin only later began
  // qualifying.  These are the MERGED flags at restamp time, which is
  // exactly where that sequence lands, so the case is reachable in
  // production even though a first store under these headers is not.
  EXPECT_EQ(RestampVerdict::kRestamp,
            EvaluateRestampStorability(Anonymous(
                Meta::kCCOriginHeaderPresent | Meta::kCCOriginPrivate |
                Meta::kCCOriginPrivateQualified | Meta::kCCOriginPublic)));
}

TEST(RevalidationStorabilityTest, QualifiedPrivateWithNoStoreStillEvicts) {
  // A qualifier on `private` does not rescue a co-present bare `no-store`.
  EXPECT_EQ(RestampVerdict::kEvict,
            EvaluateRestampStorability(Anonymous(
                Meta::kCCOriginHeaderPresent | Meta::kCCOriginPrivate |
                Meta::kCCOriginPrivateQualified | Meta::kCCOriginNoStore)));
}

TEST(RevalidationStorabilityTest,
     BarePrivateStillEvictsAlongsideQualifiedNoCache) {
  // Qualifier flags are per-directive: a qualified `no-cache` must not be
  // mistaken for a qualifier on `private`.
  EXPECT_EQ(RestampVerdict::kEvict,
            EvaluateRestampStorability(Anonymous(
                Meta::kCCOriginHeaderPresent | Meta::kCCOriginPrivate |
                Meta::kCCOriginNoCache | Meta::kCCOriginNoCacheQualified)));
}

TEST(RevalidationStorabilityTest, BarePrivateOnASecondHeaderLineStillEvicts) {
  // The OR-accumulation hazard.  cc_flags is accumulated across every token
  // of every Cache-Control header line, so:
  //     Cache-Control: private="Set-Cookie"
  //     Cache-Control: private
  // (origin sends the qualified form; an intermediary or an nginx
  // add_header appends a bare one) sets BOTH form bits.  Keying the
  // decision on "qualified seen" would read that as fully qualified and
  // skip the eviction a bare `private` plainly demands — under-eviction,
  // i.e. the original security bug, in the exact configuration the form
  // bits were added to disambiguate.  "Bare seen" is the monotone signal.
  const uint16_t both_forms =
      Meta::kCCOriginHeaderPresent | Meta::kCCOriginPrivate |
      Meta::kCCOriginPrivateQualified | Meta::kCCOriginPrivateBare;
  EXPECT_TRUE(RefreshedHeadersForbidSharedStorage(both_forms));
  EXPECT_EQ(RestampVerdict::kEvict,
            EvaluateRestampStorability(Anonymous(both_forms)));
}

TEST(RevalidationStorabilityTest, LegacyMetadataWithoutFormBitsEvicts) {
  // Metadata written before the form bits existed carries `private` with
  // neither bit set — no per-occurrence information.  Fail safe: treat as
  // blanket, matching the earlier blanket conservatism.
  const uint16_t legacy = Meta::kCCOriginHeaderPresent | Meta::kCCOriginPrivate;
  EXPECT_TRUE(RefreshedHeadersForbidSharedStorage(legacy));
  EXPECT_EQ(RestampVerdict::kEvict,
            EvaluateRestampStorability(Anonymous(legacy)));
}

TEST(RevalidationStorabilityTest, PrivateAndSetCookieDominance) {
  // Both a destructive and a personalization signal: evict wins, so the
  // entry is destroyed rather than merely left un-refreshed.
  RestampStorabilityInputs in =
      Anonymous(Meta::kCCOriginHeaderPresent | Meta::kCCOriginPrivate |
                Meta::kCCOriginPrivateBare);
  in.response_has_set_cookie = true;
  EXPECT_EQ(RestampVerdict::kEvict, EvaluateRestampStorability(in));

  // But with the QUALIFIED form the Set-Cookie signal is what remains, and
  // it must degrade to a skip — never to an eviction.
  RestampStorabilityInputs qual =
      Anonymous(Meta::kCCOriginHeaderPresent | Meta::kCCOriginPrivate |
                Meta::kCCOriginPrivateQualified);
  qual.response_has_set_cookie = true;
  EXPECT_EQ(RestampVerdict::kSkipRestamp, EvaluateRestampStorability(qual));
}

// --- §3.5: withdraw the permit, decline the life-extension ----------------

TEST(RevalidationStorabilityTest, AuthorizedWithPermitRestamps) {
  EXPECT_EQ(RestampVerdict::kRestamp,
            EvaluateRestampStorability(Authorized(kPublicFresh)));
  EXPECT_EQ(RestampVerdict::kRestamp,
            EvaluateRestampStorability(Authorized(
                Meta::kCCOriginHeaderPresent | Meta::kCCOriginMustRevalidate)));
  EXPECT_EQ(RestampVerdict::kRestamp,
            EvaluateRestampStorability(Authorized(
                Meta::kCCOriginHeaderPresent | Meta::kCCOriginSMaxagePresent)));
}

TEST(RevalidationStorabilityTest, AuthorizedWithdrawnPermitSkipsRestamp) {
  // The 304 dropped every §3.5 permit: this authenticated exchange must
  // not extend the entry's life.  Crucially NOT kEvict — the entry may
  // have been stored lawfully for anonymous traffic, and evicting on an
  // inbound request header would be a one-request shared-cache flush.
  EXPECT_EQ(
      RestampVerdict::kSkipRestamp,
      EvaluateRestampStorability(Authorized(Meta::kCCOriginHeaderPresent)));
}

TEST(RevalidationStorabilityTest, AuthorizedWithNoCacheSignalSkipsRestamp) {
  EXPECT_EQ(RestampVerdict::kSkipRestamp,
            EvaluateRestampStorability(Authorized(0)));
}

TEST(RevalidationStorabilityTest, AnonymousWithNoCacheSignalRestamps) {
  // Same flags, no Authorization: unchanged pre-fix behavior.
  EXPECT_EQ(RestampVerdict::kRestamp, EvaluateRestampStorability(Anonymous(0)));
}

TEST(RevalidationStorabilityTest, ProxyRevalidateIsNotAPermit) {
  // Mirrors the §3.5 gate's exhaustive permit list.
  EXPECT_EQ(
      RestampVerdict::kSkipRestamp,
      EvaluateRestampStorability(Authorized(Meta::kCCOriginHeaderPresent |
                                            Meta::kCCOriginProxyRevalidate)));
}

// --- Set-Cookie on the 304 -------------------------------------------------

TEST(RevalidationStorabilityTest, SetCookieOn304SkipsRestamp) {
  RestampStorabilityInputs in = Anonymous(kPublicFresh);
  in.response_has_set_cookie = true;
  EXPECT_EQ(RestampVerdict::kSkipRestamp, EvaluateRestampStorability(in));
}

// --- Cookie-bearing exchange ---------------------------------------------

RestampStorabilityInputs CookiePage(uint16_t flags, uint32_t s_maxage = 0) {
  RestampStorabilityInputs in = Anonymous(flags);
  in.refreshed_s_maxage = s_maxage;
  in.refreshed_max_age = 600;
  in.request_has_cookie = true;
  in.entry_class = CookieShareClass::kPage;
  return in;
}

TEST(RevalidationStorabilityTest, CookieRequestKeepsSharedPageFresh) {
  EXPECT_EQ(RestampVerdict::kRestamp,
            EvaluateRestampStorability(CookiePage(kPublicFresh)));
  EXPECT_EQ(
      RestampVerdict::kRestamp,
      EvaluateRestampStorability(CookiePage(
          Meta::kCCOriginHeaderPresent | Meta::kCCOriginSMaxagePresent, 600)));
}

TEST(RevalidationStorabilityTest, CookieRequestWithWithdrawnMarkSkipsRestamp) {
  // Plain max-age: storable for anonymous traffic, but a cookie-bearing
  // exchange may not extend it.  Not an eviction.
  EXPECT_EQ(
      RestampVerdict::kSkipRestamp,
      EvaluateRestampStorability(CookiePage(Meta::kCCOriginHeaderPresent)));
  // must-revalidate is a §3.5 permit but does not mark the page shareable.
  EXPECT_EQ(RestampVerdict::kSkipRestamp,
            EvaluateRestampStorability(CookiePage(
                Meta::kCCOriginHeaderPresent | Meta::kCCOriginMustRevalidate)));
  // s-maxage=0 is not a mark.
  EXPECT_EQ(
      RestampVerdict::kSkipRestamp,
      EvaluateRestampStorability(CookiePage(
          Meta::kCCOriginHeaderPresent | Meta::kCCOriginSMaxagePresent, 0)));
  // no-store still dominates.
  EXPECT_EQ(RestampVerdict::kEvict,
            EvaluateRestampStorability(CookiePage(Meta::kCCOriginHeaderPresent |
                                                  Meta::kCCOriginNoStore)));
}

TEST(RevalidationStorabilityTest, CookieRequestOnStaticAssetNeedsALifetime) {
  RestampStorabilityInputs in = CookiePage(Meta::kCCOriginHeaderPresent);
  in.entry_class = CookieShareClass::kStaticAsset;
  EXPECT_EQ(RestampVerdict::kRestamp, EvaluateRestampStorability(in));
  in.refreshed_max_age = 0;
  EXPECT_EQ(RestampVerdict::kSkipRestamp, EvaluateRestampStorability(in));
}

TEST(RevalidationStorabilityTest, NoCookieLeavesPlainPageRestamping) {
  EXPECT_EQ(
      RestampVerdict::kRestamp,
      EvaluateRestampStorability(Anonymous(Meta::kCCOriginHeaderPresent)));
}

// --- The downstream-advertising helper ------------------------------------

TEST(RevalidationStorabilityTest, ForbidsSharedStoragePredicate) {
  EXPECT_TRUE(RefreshedHeadersForbidSharedStorage(Meta::kCCOriginNoStore));
  EXPECT_TRUE(RefreshedHeadersForbidSharedStorage(Meta::kCCOriginPrivate |
                                                  Meta::kCCOriginPrivateBare));
  EXPECT_TRUE(RefreshedHeadersForbidSharedStorage(kPublicFresh |
                                                  Meta::kCCOriginNoStore));
  EXPECT_FALSE(RefreshedHeadersForbidSharedStorage(kPublicFresh));
  EXPECT_FALSE(RefreshedHeadersForbidSharedStorage(0));
  EXPECT_FALSE(RefreshedHeadersForbidSharedStorage(Meta::kCCOriginNoCache));
  // Qualified private is not a blanket restriction...
  EXPECT_FALSE(RefreshedHeadersForbidSharedStorage(
      Meta::kCCOriginPrivate | Meta::kCCOriginPrivateQualified));
  // ...but a qualified no-store is not a thing: no-store takes no argument,
  // so the bare flag always forbids regardless of other qualifiers.
  EXPECT_TRUE(RefreshedHeadersForbidSharedStorage(
      Meta::kCCOriginNoStore | Meta::kCCOriginPrivateQualified));
}

}  // namespace
}  // namespace pagespeed
