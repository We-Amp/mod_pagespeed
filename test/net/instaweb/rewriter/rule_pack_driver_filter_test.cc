// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

// The rule_packs filter on a RewriteDriver: the option gate, the place in
// the chain, the pack fetched per document, statistics and the decision
// sink.

#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "absl/strings/str_cat.h"
#include "net/instaweb/rewriter/public/rewrite_driver.h"
#include "net/instaweb/rewriter/public/rewrite_options.h"
#include "net/instaweb/rewriter/public/rule_pack_host.h"
#include "net/instaweb/rewriter/public/rule_pack_provider.h"
#include "pagespeed/kernel/base/statistics.h"
#include "pagespeed/kernel/base/string_util.h"
#include "pagespeed/kernel/base/string_writer.h"
#include "pagespeed/kernel/packs/decision.h"
#include "pagespeed/kernel/packs/pack.h"
#include "pagespeed/kernel/packs/pack_host.h"
#include "pagespeed/kernel/packs/pack_loader.h"
#include "pagespeed/kernel/packs/rule_pack_filter.h"
#include "pagespeed/kernel/packs/template.h"
#include "test/net/instaweb/rewriter/rewrite_test_base.h"
#include "test/net/instaweb/rewriter/test_rewrite_driver_factory.h"
#include "test/pagespeed/kernel/base/gtest.h"

namespace net_instaweb {
namespace {

using pagespeed::packs::Counter;
using pagespeed::packs::CounterName;

// A pack with `rules` for `host`. The pack is parsed with the id
// `edge-seo` and then given `id`, so the tests do not depend on which ids the
// loader accepts.
std::shared_ptr<const pagespeed::packs::Pack> MakePack(
    const std::string& rules, const std::string& host = "test.com",
    const std::string& id = "edge-seo",
    const std::string& site_mode = "enforce") {
  const std::string json = absl::StrCat(
      R"({"pack":{"id":"edge-seo","version":"0.1.0"},"sites":[{"host":")", host,
      R"(","mode":")", site_mode, R"("}],"rules":[)", rules, "]}");
  auto pack = pagespeed::packs::LoadPack(json, "");
  EXPECT_TRUE(pack.ok()) << pack.status();
  if (!pack.ok()) return nullptr;
  pack->info.id = id;
  return std::make_shared<const pagespeed::packs::Pack>(*std::move(pack));
}

constexpr char kCanonicalKeep[] =
    R"({"id":"c","kind":"canonical","enforce":true,)"
    R"("value":{"template":"https://test.com{path}"}})";

constexpr char kCanonicalReplace[] =
    R"({"id":"c","kind":"canonical","enforce":true,"on_present":"replace",)"
    R"("value":{"template":"https://test.com{path}"}})";

// A canonical rule that replaces whatever the page has with `/other`.
constexpr char kCanonicalReplaceOther[] =
    R"({"id":"c2","kind":"canonical","enforce":true,"on_present":"replace",)"
    R"("value":{"template":"https://test.com/other"}})";

constexpr char kPathA[] = "/etc/pagespeed/packs/test.json";
constexpr char kPathB[] = "/etc/pagespeed/packs/second.json";

std::string TitleRule(const std::string& title) {
  return absl::StrCat(R"({"id":"t","kind":"title","enforce":true,)",
                      R"("value":{"template":")", title, R"("}})");
}

// A provider that hands out whatever pack the test set, and records the
// decisions it is given.
class TestRulePackProvider : public RulePackProvider {
 public:
  struct Logged {
    std::string pack_id;
    std::string sha256;
    std::string host;
    std::string path;
    pagespeed::packs::PackDecision decision;
  };

  // The pack of the first file (kPathA).
  void Set(std::shared_ptr<const pagespeed::packs::Pack> pack,
           std::string sha256, LoadEvent event = LoadEvent::kLoaded) {
    current_.pack = std::move(pack);
    current_.sha256 = std::move(sha256);
    next_event_ = event;
  }
  // The pack of any other file.
  void SetFor(const std::string& path,
              std::shared_ptr<const pagespeed::packs::Pack> pack,
              std::string sha256) {
    LoadedRulePack& loaded = others_[path];
    loaded.pack = std::move(pack);
    loaded.sha256 = std::move(sha256);
  }

  LoadedRulePack Get(const std::string& path,
                     const pagespeed::packs::PackTrust& trust,
                     MessageHandler* handler, LoadEvent* event,
                     SignatureEvent* signature_event) override {
    paths_.push_back(path);
    trusts_.push_back(trust);
    *event = next_event_;
    next_event_ = LoadEvent::kNone;
    *signature_event = next_signature_event_;
    next_signature_event_ = SignatureEvent::kNone;
    auto other = others_.find(path);
    return other == others_.end() ? current_ : other->second;
  }
  void SetSignatureEvent(SignatureEvent e) { next_signature_event_ = e; }
  const std::vector<pagespeed::packs::PackTrust>& trusts() const {
    return trusts_;
  }

  LogResult LogDecision(const RewriteOptions& options,
                        const LoadedRulePack& pack,
                        const pagespeed::packs::PageUrl& url,
                        const pagespeed::packs::PackDecision& decision,
                        int64_t now_ms, MessageHandler* handler) override {
    logged_.push_back(
        {pack.pack->info.id, pack.sha256, url.host, url.path, decision});
    return log_result_;
  }

  const std::vector<std::string>& paths() const { return paths_; }
  const std::vector<Logged>& logged() const { return logged_; }
  void set_log_result(LogResult r) { log_result_ = r; }

 private:
  LoadedRulePack current_;
  std::map<std::string, LoadedRulePack> others_;
  LoadEvent next_event_ = LoadEvent::kNone;
  SignatureEvent next_signature_event_ = SignatureEvent::kNone;
  std::vector<pagespeed::packs::PackTrust> trusts_;
  LogResult log_result_ = LogResult::kWritten;
  std::vector<std::string> paths_;
  std::vector<Logged> logged_;
};

class RulePackDriverFilterTest : public RewriteTestBase {
 protected:
  bool AddHtmlTags() const override { return false; }

  void SetUp() override {
    factory()->set_rule_pack_provider(&provider_);
    RewriteTestBase::SetUp();
  }

  void TearDown() override {
    RewriteTestBase::TearDown();
    factory()->set_rule_pack_provider(nullptr);
  }

  // Turns the filter on with a pack file and `mode`, and builds the chain.
  void Enable(RewriteOptions::RulePackMode mode =
                  RewriteOptions::kRulePackModeEnforce) {
    options()->EnableFilter(RewriteOptions::kRulePacks);
    options()->set_rule_pack_file(kPathA);
    options()->set_rule_pack_mode(mode);
    rewrite_driver()->AddFilters();
  }

  // The filter as AddPreRenderFilters() would add it, kept at hand so a
  // test can look at the RulePackFilter it runs.
  RulePackDriverFilter* AddDriverFilter() {
    options()->set_rule_pack_file(kPathA);
    options()->set_rule_pack_mode(RewriteOptions::kRulePackModeEnforce);
    auto* filter = new RulePackDriverFilter(rewrite_driver());
    rewrite_driver()->AddOwnedEarlyPreRenderFilter(filter);
    rewrite_driver()->AddFilters();
    return filter;
  }

  // Turns the filter on with the two pack files kPathA and kPathB, in that
  // order, and builds the chain.
  void EnableTwo(RewriteOptions::RulePackMode mode =
                     RewriteOptions::kRulePackModeEnforce) {
    options()->EnableFilter(RewriteOptions::kRulePacks);
    GoogleString msg;
    ASSERT_TRUE(options()->AddRulePackFile(kPathA, &msg)) << msg;
    ASSERT_TRUE(options()->AddRulePackFile(kPathB, &msg)) << msg;
    options()->set_rule_pack_mode(mode);
    rewrite_driver()->AddFilters();
  }

  // The logged decisions of the pack `id`.
  std::vector<pagespeed::packs::PackDecision> LoggedOf(const std::string& id) {
    std::vector<pagespeed::packs::PackDecision> out;
    for (const auto& l : provider_.logged()) {
      if (l.pack_id == id) out.push_back(l.decision);
    }
    return out;
  }

  // How often the server logged a message containing `text`.
  int CountMessages(const std::string& text) {
    GoogleString dump;
    StringWriter writer(&dump);
    message_handler()->Dump(&writer);
    int n = 0;
    for (size_t pos = dump.find(text); pos != GoogleString::npos;
         pos = dump.find(text, pos + 1)) {
      ++n;
    }
    return n;
  }

  int64 Stat(Counter counter) {
    Variable* v = statistics()->GetVariable(CounterName(counter));
    return v == nullptr ? -1 : v->Get();
  }

  TestRulePackProvider provider_;
};

TEST_F(RulePackDriverFilterTest, EveryCounterIsAStatistic) {
  for (int i = 0; i < pagespeed::packs::kCounterCount; ++i) {
    EXPECT_EQ(0, Stat(static_cast<Counter>(i)))
        << CounterName(static_cast<Counter>(i));
  }
}

// Each signature event moves exactly its own statistic, once, and the trust
// configuration of the options reaches the provider.
TEST_F(RulePackDriverFilterTest, SignatureEventsMoveTheirCounters) {
  using Event = RulePackProvider::SignatureEvent;
  const struct {
    Event event;
    Counter counter;
  } kCases[] = {
      {Event::kVerified, Counter::kLoadsSigned},
      {Event::kRefusedUnsigned, Counter::kRefusedUnsigned},
      {Event::kRefusedSignatureUnreadable, Counter::kRefusedSignatureUnreadable},
      {Event::kRefusedPublisherMismatch, Counter::kRefusedPublisherMismatch},
      {Event::kRefusedUnknownKey, Counter::kRefusedUnknownKey},
      {Event::kRefusedBadSignature, Counter::kRefusedBadSignature},
  };
  GoogleString msg;
  ASSERT_TRUE(options()->AddRulePackTrustKey(
      "example-publisher", "AAECAwQFBgcICQoLDA0ODxAREhMUFRYXGBkaGxwdHh8=",
      &msg))
      << msg;
  options()->set_rule_pack_require_signed(true);
  Enable();
  size_t done = 0;
  for (const auto& c : kCases) {
    provider_.Set(MakePack(kCanonicalKeep), "aaaa",
                  RulePackProvider::LoadEvent::kNone);
    provider_.SetSignatureEvent(c.event);
    ParseUrl("http://test.com/sig.html",
             "<html><head></head><body></body></html>");
    ++done;
    // Cumulative: the cases so far moved their counters once each, the
    // others not at all.
    for (size_t j = 0; j < sizeof(kCases) / sizeof(kCases[0]); ++j) {
      EXPECT_EQ(j < done ? 1 : 0, Stat(kCases[j].counter))
          << CounterName(kCases[j].counter);
    }
  }
  ASSERT_FALSE(provider_.trusts().empty());
  EXPECT_TRUE(provider_.trusts().back().require_signed);
  // The configured key plus the built-in one, sorted by key id.
  ASSERT_EQ(2u, provider_.trusts().back().keys.size());
  EXPECT_EQ("example-publisher", provider_.trusts().back().keys[0].key_id);
  EXPECT_EQ("we-amp", provider_.trusts().back().keys[1].key_id);
}

// A pack file alone does nothing: the filter is only ever on by name (no
// rewrite level includes it; see RewriteOptionsTest).
TEST_F(RulePackDriverFilterTest, OffUnlessEnabledByName) {
  provider_.Set(MakePack(kCanonicalKeep), "aaaa");
  options()->set_rule_pack_file(kPathA);
  options()->set_rule_pack_mode(RewriteOptions::kRulePackModeEnforce);
  options()->SetRewriteLevel(RewriteOptions::kCoreFilters);
  rewrite_driver()->AddFilters();
  Parse("core", "<html><head></head><body></body></html>");
  EXPECT_EQ(GoogleString::npos, output_buffer_.find("canonical"));
  EXPECT_TRUE(provider_.paths().empty());
}

TEST_F(RulePackDriverFilterTest, OffWithoutAPackFile) {
  provider_.Set(MakePack(kCanonicalKeep), "aaaa");
  options()->EnableFilter(RewriteOptions::kRulePacks);
  rewrite_driver()->AddFilters();
  ValidateNoChanges("nofile", "<html><head></head><body></body></html>");
  EXPECT_TRUE(provider_.paths().empty());
}

TEST_F(RulePackDriverFilterTest, EnforceAppliesTheRuleAndCountsIt) {
  provider_.Set(MakePack(kCanonicalKeep), "aaaa");
  Enable();
  ValidateExpected("enforce", "<html><head></head><body></body></html>",
                   "<html><head><link rel=\"canonical\" "
                   "href=\"https://test.com/enforce.html\"></head><body>"
                   "</body></html>");
  ASSERT_EQ(1u, provider_.paths().size());
  EXPECT_EQ("/etc/pagespeed/packs/test.json", provider_.paths()[0]);
  EXPECT_EQ(1, Stat(Counter::kLoads));
  EXPECT_EQ(1, Stat(Counter::kPagesEvaluated));
  EXPECT_EQ(1, Stat(Counter::kPagesModified));
  EXPECT_EQ(1, Stat(Counter::kDecisionsApplied));
  EXPECT_EQ(0, Stat(Counter::kDecisionsWouldApply));
  ASSERT_EQ(1u, provider_.logged().size());
  EXPECT_EQ("aaaa", provider_.logged()[0].sha256);
  EXPECT_EQ("test.com", provider_.logged()[0].host);
  EXPECT_EQ("/enforce.html", provider_.logged()[0].path);
  EXPECT_EQ(pagespeed::packs::Action::kInsert,
            provider_.logged()[0].decision.action);
  EXPECT_EQ(1, Stat(Counter::kDecisionLogLines));
}

TEST_F(RulePackDriverFilterTest, ReportModeLeavesThePageAlone) {
  provider_.Set(MakePack(kCanonicalKeep), "aaaa");
  provider_.set_log_result(RulePackProvider::LogResult::kDropped);
  Enable(RewriteOptions::kRulePackModeReport);
  ValidateNoChanges("report", "<html><head></head><body></body></html>");
  EXPECT_EQ(1, Stat(Counter::kPagesEvaluated));
  EXPECT_EQ(0, Stat(Counter::kPagesModified));
  EXPECT_EQ(1, Stat(Counter::kPagesWouldModify));
  EXPECT_EQ(1, Stat(Counter::kDecisionsWouldApply));
  ASSERT_EQ(1u, provider_.logged().size());
  EXPECT_EQ(pagespeed::packs::Mode::kReport,
            provider_.logged()[0].decision.mode);
  EXPECT_EQ(0, Stat(Counter::kDecisionLogLines));
  EXPECT_EQ(1, Stat(Counter::kDecisionLogDropped));
}

TEST_F(RulePackDriverFilterTest, ReportModeWithDebugSaysWhatItWouldDo) {
  provider_.Set(MakePack(kCanonicalKeep), "aaaa");
  options()->EnableFilter(RewriteOptions::kDebug);
  Enable(RewriteOptions::kRulePackModeReport);
  Parse("debug", "<html><head></head><body></body></html>");
  EXPECT_NE(GoogleString::npos,
            output_buffer_.find("<!-- pagespeed-pack edge-seo@0.1.0 rule=c "
                                "would=insert mode=report -->"))
      << output_buffer_;
  EXPECT_EQ(GoogleString::npos, output_buffer_.find("<link"));
}

TEST_F(RulePackDriverFilterTest, EnforceWithDebugNamesTheMode) {
  provider_.Set(MakePack(kCanonicalKeep), "aaaa");
  options()->EnableFilter(RewriteOptions::kDebug);
  Enable();
  Parse("debug", "<html><head></head><body></body></html>");
  EXPECT_NE(GoogleString::npos,
            output_buffer_.find("<!-- pagespeed-pack edge-seo@0.1.0 rule=c "
                                "action=insert mode=enforce --><link "
                                "rel=\"canonical\""))
      << output_buffer_;
}

// The driver, and its filter chain, serve one document after another; a
// pack that changed in between reaches the next document, and the decisions
// of each document name the pack that made them.
TEST_F(RulePackDriverFilterTest, ThePackIsFetchedForEveryDocument) {
  provider_.Set(MakePack(TitleRule("A")), "aaaa");
  RulePackDriverFilter* driver_filter = AddDriverFilter();
  ValidateExpected("first", "<html><head></head></html>",
                   "<html><head><title>A</title></head></html>");
  const pagespeed::packs::RulePackFilter* first = driver_filter->filter();
  ASSERT_NE(nullptr, first);
  ValidateExpected("again", "<html><head></head></html>",
                   "<html><head><title>A</title></head></html>");
  // The same pack: the same filter.
  EXPECT_EQ(first, driver_filter->filter());
  provider_.Set(MakePack(TitleRule("B")), "bbbb");
  ValidateExpected("second", "<html><head></head></html>",
                   "<html><head><title>B</title></head></html>");
  // A new pack: a new filter.
  EXPECT_NE(first, driver_filter->filter());
  EXPECT_EQ("bbbb", driver_filter->host().pack().sha256);
  EXPECT_EQ(3u, provider_.paths().size());
  ASSERT_EQ(3u, provider_.logged().size());
  EXPECT_EQ("aaaa", provider_.logged()[0].sha256);
  EXPECT_EQ("aaaa", provider_.logged()[1].sha256);
  EXPECT_EQ("bbbb", provider_.logged()[2].sha256);
  EXPECT_EQ(2, Stat(Counter::kLoads));
}

// A document parsed in two flush windows, split inside the head: the flush
// reaches the filter (the response headers count as sent), and the missing
// canonical is still inserted at the end of the open head.
TEST_F(RulePackDriverFilterTest, AFlushInsideTheHeadReachesTheFilter) {
  provider_.Set(MakePack(kCanonicalKeep), "aaaa");
  RulePackDriverFilter* driver_filter = AddDriverFilter();
  SetupWriter();
  ASSERT_TRUE(
      rewrite_driver()->StartParse(StrCat(kTestDomain, "chunked.html")));
  rewrite_driver()->ParseText("<html><head><meta charset=utf-8>");
  rewrite_driver()->Flush();
  ASSERT_NE(nullptr, driver_filter->filter());
  EXPECT_TRUE(driver_filter->filter()->headers_flushed());
  rewrite_driver()->ParseText("</head><body></body></html>");
  rewrite_driver()->FinishParse();
  EXPECT_EQ(
      "<html><head><meta charset=utf-8><link rel=\"canonical\" "
      "href=\"https://test.com/chunked.html\"></head><body></body></html>",
      output_buffer_);
  EXPECT_TRUE(driver_filter->filter()->modified());
  EXPECT_EQ(1, Stat(Counter::kPagesModified));
}

TEST_F(RulePackDriverFilterTest, AFailedLoadLeavesThePageAlone) {
  provider_.Set(nullptr, "", RulePackProvider::LoadEvent::kFailed);
  Enable();
  ValidateNoChanges("failed", "<html><head></head><body></body></html>");
  EXPECT_EQ(1, Stat(Counter::kLoadErrors));
  EXPECT_EQ(0, Stat(Counter::kPagesEvaluated));
  EXPECT_TRUE(provider_.logged().empty());
}

TEST_F(RulePackDriverFilterTest, AHostThePackDoesNotListIsSkipped) {
  provider_.Set(MakePack(kCanonicalKeep, "other.test"), "aaaa");
  Enable();
  ValidateNoChanges("unlisted", "<html><head></head><body></body></html>");
  EXPECT_EQ(1, Stat(Counter::kPagesSkippedHostNotListed));
  EXPECT_EQ(0, Stat(Counter::kPagesEvaluated));
}

// The pack sees the document as the origin sent it: before AddHeadFilter,
// so a document without a head is skipped, not given a synthesized head.
TEST_F(RulePackDriverFilterTest, RunsBeforeTheHeadIsSynthesized) {
  provider_.Set(MakePack(kCanonicalKeep), "aaaa");
  options()->EnableFilter(RewriteOptions::kAddHead);
  Enable();
  Parse("nohead", "<html><body><p>x</p></body></html>");
  EXPECT_NE(GoogleString::npos, output_buffer_.find("<head/>"))
      << output_buffer_;
  EXPECT_EQ(GoogleString::npos, output_buffer_.find("canonical"));
  EXPECT_EQ(1, Stat(Counter::kPagesSkippedNoHead));
}

// Later filters see what the pack inserted like origin markup.
TEST_F(RulePackDriverFilterTest, LaterFiltersSeeTheInsertedElements) {
  provider_.Set(MakePack(kCanonicalKeep), "aaaa");
  options()->EnableFilter(RewriteOptions::kRemoveQuotes);
  Enable();
  ValidateExpected("quotes", "<html><head></head><body></body></html>",
                   "<html><head><link rel=canonical "
                   "href=https://test.com/quotes.html></head><body>"
                   "</body></html>");
}

// An enforcing canonical replace rule edits a head link, not a resource URL,
// so the origin's preload hints survive the chain's hint stripper.
TEST_F(RulePackDriverFilterTest, CanonicalReplaceKeepsPreloadHints) {
  provider_.Set(MakePack(kCanonicalReplace), "aaaa");
  options()->set_preserve_subresource_hints(false);
  Enable();
  ValidateExpected(
      "preload",
      "<html><head><link rel=\"canonical\" href=\"https://other.test/\">"
      "<link rel=\"preload\" as=\"style\" href=\"a.css\"></head>"
      "<body></body></html>",
      "<html><head><link rel=\"canonical\" "
      "href=\"https://test.com/preload.html\">"
      "<link rel=\"preload\" as=\"style\" href=\"a.css\"></head>"
      "<body></body></html>");
}

// ---------------------------------------------------------------------------
// Several pack files on one host.
// ---------------------------------------------------------------------------

// Two packs with different kinds: both act, in load order, and nothing is
// claimed.
TEST_F(RulePackDriverFilterTest, TwoPacksWithDisjointKindsBothAct) {
  provider_.Set(MakePack(kCanonicalKeep), "aaaa");
  provider_.SetFor(
      kPathB, MakePack(TitleRule("B"), "test.com", "fixture-second"), "bbbb");
  EnableTwo();
  ValidateExpected("disjoint", "<html><head></head><body></body></html>",
                   "<html><head><link rel=\"canonical\" "
                   "href=\"https://test.com/disjoint.html\"><title>B</title>"
                   "</head><body></body></html>");
  ASSERT_EQ(2u, provider_.paths().size());
  EXPECT_EQ(kPathA, provider_.paths()[0]);
  EXPECT_EQ(kPathB, provider_.paths()[1]);
  // Each file is checked against the built-in key exactly once.
  ASSERT_EQ(2u, provider_.trusts().size());
  for (const auto& trust : provider_.trusts()) {
    int built_in = 0;
    for (const auto& key : trust.keys) built_in += key.key_id == "we-amp";
    EXPECT_EQ(1, built_in);
  }
  const auto a = LoggedOf("edge-seo");
  const auto b = LoggedOf("fixture-second");
  ASSERT_EQ(1u, a.size());
  ASSERT_EQ(1u, b.size());
  EXPECT_EQ(pagespeed::packs::Action::kInsert, a[0].action);
  EXPECT_EQ(pagespeed::packs::Action::kInsert, b[0].action);
  EXPECT_EQ("", b[0].claimed_by);
  EXPECT_EQ(2, Stat(Counter::kDecisionsApplied));
  EXPECT_EQ(0, Stat(Counter::kDecisionsClaimed));
}

// Two packs with the same kind: the first pack owns it, the second records
// none/claimed with the owner's id, in enforce and in report mode.
class RulePackClaimModeTest
    : public RulePackDriverFilterTest,
      public ::testing::WithParamInterface<RewriteOptions::RulePackMode> {};

TEST_P(RulePackClaimModeTest, TheSecondPackOnTheSameKindIsClaimed) {
  const bool enforce = GetParam() == RewriteOptions::kRulePackModeEnforce;
  provider_.Set(MakePack(kCanonicalKeep), "aaaa");
  provider_.SetFor(
      kPathB, MakePack(kCanonicalReplaceOther, "test.com", "fixture-second"),
      "bbbb");
  EnableTwo(GetParam());
  const char kInput[] = "<html><head></head><body></body></html>";
  if (enforce) {
    ValidateExpected("same", kInput,
                     "<html><head><link rel=\"canonical\" "
                     "href=\"https://test.com/same.html\"></head><body>"
                     "</body></html>");
  } else {
    ValidateNoChanges("same", kInput);
  }
  const auto a = LoggedOf("edge-seo");
  const auto b = LoggedOf("fixture-second");
  ASSERT_EQ(1u, a.size());
  EXPECT_EQ(pagespeed::packs::Action::kInsert, a[0].action);
  EXPECT_EQ("", a[0].claimed_by);
  ASSERT_EQ(1u, b.size());
  EXPECT_EQ("c2", b[0].rule_id);
  EXPECT_EQ(pagespeed::packs::Action::kNone, b[0].action);
  EXPECT_EQ(pagespeed::packs::Reason::kClaimed, b[0].reason);
  EXPECT_EQ("edge-seo", b[0].claimed_by);
  EXPECT_EQ(enforce ? pagespeed::packs::Mode::kEnforce
                    : pagespeed::packs::Mode::kReport,
            b[0].mode);
  EXPECT_EQ(1, Stat(Counter::kDecisionsClaimed));
  // The claimed pack evaluated nothing on the page.
  EXPECT_EQ(1, Stat(Counter::kPagesEvaluated));
}

INSTANTIATE_TEST_SUITE_P(
    Modes, RulePackClaimModeTest,
    ::testing::Values(RewriteOptions::kRulePackModeEnforce,
                      RewriteOptions::kRulePackModeReport));

// A first pack that only reports still owns the kind, so an enforcing later
// pack does nothing on it; the overlap is logged once per process.
TEST_F(RulePackDriverFilterTest, AReportingFirstPackOwnsTheKindAndWarnsOnce) {
  provider_.Set(MakePack(kCanonicalKeep, "test.com", "edge-seo", "report"),
                "aaaa");
  provider_.SetFor(
      kPathB, MakePack(kCanonicalReplaceOther, "test.com", "fixture-second"),
      "bbbb");
  EnableTwo();
  const char kInput[] =
      "<html><head><link rel=\"canonical\" href=\"https://test.com/x\">"
      "</head><body></body></html>";
  ValidateNoChanges("report-first", kInput);
  ValidateNoChanges("report-first-again", kInput);
  const auto b = LoggedOf("fixture-second");
  ASSERT_EQ(2u, b.size());
  for (const auto& d : b) {
    EXPECT_EQ(pagespeed::packs::Reason::kClaimed, d.reason);
    EXPECT_EQ("edge-seo", d.claimed_by);
  }
  EXPECT_EQ(2, Stat(Counter::kDecisionsClaimed));
  EXPECT_EQ(1, CountMessages("rule pack overlap: canonical owned by edge-seo "
                             "(report) before fixture-second (enforce); the "
                             "later pack will not act on it"));
}

// Two rules of one kind in one pack: the first matching rule wins as it
// always has, and nothing is claimed.
TEST_F(RulePackDriverFilterTest, OnePackNeverClaimsAgainstItself) {
  provider_.Set(
      MakePack(absl::StrCat(kCanonicalKeep, ",", kCanonicalReplaceOther)),
      "aaaa");
  Enable();
  ValidateExpected("self", "<html><head></head><body></body></html>",
                   "<html><head><link rel=\"canonical\" "
                   "href=\"https://test.com/self.html\"></head><body>"
                   "</body></html>");
  ASSERT_EQ(1u, provider_.logged().size());
  EXPECT_EQ("c", provider_.logged()[0].decision.rule_id);
  EXPECT_EQ("", provider_.logged()[0].decision.claimed_by);
  for (const auto& l : provider_.logged()) {
    EXPECT_NE(pagespeed::packs::Reason::kClaimed, l.decision.reason);
  }
  EXPECT_EQ(0, Stat(Counter::kDecisionsClaimed));
}

// The first pack's head plan drops (it would replace a canonical a flush
// already sent): it still holds the kind, so the later pack stays off it
// and the page is left as the origin sent it.
TEST_F(RulePackDriverFilterTest, AClaimIsHeldWhenTheOwnersPlanDrops) {
  provider_.Set(MakePack(kCanonicalReplace), "aaaa");
  provider_.SetFor(
      kPathB, MakePack(kCanonicalReplaceOther, "test.com", "fixture-second"),
      "bbbb");
  EnableTwo();
  SetupWriter();
  ASSERT_TRUE(rewrite_driver()->StartParse(StrCat(kTestDomain, "split.html")));
  rewrite_driver()->ParseText(
      "<html><head><link rel=\"canonical\" href=\"https://other.test/\">");
  rewrite_driver()->Flush();
  rewrite_driver()->ParseText("</head><body></body></html>");
  rewrite_driver()->FinishParse();
  EXPECT_EQ(
      "<html><head><link rel=\"canonical\" href=\"https://other.test/\">"
      "</head><body></body></html>",
      output_buffer_);
  const auto a = LoggedOf("edge-seo");
  const auto b = LoggedOf("fixture-second");
  ASSERT_EQ(1u, a.size());
  EXPECT_EQ(pagespeed::packs::Reason::kNotRewritable, a[0].reason);
  ASSERT_EQ(1u, b.size());
  EXPECT_EQ(pagespeed::packs::Reason::kClaimed, b[0].reason);
  EXPECT_EQ("edge-seo", b[0].claimed_by);
}

// A second file whose pack has an id already loaded on the host is refused;
// the first pack is unaffected. The refusal is logged and counted once per
// process.
TEST_F(RulePackDriverFilterTest, ADuplicatePackIdIsRefusedOnce) {
  provider_.Set(MakePack(kCanonicalKeep), "aaaa");
  provider_.SetFor(kPathB, MakePack(TitleRule("B")), "bbbb");
  EnableTwo();
  const char kOutput[] =
      "<html><head><link rel=\"canonical\" href=\"https://test.com/dup.html\">"
      "</head><body></body></html>";
  ValidateExpected("dup", "<html><head></head><body></body></html>", kOutput);
  ValidateExpected("dup", "<html><head></head><body></body></html>", kOutput);
  EXPECT_EQ(1, Stat(Counter::kRefusedDuplicateId));
  EXPECT_EQ(1, CountMessages("rule pack refused (duplicate id): "
                             "edge-seo@0.1.0 sha256=bbbb path=" +
                             std::string(kPathB)));
  ASSERT_EQ(2u, provider_.logged().size());
  for (const auto& l : provider_.logged()) EXPECT_EQ("aaaa", l.sha256);
}

// A pack that lists other hosts claims nothing on this one: the later pack
// acts.
TEST_F(RulePackDriverFilterTest, NoClaimWithoutSelection) {
  provider_.Set(MakePack(kCanonicalKeep, "other.test"), "aaaa");
  provider_.SetFor(
      kPathB, MakePack(kCanonicalReplaceOther, "test.com", "fixture-second"),
      "bbbb");
  EnableTwo();
  ValidateExpected("unlisted", "<html><head></head><body></body></html>",
                   "<html><head><link rel=\"canonical\" "
                   "href=\"https://test.com/other\"></head><body>"
                   "</body></html>");
  EXPECT_EQ(0, Stat(Counter::kDecisionsClaimed));
}

}  // namespace
}  // namespace net_instaweb
