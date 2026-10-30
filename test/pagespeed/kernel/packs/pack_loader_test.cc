// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

#include "pagespeed/kernel/packs/pack_loader.h"

#include <cstdio>
#include <cstring>
#include <fstream>
#include <functional>
#include <string>

#include "absl/strings/str_cat.h"
#include "gtest/gtest.h"
#include "pagespeed/kernel/packs/engine_version.h"
#include "pagespeed/kernel/packs/kind_registry.h"
#include "pagespeed/kernel/packs/matcher.h"
#include "pagespeed/kernel/packs/pack.h"
#include "test/pagespeed/kernel/packs/test_json.h"

namespace pagespeed::packs {
namespace {

using Json = TestJson;

constexpr char kExamplePath[] =
    "test/pagespeed/kernel/packs/testdata/edge-seo/pack.example.json";

std::string ReadFile(const std::string& path) {
  std::ifstream in(path, std::ios::binary);
  EXPECT_TRUE(in.good()) << path;
  return std::string(std::istreambuf_iterator<char>(in), {});
}

// A small valid pack that every error test mutates.
Json Base() {
  return Json::parse(R"JSON({
    "pack": {"id": "edge-seo", "version": "0.1.0"},
    "sites": [{"host": "www.example.com", "mode": "enforce"}],
    "tables": {
      "titles": {"/": "Home"},
      "canonicals": {"/p?id=1": "https://www.example.com/p/one"},
      "hreflang": [{"en": "https://www.example.com/en/",
                    "de": "https://www.example.com/de/"}]
    },
    "rules": [
      {"id": "canon", "kind": "canonical",
       "match": {"paths": ["/p/**"], "path_regex": "^/p/([a-z]+)$"},
       "value": {"table": "canonicals",
                 "fallback_template": "https://www.example.com/p/{1}"}},
      {"id": "title", "kind": "title", "value": {"table": "titles"}},
      {"id": "desc", "kind": "description",
       "value": {"template": "Page {path}"}},
      {"id": "hl", "kind": "hreflang", "value": {"table": "hreflang"}},
      {"id": "ld", "kind": "jsonld",
       "value": {"template": "{\"@type\":\"Thing\",\"name\":\"{title}\"}"}}
    ]
  })JSON");
}

absl::StatusOr<Pack> Load(const Json& j, std::string_view engine = "2.3.0") {
  return LoadPack(j.dump(), engine, "test.json");
}

void ExpectLoadError(const Json& j, std::string_view needle,
                     std::string_view engine = "2.3.0") {
  auto p = Load(j, engine);
  ASSERT_FALSE(p.ok()) << "expected a load error containing: " << needle;
  EXPECT_EQ(p.status().code(), absl::StatusCode::kInvalidArgument);
  EXPECT_NE(std::string(p.status().message()).find(needle), std::string::npos)
      << "message was: " << p.status().message() << "\nwanted: " << needle;
  EXPECT_EQ(std::string(p.status().message()).rfind("test.json: ", 0), 0u)
      << "message must start with the source name: " << p.status().message();
}

void ExpectRawLoadError(std::string_view text, std::string_view needle) {
  auto p = LoadPack(text, "", "raw.json");
  ASSERT_FALSE(p.ok());
  EXPECT_NE(std::string(p.status().message()).find(needle), std::string::npos)
      << p.status().message();
}

// Mutates a copy of Base() and expects the given error.
void ExpectMutationError(const std::function<void(Json&)>& mutate,
                         std::string_view needle) {
  Json j = Base();
  mutate(j);
  ExpectLoadError(j, needle);
}

// ---- valid packs ------------------------------------------------------------

TEST(PackLoaderTest, BaseLoads) {
  auto p = Load(Base());
  ASSERT_TRUE(p.ok()) << p.status();
  EXPECT_EQ(p->info.id, "edge-seo");
  EXPECT_EQ(p->info.publisher, "local");  // default
  EXPECT_EQ(p->rules.size(), 5u);
}

TEST(PackLoaderTest, LicenseAbsentMeansAFreePack) {
  auto p = Load(Base());
  ASSERT_TRUE(p.ok()) << p.status();
  EXPECT_FALSE(p->info.license.has_value());
}

TEST(PackLoaderTest, LicenseFieldsLoad) {
  Json j = Base();
  j["pack"]["license"] = {{"entitlement", "edge-seo"},
                          {"expires", "2027-10-31"},
                          {"terms", "https://example.com/packs/terms"}};
  auto p = Load(j);
  ASSERT_TRUE(p.ok()) << p.status();
  ASSERT_TRUE(p->info.license.has_value());
  EXPECT_EQ(p->info.license->entitlement, "edge-seo");
  EXPECT_EQ(p->info.license->expires, "2027-10-31");
  EXPECT_EQ(p->info.license->terms, "https://example.com/packs/terms");
}

TEST(PackLoaderTest, AnEmptyLicenseLoads) {
  Json j = Base();
  j["pack"]["license"] = Json::object();
  auto p = Load(j);
  ASSERT_TRUE(p.ok()) << p.status();
  ASSERT_TRUE(p->info.license.has_value());
  EXPECT_TRUE(p->info.license->entitlement.empty());
  EXPECT_TRUE(p->info.license->expires.empty());
  EXPECT_TRUE(p->info.license->terms.empty());
}

TEST(PackLoaderTest, ALeapDayExpiryLoads) {
  Json j = Base();
  j["pack"]["license"] = {{"expires", "2028-02-29"}};
  auto p = Load(j);
  ASSERT_TRUE(p.ok()) << p.status();
}

TEST(PackLoaderTest, TheCenturyRuleForLeapDays) {
  Json ok = Base();
  ok["pack"]["license"] = {{"expires", "2000-02-29"}};
  auto p = Load(ok);
  ASSERT_TRUE(p.ok()) << p.status();
  ExpectMutationError(
      [](Json& j) { j["pack"]["license"] = {{"expires", "2100-02-29"}}; },
      "pack.license.expires: must be a date YYYY-MM-DD");
}

TEST(PackLoaderTest, LicenseErrors) {
  const std::string kEnt =
      "pack.license.entitlement: must match [a-z0-9-]{1,64}";
  const std::string kExp = "pack.license.expires: must be a date YYYY-MM-DD";
  const std::string kTerms =
      "pack.license.terms: must be an absolute https URL of at most 512 bytes";
  ExpectMutationError([](Json& j) { j["pack"]["license"] = "edge-seo"; },
                      "pack.license: must be an object");
  ExpectMutationError([](Json& j) { j["pack"]["license"] = {{"seats", 1}}; },
                      "pack.license: unknown key \"seats\"");
  ExpectMutationError(
      [](Json& j) { j["pack"]["license"] = {{"entitlement", "Edge-SEO"}}; },
      kEnt);
  ExpectMutationError(
      [](Json& j) { j["pack"]["license"] = {{"entitlement", ""}}; }, kEnt);
  ExpectMutationError(
      [](Json& j) {
        j["pack"]["license"] = {{"entitlement", std::string(65, 'a')}};
      },
      kEnt);
  ExpectMutationError(
      [](Json& j) { j["pack"]["license"] = {{"entitlement", 7}}; },
      "pack.license.entitlement: must be a string");
  ExpectMutationError(
      [](Json& j) { j["pack"]["license"] = {{"expires", "2027-1-31"}}; }, kExp);
  ExpectMutationError(
      [](Json& j) { j["pack"]["license"] = {{"expires", "2027-02-29"}}; },
      kExp);
  ExpectMutationError(
      [](Json& j) { j["pack"]["license"] = {{"expires", "1999-12-31"}}; },
      kExp);
  ExpectMutationError(
      [](Json& j) {
        j["pack"]["license"] = {{"terms", "http://example.com/t"}};
      },
      kTerms);
  ExpectMutationError(
      [](Json& j) {
        j["pack"]["license"] = {
            {"terms", "https://example.com/" + std::string(493, 'a')}};
      },
      kTerms);
  ExpectMutationError(
      [](Json& j) { j["pack"]["license"] = {{"terms", "https:///no-host"}}; },
      kTerms);
}

TEST(PackLoaderTest, ReservedKeysAreRejected) {
  ExpectMutationError([](Json& j) { j["signature"] = "x"; },
                      "$: unknown key \"signature\"");
  ExpectMutationError([](Json& j) { j["pack"]["signature"] = "x"; },
                      "pack: unknown key \"signature\"");
  ExpectMutationError([](Json& j) { j["pack"]["entitlement"] = "x"; },
                      "pack: unknown key \"entitlement\"");
}

TEST(PackLoaderTest, ADeeplyNestedJsonLdTemplateIsALoadError) {
  Json j = Base();
  const std::string deep = std::string(kMaxJsonLdDepth + 1, '[') +
                           std::string(kMaxJsonLdDepth + 1, ']');
  j["rules"][4]["value"]["template"] = deep;
  auto p = Load(j);
  ASSERT_FALSE(p.ok());
  EXPECT_NE(std::string(p.status().message()).find("nested deeper"),
            std::string::npos)
      << p.status();
  // Brackets inside strings do not count.
  j["rules"][4]["value"]["template"] = "{\"@type\":\"Thing\",\"name\":\"" +
                                       std::string(kMaxJsonLdDepth * 2, '[') +
                                       "\"}";
  EXPECT_TRUE(Load(j).ok());
}

TEST(PackLoaderTest, DefaultsAreSafe) {
  auto p = Load(Base());
  ASSERT_TRUE(p.ok());
  EXPECT_EQ(p->sites[0].mode, Mode::kEnforce);
  const Rule& r = p->rules[1];
  EXPECT_TRUE(r.enabled);
  EXPECT_FALSE(r.enforce);  // per-rule opt-in is off
  EXPECT_EQ(r.on_present, OnPresent::kKeep);
  ASSERT_EQ(r.match.paths.size(), 1u);  // defaults to "/**"
  EXPECT_EQ(r.match.paths[0].text, "/**");
  EXPECT_TRUE(r.match.exclude_paths.empty());
  EXPECT_EQ(r.match.path_regex, nullptr);

  Json j = Base();
  j["sites"][0].erase("mode");
  auto q = Load(j);
  ASSERT_TRUE(q.ok());
  EXPECT_EQ(q->sites[0].mode, Mode::kReport);  // site mode defaults to report
}

TEST(PackLoaderTest, ModelFieldsAreFilled) {
  auto p = Load(Base());
  ASSERT_TRUE(p.ok());
  const Rule& canon = p->rules[0];
  EXPECT_EQ(canon.kind, FindKind("canonical"));
  EXPECT_EQ(canon.value.source, ValueSource::kTable);
  EXPECT_EQ(canon.value.table, "canonicals");
  ASSERT_TRUE(canon.value.fallback_tpl.has_value());
  EXPECT_EQ(canon.match.path_regex_groups, 1);
  EXPECT_EQ(p->tables.at("titles").at("/"), "Home");
  ASSERT_EQ(p->hreflang_clusters.size(), 1u);
  // Entries are sorted by code: de, en.
  EXPECT_EQ(p->hreflang_clusters[0].entries[1].first, "en");
  EXPECT_EQ(p->hreflang_clusters[0].normalized_members[1],
            "https://www.example.com/en");
}

TEST(PackLoaderTest, DefaultsFallBackIntoRules) {
  Json j = Base();
  j["defaults"] = {{"paths", {"/docs/**"}}, {"exclude_paths", {"/docs/x/**"}}};
  auto p = Load(j);
  ASSERT_TRUE(p.ok()) << p.status();
  const Rule& title = p->rules[1];  // no match block
  EXPECT_EQ(title.match.paths[0].text, "/docs/**");
  EXPECT_EQ(title.match.exclude_paths[0].text, "/docs/x/**");
  const Rule& canon = p->rules[0];  // own paths, default excludes
  EXPECT_EQ(canon.match.paths[0].text, "/p/**");
  EXPECT_EQ(canon.match.exclude_paths[0].text, "/docs/x/**");
}

TEST(PackLoaderTest, HreflangCodesAreNormalized) {
  Json j = Base();
  j["tables"]["hreflang"] = {{{"en_US", "https://www.example.com/en/"},
                              {"X-Default", "https://www.example.com/"}}};
  auto p = Load(j);
  ASSERT_TRUE(p.ok()) << p.status();
  EXPECT_EQ(p->hreflang_clusters[0].entries[0].first, "en-us");
  EXPECT_EQ(p->hreflang_clusters[0].entries[1].first, "x-default");
}

TEST(PackLoaderTest, EnabledAndEnforceFlagsAreRead) {
  Json j = Base();
  j["rules"][1]["enabled"] = false;
  j["rules"][2]["enforce"] = true;
  j["rules"][2]["on_present"] = "replace";
  j["rules"][3]["on_present"] = "repair";
  auto p = Load(j);
  ASSERT_TRUE(p.ok()) << p.status();
  EXPECT_FALSE(p->rules[1].enabled);
  EXPECT_TRUE(p->rules[2].enforce);
  EXPECT_EQ(p->rules[2].on_present, OnPresent::kReplace);
  EXPECT_EQ(p->rules[3].on_present, OnPresent::kRepair);
}

TEST(PackLoaderTest, LinkHeaderIsOffByDefaultAndReadOnCanonicalRules) {
  Json j = Base();
  auto p = Load(j);
  ASSERT_TRUE(p.ok()) << p.status();
  for (const Rule& r : p->rules) EXPECT_EQ(r.link_header, LinkHeaderMode::kOff);
  EXPECT_FALSE(p->has_link_header_rule);
  j["rules"][0]["link_header"] = "match";
  p = Load(j);
  ASSERT_TRUE(p.ok()) << p.status();
  EXPECT_EQ(p->rules[0].link_header, LinkHeaderMode::kMatch);
  EXPECT_TRUE(p->has_link_header_rule);
  EXPECT_EQ(LinkHeaderModeName(p->rules[0].link_header), "match");
  j["rules"][0]["link_header"] = "off";
  p = Load(j);
  ASSERT_TRUE(p.ok()) << p.status();
  EXPECT_EQ(p->rules[0].link_header, LinkHeaderMode::kOff);
  EXPECT_FALSE(p->has_link_header_rule);
}

TEST(PackLoaderTest, ADisabledLinkHeaderRuleLeavesTheFlagFalse) {
  Json j = Base();
  j["rules"][0]["link_header"] = "match";
  j["rules"][0]["enabled"] = false;
  auto p = Load(j);
  ASSERT_TRUE(p.ok()) << p.status();
  EXPECT_EQ(p->rules[0].link_header, LinkHeaderMode::kMatch);
  EXPECT_FALSE(p->has_link_header_rule);
}

TEST(PackLoaderErrorTest, LinkHeaderMustBeOffOrMatchOnACanonicalRule) {
  ExpectMutationError([](Json& j) { j["rules"][0]["link_header"] = "on"; },
                      "rules[0].link_header: must be \"off\" or \"match\"");
  ExpectMutationError([](Json& j) { j["rules"][0]["link_header"] = true; },
                      "rules[0].link_header: must be a string");
  ExpectMutationError([](Json& j) { j["rules"][0]["link_header"] = "Match"; },
                      "rules[0].link_header: must be");
  // Not a canonical rule: refused whatever it says.
  ExpectMutationError(
      [](Json& j) { j["rules"][1]["link_header"] = "match"; },
      "rules[1].link_header: is only valid on a canonical rule");
  ExpectMutationError(
      [](Json& j) { j["rules"][1]["link_header"] = "off"; },
      "rules[1].link_header: is only valid on a canonical rule");
}

TEST(PackLoaderTest, TemplateWithJsonBracesAndPlaceholdersLoads) {
  auto p = Load(Base());
  ASSERT_TRUE(p.ok());
  const Rule& ld = p->rules[4];
  ASSERT_EQ(ld.value.source, ValueSource::kTemplate);
  bool saw_title = false;
  for (const auto& seg : ld.value.tpl.segments) {
    if (!seg.is_literal && seg.placeholder == Placeholder::kTitle) {
      saw_title = true;
    }
  }
  EXPECT_TRUE(saw_title);
}

TEST(PackLoaderTest, ValueTableKeysMayCarryAQuery) {
  Json j = Base();
  j["tables"]["titles"] = {{"/x?a=1", "With query"}};
  EXPECT_TRUE(Load(j).ok());
}

TEST(PackLoaderTest, ZeroRulesIsAllowed) {
  Json j = Base();
  j["rules"] = Json::array();
  EXPECT_TRUE(Load(j).ok());
}

// ---- the example pack -------------------------------------------------------

TEST(PackLoaderExampleTest, ExampleLoadsClean) {
  auto p = LoadPackFile(kExamplePath, EngineVersion());
  ASSERT_TRUE(p.ok()) << p.status();
  EXPECT_EQ(p->info.id, "edge-seo");
  EXPECT_EQ(p->info.version, "0.1.0");
  EXPECT_EQ(p->info.engine_min, "1.19.0");
  ASSERT_EQ(p->sites.size(), 2u);
  EXPECT_EQ(p->sites[0].host, "www.example.com");
  EXPECT_EQ(p->sites[0].mode, Mode::kReport);
  EXPECT_TRUE(p->sites[1].wildcard);
  EXPECT_EQ(p->sites[1].host, "example.org");
  EXPECT_EQ(p->sites[1].mode, Mode::kEnforce);
  ASSERT_EQ(p->rules.size(), 4u);
  EXPECT_EQ(p->rules[0].id, "canonical-shop");
  EXPECT_EQ(p->rules[1].on_present, OnPresent::kRepair);
  EXPECT_EQ(p->rules[2].value.source, ValueSource::kPattern);
  EXPECT_EQ(p->rules[2].value.pattern.size(), 3u);
  EXPECT_EQ(p->rules[3].kind, FindKind("jsonld"));
  EXPECT_TRUE(p->warnings.empty());
  // defaults.exclude_paths apply to rules without their own.
  EXPECT_EQ(p->rules[1].match.exclude_paths.size(), 2u);
}

TEST(PackLoaderExampleTest, ExampleBehavesAsDocumented) {
  auto p = LoadPackFile(kExamplePath, EngineVersion());
  ASSERT_TRUE(p.ok());
  Selection sel = SelectRules(*p, "www.example.com", "/shop/blue-widget");
  ASSERT_NE(sel.site, nullptr);
  ASSERT_TRUE(sel.by_kind[KindIndexOf("canonical")].has_value());
  EXPECT_EQ(sel.by_kind[KindIndexOf("canonical")]->captures[0], "blue-widget");
  EXPECT_FALSE(sel.by_kind[KindIndexOf("title")].has_value());
  EXPECT_EQ(SelectRules(*p, "www.example.com", "/cart/x")
                .by_kind[KindIndexOf("canonical")]
                .has_value(),
            false);
  EXPECT_EQ(FindSite(*p, "example.org"), nullptr);
  EXPECT_NE(FindSite(*p, "blog.example.org"), nullptr);
  const ValueTable& canon = p->tables.at("canonicals");
  EXPECT_EQ(*LookupTableValue(canon, "/shop/index.php", "id=12"),
            "https://www.example.com/shop/blue-widget");
  EXPECT_NE(FindCluster(*p, "https://www.example.com/de/"), nullptr);
}

TEST(PackLoaderExampleTest, ExampleRoundTripsThroughTheLoader) {
  // Re-serialising the parsed JSON (different whitespace, same content) and
  // loading it again yields an equivalent pack.
  const std::string text = ReadFile(kExamplePath);
  auto first = LoadPack(text, EngineVersion(), kExamplePath);
  ASSERT_TRUE(first.ok()) << first.status();
  const std::string compact = Json::parse(text).dump();
  const std::string pretty = Json::parse(text).dump(4);
  EXPECT_NE(compact, text);
  for (const std::string& variant : {compact, pretty}) {
    auto again = LoadPack(variant, EngineVersion());
    ASSERT_TRUE(again.ok()) << again.status();
    ASSERT_EQ(again->rules.size(), first->rules.size());
    for (size_t i = 0; i < first->rules.size(); ++i) {
      const Rule& a = first->rules[i];
      const Rule& b = again->rules[i];
      EXPECT_EQ(a.id, b.id);
      EXPECT_EQ(a.kind, b.kind);
      EXPECT_EQ(a.enabled, b.enabled);
      EXPECT_EQ(a.enforce, b.enforce);
      EXPECT_EQ(a.on_present, b.on_present);
      EXPECT_EQ(a.match.path_regex_text, b.match.path_regex_text);
      EXPECT_EQ(a.value.source, b.value.source);
      EXPECT_EQ(a.value.table, b.value.table);
    }
    EXPECT_EQ(again->tables, first->tables);
    EXPECT_EQ(again->sites.size(), first->sites.size());
    EXPECT_EQ(again->hreflang_clusters.size(), first->hreflang_clusters.size());
  }
}

TEST(PackLoaderExampleTest, ExampleIsRefusedByAnOlderEngine) {
  auto p = LoadPackFile(kExamplePath, "1.18.9");
  ASSERT_FALSE(p.ok());
  EXPECT_NE(std::string(p.status().message()).find("pack.engine_min"),
            std::string::npos);
}

// ---- document level -------------------------------------------------------

TEST(PackLoaderErrorTest, MalformedJson) {
  ExpectRawLoadError("{", "malformed JSON");
  ExpectRawLoadError("", "malformed JSON");
  ExpectRawLoadError("{\"pack\": }", "malformed JSON");
  ExpectRawLoadError("{} trailing", "malformed JSON");
  ExpectRawLoadError("{\"a\": 1,}", "malformed JSON");
  ExpectRawLoadError("// c\n{}", "malformed JSON");
}

TEST(PackLoaderErrorTest, RootMustBeAnObject) {
  ExpectRawLoadError("[]", "$: must be an object");
  // The strict reader refuses any other root before the loader sees it.
  ExpectRawLoadError("\"x\"", "malformed JSON");
  ExpectRawLoadError("null", "malformed JSON");
  ExpectRawLoadError("5", "malformed JSON");
}

TEST(PackLoaderErrorTest, ErrorsNameFileAndPath) {
  auto p = LoadPack(R"JSON({"pack":{"id":"edge-seo","version":"0.1.0"},
      "sites":[{"host":"a.test"}],
      "rules":[{"id":"a","kind":"title","value":{"template":"x"}},
               {"id":"b","kind":"title","value":{"template":"y"},
                "match":{"paths":["/ok","bad"]}}]})JSON",
                    "", "/etc/packs/my pack.json");
  ASSERT_FALSE(p.ok());
  EXPECT_EQ(p.status().message(),
            "/etc/packs/my pack.json: rules[1].match.paths[1]: glob must "
            "start with '/'");
}

TEST(PackLoaderErrorTest, SourceNameIsOptional) {
  auto p = LoadPack("[]", "");
  ASSERT_FALSE(p.ok());
  EXPECT_EQ(p.status().message(), "$: must be an object");
}

TEST(PackLoaderErrorTest, UnknownKeysAnywhere) {
  ExpectMutationError([](Json& j) { j["bogus"] = 1; },
                      "$: unknown key \"bogus\"");
  ExpectMutationError([](Json& j) { j["pack"]["bogus"] = 1; },
                      "pack: unknown key \"bogus\"");
  ExpectMutationError([](Json& j) { j["sites"][0]["bogus"] = 1; },
                      "sites[0]: unknown key \"bogus\"");
  ExpectMutationError([](Json& j) { j["defaults"] = {{"bogus", 1}}; },
                      "defaults: unknown key \"bogus\"");
  ExpectMutationError([](Json& j) { j["rules"][0]["enfroce"] = true; },
                      "rules[0]: unknown key \"enfroce\"");
  ExpectMutationError([](Json& j) { j["rules"][0]["match"]["bogus"] = 1; },
                      "rules[0].match: unknown key \"bogus\"");
  ExpectMutationError([](Json& j) { j["rules"][0]["value"]["bogus"] = 1; },
                      "rules[0].value: unknown key \"bogus\"");
}

TEST(PackLoaderErrorTest, DuplicateKeys) {
  ExpectRawLoadError(
      R"JSON({"pack":{"id":"edge-seo","version":"0.1.0"},"pack":{}})JSON",
      "Duplicate key: 'pack'");
  ExpectRawLoadError(
      R"JSON({"pack":{"id":"edge-seo","id":"edge-seo","version":"0.1.0"}})JSON",
      "Duplicate key: 'id'");
}

TEST(PackLoaderErrorTest, MissingRequiredKeys) {
  ExpectMutationError([](Json& j) { j.erase("pack"); },
                      "missing required key \"pack\"");
  ExpectMutationError([](Json& j) { j.erase("sites"); },
                      "missing required key \"sites\"");
  ExpectMutationError([](Json& j) { j.erase("rules"); },
                      "missing required key \"rules\"");
  ExpectMutationError([](Json& j) { j["pack"].erase("id"); },
                      "pack: missing required key \"id\"");
  ExpectMutationError([](Json& j) { j["pack"].erase("version"); },
                      "pack: missing required key \"version\"");
  ExpectMutationError([](Json& j) { j["sites"][0].erase("host"); },
                      "sites[0]: missing required key \"host\"");
  ExpectMutationError([](Json& j) { j["rules"][0].erase("id"); },
                      "rules[0]: missing required key \"id\"");
  ExpectMutationError([](Json& j) { j["rules"][0].erase("kind"); },
                      "rules[0]: missing required key \"kind\"");
  ExpectMutationError([](Json& j) { j["rules"][0].erase("value"); },
                      "rules[0]: missing required key \"value\"");
}

TEST(PackLoaderErrorTest, WrongTypes) {
  ExpectMutationError([](Json& j) { j["pack"] = "x"; },
                      "pack: must be an object");
  ExpectMutationError([](Json& j) { j["pack"]["id"] = 1; },
                      "pack.id: must be a string");
  ExpectMutationError([](Json& j) { j["sites"] = "x"; },
                      "sites: must be an array");
  ExpectMutationError([](Json& j) { j["sites"][0] = "x"; },
                      "sites[0]: must be an object");
  ExpectMutationError([](Json& j) { j["rules"] = Json::object(); },
                      "rules: must be an array");
  ExpectMutationError([](Json& j) { j["rules"][0]["enabled"] = "yes"; },
                      "rules[0].enabled: must be true or false");
  ExpectMutationError([](Json& j) { j["rules"][0]["enforce"] = 1; },
                      "rules[0].enforce: must be true or false");
  ExpectMutationError([](Json& j) { j["rules"][0]["match"]["paths"] = "/x"; },
                      "rules[0].match.paths: must be an array");
  ExpectMutationError([](Json& j) { j["rules"][0]["match"]["paths"] = {1}; },
                      "rules[0].match.paths[0]: must be a string");
  ExpectMutationError([](Json& j) { j["tables"] = Json::array(); },
                      "tables: must be an object");
  ExpectMutationError([](Json& j) { j["tables"]["titles"]["/"] = 5; },
                      "must be a string");
  ExpectMutationError([](Json& j) { j["tables"]["hreflang"] = Json::object(); },
                      "the hreflang table must be an array");
}

// ---- pack section -----------------------------------------------------------

TEST(PackLoaderErrorTest, PackIdMustMatchGrammar) {
  const char* kMsg = "pack.id: must match [a-z0-9-]{1,64}";
  for (const char* bad : {"Edge-SEO", "a_b", ""}) {
    ExpectMutationError([&](Json& j) { j["pack"]["id"] = bad; }, kMsg);
  }
  ExpectMutationError(
      [](Json& j) { j["pack"]["id"] = std::string(65, 'a'); }, kMsg);
}

TEST(PackLoaderTest, PackIdAcceptsGrammar) {
  for (const std::string& id : {std::string("edge-seo"),
                               std::string("fixture-second"),
                               std::string(64, 'a')}) {
    Json j = Base();
    j["pack"]["id"] = id;
    auto p = Load(j);
    ASSERT_TRUE(p.ok()) << p.status();
    EXPECT_EQ(p->info.id, id);
  }
}

TEST(PackLoaderErrorTest, VersionMustBeSemver) {
  for (const char* bad :
       {"1", "1.0", "v1.0.0", "1.0.0.0", "01.0.0", "a.b.c", ""}) {
    ExpectMutationError([&](Json& j) { j["pack"]["version"] = bad; },
                        "pack.version: must be a semantic version");
  }
  Json j = Base();
  j["pack"]["version"] = "1.2.3-rc.1+build.5";
  EXPECT_TRUE(Load(j).ok());
}

TEST(PackLoaderErrorTest, EngineMinTooNewIsRefused) {
  Json j = Base();
  j["pack"]["engine_min"] = "2.4.0";
  ExpectLoadError(j, "pack.engine_min: requires engine 2.4.0 or newer",
                  "2.3.9");
  ExpectLoadError(j, "pack.engine_min: requires engine 2.4.0 or newer",
                  "1.99.99");
}

TEST(PackLoaderErrorTest, EngineMinComparison) {
  Json j = Base();
  j["pack"]["engine_min"] = "2.3.0";
  EXPECT_TRUE(Load(j, "2.3.0").ok());
  EXPECT_TRUE(Load(j, "2.3.1").ok());
  EXPECT_TRUE(Load(j, "2.10.0").ok());  // numeric, not lexical
  EXPECT_TRUE(Load(j, "3.0.0").ok());
  EXPECT_TRUE(Load(j, "2.3.0~dev.2").ok());  // suffix after patch ignored
  EXPECT_FALSE(Load(j, "2.2.99").ok());
  EXPECT_FALSE(Load(j, "1.99.99").ok());
  EXPECT_TRUE(Load(j, "").ok());  // empty engine version skips the check
  ExpectLoadError(j, "cannot compare against engine version \"dev\"", "dev");
}

TEST(PackLoaderErrorTest, EngineMinMustBeSemver) {
  ExpectMutationError([](Json& j) { j["pack"]["engine_min"] = "new"; },
                      "pack.engine_min: must be a semantic version");
}

// ---- sites ------------------------------------------------------------------

TEST(PackLoaderErrorTest, SitesMustBeNonEmpty) {
  ExpectMutationError([](Json& j) { j["sites"] = Json::array(); },
                      "sites: must list at least one site");
}

TEST(PackLoaderErrorTest, BadHosts) {
  for (const char* bad :
       {"", "WWW.Example.com", "www.example.com:8080", "http://a.test",
        "a.test/path", "*", "*.", "a.*.test", "*a.test", "**.a.test", "a..test",
        "-a.test", "a-.test", "a b.test", "a_b.test", ".a.test",
        "*.*.a.test"}) {
    ExpectMutationError([&](Json& j) { j["sites"][0]["host"] = bad; },
                        "sites[0].host: must be a lowercase host name");
  }
}

TEST(PackLoaderErrorTest, GoodHosts) {
  for (const char* good : {"a.test", "localhost", "*.a.test", "a-b.c-d.test",
                           "127.0.0.1", "xn--bcher-kva.example"}) {
    Json j = Base();
    j["sites"][0]["host"] = good;
    EXPECT_TRUE(Load(j).ok()) << good;
  }
}

TEST(PackLoaderErrorTest, DuplicateSite) {
  ExpectMutationError(
      [](Json& j) { j["sites"].push_back({{"host", "www.example.com"}}); },
      "sites[1].host: duplicate site");
}

TEST(PackLoaderErrorTest, BadSiteMode) {
  for (const char* bad : {"on", "Enforce", "", "off"}) {
    ExpectMutationError([&](Json& j) { j["sites"][0]["mode"] = bad; },
                        "sites[0].mode: must be \"report\" or \"enforce\"");
  }
}

// ---- globs and regexes ------------------------------------------------------

TEST(PackLoaderErrorTest, BadGlobs) {
  ExpectMutationError(
      [](Json& j) { j["rules"][0]["match"]["paths"] = {"shop/*"}; },
      "rules[0].match.paths[0]: glob must start with '/'");
  ExpectMutationError(
      [](Json& j) { j["rules"][0]["match"]["paths"] = {"/a/***"}; },
      "rules[0].match.paths[0]: glob contains '***'");
  ExpectMutationError(
      [](Json& j) { j["rules"][0]["match"]["exclude_paths"] = {"/a?b"}; },
      "rules[0].match.exclude_paths[0]");
  ExpectMutationError([](Json& j) { j["rules"][0]["match"]["paths"] = {""}; },
                      "rules[0].match.paths[0]: glob is empty");
  ExpectMutationError(
      [](Json& j) { j["rules"][0]["match"]["paths"] = Json::array(); },
      "rules[0].match.paths: must not be empty");
  ExpectMutationError([](Json& j) { j["defaults"] = {{"paths", {"x"}}}; },
                      "defaults.paths[0]: glob must start with '/'");
  ExpectMutationError(
      [](Json& j) { j["defaults"] = {{"exclude_paths", {"/ok", "/a b"}}}; },
      "defaults.exclude_paths[1]");
}

TEST(PackLoaderErrorTest, BadRegexes) {
  ExpectMutationError(
      [](Json& j) { j["rules"][0]["match"]["path_regex"] = "^/p/(unclosed"; },
      "rules[0].match.path_regex: invalid RE2 pattern");
  ExpectMutationError(
      [](Json& j) { j["rules"][0]["match"]["path_regex"] = "^/p/(?=x)"; },
      "invalid RE2 pattern");  // lookahead is not RE2
  ExpectMutationError(
      [](Json& j) { j["rules"][0]["match"]["path_regex"] = "^/p/\\1$"; },
      "invalid RE2 pattern");  // backreferences are not RE2
  ExpectMutationError(
      [](Json& j) { j["rules"][0]["match"]["path_regex"] = ""; },
      "rules[0].match.path_regex: must not be empty");
  ExpectMutationError([](Json& j) { j["rules"][0]["match"]["path_regex"] = 5; },
                      "rules[0].match.path_regex: must be a string");
}

TEST(PackLoaderErrorTest, RegexLimits) {
  ExpectMutationError(
      [](Json& j) {
        j["rules"][0]["match"]["path_regex"] =
            std::string(kMaxRegexBytes + 1, 'a');
      },
      "rules[0].match.path_regex: is longer than");
  // Exceeds the per-regex memory budget at compile time.
  ExpectMutationError(
      [](Json& j) {
        j["rules"][0]["match"]["path_regex"] = "^((a{100}){100}){100}$";
      },
      "invalid RE2 pattern");
}

// ---- rules ------------------------------------------------------------------

TEST(PackLoaderErrorTest, BadRuleIds) {
  for (const char* bad : {"", "Canon", "a_b", "a b", "a.b"}) {
    ExpectMutationError([&](Json& j) { j["rules"][0]["id"] = bad; },
                        "rules[0].id: must match [a-z0-9-]{1,64}");
  }
  ExpectMutationError(
      [](Json& j) { j["rules"][0]["id"] = std::string(65, 'a'); },
      "rules[0].id: must match");
  Json j = Base();
  j["rules"][0]["id"] = std::string(64, 'a');
  EXPECT_TRUE(Load(j).ok());
}

TEST(PackLoaderErrorTest, DuplicateRuleId) {
  ExpectMutationError([](Json& j) { j["rules"][1]["id"] = "canon"; },
                      "rules[1].id: duplicate rule id \"canon\"");
}

TEST(PackLoaderErrorTest, UnknownKind) {
  for (const char* bad : {"robots", "Canonical", "", "og"}) {
    ExpectMutationError([&](Json& j) { j["rules"][0]["kind"] = bad; },
                        "rules[0].kind: must be one of canonical, title, "
                        "description, hreflang, jsonld");
  }
}

TEST(PackLoaderErrorTest, BadOnPresent) {
  for (const char* bad : {"always", "Keep", "", "delete"}) {
    ExpectMutationError([&](Json& j) { j["rules"][0]["on_present"] = bad; },
                        "rules[0].on_present: must be \"keep\", \"repair\" or "
                        "\"replace\"");
  }
}

TEST(PackLoaderErrorTest, TooManyRules) {
  Json j = Base();
  Json rules = Json::array();
  for (size_t i = 0; i < kMaxRules + 1; ++i) {
    rules.push_back({{"id", absl::StrCat("r", i)},
                     {"kind", "title"},
                     {"value", {{"template", "x"}}}});
  }
  j["rules"] = rules;
  ExpectLoadError(j, "rules: has 201 rules; the limit is 200");
  rules.pop_back();
  j["rules"] = rules;
  auto p = Load(j);
  ASSERT_TRUE(p.ok()) << p.status();
  EXPECT_EQ(p->rules.size(), kMaxRules);
}

// ---- values -----------------------------------------------------------------

TEST(PackLoaderErrorTest, ValueNeedsExactlyOneSource) {
  ExpectMutationError([](Json& j) { j["rules"][1]["value"] = Json::object(); },
                      "rules[1].value: must have exactly one of");
  ExpectMutationError(
      [](Json& j) {
        j["rules"][1]["value"] = {{"template", "x"}, {"table", "titles"}};
      },
      "rules[1].value: must have exactly one of");
}

TEST(PackLoaderErrorTest, FallbackTemplateNeedsTable) {
  ExpectMutationError(
      [](Json& j) {
        j["rules"][2]["value"] = {{"template", "x"},
                                  {"fallback_template", "y"}};
      },
      "rules[2].value.fallback_template: is only allowed together with "
      "\"table\"");
}

TEST(PackLoaderErrorTest, UnknownTable) {
  ExpectMutationError(
      [](Json& j) { j["rules"][1]["value"] = {{"table", "nope"}}; },
      "rules[1].value.table: unknown table \"nope\"");
}

TEST(PackLoaderErrorTest, KindSpecificValueShapes) {
  ExpectMutationError(
      [](Json& j) { j["rules"][3]["value"] = {{"template", "x"}}; },
      "hreflang rules take \"table\" or \"pattern\"");
  ExpectMutationError(
      [](Json& j) { j["rules"][3]["value"] = {{"table", "titles"}}; },
      "hreflang rules can only use the table \"hreflang\"");
  ExpectMutationError(
      [](Json& j) {
        j["rules"][3]["value"] = {{"table", "hreflang"},
                                  {"fallback_template", "x"}};
      },
      "not allowed for the hreflang table");
  ExpectMutationError(
      [](Json& j) {
        j["rules"][1]["value"] = {{"pattern", {{"en", "https://a.test/"}}}};
      },
      "\"pattern\" is only allowed for hreflang rules");
  ExpectMutationError(
      [](Json& j) { j["rules"][1]["value"] = {{"table", "hreflang"}}; },
      "the table \"hreflang\" is only for hreflang rules");
  ExpectMutationError(
      [](Json& j) { j["rules"][4]["value"] = {{"table", "titles"}}; },
      "jsonld rules take a \"template\"");
}

TEST(PackLoaderErrorTest, HreflangPatternErrors) {
  auto set = [](Json& j, const Json& pattern) {
    j["rules"][3]["value"] = {{"pattern", pattern}};
  };
  ExpectMutationError([&](Json& j) { set(j, Json::object()); },
                      "rules[3].value.pattern: must not be empty");
  ExpectMutationError(
      [&](Json& j) { set(j, {{"english", "https://a.test/"}}); },
      "rules[3].value.pattern.english: not a valid hreflang code");
  ExpectMutationError(
      [&](Json& j) {
        set(j, {{"en_US", "https://a.test/"}, {"en-us", "https://a.test/"}});
      },
      "duplicate hreflang code \"en-us\"");
  ExpectMutationError([&](Json& j) { set(j, {{"en", "{bogus}"}}); },
                      "rules[3].value.pattern.en: unknown placeholder {bogus}");
  ExpectMutationError([&](Json& j) { set(j, {{"en", 5}}); },
                      "rules[3].value.pattern.en: must be a string");
  Json ok = Base();
  set(ok, {{"en", "https://a.test/en{path}"}, {"x-default", "{url}"}});
  EXPECT_TRUE(Load(ok).ok());
}

// ---- templates --------------------------------------------------------------

TEST(PackLoaderErrorTest, UnknownPlaceholder) {
  ExpectMutationError(
      [](Json& j) { j["rules"][2]["value"] = {{"template", "{nope}"}}; },
      "rules[2].value.template: unknown placeholder {nope}");
  ExpectMutationError(
      [](Json& j) { j["rules"][2]["value"] = {{"template", "{Path}"}}; },
      "unknown placeholder {Path}");
  ExpectMutationError(
      [](Json& j) {
        j["rules"][0]["value"]["fallback_template"] = "https://a.test/{typo}";
      },
      "rules[0].value.fallback_template: unknown placeholder {typo}");
}

TEST(PackLoaderErrorTest, PageValuePlaceholdersOnlyInJsonLd) {
  ExpectMutationError(
      [](Json& j) { j["rules"][2]["value"] = {{"template", "{title}"}}; },
      "rules[2].value.template: placeholder {title} is only allowed in jsonld");
  ExpectMutationError(
      [](Json& j) { j["rules"][2]["value"] = {{"template", "{canonical}"}}; },
      "only allowed in jsonld");
}

TEST(PackLoaderErrorTest, CaptureGroupMustExistInPathRegex) {
  // "desc" has no path_regex at all.
  ExpectMutationError(
      [](Json& j) { j["rules"][2]["value"] = {{"template", "{1}"}}; },
      "refers to capture group 1 but path_regex has 0 groups");
  // "canon" has one group.
  ExpectMutationError(
      [](Json& j) {
        j["rules"][0]["value"]["fallback_template"] = "https://a.test/{2}";
      },
      "refers to capture group 2 but path_regex has 1 group");
}

TEST(PackLoaderErrorTest, TemplateSizeLimit) {
  ExpectMutationError(
      [](Json& j) {
        j["rules"][2]["value"] = {
            {"template", std::string(kMaxTemplateBytes + 1, 'a')}};
      },
      "rules[2].value.template: template is longer than 16384 bytes");
  ExpectMutationError(
      [](Json& j) { j["rules"][2]["value"] = {{"template", ""}}; },
      "template must not be empty");
  Json j = Base();
  j["rules"][2]["value"] = {{"template", std::string(kMaxTemplateBytes, 'a')}};
  EXPECT_TRUE(Load(j).ok());
}

// ---- tables -----------------------------------------------------------------

TEST(PackLoaderErrorTest, BadTableNames) {
  for (const char* bad : {"Titles", "a b", "a.b", ""}) {
    ExpectMutationError([&](Json& j) { j["tables"][bad] = Json::object(); },
                        "must match [a-z0-9_-]{1,64}");
  }
}

TEST(PackLoaderErrorTest, BadTableKeys) {
  for (const char* bad : {"", "about", "/a b", "/a#frag", "http://a.test/"}) {
    ExpectMutationError([&](Json& j) { j["tables"]["titles"] = {{bad, "T"}}; },
                        "must be a URL path starting with '/'");
  }
}

TEST(PackLoaderErrorTest, RelativeAndInvalidUrlsInCanonicalTables) {
  for (const char* bad :
       {"/relative", "//a.test/x", "example.com/x", "ftp://a.test/x",
        "javascript:alert(1)", "https://", "https://a.test/a b", ""}) {
    ExpectMutationError(
        [&](Json& j) { j["tables"]["canonicals"] = {{"/p", bad}}; },
        "tables.canonicals[\"/p\"]");
  }
  ExpectMutationError(
      [](Json& j) { j["tables"]["canonicals"] = {{"/p", "/relative"}}; },
      "URL must be absolute");
  ExpectMutationError(
      [](Json& j) {
        j["tables"]["canonicals"] = {
            {"/p", "https://a.test/" + std::string(kMaxUrlBytes, 'a')}};
      },
      "URL is longer than 2048 bytes");
}

TEST(PackLoaderErrorTest, TitleAndDescriptionTableLimits) {
  ExpectMutationError(
      [](Json& j) {
        j["tables"]["titles"] = {{"/", std::string(kMaxTitleChars + 1, 'a')}};
      },
      "a title must be 1 to 300 characters");
  ExpectMutationError([](Json& j) { j["tables"]["titles"] = {{"/", ""}}; },
                      "a title must be 1 to 300 characters");
  ExpectMutationError(
      [](Json& j) {
        j["tables"]["d"] = {{"/", std::string(kMaxDescriptionChars + 1, 'a')}};
        j["rules"][2]["value"] = {{"table", "d"}};
      },
      "a description must be 1 to 1000 characters");

  // Limits count characters, not bytes: 300 two-byte characters pass.
  Json ok = Base();
  std::string two_byte;
  for (size_t i = 0; i < kMaxTitleChars; ++i) two_byte += "\xc3\xa9";
  ok["tables"]["titles"] = {{"/", two_byte}};
  EXPECT_TRUE(Load(ok).ok());
}

TEST(PackLoaderErrorTest, TableValuesAreOnlyCheckedForTheKindThatUsesThem) {
  // A table nobody references may hold any string.
  Json j = Base();
  j["tables"]["unused"] = {{"/x", "not a url"}};
  EXPECT_TRUE(Load(j).ok());
}

TEST(PackLoaderErrorTest, InvalidHreflangCodeInTable) {
  ExpectMutationError(
      [](Json& j) {
        j["tables"]["hreflang"] = {{{"english", "https://a.test/"}}};
      },
      "tables.hreflang[0].english: not a valid hreflang code");
  ExpectMutationError(
      [](Json& j) {
        j["tables"]["hreflang"] = {
            {{"en", "https://a.test/"}, {"EN", "https://a.test/b"}}};
      },
      "duplicate hreflang code \"en\" in cluster");
}

TEST(PackLoaderErrorTest, RelativeUrlInHreflangTable) {
  ExpectMutationError(
      [](Json& j) { j["tables"]["hreflang"] = {{{"en", "/en/"}}}; },
      "tables.hreflang[0].en: URL must be absolute");
  ExpectMutationError([](Json& j) { j["tables"]["hreflang"] = {{{"en", 5}}}; },
                      "tables.hreflang[0].en: must be a string");
  ExpectMutationError(
      [](Json& j) { j["tables"]["hreflang"] = Json::array({Json::object()}); },
      "tables.hreflang[0]: cluster must not be empty");
}

TEST(PackLoaderErrorTest, TableEntryLimit) {
  auto many = [](size_t n) {
    Json t = Json::object();
    for (size_t i = 0; i < n; ++i) t[absl::StrCat("/p", i)] = "T";
    return t;
  };
  Json j = Base();
  j["tables"]["titles"] = many(kMaxTableEntries);
  j["tables"]["canonicals"] = Json::object();
  j["tables"]["hreflang"] = Json::array();
  auto ok = Load(j);
  ASSERT_TRUE(ok.ok()) << ok.status();

  j["tables"]["titles"] = many(kMaxTableEntries + 1);
  ExpectLoadError(j, "tables: holds more than 50000 entries in total");
}

TEST(PackLoaderErrorTest, TableEntryLimitIsTotalAcrossTables) {
  Json j = Base();
  Json a = Json::object();
  Json b = Json::object();
  for (size_t i = 0; i < kMaxTableEntries / 2 + 1; ++i) {
    a[absl::StrCat("/a", i)] = "T";
    b[absl::StrCat("/b", i)] = "T";
  }
  j["tables"] = {{"titles", a}, {"other", b}};
  ExpectLoadError(j, "tables: holds more than 50000 entries in total");
}

// ---- file limits ------------------------------------------------------------

TEST(PackLoaderErrorTest, OversizeFile) {
  std::string big = Base().dump();
  big.resize(kMaxPackFileBytes + 1, ' ');
  auto p = LoadPack(big, "", "big.json");
  ASSERT_FALSE(p.ok());
  EXPECT_NE(std::string(p.status().message()).find("file is 1048577 bytes"),
            std::string::npos);
  // Exactly at the limit is accepted (trailing whitespace is valid JSON).
  std::string at_limit = Base().dump();
  at_limit.resize(kMaxPackFileBytes, ' ');
  EXPECT_TRUE(LoadPack(at_limit, "").ok());
}

TEST(PackLoaderErrorTest, ExcessiveNesting) {
  std::string deep = R"({"x":)";
  for (size_t i = 0; i < kMaxJsonDepth + 5; ++i) deep += "[";
  for (size_t i = 0; i < kMaxJsonDepth + 5; ++i) deep += "]";
  deep += "}";
  ExpectRawLoadError(deep, "nested deeper than");
}

// Containers nested `levels` deep in total, the root object included, inside
// an unknown key (so a pack that gets past the reader fails on that key).
std::string NestedUnderUnknownKey(size_t levels) {
  std::string text = "{\"x\":";
  for (size_t i = 1; i < levels; ++i) text += "[";
  for (size_t i = 1; i < levels; ++i) text += "]";
  return text + "}";
}

TEST(PackLoaderErrorTest, NestingLimitIsTheStackLimit) {
  ExpectRawLoadError(NestedUnderUnknownKey(kMaxJsonDepth), "unknown key \"x\"");
  ExpectRawLoadError(NestedUnderUnknownKey(kMaxJsonDepth + 1),
                     "nested deeper than");
}

TEST(PackLoaderErrorTest, StrictReaderRefusesLooseJson) {
  ExpectRawLoadError("{'pack': 1}", "malformed JSON");      // single quotes
  ExpectRawLoadError("{\"pack\": NaN}", "malformed JSON");  // special floats
  ExpectRawLoadError("{\"pack\": Infinity}", "malformed JSON");
  ExpectRawLoadError("{\"pack\": [1,]}", "malformed JSON");  // trailing comma
  ExpectRawLoadError("{\"pack\": 1} {}", "malformed JSON");  // two documents
  ExpectRawLoadError("{\"pack\": /* c */ 1}", "malformed JSON");
  ExpectRawLoadError("{1: 2}", "malformed JSON");                // numeric key
  ExpectRawLoadError("{\"pack\": [1, , 2]}", "malformed JSON");  // dropped null
}

// ---- UTF-8 ------------------------------------------------------------------
//
// The JSON reader hands over whatever bytes a string held, so the loader checks
// every key and every string it accepts. The tests put a marker where the bad
// bytes go and splice the bytes into the serialized pack.

constexpr char kMarker[] = "BADBYTES";

void ExpectBadUtf8At(const std::function<void(Json&)>& place,
                     std::string_view needle) {
  const char* const kBad[] = {
      "\xC3\x28",          // invalid continuation byte
      "\xFF",              // never a valid byte
      "\xE2\x82",          // truncated three-byte sequence
      "\xED\xA0\x80",      // an encoded surrogate
      "\xC0\xAF",          // overlong encoding of '/'
      "\xF4\x90\x80\x80",  // above U+10FFFF
      "\x80",              // lone continuation byte
  };
  Json j = Base();
  place(j);
  const std::string good = j.dump();
  const size_t at = good.find(kMarker);
  ASSERT_NE(at, std::string::npos) << "marker not placed";
  for (const char* bad : kBad) {
    std::string text = good;
    text.replace(at, sizeof(kMarker) - 1, bad);
    auto p = LoadPack(text, "", "utf8.json");
    ASSERT_FALSE(p.ok()) << "accepted invalid UTF-8 at: " << needle;
    EXPECT_EQ(p.status().code(), absl::StatusCode::kInvalidArgument);
    EXPECT_NE(std::string(p.status().message()).find(needle), std::string::npos)
        << "message was: " << p.status().message() << "\nwanted: " << needle;
  }
}

TEST(PackLoaderUtf8Test, EveryStringPositionIsChecked) {
  const std::string kString = "string is not valid UTF-8";
  ExpectBadUtf8At([](Json& j) { j["pack"]["description"] = kMarker; },
                  "pack.description: " + kString);
  ExpectBadUtf8At([](Json& j) { j["pack"]["publisher"] = kMarker; },
                  "pack.publisher: " + kString);
  ExpectBadUtf8At([](Json& j) { j["pack"]["license"] = {{"terms", kMarker}}; },
                  "pack.license.terms: " + kString);
  ExpectBadUtf8At([](Json& j) { j["sites"][0]["host"] = kMarker; },
                  "sites[0].host: " + kString);
  ExpectBadUtf8At([](Json& j) { j["rules"][1]["id"] = kMarker; },
                  "rules[1].id: " + kString);
  ExpectBadUtf8At([](Json& j) { j["rules"][0]["match"]["paths"][0] = kMarker; },
                  "rules[0].match.paths[0]: " + kString);
  ExpectBadUtf8At(
      [](Json& j) { j["rules"][0]["match"]["path_regex"] = kMarker; },
      "rules[0].match.path_regex: " + kString);
  // Templates.
  ExpectBadUtf8At([](Json& j) { j["rules"][2]["value"]["template"] = kMarker; },
                  "rules[2].value.template: " + kString);
  ExpectBadUtf8At([](Json& j) { j["rules"][4]["value"]["template"] = kMarker; },
                  "rules[4].value.template: " + kString);
  ExpectBadUtf8At(
      [](Json& j) { j["rules"][0]["value"]["fallback_template"] = kMarker; },
      "rules[0].value.fallback_template: " + kString);
  // Table entries.
  ExpectBadUtf8At([](Json& j) { j["tables"]["titles"]["/"] = kMarker; },
                  "tables.titles./: " + kString);
  ExpectBadUtf8At(
      [](Json& j) { j["tables"]["canonicals"]["/p?id=1"] = kMarker; },
      "tables.canonicals./p?id=1: " + kString);
  ExpectBadUtf8At([](Json& j) { j["tables"]["hreflang"][0]["en"] = kMarker; },
                  "tables.hreflang[0].en: " + kString);
  // Pattern values.
  ExpectBadUtf8At(
      [](Json& j) {
        j["rules"][3]["value"] = {{"pattern", {{"en", kMarker}}}};
      },
      "rules[3].value.pattern.en: " + kString);
}

TEST(PackLoaderUtf8Test, EveryKeyPositionIsChecked) {
  const std::string kKey = "object key is not valid UTF-8";
  ExpectBadUtf8At([](Json& j) { j[kMarker] = 1; }, "$: " + kKey);
  ExpectBadUtf8At([](Json& j) { j["pack"][kMarker] = 1; }, "pack: " + kKey);
  ExpectBadUtf8At([](Json& j) { j["rules"][0]["match"][kMarker] = 1; },
                  "rules[0].match: " + kKey);
  ExpectBadUtf8At([](Json& j) { j["tables"]["titles"][kMarker] = "T"; },
                  "tables.titles: " + kKey);
  ExpectBadUtf8At([](Json& j) { j["tables"][kMarker] = Json::object(); },
                  "tables: " + kKey);
  ExpectBadUtf8At(
      [](Json& j) {
        j["tables"]["hreflang"][0][kMarker] = "https://www.example.com/x/";
      },
      "tables.hreflang[0]: " + kKey);
  ExpectBadUtf8At(
      [](Json& j) {
        j["rules"][3]["value"] = {{"pattern", {{kMarker, "https://x.test/"}}}};
      },
      "rules[3].value.pattern: " + kKey);
}

TEST(PackLoaderUtf8Test, ValidMultibyteTextIsAccepted) {
  Json j = Base();
  // 2-, 3- and 4-byte sequences, raw.
  j["tables"]["titles"]["/"] = "Caf\xC3\xA9 \xE2\x82\xAC \xF0\x9F\x98\x80";
  j["pack"]["description"] = "\xC3\xA9";
  auto p = Load(j);
  ASSERT_TRUE(p.ok()) << p.status();
  EXPECT_EQ(p->tables.at("titles").at("/"),
            "Caf\xC3\xA9 \xE2\x82\xAC \xF0\x9F\x98\x80");
  // The same text written as \u escapes (with a surrogate pair).
  auto q = LoadPack(
      R"JSON({"pack":{"id":"edge-seo","version":"0.1.0"},
              "sites":[{"host":"a.test"}],
              "tables":{"titles":{"/":"Caf\u00e9 \u20ac \ud83d\ude00"}},
              "rules":[{"id":"t","kind":"title","value":{"table":"titles"}}]})JSON",
      "");
  ASSERT_TRUE(q.ok()) << q.status();
  EXPECT_EQ(q->tables.at("titles").at("/"),
            "Caf\xC3\xA9 \xE2\x82\xAC \xF0\x9F\x98\x80");
}

TEST(PackLoaderUtf8Test, LoneSurrogateEscapesAreRefused) {
  for (const char* esc : {"\\ud800", "\\udc00", "\\ud800x", "\\udc00\\ud800"}) {
    const std::string text = absl::StrCat(
        R"JSON({"pack":{"id":"edge-seo","version":"0.1.0","description":")JSON",
        esc, R"JSON("},"sites":[{"host":"a.test"}],"rules":[]})JSON");
    auto p = LoadPack(text, "");
    EXPECT_FALSE(p.ok()) << esc;
  }
}

TEST(PackLoaderFileTest, MissingFile) {
  auto p = LoadPackFile("/nonexistent/dir/pack.json", "");
  ASSERT_FALSE(p.ok());
  EXPECT_EQ(p.status().code(), absl::StatusCode::kNotFound);
  EXPECT_NE(
      std::string(p.status().message()).find("/nonexistent/dir/pack.json"),
      std::string::npos);
}

TEST(PackLoaderFileTest, OversizeFileIsRefusedBeforeReading) {
  const std::string path = ::testing::TempDir() + "/oversize-pack.json";
  {
    std::ofstream out(path, std::ios::binary);
    out << std::string(kMaxPackFileBytes + 1, ' ');
  }
  auto p = LoadPackFile(path, "");
  std::remove(path.c_str());
  ASSERT_FALSE(p.ok());
  EXPECT_EQ(p.status().code(), absl::StatusCode::kInvalidArgument);
  EXPECT_NE(std::string(p.status().message()).find("file is 1048577 bytes"),
            std::string::npos);
}

TEST(PackLoaderFileTest, ErrorsCarryTheFilePath) {
  const std::string path = ::testing::TempDir() + "/bad-pack.json";
  {
    std::ofstream out(path, std::ios::binary);
    out << R"JSON({"pack":{"id":"X","version":"1.0.0"},"sites":[],"rules":[]})JSON";
  }
  auto p = LoadPackFile(path, "");
  std::remove(path.c_str());
  ASSERT_FALSE(p.ok());
  EXPECT_EQ(std::string(p.status().message()),
            path + ": pack.id: must match [a-z0-9-]{1,64}");
}

// ---- lint -------------------------------------------------------------------

TEST(PackLoaderLintTest, ShadowedRuleIsReportedNotRejected) {
  Json j = Base();
  j["rules"] = Json::array(
      {{{"id", "all"}, {"kind", "title"}, {"value", {{"template", "A"}}}},
       {{"id", "later"},
        {"kind", "title"},
        {"match", {{"paths", {"/docs/**", "/about"}}}},
        {"value", {{"template", "B"}}}}});
  auto p = Load(j);
  ASSERT_TRUE(p.ok()) << p.status();
  ASSERT_EQ(p->warnings.size(), 1u);
  EXPECT_NE(p->warnings[0].find("rule \"later\" (title) can never apply"),
            std::string::npos);
  EXPECT_NE(p->warnings[0].find("rule \"all\""), std::string::npos);
}

TEST(PackLoaderLintTest, NoWarningWhenSpecificRuleComesFirst) {
  Json j = Base();
  j["rules"] = Json::array(
      {{{"id", "docs"},
        {"kind", "title"},
        {"match", {{"paths", {"/docs/**"}}}},
        {"value", {{"template", "A"}}}},
       {{"id", "all"}, {"kind", "title"}, {"value", {{"template", "B"}}}}});
  auto p = Load(j);
  ASSERT_TRUE(p.ok());
  EXPECT_TRUE(p->warnings.empty());
}

TEST(PackLoaderLintTest,
     NoWarningAcrossKindsDisabledRulesOrNarrowEarlierRules) {
  Json j = Base();
  j["rules"] = Json::array(
      {{{"id", "t1"}, {"kind", "title"}, {"value", {{"template", "A"}}}},
       {{"id", "d1"}, {"kind", "description"}, {"value", {{"template", "A"}}}},
       {{"id", "t2-off"},
        {"kind", "title"},
        {"enabled", false},
        {"value", {{"template", "B"}}}}});
  auto p = Load(j);
  ASSERT_TRUE(p.ok());
  EXPECT_TRUE(p->warnings.empty());

  // An earlier rule with a regex or an exclude list does not cover all of
  // its glob, so it does not shadow a later rule.
  j["rules"] = Json::array(
      {{{"id", "t1"},
        {"kind", "title"},
        {"match", {{"exclude_paths", {"/x/**"}}}},
        {"value", {{"template", "A"}}}},
       {{"id", "t2"}, {"kind", "title"}, {"value", {{"template", "B"}}}}});
  p = Load(j);
  ASSERT_TRUE(p.ok());
  EXPECT_TRUE(p->warnings.empty());
}

TEST(PackLoaderLintTest, DisabledEarlierRuleDoesNotShadow) {
  Json j = Base();
  j["rules"] = Json::array(
      {{{"id", "t1"},
        {"kind", "title"},
        {"enabled", false},
        {"value", {{"template", "A"}}}},
       {{"id", "t2"}, {"kind", "title"}, {"value", {{"template", "B"}}}}});
  auto p = Load(j);
  ASSERT_TRUE(p.ok());
  EXPECT_TRUE(p->warnings.empty());
}

// ---- review hardening ---------------------------------------------------------

Json RuleWithGlobs(const std::string& id, size_t count, size_t offset = 0) {
  Json globs = Json::array();
  for (size_t g = 0; g < count; ++g) {
    globs.push_back(absl::StrCat("/g", offset + g, "/**"));
  }
  return {{"id", id},
          {"kind", "title"},
          {"match", {{"paths", globs}}},
          {"value", {{"template", "T"}}}};
}

TEST(PackLoaderGlobLimitTest, TooManyGlobsInOneRule) {
  Json j = Base();
  j["rules"] = Json::array({RuleWithGlobs("a", kMaxGlobsPerRule + 1)});
  ExpectLoadError(j, "rules[0].match.paths: has 33 globs; the limit is 32");
  j["rules"] = Json::array({RuleWithGlobs("a", kMaxGlobsPerRule)});
  EXPECT_TRUE(Load(j).ok());
  Json k = Base();
  Json excludes = Json::array();
  for (size_t g = 0; g < kMaxGlobsPerRule + 1; ++g) {
    excludes.push_back(absl::StrCat("/x", g));
  }
  k["rules"][1]["match"] = {{"exclude_paths", excludes}};
  ExpectLoadError(k, "rules[1].match.exclude_paths: has 33 globs");
}

TEST(PackLoaderGlobLimitTest, TwoHundredRulesOf33GlobsIsALoadError) {
  Json j = Base();
  Json rules = Json::array();
  for (size_t i = 0; i < 200; ++i) {
    rules.push_back(RuleWithGlobs(absl::StrCat("r", i), 33));
  }
  j["rules"] = rules;
  ExpectLoadError(j, "has 33 globs; the limit is 32");
}

TEST(PackLoaderGlobLimitTest, TwoHundredRulesOf32GlobsLoadsAndMatches) {
  Json j = Base();
  Json rules = Json::array();
  for (size_t i = 0; i < 200; ++i) {
    rules.push_back(RuleWithGlobs(absl::StrCat("r", i), 32));  // shared globs
  }
  j["rules"] = rules;
  auto p = Load(j);
  ASSERT_TRUE(p.ok()) << p.status();
  EXPECT_TRUE(MatchRulePath(p->rules[199], "/g31/x").has_value());
  EXPECT_FALSE(MatchRulePath(p->rules[199], "/g32/x").has_value());
}

TEST(PackLoaderGlobLimitTest, DistinctGlobTotalIsCapped) {
  Json j = Base();
  Json rules = Json::array();
  // 63 rules x 32 distinct globs = 2016 > 2000.
  for (size_t i = 0; i < 63; ++i) {
    rules.push_back(RuleWithGlobs(absl::StrCat("r", i), 32, i * 32));
  }
  j["rules"] = rules;
  ExpectLoadError(j, "the pack uses more than 2000 distinct globs");
  rules.pop_back();  // 62 x 32 = 1984
  j["rules"] = rules;
  EXPECT_TRUE(Load(j).ok());
}

TEST(PackLoaderHtmlTextTest, ControlCharactersInTitleValuesAreRejected) {
  for (const char* bad : {"A\0B", "A\x01 B", "A\nB", "A\tB", "A\x7f"}) {
    Json j = Base();
    j["tables"]["titles"] = {
        {"/", std::string(bad, bad[1] == '\0' ? 3 : strlen(bad))}};
    ExpectLoadError(j,
                    "a title cannot contain NUL or other control characters");
  }
  Json j = Base();
  j["tables"]["titles"]["/"] = Json::parse("\"a\\u0000b\"");
  ExpectLoadError(j, "a title cannot contain NUL");
}

TEST(PackLoaderHtmlTextTest, ControlCharactersInDescriptionAndTemplates) {
  ExpectMutationError(
      [](Json& j) {
        j["tables"]["d"]["/"] = Json::parse("\"x\\u0000\"");
        j["rules"][2]["value"] = {{"table", "d"}};
      },
      "a description cannot contain NUL");
  ExpectMutationError(
      [](Json& j) {
        j["rules"][2]["value"] = {{"template", Json::parse("\"a\\u0001b\"")}};
      },
      "template cannot contain NUL or other control characters");
  ExpectMutationError(
      [](Json& j) {
        j["rules"][1]["value"] = {{"template", Json::parse("\"a\\nb\"")}};
      },
      "template cannot contain NUL or other control characters");
}

TEST(PackLoaderUrlValueTest, UnsafeCharactersInUrlTableValues) {
  for (const char* bad : {"https://a.test/\"x", "https://a.test/<x>",
                          "https://a.test/a\\b", "https://a.test/x>"}) {
    ExpectMutationError(
        [&](Json& j) { j["tables"]["canonicals"] = {{"/p", bad}}; },
        "backslash or one of the characters");
    ExpectMutationError(
        [&](Json& j) { j["tables"]["hreflang"] = {{{"en", bad}}}; },
        "tables.hreflang[0].en: URL contains a backslash");
  }
}

TEST(PackLoaderHostTest, WildcardNeedsTwoLabels) {
  for (const char* bad : {"*.com", "*.localhost"}) {
    ExpectMutationError([&](Json& j) { j["sites"][0]["host"] = bad; },
                        "a wildcard needs at least two labels");
  }
  Json j = Base();
  j["sites"][0]["host"] = "*.example.co.uk";
  EXPECT_TRUE(Load(j).ok());
}

TEST(PackLoaderClusterTest, UrlInTwoClustersIsALoadError) {
  ExpectMutationError(
      [](Json& j) {
        j["tables"]["hreflang"] = {
            {{"en", "https://a.test/en/"}, {"de", "https://a.test/de/"}},
            {{"en", "https://a.test/EN/x"}, {"fr", "https://A.test/de"}}};
      },
      "tables.hreflang[1]: URL https://a.test/de is also in cluster 0");
}

TEST(PackLoaderClusterTest, SameUrlTwiceInOneClusterIsFine) {
  Json j = Base();
  j["tables"]["hreflang"] = {
      {{"en", "https://a.test/en/"}, {"x-default", "https://a.test/en/"}}};
  auto p = Load(j);
  ASSERT_TRUE(p.ok()) << p.status();
  EXPECT_EQ(p->cluster_index.size(), 1u);
}

TEST(PackLoaderRegexTest, RegexMatchingIsByteExact) {
  Json j = Base();
  j["rules"][0]["match"]["path_regex"] = "^/p/(.+)$";
  auto p = Load(j);
  ASSERT_TRUE(p.ok()) << p.status();
  auto m = MatchRulePath(p->rules[0], "/p/\xff\xfe");
  ASSERT_TRUE(m.has_value());
  EXPECT_EQ((*m)[0], "\xff\xfe");
}

TEST(PackLoaderDepthTest, DeepNestingIsRejectedWithoutBuildingIt) {
  std::string deep = R"({"x":)";
  for (int i = 0; i < 5000; ++i) deep += "[";
  for (int i = 0; i < 5000; ++i) deep += "]";
  deep += "}";
  ExpectRawLoadError(deep, "nested deeper than");
}

TEST(PackLoaderFileTest, ReadsAtMostTheLimitPlusOneByte) {
  const std::string path = ::testing::TempDir() + "/exact-pack.json";
  std::string text = Base().dump();
  text.resize(kMaxPackFileBytes, ' ');
  {
    std::ofstream out(path, std::ios::binary);
    out << text;
  }
  auto p = LoadPackFile(path, "");
  std::remove(path.c_str());
  EXPECT_TRUE(p.ok()) << p.status();
}

TEST(PackLoaderLintTest, EarlierExcludesMustBeASubsetOfTheLaterOnes) {
  Json j = Base();
  // Earlier rule skips /x/**; the later rule applies there, so not shadowed.
  j["rules"] = Json::array(
      {{{"id", "t1"},
        {"kind", "title"},
        {"match", {{"exclude_paths", {"/x/**"}}}},
        {"value", {{"template", "A"}}}},
       {{"id", "t2"}, {"kind", "title"}, {"value", {{"template", "B"}}}}});
  auto p = Load(j);
  ASSERT_TRUE(p.ok());
  EXPECT_TRUE(p->warnings.empty());

  // Later rule excludes at least what the earlier one does: shadowed.
  j["rules"][1]["match"] = {{"exclude_paths", {"/x/**", "/y"}}};
  p = Load(j);
  ASSERT_TRUE(p.ok());
  EXPECT_EQ(p->warnings.size(), 1u);
}

}  // namespace
}  // namespace pagespeed::packs
