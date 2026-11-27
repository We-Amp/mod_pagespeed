// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

#include "lib/net/upstream_pin.h"

#include <optional>
#include <vector>

#include "gtest/gtest.h"

// G1 upstream-pin classifier battery. The dangerous direction here
// is OVER-matching: a page steering a subresource at a host/port that is NOT the
// configured upstream but is wrongly classified as pinned (→ loopback/RFC1918
// egress). Every "not pinned" case below is an attempted lateral pivot.

namespace pagespeed {
namespace {

// A typical SPA-behind-the-module config: the worker's own loopback upstream.
std::vector<PinnedUpstream> Pin(std::initializer_list<const char*> cfgs) {
  std::vector<PinnedUpstream> v;
  for (const char* c : cfgs) {
    auto u = ParseUpstream(c);
    if (u.has_value()) v.push_back(*u);
  }
  return v;
}

TEST(UpstreamPin, ExactMatchAllowed) {
  const auto pinned = Pin({"http://127.0.0.1:3000"});
  EXPECT_TRUE(IsPinnedUpstream("http://127.0.0.1:3000/api/data", pinned));
  EXPECT_TRUE(IsPinnedUpstream("http://127.0.0.1:3000/", pinned));
}

TEST(UpstreamPin, PrivateButWrongPortDenied) {
  // The single most important deny: the SPA fetches its own-host Redis/SSH.
  const auto pinned = Pin({"http://127.0.0.1:3000"});
  EXPECT_FALSE(IsPinnedUpstream("http://127.0.0.1:6379/", pinned));  // Redis
  EXPECT_FALSE(IsPinnedUpstream("http://127.0.0.1:22/", pinned));    // SSH
  EXPECT_FALSE(IsPinnedUpstream("http://127.0.0.1/", pinned));  // :80 default
}

TEST(UpstreamPin, DifferentPrivateHostDenied) {
  const auto pinned = Pin({"http://127.0.0.1:3000"});
  EXPECT_FALSE(IsPinnedUpstream("http://192.168.1.1:3000/admin", pinned));
  EXPECT_FALSE(IsPinnedUpstream("http://10.0.0.5:3000/", pinned));
}

TEST(UpstreamPin, SchemeMustMatch) {
  const auto pinned = Pin({"http://backend.internal:8080"});
  EXPECT_TRUE(IsPinnedUpstream("http://backend.internal:8080/x", pinned));
  EXPECT_FALSE(IsPinnedUpstream("https://backend.internal:8080/x", pinned));
}

TEST(UpstreamPin, UserinfoNeverPinned) {
  const auto pinned = Pin({"http://127.0.0.1:3000"});
  // post-@ host is evil.com → not the upstream anyway, but also userinfo-rejected
  EXPECT_FALSE(IsPinnedUpstream("http://127.0.0.1:3000@evil.com/", pinned));
  // post-@ host IS the upstream, but userinfo presence alone forces a miss → G2
  EXPECT_FALSE(IsPinnedUpstream("http://evil.com@127.0.0.1:3000/", pinned));
}

TEST(UpstreamPin, NoSubstringOrSuffixMatch) {
  const auto pinned = Pin({"http://backend.internal:8080"});
  EXPECT_FALSE(
      IsPinnedUpstream("http://backend.internal.evil.com:8080/", pinned));
  EXPECT_FALSE(IsPinnedUpstream("http://evilbackend.internal:8080/", pinned));
  EXPECT_FALSE(
      IsPinnedUpstream("http://backend.internal.attacker:8080/", pinned));
}

TEST(UpstreamPin, HostCaseInsensitive) {
  const auto pinned = Pin({"http://Backend.Internal:8080"});
  EXPECT_TRUE(IsPinnedUpstream("http://BACKEND.INTERNAL:8080/", pinned));
}

TEST(UpstreamPin, TrailingDotIsSameHost) {
  const auto pinned = Pin({"http://backend.internal:8080"});
  EXPECT_TRUE(IsPinnedUpstream("http://backend.internal.:8080/", pinned));
}

TEST(UpstreamPin, Ipv6CanonicalizedForms) {
  const auto pinned = Pin({"http://[::1]:3000"});
  EXPECT_TRUE(IsPinnedUpstream("http://[::1]:3000/", pinned));
  EXPECT_TRUE(IsPinnedUpstream("http://[0:0:0:0:0:0:0:1]:3000/", pinned));
  EXPECT_FALSE(IsPinnedUpstream("http://[::2]:3000/", pinned));
}

TEST(UpstreamPin, DefaultPortsCanonicalized) {
  const auto pinned = Pin({"http://backend.internal"});  // implicit :80
  EXPECT_TRUE(IsPinnedUpstream("http://backend.internal:80/", pinned));
  EXPECT_TRUE(IsPinnedUpstream("http://backend.internal/", pinned));
  EXPECT_FALSE(IsPinnedUpstream("http://backend.internal:8080/", pinned));

  const auto pinned_s = Pin({"https://backend.internal"});  // implicit :443
  EXPECT_TRUE(IsPinnedUpstream("https://backend.internal:443/", pinned_s));
  EXPECT_TRUE(IsPinnedUpstream("https://backend.internal/", pinned_s));
}

TEST(UpstreamPin, EncodedIpUpstreamCanonicalizesConsistently) {
  // The config and the page may spell the loopback IP differently; both must
  // canonicalize to 127.0.0.1 and match (and only at the configured port).
  const auto pinned = Pin({"http://0x7f000001:3000"});  // -> 127.0.0.1:3000
  EXPECT_TRUE(IsPinnedUpstream("http://127.0.0.1:3000/", pinned));
  EXPECT_TRUE(IsPinnedUpstream("http://2130706433:3000/", pinned));
  EXPECT_FALSE(IsPinnedUpstream("http://127.0.0.1:6379/", pinned));
}

TEST(UpstreamPin, MultipleUpstreams) {
  const auto pinned =
      Pin({"http://127.0.0.1:3000", "https://api.internal:443"});
  EXPECT_TRUE(IsPinnedUpstream("http://127.0.0.1:3000/a", pinned));
  EXPECT_TRUE(IsPinnedUpstream("https://api.internal/b", pinned));
  EXPECT_FALSE(IsPinnedUpstream("http://api.internal:3000/",
                                pinned));  // wrong scheme/port
}

TEST(UpstreamPin, EmptyPinListNeverMatches) {
  EXPECT_FALSE(IsPinnedUpstream("http://127.0.0.1:3000/", {}));
}

TEST(UpstreamPin, MalformedSubresourceNotPinned) {
  const auto pinned = Pin({"http://127.0.0.1:3000"});
  EXPECT_FALSE(IsPinnedUpstream("not a url", pinned));
  EXPECT_FALSE(
      IsPinnedUpstream("ftp://127.0.0.1:3000/", pinned));  // bad scheme
  EXPECT_FALSE(
      IsPinnedUpstream("http://127.0.0.1:99999/", pinned));  // bad port
}

// H4 — an IPv6 zone-id is load-bearing for link-local routing; a zoned host is not
// a valid pin target (fail-closed), so it can neither be configured nor over-match.
TEST(UpstreamPin, ZoneIdHostsAreNotPinnable) {
  EXPECT_FALSE(ParseUpstream("http://[fe80::1%25eth0]:3000").has_value());
  const auto pinned =
      Pin({"http://[fe80::1]:3000"});  // zoneless link-local pin
  ASSERT_EQ(pinned.size(), 1u);
  EXPECT_TRUE(IsPinnedUpstream("http://[fe80::1]:3000/", pinned));
  // a zoned subresource must NOT match the zoneless pin (different scope/NIC)
  EXPECT_FALSE(IsPinnedUpstream("http://[fe80::1%25eth1]:3000/", pinned));
}

// N2 — host_is_ip_literal is part of the identity key (a hostname can never
// string-equal an IP-literal pin even if literal recognition ever changes).
TEST(UpstreamPin, IpLiteralFlagIsPartOfIdentity) {
  const auto ip = ParseUpstream("http://127.0.0.1:3000");
  const auto host = ParseUpstream("http://backend.internal:3000");
  ASSERT_TRUE(ip.has_value());
  ASSERT_TRUE(host.has_value());
  EXPECT_TRUE(ip->is_ip_literal);
  EXPECT_FALSE(host->is_ip_literal);
}

TEST(UpstreamPin, ParseUpstreamRejectsBadConfig) {
  EXPECT_FALSE(ParseUpstream("not a url").has_value());
  EXPECT_FALSE(ParseUpstream("ftp://backend.internal:21").has_value());
  EXPECT_FALSE(ParseUpstream("http://user:pass@127.0.0.1:3000")
                   .has_value());  // userinfo
  EXPECT_FALSE(
      ParseUpstream("http://127.0.0.1:99999").has_value());  // bad port
  auto ok = ParseUpstream("http://127.0.0.1:3000/ignored/path?q=1");
  ASSERT_TRUE(ok.has_value());
  EXPECT_EQ(ok->scheme, "http");
  EXPECT_EQ(ok->canon_host, "127.0.0.1");
  EXPECT_EQ(ok->port, 3000);
}

}  // namespace
}  // namespace pagespeed
