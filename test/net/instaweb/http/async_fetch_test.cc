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

#include "net/instaweb/http/public/async_fetch.h"

#include "net/instaweb/http/public/request_context.h"
#include "pagespeed/kernel/base/basictypes.h"
#include "pagespeed/kernel/base/null_mutex.h"
#include "pagespeed/kernel/base/shared_string.h"
#include "pagespeed/kernel/http/http_names.h"
#include "pagespeed/kernel/http/http_options.h"
#include "pagespeed/kernel/http/response_headers.h"
#include "test/pagespeed/kernel/base/gtest.h"
#include "test/pagespeed/kernel/base/mock_message_handler.h"

namespace net_instaweb {
namespace {

class TestSharedAsyncFetch : public SharedAsyncFetch {
 public:
  explicit TestSharedAsyncFetch(AsyncFetch* base_fetch)
      : SharedAsyncFetch(base_fetch) {}

  ~TestSharedAsyncFetch() override {}

 private:
  TestSharedAsyncFetch(const TestSharedAsyncFetch&) = delete;
  TestSharedAsyncFetch& operator=(const TestSharedAsyncFetch&) = delete;
};

// Records whether the body arrived over the shared-storage write path (and
// from where), in addition to buffering it like StringAsyncFetch.
class SharedCapturingAsyncFetch : public StringAsyncFetch {
 public:
  explicit SharedCapturingAsyncFetch(const RequestContextPtr& ctx)
      : StringAsyncFetch(ctx), shared_writes_(0), last_shared_data_(nullptr) {}

  int shared_writes() const { return shared_writes_; }
  const char* last_shared_data() const { return last_shared_data_; }

 protected:
  bool HandleWriteShared(const StringPiece& content,
                         const SharedString& storage,
                         MessageHandler* handler) override {
    ++shared_writes_;
    last_shared_data_ = content.data();
    return HandleWrite(content, handler);
  }

 private:
  int shared_writes_;
  const char* last_shared_data_;

  SharedCapturingAsyncFetch(const SharedCapturingAsyncFetch&) = delete;
  SharedCapturingAsyncFetch& operator=(const SharedCapturingAsyncFetch&) =
      delete;
};

// Tests the AsyncFetch class and some of its derivations.
class AsyncFetchTest : public testing::Test {
 protected:
  AsyncFetchTest()
      : request_context_(new RequestContext(kDefaultHttpOptionsForTests,
                                            new NullMutex, nullptr)),
        string_fetch_(request_context_),
        handler_(new NullMutex) {}

  bool CheckCacheControlPublicWithVia(const char* via) {
    StringAsyncFetch fetch(request_context_);
    fetch.response_headers()->Add(HttpAttributes::kCacheControl, "max-age:100");
    if (via != nullptr) {
      fetch.request_headers()->Add(HttpAttributes::kVia, via);
    }
    fetch.FixCacheControlForGoogleCache();
    return fetch.response_headers()->HasValue(HttpAttributes::kCacheControl,
                                              "public");
  }

 protected:
  RequestContextPtr request_context_;
  StringAsyncFetch string_fetch_;
  HTTPValue fallback_value_;
  MockMessageHandler handler_;
};

TEST_F(AsyncFetchTest, LackOfContentLengthPropagatesToShared) {
  TestSharedAsyncFetch fetch(&string_fetch_);
  fetch.response_headers()->set_status_code(HttpStatus::kOK);
  EXPECT_FALSE(fetch.content_length_known());
  fetch.HeadersComplete();
  EXPECT_FALSE(string_fetch_.content_length_known());
}

TEST_F(AsyncFetchTest, ContentLengthPropagatesToShared) {
  TestSharedAsyncFetch fetch(&string_fetch_);
  fetch.response_headers()->set_status_code(HttpStatus::kOK);
  fetch.set_content_length(42);
  EXPECT_TRUE(fetch.content_length_known());
  fetch.HeadersComplete();
  EXPECT_TRUE(string_fetch_.content_length_known());
  EXPECT_EQ(42, string_fetch_.content_length());
}

TEST_F(AsyncFetchTest, LackOfContentLengthPropagatesToFallback) {
  FallbackSharedAsyncFetch fetch(&string_fetch_, &fallback_value_, &handler_);
  fetch.response_headers()->set_status_code(HttpStatus::kOK);
  EXPECT_FALSE(fetch.content_length_known());
  fetch.HeadersComplete();
  EXPECT_FALSE(string_fetch_.content_length_known());
}

TEST_F(AsyncFetchTest, ContentLengthPropagatesToFallback) {
  FallbackSharedAsyncFetch fetch(&string_fetch_, &fallback_value_, &handler_);
  fetch.response_headers()->set_status_code(HttpStatus::kOK);
  fetch.set_content_length(42);
  EXPECT_TRUE(fetch.content_length_known());
  fetch.HeadersComplete();
  EXPECT_TRUE(string_fetch_.content_length_known());
  EXPECT_EQ(42, string_fetch_.content_length());
}

TEST_F(AsyncFetchTest, LackOfContentLengthPropagatesToConditional) {
  ConditionalSharedAsyncFetch fetch(&string_fetch_, &fallback_value_,
                                    &handler_);
  fetch.response_headers()->set_status_code(HttpStatus::kOK);
  EXPECT_FALSE(fetch.content_length_known());
  fetch.HeadersComplete();
  EXPECT_FALSE(string_fetch_.content_length_known());
}

TEST_F(AsyncFetchTest, WriteSharedDefaultCopies) {
  string_fetch_.response_headers()->set_status_code(HttpStatus::kOK);
  SharedString storage("shared body");
  EXPECT_TRUE(string_fetch_.WriteShared(storage.Value(), storage, &handler_));
  EXPECT_EQ("shared body", string_fetch_.buffer());
  EXPECT_TRUE(string_fetch_.headers_complete());
}

TEST_F(AsyncFetchTest, WriteSharedSkipsBodyForHead) {
  string_fetch_.request_headers()->set_method(RequestHeaders::kHead);
  string_fetch_.response_headers()->set_status_code(HttpStatus::kOK);
  SharedString storage("shared body");
  EXPECT_TRUE(string_fetch_.WriteShared(storage.Value(), storage, &handler_));
  EXPECT_TRUE(string_fetch_.buffer().empty());
}

TEST_F(AsyncFetchTest, WriteSharedForwardsReferenceThroughShared) {
  SharedCapturingAsyncFetch capturing_fetch(request_context_);
  TestSharedAsyncFetch fetch(&capturing_fetch);
  fetch.response_headers()->set_status_code(HttpStatus::kOK);
  SharedString storage("shared body");
  EXPECT_TRUE(fetch.WriteShared(storage.Value(), storage, &handler_));
  EXPECT_EQ("shared body", capturing_fetch.buffer());
  EXPECT_EQ(1, capturing_fetch.shared_writes());
  // The pass-through wrapper must forward the reference, not a copy.
  EXPECT_EQ(storage.Value().data(), capturing_fetch.last_shared_data());
}

TEST_F(AsyncFetchTest, WriteSharedDroppedWhileServingFallback) {
  ResponseHeaders fallback_headers;
  fallback_headers.set_major_version(1);
  fallback_headers.set_minor_version(1);
  fallback_headers.SetStatusAndReason(HttpStatus::kOK);
  fallback_value_.SetHeaders(&fallback_headers);
  fallback_value_.Write("stale body", &handler_);
  // Heap-allocated: FallbackSharedAsyncFetch deletes itself in HandleDone.
  FallbackSharedAsyncFetch* fetch =
      new FallbackSharedAsyncFetch(&string_fetch_, &fallback_value_, &handler_);
  fetch->response_headers()->set_status_code(HttpStatus::kInternalServerError);
  fetch->HeadersComplete();
  ASSERT_TRUE(fetch->serving_fallback());
  SharedString storage("origin body");
  EXPECT_TRUE(fetch->WriteShared(storage.Value(), storage, &handler_));
  // The origin's bytes must be dropped, leaving only the fallback body.
  EXPECT_EQ("stale body", string_fetch_.buffer());
  fetch->Done(false);
  EXPECT_TRUE(string_fetch_.success());
}

TEST_F(AsyncFetchTest, ContentLengthPropagatesToConditional) {
  ConditionalSharedAsyncFetch fetch(&string_fetch_, &fallback_value_,
                                    &handler_);
  fetch.response_headers()->set_status_code(HttpStatus::kOK);
  fetch.set_content_length(42);
  EXPECT_TRUE(fetch.content_length_known());
  fetch.HeadersComplete();
  EXPECT_TRUE(string_fetch_.content_length_known());
  EXPECT_EQ(42, string_fetch_.content_length());
}

TEST_F(AsyncFetchTest, ViaHandling) {
  EXPECT_FALSE(CheckCacheControlPublicWithVia(nullptr));
  EXPECT_TRUE(CheckCacheControlPublicWithVia("1.1 google"));
  EXPECT_TRUE(CheckCacheControlPublicWithVia("2.0 google"));
  EXPECT_TRUE(CheckCacheControlPublicWithVia("2 google"));
  EXPECT_TRUE(CheckCacheControlPublicWithVia("1.0 GOOGLE"));
  EXPECT_FALSE(CheckCacheControlPublicWithVia("varnish"));
  EXPECT_FALSE(CheckCacheControlPublicWithVia("NotReallyGoogle"));
}

}  // namespace

}  // namespace net_instaweb
