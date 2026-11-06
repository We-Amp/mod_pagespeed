// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 We-Amp B.V.

#include "pagespeed/iis/iis_url_port.h"

#include "pagespeed/kernel/base/string.h"
#include "pagespeed/kernel/http/google_url.h"
#include "test/pagespeed/kernel/base/gtest.h"

namespace net_instaweb {
namespace {

TEST(IisUrlPortTest, NameAndIpv4HostsLoseThePort) {
  EXPECT_EQ("http://localhost/a.html",
            IisUrlWithoutPort("http://localhost:80/a.html"));
  EXPECT_EQ("https://example.com/",
            IisUrlWithoutPort("https://example.com:443/"));
  EXPECT_EQ("http://10.0.0.5/x?y=1",
            IisUrlWithoutPort("http://10.0.0.5:80/x?y=1"));
}

// A bracketed IPv6 host keeps its own colons; only the port goes.
TEST(IisUrlPortTest, Ipv6HostsKeepTheirAddress) {
  EXPECT_EQ("http://[::1]/", IisUrlWithoutPort("http://[::1]:80/"));
  EXPECT_EQ(
      "http://[::1]/pagespeed_admin/rule_packs?json=1",
      IisUrlWithoutPort("http://[::1]:80/pagespeed_admin/rule_packs?json=1"));
  EXPECT_EQ("https://[2001:db8::1]/a.css",
            IisUrlWithoutPort("https://[2001:db8::1]:443/a.css"));
  EXPECT_EQ("http://[fd00:6c61:62::9]/oc/a.html",
            IisUrlWithoutPort("http://[fd00:6c61:62::9]:80/oc/a.html"));
  EXPECT_EQ("http://[::ffff:10.0.0.5]/",
            IisUrlWithoutPort("http://[::ffff:10.0.0.5]:80/"));
}

TEST(IisUrlPortTest, TheResultIsAValidUrlNamingTheSameHost) {
  for (const char* in : {"http://[::1]:80/", "http://[2001:db8::1]:80/p",
                         "http://[fe80::1]:80/q?r=s:t"}) {
    GoogleUrl before(in);
    GoogleUrl after(IisUrlWithoutPort(in));
    ASSERT_TRUE(before.IsWebValid()) << in;
    EXPECT_TRUE(after.IsWebValid()) << in;
    EXPECT_EQ(before.Host(), after.Host()) << in;
    EXPECT_EQ(before.PathAndLeaf(), after.PathAndLeaf()) << in;
  }
}

TEST(IisUrlPortTest, UrlsWithoutAPortAreUnchanged) {
  EXPECT_EQ("http://localhost/a.html",
            IisUrlWithoutPort("http://localhost/a.html"));
  EXPECT_EQ("http://[::1]/a.html", IisUrlWithoutPort("http://[::1]/a.html"));
  EXPECT_EQ("http://[2001:db8::1]", IisUrlWithoutPort("http://[2001:db8::1]"));
  EXPECT_EQ("not a url", IisUrlWithoutPort("not a url"));
}

TEST(IisUrlPortTest, ColonsAfterTheAuthorityAreLeftAlone) {
  EXPECT_EQ("http://host/a:b?c=d:e#f:g",
            IisUrlWithoutPort("http://host:80/a:b?c=d:e#f:g"));
  EXPECT_EQ("http://host/a:b", IisUrlWithoutPort("http://host/a:b"));
  EXPECT_EQ("http://[::1]/a:b", IisUrlWithoutPort("http://[::1]:80/a:b"));
  EXPECT_EQ("http://host?q=1:2", IisUrlWithoutPort("http://host:8080?q=1:2"));
}

TEST(IisUrlPortTest, UserinfoIsNotTakenForTheHost) {
  EXPECT_EQ("http://u:p@host/", IisUrlWithoutPort("http://u:p@host:80/"));
  EXPECT_EQ("http://u:p@[::1]/", IisUrlWithoutPort("http://u:p@[::1]:80/"));
}

// A zone id is a "%25" escape inside the brackets. The URL parser does not
// accept zone ids at all, so the port removal must neither invent nor lose
// validity: the result is valid exactly when the input was.
TEST(IisUrlPortTest, ZoneIdHostsKeepTheirZone) {
  EXPECT_EQ("http://[fe80::1%25eth0]/",
            IisUrlWithoutPort("http://[fe80::1%25eth0]:80/"));
  GoogleUrl before("http://[fe80::1%25eth0]:80/");
  GoogleUrl after(IisUrlWithoutPort("http://[fe80::1%25eth0]:80/"));
  EXPECT_EQ(before.IsWebValid(), after.IsWebValid());
}

// An empty port ("host:") is the default port; the colon goes with it.
TEST(IisUrlPortTest, EmptyPortLosesTheColon) {
  EXPECT_EQ("http://host/x", IisUrlWithoutPort("http://host:/x"));
  EXPECT_EQ("http://[::1]/x", IisUrlWithoutPort("http://[::1]:/x"));
  EXPECT_TRUE(GoogleUrl(IisUrlWithoutPort("http://host:/x")).IsWebValid());
  EXPECT_TRUE(GoogleUrl(IisUrlWithoutPort("http://[::1]:/x")).IsWebValid());
}

TEST(IisUrlPortTest, EmptyAuthorityIsLeftUnchanged) {
  EXPECT_EQ("http:///x", IisUrlWithoutPort("http:///x"));
  EXPECT_EQ("http://", IisUrlWithoutPort("http://"));
  EXPECT_EQ("http://?q=1:2", IisUrlWithoutPort("http://?q=1:2"));
}

TEST(IisUrlPortTest, AnUnclosedBracketIsLeftUnchanged) {
  EXPECT_EQ("http://[::1:80/", IisUrlWithoutPort("http://[::1:80/"));
  // Whatever comes back must not be accepted as a valid URL either.
  EXPECT_FALSE(GoogleUrl(IisUrlWithoutPort("http://[::1:80/")).IsWebValid());
}

}  // namespace
}  // namespace net_instaweb
