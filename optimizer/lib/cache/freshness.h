// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

#ifndef PAGESPEED_LIB_CACHE_FRESHNESS_H_
#define PAGESPEED_LIB_CACHE_FRESHNESS_H_

#include <cstdint>

#include "lib/classify/alternate_metadata.h"
#include "lib/classify/content_type.h"

namespace pagespeed {

// Verdict from the freshness evaluation. Maps to nginx control flow:
//   kFresh        -> serve with Cache-Control: max-age=<remaining>
//   kServeNoCache -> serve but emit Cache-Control: no-cache (defensive)
//   kRevalidate   -> do not serve; conditional or full re-fetch from origin
//   kStaleServe   -> RETIRED (issue #652): EvaluateFreshness no longer
//                    returns it.  Stale content previously took this path
//                    when the origin sent no revalidation directive and was
//                    served indefinitely; ALL stale content now yields
//                    kRevalidate (conditional re-fetch with validators,
//                    full re-fetch / treat-as-MISS without).  The enum
//                    value is kept so existing switch statements stay
//                    exhaustive without churn.
enum class FreshnessVerdict : std::uint8_t {
  kFresh,         // Content is within TTL -- serve directly
  kStaleServe,    // RETIRED -- unreachable from EvaluateFreshness (#652)
  kServeNoCache,  // no-store/private/HTML-no-CC -- serve but emit no-cache
  kRevalidate,    // Stale -- revalidate (conditional or full re-fetch)
};

// Configurable parameters (from nginx location directives or defaults).
struct FreshnessConfig {
  uint32_t max_age_cap = 86400;             // Cap on effective max-age
  uint32_t immutable_max_age_cap = 604800;  // Cap for immutable content
  uint32_t html_max_age = 0;                // Default for HTML without CC
  uint32_t css_max_age = 300;               // Default for CSS/JS without CC
  uint32_t image_max_age = 3600;            // Default for images without CC
};

// Per-request input -- scalar fields only (no string copies from metadata).
// Precondition: cache_inserted_at is already Age-adjusted at insert time
// (the nginx module subtracts any inbound Age header per RFC 9111 S4.2.3).
struct FreshnessInput {
  uint32_t now_seconds = 0;        // Current time (Unix seconds)
  uint32_t cache_inserted_at = 0;  // Already Age-adjusted at insert time
  uint32_t origin_max_age = 0;
  uint32_t origin_s_maxage = 0;
  uint16_t origin_cc_flags = 0;  // AlternateMetadata::kCC* bitfield
  ContentType content_type = ContentType::kOther;
  bool is_shared_cache = true;
  bool force_revalidate = false;  // Client force-refresh (Ctrl+F5)
};

struct FreshnessResult {
  FreshnessVerdict verdict;
  uint32_t age_seconds;        // Computed age (clamped to [0, UINT32_MAX])
  uint32_t effective_max_age;  // Max-age actually used (after caps/defaults)
  uint32_t remaining_ttl;      // Seconds until stale (0 if already stale)
  bool is_stale;               // Whether content is past effective max-age
  // True ONLY when the entry is stale because its age exceeded the
  // effective freshness lifetime — i.e. genuine age-based expiry.  False
  // for client force-refresh (Ctrl+F5 / request Cache-Control: no-cache /
  // max-age=0) and for origin `no-cache` content, both of which also yield
  // kRevalidate but do NOT mean the cached variants are outdated.  The
  // nginx module uses this to decide whether a re-fetch may trigger the
  // origin-refreshed variant-set purge and the bounded SWR refresh
  // coalescing (issue #652 review): without the distinction, any anonymous
  // client sending `Cache-Control: max-age=0` (a plain browser reload)
  // could purge a URL's entire optimized variant set.
  bool expired_by_age = false;
};

// Pure function: determines cache freshness per RFC 9111.
// Single source of truth -- nginx and tests both call this.
FreshnessResult EvaluateFreshness(const FreshnessInput& input,
                                  const FreshnessConfig& config);

// Build the freshness inputs of an entry from the origin state stamped in the
// entry's OWN metadata.
//
// This is what "the entry's lifetime is bound to the origin's" means in code:
// there is no per-class TTL anywhere: an entry is fresh for exactly as long as
// the Cache-Control and validators the origin sent say it is, and the answer
// comes from the same evaluator every other entry goes through.  It matters
// most for the durable original, whose whole purpose is to still be usable
// later — "later" has to mean "while the origin says these bytes are still
// the origin's answer", not "until some fixed number of seconds elapses".
//
// A zero cache_inserted_at (metadata written without one) reads as epoch-old
// and therefore always stale: the failure direction is an extra revalidation,
// never bytes served as fresher than the origin allowed.
//
// STATUS, so nobody reads this as a completed consolidation: the serve path
// does NOT call this yet.  The two places that build this input today are
// inside the nginx front stage, which is frozen — they were left exactly as
// they are, deliberately, rather than rewired by a change that has no need to
// touch them.  So this is the forward surface (the durable-original class and
// anything else that has to ask "is this entry still the origin's answer?"
// without being nginx), and for now its only callers are tests.
//
// What keeps that from becoming three drifting copies: cache_freshness_test.cc
// transcribes BOTH front-stage build-ups as oracles and compares them to this
// field by field.  If one of them is edited and this is not, that test goes
// red at the seam instead of the difference being discovered by a cache
// serving something stale.  When the frozen line is next open to change, the
// two sites become calls to this and the oracles retire.
FreshnessInput FreshnessInputFromMetadata(const AlternateMetadata& metadata,
                                          uint32_t now_seconds,
                                          bool is_shared_cache = true,
                                          bool force_revalidate = false);

// Clamp a raw age (which may overflow uint32_t) to [0, UINT32_MAX].
// Exposed for testing the overflow path that is unreachable via the
// uint32_t-typed FreshnessInput fields.
uint32_t ClampAge(int64_t age_raw, uint32_t cache_inserted_at,
                  uint32_t now_seconds);

}  // namespace pagespeed

#endif  // PAGESPEED_LIB_CACHE_FRESHNESS_H_
