// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

// PageSpeed 2.0 - Unused CSS Remover Tests

#include "src/browser/unused_css_remover.h"

#include <cstddef>
#include <string>
#include <utility>
#include <vector>

#include "gtest/gtest.h"

namespace pagespeed {
namespace {

// --- Remove() tests ---

TEST(UnusedCssRemoverTest, EmptyInput) {
  auto result = UnusedCssRemover::Remove("", {});
  EXPECT_EQ(result.cleaned_css, "");
  EXPECT_EQ(result.original_bytes, 0u);
  EXPECT_EQ(result.cleaned_bytes, 0u);
  EXPECT_EQ(result.rules_removed, 0u);
  EXPECT_FLOAT_EQ(result.removal_ratio, 0.0f);
}

TEST(UnusedCssRemoverTest, AllUsed) {
  std::string css = "body { color: red; }\nh1 { font-size: 2em; }";
  // The entire CSS is covered.
  std::vector<std::pair<size_t, size_t>> used = {{0, css.size()}};

  auto result = UnusedCssRemover::Remove(css, used);
  EXPECT_EQ(result.rules_removed, 0u);
  // Both rules should be present.
  EXPECT_NE(result.cleaned_css.find("body"), std::string::npos);
  EXPECT_NE(result.cleaned_css.find("h1"), std::string::npos);
  EXPECT_FLOAT_EQ(result.removal_ratio, 0.0f);
}

TEST(UnusedCssRemoverTest, AllUnused) {
  std::string css = "body { color: red; }\nh1 { font-size: 2em; }";
  // No ranges used.
  std::vector<std::pair<size_t, size_t>> used = {};

  auto result = UnusedCssRemover::Remove(css, used);
  EXPECT_EQ(result.rules_removed, 2u);
  EXPECT_EQ(result.cleaned_css, "");
  EXPECT_GT(result.removal_ratio, 0.0f);
}

TEST(UnusedCssRemoverTest, BasicRemoval) {
  // Three rules: body (used), h1 (unused), p (used).
  std::string css =
      "body { color: red; }\n"
      "h1 { font-size: 2em; }\n"
      "p { margin: 0; }";

  // Mark "body" rule and "p" rule as used.
  size_t body_start = 0;
  size_t body_end = css.find('\n');
  size_t p_start = css.rfind("p {");
  size_t p_end = css.size();

  std::vector<std::pair<size_t, size_t>> used = {
      {body_start, body_end},
      {p_start, p_end},
  };

  auto result = UnusedCssRemover::Remove(css, used);
  EXPECT_EQ(result.rules_removed, 1u);
  EXPECT_NE(result.cleaned_css.find("body"), std::string::npos);
  EXPECT_NE(result.cleaned_css.find("p {"), std::string::npos);
  EXPECT_EQ(result.cleaned_css.find("h1"), std::string::npos);
}

TEST(UnusedCssRemoverTest, PreserveFontFace) {
  std::string css =
      "@font-face { font-family: 'Custom'; src: url(font.woff2); }\n"
      ".unused { color: red; }";

  // No ranges used -- @font-face is still preserved.
  auto result = UnusedCssRemover::Remove(css, {});
  EXPECT_NE(result.cleaned_css.find("@font-face"), std::string::npos);
  EXPECT_EQ(result.cleaned_css.find(".unused"), std::string::npos);
  EXPECT_EQ(result.rules_removed, 1u);
}

TEST(UnusedCssRemoverTest, PreserveKeyframes) {
  std::string css =
      "@keyframes fadeIn { from { opacity: 0; } to { opacity: 1; } }\n"
      ".unused { color: red; }";

  auto result = UnusedCssRemover::Remove(css, {});
  EXPECT_NE(result.cleaned_css.find("@keyframes"), std::string::npos);
  EXPECT_EQ(result.cleaned_css.find(".unused"), std::string::npos);
}

TEST(UnusedCssRemoverTest, PreserveWebkitKeyframes) {
  std::string css =
      "@-webkit-keyframes spin { from { transform: rotate(0); } "
      "to { transform: rotate(360deg); } }\n"
      ".unused { display: none; }";

  auto result = UnusedCssRemover::Remove(css, {});
  EXPECT_NE(result.cleaned_css.find("@-webkit-keyframes"), std::string::npos);
}

TEST(UnusedCssRemoverTest, PreserveCharset) {
  std::string css =
      "@charset \"UTF-8\";\n"
      ".unused { color: red; }";

  auto result = UnusedCssRemover::Remove(css, {});
  EXPECT_NE(result.cleaned_css.find("@charset"), std::string::npos);
  EXPECT_EQ(result.cleaned_css.find(".unused"), std::string::npos);
}

TEST(UnusedCssRemoverTest, PreserveImport) {
  std::string css =
      "@import url('reset.css');\n"
      ".unused { color: red; }";

  auto result = UnusedCssRemover::Remove(css, {});
  EXPECT_NE(result.cleaned_css.find("@import"), std::string::npos);
  EXPECT_EQ(result.cleaned_css.find(".unused"), std::string::npos);
}

TEST(UnusedCssRemoverTest, OverlappingRanges) {
  std::string css =
      "a { color: blue; }\n"
      "b { color: red; }\n"
      "c { color: green; }";

  // Overlapping ranges covering "a" and "b" rules.
  size_t b_end = css.find("c {");
  std::vector<std::pair<size_t, size_t>> used = {
      {0, 10},
      {5, b_end},
  };

  auto result = UnusedCssRemover::Remove(css, used);
  EXPECT_NE(result.cleaned_css.find("a {"), std::string::npos);
  EXPECT_NE(result.cleaned_css.find("b {"), std::string::npos);
  EXPECT_EQ(result.cleaned_css.find("c {"), std::string::npos);
  EXPECT_EQ(result.rules_removed, 1u);
}

TEST(UnusedCssRemoverTest, PartialRangeOverlap) {
  // A range that partially overlaps a rule still keeps that rule.
  std::string css = "body { color: red; }";
  // Only a few bytes overlap with the rule.
  std::vector<std::pair<size_t, size_t>> used = {{3, 8}};

  auto result = UnusedCssRemover::Remove(css, used);
  EXPECT_EQ(result.rules_removed, 0u);
  EXPECT_NE(result.cleaned_css.find("body"), std::string::npos);
}

TEST(UnusedCssRemoverTest, RemovalRatio) {
  std::string css = "a{x:1}b{x:2}c{x:3}d{x:4}";
  // Only first rule used.
  std::vector<std::pair<size_t, size_t>> used = {{0, 6}};

  auto result = UnusedCssRemover::Remove(css, used);
  EXPECT_EQ(result.rules_removed, 3u);
  EXPECT_EQ(result.original_bytes, css.size());
  EXPECT_GT(result.removal_ratio, 0.0f);
  EXPECT_LT(result.removal_ratio, 1.0f);
}

TEST(UnusedCssRemoverTest, NestedBraces) {
  // @media rule with nested braces.
  std::string css =
      "@media (max-width: 768px) {\n"
      "  .mobile { display: block; }\n"
      "}\n"
      ".unused { color: red; }";

  // Mark the @media rule as used.
  size_t media_end = css.find("}\n.unused");
  std::vector<std::pair<size_t, size_t>> used = {{0, media_end + 1}};

  auto result = UnusedCssRemover::Remove(css, used);
  EXPECT_NE(result.cleaned_css.find("@media"), std::string::npos);
  EXPECT_NE(result.cleaned_css.find(".mobile"), std::string::npos);
  EXPECT_EQ(result.cleaned_css.find(".unused"), std::string::npos);
}

TEST(UnusedCssRemoverTest, StringsWithBraces) {
  // CSS with braces inside string literals (should not confuse parser).
  std::string css =
      ".icon::before { content: '{'; }\n"
      ".unused { color: red; }";

  std::vector<std::pair<size_t, size_t>> used = {{0, css.find('\n')}};

  auto result = UnusedCssRemover::Remove(css, used);
  EXPECT_NE(result.cleaned_css.find(".icon"), std::string::npos);
  EXPECT_EQ(result.cleaned_css.find(".unused"), std::string::npos);
}

TEST(UnusedCssRemoverTest, CommentsPreserved) {
  std::string css =
      "/* License header */\n"
      "body { color: red; }";

  std::vector<std::pair<size_t, size_t>> used = {
      {css.find("body"), css.size()}};

  auto result = UnusedCssRemover::Remove(css, used);
  EXPECT_NE(result.cleaned_css.find("License header"), std::string::npos);
  EXPECT_NE(result.cleaned_css.find("body"), std::string::npos);
}

TEST(UnusedCssRemoverTest, WhitespaceOnly) {
  std::string css = "   \n  \t  \n  ";
  auto result = UnusedCssRemover::Remove(css, {});
  EXPECT_EQ(result.cleaned_css, "");
  EXPECT_EQ(result.rules_removed, 0u);
}

TEST(UnusedCssRemoverTest, MultipleFontFacesAndKeyframes) {
  std::string css =
      "@font-face { font-family: 'A'; src: url(a.woff2); }\n"
      "@font-face { font-family: 'B'; src: url(b.woff2); }\n"
      "@keyframes slide { from { left: 0; } to { left: 100px; } }\n"
      ".used { color: red; }\n"
      ".unused { color: blue; }";

  // Only .used is covered.
  size_t used_start = css.find(".used");
  size_t used_end = css.find('\n', used_start);
  std::vector<std::pair<size_t, size_t>> used = {{used_start, used_end}};

  auto result = UnusedCssRemover::Remove(css, used);
  // All @font-face and @keyframes preserved.
  EXPECT_NE(result.cleaned_css.find("font-family: 'A'"), std::string::npos);
  EXPECT_NE(result.cleaned_css.find("font-family: 'B'"), std::string::npos);
  EXPECT_NE(result.cleaned_css.find("@keyframes"), std::string::npos);
  EXPECT_NE(result.cleaned_css.find(".used"), std::string::npos);
  EXPECT_EQ(result.cleaned_css.find(".unused"), std::string::npos);
}

TEST(UnusedCssRemoverTest, BracesInsideComments) {
  // Braces inside comments must not corrupt depth tracking.
  std::string css =
      ".used { color: red; } /* { unused } */ .real { margin: 0; }";

  // Mark .used and .real as used.
  size_t used_end = css.find('}') + 1;
  size_t real_start = css.find(".real");
  std::vector<std::pair<size_t, size_t>> used = {
      {0, used_end},
      {real_start, css.size()},
  };

  auto result = UnusedCssRemover::Remove(css, used);
  EXPECT_NE(result.cleaned_css.find(".used"), std::string::npos);
  EXPECT_NE(result.cleaned_css.find(".real"), std::string::npos);
  // .real rule must have its actual content, not be mangled.
  EXPECT_NE(result.cleaned_css.find("margin: 0"), std::string::npos);
}

TEST(UnusedCssRemoverTest, CommentWithClosingBraceAfterRule) {
  // A comment containing '}' after a rule must not end the rule early.
  std::string css =
      ".box { display: flex; /* } */ padding: 1px; }\n"
      ".unused { color: red; }";

  size_t box_end = css.find('\n');
  std::vector<std::pair<size_t, size_t>> used = {{0, box_end}};

  auto result = UnusedCssRemover::Remove(css, used);
  EXPECT_NE(result.cleaned_css.find(".box"), std::string::npos);
  // The full .box rule must be preserved including 'padding'.
  EXPECT_NE(result.cleaned_css.find("padding: 1px"), std::string::npos);
  EXPECT_EQ(result.cleaned_css.find(".unused"), std::string::npos);
  EXPECT_EQ(result.rules_removed, 1u);
}

TEST(UnusedCssRemoverTest, MediaRuleWithCommentBrace) {
  // @media block with a comment containing '{' inside.
  std::string css =
      "@media screen /* { */ {\n"
      "  .inner { color: blue; }\n"
      "}\n"
      ".unused { color: red; }";

  size_t media_end = css.find("}\n.unused") + 1;
  std::vector<std::pair<size_t, size_t>> used = {{0, media_end}};

  auto result = UnusedCssRemover::Remove(css, used);
  EXPECT_NE(result.cleaned_css.find("@media"), std::string::npos);
  EXPECT_NE(result.cleaned_css.find(".inner"), std::string::npos);
  EXPECT_EQ(result.cleaned_css.find(".unused"), std::string::npos);
  EXPECT_EQ(result.rules_removed, 1u);
}

// --- MergeViewportCoverage() tests ---

TEST(UnusedCssRemoverTest, MergeEmptyViewports) {
  auto result = UnusedCssRemover::MergeViewportCoverage({});
  EXPECT_TRUE(result.empty());
}

TEST(UnusedCssRemoverTest, MergeSingleViewport) {
  std::vector<std::vector<std::pair<size_t, size_t>>> viewports = {
      {{0, 10}, {20, 30}},
  };
  auto result = UnusedCssRemover::MergeViewportCoverage(viewports);
  ASSERT_EQ(result.size(), 2u);
  EXPECT_EQ(result[0].first, 0u);
  EXPECT_EQ(result[0].second, 10u);
  EXPECT_EQ(result[1].first, 20u);
  EXPECT_EQ(result[1].second, 30u);
}

TEST(UnusedCssRemoverTest, MergeMultipleViewports) {
  // Mobile uses bytes 0-10, 50-60
  // Tablet uses bytes 5-15, 40-55
  // Desktop uses bytes 20-30, 55-70
  std::vector<std::vector<std::pair<size_t, size_t>>> viewports = {
      {{0, 10}, {50, 60}},
      {{5, 15}, {40, 55}},
      {{20, 30}, {55, 70}},
  };

  auto result = UnusedCssRemover::MergeViewportCoverage(viewports);
  // Expected merged: [0,15], [20,30], [40,70]
  ASSERT_EQ(result.size(), 3u);
  EXPECT_EQ(result[0].first, 0u);
  EXPECT_EQ(result[0].second, 15u);
  EXPECT_EQ(result[1].first, 20u);
  EXPECT_EQ(result[1].second, 30u);
  EXPECT_EQ(result[2].first, 40u);
  EXPECT_EQ(result[2].second, 70u);
}

TEST(UnusedCssRemoverTest, MergeOverlappingRanges) {
  std::vector<std::vector<std::pair<size_t, size_t>>> viewports = {
      {{0, 20}},
      {{10, 30}},
      {{25, 50}},
  };

  auto result = UnusedCssRemover::MergeViewportCoverage(viewports);
  // All overlap -> single range [0, 50].
  ASSERT_EQ(result.size(), 1u);
  EXPECT_EQ(result[0].first, 0u);
  EXPECT_EQ(result[0].second, 50u);
}

TEST(UnusedCssRemoverTest, MergeAdjacentRanges) {
  std::vector<std::vector<std::pair<size_t, size_t>>> viewports = {
      {{0, 10}},
      {{10, 20}},
      {{20, 30}},
  };

  auto result = UnusedCssRemover::MergeViewportCoverage(viewports);
  // Adjacent ranges merge into [0, 30].
  ASSERT_EQ(result.size(), 1u);
  EXPECT_EQ(result[0].first, 0u);
  EXPECT_EQ(result[0].second, 30u);
}

TEST(UnusedCssRemoverTest, MergeWithEmptyViewport) {
  std::vector<std::vector<std::pair<size_t, size_t>>> viewports = {
      {{0, 10}},
      {},
      {{20, 30}},
  };

  auto result = UnusedCssRemover::MergeViewportCoverage(viewports);
  ASSERT_EQ(result.size(), 2u);
  EXPECT_EQ(result[0].first, 0u);
  EXPECT_EQ(result[0].second, 10u);
  EXPECT_EQ(result[1].first, 20u);
  EXPECT_EQ(result[1].second, 30u);
}

TEST(UnusedCssRemoverTest, MergeDuplicateRanges) {
  std::vector<std::vector<std::pair<size_t, size_t>>> viewports = {
      {{0, 10}, {0, 10}},
      {{0, 10}},
  };

  auto result = UnusedCssRemover::MergeViewportCoverage(viewports);
  ASSERT_EQ(result.size(), 1u);
  EXPECT_EQ(result[0].first, 0u);
  EXPECT_EQ(result[0].second, 10u);
}

// --- Edge case tests for uncovered code paths ---

TEST(UnusedCssRemoverTest, EscapedQuotesInStrings) {
  // CSS with escaped quotes inside string literals.
  // Targets FindClosingBrace's escape handling (lines 75-77).
  std::string css =
      ".icon::before { content: \"\\\"quoted\\\"\"; }\n"
      ".unused { color: red; }";

  size_t icon_end = css.find('\n');
  std::vector<std::pair<size_t, size_t>> used = {{0, icon_end}};

  auto result = UnusedCssRemover::Remove(css, used);
  EXPECT_NE(result.cleaned_css.find(".icon"), std::string::npos);
  EXPECT_EQ(result.cleaned_css.find(".unused"), std::string::npos);
  EXPECT_EQ(result.rules_removed, 1u);
}

TEST(UnusedCssRemoverTest, EscapedBackslashInStrings) {
  // CSS with escaped backslash followed by a closing quote.
  // Ensures the backslash escape path in FindClosingBrace works.
  std::string css =
      ".path::after { content: \"C:\\\\path\\\\\"; }\n"
      ".unused { color: red; }";

  size_t path_end = css.find('\n');
  std::vector<std::pair<size_t, size_t>> used = {{0, path_end}};

  auto result = UnusedCssRemover::Remove(css, used);
  EXPECT_NE(result.cleaned_css.find(".path"), std::string::npos);
  EXPECT_EQ(result.cleaned_css.find(".unused"), std::string::npos);
}

TEST(UnusedCssRemoverTest, SingleQuoteStringsWithEscapes) {
  // Single-quoted string with escaped single quote inside.
  std::string css =
      ".sq::before { content: '\\'hello\\''; }\n"
      ".unused { color: red; }";

  size_t end = css.find('\n');
  std::vector<std::pair<size_t, size_t>> used = {{0, end}};

  auto result = UnusedCssRemover::Remove(css, used);
  EXPECT_NE(result.cleaned_css.find(".sq"), std::string::npos);
  EXPECT_EQ(result.cleaned_css.find(".unused"), std::string::npos);
}

TEST(UnusedCssRemoverTest, UnclosedComment) {
  // Unclosed CSS comment -- FindClosingBrace returns css.size().
  // Targets line 88.
  std::string css = ".box { color: red; /* unclosed comment";

  std::vector<std::pair<size_t, size_t>> used = {{0, css.size()}};
  auto result = UnusedCssRemover::Remove(css, used);
  // The entire string is treated as one rule (even though malformed).
  EXPECT_NE(result.cleaned_css.find(".box"), std::string::npos);
}

TEST(UnusedCssRemoverTest, UnbalancedBraces) {
  // Unbalanced braces -- FindClosingBrace returns css.size().
  // Targets line 106.
  std::string css = ".open { color: red; .inner { margin: 0; }";

  std::vector<std::pair<size_t, size_t>> used = {{0, css.size()}};
  auto result = UnusedCssRemover::Remove(css, used);
  // Treated as one big rule since the outer brace never closes.
  EXPECT_NE(result.cleaned_css.find(".open"), std::string::npos);
}

TEST(UnusedCssRemoverTest, MalformedAtRuleWithoutBrace) {
  // Malformed at-rule (e.g., @media without opening brace).
  // FindRuleEnd: at-rule without block found, falls through to semicolon.
  // Targets lines 154-158.
  std::string css =
      "@media screen and (min-width: 768px);\n"
      ".used { color: red; }";

  size_t used_start = css.find(".used");
  std::vector<std::pair<size_t, size_t>> used = {{used_start, css.size()}};

  auto result = UnusedCssRemover::Remove(css, used);
  EXPECT_NE(result.cleaned_css.find(".used"), std::string::npos);
}

TEST(UnusedCssRemoverTest, MalformedAtRuleNoSemicolonNoBrace) {
  // At-rule that has no semicolon and no brace.
  // Targets the code path where FindOpenBrace returns npos and
  // css.find(';') also returns npos -- returns css.size().
  std::string css = "@media screen and (min-width: 768px)";

  std::vector<std::pair<size_t, size_t>> used = {{0, css.size()}};
  auto result = UnusedCssRemover::Remove(css, used);
  EXPECT_NE(result.cleaned_css.find("@media"), std::string::npos);
}

TEST(UnusedCssRemoverTest, RegularRuleWithoutBlock) {
  // Regular rule text that has no braces (trailing text).
  // Targets FindRuleEnd line 167 (no block found).
  std::string css = "body";

  std::vector<std::pair<size_t, size_t>> used = {{0, css.size()}};
  auto result = UnusedCssRemover::Remove(css, used);
  // Treated as one rule, kept because used range overlaps.
  EXPECT_NE(result.cleaned_css.find("body"), std::string::npos);
}

TEST(UnusedCssRemoverTest, CommentOnlyInputNotRule) {
  // SkipComment at the beginning of a non-comment position.
  // Targets line 179 (return pos unchanged).
  std::string css = ".used { color: red; }";

  std::vector<std::pair<size_t, size_t>> used = {{0, css.size()}};
  auto result = UnusedCssRemover::Remove(css, used);
  EXPECT_EQ(result.rules_removed, 0u);
}

TEST(UnusedCssRemoverTest, FindOpenBraceSkipsComment) {
  // Targets FindOpenBrace's comment-skipping path (line 127-129).
  // A comment between the selector and the opening brace.
  std::string css =
      ".box /* comment */ { color: red; }\n"
      ".unused { color: blue; }";

  size_t box_end = css.find('\n');
  std::vector<std::pair<size_t, size_t>> used = {{0, box_end}};

  auto result = UnusedCssRemover::Remove(css, used);
  EXPECT_NE(result.cleaned_css.find(".box"), std::string::npos);
  EXPECT_EQ(result.cleaned_css.find(".unused"), std::string::npos);
}

TEST(UnusedCssRemoverTest, FindOpenBraceUnclosedComment) {
  // Comment that never closes -- FindOpenBrace returns npos.
  // Targets line 128 (comment end == npos).
  // When used as a regular rule, FindRuleEnd calls FindOpenBrace,
  // gets npos, and returns css.size().
  std::string css = ".box /* unclosed comment { color: red; }";

  std::vector<std::pair<size_t, size_t>> used = {{0, css.size()}};
  auto result = UnusedCssRemover::Remove(css, used);
  // Entire string treated as one rule.
  EXPECT_NE(result.cleaned_css.find(".box"), std::string::npos);
}

TEST(UnusedCssRemoverTest, PreserveMozKeyframes) {
  std::string css =
      "@-moz-keyframes spin { from { transform: rotate(0); } "
      "to { transform: rotate(360deg); } }\n"
      ".unused { display: none; }";

  auto result = UnusedCssRemover::Remove(css, {});
  EXPECT_NE(result.cleaned_css.find("@-moz-keyframes"), std::string::npos);
  EXPECT_EQ(result.cleaned_css.find(".unused"), std::string::npos);
}

TEST(UnusedCssRemoverTest, MediaQueryWithNestedRules) {
  // @media block with multiple nested rules.
  std::string css =
      "@media (max-width: 480px) {\n"
      "  .mobile-only { display: block; }\n"
      "  .mobile-nav { position: fixed; }\n"
      "}\n"
      "@media (min-width: 1024px) {\n"
      "  .desktop-only { display: block; }\n"
      "}\n"
      ".unused { color: red; }";

  // Mark both @media blocks as used.
  size_t first_media_end = css.find("}\n@media (min-width") + 1;
  size_t second_media_end = css.find("}\n.unused") + 1;
  std::vector<std::pair<size_t, size_t>> used = {
      {0, first_media_end},
      {css.find("@media (min-width"), second_media_end},
  };

  auto result = UnusedCssRemover::Remove(css, used);
  EXPECT_NE(result.cleaned_css.find(".mobile-only"), std::string::npos);
  EXPECT_NE(result.cleaned_css.find(".desktop-only"), std::string::npos);
  EXPECT_EQ(result.cleaned_css.find(".unused"), std::string::npos);
}

TEST(UnusedCssRemoverTest, SupportsAtRule) {
  // @supports rule should be treated as an at-rule with a block.
  std::string css =
      "@supports (display: grid) {\n"
      "  .grid-layout { display: grid; }\n"
      "}\n"
      ".unused { color: red; }";

  size_t supports_end = css.find("}\n.unused") + 1;
  std::vector<std::pair<size_t, size_t>> used = {{0, supports_end}};

  auto result = UnusedCssRemover::Remove(css, used);
  EXPECT_NE(result.cleaned_css.find("@supports"), std::string::npos);
  EXPECT_NE(result.cleaned_css.find(".grid-layout"), std::string::npos);
  EXPECT_EQ(result.cleaned_css.find(".unused"), std::string::npos);
}

TEST(UnusedCssRemoverTest, ComplexSelectorWithCommas) {
  // Complex selector list with commas.
  std::string css =
      "h1, h2, h3 { font-weight: bold; }\n"
      ".unused { color: red; }";

  size_t rule_end = css.find('\n');
  std::vector<std::pair<size_t, size_t>> used = {{0, rule_end}};

  auto result = UnusedCssRemover::Remove(css, used);
  EXPECT_NE(result.cleaned_css.find("h1, h2, h3"), std::string::npos);
  EXPECT_EQ(result.cleaned_css.find(".unused"), std::string::npos);
}

TEST(UnusedCssRemoverTest, EmptyUsedRangeStillOverlaps) {
  // An empty used range {5, 5} still triggers overlap because
  // OverlapsUsedRange checks (range.second > rule_start) && (range.first <
  // rule_end), without checking if the used range itself is non-empty.
  std::string css = "body { color: red; }";
  std::vector<std::pair<size_t, size_t>> used = {{5, 5}};

  auto result = UnusedCssRemover::Remove(css, used);
  // The rule [0,20) overlaps with used {5,5} per the algorithm, so it is kept.
  EXPECT_EQ(result.rules_removed, 0u);
}

TEST(UnusedCssRemoverTest, EmptyRuleRangeNotOverlap) {
  // When the rule's own range has start >= end, OverlapsUsedRange returns
  // false immediately (line 49). This is exercised indirectly when the CSS
  // parser produces a zero-length rule, but we test the Remove() path
  // with a rule that is genuinely unused and no used ranges at all.
  std::string css = "body { color: red; }";
  std::vector<std::pair<size_t, size_t>> used = {};

  auto result = UnusedCssRemover::Remove(css, used);
  EXPECT_EQ(result.rules_removed, 1u);
}

TEST(UnusedCssRemoverTest, MergeViewportCoverageSingleEmptyViewport) {
  std::vector<std::vector<std::pair<size_t, size_t>>> viewports = {{}};
  auto result = UnusedCssRemover::MergeViewportCoverage(viewports);
  EXPECT_TRUE(result.empty());
}

TEST(UnusedCssRemoverTest, MergeAllEmptyViewports) {
  std::vector<std::vector<std::pair<size_t, size_t>>> viewports = {{}, {}, {}};
  auto result = UnusedCssRemover::MergeViewportCoverage(viewports);
  EXPECT_TRUE(result.empty());
}

// An escaped quote in a selector opens no string. Inside a
// block (here an @media), the brace matcher took `.a\'b` for the start of a
// string, ran to the end of the sheet and swallowed every later rule into the
// @media block.
TEST(UnusedCssRemoverTest, EscapedQuoteInSelectorKeepsLaterRules) {
  std::string css =
      "@media (min-width:1px){.a\\'b{color:red}}\n.c{color:blue}\n"
      ".d{color:green}";
  const size_t media_end = css.find("}}") + 2;
  const size_t c_start = css.find(".c");
  const size_t c_end = css.find('}', c_start) + 1;
  std::vector<std::pair<size_t, size_t>> used = {{0, media_end},
                                                 {c_start, c_end}};
  auto result = UnusedCssRemover::Remove(css, used);
  EXPECT_EQ(result.rules_removed, 1u);
  EXPECT_NE(result.cleaned_css.find(".a\\'b"), std::string::npos);
  EXPECT_NE(result.cleaned_css.find(".c{"), std::string::npos);
  EXPECT_EQ(result.cleaned_css.find(".d{"), std::string::npos)
      << result.cleaned_css;
}

// A string missing its closing quote ends at the newline (a bad string), so
// the block after it closes and the next rule is its own.
TEST(UnusedCssRemoverTest, BadStringEndsAtNewline) {
  std::string css = "@media (x){.a{content:\"open\n}}\n.c{color:blue}";
  const size_t media_end = css.find("}}") + 2;
  std::vector<std::pair<size_t, size_t>> used = {{0, media_end}};
  auto result = UnusedCssRemover::Remove(css, used);
  EXPECT_EQ(result.rules_removed, 1u);
  EXPECT_EQ(result.cleaned_css.find(".c{"), std::string::npos)
      << result.cleaned_css;
}

}  // namespace
}  // namespace pagespeed
