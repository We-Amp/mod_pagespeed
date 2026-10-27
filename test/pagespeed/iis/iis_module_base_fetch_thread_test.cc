// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

// Thread rule for rewritten HTML.  The rewriter produces HTML on its own
// threads while the request sits on an IIS thread, and IIS request objects
// are not thread safe.  The base fetch therefore keeps HTML in module memory
// and only the thread that owns the request (the one inside OnSendResponse,
// then the one completing the request) may call into IIS.  The fake IIS
// objects here record the thread of every call and detect overlapping calls.

#include <atomic>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
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

// Shared by the fake context and response: who called what, and whether two
// calls were ever inside the fakes at once.
class CallLog {
 public:
  void Reset() {
    std::lock_guard<std::mutex> guard(mu_);
    calls_.clear();
    overlaps_ = 0;
  }
  int overlaps() const { return overlaps_.load(); }
  int Count(const char* method) {
    std::lock_guard<std::mutex> guard(mu_);
    int n = 0;
    for (const Call& c : calls_) {
      if (strcmp(c.method, method) == 0) ++n;
    }
    return n;
  }
  int Total() {
    std::lock_guard<std::mutex> guard(mu_);
    return static_cast<int>(calls_.size());
  }
  // Calls made by |thread|.
  int CallsFrom(DWORD thread) {
    std::lock_guard<std::mutex> guard(mu_);
    int n = 0;
    for (const Call& c : calls_) {
      if (c.thread == thread) ++n;
    }
    return n;
  }
  // Calls not made by |thread|.
  int CallsNotFrom(DWORD thread) {
    std::lock_guard<std::mutex> guard(mu_);
    int n = 0;
    for (const Call& c : calls_) {
      if (c.thread != thread) ++n;
    }
    return n;
  }
  DWORD ThreadOfLast(const char* method) {
    std::lock_guard<std::mutex> guard(mu_);
    DWORD thread = 0;
    for (const Call& c : calls_) {
      if (strcmp(c.method, method) == 0) thread = c.thread;
    }
    return thread;
  }

  // One call inside the fakes.  Holds the "busy" flag for its lifetime and
  // yields once inside, so a second thread arriving meanwhile is seen.
  class Scope {
   public:
    Scope(CallLog* log, const char* method) : log_(log) {
      if (log_->busy_.fetch_add(1) != 0) {
        log_->overlaps_.fetch_add(1);
      }
      std::lock_guard<std::mutex> guard(log_->mu_);
      log_->calls_.push_back({method, GetCurrentThreadId()});
    }
    ~Scope() {
      std::this_thread::yield();
      log_->busy_.fetch_sub(1);
    }

   private:
    CallLog* log_;
  };

 private:
  struct Call {
    const char* method;
    DWORD thread;
  };
  std::mutex mu_;
  std::vector<Call> calls_;
  std::atomic<int> busy_{0};
  std::atomic<int> overlaps_{0};
};

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

class FakeCachePolicy : public IHttpCachePolicy {
 public:
  FakeCachePolicy() {
    memset(&kernel_, 0, sizeof(kernel_));
    memset(&user_, 0, sizeof(user_));
  }
  HTTP_CACHE_POLICY* GetKernelCachePolicy(VOID) override { return &kernel_; }
  VOID SetKernelCacheInvalidatorSet(VOID) override {}
  HTTP_CACHE_POLICY* GetUserCachePolicy(VOID) override { return &user_; }
  HRESULT AppendVaryByHeader(PCSTR) override { return S_OK; }
  PCSTR GetVaryByHeaders(VOID) const override { return nullptr; }
  HRESULT AppendVaryByQueryString(PCSTR) override { return S_OK; }
  PCSTR GetVaryByQueryStrings(VOID) const override { return nullptr; }
  HRESULT SetVaryByValue(PCSTR) override { return S_OK; }
  PCSTR GetVaryByValue(VOID) const override { return nullptr; }
  BOOL IsUserCacheEnabled(VOID) const override { return TRUE; }
  VOID DisableUserCache(VOID) override {}
  BOOL IsCached(VOID) const override { return FALSE; }
  VOID SetIsCached(VOID) override {}
  BOOL GetKernelCacheInvalidatorSet(VOID) const override { return FALSE; }

 private:
  HTTP_CACHE_POLICY kernel_;
  HTTP_CACHE_POLICY user_;
};

// An IIS response that logs every call it gets and keeps the body chunks it
// was handed.
class FakeResponse : public StubHttpResponse {
 public:
  explicit FakeResponse(CallLog* log) : log_(log) {}

  IHttpCachePolicy* GetCachePolicy() override {
    CallLog::Scope s(log_, "GetCachePolicy");
    return &cache_policy_;
  }
  HRESULT SetStatus(USHORT, PCSTR, USHORT, HRESULT, IAppHostConfigException*,
                    BOOL) override {
    CallLog::Scope s(log_, "SetStatus");
    return S_OK;
  }
  HRESULT SetHeader(PCSTR, PCSTR, USHORT, BOOL) override {
    CallLog::Scope s(log_, "SetHeader");
    return S_OK;
  }
  HRESULT SetHeader(HTTP_HEADER_ID, PCSTR, USHORT, BOOL) override {
    CallLog::Scope s(log_, "SetHeader");
    return S_OK;
  }
  HRESULT DeleteHeader(PCSTR) override {
    CallLog::Scope s(log_, "DeleteHeader");
    return S_OK;
  }
  HRESULT DeleteHeader(HTTP_HEADER_ID) override {
    CallLog::Scope s(log_, "DeleteHeader");
    return S_OK;
  }
  VOID Clear() override { CallLog::Scope s(log_, "Clear"); }
  VOID DisableBuffering() override {
    CallLog::Scope s(log_, "DisableBuffering");
  }
  VOID ResetConnection() override { CallLog::Scope s(log_, "ResetConnection"); }
  BOOL GetHeadersSuppressed() const override {
    CallLog::Scope s(log_, "GetHeadersSuppressed");
    return FALSE;
  }
  HRESULT WriteEntityChunkByReference(HTTP_DATA_CHUNK* chunk, LONG) override {
    CallLog::Scope s(log_, "WriteEntityChunkByReference");
    body_.append(static_cast<const char*>(chunk->FromMemory.pBuffer),
                 chunk->FromMemory.BufferLength);
    return S_OK;
  }

  const GoogleString& body() const { return body_; }

 private:
  CallLog* log_;
  FakeCachePolicy cache_policy_;
  GoogleString body_;
};

// Hands out request memory until |budget| allocations have been made.
class FakeContext : public StubHttpContext {
 public:
  explicit FakeContext(int budget) : response_(&log_), budget_(budget) {}
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
    CallLog::Scope s(&log_, "AllocateRequestMemory");
    if (budget_.load() <= 0) {
      return nullptr;
    }
    budget_.fetch_sub(1);
    void* p = malloc(size);
    std::lock_guard<std::mutex> guard(blocks_mu_);
    blocks_.push_back(p);
    return p;
  }
  VOID SetRequestHandled() override {
    CallLog::Scope s(&log_, "SetRequestHandled");
  }
  VOID IndicateCompletion(REQUEST_NOTIFICATION_STATUS status) override {
    CallLog::Scope s(&log_, "IndicateCompletion");
    last_status_ = status;
    if (on_complete_) on_complete_();
  }

  CallLog* log() { return &log_; }
  FakeResponse* response() { return &response_; }
  REQUEST_NOTIFICATION_STATUS last_status() const { return last_status_; }
  void set_on_complete(std::function<void()> f) { on_complete_ = f; }

 private:
  CallLog log_;
  FakeRequest request_;
  FakeResponse response_;
  std::atomic<int> budget_;
  std::mutex blocks_mu_;
  std::vector<void*> blocks_;
  REQUEST_NOTIFICATION_STATUS last_status_ = RQ_NOTIFICATION_CONTINUE;
  std::function<void()> on_complete_;
};

// Exposes what the tests need beyond the public surface: whether the fetch
// lock is free, and when the fetch is destroyed.
class TestFetch : public IisModuleBaseFetch {
 public:
  TestFetch(IHttpContext* context, FetchType type, MessageHandler* handler,
            const RequestContextPtr& request_context, bool* destroyed)
      : IisModuleBaseFetch(context, type, handler, request_context, nullptr),
        destroyed_(destroyed) {}
  ~TestFetch() override { *destroyed_ = true; }
  // Run on a thread other than the one holding the lock, if any.
  bool LockIsFreeFromAnotherThread() {
    bool free_lock = false;
    std::thread other([this, &free_lock] {
      if (TryEnterCriticalSection(&critical_section_)) {
        free_lock = true;
        LeaveCriticalSection(&critical_section_);
      }
    });
    other.join();
    return free_lock;
  }

 private:
  bool* destroyed_;
};

class IisModuleBaseFetchThreadTest : public testing::Test {
 protected:
  // New fetches start with two references (the context's and the
  // completion's); a fetch that never ran Done releases both here.
  void NewFetch(FakeContext* context, FetchType type = kHtml) {
    fetch_ = new TestFetch(
        context, type, &handler_,
        RequestContext::NewTestRequestContext(&thread_system_), &destroyed_);
    fetch_->response_headers()->set_status_code(200);
    fetch_->response_headers()->Add("Content-Type", "text/html");
    // The constructor reads the request; only later calls are of interest.
    context->log()->Reset();
  }
  void ReleaseWithoutDone() {
    fetch_->ReleaseRef();
    fetch_->ReleaseRef();
    fetch_ = nullptr;
  }

  TestFetch* fetch_ = nullptr;
  bool destroyed_ = false;
  WinThreadSystem thread_system_;
  NullMessageHandler handler_;
};

// (a) Rewritten HTML arrives on a worker thread before the request has been
// handed over.  The worker must not call into IIS at all: not for the
// headers its first write triggers, not for the body.
TEST_F(IisModuleBaseFetchThreadTest, WorkerOutputBeforeHandOffNeverCallsIis) {
  FakeContext context(1000);
  NewFetch(&context);
  DWORD worker_id = 0;
  std::thread worker([&] {
    worker_id = GetCurrentThreadId();
    fetch_->Write("<html><head></head>", &handler_);
    fetch_->Write(GoogleString(5000, 'x'), &handler_);
    fetch_->Write("</html>", &handler_);
  });
  worker.join();
  EXPECT_EQ(0, context.log()->CallsFrom(worker_id));
  EXPECT_EQ(0, context.log()->Total());
  ReleaseWithoutDone();
}

// (b) The thread that owns the request keeps working on the IIS response
// while the worker writes a lot of output: no two calls into IIS are ever
// inside it at the same time.
TEST_F(IisModuleBaseFetchThreadTest, OwnerThreadAndWorkerNeverOverlapInIis) {
  FakeContext context(1000000);
  NewFetch(&context);
  std::atomic<bool> worker_done(false);
  std::thread worker([&] {
    const GoogleString chunk(5000, 'x');
    for (int i = 0; i < 10000; ++i) {
      fetch_->Write(chunk, &handler_);
    }
    worker_done = true;
  });
  IHttpResponse* response = context.GetResponse();
  int rounds = 0;
  while (!worker_done.load()) {
    response->Clear();
    response->SetHeader("X-Owner", "1", 1, TRUE);
    ++rounds;
  }
  worker.join();
  EXPECT_GT(rounds, 0);
  EXPECT_EQ(0, context.log()->overlaps());
  ReleaseWithoutDone();
}

// ---- The cases below use the hand-off API. ----

// (c) After the hand-off the worker completes the request: exactly the bytes
// written, in one chunk, one completion, and no lock held across it.
TEST_F(IisModuleBaseFetchThreadTest, HandOffThenDoneOnWorkerCompletesOnce) {
  FakeContext context(10);
  NewFetch(&context);
  bool lock_free_in_completion = false;
  context.set_on_complete(
      [&] { lock_free_in_completion = fetch_->LockIsFreeFromAnotherThread(); });
  fetch_->EnterNotification();
  EXPECT_TRUE(fetch_->Write("<html>", &handler_));
  EXPECT_TRUE(fetch_->Write("<body>hi</body>", &handler_));
  EXPECT_TRUE(fetch_->Write("</html>", &handler_));
  EXPECT_EQ(0, context.log()->Total());
  fetch_->HandOffToCompletion();
  fetch_->LeaveNotification();
  DWORD worker_id = 0;
  std::thread worker([&] {
    worker_id = GetCurrentThreadId();
    fetch_->Done(true);
  });
  worker.join();
  EXPECT_EQ("<html><body>hi</body></html>", context.response()->body());
  EXPECT_EQ(1, context.log()->Count("WriteEntityChunkByReference"));
  EXPECT_EQ(1, context.log()->Count("IndicateCompletion"));
  EXPECT_EQ(RQ_NOTIFICATION_CONTINUE, context.last_status());
  EXPECT_EQ(worker_id, context.log()->ThreadOfLast("IndicateCompletion"));
  EXPECT_TRUE(lock_free_in_completion);
  EXPECT_EQ(0, context.log()->overlaps());
  // Done released the completion reference; the context's is left.
  fetch_->ReleaseRef();
  fetch_ = nullptr;
  EXPECT_TRUE(destroyed_);
}

// (d) The owner collected the headers (IIS is sending them): the worker's
// first write does not apply them a second time, and neither does the
// completion.
TEST_F(IisModuleBaseFetchThreadTest, ForcedCollectIsNotRepeatedByTheWorker) {
  FakeContext context(10);
  NewFetch(&context);
  const DWORD owner_id = GetCurrentThreadId();
  fetch_->EnterNotification();
  EXPECT_EQ(0, fetch_->CollectHeaders());
  EXPECT_TRUE(fetch_->headers_collected());
  EXPECT_EQ(1, context.log()->Count("SetStatus"));
  std::thread worker([&] { fetch_->Write("<html></html>", &handler_); });
  worker.join();
  EXPECT_EQ(1, context.log()->Count("SetStatus"));
  fetch_->HandOffToCompletion();
  fetch_->LeaveNotification();
  fetch_->Done(true);
  EXPECT_EQ(1, context.log()->Count("SetStatus"));
  EXPECT_EQ(0, context.log()->CallsNotFrom(owner_id));
  fetch_->ReleaseRef();
  fetch_ = nullptr;
}

// (e) Nothing collected the headers before the final send: the completion
// does, once.
TEST_F(IisModuleBaseFetchThreadTest, CompletionCollectsHeadersWhenNoneYet) {
  FakeContext context(10);
  NewFetch(&context);
  fetch_->Write("<html></html>", &handler_);
  EXPECT_FALSE(fetch_->headers_collected());
  EXPECT_EQ(0, context.log()->Total());
  fetch_->EnterNotification();
  fetch_->HandOffToCompletion();
  fetch_->LeaveNotification();
  fetch_->Done(true);
  EXPECT_EQ(1, context.log()->Count("SetStatus"));
  EXPECT_EQ(1, context.log()->Count("WriteEntityChunkByReference"));
  EXPECT_EQ("<html></html>", context.response()->body());
  fetch_->ReleaseRef();
  fetch_ = nullptr;
}

// (f) The request went away while the rewriter was still running: late
// worker output and the late Done never reach IIS, and the fetch is freed.
TEST_F(IisModuleBaseFetchThreadTest, DetachedFetchNeverCallsIis) {
  FakeContext context(10);
  NewFetch(&context);
  EXPECT_TRUE(fetch_->Write("<html>", &handler_));
  fetch_->DetachFromRequest();
  std::thread worker([&] {
    fetch_->Write("</html>", &handler_);
    fetch_->Done(true);
  });
  worker.join();
  EXPECT_EQ(0, context.log()->Total());
  EXPECT_FALSE(destroyed_);
  // Done released the completion reference; the context's is left.
  fetch_->ReleaseRef();
  fetch_ = nullptr;
  EXPECT_TRUE(destroyed_);
}

// (g) The request pool is exhausted when the body is moved into request
// memory at completion: the response is reset once, not completed with a
// hole.
TEST_F(IisModuleBaseFetchThreadTest, PoolExhaustedAtCompletionResetsOnce) {
  FakeContext context(0);
  NewFetch(&context);
  EXPECT_TRUE(fetch_->Write("<html></html>", &handler_));
  fetch_->EnterNotification();
  fetch_->HandOffToCompletion();
  fetch_->LeaveNotification();
  fetch_->Done(true);
  EXPECT_EQ(1, context.log()->Count("ResetConnection"));
  EXPECT_EQ(0, context.log()->Count("WriteEntityChunkByReference"));
  EXPECT_EQ(1, context.log()->Count("IndicateCompletion"));
  fetch_->ReleaseRef();
  fetch_ = nullptr;
}

// (h) Resource fetches are not subject to the HTML thread rule: they still
// buffer into request memory as they are written.
TEST_F(IisModuleBaseFetchThreadTest, ResourceFetchStillAllocatesAtWrite) {
  FakeContext context(10);
  NewFetch(&context, kResource);
  EXPECT_TRUE(fetch_->Write("body", &handler_));
  EXPECT_EQ(1, context.log()->Count("AllocateRequestMemory"));
  ReleaseWithoutDone();
}

}  // namespace
}  // namespace net_instaweb
