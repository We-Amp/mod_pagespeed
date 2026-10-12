// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

#include "pagespeed/kernel/packs/url_norm.h"

#include "gtest/gtest.h"

// The normalization cases below follow the URL equality rules of an
// SEO-defects checker: case, default port, fragment, single trailing slash,
// query kept.
namespace pagespeed::packs {
namespace {

std::string N(std::string_view u) { return NormUrl(u).value_or("<invalid>"); }

TEST(NormUrlTest, ReferenceCases) {
  EXPECT_EQ(N("HTTPS://Www.Example.COM:443/De/#top"),
            "https://www.example.com/De");
  EXPECT_EQ(N("http://example.com:80/a/"), "http://example.com/a");
  EXPECT_EQ(N("https://example.com/a?b=1&c=2"),
            "https://example.com/a?b=1&c=2");
  EXPECT_EQ(N("https://example.com/a/?b=1"), "https://example.com/a?b=1");
  EXPECT_EQ(N("https://example.com/a//"), "https://example.com/a/");
  EXPECT_EQ(N("https://example.com"), N("https://example.com/"));
  EXPECT_NE(N("https://example.com/A"), N("https://example.com/a"));
  EXPECT_EQ(N("https://example.com:8443/a"), "https://example.com:8443/a");
  EXPECT_FALSE(NormUrl("not a url").has_value());
  EXPECT_FALSE(NormUrl("javascript:alert(1)").has_value());
  EXPECT_FALSE(NormUrl("").has_value());
  // The scanner also resolves "/rel" against a base; that is not ported.
}

TEST(NormUrlTest, BareTrailingQuestionMarkIsNoQuery) {
  // new URL("https://a.test/x?").search === "" in the reference.
  EXPECT_EQ(N("https://a.test/x?"), "https://a.test/x");
  EXPECT_EQ(N("https://a.test/x/?"), "https://a.test/x");
  EXPECT_EQ(N("https://a.test?"), "https://a.test");
  EXPECT_EQ(N("https://a.test/x?#f"), "https://a.test/x");
}

TEST(NormUrlTest, BackslashesBeforeTheQueryAreSlashes) {
  EXPECT_EQ(N("https://a.test\\x\\y"), "https://a.test/x/y");
  EXPECT_EQ(N("https://a.test/x\\"), "https://a.test/x");
  // After the '?' they are kept.
  EXPECT_EQ(N("https://a.test/x?a=\\b"), "https://a.test/x?a=\\b");
}

TEST(NormUrlTest, SurroundingWhitespaceIsTrimmed) {
  EXPECT_EQ(N("  https://a.test/x \n"), "https://a.test/x");
}

TEST(NormUrlTest, LowercasesSchemeAndHost) {
  EXPECT_EQ(N("HTTPS://WWW.Example.COM/Path"), "https://www.example.com/Path");
}

TEST(NormUrlTest, KeepsPathAndQueryCase) {
  EXPECT_EQ(N("https://a.test/A/B?Q=1&r=2"), "https://a.test/A/B?Q=1&r=2");
}

TEST(NormUrlTest, DropsDefaultPorts) {
  EXPECT_EQ(N("http://a.test:80/x"), "http://a.test/x");
  EXPECT_EQ(N("https://a.test:443/x"), "https://a.test/x");
}

TEST(NormUrlTest, KeepsNonDefaultPorts) {
  EXPECT_EQ(N("https://a.test:80/x"), "https://a.test:80/x");
  EXPECT_EQ(N("http://a.test:443/x"), "http://a.test:443/x");
  EXPECT_EQ(N("https://a.test:8443/x"), "https://a.test:8443/x");
}

TEST(NormUrlTest, DropsFragment) {
  EXPECT_EQ(N("https://a.test/x#section"), "https://a.test/x");
  EXPECT_EQ(N("https://a.test/x?q=1#section"), "https://a.test/x?q=1");
  EXPECT_EQ(N("https://a.test#top"), "https://a.test");
}

TEST(NormUrlTest, IgnoresOneTrailingSlash) {
  EXPECT_EQ(N("https://a.test/"), "https://a.test");
  EXPECT_EQ(N("https://a.test"), "https://a.test");
  EXPECT_EQ(N("https://a.test/x/"), "https://a.test/x");
  EXPECT_EQ(N("https://a.test/x/?q=1"), "https://a.test/x?q=1");
  // Only a single slash is ignored.
  EXPECT_EQ(N("https://a.test/x//"), "https://a.test/x/");
}

TEST(NormUrlTest, TrailingSlashEqualityAcrossForms) {
  EXPECT_EQ(N("https://A.test:443/x/#f"), N("https://a.test/x"));
}

TEST(NormUrlTest, QueryOnlyAfterHost) {
  EXPECT_EQ(N("https://a.test?q=1"), "https://a.test?q=1");
}

TEST(NormUrlTest, Ipv6Host) {
  EXPECT_EQ(N("http://[::1]:80/x"), "http://[::1]/x");
  EXPECT_EQ(N("http://[::1]:8080/x"), "http://[::1]:8080/x");
}

TEST(NormUrlTest, RejectsNonAbsoluteOrNonHttp) {
  EXPECT_FALSE(NormUrl("").has_value());
  EXPECT_FALSE(NormUrl("/relative").has_value());
  EXPECT_FALSE(NormUrl("//a.test/x").has_value());
  EXPECT_FALSE(NormUrl("ftp://a.test/x").has_value());
  EXPECT_FALSE(NormUrl("mailto:a@b.test").has_value());
  EXPECT_FALSE(NormUrl("javascript:alert(1)").has_value());
}

TEST(NormUrlTest, RejectsMalformedAuthority) {
  EXPECT_FALSE(NormUrl("https:///x").has_value());
  EXPECT_FALSE(NormUrl("https://:80/x").has_value());
  EXPECT_FALSE(NormUrl("https://user@a.test/x").has_value());
  EXPECT_FALSE(NormUrl("https://a.test:abc/x").has_value());
  EXPECT_FALSE(NormUrl("https://a.test:99999/x").has_value());
  EXPECT_FALSE(NormUrl("https://a b.test/x").has_value());
  EXPECT_FALSE(NormUrl("https://[::1/x").has_value());
}

TEST(HreflangCodeTest, AcceptsValidCodes) {
  for (const char* code : {"en", "de", "fil", "en-us", "en-gb", "zh-hans",
                           "zh-hans-cn", "es-419", "x-default"}) {
    EXPECT_TRUE(NormalizeHreflangCode(code).has_value()) << code;
  }
}

TEST(HreflangCodeTest, NormalizesCaseAndUnderscore) {
  EXPECT_EQ(NormalizeHreflangCode("en_US"), "en-us");
  EXPECT_EQ(NormalizeHreflangCode("EN-uk"), "en-uk");
  EXPECT_EQ(NormalizeHreflangCode("X-Default"), "x-default");
  EXPECT_EQ(NormalizeHreflangCode("zh_Hans_CN"), "zh-hans-cn");
}

TEST(HreflangCodeTest, RejectsInvalidCodes) {
  for (const char* code :
       {"", "e", "english", "en-", "-en", "en-u", "en-usa", "en-us-x",
        "en-us-latn", "en--us", "x-default-2", "en us", "e1", "en-12",
        "en-1234", "xdefault", "en-latn-gb-x"}) {
    EXPECT_FALSE(NormalizeHreflangCode(code).has_value()) << code;
  }
}

}  // namespace
}  // namespace pagespeed::packs
