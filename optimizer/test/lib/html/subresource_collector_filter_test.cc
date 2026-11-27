// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

// Unit tests for SubresourceCollectorFilter.

#include "lib/html/subresource_collector_filter.h"

#include <memory>
#include <string>
#include <string_view>

#include "gtest/gtest.h"
#include "lib/html/compat/message_handler.h"
#include "lib/html/html_keywords.h"
#include "lib/html/html_name.h"
#include "lib/html/html_parse.h"

namespace net_instaweb {

class SubresourceCollectorFilterTest : public testing::Test {
 protected:
  void SetUp() override { HtmlKeywords::Init(); }

  NginxScanResult Scan(std::string_view html) {
    NginxScanResult result;
    auto parser = std::make_unique<HtmlParse>(&message_handler_);
    auto filter = std::make_unique<SubresourceCollectorFilter>(&result);
    parser->AddFilter(filter.get());
    parser->StartParse("http://test.com/page.html");
    parser->ParseText(html);
    parser->FinishParse();
    return result;
  }

  NullMessageHandler message_handler_;
};

// ============================================================================
// Stylesheet collection
// ============================================================================

TEST_F(SubresourceCollectorFilterTest, CollectsStylesheet) {
  auto result = Scan(
      "<html><head>"
      "<link rel=\"stylesheet\" href=\"/style.css\">"
      "</head><body></body></html>");
  ASSERT_EQ(1u, result.subresources.size());
  EXPECT_EQ("/style.css", result.subresources[0].url);
  EXPECT_EQ(NginxScanResult::SubresourceInfo::kStylesheet,
            result.subresources[0].type);
}

TEST_F(SubresourceCollectorFilterTest, CollectsStylesheetWithMedia) {
  auto result =
      Scan(R"(<link rel="stylesheet" href="/print.css" media="print">)");
  ASSERT_EQ(1u, result.subresources.size());
  EXPECT_EQ("/print.css", result.subresources[0].url);
  EXPECT_EQ("print", result.subresources[0].media);
}

TEST_F(SubresourceCollectorFilterTest, CollectsStylesheetCrossorigin) {
  auto result = Scan(
      "<link rel=\"stylesheet\" href=\"/s.css\" "
      "crossorigin=\"anonymous\">");
  ASSERT_EQ(1u, result.subresources.size());
  EXPECT_EQ("anonymous", result.subresources[0].crossorigin);
}

TEST_F(SubresourceCollectorFilterTest, IgnoresLinkWithoutHref) {
  auto result = Scan("<link rel=\"stylesheet\">");
  EXPECT_EQ(0u, result.subresources.size());
}

TEST_F(SubresourceCollectorFilterTest, IgnoresNonStylesheetLink) {
  auto result = Scan(R"(<link rel="icon" href="/favicon.ico">)");
  EXPECT_EQ(0u, result.subresources.size());
}

// ============================================================================
// rel attribute parsing (HTML5 whitespace, case-insensitive)
// ============================================================================

TEST_F(SubresourceCollectorFilterTest, RelStylesheetPrefetch) {
  // "stylesheet" among multiple tokens.
  auto result = Scan(R"(<link rel="stylesheet prefetch" href="/s.css">)");
  ASSERT_EQ(1u, result.subresources.size());
  EXPECT_EQ("/s.css", result.subresources[0].url);
}

TEST_F(SubresourceCollectorFilterTest, RelCaseInsensitive) {
  auto result = Scan(R"(<link rel="Stylesheet" href="/s.css">)");
  ASSERT_EQ(1u, result.subresources.size());
}

TEST_F(SubresourceCollectorFilterTest, RelWithTabs) {
  // HTML5 space chars: tab, LF, FF, CR, space.
  auto result = Scan("<link rel=\"prefetch\tstylesheet\" href=\"/s.css\">");
  ASSERT_EQ(1u, result.subresources.size());
}

TEST_F(SubresourceCollectorFilterTest, RelWithNewline) {
  auto result = Scan("<link rel=\"prefetch\nstylesheet\" href=\"/s.css\">");
  ASSERT_EQ(1u, result.subresources.size());
}

TEST_F(SubresourceCollectorFilterTest, RelNoVerticalTab) {
  // \v (0x0B) is NOT HTML5 whitespace. "stylesheet\vextra" is a single
  // token that doesn't match "stylesheet".
  auto result = Scan("<link rel=\"stylesheet\x0bextra\" href=\"/s.css\">");
  EXPECT_EQ(0u, result.subresources.size());
}

// ============================================================================
// Script collection
// ============================================================================

TEST_F(SubresourceCollectorFilterTest, CollectsScript) {
  auto result = Scan("<script src=\"/app.js\"></script>");
  ASSERT_EQ(1u, result.subresources.size());
  EXPECT_EQ("/app.js", result.subresources[0].url);
  EXPECT_EQ(NginxScanResult::SubresourceInfo::kScript,
            result.subresources[0].type);
}

TEST_F(SubresourceCollectorFilterTest, IgnoresInlineScript) {
  auto result = Scan("<script>console.log('hello');</script>");
  EXPECT_EQ(0u, result.subresources.size());
}

TEST_F(SubresourceCollectorFilterTest, CollectsScriptCrossorigin) {
  auto result =
      Scan(R"(<script src="/app.js" crossorigin="anonymous"></script>)");
  ASSERT_EQ(1u, result.subresources.size());
  EXPECT_EQ("anonymous", result.subresources[0].crossorigin);
}

// ============================================================================
// Base href URL resolution
// ============================================================================

TEST_F(SubresourceCollectorFilterTest, BaseHrefResolvesRelative) {
  auto result = Scan(
      "<base href=\"http://cdn.example.com/assets/\">"
      "<link rel=\"stylesheet\" href=\"style.css\">");
  ASSERT_EQ(1u, result.subresources.size());
  EXPECT_EQ("http://cdn.example.com/assets/style.css",
            result.subresources[0].url);
  EXPECT_EQ("http://cdn.example.com/assets/", result.base_url);
}

TEST_F(SubresourceCollectorFilterTest, BaseHrefResolvesAbsolutePath) {
  auto result = Scan(
      "<base href=\"http://cdn.example.com/assets/\">"
      "<link rel=\"stylesheet\" href=\"/style.css\">");
  ASSERT_EQ(1u, result.subresources.size());
  EXPECT_EQ("http://cdn.example.com/style.css", result.subresources[0].url);
}

TEST_F(SubresourceCollectorFilterTest, AbsoluteUrlNotResolved) {
  auto result = Scan(
      "<base href=\"http://cdn.example.com/\">"
      "<link rel=\"stylesheet\" href=\"https://other.com/s.css\">");
  ASSERT_EQ(1u, result.subresources.size());
  EXPECT_EQ("https://other.com/s.css", result.subresources[0].url);
}

TEST_F(SubresourceCollectorFilterTest, ProtocolRelativeNotResolved) {
  auto result = Scan(
      "<base href=\"http://cdn.example.com/\">"
      "<link rel=\"stylesheet\" href=\"//other.com/s.css\">");
  ASSERT_EQ(1u, result.subresources.size());
  EXPECT_EQ("//other.com/s.css", result.subresources[0].url);
}

// ============================================================================
// <template> depth tracking
// ============================================================================

TEST_F(SubresourceCollectorFilterTest, SkipsInsideTemplate) {
  auto result = Scan(
      "<link rel=\"stylesheet\" href=\"/outside.css\">"
      "<template>"
      "<link rel=\"stylesheet\" href=\"/inside.css\">"
      "<script src=\"/inside.js\"></script>"
      "</template>"
      "<script src=\"/outside.js\"></script>");
  ASSERT_EQ(2u, result.subresources.size());
  EXPECT_EQ("/outside.css", result.subresources[0].url);
  EXPECT_EQ("/outside.js", result.subresources[1].url);
}

TEST_F(SubresourceCollectorFilterTest, NestedTemplates) {
  auto result = Scan(
      "<template>"
      "<template>"
      "<link rel=\"stylesheet\" href=\"/deep.css\">"
      "</template>"
      "<link rel=\"stylesheet\" href=\"/inner.css\">"
      "</template>"
      "<link rel=\"stylesheet\" href=\"/outer.css\">");
  ASSERT_EQ(1u, result.subresources.size());
  EXPECT_EQ("/outer.css", result.subresources[0].url);
}

// ============================================================================
// CSP detection
// ============================================================================

TEST_F(SubresourceCollectorFilterTest, CspNonceDetection) {
  auto result = Scan(
      "<meta http-equiv=\"Content-Security-Policy\" "
      "content=\"style-src 'nonce-abc123'\">");
  EXPECT_TRUE(result.has_nonce_csp);
  EXPECT_FALSE(result.has_hash_csp);
  EXPECT_FALSE(result.has_restrictive_style_csp);
}

TEST_F(SubresourceCollectorFilterTest, CspHashDetection) {
  auto result = Scan(
      "<meta http-equiv=\"Content-Security-Policy\" "
      "content=\"style-src 'sha256-abc123='\">");
  EXPECT_FALSE(result.has_nonce_csp);
  EXPECT_TRUE(result.has_hash_csp);
  EXPECT_FALSE(result.has_restrictive_style_csp);
}

TEST_F(SubresourceCollectorFilterTest, CspRestrictiveDetection) {
  auto result = Scan(
      "<meta http-equiv=\"Content-Security-Policy\" "
      "content=\"style-src https://cdn.example.com\">");
  EXPECT_FALSE(result.has_nonce_csp);
  EXPECT_FALSE(result.has_hash_csp);
  EXPECT_TRUE(result.has_restrictive_style_csp);
}

TEST_F(SubresourceCollectorFilterTest, CspUnsafeInlineNotRestrictive) {
  auto result = Scan(
      "<meta http-equiv=\"Content-Security-Policy\" "
      "content=\"style-src 'unsafe-inline' https://cdn.example.com\">");
  EXPECT_FALSE(result.has_restrictive_style_csp);
}

TEST_F(SubresourceCollectorFilterTest, CspFallbackChainStyleSrcElem) {
  // style-src-elem takes priority over style-src and default-src.
  auto result = Scan(
      "<meta http-equiv=\"Content-Security-Policy\" "
      "content=\"default-src 'unsafe-inline'; "
      "style-src 'unsafe-inline'; "
      "style-src-elem 'nonce-x'\">");
  EXPECT_TRUE(result.has_nonce_csp);
}

TEST_F(SubresourceCollectorFilterTest, CspFallbackChainStyleSrc) {
  // style-src takes priority over default-src.
  auto result = Scan(
      "<meta http-equiv=\"Content-Security-Policy\" "
      "content=\"default-src 'unsafe-inline'; "
      "style-src 'nonce-x'\">");
  EXPECT_TRUE(result.has_nonce_csp);
}

TEST_F(SubresourceCollectorFilterTest, CspFallbackChainDefaultSrc) {
  auto result = Scan(
      "<meta http-equiv=\"Content-Security-Policy\" "
      "content=\"default-src 'nonce-x'\">");
  EXPECT_TRUE(result.has_nonce_csp);
}

TEST_F(SubresourceCollectorFilterTest, CspIgnoresReportOnly) {
  // Content-Security-Policy-Report-Only should be ignored.
  auto result = Scan(
      "<meta http-equiv=\"Content-Security-Policy-Report-Only\" "
      "content=\"style-src 'nonce-x'\">");
  EXPECT_FALSE(result.has_nonce_csp);
}

TEST_F(SubresourceCollectorFilterTest, CspNoCspHeaders) {
  auto result = Scan("<html><head></head><body></body></html>");
  EXPECT_FALSE(result.has_nonce_csp);
  EXPECT_FALSE(result.has_hash_csp);
  EXPECT_FALSE(result.has_restrictive_style_csp);
}

TEST_F(SubresourceCollectorFilterTest, CspSha384Detection) {
  auto result = Scan(
      "<meta http-equiv=\"Content-Security-Policy\" "
      "content=\"style-src 'sha384-abc='\">");
  EXPECT_TRUE(result.has_hash_csp);
}

TEST_F(SubresourceCollectorFilterTest, CspSha512Detection) {
  auto result = Scan(
      "<meta http-equiv=\"Content-Security-Policy\" "
      "content=\"style-src 'sha512-abc='\">");
  EXPECT_TRUE(result.has_hash_csp);
}

// ============================================================================
// Subresource limit
// ============================================================================

TEST_F(SubresourceCollectorFilterTest, SubresourceLimit) {
  std::string html;
  for (int i = 0; i < 600; ++i) {
    html +=
        R"(<link rel="stylesheet" href="/s)" + std::to_string(i) + ".css\">";
  }
  auto result = Scan(html);
  EXPECT_EQ(500u, result.subresources.size());
}

// ============================================================================
// Mixed subresources
// ============================================================================

TEST_F(SubresourceCollectorFilterTest, MixedSubresources) {
  auto result = Scan(
      "<html><head>"
      "<link rel=\"stylesheet\" href=\"/a.css\">"
      "<link rel=\"stylesheet\" href=\"/b.css\" media=\"screen\">"
      "<script src=\"/app.js\"></script>"
      "<script src=\"/vendor.js\"></script>"
      "</head><body>"
      "<link rel=\"stylesheet\" href=\"/c.css\">"
      "</body></html>");
  ASSERT_EQ(5u, result.subresources.size());
  EXPECT_EQ(NginxScanResult::SubresourceInfo::kStylesheet,
            result.subresources[0].type);
  EXPECT_EQ("screen", result.subresources[1].media);
  EXPECT_EQ(NginxScanResult::SubresourceInfo::kScript,
            result.subresources[2].type);
  EXPECT_EQ("/vendor.js", result.subresources[3].url);
  EXPECT_EQ("/c.css", result.subresources[4].url);
}

// ============================================================================
// Keyword additions (Step 6b)
// ============================================================================

TEST_F(SubresourceCollectorFilterTest, KeywordLookupCrossorigin) {
  EXPECT_EQ(HtmlName::kCrossorigin, HtmlName::Lookup("crossorigin"));
}

TEST_F(SubresourceCollectorFilterTest, KeywordLookupIntegrity) {
  EXPECT_EQ(HtmlName::kIntegrity, HtmlName::Lookup("integrity"));
}

TEST_F(SubresourceCollectorFilterTest, KeywordLookupTemplate) {
  EXPECT_EQ(HtmlName::kTemplate, HtmlName::Lookup("template"));
}

TEST_F(SubresourceCollectorFilterTest, KeywordLookupCaseInsensitive) {
  EXPECT_EQ(HtmlName::kCrossorigin, HtmlName::Lookup("CROSSORIGIN"));
  EXPECT_EQ(HtmlName::kIntegrity, HtmlName::Lookup("Integrity"));
  EXPECT_EQ(HtmlName::kTemplate, HtmlName::Lookup("TEMPLATE"));
}

// Covers subresource_collector_filter.cc line 216 (CSP meta with no
// script-src, style-src, or default-src directive → early return).
TEST_F(SubresourceCollectorFilterTest, CspWithoutRelevantDirective) {
  auto result = Scan(
      "<html><head>"
      "<meta http-equiv=\"Content-Security-Policy\" content=\"img-src *\">"
      "<link rel=\"stylesheet\" href=\"/style.css\">"
      "</head></html>");
  // No CSP restriction on stylesheets — link still collected
  ASSERT_EQ(1u, result.subresources.size());
  EXPECT_FALSE(result.has_restrictive_style_csp);
}

// Covers subresource_collector_filter.cc lines 290, 292 (URL resolution:
// base has scheme://host but no path slash after host → returns URL as-is).
TEST_F(SubresourceCollectorFilterTest, UrlResolveBaseWithoutPath) {
  auto result = Scan(
      "<base href=\"http://example.com\">"
      "<link rel=\"stylesheet\" href=\"/s.css\">");
  ASSERT_EQ(1u, result.subresources.size());
  // base="http://example.com", href="/s.css": base has "://" but no '/' after
  // host → host_end == npos → falls through to line 292: return url as-is.
  EXPECT_EQ("/s.css", result.subresources[0].url);
}

// Covers subresource_collector_filter.cc line 301 (URL resolution:
// base URL has no slash at all → return URL as-is).
TEST_F(SubresourceCollectorFilterTest, UrlResolveBaseWithNoSlash) {
  auto result = Scan(
      "<base href=\"noslash\">"
      "<link rel=\"stylesheet\" href=\"relative.css\">");
  ASSERT_EQ(1u, result.subresources.size());
  // base="noslash" (no slashes), href="relative.css" (doesn't start with /):
  // rfind('/') returns npos → falls through to line 301: return url as-is.
  EXPECT_EQ("relative.css", result.subresources[0].url);
}

// Covers subresource_collector_filter.cc line 290 (no-scheme path: base
// doesn't contain "://" but URL starts with "/").
TEST_F(SubresourceCollectorFilterTest, UrlResolveBaseNoScheme) {
  auto result = Scan(
      "<base href=\"example.com\">"
      "<link rel=\"stylesheet\" href=\"/style.css\">");
  ASSERT_EQ(1u, result.subresources.size());
  // base="example.com" (no "://"), href="/style.css":
  // scheme_end == npos → skips host extraction → line 292: return url as-is.
  EXPECT_EQ("/style.css", result.subresources[0].url);
}

// Covers subresource_collector_filter.cc lines 100-101 (limit_reached_ set
// via the kScript path when the 500th subresource is a script element).
TEST_F(SubresourceCollectorFilterTest, SubresourceLimitViaScript) {
  std::string html;
  // 499 stylesheets fill via the kLink path
  for (int i = 0; i < 499; ++i) {
    html +=
        R"(<link rel="stylesheet" href="/s)" + std::to_string(i) + ".css\">";
  }
  // Next two are scripts — the 500th subresource triggers limit via kScript path
  html += "<script src=\"/a.js\"></script>";
  html += "<script src=\"/b.js\"></script>";
  auto result = Scan(html);
  EXPECT_EQ(500u, result.subresources.size());
  EXPECT_EQ(NginxScanResult::SubresourceInfo::kScript,
            result.subresources[499].type);
}

}  // namespace net_instaweb
