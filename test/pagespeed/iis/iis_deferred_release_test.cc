// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

// The release queue that retires a replaced site state: its tasks run on a
// thread of their own, exactly once, and a task may post another task. The
// queue is header-only and free of IIS headers, so it builds on any platform,
// but today it runs only in the Windows lane.

#include "pagespeed/iis/iis_deferred_release.h"

#include <atomic>
#include <thread>
#include <vector>

#include "test/pagespeed/kernel/base/gtest.h"

namespace net_instaweb {
namespace {

TEST(DeferredReleaseQueueTest, TaskRunsOnADifferentThreadThanThePoster) {
  DeferredReleaseQueue queue;
  std::thread::id ran_on;
  std::thread::id poster;
  std::thread t([&] {
    poster = std::this_thread::get_id();
    queue.Post([&] { ran_on = std::this_thread::get_id(); });
  });
  t.join();
  queue.StopAndDrain();
  EXPECT_NE(std::thread::id(), ran_on);
  EXPECT_NE(poster, ran_on);
  EXPECT_NE(std::this_thread::get_id(), ran_on);
}

TEST(DeferredReleaseQueueTest, EveryPostedTaskRunsExactlyOnce) {
  const int kThreads = 8;
  const int kPerThread = 125;  // 1000 in all
  std::vector<std::atomic<int>> counts(kThreads * kPerThread);
  for (auto& c : counts) c = 0;
  {
    DeferredReleaseQueue queue;
    std::vector<std::thread> posters;
    for (int t = 0; t < kThreads; ++t) {
      posters.emplace_back([&, t] {
        for (int i = 0; i < kPerThread; ++i) {
          int slot = t * kPerThread + i;
          queue.Post([&counts, slot] { ++counts[slot]; });
        }
      });
    }
    for (auto& p : posters) p.join();
    queue.StopAndDrain();
  }
  for (const auto& c : counts) EXPECT_EQ(1, c.load());
}

TEST(DeferredReleaseQueueTest, ATaskMayPostAnotherAndBothRunBeforeDrainEnds) {
  DeferredReleaseQueue queue;
  std::atomic<int> ran(0);
  queue.Post([&] {
    ++ran;
    queue.Post([&] { ++ran; });
  });
  queue.StopAndDrain();
  EXPECT_EQ(2, ran.load());
}

TEST(DeferredReleaseQueueTest, StopIsIdempotentAndLaterPostsRunInline) {
  DeferredReleaseQueue queue;
  std::atomic<int> ran(0);
  queue.Post([&] { ++ran; });
  queue.StopAndDrain();
  queue.StopAndDrain();
  EXPECT_EQ(1, ran.load());

  std::thread::id ran_on;
  queue.Post([&] { ran_on = std::this_thread::get_id(); });
  EXPECT_EQ(std::this_thread::get_id(), ran_on);
}

TEST(DeferredReleaseQueueTest, AQueueThatNeverSawAPostStopsCleanly) {
  DeferredReleaseQueue queue;
  queue.StopAndDrain();
  queue.StopAndDrain();
}

}  // namespace
}  // namespace net_instaweb
