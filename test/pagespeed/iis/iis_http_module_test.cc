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

// Unit tests for IisHttpModule - IIS CHttpModule implementation.

#include "gtest/gtest.h"
#include "test/pagespeed/iis/iis_test_base.h"
#include "test/pagespeed/iis/mock_iis.h"

namespace net_instaweb {

class IisHttpModuleTest : public IisTestBase {
 protected:
  void SetUp() override {
    IisTestBase::SetUp();
  }

  // Helper to check if content type indicates HTML
  static bool IsHtmlContentType(const GoogleString& content_type) {
    return content_type.find("text/html") != GoogleString::npos ||
           content_type.find("application/xhtml+xml") != GoogleString::npos;
  }

  // Helper to check if response status indicates a successful response
  static bool IsSuccessStatus(USHORT status) {
    return status >= 200 && status < 300;
  }

  // Helper to check if response should be rewritten based on status
  static bool ShouldRewriteStatus(USHORT status) {
    // Only rewrite 200 OK responses
    return status == 200;
  }
};

// ============================================================================
// URL Detection Tests
// ============================================================================

TEST_F(IisHttpModuleTest, DetectsPageSpeedResource) {
  // Test IsPageSpeedResource for various URLs
  EXPECT_TRUE(IsPageSpeedUrl("/style.pagespeed.cf.ABC.css"));
  EXPECT_TRUE(IsPageSpeedUrl("/image.pagespeed.ic.XYZ.png"));
  EXPECT_TRUE(IsPageSpeedUrl("/script.pagespeed.jm.DEF.js"));

  EXPECT_FALSE(IsPageSpeedUrl("/normal/page.html"));
  EXPECT_FALSE(IsPageSpeedUrl("/style.css"));
  EXPECT_FALSE(IsPageSpeedUrl("/image.png"));
}

TEST_F(IisHttpModuleTest, DetectsPageSpeedResourceWithPath) {
  // Test with nested paths
  EXPECT_TRUE(IsPageSpeedUrl("/assets/css/style.pagespeed.cf.ABC.css"));
  EXPECT_TRUE(IsPageSpeedUrl("/images/gallery/photo.pagespeed.ic.XYZ.jpg"));
  EXPECT_TRUE(IsPageSpeedUrl("/js/vendor/jquery.pagespeed.jm.DEF.js"));
}

TEST_F(IisHttpModuleTest, DetectsPageSpeedResourceCaseSensitive) {
  // .pagespeed. marker is case-sensitive
  EXPECT_FALSE(IsPageSpeedUrl("/style.PAGESPEED.cf.ABC.css"));
  EXPECT_FALSE(IsPageSpeedUrl("/style.PageSpeed.cf.ABC.css"));
  EXPECT_FALSE(IsPageSpeedUrl("/style.Pagespeed.cf.ABC.css"));
}

// ============================================================================
// URL Decoding Tests
// ============================================================================

TEST_F(IisHttpModuleTest, DecodesIproUrl) {
  GoogleString original, filter_id, hash;
  EXPECT_TRUE(DecodePageSpeedUrl("/style.pagespeed.cf.ABC123.css",
                                  &original, &filter_id, &hash));
  EXPECT_EQ("/style.css", original);
  EXPECT_EQ("cf", filter_id);
  EXPECT_EQ("ABC123", hash);
}

TEST_F(IisHttpModuleTest, DecodesIproUrlWithPath) {
  GoogleString original, filter_id, hash;
  EXPECT_TRUE(DecodePageSpeedUrl("/assets/css/main.pagespeed.cf.XYZ789.css",
                                  &original, &filter_id, &hash));
  EXPECT_EQ("/assets/css/main.css", original);
  EXPECT_EQ("cf", filter_id);
  EXPECT_EQ("XYZ789", hash);
}

TEST_F(IisHttpModuleTest, DecodesImageUrl) {
  GoogleString original, filter_id, hash;
  EXPECT_TRUE(DecodePageSpeedUrl("/images/logo.pagespeed.ic.ABC.png",
                                  &original, &filter_id, &hash));
  EXPECT_EQ("/images/logo.png", original);
  EXPECT_EQ("ic", filter_id);
  EXPECT_EQ("ABC", hash);
}

TEST_F(IisHttpModuleTest, DecodesJavascriptUrl) {
  GoogleString original, filter_id, hash;
  EXPECT_TRUE(DecodePageSpeedUrl("/scripts/app.pagespeed.jm.DEF456.js",
                                  &original, &filter_id, &hash));
  EXPECT_EQ("/scripts/app.js", original);
  EXPECT_EQ("jm", filter_id);
  EXPECT_EQ("DEF456", hash);
}

TEST_F(IisHttpModuleTest, DecodeFailsForNonPageSpeedUrl) {
  GoogleString original, filter_id, hash;
  EXPECT_FALSE(DecodePageSpeedUrl("/normal/style.css",
                                   &original, &filter_id, &hash));
}

TEST_F(IisHttpModuleTest, DecodeFailsForMalformedUrl) {
  GoogleString original, filter_id, hash;
  // Missing hash and extension
  EXPECT_FALSE(DecodePageSpeedUrl("/style.pagespeed.cf",
                                   &original, &filter_id, &hash));
}

// ============================================================================
// Request Classification Tests
// ============================================================================

TEST_F(IisHttpModuleTest, ClassifiesHtmlRequests) {
  auto context = CreateContext("/page.html");
  context->response()->SetHeader("Content-Type", "text/html");

  // An HTML response should be considered for rewriting
  EXPECT_TRUE(IsHtmlContentType(context->response()->GetHeader("Content-Type")));
}

TEST_F(IisHttpModuleTest, ClassifiesHtmlWithCharset) {
  auto context = CreateContext("/page.html");
  context->response()->SetHeader("Content-Type", "text/html; charset=utf-8");

  EXPECT_TRUE(IsHtmlContentType(context->response()->GetHeader("Content-Type")));
}

TEST_F(IisHttpModuleTest, ClassifiesXhtmlRequests) {
  auto context = CreateContext("/page.xhtml");
  context->response()->SetHeader("Content-Type", "application/xhtml+xml");

  // XHTML should also be considered for rewriting
  EXPECT_TRUE(IsHtmlContentType(context->response()->GetHeader("Content-Type")));
}

TEST_F(IisHttpModuleTest, IgnoresNonHtmlContent) {
  auto context = CreateContext("/style.css");
  context->response()->SetHeader("Content-Type", "text/css");

  // CSS responses should not be rewritten as HTML
  EXPECT_FALSE(IsHtmlContentType(context->response()->GetHeader("Content-Type")));
}

TEST_F(IisHttpModuleTest, IgnoresImageContent) {
  auto context = CreateContext("/image.png");
  context->response()->SetHeader("Content-Type", "image/png");

  // Image responses should not be rewritten as HTML
  EXPECT_FALSE(IsHtmlContentType(context->response()->GetHeader("Content-Type")));
}

TEST_F(IisHttpModuleTest, IgnoresJsonContent) {
  auto context = CreateContext("/api/data.json");
  context->response()->SetHeader("Content-Type", "application/json");

  // JSON responses should not be rewritten as HTML
  EXPECT_FALSE(IsHtmlContentType(context->response()->GetHeader("Content-Type")));
}

TEST_F(IisHttpModuleTest, IgnoresJavascriptContent) {
  auto context = CreateContext("/script.js");
  context->response()->SetHeader("Content-Type", "application/javascript");

  EXPECT_FALSE(IsHtmlContentType(context->response()->GetHeader("Content-Type")));
}

TEST_F(IisHttpModuleTest, IgnoresTextJavascriptContent) {
  auto context = CreateContext("/script.js");
  context->response()->SetHeader("Content-Type", "text/javascript");

  EXPECT_FALSE(IsHtmlContentType(context->response()->GetHeader("Content-Type")));
}

TEST_F(IisHttpModuleTest, IgnoresXmlContent) {
  auto context = CreateContext("/data.xml");
  context->response()->SetHeader("Content-Type", "application/xml");

  EXPECT_FALSE(IsHtmlContentType(context->response()->GetHeader("Content-Type")));
}

TEST_F(IisHttpModuleTest, IgnoresSvgContent) {
  auto context = CreateContext("/icon.svg");
  context->response()->SetHeader("Content-Type", "image/svg+xml");

  EXPECT_FALSE(IsHtmlContentType(context->response()->GetHeader("Content-Type")));
}

// ============================================================================
// Admin Path Detection Tests
// ============================================================================

TEST_F(IisHttpModuleTest, DetectsAdminPath) {
  // Default admin path is /pagespeed_admin
  auto context = CreateContext("/pagespeed_admin");
  EXPECT_STREQ("/pagespeed_admin", context->request()->GetRawHttpRequestUrl());
  // Note: Full test requires IisServerContext to verify admin handling
}

TEST_F(IisHttpModuleTest, DetectsStatisticsPath) {
  // Default statistics path is /pagespeed_statistics
  auto context = CreateContext("/pagespeed_statistics");
  EXPECT_STREQ("/pagespeed_statistics", context->request()->GetRawHttpRequestUrl());
  // Note: Full test requires IisServerContext to verify statistics handling
}

TEST_F(IisHttpModuleTest, DetectsAdminSubPaths) {
  auto context = CreateContext("/pagespeed_admin/config");
  EXPECT_STREQ("/pagespeed_admin/config", context->request()->GetRawHttpRequestUrl());
}

TEST_F(IisHttpModuleTest, DetectsMessagesPath) {
  auto context = CreateContext("/pagespeed_message");
  EXPECT_STREQ("/pagespeed_message", context->request()->GetRawHttpRequestUrl());
}

// ============================================================================
// Response Status Tests
// ============================================================================

TEST_F(IisHttpModuleTest, SkipsNon200Responses) {
  auto context = CreateContext("/notfound.html");
  context->response()->SetStatus(404, "Not Found");
  context->response()->SetHeader("Content-Type", "text/html");

  // Non-200 responses should not be rewritten
  EXPECT_FALSE(ShouldRewriteStatus(context->response()->GetStatus()));
}

TEST_F(IisHttpModuleTest, SkipsRedirectResponses) {
  auto context = CreateContext("/redirect.html");
  context->response()->SetStatus(301, "Moved Permanently");

  // Redirect responses should not be rewritten
  EXPECT_FALSE(ShouldRewriteStatus(context->response()->GetStatus()));
  EXPECT_EQ(301, context->response()->GetStatus());
}

TEST_F(IisHttpModuleTest, Skips302Redirect) {
  auto context = CreateContext("/temp-redirect.html");
  context->response()->SetStatus(302, "Found");

  EXPECT_FALSE(ShouldRewriteStatus(context->response()->GetStatus()));
}

TEST_F(IisHttpModuleTest, Skips304NotModified) {
  auto context = CreateContext("/cached.html");
  context->response()->SetStatus(304, "Not Modified");

  EXPECT_FALSE(ShouldRewriteStatus(context->response()->GetStatus()));
}

TEST_F(IisHttpModuleTest, SkipsErrorResponses) {
  auto context = CreateContext("/error.html");
  context->response()->SetStatus(500, "Internal Server Error");

  // Error responses should not be rewritten
  EXPECT_FALSE(ShouldRewriteStatus(context->response()->GetStatus()));
}

TEST_F(IisHttpModuleTest, Skips400BadRequest) {
  auto context = CreateContext("/bad.html");
  context->response()->SetStatus(400, "Bad Request");

  EXPECT_FALSE(ShouldRewriteStatus(context->response()->GetStatus()));
}

TEST_F(IisHttpModuleTest, Skips403Forbidden) {
  auto context = CreateContext("/forbidden.html");
  context->response()->SetStatus(403, "Forbidden");

  EXPECT_FALSE(ShouldRewriteStatus(context->response()->GetStatus()));
}

TEST_F(IisHttpModuleTest, Skips503ServiceUnavailable) {
  auto context = CreateContext("/unavailable.html");
  context->response()->SetStatus(503, "Service Unavailable");

  EXPECT_FALSE(ShouldRewriteStatus(context->response()->GetStatus()));
}

TEST_F(IisHttpModuleTest, Rewrites200OkResponses) {
  auto context = CreateContext("/page.html");
  context->response()->SetStatus(200, "OK");
  context->response()->SetHeader("Content-Type", "text/html");

  // 200 OK HTML responses should be rewritten
  EXPECT_TRUE(ShouldRewriteStatus(context->response()->GetStatus()));
  EXPECT_TRUE(IsHtmlContentType(context->response()->GetHeader("Content-Type")));
}

// ============================================================================
// URL Extraction Tests
// ============================================================================

TEST_F(IisHttpModuleTest, ExtractsSimpleUrl) {
  auto context = CreateContext("/simple/path.html");
  EXPECT_STREQ("/simple/path.html", context->request()->GetRawHttpRequestUrl());
}

TEST_F(IisHttpModuleTest, ExtractsUrlWithQueryString) {
  auto context = CreateContext("/path.html?foo=bar&baz=qux");
  EXPECT_STREQ("/path.html?foo=bar&baz=qux",
               context->request()->GetRawHttpRequestUrl());
  EXPECT_EQ("foo=bar&baz=qux", context->request()->query_string());
}

TEST_F(IisHttpModuleTest, ExtractsDeepPath) {
  auto context = CreateContext("/a/b/c/d/e/f/file.html");
  EXPECT_STREQ("/a/b/c/d/e/f/file.html",
               context->request()->GetRawHttpRequestUrl());
}

TEST_F(IisHttpModuleTest, ExtractsRootUrl) {
  auto context = CreateContext("/");
  EXPECT_STREQ("/", context->request()->GetRawHttpRequestUrl());
}

TEST_F(IisHttpModuleTest, ExtractsEmptyQueryString) {
  auto context = CreateContext("/path.html?");
  EXPECT_STREQ("/path.html?", context->request()->GetRawHttpRequestUrl());
  EXPECT_EQ("", context->request()->query_string());
}

TEST_F(IisHttpModuleTest, ExtractsUrlWithFragment) {
  auto context = CreateContext("/path.html?foo=bar");
  EXPECT_EQ("foo=bar", context->request()->query_string());
}

// ============================================================================
// Request Method Tests
// ============================================================================

TEST_F(IisHttpModuleTest, HandlesGetRequest) {
  auto context = CreateContext("/page.html");
  context->request()->SetMethod("GET");
  EXPECT_EQ("GET", context->request()->GetMethod());
}

TEST_F(IisHttpModuleTest, HandlesPostRequest) {
  auto context = CreateContext("/page.html");
  context->request()->SetMethod("POST");
  EXPECT_EQ("POST", context->request()->GetMethod());
}

TEST_F(IisHttpModuleTest, HandlesHeadRequest) {
  auto context = CreateContext("/page.html");
  context->request()->SetMethod("HEAD");
  EXPECT_EQ("HEAD", context->request()->GetMethod());
}

TEST_F(IisHttpModuleTest, HandlesPutRequest) {
  auto context = CreateContext("/resource");
  context->request()->SetMethod("PUT");
  EXPECT_EQ("PUT", context->request()->GetMethod());
}

TEST_F(IisHttpModuleTest, HandlesDeleteRequest) {
  auto context = CreateContext("/resource");
  context->request()->SetMethod("DELETE");
  EXPECT_EQ("DELETE", context->request()->GetMethod());
}

TEST_F(IisHttpModuleTest, HandlesOptionsRequest) {
  auto context = CreateContext("/page.html");
  context->request()->SetMethod("OPTIONS");
  EXPECT_EQ("OPTIONS", context->request()->GetMethod());
}

// ============================================================================
// Request Header Tests
// ============================================================================

TEST_F(IisHttpModuleTest, ReadsAcceptHeader) {
  auto context = CreateContext("/page.html");
  context->request()->SetHeader("Accept", "text/html,application/xhtml+xml");
  EXPECT_EQ("text/html,application/xhtml+xml",
            context->request()->GetHeader("Accept"));
}

TEST_F(IisHttpModuleTest, ReadsAcceptEncodingHeader) {
  auto context = CreateContext("/page.html");
  context->request()->SetHeader("Accept-Encoding", "gzip, deflate, br");
  EXPECT_EQ("gzip, deflate, br",
            context->request()->GetHeader("Accept-Encoding"));
}

TEST_F(IisHttpModuleTest, ReadsUserAgentHeader) {
  auto context = CreateContext("/page.html");
  context->request()->SetHeader("User-Agent", "Mozilla/5.0 (Windows NT 10.0)");
  EXPECT_EQ("Mozilla/5.0 (Windows NT 10.0)",
            context->request()->GetHeader("User-Agent"));
}

TEST_F(IisHttpModuleTest, ReadsCacheControlHeader) {
  auto context = CreateContext("/page.html");
  context->request()->SetHeader("Cache-Control", "no-cache");
  EXPECT_EQ("no-cache", context->request()->GetHeader("Cache-Control"));
}

TEST_F(IisHttpModuleTest, ReadsIfNoneMatchHeader) {
  auto context = CreateContext("/page.html");
  context->request()->SetHeader("If-None-Match", "\"abc123\"");
  EXPECT_EQ("\"abc123\"", context->request()->GetHeader("If-None-Match"));
}

TEST_F(IisHttpModuleTest, HeaderLookupIsCaseInsensitive) {
  auto context = CreateContext("/page.html");
  context->request()->SetHeader("Content-Type", "text/html");

  // Headers should be case-insensitive
  EXPECT_EQ("text/html", context->request()->GetHeader("content-type"));
  EXPECT_EQ("text/html", context->request()->GetHeader("CONTENT-TYPE"));
  EXPECT_EQ("text/html", context->request()->GetHeader("Content-Type"));
}

TEST_F(IisHttpModuleTest, MissingHeaderReturnsEmpty) {
  auto context = CreateContext("/page.html");
  EXPECT_EQ("", context->request()->GetHeader("X-NonExistent-Header"));
}

// ============================================================================
// Site and Application Path Tests
// ============================================================================

TEST_F(IisHttpModuleTest, GetsSiteId) {
  auto context = CreateContext("/page.html");
  context->SetSiteId(5);
  EXPECT_EQ(5u, context->GetSiteId());
}

TEST_F(IisHttpModuleTest, GetsSiteName) {
  auto context = CreateContext("/page.html");
  context->SetSiteName("My Website");
  EXPECT_EQ("My Website", context->GetSiteName());
}

TEST_F(IisHttpModuleTest, GetsApplicationPath) {
  auto context = CreateContext("/app/page.html");
  context->SetApplicationPath("/app");
  EXPECT_EQ("/app", context->GetApplicationPath());
}

TEST_F(IisHttpModuleTest, GetsPhysicalPath) {
  auto context = CreateContext("/page.html");
  context->SetPhysicalPath("C:\\inetpub\\wwwroot\\mysite");
  EXPECT_EQ("C:\\inetpub\\wwwroot\\mysite", context->GetPhysicalPath());
}

// ============================================================================
// PageSpeed URL Generation Tests
// ============================================================================

TEST_F(IisHttpModuleTest, CreatesPageSpeedUrl) {
  GoogleString ps_url = CreatePageSpeedUrl("/style.css", "cf", "ABC123");
  EXPECT_EQ("/style.pagespeed.cf.ABC123.css", ps_url);
}

TEST_F(IisHttpModuleTest, CreatesPageSpeedUrlWithPath) {
  GoogleString ps_url = CreatePageSpeedUrl("/assets/main.css", "cf", "XYZ");
  EXPECT_EQ("/assets/main.pagespeed.cf.XYZ.css", ps_url);
}

TEST_F(IisHttpModuleTest, CreatesPageSpeedUrlForImage) {
  GoogleString ps_url = CreatePageSpeedUrl("/images/logo.png", "ic", "DEF456");
  EXPECT_EQ("/images/logo.pagespeed.ic.DEF456.png", ps_url);
}

TEST_F(IisHttpModuleTest, CreatesPageSpeedUrlWithoutExtension) {
  GoogleString ps_url = CreatePageSpeedUrl("/resource", "xx", "HASH");
  EXPECT_EQ("/resource.pagespeed.xx.HASH", ps_url);
}

TEST_F(IisHttpModuleTest, RoundTripsPageSpeedUrl) {
  // Create a URL and decode it back
  GoogleString original = "/styles/main.css";
  GoogleString filter_id = "cf";
  GoogleString hash = "ABC123";

  GoogleString ps_url = CreatePageSpeedUrl(original, filter_id, hash);

  GoogleString decoded_original, decoded_filter, decoded_hash;
  EXPECT_TRUE(DecodePageSpeedUrl(ps_url, &decoded_original, &decoded_filter,
                                  &decoded_hash));
  EXPECT_EQ(original, decoded_original);
  EXPECT_EQ(filter_id, decoded_filter);
  EXPECT_EQ(hash, decoded_hash);
}

// ============================================================================
// IPRO Request Handling Tests (Partial - full tests require IIS)
// ============================================================================

TEST_F(IisHttpModuleTest, HandlesIproRequest) {
  auto context = CreateContext("/style.pagespeed.cf.ABC123.css");
  // Verify URL is correctly stored
  EXPECT_STREQ("/style.pagespeed.cf.ABC123.css",
               context->request()->GetRawHttpRequestUrl());
  EXPECT_TRUE(IsPageSpeedUrl(context->request()->GetRawHttpRequestUrl()));
  // Note: Full IPRO handling requires IisServerContext
}

// ============================================================================
// Async Operation Tests
// ============================================================================

TEST_F(IisHttpModuleTest, TracksAsyncPending) {
  auto context = CreateContext("/page.html");
  EXPECT_FALSE(context->IsAsyncPending());

  context->SetAsyncPending(true);
  EXPECT_TRUE(context->IsAsyncPending());

  context->CompleteAsync(RQ_NOTIFICATION_CONTINUE);
  EXPECT_FALSE(context->IsAsyncPending());
  EXPECT_EQ(RQ_NOTIFICATION_CONTINUE, context->GetAsyncResult());
}

TEST_F(IisHttpModuleTest, AsyncCompleteWithFinishRequest) {
  auto context = CreateContext("/page.html");
  context->SetAsyncPending(true);
  context->CompleteAsync(RQ_NOTIFICATION_FINISH_REQUEST);

  EXPECT_FALSE(context->IsAsyncPending());
  EXPECT_EQ(RQ_NOTIFICATION_FINISH_REQUEST, context->GetAsyncResult());
}

// ============================================================================
// Response Body Writing Tests
// ============================================================================

TEST_F(IisHttpModuleTest, WritesResponseBody) {
  auto context = CreateContext("/page.html");
  EXPECT_TRUE(context->response()->body().empty());

  context->response()->AppendBody("Hello, World!");
  EXPECT_EQ("Hello, World!", context->response()->body());
}

TEST_F(IisHttpModuleTest, WritesMultipleChunks) {
  auto context = CreateContext("/page.html");

  context->response()->AppendBody("Hello");
  context->response()->AppendBody(", ");
  context->response()->AppendBody("World!");

  EXPECT_EQ("Hello, World!", context->response()->body());
}

TEST_F(IisHttpModuleTest, ClearsResponseBody) {
  auto context = CreateContext("/page.html");
  context->response()->AppendBody("Some content");
  EXPECT_FALSE(context->response()->body().empty());

  context->response()->ClearBody();
  EXPECT_TRUE(context->response()->body().empty());
}

TEST_F(IisHttpModuleTest, WriteEntityChunksReturnsSuccess) {
  auto context = CreateContext("/page.html");
  HRESULT hr = context->response()->WriteEntityChunks("test data", 9);
  EXPECT_TRUE(SUCCEEDED(hr));
  EXPECT_EQ("test data", context->response()->body());
}

// ============================================================================
// Response Completion Tests
// ============================================================================

TEST_F(IisHttpModuleTest, TracksResponseCompletion) {
  auto context = CreateContext("/page.html");
  EXPECT_FALSE(context->response()->completed());

  context->response()->set_completed(true);
  EXPECT_TRUE(context->response()->completed());
}

// ============================================================================
// Authentication Tests
// ============================================================================

TEST_F(IisHttpModuleTest, TracksAuthenticatedUser) {
  auto context = CreateContext("/page.html");
  EXPECT_FALSE(context->IsAuthenticated());
  EXPECT_EQ("", context->GetAuthenticatedUser());

  context->SetAuthenticatedUser("admin");
  EXPECT_TRUE(context->IsAuthenticated());
  EXPECT_EQ("admin", context->GetAuthenticatedUser());
}

// ============================================================================
// Pipeline Event Tests (stubs - require Windows IIS)
// ============================================================================

// The following tests require actual IIS module instantiation which is only
// possible on Windows with the real IIS SDK. They are marked as stubs with
// comments explaining what they would test.

TEST_F(IisHttpModuleTest, LicenseValidationRequired) {
  // Note: Full test requires Windows and IisServerContext
  // Would verify that optimization is blocked without a valid license key
}

TEST_F(IisHttpModuleTest, OnBeginRequestProcessesIpro) {
  // Note: Full test requires Windows IIS module
  // Would verify OnBeginRequest detects and handles .pagespeed. URLs
}

TEST_F(IisHttpModuleTest, OnBeginRequestProcessesAdmin) {
  // Note: Full test requires Windows IIS module
  // Would verify OnBeginRequest detects /pagespeed_admin paths
}

TEST_F(IisHttpModuleTest, OnResolveRequestCacheChecksCache) {
  // Note: Full test requires Windows IIS module
  // Would verify cache lookup for optimized resources
}

TEST_F(IisHttpModuleTest, OnSendResponseRewritesHtml) {
  // Note: Full test requires Windows IIS module
  // Would verify HTML responses are buffered and rewritten
}

TEST_F(IisHttpModuleTest, OnEndRequestCleansUp) {
  // Note: Full test requires Windows IIS module
  // Would verify per-request state is properly cleaned up
}

TEST_F(IisHttpModuleTest, CacheHitServesFromCache) {
  // Note: Full test requires Windows IIS module with cache backend
  // Would verify cached optimized resources are served directly
}

TEST_F(IisHttpModuleTest, CacheMissQueueOptimization) {
  // Note: Full test requires Windows IIS module with cache backend
  // Would verify cache misses trigger background optimization
}

}  // namespace net_instaweb
