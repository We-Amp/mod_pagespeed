// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

// The rule-pack fixtures through a real RewriteDriver: the rule_packs filter
// as the server runs it (RulePackDriverFilter, RewriteDriverPackHost, the
// provider seam), in three chain variants, whole-document and in flush
// windows.
//
//   (a) rule_packs alone.
//   (b) rule_packs with the core filters.
//   (c) rule_packs with the core filters plus the filters that inline,
//       defer, minify and collapse what the pack's page contains.
//
// For every case (the kernel runner's cases, expected files and chunkings):
//   1. (a) enforce: output and decisions equal the expected files, the
//      same as the kernel-level run;
//   2. (a) idempotence: the expected output run again is unchanged;
//   3. (a) determinism, and device independence: a second run, and runs for
//      other user agents, give the same output and decisions;
//   4. every variant, report-only: byte-identical to the same chain without
//      the filter (with debug comments on, the filter's own comments are the
//      only difference), and the decisions are the expected ones in report
//      mode;
//   5. (b) and (c): the elements the pack put in are present with the same
//      content (canonical URL equal after normalization, title, description
//      and JSON-LD text) as in the expected output, and the decisions equal
//      (a)'s;
//   6. every variant, each chunking: the chunked property of the kernel
//      runner (ChunkedOutcomeAllowed): the whole-document result, or the
//      output without the filter with the same rules decided
//      `not_rewritable`; never anything else. For (a) that is compared byte
//      for byte; with other filters in the chain the bytes also depend on
//      where the windows fall, so there "the same" means the same pack
//      elements.
// The expected files and the list of module-specific overrides are the
// kernel runner's.

#include <algorithm>
#include <cstddef>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "absl/strings/ascii.h"
#include "absl/strings/match.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/str_join.h"
#include "absl/strings/str_split.h"
#include "net/instaweb/rewriter/public/rewrite_driver.h"
#include "net/instaweb/rewriter/public/rewrite_options.h"
#include "net/instaweb/rewriter/public/rule_pack_provider.h"
#include "pagespeed/kernel/base/null_message_handler.h"
#include "pagespeed/kernel/base/string_writer.h"
#include "pagespeed/kernel/html/empty_html_filter.h"
#include "pagespeed/kernel/html/html_element.h"
#include "pagespeed/kernel/html/html_name.h"
#include "pagespeed/kernel/html/html_node.h"
#include "pagespeed/kernel/html/html_parse.h"
#include "pagespeed/kernel/http/content_type.h"
#include "pagespeed/kernel/http/http_names.h"
#include "pagespeed/kernel/http/request_headers.h"
#include "pagespeed/kernel/http/response_headers.h"
#include "pagespeed/kernel/packs/decision.h"
#include "pagespeed/kernel/packs/pack.h"
#include "pagespeed/kernel/packs/pack_host.h"
#include "pagespeed/kernel/packs/pack_loader.h"
#include "pagespeed/kernel/packs/url_norm.h"
#include "test/net/instaweb/rewriter/rewrite_test_base.h"
#include "test/net/instaweb/rewriter/test_rewrite_driver_factory.h"
#include "test/pagespeed/kernel/base/gtest.h"
#include "test/pagespeed/kernel/packs/fixture_runner.h"

namespace net_instaweb {
namespace {

using pagespeed::packs::CaseRun;
using pagespeed::packs::DecisionsToJsonl;
using pagespeed::packs::FixtureCase;
using pagespeed::packs::HeaderLine;
using pagespeed::packs::PackDecision;
using pagespeed::packs::RunResult;

constexpr char kFixturesDir[] =
    "test/pagespeed/kernel/packs/testdata/edge-seo/fixtures";
constexpr char kPackPath[] = "/etc/pagespeed/packs/fixture.json";

// Hands out the fixture pack and records the decisions it is given.
class FixtureProvider : public RulePackProvider {
 public:
  void set_pack(std::shared_ptr<const pagespeed::packs::Pack> pack) {
    pack_.pack = std::move(pack);
    pack_.sha256 = "fixture";
  }
  // The pack of the file at `path`; any other path gets the pack of
  // set_pack().
  void SetFor(const std::string& path,
              std::shared_ptr<const pagespeed::packs::Pack> pack) {
    LoadedRulePack& loaded = others_[path];
    loaded.pack = std::move(pack);
    loaded.sha256 = "fixture";
  }
  void ClearFiles() { others_.clear(); }
  LoadedRulePack Get(const std::string& path,
                     const pagespeed::packs::PackTrust& trust,
                     MessageHandler* handler, LoadEvent* event,
                     SignatureEvent* signature_event) override {
    *event = LoadEvent::kNone;
    *signature_event = SignatureEvent::kNone;
    auto other = others_.find(path);
    return other == others_.end() ? pack_ : other->second;
  }
  LogResult LogDecision(const RewriteOptions& options,
                        const LoadedRulePack& pack,
                        const pagespeed::packs::PageUrl& url,
                        const PackDecision& decision, int64_t now_ms,
                        MessageHandler* handler) override {
    decisions_.push_back(decision);
    ids_.push_back(pack.pack->info.id);
    return LogResult::kWritten;
  }
  std::vector<PackDecision> TakeDecisions() {
    ids_.clear();
    return std::exchange(decisions_, {});
  }
  // The decisions grouped by pack, in the order of `ids`, and for each the
  // index of its pack in `ids`.
  std::vector<PackDecision> TakeGrouped(const std::vector<std::string>& ids,
                                        std::vector<size_t>* pack_of) {
    std::vector<PackDecision> out;
    pack_of->clear();
    for (size_t i = 0; i < ids.size(); ++i) {
      for (size_t j = 0; j < decisions_.size(); ++j) {
        if (ids_[j] != ids[i]) continue;
        out.push_back(decisions_[j]);
        pack_of->push_back(i);
      }
    }
    decisions_.clear();
    ids_.clear();
    return out;
  }

 private:
  LoadedRulePack pack_;
  std::map<std::string, LoadedRulePack> others_;
  std::vector<PackDecision> decisions_;
  std::vector<std::string> ids_;
};

// The chain variants.
struct Variant {
  const char* name;
  bool core_filters;
  std::vector<RewriteOptions::Filter> filters;
};

const std::vector<Variant>& Variants() {
  static const auto* variants = new std::vector<Variant>{
      {"alone", false, {}},
      {"core", true, {}},
      {"core-plus",
       true,
       {RewriteOptions::kRewriteJavascriptInline,
        RewriteOptions::kDeferJavascript, RewriteOptions::kInlineJavascript,
        RewriteOptions::kCollapseWhitespace}},
  };
  return *variants;
}

constexpr char kDesktopAgent[] =
    "Mozilla/5.0 (X11; Linux x86_64) AppleWebKit/537.36 (KHTML, like Gecko) "
    "Chrome/130.0 Safari/537.36";
constexpr char kAndroidAgent[] =
    "Mozilla/5.0 (Linux; Android 14; Pixel 8) AppleWebKit/537.36 (KHTML, "
    "like Gecko) Chrome/130.0 Mobile Safari/537.36";
constexpr char kIpadAgent[] =
    "Mozilla/5.0 (iPad; CPU OS 17_0 like Mac OS X) AppleWebKit/605.1.15 "
    "(KHTML, like Gecko) Version/17.0 Mobile/15E148 Safari/604.1";

// The head elements a pack puts in, as the document says them.
struct Facts {
  std::vector<std::string> canonicals;
  std::vector<std::string> titles;
  std::vector<std::string> descriptions;
  std::vector<std::string> json_ld;
  bool operator==(const Facts& o) const {
    return canonicals == o.canonicals && titles == o.titles &&
           descriptions == o.descriptions && json_ld == o.json_ld;
  }
};

std::string Collapse(std::string_view text) {
  return absl::StrJoin(
      absl::StrSplit(absl::StripAsciiWhitespace(text),
                     absl::ByAnyChar(" \t\r\n\f"), absl::SkipEmpty()),
      " ");
}

class FactFilter : public EmptyHtmlFilter {
 public:
  explicit FactFilter(Facts* facts) : facts_(facts) {}
  const char* Name() const override { return "FactFilter"; }

  void StartElement(HtmlElement* e) override {
    if (e->keyword() == HtmlName::kLink) {
      const char* rel = e->AttributeValue(HtmlName::kRel);
      const char* href = e->AttributeValue(HtmlName::kHref);
      if (rel != nullptr && href != nullptr &&
          absl::AsciiStrToLower(rel) == "canonical") {
        // Canonical URLs are compared the way the pack compares them.
        facts_->canonicals.push_back(
            pagespeed::packs::NormUrl(href).value_or(href));
      }
    } else if (e->keyword() == HtmlName::kMeta) {
      const char* name = e->AttributeValue(HtmlName::kName);
      const char* content = e->AttributeValue(HtmlName::kContent);
      if (name != nullptr && content != nullptr &&
          absl::AsciiStrToLower(name) == "description") {
        facts_->descriptions.push_back(Collapse(content));
      }
    } else if (e->keyword() == HtmlName::kTitle) {
      facts_->titles.emplace_back();
      in_ = &facts_->titles.back();
    } else if (e->keyword() == HtmlName::kScript) {
      const char* type = e->AttributeValue(HtmlName::kType);
      if (type != nullptr &&
          absl::AsciiStrToLower(type) == "application/ld+json") {
        facts_->json_ld.emplace_back();
        in_ = &facts_->json_ld.back();
      }
    }
  }
  void Characters(HtmlCharactersNode* c) override {
    if (in_ != nullptr) in_->append(c->contents());
  }
  void EndElement(HtmlElement* e) override {
    if (in_ == nullptr) return;
    if (e->keyword() == HtmlName::kTitle) *in_ = Collapse(*in_);
    in_ = nullptr;
  }

 private:
  Facts* facts_;
  std::string* in_ = nullptr;
};

Facts CollectFacts(const std::string& html) {
  NullMessageHandler handler;
  HtmlParse parser(&handler);
  Facts facts;
  FactFilter filter(&facts);
  parser.AddFilter(&filter);
  parser.StartParse("https://fixture.test/facts.html");
  parser.ParseText(html);
  parser.FinishParse();
  return facts;
}

// The `Link` lines of `headers`. A chain with other filters in it may touch
// other response headers (caching headers, say); the pack's promise is about
// `Link`.
std::vector<HeaderLine> LinkLines(const std::vector<HeaderLine>& headers) {
  std::vector<HeaderLine> out;
  for (const HeaderLine& line : headers) {
    if (absl::EqualsIgnoreCase(line.first, "link")) out.push_back(line);
  }
  return out;
}

RunResult LinkOnly(RunResult r) {
  r.headers = LinkLines(r.headers);
  return r;
}

// The flush offsets of a case's own flush_every plus those of a chunking.
std::vector<size_t> MergeCuts(std::vector<size_t> a,
                              const std::vector<size_t>& b) {
  a.insert(a.end(), b.begin(), b.end());
  std::sort(a.begin(), a.end());
  a.erase(std::unique(a.begin(), a.end()), a.end());
  return a;
}

class RulePackChainTest : public RewriteTestBase {
 protected:
  bool AddHtmlTags() const override { return false; }

  void SetUp() override {
    factory()->set_rule_pack_provider(&provider_);
    RewriteTestBase::SetUp();
    // Resources the fixtures point at, so the core filters have something to
    // fetch (they find nothing to optimize in them).
    SetResponseWithDefaultHeaders("https://fixture.test/app.js",
                                  kContentTypeJavascript, "var a = 1;", 100);
    SetResponseWithDefaultHeaders("https://fixture.test/a.css", kContentTypeCss,
                                  "a { color: red; }", 100);
  }

  void TearDown() override {
    RewriteTestBase::TearDown();
    factory()->set_rule_pack_provider(nullptr);
  }

  struct Chain {
    const Variant* variant = nullptr;
    bool debug = false;
    // False: the same chain without the rule_packs filter.
    bool with_pack = true;
    pagespeed::packs::Mode mode = pagespeed::packs::Mode::kEnforce;
    const char* user_agent = kDesktopAgent;
    std::vector<size_t> flush_after;
    // The response headers the origin sent; the driver has none when unset.
    std::optional<std::vector<HeaderLine>> origin_headers;
    // Several pack files on the vhost, in order (RulePackFile lines), and the
    // ids of the packs the host runs, in order. Empty: the one file.
    std::vector<std::string> pack_paths;
    std::vector<std::string> pack_ids;
  };

  // Runs `html` (served at `url`) through a fresh driver of the chain.
  RunResult Run(const Chain& chain, const std::string& url,
                const std::string& html) {
    RewriteOptions* o = options()->Clone();
    if (chain.variant->core_filters) {
      o->SetRewriteLevel(RewriteOptions::kCoreFilters);
    }
    for (RewriteOptions::Filter f : chain.variant->filters) o->EnableFilter(f);
    if (chain.debug) o->EnableFilter(RewriteOptions::kDebug);
    if (chain.with_pack) {
      o->EnableFilter(RewriteOptions::kRulePacks);
      if (chain.pack_paths.empty()) {
        o->set_rule_pack_file(kPackPath);
      } else {
        for (const std::string& path : chain.pack_paths) {
          GoogleString msg;
          EXPECT_TRUE(o->AddRulePackFile(path, &msg)) << msg;
        }
      }
      o->set_rule_pack_mode(chain.mode == pagespeed::packs::Mode::kReport
                                ? RewriteOptions::kRulePackModeReport
                                : RewriteOptions::kRulePackModeEnforce);
    }
    server_context()->ComputeSignature(o);
    RewriteDriver* d = MakeDriver(server_context(), o);
    RequestHeaders request_headers;
    request_headers.Add("User-Agent", chain.user_agent);
    d->SetRequestHeaders(request_headers);
    d->AddFilters();
    ResponseHeaders response_headers;
    if (chain.origin_headers.has_value()) {
      response_headers.set_major_version(1);
      response_headers.set_minor_version(1);
      response_headers.SetStatusAndReason(HttpStatus::kOK);
      for (const auto& [name, value] : *chain.origin_headers) {
        response_headers.Add(name, value);
      }
      d->set_response_headers_ptr(&response_headers);
    }
    RunResult result;
    StringWriter writer(&result.html);
    d->SetWriter(&writer);
    EXPECT_TRUE(d->StartParse(url));
    size_t at = 0;
    for (size_t cut : chain.flush_after) {
      if (cut <= at || cut >= html.size()) continue;
      d->ParseText(std::string_view(html).substr(at, cut - at));
      d->Flush();
      at = cut;
    }
    d->ParseText(std::string_view(html).substr(at));
    d->FinishParse();
    d->WaitForShutDown();
    if (chain.origin_headers.has_value()) {
      for (int i = 0; i < response_headers.NumAttributes(); ++i) {
        result.headers.emplace_back(response_headers.Name(i),
                                    response_headers.Value(i));
      }
    }
    d->Clear();
    delete d;
    if (chain.pack_paths.empty()) {
      result.decisions = provider_.TakeDecisions();
    } else {
      result.decisions =
          provider_.TakeGrouped(chain.pack_ids, &result.decision_pack);
    }
    if (chain.debug) result.html = WithoutDebugFilterComments(result.html);
    return result;
  }

  // With the debug filter on, the chain adds comments of its own: a timing
  // comment at every flush and a summary (filters, options, timings) at the
  // end of the document. Take them off, so what is left is the page and the
  // pack's comments.
  static std::string WithoutDebugFilterComments(const std::string& html) {
    std::string out;
    size_t at = 0;
    while (true) {
      const size_t start = html.find("<!--\n", at);
      if (start == std::string::npos) break;
      const size_t end = html.find("-->", start);
      if (end == std::string::npos) break;
      const std::string_view body =
          std::string_view(html).substr(start + 5, end - start - 5);
      out.append(html, at, start - at);
      // (The fixtures' own license header is a comment too; it stays.)
      if (body.rfind("#Flush after", 0) != 0 &&
          body.rfind("mod_pagespeed ", 0) != 0) {
        out.append(html, start, end + 3 - start);
      }
      at = end + 3;
    }
    out.append(html, at, std::string::npos);
    return out;
  }

  FixtureProvider provider_;
};

class RulePackFixtureTest : public RulePackChainTest,
                            public testing::WithParamInterface<FixtureCase> {};

TEST_P(RulePackFixtureTest, HoldsTheProperties) {
  const FixtureCase& c = GetParam();
  auto kernel = pagespeed::packs::RunCase(kFixturesDir, c);
  ASSERT_TRUE(kernel.ok()) << kernel.status();
  auto expected = pagespeed::packs::ReadExpected(c);
  ASSERT_TRUE(expected.ok()) << expected.status();
  auto loaded = pagespeed::packs::LoadPackFile(
      absl::StrCat(kFixturesDir, "/", kernel->pack_file), "");
  ASSERT_TRUE(loaded.ok()) << loaded.status();
  auto full_pack =
      std::make_shared<const pagespeed::packs::Pack>(*std::move(loaded));
  provider_.set_pack(full_pack);

  // The override list is the kernel runner's.
  EXPECT_EQ(pagespeed::packs::HasModuleOverride(c),
            pagespeed::packs::ModuleOverrides().count(c.name) == 1);

  const CaseRun& run = *kernel;
  const bool debug = run.debug;
  const std::string forced_report =
      DecisionsToJsonl(run.host, run.path, kernel->enforce.decisions, true);
  const std::string forced_report_document = DecisionsToJsonl(
      run.host, run.path,
      pagespeed::packs::DocumentDecisions(kernel->enforce.decisions), true);
  const std::vector<HeaderLine> origin_link = LinkLines(run.origin_headers);
  const Facts expected_facts = CollectFacts(expected->html);
  std::string alone_decisions;
  std::vector<HeaderLine> alone_link;

  for (const Variant& variant : Variants()) {
    SCOPED_TRACE(variant.name);
    const bool alone = !variant.core_filters;
    Chain base;
    base.variant = &variant;
    base.debug = debug;
    base.mode = run.directive_mode;
    base.flush_after = run.flush_cuts;
    if (run.has_origin_headers) base.origin_headers = run.origin_headers;
    Chain pack_free = base;
    pack_free.with_pack = false;
    Chain report = base;
    report.mode = pagespeed::packs::Mode::kReport;

    RunResult whole = Run(base, run.url, run.input_html);
    RunResult roundtrip = Run(pack_free, run.url, run.input_html);
    RunResult reported = Run(report, run.url, run.input_html);
    // What the head alone does, the reference for a chunking that drops the
    // end of the document.
    provider_.set_pack(pagespeed::packs::HeadOnlyPack(*full_pack));
    RunResult head_only = Run(base, run.url, run.input_html);
    // And what the end of the document alone does, the reference for a
    // chunking that drops the head.
    provider_.set_pack(pagespeed::packs::DocumentEndOnlyPack(*full_pack));
    RunResult document_only = Run(base, run.url, run.input_html);
    provider_.set_pack(full_pack);
    const std::string decisions =
        DecisionsToJsonl(run.host, run.path, whole.decisions, false);

    if (alone) {
      // 1. The chain with the filter alone equals the kernel run.
      EXPECT_EQ(whole.html, expected->html);
      EXPECT_EQ(decisions, expected->decisions);
      alone_decisions = decisions;
      alone_link = LinkLines(whole.headers);
      if (run.has_origin_headers) {
        EXPECT_EQ(pagespeed::packs::HeadersToText(whole.headers),
                  expected->headers);
      }

      // 2. idempotence
      // (The second pass sees the headers the first one left.)
      Chain again_chain = base;
      if (run.has_origin_headers) again_chain.origin_headers = whole.headers;
      RunResult again = Run(again_chain, run.url, whole.html);
      EXPECT_EQ(again.html, whole.html);
      EXPECT_EQ(again.headers, whole.headers);
      for (const PackDecision& d : again.decisions) {
        if (d.mode != pagespeed::packs::Mode::kEnforce) continue;
        EXPECT_EQ(d.action, pagespeed::packs::Action::kNone)
            << d.rule_id << ": " << pagespeed::packs::ReasonName(d.reason);
      }

      // 3. determinism, and device independence
      RunResult second = Run(base, run.url, run.input_html);
      EXPECT_EQ(second.html, whole.html);
      EXPECT_EQ(second.decisions, whole.decisions);
      EXPECT_EQ(second.headers, whole.headers);
      for (const char* agent : {kAndroidAgent, kIpadAgent}) {
        Chain device = base;
        device.user_agent = agent;
        RunResult other = Run(device, run.url, run.input_html);
        EXPECT_EQ(other.html, whole.html) << agent;
        EXPECT_EQ(other.decisions, whole.decisions) << agent;
        EXPECT_EQ(other.headers, whole.headers) << agent;
      }
    } else {
      // 5. The pack's elements arrive with their content, and the pack
      // decides what it decided alone.
      EXPECT_EQ(decisions, alone_decisions);
      const Facts got = CollectFacts(whole.html);
      EXPECT_EQ(got.canonicals, expected_facts.canonicals);
      EXPECT_EQ(got.titles, expected_facts.titles);
      EXPECT_EQ(got.descriptions, expected_facts.descriptions);
      EXPECT_EQ(got.json_ld, expected_facts.json_ld);
      EXPECT_EQ(LinkLines(whole.headers), alone_link);
    }

    // 4. report-only (the headers stay as the origin sent them)
    EXPECT_EQ(DecisionsToJsonl(run.host, run.path, reported.decisions, false),
              forced_report);
    EXPECT_EQ(LinkLines(reported.headers), origin_link);
    // (With debug comments on, report mode adds would= comments and nothing
    // else.)
    EXPECT_EQ(pagespeed::packs::StripPackComments(reported.html),
              roundtrip.html);

    // 6. chunked
    for (const pagespeed::packs::Chunking& chunking :
         pagespeed::packs::ChunkingsFor(run.input_html)) {
      // (The debug filter keeps time per flush and asserts when the first
      // flush comes before the first parsed event, as a window of 16 bytes
      // does when it ends inside the license header. Those windows are left
      // out for the one case that turns debug on.)
      if (debug && chunking.name.rfind("every-", 0) == 0) continue;
      SCOPED_TRACE(chunking.name);
      const std::vector<size_t> cuts =
          MergeCuts(chunking.flush_after, run.flush_cuts);
      Chain chunked = base;
      chunked.flush_after = cuts;
      Chain chunked_free = pack_free;
      chunked_free.flush_after = cuts;
      Chain chunked_report = report;
      chunked_report.flush_after = cuts;
      RunResult cr = Run(chunked, run.url, run.input_html);
      RunResult cr_free = Run(chunked_free, run.url, run.input_html);
      RunResult cr_report = Run(chunked_report, run.url, run.input_html);
      if (alone) {
        EXPECT_EQ(cr_free.html, roundtrip.html);
      }
      std::string why;
      // The chain alone keeps the kernel's promise to the byte. With other
      // filters in it the bytes also depend on where the windows fall (a
      // deferred script goes after the last window, ...), so there the
      // promise is about the pack's elements.
      std::function<bool(const std::string&, const std::string&)> same;
      if (!alone) {
        same = [](const std::string& a, const std::string& b) {
          return CollectFacts(a) == CollectFacts(b);
        };
      }
      EXPECT_TRUE(pagespeed::packs::ChunkedOutcomeAllowed(
          whole, cr, cr_free.html, &why, same, &head_only.html,
          &document_only.html))
          << why;
      // The response-header decisions have their own rule. Through a chain
      // with other filters in it only `Link` is promised.
      EXPECT_TRUE(alone ? pagespeed::packs::ChunkedHeadersAllowed(
                              whole, cr, run.origin_headers)
                        : pagespeed::packs::ChunkedHeadersAllowed(
                              LinkOnly(whole), LinkOnly(cr), origin_link))
          << pagespeed::packs::DescribeDecisions(cr.decisions);
      EXPECT_TRUE(pagespeed::packs::ChunkedHeadersAllowed(
          LinkOnly(reported), LinkOnly(cr_report), origin_link))
          << pagespeed::packs::DescribeDecisions(cr_report.decisions);
      EXPECT_EQ(pagespeed::packs::StripPackComments(cr_report.html),
                cr_free.html);
      EXPECT_EQ(
          DecisionsToJsonl(
              run.host, run.path,
              pagespeed::packs::DocumentDecisions(cr_report.decisions), false),
          forced_report_document);
    }
  }
}

// The chunked property is not vacuous through the chain either: some windows
// keep the whole-document result although a flush fell inside the head, and
// some drop a head plan that needed an element the server had already sent.
class RulePackChunkingTest : public RulePackChainTest {};

TEST_F(RulePackChunkingTest, ChunkingsReachBothOutcomes) {
  int kept = 0;
  int dropped = 0;
  for (const FixtureCase& c :
       pagespeed::packs::ListFixtureCases(kFixturesDir)) {
    auto kernel = pagespeed::packs::RunCase(kFixturesDir, c);
    ASSERT_TRUE(kernel.ok()) << c.name << ": " << kernel.status();
    if (kernel->enforce.html == kernel->roundtrip.html) continue;  // no-op
    auto loaded = pagespeed::packs::LoadPackFile(
        absl::StrCat(kFixturesDir, "/", kernel->pack_file), "");
    ASSERT_TRUE(loaded.ok()) << loaded.status();
    provider_.set_pack(
        std::make_shared<const pagespeed::packs::Pack>(*std::move(loaded)));
    Chain chain;
    chain.variant = &Variants()[0];
    chain.debug = kernel->debug;
    chain.mode = kernel->directive_mode;
    chain.flush_after = kernel->flush_cuts;
    if (kernel->has_origin_headers)
      chain.origin_headers = kernel->origin_headers;
    const RunResult whole = Run(chain, kernel->url, kernel->input_html);
    for (const pagespeed::packs::Chunking& chunking :
         pagespeed::packs::ChunkingsFor(kernel->input_html)) {
      // (Debug filter flush assert, as above.)
      if (kernel->debug && chunking.name.rfind("every-", 0) == 0) continue;
      chain.flush_after = MergeCuts(chunking.flush_after, kernel->flush_cuts);
      const RunResult cr = Run(chain, kernel->url, kernel->input_html);
      if (cr.html == whole.html && cr.decisions == whole.decisions) {
        if (chunking.name == "after-head" ||
            chunking.name == "before-head-close") {
          ++kept;
        }
      } else {
        ++dropped;
      }
    }
  }
  std::printf(
      "chain: %d chunked runs kept the whole-document result with a split "
      "head, %d dropped the head plan\n",
      kept, dropped);
  EXPECT_GT(kept, 0);
  EXPECT_GT(dropped, 0);
}

// The pack and pages of the per-port system test
// (test/system/system/test_rule_packs.py) through the same chain, so the data
// the servers get is known to load and to do what that test expects.
constexpr char kSystemTestDir[] = "install/mod_pagespeed_test/rule_packs";
constexpr char kSystemTestPath[] = "/mod_pagespeed_test/rule_packs/";

class RulePackSystemPagesTest : public RulePackChainTest {
 protected:
  void SetUp() override {
    RulePackChainTest::SetUp();
    auto loaded = pagespeed::packs::LoadPackFile(
        std::string(kSystemTestDir) + "/pack.json", "");
    ASSERT_TRUE(loaded.ok()) << loaded.status();
    provider_.set_pack(
        std::make_shared<const pagespeed::packs::Pack>(*std::move(loaded)));
  }

  RunResult RunPage(const std::string& host, const std::string& page,
                    pagespeed::packs::Mode mode) {
    auto html =
        pagespeed::packs::ReadFile(std::string(kSystemTestDir) + "/" + page);
    EXPECT_TRUE(html.ok()) << html.status();
    const Variant alone = Variants()[0];
    Chain chain;
    chain.variant = &alone;
    chain.mode = mode;
    return Run(chain, absl::StrCat("http://", host, kSystemTestPath, page),
               html.ok() ? *html : "");
  }
};

TEST_F(RulePackSystemPagesTest, MissingElementsAreInserted) {
  const RunResult r = RunPage("rulepacks.example.com", "missing.html",
                              pagespeed::packs::Mode::kEnforce);
  const Facts facts = CollectFacts(r.html);
  const std::string path = absl::StrCat(kSystemTestPath, "missing.html");
  EXPECT_EQ(facts.canonicals,
            std::vector<std::string>{"https://rulepacks.example.com" + path});
  EXPECT_EQ(facts.titles, std::vector<std::string>{"Title for " + path});
  EXPECT_EQ(facts.descriptions,
            std::vector<std::string>{"Description for " + path});
  EXPECT_EQ(r.decisions.size(), 3u);
}

TEST_F(RulePackSystemPagesTest, ACompletePageIsLeftAlone) {
  const RunResult r = RunPage("rulepacks.example.com", "present.html",
                              pagespeed::packs::Mode::kEnforce);
  const Chain plain = [&] {
    Chain c;
    c.variant = &Variants()[0];
    c.with_pack = false;
    return c;
  }();
  auto html =
      pagespeed::packs::ReadFile(std::string(kSystemTestDir) + "/present.html");
  ASSERT_TRUE(html.ok());
  EXPECT_EQ(
      r.html,
      Run(plain, "http://rulepacks.example.com/present.html", *html).html);
  for (const PackDecision& d : r.decisions) {
    EXPECT_EQ(d.action, pagespeed::packs::Action::kNone) << d.rule_id;
  }
}

TEST_F(RulePackSystemPagesTest, ABodyCanonicalIsReplacedByOneInTheHead) {
  const RunResult r = RunPage("rulepacks.example.com", "body-canonical.html",
                              pagespeed::packs::Mode::kEnforce);
  const Facts facts = CollectFacts(r.html);
  EXPECT_EQ(facts.canonicals,
            std::vector<std::string>{"https://rulepacks.example.com" +
                                     std::string(kSystemTestPath) +
                                     "body-canonical.html"});
  EXPECT_EQ(r.html.find("elsewhere"), std::string::npos) << r.html;
}

TEST_F(RulePackSystemPagesTest, TheReportHostIsLeftAloneAndReports) {
  const RunResult r = RunPage("rulepacks-report.example.com", "missing.html",
                              pagespeed::packs::Mode::kReport);
  EXPECT_TRUE(CollectFacts(r.html).canonicals.empty());
  ASSERT_EQ(r.decisions.size(), 3u);
  for (const PackDecision& d : r.decisions) {
    EXPECT_EQ(d.mode, pagespeed::packs::Mode::kReport) << d.rule_id;
    EXPECT_EQ(d.action, pagespeed::packs::Action::kInsert) << d.rule_id;
  }
}

TEST_F(RulePackSystemPagesTest, AHostTheFileDoesNotListIsLeftAlone) {
  const RunResult r = RunPage("rulepacks-unlisted.example.com", "missing.html",
                              pagespeed::packs::Mode::kEnforce);
  EXPECT_TRUE(CollectFacts(r.html).canonicals.empty());
  EXPECT_TRUE(r.decisions.empty());
}

// The packs of the multi-pack system test vhosts (install/.../rule_packs/multi)
// through the same chain.
class RulePackSystemMultiPagesTest : public RulePackChainTest {
 protected:
  RunResult RunMulti(const std::string& host,
                     const std::vector<std::string>& files,
                     const std::vector<std::string>& ids) {
    Chain chain;
    chain.variant = &Variants()[0];
    for (size_t i = 0; i < files.size(); ++i) {
      auto loaded = pagespeed::packs::LoadPackFile(
          absl::StrCat(kSystemTestDir, "/multi/", files[i]), "");
      EXPECT_TRUE(loaded.ok()) << loaded.status();
      const std::string path =
          absl::StrCat("/etc/pagespeed/packs/system-multi-", i, ".json");
      provider_.SetFor(path, std::make_shared<const pagespeed::packs::Pack>(
                                 *std::move(loaded)));
      chain.pack_paths.push_back(path);
    }
    chain.pack_ids = ids;
    auto html = pagespeed::packs::ReadFile(std::string(kSystemTestDir) +
                                           "/missing.html");
    EXPECT_TRUE(html.ok()) << html.status();
    return Run(chain,
               absl::StrCat("http://", host, kSystemTestPath, "missing.html"),
               html.ok() ? *html : "");
  }
};

TEST_F(RulePackSystemMultiPagesTest, TheFirstPackOwnsTheKindBothCarry) {
  const RunResult r =
      RunMulti("rulepacks-multi.example.com", {"a.json", "b.json"},
               {"edge-seo", "fixture-second"});
  const Facts facts = CollectFacts(r.html);
  const std::string path = absl::StrCat(kSystemTestPath, "missing.html");
  EXPECT_EQ(
      facts.canonicals,
      std::vector<std::string>{"https://rulepacks-multi.example.com" + path});
  EXPECT_EQ(facts.titles, std::vector<std::string>{"Second title for " + path});
  EXPECT_EQ(facts.descriptions,
            std::vector<std::string>{"Second description for " + path});
  ASSERT_EQ(r.decisions.size(), 4u);
  EXPECT_EQ(r.decisions[3].reason, pagespeed::packs::Reason::kClaimed);
  EXPECT_EQ(r.decisions[3].claimed_by, "edge-seo");
}

TEST_F(RulePackSystemMultiPagesTest, ASecondFileWithTheSamePackIdIsRefused) {
  const RunResult r = RunMulti("rulepacks-multi-dup.example.com",
                               {"a.json", "dup.json"}, {"edge-seo"});
  const Facts facts = CollectFacts(r.html);
  EXPECT_EQ(facts.canonicals.size(), 1u);
  EXPECT_TRUE(facts.titles.empty());
  EXPECT_EQ(r.decisions.size(), 1u);
}

// The Link header cases of the system test, on the pages and headers its
// flush origin serves (test/system/pagespeed_test_framework/flush_origin.py).
TEST_F(RulePackSystemPagesTest, TheLinkHeaderRuleReplacesOnlyTheConflict) {
  const std::string url =
      "http://rulepacks.example.com/mod_pagespeed_test/flush_origin/"
      "link_header";
  const std::string html =
      "<!doctype html>\n<html>\n<head>\n<meta charset=\"utf-8\">\n"
      "<title>T</title>\n</head>\n<body>\n<p>x</p>\n</body>\n</html>\n";
  const std::string preload =
      "</mod_pagespeed_test/rule_packs/app.css>; rel=preload; as=style";
  const std::string preconnect = "<https://cdn.example.net>; rel=preconnect";
  Chain chain;
  chain.variant = &Variants()[0];
  chain.origin_headers = std::vector<HeaderLine>{
      {"Content-Type", "text/html; charset=UTF-8"},
      {"Link", preload +
                   ", <https://wrong.example.com/stale>; "
                   "rel=\"canonical\", " +
                   preconnect}};
  const RunResult r = Run(chain, url, html);
  EXPECT_EQ(r.headers, (std::vector<HeaderLine>{
                           {"Content-Type", "text/html; charset=UTF-8"},
                           {"Link", preload + ", " + preconnect},
                           {"Link",
                            "<https://rulepacks.example.com/mod_pagespeed_test/"
                            "flush_origin/link_header>; rel=\"canonical\""}}));
  EXPECT_EQ(CollectFacts(r.html).canonicals,
            std::vector<std::string>{"https://rulepacks.example.com/"
                                     "mod_pagespeed_test/flush_origin/"
                                     "link_header"});
  // The report host reads the header and leaves it.
  chain.mode = pagespeed::packs::Mode::kReport;
  EXPECT_EQ(Run(chain, url, html).headers, *chain.origin_headers);
}

// And once the first window has gone out, as on the split-head route.
TEST_F(RulePackSystemPagesTest, TheLinkHeaderIsSkippedAfterAFlush) {
  const std::string url =
      "http://rulepacks.example.com/mod_pagespeed_test/flush_origin/"
      "split_head";
  const std::string first =
      "<!doctype html>\n<html>\n<head>\n<meta charset=\"utf-8\">\n";
  const std::string html =
      first + "</head>\n<body>\n<p>x</p>\n</body>\n</html>\n";
  Chain chain;
  chain.variant = &Variants()[0];
  chain.flush_after = {first.size()};
  chain.origin_headers = std::vector<HeaderLine>{
      {"Link", "<https://wrong.example.com/split>; rel=canonical"}};
  const RunResult r = Run(chain, url, html);
  EXPECT_EQ(r.headers, *chain.origin_headers);
  EXPECT_EQ(CollectFacts(r.html).canonicals,
            std::vector<std::string>{"https://rulepacks.example.com/"
                                     "mod_pagespeed_test/flush_origin/"
                                     "split_head"});
  ASSERT_EQ(r.decisions.size(), 2u);
  EXPECT_EQ(r.decisions[1].reason, pagespeed::packs::Reason::kHeadersFlushed);
  EXPECT_TRUE(r.decisions[1].header);
}

// ---------------------------------------------------------------------------
// Several packs on one vhost: the cases of testdata/multi-pack through the
// chain, in the three variants.
// ---------------------------------------------------------------------------

constexpr char kMultiFixturesDir[] =
    "test/pagespeed/kernel/packs/testdata/multi-pack";

class RulePackMultiFixtureTest
    : public RulePackChainTest,
      public testing::WithParamInterface<FixtureCase> {
 protected:
  // Loads the pack files of `run` into the provider (the files as the vhost
  // lists them, `transform` applied to the packs the host runs) and returns
  // the paths and the ids of the packs the host runs.
  void SetPacks(
      const CaseRun& run, Chain* chain,
      const std::function<std::shared_ptr<const pagespeed::packs::Pack>(
          size_t, std::shared_ptr<const pagespeed::packs::Pack>)>& transform) {
    provider_.ClearFiles();
    chain->pack_paths.clear();
    chain->pack_ids.clear();
    size_t running = 0;
    for (size_t i = 0; i < run.pack_files.size(); ++i) {
      auto loaded = pagespeed::packs::LoadPackFile(
          absl::StrCat(kMultiFixturesDir, "/", run.pack_files[i]), "");
      ASSERT_TRUE(loaded.ok()) << loaded.status();
      auto pack =
          std::make_shared<const pagespeed::packs::Pack>(*std::move(loaded));
      const std::string path =
          absl::StrCat("/etc/pagespeed/packs/multi-", i, ".json");
      if (!run.refused[i]) {
        chain->pack_ids.push_back(pack->info.id);
        pack = transform(running++, std::move(pack));
      }
      provider_.SetFor(path, std::move(pack));
      chain->pack_paths.push_back(path);
    }
  }
};

TEST_P(RulePackMultiFixtureTest, HoldsTheProperties) {
  const FixtureCase& c = GetParam();
  auto kernel = pagespeed::packs::RunCase(kMultiFixturesDir, c);
  ASSERT_TRUE(kernel.ok()) << kernel.status();
  auto expected = pagespeed::packs::ReadExpected(c);
  ASSERT_TRUE(expected.ok()) << expected.status();
  const CaseRun& run = *kernel;
  const std::string forced_report =
      DecisionsToJsonl(run.host, run.path, run.enforce.decisions, true);
  const Facts expected_facts = CollectFacts(expected->html);
  const std::vector<HeaderLine> origin_link = LinkLines(run.origin_headers);
  auto keep = [](size_t, std::shared_ptr<const pagespeed::packs::Pack> p) {
    return p;
  };
  std::string alone_decisions;
  std::vector<HeaderLine> alone_link;

  for (const Variant& variant : Variants()) {
    SCOPED_TRACE(variant.name);
    const bool alone = !variant.core_filters;
    Chain base;
    base.variant = &variant;
    base.mode = run.directive_mode;
    base.flush_after = run.flush_cuts;
    if (run.has_origin_headers) base.origin_headers = run.origin_headers;
    SetPacks(run, &base, keep);
    Chain pack_free = base;
    pack_free.with_pack = false;
    Chain report = base;
    report.mode = pagespeed::packs::Mode::kReport;

    RunResult whole = Run(base, run.url, run.input_html);
    RunResult roundtrip = Run(pack_free, run.url, run.input_html);
    RunResult reported = Run(report, run.url, run.input_html);
    const std::string decisions =
        DecisionsToJsonl(run.host, run.path, whole.decisions, false);

    // The reference runs of the chunked property: every way of dropping the
    // head and/or the end-of-document plan of each pack (claims kept).
    CaseRun refs;
    refs.drop_states = run.drop_states;
    std::vector<std::vector<unsigned>> states = run.drop_states;
    for (const std::vector<unsigned>& state : states) {
      Chain ref_chain = base;
      SetPacks(run, &ref_chain,
               [&](size_t i,
                   const std::shared_ptr<const pagespeed::packs::Pack>& p) {
                 return pagespeed::packs::ClaimOnlyPack(*p, state[i]);
               });
      refs.drop_runs.push_back(Run(ref_chain, run.url, run.input_html));
    }
    SetPacks(run, &base, keep);

    // One pack runs (a single file, or the second is refused): the
    // single-pack references.
    RunResult head_only;
    RunResult document_only;
    if (run.drop_states.empty()) {
      auto loaded = pagespeed::packs::LoadPackFile(
          absl::StrCat(kMultiFixturesDir, "/", run.pack_files[0]), "");
      ASSERT_TRUE(loaded.ok()) << loaded.status();
      auto full =
          std::make_shared<const pagespeed::packs::Pack>(*std::move(loaded));
      const std::string first = base.pack_paths[0];
      provider_.SetFor(first, pagespeed::packs::HeadOnlyPack(*full));
      head_only = Run(base, run.url, run.input_html);
      provider_.SetFor(first, pagespeed::packs::DocumentEndOnlyPack(*full));
      document_only = Run(base, run.url, run.input_html);
      provider_.SetFor(first, full);
    }

    if (alone) {
      EXPECT_EQ(whole.html, expected->html);
      EXPECT_EQ(decisions, expected->decisions);
      alone_decisions = decisions;
      alone_link = LinkLines(whole.headers);
      if (run.has_origin_headers) {
        EXPECT_EQ(pagespeed::packs::HeadersToText(whole.headers),
                  expected->headers);
      }
      // idempotence
      Chain again_chain = base;
      if (run.has_origin_headers) again_chain.origin_headers = whole.headers;
      RunResult again = Run(again_chain, run.url, whole.html);
      EXPECT_EQ(again.html, whole.html);
      EXPECT_EQ(again.headers, whole.headers);
      for (const PackDecision& d : again.decisions) {
        if (d.mode != pagespeed::packs::Mode::kEnforce) continue;
        EXPECT_EQ(d.action, pagespeed::packs::Action::kNone) << d.rule_id;
      }
      // determinism, and device independence
      RunResult second = Run(base, run.url, run.input_html);
      EXPECT_EQ(second.html, whole.html);
      EXPECT_EQ(second.decisions, whole.decisions);
      for (const char* agent : {kAndroidAgent, kIpadAgent}) {
        Chain device = base;
        device.user_agent = agent;
        RunResult other = Run(device, run.url, run.input_html);
        EXPECT_EQ(other.html, whole.html) << agent;
        EXPECT_EQ(other.decisions, whole.decisions) << agent;
      }
    } else {
      EXPECT_EQ(decisions, alone_decisions);
      const Facts got = CollectFacts(whole.html);
      EXPECT_EQ(got.canonicals, expected_facts.canonicals);
      EXPECT_EQ(got.titles, expected_facts.titles);
      EXPECT_EQ(got.descriptions, expected_facts.descriptions);
      EXPECT_EQ(got.json_ld, expected_facts.json_ld);
      EXPECT_EQ(LinkLines(whole.headers), alone_link);
    }

    // report-only
    EXPECT_EQ(DecisionsToJsonl(run.host, run.path, reported.decisions, false),
              forced_report);
    EXPECT_EQ(LinkLines(reported.headers), origin_link);
    EXPECT_EQ(reported.html, roundtrip.html);

    // chunked
    for (const pagespeed::packs::Chunking& chunking :
         pagespeed::packs::ChunkingsFor(run.input_html)) {
      SCOPED_TRACE(chunking.name);
      const std::vector<size_t> cuts =
          MergeCuts(chunking.flush_after, run.flush_cuts);
      Chain chunked = base;
      chunked.flush_after = cuts;
      Chain chunked_free = pack_free;
      chunked_free.flush_after = cuts;
      Chain chunked_report = report;
      chunked_report.flush_after = cuts;
      RunResult cr = Run(chunked, run.url, run.input_html);
      RunResult cr_free = Run(chunked_free, run.url, run.input_html);
      RunResult cr_report = Run(chunked_report, run.url, run.input_html);
      std::function<bool(const std::string&, const std::string&)> same;
      if (!alone) {
        same = [](const std::string& a, const std::string& b) {
          return CollectFacts(a) == CollectFacts(b);
        };
      }
      std::string why;
      if (run.drop_states.empty()) {
        EXPECT_TRUE(pagespeed::packs::ChunkedOutcomeAllowed(
            whole, cr, cr_free.html, &why, same, &head_only.html,
            &document_only.html))
            << why;
      } else {
        EXPECT_TRUE(pagespeed::packs::ChunkedOutcomeAllowedForPacks(
            whole, refs, cr, cr_free.html, &why, same))
            << why;
      }
      EXPECT_TRUE(alone ? pagespeed::packs::ChunkedHeadersAllowed(
                              whole, cr, run.origin_headers)
                        : pagespeed::packs::ChunkedHeadersAllowed(
                              LinkOnly(whole), LinkOnly(cr), origin_link))
          << pagespeed::packs::DescribeDecisions(cr.decisions);
      EXPECT_EQ(cr_report.html, cr_free.html);
      // The claimed decisions are the same in every chunking.
      auto claimed = [](const RunResult& r) {
        std::vector<std::string> out;
        for (const PackDecision& d : r.decisions) {
          if (d.reason == pagespeed::packs::Reason::kClaimed) {
            out.push_back(absl::StrCat(d.rule_id, "<-", d.claimed_by));
          }
        }
        return out;
      };
      EXPECT_EQ(claimed(cr), claimed(whole));
      EXPECT_EQ(claimed(cr_report), claimed(whole));
    }
  }
}

INSTANTIATE_TEST_SUITE_P(
    MultiPack, RulePackMultiFixtureTest,
    testing::ValuesIn(pagespeed::packs::ListFixtureCases(kMultiFixturesDir)),
    [](const testing::TestParamInfo<FixtureCase>& info) {
      std::string n = info.param.name;
      for (char& ch : n) {
        if (ch == '-') ch = '_';
      }
      return n;
    });

// The counters of the server: a claim moves rule_pack_decisions_claimed, a
// refused duplicate id moves rule_pack_refused_duplicate_id (once).
class RulePackMultiCounterTest : public RulePackMultiFixtureTest {
 protected:
  int64_t Stat(pagespeed::packs::Counter counter) {
    Variable* v =
        statistics()->GetVariable(pagespeed::packs::CounterName(counter));
    return v == nullptr ? -1 : v->Get();
  }
  RunResult RunNamed(const std::string& name) {
    FixtureCase c{name, absl::StrCat(kMultiFixturesDir, "/", name)};
    auto kernel = pagespeed::packs::RunCase(kMultiFixturesDir, c);
    EXPECT_TRUE(kernel.ok()) << kernel.status();
    Chain chain;
    chain.variant = &Variants()[0];
    SetPacks(*kernel, &chain,
             [](size_t, std::shared_ptr<const pagespeed::packs::Pack> p) {
               return p;
             });
    if (kernel->has_origin_headers)
      chain.origin_headers = kernel->origin_headers;
    return Run(chain, kernel->url, kernel->input_html);
  }
};

TEST_F(RulePackMultiCounterTest, AClaimIsCounted) {
  using pagespeed::packs::Counter;
  RunNamed("disjoint-kinds");
  EXPECT_EQ(0, Stat(Counter::kDecisionsClaimed));
  RunNamed("same-kind-keep-first");
  EXPECT_EQ(1, Stat(Counter::kDecisionsClaimed));
}

TEST_F(RulePackMultiCounterTest, ADuplicateIdIsRefusedAndTheFirstPackActs) {
  using pagespeed::packs::Counter;
  const RunResult r = RunNamed("duplicate-id");
  EXPECT_EQ(1, Stat(Counter::kRefusedDuplicateId));
  ASSERT_EQ(1u, r.decisions.size());
  EXPECT_EQ("a-canonical-disjoint", r.decisions[0].rule_id);
  EXPECT_EQ(1u, CollectFacts(r.html).canonicals.size());
  EXPECT_TRUE(CollectFacts(r.html).titles.empty());
}

INSTANTIATE_TEST_SUITE_P(
    Cases, RulePackFixtureTest,
    testing::ValuesIn(pagespeed::packs::ListFixtureCases(kFixturesDir)),
    [](const testing::TestParamInfo<FixtureCase>& info) {
      std::string n = info.param.name;
      for (char& ch : n) {
        if (ch == '-') ch = '_';
      }
      return n;
    });

}  // namespace
}  // namespace net_instaweb
