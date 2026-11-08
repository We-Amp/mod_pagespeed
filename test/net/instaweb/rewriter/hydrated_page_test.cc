// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

// A page a client-side framework hydrates keeps its stylesheet URLs and inline
// styles, from the flush window in which that becomes known.

#include <memory>

#include "net/instaweb/rewriter/public/rewrite_driver.h"
#include "net/instaweb/rewriter/public/rewrite_options.h"
#include "pagespeed/kernel/base/string.h"
#include "pagespeed/kernel/base/string_util.h"
#include "pagespeed/kernel/http/content_type.h"
#include "pagespeed/kernel/http/http_names.h"
#include "pagespeed/kernel/http/response_headers.h"
#include "test/net/instaweb/rewriter/rewrite_test_base.h"
#include "test/pagespeed/kernel/base/gtest.h"
#include "test/pagespeed/kernel/http/user_agent_matcher_test_base.h"

namespace net_instaweb {

namespace {

const char kCss[] = ".a {  color:  red;  }\n";
const char kCssB[] = ".b {  color:  blue;  }\n";
const char kPage[] =
    "<html><head><link rel=\"stylesheet\" href=\"a.css\">"
    "<link rel=\"stylesheet\" href=\"b.css\">"
    "<style>\n.hd {  color:  blue;  }\n</style></head>"
    "<body><div style=\"background: url('x.png')\">t</div>";

class HydratedPageTest : public RewriteTestBase {
 protected:
  void SetUp() override {
    RewriteTestBase::SetUp();
    SetResponseWithDefaultHeaders("a.css", kContentTypeCss, kCss, 100);
    SetResponseWithDefaultHeaders("b.css", kContentTypeCss, kCssB, 100);
    SetResponseWithDefaultHeaders("s.js", kContentTypeJavascript,
                                  "var  s  =  1;\n", 100);
    SetResponseWithDefaultHeaders("t.js", kContentTypeJavascript,
                                  "var  t  =  1;\n", 100);
  }

  GoogleString Rewritten(StringPiece name, StringPiece html) {
    Parse(name, html);
    return output_buffer_;
  }
};

TEST_F(HydratedPageTest, MarkersAreRecognized) {
  options()->EnableFilter(RewriteOptions::kRewriteCss);
  rewrite_driver()->AddFilters();
  const char* const kMarked[] = {
      "<script src=\"/_next/static/chunks/main.js\" async></script>",
      "<script src=\"https://cdn.example.com/app/_next/static/a.js\"></script>",
      "<link rel=\"preload\" as=\"script\" href=\"/_next/static/a.js\">",
      "<link rel=\"stylesheet\" href=\"/_nuxt/entry.css\">",
      "<link rel=\"modulepreload\" href=\"/_app/immutable/entry/start.js\">",
      "<link rel=\"stylesheet\" href=\"/p.css\" data-precedence=\"default\">",
      "<style data-precedence=\"high\" data-href=\"x\">a{}</style>",
      "<script id=\"__NEXT_DATA__\" type=\"application/json\">{}</script>",
      "<script id=\"__NUXT_DATA__\" type=\"application/json\">[]</script>",
      "<script id=\"ng-state\" type=\"application/json\">{}</script>",
      "<div id=\"__next\"></div>",
      "<div id=\"__nuxt\"></div>",
      "<div data-reactroot=\"\"></div>",
      "<div id=\"app\" data-server-rendered=\"true\"></div>",
      "<style ng-app-id=\"ng\">a{}</style>",
      "<app-root ng-version=\"19.0.0\"></app-root>",
      "<app-root ng-server-context=\"ssr\"></app-root>",
      "<body data-sveltekit-preload-data=\"hover\"></body>",
      "<astro-island uid=\"x\"></astro-island>",
      "<!--$--><div></div><!--/$-->",
      "<!--[--><div></div><!--]-->",
      "<script>window.__reactRouterContext = {};</script>",
      "<script>window.__remixContext = {};</script>",
  };
  int index = 0;
  for (const char* marked : kMarked) {
    ClearRewriteDriver();
    ASSERT_FALSE(rewrite_driver()->is_hydrated_document());
    Parse(StrCat("marked", IntegerToString(index++)), marked);
    EXPECT_TRUE(rewrite_driver()->is_hydrated_document()) << marked;
  }
}

TEST_F(HydratedPageTest, TextThatMentionsAMarkerIsNotAMarker) {
  options()->EnableFilter(RewriteOptions::kRewriteCss);
  rewrite_driver()->AddFilters();
  const char* const kUnmarked[] = {
      "<pre>&lt;script id=\"__NEXT_DATA__\"&gt; /_next/static/a.js "
      "data-precedence window.__remixContext</pre>",
      "<pre>&lt;!--$--&gt;</pre>",
      "<p>/_nuxt/entry.css data-reactroot ng-version</p>",
      "<a href=\"/_next/docs\">about</a>",
      "<img src=\"/_next/static/a.png\">",
      "<div id=\"__NEXT_DATA__\"></div>",
      "<script id=\"__next\"></script>",
      "<script src=\"/x.js?u=/_next/a.js\"></script>",
      "<!-- window.__remixContext -->",
      "<!-- $ -->",
      "<div data-precedence-not=\"1\" x-data-reactroot=\"\"></div>",
      "<style>a{background:url(/_next/a.png)}</style>",
      "<script>var s = '<!--$-->';</script>",
  };
  int index = 0;
  for (const char* unmarked : kUnmarked) {
    ClearRewriteDriver();
    Parse(StrCat("unmarked", IntegerToString(index++)), unmarked);
    EXPECT_FALSE(rewrite_driver()->is_hydrated_document()) << unmarked;
  }
}

TEST_F(HydratedPageTest, VaryRscHeaderIsASignal) {
  options()->EnableFilter(RewriteOptions::kRewriteCss);
  rewrite_driver()->AddFilters();
  ResponseHeaders headers;
  headers.Add(HttpAttributes::kVary,
              "RSC, Next-Router-State-Tree, Next-Router-Prefetch");
  headers.ComputeCaching();
  rewrite_driver()->set_response_headers_ptr(&headers);
  ValidateNoChanges("vary_rsc", kPage);
  EXPECT_TRUE(rewrite_driver()->is_hydrated_document());
}

TEST_F(HydratedPageTest, OtherVaryIsNotASignal) {
  options()->EnableFilter(RewriteOptions::kRewriteCss);
  rewrite_driver()->AddFilters();
  ResponseHeaders headers;
  headers.Add(HttpAttributes::kVary, "Accept-Encoding, RSC-Not");
  headers.ComputeCaching();
  rewrite_driver()->set_response_headers_ptr(&headers);
  Parse("vary_other", kPage);
  EXPECT_FALSE(rewrite_driver()->is_hydrated_document());
}

// The marker comes after the stylesheets it protects, in the same flush
// window: nothing in that window may be rewritten.
TEST_F(HydratedPageTest, SignalCancelsRewritesEarlierInItsWindow) {
  options()->EnableFilter(RewriteOptions::kRewriteCss);
  options()->EnableFilter(RewriteOptions::kCombineCss);
  options()->EnableFilter(RewriteOptions::kInlineCss);
  options()->EnableFilter(RewriteOptions::kExtendCacheCss);
  rewrite_driver()->AddFilters();
  ValidateNoChanges("same_window",
                    StrCat(kPage, "<div id=\"__next\"></div></body></html>"));
  EXPECT_TRUE(rewrite_driver()->is_hydrated_document());
}

// A window on the wire cannot be recalled, and the latch never lets go.
TEST_F(HydratedPageTest, LatchHoldsForLaterWindowsOnly) {
  options()->EnableFilter(RewriteOptions::kExtendCacheCss);
  rewrite_driver()->AddFilters();
  SetupWriter();
  rewrite_driver()->StartParse(StrCat(kTestDomain, "windows.html"));
  rewrite_driver()->ParseText("<link rel=\"stylesheet\" href=\"a.css\">");
  rewrite_driver()->Flush();
  const GoogleString first_window = output_buffer_;
  EXPECT_NE(GoogleString::npos, first_window.find(".pagespeed."))
      << first_window;
  EXPECT_FALSE(rewrite_driver()->is_hydrated_document());

  rewrite_driver()->ParseText("<div id=\"__next\"></div>");
  rewrite_driver()->ParseText("<link rel=\"stylesheet\" href=\"b.css\">");
  rewrite_driver()->Flush();
  EXPECT_TRUE(rewrite_driver()->is_hydrated_document());
  rewrite_driver()->ParseText("<link rel=\"stylesheet\" href=\"a.css\">");
  rewrite_driver()->FinishParse();
  EXPECT_TRUE(StringPiece(output_buffer_).starts_with(first_window));
  EXPECT_EQ(StrCat(first_window,
                   "<div id=\"__next\"></div>"
                   "<link rel=\"stylesheet\" href=\"b.css\">"
                   "<link rel=\"stylesheet\" href=\"a.css\">"),
            output_buffer_);
}

TEST_F(HydratedPageTest, ImagesAreStillRewritten) {
  options()->EnableFilter(RewriteOptions::kExtendCacheCss);
  options()->EnableFilter(RewriteOptions::kExtendCacheImages);
  rewrite_driver()->AddFilters();
  SetResponseWithDefaultHeaders("i.png", kContentTypePng, "png", 100);
  const GoogleString out = Rewritten(
      "images",
      "<link rel=\"stylesheet\" href=\"a.css\" data-precedence=\"default\">"
      "<img src=\"i.png\">");
  EXPECT_NE(GoogleString::npos,
            out.find("<link rel=\"stylesheet\" href=\"a.css\""))
      << out;
  EXPECT_NE(GoogleString::npos, out.find("i.png.pagespeed.ce.")) << out;
}

TEST_F(HydratedPageTest, PageWithoutASignalIsIdenticalWithDetectionOnOrOff) {
  options()->EnableFilter(RewriteOptions::kRewriteCss);
  options()->EnableFilter(RewriteOptions::kCombineCss);
  options()->EnableFilter(RewriteOptions::kExtendCacheCss);
  rewrite_driver()->AddFilters();
  const GoogleString on = Rewritten("plain", kPage);
  EXPECT_FALSE(rewrite_driver()->is_hydrated_document());
  EXPECT_NE(kPage, on);
  output_buffer_.clear();

  SetActiveServer(kSecondary);
  options()->set_detect_hydrated_pages(false);
  options()->EnableFilter(RewriteOptions::kRewriteCss);
  options()->EnableFilter(RewriteOptions::kCombineCss);
  options()->EnableFilter(RewriteOptions::kExtendCacheCss);
  rewrite_driver()->AddFilters();
  EXPECT_EQ(on, Rewritten("plain", kPage));
}

TEST_F(HydratedPageTest, SwitchOffRestoresRewritingOfAMarkedPage) {
  options()->set_detect_hydrated_pages(false);
  options()->EnableFilter(RewriteOptions::kExtendCacheCss);
  rewrite_driver()->AddFilters();
  const GoogleString out = Rewritten("switch_off",
                                     "<link rel=\"stylesheet\" href=\"a.css\">"
                                     "<div id=\"__next\"></div>");
  EXPECT_FALSE(rewrite_driver()->is_hydrated_document());
  EXPECT_NE(GoogleString::npos, out.find("a.css.pagespeed.ce.")) << out;
}

TEST_F(HydratedPageTest, SwitchIsServerWideAndOutOfTheSignature) {
  const RewriteOptions::PropertyBase* property =
      RewriteOptions::LookupOptionByName("DetectHydratedPages");
  ASSERT_NE(nullptr, property);
  EXPECT_EQ(RewriteOptions::kProcessScopeStrict, property->scope());
  EXPECT_TRUE(options()->detect_hydrated_pages());

  std::unique_ptr<RewriteOptions> off(options()->Clone());
  off->set_detect_hydrated_pages(false);
  std::unique_ptr<RewriteOptions> on(options()->Clone());
  on->ComputeSignature();
  off->ComputeSignature();
  EXPECT_EQ(on->signature(), off->signature());
}

TEST_F(HydratedPageTest, OnlyImagePreloadsGoOnAHydratedPage) {
  options()->EnableFilter(RewriteOptions::kExtendCacheCss);
  rewrite_driver()->AddFilters();
  const char kKept[] =
      "<link rel=\"preload\" as=\"style\" href=\"a.css\">"
      "<link rel=\"preload\" as=\"script\" href=\"s.js\">"
      "<link rel=\"subresource\" href=\"b.css\">"
      "<link rel=\"modulepreload\" href=\"m.js\">";
  const char kImage[] = "<link rel=\"preload\" as=\"image\" href=\"i.png\">";
  const char kMarker[] = "<div id=\"__next\"></div>";
  // The marker follows the hints: same flush window. Stylesheet and script
  // urls stay, so do their hints. Images are still rewritten, so their
  // preloads go, as on any page.
  ValidateExpected("hints_kept", StrCat(kKept, kImage, kMarker),
                   StrCat(kKept, kMarker));
  EXPECT_TRUE(rewrite_driver()->is_hydrated_document());
  ClearRewriteDriver();
  // Without a marker the script, style and subresource hints go, as before.
  ValidateExpected("hints_stripped", StrCat(kKept, kImage),
                   "<link rel=\"modulepreload\" href=\"m.js\">");
}

// defer_javascript retypes scripts, renames iframes inside the framework's tree
// and adds nodes at the end of the body: a framework that compares tag names
// sees a different tree. A marked page keeps all of that.
TEST_F(HydratedPageTest, DeferJavascriptLeavesAMarkedPage) {
  // An empty user agent counts as a bot, which never gets deferral.
  SetCurrentUserAgent(UserAgentMatcherTestBase::kChrome18UserAgent);
  options()->EnableFilter(RewriteOptions::kDeferJavascript);
  rewrite_driver()->AddFilters();
  const char kBody[] =
      "<script src=\"s.js\"></script>"
      "<div><iframe src=\"f.html\"></iframe>"
      "<img src=\"i.png\" onload=\"go()\"></div>"
      "<script>var  a  =  1;</script>";
  const GoogleString plain = Rewritten("defer_plain", kBody);
  EXPECT_FALSE(rewrite_driver()->is_hydrated_document());
  EXPECT_TRUE(rewrite_driver()->js_deferral_started());
  EXPECT_NE(GoogleString::npos, plain.find("text/psajs")) << plain;
  EXPECT_NE(GoogleString::npos, plain.find("pagespeed_iframe")) << plain;
  EXPECT_NE(GoogleString::npos, plain.find("js_defer")) << plain;

  ClearRewriteDriver();
  const GoogleString marked =
      Rewritten("defer_marked", StrCat("<div id=\"__next\"></div>", kBody));
  EXPECT_TRUE(rewrite_driver()->is_hydrated_document());
  EXPECT_FALSE(rewrite_driver()->js_deferral_started());
  EXPECT_NE(GoogleString::npos,
            marked.find(StrCat("<div id=\"__next\"></div>", kBody)))
      << marked;
  EXPECT_EQ(GoogleString::npos, marked.find("psajs")) << marked;
  EXPECT_EQ(GoogleString::npos, marked.find("pagespeed_iframe")) << marked;
  EXPECT_EQ(GoogleString::npos, marked.find("js_defer")) << marked;
}

// Once a script is retyped it needs the deferral runtime: a marker in a later
// flush window does not stop deferral halfway.
TEST_F(HydratedPageTest, DeferralStartedBeforeTheMarkerFinishes) {
  SetCurrentUserAgent(UserAgentMatcherTestBase::kChrome18UserAgent);
  options()->EnableFilter(RewriteOptions::kDeferJavascript);
  rewrite_driver()->AddFilters();
  SetupWriter();
  SetDriverRequestHeaders();  // Parse() does this; StartParse() does not.
  rewrite_driver()->StartParse(StrCat(kTestDomain, "defer_windows.html"));
  rewrite_driver()->ParseText("<html><body><script src=\"s.js\"></script>");
  rewrite_driver()->Flush();
  EXPECT_TRUE(rewrite_driver()->js_deferral_started());
  rewrite_driver()->ParseText(
      "<div id=\"__next\"></div><script src=\"t.js\"></script>"
      "</body></html>");
  rewrite_driver()->FinishParse();
  EXPECT_TRUE(rewrite_driver()->is_hydrated_document());
  EXPECT_NE(GoogleString::npos,
            output_buffer_.find("<script src=\"t.js\" type=\"text/psajs\""))
      << output_buffer_;
  EXPECT_NE(GoogleString::npos, output_buffer_.find("js_defer"))
      << output_buffer_;
}

// With disable_javascript on as well, scripts are off on purpose, so all three
// deferral filters keep running on a marked page: a retyped script always gets
// the runtime, as on an unmarked page.
TEST_F(HydratedPageTest, DeferralFiltersAgreeUnderDisableJavascript) {
  SetCurrentUserAgent(UserAgentMatcherTestBase::kChrome18UserAgent);
  options()->EnableFilter(RewriteOptions::kDeferJavascript);
  options()->EnableFilter(RewriteOptions::kDisableJavascript);
  rewrite_driver()->AddFilters();
  const char kBody[] =
      "<script src=\"s.js\"></script>"
      "<div><iframe src=\"f.html\"></iframe></div>";
  const GoogleString plain = Rewritten("both_plain", kBody);
  EXPECT_FALSE(rewrite_driver()->is_hydrated_document());
  EXPECT_NE(GoogleString::npos, plain.find("text/psajs")) << plain;
  EXPECT_NE(GoogleString::npos, plain.find("pagespeed_iframe")) << plain;
  EXPECT_NE(GoogleString::npos, plain.find("js_defer")) << plain;

  ClearRewriteDriver();
  const GoogleString marked =
      Rewritten("both_marked", StrCat("<div id=\"__next\"></div>", kBody));
  EXPECT_TRUE(rewrite_driver()->is_hydrated_document());
  EXPECT_NE(GoogleString::npos, marked.find("text/psajs")) << marked;
  EXPECT_NE(GoogleString::npos, marked.find("pagespeed_iframe")) << marked;
  EXPECT_NE(GoogleString::npos, marked.find("js_defer")) << marked;
}

// Every filter that touches a stylesheet or a script, alone, leaves a marked
// page byte for byte.
class HydratedPageFilterTest
    : public HydratedPageTest,
      public ::testing::WithParamInterface<RewriteOptions::Filter> {};

TEST_P(HydratedPageFilterTest, MarkedPageIsUnchanged) {
  options()->EnableFilter(GetParam());
  options()->EnableFilter(RewriteOptions::kHtmlWriterFilter);
  options()->set_css_outline_min_bytes(0);
  options()->set_js_outline_min_bytes(0);
  options()->WriteableDomainLawyer()->AddRewriteDomainMapping(
      "http://cdn.example.com", kTestDomain, message_handler());
  rewrite_driver()->AddFilters();
  ValidateNoChanges(
      "class", StrCat("<html><head>"
                      "<link rel=\"stylesheet\" href=\"",
                      kTestDomain,
                      "a.css\" data-precedence=\"default\">"
                      "<link rel=\"stylesheet\" href=\"b.css\">"
                      "<style>@import url(a.css);</style>"
                      "<style>\n.hd {  color:  blue;  }\n</style></head>"
                      "<body><!--$--><p>a  b</p>  <!--/$-->"
             "<div style=\"color:  red;\">t</div>"
                      "<link rel=\"stylesheet\" href=\"b.css\">"
                      "<script src=\"",
                      kTestDomain,
                      "s.js\" async></script><script src=\"s.js\"></script>"
                      "<script src=\"t.js\"></script>"
                      "<script>var  a  =  1;</script></body></html>"));
}

INSTANTIATE_TEST_SUITE_P(
    StylesheetsAndScripts, HydratedPageFilterTest,
    ::testing::Values(
        RewriteOptions::kRewriteCss, RewriteOptions::kFlattenCssImports,
        RewriteOptions::kInlineCss, RewriteOptions::kExtendCacheCss,
        RewriteOptions::kCombineCss, RewriteOptions::kInlineImportToLink,
        RewriteOptions::kOutlineCss, RewriteOptions::kMoveCssToHead,
        RewriteOptions::kMoveCssAboveScripts,
        RewriteOptions::kRewriteStyleAttributes,
        RewriteOptions::kRewriteDomains,
        RewriteOptions::kRewriteJavascriptExternal,
        RewriteOptions::kRewriteJavascriptInline,
        RewriteOptions::kExtendCacheScripts, RewriteOptions::kCombineJavascript,
        RewriteOptions::kInlineJavascript, RewriteOptions::kOutlineJavascript,
        RewriteOptions::kRemoveComments, RewriteOptions::kCollapseWhitespace,
        RewriteOptions::kLeftTrimUrls));

}  // namespace

}  // namespace net_instaweb
