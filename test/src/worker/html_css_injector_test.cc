// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

// Tests for the HTML Critical CSS Injector.

#include "src/worker/html_css_injector.h"

#include <cstddef>
#include <string>
#include <string_view>

#include "gtest/gtest.h"
#include "lib/base/string_writer.h"
#include "lib/html/compat/message_handler.h"
#include "lib/html/empty_html_filter.h"
#include "lib/html/html_element.h"
#include "lib/html/html_keywords.h"
#include "lib/html/html_node.h"
#include "lib/html/html_parse.h"
#include "lib/html/html_writer_filter.h"

namespace pagespeed {
namespace {

// Test: Basic injection before </head>.
TEST(HtmlCssInjectorTest, InjectBeforeHead) {
  std::string html =
      "<html><head><title>Test</title></head><body>Hello</body></html>";
  std::string css = "body { margin: 0; }";

  auto result = InjectCriticalCss(html, css);
  EXPECT_TRUE(result.success) << result.error_message;
  EXPECT_TRUE(result.injected);
  // Style tag should appear before </head>.
  EXPECT_NE(result.html.find(
                "<style data-pagespeed-critical>body { margin: 0; }</style>"
                "</head>"),
            std::string::npos);
  // Original content should still be present.
  EXPECT_NE(result.html.find("<title>Test</title>"), std::string::npos);
  EXPECT_NE(result.html.find("Hello"), std::string::npos);
}

// Test: Uppercase </HEAD>.
TEST(HtmlCssInjectorTest, InjectBeforeUppercaseHead) {
  std::string html =
      "<HTML><HEAD><TITLE>Test</TITLE></HEAD><BODY>Hello</BODY></HTML>";
  std::string css = "h1 { color: red; }";

  auto result = InjectCriticalCss(html, css);
  EXPECT_TRUE(result.success);
  EXPECT_TRUE(result.injected);
  EXPECT_NE(result.html.find(
                "<style data-pagespeed-critical>h1 { color: red; }</style>"
                "</HEAD>"),
            std::string::npos);
}

// Test: Mixed case </hEaD>.
TEST(HtmlCssInjectorTest, InjectBeforeMixedCaseHead) {
  std::string html = "<html><head></hEaD><body></body></html>";
  std::string css = "p { font-size: 14px; }";

  auto result = InjectCriticalCss(html, css);
  EXPECT_TRUE(result.success);
  EXPECT_TRUE(result.injected);
  EXPECT_NE(result.html.find("</style></hEaD>"), std::string::npos);
}

// Test: Missing </head> falls back to </body>.
TEST(HtmlCssInjectorTest, FallbackToBody) {
  std::string html = "<html><body><p>Hello</p></body></html>";
  std::string css = "p { color: blue; }";

  auto result = InjectCriticalCss(html, css);
  EXPECT_TRUE(result.success);
  EXPECT_TRUE(result.injected);
  EXPECT_NE(result.html.find("</style></body>"), std::string::npos);
}

// Test: Missing both </head> and </body> falls back to after <head>.
TEST(HtmlCssInjectorTest, FallbackToAfterHead) {
  std::string html = "<html><head><title>T</title><p>Hello</p>";
  std::string css = "p { color: blue; }";

  auto result = InjectCriticalCss(html, css);
  EXPECT_TRUE(result.success);
  EXPECT_TRUE(result.injected);
  // Style tag should be injected after <head>.
  size_t head_end = result.html.find("<head>") + 6;
  EXPECT_NE(result.html.find("<style data-pagespeed-critical>", head_end),
            std::string::npos);
}

// Test: No head or body at all — insert at document start.
TEST(HtmlCssInjectorTest, FallbackToDocumentStart) {
  std::string html = "<div>Hello World</div>";
  std::string css = "div { margin: 0; }";

  auto result = InjectCriticalCss(html, css);
  EXPECT_TRUE(result.success);
  EXPECT_TRUE(result.injected);
  EXPECT_EQ(result.html.find("<style data-pagespeed-critical>"), 0u);
}

// Test: </head> inside HTML comment is skipped.
TEST(HtmlCssInjectorTest, HeadInsideCommentSkipped) {
  std::string html =
      "<html><head>"
      "<!-- </head> this is a comment -->"
      "</head><body>Hello</body></html>";
  std::string css = "body { background: white; }";

  auto result = InjectCriticalCss(html, css);
  EXPECT_TRUE(result.success);
  EXPECT_TRUE(result.injected);
  // The injected style tag should be before the real </head>,
  // not the one inside the comment.
  size_t comment_end = result.html.find("-->");
  size_t style_pos = result.html.find("<style data-pagespeed-critical>");
  EXPECT_GT(style_pos, comment_end);
}

// Test: Empty CSS returns original HTML unchanged.
TEST(HtmlCssInjectorTest, EmptyCssNoInjection) {
  std::string html = "<html><head></head><body>Hello</body></html>";

  auto result = InjectCriticalCss(html, "");
  EXPECT_TRUE(result.success);
  EXPECT_FALSE(result.injected);
  EXPECT_EQ(result.html, html);
}

// Test: Empty HTML returns failure.
TEST(HtmlCssInjectorTest, EmptyHtmlFailure) {
  auto result = InjectCriticalCss("", "body { margin: 0; }");
  EXPECT_FALSE(result.success);
  EXPECT_FALSE(result.injected);
}

// Test: Multiple </head> inside comments, real one at the end.
TEST(HtmlCssInjectorTest, MultipleCommentsBeforeRealHead) {
  std::string html =
      "<html><head>"
      "<!-- </head> -->"
      "<!-- another </head> -->"
      "</head><body></body></html>";
  std::string css = "a { color: green; }";

  auto result = InjectCriticalCss(html, css);
  EXPECT_TRUE(result.success);
  EXPECT_TRUE(result.injected);
  // Style should be before the real </head>, after all comments.
  size_t second_comment_end = result.html.rfind("-->");
  size_t style_pos = result.html.find("<style data-pagespeed-critical>");
  EXPECT_GT(style_pos, second_comment_end);
}

// Test: Preserves existing style tags.
TEST(HtmlCssInjectorTest, PreservesExistingStyles) {
  std::string html =
      "<html><head>"
      "<style>.existing { color: red; }</style>"
      "</head><body></body></html>";
  std::string css = "body { margin: 0; }";

  auto result = InjectCriticalCss(html, css);
  EXPECT_TRUE(result.success);
  EXPECT_TRUE(result.injected);
  EXPECT_NE(result.html.find(".existing { color: red; }"), std::string::npos);
  EXPECT_NE(result.html.find("data-pagespeed-critical"), std::string::npos);
}

// Test 0a: CSS containing </style> XSS payload aborts injection.
TEST(HtmlCssInjectorTest, StyleCloseXssAborted) {
  std::string html = "<html><head></head><body></body></html>";
  std::string css = "body{}</style><script>alert(1)</script>";

  auto result = InjectCriticalCss(html, css);
  EXPECT_TRUE(result.success);
  EXPECT_FALSE(result.injected);
  // The HTML should be empty (not returned unchanged) since injection
  // was aborted due to dangerous CSS, not because CSS was empty.
  EXPECT_TRUE(result.html.empty());
}

// Test 0a: Case-insensitive </STYLE> detection.
TEST(HtmlCssInjectorTest, StyleCloseXssCaseInsensitive) {
  std::string html = "<html><head></head><body></body></html>";
  std::string css = "body{}</STYLE><script>alert(1)</script>";

  auto result = InjectCriticalCss(html, css);
  EXPECT_TRUE(result.success);
  EXPECT_FALSE(result.injected);
}

// Test 0a: </style followed by space (no closing ">") is still detected.
TEST(HtmlCssInjectorTest, StyleCloseXssWithSpace) {
  std::string html = "<html><head></head><body></body></html>";
  std::string css = "body{}</style <script>alert(1)</script>";

  auto result = InjectCriticalCss(html, css);
  EXPECT_TRUE(result.success);
  EXPECT_FALSE(result.injected);
}

// Test 0a: </style followed by tab is detected.
TEST(HtmlCssInjectorTest, StyleCloseXssWithTab) {
  std::string html = "<html><head></head><body></body></html>";
  std::string css = "body{}</style\t<script>alert(1)</script>";

  auto result = InjectCriticalCss(html, css);
  EXPECT_TRUE(result.success);
  EXPECT_FALSE(result.injected);
}

// Test 0a: </style followed by newline is detected.
TEST(HtmlCssInjectorTest, StyleCloseXssWithNewline) {
  std::string html = "<html><head></head><body></body></html>";
  std::string css = "body{}</style\n<script>alert(1)</script>";

  auto result = InjectCriticalCss(html, css);
  EXPECT_TRUE(result.success);
  EXPECT_FALSE(result.injected);
}

// Test 0a: Mixed case </sTyLe with tab is detected.
TEST(HtmlCssInjectorTest, StyleCloseXssMixedCaseWithTab) {
  std::string html = "<html><head></head><body></body></html>";
  std::string css = "body{}</sTyLe\t<script>alert(1)</script>";

  auto result = InjectCriticalCss(html, css);
  EXPECT_TRUE(result.success);
  EXPECT_FALSE(result.injected);
}

// Test 0a: Null bytes in CSS are stripped.
TEST(HtmlCssInjectorTest, NullBytesStripped) {
  std::string html = "<html><head></head><body></body></html>";
  std::string css = "body { margin: 0; }";
  css.push_back('\0');  // Append a null byte.
  css.append("extra");

  auto result = InjectCriticalCss(html, css);
  EXPECT_TRUE(result.success);
  EXPECT_TRUE(result.injected);
  // Null byte should have been stripped, leaving "body { margin: 0; }extra".
  EXPECT_NE(result.html.find("body { margin: 0; }extra"), std::string::npos);
}

// Test 0b: </head> inside <script> is not matched.
TEST(HtmlCssInjectorTest, HeadInsideScriptSkipped) {
  std::string html =
      "<html><head>"
      "<script>var s = \"</head>\";</script>"
      "</head><body></body></html>";
  std::string css = "body { color: red; }";

  auto result = InjectCriticalCss(html, css);
  EXPECT_TRUE(result.success);
  EXPECT_TRUE(result.injected);
  // Should inject before the real </head>, not the one inside <script>.
  size_t script_end = result.html.find("</script>");
  size_t style_pos = result.html.find("<style data-pagespeed-critical>");
  EXPECT_GT(style_pos, script_end);
}

// Test 0b: </head> inside <textarea> is not matched.
TEST(HtmlCssInjectorTest, HeadInsideTextareaSkipped) {
  std::string html =
      "<html><head>"
      "<textarea></head></textarea>"
      "</head><body></body></html>";
  std::string css = "body { margin: 0; }";

  auto result = InjectCriticalCss(html, css);
  EXPECT_TRUE(result.success);
  EXPECT_TRUE(result.injected);
  size_t textarea_end = result.html.find("</textarea>");
  size_t style_pos = result.html.find("<style data-pagespeed-critical>");
  EXPECT_GT(style_pos, textarea_end);
}

// Test 0b: </head> inside <style> is not matched.
TEST(HtmlCssInjectorTest, HeadInsideStyleSkipped) {
  std::string html =
      "<html><head>"
      "<style>/* </head> */</style>"
      "</head><body></body></html>";
  std::string css = "body { color: blue; }";

  auto result = InjectCriticalCss(html, css);
  EXPECT_TRUE(result.success);
  EXPECT_TRUE(result.injected);
  size_t style_tag_end = result.html.find("</style>");
  size_t injected_pos = result.html.find("<style data-pagespeed-critical>");
  EXPECT_GT(injected_pos, style_tag_end);
}

// Test 0b: </head> inside <title> is not matched.
TEST(HtmlCssInjectorTest, HeadInsideTitleSkipped) {
  std::string html =
      "<html><head>"
      "<title></head></title>"
      "</head><body></body></html>";
  std::string css = "body { color: green; }";

  auto result = InjectCriticalCss(html, css);
  EXPECT_TRUE(result.success);
  EXPECT_TRUE(result.injected);
  size_t title_end = result.html.find("</title>");
  size_t injected_pos = result.html.find("<style data-pagespeed-critical>");
  EXPECT_GT(injected_pos, title_end);
}

// Test 0b: </head> inside <xmp> is not matched.
TEST(HtmlCssInjectorTest, HeadInsideXmpSkipped) {
  std::string html =
      "<html><head>"
      "<xmp></head></xmp>"
      "</head><body></body></html>";
  std::string css = "body { color: purple; }";

  auto result = InjectCriticalCss(html, css);
  EXPECT_TRUE(result.success);
  EXPECT_TRUE(result.injected);
  size_t xmp_end = result.html.find("</xmp>");
  size_t injected_pos = result.html.find("<style data-pagespeed-critical>");
  EXPECT_GT(injected_pos, xmp_end);
}

// Test 0c: <header> is not matched when looking for <head>.
TEST(HtmlCssInjectorTest, HeaderNotMatchedAsHead) {
  // No </head>, no </body>, only <header> — should NOT match as <head>.
  // Falls through to strategy 4 (document start).
  std::string html = "<html><header>Hello</header></html>";
  std::string css = "body { margin: 0; }";

  auto result = InjectCriticalCss(html, css);
  EXPECT_TRUE(result.success);
  EXPECT_TRUE(result.injected);
  // Injection should be at document start (strategy 4), not after <header>.
  EXPECT_EQ(result.html.find("<style data-pagespeed-critical>"), 0u);
}

// Test 0c: <head class="foo"> still matches (word boundary after name).
TEST(HtmlCssInjectorTest, HeadWithAttributesMatches) {
  std::string html = "<html><head class=\"foo\"><title>T</title><p>Hello</p>";
  std::string css = "p { color: blue; }";

  auto result = InjectCriticalCss(html, css);
  EXPECT_TRUE(result.success);
  EXPECT_TRUE(result.injected);
  // Should inject after <head class="foo">, not at document start.
  size_t head_end = result.html.find("<head class=\"foo\">") + 18;
  size_t style_pos = result.html.find("<style data-pagespeed-critical>");
  EXPECT_EQ(style_pos, head_end);
}

// ========== Coverage: Empty HTML and missing tag ==========

TEST(HtmlCssInjectorTest, EmptyHtmlReturnsFailure) {
  auto result = InjectCriticalCss("", "body { color: red; }");
  EXPECT_FALSE(result.success);
  EXPECT_FALSE(result.injected);
  EXPECT_TRUE(result.html.empty());
}

TEST(HtmlCssInjectorTest, TagNotFoundFallsToDocStart) {
  // HTML without <head>, </head>, or </body> — falls through to
  // strategy 4: insert at document start.
  std::string html = "<p>Just a paragraph</p>";
  std::string css = "p { margin: 0; }";

  auto result = InjectCriticalCss(html, css);
  EXPECT_TRUE(result.success);
  EXPECT_TRUE(result.injected);
  // The style tag should be at position 0 (document start).
  EXPECT_EQ(result.html.find("<style data-pagespeed-critical>"), 0u)
      << "Without head/body tags, injection should be at document start";
  // Original content should still be present after the injected style.
  EXPECT_NE(result.html.find("<p>Just a paragraph</p>"), std::string::npos);
}

// ========== Coverage: TrySkipRawTextElement edge cases ==========

// Raw text tag followed by a non-boundary character (e.g., <scripting>)
// should NOT be treated as a raw text element.  This exercises the
// word-boundary check in TrySkipRawTextElement (lines 47-50).
TEST(HtmlCssInjectorTest, ScriptingTagNotTreatedAsScript) {
  // <scripting> should not be treated as <script>.
  // The </head> at the end should be found normally.
  std::string html =
      "<html><head>"
      "<scripting></head></scripting>"
      "</head><body></body></html>";
  std::string css = "body { color: red; }";

  auto result = InjectCriticalCss(html, css);
  EXPECT_TRUE(result.success);
  EXPECT_TRUE(result.injected);
  // The first </head> (inside <scripting>) is NOT inside a raw text element,
  // so it should be matched as the injection point.
  size_t style_pos = result.html.find("<style data-pagespeed-critical>");
  size_t first_head_close = result.html.find("</head>");
  EXPECT_LT(style_pos, first_head_close);
}

// Unclosed raw text element: <script> without </script>.
// TrySkipRawTextElement returns npos for the unclosed element (line 66),
// but the caller treats this the same as "not a raw text tag" and
// continues scanning.  The </head> is still found because the scanner
// advances past the unrecognized <script> character by character.
TEST(HtmlCssInjectorTest, UnclosedScriptTagStillFindsHead) {
  std::string html =
      "<html><head>"
      "<script>var x = 1; /* no closing script tag */"
      "</head><body></body></html>";
  std::string css = "body { color: red; }";

  auto result = InjectCriticalCss(html, css);
  EXPECT_TRUE(result.success);
  EXPECT_TRUE(result.injected);
  // The unclosed <script> causes TrySkipRawTextElement to return npos,
  // but </head> is still found by the character-by-character scan.
  EXPECT_NE(result.html.find("</style></head>"), std::string::npos);
}

// Unclosed HTML comment means no </head> or </body> can be found.
TEST(HtmlCssInjectorTest, UnclosedCommentBlocksAllMatches) {
  std::string html =
      "<html><head>"
      "<!-- this comment is never closed"
      "</head><body></body></html>";
  std::string css = "body { color: blue; }";

  auto result = InjectCriticalCss(html, css);
  EXPECT_TRUE(result.success);
  EXPECT_TRUE(result.injected);
  // Unclosed comment swallows everything, so injection falls through to
  // strategy 3 (after <head>) or strategy 4 (document start).
  size_t style_pos = result.html.find("<style data-pagespeed-critical>");
  // <head> tag does exist, so strategy 3 should work.
  size_t head_open_end = result.html.find("<head>") + 6;
  EXPECT_EQ(style_pos, head_open_end);
}

// Raw text element with closing tag missing '>' — the search for '>' after
// </script fails, causing TrySkipRawTextElement to return npos.
TEST(HtmlCssInjectorTest, ScriptClosingTagMissingAngleBracket) {
  std::string html =
      "<html><head>"
      "<script>code</script"
      "</head><body></body></html>";
  std::string css = "body { margin: 0; }";

  auto result = InjectCriticalCss(html, css);
  EXPECT_TRUE(result.success);
  EXPECT_TRUE(result.injected);
  // The </script without '>' means the closing tag is malformed.
  // TrySkipRawTextElement returns npos, so </head> cannot be found
  // through that path.  Injection still works via strategy 3 or 4.
}

// <head without closing '>' — FindOpenTagEnd returns npos for that tag.
// Falls through to strategy 4.
TEST(HtmlCssInjectorTest, HeadTagWithoutClosingAngleBracket) {
  std::string html = "<html><head no-close";
  std::string css = "body { margin: 0; }";

  auto result = InjectCriticalCss(html, css);
  EXPECT_TRUE(result.success);
  EXPECT_TRUE(result.injected);
  // No valid <head>, </head>, or </body> → document start.
  EXPECT_EQ(result.html.find("<style data-pagespeed-critical>"), 0u);
}

// ---------------------------------------------------------------------------
// InjectCriticalCssAt: the caller's position, the same sanitization
// ---------------------------------------------------------------------------

TEST(HtmlCssInjectorTest, InjectAtGivenOffset) {
  std::string html = "<html><head><title>T</title><meta name=x></head></html>";
  size_t pos = html.find("<meta");
  auto result = InjectCriticalCssAt(html, pos, "a{}");
  ASSERT_TRUE(result.success);
  EXPECT_TRUE(result.injected);
  EXPECT_EQ(result.html,
            "<html><head><title>T</title>"
            "<style data-pagespeed-critical>a{}</style>"
            "<meta name=x></head></html>");
}

TEST(HtmlCssInjectorTest, InjectAtStartAndEnd) {
  auto at_start = InjectCriticalCssAt("<p>x</p>", 0, "a{}");
  ASSERT_TRUE(at_start.success);
  EXPECT_EQ(at_start.html,
            "<style data-pagespeed-critical>a{}</style><p>x</p>");
  auto at_end = InjectCriticalCssAt("<p>x</p>", 8, "a{}");
  ASSERT_TRUE(at_end.success);
  EXPECT_EQ(at_end.html, "<p>x</p><style data-pagespeed-critical>a{}</style>");
}

TEST(HtmlCssInjectorTest, InjectAtRejectsOffsetPastEnd) {
  auto result = InjectCriticalCssAt("<p>x</p>", 9, "a{}");
  EXPECT_FALSE(result.success);
  EXPECT_FALSE(result.injected);
  EXPECT_TRUE(result.html.empty());
}

TEST(HtmlCssInjectorTest, InjectAtKeepsTheStyleTerminatorRefusal) {
  // Same contract as InjectCriticalCss: success with an EMPTY document.
  auto result = InjectCriticalCssAt("<html></html>", 6, "a{} </STYLE><script>");
  EXPECT_TRUE(result.success);
  EXPECT_FALSE(result.injected);
  EXPECT_TRUE(result.html.empty());
  EXPECT_FALSE(result.error_message.empty());

  auto null_bytes = InjectCriticalCssAt("<html></html>", 6,
                                        std::string("a{\0color:red}", 13));
  ASSERT_TRUE(null_bytes.success);
  EXPECT_NE(null_bytes.html.find("a{color:red}"), std::string::npos);
  EXPECT_EQ(null_bytes.html.find('\0'), std::string::npos);
}

TEST(HtmlCssInjectorTest, InjectAtEmptyInputs) {
  auto empty_css = InjectCriticalCssAt("<html></html>", 6, "");
  EXPECT_TRUE(empty_css.success);
  EXPECT_FALSE(empty_css.injected);
  EXPECT_EQ(empty_css.html, "<html></html>");
  auto empty_html = InjectCriticalCssAt("", 0, "a{}");
  EXPECT_FALSE(empty_html.success);
}

// ---------------------------------------------------------------------------
// CriticalCssNamesCascadeLayer: over-inclusive on purpose
// ---------------------------------------------------------------------------

TEST(HtmlCssInjectorTest, NamesCascadeLayer) {
  EXPECT_TRUE(CriticalCssNamesCascadeLayer("@layer theme{a{}}"));
  EXPECT_TRUE(CriticalCssNamesCascadeLayer("@layer theme, base, utilities;"));
  EXPECT_TRUE(CriticalCssNamesCascadeLayer("a{} @LAYER x.y { b{} }"));
  EXPECT_TRUE(CriticalCssNamesCascadeLayer("@layer /* c */ x{}"));
  EXPECT_TRUE(CriticalCssNamesCascadeLayer("@import url(a.css) layer(lib);"));
  EXPECT_TRUE(CriticalCssNamesCascadeLayer("@import 'a.css' layer;"));
  EXPECT_TRUE(
      CriticalCssNamesCascadeLayer("@media (min-width:1px){@layer x{}}"));

  EXPECT_FALSE(CriticalCssNamesCascadeLayer(""));
  EXPECT_FALSE(CriticalCssNamesCascadeLayer("a{color:red}"));
  EXPECT_FALSE(CriticalCssNamesCascadeLayer("@layer{a{}} @layer {b{}}"));
  EXPECT_FALSE(CriticalCssNamesCascadeLayer("@layer /* c */ {a{}}"));
  EXPECT_FALSE(CriticalCssNamesCascadeLayer("@layers x{}"));
  EXPECT_FALSE(CriticalCssNamesCascadeLayer("@import url(player.css);x{}"))
      << "\"layer\" counts only as the import condition, not inside a URL";
}

// Comments end where the serve path's HtmlLexer ends them, which is where a
// browser does: the placement replayed on this reading must be the serve
// path's, and the served page is read by a browser.
TEST(HtmlScanTest, CommentsEndWhereTheServePathLexerEndsThem) {
  EXPECT_EQ(html_scan::ScanComment("<!---->x", 0), 7u);
  EXPECT_EQ(html_scan::ScanComment("<!-- a -->x", 0), 10u);
  // These end at once, or at `--!>`.
  EXPECT_EQ(html_scan::ScanComment("<!-->x<!-- y -->", 0), 5u);
  EXPECT_EQ(html_scan::ScanComment("<!--->x<!-- y -->", 0), 6u);
  EXPECT_EQ(html_scan::ScanComment("<!-- a --!> b -->", 0), 11u);
  EXPECT_EQ(html_scan::ScanComment("<!-- a -!> b -->", 0), 16u);
  EXPECT_EQ(html_scan::ScanComment("<!-- a --!x b -->", 0), 17u);
  EXPECT_EQ(html_scan::ScanComment("<!-- never closed", 0), std::string::npos);
  EXPECT_EQ(html_scan::ScanComment("<!--", 0), std::string::npos);
  EXPECT_EQ(html_scan::ScanComment("<!---", 0), std::string::npos);

  EXPECT_EQ(html_scan::CommentText("<!-->", 0, 5), "");
  EXPECT_EQ(html_scan::CommentText("<!--->", 0, 6), "");
  EXPECT_EQ(html_scan::CommentText("<!---->", 0, 7), "");
  EXPECT_EQ(html_scan::CommentText("<!-- a --!>", 0, 11), " a ");
  EXPECT_EQ(html_scan::CommentText("<!--a--->", 0, 9), "a-");
}

// Records what the serve path's parser read: the first comment's text, and
// whether the <b> after the comment under test was read as an element.
class CommentRecorder : public net_instaweb::EmptyHtmlFilter {
 public:
  void StartDocument() override {
    comments = 0;
    first_comment.clear();
    b_elements = 0;
  }
  void Comment(net_instaweb::HtmlCommentNode* comment) override {
    if (comments++ == 0) first_comment = comment->contents();
  }
  void StartElement(net_instaweb::HtmlElement* element) override {
    if (element->name_str() == "b") ++b_elements;
  }
  const char* Name() const override { return "CommentRecorder"; }

  int comments = 0;
  std::string first_comment;
  int b_elements = 0;
};

// html_scan::ScanComment and CommentText against the vendored lexer itself,
// on every comment body up to six bytes long made of the bytes that matter
// to the comment states. Where ScanComment says the comment ends, the parser
// must have ended it there with the same text (the page is written back as
// `<!--` + CommentText + `-->` followed by the rest as it came in); where it
// says the comment never ends, the parser must read no element after it.
TEST(HtmlScanTest, CommentsEndExactlyWhereTheVendoredLexerEndsThem) {
  net_instaweb::HtmlKeywords::Init();
  net_instaweb::NullMessageHandler message_handler;
  net_instaweb::HtmlParse parser(&message_handler);
  CommentRecorder recorder;
  parser.AddFilter(&recorder);
  std::string written;
  net_instaweb::StringWriter writer(&written);
  net_instaweb::HtmlWriterFilter writer_filter(&parser);
  writer_filter.set_writer(&writer);
  parser.AddFilter(&writer_filter);

  constexpr std::string_view kBytes = "-!>a";
  constexpr std::string_view kTail = "<b>t</b>";
  std::string body;
  int checked = 0;
  auto check = [&] {
    std::string page = "<!--";
    page += body;
    page += kTail;
    SCOPED_TRACE(page);
    written.clear();
    ASSERT_TRUE(parser.StartParse("http://example.com/"));
    parser.ParseText(page);
    parser.FinishParse();
    const size_t end = html_scan::ScanComment(page, 0);
    if (end == std::string::npos) {
      EXPECT_EQ(recorder.b_elements, 0);
    } else {
      ASSERT_GE(recorder.comments, 1);
      EXPECT_EQ(recorder.first_comment, html_scan::CommentText(page, 0, end));
      std::string expected = "<!--";
      expected += html_scan::CommentText(page, 0, end);
      expected += "-->";
      expected += page.substr(end);
      EXPECT_EQ(written, expected);
      EXPECT_EQ(recorder.b_elements, 1);
    }
    ++checked;
  };
  // Every string over kBytes of length 0..6, in order of length.
  for (size_t len = 0; len <= 6; ++len) {
    body.assign(len, kBytes[0]);
    while (true) {
      check();
      size_t k = len;
      while (k > 0) {
        const size_t at = kBytes.find(body[k - 1]);
        if (at + 1 < kBytes.size()) {
          body[k - 1] = kBytes[at + 1];
          break;
        }
        body[k - 1] = kBytes[0];
        --k;
      }
      if (k == 0) break;
    }
  }
  EXPECT_EQ(checked, 5461);  // 4^0 + ... + 4^6
}

// ---------------------------------------------------------------------------
// The `<` rules for a critical block's text (CssOpensMarkupOutsideStrings,
// EscapeLessThanForStyleElement).
// ---------------------------------------------------------------------------

// True when `text` holds a `<` then an ASCII letter, `/`, `!` or `?`.
bool HoldsMarkupOpener(std::string_view text) {
  for (size_t i = 0; i + 1 < text.size(); ++i) {
    if (text[i] != '<') continue;
    const char next = text[i + 1];
    if ((next >= 'a' && next <= 'z') || (next >= 'A' && next <= 'Z') ||
        next == '/' || next == '!' || next == '?') {
      return true;
    }
  }
  return false;
}

TEST(CssLessThanTest, StringsAreEscaped) {
  EXPECT_EQ(EscapeLessThanForStyleElement("a{syntax:\"<color>\"}"),
            "a{syntax:\"\\3c color>\"}");
  EXPECT_EQ(EscapeLessThanForStyleElement("a{content:'<'}"),
            "a{content:'\\3c '}");
  EXPECT_EQ(EscapeLessThanForStyleElement("a{content:\"</style>\"}"),
            "a{content:\"\\3c /style>\"}");
  EXPECT_EQ(EscapeLessThanForStyleElement("a{content:\"<!-- <? <A\"}"),
            "a{content:\"\\3c !-- \\3c ? \\3c A\"}");
  // A `<` before a hex digit or a space: the escape's own space ends it, so
  // the next character stays itself.
  EXPECT_EQ(EscapeLessThanForStyleElement("a{content:\"<1 < \"}"),
            "a{content:\"\\3c 1 \\3c  \"}");
  for (std::string_view css :
       {"a{syntax:\"<color>\"}", "a{content:'<'}", "a{content:\"</style>\"}",
        "a{content:\"<!-- <? <A\"}"}) {
    EXPECT_FALSE(CssOpensMarkupOutsideStrings(css)) << css;
  }
}

TEST(CssLessThanTest, QuotesEscapesAndNewlinesEndStringsAsCssDoes) {
  // An escaped quote does not end the string; the other quote does not
  // either.
  EXPECT_EQ(EscapeLessThanForStyleElement("a{content:\"\\\"<b'<i\"}"),
            "a{content:\"\\\"\\3c b'\\3c i\"}");
  // A `\<` escape is the same character: it becomes the hex escape, without
  // the backslash doubled.
  EXPECT_EQ(EscapeLessThanForStyleElement("a{content:\"\\<b\"}"),
            "a{content:\"\\3c b\"}");
  // A backslash before a newline continues the string, CR LF included.
  EXPECT_EQ(EscapeLessThanForStyleElement("a{content:\"x\\\n<b\"}"),
            "a{content:\"x\\\n\\3c b\"}");
  EXPECT_EQ(EscapeLessThanForStyleElement("a{content:\"x\\\r\n<b\"}"),
            "a{content:\"x\\\r\n\\3c b\"}");
  // An unescaped newline ends the string (a bad string): a `<` after it is
  // outside.
  EXPECT_TRUE(CssOpensMarkupOutsideStrings("a{content:\"x\n<b>}"));
  // An escaped quote outside a string opens none.
  EXPECT_TRUE(CssOpensMarkupOutsideStrings(".x\\\"{a:1}<b"));
  // A string left open at the end of the text.
  EXPECT_EQ(EscapeLessThanForStyleElement("a{content:\"<b"),
            "a{content:\"\\3c b");
}

TEST(CssLessThanTest, CommentsAreEscapedAndHideQuotes) {
  EXPECT_EQ(EscapeLessThanForStyleElement("/* <div> \" */a{}"),
            "/* \\3c div> \" */a{}");
  // The quote in the comment opened no string: the `<b` after it is outside.
  EXPECT_TRUE(CssOpensMarkupOutsideStrings("/* \" */<b"));
  // `/*` inside a string opens no comment.
  EXPECT_TRUE(CssOpensMarkupOutsideStrings("a{content:\"/*\"}<b */"));
}

TEST(CssLessThanTest, OutsideStringsOnlyAMarkupOpenerIsReported) {
  for (std::string_view css : {
           "a{}<b",
           "a{}</style>",
           "a{}<!--",
           "a{}<?x",
           "a{--x:a<B}",
           "a{}</",
           "a{}<z",
       }) {
    EXPECT_TRUE(CssOpensMarkupOutsideStrings(css)) << css;
  }
  for (std::string_view css : {
           "@media (width<600px){a{color:red}}",
           "@media (width<=600px){a{color:red}}",
           "@media (400px < width < 900px){a{color:red}}",
           "a{--x:1<2}",
           "a{}<",
           "",
       }) {
    EXPECT_FALSE(CssOpensMarkupOutsideStrings(css)) << css;
    // A bare `<` is left as it is.
    EXPECT_EQ(EscapeLessThanForStyleElement(css), css);
  }
  // An escaped `<` in a selector is written as the hex escape too.
  EXPECT_EQ(EscapeLessThanForStyleElement(".\\<b\\>{a:1}"), ".\\3c b\\>{a:1}");
  EXPECT_FALSE(CssOpensMarkupOutsideStrings(".\\<b\\>{a:1}"));
}

TEST(CssLessThanTest, UnterminatedStringOrCommentAtTheEnd) {
  // A string or comment still open at the end of the text: its `<` is
  // escaped and is not an opener outside.
  EXPECT_EQ(EscapeLessThanForStyleElement("a{content:'<b"),
            "a{content:'\\3c b");
  EXPECT_FALSE(CssOpensMarkupOutsideStrings("a{content:'<b"));
  EXPECT_EQ(EscapeLessThanForStyleElement("a{}/* <b"), "a{}/* \\3c b");
  EXPECT_FALSE(CssOpensMarkupOutsideStrings("a{}/* <b"));
  // A backslash as the very last byte, in a string and outside one.
  EXPECT_EQ(EscapeLessThanForStyleElement("a{content:\"<\\"),
            "a{content:\"\\3c \\");
  EXPECT_EQ(EscapeLessThanForStyleElement("a{}\\"), "a{}\\");
  EXPECT_FALSE(CssOpensMarkupOutsideStrings("a{}\\"));
  // A `<` as the very last byte outside: a bare delimiter, kept.
  EXPECT_EQ(EscapeLessThanForStyleElement("a{}<"), "a{}<");
}

TEST(CssLessThanTest, TextAlreadyHoldingTheEscapeIsKept) {
  // An author's `\3c` (with or without its space) is copied as it is, and a
  // `<` next to it is escaped on its own.
  for (std::string_view css : {"a{content:\"\\3c b\"}", "a{content:\"\\3cb\"}",
                               ".\\3c b{a:1}", "a{content:'\\3C '}"}) {
    EXPECT_EQ(EscapeLessThanForStyleElement(css), css) << css;
    EXPECT_FALSE(CssOpensMarkupOutsideStrings(css)) << css;
  }
  EXPECT_EQ(EscapeLessThanForStyleElement("a{content:\"\\3c <\"}"),
            "a{content:\"\\3c \\3c \"}");
  // An escaped backslash before a `<`: the `<` is a character of its own.
  EXPECT_EQ(EscapeLessThanForStyleElement("a{content:\"\\\\<b\"}"),
            "a{content:\"\\\\\\3c b\"}");
}

// A Tailwind CSS v4 stylesheet, the shape of its output: the layer order,
// `@property` registrations with `syntax` strings, arbitrary-value utilities
// whose class names escape `<`, `content` strings holding `<`, a range media
// query and CSS nesting with `&`.
constexpr std::string_view kTailwindV4Css =
    "@layer theme,base,components,utilities;"
    "@layer theme{:root,:host{--color-red-500:oklch(63.7% .237 25.331)}}"
    "@layer utilities{"
    ".content-\\[\\'\\<\\'\\]{--tw-content:'<';content:var(--tw-content)}"
    ".before\\:content-\\[\\\"\\<\\/b\\>\\\"\\]{&::before{"
    "--tw-content:\"</b>\";content:var(--tw-content)}}"
    ".bg-linear-to-r{--tw-gradient-position:to right in oklab;"
    "background-image:linear-gradient(var(--tw-gradient-stops))}"
    ".hover\\:text-red-500{&:hover{@media (hover:hover){"
    "color:var(--color-red-500)}}}"
    ".md\\:flex{@media (width>=48rem){display:flex}}"
    ".max-md\\:hidden{@media (width<48rem){display:none}}}"
    "@property --tw-gradient-from{syntax:\"<color>\";inherits:false;"
    "initial-value:#0000}"
    "@property --tw-gradient-from-position{syntax:\"<length-percentage>\";"
    "inherits:false;initial-value:0%}"
    "@property --tw-content{syntax:\"*\";inherits:false;initial-value:\"\"}";

TEST(CssLessThanTest, TailwindV4SheetIsKeptAndMarkupFree) {
  EXPECT_FALSE(CssOpensMarkupOutsideStrings(kTailwindV4Css));
  const std::string escaped = EscapeLessThanForStyleElement(kTailwindV4Css);
  EXPECT_FALSE(HoldsMarkupOpener(escaped)) << escaped;
  EXPECT_NE(escaped.find("syntax:\"\\3c color>\""), std::string::npos)
      << escaped;
  EXPECT_NE(escaped.find("syntax:\"\\3c length-percentage>\""),
            std::string::npos)
      << escaped;
  EXPECT_NE(escaped.find("--tw-content:'\\3c '"), std::string::npos) << escaped;
  EXPECT_NE(escaped.find("--tw-content:\"\\3c /b>\""), std::string::npos)
      << escaped;
  EXPECT_NE(escaped.find(".content-\\[\\'\\3c \\'\\]"), std::string::npos)
      << escaped;
  EXPECT_NE(escaped.find("(width<48rem)"), std::string::npos) << escaped;
  // Escaping twice changes nothing: the escape holds no `<`.
  EXPECT_EQ(EscapeLessThanForStyleElement(escaped), escaped);

  // The string injector writes the same text into the block.
  auto result = InjectCriticalCss("<html><head></head><body></body></html>",
                                  kTailwindV4Css);
  EXPECT_TRUE(result.success);
  ASSERT_TRUE(result.injected);
  EXPECT_NE(result.html.find("<style data-pagespeed-critical>" + escaped +
                             "</style></head>"),
            std::string::npos)
      << result.html;
}

// `</style` inside a CSS string no longer refuses the block: it is written as
// `\3c /style`, which does not end the element. Outside a string it still
// does (StyleCloseXss* above).
TEST(HtmlCssInjectorTest, StyleEndTagInsideAStringIsEscaped) {
  auto result =
      InjectCriticalCss("<html><head></head><body></body></html>",
                        "a{content:\"</STYLE><script>x()</script>\"}");
  EXPECT_TRUE(result.success);
  ASSERT_TRUE(result.injected);
  EXPECT_NE(result.html.find("a{content:\"\\3c /STYLE>\\3c script>x()"
                             "\\3c /script>\"}</style></head>"),
            std::string::npos)
      << result.html;
}

}  // namespace
}  // namespace pagespeed
