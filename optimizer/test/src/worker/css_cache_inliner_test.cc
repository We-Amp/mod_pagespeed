// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

// Tests for the CSS Cache Inliner.

#include "src/worker/css_cache_inliner.h"

#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "gtest/gtest.h"

namespace pagespeed {
namespace {

// Lookup that returns preset CSS for specific URLs.
css::CssLookupFn MakeLookup(
    std::initializer_list<std::pair<const std::string, std::string>> entries) {
  auto map =
      std::make_shared<std::unordered_map<std::string, std::string>>(entries);
  return [map](std::string_view url) -> std::optional<std::string> {
    auto it = map->find(std::string(url));
    if (it != map->end()) return it->second;
    return std::nullopt;
  };
}

// Lookup that never finds anything.
css::CssLookupFn EmptyLookup() {
  return [](std::string_view) -> std::optional<std::string> {
    return std::nullopt;
  };
}

// Count non-overlapping occurrences of `sub` in `s`.
size_t CountOccurrences(const std::string& s, const std::string& sub) {
  size_t n = 0;
  for (size_t p = s.find(sub); p != std::string::npos;
       p = s.find(sub, p + sub.size())) {
    ++n;
  }
  return n;
}

// 1. AllCssCached - 2 links, both in lookup -> 2 <style> blocks.
TEST(InlineCachedStylesheets, AllCssCached) {
  std::string html =
      "<html><head>"
      "<link rel=\"stylesheet\" href=\"/a.css\">"
      "<link rel=\"stylesheet\" href=\"/b.css\">"
      "</head><body>Hello</body></html>";

  auto lookup = MakeLookup(
      {{"/a.css", "body { margin: 0; }"}, {"/b.css", "h1 { color: red; }"}});

  CssInliningStats stats;
  std::string result =
      InlineCachedStylesheets(html, "http://example.com/", lookup, &stats);

  // Both stylesheets should be inlined.
  EXPECT_NE(std::string::npos, result.find("body { margin: 0; }"));
  EXPECT_NE(std::string::npos, result.find("h1 { color: red; }"));

  // Both should use <style data-pagespeed-inlined>.
  size_t first = result.find("<style data-pagespeed-inlined>");
  EXPECT_NE(std::string::npos, first);
  size_t second = result.find("<style data-pagespeed-inlined>", first + 1);
  EXPECT_NE(std::string::npos, second);

  // Inlined styles should appear before </head>.
  size_t head_close = result.find("</head>");
  EXPECT_NE(std::string::npos, head_close);
  EXPECT_LT(second, head_close);

  // The external <link> tags are REMOVED for the sheets we inlined, so the
  // browser loads each sheet exactly once (external + inlined <style> would
  // otherwise double-load — the prod critical-CSS double-ship). The inlined
  // <style> copies above carry the bytes.
  EXPECT_EQ(std::string::npos, result.find("href=\"/a.css\""));
  EXPECT_EQ(std::string::npos, result.find("href=\"/b.css\""));

  EXPECT_EQ(2u, stats.stylesheets_found);
  EXPECT_EQ(2u, stats.stylesheets_cached);
}

// The link remover's predicate claims, in its own comment, to mirror
// HtmlScanner's collection gate EXACTLY. The scanner also collects an
// async-deferred primary — rel="preload" as="style" + the marker — so this must
// too, or a run over reprocessed markup would inline the sheet AND leave its
// <link> behind: the double-ship the remover exists to prevent. The two gates
// silently diverging is precisely the failure that comment rules out.
TEST(InlineCachedStylesheets, RemovesTheAsyncDeferredPreloadPrimary) {
  std::string html =
      "<html><head>"
      "<link rel=\"preload\" as=\"style\" href=\"/deferred.css\" "
      "data-pagespeed-media=\"screen\" data-pagespeed-async=\"\">"
      "</head><body>Hello</body></html>";

  auto lookup = MakeLookup({{"/deferred.css", "body { margin: 0; }"}});

  CssInliningStats stats;
  std::string result =
      InlineCachedStylesheets(html, "http://example.com/", lookup, &stats);

  EXPECT_NE(std::string::npos, result.find("body { margin: 0; }"))
      << "the deferred primary was not recognised as a stylesheet: " << result;
  EXPECT_EQ(std::string::npos, result.find("href=\"/deferred.css\""))
      << "inlined the sheet but left its <link> — the sheet now loads twice: "
      << result;
  EXPECT_EQ(1u, stats.stylesheets_found);
  EXPECT_EQ(1u, stats.stylesheets_cached);
}

// The widened arm is keyed on the marker, not on rel="preload": an author's
// own preload is a hint about a sheet declared elsewhere, and removing it would
// delete markup we never inlined.
TEST(InlineCachedStylesheets, LeavesUnmarkedPreloadsAlone) {
  std::string html =
      "<html><head>"
      "<link rel=\"preload\" as=\"style\" href=\"/theirs.css\">"
      "<link rel=\"preload\" as=\"font\" href=\"/f.woff2\" crossorigin>"
      "</head><body>Hello</body></html>";

  auto lookup = MakeLookup({{"/theirs.css", "body { margin: 0; }"}});

  CssInliningStats stats;
  std::string result =
      InlineCachedStylesheets(html, "http://example.com/", lookup, &stats);

  EXPECT_NE(std::string::npos, result.find("href=\"/theirs.css\"")) << result;
  EXPECT_NE(std::string::npos, result.find("href=\"/f.woff2\"")) << result;
  EXPECT_EQ(std::string::npos, result.find("<style data-pagespeed-inlined>"))
      << result;
  EXPECT_EQ(0u, stats.stylesheets_found);
}

// 2. PartialCache - 2 links, only 1 in lookup.
TEST(InlineCachedStylesheets, PartialCache) {
  std::string html =
      "<html><head>"
      "<link rel=\"stylesheet\" href=\"/a.css\">"
      "<link rel=\"stylesheet\" href=\"/b.css\">"
      "</head><body></body></html>";

  auto lookup = MakeLookup({{"/a.css", "body { margin: 0; }"}});

  CssInliningStats stats;
  std::string result =
      InlineCachedStylesheets(html, "http://example.com/", lookup, &stats);

  // Only /a.css should be inlined.
  EXPECT_NE(std::string::npos, result.find("body { margin: 0; }"));
  EXPECT_NE(std::string::npos, result.find("<style data-pagespeed-inlined>"));

  // Only one <style> block.
  size_t first = result.find("<style data-pagespeed-inlined>");
  size_t second = result.find("<style data-pagespeed-inlined>", first + 1);
  EXPECT_EQ(std::string::npos, second);

  // The inlined link is removed; the un-cached link is left intact so the
  // browser still loads it.
  EXPECT_EQ(std::string::npos, result.find("href=\"/a.css\""));
  EXPECT_NE(std::string::npos, result.find("href=\"/b.css\""));

  EXPECT_EQ(2u, stats.stylesheets_found);
  EXPECT_EQ(1u, stats.stylesheets_cached);
}

// 3. NoCssCached - lookup always returns nullopt.
TEST(InlineCachedStylesheets, NoCssCached) {
  std::string html =
      "<html><head>"
      "<link rel=\"stylesheet\" href=\"/a.css\">"
      "<link rel=\"stylesheet\" href=\"/b.css\">"
      "</head><body></body></html>";

  CssInliningStats stats;
  std::string result = InlineCachedStylesheets(html, "http://example.com/",
                                               EmptyLookup(), &stats);

  EXPECT_EQ(html, result);
  EXPECT_EQ(2u, stats.stylesheets_found);
  EXPECT_EQ(0u, stats.stylesheets_cached);
}

// 4. NoStylesheets - no <link> tags.
TEST(InlineCachedStylesheets, NoStylesheets) {
  std::string html =
      "<html><head><title>Test</title></head><body>Hello</body></html>";

  CssInliningStats stats;
  std::string result = InlineCachedStylesheets(html, "http://example.com/",
                                               EmptyLookup(), &stats);

  EXPECT_EQ(html, result);
  EXPECT_EQ(0u, stats.stylesheets_found);
}

// 5. MediaAttribute - media="print" wraps CSS in @media print { }.
TEST(InlineCachedStylesheets, MediaAttribute) {
  std::string html =
      "<html><head>"
      "<link rel=\"stylesheet\" href=\"/print.css\" media=\"print\">"
      "</head><body></body></html>";

  auto lookup = MakeLookup({{"/print.css", "body { font-size: 12pt; }"}});

  std::string result =
      InlineCachedStylesheets(html, "http://example.com/", lookup);

  EXPECT_NE(std::string::npos,
            result.find("@media print { body { font-size: 12pt; } }"));
}

// 6. MediaAll - media="all" does NOT wrap.
TEST(InlineCachedStylesheets, MediaAll) {
  std::string html =
      "<html><head>"
      "<link rel=\"stylesheet\" href=\"/all.css\" media=\"all\">"
      "</head><body></body></html>";

  auto lookup = MakeLookup({{"/all.css", "body { margin: 0; }"}});

  std::string result =
      InlineCachedStylesheets(html, "http://example.com/", lookup);

  // CSS should be inlined without @media wrapper.
  EXPECT_NE(std::string::npos, result.find("body { margin: 0; }"));
  EXPECT_EQ(std::string::npos, result.find("@media all"));
}

// 7. ImportFlattening - CSS with @import, both available.
TEST(InlineCachedStylesheets, ImportFlattening) {
  std::string html =
      "<html><head>"
      "<link rel=\"stylesheet\" href=\"/main.css\">"
      "</head><body></body></html>";

  auto lookup = MakeLookup(
      {{"/main.css", "@import url(\"/imported.css\");\nbody { margin: 0; }"},
       {"/imported.css", "h1 { color: blue; }"}});

  std::string result =
      InlineCachedStylesheets(html, "http://example.com/", lookup);

  // Both the main CSS and the imported content should appear.
  EXPECT_NE(std::string::npos, result.find("h1 { color: blue; }"));
  EXPECT_NE(std::string::npos, result.find("body { margin: 0; }"));
}

// 8. InlineCssPreserved - existing <style> blocks unchanged.
TEST(InlineCachedStylesheets, InlineCssPreserved) {
  std::string html =
      "<html><head>"
      "<style>body { color: red; }</style>"
      "<link rel=\"stylesheet\" href=\"/a.css\">"
      "</head><body></body></html>";

  auto lookup = MakeLookup({{"/a.css", "h1 { font-size: 24px; }"}});

  std::string result =
      InlineCachedStylesheets(html, "http://example.com/", lookup);

  // Original inline style should still be present.
  EXPECT_NE(std::string::npos,
            result.find("<style>body { color: red; }</style>"));
  // Inlined stylesheet should also be present.
  EXPECT_NE(std::string::npos, result.find("h1 { font-size: 24px; }"));
}

// 9. CrossOriginHref - full URL passed to lookup.
TEST(InlineCachedStylesheets, CrossOriginHref) {
  std::string html =
      "<html><head>"
      "<link rel=\"stylesheet\" "
      "href=\"https://cdn.example.com/style.css\">"
      "</head><body></body></html>";

  std::string looked_up_url;
  css::CssLookupFn lookup =
      [&](std::string_view url) -> std::optional<std::string> {
    looked_up_url = std::string(url);
    return "h1 { color: green; }";
  };

  std::string result =
      InlineCachedStylesheets(html, "http://example.com/", lookup);

  EXPECT_EQ("https://cdn.example.com/style.css", looked_up_url);
  EXPECT_NE(std::string::npos, result.find("h1 { color: green; }"));
}

// 10. StyleCloseXss - CSS containing </style> is skipped.
TEST(InlineCachedStylesheets, StyleCloseXss) {
  std::string html =
      "<html><head>"
      "<link rel=\"stylesheet\" href=\"/evil.css\">"
      "</head><body></body></html>";

  auto lookup =
      MakeLookup({{"/evil.css", "body{}</style><script>alert(1)</script>"}});

  std::string result =
      InlineCachedStylesheets(html, "http://example.com/", lookup);

  // The malicious CSS must NOT appear in the output.
  EXPECT_EQ(std::string::npos, result.find("<script>alert(1)</script>"));
  EXPECT_EQ(std::string::npos, result.find("data-pagespeed-inlined"));
}

// 11. StyleCloseCaseVariant - </STYLE> is also caught.
TEST(InlineCachedStylesheets, StyleCloseCaseVariant) {
  std::string html =
      "<html><head>"
      "<link rel=\"stylesheet\" href=\"/evil.css\">"
      "</head><body></body></html>";

  auto lookup =
      MakeLookup({{"/evil.css", "body{}</STYLE><script>alert(1)</script>"}});

  std::string result =
      InlineCachedStylesheets(html, "http://example.com/", lookup);

  EXPECT_EQ(std::string::npos, result.find("<script>alert(1)</script>"));
  EXPECT_EQ(std::string::npos, result.find("data-pagespeed-inlined"));
}

// 12. NullBytesStripped - null bytes removed, clean CSS inlined.
TEST(InlineCachedStylesheets, NullBytesStripped) {
  std::string html =
      "<html><head>"
      "<link rel=\"stylesheet\" href=\"/nulls.css\">"
      "</head><body></body></html>";

  std::string css_with_nulls = "body { margin: 0; }";
  css_with_nulls.push_back('\0');
  css_with_nulls.append("extra");

  auto lookup = MakeLookup({{"/nulls.css", css_with_nulls}});

  std::string result =
      InlineCachedStylesheets(html, "http://example.com/", lookup);

  // Null byte stripped, content inlined.
  EXPECT_NE(std::string::npos, result.find("body { margin: 0; }extra"));
  EXPECT_NE(std::string::npos, result.find("data-pagespeed-inlined"));
}

// 13. CleanCssPasses - normal CSS is inlined successfully.
TEST(InlineCachedStylesheets, CleanCssPasses) {
  std::string html =
      "<html><head>"
      "<link rel=\"stylesheet\" href=\"/clean.css\">"
      "</head><body></body></html>";

  auto lookup = MakeLookup({{"/clean.css", "body { margin: 0; }"}});

  CssInliningStats stats;
  std::string result =
      InlineCachedStylesheets(html, "http://example.com/", lookup, &stats);

  EXPECT_NE(std::string::npos, result.find("<style data-pagespeed-inlined>"));
  EXPECT_NE(std::string::npos, result.find("body { margin: 0; }"));
  EXPECT_EQ(1u, stats.stylesheets_cached);
  EXPECT_GT(stats.bytes_inlined, 0u);
}

// 14. DataUriSkipped - data: URIs are not looked up.
TEST(InlineCachedStylesheets, DataUriSkipped) {
  std::string html =
      "<html><head>"
      "<link rel=\"stylesheet\" "
      "href=\"data:text/css,body{margin:0}\">"
      "</head><body></body></html>";

  bool lookup_called = false;
  css::CssLookupFn lookup =
      [&](std::string_view) -> std::optional<std::string> {
    lookup_called = true;
    return std::nullopt;
  };

  std::string result =
      InlineCachedStylesheets(html, "http://example.com/", lookup);

  EXPECT_FALSE(lookup_called);
  EXPECT_EQ(std::string::npos, result.find("data-pagespeed-inlined"));
}

// 15. EmptyCssSkipped - empty cache entry produces no <style> block.
TEST(InlineCachedStylesheets, EmptyCssSkipped) {
  std::string html =
      "<html><head>"
      "<link rel=\"stylesheet\" href=\"/empty.css\">"
      "</head><body></body></html>";

  auto lookup = MakeLookup({{"/empty.css", ""}});

  std::string result =
      InlineCachedStylesheets(html, "http://example.com/", lookup);

  EXPECT_EQ(std::string::npos, result.find("data-pagespeed-inlined"));
}

// 16. SizeCapFallback - enriched HTML > 10MB falls back to original.
TEST(InlineCachedStylesheets, SizeCapFallback) {
  std::string html =
      "<html><head>"
      "<link rel=\"stylesheet\" href=\"/huge.css\">"
      "</head><body></body></html>";

  // Create CSS just large enough to exceed 10MB when combined with HTML.
  std::string huge_css(10UL * 1024 * 1024, 'x');
  auto lookup = MakeLookup({{"/huge.css", huge_css}});

  std::string result =
      InlineCachedStylesheets(html, "http://example.com/", lookup);

  // Output should equal the original HTML — size cap triggered.
  EXPECT_EQ(html, result);
}

// 17. StylesheetCountCap - only first 50 links are processed.
TEST(InlineCachedStylesheets, StylesheetCountCap) {
  std::string html = "<html><head>";
  std::unordered_map<std::string, std::string> css_map;
  for (int i = 0; i < 55; ++i) {
    std::string href = "/style" + std::to_string(i) + ".css";
    html += "<link rel=\"stylesheet\" href=\"" + href + "\">";
    css_map[href] = "div { color: red; }";
  }
  html += "</head><body></body></html>";

  auto map = std::make_shared<std::unordered_map<std::string, std::string>>(
      std::move(css_map));
  css::CssLookupFn lookup =
      [map](std::string_view url) -> std::optional<std::string> {
    auto it = map->find(std::string(url));
    if (it != map->end()) return it->second;
    return std::nullopt;
  };

  CssInliningStats stats;
  std::string result =
      InlineCachedStylesheets(html, "http://example.com/", lookup, &stats);

  // All 55 should be found, but only 50 processed due to cap.
  EXPECT_EQ(55u, stats.stylesheets_found);
  EXPECT_EQ(50u, stats.stylesheets_cached);
  EXPECT_NE(std::string::npos, result.find("data-pagespeed-inlined"));
  // The first 50 links are inlined and removed; over-cap links are kept.
  EXPECT_EQ(std::string::npos, result.find("/style0.css"));
  EXPECT_NE(std::string::npos, result.find("/style54.css"));
}

// 18. RelativeUrlResolution - relative href resolved against page URL.
TEST(InlineCachedStylesheets, RelativeUrlResolution) {
  std::string html =
      "<html><head>"
      "<link rel=\"stylesheet\" href=\"css/main.css\">"
      "</head><body></body></html>";

  std::string looked_up_url;
  css::CssLookupFn lookup =
      [&](std::string_view url) -> std::optional<std::string> {
    looked_up_url = std::string(url);
    return "h1 { color: blue; }";
  };

  InlineCachedStylesheets(html, "http://example.com/about/", lookup);

  // Relative URL should be resolved against the page directory.
  // Accept either full or path-only resolution.
  bool resolved_correctly =
      looked_up_url == "http://example.com/about/css/main.css" ||
      looked_up_url == "/about/css/main.css";
  EXPECT_TRUE(resolved_correctly)
      << "Expected resolved URL, got: " << looked_up_url;
}

// 19. QueryStringPreserved - query string kept in lookup URL.
TEST(InlineCachedStylesheets, QueryStringPreserved) {
  std::string html =
      "<html><head>"
      "<link rel=\"stylesheet\" href=\"/style.css?v=123\">"
      "</head><body></body></html>";

  std::string looked_up_url;
  css::CssLookupFn lookup =
      [&](std::string_view url) -> std::optional<std::string> {
    looked_up_url = std::string(url);
    return "body { margin: 0; }";
  };

  InlineCachedStylesheets(html, "http://example.com/", lookup);

  EXPECT_NE(std::string::npos, looked_up_url.find("?v=123"))
      << "Query string should be preserved, got: " << looked_up_url;
}

// 20. NullByteXssBypass - null byte inside </style to bypass detection.
TEST(InlineCachedStylesheets, NullByteXssBypass) {
  std::string html =
      "<html><head>"
      "<link rel=\"stylesheet\" href=\"/evil.css\">"
      "</head><body></body></html>";

  // Attacker tries to split </style with a null byte.
  std::string evil_css = "body{}</sty";
  evil_css.push_back('\0');
  evil_css.append("le><script>alert(1)</script>");

  auto lookup = MakeLookup({{"/evil.css", evil_css}});

  std::string result =
      InlineCachedStylesheets(html, "http://example.com/", lookup);

  // Null byte stripped -> becomes </style> -> rejected.
  EXPECT_EQ(std::string::npos, result.find("<script>alert(1)</script>"));
  EXPECT_EQ(std::string::npos, result.find("data-pagespeed-inlined"));
}

// 21. PerSheetSizeCap - single stylesheet > 2MB is skipped, others inlined.
TEST(InlineCachedStylesheets, PerSheetSizeCap) {
  std::string html =
      "<html><head>"
      "<link rel=\"stylesheet\" href=\"/huge.css\">"
      "<link rel=\"stylesheet\" href=\"/small.css\">"
      "</head><body></body></html>";

  // Create CSS that exceeds the 2MB per-sheet cap.
  std::string huge_css(2UL * 1024 * 1024 + 1, 'x');
  auto lookup = MakeLookup(
      {{"/huge.css", huge_css}, {"/small.css", "body { margin: 0; }"}});

  CssInliningStats stats;
  std::string result =
      InlineCachedStylesheets(html, "http://example.com/", lookup, &stats);

  // The small stylesheet should be inlined, the huge one skipped.
  EXPECT_NE(std::string::npos, result.find("body { margin: 0; }"));
  EXPECT_NE(std::string::npos, result.find("data-pagespeed-inlined"));
  EXPECT_EQ(2u, stats.stylesheets_found);
  // Only the small one should be cached.
  EXPECT_EQ(1u, stats.stylesheets_cached);
}

// 22. PerSheetSizeCapExactBoundary - stylesheet at exactly 2MB is inlined.
TEST(InlineCachedStylesheets, PerSheetSizeCapExactBoundary) {
  std::string html =
      "<html><head>"
      "<link rel=\"stylesheet\" href=\"/exact.css\">"
      "</head><body></body></html>";

  // Create CSS at exactly 2MB (should pass the check: !(size > 2MB)).
  std::string exact_css(2UL * 1024 * 1024, 'y');
  auto lookup = MakeLookup({{"/exact.css", exact_css}});

  CssInliningStats stats;
  std::string result =
      InlineCachedStylesheets(html, "http://example.com/", lookup, &stats);

  // At exactly 2MB, the CSS should be inlined (not exceeding the cap).
  EXPECT_EQ(1u, stats.stylesheets_found);
  EXPECT_EQ(1u, stats.stylesheets_cached);
  EXPECT_NE(std::string::npos, result.find("data-pagespeed-inlined"));
}

// 23. MixedMediaAttributes - multiple links with different media attributes.
TEST(InlineCachedStylesheets, MixedMediaAttributes) {
  std::string html =
      "<html><head>"
      "<link rel=\"stylesheet\" href=\"/main.css\">"
      "<link rel=\"stylesheet\" href=\"/print.css\" media=\"print\">"
      "<link rel=\"stylesheet\" href=\"/screen.css\" media=\"screen\">"
      "</head><body></body></html>";

  auto lookup = MakeLookup({{"/main.css", "body { margin: 0; }"},
                            {"/print.css", "body { font-size: 12pt; }"},
                            {"/screen.css", "body { font-size: 16px; }"}});

  CssInliningStats stats;
  std::string result =
      InlineCachedStylesheets(html, "http://example.com/", lookup, &stats);

  // main.css: no media wrapper.
  EXPECT_NE(std::string::npos, result.find("body { margin: 0; }"));
  // print.css: wrapped in @media print.
  EXPECT_NE(std::string::npos,
            result.find("@media print { body { font-size: 12pt; } }"));
  // screen.css: wrapped in @media screen.
  EXPECT_NE(std::string::npos,
            result.find("@media screen { body { font-size: 16px; } }"));
  EXPECT_EQ(3u, stats.stylesheets_found);
  EXPECT_EQ(3u, stats.stylesheets_cached);
}

// 24. NullStatsPointer - passing nullptr for stats does not crash.
TEST(InlineCachedStylesheets, NullStatsPointer) {
  std::string html =
      "<html><head>"
      "<link rel=\"stylesheet\" href=\"/a.css\">"
      "</head><body></body></html>";

  auto lookup = MakeLookup({{"/a.css", "body { margin: 0; }"}});

  // Pass nullptr for stats -- should not crash.
  std::string result =
      InlineCachedStylesheets(html, "http://example.com/", lookup, nullptr);

  EXPECT_NE(std::string::npos, result.find("body { margin: 0; }"));
}

// 25. NoHeadCloseFallback - styles appended at end when no </head>.
TEST(InlineCachedStylesheets, NoHeadCloseFallback) {
  // HtmlScanner needs <link> to find stylesheets. No </head> tag.
  std::string html =
      "<html>"
      "<link rel=\"stylesheet\" href=\"/a.css\">"
      "<body>Hello</body></html>";

  auto lookup = MakeLookup({{"/a.css", "body { margin: 0; }"}});

  std::string result =
      InlineCachedStylesheets(html, "http://example.com/", lookup);

  // Styles should appear somewhere in the output.
  EXPECT_NE(std::string::npos, result.find("data-pagespeed-inlined"));
  EXPECT_NE(std::string::npos, result.find("body { margin: 0; }"));
}

// Link removal must mirror the parser's notion of inert content. <iframe> and
// <xmp> bodies are literal text (the scanner never collects a <link> inside
// them), so look-alike links there — even with an href matching an inlined
// sheet — must survive.
TEST(InlineCachedStylesheets, KeepsLinkInsideLiteralIframeXmp) {
  std::string html =
      "<html><head>"
      "<link rel=\"stylesheet\" href=\"/a.css\">"
      "</head><body>"
      "<iframe><link rel=\"stylesheet\" href=\"/a.css\"></iframe>"
      "<xmp><link rel=\"stylesheet\" href=\"/a.css\"></xmp>"
      "</body></html>";

  auto lookup = MakeLookup({{"/a.css", "body { margin: 0; }"}});

  CssInliningStats stats;
  std::string result =
      InlineCachedStylesheets(html, "http://example.com/", lookup, &stats);

  EXPECT_EQ(1u, stats.stylesheets_found);
  EXPECT_EQ(1u, stats.stylesheets_cached);
  // The head link is removed; the two inert literal copies survive.
  EXPECT_EQ(2u, CountOccurrences(result, "href=\"/a.css\""));
}

// A </scriptx> is NOT a valid </script> close (the lexer requires a boundary
// char after the name), so the script body stays inert; the in-script <link>
// literal must survive.
TEST(InlineCachedStylesheets, KeepsScriptBodyLinkWithLookalikeClose) {
  std::string html =
      "<html><head>"
      "<link rel=\"stylesheet\" href=\"/a.css\">"
      "<script>var x=1;</scriptx>"
      "<link rel=\"stylesheet\" href=\"/a.css\"></script>"
      "</head><body></body></html>";

  auto lookup = MakeLookup({{"/a.css", "body { margin: 0; }"}});

  CssInliningStats stats;
  std::string result =
      InlineCachedStylesheets(html, "http://example.com/", lookup, &stats);

  EXPECT_EQ(1u, stats.stylesheets_found);
  EXPECT_EQ(1u, stats.stylesheets_cached);
  // Head link removed; the inert script-body link literal survives.
  EXPECT_EQ(1u, CountOccurrences(result, "href=\"/a.css\""));
}

// The scanner's collection gate is case-SENSITIVE (rel == "stylesheet"); a
// rel="STYLESHEET" link is never inlined, so removal must NOT strip it.
TEST(InlineCachedStylesheets, KeepsCaseVariantRelStylesheetLink) {
  std::string html =
      "<html><head>"
      "<link rel=\"stylesheet\" href=\"/a.css\">"
      "<link rel=\"STYLESHEET\" href=\"/a.css\" id=\"upper\">"
      "</head><body></body></html>";

  auto lookup = MakeLookup({{"/a.css", "body { margin: 0; }"}});

  CssInliningStats stats;
  std::string result =
      InlineCachedStylesheets(html, "http://example.com/", lookup, &stats);

  EXPECT_EQ(1u, stats.stylesheets_found);
  EXPECT_EQ(1u, stats.stylesheets_cached);
  // The uppercase-rel link was never inlined, so it must remain.
  EXPECT_NE(std::string::npos, result.find("id=\"upper\""));
}

// The scanner deliberately skips data-pagespeed-async-fallback links (its own
// artifact), so they are never inlined and removal must leave them alone even
// when the href matches.
TEST(InlineCachedStylesheets, KeepsAsyncFallbackLink) {
  std::string html =
      "<html><head>"
      "<link rel=\"stylesheet\" href=\"/a.css\">"
      "<link rel=\"stylesheet\" href=\"/a.css\" data-pagespeed-async-fallback>"
      "</head><body></body></html>";

  auto lookup = MakeLookup({{"/a.css", "body { margin: 0; }"}});

  CssInliningStats stats;
  std::string result =
      InlineCachedStylesheets(html, "http://example.com/", lookup, &stats);

  EXPECT_EQ(1u, stats.stylesheets_found);
  EXPECT_EQ(1u, stats.stylesheets_cached);
  // The async-fallback artifact survives.
  EXPECT_NE(std::string::npos, result.find("data-pagespeed-async-fallback"));
}

// Closes the media-scoped variant of the double-ship: a media-scoped sheet's
// inlined copy is @media-wrapped (NOT byte-identical to the external sheet, so a
// downstream text-dedup would miss it), but the external <link> is removed
// outright, so the browser sees the sheet exactly once.
TEST(InlineCachedStylesheets, RemovesInlinedLinkMediaScoped) {
  std::string html =
      "<html><head>"
      "<link rel=\"stylesheet\" href=\"/screen.css\" media=\"screen\">"
      "</head><body></body></html>";

  auto lookup = MakeLookup({{"/screen.css", "body { margin: 0; }"}});

  std::string result =
      InlineCachedStylesheets(html, "http://example.com/", lookup);

  EXPECT_NE(std::string::npos,
            result.find("@media screen { body { margin: 0; } }"));
  EXPECT_EQ(std::string::npos, result.find("href=\"/screen.css\""));
}

// An entity-encoded source href (&amp;) still matches the inlined sheet and is
// removed: both the scanner and the removal filter compare parser-decoded hrefs.
TEST(InlineCachedStylesheets, RemovesEntityEncodedHrefLink) {
  std::string html =
      "<html><head>"
      "<link rel=\"stylesheet\" href=\"/a.css?x=1&amp;y=2\">"
      "</head><body></body></html>";

  // The scanner decodes the href; the cache is keyed on the decoded URL.
  auto lookup = MakeLookup({{"/a.css?x=1&y=2", "body { margin: 0; }"}});

  CssInliningStats stats;
  std::string result =
      InlineCachedStylesheets(html, "http://example.com/", lookup, &stats);

  EXPECT_EQ(1u, stats.stylesheets_cached);
  EXPECT_NE(std::string::npos, result.find("body { margin: 0; }"));
  // The external link is gone despite the &amp; spelling.
  EXPECT_EQ(std::string::npos, result.find("href=\"/a.css"));
}

// Hrefs resolve against the document's <base href>, not the page
// directory, as the serve path's combined-stylesheet gather resolves them.
// Under a cross-host base a root-relative href is the base host's sheet and
// the lookup is asked for it by that absolute URL; before the fix it asked for
// the page host's "/css/site.css" and the sheet was never inlined.
TEST(InlineCachedStylesheets, CrossHostBaseHrefResolvesToTheBaseHost) {
  std::string html =
      "<html><head><base href=\"https://cdn.example.com/assets/\">"
      "<link rel=\"stylesheet\" href=\"/css/site.css\">"
      "<link rel=\"stylesheet\" href=\"theme.css\">"
      "</head><body></body></html>";

  std::vector<std::string> looked_up;
  auto lookup = MakeLookup(
      {{"https://cdn.example.com/css/site.css", "body { margin: 0; }"},
       {"https://cdn.example.com/assets/theme.css", "h1 { color: red; }"}});
  css::CssLookupFn recording =
      [&](std::string_view url) -> std::optional<std::string> {
    looked_up.emplace_back(url);
    return lookup(url);
  };

  CssInliningStats stats;
  std::string result = InlineCachedStylesheets(
      html, "https://example.com/page.html", recording, &stats);

  EXPECT_EQ(
      (std::vector<std::string>{"https://cdn.example.com/css/site.css",
                                "https://cdn.example.com/assets/theme.css"}),
      looked_up);
  EXPECT_EQ(2u, stats.stylesheets_cached);
  EXPECT_NE(std::string::npos, result.find("body { margin: 0; }"));
  EXPECT_NE(std::string::npos, result.find("h1 { color: red; }"));
  EXPECT_EQ(std::string::npos, result.find("rel=\"stylesheet\""));
}

// A protocol-relative href gets the document's scheme,
// with no <base> at all, so the lookup is asked for a URL whose host and path
// can be split: the CDN's "/c.css" keyed by the CDN host, the page's own
// "/d.css" keyed by the page host. Before the fix the key was the literal
// "//host/path" on the page host, which nginx never stores: a guaranteed miss,
// so the sheet counted as missing and the page was marked for revalidation on
// every request. The same resolution serves the combined-stylesheet gather.
TEST(InlineCachedStylesheets, ProtocolRelativeHrefGetsTheDocumentScheme) {
  std::string html =
      "<html><head>"
      "<link rel=\"stylesheet\" href=\"//cdn.example.com/c.css\">"
      "<link rel=\"stylesheet\" href=\"//example.com/d.css\">"
      "</head><body></body></html>";

  std::vector<std::string> looked_up;
  auto lookup = MakeLookup({{"https://cdn.example.com/c.css", "p{margin:0}"},
                            {"https://example.com/d.css", "q{margin:0}"}});
  css::CssLookupFn recording =
      [&](std::string_view url) -> std::optional<std::string> {
    looked_up.emplace_back(url);
    return lookup(url);
  };

  CssInliningStats stats;
  std::string result = InlineCachedStylesheets(
      html, "https://example.com/page.html", recording, &stats);

  EXPECT_EQ((std::vector<std::string>{"https://cdn.example.com/c.css",
                                      "https://example.com/d.css"}),
            looked_up);
  EXPECT_EQ(2u, stats.stylesheets_cached);
  EXPECT_NE(std::string::npos, result.find("p{margin:0}"));
  EXPECT_NE(std::string::npos, result.find("q{margin:0}"));

  // The worker hands over the cache-normalized page path with the scheme
  // beside it; the href then takes that scheme.
  looked_up.clear();
  InlineCachedStylesheets(html, "/page.html", recording, &stats, "example.com",
                          "http");
  EXPECT_EQ((std::vector<std::string>{"http://cdn.example.com/c.css",
                                      "http://example.com/d.css"}),
            looked_up);
}

// A root-relative <base href> moves the directory relative hrefs resolve in
// but names no host of its own, so the result stays the page's own path and
// the caller keys it by the page host, as before.
TEST(InlineCachedStylesheets, RootRelativeBaseHrefMovesTheDirectory) {
  std::string html =
      "<html><head><base href=\"/sub/\">"
      "<link rel=\"stylesheet\" href=\"a.css\">"
      "</head><body></body></html>";

  std::string looked_up_url;
  css::CssLookupFn lookup =
      [&](std::string_view url) -> std::optional<std::string> {
    looked_up_url = std::string(url);
    return "p { margin: 0; }";
  };

  std::string result =
      InlineCachedStylesheets(html, "https://example.com/page.html", lookup);

  EXPECT_EQ("/sub/a.css", looked_up_url);
  EXPECT_NE(std::string::npos, result.find("p { margin: 0; }"));
}

}  // namespace
}  // namespace pagespeed
