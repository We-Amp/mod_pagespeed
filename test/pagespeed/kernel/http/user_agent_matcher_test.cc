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

#include "pagespeed/kernel/http/user_agent_matcher.h"

#include "pagespeed/kernel/http/request_headers.h"
#include "test/pagespeed/kernel/base/gtest.h"
#include "test/pagespeed/kernel/http/user_agent_matcher_test_base.h"

namespace net_instaweb {

class UserAgentMatcherTest : public UserAgentMatcherTestBase {};

TEST_F(UserAgentMatcherTest, IsIeTest) {
  EXPECT_TRUE(user_agent_matcher_->IsIe(kIe6UserAgent));
  EXPECT_TRUE(user_agent_matcher_->IsIe(kIe7UserAgent));
  EXPECT_TRUE(user_agent_matcher_->IsIe(kIe8UserAgent));
  EXPECT_TRUE(user_agent_matcher_->IsIe(kIe9UserAgent));
  for (int i = 0; i < kIe11UserAgentsArraySize; ++i) {
    EXPECT_TRUE(user_agent_matcher_->IsIe(kIe11UserAgents[i]));
  }
  EXPECT_FALSE(user_agent_matcher_->IsIe(kFirefoxUserAgent));
  EXPECT_FALSE(user_agent_matcher_->IsIe(kChromeUserAgent));
}

TEST_F(UserAgentMatcherTest, SupportsImageInlining) {
  VerifyImageInliningSupport();
}

TEST_F(UserAgentMatcherTest, SupportsLazyloadImages) {
  EXPECT_TRUE(user_agent_matcher_->SupportsLazyloadImages(kChromeUserAgent));
  EXPECT_TRUE(user_agent_matcher_->SupportsLazyloadImages(kFirefoxUserAgent));
  EXPECT_TRUE(user_agent_matcher_->SupportsLazyloadImages(kIPhoneUserAgent));
  EXPECT_TRUE(
      user_agent_matcher_->SupportsLazyloadImages(kBlackBerryOS6UserAgent));
  EXPECT_FALSE(
      user_agent_matcher_->SupportsLazyloadImages(kBlackBerryOS5UserAgent));
  EXPECT_FALSE(
      user_agent_matcher_->SupportsLazyloadImages(kGooglePlusUserAgent));
}

TEST_F(UserAgentMatcherTest, NotSupportsImageInlining) {
  EXPECT_FALSE(user_agent_matcher_->SupportsImageInlining(kIe6UserAgent));
  EXPECT_FALSE(user_agent_matcher_->SupportsImageInlining(kFirefox1UserAgent));
  EXPECT_FALSE(user_agent_matcher_->SupportsImageInlining(kNokiaUserAgent));
  EXPECT_FALSE(user_agent_matcher_->SupportsImageInlining(kOpera5UserAgent));
  EXPECT_FALSE(user_agent_matcher_->SupportsImageInlining(kPSPUserAgent));
  EXPECT_FALSE(
      user_agent_matcher_->SupportsImageInlining(kGooglePlusUserAgent));
  EXPECT_TRUE(
      user_agent_matcher_->SupportsImageInlining(kAndroidChrome18UserAgent));
}

TEST_F(UserAgentMatcherTest, SupportsJsDefer) {
  EXPECT_FALSE(user_agent_matcher_->SupportsJsDefer(kIe9UserAgent, false));
  for (int i = 0; i < kIe11UserAgentsArraySize; ++i) {
    EXPECT_FALSE(
        user_agent_matcher_->SupportsJsDefer(kIe11UserAgents[i], false))
        << i << ": " << kIe11UserAgents[i];
  }
  EXPECT_TRUE(user_agent_matcher_->SupportsJsDefer(kChromeUserAgent, false));
  EXPECT_TRUE(user_agent_matcher_->SupportsJsDefer(kFirefoxUserAgent, false));
  EXPECT_TRUE(user_agent_matcher_->SupportsJsDefer(kSafariUserAgent, false));
}

TEST_F(UserAgentMatcherTest, SupportsJsDeferAllowMobile) {
  EXPECT_TRUE(user_agent_matcher_->SupportsJsDefer(kAndroidHCUserAgent, true));
  EXPECT_TRUE(user_agent_matcher_->SupportsJsDefer(kIPhone4Safari, true));
  // Desktop is also supported.
  EXPECT_TRUE(user_agent_matcher_->SupportsJsDefer(kChromeUserAgent, true));
}

TEST_F(UserAgentMatcherTest, NotSupportsJsDefer) {
  EXPECT_FALSE(user_agent_matcher_->SupportsJsDefer(kIe6UserAgent, false));
  EXPECT_FALSE(user_agent_matcher_->SupportsJsDefer(kIe8UserAgent, false));
  EXPECT_FALSE(user_agent_matcher_->SupportsJsDefer(kFirefox1UserAgent, false));
  EXPECT_FALSE(user_agent_matcher_->SupportsJsDefer(kFirefox3UserAgent, false));
  EXPECT_FALSE(user_agent_matcher_->SupportsJsDefer(kNokiaUserAgent, false));
  EXPECT_FALSE(user_agent_matcher_->SupportsJsDefer(kOpera5UserAgent, false));
  EXPECT_FALSE(user_agent_matcher_->SupportsJsDefer(kPSPUserAgent, false));
  // Mobile is not supported too.
  EXPECT_FALSE(user_agent_matcher_->SupportsJsDefer(kIPhone4Safari, false));
}

TEST_F(UserAgentMatcherTest, NotSupportsJsDeferAllowMobile) {
  EXPECT_FALSE(user_agent_matcher_->SupportsJsDefer(kOperaMobi9, true));
}

// The two bot tests below assert what UserAgentMatcher answers at its own
// layer, and that answer is deliberately unchanged. They do NOT mean bots are
// served deferred JavaScript: DeviceProperties::SupportsJsDefer ANDs in
// !IsBot(), so every user agent in these two tests loses defer before any
// filter sees it. See DeferDisabledForGooglebot in
// test/net/instaweb/rewriter/js_defer_disabled_filter_test.cc for the
// end-to-end behaviour, and the kDeferJSAllowlist comment in
// pagespeed/kernel/http/user_agent_matcher.cc for why the bot entries here
// were kept rather than deleted.

// Googlebot for mobile generally includes the UA for the mobile device
// being impersonated.
#define GOOGLEBOT_MOBILE \
  "(compatible; Googlebot-Mobile/2.1; +http://www.google.com/bot.html)"
TEST_F(UserAgentMatcherTest, MobileBotSupportsJsDefer) {
  // Reference: https://developers.google.com/webmasters/smartphone-sites/
  // detecting-user-agents
  const char kGoogleBotIphoneUA[] =
      "Mozilla/5.0 (iPhone; CPU iPhone OS 6_0 like Mac OS X) "
      "AppleWebKit/536.26 (KHTML, like Gecko) Version/6.0 Mobile/10A5376e "
      "Safari/8536.25 " GOOGLEBOT_MOBILE;
  EXPECT_TRUE(user_agent_matcher_->SupportsJsDefer(kGoogleBotIphoneUA, true));

  const char kGoogleBotAndroidUA[] =
      "Mozilla/5.0 (Linux; Android 4.3; Nexus 4 Build/JWR66Y) "
      "AppleWebKit/537.36 (KHTML, like Gecko) Chrome/32.0.1666.0 Mobile "
      "Safari/537.36 " GOOGLEBOT_MOBILE;
  EXPECT_TRUE(user_agent_matcher_->SupportsJsDefer(kGoogleBotAndroidUA, true));

  // Feature-phones don't support JS Defer.
  const char kSamsungFeatureBot[] =
      "SAMSUNG-SGH-E250/1.0 Profile/MIDP-2.0 Configuration/CLDC-1.1 "
      "UP.Browser/6.2.3.3.c.1.101 (GUI) MMP/2.0 " GOOGLEBOT_MOBILE;
  const char kDoCoMoBot[] =
      "DoCoMo/2.0 N905i(c100;TB;W24H16) " GOOGLEBOT_MOBILE;
  EXPECT_FALSE(user_agent_matcher_->SupportsJsDefer(kSamsungFeatureBot, true));
  EXPECT_FALSE(user_agent_matcher_->SupportsJsDefer(kDoCoMoBot, true));
}

// Googlebot for desktop generally does not includes a specific browser UA.
#define GOOGLEBOT_DESKTOP \
  "(compatible; Googlebot/2.1; +http://www.google.com/bot.html)"
TEST_F(UserAgentMatcherTest, DesktopBotSupportsJsDefer) {
  // https://support.google.com/webmasters/answer/1061943?hl=en
  const char kGooglebotNormal[] = "Mozilla/5.0 " GOOGLEBOT_DESKTOP;
  EXPECT_TRUE(user_agent_matcher_->SupportsJsDefer(kGooglebotNormal, true));

  const char kGooglebotRare[] =
      "Googlebot/2.1 (+http://www.google.com/bot.html)";
  EXPECT_TRUE(user_agent_matcher_->SupportsJsDefer(kGooglebotRare, true));

  const char kGooglebotNews[] = "Googlebot-News";
  EXPECT_TRUE(user_agent_matcher_->SupportsJsDefer(kGooglebotNews, true));

  const char kGooglebotImage[] = "Googlebot-Image/1.0";
  EXPECT_TRUE(user_agent_matcher_->SupportsJsDefer(kGooglebotImage, true));

  const char kGooglebotVideo[] = "Googlebot-Video/1.0";
  EXPECT_TRUE(user_agent_matcher_->SupportsJsDefer(kGooglebotVideo, true));

  const char kMediaPartners[] = "Mediapartners-Google";
  EXPECT_TRUE(user_agent_matcher_->SupportsJsDefer(kMediaPartners, true));

  const char kGooglebotAdsBot[] = "Googlebot-AdsBot/1.0";
  EXPECT_TRUE(user_agent_matcher_->SupportsJsDefer(kGooglebotAdsBot, true));

  // This UA was extrapolated from the snippet when google-searching for
  // "what's my user agent" from Firefox.
  const char kGoogleBotFirefoxUA[] = "Mozilla/5.0 " GOOGLEBOT_DESKTOP
                                     " Mozilla/5.0 (Windows NT 6.1; WOW64; "
                                     "rv:24.0) Gecko/20100101 Firefox/24.0";
  EXPECT_TRUE(user_agent_matcher_->SupportsJsDefer(kGoogleBotFirefoxUA, true));
}

TEST_F(UserAgentMatcherTest, WebpCapableLackingAcceptHeader) {
  EXPECT_FALSE(user_agent_matcher_->LegacyWebp(kTestingWebp));
  EXPECT_FALSE(user_agent_matcher_->LegacyWebp(kTestingWebpLosslessAlpha));

  EXPECT_TRUE(user_agent_matcher_->LegacyWebp(kAndroidICSUserAgent));
  EXPECT_FALSE(user_agent_matcher_->LegacyWebp(kChrome12UserAgent));
  EXPECT_FALSE(user_agent_matcher_->LegacyWebp(kChrome18UserAgent));
  EXPECT_FALSE(user_agent_matcher_->LegacyWebp(kOpera1110UserAgent));
  EXPECT_FALSE(user_agent_matcher_->LegacyWebp(kIPadChrome29UserAgent));
  EXPECT_FALSE(user_agent_matcher_->LegacyWebp(kIPadChrome36UserAgent));
  EXPECT_FALSE(user_agent_matcher_->LegacyWebp(kIPhoneChrome36UserAgent));
  EXPECT_FALSE(user_agent_matcher_->LegacyWebp(kOperaWithFirefoxUserAgent));
}

TEST_F(UserAgentMatcherTest, DoesntSupportWebp) {
  // The most interesting tests here are the recent but slightly older versions
  // of Chrome and Opera that can't display webp.
  EXPECT_FALSE(user_agent_matcher_->LegacyWebp(kAndroidHCUserAgent));
  EXPECT_FALSE(user_agent_matcher_->LegacyWebp(kChromeUserAgent));
  EXPECT_FALSE(user_agent_matcher_->LegacyWebp(kChrome9UserAgent));
  EXPECT_FALSE(user_agent_matcher_->LegacyWebp(kChrome15UserAgent));
  EXPECT_FALSE(user_agent_matcher_->LegacyWebp(kOpera1101UserAgent));
  EXPECT_FALSE(user_agent_matcher_->LegacyWebp(kFirefoxUserAgent));
  EXPECT_FALSE(user_agent_matcher_->LegacyWebp(kFirefox1UserAgent));
  EXPECT_FALSE(user_agent_matcher_->LegacyWebp(kFirefox42AndroidUserAgent));
  // https://github.com/apache/incubator-pagespeed-mod/issues/596: the legacy
  // allow list used to carry *Firefox/66.* .. *Firefox/71.* entries meant to
  // cover Firefox versions that were WebP-capable without sending the Accept
  // header. They never took effect -- the open-ended "*Firefox/*" entry in the
  // block list is registered after every allow entry, and the highest matching
  // index wins -- so every Firefox has always been false here. These pin that
  // verdict independently of whether those entries are present.
  EXPECT_FALSE(user_agent_matcher_->LegacyWebp(
      "Mozilla/5.0 (Windows NT 10.0; Win64; x64; rv:66.0) Gecko/20100101 "
      "Firefox/66.0"));
  EXPECT_FALSE(user_agent_matcher_->LegacyWebp(
      "Mozilla/5.0 (Windows NT 10.0; Win64; x64; rv:68.0) Gecko/20100101 "
      "Firefox/68.0"));
  EXPECT_FALSE(user_agent_matcher_->LegacyWebp(
      "Mozilla/5.0 (Android 9; Mobile; rv:71.0) Gecko/71.0 Firefox/71.0"));
  EXPECT_FALSE(user_agent_matcher_->LegacyWebp(kIe6UserAgent));
  EXPECT_FALSE(user_agent_matcher_->LegacyWebp(kIe7UserAgent));
  EXPECT_FALSE(user_agent_matcher_->LegacyWebp(kIe8UserAgent));
  EXPECT_FALSE(user_agent_matcher_->LegacyWebp(kIe9UserAgent));
  for (int i = 0; i < kIe11UserAgentsArraySize; ++i) {
    EXPECT_FALSE(user_agent_matcher_->LegacyWebp(kIe11UserAgents[i]));
  }
  EXPECT_FALSE(user_agent_matcher_->LegacyWebp(kIPhoneUserAgent));
  EXPECT_FALSE(user_agent_matcher_->LegacyWebp(kNokiaUserAgent));
  EXPECT_FALSE(user_agent_matcher_->LegacyWebp(kOpera5UserAgent));
  EXPECT_FALSE(user_agent_matcher_->LegacyWebp(kOpera8UserAgent));
  EXPECT_FALSE(user_agent_matcher_->LegacyWebp(kPSPUserAgent));
  EXPECT_FALSE(user_agent_matcher_->LegacyWebp(kSafariUserAgent));
  EXPECT_FALSE(user_agent_matcher_->LegacyWebp(kIPhoneChrome21UserAgent));
  EXPECT_FALSE(user_agent_matcher_->LegacyWebp(kIPadChrome28UserAgent));
  EXPECT_FALSE(user_agent_matcher_->LegacyWebp(kWindowsPhoneUserAgent));
}

// the design record: browsers that decode WebP but omit image/webp from the navigation
// Accept header. Safari 16+ and Firefox 132+.
TEST_F(UserAgentMatcherTest, SupportsWebpButOmitsNavigationAccept) {
  // Safari, at and above the Version/16 floor. Safari 26 is deliberately far
  // past it, in two senses: an open-ended allow entry must not go stale on a
  // version-digit rollover (which is what "*Chrome/??.*" did to animated WebP
  // for every Chrome >= 100), and Version/26 is Apple's real 2025 renumbering
  // of Safari to OS-aligned versions, which the predicate must admit.
  EXPECT_TRUE(user_agent_matcher_->SupportsWebpButOmitsNavigationAccept(
      kSafari16UserAgent));
  EXPECT_TRUE(user_agent_matcher_->SupportsWebpButOmitsNavigationAccept(
      kSafari17IPhoneUserAgent));
  EXPECT_TRUE(user_agent_matcher_->SupportsWebpButOmitsNavigationAccept(
      kSafari18UserAgent));
  EXPECT_TRUE(user_agent_matcher_->SupportsWebpButOmitsNavigationAccept(
      kSafari26UserAgent));

  // Firefox, at and above the floor, desktop and Android.
  EXPECT_TRUE(user_agent_matcher_->SupportsWebpButOmitsNavigationAccept(
      kFirefox132UserAgent));
  EXPECT_TRUE(user_agent_matcher_->SupportsWebpButOmitsNavigationAccept(
      kFirefox141UserAgent));
  EXPECT_TRUE(user_agent_matcher_->SupportsWebpButOmitsNavigationAccept(
      kFirefox141AndroidUserAgent));
}

TEST_F(UserAgentMatcherTest, DoesntSupportWebpButOmitsNavigationAccept) {
  // Below the Safari floor. 14 and 15 are the load-bearing rows: they DO
  // decode WebP on a new enough macOS, but not on Catalina, and their frozen
  // "10_15_7" OS token (these constants are byte-identical to real Catalina
  // UAs) makes the distinction undecidable -- so the design record floor denies
  // them. A grant here would render broken images on real Catalina machines.
  EXPECT_FALSE(user_agent_matcher_->SupportsWebpButOmitsNavigationAccept(
      kSafari14UserAgent));
  EXPECT_FALSE(user_agent_matcher_->SupportsWebpButOmitsNavigationAccept(
      kSafari15UserAgent));
  // Safari 13 has no WebP decoder at all, on any OS.
  EXPECT_FALSE(user_agent_matcher_->SupportsWebpButOmitsNavigationAccept(
      kSafari13UserAgent));
  EXPECT_FALSE(user_agent_matcher_->SupportsWebpButOmitsNavigationAccept(
      kSafari9UserAgent));
  EXPECT_FALSE(user_agent_matcher_->SupportsWebpButOmitsNavigationAccept(
      kSafari6UserAgent));
  EXPECT_FALSE(user_agent_matcher_->SupportsWebpButOmitsNavigationAccept(
      kSafariUserAgent));
  EXPECT_FALSE(user_agent_matcher_->SupportsWebpButOmitsNavigationAccept(
      kIPhone4Safari));
  EXPECT_FALSE(
      user_agent_matcher_->SupportsWebpButOmitsNavigationAccept(kIPodSafari));

  // Below the Firefox floor.
  EXPECT_FALSE(user_agent_matcher_->SupportsWebpButOmitsNavigationAccept(
      kFirefox131UserAgent));
  EXPECT_FALSE(user_agent_matcher_->SupportsWebpButOmitsNavigationAccept(
      kFirefoxUserAgent));
  EXPECT_FALSE(user_agent_matcher_->SupportsWebpButOmitsNavigationAccept(
      kFirefox1UserAgent));
  EXPECT_FALSE(user_agent_matcher_->SupportsWebpButOmitsNavigationAccept(
      kFirefox3UserAgent));
  EXPECT_FALSE(user_agent_matcher_->SupportsWebpButOmitsNavigationAccept(
      kFirefox5UserAgent));
  EXPECT_FALSE(user_agent_matcher_->SupportsWebpButOmitsNavigationAccept(
      kFirefox7UserAgent));
  EXPECT_FALSE(user_agent_matcher_->SupportsWebpButOmitsNavigationAccept(
      kFirefox42AndroidUserAgent));
  EXPECT_FALSE(user_agent_matcher_->SupportsWebpButOmitsNavigationAccept(
      kFirefoxMobileUserAgent));
  EXPECT_FALSE(user_agent_matcher_->SupportsWebpButOmitsNavigationAccept(
      kOperaWithFirefoxUserAgent));

  // The Chromium family: every one of these carries "Safari/537.36", which is
  // exactly why the Safari rule keys on the Version/ token and not on Safari/.
  // They all send Accept: image/webp anyway, so a UA-derived grant would be
  // both redundant and a way for the guess to reach a population it was never
  // measured against.
  EXPECT_FALSE(user_agent_matcher_->SupportsWebpButOmitsNavigationAccept(
      kChromeUserAgent));
  EXPECT_FALSE(user_agent_matcher_->SupportsWebpButOmitsNavigationAccept(
      kChrome137UserAgent));
  EXPECT_FALSE(user_agent_matcher_->SupportsWebpButOmitsNavigationAccept(
      kEdge137UserAgent));
  EXPECT_FALSE(user_agent_matcher_->SupportsWebpButOmitsNavigationAccept(
      kCriOS137UserAgent));
  EXPECT_FALSE(user_agent_matcher_->SupportsWebpButOmitsNavigationAccept(
      kCriOS48UserAgent));
  EXPECT_FALSE(user_agent_matcher_->SupportsWebpButOmitsNavigationAccept(
      kIPadChrome36UserAgent));
  EXPECT_FALSE(user_agent_matcher_->SupportsWebpButOmitsNavigationAccept(
      kOpera1110UserAgent));
  EXPECT_FALSE(user_agent_matcher_->SupportsWebpButOmitsNavigationAccept(
      kAndroidWebView114UserAgent));

  // Android: the old stock browser advertises "Version/4.0 ... Safari/", i.e.
  // the same shape as Safari, and is caught by both the floor and the
  // Android-with-Safari deny.
  EXPECT_FALSE(user_agent_matcher_->SupportsWebpButOmitsNavigationAccept(
      kAndroidICSUserAgent));
  EXPECT_FALSE(user_agent_matcher_->SupportsWebpButOmitsNavigationAccept(
      kAndroidHCUserAgent));
  EXPECT_FALSE(user_agent_matcher_->SupportsWebpButOmitsNavigationAccept(
      kAndroidNexusSUserAgent));
  EXPECT_FALSE(user_agent_matcher_->SupportsWebpButOmitsNavigationAccept(
      kSilkTabletUserAgent));

  // Firefox for iOS is WebKit underneath and does decode WebP, but its UA
  // carries no Version/ token and no Firefox/ token, so D1 declines it. This
  // is a known, deliberate false negative rather than an oversight.
  EXPECT_FALSE(user_agent_matcher_->SupportsWebpButOmitsNavigationAccept(
      kFxiOS126UserAgent));

  // Crawlers and tooling. Applebot is the load-bearing case: it advertises
  // "Version/N... Safari/" above the version floor and would otherwise be
  // handed WebP.
  EXPECT_FALSE(user_agent_matcher_->SupportsWebpButOmitsNavigationAccept(
      kApplebotSafari16UserAgent));
  EXPECT_FALSE(user_agent_matcher_->SupportsWebpButOmitsNavigationAccept(
      kGooglebotUserAgent));

  // Non-browsers and pre-WebKit handsets.
  EXPECT_FALSE(
      user_agent_matcher_->SupportsWebpButOmitsNavigationAccept(kIe6UserAgent));
  EXPECT_FALSE(
      user_agent_matcher_->SupportsWebpButOmitsNavigationAccept(kIe9UserAgent));
  EXPECT_FALSE(user_agent_matcher_->SupportsWebpButOmitsNavigationAccept(
      kBlackBerryOS6UserAgent));
  EXPECT_FALSE(user_agent_matcher_->SupportsWebpButOmitsNavigationAccept(
      kWindowsPhoneUserAgent));
  EXPECT_FALSE(
      user_agent_matcher_->SupportsWebpButOmitsNavigationAccept(kPSPUserAgent));
  EXPECT_FALSE(
      user_agent_matcher_->SupportsWebpButOmitsNavigationAccept(kNokiaUserAgent));
  EXPECT_FALSE(user_agent_matcher_->SupportsWebpButOmitsNavigationAccept(""));
}

// A version token with no minor component ("Firefox/3", "Version/9 Safari/")
// must still be held below the floor. No shipping browser emits that shape,
// but crawlers fabricate it, and every floor pattern that requires a '.' can
// be bypassed by simply omitting one character.
TEST_F(UserAgentMatcherTest, WebpNavigationAcceptFloorRejectsMajorOnlyVersion) {
  EXPECT_FALSE(user_agent_matcher_->SupportsWebpButOmitsNavigationAccept(
      "Mozilla/5.0 (compatible; Some.ru; +http://some.ru/bot) Firefox/3"));
  EXPECT_FALSE(user_agent_matcher_->SupportsWebpButOmitsNavigationAccept(
      "Mozilla/5.0 (Linux, Sony/COM2/1.01 [en]; like Gecko/2007) Firefox/2"));
  EXPECT_FALSE(user_agent_matcher_->SupportsWebpButOmitsNavigationAccept(
      "Mozilla/5.0 (X11; Linux) Gecko/20100101 Firefox/99 Build/x"));
  EXPECT_FALSE(user_agent_matcher_->SupportsWebpButOmitsNavigationAccept(
      "Mozilla/5.0 (X11; Linux) Gecko/20100101 Firefox/131"));
  EXPECT_FALSE(user_agent_matcher_->SupportsWebpButOmitsNavigationAccept(
      "Mozilla/5.0 (Macintosh) AppleWebKit/605.1.15 Version/9 Safari/605.1.15"));
  EXPECT_FALSE(user_agent_matcher_->SupportsWebpButOmitsNavigationAccept(
      "Mozilla/5.0 (Macintosh) AppleWebKit/605.1.15 Version/13 Safari/605.1"));
  EXPECT_FALSE(user_agent_matcher_->SupportsWebpButOmitsNavigationAccept(
      "Mozilla/5.0 (Macintosh) AppleWebKit/605.1.15 Version/15 Safari/605.1"));
  // ...while the same shapes at or above the floor still pass.
  EXPECT_TRUE(user_agent_matcher_->SupportsWebpButOmitsNavigationAccept(
      "Mozilla/5.0 (X11; Linux) Gecko/20100101 Firefox/132"));
  EXPECT_TRUE(user_agent_matcher_->SupportsWebpButOmitsNavigationAccept(
      "Mozilla/5.0 (X11; Linux) Gecko/20100101 Firefox/1000.0"));
}

// The floor must hold for EVERY delimiter that can terminate a sub-floor
// version token, not just '.' and ' ': the deny list has no character
// classes, so each of these shapes once slipped through an enumeration hole
// and was granted. All fabricated UA shapes -- no real browser emits any of
// them -- which is exactly why they are the shapes a floor bypass would use.
TEST_F(UserAgentMatcherTest, WebpNavigationAcceptFloorHoldsForEveryDelimiter) {
  // Safari, single-digit sub-floor major with exotic delimiters.
  EXPECT_FALSE(user_agent_matcher_->SupportsWebpButOmitsNavigationAccept(
      "Mozilla/5.0 (Macintosh) AppleWebKit/605.1.15 Version/9;Safari/605.1.15"));
  // Single digit glued directly to the Safari token.
  EXPECT_FALSE(user_agent_matcher_->SupportsWebpButOmitsNavigationAccept(
      "Mozilla/5.0 (Macintosh) AppleWebKit/605.1.15 Version/9Safari/605.1.15"));
  // Two-digit sub-floor majors: the per-major catch-alls must fire on every
  // delimiter shape, including glued.
  EXPECT_FALSE(user_agent_matcher_->SupportsWebpButOmitsNavigationAccept(
      "Mozilla/5.0 (Macintosh) AppleWebKit/605.1.15 "
      "Version/15)Safari/605.1.15"));
  EXPECT_FALSE(user_agent_matcher_->SupportsWebpButOmitsNavigationAccept(
      "Mozilla/5.0 (Macintosh) AppleWebKit/605.1.15 "
      "Version/15,Safari/605.1.15"));
  EXPECT_FALSE(user_agent_matcher_->SupportsWebpButOmitsNavigationAccept(
      "Mozilla/5.0 (Macintosh) AppleWebKit/605.1.15 "
      "Version/14/Safari/605.1.15"));
  EXPECT_FALSE(user_agent_matcher_->SupportsWebpButOmitsNavigationAccept(
      "Mozilla/5.0 (Macintosh) AppleWebKit/605.1.15 "
      "Version/15Safari/605.1.15"));
  // Zero-padded spelling must not be misread as a two-digit major.
  EXPECT_FALSE(user_agent_matcher_->SupportsWebpButOmitsNavigationAccept(
      "Mozilla/5.0 (Macintosh) AppleWebKit/605.1.15 Version/09 Safari/605.1"));

  // Firefox, sub-floor majors with non-space, non-dot delimiters.
  EXPECT_FALSE(user_agent_matcher_->SupportsWebpButOmitsNavigationAccept(
      "Mozilla/5.0 (X11; Linux) Gecko/20100101 Firefox/45;x"));
  EXPECT_FALSE(user_agent_matcher_->SupportsWebpButOmitsNavigationAccept(
      "Mozilla/5.0 (X11; Linux) Gecko/20100101 Firefox/45)"));
  EXPECT_FALSE(user_agent_matcher_->SupportsWebpButOmitsNavigationAccept(
      "Mozilla/5.0 (X11; Linux) Gecko/20100101 Firefox/131;x"));
  EXPECT_FALSE(user_agent_matcher_->SupportsWebpButOmitsNavigationAccept(
      "Mozilla/5.0 (X11; Linux) Gecko/20100101 Firefox/104:x"));

  // An alien token merely CONTAINING "Version/" must not satisfy the
  // token-anchored allow entry -- real Safari always space-precedes it.
  EXPECT_FALSE(user_agent_matcher_->SupportsWebpButOmitsNavigationAccept(
      "Mozilla/5.0 AppVersion/22.1 AppleWebKit/601 (KHTML, like Gecko) "
      "Safari/601.1"));
  EXPECT_FALSE(user_agent_matcher_->SupportsWebpButOmitsNavigationAccept(
      "Mozilla/5.0 MyFirefox/9000.0"));

  // ...and none of the above tightening may cost a real population: the
  // documented over-deny of the Safari 10-15 catch-alls is bounded to the
  // never-existing majors 100-159, so 99 and 160 pass, as do the real floors
  // and the three-digit Firefox future the width rows must never swallow.
  EXPECT_TRUE(user_agent_matcher_->SupportsWebpButOmitsNavigationAccept(
      kSafari16UserAgent));
  EXPECT_TRUE(user_agent_matcher_->SupportsWebpButOmitsNavigationAccept(
      kSafari26UserAgent));
  EXPECT_TRUE(user_agent_matcher_->SupportsWebpButOmitsNavigationAccept(
      kFirefox132UserAgent));
  EXPECT_TRUE(user_agent_matcher_->SupportsWebpButOmitsNavigationAccept(
      kFirefox141UserAgent));
  EXPECT_TRUE(user_agent_matcher_->SupportsWebpButOmitsNavigationAccept(
      "Mozilla/5.0 (X11; Linux) Gecko/20100101 Firefox/451.0"));
  EXPECT_TRUE(user_agent_matcher_->SupportsWebpButOmitsNavigationAccept(
      "Mozilla/5.0 (Macintosh; Intel Mac OS X 10_15_7) AppleWebKit/605.1.15 "
      "(KHTML, like Gecko) Version/99.0 Safari/605.1.15"));
  EXPECT_TRUE(user_agent_matcher_->SupportsWebpButOmitsNavigationAccept(
      "Mozilla/5.0 (Macintosh; Intel Mac OS X 10_15_7) AppleWebKit/605.1.15 "
      "(KHTML, like Gecko) Version/160.0 Safari/605.1.15"));
}

TEST_F(UserAgentMatcherTest, IsAndroidUserAgentTest) {
  EXPECT_TRUE(user_agent_matcher_->IsAndroidUserAgent(kAndroidHCUserAgent));
  EXPECT_FALSE(user_agent_matcher_->IsAndroidUserAgent(kIe6UserAgent));
}

TEST_F(UserAgentMatcherTest, IsiOSUserAgentTest) {
  EXPECT_TRUE(user_agent_matcher_->IsiOSUserAgent(kIPhoneUserAgent));
  EXPECT_TRUE(user_agent_matcher_->IsiOSUserAgent(kIPadUserAgent));
  EXPECT_TRUE(user_agent_matcher_->IsiOSUserAgent(kIPodSafari));
  EXPECT_TRUE(user_agent_matcher_->IsiOSUserAgent(kIPhoneChrome21UserAgent));
  EXPECT_TRUE(user_agent_matcher_->IsiOSUserAgent(kIPadChrome28UserAgent));
  EXPECT_TRUE(user_agent_matcher_->IsiOSUserAgent(kIPadChrome29UserAgent));
  EXPECT_TRUE(user_agent_matcher_->IsiOSUserAgent(kIPadChrome36UserAgent));
  EXPECT_TRUE(user_agent_matcher_->IsiOSUserAgent(kIPhoneChrome36UserAgent));
  EXPECT_FALSE(user_agent_matcher_->IsiOSUserAgent(kIe6UserAgent));
}

TEST_F(UserAgentMatcherTest, ChromeBuildNumberTest) {
  int major = -1;
  int minor = -1;
  int build = -1;
  int patch = -1;
  EXPECT_TRUE(user_agent_matcher_->GetChromeBuildNumber(
      kChrome9UserAgent, &major, &minor, &build, &patch));
  EXPECT_EQ(major, 9);
  EXPECT_EQ(minor, 0);
  EXPECT_EQ(build, 597);
  EXPECT_EQ(patch, 19);

  // On iOS it's "CriOS", not "Chrome".
  EXPECT_TRUE(user_agent_matcher_->GetChromeBuildNumber(
      kIPhoneChrome21UserAgent, &major, &minor, &build, &patch));
  EXPECT_EQ(major, 21);
  EXPECT_EQ(minor, 0);
  EXPECT_EQ(build, 1180);
  EXPECT_EQ(patch, 82);

  EXPECT_FALSE(user_agent_matcher_->GetChromeBuildNumber(
      kAndroidHCUserAgent, &major, &minor, &build, &patch));
  EXPECT_FALSE(user_agent_matcher_->GetChromeBuildNumber(
      kChromeUserAgent, &major, &minor, &build, &patch));
  EXPECT_FALSE(user_agent_matcher_->GetChromeBuildNumber(
      "Chrome/10.0", &major, &minor, &build, &patch));
  EXPECT_FALSE(user_agent_matcher_->GetChromeBuildNumber(
      "Chrome/10.0.1.", &major, &minor, &build, &patch));
}

TEST_F(UserAgentMatcherTest, ExceedsChromeBuildAndPatchTest) {
  EXPECT_TRUE(user_agent_matcher_->UserAgentExceedsChromeBuildAndPatch(
      kIPhoneChrome21UserAgent, 1000, 0));
  EXPECT_TRUE(user_agent_matcher_->UserAgentExceedsChromeBuildAndPatch(
      kIPhoneChrome21UserAgent, 1000, 999));
  EXPECT_TRUE(user_agent_matcher_->UserAgentExceedsChromeBuildAndPatch(
      kIPhoneChrome21UserAgent, 1180, 82));
  EXPECT_FALSE(user_agent_matcher_->UserAgentExceedsChromeBuildAndPatch(
      kIPhoneChrome21UserAgent, 1180, 83));
  EXPECT_FALSE(user_agent_matcher_->UserAgentExceedsChromeBuildAndPatch(
      kIPhoneChrome21UserAgent, 1181, 0));
  EXPECT_FALSE(user_agent_matcher_->UserAgentExceedsChromeBuildAndPatch(
      kIPhoneChrome21UserAgent, 1181, 83));

  EXPECT_TRUE(user_agent_matcher_->UserAgentExceedsChromeAndroidBuildAndPatch(
      kAndroidChrome21UserAgent, 1000, 0));
  EXPECT_FALSE(user_agent_matcher_->UserAgentExceedsChromeAndroidBuildAndPatch(
      kIPhoneChrome21UserAgent, 1000, 0));

  EXPECT_TRUE(user_agent_matcher_->UserAgentExceedsChromeiOSBuildAndPatch(
      kIPhoneChrome21UserAgent, 1000, 0));
  EXPECT_FALSE(user_agent_matcher_->UserAgentExceedsChromeiOSBuildAndPatch(
      kAndroidChrome21UserAgent, 1000, 0));
}

TEST_F(UserAgentMatcherTest, SupportsDnsPrefetch) {
  EXPECT_TRUE(user_agent_matcher_->SupportsDnsPrefetch(kChromeUserAgent));
  EXPECT_TRUE(user_agent_matcher_->SupportsDnsPrefetch(kIe9UserAgent));
  EXPECT_TRUE(user_agent_matcher_->SupportsDnsPrefetch(kFirefox5UserAgent));
  EXPECT_TRUE(user_agent_matcher_->SupportsDnsPrefetch(kSafari6UserAgent));
  EXPECT_TRUE(user_agent_matcher_->SupportsDnsPrefetch(kSafari9UserAgent));
  for (int i = 0; i < kIe11UserAgentsArraySize; ++i) {
    EXPECT_TRUE(user_agent_matcher_->SupportsDnsPrefetch(kIe11UserAgents[i]));
  }
}

TEST_F(UserAgentMatcherTest, DoesntSupportDnsPrefetch) {
  EXPECT_FALSE(user_agent_matcher_->SupportsDnsPrefetch(kFirefox1UserAgent));
  EXPECT_FALSE(user_agent_matcher_->SupportsDnsPrefetch(kIe6UserAgent));
  EXPECT_FALSE(user_agent_matcher_->SupportsDnsPrefetch(kIe7UserAgent));
  EXPECT_FALSE(user_agent_matcher_->SupportsDnsPrefetch(kIe8UserAgent));
  EXPECT_FALSE(user_agent_matcher_->SupportsDnsPrefetch(kSafariUserAgent));
}

TEST_F(UserAgentMatcherTest, GetDeviceTypeForUA) { VerifyGetDeviceTypeForUA(); }

TEST_F(UserAgentMatcherTest, IE11NoDeferJs) {
  for (int i = 0; i < kIe11UserAgentsArraySize; ++i) {
    const char* user_agent = kIe11UserAgents[i];
    EXPECT_FALSE(user_agent_matcher_->SupportsJsDefer(user_agent, true));
  }
}

TEST_F(UserAgentMatcherTest, Mobilization) { VerifyMobilizationSupport(); }

TEST_F(UserAgentMatcherTest, SupportsNativeLazyLoading) {
  // Chromium >= 77 (Chrome, Edge, Opera share the Chrome/<version> token).
  EXPECT_TRUE(user_agent_matcher_->SupportsNativeLazyLoading(
      "Mozilla/5.0 (X11; Linux x86_64) AppleWebKit/537.36 "
      "(KHTML, like Gecko) Chrome/120.0.0.0 Safari/537.36"));
  EXPECT_TRUE(user_agent_matcher_->SupportsNativeLazyLoading(
      "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 "
      "(KHTML, like Gecko) Chrome/77.0.3865.90 Safari/537.36"));
  EXPECT_TRUE(user_agent_matcher_->SupportsNativeLazyLoading(
      "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 "
      "(KHTML, like Gecko) Chrome/119.0.0.0 Safari/537.36 Edg/119.0.0.0"));
  EXPECT_FALSE(user_agent_matcher_->SupportsNativeLazyLoading(
      "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 "
      "(KHTML, like Gecko) Chrome/76.0.3809.100 Safari/537.36"));
  EXPECT_FALSE(
      user_agent_matcher_->SupportsNativeLazyLoading(kChrome18UserAgent));

  // Firefox >= 75.
  EXPECT_TRUE(user_agent_matcher_->SupportsNativeLazyLoading(
      "Mozilla/5.0 (X11; Linux x86_64; rv:109.0) Gecko/20100101 "
      "Firefox/115.0"));
  EXPECT_TRUE(user_agent_matcher_->SupportsNativeLazyLoading(
      "Mozilla/5.0 (X11; Linux x86_64; rv:75.0) Gecko/20100101 "
      "Firefox/75.0"));
  EXPECT_FALSE(user_agent_matcher_->SupportsNativeLazyLoading(
      "Mozilla/5.0 (X11; Linux x86_64; rv:74.0) Gecko/20100101 "
      "Firefox/74.0"));

  // Safari >= 15.4 (separate Version/<major>.<minor> token).
  EXPECT_TRUE(user_agent_matcher_->SupportsNativeLazyLoading(
      "Mozilla/5.0 (Macintosh; Intel Mac OS X 10_15_7) AppleWebKit/605.1.15 "
      "(KHTML, like Gecko) Version/15.4 Safari/605.1.15"));
  EXPECT_TRUE(user_agent_matcher_->SupportsNativeLazyLoading(
      "Mozilla/5.0 (Macintosh; Intel Mac OS X 10_15_7) AppleWebKit/605.1.15 "
      "(KHTML, like Gecko) Version/17.1 Safari/605.1.15"));
  EXPECT_FALSE(user_agent_matcher_->SupportsNativeLazyLoading(
      "Mozilla/5.0 (Macintosh; Intel Mac OS X 10_15_7) AppleWebKit/605.1.15 "
      "(KHTML, like Gecko) Version/15.3 Safari/605.1.15"));
  EXPECT_FALSE(
      user_agent_matcher_->SupportsNativeLazyLoading(kSafariUserAgent));

  // Unknown or legacy user agents fall back to the script-based path.
  EXPECT_FALSE(user_agent_matcher_->SupportsNativeLazyLoading(kIe10UserAgent));
  EXPECT_FALSE(
      user_agent_matcher_->SupportsNativeLazyLoading(kCriOS48UserAgent));
  EXPECT_FALSE(user_agent_matcher_->SupportsNativeLazyLoading(""));
}

}  // namespace net_instaweb
