// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

#include "lib/classify/hostname.h"

#include "gtest/gtest.h"

namespace pagespeed {
namespace {

TEST(HostnameTest, Lowercase) {
  EXPECT_EQ(NormalizeHostname("Example.COM"), "example.com");
  EXPECT_EQ(NormalizeHostname("WWW.EXAMPLE.COM"), "www.example.com");
}

TEST(HostnameTest, StripDefaultPort80) {
  EXPECT_EQ(NormalizeHostname("example.com:80"), "example.com");
}

TEST(HostnameTest, StripDefaultPort443) {
  EXPECT_EQ(NormalizeHostname("example.com:443"), "example.com");
}

TEST(HostnameTest, StripTrailingDot) {
  EXPECT_EQ(NormalizeHostname("example.com."), "example.com");
}

TEST(HostnameTest, PreserveNonDefaultPort) {
  EXPECT_EQ(NormalizeHostname("example.com:8080"), "example.com:8080");
  EXPECT_EQ(NormalizeHostname("example.com:3000"), "example.com:3000");
}

TEST(HostnameTest, EmptyString) { EXPECT_EQ(NormalizeHostname(""), ""); }

TEST(HostnameTest, CombinedNormalization) {
  EXPECT_EQ(NormalizeHostname("Example.COM.:80"), "example.com");
  EXPECT_EQ(NormalizeHostname("WWW.Example.COM.:443"), "www.example.com");
}

TEST(HostnameTest, AlreadyNormalized) {
  EXPECT_EQ(NormalizeHostname("example.com"), "example.com");
}

TEST(HostnameTest, PortOnly) {
  // Edge case: just a colon
  EXPECT_EQ(NormalizeHostname(":80"), "");
}

TEST(HostnameTest, IPv6WithPort) {
  EXPECT_EQ(NormalizeHostname("[::1]:80"), "[::1]");
  EXPECT_EQ(NormalizeHostname("[::1]:8080"), "[::1]:8080");
}

TEST(HostnameTest, IPv6WithoutPort) {
  EXPECT_EQ(NormalizeHostname("[::1]"), "[::1]");
}

TEST(HostnameTest, TrailingDotWithNonDefaultPort) {
  EXPECT_EQ(NormalizeHostname("example.com.:8080"), "example.com:8080");
}

}  // namespace
}  // namespace pagespeed
