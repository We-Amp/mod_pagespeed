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

// Unit tests for the zero-copy aliased serve decision core
// (CycloneZeroCopyServe, the design record).  These encode the safety rules the IIS
// sink relies on; every rule here maps to a hazard class from the nginx
// default-on review (poll-not-barrier forced wrap, check-then-copy TOCTOU,
// fail-closed-when-copy-possible).

#include "pagespeed/iis/iis_zerocopy_serve.h"

#include <cstddef>
#include <cstdint>
#include <initializer_list>

#include "gtest/gtest.h"
#include "pagespeed/kernel/base/mapped_shared_string.h"

namespace net_instaweb {
namespace {

using Planner = IisZeroCopyServePlanner;
using Step = IisZeroCopyServePlanner::Step;

const size_t kChunk = Planner::kChunkBytes;
const size_t kTail = Planner::kTailCopyBytes;
const size_t kMinAlias = Planner::kMinAliasBytes;

TEST(IisZeroCopyServePlannerTest, HappyPathAliasesThenCopiesTail) {
  // 1MB body: aliased 128KB chunks until only the tail window remains,
  // which is always copied.
  const size_t kBody = 1024 * 1024;
  Planner p(kBody);
  size_t aliased = 0;
  int alias_steps = 0;
  while (true) {
    ASSERT_FALSE(p.done());
    Planner::Next n = p.PlanNext(LeaseRenewal::kOk, false);
    if (n.step == Step::kCopyTail) {
      EXPECT_EQ(p.offset(), n.offset);
      EXPECT_EQ(kBody - p.offset(), n.length);
      EXPECT_LE(n.length, kTail);
      p.Advance(n.length);
      break;
    }
    ASSERT_EQ(Step::kAliasChunk, n.step);
    EXPECT_EQ(p.offset(), n.offset);
    EXPECT_GE(n.length, size_t{1});
    EXPECT_LE(n.length, kChunk);
    // An aliased chunk never covers any of the final kTail bytes.
    EXPECT_LE(n.offset + n.length, kBody - kTail);
    aliased += n.length;
    alias_steps++;
    p.Advance(n.length);
  }
  EXPECT_TRUE(p.done());
  EXPECT_EQ(kBody - kTail, aliased);
  EXPECT_EQ(8, alias_steps);  // (1MB - 16KB) / 128KB rounds up to 8
}

TEST(IisZeroCopyServePlannerTest, BodiesBelowAliasThresholdCopyOutright) {
  // Aliasing must alias something worthwhile: below kTail + kMinAlias the
  // whole body is copied in one step -- never a degenerate tiny aliased
  // chunk plus a full async round-trip (e.g. the old 16KB+1 -> 1-byte
  // aliased chunk shape).
  for (size_t body : {size_t{1}, kTail / 2, kTail, kTail + 1, 2 * kTail - 1,
                      kTail + kMinAlias - 1}) {
    Planner p(body);
    Planner::Next n = p.PlanNext(LeaseRenewal::kOk, false);
    EXPECT_EQ(Step::kCopyTail, n.step) << body;
    EXPECT_EQ(size_t{0}, n.offset) << body;
    EXPECT_EQ(body, n.length) << body;
    p.Advance(n.length);
    EXPECT_TRUE(p.done()) << body;
  }
}

TEST(IisZeroCopyServePlannerTest, ThresholdBodyAliasesWorthwhileChunk) {
  // Exactly at the threshold: one kMinAlias-sized aliased chunk, then the
  // always-copied tail.
  Planner p(kTail + kMinAlias);
  Planner::Next n = p.PlanNext(LeaseRenewal::kOk, false);
  ASSERT_EQ(Step::kAliasChunk, n.step);
  EXPECT_EQ(kMinAlias, n.length);
  p.Advance(n.length);
  n = p.PlanNext(LeaseRenewal::kOk, false);
  EXPECT_EQ(Step::kCopyTail, n.step);
  EXPECT_EQ(kTail, n.length);
  p.Advance(n.length);
  EXPECT_TRUE(p.done());
}

TEST(IisZeroCopyServePlannerTest, TornEpochAbortsBeforeAnySubmit) {
  // kTorn at plan time: the remaining source bytes are unrecoverable; a
  // copy fallback would serve overwritten bytes.  Abort, never copy.
  Planner p(1024 * 1024);
  Planner::Next n = p.PlanNext(LeaseRenewal::kTorn, false);
  EXPECT_EQ(Step::kAbort, n.step);
  // Also mid-serve.
  p.Advance(kChunk);
  n = p.PlanNext(LeaseRenewal::kTorn, true);
  EXPECT_EQ(Step::kAbort, n.step);
}

TEST(IisZeroCopyServePlannerTest, WrapInFlightCopiesInsteadOfAliasing) {
  // kCopyNow: a wrap is in flight but the region is intact -- de-alias by
  // copying the remainder (MAJOR-3 discipline: degrade to copy, never fail
  // a request whose correct bytes can still be produced).
  Planner p(1024 * 1024);
  p.Advance(kChunk);
  Planner::Next n = p.PlanNext(LeaseRenewal::kCopyNow, false);
  EXPECT_EQ(Step::kCopyTail, n.step);
  EXPECT_EQ(kChunk, n.offset);
  EXPECT_EQ(1024 * 1024 - kChunk, n.length);
}

TEST(IisZeroCopyServePlannerTest, LeasesOffCopiesAndNeverAborts) {
  // kLeasesOff: no lease protection exists.  Aliasing would be unguarded
  // and aborting would turn every leases-off serve into a reset; the only
  // sound action is the copy.
  Planner p(1024 * 1024);
  Planner::Next n = p.PlanNext(LeaseRenewal::kLeasesOff, false);
  EXPECT_EQ(Step::kCopyTail, n.step);
  EXPECT_EQ(size_t{1024 * 1024}, n.length);
}

TEST(IisZeroCopyServePlannerTest, ForcedWrapPressureForcesCopyEvenWithLiveLease) {
  // BLOCKER-1 class (poll-not-barrier): a ceiling-forced wrap ignores
  // leases, and the aliased in-flight window is client-paced (unbounded),
  // so ANY finite force-wrap deadline must force the copy path -- a live
  // lease (kOk) is NOT sufficient.
  Planner p(1024 * 1024);
  Planner::Next n = p.PlanNext(LeaseRenewal::kOk, true /* pressure */);
  EXPECT_EQ(Step::kCopyTail, n.step);
  EXPECT_EQ(size_t{1024 * 1024}, n.length);
}

TEST(IisZeroCopyServePlannerTest, PressureMidServeCopiesRemainderOnly) {
  Planner p(3 * kChunk + kTail);
  Planner::Next n = p.PlanNext(LeaseRenewal::kOk, false);
  ASSERT_EQ(Step::kAliasChunk, n.step);
  p.Advance(n.length);
  n = p.PlanNext(LeaseRenewal::kOk, true);
  EXPECT_EQ(Step::kCopyTail, n.step);
  EXPECT_EQ(kChunk, n.offset);
  EXPECT_EQ(2 * kChunk + kTail, n.length);
  p.Advance(n.length);
  EXPECT_TRUE(p.done());
}

TEST(IisZeroCopyServePlannerTest, PostExposureVerdict) {
  // Verify-after-exposure (BLOCKER-2 class, check-then-copy TOCTOU): only
  // an epoch move (kTorn) invalidates bytes the kernel already read.
  // kCopyNow / kLeasesOff at completion time do NOT: the epoch check
  // proves the region was byte-stable through the whole in-flight window.
  EXPECT_FALSE(Planner::SentBytesTrustworthy(LeaseRenewal::kTorn));
  EXPECT_TRUE(Planner::SentBytesTrustworthy(LeaseRenewal::kOk));
  EXPECT_TRUE(Planner::SentBytesTrustworthy(LeaseRenewal::kCopyNow));
  EXPECT_TRUE(Planner::SentBytesTrustworthy(LeaseRenewal::kLeasesOff));
}

TEST(IisZeroCopyServePlannerTest, ChunkLengthArithmeticNeverZeroNeverPastTail) {
  // Sweep bodies around the boundary sizes; every aliased plan must make
  // progress and stay clear of the tail window; totals must add up.
  for (size_t body :
       {kTail + 1, kTail + 2, 2 * kTail - 1, 2 * kTail, kChunk,
        kChunk + kTail, kChunk + kTail + 1, 2 * kChunk + kTail - 1,
        5 * kChunk + 3}) {
    Planner p(body);
    size_t total = 0;
    while (!p.done()) {
      Planner::Next n = p.PlanNext(LeaseRenewal::kOk, false);
      ASSERT_GE(n.length, size_t{1}) << body;
      if (n.step == Step::kAliasChunk) {
        ASSERT_LE(n.offset + n.length, body - kTail) << body;
        ASSERT_GE(n.length, kMinAlias) << body;  // worthwhile chunks only
      } else {
        ASSERT_EQ(Step::kCopyTail, n.step) << body;
        ASSERT_EQ(body - n.offset, n.length) << body;
      }
      total += n.length;
      p.Advance(n.length);
    }
    EXPECT_EQ(body, total) << body;
  }
}

}  // namespace
}  // namespace net_instaweb
