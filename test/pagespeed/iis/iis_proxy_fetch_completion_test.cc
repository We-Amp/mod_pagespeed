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

// Regression test for the OnSendResponse "leave" path use-after-free:
// ProxyFetch::Done() can synchronously tear the request down and free the inner
// request context, so the completion helper must touch the context only BEFORE
// Done(). These fakes flag any access to the context after it is "freed" (i.e.
// after Done() runs), reproducing what AppVerifier + Page Heap caught on IIS.

#include "pagespeed/iis/iis_proxy_fetch_completion.h"

#include <vector>

#include "gtest/gtest-spi.h"
#include "pagespeed/kernel/base/string.h"
#include "pagespeed/kernel/base/string_util.h"
#include "test/pagespeed/kernel/base/gtest.h"

namespace net_instaweb {
namespace {

class FakeInner;

// Stands in for ProxyFetch. Its Done() simulates a SYNCHRONOUS completion that
// frees the inner request context (HandleDone -> IndicateCompletion ->
// CleanupStoredContext -> delete inner).
class FakeProxyFetch {
 public:
  FakeProxyFetch(FakeInner* inner, std::vector<GoogleString>* log)
      : inner_(inner), log_(log) {}
  void Done(bool success);  // defined after FakeInner

 private:
  FakeInner* inner_;
  std::vector<GoogleString>* log_;
};

class FakeInner {
 public:
  explicit FakeInner(std::vector<GoogleString>* log) : log_(log) {}

  // Any access while !live_ is a use-after-free.
  FakeProxyFetch* proxy_fetch() {
    CheckLive("proxy_fetch");
    return proxy_fetch_;
  }
  void set_proxy_fetch(FakeProxyFetch* x) {
    CheckLive("set_proxy_fetch");
    proxy_fetch_ = x;
    log_->push_back("set_proxy_fetch");
  }
  void set_leave(bool x) {
    CheckLive("set_leave");
    leave_ = x;
    log_->push_back("set_leave");
  }

  void Free() { live_ = false; }
  bool leave() const { return leave_; }

 private:
  void CheckLive(const char* who) {
    if (!live_) {
      ADD_FAILURE() << "use-after-free: " << who
                    << "() called after the context was freed by Done()";
    }
  }

  std::vector<GoogleString>* log_;
  FakeProxyFetch* proxy_fetch_ = nullptr;
  bool leave_ = false;
  bool live_ = true;
};

void FakeProxyFetch::Done(bool success) {
  log_->push_back("Done");
  // Simulate the synchronous CleanupStoredContext that frees the inner context.
  inner_->Free();
}

// The fix: state is cleared first, Done() is last, nothing touches the context
// after Done(). With the buggy order (Done() first) the fakes would record a
// use-after-free via ADD_FAILURE.
TEST(FinishProxyFetchAndLeaveTest, DrivesDoneLastAndNeverTouchesContextAfter) {
  std::vector<GoogleString> log;
  FakeInner inner(&log);
  FakeProxyFetch fetch(&inner, &log);
  inner.set_proxy_fetch(&fetch);
  log.clear();  // ignore the setup write

  FinishProxyFetchAndLeave<FakeInner, FakeProxyFetch>(&inner);

  // Done() must be the final operation; all context mutation precedes it.
  ASSERT_FALSE(log.empty());
  EXPECT_STREQ("Done", log.back().c_str());
  EXPECT_EQ(StrCat("set_proxy_fetch,set_leave,Done"), JoinCollection(log, ","));
}

// The leave flag must be set (a re-entrant OnSendResponse relies on it to bail).
TEST(FinishProxyFetchAndLeaveTest, SetsLeaveBeforeDone) {
  std::vector<GoogleString> log;
  FakeInner inner(&log);
  FakeProxyFetch fetch(&inner, &log);
  inner.set_proxy_fetch(&fetch);

  FinishProxyFetchAndLeave<FakeInner, FakeProxyFetch>(&inner);

  EXPECT_TRUE(inner.leave());
}

// Proves the fakes actually catch the ORIGINAL buggy order (the regression we
// are fixing): Done() first, then touch the now-freed context. This is the
// "red" the helper turns green — verified in a single build via gtest's
// failure-capture so we don't need to ship the buggy code to demonstrate it.
TEST(FinishProxyFetchAndLeaveTest, FakesDetectBuggyDoneFirstOrder) {
  std::vector<GoogleString> log;
  FakeInner inner(&log);
  FakeProxyFetch fetch(&inner, &log);
  inner.set_proxy_fetch(&fetch);
  EXPECT_NONFATAL_FAILURE(
      {
        FakeProxyFetch* pf = inner.proxy_fetch();  // live: ok
        pf->Done(true);                            // frees the context
        inner.set_proxy_fetch(nullptr);            // use-after-free
      },
      "use-after-free");
}

// A null proxy_fetch must be a safe no-op-ish path (no Done(), no crash).
TEST(FinishProxyFetchAndLeaveTest, NullProxyFetchDoesNotCallDone) {
  std::vector<GoogleString> log;
  FakeInner inner(&log);  // proxy_fetch_ defaults to null
  log.clear();

  FinishProxyFetchAndLeave<FakeInner, FakeProxyFetch>(&inner);

  EXPECT_TRUE(inner.leave());
  for (const GoogleString& entry : log) {
    EXPECT_STRNE("Done", entry.c_str());
  }
}

}  // namespace
}  // namespace net_instaweb
