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

// What a per-host admin console may see of the optimizer's statistics: at
// most its own site's row of the serve savings per host, the rest folded
// into "other" with exact 64-bit sums, every other key kept -- and nothing
// at all of an answer the module cannot read strictly.

#include "pagespeed/system/daemon_site_filter.h"

#include <memory>
#include <vector>

#include "pagespeed/kernel/base/json.h"
#include "pagespeed/kernel/base/string.h"
#include "pagespeed/kernel/base/string_util.h"
#include "test/pagespeed/kernel/base/gtest.h"

namespace net_instaweb {
namespace {

// Two sites' rows and a third's, as the optimizer writes them (compact,
// keys sorted).
constexpr char kStats[] =
    "{\"cache\":{\"entries\":450,\"size_bytes\":314572800},"
    "\"serve_savings\":{\"css\":{\"hits\":10,\"optimized_bytes\":1000,"
    "\"original_bytes\":2900}},"
    "\"serve_savings_by_host\":{\"hosts\":["
    "{\"hits\":5,\"host\":\"www.example.test\",\"optimized_bytes\":500,"
    "\"original_bytes\":1000},"
    "{\"hits\":3,\"host\":\"static.example.test\",\"optimized_bytes\":300,"
    "\"original_bytes\":900},"
    "{\"hits\":2,\"host\":\"other-site.test\",\"optimized_bytes\":200,"
    "\"original_bytes\":800}],"
    "\"limit\":32,"
    "\"other\":{\"hits\":1,\"optimized_bytes\":100,\"original_bytes\":200}},"
    "\"started_at_ms\":1759230000000,\"syscall_filter\":\"unknown\","
    "\"uptime_seconds\":3600}";

Json::Value Parse(StringPiece text) {
  Json::CharReaderBuilder builder;
  std::unique_ptr<Json::CharReader> reader(builder.newCharReader());
  Json::Value root;
  std::string errors;
  EXPECT_TRUE(
      reader->parse(text.data(), text.data() + text.size(), &root, &errors))
      << errors << "\n" << text;
  return root;
}

// A stats document whose by-host block is `block` (JSON text).
GoogleString WithBlock(StringPiece block) {
  return StrCat("{\"cache\":{\"entries\":1},\"serve_savings_by_host\":", block,
                ",\"uptime_seconds\":7}");
}

GoogleString Filtered(StringPiece body, StringPiece own_host) {
  GoogleString out;
  EXPECT_TRUE(FilterStatsForSite(body, own_host, &out)) << body;
  return out;
}

TEST(DaemonSiteFilterTest, KeepsOnlyTheOwnRowAndFoldsTheRest) {
  const GoogleString out = Filtered(kStats, "www.example.test");
  const Json::Value block = Parse(out)["serve_savings_by_host"];
  ASSERT_EQ(1u, block["hosts"].size());
  EXPECT_EQ("www.example.test", block["hosts"][0]["host"].asString());
  EXPECT_EQ(5u, block["hosts"][0]["hits"].asUInt64());
  EXPECT_EQ(1000u, block["hosts"][0]["original_bytes"].asUInt64());
  EXPECT_EQ(500u, block["hosts"][0]["optimized_bytes"].asUInt64());
  EXPECT_EQ(1u + 3 + 2, block["other"]["hits"].asUInt64());
  EXPECT_EQ(200u + 900 + 800, block["other"]["original_bytes"].asUInt64());
  EXPECT_EQ(100u + 300 + 200, block["other"]["optimized_bytes"].asUInt64());
  EXPECT_EQ(32u, block["limit"].asUInt64());
  EXPECT_EQ("www.example.test", block["site"].asString());
  EXPECT_EQ(GoogleString::npos, out.find("static.example.test"));
  EXPECT_EQ(GoogleString::npos, out.find("other-site.test"));
}

TEST(DaemonSiteFilterTest, WithoutAnOwnRowEveryRowIsFolded) {
  for (const char* own : {"nothing.example.test", ""}) {
    const GoogleString out = Filtered(kStats, own);
    const Json::Value block = Parse(out)["serve_savings_by_host"];
    EXPECT_EQ(0u, block["hosts"].size()) << own;
    EXPECT_EQ(11u, block["other"]["hits"].asUInt64()) << own;
    EXPECT_EQ(2900u, block["other"]["original_bytes"].asUInt64()) << own;
    // The marker names the site the console was given, "" for none.
    EXPECT_TRUE(block["site"].isString()) << own;
    EXPECT_EQ(own, block["site"].asString()) << own;
    EXPECT_EQ(GoogleString::npos, out.find("www.example.test")) << own;
    EXPECT_EQ(GoogleString::npos, out.find("static.example.test")) << own;
    EXPECT_EQ(GoogleString::npos, out.find("other-site.test")) << own;
  }
}

TEST(DaemonSiteFilterTest, KeepsEveryOtherKeyAndValue) {
  // Every top-level key survives with its value -- integers of every size,
  // a fraction, a negative, nesting, text with escapes and non-ASCII.
  const GoogleString body =
      "{\"big\":18446744073709551615,\"fraction\":0.1,\"list\":[1,2,"
      "{\"a\":\"b\"}],\"negative\":-5,\"serve_savings_by_host\":{\"hosts\":[],"
      "\"limit\":32,\"other\":{\"hits\":0,\"optimized_bytes\":0,"
      "\"original_bytes\":0}},\"text\":\"caf\\u00e9 \\\"quoted\\\"\","
      "\"zeta\":{\"nested\":{\"deep\":true,\"none\":null}}}";
  const Json::Value before = Parse(body);
  const Json::Value after = Parse(Filtered(body, "www.example.test"));
  EXPECT_EQ(before.getMemberNames(), after.getMemberNames());
  for (const std::string& key : before.getMemberNames()) {
    if (key == "serve_savings_by_host") continue;
    EXPECT_TRUE(before[key] == after[key]) << key;
  }
  EXPECT_EQ(18446744073709551615u, after["big"].asUInt64());
}

TEST(DaemonSiteFilterTest, AnAnswerKeepsTheOptimizersKeyOrderAndSpacing) {
  // The optimizer writes compact JSON with sorted keys; where nothing is
  // folded, the per-host answer is the optimizer's bytes plus the marker,
  // which sorts after "other".
  const GoogleString body =
      "{\"a\":1,\"serve_savings_by_host\":{\"hosts\":[{\"hits\":5,\"host\":"
      "\"www.example.test\",\"optimized_bytes\":500,\"original_bytes\":1000}],"
      "\"limit\":32,\"other\":{\"hits\":0,\"optimized_bytes\":0,"
      "\"original_bytes\":0}},\"z\":\"x\"}";
  const GoogleString expected =
      "{\"a\":1,\"serve_savings_by_host\":{\"hosts\":[{\"hits\":5,\"host\":"
      "\"www.example.test\",\"optimized_bytes\":500,\"original_bytes\":1000}],"
      "\"limit\":32,\"other\":{\"hits\":0,\"optimized_bytes\":0,"
      "\"original_bytes\":0},\"site\":\"www.example.test\"},\"z\":\"x\"}";
  EXPECT_EQ(expected, Filtered(body, "www.example.test"));
}

TEST(DaemonSiteFilterTest, AnUpstreamSiteMarkerIsReplaced) {
  // The marker is the module's statement, never the optimizer's: an
  // upstream "site" naming another host is replaced, also by "".
  const GoogleString body = WithBlock(
      "{\"hosts\":[{\"hits\":1,\"host\":\"www.example.test\","
      "\"optimized_bytes\":1,\"original_bytes\":2}],\"limit\":32,"
      "\"other\":{\"hits\":0,\"optimized_bytes\":0,\"original_bytes\":0},"
      "\"site\":\"evil.test\"}");
  for (const char* own : {"www.example.test", ""}) {
    const GoogleString out = Filtered(body, own);
    EXPECT_EQ(own, Parse(out)["serve_savings_by_host"]["site"].asString())
        << own;
    EXPECT_EQ(GoogleString::npos, out.find("evil.test")) << own;
  }
}

TEST(DaemonSiteFilterTest, NonAsciiTextIsWrittenAsEscapes) {
  // The per-host answer is ASCII whatever bytes the optimizer sent; the
  // values are unchanged after parsing.
  const GoogleString body =
      "{\"note\":\"caf\xc3\xa9 \xe2\x82\xac\",\"serve_savings_by_host\":"
      "{\"hosts\":[],\"limit\":32,\"other\":{\"hits\":0,"
      "\"optimized_bytes\":0,\"original_bytes\":0}}}";
  const GoogleString out = Filtered(body, "www.example.test");
  for (const char c : out) {
    EXPECT_LT(static_cast<unsigned char>(c), 0x80u) << out;
  }
  EXPECT_NE(GoogleString::npos, out.find("\\u00e9")) << out;
  EXPECT_EQ(Parse(body)["note"].asString(), Parse(out)["note"].asString());
}

TEST(DaemonSiteFilterTest, RepeatedOwnRowsAreSummedIntoOneRow) {
  const GoogleString out = Filtered(
      WithBlock("{\"hosts\":[{\"hits\":2,\"host\":\"www.example.test\","
                "\"optimized_bytes\":20,\"original_bytes\":40},"
                "{\"hits\":3,\"host\":\"www.example.test\","
                "\"optimized_bytes\":30,\"original_bytes\":60}],\"limit\":32,"
                "\"other\":{\"hits\":0,\"optimized_bytes\":0,"
                "\"original_bytes\":0}}"),
      "www.example.test");
  const Json::Value block = Parse(out)["serve_savings_by_host"];
  ASSERT_EQ(1u, block["hosts"].size());
  EXPECT_EQ(5u, block["hosts"][0]["hits"].asUInt64());
  EXPECT_EQ(100u, block["hosts"][0]["original_bytes"].asUInt64());
}

TEST(DaemonSiteFilterTest, FoldKeepsTheTotalsWithLargeCounters) {
  const GoogleString out = Filtered(
      WithBlock("{\"hosts\":[{\"hits\":9223372036854775808,\"host\":"
                "\"www.example.test\",\"optimized_bytes\":1,"
                "\"original_bytes\":18446744073709551615},"
                "{\"hits\":1,\"host\":\"other-site.test\","
                "\"optimized_bytes\":1,\"original_bytes\":1}],\"limit\":32,"
                "\"other\":{\"hits\":18446744073709551614,"
                "\"optimized_bytes\":0,\"original_bytes\":0}}"),
      "www.example.test");
  const Json::Value block = Parse(out)["serve_savings_by_host"];
  EXPECT_EQ(9223372036854775808u, block["hosts"][0]["hits"].asUInt64());
  EXPECT_EQ(18446744073709551615u,
            block["hosts"][0]["original_bytes"].asUInt64());
  EXPECT_EQ(18446744073709551615u, block["other"]["hits"].asUInt64());
  EXPECT_NE(GoogleString::npos, out.find("18446744073709551615"));
}

TEST(DaemonSiteFilterTest, AFoldThatWouldOverflowRemovesTheBlock) {
  const GoogleString out = Filtered(
      WithBlock("{\"hosts\":[{\"hits\":1,\"host\":\"other-site.test\","
                "\"optimized_bytes\":1,\"original_bytes\":1}],\"limit\":32,"
                "\"other\":{\"hits\":18446744073709551615,"
                "\"optimized_bytes\":0,\"original_bytes\":0}}"),
      "www.example.test");
  const Json::Value root = Parse(out);
  EXPECT_FALSE(root.isMember("serve_savings_by_host"));
  EXPECT_EQ(1u, root["cache"]["entries"].asUInt64());
  EXPECT_EQ(GoogleString::npos, out.find("other-site.test"));
}

TEST(DaemonSiteFilterTest, MalformedBlockIsRemovedWhole) {
  const char kOther[] =
      "\"other\":{\"hits\":0,\"optimized_bytes\":0,\"original_bytes\":0}";
  const GoogleString row_tail = ",\"optimized_bytes\":1,\"original_bytes\":1}";
  const std::vector<GoogleString> blocks = {
      "\"evil.test\"",
      "[\"evil.test\"]",
      "{}",
      StrCat("{\"hosts\":{\"evil.test\":1},\"limit\":32,", kOther, "}"),
      StrCat("{\"hosts\":[{\"hits\":1,\"host\":7", row_tail,
             "],\"limit\":32,", kOther, "}"),
      StrCat("{\"hosts\":[{\"hits\":1", row_tail, "],\"limit\":32,", kOther,
             "}"),
      StrCat("{\"hosts\":[\"evil.test\"],\"limit\":32,", kOther, "}"),
      StrCat("{\"hosts\":[{\"hits\":-1,\"host\":\"evil.test\"", row_tail,
             "],\"limit\":32,", kOther, "}"),
      StrCat("{\"hosts\":[{\"hits\":1.5,\"host\":\"evil.test\"", row_tail,
             "],\"limit\":32,", kOther, "}"),
      StrCat("{\"hosts\":[{\"hits\":1e3,\"host\":\"evil.test\"", row_tail,
             "],\"limit\":32,", kOther, "}"),
      StrCat("{\"hosts\":[{\"hits\":\"7\",\"host\":\"evil.test\"", row_tail,
             "],\"limit\":32,", kOther, "}"),
      StrCat("{\"hosts\":[{\"hits\":18446744073709551616,\"host\":"
             "\"evil.test\"",
             row_tail, "],\"limit\":32,", kOther, "}"),
      StrCat("{\"hosts\":[],\"limit\":-1,", kOther, "}"),
      StrCat("{\"hosts\":[],\"limit\":\"32\",", kOther, "}"),
      "{\"hosts\":[{\"hits\":1,\"host\":\"evil.test\",\"optimized_bytes\":1,"
      "\"original_bytes\":1}],\"limit\":32}",
      "{\"hosts\":[],\"limit\":32,\"other\":{\"hits\":null,"
      "\"optimized_bytes\":0,\"original_bytes\":0}}",
  };
  for (const GoogleString& block : blocks) {
    const GoogleString out = Filtered(WithBlock(block), "evil.test");
    const Json::Value root = Parse(out);
    EXPECT_FALSE(root.isMember("serve_savings_by_host")) << block;
    EXPECT_EQ(1u, root["cache"]["entries"].asUInt64()) << block;
    EXPECT_EQ(7u, root["uptime_seconds"].asUInt64()) << block;
    EXPECT_EQ(GoogleString::npos, out.find("evil.test")) << block;
    // A removed block carries no marker: no block, no row.
    EXPECT_EQ(GoogleString::npos, out.find("\"site\"")) << block;
  }
}

TEST(DaemonSiteFilterTest, UnknownKeysInsideTheBlockAreDropped) {
  const GoogleString out = Filtered(
      WithBlock("{\"hosts\":[{\"alias\":\"evil.test\",\"hits\":1,\"host\":"
                "\"www.example.test\",\"optimized_bytes\":1,"
                "\"original_bytes\":2}],\"limit\":32,\"names\":[\"evil.test\"],"
                "\"other\":{\"hits\":0,\"note\":\"evil.test\","
                "\"optimized_bytes\":0,\"original_bytes\":0}}"),
      "www.example.test");
  EXPECT_EQ(GoogleString::npos, out.find("evil.test"));
  EXPECT_NE(GoogleString::npos, out.find("\"host\":\"www.example.test\""));
}

TEST(DaemonSiteFilterTest, AnAnswerWithoutTheBlockKeepsEverything) {
  const GoogleString body = "{\"cache\":{\"entries\":3},\"uptime_seconds\":9}";
  EXPECT_EQ(body, Filtered(body, "www.example.test"));
}

TEST(DaemonSiteFilterTest, NotAnObjectIsRefused) {
  for (const char* body :
       {"", "null", "[]", "3", "\"x\"", "not json",
        "{\"a\":1} trailing", "{\"a\":1}{}", "{\"a\":1,}",
        "{/* c */\"a\":1}", "{'a':1}", "{\"a\":NaN}"}) {
    GoogleString out = "stale";
    EXPECT_FALSE(FilterStatsForSite(body, "www.example.test", &out)) << body;
    EXPECT_EQ("", out) << body;
  }
}

TEST(DaemonSiteFilterTest, RepeatedKeysAreRefused) {
  // A second block naming another site must never be the one that wins.
  const GoogleString twice = StrCat(
      "{\"serve_savings_by_host\":{\"hosts\":[],\"limit\":32,\"other\":"
      "{\"hits\":0,\"optimized_bytes\":0,\"original_bytes\":0}},"
      "\"serve_savings_by_host\":{\"hosts\":[{\"hits\":1,\"host\":"
      "\"evil.test\",\"optimized_bytes\":1,\"original_bytes\":1}],"
      "\"limit\":32,\"other\":{\"hits\":0,\"optimized_bytes\":0,"
      "\"original_bytes\":0}}}");
  const GoogleString row_twice = WithBlock(
      "{\"hosts\":[{\"hits\":1,\"host\":\"www.example.test\",\"host\":"
      "\"evil.test\",\"optimized_bytes\":1,\"original_bytes\":1}],"
      "\"limit\":32,\"other\":{\"hits\":0,\"optimized_bytes\":0,"
      "\"original_bytes\":0}}");
  for (const GoogleString& body : {twice, row_twice}) {
    GoogleString out = "stale";
    EXPECT_FALSE(FilterStatsForSite(body, "evil.test", &out)) << body;
    EXPECT_EQ("", out);
  }
}

TEST(DaemonSiteFilterTest, DeepNestingIsRefused) {
  const GoogleString body =
      StrCat("{\"a\":", GoogleString(200, '['), GoogleString(200, ']'), "}");
  GoogleString out = "stale";
  EXPECT_FALSE(FilterStatsForSite(body, "www.example.test", &out));
  EXPECT_EQ("", out);
}

// The optimizer's cooldown list, as it writes it: entries of two sites, one
// of them spelt in capitals.
constexpr char kCooldowns[] =
    "{\"cooldowns\":["
    "{\"duration_seconds\":60,\"hostname\":\"www.example.test\","
    "\"reason\":\"processing\",\"remaining_seconds\":30,\"scheme\":\"https\","
    "\"url\":\"/a.css\"},"
    "{\"duration_seconds\":60,\"hostname\":\"static.example.test\","
    "\"reason\":\"write_failure\",\"remaining_seconds\":20,"
    "\"scheme\":\"https\",\"url\":\"/b.js\"},"
    "{\"duration_seconds\":60,\"hostname\":\"WWW.Example.Test\","
    "\"reason\":\"revalidation\",\"remaining_seconds\":10,"
    "\"scheme\":\"http\",\"url\":\"/c.png\"}],"
    "\"count\":3,\"enabled\":true}";

GoogleString FilteredCooldowns(StringPiece body, StringPiece own_host) {
  GoogleString out;
  EXPECT_TRUE(FilterCooldownsForSite(body, own_host, &out)) << body;
  return out;
}

TEST(DaemonSiteFilterTest, CooldownsKeepOnlyTheOwnHostsEntries) {
  const GoogleString out = FilteredCooldowns(kCooldowns, "www.example.test");
  const Json::Value root = Parse(out);
  ASSERT_EQ(2u, root["cooldowns"].size());
  EXPECT_EQ("/a.css", root["cooldowns"][0]["url"].asString());
  EXPECT_EQ("/c.png", root["cooldowns"][1]["url"].asString());
  EXPECT_EQ(2u, root["count"].asUInt64());
  EXPECT_TRUE(root["enabled"].asBool());
  EXPECT_EQ(GoogleString::npos, out.find("static.example.test"));
  EXPECT_EQ(GoogleString::npos, out.find("/b.js"));
}

TEST(DaemonSiteFilterTest, CooldownsWithoutAnOwnHostAreEmpty) {
  const Json::Value root = Parse(FilteredCooldowns(kCooldowns, ""));
  EXPECT_EQ(0u, root["cooldowns"].size());
  EXPECT_EQ(0u, root["count"].asUInt64());
}

TEST(DaemonSiteFilterTest, CooldownsDropMalformedEntriesAndUnknownKeys) {
  const GoogleString out = FilteredCooldowns(
      "{\"cooldowns\":[\"junk\",{\"hostname\":7,\"url\":\"/seven\"},"
      "{\"url\":\"/nohost\"},{\"hostname\":\"bad host\",\"url\":\"/bad\"},"
      "{\"hostname\":\"www.example.test\",\"reason\":\"no-url\"},"
      "{\"hostname\":\"www.example.test:8443\",\"note\":\"static.example."
      "test\",\"url\":\"/ok\"}],\"count\":5,\"enabled\":true}",
      "www.example.test");
  const Json::Value root = Parse(out);
  ASSERT_EQ(1u, root["cooldowns"].size());
  EXPECT_EQ("/ok", root["cooldowns"][0]["url"].asString());
  EXPECT_EQ(1u, root["count"].asUInt64());
  EXPECT_EQ(GoogleString::npos, out.find("static.example.test"));
  EXPECT_EQ(GoogleString::npos, out.find("/seven"));
  // An entry without a URL names nothing the console can show: dropped.
  EXPECT_EQ(GoogleString::npos, out.find("no-url"));
}

TEST(DaemonSiteFilterTest, CooldownsDropAnEntryWithAWrongTypedField) {
  // The fields the optimizer writes, with the types it writes them in; an
  // entry with any of them of another type is dropped whole.
  const char kOwn[] = "\"hostname\":\"www.example.test\"";
  // A URL, so only the field under test can make the entry malformed.
  const char kUrl[] = ",\"url\":\"/w\"";
  const std::vector<GoogleString> wrong = {
      StrCat("{", kOwn, ",\"url\":{\"a\":\"static.example.test\"}}"),
      StrCat("{", kOwn, ",\"url\":[\"/x\"]}"),
      StrCat("{", kOwn, kUrl, ",\"scheme\":7}"),
      StrCat("{", kOwn, kUrl, ",\"reason\":null}"),
      StrCat("{", kOwn, kUrl, ",\"reason\":true}"),
      StrCat("{", kOwn, kUrl, ",\"remaining_seconds\":-1}"),
      StrCat("{", kOwn, kUrl, ",\"remaining_seconds\":1.5}"),
      StrCat("{", kOwn, kUrl, ",\"remaining_seconds\":\"30\"}"),
      StrCat("{", kOwn, kUrl, ",\"duration_seconds\":1e3}"),
      StrCat("{", kOwn, kUrl, ",\"duration_seconds\":{}}"),
  };
  for (const GoogleString& entry : wrong) {
    const GoogleString body =
        StrCat("{\"cooldowns\":[", entry, ",{", kOwn,
               ",\"url\":\"/ok\",\"remaining_seconds\":3,"
               "\"duration_seconds\":60}],\"count\":2,\"enabled\":true}");
    const GoogleString out = FilteredCooldowns(body, "www.example.test");
    const Json::Value root = Parse(out);
    ASSERT_EQ(1u, root["cooldowns"].size()) << entry;
    EXPECT_EQ("/ok", root["cooldowns"][0]["url"].asString()) << entry;
    EXPECT_EQ(1u, root["count"].asUInt64()) << entry;
    EXPECT_EQ(GoogleString::npos, out.find("static.example.test")) << entry;
  }
}

TEST(DaemonSiteFilterTest, CooldownsKeepOnlyTheKeysTheConsoleReads) {
  // An unknown top-level key is gone, whatever it carries; "enabled"
  // survives only as a boolean.
  const GoogleString out = FilteredCooldowns(
      "{\"by_host\":{\"static.example.test\":2},\"cooldowns\":[],"
      "\"count\":0,\"enabled\":true,\"note\":\"static.example.test\"}",
      "www.example.test");
  EXPECT_EQ("{\"cooldowns\":[],\"count\":0,\"enabled\":true}", out);
  for (const char* enabled : {"\"static.example.test\"", "1", "null", "{}"}) {
    const GoogleString other = FilteredCooldowns(
        StrCat("{\"cooldowns\":[],\"count\":0,\"enabled\":", enabled, "}"),
        "www.example.test");
    EXPECT_EQ("{\"cooldowns\":[],\"count\":0}", other) << enabled;
  }
  EXPECT_EQ("{\"cooldowns\":[],\"count\":0,\"enabled\":false}",
            FilteredCooldowns("{\"enabled\":false}", "www.example.test"));
}

TEST(DaemonSiteFilterTest, CooldownsThatAreNotAListBecomeEmpty) {
  for (const char* body :
       {"{\"cooldowns\":{\"static.example.test\":1},\"count\":1,"
        "\"enabled\":true}",
        "{\"count\":0,\"enabled\":false}"}) {
    const GoogleString out = FilteredCooldowns(body, "www.example.test");
    const Json::Value root = Parse(out);
    EXPECT_TRUE(root["cooldowns"].isArray()) << body;
    EXPECT_EQ(0u, root["cooldowns"].size()) << body;
    EXPECT_EQ(0u, root["count"].asUInt64()) << body;
    EXPECT_EQ(GoogleString::npos, out.find("static.example.test")) << body;
  }
}

TEST(DaemonSiteFilterTest, CooldownsThatAreNotAnObjectAreRefused) {
  for (const char* body :
       {"[{\"hostname\":\"static.example.test\",\"url\":\"/b.js\"}]", "",
        "not json",
        "{\"cooldowns\":[],\"cooldowns\":[{\"hostname\":"
        "\"www.example.test\"}]}"}) {
    GoogleString out = "stale";
    EXPECT_FALSE(FilterCooldownsForSite(body, "www.example.test", &out))
        << body;
    EXPECT_EQ("", out) << body;
  }
}

}  // namespace
}  // namespace net_instaweb
