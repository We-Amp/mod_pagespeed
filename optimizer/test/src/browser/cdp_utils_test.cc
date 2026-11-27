// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

// Tests for CDP (Chrome DevTools Protocol) utility functions.
// Base64Encode is used to encode resource bodies in CDP network responses.
// GuessContentType maps file extensions to MIME types for CDP resource metadata.

#include "src/browser/cdp_utils.h"

#include <string>
#include <string_view>

#include "gtest/gtest.h"

namespace pagespeed::cdp_utils {
namespace {

// --- Base64Encode tests ---

TEST(Base64EncodeTest, EmptyInput) { EXPECT_EQ("", Base64Encode("")); }

TEST(Base64EncodeTest, SingleByte) {
  // 'A' = 0x41 -> 010000 01xxxx -> 'Q', 'Q', '=', '='
  EXPECT_EQ("QQ==", Base64Encode("A"));
}

TEST(Base64EncodeTest, TwoBytes) {
  // "AB" -> 010000 010100 0010xx -> 'Q', 'U', 'I', '='
  EXPECT_EQ("QUI=", Base64Encode("AB"));
}

TEST(Base64EncodeTest, ThreeBytes) {
  // "ABC" -> full 3-byte group, no padding.
  EXPECT_EQ("QUJD", Base64Encode("ABC"));
}

TEST(Base64EncodeTest, FourBytes) {
  // 4 bytes = 1 full group (3 bytes) + 1 remainder.
  EXPECT_EQ("QUJDRA==", Base64Encode("ABCD"));
}

TEST(Base64EncodeTest, FiveBytes) {
  // 5 bytes = 1 full group (3 bytes) + 2-byte remainder.
  EXPECT_EQ("QUJDREU=", Base64Encode("ABCDE"));
}

TEST(Base64EncodeTest, SixBytes) {
  // 6 bytes = 2 full groups, no padding.
  EXPECT_EQ("QUJDREVG", Base64Encode("ABCDEF"));
}

TEST(Base64EncodeTest, HelloWorld) {
  EXPECT_EQ("SGVsbG8gV29ybGQ=", Base64Encode("Hello World"));
}

TEST(Base64EncodeTest, BinaryData) {
  // All-zero bytes.
  std::string zeros(3, '\0');
  EXPECT_EQ("AAAA", Base64Encode(zeros));
}

TEST(Base64EncodeTest, AllByteValues) {
  // Encode bytes 0x00..0xFF and verify the result is non-empty and
  // has the expected length (4 * ceil(256/3) = 4 * 86 = 344).
  std::string all_bytes;
  for (int i = 0; i < 256; ++i) {
    all_bytes.push_back(static_cast<char>(i));
  }
  std::string encoded = Base64Encode(all_bytes);
  // 256 bytes: 85 full groups (255 bytes) + 1 remainder = 85*4 + 4 = 344.
  EXPECT_EQ(344u, encoded.size());

  // Verify only valid base64 characters + padding at end.
  for (size_t i = 0; i < encoded.size(); ++i) {
    char c = encoded[i];
    bool valid = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                 (c >= '0' && c <= '9') || c == '+' || c == '/' || c == '=';
    EXPECT_TRUE(valid) << "Invalid base64 char at position " << i << ": '" << c
                       << "'";
  }
}

TEST(Base64EncodeTest, PaddingAlignment) {
  // 1 byte remainder -> "==" padding.
  EXPECT_TRUE(Base64Encode("x").ends_with("=="));
  // 2 byte remainder -> "=" padding.
  EXPECT_TRUE(Base64Encode("xy").ends_with("="));
  // 0 byte remainder -> no padding.
  EXPECT_FALSE(Base64Encode("xyz").ends_with("="));
}

TEST(Base64EncodeTest, HighBitBytes) {
  // Bytes with high bit set: 0xFF, 0xFE, 0xFD.
  std::string data = {'\xFF', '\xFE', '\xFD'};
  std::string encoded = Base64Encode(data);
  EXPECT_EQ(4u, encoded.size());
  EXPECT_FALSE(encoded.ends_with("="));
}

// --- GuessContentType tests ---

TEST(GuessContentTypeTest, CssExtension) {
  EXPECT_EQ("text/css", GuessContentType("/path/to/style.css"));
}

TEST(GuessContentTypeTest, JsExtension) {
  EXPECT_EQ("application/javascript", GuessContentType("/app.js"));
}

TEST(GuessContentTypeTest, PngExtension) {
  EXPECT_EQ("image/png", GuessContentType("/image.png"));
}

TEST(GuessContentTypeTest, JpgExtension) {
  EXPECT_EQ("image/jpeg", GuessContentType("/photo.jpg"));
}

TEST(GuessContentTypeTest, JpegExtension) {
  EXPECT_EQ("image/jpeg", GuessContentType("/photo.jpeg"));
}

TEST(GuessContentTypeTest, GifExtension) {
  EXPECT_EQ("image/gif", GuessContentType("/anim.gif"));
}

TEST(GuessContentTypeTest, SvgExtension) {
  EXPECT_EQ("image/svg+xml", GuessContentType("/icon.svg"));
}

TEST(GuessContentTypeTest, WebpExtension) {
  EXPECT_EQ("image/webp", GuessContentType("/image.webp"));
}

TEST(GuessContentTypeTest, AvifExtension) {
  EXPECT_EQ("image/avif", GuessContentType("/image.avif"));
}

TEST(GuessContentTypeTest, Woff2Extension) {
  EXPECT_EQ("font/woff2", GuessContentType("/font.woff2"));
}

TEST(GuessContentTypeTest, WoffExtension) {
  EXPECT_EQ("font/woff", GuessContentType("/font.woff"));
}

TEST(GuessContentTypeTest, HtmlExtension) {
  EXPECT_EQ("text/html", GuessContentType("/page.html"));
}

TEST(GuessContentTypeTest, UnknownExtension) {
  EXPECT_EQ("application/octet-stream", GuessContentType("/file.xyz"));
}

TEST(GuessContentTypeTest, NoExtension) {
  EXPECT_EQ("application/octet-stream", GuessContentType("/no-extension"));
}

TEST(GuessContentTypeTest, NoDot) {
  EXPECT_EQ("application/octet-stream", GuessContentType("filename"));
}

TEST(GuessContentTypeTest, QueryStringStripped) {
  // Extension should be recognized even with a query string.
  EXPECT_EQ("text/css", GuessContentType("/style.css?v=123"));
}

TEST(GuessContentTypeTest, QueryStringOnJs) {
  EXPECT_EQ("application/javascript", GuessContentType("/app.js?hash=abc123"));
}

TEST(GuessContentTypeTest, PathWithMultipleDots) {
  // Should use the last dot for extension detection.
  EXPECT_EQ("text/css", GuessContentType("/path/to/file.min.css"));
}

TEST(GuessContentTypeTest, FullUrl) {
  EXPECT_EQ("image/png",
            GuessContentType("https://cdn.example.com/img/logo.png"));
}

TEST(GuessContentTypeTest, UrlWithPort) {
  EXPECT_EQ("application/javascript",
            GuessContentType("http://localhost:8080/bundle.js"));
}

TEST(GuessContentTypeTest, EmptyString) {
  EXPECT_EQ("application/octet-stream", GuessContentType(""));
}

TEST(GuessContentTypeTest, DotOnly) {
  // Just a dot - extension is empty, should return fallback.
  EXPECT_EQ("application/octet-stream", GuessContentType("."));
}

TEST(GuessContentTypeTest, EndsWithDot) {
  EXPECT_EQ("application/octet-stream", GuessContentType("/file."));
}

// --- Additional edge case tests ---

TEST(Base64EncodeTest, LargeInput) {
  // Encode a large buffer and verify the output length.
  std::string large(10000, 'x');
  std::string encoded = Base64Encode(large);
  // Expected: ceil(10000/3) * 4 = 3334 * 4 = 13336.
  // 10000 / 3 = 3333 remainder 1 -> 3333*4 + 4 = 13336.
  EXPECT_EQ(13336u, encoded.size());
}

TEST(Base64EncodeTest, NullBytesInInput) {
  // Input containing null bytes should encode correctly.
  std::string data = {'\0', '\0', '\0'};
  EXPECT_EQ("AAAA", Base64Encode(data));
}

TEST(Base64EncodeTest, MixedNullAndData) {
  std::string data = {'\0', 'A', '\0'};
  std::string encoded = Base64Encode(data);
  EXPECT_EQ(4u, encoded.size());
  EXPECT_FALSE(encoded.ends_with("="));
}

TEST(GuessContentTypeTest, UrlWithFragment) {
  // GuessContentType only strips query strings (?), not fragments (#).
  // A fragment after the extension makes it unrecognized.
  EXPECT_EQ("application/octet-stream", GuessContentType("/style.css#section"));
}

TEST(GuessContentTypeTest, UrlWithQueryAndFragment) {
  // Query string is stripped first, so ?v=1#main is removed and .js matches.
  // The query starts at '?' which is before '#', so ext = ".js".
  EXPECT_EQ("application/javascript", GuessContentType("/app.js?v=1#main"));
}

TEST(GuessContentTypeTest, UrlWithMultipleQueryParams) {
  EXPECT_EQ("image/png",
            GuessContentType("/image.png?w=100&h=200&format=auto"));
}

TEST(GuessContentTypeTest, Dotfile) {
  // Hidden file (starts with dot, no extension after).
  EXPECT_EQ("application/octet-stream", GuessContentType("/.htaccess"));
}

TEST(GuessContentTypeTest, DoubleExtension) {
  EXPECT_EQ("application/javascript", GuessContentType("/bundle.min.js"));
}

TEST(GuessContentTypeTest, LongPath) {
  EXPECT_EQ("image/webp",
            GuessContentType("/a/very/long/path/to/the/image.webp"));
}

}  // namespace
}  // namespace pagespeed::cdp_utils
