// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

#include "lib/classify/content_type.h"

#include "gtest/gtest.h"

namespace pagespeed {
namespace {

TEST(ContentTypeTest, Html) {
  EXPECT_EQ(ClassifyContentType("text/html"), ContentType::kHtml);
  EXPECT_EQ(ClassifyContentType("text/html; charset=utf-8"),
            ContentType::kHtml);
  EXPECT_EQ(ClassifyContentType("TEXT/HTML"), ContentType::kHtml);
  EXPECT_EQ(ClassifyContentType("application/xhtml+xml"), ContentType::kHtml);
}

TEST(ContentTypeTest, Css) {
  EXPECT_EQ(ClassifyContentType("text/css"), ContentType::kCss);
  EXPECT_EQ(ClassifyContentType("text/css; charset=utf-8"), ContentType::kCss);
  EXPECT_EQ(ClassifyContentType("Text/CSS"), ContentType::kCss);
}

TEST(ContentTypeTest, Javascript) {
  EXPECT_EQ(ClassifyContentType("application/javascript"), ContentType::kJs);
  EXPECT_EQ(ClassifyContentType("text/javascript"), ContentType::kJs);
  EXPECT_EQ(ClassifyContentType("application/x-javascript"), ContentType::kJs);
  EXPECT_EQ(ClassifyContentType("application/ecmascript"), ContentType::kJs);
  EXPECT_EQ(ClassifyContentType("text/ecmascript"), ContentType::kJs);
  EXPECT_EQ(ClassifyContentType("APPLICATION/JAVASCRIPT"), ContentType::kJs);
}

TEST(ContentTypeTest, Images) {
  EXPECT_EQ(ClassifyContentType("image/jpeg"), ContentType::kImage);
  EXPECT_EQ(ClassifyContentType("image/png"), ContentType::kImage);
  EXPECT_EQ(ClassifyContentType("image/webp"), ContentType::kImage);
  EXPECT_EQ(ClassifyContentType("image/avif"), ContentType::kImage);
  EXPECT_EQ(ClassifyContentType("image/gif"), ContentType::kImage);
  EXPECT_EQ(ClassifyContentType("image/svg+xml"), ContentType::kImage);
  EXPECT_EQ(ClassifyContentType("IMAGE/JPEG"), ContentType::kImage);
}

TEST(ContentTypeTest, Other) {
  EXPECT_EQ(ClassifyContentType("application/octet-stream"),
            ContentType::kOther);
  EXPECT_EQ(ClassifyContentType("application/json"), ContentType::kOther);
  EXPECT_EQ(ClassifyContentType("font/woff2"), ContentType::kOther);
  EXPECT_EQ(ClassifyContentType(""), ContentType::kOther);
}

TEST(ContentTypeTest, ParameterStripping) {
  EXPECT_EQ(ClassifyContentType("text/html; charset=utf-8; boundary=something"),
            ContentType::kHtml);
  EXPECT_EQ(ClassifyContentType("  text/css  "), ContentType::kCss);
}

TEST(ContentTypeTest, ContentTypeMimeRoundTrip) {
  EXPECT_EQ(ContentTypeMime(ContentType::kHtml), "text/html");
  EXPECT_EQ(ContentTypeMime(ContentType::kCss), "text/css");
  EXPECT_EQ(ContentTypeMime(ContentType::kJs), "application/javascript");
  // kImage returns a generic type
  EXPECT_EQ(ContentTypeMime(ContentType::kImage), "image/jpeg");
  EXPECT_EQ(ContentTypeMime(ContentType::kOther), "application/octet-stream");
}

// Tells text/html from XHTML, which ClassifyContentType files as kHtml too.
TEST(ContentTypeTest, IsTextHtmlMediaType) {
  EXPECT_TRUE(IsTextHtmlMediaType("text/html"));
  EXPECT_TRUE(IsTextHtmlMediaType("TEXT/HTML"));
  EXPECT_TRUE(IsTextHtmlMediaType("text/html; charset=utf-8"));
  EXPECT_TRUE(IsTextHtmlMediaType(" text/html ;charset=utf-8"));
  EXPECT_FALSE(IsTextHtmlMediaType("application/xhtml+xml"));
  EXPECT_FALSE(IsTextHtmlMediaType("application/xhtml+xml; charset=utf-8"));
  EXPECT_FALSE(IsTextHtmlMediaType("text/htmlx"));
  EXPECT_FALSE(IsTextHtmlMediaType("text/xml"));
  EXPECT_FALSE(IsTextHtmlMediaType("text/plain"));
  EXPECT_FALSE(IsTextHtmlMediaType(""));
}

TEST(ContentTypeTest, ContentTypeMimeUnknownEnum) {
  // Defensive fallback for unknown ContentType value
  EXPECT_EQ(ContentTypeMime(static_cast<ContentType>(99)),
            "application/octet-stream");
}

}  // namespace
}  // namespace pagespeed
