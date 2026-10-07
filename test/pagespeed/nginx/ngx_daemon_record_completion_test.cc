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

#include "pagespeed/nginx/ngx_daemon_record_completion.h"

#include <cstdint>
#include <memory>

#include "pagespeed/kernel/base/basictypes.h"
#include "pagespeed/kernel/base/statistics.h"
#include "pagespeed/kernel/base/string.h"
#include "pagespeed/kernel/base/thread_system.h"
#include "pagespeed/kernel/http/http_options.h"
#include "pagespeed/kernel/http/response_headers.h"
#include "pagespeed/kernel/thread/queued_worker_pool.h"
#include "pagespeed/kernel/util/platform.h"
#include "pagespeed/kernel/util/simple_stats.h"
#include "pagespeed/system/daemon_adapter.h"
#include "pagespeed/system/ipro_recorder.h"
#include "test/pagespeed/kernel/base/gtest.h"

namespace net_instaweb {
namespace {

// The IproRecorder contract makes DoneAndSetHeaders delete the recorder, so
// what one call carried is kept in statics -- the same reason the shipped
// recorder reports through an outcome sink.
class FakeRecorder : public IproRecorder {
 public:
  static void Reset() {
    calls = 0;
    last_headers = nullptr;
    last_complete = false;
  }

  bool Write(const StringPiece& contents, MessageHandler* handler) override {
    return true;
  }
  bool Flush(MessageHandler* handler) override { return true; }
  void ConsiderResponseHeaders(HeadersKind headers_kind,
                               ResponseHeaders* response_headers) override {}
  void SaveCacheControl(const char* cache_control) override {}
  void Fail() override {}
  bool failed() const override { return false; }
  MessageHandler* handler() override { return nullptr; }
  const HttpOptions& http_options() const override { return http_options_; }
  const GoogleString& url() const override { return url_; }

  void DoneAndSetHeaders(ResponseHeaders* response_headers,
                         bool entire_response_received) override {
    ++calls;
    last_headers = response_headers;
    last_complete = entire_response_received;
    delete this;
  }

  static int calls;
  static ResponseHeaders* last_headers;
  static bool last_complete;

 private:
  HttpOptions http_options_;
  GoogleString url_;
};

int FakeRecorder::calls = 0;
ResponseHeaders* FakeRecorder::last_headers = nullptr;
bool FakeRecorder::last_complete = false;

class NgxDaemonRecordCompletionTest : public testing::Test {
 protected:
  NgxDaemonRecordCompletionTest()
      : thread_system_(Platform::CreateThreadSystem()),
        stats_(thread_system_.get()) {}

  void SetUp() override {
    ResetDaemonCompletionQueueForTesting();
    // The production call site is the factory's InitStats; the counter is
    // then looked up from the statistics object at hand, never cached
    // process-wide.
    ps_daemon_record_completion_init_stats(&stats_);
    dropped_ = stats_.GetVariable(kIproDaemonRecordDropped);
    FakeRecorder::Reset();
  }

  void TearDown() override {
    ResetDaemonCompletionQueueForTesting();
    // The latch test installs a clock capturing a stack local; leaving it
    // installed would hand every later test a dangling reference.
    SetDaemonNowMsForTesting(nullptr);
  }

  std::unique_ptr<ThreadSystem> thread_system_;
  SimpleStats stats_;
  Variable* dropped_ = nullptr;
};

TEST_F(NgxDaemonRecordCompletionTest, RunCompletesRecorderAndReleasesRoom) {
  ASSERT_TRUE(TryQueueDaemonCompletion(100, dropped_));
  ResponseHeaders* headers = new ResponseHeaders();
  // CallRun is what the pool calls, and it deletes the closure: the
  // destructor releasing the queue reservation is part of what is tested
  // here.
  DaemonRecordCompletion* completion =
      new DaemonRecordCompletion(new FakeRecorder(), headers, true, 100);
  completion->CallRun();
  EXPECT_EQ(1, FakeRecorder::calls);
  EXPECT_EQ(headers, FakeRecorder::last_headers);
  EXPECT_TRUE(FakeRecorder::last_complete);

  // The reservation went with the closure: the full byte cap is available
  // again.
  EXPECT_TRUE(
      TryQueueDaemonCompletion(kDaemonCompletionQueueMaxBytes, dropped_));
}

TEST_F(NgxDaemonRecordCompletionTest, CancelFinishesIncompleteAndReleases) {
  ASSERT_TRUE(TryQueueDaemonCompletion(50, dropped_));
  DaemonRecordCompletion* completion = new DaemonRecordCompletion(
      new FakeRecorder(), new ResponseHeaders(), true, 50);
  completion->CallCancel();
  EXPECT_EQ(1, FakeRecorder::calls);
  EXPECT_EQ(nullptr, FakeRecorder::last_headers);
  EXPECT_FALSE(FakeRecorder::last_complete);

  EXPECT_TRUE(
      TryQueueDaemonCompletion(kDaemonCompletionQueueMaxBytes, dropped_));
}

TEST_F(NgxDaemonRecordCompletionTest, ByteCapTurnsAwayWhatDoesNotFit) {
  EXPECT_TRUE(
      TryQueueDaemonCompletion(kDaemonCompletionQueueMaxBytes, dropped_));
  EXPECT_EQ(0, dropped_->Get());
  EXPECT_FALSE(TryQueueDaemonCompletion(1, dropped_));
  EXPECT_EQ(1, dropped_->Get());

  DaemonCompletionExited(kDaemonCompletionQueueMaxBytes);
  EXPECT_TRUE(TryQueueDaemonCompletion(1, dropped_));
}

TEST_F(NgxDaemonRecordCompletionTest, ItemCapTurnsAwayWhatDoesNotFit) {
  for (int i = 0; i < kDaemonCompletionQueueMaxItems; ++i) {
    ASSERT_TRUE(TryQueueDaemonCompletion(0, dropped_)) << i;
  }
  EXPECT_EQ(0, dropped_->Get());
  EXPECT_FALSE(TryQueueDaemonCompletion(0, dropped_));
  EXPECT_EQ(1, dropped_->Get());

  DaemonCompletionExited(0);
  EXPECT_TRUE(TryQueueDaemonCompletion(0, dropped_));
}

TEST_F(NgxDaemonRecordCompletionTest, OversizedSingleBodyDroppedInline) {
  EXPECT_FALSE(
      TryQueueDaemonCompletion(kDaemonCompletionQueueMaxBytes + 1, dropped_));
  EXPECT_EQ(1, dropped_->Get());
  // Nothing was ever reserved, so the whole cap is still there.
  EXPECT_TRUE(
      TryQueueDaemonCompletion(kDaemonCompletionQueueMaxBytes, dropped_));
}

TEST_F(NgxDaemonRecordCompletionTest, RollbackLeavesNoPartialReservation) {
  // Two reservations racing the caps can transiently overshoot; the loser
  // must hand its bytes and item back.  Drive the same shape serially:
  // fill to the brim, fail one, then confirm exactly the cap remains.
  EXPECT_TRUE(
      TryQueueDaemonCompletion(kDaemonCompletionQueueMaxBytes - 10, dropped_));
  EXPECT_TRUE(TryQueueDaemonCompletion(10, dropped_));
  EXPECT_FALSE(TryQueueDaemonCompletion(1, dropped_));
  DaemonCompletionExited(kDaemonCompletionQueueMaxBytes - 10);
  DaemonCompletionExited(10);
  EXPECT_TRUE(
      TryQueueDaemonCompletion(kDaemonCompletionQueueMaxBytes, dropped_));
}

TEST_F(NgxDaemonRecordCompletionTest, OpenAttemptLatch) {
  int64 now = 1000000;
  SetDaemonNowMsForTesting([&now] { return now; });
  EXPECT_TRUE(ShouldPostDaemonOpenAttempt());
  EXPECT_FALSE(ShouldPostDaemonOpenAttempt());
  now += kDaemonOpenAttemptIntervalMs - 1;
  EXPECT_FALSE(ShouldPostDaemonOpenAttempt());
  now += 1;
  EXPECT_TRUE(ShouldPostDaemonOpenAttempt());
  EXPECT_FALSE(ShouldPostDaemonOpenAttempt());
}

TEST_F(NgxDaemonRecordCompletionTest, NullVariableAccepted) {
  ps_daemon_record_completion_init_stats(nullptr);
  // A missing counter is never an error: queueing and drops work the same,
  // just uncounted.
  EXPECT_TRUE(TryQueueDaemonCompletion(1, nullptr));
  EXPECT_FALSE(
      TryQueueDaemonCompletion(kDaemonCompletionQueueMaxBytes + 1, nullptr));
}

TEST_F(NgxDaemonRecordCompletionTest, PostedOpenAttemptRuns) {
  // RecordCache() on an unresolved adapter is a no-op by design -- the
  // health guard returns before any open, announce or latch -- which is
  // what makes the poster executable in a unit test without a daemon.  The
  // assertions are thin for the same reason: what matters is that Run
  // reaches the adapter and returns, which the pool's shutdown waits out.
  DaemonAdapter adapter("", "", nullptr);
  QueuedWorkerPool pool(1, "daemon_record_test", thread_system_.get());
  QueuedWorkerPool::Sequence* sequence = pool.NewSequence();
  PostDaemonOpenAttempt(sequence, &adapter);
  pool.ShutDown();
  EXPECT_EQ(nullptr, adapter.RecordCacheIfOpen());
}

TEST_F(NgxDaemonRecordCompletionTest, PostedOpenAttemptCancelledAtShutdown) {
  // The shutdown path the poster exists to survive: a sequence whose
  // shutdown has begun cancels a later Add immediately instead of running
  // it.  The sequence has to exist first: NewSequence() returns nullptr
  // once the pool itself is in shutdown.
  DaemonAdapter adapter("", "", nullptr);
  QueuedWorkerPool pool(1, "daemon_record_test", thread_system_.get());
  QueuedWorkerPool::Sequence* sequence = pool.NewSequence();
  pool.InitiateShutDown();
  PostDaemonOpenAttempt(sequence, &adapter);
  pool.WaitForShutDownComplete();
  EXPECT_EQ(nullptr, adapter.RecordCacheIfOpen());
}

}  // namespace
}  // namespace net_instaweb
