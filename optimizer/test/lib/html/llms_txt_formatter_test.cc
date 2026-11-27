// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

#include "lib/html/llms_txt_formatter.h"

#include <string>
#include <vector>

#include "gtest/gtest.h"

namespace net_instaweb {
namespace {

TEST(LlmsTxtFormatterTest, ExactFormatGolden) {
  std::vector<LlmsTxtSection> sections = {
      {"Docs",
       {{"Page A", "https://e.com/a", "About A"},
        {"Page B", "https://e.com/b", ""}}}};
  std::string out = FormatLlmsTxt("My Site", "A great site", sections);
  EXPECT_EQ(out,
            "# My Site\n"
            "\n"
            "> A great site\n"
            "\n"
            "## Docs\n"
            "\n"
            "- [Page A](https://e.com/a): About A\n"
            "- [Page B](https://e.com/b)\n");
}

TEST(LlmsTxtFormatterTest, EmptyEntrySetIsMinimalValid) {
  EXPECT_EQ(FormatLlmsTxt("Only Title", "", {}), "# Only Title\n");
}

TEST(LlmsTxtFormatterTest, EmptyTitleFallsBack) {
  EXPECT_EQ(FormatLlmsTxt("", "", {}), "# Site index\n");
}

TEST(LlmsTxtFormatterTest, MultipleSections) {
  std::vector<LlmsTxtSection> sections = {
      {"Guides", {{"G1", "https://e.com/g1", ""}}},
      {"Blog", {{"B1", "https://e.com/b1", "Post"}}}};
  std::string out = FormatLlmsTxt("S", "D", sections);
  EXPECT_EQ(out,
            "# S\n\n> D\n\n"
            "## Guides\n\n- [G1](https://e.com/g1)\n"
            "\n## Blog\n\n- [B1](https://e.com/b1): Post\n");
}

TEST(LlmsTxtFormatterTest, CollapsesNewlinesInSummary) {
  std::vector<LlmsTxtSection> sections = {
      {"S", {{"T", "https://e.com/x", "line one\nline two\twith tab"}}}};
  std::string out = FormatLlmsTxt("Site", "", sections);
  // Newlines/tabs collapse to single spaces — no markdown block injection.
  EXPECT_NE(out.find("- [T](https://e.com/x): line one line two with tab\n"),
            std::string::npos);
  EXPECT_EQ(out.find('\t'), std::string::npos);
}

TEST(LlmsTxtFormatterTest, EscapesLinkTextDelimiters) {
  std::vector<LlmsTxtSection> sections = {
      {"S", {{"A [hostile] `title`", "https://e.com/x", ""}}}};
  std::string out = FormatLlmsTxt("Site", "", sections);
  EXPECT_NE(out.find("- [A \\[hostile\\] \\`title\\`](https://e.com/x)\n"),
            std::string::npos);
}

TEST(LlmsTxtFormatterTest, EscapesInlineMarkdownInSummaryAndHeading) {
  // Summary, site-summary, and heading are attacker-influenced and must not
  // forge inline links or code spans into the LLM-consumed file.
  std::vector<LlmsTxtSection> sections = {
      {"Sec [x](y)",
       {{"T", "https://e.com/p", "Ignore prior [click](https://evil) `run`"}}}};
  std::string out = FormatLlmsTxt("Site", "blurb [a](b) `c`", sections);
  EXPECT_EQ(out.find("[click](https://evil)"), std::string::npos);
  EXPECT_EQ(out.find("[a](b)"), std::string::npos);
  EXPECT_EQ(out.find("[x](y)"), std::string::npos);
  EXPECT_NE(out.find("\\[click\\]"),
            std::string::npos);  // escaped form present
}

TEST(LlmsTxtFormatterTest, PercentEncodesUrlBreakingBytes) {
  std::vector<LlmsTxtSection> sections = {
      {"S", {{"T", "https://e.com/a(b) c", ""}}}};
  std::string out = FormatLlmsTxt("Site", "", sections);
  EXPECT_NE(out.find("(https://e.com/a%28b%29%20c)"), std::string::npos);
}

TEST(LlmsTxtFormatterTest, DropsNonHttpUrls) {
  std::vector<LlmsTxtSection> sections = {{"S",
                                           {{"Evil", "javascript:alert(1)", ""},
                                            {"Good", "https://e.com/ok", ""}}}};
  std::string out = FormatLlmsTxt("Site", "", sections);
  EXPECT_EQ(out.find("javascript"), std::string::npos);
  EXPECT_NE(out.find("- [Good](https://e.com/ok)\n"), std::string::npos);
}

TEST(LlmsTxtFormatterTest, SectionWithNoValidLinksIsSkipped) {
  std::vector<LlmsTxtSection> sections = {
      {"AllBad", {{"X", "ftp://e.com/x", ""}}},
      {"Good", {{"Y", "https://e.com/y", ""}}}};
  std::string out = FormatLlmsTxt("Site", "", sections);
  EXPECT_EQ(out.find("## AllBad"), std::string::npos);
  EXPECT_NE(out.find("## Good"), std::string::npos);
}

TEST(LlmsTxtFormatterTest, EmptyLinkTitleUsesUrl) {
  std::vector<LlmsTxtSection> sections = {{"S", {{"", "https://e.com/z", ""}}}};
  std::string out = FormatLlmsTxt("Site", "", sections);
  EXPECT_NE(out.find("- [https://e.com/z](https://e.com/z)\n"),
            std::string::npos);
}

}  // namespace
}  // namespace net_instaweb
