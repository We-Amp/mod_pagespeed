// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

#ifndef LIB_NET_SSRF_GUARD_H_
#define LIB_NET_SSRF_GUARD_H_

#include <optional>
#include <string>
#include <string_view>
#include <vector>

// SSRF guard for the agent_optimize out-of-Chrome subresource fetcher (the
// network-trust kernel; spec G2). This is a C++ port of the scanner's
// `agent-readability-scanner/src/ssrf.mjs`, kept byte-for-byte decision-compatible
// so the public RenderPeek scanner and the in-product worker make IDENTICAL
// allow/deny calls on the same address.
//
// What this stops: the worker's fetcher being used as an open relay into
// loopback, link-local (incl. the cloud metadata endpoint 169.254.169.254),
// RFC1918/ULA, CGNAT, and other reserved ranges. Encoding tricks
// (decimal/octal/hex IPv4, v4-mapped IPv6) are intentionally NOT trusted to the
// textual predicate — they are caught at connect time by re-checking the
// resolver's output (the fetcher resolves once, validates every returned
// address here, then connects to the validated IP literal; see P1.3).
//
// Resolver boundary (the deliberate split vs ssrf.mjs): the worker has NO DNS
// resolver of its own, and this library has ZERO dependencies so it stays
// hermetically unit-testable offline. DNS resolution is therefore the fetcher's
// job (on the libuv work pool); the fetcher passes the resolved A/AAAA addresses
// into ResolveSafe()/IsPublicUrl() here. The range predicates (IsPrivateV4/V6/Ip)
// operate purely on already-textual IPs and need no network.
//
// Fail-closed everywhere: any parse error, unparseable IP, empty resolved set,
// disallowed scheme, or `::ffff:`-prefixed address => BLOCK.

namespace pagespeed {

// Mirrors ssrf.mjs isPrivateV4. true => BLOCK. Operates on the textual label
// form (split on '.', JS Number() per label). A non-4-label input, a non-integer
// or out-of-range label => true (fail-closed). NOTE: in the real call path this
// is only reached via IsPrivateIp() for canonical dotted-decimal (inet_pton-valid)
// input; it is exposed for direct testing and to match ssrf.mjs exactly.
bool IsPrivateV4(std::string_view ip);

// Mirrors ssrf.mjs isPrivateV6. true => BLOCK. Operates on the textual address
// (lower-cased, zone id after '%' dropped). Handles ::1, ::, embedded dotted v4
// (private), ALL `::ffff:`-prefixed (v4-mapped — blocked regardless of embedded
// publicness, exactly as ssrf.mjs does), ULA fc00::/7, link-local fe80::/10,
// multicast ff00::/8, and 2001:db8::/32 documentation.  Beyond ssrf.mjs, on the
// parsed address: v4-mapped in any spelling, the local-use NAT64 range
// 64:ff9b:1::/48, and an IPv4 address embedded by v4-compatible (::/96),
// NAT64 (64:ff9b::/96), 6to4 (2002::/16), Teredo (2001::/32, server and
// de-obfuscated client), SIIT (::ffff:0:a.b.c.d) or ISATAP (::5efe:a.b.c.d)
// form judged as that IPv4 address; Teredo, site-local fec0::/10 and the
// discard prefix 100::/64 are never public.
bool IsPrivateV6(std::string_view ip);

// Mirrors ssrf.mjs isPrivateIp. Classifies via inet_pton (the net.isIP analog):
// a valid IPv4 literal => IsPrivateV4; a valid IPv6 literal => IsPrivateV6 (on
// the original string, so the textual `::ffff:` rule still applies); anything
// not a parseable IP literal => true (BLOCK, unparseable = deny).
bool IsPrivateIp(std::string_view ip);

// True iff `ip` is an IPv4 link-local address (169.254.0.0/16 — the range that
// contains the cloud-metadata endpoint 169.254.169.254) or an IPv6 link-local
// address (fe80::/10). Operates on an already-resolved IP literal (inet_pton);
// returns false for any non-literal input. Used by the G1 upstream carve-out,
// which may legitimately reach loopback/RFC1918 backends but must NEVER pin a
// link-local/metadata address even for the render's own configured origin.
// An IPv6 address that carries an IPv4 link-local address (v4-mapped,
// v4-compatible, NAT64 or 6to4 form, e.g. ::ffff:169.254.169.254 or
// 64:ff9b::a9fe:a9fe) counts as link-local too.
bool IsLinkLocalIp(std::string_view ip);

// Scheme allowlist (ssrf.mjs resolveSafe): only "http"/"https" (case-insensitive,
// colon optional) pass. file:/data:/gopher:/ftp:/ws:/blob:/chrome:/javascript:
// and the empty scheme are rejected.
bool IsAllowedScheme(std::string_view scheme);

// Belt-and-suspenders hostname-literal denylist (spec §3.3.4) layered ON TOP of
// the resolved-IP check: blocks the cloud-metadata special names directly by
// hostname even before resolution, so a resolver that maps a metadata alias to a
// non-link-local address cannot slip through. Trailing dot is normalized away.
// This is NOT part of ssrf.mjs's IP predicate; it is the separate host-literal
// layer the spec mandates.
bool IsBlockedHostLiteral(std::string_view host);

// The authority parsed out of an http(s) URL (the fields ResolveSafe + the G1
// upstream-pin classifier need).
struct UrlAuthority {
  std::string scheme;  // lower-cased, without trailing ':'
  std::string host;  // lower-cased; IPv6 literals have surrounding [] stripped
  bool host_is_ip_literal = false;  // host is itself a valid IP (no DNS needed)
  int port = 0;  // explicit port, else scheme default (http=80, https=443),
                 // else 0 for an unknown scheme; -1 if the port was malformed
  bool had_userinfo = false;  // an '@' was present in the authority (userinfo)
};

// Parse an http(s) URL's authority the way WHATWG `new URL()` does for the parts
// we care about: requires a "scheme://" form; the host is the authority AFTER the
// last '@' (userinfo stripped; `had_userinfo` records that it was present); IPv6
// literals keep only the bracket contents; the port is captured (scheme default
// applied when absent). Returns nullopt on any structural parse failure (=> caller
// blocks). Does NOT validate the scheme or resolve DNS — that is ResolveSafe's job.
std::optional<UrlAuthority> ParseHttpUrl(std::string_view raw_url);

struct ResolveSafeResult {
  std::string host;  // normalized host (brackets stripped)
  std::vector<std::string>
      safe_addresses;  // every address that passed IsPrivateIp==false
};

// resolveSafe-equivalent. Enforces the scheme allowlist and the host-literal
// denylist, then validates addresses:
//   - if the host is itself an IP literal: validate it directly (resolved_addresses
//     is ignored), matching ssrf.mjs's `if (net.isIP(host)) ...` branch.
//   - otherwise: resolved_addresses MUST be non-empty and EVERY entry must be
//     public (IsPrivateIp==false); an empty set or any private entry => BLOCK.
// Returns the validated result, or nullopt on any block/parse failure (fail-closed).
std::optional<ResolveSafeResult> ResolveSafe(
    std::string_view raw_url,
    const std::vector<std::string>& resolved_addresses);

// assertPublicUrl-equivalent: true iff ResolveSafe succeeds.
bool IsPublicUrl(std::string_view raw_url,
                 const std::vector<std::string>& resolved_addresses);

}  // namespace pagespeed

#endif  // LIB_NET_SSRF_GUARD_H_
