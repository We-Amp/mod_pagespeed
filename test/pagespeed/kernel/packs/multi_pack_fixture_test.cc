// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

// Runs every case under testdata/multi-pack through HtmlParse + one
// RulePackFilter per pack + HtmlWriterFilter, the way a server runs several
// packs on one virtual host, and checks the properties of the single-pack
// runner for the packs together:
//   1. enforce: output (and response headers) and decisions equal the expected
//      files; the decisions are those of the first pack, then the second;
//   2. idempotence: the expected output run again is unchanged;
//   3. report-only: the output equals the same parse without the filters and
//      the decisions equal the expected ones with the mode forced to report
//      (ownership of a target does not depend on the mode);
//   4. determinism;
//   5. each chunking: per pack, the whole-document result or the pack's plan
//      dropped as not_rewritable (a dropped plan still holds its claims), and
//      the `claimed` decisions are identical in every chunking.

#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "absl/strings/str_cat.h"
#include "gtest/gtest.h"
#include "pagespeed/kernel/packs/decision.h"
#include "pagespeed/kernel/packs/pack.h"
#include "pagespeed/kernel/packs/pack_loader.h"
#include "test/pagespeed/kernel/packs/fixture_runner.h"

namespace pagespeed::packs {
namespace {

constexpr char kFixturesDir[] =
    "test/pagespeed/kernel/packs/testdata/multi-pack";

std::vector<FixtureCase> Cases() { return ListFixtureCases(kFixturesDir); }

// The `claimed` decisions as (rule, owner) lines.
std::vector<std::string> ClaimedLines(const std::vector<PackDecision>& all) {
  std::vector<std::string> out;
  for (const PackDecision& d : all) {
    if (d.reason == Reason::kClaimed) {
      out.push_back(absl::StrCat(d.rule_id, "<-", d.claimed_by));
    }
  }
  return out;
}

class MultiPackFixtureTest : public testing::TestWithParam<FixtureCase> {};

TEST_P(MultiPackFixtureTest, HoldsTheProperties) {
  const FixtureCase& c = GetParam();
  auto run = RunCase(kFixturesDir, c);
  ASSERT_TRUE(run.ok()) << run.status();
  auto expected = ReadExpected(c);
  ASSERT_TRUE(expected.ok()) << expected.status();
  EXPECT_FALSE(HasModuleOverride(c));

  // 1. enforce
  EXPECT_EQ(run->enforce.html, expected->html);
  if (run->has_origin_headers) {
    EXPECT_EQ(HeadersToText(run->enforce.headers), expected->headers);
  }
  EXPECT_EQ(
      DecisionsToJsonl(run->host, run->path, run->enforce.decisions, false),
      expected->decisions);

  // 2. idempotence
  EXPECT_EQ(run->idempotent.html, expected->html);
  EXPECT_FALSE(run->idempotent.modified);
  EXPECT_EQ(run->idempotent.headers, run->enforce.headers);
  for (const PackDecision& d : run->idempotent.decisions) {
    if (d.mode != Mode::kEnforce) continue;
    EXPECT_EQ(d.action, Action::kNone) << d.rule_id;
  }

  // 3. report-only
  EXPECT_EQ(run->report.html, run->roundtrip.html);
  EXPECT_FALSE(run->report.modified);
  EXPECT_FALSE(run->report.headers_modified);
  EXPECT_EQ(run->report.headers, run->origin_headers);
  EXPECT_EQ(
      DecisionsToJsonl(run->host, run->path, run->report.decisions, false),
      DecisionsToJsonl(run->host, run->path, run->enforce.decisions, true));
  EXPECT_EQ(ClaimedLines(run->report.decisions),
            ClaimedLines(run->enforce.decisions));
  EXPECT_EQ(run->report.claimed_count, run->enforce.claimed_count);
  EXPECT_EQ(run->enforce.claimed_count,
            static_cast<int64_t>(ClaimedLines(run->enforce.decisions).size()));

  // 4. determinism
  EXPECT_EQ(run->again.html, run->enforce.html);
  EXPECT_EQ(run->again.decisions, run->enforce.decisions);
  EXPECT_EQ(run->again.headers, run->enforce.headers);

  // 5. chunked
  const std::vector<std::string> claimed = ClaimedLines(run->enforce.decisions);
  const std::string forced_document = DecisionsToJsonl(
      run->host, run->path, DocumentDecisions(run->enforce.decisions), true);
  for (const ChunkedRun& cr : run->chunked) {
    SCOPED_TRACE(cr.chunking.name);
    EXPECT_EQ(cr.roundtrip.html, run->roundtrip.html);
    std::string why;
    if (run->drop_states.empty()) {
      EXPECT_TRUE(ChunkedOutcomeAllowed(
          run->enforce, cr.enforce, cr.roundtrip.html, &why, nullptr,
          &run->head_only.html, &run->document_only.html))
          << why;
    } else {
      EXPECT_TRUE(ChunkedOutcomeAllowedForPacks(run->enforce, *run, cr.enforce,
                                                cr.roundtrip.html, &why))
          << why;
    }
    EXPECT_EQ(ClaimedLines(cr.enforce.decisions), claimed);
    EXPECT_EQ(ClaimedLines(cr.report.decisions), claimed);
    EXPECT_EQ(cr.enforce.claimed_count, run->enforce.claimed_count);
    EXPECT_TRUE(
        ChunkedHeadersAllowed(run->enforce, cr.enforce, run->origin_headers))
        << DescribeDecisions(cr.enforce.decisions);
    EXPECT_EQ(cr.report.headers, run->origin_headers);
    EXPECT_EQ(cr.report.html, cr.roundtrip.html);
    EXPECT_EQ(DecisionsToJsonl(run->host, run->path,
                               DocumentDecisions(cr.report.decisions), false),
              forced_document);
  }
}

INSTANTIATE_TEST_SUITE_P(Cases, MultiPackFixtureTest,
                         testing::ValuesIn(Cases()),
                         [](const testing::TestParamInfo<FixtureCase>& info) {
                           std::string n = info.param.name;
                           for (char& ch : n) {
                             if (ch == '-') ch = '_';
                           }
                           return n;
                         });

TEST(MultiPackFixtureSetTest, HasTheNineCases) {
  std::vector<std::string> names;
  for (const FixtureCase& c : Cases()) names.push_back(c.name);
  EXPECT_EQ(names,
            (std::vector<std::string>{
                "claim-held-on-split-head", "disjoint-kinds", "duplicate-id",
                "jsonld-and-head", "link-header-claim",
                "same-kind-first-report", "same-kind-host-mismatch",
                "same-kind-keep-first", "single-pack-two-same-kind-rules"}));
}

// What each case is about, stated once: who holds what.
TEST(MultiPackFixtureSetTest, ClaimsAreWhereTheCasesSayTheyAre) {
  const std::map<std::string, std::vector<std::string>> want = {
      {"claim-held-on-split-head", {"b-title-split<-edge-seo"}},
      {"disjoint-kinds", {}},
      {"duplicate-id", {}},
      {"jsonld-and-head", {}},
      {"link-header-claim", {"b-canonical-link<-edge-seo"}},
      {"same-kind-first-report", {"b-canonical-report<-edge-seo"}},
      {"same-kind-host-mismatch", {}},
      {"same-kind-keep-first", {"b-canonical-keep<-edge-seo"}},
      {"single-pack-two-same-kind-rules", {}},
  };
  for (const FixtureCase& c : Cases()) {
    auto run = RunCase(kFixturesDir, c);
    ASSERT_TRUE(run.ok()) << c.name << ": " << run.status();
    ASSERT_EQ(want.count(c.name), 1u) << c.name;
    EXPECT_EQ(ClaimedLines(run->enforce.decisions), want.at(c.name)) << c.name;
  }
}

// claim-held-on-split-head: in a chunking that drops the first pack's head
// plan the page is the pack-free one, and the second pack stays claimed.
TEST(MultiPackFixtureSetTest, AClaimIsHeldWhenTheOwnersHeadPlanDrops) {
  int dropped = 0;
  for (const FixtureCase& c : Cases()) {
    if (c.name != "claim-held-on-split-head") continue;
    auto run = RunCase(kFixturesDir, c);
    ASSERT_TRUE(run.ok()) << run.status();
    for (const ChunkedRun& cr : run->chunked) {
      if (cr.enforce.html != run->roundtrip.html) continue;
      ++dropped;
      EXPECT_EQ(ClaimedLines(cr.enforce.decisions),
                (std::vector<std::string>{"b-title-split<-edge-seo"}))
          << cr.chunking.name;
      ASSERT_FALSE(cr.enforce.decisions.empty());
      EXPECT_EQ(cr.enforce.decisions[0].reason, Reason::kNotRewritable)
          << cr.chunking.name;
    }
  }
  EXPECT_GT(dropped, 0);
}

// The duplicate-id case: the second file is left out of the run, and the run
// is the first pack's alone.
TEST(MultiPackFixtureSetTest, ADuplicateIdFileIsLeftOut) {
  for (const FixtureCase& c : Cases()) {
    if (c.name != "duplicate-id") continue;
    auto run = RunCase(kFixturesDir, c);
    ASSERT_TRUE(run.ok()) << run.status();
    EXPECT_EQ(run->pack_files.size(), 2u);
    EXPECT_EQ(run->refused, (std::vector<bool>{false, true}));
    for (const PackDecision& d : run->enforce.decisions) {
      EXPECT_NE(d.rule_id, "dup-title");
    }
  }
}

// disjoint-kinds: the two packs together give what each gives on the output
// of the other, in order.
TEST(MultiPackFixtureSetTest, DisjointKindsEqualsAThenB) {
  for (const FixtureCase& c : Cases()) {
    if (c.name != "disjoint-kinds") continue;
    auto run = RunCase(kFixturesDir, c);
    ASSERT_TRUE(run.ok()) << run.status();
    auto a = LoadPackFile(std::string(kFixturesDir) + "/pack-a.json", "");
    auto b = LoadPackFile(std::string(kFixturesDir) + "/pack-b.json", "");
    ASSERT_TRUE(a.ok() && b.ok());
    RunOptions opts;
    auto first = RunPackOnHtml(std::make_shared<const Pack>(*std::move(a)),
                               run->url, run->input_html, opts);
    ASSERT_TRUE(first.ok()) << first.status();
    auto second = RunPackOnHtml(std::make_shared<const Pack>(*std::move(b)),
                                run->url, first->html, opts);
    ASSERT_TRUE(second.ok()) << second.status();
    EXPECT_EQ(run->enforce.html, second->html);
    EXPECT_NE(run->enforce.html, run->roundtrip.html);
  }
}

}  // namespace
}  // namespace pagespeed::packs
