// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

#include "pagespeed/kernel/packs/kind_registry.h"

#include <set>
#include <string>

#include "gtest/gtest.h"
#include "pagespeed/kernel/packs/page_facts.h"

namespace pagespeed::packs {
namespace {

TEST(KindRegistryTest, KindsAreListedInApplicationOrder) {
  std::string names;
  for (const RuleKind* kind : AllKinds()) {
    names += std::string(names.empty() ? "" : ",") + std::string(kind->name());
  }
  EXPECT_EQ(names, "canonical,title,description,hreflang,jsonld");
  EXPECT_EQ(kKindCount, 5u);
  EXPECT_EQ(KindNameList(), "canonical, title, description, hreflang, jsonld");
}

TEST(KindRegistryTest, LookupByNameAndIndexAgree) {
  std::set<const RuleKind*> seen;
  for (size_t i = 0; i < kKindCount; ++i) {
    const RuleKind* kind = AllKinds()[i];
    ASSERT_NE(kind, nullptr);
    EXPECT_EQ(FindKind(kind->name()), kind);
    EXPECT_EQ(KindIndex(kind), i);
    EXPECT_EQ(KindIndexOf(kind->name()), i);
    EXPECT_TRUE(seen.insert(kind).second) << "one instance per kind";
  }
}

TEST(KindRegistryTest, UnknownNamesAreNotKinds) {
  EXPECT_EQ(FindKind(""), nullptr);
  EXPECT_EQ(FindKind("Canonical"), nullptr);
  EXPECT_EQ(FindKind("robots"), nullptr);
  EXPECT_EQ(KindIndexOf("robots"), kKindCount);
}

TEST(KindRegistryDeathTest, IndexOfANullKindIsFatal) {
  EXPECT_DEATH(KindIndex(nullptr), "null or foreign");
}

TEST(KindRegistryTest, DecisionPointsAndDefectPrefixes) {
  for (const char* name : {"canonical", "title", "description", "hreflang"}) {
    EXPECT_EQ(FindKind(name)->decision_point(), DecisionPoint::kHeadEnd)
        << name;
  }
  EXPECT_EQ(FindKind("jsonld")->decision_point(), DecisionPoint::kDocumentEnd);
  for (const RuleKind* kind : AllKinds()) {
    EXPECT_EQ(kind->lens_defect_prefix(), kind->name());
  }
}

// The head values a later kind quotes come from the three single-valued kinds,
// one each; the kinds that plan several elements do not feed a placeholder.
TEST(KindRegistryTest, HeadSlotsAreTheThreeSingleValuedKinds) {
  EXPECT_EQ(FindKind("canonical")->head_slot(), HeadSlot::kCanonical);
  EXPECT_EQ(FindKind("title")->head_slot(), HeadSlot::kTitle);
  EXPECT_EQ(FindKind("description")->head_slot(), HeadSlot::kDescription);
  EXPECT_EQ(FindKind("hreflang")->head_slot(), HeadSlot::kNone);
  EXPECT_EQ(FindKind("jsonld")->head_slot(), HeadSlot::kNone);
  EXPECT_EQ(FindKind("hreflang")->FinalValue(nullptr, {}, ""), "");
}

TEST(KindRegistryTest, TheFilterContractOfEachKind) {
  const RuleKind* hreflang = FindKind("hreflang");
  for (OnPresent rung :
       {OnPresent::kKeep, OnPresent::kRepair, OnPresent::kReplace}) {
    EXPECT_FALSE(hreflang->ModifiesUrls(rung));
    EXPECT_FALSE(hreflang->MayInjectScripts());
    EXPECT_FALSE(FindKind("jsonld")->ModifiesUrls(rung));
    EXPECT_FALSE(FindKind("canonical")->ModifiesUrls(rung));
  }
  EXPECT_TRUE(FindKind("jsonld")->MayInjectScripts());
  for (const char* name : {"canonical", "title", "description"}) {
    EXPECT_FALSE(FindKind(name)->MayInjectScripts()) << name;
  }
}

TEST(PageFactsTest, FactsAreKeptPerKind) {
  PageFacts facts;
  for (const RuleKind* kind : AllKinds()) EXPECT_TRUE(facts.For(kind).empty());
  facts.For(FindKind("title")).push_back(ElementFact{});
  facts.For(FindKind("title")).back().late = true;
  EXPECT_EQ(facts.For(FindKind("title")).size(), 1u);
  EXPECT_TRUE(facts.For(FindKind("title"))[0].late);
  EXPECT_TRUE(facts.For(FindKind("canonical")).empty());
  EXPECT_TRUE(facts.For(FindKind("jsonld")).empty());
}

}  // namespace
}  // namespace pagespeed::packs
