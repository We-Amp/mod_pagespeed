// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

#ifndef PAGESPEED_LIB_CACHE_VARY_STORABILITY_H_
#define PAGESPEED_LIB_CACHE_VARY_STORABILITY_H_

#include <string_view>

namespace pagespeed {

// Store-side `Vary` refusal predicate: may a response carrying this `Vary`
// header be stored in the shared cache at all?
//
// The cache keeps one stored response per URL and serves it to every client
// (the capability mask picks among the optimizer's own variants of it, not
// among answers the origin gave).  So a response may be stored only when its
// `Vary` names nothing but `Accept-Encoding`: the stored copy is always the
// uncoded form and the coding is chosen per request.  Any other field
// (`Accept`, `User-Agent`, `Save-Data`, `Origin`, ...) or `*` refuses it, and
// such a response is passed through on every request, never cached or
// optimized.
//
// Every caller records a response to a request a client sent: the nginx
// front end's record and revalidation paths, the headers sidecar, and the C
// ABI (`ps_vary_uncacheable`) that other front ends record through.  Allowing
// another field needs the stored response itself keyed on that field.
//
// `combined_vary` is the response's `Vary` value, several `Vary` lines joined
// with `,` (RFC 9110 §5.3); a per-line call would accept a list the whole
// would refuse.  Empty means no variance: storable.  Field names match ASCII
// case-insensitively, OWS around a member is trimmed (RFC 9110 §5.6.1), and
// empty members are skipped, so `Vary: Accept-Encoding,,accept-encoding` is
// storable.
bool VaryUncacheable(std::string_view combined_vary);

// Everything a response's `Vary` decides on the store side, in one call.
// `varies_accept` is always false now that `Vary: Accept` is refused; it is
// kept so the stored marker and its readers keep their meaning.
struct VaryStoreVerdict {
  // May the response enter the shared cache at all?  Exactly
  // `!VaryUncacheable(combined_vary)`.
  bool storable = true;

  // Always false: a `Vary: Accept` response is not stored.  When set (by an
  // earlier build) the stored original carries
  // `AlternateMetadata::kFlagOriginVariesAccept` and is never optimized.
  bool varies_accept = false;
};

// Classify one response's combined `Vary` for the store path.
//
// `combined_vary` has exactly the shape `VaryUncacheable` documents: the
// response's several `Vary` lines joined with `,`, empty when it carries none.
VaryStoreVerdict ClassifyVaryForStore(std::string_view combined_vary);

}  // namespace pagespeed

#endif  // PAGESPEED_LIB_CACHE_VARY_STORABILITY_H_
