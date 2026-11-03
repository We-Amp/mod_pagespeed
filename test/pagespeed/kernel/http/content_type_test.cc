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

#include "pagespeed/kernel/http/content_type.h"

#include <cstring>

#include "test/pagespeed/kernel/base/gtest.h"

namespace net_instaweb {

class ContentTypeTest : public testing::Test {
 protected:
  ContentType::Type ExtToType(const char* ext) {
    return NameExtensionToContentType(ext)->type();
  }
  ContentType::Type MimeToType(const char* mime_type) {
    return MimeTypeToContentType(mime_type)->type();
  }
};

TEST_F(ContentTypeTest, TestUnknown) {
  EXPECT_EQ(nullptr, NameExtensionToContentType(".unknown"));
  EXPECT_EQ(nullptr, MimeTypeToContentType("unknown/unknown"));
}

TEST_F(ContentTypeTest, TestExtensions) {
  EXPECT_EQ(ContentType::kHtml, ExtToType(".html"));
  EXPECT_EQ(ContentType::kHtml, ExtToType(".htm"));
  EXPECT_EQ(ContentType::kXhtml, ExtToType(".xhtml"));
  EXPECT_EQ(ContentType::kJavascript, ExtToType(".js"));
  EXPECT_EQ(ContentType::kCss, ExtToType(".css"));
  EXPECT_EQ(ContentType::kText, ExtToType(".txt"));
  EXPECT_EQ(ContentType::kXml, ExtToType(".xml"));
  EXPECT_EQ(ContentType::kPng, ExtToType(".png"));
  EXPECT_EQ(ContentType::kGif, ExtToType(".gif"));
  EXPECT_EQ(ContentType::kJpeg, ExtToType(".jpg"));
  EXPECT_EQ(ContentType::kJpeg, ExtToType(".jpeg"));
  EXPECT_EQ(ContentType::kSwf, ExtToType(".swf"));
  EXPECT_EQ(ContentType::kWebp, ExtToType(".webp"));
  EXPECT_EQ(ContentType::kIco, ExtToType(".ico"));
  EXPECT_EQ(ContentType::kJson, ExtToType(".json"));
  EXPECT_EQ(ContentType::kSourceMap, ExtToType(".map"));
  EXPECT_EQ(ContentType::kPdf, ExtToType(".pdf"));
  EXPECT_EQ(ContentType::kOctetStream, ExtToType(".bin"));
  EXPECT_EQ(ContentType::kVideo, ExtToType(".mpg"));
  EXPECT_EQ(ContentType::kVideo, ExtToType(".mp4"));
  EXPECT_EQ(ContentType::kVideo, ExtToType(".3gp"));
  EXPECT_EQ(ContentType::kVideo, ExtToType(".flv"));
  EXPECT_EQ(ContentType::kVideo, ExtToType(".ogg"));

  EXPECT_EQ(ContentType::kAudio, ExtToType(".mp3"));
  EXPECT_EQ(ContentType::kAudio, ExtToType(".wav"));
}

TEST_F(ContentTypeTest, SvgIsXmlButIdentifiable) {
  const ContentType* svg = MimeTypeToContentType("image/svg+xml");
  ASSERT_TRUE(svg != nullptr);
  EXPECT_TRUE(svg->IsSvg());
  EXPECT_FALSE(svg->IsImage());  // image filters must keep skipping SVG
  EXPECT_FALSE(kContentTypePng.IsSvg());
  EXPECT_FALSE(kContentTypeXml.IsSvg());
}

TEST_F(ContentTypeTest, TestMimeType) {
  EXPECT_EQ(ContentType::kHtml, MimeToType("text/html"));
  EXPECT_EQ(ContentType::kHtml, MimeToType("text/html; charset=UTF-8"));
  EXPECT_EQ(ContentType::kXhtml, MimeToType("application/xhtml+xml"));
  EXPECT_EQ(ContentType::kXhtml, MimeToType("application/xhtml+xml; "
                                            "charset=utf-8"));
  EXPECT_EQ(ContentType::kCeHtml, MimeToType("application/ce-html+xml"));
  EXPECT_EQ(ContentType::kJavascript, MimeToType("text/javascript"));
  EXPECT_EQ(ContentType::kJavascript, MimeToType("application/x-javascript"));
  EXPECT_EQ(ContentType::kJavascript, MimeToType("application/javascript"));
  EXPECT_EQ(ContentType::kJavascript, MimeToType("text/ecmascript"));
  EXPECT_EQ(ContentType::kJavascript, MimeToType("application/ecmascript"));
  EXPECT_EQ(ContentType::kJson, MimeToType("application/json"));
  EXPECT_EQ(ContentType::kJson, MimeToType("application/x-json"));
  EXPECT_EQ(ContentType::kCss, MimeToType("text/css"));
  EXPECT_EQ(ContentType::kText, MimeToType("text/plain"));
  EXPECT_EQ(ContentType::kXml, MimeToType("application/xml"));
  EXPECT_EQ(ContentType::kXml, MimeToType("text/xml"));
  EXPECT_EQ(ContentType::kPng, MimeToType("image/png"));
  EXPECT_EQ(ContentType::kGif, MimeToType("image/gif"));

  EXPECT_EQ(ContentType::kJpeg, MimeToType("image/jpeg"));
  EXPECT_EQ(ContentType::kJpeg, MimeToType("image/jpg"));
  EXPECT_EQ(ContentType::kSwf, MimeToType("application/x-shockwave-flash"));
  EXPECT_EQ(ContentType::kWebp, MimeToType("image/webp"));
  EXPECT_EQ(ContentType::kIco, MimeToType("image/x-icon"));
  EXPECT_EQ(ContentType::kIco, MimeToType("image/vnd.microsoft.icon"));
  EXPECT_EQ(ContentType::kVideo, MimeToType("video/3gp"));
  EXPECT_EQ(ContentType::kVideo, MimeToType("video/mpeg"));
  EXPECT_EQ(ContentType::kVideo, MimeToType("video/x-flv"));
  EXPECT_EQ(ContentType::kVideo, MimeToType("video/ogg"));
  EXPECT_EQ(ContentType::kVideo, MimeToType("video/mpeg4"));
  EXPECT_EQ(ContentType::kVideo, MimeToType("video/webm"));
  EXPECT_EQ(ContentType::kVideo, MimeToType("video/x-ms-asf"));
  EXPECT_EQ(ContentType::kVideo, MimeToType("video/x-ms-wmv"));
  EXPECT_EQ(ContentType::kVideo, MimeToType("video/quicktime"));

  EXPECT_EQ(ContentType::kAudio, MimeToType("audio/ogg"));
  EXPECT_EQ(ContentType::kAudio, MimeToType("audio/mpeg"));
  EXPECT_EQ(ContentType::kAudio, MimeToType("audio/webm"));
  EXPECT_EQ(ContentType::kAudio, MimeToType("audio/mp3"));
  EXPECT_EQ(ContentType::kAudio, MimeToType("audio/x-mpeg"));
  EXPECT_EQ(ContentType::kAudio, MimeToType("audio/x-wav"));
  EXPECT_EQ(ContentType::kAudio, MimeToType("audio/mp4"));
  EXPECT_EQ(ContentType::kAudio, MimeToType("audio/wav"));

  EXPECT_EQ(ContentType::kOctetStream, MimeToType("application/octet-stream"));
  EXPECT_EQ(ContentType::kOctetStream, MimeToType("binary/octet-stream"));
}

TEST_F(ContentTypeTest, ConstantSanityCheck) {
  EXPECT_EQ(ContentType::kHtml, kContentTypeHtml.type());
  EXPECT_EQ(ContentType::kXhtml, kContentTypeXhtml.type());
  EXPECT_EQ(ContentType::kCeHtml, kContentTypeCeHtml.type());
  EXPECT_EQ(ContentType::kJavascript, kContentTypeJavascript.type());
  EXPECT_EQ(ContentType::kCss, kContentTypeCss.type());
  EXPECT_EQ(ContentType::kText, kContentTypeText.type());
  EXPECT_EQ(ContentType::kXml, kContentTypeXml.type());
  EXPECT_EQ(ContentType::kJson, kContentTypeJson.type());
  EXPECT_EQ(ContentType::kSourceMap, kContentTypeSourceMap.type());
  EXPECT_EQ(ContentType::kPng, kContentTypePng.type());
  EXPECT_EQ(ContentType::kGif, kContentTypeGif.type());
  EXPECT_EQ(ContentType::kJpeg, kContentTypeJpeg.type());
  EXPECT_EQ(ContentType::kSwf, kContentTypeSwf.type());
  EXPECT_EQ(ContentType::kWebp, kContentTypeWebp.type());
  EXPECT_EQ(ContentType::kIco, kContentTypeIco.type());
  EXPECT_EQ(ContentType::kPdf, kContentTypePdf.type());
  EXPECT_EQ(ContentType::kOctetStream, kContentTypeBinaryOctetStream.type());
}

// Checks that empty string is parsed correctly and results in empty set and
// nothing is crashing.
TEST(MimeTypeListToContentTypeSetTest, EmptyTest) {
  GoogleString s;
  std::set<const ContentType*> out;
  out.insert(&kContentTypeXml);
  MimeTypeListToContentTypeSet(s, &out);
  EXPECT_TRUE(out.empty());
}

// Next two tests check the good cases, where string is correctly formed and set
// should be correctly populated.
// Single entry.
TEST(MimeTypeListToContentTypeSetTest, OkTestSingle) {
  GoogleString s = "image/gif";
  std::set<const ContentType*> out;
  out.insert(&kContentTypeXml);

  MimeTypeListToContentTypeSet(s, &out);
  EXPECT_EQ(1, out.size());
  EXPECT_EQ(1, out.count(&kContentTypeGif));
}

// Multiple entries.
TEST(MimeTypeListToContentTypeSetTest, OkTestMultiple) {
  GoogleString s = "image/gif,image/jpeg,application/octet-stream,image/jpeg";
  std::set<const ContentType*> out;
  out.insert(&kContentTypeXml);

  MimeTypeListToContentTypeSet(s, &out);
  EXPECT_EQ(3, out.size());
  EXPECT_EQ(1, out.count(&kContentTypeBinaryOctetStream));
  EXPECT_EQ(1, out.count(&kContentTypeJpeg));
  EXPECT_EQ(1, out.count(&kContentTypeGif));
}

// Tests malformed string and string with bad mime-types.
TEST(MimeTypeListToContentTypeSetTest, TestBadString) {
  GoogleString s = "image/gif,,,,,";
  std::set<const ContentType*> out;
  out.insert(&kContentTypeXml);

  MimeTypeListToContentTypeSet(s, &out);
  EXPECT_EQ(1, out.size());
  EXPECT_EQ(1, out.count(&kContentTypeGif));

  s = "apple,orange,turnip,,,,image/jpeg,";

  MimeTypeListToContentTypeSet(s, &out);
  EXPECT_EQ(1, out.size());
  EXPECT_EQ(1, out.count(&kContentTypeJpeg));
}
// Entries separated by ", " (comma + whitespace) must be trimmed before
// lookup, otherwise only the first entry is recognized.
TEST(MimeTypeListToContentTypeSetTest, WhitespaceAroundEntries) {
  GoogleString s = "text/html, application/xhtml+xml , image/gif";
  std::set<const ContentType*> out;

  MimeTypeListToContentTypeSet(s, &out);
  EXPECT_EQ(3, out.size());
  EXPECT_EQ(1, out.count(&kContentTypeHtml));
  EXPECT_EQ(1, out.count(&kContentTypeXhtml));
  EXPECT_EQ(1, out.count(&kContentTypeGif));
}

// Whitespace-only entries are dropped, not warned on or matched.
TEST(MimeTypeListToContentTypeSetTest, WhitespaceOnlyEntries) {
  GoogleString s = "image/gif, ,image/jpeg";
  std::set<const ContentType*> out;

  MimeTypeListToContentTypeSet(s, &out);
  EXPECT_EQ(2, out.size());
  EXPECT_EQ(1, out.count(&kContentTypeGif));
  EXPECT_EQ(1, out.count(&kContentTypeJpeg));
}

// A string with no parameters is all mime type; the charset output is
// cleared rather than left untouched.
TEST_F(ContentTypeTest, ParseContentTypeMimeOnly) {
  GoogleString mime_type = "sentinel";
  GoogleString charset = "sentinel";
  EXPECT_TRUE(ParseContentType("text/html", &mime_type, &charset));
  EXPECT_EQ("text/html", mime_type);
  EXPECT_EQ("", charset);
}

TEST_F(ContentTypeTest, ParseContentTypeCharsetVariants) {
  GoogleString mime_type;
  GoogleString charset;
  // Whitespace around the parameter name and value is trimmed; the value
  // keeps its case.
  EXPECT_TRUE(
      ParseContentType("text/html; charset=UTF-8", &mime_type, &charset));
  EXPECT_EQ("text/html", mime_type);
  EXPECT_EQ("UTF-8", charset);

  EXPECT_TRUE(ParseContentType("text/css;charset=utf-8", &mime_type, &charset));
  EXPECT_EQ("text/css", mime_type);
  EXPECT_EQ("utf-8", charset);

  // The parameter name is matched case-insensitively.
  EXPECT_TRUE(ParseContentType("text/html; CHARSET = iso-8859-1", &mime_type,
                               &charset));
  EXPECT_EQ("text/html", mime_type);
  EXPECT_EQ("iso-8859-1", charset);

  // Parameters other than charset are ignored.
  EXPECT_TRUE(ParseContentType("text/html; foo=bar; charset=utf-8", &mime_type,
                               &charset));
  EXPECT_EQ("text/html", mime_type);
  EXPECT_EQ("utf-8", charset);

  // Only the first charset parameter is used.
  EXPECT_TRUE(ParseContentType("text/html; charset=utf-8; charset=shift_jis",
                               &mime_type, &charset));
  EXPECT_EQ("text/html", mime_type);
  EXPECT_EQ("utf-8", charset);

  // A charset parameter with an empty value extracts an empty charset.
  EXPECT_TRUE(ParseContentType("text/html; charset=", &mime_type, &charset));
  EXPECT_EQ("text/html", mime_type);
  EXPECT_EQ("", charset);
}

TEST_F(ContentTypeTest, ParseContentTypeMalformed) {
  GoogleString mime_type = "sentinel";
  GoogleString charset = "sentinel";
  // Empty input fails and clears both outputs.
  EXPECT_FALSE(ParseContentType("", &mime_type, &charset));
  EXPECT_EQ("", mime_type);
  EXPECT_EQ("", charset);

  // A lone separator yields neither a mime type nor a charset.
  EXPECT_FALSE(ParseContentType(";", &mime_type, &charset));
  EXPECT_EQ("", mime_type);
  EXPECT_EQ("", charset);

  // A parameter without '=' cannot be a charset parameter.
  EXPECT_TRUE(ParseContentType("text/html; charset", &mime_type, &charset));
  EXPECT_EQ("text/html", mime_type);
  EXPECT_EQ("", charset);

  // A value containing '=' does not split into exactly two pieces, so no
  // charset is extracted.
  EXPECT_TRUE(
      ParseContentType("text/html; charset=utf-8=extra", &mime_type, &charset));
  EXPECT_EQ("text/html", mime_type);
  EXPECT_EQ("", charset);
}

TEST_F(ContentTypeTest, IsCss) {
  EXPECT_TRUE(kContentTypeCss.IsCss());
  EXPECT_FALSE(kContentTypeHtml.IsCss());
  EXPECT_FALSE(kContentTypeJavascript.IsCss());
  EXPECT_FALSE(kContentTypePng.IsCss());
}

TEST_F(ContentTypeTest, IsJsLike) {
  EXPECT_TRUE(kContentTypeJavascript.IsJsLike());
  EXPECT_TRUE(kContentTypeJson.IsJsLike());
  // Source maps use the application/javascript mime type but are a distinct
  // type, so they do not count as JS-like.
  EXPECT_FALSE(kContentTypeSourceMap.IsJsLike());
  EXPECT_FALSE(kContentTypeCss.IsJsLike());
  EXPECT_FALSE(kContentTypeHtml.IsJsLike());
}

TEST_F(ContentTypeTest, IsFlash) {
  EXPECT_TRUE(kContentTypeSwf.IsFlash());
  EXPECT_FALSE(kContentTypeHtml.IsFlash());
  EXPECT_FALSE(kContentTypeJavascript.IsFlash());
  EXPECT_FALSE(kContentTypePng.IsFlash());
}

// The bound must cover the longest extension filters can produce, excluding
// the dot: "jpeg", "webp" and "avif" are all 4 characters.
TEST_F(ContentTypeTest, MaxProducedExtensionLength) {
  EXPECT_EQ(4, ContentType::MaxProducedExtensionLength());
  EXPECT_LE(static_cast<int>(strlen(kContentTypeWebp.file_extension())) - 1,
            ContentType::MaxProducedExtensionLength());
  EXPECT_LE(static_cast<int>(strlen(kContentTypeAvif.file_extension())) - 1,
            ContentType::MaxProducedExtensionLength());
  const ContentType* jpeg = NameExtensionToContentType("a.jpeg");
  ASSERT_TRUE(jpeg != nullptr);
  EXPECT_LE(static_cast<int>(strlen(jpeg->file_extension())) - 1,
            ContentType::MaxProducedExtensionLength());
}

TEST_F(ContentTypeTest, IsCompressible) {
  // Text-ish types are compressible.
  EXPECT_TRUE(kContentTypeHtml.IsCompressible());
  EXPECT_TRUE(kContentTypeXhtml.IsCompressible());
  EXPECT_TRUE(kContentTypeCeHtml.IsCompressible());
  EXPECT_TRUE(kContentTypeXml.IsCompressible());
  EXPECT_TRUE(kContentTypeJavascript.IsCompressible());
  EXPECT_TRUE(kContentTypeJson.IsCompressible());
  EXPECT_TRUE(kContentTypeCss.IsCompressible());
  EXPECT_TRUE(kContentTypeText.IsCompressible());

  // Images, flash, pdf, video, audio and opaque binaries are not.
  EXPECT_FALSE(kContentTypePng.IsCompressible());
  EXPECT_FALSE(kContentTypeGif.IsCompressible());
  EXPECT_FALSE(kContentTypeJpeg.IsCompressible());
  EXPECT_FALSE(kContentTypeWebp.IsCompressible());
  EXPECT_FALSE(kContentTypeAvif.IsCompressible());
  EXPECT_FALSE(kContentTypeIco.IsCompressible());
  EXPECT_FALSE(kContentTypeSwf.IsCompressible());
  EXPECT_FALSE(kContentTypePdf.IsCompressible());
  EXPECT_FALSE(kContentTypeBinaryOctetStream.IsCompressible());
  const ContentType* mp4 = MimeTypeToContentType("video/mp4");
  ASSERT_TRUE(mp4 != nullptr);
  EXPECT_FALSE(mp4->IsCompressible());
  const ContentType* mp3 = MimeTypeToContentType("audio/mpeg");
  ASSERT_TRUE(mp3 != nullptr);
  EXPECT_FALSE(mp3->IsCompressible());
}

TEST_F(ContentTypeTest, IsLikelyStaticResource) {
  // Styles, scripts, images, flash, pdf, video and audio are static.
  EXPECT_TRUE(kContentTypeCss.IsLikelyStaticResource());
  EXPECT_TRUE(kContentTypeJavascript.IsLikelyStaticResource());
  EXPECT_TRUE(kContentTypePng.IsLikelyStaticResource());
  EXPECT_TRUE(kContentTypeGif.IsLikelyStaticResource());
  EXPECT_TRUE(kContentTypeJpeg.IsLikelyStaticResource());
  EXPECT_TRUE(kContentTypeWebp.IsLikelyStaticResource());
  EXPECT_TRUE(kContentTypeAvif.IsLikelyStaticResource());
  EXPECT_TRUE(kContentTypeIco.IsLikelyStaticResource());
  EXPECT_TRUE(kContentTypeSwf.IsLikelyStaticResource());
  EXPECT_TRUE(kContentTypePdf.IsLikelyStaticResource());
  const ContentType* mp4 = MimeTypeToContentType("video/mp4");
  ASSERT_TRUE(mp4 != nullptr);
  EXPECT_TRUE(mp4->IsLikelyStaticResource());
  const ContentType* mp3 = MimeTypeToContentType("audio/mpeg");
  ASSERT_TRUE(mp3 != nullptr);
  EXPECT_TRUE(mp3->IsLikelyStaticResource());

  // Markup, plain text, xml, json, source maps and unknown or opaque types
  // are not.
  EXPECT_FALSE(kContentTypeHtml.IsLikelyStaticResource());
  EXPECT_FALSE(kContentTypeXhtml.IsLikelyStaticResource());
  EXPECT_FALSE(kContentTypeCeHtml.IsLikelyStaticResource());
  EXPECT_FALSE(kContentTypeText.IsLikelyStaticResource());
  EXPECT_FALSE(kContentTypeXml.IsLikelyStaticResource());
  EXPECT_FALSE(kContentTypeJson.IsLikelyStaticResource());
  EXPECT_FALSE(kContentTypeSourceMap.IsLikelyStaticResource());
  EXPECT_FALSE(kContentTypeBinaryOctetStream.IsLikelyStaticResource());
  const ContentType kUnrecognized = {"application/x-unknown", ".unknown",
                                     ContentType::kOther};
  EXPECT_FALSE(kUnrecognized.IsLikelyStaticResource());
}

}  // namespace net_instaweb
