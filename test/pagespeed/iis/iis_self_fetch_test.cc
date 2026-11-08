// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 We-Amp B.V.

#include "pagespeed/iis/iis_self_fetch.h"

#include <cstring>

#include "pagespeed/iis/iis_ip_string.h"
#include "test/pagespeed/kernel/base/gtest.h"

namespace net_instaweb {
namespace {

// The User-Agent the module's WinHTTP fetcher sends (asyncwinhttp.cpp).
const char kFetchUa[] = "WinHttp mod_pagespeed ";
const char kBrowserUa[] = "Mozilla/5.0 (Windows NT 10.0; Win64; x64)";

// A socket address from its text form; IPv4 when it parses as one.
struct Addr {
  explicit Addr(const char* text) {
    memset(&storage, 0, sizeof(storage));
    SOCKADDR_IN* v4 = reinterpret_cast<SOCKADDR_IN*>(&storage);
    SOCKADDR_IN6* v6 = reinterpret_cast<SOCKADDR_IN6*>(&storage);
    if (inet_pton(AF_INET, text, &v4->sin_addr) == 1) {
      v4->sin_family = AF_INET;
      v4->sin_port = htons(8091);
    } else {
      EXPECT_EQ(1, inet_pton(AF_INET6, text, &v6->sin6_addr)) << text;
      v6->sin6_family = AF_INET6;
      v6->sin6_port = htons(8091);
    }
  }
  SOCKADDR* sa() { return reinterpret_cast<SOCKADDR*>(&storage); }
  SOCKADDR_STORAGE storage;
};

bool ThisServer(const char* peer, const char* local) {
  Addr p(peer), l(local);
  return IisSelfFetchPeerIsThisServer(p.sa(), l.sa());
}

TEST(IisSelfFetchTest, LoopbackPeerIsThisServer) {
  EXPECT_TRUE(ThisServer("127.0.0.1", "10.0.0.5"));
  EXPECT_TRUE(ThisServer("127.1.2.3", "10.0.0.5"));
  EXPECT_TRUE(ThisServer("::1", "fd00::5"));
  EXPECT_TRUE(ThisServer("::ffff:127.0.0.1", "10.0.0.5"));
  EXPECT_TRUE(ThisServer("::ffff:127.0.0.1", "::ffff:10.0.0.5"));
}

TEST(IisSelfFetchTest, PeerEqualToArrivalAddressIsThisServer) {
  EXPECT_TRUE(ThisServer("10.0.0.5", "10.0.0.5"));
  EXPECT_TRUE(ThisServer("fd00::5", "fd00::5"));
  // Mapped and plain forms of one IPv4 address are the same address.
  EXPECT_TRUE(ThisServer("::ffff:10.0.0.5", "10.0.0.5"));
  EXPECT_TRUE(ThisServer("10.0.0.5", "::ffff:10.0.0.5"));
  EXPECT_TRUE(ThisServer("::ffff:10.0.0.5", "::ffff:10.0.0.5"));
}

TEST(IisSelfFetchTest, OtherPeersAreNotThisServer) {
  EXPECT_FALSE(ThisServer("10.0.0.6", "10.0.0.5"));
  EXPECT_FALSE(ThisServer("203.0.113.9", "10.0.0.5"));
  EXPECT_FALSE(ThisServer("fd00::6", "fd00::5"));
  EXPECT_FALSE(ThisServer("::ffff:10.0.0.6", "10.0.0.5"));
  // ::2 and ::127.0.0.1 (IPv4-compatible) are not loopback.
  EXPECT_FALSE(ThisServer("::2", "fd00::5"));
  EXPECT_FALSE(ThisServer("::127.0.0.1", "fd00::5"));
  // An IPv6 address whose low bytes match an IPv4 local is not it.
  EXPECT_FALSE(ThisServer("fd00::a00:5", "10.0.0.5"));
  EXPECT_FALSE(ThisServer("224.0.0.0", "10.0.0.5"));
}

TEST(IisSelfFetchTest, NullAddresses) {
  Addr loop("127.0.0.1"), own("10.0.0.5");
  EXPECT_FALSE(IisSelfFetchPeerIsThisServer(nullptr, own.sa()));
  EXPECT_FALSE(IisSelfFetchPeerIsThisServer(nullptr, nullptr));
  EXPECT_TRUE(IisSelfFetchPeerIsThisServer(loop.sa(), nullptr));
  EXPECT_FALSE(IisSelfFetchPeerIsThisServer(own.sa(), nullptr));
}

TEST(IisSelfFetchTest, SelfFetchNeedsMarkerAndThisServer) {
  Addr loop4("127.0.0.1"), loop6("::1"), mapped("::ffff:127.0.0.1");
  Addr own("10.0.0.5");
  EXPECT_TRUE(IisIsSelfFetch(kFetchUa, loop4.sa(), own.sa()));
  EXPECT_TRUE(IisIsSelfFetch(kFetchUa, loop6.sa(), own.sa()));
  EXPECT_TRUE(IisIsSelfFetch(kFetchUa, mapped.sa(), own.sa()));
  EXPECT_TRUE(IisIsSelfFetch(kFetchUa, own.sa(), own.sa()));
  // The marker anywhere in the User-Agent, as other fetchers send it.
  EXPECT_TRUE(
      IisIsSelfFetch("Serf/1.1 mod_pagespeed/1.1", loop4.sa(), own.sa()));
}

TEST(IisSelfFetchTest, RemoteClientCopyingTheMarkerIsNotASelfFetch) {
  Addr remote("203.0.113.9"), remote6("2001:db8::9"), own("10.0.0.5");
  EXPECT_FALSE(IisIsSelfFetch(kFetchUa, remote.sa(), own.sa()));
  EXPECT_FALSE(IisIsSelfFetch(kFetchUa, remote6.sa(), own.sa()));
}

TEST(IisSelfFetchTest, LocalClientWithoutTheMarkerIsNotASelfFetch) {
  Addr loop("127.0.0.1"), own("10.0.0.5");
  EXPECT_FALSE(IisIsSelfFetch(kBrowserUa, loop.sa(), own.sa()));
  EXPECT_FALSE(IisIsSelfFetch(kBrowserUa, own.sa(), own.sa()));
  EXPECT_FALSE(IisIsSelfFetch("", loop.sa(), own.sa()));
}

TEST(IisSelfFetchTest, RelabelWritesTheReservedIpv4Address) {
  for (const char* text :
       {"127.0.0.1", "10.0.0.5", "::1", "::ffff:127.0.0.1", "fd00::5"}) {
    Addr a(text);
    ASSERT_TRUE(IisRelabelSelfFetch(a.sa())) << text;
    EXPECT_EQ(AF_INET, a.sa()->sa_family) << text;
    EXPECT_EQ(kIisSelfFetchClientIp, GetIPString(a.sa())) << text;
    EXPECT_EQ(0, reinterpret_cast<SOCKADDR_IN*>(a.sa())->sin_port) << text;
    // The relabeled address is no longer this server.
    Addr own("10.0.0.5");
    EXPECT_FALSE(IisSelfFetchPeerIsThisServer(a.sa(), own.sa())) << text;
  }
}

TEST(IisSelfFetchTest, RelabelLeavesUnknownFamiliesAlone) {
  SOCKADDR_STORAGE s;
  memset(&s, 0x5a, sizeof(s));
  SOCKADDR* sa = reinterpret_cast<SOCKADDR*>(&s);
  sa->sa_family = AF_UNSPEC;
  EXPECT_FALSE(IisRelabelSelfFetch(sa));
  EXPECT_EQ(AF_UNSPEC, sa->sa_family);
  EXPECT_FALSE(IisRelabelSelfFetch(nullptr));
}

}  // namespace
}  // namespace net_instaweb
