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

// The serve path's posted notifications: the closure sends exactly one
// notification through the REAL shared functions (binding the stub daemon
// library, the same stand-in the serve arm's own suite binds), a cancelled
// closure sends nothing, and everything the send reads is a copy the
// closure owns.

#include "pagespeed/nginx/ngx_daemon_serve_notify.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <thread>

#include "net/instaweb/rewriter/public/option_context.h"
#include "net/instaweb/rewriter/public/rewrite_options.h"
#include "pagespeed/kernel/base/statistics.h"
#include "pagespeed/kernel/base/string.h"
#include "pagespeed/kernel/base/string_util.h"
#include "pagespeed/kernel/base/thread_system.h"
#include "pagespeed/kernel/thread/queued_worker_pool.h"
#include "pagespeed/kernel/util/platform.h"
#include "pagespeed/kernel/util/simple_stats.h"
#include "pagespeed/nginx/ngx_daemon_record_completion.h"
#include "pagespeed/system/daemon_abi.h"
#include "test/pagespeed/kernel/base/gtest.h"

namespace net_instaweb {
namespace {

class NgxDaemonServeNotifyTest : public testing::Test {
 public:
  NgxDaemonServeNotifyTest()
      : thread_system_(Platform::CreateThreadSystem()),
        stats_(thread_system_.get()) {
    RewriteOptions::Initialize();
  }
  ~NgxDaemonServeNotifyTest() override { RewriteOptions::Terminate(); }

 protected:
  void SetUp() override {
    ResetDaemonCompletionQueueForTesting();
    ps_daemon_record_completion_init_stats(&stats_);
    dropped_ = stats_.GetVariable(kIproDaemonNotifyDropped);
    // Two distinct counters for the send outcomes; their names are
    // test-local, their roles the production ones.
    stats_.AddVariable("test_notified");
    stats_.AddVariable("test_notify_failed");
    notified_ = stats_.GetVariable("test_notified");
    failed_ = stats_.GetVariable("test_notify_failed");
    log_path_ = StrCat(GTestTempDir(), "/daemon_notify_stub_calls.log");
    remove(log_path_.c_str());
    setenv("PS_STUB_LOG", log_path_.c_str(), 1);
    unsetenv("PS_STUB_NOTIFY_FAIL");

    GoogleString error;
    abi_.reset(LoadDaemonAbi(
        StrCat(GTestSrcDir(), "/test/pagespeed/system/libdaemon_stub.so"),
        &error));
    ASSERT_TRUE(abi_ != nullptr) << error;

    // A context pair the stub accepts (it recomputes the signature), the
    // same way the record arm renders it.
    RewriteOptions options(thread_system_.get());
    ASSERT_EQ(OptionContextStatus::kOk,
              OptionContext::Compute(options, &context_, &signature_));
  }

  void TearDown() override {
    unsetenv("PS_STUB_LOG");
    unsetenv("PS_STUB_NOTIFY_FAIL");
    ResetDaemonCompletionQueueForTesting();
  }

  // One request and one decision, shaped so the shared gates pass: an
  // optimized fallback serve, or a flagged age-expired fall-through.
  DaemonServeRequest MakeRequest() {
    DaemonServeRequest request;
    request.url = "/a/photo.png";
    request.hostname = "example.com";
    request.scheme = "http";
    return request;
  }
  DaemonServeDecision MakeFallbackDecision() {
    DaemonServeDecision decision;
    decision.verdict = DaemonServeVerdict::kServeOptimized;
    decision.fallback_hit = true;
    decision.ps_content_type = 3;  // image
    decision.client_mask = 0x09;
    return decision;
  }
  DaemonServeDecision MakeAgedDecision() {
    DaemonServeDecision decision;
    decision.verdict = DaemonServeVerdict::kFallThrough;
    decision.stale_variant_expired_by_age = true;
    decision.ps_content_type = 3;
    decision.client_mask = 0x08;
    return decision;
  }
  DaemonServeDecision MakeLostCopyDecision() {
    DaemonServeDecision decision;
    decision.verdict = DaemonServeVerdict::kServeOriginal;
    decision.lost_optimized_copy = true;
    decision.ps_content_type = 1;  // stylesheet
    decision.client_mask = 0x09;
    return decision;
  }

  // The reservation the production poster takes before the closure exists
  // (its destructor releases it; CallRun/CallCancel delete the closure).
  DaemonServeNotify* MakeNotify(DaemonServeNotifyKind kind,
                                const DaemonServeDecision& decision) {
    EXPECT_TRUE(TryQueueDaemonCompletion(0, dropped_));
    return new DaemonServeNotify(
        kind, abi_.get(), "/tmp/notify.sock", "/a/photo.png", "example.com",
        "http", decision, context_, signature_, notified_, failed_);
  }

  GoogleString StubLog() {
    GoogleString contents;
    FILE* f = fopen(log_path_.c_str(), "r");
    if (f == nullptr) {
      return "";
    }
    char buf[1024];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0) {
      contents.append(buf, n);
    }
    fclose(f);
    return contents;
  }

  std::unique_ptr<ThreadSystem> thread_system_;
  SimpleStats stats_;
  Variable* dropped_ = nullptr;
  Variable* notified_ = nullptr;
  Variable* failed_ = nullptr;
  GoogleString log_path_;
  GoogleString context_;
  GoogleString signature_;
  std::unique_ptr<DaemonAbi> abi_;
};

TEST_F(NgxDaemonServeNotifyTest, FallbackRenotifyRunsAndSendsOnce) {
  DaemonServeNotify* notify = MakeNotify(
      DaemonServeNotifyKind::kFallbackRenotify, MakeFallbackDecision());
  notify->CallRun();
  const GoogleString log = StubLog();
  EXPECT_NE(GoogleString::npos,
            log.find("notify socket=/tmp/notify.sock url=/a/photo.png "
                     "host=example.com scheme=http content_type=3 mask=9 "));
  EXPECT_EQ(1, notified_->Get());
  EXPECT_EQ(0, failed_->Get());
  // The reservation went with the closure: the whole items cap is free.
  for (int i = 0; i < kDaemonCompletionQueueMaxItems; ++i) {
    ASSERT_TRUE(TryQueueDaemonCompletion(0, dropped_)) << i;
  }
}

TEST_F(NgxDaemonServeNotifyTest, OriginRefreshedRunsAndSendsTheSentinel) {
  DaemonServeNotify* notify =
      MakeNotify(DaemonServeNotifyKind::kOriginRefreshed, MakeAgedDecision());
  notify->CallRun();
  const GoogleString log = StubLog();
  EXPECT_NE(GoogleString::npos, log.find("url=/a/photo.png"));
  // The sentinel mask, not a capability vector (daemon_serve_arm.h).
  EXPECT_NE(GoogleString::npos,
            log.find(StrCat(
                " mask=", static_cast<uint64>(kOriginRefreshedSentinel), " ")));
  EXPECT_EQ(1, notified_->Get());
  EXPECT_EQ(0, failed_->Get());
}

TEST_F(NgxDaemonServeNotifyTest, ALostCopyNotifyRunsAndSendsOnce) {
  DaemonServeNotify* notify =
      MakeNotify(DaemonServeNotifyKind::kLostCopy, MakeLostCopyDecision());
  notify->CallRun();
  const GoogleString log = StubLog();
  // An ordinary notification: the entry's class and the client's own mask.
  EXPECT_NE(GoogleString::npos,
            log.find("notify socket=/tmp/notify.sock url=/a/photo.png "
                     "host=example.com scheme=http content_type=1 mask=9 "));
  EXPECT_EQ(1, CountSubstring(log, "notify socket="));
  EXPECT_EQ(1, notified_->Get());
  EXPECT_EQ(0, failed_->Get());
  // The reservation went with the closure: the whole items cap is free.
  for (int i = 0; i < kDaemonCompletionQueueMaxItems; ++i) {
    ASSERT_TRUE(TryQueueDaemonCompletion(0, dropped_)) << i;
  }
}

TEST_F(NgxDaemonServeNotifyTest, NothingIsSentForADecisionThatDoesNotAskForIt) {
  // The closure carries the decision's own flag across the thread boundary:
  // a lost-copy closure built from a decision without it sends nothing, and
  // neither do the other two kinds when handed a lost-copy decision.
  DaemonServeDecision plain = MakeLostCopyDecision();
  plain.lost_optimized_copy = false;
  DaemonServeNotify* unflagged =
      MakeNotify(DaemonServeNotifyKind::kLostCopy, plain);
  unflagged->CallRun();
  DaemonServeNotify* as_fallback = MakeNotify(
      DaemonServeNotifyKind::kFallbackRenotify, MakeLostCopyDecision());
  as_fallback->CallRun();
  DaemonServeNotify* as_refresh = MakeNotify(
      DaemonServeNotifyKind::kOriginRefreshed, MakeLostCopyDecision());
  as_refresh->CallRun();
  EXPECT_EQ("", StubLog());
  EXPECT_EQ(0, notified_->Get());
  EXPECT_EQ(0, failed_->Get());
}

TEST_F(NgxDaemonServeNotifyTest, CancelledNotificationSendsNothing) {
  DaemonServeNotify* notify = MakeNotify(
      DaemonServeNotifyKind::kFallbackRenotify, MakeFallbackDecision());
  notify->CallCancel();
  EXPECT_EQ("", StubLog());
  EXPECT_EQ(0, notified_->Get());
  EXPECT_EQ(0, failed_->Get());
  for (int i = 0; i < kDaemonCompletionQueueMaxItems; ++i) {
    ASSERT_TRUE(TryQueueDaemonCompletion(0, dropped_)) << i;
  }
}

TEST_F(NgxDaemonServeNotifyTest, TheClosureOwnsItsStrings) {
  GoogleString url("/a/photo.png");
  GoogleString host("example.com");
  GoogleString scheme("http");
  GoogleString socket("/tmp/notify.sock");
  GoogleString context(context_);
  GoogleString signature(signature_);
  DaemonServeDecision decision = MakeFallbackDecision();
  EXPECT_TRUE(TryQueueDaemonCompletion(0, dropped_));
  DaemonServeNotify* notify = new DaemonServeNotify(
      DaemonServeNotifyKind::kFallbackRenotify, abi_.get(), socket, url, host,
      scheme, decision, context, signature, notified_, failed_);
  // Every source is destroyed or rewritten before the closure runs: the
  // send must carry the copies, untouched.
  url.assign("/tampered");
  host.assign("/tampered");
  scheme.assign("/tampered");
  socket.assign("/tampered");
  context.assign("/tampered");
  signature.assign("/tampered");
  decision.client_mask = 0x0A;
  notify->CallRun();
  const GoogleString log = StubLog();
  EXPECT_NE(GoogleString::npos,
            log.find("notify socket=/tmp/notify.sock url=/a/photo.png "
                     "host=example.com scheme=http"));
  EXPECT_NE(GoogleString::npos, log.find(" mask=9 "));
  EXPECT_EQ(GoogleString::npos, log.find("tampered"));
  EXPECT_EQ(1, notified_->Get());
}

TEST_F(NgxDaemonServeNotifyTest, ASuppressedDecisionMovesNeitherCounter) {
  // fallback_hit unset: the shared function's own gate suppresses the send.
  DaemonServeDecision decision = MakeFallbackDecision();
  decision.fallback_hit = false;
  DaemonServeNotify* notify =
      MakeNotify(DaemonServeNotifyKind::kFallbackRenotify, decision);
  notify->CallRun();
  EXPECT_EQ("", StubLog());
  EXPECT_EQ(0, notified_->Get());
  EXPECT_EQ(0, failed_->Get());
}

TEST_F(NgxDaemonServeNotifyTest, AFailedSendMovesOnlyTheFailureCounter) {
  setenv("PS_STUB_NOTIFY_FAIL", "1", 1);
  DaemonServeNotify* notify = MakeNotify(
      DaemonServeNotifyKind::kFallbackRenotify, MakeFallbackDecision());
  notify->CallRun();
  EXPECT_EQ(0, notified_->Get());
  EXPECT_EQ(1, failed_->Get());
}

TEST_F(NgxDaemonServeNotifyTest, NullSequenceDropsAndCallsNothingInline) {
  PostDaemonServeNotify(nullptr, DaemonServeNotifyKind::kFallbackRenotify,
                        abi_.get(), "/tmp/notify.sock", MakeRequest(),
                        MakeFallbackDecision(), context_, signature_, notified_,
                        failed_, dropped_);
  EXPECT_EQ("", StubLog());
  EXPECT_EQ(1, dropped_->Get());
  EXPECT_EQ(0, notified_->Get());
}

TEST_F(NgxDaemonServeNotifyTest, QueueCapsTurnTheClosureAway) {
  for (int i = 0; i < kDaemonCompletionQueueMaxItems; ++i) {
    ASSERT_TRUE(TryQueueDaemonCompletion(0, dropped_)) << i;
  }
  QueuedWorkerPool pool(1, "daemon_notify_test", thread_system_.get());
  QueuedWorkerPool::Sequence* sequence = pool.NewSequence();
  const int before = dropped_->Get();
  PostDaemonServeNotify(sequence, DaemonServeNotifyKind::kFallbackRenotify,
                        abi_.get(), "/tmp/notify.sock", MakeRequest(),
                        MakeFallbackDecision(), context_, signature_, notified_,
                        failed_, dropped_);
  EXPECT_EQ(before + 1, dropped_->Get());
  pool.ShutDown();
  EXPECT_EQ("", StubLog());
}

TEST_F(NgxDaemonServeNotifyTest, TheNotificationBudgetIsHalfTheItemCap) {
  // Half the item cap is the notification budget: at half-minus-one a
  // notification is accepted, at half it is refused and counted -- and a
  // record completion still fits up to the full cap, because the recording
  // always wins the upper half.  The numbers are written out here, not
  // taken from the constants under test: a test that loops to the constant
  // cannot notice the constant changing.
  static_assert(kDaemonCompletionQueueMaxItems == 64,
                "the item cap changed: revisit the notification budget");
  static_assert(kDaemonNotifyQueueMaxItems == 32,
                "the notification budget must stay half the item cap");
  for (int i = 0; i < 32; ++i) {
    ASSERT_TRUE(TryQueueDaemonNotification(dropped_)) << i;
  }
  const int before = dropped_->Get();
  EXPECT_FALSE(TryQueueDaemonNotification(dropped_))
      << "a notification past the half-cap budget must be refused";
  EXPECT_EQ(before + 1, dropped_->Get());
  for (int i = 32; i < 64; ++i) {
    ASSERT_TRUE(TryQueueDaemonCompletion(0, dropped_))
        << i << "a record completion must still fit up to the full cap";
  }
  EXPECT_FALSE(TryQueueDaemonCompletion(0, dropped_));
}

TEST_F(NgxDaemonServeNotifyTest, AVaryAcceptOriginIsSuppressedByTheSharedGate) {
  // vary_accept_origin_declared set: the shared function's own 0x04 gate
  // suppresses the send (a URL whose origin negotiates on Accept never
  // acquires a variant family, so a re-notify would be permanent traffic).
  // The port copies the field so the gate can fire; deleting the copy turns
  // this red.
  DaemonServeDecision decision = MakeFallbackDecision();
  decision.vary_accept_origin_declared = true;
  DaemonServeNotify* notify =
      MakeNotify(DaemonServeNotifyKind::kFallbackRenotify, decision);
  notify->CallRun();
  EXPECT_EQ("", StubLog());
  EXPECT_EQ(0, notified_->Get());
  EXPECT_EQ(0, failed_->Get());
}

TEST_F(NgxDaemonServeNotifyTest, PostedToARealSequenceItRuns) {
  QueuedWorkerPool pool(1, "daemon_notify_test", thread_system_.get());
  QueuedWorkerPool::Sequence* sequence = pool.NewSequence();
  PostDaemonServeNotify(sequence, DaemonServeNotifyKind::kOriginRefreshed,
                        abi_.get(), "/tmp/notify.sock", MakeRequest(),
                        MakeAgedDecision(), context_, signature_, notified_,
                        failed_, dropped_);
  // The one worker picks the closure up on its own thread; the pool is
  // shut down only after the send landed (ShutDown cancels, not drains).
  bool ran = false;
  for (int i = 0; i < 400 && !ran; ++i) {
    ran = StubLog().find("url=/a/photo.png") != GoogleString::npos;
    if (!ran) {
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
  }
  pool.ShutDown();
  EXPECT_TRUE(ran) << "the posted closure never ran on the sequence";
  EXPECT_EQ(1, notified_->Get());
  EXPECT_EQ(0, dropped_->Get());
}

TEST_F(NgxDaemonServeNotifyTest, PostedAfterShutdownIsCancelledNotSent) {
  QueuedWorkerPool pool(1, "daemon_notify_test", thread_system_.get());
  QueuedWorkerPool::Sequence* sequence = pool.NewSequence();
  pool.InitiateShutDown();
  PostDaemonServeNotify(sequence, DaemonServeNotifyKind::kFallbackRenotify,
                        abi_.get(), "/tmp/notify.sock", MakeRequest(),
                        MakeFallbackDecision(), context_, signature_, notified_,
                        failed_, dropped_);
  pool.WaitForShutDownComplete();
  EXPECT_EQ("", StubLog());
  EXPECT_EQ(0, notified_->Get());
}

}  // namespace
}  // namespace net_instaweb
