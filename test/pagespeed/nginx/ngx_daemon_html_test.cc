// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

#include "pagespeed/nginx/ngx_daemon_html.h"

#include <atomic>
#include <chrono>
#include <memory>
#include <thread>
#include <vector>

#include "pagespeed/kernel/base/statistics.h"
#include "pagespeed/kernel/base/string.h"
#include "pagespeed/kernel/base/thread_system.h"
#include "pagespeed/kernel/thread/queued_worker_pool.h"
#include "pagespeed/kernel/util/platform.h"
#include "pagespeed/kernel/util/simple_stats.h"
#include "pagespeed/nginx/ngx_daemon_record_completion.h"
#include "pagespeed/system/module_entity_tags.h"
#include "test/pagespeed/kernel/base/gtest.h"

namespace net_instaweb {
namespace {

class NgxDaemonHtmlTest : public testing::Test {
 protected:
  NgxDaemonHtmlTest()
      : thread_system_(Platform::CreateThreadSystem()),
        stats_(thread_system_.get()) {
    ps_daemon_record_completion_init_stats(&stats_);
    NgxDaemonHtmlInitStats(&stats_);
    posted_ = stats_.FindVariable(kDaemonHtmlCommitsPosted);
    finished_ = stats_.FindVariable(kDaemonHtmlCommitsFinished);
    dropped_ = stats_.FindVariable(kDaemonHtmlCommitsDropped);
    ResetDaemonCompletionQueueForTesting();
  }
  ~NgxDaemonHtmlTest() override { ResetDaemonCompletionQueueForTesting(); }

  // A commit that records where it ran and holds a token whose release
  // stands for the byte budget reservation the engine's commit carries.
  DaemonHtmlCommit Commit(std::atomic<int>* runs,
                          std::shared_ptr<int>* token = nullptr,
                          std::thread::id* ran_on = nullptr) {
    std::shared_ptr<int> hold = std::make_shared<int>(1);
    if (token != nullptr) *token = hold;
    return DaemonHtmlCommit([runs, hold, ran_on] {
      runs->fetch_add(1);
      if (ran_on != nullptr) *ran_on = std::this_thread::get_id();
    });
  }

  std::unique_ptr<ThreadSystem> thread_system_;
  SimpleStats stats_;
  Variable* posted_ = nullptr;
  Variable* finished_ = nullptr;
  Variable* dropped_ = nullptr;
};

TEST_F(NgxDaemonHtmlTest, TheCountersAreRegisteredWithTheRecordArm) {
  ASSERT_NE(nullptr, posted_);
  ASSERT_NE(nullptr, finished_);
  ASSERT_NE(nullptr, dropped_);
  EXPECT_STREQ("daemon_html_commits_posted", kDaemonHtmlCommitsPosted);
  EXPECT_STREQ("daemon_html_commits_finished", kDaemonHtmlCommitsFinished);
  EXPECT_STREQ("daemon_html_commits_dropped", kDaemonHtmlCommitsDropped);
}

// A commit never runs on the thread that posts it (nginx's event thread):
// it runs on the pool, and the posted and finished counters agree after.
TEST_F(NgxDaemonHtmlTest, ACommitRunsOnThePoolAndIsCountedBothWays) {
  NgxDaemonHtmlRunner runner(/*bounded=*/true, posted_, finished_, dropped_);
  QueuedWorkerPool pool(1, "daemon_html_test", thread_system_.get());
  runner.set_sequence(pool.NewSequence());
  std::atomic<int> runs{0};
  std::shared_ptr<int> token;
  std::weak_ptr<int> weak;
  std::thread::id ran_on;
  runner.RunBlocking(Commit(&runs, &token, &ran_on));
  weak = token;
  token.reset();
  pool.ShutDown();
  // Run or cancelled by the shutdown: either way exactly once, counted, and
  // the reservation it owned is gone.
  EXPECT_EQ(1, posted_->Get());
  EXPECT_EQ(1, finished_->Get());
  EXPECT_EQ(0, dropped_->Get());
  EXPECT_TRUE(weak.expired());
  if (runs.load() == 1) {
    EXPECT_NE(std::this_thread::get_id(), ran_on);
  }
}

// No pool: a store's commit is dropped, never run inline on the event
// thread, and what it owned is released.
TEST_F(NgxDaemonHtmlTest, WithoutASequenceABoundedCommitIsDropped) {
  NgxDaemonHtmlRunner runner(/*bounded=*/true, posted_, finished_, dropped_);
  std::atomic<int> runs{0};
  std::shared_ptr<int> token;
  runner.RunBlocking(Commit(&runs, &token));
  std::weak_ptr<int> weak = token;
  token.reset();
  EXPECT_EQ(0, runs.load());
  EXPECT_TRUE(weak.expired());
  EXPECT_EQ(1, dropped_->Get());
  EXPECT_EQ(0, posted_->Get());
}

// The queue caps the record arm shares: a full queue turns a store away.
TEST_F(NgxDaemonHtmlTest, AFullQueueTurnsAStoreAway) {
  for (int i = 0; i < kDaemonCompletionQueueMaxItems; ++i) {
    ASSERT_TRUE(TryQueueDaemonCompletion(0, nullptr));
  }
  NgxDaemonHtmlRunner runner(/*bounded=*/true, posted_, finished_, dropped_);
  QueuedWorkerPool pool(1, "daemon_html_test", thread_system_.get());
  runner.set_sequence(pool.NewSequence());
  std::atomic<int> runs{0};
  runner.RunBlocking(Commit(&runs));
  pool.ShutDown();
  EXPECT_EQ(0, runs.load());
  EXPECT_EQ(1, dropped_->Get());
  EXPECT_EQ(0, posted_->Get());
}

// At shutdown queued work is cancelled unrun, and what it owned is released.
TEST_F(NgxDaemonHtmlTest, AShutdownCancelsQueuedWorkUnrun) {
  NgxDaemonHtmlRunner stores(/*bounded=*/true, posted_, finished_, dropped_);
  NgxDaemonHtmlRunner admin(/*bounded=*/false, posted_, finished_, dropped_);
  QueuedWorkerPool pool(1, "daemon_html_test", thread_system_.get());
  QueuedWorkerPool::Sequence* sequence = pool.NewSequence();
  stores.set_sequence(sequence);
  admin.set_sequence(sequence);
  pool.InitiateShutDown();
  std::atomic<int> store_runs{0};
  std::atomic<int> admin_runs{0};
  std::shared_ptr<int> token;
  stores.RunBlocking(Commit(&store_runs, &token));
  std::weak_ptr<int> weak = token;
  token.reset();
  admin.RunBlocking(Commit(&admin_runs));
  pool.WaitForShutDownComplete();
  EXPECT_EQ(0, store_runs.load());
  EXPECT_EQ(0, admin_runs.load());
  EXPECT_TRUE(weak.expired());
  EXPECT_EQ(2, posted_->Get());
  EXPECT_EQ(2, finished_->Get());
  // The queue room the store reserved is back.
  for (int i = 0; i < kDaemonCompletionQueueMaxItems; ++i) {
    EXPECT_TRUE(TryQueueDaemonCompletion(0, nullptr));
  }
}

// Unbounded work is never turned away by the caps.
TEST_F(NgxDaemonHtmlTest, AdminWorkIgnoresTheQueueCaps) {
  for (int i = 0; i < kDaemonCompletionQueueMaxItems; ++i) {
    ASSERT_TRUE(TryQueueDaemonCompletion(0, nullptr));
  }
  NgxDaemonHtmlRunner admin(/*bounded=*/false, posted_, finished_, dropped_);
  QueuedWorkerPool pool(1, "daemon_html_test", thread_system_.get());
  admin.set_sequence(pool.NewSequence());
  std::atomic<int> runs{0};
  admin.RunBlocking(Commit(&runs));
  for (int i = 0; i < 5000 && runs.load() == 0; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  pool.ShutDown();
  EXPECT_EQ(1, runs.load());
  EXPECT_EQ(0, dropped_->Get());
}

TEST_F(NgxDaemonHtmlTest, RequestFactsCountEveryLine) {
  const std::vector<NgxHeaderLine> lines = {
      {"Accept", "text/html;q=0.5"},
      {"accept", "text/markdown"},
      {"Cache-Control", "max-age=0"},
      {"CACHE-CONTROL", "no-transform"},
      {"Pragma", "no-cache"},
      {"Proxy-Authorization", "Basic eA=="},
      {"Cookie2", "$Version=1"},
      {"If-Match", "\"a\""},
      {"If-Unmodified-Since", "Sun, 15 Nov 2026 12:00:00 GMT"},
      {"Range", "bytes=0-1"},
      {"If-Range", "\"a\""},
      {"Origin", "https://other.test"},
      // Read from the stash only, never from the lines.
      {"If-None-Match", "W/\"psm-x\""},
      {"If-Modified-Since", "Sun, 15 Nov 2026 12:00:00 GMT"},
  };
  DaemonHtmlRequestFacts facts;
  FillDaemonHtmlRequestHeaderFacts(lines, nullptr, &facts);
  EXPECT_EQ((std::vector<GoogleString>{"text/html;q=0.5", "text/markdown"}),
            facts.accept_lines);
  EXPECT_TRUE(facts.has_cache_control);
  EXPECT_EQ("max-age=0, no-transform", facts.cache_control);
  EXPECT_TRUE(facts.has_pragma);
  EXPECT_EQ("no-cache", facts.pragma);
  EXPECT_TRUE(facts.has_authorization);
  EXPECT_TRUE(facts.has_cookie);
  EXPECT_TRUE(facts.has_if_match);
  EXPECT_TRUE(facts.has_if_unmodified_since);
  EXPECT_TRUE(facts.has_range);
  EXPECT_TRUE(facts.has_if_range);
  EXPECT_TRUE(facts.has_origin);
  EXPECT_FALSE(facts.has_if_none_match);
  EXPECT_FALSE(facts.has_if_modified_since);

  ClientConditionalStash stash;
  stash.has_if_none_match = true;
  stash.if_none_match = "W/\"psm-x\", \"v1\"";
  DaemonHtmlRequestFacts with_stash;
  FillDaemonHtmlRequestHeaderFacts({}, &stash, &with_stash);
  EXPECT_TRUE(with_stash.has_if_none_match);
  EXPECT_EQ("W/\"psm-x\", \"v1\"", with_stash.if_none_match);
  EXPECT_FALSE(with_stash.has_if_modified_since);
  EXPECT_FALSE(with_stash.has_cache_control);
  EXPECT_TRUE(with_stash.accept_lines.empty());
}

TEST_F(NgxDaemonHtmlTest, AnEmptyFieldIsPresent) {
  DaemonHtmlRequestFacts facts;
  FillDaemonHtmlRequestHeaderFacts(
      {{"Cache-Control", ""}, {"Authorization", ""}}, nullptr, &facts);
  EXPECT_TRUE(facts.has_cache_control);
  EXPECT_EQ("", facts.cache_control);
  EXPECT_TRUE(facts.has_authorization);
  EXPECT_FALSE(facts.has_cookie);
}

// proxy_hide_header cannot hide what refuses a store: those lines come
// from the upstream as it sent them.
TEST_F(NgxDaemonHtmlTest, TheRefusalClassComesFromTheUpstream) {
  const DaemonHtmlHeaderLines out = {{"Content-Type", "text/html"},
                                     {"Cache-Control", "max-age=60"},
                                     {"X-Kept", "1"}};
  const DaemonHtmlHeaderLines upstream = {
      {"Content-Type", "text/html"}, {"Set-Cookie", "a=1"},
      {"Cache-Control", "private"},  {"vary", "Cookie"},
      {"Pragma", "no-cache"},        {"X-Hidden", "1"}};
  const DaemonHtmlHeaderLines origin =
      DaemonHtmlOriginLines(out, /*has_upstream=*/true, upstream);
  EXPECT_EQ((DaemonHtmlHeaderLines{{"Content-Type", "text/html"},
                                   {"X-Kept", "1"},
                                   {"Set-Cookie", "a=1"},
                                   {"Cache-Control", "private"},
                                   {"vary", "Cookie"},
                                   {"Pragma", "no-cache"}}),
            origin);
  EXPECT_EQ(out, DaemonHtmlOriginLines(out, /*has_upstream=*/false, upstream));
}

TEST_F(NgxDaemonHtmlTest, MarkdownLinesAreOwnedKeptOrRemoved) {
  for (const char* name : {"Content-Type", "content-length", "Cache-Control",
                           "Vary", "X-Robots-Tag", "Age", "ETag"}) {
    EXPECT_EQ(NgxMarkdownLine::kOwned, ClassifyMarkdownLine(name)) << name;
  }
  for (const char* name :
       {"Date", "Server", "Connection", "Keep-Alive", "Transfer-Encoding"}) {
    EXPECT_EQ(NgxMarkdownLine::kTransport, ClassifyMarkdownLine(name)) << name;
  }
  for (const char* name : {"Content-Encoding", "content-range"}) {
    EXPECT_EQ(NgxMarkdownLine::kConflict, ClassifyMarkdownLine(name)) << name;
  }
  for (const char* name :
       {"Expires", "Last-Modified", "Set-Cookie", "CDN-Cache-Control",
        "Surrogate-Control", "Strict-Transport-Security", "Content-Language",
        "Accept-Ranges", "Location", "X-Filter", "Link"}) {
    EXPECT_EQ(NgxMarkdownLine::kRemove, ClassifyMarkdownLine(name)) << name;
  }
}

TEST_F(NgxDaemonHtmlTest, TheAnswersLength) {
  DaemonHtmlServeResult ok;
  ok.headers = {{"Content-Type", "text/markdown; charset=utf-8"},
                {"Content-Length", "42"}};
  EXPECT_EQ(42, DaemonHtmlAnswerContentLength(ok));
  DaemonHtmlServeResult not_modified;
  not_modified.headers = {{"ETag", "W/\"psm-x\""}};
  EXPECT_EQ(-1, DaemonHtmlAnswerContentLength(not_modified));
  DaemonHtmlServeResult bad;
  bad.headers = {{"Content-Length", "4x"}};
  EXPECT_EQ(-1, DaemonHtmlAnswerContentLength(bad));
}

}  // namespace
}  // namespace net_instaweb
