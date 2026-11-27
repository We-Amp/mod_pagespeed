// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

// Tests for the Font URL Extractor.

#include "src/worker/font_url_extractor.h"

#include <string>
#include <vector>

#include "gtest/gtest.h"

namespace pagespeed {
namespace {

TEST(FontUrlExtractorTest, SingleWoff2Font) {
  std::string css =
      "@font-face {\n"
      "  font-family: 'Inter';\n"
      "  src: url('/fonts/inter.woff2') format('woff2');\n"
      "}";

  auto urls = ExtractFontUrls(css);
  ASSERT_EQ(urls.size(), 1u);
  EXPECT_EQ(urls[0], "/fonts/inter.woff2");
}

TEST(FontUrlExtractorTest, PrefersWoff2OverWoff) {
  std::string css =
      "@font-face {\n"
      "  font-family: 'Inter';\n"
      "  src: url('/fonts/inter.woff') format('woff'),\n"
      "       url('/fonts/inter.woff2') format('woff2');\n"
      "}";

  auto urls = ExtractFontUrls(css);
  ASSERT_EQ(urls.size(), 1u);
  EXPECT_EQ(urls[0], "/fonts/inter.woff2");
}

TEST(FontUrlExtractorTest, MultipleFontFaces) {
  std::string css =
      "@font-face {\n"
      "  font-family: 'Inter';\n"
      "  src: url('/fonts/inter-regular.woff2') format('woff2');\n"
      "}\n"
      "@font-face {\n"
      "  font-family: 'Inter';\n"
      "  font-weight: 700;\n"
      "  src: url('/fonts/inter-bold.woff2') format('woff2');\n"
      "}";

  auto urls = ExtractFontUrls(css);
  ASSERT_EQ(urls.size(), 2u);
  EXPECT_EQ(urls[0], "/fonts/inter-regular.woff2");
  EXPECT_EQ(urls[1], "/fonts/inter-bold.woff2");
}

TEST(FontUrlExtractorTest, DeduplicatesUrls) {
  std::string css =
      "@font-face {\n"
      "  font-family: 'Inter';\n"
      "  src: url('/fonts/inter.woff2') format('woff2');\n"
      "}\n"
      "@font-face {\n"
      "  font-family: 'Inter';\n"
      "  font-style: italic;\n"
      "  src: url('/fonts/inter.woff2') format('woff2');\n"
      "}";

  auto urls = ExtractFontUrls(css);
  ASSERT_EQ(urls.size(), 1u);
  EXPECT_EQ(urls[0], "/fonts/inter.woff2");
}

TEST(FontUrlExtractorTest, SkipsDataUrls) {
  std::string css =
      "@font-face {\n"
      "  font-family: 'Icons';\n"
      "  src: url(data:font/woff2;base64,AAAA) format('woff2');\n"
      "}";

  auto urls = ExtractFontUrls(css);
  EXPECT_TRUE(urls.empty());
}

TEST(FontUrlExtractorTest, CapsAtTen) {
  std::string css;
  for (int i = 0; i < 15; ++i) {
    css += "@font-face { src: url('/font" + std::to_string(i) +
           ".woff2') format('woff2'); }\n";
  }

  auto urls = ExtractFontUrls(css);
  EXPECT_EQ(urls.size(), 10u);
}

TEST(FontUrlExtractorTest, NoFontFace) {
  std::string css = "body { font-family: sans-serif; }";
  auto urls = ExtractFontUrls(css);
  EXPECT_TRUE(urls.empty());
}

TEST(FontUrlExtractorTest, ExtensionBasedWoff2Detection) {
  std::string css =
      "@font-face {\n"
      "  font-family: 'Custom';\n"
      "  src: url('/fonts/custom.woff2');\n"
      "}";

  auto urls = ExtractFontUrls(css);
  ASSERT_EQ(urls.size(), 1u);
  EXPECT_EQ(urls[0], "/fonts/custom.woff2");
}

TEST(FontUrlExtractorTest, SkipsNonWoff2Only) {
  std::string css =
      "@font-face {\n"
      "  font-family: 'Legacy';\n"
      "  src: url('/fonts/legacy.woff') format('woff');\n"
      "}";

  auto urls = ExtractFontUrls(css);
  EXPECT_TRUE(urls.empty());
}

TEST(FontUrlExtractorTest, HandlesQuotedAndUnquotedUrls) {
  std::string css =
      "@font-face {\n"
      "  src: url(\"/fonts/a.woff2\") format('woff2');\n"
      "}\n"
      "@font-face {\n"
      "  src: url('/fonts/b.woff2') format('woff2');\n"
      "}\n"
      "@font-face {\n"
      "  src: url(/fonts/c.woff2) format('woff2');\n"
      "}";

  auto urls = ExtractFontUrls(css);
  ASSERT_EQ(urls.size(), 3u);
  EXPECT_EQ(urls[0], "/fonts/a.woff2");
  EXPECT_EQ(urls[1], "/fonts/b.woff2");
  EXPECT_EQ(urls[2], "/fonts/c.woff2");
}

TEST(FontUrlExtractorTest, LocalSourceIgnored) {
  std::string css =
      "@font-face {\n"
      "  font-family: 'Inter';\n"
      "  src: local('Inter'), url('/fonts/inter.woff2') format('woff2');\n"
      "}";

  auto urls = ExtractFontUrls(css);
  ASSERT_EQ(urls.size(), 1u);
  EXPECT_EQ(urls[0], "/fonts/inter.woff2");
}

TEST(FontUrlExtractorTest, AbsoluteUrlPreserved) {
  std::string css =
      "@font-face {\n"
      "  src: url('https://cdn.example.com/inter.woff2') format('woff2');\n"
      "}";

  auto urls = ExtractFontUrls(css);
  ASSERT_EQ(urls.size(), 1u);
  EXPECT_EQ(urls[0], "https://cdn.example.com/inter.woff2");
}

TEST(FontUrlExtractorTest, CaseInsensitiveFontFace) {
  std::string css =
      "@Font-Face {\n"
      "  SRC: URL('/fonts/inter.woff2') FORMAT('WOFF2');\n"
      "}";

  auto urls = ExtractFontUrls(css);
  ASSERT_EQ(urls.size(), 1u);
  EXPECT_EQ(urls[0], "/fonts/inter.woff2");
}

TEST(FontUrlExtractorTest, EmptyCssInput) {
  auto urls = ExtractFontUrls("");
  EXPECT_TRUE(urls.empty());
}

TEST(FontUrlExtractorTest, FontFaceInsideLayerBlock) {
  // @font-face nested inside @layer — the simple brace matching in
  // ExtractFontUrls finds the first '}' (closing @font-face), so the
  // inner @font-face is still found.
  std::string css =
      "@layer base {\n"
      "  @font-face {\n"
      "    font-family: 'Inter';\n"
      "    src: url('/fonts/inter.woff2') format('woff2');\n"
      "  }\n"
      "}\n";

  auto urls = ExtractFontUrls(css);
  ASSERT_EQ(urls.size(), 1u);
  EXPECT_EQ(urls[0], "/fonts/inter.woff2");
}

TEST(FontUrlExtractorTest, QueryStringPreserved) {
  std::string css =
      "@font-face {\n"
      "  src: url('/fonts/inter.woff2?v=3') format('woff2');\n"
      "}";

  auto urls = ExtractFontUrls(css);
  ASSERT_EQ(urls.size(), 1u);
  EXPECT_EQ(urls[0], "/fonts/inter.woff2?v=3");
}

}  // namespace
}  // namespace pagespeed
