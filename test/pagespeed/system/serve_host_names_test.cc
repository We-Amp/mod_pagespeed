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

// The host a serve is recorded under, and the host-name rule it is checked
// against.  The shapes are the ones a real server meets: every spelling of a
// name a client can send, address literals, and the names server
// configurations use that are not host names (wildcards, patterns, "_").

#include "pagespeed/system/serve_host_names.h"

#include <utility>
#include <vector>

#include "pagespeed/kernel/base/string.h"
#include "pagespeed/kernel/base/string_util.h"
#include "test/pagespeed/kernel/base/gtest.h"

namespace net_instaweb {
namespace {

ConfiguredHostNames Names(const char* primary,
                          std::vector<GoogleString> aliases = {}) {
  ConfiguredHostNames names;
  names.primary = primary;
  names.aliases = std::move(aliases);
  return names;
}

TEST(ServeHostNamesTest, NormalizesLikeTheOptimizer) {
  struct Case {
    GoogleString in;
    GoogleString out;
  };
  const std::vector<Case> cases = {
      {"www.example.com", "www.example.com"},
      {"WWW.Example.COM", "www.example.com"},
      {"www.example.com.", "www.example.com"},
      {"www.example.com:8443", "www.example.com"},
      {"www.example.com.:443", "www.example.com"},
      {"a_b.test", "a_b.test"},
      {"10.1.2.3", "10.1.2.3"},
      {"10.1.2.3:80", "10.1.2.3"},
      {"[::1]", "[::1]"},
      {"[2001:db8::1]", "[2001:db8::1]"},
      {"[2001:DB8::1]:80", "[2001:db8::1]"},
      {GoogleString(127, 'a'), GoogleString(127, 'a')},
  };
  for (const Case& c : cases) {
    GoogleString out;
    EXPECT_TRUE(NormalizeServeHostName(c.in, &out)) << c.in;
    EXPECT_EQ(c.out, out) << c.in;
  }
}

TEST(ServeHostNamesTest, RefusesWhatTheOptimizerRefuses) {
  const std::vector<GoogleString> refused = {
      "",
      "_",
      ".",
      "..",
      "-",
      "[:]",
      "[]",
      "www.example.com..",
      GoogleString(128, 'a'),
      GoogleString(300, 'a'),
      "[::1]x",
      "[zz::1]",
      "[::1",
      "[::1].",
      "a[b].test",
      "a]b.test",
      "::1",
      "2001:db8::1",
      "example.com:",
      "example.com:123456",
      "bad host",
      " a.test",
      "a\"b.test",
      "<img src=x>",
      "caf\xc3\xa9.test",
      "*.example.test",
      "www.example.*",
      "~^www\\d+\\.example\\.test$",
      "(other)",
      GoogleString("a\0b.test", 8),
  };
  for (const GoogleString& host : refused) {
    GoogleString out = "stale";
    EXPECT_FALSE(NormalizeServeHostName(host, &out)) << host;
    EXPECT_EQ("", out) << host;
  }
}

TEST(ServeHostNamesTest, ExactNamesMatchInEveryForm) {
  const ConfiguredHostNames names =
      Names("www.example.test", {"Static.Example.TEST", "example.test"});
  EXPECT_EQ("www.example.test", VouchedServeHost("www.example.test", names));
  EXPECT_EQ("www.example.test", VouchedServeHost("WWW.Example.Test", names));
  EXPECT_EQ("static.example.test",
            VouchedServeHost("static.example.test:8443", names));
  EXPECT_EQ("static.example.test",
            VouchedServeHost("STATIC.example.test.", names));
  EXPECT_EQ("example.test", VouchedServeHost("example.test.:80", names));
}

TEST(ServeHostNamesTest, AWildcardMatchedHostCountsUnderThePrimaryName) {
  // The server matched the request through a wildcard; the wildcard itself
  // is never a name, and the host it matched is not one either.  (nginx's
  // regular-expression entries never reach this rule -- nginx stores them
  // without their "~", so its adapter drops them; the "~" spelling here is
  // still refused by the rule.)
  const ConfiguredHostNames names = Names(
      "www.example.test",
      {"*.example.test", "www.example.*", "~^shop\\d+\\.example\\.test$"});
  EXPECT_EQ("www.example.test", VouchedServeHost("shop.example.test", names));
  EXPECT_EQ("www.example.test", VouchedServeHost("shop1.example.test", names));
  EXPECT_EQ("www.example.test", VouchedServeHost("www.example.org", names));
}

TEST(ServeHostNamesTest, TheDefaultSiteCountsAnUnknownHostUnderItsName) {
  const ConfiguredHostNames names = Names("www.example.test");
  EXPECT_EQ("www.example.test", VouchedServeHost("unknown.test", names));
  EXPECT_EQ("www.example.test", VouchedServeHost("", names));
}

TEST(ServeHostNamesTest, AVisitorChosenHostIsNeverTheRow) {
  // Whatever a client sends -- through a catch-all site, an absolute-form
  // request target or a raw Host header -- the row is the site's own name.
  const ConfiguredHostNames names = Names("www.example.test");
  const std::vector<GoogleString> chosen = {
      "attacker.test",    "ATTACKER.TEST:8080", "attacker.test.",
      "[::1]",            "[2001:db8::1]:443",  "10.1.2.3",
      "a_b.test",         GoogleString(300, 'a'),
      "<img src=x>",      "www.example.test.attacker.test",
      "example.test",     "w.example.test",
  };
  for (const GoogleString& host : chosen) {
    EXPECT_EQ("www.example.test", VouchedServeHost(host, names)) << host;
  }
  // Without a usable name of its own, the site records no host at all.
  for (const GoogleString& host : chosen) {
    EXPECT_EQ("", VouchedServeHost(host, Names("_"))) << host;
  }
}

TEST(ServeHostNamesTest, AnUnusablePrimaryNameRecordsNoHost) {
  for (const char* primary :
       {"", "_", "*.example.test", "www.example.*", "~^www\\d+$",
        ".example.test", "bad host", "localhost..", "[zz::1]"}) {
    EXPECT_EQ("", VouchedServeHost("www.example.test", Names(primary)))
        << primary;
    EXPECT_EQ("", VouchedServeHost("", Names(primary))) << primary;
  }
}

TEST(ServeHostNamesTest, AnExactAliasIsVouchedForWithoutAUsablePrimaryName) {
  // nginx: "server_name _ www.example.test;" -- the alias is exact, "_" is
  // not a name.
  const ConfiguredHostNames names = Names("_", {"_", "www.example.test"});
  EXPECT_EQ("www.example.test", VouchedServeHost("WWW.example.test.", names));
  EXPECT_EQ("", VouchedServeHost("other.test", names));
}

TEST(ServeHostNamesTest, ASuffixWildcardIsNotAnExactName) {
  // nginx's ".example.test" also matches every subdomain, so it vouches for
  // no request host; nginx itself reports such a block's primary name
  // without the dot, which is a usable name.
  EXPECT_EQ("", VouchedServeHost("example.test",
                                  Names("_", {".example.test"})));
  const ConfiguredHostNames nginx_spelling =
      Names("example.test", {".example.test"});
  EXPECT_EQ("example.test",
            VouchedServeHost("example.test", nginx_spelling));
  EXPECT_EQ("example.test",
            VouchedServeHost("shop.example.test", nginx_spelling));
}

TEST(ServeHostNamesTest, AddressLiteralNames) {
  EXPECT_EQ("[2001:db8::1]",
            VouchedServeHost("[2001:DB8::1]:443", Names("[2001:db8::1]")));
  EXPECT_EQ("10.1.2.3", VouchedServeHost("10.1.2.3:80", Names("10.1.2.3")));
  EXPECT_EQ("10.1.2.3", VouchedServeHost("[::1]", Names("10.1.2.3")));
}

}  // namespace
}  // namespace net_instaweb
