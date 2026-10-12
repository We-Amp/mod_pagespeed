// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

#include "pagespeed/kernel/packs/json_util.h"

#include <string>

#include "gtest/gtest.h"
#include "pagespeed/kernel/packs/pack.h"

namespace pagespeed::packs {
namespace {

bool Parses(const std::string& text, bool reject_duplicate_keys = false) {
  Json::Value v;
  return ParseStrictJson(text, kMaxJsonLdDepth, reject_duplicate_keys, &v);
}

TEST(ParseStrictJsonTest, AcceptsObjectsAndArrays) {
  EXPECT_TRUE(Parses(R"({"@type":"Thing","n":[1,2.5,-3e2,true,false,null]})"));
  EXPECT_TRUE(Parses(R"([{"a":1},{"b":"\u00e9\n\"x\""}])"));
  EXPECT_TRUE(Parses(" \n\t{\"a\" : 1}\r\n"));
  EXPECT_TRUE(Parses("{\"caf\xC3\xA9\":\"\xE2\x82\xAC\"}"));
}

TEST(ParseStrictJsonTest, LargeNumbers) {
  EXPECT_TRUE(Parses(R"({"a":123456789012345678901234567890})"));
  EXPECT_TRUE(Parses(R"({"a":1e308,"b":-1e-320})"));
  EXPECT_TRUE(Parses(R"({"a":1e999})"));
}

TEST(ParseStrictJsonTest, RejectsWhatABrowserRejects) {
  EXPECT_FALSE(Parses(""));
  EXPECT_FALSE(Parses("   "));
  EXPECT_FALSE(Parses("\"just a string\""));  // not an object or array
  EXPECT_FALSE(Parses("42"));
  EXPECT_FALSE(Parses("{\"a\":1,}"));
  EXPECT_FALSE(Parses("[1,2,]"));
  EXPECT_FALSE(Parses("{'a':1}"));
  EXPECT_FALSE(Parses("{\"a\":1} // comment"));
  EXPECT_FALSE(Parses("/* c */ {\"a\":1}"));
  EXPECT_FALSE(Parses("{\"a\":NaN}"));
  EXPECT_FALSE(Parses("{\"a\":Infinity}"));
  EXPECT_FALSE(Parses("{\"a\":1} {\"b\":2}"));
  EXPECT_FALSE(Parses("{\"a\":1}x"));
  EXPECT_FALSE(Parses("{\"a\":"));
  EXPECT_FALSE(Parses("<!-- {\"a\":1} -->"));
  EXPECT_FALSE(Parses("{a:1}"));
}

TEST(ParseStrictJsonTest, RejectsBytesJsoncppWouldLetThrough) {
  // Invalid UTF-8.
  EXPECT_FALSE(Parses("{\"a\":\"\xFF\"}"));
  EXPECT_FALSE(Parses("{\"a\":\"\xC3\"}"));
  // A raw control character inside a string (an escaped one is fine).
  EXPECT_FALSE(Parses(std::string("{\"a\":\"x\ny\"}")));
  EXPECT_FALSE(Parses(std::string("{\"a\":\"x\ty\"}")));
  EXPECT_FALSE(Parses(std::string("{\"a\":\"x\0y\"}", 11)));
  EXPECT_TRUE(Parses(R"({"a":"x\ny\t\u0000"})"));
  // A control character after an escaped quote is still inside the string.
  EXPECT_FALSE(Parses(std::string("{\"a\":\"\\\"\n\"}")));
  // Whitespace between tokens is not inside a string.
  EXPECT_TRUE(Parses("{\n\"a\"\t:\r\n1}"));
}

TEST(ParseStrictJsonTest, BoundsNesting) {
  std::string ok(kMaxJsonLdDepth, '[');
  ok.append(kMaxJsonLdDepth, ']');
  EXPECT_TRUE(Parses(ok));
  std::string deep(kMaxJsonLdDepth + 1, '[');
  deep.append(kMaxJsonLdDepth + 1, ']');
  EXPECT_FALSE(Parses(deep));
  // Brackets inside strings do not count.
  std::string text = "{\"a\":\"" + std::string(500, '[') + "\"}";
  EXPECT_TRUE(Parses(text));
}

TEST(ParseStrictJsonTest, DuplicateKeysAreAnErrorOnlyOnRequest) {
  const std::string text = R"({"a":1,"a":2})";
  EXPECT_TRUE(Parses(text, false));
  EXPECT_FALSE(Parses(text, true));
  Json::Value v;
  ASSERT_TRUE(ParseStrictJson(text, kMaxJsonLdDepth, false, &v));
  EXPECT_EQ(v["a"].asInt(), 2);  // the last one wins, as in a browser
  EXPECT_FALSE(Parses(R"({"x":{"a":1,"a":2}})", true));
}

TEST(ParseStrictJsonTest, LeavesTheOutputAloneOnFailure) {
  Json::Value v("kept");
  EXPECT_FALSE(ParseStrictJson("{", kMaxJsonLdDepth, false, &v));
  EXPECT_EQ(v.asString(), "kept");
}

TEST(CompactJsonTest, SortsKeysAndDropsWhitespace) {
  Json::Value a;
  Json::Value b;
  ASSERT_TRUE(ParseStrictJson(R"({ "b": [1, 2], "a": "x" })", 64, false, &a));
  ASSERT_TRUE(ParseStrictJson(R"({"a":"x","b":[1,2]})", 64, false, &b));
  EXPECT_EQ(CompactJson(a), R"({"a":"x","b":[1,2]})");
  EXPECT_EQ(CompactJson(a), CompactJson(b));
}

TEST(CompactJsonTest, OutputParsesAgain) {
  Json::Value v;
  ASSERT_TRUE(ParseStrictJson(
      "{\"n\":\"caf\xC3\xA9 \\u0001 \\\" \\\\ /\",\"d\":1.5,\"l\":[[],{}]}", 64,
      true, &v));
  Json::Value again;
  EXPECT_TRUE(ParseStrictJson(CompactJson(v), 64, true, &again));
  EXPECT_EQ(CompactJson(v), CompactJson(again));
}

}  // namespace
}  // namespace pagespeed::packs
