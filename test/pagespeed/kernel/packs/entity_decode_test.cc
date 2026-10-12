// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

// Unit tests for the UTF-8-safe entity decoder. Pins the contract: decode HTML
// entities to human glyphs while NEVER erasing a non-ASCII byte and NEVER
// returning invalid UTF-8.

#include "pagespeed/kernel/packs/entity_decode.h"

#include <string>

#include "gtest/gtest.h"
#include "pagespeed/kernel/html/html_keywords.h"

namespace pagespeed::packs {
namespace {

class SafeEntityDecodeTest : public testing::Test {
 protected:
  void SetUp() override { net_instaweb::HtmlKeywords::Init(); }
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
  EXPECT_EQ(SafeDecodeHtmlEntities("caf\xE2\x80"), "caf");
  // A lone lead byte at the end is also dropped.
  EXPECT_EQ(SafeDecodeHtmlEntities("ok\xE2"), "ok");
  // A complete sequence is preserved.
  EXPECT_EQ(SafeDecodeHtmlEntities("ok\xE2\x80\x94"), "ok\xE2\x80\x94");
}

// A named entity that decodes to a single Latin-1 byte > 127 (e.g. &middot; ->
// 0xB7) is a lone invalid byte; the decoder must fall back to the original
// entity text rather than ship the invalid byte.
TEST_F(SafeEntityDecodeTest, NonAsciiEntityFallsBackToOriginal) {
  const std::string out = SafeDecodeHtmlEntities("a &middot; b");
  // Never an invalid byte; the literal entity text survives.
  EXPECT_TRUE(IsWellFormedUtf8(out));
  EXPECT_NE(out.find("&middot;"), std::string::npos);
}

// &nbsp; decodes to Latin-1 0xA0 (a lone invalid UTF-8 byte) — it must be folded
// to an ASCII space, never shipped raw.
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

TEST_F(SafeEntityDecodeTest, FoldLatin1NbspInPlace) {
  std::string s = "a\xA0\x62";  // a, NBSP, b
  FoldLatin1Nbsp(&s);
  EXPECT_EQ(s, "a b");
}

}  // namespace
}  // namespace pagespeed::packs
