// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

#ifndef PAGESPEED_SRC_NGINX_AUTHZ_CACHE_GATE_H_
#define PAGESPEED_SRC_NGINX_AUTHZ_CACHE_GATE_H_

// RFC 9111 §3.5 shared-cache gate for requests carrying Authorization.
// Pure C++ (no nginx headers) so the decision logic is unit-testable
// without an nginx build (same pattern as etag_util).
//
// A shared cache MUST NOT use a stored response to satisfy a request with
// an Authorization header, and MUST NOT store the response to such a
// request, unless the response contains a cache directive that explicitly
// allows it.  RFC 9111 §3.5 names exactly three permitting response
// directives:
//   public, must-revalidate, s-maxage
// (proxy-revalidate is deliberately NOT treated as a permit — §3.5's
// enumeration is exhaustive, and widening it would widen the exception.
// Proxy-Authorization is likewise out of scope: §3.5 names Authorization
// only, and the header is hop-by-hop — consumed by the first proxy; nginx
// binds no headers_in slot for it.)
//
// Both call sites speak the AlternateMetadata origin_cc_flags bit
// vocabulary: the record path passes the just-parsed response flags, the
// HIT path passes the STORED entry's flags.  The s-maxage permit keys on
// kCCOriginSMaxagePresent alone — the flag also covers s-maxage=0, which
// is a valid §3.5 permit (the entry is merely immediately stale).  A
// stored s-maxage VALUE without the flag cannot occur: the flag and value
// were introduced atomically in metadata v3, the parser sets both
// together, and every writer (worker WriteVariant, 304 restamp) copies
// them wholesale, while Deserialize rejects pre-v3 metadata.  So
// origin_cc_flags == 0 means the origin sent no Cache-Control at store
// time (or the metadata is corrupt) — no permission signal either way,
// and the gate fails closed for Authorization-bearing requests.
//
// RSL-CAP exemption (applied by the caller, not here): an
// `Authorization: License ...` credential that the module's own PREACCESS
// RSL-CAP gate validated and terminated is NOT end-user authentication
// toward the origin — the origin response is not conditioned on it — so
// it must not trigger this gate.  The exemption is DERIVED at gate time
// from live enforcement state plus the header's auth scheme, with no
// per-request state: with rsl_cap_enforcement ON, PREACCESS 401s every
// main external request whose Authorization is not a VALID License token,
// so a License-scheme credential that reaches a serve/record gate on that
// same main external pass was necessarily validated and terminated by the
// module.  The exemption therefore applies ONLY where PREACCESS
// adjudicated: never on internal requests (r->internal — error_page /
// index / try_files redirect targets and subrequests), which PREACCESS
// declines.  In particular the error_page target of the PREACCESS 401
// itself re-enters the module with the UNVALIDATED header still attached;
// exempting it would let an invalid License credential reach cached
// content ungated.  Internal requests carrying a License-scheme header
// are treated as ordinary Authorization (§3.5 gating, fail-closed).
// Enforcement off, invalid tokens (401'd before ever reaching a gate),
// and every other scheme likewise stay fail-closed.  Accepted edge:
// enforcement flipping ON between PREACCESS and a gate can exempt a
// License-scheme header that was never validated for that single
// in-flight window (bounded to one shared-config poll interval, ~1s); the
// consequence is pre-fix behavior for an agent-addressed credential,
// never end-user origin auth.

#include <cstdint>

namespace pagespeed {

// Returns true when a shared cache may store (record path) or serve (HIT
// path) the response even though the request carries an Authorization
// header — i.e. when the response's Cache-Control carries an explicit
// permit (public / must-revalidate / s-maxage).  When
// request_has_authorization is false the gate is a no-op (always true).
bool AuthzCacheGateAllows(bool request_has_authorization,
                          uint16_t origin_cc_flags);

// Stale tightening for EVERY stale reuse of a stored entry — the
// bounded-SWR coalesced serve AND the stale-if-error serve: once an entry
// is STALE, must-revalidate and s-maxage require revalidation before
// reuse (RFC 9111 §4.2.4, §5.2.2.10) — the very premise under which §3.5
// permits them — so for an Authorization-bearing request a stale serve
// additionally requires `public`.  Anonymous requests keep the
// pre-existing bounded-stale / stale-if-error behavior unchanged (always
// true).
bool AuthzCacheGateAllowsStale(bool request_has_authorization,
                               uint16_t origin_cc_flags);

// Full permission for ANY stale reuse of a stored entry.  RFC 9111
// §5.2.2.2 / §5.2.2.8: a cache MUST NOT use a stale response whose
// Cache-Control carried must-revalidate (or, for a shared cache,
// proxy-revalidate / s-maxage) without successful validation, so such an entry is
// refused for every request, anonymous or not.  Otherwise defers to
// AuthzCacheGateAllowsStale.
bool StaleServePermitted(bool request_has_authorization,
                         uint16_t origin_cc_flags);

}  // namespace pagespeed

#endif  // PAGESPEED_SRC_NGINX_AUTHZ_CACHE_GATE_H_
