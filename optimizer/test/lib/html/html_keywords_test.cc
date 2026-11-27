// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

// Ported from mod_pagespeed for PageSpeed 2.0

// Unit-test the HTML escaper.

#include "lib/html/html_keywords.h"

#include <string>
#include <string_view>

#include "absl/strings/str_cat.h"
#include "absl/strings/str_format.h"
#include "gtest/gtest.h"
#include "lib/html/compat/message_handler.h"
#include "lib/html/html_name.h"
#include "lib/html/html_parse.h"

namespace net_instaweb {

class HtmlKeywordsTest : public testing::Test {
 protected:
  static void SetUpTestSuite() { HtmlKeywords::Init(); }

  std::string_view Unescape(std::string_view escaped, std::string* buf) {
    bool decoding_error;
    std::string_view unescaped =
        HtmlKeywords::Unescape(escaped, buf, &decoding_error);
    EXPECT_FALSE(decoding_error);
    return unescaped;
  }

  bool UnescapeEncodingError(std::string_view escaped) {
    std::string buf;
    bool decoding_error;
    HtmlKeywords::Unescape(escaped, &buf, &decoding_error);
    return decoding_error;
  }

  // In general HtmlKeywords is not reversible, but it is in
  // specific cases.
  void BiTest(const std::string& escaped, const std::string& unescaped) {
    std::string buf;
    EXPECT_STREQ(escaped.c_str(),
                 std::string(HtmlKeywords::Escape(unescaped, &buf)).c_str());
    EXPECT_STREQ(unescaped.c_str(),
                 std::string(Unescape(escaped, &buf)).c_str());
  }

  // Validates that the provided text is not altered by escaping it.
  void Unchanged(const std::string& text) {
    std::string buf;
    EXPECT_STREQ(text.c_str(),
                 std::string(HtmlKeywords::Escape(text, &buf)).c_str());
    EXPECT_STREQ(text.c_str(), std::string(Unescape(text, &buf)).c_str());
  }

  void TestEscape(const std::string& symbolic_code, char value) {
    std::string symbolic_escaped = absl::StrCat("&", symbolic_code, ";");
    std::string numeric_escaped =
        absl::StrFormat("&#%02d;", static_cast<unsigned char>(value));
    std::string unescaped(&value, 1);
    std::string buf;
    BiTest(symbolic_escaped, unescaped);
    EXPECT_EQ(unescaped, Unescape(numeric_escaped, &buf));
  }
};

TEST_F(HtmlKeywordsTest, Keywords) {
  EXPECT_TRUE(HtmlKeywords::KeywordToString(HtmlName::kNotAKeyword) == nullptr);
  for (int i = 0; i < HtmlName::num_keywords(); ++i) {
    auto keyword = static_cast<HtmlName::Keyword>(i);
    const std::string_view* name = HtmlKeywords::KeywordToString(keyword);
    EXPECT_EQ(keyword, HtmlName::Lookup(*name));
  }
}

TEST_F(HtmlKeywordsTest, TagCloseStyleCategories) {
  // The tag-close-style tables are queried through HtmlParse's member
  // predicates (the canonical API surface; the optimizer-only HtmlKeywords
  // statics went away with the #1130 vendoring).
  NullMessageHandler message_handler;
  HtmlParse parse(&message_handler);
  EXPECT_TRUE(parse.IsImplicitlyClosedTag(HtmlName::kImg));
  EXPECT_TRUE(parse.IsImplicitlyClosedTag(HtmlName::kBr));
  EXPECT_TRUE(parse.IsImplicitlyClosedTag(HtmlName::kXml));
  EXPECT_TRUE(parse.IsImplicitlyClosedTag(HtmlName::kWbr));
  EXPECT_FALSE(parse.IsImplicitlyClosedTag(HtmlName::kDiv));
  EXPECT_FALSE(parse.IsImplicitlyClosedTag(HtmlName::kA));

  // Brief termination is disallowed for non-brief tags and for implicitly
  // closed tags, allowed for everything else.
  EXPECT_FALSE(parse.TagAllowsBriefTermination(HtmlName::kDiv));
  EXPECT_FALSE(parse.TagAllowsBriefTermination(HtmlName::kScript));
  EXPECT_FALSE(parse.TagAllowsBriefTermination(HtmlName::kTextarea));
  EXPECT_FALSE(parse.TagAllowsBriefTermination(HtmlName::kImg));
  EXPECT_TRUE(parse.TagAllowsBriefTermination(HtmlName::kB));
  EXPECT_TRUE(parse.TagAllowsBriefTermination(HtmlName::kTable));
}

TEST_F(HtmlKeywordsTest, Bidirectional) {
  BiTest("a&amp;b", "a&b");

  // octal 200 is decimal 128, and lacks symbolic representation
  BiTest("a&#128;&#07;b", "a\200\007b");

  std::string buf;
  EXPECT_EQ("'", Unescape("&#39;", &buf));
  EXPECT_EQ("(", Unescape("&#40;", &buf));
  EXPECT_EQ(")", Unescape("&#41;", &buf));
}

TEST_F(HtmlKeywordsTest, Hex) {
  std::string buf;
  EXPECT_EQ("'", Unescape("&#x27;", &buf));
  EXPECT_EQ("(", Unescape("&#x28;", &buf));
  EXPECT_EQ(")", Unescape("&#x29;", &buf));
}

TEST_F(HtmlKeywordsTest, AllCodes) {
  TestEscape("AElig", 0xC6);
  TestEscape("Aacute", 0xC1);
  TestEscape("Acirc", 0xC2);
  TestEscape("Agrave", 0xC0);
  TestEscape("Aring", 0xC5);
  TestEscape("Atilde", 0xC3);
  TestEscape("Auml", 0xC4);
  TestEscape("Ccedil", 0xC7);
  TestEscape("ETH", 0xD0);
  TestEscape("Eacute", 0xC9);
  TestEscape("Ecirc", 0xCA);
  TestEscape("Egrave", 0xC8);
  TestEscape("Euml", 0xCB);
  TestEscape("Iacute", 0xCD);
  TestEscape("Icirc", 0xCE);
  TestEscape("Igrave", 0xCC);
  TestEscape("Iuml", 0xCF);
  TestEscape("Ntilde", 0xD1);
  TestEscape("Oacute", 0xD3);
  TestEscape("Ocirc", 0xD4);
  TestEscape("Ograve", 0xD2);
  TestEscape("Oslash", 0xD8);
  TestEscape("Otilde", 0xD5);
  TestEscape("Ouml", 0xD6);
  TestEscape("THORN", 0xDE);
  TestEscape("Uacute", 0xDA);
  TestEscape("Ucirc", 0xDB);
  TestEscape("Ugrave", 0xD9);
  TestEscape("Uuml", 0xDC);
  TestEscape("Yacute", 0xDD);
  TestEscape("aacute", 0xE1);
  TestEscape("acirc", 0xE2);
  TestEscape("acute", 0xB4);
  TestEscape("aelig", 0xE6);
  TestEscape("agrave", 0xE0);
  TestEscape("amp", 0x26);
  TestEscape("aring", 0xE5);
  TestEscape("atilde", 0xE3);
  TestEscape("auml", 0xE4);
  TestEscape("brvbar", 0xA6);
  TestEscape("ccedil", 0xE7);
  TestEscape("cedil", 0xB8);
  TestEscape("cent", 0xA2);
  TestEscape("copy", 0xA9);
  TestEscape("curren", 0xA4);
  TestEscape("deg", 0xB0);
  TestEscape("divide", 0xF7);
  TestEscape("eacute", 0xE9);
  TestEscape("ecirc", 0xEA);
  TestEscape("egrave", 0xE8);
  TestEscape("eth", 0xF0);
  TestEscape("euml", 0xEB);
  TestEscape("frac12", 0xBD);
  TestEscape("frac14", 0xBC);
  TestEscape("frac34", 0xBE);
  TestEscape("gt", 0x3E);
  TestEscape("iacute", 0xED);
  TestEscape("icirc", 0xEE);
  TestEscape("iexcl", 0xA1);
  TestEscape("igrave", 0xEC);
  TestEscape("iquest", 0xBF);
  TestEscape("iuml", 0xEF);
  TestEscape("laquo", 0xAB);
  TestEscape("lt", 0x3C);
  TestEscape("macr", 0xAF);
  TestEscape("micro", 0xB5);
  TestEscape("middot", 0xB7);
  TestEscape("nbsp", 0xA0);
  TestEscape("not", 0xAC);
  TestEscape("ntilde", 0xF1);
  TestEscape("oacute", 0xF3);
  TestEscape("ocirc", 0xF4);
  TestEscape("ograve", 0xF2);
  TestEscape("ordf", 0xAA);
  TestEscape("ordm", 0xBA);
  TestEscape("oslash", 0xF8);
  TestEscape("otilde", 0xF5);
  TestEscape("ouml", 0xF6);
  TestEscape("para", 0xB6);
  TestEscape("plusmn", 0xB1);
  TestEscape("pound", 0xA3);
  TestEscape("quot", 0x22);
  TestEscape("raquo", 0xBB);
  TestEscape("reg", 0xAE);
  TestEscape("sect", 0xA7);
  TestEscape("shy", 0xAD);
  TestEscape("sup1", 0xB9);
  TestEscape("sup2", 0xB2);
  TestEscape("sup3", 0xB3);
  TestEscape("szlig", 0xDF);
  TestEscape("thorn", 0xFE);
  TestEscape("times", 0xD7);
  TestEscape("uacute", 0xFA);
  TestEscape("ucirc", 0xFB);
  TestEscape("ugrave", 0xF9);
  TestEscape("uml", 0xA8);
  TestEscape("uuml", 0xFC);
  TestEscape("yacute", 0xFD);
  TestEscape("yen", 0xA5);
  TestEscape("yuml", 0xFF);
}

TEST_F(HtmlKeywordsTest, DetectEncodingErrors) {
  EXPECT_FALSE(UnescapeEncodingError("abc"));
  EXPECT_FALSE(UnescapeEncodingError("a&amp;b"));
  EXPECT_FALSE(UnescapeEncodingError("a&b"));
  EXPECT_FALSE(UnescapeEncodingError("a&b&amp;c"));
  EXPECT_FALSE(UnescapeEncodingError("&#126;"));
  EXPECT_FALSE(UnescapeEncodingError("&#127;"));
  EXPECT_FALSE(UnescapeEncodingError("&#128;"));
  EXPECT_FALSE(UnescapeEncodingError("&#255;"));
  EXPECT_FALSE(UnescapeEncodingError("&apos;"));  // Ignore invalid code.
  EXPECT_FALSE(UnescapeEncodingError("&acute;"));
  EXPECT_FALSE(UnescapeEncodingError("&ACUTE;"));  // sloppy case OK.
  EXPECT_FALSE(UnescapeEncodingError("&yuml;"));   // lower-case is 255.
  EXPECT_TRUE(UnescapeEncodingError("&YUML;"));    // sloppy-case OK.
  EXPECT_TRUE(UnescapeEncodingError("&Yuml;"));  // upper-case is 376; no good.
  EXPECT_TRUE(UnescapeEncodingError("&#256;"));
  EXPECT_TRUE(UnescapeEncodingError("&#2560;"));
  EXPECT_TRUE(UnescapeEncodingError("\200"));
}

TEST_F(HtmlKeywordsTest, EscapedSingleByteAccented) {
  BiTest("&atilde;&Yacute;&yacute;", "\xe3\xdd\xfd");
}

TEST_F(HtmlKeywordsTest, MissingNumber) {
  BiTest("&amp;#;", "&#;");
  BiTest("&amp;#", "&#");
}

TEST_F(HtmlKeywordsTest, NotReallyDecimal) {
  std::string buf;
  EXPECT_STREQ("\001F", std::string(Unescape("&#1F", &buf)).c_str());
}

TEST_F(HtmlKeywordsTest, Apos) {
  // Correct &apos; which appears in web sites but is not valid HTML.
  // http://fishbowl.pastiche.org/2003/07/01/the_curse_of_apos/
  std::string buf;
  EXPECT_STREQ("'", std::string(Unescape("&apos;", &buf)).c_str());
  EXPECT_STREQ("&#39;", std::string(HtmlKeywords::Escape("'", &buf)).c_str());
}

TEST_F(HtmlKeywordsTest, Unescape) {
  std::string buf;
  bool decoding_error;
  EXPECT_STREQ("a\32b", std::string(HtmlKeywords::Unescape("a&#26;b", &buf,
                                                           &decoding_error))
                            .c_str());
  EXPECT_FALSE(decoding_error);
  EXPECT_STREQ(
      "", std::string(HtmlKeywords::Unescape("a&chi;b", &buf, &decoding_error))
              .c_str())
      << "&chi; is multi-byte so we can't represent it yet.";
  EXPECT_TRUE(decoding_error);
  std::string expected;
  expected += 'a';
  expected += static_cast<char>(0x03);
  expected += static_cast<char>(0xa7);  // equivalent to &sect;
  expected += 'b';
  EXPECT_STREQ("a&#03;&sect;b",
               std::string(HtmlKeywords::Escape(expected, &buf)).c_str());
}

TEST_F(HtmlKeywordsTest, ListView) {
  static const char kListView[] =
      "http://list.taobao.com/market/baby.htm?spm=1.151829.71436.25&"
      "cat=50032645&sort=_bid&spercent=95&isprepay=1&user_type=0&gobaby=1&"
      "random=false&lstyle=imgw&as=1&viewIndex=1&yp4p_page=0&commend=all&"
      "atype=b&style=grid&olu=yes&isnew=2&mSelect=false&#ListView";
  std::string buf;
  EXPECT_STREQ(kListView, std::string(Unescape(kListView, &buf)).c_str());
}

TEST_F(HtmlKeywordsTest, DoubleAmpersand) {
  std::string buf;
  EXPECT_STREQ("&&", std::string(Unescape("&&", &buf)).c_str());
  BiTest("&amp;&amp;", "&&");
  EXPECT_STREQ("&&", std::string(Unescape("&amp&amp", &buf)).c_str());
}

TEST_F(HtmlKeywordsTest, KeepSemicolonOnInvalidEscape) {
  std::string buf;
  EXPECT_STREQ("a&b;c", std::string(Unescape("a&b;c", &buf)).c_str());
}

TEST_F(HtmlKeywordsTest, Ocircoooo) {
  // This testcase does not behave the same as Chrome, which surprisingly
  // interprets &ocircoooo as &ocirc;oooo.
  std::string buf;
  EXPECT_STREQ("&ocircoooo", std::string(Unescape("&ocircoooo", &buf)).c_str());
  BiTest("&amp;ocircoooo", "&ocircoooo");
}

TEST_F(HtmlKeywordsTest, WhitespaceNotChanged) {
  Unchanged("a b");
  Unchanged("a\nb");
  Unchanged("a\rb");
  Unchanged("a\tb");
  Unchanged("a\fb");
}

// Covers CaseFoldStringEqual (compat/string_hash.h, canonical
// string_hash.h mirror): returns false when strings have the same length
// but differ after case-folding. Exercises the mismatch branch.
TEST_F(HtmlKeywordsTest, CaseFoldStringEqualMismatch) {
  net_instaweb::CaseFoldStringEqual eq;
  // Same length, same content (case-insensitive) -> true
  EXPECT_TRUE(eq("amp", "AMP"));
  EXPECT_TRUE(eq("amp", "amp"));
  EXPECT_TRUE(eq("Lt", "lt"));
  // Same length, different content -> false (line 217)
  EXPECT_FALSE(eq("amp", "abc"));
  EXPECT_FALSE(eq("amp", "xyz"));
  EXPECT_FALSE(eq("gt", "lt"));
  // Different length -> false (line 213)
  EXPECT_FALSE(eq("amp", "am"));
  EXPECT_FALSE(eq("a", "ab"));
  EXPECT_FALSE(eq("", "x"));
}

}  // namespace net_instaweb
