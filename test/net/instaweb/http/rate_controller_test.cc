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

// Unit tests for RateController itself, with fetchers that complete before
// Fetch() returns.

#include "net/instaweb/http/public/rate_controller.h"

#include <memory>
#include <vector>

#include "net/instaweb/http/public/async_fetch.h"
#include "net/instaweb/http/public/request_context.h"
#include "net/instaweb/http/public/url_async_fetcher.h"
#include "pagespeed/kernel/base/null_message_handler.h"
#include "pagespeed/kernel/base/string.h"
#include "pagespeed/kernel/base/string_util.h"
#include "pagespeed/kernel/base/thread_system.h"
#include "pagespeed/kernel/http/http_names.h"
#include "pagespeed/kernel/http/response_headers.h"
#include "pagespeed/kernel/util/platform.h"
#include "pagespeed/kernel/util/simple_stats.h"
#include "test/pagespeed/kernel/base/gtest.h"

namespace net_instaweb {

namespace {

class BackgroundFetch : public StringAsyncFetch {
 public:
  explicit BackgroundFetch(const RequestContextPtr& ctx)
      : StringAsyncFetch(ctx) {}
  bool IsBackgroundFetch() const override { return true; }
};

// Holds fetches until Release() is called; after that every fetch completes
// before Fetch() returns. Records the order in which fetches reach it and the
// deepest nesting of Fetch() calls.
class SyncCompletingFetcher : public UrlAsyncFetcher {
 public:
  void Fetch(const GoogleString& url, MessageHandler* handler,
             AsyncFetch* fetch) override {
    started_.push_back(url);
    if (!release_) {
      held_.push_back(fetch);
      return;
    }
    ++depth_;
    if (depth_ > max_depth_) {
      max_depth_ = depth_;
    }
    Complete(fetch);
    --depth_;
  }

  // Completes the held fetches from outside any Fetch() call.
  void Release() {
    release_ = true;
    std::vector<AsyncFetch*> held;
    held.swap(held_);
    for (AsyncFetch* fetch : held) {
      ++depth_;
      if (depth_ > max_depth_) {
        max_depth_ = depth_;
      }
      Complete(fetch);
      --depth_;
    }
  }

  const std::vector<GoogleString>& started() const { return started_; }
  int max_depth() const { return max_depth_; }

 private:
  static void Complete(AsyncFetch* fetch) {
    fetch->response_headers()->SetStatusAndReason(HttpStatus::kOK);
    fetch->HeadersComplete();
    fetch->Done(true);
  }

  bool release_ = false;
  int depth_ = 0;
  int max_depth_ = 0;
  std::vector<AsyncFetch*> held_;
  std::vector<GoogleString> started_;
};

class RateControllerTest : public ::testing::Test {
 protected:
  RateControllerTest()
      : thread_system_(Platform::CreateThreadSystem()),
        stats_(thread_system_.get()) {
    RateController::InitStats(&stats_);
  }

  int global_fetch_queue_size() {
    return stats_
        .GetUpDownCounter(RateController::kCurrentGlobalFetchQueueSize)
        ->Get();
  }

  std::unique_ptr<ThreadSystem> thread_system_;
  SimpleStats stats_;
  NullMessageHandler handler_;
};

TEST_F(RateControllerTest, SynchronousCompletionDrainsQueueWithBoundedDepth) {
  constexpr int kQueued = 1000;
  RateController controller(kQueued, 1, kQueued, thread_system_.get(), &stats_);
  SyncCompletingFetcher fetcher;

  // One fetch goes out and is held; the rest queue behind it.
  std::vector<std::unique_ptr<BackgroundFetch>> fetches;
  std::vector<GoogleString> urls;
  for (int i = 0; i <= kQueued; ++i) {
    urls.push_back(StrCat("http://www.example.com/", IntegerToString(i)));
    fetches.push_back(std::make_unique<BackgroundFetch>(
        RequestContext::NewTestRequestContext(thread_system_.get())));
    controller.Fetch(&fetcher, urls[i], &handler_, fetches[i].get());
  }
  EXPECT_EQ(1, fetcher.started().size());
  EXPECT_EQ(kQueued, global_fetch_queue_size());

  // Completing the first fetch starts the next one, which completes at once,
  // and so on through the whole queue.
  fetcher.Release();

  EXPECT_EQ(0, global_fetch_queue_size());
  EXPECT_EQ(kQueued + 1, fetcher.started().size());
  EXPECT_EQ(urls, fetcher.started());  // Same order as queued.
  for (const auto& fetch : fetches) {
    EXPECT_TRUE(fetch->done());
    EXPECT_TRUE(fetch->success());
  }
  // The outermost completion plus the one fetch it starts at a time.
  EXPECT_GE(2, fetcher.max_depth());
}

TEST_F(RateControllerTest, SynchronousCompletionStillAllowsNewFetches) {
  RateController controller(10, 1, 10, thread_system_.get(), &stats_);
  SyncCompletingFetcher fetcher;
  fetcher.Release();
  // Everything completes inside Fetch(); the host bookkeeping must be left
  // clean so later fetches are not queued or dropped.
  for (int i = 0; i < 50; ++i) {
    BackgroundFetch fetch(
        RequestContext::NewTestRequestContext(thread_system_.get()));
    controller.Fetch(&fetcher, "http://www.example.com/x", &handler_, &fetch);
    EXPECT_TRUE(fetch.done());
    EXPECT_TRUE(fetch.success());
  }
  EXPECT_EQ(50, fetcher.started().size());
  EXPECT_EQ(0, global_fetch_queue_size());
}

}  // namespace

}  // namespace net_instaweb
