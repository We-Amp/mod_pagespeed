// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

// A full purge ("purge-all") while other threads keep reading and writing
// through the optimizer's cache layer.
//
// PageSpeedCache::ResetVolume() stops and destroys the cache under the cache
// layer's reset lock, but that lock is held per call, not for the lifetime of
// a handle: a ReadResult is released or renewed, and a WriteResult is
// committed (close_sync), outside it. Those calls use per-stripe state of the
// cache library that stop() frees, so they race a purge on another thread.
// The cache library orders stop() against such calls from 6e64530 on (a call
// in flight is waited for; a commit that starts after stop() is refused with
// CacheError::Closed and writes nothing).
//
// This test drives exactly that interleaving. On its own it only checks
// results (every purge succeeds, every hit returns the bytes its writer
// wrote, the cache works afterwards); the memory-safety proof comes from
// running it under the sanitizers (--config=asan / --config=tsan), where a
// handle call that touched freed stripe state is reported.
//
// Bounded and deterministic in its amount of work: a fixed number of purges.
// Before each one every reader and writer completes a fixed number of free-
// running operations, then all of them arm one handle call that runs outside
// the reset lock (a held read result, a fully streamed write awaiting its
// commit) and release it the moment the purge starts. No wall-clock
// assertion, no sleeps, no processes and no signals. Linux only (BUILD), so
// it runs in the Linux sanitizer containers.

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <expected>
#include <filesystem>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "gtest/gtest.h"
#include "lib/cache/cache.h"
#include "lib/classify/alternate_id.h"
#include "lib/classify/alternate_metadata.h"
#include "lib/classify/capability_mask.h"
#include "test/test_util/temp_dir.h"

namespace pagespeed {
namespace {

constexpr int kKeys = 64;
constexpr int kReaders = 4;
constexpr int kWriters = 4;
constexpr int kPurges = 40;
// Operations every worker completes between two purges.
constexpr uint64_t kOpsPerRound = 24;

std::string KeyUrl(int k) {
  return "/purge-load/" + std::to_string(k) + ".css";
}

// Deterministic content for key k: every writer writes the same bytes for a
// key, so any hit, before or after any purge, must return exactly these.
std::string KeyData(int k) {
  const size_t size = 2048 + static_cast<size_t>(k % 8) * 3000;
  std::string data(size, '\0');
  uint32_t x = 0x9E3779B9u ^ static_cast<uint32_t>(k);
  for (size_t i = 0; i < size; ++i) {
    x = x * 1664525u + 1013904223u;
    data[i] = static_cast<char>(x >> 24);
  }
  return data;
}

AlternateId DefaultId() {
  CapabilityMask mask;
  return MaskToAlternateId(static_cast<uint8_t>(mask.Encode() & 0xFF));
}

AlternateMetadata Meta() {
  CapabilityMask mask;
  AlternateMetadata meta;
  meta.full_mask = mask.Encode();
  meta.content_type = ContentType::kCss;
  meta.origin_content_type = "text/css";
  return meta;
}

class PurgeUnderLoadTest : public ::testing::Test {
 protected:
  void SetUp() override {
    temp_dir_ = pagespeed::test::MakeTempDir();
    PageSpeedCacheConfig config;
    config.volume_path = temp_dir_ + "/cache.vol";
    config.volume_size = static_cast<uint64_t>(32 * 1024 * 1024);
    config.enable_checksum = true;
    config.verify_checksum_on_read = true;
    config.ram_cache_size =
        0;  // every hit is a disk hit with a borrowed region
    auto result = PageSpeedCache::Create(config);
    ASSERT_TRUE(result.has_value()) << "Failed to create cache";
    cache_ = std::move(*result);
  }

  void TearDown() override {
    cache_.reset();
    std::filesystem::remove_all(temp_dir_);
  }

  // One write of key k, in three steps the way the worker writes: obtain the
  // handle (under the reset lock), stream the content and commit (outside
  // it). A refusal at any step is an expected outcome around a purge.
  bool WriteKey(int k, const std::string& data) {
    auto wh = cache_->WriteAlternate(KeyUrl(k), "example.com", "https",
                                     DefaultId(), data.size(), Meta());
    if (!wh) return false;
    const auto bytes = std::as_bytes(std::span(data.data(), data.size()));
    const size_t half = bytes.size() / 2;
    if (!wh->write_sync(bytes.first(half))) return false;
    if (!wh->write_sync(bytes.subspan(half))) return false;
    return wh->close_sync().has_value();
  }

  std::string temp_dir_;
  std::unique_ptr<PageSpeedCache> cache_;
};

TEST_F(PurgeUnderLoadTest, PurgeAllWhileReadsAndWritesAreInFlight) {
  std::vector<std::string> data(kKeys);
  for (int k = 0; k < kKeys; ++k) {
    data[k] = KeyData(k);
    ASSERT_TRUE(WriteKey(k, data[k])) << "initial write of key " << k;
  }

  constexpr int kWorkers = kReaders + kWriters;
  std::atomic<bool> done{false};
  std::atomic<int> mismatches{0};
  std::atomic<uint64_t> hits{0};
  std::atomic<uint64_t> commits{0};
  std::atomic<uint64_t> refused{0};
  std::atomic<uint64_t> armed_reads{0};
  std::atomic<uint64_t> armed_commits{0};
  std::vector<std::atomic<uint64_t>> progress(kWorkers);
  for (auto& x : progress) x.store(0);
  // Rendezvous per purge round p (1-based): the purger raises arm_round to p;
  // every worker then prepares one handle call that runs OUTSIDE the cache
  // layer's reset lock (a reader holds a disk-hit ReadResult, a writer has
  // streamed a WriteResult up to its commit), reports armed, and waits for
  // go_round == p. The purger sets go_round and calls ResetVolume() at once,
  // so the releases, renewals and commits race the purge's stop().
  std::atomic<int> arm_round{0};
  std::atomic<int> go_round{0};
  std::atomic<int> armed{0};

  auto check = [&](int k, const ReadResult& rr) {
    auto content = rr.content();
    std::string got(reinterpret_cast<const char*>(content.data()),
                    content.size());
    if (got != data[k]) {
      mismatches.fetch_add(1);
    } else {
      hits.fetch_add(1);
    }
  };
  auto wait_go = [&](int round) {
    armed.fetch_add(1, std::memory_order_acq_rel);
    while (go_round.load(std::memory_order_acquire) < round) {
      std::this_thread::yield();
    }
  };

  std::vector<std::thread> threads;
  threads.reserve(kWorkers);
  for (int r = 0; r < kReaders; ++r) {
    threads.emplace_back([&, r] {
      uint64_t i = 0;
      int served_round = 0;
      while (!done.load(std::memory_order_acquire)) {
        const int k =
            static_cast<int>((i * 7 + static_cast<uint64_t>(r) * 13) % kKeys);
        const int round = arm_round.load(std::memory_order_acquire);
        if (round > served_round) {
          served_round = round;
          auto rr = cache_->ReadAlternate(KeyUrl(k), "example.com", "https",
                                          DefaultId());
          if (!(rr && rr->is_valid())) {
            (void)WriteKey(k, data[k]);
            rr = cache_->ReadAlternate(KeyUrl(k), "example.com", "https",
                                       DefaultId());
          }
          const bool held = rr && rr->is_valid();
          if (held) {
            check(k, *rr);
            armed_reads.fetch_add(1);
          }
          wait_go(round);
          if (held) {
            (void)rr->renew_lease();
            (void)rr->ns_until_forced_wrap();
            if (r % 2 == 0) {
              rr->release();
            } else {
              rr = std::unexpected(cyclone::CacheError::NotFound);  // drop it
            }
          }
        } else {
          auto rr = cache_->ReadAlternate(KeyUrl(k), "example.com", "https",
                                          DefaultId());
          if (rr && rr->is_valid()) {
            check(k, *rr);
            (void)rr->renew_lease();
            (void)rr->ns_until_forced_wrap();
            (void)rr->renew_lease_strict();
            if (i % 2 == 0) rr->release();
          }
        }
        ++i;
        progress[r].fetch_add(1, std::memory_order_release);
      }
    });
  }
  for (int w = 0; w < kWriters; ++w) {
    threads.emplace_back([&, w] {
      uint64_t i = 0;
      int served_round = 0;
      while (!done.load(std::memory_order_acquire)) {
        const int k =
            static_cast<int>((i * 5 + static_cast<uint64_t>(w) * 17) % kKeys);
        const int round = arm_round.load(std::memory_order_acquire);
        if (round > served_round) {
          served_round = round;
          // Stream the whole entry, then hold the commit for the purge.
          auto wh = cache_->WriteAlternate(KeyUrl(k), "example.com", "https",
                                           DefaultId(), data[k].size(), Meta());
          const auto bytes =
              std::as_bytes(std::span(data[k].data(), data[k].size()));
          const bool streamed = wh && wh->write_sync(bytes).has_value();
          if (streamed) armed_commits.fetch_add(1);
          wait_go(round);
          if (streamed && wh->close_sync().has_value()) {
            commits.fetch_add(1);
          } else {
            refused.fetch_add(1);
          }
        } else if (WriteKey(k, data[k])) {
          commits.fetch_add(1);
        } else {
          refused.fetch_add(1);
        }
        ++i;
        progress[kReaders + w].fetch_add(1, std::memory_order_release);
      }
    });
  }

  // Purger (this thread). Before each purge every worker has done
  // kOpsPerRound free-running operations since the previous one, then all of
  // them arm; the purge starts the moment they are released.
  std::vector<uint64_t> mark(kWorkers, 0);
  int purge_failures = 0;
  for (int p = 1; p <= kPurges; ++p) {
    for (int t = 0; t < kWorkers; ++t) {
      while (progress[t].load(std::memory_order_acquire) <
             mark[t] + kOpsPerRound) {
        std::this_thread::yield();
      }
    }
    armed.store(0, std::memory_order_release);
    arm_round.store(p, std::memory_order_release);
    while (armed.load(std::memory_order_acquire) < kWorkers) {
      std::this_thread::yield();
    }
    go_round.store(p, std::memory_order_release);
    if (!cache_->ResetVolume().has_value()) ++purge_failures;
    for (int t = 0; t < kWorkers; ++t) {
      mark[t] = progress[t].load(std::memory_order_acquire);
    }
  }
  done.store(true, std::memory_order_release);
  for (auto& t : threads) t.join();

  EXPECT_EQ(purge_failures, 0);
  EXPECT_EQ(mismatches.load(), 0) << "a hit returned bytes its key never had";
  EXPECT_GT(commits.load(), 0u);
  EXPECT_GT(hits.load(), 0u);
  // The rendezvous really put handle calls against the purges.
  EXPECT_GT(armed_reads.load(), 0u);
  EXPECT_GT(armed_commits.load(), 0u);
  RecordProperty("hits", static_cast<int>(hits.load()));
  RecordProperty("commits", static_cast<int>(commits.load()));
  RecordProperty("refused_writes", static_cast<int>(refused.load()));
  RecordProperty("armed_reads", static_cast<int>(armed_reads.load()));
  RecordProperty("armed_commits", static_cast<int>(armed_commits.load()));

  // The cache works after the storm.
  ASSERT_TRUE(WriteKey(0, data[0]));
  auto rr =
      cache_->ReadAlternate(KeyUrl(0), "example.com", "https", DefaultId());
  ASSERT_TRUE(rr.has_value() && rr->is_valid());
  auto content = rr->content();
  EXPECT_EQ(std::string(reinterpret_cast<const char*>(content.data()),
                        content.size()),
            data[0]);
}

// The two refusals the library promises around a purge, on one thread so the
// order is fixed: a write streamed before the purge and committed after it
// is refused with CacheError::Closed and leaves no entry behind, and a read
// result taken before the purge can still be renewed and released after it.
// Each purge also advances the generation file other processes watch.
TEST_F(PurgeUnderLoadTest, CommitAfterPurgeIsRefusedAndLeavesNoEntry) {
  const std::string gen_path = temp_dir_ + "/cache.vol.gen";
  const std::string data = KeyData(1);
  ASSERT_TRUE(WriteKey(1, data));
  auto held =
      cache_->ReadAlternate(KeyUrl(1), "example.com", "https", DefaultId());
  ASSERT_TRUE(held.has_value() && held->is_valid());

  auto wh = cache_->WriteAlternate(KeyUrl(2), "example.com", "https",
                                   DefaultId(), data.size(), Meta());
  ASSERT_TRUE(wh.has_value());
  ASSERT_TRUE(wh->write_sync(std::as_bytes(std::span(data.data(), data.size())))
                  .has_value());

  const uint64_t gen_before = PageSpeedCache::ReadGenerationFile(gen_path);
  ASSERT_TRUE(cache_->ResetVolume().has_value());
  EXPECT_GT(PageSpeedCache::ReadGenerationFile(gen_path), gen_before);

  auto closed = wh->close_sync();
  ASSERT_FALSE(closed.has_value());
  EXPECT_EQ(closed.error(), cyclone::CacheError::Closed);
  EXPECT_FALSE(
      cache_->ReadAlternate(KeyUrl(2), "example.com", "https", DefaultId())
          .has_value());
  // The purge emptied the cache; the held result is from before it.
  EXPECT_FALSE(
      cache_->ReadAlternate(KeyUrl(1), "example.com", "https", DefaultId())
          .has_value());
  (void)held->renew_lease();
  (void)held->ns_until_forced_wrap();
  held->release();

  // The cache works after the purge.
  ASSERT_TRUE(WriteKey(2, data));
  auto rr =
      cache_->ReadAlternate(KeyUrl(2), "example.com", "https", DefaultId());
  ASSERT_TRUE(rr.has_value() && rr->is_valid());
}

}  // namespace
}  // namespace pagespeed
