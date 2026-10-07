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

#include "net/instaweb/rewriter/public/critical_selector_filter.h"

#include "net/instaweb/http/public/logging_proto_impl.h"
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
#include "pagespeed/kernel/base/charset_util.h"
#include "pagespeed/kernel/base/hasher.h"
#include "pagespeed/kernel/base/statistics.h"
#include "pagespeed/kernel/base/string.h"
#include "pagespeed/kernel/base/string_util.h"
#include "pagespeed/kernel/base/timer.h"
#include "pagespeed/kernel/http/content_type.h"
#include "pagespeed/kernel/http/semantic_type.h"
#include "pagespeed/opt/logging/enums.pb.h"
#include "pagespeed/opt/logging/log_record.h"
#include "test/net/instaweb/rewriter/rewrite_test_base.h"
#include "test/net/instaweb/rewriter/test_rewrite_driver_factory.h"
#include "test/pagespeed/kernel/base/gtest.h"
#include "test/pagespeed/kernel/base/mock_timer.h"
#include "test/pagespeed/kernel/http/user_agent_matcher_test_base.h"

namespace net_instaweb {

namespace {

const char kRequestUrl[] = "http://www.example.com/";

class CriticalSelectorFilterTest : public RewriteTestBase {
 protected:
  virtual void SetUpBeforeSelectorsFilter() { rewrite_driver()->AddFilters(); }

  virtual void SetUpAfterSelectorsFilter() {
    server_context()->ComputeSignature(options());
  }

  void SetUp() override {
    RewriteTestBase::SetUp();

    // Enable critical selector filter alone so that
    // testing isn't disrupted by beacon injection.
    SetUpBeforeSelectorsFilter();
    filter_ = new CriticalSelectorFilter(rewrite_driver());
    rewrite_driver()->AppendOwnedPreRenderFilter(filter_);
    SetUpAfterSelectorsFilter();
    // Setup pcache.
    pcache_ = rewrite_driver()->server_context()->page_property_cache();
    const PropertyCache::Cohort* beacon_cohort =
        SetupCohort(pcache_, RewriteDriver::kBeaconCohort);
    const PropertyCache::Cohort* dom_cohort =
        SetupCohort(pcache_, RewriteDriver::kDomCohort);
    server_context()->set_dom_cohort(dom_cohort);
    server_context()->set_beacon_cohort(beacon_cohort);
    server_context()->set_critical_selector_finder(
        new BeaconCriticalSelectorFinder(server_context()->beacon_cohort(),
                                         factory()->nonce_generator(),
                                         statistics()));
    ResetDriver();
    // Set up initial candidates for critical selector beacon
    candidates_.insert("div");
    candidates_.insert("*");
    candidates_.insert("span");
    // Selectors the test stylesheets use and browsers are asked about, but
    // never report as critical.
    candidates_.insert("p");
    candidates_.insert("i");
    candidates_.insert("section");
    candidates_.insert("random");
    candidates_.insert(".not-critical");
    // Write out some initial critical selectors for us to work with.
    StringSet selectors;
    selectors.insert("div");
    selectors.insert("*");
    WriteCriticalSelectorsToPropertyCache(selectors);

    // Some weird but valid CSS.
    SetResponseWithDefaultHeaders("a.css", kContentTypeCss,
                                  "div,span,*::first-letter { display: block; }"
                                  "p { display: inline; }",
                                  100);
    SetResponseWithDefaultHeaders("b.css", kContentTypeCss,
                                  "@media screen,print { * { margin: 0px; } }",
                                  100);
    SetResponseWithDefaultHeaders("http://unauthorized.com/unauth.css",
                                  kContentTypeCss,
                                  "div { background-color: blue }"
                                  "random { color: white }",
                                  100);
  }

  void ResetDriver() {
    rewrite_driver()->Clear();
    rewrite_driver()->set_request_context(
        RequestContext::NewTestRequestContext(factory()->thread_system()));
    page_ = NewMockPage(kRequestUrl);
    rewrite_driver()->set_property_page(page_);
    pcache_->Read(page_);
    SetHtmlMimetype();  // Don't wrap scripts in <![CDATA[ ]]>
  }

  void WriteCriticalSelectorsToPropertyCache(const StringSet& selectors) {
    factory()->mock_timer()->AdvanceMs(
        options()->beacon_reinstrument_time_sec() * Timer::kSecondMs);
    last_beacon_metadata_ =
        server_context()->critical_selector_finder()->PrepareForBeaconInsertion(
            candidates_, rewrite_driver());
    ASSERT_EQ(kBeaconWithNonce, last_beacon_metadata_.status);
    ResetDriver();
    server_context()
        ->critical_selector_finder()
        ->WriteCriticalSelectorsToPropertyCache(
            selectors, last_beacon_metadata_.nonce, rewrite_driver());
    page_->WriteCohort(server_context()->beacon_cohort());
  }

  // The script that turns deferred stylesheets on. It is written once per
  // document, just ahead of the first deferred stylesheet.
  GoogleString JsLoader() {
    return StrCat(
        "<script data-pagespeed-no-defer type=\"text/javascript\">",
        rewrite_driver()->server_context()->static_asset_manager()->GetAsset(
            StaticAssetEnum::CRITICAL_CSS_LOADER_JS,
            rewrite_driver()->options()),
        "pagespeed.CriticalCssLoader.Run();</script>");
  }

  // The preload that stands where a deferred stylesheet <link> stood: the
  // link's own attributes, minus id (the inline block keeps it), fetched as
  // a style and marked for the loader.
  GoogleString PreloadFor(StringPiece link) {
    GoogleString out(link.data(), link.size());
    GlobalReplaceSubstring("rel=stylesheet", "rel=preload", &out);
    GlobalReplaceSubstring("rel=\"stylesheet\"", "rel=\"preload\"", &out);
    GlobalReplaceSubstring(" id=\"main\"", "", &out);
    GlobalReplaceSubstring(">", " as=\"style\" data-pagespeed-deferred-css>",
                           &out);
    return out;
  }

  // Everything that replaces a deferred stylesheet <link>, after its inline
  // critical rules: the preload, and the link itself for browsers without
  // scripts.
  GoogleString Deferred(StringPiece link) {
    return StrCat(PreloadFor(link), "<noscript>", link, "</noscript>");
  }

  // The first (or only) deferred stylesheet of a document: the loader script,
  // then the stylesheet.
  GoogleString LoadRestOfCss(StringPiece link) {
    return StrCat(JsLoader(), Deferred(link));
  }

  GoogleString CssLinkHrefMedia(StringPiece url, StringPiece media) {
    return StrCat("<link rel=stylesheet href=", url, " media=\"", media, "\">");
  }

  bool AddHtmlTags() const override { return false; }

  void ValidateRewriterLogging(RewriterHtmlApplication::Status html_status) {
    rewrite_driver()->log_record()->WriteLog();

    const LoggingInfo& logging_info =
        *rewrite_driver()->log_record()->logging_info();
    ASSERT_EQ(1, logging_info.rewriter_stats_size());
    const RewriterStats& rewriter_stats = logging_info.rewriter_stats(0);
    EXPECT_EQ("pr", rewriter_stats.id());
    EXPECT_EQ(html_status, rewriter_stats.html_status());
  }

  CriticalSelectorFilter* filter_;  // owned by the driver;
  PropertyCache* pcache_;
  PropertyPage* page_;
  StringSet candidates_;
  BeaconMetadata last_beacon_metadata_;
};

TEST_F(CriticalSelectorFilterTest, BasicOperation) {
  // An inline block stays where it is, whole. Each stylesheet link becomes
  // its critical rules, a preload and a noscript copy, all in the link's own
  // place; the loader script is written once, ahead of the first preload.
  GoogleString inline_css =
      "<style>*,p {display: none; } span {display: inline; }</style>";
  GoogleString html =
      StrCat("<head>", inline_css, CssLinkHref("a.css"), CssLinkHref("b.css"),
             "</head>"
             "<body><div>Stuff</div></body>");

  ValidateExpected(
      "basic", html,
      StrCat("<head>", inline_css,
             "<style>div,*::first-letter{display:block}</style>",  // a.css
             JsLoader(), Deferred(CssLinkHref("a.css")),
             "<style>@media screen{*{margin:0}}</style>",  // b.css
             Deferred(CssLinkHref("b.css")),
             "</head>"
             "<body><div>Stuff</div></body>"));
  ValidateRewriterLogging(RewriterHtmlApplication::ACTIVE);
}

// Regression test for a "</style>" breakout on the critical-CSS inline path.
// The '*' rule is critical, so it is minified and written into an inline
// <style>. Its content string uses hex escapes that decode to
// "</style><script>...". Whatever the filter does with this stylesheet, no
// literal "</style><script>" and no bare <script> made from the decoded
// string may appear in the page.
TEST_F(CriticalSelectorFilterTest,
       DoesNotBreakOutOfInlineStyleViaEscapedClosingTag) {
  SetResponseWithDefaultHeaders(
      "c.css", kContentTypeCss,
      "*{content:\"\\3C/style\\3E\\3Cscript\\3Ealert(1)\\3C/script\\3E\"}",
      100);
  Parse("escaped_style_breakout", StrCat("<head>", CssLinkHref("c.css"),
                                         "</head>"
                                         "<body><div>Stuff</div></body>"));
  // The loader script legitimately follows the inline block, so look for the
  // script the decoded string would have made.
  EXPECT_EQ(GoogleString::npos, output_buffer_.find("</style><script>alert"))
      << output_buffer_;
  EXPECT_EQ(GoogleString::npos, output_buffer_.find("<script>alert"))
      << output_buffer_;
  // The string reaches the page with its escapes intact.
  EXPECT_NE(GoogleString::npos, output_buffer_.find("\\3C/style\\3E"))
      << output_buffer_;
}

// A critical subset that carries a literal closing style tag would end the
// inline <style> early. The filter must leave such a stylesheet alone: its
// link stays as it is. (A region the parser keeps verbatim is the way such
// bytes reach the subset.)
TEST_F(CriticalSelectorFilterTest,
       AClosingStyleTagInTheSubsetKeepsTheStylesheetBlocking) {
  SetResponseWithDefaultHeaders(
      "c.css", kContentTypeCss,
      "@huh { content: \"</style><script>alert(1)</script>\"; }"
      "div { color: red; }",
      100);
  ValidateNoChanges("literal_style_breakout",
                    StrCat("<head>", CssLinkHref("c.css"),
                           "</head>"
                           "<body><div>Stuff</div></body>"));
}

TEST_F(CriticalSelectorFilterTest, CspForbidsInlineStyle) {
  // A style-src policy without 'unsafe-inline' would make the browser block
  // the inline <style> blocks this filter swaps in for <link> tags, so the
  // filter must leave the page alone entirely.
  const char kCsp[] =
      "<meta http-equiv=\"Content-Security-Policy\" content=\"style-src *;\">";
  GoogleString html =
      StrCat("<head>", kCsp, CssLinkHref("a.css"), CssLinkHref("b.css"),
             "</head>"
             "<body><div>Stuff</div></body>");
  ValidateNoChanges("csp_no_inline", html);
}

TEST_F(CriticalSelectorFilterTest, CspAllowsInlineStyle) {
  // With 'unsafe-inline' permitted, the filter behaves as usual.
  const char kCsp[] =
      "<meta http-equiv=\"Content-Security-Policy\" "
      "content=\"style-src * 'unsafe-inline';\">";
  GoogleString html =
      StrCat("<head>", kCsp, CssLinkHref("a.css"), CssLinkHref("b.css"),
             "</head>"
             "<body><div>Stuff</div></body>");
  ValidateExpected(
      "csp_unsafe_inline", html,
      StrCat("<head>", kCsp,
             "<style>div,*::first-letter{display:block}</style>",  // a.css
             JsLoader(), Deferred(CssLinkHref("a.css")),
             "<style>@media screen{*{margin:0}}</style>",  // b.css
             Deferred(CssLinkHref("b.css")),
             "</head>"
             "<body><div>Stuff</div></body>"));
}

// Regression test for bug M4: the filter inlines the critical subset, moves
// the non-critical CSS into <noscript> blocks, and re-adds it with an injected
// inline bootstrap <script>. Under a policy that permits inline style but
// forbids inline script (style-src ... 'unsafe-inline'; script-src * without
// 'unsafe-inline'), the browser would block that loader, stranding all the
// non-critical CSS. The filter must therefore leave the page untouched: no
// inline <style> swap, no <noscript> blocks, and no blocked loader script, so
// every stylesheet still loads normally via its original <link>.
TEST_F(CriticalSelectorFilterTest, CspForbidsInlineScriptKeepsCssIntact) {
  const char kCsp[] =
      "<meta http-equiv=\"Content-Security-Policy\" "
      "content=\"style-src * 'unsafe-inline'; script-src *;\">";
  GoogleString html =
      StrCat("<head>", kCsp, CssLinkHref("a.css"), CssLinkHref("b.css"),
             "</head>"
             "<body><div>Stuff</div></body>");
  ValidateNoChanges("csp_no_inline_script", html);
}

TEST_F(CriticalSelectorFilterTest, UnauthorizedCss) {
  GoogleString inline_css =
      "<style>*,p {display: none; } span {display: inline; }</style>";
  GoogleString unauth = CssLinkHref("http://unauthorized.com/unauth.css");
  GoogleString html =
      StrCat("<head>", inline_css, unauth, CssLinkHref("a.css"),
             CssLinkHref("b.css"),
             "</head>"
             "<body><div>Stuff</div></body>");

  // The stylesheet the filter may not read keeps its blocking link.
  ValidateExpected(
      "basic", html,
      StrCat("<head>", inline_css, unauth,
             "<style>div,*::first-letter{display:block}</style>",  // a.css
             JsLoader(), Deferred(CssLinkHref("a.css")),
             "<style>@media screen{*{margin:0}}</style>",  // b.css
             Deferred(CssLinkHref("b.css")),
             "</head>"
             "<body><div>Stuff</div></body>"));
  ValidateRewriterLogging(RewriterHtmlApplication::ACTIVE);
  EXPECT_EQ(3, statistics()
                   ->GetVariable(
                       CssSummarizerBase::kNumCssUsedForCriticalCssComputation)
                   ->Get());
  EXPECT_EQ(1,
            statistics()
                ->GetVariable(
                    CssSummarizerBase::kNumCssNotUsedForCriticalCssComputation)
                ->Get());
}

TEST_F(CriticalSelectorFilterTest, AllowUnauthorizedCss) {
  options()->ClearSignatureForTesting();
  options()->AddInlineUnauthorizedResourceType(semantic_type::kStylesheet);
  options()->ComputeSignature();
  GoogleString inline_css =
      "<style>*,p {display: none; } span {display: inline; }</style>";
  GoogleString unauth = CssLinkHref("http://unauthorized.com/unauth.css");
  GoogleString html =
      StrCat("<head>", inline_css, unauth, CssLinkHref("a.css"),
             CssLinkHref("b.css"),
             "</head>"
             "<body><div>Stuff</div></body>");

  ValidateExpected(
      "basic", html,
      StrCat("<head>", inline_css,
             "<style>div{background-color:#00f}</style>",  // unauth.css
             JsLoader(), Deferred(unauth),
             "<style>div,*::first-letter{display:block}</style>",  // a.css
             Deferred(CssLinkHref("a.css")),
             "<style>@media screen{*{margin:0}}</style>",  // b.css
             Deferred(CssLinkHref("b.css")),
             "</head>"
             "<body><div>Stuff</div></body>"));
  ValidateRewriterLogging(RewriterHtmlApplication::ACTIVE);
  EXPECT_EQ(4, statistics()
                   ->GetVariable(
                       CssSummarizerBase::kNumCssUsedForCriticalCssComputation)
                   ->Get());
  EXPECT_EQ(0,
            statistics()
                ->GetVariable(
                    CssSummarizerBase::kNumCssNotUsedForCriticalCssComputation)
                ->Get());
}

TEST_F(CriticalSelectorFilterTest, StylesInBody) {
  // A stylesheet link in the body is deferred in the body, where it stood.
  GoogleString inline_css =
      "<style>*,p {display: none; } span {display: inline; }</style>";
  GoogleString html =
      StrCat("<head></head><body>", inline_css, CssLinkHref("a.css"),
             "<div>Stuff</div>", CssLinkHref("b.css"), "</body>");

  ValidateExpected(
      "style_in_body", html,
      StrCat("<head></head><body>", inline_css,
             "<style>div,*::first-letter{display:block}</style>",  // a.css
             JsLoader(), Deferred(CssLinkHref("a.css")), "<div>Stuff</div>",
             "<style>@media screen{*{margin:0}}</style>",  // b.css
             Deferred(CssLinkHref("b.css")), "</body>"));
  ValidateRewriterLogging(RewriterHtmlApplication::ACTIVE);
}

TEST_F(CriticalSelectorFilterTest, EmptyBlock) {
  // No empty <style> block is written when none of a stylesheet's rules is
  // critical ('i' is not); the stylesheet is still deferred.
  SetResponseWithDefaultHeaders("c.css", kContentTypeCss,
                                "i { font-style:italic; }", 100);
  GoogleString html = StrCat("<head>", CssLinkHref("c.css"),
                             "</head>"
                             "<body><div>Stuff</div></body>");

  ValidateExpected("empty_block", html,
                   StrCat("<head>", LoadRestOfCss(CssLinkHref("c.css")),
                          "</head>"
                          "<body><div>Stuff</div></body>"));
  ValidateRewriterLogging(RewriterHtmlApplication::ACTIVE);
}

TEST_F(CriticalSelectorFilterTest, DisabledForIE) {
  SetCurrentUserAgent(UserAgentMatcherTestBase::kIe7UserAgent);
  GoogleString css =
      StrCat("<style>*,p {display: none; } span {display: inline; }</style>",
             CssLinkHref("a.css"), CssLinkHref("b.css"));
  GoogleString html = StrCat("<head>", css,
                             "</head>"
                             "<body><div>Stuff</div></body>");
  ValidateNoChanges("on_ie", html);
  ValidateRewriterLogging(RewriterHtmlApplication::USER_AGENT_NOT_SUPPORTED);
}

TEST_F(CriticalSelectorFilterTest, NoScript) {
  // A stylesheet the page itself put inside <noscript> is left alone.
  GoogleString css1 =
      "<style>*,p {display: none; } span {display: inline; }</style>";
  GoogleString css2 = StrCat("<noscript>", CssLinkHref("a.css"), "</noscript>");
  GoogleString css3 = CssLinkHref("b.css");

  GoogleString html = StrCat("<head>", css1, css2, css3,
                             "</head>"
                             "<body><div>Stuff</div></body>");

  ValidateExpected("noscript", html,
                   StrCat("<head>", css1, css2,
                          "<style>@media screen{*{margin:0}}</style>",  // b.css
                          LoadRestOfCss(css3),
                          "</head>"
                          "<body><div>Stuff</div></body>"));
  ValidateRewriterLogging(RewriterHtmlApplication::ACTIVE);
}

TEST_F(CriticalSelectorFilterTest, Alternate) {
  // An alternate stylesheet does not block rendering; it is left alone.
  GoogleString alternate = "<link rel=\"alternate stylesheet\" href=\"a.css\">";
  GoogleString html = StrCat("<head>", alternate, CssLinkHref("b.css"),
                             "</head>"
                             "<body><div>Stuff</div></body>");

  ValidateExpected("alternate", html,
                   StrCat("<head>", alternate,
                          "<style>@media screen{*{margin:0}}</style>",  // b.css
                          LoadRestOfCss(CssLinkHref("b.css")),
                          "</head>"
                          "<body><div>Stuff</div></body>"));
}

TEST_F(CriticalSelectorFilterTest, Media) {
  // The inline block carries the screen part of the link's media; the
  // preload and the noscript copy keep the link's media as written.
  GoogleString inline_css =
      "<style media=screen,print>*,p {display: none; } "
      "span {display: inline; }</style>";
  GoogleString link_a = CssLinkHrefMedia("a.css", "screen");
  GoogleString link_b = CssLinkHrefMedia("b.css", "screen and (color), aural");

  GoogleString html = StrCat("<head>", inline_css, link_a, link_b,
                             "</head>"
                             "<body><div>Stuff</div></body>");

  ValidateExpected(
      "foo", html,
      StrCat("<head>", inline_css,
             "<style media=\"screen\">"
             "div,*::first-letter{display:block}"  // from a.css
             "</style>",
             JsLoader(), Deferred(link_a),
             "<style media=\"screen and (color)\">"
             "@media screen{*{margin:0}}"  // from b.css
             "</style>",
             Deferred(link_b),
             "</head>"
             "<body><div>Stuff</div></body>"));
}

TEST_F(CriticalSelectorFilterTest, NonScreenMedia) {
  // A print-only block is not touched, like every inline block.
  GoogleString inline_css =
      "<style media=print>*,p {display: none; } "
      "span {display: inline; }</style>";
  GoogleString link_a = CssLinkHrefMedia("a.css", "screen");
  GoogleString link_b = CssLinkHrefMedia("b.css", "screen and (color), aural");

  GoogleString html = StrCat("<head>", inline_css, link_a, link_b,
                             "</head>"
                             "<body><div>Stuff</div></body>");

  ValidateExpected(
      "non_screen_media", html,
      StrCat("<head>", inline_css,
             "<style media=\"screen\">"
             "div,*::first-letter{display:block}"  // from a.css
             "</style>",
             JsLoader(), Deferred(link_a),
             "<style media=\"screen and (color)\">"
             "@media screen{*{margin:0}}"  // from b.css
             "</style>",
             Deferred(link_b),
             "</head>"
             "<body><div>Stuff</div></body>"));
}

TEST_F(CriticalSelectorFilterTest, SameCssDifferentSelectors) {
  // We should not reuse results for same CSS when selectors are different.
  SetResponseWithDefaultHeaders("c.css", kContentTypeCss,
                                "div,span { display: inline-block; }", 100);
  GoogleString link = CssLinkHref("c.css");

  GoogleString critical_css_div = "div{display:inline-block}";
  GoogleString critical_css_span = "span{display:inline-block}";
  GoogleString critical_css_div_span = "div,span{display:inline-block}";
  // Check what we compute for a page with div.
  ValidateExpected("with_div", StrCat(link, "<div>Foo</div>"),
                   StrCat("<style>", critical_css_div, "</style>",
                          LoadRestOfCss(link), "<div>Foo</div>"));

  // Update the selector list with span. Because we are storing the last N
  // beacon entries, both div and span should now be in the critical set. We
  // also clear the property cache entry for our result, which is needed because
  // the test harness is not really keying the pcache by the URL like the real
  // system would.
  StringSet selectors;
  selectors.insert("span");
  WriteCriticalSelectorsToPropertyCache(selectors);

  // Note that calling ResetDriver() just resets the state in the
  // driver. Whatever has been written to the property & metadata caches so far
  // will persist. Upon rewriting, the property cache contents will be read and
  // the critical selector info in the driver will be repopulated.
  ResetDriver();
  ValidateExpected("with_div_span", StrCat(link, "<span>Foo</span>"),
                   StrCat("<style>", critical_css_div_span, "</style>",
                          LoadRestOfCss(link), "<span>Foo</span>"));

  // Now send enough beacons to eliminate support for div; only span should be
  // left.
  CriticalSelectorFinder* finder = server_context()->critical_selector_finder();
  for (int i = 0; i < finder->SupportInterval(); ++i) {
    WriteCriticalSelectorsToPropertyCache(selectors);
    // We are sending enough beacons with the same selector set here that we
    // will enter low frequency beaconing mode, so advance time more to ensure
    // rebeaconing actually occurs.
    factory()->mock_timer()->AdvanceMs(
        options()->beacon_reinstrument_time_sec() * Timer::kSecondMs *
        kLowFreqBeaconMult);
  }
  ResetDriver();
  ValidateExpected("with_span", StrCat(link, "<span>Foo</span>"),
                   StrCat("<style>", critical_css_span, "</style>",
                          LoadRestOfCss(link), "<span>Foo</span>"));
}

TEST_F(CriticalSelectorFilterTest, ASelectorNoBrowserHasReportedOnIsKept) {
  // ".fresh" arrived with a changed stylesheet: no browser has been asked
  // about it yet, so nothing says its rule can wait. "p" has been asked
  // about and was never reported; its rule can.
  SetResponseWithDefaultHeaders(
      "c.css", kContentTypeCss,
      "div { color: red; } .fresh { color: blue; } p { color: green; }", 100);
  ValidateExpected("unknown_selector_kept", CssLinkHref("c.css"),
                   StrCat("<style>div{color:red}.fresh{color:#00f}</style>",
                          LoadRestOfCss(CssLinkHref("c.css"))));
}

TEST_F(CriticalSelectorFilterTest,
       StylesheetsStayBlockingUntilABrowserReportsOnTheNewSelectors) {
  GoogleString html = StrCat("<head>", CssLinkHref("a.css"),
                             "</head>"
                             "<body><div>Stuff</div></body>");

  // The page gains a selector; the page view that notices asks browsers
  // about it.
  ResetDriver();
  candidates_.insert(".fresh");
  factory()->mock_timer()->AdvanceMs(
      options()->beacon_reinstrument_time_sec() * Timer::kSecondMs);
  BeaconMetadata asked =
      server_context()->critical_selector_finder()->PrepareForBeaconInsertion(
          candidates_, rewrite_driver());
  ASSERT_EQ(kBeaconWithNonce, asked.status);

  // Until one answers, the stored reports say nothing about ".fresh": the
  // stylesheet keeps its blocking link.
  ResetDriver();
  ValidateNoChanges("awaiting_report", html);

  // A browser that was asked about the new set reports.
  ResetDriver();
  StringSet selectors;
  selectors.insert("div");
  selectors.insert("*");
  server_context()
      ->critical_selector_finder()
      ->WriteCriticalSelectorsToPropertyCache(selectors, asked.nonce,
                                              rewrite_driver());
  page_->WriteCohort(server_context()->beacon_cohort());

  ResetDriver();
  ValidateExpected(
      "report_arrived", html,
      StrCat("<head>", "<style>div,*::first-letter{display:block}</style>",
             LoadRestOfCss(CssLinkHref("a.css")),
             "</head>"
             "<body><div>Stuff</div></body>"));
}

// Pin (passes before this task's implementation): a rule whose selector
// browsers reported as unmatched is left out, and the first report that
// shows it matching puts it into the very next response. This is the path a
// page takes when its markup starts using a rule that already existed.
TEST_F(CriticalSelectorFilterTest,
       TheFirstReportThatASelectorMatchesTakesEffectOnTheNextResponse) {
  GoogleString link = CssLinkHref("a.css");
  // "span" has been asked about and never reported.
  ValidateExpected("before_the_report", link,
                   StrCat("<style>div,*::first-letter{display:block}</style>",
                          LoadRestOfCss(link)));

  // The markup now has a span, and one browser has seen it.
  StringSet selectors;
  selectors.insert("div");
  selectors.insert("*");
  selectors.insert("span");
  WriteCriticalSelectorsToPropertyCache(selectors);

  ResetDriver();
  ValidateExpected(
      "after_the_report", link,
      StrCat("<style>div,span,*::first-letter{display:block}</style>",
             LoadRestOfCss(link)));
}

TEST_F(CriticalSelectorFilterTest, RetainPseudoOnly) {
  // Make sure we handle things like :hover OK.
  GoogleString css = ":hover { border: 2px solid red; }";
  SetResponseWithDefaultHeaders("c.css", kContentTypeCss, css, 100);
  ValidateExpected("hover", CssLinkHref("c.css"),
                   StrCat("<style>:hover{border:2px solid red}</style>",
                          LoadRestOfCss(CssLinkHref("c.css"))));
}

TEST_F(CriticalSelectorFilterTest, RetainUnparseable) {
  // Make sure we keep unparseable fragments around, particularly when
  // the problem is with the selector, as well as with the entire region.
  GoogleString css = "!huh! {background: white; } @huh { display: block; }";
  SetResponseWithDefaultHeaders("c.css", kContentTypeCss, css, 100);
  ValidateExpected(
      "partly_unparseable", CssLinkHref("c.css"),
      StrCat("<style>!huh! {background:#fff}@huh { display: block; }</style>",
             LoadRestOfCss(CssLinkHref("c.css"))));
}

TEST_F(CriticalSelectorFilterTest, CharsetIncompatibleNonAscii) {
  // c.css declares utf-8 via its BOM and its critical subset carries a
  // non-ASCII byte, while the page is iso-8859-1. Inlining would garble the
  // bytes, so the filter must leave this stylesheet alone.
  GoogleString css = StrCat(kUtf8Bom, "div { content: \"\xD2\x90\"; }");
  SetResponseWithDefaultHeaders("c.css", kContentTypeCss, css, 100);
  ValidateNoChanges(
      "charset_mismatch",
      StrCat("<meta charset=\"ISO-8859-1\">", CssLinkHref("c.css")));
}

TEST_F(CriticalSelectorFilterTest, CharsetIncompatibleAsciiOnly) {
  // Same charset mismatch, but the critical subset keeps to the ASCII
  // subset, which is charset-agnostic --- so inlining proceeds.
  GoogleString css = StrCat(kUtf8Bom, "div { color: red; }");
  SetResponseWithDefaultHeaders("c.css", kContentTypeCss, css, 100);
  ValidateExpected(
      "charset_mismatch_ascii",
      StrCat("<meta charset=\"ISO-8859-1\">", CssLinkHref("c.css")),
      StrCat("<meta charset=\"ISO-8859-1\">", "<style>div{color:red}</style>",
             LoadRestOfCss(CssLinkHref("c.css"))));
}

// Regression test for bug M6: c.css declares no charset at all (no
// Content-Type charset, no @charset rule, no BOM, no charset attribute), so
// summary.charset is empty. Its critical subset carries a non-ASCII byte.
// With the charset unknown the filter cannot assume it matches the page, so
// inlining could garble the bytes; it must treat the unknown charset as a
// mismatch and leave the stylesheet alone rather than inline mojibake.
TEST_F(CriticalSelectorFilterTest, CharsetUnknownNonAsciiNotInlined) {
  GoogleString css = "div { content: \"\xD2\x90\"; }";  // no BOM / @charset
  SetResponseWithDefaultHeaders("c.css", kContentTypeCss, css, 100);
  ValidateNoChanges(
      "charset_unknown_non_ascii",
      StrCat("<meta charset=\"ISO-8859-1\">", CssLinkHref("c.css")));
}

TEST_F(CriticalSelectorFilterTest, CopiesLinkAttributes) {
  // id and data-* carry over from the replaced link to the inline style;
  // other attributes (here foo) do not. The preload does not repeat the id.
  GoogleString link =
      "<link rel=\"stylesheet\" href=\"a.css\" id=\"main\""
      " data-x=\"1\" foo=\"bar\">";
  ValidateExpected("copy_attrs", link,
                   StrCat("<style id=\"main\" data-x=\"1\">"
                          "div,*::first-letter{display:block}</style>",
                          LoadRestOfCss(link)));
}

TEST_F(CriticalSelectorFilterTest, ADisabledStylesheetIsLeftAlone) {
  // The page switched this stylesheet off (a theme a script may turn on
  // later). Its rules must not apply, so none of them may be inlined.
  ValidateNoChanges("disabled",
                    "<link rel=stylesheet href=a.css disabled>"
                    "<div>Stuff</div>");
}

TEST_F(CriticalSelectorFilterTest, ATitledStylesheetIsLeftAlone) {
  // A title makes the link part of a named set of stylesheets that the
  // browser, the visitor or a script can switch between. The filter does not
  // take such a set apart.
  ValidateNoChanges("titled",
                    "<link rel=stylesheet href=a.css title=\"Main\">"
                    "<link rel=\"alternate stylesheet\" href=b.css"
                    " title=\"Other\">"
                    "<div>Stuff</div>");
}

TEST_F(CriticalSelectorFilterTest, TheIdStaysOnThePreloadWithoutAnInlineBlock) {
  // None of this stylesheet's rules is critical, so no inline block is
  // written; the id a page script may look up then stays on the link.
  SetResponseWithDefaultHeaders("c.css", kContentTypeCss,
                                "i { font-style:italic; }", 100);
  GoogleString link = "<link rel=stylesheet href=c.css id=theme>";
  ValidateExpected(
      "id_without_inline_block", link,
      StrCat(JsLoader(),
             "<link rel=preload href=c.css id=theme as=\"style\" "
             "data-pagespeed-deferred-css>",
             "<noscript>", link, "</noscript>"));
}

TEST_F(CriticalSelectorFilterTest, ThePreloadHasOneAsAttribute) {
  Parse("one_as", "<link rel=stylesheet href=a.css as=style>");
  EXPECT_NE(GoogleString::npos,
            output_buffer_.find("<link rel=preload href=a.css as=\"style\" "
                                "data-pagespeed-deferred-css>"))
      << output_buffer_;
}

TEST_F(CriticalSelectorFilterTest, PlainImportKeepsTheStylesheetBlocking) {
  // The rules of an imported file are in neither the inline block (imported
  // selectors are never reported on) nor, once the link is deferred, in
  // anything that blocks rendering. A stylesheet that still imports another
  // keeps its blocking link.
  GoogleString css = "@import url(imp.css); div { color: red; }";
  SetResponseWithDefaultHeaders("c.css", kContentTypeCss, css, 100);
  ValidateNoChanges("plain_import", CssLinkHref("c.css"));
}

TEST_F(CriticalSelectorFilterTest, LayeredImportKeepsTheStylesheetBlocking) {
  // A layered import both fetches rules and declares a layer. Neither may be
  // lost, so the stylesheet keeps its blocking link.
  GoogleString css =
      "@import url(theme.css) layer(theme);"
      "@layer a { span { color: red; } }"
      "@layer theme { div { color: red; } }";
  SetResponseWithDefaultHeaders("c.css", kContentTypeCss, css, 100);
  ValidateNoChanges("layered_import", CssLinkHref("c.css"));
}

TEST_F(CriticalSelectorFilterTest,
       PlainImportAfterLayeredImportKeepsTheStylesheetBlocking) {
  GoogleString css =
      "@import url(theme.css) layer(theme);"
      "@import url(plain.css);"
      "div { color: red; }";
  SetResponseWithDefaultHeaders("c.css", kContentTypeCss, css, 100);
  ValidateNoChanges("plain_import_after_layered", CssLinkHref("c.css"));
}

TEST_F(CriticalSelectorFilterTest,
       AnonymousLayerImportKeepsTheStylesheetBlocking) {
  GoogleString css =
      "@import url(anon.css) layer;"
      "div { color: red; }";
  SetResponseWithDefaultHeaders("c.css", kContentTypeCss, css, 100);
  ValidateNoChanges("anonymous_layer_import", CssLinkHref("c.css"));
}

TEST_F(CriticalSelectorFilterTest, ConditionedLayeredImportSupportsKept) {
  // The layer declaration from a CONDITIONED import is subject to the
  // import conditions (CSS Cascade 5): a false document-global condition
  // means the layer contributes nothing to layer order. Rewriting to an
  // unconditional "@layer theme;" would declare theme even when the
  // condition fails, flipping first-occurrence order against the deferred
  // full copy, so the import is kept verbatim and the browser evaluates
  // the condition itself.
  GoogleString css =
      "@import url(cond.css) layer(theme) supports(display: grid);"
      "div { color: red; }";
  SetResponseWithDefaultHeaders("c.css", kContentTypeCss, css, 100);
  ValidateExpected(
      "conditioned_layered_import_supports", CssLinkHref("c.css"),
      StrCat("<style>@import url(cond.css) layer(theme) "
             "supports(display: grid);div{color:red}</style>",
             LoadRestOfCss(CssLinkHref("c.css"))));
}

TEST_F(CriticalSelectorFilterTest, ConditionedLayeredImportMediaKept) {
  // Same as the supports() case, with a media query list condition: the
  // declaration only holds where the condition matches, and the subset is
  // cached per-page while the condition varies per visitor --- the import
  // is kept verbatim for the browser to evaluate.
  GoogleString css =
      "@import url(cond.css) layer(theme) screen and (min-width:1200px);"
      "div { color: red; }";
  SetResponseWithDefaultHeaders("c.css", kContentTypeCss, css, 100);
  ValidateExpected(
      "conditioned_layered_import_media", CssLinkHref("c.css"),
      StrCat("<style>@import url(cond.css) layer(theme) "
             "screen and (min-width:1200px);div{color:red}</style>",
             LoadRestOfCss(CssLinkHref("c.css"))));
}

TEST_F(CriticalSelectorFilterTest, MediaNestedLayeredImportDropped) {
  // An @import nested in a top-level @media flattens to a top-level
  // region carrying the media annotation, but nested @import is invalid
  // CSS that every browser ignores: no statement may be emitted for it
  // (a "@media print{@layer theme;}" wrapper would still declare a layer
  // the source never declares), and the fetch has no place in the inline
  // subset. The region is dropped, annotation and all.
  GoogleString css =
      "@media print { @import url(x) layer(theme); }"
      "div { color: red; }";
  SetResponseWithDefaultHeaders("c.css", kContentTypeCss, css, 100);
  ValidateExpected("media_nested_layered_import_dropped",
                   CssLinkHref("c.css"),
                   StrCat("<style>div{color:red}</style>",
                          LoadRestOfCss(CssLinkHref("c.css"))));
}

TEST_F(CriticalSelectorFilterTest, PastTopOfSheetLayeredImportDropped) {
  // An @import after a style rule is invalid (CSS lets only @charset and
  // statement-form @layer precede it), so every browser ignores it: it
  // is dropped without leaving a "@layer" statement the source never
  // made.
  GoogleString css =
      "span { color: red; }"
      "@import url(x) layer(theme);"
      "div { color: red; }";
  SetResponseWithDefaultHeaders("c.css", kContentTypeCss, css, 100);
  ValidateExpected("past_top_of_sheet_layered_import_dropped",
                   CssLinkHref("c.css"),
                   StrCat("<style>div{color:red}</style>",
                          LoadRestOfCss(CssLinkHref("c.css"))));
}

TEST_F(CriticalSelectorFilterTest,
       LayeredImportAfterLayerStatementKeepsTheStylesheetBlocking) {
  // Statement-form "@layer a;" is the one statement allowed before an
  // @import, so the import is live.
  GoogleString css =
      "@layer a;"
      "@import url(x) layer(theme);"
      "div { color: red; }";
  SetResponseWithDefaultHeaders("c.css", kContentTypeCss, css, 100);
  ValidateNoChanges("layered_import_after_layer_statement",
                    CssLinkHref("c.css"));
}

TEST_F(CriticalSelectorFilterTest, OddlyFormedImportKeepsTheStylesheetBlocking) {
  // An import in a valid position whose clauses are in an order the grammar
  // does not allow. The filter does not judge whether a browser honours it:
  // when in doubt the stylesheet keeps its blocking link.
  GoogleString css =
      "@import url(x) supports(display: grid) layer(theme);"
      "div { color: red; }";
  SetResponseWithDefaultHeaders("c.css", kContentTypeCss, css, 100);
  ValidateNoChanges("oddly_formed_import", CssLinkHref("c.css"));
}

TEST_F(CriticalSelectorFilterTest, MediaNestedLayerStatementBlocksImport) {
  // A statement-form @layer flattened out of a @media block was wrapped
  // in an at-rule, which DOES invalidate a following @import (only
  // top-level statements may precede one): the import is invalid and is
  // dropped without a statement, while the annotated statement itself
  // rides along verbatim.
  GoogleString css =
      "@media print { @layer a; }"
      "@import url(x) layer(theme);"
      "div { color: red; }";
  SetResponseWithDefaultHeaders("c.css", kContentTypeCss, css, 100);
  ValidateExpected("media_nested_layer_statement_blocks_import",
                   CssLinkHref("c.css"),
                   StrCat("<style>@media print{@layer a;}"
                          "div{color:red}</style>",
                          LoadRestOfCss(CssLinkHref("c.css"))));
}

TEST_F(CriticalSelectorFilterTest, KeyframesStayInTheInlineBlock) {
  // A rule that names an animation is of no use without its keyframes: the
  // animation would only start, and a "forwards" end state only appear, when
  // the full stylesheet arrives. Keyframes (incl. vendor-prefixed) stay
  // inline, like @font-face.
  GoogleString css =
      "@keyframes spin { from { transform: rotate(0deg); }"
      " to { transform: rotate(360deg); } }"
      "@-webkit-keyframes spin { from { transform: rotate(0deg); } }"
      "@font-face { font-family: Cool; src: url(cool.woff2); }"
      "div { animation: spin 2s linear infinite; }";
  SetResponseWithDefaultHeaders("c.css", kContentTypeCss, css, 100);
  Parse("keyframes_stay", CssLinkHref("c.css"));
  EXPECT_NE(GoogleString::npos, output_buffer_.find("@keyframes spin"))
      << output_buffer_;
  EXPECT_NE(GoogleString::npos, output_buffer_.find("@-webkit-keyframes spin"))
      << output_buffer_;
  EXPECT_NE(GoogleString::npos,
            output_buffer_.find(
                "@font-face{font-family:Cool;src:url(cool.woff2)}"))
      << output_buffer_;
  EXPECT_NE(GoogleString::npos,
            output_buffer_.find("div{animation:spin 2s linear infinite}"))
      << output_buffer_;
  EXPECT_NE(GoogleString::npos,
            output_buffer_.find(Deferred(CssLinkHref("c.css"))))
      << output_buffer_;
}

TEST_F(CriticalSelectorFilterTest, FilterInsideGroupRules) {
  // Selector filtering recurses into conditional group rule
  // (@supports/@layer/@container) bodies. "section" is not critical, so the
  // @supports body empties; an empty @supports declares nothing, so the node
  // is dropped from the critical subset (the deferred full copy retains it).
  GoogleString css =
      "@supports (display: grid) { section { display: grid; } }"
      "div { color: red; }"
      "p { color: blue; }";
  SetResponseWithDefaultHeaders("c.css", kContentTypeCss, css, 100);
  ValidateExpected("filter_inside_group_rules", CssLinkHref("c.css"),
                   StrCat("<style>div{color:red}</style>",
                          LoadRestOfCss(CssLinkHref("c.css"))));
}

TEST_F(CriticalSelectorFilterTest, MediaStraySemicolonRuleFiltered) {
  // A stray ';' between two rules in an @media body used to leave the rule
  // after it as a verbatim region with no selectors, which this filter
  // retained wholesale to be conservative. Now that the ';' is skipped, the
  // rule has a real selector and is filtered by criticality: "p" is not
  // critical, so only "div" reaches the critical subset.
  GoogleString css = "@media screen { div { color: red }; p { color: blue } }";
  SetResponseWithDefaultHeaders("c.css", kContentTypeCss, css, 100);
  ValidateExpected("media_stray_semicolon_filtered", CssLinkHref("c.css"),
                   StrCat("<style>@media screen{div{color:red}}</style>",
                          LoadRestOfCss(CssLinkHref("c.css"))));
}

TEST_F(CriticalSelectorFilterTest, KeyframesAndGroupRulesStay) {
  GoogleString css =
      "@keyframes spin { from { transform: rotate(0deg); } }"
      "@layer base { div { margin: 0px; } }"
      "div { color: red; }";
  SetResponseWithDefaultHeaders("c.css", kContentTypeCss, css, 100);
  Parse("keyframes_and_groups_stay", CssLinkHref("c.css"));
  EXPECT_NE(GoogleString::npos, output_buffer_.find("@keyframes spin"))
      << output_buffer_;
  EXPECT_NE(GoogleString::npos,
            output_buffer_.find("@layer base{div{margin:0}}"))
      << output_buffer_;
  EXPECT_NE(GoogleString::npos, output_buffer_.find("div{color:red}"))
      << output_buffer_;
}

TEST_F(CriticalSelectorFilterTest, EmptyLayerDeclarationKept) {
  // "@layer base{}" still DECLARES layer base: layer priority is
  // first-occurrence declaration order, so an emptied block-form @layer must
  // stay in the subset as an empty declaration rather than be dropped.
  GoogleString css = "@layer base { span { color: red; } } div { color: blue; }";
  SetResponseWithDefaultHeaders("c.css", kContentTypeCss, css, 100);
  ValidateExpected("empty_layer_kept", CssLinkHref("c.css"),
                   StrCat("<style>@layer base{}div{color:#00f}</style>",
                          LoadRestOfCss(CssLinkHref("c.css"))));
}

TEST_F(CriticalSelectorFilterTest, LayerDeclarationOrderPreserved) {
  // Dropping the emptied first "@layer a{}" would make "b" the subset's
  // first-declared layer, flipping the cascade to b-then-a while the source
  // (and the deferred full copy, whose first occurrences are already fixed
  // by this inline subset) means a-then-b.
  GoogleString css =
      "@layer a { span { color: red; } }"
      "@layer b { div { color: red; } }"
      "@layer a { div { margin: 0px; } }";
  SetResponseWithDefaultHeaders("c.css", kContentTypeCss, css, 100);
  ValidateExpected("layer_order_preserved", CssLinkHref("c.css"),
                   StrCat("<style>@layer a{}@layer b{div{color:red}}"
                          "@layer a{div{margin:0}}</style>",
                          LoadRestOfCss(CssLinkHref("c.css"))));
}

TEST_F(CriticalSelectorFilterTest, LayerStatementKeptGroupFiltered) {
  // Statement-form "@layer a,b;" is an unparsed region that declares layers,
  // so it rides along verbatim; the emptied block form of "b" stays as an
  // empty declaration next to it.
  GoogleString css =
      "@layer a,b;@layer b { span { color: red; } } div { color: red; }";
  SetResponseWithDefaultHeaders("c.css", kContentTypeCss, css, 100);
  ValidateExpected("layer_statement_kept", CssLinkHref("c.css"),
                   StrCat("<style>@layer a,b;@layer b{}div{color:red}</style>",
                          LoadRestOfCss(CssLinkHref("c.css"))));
}

TEST_F(CriticalSelectorFilterTest, GroupMediaGatesBody) {
  // The group node's media annotation (from an enclosing top-level @media)
  // is filtered like a ruleset's: definitely-non-screen media deletes the
  // node whole, @layer included — a @layer inside a non-matching conditional
  // rule takes no effect and contributes no first occurrence to layer order
  // (CSS Cascade 5, "Layer Ordering"), so for screen the source declared
  // nothing and print is restored by the deferred full copy. Partially
  // applying media compacts to the screen queries; raw MQ4 forms are
  // conservatively screen-affecting and keep the node.
  GoogleString css =
      "@media print { @supports (display: grid) { div { color: red; } } }"
      "@media print { @layer a { div { color: red; } } }"
      "@media screen,print { @layer b { div { color: red; } } }"
      "@media (width >= 768px) { @layer c { div { color: red; } } }"
      "div { color: red; }";
  SetResponseWithDefaultHeaders("c.css", kContentTypeCss, css, 100);
  ValidateExpected("group_media_gates_body", CssLinkHref("c.css"),
                   StrCat("<style>@media screen{@layer b{div{color:red}}}"
                          "@media (width >= 768px){@layer c{div{color:red}}}"
                          "div{color:red}</style>",
                          LoadRestOfCss(CssLinkHref("c.css"))));
}

TEST_F(CriticalSelectorFilterTest, NestedGroupsFiltered) {
  // Recursion filters nested groups: in "x" the critical rule survives
  // doubly wrapped and its non-critical sibling is dropped; in "y" the inner
  // @supports empties and is dropped (no declaration side effects), which
  // empties the enclosing @layer body — kept as a bare declaration.
  GoogleString css =
      "@layer x { @supports (display: grid) {"
      " div { color: red; } span { color: red; } } }"
      "@layer y { @supports (display: grid) { span { color: red; } } }"
      "div { color: red; }";
  SetResponseWithDefaultHeaders("c.css", kContentTypeCss, css, 100);
  ValidateExpected(
      "nested_groups_filtered", CssLinkHref("c.css"),
      StrCat("<style>@layer x{@supports (display: grid){div{color:red}}}"
             "@layer y{}div{color:red}</style>",
             LoadRestOfCss(CssLinkHref("c.css"))));
}

TEST_F(CriticalSelectorFilterTest, KeyframesInsideGroupStay) {
  GoogleString css =
      "@supports (display: grid) {"
      " @keyframes spin { from { transform: rotate(0deg); } }"
      " div { color: red; } }";
  SetResponseWithDefaultHeaders("c.css", kContentTypeCss, css, 100);
  Parse("keyframes_inside_group_stay", CssLinkHref("c.css"));
  EXPECT_NE(GoogleString::npos,
            output_buffer_.find("@supports (display: grid){"))
      << output_buffer_;
  EXPECT_NE(GoogleString::npos, output_buffer_.find("@keyframes spin"))
      << output_buffer_;
  EXPECT_NE(GoogleString::npos, output_buffer_.find("div{color:red}"))
      << output_buffer_;
}

TEST_F(CriticalSelectorFilterTest, FontFaceInsideGroupKept) {
  // Body font-faces mirror the never-filtered top-level bucket (@font-face
  // shapes text from the first paint on), and a font-face-only body keeps
  // its group even though every body ruleset was filtered out.
  GoogleString css =
      "@supports (display: grid) {"
      " @font-face { font-family: Cool; src: url(cool.woff2); }"
      " span { color: red; } }"
      "div { color: red; }";
  SetResponseWithDefaultHeaders("c.css", kContentTypeCss, css, 100);
  ValidateExpected(
      "font_face_inside_group_kept", CssLinkHref("c.css"),
      StrCat("<style>@supports (display: grid)"
             "{@font-face{font-family:Cool;src:url(cool.woff2)}}"
             "div{color:red}</style>",
             LoadRestOfCss(CssLinkHref("c.css"))));
}

TEST_F(CriticalSelectorFilterTest, EscapedIdentEmptyGroupKept) {
  // "@\73 upports" parses as a supports group (the dispatch ident is
  // decoded), but the stored prelude is verbatim bytes, which the
  // case-insensitive "@supports"/"@container" drop-if-empty test must not
  // match. The polarity is deliberate: a mis-kept empty group costs a few
  // bytes, while treating an escape-obscured "@layer" as droppable would
  // corrupt cascade order.
  GoogleString css =
      "@\\73 upports (display: grid) { span { color: red; } }"
      "div { color: red; }";
  SetResponseWithDefaultHeaders("c.css", kContentTypeCss, css, 100);
  ValidateExpected(
      "escaped_ident_empty_group_kept", CssLinkHref("c.css"),
      StrCat("<style>@\\73 upports (display: grid){}div{color:red}</style>",
             LoadRestOfCss(CssLinkHref("c.css"))));
}

TEST_F(CriticalSelectorFilterTest, EmptyContainerDropped) {
  // The @container arm of the emptied-group drop: an @container whose body
  // filtered down to nothing declares nothing (unlike block-form @layer),
  // so the node is dropped from the critical subset exactly like
  // @supports; the deferred full copy retains it.
  GoogleString css =
      "@container (width > 400px) { span { color: red; } }"
      "div { color: red; }";
  SetResponseWithDefaultHeaders("c.css", kContentTypeCss, css, 100);
  ValidateExpected("empty_container_dropped", CssLinkHref("c.css"),
                   StrCat("<style>div{color:red}</style>",
                          LoadRestOfCss(CssLinkHref("c.css"))));
}

TEST_F(CriticalSelectorFilterTest, EmptyLayerKeepsContainerAlive) {
  // The element-sensitive exception of CSS Cascade 5's layer ordering:
  // layers inside @container contribute to layer order regardless of the
  // container condition, which is evaluated per element. The emptied
  // inner "@layer a{}" stays as a declaration; because it stays, the
  // @container body is non-empty and the container node itself survives
  // too, so the layer declaration is not dropped along with it.
  GoogleString css =
      "@container (width > 400px) { @layer a { span { color: red; } } }"
      "div { color: red; }";
  SetResponseWithDefaultHeaders("c.css", kContentTypeCss, css, 100);
  ValidateExpected("empty_layer_keeps_container", CssLinkHref("c.css"),
                   StrCat("<style>@container (width > 400px){@layer a{}}"
                          "div{color:red}</style>",
                          LoadRestOfCss(CssLinkHref("c.css"))));
}

TEST_F(CriticalSelectorFilterTest, UnlayeredRuleBetweenLayersPinned) {
  // An unlayered rule interleaved between two layer blocks must stay
  // between the emptied-but-retained layer declarations: a-then-b order
  // is fixed by first occurrence, and keeping the survivor's relative
  // position is what makes the inline subset's cascade identical to the
  // source's (and to the deferred full copy's).
  GoogleString css =
      "@layer a { span { color: red; } }"
      "div { color: red; }"
      "@layer b { span { color: red; } }";
  SetResponseWithDefaultHeaders("c.css", kContentTypeCss, css, 100);
  ValidateExpected("unlayered_between_layers_pinned", CssLinkHref("c.css"),
                   StrCat("<style>@layer a{}div{color:red}@layer b{}</style>",
                          LoadRestOfCss(CssLinkHref("c.css"))));
}

TEST_F(CriticalSelectorFilterTest, NoSelectorInfo) {
  // We shouldn't change things when there is no info on selectors available.
  GoogleString css = CssLinkHref("a.css");
  page_->DeleteProperty(server_context()->beacon_cohort(),
                        CriticalSelectorFinder::kCriticalSelectorsPropertyName);
  page_->WriteCohort(server_context()->beacon_cohort());

  ResetDriver();
  ValidateNoChanges("no_sel_info", StrCat(css, "<div>Foo</div>"));

  ValidateNoChanges("no_sel_info", StrCat(css, "<div>Foo</div>"));
  ValidateRewriterLogging(RewriterHtmlApplication::PROPERTY_CACHE_MISS);
}

TEST_F(CriticalSelectorFilterTest, ResolveUrlsProperly) {
  SetResponseWithDefaultHeaders("dir/c.css", kContentTypeCss,
                                "* { background-image: url(d.png); }", 100);
  ValidateExpected("rel_path", CssLinkHref("dir/c.css"),
                   StrCat("<style>*{background-image:url(dir/d.png)}</style>",
                          LoadRestOfCss(CssLinkHref("dir/c.css"))));
}

TEST_F(CriticalSelectorFilterTest, DoNotLazyLoadIfNothingRewritten) {
  // Make sure we don't do the whole 'lazy load rest of CSS' schpiel if we
  // did not end up changing the main CSS.
  SetupWaitFetcher();
  ValidateNoChanges("not_loaded",
                    StrCat(CssLinkHref("a.css"), CssLinkHref("b.css")));
  CallFetcherCallbacks();
  // Skip ValidateRewriterLogging because fetcher interferes with WriteLog.
}

TEST_F(CriticalSelectorFilterTest, TestOnlyOptionLeavesOutTheRunCall) {
  options()->ClearSignatureForTesting();
  options()->set_test_only_prioritize_critical_css_dont_apply_original_css(
      true);
  options()->ComputeSignature();
  GoogleString link = CssLinkHref("a.css");
  GoogleString loader_without_run = StrCat(
      "<script data-pagespeed-no-defer type=\"text/javascript\">",
      rewrite_driver()->server_context()->static_asset_manager()->GetAsset(
          StaticAssetEnum::CRITICAL_CSS_LOADER_JS, rewrite_driver()->options()),
      "</script>");
  ValidateExpected("test_only", link,
                   StrCat("<style>div,*::first-letter{display:block}</style>",
                          loader_without_run, Deferred(link)));
}

// Pin (passes before and after this change): the page must keep working
// under a policy that forbids inline event-handler attributes while allowing
// inline script elements, so the loader must be a script element and the
// preload must carry no handler attribute.
TEST_F(CriticalSelectorFilterTest, LoaderUsesNoEventHandlerAttributes) {
  Parse("no_handler_attributes",
        StrCat("<head>", CssLinkHref("a.css"), CssLinkHref("b.css"),
               "</head><body><div>Stuff</div></body>"));
  EXPECT_EQ(GoogleString::npos, output_buffer_.find(" onload=")) << output_buffer_;
  EXPECT_EQ(GoogleString::npos, output_buffer_.find(" onerror="))
      << output_buffer_;
}

TEST_F(CriticalSelectorFilterTest, ALinkWithAnEventHandlerIsLeftAlone) {
  // The preload is a copy of the link. A handler on it would run when the
  // preload arrives and again when the stylesheet applies, the first time
  // before the styles are there. Such a link keeps its blocking form.
  ValidateNoChanges("handler",
                    "<link rel=stylesheet href=a.css onload=\"ready()\">");
}

TEST_F(CriticalSelectorFilterTest,
       AStylesheetThatIsNearlyAllCriticalStaysBlocking) {
  // Every rule of this stylesheet is critical. Deferring it would send
  // nearly the whole file inline and then again as a file.
  GoogleString css;
  for (int i = 1; i <= 400; ++i) {
    StrAppend(&css, "div{margin:", IntegerToString(i), "px}");
  }
  SetResponseWithDefaultHeaders("c.css", kContentTypeCss, css, 100);
  ValidateNoChanges("nearly_all_critical", CssLinkHref("c.css"));
}

TEST_F(CriticalSelectorFilterTest, AStylesheetThatIsHalfCriticalIsDeferred) {
  // Same size, but only half of it is critical ("p" is not).
  GoogleString css;
  for (int i = 1; i <= 400; ++i) {
    StrAppend(&css, "div{margin:", IntegerToString(i), "px}");
    StrAppend(&css, "p{margin:", IntegerToString(i), "px}");
  }
  SetResponseWithDefaultHeaders("c.css", kContentTypeCss, css, 100);
  Parse("half_critical", CssLinkHref("c.css"));
  EXPECT_NE(GoogleString::npos,
            output_buffer_.find(Deferred(CssLinkHref("c.css"))))
      << output_buffer_;
  EXPECT_NE(GoogleString::npos, output_buffer_.find("div{margin:400px}"))
      << output_buffer_;
  EXPECT_EQ(GoogleString::npos, output_buffer_.find("p{margin:")) << output_buffer_;
}

class CriticalSelectorWithRewriteCssFilterTest
    : public CriticalSelectorFilterTest {
 protected:
  void SetUp() override {
    options()->EnableFilter(RewriteOptions::kRewriteCss);
    CriticalSelectorFilterTest::SetUp();
  }
};

TEST_F(CriticalSelectorWithRewriteCssFilterTest, ProperlyUsedOptimized) {
  // The preload and the noscript copy name the stylesheet as optimized by
  // the filters that ran before this one.
  GoogleString link_a = CssLinkHref(Encode("", "cf", "0", "a.css", "css"));
  GoogleString link_b = CssLinkHref(Encode("", "cf", "0", "b.css", "css"));
  GoogleString html =
      StrCat("<head>", CssLinkHref("a.css"), CssLinkHref("b.css"),
             "</head>"
             "<body><div>Stuff</div></body>");

  ValidateExpected(
      "with_rewrite_css", html,
      StrCat("<head>",
             "<style>div,*::first-letter{display:block}</style>",  // a.css
             JsLoader(), Deferred(link_a),
             "<style>@media screen{*{margin:0}}</style>",  // b.css
             Deferred(link_b),
             "</head>"
             "<body><div>Stuff</div></body>"));
}

class CriticalSelectorWithCombinerFilterTest
    : public CriticalSelectorFilterTest {
 protected:
  void SetUp() override {
    options()->EnableFilter(RewriteOptions::kCombineCss);
    CriticalSelectorFilterTest::SetUp();
  }
};

TEST_F(CriticalSelectorWithCombinerFilterTest, Interaction) {
  GoogleString css = StrCat(CssLinkHref("a.css"), CssLinkHref("b.css"));

  // Only one <style> element since combine_css ran before us.
  GoogleString critical_css =
      "<style>div,*::first-letter{display:block}"  // from a.css
      "@media screen{*{margin:0}}</style>";        // from b.css

  GoogleString combined_url =
      Encode("", "cc", "0", MultiUrl("a.css", "b.css"), "css");

  ValidateExpected(
      "with_combiner", css,
      StrCat(critical_css, LoadRestOfCss(CssLinkHref(combined_url))));
}

TEST_F(CriticalSelectorWithCombinerFilterTest, ResolveWhenCombineAcrossPaths) {
  // Make sure we get proper URL resolution when doing combine-across-paths.
  SetResponseWithDefaultHeaders("dir/a.css", kContentTypeCss,
                                "* { background-image: url(/dir/d.png); }",
                                100);
  GoogleString css = StrCat(CssLinkHref("dir/a.css"), CssLinkHref("b.css"));

  // Only one <style> element since combine_css ran before us.
  GoogleString critical_css =
      "<style>*{background-image:url(dir/d.png)}"  // from dir/a.css
      "@media screen{*{margin:0}}</style>";        // from b.css

  GoogleString combined_url = "dir,_a.css+b.css.pagespeed.cc.0.css";

  ValidateExpected(
      "with_combiner_rel", css,
      StrCat(critical_css, LoadRestOfCss(CssLinkHref(combined_url))));
}

class CriticalSelectorWithInlineCssFilterTest
    : public CriticalSelectorFilterTest {
 protected:
  void SetUpBeforeSelectorsFilter() override {}

  // Add the inline css filter after the critical selector filter so
  // it matches the order that is in RewriteDriver.
  void SetUpAfterSelectorsFilter() override {
    options()->EnableFilter(RewriteOptions::kInlineCss);
    rewrite_driver()->AddFilters();
  }
};

// When CriticalSelectorFilter and InlineCss are both enabled, only
// CriticalSelectorFilter will be applied.
TEST_F(CriticalSelectorWithInlineCssFilterTest, AvoidTryingToInlineTwice) {
  GoogleString input_html =
      StrCat("<head>", CssLinkHref("a.css"), CssLinkHref("b.css"),
             "</head>"
             "<body><div>Stuff</div></body>");
  GoogleString expected_html =
      StrCat("<head>",
             "<style>div,*::first-letter{display:block}</style>",  // a.css
             JsLoader(), Deferred(CssLinkHref("a.css")),
             "<style>@media screen{*{margin:0}}</style>",  // b.css
             Deferred(CssLinkHref("b.css")),
             "</head>"
             "<body><div>Stuff</div></body>");
  ValidateExpected("with_inline_css", input_html, expected_html);
}

class CriticalSelectorWithBackgroundImageCacheExtensionTest
    : public CriticalSelectorFilterTest {
 protected:
  void SetUp() override {
    options()->EnableFilter(RewriteOptions::kExtendCacheImages);
    options()->EnableFilter(RewriteOptions::kRewriteCss);
    CriticalSelectorFilterTest::SetUp();
  }
};

// An inline style block stays in place, as optimized by the filters that ran
// before this one; the stylesheet next to it is deferred with its optimized
// URL.
TEST_F(CriticalSelectorWithBackgroundImageCacheExtensionTest,
       DontMixUnoptAndOptStyles) {
  // Triggers calling InlineOutputResource::url().
  options()->ClearSignatureForTesting();
  options()->set_enable_cache_purge(true);
  options()->ComputeSignature();

  SetResponseWithDefaultHeaders("background.png", kContentTypePng,
                                "Dummy contents", 100);
  SetResponseWithDefaultHeaders("extern.css", kContentTypeCss,
                                "* {background: url(background.png)}\n"
                                ".not-critical { margin: 0 }",
                                100);

  GoogleString input_html =
      StrCat("<head>\n"
             "  <style>div { background: url(background.png) }"
             ".not-critical { color: red; }</style>\n"
             "  ",
             CssLinkHref("extern.css"),
             "\n"
             "</head>\n"
             "<body>\n"
             "  <div>Foo</div>\n"
             "  <span>Bar</span>\n"
             "</body>\n");
  GoogleString image = Encode("", "ce", "0", "background.png", "png");
  GoogleString expected_html =
      StrCat("<head>\n"
             "  <style>div{background:url(",
             image,
             ")}"
             ".not-critical{color:red}</style>\n"
             "  <style>*{background:url(",
             image, ")}</style>",
             LoadRestOfCss(
                 CssLinkHref(Encode("", "cf", "0", "extern.css", "css"))),
             "\n"
             "</head>\n"
             "<body>\n"
             "  <div>Foo</div>\n"
             "  <span>Bar</span>\n"
             "</body>\n");
  ValidateExpected("dont_mix_unopt_and_opt", input_html, expected_html);
}

}  // namespace

}  // namespace net_instaweb
