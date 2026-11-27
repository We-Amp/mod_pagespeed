// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

// PageSpeed 2.0 - Analysis Queue Tests

#include "src/browser/analysis_queue.h"

#include <atomic>
#include <cstdint>
#include <string>
#include <thread>
#include <vector>

#include "gtest/gtest.h"

namespace pagespeed {
namespace {

AnalysisQueue::Item MakeItem(uint64_t hash, bool warmup = false,
                             uint32_t freq = 1) {
  AnalysisQueue::Item item;
  item.url = "https://example.com/" + std::to_string(hash);
  item.hostname = "example.com";
  item.template_hash = hash;
  item.is_warmup = warmup;
  item.frequency = freq;
  return item;
}

TEST(AnalysisQueueTest, EnqueueAndDequeue) {
  AnalysisQueue queue(100);
  EXPECT_TRUE(queue.Enqueue(MakeItem(1)));
  EXPECT_EQ(queue.size(), 1u);

  auto item = queue.Dequeue();
  ASSERT_TRUE(item.has_value());
  EXPECT_EQ(item->template_hash, 1u);
  EXPECT_EQ(queue.size(), 0u);
}

TEST(AnalysisQueueTest, EmptyDequeueReturnsNullopt) {
  AnalysisQueue queue;
  auto item = queue.Dequeue();
  EXPECT_FALSE(item.has_value());
}

TEST(AnalysisQueueTest, DeduplicatesByTemplateHash) {
  AnalysisQueue queue;
  EXPECT_TRUE(queue.Enqueue(MakeItem(42)));
  EXPECT_FALSE(queue.Enqueue(MakeItem(42)));
  EXPECT_EQ(queue.size(), 1u);
}

TEST(AnalysisQueueTest, DedupClearedAfterDequeue) {
  AnalysisQueue queue;
  EXPECT_TRUE(queue.Enqueue(MakeItem(42)));
  queue.Dequeue();
  EXPECT_TRUE(queue.Enqueue(MakeItem(42)));
  EXPECT_EQ(queue.size(), 1u);
}

TEST(AnalysisQueueTest, WarmupItemsFirst) {
  AnalysisQueue queue;
  EXPECT_TRUE(queue.Enqueue(MakeItem(1, false, 10)));
  EXPECT_TRUE(queue.Enqueue(MakeItem(2, true, 1)));
  EXPECT_TRUE(queue.Enqueue(MakeItem(3, false, 20)));

  auto item1 = queue.Dequeue();
  ASSERT_TRUE(item1.has_value());
  EXPECT_TRUE(item1->is_warmup);
  EXPECT_EQ(item1->template_hash, 2u);
}

TEST(AnalysisQueueTest, HigherFrequencyFirst) {
  AnalysisQueue queue;
  EXPECT_TRUE(queue.Enqueue(MakeItem(1, false, 5)));
  EXPECT_TRUE(queue.Enqueue(MakeItem(2, false, 20)));
  EXPECT_TRUE(queue.Enqueue(MakeItem(3, false, 10)));

  auto item1 = queue.Dequeue();
  EXPECT_EQ(item1->frequency, 20u);
  auto item2 = queue.Dequeue();
  EXPECT_EQ(item2->frequency, 10u);
  auto item3 = queue.Dequeue();
  EXPECT_EQ(item3->frequency, 5u);
}

TEST(AnalysisQueueTest, EvictsLowestPriorityWhenFull) {
  AnalysisQueue queue(3);
  EXPECT_TRUE(queue.Enqueue(MakeItem(1, false, 10)));
  EXPECT_TRUE(queue.Enqueue(MakeItem(2, false, 20)));
  EXPECT_TRUE(queue.Enqueue(MakeItem(3, false, 5)));
  EXPECT_EQ(queue.size(), 3u);

  // Add higher priority item — should evict hash=3 (freq=5).
  EXPECT_TRUE(queue.Enqueue(MakeItem(4, false, 15)));
  EXPECT_EQ(queue.size(), 3u);
  EXPECT_EQ(queue.drops(), 1u);

  // Verify hash=3 was evicted.
  std::vector<uint64_t> hashes;
  while (auto item = queue.Dequeue()) {
    hashes.push_back(item->template_hash);
  }
  EXPECT_EQ(hashes.size(), 3u);
  for (auto h : hashes) {
    EXPECT_NE(h, 3u);
  }
}

TEST(AnalysisQueueTest, DropsLowPriorityWhenFull) {
  AnalysisQueue queue(2);
  EXPECT_TRUE(queue.Enqueue(MakeItem(1, false, 10)));
  EXPECT_TRUE(queue.Enqueue(MakeItem(2, false, 20)));

  // Lower priority than both — dropped.
  EXPECT_FALSE(queue.Enqueue(MakeItem(3, false, 5)));
  EXPECT_EQ(queue.size(), 2u);
  EXPECT_EQ(queue.drops(), 1u);
}

TEST(AnalysisQueueTest, ShutdownPreventsEnqueue) {
  AnalysisQueue queue;
  EXPECT_TRUE(queue.Enqueue(MakeItem(1)));
  queue.Shutdown();
  EXPECT_TRUE(queue.is_shutdown());
  EXPECT_FALSE(queue.Enqueue(MakeItem(2)));
  EXPECT_EQ(queue.size(), 1u);
}

TEST(AnalysisQueueTest, ShutdownAllowsDequeue) {
  AnalysisQueue queue;
  EXPECT_TRUE(queue.Enqueue(MakeItem(1)));
  queue.Shutdown();

  auto item = queue.Dequeue();
  ASSERT_TRUE(item.has_value());
  EXPECT_EQ(item->template_hash, 1u);
}

TEST(AnalysisQueueTest, DropsCounter) {
  AnalysisQueue queue(1);
  EXPECT_TRUE(queue.Enqueue(MakeItem(1, false, 10)));
  EXPECT_FALSE(queue.Enqueue(MakeItem(2, false, 5)));
  EXPECT_FALSE(queue.Enqueue(MakeItem(3, false, 3)));
  EXPECT_EQ(queue.drops(), 2u);
}

TEST(AnalysisQueueTest, ConcurrentAccess) {
  AnalysisQueue queue(500);
  constexpr int kThreads = 4;
  constexpr int kOpsPerThread = 100;

  std::vector<std::thread> threads;
  threads.reserve(kThreads);
  for (int t = 0; t < kThreads; ++t) {
    threads.emplace_back([&queue, t]() {
      for (int i = 0; i < kOpsPerThread; ++i) {
        auto hash = static_cast<uint64_t>(t) * kOpsPerThread + i;
        queue.Enqueue(MakeItem(hash, i % 3 == 0, i + 1));
      }
    });
  }
  for (auto& t : threads) {
    t.join();
  }

  // Dequeue all — should not crash, order should be consistent.
  size_t count = 0;
  bool prev_warmup = true;
  while (auto item = queue.Dequeue()) {
    // Verify ordering: warmup first, then by frequency desc.
    if (item->is_warmup && !prev_warmup) {
      ADD_FAILURE() << "Non-warmup before warmup";
    }
    prev_warmup = item->is_warmup;
    count++;
  }
  EXPECT_GT(count, 0u);
  EXPECT_LE(count, static_cast<size_t>(kThreads * kOpsPerThread));
}

TEST(AnalysisQueueTest, LargeCapacity) {
  AnalysisQueue queue(10000);
  for (uint64_t i = 0; i < 5000; ++i) {
    EXPECT_TRUE(queue.Enqueue(MakeItem(i)));
  }
  EXPECT_EQ(queue.size(), 5000u);
  EXPECT_EQ(queue.drops(), 0u);
}

// --- Additional edge case tests ---

TEST(AnalysisQueueTest, WarmupEvictsNonWarmup) {
  // Queue full of non-warmup items. Warmup item should evict lowest.
  AnalysisQueue queue(3);
  EXPECT_TRUE(queue.Enqueue(MakeItem(1, false, 10)));
  EXPECT_TRUE(queue.Enqueue(MakeItem(2, false, 20)));
  EXPECT_TRUE(queue.Enqueue(MakeItem(3, false, 5)));

  // Warmup item should evict hash=3 (freq=5, lowest priority).
  EXPECT_TRUE(queue.Enqueue(MakeItem(4, true, 1)));
  EXPECT_EQ(queue.size(), 3u);

  // Warmup item should be first dequeued.
  auto item = queue.Dequeue();
  ASSERT_TRUE(item.has_value());
  EXPECT_TRUE(item->is_warmup);
  EXPECT_EQ(item->template_hash, 4u);
}

TEST(AnalysisQueueTest, EqualPriorityDoesNotEvict) {
  // Queue full with items of equal priority. New item with same
  // priority as tail should NOT evict.
  AnalysisQueue queue(2);
  EXPECT_TRUE(queue.Enqueue(MakeItem(1, false, 10)));
  EXPECT_TRUE(queue.Enqueue(MakeItem(2, false, 10)));

  // Same frequency, same warmup -- not higher priority than tail.
  EXPECT_FALSE(queue.Enqueue(MakeItem(3, false, 10)));
  EXPECT_EQ(queue.drops(), 1u);
}

TEST(AnalysisQueueTest, SingleItemQueue) {
  AnalysisQueue queue(1);
  EXPECT_TRUE(queue.Enqueue(MakeItem(1, false, 5)));
  EXPECT_EQ(queue.size(), 1u);

  // Lower priority should be dropped.
  EXPECT_FALSE(queue.Enqueue(MakeItem(2, false, 3)));

  // Higher priority should evict.
  EXPECT_TRUE(queue.Enqueue(MakeItem(3, false, 10)));
  EXPECT_EQ(queue.size(), 1u);
  auto item = queue.Dequeue();
  EXPECT_EQ(item->template_hash, 3u);
}

TEST(AnalysisQueueTest, MultipleWarmupsSortedByFrequency) {
  AnalysisQueue queue;
  EXPECT_TRUE(queue.Enqueue(MakeItem(1, true, 5)));
  EXPECT_TRUE(queue.Enqueue(MakeItem(2, true, 15)));
  EXPECT_TRUE(queue.Enqueue(MakeItem(3, true, 10)));

  auto item1 = queue.Dequeue();
  auto item2 = queue.Dequeue();
  auto item3 = queue.Dequeue();
  EXPECT_EQ(item1->frequency, 15u);
  EXPECT_EQ(item2->frequency, 10u);
  EXPECT_EQ(item3->frequency, 5u);
}

TEST(AnalysisQueueTest, MixedWarmupAndNonWarmupOrdering) {
  AnalysisQueue queue;
  EXPECT_TRUE(queue.Enqueue(MakeItem(1, false, 100)));
  EXPECT_TRUE(queue.Enqueue(MakeItem(2, true, 1)));
  EXPECT_TRUE(queue.Enqueue(MakeItem(3, true, 50)));
  EXPECT_TRUE(queue.Enqueue(MakeItem(4, false, 50)));

  // Warmups first (by freq desc), then non-warmups (by freq desc).
  auto i1 = queue.Dequeue();
  auto i2 = queue.Dequeue();
  auto i3 = queue.Dequeue();
  auto i4 = queue.Dequeue();

  EXPECT_TRUE(i1->is_warmup);
  EXPECT_EQ(i1->frequency, 50u);
  EXPECT_TRUE(i2->is_warmup);
  EXPECT_EQ(i2->frequency, 1u);
  EXPECT_FALSE(i3->is_warmup);
  EXPECT_EQ(i3->frequency, 100u);
  EXPECT_FALSE(i4->is_warmup);
  EXPECT_EQ(i4->frequency, 50u);
}

TEST(AnalysisQueueTest, ItemFieldsPreserved) {
  AnalysisQueue queue;
  AnalysisQueue::Item item;
  item.url = "https://example.com/page";
  item.hostname = "example.com";
  item.template_hash = 42;
  item.mask = 0x08;
  item.is_warmup = true;
  item.frequency = 7;
  item.retry_count = 2;

  EXPECT_TRUE(queue.Enqueue(std::move(item)));
  auto dequeued = queue.Dequeue();
  ASSERT_TRUE(dequeued.has_value());
  EXPECT_EQ(dequeued->url, "https://example.com/page");
  EXPECT_EQ(dequeued->hostname, "example.com");
  EXPECT_EQ(dequeued->template_hash, 42u);
  EXPECT_EQ(dequeued->mask, 0x08u);
  EXPECT_TRUE(dequeued->is_warmup);
  EXPECT_EQ(dequeued->frequency, 7u);
  EXPECT_EQ(dequeued->retry_count, 2);
}

TEST(AnalysisQueueTest, ShutdownBeforeAnyEnqueue) {
  AnalysisQueue queue;
  queue.Shutdown();
  EXPECT_TRUE(queue.is_shutdown());
  EXPECT_FALSE(queue.Enqueue(MakeItem(1)));
  EXPECT_EQ(queue.size(), 0u);
}

TEST(AnalysisQueueTest, MultipleDequeueOnEmptyReturnsNullopt) {
  AnalysisQueue queue;
  EXPECT_FALSE(queue.Dequeue().has_value());
  EXPECT_FALSE(queue.Dequeue().has_value());
  EXPECT_FALSE(queue.Dequeue().has_value());
}

TEST(AnalysisQueueTest, ConcurrentEnqueueDequeue) {
  AnalysisQueue queue(100);

  std::vector<std::thread> producers;
  std::vector<std::thread> consumers;

  std::atomic<int> dequeued_count{0};

  // Producers: enqueue items.
  producers.reserve(4);
  for (int t = 0; t < 4; ++t) {
    producers.emplace_back([&queue, t]() {
      for (int i = 0; i < 50; ++i) {
        auto hash = static_cast<uint64_t>(t) * 50 + i;
        queue.Enqueue(MakeItem(hash, false, i + 1));
      }
    });
  }

  // Consumers: dequeue items.
  consumers.reserve(2);
  for (int t = 0; t < 2; ++t) {
    consumers.emplace_back([&queue, &dequeued_count]() {
      for (int i = 0; i < 100; ++i) {
        if (queue.Dequeue().has_value()) {
          dequeued_count.fetch_add(1);
        }
      }
    });
  }

  for (auto& t : producers) t.join();
  for (auto& t : consumers) t.join();

  // Drain remaining items.
  while (queue.Dequeue().has_value()) dequeued_count.fetch_add(1);

  EXPECT_GT(dequeued_count.load(), 0);
}

}  // namespace
}  // namespace pagespeed
