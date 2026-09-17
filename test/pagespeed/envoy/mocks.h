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

// Mock implementations for Envoy PageSpeed filter unit testing.
//
// This file provides mock implementations of both Envoy interfaces
// (StreamDecoderFilterCallbacks, StreamEncoderFilterCallbacks, Dispatcher)
// and PageSpeed interfaces (UrlAsyncFetcher, EnvoyServerContext) needed
// to unit test http_filter.cc and envoy_html_rewriter.cc without requiring
// a full Envoy or PageSpeed runtime.
//
// Usage:
//   #include "test/pagespeed/envoy/mocks.h"
//   using ::testing::NiceMock;
//   NiceMock<MockStreamDecoderFilterCallbacks> decoder_callbacks;
//   NiceMock<MockStreamEncoderFilterCallbacks> encoder_callbacks;

#ifndef TEST_PAGESPEED_ENVOY_MOCKS_H_
#define TEST_PAGESPEED_ENVOY_MOCKS_H_

#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "envoy/event/dispatcher.h"
#include "envoy/event/timer.h"
#include "envoy/http/filter.h"
#include "gmock/gmock.h"
#include "net/instaweb/http/public/async_fetch.h"
#include "net/instaweb/http/public/url_async_fetcher.h"
#include "pagespeed/kernel/base/basictypes.h"
#include "pagespeed/kernel/base/message_handler.h"
#include "pagespeed/kernel/base/string.h"
#include "pagespeed/kernel/base/string_util.h"
#include "source/common/buffer/buffer_impl.h"
#include "source/common/http/header_map_impl.h"
#include "test/mocks/event/mocks.h"
#include "test/mocks/http/mocks.h"
#include "test/mocks/stream_info/mocks.h"

namespace net_instaweb {
namespace testing {

// ============================================================================
// Envoy Mock Components
// ============================================================================

// Extended mock for Envoy's StreamDecoderFilterCallbacks with commonly used
// methods for PageSpeed filter testing.
class MockStreamDecoderFilterCallbacks
    : public Envoy::Http::MockStreamDecoderFilterCallbacks {
 public:
  MockStreamDecoderFilterCallbacks() {
    // Set up default behavior for commonly called methods.
    ON_CALL(*this, dispatcher())
        .WillByDefault(::testing::ReturnRef(dispatcher_));
    ON_CALL(*this, streamInfo())
        .WillByDefault(::testing::ReturnRef(stream_info_));
  }

  ~MockStreamDecoderFilterCallbacks() override = default;

  // Additional mock methods commonly used in PageSpeed filter tests.
  MOCK_METHOD(void, sendLocalReply,
              (Envoy::Http::Code code, absl::string_view body,
               std::function<void(Envoy::Http::ResponseHeaderMap& headers)>
                   modify_headers,
               const absl::optional<Grpc::Status::GrpcStatus> grpc_status,
               absl::string_view details));

  // Returns the captured response from sendLocalReply.
  Envoy::Http::Code last_reply_code() const { return last_reply_code_; }
  const std::string& last_reply_body() const { return last_reply_body_; }

  void CaptureLocalReply(
      Envoy::Http::Code code, absl::string_view body,
      std::function<void(Envoy::Http::ResponseHeaderMap&)> modify_headers,
      const absl::optional<Grpc::Status::GrpcStatus> /* grpc_status */,
      absl::string_view /* details */) {
    last_reply_code_ = code;
    last_reply_body_ = std::string(body);
    if (modify_headers && captured_response_headers_) {
      modify_headers(*captured_response_headers_);
    }
  }

  // Set up response header capture.
  void SetupResponseHeaderCapture(
      Envoy::Http::ResponseHeaderMapPtr response_headers) {
    captured_response_headers_ = std::move(response_headers);
  }

  Envoy::Http::ResponseHeaderMap* captured_response_headers() {
    return captured_response_headers_.get();
  }

 private:
  ::testing::NiceMock<Envoy::Event::MockDispatcher> dispatcher_;
  ::testing::NiceMock<Envoy::StreamInfo::MockStreamInfo> stream_info_;
  Envoy::Http::Code last_reply_code_{Envoy::Http::Code::OK};
  std::string last_reply_body_;
  Envoy::Http::ResponseHeaderMapPtr captured_response_headers_;
};

// Extended mock for Envoy's StreamEncoderFilterCallbacks with commonly used
// methods for PageSpeed filter testing.
class MockStreamEncoderFilterCallbacks
    : public Envoy::Http::MockStreamEncoderFilterCallbacks {
 public:
  MockStreamEncoderFilterCallbacks() {
    ON_CALL(*this, dispatcher())
        .WillByDefault(::testing::ReturnRef(dispatcher_));
    ON_CALL(*this, streamInfo())
        .WillByDefault(::testing::ReturnRef(stream_info_));
  }

  ~MockStreamEncoderFilterCallbacks() override = default;

 private:
  ::testing::NiceMock<Envoy::Event::MockDispatcher> dispatcher_;
  ::testing::NiceMock<Envoy::StreamInfo::MockStreamInfo> stream_info_;
};

// A fake timer implementation for testing async operations.
// Allows tests to manually trigger timer callbacks without waiting.
class FakeTimer : public Envoy::Event::Timer {
 public:
  FakeTimer() : enabled_(false), callback_(nullptr) {}
  ~FakeTimer() override = default;

  void disableTimer() override {
    enabled_ = false;
  }

  void enableTimer(std::chrono::milliseconds timeout,
                   const Envoy::ScopeTrackedObject* /* scope */) override {
    enabled_ = true;
    timeout_ms_ = timeout;
  }

  void enableHRTimer(std::chrono::microseconds timeout,
                     const Envoy::ScopeTrackedObject* /* scope */) override {
    enabled_ = true;
    timeout_ms_ = std::chrono::duration_cast<std::chrono::milliseconds>(timeout);
  }

  bool enabled() override { return enabled_; }

  // Test helper: Invoke the callback as if the timer fired.
  void invokeCallback() {
    if (enabled_ && callback_) {
      enabled_ = false;
      callback_();
    }
  }

  void setCallback(Envoy::Event::TimerCb callback) {
    callback_ = std::move(callback);
  }

  std::chrono::milliseconds timeout() const { return timeout_ms_; }

 private:
  bool enabled_;
  std::chrono::milliseconds timeout_ms_{0};
  Envoy::Event::TimerCb callback_;
};

// A fake dispatcher for testing async operations.
// Collects posted callbacks for manual execution by tests.
class FakeDispatcher : public Envoy::Event::MockDispatcher {
 public:
  FakeDispatcher() {
    // Override post() to collect callbacks.
    ON_CALL(*this, post(::testing::_))
        .WillByDefault([this](Envoy::Event::PostCb callback) {
          pending_callbacks_.push_back(std::move(callback));
        });

    // Override createTimer() to return FakeTimers.
    ON_CALL(*this, createTimer_(::testing::_))
        .WillByDefault([this](Envoy::Event::TimerCb cb) -> Envoy::Event::Timer* {
          auto timer = new FakeTimer();
          timer->setCallback(std::move(cb));
          created_timers_.push_back(timer);
          return timer;
        });
  }

  ~FakeDispatcher() override = default;

  // Run all pending callbacks (single iteration).
  void runPendingCallbacks() {
    std::vector<Envoy::Event::PostCb> callbacks;
    callbacks.swap(pending_callbacks_);
    for (auto& cb : callbacks) {
      cb();
    }
  }

  // Run all pending callbacks recursively until none remain.
  void runAllPendingCallbacks() {
    while (!pending_callbacks_.empty()) {
      runPendingCallbacks();
    }
  }

  // Get the number of pending callbacks.
  size_t pendingCallbackCount() const { return pending_callbacks_.size(); }

  // Get created timers for test inspection.
  const std::vector<FakeTimer*>& created_timers() const {
    return created_timers_;
  }

  // Fire all enabled timers.
  void fireAllTimers() {
    for (auto* timer : created_timers_) {
      if (timer->enabled()) {
        timer->invokeCallback();
      }
    }
  }

 private:
  std::vector<Envoy::Event::PostCb> pending_callbacks_;
  std::vector<FakeTimer*> created_timers_;
};

// ============================================================================
// PageSpeed Mock Components
// ============================================================================

// Mock URL fetcher for testing resource fetching in PageSpeed filters.
// Allows tests to set up expected responses for specific URLs.
class MockUrlAsyncFetcher : public UrlAsyncFetcher {
 public:
  MockUrlAsyncFetcher() = default;
  ~MockUrlAsyncFetcher() override = default;

  MOCK_METHOD(void, Fetch,
              (const GoogleString& url, MessageHandler* handler,
               AsyncFetch* fetch),
              (override));

  MOCK_METHOD(bool, SupportsHttps, (), (const, override));
  MOCK_METHOD(int64, timeout_ms, (), (override));
  MOCK_METHOD(void, ShutDown, (), (override));

  // Helper to set up a successful response for a URL.
  void SetResponse(const GoogleString& url, int status_code,
                   const GoogleString& content_type,
                   const GoogleString& body) {
    responses_[url] = {status_code, content_type, body, true};
  }

  // Helper to set up a failed response for a URL.
  void SetFailure(const GoogleString& url) {
    responses_[url] = {0, "", "", false};
  }

  // Default implementation that uses stored responses.
  void FetchWithStoredResponses(const GoogleString& url,
                                MessageHandler* /* handler */,
                                AsyncFetch* fetch) {
    auto it = responses_.find(url);
    if (it != responses_.end()) {
      const auto& resp = it->second;
      if (resp.success) {
        fetch->response_headers()->SetStatusAndReason(
            static_cast<HttpStatus::Code>(resp.status_code));
        fetch->response_headers()->Add("Content-Type", resp.content_type);
        fetch->response_headers()->ComputeCaching();
        fetch->HeadersComplete();
        fetch->Write(resp.body, nullptr);
        fetch->Done(true);
      } else {
        fetch->Done(false);
      }
    } else {
      // URL not found - simulate 404.
      fetch->response_headers()->SetStatusAndReason(HttpStatus::kNotFound);
      fetch->HeadersComplete();
      fetch->Done(false);
    }
  }

 private:
  struct Response {
    int status_code;
    GoogleString content_type;
    GoogleString body;
    bool success;
  };
  std::map<GoogleString, Response> responses_;
};

// Mock for capturing AsyncFetch callbacks.
// Useful for testing code that creates AsyncFetch objects.
class MockAsyncFetch : public AsyncFetch {
 public:
  explicit MockAsyncFetch(const RequestContextPtr& request_context)
      : AsyncFetch(request_context) {}
  ~MockAsyncFetch() override = default;

  MOCK_METHOD(bool, HandleWrite, (const StringPiece& sp, MessageHandler* handler),
              (override));
  MOCK_METHOD(bool, HandleFlush, (MessageHandler* handler), (override));
  MOCK_METHOD(void, HandleHeadersComplete, (), (override));
  MOCK_METHOD(void, HandleDone, (bool success), (override));

  // Accessors for captured data.
  const GoogleString& written_data() const { return written_data_; }
  bool headers_complete() const { return headers_complete_; }
  bool done_called() const { return done_called_; }
  bool done_success() const { return done_success_; }

  // Default implementations that capture data.
  bool WriteImpl(const StringPiece& sp, MessageHandler* /* handler */) {
    written_data_.append(sp.data(), sp.size());
    return true;
  }

  bool FlushImpl(MessageHandler* /* handler */) { return true; }

  void HeadersCompleteImpl() { headers_complete_ = true; }

  void DoneImpl(bool success) {
    done_called_ = true;
    done_success_ = success;
  }

 private:
  GoogleString written_data_;
  bool headers_complete_{false};
  bool done_called_{false};
  bool done_success_{false};
};

// ============================================================================
// Test Utilities
// ============================================================================

// Helper to create request headers for testing.
inline Envoy::Http::TestRequestHeaderMapImpl CreateTestRequestHeaders(
    const std::string& method, const std::string& host,
    const std::string& path) {
  return Envoy::Http::TestRequestHeaderMapImpl{
      {":method", method}, {":authority", host}, {":path", path}};
}

// Helper to create response headers for testing.
inline Envoy::Http::TestResponseHeaderMapImpl CreateTestResponseHeaders(
    int status_code, const std::string& content_type) {
  return Envoy::Http::TestResponseHeaderMapImpl{
      {":status", std::to_string(status_code)},
      {"content-type", content_type}};
}

// Helper to create a buffer with test data.
inline Envoy::Buffer::OwnedImpl CreateTestBuffer(const std::string& data) {
  Envoy::Buffer::OwnedImpl buffer;
  buffer.add(data);
  return buffer;
}

}  // namespace testing
}  // namespace net_instaweb

#endif  // TEST_PAGESPEED_ENVOY_MOCKS_H_
