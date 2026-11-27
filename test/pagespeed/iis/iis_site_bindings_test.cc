// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 We-Amp B.V.

#include "pagespeed/iis/iis_site_bindings.h"

#include "pagespeed/system/serve_host_names.h"
#include "test/pagespeed/kernel/base/gtest.h"

namespace net_instaweb {
namespace {

GoogleString Host(StringPiece protocol, StringPiece info) {
  GoogleString host = "unset";
  if (!IisBindingHostName(protocol, info, &host)) return "<none>";
  return host;
}

TEST(IisSiteBindingsTest, ANamedBindingGivesItsHost) {
  EXPECT_EQ("probe-a.test", Host("http", "*:8201:probe-a.test"));
  EXPECT_EQ("www.example.com", Host("https", "*:443:www.example.com"));
  EXPECT_EQ("example.com", Host("HTTP", "10.0.0.1:80:example.com"));
  EXPECT_EQ("example.com", Host("http", "[2001:db8::1]:80:example.com"));
}

TEST(IisSiteBindingsTest, AnEmptyHostNeverVouches) {
  // Any host reaches such a binding, so it names none.
  EXPECT_EQ("<none>", Host("http", "*:8201:"));
  EXPECT_EQ("<none>", Host("http", "127.0.0.1:8205:"));
  EXPECT_EQ("<none>", Host("http", "[::1]:80:"));
}

TEST(IisSiteBindingsTest, OnlyWebBindingsWithWellFormedInformationCount) {
  EXPECT_EQ("<none>", Host("net.tcp", "808:*"));
  EXPECT_EQ("<none>", Host("ftp", "*:21:ftp.example.com"));
  EXPECT_EQ("<none>", Host("http", "*:80"));
  EXPECT_EQ("<none>", Host("http", "*:port:example.com"));
  EXPECT_EQ("<none>", Host("http", "*::example.com"));
  EXPECT_EQ("<none>", Host("http", "*:123456:example.com"));
  EXPECT_EQ("<none>", Host("http", "[2001:db8::1:80:example.com"));
  EXPECT_EQ("<none>", Host("http", ""));
}

TEST(IisSiteBindingsTest, AWildcardOrOddHostNeverVouches) {
  // IIS 10 accepts a wildcard host header binding; it names no one host.
  EXPECT_EQ("<none>", Host("http", "*:80:*.example.com"));
  EXPECT_EQ("<none>", Host("http", "*:80:exa mple.com"));
  EXPECT_EQ("<none>", Host("http", "*:80:example.com:81"));
}

TEST(IisSiteBindingsTest, TheSiteNamesAreItsNamedBindingsOnceEach) {
  const ConfiguredHostNames names = IisSiteHostNames({
      {"http", "*:8201:"},
      {"http", "*:8201:probe-a.test"},
      {"https", "*:8443:probe-a.test"},
      {"http", "*:8202:probe-c.test"},
      {"http", "127.0.0.1:8205:"},
  });
  EXPECT_EQ("probe-a.test", names.primary);
  ASSERT_EQ(1u, names.aliases.size());
  EXPECT_EQ("probe-c.test", names.aliases[0]);

  EXPECT_TRUE(HostExactlyConfigured("probe-a.test:8201", names));
  EXPECT_TRUE(HostExactlyConfigured("PROBE-C.test", names));
  EXPECT_FALSE(HostExactlyConfigured("evil.test:8201", names));
  EXPECT_FALSE(HostExactlyConfigured("127.0.0.1:8205", names));
}

TEST(IisSiteBindingsTest, ASiteWithOnlyEmptyHostsNamesNoOne) {
  const ConfiguredHostNames names =
      IisSiteHostNames({{"http", "*:8203:"}, {"http", "127.0.0.1:8205:"}});
  EXPECT_TRUE(names.primary.empty());
  EXPECT_TRUE(names.aliases.empty());
  EXPECT_FALSE(HostExactlyConfigured("localhost:8203", names));
  EXPECT_FALSE(HostExactlyConfigured("127.0.0.1:8205", names));
}

}  // namespace
}  // namespace net_instaweb
