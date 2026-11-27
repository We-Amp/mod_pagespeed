// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

#ifndef PAGESPEED_SRC_NGINX_REVALIDATION_STORABILITY_H_
#define PAGESPEED_SRC_NGINX_REVALIDATION_STORABILITY_H_

// Storability re-evaluation for the conditional-revalidation (304) restamp
// path.  Pure C++ (no nginx headers) so the decision logic is unit-testable
// without an nginx build (same pattern as etag_util / authz_cache_gate).
//
// Issue #1016.  A 304 answer to a conditional re-fetch merges the origin's
// refreshed Cache-Control into the stored entry's metadata and writes the
// entry back with a fresh cache_inserted_at.  The store-side storability
// checks, however, lived only in the header-filter MISS path, so the
// restamp re-freshened and persisted entries the shared cache is no longer
// allowed to hold: a 304 carrying `no-store` or `private`, or one that
// withdraws the RFC 9111 §3.5 permit an Authorization-bearing exchange
// needs, was written straight back.  This kernel is that missing gate,
// evaluated over the MERGED (post-restamp) flags — the same vocabulary the
// record path checks, so both paths agree on what "storable" means.
//
// This kernel decides RETENTION only.  The caller keeps serving the
// current requester exactly as before.  Whether a withdrawn §3.5 permit
// should additionally suppress that serve was adjudicated 2026-07-23 —
// see the kSkipRestamp verdict note below: the serve is compliant.
//
// Three verdicts, because "not storable" has two materially different
// causes and collapsing them would be wrong in both directions:
//
//   kRestamp     — refreshed headers are storable; write the entry back.
//
//   kEvict       — the refreshed headers say this representation must not
//                  live in a shared cache at all: `no-store`, or the BARE
//                  `private`.  The qualified form (`private="Set-Cookie"`,
//                  RFC 9111 §5.2.2.7) restricts only the named fields and
//                  explicitly permits storing the remainder, so it is NOT
//                  an eviction — treating it as one would destroy the
//                  entry and every derived variant on every revalidation,
//                  for an idiom origins pair with a cacheable `max-age`
//                  routinely.
//                  Declining the write-back is not enough: the PREVIOUSLY
//                  stored copy is the thing the origin just told us not to
//                  hold, and it would keep being served to other clients
//                  until its own TTL ran out.  The entry has to go.
//
//   kSkipRestamp — the entry is still legitimately storable, but THIS
//                  exchange may not extend its life: the requester carried
//                  Authorization and the refreshed Cache-Control no longer
//                  carries a §3.5 permit (public / must-revalidate /
//                  s-maxage), the requester carried a Cookie header and the
//                  refreshed headers no longer satisfy the cookie rule for
//                  the entry (cookie_cache_gate.h), or the 304 carried
//                  Set-Cookie, which marks the exchange as personalized.
//                  Deliberately NOT kEvict:
//                  §3.5 restricts storing the response to the AUTHENTICATED
//                  request, not the anonymous entry that was already
//                  lawfully stored — and evicting on an inbound header
//                  would hand any client a one-request shared-cache flush.
//                  Leaving the stale stamp in place is self-correcting: the
//                  entry simply revalidates again on the next request.
//                  Serving THIS requester the entry after the
//                  permit-withdrawing 304 is adjudicated compliant
//                  (operator decision, 2026-07-23): §4.3.4 lets a cache use
//                  the stored response on a 304, and that 304 answered a
//                  conditional request carrying the credentials — i.e.
//                  per-request permission from the origin, which regains
//                  full control on any 200 (the refreshed headers then
//                  restamp or evict).  The strict alternative — discard the
//                  304 and re-fetch unconditionally — buys an origin
//                  round-trip for no practical exposure, and was rejected.
//
// no-store / bare private dominate: an entry that must not be held is
// evicted regardless of any permit or personalization signal also present.
//
// Set-Cookie parity note: the MISS path treats a Set-Cookie response as
// uncacheable outright (it never stores).  Here the body was already stored
// under headers that permitted it, and a 304's Set-Cookie is typically a
// session refresh attached to the exchange rather than a statement about
// the representation — so the proportionate answer is to decline the
// life-extension, not to destroy a lawfully cached entry.

#include <cstdint>
#include <span>
#include <string_view>

#include "lib/cache/cache.h"
#include "lib/cache/html_cache_policy.h"
#include "lib/classify/alternate_id.h"

namespace pagespeed {

enum class RestampVerdict : uint8_t {
  kRestamp,
  kSkipRestamp,
  kEvict,
};

struct RestampStorabilityInputs {
  // Cache-Control flags AFTER merging the 304's directives into the stored
  // entry's metadata — i.e. exactly the flags the restamped entry would be
  // persisted with.  A 304 without Cache-Control leaves the stored flags in
  // place, which is what the caller passes.
  uint16_t refreshed_cc_flags = 0;

  // RFC 9111 §3.5 gate input for the request that triggered the
  // revalidation.  The RSL-CAP License exemption is applied by the caller
  // (see authz_cache_gate.h) — this is the effective signal, not raw
  // header presence.
  bool request_has_authorization = false;

  // The parsed lifetimes AFTER the merge (s-maxage, max-age), for the
  // cookie rule below.
  uint32_t refreshed_s_maxage = 0;
  uint32_t refreshed_max_age = 0;
  // The request that triggered the revalidation carried a Cookie header.  Such
  // an exchange may extend the entry's life only while the refreshed headers
  // still satisfy the cookie rule for the entry's class
  // (cookie_cache_gate.h): a page must stay marked public or s-maxage above
  // zero, a static asset must keep a stated lifetime.  Otherwise
  // kSkipRestamp, for the reasons given for the Authorization case above.
  bool request_has_cookie = false;
  CookieShareClass entry_class = CookieShareClass::kPage;
  // The 304 itself carried Set-Cookie.
  bool response_has_set_cookie = false;
};

// Decide what to do with the stored entry once the 304's headers have been
// merged in.  Total function: every input combination yields a verdict.
RestampVerdict EvaluateRestampStorability(
    const RestampStorabilityInputs& inputs);

// True when the refreshed headers forbid a shared cache from holding the
// representation at all (`no-store`, or a blanket `private` — see the
// bare/qualified decision rule in alternate_metadata.h).  Exposed
// separately because the caller must ALSO stop advertising the entry as
// cacheable downstream: the restamp path's Cache-Control emitter otherwise
// replaces the origin's restrictive directive with a plain `max-age`,
// which would invite the next cache in the chain to store what this one
// just evicted.
bool RefreshedHeadersForbidSharedStorage(uint16_t refreshed_cc_flags);

// What ApplyRestampVerdict actually did — the observable outcome, so a
// test can assert the cache effect rather than re-deriving the verdict.
enum class RestampAction : uint8_t {
  kEvicted,      // entry removed
  kEvictFailed,  // removal attempted and refused (stripe not owned, ...)
  kSkipped,      // deliberately left untouched
  kWrittenBack,  // restamped
  kEntryGone,    // purge fence tripped: the alternate vanished mid-flight
  kWriteFailed,  // write-back could not be opened
};

// Carry out a verdict against the cache.  A template over the cache type
// rather than a virtual interface: production instantiates it with the
// real PageSpeedCache (no indirection, no ABI change), while tests
// instantiate it against a real cache on a temp volume and assert the
// effects that have no other seam — that kEvict actually removes, and that
// kSkipRestamp actually does not write.  Those two are precisely the
// behaviors that survived a full review cycle unasserted.
//
// Cache must provide AlternateExists / Remove / WriteAlternate /
// EvictAlternateFromRamCache with the PageSpeedCache signatures.
template <typename Cache, typename Metadata>
RestampAction ApplyRestampVerdict(Cache& cache, RestampVerdict verdict,
                                  std::string_view url,
                                  std::string_view hostname,
                                  std::string_view scheme, AlternateId id,
                                  const Metadata& restamped_meta,
                                  std::span<const std::byte> content) {
  switch (verdict) {
    case RestampVerdict::kEvict: {
      // Drops the whole URL: every derived alternate came from the same
      // origin body, so all are covered by the origin's new directive.
      auto removed = cache.Remove(url, hostname, scheme);
      if (removed.has_value()) {
        return RestampAction::kEvicted;
      }
      // NotFound is success, not failure: a purge (or a concurrent
      // eviction) got there first, and "the entry is not in the cache" is
      // exactly the end state being asked for.  Reporting it as a failure
      // would emit an operator-facing error claiming a non-storable
      // response "may remain cached" at the precise moment it definitely
      // does not — and would bury the genuine NotOwned signal in noise.
      //
      // Caveat, deliberately accepted: remove_sync also returns NotFound
      // when a candidate entry EXISTS but is unverifiable (bad chain
      // offset, map_region failure, invalid DocumentReader), so a
      // directory entry can survive while this reports kEvicted.  Benign —
      // an unreadable document fails CRC on read and is served as a miss,
      // so it cannot leak the restricted response either way.
      if (removed.error() == cyclone::CacheError::NotFound) {
        return RestampAction::kEvicted;
      }
      return RestampAction::kEvictFailed;
    }
    case RestampVerdict::kSkipRestamp:
      return RestampAction::kSkipped;
    case RestampVerdict::kRestamp:
      break;
  }
  // Purge fence (issue #652): a purge racing this in-flight revalidation
  // makes the alternate vanish from the shared directory, so "entry gone
  // => skip the write-back" is a one-lookup cross-process fence.
  if (!cache.AlternateExists(url, hostname, scheme, id)) {
    return RestampAction::kEntryGone;
  }
  auto wh = cache.WriteAlternate(url, hostname, scheme, id, content.size(),
                                 restamped_meta);
  if (!wh.has_value()) {
    return RestampAction::kWriteFailed;
  }
  // Best-effort: the content was already served to the current requester.
  (void)wh->write_sync(content);
  (void)wh->close_sync();
  // Cyclone's RAM tier is write-around (reads populate it, writes do NOT
  // evict): the stale read that triggered this revalidation populated the
  // RAM tier in THIS process, and the write-back above — same bytes, new
  // cache_inserted_at — does not displace it (issue #1126, same class as
  // #1125).  Left in place, every later same-process read keeps computing
  // freshness from the PRE-restamp stamp, so the entry revalidates to
  // origin on every request forever.  Evict post-commit so the next read
  // re-admits the restamped document.
  cache.EvictAlternateFromRamCache(url, hostname, scheme, id);
  return RestampAction::kWrittenBack;
}

}  // namespace pagespeed

#endif  // PAGESPEED_SRC_NGINX_REVALIDATION_STORABILITY_H_
