// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

#include "lib/html/sitemap_parser.h"

#include <string>

#include "gtest/gtest.h"

namespace net_instaweb {
namespace {

TEST(SitemapParserTest, ValidUrlset) {
  ParsedSitemap r = ParseSitemap(
      "<?xml version=\"1.0\"?>\n"
      "<urlset xmlns=\"http://www.sitemaps.org/schemas/sitemap/0.9\">\n"
      "  <url><loc>https://example.com/a</loc></url>\n"
      "  "
      "<url><loc>https://example.com/b</loc><lastmod>2026-01-01</lastmod></"
      "url>\n"
      "</urlset>");
  ASSERT_EQ(r.page_locs.size(), 2u);
  EXPECT_EQ(r.page_locs[0], "https://example.com/a");
  EXPECT_EQ(r.page_locs[1], "https://example.com/b");
  EXPECT_TRUE(r.nested_sitemap_locs.empty());
  EXPECT_FALSE(r.truncated);
}

TEST(SitemapParserTest, SitemapIndex) {
  ParsedSitemap r = ParseSitemap(
      "<sitemapindex>\n"
      "  <sitemap><loc>https://example.com/sitemap1.xml</loc></sitemap>\n"
      "  <sitemap><loc>https://example.com/sitemap2.xml</loc></sitemap>\n"
      "</sitemapindex>");
  EXPECT_TRUE(r.page_locs.empty());
  ASSERT_EQ(r.nested_sitemap_locs.size(), 2u);
  EXPECT_EQ(r.nested_sitemap_locs[0], "https://example.com/sitemap1.xml");
  EXPECT_EQ(r.nested_sitemap_locs[1], "https://example.com/sitemap2.xml");
}

TEST(SitemapParserTest, DecodesXmlEntities) {
  ParsedSitemap r = ParseSitemap(
      "<urlset><url><loc>https://e.com/?a=1&amp;b=2&amp;c=3</loc></url></"
      "urlset>");
  ASSERT_EQ(r.page_locs.size(), 1u);
  EXPECT_EQ(r.page_locs[0], "https://e.com/?a=1&b=2&c=3");
}

TEST(SitemapParserTest, DecodesNumericEntities) {
  // &#x2D; (hex) -> '-', &#46; (decimal) -> '.'
  ParsedSitemap r = ParseSitemap(
      "<urlset><url><loc>https://e.com/a&#x2D;b&#46;c</loc></url></urlset>");
  ASSERT_EQ(r.page_locs.size(), 1u);
  EXPECT_EQ(r.page_locs[0], "https://e.com/a-b.c");
}

TEST(SitemapParserTest, UnwrapsCdata) {
  ParsedSitemap r = ParseSitemap(
      "<urlset><url><loc><![CDATA[https://e.com/x?y=1&z=2]]></loc></url></"
      "urlset>");
  ASSERT_EQ(r.page_locs.size(), 1u);
  EXPECT_EQ(r.page_locs[0], "https://e.com/x?y=1&z=2");
}

TEST(SitemapParserTest, TrimsWhitespace) {
  ParsedSitemap r = ParseSitemap(
      "<urlset><url><loc>\n   https://e.com/y   \n</loc></url></urlset>");
  ASSERT_EQ(r.page_locs.size(), 1u);
  EXPECT_EQ(r.page_locs[0], "https://e.com/y");
}

TEST(SitemapParserTest, OffOriginLocsRetained) {
  // The parser does NOT filter by origin; that is the builder's job.
  ParsedSitemap r = ParseSitemap(
      "<urlset><url><loc>https://evil.example/p</loc></url></urlset>");
  ASSERT_EQ(r.page_locs.size(), 1u);
  EXPECT_EQ(r.page_locs[0], "https://evil.example/p");
}

TEST(SitemapParserTest, RejectsLocWithInternalWhitespace) {
  // A real <loc> URL has no unescaped whitespace; such a value is dropped.
  ParsedSitemap r = ParseSitemap(
      "<urlset><url><loc>https://e.com/a b</loc></url>"
      "<url><loc>https://e.com/ok</loc></url></urlset>");
  ASSERT_EQ(r.page_locs.size(), 1u);
  EXPECT_EQ(r.page_locs[0], "https://e.com/ok");
}

TEST(SitemapParserTest, SkipsSelfClosingLoc) {
  ParsedSitemap r = ParseSitemap(
      "<urlset><url><loc/></url><url><loc>https://e.com/real</loc></url></"
      "urlset>");
  ASSERT_EQ(r.page_locs.size(), 1u);
  EXPECT_EQ(r.page_locs[0], "https://e.com/real");
}

TEST(SitemapParserTest, DoesNotMatchLocationElement) {
  // "<location>" must not be mistaken for "<loc>".
  ParsedSitemap r = ParseSitemap(
      "<urlset><url><location>nope</location>"
      "<loc>https://e.com/real</loc></url></urlset>");
  ASSERT_EQ(r.page_locs.size(), 1u);
  EXPECT_EQ(r.page_locs[0], "https://e.com/real");
}

TEST(SitemapParserTest, MalformedUnclosedLocFailsClosed) {
  // No </loc> — must not crash and must not emit a garbage entry.
  ParsedSitemap r = ParseSitemap("<urlset><url><loc>https://e.com/z");
  EXPECT_TRUE(r.page_locs.empty());
  EXPECT_TRUE(r.nested_sitemap_locs.empty());
}

TEST(SitemapParserTest, NotASitemapReturnsEmpty) {
  ParsedSitemap r =
      ParseSitemap("<html><body><loc>https://e.com/x</loc></body></html>");
  EXPECT_TRUE(r.page_locs.empty());
  EXPECT_TRUE(r.nested_sitemap_locs.empty());
}

TEST(SitemapParserTest, EmptyInput) {
  ParsedSitemap r = ParseSitemap("");
  EXPECT_TRUE(r.page_locs.empty());
  EXPECT_TRUE(r.nested_sitemap_locs.empty());
  EXPECT_FALSE(r.truncated);
}

TEST(SitemapParserTest, CapsAtMaxEntries) {
  std::string xml = "<urlset>";
  for (std::size_t i = 0; i < kSitemapMaxEntries + 5; ++i) {
    xml += "<url><loc>https://e.com/p";
    xml += std::to_string(i);
    xml += "</loc></url>";
  }
  xml += "</urlset>";
  ParsedSitemap r = ParseSitemap(xml);
  EXPECT_EQ(r.page_locs.size(), kSitemapMaxEntries);
  EXPECT_TRUE(r.truncated);
}

}  // namespace
}  // namespace net_instaweb
