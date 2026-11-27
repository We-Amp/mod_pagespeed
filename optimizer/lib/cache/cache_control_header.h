// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

#ifndef PAGESPEED_LIB_CACHE_CACHE_CONTROL_HEADER_H_
#define PAGESPEED_LIB_CACHE_CACHE_CONTROL_HEADER_H_

#include <cstddef>
#include <cstdint>

#include "lib/classify/alternate_metadata.h"
#include "lib/classify/content_type.h"

namespace pagespeed {

// Cache mode: safe-by-default vs aggressive (opt-in).
enum class CacheMode : uint8_t { kSafe = 0, kAggressive = 1 };

// Single source of truth for "does RFC 9111 require revalidation before
// serving the response stale?" — used by cache_control_header.cc to suppress
// stale-if-error / stale-while-revalidate per §4.2.4.  (freshness.cc used to
// consult this to pick kRevalidate vs kStaleServe; since issue #652 ALL
// stale content yields kRevalidate, so the freshness verdict no longer
// depends on it.)
//
// `no-cache` and `must-revalidate` always apply. `proxy-revalidate`
// (§5.2.2.10) and `s-maxage` (§5.2.2.9) are shared-cache-only directives:
// a private cache (e.g. an origin-private ASP.NET middleware) must ignore
// them.
inline bool ComputeSharedRevalidationRequired(uint16_t origin_cc_flags,
                                              bool is_shared_cache) {
  using AM = AlternateMetadata;
  const uint16_t cc = origin_cc_flags;
  return ((cc & AM::kCCOriginNoCache) != 0) ||
         ((cc & AM::kCCOriginMustRevalidate) != 0) ||
         (((cc & AM::kCCOriginProxyRevalidate) != 0) && is_shared_cache) ||
         (((cc & AM::kCCOriginSMaxagePresent) != 0) && is_shared_cache);
}

// Input for Cache-Control header assembly.  All fields are scalar
// (no nginx types) so the function is testable in isolation.
struct CacheControlInput {
  CacheMode mode = CacheMode::kSafe;
  uint16_t origin_cc_flags = 0;    // AlternateMetadata::kCC* bitfield
  uint32_t effective_max_age = 0;  // From EvaluateFreshness()
  uint32_t origin_max_age = 0;     // Raw origin max-age (for s-maxage split)
  bool synthesize_swr = false;     // Config flag (pagespeed_synthesize_swr)
  // Shared (proxy) cache vs private cache. Gates proxy-revalidate / s-maxage
  // per RFC 9111 §5.2.2.9–10. nginx and Apache integrations are shared
  // caches (true). A private-cache integration must set this false.
  bool is_shared_cache = true;
  ContentType content_type = ContentType::kOther;
  // Relay the origin's `no-cache` (issue #1016).  This is about FIDELITY,
  // not about storage permission: `no-cache` means "revalidate before
  // reuse", it applies to every response that carries it, and dropping it
  // silently converts a must-revalidate-on-every-use resource into one
  // reusable for the whole max-age.  The 304 path sets this on ALL
  // outcomes, including a plain restamp.
  //
  // Deliberately separate from forward_origin_restrictions below: that one
  // is keyed on "we evicted", which is true for no-store/private only —
  // gating `no-cache` on it would drop `no-cache, max-age=600` entirely.
  // (That conflation was a real regression; the two concerns are not the
  // same predicate and must not share a flag.)
  //
  // Off by default so pre-existing callers stay byte-identical.  On the
  // HIT path the kServeNoCache branch emits `no-cache` itself without
  // reaching this builder.  The coalesced-SWR serve does reach the builder
  // and sets this flag as a guard, but it requires expired_by_age, which
  // freshness.cc never sets for a no-cache entry, so no-cache entries
  // cannot take that serve today.
  bool relay_origin_no_cache = false;

  // Forward the origin's STORAGE-restricting directives (`no-store`, bare
  // `private`) instead of dropping them.  Off by default, so every
  // pre-existing caller is byte-identical: those callers serve entries
  // that, by construction, were storable, and this builder has never
  // emitted the restrictive set.
  //
  // The 304-restamp path turns it on only when the refreshed headers
  // forbid shared storage: the response still goes to the current
  // requester, but the directive that made us evict must travel downstream
  // too, or the next cache in the chain stores exactly what this one just
  // dropped.  With it on: the safe-mode `must-revalidate` auto-add is
  // suppressed (only the origin's own is preserved) so the forwarded set
  // reflects what the origin actually said; `max-age` is suppressed under
  // `no-store`; and stale-while-revalidate / stale-if-error are suppressed
  // outright — both are permission to serve a stale STORED copy, which
  // presupposes the very storage being forbidden.
  bool forward_origin_restrictions = false;
};

// Output from Cache-Control header assembly.
struct CacheControlOutput {
  size_t len = 0;  // Bytes written to caller buffer (not counting NUL)
  uint32_t final_max_age = 0;  // For Age header calculation
};

// Build a Cache-Control header value into a caller-provided buffer.
//
// Rules:
//   Safe mode:   add must-revalidate, suppress SWR synthesis, strip immutable,
//                do NOT add public.
//   Aggressive:  add public (if origin allows); add stale-if-error=86400
//                and synthesize SWR only when stale serving is permitted
//                (i.e. revalidation_required is false — RFC 9111 §4.2.4);
//                strip immutable on transformed content.
//   Both modes:  never add public when origin sent private or no-store;
//                always strip immutable on transformed content;
//                preserve origin's must-revalidate, proxy-revalidate,
//                no-transform if present.
//
// Returns the number of bytes written and the final max-age used.
// The buffer is NOT NUL-terminated.
CacheControlOutput BuildCacheControlHeader(const CacheControlInput& input,
                                           char* buf, size_t capacity);

}  // namespace pagespeed

#endif  // PAGESPEED_LIB_CACHE_CACHE_CONTROL_HEADER_H_
