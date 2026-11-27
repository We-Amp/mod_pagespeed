// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

// Tests for the server-name and alias matching that decides which request
// hostnames a server block stands behind (src/nginx/host_vouch.h).

#include "src/nginx/host_vouch.h"

#include <string>
#include <unordered_map>

#include "gtest/gtest.h"

using pagespeed::AuthorityHost;
using pagespeed::HostAliasesName;
using pagespeed::ServerNameMatchesHost;

namespace {

TEST(HostVouch, AuthorityHostDropsPortCaseAndTrailingDot) {
  EXPECT_EQ("example.com", AuthorityHost("Example.COM"));
  EXPECT_EQ("example.com", AuthorityHost("example.com:8080"));
  EXPECT_EQ("example.com", AuthorityHost("example.com.:443"));
  EXPECT_EQ("[::1]", AuthorityHost("[::1]:8080"));
  EXPECT_EQ("[::1]", AuthorityHost("[::1]"));
  EXPECT_EQ("10.0.0.5", AuthorityHost("10.0.0.5:80"));
  EXPECT_EQ("", AuthorityHost(""));
  EXPECT_EQ("", AuthorityHost("[::1"));
}

TEST(HostVouch, ExactNameMatchesOnlyThatName) {
  EXPECT_TRUE(ServerNameMatchesHost("example.com", "example.com"));
  EXPECT_TRUE(ServerNameMatchesHost("Example.com", "example.com"));
  EXPECT_FALSE(ServerNameMatchesHost("example.com", "www.example.com"));
  EXPECT_FALSE(ServerNameMatchesHost("example.com", "example.com.evil"));
  EXPECT_FALSE(ServerNameMatchesHost("example.com", "10.0.0.5"));
}

TEST(HostVouch, LeadingWildcardNeedsALabelBelowTheDomain) {
  EXPECT_TRUE(ServerNameMatchesHost("*.example.com", "www.example.com"));
  EXPECT_TRUE(ServerNameMatchesHost("*.example.com", "a.b.example.com"));
  EXPECT_FALSE(ServerNameMatchesHost("*.example.com", "example.com"));
  EXPECT_FALSE(ServerNameMatchesHost("*.example.com", "badexample.com"));
  EXPECT_FALSE(ServerNameMatchesHost("*.example.com", "example.com.other"));
}

TEST(HostVouch, DotWildcardAlsoNamesTheDomainItself) {
  EXPECT_TRUE(ServerNameMatchesHost(".example.com", "example.com"));
  EXPECT_TRUE(ServerNameMatchesHost(".example.com", "www.example.com"));
  EXPECT_FALSE(ServerNameMatchesHost(".example.com", "badexample.com"));
}

TEST(HostVouch, TrailingWildcardNeedsALabelAfterThePrefix) {
  EXPECT_TRUE(ServerNameMatchesHost("www.example.*", "www.example.org"));
  EXPECT_TRUE(ServerNameMatchesHost("www.example.*", "www.example.co.uk"));
  EXPECT_FALSE(ServerNameMatchesHost("www.example.*", "www.example."));
  EXPECT_FALSE(ServerNameMatchesHost("www.example.*", "www.examples.org"));
}

TEST(HostVouch, CatchAllNamesVouchForNothing) {
  EXPECT_FALSE(ServerNameMatchesHost("", "example.com"));
  EXPECT_FALSE(ServerNameMatchesHost("_", "example.com"));
  EXPECT_FALSE(ServerNameMatchesHost("example.com", ""));
}

TEST(HostVouch, AliasTableNamesBothSides) {
  const std::unordered_map<std::string, std::string> aliases = {
      {"www.example.com", "example.com"}, {"cdn.example.net:8080", "cdn"}};
  EXPECT_TRUE(HostAliasesName(aliases, "www.example.com"));
  EXPECT_TRUE(HostAliasesName(aliases, "example.com"));
  EXPECT_TRUE(HostAliasesName(aliases, "cdn.example.net:8080"));
  EXPECT_FALSE(HostAliasesName(aliases, "cdn.example.net"));
  EXPECT_FALSE(HostAliasesName(aliases, "other.example.com"));
  EXPECT_FALSE(HostAliasesName(aliases, ""));
  EXPECT_FALSE(HostAliasesName({}, "example.com"));
}

}  // namespace
