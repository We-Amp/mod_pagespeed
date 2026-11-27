// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

#ifndef PAGESPEED_SRC_NGINX_ETAG_UTIL_H_
#define PAGESPEED_SRC_NGINX_ETAG_UTIL_H_

// Weak-ETag construction for cache-HIT responses.  Pure C++ (no nginx
// headers) so the format logic is unit-testable without an nginx build.
//
// Format:
//   With stored content identity (see ContentIdentity64):
//     W/"ps-<mask8><flags2>-<identity16>-<length16>"
//   Without any identity signal: NO ETag is emitted (FormatHitETag
//     returns 0).  A mask+flags+length-only tag would validate any
//     revision of the same optimized length, answering 304 for changed
//     bytes (a false 304 pins a stale copy at the client); an absent
//     validator can never do that.  The no-signal arm covers pre-v7 cache entries
//     (which age out) and entries from origins that emit neither ETag nor
//     Last-Modified that the worker has not stamped (the worker stamps
//     every variant it writes from a raw origin input, so this population
//     shrinks to entries it has not yet processed).
//
// Rationale (RFC 9110 §8.8.3 — an entity tag identifies one representation):
//   * <mask8><flags2> pins the representation recipe: capability mask
//     dimensions (format, Content-Encoding, viewport) plus the metadata
//     flags byte.  Two encodings of the same source differ here, which is
//     what makes the tag per-representation.  The flags byte is load-bearing
//     beyond revalidation state: kFlagOriginVariesAccept marks an entry that
//     is served with `Accept` named in `Vary` and is never re-optimized, so
//     it must not validate against an otherwise-identical entry stored
//     without the marker (they are served with different cache keying).
//     Including the whole byte gives that separation for free, and gives it
//     to any flag bit added later.
//   * <identity16> pins the SOURCE content revision — it deliberately hashes
//     the identity (pre-transform origin) content, not the encoded variant
//     bytes.  Hashing served bytes would need a per-HIT SHA-256 on the hot
//     path or a metadata format bump to memoize; source identity plus the
//     recipe (mask) plus the encoded length already distinguishes what a
//     conditional-request comparison needs to distinguish.  The validator
//     stays weak because the module cannot promise byte-identity across
//     re-optimizations of the same source.
//   * <length16> is kept as a cheap secondary discriminator (it still
//     catches revisions on entries whose origin re-uses validators
//     incorrectly).

#include <cstddef>
#include <cstdint>
#include <optional>

#include "lib/classify/alternate_metadata.h"

namespace pagespeed {

// Maximum bytes FormatHitETag writes, including the trailing NUL.
// W/"ps- (6) + mask+flags (10) + '-' + identity (16) + '-' + length (16)
// + closing quote (1) + NUL = 52.
inline constexpr size_t kHitETagBufSize = 52;

// 64-bit content-identity discriminator for the ETag, or nullopt when the
// metadata carries no content-identity signal at all.  Tiers:
//   1. v7 origin_html_hash (SHA-256 of the pre-rewrite origin content the
//      variant was produced from) when non-zero: first 8 bytes, big-endian.
//   2. Origin validators (v5 origin_etag / origin_last_modified) when either
//      is present: 64-bit FNV-1a over the verbatim origin ETag bytes, a
//      domain separator, and the Last-Modified timestamp.  Opaque, cheap,
//      and refreshed by the revalidation restamp path whenever the origin
//      revises its validators.
//   3. nullopt: caller must emit no ETag at all (never a length-only tag,
//      which would validate a same-length revision of the same variant).
std::optional<uint64_t> ContentIdentity64(const AlternateMetadata& meta);

// Formats the weak ETag for a cache-HIT response into buf.
// body_len is the size of the encoded variant bytes actually served.
// Returns the number of characters written (excluding the NUL), or 0 when
// buf_size is too small (callers must suppress the header, never truncate)
// or when the metadata carries no content-identity signal (the caller must
// then emit no ETag header; with no tag there is no 304, so a stale copy
// can never be confirmed).
size_t FormatHitETag(const AlternateMetadata& meta, size_t body_len, char* buf,
                     size_t buf_size);

}  // namespace pagespeed

#endif  // PAGESPEED_SRC_NGINX_ETAG_UTIL_H_
