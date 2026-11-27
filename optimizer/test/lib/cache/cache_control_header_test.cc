// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

#include "lib/cache/cache_control_header.h"

#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>

#include "gtest/gtest.h"
#include "lib/classify/alternate_metadata.h"

namespace pagespeed {
namespace {

using AM = AlternateMetadata;

class CacheControlHeaderTest : public ::testing::Test {
 protected:
  // Returns a default CacheControlInput for safe mode.
  CacheControlInput MakeSafe() {
    CacheControlInput input{};
    input.mode = CacheMode::kSafe;
    input.origin_cc_flags = AM::kCCOriginHeaderPresent;
    input.effective_max_age = 300;
    input.origin_max_age = 300;
    input.content_type = ContentType::kCss;
    return input;
  }

  // Returns a default CacheControlInput for aggressive mode.
  CacheControlInput MakeAggressive() {
    CacheControlInput input{};
    input.mode = CacheMode::kAggressive;
    input.origin_cc_flags = AM::kCCOriginHeaderPresent | AM::kCCOriginPublic;
    input.effective_max_age = 86400;
    input.origin_max_age = 86400;
    input.content_type = ContentType::kCss;
    return input;
  }

  // Build header and return as string for easy assertions.
  std::string Build(const CacheControlInput& input) {
    char buf[256];
    auto result = BuildCacheControlHeader(input, buf, sizeof(buf));
    return std::string(buf, result.len);
  }

  // Check whether needle is found in the built header.
  bool Contains(const std::string& header, const char* needle) {
    return header.find(needle) != std::string::npos;
  }
};

// --- Safe mode tests ---

TEST_F(CacheControlHeaderTest, SafeCssContainsMustRevalidate) {
  auto input = MakeSafe();
  input.content_type = ContentType::kCss;
  auto hdr = Build(input);
  EXPECT_TRUE(Contains(hdr, "must-revalidate"));
}

TEST_F(CacheControlHeaderTest, SafeCssNoSWR) {
  auto input = MakeSafe();
  input.synthesize_swr = true;  // Even when configured...
  auto hdr = Build(input);
  EXPECT_FALSE(Contains(hdr, "stale-while-revalidate"));
}

TEST_F(CacheControlHeaderTest, SafeCssNoImmutable) {
  auto input = MakeSafe();
  input.origin_cc_flags |= AM::kCCOriginImmutable;
  auto hdr = Build(input);
  EXPECT_FALSE(Contains(hdr, "immutable"));
}

TEST_F(CacheControlHeaderTest, SafeCssNoPublic) {
  auto input = MakeSafe();
  input.origin_cc_flags |= AM::kCCOriginPublic;
  auto hdr = Build(input);
  EXPECT_FALSE(Contains(hdr, "public"));
}

TEST_F(CacheControlHeaderTest, SafeImageContainsMustRevalidate) {
  auto input = MakeSafe();
  input.content_type = ContentType::kImage;
  auto hdr = Build(input);
  EXPECT_TRUE(Contains(hdr, "must-revalidate"));
}

TEST_F(CacheControlHeaderTest, SafeOriginMaxAge60) {
  auto input = MakeSafe();
  input.effective_max_age = 60;
  input.origin_max_age = 60;
  auto hdr = Build(input);
  EXPECT_TRUE(Contains(hdr, "max-age=60"));
}

TEST_F(CacheControlHeaderTest, SafeOriginImmutableFlagStripped) {
  auto input = MakeSafe();
  input.origin_cc_flags |= AM::kCCOriginImmutable;
  auto hdr = Build(input);
  EXPECT_FALSE(Contains(hdr, "immutable"));
}

TEST_F(CacheControlHeaderTest, SafeOriginPrivateNoPublic) {
  auto input = MakeSafe();
  input.origin_cc_flags |= AM::kCCOriginPrivate | AM::kCCOriginPublic;
  auto hdr = Build(input);
  EXPECT_FALSE(Contains(hdr, "public"));
}

TEST_F(CacheControlHeaderTest, SafeSWRConfigOnSuppressed) {
  auto input = MakeSafe();
  input.synthesize_swr = true;  // Safe mode never synthesizes SWR.
  auto hdr = Build(input);
  EXPECT_FALSE(Contains(hdr, "stale-while-revalidate"));
}

// --- Aggressive mode tests ---

TEST_F(CacheControlHeaderTest, AggressiveCssContainsPublic) {
  auto input = MakeAggressive();
  auto hdr = Build(input);
  EXPECT_TRUE(Contains(hdr, "public"));
}

TEST_F(CacheControlHeaderTest, AggressiveCssContainsStaleIfError) {
  auto input = MakeAggressive();
  auto hdr = Build(input);
  EXPECT_TRUE(Contains(hdr, "stale-if-error=86400"));
}

TEST_F(CacheControlHeaderTest, AggressiveOriginMustRevalidatePreserved) {
  auto input = MakeAggressive();
  input.origin_cc_flags |= AM::kCCOriginMustRevalidate;
  auto hdr = Build(input);
  EXPECT_TRUE(Contains(hdr, "must-revalidate"));
}

TEST_F(CacheControlHeaderTest, AggressiveOriginImmutableStripped) {
  auto input = MakeAggressive();
  input.origin_cc_flags |= AM::kCCOriginImmutable;
  input.effective_max_age = 604800;
  auto hdr = Build(input);
  EXPECT_FALSE(Contains(hdr, "immutable"));
  // But long TTL preserved.
  EXPECT_TRUE(Contains(hdr, "max-age=604800"));
}

TEST_F(CacheControlHeaderTest, AggressiveOriginPrivateNoPublic) {
  auto input = MakeAggressive();
  input.origin_cc_flags |= AM::kCCOriginPrivate;
  auto hdr = Build(input);
  EXPECT_FALSE(Contains(hdr, "public"));
}

TEST_F(CacheControlHeaderTest, AggressiveSWRPresentWhenEnabled) {
  auto input = MakeAggressive();
  input.synthesize_swr = true;  // No revalidation-required flags set.
  auto hdr = Build(input);
  EXPECT_TRUE(Contains(hdr, "stale-while-revalidate="));
}

TEST_F(CacheControlHeaderTest,
       AggressiveSWRSuppressedWhenRevalidationRequired) {
  auto input = MakeAggressive();
  input.synthesize_swr = true;
  input.origin_cc_flags |= AM::kCCOriginMustRevalidate;
  auto hdr = Build(input);
  EXPECT_FALSE(Contains(hdr, "stale-while-revalidate"));
}

TEST_F(CacheControlHeaderTest,
       AggressiveStaleIfErrorSuppressedWhenRevalidationRequired) {
  // RFC 9111 §5.2.2.11: stale-if-error conflicts with must-revalidate.
  auto input = MakeAggressive();
  input.origin_cc_flags |= AM::kCCOriginMustRevalidate;
  auto hdr = Build(input);
  EXPECT_FALSE(Contains(hdr, "stale-if-error"));
}

TEST_F(CacheControlHeaderTest,
       AggressiveBothStaleDirectivesSuppressedWhenRevalidationRequired) {
  // Combined case: must-revalidate origin + SWR synthesis enabled should
  // suppress both stale-while-revalidate and stale-if-error together
  // (RFC 9111 §4.2.4).
  auto input = MakeAggressive();
  input.synthesize_swr = true;
  input.origin_cc_flags |= AM::kCCOriginMustRevalidate;
  auto hdr = Build(input);
  EXPECT_FALSE(Contains(hdr, "stale-while-revalidate"));
  EXPECT_FALSE(Contains(hdr, "stale-if-error"));
  // must-revalidate should still be emitted (origin sent it).
  EXPECT_TRUE(Contains(hdr, "must-revalidate"));
}

TEST_F(CacheControlHeaderTest,
       AggressivePrivateCacheIgnoresSMaxageForStaleIfError) {
  // In a private-cache integration (is_shared_cache=false), origin
  // s-maxage must NOT suppress stale-if-error (RFC 9111 §5.2.2.9 limits
  // s-maxage to shared caches). Regression guard for issue #260.
  auto input = MakeAggressive();
  input.is_shared_cache = false;
  input.origin_cc_flags |= AM::kCCOriginSMaxagePresent;
  auto hdr = Build(input);
  EXPECT_TRUE(Contains(hdr, "stale-if-error"));
}

TEST_F(CacheControlHeaderTest,
       AggressiveSharedCacheSMaxageSuppressesStaleIfError) {
  // Same origin flags with is_shared_cache=true (the nginx default) must
  // suppress stale-if-error. Pairs with the private-cache test above.
  auto input = MakeAggressive();
  input.is_shared_cache = true;
  input.origin_cc_flags |= AM::kCCOriginSMaxagePresent;
  auto hdr = Build(input);
  EXPECT_FALSE(Contains(hdr, "stale-if-error"));
}

// --- Both modes tests ---

TEST_F(CacheControlHeaderTest, BothOriginNoStoreNoPublic) {
  // Safe mode.
  {
    auto input = MakeSafe();
    input.origin_cc_flags |= AM::kCCOriginNoStore | AM::kCCOriginPublic;
    auto hdr = Build(input);
    EXPECT_FALSE(Contains(hdr, "public"));
  }
  // Aggressive mode.
  {
    auto input = MakeAggressive();
    input.origin_cc_flags |= AM::kCCOriginNoStore;
    auto hdr = Build(input);
    EXPECT_FALSE(Contains(hdr, "public"));
  }
}

TEST_F(CacheControlHeaderTest, BothImmutableAlwaysStripped) {
  for (auto mode : {CacheMode::kSafe, CacheMode::kAggressive}) {
    CacheControlInput input{};
    input.mode = mode;
    input.origin_cc_flags = AM::kCCOriginHeaderPresent | AM::kCCOriginImmutable;
    input.effective_max_age = 3600;
    input.origin_max_age = 3600;
    if (mode == CacheMode::kAggressive) {
      input.origin_cc_flags |= AM::kCCOriginPublic;
    }
    auto hdr = Build(input);
    EXPECT_FALSE(Contains(hdr, "immutable"))
        << "mode=" << static_cast<int>(mode);
  }
}

TEST_F(CacheControlHeaderTest, PreservesProxyRevalidate) {
  auto input = MakeAggressive();
  input.origin_cc_flags |= AM::kCCOriginProxyRevalidate;
  auto hdr = Build(input);
  EXPECT_TRUE(Contains(hdr, "proxy-revalidate"));
}

TEST_F(CacheControlHeaderTest, PreservesNoTransform) {
  auto input = MakeSafe();
  input.origin_cc_flags |= AM::kCCOriginNoTransform;
  auto hdr = Build(input);
  EXPECT_TRUE(Contains(hdr, "no-transform"));
}

TEST_F(CacheControlHeaderTest, SMaxageSplitBrowserMaxAge) {
  // When origin sent s-maxage, browser-facing max-age comes from origin_max_age.
  auto input = MakeSafe();
  input.origin_cc_flags |= AM::kCCOriginSMaxagePresent;
  input.effective_max_age = 86400;  // s-maxage derived
  input.origin_max_age = 60;        // browser-facing
  auto hdr = Build(input);
  EXPECT_TRUE(Contains(hdr, "s-maxage=86400"));
  EXPECT_TRUE(Contains(hdr, "max-age=60"));
}

TEST_F(CacheControlHeaderTest, NullBufferReturnsZeroLen) {
  CacheControlInput input{};
  auto result = BuildCacheControlHeader(input, nullptr, 0);
  EXPECT_EQ(result.len, 0u);
}

TEST_F(CacheControlHeaderTest, SafeNoStaleIfError) {
  auto input = MakeSafe();
  auto hdr = Build(input);
  EXPECT_FALSE(Contains(hdr, "stale-if-error"));
}

TEST_F(CacheControlHeaderTest, AggressiveNoMustRevalidateUnlessOrigin) {
  // Aggressive without origin must-revalidate should not add it.
  auto input = MakeAggressive();
  auto hdr = Build(input);
  EXPECT_FALSE(Contains(hdr, "must-revalidate"));
}

// --- Edge case tests ---

TEST_F(CacheControlHeaderTest, MaxAgeUINT32Max) {
  auto input = MakeSafe();
  input.effective_max_age = UINT32_MAX;
  input.origin_max_age = UINT32_MAX;
  auto hdr = Build(input);
  // snprintf should format the large value correctly.
  EXPECT_TRUE(Contains(hdr, "max-age=4294967295"));
  EXPECT_TRUE(Contains(hdr, "must-revalidate"));
}

TEST_F(CacheControlHeaderTest, SafeEmptyFlagsOnlyEmitsMaxAge) {
  CacheControlInput input{};
  input.mode = CacheMode::kSafe;
  input.origin_cc_flags = 0;  // No flags at all.
  input.effective_max_age = 300;
  input.origin_max_age = 300;
  auto hdr = Build(input);
  // Safe mode always adds must-revalidate even with no origin flags.
  EXPECT_TRUE(Contains(hdr, "must-revalidate"));
  EXPECT_TRUE(Contains(hdr, "max-age=300"));
  // No public, no proxy-revalidate, no no-transform, no s-maxage.
  EXPECT_FALSE(Contains(hdr, "public"));
  EXPECT_FALSE(Contains(hdr, "proxy-revalidate"));
  EXPECT_FALSE(Contains(hdr, "no-transform"));
  EXPECT_FALSE(Contains(hdr, "s-maxage"));
}

TEST_F(CacheControlHeaderTest, AggressiveEmptyFlagsNoPublic) {
  CacheControlInput input{};
  input.mode = CacheMode::kAggressive;
  input.origin_cc_flags = 0;  // No flags — notably no kCCOriginPublic.
  input.effective_max_age = 86400;
  input.origin_max_age = 86400;
  auto hdr = Build(input);
  // public requires kCCOriginPublic flag — should not appear.
  EXPECT_FALSE(Contains(hdr, "public"));
  // stale-if-error present when revalidation_required is false (default).
  EXPECT_TRUE(Contains(hdr, "stale-if-error=86400"));
  // must-revalidate not added without origin flag.
  EXPECT_FALSE(Contains(hdr, "must-revalidate"));
  EXPECT_TRUE(Contains(hdr, "max-age=86400"));
}

TEST_F(CacheControlHeaderTest, SmallBufferTruncatesSafely) {
  auto input = MakeSafe();
  char buf[10];  // Too small for full output.
  auto result = BuildCacheControlHeader(input, buf, sizeof(buf));
  // Directives that don't fit entirely should be skipped, not truncated.
  std::string header(buf, result.len);
  // Must not contain partial directives like "must-re" or "max-ag".
  // Every comma-separated token must be a complete directive.
  EXPECT_EQ(header.find("must-re"), std::string::npos)
      << "Truncated directive found: " << header;
}

TEST_F(CacheControlHeaderTest, NoPartialDirectivesInTightBuffer) {
  // Buffer just big enough for "must-revalidate" (16 chars) but not
  // "must-revalidate, max-age=300" (28 chars).
  auto input = MakeSafe();
  input.effective_max_age = 300;
  char buf[20];
  auto result = BuildCacheControlHeader(input, buf, sizeof(buf));
  std::string header(buf, result.len);
  // "must-revalidate" fits (16 chars). "max-age=300" needs 13 more
  // (", max-age=300" = 13) → total 29 > 20, so max-age is skipped.
  if (header.find("max-age") != std::string::npos) {
    // If max-age IS present, it must be complete (not "max-ag" or similar).
    EXPECT_NE(header.find("max-age="), std::string::npos)
        << "Truncated max-age found: " << header;
  }
}

// --- Equivalence: ComputeSharedRevalidationRequired exhaustive table ---
//
// Issue #260: lock in that the single shared helper matches the explicit
// RFC 9111 rules across every combination of the four relevant origin
// Cache-Control flags × is_shared_cache. Any future edit to the helper
// that breaks this table will fail this test.
TEST(ComputeSharedRevalidationRequired, ExhaustiveFlagMatrix) {
  constexpr uint16_t kNoCache = AM::kCCOriginNoCache;
  constexpr uint16_t kMustRev = AM::kCCOriginMustRevalidate;
  constexpr uint16_t kProxyRev = AM::kCCOriginProxyRevalidate;
  constexpr uint16_t kSMaxage = AM::kCCOriginSMaxagePresent;
  constexpr uint16_t kFlags[4] = {kNoCache, kMustRev, kProxyRev, kSMaxage};

  for (unsigned mask = 0; mask < 16; ++mask) {
    uint16_t cc = 0;
    for (unsigned bit = 0; bit < 4; ++bit) {
      if ((mask >> bit) & 1) cc |= kFlags[bit];
    }
    for (bool is_shared : {false, true}) {
      // Expected: no-cache / must-revalidate always apply. proxy-revalidate
      // and s-maxage only when is_shared_cache (RFC 9111 §5.2.2.9–10).
      const bool expected =
          (cc & kNoCache) || (cc & kMustRev) ||
          (is_shared && ((cc & kProxyRev) || (cc & kSMaxage)));
      EXPECT_EQ(ComputeSharedRevalidationRequired(cc, is_shared), expected)
          << "mask=" << mask << " is_shared=" << (is_shared ? "true" : "false");
    }
  }
}

// Private-cache regression guard: proxy-revalidate and s-maxage alone must
// NOT trigger revalidation_required in a private-cache integration. This
// is the specific footgun flagged in issue #260 — the old nginx inline
// computation ignored is_shared_cache.
TEST(ComputeSharedRevalidationRequired, PrivateCacheIgnoresSharedDirectives) {
  EXPECT_FALSE(ComputeSharedRevalidationRequired(AM::kCCOriginProxyRevalidate,
                                                 /*is_shared_cache=*/false));
  EXPECT_FALSE(ComputeSharedRevalidationRequired(AM::kCCOriginSMaxagePresent,
                                                 /*is_shared_cache=*/false));
  // Sanity: the same flags DO trigger in a shared-cache integration.
  EXPECT_TRUE(ComputeSharedRevalidationRequired(AM::kCCOriginProxyRevalidate,
                                                /*is_shared_cache=*/true));
  EXPECT_TRUE(ComputeSharedRevalidationRequired(AM::kCCOriginSMaxagePresent,
                                                /*is_shared_cache=*/true));
  // no-cache and must-revalidate apply in both modes.
  EXPECT_TRUE(ComputeSharedRevalidationRequired(AM::kCCOriginNoCache,
                                                /*is_shared_cache=*/false));
  EXPECT_TRUE(ComputeSharedRevalidationRequired(AM::kCCOriginMustRevalidate,
                                                /*is_shared_cache=*/false));
}

// --- forward_origin_restrictions (issue #1016) ---
//
// The 304-restamp path evicts an entry whose refreshed headers forbid
// shared storage, then still answers the current requester.  The directive
// that drove the eviction has to reach caches downstream of us, or the next
// one stores exactly what this one just dropped.

// --- Byte-identity golden tests (issue #1016) ---
//
// #1016 threaded two new flags through this builder.  Both default off, and
// the guarantee is that a caller which sets neither gets output byte-for-byte
// identical to the pre-hardening behavior.  Establishing that by reading every branch by hand
// has now cost two review cycles; these pin the exact strings instead, so any
// future edit that perturbs the default path fails here rather than needing a
// human to re-derive it.  Substring assertions would not do: they still pass
// if a directive is reordered, dropped, or its value changed.

TEST_F(CacheControlHeaderTest, GoldenSafeModeDefaultPath) {
  EXPECT_EQ("must-revalidate, max-age=300", Build(MakeSafe()));
}

TEST_F(CacheControlHeaderTest, GoldenAggressiveModeDefaultPath) {
  EXPECT_EQ("public, max-age=86400, stale-if-error=86400",
            Build(MakeAggressive()));
}

TEST_F(CacheControlHeaderTest, GoldenAggressiveModeWithSwr) {
  auto input = MakeAggressive();
  input.synthesize_swr = true;
  EXPECT_EQ(
      "public, max-age=86400, stale-while-revalidate=86400, "
      "stale-if-error=86400",
      Build(input));
}

TEST_F(CacheControlHeaderTest, ForwardOff_IsUnchangedForExistingCallers) {
  // Restriction flags PRESENT but forwarding off: a pre-existing caller
  // must see the pre-hardening bytes, not merely "no private/no-store".
  auto input = MakeSafe();
  input.origin_cc_flags |=
      AM::kCCOriginPrivate | AM::kCCOriginPrivateBare | AM::kCCOriginNoStore;
  EXPECT_EQ("must-revalidate, max-age=300", Build(input));
}

TEST_F(CacheControlHeaderTest, RelayOff_IsUnchangedForExistingCallers) {
  // Same for the no-cache relay: flags present, relay off, bytes unchanged.
  auto input = MakeSafe();
  input.origin_cc_flags |= AM::kCCOriginNoCache | AM::kCCOriginNoCacheBare;
  EXPECT_EQ("must-revalidate, max-age=300", Build(input));
}

TEST_F(CacheControlHeaderTest, ForwardPrivatePreservesRevalidationDirectives) {
  // The regression that motivated the fix: the hand-rolled emitter sent a
  // single bare token, dropping no-cache — and `private` permits browser
  // storage, so no-cache was the only thing forcing revalidation.
  auto input = MakeSafe();
  input.forward_origin_restrictions = true;
  input.origin_cc_flags = AM::kCCOriginHeaderPresent | AM::kCCOriginPrivate |
                          AM::kCCOriginNoCache | AM::kCCOriginNoTransform |
                          AM::kCCOriginMustRevalidate;
  auto hdr = Build(input);
  EXPECT_TRUE(Contains(hdr, "private"));
  EXPECT_TRUE(Contains(hdr, "no-cache"));
  EXPECT_TRUE(Contains(hdr, "no-transform"));
  EXPECT_TRUE(Contains(hdr, "must-revalidate"));
}

TEST_F(CacheControlHeaderTest, ForwardNoStoreSuppressesMaxAge) {
  // A freshness lifetime alongside no-store is a mixed signal.
  auto input = MakeSafe();
  input.forward_origin_restrictions = true;
  input.origin_cc_flags = AM::kCCOriginHeaderPresent | AM::kCCOriginNoStore;
  input.effective_max_age = 600;
  input.origin_max_age = 600;
  auto hdr = Build(input);
  EXPECT_TRUE(Contains(hdr, "no-store"));
  EXPECT_FALSE(Contains(hdr, "max-age"));
}

TEST_F(CacheControlHeaderTest, ForwardPrivateKeepsMaxAge) {
  // `private, max-age=600` is meaningful: the browser may cache it.
  auto input = MakeSafe();
  input.forward_origin_restrictions = true;
  input.origin_cc_flags = AM::kCCOriginHeaderPresent | AM::kCCOriginPrivate;
  input.effective_max_age = 600;
  input.origin_max_age = 600;
  auto hdr = Build(input);
  EXPECT_TRUE(Contains(hdr, "private"));
  EXPECT_TRUE(Contains(hdr, "max-age=600"));
}

TEST_F(CacheControlHeaderTest, ForwardDoesNotInventMustRevalidate) {
  // Safe mode normally auto-adds must-revalidate; a header whose job is to
  // relay the origin's own statement must not put words in its mouth.
  auto input = MakeSafe();
  input.forward_origin_restrictions = true;
  input.origin_cc_flags = AM::kCCOriginHeaderPresent | AM::kCCOriginPrivate;
  auto hdr = Build(input);
  EXPECT_FALSE(Contains(hdr, "must-revalidate"));
}

TEST_F(CacheControlHeaderTest, ForwardQualifiedPrivateIsNotSentBare) {
  // `private="Set-Cookie"` is not a blanket restriction; forwarding it as a
  // bare `private` would over-restrict every cache downstream.
  auto input = MakeSafe();
  input.forward_origin_restrictions = true;
  input.origin_cc_flags = AM::kCCOriginHeaderPresent | AM::kCCOriginPrivate |
                          AM::kCCOriginPrivateQualified;
  input.effective_max_age = 600;
  input.origin_max_age = 600;
  auto hdr = Build(input);
  EXPECT_FALSE(Contains(hdr, "private"));
  EXPECT_TRUE(Contains(hdr, "max-age=600"));
}

TEST_F(CacheControlHeaderTest, ForwardNeverEmitsPublicAlongsideRestriction) {
  // origin_private already suppresses public; assert it holds in
  // aggressive mode with forwarding on.  Also assert the stale-serving
  // permissions are gone: both are permission to serve a stale STORED
  // copy, which presupposes the storage no-store forbids.  The earlier
  // version of this test asserted only public/no-store and so passed
  // while emitting `no-store, stale-if-error=86400`.
  auto input = MakeAggressive();
  input.forward_origin_restrictions = true;
  input.synthesize_swr = true;
  input.origin_cc_flags =
      AM::kCCOriginHeaderPresent | AM::kCCOriginPublic | AM::kCCOriginNoStore;
  auto hdr = Build(input);
  EXPECT_TRUE(Contains(hdr, "no-store"));
  EXPECT_FALSE(Contains(hdr, "public"));
  EXPECT_FALSE(Contains(hdr, "stale-if-error")) << hdr;
  EXPECT_FALSE(Contains(hdr, "stale-while-revalidate")) << hdr;
}

TEST_F(CacheControlHeaderTest, ForwardPrivateSuppressesStaleServing) {
  auto input = MakeAggressive();
  input.forward_origin_restrictions = true;
  input.synthesize_swr = true;
  input.origin_cc_flags = AM::kCCOriginHeaderPresent | AM::kCCOriginPrivate |
                          AM::kCCOriginPrivateBare;
  auto hdr = Build(input);
  EXPECT_TRUE(Contains(hdr, "private"));
  EXPECT_FALSE(Contains(hdr, "stale-if-error")) << hdr;
  EXPECT_FALSE(Contains(hdr, "stale-while-revalidate")) << hdr;
}

// --- relay_origin_no_cache: the directive must survive a plain restamp ---

TEST_F(CacheControlHeaderTest, RelayNoCacheWithForwardingOff) {
  // THE regression case.  A 304 carrying `no-cache, max-age=600` is neither
  // no-store nor private, so forward_origin_restrictions is off — and if
  // no-cache were gated on that flag it would vanish here, turning a
  // revalidate-before-every-reuse resource into one freely reusable for
  // 600s.  This configuration was covered by nothing.
  auto input = MakeSafe();
  input.relay_origin_no_cache = true;
  input.forward_origin_restrictions = false;
  input.origin_cc_flags = AM::kCCOriginHeaderPresent | AM::kCCOriginNoCache |
                          AM::kCCOriginNoCacheBare;
  input.effective_max_age = 600;
  input.origin_max_age = 600;
  auto hdr = Build(input);
  EXPECT_TRUE(Contains(hdr, "no-cache")) << hdr;
  EXPECT_TRUE(Contains(hdr, "max-age=600")) << hdr;
}

TEST_F(CacheControlHeaderTest, RelayNoCacheInAggressiveModeToo) {
  auto input = MakeAggressive();
  input.relay_origin_no_cache = true;
  input.origin_cc_flags = AM::kCCOriginHeaderPresent | AM::kCCOriginNoCache |
                          AM::kCCOriginNoCacheBare;
  auto hdr = Build(input);
  EXPECT_TRUE(Contains(hdr, "no-cache")) << hdr;
}

TEST_F(CacheControlHeaderTest, RelayOffKeepsNoCacheOut) {
  // Byte-identity for pre-existing callers: they never asked for no-cache
  // relaying and must not start receiving it.
  auto input = MakeSafe();
  input.origin_cc_flags = AM::kCCOriginHeaderPresent | AM::kCCOriginNoCache |
                          AM::kCCOriginNoCacheBare;
  auto hdr = Build(input);
  EXPECT_FALSE(Contains(hdr, "no-cache")) << hdr;
}

TEST_F(CacheControlHeaderTest, QualifiedNoCacheIsNotRelayedBare) {
  // `no-cache="Set-Cookie"` restricts one field from being reused without
  // revalidation; relaying it bare would force revalidation of the whole
  // response.
  auto input = MakeSafe();
  input.relay_origin_no_cache = true;
  input.origin_cc_flags = AM::kCCOriginHeaderPresent | AM::kCCOriginNoCache |
                          AM::kCCOriginNoCacheQualified;
  auto hdr = Build(input);
  EXPECT_FALSE(Contains(hdr, "no-cache")) << hdr;
}

TEST_F(CacheControlHeaderTest, BareBeatsQualifiedWhenBothOccur) {
  // OR-accumulation across header lines: a bare occurrence anywhere wins.
  auto input = MakeSafe();
  input.relay_origin_no_cache = true;
  input.forward_origin_restrictions = true;
  input.origin_cc_flags = AM::kCCOriginHeaderPresent | AM::kCCOriginPrivate |
                          AM::kCCOriginPrivateQualified |
                          AM::kCCOriginPrivateBare | AM::kCCOriginNoCache |
                          AM::kCCOriginNoCacheQualified |
                          AM::kCCOriginNoCacheBare;
  auto hdr = Build(input);
  EXPECT_TRUE(Contains(hdr, "private")) << hdr;
  EXPECT_TRUE(Contains(hdr, "no-cache")) << hdr;
}

TEST_F(CacheControlHeaderTest, LegacyMetadataWithNoFormBitsTreatedAsBare) {
  // Metadata written before the form bits existed carries neither; fail
  // safe by treating it as the blanket directive.
  auto input = MakeSafe();
  input.relay_origin_no_cache = true;
  input.forward_origin_restrictions = true;
  input.origin_cc_flags =
      AM::kCCOriginHeaderPresent | AM::kCCOriginPrivate | AM::kCCOriginNoCache;
  auto hdr = Build(input);
  EXPECT_TRUE(Contains(hdr, "private")) << hdr;
  EXPECT_TRUE(Contains(hdr, "no-cache")) << hdr;
}

}  // namespace
}  // namespace pagespeed
