// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

#include "lib/html/robots_ai_directives.h"

#include "gtest/gtest.h"

namespace net_instaweb {
namespace {

TEST(RobotsAiDirectivesTest, WildcardDisallowRootBlocks) {
  EXPECT_TRUE(
      AnalyzeRobotsForAi("User-agent: *\nDisallow: /").ai_blocked_site_wide);
}

TEST(RobotsAiDirectivesTest, AiUserAgentDisallowRootBlocks) {
  EXPECT_TRUE(AnalyzeRobotsForAi("User-agent: GPTBot\nDisallow: /")
                  .ai_blocked_site_wide);
  EXPECT_TRUE(AnalyzeRobotsForAi("User-agent: Google-Extended\nDisallow: /")
                  .ai_blocked_site_wide);
}

TEST(RobotsAiDirectivesTest, CaseInsensitiveUserAgent) {
  EXPECT_TRUE(AnalyzeRobotsForAi("user-agent: gptbot\ndisallow: /")
                  .ai_blocked_site_wide);
}

TEST(RobotsAiDirectivesTest, NonRootDisallowDoesNotBlock) {
  EXPECT_FALSE(AnalyzeRobotsForAi("User-agent: *\nDisallow: /private/")
                   .ai_blocked_site_wide);
}

TEST(RobotsAiDirectivesTest, NonAiUserAgentDisallowDoesNotBlock) {
  // Blocking a non-AI crawler (Googlebot search) at root must NOT suppress.
  EXPECT_FALSE(AnalyzeRobotsForAi("User-agent: Googlebot\nDisallow: /")
                   .ai_blocked_site_wide);
}

TEST(RobotsAiDirectivesTest, AllowRootBeatsDisallowRoot) {
  EXPECT_FALSE(AnalyzeRobotsForAi("User-agent: *\nAllow: /\nDisallow: /")
                   .ai_blocked_site_wide);
}

TEST(RobotsAiDirectivesTest, EmptyRobotsIsPermissive) {
  EXPECT_FALSE(AnalyzeRobotsForAi("").ai_blocked_site_wide);
}

TEST(RobotsAiDirectivesTest, CommentsAndBlankLinesIgnored) {
  EXPECT_TRUE(AnalyzeRobotsForAi(
                  "# my robots\n\nUser-agent: CCBot   # the common crawl bot\n"
                  "Disallow: /   # block everything\n")
                  .ai_blocked_site_wide);
}

TEST(RobotsAiDirectivesTest, WildcardStarPathBlocks) {
  EXPECT_TRUE(
      AnalyzeRobotsForAi("User-agent: *\nDisallow: /*").ai_blocked_site_wide);
}

TEST(RobotsAiDirectivesTest, KnownAgentsNonEmpty) {
  EXPECT_FALSE(KnownAiUserAgents().empty());
}

TEST(RobotsAiDirectivesTest, PageNoaiHeaderExcludes) {
  EXPECT_TRUE(PageExcludedByAiDirectives("noai", "", ""));
  EXPECT_TRUE(PageExcludedByAiDirectives("noindex, noai", "", ""));
  EXPECT_TRUE(PageExcludedByAiDirectives("noimageai", "", ""));
}

TEST(RobotsAiDirectivesTest, PageNoaiMetaExcludes) {
  EXPECT_TRUE(PageExcludedByAiDirectives("", "noai", ""));
  EXPECT_TRUE(PageExcludedByAiDirectives("", "noindex,noai", ""));
}

TEST(RobotsAiDirectivesTest, GoogleExtendedNoneExcludes) {
  EXPECT_TRUE(PageExcludedByAiDirectives("", "", "none"));
  EXPECT_TRUE(PageExcludedByAiDirectives("", "", "  NONE  "));
}

TEST(RobotsAiDirectivesTest, OrdinaryDirectivesDoNotExclude) {
  EXPECT_FALSE(PageExcludedByAiDirectives("noindex", "noindex, nofollow", ""));
  EXPECT_FALSE(PageExcludedByAiDirectives("", "", ""));
  EXPECT_FALSE(PageExcludedByAiDirectives("", "", "all"));
}

TEST(RobotsAiDirectivesTest, NoneTokenExcludes) {
  EXPECT_TRUE(PageExcludedByAiDirectives("none", "", ""));
}

}  // namespace
}  // namespace net_instaweb
