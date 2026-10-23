// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

// Pure planner tests: facts in, plan out, no HTML parsing.

#include "pagespeed/kernel/packs/planner.h"

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "absl/strings/str_cat.h"
#include "gtest/gtest.h"
#include "pagespeed/kernel/html/html_keywords.h"
#include "pagespeed/kernel/packs/decision.h"
#include "pagespeed/kernel/packs/json_util.h"
#include "pagespeed/kernel/packs/kind_registry.h"
#include "pagespeed/kernel/packs/pack_loader.h"
#include "pagespeed/kernel/packs/page_facts.h"
#include "pagespeed/kernel/packs/planner_common.h"
#include "pagespeed/kernel/packs/url_norm.h"

namespace pagespeed::packs {
namespace {

Pack MakePack(
    std::string_view kind, std::string_view on_present,
    std::string_view value = "{\"template\":\"https://t.test{path}\"}",
    std::string_view extra = "") {
  std::string json = absl::StrCat(
      R"({"pack":{"id":"edge-seo","version":"0.1.0"},)",
      R"("sites":[{"host":"t.test","mode":"enforce"}],"rules":[{"id":"r1",)",
      R"("kind":")", kind, R"(","enforce":true,"value":)", value,
      R"(,"on_present":")", on_present, R"("})", extra, "]}");
  auto p = LoadPack(json, "");
  EXPECT_TRUE(p.ok()) << p.status() << "\n" << json;
  return p.ok() ? *std::move(p) : Pack();
}

PageContext Ctx(std::string_view path = "/x") {
  PageContext c;
  c.url = {"https", "t.test", std::string(path), ""};
  c.global_mode = Mode::kEnforce;
  return c;
}

// A fake element handle: the planner never dereferences it.
net_instaweb::HtmlElement* Handle(int n) {
  // NOLINTNEXTLINE(performance-no-int-to-ptr): intentional fake handle
  return reinterpret_cast<net_instaweb::HtmlElement*>(
      static_cast<uintptr_t>(n * 16));
}

ElementFact Fact(int id, std::string value, bool in_head = true) {
  ElementFact f;
  f.element = Handle(id);
  f.in_head = in_head;
  f.value = std::move(value);
  f.has_value_attr = true;
  return f;
}

const RuleKind* Canonical() { return FindKind("canonical"); }
const RuleKind* Title() { return FindKind("title"); }
const RuleKind* Description() { return FindKind("description"); }

PageFacts Facts() {
  PageFacts f;
  f.head = Handle(100);
  return f;
}

RulePlan Only(const PagePlan& plan) {
  EXPECT_EQ(plan.rules.size(), 1u);
  return plan.rules.empty() ? RulePlan() : plan.rules.front();
}

class PlannerTest : public testing::Test {
 protected:
  void SetUp() override { net_instaweb::HtmlKeywords::Init(); }
};

TEST_F(PlannerTest, HostNotListedSkipsThePage) {
  Pack pack = MakePack("canonical", "keep");
  PageContext ctx = Ctx();
  ctx.url.host = "other.test";
  PagePlan plan = BuildPlan(pack, ctx, Facts());
  EXPECT_EQ(plan.skip, SkipReason::kHostNotListed);
  EXPECT_TRUE(plan.rules.empty());
}

TEST_F(PlannerTest, NoHeadSkipsThePage) {
  Pack pack = MakePack("canonical", "keep");
  PageFacts facts;  // head == nullptr
  EXPECT_EQ(BuildPlan(pack, Ctx(), facts).skip, SkipReason::kNoHead);
}

TEST_F(PlannerTest, MalformedHeadSkipsThePage) {
  Pack pack = MakePack("canonical", "keep");
  PageFacts facts = Facts();
  facts.malformed_head = true;
  EXPECT_EQ(BuildPlan(pack, Ctx(), facts).skip, SkipReason::kMalformedHead);
}

TEST_F(PlannerTest, OnlyTheKindsOfTheDecisionPointArePlanned) {
  Pack pack = MakePack("canonical", "keep");
  EXPECT_EQ(Only(BuildPlan(pack, Ctx(), Facts(), DecisionPoint::kHeadEnd))
                .decision.kind,
            Canonical());
  // No canonical rule decides at the end of the document, so nothing is
  // planned there (and no head is needed).
  PagePlan end = BuildPlan(pack, Ctx(), PageFacts(), DecisionPoint::kDocumentEnd);
  EXPECT_EQ(end.skip, SkipReason::kNone);
  EXPECT_TRUE(end.rules.empty());
}

TEST_F(PlannerTest, KeepInsertsOnlyWhenAbsent) {
  Pack pack = MakePack("canonical", "keep");
  const RulePlan rp = Only(BuildPlan(pack, Ctx(), Facts()));
  ASSERT_EQ(rp.ops.size(), 1u);
  EXPECT_EQ(rp.ops[0].type, OpType::kInsert);
  EXPECT_EQ(rp.ops[0].kind, Canonical());
  EXPECT_EQ(rp.ops[0].value, "https://t.test/x");
  EXPECT_EQ(rp.ops[0].attr, net_instaweb::HtmlName::kHref);
  EXPECT_EQ(rp.decision.action, Action::kInsert);
  EXPECT_EQ(rp.decision.reason, Reason::kAbsent);
  EXPECT_EQ(rp.decision.defect, "canonical-missing");
  EXPECT_EQ(rp.decision.after, "https://t.test/x");
  EXPECT_TRUE(rp.has_target);
  EXPECT_EQ(rp.target_norm, "https://t.test/x");
}

TEST_F(PlannerTest, KeepLeavesEveryPresentFormAlone) {
  Pack pack = MakePack("canonical", "keep");
  for (const auto& [value, in_head] :
       {std::pair{"https://other.test/", true}, std::pair{"", true},
        std::pair{"javascript:void(0)", true},
        std::pair{"https://other.test/", false}}) {
    PageFacts facts = Facts();
    facts.For(Canonical()).push_back(Fact(1, value, in_head));
    const RulePlan rp = Only(BuildPlan(pack, Ctx(), facts));
    EXPECT_TRUE(rp.ops.empty()) << value;
    EXPECT_EQ(rp.decision.action, Action::kNone) << value;
  }
}

TEST_F(PlannerTest, KeepRemovesOnlyExactDuplicates) {
  Pack pack = MakePack("canonical", "keep");
  PageFacts facts = Facts();
  facts.For(Canonical()).push_back(Fact(1, "https://a.test/p"));
  facts.For(Canonical()).push_back(Fact(2, "https://A.test/p/"));
  facts.For(Canonical()).push_back(Fact(3, "https://b.test/p"));
  const RulePlan rp = Only(BuildPlan(pack, Ctx(), facts));
  ASSERT_EQ(rp.ops.size(), 1u);
  EXPECT_EQ(rp.ops[0].type, OpType::kRemove);
  EXPECT_EQ(rp.ops[0].target.element, Handle(2));
  EXPECT_EQ(rp.decision.action, Action::kDedupe);
  EXPECT_EQ(rp.decision.defect, "canonical-multiple");  // b.test remains
}

TEST_F(PlannerTest, KeepPrefersTheDuplicateInHead) {
  Pack pack = MakePack("canonical", "keep");
  PageFacts facts = Facts();
  facts.For(Canonical()).push_back(Fact(1, "https://a.test/p", false));
  facts.For(Canonical()).push_back(Fact(2, "https://a.test/p", true));
  const RulePlan rp = Only(BuildPlan(pack, Ctx(), facts));
  ASSERT_EQ(rp.ops.size(), 1u);
  EXPECT_EQ(rp.ops[0].target.element, Handle(1));
}

TEST_F(PlannerTest, KeepReportsConflictWithoutChange) {
  Pack pack = MakePack("canonical", "keep");
  PageFacts facts = Facts();
  facts.For(Canonical()).push_back(Fact(1, "https://a.test/p"));
  facts.For(Canonical()).push_back(Fact(2, "https://b.test/p"));
  const RulePlan rp = Only(BuildPlan(pack, Ctx(), facts));
  EXPECT_TRUE(rp.ops.empty());
  EXPECT_EQ(rp.decision.reason, Reason::kConflict);
}

TEST_F(PlannerTest, RepairKeepsTheFirstUsableAndDropsTheRest) {
  Pack pack = MakePack("canonical", "repair");
  PageFacts facts = Facts();
  facts.For(Canonical()).push_back(Fact(1, "https://a.test/p"));
  facts.For(Canonical()).push_back(Fact(2, "https://b.test/p"));
  facts.For(Canonical()).push_back(Fact(3, "https://c.test/p", false));
  const RulePlan rp = Only(BuildPlan(pack, Ctx(), facts));
  ASSERT_EQ(rp.ops.size(), 2u);
  EXPECT_EQ(rp.ops[0].target.element, Handle(2));
  EXPECT_EQ(rp.ops[1].target.element, Handle(3));
  EXPECT_EQ(rp.decision.reason, Reason::kConflict);
  EXPECT_EQ(rp.decision.defect, "canonical-multiple");
}

TEST_F(PlannerTest, RepairFillsAnEmptyElementInHeadInPlace) {
  Pack pack = MakePack("canonical", "repair");
  PageFacts facts = Facts();
  facts.For(Canonical()).push_back(Fact(1, ""));
  const RulePlan rp = Only(BuildPlan(pack, Ctx(), facts));
  ASSERT_EQ(rp.ops.size(), 1u);
  EXPECT_EQ(rp.ops[0].type, OpType::kSetValue);
  EXPECT_EQ(rp.decision.action, Action::kFill);
  EXPECT_EQ(rp.decision.reason, Reason::kEmpty);
  EXPECT_EQ(rp.decision.defect, "canonical-unusable");
}

TEST_F(PlannerTest, RepairMovesAnOutOfHeadElementIntoHead) {
  Pack pack = MakePack("canonical", "repair");
  PageFacts facts = Facts();
  facts.For(Canonical()).push_back(Fact(1, "https://a.test/p", false));
  const RulePlan rp = Only(BuildPlan(pack, Ctx(), facts));
  ASSERT_EQ(rp.ops.size(), 2u);
  EXPECT_EQ(rp.ops[0].type, OpType::kRemove);
  EXPECT_EQ(rp.ops[1].type, OpType::kInsert);
  EXPECT_EQ(rp.decision.reason, Reason::kOutOfHead);
}

TEST_F(PlannerTest, RepairLeavesAUsableElementAloneWhateverItsValue) {
  Pack pack = MakePack("canonical", "repair");
  PageFacts facts = Facts();
  facts.For(Canonical()).push_back(Fact(1, "https://other.test/"));
  const RulePlan rp = Only(BuildPlan(pack, Ctx(), facts));
  EXPECT_TRUE(rp.ops.empty());
  EXPECT_EQ(rp.decision.reason, Reason::kPresent);
}

TEST_F(PlannerTest, RepairResolvesRelativeHrefsAgainstBase) {
  Pack pack = MakePack("canonical", "repair");
  PageFacts facts = Facts();
  facts.base_href = "https://t.test/dir/";
  facts.For(Canonical()).push_back(Fact(1, "x"));
  const RulePlan rp = Only(BuildPlan(pack, Ctx("/dir/x"), facts));
  EXPECT_TRUE(rp.ops.empty());
  EXPECT_EQ(rp.decision.reason, Reason::kEqual);
}

TEST_F(PlannerTest, ReplaceLeavesAnEqualElementUntouched) {
  Pack pack = MakePack("canonical", "replace");
  PageFacts facts = Facts();
  facts.For(Canonical()).push_back(Fact(1, "https://t.test/x/"));
  const RulePlan rp = Only(BuildPlan(pack, Ctx(), facts));
  EXPECT_TRUE(rp.ops.empty());
  EXPECT_EQ(rp.decision.reason, Reason::kEqual);
}

TEST_F(PlannerTest, ReplaceRewritesTheFirstInHeadElementAndRemovesTheRest) {
  Pack pack = MakePack("canonical", "replace");
  PageFacts facts = Facts();
  facts.For(Canonical()).push_back(Fact(1, "https://a.test/"));
  facts.For(Canonical()).push_back(Fact(2, "https://b.test/"));
  const RulePlan rp = Only(BuildPlan(pack, Ctx(), facts));
  ASSERT_EQ(rp.ops.size(), 2u);
  EXPECT_EQ(rp.ops[0].type, OpType::kSetValue);
  EXPECT_EQ(rp.ops[0].target.element, Handle(1));
  EXPECT_EQ(rp.ops[1].type, OpType::kRemove);
  EXPECT_EQ(rp.decision.action, Action::kReplace);
}

TEST_F(PlannerTest, ReplaceKeepsAnEqualElementWhenOthersDiffer) {
  Pack pack = MakePack("canonical", "replace");
  PageFacts facts = Facts();
  facts.For(Canonical()).push_back(Fact(1, "https://a.test/"));
  facts.For(Canonical()).push_back(Fact(2, "https://t.test/x"));
  const RulePlan rp = Only(BuildPlan(pack, Ctx(), facts));
  ASSERT_EQ(rp.ops.size(), 1u);
  EXPECT_EQ(rp.ops[0].type, OpType::kRemove);
  EXPECT_EQ(rp.ops[0].target.element, Handle(1));
}

TEST_F(PlannerTest, ExtraRelTokensAreKeptOnRemoval) {
  Pack pack = MakePack("canonical", "repair");
  PageFacts facts = Facts();
  facts.For(Canonical()).push_back(Fact(1, "https://a.test/"));
  ElementFact second = Fact(2, "https://b.test/");
  second.other_rel_tokens = {"alternate", "x"};
  facts.For(Canonical()).push_back(second);
  const RulePlan rp = Only(BuildPlan(pack, Ctx(), facts));
  ASSERT_EQ(rp.ops.size(), 1u);
  EXPECT_EQ(rp.ops[0].type, OpType::kRemoveRelToken);
  EXPECT_EQ(rp.ops[0].value, "alternate x");
}

TEST_F(PlannerTest, TitleUsesTheEscapedTextAndComparesDecoded) {
  Pack pack = MakePack("title", "replace", R"({"template":"Fish & Chips"})");
  PageFacts facts = Facts();
  facts.For(Title()).push_back(Fact(1, "Fish & Chips"));  // value is decoded
  const RulePlan rp = Only(BuildPlan(pack, Ctx(), facts));
  EXPECT_TRUE(rp.ops.empty());  // already equal after decoding
  EXPECT_EQ(rp.decision.reason, Reason::kEqual);

  const RulePlan ins = Only(BuildPlan(pack, Ctx(), Facts()));
  ASSERT_EQ(ins.ops.size(), 1u);
  EXPECT_EQ(ins.ops[0].value, "Fish &amp; Chips");
  // The title's value is its text, not an attribute.
  EXPECT_EQ(ins.ops[0].attr, net_instaweb::HtmlName::kNotAKeyword);
}

TEST_F(PlannerTest, TitleRepairFillsABlankTitle) {
  Pack pack = MakePack("title", "repair", R"({"template":"T {path}"})");
  PageFacts facts = Facts();
  facts.For(Title()).push_back(Fact(1, "  \n "));
  const RulePlan rp = Only(BuildPlan(pack, Ctx(), facts));
  EXPECT_EQ(rp.decision.action, Action::kFill);
  EXPECT_EQ(rp.decision.defect, "title-missing");
  ASSERT_FALSE(rp.ops.empty());
  EXPECT_EQ(rp.ops[0].value, "T /x");
}

TEST_F(PlannerTest, DescriptionKeepDedupesAfterWhitespaceCollapse) {
  Pack pack = MakePack("description", "keep", R"({"template":"D"})");
  PageFacts facts = Facts();
  facts.For(Description()).push_back(Fact(1, "a  b"));
  facts.For(Description()).push_back(Fact(2, " a b "));
  const RulePlan rp = Only(BuildPlan(pack, Ctx(), facts));
  ASSERT_EQ(rp.ops.size(), 1u);
  EXPECT_EQ(rp.decision.action, Action::kDedupe);
  EXPECT_TRUE(rp.decision.defect.empty());
}

TEST_F(PlannerTest, InvalidExpandedUrlIsSkippedInEveryMode) {
  Pack pack = MakePack("canonical", "replace", R"({"template":"{path}"})");
  PageFacts facts = Facts();
  facts.For(Canonical()).push_back(Fact(1, "https://a.test/"));
  const RulePlan rp = Only(BuildPlan(pack, Ctx(), facts));
  EXPECT_TRUE(rp.ops.empty());
  EXPECT_EQ(rp.decision.reason, Reason::kInvalidValue);
  EXPECT_FALSE(rp.has_target);
}

TEST_F(PlannerTest, ATableMissWithoutFallbackHasNoValue) {
  auto loaded = LoadPack(
      R"({"pack":{"id":"edge-seo","version":"0.1.0"},)"
      R"("sites":[{"host":"t.test","mode":"enforce"}],)"
      R"("tables":{"t":{"/x":"https://t.test/from-table"}},)"
      R"("rules":[{"id":"r1","kind":"canonical","enforce":true,)"
      R"("value":{"table":"t"}}]})",
      "");
  ASSERT_TRUE(loaded.ok()) << loaded.status();
  const RulePlan rp = Only(BuildPlan(*loaded, Ctx("/other"), Facts()));
  EXPECT_EQ(rp.decision.reason, Reason::kNoValue);
  const RulePlan hit = Only(BuildPlan(*loaded, Ctx(), Facts()));
  ASSERT_EQ(hit.ops.size(), 1u);
  EXPECT_EQ(hit.ops[0].value, "https://t.test/from-table");
}

TEST_F(PlannerTest, EffectiveModeIsTheLowestOfTheThree) {
  Pack pack = MakePack("canonical", "keep");
  PageContext ctx = Ctx();
  ctx.global_mode = Mode::kReport;
  EXPECT_EQ(Only(BuildPlan(pack, ctx, Facts())).decision.mode, Mode::kReport);
  ctx.global_mode = Mode::kEnforce;
  EXPECT_EQ(Only(BuildPlan(pack, ctx, Facts())).decision.mode, Mode::kEnforce);
}

TEST_F(PlannerTest, SizeLimitDropsTheWholePlan) {
  Pack pack = MakePack("canonical", "keep");
  PagePlan plan = BuildPlan(pack, Ctx(), Facts(), DecisionPoint::kHeadEnd,
                            /*max_added_bytes=*/10);
  EXPECT_EQ(plan.skip, SkipReason::kSizeLimit);
  EXPECT_TRUE(plan.rules.empty());
}

TEST_F(PlannerTest, DecisionsHashWhatWasOnThePageAndQuoteWhatTheRuleSets) {
  Pack pack = MakePack("canonical", "replace");
  PageFacts facts = Facts();
  facts.For(Canonical()).push_back(Fact(1, "https://secret.test/page"));
  const RulePlan rp = Only(BuildPlan(pack, Ctx(), facts));
  const std::string line = DecisionToJsonLine("t.test", "/x", rp.decision);
  EXPECT_EQ(line.find("secret"), std::string::npos);
  EXPECT_NE(line.find("\"after\":\"https://t.test/x\""), std::string::npos);
  EXPECT_FALSE(rp.decision.before_hash.empty());
  EXPECT_NE(line.find("fnv1a64:"), std::string::npos);
}

// ---------------------------------------------------------------------------
// Late elements
// ---------------------------------------------------------------------------

struct Late {
  std::shared_ptr<const Pack> pack;  // the plan points into it
  RulePlan plan;
  bool inserted = false;
  std::vector<ElementFact> earlier;
};

Late Decide(Pack pack, const PageFacts& facts) {
  Late l;
  l.pack = std::make_shared<const Pack>(std::move(pack));
  l.plan = Only(BuildPlan(*l.pack, Ctx(), facts));
  for (const PlanOp& op : l.plan.ops) {
    if (op.type == OpType::kInsert) l.inserted = true;
  }
  l.earlier = facts.For(l.plan.decision.kind);
  return l;
}

LateAction OnLate(const Late& l, const ElementFact& late) {
  PlanState state;
  state.plan = &l.plan;
  state.inserted = l.inserted;
  state.base = "https://t.test/x";
  state.earlier = l.earlier;
  return l.plan.decision.kind->OnLateElement(state, late);
}

TEST_F(PlannerTest, LateElementAfterKeepInsertedIsRemoved) {
  Late l = Decide(MakePack("canonical", "keep"), Facts());
  ASSERT_TRUE(l.inserted);
  LateAction other = OnLate(l, Fact(9, "https://other.test/", false));
  EXPECT_EQ(other.type, LateAction::kRemove);
  EXPECT_TRUE(other.conflict);
  EXPECT_EQ(other.norm, "https://other.test");
  LateAction same = OnLate(l, Fact(9, "https://t.test/x/", false));
  EXPECT_EQ(same.type, LateAction::kRemove);
  EXPECT_FALSE(same.conflict);
}

TEST_F(PlannerTest, LateElementUnderKeepWithAHeadValueOnlyLosesDuplicates) {
  PageFacts facts = Facts();
  facts.For(Canonical()).push_back(Fact(1, "https://a.test/p"));
  Late l = Decide(MakePack("canonical", "keep"), facts);
  ASSERT_FALSE(l.inserted);
  EXPECT_EQ(OnLate(l, Fact(9, "https://b.test/p", false)).type,
            LateAction::kLeave);
  EXPECT_EQ(OnLate(l, Fact(9, "https://A.test/p/", false)).type,
            LateAction::kRemove);
  EXPECT_EQ(OnLate(l, Fact(9, "", false)).type, LateAction::kLeave);
}

TEST_F(PlannerTest, LateElementUnderRepairOrReplaceIsRemoved) {
  for (std::string_view rung : {"repair", "replace"}) {
    PageFacts facts = Facts();
    facts.For(Canonical()).push_back(Fact(1, "https://t.test/x"));
    Late l = Decide(MakePack("canonical", rung), facts);
    EXPECT_EQ(OnLate(l, Fact(9, "https://t.test/x", false)).type,
              LateAction::kRemove)
        << rung;
    ElementFact alt = Fact(9, "https://b.test/", false);
    alt.other_rel_tokens = {"alternate"};
    EXPECT_EQ(OnLate(l, alt).type, LateAction::kRemoveRelToken) << rung;
  }
}

TEST_F(PlannerTest, LateElementIsLeftAloneWhenTheRuleHasNoValue) {
  Late l = Decide(MakePack("canonical", "repair", R"({"template":"{path}"})"),
                  Facts());
  EXPECT_EQ(OnLate(l, Fact(9, "https://b.test/", false)).type,
            LateAction::kLeave);
}

// ---------------------------------------------------------------------------
// hreflang
// ---------------------------------------------------------------------------

constexpr char kCluster[] =
    R"({"pattern":{"en":"https://t.test{path}","de":"https://t.test/de{path}",)"
    R"("x-default":"https://t.test{path}"}})";

const RuleKind* Hreflang() { return FindKind("hreflang"); }
const RuleKind* JsonLd() { return FindKind("jsonld"); }

ElementFact HFact(int id, std::string code, std::string href,
                  bool in_head = true) {
  ElementFact f;
  f.element = Handle(id);
  f.in_head = in_head;
  f.code = std::move(code);
  f.value = std::move(href);
  f.has_value_attr = true;
  return f;
}

std::vector<std::string> InsertedCodes(const RulePlan& rp) {
  std::vector<std::string> out;
  for (const PlanOp& op : rp.ops) {
    if (op.type == OpType::kInsert) out.push_back(op.code);
  }
  return out;
}

TEST_F(PlannerTest, HreflangKeepInsertsTheWholeClusterWhenAbsent) {
  Pack pack = MakePack("hreflang", "keep", kCluster);
  const RulePlan rp = Only(BuildPlan(pack, Ctx(), Facts()));
  EXPECT_EQ(InsertedCodes(rp),
            (std::vector<std::string>{"de", "en", "x-default"}));
  EXPECT_EQ(rp.decision.action, Action::kInsert);
  EXPECT_EQ(rp.decision.reason, Reason::kAbsent);
  EXPECT_EQ(rp.decision.defect, "");
  EXPECT_EQ(rp.decision.after,
            "de=https://t.test/de/x, en=https://t.test/x, "
            "x-default=https://t.test/x");
  for (const PlanOp& op : rp.ops) {
    EXPECT_EQ(op.attr, net_instaweb::HtmlName::kHref);
    EXPECT_EQ(op.kind, Hreflang());
  }
}

TEST_F(PlannerTest, HreflangKeepObservesAndNeverChangesAPresentCluster) {
  Pack pack = MakePack("hreflang", "keep", kCluster);
  PageFacts facts = Facts();
  facts.For(Hreflang()).push_back(HFact(1, "en", "https://other.test/en"));
  facts.For(Hreflang()).push_back(HFact(2, "xx_YY", "/relative"));
  const RulePlan rp = Only(BuildPlan(pack, Ctx(), facts));
  EXPECT_TRUE(rp.ops.empty());
  EXPECT_EQ(rp.decision.action, Action::kNone);
  EXPECT_EQ(rp.decision.defect, "hreflang-invalid");
}

TEST_F(PlannerTest, HreflangKeepRemovesOnlyExactDuplicates) {
  Pack pack = MakePack("hreflang", "keep", kCluster);
  PageFacts facts = Facts();
  facts.For(Hreflang()).push_back(HFact(1, "en", "https://t.test/x"));
  facts.For(Hreflang()).push_back(HFact(2, "EN", "https://T.test/x/"));
  facts.For(Hreflang()).push_back(HFact(3, "en", "https://t.test/other"));
  const RulePlan rp = Only(BuildPlan(pack, Ctx(), facts));
  ASSERT_EQ(rp.ops.size(), 1u);
  EXPECT_EQ(rp.ops[0].type, OpType::kRemove);
  EXPECT_EQ(rp.ops[0].target.element, Handle(2));
  EXPECT_EQ(rp.decision.action, Action::kDedupe);
}

TEST_F(PlannerTest, HreflangRepairNeverDeletesTheEntryForThePage) {
  Pack pack = MakePack("hreflang", "repair", kCluster);
  // Two entries for "en": the first points elsewhere, the second at the page.
  PageFacts facts = Facts();
  facts.For(Hreflang()).push_back(HFact(1, "en", "https://other.test/en"));
  facts.For(Hreflang()).push_back(HFact(2, "en", "https://t.test/x"));
  const RulePlan rp = Only(BuildPlan(pack, Ctx(), facts));
  for (const PlanOp& op : rp.ops) {
    if (op.type == OpType::kRemove) {
      EXPECT_EQ(op.target.element, Handle(1));
    }
  }
  bool removes_first = false;
  for (const PlanOp& op : rp.ops) {
    removes_first |=
        op.type == OpType::kRemove && op.target.element == Handle(1);
  }
  EXPECT_TRUE(removes_first);
  // The missing codes are added; "en" is not.
  EXPECT_EQ(InsertedCodes(rp), (std::vector<std::string>{"de", "x-default"}));
}

TEST_F(PlannerTest, HreflangRepairPointsASelfCodeThatPointsElsewhereAtThePage) {
  Pack pack = MakePack(
      "hreflang", "repair",
      R"({"pattern":{"en":"https://t.test{path}","de":"https://t.test/de{path}"}})");
  PageFacts facts = Facts();
  facts.For(Hreflang()).push_back(HFact(1, "en", "https://other.test/en"));
  facts.For(Hreflang()).push_back(HFact(2, "de", "https://t.test/de/x"));
  const RulePlan rp = Only(BuildPlan(pack, Ctx(), facts));
  ASSERT_EQ(rp.ops.size(), 1u);
  EXPECT_EQ(rp.ops[0].type, OpType::kSetValue);
  EXPECT_EQ(rp.ops[0].target.element, Handle(1));
  EXPECT_EQ(rp.ops[0].value, "https://t.test/x");
  EXPECT_EQ(rp.ops[0].attr, net_instaweb::HtmlName::kHref);
  EXPECT_EQ(rp.decision.action, Action::kReplace);
}

TEST_F(PlannerTest, HreflangRepairDropsWhatIsUnusableAndLeavesTheRest) {
  Pack pack = MakePack("hreflang", "repair", kCluster);
  PageFacts facts = Facts();
  facts.For(Hreflang()).push_back(HFact(1, "en", "https://t.test/x"));
  facts.For(Hreflang()).push_back(HFact(2, "de", "https://t.test/de/x"));
  facts.For(Hreflang()).push_back(HFact(3, "x-default", "https://t.test/x"));
  facts.For(Hreflang()).push_back(HFact(4, "en_US", "https://t.test/x"));
  facts.For(Hreflang()).push_back(HFact(5, "fr", "/relative"));
  facts.For(Hreflang()).push_back(HFact(6, "fr", "", true));
  facts.For(Hreflang()).push_back(HFact(7, "it", "https://t.test/it", false));
  const RulePlan rp = Only(BuildPlan(pack, Ctx(), facts));
  std::vector<void*> removed;
  for (const PlanOp& op : rp.ops) {
    ASSERT_EQ(op.type, OpType::kRemove);
    removed.push_back(op.target.element);
  }
  EXPECT_EQ(removed, (std::vector<void*>{Handle(4), Handle(5), Handle(6),
                                         Handle(7)}));
  EXPECT_EQ(rp.decision.action, Action::kRemove);
  EXPECT_EQ(rp.decision.defect, "hreflang-invalid");
  EXPECT_EQ(rp.decision.removed,
            "en_US=https://t.test/x (unusable); fr=/relative (unusable); "
            "fr= (empty); it=https://t.test/it (out_of_head)");
}

TEST_F(PlannerTest, HreflangReplaceKeepsEqualEntriesAndRewritesTheRest) {
  Pack pack = MakePack("hreflang", "replace", kCluster);
  PageFacts facts = Facts();
  facts.For(Hreflang()).push_back(HFact(1, "en", "https://t.test/x"));
  facts.For(Hreflang()).push_back(HFact(2, "de", "https://t.test/old"));
  facts.For(Hreflang()).push_back(HFact(3, "fr", "https://t.test/fr"));
  const RulePlan rp = Only(BuildPlan(pack, Ctx(), facts));
  int sets = 0, removes = 0, inserts = 0;
  for (const PlanOp& op : rp.ops) {
    sets += op.type == OpType::kSetValue;
    removes += op.type == OpType::kRemove;
    inserts += op.type == OpType::kInsert;
    if (op.type == OpType::kSetValue) {
      EXPECT_EQ(op.target.element, Handle(2));
      EXPECT_EQ(op.value, "https://t.test/de/x");
    }
    if (op.type == OpType::kRemove) EXPECT_EQ(op.target.element, Handle(3));
  }
  EXPECT_EQ(sets, 1);
  EXPECT_EQ(removes, 1);
  EXPECT_EQ(inserts, 1);  // x-default
}

TEST_F(PlannerTest, HreflangClusterWithoutAnEntryForThePageIsSkipped) {
  Pack pack = MakePack(
      "hreflang", "keep",
      R"({"pattern":{"en":"https://t.test/elsewhere","de":"https://t.test/de"}})");
  const RulePlan rp = Only(BuildPlan(pack, Ctx(), Facts()));
  EXPECT_TRUE(rp.ops.empty());
  EXPECT_EQ(rp.decision.reason, Reason::kClusterWithoutSelf);
  EXPECT_FALSE(rp.has_target);
}

TEST_F(PlannerTest, HreflangInvalidPatternValueIsSkipped) {
  Pack pack =
      MakePack("hreflang", "keep", R"({"pattern":{"en":"{path}"}})");
  const RulePlan rp = Only(BuildPlan(pack, Ctx(), Facts()));
  EXPECT_TRUE(rp.ops.empty());
  EXPECT_EQ(rp.decision.reason, Reason::kInvalidValue);
}

TEST_F(PlannerTest, HreflangTableComesFromTheClusterOfThePage) {
  const std::string json =
      R"({"pack":{"id":"edge-seo","version":"0.1.0"},)"
      R"("sites":[{"host":"t.test","mode":"enforce"}],)"
      R"("tables":{"hreflang":[{"en":"https://t.test/en/","de":"https://t.test/de/"}]},)"
      R"("rules":[{"id":"r1","kind":"hreflang","enforce":true,)"
      R"("value":{"table":"hreflang"}}]})";
  auto loaded = LoadPack(json, "");
  ASSERT_TRUE(loaded.ok()) << loaded.status();
  Pack pack = *std::move(loaded);
  RulePlan hit = Only(BuildPlan(pack, Ctx("/en/"), Facts()));
  EXPECT_EQ(InsertedCodes(hit), (std::vector<std::string>{"de", "en"}));
  RulePlan miss = Only(BuildPlan(pack, Ctx("/other"), Facts()));
  EXPECT_EQ(miss.decision.reason, Reason::kNoValue);
}

TEST_F(PlannerTest, EveryEmittedHreflangCodeIsValidAndSelfIsPresent) {
  for (std::string_view rung : {"keep", "repair", "replace"}) {
    Pack pack = MakePack("hreflang", rung, kCluster);
    const RulePlan rp = Only(BuildPlan(pack, Ctx(), Facts()));
    bool self = false;
    for (const PlanOp& op : rp.ops) {
      if (op.type != OpType::kInsert && op.type != OpType::kSetValue) continue;
      EXPECT_EQ(NormalizeHreflangCode(op.code), op.code) << rung;
      self |= NormUrl(op.value) == NormUrl("https://t.test/x");
    }
    EXPECT_TRUE(self) << rung;
  }
}

TEST_F(PlannerTest, HreflangLateLinkRules) {
  // repair / replace: a late link is out of <head>, so it goes.
  for (std::string_view rung : {"repair", "replace"}) {
    Late l = Decide(MakePack("hreflang", rung, kCluster), Facts());
    EXPECT_EQ(OnLate(l, HFact(9, "fr", "https://t.test/fr", false)).type,
              LateAction::kRemove)
        << rung;
  }
  // keep, the pack inserted the cluster: a late link for a code of the
  // cluster would be a second answer; any other code is left alone.
  Late l = Decide(MakePack("hreflang", "keep", kCluster), Facts());
  ASSERT_TRUE(l.inserted);
  LateAction conflict =
      OnLate(l, HFact(9, "de", "https://t.test/other-de", false));
  EXPECT_EQ(conflict.type, LateAction::kRemove);
  EXPECT_TRUE(conflict.conflict);
  LateAction same = OnLate(l, HFact(9, "EN", "https://t.test/x/", false));
  EXPECT_EQ(same.type, LateAction::kRemove);
  EXPECT_FALSE(same.conflict);
  EXPECT_EQ(OnLate(l, HFact(9, "fr", "https://t.test/fr", false)).type,
            LateAction::kLeave);
  EXPECT_EQ(OnLate(l, HFact(9, "en_US", "https://t.test/x", false)).type,
            LateAction::kLeave);
  // keep, the head already had links: late ones are only observed.
  PageFacts facts = Facts();
  facts.For(Hreflang()).push_back(HFact(1, "en", "https://t.test/x"));
  Late present = Decide(MakePack("hreflang", "keep", kCluster), facts);
  ASSERT_FALSE(present.inserted);
  EXPECT_EQ(OnLate(present, HFact(9, "en", "https://t.test/x", false)).type,
            LateAction::kLeave);
}

// ---------------------------------------------------------------------------
// jsonld
// ---------------------------------------------------------------------------

constexpr char kOrg[] =
    R"({"template":"{\"@context\":\"https://schema.org\",\"@type\":\"Organization\",\"name\":\"T\"}"})";
constexpr char kOrgText[] =
    R"({"@context":"https://schema.org","@type":"Organization","name":"T"})";

ElementFact JFact(int id, std::string text, bool in_head = false) {
  ElementFact f;
  f.element = Handle(id);
  f.in_head = in_head;
  f.value = std::move(text);
  f.text_mode = TextMode::kRaw;
  return f;
}

PagePlan DocumentPlan(const Pack& pack, const PageFacts& facts,
                      PageContext ctx = Ctx()) {
  return BuildPlan(pack, ctx, facts, DecisionPoint::kDocumentEnd);
}

TEST_F(PlannerTest, JsonLdIsPlannedOnlyAtTheEndOfTheDocument) {
  Pack pack = MakePack("jsonld", "keep", kOrg);
  EXPECT_TRUE(BuildPlan(pack, Ctx(), Facts(), DecisionPoint::kHeadEnd)
                  .rules.empty());
  const RulePlan rp = Only(DocumentPlan(pack, Facts()));
  ASSERT_EQ(rp.ops.size(), 1u);
  EXPECT_EQ(rp.ops[0].type, OpType::kInsert);
  EXPECT_EQ(rp.ops[0].kind, JsonLd());
  EXPECT_EQ(rp.ops[0].value, kOrgText);
  EXPECT_EQ(rp.ops[0].attr, net_instaweb::HtmlName::kNotAKeyword);
  EXPECT_EQ(rp.decision.action, Action::kInsert);
  EXPECT_EQ(rp.decision.reason, Reason::kAbsent);
  EXPECT_EQ(rp.decision.after, "Organization");
}

TEST_F(PlannerTest, JsonLdKeepStandsDownForABlockOfTheTypeOrAnInvalidBlock) {
  Pack pack = MakePack("jsonld", "keep", kOrg);
  PageFacts facts = Facts();
  facts.For(JsonLd()).push_back(
      JFact(1, R"({"@type":"Organization","name":"Other"})"));
  RulePlan rp = Only(DocumentPlan(pack, facts));
  EXPECT_TRUE(rp.ops.empty());
  EXPECT_EQ(rp.decision.reason, Reason::kPresent);

  facts = Facts();
  facts.For(JsonLd()).push_back(JFact(1, "{not json"));
  rp = Only(DocumentPlan(pack, facts));
  EXPECT_TRUE(rp.ops.empty());
  EXPECT_EQ(rp.decision.defect, "jsonld-invalid");

  // A block of another type does not stop the insert.
  facts = Facts();
  facts.For(JsonLd()).push_back(JFact(1, R"({"@type":"Product"})"));
  rp = Only(DocumentPlan(pack, facts));
  ASSERT_EQ(rp.ops.size(), 1u);
  EXPECT_EQ(rp.ops[0].type, OpType::kInsert);
}

TEST_F(PlannerTest, JsonLdEqualBlocksAreEqualWhateverTheKeyOrder) {
  Pack pack = MakePack("jsonld", "keep", kOrg);
  PageFacts facts = Facts();
  facts.For(JsonLd()).push_back(JFact(
      1, R"( { "name" : "T", "@type":"Organization",
  "@context":"https://schema.org" } )"));
  facts.For(JsonLd()).push_back(JFact(
      2, R"({"@context":"https://schema.org","@type":"Organization","name":"T"})"));
  const RulePlan rp = Only(DocumentPlan(pack, facts));
  ASSERT_EQ(rp.ops.size(), 1u);
  EXPECT_EQ(rp.ops[0].type, OpType::kRemove);
  EXPECT_EQ(rp.ops[0].target.element, Handle(2));
  EXPECT_EQ(rp.decision.action, Action::kDedupe);
  EXPECT_EQ(rp.decision.removed, "Organization (duplicate)");
}

TEST_F(PlannerTest, JsonLdRepairRemovesInvalidAndDuplicateBlocksAndInserts) {
  Pack pack = MakePack("jsonld", "repair", kOrg);
  PageFacts facts = Facts();
  facts.For(JsonLd()).push_back(JFact(1, "{oops"));
  facts.For(JsonLd()).push_back(JFact(2, "   "));
  facts.For(JsonLd()).push_back(JFact(3, R"({"@type":"Product"})"));
  const RulePlan rp = Only(DocumentPlan(pack, facts));
  int removes = 0;
  int inserts = 0;
  for (const PlanOp& op : rp.ops) {
    removes += op.type == OpType::kRemove;
    inserts += op.type == OpType::kInsert;
  }
  EXPECT_EQ(removes, 2);
  EXPECT_EQ(inserts, 1);
  EXPECT_EQ(rp.decision.removed,
            "unparsable block (unusable); unparsable block (empty)");
}

TEST_F(PlannerTest, JsonLdReplaceRewritesInPlaceAndSparesOtherTypes) {
  Pack pack = MakePack("jsonld", "replace", kOrg);
  PageFacts facts = Facts();
  facts.For(JsonLd()).push_back(JFact(1, R"({"@type":"Product","name":"P"})"));
  facts.For(JsonLd()).push_back(
      JFact(2, R"({"@type":"Organization","name":"Old"})"));
  facts.For(JsonLd()).push_back(
      JFact(3, R"({"@type":"Organization","name":"Dup"})"));
  facts.For(JsonLd()).push_back(JFact(4, R"([{"@type":"BreadcrumbList"}])"));
  const RulePlan rp = Only(DocumentPlan(pack, facts));
  ASSERT_EQ(rp.ops.size(), 2u);
  EXPECT_EQ(rp.ops[0].type, OpType::kSetValue);
  EXPECT_EQ(rp.ops[0].target.element, Handle(2));
  EXPECT_EQ(rp.ops[0].value, kOrgText);
  EXPECT_EQ(rp.ops[1].type, OpType::kRemove);
  EXPECT_EQ(rp.ops[1].target.element, Handle(3));
  for (const PlanOp& op : rp.ops) {
    EXPECT_NE(op.target.element, Handle(1));
    EXPECT_NE(op.target.element, Handle(4));
  }
}

TEST_F(PlannerTest, JsonLdReplaceStandsDownForABlockThatCarriesOtherTypes) {
  Pack pack = MakePack("jsonld", "replace", kOrg);
  for (const char* text :
       {R"({"@graph":[{"@type":"Organization"},{"@type":"WebPage"}]})",
        R"({"@type":["Organization","LocalBusiness"]})"}) {
    PageFacts facts = Facts();
    facts.For(JsonLd()).push_back(JFact(1, text));
    const RulePlan rp = Only(DocumentPlan(pack, facts));
    EXPECT_TRUE(rp.ops.empty()) << text;
    EXPECT_EQ(rp.decision.action, Action::kNone) << text;
    EXPECT_EQ(rp.decision.reason, Reason::kConflict) << text;
  }
}

TEST_F(PlannerTest, JsonLdBlocksTooDeepOrTooLargeAreNeverTouched) {
  Pack pack = MakePack("jsonld", "repair", kOrg);
  std::string deep;
  for (size_t i = 0; i <= kMaxJsonLdDepth; ++i) deep += "[";
  deep += "1";
  for (size_t i = 0; i <= kMaxJsonLdDepth; ++i) deep += "]";
  PageFacts facts = Facts();
  facts.For(JsonLd()).push_back(JFact(1, deep));
  facts.For(JsonLd()).push_back(JFact(2, "{broken"));
  RulePlan rp = Only(DocumentPlan(pack, facts));
  EXPECT_TRUE(rp.ops.empty());
  EXPECT_EQ(rp.decision.action, Action::kNone);
  EXPECT_EQ(rp.decision.reason, Reason::kPresent);

  ElementFact big = JFact(1, "");
  big.unreadable = true;
  facts = Facts();
  facts.For(JsonLd()).push_back(big);
  rp = Only(DocumentPlan(pack, facts));
  EXPECT_TRUE(rp.ops.empty());
  EXPECT_EQ(rp.decision.reason, Reason::kPresent);
}

TEST_F(PlannerTest, JsonLdTemplateMustExpandToJsonWithAType) {
  for (const char* tpl :
       {R"({"template":"{\"@type\":\"Thing\",\"name\":\"{title}\""})",
        R"({"template":"{\"name\":\"no type\"}"})",
        R"({"template":"{\"@type\":\"Thing\",\"a\":1,\"a\":2}"})"}) {
    Pack pack = MakePack("jsonld", "keep", tpl);
    const RulePlan rp = Only(DocumentPlan(pack, Facts()));
    EXPECT_TRUE(rp.ops.empty()) << tpl;
    EXPECT_EQ(rp.decision.reason, Reason::kInvalidValue) << tpl;
  }
}

TEST_F(PlannerTest, JsonLdQuotesTheFinalHeadValuesAndEscapesThem) {
  const std::string tpl =
      R"({"template":"{\"@type\":\"WebPage\",\"name\":\"{title}\",\"url\":\"{canonical}\"}"})";
  Pack pack = MakePack(
      "jsonld", "keep", tpl,
      R"(,{"id":"t","kind":"title","enforce":true,"value":{"template":"Say \"hi\" </script>"}})"
      R"(,{"id":"c","kind":"canonical","enforce":true,"value":{"template":"https://t.test{path}"}})");
  PageFacts facts = Facts();
  PagePlan head = BuildPlan(pack, Ctx(), facts, DecisionPoint::kHeadEnd);
  PageContext ctx = Ctx();
  ctx.head_plan = &head;
  PagePlan doc = DocumentPlan(pack, facts, ctx);
  ASSERT_EQ(doc.rules.size(), 1u);
  ASSERT_EQ(doc.rules[0].ops.size(), 1u);
  const std::string& text = doc.rules[0].ops[0].value;
  // (Only '<' is written as a unicode escape.)
  const std::string escaped =
      std::string(R"("name":"Say \"hi\" \)") + "u003c/script>\"";
  EXPECT_NE(text.find(escaped), std::string::npos) << text;
  EXPECT_NE(text.find(R"("url":"https://t.test/x")"), std::string::npos)
      << text;
  EXPECT_EQ(text.find("</"), std::string::npos) << text;
}

TEST_F(PlannerTest, JsonLdQuotesOnlyWhatIsReallyInTheDocument) {
  const std::string tpl =
      R"({"template":"{\"@type\":\"WebPage\",\"name\":\"{title}\"}"})";
  // The title rule only reports, or its plan was dropped: an enforcing jsonld
  // rule quotes the title the page has; a reporting one the planned title.
  for (bool rule_enforces : {true, false}) {
    const std::string jsonld_rule =
        absl::StrCat(R"(,{"id":"j2","kind":"jsonld","enforce":)",
                     rule_enforces ? "true" : "false",
                     R"(,"match":{"paths":["/x"]},"value":)", tpl, "}");
    Pack pack = MakePack(
        "title", "replace", R"({"template":"Planned"})", jsonld_rule);
    PageFacts facts = Facts();
    ElementFact title = Fact(5, "Existing");
    title.text_mode = TextMode::kDecoded;
    facts.For(Title()).push_back(title);
    PagePlan head = BuildPlan(pack, Ctx(), facts, DecisionPoint::kHeadEnd);
    for (bool applied : {true, false}) {
      PageContext ctx = Ctx();
      ctx.head_plan = &head;
      ctx.head_applied = applied;
      PagePlan doc = DocumentPlan(pack, facts, ctx);
      ASSERT_EQ(doc.rules.size(), 1u);
      ASSERT_EQ(doc.rules[0].ops.size(), 1u);
      const bool quotes_planned = !rule_enforces || applied;
      EXPECT_NE(doc.rules[0].ops[0].value.find(quotes_planned ? "Planned"
                                                              : "Existing"),
                std::string::npos)
          << "enforce=" << rule_enforces << " applied=" << applied << " "
          << doc.rules[0].ops[0].value;
    }
  }
}

TEST_F(PlannerTest, EveryJsonLdTextTheKindPlansParsesWithTheStrictReader) {
  const std::string tpl =
      R"({"template":"{\"@type\":\"WebPage\",\"name\":\"{title}\",\"d\":\"{description}\",\"u\":\"{canonical}\"}"})";
  for (std::string_view rung : {"keep", "repair", "replace"}) {
    Pack pack = MakePack(
        "jsonld", rung, tpl,
        R"(,{"id":"t","kind":"title","enforce":true,"on_present":"replace","value":{"template":"A \\ \" ' & cafe"}})"
        R"(,{"id":"d","kind":"description","enforce":true,"value":{"template":"</script><!-- x -->"}})");
    PageFacts facts = Facts();
    PagePlan head = BuildPlan(pack, Ctx(), facts, DecisionPoint::kHeadEnd);
    PageContext ctx = Ctx();
    ctx.head_plan = &head;
    PagePlan doc = DocumentPlan(pack, facts, ctx);
    ASSERT_EQ(doc.rules.size(), 1u) << rung;
    for (const PlanOp& op : doc.rules[0].ops) {
      if (op.type == OpType::kRemove) continue;
      Json::Value parsed;
      EXPECT_TRUE(ParseStrictJson(op.value, kMaxJsonLdDepth, true, &parsed))
          << rung << ": " << op.value;
    }
  }
}

TEST_F(PlannerTest, HreflangRemovedListHoldsOnlyWellFormedUtf8) {
  Pack pack = MakePack("hreflang", "repair", kCluster);
  PageFacts facts = Facts();
  // A Latin-1 byte in the code and in the href.
  facts.For(Hreflang()).push_back(
      HFact(1, "d\xE9", "https://t.test/caf\xE9", true));
  const RulePlan rp = Only(BuildPlan(pack, Ctx(), facts));
  ASSERT_FALSE(rp.decision.removed.empty());
  EXPECT_EQ(ReplaceInvalidUtf8(rp.decision.removed), rp.decision.removed);
  EXPECT_NE(rp.decision.removed.find("\xEF\xBF\xBD"), std::string::npos);
}

TEST(HelpersTest, ReplaceInvalidUtf8) {
  EXPECT_EQ(ReplaceInvalidUtf8("plain"), "plain");
  EXPECT_EQ(ReplaceInvalidUtf8("caf\xC3\xA9 \xE2\x82\xAC \xF0\x9F\x98\x80"),
            "caf\xC3\xA9 \xE2\x82\xAC \xF0\x9F\x98\x80");
  EXPECT_EQ(ReplaceInvalidUtf8("a\xE9z"), "a\xEF\xBF\xBDz");
  EXPECT_EQ(ReplaceInvalidUtf8("\xC3"), "\xEF\xBF\xBD");        // truncated
  EXPECT_EQ(ReplaceInvalidUtf8("\xC0\xAF"), "\xEF\xBF\xBD\xEF\xBF\xBD");  // overlong
  EXPECT_EQ(ReplaceInvalidUtf8("\xED\xA0\x80"),
            "\xEF\xBF\xBD\xEF\xBF\xBD\xEF\xBF\xBD");  // surrogate
  EXPECT_EQ(ReplaceInvalidUtf8("\xF5\x80\x80\x80").size(), 12u);
}

TEST(HelpersTest, CollapseWhitespace) {
  EXPECT_EQ(CollapseWhitespace("  a \t\n b  "), "a b");
  EXPECT_EQ(CollapseWhitespace(""), "");
  EXPECT_EQ(CollapseWhitespace(" \n"), "");
}

TEST(HelpersTest, ResolveUrl) {
  const std::string base = "https://a.test/d/e/f?q=1#frag";
  EXPECT_EQ(ResolveUrl(base, "g"), "https://a.test/d/e/g");
  EXPECT_EQ(ResolveUrl(base, "../g"), "https://a.test/d/g");
  EXPECT_EQ(ResolveUrl(base, "../../../g"), "https://a.test/g");
  EXPECT_EQ(ResolveUrl(base, "/g"), "https://a.test/g");
  EXPECT_EQ(ResolveUrl(base, "//b.test/g"), "https://b.test/g");
  EXPECT_EQ(ResolveUrl(base, "?z=2"), "https://a.test/d/e/f?z=2");
  EXPECT_EQ(ResolveUrl(base, ""), "https://a.test/d/e/f?q=1");
  EXPECT_EQ(ResolveUrl(base, "http://c.test/x"), "http://c.test/x");
  EXPECT_EQ(ResolveUrl(base, "./g/."), "https://a.test/d/e/g/");
  EXPECT_EQ(ResolveUrl("not a url", "g"), "");
}

TEST(HelpersTest, ParsePageUrl) {
  auto p = ParsePageUrl("HTTPS://Www.Example.com:8443/a/b?x=1#frag");
  ASSERT_TRUE(p.has_value());
  EXPECT_EQ(p->scheme, "https");
  EXPECT_EQ(p->host, "www.example.com");
  EXPECT_EQ(p->path, "/a/b");
  EXPECT_EQ(p->query, "x=1");
  EXPECT_EQ(ParsePageUrl("https://a.test")->path, "/");
  EXPECT_EQ(ParsePageUrl("https://a.test?q=1")->path, "/");
  EXPECT_FALSE(ParsePageUrl("ftp://a.test/").has_value());
  EXPECT_FALSE(ParsePageUrl("/relative").has_value());
  EXPECT_FALSE(ParsePageUrl("https://user@a.test/").has_value());
  EXPECT_EQ(PageUrlString(*p), "https://www.example.com/a/b?x=1");
}

TEST(HelpersTest, DecodeAttribute) {
  EXPECT_EQ(DecodeAttribute("a&amp;b"), "a&b");
  EXPECT_EQ(DecodeAttribute("caf&#233;"), "caf\xC3\xA9");
  EXPECT_EQ(DecodeAttribute("&#x1F600;"), "\xF0\x9F\x98\x80");
  EXPECT_EQ(DecodeAttribute("&amp;#233;"), "&#233;");
  EXPECT_EQ(DecodeAttribute("&#xD800;"), "&#xD800;");  // a surrogate stays
}

}  // namespace
}  // namespace pagespeed::packs
