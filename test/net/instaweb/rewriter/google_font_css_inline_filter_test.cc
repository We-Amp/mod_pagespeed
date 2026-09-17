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

// Unit tests for GoogleFontCssInlineFilter

#include "net/instaweb/rewriter/public/domain_lawyer.h"
#include "net/instaweb/rewriter/public/rewrite_driver.h"
#include "net/instaweb/rewriter/public/rewrite_options.h"
#include "net/instaweb/rewriter/public/server_context.h"
#include "pagespeed/kernel/base/ref_counted_ptr.h"
#include "pagespeed/kernel/base/string.h"
#include "pagespeed/kernel/base/string_util.h"
#include "pagespeed/kernel/base/timer.h"
#include "pagespeed/kernel/http/content_type.h"
#include "pagespeed/kernel/http/response_headers.h"
#include "pagespeed/opt/http/request_context.h"
#include "test/net/instaweb/http/ua_sensitive_test_fetcher.h"
#include "test/net/instaweb/rewriter/rewrite_test_base.h"
#include "test/pagespeed/kernel/base/gtest.h"
#include "test/pagespeed/kernel/base/mock_message_handler.h"

namespace net_instaweb {

namespace {

const char kRoboto[] = "http://fonts.googleapis.com/css?family=Roboto";
const char kRobotoSsl[] = "https://fonts.googleapis.com/css?family=Roboto";
const char kLato[] = "http://fonts.googleapis.com/css?family=Lato";

// Preconnect hints the filter inserts ahead of the font CSS; the scheme
// follows the font link's scheme.
const char kPreconnectHttp[] =
    "<link rel=\"preconnect\" href=\"http://fonts.gstatic.com\" crossorigin>";
const char kPreconnectHttps[] =
    "<link rel=\"preconnect\" href=\"https://fonts.gstatic.com\" crossorigin>";

class GoogleFontCssInlineFilterTestBase : public RewriteTestBase {
 protected:
  void SetUp() override { RewriteTestBase::SetUp(); }

  void SetUpForFontFilterTest(RewriteOptions::Filter filter_to_enable) {
    AddFilter(filter_to_enable);

    // We want to enable debug after AddFilters, since we don't want the
    // standard debug filter output, just that from the font filter.
    options()->ClearSignatureForTesting();
    options()->EnableFilter(RewriteOptions::kDebug);
    server_context()->ComputeSignature(options());
  }

  void SetFontCssResponse(StringPiece url_with_ua, StringPiece content) {
    // Font loader CSS gets Cache-Control:private, max-age=86400
    ResponseHeaders response_headers;
    SetDefaultLongCacheHeaders(&kContentTypeCss, &response_headers);
    response_headers.SetDateAndCaching(timer()->NowMs(),
                                       86400 * Timer::kSecondMs, ", private");
    SetFetchResponse(url_with_ua, response_headers, content);
  }

  void ResetUserAgent(StringPiece user_agent) {
    ClearRewriteDriver();
    rewrite_driver()->SetSessionFetcher(
        new UserAgentSensitiveTestFetcher(rewrite_driver()->async_fetcher()));

    // Now upload some UA-specific CSS where UaSensitiveFetcher will find it,
    // for two fake UAs: Chromezilla and Safieri, used since they are short,
    // unlike real UA strings.
    SetFontCssResponse(StrCat(kRoboto, "&UA=Chromezilla"), "font_chromezilla");
    SetFontCssResponse(StrCat(kRoboto, "&UA=Safieri"), "font_safieri");

    // If other filters will try to fetch this, they won't have a UA.
    SetFontCssResponse(StrCat(kRoboto, "&UA=unknown"), "font_huh");
    SetCurrentUserAgent(user_agent);
  }
};

class GoogleFontCssInlineFilterTest : public GoogleFontCssInlineFilterTestBase {
 protected:
  void SetUp() override {
    GoogleFontCssInlineFilterTestBase::SetUp();
    SetUpForFontFilterTest(RewriteOptions::kInlineGoogleFontCss);
  }
};

TEST_F(GoogleFontCssInlineFilterTest, BasicOperation) {
  ResetUserAgent("Chromezilla");
  ValidateExpected("simple", CssLinkHref(kRoboto),
                   StrCat(kPreconnectHttp, "<style>font_chromezilla</style>"));

  // Different UAs get different cache entries
  ResetUserAgent("Safieri");
  ValidateExpected("simple2", CssLinkHref(kRoboto),
                   StrCat(kPreconnectHttp, "<style>font_safieri</style>"));
}

TEST_F(GoogleFontCssInlineFilterTest, LargeCssNowInlines) {
  // Real Font Service CSS responses typically run well past the old 3K
  // default; make sure a response of realistic size inlines under the
  // default configuration.
  ResetUserAgent("Chromezilla");
  GoogleString large_css(20 * 1024, 'a');
  SetFontCssResponse(StrCat(kRoboto, "&UA=Chromezilla"), large_css);
  ValidateExpected(
      "large_css", CssLinkHref(kRoboto),
      StrCat(kPreconnectHttp, "<style>", large_css, "</style>"));
}

TEST_F(GoogleFontCssInlineFilterTest, OverDefaultLimitDoesNotInline) {
  // Pin the upper edge of the raised default: a response over 48 KiB stays
  // external under default configuration, but still gets the hint.
  ResetUserAgent("Chromezilla");
  GoogleString oversize_css(48 * 1024 + 1, 'a');
  SetFontCssResponse(StrCat(kRoboto, "&UA=Chromezilla"), oversize_css);
  ValidateExpected(
      "oversize_css", CssLinkHref(kRoboto),
      StrCat(kPreconnectHttp, CssLinkHref(kRoboto),
             "<!--CSS not inlined since it&#39;s bigger than 49152 bytes-->"));
}

TEST_F(GoogleFontCssInlineFilterTest, PreconnectFollowsLinkScheme) {
  // The loader CSS references fonts.gstatic.com over the scheme it was
  // fetched with, so an https font link must produce an https hint.
  ResetUserAgent("Chromezilla");
  SetFontCssResponse(StrCat(kRobotoSsl, "&UA=Chromezilla"),
                     "sfont_chromezilla");
  ValidateExpected(
      "https_link", CssLinkHref(kRobotoSsl),
      StrCat(kPreconnectHttps, "<style>sfont_chromezilla</style>"));
}

TEST_F(GoogleFontCssInlineFilterTest, PreconnectInsertedOnce) {
  // Two links to different font families: the hint is once-per-document,
  // not once-per-URL.
  ResetUserAgent("Chromezilla");
  SetFontCssResponse(StrCat(kLato, "&UA=Chromezilla"), "lato_chromezilla");
  ValidateExpected("two_font_links",
                   StrCat(CssLinkHref(kRoboto), CssLinkHref(kLato)),
                   StrCat(kPreconnectHttp, "<style>font_chromezilla</style>",
                          "<style>lato_chromezilla</style>"));
}

TEST_F(GoogleFontCssInlineFilterTest, AuthorPreconnectRespected) {
  // An author-supplied crossorigin preconnect for the font file host,
  // appearing ahead of the font link, suppresses ours; the href is resolved,
  // so a protocol-relative hint counts too.
  ResetUserAgent("Chromezilla");
  const char kAuthorPreconnect[] =
      "<link rel=\"preconnect\" href=\"//fonts.gstatic.com/\" crossorigin>";
  ValidateExpected(
      "author_preconnect", StrCat(kAuthorPreconnect, CssLinkHref(kRoboto)),
      StrCat(kAuthorPreconnect, "<style>font_chromezilla</style>"));
}

TEST_F(GoogleFontCssInlineFilterTest, AuthorMultiTokenRelPreconnectRespected) {
  // HTML link types are a space-separated token list, so
  // rel="preconnect dns-prefetch" is a valid author preconnect and must
  // suppress our hint exactly like the single-token form does.
  ResetUserAgent("Chromezilla");
  const char kAuthorPreconnect[] =
      "<link rel=\"preconnect dns-prefetch\" href=\"//fonts.gstatic.com/\" "
      "crossorigin>";
  ValidateExpected(
      "author_multi_token_preconnect",
      StrCat(kAuthorPreconnect, CssLinkHref(kRoboto)),
      StrCat(kAuthorPreconnect, "<style>font_chromezilla</style>"));
}

TEST_F(GoogleFontCssInlineFilterTest, AuthorPreconnectUnrelatedHost) {
  // A preconnect for some other host does not suppress the font hint.
  ResetUserAgent("Chromezilla");
  const char kCdnPreconnect[] =
      "<link rel=\"preconnect\" href=\"//cdn.example.com/\" crossorigin>";
  ValidateExpected(
      "author_preconnect_unrelated",
      StrCat(kCdnPreconnect, CssLinkHref(kRoboto)),
      StrCat(kCdnPreconnect, kPreconnectHttp,
             "<style>font_chromezilla</style>"));
}

TEST_F(GoogleFontCssInlineFilterTest, AuthorPreconnectWithoutCrossorigin) {
  // Fonts are fetched in CORS mode: an author preconnect without crossorigin
  // opens a connection the font fetch can't use, so ours is still inserted.
  ResetUserAgent("Chromezilla");
  const char kNonCorsPreconnect[] =
      "<link rel=\"preconnect\" href=\"//fonts.gstatic.com/\">";
  ValidateExpected(
      "author_preconnect_non_cors",
      StrCat(kNonCorsPreconnect, CssLinkHref(kRoboto)),
      StrCat(kNonCorsPreconnect, kPreconnectHttp,
             "<style>font_chromezilla</style>"));
}

TEST_F(GoogleFontCssInlineFilterTest, AuthorUseCredentialsPreconnect) {
  // crossorigin="use-credentials" warms a credentialed connection, but the
  // font fetch is CORS-mode with same-origin credentials, so it can't reuse
  // it either: like the missing-attribute case, it must not suppress ours.
  ResetUserAgent("Chromezilla");
  const char kCredentialedPreconnect[] =
      "<link rel=\"preconnect\" href=\"//fonts.gstatic.com/\" "
      "crossorigin=\"use-credentials\">";
  ValidateExpected(
      "author_use_credentials_preconnect",
      StrCat(kCredentialedPreconnect, CssLinkHref(kRoboto)),
      StrCat(kCredentialedPreconnect, kPreconnectHttp,
             "<style>font_chromezilla</style>"));

  // CORS settings attribute keywords match ASCII case-insensitively.
  const char kCredentialedUpperPreconnect[] =
      "<link rel=\"preconnect\" href=\"//fonts.gstatic.com/\" "
      "crossorigin=\"USE-CREDENTIALS\">";
  ValidateExpected(
      "author_use_credentials_upper_preconnect",
      StrCat(kCredentialedUpperPreconnect, CssLinkHref(kRoboto)),
      StrCat(kCredentialedUpperPreconnect, kPreconnectHttp,
             "<style>font_chromezilla</style>"));
}

TEST_F(GoogleFontCssInlineFilterTest, AuthorAnonymousPreconnect) {
  // crossorigin="anonymous" spelled out is the same Anonymous state as the
  // value-less attribute, so it suppresses our hint too.
  ResetUserAgent("Chromezilla");
  const char kAuthorPreconnect[] =
      "<link rel=\"preconnect\" href=\"//fonts.gstatic.com/\" "
      "crossorigin=\"anonymous\">";
  ValidateExpected(
      "author_anonymous_preconnect",
      StrCat(kAuthorPreconnect, CssLinkHref(kRoboto)),
      StrCat(kAuthorPreconnect, "<style>font_chromezilla</style>"));
}

TEST_F(GoogleFontCssInlineFilterTest, UsageRestrictions) {
  ResetUserAgent("Chromezilla");

  options()->ClearSignatureForTesting();
  options()->set_modify_caching_headers(false);
  server_context()->ComputeSignature(options());
  // Even though inlining is blocked, the surviving <link> still fetches from
  // fonts.gstatic.com, so the preconnect hint is inserted regardless.
  ValidateExpected("incompat1", CssLinkHref(kRoboto),
                   StrCat(kPreconnectHttp, CssLinkHref(kRoboto),
                          "<!--Cannot inline font loader CSS when "
                          "ModifyCachingHeaders is off-->"));

  options()->ClearSignatureForTesting();
  options()->set_modify_caching_headers(true);
  options()->set_downstream_cache_purge_location_prefix("foo");
  server_context()->ComputeSignature(options());
  ValidateExpected("incompat2", CssLinkHref(kRoboto),
                   StrCat(kPreconnectHttp, CssLinkHref(kRoboto),
                          "<!--Cannot inline font loader CSS when "
                          "using downstream cache-->"));
}

TEST_F(GoogleFontCssInlineFilterTest, ProtocolRelative) {
  ResetUserAgent("Chromezilla");
  ValidateExpected("proto_rel",
                   CssLinkHref("//fonts.googleapis.com/css?family=Roboto"),
                   StrCat(kPreconnectHttp, "<style>font_chromezilla</style>"));
}

class GoogleFontCssInlineFilterSizeLimitTest
    : public GoogleFontCssInlineFilterTestBase {
 protected:
  void SetUp() override {
    GoogleFontCssInlineFilterTestBase::SetUp();
    // GoogleFontCssInlineFilter uses google_font_css_inline_max_bytes.
    // Set a threshold at font_safieri, which should prevent longer
    // font_chromezilla from inlining.
    options()->set_google_font_css_inline_max_bytes(
        STATIC_STRLEN("font_safieri"));
    SetUpForFontFilterTest(RewriteOptions::kInlineGoogleFontCss);
  }
};

TEST_F(GoogleFontCssInlineFilterSizeLimitTest, SizeLimit) {
  // The over-cap link survives, and still triggers font file fetches, so the
  // preconnect hint is inserted for it as well.
  ResetUserAgent("Chromezilla");
  ValidateExpected(
      "slightly_long", CssLinkHref(kRoboto),
      StrCat(kPreconnectHttp, CssLinkHref(kRoboto),
             "<!--CSS not inlined since it&#39;s bigger than 12 bytes-->"));

  ResetUserAgent("Safieri");
  ValidateExpected("short", CssLinkHref(kRoboto),
                   StrCat(kPreconnectHttp, "<style>font_safieri</style>"));
}

class GoogleFontCssInlineFilterAndImportTest
    : public GoogleFontCssInlineFilterTest {
 protected:
  void SetUp() override {
    RewriteTestBase::SetUp();  // skipped base on purpose
    options()->EnableFilter(
        RewriteOptions::RewriteOptions::kInlineImportToLink);
    SetUpForFontFilterTest(RewriteOptions::kInlineGoogleFontCss);
  }
};

TEST_F(GoogleFontCssInlineFilterAndImportTest, ViaInlineImport) {
  // Tests to make sure that if InlineImportToLink filter is on we
  // convert a <style>@import'ing </style> this as well.
  ResetUserAgent("Chromezilla");
  ValidateExpected("import",
                   absl::StrFormat("<style>@import \"%s\";</style>", kRoboto),
                   StrCat(kPreconnectHttp, "<style>font_chromezilla</style>"));

  ResetUserAgent("Safieri");
  ValidateExpected("import",
                   absl::StrFormat("<style>@import \"%s\";</style>", kRoboto),
                   StrCat(kPreconnectHttp, "<style>font_safieri</style>"));
}

class GoogleFontCssInlineFilterAndWidePermissionsTest
    : public GoogleFontCssInlineFilterTestBase {
  void SetUp() override {
    GoogleFontCssInlineFilterTestBase::SetUp();
    // Check that we don't rely solely on authorization to properly
    // dispatch the URL to us.
    options()->WriteableDomainLawyer()->AddDomain("*", message_handler());
    rewrite_driver()->request_context()->AddSessionAuthorizedFetchOrigin(
        "http://fonts.googleapis.com");
    options()->EnableFilter(RewriteOptions::kInlineCss);
    SetUpForFontFilterTest(RewriteOptions::kInlineGoogleFontCss);
  }
};

TEST_F(GoogleFontCssInlineFilterAndWidePermissionsTest, WithWideAuthorization) {
  ResetUserAgent("Chromezilla");
  ValidateExpected("with_domain_*", CssLinkHref(kRoboto),
                   StrCat(kPreconnectHttp, "<style>font_chromezilla</style>"));
}

// Negative test for the above, with font filter off, to make sure
// it's not inline_css doing stuff.
class NoGoogleFontCssInlineFilterAndWidePermissionsTest
    : public GoogleFontCssInlineFilterTestBase {
 protected:
  void SetupForAuthorizedFetchOrigin() {
    // Check that we don't rely solely on authorization to properly
    // dispatch the URL to us. Note that we can't only use DomainLawyer here
    // since UserAgentSensitiveTestFetcher is at http layer so is simply
    // unaware of it.
    options()->WriteableDomainLawyer()->AddDomain("*", message_handler());
    rewrite_driver()->request_context()->AddSessionAuthorizedFetchOrigin(
        "http://fonts.googleapis.com");
    SetUpForFontFilterTest(RewriteOptions::kInlineCss);
  }
};

TEST_F(NoGoogleFontCssInlineFilterAndWidePermissionsTest,
       WithWideAuthorization) {
  // Since font inlining isn't on, the regular inliner complains. This isn't
  // ideal, but doing otherwise requires inline_css to know about
  // inline_google_font_css, which also seems suboptimal.
  ResetUserAgent("Chromezilla");
  SetupForAuthorizedFetchOrigin();
  ValidateExpected("with_domain_*_without_font_filter", CssLinkHref(kRoboto),
                   StrCat(CssLinkHref(kRoboto),
                          "<!--Uncacheable content, preventing rewriting of "
                          "http://fonts.googleapis.com/css?family=Roboto-->"));
}

class GoogleFontCssInlineFilterDnsPrefetchTest
    : public GoogleFontCssInlineFilterTestBase {
 protected:
  void SetUp() override {
    GoogleFontCssInlineFilterTestBase::SetUp();
    options()->set_support_noscript_enabled(false);
    options()->EnableFilter(RewriteOptions::kInsertDnsPrefetch);
    SetUpForFontFilterTest(RewriteOptions::kInlineGoogleFontCss);
  }
};

TEST_F(GoogleFontCssInlineFilterDnsPrefetchTest, SingleHintWithDnsPrefetch) {
  // A UA that both supports DNS prefetch (matches *Chrome/*) and is easy for
  // UserAgentSensitiveTestFetcher to key on. The fetcher escapes the UA when
  // appending it as a query param.
  ResetUserAgent("Chromezilla Chrome/42");
  SetFontCssResponse(StrCat(kRoboto, "&UA=Chromezilla+Chrome%2f42"),
                     "font_chrome");

  // A body resource on a third-party domain gives insert_dns_prefetch a
  // domain to act on once its stored domain list stabilizes.
  const char kBody[] =
      "<body><img src=\"http://cdn.example.com/a.jpg\"></body>";
  const char kCdnPreconnect[] =
      "<link rel=\"preconnect\" href=\"http://cdn.example.com\">";
  GoogleString input =
      StrCat("<head>", CssLinkHref(kRoboto), "</head>", kBody);
  GoogleString head_content =
      StrCat(kPreconnectHttp, "<style>font_chrome</style>");

  // First rewrite: insert_dns_prefetch has no stored domain list yet (it is
  // written at the end of this rewrite), so ours is the only hint.
  ValidateExpected("dns_prefetch_1", input,
                   StrCat("<head>", head_content, "</head>", kBody));
  // Later rewrites: the stored list is stable, so insert_dns_prefetch warms
  // up cdn.example.com at the end of the head. It must never add a second
  // fonts.gstatic.com hint (ours is already in the head) nor one for
  // fonts.googleapis.com (whose <link> is in the head, then inlined away).
  ValidateExpected(
      "dns_prefetch_2", input,
      StrCat("<head>", head_content, kCdnPreconnect, "</head>", kBody));
  ValidateExpected(
      "dns_prefetch_3", input,
      StrCat("<head>", head_content, kCdnPreconnect, "</head>", kBody));
}

}  // namespace

}  // namespace net_instaweb
