// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

#include "pagespeed/kernel/packs/rule_pack_filter.h"

#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "absl/strings/str_cat.h"
#include "gtest/gtest.h"
#include "pagespeed/kernel/base/null_message_handler.h"
#include "pagespeed/kernel/base/string_writer.h"
#include "pagespeed/kernel/html/html_keywords.h"
#include "pagespeed/kernel/html/html_parse.h"
#include "pagespeed/kernel/html/html_writer_filter.h"
#include "pagespeed/kernel/packs/pack_host.h"
#include "pagespeed/kernel/packs/pack_loader.h"
#include "pagespeed/kernel/packs/planner_common.h"
#include "test/pagespeed/kernel/packs/test_pack_host.h"

namespace pagespeed::packs {
namespace {

std::shared_ptr<const Pack> MakePack(std::string_view rules,
                                     std::string_view site_mode = "enforce") {
  const std::string json =
      absl::StrCat(R"({"pack":{"id":"edge-seo","version":"0.1.0"},)",
                   R"("sites":[{"host":"t.test","mode":")", site_mode,
                   R"("}],"rules":[)", rules, "]}");
  auto p = LoadPack(json, "");
  EXPECT_TRUE(p.ok()) << p.status() << "\n" << json;
  return p.ok() ? std::make_shared<const Pack>(*std::move(p)) : nullptr;
}

constexpr char kCanonicalKeep[] =
    R"({"id":"c","kind":"canonical","enforce":true,)"
    R"("value":{"template":"https://t.test{path}"}})";
constexpr char kCanonicalRepair[] =
    R"({"id":"c","kind":"canonical","enforce":true,"on_present":"repair",)"
    R"("value":{"template":"https://t.test{path}"}})";
constexpr char kCanonicalReplace[] =
    R"({"id":"c","kind":"canonical","enforce":true,"on_present":"replace",)"
    R"("value":{"template":"https://t.test{path}"}})";
constexpr char kTitleKeep[] = R"({"id":"t","kind":"title","enforce":true,)"
                              R"("value":{"template":"T {path}"}})";

struct FilterRun {
  std::string html;
  bool modified = false;
  bool would_modify = false;
  bool headers_flushed = false;
  bool headers_modified = false;
  SkipReason skip = SkipReason::kNone;
  std::vector<PackDecision> decisions;
  std::string disabled;  // why the filter disabled itself, if it did
};

struct Options {
  Mode mode = Mode::kEnforce;
  RulePackFilterOptions filter;
  // Parse in these chunks, with a Flush() after each.
  std::vector<std::string> chunks;
  // Flush after the last chunk too. A server that finishes the parse right
  // after its last chunk (no flush in between) leaves the whole tail, the
  // closing </body> and </html> included, in the final window.
  bool flush_last = true;
  bool debug = false;
  // The origin's response headers; the host has none when unset.
  std::optional<std::vector<std::pair<std::string, std::string>>> headers;
};

class RulePackFilterTest : public testing::Test {
 protected:
  void SetUp() override { net_instaweb::HtmlKeywords::Init(); }

  FilterRun Run(std::shared_ptr<const Pack> pack, std::string_view url,
                std::string_view html, const Options& o = Options()) {
    host_ = std::make_unique<TestPackHost>(o.debug);
    if (o.headers.has_value()) host_->SetResponseHeaders(*o.headers);
    net_instaweb::NullMessageHandler handler;
    net_instaweb::HtmlParse parser(&handler);
    std::vector<GoogleString> disabled;
    parser.SetDynamicallyDisabledFilterList(&disabled);
    RulePackFilter filter(&parser, std::move(pack), o.mode, host_.get(),
                          o.filter);
    parser.AddFilter(&filter);
    FilterRun run;
    net_instaweb::StringWriter writer(&run.html);
    net_instaweb::HtmlWriterFilter wf(&parser);
    wf.set_writer(&writer);
    parser.AddFilter(&wf);
    EXPECT_TRUE(parser.StartParse(url));
    if (o.chunks.empty()) {
      parser.ParseText(html);
    } else {
      for (size_t i = 0; i < o.chunks.size(); ++i) {
        parser.ParseText(o.chunks[i]);
        if (i + 1 < o.chunks.size() || o.flush_last) {
          parser.Flush();
          host_->FlushHeaders();
        }
      }
    }
    parser.FinishParse();
    run.modified = filter.modified();
    run.would_modify = filter.would_modify();
    run.headers_flushed = filter.headers_flushed();
    run.skip = filter.skip_reason();
    run.decisions = filter.decisions();
    run.headers_modified = filter.headers_modified();
    if (!disabled.empty()) run.disabled = disabled.front();
    return run;
  }

  std::unique_ptr<TestPackHost> host_;
};

TEST_F(RulePackFilterTest, HeadElementSeenEarlyIsRewritableAtHeadClose) {
  // The canonical is the first thing in <head>; a large body follows.
  std::string html =
      "<html><head><link rel=\"canonical\" href=\"https://x.test/\">"
      "<title>T</title></head><body>";
  for (int i = 0; i < 2000; ++i) html += "<p>filler paragraph</p>\n";
  html += "</body></html>";
  FilterRun r = Run(MakePack(kCanonicalReplace), "https://t.test/x", html);
  EXPECT_EQ(r.skip, SkipReason::kNone);
  EXPECT_TRUE(r.modified);
  EXPECT_NE(r.html.find("href=\"https://t.test/x\""), std::string::npos);
  EXPECT_EQ(r.html.find("x.test"), std::string::npos);
}

TEST_F(RulePackFilterTest, ReportModeLeavesTheBytesAlone) {
  const std::string html =
      "<html><head><title>T</title></head><body></body></html>";
  Options o;
  o.mode = Mode::kReport;
  FilterRun r = Run(MakePack(kCanonicalKeep), "https://t.test/x", html, o);
  EXPECT_EQ(r.html, html);
  EXPECT_FALSE(r.modified);
  EXPECT_TRUE(r.would_modify);
  ASSERT_EQ(r.decisions.size(), 1u);
  EXPECT_EQ(r.decisions[0].mode, Mode::kReport);
  EXPECT_EQ(r.decisions[0].action, Action::kInsert);
  EXPECT_EQ(host_->count(Counter::kDecisionsWouldApply), 1);
  EXPECT_EQ(host_->count(Counter::kPagesWouldModify), 1);
  EXPECT_EQ(host_->count(Counter::kPagesModified), 0);
}

TEST_F(RulePackFilterTest, SiteModeCapsTheRule) {
  const std::string html = "<html><head></head><body></body></html>";
  FilterRun r =
      Run(MakePack(kCanonicalKeep, "report"), "https://t.test/x", html);
  EXPECT_EQ(r.html, html);
  EXPECT_FALSE(r.modified);
}

TEST_F(RulePackFilterTest, RuleWithoutEnforceIsReportOnly) {
  const std::string html = "<html><head></head><body></body></html>";
  FilterRun r = Run(MakePack(R"({"id":"c","kind":"canonical",)"
                             R"("value":{"template":"https://t.test{path}"}})"),
                    "https://t.test/x", html);
  EXPECT_EQ(r.html, html);
  EXPECT_FALSE(r.modified);
  EXPECT_TRUE(r.would_modify);
}

TEST_F(RulePackFilterTest, OwnOutputIsAFixedPoint) {
  const std::string html =
      "<html><head><link rel=canonical href=a><link rel=canonical href=b>"
      "<title></title><meta name=description content=\"\"></head><body>"
      "</body></html>";
  auto pack = MakePack(
      R"({"id":"c","kind":"canonical","enforce":true,"on_present":"replace",)"
      R"("value":{"template":"https://t.test{path}"}},)"
      R"({"id":"t","kind":"title","enforce":true,"on_present":"repair",)"
      R"("value":{"template":"T"}},)"
      R"({"id":"d","kind":"description","enforce":true,"on_present":"repair",)"
      R"("value":{"template":"D"}})");
  FilterRun once = Run(pack, "https://t.test/x", html);
  EXPECT_TRUE(once.modified);
  FilterRun twice = Run(pack, "https://t.test/x", once.html);
  EXPECT_EQ(twice.html, once.html);
  EXPECT_FALSE(twice.modified);
}

TEST_F(RulePackFilterTest, PackOutlivesTheCallersReference) {
  auto pack = MakePack(kCanonicalKeep);
  TestPackHost host;
  net_instaweb::NullMessageHandler handler;
  net_instaweb::HtmlParse parser(&handler);
  RulePackFilter filter(&parser, pack, Mode::kEnforce, &host);
  pack.reset();  // the filter holds the only reference now
  parser.AddFilter(&filter);
  std::string out;
  net_instaweb::StringWriter writer(&out);
  net_instaweb::HtmlWriterFilter wf(&parser);
  wf.set_writer(&writer);
  parser.AddFilter(&wf);
  ASSERT_TRUE(parser.StartParse("https://t.test/x"));
  parser.ParseText("<html><head></head></html>");
  parser.FinishParse();
  EXPECT_TRUE(filter.modified());
  EXPECT_NE(out.find("rel=\"canonical\""), std::string::npos);
}

TEST_F(RulePackFilterTest, OneFilterServesManyDocuments) {
  // A parser (and its filters) is reused across requests; the URL and every
  // fact come from the current document.
  auto pack = MakePack(kCanonicalKeep);
  TestPackHost host;
  net_instaweb::NullMessageHandler handler;
  net_instaweb::HtmlParse parser(&handler);
  RulePackFilter filter(&parser, pack, Mode::kEnforce, &host);
  parser.AddFilter(&filter);
  std::string out;
  net_instaweb::StringWriter writer(&out);
  net_instaweb::HtmlWriterFilter wf(&parser);
  wf.set_writer(&writer);
  parser.AddFilter(&wf);
  const char* urls[] = {"https://t.test/a", "https://other.test/b",
                        "https://t.test/c"};
  const char* expected[] = {
      "<html><head><link rel=\"canonical\" href=\"https://t.test/a\">"
      "</head></html>",
      "<html><head></head></html>",
      "<html><head><link rel=\"canonical\" href=\"https://t.test/c\">"
      "</head></html>",
  };
  for (int i = 0; i < 3; ++i) {
    out.clear();
    ASSERT_TRUE(parser.StartParse(urls[i]));
    parser.ParseText("<html><head></head></html>");
    parser.FinishParse();
    EXPECT_EQ(out, expected[i]) << urls[i];
    EXPECT_EQ(filter.decisions().size(), i == 1 ? 0u : 1u) << urls[i];
  }
  EXPECT_EQ(host.count(Counter::kPagesEvaluated), 2);
  EXPECT_EQ(host.count(Counter::kPagesSkippedHostNotListed), 1);
}

TEST_F(RulePackFilterTest, HostNotListedDisablesTheFilter) {
  const std::string html = "<html><body></body></html>";
  FilterRun r = Run(MakePack(kCanonicalKeep), "https://other.test/x", html);
  EXPECT_EQ(r.skip, SkipReason::kHostNotListed);
  EXPECT_NE(r.disabled.find("rule_packs: host not listed"), std::string::npos)
      << r.disabled;
  EXPECT_EQ(host_->count(Counter::kPagesSkippedHostNotListed), 1);
  EXPECT_EQ(host_->count(Counter::kPagesEvaluated), 0);
}

TEST_F(RulePackFilterTest, NoRuleForThePathDisablesTheFilter) {
  auto pack = MakePack(
      R"({"id":"c","kind":"canonical","enforce":true,"match":{"paths":["/a/**"]},)"
      R"("value":{"template":"https://t.test{path}"}})");
  FilterRun r = Run(pack, "https://t.test/b", "<html><head></head></html>");
  EXPECT_FALSE(r.modified);
  EXPECT_TRUE(r.decisions.empty());
  EXPECT_NE(r.disabled.find("no rule"), std::string::npos) << r.disabled;
}

TEST_F(RulePackFilterTest, NoHeadSkipsThePage) {
  const std::string html = "<html><body></body></html>";
  FilterRun r = Run(MakePack(kCanonicalKeep), "https://t.test/x", html);
  EXPECT_EQ(r.skip, SkipReason::kNoHead);
  EXPECT_EQ(r.html, html);
  EXPECT_TRUE(r.decisions.empty());
  EXPECT_EQ(host_->count(Counter::kPagesSkippedNoHead), 1);
}

TEST_F(RulePackFilterTest, SizeLimitLeavesThePageUntouched) {
  const std::string html = "<html><head></head><body></body></html>";
  Options o;
  o.filter.max_added_bytes = 8;
  FilterRun r = Run(MakePack(kCanonicalKeep), "https://t.test/x", html, o);
  EXPECT_EQ(r.skip, SkipReason::kSizeLimit);
  EXPECT_EQ(r.html, html);
  EXPECT_FALSE(r.modified);
}

TEST_F(RulePackFilterTest, OnlyTheFirstHeadIsUsed) {
  const std::string html =
      "<html><head></head><head></head><body></body></html>";
  FilterRun r = Run(MakePack(kCanonicalKeep), "https://t.test/x", html);
  EXPECT_EQ(r.html,
            "<html><head><link rel=\"canonical\" href=\"https://t.test/x\">"
            "</head><head></head><body></body></html>");
}

TEST_F(RulePackFilterTest, AttributeValuesAreQuoted) {
  auto pack =
      MakePack(R"({"id":"c","kind":"canonical","enforce":true,)"
               R"("value":{"template":"https://t.test{path}?a=1&b=2"}})");
  FilterRun r =
      Run(pack, "https://t.test/x", "<html><head></head><body></body></html>");
  EXPECT_NE(r.html.find("href=\"https://t.test/x?a=1&amp;b=2\""),
            std::string::npos)
      << r.html;
}

TEST_F(RulePackFilterTest, TitleTextIsEscaped) {
  auto pack = MakePack(
      R"({"id":"t","kind":"title","enforce":true,)"
      R"("value":{"template":"</title><script>x</script> & {path}"}})");
  FilterRun r =
      Run(pack, "https://t.test/x", "<html><head></head><body></body></html>");
  EXPECT_NE(r.html.find("<title>&lt;/title&gt;&lt;script&gt;x&lt;/script&gt; "
                        "&amp; /x</title>"),
            std::string::npos)
      << r.html;
}

TEST_F(RulePackFilterTest, PageWithUnclosedHeadDoesNotCrash) {
  FilterRun r =
      Run(MakePack(kCanonicalKeep), "https://t.test/x", "<html><head><title>t");
  EXPECT_EQ(r.skip, SkipReason::kNone);
  FilterRun garbage = Run(MakePack(kCanonicalKeep), "https://t.test/x",
                          "<<<>>><head><link rel=><meta name=description");
  (void)garbage;
}

TEST_F(RulePackFilterTest, UnclosedInertElementInHeadSkipsThePage) {
  const std::string html =
      "<html><head><svg><title>t</title></head><body><p>x</p></body></html>";
  FilterRun r = Run(MakePack(kCanonicalKeep), "https://t.test/x", html);
  EXPECT_EQ(r.skip, SkipReason::kMalformedHead);
  EXPECT_FALSE(r.modified);
  EXPECT_TRUE(r.decisions.empty());
}

TEST_F(RulePackFilterTest, HeadWithoutCloseEndsAtBody) {
  FilterRun r =
      Run(MakePack(kCanonicalKeep), "https://t.test/x",
          "<html><head><title>T</title>\n<body><p>hi</p></body></html>");
  EXPECT_EQ(r.html,
            "<html><head><title>T</title>\n<link rel=\"canonical\" "
            "href=\"https://t.test/x\"><body><p>hi</p></body></html>");
}

TEST_F(RulePackFilterTest, ManyTitlesDoNotInvalidateTheOpenOne) {
  // More titles than a vector's first allocation: the element being read
  // must survive the growth.
  std::string html = "<html><head>";
  for (int i = 0; i < 40; ++i) html += absl::StrCat("<title>t", i, "</title>");
  html += "</head><body></body></html>";
  auto pack = MakePack(
      R"({"id":"t","kind":"title","enforce":true,"on_present":"repair",)"
      R"("value":{"template":"T"}})");
  FilterRun r = Run(pack, "https://t.test/x", html);
  EXPECT_TRUE(r.modified);
  EXPECT_NE(r.html.find("<title>t0</title>"), std::string::npos);
  EXPECT_EQ(r.html.find("<title>t1</title>"), std::string::npos);
}

TEST_F(RulePackFilterTest, SizeLimitCountsTheEscapedBytes) {
  // 40 '&' are 200 bytes once escaped.
  auto pack = MakePack(
      R"({"id":"d","kind":"description","enforce":true,)"
      R"("value":{"template":"&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&"}})");
  Options o;
  o.filter.max_added_bytes = 150;
  const std::string html = "<html><head></head><body></body></html>";
  EXPECT_EQ(Run(pack, "https://t.test/x", html, o).skip,
            SkipReason::kSizeLimit);
  o.filter.max_added_bytes = 400;
  EXPECT_EQ(Run(pack, "https://t.test/x", html, o).skip, SkipReason::kNone);
}

TEST_F(RulePackFilterTest, DebugCommentsComeFromTheHost) {
  Options o;
  o.debug = true;
  FilterRun r = Run(MakePack(kCanonicalKeep), "https://t.test/x",
                    "<html><head></head></html>", o);
  EXPECT_EQ(r.html,
            "<html><head><!-- pagespeed-pack edge-seo@0.1.0 rule=c "
            "action=insert mode=enforce --><link rel=\"canonical\" "
            "href=\"https://t.test/x\"></head></html>");
}

TEST_F(RulePackFilterTest, ReportModeIsByteIdenticalWithoutDebug) {
  const std::string html =
      "<html><head><title>Old</title></head><body>"
      "<link rel=canonical href=/body></body></html>";
  Options o;
  o.mode = Mode::kReport;
  FilterRun r = Run(MakePack(absl::StrCat(kCanonicalRepair, ",", kTitleKeep)),
                    "https://t.test/x", html, o);
  EXPECT_FALSE(r.modified);
  EXPECT_TRUE(r.would_modify);
  EXPECT_EQ(r.html, html);
}

TEST_F(RulePackFilterTest, ReportModeKeepsPageTextThatLooksLikeADebugComment) {
  // A mangled end tag whose text starts like a debug comment but never
  // closes: the page's bytes stay verbatim, and the comment the filter adds
  // after them is a comment of its own (the fuzz harness once took the two
  // for one comment and reported a difference that was not there).
  const std::string html = "<html><head></head></html<!-- pagespeed-pack >\n";
  Options o;
  o.mode = Mode::kReport;
  o.debug = true;
  FilterRun r = Run(MakePack(kCanonicalKeep), "https://t.test/x", html, o);
  EXPECT_FALSE(r.modified);
  EXPECT_EQ(r.html,
            "<html><head><!-- pagespeed-pack edge-seo@0.1.0 rule=c "
            "would=insert mode=report --></head></html<!-- pagespeed-pack "
            ">\n");
}

TEST_F(RulePackFilterTest, ReportModeDebugCommentsLeaveABriefHeadAsWritten) {
  // A head the page wrote as <head/> stays <head/>: a comment inside it
  // would make the writer spell out <head>...</head>, a change to the page
  // beyond the comment. The comments go right after it, in rule order.
  const std::string html = "<html><head/><body></body></html>";
  Options o;
  o.mode = Mode::kReport;
  o.debug = true;
  FilterRun r = Run(MakePack(absl::StrCat(kCanonicalKeep, ",", kTitleKeep)),
                    "https://t.test/x", html, o);
  EXPECT_FALSE(r.modified);
  EXPECT_EQ(r.html,
            "<html><head/><!-- pagespeed-pack edge-seo@0.1.0 rule=c "
            "would=insert mode=report --><!-- pagespeed-pack edge-seo@0.1.0 "
            "rule=t would=insert mode=report --><body></body></html>");
}

TEST_F(RulePackFilterTest, ReportModeWithDebugSaysWhatItWouldDo) {
  Options o;
  o.mode = Mode::kReport;
  o.debug = true;
  // A missing canonical: the would= comment goes where the insert would.
  FilterRun r = Run(MakePack(kCanonicalKeep), "https://t.test/x",
                    "<html><head></head></html>", o);
  EXPECT_FALSE(r.modified);
  EXPECT_EQ(r.html,
            "<html><head><!-- pagespeed-pack edge-seo@0.1.0 rule=c "
            "would=insert mode=report --></head></html>");
  // A wrong canonical under replace: before the element it would change.
  r = Run(MakePack(kCanonicalReplace), "https://t.test/x",
          "<html><head><link rel=canonical href=https://x.test/>"
          "</head></html>",
          o);
  EXPECT_EQ(r.html,
            "<html><head><!-- pagespeed-pack edge-seo@0.1.0 rule=c "
            "would=replace mode=report --><link rel=canonical "
            "href=https://x.test/></head></html>");
  // A canonical in the body under repair: marked where it would be removed.
  r = Run(MakePack(kCanonicalRepair), "https://t.test/x",
          "<html><head><link rel=canonical href=https://t.test/x></head>"
          "<body><link rel=canonical href=https://x.test/></body></html>",
          o);
  EXPECT_EQ(r.html,
            "<html><head><link rel=canonical href=https://t.test/x></head>"
            "<body><!-- pagespeed-pack edge-seo@0.1.0 rule=c would=remove "
            "mode=report --><link rel=canonical href=https://x.test/>"
            "</body></html>");
  // Nothing to do: no comment.
  r = Run(MakePack(kCanonicalKeep), "https://t.test/x",
          "<html><head><link rel=canonical href=https://t.test/x></head>"
          "</html>",
          o);
  EXPECT_EQ(r.html,
            "<html><head><link rel=canonical href=https://t.test/x></head>"
            "</html>");
}

TEST_F(RulePackFilterTest, FilterContract) {
  TestPackHost host;
  net_instaweb::NullMessageHandler handler;
  net_instaweb::HtmlParse parser(&handler);
  RulePackFilter keep(&parser, MakePack(kCanonicalKeep), Mode::kEnforce, &host);
  EXPECT_FALSE(keep.CanModifyUrls());
  EXPECT_EQ(keep.GetScriptUsage(),
            net_instaweb::HtmlFilter::kNeverInjectsScripts);
  EXPECT_STREQ(keep.Name(), "RulePackFilter");
  RulePackFilter replace(&parser, MakePack(kCanonicalReplace), Mode::kEnforce,
                         &host);
  // Replacing the canonical link never rewrites a resource URL.
  EXPECT_FALSE(replace.CanModifyUrls());
  RulePackFilter repair(&parser, MakePack(kCanonicalRepair), Mode::kEnforce,
                        &host);
  EXPECT_FALSE(repair.CanModifyUrls());
  RulePackFilter report(&parser, MakePack(kCanonicalReplace), Mode::kReport,
                        &host);
  EXPECT_FALSE(report.CanModifyUrls());
  RulePackFilter jsonld(
      &parser,
      MakePack(R"({"id":"j","kind":"jsonld","enforce":true,"value":)"
               R"({"template":"{\"@type\":\"Thing\"}"}})"),
      Mode::kEnforce, &host);
  EXPECT_EQ(jsonld.GetScriptUsage(),
            net_instaweb::HtmlFilter::kMayInjectScripts);
}

TEST_F(RulePackFilterTest, CountersFollowTheDecisions) {
  FilterRun r =
      Run(MakePack(absl::StrCat(kCanonicalKeep, ",", kTitleKeep)),
          "https://t.test/x", "<html><head><title>Old</title></head></html>");
  EXPECT_TRUE(r.modified);
  EXPECT_EQ(host_->count(Counter::kPagesEvaluated), 1);
  EXPECT_EQ(host_->count(Counter::kPagesModified), 1);
  EXPECT_EQ(host_->count(Counter::kDecisionsApplied), 1);  // the canonical
  EXPECT_EQ(host_->count(Counter::kDecisionsWouldApply), 0);
  ASSERT_EQ(host_->decisions().size(), 2u);
  EXPECT_EQ(host_->decisions(), r.decisions);
}

// ---------------------------------------------------------------------------
// Streaming: flush windows and late elements
// ---------------------------------------------------------------------------

TEST_F(RulePackFilterTest, AFlushMarksTheHeadersSent) {
  Options o;
  o.chunks = {"<html><head>", "</head></html>"};
  EXPECT_TRUE(
      Run(MakePack(kCanonicalKeep), "https://t.test/x", "", o).headers_flushed);
}

TEST_F(RulePackFilterTest, AnInsertSucceedsWhenTheHeadSpansAFlush) {
  // The head opened in an earlier window; the insert goes before </head>,
  // which is in the current one.
  Options o;
  o.chunks = {"<html><head><meta charset=utf-8>", "<title>T</title></head>",
              "<body></body></html>"};
  FilterRun r = Run(MakePack(kCanonicalKeep), "https://t.test/x", "", o);
  EXPECT_EQ(r.skip, SkipReason::kNone);
  EXPECT_EQ(r.html,
            "<html><head><meta charset=utf-8><title>T</title><link "
            "rel=\"canonical\" href=\"https://t.test/x\"></head><body></body>"
            "</html>");
  ASSERT_EQ(r.decisions.size(), 1u);
  EXPECT_EQ(r.decisions[0].action, Action::kInsert);
}

TEST_F(RulePackFilterTest, AnInsertAtBodySucceedsWhenTheHeadSpansAFlush) {
  Options o;
  o.chunks = {"<html><head><title>T</title>", "<body><p>x</p></body></html>"};
  FilterRun r = Run(MakePack(kCanonicalKeep), "https://t.test/x", "", o);
  EXPECT_EQ(r.html,
            "<html><head><title>T</title><link rel=\"canonical\" "
            "href=\"https://t.test/x\"><body><p>x</p></body></html>");
}

TEST_F(RulePackFilterTest, AnEditOfAnElementAlreadySentDropsTheHeadPlan) {
  // The canonical to replace went out in the first window. Nothing in the
  // head changes, not even the insert of the title the head lacks.
  Options o;
  o.chunks = {"<html><head><link rel=canonical href=https://x.test/>",
              "</head><body></body></html>"};
  auto pack = MakePack(absl::StrCat(kCanonicalReplace, ",", kTitleKeep));
  FilterRun r = Run(pack, "https://t.test/x", "", o);
  EXPECT_EQ(r.skip, SkipReason::kNotRewritable);
  EXPECT_FALSE(r.modified);
  EXPECT_EQ(r.html,
            "<html><head><link rel=canonical href=https://x.test/></head>"
            "<body></body></html>");
  ASSERT_EQ(r.decisions.size(), 2u);
  for (const PackDecision& d : r.decisions) {
    EXPECT_EQ(d.reason, Reason::kNotRewritable) << d.rule_id;
    EXPECT_EQ(d.action, Action::kNone) << d.rule_id;
    EXPECT_TRUE(d.before_hash.empty()) << d.rule_id;
    EXPECT_TRUE(d.after.empty()) << d.rule_id;
  }
  EXPECT_EQ(host_->count(Counter::kPagesSkippedNotRewritable), 1);
  EXPECT_EQ(host_->count(Counter::kDecisionsApplied), 0);
}

TEST_F(RulePackFilterTest, TheSameEditSucceedsInOneWindow) {
  auto pack = MakePack(absl::StrCat(kCanonicalReplace, ",", kTitleKeep));
  FilterRun r = Run(pack, "https://t.test/x",
                    "<html><head><link rel=canonical href=https://x.test/>"
                    "</head><body></body></html>");
  EXPECT_EQ(r.skip, SkipReason::kNone);
  EXPECT_EQ(r.html,
            "<html><head><link rel=canonical href=\"https://t.test/x\">"
            "<title>T /x</title></head><body></body></html>");
}

TEST_F(RulePackFilterTest, LateCanonicalAfterAnInsertIsRemovedInAnyWindow) {
  // keep inserted a canonical at head close; one in the body, flushed in a
  // later window, would make the page worse than the input.
  Options o;
  o.chunks = {"<html><head></head><body><p>a</p>",
              "<link rel=canonical href=https://x.test/>", "</body></html>"};
  FilterRun r = Run(MakePack(kCanonicalKeep), "https://t.test/x", "", o);
  EXPECT_EQ(r.html,
            "<html><head><link rel=\"canonical\" href=\"https://t.test/x\">"
            "</head><body><p>a</p></body></html>");
  ASSERT_EQ(r.decisions.size(), 2u);
  EXPECT_EQ(r.decisions[0].action, Action::kInsert);
  EXPECT_EQ(r.decisions[1].action, Action::kRemove);
  EXPECT_EQ(r.decisions[1].reason, Reason::kLateConflict);
  EXPECT_FALSE(r.decisions[1].before_hash.empty());
  EXPECT_EQ(host_->count(Counter::kLateRemoved), 1);
}

TEST_F(RulePackFilterTest, LateDuplicateOfTheInsertedValueIsDeduped) {
  FilterRun r = Run(MakePack(kCanonicalKeep), "https://t.test/x",
                    "<html><head></head><body>"
                    "<link rel=canonical href=\"https://t.test/x\">"
                    "</body></html>");
  EXPECT_EQ(r.html,
            "<html><head><link rel=\"canonical\" href=\"https://t.test/x\">"
            "</head><body></body></html>");
  ASSERT_EQ(r.decisions.size(), 2u);
  EXPECT_EQ(r.decisions[1].action, Action::kDedupe);
  EXPECT_EQ(r.decisions[1].reason, Reason::kDuplicate);
}

TEST_F(RulePackFilterTest, LateConflictUnderKeepWithAHeadValueIsLeftAlone) {
  // The input already had two canonicals; keep does not pick one.
  const std::string html =
      "<html><head><link rel=canonical href=https://a.test/></head><body>"
      "<link rel=canonical href=https://b.test/></body></html>";
  FilterRun r = Run(MakePack(kCanonicalKeep), "https://t.test/x", html);
  EXPECT_EQ(r.html, html);
  ASSERT_EQ(r.decisions.size(), 1u);
  EXPECT_EQ(r.decisions[0].reason, Reason::kConflict);
  EXPECT_EQ(r.decisions[0].defect, "canonical-multiple");
}

TEST_F(RulePackFilterTest, LateDuplicatesUnderKeepAreFoundAcrossLateElements) {
  // Each late element is compared with every earlier one, late ones
  // included: the second b.test is a duplicate of the first.
  Options o;
  o.chunks = {"<html><head><link rel=canonical href=https://a.test/></head>",
              "<body><link rel=canonical href=https://b.test/>",
              "<link rel=canonical href=https://a.test/>",
              "<link rel=canonical href=https://b.test/></body></html>"};
  FilterRun r = Run(MakePack(kCanonicalKeep), "https://t.test/x", "", o);
  EXPECT_EQ(r.html,
            "<html><head><link rel=canonical href=https://a.test/></head>"
            "<body><link rel=canonical href=https://b.test/></body></html>");
  EXPECT_EQ(host_->count(Counter::kLateRemoved), 2);
}

TEST_F(RulePackFilterTest,
       LateElementsUnderRepairAreRemovedAndDecidedAsAWhole) {
  Options o;
  o.chunks = {"<html><head></head><body>",
              "<link rel=canonical href=https://b.test/>", "</body></html>"};
  FilterRun r = Run(MakePack(kCanonicalRepair), "https://t.test/x", "", o);
  EXPECT_EQ(r.html,
            "<html><head><link rel=\"canonical\" href=\"https://t.test/x\">"
            "</head><body></body></html>");
  // The decision is the ladder's over the whole document: the page had a
  // canonical, outside the head.
  ASSERT_EQ(r.decisions.size(), 1u);
  EXPECT_EQ(r.decisions[0].action, Action::kReplace);
  EXPECT_EQ(r.decisions[0].reason, Reason::kOutOfHead);
  EXPECT_EQ(r.decisions[0].defect, "canonical-unusable");
}

TEST_F(RulePackFilterTest, LateElementsAreOnlyRecordedInReportMode) {
  const std::string html =
      "<html><head></head><body><title>B</title></body></html>";
  Options o;
  o.mode = Mode::kReport;
  FilterRun r = Run(MakePack(kTitleKeep), "https://t.test/x", html, o);
  EXPECT_EQ(r.html, html);
  ASSERT_EQ(r.decisions.size(), 2u);
  EXPECT_EQ(r.decisions[0].action, Action::kInsert);
  EXPECT_EQ(r.decisions[1].reason, Reason::kLateConflict);
  EXPECT_EQ(r.decisions[1].mode, Mode::kReport);
  EXPECT_EQ(host_->count(Counter::kLateRemoved), 0);
}

TEST_F(RulePackFilterTest, LateElementsAreLeftAloneWhenTheHeadPlanDropped) {
  Options o;
  o.chunks = {"<html><head><title>a</title><title>b</title>",
              "</head><body><title>c</title></body></html>"};
  auto pack = MakePack(
      R"({"id":"t","kind":"title","enforce":true,"on_present":"repair",)"
      R"("value":{"template":"T"}})");
  FilterRun r = Run(pack, "https://t.test/x", "", o);
  EXPECT_FALSE(r.modified);
  EXPECT_EQ(r.html,
            "<html><head><title>a</title><title>b</title></head><body>"
            "<title>c</title></body></html>");
  ASSERT_EQ(r.decisions.size(), 1u);
  EXPECT_EQ(r.decisions[0].reason, Reason::kNotRewritable);
}

TEST_F(RulePackFilterTest, ALateElementIsNotDecidedAsAWholeWhenTheHeadDropped) {
  // The title edit needs an element already sent, so the head plan drops.
  // The canonical rule had nothing to do at head close (its head canonical is
  // usable); the body canonical it would remove arrives late and must not be
  // reported as removed, since nothing in the page changed.
  Options o;
  o.chunks = {
      "<html><head><link rel=canonical href=https://t.test/x>"
      "<title>Old</title>",
      "</head><body><link rel=canonical href=https://b.test/>"
      "</body></html>"};
  auto pack = MakePack(absl::StrCat(
      kCanonicalRepair, ",",
      R"({"id":"t","kind":"title","enforce":true,"on_present":"replace",)"
      R"("value":{"template":"T"}})"));
  FilterRun r = Run(pack, "https://t.test/x", "", o);
  EXPECT_EQ(r.skip, SkipReason::kNotRewritable);
  EXPECT_FALSE(r.modified);
  EXPECT_EQ(r.html,
            "<html><head><link rel=canonical href=https://t.test/x>"
            "<title>Old</title></head><body>"
            "<link rel=canonical href=https://b.test/></body></html>");
  ASSERT_EQ(r.decisions.size(), 2u);
  for (const PackDecision& d : r.decisions) {
    EXPECT_EQ(d.action, Action::kNone) << d.rule_id;
    EXPECT_EQ(d.reason, Reason::kNotRewritable) << d.rule_id;
    EXPECT_TRUE(d.after.empty()) << d.rule_id;
    EXPECT_TRUE(d.before_hash.empty()) << d.rule_id;
  }
  EXPECT_EQ(host_->count(Counter::kPagesSkippedNotRewritable), 1);
  EXPECT_EQ(host_->count(Counter::kDecisionsApplied), 0);
  EXPECT_EQ(host_->count(Counter::kPagesModified), 0);
  EXPECT_EQ(host_->count(Counter::kLateRemoved), 0);
  EXPECT_EQ(host_->count(Counter::kLateUnremovable), 0);
}

TEST_F(RulePackFilterTest, ATitleSplitByAFlushIsSeenWhole) {
  // The lexer holds back a <title> until its end tag, so a flush between
  // "<title>a" and "b</title>" does not split the element across windows.
  // The filter relies on this: a title, head or late, is never half sent.
  auto replace = MakePack(
      R"({"id":"t","kind":"title","enforce":true,"on_present":"replace",)"
      R"("value":{"template":"T"}})");
  Options head;
  head.chunks = {"<html><head><title>a",
                 "b</title></head><body></body></html>"};
  FilterRun h = Run(replace, "https://t.test/x", "", head);
  EXPECT_EQ(h.skip, SkipReason::kNone);
  EXPECT_EQ(h.html, "<html><head><title>T</title></head><body></body></html>");
  ASSERT_EQ(h.decisions.size(), 1u);
  EXPECT_EQ(h.decisions[0].action, Action::kReplace);
  EXPECT_EQ(h.decisions[0].before_hash, HashValue("ab"));

  Options late;
  late.chunks = {"<html><head><title>T</title></head><body><title>a",
                 "b</title></body></html>"};
  FilterRun l = Run(replace, "https://t.test/x", "", late);
  EXPECT_EQ(l.html, "<html><head><title>T</title></head><body></body></html>");
  EXPECT_EQ(host_->count(Counter::kLateRemoved), 1);
  EXPECT_EQ(host_->count(Counter::kLateUnremovable), 0);
}

// ---------------------------------------------------------------------------
// jsonld: the document-end decision
// ---------------------------------------------------------------------------

std::string JsonRule(std::string_view on_present,
                     std::string_view type = "Organization",
                     std::string_view id = "j") {
  return absl::StrCat(R"({"id":")", id,
                      R"(","kind":"jsonld","enforce":true,"on_present":")",
                      on_present, R"(","value":{"template":)",
                      R"("{\"@type\":\")", type,
                      R"(\",\"name\":\"{title}\"}"}})");
}

constexpr char kBlockOrg[] =
    R"(<script type="application/ld+json">{"@type":"Organization","name":"T /x"}</script>)";

TEST_F(RulePackFilterTest, JsonLdIsAppendedToTheBodyBeforeItsEnd) {
  FilterRun r = Run(MakePack(absl::StrCat(kTitleKeep, ",", JsonRule("keep"))),
                    "https://t.test/x",
                    "<html><head></head><body><p>a</p></body></html>");
  EXPECT_EQ(r.html, absl::StrCat("<html><head><title>T /x</title></head>"
                                 "<body><p>a</p>",
                                 kBlockOrg, "</body></html>"));
  ASSERT_EQ(r.decisions.size(), 2u);
  EXPECT_EQ(r.decisions[0].kind->name(), "title");
  EXPECT_EQ(r.decisions[1].kind->name(), "jsonld");
  EXPECT_EQ(r.decisions[1].action, Action::kInsert);
  EXPECT_EQ(r.decisions[1].after, "Organization");
}

TEST_F(RulePackFilterTest, JsonLdFallsBackToTheEndOfHtmlWhenTheBodyWasSent) {
  Options o;
  o.flush_last = false;
  o.chunks = {"<html><head></head><body><p>a</p></body>", "</html>"};
  FilterRun r = Run(MakePack(JsonRule("keep")), "https://t.test/x", "", o);
  EXPECT_EQ(r.skip, SkipReason::kNone);
  EXPECT_EQ(r.html, absl::StrCat("<html><head></head><body><p>a</p></body>",
                                 R"(<script type="application/ld+json">)"
                                 R"({"@type":"Organization","name":""}</script>)",
                                 "</html>"));
  ASSERT_EQ(r.decisions.size(), 1u);
  EXPECT_EQ(r.decisions[0].action, Action::kInsert);
}

TEST_F(RulePackFilterTest, JsonLdWithNowhereToGoIsNotRewritable) {
  // No <body>, no <html>: there is no element to append to.
  const std::string html = "<head><title>T</title></head>";
  FilterRun r = Run(MakePack(JsonRule("keep")), "https://t.test/x", html);
  EXPECT_EQ(r.html, html);
  EXPECT_EQ(r.skip, SkipReason::kNotRewritable);
  ASSERT_EQ(r.decisions.size(), 1u);
  EXPECT_EQ(r.decisions[0].action, Action::kNone);
  EXPECT_EQ(r.decisions[0].reason, Reason::kNotRewritable);
  // ... but in report mode nothing was going to change, so the plan stands.
  Options report;
  report.mode = Mode::kReport;
  FilterRun rr = Run(MakePack(JsonRule("keep")), "https://t.test/x", html,
                     report);
  EXPECT_EQ(rr.skip, SkipReason::kNone);
  ASSERT_EQ(rr.decisions.size(), 1u);
  EXPECT_EQ(rr.decisions[0].action, Action::kInsert);
}

TEST_F(RulePackFilterTest, JsonLdSeesBlocksInTheHeadAndTheBody) {
  const std::string html = absl::StrCat(
      "<html><head>", kBlockOrg, "</head><body>", kBlockOrg, "</body></html>");
  // keep: an exact duplicate is removed, nothing is inserted.
  FilterRun r = Run(MakePack(JsonRule("keep")), "https://t.test/x", html);
  EXPECT_EQ(r.html, absl::StrCat("<html><head>", kBlockOrg,
                                 "</head><body></body></html>"));
  ASSERT_EQ(r.decisions.size(), 1u);
  EXPECT_EQ(r.decisions[0].action, Action::kDedupe);
  EXPECT_EQ(r.decisions[0].removed, "Organization (duplicate)");
}

TEST_F(RulePackFilterTest, JsonLdStaysOutOfInertSubtrees) {
  const std::string html = absl::StrCat(
      "<html><head></head><body><noscript>", kBlockOrg,
      "</noscript></body></html>");
  FilterRun r = Run(MakePack(JsonRule("keep")), "https://t.test/x", html);
  // The block in <noscript> is not the page's: the rule inserts its own.
  EXPECT_EQ(r.decisions.at(0).action, Action::kInsert);
}

TEST_F(RulePackFilterTest, JsonLdSeesTheValuesTheHeadRulesWrote) {
  FilterRun r = Run(MakePack(absl::StrCat(kTitleKeep, ",", JsonRule("keep"))),
                    "https://t.test/x",
                    "<html><head></head><body></body></html>");
  EXPECT_NE(r.html.find(R"("name":"T /x")"), std::string::npos) << r.html;
  // The head rule only reports: an enforcing jsonld rule quotes what the page
  // has (nothing), not the title nobody wrote.
  auto pack = MakePack(absl::StrCat(
      R"({"id":"t","kind":"title","enforce":false,"value":{"template":"T"}},)",
      JsonRule("keep")));
  FilterRun q = Run(pack, "https://t.test/x",
                    "<html><head></head><body></body></html>");
  EXPECT_NE(q.html.find(R"("name":"")"), std::string::npos) << q.html;
  EXPECT_EQ(q.html.find("<title>"), std::string::npos);
}

TEST_F(RulePackFilterTest, JsonLdRemovalOfASentBlockIsNotRewritable) {
  // The invalid block went out in the first window: repair cannot remove it,
  // and the insert the rule also wants is dropped with it.
  Options o;
  o.chunks = {R"(<html><head><script type="application/ld+json">{bad</script>)",
              "</head><body></body></html>"};
  FilterRun r = Run(MakePack(JsonRule("repair")), "https://t.test/x", "", o);
  EXPECT_EQ(r.skip, SkipReason::kNotRewritable);
  EXPECT_FALSE(r.modified);
  EXPECT_EQ(r.html,
            R"(<html><head><script type="application/ld+json">{bad</script>)"
            "</head><body></body></html>");
  ASSERT_EQ(r.decisions.size(), 1u);
  EXPECT_EQ(r.decisions[0].action, Action::kNone);
  EXPECT_EQ(r.decisions[0].reason, Reason::kNotRewritable);
  EXPECT_TRUE(r.decisions[0].removed.empty());
  EXPECT_EQ(host_->count(Counter::kPagesSkippedNotRewritable), 1);
  // The same page in one window is repaired.
  FilterRun whole =
      Run(MakePack(JsonRule("repair")), "https://t.test/x",
          R"(<html><head><script type="application/ld+json">{bad</script>)"
          "</head><body></body></html>");
  EXPECT_EQ(whole.skip, SkipReason::kNone);
  EXPECT_EQ(whole.decisions.at(0).action, Action::kReplace);
}

TEST_F(RulePackFilterTest, AJsonLdBlockSplitByAFlushIsRemovedWholeOrNotAtAll) {
  // The lexer holds back a <script> until its end tag, so a flush inside the
  // text does not split the block across windows (as for <title>): the
  // removal through the element's own events succeeds.
  const std::string tail = "</head><body></body></html>";
  const std::string head =
      R"(<html><head><script type="application/ld+json">{"@type":"Org)";
  Options split;
  split.flush_last = false;
  split.chunks = {head, absl::StrCat(R"(anization"} {junk</script>)", tail)};
  FilterRun r = Run(MakePack(JsonRule("repair", "Thing")), "https://t.test/x",
                    "", split);
  Options one;
  FilterRun whole = Run(MakePack(JsonRule("repair", "Thing")),
                        "https://t.test/x", absl::StrCat(head, R"(anization"} {junk</script>)", tail), one);
  // Either the whole-document result, or the page as sent with the rules
  // recording not_rewritable. Never a half-removed block.
  if (r.html == whole.html) {
    EXPECT_EQ(r.decisions, whole.decisions);
    EXPECT_EQ(r.html.find("{junk"), std::string::npos);
  } else {
    EXPECT_EQ(r.html, absl::StrCat(head, R"(anization"} {junk</script>)", tail));
    ASSERT_EQ(r.decisions.size(), 1u);
    EXPECT_EQ(r.decisions[0].reason, Reason::kNotRewritable);
  }
  // Pin the behaviour of this lexer: whole.
  EXPECT_EQ(r.html, whole.html);

  // A block whose text reaches the filter in two windows (a flush right after
  // the start tag, then the text and the end tag) is the same story.
  Options after_start;
  after_start.flush_last = false;
  after_start.chunks = {
      R"(<html><head><script type="application/ld+json">)",
      absl::StrCat(R"({junk</script>)", tail)};
  FilterRun s = Run(MakePack(JsonRule("repair", "Thing")), "https://t.test/x",
                    "", after_start);
  if (s.skip == SkipReason::kNone) {
    EXPECT_EQ(s.html.find("{junk"), std::string::npos);
  } else {
    EXPECT_EQ(s.skip, SkipReason::kNotRewritable);
    EXPECT_NE(s.html.find("{junk"), std::string::npos);
    EXPECT_EQ(s.decisions.at(0).reason, Reason::kNotRewritable);
  }
}

TEST_F(RulePackFilterTest, JsonLdBlockInTheLastWindowIsRewrittenInPlace) {
  const std::string html = absl::StrCat(
      R"(<html><head></head><body><script id="ld" type="application/ld+json">)"
      R"({"@type":"Organization","name":"Old"}</script></body></html>)");
  FilterRun r = Run(MakePack(JsonRule("replace")), "https://t.test/x", html);
  EXPECT_EQ(
      r.html,
      R"(<html><head></head><body><script id="ld" type="application/ld+json">)"
      R"({"@type":"Organization","name":""}</script></body></html>)");
  EXPECT_EQ(r.decisions.at(0).action, Action::kReplace);
  // Rewritten again: nothing to do.
  FilterRun again = Run(MakePack(JsonRule("replace")), "https://t.test/x", r.html);
  EXPECT_FALSE(again.modified);
  EXPECT_EQ(again.decisions.at(0).reason, Reason::kEqual);
}

TEST_F(RulePackFilterTest, TheHeadPlanDroppedDoesNotStopTheDocumentPlan) {
  // The title edit needs an element already sent, so the head plan drops; the
  // JSON-LD append only needs the body, so it goes ahead -- quoting the
  // title the page really has.
  Options o;
  o.flush_last = false;
  o.chunks = {"<html><head><title>Old</title>",
              "</head><body></body></html>"};
  auto pack = MakePack(absl::StrCat(
      R"({"id":"t","kind":"title","enforce":true,"on_present":"replace",)"
      R"("value":{"template":"New"}},)",
      JsonRule("keep")));
  FilterRun r = Run(pack, "https://t.test/x", "", o);
  EXPECT_EQ(r.skip, SkipReason::kNotRewritable);
  EXPECT_NE(r.html.find("<title>Old</title>"), std::string::npos);
  EXPECT_NE(r.html.find(R"("name":"Old")"), std::string::npos) << r.html;
  ASSERT_EQ(r.decisions.size(), 2u);
  EXPECT_EQ(r.decisions[0].reason, Reason::kNotRewritable);
  EXPECT_EQ(r.decisions[1].action, Action::kInsert);
}

TEST_F(RulePackFilterTest, ReportModeLeavesJsonLdAlone) {
  const std::string html = "<html><head></head><body></body></html>";
  Options o;
  o.mode = Mode::kReport;
  FilterRun r = Run(MakePack(JsonRule("keep")), "https://t.test/x", html, o);
  EXPECT_EQ(r.html, html);
  EXPECT_FALSE(r.modified);
  EXPECT_TRUE(r.would_modify);
  EXPECT_EQ(r.decisions.at(0).action, Action::kInsert);
  EXPECT_EQ(r.decisions.at(0).mode, Mode::kReport);
}

TEST_F(RulePackFilterTest, JsonLdDebugCommentSitsInFrontOfTheBlock) {
  Options o;
  o.debug = true;
  FilterRun r = Run(MakePack(JsonRule("keep")), "https://t.test/x",
                    "<html><head></head><body></body></html>", o);
  EXPECT_NE(
      r.html.find("<body><!-- pagespeed-pack edge-seo@0.1.0 rule=j "
                  "action=insert mode=enforce --><script"),
      std::string::npos)
      << r.html;
}

TEST_F(RulePackFilterTest, TheSizeLimitCoversBothDecisionPoints) {
  RulePackFilterOptions limit;
  limit.max_added_bytes = 200;
  Options o;
  o.filter = limit;
  // Each plan fits alone (the canonical about 80 bytes, the block about 120);
  // together they do not.
  auto pack = MakePack(absl::StrCat(
      kCanonicalKeep, ",",
      R"({"id":"j","kind":"jsonld","enforce":true,"value":{"template":)"
      R"("{\"@type\":\"Thing\",\"name\":\")",
      std::string(60, 'x'), R"(\"}"}})"));
  FilterRun r = Run(pack, "https://t.test/x",
                    "<html><head></head><body></body></html>", o);
  EXPECT_EQ(r.skip, SkipReason::kSizeLimit);
  EXPECT_NE(r.html.find("canonical"), std::string::npos);
  EXPECT_EQ(r.html.find("ld+json"), std::string::npos);
}

// ---------------------------------------------------------------------------
// hreflang
// ---------------------------------------------------------------------------

constexpr char kHreflangKeep[] =
    R"({"id":"h","kind":"hreflang","enforce":true,"value":{"pattern":)"
    R"({"en":"https://t.test{path}","de":"https://t.test/de{path}"}}})";
constexpr char kHreflangRepair[] =
    R"({"id":"h","kind":"hreflang","enforce":true,"on_present":"repair",)"
    R"("value":{"pattern":{"en":"https://t.test{path}","de":"https://t.test/de{path}"}}})";

TEST_F(RulePackFilterTest, HreflangClusterIsInsertedAtHeadClose) {
  FilterRun r = Run(MakePack(kHreflangKeep), "https://t.test/x",
                    "<html><head><title>T</title></head><body></body></html>");
  EXPECT_EQ(r.html,
            "<html><head><title>T</title>"
            "<link rel=\"alternate\" hreflang=\"de\" "
            "href=\"https://t.test/de/x\">"
            "<link rel=\"alternate\" hreflang=\"en\" href=\"https://t.test/x\">"
            "</head><body></body></html>");
  ASSERT_EQ(r.decisions.size(), 1u);
  EXPECT_EQ(r.decisions[0].action, Action::kInsert);
}

TEST_F(RulePackFilterTest, HreflangInsertSucceedsWhenTheHeadSpansAFlush) {
  Options o;
  o.chunks = {"<html><head><title>T</title>", "</head><body></body></html>"};
  FilterRun r = Run(MakePack(kHreflangKeep), "https://t.test/x", "", o);
  EXPECT_EQ(r.skip, SkipReason::kNone);
  EXPECT_NE(r.html.find("hreflang=\"en\""), std::string::npos);
}

TEST_F(RulePackFilterTest, HreflangRepairOfASentLinkIsNotRewritable) {
  Options o;
  o.chunks = {"<html><head><link rel=alternate hreflang=xx_YY href=/rel>",
              "</head><body></body></html>"};
  FilterRun r = Run(MakePack(kHreflangRepair), "https://t.test/x", "", o);
  EXPECT_EQ(r.skip, SkipReason::kNotRewritable);
  EXPECT_FALSE(r.modified);
  ASSERT_EQ(r.decisions.size(), 1u);
  EXPECT_EQ(r.decisions[0].reason, Reason::kNotRewritable);
}

TEST_F(RulePackFilterTest, HreflangLinkInTheBodyIsRemovedUnderRepair) {
  FilterRun r = Run(MakePack(kHreflangRepair), "https://t.test/x",
                    "<html><head></head><body>"
                    "<link rel=alternate hreflang=fr href=https://t.test/fr>"
                    "</body></html>");
  EXPECT_EQ(r.html.find("hreflang=fr"), std::string::npos) << r.html;
  EXPECT_NE(r.html.find("hreflang=\"en\""), std::string::npos);
  ASSERT_EQ(r.decisions.size(), 1u);
  // Decided as the ladder decides it over the whole document.
  EXPECT_EQ(r.decisions[0].removed, "fr=https://t.test/fr (out_of_head)");
  EXPECT_EQ(host_->count(Counter::kLateRemoved), 1);
}

TEST_F(RulePackFilterTest, HreflangLateLinkForAnInsertedCodeIsRemovedUnderKeep) {
  FilterRun r = Run(MakePack(kHreflangKeep), "https://t.test/x",
                    "<html><head></head><body>"
                    "<link rel=alternate hreflang=de href=https://t.test/old>"
                    "<link rel=alternate hreflang=fr href=https://t.test/fr>"
                    "</body></html>");
  EXPECT_EQ(r.html.find("/old"), std::string::npos) << r.html;
  EXPECT_NE(r.html.find("hreflang=fr"), std::string::npos) << r.html;
  ASSERT_EQ(r.decisions.size(), 2u);
  EXPECT_EQ(r.decisions[0].action, Action::kInsert);
  EXPECT_EQ(r.decisions[1].action, Action::kRemove);
  EXPECT_EQ(r.decisions[1].reason, Reason::kLateConflict);
}

TEST_F(RulePackFilterTest, HreflangIgnoresLinksWithOtherMeanings) {
  const std::string html =
      "<html><head>"
      "<link rel=\"alternate stylesheet\" hreflang=fr href=https://t.test/s>"
      "<link rel=\"alternate canonical\" hreflang=fr href=https://t.test/c>"
      "</head><body></body></html>";
  FilterRun r = Run(MakePack(kHreflangRepair), "https://t.test/x", html);
  EXPECT_NE(r.html.find("stylesheet"), std::string::npos);
  EXPECT_NE(r.html.find("alternate canonical"), std::string::npos);
  EXPECT_EQ(r.decisions.at(0).action, Action::kInsert);
}

TEST_F(RulePackFilterTest, HreflangDebugCommentIsOnePerRule) {
  Options o;
  o.debug = true;
  FilterRun r = Run(MakePack(kHreflangKeep), "https://t.test/x",
                    "<html><head></head><body></body></html>", o);
  size_t comments = 0;
  for (size_t at = r.html.find("<!-- pagespeed-pack"); at != std::string::npos;
       at = r.html.find("<!-- pagespeed-pack", at + 1)) {
    ++comments;
  }
  EXPECT_EQ(comments, 1u) << r.html;
}

TEST_F(RulePackFilterTest, HeadAppliedAndDocumentPlanDroppedCountsAsModified) {
  // The head plan inserts the canonical; the JSON-LD block has nowhere to go
  // (no <body>, no <html>).
  const std::string html = "<head><title>T</title></head>";
  FilterRun r = Run(MakePack(absl::StrCat(kCanonicalKeep, ",", JsonRule("keep"))),
                    "https://t.test/x", html);
  EXPECT_TRUE(r.modified);
  EXPECT_NE(r.html.find("canonical"), std::string::npos);
  EXPECT_EQ(r.html.find("ld+json"), std::string::npos);
  EXPECT_EQ(r.skip, SkipReason::kNotRewritable);
  ASSERT_EQ(r.decisions.size(), 2u);
  EXPECT_EQ(r.decisions[0].kind->name(), "canonical");
  EXPECT_EQ(r.decisions[0].action, Action::kInsert);
  EXPECT_EQ(r.decisions[1].kind->name(), "jsonld");
  EXPECT_EQ(r.decisions[1].action, Action::kNone);
  EXPECT_EQ(r.decisions[1].reason, Reason::kNotRewritable);
  EXPECT_EQ(host_->count(Counter::kPagesModified), 1);
  EXPECT_EQ(host_->count(Counter::kDecisionsApplied), 1);  // the canonical
  EXPECT_EQ(host_->count(Counter::kPagesSkippedNotRewritable), 0);
  EXPECT_EQ(host_->count(Counter::kPagesSkippedNoHead), 0);
  EXPECT_EQ(host_->count(Counter::kPagesSkippedMalformedHead), 0);
  EXPECT_EQ(host_->count(Counter::kPagesSkippedSizeLimit), 0);
  // Nothing applied: the skip is counted.
  FilterRun none = Run(MakePack(JsonRule("keep")), "https://t.test/x", html);
  EXPECT_FALSE(none.modified);
  EXPECT_EQ(host_->count(Counter::kPagesSkippedNotRewritable), 1);
  EXPECT_EQ(host_->count(Counter::kPagesModified), 0);
}

TEST_F(RulePackFilterTest, HreflangRemovedListIsUtf8WhateverThePageHolds) {
  FilterRun r = Run(MakePack(kHreflangRepair), "https://t.test/x",
                    "<html><head><link rel=alternate hreflang=\"d\xE9\" "
                    "href=\"https://t.test/caf\xE9\"></head><body></body>"
                    "</html>");
  ASSERT_EQ(r.decisions.size(), 1u);
  // (The attribute decoder already drops such bytes; the planner test
  // covers the replacement.)
  EXPECT_FALSE(r.decisions[0].removed.empty());
  EXPECT_EQ(ReplaceInvalidUtf8(r.decisions[0].removed),
            r.decisions[0].removed);
}

TEST_F(RulePackFilterTest, HreflangAndJsonLdContract) {
  TestPackHost host;
  net_instaweb::NullMessageHandler handler;
  net_instaweb::HtmlParse parser(&handler);
  // hreflang adds alternate links but never rewrites a resource URL.
  for (const char* rule : {kHreflangKeep, kHreflangRepair}) {
    RulePackFilter enforcing(&parser, MakePack(rule), Mode::kEnforce, &host);
    EXPECT_FALSE(enforcing.CanModifyUrls()) << rule;
    EXPECT_EQ(enforcing.GetScriptUsage(),
              net_instaweb::HtmlFilter::kNeverInjectsScripts);
    RulePackFilter reporting(&parser, MakePack(rule), Mode::kReport, &host);
    EXPECT_FALSE(reporting.CanModifyUrls()) << rule;
  }
  // JSON-LD adds a script element and no URL attribute.
  for (const char* rung : {"keep", "repair", "replace"}) {
    RulePackFilter jsonld(&parser, MakePack(JsonRule(rung)), Mode::kEnforce,
                          &host);
    EXPECT_FALSE(jsonld.CanModifyUrls()) << rung;
    EXPECT_EQ(jsonld.GetScriptUsage(),
              net_instaweb::HtmlFilter::kMayInjectScripts)
        << rung;
    RulePackFilter reporting(&parser, MakePack(JsonRule(rung)), Mode::kReport,
                             &host);
    EXPECT_EQ(reporting.GetScriptUsage(),
              net_instaweb::HtmlFilter::kNeverInjectsScripts)
        << rung;
  }
}


// The Link: rel=canonical response header (link_header: match)
// ---------------------------------------------------------------------------

std::string LinkRule(std::string_view on_present, std::string_view link) {
  return absl::StrCat(
      R"({"id":"c","kind":"canonical","enforce":true,"on_present":")",
      on_present, R"(","link_header":")", link,
      R"(","value":{"template":"https://t.test{path}"}})");
}

using HeaderLines = std::vector<std::pair<std::string, std::string>>;

constexpr char kHead[] =
    "<html><head><title>T</title></head><body></body></html>";

TEST_F(RulePackFilterTest, LinkHeaderIsAddedNextToTheOriginsLinks) {
  Options o;
  o.headers = HeaderLines{{"Content-Type", "text/html"},
                          {"Link", "</a.css>; rel=preload; as=style"}};
  FilterRun r =
      Run(MakePack(LinkRule("keep", "match")), "https://t.test/x", kHead, o);
  EXPECT_TRUE(r.modified);
  EXPECT_TRUE(r.headers_modified);
  EXPECT_EQ(host_->headers().lines(),
            (HeaderLines{{"Content-Type", "text/html"},
                         {"Link", "</a.css>; rel=preload; as=style"},
                         {"Link", "<https://t.test/x>; rel=\"canonical\""}}));
  ASSERT_EQ(r.decisions.size(), 2u);
  EXPECT_FALSE(r.decisions[0].header);
  const PackDecision& h = r.decisions[1];
  EXPECT_TRUE(h.header);
  EXPECT_EQ(h.action, Action::kInsert);
  EXPECT_EQ(h.reason, Reason::kAbsent);
  EXPECT_EQ(h.after, "https://t.test/x");
  EXPECT_EQ(host_->count(Counter::kHeadersSet), 1);
  EXPECT_EQ(host_->count(Counter::kHeadersFlushed), 0);
  EXPECT_EQ(host_->count(Counter::kPagesModified), 1);
  EXPECT_EQ(host_->decisions(), r.decisions);
}

TEST_F(RulePackFilterTest, LinkHeaderIsAddedEvenWhenOnlyTheHeaderChanges) {
  // The page already carries the canonical; the header does not.
  Options o;
  o.headers = HeaderLines{{"Content-Type", "text/html"}};
  FilterRun r = Run(MakePack(LinkRule("keep", "match")), "https://t.test/x",
                    "<html><head><link rel=canonical href=https://t.test/x>"
                    "</head><body></body></html>",
                    o);
  EXPECT_FALSE(r.modified);
  EXPECT_TRUE(r.headers_modified);
  EXPECT_EQ(host_->headers().lines().size(), 2u);
  EXPECT_EQ(host_->count(Counter::kPagesModified), 1);
}

TEST_F(RulePackFilterTest, LinkHeaderIsOffByDefault) {
  Options o;
  o.headers = HeaderLines{{"Link", "<https://t.test/stale>; rel=canonical"}};
  FilterRun r =
      Run(MakePack(LinkRule("repair", "off")), "https://t.test/x", kHead, o);
  EXPECT_FALSE(r.headers_modified);
  EXPECT_EQ(host_->headers().lines(), *o.headers);
  for (const PackDecision& d : r.decisions) EXPECT_FALSE(d.header);
  r = Run(MakePack(kCanonicalRepair), "https://t.test/x", kHead, o);
  for (const PackDecision& d : r.decisions) EXPECT_FALSE(d.header);
}

TEST_F(RulePackFilterTest, LinkHeaderConflictIsRepairedAndOthersSurvive) {
  Options o;
  o.headers =
      HeaderLines{{"Link",
                   "</a.css>; rel=preload; as=style, <https://t.test/stale>; "
                   "rel=canonical, <https://cdn.test>; rel=preconnect"}};
  FilterRun r =
      Run(MakePack(LinkRule("repair", "match")), "https://t.test/x", kHead, o);
  EXPECT_EQ(host_->headers().lines(),
            (HeaderLines{{"Link",
                          "</a.css>; rel=preload; as=style, "
                          "<https://cdn.test>; rel=preconnect"},
                         {"Link", "<https://t.test/x>; rel=\"canonical\""}}));
  const PackDecision& h = r.decisions.back();
  EXPECT_TRUE(h.header);
  EXPECT_EQ(h.action, Action::kReplace);
  EXPECT_EQ(h.reason, Reason::kHeaderConflict);
  EXPECT_EQ(h.defect, "canonical-header-conflict");
  EXPECT_EQ(host_->count(Counter::kHeadersSet), 1);
}

TEST_F(RulePackFilterTest, LinkHeaderConflictUnderKeepIsOnlyReported) {
  Options o;
  o.headers = HeaderLines{{"Link", "<https://t.test/stale>; rel=canonical"}};
  FilterRun r =
      Run(MakePack(LinkRule("keep", "match")), "https://t.test/x", kHead, o);
  EXPECT_FALSE(r.headers_modified);
  EXPECT_EQ(host_->headers().lines(), *o.headers);
  const PackDecision& h = r.decisions.back();
  EXPECT_TRUE(h.header);
  EXPECT_EQ(h.action, Action::kNone);
  EXPECT_EQ(h.reason, Reason::kHeaderConflict);
  EXPECT_EQ(host_->count(Counter::kHeadersSet), 0);
  // (The report of a conflict is not a change: nothing applied.)
  EXPECT_EQ(host_->count(Counter::kDecisionsApplied), 1);
}

TEST_F(RulePackFilterTest, LinkHeaderEqualIsLeftAlone) {
  Options o;
  o.headers = HeaderLines{{"Link", "</x>; rel=canonical"}};
  FilterRun r =
      Run(MakePack(LinkRule("replace", "match")), "https://t.test/x", kHead, o);
  EXPECT_FALSE(r.headers_modified);
  EXPECT_EQ(host_->headers().lines(), *o.headers);
  EXPECT_EQ(r.decisions.back().reason, Reason::kEqual);
}

TEST_F(RulePackFilterTest, LinkHeaderFollowsThePageCanonicalTheRuleKeeps) {
  // repair leaves the page's own usable canonical; the header must agree
  // with it, not with the rule's value.
  Options o;
  o.headers = HeaderLines{{"Link", "<https://t.test/stale>; rel=canonical"}};
  FilterRun r = Run(MakePack(LinkRule("repair", "match")), "https://t.test/x",
                    "<html><head><link rel=canonical href=https://t.test/mine>"
                    "</head><body></body></html>",
                    o);
  EXPECT_EQ(
      host_->headers().lines(),
      (HeaderLines{{"Link", "<https://t.test/mine>; rel=\"canonical\""}}));
  // ... and with nothing in the header it adds nothing: the rule's value is
  // not the document's canonical.
  o.headers = HeaderLines{{"Content-Type", "text/html"}};
  r = Run(MakePack(LinkRule("repair", "match")), "https://t.test/x",
          "<html><head><link rel=canonical href=https://t.test/mine>"
          "</head><body></body></html>",
          o);
  EXPECT_EQ(host_->headers().lines(), *o.headers);
  for (const PackDecision& d : r.decisions) EXPECT_FALSE(d.header);
}

TEST_F(RulePackFilterTest, ReportModeNeverChangesTheHeaders) {
  Options o;
  o.mode = Mode::kReport;
  o.headers = HeaderLines{{"Link", "<https://t.test/stale>; rel=canonical"}};
  FilterRun r =
      Run(MakePack(LinkRule("repair", "match")), "https://t.test/x", kHead, o);
  EXPECT_FALSE(r.headers_modified);
  EXPECT_EQ(host_->headers().lines(), *o.headers);
  const PackDecision& h = r.decisions.back();
  EXPECT_TRUE(h.header);
  EXPECT_EQ(h.mode, Mode::kReport);
  EXPECT_EQ(h.action, Action::kReplace);  // what it would do
  EXPECT_EQ(host_->count(Counter::kHeadersSet), 0);
  EXPECT_EQ(host_->count(Counter::kDecisionsWouldApply), 2);
  // A rule that is only a report rule behaves the same in an enforcing run.
  o.mode = Mode::kEnforce;
  r = Run(MakePack(R"({"id":"c","kind":"canonical","on_present":"repair",)"
                   R"("link_header":"match",)"
                   R"("value":{"template":"https://t.test{path}"}})"),
          "https://t.test/x", kHead, o);
  EXPECT_FALSE(r.headers_modified);
  EXPECT_EQ(host_->headers().lines(), *o.headers);
}

TEST_F(RulePackFilterTest, LinkHeaderIsSkippedOnceTheHeadersWentOut) {
  Options o;
  o.headers = HeaderLines{{"Link", "</a.css>; rel=preload"}};
  o.chunks = {"<html><head><title>T</title>", "</head><body></body></html>"};
  FilterRun r =
      Run(MakePack(LinkRule("repair", "match")), "https://t.test/x", "", o);
  // The canonical is still inserted; only the header is skipped.
  EXPECT_TRUE(r.modified);
  EXPECT_FALSE(r.headers_modified);
  EXPECT_EQ(host_->headers().lines(), *o.headers);
  ASSERT_EQ(r.decisions.size(), 2u);
  EXPECT_EQ(r.decisions[0].action, Action::kInsert);
  const PackDecision& h = r.decisions[1];
  EXPECT_TRUE(h.header);
  EXPECT_EQ(h.action, Action::kNone);
  EXPECT_EQ(h.reason, Reason::kHeadersFlushed);
  EXPECT_TRUE(h.after.empty());
  EXPECT_EQ(host_->count(Counter::kHeadersFlushed), 1);
  EXPECT_EQ(host_->count(Counter::kHeadersSet), 0);
}

TEST_F(RulePackFilterTest, LinkHeaderDecidedBeforeTheFirstFlushStillApplies) {
  // The flush comes after the head closed: the headers are still writable.
  Options o;
  o.headers = HeaderLines{{"Content-Type", "text/html"}};
  o.chunks = {"<html><head><title>T</title></head>", "<body></body></html>"};
  FilterRun r =
      Run(MakePack(LinkRule("keep", "match")), "https://t.test/x", "", o);
  EXPECT_TRUE(r.headers_modified);
  EXPECT_EQ(host_->count(Counter::kHeadersFlushed), 0);
}

TEST_F(RulePackFilterTest, AHostWithoutHeadersHasNothingToMatch) {
  FilterRun r =
      Run(MakePack(LinkRule("repair", "match")), "https://t.test/x", kHead);
  EXPECT_TRUE(r.modified);
  EXPECT_FALSE(r.headers_modified);
  ASSERT_EQ(r.decisions.size(), 1u);
  EXPECT_FALSE(r.decisions[0].header);
  // A flush changes nothing for it: no headers, no header decision.
  Options o;
  o.chunks = {"<html><head><title>T</title>", "</head><body></body></html>"};
  r = Run(MakePack(LinkRule("repair", "match")), "https://t.test/x", "", o);
  ASSERT_EQ(r.decisions.size(), 1u);
  EXPECT_FALSE(r.decisions[0].header);
  EXPECT_EQ(host_->count(Counter::kHeadersFlushed), 0);
}

TEST_F(RulePackFilterTest, NoHeaderActionWhenTheHeadPlanIsDropped) {
  // The canonical to replace was sent in the first window, so the head plan
  // is dropped: the document keeps what it has, and so does the header.
  Options o;
  o.headers = HeaderLines{{"Link", "<https://t.test/stale>; rel=canonical"}};
  o.chunks = {"<html><head><link rel=canonical href=https://x.test/>",
              "</head><body></body></html>"};
  FilterRun r =
      Run(MakePack(LinkRule("replace", "match")), "https://t.test/x", "", o);
  EXPECT_EQ(r.skip, SkipReason::kNotRewritable);
  EXPECT_FALSE(r.headers_modified);
  ASSERT_EQ(r.decisions.size(), 1u);
  EXPECT_EQ(r.decisions[0].reason, Reason::kNotRewritable);
  EXPECT_EQ(host_->headers().lines(), *o.headers);
}

TEST_F(RulePackFilterTest, LinkHeaderDecisionsFollowTheirRuleInOrder) {
  Options o;
  o.headers = HeaderLines{{"Content-Type", "text/html"}};
  auto pack =
      MakePack(absl::StrCat(LinkRule("keep", "match"), ",", kTitleKeep));
  FilterRun r = Run(pack, "https://t.test/x",
                    "<html><head></head><body></body></html>", o);
  ASSERT_EQ(r.decisions.size(), 3u);
  EXPECT_EQ(r.decisions[0].rule_id, "c");
  EXPECT_FALSE(r.decisions[0].header);
  EXPECT_EQ(r.decisions[1].rule_id, "c");
  EXPECT_TRUE(r.decisions[1].header);
  EXPECT_EQ(r.decisions[2].rule_id, "t");
}

}  // namespace
}  // namespace pagespeed::packs
