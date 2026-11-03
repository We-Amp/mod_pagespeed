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

#include "pagespeed/system/site_cache_fragment.h"

#include "net/instaweb/rewriter/public/rewrite_options.h"
#include "pagespeed/kernel/base/null_message_handler.h"
#include "test/pagespeed/kernel/base/gtest.h"

namespace net_instaweb {
namespace {

SiteIdentity Identity(std::vector<GoogleString> names,
                      std::vector<GoogleString> addresses) {
  SiteIdentity identity;
  identity.names = std::move(names);
  identity.listen_addresses = std::move(addresses);
  return identity;
}

class SiteCacheFragmentTest : public testing::Test {
 protected:
  static void SetUpTestSuite() { RewriteOptions::Initialize(); }
  static void TearDownTestSuite() { RewriteOptions::Terminate(); }

  NullMessageHandler handler_;
};

TEST_F(SiteCacheFragmentTest, ValueIsFixedForAGivenIdentity) {
  // A change here colds every site's cache at update: it must be deliberate.
  EXPECT_EQ(
      "site:896d7b6145f1c83a9a4528fb99321bf8",
      DeriveSiteCacheFragment(Identity({"a.example.test"}, {"0.0.0.0:80"})));
}

TEST_F(SiteCacheFragmentTest, OrderRepeatsAndCaseDoNotMatter) {
  EXPECT_EQ(
      DeriveSiteCacheFragment(Identity({"a.example.test", "b.example.test"},
                                       {"0.0.0.0:80", "[::]:80"})),
      DeriveSiteCacheFragment(
          Identity({"B.Example.Test", "a.example.test", "b.example.test"},
                   {"[::]:80", "0.0.0.0:80", "[::]:80"})));
}

TEST_F(SiteCacheFragmentTest, NamesAndAddressesSeparateSites) {
  const GoogleString base =
      DeriveSiteCacheFragment(Identity({"a.example.test"}, {"0.0.0.0:80"}));
  EXPECT_NE(base, DeriveSiteCacheFragment(
                      Identity({"b.example.test"}, {"0.0.0.0:80"})));
  EXPECT_NE(base, DeriveSiteCacheFragment(
                      Identity({"a.example.test"}, {"0.0.0.0:81"})));
  EXPECT_NE(base, DeriveSiteCacheFragment(
                      Identity({"a.example.test"}, {"unix:/run/a.sock"})));
  // A value in one list is not the same value in the other.
  EXPECT_NE(DeriveSiteCacheFragment(Identity({"x"}, {})),
            DeriveSiteCacheFragment(Identity({}, {"x"})));
  // Values do not run together.
  EXPECT_NE(DeriveSiteCacheFragment(Identity({"ab", "c"}, {})),
            DeriveSiteCacheFragment(Identity({"a", "bc"}, {})));
}

TEST_F(SiteCacheFragmentTest, PatternsKeepTheirCase) {
  EXPECT_NE(DeriveSiteCacheFragment(Identity({"~^A\\.test$"}, {})),
            DeriveSiteCacheFragment(Identity({"~^a\\.test$"}, {})));
}

TEST_F(SiteCacheFragmentTest, DerivedValueIsOnlySetByTheInternalSetter) {
  const GoogleString derived =
      DeriveSiteCacheFragment(Identity({"a.example.test"}, {"0.0.0.0:80"}));
  RewriteOptions options(nullptr);
  GoogleString message;
  EXPECT_EQ(RewriteOptions::kOptionValueInvalid,
            options.ParseAndSetOptionFromName1(RewriteOptions::kCacheFragment,
                                               derived, &message, &handler_));
  EXPECT_EQ("", options.cache_fragment());
  options.set_cache_fragment(derived);
  EXPECT_EQ(derived, options.cache_fragment());
}

TEST_F(SiteCacheFragmentTest, SiteIdIsTheDigestOfTheServerFragment) {
  EXPECT_EQ("", SiteIdFromServerFragment(""));
  // SHA-256 of "abc".
  EXPECT_EQ("ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad",
            SiteIdFromServerFragment("abc"));
}

TEST_F(SiteCacheFragmentTest, SiteIdIsSixtyFourLowercaseHex) {
  const GoogleString site = SiteIdFromServerFragment(
      DeriveSiteCacheFragment(Identity({"a.example.test"}, {"0.0.0.0:80"})));
  EXPECT_EQ(64u, site.size());
  EXPECT_TRUE(IsSiteId(site));
  // A derived fragment and an explicit one are hashed the same way.
  EXPECT_EQ("79f896c0d179deea1409a853e933061f44d9c9ffa9ed8e61e5dfd070b96f5d19",
            site);
  // An explicit CacheFragment X names the site sha256(X).
  EXPECT_EQ("4b68ab3847feda7d6c62c1fbcbeebfa35eab7351ed5e78f4ddadea5df64b8015",
            SiteIdFromServerFragment("X"));
}

TEST_F(SiteCacheFragmentTest, OnlyTheSiteIdFormIsASite) {
  EXPECT_FALSE(IsSiteId(""));
  EXPECT_FALSE(IsSiteId("abc"));
  const GoogleString site = SiteIdFromServerFragment("X");
  EXPECT_TRUE(IsSiteId(site));
  GoogleString upper = site;
  UpperString(&upper);
  EXPECT_FALSE(IsSiteId(upper));
  EXPECT_FALSE(IsSiteId(StrCat(site, "0")));
  EXPECT_FALSE(IsSiteId(site.substr(1)));
  GoogleString not_hex = site;
  not_hex[10] = 'g';
  EXPECT_FALSE(IsSiteId(not_hex));
  // A fragment is not a site id.
  EXPECT_FALSE(IsSiteId("site:896d7b6145f1c83a9a4528fb99321bf8"));
}

// The IIS vectors are pinned against the UTF-8 bytes of the site name, spelled
// out as escapes so no source encoding can change them.
// IIS supplies site names upper-cased; the vectors pin the derivation itself.
TEST_F(SiteCacheFragmentTest, IisValueIsFixedForAGivenSite) {
  EXPECT_EQ("site:301d34dd212d3d358dc2d00d388d8828",
            DeriveIisSiteCacheFragment(1, "Default Web Site"));
  EXPECT_EQ("site:a01dd46803f4d1b0b8757124dbe7a153",
            DeriveIisSiteCacheFragment(4294967295u, "x"));
}

TEST_F(SiteCacheFragmentTest, IisNonAsciiNamesAreHashedAsUtf8) {
  // U+00E9, precomposed: "caf\xC3\xA9".
  EXPECT_EQ("site:c6ab80c63e32db65e9bca7c7910f50c3",
            DeriveIisSiteCacheFragment(2, "caf\xC3\xA9"));
  // The same text decomposed (e + U+0301) is a different name.
  EXPECT_EQ("site:59b480f40a84f009528c5b85cea9fe51",
            DeriveIisSiteCacheFragment(2, "cafe\xCC\x81"));
  // The module does not fold the name: U+00C9 upper case.
  EXPECT_EQ("site:d8e77191c1dedb8bc1ca2d2fa0184a6d",
            DeriveIisSiteCacheFragment(2, "CAF\xC3\x89"));
  // Two CJK characters, U+7AD9 U+70B9.
  EXPECT_EQ("site:58cbf5d98caa9cc65c93056435f600c7",
            DeriveIisSiteCacheFragment(3, "\xE7\xAB\x99\xE7\x82\xB9"));
  EXPECT_EQ("site:1496ac58faf7e96f7edde8a98befdb82",
            DeriveIisSiteCacheFragment(3, "\xE7\xAB\x99\xE7\x82\xB9 "));
}

TEST_F(SiteCacheFragmentTest, IisRenameOrIdReuseChangesTheSite) {
  const GoogleString base = DeriveIisSiteCacheFragment(2, "caf\xC3\xA9");
  // Renamed, same id.
  EXPECT_NE(base, DeriveIisSiteCacheFragment(2, "cafe"));
  // Same name, another id (a site recreated under a new id).
  EXPECT_NE(base, DeriveIisSiteCacheFragment(5, "caf\xC3\xA9"));
  // A reused id with a new name.
  EXPECT_NE(DeriveIisSiteCacheFragment(7, "old"),
            DeriveIisSiteCacheFragment(7, "new"));
  // The id and the name do not run together.
  EXPECT_NE(DeriveIisSiteCacheFragment(1, "2x"),
            DeriveIisSiteCacheFragment(12, "x"));
  // And the site that follows from each is different too.
  EXPECT_NE(SiteIdFromServerFragment(DeriveIisSiteCacheFragment(7, "old")),
            SiteIdFromServerFragment(DeriveIisSiteCacheFragment(7, "new")));
}

TEST_F(SiteCacheFragmentTest, IisRequestOfAnotherSiteGetsNoSite) {
  const GoogleString fragment = DeriveIisSiteCacheFragment(7, "site");
  // The site the context was created for: its site.
  EXPECT_EQ(SiteIdFromServerFragment(fragment),
            IisRequestSiteId(7, 7, fragment));
  EXPECT_TRUE(IsSiteId(IisRequestSiteId(7, 7, fragment)));
  // Any other IIS site id: no site, so both arms decline.
  EXPECT_EQ("", IisRequestSiteId(8, 7, fragment));
  EXPECT_EQ("", IisRequestSiteId(0, 7, fragment));
  EXPECT_FALSE(IsSiteId(IisRequestSiteId(8, 7, fragment)));
}

TEST_F(SiteCacheFragmentTest, IisValueCannotBeSetByHand) {
  const GoogleString derived = DeriveIisSiteCacheFragment(1, "Default Web Site");
  RewriteOptions options(nullptr);
  GoogleString message;
  EXPECT_EQ(RewriteOptions::kOptionValueInvalid,
            options.ParseAndSetOptionFromName1(RewriteOptions::kCacheFragment,
                                               derived, &message, &handler_));
}

}  // namespace
}  // namespace net_instaweb
