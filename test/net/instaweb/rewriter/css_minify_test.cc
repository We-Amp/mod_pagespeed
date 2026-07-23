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

#include <memory>

#include "net/instaweb/rewriter/public/css_minify.h"

#include "pagespeed/kernel/base/google_message_handler.h"
#include "pagespeed/kernel/base/string.h"
#include "pagespeed/kernel/base/string_writer.h"
#include "test/pagespeed/kernel/base/gtest.h"
#include "third_party/css_parser/src/webutil/css/parser.h"

namespace net_instaweb {

namespace {

class CssMinifyTest : public ::testing::Test {
 protected:
  CssMinifyTest() {}
  ~CssMinifyTest() override {}

  void RewriteCss(const StringPiece& in_text, GoogleString* out_text) {
    out_text->clear();

    // Parse CSS.
    // XXX(oschaaf): css
    CssStringPiece tmp(in_text.data(), in_text.size());
    Css::Parser parser(tmp);
    parser.set_preservation_mode(true);
    parser.set_quirks_mode(false);
    std::unique_ptr<Css::Stylesheet> stylesheet(parser.ParseRawStylesheet());

    if (parser.errors_seen_mask() != Css::Parser::kNoError) {
      in_text.CopyToString(out_text);
      return;
    }

    // Re-serialize
    StringWriter out_writer(out_text);
    EXPECT_TRUE(CssMinify::Stylesheet(*stylesheet, &out_writer, &handler_));
  }

  GoogleMessageHandler handler_;
};

TEST_F(CssMinifyTest, RewriteCssIncompleteUnicode) {
  // Test that a css string with an incomplete unicode character doesn't hang.
  // This string should not get minified either due to the error in it.
  static const unsigned char kCssStringData[] = {
      64,  109, 101, 100, 105, 97, 32,  40,  106, 97,  120, 45, 119, 105,
      100, 116, 104, 58,  32,  88, 200, 194, 143, 135, 41,  32, 0};
  const char* css_string(reinterpret_cast<const char*>(kCssStringData));
  // This string is `echo "QG1lZGlhIChqYXgtd2lkdGg6IFjIwo+HKSA=" |base64 -d`
  GoogleString rewritten;
  RewriteCss(css_string, &rewritten);
  EXPECT_STREQ(rewritten, css_string) << "Should not hang";
}

TEST_F(CssMinifyTest, MinifyStylesheeetCollectingUrls) {
  const char kCss[] =
      ".a {\n"
      "  background-color: darkgreen;\n"
      "  background-image: url(foo.png);\n"
      "}";
  GoogleString minified;
  StringWriter writer(&minified);
  StringVector urls;
  CssMinify minify(&writer, &handler_);
  minify.set_url_collector(&urls);
  EXPECT_TRUE(minify.ParseStylesheet(kCss));
  ASSERT_EQ(1, urls.size());
  EXPECT_STREQ("foo.png", urls[0]);
  EXPECT_STREQ(".a{background-color:#006400;background-image:url(foo.png)}",
               minified);
}

TEST_F(CssMinifyTest, MinifyImportStylesheeetCollectingUrls) {
  const char kCss[] = "@import   'foo.png';";
  GoogleString minified;
  StringWriter writer(&minified);
  StringVector urls;
  CssMinify minify(&writer, &handler_);
  minify.set_url_collector(&urls);
  EXPECT_TRUE(minify.ParseStylesheet(kCss));
  ASSERT_EQ(1, urls.size());
  EXPECT_STREQ("foo.png", urls[0]);
  EXPECT_STREQ("@import url(foo.png);", minified);
}

TEST_F(CssMinifyTest, MinifyInvalid) {
  const char kCss[] = "{";
  GoogleString minified;
  StringWriter writer(&minified);
  CssMinify minify(&writer, &handler_);
  EXPECT_FALSE(minify.ParseStylesheet(kCss));
}

TEST_F(CssMinifyTest, DoNotFixBadColorsOrUnits) {
  const char kCss[] =
      ".a {\n"
      "  width: 10;\n"
      "  height: 20px;;\n"
      "  background-color: 0f0f0f;\n"
      "  foreground-color: #f0f0f0;\n"
      "}";
  GoogleString minified;
  StringWriter writer(&minified);
  CssMinify minify(&writer, &handler_);
  EXPECT_TRUE(minify.ParseStylesheet(kCss));

  // Note that we don't "fix" the '10' by appending a 'px', or the
  // background-color by adding a '#'.  In quirks-mode we would add the #.
  EXPECT_STREQ(
      ".a{width:10;height:20px;"
      "background-color: 0f0f0f;"  // Missing # not added, space is not removed.
      "foreground-color:#f0f0f0}",
      minified);
}

TEST_F(CssMinifyTest, RemoveZeroLengthButNotTimeOrPercentSuffix) {
  const char kCss[] =
      ".a {\n"
      "  width: 0px;\n"
      "  height: 0%;\n"
      "  -moz-transition-delay: 0s, 0s;\n"
      "}";
  GoogleString minified;
  StringWriter writer(&minified);
  CssMinify minify(&writer, &handler_);
  EXPECT_TRUE(minify.ParseStylesheet(kCss));
  // TODO(jmarantz): this CSS is not well minified.  We should strip
  // the spaces around the comma.
  EXPECT_STREQ(".a{width:0;height:0%;-moz-transition-delay:0s , 0s}", minified);
}

TEST_F(CssMinifyTest, ParsingAndMinifyingBackgroundAndFont) {
  const char kCss[] =
      ".a {\n"
      "  font:normal 16px Foo, sans-serif;\n"
      "}\n"
      "body {\n"
      "  background: #fff;\n"
      "}";
  Css::Parser parser(kCss);
  std::unique_ptr<Css::Stylesheet> stylesheet(parser.ParseStylesheet());
  GoogleString minified;
  StringWriter writer(&minified);

  EXPECT_TRUE(CssMinify::Stylesheet(*stylesheet, &writer, &handler_));
  // TODO(peleyal): We are adding more data than required. Should be:
  // ".a{font:normal 16px Foo,sans-serif}"
  // "body{background:#fff}"
  EXPECT_STREQ(
      ".a{font:16px Foo,sans-serif;font-style:normal;font-variant:normal;"
      "font-weight:normal;font-size:16px;line-height:normal;"
      "font-family:Foo,sans-serif}"
      "body{background:#fff;background-color:#fff;background-image:none;"
      "background-repeat:repeat;background-attachment:scroll;"
      "background-position-x:0%;background-position-y:0%}",
      minified);
}

TEST_F(CssMinifyTest, ParsingAndMinifingViewportUnits) {
  const char kCss[] =
      ".a {\n"
      "  margin-top: 70vh;\n"
      "  margin-bottom: 20vw;\n"
      "}\n";

  Css::Parser parser(kCss);
  std::unique_ptr<Css::Stylesheet> stylesheet(parser.ParseStylesheet());
  GoogleString minified;
  StringWriter writer(&minified);

  EXPECT_TRUE(CssMinify::Stylesheet(*stylesheet, &writer, &handler_));
  EXPECT_STREQ(".a{margin-top:70vh;margin-bottom:20vw}", minified);
}

TEST_F(CssMinifyTest, StraySingleQuote1) {
  static const char kCss[] =
      ".view_all a{\n"
      "  display: block;\n"
      "  'width: 100%;\n"
      "  padding: 5px 0 1px 0"
      "}";

  Css::Parser parser(kCss);
  std::unique_ptr<Css::Stylesheet> stylesheet(parser.ParseStylesheet());
  GoogleString minified;
  StringWriter writer(&minified);

  EXPECT_TRUE(CssMinify::Stylesheet(*stylesheet, &writer, &handler_));
  // There are two bits of error recovery happening here:
  // 1) error recovery for the unclosed 'width string eats all the way until
  //    the end of line.
  // 2) error recovery for the declaration starting with 'width eats all the way
  //    until the next semicolon or a closing } (skipping matching ones before)
  // --- which is after the padding declaration, since the first semicolon is
  // just a part of the 'width... string
  EXPECT_STREQ(".view_all a{display:block}", minified);
}

TEST_F(CssMinifyTest, StraySingleQuote2) {
  static const char kCss[] =
      ".view_all a{\n"
      "  display: block;\n"
      "  'width: 100%;\n"
      "  padding: 5px 0 1px 0;"
      "}";

  GoogleString minified;
  StringWriter writer(&minified);
  CssMinify minify(&writer, &handler_);
  StringVector urls;
  minify.set_url_collector(&urls);
  ASSERT_TRUE(minify.ParseStylesheet(kCss));
  EXPECT_STREQ(
      ".view_all a{display:block;'width: 100%;\n"
      "  padding: 5px 0 1px 0}",
      minified);
}

TEST_F(CssMinifyTest, StraySingleQuote3) {
  // Non-permissive mode, should drop anything on the 'width line till \n,
  // and then continue recovery until the next semicolon.
  static const char kCss[] =
      ".view_all a{\n"
      "  display: block;\n"
      "  'width: 100%; border:1px solid red;\n"
      "  padding: 5px 0 1px 0;"
      "  margin: 1px;"
      "}";

  Css::Parser parser(kCss);
  std::unique_ptr<Css::Stylesheet> stylesheet(parser.ParseStylesheet());
  GoogleString minified;
  StringWriter writer(&minified);

  EXPECT_TRUE(CssMinify::Stylesheet(*stylesheet, &writer, &handler_));
  EXPECT_STREQ(".view_all a{display:block;margin:1px}", minified);
}

// checking whether unit is retained for zero value in calc function
TEST_F(CssMinifyTest, CalcFunctionWithZeroValueAndUnit) {
  static const char kCss[] =
      "@font-face {\n"
      " width: calc(600px - 0px)"
      "}";

  GoogleString minified;
  StringWriter writer(&minified);
  CssMinify minify(&writer, &handler_);

  EXPECT_TRUE(minify.ParseStylesheet(kCss));
  EXPECT_HAS_SUBSTR("width:calc(600px - 0px)", minified);
}

// checking unit is retained for zero value in calc with nested var()
TEST_F(CssMinifyTest, CalcFunctionNestedVarKeepsZeroUnit) {
  static const char kCss[] =
      ".a {\n"
      " width: calc(var(--x) - 0px)"
      "}";

  GoogleString minified;
  StringWriter writer(&minified);
  CssMinify minify(&writer, &handler_);

  EXPECT_TRUE(minify.ParseStylesheet(kCss));
  EXPECT_HAS_SUBSTR("width:calc(var(--x) - 0px)", minified);
}

// checking unit is retained for zero value in calc with nested min()
TEST_F(CssMinifyTest, CalcFunctionNestedMinKeepsZeroUnit) {
  static const char kCss[] =
      ".a {\n"
      " width: calc(min(100%, 50px) - 0px)"
      "}";

  GoogleString minified;
  StringWriter writer(&minified);
  CssMinify minify(&writer, &handler_);

  EXPECT_TRUE(minify.ParseStylesheet(kCss));
  EXPECT_HAS_SUBSTR("width:calc(min(100%,50px) - 0px)", minified);
}

// checking unit is retained for zero value in calc with nested calc()
TEST_F(CssMinifyTest, CalcFunctionNestedCalcKeepsZeroUnit) {
  static const char kCss[] =
      ".a {\n"
      " width: calc(calc(600px) - 0px)"
      "}";

  GoogleString minified;
  StringWriter writer(&minified);
  CssMinify minify(&writer, &handler_);

  EXPECT_TRUE(minify.ParseStylesheet(kCss));
  EXPECT_HAS_SUBSTR("width:calc(calc(600px) - 0px)", minified);
}

// checking unit is retained for zero value in -webkit-calc
TEST_F(CssMinifyTest, WebkitCalcFunctionKeepsZeroUnit) {
  static const char kCss[] =
      ".a {\n"
      " width: -webkit-calc(var(--x) - 0px)"
      "}";

  GoogleString minified;
  StringWriter writer(&minified);
  CssMinify minify(&writer, &handler_);

  EXPECT_TRUE(minify.ParseStylesheet(kCss));
  EXPECT_HAS_SUBSTR("width:-webkit-calc(var(--x) - 0px)", minified);
}

// checking unit is retained for zero value in uppercase CALC
TEST_F(CssMinifyTest, CalcFunctionUppercaseKeepsZeroUnit) {
  static const char kCss[] =
      ".a {\n"
      " width: CALC(var(--x) - 0px)"
      "}";

  GoogleString minified;
  StringWriter writer(&minified);
  CssMinify minify(&writer, &handler_);

  EXPECT_TRUE(minify.ParseStylesheet(kCss));
  EXPECT_HAS_SUBSTR("width:CALC(var(--x) - 0px)", minified);
}

// checking unit is retained for zero value in mixed-case Calc
TEST_F(CssMinifyTest, CalcFunctionMixedCaseKeepsZeroUnit) {
  static const char kCss[] =
      ".a {\n"
      " width: Calc(600px - 0px)"
      "}";

  GoogleString minified;
  StringWriter writer(&minified);
  CssMinify minify(&writer, &handler_);

  EXPECT_TRUE(minify.ParseStylesheet(kCss));
  EXPECT_HAS_SUBSTR("width:Calc(600px - 0px)", minified);
}

// checking unit is retained for zero value at top level of uppercase MIN
TEST_F(CssMinifyTest, MinFunctionUppercaseKeepsZeroUnit) {
  static const char kCss[] =
      ".a {\n"
      " width: MIN(100% - 0px, 200px)"
      "}";

  GoogleString minified;
  StringWriter writer(&minified);
  CssMinify minify(&writer, &handler_);

  EXPECT_TRUE(minify.ParseStylesheet(kCss));
  EXPECT_HAS_SUBSTR("width:MIN(100% - 0px,200px)", minified);
}

// checking unit is retained for zero value at top level of min()
TEST_F(CssMinifyTest, MinFunctionKeepsZeroUnit) {
  static const char kCss[] =
      ".a {\n"
      " width: min(100% - 0px, 200px)"
      "}";

  GoogleString minified;
  StringWriter writer(&minified);
  CssMinify minify(&writer, &handler_);

  EXPECT_TRUE(minify.ParseStylesheet(kCss));
  EXPECT_HAS_SUBSTR("width:min(100% - 0px,200px)", minified);
}

// checking unit is still stripped for zero value outside calc context
TEST_F(CssMinifyTest, ZeroUnitStillStrippedOutsideCalc) {
  static const char kCss[] =
      ".a {\n"
      " width: calc(600px - 0px);"
      " margin: 0px 10px 0em 0%"
      "}";

  GoogleString minified;
  StringWriter writer(&minified);
  CssMinify minify(&writer, &handler_);

  EXPECT_TRUE(minify.ParseStylesheet(kCss));
  EXPECT_HAS_SUBSTR("width:calc(600px - 0px);margin:0 10px 0 0%", minified);
}

// checking unicode-range descriptor minification
TEST_F(CssMinifyTest, CssUnicodeRangeDescriptor) {
  static const char kCss[] =
      "@font-face {\n"
      " unicode-range: U+0400-045F, U+0490-0491, U+04B0-04B1, U+2116;\n"
      "}";

  GoogleString minified;
  StringWriter writer(&minified);
  CssMinify minify(&writer, &handler_);

  EXPECT_TRUE(minify.ParseStylesheet(kCss));
  EXPECT_HAS_SUBSTR("unicode-range:U+0400-045F,U+0490-0491,U+04B0-04B1,U+2116",
                    minified);
}

TEST_F(CssMinifyTest, GroupRules) {
  // Preludes are verbatim source bytes; bodies are minified structure.
  // The @font-face inside @supports stays inside its condition.
  static const char kCss[] =
      "@supports (display: grid) {\n"
      "  .a { top: 0px; }\n"
      "}\n"
      "@layer base {\n"
      "  .b { left: 0px; }\n"
      "}\n"
      "@container sidebar (width >= 400px) {\n"
      "  .c { right: 0px; }\n"
      "}\n"
      "@supports (font-format: woff2) {\n"
      "  @font-face { font-family: A; src: url(a.woff2); }\n"
      "}";
  GoogleString minified;
  StringWriter writer(&minified);
  CssMinify minify(&writer, &handler_);
  EXPECT_TRUE(minify.ParseStylesheet(kCss));
  EXPECT_STREQ(
      "@supports (display: grid){.a{top:0}}"
      "@layer base{.b{left:0}}"
      "@container sidebar (width >= 400px){.c{right:0}}"
      "@supports (font-format: woff2)"
      "{@font-face{font-family:A;src:url(a.woff2)}}",
      minified);
}

TEST_F(CssMinifyTest, GroupRulesIdempotent) {
  static const char kCss[] =
      "@layer x { @supports (a:b) { @media screen { .a { top: 0px } } } }\n"
      "@media (width >= 768px) { .b { left: 0px } }";
  GoogleString minified_once;
  {
    StringWriter writer(&minified_once);
    CssMinify minify(&writer, &handler_);
    EXPECT_TRUE(minify.ParseStylesheet(kCss));
  }
  GoogleString minified_twice;
  {
    StringWriter writer(&minified_twice);
    CssMinify minify(&writer, &handler_);
    EXPECT_TRUE(minify.ParseStylesheet(minified_once));
  }
  EXPECT_EQ(minified_once, minified_twice);
  EXPECT_STREQ(
      "@layer x{@supports (a:b){@media screen{.a{top:0}}}}"
      "@media (width >= 768px){.b{left:0}}",
      minified_once);
}

TEST_F(CssMinifyTest, MediaRegroupingAroundGroupRules) {
  // All three children of the @media carry equal media annotations (the
  // group node included), so re-grouping emits one @media wrapper.
  static const char kCss[] =
      "@media screen {\n"
      "  .a { top: 0px }\n"
      "  @supports (a:b) { .b { left: 0px } }\n"
      "  .c { right: 0px }\n"
      "}";
  GoogleString minified;
  StringWriter writer(&minified);
  CssMinify minify(&writer, &handler_);
  EXPECT_TRUE(minify.ParseStylesheet(kCss));
  EXPECT_STREQ(
      "@media screen{.a{top:0}@supports (a:b){.b{left:0}}.c{right:0}}",
      minified);
}

TEST_F(CssMinifyTest, RawMediaExpressions) {
  // Raw MQ4 expressions serialize verbatim. Equal raw queries merge into one
  // @media run; unequal raw queries stay separate (order-preserving).
  static const char kCss[] =
      "@media (width >= 768px) { .a { top: 0px } }\n"
      "@media (width >= 768px) { .b { left: 0px } }\n"
      "@media (400px <= width <= 700px) { .c { right: 0px } }\n"
      "@media screen and (width >= 64em) { .d { bottom: 0px } }";
  GoogleString minified;
  StringWriter writer(&minified);
  CssMinify minify(&writer, &handler_);
  EXPECT_TRUE(minify.ParseStylesheet(kCss));
  EXPECT_STREQ(
      "@media (width >= 768px){.a{top:0}.b{left:0}}"
      "@media (400px <= width <= 700px){.c{right:0}}"
      "@media screen and (width >= 64em){.d{bottom:0}}",
      minified);
}

TEST_F(CssMinifyTest, LayerStatementAndBlockOrdering) {
  // All four top-level forms interleaved — statement-form @layer, @layer
  // blocks, a plain ruleset, and an @media run: everything lives in one
  // ordered sequence and cascade order must survive minification. The two
  // contiguous equal-media rulesets regroup into a single @media wrapper;
  // nothing else may merge or move.
  static const char kCss[] =
      "@layer a, b;\n"
      "@layer b { .a { top: 0px } }\n"
      ".plain { color: red }\n"
      "@media screen { .m1 { top: 1px } }\n"
      "@media screen { .m2 { top: 2px } }\n"
      "@layer a { .b { left: 0px } }";
  GoogleString minified;
  StringWriter writer(&minified);
  CssMinify minify(&writer, &handler_);
  EXPECT_TRUE(minify.ParseStylesheet(kCss));
  EXPECT_STREQ(
      "@layer a, b;"
      "@layer b{.a{top:0}}"
      ".plain{color:red}"
      "@media screen{.m1{top:1px}.m2{top:2px}}"
      "@layer a{.b{left:0}}",
      minified);
}

}  // namespace
}  // namespace net_instaweb
