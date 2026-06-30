// Copyright (c) 2024-2026 We-Amp B.V.

// the design record unit tests for the PSL-free over-cap host matchers. These mirror the
// ModPageSpeed 2.0 worker tests (OverCapFlagsSiteScopeOnOffLicenseDomain,
// OverCapExemptsNonSiteScopes via IsExternalFqdn, and
// OverCapSuffixSubstringIsNotASubdomain) at the matcher level — the
// SystemServerContext latch/policy plumbing is exercised separately.

#include "pagespeed/system/over_cap_match.h"

#include "test/pagespeed/kernel/base/gtest.h"

namespace net_instaweb {
namespace {

TEST(OverCapMatchTest, IsExternalFqdnAcceptsRealHosts) {
  EXPECT_TRUE(IsExternalFqdn("example.com"));
  EXPECT_TRUE(IsExternalFqdn("www.example.com"));
  EXPECT_TRUE(IsExternalFqdn("a.b.c.example.co.uk"));
}

TEST(OverCapMatchTest, IsExternalFqdnRejectsNonRoutable) {
  EXPECT_FALSE(IsExternalFqdn(""));
  EXPECT_FALSE(IsExternalFqdn("localhost"));
  EXPECT_FALSE(IsExternalFqdn("intranet"));        // single-label internal host
  EXPECT_FALSE(IsExternalFqdn("192.168.1.10"));    // dotted-quad IPv4 literal
  EXPECT_FALSE(IsExternalFqdn("dev.localhost"));   // .localhost suffix
  EXPECT_FALSE(IsExternalFqdn("example.com:8080"));  // embedded port/colon
  EXPECT_FALSE(IsExternalFqdn("[::1]"));           // IPv6 literal (colon)
  EXPECT_FALSE(IsExternalFqdn(".example.com"));    // leading dot
  EXPECT_FALSE(IsExternalFqdn("example.com."));    // trailing dot
}

TEST(OverCapMatchTest, MatchesExactAndSubdomains) {
  EXPECT_TRUE(HostnameMatchesLicensedDomain("example.com", "example.com"));
  EXPECT_TRUE(HostnameMatchesLicensedDomain("www.example.com", "example.com"));
  EXPECT_TRUE(
      HostnameMatchesLicensedDomain("a.b.example.com", "example.com"));
}

TEST(OverCapMatchTest, DoesNotMatchOtherDomains) {
  // The crux: a registrable domain that merely ends with the licensed string
  // is NOT a subdomain — suffix-with-dot guards against badexample.com.
  EXPECT_FALSE(HostnameMatchesLicensedDomain("badexample.com", "example.com"));
  EXPECT_FALSE(HostnameMatchesLicensedDomain("notexample.com", "example.com"));
  EXPECT_FALSE(
      HostnameMatchesLicensedDomain("example.com.evil.com", "example.com"));
  EXPECT_FALSE(HostnameMatchesLicensedDomain("other.org", "example.com"));
}

TEST(OverCapMatchTest, EmptyLicensedNeverMatches) {
  EXPECT_FALSE(HostnameMatchesLicensedDomain("example.com", ""));
  EXPECT_FALSE(HostnameMatchesLicensedDomain("", ""));
}

}  // namespace
}  // namespace net_instaweb
