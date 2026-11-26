// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 We-Amp B.V.

#include "pagespeed/system/module_entity_tags.h"

#include <initializer_list>

#include "pagespeed/kernel/base/string.h"
#include "pagespeed/kernel/base/string_util.h"
#include "test/pagespeed/kernel/base/gtest.h"

namespace net_instaweb {
namespace {

bool Carries(std::initializer_list<StringPiece> lines) {
  StringPieceVector v(lines.begin(), lines.end());
  return ConditionalsCarryModuleTag(v);
}

const char kMarkdownTag[] = "W/\"psm-0123456789abcdef\"";
const char kHtmlTag[] = "W/\"psh-0123456789abcdef\"";

TEST(ModuleEntityTagsTest, NoFieldCarriesNothing) {
  EXPECT_FALSE(ConditionalsCarryModuleTag(StringPieceVector()));
  EXPECT_FALSE(Carries({""}));
  EXPECT_FALSE(Carries({" \t "}));
  EXPECT_FALSE(Carries({", ,,"}));
}

TEST(ModuleEntityTagsTest, ModuleTagsWeakOrStrong) {
  EXPECT_TRUE(Carries({kMarkdownTag}));
  EXPECT_TRUE(Carries({kHtmlTag}));
  // A strong spelling of a module tag is still one: a weak comparison at a
  // chained server would match it.
  EXPECT_TRUE(Carries({"\"psm-0123456789abcdef\""}));
  EXPECT_TRUE(Carries({"\"psh-0123456789abcdef\""}));
  // The prefix alone.
  EXPECT_TRUE(Carries({"W/\"psm-\""}));
  EXPECT_TRUE(Carries({"\"psh-\""}));
}

TEST(ModuleEntityTagsTest, ForeignTagsPass) {
  EXPECT_FALSE(Carries({"W/\"v1\""}));
  EXPECT_FALSE(Carries({"\"v1\""}));
  EXPECT_FALSE(Carries({"\"v1\", W/\"v2\", \"\""}));
  // The prefix not at the start of the opaque-tag.
  EXPECT_FALSE(Carries({"\"xpsm-1\""}));
  EXPECT_FALSE(Carries({"W/\"v-psh-1\""}));
  // Too short to be the prefix.
  EXPECT_FALSE(Carries({"\"psm\""}));
  EXPECT_FALSE(Carries({"W/\"ps\""}));
  EXPECT_FALSE(Carries({"\"psx-1\""}));
  // Matched exactly, as entity tags are compared.
  EXPECT_FALSE(Carries({"W/\"PSM-0123456789abcdef\""}));
  EXPECT_FALSE(Carries({"\"Psh-1\""}));
  // A comma inside the quotes is part of the opaque-tag.
  EXPECT_FALSE(Carries({"\"a,psm-b\""}));
  // obs-text is allowed in an opaque-tag.
  EXPECT_FALSE(Carries({"\"\x80psm-1\""}));
}

TEST(ModuleEntityTagsTest, StarIsTheClientsOwnCondition) {
  EXPECT_FALSE(Carries({"*"}));
  EXPECT_FALSE(Carries({" * "}));
  // `*` inside a list is malformed: only the prefix scan decides.
  EXPECT_TRUE(Carries({"*, W/\"psm-1\""}));
  EXPECT_TRUE(Carries({"W/\"psm-1\", *"}));
  EXPECT_FALSE(Carries({"*, \"v1\""}));
}

TEST(ModuleEntityTagsTest, ListsAndOws) {
  EXPECT_TRUE(Carries({"\"v1\", W/\"psm-0123456789abcdef\""}));
  EXPECT_TRUE(Carries({"\"v1\",W/\"psm-1\""}));
  EXPECT_TRUE(Carries({"W/\"psm-1\", \"v1\""}));
  EXPECT_TRUE(Carries({", ,W/\"psh-1\",, "}));
  EXPECT_TRUE(Carries({"\t\"v1\",\tW/\"psm-1\"\t"}));
  EXPECT_TRUE(Carries({"\"v1\" , \"v2\" , \"psm-1\""}));
}

TEST(ModuleEntityTagsTest, EveryLineIsScanned) {
  EXPECT_TRUE(Carries({"\"v1\"", kMarkdownTag}));
  EXPECT_TRUE(Carries({kHtmlTag, "\"v1\""}));
  EXPECT_TRUE(Carries({"", "W/\"v1\"", "\"psh-1\""}));
  EXPECT_FALSE(Carries({"\"v1\"", "W/\"v2\"", "*"}));
}

// Spellings a strict parser rejects.  None may carry a module tag past the
// rule, and none without a module prefix is treated as one.
TEST(ModuleEntityTagsTest, MalformedFieldsFailClosed) {
  // Unterminated.
  EXPECT_TRUE(Carries({"W/\"psm-1"}));
  EXPECT_TRUE(Carries({"\"v1\", W/\"psm-1"}));
  // Unquoted.
  EXPECT_TRUE(Carries({"psm-1"}));
  EXPECT_TRUE(Carries({"W/psh-1"}));
  // A lower-case or spaced weakness prefix.
  EXPECT_TRUE(Carries({"w/\"psm-1\""}));
  EXPECT_TRUE(Carries({"W/ \"psm-1\""}));
  // A stray quote that would hide the tag inside a longer opaque-tag.
  EXPECT_TRUE(Carries({"\"x, W/\"psm-1\""}));
  // Junk after a tag.
  EXPECT_TRUE(Carries({"\"v1\" psm-1"}));
  EXPECT_TRUE(Carries({"\"v1\"W/\"psm-1\""}));
  // Control characters and NUL inside the field.
  EXPECT_TRUE(Carries({StringPiece("\"v1\"\0W/\"psm-1\"", 14)}));
  EXPECT_TRUE(Carries({"\"a\x01psm-1\""}));
  EXPECT_TRUE(Carries({"\"ps\x7fm-1\", psm-"}));
  // Malformed without a module prefix: left alone.
  EXPECT_FALSE(Carries({"\"v1"}));
  EXPECT_FALSE(Carries({"v1"}));
  EXPECT_FALSE(Carries({"\"v1\" junk"}));
  EXPECT_FALSE(Carries({"W/ \"v1\""}));
}

TEST(ModuleEntityTagsTest, LongListsScanToTheEnd) {
  GoogleString line;
  for (int i = 0; i < 10000; ++i) {
    StrAppend(&line, "W/\"v", IntegerToString(i), "\", ");
  }
  EXPECT_FALSE(Carries({line}));
  StrAppend(&line, kMarkdownTag);
  EXPECT_TRUE(Carries({line}));
}

TEST(ModuleEntityTagsTest, StashWithoutConditionals) {
  ClientConditionalStash stash;
  EXPECT_FALSE(
      StashClientConditionals(StringPieceVector(), StringPieceVector(), &stash));
  EXPECT_FALSE(stash.has_if_none_match);
  EXPECT_FALSE(stash.has_if_modified_since);
  EXPECT_FALSE(stash.stripped);
}

// A request without a module tag keeps both fields exactly; the stash still
// records them as they arrived.
TEST(ModuleEntityTagsTest, ForeignTagKeepsBothFields) {
  ClientConditionalStash stash;
  StringPieceVector inm = {"W/\"v1\""};
  StringPieceVector ims = {"Sun, 15 Nov 2026 11:00:00 GMT"};
  EXPECT_FALSE(StashClientConditionals(inm, ims, &stash));
  EXPECT_TRUE(stash.has_if_none_match);
  EXPECT_EQ("W/\"v1\"", stash.if_none_match);
  EXPECT_TRUE(stash.has_if_modified_since);
  EXPECT_EQ("Sun, 15 Nov 2026 11:00:00 GMT", stash.if_modified_since);
  EXPECT_FALSE(stash.stripped);
}

TEST(ModuleEntityTagsTest, IfModifiedSinceAloneIsKept) {
  ClientConditionalStash stash;
  StringPieceVector ims = {"Sun, 15 Nov 2026 11:00:00 GMT"};
  EXPECT_FALSE(StashClientConditionals(StringPieceVector(), ims, &stash));
  EXPECT_FALSE(stash.has_if_none_match);
  EXPECT_TRUE(stash.has_if_modified_since);
  EXPECT_FALSE(stash.stripped);
}

// A module tag anywhere drops both fields; the stash holds them as they
// arrived, every line joined.
TEST(ModuleEntityTagsTest, ModuleTagDropsBothFields) {
  ClientConditionalStash stash;
  StringPieceVector inm = {"\"v1\"", kMarkdownTag};
  StringPieceVector ims = {"Sun, 15 Nov 2026 11:00:00 GMT"};
  EXPECT_TRUE(StashClientConditionals(inm, ims, &stash));
  EXPECT_TRUE(stash.stripped);
  EXPECT_TRUE(stash.has_if_none_match);
  EXPECT_EQ(StrCat("\"v1\", ", kMarkdownTag), stash.if_none_match);
  EXPECT_TRUE(stash.has_if_modified_since);
  EXPECT_EQ("Sun, 15 Nov 2026 11:00:00 GMT", stash.if_modified_since);
}

TEST(ModuleEntityTagsTest, EmptyFieldIsPresent) {
  ClientConditionalStash stash;
  StringPieceVector inm = {""};
  EXPECT_FALSE(StashClientConditionals(inm, StringPieceVector(), &stash));
  EXPECT_TRUE(stash.has_if_none_match);
  EXPECT_EQ("", stash.if_none_match);
}

struct FactsLike {
  GoogleString if_none_match = "old";
  bool has_if_none_match = false;
  GoogleString if_modified_since = "old";
  bool has_if_modified_since = true;
};

TEST(ModuleEntityTagsTest, CopyIntoFacts) {
  ClientConditionalStash stash;
  stash.if_none_match = kMarkdownTag;
  stash.has_if_none_match = true;
  stash.stripped = true;
  FactsLike facts;
  CopyClientConditionals(stash, &facts);
  EXPECT_EQ(kMarkdownTag, facts.if_none_match);
  EXPECT_TRUE(facts.has_if_none_match);
  EXPECT_EQ("", facts.if_modified_since);
  EXPECT_FALSE(facts.has_if_modified_since);
}

}  // namespace
}  // namespace net_instaweb
