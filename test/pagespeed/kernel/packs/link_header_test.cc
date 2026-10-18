// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

#include "pagespeed/kernel/packs/link_header.h"

#include <string>
#include <vector>

#include "gtest/gtest.h"
#include "pagespeed/kernel/packs/pack.h"

namespace pagespeed::packs {
namespace {

TEST(SplitLinkHeaderTest, EmptyAndBlank) {
  EXPECT_TRUE(SplitLinkHeader("").empty());
  EXPECT_TRUE(SplitLinkHeader("   ").empty());
  EXPECT_TRUE(SplitLinkHeader(" , ,, ").empty());
}

TEST(SplitLinkHeaderTest, OneValueWithParameters) {
  auto v = SplitLinkHeader("<https://a.test/x>; rel=\"canonical\"");
  ASSERT_EQ(v.size(), 1u);
  EXPECT_EQ(v[0].target, "https://a.test/x");
  ASSERT_EQ(v[0].params.size(), 1u);
  EXPECT_EQ(v[0].params[0].first, "rel");
  EXPECT_EQ(v[0].params[0].second, "canonical");
  EXPECT_EQ(v[0].raw, "<https://a.test/x>; rel=\"canonical\"");
  EXPECT_TRUE(v[0].HasRel("canonical"));
  EXPECT_FALSE(v[0].HasRel("preload"));
}

TEST(SplitLinkHeaderTest, SeveralValuesKeepTheirRawText) {
  auto v = SplitLinkHeader(
      "</a.css>; rel=preload; as=style, <https://a.test/x>; rel=canonical ,"
      "<https://cdn.test>;rel=preconnect");
  ASSERT_EQ(v.size(), 3u);
  EXPECT_EQ(v[0].raw, "</a.css>; rel=preload; as=style");
  EXPECT_EQ(v[1].raw, "<https://a.test/x>; rel=canonical");
  EXPECT_EQ(v[2].raw, "<https://cdn.test>;rel=preconnect");
  EXPECT_TRUE(v[0].HasRel("preload"));
  EXPECT_EQ(v[0].Param("as"), "style");
  EXPECT_TRUE(v[1].HasRel("canonical"));
  EXPECT_TRUE(v[2].HasRel("preconnect"));
}

TEST(SplitLinkHeaderTest, CommaInsideTheTargetIsNotASeparator) {
  auto v = SplitLinkHeader(
      "<https://a.test/x,y?z=1,2>; rel=canonical, <https://a.test/b>; "
      "rel=next");
  ASSERT_EQ(v.size(), 2u);
  EXPECT_EQ(v[0].target, "https://a.test/x,y?z=1,2");
  EXPECT_EQ(v[1].target, "https://a.test/b");
}

TEST(SplitLinkHeaderTest, CommaAndSemicolonInsideAQuotedStringAreLiteral) {
  auto v = SplitLinkHeader(
      "<https://a.test/x>; title=\"one, two; three\"; rel=canonical, "
      "<https://a.test/y>; rel=next");
  ASSERT_EQ(v.size(), 2u);
  EXPECT_EQ(v[0].Param("title"), "one, two; three");
  EXPECT_TRUE(v[0].HasRel("canonical"));
  EXPECT_EQ(v[1].target, "https://a.test/y");
}

TEST(SplitLinkHeaderTest, EscapedQuoteInsideAQuotedString) {
  auto v =
      SplitLinkHeader("<https://a.test/x>; title=\"a \\\" b, c\"; rel=next");
  ASSERT_EQ(v.size(), 1u);
  EXPECT_EQ(v[0].Param("title"), "a \" b, c");
  EXPECT_TRUE(v[0].HasRel("next"));
}

TEST(SplitLinkHeaderTest, RelHoldsSeveralTokensCaseInsensitively) {
  auto v = SplitLinkHeader("<https://a.test/x>; REL=\"Alternate  CANONICAL\"");
  ASSERT_EQ(v.size(), 1u);
  EXPECT_TRUE(v[0].HasRel("canonical"));
  EXPECT_TRUE(v[0].HasRel("alternate"));
  EXPECT_FALSE(v[0].HasRel("canon"));
}

TEST(SplitLinkHeaderTest, EveryRelParameterCounts) {
  auto v = SplitLinkHeader("<https://a.test/x>; rel=next; rel=canonical");
  ASSERT_EQ(v.size(), 1u);
  EXPECT_TRUE(v[0].HasRel("next"));
  EXPECT_TRUE(v[0].HasRel("canonical"));
}

TEST(SplitLinkHeaderTest, ParametersWithoutValueAndExtendedNames) {
  auto v = SplitLinkHeader("<https://a.test/x>; async; title*=UTF-8''a%20b");
  ASSERT_EQ(v.size(), 1u);
  EXPECT_EQ(v[0].Param("async"), "");
  EXPECT_EQ(v[0].Param("title*"), "UTF-8''a%20b");
  EXPECT_FALSE(v[0].HasRel("canonical"));
}

TEST(SplitLinkHeaderTest, SurroundingWhitespaceIsNotPartOfARawValue) {
  auto v = SplitLinkHeader("  <https://a.test/x> ;\trel=canonical   ");
  ASSERT_EQ(v.size(), 1u);
  EXPECT_EQ(v[0].raw, "<https://a.test/x> ;\trel=canonical");
  EXPECT_TRUE(v[0].HasRel("canonical"));
}

TEST(SplitLinkHeaderTest, ReportsWhetherEverythingWasParsed) {
  bool clean = false;
  SplitLinkHeader("<https://a.test/x>; rel=next, </b>; rel=prev", &clean);
  EXPECT_TRUE(clean);
  SplitLinkHeader(" , ", &clean);
  EXPECT_TRUE(clean);
  // An unquoted value with spaces does not parse.
  auto v = SplitLinkHeader(
      "<https://a.test/stale>; rel=canonical, </a.css>; rel=preload; "
      "media=(max-width: 600px)",
      &clean);
  EXPECT_FALSE(clean);
  ASSERT_EQ(v.size(), 1u);
  SplitLinkHeader("junk, <https://a.test/x>", &clean);
  EXPECT_FALSE(clean);
}

TEST(SplitLinkHeaderTest, MalformedPiecesAreSkippedNotFatal) {
  // No leading '<'.
  auto a = SplitLinkHeader("junk; rel=canonical, <https://a.test/x>; rel=next");
  ASSERT_EQ(a.size(), 1u);
  EXPECT_EQ(a[0].target, "https://a.test/x");
  // Unterminated target.
  EXPECT_TRUE(SplitLinkHeader("<https://a.test/x; rel=canonical").empty());
  // Unterminated quote swallows the rest of that value only.
  auto b =
      SplitLinkHeader("<https://a.test/x>; title=\"oops, <https://a.test/y>");
  EXPECT_TRUE(b.empty());
  // Garbage between the target and the parameters.
  auto c =
      SplitLinkHeader("<https://a.test/x> junk; rel=canonical, </b>; rel=next");
  ASSERT_EQ(c.size(), 1u);
  EXPECT_EQ(c[0].target, "/b");
}

TEST(SplitLinkHeaderTest, AngleBracketsAndCommasInAQuotedStringAreLiteral) {
  auto v = SplitLinkHeader(
      "<https://a.test/x>; title=\"<a>, <b>\"; rel=canonical, "
      "<https://a.test/y,z?q=\"1,2\">; rel=next");
  ASSERT_EQ(v.size(), 2u);
  EXPECT_EQ(v[0].Param("title"), "<a>, <b>");
  EXPECT_TRUE(v[0].HasRel("canonical"));
  EXPECT_EQ(v[1].target, "https://a.test/y,z?q=\"1,2\"");
  EXPECT_TRUE(v[1].HasRel("next"));
}

TEST(SplitLinkHeaderTest, ASemicolonInsideTheTargetIsPartOfIt) {
  auto v = SplitLinkHeader("<https://a.test/x;y,z>; rel=canonical");
  ASSERT_EQ(v.size(), 1u);
  EXPECT_EQ(v[0].target, "https://a.test/x;y,z");
  ASSERT_EQ(v[0].params.size(), 1u);
}

TEST(SplitLinkHeaderTest, CommasWithoutSpacesAndAdjacentValues) {
  auto v = SplitLinkHeader(
      "<https://a.test/1>;rel=next,<https://a.test/2>;rel=prev,<https://a.test/"
      "3>");
  ASSERT_EQ(v.size(), 3u);
  EXPECT_EQ(v[2].target, "https://a.test/3");
  EXPECT_TRUE(v[2].params.empty());
}

TEST(SplitLinkHeaderTest, HugeInputDoesNotMisbehave) {
  std::string big;
  for (int i = 0; i < 5000; ++i) big += "<https://a.test/x>; rel=next, ";
  EXPECT_EQ(SplitLinkHeader(big).size(), 5000u);
  EXPECT_TRUE(SplitLinkHeader(std::string(100000, '<')).empty());
  EXPECT_TRUE(SplitLinkHeader(std::string(100000, '"')).empty());
}

TEST(EncodeLinkTargetTest, PercentEncodesWhatMayNotAppearRaw) {
  EXPECT_EQ(EncodeLinkTarget("https://a.test/x"), "https://a.test/x");
  EXPECT_EQ(EncodeLinkTarget("https://a.test/a b"), "https://a.test/a%20b");
  EXPECT_EQ(EncodeLinkTarget("https://a.test/<x>\"y\",z;w\\"),
            "https://a.test/%3Cx%3E%22y%22%2Cz%3Bw%5C");
  EXPECT_EQ(EncodeLinkTarget("https://a.test/\xC3\xA9"),
            "https://a.test/%C3%A9");
  EXPECT_EQ(EncodeLinkTarget(std::string("https://a.test/a\0b", 18)),
            "https://a.test/a%00b");
  // Existing escapes and query punctuation stay.
  EXPECT_EQ(EncodeLinkTarget("https://a.test/a%20b?x=1&y=2#f"),
            "https://a.test/a%20b?x=1&y=2#f");
}

TEST(MakeCanonicalLinkValueTest, WritesAParsableValue) {
  auto value = MakeCanonicalLinkValue("https://a.test/x,y z");
  ASSERT_TRUE(value.has_value());
  EXPECT_EQ(*value, "<https://a.test/x%2Cy%20z>; rel=\"canonical\"");
  auto back = SplitLinkHeader(*value);
  ASSERT_EQ(back.size(), 1u);
  EXPECT_TRUE(back[0].HasRel("canonical"));
  EXPECT_EQ(back[0].target, "https://a.test/x%2Cy%20z");
}

TEST(MakeCanonicalLinkValueTest, LengthIsCapped) {
  EXPECT_FALSE(MakeCanonicalLinkValue("").has_value());
  const std::string ok =
      "https://a.test/" + std::string(kMaxUrlBytes - 15, 'a');
  EXPECT_TRUE(MakeCanonicalLinkValue(ok).has_value());
  EXPECT_FALSE(MakeCanonicalLinkValue(ok + "a").has_value());
  // Encoding can push a short-enough URL over the cap.
  const std::string spaces =
      "https://a.test/" + std::string(kMaxUrlBytes / 2, ' ');
  EXPECT_FALSE(MakeCanonicalLinkValue(spaces).has_value());
}

TEST(LinkValueWithoutRelTokenTest, DropsOnlyThatToken) {
  auto v = SplitLinkHeader(
      "<https://a.test/x>; rel=\"canonical preload\"; as=style; "
      "title=\"a \\\"b\\\"\"; async");
  ASSERT_EQ(v.size(), 1u);
  EXPECT_EQ(LinkValueWithoutRelToken(v[0], "canonical"),
            "<https://a.test/x>; rel=\"preload\"; as=\"style\"; "
            "title=\"a \\\"b\\\"\"; async");
  // What it wrote is a link-value that says the same minus the token.
  auto back = SplitLinkHeader(LinkValueWithoutRelToken(v[0], "canonical"));
  ASSERT_EQ(back.size(), 1u);
  EXPECT_TRUE(back[0].HasRel("preload"));
  EXPECT_FALSE(back[0].HasRel("canonical"));
  EXPECT_EQ(back[0].Param("title"), "a \"b\"");
}

TEST(LinkValueWithoutRelTokenTest,
     ExtendedValuesStayTokensAndRelsAreMerged) {
  auto v = SplitLinkHeader(
      "<https://a.test/x>; title*=UTF-8''a%20b; rel=\"canonical preload\"; "
      "rel=\"canonical\"");
  ASSERT_EQ(v.size(), 1u);
  EXPECT_EQ(LinkValueWithoutRelToken(v[0], "canonical"),
            "<https://a.test/x>; title*=UTF-8''a%20b; rel=\"preload\"");
}

TEST(LinkValueWithoutRelTokenTest, EveryRelParameterIsFiltered) {
  // Both orders: the token is dropped from the merged rel parameters and
  // the link-value keeps its remaining meaning.
  auto a =
      SplitLinkHeader("<https://a.test/x>; rel=\"canonical\"; rel=\"next\"");
  ASSERT_EQ(a.size(), 1u);
  EXPECT_EQ(LinkValueWithoutRelToken(a[0], "canonical"),
            "<https://a.test/x>; rel=\"next\"");

  auto b =
      SplitLinkHeader("<https://a.test/x>; rel=\"next\"; rel=\"canonical\"");
  ASSERT_EQ(b.size(), 1u);
  EXPECT_EQ(LinkValueWithoutRelToken(b[0], "canonical"),
            "<https://a.test/x>; rel=\"next\"");
  EXPECT_EQ(LinkValueWithoutRelToken(b[0], "next"),
            "<https://a.test/x>; rel=\"canonical\"");
}

TEST(LinkValueWithoutRelTokenTest, OnlyTokenLeavesNothing) {
  auto v = SplitLinkHeader("<https://a.test/x>; rel=canonical; title=t");
  ASSERT_EQ(v.size(), 1u);
  EXPECT_EQ(LinkValueWithoutRelToken(v[0], "canonical"), "");
}

TEST(LinkValueWithoutRelTokenTest, ValueWithoutTheTokenIsReturnedAsWritten) {
  auto v = SplitLinkHeader("<https://a.test/x> ;  rel=next");
  ASSERT_EQ(v.size(), 1u);
  EXPECT_EQ(LinkValueWithoutRelToken(v[0], "canonical"), v[0].raw);
}

// ---------------------------------------------------------------------------
// PlanCanonicalLinkHeader
// ---------------------------------------------------------------------------

constexpr char kPage[] = "https://a.test/page";
constexpr char kNorm[] = "https://a.test/page";

CanonicalLinkEdit Plan(const std::vector<std::string>& lines,
                       OnPresent on_present = OnPresent::kKeep,
                       std::string_view final_norm = kNorm,
                       std::string_view final_url = kPage,
                       std::string_view target_norm = kNorm) {
  return PlanCanonicalLinkHeader(lines, kPage, final_norm, final_url,
                                 target_norm, on_present);
}

TEST(PlanCanonicalLinkHeaderTest, AddsWhenTheHeaderHasNoCanonical) {
  CanonicalLinkEdit e = Plan({"</a.css>; rel=preload; as=style"});
  ASSERT_TRUE(e.applicable);
  EXPECT_EQ(e.action, Action::kInsert);
  EXPECT_EQ(e.reason, Reason::kAbsent);
  EXPECT_TRUE(e.defect.empty());
  EXPECT_TRUE(e.mutate);
  EXPECT_TRUE(e.append_only);
  EXPECT_EQ(e.lines, (std::vector<std::string>{
                         "</a.css>; rel=preload; as=style",
                         "<https://a.test/page>; rel=\"canonical\""}));
  // No Link header at all.
  e = Plan({});
  EXPECT_EQ(e.action, Action::kInsert);
  ASSERT_EQ(e.lines.size(), 1u);
}

TEST(PlanCanonicalLinkHeaderTest, AddsOnlyTheRulesOwnValue) {
  // The document keeps its own canonical (keep, or repair with a usable
  // one): the header is not made to say something the rule did not decide.
  CanonicalLinkEdit e = Plan({"</a.css>; rel=preload"}, OnPresent::kRepair,
                             "https://a.test/mine", "https://a.test/mine");
  EXPECT_FALSE(e.applicable);
  EXPECT_FALSE(e.mutate);
}

TEST(PlanCanonicalLinkHeaderTest, NothingToSayWithoutADocumentCanonical) {
  EXPECT_FALSE(
      Plan({"</a.css>; rel=preload"}, OnPresent::kRepair, "", "").applicable);
}

TEST(PlanCanonicalLinkHeaderTest, EqualIsLeftAlone) {
  // Absolute, relative, other case host, a trailing slash, extra parameters:
  // all the same URL as far as the document is concerned.
  for (const char* v :
       {"<https://a.test/page>; rel=canonical", "</page>; rel=canonical",
        "<https://A.test/page/>; rel=\"canonical\"; title=x",
        "<https://a.test/page#frag>; REL=\"Alternate Canonical\""}) {
    CanonicalLinkEdit e = Plan({v}, OnPresent::kReplace);
    ASSERT_TRUE(e.applicable) << v;
    EXPECT_EQ(e.action, Action::kNone) << v;
    EXPECT_EQ(e.reason, Reason::kEqual) << v;
    EXPECT_FALSE(e.mutate) << v;
  }
}

TEST(PlanCanonicalLinkHeaderTest, KeepOnlyReportsAConflict) {
  CanonicalLinkEdit e = Plan({"<https://a.test/stale>; rel=canonical"});
  ASSERT_TRUE(e.applicable);
  EXPECT_EQ(e.action, Action::kNone);
  EXPECT_EQ(e.reason, Reason::kHeaderConflict);
  EXPECT_EQ(e.defect, "canonical-header-conflict");
  EXPECT_FALSE(e.before_hash.empty());
  EXPECT_FALSE(e.mutate);
}

TEST(PlanCanonicalLinkHeaderTest, RepairTakesOutOnlyTheConflictingEntry) {
  CanonicalLinkEdit e =
      Plan({"</a.css>; rel=preload; as=style, <https://a.test/stale>; "
            "rel=\"canonical\", <https://cdn.test>; rel=preconnect",
            "<https://a.test/amp>; rel=amphtml"},
           OnPresent::kRepair);
  ASSERT_TRUE(e.applicable);
  EXPECT_EQ(e.action, Action::kReplace);
  EXPECT_EQ(e.reason, Reason::kHeaderConflict);
  EXPECT_EQ(e.defect, "canonical-header-conflict");
  EXPECT_TRUE(e.mutate);
  EXPECT_FALSE(e.append_only);
  EXPECT_EQ(e.lines, (std::vector<std::string>{
                         "</a.css>; rel=preload; as=style, <https://cdn.test>; "
                         "rel=preconnect",
                         "<https://a.test/amp>; rel=amphtml",
                         "<https://a.test/page>; rel=\"canonical\""}));
}

TEST(PlanCanonicalLinkHeaderTest, AnEqualEntryThatRemainsIsNotAddedAgain) {
  CanonicalLinkEdit e = Plan({"<https://a.test/page>; rel=canonical, "
                              "<https://a.test/stale>; rel=canonical"},
                             OnPresent::kReplace);
  EXPECT_EQ(e.action, Action::kReplace);
  EXPECT_EQ(e.lines,
            (std::vector<std::string>{"<https://a.test/page>; rel=canonical"}));
}

TEST(PlanCanonicalLinkHeaderTest, LineWithOnlyAConflictIsDropped) {
  CanonicalLinkEdit e =
      Plan({"<https://a.test/stale>; rel=canonical", "</a.css>; rel=preload"},
           OnPresent::kReplace);
  EXPECT_EQ(e.lines, (std::vector<std::string>{
                         "</a.css>; rel=preload",
                         "<https://a.test/page>; rel=\"canonical\""}));
}

TEST(PlanCanonicalLinkHeaderTest, AMixedRelKeepsTheRestOfItsMeaning) {
  CanonicalLinkEdit e =
      Plan({"<https://a.test/old>; rel=\"canonical preload\"; as=style"},
           OnPresent::kReplace);
  EXPECT_EQ(e.lines, (std::vector<std::string>{
                         "<https://a.test/old>; rel=\"preload\"; as=\"style\"",
                         "<https://a.test/page>; rel=\"canonical\""}));
}

TEST(PlanCanonicalLinkHeaderTest, UnrelatedLinesAreKeptByteForByte) {
  CanonicalLinkEdit e = Plan({"  </a.css> ;rel=preload  ,junk",
                              "<https://a.test/stale>; rel=canonical"},
                             OnPresent::kRepair);
  ASSERT_EQ(e.lines.size(), 2u);
  EXPECT_EQ(e.lines[0], "  </a.css> ;rel=preload  ,junk");
}

TEST(PlanCanonicalLinkHeaderTest, ALineWithUnparsedTextIsNeverRebuilt) {
  // The preload's unquoted value with spaces does not parse; taking the stale
  // canonical out would drop it, so the conflict is reported and nothing
  // changes.
  const std::string line =
      "<https://a.test/stale>; rel=canonical, </a.css>; rel=preload; "
      "media=(max-width: 600px)";
  CanonicalLinkEdit e = Plan({line}, OnPresent::kReplace);
  ASSERT_TRUE(e.applicable);
  EXPECT_EQ(e.action, Action::kNone);
  EXPECT_EQ(e.reason, Reason::kHeaderConflict);
  EXPECT_EQ(e.defect, "canonical-header-conflict");
  EXPECT_FALSE(e.mutate);
  // A clean line next to it is still fine, and an unclean line without a
  // conflict does not stop the repair of another.
  e = Plan(
      {"<https://a.test/stale>; rel=canonical", "junk, </a.css>; rel=preload"},
      OnPresent::kReplace);
  EXPECT_TRUE(e.mutate);
  EXPECT_EQ(e.lines[0], "junk, </a.css>; rel=preload");
}

TEST(PlanCanonicalLinkHeaderTest, AnUnparsableTargetIsAConflict) {
  CanonicalLinkEdit e = Plan({"<mailto:x@a.test>; rel=canonical"});
  EXPECT_EQ(e.reason, Reason::kHeaderConflict);
}

TEST(PlanCanonicalLinkHeaderTest, ACanonicalBehindADuplicateRelIsStillSeen) {
  // A second rel parameter must not hide a canonical behind it, and a
  // repair of a duplicated rel keeps the remaining tokens.
  CanonicalLinkEdit e =
      Plan({"<https://a.test/stale>; rel=\"next\"; rel=\"canonical\""},
           OnPresent::kRepair);
  ASSERT_TRUE(e.applicable);
  EXPECT_EQ(e.action, Action::kReplace);
  EXPECT_EQ(e.reason, Reason::kHeaderConflict);
  EXPECT_EQ(e.lines, (std::vector<std::string>{
                         "<https://a.test/stale>; rel=\"next\"",
                         "<https://a.test/page>; rel=\"canonical\""}));

  e = Plan({"<https://a.test/stale>; rel=\"canonical\"; rel=\"next\""},
           OnPresent::kRepair);
  EXPECT_EQ(e.action, Action::kReplace);
  EXPECT_EQ(e.lines, (std::vector<std::string>{
                         "<https://a.test/stale>; rel=\"next\"",
                         "<https://a.test/page>; rel=\"canonical\""}));
}

TEST(PlanCanonicalLinkHeaderTest, ACommaInTheTargetStaysOneEntry) {
  CanonicalLinkEdit e = PlanCanonicalLinkHeader(
      {"<https://a.test/a,b>; rel=canonical"}, kPage, "https://a.test/a,b",
      "https://a.test/a,b", "https://a.test/a,b", OnPresent::kKeep);
  EXPECT_EQ(e.reason, Reason::kEqual);
}

TEST(PlanCanonicalLinkHeaderTest, NoValueIsWrittenWhenTheUrlCannotBeEncoded) {
  const std::string long_url =
      "https://a.test/" + std::string(kMaxUrlBytes - 15, ' ');
  CanonicalLinkEdit e = PlanCanonicalLinkHeader({}, kPage, long_url, long_url,
                                                long_url, OnPresent::kKeep);
  EXPECT_FALSE(e.applicable);
  // A conflict that cannot be repaired is reported and the header stays.
  e = PlanCanonicalLinkHeader({"<https://a.test/stale>; rel=canonical"}, kPage,
                              long_url, long_url, long_url,
                              OnPresent::kReplace);
  EXPECT_TRUE(e.applicable);
  EXPECT_EQ(e.action, Action::kNone);
  EXPECT_EQ(e.reason, Reason::kHeaderConflict);
  EXPECT_FALSE(e.mutate);
}

}  // namespace
}  // namespace pagespeed::packs
