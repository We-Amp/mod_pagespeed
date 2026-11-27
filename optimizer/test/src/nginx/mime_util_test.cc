// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

// PageSpeed 2.0 - Unit tests for MIME type detection from URL extensions

#include "src/nginx/mime_util.h"

#include "gtest/gtest.h"

namespace pagespeed {
namespace {

// ============================================================================
// Known extensions
// ============================================================================

TEST(MimeFromUrlTest, HtmlExtension) {
  EXPECT_STREQ("text/html", MimeFromUrl("/page.html"));
  EXPECT_STREQ("text/html", MimeFromUrl("/page.htm"));
}

TEST(MimeFromUrlTest, CssExtension) {
  EXPECT_STREQ("text/css", MimeFromUrl("/styles/main.css"));
}

TEST(MimeFromUrlTest, JsExtension) {
  EXPECT_STREQ("application/javascript", MimeFromUrl("/app.js"));
}

TEST(MimeFromUrlTest, ImageExtensions) {
  EXPECT_STREQ("image/jpeg", MimeFromUrl("/photo.jpg"));
  EXPECT_STREQ("image/jpeg", MimeFromUrl("/photo.jpeg"));
  EXPECT_STREQ("image/png", MimeFromUrl("/icon.png"));
  EXPECT_STREQ("image/webp", MimeFromUrl("/hero.webp"));
  EXPECT_STREQ("image/avif", MimeFromUrl("/hero.avif"));
  EXPECT_STREQ("image/gif", MimeFromUrl("/anim.gif"));
  EXPECT_STREQ("image/svg+xml", MimeFromUrl("/logo.svg"));
}

TEST(MimeFromUrlTest, FontExtensions) {
  EXPECT_STREQ("font/woff2", MimeFromUrl("/font.woff2"));
  EXPECT_STREQ("font/woff", MimeFromUrl("/font.woff"));
}

TEST(MimeFromUrlTest, DataExtensions) {
  EXPECT_STREQ("application/json", MimeFromUrl("/api/data.json"));
  EXPECT_STREQ("application/xml", MimeFromUrl("/feed.xml"));
}

// ============================================================================
// Query strings
// ============================================================================

TEST(MimeFromUrlTest, QueryStringStripped) {
  EXPECT_STREQ("text/css", MimeFromUrl("/style.css?v=1.2.3"));
  EXPECT_STREQ("image/png", MimeFromUrl("/img.png?w=100&h=100"));
}

// ============================================================================
// Trailing slash
// ============================================================================

TEST(MimeFromUrlTest, TrailingSlashIsHtml) {
  EXPECT_STREQ("text/html", MimeFromUrl("/blog/post/"));
  EXPECT_STREQ("text/html", MimeFromUrl("/"));
}

// ============================================================================
// Unrecognized / missing extension
// ============================================================================

TEST(MimeFromUrlTest, UnknownExtension) {
  EXPECT_EQ(nullptr, MimeFromUrl("/data.bin"));
  EXPECT_EQ(nullptr, MimeFromUrl("/archive.tar.gz"));
}

TEST(MimeFromUrlTest, NoExtension) {
  EXPECT_EQ(nullptr, MimeFromUrl("/path/without/extension"));
}

TEST(MimeFromUrlTest, EmptyUrl) { EXPECT_EQ(nullptr, MimeFromUrl("")); }

// ============================================================================
// Path depth
// ============================================================================

TEST(MimeFromUrlTest, DeepPath) {
  EXPECT_STREQ("image/jpeg", MimeFromUrl("/a/b/c/d/e/photo.jpg"));
}

TEST(MimeFromUrlTest, DotInDirectory) {
  // The last dot determines the extension.
  EXPECT_STREQ("application/javascript", MimeFromUrl("/static/v2.0/bundle.js"));
}

}  // namespace
}  // namespace pagespeed
