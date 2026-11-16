// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

// Test coverage for lib/html/content_type.cc
// Covers: type classification, extension lookup, MIME lookup, parsing, and
// adversarial inputs.

#include "lib/html/content_type.h"

#include <set>
#include <string>

#include "gtest/gtest.h"

namespace net_instaweb {
namespace {

// --- Type classification methods ---

TEST(ContentTypeTest, IsCss) {
  EXPECT_TRUE(kContentTypeCss.IsCss());
  EXPECT_FALSE(kContentTypeHtml.IsCss());
  EXPECT_FALSE(kContentTypeJavascript.IsCss());
  EXPECT_FALSE(kContentTypePng.IsCss());
}

TEST(ContentTypeTest, IsJsLike) {
  EXPECT_TRUE(kContentTypeJavascript.IsJsLike());
  EXPECT_TRUE(kContentTypeJson.IsJsLike());
  EXPECT_FALSE(kContentTypeCss.IsJsLike());
  EXPECT_FALSE(kContentTypeHtml.IsJsLike());
  EXPECT_FALSE(kContentTypeSourceMap.IsJsLike());
}

TEST(ContentTypeTest, IsHtmlLike) {
  EXPECT_TRUE(kContentTypeHtml.IsHtmlLike());
  EXPECT_TRUE(kContentTypeXhtml.IsHtmlLike());
  EXPECT_TRUE(kContentTypeCeHtml.IsHtmlLike());
  EXPECT_FALSE(kContentTypeXml.IsHtmlLike());
  EXPECT_FALSE(kContentTypeCss.IsHtmlLike());
  EXPECT_FALSE(kContentTypeJavascript.IsHtmlLike());
}

TEST(ContentTypeTest, IsXmlLike) {
  EXPECT_TRUE(kContentTypeXhtml.IsXmlLike());
  EXPECT_TRUE(kContentTypeXml.IsXmlLike());
  EXPECT_FALSE(kContentTypeHtml.IsXmlLike());
  EXPECT_TRUE(kContentTypeCeHtml.IsXmlLike());
  EXPECT_FALSE(kContentTypePng.IsXmlLike());
}

TEST(ContentTypeTest, IsFlash) {
  EXPECT_TRUE(kContentTypeSwf.IsFlash());
  EXPECT_FALSE(kContentTypePng.IsFlash());
  EXPECT_FALSE(kContentTypeHtml.IsFlash());
}

TEST(ContentTypeTest, IsImage) {
  EXPECT_TRUE(kContentTypePng.IsImage());
  EXPECT_TRUE(kContentTypeGif.IsImage());
  EXPECT_TRUE(kContentTypeJpeg.IsImage());
  EXPECT_TRUE(kContentTypeWebp.IsImage());
  EXPECT_TRUE(kContentTypeAvif.IsImage());
  EXPECT_FALSE(kContentTypeSwf.IsImage());
  EXPECT_FALSE(kContentTypeIco.IsImage());
  EXPECT_FALSE(kContentTypeHtml.IsImage());
  EXPECT_FALSE(kContentTypePdf.IsImage());
}

TEST(ContentTypeTest, IsVideo) {
  const ContentType* mp4 = MimeTypeToContentType("video/mp4");
  ASSERT_NE(nullptr, mp4);
  EXPECT_TRUE(mp4->IsVideo());
  EXPECT_FALSE(kContentTypeHtml.IsVideo());
  EXPECT_FALSE(kContentTypePng.IsVideo());
}

TEST(ContentTypeTest, IsAudio) {
  const ContentType* mp3 = MimeTypeToContentType("audio/mpeg");
  ASSERT_NE(nullptr, mp3);
  EXPECT_TRUE(mp3->IsAudio());
  EXPECT_FALSE(kContentTypeHtml.IsAudio());
  EXPECT_FALSE(kContentTypePng.IsAudio());
}

TEST(ContentTypeTest, IsCompressible) {
  EXPECT_TRUE(kContentTypeHtml.IsCompressible());
  EXPECT_TRUE(kContentTypeXhtml.IsCompressible());
  EXPECT_TRUE(kContentTypeCeHtml.IsCompressible());
  EXPECT_TRUE(kContentTypeXml.IsCompressible());
  EXPECT_TRUE(kContentTypeJavascript.IsCompressible());
  EXPECT_TRUE(kContentTypeJson.IsCompressible());
  EXPECT_TRUE(kContentTypeCss.IsCompressible());
  EXPECT_TRUE(kContentTypeText.IsCompressible());
  EXPECT_FALSE(kContentTypePng.IsCompressible());
  EXPECT_FALSE(kContentTypeJpeg.IsCompressible());
  EXPECT_FALSE(kContentTypeWebp.IsCompressible());
  EXPECT_FALSE(kContentTypePdf.IsCompressible());
  EXPECT_FALSE(kContentTypeBinaryOctetStream.IsCompressible());
}

TEST(ContentTypeTest, IsLikelyStaticResource) {
  // Static resources
  EXPECT_TRUE(kContentTypeCss.IsLikelyStaticResource());
  EXPECT_TRUE(kContentTypeJavascript.IsLikelyStaticResource());
  EXPECT_TRUE(kContentTypePng.IsLikelyStaticResource());
  EXPECT_TRUE(kContentTypeGif.IsLikelyStaticResource());
  EXPECT_TRUE(kContentTypeJpeg.IsLikelyStaticResource());
  EXPECT_TRUE(kContentTypeWebp.IsLikelyStaticResource());
  EXPECT_TRUE(kContentTypeAvif.IsLikelyStaticResource());
  EXPECT_TRUE(kContentTypeSwf.IsLikelyStaticResource());
  EXPECT_TRUE(kContentTypeIco.IsLikelyStaticResource());
  EXPECT_TRUE(kContentTypePdf.IsLikelyStaticResource());

  // Dynamic/non-static
  EXPECT_FALSE(kContentTypeHtml.IsLikelyStaticResource());
  EXPECT_FALSE(kContentTypeXhtml.IsLikelyStaticResource());
  EXPECT_FALSE(kContentTypeCeHtml.IsLikelyStaticResource());
  EXPECT_FALSE(kContentTypeXml.IsLikelyStaticResource());
  EXPECT_FALSE(kContentTypeJson.IsLikelyStaticResource());
  EXPECT_FALSE(kContentTypeSourceMap.IsLikelyStaticResource());
  EXPECT_FALSE(kContentTypeText.IsLikelyStaticResource());
  EXPECT_FALSE(kContentTypeBinaryOctetStream.IsLikelyStaticResource());
}

// --- MaxProducedExtensionLength ---

TEST(ContentTypeTest, MaxProducedExtensionLength) {
  EXPECT_EQ(4, ContentType::MaxProducedExtensionLength());
}

// --- NameExtensionToContentType ---

TEST(ContentTypeTest, ExtensionLookupBasic) {
  EXPECT_EQ(ContentType::kHtml,
            NameExtensionToContentType("page.html")->type());
  EXPECT_EQ(ContentType::kCss, NameExtensionToContentType("style.css")->type());
  EXPECT_EQ(ContentType::kJavascript,
            NameExtensionToContentType("app.js")->type());
  EXPECT_EQ(ContentType::kPng, NameExtensionToContentType("img.png")->type());
  EXPECT_EQ(ContentType::kGif, NameExtensionToContentType("anim.gif")->type());
  EXPECT_EQ(ContentType::kJpeg,
            NameExtensionToContentType("photo.jpg")->type());
  EXPECT_EQ(ContentType::kJpeg,
            NameExtensionToContentType("photo.jpeg")->type());
  EXPECT_EQ(ContentType::kWebp, NameExtensionToContentType("img.webp")->type());
  EXPECT_EQ(ContentType::kIco,
            NameExtensionToContentType("favicon.ico")->type());
  EXPECT_EQ(ContentType::kPdf, NameExtensionToContentType("doc.pdf")->type());
  EXPECT_EQ(ContentType::kJson,
            NameExtensionToContentType("data.json")->type());
  EXPECT_EQ(ContentType::kSourceMap,
            NameExtensionToContentType("app.map")->type());
  EXPECT_EQ(ContentType::kSwf, NameExtensionToContentType("flash.swf")->type());
  EXPECT_EQ(ContentType::kText,
            NameExtensionToContentType("readme.txt")->type());
  EXPECT_EQ(ContentType::kXml, NameExtensionToContentType("feed.xml")->type());
  EXPECT_EQ(ContentType::kXhtml,
            NameExtensionToContentType("page.xhtml")->type());
}

TEST(ContentTypeTest, ExtensionLookupCaseInsensitive) {
  EXPECT_EQ(ContentType::kHtml,
            NameExtensionToContentType("page.HTML")->type());
  EXPECT_EQ(ContentType::kJpeg,
            NameExtensionToContentType("photo.JPG")->type());
  EXPECT_EQ(ContentType::kPng, NameExtensionToContentType("img.PNG")->type());
  EXPECT_EQ(ContentType::kCss, NameExtensionToContentType("style.Css")->type());
}

TEST(ContentTypeTest, ExtensionLookupWithPath) {
  EXPECT_EQ(ContentType::kHtml,
            NameExtensionToContentType("/path/to/page.html")->type());
  EXPECT_EQ(
      ContentType::kJpeg,
      NameExtensionToContentType("https://example.com/photo.jpg")->type());
}

TEST(ContentTypeTest, ExtensionLookupUnknown) {
  EXPECT_EQ(nullptr, NameExtensionToContentType("file.xyz"));
  EXPECT_EQ(nullptr, NameExtensionToContentType("file.docx"));
  EXPECT_EQ(nullptr, NameExtensionToContentType("noextension"));
}

TEST(ContentTypeTest, ExtensionLookupVideoAudio) {
  EXPECT_EQ(ContentType::kVideo,
            NameExtensionToContentType("clip.mp4")->type());
  EXPECT_EQ(ContentType::kVideo,
            NameExtensionToContentType("clip.mpg")->type());
  EXPECT_EQ(ContentType::kVideo,
            NameExtensionToContentType("clip.webm")->type());
  EXPECT_EQ(ContentType::kVideo,
            NameExtensionToContentType("clip.mov")->type());
  EXPECT_EQ(ContentType::kAudio,
            NameExtensionToContentType("song.mp3")->type());
  EXPECT_EQ(ContentType::kAudio,
            NameExtensionToContentType("sound.wav")->type());
}

TEST(ContentTypeTest, ExtensionLookupSvg) {
  const ContentType* svg = NameExtensionToContentType("drawing.svg");
  ASSERT_NE(nullptr, svg);
  EXPECT_EQ(ContentType::kXml, svg->type());
}

// --- MimeTypeToContentType ---

TEST(ContentTypeTest, MimeLookupBasic) {
  EXPECT_EQ(ContentType::kHtml, MimeTypeToContentType("text/html")->type());
  EXPECT_EQ(ContentType::kCss, MimeTypeToContentType("text/css")->type());
  EXPECT_EQ(ContentType::kJavascript,
            MimeTypeToContentType("application/javascript")->type());
  EXPECT_EQ(ContentType::kPng, MimeTypeToContentType("image/png")->type());
  EXPECT_EQ(ContentType::kGif, MimeTypeToContentType("image/gif")->type());
  EXPECT_EQ(ContentType::kJpeg, MimeTypeToContentType("image/jpeg")->type());
  EXPECT_EQ(ContentType::kWebp, MimeTypeToContentType("image/webp")->type());
  EXPECT_EQ(ContentType::kJson,
            MimeTypeToContentType("application/json")->type());
  EXPECT_EQ(ContentType::kPdf,
            MimeTypeToContentType("application/pdf")->type());
}

TEST(ContentTypeTest, MimeLookupWithCharset) {
  EXPECT_EQ(ContentType::kHtml,
            MimeTypeToContentType("text/html; charset=utf-8")->type());
  EXPECT_EQ(ContentType::kCss,
            MimeTypeToContentType("text/css; charset=UTF-8")->type());
  EXPECT_EQ(ContentType::kJson,
            MimeTypeToContentType("application/json; charset=utf-8")->type());
}

TEST(ContentTypeTest, MimeLookupCaseInsensitive) {
  EXPECT_EQ(ContentType::kHtml, MimeTypeToContentType("TEXT/HTML")->type());
  EXPECT_EQ(ContentType::kPng, MimeTypeToContentType("Image/PNG")->type());
  EXPECT_EQ(ContentType::kCss, MimeTypeToContentType("Text/Css")->type());
}

TEST(ContentTypeTest, MimeLookupJsSynonyms) {
  EXPECT_EQ(ContentType::kJavascript,
            MimeTypeToContentType("text/javascript")->type());
  EXPECT_EQ(ContentType::kJavascript,
            MimeTypeToContentType("application/x-javascript")->type());
  EXPECT_EQ(ContentType::kJavascript,
            MimeTypeToContentType("text/x-javascript")->type());
  EXPECT_EQ(ContentType::kJavascript,
            MimeTypeToContentType("text/ecmascript")->type());
  EXPECT_EQ(ContentType::kJavascript,
            MimeTypeToContentType("application/ecmascript")->type());
  EXPECT_EQ(ContentType::kJavascript, MimeTypeToContentType("text/js")->type());
  EXPECT_EQ(ContentType::kJavascript,
            MimeTypeToContentType("text/jscript")->type());
  EXPECT_EQ(ContentType::kJavascript,
            MimeTypeToContentType("text/x-js")->type());
}

TEST(ContentTypeTest, MimeLookupJsonSynonyms) {
  EXPECT_EQ(ContentType::kJson,
            MimeTypeToContentType("application/json")->type());
  EXPECT_EQ(ContentType::kJson,
            MimeTypeToContentType("application/x-json")->type());
}

TEST(ContentTypeTest, JsonCanonicalMimeType) {
  // Regression test: the canonical JSON entry used
  // to carry "application/javascript", so kContentTypeJson.mime_type() lied.
  EXPECT_STREQ("application/json", kContentTypeJson.mime_type());
  EXPECT_EQ(&kContentTypeJson, MimeTypeToContentType("application/json"));
}

TEST(ContentTypeTest, MimeLookupImageSynonyms) {
  EXPECT_EQ(ContentType::kJpeg, MimeTypeToContentType("image/jpg")->type());
  EXPECT_EQ(ContentType::kIco,
            MimeTypeToContentType("image/vnd.microsoft.icon")->type());
  EXPECT_EQ(ContentType::kIco, MimeTypeToContentType("image/x-icon")->type());
}

TEST(ContentTypeTest, AvifLookup) {
  // Ported from mod_pagespeed 1.15.
  const ContentType* by_mime = MimeTypeToContentType("image/avif");
  ASSERT_NE(nullptr, by_mime);
  EXPECT_EQ(ContentType::kAvif, by_mime->type());
  EXPECT_TRUE(by_mime->IsImage());

  const ContentType* by_ext = NameExtensionToContentType("image.avif");
  ASSERT_NE(nullptr, by_ext);
  EXPECT_EQ(ContentType::kAvif, by_ext->type());

  EXPECT_EQ(ContentType::kAvif, kContentTypeAvif.type());
}

TEST(ContentTypeTest, MimeLookupOctetStreamSynonyms) {
  EXPECT_EQ(ContentType::kOctetStream,
            MimeTypeToContentType("application/octet-stream")->type());
  EXPECT_EQ(ContentType::kOctetStream,
            MimeTypeToContentType("binary/octet-stream")->type());
}

TEST(ContentTypeTest, MimeLookupUnknown) {
  EXPECT_EQ(nullptr, MimeTypeToContentType("application/x-unknown"));
  EXPECT_EQ(nullptr, MimeTypeToContentType("foo/bar"));
  EXPECT_EQ(nullptr, MimeTypeToContentType(""));
}

TEST(ContentTypeTest, MimeLookupVideoAudio) {
  EXPECT_EQ(ContentType::kVideo, MimeTypeToContentType("video/mp4")->type());
  EXPECT_EQ(ContentType::kVideo, MimeTypeToContentType("video/webm")->type());
  EXPECT_EQ(ContentType::kVideo,
            MimeTypeToContentType("video/quicktime")->type());
  EXPECT_EQ(ContentType::kAudio, MimeTypeToContentType("audio/mpeg")->type());
  EXPECT_EQ(ContentType::kAudio, MimeTypeToContentType("audio/wav")->type());
  EXPECT_EQ(ContentType::kAudio, MimeTypeToContentType("audio/ogg")->type());
}

TEST(ContentTypeTest, MimeLookupSvg) {
  const ContentType* svg = MimeTypeToContentType("image/svg+xml");
  ASSERT_NE(nullptr, svg);
  EXPECT_EQ(ContentType::kXml, svg->type());
}

// --- ParseContentType ---

TEST(ContentTypeTest, ParseContentTypeBasic) {
  std::string mime, charset;
  EXPECT_TRUE(ParseContentType("text/html", &mime, &charset));
  EXPECT_EQ("text/html", mime);
  EXPECT_TRUE(charset.empty());
}

TEST(ContentTypeTest, ParseContentTypeWithCharset) {
  std::string mime, charset;
  EXPECT_TRUE(ParseContentType("text/html; charset=utf-8", &mime, &charset));
  EXPECT_EQ("text/html", mime);
  EXPECT_EQ("utf-8", charset);
}

TEST(ContentTypeTest, ParseContentTypeCharsetCaseInsensitive) {
  std::string mime, charset;
  EXPECT_TRUE(ParseContentType("text/html; Charset=UTF-8", &mime, &charset));
  EXPECT_EQ("text/html", mime);
  EXPECT_EQ("UTF-8", charset);
}

TEST(ContentTypeTest, ParseContentTypeMultipleParams) {
  std::string mime, charset;
  EXPECT_TRUE(ParseContentType("text/html; boundary=something; charset=utf-8",
                               &mime, &charset));
  EXPECT_EQ("text/html", mime);
  EXPECT_EQ("utf-8", charset);
}

TEST(ContentTypeTest, ParseContentTypeEmpty) {
  std::string mime, charset;
  EXPECT_FALSE(ParseContentType("", &mime, &charset));
  EXPECT_TRUE(mime.empty());
  EXPECT_TRUE(charset.empty());
}

TEST(ContentTypeTest, ParseContentTypeMimeOnly) {
  std::string mime, charset;
  EXPECT_TRUE(ParseContentType("application/json", &mime, &charset));
  EXPECT_EQ("application/json", mime);
  EXPECT_TRUE(charset.empty());
}

TEST(ContentTypeTest, ParseContentTypeWhitespace) {
  std::string mime, charset;
  EXPECT_TRUE(
      ParseContentType("text/html;  charset = utf-8 ", &mime, &charset));
  EXPECT_EQ("text/html", mime);
  EXPECT_EQ("utf-8", charset);
}

TEST(ContentTypeTest, ParseContentTypeDoubleSemicolon) {
  std::string mime, charset;
  EXPECT_TRUE(ParseContentType("text/html;; charset=utf-8", &mime, &charset));
  EXPECT_EQ("text/html", mime);
  EXPECT_EQ("utf-8", charset);
}

// --- MimeTypeListToContentTypeSet ---

TEST(ContentTypeTest, MimeTypeListEmpty) {
  std::set<const ContentType*> out;
  MimeTypeListToContentTypeSet("", &out);
  EXPECT_TRUE(out.empty());
}

TEST(ContentTypeTest, MimeTypeListSingle) {
  std::set<const ContentType*> out;
  MimeTypeListToContentTypeSet("image/png", &out);
  EXPECT_EQ(1u, out.size());
  EXPECT_NE(out.end(), out.find(&kContentTypePng));
}

TEST(ContentTypeTest, MimeTypeListMultiple) {
  std::set<const ContentType*> out;
  MimeTypeListToContentTypeSet("image/gif,image/jpeg,image/png", &out);
  EXPECT_EQ(3u, out.size());
  EXPECT_NE(out.end(), out.find(&kContentTypeGif));
  EXPECT_NE(out.end(), out.find(&kContentTypeJpeg));
  EXPECT_NE(out.end(), out.find(&kContentTypePng));
}

TEST(ContentTypeTest, MimeTypeListWithWhitespace) {
  std::set<const ContentType*> out;
  MimeTypeListToContentTypeSet("image/gif , image/jpeg , image/png", &out);
  EXPECT_EQ(3u, out.size());
}

TEST(ContentTypeTest, MimeTypeListWithUnknown) {
  std::set<const ContentType*> out;
  MimeTypeListToContentTypeSet("image/gif,application/x-unknown,image/png",
                               &out);
  EXPECT_EQ(2u, out.size());
}

TEST(ContentTypeTest, MimeTypeListDuplicates) {
  std::set<const ContentType*> out;
  MimeTypeListToContentTypeSet("image/jpeg,image/jpeg,image/jpeg", &out);
  EXPECT_EQ(1u, out.size());
}

TEST(ContentTypeTest, MimeTypeListMalformedCommas) {
  std::set<const ContentType*> out;
  MimeTypeListToContentTypeSet("image/gif,,,,,image/png", &out);
  EXPECT_EQ(2u, out.size());
}

// --- Constant sanity checks ---

TEST(ContentTypeTest, ConstantMimeTypes) {
  EXPECT_STREQ("text/html", kContentTypeHtml.mime_type());
  EXPECT_STREQ("text/css", kContentTypeCss.mime_type());
  EXPECT_STREQ("application/javascript", kContentTypeJavascript.mime_type());
  EXPECT_STREQ("image/png", kContentTypePng.mime_type());
  EXPECT_STREQ("image/gif", kContentTypeGif.mime_type());
  EXPECT_STREQ("image/jpeg", kContentTypeJpeg.mime_type());
  EXPECT_STREQ("image/webp", kContentTypeWebp.mime_type());
  EXPECT_STREQ("image/avif", kContentTypeAvif.mime_type());
  EXPECT_STREQ("application/pdf", kContentTypePdf.mime_type());
}

TEST(ContentTypeTest, ConstantExtensions) {
  EXPECT_STREQ(".html", kContentTypeHtml.file_extension());
  EXPECT_STREQ(".css", kContentTypeCss.file_extension());
  EXPECT_STREQ(".js", kContentTypeJavascript.file_extension());
  EXPECT_STREQ(".png", kContentTypePng.file_extension());
  EXPECT_STREQ(".jpg", kContentTypeJpeg.file_extension());
  EXPECT_STREQ(".webp", kContentTypeWebp.file_extension());
  EXPECT_STREQ(".avif", kContentTypeAvif.file_extension());
}

// --- Adversarial inputs ---

TEST(ContentTypeTest, AdversarialLongMime) {
  std::string long_mime(10000, 'x');
  EXPECT_EQ(nullptr, MimeTypeToContentType(long_mime));
}

TEST(ContentTypeTest, AdversarialLongExtension) {
  std::string long_name = "file." + std::string(10000, 'x');
  EXPECT_EQ(nullptr, NameExtensionToContentType(long_name));
}

TEST(ContentTypeTest, AdversarialNullBytesInMime) {
  std::string with_null("text/html\0garbage", 17);
  // String comparison should fail due to length mismatch or embedded null
  EXPECT_EQ(nullptr, MimeTypeToContentType(with_null));
}

TEST(ContentTypeTest, ParseContentTypeLongInput) {
  std::string long_ct = "text/html; charset=" + std::string(10000, 'a');
  std::string mime, charset;
  EXPECT_TRUE(ParseContentType(long_ct, &mime, &charset));
  EXPECT_EQ("text/html", mime);
  EXPECT_EQ(std::string(10000, 'a'), charset);
}

}  // namespace
}  // namespace net_instaweb
