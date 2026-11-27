// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 We-Amp B.V.

// Tests for the recognition of the worker's agent fetches arriving from this
// server (src/nginx/agent_self_fetch.h): the User-Agent marker, the peer test
// over IPv4, IPv6 and IPv4-mapped addresses, and a pin that the worker's
// fetcher still sends the marker.

#include "src/nginx/agent_self_fetch.h"

#include <array>
#include <span>
#include <string>
#include <vector>

#include "gtest/gtest.h"
#include "lib/net/curl_fetcher.h"

using pagespeed::AgentFetchPeerIsThisServer;
using pagespeed::IsAgentFetchUserAgent;
using pagespeed::IsAgentSelfFetch;
using pagespeed::kAgentFetchUserAgentMarker;
using pagespeed::kAgentSelfFetchLabel;
using pagespeed::kAgentSelfFetchLabelBytes;

namespace {

using Addr = std::vector<unsigned char>;

Addr V4(unsigned char a, unsigned char b, unsigned char c, unsigned char d) {
  return {a, b, c, d};
}

// 16 bytes from eight 16-bit groups.
Addr V6(std::array<unsigned, 8> groups) {
  Addr out;
  for (unsigned g : groups) {
    out.push_back(static_cast<unsigned char>(g >> 8));
    out.push_back(static_cast<unsigned char>(g & 0xff));
  }
  return out;
}

// ::ffff:a.b.c.d
Addr Mapped(unsigned char a, unsigned char b, unsigned char c,
            unsigned char d) {
  Addr out(10, 0);
  out.push_back(0xff);
  out.push_back(0xff);
  out.insert(out.end(), {a, b, c, d});
  return out;
}

const Addr kNone;
constexpr char kWorkerUa[] =
    "Mozilla/5.0 (compatible; mod_pagespeed agent_optimize)";
constexpr char kBrowserUa[] =
    "Mozilla/5.0 (X11; Linux x86_64) AppleWebKit/537.36 Chrome/130.0 Safari";

TEST(AgentSelfFetch, MarkerMatchesTheWorkerUserAgentOnly) {
  EXPECT_TRUE(IsAgentFetchUserAgent(kWorkerUa));
  EXPECT_TRUE(IsAgentFetchUserAgent("x mod_pagespeed agent_optimize y"));
  EXPECT_FALSE(IsAgentFetchUserAgent(kBrowserUa));
  EXPECT_FALSE(IsAgentFetchUserAgent(""));
  EXPECT_FALSE(IsAgentFetchUserAgent("mod_pagespeed"));
  EXPECT_FALSE(IsAgentFetchUserAgent("MOD_PAGESPEED AGENT_OPTIMIZE"));
}

TEST(AgentSelfFetch, TheWorkerFetcherStillSendsTheMarker) {
  pagespeed::CurlFetchOptions opts;
  EXPECT_TRUE(IsAgentFetchUserAgent(opts.user_agent)) << opts.user_agent;
  EXPECT_EQ(std::string(kWorkerUa), opts.user_agent);
}

TEST(AgentSelfFetch, LoopbackPeersAreThisServer) {
  const Addr local = V4(192, 0, 2, 20);
  EXPECT_TRUE(AgentFetchPeerIsThisServer(V4(127, 0, 0, 1), local));
  EXPECT_TRUE(AgentFetchPeerIsThisServer(V4(127, 9, 8, 7), local));
  EXPECT_TRUE(AgentFetchPeerIsThisServer(V4(127, 0, 0, 1), kNone));
  EXPECT_TRUE(AgentFetchPeerIsThisServer(V6({0, 0, 0, 0, 0, 0, 0, 1}), kNone));
  EXPECT_TRUE(AgentFetchPeerIsThisServer(Mapped(127, 0, 0, 1), kNone));
  EXPECT_TRUE(AgentFetchPeerIsThisServer(Mapped(127, 3, 2, 1), local));
}

TEST(AgentSelfFetch, PeerOnTheArrivalAddressIsThisServer) {
  EXPECT_TRUE(AgentFetchPeerIsThisServer(V4(192, 0, 2, 20), V4(192, 0, 2, 20)));
  const Addr v6 = V6({0x2001, 0xdb8, 0, 0, 0, 0, 0, 0x20});
  EXPECT_TRUE(AgentFetchPeerIsThisServer(v6, v6));
  // A dual-stack listener reports both ends IPv4-mapped; either form of the
  // same IPv4 address is the same address.
  EXPECT_TRUE(
      AgentFetchPeerIsThisServer(Mapped(192, 0, 2, 20), Mapped(192, 0, 2, 20)));
  EXPECT_TRUE(
      AgentFetchPeerIsThisServer(Mapped(192, 0, 2, 20), V4(192, 0, 2, 20)));
  EXPECT_TRUE(
      AgentFetchPeerIsThisServer(V4(192, 0, 2, 20), Mapped(192, 0, 2, 20)));
}

TEST(AgentSelfFetch, OtherPeersAreNotThisServer) {
  const Addr local = V4(192, 0, 2, 20);
  EXPECT_FALSE(AgentFetchPeerIsThisServer(V4(192, 0, 2, 21), local));
  EXPECT_FALSE(AgentFetchPeerIsThisServer(V4(198, 51, 100, 9), local));
  EXPECT_FALSE(AgentFetchPeerIsThisServer(V4(198, 51, 100, 9), kNone));
  EXPECT_FALSE(AgentFetchPeerIsThisServer(Mapped(198, 51, 100, 9), local));
  EXPECT_FALSE(
      AgentFetchPeerIsThisServer(V6({0x2001, 0xdb8, 0, 0, 0, 0, 0, 0x21}),
                                 V6({0x2001, 0xdb8, 0, 0, 0, 0, 0, 0x20})));
  // Not loopback: the unspecified address, ::2, and an IPv4-compatible
  // (not mapped) form of 127.0.0.1.
  EXPECT_FALSE(AgentFetchPeerIsThisServer(V6({0, 0, 0, 0, 0, 0, 0, 0}), local));
  EXPECT_FALSE(AgentFetchPeerIsThisServer(V6({0, 0, 0, 0, 0, 0, 0, 2}), local));
  EXPECT_FALSE(
      AgentFetchPeerIsThisServer(V6({0, 0, 0, 0, 0, 0, 0x7f00, 1}), local));
  // An IPv6 peer whose low bytes equal the IPv4 local address is not it.
  EXPECT_FALSE(AgentFetchPeerIsThisServer(
      V6({0x2001, 0xdb8, 0, 0, 0, 0, 0xc000, 0x0214}), local));
  // No address, or a malformed one, is never this server.
  EXPECT_FALSE(AgentFetchPeerIsThisServer(kNone, local));
  EXPECT_FALSE(AgentFetchPeerIsThisServer(Addr{127, 0, 0}, local));
  EXPECT_FALSE(AgentFetchPeerIsThisServer(Addr{192, 0, 2}, Addr{192, 0, 2}));
}

TEST(AgentSelfFetch, NeedsBothTheMarkerAndAPeerThatIsThisServer) {
  const Addr local = V4(192, 0, 2, 20);
  EXPECT_TRUE(IsAgentSelfFetch(kWorkerUa, V4(127, 0, 0, 1), local));
  EXPECT_TRUE(IsAgentSelfFetch(kWorkerUa, V4(192, 0, 2, 20), local));
  EXPECT_TRUE(IsAgentSelfFetch(kWorkerUa, V6({0, 0, 0, 0, 0, 0, 0, 1}), kNone));
  // A local client without the marker keeps its address.
  EXPECT_FALSE(IsAgentSelfFetch(kBrowserUa, V4(127, 0, 0, 1), local));
  EXPECT_FALSE(IsAgentSelfFetch("", V4(192, 0, 2, 20), local));
  // A remote client copying the marker keeps its own address.
  EXPECT_FALSE(IsAgentSelfFetch(kWorkerUa, V4(198, 51, 100, 9), local));
  EXPECT_FALSE(IsAgentSelfFetch(kWorkerUa, Mapped(198, 51, 100, 9),
                                Mapped(192, 0, 2, 20)));
  EXPECT_FALSE(IsAgentSelfFetch(kWorkerUa,
                                V6({0x2001, 0xdb8, 0, 0, 0, 0, 0, 0x99}),
                                V6({0x2001, 0xdb8, 0, 0, 0, 0, 0, 0x20})));
}

TEST(AgentSelfFetch, TheLabelIsNeitherLoopbackNorAnOwnAddress) {
  EXPECT_EQ("224.0.0.0", kAgentSelfFetchLabel);
  const Addr label(std::begin(kAgentSelfFetchLabelBytes),
                   std::end(kAgentSelfFetchLabelBytes));
  EXPECT_EQ(V4(224, 0, 0, 0), label);
  EXPECT_FALSE(AgentFetchPeerIsThisServer(label, V4(127, 0, 0, 1)));
  EXPECT_FALSE(AgentFetchPeerIsThisServer(label, V4(192, 0, 2, 20)));
  EXPECT_FALSE(IsAgentSelfFetch(kWorkerUa, label, V4(192, 0, 2, 20)));
  EXPECT_EQ(std::string_view("mod_pagespeed agent_optimize"),
            kAgentFetchUserAgentMarker);
}

}  // namespace
