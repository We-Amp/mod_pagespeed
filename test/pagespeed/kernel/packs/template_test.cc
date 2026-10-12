// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

#include "pagespeed/kernel/packs/template.h"

#include "gtest/gtest.h"

namespace pagespeed::packs {
namespace {

Template Parse(std::string_view text, bool page_values = false,
               int groups = 0) {
  TemplateParseOptions o;
  o.allow_page_values = page_values;
  o.capture_groups = groups;
  auto t = ParseTemplate(text, o);
  EXPECT_TRUE(t.ok()) << t.status();
  return t.ok() ? *std::move(t) : Template();
}

ExpandContext Ctx() {
  ExpandContext c;
  c.url = {"https", "www.example.com", "/shop/blue", "a=1&b=2"};
  c.captures = {"blue", "x"};
  c.title = "My Title";
  c.description = "My Description";
  c.canonical = "https://www.example.com/shop/blue";
  return c;
}

std::string Expand(std::string_view text,
                   EscapeContext e = EscapeContext::kNone,
                   bool page_values = false, int groups = 2) {
  auto r = ExpandTemplate(Parse(text, page_values, groups), Ctx(), e, 1 << 20);
  EXPECT_TRUE(r.ok()) << r.status();
  return r.ok() ? *r : "<error>";
}

TEST(TemplateParseTest, EveryPlaceholderExpands) {
  EXPECT_EQ(Expand("{scheme}"), "https");
  EXPECT_EQ(Expand("{host}"), "www.example.com");
  EXPECT_EQ(Expand("{path}"), "/shop/blue");
  EXPECT_EQ(Expand("{query}"), "?a=1&b=2");
  EXPECT_EQ(Expand("{url}"), "https://www.example.com/shop/blue?a=1&b=2");
  EXPECT_EQ(Expand("{1}-{2}"), "blue-x");
  EXPECT_EQ(
      Expand("{title}|{description}|{canonical}", EscapeContext::kNone, true),
      "My Title|My Description|https://www.example.com/shop/blue");
}

TEST(TemplateExpandTest, EmptyQueryHasNoQuestionMark) {
  ExpandContext c = Ctx();
  c.url.query.clear();
  auto r =
      ExpandTemplate(Parse("{url}|{query}|"), c, EscapeContext::kNone, 100);
  ASSERT_TRUE(r.ok());
  EXPECT_EQ(*r, "https://www.example.com/shop/blue||");
}

TEST(TemplateExpandTest, MissingCaptureExpandsEmpty) {
  // Template references groups 1..3; the context supplies fewer.
  ExpandContext c = Ctx();
  c.captures = {"only"};
  auto r = ExpandTemplate(Parse("[{1}][{2}][{3}]", false, 3), c,
                          EscapeContext::kNone, 100);
  ASSERT_TRUE(r.ok());
  EXPECT_EQ(*r, "[only][][]");
}

TEST(TemplateExpandTest, MissingPageValuesExpandEmpty) {
  ExpandContext c = Ctx();
  c.title.clear();
  c.description.clear();
  c.canonical.clear();
  auto r = ExpandTemplate(Parse("<{title}><{description}><{canonical}>", true),
                          c, EscapeContext::kNone, 100);
  ASSERT_TRUE(r.ok());
  EXPECT_EQ(*r, "<><><>");
}

TEST(TemplateExpandTest, ExpansionIsSinglePass) {
  // A value that looks like a placeholder is not expanded again.
  ExpandContext c = Ctx();
  c.url.path = "/{host}";
  auto r = ExpandTemplate(Parse("{path}"), c, EscapeContext::kNone, 100);
  ASSERT_TRUE(r.ok());
  EXPECT_EQ(*r, "/{host}");
}

TEST(TemplateParseTest, UnknownPlaceholderIsAnError) {
  for (const char* t : {"{foo}", "{Host}", "{0}", "{10}", "{path }x{bogus}",
                        "{title_}", "{_}"}) {
    auto r = ParseTemplate(t, {});
    EXPECT_FALSE(r.ok()) << t;
    if (!r.ok()) {
      EXPECT_NE(r.status().message().find("placeholder"), std::string::npos)
          << t;
    }
  }
}

TEST(TemplateParseTest, PageValuesRequireOptIn) {
  for (const char* t : {"{title}", "{description}", "{canonical}"}) {
    auto r = ParseTemplate(t, {});
    ASSERT_FALSE(r.ok()) << t;
    EXPECT_NE(r.status().message().find("only allowed in jsonld"),
              std::string::npos);
  }
}

TEST(TemplateParseTest, CaptureBeyondRegexGroupsIsAnError) {
  TemplateParseOptions o;
  o.capture_groups = 1;
  EXPECT_TRUE(ParseTemplate("{1}", o).ok());
  auto r = ParseTemplate("{2}", o);
  ASSERT_FALSE(r.ok());
  EXPECT_NE(r.status().message().find("capture group 2"), std::string::npos);
  EXPECT_FALSE(ParseTemplate("{1}", {}).ok());  // no path_regex at all
}

TEST(TemplateParseTest, BracesThatAreNotTokensAreLiteral) {
  EXPECT_EQ(Expand("{}"), "{}");
  EXPECT_EQ(Expand("a { b } c"), "a { b } c");
  EXPECT_EQ(Expand("{\"a\":{\"b\":1}}"), "{\"a\":{\"b\":1}}");
  EXPECT_EQ(Expand("{{host}}"), "{www.example.com}");
  EXPECT_EQ(Expand("{unterminated"), "{unterminated");
  EXPECT_EQ(Expand("}{"), "}{");
}

TEST(TemplateParseTest, EmptyTextParsesToNothing) { EXPECT_EQ(Expand(""), ""); }

TEST(TemplateEscapeTest, AttributeContextDoesNotEscape) {
  ExpandContext c = Ctx();
  c.url.path = "/a&b<c>\"d";
  auto r = ExpandTemplate(Parse("{path}"), c, EscapeContext::kNone, 100);
  ASSERT_TRUE(r.ok());
  EXPECT_EQ(*r, "/a&b<c>\"d");
}

TEST(TemplateEscapeTest, HtmlTextEscapesValuesAndLiterals) {
  ExpandContext c = Ctx();
  c.url.path = "/a&b<c>\"d'";
  auto r = ExpandTemplate(Parse("<{path}>&"), c, EscapeContext::kHtmlText, 100);
  ASSERT_TRUE(r.ok());
  // The whole output is escaped, literal template text included.
  EXPECT_EQ(*r, "&lt;/a&amp;b&lt;c&gt;\"d'&gt;&amp;");
}

TEST(TemplateEscapeTest, JsonStringEscapesValues) {
  ExpandContext c = Ctx();
  c.title = "He said \"hi\" \\ and\nleft\t\x01";
  EXPECT_EQ(Expand("{\"name\":\"{title}\"}", EscapeContext::kJsonString, true)
                .substr(0, 8),
            "{\"name\":");
  auto r = ExpandTemplate(Parse("\"{title}\"", true), c,
                          EscapeContext::kJsonString, 200);
  ASSERT_TRUE(r.ok());
  EXPECT_EQ(*r, "\"He said \\\"hi\\\" \\\\ and\\nleft\\t\\u0001\"");
}

TEST(TemplateEscapeTest, JsonLdNeverContainsRawLessThan) {
  ExpandContext c = Ctx();
  c.title = "</script><script>alert(1)</script>";
  auto r =
      ExpandTemplate(Parse("{\"headline\":\"{title}\",\"x\":\"</b>\"}", true),
                     c, EscapeContext::kJsonString, 1000);
  ASSERT_TRUE(r.ok());
  EXPECT_EQ(r->find('<'), std::string::npos);
  EXPECT_EQ(r->find("</script"), std::string::npos);
  EXPECT_NE(r->find("\\u003c/script>"), std::string::npos);
  EXPECT_NE(r->find("\"x\":\"\\u003c/b>\""), std::string::npos);
}

TEST(TemplateSizeTest, ExactlyAtTheCapIsAllowed) {
  auto r = ExpandTemplate(Parse("abcde"), Ctx(), EscapeContext::kNone, 5);
  ASSERT_TRUE(r.ok());
  EXPECT_EQ(*r, "abcde");
}

TEST(TemplateSizeTest, OverTheCapIsRejected) {
  auto r = ExpandTemplate(Parse("abcdef"), Ctx(), EscapeContext::kNone, 5);
  ASSERT_FALSE(r.ok());
  EXPECT_EQ(r.status().code(), absl::StatusCode::kResourceExhausted);
}

TEST(TemplateSizeTest, CapAppliesAfterValueExpansion) {
  ExpandContext c = Ctx();
  c.url.path = std::string(100, 'x');
  auto r = ExpandTemplate(Parse("{path}{path}"), c, EscapeContext::kNone, 199);
  ASSERT_FALSE(r.ok());
  EXPECT_EQ(r.status().code(), absl::StatusCode::kResourceExhausted);
  EXPECT_TRUE(
      ExpandTemplate(Parse("{path}{path}"), c, EscapeContext::kNone, 200).ok());
}

TEST(TemplateSizeTest, CapAppliesAfterEscaping) {
  ExpandContext c = Ctx();
  c.title = "<<<<";  // 4 bytes raw, 6 each as \u003c once emitted
  Template t = Parse("{title}", true);
  // JSON string escaping leaves '<' alone, the final pass turns it to 6 bytes.
  EXPECT_FALSE(ExpandTemplate(t, c, EscapeContext::kJsonString, 23).ok());
  EXPECT_TRUE(ExpandTemplate(t, c, EscapeContext::kJsonString, 24).ok());
  // HTML text: '<' becomes "&lt;" (4 bytes).
  EXPECT_FALSE(ExpandTemplate(t, c, EscapeContext::kHtmlText, 15).ok());
  EXPECT_TRUE(ExpandTemplate(t, c, EscapeContext::kHtmlText, 16).ok());
}

TEST(TemplateEscapeTest, HtmlTextEscapesLiteralTemplateTextToo) {
  auto r = ExpandTemplate(Parse("Tom & Jerry </title>"), Ctx(),
                          EscapeContext::kHtmlText, 100);
  ASSERT_TRUE(r.ok());
  EXPECT_EQ(*r, "Tom &amp; Jerry &lt;/title&gt;");
}

TEST(TemplateEscapeTest, EscapeForHtmlTextForTableValues) {
  EXPECT_EQ(EscapeForHtmlText("</title><script>alert(1)</script>"),
            "&lt;/title&gt;&lt;script&gt;alert(1)&lt;/script&gt;");
  EXPECT_EQ(EscapeForHtmlText("a & b"), "a &amp; b");
  EXPECT_EQ(EscapeForHtmlText("&amp;"), "&amp;amp;");
  EXPECT_EQ(EscapeForHtmlText(""), "");
}

TEST(TemplateSizeTest, HugeValueIsRejectedBeforeItIsEscapedOrCopied) {
  ExpandContext c = Ctx();
  c.title = std::string(5 * 1024 * 1024, '<');  // would be 20 MiB escaped
  auto r = ExpandTemplate(Parse("{title}", true), c, EscapeContext::kHtmlText,
                          16 * 1024);
  ASSERT_FALSE(r.ok());
  EXPECT_EQ(r.status().code(), absl::StatusCode::kResourceExhausted);
  r = ExpandTemplate(Parse("{title}", true), c, EscapeContext::kJsonString,
                     16 * 1024);
  ASSERT_FALSE(r.ok());
  EXPECT_EQ(r.status().code(), absl::StatusCode::kResourceExhausted);
}

TEST(TemplateSizeTest, CapIsCheckedPieceByPiece) {
  ExpandContext c = Ctx();
  c.url.path = std::string(10, 'x');
  auto r = ExpandTemplate(Parse("{path}-{path}-{path}"), c,
                          EscapeContext::kNone, 32);
  EXPECT_TRUE(r.ok());
  r = ExpandTemplate(Parse("{path}-{path}-{path}"), c, EscapeContext::kNone,
                     31);
  EXPECT_FALSE(r.ok());
}

TEST(TemplateExpandTest, UrlAndQueryExpandPieceWise) {
  ExpandContext c = Ctx();
  auto r =
      ExpandTemplate(Parse("{url}|{query}"), c, EscapeContext::kHtmlText, 200);
  ASSERT_TRUE(r.ok());
  EXPECT_EQ(*r, "https://www.example.com/shop/blue?a=1&amp;b=2|?a=1&amp;b=2");
}

}  // namespace
}  // namespace pagespeed::packs
