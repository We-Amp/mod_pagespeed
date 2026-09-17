/*
 * Licensed to the Apache Software Foundation (ASF) under one
 * or more contributor license agreements.  See the NOTICE file
 * distributed with this work for additional information
 * regarding copyright ownership.  The ASF licenses this file
 * to you under the Apache License, Version 2.0 (the
 * "License"); you may not use this file except in compliance
 * with the License.  You may obtain a copy of the License at
 *
 *   http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing,
 * software distributed under the License is distributed on an
 * "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
 * KIND, either express or implied.  See the License for the
 * specific language governing permissions and limitations
 * under the License.
 */

// Unit tests for GetIPString(SOCKADDR*).  The headline case is IPv6: the
// address must be reinterpreted as SOCKADDR_IN6 so sin6_addr is read at the
// correct offset (the previous PSOCKADDR_IN signature mis-decoded v6 clients).

#include <cstring>

#include "gtest/gtest.h"
// iis_ip_string.h pulls in <winsock2.h> + <ws2tcpip.h> (inet_pton/inet_ntop) in
// the correct order, so it is the only socket header this test needs.
#include "pagespeed/iis/iis_ip_string.h"

namespace net_instaweb {
namespace {

SOCKADDR_IN MakeV4(const char* ip) {
  SOCKADDR_IN addr;
  std::memset(&addr, 0, sizeof(addr));
  addr.sin_family = AF_INET;
  EXPECT_EQ(1, inet_pton(AF_INET, ip, &addr.sin_addr));
  return addr;
}

SOCKADDR_IN6 MakeV6(const char* ip) {
  SOCKADDR_IN6 addr;
  std::memset(&addr, 0, sizeof(addr));
  addr.sin6_family = AF_INET6;
  EXPECT_EQ(1, inet_pton(AF_INET6, ip, &addr.sin6_addr));
  return addr;
}

TEST(IisIpStringTest, NullReturnsEmpty) { EXPECT_EQ("", GetIPString(nullptr)); }

TEST(IisIpStringTest, Ipv4) {
  SOCKADDR_IN addr = MakeV4("192.0.2.1");
  EXPECT_EQ("192.0.2.1", GetIPString(reinterpret_cast<SOCKADDR*>(&addr)));
}

TEST(IisIpStringTest, Ipv4Loopback) {
  SOCKADDR_IN addr = MakeV4("127.0.0.1");
  EXPECT_EQ("127.0.0.1", GetIPString(reinterpret_cast<SOCKADDR*>(&addr)));
}

TEST(IisIpStringTest, Ipv4UnspecifiedIsEmpty) {
  SOCKADDR_IN addr = MakeV4("0.0.0.0");
  EXPECT_EQ("", GetIPString(reinterpret_cast<SOCKADDR*>(&addr)));
}

// Without the SOCKADDR_IN6 reinterpret these would decode to garbage / the
// wrong bytes; assert the canonical compressed forms.
TEST(IisIpStringTest, Ipv6) {
  SOCKADDR_IN6 addr = MakeV6("2001:db8::1");
  EXPECT_EQ("2001:db8::1", GetIPString(reinterpret_cast<SOCKADDR*>(&addr)));
}

TEST(IisIpStringTest, Ipv6Loopback) {
  SOCKADDR_IN6 addr = MakeV6("::1");
  EXPECT_EQ("::1", GetIPString(reinterpret_cast<SOCKADDR*>(&addr)));
}

TEST(IisIpStringTest, Ipv6UnspecifiedIsEmpty) {
  SOCKADDR_IN6 addr = MakeV6("::");
  EXPECT_EQ("", GetIPString(reinterpret_cast<SOCKADDR*>(&addr)));
}

TEST(IisIpStringTest, Ipv4MappedIpv6IsNonEmpty) {
  SOCKADDR_IN6 addr = MakeV6("::ffff:192.0.2.1");
  // The exact textual form of a v4-mapped address is platform-dependent; just
  // require that a non-empty (non-unspecified) address decodes successfully.
  EXPECT_NE("", GetIPString(reinterpret_cast<SOCKADDR*>(&addr)));
}

}  // namespace
}  // namespace net_instaweb
