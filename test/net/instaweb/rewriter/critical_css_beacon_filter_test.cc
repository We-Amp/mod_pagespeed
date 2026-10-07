/*
 * Licensed to the Apache Software Foundation (ASF) under one
 * or more contributor license agreements.  See the NOTICE file
 * distributed with this work for additional information
 * regarding copyright ownership.  The ASF licenses this file
 * to you under the Apache License, Version 2.0 (the
 * "License"); you may not use this file except in compliance
 * with the License.  You may obtain a copy of the License at
 * 
 *   http://www.apache.org/licenses/LICENSE-2.0
 * 
 * Unless required by applicable law or agreed to in writing,
 * software distributed under the License is distributed on an
 * "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
 * KIND, either express or implied.  See the License for the
 * specific language governing permissions and limitations
 * under the License.
 */

#include "net/instaweb/rewriter/public/critical_css_beacon_filter.h"

#include "net/instaweb/http/public/request_context.h"
#include "net/instaweb/rewriter/public/critical_finder_support_util.h"
#include "net/instaweb/rewriter/public/critical_selector_finder.h"
#include "net/instaweb/rewriter/public/css_summarizer_base.h"
#include "net/instaweb/rewriter/public/rewrite_driver.h"
#include "net/instaweb/rewriter/public/rewrite_options.h"
#include "net/instaweb/rewriter/public/server_context.h"
#include "net/instaweb/rewriter/public/static_asset_manager.h"
#include "net/instaweb/util/public/mock_property_page.h"
#include "net/instaweb/util/public/property_cache.h"
#include "pagespeed/kernel/base/escaping.h"
#include "pagespeed/kernel/base/statistics.h"
#include "pagespeed/kernel/base/string.h"
#include "pagespeed/kernel/base/string_util.h"
#include "pagespeed/kernel/http/content_type.h"
#include "pagespeed/kernel/http/semantic_type.h"
#include "test/net/instaweb/rewriter/rewrite_test_base.h"
#include "test/net/instaweb/rewriter/test_rewrite_driver_factory.h"
#include "test/pagespeed/kernel/base/gtest.h"
#include "test/pagespeed/kernel/html/html_parse_test_base.h"
#include "test/pagespeed/kernel/http/user_agent_matcher_test_base.h"

namespace net_instaweb {

namespace {

const char kInlineStyle[] =
    "<style media='not print'>"
    "a{color:red}"
    "a:visited{color:green}"
    "p{color:green}"
    "</style>";
// An external stylesheet with the same rules as the inline block above.
const char kStyleI[] =
    "a{color:red}"
    "a:visited{color:green}"
    "p{color:green}";
const char kLinkI[] = "<link rel=stylesheet href=i.css>";
const char kStyleA[] =
    "div ul:hover>li{color:red}"
    ":hover{color:red}"
    ".sec h1#id{color:green}";
const char kStyleB[] =
    "a{color:green}"
    "@media screen { p:hover{color:red} }"
    "@media print { span{color:green} }"
    "div ul > li{color:green}";

// The styles above produce the following beacon initialization selector lists.
// Inline blocks contribute nothing: the selectors named "Inline" below come
// from i.css, the external stylesheet with the inline block's rules.
const char kSelectorsInline[] = "\"a\",\"p\"";
const char kSelectorsInlineWithUnauthSelectors[] = "\"a\",\"div\",\"p\"";
const char kSelectorsA[] = "\".sec h1#id\",\"div ul > li\"";
const char kSelectorsB[] = "\"a\",\"div ul > li\",\"p\"";
const char kSelectorsInlineAB[] = "\".sec h1#id\",\"a\",\"div ul > li\",\"p\"";

// The following styles do not add selectors to the beacon initialization.
const char kInlinePrint[] =
    "<style media='print'>"
    "span{color:red}"
    "</style>";
const char kStyleCorrupt[] = "span{color:";
const char kStyleEmpty[] = "/* This has no selectors */";
const char kStyleForUnauthCss[] = "div{display:inline}";
const char kUnauthDomainUrl[] = "http://unauthorized.com/d.css";

// Common setup / result generation code for all tests
class CriticalCssBeaconFilterTestBase : public RewriteTestBase {
 public:
  CriticalCssBeaconFilterTestBase() {}
  ~CriticalCssBeaconFilterTestBase() override {}

 protected:
  // Set everything up except for filter configuration.
  void SetUp() override {
    RewriteTestBase::SetUp();
    SetCurrentUserAgent(UserAgentMatcherTestBase::kChrome18UserAgent);
    SetHtmlMimetype();  // Don't wrap scripts in <![CDATA[ ]]>
    factory()->set_use_beacon_results_in_filters(true);
    rewrite_driver()->set_property_page(NewMockPage(kTestDomain));
    // Set up pcache for page.
    const PropertyCache::Cohort* cohort =
        SetupCohort(page_property_cache(), RewriteDriver::kBeaconCohort);
    server_context()->set_beacon_cohort(cohort);
    page_property_cache()->Read(rewrite_driver()->property_page());
    // Set up and register a beacon finder.
    CriticalSelectorFinder* finder = new BeaconCriticalSelectorFinder(
        server_context()->beacon_cohort(), factory()->nonce_generator(),
        statistics());
    server_context()->set_critical_selector_finder(finder);
    // Set up contents of CSS files.
    SetResponseWithDefaultHeaders("i.css", kContentTypeCss, kStyleI, 100);
    SetResponseWithDefaultHeaders("a.css", kContentTypeCss, kStyleA, 100);
    SetResponseWithDefaultHeaders("b.css", kContentTypeCss, kStyleB, 100);
    SetResponseWithDefaultHeaders("corrupt.css", kContentTypeCss, kStyleCorrupt,
                                  100);
    SetResponseWithDefaultHeaders("empty.css", kContentTypeCss, kStyleEmpty,
                                  100);
    SetResponseWithDefaultHeaders(kUnauthDomainUrl, kContentTypeCss,
                                  kStyleForUnauthCss, 100);
  }

  // Return a css_filter optimized url.
  GoogleString UrlOpt(StringPiece url) {
    return Encode("", RewriteOptions::kCssFilterId, "0", url, "css");
  }

  // Return a link tag with a css_filter optimized url.
  GoogleString CssLinkHrefOpt(StringPiece url) {
    return CssLinkHref(UrlOpt(url));
  }

  GoogleString InputHtml(StringPiece head) {
    return StrCat("<head>", head, "</head><body><p>content</p></body>");
  }

  GoogleString BeaconHtml(StringPiece head, StringPiece selectors) {
    GoogleString html =
        StrCat("<head>", head,
               "</head><body><p>content</p>"
               "<script data-pagespeed-no-defer type=\"text/javascript\">",
               server_context()->static_asset_manager()->GetAsset(
                   StaticAssetEnum::CRITICAL_CSS_BEACON_JS, options()),
               "pagespeed.selectors=[", selectors, "];");
    // Mirror the production code, which JS-escapes the beacon URL before
    // splicing it into the single-quoted criticalCssBeaconInit(...) argument.
    GoogleString escaped_beacon_url;
    EscapeToJsStringLiteral(options()->beacon_url().http,
                            false /* add_quotes */, &escaped_beacon_url);
    StrAppend(&html, "pagespeed.criticalCssBeaconInit('", escaped_beacon_url,
              "','", kTestDomain, "','0','", ExpectedNonce(),
              "',pagespeed.selectors);"
              "</script></body>");
    return html;
  }

  // Serves |css| as g.css, parses a page that links it and expects exactly
  // |selectors| as the candidates browsers are asked about.
  void ExpectCandidates(StringPiece css, StringPiece selectors) {
    SetResponseWithDefaultHeaders("g.css", kContentTypeCss, css, 100);
    ParseUrl(kTestDomain, InputHtml(CssLinkHref("g.css")));
    EXPECT_NE(
        GoogleString::npos,
        output_buffer_.find(StrCat("pagespeed.selectors=[", selectors, "];")))
        << output_buffer_;
  }

  // The next view of the same page: a fresh driver state over the stored
  // property data.
  void NextView() {
    rewrite_driver()->Clear();
    rewrite_driver()->set_request_context(
        RequestContext::NewTestRequestContext(factory()->thread_system()));
    SetCurrentUserAgent(UserAgentMatcherTestBase::kChrome18UserAgent);
    SetHtmlMimetype();
    rewrite_driver()->set_property_page(NewMockPage(kTestDomain));
    page_property_cache()->Read(rewrite_driver()->property_page());
  }

  GoogleString SelectorsOnlyHtml(StringPiece head, StringPiece selectors) {
    return StrCat("<head>", head,
                  "</head><body><p>content</p>"
                  "<script data-pagespeed-no-defer type=\"text/javascript\">",
                  CriticalCssBeaconFilter::kInitializePageSpeedJs,
                  "pagespeed.selectors=[", selectors,
                  "];"
                  "</script></body>");
  }

 private:
  CriticalCssBeaconFilterTestBase(const CriticalCssBeaconFilterTestBase&) = delete;
  CriticalCssBeaconFilterTestBase& operator=(const CriticalCssBeaconFilterTestBase&) = delete;
};

// Standard test setup enables filter via RewriteOptions.
class CriticalCssBeaconFilterTest : public CriticalCssBeaconFilterTestBase {
 public:
  CriticalCssBeaconFilterTest() {}
  ~CriticalCssBeaconFilterTest() override {}

 protected:
  void SetUp() override {
    CriticalCssBeaconFilterTestBase::SetUp();
    options()->EnableFilter(RewriteOptions::kPrioritizeCriticalCss);
    rewrite_driver()->AddFilters();
  }

 private:
  CriticalCssBeaconFilterTest(const CriticalCssBeaconFilterTest&) = delete;
  CriticalCssBeaconFilterTest& operator=(const CriticalCssBeaconFilterTest&) = delete;
};

TEST_F(CriticalCssBeaconFilterTest, ExtractFromStylesheet) {
  ValidateExpectedUrl(kTestDomain, InputHtml(kLinkI),
                      BeaconHtml(kLinkI, kSelectorsInline));
}

// prioritize_critical_css leaves inline <style> blocks as they are, so no
// browser needs to be asked about their selectors. Asking would also make
// every response of a page whose inline blocks carry generated class names
// look like a page with new rules.
TEST_F(CriticalCssBeaconFilterTest, SelectorsOfInlineBlocksAreNotCandidates) {
  GoogleString css = StrCat(CssLinkHref("a.css"), kInlineStyle);
  ValidateExpectedUrl(kTestDomain, InputHtml(css),
                      BeaconHtml(css, kSelectorsA));
}

TEST_F(CriticalCssBeaconFilterTest, APageWithOnlyInlineBlocksIsNotInstrumented) {
  ValidateNoChanges("only_inline_blocks", InputHtml(kInlineStyle));
}

TEST_F(CriticalCssBeaconFilterTest, InitCallHasNoSelectionArgumentByDefault) {
  // By default browsers report every selector that matches anything in the
  // page; the init call says nothing about selection.
  ParseUrl(kTestDomain, InputHtml(kLinkI));
  EXPECT_NE(GoogleString::npos, output_buffer_.find("',pagespeed.selectors);"))
      << output_buffer_;
  EXPECT_EQ(GoogleString::npos, output_buffer_.find("pagespeed.selectors,"))
      << output_buffer_;
}

// With CriticalCssAboveTheFoldOnly on, browsers are asked to report only the
// selectors that match in the first screen.
class CriticalCssBeaconAboveTheFoldOnlyTest
    : public CriticalCssBeaconFilterTestBase {
 protected:
  void SetUp() override {
    CriticalCssBeaconFilterTestBase::SetUp();
    options()->set_critical_css_above_the_fold_only(true);
    options()->EnableFilter(RewriteOptions::kPrioritizeCriticalCss);
    rewrite_driver()->AddFilters();
  }
};

TEST_F(CriticalCssBeaconAboveTheFoldOnlyTest,
       InitCallAsksForAboveTheFoldSelection) {
  ParseUrl(kTestDomain, InputHtml(kLinkI));
  EXPECT_NE(GoogleString::npos,
            output_buffer_.find("',pagespeed.selectors,true);"))
      << output_buffer_;
}

TEST_F(CriticalCssBeaconFilterTest, GroupRuleSelectorsBeaconed) {
  // Selectors inside conditional group rules (@supports/@layer/@container)
  // are beacon candidates: the condition cannot be evaluated server-side,
  // the beacon JS tests candidates against the rendered DOM regardless of
  // whether any rule under them applies, and the selector filter keeps
  // survivors wrapped in their group prelude so the browser re-evaluates
  // the condition — a candidate under a false condition can only cost
  // subset bytes, never styling.
  ExpectCandidates("a{color:red}"
                   "@supports (display: grid){section{display:grid}}"
                   "p{color:green}",
                   "\"a\",\"p\",\"section\"");
}

TEST_F(CriticalCssBeaconFilterTest, NestedGroupSelectorsBeaconed) {
  // Selectors from nested group bodies are collected once and deduped with
  // top-level occurrences of the same selector.
  ExpectCandidates(
      "a{color:red}"
      "@layer l{@supports (display: grid){a{color:green}section{display:grid}}}",
      "\"a\",\"section\"");
}

TEST_F(CriticalCssBeaconFilterTest, GroupMediaGatesBeaconing) {
  // The group node's media annotation (from an enclosing top-level @media)
  // gates the whole body BEFORE recursion: "aside" sits under a print-only
  // @media, and the body ruleset's own empty annotation must not let it
  // leak into the candidate set.
  ExpectCandidates("a{color:red}"
                   "p{color:green}"
                   "@media print{@supports (display: grid){aside{color:red}}}",
                   kSelectorsInline);
}

TEST_F(CriticalCssBeaconFilterTest, GroupUnderScreenMediaBeaconed) {
  ExpectCandidates("a{color:red}"
                   "p{color:green}"
                   "@media screen{@supports (display: grid){aside{color:red}}}",
                   "\"a\",\"aside\",\"p\"");
}

TEST_F(CriticalCssBeaconFilterTest, GroupUnderRawMediaQueryBeaconed) {
  // A raw MQ4 media form ("(width >= 768px)") cannot be evaluated
  // server-side; CanMediaAffectScreen treats it as screen-affecting, so the
  // gated group's selectors are conservatively collected.
  ExpectCandidates(
      "a{color:red}"
      "p{color:green}"
      "@media (width >= 768px){@supports (display: grid){aside{color:red}}}",
      "\"a\",\"aside\",\"p\"");
}

TEST_F(CriticalCssBeaconFilterTest, PureGroupRuleArmsBeacon) {
  // A sheet whose every rule lives inside @layer (Tailwind-v4 default
  // output) must still produce candidates: with an empty candidate set
  // PrepareForBeaconInsertion returns kDoNotBeacon and the page would never
  // instrument, permanently disabling prioritize_critical_css for it.
  ExpectCandidates("@layer base{p{color:green}}", "\"p\"");
}

// Regression: the beacon URL is JS-escaped (matching page_url) before it is
// spliced into the single-quoted criticalCssBeaconInit(...) argument, so a
// single quote in the (admin-configured) beacon URL is emitted as \' and
// cannot terminate the JS string literal early. BeaconHtml() applies the same
// escaping, so the exact-match validation fails if the filter emits a raw
// quote.
TEST_F(CriticalCssBeaconFilterTest, BeaconUrlJsEscaped) {
  options()->ClearSignatureForTesting();
  options()->set_beacon_url("http://test.com/beacon'x");
  options()->ComputeSignature();
  ValidateExpectedUrl(kTestDomain, InputHtml(kLinkI),
                      BeaconHtml(kLinkI, kSelectorsInline));
  // The raw, unescaped single-quote form must not appear in the output.
  EXPECT_EQ(GoogleString::npos, output_buffer_.find("beacon'x"));
}

TEST_F(CriticalCssBeaconFilterTest, CspForbidsInlineScript) {
  // The beacon bootstrap is an inline script; under a script-src policy
  // without 'unsafe-inline' the browser blocks it, so no beaconing.
  const char kCsp[] =
      "<meta http-equiv=\"Content-Security-Policy\" "
      "content=\"script-src *;\">";
  ValidateNoChanges("csp_no_inline_script", InputHtml(StrCat(kCsp, kLinkI)));
}

TEST_F(CriticalCssBeaconFilterTest, CspAllowsInlineScript) {
  // With 'unsafe-inline' permitted the filter behaves as usual.
  const char kCsp[] =
      "<meta http-equiv=\"Content-Security-Policy\" "
      "content=\"script-src * 'unsafe-inline';\">";
  GoogleString head = StrCat(kCsp, kLinkI);
  ValidateExpectedUrl(kTestDomain, InputHtml(head),
                      BeaconHtml(head, kSelectorsInline));
}

TEST_F(CriticalCssBeaconFilterTest, DisabledForIE) {
  SetCurrentUserAgent(UserAgentMatcherTestBase::kIe7UserAgent);
  ValidateNoChanges("on_ie", InputHtml(kLinkI));
}

TEST_F(CriticalCssBeaconFilterTest, DisabledForBots) {
  SetCurrentUserAgent(UserAgentMatcherTestBase::kGooglebotUserAgent);
  ValidateNoChanges("on_bot", InputHtml(kLinkI));
}

TEST_F(CriticalCssBeaconFilterTest, ExtractFromUnopt) {
  ValidateExpectedUrl(kTestDomain, InputHtml(CssLinkHref("a.css")),
                      BeaconHtml(CssLinkHref("a.css"), kSelectorsA));
}

TEST_F(CriticalCssBeaconFilterTest, ExtractFromOpt) {
  GoogleString input_html =
      InputHtml(StrCat(CssLinkHref("b.css"), kLinkI));
  GoogleString expected_html =
      BeaconHtml(StrCat(CssLinkHrefOpt("b.css"), kLinkI), kSelectorsB);
  ValidateExpectedUrl(kTestDomain, input_html, expected_html);
}

TEST_F(CriticalCssBeaconFilterTest, DontExtractFromNoScript) {
  GoogleString input_html = InputHtml(StrCat(
      CssLinkHref("a.css"), "<noscript>", CssLinkHref("b.css"), "</noscript>"));
  GoogleString expected_html =
      BeaconHtml(StrCat(CssLinkHref("a.css"), "<noscript>",
                        CssLinkHrefOpt("b.css"), "</noscript>"),
                 kSelectorsA);
  ValidateExpectedUrl(kTestDomain, input_html, expected_html);
}

TEST_F(CriticalCssBeaconFilterTest, DontExtractFromAlternate) {
  GoogleString input_html = InputHtml(StrCat(
      CssLinkHref("a.css"), "<link rel=\"alternate stylesheet\" href=b.css>"));
  GoogleString expected_html = BeaconHtml(
      StrCat(CssLinkHref("a.css"),
             "<link rel=\"alternate stylesheet\" href=", UrlOpt("b.css"), ">"),
      kSelectorsA);
  ValidateExpectedUrl(kTestDomain, input_html, expected_html);
}

TEST_F(CriticalCssBeaconFilterTest, Unauthorized) {
  GoogleString css = StrCat(CssLinkHref(kUnauthDomainUrl), kLinkI);
  ValidateExpectedUrl(kTestDomain, InputHtml(css),
                      BeaconHtml(css, kSelectorsInline));
  EXPECT_EQ(1, statistics()
                   ->GetVariable(
                       CssSummarizerBase::kNumCssUsedForCriticalCssComputation)
                   ->Get());
  EXPECT_EQ(1,
            statistics()
                ->GetVariable(
                    CssSummarizerBase::kNumCssNotUsedForCriticalCssComputation)
                ->Get());
}

TEST_F(CriticalCssBeaconFilterTest, AllowUnauthorized) {
  options()->ClearSignatureForTesting();
  options()->AddInlineUnauthorizedResourceType(semantic_type::kStylesheet);
  options()->ComputeSignature();
  GoogleString css = StrCat(CssLinkHref(kUnauthDomainUrl), kLinkI);
  ValidateExpectedUrl(kTestDomain, InputHtml(css),
                      BeaconHtml(css, kSelectorsInlineWithUnauthSelectors));
  EXPECT_EQ(2, statistics()
                   ->GetVariable(
                       CssSummarizerBase::kNumCssUsedForCriticalCssComputation)
                   ->Get());
  EXPECT_EQ(0,
            statistics()
                ->GetVariable(
                    CssSummarizerBase::kNumCssNotUsedForCriticalCssComputation)
                ->Get());
}

TEST_F(CriticalCssBeaconFilterTest, Missing) {
  SetFetchFailOnUnexpected(false);
  GoogleString css = StrCat(CssLinkHref("404.css"), kLinkI);
  ValidateExpectedUrl(kTestDomain, InputHtml(css),
                      BeaconHtml(css, kSelectorsInline));
}

TEST_F(CriticalCssBeaconFilterTest, Corrupt) {
  GoogleString css = StrCat(CssLinkHref("corrupt.css"), kLinkI);
  ValidateExpectedUrl(kTestDomain, InputHtml(css),
                      BeaconHtml(css, kSelectorsInline));
}

TEST_F(CriticalCssBeaconFilterTest, EmptyCssIgnored) {
  // This failed when SummariesDone called SplitStringPieceToVector()
  // with "omit_empty_strings" set to false. The beacon selector list
  // looked like the following: [,".sec h1#id","a","div ul > li","p"].
  // That caused the beacon JavaScript to take the length of 'undefined'.
  GoogleString input_html = InputHtml(
      StrCat(CssLinkHref("a.css"), kLinkI, CssLinkHref("empty.css")));
  GoogleString expected_html = BeaconHtml(
      StrCat(CssLinkHref("a.css"), kLinkI, CssLinkHrefOpt("empty.css")),
      kSelectorsInlineAB);
  ValidateExpectedUrl(kTestDomain, input_html, expected_html);
}

TEST_F(CriticalCssBeaconFilterTest, EmptyCssDoesNotTriggerBeaconCode) {
  GoogleString input_html = InputHtml(CssLinkHref("empty.css"));
  GoogleString expected_html = InputHtml(CssLinkHrefOpt("empty.css"));
  ValidateExpectedUrl(kTestDomain, input_html, expected_html);
}

TEST_F(CriticalCssBeaconFilterTest, NonScreenMediaInline) {
  ValidateNoChanges("non-screen-inline", InputHtml(kInlinePrint));
}

TEST_F(CriticalCssBeaconFilterTest, NonScreenMediaExternal) {
  ValidateNoChanges(
      "non-screen-external",
      InputHtml("<link rel=stylesheet href='a.css' media='print'>"));
}

TEST_F(CriticalCssBeaconFilterTest, MixOfGoodAndBad) {
  // Make sure we don't see any strange interactions / missed connections.
  SetFetchFailOnUnexpected(false);
  GoogleString input_html = InputHtml(
      StrCat(CssLinkHref("a.css"), CssLinkHref("404.css"), kLinkI,
             CssLinkHref(kUnauthDomainUrl), CssLinkHref("corrupt.css"),
             kInlinePrint, CssLinkHref("b.css")));
  GoogleString expected_html = BeaconHtml(
      StrCat(CssLinkHref("a.css"), CssLinkHref("404.css"), kLinkI,
             CssLinkHref(kUnauthDomainUrl), CssLinkHref("corrupt.css"),
             kInlinePrint, CssLinkHrefOpt("b.css")),
      kSelectorsInlineAB);
  ValidateExpectedUrl(kTestDomain, input_html, expected_html);
}

TEST_F(CriticalCssBeaconFilterTest, EverythingThatParses) {
  GoogleString input_html = InputHtml(
      StrCat(CssLinkHref("a.css"), kLinkI, CssLinkHref("b.css")));
  GoogleString expected_html = BeaconHtml(
      StrCat(CssLinkHref("a.css"), kLinkI, CssLinkHrefOpt("b.css")),
      kSelectorsInlineAB);
  ValidateExpectedUrl(kTestDomain, input_html, expected_html);
}

TEST_F(CriticalCssBeaconFilterTest, FalseBeaconResultsGivesEmptyBeaconUrl) {
  factory()->set_use_beacon_results_in_filters(false);
  GoogleString input_html = InputHtml(
      StrCat(CssLinkHref("a.css"), kLinkI, CssLinkHref("b.css")));
  GoogleString expected_html = SelectorsOnlyHtml(
      StrCat(CssLinkHref("a.css"), kLinkI, CssLinkHrefOpt("b.css")),
      kSelectorsInlineAB);
  ValidateExpectedUrl(kTestDomain, input_html, expected_html);
}

// This class explicitly only includes the beacon filter and its prerequisites;
// this lets us test the presence of beacon results without the critical
// selector filter injecting a lot of stuff in the output.
class CriticalCssBeaconOnlyTest : public CriticalCssBeaconFilterTestBase {
 public:
  CriticalCssBeaconOnlyTest() {}
  ~CriticalCssBeaconOnlyTest() override {}

 protected:
  void SetUp() override {
    CriticalCssBeaconFilterTestBase::SetUp();
    // Need to set up filters that are normally auto-enabled by
    // kPrioritizeCriticalCss: we're switching on CriticalCssBeaconFilter by
    // hand so that we don't turn on CriticalSelectorFilter.
    options()->EnableFilter(RewriteOptions::kRewriteCss);
    options()->EnableFilter(RewriteOptions::kFlattenCssImports);
    options()->EnableFilter(RewriteOptions::kInlineImportToLink);
    CriticalCssBeaconFilter::InitStats(statistics());
    filter_ = new CriticalCssBeaconFilter(rewrite_driver());
    rewrite_driver()->AddFilters();
    rewrite_driver()->AppendOwnedPreRenderFilter(filter_);
  }

 private:
  CriticalCssBeaconFilter* filter_;  // Owned by rewrite_driver()

  CriticalCssBeaconOnlyTest(const CriticalCssBeaconOnlyTest&) = delete;
  CriticalCssBeaconOnlyTest& operator=(const CriticalCssBeaconOnlyTest&) = delete;
};

// Make sure we re-beacon if candidate data changes.
TEST_F(CriticalCssBeaconOnlyTest, ExtantPCache) {
  // Inject pcache entry.
  StringSet selectors;
  selectors.insert("div ul > li");
  selectors.insert("p");
  selectors.insert("span");  // Doesn't occur in our CSS
  CriticalSelectorFinder* finder = server_context()->critical_selector_finder();
  RewriteDriver* driver = rewrite_driver();
  BeaconMetadata metadata =
      finder->PrepareForBeaconInsertion(selectors, driver);
  ASSERT_TRUE(metadata.status == kBeaconWithNonce);
  EXPECT_EQ(ExpectedNonce(), metadata.nonce);
  finder->WriteCriticalSelectorsToPropertyCache(selectors, metadata.nonce,
                                                driver);
  // Force cohort to persist.
  rewrite_driver()->property_page()->WriteCohort(
      server_context()->beacon_cohort());
  // Now do the test.

  GoogleString input_html = InputHtml(
      StrCat(CssLinkHref("a.css"), kLinkI, CssLinkHref("b.css")));
  GoogleString expected_html = BeaconHtml(
      StrCat(CssLinkHref("a.css"), kLinkI, CssLinkHrefOpt("b.css")),
      kSelectorsInlineAB);
  ValidateExpectedUrl(kTestDomain, input_html, expected_html);
}

// Inline <style> blocks can differ from one response to the next (generated
// class names). That must not look like a page whose rules changed: a report
// requested by an earlier response still counts, and the page becomes
// current.
TEST_F(CriticalCssBeaconOnlyTest, ChangingInlineBlocksDoNotMakeAReportStale) {
  CriticalSelectorFinder* finder = server_context()->critical_selector_finder();
  ParseUrl(kTestDomain,
           InputHtml(StrCat(CssLinkHref("a.css"),
                            "<style>.generated-1{color:red}</style>")));
  ASSERT_NE(GoogleString::npos, output_buffer_.find("criticalCssBeaconInit"))
      << output_buffer_;
  GoogleString nonce = ExpectedNonce();

  // The next response: same stylesheet, another inline block.
  NextView();
  ParseUrl(kTestDomain,
           InputHtml(StrCat(CssLinkHref("a.css"),
                            "<style>.generated-2{color:red}</style>")));

  // The browser that got the first response reports.
  NextView();
  StringSet reported;
  reported.insert("div ul > li");
  finder->WriteCriticalSelectorsToPropertyCache(reported, nonce,
                                                rewrite_driver());
  rewrite_driver()->property_page()->WriteCohort(
      server_context()->beacon_cohort());

  NextView();
  EXPECT_TRUE(finder->HasCurrentBeaconData(rewrite_driver()));
  EXPECT_STREQ("div ul > li",
               JoinCollection(finder->GetCriticalSelectors(rewrite_driver()),
                              ","));
}

class CriticalCssBeaconWithCombinerFilterTest
    : public CriticalCssBeaconFilterTest {
 public:
  void SetUp() override {
    options()->EnableFilter(RewriteOptions::kCombineCss);
    CriticalCssBeaconFilterTest::SetUp();
  }
};

TEST_F(CriticalCssBeaconWithCombinerFilterTest, Interaction) {
  // Make sure that beacon insertion interacts with combine CSS properly.
  GoogleString input_html =
      InputHtml(StrCat(CssLinkHref("a.css"), CssLinkHref("b.css")));
  GoogleString combined_url = Encode("", RewriteOptions::kCssCombinerId, "0",
                                     MultiUrl("a.css", "b.css"), "css");
  // css_filter applies after css_combine and adds to the url encoding.
  GoogleString expected_url = UrlOpt(combined_url);
  GoogleString expected_html =
      BeaconHtml(CssLinkHref(expected_url), kSelectorsInlineAB);
  ValidateExpectedUrl(kTestDomain, input_html, expected_html);
}

}  // namespace

}  // namespace net_instaweb
