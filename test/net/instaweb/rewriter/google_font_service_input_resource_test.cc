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

#include <memory>

#include "net/instaweb/rewriter/public/google_font_service_input_resource.h"

#include "net/instaweb/http/public/counting_url_async_fetcher.h"
#include "net/instaweb/rewriter/public/resource.h"
#include "net/instaweb/rewriter/public/rewrite_driver.h"
#include "net/instaweb/rewriter/public/server_context.h"
#include "pagespeed/kernel/base/ref_counted_ptr.h"
#include "pagespeed/kernel/base/string.h"
#include "pagespeed/kernel/base/timer.h"
#include "pagespeed/kernel/http/content_type.h"
#include "pagespeed/kernel/http/google_url.h"
#include "pagespeed/kernel/http/response_headers.h"
#include "test/net/instaweb/http/ua_sensitive_test_fetcher.h"
#include "test/net/instaweb/rewriter/mock_resource_callback.h"
#include "test/net/instaweb/rewriter/rewrite_test_base.h"
#include "test/pagespeed/kernel/base/gtest.h"

namespace net_instaweb {

namespace {

const char kRoboto[] = "http://fonts.googleapis.com/css?family=Roboto";
const char kRobotoSsl[] = "https://fonts.googleapis.com/css?family=Roboto";
const char kNonCss[] = "http://fonts.googleapis.com/some.txt";

// The URLs actually requested: the filter adds display=swap when the loader URL
// has no display parameter.
const char kRobotoSwap[] =
    "http://fonts.googleapis.com/css?family=Roboto&display=swap";
const char kRobotoSslSwap[] =
    "https://fonts.googleapis.com/css?family=Roboto&display=swap";
const char kRobotoOptional[] =
    "http://fonts.googleapis.com/css?family=Roboto&display=optional";

class GoogleFontServiceInputResourceTest : public RewriteTestBase {
 protected:
  void SetUp() override {
    RewriteTestBase::SetUp();
    GoogleFontServiceInputResource::InitStats(statistics());

    // Font loader CSS gets Cache-Control:private, max-age=86400
    ResponseHeaders response_headers;
    SetDefaultLongCacheHeaders(&kContentTypeCss, &response_headers);
    response_headers.SetDateAndCaching(timer()->NowMs(),
                                       86400 * Timer::kSecondMs, ", private");

    // Set them up in spots where UaSensitiveFetcher would direct them.
    SetFetchResponse(StrCat(kRobotoSwap, "&UA=Chromezilla"), response_headers,
                     "font_chromezilla");

    SetFetchResponse(StrCat(kRobotoSwap, "&UA=Safieri"), response_headers,
                     "font_safieri");

    SetFetchResponse(StrCat(kRobotoSslSwap, "&UA=Chromezilla"),
                     response_headers, "sfont_chromezilla");

    SetFetchResponse(StrCat(kRobotoSslSwap, "&UA=Safieri"), response_headers,
                     "sfont_safieri");

    ResponseHeaders non_css;
    SetDefaultLongCacheHeaders(&kContentTypeText, &non_css);
    SetFetchResponse(StrCat(kNonCss, "?UA=Chromezilla"), non_css,
                     "something weird");
  }

  void ResetUserAgent(StringPiece user_agent) {
    ClearRewriteDriver();
    rewrite_driver()->SetSessionFetcher(
        new UserAgentSensitiveTestFetcher(rewrite_driver()->async_fetcher()));
    SetCurrentUserAgent(user_agent);
    SetDriverRequestHeaders();
  }
};

TEST_F(GoogleFontServiceInputResourceTest, Creation) {
  ResetUserAgent("Chromezilla");

  std::unique_ptr<GoogleFontServiceInputResource> resource;

  GoogleUrl url1("efpeRO#@($@#K$!@($");
  resource.reset(GoogleFontServiceInputResource::Make(url1, rewrite_driver()));
  EXPECT_TRUE(resource.get() == nullptr);

  GoogleUrl url2("http://example.com/foo.css");
  resource.reset(GoogleFontServiceInputResource::Make(url2, rewrite_driver()));
  EXPECT_TRUE(resource.get() == nullptr);

  GoogleUrl url3(kRoboto);
  resource.reset(GoogleFontServiceInputResource::Make(url3, rewrite_driver()));
  ASSERT_TRUE(resource.get() != nullptr);
  EXPECT_EQ(kRobotoSwap, resource->url());
  EXPECT_EQ(
      "gfnt://fonts.googleapis.com/css?family=Roboto&display=swap"
      "&X-PS-UA=Chromezilla",
      resource->cache_key());

  GoogleUrl url4(kRobotoSsl);
  resource.reset(GoogleFontServiceInputResource::Make(url4, rewrite_driver()));
  ASSERT_TRUE(resource.get() != nullptr);
  EXPECT_EQ(kRobotoSslSwap, resource->url());
  EXPECT_EQ(
      "gfnts://fonts.googleapis.com/css?family=Roboto&display=swap"
      "&X-PS-UA=Chromezilla",
      resource->cache_key());
}

TEST_F(GoogleFontServiceInputResourceTest, DisplaySwapAddedWhenMissing) {
  ResetUserAgent("Chromezilla");
  std::unique_ptr<GoogleFontServiceInputResource> resource;

  // No query at all.
  GoogleUrl no_query("http://fonts.googleapis.com/css");
  resource.reset(
      GoogleFontServiceInputResource::Make(no_query, rewrite_driver()));
  ASSERT_TRUE(resource.get() != nullptr);
  EXPECT_EQ("http://fonts.googleapis.com/css?display=swap", resource->url());

  // A multi-family query keeps its text exactly as written.
  GoogleUrl families(
      "https://fonts.googleapis.com/css?family=Open+Sans:400,700|Lato");
  resource.reset(
      GoogleFontServiceInputResource::Make(families, rewrite_driver()));
  ASSERT_TRUE(resource.get() != nullptr);
  EXPECT_EQ(
      "https://fonts.googleapis.com/css?family=Open+Sans:400,700|Lato"
      "&display=swap",
      resource->url());

  // A parameter that merely starts with "display" is not display=.
  GoogleUrl lookalike(
      "http://fonts.googleapis.com/css?family=Roboto&displayx=1");
  resource.reset(
      GoogleFontServiceInputResource::Make(lookalike, rewrite_driver()));
  ASSERT_TRUE(resource.get() != nullptr);
  EXPECT_EQ(
      "http://fonts.googleapis.com/css?family=Roboto&displayx=1&display=swap",
      resource->url());
}

TEST_F(GoogleFontServiceInputResourceTest, CssPathsOnly) {
  ResetUserAgent("Chromezilla");
  std::unique_ptr<GoogleFontServiceInputResource> resource;

  // Icon fonts rely on ligatures, so /icon is fetched unchanged.
  GoogleUrl icon("https://fonts.googleapis.com/icon?family=Material+Icons");
  resource.reset(GoogleFontServiceInputResource::Make(icon, rewrite_driver()));
  ASSERT_TRUE(resource.get() != nullptr);
  EXPECT_EQ("https://fonts.googleapis.com/icon?family=Material+Icons",
            resource->url());

  GoogleUrl css2("https://fonts.googleapis.com/css2?family=Roboto");
  resource.reset(GoogleFontServiceInputResource::Make(css2, rewrite_driver()));
  ASSERT_TRUE(resource.get() != nullptr);
  EXPECT_EQ("https://fonts.googleapis.com/css2?family=Roboto&display=swap",
            resource->url());
}

TEST_F(GoogleFontServiceInputResourceTest, ExplicitDisplayKept) {
  ResetUserAgent("Chromezilla");
  std::unique_ptr<GoogleFontServiceInputResource> resource;

  GoogleUrl optional(kRobotoOptional);
  resource.reset(
      GoogleFontServiceInputResource::Make(optional, rewrite_driver()));
  ASSERT_TRUE(resource.get() != nullptr);
  EXPECT_EQ(kRobotoOptional, resource->url());
  EXPECT_EQ(
      "gfnt://fonts.googleapis.com/css?family=Roboto&display=optional"
      "&X-PS-UA=Chromezilla",
      resource->cache_key());

  // display first in the query, and an empty value, are still the author's.
  GoogleUrl first(
      "http://fonts.googleapis.com/css?display=block&family=Roboto");
  resource.reset(GoogleFontServiceInputResource::Make(first, rewrite_driver()));
  ASSERT_TRUE(resource.get() != nullptr);
  EXPECT_EQ("http://fonts.googleapis.com/css?display=block&family=Roboto",
            resource->url());

  GoogleUrl empty("http://fonts.googleapis.com/css?family=Roboto&display=");
  resource.reset(GoogleFontServiceInputResource::Make(empty, rewrite_driver()));
  ASSERT_TRUE(resource.get() != nullptr);
  EXPECT_EQ("http://fonts.googleapis.com/css?family=Roboto&display=",
            resource->url());
}

TEST_F(GoogleFontServiceInputResourceTest, FetchesExplicitDisplayUnchanged) {
  ResponseHeaders response_headers;
  SetDefaultLongCacheHeaders(&kContentTypeCss, &response_headers);
  response_headers.SetDateAndCaching(timer()->NowMs(), 86400 * Timer::kSecondMs,
                                     ", private");
  SetFetchResponse(StrCat(kRobotoOptional, "&UA=Chromezilla"), response_headers,
                   "font_optional");
  ResetUserAgent("Chromezilla");

  GoogleUrl url(kRobotoOptional);
  ResourcePtr resource(
      GoogleFontServiceInputResource::Make(url, rewrite_driver()));
  ASSERT_TRUE(resource.get() != nullptr);
  MockResourceCallback callback(resource, server_context()->thread_system());
  resource->LoadAsync(Resource::kReportFailureIfNotCacheable,
                      rewrite_driver()->request_context(), &callback);
  EXPECT_TRUE(callback.done());
  EXPECT_TRUE(callback.success());
  EXPECT_EQ("font_optional", resource->ExtractUncompressedContents());
}

TEST_F(GoogleFontServiceInputResourceTest, Load) {
  GoogleUrl url(kRoboto);
  ResetUserAgent("Chromezilla");

  ResourcePtr resource(
      GoogleFontServiceInputResource::Make(url, rewrite_driver()));
  ASSERT_TRUE(resource.get() != nullptr);
  MockResourceCallback callback(resource, server_context()->thread_system());
  resource->LoadAsync(Resource::kReportFailureIfNotCacheable,
                      rewrite_driver()->request_context(), &callback);
  EXPECT_TRUE(callback.done());
  EXPECT_TRUE(callback.success());
  EXPECT_EQ("font_chromezilla", resource->ExtractUncompressedContents());
  EXPECT_EQ(1, counting_url_async_fetcher()->fetch_count());

  // Make sure it's cached.
  ResourcePtr resource2(
      GoogleFontServiceInputResource::Make(url, rewrite_driver()));
  ASSERT_TRUE(resource2.get() != nullptr);
  MockResourceCallback callback2(resource2, server_context()->thread_system());
  resource2->LoadAsync(Resource::kReportFailureIfNotCacheable,
                       rewrite_driver()->request_context(), &callback2);
  EXPECT_TRUE(callback2.done());
  EXPECT_TRUE(callback2.success());
  EXPECT_EQ("font_chromezilla", resource2->ExtractUncompressedContents());
  EXPECT_EQ(1, counting_url_async_fetcher()->fetch_count());

  // But that different UA gets different string.
  ResetUserAgent("Safieri");
  ResourcePtr resource3(
      GoogleFontServiceInputResource::Make(url, rewrite_driver()));
  ASSERT_TRUE(resource3.get() != nullptr);
  MockResourceCallback callback3(resource3, server_context()->thread_system());
  resource3->LoadAsync(Resource::kReportFailureIfNotCacheable,
                       rewrite_driver()->request_context(), &callback3);
  EXPECT_TRUE(callback3.done());
  EXPECT_TRUE(callback3.success());
  EXPECT_EQ("font_safieri", resource3->ExtractUncompressedContents());
  EXPECT_EQ(2, counting_url_async_fetcher()->fetch_count());
}

TEST_F(GoogleFontServiceInputResourceTest, UANormalization) {
  const char kIE7a[] =
      "Mozilla/4.0 (compatible; MSIE 7.0; Windows NT 5.1; Trident/4.0; "
      ".NET CLR 2.0.50727; .NET CLR 3.0.4506.2152; .NET CLR 3.5.30729)";

  GoogleUrl url(kRobotoSwap);
  std::unique_ptr<GoogleUrl> url_plus_ua(url.CopyAndAddQueryParam("UA", kIE7a));
  ResponseHeaders response_headers;
  SetDefaultLongCacheHeaders(&kContentTypeCss, &response_headers);
  response_headers.SetDateAndCaching(timer()->NowMs(), 86400 * Timer::kSecondMs,
                                     ", private");
  SetFetchResponse(url_plus_ua->Spec(), response_headers, "font_IE7");

  // Try fetches with a couple of possible aliases. The one we uploaded it under
  // is first, since it's the only one the fetcher replies to.
  ResetUserAgent(kIE7a);

  ResourcePtr resource(
      GoogleFontServiceInputResource::Make(url, rewrite_driver()));
  ASSERT_TRUE(resource.get() != nullptr);
  MockResourceCallback callback(resource, server_context()->thread_system());
  resource->LoadAsync(Resource::kReportFailureIfNotCacheable,
                      rewrite_driver()->request_context(), &callback);
  EXPECT_TRUE(callback.done());
  EXPECT_TRUE(callback.success());
  EXPECT_EQ("font_IE7", resource->ExtractUncompressedContents());
  EXPECT_EQ(1, counting_url_async_fetcher()->fetch_count());

  // Different list of .NET versions.
  const char kIE7b[] =
      "Mozilla/4.0 (compatible; MSIE 7.0; Windows NT 5.1; Trident/4.0; "
      ".NET CLR 2.0.50727; .NET4.0C; .NET4.0E)";
  ResetUserAgent(kIE7b);

  ResourcePtr resource2(
      GoogleFontServiceInputResource::Make(url, rewrite_driver()));
  ASSERT_TRUE(resource2.get() != nullptr);
  MockResourceCallback callback2(resource2, server_context()->thread_system());
  resource2->LoadAsync(Resource::kReportFailureIfNotCacheable,
                       rewrite_driver()->request_context(), &callback2);
  EXPECT_TRUE(callback2.done());
  EXPECT_TRUE(callback2.success());
  EXPECT_EQ("font_IE7", resource->ExtractUncompressedContents());
  EXPECT_EQ(1, counting_url_async_fetcher()->fetch_count());
}

TEST_F(GoogleFontServiceInputResourceTest, LoadParallel) {
  GoogleUrl url(kRoboto);
  SetupWaitFetcher();

  ResetUserAgent("Chromezilla");
  ResourcePtr resource(
      GoogleFontServiceInputResource::Make(url, rewrite_driver()));
  ASSERT_TRUE(resource.get() != nullptr);
  MockResourceCallback callback(resource, server_context()->thread_system());
  resource->LoadAsync(Resource::kReportFailureIfNotCacheable,
                      rewrite_driver()->request_context(), &callback);
  EXPECT_FALSE(callback.done());

  ResetUserAgent("Safieri");
  ResourcePtr resource2(
      GoogleFontServiceInputResource::Make(url, rewrite_driver()));
  ASSERT_TRUE(resource2.get() != nullptr);
  MockResourceCallback callback2(resource2, server_context()->thread_system());
  resource2->LoadAsync(Resource::kReportFailureIfNotCacheable,
                       rewrite_driver()->request_context(), &callback2);
  EXPECT_FALSE(callback2.done());

  CallFetcherCallbacks();

  ASSERT_TRUE(callback.done());
  EXPECT_TRUE(callback.success());
  EXPECT_EQ("font_chromezilla", resource->ExtractUncompressedContents());

  ASSERT_TRUE(callback2.done());
  EXPECT_TRUE(callback2.success());
  EXPECT_EQ("font_safieri", resource2->ExtractUncompressedContents());
}

TEST_F(GoogleFontServiceInputResourceTest, FetchFailure) {
  SetFetchFailOnUnexpected(false);

  // Regression test --- don't crash when fetch fails.
  // Bug discovered by accident due to a bug in a test.
  ResetUserAgent("Huhzilla");
  GoogleUrl url(kRoboto);
  ResourcePtr resource(
      GoogleFontServiceInputResource::Make(url, rewrite_driver()));
  ASSERT_TRUE(resource.get() != nullptr);
  MockResourceCallback callback(resource, server_context()->thread_system());
  resource->LoadAsync(Resource::kLoadEvenIfNotCacheable,
                      rewrite_driver()->request_context(), &callback);
  EXPECT_TRUE(callback.done());
  EXPECT_FALSE(callback.success());
  EXPECT_EQ("", resource->ExtractUncompressedContents());
  EXPECT_EQ(1, counting_url_async_fetcher()->fetch_count());
}

TEST_F(GoogleFontServiceInputResourceTest, DontLoadNonCss) {
  ResetUserAgent("Chromezilla");

  GoogleUrl non_css_url(kNonCss);
  ResourcePtr resource(
      GoogleFontServiceInputResource::Make(non_css_url, rewrite_driver()));
  ASSERT_TRUE(resource.get() != nullptr);
  MockResourceCallback callback(resource, server_context()->thread_system());
  resource->LoadAsync(Resource::kLoadEvenIfNotCacheable,
                      rewrite_driver()->request_context(), &callback);
  EXPECT_TRUE(callback.done());
  EXPECT_FALSE(resource->HttpStatusOk());
  EXPECT_EQ(1, counting_url_async_fetcher()->fetch_count());

  // Make sure we don't end up caching a success, either.
  ResourcePtr resource2(
      GoogleFontServiceInputResource::Make(non_css_url, rewrite_driver()));
  ASSERT_TRUE(resource2.get() != nullptr);
  MockResourceCallback callback2(resource2, server_context()->thread_system());
  resource2->LoadAsync(Resource::kLoadEvenIfNotCacheable,
                       rewrite_driver()->request_context(), &callback2);
  EXPECT_TRUE(callback2.done());
  EXPECT_FALSE(resource->HttpStatusOk());
  EXPECT_EQ(1, counting_url_async_fetcher()->fetch_count());
}

TEST_F(GoogleFontServiceInputResourceTest, IsFontServiceUrlTest) {
  GoogleUrl roboto_url(kRoboto);
  EXPECT_TRUE(GoogleFontServiceInputResource::IsFontServiceUrl(roboto_url));

  GoogleUrl roboto_ssl_url(kRobotoSsl);
  EXPECT_TRUE(GoogleFontServiceInputResource::IsFontServiceUrl(roboto_ssl_url));

  GoogleUrl example_url("http://example.com/");
  EXPECT_FALSE(GoogleFontServiceInputResource::IsFontServiceUrl(example_url));
}

}  // namespace

}  // namespace net_instaweb
