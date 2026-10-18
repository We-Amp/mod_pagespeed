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

#include "pagespeed/kernel/base/escaping.h"

#include "test/pagespeed/kernel/base/gtest.h"

namespace net_instaweb {

namespace {

class EscapingTest : public testing::Test {
 public:
  void ExpectEscape(const char* name, const char* expect, const char* in) {
    GoogleString out_unquoted, out_quoted;
    EscapeToJsStringLiteral(in, false, &out_unquoted);
    EscapeToJsStringLiteral(in, true, &out_quoted);
    EXPECT_STREQ(expect, out_unquoted) << " on test " << name;
    EXPECT_STREQ(StrCat("\"", expect, "\""), out_quoted) << " on test " << name;
  }
};

TEST_F(EscapingTest, JsEscapeBasic) {
  ExpectEscape("normal", "abc", "abc");
  ExpectEscape("quote", "abc\\\"d", "abc\"d");
  ExpectEscape("backslash", "abc\\\\d", "abc\\d");
  ExpectEscape("carriage_control", "abc\\n\\rde", "abc\n\rde");
}

TEST_F(EscapingTest, JsAvoidCloseScript) {
  ExpectEscape("avoid_close_script", "Foo<\\/script>Bar", "Foo</script>Bar");
  ExpectEscape("not_heavily_excessive_escaping", "/s", "/s");
}

TEST_F(EscapingTest, JsAvoidCloseScriptSpace) {
  ExpectEscape("avoid_close_script2", "Foo<\\/script  >Bar",
               "Foo</script  >Bar");
}

TEST_F(EscapingTest, JsAvoidCloseScriptCase) {
  ExpectEscape("avoid_close_script3", "Foo<\\/scrIpt>Bar", "Foo</scrIpt>Bar");
}

TEST_F(EscapingTest, JsCloseScriptConservativeBehavior) {
  // We don't need to escape </scripty>, but it's safe to do so.
  ExpectEscape("close_script_conservative", "Foo<\\/scripty>Bar",
               "Foo</scripty>Bar");
}

TEST_F(EscapingTest, JsSingleQuotes) {
  GoogleString out_unquoted, out_quoted;
  const char kIn[] = "foo'";
  EscapeToJsStringLiteral(kIn, false, &out_unquoted);
  EscapeToJsStringLiteral(kIn, true, &out_quoted);
  EXPECT_STREQ("foo\\'", out_unquoted);
  EXPECT_STREQ("\"foo'\"", out_quoted);
}

TEST_F(EscapingTest, JsAvoidWeirdParsingSequence) {
  // Some sequences have an effect on HTML parsing, so we want to avoid them.
  GoogleString out;
  EscapeToJsStringLiteral("a <ScrIpt", false, &out);
  EXPECT_EQ("a \\u003cScrIpt", out);

  out.clear();
  EscapeToJsStringLiteral("Foo <!-- ", false, &out);
  EXPECT_EQ("Foo \\u003c!-- ", out);

  out.clear();
  EscapeToJsStringLiteral("Bar ---> ", false, &out);
  EXPECT_EQ("Bar -\\u002d-> ", out);
}

TEST_F(EscapingTest, JsDontEscapeWayTooMuch) {
  GoogleString out;
  EscapeToJsStringLiteral("<div", false, &out);
  EXPECT_EQ("<div", out);

  out.clear();
  EscapeToJsStringLiteral("-----!", false, &out);
  EXPECT_EQ("-----!", out);
}

TEST_F(EscapingTest, JsonEscapeBasic) {
  GoogleString out;
  EscapeToJsonStringLiteral("abc\1\3\n\t\"\\", true, &out);
  EXPECT_EQ("\"abc\\u0001\\u0003\\u000a\\u0009\\u0022\\u005c\"", out);
}

TEST_F(EscapingTest, JsonEscapeAppend) {
  GoogleString out;
  EscapeToJsonStringLiteral("ab", true, &out);
  EscapeToJsStringLiteral("cd", false, &out);
  EXPECT_EQ("\"ab\"cd", out);
}

TEST_F(EscapingTest, JsonEscapeKeepsUtf8Verbatim) {
  // Bytes above 0x7F in valid UTF-8 input are part of multi-byte
  // sequences and need no escaping in JSON; writing each one as \u00xx
  // would decode to unrelated Latin-1 characters and corrupt the text.
  GoogleString out;
  // "caf<0xC3><0xA9>" is "café"; <0xE2><0x82><0xAC> is the euro sign.
  EscapeToJsonStringLiteral("caf\xC3\xA9 \xE2\x82\xAC", true, &out);
  EXPECT_EQ("\"caf\xC3\xA9 \xE2\x82\xAC\"", out);
  // A four-byte sequence stays verbatim as well.
  out.clear();
  EscapeToJsonStringLiteral("\xF0\x9F\x92\xA9", true, &out);
  EXPECT_EQ("\"\xF0\x9F\x92\xA9\"", out);
  // Control characters and JSON metacharacters are still escaped.
  out.clear();
  EscapeToJsonStringLiteral("a\x01\"\\\x7F", true, &out);
  EXPECT_EQ("\"a\\u0001\\u0022\\u005c\x7F\"", out);
}

TEST_F(EscapingTest, JsonEscapeEscapesMalformedUtf8) {
  // Bytes that do not form a well-formed UTF-8 sequence would make the
  // output invalid JSON if passed through, so each is escaped as \u00xx.
  GoogleString out;

  // A continuation byte with no lead byte.
  EscapeToJsonStringLiteral("a\x80" "b", true, &out);
  EXPECT_EQ("\"a\\u0080b\"", out);

  // A truncated two-byte sequence at the end of the input.
  out.clear();
  EscapeToJsonStringLiteral("caf\xC3", true, &out);
  EXPECT_EQ("\"caf\\u00c3\"", out);

  // A three-byte sequence whose last continuation byte is missing: the
  // lead byte and the orphaned continuation byte are escaped singly.
  out.clear();
  EscapeToJsonStringLiteral("\xE2\x82z", true, &out);
  EXPECT_EQ("\"\\u00e2\\u0082z\"", out);

  // An overlong encoding is not well-formed UTF-8.
  out.clear();
  EscapeToJsonStringLiteral("\xC0\xAF", true, &out);
  EXPECT_EQ("\"\\u00c0\\u00af\"", out);

  // A surrogate half (U+D800) is not a character and not valid UTF-8.
  out.clear();
  EscapeToJsonStringLiteral("\xED\xA0\x80", true, &out);
  EXPECT_EQ("\"\\u00ed\\u00a0\\u0080\"", out);

  // A code point above U+10FFFF is not valid UTF-8 either.
  out.clear();
  EscapeToJsonStringLiteral("\xF4\x90\x80\x80", true, &out);
  EXPECT_EQ("\"\\u00f4\\u0090\\u0080\\u0080\"", out);
}

}  // namespace

}  // namespace net_instaweb
