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

// Tests for the mock IIS infrastructure.
// These tests verify that the mocking infrastructure works correctly
// and can be used for IIS module testing.

#include "test/pagespeed/iis/mock_iis.h"

#include "gtest/gtest.h"
#include "test/pagespeed/iis/iis_test_base.h"

namespace net_instaweb {

class MockIisTest : public IisTestBase {
 protected:
};

TEST_F(MockIisTest, MockHttpRequestSetsUrl) {
  MockHttpRequest request;
  request.SetUrl("/test/page.html?foo=bar");

  EXPECT_STREQ("/test/page.html?foo=bar", request.GetRawHttpRequestUrl());
  EXPECT_EQ("foo=bar", request.query_string());
}

TEST_F(MockIisTest, MockHttpRequestSetsMethod) {
  MockHttpRequest request;
  request.SetMethod("POST");

  EXPECT_EQ("POST", request.GetMethod());
}

TEST_F(MockIisTest, MockHttpRequestSetsHeaders) {
  MockHttpRequest request;
  request.SetHeader("Content-Type", "text/html");
  request.SetHeader("Accept-Encoding", "gzip");

  EXPECT_EQ("text/html", request.GetHeader("Content-Type"));
  EXPECT_EQ("text/html", request.GetHeader("content-type"));  // Case insensitive
  EXPECT_EQ("gzip", request.GetHeader("Accept-Encoding"));
  EXPECT_EQ("", request.GetHeader("Missing-Header"));
}

TEST_F(MockIisTest, MockHttpResponseSetsStatus) {
  MockHttpResponse response;
  response.SetStatus(404, "Not Found");

  EXPECT_EQ(404, response.GetStatus());
  EXPECT_EQ("Not Found", response.GetStatusReason());
}

TEST_F(MockIisTest, MockHttpResponseWritesBody) {
  MockHttpResponse response;
  response.AppendBody("Hello ");
  response.AppendBody("World");

  EXPECT_EQ("Hello World", response.body());
}

TEST_F(MockIisTest, MockHttpResponseSetsHeaders) {
  MockHttpResponse response;
  response.SetHeader("Content-Type", "application/json");
  response.SetHeader("Cache-Control", "no-cache");

  EXPECT_EQ("application/json", response.GetHeader("Content-Type"));
  EXPECT_EQ("no-cache", response.GetHeader("Cache-Control"));
}

TEST_F(MockIisTest, MockHttpContextHasRequestAndResponse) {
  MockHttpContext context;

  context.request()->SetUrl("/api/test");
  context.request()->SetMethod("PUT");
  context.response()->SetStatus(200, "OK");
  context.response()->AppendBody("{\"status\":\"ok\"}");

  EXPECT_STREQ("/api/test", context.request()->GetRawHttpRequestUrl());
  EXPECT_EQ("PUT", context.request()->GetMethod());
  EXPECT_EQ(200, context.response()->GetStatus());
  EXPECT_EQ("{\"status\":\"ok\"}", context.response()->body());
}

TEST_F(MockIisTest, MockHttpContextBuilderCreatesContext) {
  auto context = MockHttpContextBuilder()
      .SetUrl("/page.html")
      .SetMethod("GET")
      .AddRequestHeader("Accept", "text/html")
      .SetSiteId(5)
      .SetSiteName("MySite")
      .Build();

  EXPECT_STREQ("/page.html", context->request()->GetRawHttpRequestUrl());
  EXPECT_EQ("GET", context->request()->GetMethod());
  EXPECT_EQ("text/html", context->request()->GetHeader("Accept"));
  EXPECT_EQ(5u, context->GetSiteId());
  EXPECT_EQ("MySite", context->GetSiteName());
}

TEST_F(MockIisTest, MockAppHostElementAttributes) {
  MockAppHostElement element("testElement");
  element.SetAttribute("enabled", "true");
  element.SetAttribute("path", "/cache");

  EXPECT_EQ("testElement", element.GetName());
  EXPECT_EQ("true", element.GetAttribute("enabled"));
  EXPECT_EQ("/cache", element.GetAttribute("path"));
  EXPECT_TRUE(element.HasAttribute("enabled"));
  EXPECT_FALSE(element.HasAttribute("missing"));
}

TEST_F(MockIisTest, MockAppHostElementChildren) {
  MockAppHostElement parent("parent");
  MockAppHostElement* child1 = new MockAppHostElement("child1");
  MockAppHostElement* child2 = new MockAppHostElement("child2");

  child1->SetAttribute("value", "one");
  child2->SetAttribute("value", "two");

  parent.AddChildElement(child1);
  parent.AddChildElement(child2);

  EXPECT_EQ(2u, parent.GetChildElements().size());
  EXPECT_EQ("one", parent.GetChildElement("child1")->GetAttribute("value"));
  EXPECT_EQ("two", parent.GetChildElement("child2")->GetAttribute("value"));
  EXPECT_EQ(nullptr, parent.GetChildElement("nonexistent"));
}

TEST_F(MockIisTest, MockConfigBuilderCreatesPageSpeedConfig) {
  auto config = MockConfigBuilder()
      .SetEnabled(true)
      .SetFileCachePath("C:\\cache")
      .SetLruCacheSize(5000000)
      .AddEnabledFilter("combine_css")
      .AddEnabledFilter("combine_javascript")
      .AddDisabledFilter("inline_css")
      .SetImageQuality(85)
      .SetWebpQuality(75)
      .SetAdminEnabled(true)
      .SetAdminPath("/ps_admin")
      .Build();

  EXPECT_EQ("pagespeed", config->GetName());

  auto* settings = config->GetChildElement("settings");
  ASSERT_NE(nullptr, settings);
  EXPECT_EQ("true", settings->GetAttribute("enabled"));
  EXPECT_EQ("C:\\cache", settings->GetAttribute("fileCachePath"));

  auto* cache = settings->GetChildElement("cache");
  ASSERT_NE(nullptr, cache);
  EXPECT_EQ("5000000", cache->GetAttribute("lruCacheSizeBytes"));

  auto* filters = settings->GetChildElement("filters");
  ASSERT_NE(nullptr, filters);
  EXPECT_EQ("combine_css,combine_javascript", filters->GetAttribute("enabledFilters"));
  EXPECT_EQ("inline_css", filters->GetAttribute("disabledFilters"));

  auto* images = settings->GetChildElement("images");
  ASSERT_NE(nullptr, images);
  EXPECT_EQ("85", images->GetAttribute("recompressQuality"));
  EXPECT_EQ("75", images->GetAttribute("webpQuality"));

  auto* admin = settings->GetChildElement("admin");
  ASSERT_NE(nullptr, admin);
  EXPECT_EQ("true", admin->GetAttribute("enabled"));
  EXPECT_EQ("/ps_admin", admin->GetAttribute("path"));
}

TEST_F(MockIisTest, IisTestBaseCreatesContext) {
  auto context = CreateContext("/test.html");
  EXPECT_STREQ("/test.html", context->request()->GetRawHttpRequestUrl());
  EXPECT_EQ("GET", context->request()->GetMethod());
}

TEST_F(MockIisTest, PageSpeedUrlDetection) {
  EXPECT_TRUE(IsPageSpeedUrl("/style.pagespeed.cf.ABC123.css"));
  EXPECT_TRUE(IsPageSpeedUrl("/image.pagespeed.ic.XYZ789.png"));
  EXPECT_FALSE(IsPageSpeedUrl("/normal/page.html"));
  EXPECT_FALSE(IsPageSpeedUrl("/style.css"));
}

TEST_F(MockIisTest, PageSpeedUrlCreation) {
  GoogleString ps_url = CreatePageSpeedUrl("/styles/main.css", "cf", "ABC123");
  EXPECT_EQ("/styles/main.pagespeed.cf.ABC123.css", ps_url);

  ps_url = CreatePageSpeedUrl("/scripts/app.js", "jm", "DEF456");
  EXPECT_EQ("/scripts/app.pagespeed.jm.DEF456.js", ps_url);
}

TEST_F(MockIisTest, PageSpeedUrlDecoding) {
  GoogleString original, filter_id, hash;

  EXPECT_TRUE(DecodePageSpeedUrl("/styles/main.pagespeed.cf.ABC123.css",
                                  &original, &filter_id, &hash));
  EXPECT_EQ("/styles/main.css", original);
  EXPECT_EQ("cf", filter_id);
  EXPECT_EQ("ABC123", hash);

  EXPECT_TRUE(DecodePageSpeedUrl("/image.pagespeed.ic.XYZ.png",
                                  &original, &filter_id, &hash));
  EXPECT_EQ("/image.png", original);
  EXPECT_EQ("ic", filter_id);
  EXPECT_EQ("XYZ", hash);

  EXPECT_FALSE(DecodePageSpeedUrl("/normal.css", &original, &filter_id, &hash));
}

}  // namespace net_instaweb
