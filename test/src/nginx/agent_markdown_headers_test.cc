// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

#include "src/nginx/agent_markdown_headers.h"

#include <cstddef>
#include <string>

#include "gtest/gtest.h"

namespace pagespeed {
namespace {

// The estimate is bytes / 4, rounded down.
TEST(AgentMarkdownHeaders, TokenEstimateIsBytesOverFourRoundedDown) {
  EXPECT_EQ(EstimateMarkdownTokens(0), 0u);
  EXPECT_EQ(EstimateMarkdownTokens(3), 0u);
  EXPECT_EQ(EstimateMarkdownTokens(4), 1u);
  EXPECT_EQ(EstimateMarkdownTokens(7), 1u);
  EXPECT_EQ(EstimateMarkdownTokens(2900), 725u);
  EXPECT_EQ(EstimateMarkdownTokens(2903), 725u);
}

TEST(AgentMarkdownHeaders, TokenHeaderValueIsPlainDecimal) {
  EXPECT_EQ(MarkdownTokensHeaderValue(0), "0");
  EXPECT_EQ(MarkdownTokensHeaderValue(3), "0");
  EXPECT_EQ(MarkdownTokensHeaderValue(2900), "725");
  // The Markdown copy has a fixed size limit far below this, but the value
  // must not wrap for any body size the serve path can hold.
  EXPECT_EQ(MarkdownTokensHeaderValue(static_cast<size_t>(1) << 32),
            "1073741824");
}

TEST(AgentMarkdownHeaders, ContentSignalAcceptsPolicyValues) {
  EXPECT_TRUE(
      IsValidContentSignalValue("search=yes, ai-input=yes, ai-train=no"));
  EXPECT_TRUE(IsValidContentSignalValue("ai-train=no"));
  EXPECT_TRUE(IsValidContentSignalValue("search=yes,\tai-train=no"));
  EXPECT_TRUE(
      IsValidContentSignalValue(std::string(kMaxContentSignalBytes, 'a')));
}

// Anything that could split the response header or is not plain ASCII is
// refused when nginx reads the configuration.
TEST(AgentMarkdownHeaders, ContentSignalRefusesUnsafeValues) {
  EXPECT_FALSE(IsValidContentSignalValue(""));
  EXPECT_FALSE(IsValidContentSignalValue("ai-train=no\r\nSet-Cookie: x=1"));
  EXPECT_FALSE(IsValidContentSignalValue("ai-train=no\n"));
  EXPECT_FALSE(IsValidContentSignalValue(std::string("ai-train=no\0x", 13)));
  EXPECT_FALSE(IsValidContentSignalValue("ai-train=n\x7f"));
  EXPECT_FALSE(IsValidContentSignalValue("ai-train=n\xc3\xb6"));
  EXPECT_FALSE(IsValidContentSignalValue(" ai-train=no"));
  EXPECT_FALSE(IsValidContentSignalValue("ai-train=no\t"));
  EXPECT_FALSE(
      IsValidContentSignalValue(std::string(kMaxContentSignalBytes + 1, 'a')));
}

}  // namespace
}  // namespace pagespeed
