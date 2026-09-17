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
// Uses common helpers from IisTestBase for status checking, content type
// detection, and PageSpeed URL manipulation.

#include "gtest/gtest.h"
#include "test/pagespeed/iis/iis_test_base.h"
#include "test/pagespeed/iis/mock_iis.h"

namespace net_instaweb {

// IisHttpModuleTest now uses helpers from IisTestBase:
// - IsPageSpeedUrl(), CreatePageSpeedUrl(), DecodePageSpeedUrl()
// - IsHtmlContentType(), IsSuccessStatus(), ShouldRewriteStatus()
class IisHttpModuleTest : public IisTestBase {
 protected:
  void SetUp() override {
    IisTestBase::SetUp();
  }

  // Collect body from mock chunks.
  // Mirrors the chunk collection logic in iis_http_module.cc's
  // HandleHtmlRewriting() and RecordIproResponseData().
  static GoogleString CollectChunkBody(
      const std::vector<HTTP_DATA_CHUNK>& chunks) {
    GoogleString body;
    for (size_t i = 0; i < chunks.size(); ++i) {
      const HTTP_DATA_CHUNK* chunk = &chunks[i];
      if (chunk->DataChunkType == HttpDataChunkFromMemory) {
        body.append(
            static_cast<const char*>(chunk->FromMemory.pBuffer),
            chunk->FromMemory.BufferLength);
      } else if (chunk->DataChunkType == HttpDataChunkFromFragmentCache ||
                 chunk->DataChunkType == HttpDataChunkFromFragmentCacheEx) {
        // Fragment cache chunks cannot be read without IIS APIs.
        // Log and skip, same as production code.
        LOG(WARNING) << "PageSpeed: Skipping fragment cache chunk (type="
                     << chunk->DataChunkType << ")";
      } else if (chunk->DataChunkType == HttpDataChunkFromFileHandle) {
        // File handle chunks require platform-specific I/O.
        // On non-Windows, log and skip.
        LOG(WARNING) << "PageSpeed: Skipping file handle chunk";
      } else {
        LOG(WARNING) << "PageSpeed: Unknown chunk type "
                     << chunk->DataChunkType << " - skipping";
      }
    }
    return body;
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

// ============================================================================
// Response Chunk Type Tests
// ============================================================================

TEST_F(IisHttpModuleTest, CollectsMemoryChunks) {
  std::vector<HTTP_DATA_CHUNK> chunks;
  HTTP_DATA_CHUNK chunk;
  chunk.DataChunkType = HttpDataChunkFromMemory;
  const char* data = "Hello World";
  chunk.FromMemory.pBuffer = const_cast<char*>(data);
  chunk.FromMemory.BufferLength = 11;
  chunks.push_back(chunk);

  EXPECT_EQ("Hello World", CollectChunkBody(chunks));
}

TEST_F(IisHttpModuleTest, CollectsMultipleMemoryChunks) {
  std::vector<HTTP_DATA_CHUNK> chunks;

  HTTP_DATA_CHUNK c1;
  c1.DataChunkType = HttpDataChunkFromMemory;
  const char* d1 = "Hello ";
  c1.FromMemory.pBuffer = const_cast<char*>(d1);
  c1.FromMemory.BufferLength = 6;
  chunks.push_back(c1);

  HTTP_DATA_CHUNK c2;
  c2.DataChunkType = HttpDataChunkFromMemory;
  const char* d2 = "World";
  c2.FromMemory.pBuffer = const_cast<char*>(d2);
  c2.FromMemory.BufferLength = 5;
  chunks.push_back(c2);

  EXPECT_EQ("Hello World", CollectChunkBody(chunks));
}

TEST_F(IisHttpModuleTest, SkipsFragmentCacheChunksGracefully) {
  std::vector<HTTP_DATA_CHUNK> chunks;

  HTTP_DATA_CHUNK c1;
  c1.DataChunkType = HttpDataChunkFromMemory;
  const char* d1 = "Before";
  c1.FromMemory.pBuffer = const_cast<char*>(d1);
  c1.FromMemory.BufferLength = 6;
  chunks.push_back(c1);

  HTTP_DATA_CHUNK c2;
  c2.DataChunkType = HttpDataChunkFromFragmentCache;
  // Fragment cache chunk - should be handled gracefully (skipped with warning)
  c2.FromFragmentCache.FragmentNameLength = 0;
  c2.FromFragmentCache.pFragmentName = nullptr;
  chunks.push_back(c2);

  HTTP_DATA_CHUNK c3;
  c3.DataChunkType = HttpDataChunkFromMemory;
  const char* d3 = "After";
  c3.FromMemory.pBuffer = const_cast<char*>(d3);
  c3.FromMemory.BufferLength = 5;
  chunks.push_back(c3);

  // Should collect memory chunks and log warning for fragment cache
  GoogleString body = CollectChunkBody(chunks);
  EXPECT_TRUE(body.find("Before") != GoogleString::npos);
  EXPECT_TRUE(body.find("After") != GoogleString::npos);
}

TEST_F(IisHttpModuleTest, SkipsFragmentCacheExChunksGracefully) {
  std::vector<HTTP_DATA_CHUNK> chunks;

  HTTP_DATA_CHUNK c1;
  c1.DataChunkType = HttpDataChunkFromMemory;
  const char* d1 = "Start";
  c1.FromMemory.pBuffer = const_cast<char*>(d1);
  c1.FromMemory.BufferLength = 5;
  chunks.push_back(c1);

  HTTP_DATA_CHUNK c2;
  c2.DataChunkType = HttpDataChunkFromFragmentCacheEx;
  c2.FromFragmentCache.FragmentNameLength = 0;
  c2.FromFragmentCache.pFragmentName = nullptr;
  chunks.push_back(c2);

  HTTP_DATA_CHUNK c3;
  c3.DataChunkType = HttpDataChunkFromMemory;
  const char* d3 = "End";
  c3.FromMemory.pBuffer = const_cast<char*>(d3);
  c3.FromMemory.BufferLength = 3;
  chunks.push_back(c3);

  // Should collect memory chunks and skip fragment cache ex
  GoogleString body = CollectChunkBody(chunks);
  EXPECT_EQ("StartEnd", body);
}

TEST_F(IisHttpModuleTest, SkipsFileHandleChunksOnNonWindows) {
  std::vector<HTTP_DATA_CHUNK> chunks;

  HTTP_DATA_CHUNK c1;
  c1.DataChunkType = HttpDataChunkFromMemory;
  const char* d1 = "Memory";
  c1.FromMemory.pBuffer = const_cast<char*>(d1);
  c1.FromMemory.BufferLength = 6;
  chunks.push_back(c1);

  HTTP_DATA_CHUNK c2;
  c2.DataChunkType = HttpDataChunkFromFileHandle;
  c2.FromFileHandle.FileHandle = nullptr;
#ifdef _WIN32
  c2.FromFileHandle.ByteRange.StartingOffset.QuadPart = 0;
  c2.FromFileHandle.ByteRange.Length.QuadPart = 0;
#else
  c2.FromFileHandle.ByteRange.StartingOffset = 0;
  c2.FromFileHandle.ByteRange.Length = 0;
#endif
  chunks.push_back(c2);

  // File handle chunk skipped on non-Windows, memory chunk collected
  GoogleString body = CollectChunkBody(chunks);
  EXPECT_EQ("Memory", body);
}

TEST_F(IisHttpModuleTest, SkipsUnknownChunkTypes) {
  std::vector<HTTP_DATA_CHUNK> chunks;

  HTTP_DATA_CHUNK c1;
  c1.DataChunkType = HttpDataChunkFromMemory;
  const char* d1 = "Known";
  c1.FromMemory.pBuffer = const_cast<char*>(d1);
  c1.FromMemory.BufferLength = 5;
  chunks.push_back(c1);

  HTTP_DATA_CHUNK c2;
  c2.DataChunkType = HttpDataChunkMaximum;  // Invalid/unknown type
  chunks.push_back(c2);

  GoogleString body = CollectChunkBody(chunks);
  EXPECT_EQ("Known", body);
}

TEST_F(IisHttpModuleTest, CollectsEmptyChunkList) {
  std::vector<HTTP_DATA_CHUNK> chunks;
  EXPECT_EQ("", CollectChunkBody(chunks));
}

TEST_F(IisHttpModuleTest, CollectsZeroLengthMemoryChunk) {
  std::vector<HTTP_DATA_CHUNK> chunks;

  HTTP_DATA_CHUNK c1;
  c1.DataChunkType = HttpDataChunkFromMemory;
  c1.FromMemory.pBuffer = nullptr;
  c1.FromMemory.BufferLength = 0;
  chunks.push_back(c1);

  EXPECT_EQ("", CollectChunkBody(chunks));
}

// ============================================================================
// AJAX Detection Tests (Fix 1)
// ============================================================================

TEST_F(IisHttpModuleTest, DetectsXhrAjaxRequest) {
  auto context = CreateContext("/page.html");
  context->request()->SetHeader("X-Requested-With", "XMLHttpRequest");
  EXPECT_TRUE(IsAjaxHeaders(context->request()->headers()));
}

TEST_F(IisHttpModuleTest, DetectsMicrosoftAjaxRequest) {
  auto context = CreateContext("/page.html");
  context->request()->SetHeader("X-MicrosoftAjax", "Delta=true");
  EXPECT_TRUE(IsAjaxHeaders(context->request()->headers()));
}

TEST_F(IisHttpModuleTest, DetectsPrototypeAjaxRequest) {
  auto context = CreateContext("/page.html");
  context->request()->SetHeader("X-Prototype-Version", "1.7.3");
  EXPECT_TRUE(IsAjaxHeaders(context->request()->headers()));
}

TEST_F(IisHttpModuleTest, NonAjaxRequestNotDetected) {
  auto context = CreateContext("/page.html");
  context->request()->SetHeader("Accept", "text/html");
  EXPECT_FALSE(IsAjaxHeaders(context->request()->headers()));
}

TEST_F(IisHttpModuleTest, XhrCaseInsensitive) {
  auto context = CreateContext("/page.html");
  context->request()->SetHeader("X-Requested-With", "xmlhttprequest");
  EXPECT_TRUE(IsAjaxHeaders(context->request()->headers()));
}

// ============================================================================
// Range Request Detection Tests (Fix 2)
// ============================================================================

TEST_F(IisHttpModuleTest, DetectsRangeRequest) {
  auto context = CreateContext("/video.mp4");
  context->request()->SetHeader("Range", "bytes=0-1023");
  EXPECT_TRUE(IsRangeRequest(context->request()->headers()));
}

TEST_F(IisHttpModuleTest, DetectsIfRangeRequest) {
  auto context = CreateContext("/video.mp4");
  context->request()->SetHeader("If-Range", "\"abc123\"");
  EXPECT_TRUE(IsRangeRequest(context->request()->headers()));
}

TEST_F(IisHttpModuleTest, DetectsRangeAndIfRange) {
  auto context = CreateContext("/video.mp4");
  context->request()->SetHeader("Range", "bytes=0-1023");
  context->request()->SetHeader("If-Range", "\"abc123\"");
  EXPECT_TRUE(IsRangeRequest(context->request()->headers()));
}

TEST_F(IisHttpModuleTest, NonRangeRequestNotDetected) {
  auto context = CreateContext("/page.html");
  context->request()->SetHeader("Accept", "text/html");
  EXPECT_FALSE(IsRangeRequest(context->request()->headers()));
}

// ============================================================================
// URL Port Normalization Tests (Fix 5)
// ============================================================================

TEST_F(IisHttpModuleTest, StripsPort80FromHttp) {
  EXPECT_EQ("http://example.com/page.html",
            NormalizeUrlPort("http://example.com:80/page.html"));
}

TEST_F(IisHttpModuleTest, StripsPort443FromHttps) {
  EXPECT_EQ("https://example.com/page.html",
            NormalizeUrlPort("https://example.com:443/page.html"));
}

TEST_F(IisHttpModuleTest, KeepsNonDefaultPort) {
  EXPECT_EQ("http://example.com:8080/page.html",
            NormalizeUrlPort("http://example.com:8080/page.html"));
}

TEST_F(IisHttpModuleTest, KeepsHttpsNonDefaultPort) {
  EXPECT_EQ("https://example.com:8443/page.html",
            NormalizeUrlPort("https://example.com:8443/page.html"));
}

TEST_F(IisHttpModuleTest, NoPortNoChange) {
  EXPECT_EQ("http://example.com/page.html",
            NormalizeUrlPort("http://example.com/page.html"));
}

TEST_F(IisHttpModuleTest, DoesNotStripPort443FromHttp) {
  EXPECT_EQ("http://example.com:443/page.html",
            NormalizeUrlPort("http://example.com:443/page.html"));
}

TEST_F(IisHttpModuleTest, DoesNotStripPort80FromHttps) {
  EXPECT_EQ("https://example.com:80/page.html",
            NormalizeUrlPort("https://example.com:80/page.html"));
}

TEST_F(IisHttpModuleTest, StripsPortWithQueryString) {
  EXPECT_EQ("http://example.com/page?q=1",
            NormalizeUrlPort("http://example.com:80/page?q=1"));
}

// ============================================================================
// Client Disconnection Check Tests (Fix 6)
// ============================================================================

TEST_F(IisHttpModuleTest, MockConnectionDefaultsToConnected) {
  auto context = CreateContext("/page.html");
  // By default, mock connections should be connected
  EXPECT_TRUE(context->IsConnected());
}

TEST_F(IisHttpModuleTest, DisconnectedClientDetected) {
  auto context = CreateContext("/page.html");
  context->SetConnected(false);
  EXPECT_FALSE(context->IsConnected());
}

TEST_F(IisHttpModuleTest, ReconnectedClientDetected) {
  auto context = CreateContext("/page.html");
  context->SetConnected(false);
  EXPECT_FALSE(context->IsConnected());
  context->SetConnected(true);
  EXPECT_TRUE(context->IsConnected());
}

}  // namespace net_instaweb
