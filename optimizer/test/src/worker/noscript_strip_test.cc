// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

// Tests for StripNoscriptElements: the document a browser
// running scripts renders, for the analysis renders that run with script
// execution disabled.

#include "src/worker/noscript_strip.h"

#include <algorithm>
#include <string>
#include <string_view>
#include <vector>

#include "absl/strings/str_cat.h"
#include "gtest/gtest.h"
#include "src/worker/html_scanner.h"

namespace pagespeed {
namespace {

std::string Strip(std::string_view html) {
  return StripNoscriptElements(html).html;
}

TEST(NoscriptStripTest, NoNoscriptLeavesTheDocumentByteForByte) {
  const std::string html =
      "<!DOCTYPE html><html><head><title>T</title>"
      "<link rel=stylesheet href=/a.css></head>"
      "<body><div class=\"a\" title='x > y'>Hi</div></body></html>";
  NoscriptStripResult r = StripNoscriptElements(html);
  EXPECT_EQ(r.html, html);
  EXPECT_EQ(r.removed, 0u);
  EXPECT_TRUE(r.removed_ranges.empty());
}

TEST(NoscriptStripTest, RemovesBodyNoscriptAndItsContent) {
  NoscriptStripResult r = StripNoscriptElements(
      "<body><noscript><div class=nojs-banner>Enable JS</div></noscript>"
      "<div class=hero>Hero</div></body>");
  EXPECT_EQ(r.html, "<body><div class=hero>Hero</div></body>");
  EXPECT_EQ(r.removed, 1u);
}

TEST(NoscriptStripTest, RemovesHeadNoscriptWithLinkStyleAndMeta) {
  EXPECT_EQ(Strip("<head><meta charset=utf-8>"
                  "<noscript><link rel=stylesheet href=/nojs.css>"
                  "<style>.hero{display:none}</style>"
                  "<meta http-equiv=refresh content=\"0;url=/nojs\">"
                  "</noscript><title>T</title></head>"),
            "<head><meta charset=utf-8><title>T</title></head>");
}

TEST(NoscriptStripTest, MatchesTagNamesCaseInsensitively) {
  EXPECT_EQ(Strip("a<NOSCRIPT><p>x</p></NoScript>b"), "ab");
  EXPECT_EQ(Strip("a<noscript data-x=1 >x</noscript >b"), "ab");
  EXPECT_EQ(Strip("a<noscript>x</noscript\n>b"), "ab");
}

TEST(NoscriptStripTest, NestedNoscriptEndsAtTheFirstEndTag) {
  // With scripting on, <noscript> content is raw text: the inner start tag is
  // text, and the first </noscript> closes the outer element. What follows is
  // document content again; the stray end tag is ignored by the browser.
  NoscriptStripResult r = StripNoscriptElements(
      "<body><noscript>a<noscript>b</noscript>c</noscript>d</body>");
  EXPECT_EQ(r.html, "<body>c</noscript>d</body>");
  EXPECT_EQ(r.removed, 1u);
}

TEST(NoscriptStripTest, UnclosedNoscriptRunsToTheEnd) {
  EXPECT_EQ(Strip("<body><p>a</p><noscript><p>b</p></body></html>"),
            "<body><p>a</p>");
  // A start tag that never closes is dropped by the tokenizer as well.
  EXPECT_EQ(Strip("<body><p>a</p><noscript class=\"x"), "<body><p>a</p>");
}

TEST(NoscriptStripTest, SelfClosingSyntaxStillOpensRawText) {
  // `/>` is ignored on an HTML element: <noscript/> opens one.
  EXPECT_EQ(Strip("a<noscript/>b</noscript>c"), "ac");
}

TEST(NoscriptStripTest, EndTagNeedsADelimiter) {
  // </noscriptx> is not the end tag; the element runs on to </noscript>.
  EXPECT_EQ(Strip("a<noscript>b</noscriptx>c</noscript>d"), "ad");
  // <noscripts> is some other element.
  EXPECT_EQ(Strip("a<noscripts>b</noscripts>c"), "a<noscripts>b</noscripts>c");
}

TEST(NoscriptStripTest, RawTextInsideNoscriptDoesNotHideTheEndTag) {
  // In a scripting browser the <noscript> raw text ends at the first
  // </noscript>, even one that looks like it sits in a <style> or a comment.
  EXPECT_EQ(Strip("a<noscript><style>p{}</noscript>b</style>c"), "ab</style>c");
  EXPECT_EQ(Strip("a<noscript><!-- </noscript> -->b"), "a -->b");
  EXPECT_EQ(Strip("a<noscript><img alt=\"</noscript>\">b"), "a\">b");
}

TEST(NoscriptStripTest, KeepsNoscriptInsideTextAndComments) {
  const std::string html =
      "<!-- <noscript>x</noscript> -->"
      "<script>document.write('<noscript>x</noscript>')</script>"
      "<style>/* <noscript> */</style>"
      "<textarea><noscript>x</noscript></textarea>"
      "<title><noscript></title>"
      "<xmp><noscript></xmp>"
      "<iframe><noscript></iframe>"
      "<noembed><noscript></noembed>"
      "<noframes><noscript></noframes>"
      "<div title=\"<noscript>\" data-x='<noscript>'>x</div>"
      "<!DOCTYPE <noscript>>";
  NoscriptStripResult r = StripNoscriptElements(html);
  EXPECT_EQ(r.html, html);
  EXPECT_EQ(r.removed, 0u);
}

TEST(NoscriptStripTest, PlaintextHidesTheRest) {
  const std::string html = "a<plaintext><noscript>x</noscript>";
  EXPECT_EQ(Strip(html), html);
}

TEST(NoscriptStripTest, UnclosedCommentOrRawTextHidesTheRest) {
  for (const std::string html :
       {"a<!-- <noscript>x</noscript>", "a<script><noscript>x</noscript>",
        "a<style><noscript>"}) {
    EXPECT_EQ(Strip(html), html) << html;
  }
}

TEST(NoscriptStripTest, RemovesNoscriptInsideTemplate) {
  // Template content is parsed as HTML with the same scripting flag.
  EXPECT_EQ(Strip("<template><p>a</p><noscript><p>b</p></noscript></template>"),
            "<template><p>a</p></template>");
}

TEST(NoscriptStripTest, KeepsNoscriptInForeignContent) {
  // In inline <svg> or <math>, <noscript> is a foreign element, not raw
  // text, and renders the same with scripting on or off.
  const std::string svg =
      "<svg><noscript><text>x</text></noscript><noscript/></svg>";
  EXPECT_EQ(Strip(svg), svg);
  const std::string math = "<math><noscript><mi>x</mi></noscript></math>";
  EXPECT_EQ(Strip(math), math);
  // Back in HTML content after </svg>, and after a self-closed <svg/>.
  EXPECT_EQ(Strip(svg + "<noscript>y</noscript>z"), svg + "z");
  EXPECT_EQ(Strip("<svg/><noscript>y</noscript>z"), "<svg/>z");
  // Nested foreign roots.
  EXPECT_EQ(Strip("<svg><svg></svg><noscript>y</noscript></svg>"
                  "<noscript>w</noscript>"),
            "<svg><svg></svg><noscript>y</noscript></svg>");
}

// The foreign content lists are the HTML parser's, as the vendored HtmlLexer
// has them: each breakout start tag (and <font> with color, face or size, and
// the end tags </p> and </br>) ends SVG and MathML content, so a <noscript>
// after it is an HTML one and goes; any other tag keeps the content foreign,
// and so does a <noscript> in it. Inside an integration point it goes too.
TEST(NoscriptStripTest, FollowsTheParsersForeignContentLists) {
  const char* const kNoscript = "<noscript>N</noscript>";
  auto removed = [&](const std::string& before) {
    return Strip(absl::StrCat(before, kNoscript)) == before;
  };
  auto kept = [&](const std::string& before) {
    const std::string html = absl::StrCat(before, kNoscript);
    return Strip(html) == html;
  };
  for (const char* name :
       {"b",      "big",    "blockquote", "body",    "br",    "center", "code",
        "dd",     "div",    "dl",         "dt",      "em",    "embed",  "h1",
        "h2",     "h3",     "h4",         "h5",      "h6",    "head",   "hr",
        "i",      "img",    "li",         "listing", "menu",  "meta",   "nobr",
        "ol",     "p",      "pre",        "ruby",    "s",     "small",  "span",
        "strong", "strike", "sub",        "sup",     "table", "tt",     "u",
        "ul",     "var",    "DIV"}) {
    EXPECT_TRUE(removed(absl::StrCat("<svg><g><", name, ">"))) << name;
    EXPECT_TRUE(removed(absl::StrCat("<math><mrow><", name, ">"))) << name;
  }
  for (const char* font :
       {"<font color=red>", "<font face=x>", "<font x=1 SIZE=2>"}) {
    EXPECT_TRUE(removed(absl::StrCat("<svg>", font))) << font;
  }
  for (const char* name : {"a", "link", "input", "form", "select", "option",
                           "td", "tr", "font", "article", "section"}) {
    EXPECT_TRUE(kept(absl::StrCat("<svg><", name, ">"))) << name;
  }
  EXPECT_TRUE(kept("<svg><font x=1>"));
  EXPECT_TRUE(removed("<svg><g></p>"));
  EXPECT_TRUE(removed("<math><mrow></BR>"));
  for (const char* point :
       {"<svg><foreignObject>", "<svg><desc>", "<svg><title>",
        "<svg><FOREIGNOBJECT>", "<math><mi>", "<math><mo>", "<math><mn>",
        "<math><ms>", "<math><mtext>",
        "<math><annotation-xml encoding=\"text/html\">",
        "<math><annotation-xml encoding='APPLICATION/XHTML+XML'>",
        "<math><annotation-xml><svg><desc>",
        "<svg><foreignObject><math><mi>"}) {
    EXPECT_TRUE(removed(point)) << point;
  }
  for (const char* foreign :
       {"<math><mi><mglyph>", "<math><mi><malignmark>", "<math><title>",
        "<math><annotation-xml>",
        "<math><annotation-xml encoding=\"image/svg+xml\">",
        "<math><svg><desc>", "<svg><math><mi>", "<svg><mi>"}) {
    EXPECT_TRUE(kept(foreign)) << foreign;
  }
}

TEST(NoscriptStripTest, RemovesTheWorkersOwnAsyncFallbackCopy) {
  EXPECT_EQ(Strip("<head><link rel=preload as=style href=/a.css "
                  "data-pagespeed-async>"
                  "<noscript data-pagespeed-async-fallback>"
                  "<link rel=\"stylesheet\" href=\"/a.css\"></noscript>"
                  "</head>"),
            "<head><link rel=preload as=style href=/a.css "
            "data-pagespeed-async></head>");
}

TEST(NoscriptStripTest, LessThanThatStartsNoTagIsText) {
  EXPECT_EQ(Strip("a < b <3 <noscript>x</noscript>c"), "a < b <3 c");
}

TEST(NoscriptStripTest, RecordsRemovedRangesAndMapsOffsets) {
  //                  0123456789...
  const std::string html = "ab<noscript>x</noscript>cd<noscript>y</noscript>e";
  NoscriptStripResult r = StripNoscriptElements(html);
  ASSERT_EQ(r.html, "abcde");
  ASSERT_EQ(r.removed_ranges.size(), 2u);
  EXPECT_EQ(r.removed_ranges[0].first, 2u);
  EXPECT_EQ(r.removed_ranges[0].second, 24u);
  EXPECT_EQ(r.removed_ranges[1].first, 26u);
  EXPECT_EQ(r.removed_ranges[1].second, 48u);

  EXPECT_EQ(r.MapOffset(0), 0u);
  EXPECT_EQ(r.MapOffset(2), 2u);   // where the first range starts
  EXPECT_EQ(r.MapOffset(10), 2u);  // inside it: where it stood
  EXPECT_EQ(r.MapOffset(24), 2u);  // just after it: 'c'
  EXPECT_EQ(r.MapOffset(25), 3u);  // 'd'
  EXPECT_EQ(r.MapOffset(26), 4u);
  EXPECT_EQ(r.MapOffset(30), 4u);
  EXPECT_EQ(r.MapOffset(48), 4u);  // 'e'
  EXPECT_EQ(r.MapOffset(html.size()), r.html.size());
}

// Review cases, and more, each checked in Chromium (145
// and the product's 154): the JS-on render of the input equals the JS-off
// render of the expected output (tools/async-css-probe/noscript_compare.mjs).
// The context around each case is a small page with a styled hero.
struct Case {
  const char* name;
  const char* in;
  const char* out;
};

constexpr char kHead[] =
    "<!doctype html><html><head><title>T</title>"
    "<style>.hero{height:200px;background:red}</style></head><body>";
constexpr char kTail[] = "<div class=hero>HERO</div><p>AFTER</p></body></html>";

constexpr Case kCases[] = {
    // c01-c04: attributes. A quote opens a value only right after `=`.
    {"c01 baseline",
     "<noscript><style>.hero{display:none}</style><div>NOJS</div></noscript>",
     ""},
    {"c02 apostrophe in an unquoted value",
     "<noscript class=it's>NOJS</noscript>", ""},
    {"c03 apostrophe, more apostrophes later",
     "<noscript class=it's>NOJS</noscript><p>Don't</p>", "<p>Don't</p>"},
    {"c04 doubled quote", "<noscript data-a=\"x\"\">NOJS</noscript>", ""},
    {"c20 apostrophe in an earlier tag",
     "<img alt=Bob's src=x.png><noscript><div>NOJS</div></noscript>",
     "<img alt=Bob's src=x.png>"},
    // c05/c06/c22/e14/e24: script data, escaped and double-escaped.
    {"c05 double-escaped script writing a noscript",
     "<script><!--\n"
     "document.write('<script src=a.js></script>');\n"
     "document.write('<noscript>');\n//--></script>",
     "<script><!--\ndocument.write('<script src=a.js></script>');\n"
     "document.write('<noscript>');\n//--></script>"},
    {"c06 double-escaped script",
     "<script><!--\n"
     "document.write('<script src=a.js></script>');\n"
     "document.write('<noscript><img src=p.gif></noscript>');\n//--></script>",
     "<script><!--\ndocument.write('<script src=a.js></script>');\n"
     "document.write('<noscript><img src=p.gif></noscript>');\n//--></script>"},
    {"c22 double-escaped script, string",
     "<script><!--\n"
     "document.write('<script src=x.js></script>');\n"
     "var tag = \"<noscript>\";\n//--></script>",
     "<script><!--\ndocument.write('<script src=x.js></script>');\n"
     "var tag = \"<noscript>\";\n//--></script>"},
    {"c13 apostrophes before a script string",
     "<p title=it's>Hi</p><script>// don't\n"
     "if (a > b) { el.innerHTML = \"<noscript>\"; }</script>",
     "<p title=it's>Hi</p><script>// don't\n"
     "if (a > b) { el.innerHTML = \"<noscript>\"; }</script>"},
    {"e14 --> leaves the escape",
     "<script><!-- x --></script><noscript>N</noscript>",
     "<script><!-- x --></script>"},
    {"e24 nested script in an escape",
     "<script><!--<script></script>--></script><noscript>N</noscript>",
     "<script><!--<script></script>--></script>"},
    {"c14 end tag in a script attribute",
     "<script data-x=\"</script>\">var a=1;</script><noscript>N</noscript>",
     "<script data-x=\"</script>\">var a=1;</script>"},
    {"e13 quoted '>' in an end tag",
     "<script>var a=1;</script x=\">\"><noscript>N</noscript>",
     "<script>var a=1;</script x=\">\">"},
    // c07-c09, e18: comments.
    {"c07 <!-->", "<!--><noscript><div>NOJS</div></noscript>", "<!-->"},
    {"c08 <!--->", "<!---><noscript><div>NOJS</div></noscript>", "<!--->"},
    {"c09 --!>", "<!-- a --!><noscript><div>NOJS</div></noscript>",
     "<!-- a --!>"},
    {"e23 bogus comment", "<?php <noscript> ?><noscript>N</noscript>",
     "<?php <noscript> ?>"},
    // c10-c12, c16-c18, e08-e12, e19-e21, e26: foreign content.
    {"c10 svg breakout",
     "<svg width=10 height=10><p>x</p><noscript><div>NOJS</div></noscript>"
     "</svg>",
     "<svg width=10 height=10><p>x</p></svg>"},
    {"c11 svg closed by a parent's end tag",
     "<div><svg width=10 height=10></div><noscript><div>NOJS</div></noscript>",
     "<div><svg width=10 height=10></div>"},
    {"c12 foreignObject",
     "<svg><foreignObject><noscript><div>NOJS</div></noscript></foreignObject>"
     "</svg>",
     "<svg><foreignObject></foreignObject></svg>"},
    {"c16 annotation-xml text/html",
     "<math><annotation-xml encoding=\"text/html\"><noscript><div>N</div>"
     "</noscript></annotation-xml></math>",
     "<math><annotation-xml encoding=\"text/html\"></annotation-xml></math>"},
    {"c17 svg title is not RCDATA",
     "<svg><title><p>x</p></title></svg><noscript><div>N</div></noscript>",
     "<svg><title><p>x</p></title></svg>"},
    {"c18 '/' in an unquoted value is not self-closing",
     "<svg class=a/><noscript/><rect/></svg>",
     "<svg class=a/><noscript/><rect/></svg>"},
    {"e08 MathML text integration point",
     "<math><mi><noscript><b>N</b></noscript></mi></math>",
     "<math><mi></mi></math>"},
    {"e09 font with color breaks out",
     "<svg><font color=red>F</font><noscript>N</noscript></svg>",
     "<svg><font color=red>F</font></svg>"},
    {"e10 </div> closes the svg",
     "<div><svg></div><noscript><b>N</b></noscript>", "<div><svg></div>"},
    {"e11 CDATA in svg",
     "<svg><![CDATA[<noscript>]]></svg><noscript>N</noscript>",
     "<svg><![CDATA[<noscript>]]></svg>"},
    {"e12 svg style is not raw text", "<svg><style><noscript></style></svg>",
     "<svg><style><noscript></style></svg>"},
    {"e19 svg title is an integration point",
     "<svg><title><noscript>N</noscript></title></svg>",
     "<svg><title></title></svg>"},
    {"e20 a noscript in svg after desc is foreign",
     "<svg><desc><span>d</span></desc><noscript>N</noscript></svg>",
     "<svg><desc><span>d</span></desc><noscript>N</noscript></svg>"},
    {"e21 svg in annotation-xml",
     "<math><annotation-xml><svg><foreignObject><noscript>N</noscript>"
     "</foreignObject></svg></annotation-xml></math>",
     "<math><annotation-xml><svg><foreignObject></foreignObject></svg>"
     "</annotation-xml></math>"},
    {"e26 p breaks out of svg", "<p><svg><p>x</p><noscript>N</noscript></svg>",
     "<p><svg><p>x</p></svg>"},
    // c15, c19, e15, e25: other insertion modes.
    {"c15 noscript in select",
     "<select><option>a</option><noscript></select><div>X</div></noscript>"
     "</select>",
     "<select><option>a</option></select>"},
    {"c19 template", "<template><noscript><div>N</div></noscript></template>",
     "<template></template>"},
    {"e15 table",
     "<table><tr><td>c</td></tr><noscript><tr><td>N</td></tr>"
     "</noscript></table>",
     "<table><tr><td>c</td></tr></table>"},
    {"e25 li", "<ul><li>a<li>b<noscript><li>N</noscript></ul>",
     "<ul><li>a<li>b</ul>"},
    // e01-e07, e17: the <noscript> itself.
    {"e01 nested", "<noscript>a<noscript>b</noscript>c</noscript>",
     "c</noscript>"},
    {"e03 uppercase", "<NOSCRIPT><DIV>N</DIV></NoScript>", ""},
    {"e04 <noscript/>", "<noscript/>N</noscript>", ""},
    {"e05 style inside", "<noscript><style>p{}</noscript>X</style>",
     "X</style>"},
    {"e06 comment inside", "<noscript><!-- </noscript> -->", " -->"},
    {"e07 attribute inside", "<noscript><img alt=\"</noscript>\">", "\">"},
    {"e17 text banner", "<noscript>Please enable JavaScript</noscript>", ""},
    {"c21 GTM snippet",
     "<noscript><iframe src=\"https://www.googletagmanager.com/ns.html?id=G\" "
     "height=\"0\" width=\"0\" style=\"display:none;visibility:hidden\">"
     "</iframe></noscript>",
     ""},
};

TEST(NoscriptStripTest, ReadsThePageAsAScriptingBrowserDoes) {
  for (const Case& c : kCases) {
    SCOPED_TRACE(c.name);
    const std::string in = absl::StrCat(kHead, c.in, kTail);
    NoscriptStripResult r = StripNoscriptElements(in);
    EXPECT_EQ(r.html, absl::StrCat(kHead, c.out, kTail));
    EXPECT_TRUE(r.reliable) << r.unreliable_reason;
  }
}

TEST(NoscriptStripTest, HeadNoscriptWithLinkStyleAndMeta) {
  EXPECT_EQ(Strip("<html><head><title>T</title><noscript>"
                  "<link rel=stylesheet href=n.css><style>.h{display:none}"
                  "</style><meta http-equiv=refresh content=\"9;url=x\">"
                  "</noscript></head><body>x</body></html>"),
            "<html><head><title>T</title></head><body>x</body></html>");
}

// FAIL CLOSED: a removal that runs to the end with markup after its start
// tag could be a reading gone wrong, and the caller must not render it.
TEST(NoscriptStripTest, AnUnclosedNoscriptWithMarkupAfterIsUnreliable) {
  for (const char* tail :
       {"<noscript><div class=hero>HERO</div><p>AFTER</p>",
        "<noscript data-a=\"x\"<div class=hero>HERO</div><p>AFTER</p>",
        "<noscript class=\"never closed><div class=hero>HERO</div>"}) {
    SCOPED_TRACE(tail);
    NoscriptStripResult r =
        StripNoscriptElements(absl::StrCat("<body><p>a</p>", tail));
    EXPECT_FALSE(r.reliable);
    EXPECT_FALSE(r.unreliable_reason.empty());
    EXPECT_EQ(r.html, "<body><p>a</p>");
  }
  // Only text after it: a scripting browser shows nothing there either.
  NoscriptStripResult text = StripNoscriptElements(
      "<body><div class=hero>HERO</div><noscript>tail text only");
  EXPECT_TRUE(text.reliable);
  EXPECT_EQ(text.html, "<body><div class=hero>HERO</div>");
}

// What HtmlScanner would collect into `elements`, for the cross-check.
TEST(NoscriptStripTest, CountsTheStartTagsTheScannerCollects) {
  EXPECT_EQ(StripNoscriptElements("<html><head></head><body><p>a<br>b</p>"
                                  "<noscript><div>n</div></noscript>"
                                  "<template><span></span></template>"
                                  "<noembed><b></b></noembed>"
                                  "<svg><noscript><text/></noscript></svg>"
                                  "<style data-pagespeed-critical></style>"
                                  "<script>var s='<i>';</script>"
                                  "</body></html>")
                .rendered_start_tags,
            // html head body p br svg script
            7u);
  EXPECT_EQ(StripNoscriptElements("<body><p>a<noscript><i></i></noscript>"
                                  "<svg><rect/></svg></body>")
                .rendered_tags,
            (std::vector<std::string>{"body", "p", "svg", "rect"}));
}

std::vector<CollectedElement> Elements(
    std::initializer_list<const char*> tags) {
  std::vector<CollectedElement> out;
  for (const char* t : tags) {
    CollectedElement e;
    e.tag_name = t;
    out.push_back(std::move(e));
  }
  return out;
}

NoscriptStripResult Kept(std::initializer_list<const char*> tags) {
  NoscriptStripResult r;
  for (const char* t : tags) r.rendered_tags.emplace_back(t);
  r.rendered_start_tags = r.rendered_tags.size();
  return r;
}

TEST(NoscriptStripTest, AgreementNeedsTheSameElementsAfterBody) {
  const auto kept = Kept({"html", "head", "title", "body", "div", "p"});
  EXPECT_TRUE(NoscriptStripAgreesWithScan(
      kept, Elements({"html", "head", "title", "body", "div", "p"})));
  // Case-insensitive, as the scanner keeps a name's spelling.
  EXPECT_TRUE(NoscriptStripAgreesWithScan(
      kept, Elements({"HTML", "head", "title", "BODY", "Div", "p"})));
  // Before <body> only the count matters.
  EXPECT_TRUE(NoscriptStripAgreesWithScan(
      kept, Elements({"html", "head", "meta", "body", "div", "p"})));
  // After it, one element missing, extra or different is a disagreement,
  // although the counts are within tolerance.
  EXPECT_FALSE(NoscriptStripAgreesWithScan(
      kept, Elements({"html", "head", "title", "body", "div"})));
  EXPECT_FALSE(NoscriptStripAgreesWithScan(
      kept, Elements({"html", "head", "title", "body", "div", "p", "b"})));
  EXPECT_FALSE(NoscriptStripAgreesWithScan(
      kept, Elements({"html", "head", "title", "body", "div", "span"})));
}

std::vector<CollectedElement> ElementsOf(const std::vector<std::string>& v) {
  std::vector<CollectedElement> out;
  out.reserve(v.size());
  for (const std::string& t : v) {
    CollectedElement e;
    e.tag_name = t;
    out.push_back(std::move(e));
  }
  return out;
}

NoscriptStripResult KeptOf(const std::vector<std::string>& v) {
  NoscriptStripResult r;
  r.rendered_tags = v;
  r.rendered_start_tags = v.size();
  return r;
}

// body, then `n` alternating div/span pairs (so an alignment cannot slide).
std::vector<std::string> Page(size_t n) {
  std::vector<std::string> v = {"body"};
  for (size_t i = 0; i < n; ++i) {
    v.emplace_back("div");
    v.emplace_back(i % 3 == 0 ? "p" : "span");
  }
  return v;
}

TEST(NoscriptStripTest, AgreementPastTheFirst200AllowsIsolatedDifferences) {
  const std::vector<std::string> names = Page(600);
  EXPECT_TRUE(NoscriptStripAgreesWithScan(KeptOf(names), ElementsOf(names)));
  // Past the exact window, isolated differences (an element read as another,
  // or one or two on one side only) pass while they are few enough.
  std::vector<std::string> kept = names;
  for (size_t at : {300u, 500u}) kept[at] = "b";  // substitutions
  kept.erase(kept.begin() + 900);                 // one missing
  kept.insert(kept.begin() + 1000, "i");          // one extra
  EXPECT_TRUE(NoscriptStripAgreesWithScan(KeptOf(kept), ElementsOf(names)));
  // Too many of them, though each is isolated: 2 * max(3, 1%) of ~1200 is
  // 24 edits, and each substitution is two.
  for (size_t at = 210; at < 1190; at += 70) kept[at] = "b";
  EXPECT_FALSE(NoscriptStripAgreesWithScan(KeptOf(kept), ElementsOf(names)));
  // Inside the window, one differing name does not pass.
  std::vector<std::string> swapped = names;
  swapped[kExactElementsAfterBody] = "b";
  EXPECT_FALSE(NoscriptStripAgreesWithScan(KeptOf(swapped), ElementsOf(names)));
  swapped = names;
  swapped[kExactElementsAfterBody + 1] = "b";
  EXPECT_TRUE(NoscriptStripAgreesWithScan(KeptOf(swapped), ElementsOf(names)));
}

// A removal that went wrong deletes a contiguous run, wherever the
// fold is. Past the window, a run of three or more elements the scanner has
// and the removal lacks is a disagreement, although the counts are within
// tolerance (w02/w05: 1222 vs 1226).
TEST(NoscriptStripTest, AgreementCatchesARunLostPastTheWindow) {
  const std::vector<std::string> names = Page(600);
  for (size_t run : {1u, 2u, 3u, 4u}) {
    SCOPED_TRACE(run);
    std::vector<std::string> kept = names;
    kept.erase(kept.begin() + 432, kept.begin() + 432 + run);
    EXPECT_EQ(NoscriptStripAgreesWithScan(KeptOf(kept), ElementsOf(names)),
              run <= kMaxDeletionRun);
    // The same at the end of the document.
    kept = names;
    kept.resize(kept.size() - run);
    EXPECT_EQ(NoscriptStripAgreesWithScan(KeptOf(kept), ElementsOf(names)),
              run <= kMaxDeletionRun);
  }
  // And a run the removal has and the scanner lacks.
  std::vector<std::string> extra = names;
  extra.insert(extra.begin() + 432, {"b", "b", "b"});
  EXPECT_FALSE(NoscriptStripAgreesWithScan(KeptOf(extra), ElementsOf(names)));
}

// Review case n21: a removal that loses the fold (three divs and
// the hero) but keeps the 1000 spans after it is within the count tolerance
// (1008 vs 1012); the names after <body> are not.
TEST(NoscriptStripTest, AgreementCatchesALostFold) {
  std::vector<CollectedElement> scanned =
      Elements({"html", "head", "title", "link", "body", "span", "div", "svg",
                "noscript", "div", "div", "div", "div"});
  NoscriptStripResult lost = Kept({"html", "head", "title", "link", "body",
                                   "span", "div", "svg", "noscript"});
  for (int i = 0; i < 1000; ++i) {
    CollectedElement e;
    e.tag_name = "span";
    scanned.push_back(e);
    lost.rendered_tags.emplace_back("span");
  }
  ASSERT_LE(scanned.size() - lost.rendered_tags.size(), 12u);
  EXPECT_FALSE(NoscriptStripAgreesWithScan(lost, scanned));
}

// And the real removal of that page keeps the fold and agrees.
TEST(NoscriptStripTest, TheMisnestedPageKeepsItsFoldAndAgrees) {
  std::string page =
      "<!doctype html><html><head><title>T</title>"
      "<link rel=stylesheet href=/app.css></head><body>"
      "<span><div><svg width=10 height=10></span><noscript/></svg>"
      "<div class=f0>FOLD0</div><div class=f1>FOLD1</div>"
      "<div class=f2>FOLD2</div><div class=hero>HERO</div>"
      "<noscript><img src=p.gif width=1 height=1></noscript>";
  for (int i = 0; i < 1000; ++i) absl::StrAppend(&page, "<span>x</span> ");
  page += "</body></html>";
  NoscriptStripResult r = StripNoscriptElements(page);
  ASSERT_TRUE(r.reliable);
  EXPECT_EQ(r.removed, 1u);
  EXPECT_NE(r.html.find("<div class=hero>HERO</div><span>"), std::string::npos);
  HtmlScanner scanner;
  EXPECT_TRUE(NoscriptStripAgreesWithScan(
      r, scanner.Scan("http://example.com/", page).elements));
}

// A removal that went wrong only deletes. A run of two deleted
// elements (w06's bite) is refused; so are more than kMaxDeletions single
// deletions in all (w09 took 8 bites of 2), while elements read as other
// elements and extra ones keep the isolated-difference tolerance.
TEST(NoscriptStripTest, DeletionsHaveAStricterBudget) {
  const std::vector<std::string> names = Page(600);
  auto agrees_without = [&](std::initializer_list<size_t> at) {
    std::vector<std::string> kept = names;
    std::vector<size_t> sorted(at);
    std::sort(sorted.rbegin(), sorted.rend());
    for (size_t i : sorted) kept.erase(kept.begin() + i);
    return NoscriptStripAgreesWithScan(KeptOf(kept), ElementsOf(names));
  };
  EXPECT_TRUE(agrees_without({433}));
  EXPECT_FALSE(agrees_without({433, 434}));  // one bite of two
  EXPECT_TRUE(agrees_without({433, 601, 803}));
  EXPECT_FALSE(agrees_without({433, 601, 803, 1001}));  // four in all
  // w09: eight bites of two.
  std::vector<std::string> bitten = names;
  for (size_t bite = 0; bite < 8; ++bite) {
    const size_t at = 1100 - bite * 100;
    bitten.erase(bitten.begin() + at, bitten.begin() + at + 2);
  }
  EXPECT_FALSE(NoscriptStripAgreesWithScan(KeptOf(bitten), ElementsOf(names)));
  // Insertions are not deletions.
  std::vector<std::string> extra = names;
  for (size_t at : {1001u, 803u, 601u, 433u})
    extra.insert(extra.begin() + at, "i");
  extra.insert(extra.begin() + 700, {"i", "i"});
  EXPECT_TRUE(NoscriptStripAgreesWithScan(KeptOf(extra), ElementsOf(names)));
  // But deletions count gross: an element read as another is
  // a deletion too, so a two-element bite cannot be paired away by two
  // insertions (a `<!--><i></i><b></b>-->` the scanner's lexer reads as one
  // comment), and more than kMaxDeletions substitutions are refused.
  std::vector<std::string> paired = names;
  paired[433] = "i";
  paired[434] = "b";
  EXPECT_FALSE(NoscriptStripAgreesWithScan(KeptOf(paired), ElementsOf(names)));
  std::vector<std::string> other = names;
  for (size_t at : {300u, 601u, 803u}) other[at] = "b";
  EXPECT_TRUE(NoscriptStripAgreesWithScan(KeptOf(other), ElementsOf(names)));
  other[1001] = "b";
  EXPECT_FALSE(NoscriptStripAgreesWithScan(KeptOf(other), ElementsOf(names)));
}

// Review pages: behind a hidden nav long enough to put the fold
// past the exact window (2 + 220 elements here), a model drift
// (a nested <a> across a foreignObject, quirks doctypes the model reads as
// no-quirks, bites repeated four times) makes an svg's <noscript/> take the
// fold, and a `<!--><i></i><b></b>-->` after it adds the two elements the
// lexer does not see, to pair the deletion away. Every one must be refused.
// g1 (an ignored nested <form> that used to close an open <p>) is now read
// right: the fold stays and the readings agree.
std::string ReviewPage(std::string_view doctype, std::string_view body,
                       int repeat = 1) {
  std::string page = absl::StrCat(
      doctype,
      "<html><head><title>T</title><link rel=stylesheet href=/app.css>"
      "</head><body><nav hidden><ul>");
  for (int i = 0; i < 110; ++i) absl::StrAppend(&page, "<li><a href=/>l</a>");
  absl::StrAppend(&page, "</ul></nav>");
  for (int i = 0; i < repeat; ++i) {
    absl::StrAppend(&page, body,
                    "<noscript><img src=p.gif width=1 height=1></noscript>"
                    "<!--><i></i><b></b>-->");
  }
  for (int i = 0; i < 1000; ++i) absl::StrAppend(&page, "<span>x</span> ");
  page += "</body></html>";
  return page;
}

TEST(NoscriptStripTest, PairedAwayDeletionsAreRefused) {
  struct Case {
    const char* name;
    std::string page;
  };
  const std::string fold = "<div class=hero>HERO</div><p class=lead>LEAD</p>";
  const std::string drift =
      "<span><p>x<table><tr><td>t</td></tr></table><svg width=10 height=10>"
      "</span><noscript/></svg>" +
      fold;
  const Case refused[] = {
      {"f2", ReviewPage("<!doctype html>",
                        "<a href=/1><svg width=10 height=10><foreignObject "
                        "width=10 height=10><a href=/2>x</a></foreignObject>"
                        "</a><noscript/></svg>" +
                            fold)},
      {"q2", ReviewPage("<!DOCTYPE HTML PUBLIC \"-//W3C//DTD HTML 4.01 "
                        "Transitional//EN\">",
                        drift)},
      {"q3", ReviewPage("<!DOCTYPE foo>", drift)},
      {"q4", ReviewPage("x<!doctype html>", drift)},
      {"g3", ReviewPage("<!doctype html>",
                        "<a href=/1><svg width=10 height=10><foreignObject "
                        "width=10 height=10><a href=/2>x</a></foreignObject>"
                        "</a><noscript/></svg><div>B</div><b>bold</b>",
                        4)},
  };
  HtmlScanner scanner;
  for (const Case& c : refused) {
    SCOPED_TRACE(c.name);
    NoscriptStripResult r = StripNoscriptElements(c.page);
    EXPECT_FALSE(NoscriptStripAgreesWithScan(
        r, scanner.Scan("http://example.com/", c.page).elements));
  }
  const std::string g1 = ReviewPage(
      "<!doctype html>",
      "<form><span><p>x<form><svg width=10 height=10></span><noscript/>"
      "</svg>" +
          fold);
  NoscriptStripResult r = StripNoscriptElements(g1);
  EXPECT_NE(r.html.find(fold), std::string::npos);
  EXPECT_TRUE(NoscriptStripAgreesWithScan(
      r, scanner.Scan("http://example.com/", g1).elements));
}

// An ignored nested <form> is ignored entirely; it does not
// close an open <p> on the way (g1's drift).
TEST(NoscriptStripTest, AnIgnoredFormClosesNothing) {
  // The <p> stays open, so the <span> in it does too; </span> closes the
  // <span> and the <svg> in it, and the <noscript/> after it is HTML.
  const std::string page =
      "<!doctype html><body><form><p><span>x<form><svg width=10 height=10>"
      "</span><noscript/>N</noscript><div class=hero>H</div></body>";
  EXPECT_EQ(Strip(page),
            "<!doctype html><body><form><p><span>x<form><svg width=10 "
            "height=10></span><div class=hero>H</div></body>");
}

// The start tags that close an open element of the same kind. In each,
// the first one (and the <span> in it) closes, so the later </span> is
// ignored, the <svg> stays open and its <noscript> is foreign: nothing goes.
TEST(NoscriptStripTest, AStartTagClosesAnOpenOneOfItsKind) {
  for (const char* body :
       {"<a href=/1><span>one<a href=/2><svg width=10 height=10></span>",
        "<button><span>one<button><svg width=10 height=10></span>",
        "<nobr><span>one<nobr><svg width=10 height=10></span>"}) {
    SCOPED_TRACE(body);
    const std::string page =
        absl::StrCat("<!doctype html><body>", body,
                     "<noscript><b>N</b></noscript><div class=hero>H</div>"
                     "</body>");
    NoscriptStripResult r = StripNoscriptElements(page);
    EXPECT_TRUE(r.reliable);
    EXPECT_EQ(r.html, page);
  }
}

// The optional ones: without a doctype (quirks mode) <table> does not
// close an open <p>, so the <span> stays open, </span> closes it and the
// <svg> with it; and a nested <form> start tag is ignored, also after an
// ancestor's end tag popped the first form (the pointer outlives it).
TEST(NoscriptStripTest, QuirksTableAndNestedForm) {
  const std::string table =
      "<p><span><table><tr><td>c</td></tr></table><svg width=10 height=10>"
      "</span><noscript><b>N</b></noscript><div class=hero>H</div></body>";
  EXPECT_EQ(Strip(absl::StrCat("<html><body>", table)),
            "<html><body><p><span><table><tr><td>c</td></tr></table>"
            "<svg width=10 height=10></span><div class=hero>H</div></body>");
  // With a doctype, <table> closes the <p> and the <span>: nothing goes.
  const std::string with_doctype = absl::StrCat("<!doctype html><body>", table);
  EXPECT_EQ(Strip(with_doctype), with_doctype);
  // The second <form> is ignored, so its </form> closes nothing and the <svg>
  // stays open.
  const std::string forms =
      "<!doctype html><body><div><form></div><form><span><svg width=10 "
      "height=10></form><noscript><b>N</b></noscript><div class=hero>H</div>"
      "</body>";
  EXPECT_EQ(Strip(forms), forms);
}

// Cases w02/w05: "close a p element" in full. A <div> closes the <p> and the
// <span> opened in it, so the later </span> is ignored (the <body> is special)
// and the <svg> stays open: its <noscript/> is foreign, and only the pixel
// <noscript> after the fold goes. The same for a void <hr>, which closes a
// <p> as well. (With a doctype: in quirks mode <table> does not; see
// QuirksTableAndNestedForm.)
TEST(NoscriptStripTest, AStartTagClosesAnOpenParagraphInButtonScope) {
  for (const char* closer : {"<div>X</div>", "<hr>", "<table></table>"}) {
    SCOPED_TRACE(closer);
    const std::string head =
        absl::StrCat("<!doctype html><body><p><span>", closer,
                     "<svg width=10 height=10></span><noscript/></svg>"
                     "<div class=hero>HERO</div><div class=f0>FOLD0</div>");
    NoscriptStripResult r = StripNoscriptElements(
        absl::StrCat(head,
                     "<noscript><img src=p.gif width=1 height=1></noscript>"
                     "<span>x</span></body>"));
    EXPECT_TRUE(r.reliable);
    EXPECT_EQ(r.html, absl::StrCat(head, "<span>x</span></body>"));
  }
}

TEST(NoscriptStripTest, TheW02PageKeepsItsFoldAndAgrees) {
  std::string page =
      "<!doctype html><html><head><title>T</title></head><body><nav hidden>"
      "<ul>";
  for (int i = 0; i < 70; ++i) absl::StrAppend(&page, "<li><a href=/>l</a>");
  absl::StrAppend(&page,
                  "</ul></nav><p><span><div>X</div><svg width=10 height=10>"
                  "</span><noscript/></svg><div class=hero>HERO</div>"
                  "<div class=f0>FOLD0</div><noscript><img src=p.gif "
                  "width=1 height=1></noscript>");
  for (int i = 0; i < 1000; ++i) absl::StrAppend(&page, "<span>x</span> ");
  page += "</body></html>";
  NoscriptStripResult r = StripNoscriptElements(page);
  ASSERT_TRUE(r.reliable);
  EXPECT_EQ(r.removed, 1u);
  EXPECT_NE(r.html.find("<div class=hero>HERO</div><div class=f0>FOLD0</div>"
                        "<span>"),
            std::string::npos);
  HtmlScanner scanner;
  EXPECT_TRUE(NoscriptStripAgreesWithScan(
      r, scanner.Scan("http://example.com/", page).elements));
}

// React/Next.js streaming markup puts a <template> in an <svg>. The
// scanner leaves every <template> (and <noembed>, <noframes>) and what it
// holds out of its elements, whatever the namespace, so the removal's list
// does too, or such a page is refused for nothing. Seen on a large portal page.
TEST(NoscriptStripTest, ATemplateInSvgIsLeftOutOfTheListLikeTheScanner) {
  const std::string page =
      "<html><body><svg><!--$?--><template id=\"B:2\"><path/></template>"
      "<noembed><g/></noembed></svg><noscript><img src=p.gif width=1 "
      "height=1></noscript><div class=hero>HERO</div></body></html>";
  NoscriptStripResult r = StripNoscriptElements(page);
  EXPECT_EQ(r.rendered_tags,
            (std::vector<std::string>{"html", "body", "svg", "div"}));
  HtmlScanner scanner;
  EXPECT_TRUE(NoscriptStripAgreesWithScan(
      r, scanner.Scan("http://example.com/", page).elements));
}

// More review cases: the removal itself must read misnested end
// tags as the tree builder does, and the name check must catch a reading
// that still goes wrong.
TEST(NoscriptStripTest, MisnestedEndTagsCloseWhatTheTreeBuilderCloses) {
  // n01: </span> across an open <div> is ignored, so the <svg> stays open,
  // the <noscript/> in it is foreign, and the fold survives.
  const std::string n01 =
      "<body><span><div><svg width=10 height=10></span><noscript/></svg>"
      "<div class=f0>FOLD0</div><div class=hero>HERO</div><p>AFTER</p>"
      "<footer><noscript><img src=p.gif width=1 height=1></noscript>"
      "</footer></body>";
  NoscriptStripResult r = StripNoscriptElements(n01);
  EXPECT_TRUE(r.reliable);
  EXPECT_EQ(r.html,
            "<body><span><div><svg width=10 height=10></span><noscript/></svg>"
            "<div class=f0>FOLD0</div><div class=hero>HERO</div><p>AFTER</p>"
            "<footer></footer></body>");
  // n03: </h1> closes an open <h2>, and the <svg> in it.
  EXPECT_EQ(Strip("<body><h2><svg></h1><noscript><div>NOJS</div></noscript>"
                  "<div class=hero>HERO</div></body>"),
            "<body><h2><svg></h1><div class=hero>HERO</div></body>");
  // n17: </a> with a special element after it (the adoption agency's
  // furthest block) leaves that element open and closes the <svg> after it.
  EXPECT_EQ(Strip("<body><a href=#><div><svg></a><noscript><div>N</div>"
                  "</noscript><div class=hero>HERO</div></body>"),
            "<body><a href=#><div><svg></a><div class=hero>HERO</div></body>");
  // </form> removes the form alone: the <svg> opened in it stays open.
  EXPECT_EQ(Strip("<body><form><svg></form><noscript><b>N</b></noscript>"
                  "</svg><div class=hero>HERO</div></body>"),
            "<body><form><svg></form><noscript><b>N</b></noscript>"
            "</svg><div class=hero>HERO</div></body>");
  // Without one, everything after the <a> closes with it.
  EXPECT_EQ(Strip("<body><a href=#><svg></a><noscript>N</noscript>"
                  "<div class=hero>HERO</div></body>"),
            "<body><a href=#><svg></a><div class=hero>HERO</div></body>");
  // </ul> is not in scope past a foreignObject: ignored.
  EXPECT_EQ(Strip("<body><ul><li><svg><foreignObject></ul>"
                  "<noscript><div>N</div></noscript></foreignObject></svg>"
                  "<div class=hero>HERO</div></body>"),
            "<body><ul><li><svg><foreignObject></ul></foreignObject></svg>"
            "<div class=hero>HERO</div></body>");
}

}  // namespace
}  // namespace pagespeed
