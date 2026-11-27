// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

// Test for the Critical CSS Extractor

#include "src/worker/critical_css_extractor.h"

#include <algorithm>
#include <limits>
#include <string>
#include <vector>

#include "absl/container/flat_hash_set.h"
#include "absl/strings/str_cat.h"
#include "gtest/gtest.h"
#include "lib/classify/capability_mask.h"
#include "src/worker/cascade_layer_order.h"
#include "src/worker/html_css_injector.h"

namespace pagespeed {
namespace {

// Helper to create test elements
CollectedElement MakeElement(const std::string& tag, int depth, int index,
                             const std::string& id = "",
                             const std::vector<std::string>& classes = {}) {
  CollectedElement elem;
  elem.tag_name = tag;
  elem.depth = depth;
  elem.element_index = index;
  elem.id = id;
  elem.classes = classes;
  return elem;
}

TEST(CriticalCssExtractorTest, ExtractFromEmptyCss) {
  CriticalCssExtractor extractor;
  std::vector<CollectedElement> elements;
  elements.push_back(MakeElement("div", 0, 0));

  CriticalCssResult result = extractor.Extract(elements, "");

  EXPECT_TRUE(result.success);
  EXPECT_TRUE(result.critical_css.empty());
  EXPECT_EQ(0, result.total_rules);
  EXPECT_EQ(0, result.critical_rules);
}

TEST(CriticalCssExtractorTest, IncludesUniversalSelector) {
  CriticalCssExtractor extractor;
  std::vector<CollectedElement> elements;
  elements.push_back(MakeElement("body", 0, 0));

  std::string css = "* { box-sizing: border-box; }";
  CriticalCssResult result = extractor.Extract(elements, css);

  EXPECT_TRUE(result.success);
  EXPECT_NE(result.critical_css.find("* {"), std::string::npos);
  EXPECT_EQ(1, result.critical_rules);
}

TEST(CriticalCssExtractorTest, IncludesHtmlBodySelectors) {
  CriticalCssExtractor extractor;
  std::vector<CollectedElement> elements;
  elements.push_back(MakeElement("html", 0, 0));
  elements.push_back(MakeElement("body", 1, 1));

  std::string css =
      "html { font-size: 16px; }\n"
      "body { margin: 0; }\n"
      ":root { --color: red; }";

  CriticalCssResult result = extractor.Extract(elements, css);

  EXPECT_TRUE(result.success);
  EXPECT_NE(result.critical_css.find("html {"), std::string::npos);
  EXPECT_NE(result.critical_css.find("body {"), std::string::npos);
  EXPECT_NE(result.critical_css.find(":root {"), std::string::npos);
  EXPECT_EQ(3, result.critical_rules);
}

TEST(CriticalCssExtractorTest, MatchesClassSelector) {
  CriticalCssExtractor extractor;
  std::vector<CollectedElement> elements;
  elements.push_back(MakeElement("div", 0, 0, "", {"container"}));

  std::string css =
      ".container { max-width: 1200px; }\n"
      ".footer { margin-top: 50px; }";

  CriticalCssResult result = extractor.Extract(elements, css);

  EXPECT_TRUE(result.success);
  EXPECT_NE(result.critical_css.find(".container {"), std::string::npos);
  // Footer should not be included (excluded pattern)
  EXPECT_EQ(result.critical_css.find(".footer {"), std::string::npos);
}

TEST(CriticalCssExtractorTest, MatchesIdSelector) {
  CriticalCssExtractor extractor;
  std::vector<CollectedElement> elements;
  elements.push_back(MakeElement("div", 0, 0, "main-content"));

  std::string css =
      "#main-content { padding: 20px; }\n"
      "#sidebar { width: 300px; }";

  CriticalCssResult result = extractor.Extract(elements, css);

  EXPECT_TRUE(result.success);
  EXPECT_NE(result.critical_css.find("#main-content {"), std::string::npos);
  // Sidebar not in elements, should not be included
  EXPECT_EQ(result.critical_css.find("#sidebar {"), std::string::npos);
}

TEST(CriticalCssExtractorTest, MatchesTagSelector) {
  CriticalCssExtractor extractor;
  std::vector<CollectedElement> elements;
  elements.push_back(MakeElement("header", 0, 0));
  elements.push_back(MakeElement("nav", 1, 1));

  std::string css =
      "header { background: #fff; }\n"
      "nav { display: flex; }";

  CriticalCssResult result = extractor.Extract(elements, css);

  EXPECT_TRUE(result.success);
  EXPECT_NE(result.critical_css.find("header {"), std::string::npos);
  EXPECT_NE(result.critical_css.find("nav {"), std::string::npos);
}

TEST(CriticalCssExtractorTest, MatchesCombinedSelector) {
  CriticalCssExtractor extractor;
  std::vector<CollectedElement> elements;
  elements.push_back(MakeElement("div", 0, 0, "hero", {"container", "dark"}));

  std::string css = "div#hero.container.dark { color: white; }";

  CriticalCssResult result = extractor.Extract(elements, css);

  EXPECT_TRUE(result.success);
  EXPECT_NE(result.critical_css.find("div#hero.container.dark {"),
            std::string::npos);
}

TEST(CriticalCssExtractorTest, ExcludesMediaPrintRules) {
  CriticalCssExtractor extractor;
  std::vector<CollectedElement> elements;
  elements.push_back(MakeElement("body", 0, 0));

  std::string css =
      "body { margin: 0; }\n"
      "@media print { body { font-size: 12pt; } }";

  CriticalCssResult result = extractor.Extract(elements, css);

  EXPECT_TRUE(result.success);
  EXPECT_NE(result.critical_css.find("body { margin"), std::string::npos);
  EXPECT_EQ(result.critical_css.find("@media print"), std::string::npos);
}

TEST(CriticalCssExtractorTest, IncludesFontFaceRules) {
  CriticalCssExtractor extractor;
  std::vector<CollectedElement> elements;
  elements.push_back(MakeElement("body", 0, 0));

  std::string css =
      "@font-face { font-family: 'Custom'; src: url('font.woff2'); }";

  CriticalCssResult result = extractor.Extract(elements, css);

  EXPECT_TRUE(result.success);
  EXPECT_NE(result.critical_css.find("@font-face"), std::string::npos);
}

TEST(CriticalCssExtractorTest, IncludesKeyframesRules) {
  CriticalCssExtractor extractor;
  std::vector<CollectedElement> elements;
  elements.push_back(MakeElement("body", 0, 0));

  std::string css =
      "@keyframes fadeIn { from { opacity: 0; } to { opacity: 1; } }";

  CriticalCssResult result = extractor.Extract(elements, css);

  EXPECT_TRUE(result.success);
  EXPECT_NE(result.critical_css.find("@keyframes fadeIn"), std::string::npos);
}

TEST(CriticalCssExtractorTest, ExcludesElementsWithExcludedClassPattern) {
  CriticalCssExtractor extractor;
  std::vector<CollectedElement> elements;
  elements.push_back(MakeElement("div", 0, 0, "", {"hero"}));
  elements.push_back(MakeElement("div", 1, 1, "", {"lazy-load"}));
  elements.push_back(MakeElement("footer", 2, 2, "", {"site-footer"}));

  std::string css =
      ".hero { height: 100vh; }\n"
      ".lazy-load { opacity: 0; }\n"
      ".site-footer { margin-top: 50px; }";

  CriticalCssResult result = extractor.Extract(elements, css);

  EXPECT_TRUE(result.success);
  EXPECT_NE(result.critical_css.find(".hero {"), std::string::npos);
  // lazy-load and footer should be excluded
  EXPECT_EQ(result.critical_css.find(".lazy-load {"), std::string::npos);
  EXPECT_EQ(result.critical_css.find(".site-footer {"), std::string::npos);
}

TEST(CriticalCssExtractorTest, IncludesHeaderNavHeroPatterns) {
  CriticalCssExtractor extractor;
  std::vector<CollectedElement> elements;
  elements.push_back(MakeElement("section", 0, 0, "", {"hero-section"}));
  elements.push_back(MakeElement("div", 1, 1, "header-wrapper"));
  elements.push_back(MakeElement("nav", 2, 2, "main-nav"));

  std::string css =
      ".hero-section { min-height: 100vh; }\n"
      "#header-wrapper { position: fixed; }\n"
      "#main-nav { display: flex; }";

  CriticalCssResult result = extractor.Extract(elements, css);

  EXPECT_TRUE(result.success);
  // These should all be included due to pattern matching
  EXPECT_NE(result.critical_css.find(".hero-section {"), std::string::npos);
  EXPECT_NE(result.critical_css.find("#header-wrapper {"), std::string::npos);
  EXPECT_NE(result.critical_css.find("#main-nav {"), std::string::npos);
}

TEST(CriticalCssExtractorTest, RespectsMaxElements) {
  CriticalCssConfig config;
  config.max_elements = 2;
  CriticalCssExtractor extractor(config);

  std::vector<CollectedElement> elements;
  elements.push_back(MakeElement("div", 0, 0, "", {"first"}));
  elements.push_back(MakeElement("div", 0, 1, "", {"second"}));
  elements.push_back(MakeElement("div", 0, 2, "", {"third"}));  // Beyond limit

  std::string css =
      ".first { color: red; }\n"
      ".second { color: blue; }\n"
      ".third { color: green; }";

  CriticalCssResult result = extractor.Extract(elements, css);

  EXPECT_TRUE(result.success);
  EXPECT_NE(result.critical_css.find(".first {"), std::string::npos);
  EXPECT_NE(result.critical_css.find(".second {"), std::string::npos);
  // Third is beyond max_elements and doesn't have include patterns
  // (but since max_depth is still 10, it might still be included)
}

TEST(CriticalCssExtractorTest, RespectsMaxDepth) {
  CriticalCssConfig config;
  config.max_depth = 2;
  config.max_elements = 100;  // Don't limit by elements
  CriticalCssExtractor extractor(config);

  std::vector<CollectedElement> elements;
  elements.push_back(MakeElement("div", 1, 0, "", {"shallow"}));
  elements.push_back(MakeElement("div", 5, 1, "", {"deep"}));  // Beyond depth

  std::string css =
      ".shallow { color: red; }\n"
      ".deep { color: blue; }";

  CriticalCssResult result = extractor.Extract(elements, css);

  EXPECT_TRUE(result.success);
  EXPECT_NE(result.critical_css.find(".shallow {"), std::string::npos);
  // Deep element is excluded due to depth
  EXPECT_EQ(result.critical_css.find(".deep {"), std::string::npos);
}

TEST(CriticalCssExtractorTest, HandlesCommaSeparatedSelectors) {
  CriticalCssExtractor extractor;
  std::vector<CollectedElement> elements;
  elements.push_back(MakeElement("h1", 0, 0));
  elements.push_back(MakeElement("h2", 0, 1));

  std::string css = "h1, h2, h3, h4 { font-weight: bold; }";

  CriticalCssResult result = extractor.Extract(elements, css);

  EXPECT_TRUE(result.success);
  // Should be included because h1 and h2 are in elements
  EXPECT_NE(result.critical_css.find("h1, h2, h3, h4 {"), std::string::npos);
}

TEST(CriticalCssExtractorTest, HandlesDescendantSelectors) {
  CriticalCssExtractor extractor;
  std::vector<CollectedElement> elements;
  elements.push_back(MakeElement("header", 0, 0));
  elements.push_back(MakeElement("nav", 1, 1));
  elements.push_back(MakeElement("a", 2, 2));

  std::string css = "header nav a { color: blue; }";

  CriticalCssResult result = extractor.Extract(elements, css);

  EXPECT_TRUE(result.success);
  // Should match because 'a' is in elements
  EXPECT_NE(result.critical_css.find("header nav a {"), std::string::npos);
}

TEST(CriticalCssExtractorTest, HandlesChildSelectors) {
  CriticalCssExtractor extractor;
  std::vector<CollectedElement> elements;
  elements.push_back(MakeElement("ul", 0, 0, "", {"menu"}));
  elements.push_back(MakeElement("li", 1, 1));

  std::string css = "ul.menu > li { list-style: none; }";

  CriticalCssResult result = extractor.Extract(elements, css);

  EXPECT_TRUE(result.success);
  // Should match because 'li' is in elements
  EXPECT_NE(result.critical_css.find("ul.menu > li {"), std::string::npos);
}

TEST(CriticalCssExtractorTest, HandlesCssComments) {
  CriticalCssExtractor extractor;
  std::vector<CollectedElement> elements;
  elements.push_back(MakeElement("body", 0, 0));

  std::string css =
      "/* This is a comment */\n"
      "body { margin: 0; }\n"
      "/* Another comment */";

  CriticalCssResult result = extractor.Extract(elements, css);

  EXPECT_TRUE(result.success);
  EXPECT_NE(result.critical_css.find("body {"), std::string::npos);
  EXPECT_EQ(1, result.critical_rules);
}

TEST(CriticalCssExtractorTest, HandlesNestedBraces) {
  CriticalCssExtractor extractor;
  std::vector<CollectedElement> elements;
  elements.push_back(MakeElement("div", 0, 0));

  std::string css =
      "@media screen {\n"
      "  div { padding: 10px; }\n"
      "}";

  CriticalCssResult result = extractor.Extract(elements, css);

  EXPECT_TRUE(result.success);
  // @media screen should be included (not print)
  EXPECT_NE(result.critical_css.find("@media screen"), std::string::npos);
}

TEST(CriticalCssExtractorTest, PreservesOriginalCssFormatting) {
  CriticalCssExtractor extractor;
  std::vector<CollectedElement> elements;
  elements.push_back(MakeElement("body", 0, 0));

  std::string css = "body {\n  margin: 0;\n  padding: 0;\n}";

  CriticalCssResult result = extractor.Extract(elements, css);

  EXPECT_TRUE(result.success);
  // Body content should be preserved
  EXPECT_NE(result.critical_css.find("margin: 0"), std::string::npos);
  EXPECT_NE(result.critical_css.find("padding: 0"), std::string::npos);
}

TEST(CriticalCssExtractorTest, StatisticsAreAccurate) {
  CriticalCssExtractor extractor;
  std::vector<CollectedElement> elements;
  elements.push_back(MakeElement("body", 0, 0));
  elements.push_back(MakeElement("div", 1, 1, "", {"content"}));

  std::string css =
      "body { margin: 0; }\n"
      ".content { padding: 20px; }\n"
      ".footer { margin-top: 50px; }\n"
      "@media print { body { font-size: 12pt; } }";

  CriticalCssResult result = extractor.Extract(elements, css);

  EXPECT_TRUE(result.success);
  EXPECT_EQ(4, result.total_rules);
  // body and .content should be included, .footer and @media print excluded
  EXPECT_EQ(2, result.critical_rules);
}

// --- Viewport-aware @media filtering tests ---

TEST(CriticalCssExtractorTest, ExcludesDesktopMediaForMobile) {
  CriticalCssExtractor extractor;
  std::vector<CollectedElement> elements;
  elements.push_back(MakeElement("body", 0, 0));

  std::string css =
      "body { margin: 0; }\n"
      "@media (min-width: 1024px) { .desktop-only { display: block; } }";

  CriticalCssResult result =
      extractor.Extract(elements, css, CapabilityMask::Viewport::kMobile);

  EXPECT_TRUE(result.success);
  EXPECT_NE(result.critical_css.find("body {"), std::string::npos);
  EXPECT_EQ(result.critical_css.find("@media (min-width: 1024px)"),
            std::string::npos)
      << "Desktop-only @media should be excluded for mobile";
}

TEST(CriticalCssExtractorTest, ExcludesMobileMediaForDesktop) {
  CriticalCssExtractor extractor;
  std::vector<CollectedElement> elements;
  elements.push_back(MakeElement("body", 0, 0));

  std::string css =
      "body { margin: 0; }\n"
      "@media (max-width: 280px) { body { display: block; } }\n"
      "@media (max-width: 479px) { body { display: block; } }";

  CriticalCssResult result =
      extractor.Extract(elements, css, CapabilityMask::Viewport::kDesktop);

  EXPECT_TRUE(result.success);
  EXPECT_NE(result.critical_css.find("body {"), std::string::npos);
  EXPECT_NE(result.critical_css.find("@media (max-width: 280px)"),
            std::string::npos)
      << "every class's windows start at 0 (zoom, fold covers, KaiOS)";
  EXPECT_NE(result.critical_css.find("@media (max-width: 479px)"),
            std::string::npos)
      << "a desktop User-Agent can have a phone-width window";
}

TEST(CriticalCssExtractorTest, IncludesOverlappingMedia) {
  CriticalCssExtractor extractor;
  std::vector<CollectedElement> elements;
  elements.push_back(MakeElement("body", 0, 0));

  // min-width: 400px overlaps Mobile (0-980) and Tablet (0-1480)
  std::string css = "@media (min-width: 400px) { body { display: flex; } }";

  CriticalCssResult result_mobile =
      extractor.Extract(elements, css, CapabilityMask::Viewport::kMobile);
  CriticalCssResult result_tablet =
      extractor.Extract(elements, css, CapabilityMask::Viewport::kTablet);

  EXPECT_NE(result_mobile.critical_css.find("@media (min-width: 400px)"),
            std::string::npos)
      << "Should be included for mobile (overlaps 400-980)";
  EXPECT_NE(result_tablet.critical_css.find("@media (min-width: 400px)"),
            std::string::npos)
      << "Should be included for tablet (overlaps 400-1480)";
}

TEST(CriticalCssExtractorTest, IncludesUnrecognizedMediaConservatively) {
  CriticalCssExtractor extractor;
  std::vector<CollectedElement> elements;
  elements.push_back(MakeElement("body", 0, 0));

  // `(hover: hover)` is read since touch emulation (and dropped for the phone);
  // orientation is a feature the evaluator does not read.
  std::string css =
      "@media screen and (orientation: portrait) { body { margin: 0; } }";

  CriticalCssResult result_mobile =
      extractor.Extract(elements, css, CapabilityMask::Viewport::kMobile);
  CriticalCssResult result_desktop =
      extractor.Extract(elements, css, CapabilityMask::Viewport::kDesktop);

  EXPECT_NE(result_mobile.critical_css.find("@media screen"), std::string::npos)
      << "Unrecognized media query should be conservatively included (mobile)";
  EXPECT_NE(result_desktop.critical_css.find("@media screen"),
            std::string::npos)
      << "Unrecognized media query should be conservatively included "
         "(desktop)";
}

TEST(CriticalCssExtractorTest, IncludesMinMaxOverlap) {
  CriticalCssExtractor extractor;
  std::vector<CollectedElement> elements;
  elements.push_back(MakeElement("body", 0, 0));

  // 1000-1279: wider than any phone window (RetentionWidthRange: mobile
  // 0-980), within the tablet (0-1480) and desktop (0+) ranges.
  std::string css =
      "@media (min-width: 1000px) and (max-width: 1279px) "
      "{ body { display: block; } }";

  CriticalCssResult result_mobile =
      extractor.Extract(elements, css, CapabilityMask::Viewport::kMobile);
  CriticalCssResult result_tablet =
      extractor.Extract(elements, css, CapabilityMask::Viewport::kTablet);
  CriticalCssResult result_desktop =
      extractor.Extract(elements, css, CapabilityMask::Viewport::kDesktop);

  EXPECT_EQ(result_mobile.critical_css.find("@media"), std::string::npos)
      << "1000-1279 range should be excluded for mobile (0-980)";
  EXPECT_NE(result_tablet.critical_css.find("@media"), std::string::npos)
      << "1000-1279 range should be included for tablet (0-1480)";
  EXPECT_NE(result_desktop.critical_css.find("@media"), std::string::npos)
      << "1000-1279 range should be included for desktop (0+)";
}

TEST(CriticalCssExtractorTest, DefaultViewportIsDesktop) {
  CriticalCssExtractor extractor;
  std::vector<CollectedElement> elements;
  elements.push_back(MakeElement("body", 0, 0));

  // Wider than any tablet window: only the desktop class keeps it.
  std::string css = "@media (min-width: 1481px) { body { display: block; } }";

  // Default (no viewport arg) should behave as desktop
  CriticalCssResult result_default = extractor.Extract(elements, css);
  CriticalCssResult result_desktop =
      extractor.Extract(elements, css, CapabilityMask::Viewport::kDesktop);

  EXPECT_EQ(result_default.critical_css, result_desktop.critical_css)
      << "Default viewport should match desktop behavior";
  // Both keep the query a tablet or phone would drop.
  EXPECT_NE(result_default.critical_css.find("@media"), std::string::npos);
  EXPECT_EQ(extractor.Extract(elements, css, CapabilityMask::Viewport::kTablet)
                .critical_css.find("@media"),
            std::string::npos);
}

TEST(CriticalCssExtractorTest, HandlesEmUnitsInMedia) {
  CriticalCssExtractor extractor;
  std::vector<CollectedElement> elements;
  elements.push_back(MakeElement("body", 0, 0));

  // 64em = 64 * 16 = 1024px — should match desktop but not mobile
  std::string css = "@media (min-width: 64em) { body { display: block; } }";

  CriticalCssResult result_mobile =
      extractor.Extract(elements, css, CapabilityMask::Viewport::kMobile);
  CriticalCssResult result_desktop =
      extractor.Extract(elements, css, CapabilityMask::Viewport::kDesktop);

  EXPECT_EQ(result_mobile.critical_css.find("@media"), std::string::npos)
      << "64em (1024px) should be excluded for mobile (0-980)";
  EXPECT_NE(result_desktop.critical_css.find("@media"), std::string::npos)
      << "64em (1024px) should be included for desktop (1024-65535)";
}

TEST(CriticalCssExtractorTest, HandlesRemUnitsInMedia) {
  CriticalCssExtractor extractor;
  std::vector<CollectedElement> elements;
  elements.push_back(MakeElement("body", 0, 0));

  // 62rem = 62 * 16 = 992px — should match tablet but not mobile (0-980)
  std::string css = "@media (min-width: 62rem) { body { display: block; } }";

  CriticalCssResult result_mobile =
      extractor.Extract(elements, css, CapabilityMask::Viewport::kMobile);
  CriticalCssResult result_tablet =
      extractor.Extract(elements, css, CapabilityMask::Viewport::kTablet);

  EXPECT_EQ(result_mobile.critical_css.find("@media"), std::string::npos)
      << "62rem (992px) should be excluded for mobile (0-980)";
  EXPECT_NE(result_tablet.critical_css.find("@media"), std::string::npos)
      << "62rem (992px) should be included for tablet (0-1480)";
}

TEST(CriticalCssExtractorTest, HandlesMediaQueryList) {
  CriticalCssExtractor extractor;
  std::vector<CollectedElement> elements;
  elements.push_back(MakeElement("body", 0, 0));

  // Comma-separated list: an empty band, or wider than any tablet.
  std::string css =
      "@media (min-width: 800px) and (max-width: 700px), (min-width: 1481px) "
      "{ body { display: block; } }";

  CriticalCssResult result_mobile =
      extractor.Extract(elements, css, CapabilityMask::Viewport::kMobile);
  CriticalCssResult result_tablet =
      extractor.Extract(elements, css, CapabilityMask::Viewport::kTablet);
  CriticalCssResult result_desktop =
      extractor.Extract(elements, css, CapabilityMask::Viewport::kDesktop);

  EXPECT_EQ(result_mobile.critical_css.find("@media"), std::string::npos)
      << "Should exclude for mobile (neither subquery overlaps 0-980)";
  EXPECT_EQ(result_tablet.critical_css.find("@media"), std::string::npos)
      << "Should exclude for tablet (neither subquery overlaps 0-1480)";
  EXPECT_NE(result_desktop.critical_css.find("@media"), std::string::npos)
      << "Should include for desktop (second subquery matches 1481+)";
}

TEST(CriticalCssExtractorTest, MediaQueryListWithUnrecognizedSubquery) {
  CriticalCssExtractor extractor;
  std::vector<CollectedElement> elements;
  elements.push_back(MakeElement("body", 0, 0));

  // One parseable subquery that doesn't match + one unrecognized
  std::string css =
      "@media (max-width: 479px), (orientation: portrait) "
      "{ body { display: block; } }";

  // Should conservatively include for all viewports because one
  // subquery is unrecognized.
  CriticalCssResult result_desktop =
      extractor.Extract(elements, css, CapabilityMask::Viewport::kDesktop);

  EXPECT_NE(result_desktop.critical_css.find("@media"), std::string::npos)
      << "Should conservatively include when any subquery is unrecognized";
}

// --- New tests for overflow bug and edge cases ---

TEST(CriticalCssExtractorTest, HugeValueInMediaQueryDoesNotOverflow) {
  CriticalCssExtractor extractor;
  std::vector<CollectedElement> elements;
  elements.push_back(MakeElement("body", 0, 0));

  // A huge value like 999999999999px would overflow a 32-bit int. It is read
  // as a (very large) length without UB: no window is that wide, so the block
  // is excluded everywhere. A number too long to read at all is kept.
  std::string css =
      "@media (min-width: 999999999999px) { body { display: block; } }";
  std::string css_too_long =
      "@media (min-width: 99999999999999999999999px) { body { color: red; } }";

  CriticalCssResult result_mobile =
      extractor.Extract(elements, css, CapabilityMask::Viewport::kMobile);
  CriticalCssResult result_desktop =
      extractor.Extract(elements, css, CapabilityMask::Viewport::kDesktop);

  EXPECT_EQ(result_mobile.critical_css.find("@media"), std::string::npos)
      << "Huge min-width cannot apply on mobile";
  EXPECT_EQ(result_desktop.critical_css.find("@media"), std::string::npos)
      << "Huge min-width cannot apply on desktop";
  CriticalCssResult too_long = extractor.Extract(
      elements, css_too_long, CapabilityMask::Viewport::kDesktop);
  EXPECT_NE(too_long.critical_css.find("@media"), std::string::npos)
      << "An unreadable number is conservatively included";
}

TEST(CriticalCssExtractorTest, HandlesFractionalEmInMedia) {
  CriticalCssExtractor extractor;
  std::vector<CollectedElement> elements;
  elements.push_back(MakeElement("body", 0, 0));

  // 61.9375em * 16 = 991px -- should be excluded for mobile (0-980),
  // included for tablet (0-1480).
  std::string css =
      "@media (min-width: 61.9375em) { body { display: block; } }";

  CriticalCssResult result_mobile =
      extractor.Extract(elements, css, CapabilityMask::Viewport::kMobile);
  CriticalCssResult result_tablet =
      extractor.Extract(elements, css, CapabilityMask::Viewport::kTablet);

  EXPECT_EQ(result_mobile.critical_css.find("@media"), std::string::npos)
      << "61.9375em (991px) should be excluded for mobile (0-980)";
  EXPECT_NE(result_tablet.critical_css.find("@media"), std::string::npos)
      << "61.9375em (991px) should be included for tablet (0-1480)";

  // 1.5em * 16 = 24px -- should be included for all viewports.
  std::string css2 = "@media (min-width: 1.5em) { body { display: block; } }";

  CriticalCssResult result2_mobile =
      extractor.Extract(elements, css2, CapabilityMask::Viewport::kMobile);
  CriticalCssResult result2_desktop =
      extractor.Extract(elements, css2, CapabilityMask::Viewport::kDesktop);

  EXPECT_NE(result2_mobile.critical_css.find("@media"), std::string::npos)
      << "1.5em (24px) should be included for mobile";
  EXPECT_NE(result2_desktop.critical_css.find("@media"), std::string::npos)
      << "1.5em (24px) should be included for desktop";
}

TEST(CriticalCssExtractorTest, HandlesCombinedScreenPrefixWithEm) {
  CriticalCssExtractor extractor;
  std::vector<CollectedElement> elements;
  elements.push_back(MakeElement("body", 0, 0));

  // "screen and" prefix + em unit: 64em = 1024px
  std::string css =
      "@media screen and (min-width: 64em) "
      "{ body { display: block; } }";

  CriticalCssResult result_mobile =
      extractor.Extract(elements, css, CapabilityMask::Viewport::kMobile);
  CriticalCssResult result_desktop =
      extractor.Extract(elements, css, CapabilityMask::Viewport::kDesktop);

  EXPECT_EQ(result_mobile.critical_css.find("@media"), std::string::npos)
      << "screen and 64em (1024px) should be excluded for mobile";
  EXPECT_NE(result_desktop.critical_css.find("@media"), std::string::npos)
      << "screen and 64em (1024px) should be included for desktop";
}

TEST(CriticalCssExtractorTest, NotKeywordIsEvaluated) {
  CriticalCssExtractor extractor;
  std::vector<CollectedElement> elements;
  elements.push_back(MakeElement("body", 0, 0));

  // `not (max-width: 479px)` is 480 px and up: both a phone in landscape
  // and a desktop window can be that wide.
  std::string css =
      "@media not (max-width: 479px) { body { display: block; } }";

  CriticalCssResult result_mobile =
      extractor.Extract(elements, css, CapabilityMask::Viewport::kMobile);
  CriticalCssResult result_desktop =
      extractor.Extract(elements, css, CapabilityMask::Viewport::kDesktop);

  EXPECT_NE(result_mobile.critical_css.find("@media"), std::string::npos)
      << "'not (max-width: 479px)' can apply on mobile (landscape)";
  EXPECT_NE(result_desktop.critical_css.find("@media"), std::string::npos)
      << "'not (max-width: 479px)' can apply on desktop";

  // "not screen and (min-width: 1024px)" is every width below 1024.
  std::string css2 =
      "@media not screen and (min-width: 1024px) "
      "{ body { display: block; } }";

  CriticalCssResult result2_mobile =
      extractor.Extract(elements, css2, CapabilityMask::Viewport::kMobile);
  CriticalCssResult result2_desktop =
      extractor.Extract(elements, css2, CapabilityMask::Viewport::kDesktop);

  EXPECT_NE(result2_mobile.critical_css.find("@media"), std::string::npos)
      << "'not screen and (min-width: 1024px)' applies on mobile";
  EXPECT_NE(result2_desktop.critical_css.find("@media"), std::string::npos)
      << "'not screen and (min-width: 1024px)' applies to a narrow desktop";

  // And a `not` that no window of the class satisfies is excluded.
  std::string css3 =
      "@media not all and (max-width: 980px) { body { display: block; } }";
  EXPECT_EQ(extractor.Extract(elements, css3, CapabilityMask::Viewport::kMobile)
                .critical_css.find("@media"),
            std::string::npos)
      << "above 980 px is past every phone window";
}

TEST(CriticalCssExtractorTest, HandlesThreeSubqueries) {
  CriticalCssExtractor extractor;
  std::vector<CollectedElement> elements;
  elements.push_back(MakeElement("body", 0, 0));

  // Three subqueries: each covers one viewport class exactly.
  std::string css =
      "@media (max-width: 479px), "
      "(min-width: 480px) and (max-width: 1279px), "
      "(min-width: 1280px) "
      "{ body { display: block; } }";

  CriticalCssResult result_mobile =
      extractor.Extract(elements, css, CapabilityMask::Viewport::kMobile);
  CriticalCssResult result_tablet =
      extractor.Extract(elements, css, CapabilityMask::Viewport::kTablet);
  CriticalCssResult result_desktop =
      extractor.Extract(elements, css, CapabilityMask::Viewport::kDesktop);

  EXPECT_NE(result_mobile.critical_css.find("@media"), std::string::npos)
      << "First subquery (max-width: 479px) should match mobile";
  EXPECT_NE(result_tablet.critical_css.find("@media"), std::string::npos)
      << "Second subquery (480px-1279px) should match tablet";
  EXPECT_NE(result_desktop.critical_css.find("@media"), std::string::npos)
      << "Third subquery (min-width: 1280px) should match desktop";
}

TEST(CriticalCssExtractorTest, HandlesOnlyScreenPrefix) {
  CriticalCssExtractor extractor;
  std::vector<CollectedElement> elements;
  elements.push_back(MakeElement("body", 0, 0));

  // "only screen and" prefix should be stripped, leaving min-width: 1024px
  std::string css =
      "@media only screen and (min-width: 1024px) "
      "{ body { display: block; } }";

  CriticalCssResult result_mobile =
      extractor.Extract(elements, css, CapabilityMask::Viewport::kMobile);
  CriticalCssResult result_desktop =
      extractor.Extract(elements, css, CapabilityMask::Viewport::kDesktop);

  EXPECT_EQ(result_mobile.critical_css.find("@media"), std::string::npos)
      << "only screen and 1024px should be excluded for mobile";
  EXPECT_NE(result_desktop.critical_css.find("@media"), std::string::npos)
      << "only screen and 1024px should be included for desktop";
}

// ========== Phase 3.4: prefers-color-scheme preservation ==========

TEST(CriticalCssExtractorTest, PrefersColorSchemeDarkPreserved) {
  CriticalCssExtractor extractor;
  std::vector<CollectedElement> elements;
  elements.push_back(MakeElement("html", 0, 0));
  elements.push_back(MakeElement("body", 1, 1));

  std::string css =
      "body { background: #fff; }\n"
      "@media (prefers-color-scheme: dark) {\n"
      "  body { background: #111; color: #eee; }\n"
      "}";

  CriticalCssResult result =
      extractor.Extract(elements, css, CapabilityMask::Viewport::kDesktop);

  EXPECT_TRUE(result.success);
  // prefers-color-scheme is not a viewport media query — it should be
  // conservatively included since the extractor can't evaluate it.
  EXPECT_NE(result.critical_css.find("prefers-color-scheme"), std::string::npos)
      << "prefers-color-scheme media query should be preserved in critical CSS";
  EXPECT_NE(result.critical_css.find("background: #111"), std::string::npos)
      << "Dark mode styles should be included";
}

TEST(CriticalCssExtractorTest, PrefersColorSchemeLightPreserved) {
  CriticalCssExtractor extractor;
  std::vector<CollectedElement> elements;
  elements.push_back(MakeElement("html", 0, 0));
  elements.push_back(MakeElement("body", 1, 1));

  std::string css =
      "@media (prefers-color-scheme: light) {\n"
      "  body { background: #fff; color: #333; }\n"
      "}";

  CriticalCssResult result =
      extractor.Extract(elements, css, CapabilityMask::Viewport::kMobile);

  EXPECT_TRUE(result.success);
  EXPECT_NE(result.critical_css.find("prefers-color-scheme"), std::string::npos)
      << "prefers-color-scheme: light should be preserved";
}

// ========== Phase 3.5: prefers-reduced-motion preservation ==========

TEST(CriticalCssExtractorTest, PrefersReducedMotionPreserved) {
  CriticalCssExtractor extractor;
  std::vector<CollectedElement> elements;
  elements.push_back(MakeElement("html", 0, 0));
  elements.push_back(MakeElement("body", 1, 1));

  std::string css =
      "@media (prefers-reduced-motion: reduce) {\n"
      "  *, *::before, *::after {\n"
      "    animation-duration: 0.01ms !important;\n"
      "    transition-duration: 0.01ms !important;\n"
      "  }\n"
      "}";

  CriticalCssResult result =
      extractor.Extract(elements, css, CapabilityMask::Viewport::kDesktop);

  EXPECT_TRUE(result.success);
  EXPECT_NE(result.critical_css.find("prefers-reduced-motion"),
            std::string::npos)
      << "prefers-reduced-motion should be preserved in critical CSS";
  EXPECT_NE(result.critical_css.find("animation-duration"), std::string::npos)
      << "Reduced motion styles should be included";
}

// ========== Additional coverage: unclosed braces, selectors, edge cases ==========

TEST(CriticalCssExtractorTest, UnclosedBracesFallthrough) {
  CriticalCssExtractor extractor;
  std::vector<CollectedElement> elements;
  elements.push_back(MakeElement("body", 0, 0));

  // CSS with an unclosed brace: FindBlockEnd should reach end-of-string.
  std::string css = "body { margin: 0;\n .content { padding: 10px; }";

  CriticalCssResult result = extractor.Extract(elements, css);

  // Should not crash; the unclosed brace causes the entire rest of the
  // input to be consumed as one rule's body.
  EXPECT_TRUE(result.success);
  // At least 1 rule should be parsed (body rule with unclosed brace
  // consuming remainder).
  EXPECT_GE(result.total_rules, 1);
}

TEST(CriticalCssExtractorTest, MediaFootprintNotMatchedAsPrint) {
  CriticalCssExtractor extractor;
  std::vector<CollectedElement> elements;
  elements.push_back(MakeElement("body", 0, 0));

  // "footprint" contains "print" but should NOT be matched as @media print
  // because HasWordBoundary requires a word boundary around "print".
  std::string css = "@media footprint { body { display: block; } }";

  CriticalCssResult result = extractor.Extract(elements, css);

  EXPECT_TRUE(result.success);
  // "footprint" is not "print" at a word boundary, so the rule should NOT
  // be excluded as a print rule. It is an unrecognized @media and should
  // be conservatively included.
  EXPECT_NE(result.critical_css.find("@media footprint"), std::string::npos)
      << "@media footprint should not be excluded (not a print rule)";
}

TEST(CriticalCssExtractorTest, MediaBlueprintNotMatchedAsPrint) {
  CriticalCssExtractor extractor;
  std::vector<CollectedElement> elements;
  elements.push_back(MakeElement("body", 0, 0));

  // "blueprint" contains "print" but no word boundary before it.
  std::string css = "@media blueprint { body { display: block; } }";

  CriticalCssResult result = extractor.Extract(elements, css);

  EXPECT_TRUE(result.success);
  EXPECT_NE(result.critical_css.find("@media blueprint"), std::string::npos)
      << "@media blueprint should not be excluded";
}

TEST(CriticalCssExtractorTest, UniversalSelectorStarMatches) {
  CriticalCssExtractor extractor;
  std::vector<CollectedElement> elements;
  elements.push_back(MakeElement("div", 0, 0, "", {"container"}));

  std::string css =
      "* { box-sizing: border-box; }\n"
      "*, *::before, *::after { box-sizing: inherit; }";

  CriticalCssResult result = extractor.Extract(elements, css);

  EXPECT_TRUE(result.success);
  EXPECT_NE(result.critical_css.find("* {"), std::string::npos);
  // The selector starting with * should also be included.
  EXPECT_NE(result.critical_css.find("*, *::before, *::after"),
            std::string::npos);
}

TEST(CriticalCssExtractorTest, ComplexSelectorWithChildCombinator) {
  CriticalCssExtractor extractor;
  std::vector<CollectedElement> elements;
  elements.push_back(MakeElement("div", 0, 0, "", {"foo"}));
  elements.push_back(MakeElement("span", 1, 1, "", {"bar"}));

  // "div > .foo + .bar" — final selector after combinators is ".bar".
  std::string css = "div > .foo + .bar { color: red; }";

  CriticalCssResult result = extractor.Extract(elements, css);

  EXPECT_TRUE(result.success);
  // The extractor uses the last part after combinators to match.
  // ".bar" matches the span with class "bar".
  EXPECT_NE(result.critical_css.find("div > .foo + .bar"), std::string::npos);
}

TEST(CriticalCssExtractorTest, SiblingCombinatorSelector) {
  CriticalCssExtractor extractor;
  std::vector<CollectedElement> elements;
  elements.push_back(MakeElement("p", 0, 0));

  // "h1 ~ p" — final selector is "p", which matches.
  std::string css = "h1 ~ p { margin-top: 10px; }";

  CriticalCssResult result = extractor.Extract(elements, css);

  EXPECT_TRUE(result.success);
  EXPECT_NE(result.critical_css.find("h1 ~ p"), std::string::npos);
}

TEST(CriticalCssExtractorTest, AttributeSelectorSkipped) {
  CriticalCssExtractor extractor;
  std::vector<CollectedElement> elements;
  elements.push_back(MakeElement("input", 0, 0));

  // "input[type=text]" — the attribute selector is not matched by
  // SimpleSelectorMatchesElement (it breaks at '['), but the tag name
  // "input" should still match.
  std::string css = "input[type=text] { border: 1px solid; }";

  CriticalCssResult result = extractor.Extract(elements, css);

  EXPECT_TRUE(result.success);
  // The tag name "input" should be extracted before the '['.
  EXPECT_NE(result.critical_css.find("input[type=text]"), std::string::npos)
      << "input tag should match even with attribute selector";
}

TEST(CriticalCssExtractorTest, PseudoSelectorSkipped) {
  CriticalCssExtractor extractor;
  std::vector<CollectedElement> elements;
  elements.push_back(MakeElement("a", 0, 0));

  // "a:hover" — the pseudo selector is not matched but the tag name "a"
  // should still match.
  std::string css =
      "a:hover { color: blue; }\n"
      "a:visited { color: purple; }";

  CriticalCssResult result = extractor.Extract(elements, css);

  EXPECT_TRUE(result.success);
  // The 'a' tag matches both rules.
  EXPECT_NE(result.critical_css.find("a:hover"), std::string::npos);
  EXPECT_NE(result.critical_css.find("a:visited"), std::string::npos);
}

// A `:where()`/`:is()` wrapper that holds the only DOM-matchable token must be
// evaluated even when an unhandled pseudo (`:hover`, `:focus-visible`, ...)
// precedes it. The parser used to `break` on the leading pseudo before reaching
// the wrapper, so `:hover:where(.foo)` was dropped even when an above-the-fold
// element carried class "foo" — defeating the wrapper-desugaring path's purpose.
TEST(CriticalCssExtractorTest, WhereAfterNonWherePseudoStillEvaluated) {
  CriticalCssExtractor extractor;
  std::vector<CollectedElement> elements;
  elements.push_back(MakeElement("div", 0, 0, "", {"foo"}));

  std::string css = ":hover:where(.foo) { color: red; }";

  CriticalCssResult result = extractor.Extract(elements, css);

  EXPECT_TRUE(result.success);
  EXPECT_EQ(1, result.critical_rules);
  EXPECT_NE(result.critical_css.find(":where(.foo)"), std::string::npos)
      << "rule whose only matchable token is inside :where() after a leading "
         "pseudo must be retained";
}

TEST(CriticalCssExtractorTest, ClassWithPseudoSelector) {
  CriticalCssExtractor extractor;
  std::vector<CollectedElement> elements;
  elements.push_back(MakeElement("button", 0, 0, "", {"btn"}));

  // ".btn:focus" — class ".btn" should be parsed before ':'.
  std::string css = ".btn:focus { outline: 2px solid blue; }";

  CriticalCssResult result = extractor.Extract(elements, css);

  EXPECT_TRUE(result.success);
  EXPECT_NE(result.critical_css.find(".btn:focus"), std::string::npos)
      << ".btn class should match even with :focus pseudo-selector";
}

TEST(CriticalCssExtractorTest, IdWithPseudoSelector) {
  CriticalCssExtractor extractor;
  std::vector<CollectedElement> elements;
  elements.push_back(MakeElement("div", 0, 0, "main"));

  // "#main:first-child" — ID "main" should be parsed before ':'.
  std::string css = "#main:first-child { padding: 20px; }";

  CriticalCssResult result = extractor.Extract(elements, css);

  EXPECT_TRUE(result.success);
  EXPECT_NE(result.critical_css.find("#main:first-child"), std::string::npos);
}

TEST(CriticalCssExtractorTest, EmptySelectorIgnored) {
  CriticalCssExtractor extractor;
  std::vector<CollectedElement> elements;
  elements.push_back(MakeElement("body", 0, 0));

  // A comma-separated list with an empty segment.
  std::string css = "body, , div { margin: 0; }";

  CriticalCssResult result = extractor.Extract(elements, css);

  EXPECT_TRUE(result.success);
  // Should still match because "body" is in elements.
  EXPECT_NE(result.critical_css.find("body,"), std::string::npos);
}

TEST(CriticalCssExtractorTest, StringInCssBodyNotAffectsBlockParsing) {
  CriticalCssExtractor extractor;
  std::vector<CollectedElement> elements;
  elements.push_back(MakeElement("div", 0, 0, "", {"quote"}));

  // A CSS rule containing a string with braces inside it.
  std::string css =
      ".quote { content: \"{ braces }\"; color: red; }\n"
      ".other { padding: 10px; }";

  CriticalCssResult result = extractor.Extract(elements, css);

  EXPECT_TRUE(result.success);
  // The braces inside the string should not affect block parsing.
  EXPECT_NE(result.critical_css.find(".quote {"), std::string::npos);
  // .other should be a separate rule, not swallowed.
  EXPECT_EQ(result.total_rules, 2);
}

TEST(CriticalCssExtractorTest, MediaPrintCaseInsensitive) {
  CriticalCssExtractor extractor;
  std::vector<CollectedElement> elements;
  elements.push_back(MakeElement("body", 0, 0));

  // Various cases of @media print.
  std::string css =
      "@MEDIA PRINT { body { font-size: 12pt; } }\n"
      "@Media Print { body { background: white; } }\n"
      "body { margin: 0; }";

  CriticalCssResult result = extractor.Extract(elements, css);

  EXPECT_TRUE(result.success);
  // All print variants should be excluded.
  EXPECT_EQ(result.critical_css.find("PRINT"), std::string::npos);
  EXPECT_EQ(result.critical_css.find("Print"), std::string::npos);
  // Regular rule should be included.
  EXPECT_NE(result.critical_css.find("body { margin"), std::string::npos);
  EXPECT_EQ(result.critical_rules, 1);
}

TEST(CriticalCssExtractorTest, IncludesImportRule) {
  CriticalCssExtractor extractor;
  std::vector<CollectedElement> elements;
  elements.push_back(MakeElement("body", 0, 0));

  std::string css = "@import url('fonts.css') { }";

  CriticalCssResult result = extractor.Extract(elements, css);

  EXPECT_TRUE(result.success);
  EXPECT_NE(result.critical_css.find("@import"), std::string::npos);
}

TEST(CriticalCssExtractorTest, IncludesCharsetRule) {
  CriticalCssExtractor extractor;
  std::vector<CollectedElement> elements;
  elements.push_back(MakeElement("body", 0, 0));

  std::string css = "@charset \"UTF-8\" { }";

  CriticalCssResult result = extractor.Extract(elements, css);

  EXPECT_TRUE(result.success);
  EXPECT_NE(result.critical_css.find("@charset"), std::string::npos);
}

TEST(CriticalCssExtractorTest, IncludesWebkitKeyframes) {
  CriticalCssExtractor extractor;
  std::vector<CollectedElement> elements;
  elements.push_back(MakeElement("body", 0, 0));

  std::string css =
      "@-webkit-keyframes spin { from { transform: rotate(0); } "
      "to { transform: rotate(360deg); } }";

  CriticalCssResult result = extractor.Extract(elements, css);

  EXPECT_TRUE(result.success);
  EXPECT_NE(result.critical_css.find("@-webkit-keyframes"), std::string::npos);
}

// ========== Coverage: malformed CSS, unrecognized units, attribute selectors ==========

TEST(CriticalCssExtractorTest, MalformedCssNoBrace) {
  CriticalCssExtractor extractor;
  std::vector<CollectedElement> elements;
  elements.push_back(MakeElement("body", 0, 0));

  // CSS with no opening brace at all — ParseCssRules should not crash
  // and should produce 0 rules (no brace found).
  std::string css = "body margin: 0;";

  CriticalCssResult result = extractor.Extract(elements, css);

  EXPECT_TRUE(result.success);
  EXPECT_EQ(result.total_rules, 0);
  EXPECT_TRUE(result.critical_css.empty());
}

TEST(CriticalCssExtractorTest, UnrecognizedLengthUnit) {
  CriticalCssExtractor extractor;
  std::vector<CollectedElement> elements;
  elements.push_back(MakeElement("body", 0, 0));

  // "vw" is not a recognized unit for ParseCssLength (only px, em, rem).
  // The query should be conservatively included for all viewports.
  std::string css = "@media (min-width: 100vw) { body { display: block; } }";

  CriticalCssResult result_mobile =
      extractor.Extract(elements, css, CapabilityMask::Viewport::kMobile);
  CriticalCssResult result_desktop =
      extractor.Extract(elements, css, CapabilityMask::Viewport::kDesktop);

  EXPECT_TRUE(result_mobile.success);
  EXPECT_TRUE(result_desktop.success);
  // Since "vw" is unrecognized, the query should be conservatively included.
  EXPECT_NE(result_mobile.critical_css.find("@media"), std::string::npos)
      << "Unrecognized unit 'vw' should be conservatively included for mobile";
  EXPECT_NE(result_desktop.critical_css.find("@media"), std::string::npos)
      << "Unrecognized unit 'vw' should be conservatively included for desktop";
}

TEST(CriticalCssExtractorTest, SelectorWithAttributeBracket) {
  CriticalCssExtractor extractor;
  std::vector<CollectedElement> elements;
  elements.push_back(MakeElement("input", 0, 0));

  // Attribute selector with quoted value: input[type="text"]
  // The parser should extract "input" before the '['.
  std::string css = "input[type=\"text\"] { border: 1px solid #ccc; }";

  CriticalCssResult result = extractor.Extract(elements, css);

  EXPECT_TRUE(result.success);
  EXPECT_NE(result.critical_css.find("input[type=\"text\"]"), std::string::npos)
      << "Attribute selector with quotes should match via tag name";
}

// =============================================================================
// Additional Coverage: @-moz-keyframes, exclude patterns, include patterns,
// always_include_selectors, depth limit, media prefix stripping, combinators
// =============================================================================

TEST(CriticalCssExtractorTest, IncludesMozKeyframes) {
  CriticalCssExtractor extractor;
  std::vector<CollectedElement> elements;
  elements.push_back(MakeElement("body", 0, 0));

  std::string css =
      "@-moz-keyframes slide { from { left: 0; } to { left: 100px; } }";

  CriticalCssResult result = extractor.Extract(elements, css);

  EXPECT_TRUE(result.success);
  EXPECT_NE(result.critical_css.find("@-moz-keyframes"), std::string::npos)
      << "@-moz-keyframes should always be included";
}

TEST(CriticalCssExtractorTest, ExcludesElementByIdPattern) {
  CriticalCssConfig config;
  config.exclude_id_patterns = {"footer", "below-fold"};
  config.max_elements = 100;
  config.max_depth = 10;
  CriticalCssExtractor extractor(config);

  std::vector<CollectedElement> elements;
  elements.push_back(MakeElement("div", 1, 0, "main-content"));
  elements.push_back(MakeElement("div", 1, 1, "footer-wrapper"));

  std::string css =
      "#main-content { padding: 20px; }\n"
      "#footer-wrapper { margin-top: 50px; }";

  CriticalCssResult result = extractor.Extract(elements, css);

  EXPECT_TRUE(result.success);
  EXPECT_NE(result.critical_css.find("#main-content"), std::string::npos);
  // "footer-wrapper" matches the "footer" exclude ID pattern.
  EXPECT_EQ(result.critical_css.find("#footer-wrapper"), std::string::npos)
      << "Element with ID matching exclude_id_patterns should be excluded";
}

TEST(CriticalCssExtractorTest, ExcludesElementByTagPattern) {
  CriticalCssConfig config;
  config.exclude_tag_patterns = {"footer"};
  config.max_elements = 100;
  config.max_depth = 10;
  CriticalCssExtractor extractor(config);

  std::vector<CollectedElement> elements;
  elements.push_back(MakeElement("header", 1, 0));
  elements.push_back(MakeElement("footer", 1, 1));

  std::string css =
      "header { background: #fff; }\n"
      "footer { padding: 20px; }";

  CriticalCssResult result = extractor.Extract(elements, css);

  EXPECT_TRUE(result.success);
  EXPECT_NE(result.critical_css.find("header"), std::string::npos);
  EXPECT_EQ(result.critical_css.find("footer"), std::string::npos)
      << "Element with tag matching exclude_tag_patterns should be excluded";
}

TEST(CriticalCssExtractorTest, IncludesElementByClassPattern) {
  // Deep element (beyond max_depth) should be included if it matches
  // include_class_patterns.
  CriticalCssConfig config;
  config.max_depth = 2;
  config.max_elements = 0;  // No elements by index
  config.include_class_patterns = {"hero", "banner"};
  CriticalCssExtractor extractor(config);

  std::vector<CollectedElement> elements;
  // This element is at depth 1 (within max_depth) but beyond max_elements.
  // Include via class pattern.
  elements.push_back(MakeElement("div", 1, 100, "", {"hero-section"}));
  // This element is beyond both limits but matches include pattern.
  elements.push_back(MakeElement("div", 1, 101, "", {"banner-top"}));

  std::string css =
      ".hero-section { min-height: 100vh; }\n"
      ".banner-top { background: blue; }";

  CriticalCssResult result = extractor.Extract(elements, css);

  EXPECT_TRUE(result.success);
  EXPECT_NE(result.critical_css.find(".hero-section"), std::string::npos)
      << "Element matching include_class_patterns should be included";
  EXPECT_NE(result.critical_css.find(".banner-top"), std::string::npos)
      << "Element matching include_class_patterns should be included";
}

TEST(CriticalCssExtractorTest, IncludesElementByIdPattern) {
  CriticalCssConfig config;
  config.max_depth = 2;
  config.max_elements = 0;
  config.include_id_patterns = {"hero", "header", "nav"};
  CriticalCssExtractor extractor(config);

  std::vector<CollectedElement> elements;
  // Deep element beyond max_elements but with matching include ID pattern.
  elements.push_back(MakeElement("div", 1, 100, "nav-main"));

  std::string css = "#nav-main { display: flex; }";

  CriticalCssResult result = extractor.Extract(elements, css);

  EXPECT_TRUE(result.success);
  EXPECT_NE(result.critical_css.find("#nav-main"), std::string::npos)
      << "Element matching include_id_patterns should be included";
}

TEST(CriticalCssExtractorTest, IncludesElementByTagPatternOverride) {
  CriticalCssConfig config;
  config.max_depth = 2;
  config.max_elements = 0;
  config.include_tag_patterns = {"header", "nav", "main"};
  CriticalCssExtractor extractor(config);

  std::vector<CollectedElement> elements;
  // Beyond max_elements but matches include_tag_patterns.
  elements.push_back(MakeElement("main", 1, 100));

  std::string css = "main { padding: 20px; }";

  CriticalCssResult result = extractor.Extract(elements, css);

  EXPECT_TRUE(result.success);
  EXPECT_NE(result.critical_css.find("main"), std::string::npos)
      << "Element matching include_tag_patterns should be included";
}

TEST(CriticalCssExtractorTest, AlwaysIncludeSelectorCommaVariant) {
  // Test always_include_selectors matching via comma-separated lists.
  CriticalCssConfig config;
  config.always_include_selectors = {"*", "html", "body", ":root"};
  config.max_elements = 0;
  config.max_depth = 0;
  CriticalCssExtractor extractor(config);

  // No elements that would match via element matching.
  std::vector<CollectedElement> elements;

  // Selector lists containing always_include_selectors:
  // "html, body" contains "html" followed by ","
  std::string css =
      "html, body { margin: 0; }\n"
      "body, div { padding: 0; }";

  CriticalCssResult result = extractor.Extract(elements, css);

  EXPECT_TRUE(result.success);
  // "html," matches the pattern_lower + "," check.
  EXPECT_NE(result.critical_css.find("html, body"), std::string::npos)
      << "Selector list containing always-include pattern should be included";
}

TEST(CriticalCssExtractorTest, DepthLimitExcludesDeepElements) {
  CriticalCssConfig config;
  config.max_depth = 3;
  config.max_elements = 100;
  // Clear include patterns so only depth matters for exclusion.
  config.include_tag_patterns = {};
  config.include_class_patterns = {};
  config.include_id_patterns = {};
  config.exclude_tag_patterns = {};
  config.exclude_class_patterns = {};
  config.exclude_id_patterns = {};
  CriticalCssExtractor extractor(config);

  std::vector<CollectedElement> elements;
  elements.push_back(MakeElement("div", 2, 0, "", {"shallow"}));
  elements.push_back(MakeElement("div", 5, 1, "", {"deep-element"}));

  std::string css =
      ".shallow { color: red; }\n"
      ".deep-element { color: blue; }";

  CriticalCssResult result = extractor.Extract(elements, css);

  EXPECT_TRUE(result.success);
  EXPECT_NE(result.critical_css.find(".shallow"), std::string::npos);
  EXPECT_EQ(result.critical_css.find(".deep-element"), std::string::npos)
      << "Element exceeding max_depth should be excluded";
}

TEST(CriticalCssExtractorTest, MediaAllAndPrefix) {
  CriticalCssExtractor extractor;
  std::vector<CollectedElement> elements;
  elements.push_back(MakeElement("body", 0, 0));

  // "all and" prefix should be stripped, leaving min-width: 1024px.
  std::string css =
      "@media all and (min-width: 1024px) "
      "{ body { display: block; } }";

  CriticalCssResult result_mobile =
      extractor.Extract(elements, css, CapabilityMask::Viewport::kMobile);
  CriticalCssResult result_desktop =
      extractor.Extract(elements, css, CapabilityMask::Viewport::kDesktop);

  EXPECT_EQ(result_mobile.critical_css.find("@media"), std::string::npos)
      << "'all and' prefix 1024px should be excluded for mobile";
  EXPECT_NE(result_desktop.critical_css.find("@media"), std::string::npos)
      << "'all and' prefix 1024px should be included for desktop";
}

TEST(CriticalCssExtractorTest, MaxWidthWithSpaceBeforeColon) {
  CriticalCssExtractor extractor;
  std::vector<CollectedElement> elements;
  elements.push_back(MakeElement("body", 0, 0));

  // "min-width :" / "max-width :" with space before colon.
  std::string css =
      "@media (min-width : 981px) { body { display: block; } }\n"
      "@media (max-width : 319px) { body { display: block; } }";

  CriticalCssResult result_mobile =
      extractor.Extract(elements, css, CapabilityMask::Viewport::kMobile);
  CriticalCssResult result_desktop =
      extractor.Extract(elements, css, CapabilityMask::Viewport::kDesktop);

  EXPECT_EQ(result_mobile.critical_css.find("min-width : 981px"),
            std::string::npos)
      << "min-width : 981px is read and excluded for mobile";
  EXPECT_NE(result_mobile.critical_css.find("max-width : 319px"),
            std::string::npos)
      << "max-width : 319px is read and kept for mobile";
  EXPECT_NE(result_desktop.critical_css.find("min-width : 981px"),
            std::string::npos)
      << "min-width : 981px should be included for desktop";
  EXPECT_NE(result_desktop.critical_css.find("max-width : 319px"),
            std::string::npos)
      << "max-width : 319px should be included for desktop";
}

TEST(CriticalCssExtractorTest, MinWidthWithSpaceBeforeColon) {
  CriticalCssExtractor extractor;
  std::vector<CollectedElement> elements;
  elements.push_back(MakeElement("body", 0, 0));

  // "min-width :" with space before colon.
  std::string css = "@media (min-width : 1024px) { body { display: block; } }";

  CriticalCssResult result_mobile =
      extractor.Extract(elements, css, CapabilityMask::Viewport::kMobile);
  CriticalCssResult result_desktop =
      extractor.Extract(elements, css, CapabilityMask::Viewport::kDesktop);

  EXPECT_EQ(result_mobile.critical_css.find("@media"), std::string::npos)
      << "min-width: 1024px should be excluded for mobile";
  EXPECT_NE(result_desktop.critical_css.find("@media"), std::string::npos)
      << "min-width: 1024px should be included for desktop";
}

TEST(CriticalCssExtractorTest, SelectorNoMatchReturnsEmpty) {
  CriticalCssExtractor extractor;
  std::vector<CollectedElement> elements;
  // Elements that don't match any CSS selector.
  elements.push_back(MakeElement("div", 0, 0, "main", {"wrapper"}));

  // CSS with selectors that don't match the elements.
  std::string css = ".nonexistent { color: red; }";

  CriticalCssResult result = extractor.Extract(elements, css);

  EXPECT_TRUE(result.success);
  EXPECT_EQ(result.critical_rules, 0);
  EXPECT_TRUE(result.critical_css.empty());
}

TEST(CriticalCssExtractorTest, MultipleClassesAllMustMatch) {
  CriticalCssExtractor extractor;
  std::vector<CollectedElement> elements;
  // Element with only one of the two required classes.
  elements.push_back(MakeElement("div", 0, 0, "", {"foo"}));

  // Selector requires both .foo and .bar.
  std::string css = ".foo.bar { color: red; }";

  CriticalCssResult result = extractor.Extract(elements, css);

  EXPECT_TRUE(result.success);
  // Should NOT match because element lacks class "bar".
  EXPECT_EQ(result.critical_css.find(".foo.bar"), std::string::npos)
      << "Selector requiring multiple classes should not match element with "
         "only one";
}

TEST(CriticalCssExtractorTest, MultipleClassesAllPresent) {
  CriticalCssExtractor extractor;
  std::vector<CollectedElement> elements;
  elements.push_back(MakeElement("div", 0, 0, "", {"foo", "bar", "baz"}));

  std::string css = ".foo.bar { color: red; }";

  CriticalCssResult result = extractor.Extract(elements, css);

  EXPECT_TRUE(result.success);
  EXPECT_NE(result.critical_css.find(".foo.bar"), std::string::npos)
      << "Selector should match when all required classes are present";
}

TEST(CriticalCssExtractorTest, TagAndIdCombinedNoMatch) {
  CriticalCssExtractor extractor;
  std::vector<CollectedElement> elements;
  // Element tag is "div" but selector requires "span".
  elements.push_back(MakeElement("div", 0, 0, "main"));

  std::string css = "span#main { color: red; }";

  CriticalCssResult result = extractor.Extract(elements, css);

  EXPECT_TRUE(result.success);
  EXPECT_EQ(result.critical_css.find("span#main"), std::string::npos)
      << "Tag name mismatch should prevent match even if ID matches";
}

TEST(CriticalCssExtractorTest, IdMismatchPreventsMatch) {
  CriticalCssExtractor extractor;
  std::vector<CollectedElement> elements;
  elements.push_back(MakeElement("div", 0, 0, "sidebar"));

  std::string css = "#main { color: red; }";

  CriticalCssResult result = extractor.Extract(elements, css);

  EXPECT_TRUE(result.success);
  EXPECT_EQ(result.critical_css.find("#main"), std::string::npos)
      << "ID mismatch should prevent match";
}

TEST(CriticalCssExtractorTest, CaseInsensitiveTagMatching) {
  CriticalCssExtractor extractor;
  std::vector<CollectedElement> elements;
  elements.push_back(MakeElement("DIV", 0, 0));

  std::string css = "div { margin: 0; }";

  CriticalCssResult result = extractor.Extract(elements, css);

  EXPECT_TRUE(result.success);
  EXPECT_NE(result.critical_css.find("div"), std::string::npos)
      << "Tag matching should be case-insensitive";
}

TEST(CriticalCssExtractorTest, ExcludeClassMultipleClasses) {
  // Element has multiple classes, one of which matches exclude pattern.
  CriticalCssConfig config;
  config.exclude_class_patterns = {"lazy"};
  config.max_elements = 100;
  config.max_depth = 10;
  config.include_tag_patterns = {};
  config.include_class_patterns = {};
  config.include_id_patterns = {};
  CriticalCssExtractor extractor(config);

  std::vector<CollectedElement> elements;
  elements.push_back(
      MakeElement("img", 1, 0, "", {"hero-image", "lazyload-fallback"}));

  std::string css = ".hero-image { width: 100%; }";

  CriticalCssResult result = extractor.Extract(elements, css);

  EXPECT_TRUE(result.success);
  // "lazyload-fallback" contains "lazy" pattern.
  EXPECT_EQ(result.critical_css.find(".hero-image"), std::string::npos)
      << "Element with any class matching exclude pattern should be excluded";
}

TEST(CriticalCssExtractorTest, ContainsPatternCaseInsensitive) {
  // Verify ContainsPattern is case-insensitive.
  CriticalCssConfig config;
  config.include_class_patterns = {"HERO"};
  config.max_depth = 10;
  config.max_elements = 0;
  CriticalCssExtractor extractor(config);

  std::vector<CollectedElement> elements;
  elements.push_back(MakeElement("div", 1, 100, "", {"hero-section"}));

  std::string css = ".hero-section { height: 100vh; }";

  CriticalCssResult result = extractor.Extract(elements, css);

  EXPECT_TRUE(result.success);
  EXPECT_NE(result.critical_css.find(".hero-section"), std::string::npos)
      << "Pattern matching should be case-insensitive";
}

TEST(CriticalCssExtractorTest, EmptyNoElementsNoRules) {
  CriticalCssExtractor extractor;
  std::vector<CollectedElement> elements;

  std::string css = ".something { color: red; }";

  CriticalCssResult result = extractor.Extract(elements, css);

  EXPECT_TRUE(result.success);
  EXPECT_EQ(result.total_rules, 1);
  // No elements to match against, so no critical rules (except always-include).
  EXPECT_EQ(result.critical_rules, 0);
}

TEST(CriticalCssExtractorTest, SingleCharComment) {
  CriticalCssExtractor extractor;
  std::vector<CollectedElement> elements;
  elements.push_back(MakeElement("body", 0, 0));

  // CSS with multiple comments interspersed.
  std::string css =
      "/* comment 1 */\n"
      "/* comment 2 */\n"
      "body { margin: 0; }\n"
      "/* trailing comment */";

  CriticalCssResult result = extractor.Extract(elements, css);

  EXPECT_TRUE(result.success);
  EXPECT_EQ(result.total_rules, 1);
  EXPECT_EQ(result.critical_rules, 1);
}

TEST(CriticalCssExtractorTest, MediaPrintAndScreen) {
  CriticalCssExtractor extractor;
  std::vector<CollectedElement> elements;
  elements.push_back(MakeElement("body", 0, 0));

  // @media print, screen — a list applies if ANY query does, and `screen`
  // does.
  std::string css = "@media print, screen { body { font-size: 14pt; } }";

  CriticalCssResult result = extractor.Extract(elements, css);

  EXPECT_TRUE(result.success);
  EXPECT_NE(result.critical_css.find("@media print, screen"), std::string::npos)
      << "@media print, screen applies on screen";
}

TEST(CriticalCssExtractorTest, SingleQuoteInCssBody) {
  CriticalCssExtractor extractor;
  std::vector<CollectedElement> elements;
  elements.push_back(MakeElement("div", 0, 0, "", {"icon"}));

  // CSS with single-quoted string containing braces.
  std::string css =
      ".icon { content: '{ icon }'; display: inline; }\n"
      ".other { padding: 5px; }";

  CriticalCssResult result = extractor.Extract(elements, css);

  EXPECT_TRUE(result.success);
  EXPECT_NE(result.critical_css.find(".icon"), std::string::npos);
  EXPECT_EQ(result.total_rules, 2);
}

TEST(CriticalCssExtractorTest, EscapedQuoteInString) {
  CriticalCssExtractor extractor;
  std::vector<CollectedElement> elements;
  elements.push_back(MakeElement("div", 0, 0, "", {"escaped"}));

  // CSS with escaped quote inside a string.
  std::string css = R"(.escaped { content: "he said \"hello\" { }"; })";

  CriticalCssResult result = extractor.Extract(elements, css);

  EXPECT_TRUE(result.success);
  EXPECT_GE(result.total_rules, 1);
}

// =============================================================================
// Coverage: double-backslash in CSS string should not confuse block parser.
// "test\\" ends the string (the backslash is escaped, not the quote).
// =============================================================================

TEST(CriticalCssExtractorTest, DoubleBackslashInString) {
  CriticalCssExtractor extractor;
  std::vector<CollectedElement> elements;
  elements.push_back(MakeElement("div", 0, 0, "", {"dbs"}));

  // The CSS contains a value with a double-backslash before the closing quote.
  // The parser must recognize that the quote after \\ is real (not escaped).
  std::string css = R"(.dbs { content: "test\\"; color: red; })";

  CriticalCssResult result = extractor.Extract(elements, css);

  EXPECT_TRUE(result.success);
  EXPECT_GE(result.total_rules, 1);
  // The rule should be extracted correctly despite the \\ in the string.
  EXPECT_NE(result.critical_css.find("color: red"), std::string::npos);
}

// =============================================================================
// Coverage: ch unit in media queries (unrecognized, should be conservatively
// included).
// =============================================================================

TEST(CriticalCssExtractorTest, UnrecognizedChUnitInMedia) {
  CriticalCssExtractor extractor;
  std::vector<CollectedElement> elements;
  elements.push_back(MakeElement("body", 0, 0));

  // "ch" is not a recognized unit for ParseCssLength (only px, em, rem).
  std::string css = "@media (min-width: 80ch) { body { display: block; } }";

  CriticalCssResult result_mobile =
      extractor.Extract(elements, css, CapabilityMask::Viewport::kMobile);
  CriticalCssResult result_desktop =
      extractor.Extract(elements, css, CapabilityMask::Viewport::kDesktop);

  EXPECT_TRUE(result_mobile.success);
  EXPECT_TRUE(result_desktop.success);
  // "ch" is unrecognized → conservatively included for all viewports.
  EXPECT_NE(result_mobile.critical_css.find("@media"), std::string::npos)
      << "Unrecognized unit 'ch' should be conservatively included for mobile";
  EXPECT_NE(result_desktop.critical_css.find("@media"), std::string::npos)
      << "Unrecognized unit 'ch' should be conservatively included for desktop";
}

// =============================================================================
// Coverage: ex unit in media queries (unrecognized, should be conservatively
// included).
// =============================================================================

TEST(CriticalCssExtractorTest, UnrecognizedExUnitInMedia) {
  CriticalCssExtractor extractor;
  std::vector<CollectedElement> elements;
  elements.push_back(MakeElement("body", 0, 0));

  std::string css = "@media (max-width: 50ex) { body { display: block; } }";

  CriticalCssResult result =
      extractor.Extract(elements, css, CapabilityMask::Viewport::kDesktop);

  EXPECT_TRUE(result.success);
  EXPECT_NE(result.critical_css.find("@media"), std::string::npos)
      << "Unrecognized unit 'ex' should be conservatively included";
}

// =============================================================================
// Coverage: Null byte in CSS content (XSS prevention at injection layer).
// The extractor itself should handle null bytes without crashing.
// =============================================================================

TEST(CriticalCssExtractorTest, NullByteInCssContent) {
  CriticalCssExtractor extractor;
  std::vector<CollectedElement> elements;
  elements.push_back(MakeElement("body", 0, 0));

  // CSS with embedded null byte in the body.
  std::string css = "body { margin: 0; }";
  css.insert(css.find("margin"), 1, '\0');

  CriticalCssResult result = extractor.Extract(elements, css);

  EXPECT_TRUE(result.success);
  // The rule should still be extracted (null byte is in the body, not selector).
  EXPECT_GE(result.total_rules, 1);
}

TEST(CriticalCssExtractorTest, NullByteInSelector) {
  CriticalCssExtractor extractor;
  std::vector<CollectedElement> elements;
  elements.push_back(MakeElement("body", 0, 0));

  // CSS with null byte in the selector — should not crash.
  std::string css = std::string("bo") + '\0' + "dy { margin: 0; }";

  CriticalCssResult result = extractor.Extract(elements, css);

  EXPECT_TRUE(result.success);
  // The selector with null byte may or may not match, but must not crash.
}

// =============================================================================
// Coverage: Empty viewport (no elements at all).
// =============================================================================

TEST(CriticalCssExtractorTest, EmptyElementsWithComplexCss) {
  CriticalCssExtractor extractor;
  std::vector<CollectedElement> elements;
  // No elements at all.

  std::string css =
      "div { color: red; }\n"
      "body { margin: 0; }\n"
      "@font-face { font-family: 'Test'; src: url('test.woff2'); }\n"
      "@keyframes fade { from { opacity: 0; } to { opacity: 1; } }\n"
      "@media (min-width: 1024px) { .wide { display: block; } }\n"
      "@media (min-width: 1024px) { body { padding: 1px; } }\n"
      "@media print { body { font-size: 12pt; } }";

  CriticalCssResult result = extractor.Extract(elements, css);

  EXPECT_TRUE(result.success);
  // @font-face and @keyframes should be included regardless of elements.
  EXPECT_NE(result.critical_css.find("@font-face"), std::string::npos)
      << "@font-face should always be included even with no elements";
  EXPECT_NE(result.critical_css.find("@keyframes"), std::string::npos)
      << "@keyframes should always be included even with no elements";
  // @media blocks are filtered by their rules like everything else: the one
  // whose only rule matches no element is left out, the body one is kept.
  EXPECT_EQ(result.critical_css.find(".wide"), std::string::npos)
      << "a @media block none of whose rules the fold needs is left out";
  EXPECT_NE(result.critical_css.find("padding: 1px"), std::string::npos)
      << "a @media block with a rule the fold needs is kept";
  // @media print should be excluded.
  EXPECT_EQ(result.critical_css.find("@media print"), std::string::npos);
  // div and body are in always_include_selectors for body, but div is not.
  // Since there are no elements, div should NOT match (no element matching).
  EXPECT_EQ(result.critical_css.find("div {"), std::string::npos)
      << "div should not be included with no elements";
}

// =============================================================================
// Coverage: Malformed selector starting with special characters.
// =============================================================================

TEST(CriticalCssExtractorTest, MalformedSelectorSpecialChars) {
  CriticalCssExtractor extractor;
  std::vector<CollectedElement> elements;
  elements.push_back(MakeElement("div", 0, 0));

  // Selector starting with non-standard characters.
  std::string css =
      "!!! { color: red; }\n"
      "div { margin: 0; }";

  CriticalCssResult result = extractor.Extract(elements, css);

  EXPECT_TRUE(result.success);
  // The malformed selector should not crash the parser.
  EXPECT_GE(result.total_rules, 2);
  // div should still match.
  EXPECT_NE(result.critical_css.find("div {"), std::string::npos);
}

// =============================================================================
// Coverage: Very large CSS input. Exercises performance without crashing.
// =============================================================================

TEST(CriticalCssExtractorTest, VeryLargeCssInput) {
  CriticalCssExtractor extractor;
  std::vector<CollectedElement> elements;
  elements.push_back(MakeElement("body", 0, 0));
  elements.push_back(MakeElement("div", 1, 1, "", {"container"}));

  // Generate a large CSS string with many rules.
  std::string css;
  for (int i = 0; i < 1000; ++i) {
    css += ".class-" + std::to_string(i) + " { color: red; }\n";
  }
  // Add a matching rule at the end.
  css += ".container { padding: 20px; }\n";
  css += "body { margin: 0; }";

  CriticalCssResult result = extractor.Extract(elements, css);

  EXPECT_TRUE(result.success);
  // Should have parsed all rules.
  EXPECT_EQ(result.total_rules, 1002);
  // body and .container should be included.
  EXPECT_NE(result.critical_css.find("body {"), std::string::npos);
  EXPECT_NE(result.critical_css.find(".container {"), std::string::npos);
  // Most .class-N rules should not be included (no matching elements).
  EXPECT_EQ(result.critical_css.find(".class-500 {"), std::string::npos);
}

// =============================================================================
// Coverage: Media query with "screen" alone (no "and"), should be
// conservatively included for all viewports.
// =============================================================================

TEST(CriticalCssExtractorTest, MediaScreenAloneConservativelyIncluded) {
  CriticalCssExtractor extractor;
  std::vector<CollectedElement> elements;
  elements.push_back(MakeElement("body", 0, 0));

  std::string css = "@media screen { body { background: white; } }";

  CriticalCssResult result_mobile =
      extractor.Extract(elements, css, CapabilityMask::Viewport::kMobile);
  CriticalCssResult result_desktop =
      extractor.Extract(elements, css, CapabilityMask::Viewport::kDesktop);

  EXPECT_TRUE(result_mobile.success);
  EXPECT_TRUE(result_desktop.success);
  // "screen" alone is not parseable as min/max-width, should be conservatively
  // included.
  EXPECT_NE(result_mobile.critical_css.find("@media screen"), std::string::npos)
      << "@media screen should be conservatively included for mobile";
  EXPECT_NE(result_desktop.critical_css.find("@media screen"),
            std::string::npos)
      << "@media screen should be conservatively included for desktop";
}

// =============================================================================
// Coverage: Media query with zero-width value (min-width: 0px).
// =============================================================================

TEST(CriticalCssExtractorTest, MediaZeroWidthMinWidth) {
  CriticalCssExtractor extractor;
  std::vector<CollectedElement> elements;
  elements.push_back(MakeElement("body", 0, 0));

  // min-width: 0px should match all viewports.
  std::string css = "@media (min-width: 0px) { body { display: block; } }";

  CriticalCssResult result_mobile =
      extractor.Extract(elements, css, CapabilityMask::Viewport::kMobile);
  CriticalCssResult result_desktop =
      extractor.Extract(elements, css, CapabilityMask::Viewport::kDesktop);

  EXPECT_NE(result_mobile.critical_css.find("@media"), std::string::npos)
      << "min-width: 0px should match mobile";
  EXPECT_NE(result_desktop.critical_css.find("@media"), std::string::npos)
      << "min-width: 0px should match desktop";
}

// =============================================================================
// Coverage: Media query with max-width: 0px. Should only match nothing (0px
// viewport doesn't exist in practice), effectively excluded for all.
// =============================================================================

TEST(CriticalCssExtractorTest, MediaZeroWidthMaxWidth) {
  CriticalCssExtractor extractor;
  std::vector<CollectedElement> elements;
  elements.push_back(MakeElement("body", 0, 0));

  std::string css = "@media (max-width: 0px) { body { display: none; } }";

  CriticalCssResult result_mobile =
      extractor.Extract(elements, css, CapabilityMask::Viewport::kMobile);
  CriticalCssResult result_tablet =
      extractor.Extract(elements, css, CapabilityMask::Viewport::kTablet);
  CriticalCssResult result_desktop =
      extractor.Extract(elements, css, CapabilityMask::Viewport::kDesktop);

  // Every class's windows start at 0 px, so a 0-0 px range overlaps all.
  EXPECT_NE(result_mobile.critical_css.find("@media"), std::string::npos)
      << "max-width: 0px overlaps mobile (0-980)";
  EXPECT_NE(result_tablet.critical_css.find("@media"), std::string::npos)
      << "max-width: 0px overlaps tablet (0-1480)";
  EXPECT_NE(result_desktop.critical_css.find("@media"), std::string::npos)
      << "max-width: 0px overlaps desktop (0+)";
}

// =============================================================================
// Coverage: Selector with only a pseudo-class (e.g., ":root").
// =============================================================================

TEST(CriticalCssExtractorTest, PseudoClassOnlySelectorRoot) {
  CriticalCssExtractor extractor;
  std::vector<CollectedElement> elements;
  elements.push_back(MakeElement("html", 0, 0));

  std::string css = ":root { --primary: blue; }";

  CriticalCssResult result = extractor.Extract(elements, css);

  EXPECT_TRUE(result.success);
  // :root is in always_include_selectors.
  EXPECT_NE(result.critical_css.find(":root"), std::string::npos)
      << ":root should always be included";
}

// =============================================================================
// Coverage: always_include_selectors comma variant — ", body" (with space).
// =============================================================================

TEST(CriticalCssExtractorTest, AlwaysIncludeSelectorCommaSpaceVariant) {
  CriticalCssConfig config;
  config.always_include_selectors = {"*", "html", "body", ":root"};
  config.max_elements = 0;
  config.max_depth = 0;
  CriticalCssExtractor extractor(config);

  std::vector<CollectedElement> elements;

  // Selector with ", body" pattern.
  std::string css = "div, body { margin: 0; }";

  CriticalCssResult result = extractor.Extract(elements, css);

  EXPECT_TRUE(result.success);
  // Should be included because "body" appears after ", ".
  EXPECT_NE(result.critical_css.find("div, body"), std::string::npos)
      << "Selector list with ', body' should match always_include_selectors";
}

// =============================================================================
// Coverage: always_include_selectors comma variant — ",body" (no space).
// =============================================================================

TEST(CriticalCssExtractorTest, AlwaysIncludeSelectorCommaNoSpaceVariant) {
  CriticalCssConfig config;
  config.always_include_selectors = {"*", "html", "body", ":root"};
  config.max_elements = 0;
  config.max_depth = 0;
  CriticalCssExtractor extractor(config);

  std::vector<CollectedElement> elements;

  // Selector with ",body" (no space after comma).
  std::string css = "div,body { margin: 0; }";

  CriticalCssResult result = extractor.Extract(elements, css);

  EXPECT_TRUE(result.success);
  EXPECT_NE(result.critical_css.find("div,body"), std::string::npos)
      << "Selector list with ',body' should match always_include_selectors";
}

// =============================================================================
// Coverage: Element with empty ID (should not match ID patterns).
// =============================================================================

TEST(CriticalCssExtractorTest, ElementWithEmptyIdSkipsIdPatternCheck) {
  CriticalCssConfig config;
  config.exclude_id_patterns = {"footer"};
  config.max_elements = 100;
  config.max_depth = 10;
  CriticalCssExtractor extractor(config);

  std::vector<CollectedElement> elements;
  // Element with empty ID should not trigger ID pattern matching.
  elements.push_back(MakeElement("div", 1, 0));

  std::string css = "div { color: red; }";

  CriticalCssResult result = extractor.Extract(elements, css);

  EXPECT_TRUE(result.success);
  EXPECT_NE(result.critical_css.find("div"), std::string::npos)
      << "Element with empty ID should not be excluded by ID patterns";
}

// =============================================================================
// Coverage: CSS with only comments (no rules).
// =============================================================================

TEST(CriticalCssExtractorTest, OnlyComments) {
  CriticalCssExtractor extractor;
  std::vector<CollectedElement> elements;
  elements.push_back(MakeElement("body", 0, 0));

  std::string css = "/* comment 1 */ /* comment 2 */";

  CriticalCssResult result = extractor.Extract(elements, css);

  EXPECT_TRUE(result.success);
  EXPECT_EQ(result.total_rules, 0);
  EXPECT_TRUE(result.critical_css.empty());
}

// =============================================================================
// Coverage: CSS with unclosed comment.
// =============================================================================

TEST(CriticalCssExtractorTest, UnclosedComment) {
  CriticalCssExtractor extractor;
  std::vector<CollectedElement> elements;
  elements.push_back(MakeElement("body", 0, 0));

  std::string css = "body { margin: 0; } /* unclosed comment";

  CriticalCssResult result = extractor.Extract(elements, css);

  EXPECT_TRUE(result.success);
  // The first rule should be parsed correctly.
  EXPECT_GE(result.total_rules, 1);
  EXPECT_NE(result.critical_css.find("body"), std::string::npos);
}

TEST(CriticalCssExtractorTest, UnclosedCommentDoesNotSilentlyDropRules) {
  CriticalCssExtractor extractor;
  std::vector<CollectedElement> elements;
  elements.push_back(MakeElement("body", 0, 0));
  elements.push_back(MakeElement("h1", 0, 0));

  // Rule before the unterminated comment should be preserved.
  // Rule after should NOT appear (comment consumes it via npos).
  std::string css = "body { margin: 0; } /* unterminated h1 { color: red; }";

  CriticalCssResult result = extractor.Extract(elements, css);

  EXPECT_TRUE(result.success);
  EXPECT_NE(result.critical_css.find("body"), std::string::npos);
  // h1 rule is inside the unterminated comment — must NOT be extracted.
  EXPECT_EQ(result.critical_css.find("h1"), std::string::npos);
}

// =============================================================================
// Coverage: Element deep in DOM exceeding both max_elements AND max_depth,
// but matching include pattern — should still be included.
// =============================================================================

TEST(CriticalCssExtractorTest, DeepElementIncludedByPattern) {
  CriticalCssConfig config;
  config.max_depth = 3;
  config.max_elements = 2;
  config.include_class_patterns = {"hero"};
  CriticalCssExtractor extractor(config);

  std::vector<CollectedElement> elements;
  // Beyond max_elements AND within max_depth — included by index/depth OR pattern.
  elements.push_back(MakeElement("div", 1, 0, "", {"normal"}));
  elements.push_back(MakeElement("div", 1, 1, "", {"normal2"}));
  // Beyond max_elements (index=100) but within max_depth (depth=2) — included.
  elements.push_back(MakeElement("div", 2, 100, "", {"hero-banner"}));

  std::string css =
      ".normal { color: red; }\n"
      ".normal2 { color: blue; }\n"
      ".hero-banner { min-height: 100vh; }";

  CriticalCssResult result = extractor.Extract(elements, css);

  EXPECT_TRUE(result.success);
  // All three should be included (first two by index, third by pattern).
  EXPECT_NE(result.critical_css.find(".normal {"), std::string::npos);
  EXPECT_NE(result.critical_css.find(".hero-banner"), std::string::npos)
      << "Deep element matching include_class_patterns should be included";
}

// =============================================================================
// Coverage: SimpleSelectorMatchesElement with empty selector.
// =============================================================================

TEST(CriticalCssExtractorTest, EmptyIndividualSelectorInList) {
  CriticalCssExtractor extractor;
  std::vector<CollectedElement> elements;
  elements.push_back(MakeElement("span", 0, 0));

  // Multiple commas with empty segments.
  std::string css = ",,span,, { color: red; }";

  CriticalCssResult result = extractor.Extract(elements, css);

  EXPECT_TRUE(result.success);
  // "span" in the selector list should still match.
  EXPECT_NE(result.critical_css.find("span"), std::string::npos)
      << "span should match even with empty selector segments";
}

// =============================================================================
// Coverage: Media query with min-width exactly at viewport boundary.
// =============================================================================

TEST(CriticalCssExtractorTest, MediaQueryAtExactBoundary) {
  CriticalCssExtractor extractor;
  std::vector<CollectedElement> elements;
  elements.push_back(MakeElement("body", 0, 0));

  // Mobile windows are 0-980 px (RetentionWidthRange).
  // min-width: 981px should exclude mobile, include tablet (0-1480);
  // min-width: 980px can still apply on mobile.
  std::string css =
      "@media (min-width: 981px) { body { display: block; } }\n"
      "@media (min-width: 980px) { body { display: block; } }";

  CriticalCssResult result_mobile =
      extractor.Extract(elements, css, CapabilityMask::Viewport::kMobile);
  CriticalCssResult result_tablet =
      extractor.Extract(elements, css, CapabilityMask::Viewport::kTablet);

  EXPECT_EQ(result_mobile.critical_css.find("min-width: 981px"),
            std::string::npos)
      << "min-width: 981px should exclude mobile (max 980)";
  EXPECT_NE(result_mobile.critical_css.find("min-width: 980px"),
            std::string::npos)
      << "min-width: 980px should include mobile (max 980)";
  EXPECT_NE(result_tablet.critical_css.find("min-width: 981px"),
            std::string::npos)
      << "min-width: 981px should include tablet (up to 1480)";
}

// =============================================================================
// Coverage: Media query with max-width at exact desktop boundary.
// =============================================================================

TEST(CriticalCssExtractorTest, MediaQueryMaxWidthAtDesktopBoundary) {
  CriticalCssExtractor extractor;
  std::vector<CollectedElement> elements;
  elements.push_back(MakeElement("body", 0, 0));

  // A desktop User-Agent can be any width at all, so every
  // max-width block stays.
  std::string css =
      "@media (max-width: 1023px) { body { display: block; } }\n"
      "@media (max-width: 320px) { body { display: block; } }\n"
      "@media (max-width: 319px) { body { display: block; } }";

  CriticalCssResult result_tablet =
      extractor.Extract(elements, css, CapabilityMask::Viewport::kTablet);
  CriticalCssResult result_desktop =
      extractor.Extract(elements, css, CapabilityMask::Viewport::kDesktop);

  EXPECT_NE(result_tablet.critical_css.find("max-width: 1023px"),
            std::string::npos)
      << "max-width: 1023px should include tablet";
  EXPECT_NE(result_desktop.critical_css.find("max-width: 1023px"),
            std::string::npos)
      << "max-width: 1023px should include desktop (a narrow window)";
  EXPECT_NE(result_desktop.critical_css.find("max-width: 320px"),
            std::string::npos)
      << "max-width: 320px should include desktop";
  EXPECT_NE(result_desktop.critical_css.find("max-width: 319px"),
            std::string::npos)
      << "max-width: 319px should include desktop (zoom, narrow window)";
}

// =============================================================================
// Coverage: Media query with fractional rem value.
// =============================================================================

TEST(CriticalCssExtractorTest, HandlesFractionalRemInMedia) {
  CriticalCssExtractor extractor;
  std::vector<CollectedElement> elements;
  elements.push_back(MakeElement("body", 0, 0));

  // 61.3rem * 16 = 980.8px: a whole-pixel window must be at least 981, one
  // past the widest phone window (980) — include tablet, exclude mobile.
  std::string css = "@media (min-width: 61.3rem) { body { display: block; } }";

  CriticalCssResult result_mobile =
      extractor.Extract(elements, css, CapabilityMask::Viewport::kMobile);
  CriticalCssResult result_tablet =
      extractor.Extract(elements, css, CapabilityMask::Viewport::kTablet);

  EXPECT_EQ(result_mobile.critical_css.find("@media"), std::string::npos)
      << "61.3rem (980.8px) should exclude mobile (0-980)";
  EXPECT_NE(result_tablet.critical_css.find("@media"), std::string::npos)
      << "61.3rem (980.8px) should include tablet (0-1480)";
}

// =============================================================================
// Coverage: Selector with tag name containing hyphens (custom elements).
// =============================================================================

TEST(CriticalCssExtractorTest, CustomElementTagWithHyphens) {
  CriticalCssExtractor extractor;
  std::vector<CollectedElement> elements;
  elements.push_back(MakeElement("my-component", 0, 0));

  std::string css = "my-component { display: block; }";

  CriticalCssResult result = extractor.Extract(elements, css);

  EXPECT_TRUE(result.success);
  EXPECT_NE(result.critical_css.find("my-component"), std::string::npos)
      << "Custom element with hyphens should match";
}

// =============================================================================
// Coverage: Selector with underscore in class name.
// =============================================================================

TEST(CriticalCssExtractorTest, UnderscoreInClassName) {
  CriticalCssExtractor extractor;
  std::vector<CollectedElement> elements;
  elements.push_back(MakeElement("div", 0, 0, "", {"my_class_name"}));

  std::string css = ".my_class_name { padding: 10px; }";

  CriticalCssResult result = extractor.Extract(elements, css);

  EXPECT_TRUE(result.success);
  EXPECT_NE(result.critical_css.find(".my_class_name"), std::string::npos)
      << "Class with underscores should match";
}

// =============================================================================
// Coverage: @media print variants. Only the ones no screen can match are
// excluded; the word "print" alone decides nothing.
// =============================================================================

TEST(CriticalCssExtractorTest, MediaPrintWithCommaAndOther) {
  CriticalCssExtractor extractor;
  std::vector<CollectedElement> elements;
  elements.push_back(MakeElement("body", 0, 0));

  std::string css = "@media screen, print { body { font-size: 14pt; } }";

  CriticalCssResult result = extractor.Extract(elements, css);

  EXPECT_TRUE(result.success);
  EXPECT_NE(result.critical_css.find("@media screen, print"), std::string::npos)
      << "@media screen, print applies on screen";
}

TEST(CriticalCssExtractorTest, MediaPrintVariantsExcludedOnlyWhenPrintOnly) {
  std::vector<CollectedElement> elements;
  elements.push_back(MakeElement("body", 0, 0));
  CriticalCssExtractor extractor;

  struct Case {
    const char* media;
    bool kept;
  };
  const Case kCases[] = {
      {"@media print", false},
      {"@media only print", false},
      {"@media PRINT", false},
      {"@media print and (min-width: 1px)", false},
      {"@media print, print", false},
      {"@media not screen", false},
      {"@media not print", true},
      {"@media not print and (min-width: 1px)", true},
      {"@media screen, print", true},
      {"@media print, (min-width: 1px)", true},
      {"@media not all and (print)", true},  // `(print)` is no feature
  };
  for (auto viewport :
       {CapabilityMask::Viewport::kMobile, CapabilityMask::Viewport::kTablet,
        CapabilityMask::Viewport::kDesktop}) {
    for (const Case& c : kCases) {
      std::string css = std::string(c.media) + " { body { --marker: 1; } }";
      CriticalCssResult result = extractor.Extract(elements, css, viewport);
      ASSERT_TRUE(result.success);
      EXPECT_EQ(result.critical_css.find("--marker") != std::string::npos,
                c.kept)
          << c.media << " viewport " << static_cast<int>(viewport);
    }
  }
}

// =============================================================================
// Coverage: is_critical logic — element at index < max_elements but
// depth > max_depth. The OR logic means it should still be included
// (element_index < max_elements OR depth <= max_depth OR IsElementIncluded).
// =============================================================================

TEST(CriticalCssExtractorTest, ElementCriticalByIndexDespiteDeepDepth) {
  CriticalCssConfig config;
  config.max_depth = 2;
  config.max_elements = 10;
  // Clear include/exclude patterns.
  config.include_tag_patterns = {};
  config.include_class_patterns = {};
  config.include_id_patterns = {};
  config.exclude_tag_patterns = {};
  config.exclude_class_patterns = {};
  config.exclude_id_patterns = {};
  CriticalCssExtractor extractor(config);

  std::vector<CollectedElement> elements;
  // Index 0 (< max_elements=10), depth 5 (> max_depth=2).
  // But depth > max_depth triggers IsElementExcluded → returns true.
  // So this element IS excluded due to depth.
  elements.push_back(MakeElement("div", 5, 0, "", {"deep-but-first"}));

  std::string css = ".deep-but-first { color: red; }";

  CriticalCssResult result = extractor.Extract(elements, css);

  EXPECT_TRUE(result.success);
  // Element at depth 5 > max_depth 2 → IsElementExcluded returns true.
  EXPECT_EQ(result.critical_css.find(".deep-but-first"), std::string::npos)
      << "Element exceeding max_depth should be excluded even if index < "
         "max_elements";
}

TEST(CriticalCssExtractorTest, ElementBeyondMaxButIncludedByPattern) {
  CriticalCssConfig config;
  config.max_elements = 2;
  config.max_depth = 10;
  config.include_class_patterns = {"hero"};
  CriticalCssExtractor extractor(config);

  std::vector<CollectedElement> elements;
  elements.push_back(MakeElement("div", 1, 0, "", {"first"}));
  elements.push_back(MakeElement("div", 1, 1, "", {"second"}));
  // Index 5 > max_elements(2), but "hero-banner" matches include pattern.
  elements.push_back(MakeElement("section", 1, 5, "", {"hero-banner"}));

  std::string css =
      ".first { color: red; }\n"
      ".second { color: blue; }\n"
      ".hero-banner { min-height: 100vh; }";

  CriticalCssResult result = extractor.Extract(elements, css);

  EXPECT_TRUE(result.success);
  EXPECT_NE(result.critical_css.find(".first {"), std::string::npos);
  EXPECT_NE(result.critical_css.find(".second {"), std::string::npos);
  EXPECT_NE(result.critical_css.find(".hero-banner {"), std::string::npos)
      << "Element beyond max_elements but matching include pattern should be "
         "included";
}

TEST(CriticalCssExtractorTest, ElementBeyondMaxNotIncludedWithoutPattern) {
  CriticalCssConfig config;
  config.max_elements = 2;
  config.max_depth = 10;
  CriticalCssExtractor extractor(config);

  std::vector<CollectedElement> elements;
  elements.push_back(MakeElement("div", 1, 0, "", {"first"}));
  elements.push_back(MakeElement("div", 1, 1, "", {"second"}));
  // Index 5 > max_elements(2) and not matching any include pattern.
  elements.push_back(MakeElement("section", 1, 5, "", {"footer"}));

  std::string css =
      ".first { color: red; }\n"
      ".second { color: blue; }\n"
      ".footer { margin-top: 40px; }";

  CriticalCssResult result = extractor.Extract(elements, css);

  EXPECT_TRUE(result.success);
  EXPECT_NE(result.critical_css.find(".first {"), std::string::npos);
  // Element at index 5 should NOT be critical (beyond max and no include).
  EXPECT_EQ(result.critical_css.find(".footer {"), std::string::npos)
      << "Element beyond max_elements without include pattern should be "
         "excluded";
}

// ========== @layer / @supports container at-rule tests ==========

TEST(CriticalCssExtractorTest, LayerWithMatchingInnerRules) {
  CriticalCssExtractor extractor;
  std::vector<CollectedElement> elements;
  elements.push_back(MakeElement("body", 0, 0));
  elements.push_back(MakeElement("div", 1, 1, "", {"btn"}));

  std::string css =
      "@layer utilities {\n"
      "  .btn { color: red; }\n"
      "  .hidden { display: none; }\n"
      "}";

  CriticalCssResult result = extractor.Extract(elements, css);

  EXPECT_TRUE(result.success);
  EXPECT_NE(result.critical_css.find("@layer utilities"), std::string::npos)
      << "@layer wrapper should be present";
  EXPECT_NE(result.critical_css.find(".btn {"), std::string::npos)
      << "Matching inner rule should be included";
  EXPECT_EQ(result.critical_css.find(".hidden {"), std::string::npos)
      << "Non-matching inner rule should be excluded";
}

TEST(CriticalCssExtractorTest, LayerWithNoMatchingInnerRules) {
  CriticalCssExtractor extractor;
  std::vector<CollectedElement> elements;
  elements.push_back(MakeElement("body", 0, 0));

  std::string css =
      "@layer utilities {\n"
      "  .hidden { display: none; }\n"
      "  .invisible { visibility: hidden; }\n"
      "}";

  CriticalCssResult result = extractor.Extract(elements, css);

  EXPECT_TRUE(result.success);
  EXPECT_EQ(result.critical_css.find("@layer"), std::string::npos)
      << "Empty @layer block (no surviving inner rules) should be omitted";
}

TEST(CriticalCssExtractorTest, NestedLayerAndSupports) {
  CriticalCssExtractor extractor;
  std::vector<CollectedElement> elements;
  elements.push_back(MakeElement("body", 0, 0));
  elements.push_back(MakeElement("div", 1, 1, "", {"grid"}));

  // Tailwind v4 pattern: @layer { @supports { selector { ... } } }
  std::string css =
      "@layer utilities {\n"
      "  @supports (display: grid) {\n"
      "    .grid { display: grid; }\n"
      "  }\n"
      "}";

  CriticalCssResult result = extractor.Extract(elements, css);

  EXPECT_TRUE(result.success);
  EXPECT_NE(result.critical_css.find("@layer utilities"), std::string::npos)
      << "Outer @layer should be present";
  EXPECT_NE(result.critical_css.find("@supports (display: grid)"),
            std::string::npos)
      << "Inner @supports should be present";
  EXPECT_NE(result.critical_css.find(".grid {"), std::string::npos)
      << "Matching inner rule should be included";
}

TEST(CriticalCssExtractorTest, MultipleLayerBlocks) {
  CriticalCssExtractor extractor;
  std::vector<CollectedElement> elements;
  elements.push_back(MakeElement("html", 0, 0));
  elements.push_back(MakeElement("body", 1, 1));
  elements.push_back(MakeElement("nav", 2, 2));

  std::string css =
      "@layer theme {\n"
      "  :root { --color-primary: blue; }\n"
      "}\n"
      "@layer base {\n"
      "  body { margin: 0; }\n"
      "}\n"
      "@layer utilities {\n"
      "  .hidden { display: none; }\n"
      "}";

  CriticalCssResult result = extractor.Extract(elements, css);

  EXPECT_TRUE(result.success);
  EXPECT_NE(result.critical_css.find("@layer theme"), std::string::npos)
      << "@layer theme should be present (contains :root)";
  EXPECT_NE(result.critical_css.find("@layer base"), std::string::npos)
      << "@layer base should be present (contains body)";
  EXPECT_EQ(result.critical_css.find("@layer utilities"), std::string::npos)
      << "@layer utilities should be omitted (no matching rules)";
}

// The two layer-order cases. The combined sheet is the
// inline <style> bodies first, then the fetched external sheets
// (Worker::BuildCombinedCss); a block extracted from it mentions its layers in
// THAT order. Both blocks below name a layer, so the serve path keeps them
// after the page's sheets (CriticalCssNamesCascadeLayer), where the sheets'
// own mentions fix the layer order.
TEST(CriticalCssExtractorTest,
     LayeredBlocksNameALayerSoTheyKeepTheOldPlacement) {
  std::vector<CollectedElement> elements;
  elements.push_back(MakeElement("body", 0, 0));
  elements.push_back(MakeElement("h1", 1, 1, "", {"hero"}));
  CriticalCssExtractor extractor;

  // Case A: the page is <link href=app.css> then <style>@layer theme{...}.
  // app.css orders reset before theme; the combined sheet puts the inline
  // style first, so the block mentions theme before reset.
  const std::string case_a =
      "@layer theme{.hero{padding:0}}\n"
      "@layer reset{h1{color:black}}\n"
      "@layer theme{h1{color:navy}}\n";
  CriticalCssResult a = extractor.Extract(elements, case_a);
  ASSERT_TRUE(a.success);
  EXPECT_LT(a.critical_css.find("@layer theme"),
            a.critical_css.find("@layer reset"))
      << "the block's layer order is the combined order: " << a.critical_css;
  EXPECT_TRUE(CriticalCssNamesCascadeLayer(a.critical_css));

  // Case B: a cross-origin sheet declaring @layer lib precedes app.css; it is
  // never in the combined sheet, so the block only knows about `app`.
  const std::string case_b = "@layer app{h1{color:navy}}\n";
  CriticalCssResult b = extractor.Extract(elements, case_b);
  ASSERT_TRUE(b.success);
  EXPECT_NE(b.critical_css.find("@layer app"), std::string::npos);
  EXPECT_TRUE(CriticalCssNamesCascadeLayer(b.critical_css));

  // A layer with nothing critical is dropped, not kept as a placeholder: an
  // unlayered block mentions no layer and may go before the sheets.
  CriticalCssResult c = extractor.Extract(
      elements, "@layer unused{.nope{color:red}}\nh1{color:navy}\n");
  ASSERT_TRUE(c.success);
  EXPECT_FALSE(CriticalCssNamesCascadeLayer(c.critical_css)) << c.critical_css;
}

TEST(CriticalCssExtractorTest, FontFaceInsideLayer) {
  CriticalCssExtractor extractor;
  std::vector<CollectedElement> elements;
  elements.push_back(MakeElement("body", 0, 0));

  std::string css =
      "@layer base {\n"
      "  @font-face { font-family: 'Inter'; src: url('inter.woff2'); }\n"
      "  .hidden { display: none; }\n"
      "}";

  CriticalCssResult result = extractor.Extract(elements, css);

  EXPECT_TRUE(result.success);
  EXPECT_NE(result.critical_css.find("@layer base"), std::string::npos)
      << "@layer should be present (contains @font-face)";
  EXPECT_NE(result.critical_css.find("@font-face"), std::string::npos)
      << "@font-face inside @layer should be always included";
  EXPECT_EQ(result.critical_css.find(".hidden {"), std::string::npos)
      << "Non-matching rule inside @layer should be excluded";
}

TEST(CriticalCssExtractorTest, MediaPrintInsideLayer) {
  CriticalCssExtractor extractor;
  std::vector<CollectedElement> elements;
  elements.push_back(MakeElement("body", 0, 0));

  std::string css =
      "@layer base {\n"
      "  @media print { body { font-size: 12pt; } }\n"
      "  body { margin: 0; }\n"
      "}";

  CriticalCssResult result = extractor.Extract(elements, css);

  EXPECT_TRUE(result.success);
  EXPECT_NE(result.critical_css.find("@layer base"), std::string::npos)
      << "@layer should be present (contains body)";
  EXPECT_EQ(result.critical_css.find("@media print"), std::string::npos)
      << "@media print inside @layer should be excluded";
  EXPECT_NE(result.critical_css.find("body {"), std::string::npos)
      << "body rule inside @layer should be included";
}

TEST(CriticalCssExtractorTest, RecursionDepthBounded) {
  CriticalCssExtractor extractor;
  std::vector<CollectedElement> elements;
  elements.push_back(MakeElement("body", 0, 0));

  // Create deeply nested @layer blocks (7 levels deep, beyond limit of 5)
  std::string css =
      "@layer a { @layer b { @layer c { @layer d { @layer e { @layer f {"
      " body { color: red; }"
      " } } } } } }";

  CriticalCssResult result = extractor.Extract(elements, css);

  EXPECT_TRUE(result.success);
  // At depth 5, recursion stops — innermost layers are dropped
  EXPECT_EQ(result.critical_css.find("color: red"), std::string::npos)
      << "Rules beyond recursion depth should be dropped";
}

TEST(CriticalCssExtractorTest, LayerWithViewportMedia) {
  CriticalCssExtractor extractor;
  std::vector<CollectedElement> elements;
  elements.push_back(MakeElement("body", 0, 0));
  elements.push_back(MakeElement("div", 1, 1, "", {"container"}));

  std::string css =
      "@layer utilities {\n"
      "  .container { max-width: 100%; }\n"
      "  @media (min-width: 1024px) { .container { max-width: 1200px; } }\n"
      "}";

  CriticalCssResult result_mobile =
      extractor.Extract(elements, css, CapabilityMask::Viewport::kMobile);
  CriticalCssResult result_desktop =
      extractor.Extract(elements, css, CapabilityMask::Viewport::kDesktop);

  // Both viewports should include .container
  EXPECT_NE(result_mobile.critical_css.find("max-width: 100%"),
            std::string::npos);
  EXPECT_NE(result_desktop.critical_css.find("max-width: 100%"),
            std::string::npos);

  // With no proven layer order (none is passed here), CSS that names a
  // cascade layer gets its block AFTER the sheets, where a dropped override
  // that applies to some window (a phone zoomed out past 1024 px) would lose
  // to the kept base rule for good; so every block that can apply at any
  // width is kept, on every variant.
  EXPECT_NE(result_mobile.critical_css.find("max-width: 1200px"),
            std::string::npos)
      << "a layered page keeps the 1024px+ block for mobile too";
  EXPECT_NE(result_desktop.critical_css.find("max-width: 1200px"),
            std::string::npos)
      << "Desktop-only @media inside @layer should be included for desktop";
}

TEST(CriticalCssExtractorTest, StandaloneSupportsBlock) {
  CriticalCssExtractor extractor;
  std::vector<CollectedElement> elements;
  elements.push_back(MakeElement("div", 0, 0));

  std::string css =
      "@supports (display: grid) {\n"
      "  div { display: grid; }\n"
      "  .unknown-class { color: blue; }\n"
      "}\n";

  auto result = extractor.Extract(elements, css);
  EXPECT_TRUE(result.success);
  // div matches, .unknown-class does not → only div survives
  EXPECT_NE(result.critical_css.find("@supports (display: grid)"),
            std::string::npos);
  EXPECT_NE(result.critical_css.find("display: grid"), std::string::npos);
  EXPECT_EQ(result.critical_css.find("unknown-class"), std::string::npos);
}

TEST(CriticalCssExtractorTest, MinifiedSupportsBoundaryKeepsAboveFoldRules) {
  CriticalCssExtractor extractor;
  std::vector<CollectedElement> elements;
  elements.push_back(MakeElement("div", 0, 0));

  const std::string css =
      "@supports(display:grid){div{display:grid}.unknown-class{color:blue}}";

  auto result = extractor.Extract(elements, css);
  EXPECT_TRUE(result.success);
  EXPECT_NE(result.critical_css.find("@supports(display:grid)"),
            std::string::npos);
  EXPECT_NE(result.critical_css.find("display:grid"), std::string::npos);
  EXPECT_EQ(result.critical_css.find("unknown-class"), std::string::npos);
}

TEST(CriticalCssExtractorTest, MinifiedLayerBoundaryKeepsAboveFoldRules) {
  CriticalCssExtractor extractor;
  std::vector<CollectedElement> elements;
  elements.push_back(MakeElement("div", 0, 0));

  const std::string css = "@layer(x){div{margin:0}.unknown-class{color:blue}}";

  auto result = extractor.Extract(elements, css);
  EXPECT_TRUE(result.success);
  EXPECT_NE(result.critical_css.find("@layer(x)"), std::string::npos);
  EXPECT_NE(result.critical_css.find("margin:0"), std::string::npos);
  EXPECT_EQ(result.critical_css.find("unknown-class"), std::string::npos);
}

TEST(CriticalCssExtractorTest, SpacedSupportsFormUnchanged) {
  CriticalCssExtractor extractor;
  std::vector<CollectedElement> elements;
  elements.push_back(MakeElement("div", 0, 0));

  // Spaced form still filters its inner rules.
  auto spaced = extractor.Extract(
      elements,
      "@supports (display:grid){div{display:grid}.unused{color:red}}");
  EXPECT_TRUE(spaced.success);
  EXPECT_NE(spaced.critical_css.find("display:grid"), std::string::npos);
  EXPECT_EQ(spaced.critical_css.find("unused"), std::string::npos);
}

TEST(CriticalCssExtractorTest, MinifiedSupportsNestedInLayerAndMedia) {
  CriticalCssExtractor extractor;
  std::vector<CollectedElement> elements;
  elements.push_back(MakeElement("div", 0, 0));

  const std::string css =
      "@layer a,b;@layer a{@supports(display:flex){div{probe-a:1}"
      ".nope{probe-b:2}}}@media(min-width:0){@supports(display:grid){"
      "div{probe-c:3}.nope{probe-d:4}}}";

  auto result = extractor.Extract(elements, css);
  EXPECT_TRUE(result.success);
  EXPECT_NE(result.critical_css.find("@layer a,b;"), std::string::npos);
  EXPECT_NE(result.critical_css.find("probe-a"), std::string::npos);
  EXPECT_EQ(result.critical_css.find("probe-b"), std::string::npos);
  EXPECT_NE(result.critical_css.find("probe-c"), std::string::npos);
}

TEST(CriticalCssExtractorTest, AnonymousLayerBlock) {
  CriticalCssExtractor extractor;
  std::vector<CollectedElement> elements;
  elements.push_back(MakeElement("body", 0, 0));

  std::string css =
      "@layer {\n"
      "  body { margin: 0; }\n"
      "}\n";

  // The block goes first (proven order): the anonymous layer is kept.
  CascadeLayerOrder proven;
  proven.proven = true;
  proven.reason.clear();
  auto result =
      extractor.Extract(elements, css, CapabilityMask::Viewport::kDesktop,
                        /*force_include=*/nullptr, &proven);
  EXPECT_TRUE(result.success);
  EXPECT_NE(result.critical_css.find("@layer"), std::string::npos);
  EXPECT_NE(result.critical_css.find("margin: 0"), std::string::npos);
  EXPECT_FALSE(result.anonymous_layers_dropped);

  // No order: the block goes after the sheets, where an anonymous layer wins
  // every normal declaration, so it carries none.
  result = extractor.Extract(elements, css);
  EXPECT_TRUE(result.success);
  EXPECT_TRUE(result.anonymous_layers_dropped);
  EXPECT_EQ(result.critical_css.find("margin: 0"), std::string::npos)
      << result.critical_css;
}

TEST(CriticalCssExtractorTest, ContainerAtRuleWordBoundary) {
  // "@layerX" should NOT be treated as a container at-rule.
  CriticalCssExtractor extractor;
  std::vector<CollectedElement> elements;
  elements.push_back(MakeElement("div", 0, 0));

  std::string css =
      "@layerX {\n"
      "  div { color: red; }\n"
      "}\n";

  auto result = extractor.Extract(elements, css);
  EXPECT_TRUE(result.success);
  // @layerX is not a known at-rule, so it should NOT be recursed into
  // and should not match any element selector check.
  // The selector "@layerX" won't match ShouldIncludeSelector, so it
  // should be dropped entirely.
  EXPECT_EQ(result.critical_css.find("color: red"), std::string::npos);
}

TEST(CriticalCssExtractorTest, EscapedColonInClassSelector) {
  CriticalCssExtractor extractor;
  std::vector<CollectedElement> elements;
  elements.push_back(MakeElement("div", 0, 0, "", {"lg:grid"}));

  std::string css = R"(.lg\:grid { display: grid; })";
  CriticalCssResult result = extractor.Extract(elements, css);

  EXPECT_TRUE(result.success);
  EXPECT_NE(result.critical_css.find(R"(.lg\:grid {)"), std::string::npos)
      << "Escaped colon in class selector should match element with class "
         "'lg:grid'";
  EXPECT_EQ(1, result.critical_rules);
}

TEST(CriticalCssExtractorTest, EscapedBracketsInSelector) {
  CriticalCssExtractor extractor;
  std::vector<CollectedElement> elements;
  elements.push_back(MakeElement("div", 0, 0, "", {"w-[100px]"}));

  std::string css = R"(.w-\[100px\] { width: 100px; })";
  CriticalCssResult result = extractor.Extract(elements, css);

  EXPECT_TRUE(result.success);
  EXPECT_NE(result.critical_css.find(R"(.w-\[100px\] {)"), std::string::npos)
      << "Escaped brackets in class selector should match element with class "
         "'w-[100px]'";
  EXPECT_EQ(1, result.critical_rules);
}

TEST(CriticalCssExtractorTest, MultipleEscapedClasses) {
  CriticalCssExtractor extractor;
  std::vector<CollectedElement> elements;
  elements.push_back(MakeElement("div", 0, 0, "", {"lg:grid-cols-[240px]"}));

  std::string css =
      R"(.lg\:grid-cols-\[240px\] { grid-template-columns: 240px; })";
  CriticalCssResult result = extractor.Extract(elements, css);

  EXPECT_TRUE(result.success);
  EXPECT_NE(result.critical_css.find(R"(.lg\:grid-cols-\[240px\] {)"),
            std::string::npos)
      << "Multiple escaped characters in a single class selector should match";
  EXPECT_EQ(1, result.critical_rules);
}

TEST(CriticalCssExtractorTest, EscapedBackslash) {
  CriticalCssExtractor extractor;
  std::vector<CollectedElement> elements;
  elements.push_back(MakeElement("div", 0, 0, "", {"class\\name"}));

  std::string css = R"(.class\\name { color: red; })";
  CriticalCssResult result = extractor.Extract(elements, css);

  EXPECT_TRUE(result.success);
  EXPECT_NE(result.critical_css.find(R"(.class\\name {)"), std::string::npos)
      << "Double backslash in selector should match element with literal "
         "backslash in class name";
  EXPECT_EQ(1, result.critical_rules);
}

TEST(CriticalCssExtractorTest, NoEscapeNeeded) {
  CriticalCssExtractor extractor;
  std::vector<CollectedElement> elements;
  elements.push_back(MakeElement("div", 0, 0, "", {"normal-class"}));

  std::string css = ".normal-class { color: blue; }";
  CriticalCssResult result = extractor.Extract(elements, css);

  EXPECT_TRUE(result.success);
  EXPECT_NE(result.critical_css.find(".normal-class {"), std::string::npos)
      << "Normal class selectors without escapes should still work";
  EXPECT_EQ(1, result.critical_rules);
}

TEST(CriticalCssExtractorTest, EscapedSelectorDoesNotMatchPartialClass) {
  CriticalCssExtractor extractor;
  std::vector<CollectedElement> elements;
  // Element has class "lg" (no colon), NOT "lg:grid".
  elements.push_back(MakeElement("div", 0, 0, "", {"lg"}));

  std::string css = R"(.lg\:grid { display: grid; })";
  CriticalCssResult result = extractor.Extract(elements, css);

  EXPECT_TRUE(result.success);
  EXPECT_EQ(result.critical_css.find(R"(.lg\:grid {)"), std::string::npos)
      << "Escaped selector .lg\\:grid should NOT match element with class 'lg'";
  EXPECT_EQ(0, result.critical_rules);
}

TEST(CriticalCssExtractorTest, CompoundEscapedAndNormalClasses) {
  CriticalCssExtractor extractor;
  std::vector<CollectedElement> elements;
  elements.push_back(MakeElement("div", 0, 0, "", {"lg:grid", "mt-4"}));

  // Compound selector: both classes must match.
  std::string css = R"(.lg\:grid.mt-4 { display: grid; })";
  CriticalCssResult result = extractor.Extract(elements, css);

  EXPECT_TRUE(result.success);
  EXPECT_NE(result.critical_css.find(R"(.lg\:grid.mt-4 {)"), std::string::npos)
      << "Compound escaped+normal class selector should match element with "
         "both classes";
  EXPECT_EQ(1, result.critical_rules);
}

TEST(CriticalCssExtractorTest, HoverPrefixEscapedSelector) {
  CriticalCssExtractor extractor;
  std::vector<CollectedElement> elements;
  elements.push_back(MakeElement("div", 0, 0, "", {"hover:bg-red-500"}));

  std::string css = R"(.hover\:bg-red-500 { background: red; })";
  CriticalCssResult result = extractor.Extract(elements, css);

  EXPECT_TRUE(result.success);
  EXPECT_NE(result.critical_css.find(R"(.hover\:bg-red-500 {)"),
            std::string::npos)
      << "Escaped hover prefix selector should match";
  EXPECT_EQ(1, result.critical_rules);
}

// =============================================================================
// CSS hex escape tests
// =============================================================================

TEST(CriticalCssExtractorTest, HexEscapeColonInClassSelectorNoSpace) {
  CriticalCssExtractor extractor;
  std::vector<CollectedElement> elements;
  elements.push_back(MakeElement("div", 0, 0, "", {"lg:grid"}));

  // \3agrid — no space after hex digits, 'g' is a hex digit so it becomes
  // part of the codepoint.  Use \3a grid (with space consumed) instead.
  // This test uses \3a grid where the space is consumed by the hex escape.
  std::string css = R"(.lg\3a grid { display: grid; })";
  CriticalCssResult result = extractor.Extract(elements, css);

  EXPECT_TRUE(result.success);
  EXPECT_NE(result.critical_css.find(R"(.lg\3a grid {)"), std::string::npos)
      << "Hex escape \\3a with consumed trailing space should match class "
         "'lg:grid'";
  EXPECT_EQ(1, result.critical_rules);
}

TEST(CriticalCssExtractorTest, HexEscapeColonInClassSelectorNoTrailingSpace) {
  CriticalCssExtractor extractor;
  std::vector<CollectedElement> elements;
  elements.push_back(MakeElement("div", 0, 0, "", {"lg:grid"}));

  // \3agrid — 'g' is a hex digit, so this actually parses as codepoint 0x3AG..
  // Wait, 'r' is not hex. So \3a consumes '3' and 'a', stops at 'g'... no,
  // 'g' is NOT a hex digit. So \3a stops, no trailing space, then 'grid'.
  std::string css = R"(.lg\3agrid { display: grid; })";
  CriticalCssResult result = extractor.Extract(elements, css);

  EXPECT_TRUE(result.success);
  EXPECT_NE(result.critical_css.find(R"(.lg\3agrid {)"), std::string::npos)
      << "Hex escape \\3a immediately followed by non-hex 'grid' should match "
         "class 'lg:grid'";
  EXPECT_EQ(1, result.critical_rules);
}

TEST(CriticalCssExtractorTest, HexEscapeBracketInSelector) {
  CriticalCssExtractor extractor;
  std::vector<CollectedElement> elements;
  elements.push_back(MakeElement("div", 0, 0, "", {"w-[100px]"}));

  // \5b = '[' (U+005B), \5d = ']' (U+005D).
  // Two spaces before { — first consumed by \5d hex escape, second is the
  // separator.  The parser trims whitespace, so output has one space before {.
  std::string css = R"(.w-\5b 100px\5d  { width: 100px; })";
  CriticalCssResult result = extractor.Extract(elements, css);

  EXPECT_TRUE(result.success);
  EXPECT_NE(result.critical_css.find(R"(.w-\5b 100px\5d {)"), std::string::npos)
      << "Hex escapes \\5b and \\5d should match class 'w-[100px]'";
  EXPECT_EQ(1, result.critical_rules);
}

TEST(CriticalCssExtractorTest, HexEscapeUpperCase) {
  CriticalCssExtractor extractor;
  std::vector<CollectedElement> elements;
  elements.push_back(MakeElement("div", 0, 0, "", {"lg:grid"}));

  // \3A (uppercase) should work the same as \3a
  std::string css = R"(.lg\3A grid { display: grid; })";
  CriticalCssResult result = extractor.Extract(elements, css);

  EXPECT_TRUE(result.success);
  EXPECT_NE(result.critical_css.find(R"(.lg\3A grid {)"), std::string::npos)
      << "Uppercase hex escape \\3A should match class 'lg:grid'";
  EXPECT_EQ(1, result.critical_rules);
}

TEST(CriticalCssExtractorTest, HexEscapeSixDigits) {
  CriticalCssExtractor extractor;
  std::vector<CollectedElement> elements;
  elements.push_back(MakeElement("div", 0, 0, "", {"lg:grid"}));

  // \00003a = ':' (U+003A) — full 6 hex digits, no trailing space needed
  std::string css = R"(.lg\00003agrid { display: grid; })";
  CriticalCssResult result = extractor.Extract(elements, css);

  EXPECT_TRUE(result.success);
  EXPECT_NE(result.critical_css.find(R"(.lg\00003agrid {)"), std::string::npos)
      << "Six-digit hex escape \\00003a should match class 'lg:grid'";
  EXPECT_EQ(1, result.critical_rules);
}

TEST(CriticalCssExtractorTest, MixedHexAndLiteralEscapes) {
  CriticalCssExtractor extractor;
  std::vector<CollectedElement> elements;
  elements.push_back(MakeElement("div", 0, 0, "", {"lg:grid-cols-[240px]"}));

  // \3a for colon (hex escape), \[ and \] for brackets (literal escapes)
  std::string css = R"(.lg\3a grid-cols-\[240px\] { display: grid; })";
  CriticalCssResult result = extractor.Extract(elements, css);

  EXPECT_TRUE(result.success);
  EXPECT_NE(result.critical_css.find(R"(.lg\3a grid-cols-\[240px\] {)"),
            std::string::npos)
      << "Mixed hex (\\3a) and literal (\\[, \\]) escapes should match class "
         "'lg:grid-cols-[240px]'";
  EXPECT_EQ(1, result.critical_rules);
}

// =============================================================================
// @media wholesale inclusion threshold tests (inside @layer)
// =============================================================================

TEST(CriticalCssExtractorTest, SmallMediaBlockIncludedWholesale) {
  // A small @media block inside @layer should be included entirely when the
  // viewport overlaps, without per-rule filtering.
  CriticalCssExtractor extractor;
  std::vector<CollectedElement> elements;
  elements.push_back(MakeElement("div", 0, 0, "", {"container"}));
  elements.push_back(MakeElement("span", 1, 1, "", {"badge"}));

  // ~200 bytes — well under the 4096 default threshold.
  std::string css =
      "@layer utilities {\n"
      "  @media (min-width: 64rem) {\n"
      "    .container { max-width: 1200px; }\n"
      "    .badge { font-size: 0.75rem; }\n"
      "    .no-match-class { color: red; }\n"
      "  }\n"
      "}";

  // Tablet viewport overlaps min-width:64rem (1024px).
  CriticalCssResult result =
      extractor.Extract(elements, css, CapabilityMask::Viewport::kTablet);

  EXPECT_TRUE(result.success);
  // The entire @media block should be included wholesale — even .no-match-class
  // which does not match any collected element.
  EXPECT_NE(result.critical_css.find("max-width: 1200px"), std::string::npos)
      << ".container rule should be present";
  EXPECT_NE(result.critical_css.find("font-size: 0.75rem"), std::string::npos)
      << ".badge rule should be present";
  EXPECT_NE(result.critical_css.find("no-match-class"), std::string::npos)
      << ".no-match-class should be present (wholesale inclusion)";
}

TEST(CriticalCssExtractorTest, LargeMediaBlockFilteredPerRule) {
  // A large @media block inside @layer should be recursed into and
  // filtered per-rule when it exceeds the wholesale size threshold.
  CriticalCssConfig config;
  config.max_wholesale_media_bytes = 256;  // Low threshold to trigger filtering
  CriticalCssExtractor extractor(config);

  std::vector<CollectedElement> elements;
  elements.push_back(MakeElement("div", 0, 0, "", {"target"}));

  // Build a large @media block (>256 bytes) with many rules, only one of
  // which matches a collected element.
  std::string inner_rules = "    .target { color: green; }\n";
  for (int i = 0; i < 30; ++i) {
    inner_rules += "    .filler-class-" + std::to_string(i) +
                   " { padding: " + std::to_string(i) + "px; }\n";
  }

  std::string css =
      "@layer utilities {\n"
      "  @media (min-width: 64rem) {\n" +
      inner_rules +
      "  }\n"
      "}";

  CriticalCssResult result =
      extractor.Extract(elements, css, CapabilityMask::Viewport::kTablet);

  EXPECT_TRUE(result.success);
  // The matching rule should be present.
  EXPECT_NE(result.critical_css.find(".target"), std::string::npos)
      << ".target rule should survive per-rule filtering";
  EXPECT_NE(result.critical_css.find("color: green"), std::string::npos);
  // The @media wrapper should be present.
  EXPECT_NE(result.critical_css.find("@media (min-width: 64rem)"),
            std::string::npos)
      << "@media wrapper should be present around filtered rules";
  // Non-matching filler rules should be excluded.
  EXPECT_EQ(result.critical_css.find("filler-class-"), std::string::npos)
      << "Non-matching filler rules should be excluded by per-rule filtering";
}

TEST(CriticalCssExtractorTest, LargeMediaBlockOutsideLayerIsFilteredPerRule) {
  // A large @media block outside any @layer is filtered per rule, like one
  // inside a layer, and a small one is left out unless one of its rules is
  // needed. Copied whole, a framework's mobile-first breakpoint
  // blocks (Bootstrap 5 has ten `(min-width:576px)` blocks) pushed the mobile
  // block past kInlineCriticalCssMaxBytes once retention kept blocks up to
  // 980 px for phones.
  CriticalCssConfig config;
  config.max_wholesale_media_bytes = 256;  // Low threshold
  CriticalCssExtractor extractor(config);

  std::vector<CollectedElement> elements;
  elements.push_back(MakeElement("div", 0, 0, "", {"target"}));

  std::string inner_rules = "    .target { color: green; }\n";
  for (int i = 0; i < 30; ++i) {
    inner_rules += "    .filler-class-" + std::to_string(i) +
                   " { padding: " + std::to_string(i) + "px; }\n";
  }

  std::string css =
      "@media (min-width: 64rem) {\n" + inner_rules + "}\n" +
      "@media (min-width: 40rem) { .target { margin: 0; } "
      ".small-filler { margin: 1px; } }\n"
      "@media (min-width: 40rem) { .only-filler { margin: 2px; } }";

  CriticalCssResult result =
      extractor.Extract(elements, css, CapabilityMask::Viewport::kTablet);

  EXPECT_TRUE(result.success);
  EXPECT_NE(result.critical_css.find("@media (min-width: 64rem)"),
            std::string::npos)
      << "@media wrapper should be present around the filtered rules";
  EXPECT_NE(result.critical_css.find(".target"), std::string::npos);
  EXPECT_EQ(result.critical_css.find("filler-class-"), std::string::npos)
      << "rules for elements not on the fold are filtered out";
  EXPECT_NE(result.critical_css.find(".small-filler"), std::string::npos)
      << "a small block with a rule the fold needs is still copied whole";
  EXPECT_EQ(result.critical_css.find(".only-filler"), std::string::npos)
      << "a small block with no rule the fold needs is left out";
}

TEST(CriticalCssExtractorTest, HexEscapeSpaceNotTreatedAsCombinator) {
  // The selector `div .lg\3a grid` has a real combinator space (between `div`
  // and `.lg\3a grid`) and a hex-escape space (after `\3a`).  Only the
  // combinator space should split the selector.
  CriticalCssExtractor extractor;
  std::vector<CollectedElement> elements;
  elements.push_back(MakeElement("div", 0, 0, "", {"lg:grid"}));

  std::string css = R"(div .lg\3a grid { display: grid; })";
  CriticalCssResult result = extractor.Extract(elements, css);

  EXPECT_TRUE(result.success);
  EXPECT_NE(result.critical_css.find(R"(div .lg\3a grid {)"), std::string::npos)
      << "Descendant selector with hex escape should match element";
  EXPECT_EQ(1, result.critical_rules);
}

TEST(CriticalCssExtractorTest, SupportsInsideLayerPreservesInsideLayer) {
  // @supports nested inside @layer should still propagate inside_layer=true,
  // so large @media blocks within are filtered per-rule.
  CriticalCssConfig config;
  config.max_wholesale_media_bytes = 128;  // Low threshold
  CriticalCssExtractor extractor(config);
  std::vector<CollectedElement> elements;
  elements.push_back(MakeElement("div", 0, 0, "", {"target"}));

  // Build a >128-byte @media block inside @supports inside @layer.
  std::string media_body;
  media_body += "    .target { color: green; }\n";
  for (int i = 0; i < 10; ++i) {
    media_body += "    .filler-" + std::to_string(i) +
                  " { padding: " + std::to_string(i) + "px; }\n";
  }
  std::string css =
      "@layer utilities {\n"
      "  @supports (display: grid) {\n"
      "    @media (min-width: 64rem) {\n" +
      media_body +
      "    }\n"
      "  }\n"
      "}";

  CriticalCssResult result =
      extractor.Extract(elements, css, CapabilityMask::Viewport::kTablet);

  EXPECT_TRUE(result.success);
  EXPECT_NE(result.critical_css.find(".target"), std::string::npos)
      << ".target should be included via per-rule filtering";
  EXPECT_EQ(result.critical_css.find("filler-"), std::string::npos)
      << "Non-matching filler rules should be excluded (per-rule filtering "
         "inside @supports inside @layer)";
}

// --- Tailwind @custom-variant :where() tests ---

TEST(CriticalCssExtractorTest, TailwindDarkVariantWithWhere) {
  CriticalCssExtractor extractor;
  std::vector<CollectedElement> elements;
  // The HTML element has class "dark:bg-stone-950" which Tailwind escapes
  // as dark\:bg-stone-950 in the CSS selector.
  elements.push_back(
      MakeElement("header", 0, 0, "", {"bg-white", "dark:bg-stone-950"}));

  std::string css = R"(
    .bg-white { background-color: white; }
    .dark\:bg-stone-950:where(.dark,.dark *) { background-color: #0c0a09; }
  )";
  CriticalCssResult result = extractor.Extract(elements, css);

  EXPECT_TRUE(result.success);
  EXPECT_NE(result.critical_css.find("bg-white"), std::string::npos)
      << "bg-white should be in critical CSS";
  EXPECT_NE(result.critical_css.find("dark\\:bg-stone-950"), std::string::npos)
      << "dark:bg-stone-950 variant with :where() should be in critical CSS";
  EXPECT_EQ(2, result.critical_rules);
}

TEST(CriticalCssExtractorTest, TailwindDarkVariantWithWhereAndSpace) {
  CriticalCssExtractor extractor;
  std::vector<CollectedElement> elements;
  elements.push_back(MakeElement("div", 0, 0, "", {"dark:text-stone-100"}));

  // Spaces inside :where() must not be treated as descendant combinators.
  std::string css =
      R"(.dark\:text-stone-100:where(.dark, .dark *) { color: #f5f5f4; })";
  CriticalCssResult result = extractor.Extract(elements, css);

  EXPECT_TRUE(result.success);
  EXPECT_NE(result.critical_css.find("dark\\:text-stone-100"),
            std::string::npos)
      << "Space inside :where() should not split the selector";
  EXPECT_EQ(1, result.critical_rules);
}

TEST(CriticalCssExtractorTest, NestedParenthesesInWhere) {
  CriticalCssExtractor extractor;
  std::vector<CollectedElement> elements;
  elements.push_back(MakeElement("div", 0, 0, "", {"foo"}));

  // Nested parentheses: :where(:not(.bar .baz)) has spaces inside nested parens.
  std::string css = R"(.foo:where(:not(.bar .baz)) { color: red; })";
  CriticalCssResult result = extractor.Extract(elements, css);

  EXPECT_TRUE(result.success);
  EXPECT_NE(result.critical_css.find(".foo:where"), std::string::npos)
      << "Nested parentheses in :where(:not()) should not break selector "
         "parsing";
  EXPECT_EQ(1, result.critical_rules);
}

TEST(CriticalCssExtractorTest, DeeplyNestedParensInWhere) {
  CriticalCssExtractor extractor;
  std::vector<CollectedElement> elements;
  elements.push_back(MakeElement("div", 0, 0, "", {"x"}));

  // Triple-nested: :where(:not(:is(.a .b))) — spaces at depth 3.
  std::string css = R"(.x:where(:not(:is(.a .b))) { color: red; })";
  CriticalCssResult result = extractor.Extract(elements, css);

  EXPECT_TRUE(result.success);
  EXPECT_NE(result.critical_css.find(".x:where"), std::string::npos)
      << "Triple-nested parentheses should not break selector parsing";
  EXPECT_EQ(1, result.critical_rules);
}

// Regression for issue #258: a rule whose ONLY matchable token lives inside a
// `:where()` wrapper (no matchable outer simple selector) was dropped because
// the matcher broke at the leading ':' before parsing the inner alternatives.
// `:where()` is a zero-specificity scoping hint, not a gating predicate, so the
// inner selector list must be desugared and the rule retained when an inner
// alternative matches.
TEST(CriticalCssExtractorTest, WhereWrapsTheOnlyMatchableSelector) {
  CriticalCssExtractor extractor;
  std::vector<CollectedElement> elements;
  elements.push_back(MakeElement("div", 0, 0, "", {"bg-white"}));

  std::string css = R"(:where(.bg-white) { background-color: white; })";
  CriticalCssResult result = extractor.Extract(elements, css);

  EXPECT_TRUE(result.success);
  EXPECT_NE(result.critical_css.find(":where(.bg-white)"), std::string::npos)
      << ":where(.bg-white) should match a .bg-white element and be retained";
  EXPECT_EQ(1, result.critical_rules);
}

// Issue #258: the core dark-mode case, exactly as reported. Tailwind v4's
// `@custom-variant dark` compiles `dark:bg-stone-900` to a rule scoped by
// `:where(.dark, .dark *)`. The card element carries the literal utility class
// `dark:bg-stone-900`, but the `.dark` scope is set on <html> by a client-side
// script that has NOT run when the DOM is sampled. So `:where(.dark, .dark *)`
// matches nothing in the sample. Per the issue, `:where()` is a zero-specificity
// scoping hint, not a gating predicate: for `A:where(B)`, the rule must be
// retained when `A` matches a sampled element, regardless of `B`. The dark rule
// must therefore land in the critical CSS so a dark site does not flash light.
TEST(CriticalCssExtractorTest, WhereDarkScopeIsNonConstraining) {
  CriticalCssExtractor extractor;
  std::vector<CollectedElement> elements;
  // The card element carries both the light and dark utility classes (as
  // Tailwind emits them), but NOT `.dark` — that scope is applied to <html>
  // post-sample by client JS, so `:where(.dark, .dark *)` does not match here.
  elements.push_back(
      MakeElement("div", 0, 0, "", {"bg-white", "dark:bg-stone-900"}));

  std::string css = R"(
    .bg-white { background-color: var(--color-white); }
    .dark\:bg-stone-900:where(.dark, .dark *) { background-color: #1c1917; }
  )";
  CriticalCssResult result = extractor.Extract(elements, css);

  EXPECT_TRUE(result.success);
  EXPECT_NE(result.critical_css.find(".bg-white"), std::string::npos)
      << "light-mode rule should be in critical CSS";
  EXPECT_NE(result.critical_css.find("dark\\:bg-stone-900"), std::string::npos)
      << "dark `:where()`-scoped rule must be retained (where() is "
         "non-constraining) to avoid a light flash on dark sites";
  EXPECT_EQ(2, result.critical_rules);
}

// Issue #258: `:is(...)` shares `:where()`'s desugaring path. A leading
// `:is(.hero, .banner)` with no outer simple selector must be retained when an
// inner alternative matches a sampled element.
TEST(CriticalCssExtractorTest, IsWrapsTheOnlyMatchableSelector) {
  CriticalCssExtractor extractor;
  std::vector<CollectedElement> elements;
  elements.push_back(MakeElement("section", 0, 0, "", {"banner"}));

  std::string css = R"(:is(.hero, .banner) { padding: 2rem; })";
  CriticalCssResult result = extractor.Extract(elements, css);

  EXPECT_TRUE(result.success);
  EXPECT_NE(result.critical_css.find(":is(.hero, .banner)"), std::string::npos)
      << ":is() inner alternative match should retain the rule";
  EXPECT_EQ(1, result.critical_rules);
}

// Issue #258 guard: `:where()`/`:is()` must remain non-constraining and must
// NOT cause false retention. A rule whose only matchable token is inside a
// `:where()` that matches NOTHING in the sampled DOM should still be dropped.
TEST(CriticalCssExtractorTest, WhereWithNoInnerMatchIsDropped) {
  CriticalCssExtractor extractor;
  std::vector<CollectedElement> elements;
  elements.push_back(MakeElement("div", 0, 0, "", {"bg-white"}));

  std::string css = R"(:where(.never-present) { color: red; })";
  CriticalCssResult result = extractor.Extract(elements, css);

  EXPECT_TRUE(result.success);
  EXPECT_EQ(result.critical_css.find(".never-present"), std::string::npos)
      << ":where() wrapping an unmatched selector must not be retained";
  EXPECT_EQ(0, result.critical_rules);
}

// ---------------------------------------------------------------------------
// Async-CSS FOUC sufficiency gate (CriticalCssIsSufficient).
// ---------------------------------------------------------------------------

// All cases below pass external_css_unresolved=false (every declared external
// sheet was resolved from cache) unless they specifically exercise the
// cold-cache fail-safe.
constexpr bool kResolved = false;
constexpr bool kUnresolved = true;

TEST(AsyncCssSufficiencyTest, ThinCriticalOverLargeSheetIsInsufficient) {
  // The live FOUC regression: ~830 B of critical CSS deferring a ~110 KB sheet
  // (~0.0075 coverage) must NOT defer.
  AsyncCssSufficiencyConfig cfg;  // defaults: 0.10 floor, 15000 byte escape
  EXPECT_FALSE(CriticalCssIsSufficient(-1.0f, 830, 110000, kResolved, cfg));
}

TEST(AsyncCssSufficiencyTest, OptimisticProfileCoverageCannotOverrideBytes) {
  // The 2026-07-04 production FOUC (#879): the browser profile claimed
  // coverage=0.39 while the EXTRACTED critical CSS was 830 B against a 115 KB
  // sheet (~0.007). The optimistic profile number must never authorize
  // deferral on its own — the byte ratio is a hard floor.
  AsyncCssSufficiencyConfig cfg;  // defaults: 0.10 floor
  EXPECT_FALSE(CriticalCssIsSufficient(0.39f, 830, 115000, kResolved, cfg));
  EXPECT_FALSE(CriticalCssIsSufficient(0.40f, 830, 110000, kResolved, cfg));
}

TEST(AsyncCssSufficiencyTest, PessimisticProfileCoverageStillGates) {
  AsyncCssSufficiencyConfig cfg;
  // Known LOW coverage -> insufficient even when the byte ratio looks healthy
  // (coverage is trusted downward: a profile that measured most rules unused
  // can veto a byte-fat critical block).
  EXPECT_FALSE(CriticalCssIsSufficient(0.05f, 40000, 110000, kResolved, cfg));
  // Both healthy -> sufficient.
  EXPECT_TRUE(CriticalCssIsSufficient(0.40f, 40000, 110000, kResolved, cfg));
}

TEST(AsyncCssSufficiencyTest, SmallDeferredSheetAlwaysSufficient) {
  // A genuinely small CACHED/RESOLVED sheet has a trivial FOUC window.
  AsyncCssSufficiencyConfig cfg;  // 15000 byte escape hatch
  EXPECT_TRUE(CriticalCssIsSufficient(-1.0f, 500, 4096, kResolved, cfg));
  EXPECT_TRUE(CriticalCssIsSufficient(0.01f, 500, 4096, kResolved, cfg));
}

TEST(AsyncCssSufficiencyTest, ColdCacheUnresolvedIsInsufficient) {
  // The iispeed.com FOUC: a declared external <link> is NOT yet in cache, so
  // the deferred byte count collapsed to the ~179 B inline-only blob — which
  // would otherwise trip the small-sheet escape hatch and defer the unmeasured
  // 39 KB sheet. external_css_unresolved must dominate and refuse to defer,
  // regardless of how small/large the (wrong) byte counts or coverage look.
  AsyncCssSufficiencyConfig cfg;
  EXPECT_FALSE(CriticalCssIsSufficient(-1.0f, 179, 179, kUnresolved, cfg));
  // Even a (falsely) healthy-looking ratio or large bytes cannot override it.
  EXPECT_FALSE(CriticalCssIsSufficient(0.95f, 50000, 60000, kUnresolved, cfg));
  // Floor-disabled gate does NOT bypass the cold-cache fail-safe.
  cfg.min_coverage_ratio = 0.0f;
  EXPECT_FALSE(CriticalCssIsSufficient(-1.0f, 1, 1000000, kUnresolved, cfg));
}

TEST(AsyncCssSufficiencyTest, RichCriticalIsSufficient) {
  AsyncCssSufficiencyConfig cfg;
  EXPECT_TRUE(
      CriticalCssIsSufficient(-1.0f, 40000, 110000, kResolved, cfg));  // ~0.36
}

TEST(AsyncCssSufficiencyTest, FloorOfZeroDisablesGate) {
  AsyncCssSufficiencyConfig cfg;
  cfg.min_coverage_ratio = 0.0f;
  EXPECT_TRUE(CriticalCssIsSufficient(-1.0f, 1, 1000000, kResolved, cfg));
}

TEST(AsyncCssSufficiencyTest, HeuristicFallbackDerivesRatioFromBytes) {
  AsyncCssSufficiencyConfig cfg;  // 0.10 floor
  EXPECT_FALSE(
      CriticalCssIsSufficient(-1.0f, 800, 100000, kResolved, cfg));  // 0.008
  EXPECT_TRUE(
      CriticalCssIsSufficient(-1.0f, 10000, 100000, kResolved, cfg));  // 0.10
}

TEST(AsyncCssSufficiencyTest, NaNCoverageFallsBackToByteRatio) {
  AsyncCssSufficiencyConfig cfg;
  const float nan = std::numeric_limits<float>::quiet_NaN();
  EXPECT_FALSE(CriticalCssIsSufficient(nan, 800, 100000, kResolved, cfg));
  EXPECT_TRUE(CriticalCssIsSufficient(nan, 20000, 100000, kResolved, cfg));
}

TEST(AsyncCssSufficiencyTest, DoesNotFightHighEndDegradedGuard) {
  // The over-report (coverage near 1.0) is handled by LooksLikeDegradedProfile,
  // not here: a high ratio is "sufficient" from this gate's perspective.
  AsyncCssSufficiencyConfig cfg;
  EXPECT_TRUE(CriticalCssIsSufficient(0.99f, 109000, 110000, kResolved, cfg));
}

TEST(AsyncCssSufficiencyTest, ZeroDeferredBytesIsSufficient) {
  AsyncCssSufficiencyConfig cfg;
  // 0-byte sheet with a substantial floor and unknown coverage: avoid div-by-0.
  cfg.min_deferred_css_bytes = 0;
  EXPECT_TRUE(CriticalCssIsSufficient(-1.0f, 0, 0, kResolved, cfg));
}

// ---------------------------------------------------------------------------
// Issue #1056: DOM-matched retention of used above-the-fold utility rules.
// ---------------------------------------------------------------------------

// A state-conditional dark variant scoped by `:where(.dark, .dark *)` is
// retained when the element literally carries the variant class, even though
// the `.dark` scope is never active in a static, JS-off, light-mode sample.
TEST(CriticalCssExtractorTest, DomMatchedRetainsDarkVariantOnAboveFoldElement) {
  CriticalCssExtractor extractor;
  std::vector<CollectedElement> elements;
  elements.push_back(MakeElement("div", 0, 0, "", {"dark:bg-stone-900"}));

  std::string css =
      R"(.dark\:bg-stone-900:where(.dark,.dark *) { background-color: #1c1917; })";
  CriticalCssResult result = extractor.Extract(elements, css);

  EXPECT_TRUE(result.success);
  EXPECT_NE(result.critical_css.find("dark\\:bg-stone-900"), std::string::npos)
      << "dark variant on an above-the-fold element must be retained";
  EXPECT_EQ(1, result.critical_rules);
}

// A skip-link's `focus:not-sr-only` variant (and the base `sr-only`) is retained
// when the element carries both classes: the `:focus` pseudo is a non-gating
// constraint for retention, and the escaped-colon variant class DOM-matches.
TEST(CriticalCssExtractorTest, DomMatchedRetainsFocusVariantSkipLink) {
  CriticalCssExtractor extractor;
  std::vector<CollectedElement> elements;
  elements.push_back(
      MakeElement("a", 0, 0, "", {"sr-only", "focus:not-sr-only"}));

  std::string css =
      ".sr-only { position: absolute; width: 1px; height: 1px; }\n"
      ".focus\\:not-sr-only:focus { position: static; width: auto; }";
  CriticalCssResult result = extractor.Extract(elements, css);

  EXPECT_TRUE(result.success);
  EXPECT_NE(result.critical_css.find(".sr-only"), std::string::npos)
      << "sr-only base rule must be retained";
  EXPECT_NE(result.critical_css.find("focus\\:not-sr-only"), std::string::npos)
      << "focus:not-sr-only variant must be retained for the skip link";
  EXPECT_EQ(2, result.critical_rules);
}

// State-unconditional Tailwind v4 utilities (.flex/.h-16/.text-sm) inside
// `@layer utilities` are retained, the @layer wrapper preserved, output balanced.
TEST(CriticalCssExtractorTest, DomMatchedRetainsStateUnconditionalUtilities) {
  CriticalCssExtractor extractor;
  std::vector<CollectedElement> elements;
  elements.push_back(MakeElement("nav", 0, 0, "", {"flex", "h-16", "text-sm"}));

  std::string css =
      "@layer utilities {\n"
      "  .flex { display: flex; }\n"
      "  .h-16 { height: 4rem; }\n"
      "  .text-sm { font-size: 0.875rem; }\n"
      "}";
  CriticalCssResult result = extractor.Extract(elements, css);

  EXPECT_TRUE(result.success);
  EXPECT_NE(result.critical_css.find("@layer utilities"), std::string::npos)
      << "the @layer wrapper must be preserved";
  EXPECT_NE(result.critical_css.find(".flex"), std::string::npos);
  EXPECT_NE(result.critical_css.find(".h-16"), std::string::npos);
  EXPECT_NE(result.critical_css.find(".text-sm"), std::string::npos);
  EXPECT_EQ(3, result.critical_rules);
  // The serialized block must be brace-balanced.
  auto opens =
      std::count(result.critical_css.begin(), result.critical_css.end(), '{');
  auto closes =
      std::count(result.critical_css.begin(), result.critical_css.end(), '}');
  EXPECT_EQ(opens, closes) << "serialized critical CSS must be brace-balanced";
}

// An attribute selector cannot be captured from the DOM sample, but when its
// identity is supplied via force_include it is re-emitted inside its @layer.
TEST(CriticalCssExtractorTest, ForceIncludeAugmentsMatcherBlindSpot) {
  CriticalCssExtractor extractor;
  std::vector<CollectedElement> elements;
  elements.push_back(MakeElement("div", 0, 0, "", {"box"}));

  std::string css =
      "@layer components {\n"
      "  [data-x] { color: red; }\n"
      "}";

  // Without force_include the attribute rule cannot match the sample DOM.
  CriticalCssResult without = extractor.Extract(elements, css);
  EXPECT_EQ(without.critical_css.find("[data-x]"), std::string::npos)
      << "attribute selector is not DOM-matchable and should be dropped";

  // With its identity forced, it is re-emitted inside its @layer wrapper.
  absl::flat_hash_set<RuleIdentity> force;
  force.insert(RuleIdentity{"components", "", NormalizeSelector("[data-x]")});
  CriticalCssResult with = extractor.Extract(
      elements, css, CapabilityMask::Viewport::kDesktop, &force);
  EXPECT_NE(with.critical_css.find("[data-x]"), std::string::npos)
      << "force_include identity must re-emit the attribute rule";
  EXPECT_NE(with.critical_css.find("@layer components"), std::string::npos)
      << "forced rule must be serialized inside its layer wrapper";
}

// The Tailwind v4 layer-order statement `@layer a,b,c,d;` is preserved and
// emitted before the block rules that reference those layers.
TEST(CriticalCssExtractorTest, LayerOrderStatementEmittedFirst) {
  CriticalCssExtractor extractor;
  std::vector<CollectedElement> elements;
  elements.push_back(MakeElement("div", 0, 0, "", {"flex"}));

  std::string css =
      "@layer theme,base,components,utilities;\n"
      "@layer utilities { .flex { display: flex; } }";
  CriticalCssResult result = extractor.Extract(elements, css);

  EXPECT_TRUE(result.success);
  size_t order_pos =
      result.critical_css.find("@layer theme,base,components,utilities;");
  ASSERT_NE(order_pos, std::string::npos)
      << "layer-order statement must be preserved";
  size_t flex_pos = result.critical_css.find(".flex");
  ASSERT_NE(flex_pos, std::string::npos);
  EXPECT_LT(order_pos, flex_pos)
      << "the layer-order statement must precede the block rules";
}

// A custom-property registration is not a style rule: dropping it does not
// merely omit styling, it changes how the RETAINED rules compute. An
// unregistered `var(--x)` is invalid at computed-value time, so a rule that
// survived into the critical block renders wrong until the full sheet lands.
TEST(CriticalCssExtractorTest, PropertyRegistrationsAreAlwaysIncluded) {
  CriticalCssExtractor extractor;
  std::vector<CollectedElement> elements;
  elements.push_back(MakeElement("div", 0, 0, "", {"box"}));

  std::string css =
      "@property --tw-border-style { syntax: \"*\"; inherits: false; "
      "initial-value: solid; }\n"
      ".box { border-style: var(--tw-border-style); }";
  CriticalCssResult result = extractor.Extract(elements, css);

  EXPECT_TRUE(result.success);
  EXPECT_NE(result.critical_css.find("@property --tw-border-style"),
            std::string::npos)
      << "@property registration must survive into the critical block";
  EXPECT_NE(result.critical_css.find(".box"), std::string::npos)
      << "the rule consuming the registered property must still be retained";
}

TEST(CriticalCssExtractorTest, CounterStyleAndFontPaletteValuesAlwaysIncluded) {
  CriticalCssExtractor extractor;
  std::vector<CollectedElement> elements;
  elements.push_back(MakeElement("div", 0, 0, "", {"box"}));

  std::string css =
      "@counter-style thumbs { system: cyclic; symbols: \"X\"; suffix: \" \"; "
      "}\n"
      "@font-palette-values --Alt { font-family: \"Bixa\"; base-palette: 1; }\n"
      ".box { list-style: thumbs; font-palette: --Alt; }";
  CriticalCssResult result = extractor.Extract(elements, css);

  EXPECT_TRUE(result.success);
  EXPECT_NE(result.critical_css.find("@counter-style thumbs"),
            std::string::npos)
      << "@counter-style must survive into the critical block";
  EXPECT_NE(result.critical_css.find("@font-palette-values --Alt"),
            std::string::npos)
      << "@font-palette-values must survive into the critical block";
}

// Tailwind v4 emits its registrations inside `@layer` (and sometimes behind an
// `@supports` probe), so the always-include rule has to hold through the
// container recursion, not just at top level.
TEST(CriticalCssExtractorTest,
     PropertyRegistrationSurvivesInsideLayerAndSupports) {
  CriticalCssExtractor extractor;
  std::vector<CollectedElement> elements;
  elements.push_back(MakeElement("div", 0, 0, "", {"box"}));

  std::string css =
      "@layer base {\n"
      "  @supports (color: red) {\n"
      "    @property --tw-x { syntax: \"*\"; inherits: false; }\n"
      "  }\n"
      "}";
  CriticalCssResult result = extractor.Extract(elements, css);

  EXPECT_TRUE(result.success);
  EXPECT_NE(result.critical_css.find("@property --tw-x"), std::string::npos)
      << "@property nested in @layer/@supports must survive";
  EXPECT_NE(result.critical_css.find("@layer base"), std::string::npos)
      << "the @layer wrapper must be preserved";
  EXPECT_NE(result.critical_css.find("@supports"), std::string::npos)
      << "the @supports wrapper must be preserved";
  auto opens =
      std::count(result.critical_css.begin(), result.critical_css.end(), '{');
  auto closes =
      std::count(result.critical_css.begin(), result.critical_css.end(), '}');
  EXPECT_EQ(opens, closes) << "serialized critical CSS must be brace-balanced";
}

// Chrome CSS-coverage ranges are byte slices of the stylesheet: the enclosing
// `@layer utilities {` prelude is not part of any used range, so the identity
// derived from coverage arrives context-free. It must still reach the rule the
// producer sees inside the layer.
TEST(CriticalCssExtractorTest,
     ForceIncludeMatchesLayeredRuleWhenCoverageIdentityLacksLayerPath) {
  CriticalCssExtractor extractor;
  std::vector<CollectedElement> elements;
  elements.push_back(MakeElement("div", 0, 0, "", {"box"}));

  std::string css = "@layer utilities { .flex { display: flex; } }";

  CriticalCssResult without = extractor.Extract(elements, css);
  ASSERT_EQ(without.critical_css.find(".flex"), std::string::npos)
      << ".flex does not match the sample DOM, so only force_include can "
         "retain it";

  absl::flat_hash_set<RuleIdentity> force;
  force.insert(RuleIdentity{"", "", NormalizeSelector(".flex")});
  CriticalCssResult with = extractor.Extract(
      elements, css, CapabilityMask::Viewport::kDesktop, &force);
  EXPECT_NE(with.critical_css.find(".flex"), std::string::npos)
      << "a context-free coverage identity must reach the layered rule";
  EXPECT_NE(with.critical_css.find("@layer utilities"), std::string::npos)
      << "the forced rule must be serialized inside its layer wrapper";
}

// The responsive-variant case: the coverage slice drops the `@media` prelude
// as well as the `@layer` one, so both context fields must relax together.
// Responsive variants are the bulk of a utility sheet — relaxing only the
// layer path would leave the seam inert for exactly the class that matters.
TEST(
    CriticalCssExtractorTest,
    ForceIncludeMatchesMediaNestedRuleWhenCoverageIdentityLacksMediaCondition) {
  CriticalCssConfig config;
  // Force the per-rule @media recursion rather than wholesale inclusion.
  config.max_wholesale_media_bytes = 8;
  CriticalCssExtractor extractor(config);
  std::vector<CollectedElement> elements;
  elements.push_back(MakeElement("div", 0, 0, "", {"box"}));

  std::string css =
      "@layer utilities {\n"
      "  @media (min-width: 768px) { .md\\:flex { display: flex; } }\n"
      "}";

  CriticalCssResult without = extractor.Extract(elements, css);
  ASSERT_EQ(without.critical_css.find("md\\:flex"), std::string::npos)
      << "the responsive variant does not match the sample DOM";

  absl::flat_hash_set<RuleIdentity> force;
  force.insert(RuleIdentity{"", "", NormalizeSelector(".md\\:flex")});
  CriticalCssResult with = extractor.Extract(
      elements, css, CapabilityMask::Viewport::kDesktop, &force);
  EXPECT_NE(with.critical_css.find("md\\:flex"), std::string::npos)
      << "a context-free coverage identity must reach the responsive variant";
  EXPECT_NE(with.critical_css.find("@layer utilities"), std::string::npos);
  EXPECT_NE(with.critical_css.find("@media (min-width: 768px)"),
            std::string::npos)
      << "the forced rule must keep its @media wrapper";
}

// The ambiguity control on relaxation: when the sheet places the same
// normalized selector in more than one cascade LAYER, a context-free identity
// matches NEITHER. Those are different rules with different precedence and
// nothing in a context-free identity says which one the browser used, so
// relaxation refuses rather than guessing.
TEST(CriticalCssExtractorTest, AmbiguousRelaxedForceIncludeKeyMatchesNothing) {
  CriticalCssExtractor extractor;
  std::vector<CollectedElement> elements;
  elements.push_back(MakeElement("div", 0, 0, "", {"box"}));

  std::string css =
      "@layer base { .btn { color: red; } }\n"
      "@layer utilities { .btn { color: blue; } }";

  absl::flat_hash_set<RuleIdentity> force;
  force.insert(RuleIdentity{"", "", NormalizeSelector(".btn")});
  CriticalCssResult result = extractor.Extract(
      elements, css, CapabilityMask::Viewport::kDesktop, &force);

  EXPECT_TRUE(result.success);
  EXPECT_EQ(result.critical_css.find(".btn"), std::string::npos)
      << "an ambiguous relaxed key must match neither context";
}

// Relaxation is a fallback for identities that LOST their context, not a
// wildcard: an identity that carries a layer path keeps exact-match semantics.
TEST(CriticalCssExtractorTest,
     RelaxedFallbackOnlyAppliesToContextFreeIdentities) {
  CriticalCssExtractor extractor;
  std::vector<CollectedElement> elements;
  elements.push_back(MakeElement("div", 0, 0, "", {"box"}));

  std::string css = "@layer utilities { .grid { display: grid; } }";

  absl::flat_hash_set<RuleIdentity> force;
  force.insert(RuleIdentity{"components", "", NormalizeSelector(".grid")});
  CriticalCssResult result = extractor.Extract(
      elements, css, CapabilityMask::Viewport::kDesktop, &force);

  EXPECT_TRUE(result.success);
  EXPECT_EQ(result.critical_css.find(".grid"), std::string::npos)
      << "an identity naming a different layer must not relax into this one";
}

// Several @media conditions within ONE layer are not ambiguity. A context-free
// coverage identity means the browser used the selector with no media
// qualification in play, so the unconditional occurrence is the one it names.
//
// The `.container` shape is why this matters: forcing the base rule forces its
// breakpoint overrides in the same layer too (@media blocks are filtered per
// rule, so they no longer ride along whole), and treating the selector as
// ambiguous would leave `.container` half-styled — a worse result than either
// forcing or dropping the whole family.
TEST(CriticalCssExtractorTest,
     RelaxedForceIncludePrefersUnconditionalOccurrence) {
  CriticalCssExtractor extractor;
  std::vector<CollectedElement> elements;
  elements.push_back(MakeElement("div", 0, 0, "", {"box"}));

  std::string css =
      "@layer utilities {\n"
      "  .container { width: 100%; }\n"
      "  @media (min-width: 640px) { .container { max-width: 640px; } }\n"
      "  @media (min-width: 768px) { .container { max-width: 768px; } }\n"
      "}";

  absl::flat_hash_set<RuleIdentity> force;
  force.insert(RuleIdentity{"", "", NormalizeSelector(".container")});
  CriticalCssResult result = extractor.Extract(
      elements, css, CapabilityMask::Viewport::kDesktop, &force);

  EXPECT_TRUE(result.success);
  EXPECT_NE(result.critical_css.find("width: 100%"), std::string::npos)
      << "the unconditional .container rule must be force-included";
  EXPECT_NE(result.critical_css.find("max-width: 640px"), std::string::npos)
      << "the 640px override rides along with its @media block";
  EXPECT_NE(result.critical_css.find("max-width: 768px"), std::string::npos)
      << "the 768px override rides along with its @media block";
  auto opens =
      std::count(result.critical_css.begin(), result.critical_css.end(), '{');
  auto closes =
      std::count(result.critical_css.begin(), result.critical_css.end(), '}');
  EXPECT_EQ(opens, closes) << "serialized critical CSS must be brace-balanced";
}

// Characterization of accepted over-inclusion: @supports is transparent for
// identity everywhere in this file, so a guarded and an unguarded copy of the
// same selector collapse to ONE context and a context-free identity forces
// BOTH. Bounded — the two are cascade alternatives, and only one can ever win.
TEST(CriticalCssExtractorTest, SupportsIsTransparentForRelaxedForceInclude) {
  CriticalCssExtractor extractor;
  std::vector<CollectedElement> elements;
  elements.push_back(MakeElement("div", 0, 0, "", {"box"}));

  std::string css =
      "@layer utilities {\n"
      "  .gap-4 { gap: 1rem; }\n"
      "  @supports (display: grid) { .gap-4 { gap: 1.5rem; } }\n"
      "}";

  absl::flat_hash_set<RuleIdentity> force;
  force.insert(RuleIdentity{"", "", NormalizeSelector(".gap-4")});
  CriticalCssResult result = extractor.Extract(
      elements, css, CapabilityMask::Viewport::kDesktop, &force);

  EXPECT_TRUE(result.success);
  EXPECT_NE(result.critical_css.find("gap: 1rem"), std::string::npos);
  EXPECT_NE(result.critical_css.find("gap: 1.5rem"), std::string::npos)
      << "@supports is transparent for identity, so both copies are forced";
}

// Prefix matching would make `@importantly` an `@import` and retain an
// arbitrary rule wholesale. At-rule names are matched with an ident boundary.
TEST(CriticalCssExtractorTest, AtRuleMatchingRespectsIdentBoundaries) {
  CriticalCssExtractor extractor;
  std::vector<CollectedElement> elements;
  elements.push_back(MakeElement("div", 0, 0, "", {"box"}));

  std::string css =
      "@propertyish --nope { color: red; }\n"
      "@importantly { color: blue; }\n"
      "@property --real { syntax: \"*\"; inherits: false; }";
  CriticalCssResult result = extractor.Extract(elements, css);

  EXPECT_TRUE(result.success);
  EXPECT_EQ(result.critical_css.find("@propertyish"), std::string::npos)
      << "@propertyish is not @property";
  EXPECT_EQ(result.critical_css.find("@importantly"), std::string::npos)
      << "@importantly is not @import";
  EXPECT_NE(result.critical_css.find("@property --real"), std::string::npos)
      << "the real registration is still retained";
}

// ---------------------------------------------------------------------------
// Measured fold: the browser's per-viewport render replaces the
// "first 25 elements" estimate.
// ---------------------------------------------------------------------------

// The live repro, in one test.  modpagespeed.com's <head> spends the whole
// 25-element estimate before the first visible body element, so the utility
// classes that lay out the header (element index ~45) are judged below the fold
// and their rules are dropped from the critical block — the page paints
// unstyled until the deferred sheet lands.  A real browser render says those
// elements ARE above the fold; consulting it must admit them.
TEST(CriticalCssExtractorTest, MeasuredFoldSelectorsAdmitBodyLevelUtilities) {
  std::vector<CollectedElement> elements;
  elements.reserve(42);
  // 40 elements consumed before the content of interest. No <body> element is
  // collected here, so the budget counts from element 0, and a budget of 25
  // keeps index 44/45 outside the estimate.
  for (int i = 0; i < 40; ++i) {
    elements.push_back(MakeElement("meta", 2, i));
  }
  elements.push_back(MakeElement("div", 3, 44, "", {"flex", "h-16"}));
  elements.push_back(MakeElement("span", 4, 45, "", {"items-center"}));

  const std::string css =
      ".flex { display: flex; }\n"
      ".h-16 { height: 4rem; }\n"
      ".items-center { align-items: center; }\n";

  // Baseline: the index estimate alone drops all three.
  CriticalCssConfig legacy_config;
  legacy_config.max_elements = 25;
  CriticalCssExtractor legacy(legacy_config);
  CriticalCssResult before = legacy.Extract(elements, css);
  ASSERT_TRUE(before.success);
  EXPECT_EQ(before.critical_css.find("display: flex"), std::string::npos)
      << "precondition: the 25-element estimate excludes index 44/45";
  EXPECT_EQ(before.critical_rules, 0);

  CriticalCssConfig config;
  config.max_elements = 25;
  config.measured_above_fold_selectors = {".flex", ".h-16", ".items-center"};
  CriticalCssExtractor extractor(config);
  CriticalCssResult after = extractor.Extract(elements, css);

  ASSERT_TRUE(after.success);
  EXPECT_NE(after.critical_css.find("display: flex"), std::string::npos);
  EXPECT_NE(after.critical_css.find("height: 4rem"), std::string::npos);
  EXPECT_NE(after.critical_css.find("align-items: center"), std::string::npos);
  EXPECT_EQ(after.critical_rules, 3);
}

// The measured set is matched against the ELEMENT, not against the rule text:
// a token nothing in this document carries admits nothing.
TEST(CriticalCssExtractorTest, MeasuredFoldSelectorsMatchElementsNotRuleText) {
  std::vector<CollectedElement> elements;
  elements.reserve(42);
  for (int i = 0; i < 40; ++i) {
    elements.push_back(MakeElement("meta", 2, i));
  }
  elements.push_back(MakeElement("div", 3, 44, "", {"mt-96"}));

  const std::string css = ".flex { display: flex; }\n.mt-96 { margin: 24rem; }";

  CriticalCssConfig config;
  config.max_elements = 25;  // index 44 stays outside the estimate
  // Measured on some other page's fold; this document has no `.flex`.
  config.measured_above_fold_selectors = {".flex"};
  CriticalCssExtractor extractor(config);
  CriticalCssResult result = extractor.Extract(elements, css);

  ASSERT_TRUE(result.success);
  EXPECT_EQ(result.critical_css.find("display: flex"), std::string::npos)
      << "a measured token no element carries must not force its rule in";
  EXPECT_EQ(result.critical_css.find("margin: 24rem"), std::string::npos);
}

// An id token admits its element too, and matching is against the raw
// attribute value — the tokens carry DOM attribute text, not escaped CSS
// identifiers.
TEST(CriticalCssExtractorTest, MeasuredFoldMatchesIdsAndRawClassValues) {
  std::vector<CollectedElement> elements;
  elements.reserve(42);
  for (int i = 0; i < 40; ++i) {
    elements.push_back(MakeElement("meta", 2, i));
  }
  elements.push_back(MakeElement("div", 3, 44, "site-bar"));
  elements.push_back(MakeElement("a", 4, 45, "", {"dark:bg-stone-900"}));

  const std::string css =
      "#site-bar { position: sticky; }\n"
      ".dark\\:bg-stone-900 { background: #1c1917; }";

  CriticalCssConfig config;
  config.measured_above_fold_selectors = {"#site-bar", ".dark:bg-stone-900"};
  CriticalCssExtractor extractor(config);
  CriticalCssResult result = extractor.Extract(elements, css);

  ASSERT_TRUE(result.success);
  EXPECT_NE(result.critical_css.find("position: sticky"), std::string::npos);
  EXPECT_NE(result.critical_css.find("#1c1917"), std::string::npos)
      << "the class token is the raw DOM value; the sheet escapes the colon";
}

// Nothing measured (no browser profile, a failed render, a profile written
// before the fold was measured) must behave exactly as before.
TEST(CriticalCssExtractorTest, EmptyMeasuredFoldFallsBackToLegacyHeuristic) {
  std::vector<CollectedElement> elements;
  elements.push_back(MakeElement("div", 1, 3, "", {"intro"}));
  elements.push_back(MakeElement("div", 1, 44, "", {"flex"}));

  const std::string css = ".intro { color: red; }\n.flex { display: flex; }";

  CriticalCssConfig config;
  config.max_elements = 25;  // index 44 stays outside the estimate
  config.measured_above_fold_selectors = {};
  CriticalCssExtractor measured(config);
  CriticalCssResult with_empty = measured.Extract(elements, css);

  CriticalCssConfig legacy_config;
  legacy_config.max_elements = 25;
  CriticalCssExtractor legacy(legacy_config);
  CriticalCssResult baseline = legacy.Extract(elements, css);

  ASSERT_TRUE(with_empty.success);
  ASSERT_TRUE(baseline.success);
  EXPECT_EQ(with_empty.critical_css, baseline.critical_css);
  EXPECT_NE(with_empty.critical_css.find("color: red"), std::string::npos);
  EXPECT_EQ(with_empty.critical_css.find("display: flex"), std::string::npos);
}

// The universal selector is not a fold descriptor.  A garbage or hostile
// profile carrying "*" must not turn every element in the document into
// above-the-fold content.
TEST(CriticalCssExtractorTest, MeasuredFoldIgnoresWildcardAndEmptyTokens) {
  std::vector<CollectedElement> elements;
  elements.reserve(42);
  for (int i = 0; i < 40; ++i) {
    elements.push_back(MakeElement("meta", 2, i));
  }
  elements.push_back(MakeElement("div", 3, 44, "", {"mt-96"}));

  const std::string css = ".mt-96 { margin: 24rem; }";

  CriticalCssConfig config;
  config.max_elements = 25;  // index 44 stays outside the estimate
  config.measured_above_fold_selectors = {"*", "", "div"};
  CriticalCssExtractor extractor(config);
  CriticalCssResult result = extractor.Extract(elements, css);

  ASSERT_TRUE(result.success);
  EXPECT_EQ(result.critical_css.find("margin: 24rem"), std::string::npos)
      << "wildcard/empty/bare-tag tokens must admit nothing";
}

// ---------------------------------------------------------------------------
// The element budget is counted from <body>
// ---------------------------------------------------------------------------

// A document as scanned: <html>, <head> full of <meta>/<link>/<script>, then
// <body>. The estimate is spent on painted elements, so a head with more
// elements than the whole budget no longer leaves the fold uncovered — the
// failure the modpagespeed.com fixture showed (its <body> is element 42, and a
// budget of 25 counted from <html> admitted no body element at all).
TEST(CriticalCssExtractorTest, FoldBudgetIsCountedFromBody) {
  std::vector<CollectedElement> elements;
  elements.push_back(MakeElement("html", 0, 0));
  elements.push_back(MakeElement("head", 1, 1));
  for (int i = 2; i < 42; ++i) {
    elements.push_back(MakeElement("meta", 2, i));
  }
  elements.push_back(MakeElement("body", 1, 42, "", {"min-h-screen"}));
  elements.push_back(MakeElement("div", 2, 43, "", {"flex"}));
  elements.push_back(MakeElement("div", 2, 44, "", {"grid"}));

  const std::string css =
      ".min-h-screen { min-height: 100vh; }\n"
      ".flex { display: flex; }\n"
      ".grid { display: grid; }\n";

  CriticalCssConfig config;
  config.max_elements = 2;  // <body> and the first element after it
  CriticalCssExtractor extractor(config);
  CriticalCssResult result = extractor.Extract(elements, css);

  ASSERT_TRUE(result.success);
  EXPECT_NE(result.critical_css.find("min-height: 100vh"), std::string::npos)
      << "<body> is the first element of the budget";
  EXPECT_NE(result.critical_css.find("display: flex"), std::string::npos)
      << "the second body element is inside a budget of 2";
  EXPECT_EQ(result.critical_css.find("display: grid"), std::string::npos)
      << "the third body element is outside a budget of 2";
  EXPECT_EQ(result.critical_rules, 2);
}

// Elements before <body> are not in the budget, with one exception: the root
// <html> is a painted box and carries page-wide state classes — Tailwind puts
// `dark` on <html>, and the `.dark { --color-...: ... }` rule keyed on it is
// the whole dark palette — so it stays eligible. A class on a <head> child
// admits nothing: nothing in <head> paints.
TEST(CriticalCssExtractorTest, RootElementStaysEligibleHeadContentDoesNot) {
  std::vector<CollectedElement> elements;
  elements.push_back(MakeElement("html", 0, 0, "", {"dark"}));
  elements.push_back(MakeElement("head", 1, 1));
  elements.push_back(MakeElement("meta", 2, 2, "", {"head-only"}));
  for (int i = 3; i < 42; ++i) {
    elements.push_back(MakeElement("meta", 2, i));
  }
  elements.push_back(MakeElement("body", 1, 42));
  elements.push_back(MakeElement("div", 2, 43, "", {"card"}));

  const std::string css =
      ".dark { --color-bg-primary: #1c1917; }\n"
      ".head-only { color: red; }\n"
      ".card { padding: 1rem; }\n";

  CriticalCssConfig config;
  config.max_elements = 2;
  CriticalCssExtractor extractor(config);
  CriticalCssResult result = extractor.Extract(elements, css);

  ASSERT_TRUE(result.success);
  EXPECT_NE(result.critical_css.find("#1c1917"), std::string::npos)
      << "the palette keyed on <html class=\"dark\"> is retained";
  EXPECT_EQ(result.critical_css.find("color: red"), std::string::npos)
      << "a class on a <head> child must not admit its rule";
  EXPECT_NE(result.critical_css.find("padding: 1rem"), std::string::npos);
}

// A fragment with no <body> element (the shape most tests in this file use)
// counts from element 0, exactly as before.
TEST(CriticalCssExtractorTest, FoldBudgetCountsFromZeroWithoutBody) {
  std::vector<CollectedElement> elements;
  elements.push_back(MakeElement("div", 0, 0, "", {"first"}));
  elements.push_back(MakeElement("div", 0, 1, "", {"second"}));
  elements.push_back(MakeElement("div", 0, 2, "", {"third"}));

  const std::string css =
      ".first { color: red; }\n"
      ".second { color: green; }\n"
      ".third { color: blue; }\n";

  CriticalCssConfig config;
  config.max_elements = 2;
  CriticalCssExtractor extractor(config);
  CriticalCssResult result = extractor.Extract(elements, css);

  ASSERT_TRUE(result.success);
  EXPECT_NE(result.critical_css.find("color: red"), std::string::npos);
  EXPECT_NE(result.critical_css.find("color: green"), std::string::npos);
  EXPECT_EQ(result.critical_css.find("color: blue"), std::string::npos);
}

// The default budget is sized for markup as written today (see the field
// comment): a header, its hidden menus and a hero on a utility-class page run
// to a few hundred elements. Pinned so a change to it is a deliberate one.
TEST(CriticalCssExtractorTest, DefaultFoldBudgetCoversAFewHundredBodyElements) {
  CriticalCssConfig config;
  EXPECT_EQ(config.max_elements, 300);
}

// ---------------------------------------------------------------------------
// position:fixed anchors
// ---------------------------------------------------------------------------

// A chat launcher as the last child of <body>: far past any document-order
// estimate of the fold, yet on screen from the first paint because it is
// position:fixed. The element a fixed rule selects is critical, and so is
// everything inside it (the launcher's own rules never mention `fixed`). The
// next sibling after the fixed subtree is not.
TEST(CriticalCssExtractorTest, FixedPositionElementAtDocumentEndIsCritical) {
  std::vector<CollectedElement> elements;
  elements.push_back(MakeElement("body", 1, 0));
  for (int i = 1; i <= 40; ++i) {
    elements.push_back(MakeElement("p", 2, i, "", {"copy"}));
  }
  elements.push_back(MakeElement("div", 2, 41, "", {"chat-root"}));
  elements.push_back(MakeElement("button", 3, 42, "", {"chat-launcher"}));
  elements.push_back(MakeElement("svg", 4, 43, "", {"chat-icon"}));
  elements.push_back(MakeElement("div", 2, 44, "", {"after-chat"}));

  const std::string css =
      ".copy { color: red; }\n"
      ".chat-root { position: fixed; bottom: 1rem; right: 1rem; }\n"
      ".chat-launcher { width: 3rem; }\n"
      ".chat-icon { stroke: currentColor; }\n"
      ".after-chat { margin: 1px; }\n";

  CriticalCssConfig config;
  config.max_elements = 2;  // <body> and the first paragraph
  CriticalCssExtractor extractor(config);
  CriticalCssResult result = extractor.Extract(elements, css);

  ASSERT_TRUE(result.success);
  EXPECT_NE(result.critical_css.find("bottom: 1rem"), std::string::npos)
      << "the fixed element itself";
  EXPECT_NE(result.critical_css.find("width: 3rem"), std::string::npos)
      << "a child of the fixed element";
  EXPECT_NE(result.critical_css.find("stroke: currentColor"), std::string::npos)
      << "a grandchild of the fixed element";
  EXPECT_EQ(result.critical_css.find("margin: 1px"), std::string::npos)
      << "the sibling after the fixed subtree is outside the fold";
}

// The anchor rule can sit inside a cascade layer and a media block (Tailwind's
// `.fixed` lives in `@layer utilities`), and it matches the element by the
// final compound of a complex selector. The @media is evaluated against the
// viewport: the same rule anchors nothing on a viewport it cannot apply to.
TEST(CriticalCssExtractorTest, FixedAnchorIsFoundInsideLayersAndMedia) {
  std::vector<CollectedElement> elements;
  elements.push_back(MakeElement("body", 1, 0));
  for (int i = 1; i <= 40; ++i) {
    elements.push_back(MakeElement("p", 2, i));
  }
  elements.push_back(MakeElement("a", 2, 41, "sticky-cta", {"fixed", "btn"}));
  elements.push_back(MakeElement("div", 2, 42, "", {"cookie-banner"}));
  elements.push_back(MakeElement("p", 3, 43, "", {"cookie-text"}));

  const std::string css =
      "@layer utilities {\n"
      "  .fixed { position: fixed; }\n"
      "  .btn { padding: 1rem; }\n"
      "}\n"
      "@media (width >= 48rem) {\n"
      "  body > .cookie-banner { position:FIXED; inset: auto 0 0 0; }\n"
      "}\n"
      ".cookie-text { font-size: 12px; }\n";

  CriticalCssConfig config;
  config.max_elements = 2;
  CriticalCssExtractor extractor(config);
  CriticalCssResult desktop =
      extractor.Extract(elements, css, CapabilityMask::Viewport::kDesktop);

  ASSERT_TRUE(desktop.success);
  EXPECT_NE(desktop.critical_css.find("padding: 1rem"), std::string::npos)
      << "an element with the layered `.fixed` utility is anchored";
  EXPECT_NE(desktop.critical_css.find("inset: auto 0 0 0"), std::string::npos)
      << "a fixed rule inside @media anchors by its final compound";
  EXPECT_NE(desktop.critical_css.find("font-size: 12px"), std::string::npos)
      << "the banner's child comes along";

  CriticalCssResult mobile =
      extractor.Extract(elements, css, CapabilityMask::Viewport::kMobile);
  ASSERT_TRUE(mobile.success);
  EXPECT_NE(mobile.critical_css.find("padding: 1rem"), std::string::npos)
      << "the unconditional `.fixed` anchors on every viewport";
  EXPECT_EQ(mobile.critical_css.find("font-size: 12px"), std::string::npos)
      << "a banner fixed only from 48rem up is not on the mobile fold";
}

// A media query the evaluator cannot read keeps its RULES (retention is
// conservative) but anchors nothing: promoting an element on the strength of
// a condition nobody evaluated is the over-inclusion the anchoring must not
// commit.
TEST(CriticalCssExtractorTest, UnparseableMediaKeepsRulesButDoesNotAnchor) {
  std::vector<CollectedElement> elements;
  elements.push_back(MakeElement("body", 1, 0));
  elements.push_back(MakeElement("div", 2, 1, "", {"hero"}));
  for (int i = 2; i <= 40; ++i) {
    elements.push_back(MakeElement("p", 2, i));
  }
  elements.push_back(MakeElement("div", 2, 41, "", {"dock"}));
  elements.push_back(MakeElement("p", 3, 42, "", {"dock-text"}));

  const std::string css =
      "@media (orientation: landscape) {\n"
      "  .hero { color: red; }\n"
      "  .dock { position: fixed; bottom: 0; }\n"
      "}\n"
      ".dock-text { font-size: 11px; }\n";

  CriticalCssConfig config;
  config.max_elements = 2;
  CriticalCssExtractor extractor(config);
  CriticalCssResult result =
      extractor.Extract(elements, css, CapabilityMask::Viewport::kMobile);

  ASSERT_TRUE(result.success);
  EXPECT_NE(result.critical_css.find("color: red"), std::string::npos)
      << "retention keeps a rule under a query it cannot evaluate";
  EXPECT_EQ(result.critical_css.find("font-size: 11px"), std::string::npos)
      << "anchoring does not act on a query it cannot evaluate";
}

// ---------------------------------------------------------------------------
// Media Queries Level 4 range syntax
// ---------------------------------------------------------------------------

// Tailwind v4 emits `@media (width>=48rem)` (minified, no spaces) for its
// `md:` variants. RETENTION reads the range syntax exactly as it reads
// `min-width:`/`max-width:`, against the range of windows each variant can
// be served to (RetentionWidthRange: mobile 0-980, tablet 0-1480, desktop
// 0+), and drops a block only when no such window satisfies it. The sheet
// here names no cascade layer, so the block precedes it and a window outside
// the class range (a zoomed-out phone) lacks a dropped override only until
// the sheet applies.
TEST(CriticalCssExtractorTest,
     RangeSyntaxMediaQueriesAreEvaluatedForRetention) {
  std::vector<CollectedElement> elements;
  elements.push_back(MakeElement("div", 0, 0, "", {"a"}));

  const std::string css =
      "@media (width>=48rem) { .a { --md: 1; } }\n"
      "@media (width>=64rem) { .a { --lg: 1; } }\n"
      "@media (width>=96rem) { .a { --2xl: 1; } }\n"
      "@media (width<=420px) { .a { --narrow: 1; } }\n"
      "@media (width<20rem) { .a { --sub-phone: 1; } }\n"
      "@media (40rem <= width < 64rem) { .a { --tablet: 1; } }\n"
      "@media (min-width: 64rem) { .a { --legacy-wide: 1; } }\n"
      "@media (orientation: landscape) { .a { --unknown: 1; } }\n";

  CriticalCssExtractor extractor;
  auto has = [](const CriticalCssResult& r, const char* marker) {
    return r.critical_css.find(marker) != std::string::npos;
  };

  CriticalCssResult mobile =
      extractor.Extract(elements, css, CapabilityMask::Viewport::kMobile);
  ASSERT_TRUE(mobile.success);
  EXPECT_TRUE(has(mobile, "--md:")) << "768+: a phone in landscape";
  EXPECT_FALSE(has(mobile, "--lg:")) << "1024+: wider than any phone window";
  EXPECT_FALSE(has(mobile, "--2xl:"));
  EXPECT_TRUE(has(mobile, "--narrow:"));
  EXPECT_TRUE(has(mobile, "--sub-phone:")) << "KaiOS, fold covers, zoom";
  EXPECT_TRUE(has(mobile, "--tablet:"));
  EXPECT_FALSE(has(mobile, "--legacy-wide:"));
  EXPECT_TRUE(has(mobile, "--unknown:"))
      << "a query retention cannot read is still kept";

  CriticalCssResult tablet =
      extractor.Extract(elements, css, CapabilityMask::Viewport::kTablet);
  ASSERT_TRUE(tablet.success);
  EXPECT_TRUE(has(tablet, "--md:"));
  EXPECT_TRUE(has(tablet, "--lg:"));
  EXPECT_FALSE(has(tablet, "--2xl:")) << "1536+: wider than any tablet";
  EXPECT_TRUE(has(tablet, "--narrow:")) << "split-screen / Slide Over";
  EXPECT_TRUE(has(tablet, "--sub-phone:"));
  EXPECT_TRUE(has(tablet, "--tablet:"));
  EXPECT_TRUE(has(tablet, "--legacy-wide:"));

  CriticalCssResult desktop =
      extractor.Extract(elements, css, CapabilityMask::Viewport::kDesktop);
  ASSERT_TRUE(desktop.success);
  EXPECT_TRUE(has(desktop, "--md:"));
  EXPECT_TRUE(has(desktop, "--lg:"));
  EXPECT_TRUE(has(desktop, "--2xl:"));
  EXPECT_TRUE(has(desktop, "--narrow:"))
      << "a desktop User-Agent can have a phone-width window";
  EXPECT_TRUE(has(desktop, "--sub-phone:"));
  EXPECT_TRUE(has(desktop, "--tablet:"));
  EXPECT_TRUE(has(desktop, "--legacy-wide:"));
}

// The shape the async-css probe showed: it renders a 375 px window with a
// desktop User-Agent, so that window is served the DESKTOP block, and during
// the flash the block is the only CSS. A `(width<=420px)` override of a
// floating button must therefore ride along in the desktop block (dropping it
// put the buttons at desktop offsets: 0.00013 -> 0.02041 fold-pixel flash),
// and in the mobile block too. The same holds for the legacy `max-width:`
// spelling, which the desktop block used to drop.
TEST(CriticalCssExtractorTest, NarrowOverrideRidesAlongInEveryVariant) {
  std::vector<CollectedElement> elements;
  elements.push_back(MakeElement("body", 0, 0));
  elements.push_back(MakeElement("div", 1, 1, "", {"share-fab-root"}));
  elements.push_back(MakeElement("div", 1, 2, "", {"theme-fab"}));

  const std::string css =
      ".share-fab-root { position: fixed; bottom: 1.5rem; right: 1.5rem; }\n"
      ".theme-fab { position: fixed; bottom: 1.5rem; left: 1.5rem; }\n"
      "@media (width<=420px) {\n"
      "  .share-fab-root { bottom: .75rem; right: .75rem; }\n"
      "}\n"
      "@media (max-width: 420px) {\n"
      "  .theme-fab { bottom: .5rem; left: .5rem; }\n"
      "}\n";

  CriticalCssExtractor extractor;
  for (auto viewport :
       {CapabilityMask::Viewport::kMobile, CapabilityMask::Viewport::kTablet,
        CapabilityMask::Viewport::kDesktop}) {
    CriticalCssResult result = extractor.Extract(elements, css, viewport);
    ASSERT_TRUE(result.success);
    EXPECT_NE(result.critical_css.find("bottom: 1.5rem"), std::string::npos);
    EXPECT_NE(result.critical_css.find("right: .75rem"), std::string::npos)
        << "range-syntax override, viewport " << static_cast<int>(viewport);
    EXPECT_NE(result.critical_css.find("left: .5rem"), std::string::npos)
        << "max-width override, viewport " << static_cast<int>(viewport);
  }
}

// Tailwind v4's `hidden lg:block` on a layered sheet whose order is not proven
// (none is passed): the block follows the sheet, so the mobile
// block must keep the `lg:` override. A
// phone zoomed out past 1024 px would otherwise keep the element hidden for
// good, because the block's `.hidden` wins the tie against the sheet's.
// Print-only blocks still go.
TEST(CriticalCssExtractorTest, LayeredSheetKeepsWideOverridesOnMobile) {
  std::vector<CollectedElement> elements;
  elements.push_back(MakeElement("body", 0, 0));
  elements.push_back(MakeElement("nav", 1, 1, "", {"hidden", "lg:block"}));

  const std::string css =
      "@layer utilities {\n"
      "  .hidden { display: none; }\n"
      "  @media (width>=64rem) { .lg\\:block { display: block; } }\n"
      "  @media print { .hidden { display: block; } }\n"
      "}\n";

  CriticalCssExtractor extractor;
  CriticalCssResult mobile =
      extractor.Extract(elements, css, CapabilityMask::Viewport::kMobile);
  ASSERT_TRUE(mobile.success);
  EXPECT_NE(mobile.critical_css.find("display: none"), std::string::npos);
  EXPECT_NE(mobile.critical_css.find("width>=64rem"), std::string::npos)
      << "the lg: override rides along in a layered page's mobile block";
  EXPECT_EQ(mobile.critical_css.find("@media print"), std::string::npos);

  // The same sheet without the layer: the block precedes the sheet, and the
  // mobile block may drop what no phone window reaches.
  const std::string unlayered =
      ".hidden { display: none; }\n"
      "@media (width>=64rem) { .lg\\:block { display: block; } }\n";
  CriticalCssResult plain =
      extractor.Extract(elements, unlayered, CapabilityMask::Viewport::kMobile);
  ASSERT_TRUE(plain.success);
  EXPECT_EQ(plain.critical_css.find("width>=64rem"), std::string::npos);
}

// On a layered page whose order is proven, the block goes first
// behind the page's `@layer` statement and loses every tie to the sheet, so
// retention uses the device class's windows, as on an unlayered page: the
// mobile block drops `lg:` (64rem = 1024 px), the tablet block keeps it and
// drops `2xl:` (96rem = 1536 px), the desktop block keeps everything. A page
// whose order is not proven keeps every width on every class.
TEST(CriticalCssExtractorTest, ProvenLayerOrderNarrowsRetention) {
  std::vector<CollectedElement> elements;
  elements.push_back(MakeElement("body", 0, 0));
  elements.push_back(
      MakeElement("nav", 1, 1, "", {"hidden", "lg:block", "xxl:flex"}));

  const std::string css =
      "@layer theme, base, utilities;\n"
      "@layer base { nav { margin: 0; } }\n"
      "@layer utilities {\n"
      "  .hidden { display: none; }\n"
      "  @media (width>=64rem) { .lg\\:block { display: block; } }\n"
      "  @media (width>=96rem) { .xxl\\:flex { display: flex; } }\n"
      "}\n";

  CascadeLayerOrder proven;
  proven.proven = true;
  proven.reason.clear();
  proven.names = {"theme", "base", "utilities"};
  CascadeLayerOrder unproven;  // the default: not computed

  CriticalCssExtractor extractor;
  auto extract = [&](CapabilityMask::Viewport viewport,
                     const CascadeLayerOrder* order) {
    CriticalCssResult r = extractor.Extract(elements, css, viewport,
                                            /*force_include=*/nullptr, order);
    EXPECT_TRUE(r.success);
    EXPECT_NE(r.critical_css.find("display: none"), std::string::npos);
    return r;
  };

  CriticalCssResult mobile =
      extract(CapabilityMask::Viewport::kMobile, &proven);
  EXPECT_TRUE(mobile.class_range_retention);
  EXPECT_EQ(mobile.critical_css.find("width>=64rem"), std::string::npos)
      << "a proven page's mobile block drops what no phone window reaches";
  EXPECT_EQ(mobile.critical_css.find("width>=96rem"), std::string::npos);
  EXPECT_FALSE(DecideCriticalCssLayerPlacement(mobile.critical_css, proven)
                   .keep_fallback)
      << "the narrowed block is one that goes first";

  CriticalCssResult tablet =
      extract(CapabilityMask::Viewport::kTablet, &proven);
  EXPECT_TRUE(tablet.class_range_retention);
  EXPECT_NE(tablet.critical_css.find("width>=64rem"), std::string::npos)
      << "a landscape tablet (up to 1480 px) reaches lg:";
  EXPECT_EQ(tablet.critical_css.find("width>=96rem"), std::string::npos);

  CriticalCssResult desktop =
      extract(CapabilityMask::Viewport::kDesktop, &proven);
  EXPECT_TRUE(desktop.class_range_retention);
  EXPECT_NE(desktop.critical_css.find("width>=64rem"), std::string::npos);
  EXPECT_NE(desktop.critical_css.find("width>=96rem"), std::string::npos);

  // Not proven (or not passed at all): the block goes after the sheets, and
  // every class keeps every block that can match some window.
  for (auto viewport :
       {CapabilityMask::Viewport::kMobile, CapabilityMask::Viewport::kTablet,
        CapabilityMask::Viewport::kDesktop}) {
    for (const CascadeLayerOrder* order :
         {static_cast<const CascadeLayerOrder*>(&unproven),
          static_cast<const CascadeLayerOrder*>(nullptr)}) {
      CriticalCssResult r = extract(viewport, order);
      EXPECT_FALSE(r.class_range_retention);
      EXPECT_NE(r.critical_css.find("width>=64rem"), std::string::npos)
          << static_cast<int>(viewport);
      EXPECT_NE(r.critical_css.find("width>=96rem"), std::string::npos)
          << static_cast<int>(viewport);
    }
  }

  // Proven, but the order does not list a layer the sheet names (the walk
  // that proved it saw a different sheet): the block would take the fallback,
  // so retention keeps every width too.
  CascadeLayerOrder partial = proven;
  partial.names = {"theme", "base"};
  CriticalCssResult r = extract(CapabilityMask::Viewport::kMobile, &partial);
  EXPECT_FALSE(r.class_range_retention);
  EXPECT_NE(r.critical_css.find("width>=64rem"), std::string::npos);
}

// An anonymous layer is a NEW layer per `@layer { }`, so the
// block's anonymous layer is registered before the sheet's and is a different
// layer. CSS with any anonymous layer keeps every width, proven or not (the
// conservative choice; the block now also carries no
// `!important` inside such a layer, which was the reason).
TEST(CriticalCssExtractorTest, AnonymousLayerKeepsEveryWidthEvenWhenProven) {
  std::vector<CollectedElement> elements;
  elements.push_back(MakeElement("body", 0, 0));
  elements.push_back(MakeElement("div", 1, 1, "", {"x"}));

  struct Case {
    const char* css;
    std::vector<std::string> names;
  };
  const Case cases[] = {
      {"@layer { .x{display:none} @media (min-width:64rem){ "
       ".x{display:block} } }",
       {}},
      {"@layer base{ .x{color:red} } @layer { .x{display:none} "
       "@media (min-width:64rem){ .x{display:block} } }",
       {"base"}},
  };
  CriticalCssExtractor extractor;
  for (const Case& c : cases) {
    CascadeLayerOrder proven;
    proven.proven = true;
    proven.reason.clear();
    proven.names = c.names;
    ASSERT_TRUE(CriticalCssBlockGoesFirst(c.css, proven))
        << "precondition: the block goes first behind the statement: " << c.css;
    CriticalCssResult r =
        extractor.Extract(elements, c.css, CapabilityMask::Viewport::kMobile,
                          /*force_include=*/nullptr, &proven);
    ASSERT_TRUE(r.success);
    EXPECT_FALSE(r.class_range_retention) << c.css;
    EXPECT_NE(r.critical_css.find("display:none"), std::string::npos)
        << r.critical_css;
    EXPECT_NE(r.critical_css.find("min-width:64rem"), std::string::npos)
        << "the override rides along: " << r.critical_css;
  }
}

// Every `@layer { }` is a new layer, and the block's (placed
// first) comes before the sheet's. For `!important` the EARLIER layer wins, so
// an `!important` the block kept would beat the sheet's own `!important`
// override (`.x.open`, not on the fold) for good; placed after the sheet
// instead, the block's later layer would win every NORMAL declaration. So the
// block keeps its place and leaves out every `!important` declaration inside
// an anonymous layer, nested ones and every escape spelling included. Normal
// declarations of the same rule stay; a rule with nothing left goes.
// `!important` in a named layer or unlayered stays: the block and the sheet
// share those layers, and the later sheet wins either way.
TEST(CriticalCssExtractorTest, ImportantInsideAnAnonymousLayerIsLeftOut) {
  std::vector<CollectedElement> elements;
  elements.push_back(MakeElement("body", 0, 0));
  elements.push_back(MakeElement("div", 1, 1, "", {"x", "y", "z"}));
  CascadeLayerOrder proven;
  proven.proven = true;
  proven.reason.clear();
  proven.names = {"n"};
  CriticalCssExtractor extractor;
  auto block = [&](const std::string& css) {
    CriticalCssResult r =
        extractor.Extract(elements, css, CapabilityMask::Viewport::kDesktop,
                          /*force_include=*/nullptr, &proven);
    EXPECT_TRUE(r.success);
    return r.critical_css;
  };

  // The issue's sheet: the `.x` rule goes, `.y` keeps its normal declaration.
  std::string b = block(
      "@layer { .x{display:none!important} .y{color:red;margin:0 !important} "
      "}");
  EXPECT_EQ(b.find("important"), std::string::npos) << b;
  EXPECT_EQ(b.find("display:none"), std::string::npos) << b;
  EXPECT_NE(b.find("color:red"), std::string::npos) << b;

  // Every spelling Chromium reads as !important, and nesting.
  for (const char* css : {
           "@layer { .x{color:red !IMPORTANT} }",
           "@layer { .x{color:red ! important} }",
           "@layer { .x{color:red !/**/important} }",
           "@layer { .x{color:red !\\69mportant} }",
           "@layer { .x{color:red !i\\6dportant} }",
           "@layer { .x{color:red !imp\\ortant} }",
           "@layer { @media (min-width:1px){ .x{color:red!important} } }",
           "@layer n{ @layer { .x{color:red!important} } }",
           "@layer { @supports (display:grid){ .x{color:red!important} } }",
           "@layer/**/{ .x{color:red!important} }",
       }) {
    b = block(css);
    EXPECT_EQ(b.find("color:red"), std::string::npos) << css << "\n" << b;
  }

  // A rule OUTSIDE an anonymous layer that CONTAINS one is
  // filtered too. A small @media copied whole, a nested style rule, and the
  // other group rules copied whole.
  for (const char* css : {
           "@layer { .x{display:block!important} } @media (min-width:1px){ "
           "@layer { .x{display:none!important} } }",
           ".x { @layer { display:none!important } }",
           "@container (min-width:1px){ @layer { .x{display:none!important} } "
           "}",
           "@scope (body){ @layer { .x{display:none!important} } }",
           "@starting-style { @layer { .x{display:none!important} } }",
           "@media (min-width:1px){ @layer/**/{ .x{display:none!important} } "
           "}",
       }) {
    b = block(css);
    EXPECT_EQ(b.find("none"), std::string::npos) << css << "\n" << b;
    EXPECT_EQ(b.find("important"), std::string::npos) << css << "\n" << b;
  }
  // ... but only inside the anonymous layer: a named layer in the same
  // wholesale @media keeps its `!important`.
  b = block(
      "@media (min-width:1px){ @layer n { .x{color:red!important} } "
      "@layer { .x{display:none!important} } }");
  EXPECT_NE(b.find("color:red!important"), std::string::npos) << b;
  EXPECT_EQ(b.find("none"), std::string::npos) << b;

  // On the CSS Syntax 3 scanners: an escaped quote in a selector does
  // not hide the rule after it, and a `;` inside a string, an escape or an
  // unquoted url() does not split a declaration.
  b = block(
      "@layer { .a\\'b{color:blue} .x{display:none!important;color:red} }");
  EXPECT_EQ(b.find("none"), std::string::npos) << b;
  EXPECT_NE(b.find("color:red"), std::string::npos) << b;
  b = block(
      "@layer { .x{background:url(a;b.png);content:\"x;y\";"
      "--v:a\\;b;display:none!important} }");
  EXPECT_NE(b.find("url(a;b.png)"), std::string::npos) << b;
  EXPECT_NE(b.find("content:\"x;y\""), std::string::npos) << b;
  EXPECT_NE(b.find("--v:a\\;b"), std::string::npos) << b;
  EXPECT_EQ(b.find("none"), std::string::npos) << b;

  // An anonymous `@import ... layer` is left out; a named one stays.
  b = block("@import url(a.css) layer; @import url(b.css) layer(n); .x{x:y}");
  EXPECT_EQ(b.find("a.css"), std::string::npos) << b;
  EXPECT_NE(b.find("b.css"), std::string::npos) << b;

  // Kept: named layer, unlayered, a `!` that is not !important, and a string.
  for (const char* css : {
           "@layer n{ .x{color:red!important} }",
           ".x{color:red!important}",
           "@layer { .x{color:red; --bang:\"a;b\"} }",
       }) {
    b = block(css);
    EXPECT_NE(b.find("color:red"), std::string::npos) << css << "\n" << b;
  }
}

// The same holds without a proven order, where the block goes after the
// sheets: CSS that uses only anonymous layers (no named one) keeps every width.
// Before the retention narrowing retention asked only whether the CSS NAMES a
// layer, and
// narrowed such a page's block while placement put it after the sheets. The
// anonymous layer's own rules are left out there, so the width
// rule shows on an unlayered breakpoint next to it.
TEST(CriticalCssExtractorTest,
     AnonymousOnlyCssWithUnprovenOrderKeepsEveryWidth) {
  std::vector<CollectedElement> elements;
  elements.push_back(MakeElement("body", 0, 0));
  elements.push_back(MakeElement("nav", 1, 1, "", {"hidden", "lg:block"}));
  const std::string css =
      "@layer { .hidden { display: none; } }\n"
      "@media (width>=64rem) { .lg\\:block { display: block; } }\n";
  CascadeLayerOrder unproven;
  CriticalCssExtractor extractor;
  for (const CascadeLayerOrder* order :
       {static_cast<const CascadeLayerOrder*>(&unproven),
        static_cast<const CascadeLayerOrder*>(nullptr)}) {
    CriticalCssResult r =
        extractor.Extract(elements, css, CapabilityMask::Viewport::kMobile,
                          /*force_include=*/nullptr, order);
    ASSERT_TRUE(r.success);
    EXPECT_FALSE(r.class_range_retention);
    EXPECT_TRUE(r.anonymous_layers_dropped);
    EXPECT_NE(r.critical_css.find("width>=64rem"), std::string::npos)
        << r.critical_css;
    EXPECT_EQ(r.critical_css.find("display: none"), std::string::npos)
        << r.critical_css;
  }
}

// When the block goes after the sheets (the order is not
// proven, or does not list a layer the sheet names), its anonymous layer is
// registered after the sheet's and wins every NORMAL declaration over any
// specificity: the block's `.x{display:none}` beat the sheet's
// `.x.open{display:block}` for good. Such a block leaves every rule inside an
// anonymous layer out, nested ones and those in rules copied whole included,
// and keeps named-layer and unlayered rules. A block that goes first keeps
// them (only their `!important` goes).
TEST(CriticalCssExtractorTest, BlockAfterTheSheetsCarriesNoAnonymousLayer) {
  std::vector<CollectedElement> elements;
  elements.push_back(MakeElement("body", 0, 0));
  elements.push_back(MakeElement("div", 1, 1, "", {"x", "y"}));
  CascadeLayerOrder unproven;
  CascadeLayerOrder missing;  // proven, but without the sheet's `n`
  missing.proven = true;
  missing.reason.clear();
  missing.names = {"other"};
  CascadeLayerOrder proven = missing;
  proven.names = {"n"};
  CriticalCssExtractor extractor;
  auto block = [&](const std::string& css, const CascadeLayerOrder& order) {
    CriticalCssResult r =
        extractor.Extract(elements, css, CapabilityMask::Viewport::kDesktop,
                          /*force_include=*/nullptr, &order);
    EXPECT_TRUE(r.success);
    return r;
  };

  const std::string issue =
      "@layer n{ .y{color:red} } @layer { .x{display:none} "
      ".x.open{display:block} } .y{margin:0}";
  for (const CascadeLayerOrder* order : {&unproven, &missing}) {
    CriticalCssResult r = block(issue, *order);
    EXPECT_TRUE(r.anonymous_layers_dropped);
    EXPECT_EQ(r.critical_css.find("display:none"), std::string::npos)
        << r.critical_css;
    EXPECT_NE(r.critical_css.find("color:red"), std::string::npos)
        << "a named layer stays: " << r.critical_css;
    EXPECT_NE(r.critical_css.find("margin:0"), std::string::npos)
        << "an unlayered rule stays: " << r.critical_css;
  }
  CriticalCssResult first = block(issue, proven);
  EXPECT_FALSE(first.anonymous_layers_dropped);
  EXPECT_NE(first.critical_css.find("display:none"), std::string::npos)
      << "a block that goes first keeps its anonymous layer: "
      << first.critical_css;

  // Anonymous layers inside rules copied whole, nested, and @layer/**/{.
  for (const char* css : {
           "@layer n{} @media (min-width:1px){ @layer { .x{display:none} } }",
           "@layer n{} .x { @layer { display:none } }",
           "@layer n{} @layer n { @layer { .x{display:none} } }",
           "@layer n{} @layer/**/{ .x{display:none} }",
       }) {
    CriticalCssResult r = block(css, unproven);
    EXPECT_EQ(r.critical_css.find("display:none"), std::string::npos)
        << css << "\n"
        << r.critical_css;
  }
  // The combined sheet goes first, the finished block does not: a rule the
  // block leaves out puts the always-kept `@import ... layer(lib)` first,
  // where it names `lib`, which the order lacks. Checked on the block, and
  // derived again without anonymous layers.
  const std::string promoted =
      ".not-on-page{color:blue} @import url(lib.css) layer(lib); "
      "@layer n{.y{color:red}} @layer { .x{display:none} }";
  ASSERT_TRUE(CriticalCssBlockGoesFirst(promoted, proven));
  CriticalCssResult redone = block(promoted, proven);
  ASSERT_TRUE(DecideCriticalCssLayerPlacement(redone.critical_css, proven)
                  .keep_fallback)
      << "precondition: the block goes after the sheets: "
      << redone.critical_css;
  EXPECT_TRUE(redone.anonymous_layers_dropped);
  EXPECT_EQ(redone.critical_css.find("display:none"), std::string::npos)
      << redone.critical_css;

  // An anonymous `@import ... layer` is left out as well (it already is).
  CriticalCssResult r =
      block("@import url(a.css) layer; @layer n{.x{color:red}}", unproven);
  EXPECT_EQ(r.critical_css.find("a.css"), std::string::npos) << r.critical_css;
}

// `@l\61yer {` is an anonymous @layer to a browser. The
// placement side reads an at-rule keyword written with an escape as a possible
// layer (CriticalCssMayUseCascadeLayer), so a block holding one always goes
// after the sheets, where an anonymous layer wins every normal declaration.
// The drop path and the `!important` strip matched the literal
// spelling `@layer` only, so the escaped one reached the block: inside an
// @media copied whole (the reported sheet), inside one filtered per rule (a
// rule without a matchable token is kept there), nested in a style rule. Such
// a block carries none of them, and the derivation says so
// (anonymous_layers_dropped), with no order and with a computed one alike,
// because the escape alone keeps the block after the sheets.
TEST(CriticalCssExtractorTest, EscapedAtKeywordIsLeftOutAfterTheSheets) {
  std::vector<CollectedElement> elements;
  elements.push_back(MakeElement("body", 0, 0));
  elements.push_back(MakeElement("nav", 1, 1, "", {"hidden"}));
  CascadeLayerOrder unproven;
  CascadeLayerOrder proven;  // lists `base`; the escape still refuses the block
  proven.proven = true;
  proven.reason.clear();
  proven.names = {"base"};
  ASSERT_FALSE(CriticalCssBlockGoesFirst("@l\\61yer{.x{}}", proven));
  CriticalCssConfig per_rule;
  per_rule.max_wholesale_media_bytes = 1;  // every @media filtered per rule
  CriticalCssExtractor whole;
  CriticalCssExtractor filtered(per_rule);
  auto block = [&](CriticalCssExtractor& extractor, const std::string& css,
                   const CascadeLayerOrder& order) {
    CriticalCssResult r =
        extractor.Extract(elements, css, CapabilityMask::Viewport::kMobile,
                          /*force_include=*/nullptr, &order);
    EXPECT_TRUE(r.success);
    return r;
  };

  // The issue's sheet (confirmed in Chromium: spliced after the sheet, the
  // block's `.hidden{display:none}` beat the sheet's `.hidden.open`).
  const std::string issue =
      "@layer base{nav{margin:0}}\n"
      "@media screen { @l\\61yer { .hidden{display:none} } }\n"
      "@l\\61yer { .hidden.open{display:block} }";
  for (const CascadeLayerOrder* order : {&unproven, &proven}) {
    for (CriticalCssExtractor* extractor : {&whole, &filtered}) {
      CriticalCssResult r = block(*extractor, issue, *order);
      EXPECT_TRUE(r.anonymous_layers_dropped);
      EXPECT_EQ(r.critical_css.find("61yer"), std::string::npos)
          << r.critical_css;
      EXPECT_EQ(r.critical_css.find("display:"), std::string::npos)
          << r.critical_css;
      EXPECT_NE(r.critical_css.find("margin:0"), std::string::npos)
          << "the named layer stays: " << r.critical_css;
    }
  }

  // Per-rule @media filtering keeps a rule without a matchable token; not
  // this one. The unlayered rule next to it stays.
  CriticalCssResult r =
      block(filtered,
            "@layer base{} @media screen { @l\\61yer { .hidden{display:none} } "
            "nav{margin:0} }",
            unproven);
  EXPECT_TRUE(r.anonymous_layers_dropped);
  EXPECT_NE(r.critical_css.find("margin:0"), std::string::npos)
      << r.critical_css;
  EXPECT_EQ(r.critical_css.find("display:none"), std::string::npos)
      << r.critical_css;

  // Nested in a style rule (CSS nesting): the rule goes whole, as a rule
  // nesting `@layer {` does. An `!important` inside goes with it (the
  // strip reads the keyword as written too). The other spellings of the
  // escape count as well, and so does a named escaped layer: the keyword is
  // not decoded, so it may be anonymous. A `;` in a comment or a string
  // before the `{` does not make the rule a statement.
  for (const char* css : {
           "@layer b{} nav { @l\\61yer { display:none } }",
           "@layer b{} @l\\61yer { .hidden{display:none!important} }",
           "@layer b{} @\\6c ayer { .hidden{display:none} }",
           "@layer b{} @l\\61yer other { .hidden{display:none} }",
           "@layer b{} @media all { @l\\61yer/**/{ .hidden{display:none} } }",
           "@layer b{} @media all { @l\\61yer/*;*/{ .hidden{display:none} } }",
           "@layer b{} nav { @l\\61yer/*;*/{ display:none } }",
           "@layer b{} @l\\61yer /* a; */ { .hidden{display:none} }",
           "@layer b{} @l\\61yer ';' { .hidden{display:none} }",
       }) {
    for (CriticalCssExtractor* extractor : {&whole, &filtered}) {
      r = block(*extractor, css, unproven);
      EXPECT_TRUE(r.anonymous_layers_dropped) << css;
      EXPECT_EQ(r.critical_css.find("display:none"), std::string::npos)
          << css << "\n"
          << r.critical_css;
    }
  }

  // A sheet with nothing but an anonymous layer, in either spelling, derives
  // an EMPTY block under an unproven order, and the derivation says why.
  for (const char* css : {
           "@l\\61yer { .hidden{display:none} nav{margin:0} }",
           "@layer { .hidden{display:none} nav{margin:0} }",
       }) {
    r = block(whole, css, unproven);
    EXPECT_TRUE(r.success) << css;
    EXPECT_TRUE(r.critical_css.empty()) << css << "\n" << r.critical_css;
    EXPECT_TRUE(r.anonymous_layers_dropped) << css;
  }
  // Unlayered and named-layer rules do not set the flag.
  r = block(whole, "@layer base{nav{margin:0}} nav{color:red}", unproven);
  EXPECT_FALSE(r.anonymous_layers_dropped);
  EXPECT_NE(r.critical_css.find("margin:0"), std::string::npos);
}

// The narrowing is decided on the combined sheet, then confirmed on the block
// with the placement rule itself. Here the two disagree: the sheet's
// `@import ... layer(lib)` follows a rule, so a browser ignores it and the
// proven order does not list `lib`; the block leaves that rule out, which
// puts the always-kept @import first, where it names `lib`. That block takes
// the fallback after the sheets, so it is extracted again for every width.
TEST(CriticalCssExtractorTest, NarrowedBlockThatWouldNotGoFirstIsRedone) {
  std::vector<CollectedElement> elements;
  elements.push_back(MakeElement("body", 0, 0));
  elements.push_back(MakeElement("nav", 1, 1, "", {"hidden", "lg:block"}));

  const std::string css =
      ".not-on-page { color: red; }\n"
      "@import url(lib.css) layer(lib);\n"
      "@layer utilities {\n"
      "  .hidden { display: none; }\n"
      "  @media (width>=64rem) { .lg\\:block { display: block; } }\n"
      "}\n";
  CascadeLayerOrder proven;
  proven.proven = true;
  proven.reason.clear();
  proven.names = {"utilities"};
  ASSERT_TRUE(CriticalCssBlockGoesFirst(css, proven))
      << "precondition: the sheet alone would allow narrowing";

  CriticalCssExtractor extractor;
  CriticalCssResult r =
      extractor.Extract(elements, css, CapabilityMask::Viewport::kMobile,
                        /*force_include=*/nullptr, &proven);
  ASSERT_TRUE(r.success);
  ASSERT_TRUE(
      DecideCriticalCssLayerPlacement(r.critical_css, proven).keep_fallback)
      << "precondition: the block names `lib`, so it goes after the sheets";
  EXPECT_FALSE(r.class_range_retention);
  EXPECT_NE(r.critical_css.find("width>=64rem"), std::string::npos)
      << "a block that goes after the sheets keeps every width";
}

// A small @media block is left out only when every rule in it has a tag, id
// or class token and none matched. A rule the matcher cannot match on (no such
// token) keeps its block whole, as before per-rule filtering. The theme-toggle
// dark palette is the case that matters: dropping it gives dark-mode visitors
// the light palette until the sheet applies.
TEST(CriticalCssExtractorTest, SmallMediaBlockWithUnmatchableRuleIsKept) {
  const std::string css =
      "@media (prefers-color-scheme: dark) {\n"
      "  :root:not([data-theme=light]) { --bg: #000; --fg: #eee; }\n"
      "}\n"
      "@media (prefers-color-scheme: dark) {\n"
      "  [data-theme=auto] { --accent: #0af; }\n"
      "}\n"
      "@media (min-width: 40rem) {\n"
      "  :root[data-theme=auto] { --gutter: 2rem; }\n"
      "}\n"
      "@media (min-width: 40rem) {\n"
      "  .not-on-page { margin: 0; }\n"
      "  .also-not-on-page, #nope { padding: 0; }\n"
      "}\n";

  std::vector<CollectedElement> with_html;
  with_html.push_back(MakeElement("html", 0, 0));
  with_html.push_back(MakeElement("body", 1, 1));
  with_html.push_back(MakeElement("div", 2, 2, "", {"card"}));
  std::vector<CollectedElement> body_only;
  body_only.push_back(MakeElement("body", 0, 0));

  CriticalCssExtractor extractor;
  for (const auto* elements : {&with_html, &body_only}) {
    for (auto viewport :
         {CapabilityMask::Viewport::kMobile, CapabilityMask::Viewport::kTablet,
          CapabilityMask::Viewport::kDesktop}) {
      CriticalCssResult result = extractor.Extract(*elements, css, viewport);
      ASSERT_TRUE(result.success);
      EXPECT_NE(result.critical_css.find("--bg: #000"), std::string::npos)
          << "the dark palette is kept, viewport "
          << static_cast<int>(viewport);
      EXPECT_NE(result.critical_css.find("--accent"), std::string::npos)
          << "[data-theme=auto] in @media is kept";
      EXPECT_NE(result.critical_css.find("--gutter"), std::string::npos)
          << ":root[data-theme=auto] in @media is kept";
      EXPECT_EQ(result.critical_css.find("not-on-page"), std::string::npos)
          << "a block of class/id rules none of which matched is left out";
    }
  }
}

// The same rule inside a LARGE @media block, which is filtered per rule: the
// palette rule without a tag, id or class token is kept, the unmatched class
// rules are dropped.
TEST(CriticalCssExtractorTest, LargeMediaBlockKeepsUnmatchableRules) {
  std::string inner = "  [data-theme=dark] { --bg: #000; }\n";
  for (int i = 0; i < 200; ++i) {
    inner += "  .unused-" + std::to_string(i) +
             " { margin: " + std::to_string(i) + "px; }\n";
  }
  inner += "  ::selection { color: #fff; }\n";
  const std::string css =
      "@media (prefers-color-scheme: dark) {\n" + inner + "}\n";
  ASSERT_GT(css.size(), 4096U);

  std::vector<CollectedElement> elements;
  elements.push_back(MakeElement("body", 0, 0));
  CriticalCssExtractor extractor;
  for (auto viewport : {CapabilityMask::Viewport::kMobile,
                        CapabilityMask::Viewport::kDesktop}) {
    CriticalCssResult result = extractor.Extract(elements, css, viewport);
    ASSERT_TRUE(result.success);
    EXPECT_NE(result.critical_css.find("--bg: #000"), std::string::npos)
        << "the [data-theme=dark] palette rule is kept";
    EXPECT_NE(result.critical_css.find("::selection"), std::string::npos);
    EXPECT_EQ(result.critical_css.find(".unused-"), std::string::npos)
        << "unmatched class rules are filtered out";
  }
  // Layered, the same.
  const std::string layered = "@layer theme {\n" + css + "}\n";
  CriticalCssResult result =
      extractor.Extract(elements, layered, CapabilityMask::Viewport::kMobile);
  ASSERT_TRUE(result.success);
  EXPECT_NE(result.critical_css.find("--bg: #000"), std::string::npos);
  EXPECT_EQ(result.critical_css.find(".unused-"), std::string::npos);
}

// A leading `:root` matches <html>, so `:root:not(...)` is retained where
// `html:not(...)` would be, at the top level too.
TEST(CriticalCssExtractorTest, RootPseudoMatchesHtml) {
  std::vector<CollectedElement> elements;
  elements.push_back(MakeElement("html", 0, 0));
  elements.push_back(MakeElement("body", 1, 1));
  const std::string css =
      ":root:not([data-theme=light]) { --x: 1; }\n"
      ":rooted { --y: 1; }\n";
  CriticalCssExtractor extractor;
  CriticalCssResult result = extractor.Extract(elements, css);
  ASSERT_TRUE(result.success);
  EXPECT_NE(result.critical_css.find("--x: 1"), std::string::npos);
  EXPECT_EQ(result.critical_css.find("--y: 1"), std::string::npos)
      << "`:rooted` is not `:root`";
}

// Anchoring reads `not` too: `not all and (min-width: 1024px)` is every width
// below 1024, so a widget fixed under it anchors on mobile (0-479) and not on
// desktop (1280+). Before the range-syntax change the query was unreadable and
// anchored
// nothing, on any viewport.
TEST(CriticalCssExtractorTest, AnchoringReadsNotQueries) {
  std::vector<CollectedElement> elements;
  elements.push_back(MakeElement("body", 1, 0));
  for (int i = 1; i <= 40; ++i) {
    elements.push_back(MakeElement("p", 2, i));
  }
  elements.push_back(MakeElement("div", 2, 41, "", {"dock"}));
  elements.push_back(MakeElement("p", 3, 42, "", {"dock-text"}));

  const std::string css =
      "@media not all and (min-width:1024px) {\n"
      "  .dock { position: fixed; bottom: 0; }\n"
      "}\n"
      ".dock-text { --dt: 1; }\n";

  CriticalCssConfig config;
  config.max_elements = 2;
  CriticalCssExtractor extractor(config);
  CriticalCssResult mobile =
      extractor.Extract(elements, css, CapabilityMask::Viewport::kMobile);
  ASSERT_TRUE(mobile.success);
  EXPECT_NE(mobile.critical_css.find("--dt:"), std::string::npos)
      << "the dock is fixed below 1024 px: anchored on mobile";
  CriticalCssResult desktop =
      extractor.Extract(elements, css, CapabilityMask::Viewport::kDesktop);
  ASSERT_TRUE(desktop.success);
  EXPECT_EQ(desktop.critical_css.find("--dt:"), std::string::npos)
      << "not fixed at 1280+: not anchored on desktop";
}

// ---------------------------------------------------------------------------
// EvaluateMediaWidth: the media-query width evaluator itself
// ---------------------------------------------------------------------------

namespace {

MediaWidthMatch Eval(std::string_view media, uint32_t lo, uint32_t hi) {
  return EvaluateMediaWidth(media, MediaWidthRange{lo, hi});
}

constexpr auto kNever = MediaWidthMatch::kNever;
constexpr auto kApplies = MediaWidthMatch::kApplies;
constexpr auto kUnknown = MediaWidthMatch::kUnknown;

}  // namespace

TEST(MediaWidthEvaluatorTest, RetentionRangesOverlapAndStartAt0) {
  MediaWidthRange m = RetentionWidthRange(CapabilityMask::Viewport::kMobile);
  MediaWidthRange t = RetentionWidthRange(CapabilityMask::Viewport::kTablet);
  MediaWidthRange d = RetentionWidthRange(CapabilityMask::Viewport::kDesktop);
  EXPECT_EQ(m.min_px, 0U);
  EXPECT_EQ(m.max_px, 980U);
  EXPECT_EQ(t.min_px, 0U);
  EXPECT_EQ(t.max_px, 1480U);
  EXPECT_EQ(d.min_px, 0U);
  EXPECT_EQ(d.max_px, kMediaWidthUnbounded);
  // Every width the browser analysis renders a class at is inside its range.
  EXPECT_LE(m.min_px, 375U);
  EXPECT_GE(m.max_px, 375U);
  EXPECT_LE(t.min_px, 768U);
  EXPECT_GE(t.max_px, 1024U);
  EXPECT_GE(d.max_px, 1440U);
}

// CSS that uses a cascade layer is retained against every width unless its
// block goes first: after the sheets, a dropped override
// that applies to any window loses to the kept base rule for good.
TEST(MediaWidthEvaluatorTest, LayeredCssIsRetainedForEveryWidthUnlessFirst) {
  CascadeLayerOrder unknown;
  CascadeLayerOrder proven;
  proven.proven = true;
  proven.reason.clear();
  proven.names = {"base", "theme"};
  for (auto viewport :
       {CapabilityMask::Viewport::kMobile, CapabilityMask::Viewport::kTablet,
        CapabilityMask::Viewport::kDesktop}) {
    const MediaWidthRange c = RetentionWidthRange(viewport);
    // Not proven: every width, for named and anonymous layers alike.
    for (const char* css : {"@layer base { a { color: red; } }", "@layer a, b;",
                            "@import url(x.css) layer(base);", "@LAYER theme{}",
                            "@layer { a { color: red; } }"}) {
      MediaWidthRange r = RetentionWidthRangeForCss(viewport, css, unknown);
      EXPECT_EQ(r.min_px, 0U) << css;
      EXPECT_EQ(r.max_px, kMediaWidthUnbounded) << css;
    }
    // Proven, and the order lists every layer the CSS names: the class range.
    for (const char* css :
         {"@layer base { a { color: red; } }", "@LAYER theme{}",
          "@layer base, theme; @layer theme { a { color: red; } }"}) {
      MediaWidthRange r = RetentionWidthRangeForCss(viewport, css, proven);
      EXPECT_EQ(r.min_px, c.min_px) << css;
      EXPECT_EQ(r.max_px, c.max_px) << css;
    }
    // Proven, but the CSS names a layer the order does not list: the block
    // would go after the sheets, so every width.
    for (const char* css : {"@layer a, b;", "@layer base.inner { a{} }"}) {
      MediaWidthRange r = RetentionWidthRangeForCss(viewport, css, proven);
      EXPECT_EQ(r.max_px, kMediaWidthUnbounded) << css;
    }
    // Proven, but with an anonymous layer (the earlier layer wins for
    // !important): every width.
    for (const char* css : {"@layer base, theme; @layer { a { color: red; } }",
                            "@layer base { @layer { a { color: red; } } }",
                            "@import url(x.css) layer; @layer base{}"}) {
      bool class_range = true;
      MediaWidthRange r =
          RetentionWidthRangeForCss(viewport, css, proven, &class_range);
      EXPECT_EQ(r.max_px, kMediaWidthUnbounded) << css;
      EXPECT_FALSE(class_range) << css;
    }
    // No layer: the class range whatever the order.
    for (const auto* order : {&unknown, &proven}) {
      for (const char* css : {"a { color: red; }",
                              "@media (min-width: 1px) { .layer { x: y; } }"}) {
        MediaWidthRange r = RetentionWidthRangeForCss(viewport, css, *order);
        EXPECT_EQ(r.min_px, c.min_px) << css;
        EXPECT_EQ(r.max_px, c.max_px) << css;
      }
    }
  }
}

TEST(MediaWidthEvaluatorTest, LegacyMinMaxWidth) {
  EXPECT_EQ(Eval("@media (min-width: 768px)", 320, 767), kNever);
  EXPECT_EQ(Eval("@media (min-width: 768px)", 320, 768), kApplies);
  EXPECT_EQ(Eval("@media (max-width: 420px)", 421, 2000), kNever);
  EXPECT_EQ(Eval("@media (max-width: 420px)", 420, 2000), kApplies);
  EXPECT_EQ(Eval("@media (max-width : 420px)", 421, 2000), kNever);
  EXPECT_EQ(Eval("@media (min-width:768px) and (max-width:1023px)", 1024, 2000),
            kNever);
  EXPECT_EQ(Eval("@media (min-width:768px) and (max-width:1023px)", 320, 768),
            kApplies);
  EXPECT_EQ(Eval("@media (width: 600px)", 320, 599), kNever);
  EXPECT_EQ(Eval("@media (width: 600px)", 600, 600), kApplies);
  EXPECT_EQ(Eval("@MEDIA SCREEN AND (MIN-WIDTH: 64EM)", 320, 1023), kNever);
}

TEST(MediaWidthEvaluatorTest, RangeSyntaxOneSided) {
  // >= and <=: inclusive.
  EXPECT_EQ(Eval("@media (width>=48rem)", 320, 767), kNever);
  EXPECT_EQ(Eval("@media (width>=48rem)", 320, 768), kApplies);
  EXPECT_EQ(Eval("@media (width <= 420px)", 421, 5000), kNever);
  EXPECT_EQ(Eval("@media (width <= 420px)", 420, 5000), kApplies);
  // > and <: strict, folded to whole pixels.
  EXPECT_EQ(Eval("@media (width > 768px)", 320, 768), kNever);
  EXPECT_EQ(Eval("@media (width > 768px)", 320, 769), kApplies);
  EXPECT_EQ(Eval("@media (width < 768px)", 768, 5000), kNever);
  EXPECT_EQ(Eval("@media (width < 768px)", 767, 5000), kApplies);
  EXPECT_EQ(Eval("@media (width < 767.5px)", 767, 767), kApplies);
  EXPECT_EQ(Eval("@media (width > 767.5px)", 767, 767), kNever);
  // Value on the left.
  EXPECT_EQ(Eval("@media (768px <= width)", 320, 767), kNever);
  EXPECT_EQ(Eval("@media (768px < width)", 320, 768), kNever);
  EXPECT_EQ(Eval("@media (768px < width)", 320, 769), kApplies);
  EXPECT_EQ(Eval("@media (420px >= width)", 421, 5000), kNever);
  EXPECT_EQ(Eval("@media (420px > width)", 420, 5000), kNever);
  EXPECT_EQ(Eval("@media (420px > width)", 419, 5000), kApplies);
  // Equality, and the boolean form.
  EXPECT_EQ(Eval("@media (width = 600px)", 601, 5000), kNever);
  EXPECT_EQ(Eval("@media (width)", 320, 5000), kApplies);
}

TEST(MediaWidthEvaluatorTest, RangeSyntaxTwoSided) {
  const char* kTabletBand = "@media (40rem <= width < 64rem)";  // 640-1023
  EXPECT_EQ(Eval(kTabletBand, 320, 639), kNever);
  EXPECT_EQ(Eval(kTabletBand, 320, 640), kApplies);
  EXPECT_EQ(Eval(kTabletBand, 1024, 5000), kNever);
  EXPECT_EQ(Eval(kTabletBand, 1023, 5000), kApplies);
  EXPECT_EQ(Eval("@media (40rem<=width<64rem)", 1024, 5000), kNever);
  EXPECT_EQ(Eval("@media (1024px > width >= 640px)", 1024, 5000), kNever);
  EXPECT_EQ(Eval("@media (1024px > width >= 640px)", 320, 640), kApplies);
  EXPECT_EQ(Eval("@media (800px <= width <= 700px)", 0, 5000), kNever)
      << "an empty band applies nowhere";
}

TEST(MediaWidthEvaluatorTest, Units) {
  // em and rem are 16 px in a media query, whatever the page's root size.
  EXPECT_EQ(Eval("@media (min-width: 48em)", 767, 767), kNever);
  EXPECT_EQ(Eval("@media (min-width: 48em)", 768, 768), kApplies);
  EXPECT_EQ(Eval("@media (width >= 48rem)", 768, 768), kApplies);
  EXPECT_EQ(Eval("@media (min-width: 47.9375em)", 766, 766), kNever);
  EXPECT_EQ(Eval("@media (min-width: 47.9375em)", 767, 767), kApplies);
  EXPECT_EQ(Eval("@media (min-width: 10in)", 959, 959), kNever);
  EXPECT_EQ(Eval("@media (min-width: 10in)", 960, 960), kApplies);
  EXPECT_EQ(Eval("@media (min-width: 72pt)", 95, 95), kNever);
  EXPECT_EQ(Eval("@media (min-width: 72pt)", 96, 96), kApplies);
  EXPECT_EQ(Eval("@media (max-width: 0)", 1, 5000), kNever);
  // A unit it does not know is unknown, never excluded.
  EXPECT_EQ(Eval("@media (min-width: 50vw)", 320, 320), kUnknown);
  EXPECT_EQ(Eval("@media (min-width: 600)", 320, 320), kUnknown);
  EXPECT_EQ(Eval("@media (min-width: calc(40rem + 1px))", 320, 320), kUnknown);
}

TEST(MediaWidthEvaluatorTest, NotAndOr) {
  // Query-level not.
  EXPECT_EQ(Eval("@media not all and (max-width: 980px)", 320, 980), kNever);
  EXPECT_EQ(Eval("@media not all and (max-width: 980px)", 320, 981), kApplies);
  EXPECT_EQ(Eval("@media not screen and (min-width: 1024px)", 1024, 5000),
            kNever);
  // Condition-level not.
  EXPECT_EQ(Eval("@media not (width >= 48rem)", 768, 5000), kNever);
  EXPECT_EQ(Eval("@media not (width >= 48rem)", 767, 5000), kApplies);
  EXPECT_EQ(Eval("@media (not (width < 48rem))", 320, 767), kNever);
  // and / or, nested.
  EXPECT_EQ(Eval("@media (width >= 40rem) and (width < 48rem)", 768, 5000),
            kNever);
  EXPECT_EQ(Eval("@media (width < 400px) or (width > 1000px)", 400, 1000),
            kNever);
  EXPECT_EQ(Eval("@media (width < 400px) or (width > 1000px)", 400, 1001),
            kApplies);
  EXPECT_EQ(Eval("@media ((width < 400px) or (width > 1000px)) and "
                 "(width > 1200px)",
                 320, 1200),
            kNever);
  // Mixing and/or without parentheses is invalid CSS: unknown, kept.
  EXPECT_EQ(Eval("@media (width < 1px) and (width < 2px) or (width < 3px)", 320,
                 5000),
            kUnknown);
}

TEST(MediaWidthEvaluatorTest, CommaLists) {
  EXPECT_EQ(Eval("@media (max-width: 319px), (min-width: 1481px)", 320, 1480),
            kNever);
  EXPECT_EQ(Eval("@media (max-width: 319px), (min-width: 1481px)", 320, 1481),
            kApplies);
  EXPECT_EQ(
      Eval("@media (max-width: 319px), (orientation: portrait)", 320, 1480),
      kUnknown)
      << "an unreadable subquery can still apply";
  EXPECT_EQ(Eval("@media (max-width: 319px), (width >= 48rem)", 320, 767),
            kNever);
}

TEST(MediaWidthEvaluatorTest, OtherFeaturesAndTypes) {
  // A feature that is not about width neither excludes nor applies by itself
  // ...
  EXPECT_EQ(Eval("@media (hover: hover)", 320, 5000), kUnknown);
  EXPECT_EQ(Eval("@media (prefers-reduced-motion: reduce)", 320, 5000),
            kUnknown);
  EXPECT_EQ(Eval("@media (height >= 600px)", 320, 5000), kUnknown);
  EXPECT_EQ(Eval("@media (min-device-width: 1024px)", 320, 400), kUnknown);
  // ... but width can still exclude through `and`, and `not` of an unknown
  // stays unknown.
  EXPECT_EQ(Eval("@media (width >= 64rem) and (hover: hover)", 320, 980),
            kNever);
  EXPECT_EQ(Eval("@media (width >= 64rem) and (hover: hover)", 320, 1024),
            kUnknown);
  EXPECT_EQ(Eval("@media not (hover: hover)", 320, 5000), kUnknown);
  // Media types.
  EXPECT_EQ(Eval("@media screen", 320, 5000), kApplies);
  EXPECT_EQ(Eval("@media only screen and (min-width: 1024px)", 320, 1023),
            kNever);
  EXPECT_EQ(Eval("@media print and (min-width: 1px)", 320, 5000), kNever);
  EXPECT_EQ(Eval("@media tv", 320, 5000), kUnknown);
  // Malformed input is unknown, never excluded.
  EXPECT_EQ(Eval("@media (width >= )", 320, 320), kUnknown);
  EXPECT_EQ(Eval("@media (min-width: 1024px", 320, 320), kUnknown);
  EXPECT_EQ(Eval("@media screen and", 320, 320), kUnknown);
  EXPECT_EQ(Eval("@media", 320, 320), kUnknown);
  // With no pointer known, the pointer features stay unknown too.
  EXPECT_EQ(Eval("@media (hover: none)", 320, 5000), kUnknown);
  EXPECT_EQ(Eval("@media (pointer: coarse)", 320, 5000), kUnknown);
  EXPECT_EQ(Eval("@media (any-pointer: fine)", 320, 5000), kUnknown);
  EXPECT_EQ(Eval("@media (hover)", 320, 5000), kUnknown);
}

namespace {

MediaWidthMatch EvalTouch(std::string_view media, uint32_t lo, uint32_t hi) {
  return EvaluateMediaWidth(media, MediaWidthRange{lo, hi},
                            MediaPointer::kTouch);
}

}  // namespace

// The phone and tablet classes point with a touch screen, as
// their analysis renders emulate (src/browser/device_emulation.h), so the
// hover/pointer features decide: `none` / `coarse` apply at every width,
// `hover` / `fine` never do, and the any-* forms read the same.
TEST(MediaWidthEvaluatorTest, PointerFeaturesUnderATouchScreen) {
  EXPECT_EQ(EvalTouch("@media (hover: none)", 320, 980), kApplies);
  EXPECT_EQ(EvalTouch("@media (hover: hover)", 320, 980), kNever);
  EXPECT_EQ(EvalTouch("@media (any-hover: none)", 320, 980), kApplies);
  EXPECT_EQ(EvalTouch("@media (any-hover: hover)", 320, 980), kNever);
  EXPECT_EQ(EvalTouch("@media (pointer: coarse)", 320, 980), kApplies);
  EXPECT_EQ(EvalTouch("@media (pointer: fine)", 320, 980), kNever);
  EXPECT_EQ(EvalTouch("@media (pointer: none)", 320, 980), kNever);
  EXPECT_EQ(EvalTouch("@media (any-pointer: coarse)", 320, 980), kApplies);
  EXPECT_EQ(EvalTouch("@media (any-pointer: fine)", 320, 980), kNever);
  EXPECT_EQ(EvalTouch("@media (any-pointer: none)", 320, 980), kNever);
  // Boolean forms: true when the feature is not `none` (MQ4 §7). A touch
  // screen has a pointer but cannot hover.
  EXPECT_EQ(EvalTouch("@media (hover)", 320, 980), kNever);
  EXPECT_EQ(EvalTouch("@media (any-hover)", 320, 980), kNever);
  EXPECT_EQ(EvalTouch("@media (pointer)", 320, 980), kApplies);
  EXPECT_EQ(EvalTouch("@media (any-pointer)", 320, 980), kApplies);
  // Case and whitespace as the evaluator normalises them elsewhere.
  EXPECT_EQ(EvalTouch("@media (HOVER : NONE)", 320, 980), kApplies);
  EXPECT_EQ(EvalTouch("@media screen and (hover:none)", 320, 980), kApplies);
  // Composition: `not`, `and` with width, `or`, comma lists.
  EXPECT_EQ(EvalTouch("@media not (hover: hover)", 320, 980), kApplies);
  EXPECT_EQ(EvalTouch("@media not (hover: none)", 320, 980), kNever);
  EXPECT_EQ(EvalTouch("@media (hover: none) and (min-width: 1024px)", 320, 980),
            kNever)
      << "the width still excludes";
  EXPECT_EQ(EvalTouch("@media (hover: none) and (max-width: 500px)", 320, 980),
            kApplies);
  EXPECT_EQ(EvalTouch("@media (hover: hover) and (max-width: 500px)", 320, 980),
            kNever);
  EXPECT_EQ(EvalTouch("@media (hover: hover), (max-width: 500px)", 320, 980),
            kApplies)
      << "a list applies when any query does";
  EXPECT_EQ(EvalTouch("@media (hover: hover) or (pointer: coarse)", 320, 980),
            kApplies);
  EXPECT_EQ(EvalTouch("@media (hover: hover) or (pointer: fine)", 320, 980),
            kNever);
  EXPECT_EQ(
      EvalTouch("@media (hover: hover) and (prefers-reduced-motion)", 320, 980),
      kNever);
  EXPECT_EQ(
      EvalTouch("@media (hover: none) and (prefers-reduced-motion)", 320, 980),
      kUnknown)
      << "the unread feature still decides";
  // A value the feature cannot take is malformed: unknown, never excluded.
  EXPECT_EQ(EvalTouch("@media (hover: sometimes)", 320, 980), kUnknown);
  EXPECT_EQ(EvalTouch("@media (pointer: medium)", 320, 980), kUnknown);
  EXPECT_EQ(EvalTouch("@media (hover: none) and (pointer: medium)", 320, 980),
            kUnknown);
  // Width features are unchanged by the pointer.
  EXPECT_EQ(EvalTouch("@media (min-width: 1024px)", 320, 980), kNever);
  EXPECT_EQ(EvalTouch("@media (min-width: 320px)", 320, 980), kApplies);
}

// The pointer each class is retained and anchored with: a touch screen for
// the phone and the tablet, whose analysis renders emulate one; not known for
// the desktop, whose User-Agent can be an iPad's (iPadOS Safari sends a
// "Macintosh" UA) as its window can be any width.
TEST(MediaWidthEvaluatorTest, RetentionPointerFollowsTheAnalysisRenders) {
  EXPECT_EQ(RetentionPointer(CapabilityMask::Viewport::kMobile),
            MediaPointer::kTouch);
  EXPECT_EQ(RetentionPointer(CapabilityMask::Viewport::kTablet),
            MediaPointer::kTouch);
  EXPECT_EQ(RetentionPointer(CapabilityMask::Viewport::kDesktop),
            MediaPointer::kUnknown);
}

// End to end through Extract: the phone and tablet blocks keep
// `(hover: none)` / `(pointer: coarse)` blocks and drop `(hover: hover)` /
// `(pointer: fine)` ones, as the touch-emulated render that validates them
// applies and ignores them; the desktop block keeps both, as before.
TEST(CriticalCssExtractorTest, TouchClassesReadHoverAndPointerQueries) {
  CriticalCssExtractor extractor;
  std::vector<CollectedElement> elements;
  elements.push_back(MakeElement("body", 0, 0));
  elements.push_back(MakeElement("nav", 1, 1, "", {"menu"}));

  const std::string css =
      "@media (hover: none) { .menu { --touch: 1; } }\n"
      "@media (hover: hover) { .menu { --mouse: 1; } }\n"
      "@media (pointer: coarse) { .menu { --coarse: 1; } }\n"
      "@media (pointer: fine) { .menu { --fine: 1; } }\n"
      "@media (any-hover: hover) and (min-width: 1px) { .menu { --any: 1; } }\n"
      "@media not (hover: hover) { .menu { --nohover: 1; } }\n";

  for (auto viewport :
       {CapabilityMask::Viewport::kMobile, CapabilityMask::Viewport::kTablet}) {
    CriticalCssResult r = extractor.Extract(elements, css, viewport);
    ASSERT_TRUE(r.success);
    EXPECT_NE(r.critical_css.find("--touch:"), std::string::npos)
        << static_cast<int>(viewport);
    EXPECT_NE(r.critical_css.find("--coarse:"), std::string::npos)
        << static_cast<int>(viewport);
    EXPECT_NE(r.critical_css.find("--nohover:"), std::string::npos)
        << static_cast<int>(viewport);
    EXPECT_EQ(r.critical_css.find("--mouse:"), std::string::npos)
        << static_cast<int>(viewport);
    EXPECT_EQ(r.critical_css.find("--fine:"), std::string::npos)
        << static_cast<int>(viewport);
    EXPECT_EQ(r.critical_css.find("--any:"), std::string::npos)
        << static_cast<int>(viewport);
  }

  CriticalCssResult desktop =
      extractor.Extract(elements, css, CapabilityMask::Viewport::kDesktop);
  ASSERT_TRUE(desktop.success);
  for (const char* marker : {"--touch:", "--mouse:", "--coarse:", "--fine:",
                             "--any:", "--nohover:"}) {
    EXPECT_NE(desktop.critical_css.find(marker), std::string::npos)
        << marker << ": a desktop User-Agent may be an iPad's; both are kept";
  }
}

// A block that goes after the page's sheets (a layered sheet whose order is
// not proven) keeps every @media block any visitor could match, the
// hover/pointer ones included: a dropped override there loses to the kept
// base rule for good, and a phone with a mouse attached does hover.
TEST(CriticalCssExtractorTest, HoverQueriesAreKeptWhenTheBlockGoesLast) {
  CriticalCssExtractor extractor;
  std::vector<CollectedElement> elements;
  elements.push_back(MakeElement("body", 0, 0));
  elements.push_back(MakeElement("nav", 1, 1, "", {"menu"}));
  const std::string css =
      "@layer base { .menu { --base: 1; } }\n"
      "@media (hover: hover) { .menu { --mouse: 1; } }\n";
  CascadeLayerOrder unknown;
  CriticalCssResult r = extractor.Extract(
      elements, css, CapabilityMask::Viewport::kMobile, nullptr, &unknown);
  ASSERT_TRUE(r.success);
  EXPECT_FALSE(r.class_range_retention);
  EXPECT_NE(r.critical_css.find("--mouse:"), std::string::npos);

  // With a proven order the block goes first and the class decides again.
  CascadeLayerOrder proven;
  proven.proven = true;
  proven.reason.clear();
  proven.names = {"base"};
  r = extractor.Extract(elements, css, CapabilityMask::Viewport::kMobile,
                        nullptr, &proven);
  ASSERT_TRUE(r.success);
  EXPECT_TRUE(r.class_range_retention);
  EXPECT_EQ(r.critical_css.find("--mouse:"), std::string::npos);
}

// Anchoring reads the pointer too: a bar fixed only under `(hover: none)` is
// on a phone's fold, one fixed under `(hover: hover)` is not; on the desktop
// neither query is known to apply, so neither anchors.
TEST(CriticalCssExtractorTest, HoverQueriesAreEvaluatedForAnchoring) {
  std::vector<CollectedElement> elements;
  elements.push_back(MakeElement("body", 1, 0));
  for (int i = 1; i <= 40; ++i) {
    elements.push_back(MakeElement("p", 2, i));
  }
  elements.push_back(MakeElement("div", 2, 41, "", {"touch-dock"}));
  elements.push_back(MakeElement("p", 3, 42, "", {"touch-text"}));
  elements.push_back(MakeElement("div", 2, 43, "", {"mouse-dock"}));
  elements.push_back(MakeElement("p", 3, 44, "", {"mouse-text"}));

  const std::string css =
      "@media (hover: none) { .touch-dock { position: fixed; } }\n"
      "@media (hover: hover) { .mouse-dock { position: fixed; } }\n"
      ".touch-text { --tt: 1; }\n"
      ".mouse-text { --mt: 1; }\n";

  CriticalCssConfig config;
  config.max_elements = 2;
  CriticalCssExtractor extractor(config);

  for (auto viewport :
       {CapabilityMask::Viewport::kMobile, CapabilityMask::Viewport::kTablet}) {
    CriticalCssResult r = extractor.Extract(elements, css, viewport);
    ASSERT_TRUE(r.success);
    EXPECT_NE(r.critical_css.find("--tt:"), std::string::npos)
        << static_cast<int>(viewport);
    EXPECT_EQ(r.critical_css.find("--mt:"), std::string::npos)
        << static_cast<int>(viewport);
  }
  CriticalCssResult desktop =
      extractor.Extract(elements, css, CapabilityMask::Viewport::kDesktop);
  ASSERT_TRUE(desktop.success);
  EXPECT_EQ(desktop.critical_css.find("--tt:"), std::string::npos);
  EXPECT_EQ(desktop.critical_css.find("--mt:"), std::string::npos);
}

// ANCHORING reads the range syntax: it decides which elements are critical,
// not which rules are emitted, so the ordering hazard does not reach it. A
// widget fixed only under `(width<=420px)` anchors on mobile and not on
// desktop; one fixed from `(width>=48rem)` the other way round; strict bounds
// and two-sided ranges are read too. Observed through each widget's CHILD,
// whose plain rule is only emitted when the widget was anchored (a top-level
// @media block is emitted wholesale by retention whatever anchoring says).
TEST(CriticalCssExtractorTest,
     RangeSyntaxMediaQueriesAreEvaluatedForAnchoring) {
  std::vector<CollectedElement> elements;
  elements.push_back(MakeElement("body", 1, 0));
  for (int i = 1; i <= 40; ++i) {
    elements.push_back(MakeElement("p", 2, i));
  }
  elements.push_back(MakeElement("div", 2, 41, "", {"narrow-dock"}));
  elements.push_back(MakeElement("p", 3, 42, "", {"narrow-text"}));
  elements.push_back(MakeElement("div", 2, 43, "", {"wide-dock"}));
  elements.push_back(MakeElement("p", 3, 44, "", {"wide-text"}));
  elements.push_back(MakeElement("div", 2, 45, "", {"tablet-dock"}));
  elements.push_back(MakeElement("p", 3, 46, "", {"tablet-text"}));
  elements.push_back(MakeElement("div", 2, 47, "", {"strict-dock"}));
  elements.push_back(MakeElement("p", 3, 48, "", {"strict-text"}));

  const std::string css =
      "@media (width<=420px) { .narrow-dock { position: fixed; } }\n"
      "@media (width >= 48rem) { .wide-dock { position: fixed; } }\n"
      "@media (40rem <= width < 80rem) { .tablet-dock { position: fixed; } }\n"
      "@media (width > 1279px) { .strict-dock { position: fixed; } }\n"
      ".narrow-text { --nt: 1; }\n"
      ".wide-text { --wt: 1; }\n"
      ".tablet-text { --tt: 1; }\n"
      ".strict-text { --st: 1; }\n";

  CriticalCssConfig config;
  config.max_elements = 2;
  CriticalCssExtractor extractor(config);

  CriticalCssResult mobile =
      extractor.Extract(elements, css, CapabilityMask::Viewport::kMobile);
  ASSERT_TRUE(mobile.success);
  EXPECT_NE(mobile.critical_css.find("--nt:"), std::string::npos);
  EXPECT_EQ(mobile.critical_css.find("--wt:"), std::string::npos);
  EXPECT_EQ(mobile.critical_css.find("--tt:"), std::string::npos)
      << "640-1279px does not overlap mobile (0-479)";
  EXPECT_EQ(mobile.critical_css.find("--st:"), std::string::npos);

  CriticalCssResult tablet =
      extractor.Extract(elements, css, CapabilityMask::Viewport::kTablet);
  ASSERT_TRUE(tablet.success);
  EXPECT_EQ(tablet.critical_css.find("--nt:"), std::string::npos);
  EXPECT_NE(tablet.critical_css.find("--wt:"), std::string::npos)
      << "768px+ overlaps tablet (480-1279)";
  EXPECT_NE(tablet.critical_css.find("--tt:"), std::string::npos);
  EXPECT_EQ(tablet.critical_css.find("--st:"), std::string::npos)
      << "width > 1279px means at least 1280px: desktop only";

  CriticalCssResult desktop =
      extractor.Extract(elements, css, CapabilityMask::Viewport::kDesktop);
  ASSERT_TRUE(desktop.success);
  EXPECT_EQ(desktop.critical_css.find("--nt:"), std::string::npos);
  EXPECT_NE(desktop.critical_css.find("--wt:"), std::string::npos);
  EXPECT_EQ(desktop.critical_css.find("--tt:"), std::string::npos)
      << "640-1279px does not reach desktop (1280+)";
  EXPECT_NE(desktop.critical_css.find("--st:"), std::string::npos);
}

// A fixed element that its own rule also hides is not on screen: the modal
// shell (`position:fixed; inset:0; display:none`) and its whole subtree must
// not ride in on the anchor.
TEST(CriticalCssExtractorTest, FixedRuleThatAlsoHidesDoesNotAnchor) {
  std::vector<CollectedElement> elements;
  elements.push_back(MakeElement("body", 1, 0));
  for (int i = 1; i <= 40; ++i) {
    elements.push_back(MakeElement("p", 2, i));
  }
  elements.push_back(MakeElement("div", 2, 41, "", {"modal"}));
  elements.push_back(MakeElement("div", 3, 42, "", {"modal-body"}));
  elements.push_back(MakeElement("div", 2, 43, "", {"toast"}));
  elements.push_back(MakeElement("div", 2, 44, "", {"ghost"}));

  const std::string css =
      ".modal { position: fixed; inset: 0; display: none; }\n"
      ".modal-body { padding: 2rem; }\n"
      ".toast { position: fixed; visibility: hidden; }\n"
      ".ghost { position: fixed; opacity: 0 }\n";

  CriticalCssConfig config;
  config.max_elements = 2;
  CriticalCssExtractor extractor(config);
  CriticalCssResult result = extractor.Extract(elements, css);

  ASSERT_TRUE(result.success);
  EXPECT_EQ(result.critical_css.find("inset: 0"), std::string::npos)
      << "display:none in the same block: not anchored";
  EXPECT_EQ(result.critical_css.find("padding: 2rem"), std::string::npos)
      << "and its subtree stays out";
  EXPECT_EQ(result.critical_css.find("visibility: hidden"), std::string::npos);
  EXPECT_EQ(result.critical_css.find("opacity: 0"), std::string::npos);
}

// Only a rule's OWN declarations are read. With CSS nesting,
// `.card { color: red; .tip { position: fixed } }` fixes the tip, not the
// card, so `.card` must not anchor; and a Tailwind v4 responsive variant
// `.md\:fixed { @media (width >= 48rem) { position: fixed } }` is a nested
// block this pass does not evaluate, so it anchors nothing (under-inclusion).
TEST(CriticalCssExtractorTest, NestedFixedDeclarationsDoNotAnchorTheOuterRule) {
  std::vector<CollectedElement> elements;
  elements.push_back(MakeElement("body", 1, 0));
  for (int i = 1; i <= 40; ++i) {
    elements.push_back(MakeElement("p", 2, i));
  }
  elements.push_back(MakeElement("div", 2, 41, "", {"card"}));
  elements.push_back(MakeElement("span", 3, 42, "", {"card-title"}));
  elements.push_back(MakeElement("div", 2, 43, "", {"md:fixed", "dock"}));

  const std::string css =
      ".card { color: red; .tip { position: fixed; } }\n"
      ".card-title { font-weight: 700; }\n"
      ".md\\:fixed { @media (width >= 48rem) { position: fixed; } }\n"
      ".dock { height: 3rem; }\n";

  CriticalCssConfig config;
  config.max_elements = 2;
  CriticalCssExtractor extractor(config);
  CriticalCssResult result =
      extractor.Extract(elements, css, CapabilityMask::Viewport::kMobile);

  ASSERT_TRUE(result.success);
  EXPECT_EQ(result.critical_css.find("font-weight: 700"), std::string::npos)
      << "the card is not fixed; only its nested tip is";
  EXPECT_EQ(result.critical_css.find("height: 3rem"), std::string::npos)
      << "a nested responsive fixed declaration anchors nothing";
}

// A compound that needs an interaction state is not fixed at first paint:
// `.focus\:fixed:focus` is the skip link, on screen only while focused.
TEST(CriticalCssExtractorTest, StateConditionalFixedRulesDoNotAnchor) {
  std::vector<CollectedElement> elements;
  elements.push_back(MakeElement("body", 1, 0));
  for (int i = 1; i <= 40; ++i) {
    elements.push_back(MakeElement("p", 2, i));
  }
  elements.push_back(MakeElement("a", 2, 41, "", {"focus:fixed", "skip-link"}));
  elements.push_back(MakeElement("div", 2, 42, "", {"hover-dock"}));

  const std::string css =
      ".focus\\:fixed:focus { position: fixed; }\n"
      ".skip-link { padding: 4px; }\n"
      ".hover-dock:hover { position: fixed; }\n";

  CriticalCssConfig config;
  config.max_elements = 2;
  CriticalCssExtractor extractor(config);
  CriticalCssResult result = extractor.Extract(elements, css);

  ASSERT_TRUE(result.success);
  EXPECT_EQ(result.critical_css.find("padding: 4px"), std::string::npos)
      << ":focus-only fixed rules do not anchor";
  EXPECT_EQ(result.critical_css.find("hover-dock"), std::string::npos);
}

// The markup can say an element is not rendered (the `hidden` attribute, an
// inline display:none): such an element is never an anchor, and a hidden
// subtree inside an anchor — the launcher's closed panel — stays out.
TEST(CriticalCssExtractorTest, HiddenElementsAreNeitherAnchorsNorAnchored) {
  std::vector<CollectedElement> elements;
  elements.push_back(MakeElement("body", 1, 0));
  for (int i = 1; i <= 40; ++i) {
    elements.push_back(MakeElement("p", 2, i));
  }
  CollectedElement hidden_root = MakeElement("div", 2, 41, "", {"chat-root"});
  hidden_root.hidden = true;
  elements.push_back(hidden_root);
  elements.push_back(MakeElement("button", 3, 42, "", {"chat-launcher"}));
  elements.push_back(MakeElement("div", 2, 43, "", {"fab-root"}));
  CollectedElement panel = MakeElement("div", 3, 44, "", {"fab-panel"});
  panel.hidden = true;
  elements.push_back(panel);
  elements.push_back(MakeElement("p", 4, 45, "", {"fab-panel-text"}));
  elements.push_back(MakeElement("button", 3, 46, "", {"fab-button"}));

  const std::string css =
      ".chat-root { position: fixed; bottom: 0; }\n"
      ".chat-launcher { width: 3rem; }\n"
      ".fab-root { position: fixed; right: 0; }\n"
      ".fab-panel { width: 20rem; }\n"
      ".fab-panel-text { font-size: 13px; }\n"
      ".fab-button { height: 3rem; }\n";

  CriticalCssConfig config;
  config.max_elements = 2;
  CriticalCssExtractor extractor(config);
  CriticalCssResult result = extractor.Extract(elements, css);

  ASSERT_TRUE(result.success);
  EXPECT_EQ(result.critical_css.find("bottom: 0"), std::string::npos)
      << "a hidden element is not an anchor";
  EXPECT_EQ(result.critical_css.find("width: 3rem"), std::string::npos)
      << "nor is its subtree anchored";
  EXPECT_NE(result.critical_css.find("right: 0"), std::string::npos)
      << "the visible fixed root is";
  EXPECT_EQ(result.critical_css.find("width: 20rem"), std::string::npos)
      << "its hidden panel stays out";
  EXPECT_EQ(result.critical_css.find("font-size: 13px"), std::string::npos)
      << "and so does everything inside the hidden panel";
  EXPECT_NE(result.critical_css.find("height: 3rem"), std::string::npos)
      << "the visible sibling after the hidden panel is anchored";
}

// One anchor pulls in at most kMaxFixedAnchorSubtree (50) elements. A fixed
// wrapper that an unclosed tag turns into an ancestor of the rest of the
// document must not promote the rest of the document.
TEST(CriticalCssExtractorTest, FixedAnchorSubtreeIsCapped) {
  std::vector<CollectedElement> elements;
  elements.push_back(MakeElement("body", 1, 0));
  for (int i = 1; i <= 40; ++i) {
    elements.push_back(MakeElement("p", 2, i));
  }
  elements.push_back(MakeElement("div", 2, 41, "", {"wrapper"}));
  // 200 "descendants": what an unclosed fixed <div> looks like to the scanner.
  for (int i = 0; i < 200; ++i) {
    elements.push_back(
        MakeElement("p", 3, 42 + i, "", {absl::StrCat("late-", i)}));
  }

  std::string css = ".wrapper { position: fixed; top: 0; }\n";
  for (int i = 0; i < 200; ++i) {
    absl::StrAppend(&css, ".late-", i, " { margin: ", i, "px; }\n");
  }

  CriticalCssConfig config;
  config.max_elements = 2;
  CriticalCssExtractor extractor(config);
  CriticalCssResult result = extractor.Extract(elements, css);

  ASSERT_TRUE(result.success);
  EXPECT_NE(result.critical_css.find("top: 0"), std::string::npos);
  EXPECT_NE(result.critical_css.find("margin: 0px"), std::string::npos)
      << "the first descendants come along";
  EXPECT_NE(result.critical_css.find("margin: 48px"), std::string::npos)
      << "up to the cap (the anchor itself counts as one)";
  EXPECT_EQ(result.critical_css.find("margin: 49px"), std::string::npos)
      << "and nothing past it";
  EXPECT_EQ(result.critical_css.find("margin: 199px"), std::string::npos);
  EXPECT_EQ(result.critical_rules, 1 + 49);
}

// An anchor is exempt from the class/id/tag exclusion patterns (a floating
// "defer-banner" is on screen whatever it is called) but not from the depth
// veto.
TEST(CriticalCssExtractorTest, FixedAnchorSkipsPatternVetoButNotDepthVeto) {
  std::vector<CollectedElement> elements;
  elements.push_back(MakeElement("body", 1, 0));
  for (int i = 1; i <= 40; ++i) {
    elements.push_back(MakeElement("p", 2, i));
  }
  elements.push_back(MakeElement("div", 2, 41, "", {"defer-banner", "fab"}));
  elements.push_back(MakeElement("div", 12, 42, "", {"deep-dock", "fab"}));

  const std::string css =
      ".fab { position: fixed; }\n"
      ".defer-banner { bottom: 2px; }\n"
      ".deep-dock { bottom: 3px; }\n";

  CriticalCssConfig config;
  config.max_elements = 2;
  CriticalCssExtractor extractor(config);
  CriticalCssResult result = extractor.Extract(elements, css);

  ASSERT_TRUE(result.success);
  EXPECT_NE(result.critical_css.find("bottom: 2px"), std::string::npos)
      << "the `defer` exclusion pattern does not veto an anchor";
  EXPECT_EQ(result.critical_css.find("bottom: 3px"), std::string::npos)
      << "an anchor deeper than max_depth is still vetoed";
}

CollectedElement Wide(const std::string& tag, int depth, int index,
                      const std::vector<std::string>& classes) {
  CollectedElement elem = MakeElement(tag, depth, index, "", classes);
  elem.may_exceed_viewport = true;
  return elem;
}

// A replaced element that may be wider than a phone (an unsized <svg> is 300
// px wide until a rule sizes it) widens the document wherever it sits. On a
// 375 px phone one such icon far below the fold makes the layout viewport
// grow and the whole fold shift (the FAQ chevron of the
// async-CSS probe fixture, at element ~1000 and depth 11). For the mobile and
// tablet blocks its rules are kept regardless of the fold estimate and of the
// depth / pattern vetoes, which only estimate the fold.
TEST(CriticalCssExtractorTest, WideReplacedElementBelowTheFoldIsCritical) {
  std::vector<CollectedElement> elements;
  elements.push_back(MakeElement("body", 1, 0));
  for (int i = 1; i <= 40; ++i) {
    elements.push_back(MakeElement("p", 2, i, "", {"copy"}));
  }
  elements.push_back(MakeElement("summary", 11, 41, "", {"faq"}));
  elements.push_back(Wide("svg", 12, 42, {"h-5", "w-5"}));
  elements.push_back(MakeElement("path", 13, 43, "", {"chevron-path"}));
  elements.push_back(MakeElement("footer", 2, 44));
  elements.push_back(Wide("iframe", 3, 45, {"map", "footer-map"}));
  elements.push_back(Wide("canvas", 3, 46, {"chart"}));

  const std::string css =
      ".copy { color: red; }\n"
      ".faq { padding: 1px; }\n"
      ".h-5 { height: 1.25rem; }\n"
      ".w-5 { width: 1.25rem; }\n"
      ".chevron-path { stroke-width: 3px; }\n"
      ".footer-map { width: 100%; }\n"
      "@media (min-width: 1px) { .chart { max-width: 100%; } }\n";

  CriticalCssConfig config;
  config.max_elements = 2;  // <body> and the first paragraph
  for (CapabilityMask::Viewport viewport :
       {CapabilityMask::Viewport::kMobile, CapabilityMask::Viewport::kTablet}) {
    CriticalCssExtractor extractor(config);
    CriticalCssResult result = extractor.Extract(elements, css, viewport);

    ASSERT_TRUE(result.success);
    EXPECT_NE(result.critical_css.find("height: 1.25rem"), std::string::npos)
        << "the svg, past the estimate and deeper than max_depth";
    EXPECT_NE(result.critical_css.find("width: 1.25rem"), std::string::npos);
    EXPECT_NE(result.critical_css.find("width: 100%"), std::string::npos)
        << "an iframe, despite the footer exclusion pattern";
    EXPECT_NE(result.critical_css.find("max-width: 100%"), std::string::npos)
        << "a canvas, inside an @media block";
    EXPECT_EQ(result.critical_css.find("padding: 1px"), std::string::npos)
        << "the svg's parent stays outside the fold";
    EXPECT_EQ(result.critical_css.find("stroke-width"), std::string::npos)
        << "the svg's own content is not included";
  }

  // A desktop window is wide enough that a 300 px element almost never
  // overflows it; the desktop block is not grown for it.
  CriticalCssExtractor desktop(config);
  CriticalCssResult result =
      desktop.Extract(elements, css, CapabilityMask::Viewport::kDesktop);
  ASSERT_TRUE(result.success);
  EXPECT_EQ(result.critical_css.find("1.25rem"), std::string::npos);
  EXPECT_EQ(result.critical_rules, 1);
}

// Hidden elements and everything inside a hidden subtree are not on screen.
TEST(CriticalCssExtractorTest, HiddenWideReplacedElementsAreNotCritical) {
  std::vector<CollectedElement> elements;
  elements.push_back(MakeElement("body", 1, 0));
  for (int i = 1; i <= 40; ++i) {
    elements.push_back(MakeElement("p", 2, i));
  }
  CollectedElement hidden = Wide("svg", 2, 41, {"hidden-icon"});
  hidden.hidden = true;
  elements.push_back(hidden);
  CollectedElement panel = MakeElement("div", 2, 42, "", {"panel"});
  panel.hidden = true;
  elements.push_back(panel);
  elements.push_back(Wide("svg", 3, 43, {"panel-icon"}));
  elements.push_back(MakeElement("img", 2, 44, "", {"fits"}));
  elements.push_back(Wide("svg", 2, 45, {"visible-icon"}));

  const std::string css =
      ".hidden-icon { height: 4px; }\n"
      ".panel-icon { height: 5px; }\n"
      ".fits { height: 3px; }\n"
      ".visible-icon { height: 6px; }\n";

  CriticalCssConfig config;
  config.max_elements = 2;
  CriticalCssExtractor extractor(config);
  CriticalCssResult result =
      extractor.Extract(elements, css, CapabilityMask::Viewport::kMobile);

  ASSERT_TRUE(result.success);
  EXPECT_EQ(result.critical_css.find("height: 3px"), std::string::npos)
      << "an element whose markup width fits (may_exceed_viewport unset)";
  EXPECT_EQ(result.critical_css.find("height: 4px"), std::string::npos)
      << "a hidden svg";
  EXPECT_EQ(result.critical_css.find("height: 5px"), std::string::npos)
      << "an svg inside a hidden panel";
  EXPECT_NE(result.critical_css.find("height: 6px"), std::string::npos)
      << "the hidden panel's subtree ends at its next sibling";
  EXPECT_EQ(result.critical_rules, 1);
}

// The reviewer's Bootstrap-shaped page: a YouTube embed (`width="560"`, sized
// by `w-full`) and a 1600 px `.img-fluid` image below 400 paragraphs. Without
// their rules the flash page is 1600 px wide on a 375 px phone. The rules
// that shrink them are kept; the paragraphs' are not.
TEST(CriticalCssExtractorTest, WideEmbedAndImageBelowTheFoldAreSized) {
  std::vector<CollectedElement> elements;
  elements.push_back(MakeElement("body", 1, 0));
  for (int i = 1; i <= 400; ++i) {
    elements.push_back(MakeElement("p", 2, i, "", {"lead"}));
  }
  elements.push_back(Wide("iframe", 2, 401, {"w-full", "aspect-video"}));
  elements.push_back(Wide("img", 2, 402, {"img-fluid"}));

  const std::string css =
      ".lead { font-size: 1.25rem; }\n"
      ".w-full { width: 100%; }\n"
      ".aspect-video { aspect-ratio: 16 / 9; }\n"
      ".img-fluid { max-width: 100%; height: auto; }\n";

  CriticalCssConfig config;
  config.max_elements = 50;
  CriticalCssExtractor extractor(config);
  CriticalCssResult result =
      extractor.Extract(elements, css, CapabilityMask::Viewport::kMobile);

  ASSERT_TRUE(result.success);
  EXPECT_NE(result.critical_css.find(".w-full"), std::string::npos);
  EXPECT_NE(result.critical_css.find(".aspect-video"), std::string::npos);
  EXPECT_NE(result.critical_css.find(".img-fluid"), std::string::npos);
  EXPECT_NE(result.critical_css.find(".lead"), std::string::npos)
      << "the fold's paragraphs";
}

// An icon set with one utility class per icon (500 icons, each its own
// rule) must not double the block: rules kept only for wide replaced
// elements stop at kMaxWideReplacedBytes, in sheet order. A small @media
// block is not copied whole for such a rule, so its other rules do not ride
// along outside the budget.
TEST(CriticalCssExtractorTest, WideReplacedRulesAreBounded) {
  std::vector<CollectedElement> elements;
  elements.push_back(MakeElement("body", 1, 0));
  elements.push_back(MakeElement("h1", 2, 1, "", {"title"}));
  for (int i = 0; i < 500; ++i) {
    elements.push_back(Wide("svg", 3, 2 + i, {absl::StrCat("icon-", i)}));
  }
  std::string css = ".title { font-size: 2rem; }\n";
  for (int i = 0; i < 500; ++i) {
    absl::StrAppend(&css, ".icon-", i,
                    " { width: 1.25rem; height: 1.25rem; color: #", 100000 + i,
                    "; mask-image: url(/icons/icon-", i, ".svg); }\n");
  }
  absl::StrAppend(&css,
                  "@media (min-width: 1px) { .icon-0 { margin: 1px; } "
                  ".unrelated-a { margin: 2px; } .unrelated-b { margin: 3px; } "
                  "}\n");

  CriticalCssConfig config;
  config.max_elements = 2;
  CriticalCssExtractor extractor(config);
  CriticalCssResult result =
      extractor.Extract(elements, css, CapabilityMask::Viewport::kMobile);

  ASSERT_TRUE(result.success);
  EXPECT_NE(result.critical_css.find("font-size: 2rem"), std::string::npos);
  EXPECT_NE(result.critical_css.find(".icon-0 {"), std::string::npos)
      << "the first icons are sized";
  EXPECT_EQ(result.critical_css.find(".icon-499 {"), std::string::npos)
      << "the budget closes before the last";
  EXPECT_LE(result.critical_css.size(),
            kMaxWideReplacedBytes +
                std::string_view(".title { font-size: "
                                 "2rem; }\n")
                    .size() +
                64)
      << "bounded by the budget (plus the title rule and a wrapper)";
  EXPECT_EQ(result.critical_css.find("unrelated"), std::string::npos)
      << "the @media block is not copied whole for a budget-only rule";
}

// The budget bounds the emitted bytes, the @media wrappers of budget-only
// rules included: on a sheet that puts every icon rule in its own @media
// block, the block still stays within kMaxWideReplacedBytes.
TEST(CriticalCssExtractorTest, WideReplacedBudgetCountsWrappers) {
  std::vector<CollectedElement> elements;
  elements.push_back(MakeElement("body", 1, 0));
  elements.push_back(MakeElement("h1", 2, 1, "", {"title"}));
  for (int i = 0; i < 500; ++i) {
    elements.push_back(Wide("svg", 3, 2 + i, {absl::StrCat("icon-", i)}));
  }
  const std::string title = ".title { font-size: 2rem; }";
  std::string css = title + "\n";
  for (int i = 0; i < 500; ++i) {
    absl::StrAppend(&css,
                    "@media (min-width: 1px) and (orientation: portrait) { "
                    ".icon-",
                    i, " { width: 1.25rem; } }\n");
  }
  CriticalCssConfig config;
  config.max_elements = 2;
  CriticalCssExtractor extractor(config);
  CriticalCssResult result =
      extractor.Extract(elements, css, CapabilityMask::Viewport::kMobile);
  ASSERT_TRUE(result.success);
  EXPECT_NE(result.critical_css.find(".icon-0 {"), std::string::npos);
  EXPECT_EQ(result.critical_css.find(".icon-499 {"), std::string::npos);
  EXPECT_LE(result.critical_css.size(),
            kMaxWideReplacedBytes + title.size() + 1);
}

// Rules kept for wide replaced elements never take the block past what the
// worker will inline: a block over the byte limit or the coverage share is
// not inlined at all, which would trade an overflow for a full flash.
TEST(CriticalCssExtractorTest, WideReplacedRulesStayWithinTheInlineLimits) {
  std::vector<CollectedElement> elements;
  elements.push_back(MakeElement("body", 1, 0));
  elements.push_back(MakeElement("h1", 2, 1, "", {"title"}));
  for (int i = 0; i < 100; ++i) {
    elements.push_back(Wide("svg", 3, 2 + i, {absl::StrCat("icon-", i)}));
  }
  std::string css = ".title { font-size: 2rem; }\n";
  for (int i = 0; i < 100; ++i) {
    absl::StrAppend(&css, ".icon-", i,
                    " { width: 1.25rem; height: 1.25rem; }\n");
  }
  // Pad the sheet so the coverage share is not what binds.
  for (int i = 0; i < 400; ++i) {
    absl::StrAppend(&css, ".unused-", i, " { margin: 1px; }\n");
  }

  CriticalCssConfig unlimited;
  unlimited.max_elements = 2;
  unlimited.inline_limit_min_sheet_bytes = 0;
  CriticalCssExtractor wide(unlimited);
  CriticalCssResult all =
      wide.Extract(elements, css, CapabilityMask::Viewport::kMobile);
  ASSERT_TRUE(all.success);
  ASSERT_NE(all.critical_css.find(".icon-99 {"), std::string::npos);

  CriticalCssConfig limited = unlimited;
  limited.inline_limit_bytes = 1500;
  CriticalCssExtractor extractor(limited);
  CriticalCssResult result =
      extractor.Extract(elements, css, CapabilityMask::Viewport::kMobile);
  ASSERT_TRUE(result.success);
  EXPECT_LE(result.critical_css.size(), limited.inline_limit_bytes);
  EXPECT_NE(result.critical_css.find("font-size: 2rem"), std::string::npos)
      << "the fold's own rules are never given up";
  EXPECT_NE(result.critical_css.find(".icon-0 {"), std::string::npos)
      << "the room left is still used";
  EXPECT_EQ(result.critical_css.find(".icon-99 {"), std::string::npos);

  // Coverage: on a sheet that is mostly icons, the block stays under the
  // share of the sheet the worker inlines (applied here although the sheet is
  // small: inline_limit_min_sheet_bytes is 0 above).
  std::string dense = ".title { font-size: 2rem; }\n";
  for (int i = 0; i < 100; ++i) {
    absl::StrAppend(&dense, ".icon-", i,
                    " { width: 1.25rem; height: 1.25rem; }\n");
  }
  CriticalCssExtractor dense_extractor(unlimited);
  CriticalCssResult dense_result = dense_extractor.Extract(
      elements, dense, CapabilityMask::Viewport::kMobile);
  ASSERT_TRUE(dense_result.success);
  EXPECT_LT(static_cast<float>(dense_result.critical_css.size()),
            unlimited.inline_limit_coverage * static_cast<float>(dense.size()));
}

// Only position:fixed anchors. A sticky element is in normal flow and is only
// in the viewport if its own position is; a universal fixed rule would anchor
// the whole document and is refused.
TEST(CriticalCssExtractorTest, StickyAndUniversalRulesDoNotAnchor) {
  std::vector<CollectedElement> elements;
  elements.push_back(MakeElement("body", 1, 0));
  for (int i = 1; i <= 40; ++i) {
    elements.push_back(MakeElement("p", 2, i));
  }
  elements.push_back(MakeElement("div", 2, 41, "", {"sticky", "toolbar"}));
  elements.push_back(MakeElement("div", 2, 42, "", {"late"}));

  const std::string css =
      ".sticky { position: sticky; }\n"
      ".toolbar { height: 2rem; }\n"
      "* { position: fixed; }\n"
      ".late { color: blue; }\n";

  CriticalCssConfig config;
  config.max_elements = 2;
  CriticalCssExtractor extractor(config);
  CriticalCssResult result = extractor.Extract(elements, css);

  ASSERT_TRUE(result.success);
  EXPECT_EQ(result.critical_css.find("height: 2rem"), std::string::npos)
      << "sticky is not an anchor";
  EXPECT_EQ(result.critical_css.find("color: blue"), std::string::npos)
      << "a universal fixed rule must not anchor every element";
}

// A backslash escapes the next code point everywhere in CSS,
// quotes and braces included (CSS Syntax 3 §4.3.7). The rule parser took the
// escaped quote in `.a\'b` for the start of a string that never ended, and
// lost every rule after it: Tailwind v4 escapes `'` that way, and shadcn/ui's
// Button carries such a class.
TEST(CriticalCssExtractorTest, EscapedQuoteInSelectorDoesNotStopTheParser) {
  std::vector<CollectedElement> elements;
  elements.push_back(MakeElement("body", 0, 0));
  elements.push_back(MakeElement("p", 1, 1, "", {"a'b"}));
  elements.push_back(MakeElement("div", 1, 2, "", {"c"}));

  CriticalCssExtractor extractor;
  CriticalCssResult result =
      extractor.Extract(elements, ".a\\'b{color:red} .c{color:blue}");
  ASSERT_TRUE(result.success);
  EXPECT_EQ(result.total_rules, 2);
  EXPECT_NE(result.critical_css.find(".a\\'b {color:red}"), std::string::npos)
      << result.critical_css;
  EXPECT_NE(result.critical_css.find(".c {color:blue}"), std::string::npos)
      << result.critical_css;
}

// The shadcn/ui Button class `[&_svg:not([class*='size-'])]:size-4`, as
// Tailwind v4 emits it, followed by fourteen more rules: all fifteen parse,
// and the rule itself matches its button by the escaped class.
TEST(CriticalCssExtractorTest, ShadcnEscapedQuoteSelectorKeepsLaterRules) {
  const std::string shadcn =
      ".\\[\\&_svg\\:not\\(\\[class\\*\\=\\'size-\\'\\]\\)\\]\\:size-4 "
      "svg:not([class*='size-']){width:calc(var(--spacing)*4)}";
  std::string css = "@layer utilities{" + shadcn;
  for (int i = 0; i < 14; ++i) {
    absl::StrAppend(&css, ".u-", i, "{margin:", i, "px}");
  }
  css += "}";

  std::vector<CollectedElement> elements;
  elements.push_back(MakeElement("body", 0, 0));
  elements.push_back(MakeElement(
      "button", 1, 1, "", {"[&_svg:not([class*='size-'])]:size-4", "u-13"}));
  elements.push_back(MakeElement("svg", 2, 2));

  CriticalCssExtractor extractor;
  CriticalCssResult result = extractor.Extract(elements, css);
  ASSERT_TRUE(result.success);
  EXPECT_NE(result.critical_css.find("margin:13px"), std::string::npos)
      << "the last rule after the escaped quotes is still seen";
  EXPECT_NE(result.critical_css.find("width:calc(var(--spacing)*4)"),
            std::string::npos)
      << "the shadcn rule matches its svg";
  EXPECT_EQ(result.critical_rules, 2) << result.critical_css;
}

// The rest of the escape and string rules the parser follows: an escaped
// brace opens no block, an escape or a string at the end of input ends the
// sheet cleanly, a backslash-newline inside a string continues it, and an
// unescaped newline ends a string (a bad string) so the block still closes.
TEST(CriticalCssExtractorTest, ParserFollowsCssSyntaxEscapesAndStrings) {
  std::vector<CollectedElement> elements;
  elements.push_back(MakeElement("body", 0, 0));
  elements.push_back(MakeElement("div", 1, 1, "", {"a{b"}));
  elements.push_back(MakeElement("div", 1, 2, "", {"c"}));
  CriticalCssExtractor extractor;

  CriticalCssResult brace =
      extractor.Extract(elements, ".a\\{b{color:red}.c{color:blue}");
  EXPECT_EQ(brace.total_rules, 2);
  EXPECT_NE(brace.critical_css.find(".a\\{b {color:red}"), std::string::npos)
      << brace.critical_css;
  EXPECT_NE(brace.critical_css.find(".c {color:blue}"), std::string::npos);

  CriticalCssResult trailing_escape =
      extractor.Extract(elements, ".c{color:blue} .a\\");
  EXPECT_TRUE(trailing_escape.success);
  EXPECT_EQ(trailing_escape.total_rules, 1);

  CriticalCssResult trailing_string =
      extractor.Extract(elements, ".c{color:blue} .d{content:\"x\\");
  EXPECT_TRUE(trailing_string.success);
  EXPECT_NE(trailing_string.critical_css.find(".c {color:blue}"),
            std::string::npos);

  CriticalCssResult continued =
      extractor.Extract(elements, ".d{content:\"x\\\n}y\"}.c{color:blue}");
  EXPECT_EQ(continued.total_rules, 2)
      << "an escaped newline continues the string, so its '}' is text";
  EXPECT_NE(continued.critical_css.find(".c {color:blue}"), std::string::npos);

  CriticalCssResult outside =
      extractor.Extract(elements, ".a\\\n{color:red}.c{color:blue}");
  EXPECT_EQ(outside.total_rules, 2) << "a backslash-newline outside a string";
  EXPECT_NE(outside.critical_css.find(".c {color:blue}"), std::string::npos);

  CriticalCssResult bad_string =
      extractor.Extract(elements, ".d{content:\"unterminated\n}.c{color:blue}");
  EXPECT_EQ(bad_string.total_rules, 2)
      << "an unescaped newline ends the string; the '}' after it closes";
  EXPECT_NE(bad_string.critical_css.find(".c {color:blue}"), std::string::npos);
}

// A `;` inside parentheses, an attribute selector or a string in an at-rule's
// prelude does not end a statement there: `@supports (a;b) { ... }` is one
// block rule, not the statement `@supports (a` followed by a rule with the
// selector `b)` (found with `@l\61yer (a;b) {` inside an @media
// copied whole, where the fragment reached the block). A real statement
// before such a rule still ends at its own `;`.
TEST(CriticalCssExtractorTest, PreludeSemicolonInParensDoesNotSplitTheRule) {
  std::vector<CollectedElement> elements;
  elements.push_back(MakeElement("body", 0, 0));
  elements.push_back(MakeElement("div", 1, 1, "", {"c", "hidden"}));
  CriticalCssExtractor extractor;
  for (const char* prelude :
       {"@supports (a;b)", "@supports selector([data-x=\";\"])",
        "@supports (content:\";\")", "@supports (x:url(a;b))",
        "@supports ((a;b) and (c))"}) {
    const std::string css =
        absl::StrCat("@import url(i.css);", prelude, " { .c{color:blue} }");
    CriticalCssResult r = extractor.Extract(elements, css);
    ASSERT_TRUE(r.success) << css;
    EXPECT_EQ(r.total_rules, 2) << css << "\n" << r.critical_css;
    EXPECT_NE(r.critical_css.find("@import url(i.css);"), std::string::npos)
        << css << "\n"
        << r.critical_css;
    EXPECT_NE(r.critical_css.find(absl::StrCat(prelude, " {")),
              std::string::npos)
        << css << "\n"
        << r.critical_css;
    EXPECT_NE(r.critical_css.find("color:blue"), std::string::npos)
        << css << "\n"
        << r.critical_css;
    EXPECT_EQ(r.critical_css.find("\nb) {"), std::string::npos)
        << "no rule with the selector `b)`: " << css << "\n"
        << r.critical_css;
  }
  // The reviewer's case: an escaped anonymous layer with such a prelude inside
  // an @media copied whole, under an unproven order. The layer goes
  // and no fragment of it stays.
  CriticalCssResult r = extractor.Extract(
      elements,
      "@layer base{div{margin:0}} "
      "@media screen { @l\\61yer (a;b) { .hidden{display:none} } }");
  ASSERT_TRUE(r.success);
  EXPECT_TRUE(r.anonymous_layers_dropped);
  EXPECT_EQ(r.critical_css.find("b)"), std::string::npos) << r.critical_css;
  EXPECT_EQ(r.critical_css.find("display:none"), std::string::npos)
      << r.critical_css;
  EXPECT_NE(r.critical_css.find("margin:0"), std::string::npos)
      << r.critical_css;
  // A `(` that is never closed: no statement is read after it (a browser
  // reads to the end of the sheet there), the rule before it is unaffected,
  // and the parser still terminates. What the brace parser makes of the rest
  // is unchanged by this fix.
  r = extractor.Extract(elements, ".c{color:blue} @supports (a;b {x:y}");
  ASSERT_TRUE(r.success);
  EXPECT_NE(r.critical_css.find("color:blue"), std::string::npos)
      << r.critical_css;
}

// Selector lists and complex selectors do not split at an escaped comma or
// combinator (`.a\,b`, `.\[\&\>svg\]`, `.a\ b`) or at one inside a string
// (`[title="x,y"]`).
TEST(CriticalCssExtractorTest, SelectorSplittingIgnoresEscapedAndNestedText) {
  std::vector<CollectedElement> elements;
  elements.push_back(MakeElement("body", 0, 0));
  elements.push_back(MakeElement("li", 1, 1, "", {"a b"}));
  elements.push_back(MakeElement("div", 1, 2, "", {"a,b"}));
  elements.push_back(MakeElement("span", 1, 3, "", {"[&>svg]:w-5"}));
  CriticalCssExtractor extractor;
  CriticalCssResult result =
      extractor.Extract(elements,
                        "ul .a\\ b{color:red}\n"
                        ".a\\,b{color:green}\n"
                        "[title=\"x,y\"] .zz, .nope{color:gray}\n"
                        ".\\[\\&\\>svg\\]\\:w-5{color:blue}\n");
  ASSERT_TRUE(result.success);
  EXPECT_NE(result.critical_css.find("color:red"), std::string::npos)
      << "an escaped space is part of the class";
  EXPECT_NE(result.critical_css.find("color:green"), std::string::npos)
      << "an escaped comma does not split the list";
  EXPECT_EQ(result.critical_css.find("color:gray"), std::string::npos)
      << "the comma inside the attribute value does not make `y\"] .zz` an "
         "alternative";
  EXPECT_NE(result.critical_css.find("color:blue"), std::string::npos)
      << "an escaped > is part of the class";
}

// An unquoted url() is raw text up to its ')': a quote in it makes it a bad
// URL, which browsers skip to the ')', so it opens no string; `url("…")`
// still holds a string. And `\` + CRLF inside a string is one line
// continuation.
TEST(CriticalCssExtractorTest, ParserReadsUnquotedUrlsAndCrlfContinuations) {
  std::vector<CollectedElement> elements;
  elements.push_back(MakeElement("body", 0, 0));
  elements.push_back(MakeElement("div", 1, 1, "", {"c"}));
  CriticalCssExtractor extractor;

  CriticalCssResult url =
      extractor.Extract(elements, ".a{background:url(it's.png)}.c{color:blue}");
  EXPECT_EQ(url.total_rules, 2);
  EXPECT_NE(url.critical_css.find(".c {color:blue}"), std::string::npos);

  CriticalCssResult escaped_paren = extractor.Extract(
      elements, ".a{background:URL( a\\)'b.png )}.c{color:blue}");
  EXPECT_EQ(escaped_paren.total_rules, 2) << "an escaped ')' does not end it";

  CriticalCssResult quoted = extractor.Extract(
      elements, ".a{background:url( \"x).png\" )}.c{color:blue}");
  EXPECT_EQ(quoted.total_rules, 2) << "a quoted url() argument is a string";

  CriticalCssResult crlf =
      extractor.Extract(elements, ".d{content:\"x\\\r\n}y\"}.c{color:blue}");
  EXPECT_EQ(crlf.total_rules, 2)
      << "the '}' after a CRLF continuation is still string text";
  EXPECT_NE(crlf.critical_css.find(".c {color:blue}"), std::string::npos);
}

// A combinator character or a space inside an attribute
// selector or a functional pseudo-class does not cut the selector, so these
// rules are kept for their fold elements. A subject with nothing to match
// (Tailwind v4's `space-y-*` / `divide-y` children) is matched on the nearest
// compound that has a tag, id or class.
TEST(CriticalCssExtractorTest, CombinatorsInsideBracketsAndParensDoNotCut) {
  std::vector<CollectedElement> elements;
  elements.push_back(MakeElement("body", 0, 0));
  elements.push_back(MakeElement("div", 1, 1, "", {"y", "c"}));
  elements.push_back(MakeElement("li", 1, 2));
  elements.push_back(MakeElement("input", 1, 3, "", {"narrow"}));
  elements.push_back(MakeElement("div", 1, 4, "", {"note"}));
  elements.push_back(MakeElement("div", 1, 5, "", {"space-y-4"}));
  elements.push_back(MakeElement("p", 2, 6));
  elements.push_back(MakeElement("ul", 1, 7, "", {"divide-y"}));
  elements.push_back(MakeElement("li", 2, 8));

  CriticalCssExtractor extractor;
  CriticalCssResult result =
      extractor.Extract(elements,
                        ".y[class~=c]{order:1}\n"
                        ".y:not([class~=q]){order:2}\n"
                        "li:nth-child(2n+1){order:3}\n"
                        "input[type=text i]{order:4}\n"
                        ".note:not(.a > .b){order:5}\n"
                        ":where(.space-y-4>:not(:last-child)){order:6}\n"
                        ":where(.divide-y>:not(:last-child)){order:7}\n"
                        ".absent>:not(:last-child){order:8}\n"
                        "input[type=text i].wide{order:9}\n");
  ASSERT_TRUE(result.success);
  for (int kept : {1, 2, 3, 4, 5, 6, 7}) {
    EXPECT_NE(result.critical_css.find(absl::StrCat("order:", kept, "}")),
              std::string::npos)
        << "rule " << kept << " is kept\n"
        << result.critical_css;
  }
  EXPECT_EQ(result.critical_css.find("order:8}"), std::string::npos)
      << "the stand-in compound must exist in the fold";
  EXPECT_EQ(result.critical_css.find("order:9}"), std::string::npos)
      << "the compound goes on after an attribute selector";
}

// A rule whose ancestor compounds name a class or id the page never uses
// cannot apply anywhere on it, so it is left out even though its subject is
// in the fold: Tailwind Typography's `.prose :where(p)` rules on a page that
// uses `.prose` only elsewhere. The ancestor may be anywhere in
// the document, not only in the fold.
TEST(CriticalCssExtractorTest, AncestorCompoundsMustBeOnThePage) {
  const std::string css =
      ".prose :where(p):not(:where([class~=not-prose] *)){margin-top:1em}\n"
      "#article .lead p{color:red}\n";
  std::vector<CollectedElement> without;
  without.push_back(MakeElement("body", 0, 0));
  without.push_back(MakeElement("p", 1, 1));
  CriticalCssConfig config;
  config.max_elements = 3;
  CriticalCssExtractor extractor(config);
  CriticalCssResult dropped = extractor.Extract(without, css);
  ASSERT_TRUE(dropped.success);
  EXPECT_EQ(dropped.critical_css.find("margin-top"), std::string::npos)
      << dropped.critical_css;
  EXPECT_EQ(dropped.critical_css.find("color:red"), std::string::npos);

  std::vector<CollectedElement> with = without;
  with.push_back(MakeElement("article", 1, 2, "article", {"prose"}));
  with.push_back(MakeElement("div", 2, 3, "", {"lead"}));
  for (int i = 4; i < 20; ++i) with.push_back(MakeElement("p", 2, i));
  CriticalCssResult kept = extractor.Extract(with, css);
  ASSERT_TRUE(kept.success);
  EXPECT_NE(kept.critical_css.find("margin-top"), std::string::npos)
      << "with .prose on the page (here past the fold estimate), kept";
  EXPECT_NE(kept.critical_css.find("color:red"), std::string::npos)
      << "an id and a class ancestor, both on the page";
}

// Not checked: a class inside :where()/:is()/:not() (Tailwind v4's dark
// variant), the root's state classes on html/body/:root, and the bare root
// state classes script commonly adds before first paint (Tailwind v3's
// `.dark .x`, `.js .x`), which the markup may not carry.
TEST(CriticalCssExtractorTest, ScriptStateAncestorsAreNotRequired) {
  std::vector<CollectedElement> elements;
  elements.push_back(MakeElement("html", 0, 0));
  elements.push_back(MakeElement("body", 1, 1));
  elements.push_back(MakeElement("div", 2, 2, "", {"x"}));
  CriticalCssExtractor extractor;
  CriticalCssResult result =
      extractor.Extract(elements,
                        ":where(.dark) .x{order:1}\n"
                        ".x:where(.dark, .dark *){order:2}\n"
                        "html.dark .x{order:3}\n"
                        ":root.js .x{order:4}\n"
                        "body.menu-open .x{order:5}\n"
                        ".dark .x{order:6}\n"
                        ".js .x{order:7}\n"
                        ".blog-only .x{order:8}\n");
  ASSERT_TRUE(result.success);
  for (int kept : {1, 2, 3, 4, 5, 6, 7}) {
    EXPECT_NE(result.critical_css.find(absl::StrCat("order:", kept, "}")),
              std::string::npos)
        << "rule " << kept << "\n"
        << result.critical_css;
  }
  EXPECT_EQ(result.critical_css.find("order:8}"), std::string::npos)
      << "any other ancestor class the page never uses";
}

}  // namespace
}  // namespace pagespeed
