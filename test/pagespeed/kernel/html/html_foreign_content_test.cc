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

// Parsing of inline SVG and MathML ("foreign content", HTML Standard section
// 13.2.6): element names that are raw text in HTML are ordinary elements
// there, integration points switch back to HTML rules, and some HTML tags end
// the foreign content.

#include "pagespeed/kernel/base/string.h"
#include "pagespeed/kernel/base/string_util.h"
#include "pagespeed/kernel/html/empty_html_filter.h"
#include "pagespeed/kernel/html/html_element.h"
#include "pagespeed/kernel/html/html_lexer.h"
#include "pagespeed/kernel/html/html_name.h"
#include "pagespeed/kernel/html/html_node.h"
#include "pagespeed/kernel/html/html_parse.h"
#include "test/pagespeed/kernel/base/gtest.h"
#include "test/pagespeed/kernel/html/html_parse_test_base.h"

namespace net_instaweb {

namespace {

// Records the event stream.  Element names without a keyword are prefixed
// with '~'; close styles are (e)xplicit, (u)nclosed, (i)mplicit, (b)rief and
// (a)uto.
class ForeignAnnotatingFilter : public EmptyHtmlFilter {
 public:
  void StartElement(HtmlElement* element) override {
    StrAppend(&buffer_, buffer_.empty() ? "+" : " +", Label(element));
  }
  void EndElement(HtmlElement* element) override {
    StrAppend(&buffer_, " -", Label(element));
    switch (element->style()) {
      case HtmlElement::AUTO_CLOSE:
        buffer_ += "(a)";
        break;
      case HtmlElement::IMPLICIT_CLOSE:
        buffer_ += "(i)";
        break;
      case HtmlElement::EXPLICIT_CLOSE:
        buffer_ += "(e)";
        break;
      case HtmlElement::BRIEF_CLOSE:
        buffer_ += "(b)";
        break;
      case HtmlElement::UNCLOSED:
        buffer_ += "(u)";
        break;
      case HtmlElement::INVISIBLE:
        buffer_ += "(I)";
        break;
    }
  }
  void Characters(HtmlCharactersNode* characters) override {
    StrAppend(&buffer_, buffer_.empty() ? "'" : " '", characters->contents(),
              "'");
  }
  void Cdata(HtmlCdataNode* cdata) override {
    StrAppend(&buffer_, buffer_.empty() ? "[" : " [", cdata->contents(), "]");
  }
  const char* Name() const override { return "ForeignAnnotatingFilter"; }

  const GoogleString& buffer() const { return buffer_; }
  void Clear() { buffer_.clear(); }

 private:
  static GoogleString Label(const HtmlElement* element) {
    return StrCat(element->keyword() == HtmlName::kNotAKeyword ? "~" : "",
                  element->name_str());
  }

  GoogleString buffer_;
};

}  // namespace

class HtmlForeignContentTest : public HtmlParseTestBase {
 protected:
  void SetUp() override {
    HtmlParseTestBase::SetUp();
    html_parse_.AddFilter(&annotation_);
  }

  bool AddBody() const override { return false; }
  bool AddHtmlTags() const override { return false; }

  // Parses html, checks that it is written back unchanged, and returns the
  // event annotation.
  GoogleString Parse(StringPiece html) { return Parse(html, html); }

  // As above, for inputs whose end tags the writer spells like the start tag.
  GoogleString Parse(StringPiece html, StringPiece written) {
    annotation_.Clear();
    ValidateExpected("foreign", html, written);
    return annotation_.buffer();
  }

  // Whether the parse of html has an HTML style element holding exactly the
  // text 'a<rect>b'.
  bool HasHtmlStyle(StringPiece html) {
    return Parse(html).find("+style 'a<rect>b' -style(e)") !=
           GoogleString::npos;
  }

  ForeignAnnotatingFilter annotation_;
};

TEST_F(HtmlForeignContentTest, HtmlStyleIsRawText) {
  EXPECT_EQ("+style 'a<rect>b' -style(e)", Parse("<style>a<rect>b</style>"));
}

TEST_F(HtmlForeignContentTest, StyleInSvgIsMarkup) {
  EXPECT_EQ("+~svg +~style 'x' +~rect -~rect(e) 'y' -~style(e) -~svg(e)",
            Parse("<svg><style>x<rect></rect>y</style></svg>"));
}

TEST_F(HtmlForeignContentTest, ScriptInSvgIsMarkup) {
  EXPECT_EQ("+~svg +~script 'x' +~g 'y' -~g(e) -~script(e) -~svg(e)",
            Parse("<svg><script>x<g>y</g></script></svg>"));
}

TEST_F(HtmlForeignContentTest, RawTextNamesInSvgAreMarkup) {
  for (const char* name : {"title", "textarea", "xmp", "iframe", "noembed",
                           "noframes", "noscript", "style", "script"}) {
    GoogleString html =
        StrCat("<svg><g><", name, ">x<rect></rect>y</", name, "></g></svg>");
    EXPECT_EQ(StrCat("+~svg +~g +~", name, " 'x' +~rect -~rect(e) 'y' -~", name,
                     "(e) -~g(e) -~svg(e)"),
              Parse(html))
        << name;
  }
}

TEST_F(HtmlForeignContentTest, RawTextNamesInMathAreMarkup) {
  for (const char* name :
       {"style", "script", "title", "textarea", "xmp", "iframe"}) {
    GoogleString html =
        StrCat("<math><", name, ">x<mrow></mrow>y</", name, "></math>");
    EXPECT_EQ(StrCat("+~math +~", name, " 'x' +~mrow -~mrow(e) 'y' -~", name,
                     "(e) -~math(e)"),
              Parse(html))
        << name;
  }
}

TEST_F(HtmlForeignContentTest, ForeignNamesAreKeywordless) {
  EXPECT_EQ("+~svg +~a 'x' -~a(e) -~svg(e) +a 'y' -a(e)",
            Parse("<svg><a href=\"/l\">x</a></svg><a href=\"/l\">y</a>"));
}

TEST_F(HtmlForeignContentTest, ForeignElementsAreNotVoid) {
  EXPECT_EQ("+~svg +~link 'x' -~link(u) -~svg(u)", Parse("<svg><link>x"));
}

TEST_F(HtmlForeignContentTest, ForeignSelfClosingStyle) {
  EXPECT_EQ("+~svg +~style -~style(b) +~rect -~rect(b) -~svg(e)",
            Parse("<svg><style/><rect/></svg>"));
}

TEST_F(HtmlForeignContentTest, SvgIntegrationPointsUseHtmlRules) {
  for (const char* name : {"foreignObject", "desc", "title", "FOREIGNOBJECT"}) {
    GoogleString html =
        StrCat("<svg><", name, "><style>a<rect>b</style></", name, "></svg>");
    EXPECT_EQ(StrCat("+~svg +~", name, " +style 'a<rect>b' -style(e) -~", name,
                     "(e) -~svg(e)"),
              Parse(html))
        << name;
  }
}

TEST_F(HtmlForeignContentTest, MathTextIntegrationPointsUseHtmlRules) {
  for (const char* name : {"mi", "mo", "mn", "ms", "mtext"}) {
    GoogleString html =
        StrCat("<math><", name, "><style>a<rect>b</style></", name, "></math>");
    EXPECT_EQ(StrCat("+~math +~", name, " +style 'a<rect>b' -style(e) -~", name,
                     "(e) -~math(e)"),
              Parse(html))
        << name;
  }
}

TEST_F(HtmlForeignContentTest, MglyphStaysMathInTextIntegrationPoint) {
  EXPECT_FALSE(HasHtmlStyle("<math><mi><mglyph><style>a<rect>b</style>"));
  EXPECT_FALSE(HasHtmlStyle("<math><mi><malignmark><style>a<rect>b</style>"));
  EXPECT_TRUE(HasHtmlStyle("<math><mi><mrow><style>a<rect>b</style>"));
}

TEST_F(HtmlForeignContentTest, AnnotationXmlEncoding) {
  EXPECT_TRUE(
      HasHtmlStyle("<math><annotation-xml encoding=\"text/html\">"
                   "<style>a<rect>b</style></annotation-xml></math>"));
  EXPECT_TRUE(
      HasHtmlStyle("<math><annotation-xml encoding='APPLICATION/XHTML+XML'>"
                   "<style>a<rect>b</style></annotation-xml></math>"));
  EXPECT_FALSE(HasHtmlStyle(
      "<math><annotation-xml><style>a<rect>b</style></annotation-xml></math>"));
  EXPECT_FALSE(
      HasHtmlStyle("<math><annotation-xml encoding=\"image/svg+xml\">"
                   "<style>a<rect>b</style></annotation-xml></math>"));
}

TEST_F(HtmlForeignContentTest, SvgInAnnotationXmlIsSvg) {
  // The svg element is SVG here, so its desc is an integration point.
  EXPECT_TRUE(
      HasHtmlStyle("<math><annotation-xml><svg><desc><style>a<rect>b</style>"));
}

TEST_F(HtmlForeignContentTest, SvgInMathIsMath) {
  // Directly inside math, <svg> is a MathML element, so desc is not an
  // integration point.
  EXPECT_FALSE(HasHtmlStyle("<math><svg><desc><style>a<rect>b</style>"));
  EXPECT_FALSE(HasHtmlStyle("<math><svg><style>a<rect>b</style>"));
}

TEST_F(HtmlForeignContentTest, MathInSvgIsSvg) {
  // Directly inside svg, <math> is an SVG element, so mi is not an
  // integration point.
  EXPECT_FALSE(HasHtmlStyle("<svg><math><mi><style>a<rect>b</style>"));
  EXPECT_FALSE(HasHtmlStyle("<svg><math><style>a<rect>b</style>"));
}

TEST_F(HtmlForeignContentTest, NestedThroughIntegrationPoints) {
  EXPECT_TRUE(
      HasHtmlStyle("<svg><foreignObject><math><mi><style>a<rect>b</style>"));
  EXPECT_FALSE(HasHtmlStyle("<math><mi><svg><style>a<rect>b</style>"));
  EXPECT_TRUE(HasHtmlStyle("<math><mtext><svg><title><style>a<rect>b</style>"));
}

TEST_F(HtmlForeignContentTest, BreakoutStartTagsEndForeignContent) {
  for (const char* name :
       {"b",      "big",    "blockquote", "body",    "br",    "center", "code",
        "dd",     "div",    "dl",         "dt",      "em",    "embed",  "h1",
        "h2",     "h3",     "h4",         "h5",      "h6",    "head",   "hr",
        "i",      "img",    "li",         "listing", "menu",  "meta",   "nobr",
        "ol",     "p",      "pre",        "ruby",    "s",     "small",  "span",
        "strong", "strike", "sub",        "sup",     "table", "tt",     "u",
        "ul",     "var",    "DIV"}) {
    EXPECT_TRUE(
        HasHtmlStyle(StrCat("<svg><g><", name, "><style>a<rect>b</style>")))
        << name;
    EXPECT_TRUE(
        HasHtmlStyle(StrCat("<math><mrow><", name, "><style>a<rect>b</style>")))
        << name;
  }
}

TEST_F(HtmlForeignContentTest, BreakoutClosesForeignElements) {
  EXPECT_EQ("+~svg +~g -~g(u) -~svg(u) +div 'x' -div(e)",
            Parse("<svg><g><div>x</div>"));
  // A later </svg> no longer has an open svg to close.
  EXPECT_EQ("+~svg -~svg(u) +br -br(i) '</svg>'", Parse("<svg><br></svg>"));
}

TEST_F(HtmlForeignContentTest, BreakoutStopsAtIntegrationPoint) {
  EXPECT_EQ(
      "+~svg +~foreignObject +~svg +~g -~g(u) -~svg(u) +div -div(u) "
      "-~foreignObject(u) -~svg(u)",
      Parse("<svg><foreignObject><svg><g><div>"));
}

TEST_F(HtmlForeignContentTest, NonBreakoutHtmlNamesStayForeign) {
  for (const char* name : {"a", "link", "input", "form", "select", "option",
                           "td", "tr", "font", "article", "section"}) {
    EXPECT_FALSE(
        HasHtmlStyle(StrCat("<svg><", name, "><style>a<rect>b</style>")))
        << name;
  }
}

TEST_F(HtmlForeignContentTest, FontWithAttributesEndsForeignContent) {
  EXPECT_FALSE(HasHtmlStyle("<svg><font><style>a<rect>b</style>"));
  EXPECT_FALSE(HasHtmlStyle("<svg><font x=1><style>a<rect>b</style>"));
  EXPECT_TRUE(HasHtmlStyle("<svg><font color=red><style>a<rect>b</style>"));
  EXPECT_TRUE(HasHtmlStyle("<svg><font face=x><style>a<rect>b</style>"));
  EXPECT_TRUE(HasHtmlStyle("<svg><font x=1 SIZE=2><style>a<rect>b</style>"));
}

TEST_F(HtmlForeignContentTest, EndTagsPAndBrEndForeignContent) {
  EXPECT_EQ("+p +~svg +~g -~g(u) -~svg(u) -p(e) +style 'a<rect>b' -style(e)",
            Parse("<p><svg><g></p><style>a<rect>b</style>"));
  EXPECT_TRUE(HasHtmlStyle("<svg><g></br><style>a<rect>b</style>"));
  EXPECT_TRUE(HasHtmlStyle("<math><mrow></P><style>a<rect>b</style>"));
}

TEST_F(HtmlForeignContentTest, ForeignEndTagClosesNearestMatch) {
  EXPECT_EQ("+~svg +~g +~rect -~rect(u) -~g(e) +~style 'x' -~style(e) -~svg(e)",
            Parse("<svg><g><rect></G><style>x</style></svg>",
                  "<svg><g><rect></g><style>x</style></svg>"));
  // An end tag with no open element of that name is kept as text.
  EXPECT_EQ("+~svg '</circle>' +~style 'x' -~style(e) -~svg(e)",
            Parse("<svg></circle><style>x</style></svg>"));
}

TEST_F(HtmlForeignContentTest, HtmlEndTagClosesForeignContent) {
  EXPECT_EQ(
      "+div +~svg +~g -~g(u) -~svg(u) -div(e) +style 'a<rect>b' -style(e)",
      Parse("<div><svg><g></div><style>a<rect>b</style>"));
}

TEST_F(HtmlForeignContentTest, HtmlEndTagStopsAtIntegrationPoint) {
  // Inside foreignObject an end tag for an element outside it is ignored, so
  // the style element is still inside the integration point.
  EXPECT_EQ(
      "+~svg +~foreignObject +div '</svg>' +style 'a<rect>b' -style(e) "
      "-div(u) -~foreignObject(u) -~svg(u)",
      Parse("<svg><foreignObject><div></svg><style>a<rect>b</style>"));
  EXPECT_EQ(
      "+~svg +~foreignObject +span '</foreignObject>' -span(u) "
      "-~foreignObject(u) -~svg(u)",
      Parse("<svg><foreignObject><span></foreignObject>"));
}

TEST_F(HtmlForeignContentTest, ForeignContentEndsWithItsEndTag) {
  EXPECT_EQ("+~svg -~svg(e) +style 'a<rect>b' -style(e)",
            Parse("<svg></SVG><style>a<rect>b</style>",
                  "<svg></svg><style>a<rect>b</style>"));
  EXPECT_EQ("+~math +~mi -~mi(e) -~math(e) +style 'a<rect>b' -style(e)",
            Parse("<math><mi></mi></math><style>a<rect>b</style>"));
  EXPECT_EQ("+~svg -~svg(b) +style 'a<rect>b' -style(e)",
            Parse("<svg/><style>a<rect>b</style>"));
}

// CDATA sections in SVG and MathML are read up to ]]>; tags inside them,
// including ones that end foreign content, are part of the section.
TEST_F(HtmlForeignContentTest, CdataInForeignContent) {
  EXPECT_EQ(
      "+~svg [<div><style>a>b</style>] +~style 'x' +~rect -~rect(e) 'y' "
      "-~style(e) -~svg(e)",
      Parse("<svg><![CDATA[<div><style>a>b</style>]]>"
            "<style>x<rect></rect>y</style></svg>"));
  EXPECT_EQ(
      "+~math [<p><style>c>d] +~style 'x' +~mrow -~mrow(e) 'y' "
      "-~style(e) -~math(e)",
      Parse("<math><![CDATA[<p><style>c>d]]>"
            "<style>x<mrow></mrow>y</style></math>"));
}

// An end tag handled as HTML that walks past SVG or MathML content follows
// the HTML Standard's "in body" rules for it.  </body>, </html>, </head> and
// </form> do not close the foreign content.
TEST_F(HtmlForeignContentTest, BodyEndTagKeepsForeignContentOpen) {
  EXPECT_EQ(
      "+body +~svg +~g '</body>' +~style 'a' +~rect 'b' -~rect(u) -~style(e) "
      "-~g(u) -~svg(u) -body(u)",
      Parse("<body><svg><g></body><style>a<rect>b</style>"));
  EXPECT_FALSE(HasHtmlStyle("<html><svg><g></html><style>a<rect>b</style>"));
  EXPECT_FALSE(HasHtmlStyle("<head><svg><g></head><style>a<rect>b</style>"));
  EXPECT_FALSE(
      HasHtmlStyle("<form><math><mrow></form><style>a<rect>b"
                   "</style>"));
}

// A formatting end tag closes the foreign content only when no special
// element lies between it and the foreign content.
TEST_F(HtmlForeignContentTest, FormattingEndTagAcrossForeignContent) {
  EXPECT_TRUE(HasHtmlStyle("<b><svg><g></b><style>a<rect>b</style>"));
  EXPECT_TRUE(
      HasHtmlStyle("<a><span><math><mrow></a><style>a<rect>b"
                   "</style>"));
  EXPECT_FALSE(HasHtmlStyle("<b><div><svg><g></b><style>a<rect>b</style>"));
  EXPECT_FALSE(HasHtmlStyle("<i><p><math><mrow></i><style>a<rect>b</style>"));
}

// End tags with in-scope rules close through foreign content and through
// special elements, as before.
TEST_F(HtmlForeignContentTest, InScopeEndTagAcrossForeignContent) {
  EXPECT_EQ(
      "+div +p +~svg +~g -~g(u) -~svg(u) -p(u) -div(e) +style 'a<rect>b' "
      "-style(e)",
      Parse("<div><p><svg><g></div><style>a<rect>b</style>"));
  EXPECT_TRUE(
      HasHtmlStyle("<ul><li><span><math></li><style>a<rect>b"
                   "</style>"));
}

// Past foreign content, an end tag with in-scope rules stops at a scope
// boundary: the default scope for </div> or </dd>, the list item scope for
// </li>.
TEST_F(HtmlForeignContentTest, InScopeEndTagStopsAtScopeBoundary) {
  EXPECT_FALSE(HasHtmlStyle(
      "<div><table><tr><td><svg><g></div><style>a<rect>b</style>"));
  EXPECT_FALSE(
      HasHtmlStyle("<dl><dd><object><math><mrow></dd><style>a<rect>b</style>"));
  EXPECT_FALSE(
      HasHtmlStyle("<ul><li><ol><svg><g></li><style>a<rect>b</style>"));
  EXPECT_TRUE(
      HasHtmlStyle("<ul><li><div><svg><g></li><style>a<rect>b</style>"));
  // Table end tags keep their matching.
  EXPECT_TRUE(
      HasHtmlStyle("<table><tr><td><svg><g></table><style>a<rect>b</style>"));
}

// Any other end tag stops at a special element.
TEST_F(HtmlForeignContentTest, OtherEndTagAcrossForeignContent) {
  EXPECT_EQ(
      "+span +div +~svg '</span>' +~style 'a' +~rect 'b' -~rect(u) "
      "-~style(e) -~svg(u) -div(u) -span(u)",
      Parse("<span><div><svg></span><style>a<rect>b</style>"));
  EXPECT_FALSE(HasHtmlStyle("<span><div><math></span><style>a<rect>b</style>"));
  EXPECT_EQ("+span +~svg -~svg(u) -span(e) +style 'a<rect>b' -style(e)",
            Parse("<span><svg></span><style>a<rect>b</style>"));
  EXPECT_TRUE(HasHtmlStyle("<span><math><mrow></span><style>a<rect>b</style>"));
}

// Without foreign content in between, end tags behave as they always have.
TEST_F(HtmlForeignContentTest, EndTagsWithoutForeignContentUnchanged) {
  EXPECT_EQ("+span +div -div(u) -span(e) 'x'", Parse("<span><div></span>x"));
  EXPECT_EQ("+b +div -div(u) -b(e) 'x'", Parse("<b><div></b>x"));
  EXPECT_EQ("+body +div -div(u) -body(e) 'x'", Parse("<body><div></body>x"));
}

// A foreign end tag closes through an integration point from inside.
TEST_F(HtmlForeignContentTest, ForeignEndTagThroughIntegrationPoint) {
  EXPECT_EQ(
      "+~svg +~foreignObject +~svg +~g -~g(u) -~svg(u) -~foreignObject(e) "
      "+~style 'x' -~style(e) -~svg(e)",
      Parse("<svg><foreignObject><svg><g></foreignObject><style>x</style>"
            "</svg>"));
}

// With an HTML element current inside an integration point, end tags for the
// integration point or the svg element are kept as text.
TEST_F(HtmlForeignContentTest, IntegrationPointEndTagsIgnoredFromHtml) {
  EXPECT_EQ(
      "+~svg +~foreignObject +p 'x</foreignObject></svg>' -p(u) "
      "-~foreignObject(u) -~svg(u)",
      Parse("<svg><foreignObject><p>x</foreignObject></svg>"));
}

// Breakout stops at a MathML text integration point.
TEST_F(HtmlForeignContentTest, BreakoutStopsAtMathTextIntegrationPoint) {
  EXPECT_EQ("+~math +~mi +~mglyph -~mglyph(u) +b 'x' -b(e) -~mi(u) -~math(u)",
            Parse("<math><mi><mglyph><b>x</b>"));
}

// An svg element inside an annotation-xml HTML integration point is SVG.
TEST_F(HtmlForeignContentTest, SvgInHtmlAnnotationXmlIsSvg) {
  EXPECT_FALSE(
      HasHtmlStyle("<math><annotation-xml encoding=\"text/html\"><svg>"
                   "<style>a<rect>b</style>"));
  EXPECT_TRUE(
      HasHtmlStyle("<math><annotation-xml encoding=\"text/html\"><svg><desc>"
                   "<style>a<rect>b</style>"));
}

// Self-closing SVG elements whose names are void or never self-closed in
// HTML.
TEST_F(HtmlForeignContentTest, SelfClosingForeignElements) {
  EXPECT_EQ("+~svg +~link -~link(b) 'x' -~svg(e)",
            Parse("<svg><link/>x</svg>"));
  EXPECT_EQ("+~svg +~a -~a(b) 'x' -~svg(e)", Parse("<svg><a/>x</svg>"));
}

// A flush inside an SVG style element keeps the parse state.
TEST_F(HtmlForeignContentTest, FlushInsideForeignStyle) {
  annotation_.Clear();
  SetupWriter();
  html_parse_.StartParse("http://test.com/flush.html");
  html_parse_.ParseText("<svg><style>a<re");
  html_parse_.Flush();
  html_parse_.ParseText("ct></rect>b</style><style>c</style></svg>");
  html_parse_.FinishParse();
  EXPECT_EQ("<svg><style>a<rect></rect>b</style><style>c</style></svg>",
            output_buffer_);
  EXPECT_EQ(
      "+~svg +~style 'a' +~rect -~rect(e) 'b' -~style(e) +~style 'c' "
      "-~style(e) -~svg(e)",
      annotation_.buffer());
}

// The nesting cap applies inside foreign content as everywhere else.
TEST_F(HtmlForeignContentTest, NestingCapInForeignContent) {
  GoogleString html = "<svg>";
  for (int i = 0; i < 600; ++i) {
    html += "<g>";
  }
  html += "<style>x</style>";
  annotation_.Clear();
  HtmlParseTestBase::Parse("nesting_cap", html);
  const GoogleString& events = annotation_.buffer();
  int starts = 0;
  for (size_t pos = events.find("+~g"); pos != GoogleString::npos;
       pos = events.find("+~g", pos + 1)) {
    ++starts;
  }
  EXPECT_EQ(HtmlLexer::kMaxNestingDepth - 1, starts);
  EXPECT_EQ(GoogleString::npos, events.find("style"));
}

TEST_F(HtmlForeignContentTest, EndOfFileInForeignContent) {
  EXPECT_EQ("+~svg +~style 'x' +~g 'y' -~g(u) -~style(u) -~svg(u)",
            Parse("<svg><style>x<g>y"));
  EXPECT_EQ("+~math +~mi +style 'a<g>' -style(u) -~mi(u) -~math(u)",
            Parse("<math><mi><style>a<g>"));
}

}  // namespace net_instaweb
