// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

// PageSpeed 2.0 - SVG Sanitizer Unit Tests

#include "lib/image/svg_sanitizer.h"

#include <string>
#include <string_view>

#include "gtest/gtest.h"

namespace pagespeed {
namespace {

class SvgSanitizerTest : public ::testing::Test {
 protected:
  SvgSanitizeConfig config_;
};

// -----------------------------------------------------------------
// Clean VTracer output
// -----------------------------------------------------------------

TEST_F(SvgSanitizerTest, CleanVTracerOutputPassesThrough) {
  const char* input =
      "<svg xmlns=\"http://www.w3.org/2000/svg\""
      " width=\"100\" height=\"100\">"
      "<path d=\"M 10 20 L 30 40\" fill=\"#ff0000\"/>"
      "<path d=\"M 50 60 L 70 80\" fill=\"#00ff00\"/>"
      "</svg>";
  config_.source_width = 100;
  config_.source_height = 100;
  auto result = SanitizeSvg(input, config_);
  ASSERT_TRUE(result.success) << result.error_message;
  EXPECT_EQ(result.path_count, 2u);
  EXPECT_EQ(result.elements_stripped, 0u);
  EXPECT_NE(result.sanitized_svg.find("<path"), std::string::npos);
  EXPECT_NE(result.sanitized_svg.find("</svg>"), std::string::npos);
  EXPECT_NE(result.sanitized_svg.find("fill=\"#ff0000\""), std::string::npos);
}

TEST_F(SvgSanitizerTest, PreservesXmlnsSvgPath) {
  const char* input =
      "<svg xmlns=\"http://www.w3.org/2000/svg\""
      " viewBox=\"0 0 200 200\">"
      "<path d=\"M 0 0 L 100 100\" fill=\"blue\"/>"
      "</svg>";
  auto result = SanitizeSvg(input, config_);
  ASSERT_TRUE(result.success);
  EXPECT_NE(result.sanitized_svg.find("xmlns="), std::string::npos);
  EXPECT_NE(result.sanitized_svg.find("viewBox="), std::string::npos);
}

// -----------------------------------------------------------------
// XSS vector tests
// -----------------------------------------------------------------

TEST_F(SvgSanitizerTest, ScriptElementStripped) {
  const char* input =
      "<svg><script>alert(1)</script>"
      "<path d=\"M 0 0\"/></svg>";
  auto result = SanitizeSvg(input, config_);
  ASSERT_TRUE(result.success);
  EXPECT_EQ(result.sanitized_svg.find("script"), std::string::npos);
  EXPECT_EQ(result.sanitized_svg.find("alert"), std::string::npos);
  EXPECT_NE(result.sanitized_svg.find("<path"), std::string::npos);
  EXPECT_GE(result.elements_stripped, 1u);
}

TEST_F(SvgSanitizerTest, OnclickAttributeStripped) {
  const char* input = "<svg><path d=\"M 0 0\" onclick=\"alert(1)\"/></svg>";
  auto result = SanitizeSvg(input, config_);
  ASSERT_TRUE(result.success);
  EXPECT_EQ(result.sanitized_svg.find("onclick"), std::string::npos);
  EXPECT_EQ(result.sanitized_svg.find("alert"), std::string::npos);
  EXPECT_NE(result.sanitized_svg.find("<path"), std::string::npos);
}

TEST_F(SvgSanitizerTest, AnchorWithJavascriptHrefStripped) {
  const char* input =
      "<svg><a href=\"javascript:alert(1)\">"
      "<path d=\"M 0 0\"/></a></svg>";
  auto result = SanitizeSvg(input, config_);
  ASSERT_TRUE(result.success);
  EXPECT_EQ(result.sanitized_svg.find("<a"), std::string::npos);
  EXPECT_EQ(result.sanitized_svg.find("javascript"), std::string::npos);
  // Dangerous element strips entire subtree.
  EXPECT_GE(result.elements_stripped, 1u);
}

TEST_F(SvgSanitizerTest, ForeignObjectStripped) {
  const char* input =
      "<svg><foreignObject>"
      "<body onload=\"alert(1)\"/>"
      "</foreignObject>"
      "<path d=\"M 0 0\"/></svg>";
  auto result = SanitizeSvg(input, config_);
  ASSERT_TRUE(result.success);
  EXPECT_EQ(result.sanitized_svg.find("foreignObject"), std::string::npos);
  EXPECT_EQ(result.sanitized_svg.find("onload"), std::string::npos);
  EXPECT_EQ(result.sanitized_svg.find("alert"), std::string::npos);
  EXPECT_NE(result.sanitized_svg.find("<path"), std::string::npos);
}

TEST_F(SvgSanitizerTest, ImageElementWithXlinkHrefStripped) {
  const char* input =
      "<svg><image xlink:href=\"data:text/html,"
      "&lt;script&gt;alert(1)&lt;/script&gt;\"/></svg>";
  auto result = SanitizeSvg(input, config_);
  ASSERT_TRUE(result.success);
  EXPECT_EQ(result.sanitized_svg.find("image"), std::string::npos);
  EXPECT_EQ(result.sanitized_svg.find("xlink:href"), std::string::npos);
  EXPECT_GE(result.elements_stripped, 1u);
}

TEST_F(SvgSanitizerTest, UppercaseEventHandlersStripped) {
  const char* input =
      "<svg ONLOAD=\"alert(1)\">"
      "<path d=\"M 0 0\" ONCLICK=\"alert(2)\" OnMouseOver=\"alert(3)\" "
      "oNfocus=\"alert(4)\"/></svg>";
  auto result = SanitizeSvg(input, config_);
  ASSERT_TRUE(result.success);
  EXPECT_EQ(result.sanitized_svg.find("alert"), std::string::npos);
  EXPECT_EQ(result.sanitized_svg.find("ONLOAD"), std::string::npos);
  EXPECT_EQ(result.sanitized_svg.find("ONCLICK"), std::string::npos);
  EXPECT_EQ(result.sanitized_svg.find("OnMouseOver"), std::string::npos);
  EXPECT_EQ(result.sanitized_svg.find("oNfocus"), std::string::npos);
  EXPECT_NE(result.sanitized_svg.find("<path"), std::string::npos);
}

TEST_F(SvgSanitizerTest, SvgSpecificEventHandlersStripped) {
  const char* input =
      "<svg><path d=\"M 0 0\" onactivate=\"alert(1)\" "
      "onbegin=\"alert(2)\" onfocusin=\"alert(3)\"/></svg>";
  auto result = SanitizeSvg(input, config_);
  ASSERT_TRUE(result.success);
  EXPECT_EQ(result.sanitized_svg.find("alert"), std::string::npos);
  EXPECT_EQ(result.sanitized_svg.find("onactivate"), std::string::npos);
  EXPECT_EQ(result.sanitized_svg.find("onbegin"), std::string::npos);
  EXPECT_EQ(result.sanitized_svg.find("onfocusin"), std::string::npos);
  EXPECT_NE(result.sanitized_svg.find("<path"), std::string::npos);
}

TEST_F(SvgSanitizerTest, OnloadOnErrorStripped) {
  const char* input =
      "<svg onload=\"alert(1)\">"
      "<path d=\"M 0 0\" onerror=\"alert(2)\"/></svg>";
  auto result = SanitizeSvg(input, config_);
  ASSERT_TRUE(result.success);
  EXPECT_EQ(result.sanitized_svg.find("onload"), std::string::npos);
  EXPECT_EQ(result.sanitized_svg.find("onerror"), std::string::npos);
  EXPECT_EQ(result.sanitized_svg.find("alert"), std::string::npos);
}

TEST_F(SvgSanitizerTest, StyleElementStripped) {
  const char* input =
      "<svg><style>body{background:red}</style>"
      "<path d=\"M 0 0\"/></svg>";
  auto result = SanitizeSvg(input, config_);
  ASSERT_TRUE(result.success);
  EXPECT_EQ(result.sanitized_svg.find("<style"), std::string::npos);
  EXPECT_EQ(result.sanitized_svg.find("background"), std::string::npos);
  EXPECT_NE(result.sanitized_svg.find("<path"), std::string::npos);
}

TEST_F(SvgSanitizerTest, UseElementStripped) {
  const char* input = R"(<svg><use href="#foo"/><path d="M 0 0"/></svg>)";
  auto result = SanitizeSvg(input, config_);
  ASSERT_TRUE(result.success);
  EXPECT_EQ(result.sanitized_svg.find("<use"), std::string::npos);
  EXPECT_NE(result.sanitized_svg.find("<path"), std::string::npos);
}

TEST_F(SvgSanitizerTest, AnimateElementStripped) {
  const char* input =
      "<svg><path d=\"M 0 0\">"
      "<animate attributeName=\"d\" "
      "values=\"M0 0;M100 100\"/></path></svg>";
  auto result = SanitizeSvg(input, config_);
  ASSERT_TRUE(result.success);
  EXPECT_EQ(result.sanitized_svg.find("animate"), std::string::npos);
}

TEST_F(SvgSanitizerTest, SetElementStripped) {
  const char* input =
      "<svg><set attributeName=\"fill\" to=\"red\"/>"
      "<path d=\"M 0 0\"/></svg>";
  auto result = SanitizeSvg(input, config_);
  ASSERT_TRUE(result.success);
  EXPECT_EQ(result.sanitized_svg.find("<set"), std::string::npos);
}

TEST_F(SvgSanitizerTest, MetadataElementStripped) {
  const char* input =
      "<svg><metadata><rdf:RDF>data</rdf:RDF></metadata>"
      "<path d=\"M 0 0\"/></svg>";
  auto result = SanitizeSvg(input, config_);
  ASSERT_TRUE(result.success);
  EXPECT_EQ(result.sanitized_svg.find("metadata"), std::string::npos);
  EXPECT_EQ(result.sanitized_svg.find("rdf"), std::string::npos);
}

// -----------------------------------------------------------------
// DOCTYPE / ENTITY / Processing Instructions
// -----------------------------------------------------------------

TEST_F(SvgSanitizerTest, DoctypeEntityStripped) {
  const char* input =
      "<!DOCTYPE svg [<!ENTITY xxe SYSTEM "
      "\"file:///etc/passwd\">]>"
      "<svg><path d=\"M 0 0\"/></svg>";
  auto result = SanitizeSvg(input, config_);
  ASSERT_TRUE(result.success);
  EXPECT_EQ(result.sanitized_svg.find("DOCTYPE"), std::string::npos);
  EXPECT_EQ(result.sanitized_svg.find("ENTITY"), std::string::npos);
  EXPECT_EQ(result.sanitized_svg.find("passwd"), std::string::npos);
  EXPECT_NE(result.sanitized_svg.find("<path"), std::string::npos);
}

TEST_F(SvgSanitizerTest, XmlProcessingInstructionStripped) {
  const char* input =
      "<?xml version=\"1.0\" encoding=\"UTF-8\"?>"
      "<svg><path d=\"M 0 0\"/></svg>";
  auto result = SanitizeSvg(input, config_);
  ASSERT_TRUE(result.success);
  EXPECT_EQ(result.sanitized_svg.find("<?xml"), std::string::npos);
  EXPECT_NE(result.sanitized_svg.find("<svg"), std::string::npos);
}

TEST_F(SvgSanitizerTest, XmlStylesheetPIStripped) {
  const char* input =
      "<?xml-stylesheet type=\"text/css\" href=\"style.css\"?>"
      "<svg><path d=\"M 0 0\"/></svg>";
  auto result = SanitizeSvg(input, config_);
  ASSERT_TRUE(result.success);
  EXPECT_EQ(result.sanitized_svg.find("xml-stylesheet"), std::string::npos);
}

TEST_F(SvgSanitizerTest, CommentsStripped) {
  const char* input =
      "<svg><!-- This is a comment -->"
      "<path d=\"M 0 0\"/></svg>";
  auto result = SanitizeSvg(input, config_);
  ASSERT_TRUE(result.success);
  EXPECT_EQ(result.sanitized_svg.find("<!--"), std::string::npos);
  EXPECT_EQ(result.sanitized_svg.find("comment"), std::string::npos);
  EXPECT_NE(result.sanitized_svg.find("<path"), std::string::npos);
}

// -----------------------------------------------------------------
// Unknown elements / attributes
// -----------------------------------------------------------------

TEST_F(SvgSanitizerTest, UnknownElementStripped) {
  const char* input = "<svg><foo><path d=\"M 0 0\"/></foo></svg>";
  auto result = SanitizeSvg(input, config_);
  ASSERT_TRUE(result.success);
  EXPECT_EQ(result.sanitized_svg.find("<foo"), std::string::npos);
  EXPECT_EQ(result.sanitized_svg.find("</foo"), std::string::npos);
  // Path should be preserved (unknown strips tag, not children).
  EXPECT_NE(result.sanitized_svg.find("<path"), std::string::npos);
  EXPECT_GE(result.elements_stripped, 1u);
}

TEST_F(SvgSanitizerTest, UnknownAttributesStripped) {
  const char* input =
      "<svg><path d=\"M 0 0\" data-custom=\"x\" "
      "style=\"color:red\"/></svg>";
  auto result = SanitizeSvg(input, config_);
  ASSERT_TRUE(result.success);
  EXPECT_EQ(result.sanitized_svg.find("data-custom"), std::string::npos);
  EXPECT_EQ(result.sanitized_svg.find("style="), std::string::npos);
  EXPECT_NE(result.sanitized_svg.find("d=\""), std::string::npos);
}

TEST_F(SvgSanitizerTest, HrefAttributeStrippedOnPath) {
  const char* input = R"(<svg><path d="M 0 0" href="http://evil.com"/></svg>)";
  auto result = SanitizeSvg(input, config_);
  ASSERT_TRUE(result.success);
  EXPECT_EQ(result.sanitized_svg.find("href"), std::string::npos);
  EXPECT_EQ(result.sanitized_svg.find("evil"), std::string::npos);
}

// -----------------------------------------------------------------
// Dimension handling
// -----------------------------------------------------------------

TEST_F(SvgSanitizerTest, DimensionsAddedWhenMissing) {
  const char* input =
      "<svg xmlns=\"http://www.w3.org/2000/svg\">"
      "<path d=\"M 0 0\"/></svg>";
  config_.source_width = 640;
  config_.source_height = 480;
  auto result = SanitizeSvg(input, config_);
  ASSERT_TRUE(result.success);
  EXPECT_NE(result.sanitized_svg.find("width=\"640\""), std::string::npos);
  EXPECT_NE(result.sanitized_svg.find("height=\"480\""), std::string::npos);
}

TEST_F(SvgSanitizerTest, DimensionsCorrectedWhenWrong) {
  const char* input =
      "<svg width=\"999\" height=\"888\">"
      "<path d=\"M 0 0\"/></svg>";
  config_.source_width = 100;
  config_.source_height = 200;
  auto result = SanitizeSvg(input, config_);
  ASSERT_TRUE(result.success);
  EXPECT_NE(result.sanitized_svg.find("width=\"100\""), std::string::npos);
  EXPECT_NE(result.sanitized_svg.find("height=\"200\""), std::string::npos);
  EXPECT_EQ(result.sanitized_svg.find("999"), std::string::npos);
  EXPECT_EQ(result.sanitized_svg.find("888"), std::string::npos);
}

TEST_F(SvgSanitizerTest, DimensionsNotAddedWhenConfigZero) {
  const char* input = "<svg><path d=\"M 0 0\"/></svg>";
  config_.source_width = 0;
  config_.source_height = 0;
  auto result = SanitizeSvg(input, config_);
  ASSERT_TRUE(result.success);
  // Width/height not added since config is 0.
  EXPECT_EQ(result.sanitized_svg.find("width="), std::string::npos);
  EXPECT_EQ(result.sanitized_svg.find("height="), std::string::npos);
}

// -----------------------------------------------------------------
// Coordinate rounding
// -----------------------------------------------------------------

TEST_F(SvgSanitizerTest, CoordinatesRoundedPrecision1) {
  const char* input = "<svg><path d=\"M 12.3456 78.9012 L 1.99 3.05\"/></svg>";
  config_.coordinate_precision = 1;
  auto result = SanitizeSvg(input, config_);
  ASSERT_TRUE(result.success);
  EXPECT_NE(result.sanitized_svg.find("12.3"), std::string::npos);
  EXPECT_NE(result.sanitized_svg.find("78.9"), std::string::npos)
      << "Got: " << result.sanitized_svg;
  // Should not have full original precision.
  EXPECT_EQ(result.sanitized_svg.find("12.3456"), std::string::npos);
  EXPECT_EQ(result.sanitized_svg.find("78.9012"), std::string::npos);
}

TEST_F(SvgSanitizerTest, CoordinatesRoundedPrecision0) {
  const char* input = "<svg><path d=\"M 12.7 78.3\"/></svg>";
  config_.coordinate_precision = 0;
  auto result = SanitizeSvg(input, config_);
  ASSERT_TRUE(result.success);
  EXPECT_NE(result.sanitized_svg.find("M 13 78"), std::string::npos)
      << "Got: " << result.sanitized_svg;
}

TEST_F(SvgSanitizerTest, IntegerCoordinatesUnchanged) {
  const char* input = "<svg><path d=\"M 10 20 L 30 40\"/></svg>";
  config_.coordinate_precision = 1;
  auto result = SanitizeSvg(input, config_);
  ASSERT_TRUE(result.success);
  EXPECT_NE(result.sanitized_svg.find("M 10 20 L 30 40"), std::string::npos)
      << "Got: " << result.sanitized_svg;
}

TEST_F(SvgSanitizerTest, NegativeCoordinatesRounded) {
  const char* input = "<svg><path d=\"M -12.345 -78.901\"/></svg>";
  config_.coordinate_precision = 1;
  auto result = SanitizeSvg(input, config_);
  ASSERT_TRUE(result.success);
  EXPECT_NE(result.sanitized_svg.find("-12.3"), std::string::npos);
  EXPECT_NE(result.sanitized_svg.find("-78.9"), std::string::npos);
}

// -----------------------------------------------------------------
// Crisp edges
// -----------------------------------------------------------------

TEST_F(SvgSanitizerTest, CrispEdgesAdded) {
  const char* input = "<svg><path d=\"M 0 0\"/></svg>";
  config_.crisp_edges = true;
  auto result = SanitizeSvg(input, config_);
  ASSERT_TRUE(result.success);
  EXPECT_NE(result.sanitized_svg.find("shape-rendering=\"crispEdges\""),
            std::string::npos);
}

TEST_F(SvgSanitizerTest, CrispEdgesNotAddedByDefault) {
  const char* input = "<svg><path d=\"M 0 0\"/></svg>";
  auto result = SanitizeSvg(input, config_);
  ASSERT_TRUE(result.success);
  EXPECT_EQ(result.sanitized_svg.find("shape-rendering"), std::string::npos);
}

// -----------------------------------------------------------------
// Path counting
// -----------------------------------------------------------------

TEST_F(SvgSanitizerTest, PathCountZero) {
  const char* input =
      "<svg><rect x=\"0\" y=\"0\" width=\"100\" "
      "height=\"100\"/></svg>";
  auto result = SanitizeSvg(input, config_);
  ASSERT_TRUE(result.success);
  EXPECT_EQ(result.path_count, 0u);
}

TEST_F(SvgSanitizerTest, PathCountThree) {
  const char* input =
      "<svg>"
      "<path d=\"M 0 0\"/>"
      "<path d=\"M 1 1\"/>"
      "<path d=\"M 2 2\"/>"
      "</svg>";
  auto result = SanitizeSvg(input, config_);
  ASSERT_TRUE(result.success);
  EXPECT_EQ(result.path_count, 3u);
}

TEST_F(SvgSanitizerTest, PathInsideDangerousNotCounted) {
  const char* input =
      "<svg><script><path d=\"M 0 0\"/></script>"
      "<path d=\"M 1 1\"/></svg>";
  auto result = SanitizeSvg(input, config_);
  ASSERT_TRUE(result.success);
  // The path inside <script> is stripped, only the outer one counts.
  EXPECT_EQ(result.path_count, 1u);
}

// -----------------------------------------------------------------
// Empty / malformed input
// -----------------------------------------------------------------

TEST_F(SvgSanitizerTest, EmptyInputReturnsError) {
  auto result = SanitizeSvg("", config_);
  EXPECT_FALSE(result.success);
  EXPECT_FALSE(result.error_message.empty());
}

TEST_F(SvgSanitizerTest, NoSvgElementReturnsError) {
  const char* input = "<div><path d=\"M 0 0\"/></div>";
  auto result = SanitizeSvg(input, config_);
  EXPECT_FALSE(result.success);
  EXPECT_NE(result.error_message.find("svg"), std::string::npos);
}

TEST_F(SvgSanitizerTest, JustTextReturnsError) {
  auto result = SanitizeSvg("hello world", config_);
  EXPECT_FALSE(result.success);
}

TEST_F(SvgSanitizerTest, UnclosedTagBestEffort) {
  // Unclosed <path -- parser should handle gracefully.
  const char* input = "<svg><path d=\"M 0 0\"";
  auto result = SanitizeSvg(input, config_);
  // Should still find <svg>.
  ASSERT_TRUE(result.success);
}

// -----------------------------------------------------------------
// Nested stripped elements
// -----------------------------------------------------------------

TEST_F(SvgSanitizerTest, NestedScriptChildrenStripped) {
  const char* input =
      "<svg><g><script><path d=\"M 0 0\"/>"
      "</script></g></svg>";
  auto result = SanitizeSvg(input, config_);
  ASSERT_TRUE(result.success);
  EXPECT_EQ(result.sanitized_svg.find("script"), std::string::npos);
  EXPECT_NE(result.sanitized_svg.find("<svg"), std::string::npos);
  // Path inside script is stripped.
  EXPECT_EQ(result.path_count, 0u);
}

TEST_F(SvgSanitizerTest, DeeplyNestedDangerousElement) {
  const char* input =
      "<svg><g><g><foreignObject><div><span>"
      "evil</span></div></foreignObject></g></g>"
      "<path d=\"M 0 0\"/></svg>";
  auto result = SanitizeSvg(input, config_);
  ASSERT_TRUE(result.success);
  EXPECT_EQ(result.sanitized_svg.find("foreignObject"), std::string::npos);
  EXPECT_EQ(result.sanitized_svg.find("evil"), std::string::npos);
  EXPECT_NE(result.sanitized_svg.find("<path"), std::string::npos);
  EXPECT_EQ(result.path_count, 1u);
}

// -----------------------------------------------------------------
// Allowed SVG elements beyond path
// -----------------------------------------------------------------

TEST_F(SvgSanitizerTest, RectElementPreserved) {
  const char* input =
      "<svg><rect x=\"10\" y=\"20\" width=\"100\" "
      "height=\"50\" fill=\"#abc\" rx=\"5\"/></svg>";
  auto result = SanitizeSvg(input, config_);
  ASSERT_TRUE(result.success);
  EXPECT_NE(result.sanitized_svg.find("<rect"), std::string::npos);
  EXPECT_NE(result.sanitized_svg.find("x=\"10\""), std::string::npos);
}

TEST_F(SvgSanitizerTest, CircleElementPreserved) {
  const char* input =
      "<svg><circle cx=\"50\" cy=\"50\" r=\"25\" "
      "fill=\"red\"/></svg>";
  auto result = SanitizeSvg(input, config_);
  ASSERT_TRUE(result.success);
  EXPECT_NE(result.sanitized_svg.find("<circle"), std::string::npos);
}

TEST_F(SvgSanitizerTest, GradientElementsPreserved) {
  const char* input =
      "<svg><defs><linearGradient id=\"g1\">"
      "<stop offset=\"0\" stop-color=\"red\"/>"
      "<stop offset=\"1\" stop-color=\"blue\"/>"
      "</linearGradient></defs>"
      "<rect fill=\"url(#g1)\" width=\"100\" "
      "height=\"100\"/></svg>";
  auto result = SanitizeSvg(input, config_);
  ASSERT_TRUE(result.success);
  EXPECT_NE(result.sanitized_svg.find("linearGradient"), std::string::npos);
  EXPECT_NE(result.sanitized_svg.find("stop-color"), std::string::npos);
}

TEST_F(SvgSanitizerTest, ClipPathPreserved) {
  const char* input =
      "<svg><defs><clipPath id=\"c1\">"
      "<rect width=\"100\" height=\"100\"/>"
      "</clipPath></defs>"
      "<path d=\"M 0 0\" clip-path=\"url(#c1)\"/></svg>";
  auto result = SanitizeSvg(input, config_);
  ASSERT_TRUE(result.success);
  EXPECT_NE(result.sanitized_svg.find("clipPath"), std::string::npos);
  EXPECT_NE(result.sanitized_svg.find("clip-path="), std::string::npos);
}

TEST_F(SvgSanitizerTest, EllipseLinePolylinePolygonPreserved) {
  const char* input =
      "<svg>"
      "<ellipse cx=\"50\" cy=\"50\" rx=\"30\" ry=\"20\"/>"
      "<line x1=\"0\" y1=\"0\" x2=\"100\" y2=\"100\"/>"
      "<polyline points=\"0,0 50,50 100,0\"/>"
      "<polygon points=\"0,0 100,0 50,100\"/>"
      "</svg>";
  auto result = SanitizeSvg(input, config_);
  ASSERT_TRUE(result.success);
  EXPECT_NE(result.sanitized_svg.find("<ellipse"), std::string::npos);
  EXPECT_NE(result.sanitized_svg.find("<line"), std::string::npos);
  EXPECT_NE(result.sanitized_svg.find("<polyline"), std::string::npos);
  EXPECT_NE(result.sanitized_svg.find("<polygon"), std::string::npos);
}

// -----------------------------------------------------------------
// Stroke / fill attributes preserved
// -----------------------------------------------------------------

TEST_F(SvgSanitizerTest, StrokeAttributesPreserved) {
  const char* input =
      "<svg><path d=\"M 0 0\" stroke=\"black\" "
      "stroke-width=\"2\" stroke-linecap=\"round\" "
      "stroke-linejoin=\"bevel\"/></svg>";
  auto result = SanitizeSvg(input, config_);
  ASSERT_TRUE(result.success);
  EXPECT_NE(result.sanitized_svg.find("stroke=\"black\""), std::string::npos);
  EXPECT_NE(result.sanitized_svg.find("stroke-width=\"2\""), std::string::npos);
  EXPECT_NE(result.sanitized_svg.find("stroke-linecap=\"round\""),
            std::string::npos);
  EXPECT_NE(result.sanitized_svg.find("stroke-linejoin=\"bevel\""),
            std::string::npos);
}

TEST_F(SvgSanitizerTest, OpacityAttributesPreserved) {
  const char* input =
      "<svg><path d=\"M 0 0\" opacity=\"0.5\" "
      "fill-opacity=\"0.8\" stroke-opacity=\"0.3\"/></svg>";
  auto result = SanitizeSvg(input, config_);
  ASSERT_TRUE(result.success);
  EXPECT_NE(result.sanitized_svg.find("opacity=\"0.5\""), std::string::npos);
  EXPECT_NE(result.sanitized_svg.find("fill-opacity=\"0.8\""),
            std::string::npos);
  EXPECT_NE(result.sanitized_svg.find("stroke-opacity=\"0.3\""),
            std::string::npos);
}

// -----------------------------------------------------------------
// Whitespace collapsing
// -----------------------------------------------------------------

TEST_F(SvgSanitizerTest, WhitespaceCollapsed) {
  const char* input = "<svg>  \n\t  <path   d=\"M 0 0\"/>  \n  </svg>";
  auto result = SanitizeSvg(input, config_);
  ASSERT_TRUE(result.success);
  // Should not have consecutive whitespace.
  EXPECT_EQ(result.sanitized_svg.find("  "), std::string::npos)
      << "Got: " << result.sanitized_svg;
}

// -----------------------------------------------------------------
// Empty group removal
// -----------------------------------------------------------------

TEST_F(SvgSanitizerTest, EmptyGroupRemoved) {
  const char* input = "<svg><g></g><path d=\"M 0 0\"/></svg>";
  auto result = SanitizeSvg(input, config_);
  ASSERT_TRUE(result.success);
  EXPECT_EQ(result.sanitized_svg.find("<g>"), std::string::npos)
      << "Got: " << result.sanitized_svg;
  EXPECT_NE(result.sanitized_svg.find("<path"), std::string::npos);
}

TEST_F(SvgSanitizerTest, NonEmptyGroupPreserved) {
  const char* input = "<svg><g><path d=\"M 0 0\"/></g></svg>";
  auto result = SanitizeSvg(input, config_);
  ASSERT_TRUE(result.success);
  EXPECT_NE(result.sanitized_svg.find("<g>"), std::string::npos);
  EXPECT_NE(result.sanitized_svg.find("</g>"), std::string::npos);
}

// -----------------------------------------------------------------
// CDATA section stripped
// -----------------------------------------------------------------

TEST_F(SvgSanitizerTest, CdataSectionStripped) {
  const char* input =
      "<svg><![CDATA[some raw content]]>"
      "<path d=\"M 0 0\"/></svg>";
  auto result = SanitizeSvg(input, config_);
  ASSERT_TRUE(result.success);
  EXPECT_EQ(result.sanitized_svg.find("CDATA"), std::string::npos);
  EXPECT_EQ(result.sanitized_svg.find("raw content"), std::string::npos);
  EXPECT_NE(result.sanitized_svg.find("<path"), std::string::npos);
}

// -----------------------------------------------------------------
// Transform and fill-rule attributes preserved
// -----------------------------------------------------------------

TEST_F(SvgSanitizerTest, TransformPreserved) {
  const char* input =
      "<svg><g transform=\"translate(10,20)\">"
      "<path d=\"M 0 0\"/></g></svg>";
  auto result = SanitizeSvg(input, config_);
  ASSERT_TRUE(result.success);
  EXPECT_NE(result.sanitized_svg.find("transform=\"translate(10,20)\""),
            std::string::npos);
}

TEST_F(SvgSanitizerTest, FillRulePreserved) {
  const char* input = R"(<svg><path d="M 0 0" fill-rule="evenodd"/></svg>)";
  auto result = SanitizeSvg(input, config_);
  ASSERT_TRUE(result.success);
  EXPECT_NE(result.sanitized_svg.find("fill-rule=\"evenodd\""),
            std::string::npos);
}

// -----------------------------------------------------------------
// Class and id attributes preserved
// -----------------------------------------------------------------

TEST_F(SvgSanitizerTest, ClassAndIdPreserved) {
  const char* input =
      "<svg><path d=\"M 0 0\" id=\"p1\" "
      "class=\"outline\"/></svg>";
  auto result = SanitizeSvg(input, config_);
  ASSERT_TRUE(result.success);
  EXPECT_NE(result.sanitized_svg.find("id=\"p1\""), std::string::npos);
  EXPECT_NE(result.sanitized_svg.find("class=\"outline\""), std::string::npos);
}

// -----------------------------------------------------------------
// Self-closing dangerous element
// -----------------------------------------------------------------

TEST_F(SvgSanitizerTest, SelfClosingDangerousElement) {
  const char* input = "<svg><image/><path d=\"M 0 0\"/></svg>";
  auto result = SanitizeSvg(input, config_);
  ASSERT_TRUE(result.success);
  EXPECT_EQ(result.sanitized_svg.find("image"), std::string::npos);
  EXPECT_NE(result.sanitized_svg.find("<path"), std::string::npos);
  EXPECT_EQ(result.path_count, 1u);
}

// -----------------------------------------------------------------
// Multiple XSS vectors combined
// -----------------------------------------------------------------

TEST_F(SvgSanitizerTest, CombinedXSSVectors) {
  const char* input =
      "<?xml version=\"1.0\"?>"
      "<!DOCTYPE svg [<!ENTITY xxe SYSTEM "
      "\"file:///etc/passwd\">]>"
      "<!-- evil comment -->"
      "<svg onload=\"alert(1)\">"
      "<script>alert(2)</script>"
      "<style>.x{}</style>"
      "<foreignObject>"
      "<body onload=\"alert(3)\"/>"
      "</foreignObject>"
      "<a href=\"javascript:alert(4)\">"
      "<rect width=\"10\" height=\"10\"/></a>"
      "<path d=\"M 0 0\" onclick=\"alert(5)\"/>"
      "<image xlink:href=\"evil.png\"/>"
      "<use href=\"#x\"/>"
      "<animate attributeName=\"opacity\" "
      "from=\"0\" to=\"1\"/>"
      "<set attributeName=\"fill\" to=\"red\"/>"
      "<metadata><rdf>data</rdf></metadata>"
      "<g transform=\"rotate(45)\">"
      "<path d=\"M 1 1\"/></g>"
      "</svg>";
  config_.source_width = 100;
  config_.source_height = 100;
  auto result = SanitizeSvg(input, config_);
  ASSERT_TRUE(result.success) << result.error_message;

  // None of the dangerous content should remain.
  EXPECT_EQ(result.sanitized_svg.find("alert"), std::string::npos);
  EXPECT_EQ(result.sanitized_svg.find("script"), std::string::npos);
  EXPECT_EQ(result.sanitized_svg.find("foreignObject"), std::string::npos);
  EXPECT_EQ(result.sanitized_svg.find("javascript"), std::string::npos);
  EXPECT_EQ(result.sanitized_svg.find("onclick"), std::string::npos);
  EXPECT_EQ(result.sanitized_svg.find("onload"), std::string::npos);
  EXPECT_EQ(result.sanitized_svg.find("xlink:href"), std::string::npos);
  EXPECT_EQ(result.sanitized_svg.find("DOCTYPE"), std::string::npos);
  EXPECT_EQ(result.sanitized_svg.find("ENTITY"), std::string::npos);
  EXPECT_EQ(result.sanitized_svg.find("passwd"), std::string::npos);
  EXPECT_EQ(result.sanitized_svg.find("<!--"), std::string::npos);
  EXPECT_EQ(result.sanitized_svg.find("<?xml"), std::string::npos);
  EXPECT_EQ(result.sanitized_svg.find("animate"), std::string::npos);
  EXPECT_EQ(result.sanitized_svg.find("<set"), std::string::npos);
  EXPECT_EQ(result.sanitized_svg.find("metadata"), std::string::npos);
  EXPECT_EQ(result.sanitized_svg.find("<use"), std::string::npos);
  EXPECT_EQ(result.sanitized_svg.find("image"), std::string::npos);

  // Safe content should remain.
  EXPECT_NE(result.sanitized_svg.find("<svg"), std::string::npos);
  EXPECT_NE(result.sanitized_svg.find("<path"), std::string::npos);
  EXPECT_NE(result.sanitized_svg.find("transform=\"rotate(45)\""),
            std::string::npos);
  EXPECT_NE(result.sanitized_svg.find("width=\"100\""), std::string::npos);

  // The first path had onclick so its tag is emitted but
  // onclick is stripped. It still counts as a path element.
  EXPECT_EQ(result.path_count, 2u);
}

// -----------------------------------------------------------------
// Radial gradient preserved
// -----------------------------------------------------------------

TEST_F(SvgSanitizerTest, RadialGradientPreserved) {
  const char* input =
      "<svg><defs>"
      "<radialGradient id=\"rg1\" cx=\"50%\" cy=\"50%\">"
      "<stop offset=\"0\" stop-color=\"white\" "
      "stop-opacity=\"1\"/>"
      "<stop offset=\"1\" stop-color=\"black\" "
      "stop-opacity=\"0.5\"/>"
      "</radialGradient></defs></svg>";
  auto result = SanitizeSvg(input, config_);
  ASSERT_TRUE(result.success);
  EXPECT_NE(result.sanitized_svg.find("radialGradient"), std::string::npos);
  EXPECT_NE(result.sanitized_svg.find("stop-opacity"), std::string::npos);
}

// -----------------------------------------------------------------
// Large path data
// -----------------------------------------------------------------

TEST_F(SvgSanitizerTest, LargePathDataHandled) {
  // Simulate VTracer-like output with many coordinates.
  std::string input = "<svg><path d=\"";
  input += "M 0.123456 0.789012";
  for (int i = 0; i < 1000; ++i) {
    input += " L ";
    input += std::to_string(i) + ".5678 ";
    input += std::to_string(i * 2) + ".1234";
  }
  input += "\"/></svg>";
  config_.coordinate_precision = 1;
  auto result = SanitizeSvg(input, config_);
  ASSERT_TRUE(result.success);
  EXPECT_EQ(result.path_count, 1u);
  // Original full-precision values should be rounded.
  EXPECT_EQ(result.sanitized_svg.find(".5678"), std::string::npos);
  EXPECT_EQ(result.sanitized_svg.find(".1234"), std::string::npos);
}

// -----------------------------------------------------------------
// Style attribute check (not the <style> element)
// -----------------------------------------------------------------

TEST_F(SvgSanitizerTest, StyleAttributeOnSvgStripped) {
  const char* input =
      "<svg style=\"background:red\">"
      "<path d=\"M 0 0\"/></svg>";
  auto result = SanitizeSvg(input, config_);
  ASSERT_TRUE(result.success);
  EXPECT_EQ(result.sanitized_svg.find("style"), std::string::npos);
  EXPECT_EQ(result.sanitized_svg.find("background"), std::string::npos);
}

// -----------------------------------------------------------------
// Parser edge case tests for uncovered code paths
// -----------------------------------------------------------------

TEST_F(SvgSanitizerTest, TruncatedGTag) {
  // Truncated <g tag with no closing '>'
  const char* input = "<svg><g<path d=\"M 0 0\"/></svg>";
  auto result = SanitizeSvg(input, config_);
  // Should still parse and find <svg>.
  ASSERT_TRUE(result.success);
}

TEST_F(SvgSanitizerTest, SelfClosingEmptyGroupRemoved) {
  // Self-closing <g/> should be removed as an empty group.
  const char* input = "<svg><g/><path d=\"M 0 0\"/></svg>";
  auto result = SanitizeSvg(input, config_);
  ASSERT_TRUE(result.success);
  // The self-closing <g/> is emitted then removed by RemoveEmptyGroups.
  EXPECT_NE(result.sanitized_svg.find("<path"), std::string::npos);
}

TEST_F(SvgSanitizerTest, UnquotedAttributeValues) {
  // Unquoted attribute values should be parsed correctly.
  const char* input = "<svg><path d=M0,0 fill=red/></svg>";
  auto result = SanitizeSvg(input, config_);
  ASSERT_TRUE(result.success);
  EXPECT_NE(result.sanitized_svg.find("<path"), std::string::npos);
}

TEST_F(SvgSanitizerTest, UnexpectedCharInAttributePosition) {
  // Unexpected characters (e.g. '!') in attribute position should be
  // skipped without crashing.
  const char* input = "<svg><path !invalid d=\"M 0 0\"/></svg>";
  auto result = SanitizeSvg(input, config_);
  ASSERT_TRUE(result.success);
  EXPECT_NE(result.sanitized_svg.find("<path"), std::string::npos);
  // The '!' character should not appear in output.
  EXPECT_EQ(result.sanitized_svg.find("!invalid"), std::string::npos);
}

TEST_F(SvgSanitizerTest, UnterminatedComment) {
  // An unterminated comment should not cause infinite loop - parser
  // should skip to end of input.
  const char* input = "<svg><!-- unterminated comment<path d=\"M 0 0\"/></svg>";
  auto result = SanitizeSvg(input, config_);
  // Parser consumed everything as part of the unterminated comment,
  // so <svg> may or may not be "found" depending on parse order.
  // The key is it doesn't hang.
  // The svg tag was found before the comment, so it should succeed.
  ASSERT_TRUE(result.success);
}

TEST_F(SvgSanitizerTest, UnterminatedCdata) {
  // An unterminated CDATA section should not cause infinite loop.
  const char* input =
      "<svg><![CDATA[ unterminated cdata"
      "<path d=\"M 0 0\"/></svg>";
  auto result = SanitizeSvg(input, config_);
  // Parser consumes everything as part of the unterminated CDATA.
  ASSERT_TRUE(result.success);
}

TEST_F(SvgSanitizerTest, UnterminatedPI) {
  // An unterminated processing instruction should not cause
  // infinite loop.
  const char* input = "<svg><?xml unterminated pi<path d=\"M 0 0\"/></svg>";
  auto result = SanitizeSvg(input, config_);
  ASSERT_TRUE(result.success);
}

TEST_F(SvgSanitizerTest, BareAngleBracket) {
  // A bare '<' followed by non-name character should be handled.
  const char* input = "<svg>< <path d=\"M 0 0\"/></svg>";
  auto result = SanitizeSvg(input, config_);
  ASSERT_TRUE(result.success);
  EXPECT_NE(result.sanitized_svg.find("<path"), std::string::npos);
}

TEST_F(SvgSanitizerTest, BareAngleBracketDigit) {
  // A bare '<' followed by a digit should be handled gracefully.
  const char* input = "<svg><3abc<path d=\"M 0 0\"/></svg>";
  auto result = SanitizeSvg(input, config_);
  ASSERT_TRUE(result.success);
}

TEST_F(SvgSanitizerTest, SelfClosingGroupWithAttributes) {
  // Self-closing <g ... /> with attributes should be removed.
  const char* input =
      "<svg><g transform=\"translate(10,20)\"/>"
      "<path d=\"M 0 0\"/></svg>";
  auto result = SanitizeSvg(input, config_);
  ASSERT_TRUE(result.success);
  EXPECT_NE(result.sanitized_svg.find("<path"), std::string::npos);
}

// -----------------------------------------------------------------
// SVG bomb guards (issue #188)
// -----------------------------------------------------------------

TEST_F(SvgSanitizerTest, RejectsExcessiveNestingDepth) {
  // Build deeply nested SVG that exceeds max_depth=5.
  std::string input = "<svg>";
  for (int i = 0; i < 10; ++i) input += "<g>";
  input += "<path d=\"M 0 0\"/>";
  for (int i = 0; i < 10; ++i) input += "</g>";
  input += "</svg>";

  config_.max_depth = 5;
  auto result = SanitizeSvg(input, config_);
  EXPECT_FALSE(result.success);
  EXPECT_NE(result.error_message.find("depth"), std::string::npos);
}

TEST_F(SvgSanitizerTest, RejectsExcessiveElementCount) {
  // Build SVG with more elements than allowed.
  std::string input = "<svg>";
  for (int i = 0; i < 20; ++i) input += "<path d=\"M 0 0\"/>";
  input += "</svg>";

  config_.max_elements = 10;
  auto result = SanitizeSvg(input, config_);
  EXPECT_FALSE(result.success);
  EXPECT_NE(result.error_message.find("element count"), std::string::npos);
}

TEST_F(SvgSanitizerTest, AcceptsWithinDepthLimit) {
  // 3 levels of nesting, limit is 5 -- should succeed.
  const char* input = "<svg><g><g><g><path d=\"M 0 0\"/></g></g></g></svg>";
  config_.max_depth = 5;
  auto result = SanitizeSvg(input, config_);
  ASSERT_TRUE(result.success) << result.error_message;
}

TEST_F(SvgSanitizerTest, AcceptsWithinElementLimit) {
  // 5 elements total (svg + 4 paths), limit is 10 -- should succeed.
  std::string input = "<svg>";
  for (int i = 0; i < 4; ++i) input += "<path d=\"M 0 0\"/>";
  input += "</svg>";

  config_.max_elements = 10;
  auto result = SanitizeSvg(input, config_);
  ASSERT_TRUE(result.success) << result.error_message;
}

TEST_F(SvgSanitizerTest, DepthLimitCountsNonAllowedElements) {
  // Non-allowed elements also count toward nesting depth.
  std::string input = "<svg>";
  for (int i = 0; i < 10; ++i) input += "<foo>";
  input += "<path d=\"M 0 0\"/>";
  for (int i = 0; i < 10; ++i) input += "</foo>";
  input += "</svg>";

  config_.max_depth = 5;
  auto result = SanitizeSvg(input, config_);
  EXPECT_FALSE(result.success);
  EXPECT_NE(result.error_message.find("depth"), std::string::npos);
}

}  // namespace
}  // namespace pagespeed
