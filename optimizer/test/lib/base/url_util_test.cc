// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

// Tests for URL utility functions.

#include "lib/base/url_util.h"

#include "gtest/gtest.h"

namespace pagespeed {
namespace {

// =============================================================================
// UrlDirectory tests
// =============================================================================

TEST(UrlUtilTest, UrlDirectoryBasic) {
  EXPECT_EQ("/path/", UrlDirectory("/path/file.css"));
  EXPECT_EQ("/", UrlDirectory("/file.css"));
  EXPECT_EQ("https://example.com/path/",
            UrlDirectory("https://example.com/path/file.css"));
}

TEST(UrlUtilTest, UrlDirectoryStripQueryAndFragment) {
  EXPECT_EQ("/path/", UrlDirectory("/path/file.css?v=1#anchor"));
  EXPECT_EQ("/path/", UrlDirectory("/path/file.css?v=1"));
  EXPECT_EQ("/path/", UrlDirectory("/path/file.css#anchor"));
}

TEST(UrlUtilTest, UrlDirectoryNoSlash) {
  // No slash → empty result
  EXPECT_EQ("", UrlDirectory("file.css"));
  EXPECT_EQ("", UrlDirectory("file.css?query"));
  EXPECT_EQ("", UrlDirectory("#anchor"));
  EXPECT_EQ("", UrlDirectory("?query"));
}

TEST(UrlUtilTest, UrlDirectoryEmpty) { EXPECT_EQ("", UrlDirectory("")); }

TEST(UrlUtilTest, UrlDirectorySchemeRelative) {
  EXPECT_EQ("//example.com/path/", UrlDirectory("//example.com/path/file.css"));
}

// =============================================================================
// ResolvePath tests
// =============================================================================

TEST(UrlUtilTest, ResolvePathBasic) {
  EXPECT_EQ("/a/b/c.css", ResolvePath("/a/b/", "c.css"));
  EXPECT_EQ("/a/b/c/d.css", ResolvePath("/a/b/", "c/d.css"));
}

TEST(UrlUtilTest, ResolvePathEmpty) {
  EXPECT_EQ("/a/b/", ResolvePath("/a/b/", ""));
}

TEST(UrlUtilTest, ResolvePathAbsolute) {
  EXPECT_EQ("/c.css", ResolvePath("/a/b/", "/c.css"));
}

TEST(UrlUtilTest, ResolvePathAbsoluteUrl) {
  EXPECT_EQ("https://cdn.example.com/style.css",
            ResolvePath("/a/b/", "https://cdn.example.com/style.css"));
}

TEST(UrlUtilTest, ResolvePathDotDot) {
  EXPECT_EQ("/a/c.css", ResolvePath("/a/b/", "../c.css"));
  EXPECT_EQ("/c.css", ResolvePath("/a/b/", "../../c.css"));
}

TEST(UrlUtilTest, ResolvePathDotSlash) {
  EXPECT_EQ("/a/b/c.css", ResolvePath("/a/b/", "./c.css"));
}

TEST(UrlUtilTest, ResolvePathMultipleDotDot) {
  EXPECT_EQ("/a/x.css", ResolvePath("/a/b/c/", "../../x.css"));
}

TEST(UrlUtilTest, ResolvePathDotDotAtRoot) {
  // Traversal above root is stripped to prevent path traversal.
  EXPECT_EQ("/x.css", ResolvePath("/", "../x.css"));
  EXPECT_EQ("/etc/passwd", ResolvePath("/", "../../../etc/passwd"));
  EXPECT_EQ("/", ResolvePath("/a/", "../../.."));
}

// =============================================================================
// UrlHostname tests
// =============================================================================

TEST(UrlUtilTest, UrlHostnameHttp) {
  EXPECT_EQ("example.com", UrlHostname("http://example.com/path"));
  EXPECT_EQ("example.com", UrlHostname("http://example.com/"));
  EXPECT_EQ("example.com", UrlHostname("http://example.com"));
}

TEST(UrlUtilTest, UrlHostnameHttps) {
  EXPECT_EQ("cdn.example.com",
            UrlHostname("https://cdn.example.com/style.css"));
}

TEST(UrlUtilTest, UrlHostnameWithPort) {
  EXPECT_EQ("example.com:8080", UrlHostname("http://example.com:8080/path"));
}

TEST(UrlUtilTest, UrlHostnameStripsUserinfo) {
  EXPECT_EQ("example.com", UrlHostname("http://user@example.com/path"));
  EXPECT_EQ("example.com:8080",
            UrlHostname("http://user:pass@example.com:8080/path"));
}

TEST(UrlUtilTest, UrlHostnameRelative) {
  EXPECT_EQ("", UrlHostname("/css/style.css"));
  EXPECT_EQ("", UrlHostname("style.css"));
  EXPECT_EQ("", UrlHostname(""));
}

TEST(UrlUtilTest, UrlHostnameSchemeRelative) {
  // Known limitation: scheme-relative URLs lack "://" so UrlHostname returns
  // empty. Callers needing scheme-relative support must handle this separately.
  EXPECT_EQ("", UrlHostname("//cdn.example.com/path"));
}

TEST(UrlUtilTest, UrlHostnameWithQueryString) {
  EXPECT_EQ("example.com", UrlHostname("http://example.com/path?q=1&r=2"));
}

TEST(UrlUtilTest, UrlHostnameWithFragment) {
  EXPECT_EQ("example.com", UrlHostname("http://example.com/path#section"));
}

TEST(UrlUtilTest, UrlHostnameWithPortAndQuery) {
  EXPECT_EQ("example.com:8080",
            UrlHostname("http://example.com:8080/path?q=1"));
}

TEST(UrlUtilTest, UrlHostnameWithoutPath) {
  EXPECT_EQ("example.com", UrlHostname("http://example.com"));
}

TEST(UrlUtilTest, UrlHostnameJustScheme) {
  EXPECT_EQ("", UrlHostname("http://"));
}

TEST(UrlUtilTest, UrlHostnameIgnoresAtInPath) {
  // @ in the path component should not be treated as userinfo separator.
  EXPECT_EQ("example.com", UrlHostname("http://example.com/user@domain"));
  EXPECT_EQ("example.com",
            UrlHostname("http://example.com/path?email=user@host"));
}

// =============================================================================
// ResolvePath traversal protection tests
// =============================================================================

TEST(UrlUtilTest, ResolvePathNoTraversalAboveRoot) {
  // Excessive ../ should not produce a bare relative path (no "../../" prefix).
  // The result must remain anchored at "/" — it should never become a relative
  // path like "../../etc/passwd" that escapes the document root.
  std::string result = ResolvePath("/css/", "../../../../etc/passwd");
  // The path must start with "/" (absolute), never a bare ".." traversal.
  ASSERT_FALSE(result.empty());
  EXPECT_EQ(result[0], '/');
  // The result should not contain "../../" without a leading "/" segment,
  // i.e., it must not be a relative path that walks above root.
  EXPECT_NE(result.substr(0, 3), "../");
}

TEST(UrlUtilTest, ResolvePathNoTraversalAboveSchemeRoot) {
  // For full URLs, ../ should not traverse above the scheme+host root.
  std::string result =
      ResolvePath("http://example.com/css/", "../../etc/passwd");
  // Must still start with the scheme+host.
  EXPECT_TRUE(result.starts_with("http://example.com"));
  // The path after the host should not contain components that go above
  // the host root (no bare "../" before the host portion).
  EXPECT_EQ(result.find("http://example.com/"), 0u);
}

TEST(UrlUtilTest, ResolvePathExcessiveDotDotWithUrl) {
  // http://host/a/ + ../../x should stay at host root, not escape.
  std::string result = ResolvePath("http://host/a/", "../../x");
  EXPECT_TRUE(result.starts_with("http://host/"));
  // The result must not contain "/../" above the host root.
  auto host_end = result.find("http://host/");
  ASSERT_EQ(host_end, 0u);
  std::string_view after_host = std::string_view(result).substr(12);
  EXPECT_EQ(after_host.find(".."), std::string_view::npos)
      << "Path after host should have no residual '..' segments. Got: "
      << result;
}

TEST(UrlUtilTest, ResolvePathProtocolRelativeTraversal) {
  // Protocol-relative URL base: //host/a/ + ../x
  // Protocol-relative URLs lack "://" so root_pos stays at 0.
  // ../x should resolve relative to /a/, yielding //host/x.
  std::string result = ResolvePath("//host/a/", "../x");
  ASSERT_FALSE(result.empty());
  EXPECT_EQ(result[0], '/');
  // Should not produce a bare relative path.
  EXPECT_NE(result.substr(0, 3), "../");
}

TEST(UrlUtilTest, ResolvePathManyDotDotsAbsolutePath) {
  // Many ../ on an absolute path base should clamp at root.
  std::string result = ResolvePath("/a/b/c/d/", "../../../../../../z");
  EXPECT_EQ(result[0], '/');
  // Must stay absolute, never produce relative traversal.
  EXPECT_NE(result.substr(0, 3), "../");
  // Should end up at /z.
  EXPECT_EQ(result, "/z");
}

TEST(UrlUtilTest, ResolvePathUrlManyDotDotsAtHostRoot) {
  // Full URL with many ../ exceeding the path depth.
  std::string result =
      ResolvePath("http://example.com/a/b/", "../../../../../etc/passwd");
  EXPECT_TRUE(result.starts_with("http://example.com/"));
  // After the host, the path should not contain ".." segments.
  std::string_view after_host = std::string_view(result).substr(
      std::string_view("http://example.com/").size());
  EXPECT_EQ(after_host.find(".."), std::string_view::npos)
      << "No '..' should remain in the path. Got: " << result;
  // Should resolve to http://example.com/etc/passwd (clamped at host root).
  EXPECT_EQ(result, "http://example.com/etc/passwd");
}

TEST(UrlUtilTest, ResolvePathTrailingDotDotAbsolutePath) {
  // Trailing .. without a following filename, but with a trailing slash.
  // "../../..{slash}" exercises the /../ collapsing loop fully.
  std::string result = ResolvePath("/a/b/c/", "../../../");
  EXPECT_EQ(result, "/");
}

TEST(UrlUtilTest, ResolvePathTrailingDotDotExcessive) {
  // More trailing .. than path depth.
  std::string result = ResolvePath("/a/", "../../../..");
  EXPECT_EQ(result[0], '/');
  EXPECT_NE(result.substr(0, 3), "../");
}

}  // namespace
}  // namespace pagespeed
