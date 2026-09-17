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

// Unit-test the caching fetcher, using a mock fetcher, and an async
// wrapper around that.

#include "pagespeed/kernel/http/bot_checker.h"

#include "test/pagespeed/kernel/base/gtest.h"

namespace net_instaweb {

class BotCheckerTest : public testing::Test {};

TEST_F(BotCheckerTest, DetectUserAgents) {
  // Spider.
  EXPECT_TRUE(BotChecker::Lookup(
      "Baiduspider+(+http://www.baidu.com/search/spider.htm)"));
  EXPECT_TRUE(
      BotChecker::Lookup("Baiduspider+(+http://help.baidu.jp/system/05.html)"));
  EXPECT_TRUE(BotChecker::Lookup(
      "BaidusSpIDER+(+http://help.baidu.jp/system/05.html)"));

  // Bot.
  EXPECT_TRUE(BotChecker::Lookup("msnbot-UDiscovery/2.0b"));
  EXPECT_TRUE(
      BotChecker::Lookup("Mozilla/5.0 (compatible; bingbot/2.0;"
                         "+http://www.bing.com/bingbot.htm)"));
  EXPECT_TRUE(BotChecker::Lookup("bitlybot"));
  EXPECT_TRUE(BotChecker::Lookup("bitlyBoT"));

  // Crawl.
  EXPECT_TRUE(BotChecker::Lookup("CrAwLER"));

  // In map.
  EXPECT_TRUE(BotChecker::Lookup("Mediapartners-Google"));
  EXPECT_TRUE(
      BotChecker::Lookup("Mozilla/5.0 (compatible; Yahoo! Slurp;"
                         "http://help.yahoo.com/help/us/ysearch/slurp)"));
  EXPECT_TRUE(BotChecker::Lookup(
      "Mozilla/5.0 (compatible; Ask Jeeves/Teoma;"
      "+http://about.ask.com/en/docs/about/webmasters.shtml"));

  // Empty.
  EXPECT_TRUE(BotChecker::Lookup(""));

  // Wget and curl are now classified as bots. This deliberately reverses the
  // original EXPECT_FALSE assertion on Wget: both are non-rendering automation
  // clients, and the cost of the two error directions is asymmetric. A bot
  // misread as a human executes the instrumentation and critical-image beacons
  // and writes a degenerate critical set into the shared, per-URL property
  // cache, which is then served to real visitors until it ages out. A human
  // misread as a bot only loses lazyload_images on that request; nothing
  // renders wrong. Poisoning shared data is the expensive direction, so
  // non-rendering clients are classified as bots.
  EXPECT_TRUE(BotChecker::Lookup("Wget/1.12 (linux-gnu)"));
  EXPECT_TRUE(BotChecker::Lookup("curl/8.6.0"));

  EXPECT_FALSE(BotChecker::Lookup(
      "Mozilla/5.0 (X11; Linux x86_64) AppleWebKit/537.36 (KHTML, like Gecko) "
      "Chrome/28.0.1500.71 Safari/537.36"));
}

// User-triggered agent fetchers and agent infrastructure that carry no
// bot/crawl/spider substring, so the substring rule in BotChecker::Lookup
// cannot see them; they only match via the exact-token gperf table. These are
// the vendors' published user agents, so they also exercise the tokenizer on
// the punctuation those strings actually contain.
TEST_F(BotCheckerTest, DetectsAgentEraFetchers) {
  // Claude-User, as published: the token sits inside a parenthesised comment
  // that also ends the string, so both the ';' and the ')' split points matter.
  EXPECT_TRUE(BotChecker::Lookup(
      "Mozilla/5.0 (Linux; Android 6.0.1; Nexus 5X Build/MMB29P) "
      "AppleWebKit/537.36 (KHTML, like Gecko) Chrome/120.0.0.0 Mobile "
      "Safari/537.36 (compatible; Claude-User/1.0; "
      "+Claude-User@anthropic.com)"));
  EXPECT_TRUE(BotChecker::Lookup(
      "Mozilla/5.0 (Macintosh; Intel Mac OS X 10_15_7) AppleWebKit/537.36 "
      "(KHTML, like Gecko) Chrome/120.0.0.0 Safari/537.36; Claude-Web/1.0"));
  EXPECT_TRUE(BotChecker::Lookup(
      "Mozilla/5.0 (Macintosh; Intel Mac OS X 10_15_7) AppleWebKit/537.36 "
      "(KHTML, like Gecko) Chrome/120.0.0.0 Safari/537.36; "
      "Perplexity-User/1.0; +https://perplexity.ai/perplexity-user"));
  EXPECT_TRUE(BotChecker::Lookup("Mozilla/5.0 (compatible; ChatGPT-User/1.0)"));
  EXPECT_TRUE(BotChecker::Lookup("anthropic-ai"));
  EXPECT_TRUE(BotChecker::Lookup("cohere-ai"));
  // GoogleOther is published both bare and in the compatible-comment form. The
  // second is the shape that needs ')' to be a split point: without it the
  // final token is "GoogleOther)" and no exact entry can ever match.
  EXPECT_TRUE(BotChecker::Lookup("GoogleOther"));
  EXPECT_TRUE(BotChecker::Lookup("Mozilla/5.0 (compatible; GoogleOther)"));
}

// The tokenizer must treat parentheses as separators. Practically every
// browser-shaped user agent carries its identifying token inside a
// parenthesised comment, so a token that opens or closes one is the common
// case, not the exotic one. Each string below has its agent token adjacent to
// a parenthesis; none of them contains bot, crawl or spider, so the token table
// is the only thing that can match, and it can only match if the split points
// include '(' and ')'.
TEST_F(BotCheckerTest, TokenizesParenthesisedUserAgentComments) {
  EXPECT_TRUE(BotChecker::Lookup("Mozilla/5.0 (compatible; GoogleOther)"));
  EXPECT_TRUE(BotChecker::Lookup("Mozilla/5.0 (GoogleOther)"));
  EXPECT_TRUE(BotChecker::Lookup("Mozilla/5.0 (compatible; Claude-Web)"));
  EXPECT_TRUE(BotChecker::Lookup("Mozilla/5.0 (Perplexity-User)"));
  EXPECT_TRUE(BotChecker::Lookup("Mozilla/5.0 (compatible; cohere-ai)"));
  EXPECT_TRUE(BotChecker::Lookup("Mozilla/5.0 (anthropic-ai)"));
}

// Agents the case-insensitive bot/crawl/spider substring rule already catches.
// They are pinned here, not added to the token table: a duplicate entry would
// be dead weight that reads as coverage, and for the Meta agents an entry would
// additionally have to guess the wire casing. Both published Meta user agents
// end in the crawler documentation URL, so "crawl" matches them.
TEST_F(BotCheckerTest, SubstringRuleAlreadyCoversTheseAgents) {
  EXPECT_TRUE(BotChecker::Lookup(
      "meta-externalagent/1.1 (+https://developers.facebook.com/docs/sharing/"
      "webmasters/crawler)"));
  EXPECT_TRUE(BotChecker::Lookup(
      "meta-externalfetcher/1.1 (+https://developers.facebook.com/docs/sharing/"
      "webmasters/crawler)"));
  // ChatGPT-User as published carries a URL containing "bot"; the token table
  // covers the form without it (see DetectsAgentEraFetchers above).
  EXPECT_TRUE(BotChecker::Lookup(
      "Mozilla/5.0 AppleWebKit/537.36 (KHTML, like Gecko); compatible; "
      "ChatGPT-User/1.0; +https://openai.com/bot"));
  EXPECT_TRUE(BotChecker::Lookup("Applebot-Extended/1.0"));
  EXPECT_TRUE(BotChecker::Lookup(
      "Mozilla/5.0 AppleWebKit/537.36 (KHTML, like Gecko); compatible; "
      "GPTBot/1.2; +https://openai.com/gptbot"));
}

// Modern generic HTTP clients -- the post-2015 analogues of the
// Apache-HttpClient / Python-urllib / Ruby / PHP entries the table has carried
// since 2013. None of them render, so none of them should beacon.
TEST_F(BotCheckerTest, DetectsModernHttpClients) {
  EXPECT_TRUE(BotChecker::Lookup("Go-http-client/2.0"));
  EXPECT_TRUE(BotChecker::Lookup(
      "node-fetch/1.0 (+https://github.com/bitinn/node-fetch)"));
  EXPECT_TRUE(BotChecker::Lookup("python-requests/2.31.0"));
  EXPECT_TRUE(BotChecker::Lookup("axios/1.6.7"));
  EXPECT_TRUE(BotChecker::Lookup("okhttp/4.12.0"));
  EXPECT_TRUE(BotChecker::Lookup("undici/6.6.2"));
  EXPECT_TRUE(BotChecker::Lookup("Scrapy/2.11.0 (+https://scrapy.org)"));
  // Bare aiohttp: the aiohttp default user agent is "Python/3.11 aiohttp/3.9.3"
  // and the leading "Python" token already matched before this change, so the
  // bare form is what actually exercises the new "aiohttp" entry.
  EXPECT_TRUE(BotChecker::Lookup("aiohttp/3.9.3"));
  EXPECT_TRUE(BotChecker::Lookup("libwww-perl/6.68"));
  EXPECT_TRUE(BotChecker::Lookup(
      "Mozilla/5.0 (X11; Linux x86_64) AppleWebKit/537.36 (KHTML, like Gecko) "
      "HeadlessChrome/120.0.0.0 Safari/537.36"));
}

// The gperf table is compiled with %compare-strncmp and is therefore
// case-sensitive; only the bot/crawl/spider substring rule ignores case. This
// pins that asymmetry so a future "just add it lowercase" edit cannot silently
// assume otherwise.
TEST_F(BotCheckerTest, TokenTableIsCaseSensitive) {
  EXPECT_FALSE(BotChecker::Lookup("chatgpt-user/1.0"));
  EXPECT_FALSE(BotChecker::Lookup("claude-user/1.0"));
  EXPECT_FALSE(BotChecker::Lookup("googleother"));
  EXPECT_FALSE(BotChecker::Lookup("CURL/8.6.0"));
  EXPECT_FALSE(BotChecker::Lookup("WGET/1.21.4"));
}

// Table lookups match a WHOLE token, never a prefix of one. gperf's generated
// lookup compares the full length and then checks the stored entry ends there,
// so a longer product token that merely starts with a listed one is not a
// match. This is what keeps short entries like "curl" and "axios" from
// spraying false positives across unrelated product names.
TEST_F(BotCheckerTest, TokenTableMatchesWholeTokensNotPrefixes) {
  EXPECT_FALSE(BotChecker::Lookup("curlywurly/1.0"));
  EXPECT_FALSE(BotChecker::Lookup("axiosly/2.0"));
  EXPECT_FALSE(BotChecker::Lookup("undicious/1.0"));
  EXPECT_FALSE(BotChecker::Lookup("Scrapyard/1.0"));
}

// False-positive canaries. Real browser user agents must stay out of the bot
// class: the token table is exact-match and case-sensitive precisely so that
// widening it cannot reach them.
TEST_F(BotCheckerTest, RealBrowsersAreNotBots) {
  // Chrome desktop.
  EXPECT_FALSE(BotChecker::Lookup(
      "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, "
      "like Gecko) Chrome/121.0.0.0 Safari/537.36"));
  // Edge desktop.
  EXPECT_FALSE(BotChecker::Lookup(
      "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, "
      "like Gecko) Chrome/121.0.0.0 Safari/537.36 Edg/121.0.0.0"));
  // Firefox desktop.
  EXPECT_FALSE(BotChecker::Lookup(
      "Mozilla/5.0 (Windows NT 10.0; Win64; x64; rv:123.0) Gecko/20100101 "
      "Firefox/123.0"));
  // Safari macOS.
  EXPECT_FALSE(BotChecker::Lookup(
      "Mozilla/5.0 (Macintosh; Intel Mac OS X 10_15_7) AppleWebKit/605.1.15 "
      "(KHTML, like Gecko) Version/17.3 Safari/605.1.15"));
  // Safari iOS.
  EXPECT_FALSE(BotChecker::Lookup(
      "Mozilla/5.0 (iPhone; CPU iPhone OS 17_3 like Mac OS X) "
      "AppleWebKit/605.1.15 (KHTML, like Gecko) Version/17.3 Mobile/15E148 "
      "Safari/604.1"));
  // Chrome Android.
  EXPECT_FALSE(BotChecker::Lookup(
      "Mozilla/5.0 (Linux; Android 14; Pixel 8) AppleWebKit/537.36 (KHTML, "
      "like Gecko) Chrome/121.0.0.0 Mobile Safari/537.36"));
}

}  // namespace net_instaweb
