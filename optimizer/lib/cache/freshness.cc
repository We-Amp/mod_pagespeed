// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

#include "lib/cache/freshness.h"

#include <algorithm>
#include <cstdint>
#include <limits>

#include "lib/classify/alternate_metadata.h"

namespace pagespeed {

uint32_t ClampAge(int64_t age_raw, uint32_t cache_inserted_at,
                  uint32_t now_seconds) {
  static constexpr uint32_t kMaxAge = std::numeric_limits<uint32_t>::max();
  if (cache_inserted_at > now_seconds + 60) {
    // Corrupted/far-future timestamp: fail toward stale.
    return kMaxAge;
  }
  if (age_raw < 0) {
    return 0;  // Small clock skew.
  }
  if (age_raw > static_cast<int64_t>(kMaxAge)) {
    return kMaxAge;
  }
  return static_cast<uint32_t>(age_raw);
}

FreshnessInput FreshnessInputFromMetadata(const AlternateMetadata& metadata,
                                          uint32_t now_seconds,
                                          bool is_shared_cache,
                                          bool force_revalidate) {
  FreshnessInput input;
  input.now_seconds = now_seconds;
  input.cache_inserted_at = metadata.cache_inserted_at;
  input.origin_max_age = metadata.origin_max_age;
  input.origin_s_maxage = metadata.origin_s_maxage;
  input.origin_cc_flags = metadata.origin_cc_flags;
  input.content_type = metadata.content_type;
  input.is_shared_cache = is_shared_cache;
  input.force_revalidate = force_revalidate;
  return input;
}

FreshnessResult EvaluateFreshness(const FreshnessInput& input,
                                  const FreshnessConfig& config) {
  using AM = AlternateMetadata;
  const uint16_t cc = input.origin_cc_flags;

  // 1. Compute age.
  int64_t age_raw = static_cast<int64_t>(input.now_seconds) -
                    static_cast<int64_t>(input.cache_inserted_at);
  uint32_t age = ClampAge(age_raw, input.cache_inserted_at, input.now_seconds);

  // 2. Defensive: no-store / private should never be cached.
  //    Serve with no-cache header (same as nginx goto emit_age path).
  if ((cc & (AM::kCCOriginNoStore | AM::kCCOriginPrivate)) != 0) {
    if (input.is_shared_cache || ((cc & AM::kCCOriginNoStore) != 0)) {
      return {FreshnessVerdict::kServeNoCache, age, 0, 0, true};
    }
    // Private cache: private flag is OK, proceed to normal evaluation.
  }

  // 3. Determine effective freshness lifetime.
  uint32_t shared_max_age;
  if (((cc & AM::kCCOriginSMaxagePresent) != 0) && input.is_shared_cache) {
    shared_max_age = input.origin_s_maxage;
  } else {
    shared_max_age = input.origin_max_age;
  }

  // Type-based defaults when origin sent no Cache-Control.
  if (shared_max_age == 0 && ((cc & AM::kCCOriginHeaderPresent) == 0)) {
    if (input.content_type == ContentType::kHtml) {
      shared_max_age = config.html_max_age;
    } else if (input.content_type == ContentType::kCss ||
               input.content_type == ContentType::kJs) {
      shared_max_age = config.css_max_age;
    } else if (input.content_type == ContentType::kImage) {
      shared_max_age = config.image_max_age;
    }
  }

  // HTML with no max-age and no CC header: treat as no-cache.
  if (shared_max_age == 0 && input.content_type == ContentType::kHtml &&
      ((cc & AM::kCCOriginHeaderPresent) == 0)) {
    return {FreshnessVerdict::kServeNoCache, age, 0, 0, true};
  }

  // 4. Apply cap.
  uint32_t effective_max_age;
  if ((cc & AM::kCCOriginImmutable) != 0) {
    effective_max_age = std::min(shared_max_age, config.immutable_max_age_cap);
  } else {
    effective_max_age = std::min(shared_max_age, config.max_age_cap);
  }

  // 5. no-cache means "revalidate before each use" (RFC 9111 S5.2.2.4).
  bool is_no_cache = (cc & AM::kCCOriginNoCache) != 0;

  // For no-cache: always stale (every request must revalidate).  Otherwise a
  // response is fresh only while its age is below its lifetime (RFC 9111
  // S4.2).
  bool is_stale = is_no_cache || (age >= effective_max_age);

  // 6. Client force-refresh (Ctrl+F5): treat as stale + must-revalidate.
  // NOT expired_by_age: the client demanded revalidation; the cached
  // variants are not known to be outdated (issue #652 review — this must
  // never trigger the origin-refreshed purge, or any anonymous client
  // could purge the variant set with a plain reload).
  if (input.force_revalidate) {
    return {FreshnessVerdict::kRevalidate, age, effective_max_age, 0, true,
            /*expired_by_age=*/false};
  }

  // 7. Verdict.
  uint32_t remaining_ttl = is_stale ? 0 : (effective_max_age - age);

  if (is_stale) {
    // Stale content must revalidate regardless of origin directives
    // (issue #652).  Previously, stale content WITHOUT a revalidation
    // directive (e.g. plain "public, max-age=30") was served indefinitely
    // as kStaleServe — a shared cache serving HITs past the freshness
    // lifetime with no stale-while-revalidate machinery behind the
    // "SWR candidate" label.  kRevalidate triggers a conditional re-fetch
    // when validators (ETag/Last-Modified) exist, and a full re-fetch
    // (treat as MISS) otherwise.
    //
    // expired_by_age: genuine age-based expiry only.  no-cache content is
    // permanently "stale" by definition (every use revalidates) — that
    // must NOT count as expiry, or no-cache pages (a very common CMS
    // default) would churn the worker's purge → re-optimize cycle on
    // every revalidation (issue #652 review).
    const bool expired_by_age = !is_no_cache && (age >= effective_max_age);
    return {FreshnessVerdict::kRevalidate,
            age,
            effective_max_age,
            0,
            true,
            expired_by_age};
  }
  return {FreshnessVerdict::kFresh, age, effective_max_age, remaining_ttl,
          false};
}

}  // namespace pagespeed
