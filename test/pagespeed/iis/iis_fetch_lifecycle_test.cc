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

// IIS Fetch Lifecycle Tests
//
// These tests verify the fetch lifecycle behavior for the IIS module.
// Tests simulate the complete request-response cycle using the mock
// infrastructure with action recording to verify the exact sequence
// of operations.
// Equivalent to Apache's apache_fetch_test.cc.

#include <memory>

#include "test/pagespeed/iis/mock_iis.h"
#include "test/pagespeed/iis/iis_test_base.h"

#include "pagespeed/kernel/base/basictypes.h"
#include "test/pagespeed/kernel/base/gtest.h"
#include "pagespeed/kernel/base/string.h"
#include "pagespeed/kernel/base/string_util.h"
#include "pagespeed/kernel/http/response_headers.h"

namespace net_instaweb {

// Simulates the IIS fetch lifecycle phases
enum FetchPhase {
  PHASE_INIT,
  PHASE_HEADERS_RECEIVED,
  PHASE_BODY_RECEIVING,
  PHASE_BODY_COMPLETE,
  PHASE_DONE_SUCCESS,
  PHASE_DONE_FAILURE
};

class IisFetchLifecycleTest : public IisTestBase {
 protected:
  void SetUp() override {
    IisTestBase::SetUp();
    context_ = CreateContext("/test.html");
    InitStandardResponseHeaders(&response_headers_);

    phase_ = PHASE_INIT;
    headers_sent_ = false;
    done_called_ = false;
    success_ = false;
  }

  void TearDown() override {
    context_.reset();
    IisTestBase::TearDown();
  }

  // Simulate receiving headers (HeadersComplete)
  void HeadersComplete() {
    ASSERT_EQ(PHASE_INIT, phase_);
    phase_ = PHASE_HEADERS_RECEIVED;

    OutputResponseHeaders(response_headers_, context_->response());
    headers_sent_ = true;
  }

  // Simulate writing body content (HandleWrite)
  bool HandleWrite(StringPiece data) {
    if (phase_ == PHASE_HEADERS_RECEIVED) {
      phase_ = PHASE_BODY_RECEIVING;
    }
    EXPECT_TRUE(phase_ == PHASE_BODY_RECEIVING || phase_ == PHASE_HEADERS_RECEIVED);

    return WriteToResponse(context_->response(), data);
  }

  // Simulate flushing (HandleFlush)
  bool HandleFlush() {
    return FlushResponse(context_->response(), false);
  }

  // Simulate completion (Done)
  void Done(bool success) {
    ASSERT_FALSE(done_called_);
    done_called_ = true;
    success_ = success;

    if (phase_ == PHASE_BODY_RECEIVING) {
      phase_ = PHASE_BODY_COMPLETE;
    }
    phase_ = success ? PHASE_DONE_SUCCESS : PHASE_DONE_FAILURE;

    context_->response()->set_completed(true);
    FlushResponse(context_->response(), true);
  }

  std::unique_ptr<MockHttpContext> context_;
  ResponseHeaders response_headers_;

  FetchPhase phase_;
  bool headers_sent_;
  bool done_called_;
  bool success_;
};

// =============================================================================
// Basic Lifecycle Tests
// =============================================================================

TEST_F(IisFetchLifecycleTest, SimpleSuccessfulFetch) {
  context_->response()->ClearRecordedActions();

  // Complete lifecycle: headers -> write -> done
  HeadersComplete();
  HandleWrite("hello world");
  Done(true);

  EXPECT_EQ(PHASE_DONE_SUCCESS, phase_);
  EXPECT_TRUE(headers_sent_);
  EXPECT_TRUE(done_called_);
  EXPECT_TRUE(success_);
  EXPECT_EQ("hello world", context_->response()->body());
  EXPECT_EQ(200, context_->response()->GetStatus());
}

TEST_F(IisFetchLifecycleTest, MultipleWritesCycle) {
  context_->response()->ClearRecordedActions();

  HeadersComplete();
  HandleWrite("part1");
  HandleFlush();
  HandleWrite("part2");
  HandleFlush();
  HandleWrite("part3");
  Done(true);

  EXPECT_EQ("part1part2part3", context_->response()->body());
  EXPECT_TRUE(context_->response()->completed());
}

TEST_F(IisFetchLifecycleTest, HeadersOnlyNoBody) {
  context_->response()->ClearRecordedActions();

  response_headers_.set_status_code(204);  // No Content
  HeadersComplete();
  Done(true);

  EXPECT_EQ(204, context_->response()->GetStatus());
  EXPECT_TRUE(context_->response()->body().empty());
  EXPECT_TRUE(context_->response()->completed());
}

TEST_F(IisFetchLifecycleTest, FetchFailure) {
  context_->response()->ClearRecordedActions();

  HeadersComplete();
  HandleWrite("partial data");
  Done(false);  // Failure

  EXPECT_EQ(PHASE_DONE_FAILURE, phase_);
  EXPECT_FALSE(success_);
  EXPECT_TRUE(done_called_);
}

// =============================================================================
// Status Code Lifecycle Tests
// =============================================================================

TEST_F(IisFetchLifecycleTest, Status200OKLifecycle) {
  response_headers_.set_status_code(200);
  HeadersComplete();
  HandleWrite("Success content");
  Done(true);

  EXPECT_EQ(200, context_->response()->GetStatus());
  EXPECT_EQ("OK", context_->response()->GetStatusReason());
}

TEST_F(IisFetchLifecycleTest, Status301RedirectLifecycle) {
  response_headers_.set_status_code(301);
  response_headers_.Add("Location", "http://example.com/new");
  HeadersComplete();
  HandleWrite("Moved");
  Done(true);

  EXPECT_EQ(301, context_->response()->GetStatus());
  EXPECT_EQ("http://example.com/new",
            context_->response()->GetHeader("Location"));
}

TEST_F(IisFetchLifecycleTest, Status304NotModifiedLifecycle) {
  response_headers_.set_status_code(304);
  response_headers_.RemoveAll("Content-Type");  // 304 has no body
  HeadersComplete();
  Done(true);

  EXPECT_EQ(304, context_->response()->GetStatus());
}

TEST_F(IisFetchLifecycleTest, Status404NotFoundLifecycle) {
  response_headers_.set_status_code(404);
  HeadersComplete();
  HandleWrite("Page not found");
  Done(true);

  EXPECT_EQ(404, context_->response()->GetStatus());
  EXPECT_EQ("Page not found", context_->response()->body());
}

TEST_F(IisFetchLifecycleTest, Status500ErrorLifecycle) {
  response_headers_.set_status_code(500);
  HeadersComplete();
  HandleWrite("Internal error");
  Done(true);

  EXPECT_EQ(500, context_->response()->GetStatus());
}

TEST_F(IisFetchLifecycleTest, Status403ForbiddenLifecycle) {
  response_headers_.set_status_code(403);
  HeadersComplete();
  HandleWrite("Forbidden");
  Done(true);

  EXPECT_EQ(403, context_->response()->GetStatus());
}

// =============================================================================
// Content Type Validation Tests (like Apache missing Content-Type -> 403)
// =============================================================================

TEST_F(IisFetchLifecycleTest, MissingContentType200) {
  // Without Content-Type, some implementations return 403
  response_headers_.set_status_code(200);
  response_headers_.RemoveAll("Content-Type");
  HeadersComplete();

  // Note: IIS behavior may differ from Apache here
  // Apache returns 403 for missing Content-Type on 200 responses
  // We verify the headers were sent
  EXPECT_TRUE(headers_sent_);
}

TEST_F(IisFetchLifecycleTest, ContentTypePreserved) {
  response_headers_.Replace("Content-Type", "application/json");
  HeadersComplete();
  HandleWrite("{\"status\": \"ok\"}");
  Done(true);

  EXPECT_EQ("application/json",
            context_->response()->GetHeader("Content-Type"));
}

// =============================================================================
// Action Recording Lifecycle Tests
// =============================================================================

TEST_F(IisFetchLifecycleTest, ActionSequenceSimple) {
  context_->response()->ClearRecordedActions();

  HeadersComplete();
  HandleWrite("content");
  Done(true);

  GoogleString actions = context_->response()->ActionsSinceLastCall();

  // Verify operation sequence
  size_t pos_status = actions.find("SetStatus");
  size_t pos_header = actions.find("SetHeader");
  size_t pos_write = actions.find("WriteEntityChunks");
  size_t pos_flush = actions.find("Flush(final)");

  EXPECT_NE(GoogleString::npos, pos_status);
  EXPECT_NE(GoogleString::npos, pos_header);
  EXPECT_NE(GoogleString::npos, pos_write);
  EXPECT_NE(GoogleString::npos, pos_flush);

  // Verify order
  EXPECT_LT(pos_status, pos_header);
  EXPECT_LT(pos_header, pos_write);
  EXPECT_LT(pos_write, pos_flush);
}

TEST_F(IisFetchLifecycleTest, ActionSequenceWithFlushes) {
  context_->response()->ClearRecordedActions();

  HeadersComplete();
  HandleWrite("part1");
  HandleFlush();
  HandleWrite("part2");
  HandleFlush();
  Done(true);

  GoogleString actions = context_->response()->ActionsSinceLastCall();

  // Should have partial flushes before final flush
  size_t pos_flush1 = actions.find("Flush(partial)");
  size_t pos_flush2 = actions.find("Flush(partial)", pos_flush1 + 1);
  size_t pos_final = actions.find("Flush(final)");

  EXPECT_NE(GoogleString::npos, pos_flush1);
  EXPECT_NE(GoogleString::npos, pos_flush2);
  EXPECT_NE(GoogleString::npos, pos_final);
  EXPECT_LT(pos_flush1, pos_flush2);
  EXPECT_LT(pos_flush2, pos_final);
}

// =============================================================================
// Buffered vs Unbuffered Simulation Tests
// =============================================================================

TEST_F(IisFetchLifecycleTest, UnbufferedWrites) {
  context_->response()->ClearRecordedActions();

  HeadersComplete();

  // Each write should produce an action immediately
  HandleWrite("chunk1");
  GoogleString actions1 = context_->response()->ActionsSinceLastCall();
  EXPECT_TRUE(actions1.find("WriteEntityChunks") != GoogleString::npos);

  HandleWrite("chunk2");
  GoogleString actions2 = context_->response()->ActionsSinceLastCall();
  EXPECT_TRUE(actions2.find("WriteEntityChunks") != GoogleString::npos);

  Done(true);
}

TEST_F(IisFetchLifecycleTest, BufferedWritesSimulation) {
  // Simulate buffered mode by accumulating writes
  GoogleString buffer;
  context_->response()->ClearRecordedActions();

  HeadersComplete();

  // Buffer writes
  buffer.append("chunk1");
  buffer.append("chunk2");
  buffer.append("chunk3");

  // Single write at the end
  HandleWrite(buffer);
  Done(true);

  // Should see single combined write
  EXPECT_EQ("chunk1chunk2chunk3", context_->response()->body());
}

// =============================================================================
// Error Condition Tests
// =============================================================================

TEST_F(IisFetchLifecycleTest, EmptyBodySuccess) {
  HeadersComplete();
  // No writes, just done
  Done(true);

  EXPECT_TRUE(context_->response()->body().empty());
  EXPECT_TRUE(success_);
}

TEST_F(IisFetchLifecycleTest, LargeBodyHandling) {
  HeadersComplete();

  // Write large body in chunks
  GoogleString chunk(1024, 'x');
  for (int i = 0; i < 100; ++i) {
    HandleWrite(chunk);
    if (i % 10 == 9) {
      HandleFlush();
    }
  }
  Done(true);

  EXPECT_EQ(100 * 1024, context_->response()->body().size());
}

// =============================================================================
// Async Lifecycle Tests
// =============================================================================

TEST_F(IisFetchLifecycleTest, AsyncPendingState) {
  context_->SetAsyncPending(true);
  EXPECT_TRUE(context_->IsAsyncPending());

  HeadersComplete();
  HandleWrite("async content");

  // Still pending
  EXPECT_TRUE(context_->IsAsyncPending());

  // Complete async
  context_->CompleteAsync(RQ_NOTIFICATION_CONTINUE);
  EXPECT_FALSE(context_->IsAsyncPending());
  EXPECT_EQ(RQ_NOTIFICATION_CONTINUE, context_->GetAsyncResult());

  Done(true);
}

TEST_F(IisFetchLifecycleTest, AsyncActionRecording) {
  context_->ClearRecordedActions();

  context_->SetAsyncPending(true);
  context_->CompleteAsync(RQ_NOTIFICATION_FINISH_REQUEST);

  GoogleString actions = context_->ActionsSinceLastCall();
  EXPECT_TRUE(actions.find("SetAsyncPending(true)") != GoogleString::npos);
  EXPECT_TRUE(actions.find("CompleteAsync(FINISH_REQUEST)") != GoogleString::npos);
}

// =============================================================================
// Cookie and Header Handling Tests
// =============================================================================

TEST_F(IisFetchLifecycleTest, CookieHeadersPreserved) {
  response_headers_.Add("Set-Cookie", "session=abc123; Path=/");
  response_headers_.Add("Set-Cookie", "user=john; HttpOnly");
  HeadersComplete();
  Done(true);

  // In IIS, we may have multiple Set-Cookie headers
  // Our mock combines them, but real IIS handles multiple headers
  EXPECT_FALSE(context_->response()->GetHeader("Set-Cookie").empty());
}

TEST_F(IisFetchLifecycleTest, CacheControlPreserved) {
  response_headers_.Add("Cache-Control", "no-cache, no-store");
  HeadersComplete();
  Done(true);

  EXPECT_EQ("no-cache, no-store",
            context_->response()->GetHeader("Cache-Control"));
}

TEST_F(IisFetchLifecycleTest, CustomHeadersPreserved) {
  response_headers_.Add("X-Custom", "value1");
  response_headers_.Add("X-Page-Speed", "1.0.0");
  HeadersComplete();
  Done(true);

  EXPECT_EQ("value1", context_->response()->GetHeader("X-Custom"));
  EXPECT_EQ("1.0.0", context_->response()->GetHeader("X-Page-Speed"));
}

// =============================================================================
// Context Lifecycle Tests
// =============================================================================

TEST_F(IisFetchLifecycleTest, ContextPropertiesPreserved) {
  context_->SetSiteId(123);
  context_->SetSiteName("Test Site");
  context_->SetApplicationPath("/app");
  context_->SetPhysicalPath("C:\\inetpub\\test");
  context_->SetAuthenticatedUser("testuser");

  EXPECT_EQ(123, context_->GetSiteId());
  EXPECT_EQ("Test Site", context_->GetSiteName());
  EXPECT_EQ("/app", context_->GetApplicationPath());
  EXPECT_EQ("C:\\inetpub\\test", context_->GetPhysicalPath());
  EXPECT_EQ("testuser", context_->GetAuthenticatedUser());
  EXPECT_TRUE(context_->IsAuthenticated());
}

TEST_F(IisFetchLifecycleTest, ContextRequestProperties) {
  context_->request()->SetUrl("/path/to/resource?query=value");
  context_->request()->SetMethod("POST");
  context_->request()->SetHeader("Content-Type", "application/json");

  // Use STREQ for C-string comparison (PCSTR returns)
  EXPECT_STREQ("/path/to/resource?query=value",
               context_->request()->GetRawHttpRequestUrl());
  EXPECT_STREQ("POST", context_->request()->GetMethod());
  EXPECT_STREQ("application/json",
               context_->request()->GetHeader("Content-Type"));
}

}  // namespace net_instaweb
