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

#include "net/instaweb/rewriter/public/css_inline_filter.h"

#include <algorithm>

#include "net/instaweb/rewriter/public/cache_extender.h"
#include "net/instaweb/rewriter/public/domain_lawyer.h"
#include "net/instaweb/rewriter/public/resource.h"
#include "net/instaweb/rewriter/public/rewrite_driver.h"
#include "net/instaweb/rewriter/public/rewrite_filter.h"
#include "net/instaweb/rewriter/public/rewrite_options.h"
#include "net/instaweb/rewriter/public/server_context.h"
#include "pagespeed/kernel/base/basictypes.h"
#include "pagespeed/kernel/base/charset_util.h"
#include "pagespeed/kernel/base/statistics.h"
#include "pagespeed/kernel/base/stdio_file_system.h"
#include "pagespeed/kernel/base/string.h"
#include "pagespeed/kernel/base/string_util.h"
#include "pagespeed/kernel/cache/lru_cache.h"
#include "pagespeed/kernel/html/inline_style_text.h"
#include "pagespeed/kernel/http/content_type.h"
#include "pagespeed/kernel/http/google_url.h"
#include "pagespeed/kernel/http/http_names.h"
#include "pagespeed/kernel/http/response_headers.h"
#include "pagespeed/kernel/http/semantic_type.h"
#include "test/net/instaweb/rewriter/prepared_style_text_recorder.h"
#include "test/net/instaweb/rewriter/rewrite_test_base.h"
#include "test/pagespeed/kernel/base/gtest.h"
#include "test/pagespeed/kernel/base/mock_message_handler.h"
#include "test/pagespeed/kernel/html/html_parse_test_base.h"

namespace net_instaweb {

namespace {

class CssInlineFilterTest : public RewriteTestBase {
 protected:
  CssInlineFilterTest() : filters_added_(false) {}

  void TestInlineCssWithOutputUrl(
      const GoogleString& html_url, const GoogleString& head_extras,
      const GoogleString& css_url, const GoogleString& css_out_url,
      const GoogleString& other_attrs, const GoogleString& css_original_body,
      bool expect_inline, const GoogleString& css_rewritten_body,
      const GoogleString& debug_string) {
    if (!filters_added_) {
      AddFilter(RewriteOptions::kInlineCss);
      filters_added_ = true;
    }

    const GoogleString html_template = StrCat(
        "<head>\n", head_extras, "  <link rel=\"stylesheet\" href=\"%s\"",
        (other_attrs.empty() ? "" : " " + other_attrs) + ">",
        "%s\n</head>\n"
        "<body>Hello, world!</body>\n");

    const GoogleString html_input = absl::StrFormat(
        *absl::ParsedFormat<'s', 's'>::New(html_template), css_url.c_str(), "");

    const GoogleString outline_html_output =
        absl::StrFormat(*absl::ParsedFormat<'s', 's'>::New(html_template),
                        css_out_url.c_str(), "");

    const GoogleString outline_debug_html_output =
        debug_string.empty()
            ? outline_html_output
            : absl::StrFormat(*absl::ParsedFormat<'s', 's'>::New(html_template),
                              css_out_url.c_str(),
                              StrCat("<!--", debug_string, "-->").c_str());

    // Put original CSS file into our fetcher.
    ResponseHeaders default_css_header;
    SetDefaultLongCacheHeaders(&kContentTypeCss, &default_css_header);
    SetFetchResponse(css_url, default_css_header, css_original_body);

    // Rewrite the HTML page.
    ParseUrl(html_url, html_input);

    const GoogleString expected_output =
        (!expect_inline
             ? GoogleString(outline_html_output)
             : StrCat(
                   "<head>\n", head_extras,
                   StrCat("  <style",
                          (other_attrs.empty() ? "" : " " + other_attrs), ">"),
                   css_rewritten_body,
                   "</style>\n"
                   "</head>\n"
                   "<body>Hello, world!</body>\n"));
    EXPECT_EQ(AddHtmlBody(expected_output), output_buffer_);

    if (!expect_inline) {
      output_buffer_.clear();
      TurnOnDebug();
      ParseUrl(html_url, html_input);
      EXPECT_EQ(AddHtmlBody(outline_debug_html_output), output_buffer_);
    }
  }

  void TestInlineCss(const GoogleString& html_url, const GoogleString& css_url,
                     const GoogleString& other_attrs,
                     const GoogleString& css_original_body, bool expect_inline,
                     const GoogleString& css_rewritten_body) {
    TestInlineCssWithOutputUrl(html_url, "", css_url, css_url, other_attrs,
                               css_original_body, expect_inline,
                               css_rewritten_body, "");
  }

  void TestNoInlineCss(const GoogleString& html_url,
                       const GoogleString& css_url,
                       const GoogleString& other_attrs,
                       const GoogleString& css_original_body,
                       const GoogleString& css_rewritten_body,
                       const GoogleString& debug_string) {
    TestInlineCssWithOutputUrl(html_url, "", css_url, css_url, other_attrs,
                               css_original_body, false, css_rewritten_body,
                               debug_string);
  }

  void VerifyNoInliningForClosingStyleTag(
      const GoogleString& closing_style_tag) {
    AddFilter(RewriteOptions::kInlineCss);
    SetResponseWithDefaultHeaders("foo.css", kContentTypeCss,
                                  StrCat("a{margin:0}", closing_style_tag),
                                  100);

    // We don't mess with links that contain a closing style tag.
    ValidateNoChanges("no_inlining_of_close_style_tag",
                      "<link rel='stylesheet' href='foo.css'>");

    TurnOnDebug();
    ValidateExpected("no_inlining_of_close_style_tag+debug",
                     "<link rel='stylesheet' href='foo.css'>",
                     "<link rel='stylesheet' href='foo.css'>"
                     "<!--CSS not inlined since it contains "
                     "a less-than sign-->");
  }

  void TurnOnDebug() {
    options()->ClearSignatureForTesting();
    options()->EnableFilter(RewriteOptions::kDebug);
    server_context()->ComputeSignature(options());
  }

  bool AddHtmlTags() const override { return false; }

 private:
  bool filters_added_;
};

TEST_F(CssInlineFilterTest, InlineCssSimple) {
  const GoogleString css = "BODY { color: red; }\n";
  TestInlineCss("http://www.example.com/index.html",
                "http://www.example.com/styles.css", "", css, true, css);
}

// Inlining would copy integrity= onto a <style> element, where browsers
// ignore it, so the integrity check the author asked for would stop
// happening. The link is left alone.
TEST_F(CssInlineFilterTest, DoNotInlineIntegrityLink) {
  const GoogleString css = "BODY { color: red; }\n";
  TestNoInlineCss("http://www.example.com/index.html",
                  "http://www.example.com/styles.css", "integrity=\"sha384-x\"",
                  css, css,
                  "CSS not inlined because of the integrity attribute");
}

class CssInlineFilterTestCustomOptions : public CssInlineFilterTest {
 protected:
  // Derived classes should add their options and then call
  // CssInlineFilterTest::SetUp().
  void SetUp() override {}
};

TEST_F(CssInlineFilterTest, InlineCssUnhealthy) {
  lru_cache()->set_is_healthy(false);
  const GoogleString css = "BODY { color: red; }\n";
  TestInlineCss("http://www.example.com/index.html",
                "http://www.example.com/styles.css", "", css, false, css);
}

TEST_F(CssInlineFilterTest, InlineCss404) {
  // Test to make sure that a missing input is handled well.
  SetFetchResponse404("404.css");
  ValidateNoChanges("404", "<link rel=stylesheet href='404.css'>");

  // Second time, to make sure caching doesn't break it.
  ValidateNoChanges("404", "<link rel=stylesheet href='404.css'>");
}

TEST_F(CssInlineFilterTest, InlineCssCached) {
  // Doing it twice should be safe, too.
  const GoogleString css = "BODY { color: red; }\n";
  TestInlineCss("http://www.example.com/index.html",
                "http://www.example.com/styles.css", "", css, true, css);
  TestInlineCss("http://www.example.com/index.html",
                "http://www.example.com/styles.css", "", css, true, css);
}

TEST_F(CssInlineFilterTest, InlineCssRewriteUrls1) {
  // CSS with a relative URL that needs to be changed:
  const GoogleString css1 = "BODY { background-image: url('bg.png'); }\n";
  const GoogleString css2 =
      "BODY { background-image: url('foo/bar/bg.png'); }\n";
  TestInlineCss("http://www.example.com/index.html",
                "http://www.example.com/foo/bar/baz.css", "", css1, true, css2);
}

TEST_F(CssInlineFilterTest, InlineCssRewriteUrls2) {
  // CSS with a relative URL, this time with ".." in it:
  const GoogleString css1 =
      "BODY { background-image: url('../quux/bg.png'); }\n";
  const GoogleString css2 =
      "BODY { background-image: url('foo/quux/bg.png'); }\n";
  TestInlineCss("http://www.example.com/index.html",
                "http://www.example.com/foo/bar/baz.css", "", css1, true, css2);
}

TEST_F(CssInlineFilterTest, NoRewriteUrlsSameDir) {
  const GoogleString css = "BODY { background-image: url('bg.png'); }\n";
  TestInlineCss("http://www.example.com/index.html",
                "http://www.example.com/baz.css", "", css, true, css);
}

TEST_F(CssInlineFilterTest, ShardSubresources) {
  UseMd5Hasher();
  DomainLawyer* lawyer = options()->WriteableDomainLawyer();
  lawyer->AddShard("www.example.com", "shard1.com,shard2.com",
                   &message_handler_);

  const GoogleString css_in =
      ".p1 { background-image: url('b1.png'); }"
      ".p2 { background-image: url('b2.png'); }";
  const GoogleString css_out =
      ".p1 { background-image: url('http://shard2.com/b1.png'); }"
      ".p2 { background-image: url('http://shard1.com/b2.png'); }";
  TestInlineCss("http://www.example.com/index.html",
                "http://www.example.com/baz.css", "", css_in, true, css_out);
}

// Fragment-only references stay as written in a stylesheet inlined from
// another directory, with shards set.
TEST_F(CssInlineFilterTest, FragmentReferenceKeptWithShards) {
  UseMd5Hasher();
  DomainLawyer* lawyer = options()->WriteableDomainLawyer();
  lawyer->AddShard("www.example.com", "shard1.com,shard2.com",
                   &message_handler_);

  const GoogleString css_in =
      ".p1 { mask: url(#m); }"
      ".p2 { mask: url('#n'); }";
  const GoogleString css_out = css_in;
  TestInlineCss("http://www.example.com/index.html",
                "http://www.example.com/foo/baz.css", "", css_in, true,
                css_out);
}

TEST_F(CssInlineFilterTest, DoNotInlineCssWithMediaNotScreen) {
  const GoogleString css = "BODY { color: red; }\n";
  TestNoInlineCss("http://www.example.com/index.html",
                  "http://www.example.com/styles.css", "media=\"print\"", css,
                  "", "CSS not inlined because media does not match screen");
}

TEST_F(CssInlineFilterTest, DoInlineCssWithMediaAll) {
  const GoogleString css = "BODY { color: red; }\n";
  TestInlineCss("http://www.example.com/index.html",
                "http://www.example.com/styles.css", "media=\"all\"", css, true,
                css);
}

TEST_F(CssInlineFilterTest, DoInlineCssWithMediaScreen) {
  const GoogleString css = "BODY { color: red; }\n";
  TestInlineCss("http://www.example.com/index.html",
                "http://www.example.com/styles.css",
                "media=\"print, audio ,, ,sCrEeN \"", css, true, css);
}

TEST_F(CssInlineFilterTest, DoInlineCssWithMediaQuery) {
  // Media queries are tested more exhaustively in css_tag_scanner_test.
  const GoogleString css = "BODY { color: red; }\n";
  TestInlineCss("http://www.example.com/index.html",
                "http://www.example.com/styles.css", "media=\"only (color)\"",
                css, true, css);
}

TEST_F(CssInlineFilterTest, Empty) {
  // Don't inline empty resources. This is defensive programming against
  // issues like: https://github.com/apache/incubator-pagespeed-mod/issues/1050
  const GoogleString css = "";
  TestNoInlineCss("http://www.example.com/index.html",
                  "http://www.example.com/styles.css", "", css, "",
                  "Resource is empty, preventing rewriting of "
                  "http://www.example.com/styles.css");
}

TEST_F(CssInlineFilterTest, InlineCssWithInvalidMedia) {
  // Use an invalid media tag, but one that's still decipherable.
  // Trying to deal with indecipherable media tags turned out to be
  // more trouble than it's worth.
  const char kNotValid[] = "not!?#?;valid";

  const GoogleString css = "BODY { color: red; }\n";
  GoogleString media;

  // Now do the actual test that we don't inline the CSS with an invalid
  // media type (and not screen or all as well).
  media = StrCat("media=\"", kNotValid, "\"");
  TestNoInlineCss("http://www.example.com/index.html",
                  "http://www.example.com/styles.css", media, css, "",
                  "CSS not inlined because media does not match screen");

  // And now test that we DO inline the CSS with an invalid media type
  // if there's also an instance of "screen" in the media attribute.
  media = StrCat("media=\"", kNotValid, ",screen\"");
  TestInlineCss("http://www.example.com/index.html",
                "http://www.example.com/styles.css", media, css, true, css);
}

TEST_F(CssInlineFilterTest, DoNotInlineCssTooBig) {
  // CSS too large to inline:
  const int64 length = 2 * RewriteOptions::kDefaultCssInlineMaxBytes;
  TestNoInlineCss("http://www.example.com/index.html",
                  "http://www.example.com/styles.css", "",
                  ("BODY { background-image: url('" +
                   GoogleString(length, 'z') + ".png'); }\n"),
                  "", "CSS not inlined since it&#39;s bigger than 2048 bytes");
}

TEST_F(CssInlineFilterTest, DoInlineCssDifferentDomain) {
  const GoogleString css = "BODY { color: red; }\n";
  options()->AddInlineUnauthorizedResourceType(semantic_type::kStylesheet);
  TestInlineCss("http://www.example.com/index.html",
                "http://unauth.com/styles.css", "", css, true, css);
  EXPECT_EQ(1,
            statistics()->GetVariable(CssInlineFilter::kNumCssInlined)->Get());
}

TEST_F(CssInlineFilterTest, DoNotInlineCssDifferentDomain) {
  // Note: This only fails because we haven't authorized unauth.com
  GoogleUrl gurl("http://unauth.com/styles.css");
  TestNoInlineCss("http://www.example.com/index.html", gurl.Spec().as_string(),
                  "", "BODY { color: red; }\n", "",
                  rewrite_driver()->GenerateUnauthorizedDomainDebugComment(
                      gurl, RewriteDriver::InputRole::kStyle));
  EXPECT_EQ(0,
            statistics()->GetVariable(CssInlineFilter::kNumCssInlined)->Get());
}

TEST_F(CssInlineFilterTest, CorrectlyInlineCssWithImports) {
  TestInlineCss("http://www.example.com/index.html",
                "http://www.example.com/dir/styles.css", "",
                "@import \"foo.css\"; BODY { color: red; }\n", true,
                "@import \"dir/foo.css\"; BODY { color: red; }\n");
}

// https://github.com/apache/incubator-pagespeed-mod/issues/252
TEST_F(CssInlineFilterTest, ClaimsXhtmlButHasUnclosedLink) {
  // XHTML text should not have unclosed links.  But if they do, like
  // in Issue 252, then we should leave them alone.
  static const char html_format[] =
      "<head>\n"
      "  %s\n"
      "  %s\n"
      "  <script type='text/javascript' src='c.js'></script>"  // 'in' <link>
      "</head>\n"
      "<body><div class=\"c1\"><div class=\"c2\"><p>\n"
      "  Yellow on Blue</p></div></div></body>";

  static const char unclosed_css[] =
      "  <link rel='stylesheet' href='a.css' type='text/css'>\n";  // unclosed
  static const char inlined_css[] = "  <style>.a {}</style>\n";

  // Put original CSS files into our fetcher.
  ResponseHeaders default_css_header;
  SetDefaultLongCacheHeaders(&kContentTypeCss, &default_css_header);
  SetFetchResponse(StrCat(kTestDomain, "a.css"), default_css_header, ".a {}");
  AddFilter(RewriteOptions::kInlineCss);
  ValidateExpected("claims_xhtml_but_has_unclosed_links",
                   absl::StrFormat(html_format, kXhtmlDtd, unclosed_css),
                   absl::StrFormat(html_format, kXhtmlDtd, inlined_css));
}

TEST_F(CssInlineFilterTest, DontInlineInNoscript) {
  options()->EnableFilter(RewriteOptions::kInlineCss);
  rewrite_driver()->AddFilters();

  const char kCssUrl[] = "a.css";
  const char kCss[] = "div {display:block;}";

  SetResponseWithDefaultHeaders(kCssUrl, kContentTypeCss, kCss, 3000);

  GoogleString html_input = StrCat("<noscript><link rel=stylesheet href=\"",
                                   kCssUrl, "\"></noscript>");

  ValidateNoChanges("noscript_noinline", html_input);
}

class CssInlineAndPriotizeFilterTest : public CssInlineFilterTest {
 public:
  void SetUp() override {
    CssInlineFilterTest::SetUp();

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
  }
};

TEST_F(CssInlineAndPriotizeFilterTest, InlineAndPrioritizeCss) {
  // Make sure we interact with Critical CSS properly, including in cached
  // case.
  options()->EnableFilter(RewriteOptions::kInlineCss);
  options()->EnableFilter(RewriteOptions::kPrioritizeCriticalCss);
  rewrite_driver()->AddFilters();

  const char kCssUrl[] = "a.css";
  const char kCss[] = "div {display:block;}";
  const char kMinCss[] = "div{display:block}";

  SetResponseWithDefaultHeaders(kCssUrl, kContentTypeCss, kCss, 3000);

  GoogleString html_input =
      StrCat("<link rel=stylesheet href=\"", kCssUrl, "\">");
  GoogleString html_output = StrCat("<style>", kMinCss, "</style>");

  ValidateExpected("inline_prioritize", html_input, html_output);
}

TEST_F(CssInlineFilterTest, InlineCombined) {
  // Make sure we interact with CSS combiner properly, including in cached
  // case.
  options()->EnableFilter(RewriteOptions::kInlineCss);
  options()->EnableFilter(RewriteOptions::kCombineCss);
  rewrite_driver()->AddFilters();

  const char kCssUrl[] = "a.css";
  const char kCss[] = "div {display:block;}";

  SetResponseWithDefaultHeaders(kCssUrl, kContentTypeCss, kCss, 3000);

  GoogleString html_input =
      StrCat("<link rel=stylesheet href=\"", kCssUrl, "\">",
             "<link rel=stylesheet href=\"", kCssUrl, "\">");
  GoogleString html_output = StrCat("<style>", kCss, "\n", kCss, "</style>");

  ValidateExpected("inline_combined", html_input, html_output);
  ValidateExpected("inline_combined", html_input, html_output);
}

TEST_F(CssInlineFilterTest, InlineMinimizeInteraction) {
  // There was a bug in async mode where we would accidentally prevent
  // minification results from rendering when inlining was not to be done.
  options()->EnableFilter(RewriteOptions::kRewriteCss);
  options()->set_css_inline_max_bytes(4);

  TestInlineCssWithOutputUrl(
      StrCat(kTestDomain, "minimize_but_not_inline.html"), "",
      StrCat(kTestDomain, "a.css"),
      // Note: Original URL was absolute, so rewritten one is as well.
      Encode(kTestDomain, "cf", "0", "a.css", "css"),
      "", /* no other attributes*/
      "div{display: none;}", false, "div{display: none}",
      "CSS not inlined since it&#39;s bigger than 4 bytes");
}

TEST_F(CssInlineFilterTest, InlineCacheExtendInteraction) {
  options()->set_css_inline_max_bytes(400);
  options()->EnableFilter(RewriteOptions::kInlineCss);
  options()->EnableFilter(RewriteOptions::kExtendCacheCss);
  rewrite_driver()->AddFilters();
  const char kCssUrl[] = "a.css";
  const char kCss[] = "div {display:block;}";

  SetResponseWithDefaultHeaders(kCssUrl, kContentTypeCss, kCss, 3000);

  ValidateExpected("inline_plus_ce", CssLinkHref(kCssUrl),
                   StrCat("<style>", kCss, "</style>"));

  // Cache extender should not have successfully produced an output on this
  // CSS, as it got inlined --- in the past it would have.
  EXPECT_EQ(0, rewrite_driver()
                   ->statistics()
                   ->GetVariable(CacheExtender::kCacheExtensions)
                   ->Get());

  // Now try again (as this should be hitting cache hit paths for the inliner).
  ValidateExpected("inline_plus_ce", CssLinkHref(kCssUrl),
                   StrCat("<style>", kCss, "</style>"));

  EXPECT_EQ(0, rewrite_driver()
                   ->statistics()
                   ->GetVariable(CacheExtender::kCacheExtensions)
                   ->Get());
}

TEST_F(CssInlineFilterTest, InlineCacheExtendInteractionRepeated) {
  // As above, but also with a repeated link
  options()->set_css_inline_max_bytes(400);
  options()->EnableFilter(RewriteOptions::kInlineCss);
  options()->EnableFilter(RewriteOptions::kExtendCacheCss);
  rewrite_driver()->AddFilters();
  const char kCssUrl[] = "a.css";
  const char kCss[] = "div {display:block;}";

  SetResponseWithDefaultHeaders(kCssUrl, kContentTypeCss, kCss, 3000);

  GoogleString inlined_css = StrCat("<style>", kCss, "</style>");

  ValidateExpected("inline_plus_ce_repeated",
                   StrCat(CssLinkHref(kCssUrl), CssLinkHref(kCssUrl)),
                   StrCat(inlined_css, inlined_css));

  // Cache extender should not have successfully produced an output on this
  // CSS, as it got inlined --- in the past it would have.
  EXPECT_EQ(0, rewrite_driver()
                   ->statistics()
                   ->GetVariable(CacheExtender::kCacheExtensions)
                   ->Get());

  // Now try again (as this should be hitting cache hit paths for the inliner).
  ValidateExpected("inline_plus_ce_repeated",
                   StrCat(CssLinkHref(kCssUrl), CssLinkHref(kCssUrl)),
                   StrCat(inlined_css, inlined_css));

  EXPECT_EQ(0, rewrite_driver()
                   ->statistics()
                   ->GetVariable(CacheExtender::kCacheExtensions)
                   ->Get());
}

TEST_F(CssInlineFilterTest, CharsetDetermination) {
  // Sigh. rewrite_filter.cc doesn't have its own unit test so we test this
  // method here since we're the only ones that use it.
  GoogleString x_css_url = "x.css";
  GoogleString y_css_url = "y.css";
  GoogleString z_css_url = "z.css";
  const char x_css_body[] = "BODY { color: red; }";
  const char y_css_body[] = "BODY { color: green; }";
  const char z_css_body[] = "BODY { color: blue; }";
  GoogleString y_bom_body = StrCat(kUtf8Bom, y_css_body);
  GoogleString z_bom_body = StrCat(kUtf8Bom, z_css_body);

  // x.css has no charset header nor a BOM.
  // y.css has no charset header but has a BOM.
  // z.css has a charset header and a BOM.
  ResponseHeaders default_header;
  SetDefaultLongCacheHeaders(&kContentTypeJavascript, &default_header);
  SetFetchResponse(StrCat(kTestDomain, x_css_url), default_header, x_css_body);
  SetFetchResponse(StrCat(kTestDomain, y_css_url), default_header, y_bom_body);
  default_header.MergeContentType("text/css; charset=iso-8859-1");
  SetFetchResponse(StrCat(kTestDomain, z_css_url), default_header, z_bom_body);

  ResourcePtr x_css_resource(CreateResource(kTestDomain, x_css_url));
  ResourcePtr y_css_resource(CreateResource(kTestDomain, y_css_url));
  ResourcePtr z_css_resource(CreateResource(kTestDomain, z_css_url));
  EXPECT_TRUE(ReadIfCached(x_css_resource));
  EXPECT_TRUE(ReadIfCached(y_css_resource));
  EXPECT_TRUE(ReadIfCached(z_css_resource));

  GoogleString result;
  const StringPiece kUsAsciiCharset("us-ascii");

  // Nothing set: charset should be empty.
  result = RewriteFilter::GetCharsetForStylesheet(x_css_resource.get(), "", "");
  EXPECT_TRUE(result.empty());

  // Only the containing charset is set.
  result = RewriteFilter::GetCharsetForStylesheet(x_css_resource.get(), "",
                                                  kUsAsciiCharset);
  EXPECT_STREQ(result, kUsAsciiCharset);

  // The containing charset is trumped by the element's charset attribute.
  result = RewriteFilter::GetCharsetForStylesheet(x_css_resource.get(), "gb",
                                                  kUsAsciiCharset);
  EXPECT_STREQ("gb", result);

  // The element's charset attribute is trumped by the resource's BOM.
  result = RewriteFilter::GetCharsetForStylesheet(y_css_resource.get(), "gb",
                                                  kUsAsciiCharset);
  EXPECT_STREQ("utf-8", result);

  // The resource's BOM is trumped by the resource's header.
  result = RewriteFilter::GetCharsetForStylesheet(z_css_resource.get(), "gb",
                                                  kUsAsciiCharset);
  EXPECT_STREQ("iso-8859-1", result);
}

TEST_F(CssInlineFilterTest, InlineWithCompatibleBom) {
  const GoogleString css = "BODY { color: red; }\n";
  const GoogleString css_with_bom = StrCat(kUtf8Bom, css);
  TestInlineCssWithOutputUrl(
      "http://www.example.com/index.html", "  <meta charset=\"UTF-8\">\n",
      "http://www.example.com/styles.css", "http://www.example.com/styles.css",
      "", css_with_bom, true, css, "");
}

TEST_F(CssInlineFilterTest, DoNotInlineWithIncompatibleBomAndNonAscii) {
  const GoogleString css = "BODY { color: red; /* \xD2\x90 */ }\n";
  const GoogleString css_with_bom = StrCat(kUtf8Bom, css);
  TestInlineCssWithOutputUrl(
      "http://www.example.com/index.html", "  <meta charset=\"ISO-8859-1\">\n",
      "http://www.example.com/styles.css", "http://www.example.com/styles.css",
      "", css_with_bom, false, "",
      "CSS not inlined due to apparent charset "
      "incompatibility; we think the HTML is ISO-8859-1 "
      "while the CSS is utf-8");
}

TEST_F(CssInlineFilterTest, DoInlineWithIncompatibleBomAndAscii) {
  // Even though the content is labeled utf-8, it keeps to ASCII subset, so it's
  // safe to inline.
  const GoogleString css = "BODY { color: red; }\n";
  const GoogleString css_with_bom = StrCat(kUtf8Bom, css);
  TestInlineCssWithOutputUrl(
      "http://www.example.com/index.html", "  <meta charset=\"ISO-8859-1\">\n",
      "http://www.example.com/styles.css", "http://www.example.com/styles.css",
      "", css_with_bom, true, css, "");
}

// Two pages in different charsets share one stylesheet whose charset is given
// by the <link charset=> attribute. Whichever page is rewritten first, the
// utf-8 page gets the stylesheet inlined and the gbk page keeps it external:
// the answer is not carried from one page to the other through the metadata
// cache.
TEST_F(CssInlineFilterTest,
       SharedStylesheetDecidedPerPageCharsetInEitherOrder) {
  AddFilter(RewriteOptions::kInlineCss);
  const GoogleString css = "a:after{content:'\xE4\xB8\xAD'}";
  SetResponseWithDefaultHeaders(StrCat(kTestDomain, "shared.css"),
                                kContentTypeCss, css, 3000);
  const char kUtf8Page[] = "<meta charset=\"utf-8\">";
  const char kGbkPage[] = "<meta charset=\"gbk\">";
  const char kLink[] = "<link rel=stylesheet href=shared.css charset=utf-8>";
  const GoogleString inlined = StrCat("<style charset=utf-8>", css, "</style>");

  ValidateExpected("utf8_first", StrCat(kUtf8Page, kLink),
                   StrCat(kUtf8Page, inlined));
  ValidateNoChanges("gbk_second", StrCat(kGbkPage, kLink));

  lru_cache()->Clear();
  ValidateNoChanges("gbk_first", StrCat(kGbkPage, kLink));
  ValidateExpected("utf8_second", StrCat(kUtf8Page, kLink),
                   StrCat(kUtf8Page, inlined));
}

// Here the stylesheet's charset comes from its Content-Type, and the stylesheet
// is minified before the inliner looks at it. The page rewritten second gets
// the minified stylesheet rebuilt from the metadata cache; it must still carry
// that charset, so that a page in another charset is decided on its own.
TEST_F(CssInlineFilterTest, MinifiedStylesheetKeepsItsCharsetForEachPage) {
  options()->EnableFilter(RewriteOptions::kRewriteCss);
  options()->EnableFilter(RewriteOptions::kInlineCss);
  rewrite_driver()->AddFilters();

  ResponseHeaders utf8_headers;
  SetDefaultLongCacheHeaders(&kContentTypeCss, &utf8_headers);
  utf8_headers.MergeContentType("text/css; charset=UTF-8");
  const GoogleString css = "a:after { content: '\xE4\xB8\xAD'; }\n";
  SetFetchResponse(StrCat(kTestDomain, "shared.css"), utf8_headers, css);
  const char kUtf8Page[] =
      "<meta charset=\"utf-8\"><link rel=stylesheet href=shared.css>";
  const char kGbkPage[] =
      "<meta charset=\"gbk\"><link rel=stylesheet href=shared.css>";

  // The utf-8 page gets the stylesheet inlined; the gbk page keeps a link.
  Parse("utf8_first", kUtf8Page);
  EXPECT_EQ(GoogleString::npos, output_buffer_.find("href=")) << output_buffer_;
  Parse("gbk_second", kGbkPage);
  EXPECT_NE(GoogleString::npos, output_buffer_.find("href=")) << output_buffer_;

  lru_cache()->Clear();
  Parse("gbk_first", kGbkPage);
  EXPECT_NE(GoogleString::npos, output_buffer_.find("href=")) << output_buffer_;
  Parse("utf8_second", kUtf8Page);
  EXPECT_EQ(GoogleString::npos, output_buffer_.find("href=")) << output_buffer_;
}

// See: http://www.alistapart.com/articles/alternate/
//  and http://www.w3.org/TR/html4/present/styles.html#h-14.3.1
TEST_F(CssInlineFilterTest, AlternateStylesheet) {
  AddFilter(RewriteOptions::kInlineCss);
  SetResponseWithDefaultHeaders("foo.css", kContentTypeCss, "a{margin:0}", 100);

  // Normal (persistent) CSS links are inlined.
  ValidateExpected("persistent", "<link rel='stylesheet' href='foo.css'>",
                   "<style>a{margin:0}</style>");

  // Make sure we accept mixed case for the keyword.
  ValidateExpected("mixed_case", "<link rel=' StyleSheet ' href='foo.css'>",
                   "<style>a{margin:0}</style>");

  // Preferred CSS links are not because inline styles cannot be given
  // a title (AFAICT).  The title attribute indicates that the given
  // CSS can be overridden by an alternate style sheet.
  ValidateNoChanges("preferred",
                    "<link rel='stylesheet' href='foo.css' title='foo'>");

  // Alternate CSS links, likewise.
  ValidateNoChanges(
      "alternate",
      "<link rel='alternate stylesheet' href='foo.css' title='foo'>");
}

TEST_F(CssInlineFilterTest, CarryAcrossOtherAttributes) {
  // Carry across attributes such as id and class to the inlined style tag.
  AddFilter(RewriteOptions::kInlineCss);
  SetResponseWithDefaultHeaders("foo.css", kContentTypeCss, "a{margin:0}", 100);

  ValidateExpected(
      "CarryAcross",
      "<link rel='stylesheet' href='foo.css' id='my-stylesheet' class='a b c'"
      " lulz='!@$@#$%@4lulz'>",
      "<style id='my-stylesheet' class='a b c' lulz='!@$@#$%@4lulz'>"
      "a{margin:0}</style>");

  // But respect pagespeed_no_transform
  ValidateNoChanges(
      "NoTransform",
      "<link rel='stylesheet' href='foo.css' id='my-stylesheet' class='a b c' "
      "pagespeed_no_transform>");
  ValidateNoChanges(
      "NoTransform",
      "<link rel='stylesheet' href='foo.css' id='my-stylesheet' class='a b c' "
      "data-pagespeed-no-transform>");
}

TEST_F(CssInlineFilterTest, NoRel) {
  AddFilter(RewriteOptions::kInlineCss);
  SetResponseWithDefaultHeaders("foo.css", kContentTypeCss, "a{margin:0}", 100);

  // We don't mess with links that lack rel attributes.
  ValidateNoChanges("no_rel", "<link href='foo.css'>");
}

TEST_F(CssInlineFilterTest, NonCss) {
  AddFilter(RewriteOptions::kInlineCss);
  SetResponseWithDefaultHeaders("foo.xsl", kContentTypeXml,
                                "<xsl:variable name='foo' select='bar'>", 100);

  ValidateNoChanges("non_css",
                    "<link rel='stylesheet' href='foo.xsl' type='text/xsl'/>");
}

TEST_F(CssInlineFilterTest, NoInliningOfCloseStyleTag) {
  VerifyNoInliningForClosingStyleTag("</style>");
}

TEST_F(CssInlineFilterTest, NoInliningOfCloseStyleTagWithCapitalization) {
  VerifyNoInliningForClosingStyleTag("</Style>");
}

TEST_F(CssInlineFilterTest, NoInliningOfCloseStyleTagWithSpaces) {
  VerifyNoInliningForClosingStyleTag("</style abc>");
}

// A stylesheet with a tag in it stays external.
TEST_F(CssInlineFilterTest, NoInliningOfAnyTag) {
  VerifyNoInliningForClosingStyleTag("td[headers=\"<x\"]{padding:3px}");
}

TEST_F(CssInlineFilterTest, StylesheetWithoutMarkupIsInlined) {
  AddFilter(RewriteOptions::kInlineCss);
  SetResponseWithDefaultHeaders("foo.css", kContentTypeCss, "a>b{margin:0}",
                                100);
  ValidateExpected("inlined", "<link rel='stylesheet' href='foo.css'>",
                   "<style>a>b{margin:0}</style>");
}

TEST_F(CssInlineFilterTest, DisabledForAmp) {
  AddFilter(RewriteOptions::kInlineCss);
  SetResponseWithDefaultHeaders("foo.css", kContentTypeCss,
                                "/* pretend there is a @font-face here */",
                                100);
  TurnOnDebug();
  ValidateExpected(
      "no_inlining_in_amp", "<html amp><link rel='stylesheet' href='foo.css'>",
      "<html amp><link rel='stylesheet' href='foo.css'>"
      "<!--CSS inlining not supported by PageSpeed for AMP documents-->");

  // Make sure same stylesheet gets inlined elsewhere.
  ValidateExpected("same_url_in_non_amp",
                   "<link rel='stylesheet' href='foo.css'>",
                   "<style>/* pretend there is a @font-face here */</style>");
}

TEST_F(CssInlineFilterTest, CheckInliningOfLinkStyleTagInBodyPedantic) {
  options()->EnableFilter(RewriteOptions::kInlineCss);
  options()->EnableFilter(RewriteOptions::kPedantic);
  rewrite_driver()->AddFilters();
  SetResponseWithDefaultHeaders("foo.css", kContentTypeCss,
                                "/* pretend there is a @font-face here */",
                                100);
  TurnOnDebug();

  // dont inline css for style link element in body
  // when pedantic is enabled AND move_css_to_head is not enabled.
  ValidateExpected(
      "check_inlining_for_link_tag_in_body_pedantic1",
      "<html><head></head><body><link property='stylesheet'"
      "rel='stylesheet' href='foo.css'></body></html>",
      "<html><head></head><body><link property='stylesheet' rel='stylesheet' "
      "href='foo.css'><!--CSS not inlined because style link element "
      "in html body--></body></html>");

  // inline css for style link element in head, pedantic enabled
  ValidateExpected("check_inlining_for_link_tag_in_body_pedantic2",
                   "<html><head><link property='stylesheet'rel='stylesheet' "
                   "href='foo.css'></head><body></body></html>",
                   "<html><head><style property='stylesheet' type=\"text/css\">"
                   "/* pretend there is a @font-face here */</style></head>"
                   "<body></body></html>");
}

TEST_F(CssInlineFilterTest, InliningOfLinkStyleTagInBody) {
  options()->EnableFilter(RewriteOptions::kInlineCss);
  options()->EnableFilter(RewriteOptions::kPedantic);
  options()->EnableFilter(RewriteOptions::kMoveCssToHead);
  rewrite_driver()->AddFilters();
  SetResponseWithDefaultHeaders("foo.css", kContentTypeCss,
                                "/* pretend there is a @font-face here */",
                                100);
  TurnOnDebug();

  // inline css for style link element in body,
  // with pedantic AND move_css_to_head enabled.
  // inlined css will be moved to head
  ValidateExpected("inlining_for_link_tag_in_body",
                   "<html><head></head><body><link property='stylesheet'"
                   "rel='stylesheet' href='foo.css'></body></html>",
                   "<html><head><style property='stylesheet' type=\"text/css\">"
                   "/* pretend there is a @font-face here */</style></head>"
                   "<body></body></html>");
}

TEST_F(CssInlineFilterTest, CheckInliningOfLinkStyleTagInBodyNonPedantic) {
  options()->EnableFilter(RewriteOptions::kInlineCss);
  rewrite_driver()->AddFilters();
  SetResponseWithDefaultHeaders("foo.css", kContentTypeCss,
                                "/* pretend there is a @font-face here */",
                                100);
  TurnOnDebug();

  // inline css for style link element in body
  // inlining is done in body since move_css_to_head,pedantic is not enabled.
  ValidateExpected(
      "check_inlining_for_link_tag_in_body_non_pedantic1",
      "<html><head></head><body><link property='stylesheet'"
      "rel='stylesheet' href='foo.css'></body></html>",
      "<html><head></head><body><style property='stylesheet'>"
      "/* pretend there is a @font-face here */</style></body></html>");

  // inline css for style link element in head
  // inlining is done in html head.
  ValidateExpected("check_inlining_for_link_tag_in_body_non_pedantic2",
                   "<html><head><link property='stylesheet'rel='stylesheet' "
                   "href='foo.css'></head><body></body></html>",
                   "<html><head><style property='stylesheet'>"
                   "/* pretend there is a @font-face here */</style>"
                   "</head><body></body></html>");
}

TEST_F(CssInlineFilterTest, BasicCsp) {
  AddFilter(RewriteOptions::kInlineCss);
  SetResponseWithDefaultHeaders("a.css", kContentTypeCss, "a{margin:0}", 100);
  TurnOnDebug();

  const char kCspNoInline[] =
      "<meta http-equiv=\"Content-Security-Policy\" content=\"style-src *;\">";
  const char kCspYesInline[] =
      "<meta http-equiv=\"Content-Security-Policy\" "
      "content=\"style-src * 'unsafe-inline';\">";

  ValidateExpected("no_inline_csp", StrCat(kCspNoInline, CssLinkHref("a.css")),
                   StrCat(kCspNoInline, CssLinkHref("a.css"),
                          "<!--PageSpeed output (by ci) not permitted by "
                          "Content Security Policy-->"));
  ValidateExpected("yes_inline_csp",
                   StrCat(kCspYesInline, CssLinkHref("a.css")),
                   StrCat(kCspYesInline, "<style>a{margin:0}</style>"));
}

// On an HTML page a stylesheet holding any markup stays external, wherever
// its link is.
class CssInlineMarkupPlacementTest : public CssInlineFilterTest {
 protected:
  void SetUp() override {
    CssInlineFilterTest::SetUp();
    SetHtmlMimetype();
    AddFilter(RewriteOptions::kInlineCss);
  }

  // Each call uses its own stylesheet URL, so no result is shared.
  void ExpectExternal(const char* id, const char* css, const char* before,
                      const char* after) {
    const GoogleString url = StrCat(id, ".css");
    SetResponseWithDefaultHeaders(url, kContentTypeCss, css, 100);
    ValidateNoChanges(id, StrCat(before, "<link rel='stylesheet' href='", url,
                                 "'>", after));
  }
};

TEST_F(CssInlineMarkupPlacementTest, ClosingTag) {
  ExpectExternal("noembed", "a{margin:0}/*</noembed>*/", "<noembed>",
                 "</noembed>");
  ExpectExternal("noframes", "a{margin:0}/*</noframes>*/", "<noframes>",
                 "</noframes>");
  ExpectExternal("textarea", "a{margin:0}/*</textarea>*/", "<textarea/>", "");
}

TEST_F(CssInlineMarkupPlacementTest, StartTag) {
  ExpectExternal("select", "a{margin:0}/*<input>*/", "<select>", "</select>");
  ExpectExternal("frameset", "a{margin:0}/*<frame>*/", "<frameset>",
                 "</frameset>");
}

TEST_F(CssInlineMarkupPlacementTest, CommentOpener) {
  ExpectExternal("comment", "a{margin:0}/*<!--*/", "", "");
}

// Stylesheets with a '<' that cannot be read as markup, on HTML pages that
// declare a supported charset.
class CssInlineLessThanTest : public CssInlineFilterTest {
 protected:
  void SetPageType(StringPiece content_type) {
    rewrite_driver()->set_response_headers_ptr(&response_headers_);
    response_headers_.Replace(HttpAttributes::kContentType, content_type);
    response_headers_.ComputeCaching();
  }

  // Inlines `css` from a link between `before` and `after`. Returns the text
  // of the <style> written in its place, or "UNCHANGED" when the page was
  // left as it was. Each call uses its own stylesheet URL.
  GoogleString Inline(StringPiece id, StringPiece css, StringPiece before = "",
                      StringPiece after = "") {
    if (!filter_added_) {
      AddFilter(RewriteOptions::kInlineCss);
      filter_added_ = true;
    }
    const GoogleString url = StrCat(id, ".css");
    SetResponseWithDefaultHeaders(url, kContentTypeCss, css, 100);
    const GoogleString input =
        StrCat(before, "<link rel='stylesheet' href='", url, "'>", after);
    // The page is at the top of the site, whatever directory the
    // stylesheet is in.
    GoogleString page_id(id);
    std::replace(page_id.begin(), page_id.end(), '/', '_');
    Parse(page_id, input);
    if (output_buffer_ == input) {
      return "UNCHANGED";
    }
    const size_t start = output_buffer_.find("<style>");
    const size_t end = output_buffer_.find("</style>");
    EXPECT_NE(GoogleString::npos, start) << output_buffer_;
    EXPECT_NE(GoogleString::npos, end) << output_buffer_;
    if (start == GoogleString::npos || end == GoogleString::npos) {
      return "NO STYLE";
    }
    EXPECT_EQ(StrCat(before, "<style>"), output_buffer_.substr(0, start + 7));
    EXPECT_EQ(StrCat("</style>", after), output_buffer_.substr(end));
    return output_buffer_.substr(start + 7, end - start - 7);
  }

  // What InlineStyleText makes of `css` for an HTML page.
  static GoogleString Prepared(StringPiece css) {
    InlineStyleText out;
    EXPECT_TRUE(InlineStyleText::Prepare(css, InlineStyleMode::kHtml, &out,
                                         nullptr))
        << css;
    return out.text();
  }

  bool filter_added_ = false;
};

const char kLessThanCss[] =
    "@property --tw-shadow-alpha{syntax:\"<percentage>\";inherits:false;"
    "initial-value:100%}"
    "@media (width < 600px){a{margin:0}}"
    "@media (width<48rem){b{margin:0}}"
    "td[headers=\"<x\"]{padding:3px}";

TEST_F(CssInlineLessThanTest, StylesheetWithLessThanSignIsInlined) {
  SetPageType("text/html; charset=utf-8");
  const GoogleString text = Inline("lt", kLessThanCss);
  EXPECT_EQ(
      "@property --tw-shadow-alpha{syntax:\"\\3c percentage>\";"
      "inherits:false;initial-value:100%}"
      "@media (width < 600px){a{margin:0}}"
      "@media (width<48rem){b{margin:0}}"
      "td[headers=\"\\3c x\"]{padding:3px}",
      text);
  EXPECT_TRUE(IsSafeInlineStyleText(text, InlineStyleMode::kHtml));
}

// Only a page that declares a supported charset gets it.
TEST_F(CssInlineLessThanTest, LessThanSignNeedsADeclaredSupportedCharset) {
  SetPageType("text/html");
  EXPECT_EQ("UNCHANGED", Inline("none", kLessThanCss));
  SetPageType("text/html; charset=shift_jis");
  EXPECT_EQ("UNCHANGED", Inline("sjis", kLessThanCss));
  SetPageType("text/html; charset=iso-2022-jp");
  EXPECT_EQ("UNCHANGED", Inline("jis", kLessThanCss));
  SetPageType("text/html; charset=windows-1252");
  EXPECT_EQ(Prepared(kLessThanCss), Inline("cp1252", kLessThanCss));
  SetPageType("text/html; charset=ISO-8859-1");
  EXPECT_EQ(Prepared(kLessThanCss), Inline("latin1", kLessThanCss));
}

TEST_F(CssInlineLessThanTest, MetaCharsetBeforeTheLinkCounts) {
  SetPageType("text/html");
  EXPECT_EQ(Prepared(kLessThanCss),
            Inline("meta_before", kLessThanCss, "<meta charset=\"utf-8\">"));
  EXPECT_EQ("UNCHANGED",
            Inline("meta_after", kLessThanCss, "", "<meta charset=\"utf-8\">"));
}

TEST_F(CssInlineLessThanTest, XhtmlWithLessThanSignIsNotInlined) {
  SetPageType("application/xhtml+xml; charset=utf-8");
  EXPECT_EQ("UNCHANGED", Inline("xhtml", kLessThanCss));
}

// The decision is cached per kind of page.
TEST_F(CssInlineLessThanTest, KindOfPageIsPartOfTheCachedDecision) {
  SetPageType("text/html");
  EXPECT_EQ("UNCHANGED", Inline("shared", kLessThanCss));
  SetPageType("text/html; charset=utf-8");
  EXPECT_EQ(Prepared(kLessThanCss), Inline("shared", kLessThanCss));
  SetPageType("text/html");
  EXPECT_EQ("UNCHANGED", Inline("shared", kLessThanCss));
}

// The size limit applies to the text as written.
TEST_F(CssInlineLessThanTest, LimitAppliesToTheWrittenText) {
  const char kCss[] = "a{content:\"<<<<\"}";  // 17 bytes, 29 written
  options()->set_css_inline_max_bytes(20);
  SetPageType("text/html; charset=utf-8");
  EXPECT_EQ("UNCHANGED", Inline("limit", kCss));
}

// The text is prepared after its URLs are resolved against the page.
TEST_F(CssInlineLessThanTest, TextIsPreparedAfterUrlResolution) {
  SetPageType("text/html; charset=utf-8");
  EXPECT_EQ("a{background:url(dir/b.png)}b{content:\"\\3c x\"}",
            Inline("dir/resolved",
                   "a{background:url(b.png)}b{content:\"<x\"}"));
}

// Where a link can be, and text that would be markup there. Covers every
// placement and payload of CssInlineMarkupPlacementTest and of the matching
// critical CSS and inline style tests.
struct MarkupPlacement {
  const char* id;
  const char* before;
  const char* after;
  const char* end_tag;  // its own end tag, as a payload
};
const MarkupPlacement kMarkupPlacements[] = {
    {"top", "", "", "</head>"},
    {"noembed", "<noembed>", "</noembed>", "</noembed>"},
    {"noframes", "<noframes>", "</noframes>", "</noframes>"},
    {"textarea", "<textarea/>", "", "</textarea>"},
    {"select", "<select>", "</select>", "</select>"},
    {"frameset", "<frameset>", "</frameset>", "</frameset>"},
    {"noscript", "<noscript>", "</noscript>", "</noscript>"},
};
const char* const kMarkupPayloads[] = {"", "</style>", "</STYLE >", "<input>",
                                       "<frame>", "<!--"};

// On a page that declares UTF-8: a payload in a comment or a string is
// written so it is no longer markup (or the link is left alone), wherever the
// link is; the same payload outside them keeps the link.
TEST_F(CssInlineLessThanTest, MarkupPlacementsAreInlinedSafelyOrKept) {
  SetPageType("text/html; charset=utf-8");
  int n = 0;
  for (const MarkupPlacement& p : kMarkupPlacements) {
    for (const char* payload_or_empty : kMarkupPayloads) {
      const StringPiece payload =
          *payload_or_empty == '\0' ? p.end_tag : payload_or_empty;
      const GoogleString kept[] = {
          StrCat("a{margin:0}/*", payload, "*/"),
          StrCat("a{content:\"", payload, "\"}"),
      };
      for (const GoogleString& css : kept) {
        const GoogleString text =
            Inline(StrCat("k", IntegerToString(n++)), css, p.before, p.after);
        if (text != "UNCHANGED") {
          EXPECT_TRUE(IsSafeInlineStyleText(text, InlineStyleMode::kHtml))
              << p.id << ": " << text;
          EXPECT_EQ(Prepared(css), text) << p.id;
          EXPECT_EQ(GoogleString::npos, text.find('<')) << p.id << ": " << text;
        } else {
          // Only a link the filter never inlines is left alone.
          EXPECT_STREQ("noscript", p.id) << css;
        }
      }
      EXPECT_EQ("UNCHANGED", Inline(StrCat("b", IntegerToString(n++)),
                                    StrCat("a{margin:0}", payload), p.before,
                                    p.after))
          << p.id << ": " << payload;
    }
  }
}

// Only the first meta tag that declares a charset counts, and only within the
// first 1024 bytes of the document.
TEST_F(CssInlineLessThanTest, OnlyAnEarlyFirstMetaCharsetCounts) {
  SetPageType("text/html");
  const GoogleString filler = StrCat("<!--", GoogleString(1100, 'x'), "-->");
  EXPECT_EQ("UNCHANGED",
            Inline("late_meta", kLessThanCss,
                   StrCat(filler, "<meta charset=\"utf-8\">")));
  EXPECT_EQ("UNCHANGED",
            Inline("second_meta", kLessThanCss,
                   "<meta charset=\"shift_jis\"><meta charset=\"utf-8\">"));
  EXPECT_EQ("UNCHANGED", Inline("unknown_label", kLessThanCss,
                                "<meta charset=\"iso-8859-12\">"));
  EXPECT_EQ(Prepared(kLessThanCss),
            Inline("first_with_charset", kLessThanCss,
                   "<meta name=\"a\" content=\"b\"><meta charset=\"utf-8\">"));
  EXPECT_EQ(Prepared(kLessThanCss),
            Inline("http_equiv", kLessThanCss,
                   "<meta http-equiv=\"Content-Type\" "
                   "content=\"text/html; charset=windows-1252\">"));
  // The response headers come first.
  SetPageType("text/html; charset=shift_jis");
  EXPECT_EQ("UNCHANGED", Inline("header_first", kLessThanCss,
                                "<meta charset=\"utf-8\">"));
}

// A byte order mark other than UTF-8's keeps the strict rule; one alone
// declares nothing.
TEST_F(CssInlineLessThanTest, ByteOrderMark) {
  SetPageType("text/html; charset=utf-8");
  EXPECT_EQ("UNCHANGED", Inline("utf16le", kLessThanCss, "\xff\xfe"));
  EXPECT_EQ("UNCHANGED", Inline("utf16be", kLessThanCss, "\xfe\xff"));
  EXPECT_EQ(Prepared(kLessThanCss), Inline("utf8", kLessThanCss, "\xef\xbb\xbf"));
  SetPageType("text/html");
  EXPECT_EQ("UNCHANGED", Inline("utf8_alone", kLessThanCss, "\xef\xbb\xbf"));
}

// The output of Tailwind CSS 4 (tailwindcss 4.3.5, unminified, for a page
// using a few utilities and variants).
TEST_F(CssInlineLessThanTest, TailwindV4StylesheetIsInlined) {
  StdioFileSystem stdio_file_system;
  GoogleString css;
  ASSERT_TRUE(stdio_file_system.ReadFile(
      StrCat(GTestSrcDir(), kTestData, "tailwind_v4.css").c_str(), &css,
      message_handler()));
  ASSERT_NE(GoogleString::npos, css.find("syntax: \"<percentage>\""));
  ASSERT_NE(GoogleString::npos, css.find("@media (width < 48rem)"));
  options()->set_css_inline_max_bytes(1 << 20);
  SetPageType("text/html; charset=utf-8");
  PreparedStyleTextRecorder recorder;
  const GoogleString text = Inline("tailwind", css);
  ASSERT_NE("UNCHANGED", text);
  EXPECT_TRUE(IsSafeInlineStyleText(text, InlineStyleMode::kHtml));
  EXPECT_TRUE(recorder.WasPrepared(text));
  EXPECT_NE(GoogleString::npos, text.find("syntax: \"\\3c percentage>\""));
  EXPECT_NE(GoogleString::npos, text.find("@media (width < 48rem)"));
  EXPECT_NE(GoogleString::npos, text.find("@media (width >= 40rem)"));
}

// Stylesheets for the tests that compare what is written with what was
// prepared.
const char* const kParityCss[] = {
    "a{margin:0}",
    "@property --x{syntax:\"<length>\";inherits:false;initial-value:0px}",
    "@media (width < 600px){a{margin:0}}@media (400px<=width<=700px){b{c:d}}",
    "a{content:\"</style><b>\"}/*<!-- <i> -->*/",
    ".a\\<b{color:red}a{b:url(x<y)}",
    "a{content:'<'}b{content:\"\\3c\"}",
    "a{margin:0}</style>",
    "a{margin:0}<b>",
    "@media (width<calc(1px + 1em)){a{margin:0}}",
    "<!--a{margin:0}-->",
};

// What is written is exactly what InlineStyleText prepared, and the link is
// left alone exactly when it refuses.
TEST_F(CssInlineLessThanTest, WrittenTextIsPreparedText) {
  SetPageType("text/html; charset=utf-8");
  int n = 0;
  for (const char* css : kParityCss) {
    PreparedStyleTextRecorder recorder;
    const GoogleString text = Inline(StrCat("p", IntegerToString(n++)), css);
    InlineStyleText expected;
    if (InlineStyleText::Prepare(css, InlineStyleMode::kHtml, &expected,
                                 nullptr)) {
      EXPECT_EQ(expected.text(), text) << css;
      EXPECT_TRUE(recorder.WasPrepared(text)) << css;
    } else {
      EXPECT_EQ("UNCHANGED", text) << css;
      EXPECT_LT(0, recorder.refusals()) << css;
    }
  }
}

// With response headers that are never finalized, and no Content-Type in
// them (as on Apache), the type the parse was started with decides.
class CssInlineUnfinalizedHeadersTest : public CssInlineFilterTest {
 protected:
  GoogleString InlineWithParseType(const ContentType& type) {
    server_context()->set_response_headers_finalized(false);
    AddFilter(RewriteOptions::kInlineCss);
    SetResponseWithDefaultHeaders("foo.css", kContentTypeCss,
                                  "@media (width < 600px){a{margin:0}}", 100);
    SetupWriter();
    rewrite_driver()->StartParseWithType(StrCat(kTestDomain, "x.html"), type);
    rewrite_driver()->ParseText(
        "<meta charset=\"utf-8\"><link rel='stylesheet' href='foo.css'>");
    rewrite_driver()->FinishParse();
    return output_buffer_;
  }
};

TEST_F(CssInlineUnfinalizedHeadersTest, HtmlParseInlinesLessThanSign) {
  EXPECT_EQ(
      "<meta charset=\"utf-8\"><style>@media (width < 600px){a{margin:0}}"
      "</style>",
      InlineWithParseType(kContentTypeHtml));
}

TEST_F(CssInlineUnfinalizedHeadersTest, XhtmlParseKeepsTheLink) {
  EXPECT_EQ("<meta charset=\"utf-8\"><link rel='stylesheet' href='foo.css'>",
            InlineWithParseType(kContentTypeXhtml));
}

TEST_F(CssInlineUnfinalizedHeadersTest, OtherParseTypeKeepsTheLink) {
  EXPECT_EQ("<meta charset=\"utf-8\"><link rel='stylesheet' href='foo.css'>",
            InlineWithParseType(kContentTypeXml));
}

// A link in SVG or MathML is not an HTML link element, so it is left alone.
TEST_F(CssInlineLessThanTest, ForeignLinkIsLeftAlone) {
  SetPageType("text/html; charset=utf-8");
  AddFilter(RewriteOptions::kInlineCss);
  filter_added_ = true;
  SetResponseWithDefaultHeaders("foo.css", kContentTypeCss, "a{margin:0}", 100);
  ValidateNoChanges("svg",
                    "<svg><link rel='stylesheet' href='foo.css'></svg>");
  ValidateNoChanges("math",
                    "<math><link rel='stylesheet' href='foo.css'></math>");
}

}  // namespace

}  // namespace net_instaweb
