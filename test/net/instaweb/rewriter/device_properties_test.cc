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

#include "net/instaweb/rewriter/public/device_properties.h"

#include "pagespeed/kernel/http/http_names.h"
#include "pagespeed/kernel/http/request_headers.h"
#include "pagespeed/kernel/http/user_agent_matcher.h"
#include "test/pagespeed/kernel/base/gtest.h"
#include "test/pagespeed/kernel/http/user_agent_matcher_test_base.h"

namespace net_instaweb {

class DevicePropertiesTest : public testing::Test {
 protected:
  DevicePropertiesTest() : device_properties_(&user_agent_matcher_) {}

  void ParseAndVerifySaveData(const char* header_value, bool expected_value) {
    RequestHeaders headers;
    if (header_value != nullptr) {
      headers.Add(HttpAttributes::kSaveData, header_value);
    }
    DeviceProperties device_properties(&user_agent_matcher_);
    device_properties.ParseRequestHeaders(headers);
    EXPECT_EQ(expected_value, device_properties.RequestsSaveData());
  }

  // Every WebP capability is false: the request advertised no "Accept:
  // image/webp" and the user agent is not in the legacy no-Accept population.
  void ExpectNoWebpSupport(const char* user_agent) {
    SCOPED_TRACE(user_agent);
    device_properties_.SetUserAgent(user_agent);
    EXPECT_FALSE(device_properties_.SupportsWebpInPlace());
    EXPECT_FALSE(device_properties_.SupportsWebpRewrittenUrls());
    EXPECT_FALSE(device_properties_.SupportsWebpLosslessAlpha());
    EXPECT_FALSE(device_properties_.SupportsWebpAnimated());
  }

  // The legacy no-Accept population (the Android browser): lossy WebP on
  // rewritten URLs only. This is the ONE capability still derived from the
  // user-agent string, via UserAgentMatcher::LegacyWebp().
  void ExpectLegacyRewrittenUrlWebpOnly(const char* user_agent) {
    SCOPED_TRACE(user_agent);
    device_properties_.SetUserAgent(user_agent);
    EXPECT_FALSE(device_properties_.SupportsWebpInPlace());
    EXPECT_TRUE(device_properties_.SupportsWebpRewrittenUrls());
    EXPECT_FALSE(device_properties_.SupportsWebpLosslessAlpha());
    EXPECT_FALSE(device_properties_.SupportsWebpAnimated());
  }

  // "Accept: image/webp" was seen, so every capability is granted regardless
  // of the user agent.
  void ExpectFullWebpSupport(const char* user_agent) {
    SCOPED_TRACE(user_agent);
    device_properties_.SetUserAgent(user_agent);
    EXPECT_TRUE(device_properties_.SupportsWebpInPlace());
    EXPECT_TRUE(device_properties_.SupportsWebpRewrittenUrls());
    EXPECT_TRUE(device_properties_.SupportsWebpLosslessAlpha());
    EXPECT_TRUE(device_properties_.SupportsWebpAnimated());
  }

  void AcceptWebp() {
    RequestHeaders headers;
    headers.Add(HttpAttributes::kAccept, "image/webp");
    device_properties_.ParseRequestHeaders(headers);
  }

  UserAgentMatcher user_agent_matcher_;
  DeviceProperties device_properties_;
};

TEST_F(DevicePropertiesTest, WebpUserAgentIdentificationNoAccept) {
  // NOTE: the purpose here is *not* to test user_agent_matcher's coverage of
  // webp user agents, just to see that they're properly reflected in
  // device_properties_.
  //
  // Note: these are all false due to the lack of accept:webp.
  ExpectNoWebpSupport(UserAgentMatcherTestBase::kIe7UserAgent);
  ExpectNoWebpSupport(UserAgentMatcherTestBase::kTestingWebp);
  ExpectNoWebpSupport(UserAgentMatcherTestBase::kTestingWebpLosslessAlpha);
  ExpectNoWebpSupport(UserAgentMatcherTestBase::kTestingWebpAnimated);

  AcceptWebp();

  // With the header the internal testing user agents stop differing: capability
  // is read off the Accept header, not off the user-agent string.
  ExpectFullWebpSupport(UserAgentMatcherTestBase::kTestingWebp);
  ExpectFullWebpSupport(UserAgentMatcherTestBase::kTestingWebpLosslessAlpha);
  ExpectFullWebpSupport(UserAgentMatcherTestBase::kTestingWebpAnimated);
}

// See https://github.com/apache/incubator-pagespeed-mod/issues/978
//
// Microsoft (v-evgena@microsoft.com and tobint@microsoft.com)
// suggests that they are planning to start masquerading IE11 on
// desktop as Chrome, and wants us not to send webp.  They have not
// coughed up what the UA will actually be, however, so we can't do
// this with a blacklist yet.
TEST_F(DevicePropertiesTest, WebpRequireAcceptHeaderExceptAndroid) {
  // Android Browser is OK -- we will serve it webp without an accept header.
  // Mobile IE actually *does* masquerade as IE as of August 2014, but
  // it's easy to avoid confusion because the UA includes 'Windows Phone'.
  ExpectLegacyRewrittenUrlWebpOnly(
      UserAgentMatcherTestBase::kAndroidICSUserAgent);

  ExpectNoWebpSupport(UserAgentMatcherTestBase::kWindowsPhoneUserAgent);

  // However Chrome will generally send us an Accept:image/webp header, whereas
  // other browsers such as (we think) IE11 in the future, will not send
  // such Accept headers.  Unfortunately, Chrome only started sending
  // accept:image/webp at version 25, so version 18 will no longer get webp
  // as of this change.
  ExpectNoWebpSupport(UserAgentMatcherTestBase::kChrome18UserAgent);
  ExpectNoWebpSupport(UserAgentMatcherTestBase::kAndroidChrome21UserAgent);
  ExpectNoWebpSupport(UserAgentMatcherTestBase::kCriOS48UserAgent);

  // However, Chrome 25 and 37 will get webp due to the accept header.
  AcceptWebp();

  ExpectFullWebpSupport(UserAgentMatcherTestBase::kChrome37UserAgent);
  ExpectFullWebpSupport(UserAgentMatcherTestBase::kOpera1110UserAgent);
  ExpectFullWebpSupport(UserAgentMatcherTestBase::kCriOS48UserAgent);
}

TEST_F(DevicePropertiesTest, WebpUserAgentIdentificationAccept) {
  RequestHeaders headers;
  headers.Add(HttpAttributes::kAccept, "*/*");
  headers.Add(HttpAttributes::kAccept, "image/webp");
  headers.Add(HttpAttributes::kAccept, "text/html");
  device_properties_.ParseRequestHeaders(headers);

  ExpectFullWebpSupport(UserAgentMatcherTestBase::kIe7UserAgent);
  ExpectFullWebpSupport(UserAgentMatcherTestBase::kTestingWebp);
  ExpectFullWebpSupport(UserAgentMatcherTestBase::kTestingWebpLosslessAlpha);
}

// A client that advertises "Accept: image/webp" is taken at its word for every
// WebP flavour, exactly as AVIF has always been handled. No
// hand-maintained browser-version list is consulted any more, so browsers that
// were never on those lists -- Safari most prominently -- now get the full set,
// and a user agent nobody has ever seen gets it too.
TEST_F(DevicePropertiesTest, WebpAcceptGrantsEveryCapabilityRegardlessOfUa) {
  AcceptWebp();

  ExpectFullWebpSupport(UserAgentMatcherTestBase::kSafariUserAgent);
  ExpectFullWebpSupport(UserAgentMatcherTestBase::kSafari9UserAgent);
  ExpectFullWebpSupport(UserAgentMatcherTestBase::kIPhone4Safari);
  ExpectFullWebpSupport(UserAgentMatcherTestBase::kFirefox7UserAgent);
  ExpectFullWebpSupport(UserAgentMatcherTestBase::kNokiaUserAgent);
  ExpectFullWebpSupport(UserAgentMatcherTestBase::kAndroidHCUserAgent);
  ExpectFullWebpSupport("Totally-Unknown-Agent/1.0");
  ExpectFullWebpSupport("");
  // The remaining matrix cell: the legacy no-Accept carve-out user agent, this
  // time WITH the header. Accept wins -- the carve-out only ever adds the lossy
  // tier on top, it never caps a request that advertises WebP.
  ExpectFullWebpSupport(UserAgentMatcherTestBase::kAndroidICSUserAgent);
}

// The other direction, and the one that pins what survives of user-agent
// detection: without the Accept header the user-agent string alone can grant
// nothing beyond the legacy Android carve-out, and never lossless/alpha or
// animated. Chromium and Firefox agents that used to be on the (now deleted)
// lossless and animated allowlists are included deliberately.
TEST_F(DevicePropertiesTest, WebpWithoutAcceptOnlyLegacyAndroidIsRewritten) {
  ExpectLegacyRewrittenUrlWebpOnly(
      UserAgentMatcherTestBase::kAndroidICSUserAgent);

  ExpectNoWebpSupport(UserAgentMatcherTestBase::kChrome137UserAgent);
  ExpectNoWebpSupport(UserAgentMatcherTestBase::kCriOS137UserAgent);
  ExpectNoWebpSupport(UserAgentMatcherTestBase::kChrome37UserAgent);
  ExpectNoWebpSupport(UserAgentMatcherTestBase::kFirefox7UserAgent);
  // Firefox on Android is excluded from the legacy carve-out.
  ExpectNoWebpSupport(UserAgentMatcherTestBase::kFirefox42AndroidUserAgent);
  // Android 3 and older are below the legacy carve-out's floor.
  ExpectNoWebpSupport(UserAgentMatcherTestBase::kAndroidHCUserAgent);
  ExpectNoWebpSupport(UserAgentMatcherTestBase::kSafariUserAgent);
}

TEST_F(DevicePropertiesTest, ProcessSaveDataHeader) {
  ParseAndVerifySaveData("on", true);
  ParseAndVerifySaveData("oN", true);
  ParseAndVerifySaveData("ON", true);
  ParseAndVerifySaveData(nullptr, false);
  ParseAndVerifySaveData("off", false);
  ParseAndVerifySaveData("ofF", false);
  ParseAndVerifySaveData("", false);
  ParseAndVerifySaveData("garbage", false);
}

TEST_F(DevicePropertiesTest, ProcessViaHeader) {
  RequestHeaders headers1;
  DeviceProperties device_properties1(&user_agent_matcher_);
  // This is to verify that unrelated headers don't set HasViaHeader().
  headers1.Add(HttpAttributes::kAccept, "image/webp");
  headers1.Add(HttpAttributes::kAccept, "text/html");
  device_properties1.ParseRequestHeaders(headers1);
  EXPECT_FALSE(device_properties1.HasViaHeader());

  RequestHeaders headers2;
  DeviceProperties device_properties2(&user_agent_matcher_);
  headers2.Add(HttpAttributes::kVia, "1.0 fred, 1.1 example.com (Apache/1.1)");
  device_properties2.ParseRequestHeaders(headers2);
  EXPECT_TRUE(device_properties2.HasViaHeader());

  RequestHeaders headers3;
  DeviceProperties device_properties3(&user_agent_matcher_);
  headers3.Add(HttpAttributes::kVia, "");
  device_properties3.ParseRequestHeaders(headers3);
  EXPECT_TRUE(device_properties3.HasViaHeader());
}

// A stock browser user agent, byte-identical to what a human's browser sends.
// No user-agent list can tell this apart from a human -- that is the whole
// point of the signature override below.
const char kSpoofedBrowserUserAgent[] =
    "Mozilla/5.0 (Macintosh; Intel Mac OS X 10_15_7) AppleWebKit/537.36 "
    "(KHTML, like Gecko) Chrome/121.0.0.0 Safari/537.36";

// Default: no verdict was set, so IsBot() is decided by the user-agent string
// alone, exactly as before.
TEST_F(DevicePropertiesTest, IsBotFallsBackToUserAgentWithoutVerdict) {
  device_properties_.SetUserAgent(kSpoofedBrowserUserAgent);
  EXPECT_FALSE(device_properties_.IsBot());

  DeviceProperties bot_properties(&user_agent_matcher_);
  bot_properties.SetUserAgent("Googlebot/2.1 (+http://www.google.com/bot.html)");
  EXPECT_TRUE(bot_properties.IsBot());
}

// A verified signature outranks the user-agent string: the request is an
// automated client even though its user agent is a real browser's.
TEST_F(DevicePropertiesTest, WebBotAuthVerdictForcesBot) {
  device_properties_.SetUserAgent(kSpoofedBrowserUserAgent);
  device_properties_.SetWebBotAuthVerdict(true);
  EXPECT_TRUE(device_properties_.IsBot());
}

// The override is one-directional. It can only ever move a request from human
// to bot, never the other way: a signature proves bot-ness, never humanity, and
// "no signature material present" is the overwhelmingly common case for bots.
TEST_F(DevicePropertiesTest, WebBotAuthVerdictNeverClearsBotness) {
  device_properties_.SetUserAgent("Googlebot/2.1");
  device_properties_.SetWebBotAuthVerdict(false);
  EXPECT_TRUE(device_properties_.IsBot());

  DeviceProperties human(&user_agent_matcher_);
  human.SetUserAgent(kSpoofedBrowserUserAgent);
  human.SetWebBotAuthVerdict(false);
  EXPECT_FALSE(human.IsBot());
}

// The memoisation trap. SetUserAgent resets is_bot_ along with every other
// user-agent-derived cache, but the signature verdict is a property of the
// request's signature headers, not of the user-agent string -- exactly like
// accepts_avif_. It must survive a later SetUserAgent, in either call order.
TEST_F(DevicePropertiesTest, WebBotAuthVerdictSurvivesSetUserAgent) {
  device_properties_.SetWebBotAuthVerdict(true);
  device_properties_.SetUserAgent(kSpoofedBrowserUserAgent);
  EXPECT_TRUE(device_properties_.IsBot());

  DeviceProperties reversed(&user_agent_matcher_);
  reversed.SetUserAgent(kSpoofedBrowserUserAgent);
  reversed.SetWebBotAuthVerdict(true);
  EXPECT_TRUE(reversed.IsBot());
  // Re-setting the user agent resets is_bot_ along with every other
  // user-agent-derived cache. The verdict is not one of them, so the answer
  // must not change.
  reversed.SetUserAgent(kSpoofedBrowserUserAgent);
  EXPECT_TRUE(reversed.IsBot());
}

// Setting the verdict after IsBot() has already memoised kFalse must still take
// effect. is_bot_ is left holding kFalse here -- nothing clears it -- so this
// passes only because IsBot() tests the override BEFORE reading the memo. Fold
// the override into the memo-miss branch instead and this is the assertion that
// catches it.
TEST_F(DevicePropertiesTest, WebBotAuthVerdictOverridesMemoisedResult) {
  device_properties_.SetUserAgent(kSpoofedBrowserUserAgent);
  EXPECT_FALSE(device_properties_.IsBot());
  device_properties_.SetWebBotAuthVerdict(true);
  EXPECT_TRUE(device_properties_.IsBot());
}

// The signalled-bot consequences a signed agent inherits: no beaconing, no
// lazyload. These are the consumers that make the classification matter.
TEST_F(DevicePropertiesTest, WebBotAuthVerdictSuppressesBeaconing) {
  RequestHeaders headers;
  device_properties_.ParseRequestHeaders(headers);
  device_properties_.SetUserAgent(kSpoofedBrowserUserAgent);
  EXPECT_TRUE(device_properties_.SupportsCriticalImagesBeacon());
  EXPECT_TRUE(device_properties_.SupportsLazyloadImages());

  DeviceProperties signed_agent(&user_agent_matcher_);
  RequestHeaders signed_headers;
  signed_agent.ParseRequestHeaders(signed_headers);
  signed_agent.SetUserAgent(kSpoofedBrowserUserAgent);
  signed_agent.SetWebBotAuthVerdict(true);
  EXPECT_FALSE(signed_agent.SupportsCriticalImagesBeacon());
  EXPECT_FALSE(signed_agent.SupportsLazyloadImages());
}

// defer_javascript retypes every script to text/psajs, which only PageSpeed's
// client-side runtime can execute. A client that does not run JavaScript
// receives an inert page, so bots are excluded at the same seam lazyload uses.
TEST_F(DevicePropertiesTest, JsDeferDisabledForBots) {
  const char* kBotUserAgents[] = {
      UserAgentMatcherTestBase::kGooglebotUserAgent,
      "Mediapartners-Google",
      "Wget/1.21.4",
      "python-requests/2.31.0",
      "Mozilla/5.0 AppleWebKit/537.36 (KHTML, like Gecko); compatible; "
      "ChatGPT-User/1.0; +https://openai.com/bot",
      "curl/8.4.0",
  };
  for (const char* user_agent : kBotUserAgents) {
    SCOPED_TRACE(user_agent);
    DeviceProperties device_properties(&user_agent_matcher_);
    device_properties.SetUserAgent(user_agent);
    ASSERT_TRUE(device_properties.IsBot());
    EXPECT_FALSE(device_properties.SupportsJsDefer(false));

    // allow_mobile is memoised with the result, so re-check on a fresh object.
    DeviceProperties mobile_allowed(&user_agent_matcher_);
    mobile_allowed.SetUserAgent(user_agent);
    EXPECT_FALSE(mobile_allowed.SupportsJsDefer(true));
  }
}

// Ordinary desktop browsers are untouched: this is the population the filter
// was written for and the only one that can run the deferral runtime.
TEST_F(DevicePropertiesTest, JsDeferAllowedForBrowsers) {
  const char* kBrowserUserAgents[] = {
      UserAgentMatcherTestBase::kChromeUserAgent,
      UserAgentMatcherTestBase::kChrome18UserAgent,
      UserAgentMatcherTestBase::kFirefoxUserAgent,
      UserAgentMatcherTestBase::kSafariUserAgent,
  };
  for (const char* user_agent : kBrowserUserAgents) {
    SCOPED_TRACE(user_agent);
    DeviceProperties device_properties(&user_agent_matcher_);
    device_properties.SetUserAgent(user_agent);
    ASSERT_FALSE(device_properties.IsBot());
    EXPECT_TRUE(device_properties.SupportsJsDefer(false));
  }
}

// Behaviour change, pinned deliberately. UserAgentMatcher::SupportsJsDefer
// returns true for an empty user agent (user_agent_matcher.cc), but a client
// that sends no user agent at all is a bot under BotChecker and is the least
// plausible thing on the internet to execute a deferral runtime.
TEST_F(DevicePropertiesTest, JsDeferDisabledForEmptyUserAgent) {
  device_properties_.SetUserAgent("");
  EXPECT_TRUE(user_agent_matcher_.SupportsJsDefer("", false));
  EXPECT_TRUE(device_properties_.IsBot());
  EXPECT_FALSE(device_properties_.SupportsJsDefer(false));
}

// Ties the Web Bot Auth verdict to this consumer: an agent presenting a
// byte-identical stock-Chrome user agent is invisible to every user-agent list,
// so only a verified signature can classify it. When one is present, defer is
// withheld along with the beacons and lazyload.
TEST_F(DevicePropertiesTest, JsDeferDisabledByWebBotAuthVerdict) {
  device_properties_.SetUserAgent(kSpoofedBrowserUserAgent);
  EXPECT_TRUE(device_properties_.SupportsJsDefer(false));

  DeviceProperties signed_agent(&user_agent_matcher_);
  signed_agent.SetUserAgent(kSpoofedBrowserUserAgent);
  signed_agent.SetWebBotAuthVerdict(true);
  EXPECT_FALSE(signed_agent.SupportsJsDefer(false));
}

}  // namespace net_instaweb
