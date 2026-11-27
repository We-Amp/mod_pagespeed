// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

// The worker's HTML mode: wire names, the precedence rule against the legacy
// disable_html boolean, and the nginx front's two decisions, which must
// reproduce the old boolean exactly for off and full.

#include "src/worker/html_mode.h"

#include <optional>
#include <string_view>

#include "gtest/gtest.h"

namespace pagespeed {
namespace {

constexpr HtmlMode kAllModes[] = {HtmlMode::kOff, HtmlMode::kMarkdownStatic,
                                  HtmlMode::kMarkdownRendered, HtmlMode::kFull};

TEST(HtmlModeTest, NamesRoundTrip) {
  for (HtmlMode m : kAllModes) {
    auto parsed = ParseHtmlMode(HtmlModeName(m));
    ASSERT_TRUE(parsed.has_value()) << HtmlModeName(m);
    EXPECT_EQ(*parsed, m);
  }
  EXPECT_EQ(HtmlModeName(HtmlMode::kOff), "off");
  EXPECT_EQ(HtmlModeName(HtmlMode::kMarkdownStatic), "markdown-static");
  EXPECT_EQ(HtmlModeName(HtmlMode::kMarkdownRendered), "markdown-rendered");
  EXPECT_EQ(HtmlModeName(HtmlMode::kFull), "full");
}

TEST(HtmlModeTest, ParseIsExact) {
  for (std::string_view bad :
       {"", "Off", "FULL", "markdown", "markdown_static", " full", "full ",
        "true", "false", "1", "0", "markdown-static\r"}) {
    EXPECT_FALSE(ParseHtmlMode(bad).has_value()) << "'" << bad << "'";
  }
}

TEST(HtmlModeTest, DefaultIsFull) {
  EXPECT_EQ(kDefaultHtmlMode, HtmlMode::kFull);
  EXPECT_EQ(ResolveHtmlMode(std::nullopt, std::nullopt), HtmlMode::kFull);
}

TEST(HtmlModeTest, LegacyMapping) {
  EXPECT_EQ(HtmlModeFromDisableHtml(true), HtmlMode::kOff);
  EXPECT_EQ(HtmlModeFromDisableHtml(false), HtmlMode::kFull);
  EXPECT_EQ(ResolveHtmlMode(std::nullopt, true), HtmlMode::kOff);
  EXPECT_EQ(ResolveHtmlMode(std::nullopt, false), HtmlMode::kFull);
}

TEST(HtmlModeTest, ExplicitModeWinsOverLegacy) {
  for (HtmlMode m : kAllModes) {
    EXPECT_EQ(ResolveHtmlMode(m, true), m);
    EXPECT_EQ(ResolveHtmlMode(m, false), m);
    EXPECT_EQ(ResolveHtmlMode(m, std::nullopt), m);
  }
}

TEST(HtmlModeTest, DerivedDisableHtmlOnlyForOff) {
  EXPECT_TRUE(HtmlModeDisablesHtml(HtmlMode::kOff));
  EXPECT_FALSE(HtmlModeDisablesHtml(HtmlMode::kMarkdownStatic));
  EXPECT_FALSE(HtmlModeDisablesHtml(HtmlMode::kMarkdownRendered));
  EXPECT_FALSE(HtmlModeDisablesHtml(HtmlMode::kFull));
  // The derived boolean maps back to the mode for off and full.
  EXPECT_EQ(HtmlModeFromDisableHtml(HtmlModeDisablesHtml(HtmlMode::kOff)),
            HtmlMode::kOff);
  EXPECT_EQ(HtmlModeFromDisableHtml(HtmlModeDisablesHtml(HtmlMode::kFull)),
            HtmlMode::kFull);
}

TEST(HtmlModeTest, OnlyFullWritesHtmlVariants) {
  EXPECT_FALSE(HtmlModeWritesHtmlVariant(HtmlMode::kOff));
  EXPECT_FALSE(HtmlModeWritesHtmlVariant(HtmlMode::kMarkdownStatic));
  EXPECT_FALSE(HtmlModeWritesHtmlVariant(HtmlMode::kMarkdownRendered));
  EXPECT_TRUE(HtmlModeWritesHtmlVariant(HtmlMode::kFull));
  EXPECT_FALSE(HtmlModeIsMarkdown(HtmlMode::kOff));
  EXPECT_TRUE(HtmlModeIsMarkdown(HtmlMode::kMarkdownStatic));
  EXPECT_TRUE(HtmlModeIsMarkdown(HtmlMode::kMarkdownRendered));
  EXPECT_FALSE(HtmlModeIsMarkdown(HtmlMode::kFull));
}

// The nginx front's decisions before html_mode, as functions of the old
// boolean: an HTML store notified unless disable_html; an HTML variant-miss
// hit re-notified unless disable_html.
bool OldNotifiesStore(bool disable_html) { return !disable_html; }
bool OldRenotifiesHit(bool disable_html) { return !disable_html; }

TEST(HtmlModeTest, NginxFrontCompatOffAndFull) {
  for (bool disable_html : {true, false}) {
    const HtmlMode m = ResolveHtmlMode(std::nullopt, disable_html);
    EXPECT_EQ(HtmlModeNotifiesHtmlStore(m), OldNotifiesStore(disable_html));
    for (bool agent : {true, false}) {
      EXPECT_EQ(HtmlModeRenotifiesHtmlHit(m, agent),
                OldRenotifiesHit(disable_html))
          << "disable_html=" << disable_html << " agent=" << agent;
    }
  }
  // The same through the explicit names.
  EXPECT_FALSE(HtmlModeNotifiesHtmlStore(HtmlMode::kOff));
  EXPECT_TRUE(HtmlModeNotifiesHtmlStore(HtmlMode::kFull));
}

TEST(HtmlModeTest, NginxFrontMarkdownModes) {
  for (HtmlMode m : {HtmlMode::kMarkdownStatic, HtmlMode::kMarkdownRendered}) {
    // The store is still sent: the worker derives the agent copy from it.
    EXPECT_TRUE(HtmlModeNotifiesHtmlStore(m));
    // A variant miss re-notifies only for an agent request.
    EXPECT_TRUE(HtmlModeRenotifiesHtmlHit(m, /*agent_demand=*/true));
    EXPECT_FALSE(HtmlModeRenotifiesHtmlHit(m, /*agent_demand=*/false));
  }
}

}  // namespace
}  // namespace pagespeed
