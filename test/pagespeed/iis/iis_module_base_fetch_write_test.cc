// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

// Buffering of response bytes in request memory: when the request pool cannot
// satisfy an allocation, the write fails instead of storing through the
// missing block.  Resource fetches buffer at write time; rewritten HTML is
// buffered in module memory and reaches request memory at completion
// (iis_module_base_fetch_thread_test.cc covers that arm).

#include <cstdlib>
#include <memory>
#include <vector>

#include "pagespeed/iis/iis_module_base_fetch.h"
#include "pagespeed/kernel/base/null_message_handler.h"
#include "pagespeed/kernel/base/string.h"
#include "pagespeed/kernel/http/response_headers.h"
#include "pagespeed/kernel/thread/win_thread_system.h"
#include "pagespeed/opt/http/request_context.h"
#include "test/pagespeed/iis/stub_iis_http_context.h"
#include "test/pagespeed/kernel/base/gtest.h"

class ConfigFactory;

namespace net_instaweb {

// The module's DLL entry point defines this; a test binary has none.
ConfigFactory* cf = nullptr;

namespace {

class FakeRequest : public StubHttpRequest {
 public:
  FakeRequest() { memset(&raw_, 0, sizeof(raw_)); }
  HTTP_REQUEST* GetRawHttpRequest() override { return &raw_; }
  const HTTP_REQUEST* GetRawHttpRequest() const override { return &raw_; }
  VOID GetHttpVersion(USHORT* major, USHORT* minor) const override {
    *major = 1;
    *minor = 1;
  }

 private:
  HTTP_REQUEST raw_;
};

class FakeResponse : public StubHttpResponse {
 public:
  // Suppressed headers make the fetch skip copying response headers.
  BOOL GetHeadersSuppressed() const override { return TRUE; }
  VOID ResetConnection() override { ++resets_; }
  int resets() const { return resets_; }

 private:
  int resets_ = 0;
};

// Hands out request memory until |budget| allocations have been made; every
// allocation after that fails the way an exhausted request pool does.
class FakeContext : public StubHttpContext {
 public:
  explicit FakeContext(int budget) : budget_(budget) {}
  ~FakeContext() {
    for (void* p : blocks_) {
      free(p);
    }
  }
  IHttpRequest* GetRequest() override { return &request_; }
  IHttpResponse* GetResponse() override { return &response_; }
  IHttpContext* GetRootContext() const override {
    return const_cast<FakeContext*>(this);
  }
  VOID* AllocateRequestMemory(DWORD size) override {
    ++calls_;
    if (budget_ <= 0) {
      return nullptr;
    }
    --budget_;
    void* p = malloc(size);
    blocks_.push_back(p);
    return p;
  }
  int calls() const { return calls_; }
  void set_budget(int budget) { budget_ = budget; }
  const FakeResponse& response() const { return response_; }

 private:
  FakeRequest request_;
  FakeResponse response_;
  int budget_;
  int calls_ = 0;
  std::vector<void*> blocks_;
};

class IisModuleBaseFetchWriteTest : public testing::Test {
 protected:
  // The fetch is reference counted; release both references when done.
  struct Releaser {
    void operator()(IisModuleBaseFetch* fetch) const {
      fetch->ReleaseRef();
      fetch->ReleaseRef();
    }
  };
  using FetchPtr = std::unique_ptr<IisModuleBaseFetch, Releaser>;

  FetchPtr NewFetch(FakeContext* context) {
    FetchPtr fetch(new IisModuleBaseFetch(
        context, kResource, &handler_,
        RequestContext::NewTestRequestContext(&thread_system_), nullptr));
    fetch->response_headers()->set_status_code(200);
    return fetch;
  }

  WinThreadSystem thread_system_;
  NullMessageHandler handler_;
};

TEST_F(IisModuleBaseFetchWriteTest, SmallWritesBufferInRequestMemory) {
  FakeContext context(10);
  FetchPtr fetch = NewFetch(&context);
  EXPECT_TRUE(fetch->Write("hello", &handler_));
  EXPECT_TRUE(fetch->Write(" world", &handler_));
  EXPECT_EQ(1, context.calls());
  EXPECT_TRUE(fetch->Write(GoogleString(70000, 'z'), &handler_));
}

TEST_F(IisModuleBaseFetchWriteTest, FailedSmallAllocationFailsTheWrite) {
  FakeContext context(0);
  FetchPtr fetch = NewFetch(&context);
  EXPECT_FALSE(fetch->Write("hello", &handler_));
  EXPECT_EQ(1, context.calls());
}

TEST_F(IisModuleBaseFetchWriteTest, FailedLargeAllocationFailsTheWrite) {
  FakeContext context(0);
  FetchPtr fetch = NewFetch(&context);
  EXPECT_FALSE(fetch->Write(GoogleString(5000, 'z'), &handler_));
}

TEST_F(IisModuleBaseFetchWriteTest, FailedFollowUpAllocationFailsTheWrite) {
  // The first block fills up; the block for the remaining bytes is refused.
  FakeContext context(1);
  FetchPtr fetch = NewFetch(&context);
  EXPECT_FALSE(fetch->Write(GoogleString(70000, 'z'), &handler_));
  EXPECT_EQ(2, context.calls());
}

TEST_F(IisModuleBaseFetchWriteTest, FailedWriteFailsTheResponse) {
  FakeContext context(0);
  IisModuleBaseFetch* fetch = new IisModuleBaseFetch(
      &context, kResource, &handler_,
      RequestContext::NewTestRequestContext(&thread_system_), nullptr);
  fetch->response_headers()->set_status_code(200);
  EXPECT_FALSE(fetch->Write("hello", &handler_));
  // Memory is available again, but the body already has a hole.
  context.set_budget(10);
  EXPECT_FALSE(fetch->Write(" world", &handler_));
  EXPECT_EQ(1, context.calls());
  EXPECT_EQ(0, context.response().resets());
  // Done releases one reference; the test holds the other.
  fetch->Done(true);
  EXPECT_EQ(1, context.response().resets());
  fetch->ReleaseRef();
}

}  // namespace
}  // namespace net_instaweb
