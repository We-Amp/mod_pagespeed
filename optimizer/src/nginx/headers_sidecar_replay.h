// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

#ifndef PAGESPEED_SRC_NGINX_HEADERS_SIDECAR_REPLAY_H_
#define PAGESPEED_SRC_NGINX_HEADERS_SIDECAR_REPLAY_H_

// Serve-side half of the response-header sidecar for the nginx front stage
// (issue #208).  Pure C++ (no nginx headers), so every decision the serve
// paths make about the stored header block is unit-testable without an
// nginx build.  The store side is headers_sidecar_record.h.
//
// THE RULE.  A response served from cache carries the same
// request-independent headers as the origin response it was recorded from.
// The record arm stores those lines verbatim in the URL's sidecar (one per
// URL, sentinel 0x6C) and marks every entry it records with two bits:
//
//   neither bit          recorded before the gate existed: serve exactly as
//                        before, no read, no replay;
//   classified only      the gate ran and there was nothing to carry: serve
//                        as before, no read;
//   classified+required  the URL's block MUST be read and replayed; a block
//                        that cannot be read makes the request a MISS (the
//                        record arm then rewrites it), never a hit without
//                        the headers.
//
// WHERE.  The handler reads the block once, right after the cache read and
// the RFC 9111 §3.5 gate and before the freshness decision, so "cannot be
// served with fidelity" is decided before any serving arm commits, and every
// served-from-cache path of the request — the fresh hit, the no-cache hit,
// the bounded coalesced stale serve, the stale-if-error serve — replays the
// same copy.  (The 304 restamp path merges the block with the 304's lines;
// see PlanRestampSidecar in headers_sidecar_record.h.)
//
// ORDER AND PRECEDENCE.  The stored lines go out first, verbatim and in
// stored order; then the serve path's own Vary, Cache-Control, Age, ETag.
// The two sets cannot collide: the gate never stores a name the entry
// metadata reproduces (Cache-Control, validators, Expires, Content-*) or one
// the serving stack stamps (Date, Age, ...), so the builder owns those and
// the block owns the rest.  Vary is a list header and both sides produce it:
// the origin's line first, then the computed one, and when the block carried
// a Vary the computed line does not repeat Accept (the origin's line already
// lists it) — exactly the shape a MISS sends.
//
// NOT REPLAYED onto the agent markdown variant: it is a separate
// representation with its own fixed header set.

#include <cstdint>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

#include "lib/cache/headers_sidecar.h"

namespace pagespeed {

// Does serving this entry need the URL's stored header block?  True exactly
// for an entry flagged classified + required that is not the agent markdown
// variant.  Everything else pays nothing.
bool SidecarReadRequired(uint8_t entry_flags, bool agent_markdown);

enum class SidecarServeOutcome : uint8_t {
  kNotRequired,  // serve as before: no block, nothing to replay
  kReplay,       // serve, replaying `fields` first
  kMissing,      // the entry requires a block and there is none: MISS
};

struct SidecarServePlan {
  SidecarServeOutcome outcome = SidecarServeOutcome::kNotRequired;
  // The stored lines, in stored order.  Views into the payload passed to
  // PlanSidecarServe, which must outlive them.
  std::vector<HeaderField> fields;
  // The block carried a Vary line: the computed Vary then leaves out Accept.
  bool carried_vary = false;
  // The block carried X-Content-Type-Options: the SVG hardening does not add
  // a second one.
  bool carried_nosniff = false;
  // kMissing because the block names a field the serve path builds itself
  // (it did not come from the gate: a damaged or foreign cache file).  The
  // caller logs it; the request is a MISS in every build.
  bool foreign_field = false;
};

// Plan the serve of one entry.  `payload` is the URL's stored block, or
// nullopt when it could not be read; it is only consulted when the entry
// requires one.  A payload that does not parse, parses to nothing, or names
// a field the serve path builds itself is treated as missing: the request
// becomes a MISS and the record arm rewrites the block from the origin.
SidecarServePlan PlanSidecarServe(uint8_t entry_flags, bool agent_markdown,
                                  std::optional<std::string_view> payload);

// True when `name` is one the serve path emits itself (reproduced from the
// entry metadata, or stamped per response).  The gate never stores such a
// name; a block that carries one did not come from it.
bool SidecarFieldCollidesWithServePath(std::string_view name);

// (Whether the computed Vary then names Accept — the entry's marker unless
// the replayed block carried the origin's own Vary line — is
// pagespeed::EmittedVaryNamesAccept in lib/cache/vary_emission.h.)

// Migration rule for entries recorded before the gate.  Such an entry
// carries no classification, and a 304 carries only part of the header set,
// so restamping it by conditional revalidation would keep it unclassified
// (and serving without its headers) indefinitely.  Instead, at its next
// GENUINE expiry it takes a full fetch once; the re-record classifies it.
// Bounded by the entry's own max-age (an entry whose life the origin keeps
// extending by no-cache, or a client's force-refresh, keeps revalidating
// conditionally as before — those are not expiries).  Classified entries
// revalidate conditionally as before.
bool ConditionalRevalidationAllowed(uint8_t entry_flags, bool expired_by_age);

// True when `name` is one of the names the sidecar carries (the allowlist):
// the stale-if-error serve drops the upstream error page's lines of these
// names before it replays the stored ones, so the error page's policy is
// never mixed into the cached representation's.
bool IsSidecarCarriedName(std::string_view name);

// Does any carried `link` field (name compared case-insensitively) already
// send this hint?  A comma-separated link-value matches when its `<target>`
// equals `url` byte for byte, its `rel` parameter (quotes stripped, split on
// spaces, compared case-insensitively) contains `rel`, and it has a
// `crossorigin` parameter exactly when `crossorigin` is true.  The HTML hit
// path adds its own hint lines; it skips one the origin's Link already sent.
bool SidecarLinkCarriesHint(std::span<const HeaderField> fields,
                            std::string_view url, std::string_view rel,
                            bool crossorigin);

// Does the stale-if-error serve of this stash drop the error page's lines of
// the carried names?  For every classified entry — including one with nothing
// to carry, whose origin sent none of those names, so the error page's must
// not appear either.  Entries recorded before the gate keep the previous
// behaviour.
bool StaleErrorDropsCarriedNames(uint8_t entry_flags);

// May a stashed stale entry be served in place of an upstream error, as far
// as its stored headers go?  An entry that requires a block may only be
// served with it; without one the upstream error passes through (fail
// closed: it takes a lost block and an origin outage at the same time).
bool StaleErrorServeHasHeaders(uint8_t entry_flags, bool have_block);

}  // namespace pagespeed

#endif  // PAGESPEED_SRC_NGINX_HEADERS_SIDECAR_REPLAY_H_
