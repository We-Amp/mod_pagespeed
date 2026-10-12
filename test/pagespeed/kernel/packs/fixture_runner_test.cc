// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

// Runs every case under testdata/edge-seo/fixtures through
// HtmlParse + RulePackFilter + HtmlWriterFilter and checks:
//   1. enforce: output and decisions equal the expected files;
//   2. idempotence: the expected output run again is unchanged, and every
//      enforcing decision on it is a no-op;
//   3. report-only: the output equals the same parse without the filter, and
//      the decisions equal the expected ones with the mode forced to report;
//   4. determinism: a second run gives the same output and decisions;
// and, for each chunking (a flush every 16, 64 and 256 bytes, right after
// <head>, right before </head>):
//   5. the output is either the whole-document output or the output without
//      the filter, and the decisions are either the whole-document ones or
//      the same rules with action `none` and reason `not_rewritable`; never
//      anything else. When the output is the one without the filter, a
//      decision kept from the whole-document run must not say an enforcing
//      rule changed something.
//      Report mode stays byte-identical in every chunking.
//
// The expected files are shared with other engines that run the same packs.
// Where this filter deliberately differs, the case carries
// expected.module.html / expected.module.decisions.jsonl, and the case must
// be listed in ModuleOverrides() (fixture_runner.cc) with the reason. A new difference
// therefore fails here until it is reviewed and listed.

#include "test/pagespeed/kernel/packs/fixture_runner.h"

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <map>
#include <set>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include "absl/strings/str_cat.h"
#include "gtest/gtest.h"
#include "pagespeed/kernel/html/html_keywords.h"
#include "pagespeed/kernel/packs/decision.h"
#include "pagespeed/kernel/packs/json_util.h"
#include "pagespeed/kernel/packs/pack.h"
#include "pagespeed/kernel/packs/planner_common.h"
#include "pagespeed/kernel/packs/url_norm.h"
#include "re2/re2.h"

namespace pagespeed::packs {
namespace {

constexpr char kFixturesDir[] =
    "test/pagespeed/kernel/packs/testdata/edge-seo/fixtures";

std::vector<FixtureCase> Cases() { return ListFixtureCases(kFixturesDir); }

class FixtureTest : public testing::TestWithParam<FixtureCase> {};

TEST_P(FixtureTest, HoldsTheProperties) {
  const FixtureCase& c = GetParam();
  auto run = RunCase(kFixturesDir, c);
  ASSERT_TRUE(run.ok()) << run.status();
  auto expected = ReadExpected(c);
  ASSERT_TRUE(expected.ok()) << expected.status();
  const std::string& host = run->host;
  const std::string& path = run->path;

  // 1. enforce
  EXPECT_EQ(run->enforce.html, expected->html);
  if (run->has_origin_headers) {
    EXPECT_EQ(HeadersToText(run->enforce.headers), expected->headers);
  } else {
    EXPECT_TRUE(expected->headers.empty());
  }
  EXPECT_EQ(DecisionsToJsonl(host, path, run->enforce.decisions, false),
            expected->decisions);
  EXPECT_EQ(run->enforce.modified, run->roundtrip.html != expected->html);

  // 2. idempotence
  EXPECT_EQ(run->idempotent.html, expected->html);
  EXPECT_FALSE(run->idempotent.modified);
  EXPECT_FALSE(run->idempotent.headers_modified);
  EXPECT_EQ(run->idempotent.headers, run->enforce.headers);
  // (A report-only rule keeps reporting what it would do; it never ran.)
  for (const PackDecision& d : run->idempotent.decisions) {
    if (d.mode != Mode::kEnforce) continue;
    EXPECT_EQ(d.action, Action::kNone)
        << d.rule_id << ": " << ReasonName(d.reason);
  }

  // 3. report-only: byte-identical (when debug comments are off; with them
  // on, report mode adds would= comments and nothing else), nothing
  // modified, same decisions.
  if (run->debug) {
    EXPECT_EQ(StripPackComments(run->report.html), run->roundtrip.html);
    bool acts = false;
    for (const PackDecision& d : run->report.decisions) {
      acts = acts || d.action != Action::kNone;
    }
    // (A plan dropped as not_rewritable writes no comments.)
    if (run->report.skip != SkipReason::kNotRewritable) {
      EXPECT_EQ(acts, run->report.html.find(" would=") != std::string::npos);
    }
  } else {
    EXPECT_EQ(run->report.html, run->roundtrip.html);
  }
  EXPECT_FALSE(run->report.modified);
  // Report mode never changes the response headers.
  EXPECT_FALSE(run->report.headers_modified);
  EXPECT_EQ(run->report.headers, run->origin_headers);
  const std::string forced =
      DecisionsToJsonl(host, path, run->enforce.decisions, true);
  EXPECT_EQ(DecisionsToJsonl(host, path, run->report.decisions, false), forced);

  const std::string forced_document = DecisionsToJsonl(
      host, path, DocumentDecisions(run->enforce.decisions), true);

  // 4. determinism
  EXPECT_EQ(run->again.html, run->enforce.html);
  EXPECT_EQ(run->again.decisions, run->enforce.decisions);
  EXPECT_EQ(run->again.headers, run->enforce.headers);

  // 5. chunked
  for (const ChunkedRun& cr : run->chunked) {
    SCOPED_TRACE(cr.chunking.name);
    // The kernel's own output does not depend on the flush windows.
    EXPECT_EQ(cr.roundtrip.html, run->roundtrip.html);
    std::string why;
    EXPECT_TRUE(ChunkedOutcomeAllowed(
        run->enforce, cr.enforce, cr.roundtrip.html, &why, nullptr,
        &run->head_only.html, &run->document_only.html))
        << why;
    EXPECT_TRUE(
        ChunkedHeadersAllowed(run->enforce, cr.enforce, run->origin_headers))
        << "headers:\n"
        << HeadersToText(cr.enforce.headers)
        << "decisions: " << DescribeDecisions(cr.enforce.decisions);
    EXPECT_EQ(cr.report.headers, run->origin_headers);
    EXPECT_EQ(run->debug ? StripPackComments(cr.report.html) : cr.report.html,
              cr.roundtrip.html);
    EXPECT_EQ(DecisionsToJsonl(host, path,
                               DocumentDecisions(cr.report.decisions), false),
              forced_document);
    EXPECT_TRUE(
        ChunkedHeadersAllowed(run->report, cr.report, run->origin_headers))
        << "decisions: " << DescribeDecisions(cr.report.decisions);
  }
}

INSTANTIATE_TEST_SUITE_P(Cases, FixtureTest, testing::ValuesIn(Cases()),
                         [](const testing::TestParamInfo<FixtureCase>& info) {
                           std::string n = info.param.name;
                           for (char& ch : n) {
                             if (ch == '-') ch = '_';
                           }
                           return n;
                         });

// The chunked property is not vacuous: some chunkings keep the whole-document
// result although a flush fell inside the head, and some drop a head plan
// that needed an element the server already sent.
TEST(FixtureSetTest, ChunkingsReachBothOutcomes) {
  int whole_with_split_head = 0;
  int dropped = 0;
  for (const FixtureCase& c : Cases()) {
    auto run = RunCase(kFixturesDir, c);
    ASSERT_TRUE(run.ok()) << c.name << ": " << run.status();
    if (run->enforce.html == run->roundtrip.html) continue;  // nothing to do
    for (const ChunkedRun& cr : run->chunked) {
      if (cr.enforce.skip == SkipReason::kNotRewritable) {
        ++dropped;
      } else if (cr.chunking.name == "after-head" ||
                 cr.chunking.name == "before-head-close") {
        ++whole_with_split_head;
      }
    }
  }
  std::printf(
      "chunked runs: %d kept the whole-document result with a split "
      "head, %d dropped the head plan\n",
      whole_with_split_head, dropped);
  EXPECT_GT(whole_with_split_head, 0);
  EXPECT_GT(dropped, 0);
}

TEST(FixtureSetTest, HasTheMinimumCoverage) { EXPECT_GE(Cases().size(), 160u); }

// ---------------------------------------------------------------------------
// Properties of what the multi-element kinds emit, over every case
// ---------------------------------------------------------------------------

// The `<link rel=alternate hreflang=...>` tags of `html`, as (code, href)
// with the href entity-decoded.
std::vector<std::pair<std::string, std::string>> AlternateLinks(
    std::string_view html) {
  static const RE2* rel =
      new RE2("(?is)\\brel\\s*=\\s*[\"']?alternate\\b");
  static const RE2* code = new RE2(
      "(?is)\\bhreflang\\s*=\\s*(?:\"([^\"]*)\"|'([^']*)'|([^\\s>]+))");
  static const RE2* href = new RE2(
      "(?is)\\bhref\\s*=\\s*(?:\"([^\"]*)\"|'([^']*)'|([^\\s>]+))");
  std::vector<std::pair<std::string, std::string>> out;
  re2::StringPiece input(html.data(), html.size());
  std::string text;
  while (RE2::FindAndConsume(&input, "(?is)(<link\\b[^>]*>)", &text)) {
    if (!RE2::PartialMatch(text, *rel)) continue;
    std::string c1, c2, c3, h1, h2, h3;
    if (!RE2::PartialMatch(text, *code, &c1, &c2, &c3)) continue;
    RE2::PartialMatch(text, *href, &h1, &h2, &h3);
    out.emplace_back(c1 + c2 + c3, DecodeAttribute(h1 + h2 + h3));
  }
  return out;
}

// The text of every `<script type="application/ld+json">` block of `html`.
std::vector<std::string> JsonLdBlocks(std::string_view html) {
  static const RE2* block = new RE2(
      "(?is)<script\\b[^>]*\\btype\\s*=\\s*[\"']?application/ld\\+json[\"']?"
      "[^>]*>(.*?)</script>");
  std::vector<std::string> out;
  re2::StringPiece input(html.data(), html.size());
  std::string text;
  while (RE2::FindAndConsume(&input, *block, &text)) out.push_back(text);
  return out;
}

bool Acted(const std::vector<PackDecision>& decisions, std::string_view kind,
           bool inserted_or_rewrote_only) {
  for (const PackDecision& d : decisions) {
    if (d.kind->name() != kind || d.mode != Mode::kEnforce) continue;
    if (inserted_or_rewrote_only) {
      if (d.action == Action::kInsert || d.action == Action::kReplace ||
          d.action == Action::kFill) {
        return true;
      }
    } else if (d.action != Action::kNone) {
      return true;
    }
  }
  return false;
}

// No hreflang cluster the filter writes lacks the entry for the page, and no
// code it writes fails the language-code check a lens applies.
TEST(FixtureSetTest, EmittedHreflangClustersHaveTheirSelfEntryAndValidCodes) {
  net_instaweb::HtmlKeywords::Init();
  static const RE2* lens_code = new RE2(
      "^(x-default|[a-z]{2,3}(-[a-z]{4})?(-([a-z]{2}|\\d{3}))?)$");
  int clusters = 0;
  for (const FixtureCase& c : Cases()) {
    auto run = RunCase(kFixturesDir, c);
    ASSERT_TRUE(run.ok()) << c.name << ": " << run.status();
    if (!Acted(run->enforce.decisions, "hreflang", true)) continue;
    ++clusters;
    const auto links = AlternateLinks(run->enforce.html);
    const auto before = AlternateLinks(run->input_html);
    const std::string page = NormUrl(run->url).value_or("?");
    bool self = false;
    for (const auto& [code, href] : links) {
      // (A link the page already had and the rule left alone is not the
      // rule's output.)
      if (std::find(before.begin(), before.end(),
                    std::make_pair(code, href)) != before.end()) {
        self = self || NormUrl(href) == page;
        continue;
      }
      EXPECT_TRUE(RE2::FullMatch(code, *lens_code))
          << c.name << ": emitted code \"" << code << "\"";
      self = self || NormUrl(href) == page;
    }
    EXPECT_TRUE(self) << c.name << ": the cluster has no entry for " << page;
  }
  EXPECT_GE(clusters, 10);
}

// What the filter writes into a JSON-LD block reads with the strict reader
// the pack loader uses (and so with a browser's JSON.parse()).
TEST(FixtureSetTest, EmittedJsonLdBlocksPassTheStrictReader) {
  net_instaweb::HtmlKeywords::Init();
  int blocks = 0;
  for (const FixtureCase& c : Cases()) {
    auto run = RunCase(kFixturesDir, c);
    ASSERT_TRUE(run.ok()) << c.name << ": " << run.status();
    if (!Acted(run->enforce.decisions, "jsonld", true)) continue;
    const auto before = JsonLdBlocks(run->input_html);
    for (const std::string& text : JsonLdBlocks(run->enforce.html)) {
      if (std::find(before.begin(), before.end(), text) != before.end()) {
        continue;  // the page's own
      }
      ++blocks;
      Json::Value parsed;
      EXPECT_TRUE(ParseStrictJson(text, kMaxJsonLdDepth, true, &parsed))
          << c.name << ": " << text;
      EXPECT_EQ(text.find("</"), std::string::npos) << c.name;
    }
  }
  EXPECT_GE(blocks, 10);
}

// The cases with module-specific expected files are exactly the listed ones,
// and each still differs from the shared files (an override that matches the
// shared result is stale and must go).
TEST(FixtureSetTest, ModuleOverridesAreExactlyTheListedOnes) {
  std::set<std::string> found;
  for (const FixtureCase& c : Cases()) {
    if (!HasModuleOverride(c)) continue;
    found.insert(c.name);
    auto shared_html = ReadFile(c.dir + "/expected.html");
    auto shared_decisions = ReadFile(c.dir + "/expected.decisions.jsonl");
    auto expected = ReadExpected(c);
    ASSERT_TRUE(shared_html.ok() && shared_decisions.ok() && expected.ok())
        << c.name;
    EXPECT_TRUE(expected->html != *shared_html ||
                expected->decisions != *shared_decisions)
        << c.name << ": the module result equals the shared one";
  }
  std::set<std::string> listed;
  for (const auto& [name, reason] : ModuleOverrides()) {
    EXPECT_FALSE(reason.empty()) << name;
    listed.insert(name);
  }
  EXPECT_EQ(found, listed);
}

// The reader opens the deepest fixture file. On Windows its absolute path
// under the runfiles tree is longer than MAX_PATH; the reader must still open
// it, and name paths with forward slashes only.
TEST(FixtureReaderTest, ReadsTheDeepestFixtureFile) {
  std::string deepest;
  for (const FixtureCase& c : Cases()) {
    EXPECT_EQ(c.dir.find('\\'), std::string::npos) << c.dir;
    const std::string path = c.dir + "/expected.decisions.jsonl";
    if (path.size() > deepest.size()) deepest = path;
  }
  ASSERT_FALSE(deepest.empty());
  std::error_code ec;
  const std::string absolute =
      std::filesystem::absolute(deepest, ec).generic_string();
  std::printf("deepest fixture file: %zu bytes of absolute path\n",
              absolute.size());
  auto text = ReadFile(deepest);
  ASSERT_TRUE(text.ok()) << text.status();
  EXPECT_FALSE(text->empty());

  auto missing = ReadFile(deepest + ".missing");
  ASSERT_FALSE(missing.ok());
  EXPECT_EQ(std::string(missing.status().message()).find('\\'),
            std::string::npos)
      << missing.status();
}

}  // namespace
}  // namespace pagespeed::packs
