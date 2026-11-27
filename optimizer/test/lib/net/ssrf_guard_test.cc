// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

#include "lib/net/ssrf_guard.h"

#include <string>
#include <vector>

#include "gtest/gtest.h"

// Adversarial battery for the agent_optimize SSRF guard.
// Each test maps to a real bypass class. The contract is byte-for-byte verdict
// parity with agent-readability-scanner/src/ssrf.mjs; where this asserts a
// behavior that the spec's own prose got wrong (e.g. all `::ffff:` blocked), the
// test cites ssrf.mjs as authoritative.

namespace pagespeed {
namespace {

// ---------------------------------------------------------------------------
// IsPrivateV4 — range coverage + boundaries (block == "is private/reserved")
// ---------------------------------------------------------------------------

TEST(SsrfGuardV4, LoopbackAndRfc1918Blocked) {
  EXPECT_TRUE(IsPrivateV4("127.0.0.1"));
  EXPECT_TRUE(IsPrivateV4("127.255.255.255"));
  EXPECT_TRUE(IsPrivateV4("10.0.0.1"));
  EXPECT_TRUE(IsPrivateV4("172.16.0.1"));
  EXPECT_TRUE(IsPrivateV4("172.31.255.255"));
  EXPECT_TRUE(IsPrivateV4("192.168.1.1"));
}

TEST(SsrfGuardV4, PublicAllowed) {
  EXPECT_FALSE(IsPrivateV4("8.8.8.8"));
  EXPECT_FALSE(IsPrivateV4("1.1.1.1"));
  EXPECT_FALSE(IsPrivateV4("93.184.216.34"));
}

TEST(SsrfGuardV4, Rfc1918_172_BoundariesOffByOne) {
  EXPECT_FALSE(IsPrivateV4("172.15.255.255"));  // just below /12
  EXPECT_FALSE(IsPrivateV4("172.32.0.0"));      // just above /12
}

TEST(SsrfGuardV4, MetadataIpBlocked) {
  EXPECT_TRUE(IsPrivateV4("169.254.169.254"));  // cloud metadata
  EXPECT_TRUE(IsPrivateV4("169.254.0.1"));
}

TEST(SsrfGuardV4, CgnatBlockedWithBoundaries) {
  EXPECT_TRUE(IsPrivateV4("100.64.0.1"));
  EXPECT_TRUE(IsPrivateV4("100.127.255.255"));
  EXPECT_FALSE(IsPrivateV4("100.63.255.255"));  // below 100.64/10
  EXPECT_FALSE(IsPrivateV4("100.128.0.0"));     // above 100.64/10
}

TEST(SsrfGuardV4, BenchmarkBlockedWithBoundaries) {
  EXPECT_TRUE(IsPrivateV4("198.18.0.1"));
  EXPECT_TRUE(IsPrivateV4("198.19.255.255"));
  EXPECT_FALSE(IsPrivateV4("198.17.0.0"));
  EXPECT_FALSE(IsPrivateV4("198.20.0.0"));
}

TEST(SsrfGuardV4, ThisNetworkAndMulticastReservedBlocked) {
  EXPECT_TRUE(IsPrivateV4("0.0.0.0"));
  EXPECT_TRUE(IsPrivateV4("0.1.2.3"));
  EXPECT_TRUE(IsPrivateV4("224.0.0.1"));
  EXPECT_TRUE(IsPrivateV4("239.255.255.255"));
  EXPECT_TRUE(IsPrivateV4("240.0.0.1"));
  EXPECT_TRUE(IsPrivateV4("255.255.255.255"));
}

TEST(SsrfGuardV4, Net192_0_0_24Blocked) {
  EXPECT_TRUE(IsPrivateV4("192.0.0.1"));
  // ssrf.mjs covers 192.0.0.0/24 but NOT 192.0.2.0/24 (TEST-NET-1). Pin the
  // (documented) gap so a future widening is a conscious change.
  EXPECT_FALSE(IsPrivateV4("192.0.2.0"));
}

TEST(SsrfGuardV4, MalformedLabelsFailClosed) {
  EXPECT_TRUE(IsPrivateV4("999.1.1.1"));  // octet > 255
  EXPECT_TRUE(IsPrivateV4("1.2.3"));      // too few labels
  EXPECT_TRUE(IsPrivateV4("1.2.3.4.5"));  // too many labels
  EXPECT_TRUE(IsPrivateV4("a.b.c.d"));    // non-numeric
  EXPECT_TRUE(IsPrivateV4(""));           // empty
}

TEST(SsrfGuardV4, JsNumberLiteralParity) {
  // Direct-predicate parity with ssrf.mjs's Number() per-label parse. These are
  // ALSO covered (and blocked) at the IsPrivateIp layer below via inet_pton; the
  // point here is to pin the predicate's standalone behavior == JS.
  // Number("0177") === 177 (decimal, NOT octal) → 177 is not private → allow.
  EXPECT_FALSE(IsPrivateV4("0177.0.0.1"));
  // Number("0x7f") === 127 → loopback → block.
  EXPECT_TRUE(IsPrivateV4("0x7f.0.0.1"));
}

// ---------------------------------------------------------------------------
// IsPrivateV6
// ---------------------------------------------------------------------------

TEST(SsrfGuardV6, LoopbackAndUnspecifiedBlocked) {
  EXPECT_TRUE(IsPrivateV6("::1"));
  EXPECT_TRUE(IsPrivateV6("::"));
}

TEST(SsrfGuardV6, AllV4MappedBlockedRegardlessOfPublicness) {
  // ssrf.mjs blocks EVERY `::ffff:`-prefixed address (the startsWith check fires
  // unconditionally after the embedded-private test). The spec prose's claim that
  // a public embedded v4 is allowed is WRONG; ssrf.mjs is authoritative.
  EXPECT_TRUE(IsPrivateV6("::ffff:127.0.0.1"));  // embedded private
  EXPECT_TRUE(
      IsPrivateV6("::ffff:93.184.216.34"));   // embedded PUBLIC → still blocked
  EXPECT_TRUE(IsPrivateV6("::ffff:7f00:1"));  // non-dotted v4-mapped
}

TEST(SsrfGuardV6, Nat64EmbeddedPrivateBlockedPublicAllowed) {
  EXPECT_TRUE(IsPrivateV6("64:ff9b::127.0.0.1"));  // embedded private
  EXPECT_FALSE(
      IsPrivateV6("64:ff9b::93.184.216.34"));  // embedded public, not ::ffff:
}

TEST(SsrfGuardV6, UlaLinkLocalMulticastDocBlocked) {
  EXPECT_TRUE(IsPrivateV6("fc00::1"));
  EXPECT_TRUE(IsPrivateV6("fd00::1"));
  EXPECT_TRUE(IsPrivateV6("fd00:ec2::254"));  // AWS metadata ULA
  EXPECT_TRUE(IsPrivateV6("fe80::1"));
  EXPECT_TRUE(IsPrivateV6("febf::1"));
  EXPECT_TRUE(IsPrivateV6("ff02::1"));
  EXPECT_TRUE(IsPrivateV6("2001:db8::1"));
}

TEST(SsrfGuardV6, PublicV6Allowed) {
  EXPECT_FALSE(IsPrivateV6("2606:4700:4700::1111"));  // Cloudflare DNS
}

TEST(SsrfGuardV6, CaseInsensitiveAndZoneIdStripped) {
  EXPECT_TRUE(IsPrivateV6("FE80::1"));
  EXPECT_TRUE(IsPrivateV6("Fc00::1"));
  EXPECT_TRUE(IsPrivateV6("fe80::1%eth0"));  // zone id dropped before match
}

// ---------------------------------------------------------------------------
// IsPrivateIp — the real call path (inet_pton-gated, unparseable = deny)
// ---------------------------------------------------------------------------

TEST(SsrfGuardIp, ParseableLiteralsClassified) {
  EXPECT_TRUE(IsPrivateIp("127.0.0.1"));
  EXPECT_FALSE(IsPrivateIp("8.8.8.8"));
  EXPECT_TRUE(IsPrivateIp("::1"));
  EXPECT_FALSE(IsPrivateIp("2606:4700:4700::1111"));
}

TEST(SsrfGuardIp, NonCanonicalLiteralsDeniedByGate) {
  // inet_pton (net.isIP analog) rejects these → unparseable → deny, matching
  // ssrf.mjs's isPrivateIp (net.isIP returns 0 → returns true). This is where the
  // octal/hex/decimal "literal" encoding tricks are actually stopped.
  EXPECT_TRUE(IsPrivateIp("0177.0.0.1"));  // leading-zero form
  EXPECT_TRUE(IsPrivateIp("0x7f000001"));  // hex dword
  EXPECT_TRUE(IsPrivateIp("2130706433"));  // decimal dword (127.0.0.1)
  EXPECT_TRUE(IsPrivateIp("0x7f.0.0.1"));  // mixed hex label
}

TEST(SsrfGuardIp, GarbageDeniedFailClosed) {
  EXPECT_TRUE(IsPrivateIp("not-an-ip"));
  EXPECT_TRUE(IsPrivateIp(""));
  EXPECT_TRUE(IsPrivateIp("999.999.999.999"));
  EXPECT_TRUE(IsPrivateIp("1.2.3"));
  EXPECT_TRUE(IsPrivateIp("1.2.3.4.5"));
}

// ---------------------------------------------------------------------------
// Scheme allowlist
// ---------------------------------------------------------------------------

TEST(SsrfGuardScheme, OnlyHttpAndHttps) {
  EXPECT_TRUE(IsAllowedScheme("http"));
  EXPECT_TRUE(IsAllowedScheme("https"));
  EXPECT_TRUE(IsAllowedScheme("HTTP"));    // case-insensitive
  EXPECT_TRUE(IsAllowedScheme("https:"));  // tolerate trailing colon
  EXPECT_FALSE(IsAllowedScheme("file"));
  EXPECT_FALSE(IsAllowedScheme("ftp"));
  EXPECT_FALSE(IsAllowedScheme("gopher"));
  EXPECT_FALSE(IsAllowedScheme("data"));
  EXPECT_FALSE(IsAllowedScheme("ws"));
  EXPECT_FALSE(IsAllowedScheme("javascript"));
  EXPECT_FALSE(IsAllowedScheme(""));
}

// ---------------------------------------------------------------------------
// Host-literal denylist (metadata special names; spec §3.3.4)
// ---------------------------------------------------------------------------

TEST(SsrfGuardHostLiteral, MetadataNamesBlocked) {
  EXPECT_TRUE(IsBlockedHostLiteral("metadata.google.internal"));
  EXPECT_TRUE(IsBlockedHostLiteral("METADATA.GOOGLE.INTERNAL"));
  EXPECT_TRUE(
      IsBlockedHostLiteral("metadata.google.internal."));  // trailing dot
  EXPECT_TRUE(IsBlockedHostLiteral("metadata"));
  EXPECT_FALSE(IsBlockedHostLiteral("example.com"));
  EXPECT_FALSE(IsBlockedHostLiteral("metadata.example.com"));
}

// ---------------------------------------------------------------------------
// ParseHttpUrl — authority extraction, the URL-parser bypass surface
// ---------------------------------------------------------------------------

TEST(SsrfGuardUrl, BasicAuthorityAndScheme) {
  auto u = ParseHttpUrl("http://example.com/path?q=1");
  ASSERT_TRUE(u.has_value());
  EXPECT_EQ(u->scheme, "http");
  EXPECT_EQ(u->host, "example.com");
  EXPECT_FALSE(u->host_is_ip_literal);
}

TEST(SsrfGuardUrl, PortStrippedFromHost) {
  auto u = ParseHttpUrl("https://example.com:8443/x");
  ASSERT_TRUE(u.has_value());
  EXPECT_EQ(u->host, "example.com");
}

TEST(SsrfGuardUrl, UserinfoHostIsAfterLastAt) {
  // The classic SSRF parser trap: host must be the authority AFTER the last '@'.
  auto u = ParseHttpUrl("http://expected.com@169.254.169.254/latest/meta-data");
  ASSERT_TRUE(u.has_value());
  EXPECT_EQ(u->host, "169.254.169.254");
  EXPECT_TRUE(u->host_is_ip_literal);

  auto u2 = ParseHttpUrl("http://a@b@127.0.0.1/");
  ASSERT_TRUE(u2.has_value());
  EXPECT_EQ(u2->host, "127.0.0.1");

  auto u3 = ParseHttpUrl("http://user:pass@127.0.0.1/");
  ASSERT_TRUE(u3.has_value());
  EXPECT_EQ(u3->host, "127.0.0.1");
}

TEST(SsrfGuardUrl, Ipv6LiteralBracketsStripped) {
  auto u = ParseHttpUrl("http://[::1]:8080/x");
  ASSERT_TRUE(u.has_value());
  EXPECT_EQ(u->host, "::1");
  EXPECT_TRUE(u->host_is_ip_literal);
}

TEST(SsrfGuardUrl, HostLowercased) {
  auto u = ParseHttpUrl("HTTP://EXAMPLE.COM/");
  ASSERT_TRUE(u.has_value());
  EXPECT_EQ(u->scheme, "http");
  EXPECT_EQ(u->host, "example.com");
}

TEST(SsrfGuardUrl, MalformedRejected) {
  EXPECT_FALSE(ParseHttpUrl("not a url").has_value());
  EXPECT_FALSE(ParseHttpUrl("http://").has_value());         // empty authority
  EXPECT_FALSE(ParseHttpUrl("://example.com").has_value());  // no scheme
  EXPECT_FALSE(
      ParseHttpUrl("http://@/path").has_value());  // empty host after @
}

// ---------------------------------------------------------------------------
// ResolveSafe / IsPublicUrl — the end-to-end verdict
// ---------------------------------------------------------------------------

TEST(SsrfGuardResolve, IpLiteralHostValidatedDirectly) {
  // Host is an IP literal → resolved_addresses ignored (ssrf.mjs net.isIP branch).
  EXPECT_FALSE(IsPublicUrl("http://127.0.0.1:6379/", {}));  // Redis on loopback
  EXPECT_FALSE(IsPublicUrl("http://169.254.169.254/latest/", {}));
  EXPECT_TRUE(IsPublicUrl("http://8.8.8.8/", {}));
  EXPECT_FALSE(IsPublicUrl("http://[::1]/", {}));
}

TEST(SsrfGuardResolve, HostnameNeedsPublicResolvedAddresses) {
  EXPECT_TRUE(IsPublicUrl("https://example.com/", {"93.184.216.34"}));
  // ANY private resolved address → block (DNS-rebinding / split-horizon defense).
  EXPECT_FALSE(
      IsPublicUrl("https://evil.test/", {"93.184.216.34", "127.0.0.1"}));
  // Empty resolved set → block (can't prove public).
  EXPECT_FALSE(IsPublicUrl("https://example.com/", {}));
}

TEST(SsrfGuardResolve, MetadataHostnameBlockedEvenIfResolverLies) {
  // Belt-and-suspenders: blocked by name regardless of what the resolver returns.
  EXPECT_FALSE(IsPublicUrl("http://metadata.google.internal/", {"8.8.8.8"}));
}

TEST(SsrfGuardResolve, SchemeEnforcedBeforeAddresses) {
  EXPECT_FALSE(IsPublicUrl("file:///etc/passwd", {}));
  EXPECT_FALSE(IsPublicUrl("gopher://127.0.0.1/", {}));
  EXPECT_FALSE(IsPublicUrl("ftp://example.com/", {"93.184.216.34"}));
}

TEST(SsrfGuardResolve, PunycodeHostJudgedByResolvedAddress) {
  // The guard does not IDN-decode; it trusts the caller's resolved addresses,
  // exactly as ssrf.mjs hands the host to dns.lookup. A punycode host resolving
  // private is blocked; resolving public passes.
  EXPECT_FALSE(IsPublicUrl("http://xn--80ak6aa92e.com/", {"10.0.0.5"}));
  EXPECT_TRUE(IsPublicUrl("http://xn--80ak6aa92e.com/", {"93.184.216.34"}));
}

TEST(SsrfGuardResolve, PortIrrelevantToVerdict) {
  EXPECT_FALSE(IsPublicUrl("http://127.0.0.1:22/", {}));
  EXPECT_FALSE(IsPublicUrl("http://127.0.0.1:6379/", {}));
  EXPECT_TRUE(IsPublicUrl("http://8.8.8.8:443/", {}));
}

// ===========================================================================
// Adversarial-review hardening (M1-M4 high-severity bypasses, S1-S5, T1-T13).
// Every expected host value below was verified against Node's `new URL()` (the
// exact reference ssrf.mjs uses). NOTE: redirect-hop re-validation is NOT tested
// here — it is the P1.3 fetcher's responsibility, not this offline predicate's.
// ===========================================================================

// M1 — a backslash terminates the authority for special schemes (WHATWG), so the
// host is what precedes it, not the public-looking name after the userinfo `@`.
TEST(SsrfGuardHardening, BackslashTerminatesAuthority) {
  auto u = ParseHttpUrl("http://127.0.0.1\\@example.com/");
  ASSERT_TRUE(u.has_value());
  EXPECT_EQ(u->host, "127.0.0.1");  // new URL() => 127.0.0.1
  EXPECT_FALSE(IsPublicUrl("http://127.0.0.1\\@example.com/", {}));  // BLOCK
}

TEST(SsrfGuardHardening, BackslashMirrorIsActuallyThePublicHost) {
  // Verified via Node: `http://example.com\@127.0.0.1/` => host == "example.com"
  // (the `\` ends the authority at example.com; 127.0.0.1 becomes path). The
  // earlier review draft had this backwards — pinning the CORRECT behavior.
  auto u = ParseHttpUrl("http://example.com\\@127.0.0.1/");
  ASSERT_TRUE(u.has_value());
  EXPECT_EQ(u->host, "example.com");
  EXPECT_TRUE(
      IsPublicUrl("http://example.com\\@127.0.0.1/", {"93.184.216.34"}));
}

TEST(SsrfGuardHardening, BackslashOpensAuthority) {
  // `http:\\127.0.0.1/` == `http://127.0.0.1/` for special schemes.
  auto u = ParseHttpUrl("http:\\\\127.0.0.1/");
  ASSERT_TRUE(u.has_value());
  EXPECT_EQ(u->host, "127.0.0.1");
  EXPECT_FALSE(IsPublicUrl("http:\\\\127.0.0.1/", {}));  // BLOCK
}

// M2 — legacy/encoded IPv4 hosts are normalized to dotted-decimal by new URL();
// they MUST be classified as literals (and blocked) even when the caller hands us
// a public resolved address. Canonicalize-then-classify (not blanket block):
// 01.01.01.01 -> 1.1.1.1 stays public/ALLOW, exactly like ssrf.mjs.
TEST(SsrfGuardHardening, EncodedIpv4LiteralsBlocked) {
  const std::vector<std::string> lies = {"93.184.216.34"};  // attacker-supplied
  EXPECT_FALSE(IsPublicUrl("http://0x7f000001/", lies));    // hex dword
  EXPECT_FALSE(IsPublicUrl("http://2130706433/", lies));    // decimal dword
  EXPECT_FALSE(IsPublicUrl("http://0177.0.0.1/", lies));    // octal label
  EXPECT_FALSE(IsPublicUrl("http://017700000001/", lies));  // octal dword
  EXPECT_FALSE(IsPublicUrl("http://0x7f.0.0.1/", lies));    // mixed hex label
  // public encoded form stays allowed (canonicalize, don't blanket-block)
  EXPECT_TRUE(IsPublicUrl("http://01.01.01.01/", {"1.1.1.1"}));  // -> 1.1.1.1
  // legacy short forms canonicalize like new URL() (1.2 -> 1.0.0.2, 0 -> 0.0.0.0)
  EXPECT_FALSE(IsPublicUrl("http://0/", {}));   // 0.0.0.0 -> BLOCK
  EXPECT_TRUE(IsPublicUrl("http://1.2/", {}));  // 1.0.0.2 -> ALLOW
}

// M3 — wide-accumulator: a label crossing 2^31 must read as >255 (BLOCK), with no
// signed-overflow UB (which on win-x64/LLP64 would wrap and flip to ALLOW).
TEST(SsrfGuardHardening, JsNumberLabelWideAndOverflowSafe) {
  EXPECT_TRUE(IsPrivateV4("0x100000001.0.0.5"));  // 4294967297 > 255
  EXPECT_TRUE(IsPrivateV4("0x10000007f.0.0.1"));
  EXPECT_TRUE(IsPrivateV4("0xfffffff0.0.0.1"));
  EXPECT_TRUE(IsPrivateV4("2147483650.0.0.1"));  // > 2^31 decimal
}

// M4 — tab/LF/CR stripped from the URL (WHATWG) before parsing, so they can't
// split the host and downgrade a loopback literal to a deferred hostname.
TEST(SsrfGuardHardening, WhitespaceStrippedFromUrl) {
  auto u = ParseHttpUrl("http://127.0.0.1\t/");
  ASSERT_TRUE(u.has_value());
  EXPECT_EQ(u->host, "127.0.0.1");
  EXPECT_FALSE(IsPublicUrl("http://127.0.0.1\t/", {}));
  EXPECT_FALSE(IsPublicUrl("http://user@\t127.0.0.1/", {}));
  EXPECT_FALSE(
      IsPublicUrl("http://127.0.0\n.1/", {}));  // LF inside host removed
}

// S1 — a single trailing root dot is stripped for the IPv4 parse.
TEST(SsrfGuardHardening, TrailingDotIpv4Literal) {
  EXPECT_FALSE(IsPublicUrl("http://127.0.0.1./", {}));  // -> 127.0.0.1 BLOCK
}

// S3 — any private address among the resolved set blocks, in any position.
TEST(SsrfGuardHardening, MultiAddressPrivateAnyOrder) {
  EXPECT_FALSE(IsPublicUrl("https://x.test/", {"127.0.0.1", "93.184.216.34"}));
  EXPECT_FALSE(IsPublicUrl("https://x.test/",
                           {"93.184.216.34", "203.0.113.5", "10.0.0.5"}));
  EXPECT_TRUE(IsPublicUrl("https://x.test/", {"93.184.216.34", "203.0.113.5"}));
}

// S4 — zone-id handling end-to-end through IsPrivateIp / IsPublicUrl, incl. the
// bracketed-literal-with-zone case that must NOT defer to a (lying) resolved addr.
TEST(SsrfGuardHardening, ZoneIdEndToEnd) {
  EXPECT_TRUE(IsPrivateIp("fe80::1%eth0"));
  EXPECT_FALSE(IsPublicUrl("http://[fe80::1%25eth0]/", {}));
  // even with a public resolved address, the bracketed link-local literal blocks
  EXPECT_FALSE(IsPublicUrl("http://[fe80::1%25eth0]/", {"93.184.216.34"}));
}

// S5 (revised) — an embedded IPv4 address is judged as that address in every
// spelling: dotted or hex, v4-compatible, NAT64 or 6to4.  This goes beyond
// ssrf.mjs on purpose.
TEST(SsrfGuardHardening, Ipv6FormsOfIpv4) {
  EXPECT_TRUE(IsPrivateV6("::127.0.0.1"));
  EXPECT_TRUE(IsPrivateV6("::7f00:1"));
  EXPECT_TRUE(IsPrivateV6("64:ff9b::127.0.0.1"));
  EXPECT_TRUE(IsPrivateV6("64:ff9b::7f00:1"));
  EXPECT_TRUE(IsPrivateV6("64:ff9b::a00:5"));     // 10.0.0.5
  EXPECT_TRUE(IsPrivateV6("64:ff9b::c0a8:101"));  // 192.168.1.1
  EXPECT_TRUE(IsPrivateV6("64:ff9b:1::1"));       // local-use NAT64 range
  EXPECT_TRUE(IsPrivateV6("2002:7f00:1::"));      // 6to4 embedding 127.x
  EXPECT_TRUE(IsPrivateV6("2002:0a00:1::"));      // 6to4 embedding 10.x
  EXPECT_TRUE(IsPrivateV6("2002:a9fe:a9fe::1"));  // 6to4 embedding metadata
  EXPECT_TRUE(IsPrivateV6("0:0:0:0:0:ffff:5db8:d822"));  // mapped, long form
  EXPECT_TRUE(IsPrivateIp("64:ff9b::a9fe:a9fe"));
  EXPECT_TRUE(IsPrivateIp("2002:c0a8:0101::"));
  // Embedded PUBLIC addresses stay public (only v4-mapped is blanket-blocked).
  EXPECT_FALSE(IsPrivateV6("64:ff9b::93.184.216.34"));
  EXPECT_FALSE(IsPrivateV6("64:ff9b::5db8:d822"));
  EXPECT_FALSE(IsPrivateV6("2002:5db8:d822::1"));
  EXPECT_FALSE(IsPrivateV6("2606:4700::1111"));
}

// N2 — site-local / Teredo are NOT blocked by ssrf.mjs; C++ matches.
// Pinned so widening these is a deliberate, parity-coordinated change.
TEST(SsrfGuardHardening, ReservedV6Ranges) {
  EXPECT_TRUE(IsPrivateV6("fec0::1"));              // site-local
  EXPECT_TRUE(IsPrivateV6("feff::1"));              // top of fec0::/10
  EXPECT_TRUE(IsPrivateV6("2001:0:4137:9e76::1"));  // Teredo
  EXPECT_TRUE(IsPrivateV6("2001::5db8:d822:0:0"));  // Teredo, any form
  EXPECT_TRUE(IsPrivateV6("100::1"));               // discard-only
  EXPECT_TRUE(IsPrivateV6("100::ffff:ffff:ffff:ffff"));
  EXPECT_FALSE(IsPrivateV6("100:0:0:1::1"));     // outside 100::/64
  EXPECT_FALSE(IsPrivateV6("2001:4860::8888"));  // public, not Teredo
  // SIIT and ISATAP carrying a private IPv4 address.
  EXPECT_TRUE(IsPrivateV6("::ffff:0:a00:5"));
  EXPECT_TRUE(IsPrivateV6("::ffff:0:127.0.0.1"));
  EXPECT_TRUE(IsPrivateV6("2606:4700::5efe:a00:5"));
  EXPECT_TRUE(IsPrivateV6("2606:4700::200:5efe:192.168.1.1"));
  // ...and a public one.
  // (A ::ffff:-spelled SIIT address is blocked whatever it carries, by the
  // textual ::ffff: rule.)
  EXPECT_TRUE(IsPrivateV6("::ffff:0:5db8:d822"));
  EXPECT_FALSE(IsPrivateV6("2606:4700::5efe:5db8:d822"));
}

TEST(SsrfGuardHardening, IsLinkLocalIpTunnelForms) {
  // Teredo: server 169.254.169.254, or client obfuscated (~a9fe:a9fe).
  EXPECT_TRUE(IsLinkLocalIp("2001:0:a9fe:a9fe::1"));
  EXPECT_TRUE(IsLinkLocalIp("2001:0:5db8:d822::5601:5601"));
  EXPECT_TRUE(IsLinkLocalIp("::ffff:0:a9fe:a9fe"));         // SIIT
  EXPECT_TRUE(IsLinkLocalIp("2606:4700::5efe:a9fe:a9fe"));  // ISATAP
  EXPECT_FALSE(IsLinkLocalIp("2001:0:5db8:d822::1"));
}

// Link-local / metadata carried inside an IPv6 address.
TEST(SsrfGuardHardening, IsLinkLocalIpIpv6Forms) {
  EXPECT_TRUE(IsLinkLocalIp("::ffff:169.254.169.254"));
  EXPECT_TRUE(IsLinkLocalIp("::ffff:a9fe:a9fe"));
  EXPECT_TRUE(IsLinkLocalIp("0:0:0:0:0:ffff:a9fe:a9fe"));
  EXPECT_TRUE(IsLinkLocalIp("::169.254.169.254"));
  EXPECT_TRUE(IsLinkLocalIp("::a9fe:a9fe"));
  EXPECT_TRUE(IsLinkLocalIp("64:ff9b::a9fe:a9fe"));
  EXPECT_TRUE(IsLinkLocalIp("64:ff9b::169.254.169.254"));
  EXPECT_TRUE(IsLinkLocalIp("64:ff9b:1::a9fe:a9fe"));      // /96 inside the /48
  EXPECT_TRUE(IsLinkLocalIp("64:ff9b:1:a9fe:a9:fe00::"));  // /48 position
  EXPECT_TRUE(IsLinkLocalIp("2002:a9fe:a9fe::1"));
  EXPECT_TRUE(IsLinkLocalIp("fe80::1%eth0"));
  EXPECT_FALSE(IsLinkLocalIp("::ffff:10.0.0.5"));
  EXPECT_FALSE(IsLinkLocalIp("64:ff9b::5db8:d822"));
  EXPECT_FALSE(IsLinkLocalIp("2002:5db8:d822::1"));
  EXPECT_FALSE(IsLinkLocalIp("2606:4700::1111"));
}

// N3 — pathological inputs fail closed (and return fast via the >2^31 early-out).
TEST(SsrfGuardHardening, PathologicalInputsFailClosed) {
  EXPECT_TRUE(IsPrivateV4(std::string(50000, '9')));
  EXPECT_TRUE(IsPrivateV4("1" + std::string(40, '0') + ".0.0.1"));
  EXPECT_FALSE(IsPublicUrl("http://" + std::string(65000, 'a') + "/", {}));
}

// N4 — mixed-case scheme + bracketed metadata host literal.
TEST(SsrfGuardHardening, MixedCaseSchemeAndBracketedMetadata) {
  EXPECT_TRUE(IsPublicUrl("HtTp://example.com/", {"93.184.216.34"}));
  EXPECT_FALSE(IsPublicUrl("FILE://x/", {}));
  EXPECT_TRUE(IsBlockedHostLiteral("[metadata.google.internal]"));
}

// S2/T13 — an embedded NUL must not let `::1\0x` classify as the valid prefix
// `::1` under c_str() truncation; fail closed.
TEST(SsrfGuardHardening, EmbeddedNulFailsClosed) {
  const std::string nul_addr =
      std::string("::1") + '\0' + "x";  // "::1\0x", len 5
  EXPECT_TRUE(IsPrivateIp(nul_addr));
  EXPECT_FALSE(IsPublicUrl("https://evil.test/", {nul_addr}));
}

// P1.4 MF-2 — IsLinkLocalIp flags the link-local / cloud-metadata ranges used by
// the G1 carve-out's extra guard. TRUE only for 169.254.0.0/16 and fe80::/10.
TEST(SsrfGuardHardening, IsLinkLocalIpRanges) {
  EXPECT_TRUE(IsLinkLocalIp("169.254.169.254"));  // the cloud metadata endpoint
  EXPECT_TRUE(IsLinkLocalIp("169.254.0.1"));
  EXPECT_TRUE(IsLinkLocalIp("fe80::1"));
  EXPECT_TRUE(IsLinkLocalIp("fe80::abcd:1234"));
  EXPECT_TRUE(IsLinkLocalIp("febf::1"));  // top of fe80::/10
  // NOT link-local: public, loopback, RFC1918, ULA, doc, out-of-range, non-literal.
  EXPECT_FALSE(IsLinkLocalIp("93.184.216.34"));
  EXPECT_FALSE(IsLinkLocalIp("127.0.0.1"));
  EXPECT_FALSE(IsLinkLocalIp("10.0.0.5"));
  EXPECT_FALSE(IsLinkLocalIp("192.168.1.1"));
  EXPECT_FALSE(IsLinkLocalIp("::1"));
  EXPECT_FALSE(IsLinkLocalIp("fec0::1"));  // just outside fe80::/10
  EXPECT_FALSE(IsLinkLocalIp("2001:db8::1"));
  EXPECT_FALSE(IsLinkLocalIp("not-an-ip"));
  EXPECT_FALSE(IsLinkLocalIp("169.254.169.254.5"));  // not a valid literal
}

}  // namespace
}  // namespace pagespeed
