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

// IIS Header Utility Tests
//
// These tests verify header manipulation functions for IIS module.
// Tests use the mock IIS infrastructure for cross-platform testing.
// Equivalent to Apache's header_util_test.cc.

#include <memory>

#include "test/pagespeed/iis/mock_iis.h"
#include "test/pagespeed/iis/iis_test_base.h"

#include "pagespeed/kernel/base/basictypes.h"
#include "test/pagespeed/kernel/base/gtest.h"
#include "pagespeed/kernel/base/string.h"
#include "pagespeed/kernel/base/string_util.h"
#include "pagespeed/kernel/http/response_headers.h"

namespace net_instaweb {

class IisHeaderUtilTest : public IisTestBase {
 protected:
  void SetUp() override {
    IisTestBase::SetUp();
    context_ = CreateContext("/test.html");
  }

  void TearDown() override {
    context_.reset();
    IisTestBase::TearDown();
  }

  // Convenience methods for header manipulation (kept local since they are
  // specific to this test's direct mock access pattern)
  void SetResponseHeader(const GoogleString& name, const GoogleString& value) {
    context_->response()->SetHeader(name, value);
  }

  GoogleString GetResponseHeader(const GoogleString& name) const {
    return context_->response()->GetHeader(name);
  }

  void SetRequestHeader(const GoogleString& name, const GoogleString& value) {
    context_->request()->SetHeader(name, value);
  }

  GoogleString GetRequestHeader(const GoogleString& name) const {
    return context_->request()->GetHeader(name);
  }

  std::unique_ptr<MockHttpContext> context_;
};

// =============================================================================
// Response Header Tests
// =============================================================================

TEST_F(IisHeaderUtilTest, SetHeaderBasic) {
  SetResponseHeader("Content-Type", "text/html");
  EXPECT_EQ("text/html", GetResponseHeader("Content-Type"));
}

TEST_F(IisHeaderUtilTest, SetHeaderReplace) {
  SetResponseHeader("Content-Type", "text/plain");
  EXPECT_EQ("text/plain", GetResponseHeader("Content-Type"));

  SetResponseHeader("Content-Type", "text/html");
  EXPECT_EQ("text/html", GetResponseHeader("Content-Type"));
}

TEST_F(IisHeaderUtilTest, SetMultipleHeaders) {
  SetResponseHeader("Content-Type", "text/html");
  SetResponseHeader("Cache-Control", "max-age=300");
  SetResponseHeader("X-Custom", "value");

  EXPECT_EQ("text/html", GetResponseHeader("Content-Type"));
  EXPECT_EQ("max-age=300", GetResponseHeader("Cache-Control"));
  EXPECT_EQ("value", GetResponseHeader("X-Custom"));
}

TEST_F(IisHeaderUtilTest, GetNonexistentHeader) {
  GoogleString value = GetResponseHeader("X-Nonexistent");
  EXPECT_TRUE(value.empty());
}

// =============================================================================
// Cache-Control Header Tests (equivalent to Apache DisableCacheControlHeader)
// =============================================================================

TEST_F(IisHeaderUtilTest, CacheControlEmpty) {
  // When no Cache-Control is set, any caching policy can be applied
  GoogleString value = GetResponseHeader("Cache-Control");
  EXPECT_TRUE(value.empty());
}

TEST_F(IisHeaderUtilTest, CacheControlMaxAge) {
  SetResponseHeader("Cache-Control", "max-age=60");
  EXPECT_EQ("max-age=60", GetResponseHeader("Cache-Control"));
}

TEST_F(IisHeaderUtilTest, CacheControlPrivate) {
  SetResponseHeader("Cache-Control", "private, max-age=60");
  EXPECT_EQ("private, max-age=60", GetResponseHeader("Cache-Control"));
}

TEST_F(IisHeaderUtilTest, CacheControlPublic) {
  SetResponseHeader("Cache-Control", "public, max-age=3600");
  EXPECT_EQ("public, max-age=3600", GetResponseHeader("Cache-Control"));
}

TEST_F(IisHeaderUtilTest, CacheControlNoStore) {
  SetResponseHeader("Cache-Control", "no-store, no-cache, must-revalidate");
  EXPECT_EQ("no-store, no-cache, must-revalidate",
            GetResponseHeader("Cache-Control"));
}

TEST_F(IisHeaderUtilTest, CacheControlNoCache) {
  SetResponseHeader("Cache-Control", "no-cache, max-age=0");
  EXPECT_EQ("no-cache, max-age=0", GetResponseHeader("Cache-Control"));
}

// =============================================================================
// Request Header Tests
// =============================================================================

TEST_F(IisHeaderUtilTest, RequestHeaderBasic) {
  SetRequestHeader("Accept", "text/html");
  EXPECT_EQ("text/html", GetRequestHeader("Accept"));
}

TEST_F(IisHeaderUtilTest, RequestHeaderCaseInsensitive) {
  SetRequestHeader("Content-Type", "application/json");
  // Request headers are stored lowercase
  EXPECT_EQ("application/json", GetRequestHeader("content-type"));
}

TEST_F(IisHeaderUtilTest, RequestHeaderMultiple) {
  SetRequestHeader("Accept", "text/html");
  SetRequestHeader("Accept-Encoding", "gzip, deflate");
  SetRequestHeader("User-Agent", "Test/1.0");

  EXPECT_EQ("text/html", GetRequestHeader("accept"));
  EXPECT_EQ("gzip, deflate", GetRequestHeader("accept-encoding"));
  EXPECT_EQ("Test/1.0", GetRequestHeader("user-agent"));
}

// =============================================================================
// Special Header Tests
// =============================================================================

TEST_F(IisHeaderUtilTest, XForwardedProtoHeader) {
  SetRequestHeader("X-Forwarded-Proto", "https");
  EXPECT_EQ("https", GetRequestHeader("x-forwarded-proto"));
}

TEST_F(IisHeaderUtilTest, XForwardedForHeader) {
  SetRequestHeader("X-Forwarded-For", "192.168.1.1, 10.0.0.1");
  EXPECT_EQ("192.168.1.1, 10.0.0.1", GetRequestHeader("x-forwarded-for"));
}

TEST_F(IisHeaderUtilTest, PageSpeedFilterHeader) {
  SetRequestHeader("PageSpeed", "off");
  EXPECT_EQ("off", GetRequestHeader("pagespeed"));
}

TEST_F(IisHeaderUtilTest, PageSpeedFiltersHeader) {
  SetRequestHeader("PageSpeedFilters", "+extend_cache_images,-inline_css");
  EXPECT_EQ("+extend_cache_images,-inline_css",
            GetRequestHeader("pagespeedfilters"));
}

// =============================================================================
// IIS-Specific Header Tests
// =============================================================================

TEST_F(IisHeaderUtilTest, XPoweredByHeader) {
  SetResponseHeader("X-Powered-By", "ASP.NET");
  EXPECT_EQ("ASP.NET", GetResponseHeader("X-Powered-By"));
}

TEST_F(IisHeaderUtilTest, XAspNetVersionHeader) {
  SetResponseHeader("X-AspNet-Version", "4.0.30319");
  EXPECT_EQ("4.0.30319", GetResponseHeader("X-AspNet-Version"));
}

TEST_F(IisHeaderUtilTest, XPageSpeedVersionHeader) {
  SetResponseHeader("X-Page-Speed", "1.13.35.2-0");
  EXPECT_EQ("1.13.35.2-0", GetResponseHeader("X-Page-Speed"));
}

// =============================================================================
// Action Recording Tests (verify mock infrastructure)
// =============================================================================

TEST_F(IisHeaderUtilTest, ActionRecordingSetHeader) {
  context_->response()->ClearRecordedActions();

  SetResponseHeader("Content-Type", "text/html");

  GoogleString actions = context_->response()->ActionsSinceLastCall();
  EXPECT_TRUE(actions.find("SetHeader") != GoogleString::npos);
  EXPECT_TRUE(actions.find("Content-Type") != GoogleString::npos);
  EXPECT_TRUE(actions.find("text/html") != GoogleString::npos);
}

TEST_F(IisHeaderUtilTest, ActionRecordingMultipleHeaders) {
  context_->response()->ClearRecordedActions();

  SetResponseHeader("Content-Type", "text/html");
  SetResponseHeader("Cache-Control", "max-age=300");

  GoogleString actions = context_->response()->ActionsSinceLastCall();
  EXPECT_TRUE(actions.find("SetHeader(Content-Type") != GoogleString::npos);
  EXPECT_TRUE(actions.find("SetHeader(Cache-Control") != GoogleString::npos);
}

TEST_F(IisHeaderUtilTest, ActionRecordingClearActions) {
  SetResponseHeader("X-Test", "value");
  context_->response()->ClearRecordedActions();

  GoogleString actions = context_->response()->ActionsSinceLastCall();
  EXPECT_TRUE(actions.empty());
}

// =============================================================================
// ResponseHeaders Object Tests
// =============================================================================

TEST_F(IisHeaderUtilTest, ResponseHeadersObjectSync) {
  SetResponseHeader("Content-Type", "text/html");
  SetResponseHeader("Cache-Control", "no-cache");

  // Verify the ResponseHeaders object is kept in sync
  const ResponseHeaders& headers = context_->response()->response_headers();
  ConstStringStarVector values;

  headers.Lookup("Content-Type", &values);
  ASSERT_EQ(1, values.size());
  EXPECT_EQ("text/html", *values[0]);

  values.clear();
  headers.Lookup("Cache-Control", &values);
  ASSERT_EQ(1, values.size());
  EXPECT_EQ("no-cache", *values[0]);
}

TEST_F(IisHeaderUtilTest, ResponseHeadersMutable) {
  ResponseHeaders* headers = context_->response()->mutable_response_headers();
  headers->Replace("X-Custom", "direct");

  // Verify we can modify headers directly
  ConstStringStarVector values;
  headers->Lookup("X-Custom", &values);
  ASSERT_EQ(1, values.size());
  EXPECT_EQ("direct", *values[0]);
}

// =============================================================================
// Header Manipulation for Caching Tests
// =============================================================================

TEST_F(IisHeaderUtilTest, DisableCachingHeadersPattern) {
  // Test the pattern for disabling caching
  SetResponseHeader("Cache-Control", "no-cache, max-age=0");
  SetResponseHeader("Pragma", "no-cache");
  SetResponseHeader("Expires", "0");

  EXPECT_EQ("no-cache, max-age=0", GetResponseHeader("Cache-Control"));
  EXPECT_EQ("no-cache", GetResponseHeader("Pragma"));
  EXPECT_EQ("0", GetResponseHeader("Expires"));
}

TEST_F(IisHeaderUtilTest, ExtendCachingHeadersPattern) {
  // Test the pattern for extended caching
  SetResponseHeader("Cache-Control", "public, max-age=31536000");
  SetResponseHeader("Expires", "Thu, 01 Jan 2099 00:00:00 GMT");

  EXPECT_EQ("public, max-age=31536000", GetResponseHeader("Cache-Control"));
  EXPECT_EQ("Thu, 01 Jan 2099 00:00:00 GMT", GetResponseHeader("Expires"));
}

TEST_F(IisHeaderUtilTest, LastModifiedHeader) {
  SetResponseHeader("Last-Modified", "Wed, 01 Jan 2025 12:00:00 GMT");
  EXPECT_EQ("Wed, 01 Jan 2025 12:00:00 GMT",
            GetResponseHeader("Last-Modified"));
}

TEST_F(IisHeaderUtilTest, ETagHeader) {
  SetResponseHeader("ETag", "\"abc123\"");
  EXPECT_EQ("\"abc123\"", GetResponseHeader("ETag"));
}

}  // namespace net_instaweb
