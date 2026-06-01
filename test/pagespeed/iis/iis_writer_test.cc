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

// IIS Writer Tests
//
// These tests verify response writing behavior for the IIS module.
// Tests use the mock IIS infrastructure with action recording to verify
// the exact sequence of IIS API calls.
// Equivalent to Apache's apache_writer_test.cc.

#include "test/pagespeed/iis/mock_iis.h"
#include "test/pagespeed/iis/iis_test_base.h"

#include "pagespeed/kernel/base/basictypes.h"
#include "test/pagespeed/kernel/base/gtest.h"
#include "pagespeed/kernel/base/string.h"
#include "pagespeed/kernel/base/string_util.h"
#include "pagespeed/kernel/http/response_headers.h"

namespace net_instaweb {

class IisWriterTest : public IisTestBase {
 protected:
  void SetUp() override {
    IisTestBase::SetUp();
    context_ = CreateContext("/test.html");
    InitStandardResponseHeaders(&response_headers_);
  }

  void TearDown() override {
    context_.reset();
    IisTestBase::TearDown();
  }

  // Simulate writing headers to the response
  void OutputHeaders() {
    OutputResponseHeaders(response_headers_, context_->response());
  }

  // Simulate writing body content
  bool Write(StringPiece data) {
    return WriteToResponse(context_->response(), data);
  }

  // Simulate flushing the response
  bool Flush(bool final_flush = false) {
    return FlushResponse(context_->response(), final_flush);
  }

  std::unique_ptr<MockHttpContext> context_;
  ResponseHeaders response_headers_;
};

// =============================================================================
// Basic Writing Tests
// =============================================================================

TEST_F(IisWriterTest, SimpleUsage) {
  context_->response()->ClearRecordedActions();

  OutputHeaders();
  Write("hello world");

  EXPECT_EQ(200, context_->response()->GetStatus());
  EXPECT_EQ("hello world", context_->response()->body());

  // Verify actions were recorded
  GoogleString actions = context_->response()->ActionsSinceLastCall();
  EXPECT_TRUE(actions.find("SetStatus(200, OK)") != GoogleString::npos);
  EXPECT_TRUE(actions.find("SetHeader(Content-Type") != GoogleString::npos);
  EXPECT_TRUE(actions.find("WriteEntityChunks(11 bytes)") != GoogleString::npos);
}

TEST_F(IisWriterTest, WriteAndFlush) {
  context_->response()->ClearRecordedActions();

  OutputHeaders();
  Write("hello");
  Flush(false);
  Write(" world");
  Flush(true);

  EXPECT_EQ("hello world", context_->response()->body());

  GoogleString actions = context_->response()->ActionsSinceLastCall();
  EXPECT_TRUE(actions.find("WriteEntityChunks(5 bytes)") != GoogleString::npos);
  EXPECT_TRUE(actions.find("Flush(partial)") != GoogleString::npos);
  EXPECT_TRUE(actions.find("WriteEntityChunks(6 bytes)") != GoogleString::npos);
  EXPECT_TRUE(actions.find("Flush(final)") != GoogleString::npos);
}

TEST_F(IisWriterTest, MultipleWrites) {
  context_->response()->ClearRecordedActions();

  OutputHeaders();
  Write("line 1\n");
  Write("line 2\n");
  Write("line 3\n");

  EXPECT_EQ("line 1\nline 2\nline 3\n", context_->response()->body());

  GoogleString actions = context_->response()->ActionsSinceLastCall();
  // Should have three WriteEntityChunks calls
  int write_count = 0;
  size_t pos = 0;
  while ((pos = actions.find("WriteEntityChunks", pos)) != GoogleString::npos) {
    write_count++;
    pos++;
  }
  EXPECT_EQ(3, write_count);
}

TEST_F(IisWriterTest, EmptyWrite) {
  context_->response()->ClearRecordedActions();

  OutputHeaders();
  Write("");

  EXPECT_TRUE(context_->response()->body().empty());

  GoogleString actions = context_->response()->ActionsSinceLastCall();
  EXPECT_TRUE(actions.find("WriteEntityChunks(0 bytes)") != GoogleString::npos);
}

// =============================================================================
// Status Code Tests
// =============================================================================

TEST_F(IisWriterTest, Status200OK) {
  response_headers_.set_status_code(200);
  OutputHeaders();
  EXPECT_EQ(200, context_->response()->GetStatus());
  EXPECT_EQ("OK", context_->response()->GetStatusReason());
}

TEST_F(IisWriterTest, Status301Redirect) {
  response_headers_.set_status_code(301);
  response_headers_.Add("Location", "http://example.com/new-page");
  OutputHeaders();

  EXPECT_EQ(301, context_->response()->GetStatus());
  EXPECT_EQ("Moved Permanently", context_->response()->GetStatusReason());
  EXPECT_EQ("http://example.com/new-page",
            context_->response()->GetHeader("Location"));
}

TEST_F(IisWriterTest, Status302Found) {
  response_headers_.set_status_code(302);
  response_headers_.Add("Location", "http://example.com/temp");
  OutputHeaders();

  EXPECT_EQ(302, context_->response()->GetStatus());
  EXPECT_EQ("Found", context_->response()->GetStatusReason());
}

TEST_F(IisWriterTest, Status304NotModified) {
  response_headers_.set_status_code(304);
  OutputHeaders();

  EXPECT_EQ(304, context_->response()->GetStatus());
  EXPECT_EQ("Not Modified", context_->response()->GetStatusReason());
}

TEST_F(IisWriterTest, Status404NotFound) {
  response_headers_.set_status_code(404);
  OutputHeaders();
  Write("Page not found");

  EXPECT_EQ(404, context_->response()->GetStatus());
  EXPECT_EQ("Not Found", context_->response()->GetStatusReason());
  EXPECT_EQ("Page not found", context_->response()->body());
}

TEST_F(IisWriterTest, Status500InternalError) {
  response_headers_.set_status_code(500);
  OutputHeaders();
  Write("Internal server error");

  EXPECT_EQ(500, context_->response()->GetStatus());
  EXPECT_EQ("Internal Server Error", context_->response()->GetStatusReason());
}

TEST_F(IisWriterTest, Status204NoContent) {
  response_headers_.set_status_code(204);
  OutputHeaders();
  // 204 should not have a body

  EXPECT_EQ(204, context_->response()->GetStatus());
  EXPECT_EQ("No Content", context_->response()->GetStatusReason());
  EXPECT_TRUE(context_->response()->body().empty());
}

// =============================================================================
// Content-Type Tests
// =============================================================================

TEST_F(IisWriterTest, ContentTypeTextHtml) {
  response_headers_.Replace("Content-Type", "text/html; charset=utf-8");
  OutputHeaders();

  EXPECT_EQ("text/html; charset=utf-8",
            context_->response()->GetHeader("Content-Type"));
}

TEST_F(IisWriterTest, ContentTypeTextPlain) {
  response_headers_.Replace("Content-Type", "text/plain");
  OutputHeaders();

  EXPECT_EQ("text/plain", context_->response()->GetHeader("Content-Type"));
}

TEST_F(IisWriterTest, ContentTypeApplicationJson) {
  response_headers_.Replace("Content-Type", "application/json");
  OutputHeaders();
  Write("{\"key\": \"value\"}");

  EXPECT_EQ("application/json",
            context_->response()->GetHeader("Content-Type"));
  EXPECT_EQ("{\"key\": \"value\"}", context_->response()->body());
}

TEST_F(IisWriterTest, ContentTypeTextCss) {
  response_headers_.Replace("Content-Type", "text/css");
  OutputHeaders();
  Write("body { color: red; }");

  EXPECT_EQ("text/css", context_->response()->GetHeader("Content-Type"));
}

TEST_F(IisWriterTest, ContentTypeApplicationJavascript) {
  response_headers_.Replace("Content-Type", "application/javascript");
  OutputHeaders();
  Write("console.log('hello');");

  EXPECT_EQ("application/javascript",
            context_->response()->GetHeader("Content-Type"));
}

TEST_F(IisWriterTest, ContentTypeImagePng) {
  response_headers_.Replace("Content-Type", "image/png");
  OutputHeaders();
  // Binary data would go here
  Write("\x89PNG\r\n\x1a\n");

  EXPECT_EQ("image/png", context_->response()->GetHeader("Content-Type"));
}

// =============================================================================
// Header Filtering Tests
// =============================================================================

TEST_F(IisWriterTest, TransferEncodingNotSent) {
  // Transfer-Encoding should be stripped (IIS handles chunking)
  response_headers_.Add("Transfer-Encoding", "chunked");
  OutputHeaders();

  // Transfer-Encoding should not be in the response
  EXPECT_TRUE(context_->response()->GetHeader("Transfer-Encoding").empty());
}

TEST_F(IisWriterTest, ContentLengthNotSent) {
  // Content-Length should be stripped (IIS calculates it)
  response_headers_.Add("Content-Length", "42");
  OutputHeaders();

  // Content-Length should not be in the response
  // (IIS will add it based on actual body size)
  EXPECT_TRUE(context_->response()->GetHeader("Content-Length").empty());
}

TEST_F(IisWriterTest, CookieHeaderPreserved) {
  response_headers_.Add("Set-Cookie", "session=abc123; Path=/");
  OutputHeaders();

  EXPECT_EQ("session=abc123; Path=/",
            context_->response()->GetHeader("Set-Cookie"));
}

TEST_F(IisWriterTest, CacheControlHeaderPreserved) {
  response_headers_.Add("Cache-Control", "max-age=3600, public");
  OutputHeaders();

  EXPECT_EQ("max-age=3600, public",
            context_->response()->GetHeader("Cache-Control"));
}

TEST_F(IisWriterTest, CustomHeadersPreserved) {
  response_headers_.Add("X-Custom-Header", "custom-value");
  response_headers_.Add("X-Page-Speed", "1.0.0");
  OutputHeaders();

  EXPECT_EQ("custom-value",
            context_->response()->GetHeader("X-Custom-Header"));
  EXPECT_EQ("1.0.0", context_->response()->GetHeader("X-Page-Speed"));
}

// =============================================================================
// Action Recording Verification Tests
// =============================================================================

TEST_F(IisWriterTest, ActionRecordingSequence) {
  context_->response()->ClearRecordedActions();

  // Perform a series of operations
  response_headers_.set_status_code(200);
  OutputHeaders();
  Write("part1");
  Flush(false);
  Write("part2");
  Flush(true);

  GoogleString actions = context_->response()->ActionsSinceLastCall();

  // Verify the sequence of operations
  size_t pos_status = actions.find("SetStatus");
  size_t pos_header = actions.find("SetHeader");
  size_t pos_write1 = actions.find("WriteEntityChunks(5 bytes)");
  size_t pos_flush1 = actions.find("Flush(partial)");
  size_t pos_write2 = actions.find("WriteEntityChunks(5 bytes)", pos_write1 + 1);
  size_t pos_flush2 = actions.find("Flush(final)");

  // All operations should be present
  EXPECT_NE(GoogleString::npos, pos_status);
  EXPECT_NE(GoogleString::npos, pos_header);
  EXPECT_NE(GoogleString::npos, pos_write1);
  EXPECT_NE(GoogleString::npos, pos_flush1);
  EXPECT_NE(GoogleString::npos, pos_write2);
  EXPECT_NE(GoogleString::npos, pos_flush2);

  // Operations should be in the correct order
  EXPECT_LT(pos_status, pos_header);
  EXPECT_LT(pos_header, pos_write1);
  EXPECT_LT(pos_write1, pos_flush1);
  EXPECT_LT(pos_flush1, pos_write2);
  EXPECT_LT(pos_write2, pos_flush2);
}

TEST_F(IisWriterTest, ActionsSinceLastCallClears) {
  context_->response()->ClearRecordedActions();

  Write("first");
  GoogleString actions1 = context_->response()->ActionsSinceLastCall();
  EXPECT_FALSE(actions1.empty());

  // Second call should return empty (actions were cleared)
  GoogleString actions2 = context_->response()->ActionsSinceLastCall();
  EXPECT_TRUE(actions2.empty());

  // New action should be recorded
  Write("second");
  GoogleString actions3 = context_->response()->ActionsSinceLastCall();
  EXPECT_FALSE(actions3.empty());
  EXPECT_TRUE(actions3.find("WriteEntityChunks") != GoogleString::npos);
}

// =============================================================================
// Large Content Tests
// =============================================================================

TEST_F(IisWriterTest, LargeBodyWrite) {
  OutputHeaders();

  // Write a large body (100KB)
  GoogleString large_body(100 * 1024, 'x');
  Write(large_body);

  EXPECT_EQ(large_body, context_->response()->body());
  EXPECT_EQ(100 * 1024, context_->response()->body().size());
}

TEST_F(IisWriterTest, ChunkedLargeBody) {
  OutputHeaders();

  // Write in chunks
  GoogleString chunk(1024, 'y');
  for (int i = 0; i < 100; ++i) {
    Write(chunk);
  }

  EXPECT_EQ(100 * 1024, context_->response()->body().size());
}

// =============================================================================
// Binary Content Tests
// =============================================================================

TEST_F(IisWriterTest, BinaryContent) {
  response_headers_.Replace("Content-Type", "application/octet-stream");
  OutputHeaders();

  // Write binary data with null bytes
  GoogleString binary_data;
  binary_data.push_back('\x00');
  binary_data.push_back('\x01');
  binary_data.push_back('\x02');
  binary_data.push_back('\xff');
  binary_data.push_back('\xfe');

  Write(binary_data);

  EXPECT_EQ(5, context_->response()->body().size());
  EXPECT_EQ(binary_data, context_->response()->body());
}

// =============================================================================
// Response Completion Tests
// =============================================================================

TEST_F(IisWriterTest, ResponseCompleted) {
  EXPECT_FALSE(context_->response()->completed());

  OutputHeaders();
  Write("content");
  context_->response()->set_completed(true);

  EXPECT_TRUE(context_->response()->completed());
}

TEST_F(IisWriterTest, ClearBodyResetsContent) {
  OutputHeaders();
  Write("original content");
  EXPECT_EQ("original content", context_->response()->body());

  context_->response()->ClearBody();
  EXPECT_TRUE(context_->response()->body().empty());

  Write("new content");
  EXPECT_EQ("new content", context_->response()->body());
}

}  // namespace net_instaweb
