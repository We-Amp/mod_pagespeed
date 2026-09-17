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

// Unit tests for IisBaseFetch - AsyncFetch implementation for IIS responses.
//
// Note: IisBaseFetch requires IisRequestContext which is only fully functional
// on Windows with the IIS SDK. These tests focus on:
// 1. Testing the mock response infrastructure that IisBaseFetch would use
// 2. Documenting expected behavior and API contracts
// 3. Verifying helper functions for response manipulation

#include <memory>

#include "gtest/gtest.h"
#include "pagespeed/kernel/base/string_util.h"
#include "pagespeed/kernel/http/response_headers.h"
#include "test/pagespeed/iis/iis_test_base.h"
#include "test/pagespeed/iis/mock_iis.h"

namespace net_instaweb {

// IisBaseFetchTest uses IisTestBase::CreateContext() for test setup
class IisBaseFetchTest : public IisTestBase {
 protected:
  void SetUp() override {
    IisTestBase::SetUp();
    context_ = CreateContext("/test.html");
  }

  void TearDown() override {
    context_.reset();
    IisTestBase::TearDown();
  }

  std::unique_ptr<MockHttpContext> context_;
};

// ============================================================================
// Response Header Tests
// ============================================================================

TEST_F(IisBaseFetchTest, ResponseHeadersAreAccessible) {
  // Verify response headers can be set and accessed
  context_->response()->SetHeader("Content-Type", "text/html");
  EXPECT_EQ("text/html", context_->response()->GetHeader("Content-Type"));
}

TEST_F(IisBaseFetchTest, SetMultipleHeaders) {
  context_->response()->SetHeader("Content-Type", "text/html; charset=utf-8");
  context_->response()->SetHeader("Cache-Control", "no-cache");
  context_->response()->SetHeader("X-Custom-Header", "custom-value");

  EXPECT_EQ("text/html; charset=utf-8",
            context_->response()->GetHeader("Content-Type"));
  EXPECT_EQ("no-cache", context_->response()->GetHeader("Cache-Control"));
  EXPECT_EQ("custom-value", context_->response()->GetHeader("X-Custom-Header"));
}

TEST_F(IisBaseFetchTest, OverwriteHeader) {
  context_->response()->SetHeader("Content-Type", "text/plain");
  EXPECT_EQ("text/plain", context_->response()->GetHeader("Content-Type"));

  // Overwrite with new value
  context_->response()->SetHeader("Content-Type", "text/html");
  EXPECT_EQ("text/html", context_->response()->GetHeader("Content-Type"));
}

TEST_F(IisBaseFetchTest, HeadersMapIsAccessible) {
  context_->response()->SetHeader("Content-Type", "text/html");
  context_->response()->SetHeader("Cache-Control", "no-cache");

  const auto& headers = context_->response()->headers();
  EXPECT_EQ(2u, headers.size());
  EXPECT_EQ("text/html", headers.at("Content-Type"));
  EXPECT_EQ("no-cache", headers.at("Cache-Control"));
}

TEST_F(IisBaseFetchTest, MissingHeaderReturnsEmpty) {
  EXPECT_EQ("", context_->response()->GetHeader("X-NonExistent"));
}

TEST_F(IisBaseFetchTest, ResponseHeadersObjectSync) {
  // Verify ResponseHeaders object is kept in sync
  context_->response()->SetHeader("Content-Type", "text/html");

  const ResponseHeaders& resp_headers = context_->response()->response_headers();
  ConstStringStarVector values;
  resp_headers.Lookup("Content-Type", &values);
  ASSERT_EQ(1u, values.size());
  EXPECT_EQ("text/html", *values[0]);
}

// ============================================================================
// Status Code Tests
// ============================================================================

TEST_F(IisBaseFetchTest, DefaultStatusIs200) {
  EXPECT_EQ(200, context_->response()->GetStatus());
  EXPECT_EQ("OK", context_->response()->GetStatusReason());
}

TEST_F(IisBaseFetchTest, SetStatusCode) {
  context_->response()->SetStatus(404, "Not Found");
  EXPECT_EQ(404, context_->response()->GetStatus());
  EXPECT_EQ("Not Found", context_->response()->GetStatusReason());
}

TEST_F(IisBaseFetchTest, SetRedirectStatus) {
  context_->response()->SetStatus(301, "Moved Permanently");
  EXPECT_EQ(301, context_->response()->GetStatus());
  EXPECT_EQ("Moved Permanently", context_->response()->GetStatusReason());
}

TEST_F(IisBaseFetchTest, SetServerErrorStatus) {
  context_->response()->SetStatus(500, "Internal Server Error");
  EXPECT_EQ(500, context_->response()->GetStatus());
  EXPECT_EQ("Internal Server Error", context_->response()->GetStatusReason());
}

TEST_F(IisBaseFetchTest, StatusReasonCanBeCustom) {
  context_->response()->SetStatus(200, "Everything is fine");
  EXPECT_EQ(200, context_->response()->GetStatus());
  EXPECT_EQ("Everything is fine", context_->response()->GetStatusReason());
}

// ============================================================================
// Content Writing Tests
// ============================================================================

TEST_F(IisBaseFetchTest, WriteContentSucceeds) {
  // Test that content can be written to the response
  const char* content = "<html><body>Hello, World!</body></html>";
  context_->response()->AppendBody(content);
  EXPECT_EQ(content, context_->response()->body());
}

TEST_F(IisBaseFetchTest, WriteMultipleChunks) {
  // Test that multiple writes are accumulated
  context_->response()->AppendBody("<html>");
  context_->response()->AppendBody("<head><title>Test</title></head>");
  context_->response()->AppendBody("<body>");
  context_->response()->AppendBody("<h1>Hello</h1>");
  context_->response()->AppendBody("</body>");
  context_->response()->AppendBody("</html>");

  GoogleString expected =
      "<html><head><title>Test</title></head><body><h1>Hello</h1></body></html>";
  EXPECT_EQ(expected, context_->response()->body());
}

TEST_F(IisBaseFetchTest, WriteEmptyContent) {
  context_->response()->AppendBody("");
  EXPECT_EQ("", context_->response()->body());
}

TEST_F(IisBaseFetchTest, WriteStringPiece) {
  GoogleString content = "Hello, World!";
  StringPiece sp(content);
  context_->response()->AppendBody(sp);
  EXPECT_EQ("Hello, World!", context_->response()->body());
}

TEST_F(IisBaseFetchTest, WriteLargeContent) {
  // Write a large content block
  GoogleString large_content;
  for (int i = 0; i < 10000; ++i) {
    StrAppend(&large_content, "Line ", IntegerToString(i), "\n");
  }
  context_->response()->AppendBody(large_content);
  EXPECT_EQ(large_content, context_->response()->body());
}

TEST_F(IisBaseFetchTest, WriteBinaryContent) {
  // Binary content should be preserved
  GoogleString binary;
  binary.push_back('\x00');
  binary.push_back('\x01');
  binary.push_back('\xFF');
  binary.push_back('\xFE');

  context_->response()->AppendBody(binary);
  EXPECT_EQ(binary, context_->response()->body());
}

// ============================================================================
// WriteEntityChunks Tests (IIS-specific write method)
// ============================================================================

TEST_F(IisBaseFetchTest, WriteEntityChunksSuccess) {
  const char* data = "Test data";
  HRESULT hr = context_->response()->WriteEntityChunks(data, strlen(data));
  EXPECT_TRUE(SUCCEEDED(hr));
  EXPECT_EQ("Test data", context_->response()->body());
}

TEST_F(IisBaseFetchTest, WriteEntityChunksMultipleCalls) {
  EXPECT_TRUE(SUCCEEDED(
      context_->response()->WriteEntityChunks("Hello", 5)));
  EXPECT_TRUE(SUCCEEDED(
      context_->response()->WriteEntityChunks(", ", 2)));
  EXPECT_TRUE(SUCCEEDED(
      context_->response()->WriteEntityChunks("World!", 6)));

  EXPECT_EQ("Hello, World!", context_->response()->body());
}

TEST_F(IisBaseFetchTest, WriteEntityChunksWithNullBytes) {
  const char data[] = "Hello\x00World";
  HRESULT hr = context_->response()->WriteEntityChunks(data, sizeof(data) - 1);
  EXPECT_TRUE(SUCCEEDED(hr));
  EXPECT_EQ(sizeof(data) - 1, context_->response()->body().size());
}

// ============================================================================
// Body Clear and Reset Tests
// ============================================================================

TEST_F(IisBaseFetchTest, ClearBody) {
  context_->response()->AppendBody("Some content");
  EXPECT_FALSE(context_->response()->body().empty());

  context_->response()->ClearBody();
  EXPECT_TRUE(context_->response()->body().empty());
}

TEST_F(IisBaseFetchTest, ClearAndRewrite) {
  context_->response()->AppendBody("Original content");
  context_->response()->ClearBody();
  context_->response()->AppendBody("New content");

  EXPECT_EQ("New content", context_->response()->body());
}

// ============================================================================
// Completion Status Tests
// ============================================================================

TEST_F(IisBaseFetchTest, DefaultNotCompleted) {
  EXPECT_FALSE(context_->response()->completed());
}

TEST_F(IisBaseFetchTest, MarkAsCompleted) {
  context_->response()->set_completed(true);
  EXPECT_TRUE(context_->response()->completed());
}

TEST_F(IisBaseFetchTest, CompletedCanBeReset) {
  context_->response()->set_completed(true);
  EXPECT_TRUE(context_->response()->completed());

  context_->response()->set_completed(false);
  EXPECT_FALSE(context_->response()->completed());
}

// ============================================================================
// Complete Response Flow Tests
// ============================================================================

TEST_F(IisBaseFetchTest, CompleteResponseFlow) {
  // Test the full flow: status -> headers -> content -> complete
  context_->response()->SetStatus(200, "OK");
  context_->response()->SetHeader("Content-Type", "text/html; charset=utf-8");
  context_->response()->SetHeader("Cache-Control", "max-age=3600");

  context_->response()->AppendBody("<!DOCTYPE html>");
  context_->response()->AppendBody("<html>");
  context_->response()->AppendBody("<head><title>Test</title></head>");
  context_->response()->AppendBody("<body><p>Hello</p></body>");
  context_->response()->AppendBody("</html>");

  context_->response()->set_completed(true);

  // Verify complete response
  EXPECT_EQ(200, context_->response()->GetStatus());
  EXPECT_EQ("OK", context_->response()->GetStatusReason());
  EXPECT_EQ("text/html; charset=utf-8",
            context_->response()->GetHeader("Content-Type"));
  EXPECT_EQ("max-age=3600",
            context_->response()->GetHeader("Cache-Control"));
  EXPECT_EQ("<!DOCTYPE html><html><head><title>Test</title></head>"
            "<body><p>Hello</p></body></html>",
            context_->response()->body());
  EXPECT_TRUE(context_->response()->completed());
}

TEST_F(IisBaseFetchTest, ErrorResponseFlow) {
  // Test error response flow
  context_->response()->SetStatus(404, "Not Found");
  context_->response()->SetHeader("Content-Type", "text/html");
  context_->response()->AppendBody("<html><body><h1>404 Not Found</h1></body></html>");
  context_->response()->set_completed(true);

  EXPECT_EQ(404, context_->response()->GetStatus());
  EXPECT_TRUE(context_->response()->completed());
}

TEST_F(IisBaseFetchTest, RedirectResponseFlow) {
  // Test redirect response (no body)
  context_->response()->SetStatus(301, "Moved Permanently");
  context_->response()->SetHeader("Location", "https://example.com/new-location");
  context_->response()->set_completed(true);

  EXPECT_EQ(301, context_->response()->GetStatus());
  EXPECT_EQ("https://example.com/new-location",
            context_->response()->GetHeader("Location"));
  EXPECT_TRUE(context_->response()->body().empty());
  EXPECT_TRUE(context_->response()->completed());
}

// ============================================================================
// IisBaseFetch Specific Tests (require Windows IIS)
// ============================================================================
// The following tests document expected IisBaseFetch behavior.
// Full implementation testing requires IisRequestContext on Windows.

TEST_F(IisBaseFetchTest, InitialState) {
  // Test that fetch is in correct initial state
  // Note: Full test requires Windows IIS and IisRequestContext
  // Expected initial state:
  // - headers_sent() should be false
  // - success() should be undefined (only valid after Done)
}

TEST_F(IisBaseFetchTest, HandleHeadersComplete) {
  // Note: Full test requires Windows IIS
  // HandleHeadersComplete should:
  // - Set headers_sent() to true
  // - Write headers to IIS response
  // - Be called only once
}

TEST_F(IisBaseFetchTest, HandleWrite) {
  // Note: Full test requires Windows IIS
  // HandleWrite should:
  // - Write content to IIS response buffer
  // - Return true on success
  // - Call SendHeaders() if not already sent
}

TEST_F(IisBaseFetchTest, HandleFlush) {
  // Note: Full test requires Windows IIS
  // HandleFlush should:
  // - Flush any buffered content to client
  // - Return true on success
}

TEST_F(IisBaseFetchTest, DoneWithSuccess) {
  // Note: Full test requires Windows IIS
  // HandleDone(success=true) should:
  // - Set success() to true
  // - Complete any pending async operations
}

TEST_F(IisBaseFetchTest, DoneWithFailure) {
  // Note: Full test requires Windows IIS
  // HandleDone(success=false) should:
  // - Set success() to false
  // - May set error status on response
}

TEST_F(IisBaseFetchTest, HeadersSentTracking) {
  // Note: Full test requires Windows IIS
  // headers_sent() should:
  // - Return false before HandleHeadersComplete or first HandleWrite
  // - Return true after headers are sent
}

TEST_F(IisBaseFetchTest, SuccessTracking) {
  // Note: Full test requires Windows IIS
  // success() should:
  // - Only be valid after HandleDone is called
  // - Reflect the success parameter passed to HandleDone
}

// ============================================================================
// Response Headers Integration Tests
// ============================================================================

TEST_F(IisBaseFetchTest, ContentTypeWithCharset) {
  context_->response()->SetHeader("Content-Type", "text/html; charset=utf-8");
  EXPECT_EQ("text/html; charset=utf-8",
            context_->response()->GetHeader("Content-Type"));
}

TEST_F(IisBaseFetchTest, CacheControlHeaders) {
  context_->response()->SetHeader("Cache-Control", "public, max-age=31536000");
  context_->response()->SetHeader("ETag", "\"abc123\"");
  context_->response()->SetHeader("Last-Modified", "Wed, 21 Oct 2025 07:28:00 GMT");

  EXPECT_EQ("public, max-age=31536000",
            context_->response()->GetHeader("Cache-Control"));
  EXPECT_EQ("\"abc123\"", context_->response()->GetHeader("ETag"));
  EXPECT_EQ("Wed, 21 Oct 2025 07:28:00 GMT",
            context_->response()->GetHeader("Last-Modified"));
}

TEST_F(IisBaseFetchTest, CompressionHeaders) {
  context_->response()->SetHeader("Content-Encoding", "gzip");
  context_->response()->SetHeader("Vary", "Accept-Encoding");

  EXPECT_EQ("gzip", context_->response()->GetHeader("Content-Encoding"));
  EXPECT_EQ("Accept-Encoding", context_->response()->GetHeader("Vary"));
}

TEST_F(IisBaseFetchTest, PageSpeedHeaders) {
  // PageSpeed-specific headers
  context_->response()->SetHeader("X-Mod-Pagespeed", "1.0.0.0");
  context_->response()->SetHeader("X-Page-Speed", "1.0.0.0");

  EXPECT_EQ("1.0.0.0", context_->response()->GetHeader("X-Mod-Pagespeed"));
  EXPECT_EQ("1.0.0.0", context_->response()->GetHeader("X-Page-Speed"));
}

// ============================================================================
// Concurrent Access Tests (Document expected behavior)
// ============================================================================

TEST_F(IisBaseFetchTest, MultipleContextsAreIndependent) {
  // Create two independent contexts
  auto context1 = CreateContext("/page1.html");
  auto context2 = CreateContext("/page2.html");

  context1->response()->SetStatus(200, "OK");
  context1->response()->SetHeader("Content-Type", "text/html");
  context1->response()->AppendBody("Page 1");

  context2->response()->SetStatus(404, "Not Found");
  context2->response()->SetHeader("Content-Type", "text/plain");
  context2->response()->AppendBody("Page 2 not found");

  // Verify they are independent
  EXPECT_EQ(200, context1->response()->GetStatus());
  EXPECT_EQ(404, context2->response()->GetStatus());
  EXPECT_EQ("text/html", context1->response()->GetHeader("Content-Type"));
  EXPECT_EQ("text/plain", context2->response()->GetHeader("Content-Type"));
  EXPECT_EQ("Page 1", context1->response()->body());
  EXPECT_EQ("Page 2 not found", context2->response()->body());
}

}  // namespace net_instaweb
