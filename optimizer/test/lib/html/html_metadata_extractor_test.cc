// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

#include "lib/html/html_metadata_extractor.h"

#include <string>
#include <string_view>

#include "gtest/gtest.h"
#include "lib/html/html_keywords.h"

namespace net_instaweb {
namespace {

class HtmlMetadataExtractorTest : public testing::Test {
 protected:
  void SetUp() override { HtmlKeywords::Init(); }
  HtmlMetadata Extract(std::string_view html) {
    return ExtractHtmlMetadata(html, "http://test.com/page.html");
  }
};

TEST_F(HtmlMetadataExtractorTest, TitleAndDescription) {
  HtmlMetadata m = Extract(
      "<html><head><title>Hello World</title>"
      "<meta name=\"description\" content=\"A page about things\">"
      "</head><body>ignored</body></html>");
  EXPECT_EQ(m.title, "Hello World");
  EXPECT_EQ(m.meta_description, "A page about things");
}

TEST_F(HtmlMetadataExtractorTest, MissingMetaLeavesEmpty) {
  HtmlMetadata m =
      Extract("<html><head><title>Only Title</title></head></html>");
  EXPECT_EQ(m.title, "Only Title");
  EXPECT_TRUE(m.meta_description.empty());
  EXPECT_TRUE(m.meta_robots.empty());
}

TEST_F(HtmlMetadataExtractorTest, DecodesEntitiesInAttribute) {
  HtmlMetadata m = Extract(
      "<head><meta name=description content=\"A &amp; B &lt;ok&gt;\"></head>");
  EXPECT_EQ(m.meta_description, "A & B <ok>");
}

// A description with a literal multibyte glyph (em-dash, curly quote, accent —
// how Chrome's rendered outerHTML carries them) must survive. Reading the
// DECODED attribute value erased it (HtmlKeywords::Unescape returns empty for
// any byte > 127), silently dropping the description on every prod page whose
// description had an em-dash. Sourcing the escaped value + UTF-8-safe decode
// preserves it.
TEST_F(HtmlMetadataExtractorTest, DescriptionPreservesNonAsciiGlyph) {
  // "Fast \xE2\x80\x94 really" = "Fast — really" (U+2014 EM DASH).
  HtmlMetadata m = Extract(
      "<head><meta name=description content=\"Fast \xE2\x80\x94 really\">"
      "</head>");
  EXPECT_EQ(m.meta_description, "Fast \xE2\x80\x94 really");
}

TEST_F(HtmlMetadataExtractorTest, TrimsTitleWhitespace) {
  HtmlMetadata m = Extract("<head><title>   Spaced   </title></head>");
  EXPECT_EQ(m.title, "Spaced");
}

TEST_F(HtmlMetadataExtractorTest, CaseInsensitiveMetaName) {
  HtmlMetadata m = Extract(
      "<head><meta NAME=\"Description\" CONTENT=\"Desc\">"
      "<meta name=\"ROBOTS\" content=\"noindex, noai\"></head>");
  EXPECT_EQ(m.meta_description, "Desc");
  EXPECT_EQ(m.meta_robots, "noindex, noai");
}

TEST_F(HtmlMetadataExtractorTest, CapturesMetaRobotsAndAi) {
  HtmlMetadata m = Extract("<head><meta name=\"ai\" content=\"noai\"></head>");
  EXPECT_EQ(m.meta_robots, "noai");
}

TEST_F(HtmlMetadataExtractorTest, FirstTitleAndFirstDescriptionWin) {
  HtmlMetadata m = Extract(
      "<head><title>First</title><title>Second</title>"
      "<meta name=description content=\"d1\">"
      "<meta name=description content=\"d2\"></head>");
  EXPECT_EQ(m.title, "First");
  EXPECT_EQ(m.meta_description, "d1");
}

TEST_F(HtmlMetadataExtractorTest, MalformedDoesNotCrash) {
  // Reaching the assertion at all proves no crash on unterminated input.
  HtmlMetadata m = Extract("<title>Unclosed and <b>nested");
  EXPECT_NE(m.title.find("Unclosed"), std::string::npos);
}

TEST_F(HtmlMetadataExtractorTest, EmptyInput) {
  HtmlMetadata m = Extract("");
  EXPECT_TRUE(m.title.empty());
  EXPECT_TRUE(m.meta_description.empty());
}

TEST_F(HtmlMetadataExtractorTest, MetaWithoutContentIgnored) {
  HtmlMetadata m = Extract("<head><meta name=\"description\"></head>");
  EXPECT_TRUE(m.meta_description.empty());
}

}  // namespace
}  // namespace net_instaweb
