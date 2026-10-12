// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

#include "pagespeed/kernel/packs/matcher.h"

#include <string>

#include "gtest/gtest.h"
#include "pagespeed/kernel/packs/kind_registry.h"
#include "pagespeed/kernel/packs/pack.h"
#include "pagespeed/kernel/packs/pack_loader.h"

namespace pagespeed::packs {
namespace {

bool G(std::string_view glob, std::string_view path) {
  auto g = CompileGlob(glob);
  EXPECT_TRUE(g.ok()) << glob << ": " << g.status();
  return g.ok() && MatchGlob(*g, path);
}

Pack Load(const std::string& json) {
  auto p = LoadPack(json, "");
  EXPECT_TRUE(p.ok()) << p.status();
  return p.ok() ? *std::move(p) : Pack();
}

// ---- globs ----------------------------------------------------------------

TEST(GlobTest, LiteralMatchesWholePathOnly) {
  EXPECT_TRUE(G("/about", "/about"));
  EXPECT_FALSE(G("/about", "/about/"));
  EXPECT_FALSE(G("/about", "/about/us"));
  EXPECT_FALSE(G("/about", "/x/about"));
  EXPECT_TRUE(G("/", "/"));
  EXPECT_FALSE(G("/", "/a"));
}

TEST(GlobTest, SingleStarStaysWithinASegment) {
  EXPECT_TRUE(G("/shop/*", "/shop/blue"));
  EXPECT_TRUE(G("/shop/*", "/shop/"));
  EXPECT_FALSE(G("/shop/*", "/shop/blue/widget"));
  EXPECT_FALSE(G("/shop/*", "/shop"));
  EXPECT_TRUE(G("/a/*/c", "/a/b/c"));
  EXPECT_FALSE(G("/a/*/c", "/a/b/x/c"));
  EXPECT_TRUE(G("/*.html", "/index.html"));
  EXPECT_FALSE(G("/*.html", "/dir/index.html"));
}

TEST(GlobTest, DoubleStarCrossesSlashes) {
  EXPECT_TRUE(G("/shop/**", "/shop/blue"));
  EXPECT_TRUE(G("/shop/**", "/shop/blue/widget/1"));
  EXPECT_TRUE(G("/shop/**", "/shop/"));
  EXPECT_FALSE(G("/shop/**", "/shop"));
  EXPECT_FALSE(G("/shop/**", "/other/shop/x"));
  EXPECT_TRUE(G("/**", "/"));
  EXPECT_TRUE(G("/**", "/a/b/c"));
  EXPECT_TRUE(G("/a/**/c", "/a/x/y/c"));
  EXPECT_FALSE(G("/a/**/c", "/a/c"));
}

TEST(GlobTest, OtherCharactersAreLiteral) {
  EXPECT_TRUE(G("/a.b", "/a.b"));
  EXPECT_FALSE(G("/a.b", "/axb"));
  EXPECT_TRUE(G("/a+b(c)[d]", "/a+b(c)[d]"));
  EXPECT_FALSE(G("/a+b", "/aab"));
  EXPECT_TRUE(G("/caf\xc3\xa9", "/caf\xc3\xa9"));
}

TEST(GlobTest, InvalidGlobsAreRejected) {
  for (const char* bad :
       {"", "shop/*", "*", "/a/***", "/a?b", "/a#b", "/a b", "/a\tb"}) {
    EXPECT_FALSE(CompileGlob(bad).ok()) << bad;
  }
  EXPECT_FALSE(CompileGlob("/" + std::string(kMaxGlobBytes, 'a')).ok());
}

// ---- hosts ----------------------------------------------------------------

TEST(HostTest, NormalizeLowercasesAndStripsPort) {
  EXPECT_EQ(NormalizeHost("WWW.Example.COM"), "www.example.com");
  EXPECT_EQ(NormalizeHost("www.example.com:8080"), "www.example.com");
  EXPECT_EQ(NormalizeHost("[::1]:8080"), "[::1]");
  EXPECT_EQ(NormalizeHost("[::1]"), "[::1]");
  EXPECT_EQ(NormalizeHost(""), "");
}

TEST(HostTest, ExactSiteMatchesOnlyThatHost) {
  Site s;
  s.host = "www.example.com";
  EXPECT_TRUE(SiteMatchesHost(s, "www.example.com"));
  EXPECT_FALSE(SiteMatchesHost(s, "example.com"));
  EXPECT_FALSE(SiteMatchesHost(s, "a.www.example.com"));
  EXPECT_FALSE(SiteMatchesHost(s, "wwwxexample.com"));
}

TEST(HostTest, WildcardMatchesSubdomainsNotTheApex) {
  Site s;
  s.host = "example.org";
  s.wildcard = true;
  EXPECT_TRUE(SiteMatchesHost(s, "a.example.org"));
  EXPECT_TRUE(SiteMatchesHost(s, "a.b.example.org"));
  EXPECT_FALSE(SiteMatchesHost(s, "example.org"));
  EXPECT_FALSE(SiteMatchesHost(s, ".example.org"));
  EXPECT_FALSE(SiteMatchesHost(s, "aexample.org"));
  EXPECT_FALSE(SiteMatchesHost(s, "a.example.org.evil.test"));
}

const char kSitesPack[] = R"({
  "pack": {"id": "edge-seo", "version": "0.1.0"},
  "sites": [
    {"host": "*.example.org", "mode": "report"},
    {"host": "*.shop.example.org", "mode": "enforce"},
    {"host": "www.shop.example.org"}
  ],
  "rules": []
})";

TEST(FindSiteTest, ExactBeatsWildcardAndLongerSuffixBeatsShorter) {
  Pack p = Load(kSitesPack);
  const Site* s = FindSite(p, "www.shop.example.org");
  ASSERT_NE(s, nullptr);
  EXPECT_FALSE(s->wildcard);
  s = FindSite(p, "a.shop.example.org");
  ASSERT_NE(s, nullptr);
  EXPECT_EQ(s->host, "shop.example.org");
  s = FindSite(p, "blog.example.org");
  ASSERT_NE(s, nullptr);
  EXPECT_EQ(s->host, "example.org");
}

TEST(FindSiteTest, NormalizesTheRequestHost) {
  Pack p = Load(kSitesPack);
  EXPECT_NE(FindSite(p, "WWW.Shop.Example.org:8443"), nullptr);
}

TEST(FindSiteTest, UnlistedHostsAndApexDoNotMatch) {
  Pack p = Load(kSitesPack);
  EXPECT_EQ(FindSite(p, "example.org"), nullptr);
  EXPECT_EQ(FindSite(p, "example.com"), nullptr);
  EXPECT_EQ(FindSite(p, ""), nullptr);
}

// ---- rule paths and precedence --------------------------------------------

const char kRulesPack[] = R"({
  "pack": {"id": "edge-seo", "version": "0.1.0"},
  "sites": [{"host": "a.test"}],
  "defaults": {"paths": ["/**"], "exclude_paths": ["/admin/**"]},
  "tables": {"t": {"/": "Home"}},
  "rules": [
    {"id": "title-home", "kind": "title", "match": {"paths": ["/"]},
     "value": {"table": "t"}},
    {"id": "title-off", "kind": "title", "enabled": false,
     "value": {"template": "x"}},
    {"id": "title-all", "kind": "title", "value": {"template": "All"}},
    {"id": "title-all-2", "kind": "title", "value": {"template": "All 2"}},
    {"id": "canon-regex", "kind": "canonical",
     "match": {"paths": ["/shop/**"], "path_regex": "^/shop/([a-z]+)(?:/(\\d+))?$"},
     "value": {"template": "https://a.test/s/{1}/{2}"}},
    {"id": "canon-excl", "kind": "canonical",
     "match": {"paths": ["/p/**"], "exclude_paths": ["/p/private/**"]},
     "value": {"template": "https://a.test{path}"}}
  ]
})";

TEST(MatchRulePathTest, DefaultsApplyWhenRuleOmitsThem) {
  Pack p = Load(kRulesPack);
  const Rule& all = p.rules[2];
  EXPECT_TRUE(MatchRulePath(all, "/x").has_value());
  EXPECT_TRUE(MatchRulePath(all, "/").has_value());
  EXPECT_FALSE(MatchRulePath(all, "/admin/users").has_value());
}

TEST(MatchRulePathTest, RuleExcludeReplacesDefaultExclude) {
  Pack p = Load(kRulesPack);
  const Rule& excl = p.rules[5];
  EXPECT_TRUE(MatchRulePath(excl, "/p/x").has_value());
  EXPECT_FALSE(MatchRulePath(excl, "/p/private/x").has_value());
  EXPECT_FALSE(MatchRulePath(excl, "/q/x").has_value());
}

TEST(MatchRulePathTest, RegexIsAFullMatchWithCaptures) {
  Pack p = Load(kRulesPack);
  const Rule& r = p.rules[4];
  auto m = MatchRulePath(r, "/shop/blue/12");
  ASSERT_TRUE(m.has_value());
  ASSERT_EQ(m->size(), 2u);
  EXPECT_EQ((*m)[0], "blue");
  EXPECT_EQ((*m)[1], "12");

  // Optional group not participating -> empty string.
  m = MatchRulePath(r, "/shop/blue");
  ASSERT_TRUE(m.has_value());
  EXPECT_EQ((*m)[0], "blue");
  EXPECT_EQ((*m)[1], "");

  // Not anchored matches are rejected: full match only.
  EXPECT_FALSE(MatchRulePath(r, "/shop/blue/12/extra").has_value());
  EXPECT_FALSE(MatchRulePath(r, "/shop/Blue").has_value());
  // The glob must match too (AND).
  EXPECT_FALSE(MatchRulePath(r, "/other/blue").has_value());
}

TEST(SelectRulesTest, ARuleWithoutAKindIsSkipped) {
  Pack pack;
  Site site;
  site.host = "a.test";
  pack.sites.push_back(site);
  pack.rules.emplace_back();  // kind == nullptr, match scope empty
  pack.rules.back().id = "no-kind";
  Selection sel = SelectRules(pack, "a.test", "/");
  ASSERT_NE(sel.site, nullptr);
  for (const auto& hit : sel.by_kind) EXPECT_FALSE(hit.has_value());
  EXPECT_TRUE(sel.shadowed.empty());
}

TEST(SelectRulesTest, FirstEnabledMatchingRulePerKindWins) {
  Pack p = Load(kRulesPack);
  Selection sel = SelectRules(p, "a.test", "/");
  ASSERT_NE(sel.site, nullptr);
  const auto& title = sel.by_kind[KindIndexOf("title")];
  ASSERT_TRUE(title.has_value());
  EXPECT_EQ(title->rule->id, "title-home");
  // title-all and title-all-2 also match "/", the disabled rule does not
  // count at all.
  ASSERT_EQ(sel.shadowed.size(), 2u);
  EXPECT_EQ(sel.shadowed[0]->id, "title-all");
  EXPECT_EQ(sel.shadowed[1]->id, "title-all-2");
  EXPECT_FALSE(sel.by_kind[KindIndexOf("canonical")].has_value());
}

TEST(SelectRulesTest, LaterRuleWinsWhenEarlierDoesNotMatchThePath) {
  Pack p = Load(kRulesPack);
  Selection sel = SelectRules(p, "a.test", "/other");
  const auto& title = sel.by_kind[KindIndexOf("title")];
  ASSERT_TRUE(title.has_value());
  EXPECT_EQ(title->rule->id, "title-all");
  ASSERT_EQ(sel.shadowed.size(), 1u);
  EXPECT_EQ(sel.shadowed[0]->id, "title-all-2");
}

TEST(SelectRulesTest, KindsAreIndependentAndCapturesTravelWithTheHit) {
  Pack p = Load(kRulesPack);
  Selection sel = SelectRules(p, "A.test:443", "/shop/blue/3");
  const auto& title = sel.by_kind[KindIndexOf("title")];
  const auto& canon = sel.by_kind[KindIndexOf("canonical")];
  ASSERT_TRUE(title.has_value());
  ASSERT_TRUE(canon.has_value());
  EXPECT_EQ(title->rule->id, "title-all");
  EXPECT_EQ(canon->rule->id, "canon-regex");
  ASSERT_EQ(canon->captures.size(), 2u);
  EXPECT_EQ(canon->captures[1], "3");
}

TEST(SelectRulesTest, UnlistedHostSelectsNothing) {
  Pack p = Load(kRulesPack);
  Selection sel = SelectRules(p, "other.test", "/");
  EXPECT_EQ(sel.site, nullptr);
  for (const auto& hit : sel.by_kind) EXPECT_FALSE(hit.has_value());
  EXPECT_TRUE(sel.shadowed.empty());
}

TEST(SelectRulesTest, ExcludedPathSelectsNothing) {
  Pack p = Load(kRulesPack);
  Selection sel = SelectRules(p, "a.test", "/admin/x");
  ASSERT_NE(sel.site, nullptr);
  EXPECT_FALSE(sel.by_kind[KindIndexOf("title")].has_value());
}

// ---- effective mode -------------------------------------------------------

TEST(EffectiveModeTest, LowestOfTheThreeWins) {
  for (Mode g : {Mode::kReport, Mode::kEnforce}) {
    for (Mode s : {Mode::kReport, Mode::kEnforce}) {
      for (bool r : {false, true}) {
        const bool all_enforce =
            g == Mode::kEnforce && s == Mode::kEnforce && r;
        EXPECT_EQ(EffectiveMode(g, s, r),
                  all_enforce ? Mode::kEnforce : Mode::kReport);
      }
    }
  }
}

// ---- tables ---------------------------------------------------------------

TEST(LookupTableValueTest, PathAndPathWithQuery) {
  ValueTable t;
  t["/a"] = "A";
  t["/shop/index.php?id=12"] = "Q12";
  t["/shop/index.php"] = "Plain";
  EXPECT_EQ(*LookupTableValue(t, "/a", ""), "A");
  // A query that has no keyed entry falls back to the path key.
  EXPECT_EQ(*LookupTableValue(t, "/a", "x=1"), "A");
  EXPECT_EQ(*LookupTableValue(t, "/shop/index.php", "id=12"), "Q12");
  EXPECT_EQ(*LookupTableValue(t, "/shop/index.php", "id=13"), "Plain");
  EXPECT_EQ(*LookupTableValue(t, "/shop/index.php", ""), "Plain");
  EXPECT_EQ(LookupTableValue(t, "/missing", ""), nullptr);
  EXPECT_EQ(LookupTableValue(t, "/missing", "id=12"), nullptr);
}

TEST(LookupTableValueTest, QueryKeyWithoutPathKeyMissesWithoutQuery) {
  ValueTable t;
  t["/p?id=1"] = "One";
  EXPECT_EQ(*LookupTableValue(t, "/p", "id=1"), "One");
  EXPECT_EQ(LookupTableValue(t, "/p", ""), nullptr);
  EXPECT_EQ(LookupTableValue(t, "/p", "id=2"), nullptr);
}

TEST(FindClusterTest, MembershipUsesNormalizedUrls) {
  Pack p = Load(R"({
    "pack": {"id": "edge-seo", "version": "0.1.0"},
    "sites": [{"host": "a.test"}],
    "tables": {"hreflang": [
      {"en": "https://a.test/en/", "de": "https://a.test/de/",
       "x-default": "https://a.test/en/"},
      {"en": "https://a.test/other/"}
    ]},
    "rules": []
  })");
  const HreflangCluster* c = FindCluster(p, "HTTPS://A.test:443/de#frag");
  ASSERT_NE(c, nullptr);
  EXPECT_EQ(c->entries.size(), 3u);
  EXPECT_EQ(c->entries[0].first, "de");  // entries are sorted by code
  EXPECT_EQ(FindCluster(p, "https://a.test/en"), c);
  c = FindCluster(p, "https://a.test/other");
  ASSERT_NE(c, nullptr);
  EXPECT_EQ(c->entries.size(), 1u);

  EXPECT_EQ(FindCluster(p, "https://a.test/fr/"), nullptr);
  EXPECT_EQ(FindCluster(p, "not a url"), nullptr);
}

// ---- byte-exact matching, trailing dot, glob sets --------------------------

TEST(GlobTest, MatchingIsByteExact) {
  EXPECT_TRUE(G("/**", "/\xff"));
  EXPECT_TRUE(G("/a/*", "/a/\xff\xfe"));
  EXPECT_TRUE(G("/**", "/caf\xc3\xa9"));
  EXPECT_TRUE(G("/caf\xc3\xa9", "/caf\xc3\xa9"));
  EXPECT_FALSE(G("/caf\xc3\xa9", "/caf\xc3"));
  // One byte per '.' : a two-byte UTF-8 letter is two characters.
  EXPECT_TRUE(G("/**", "/\xc3\xa9\xc3\xa9"));
}

TEST(HostTest, OneTrailingDotIsStripped) {
  EXPECT_EQ(NormalizeHost("www.example.com."), "www.example.com");
  EXPECT_EQ(NormalizeHost("www.example.com.:8080"), "www.example.com");
  EXPECT_EQ(NormalizeHost("www.example.com.."), "www.example.com.");
  Pack p = Load(kSitesPack);
  EXPECT_NE(FindSite(p, "WWW.shop.example.org."), nullptr);
}

TEST(SelectRulesTest, RuleGlobsAreMatchedAsASet) {
  // The set and the per-glob fallback agree.
  Pack p = Load(kRulesPack);
  Rule r = p.rules[5];
  EXPECT_TRUE(MatchRulePath(r, "/p/x").has_value());
  r.match.paths_set = nullptr;
  r.match.exclude_set = nullptr;
  EXPECT_TRUE(MatchRulePath(r, "/p/x").has_value());
  EXPECT_FALSE(MatchRulePath(r, "/p/private/x").has_value());
  EXPECT_FALSE(MatchRulePath(r, "/q").has_value());
}

TEST(SelectRulesTest, ManyRulesWithManyGlobsLoadAndMatch) {
  // 200 rules, 32 globs each (the same 32 shared by all). Timing-insensitive:
  // only asserts the results.
  std::string rules;
  for (int i = 0; i < 200; ++i) {
    std::string globs;
    for (int g = 0; g < 32; ++g) {
      if (g) globs += ",";
      globs += "\"/sec" + std::to_string(g) + "/**\"";
    }
    if (i) rules += ",";
    rules += R"({"id":"r)" + std::to_string(i) +
             R"(","kind":"title","match":{"paths":[)" + globs +
             R"(]},"value":{"template":"T"}})";
  }
  Pack p = Load(R"({"pack":{"id":"edge-seo","version":"0.1.0"},
      "sites":[{"host":"a.test"}],"rules":[)" +
                rules + "]}");
  ASSERT_EQ(p.rules.size(), 200u);
  Selection hit = SelectRules(p, "a.test", "/sec31/deep/page");
  ASSERT_TRUE(hit.by_kind[KindIndexOf("title")].has_value());
  EXPECT_EQ(hit.by_kind[KindIndexOf("title")]->rule->id, "r0");
  EXPECT_EQ(hit.shadowed.size(), 199u);
  Selection miss = SelectRules(p, "a.test", "/other");
  EXPECT_FALSE(miss.by_kind[KindIndexOf("title")].has_value());
}

}  // namespace
}  // namespace pagespeed::packs
