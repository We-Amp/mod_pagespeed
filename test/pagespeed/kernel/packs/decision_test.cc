// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

#include "pagespeed/kernel/packs/decision.h"

#include <string>

#include "gtest/gtest.h"
#include "pagespeed/kernel/packs/kind_registry.h"

namespace pagespeed::packs {
namespace {

TEST(DecisionTest, ReasonNamesAreStable) {
  EXPECT_EQ(ReasonName(Reason::kAbsent), "absent");
  EXPECT_EQ(ReasonName(Reason::kClusterWithoutSelf), "cluster_without_self");
  EXPECT_EQ(ReasonName(Reason::kShadowed), "shadowed");
  EXPECT_EQ(ReasonName(Reason::kNotRewritable), "not_rewritable");
  EXPECT_EQ(ReasonName(Reason::kLateConflict), "late_conflict");
  EXPECT_EQ(ReasonName(Reason::kHeaderConflict), "header_conflict");
  EXPECT_EQ(ReasonName(Reason::kHeadersFlushed), "headers_flushed");
  EXPECT_EQ(ReasonName(Reason::kClaimed), "claimed");
}

TEST(DecisionTest, ClaimedByIsOnlyWrittenWhenSet) {
  PackDecision d;
  d.rule_id = "c";
  d.kind = FindKind("canonical");
  d.mode = Mode::kEnforce;
  d.reason = Reason::kClaimed;
  EXPECT_EQ(DecisionToJsonLine("a.test", "/", d).find("claimed_by"),
            std::string::npos);
  d.claimed_by = "edge-seo";
  EXPECT_EQ(DecisionToJsonLine("a.test", "/", d),
            "{\"host\":\"a.test\",\"path\":\"/\",\"rule\":\"c\","
            "\"kind\":\"canonical\",\"mode\":\"enforce\","
            "\"action\":\"none\",\"reason\":\"claimed\",\"defect\":\"\","
            "\"before\":\"\",\"after\":\"\",\"claimed_by\":\"edge-seo\"}");
}

TEST(DecisionTest, LineHasFixedKeyOrderAndNoTimestamp) {
  PackDecision d;
  d.rule_id = "title-home";
  d.kind = FindKind("title");
  d.mode = Mode::kEnforce;
  d.action = Action::kInsert;
  d.reason = Reason::kAbsent;
  d.defect = "title-missing";
  d.after = "Home";
  EXPECT_EQ(DecisionToJsonLine("www.example.com", "/", d),
            "{\"host\":\"www.example.com\",\"path\":\"/\","
            "\"rule\":\"title-home\",\"kind\":\"title\",\"mode\":\"enforce\","
            "\"action\":\"insert\",\"reason\":\"absent\","
            "\"defect\":\"title-missing\",\"before\":\"\",\"after\":\"Home\"}");
}

TEST(DecisionTest, NewReasonsReachTheLine) {
  PackDecision d;
  d.rule_id = "canon";
  d.kind = FindKind("canonical");
  d.reason = Reason::kHeadersFlushed;
  const std::string line = DecisionToJsonLine("a.test", "/p", d);
  EXPECT_NE(line.find("\"reason\":\"headers_flushed\""), std::string::npos)
      << line;
}

TEST(DecisionTest, RemovedIsOnlyWrittenWhenSet) {
  PackDecision d;
  d.rule_id = "ld";
  d.kind = FindKind("jsonld");
  EXPECT_EQ(DecisionToJsonLine("a.test", "/", d).find("removed"),
            std::string::npos);
  d.removed = "Thing (duplicate)";
  EXPECT_NE(DecisionToJsonLine("a.test", "/", d)
                .find(",\"removed\":\"Thing (duplicate)\"}"),
            std::string::npos);
}

TEST(DecisionTest, HeaderIsOnlyWrittenForAResponseHeaderDecision) {
  PackDecision d;
  d.rule_id = "canon";
  d.kind = FindKind("canonical");
  d.action = Action::kInsert;
  d.reason = Reason::kAbsent;
  d.after = "https://a.test/p";
  const std::string plain = DecisionToJsonLine("a.test", "/p", d);
  EXPECT_EQ(plain.find("header"), std::string::npos);
  d.header = true;
  const std::string line = DecisionToJsonLine("a.test", "/p", d);
  EXPECT_EQ(line, plain.substr(0, plain.size() - 1) + ",\"header\":true}");
  // The flag is part of the decision's identity.
  PackDecision other = d;
  other.header = false;
  EXPECT_FALSE(other == d);
}

TEST(DecisionTest, PathIsCutAndValuesAreEscaped) {
  PackDecision d;
  d.rule_id = "t";
  d.kind = FindKind("title");
  d.after = "a \"quoted\" \\ title";
  const std::string line =
      DecisionToJsonLine("a.test", std::string(400, 'x'), d);
  EXPECT_NE(line.find("\"path\":\"" + std::string(300, 'x') + "\""),
            std::string::npos);
  EXPECT_NE(line.find("a \\\"quoted\\\" \\\\ title"), std::string::npos)
      << line;
}

TEST(DecisionTest, HashAndTruncate) {
  EXPECT_EQ(HashValue(""), "fnv1a64:cbf29ce484222325");
  EXPECT_EQ(HashValue("a"), "fnv1a64:af63dc4c8601ec8c");
  EXPECT_EQ(TruncateUtf8("short", 10), "short");
  // The two-byte "e with acute" is not split.
  EXPECT_EQ(TruncateUtf8("ab\xC3\xA9", 3), "ab");
  EXPECT_EQ(TruncateUtf8("ab\xC3\xA9", 4), "ab\xC3\xA9");
}

}  // namespace
}  // namespace pagespeed::packs
