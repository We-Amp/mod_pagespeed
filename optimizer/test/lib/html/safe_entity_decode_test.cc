// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

// Unit tests for the shared UTF-8-safe entity decoder. Pins the
// contract that the agent_optimize body-text, inline-code, and <meta
// description> paths all rely on: decode HTML entities to human glyphs while
// NEVER erasing a non-ASCII byte and NEVER shipping invalid UTF-8.

#include "lib/html/safe_entity_decode.h"

#include <string>

#include "gtest/gtest.h"
#include "lib/html/html_keywords.h"

namespace net_instaweb {
namespace {

class SafeEntityDecodeTest : public testing::Test {
 protected:
  void SetUp() override { HtmlKeywords::Init(); }
};

TEST_F(SafeEntityDecodeTest, DecodesAsciiEntities) {
  EXPECT_EQ(SafeDecodeHtmlEntities("Tom &amp; Jerry &lt;1ms&gt;"),
            "Tom & Jerry <1ms>");
}

TEST_F(SafeEntityDecodeTest, NoAmpersandFastPathIsVerbatim) {
  EXPECT_EQ(SafeDecodeHtmlEntities("plain ascii text"), "plain ascii text");
  // Literal multibyte UTF-8 (em-dash) with no entity passes through verbatim.
  EXPECT_EQ(SafeDecodeHtmlEntities("Fast \xE2\x80\x94 really"),
            "Fast \xE2\x80\x94 really");
}

// The contract: the no-'&' fast path must never ship invalid UTF-8 even when an
// upstream byte-cap split a trailing multibyte sequence. The incomplete tail is
// dropped (the rest of the field is preserved).
TEST_F(SafeEntityDecodeTest, FastPathTrimsTruncatedTrailingMultibyte) {
  // "caf" + the first 2 bytes of a 3-byte em-dash (E2 80) with the 3rd byte
  // (94) cut off by a cap. No '&' -> fast path. The dangling E2 80 is dropped.
  EXPECT_EQ(SafeDecodeHtmlEntities("caf\xE2\x80", /*value_was_capped=*/true),
            "caf");
  // A lone lead byte at the end is also dropped.
  EXPECT_EQ(SafeDecodeHtmlEntities("ok\xE2", /*value_was_capped=*/true), "ok");
  // An uncut value ending in an incomplete sequence (a non-UTF-8 page) gets
  // a replacement character, like the same bytes anywhere else.
  EXPECT_EQ(SafeDecodeHtmlEntities("Caf\xE9"), "Caf\xEF\xBF\xBD");
  EXPECT_EQ(SafeDecodeHtmlEntities("ok\xE2"), "ok\xEF\xBF\xBD");
  // A complete sequence is preserved.
  EXPECT_EQ(SafeDecodeHtmlEntities("ok\xE2\x80\x94"), "ok\xE2\x80\x94");
}

// Every named reference of the HTML Standard decodes to UTF-8, also next to
// literal UTF-8 text in the same node.
TEST_F(SafeEntityDecodeTest, DecodesHtml5NamedReferences) {
  EXPECT_EQ(SafeDecodeHtmlEntities("a &middot; b"), "a \xC2\xB7 b");
  EXPECT_EQ(SafeDecodeHtmlEntities("CSS &mdash; optimized"),
            "CSS \xE2\x80\x94 optimized");
  EXPECT_EQ(SafeDecodeHtmlEntities("Download &amp; run \xE2\x86\x92"),
            "Download & run \xE2\x86\x92");
  EXPECT_EQ(SafeDecodeHtmlEntities("&rarr;&rsquo;&ldquo;&le;&bull;&check;"),
            "\xE2\x86\x92\xE2\x80\x99\xE2\x80\x9C\xE2\x89\xA4"
            "\xE2\x80\xA2\xE2\x9C\x93");
  EXPECT_EQ(SafeDecodeHtmlEntities("&copy;&eacute;&AElig;"),
            "\xC2\xA9\xC3\xA9\xC3\x86");
  // Two code points.
  EXPECT_EQ(SafeDecodeHtmlEntities("&NotEqualTilde;"), "\xE2\x89\x82\xCC\xB8");
  // The longest name in the table.
  EXPECT_EQ(SafeDecodeHtmlEntities("&CounterClockwiseContourIntegral;"),
            "\xE2\x88\xB3");
}

TEST_F(SafeEntityDecodeTest, DecodesNumericReferences) {
  EXPECT_EQ(SafeDecodeHtmlEntities("&#x26;&#38;&#8212;&#x1F600;"),
            "&&\xE2\x80\x94\xF0\x9F\x98\x80");
  // windows-1252 range, as browsers read it.
  EXPECT_EQ(SafeDecodeHtmlEntities("&#150;&#x80;"), "\xE2\x80\x93\xE2\x82\xAC");
  // NUL, surrogates, out of range and control characters: U+FFFD.
  for (const char* in : {"&#0;", "&#xD800;", "&#x110000;", "&#99999999999;",
                         "&#1;", "&#x7F;", "&#x81;"}) {
    EXPECT_EQ(SafeDecodeHtmlEntities(in), "\xEF\xBF\xBD") << in;
  }
  // Tab and newline are kept; the ';' is optional.
  EXPECT_EQ(SafeDecodeHtmlEntities("a&#10;b&#9;c&#38x"), "a\nb\tc&x");
  // "&#" without digits is text.
  EXPECT_EQ(SafeDecodeHtmlEntities("&#; &#x; &#q"), "&#; &#x; &#q");
}

// An unknown reference stays as written and does not stop the rest of the
// text from decoding.
TEST_F(SafeEntityDecodeTest, UnknownReferenceLeftAloneRestDecoded) {
  EXPECT_EQ(SafeDecodeHtmlEntities("a &bogus; b &amp; c"), "a &bogus; b & c");
  EXPECT_EQ(SafeDecodeHtmlEntities("AT&T & co &;"), "AT&T & co &;");
  EXPECT_EQ(SafeDecodeHtmlEntities("caf\xC3\xA9 &amp; &unknownname; &lt;"),
            "caf\xC3\xA9 & &unknownname; <");
}

TEST_F(SafeEntityDecodeTest, KnowsTheStandardNames) {
  EXPECT_TRUE(IsHtml5EntityName("lt"));
  EXPECT_TRUE(IsHtml5EntityName("mdash"));
  EXPECT_TRUE(IsHtml5EntityName("CounterClockwiseContourIntegral"));
  EXPECT_FALSE(IsHtml5EntityName("bogus"));
  EXPECT_FALSE(IsHtml5EntityName("LT;"));
  EXPECT_FALSE(IsHtml5EntityName(""));
}

// The historical names are references without a ';' too, matched as the
// longest such prefix, as browsers read text.
TEST_F(SafeEntityDecodeTest, LegacyNamesWithoutSemicolon) {
  // The longest historical names.
  EXPECT_EQ(SafeDecodeHtmlEntities("&frac12x &middotx"), "\xC2\xBDx \xC2\xB7x");
  EXPECT_EQ(SafeDecodeHtmlEntities("&copy 2026"), "\xC2\xA9 2026");
  EXPECT_EQ(SafeDecodeHtmlEntities("a &amp b"), "a & b");
  EXPECT_EQ(SafeDecodeHtmlEntities("&notit;"), "\xC2\xACit;");
  // A name that is not historical needs its ';'.
  EXPECT_EQ(SafeDecodeHtmlEntities("&mdash x"), "&mdash x");
}

// &nbsp; is folded to an ASCII space like a literal no-break space.
TEST_F(SafeEntityDecodeTest, FoldsNbspToSpace) {
  const std::string out = SafeDecodeHtmlEntities("a&nbsp;b");
  EXPECT_EQ(out, "a b");
  EXPECT_TRUE(IsWellFormedUtf8(out));
}

TEST_F(SafeEntityDecodeTest, IsWellFormedUtf8Basics) {
  EXPECT_TRUE(IsWellFormedUtf8("ascii"));
  EXPECT_TRUE(IsWellFormedUtf8("caf\xC3\xA9"));    // é
  EXPECT_TRUE(IsWellFormedUtf8("\xE2\x80\x94"));   // em-dash
  EXPECT_FALSE(IsWellFormedUtf8("\xE2\x80"));      // truncated
  EXPECT_FALSE(IsWellFormedUtf8("\xB7"));          // lone Latin-1 byte
  EXPECT_FALSE(IsWellFormedUtf8("\xED\xA0\x80"));  // surrogate
}

// The UTF-8 no-break space is two bytes; both fold together, and a letter whose
// second byte is 0xA0 (U+00E0) is left alone.
TEST_F(SafeEntityDecodeTest, FoldNbspKeepsSequencesWhole) {
  std::string s = "16\xC2\xA0KB \xC3\xA0";
  FoldNbsp(&s);
  EXPECT_EQ(s, "16 KB \xC3\xA0");
  EXPECT_TRUE(IsWellFormedUtf8(s));
}

TEST_F(SafeEntityDecodeTest, LiteralNbspFoldedOnEveryPath) {
  EXPECT_EQ(SafeDecodeHtmlEntities("16\xC2\xA0KB"), "16 KB");
  EXPECT_EQ(SafeDecodeHtmlEntities("a\xC2\xA0\xC2\xA0\x62"), "a  b");
}

TEST_F(SafeEntityDecodeTest, ToWellFormedUtf8ReplacesInvalidBytes) {
  EXPECT_EQ(ToWellFormedUtf8("ok \xE2\x80\x94"), "ok \xE2\x80\x94");
  EXPECT_EQ(ToWellFormedUtf8("caf\xE9!"), "caf\xEF\xBF\xBD!");
  EXPECT_EQ(ToWellFormedUtf8("a\x80\x62"), "a\xEF\xBF\xBD\x62");
  EXPECT_EQ(ToWellFormedUtf8("\xC0\xAF"), "\xEF\xBF\xBD\xEF\xBF\xBD");
  EXPECT_EQ(ToWellFormedUtf8("\xED\xA0\x80"),
            "\xEF\xBF\xBD\xEF\xBF\xBD\xEF\xBF\xBD");
  // An incomplete sequence at the very end is replaced, or dropped when the
  // caller says the value was cut by a byte cap.
  EXPECT_EQ(ToWellFormedUtf8("caf\xE2\x80"), "caf\xEF\xBF\xBD\xEF\xBF\xBD");
  EXPECT_EQ(ToWellFormedUtf8("caf\xE2\x80", /*drop_cut_tail=*/true), "caf");
  // The same bytes in the middle are invalid, so they are replaced.
  EXPECT_EQ(ToWellFormedUtf8("\xE2\x80x"), "\xEF\xBF\xBD\xEF\xBF\xBDx");
  for (const char* in : {"\xFF", "a\xC2", "\xF4\x90\x80\x80", "\xA0\xA0"}) {
    EXPECT_TRUE(IsWellFormedUtf8(ToWellFormedUtf8(in))) << in;
  }
}

TEST_F(SafeEntityDecodeTest, DecodeNeverReturnsInvalidUtf8) {
  EXPECT_EQ(SafeDecodeHtmlEntities("caf\xE9 au lait"),
            "caf\xEF\xBF\xBD au lait");
  EXPECT_EQ(SafeDecodeHtmlEntities("caf\xE9 &lt;"), "caf\xEF\xBF\xBD <");
}

}  // namespace
}  // namespace net_instaweb
