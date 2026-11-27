// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

#include "lib/cache/cache_control_header.h"

#include <algorithm>
#include <cstdio>

#include "lib/classify/alternate_metadata.h"

namespace pagespeed {

CacheControlOutput BuildCacheControlHeader(const CacheControlInput& input,
                                           char* buf, size_t capacity) {
  using AM = AlternateMetadata;

  CacheControlOutput out;
  if (buf == nullptr || capacity == 0) {
    return out;
  }

  char* p = buf;
  char* end = buf + capacity;

  auto remaining = [&]() -> size_t { return static_cast<size_t>(end - p); };

  auto append_comma = [&]() {
    if (p > buf && remaining() >= 2) {
      *p++ = ',';
      *p++ = ' ';
    }
  };

  // Check if a directive fits (including comma separator) before writing.
  // Returns false if the directive would be truncated — caller should skip.
  auto fits = [&](size_t directive_len) -> bool {
    size_t comma_len = (p > buf) ? 2 : 0;
    return remaining() >= comma_len + directive_len + 1;  // +1 for NUL
  };

  const uint16_t flags = input.origin_cc_flags;
  const bool origin_private = ((flags & AM::kCCOriginPrivate) != 0) ||
                              ((flags & AM::kCCOriginNoStore) != 0);
  const bool revalidation_required =
      ComputeSharedRevalidationRequired(flags, input.is_shared_cache);

  // -- public directive --
  // Aggressive: add public if origin sent public and not private/no-store.
  // Safe: never add public.
  if (input.mode == CacheMode::kAggressive &&
      ((flags & AM::kCCOriginPublic) != 0) && !origin_private && fits(6)) {
    int n = snprintf(p, remaining(), "public");
    if (n > 0) p += std::min(static_cast<size_t>(n), remaining());
  }

  // -- origin storage restrictions (opt-in, issue #1016) --
  // Emitted first so the strongest statement leads the header.  Ordering
  // within Cache-Control is not semantically significant, but a cache or
  // operator reading the first token should see the restriction.
  if (input.forward_origin_restrictions) {
    if (((flags & AM::kCCOriginNoStore) != 0) && fits(8)) {
      append_comma();
      int n = snprintf(p, remaining(), "no-store");
      if (n > 0) p += std::min(static_cast<size_t>(n), remaining());
    }
    // Bare `private` only.  The qualified form (`private="Set-Cookie"`)
    // restricts named fields and is not a blanket restriction; forwarding
    // it bare would over-restrict every downstream cache.  Bare-seen is
    // the monotone signal (see alternate_metadata.h); metadata with
    // neither form bit is legacy and treated as bare, matching
    // RefreshedHeadersForbidSharedStorage.
    const bool private_is_blanket =
        ((flags & AM::kCCOriginPrivate) != 0) &&
        (((flags & AM::kCCOriginPrivateBare) != 0) ||
         ((flags & AM::kCCOriginPrivateQualified) == 0));
    if (private_is_blanket && fits(7)) {
      append_comma();
      int n = snprintf(p, remaining(), "private");
      if (n > 0) p += std::min(static_cast<size_t>(n), remaining());
    }
  }

  // -- no-cache (opt-in, issue #1016) --
  // Relayed independently of the storage restrictions above: `no-cache`
  // is what forces revalidation before reuse, and it applies to plain
  // restamps too.  Gating it on the eviction predicate would strip it from
  // every `no-cache, max-age=N` response — converting a
  // revalidate-every-time resource into one freely reusable for max-age.
  if (input.relay_origin_no_cache || input.forward_origin_restrictions) {
    const bool no_cache_is_blanket =
        ((flags & AM::kCCOriginNoCache) != 0) &&
        (((flags & AM::kCCOriginNoCacheBare) != 0) ||
         ((flags & AM::kCCOriginNoCacheQualified) == 0));
    if (no_cache_is_blanket && fits(8)) {
      append_comma();
      int n = snprintf(p, remaining(), "no-cache");
      if (n > 0) p += std::min(static_cast<size_t>(n), remaining());
    }
  }

  // -- immutable: always stripped on transformed content --
  // (We never emit immutable; origin immutable flag is intentionally ignored.)

  // -- must-revalidate --
  // Safe mode: always add must-revalidate.
  // Both modes: preserve origin's must-revalidate.
  // Forwarding mode: only the origin's own — the safe-mode auto-add would
  // put words in the origin's mouth in a header whose job is to relay what
  // the origin said.
  if (((input.mode == CacheMode::kSafe && !input.forward_origin_restrictions) ||
       ((flags & AM::kCCOriginMustRevalidate) != 0)) &&
      fits(16)) {
    append_comma();
    int n = snprintf(p, remaining(), "must-revalidate");
    if (n > 0) p += std::min(static_cast<size_t>(n), remaining());
  }

  // -- proxy-revalidate (preserve from origin) --
  if (((flags & AM::kCCOriginProxyRevalidate) != 0) && fits(17)) {
    append_comma();
    int n = snprintf(p, remaining(), "proxy-revalidate");
    if (n > 0) p += std::min(static_cast<size_t>(n), remaining());
  }

  // -- no-transform (preserve from origin) --
  if (((flags & AM::kCCOriginNoTransform) != 0) && fits(12)) {
    append_comma();
    int n = snprintf(p, remaining(), "no-transform");
    if (n > 0) p += std::min(static_cast<size_t>(n), remaining());
  }

  // -- s-maxage when origin sent it --
  // "s-maxage=" (9) + up to 10 digits = 19
  if (((flags & AM::kCCOriginSMaxagePresent) != 0) && fits(19)) {
    append_comma();
    int n = snprintf(p, remaining(), "s-maxage=%u",
                     static_cast<unsigned>(input.effective_max_age));
    if (n > 0) p += std::min(static_cast<size_t>(n), remaining());
  }

  // -- max-age --
  // "max-age=" (8) + up to 10 digits = 18
  // Under a forwarded `no-store` a freshness lifetime is meaningless (and
  // reads as a mixed signal), so it is suppressed; `private` keeps its
  // max-age, which is exactly what `private, max-age=600` means for a
  // browser cache.
  const bool suppress_max_age = input.forward_origin_restrictions &&
                                ((flags & AM::kCCOriginNoStore) != 0);
  uint32_t emitted_max_age;
  if (suppress_max_age) {
    emitted_max_age = input.effective_max_age;
  } else if ((flags & AM::kCCOriginSMaxagePresent) != 0) {
    uint32_t browser_max_age = input.origin_max_age;
    if ((browser_max_age > 0 || ((flags & AM::kCCOriginHeaderPresent) != 0)) &&
        fits(18)) {
      append_comma();
      int n = snprintf(p, remaining(), "max-age=%u",
                       static_cast<unsigned>(browser_max_age));
      if (n > 0) p += std::min(static_cast<size_t>(n), remaining());
      emitted_max_age = browser_max_age;
    } else {
      emitted_max_age = input.effective_max_age;
    }
  } else if (fits(18)) {
    append_comma();
    int n = snprintf(p, remaining(), "max-age=%u",
                     static_cast<unsigned>(input.effective_max_age));
    if (n > 0) p += std::min(static_cast<size_t>(n), remaining());
    emitted_max_age = input.effective_max_age;
  } else {
    emitted_max_age = input.effective_max_age;
  }

  // -- stale-while-revalidate (SWR) --
  // Safe mode: never synthesize SWR.
  // Aggressive mode: synthesize when configured and not revalidation-required.
  // "stale-while-revalidate=" (24) + up to 10 digits = 34
  // forward_origin_restrictions also suppresses SWR: permission to serve a
  // stale STORED copy presupposes the storage the forwarded directive
  // forbids (issue #1016).  Same reason max-age is suppressed under
  // no-store — emitting one while suppressing the other would be
  // internally inconsistent.
  if (input.mode == CacheMode::kAggressive && input.synthesize_swr &&
      !revalidation_required && !input.forward_origin_restrictions) {
    uint32_t swr =
        std::min(input.effective_max_age, static_cast<uint32_t>(86400));
    if (swr > 0 && fits(34)) {
      append_comma();
      int n = snprintf(p, remaining(), "stale-while-revalidate=%u",
                       static_cast<unsigned>(swr));
      if (n > 0) p += std::min(static_cast<size_t>(n), remaining());
    }
  }

  // -- stale-if-error (aggressive only, and only when stale serving is
  //    permitted) --
  // RFC 9111 §4.2.4 forbids serving a stale response when must-revalidate,
  // no-cache, or (for shared caches) proxy-revalidate / s-maxage are in
  // effect; §5.2.2.11 defines stale-if-error as a permission to serve
  // stale, so it's meaningless in those cases. `revalidation_required`
  // is derived above from origin flags + is_shared_cache — matching the
  // SWR guard above.
  // "stale-if-error=86400" = 20
  // Suppressed under forward_origin_restrictions for the same reason as
  // SWR above: stale-if-error is permission to serve a stale stored copy.
  if (input.mode == CacheMode::kAggressive && !revalidation_required &&
      !input.forward_origin_restrictions && fits(20)) {
    append_comma();
    int n = snprintf(p, remaining(), "stale-if-error=86400");
    if (n > 0) p += std::min(static_cast<size_t>(n), remaining());
  }

  out.len = static_cast<size_t>(p - buf);
  out.final_max_age = emitted_max_age;
  return out;
}

}  // namespace pagespeed
