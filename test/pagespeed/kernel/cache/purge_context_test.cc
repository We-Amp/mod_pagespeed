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

#include "pagespeed/kernel/cache/purge_context.h"

#include <memory>

#include "pagespeed/kernel/base/basictypes.h"
#include "pagespeed/kernel/base/named_lock_manager.h"
#include "pagespeed/kernel/base/null_statistics.h"
#include "pagespeed/kernel/base/statistics.h"
#include "pagespeed/kernel/base/thread_system.h"
#include "pagespeed/kernel/util/threadsafe_lock_manager.h"
#include "pagespeed/kernel/util/platform.h"
#include "pagespeed/kernel/util/simple_stats.h"
#include "test/pagespeed/kernel/base/gtest.h"
#include "test/pagespeed/kernel/base/mem_file_system.h"
#include "test/pagespeed/kernel/base/mock_message_handler.h"
#include "test/pagespeed/kernel/base/mock_timer.h"
#include "test/pagespeed/kernel/thread/mock_scheduler.h"

namespace {

const int kMaxBytes = 100;
const char kPurgeFile[] = "/cache/cache.flush";

}  // namespace

namespace net_instaweb {

class PurgeContextTest : public ::testing::Test,
                         public ::testing::WithParamInterface<bool> {
 public:
  bool PollAndTest(const GoogleString& url, int64 now_ms,
                   const CopyOnWrite<PurgeSet>& purge_set,
                   PurgeContext* purge_context) {
    purge_context->PollFileSystem();
    return purge_set->IsValid(url, now_ms);
  }

  bool PollAndTest1(const GoogleString& url, int64 now_ms) {
    return PollAndTest(url, now_ms, purge_set1_, purge_context1_.get());
  }

  bool PollAndTest2(const GoogleString& url, int64 now_ms) {
    return PollAndTest(url, now_ms, purge_set2_, purge_context2_.get());
  }

 protected:
  PurgeContextTest()
      : thread_system_(Platform::CreateThreadSystem()),
        timer_(thread_system_->NewMutex(), MockTimer::kApr_5_2010_ms),
        message_handler_(thread_system_->NewMutex()),
        file_system_(thread_system_.get(), &timer_),
        scheduler_(thread_system_.get(), &timer_),
        lock_manager_(&scheduler_) {
    if (HasValidStats()) {
      statistics_ = std::make_unique<SimpleStats>(thread_system_.get());
    } else {
      statistics_ = std::make_unique<NullStatistics>();
    }
    PurgeContext::InitStats(statistics_.get());
    purge_context1_.reset(MakePurgeContext());
    purge_context2_.reset(MakePurgeContext());

    purge_context1_->SetUpdateCallback(
        NewPermanentCallback(this, &PurgeContextTest::UpdatePurgeSet1));
    purge_context2_->SetUpdateCallback(
        NewPermanentCallback(this, &PurgeContextTest::UpdatePurgeSet2));

    message_handler_.AddPatternToSkipPrinting("*opening input file*");
  }

  bool HasValidStats() const { return GetParam(); }

  PurgeContext* MakePurgeContext() {
    return new PurgeContext(kPurgeFile, &file_system_, &timer_, kMaxBytes,
                            thread_system_.get(), &lock_manager_, &scheduler_,
                            statistics_.get(), &message_handler_);
  }

  void ExpectSuccessHelper(bool x, StringPiece reason) { EXPECT_TRUE(x); }
  PurgeContext::PurgeCallback* ExpectSuccess() {
    return NewCallback(this, &PurgeContextTest::ExpectSuccessHelper);
  }

  int file_parse_failures() {
    return statistics_->GetVariable(PurgeContext::kFileParseFailures)->Get();
  }

  int num_file_stats() {
    return statistics_->GetVariable(PurgeContext::kFileStats)->Get();
  }

  int ExpectStat(int expected_value) const {
    return HasValidStats() ? expected_value : 0;
  }

  int file_writes() {
    return statistics_->GetVariable(PurgeContext::kFileWrites)->Get();
  }

  void UpdatePurgeSet1(const CopyOnWrite<PurgeSet>& purge_set) {
    purge_set1_ = purge_set;
  }

  void UpdatePurgeSet2(const CopyOnWrite<PurgeSet>& purge_set) {
    purge_set2_ = purge_set;
  }

  std::unique_ptr<ThreadSystem> thread_system_;
  MockTimer timer_;
  MockMessageHandler message_handler_;
  MemFileSystem file_system_;
  MockScheduler scheduler_;
  ThreadSafeLockManager lock_manager_;
  std::unique_ptr<Statistics> statistics_;
  std::unique_ptr<PurgeContext> purge_context1_;
  std::unique_ptr<PurgeContext> purge_context2_;
  CopyOnWrite<PurgeSet> purge_set1_;
  CopyOnWrite<PurgeSet> purge_set2_;
};

TEST_P(PurgeContextTest, Empty) { EXPECT_TRUE(PollAndTest1("a", 500)); }

TEST_P(PurgeContextTest, InvalidationSharing) {
  // Set up a write-delay on purge_context1_, but let purge_context2_ have
  // immediate writes.
  purge_context1_->set_request_batching_delay_ms(1000);

  scheduler_.AdvanceTimeMs(1000);
  purge_context1_->SetCachePurgeGlobalTimestampMs(400000, ExpectSuccess());
  purge_context1_->AddPurgeUrl("a", 500000, ExpectSuccess());
  EXPECT_EQ(0, file_writes());
  EXPECT_EQ(0, num_file_stats());

  // Prior to waiting for the new purge requests to be written, the purges
  // will not take effect.
  EXPECT_TRUE(PollAndTest1("a", 500000));
  EXPECT_TRUE(PollAndTest1("b", 399999));

  // Wait a second for the write-timer to fire, then both purges will be
  // written together in one file-write.
  scheduler_.AdvanceTimeMs(1000);
  EXPECT_EQ(ExpectStat(1), file_writes());
  EXPECT_EQ(ExpectStat(2), num_file_stats());

  if (!HasValidStats()) {
    scheduler_.AdvanceTimeMs(6000);
  }

  EXPECT_FALSE(PollAndTest1("a", 500000));
  EXPECT_TRUE(PollAndTest1("a", 500001));
  EXPECT_FALSE(PollAndTest1("b", 399999));
  EXPECT_FALSE(PollAndTest1("b", 400000));
  EXPECT_TRUE(PollAndTest1("b", 400001));

  // These will get transmitted to purge_context2_, which has not
  // yet read the cache invalidation file, but will pick up the
  // changes from the file system.
  EXPECT_FALSE(PollAndTest2("a", 500000));
  EXPECT_TRUE(PollAndTest2("a", 500001));
  EXPECT_FALSE(PollAndTest2("b", 399999));
  EXPECT_FALSE(PollAndTest2("b", 400000));
  EXPECT_TRUE(PollAndTest2("b", 400001));

  EXPECT_EQ(ExpectStat(4), num_file_stats());

  // Now push a time-based flush the other direction.  Because
  // we only poll the file system periodically we do have to advance
  // time.
  purge_context2_->SetCachePurgeGlobalTimestampMs(600000, ExpectSuccess());

  if (!HasValidStats()) {
    scheduler_.AdvanceTimeMs(6000);
  }

  // This will have immediate effect because purge_context2_ has no write-delay.
  EXPECT_FALSE(PollAndTest2("a", 500001));

  // There will also be no delay for purge_context1 because purge_context2_
  // found a new version of the purge file, it updated shared stat "purge_index"
  // which is cheaply checked in every context on every poll.
  EXPECT_FALSE(PollAndTest1("a", 500001));
  scheduler_.AdvanceTimeMs(10 * Timer::kSecondMs);  // force poll
  EXPECT_FALSE(PollAndTest1("a", 500001));
  EXPECT_TRUE(PollAndTest1("b", 600001));
  EXPECT_FALSE(PollAndTest2("a", 500001));
  EXPECT_TRUE(PollAndTest2("b", 600001));

  // Now invalidate 'b' till 700k.
  purge_context2_->AddPurgeUrl("b", 700000, ExpectSuccess());
  scheduler_.AdvanceTimeMs(HasValidStats() ? 1000 : 6000);
  EXPECT_FALSE(PollAndTest2("b", 700000));

  // Again, this new value is immediately reflected in purge_context1.
  EXPECT_FALSE(PollAndTest1("b", 700000));
  scheduler_.AdvanceTimeMs(10 * Timer::kSecondMs);  // force poll
  EXPECT_FALSE(PollAndTest1("b", 700000));
  EXPECT_TRUE(PollAndTest1("b", 700001));
  EXPECT_FALSE(PollAndTest2("b", 700000));
  EXPECT_TRUE(PollAndTest2("b", 700001));
  EXPECT_EQ(0, file_parse_failures());
}

TEST_P(PurgeContextTest, EmptyPurgeFile) {
  // The currently documented mechanism to flush the entire cache is
  // to simply touch CACHE_DIR/cache.flush.  This mode of operation
  // requires disabling purging in the context.
  purge_context1_->set_enable_purge(false);
  scheduler_.AdvanceTimeMs(10 * Timer::kSecondMs);
  ASSERT_TRUE(file_system_.WriteFile(kPurgeFile, "", &message_handler_));
  EXPECT_FALSE(PollAndTest1("b", timer_.NowMs() - 1));
  EXPECT_TRUE(PollAndTest1("b", timer_.NowMs() + 1));
  EXPECT_EQ(0, file_parse_failures());
}

// NOTE: LockContentionFailure, LockContentionSuccess, FileWriteConflict, and
// FileWriteConflictWithInterveningUpdate tests were removed when
// FileSystemLockManager was replaced by ThreadSafeLockManager. These tests
// tested file-based lock stealing behavior that doesn't apply to in-memory
// locking.

TEST_P(PurgeContextTest, InvalidTimestampInPurgeRecord) {
  ASSERT_TRUE(
      file_system_.WriteFile(kPurgeFile,
                             "-1\n"               // Valid initial timestamp
                             "x\n"                // not enough tokens
                             "2000000000000 y\n"  // timestamp(ms) in far future
                             "-2 z\n"             // timestamp(ms) in far past
                             "500 a\n",  // valid record should be parsed.
                             &message_handler_));
  EXPECT_FALSE(PollAndTest1("a", 500));
  EXPECT_EQ(ExpectStat(3), file_parse_failures());
  EXPECT_TRUE(PollAndTest1("a", 501));
  EXPECT_EQ(ExpectStat(6), file_parse_failures());
}

// We test with use_null_statistics == GetParam() as both true and false.
INSTANTIATE_TEST_SUITE_P(PurgeContextTestInstance, PurgeContextTest,
                         ::testing::Bool());

}  // namespace net_instaweb
