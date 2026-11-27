// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

// Stress test for cache burst writes — reproduces a scenario where writing
// many variants concurrently causes the original alternate to become
// unreadable ("not found") despite low cache utilization.
//
// Models the production pattern:
//   1. Nginx writes the original image at CapabilityMask() (0x08)
//   2. Worker spawns threads writing 20-36 variants at different masks
//   3. Concurrent reads of the original must never fail

#include <array>
#include <atomic>
#include <chrono>
#include <cstdarg>
#include <cstddef>
#include <filesystem>
#include <map>
#include <random>
#include <string>
#include <thread>
#include <vector>

#include "gtest/gtest.h"
#include "lib/base/message_handler.h"
#include "lib/cache/cache.h"
#include "lib/classify/alternate_id.h"
#include "lib/classify/alternate_metadata.h"
#include "lib/classify/capability_mask.h"
#include "test/test_util/temp_dir.h"

namespace pagespeed {
namespace {

// Generate pseudo-random data of the given size.
std::string MakeRandomData(size_t size, uint32_t seed = 42) {
  std::mt19937 rng(seed);
  std::string data(size, '\0');
  for (size_t i = 0; i < size; i += 4) {
    uint32_t val = rng();
    size_t remaining = std::min(size - i, size_t{4});
    std::memcpy(data.data() + i, &val, remaining);
  }
  return data;
}

// Convert CapabilityMask to AlternateId (same as worker.cc MaskToId).
AlternateId MaskToId(const CapabilityMask& mask) {
  return MaskToAlternateId(static_cast<uint8_t>(mask.Encode() & 0xFF));
}

// Build the full set of variant masks the worker would generate for an image.
// This covers: 3 formats (WebP, AVIF, Original) x 3 viewports x 2 densities
// x 2 save-data = up to 36 identity variants, plus gzip/brotli for each.
// We skip compressed variants here and focus on identity variants (the main
// write burst).
std::vector<CapabilityMask> BuildVariantMasks() {
  using IF = CapabilityMask::ImageFormat;
  using VP = CapabilityMask::Viewport;
  using PD = CapabilityMask::PixelDensity;
  using SD = CapabilityMask::SaveData;
  using TE = CapabilityMask::TransferEncoding;

  std::vector<CapabilityMask> masks;

  // Worker writes optimized formats (WebP, AVIF) across all dimensions.
  // Also writes Original at non-default viewports/densities.
  IF formats[] = {IF::kWebP, IF::kAvif, IF::kOriginal};
  VP viewports[] = {VP::kMobile, VP::kTablet, VP::kDesktop};
  PD densities[] = {PD::k1x, PD::k2xPlus};
  SD save_data[] = {SD::kOff, SD::kOn};

  for (auto fmt : formats) {
    for (auto vp : viewports) {
      for (auto pd : densities) {
        for (auto sd : save_data) {
          CapabilityMask m(fmt, vp, pd, sd, TE::kIdentity);
          // Skip the default mask (0x08) — that's the "original" written by
          // nginx.
          if (m.Encode() == CapabilityMask().Encode()) continue;
          masks.push_back(m);
        }
      }
    }
  }

  return masks;
}

// Write a variant to the cache (mirrors worker.cc WriteVariant).
bool WriteVariant(PageSpeedCache* cache, std::string_view url,
                  std::string_view hostname, const CapabilityMask& mask,
                  std::string_view data) {
  auto id = MaskToId(mask);
  AlternateMetadata meta;
  meta.full_mask = mask.Encode();
  meta.content_type = ContentType::kImage;
  meta.origin_content_type = "image/jpeg";

  auto wh =
      cache->WriteAlternate(url, hostname, "https", id, data.size(), meta);
  if (!wh) return false;
  auto written =
      wh->write_sync(std::as_bytes(std::span(data.data(), data.size())));
  if (!written) return false;
  return wh->close_sync().has_value();
}

// Read the best alternate, retrying a Busy result. Busy means a writer held
// the key's directory bucket for the cache's whole wait budget, which
// happens when that writer is descheduled mid-update on an oversubscribed
// machine (a sanitizer lane running many tests at once). Busy says the
// key's presence is unknown, not that it is absent, and the cache's
// contract is to retry. A miss or corruption is returned at once, so the
// tests still fail on the condition they guard against.
auto ReadBestRetryingBusy(PageSpeedCache* cache, std::string_view url,
                          std::string_view hostname,
                          const CapabilityMask& mask) {
  constexpr int kMaxBusyRetries = 20;
  for (int attempt = 0;; ++attempt) {
    auto result = cache->ReadBestAlternate(url, hostname, "https", mask);
    if (result.has_value() || result.error() != cyclone::CacheError::Busy ||
        attempt == kMaxBusyRetries) {
      return result;
    }
    std::this_thread::yield();
  }
}

class CacheBurstTest : public ::testing::Test {
 protected:
  void SetUp() override {
    temp_dir_ = pagespeed::test::MakeTempDir();
    cache_path_ = temp_dir_ + "/cache.vol";
  }

  void TearDown() override {
    cache_.reset();
    std::filesystem::remove_all(temp_dir_);
  }

  void CreateCache(uint64_t size = 2ULL * 1024 * 1024 * 1024) {
    PageSpeedCacheConfig config;
    config.volume_path = cache_path_;
    config.volume_size = size;
    config.enable_checksum = true;
    config.verify_checksum_on_read = true;
    config.ram_cache_size = 0;  // Disabled per project convention
    // enable_mmap_directory = true is the default (multi_process.enabled=true)
    // but be explicit to match the production requirement.
    config.multi_process.enabled = true;
    config.multi_process.process_index = 0;
    config.multi_process.total_processes = 1;

    auto result = PageSpeedCache::Create(config);
    ASSERT_TRUE(result.has_value()) << "Failed to create cache";
    cache_ = std::move(*result);
  }

  // Write the "original" image at default mask (what nginx does).
  void WriteOriginal(std::string_view url, std::string_view hostname,
                     std::string_view data) {
    CapabilityMask default_mask;  // Desktop/Identity = 0x08
    auto id = MaskToId(default_mask);
    AlternateMetadata meta;
    meta.full_mask = default_mask.Encode();
    meta.content_type = ContentType::kImage;
    meta.origin_content_type = "image/jpeg";

    auto wh =
        cache_->WriteAlternate(url, hostname, "https", id, data.size(), meta);
    ASSERT_TRUE(wh.has_value())
        << "WriteAlternate failed err=" << static_cast<int>(wh.error());
    auto bytes = std::as_bytes(std::span(data.data(), data.size()));
    auto written = wh->write_sync(bytes);
    ASSERT_TRUE(written.has_value())
        << "write_sync err=" << static_cast<int>(written.error());
    auto closed = wh->close_sync();
    ASSERT_TRUE(closed.has_value())
        << "close_sync err=" << static_cast<int>(closed.error());
  }

  // Read the original via ReadBestAlternate with default mask.
  bool ReadOriginal(std::string_view url, std::string_view hostname) {
    CapabilityMask default_mask;
    auto result =
        ReadBestRetryingBusy(cache_.get(), url, hostname, default_mask);
    return result.has_value() && result->is_valid();
  }

  std::string temp_dir_;
  std::string cache_path_;
  std::unique_ptr<PageSpeedCache> cache_;
};

// Test 1: Write original, then burst-write many variants from multiple
// threads while a reader thread continuously reads the original.
TEST_F(CacheBurstTest, OriginalSurvivesConcurrentVariantWrites) {
  CreateCache();

  const std::string url = "/img/hero.jpg";
  const std::string hostname = "example.com";

  // ~15KB original image (realistic size)
  std::string original_data =
      MakeRandomData(static_cast<size_t>(15 * 1024), /*seed=*/1);
  WriteOriginal(url, hostname, original_data);

  // Verify the original is readable before we start.
  ASSERT_TRUE(ReadOriginal(url, hostname))
      << "Original must be readable before burst writes";

  auto variant_masks = BuildVariantMasks();
  ASSERT_GE(variant_masks.size(), 20u)
      << "Expected at least 20 variant masks for realistic test";

  // Generate variant data (different sizes to simulate transcoded images).
  std::vector<std::string> variant_data;
  for (size_t i = 0; i < variant_masks.size(); ++i) {
    // Variants range from 5KB to 20KB (realistic for transcoded images).
    size_t size = static_cast<size_t>(5 * 1024) + (i * 500);
    variant_data.push_back(MakeRandomData(size, /*seed=*/100 + i));
  }

  std::atomic<bool> writers_done{false};
  std::atomic<int> read_successes{0};
  std::atomic<int> read_failures{0};
  std::atomic<int> total_reads{0};

  // Reader thread: continuously read the original while writes are happening.
  std::thread reader([&]() {
    while (!writers_done.load(std::memory_order_acquire)) {
      total_reads.fetch_add(1, std::memory_order_relaxed);
      if (ReadOriginal(url, hostname)) {
        read_successes.fetch_add(1, std::memory_order_relaxed);
      } else {
        read_failures.fetch_add(1, std::memory_order_relaxed);
      }
    }
    // One final read after all writes complete.
    total_reads.fetch_add(1, std::memory_order_relaxed);
    if (ReadOriginal(url, hostname)) {
      read_successes.fetch_add(1, std::memory_order_relaxed);
    } else {
      read_failures.fetch_add(1, std::memory_order_relaxed);
    }
  });

  // Writer threads: distribute variant writes across 4 threads.
  constexpr int kNumWriterThreads = 4;
  std::vector<std::thread> writers;
  writers.reserve(kNumWriterThreads);
  std::atomic<int> write_successes{0};
  std::atomic<int> write_failures{0};

  for (int t = 0; t < kNumWriterThreads; ++t) {
    writers.emplace_back([&, t]() {
      for (size_t i = t; i < variant_masks.size(); i += kNumWriterThreads) {
        if (WriteVariant(cache_.get(), url, hostname, variant_masks[i],
                         variant_data[i])) {
          write_successes.fetch_add(1, std::memory_order_relaxed);
        } else {
          write_failures.fetch_add(1, std::memory_order_relaxed);
        }
      }
    });
  }

  // Wait for all writers to finish.
  for (auto& w : writers) w.join();
  writers_done.store(true, std::memory_order_release);
  reader.join();

  // Report statistics.
  int total = total_reads.load();
  int successes = read_successes.load();
  int failures = read_failures.load();
  int w_ok = write_successes.load();
  int w_fail = write_failures.load();

  printf(
      "Burst test: %d reads (%d ok, %d FAILED), %d writes (%d ok, %d fail), "
      "%zu variant masks\n",
      total, successes, failures, w_ok + w_fail, w_ok, w_fail,
      variant_masks.size());

  // The critical assertion: reads of the original must NEVER fail during
  // concurrent variant writes.
  EXPECT_EQ(read_failures.load(), 0)
      << "Original became unreadable during burst writes! " << failures << "/"
      << total << " reads failed";

  // After all writes, the original must still be readable.
  ASSERT_TRUE(ReadOriginal(url, hostname))
      << "Original not readable after burst writes completed";

  // Verify we can still list all alternates.
  auto alts = cache_->ListAlternates(url, hostname, "https");
  ASSERT_TRUE(alts.has_value())
      << "ListAlternates failed: " << static_cast<int>(alts.error());
  printf("Total alternates stored: %zu\n", alts->size());
}

// Test 2: Same as above but with multiple URLs hitting the same cache
// concurrently (stresses the same stripe since keys may collide).
TEST_F(CacheBurstTest, MultiUrlConcurrentBurstWrites) {
  CreateCache();

  constexpr int kNumUrls = 8;
  constexpr int kNumWriterThreadsPerUrl = 2;

  // Generate unique URLs and original data.
  std::vector<std::string> urls;
  std::vector<std::string> original_data;
  for (int i = 0; i < kNumUrls; ++i) {
    urls.push_back("/img/photo_" + std::to_string(i) + ".jpg");
    original_data.push_back(
        MakeRandomData(static_cast<size_t>(15 * 1024), /*seed=*/1000 + i));
  }

  const std::string hostname = "example.com";

  // Write all originals first (nginx's job).
  for (int i = 0; i < kNumUrls; ++i) {
    WriteOriginal(urls[i], hostname, original_data[i]);
    ASSERT_TRUE(ReadOriginal(urls[i], hostname))
        << "Original for " << urls[i] << " not readable after write";
  }

  auto variant_masks = BuildVariantMasks();

  std::atomic<bool> writers_done{false};
  std::atomic<int> read_failures{0};
  std::atomic<int> total_reads{0};

  // Reader threads: one per URL, continuously reading the original.
  std::vector<std::thread> readers;
  readers.reserve(kNumUrls);
  for (int i = 0; i < kNumUrls; ++i) {
    readers.emplace_back([&, i]() {
      while (!writers_done.load(std::memory_order_acquire)) {
        total_reads.fetch_add(1, std::memory_order_relaxed);
        if (!ReadOriginal(urls[i], hostname)) {
          read_failures.fetch_add(1, std::memory_order_relaxed);
        }
      }
      // Final read after writes complete.
      total_reads.fetch_add(1, std::memory_order_relaxed);
      if (!ReadOriginal(urls[i], hostname)) {
        read_failures.fetch_add(1, std::memory_order_relaxed);
      }
    });
  }

  // Writer threads: each URL gets kNumWriterThreadsPerUrl threads.
  std::vector<std::thread> writers;
  std::atomic<int> write_successes{0};

  for (int u = 0; u < kNumUrls; ++u) {
    for (int t = 0; t < kNumWriterThreadsPerUrl; ++t) {
      writers.emplace_back([&, u, t]() {
        for (size_t i = t; i < variant_masks.size();
             i += kNumWriterThreadsPerUrl) {
          // Use different sized data per variant.
          std::string data =
              MakeRandomData(static_cast<size_t>(5 * 1024) + (i * 300),
                             /*seed=*/2000 + u * 100 + i);
          if (WriteVariant(cache_.get(), urls[u], hostname, variant_masks[i],
                           data)) {
            write_successes.fetch_add(1, std::memory_order_relaxed);
          }
        }
      });
    }
  }

  // Wait for all writers then signal readers.
  for (auto& w : writers) w.join();
  writers_done.store(true, std::memory_order_release);
  for (auto& r : readers) r.join();

  int failures = read_failures.load();
  int total = total_reads.load();

  printf(
      "Multi-URL burst: %d reads (%d FAILED), %d URLs, %d writes ok, "
      "%zu masks/url\n",
      total, failures, kNumUrls, write_successes.load(), variant_masks.size());

  EXPECT_EQ(failures, 0)
      << "Original became unreadable during multi-URL burst! " << failures
      << "/" << total << " reads failed";

  // Verify all originals readable after everything settles.
  for (int i = 0; i < kNumUrls; ++i) {
    EXPECT_TRUE(ReadOriginal(urls[i], hostname))
        << "Original for " << urls[i] << " not readable after burst";
  }
}

// Test 3: Rapid sequential writes followed by immediate reads — no
// concurrency, just verifies the basic write-read contract with many
// alternates.
TEST_F(CacheBurstTest, SequentialBurstWriteThenRead) {
  CreateCache();

  const std::string url = "/img/banner.png";
  const std::string hostname = "example.com";
  std::string original_data =
      MakeRandomData(static_cast<size_t>(15 * 1024), /*seed=*/99);

  WriteOriginal(url, hostname, original_data);
  ASSERT_TRUE(ReadOriginal(url, hostname));

  auto variant_masks = BuildVariantMasks();

  // Write all variants sequentially.
  int written = 0;
  for (size_t i = 0; i < variant_masks.size(); ++i) {
    std::string data =
        MakeRandomData(static_cast<size_t>(8 * 1024), /*seed=*/500 + i);
    if (WriteVariant(cache_.get(), url, hostname, variant_masks[i], data)) {
      ++written;
    }

    // Check the original is still readable after every write.
    ASSERT_TRUE(ReadOriginal(url, hostname))
        << "Original became unreadable after writing variant " << i
        << " (mask=0x" << std::hex << variant_masks[i].Encode() << std::dec
        << ")";
  }

  printf("Sequential burst: wrote %d/%zu variants, original still readable\n",
         written, variant_masks.size());

  // Verify the original content is correct.
  CapabilityMask default_mask;
  auto result = cache_->ReadBestAlternate(url, hostname, "https", default_mask);
  ASSERT_TRUE(result.has_value());
  auto content = result->content();
  ASSERT_EQ(content.size(), original_data.size());
  EXPECT_EQ(
      std::memcmp(content.data(), original_data.data(), original_data.size()),
      0)
      << "Original content corrupted after burst writes";
}

// Test 4: Two cache instances on the same file — simulates the nginx+worker
// cross-process pattern. Instance A (nginx) writes the original, instance B
// (worker) reads it and writes variants. This is the exact scenario where
// production failures occur.
TEST_F(CacheBurstTest, CrossInstanceOriginalVisibility) {
  CreateCache();

  const std::string url = "/img/cross.jpg";
  const std::string hostname = "example.com";
  std::string original_data =
      MakeRandomData(static_cast<size_t>(15 * 1024), /*seed=*/77);

  // Write the original via our primary cache instance (simulates nginx).
  WriteOriginal(url, hostname, original_data);
  ASSERT_TRUE(ReadOriginal(url, hostname));

  // Open a SECOND cache instance on the same file (simulates the worker
  // process opening the same volume with enable_mmap_directory=true).
  PageSpeedCacheConfig config2;
  config2.volume_path = cache_path_;
  config2.volume_size = 2ULL * 1024 * 1024 * 1024;
  config2.enable_checksum = true;
  config2.verify_checksum_on_read = true;
  config2.ram_cache_size = 0;
  config2.multi_process.enabled = true;
  config2.multi_process.process_index = 0;
  config2.multi_process.total_processes = 1;

  auto cache2_result = PageSpeedCache::Create(config2);
  ASSERT_TRUE(cache2_result.has_value()) << "Failed to create second cache";
  auto cache2 = std::move(*cache2_result);

  // The second instance must be able to read the original written by the
  // first instance.
  {
    CapabilityMask default_mask;
    auto result =
        cache2->ReadBestAlternate(url, hostname, "https", default_mask);
    ASSERT_TRUE(result.has_value())
        << "Second cache instance cannot read original written by first! "
        << "error=" << static_cast<int>(result.error());
  }

  // Now write variants through the second instance while reading the original
  // from BOTH instances.
  auto variant_masks = BuildVariantMasks();
  std::atomic<bool> writers_done{false};
  std::atomic<int> read_failures_inst1{0};
  std::atomic<int> read_failures_inst2{0};
  std::atomic<int> total_reads{0};

  // Reader thread on instance 1 (nginx-side reads).
  std::thread reader1([&]() {
    while (!writers_done.load(std::memory_order_acquire)) {
      total_reads.fetch_add(1, std::memory_order_relaxed);
      if (!ReadOriginal(url, hostname)) {
        read_failures_inst1.fetch_add(1, std::memory_order_relaxed);
      }
    }
  });

  // Reader thread on instance 2 (worker-side reads).
  std::thread reader2([&]() {
    CapabilityMask default_mask;
    while (!writers_done.load(std::memory_order_acquire)) {
      total_reads.fetch_add(1, std::memory_order_relaxed);
      auto r = ReadBestRetryingBusy(cache2.get(), url, hostname, default_mask);
      if (!r.has_value()) {
        read_failures_inst2.fetch_add(1, std::memory_order_relaxed);
      }
    }
  });

  // Writer threads on instance 2 (worker writes variants).
  constexpr int kNumWriterThreads = 4;
  std::vector<std::thread> writers;
  writers.reserve(kNumWriterThreads);
  std::atomic<int> write_successes{0};

  for (int t = 0; t < kNumWriterThreads; ++t) {
    writers.emplace_back([&, t]() {
      for (size_t i = t; i < variant_masks.size(); i += kNumWriterThreads) {
        std::string data =
            MakeRandomData(static_cast<size_t>(8 * 1024), /*seed=*/700 + i);
        if (WriteVariant(cache2.get(), url, hostname, variant_masks[i], data)) {
          write_successes.fetch_add(1, std::memory_order_relaxed);
        }
      }
    });
  }

  for (auto& w : writers) w.join();
  writers_done.store(true, std::memory_order_release);
  reader1.join();
  reader2.join();

  int f1 = read_failures_inst1.load();
  int f2 = read_failures_inst2.load();
  int total = total_reads.load();

  printf(
      "Cross-instance burst: %d reads (%d inst1-fails, %d inst2-fails), "
      "%d writes ok\n",
      total, f1, f2, write_successes.load());

  EXPECT_EQ(f1, 0) << "Instance 1 (nginx) lost visibility of original!";
  EXPECT_EQ(f2, 0) << "Instance 2 (worker) lost visibility of original!";

  // Final check: both instances can read the original.
  EXPECT_TRUE(ReadOriginal(url, hostname))
      << "Instance 1 cannot read original after cross-instance burst";
  {
    CapabilityMask default_mask;
    auto r = cache2->ReadBestAlternate(url, hostname, "https", default_mask);
    EXPECT_TRUE(r.has_value())
        << "Instance 2 cannot read original after cross-instance burst";
  }
}

// Test 5: Cross-instance with multiple URLs and instance 1 (nginx) also
// writing originals concurrently with instance 2 (worker) writing variants.
// This is the most realistic reproduction of the production scenario.
TEST_F(CacheBurstTest, CrossInstanceConcurrentOriginalAndVariantWrites) {
  CreateCache();

  constexpr int kNumUrls = 16;
  const std::string hostname = "example.com";

  // Open second cache instance.
  PageSpeedCacheConfig config2;
  config2.volume_path = cache_path_;
  config2.volume_size = 2ULL * 1024 * 1024 * 1024;
  config2.enable_checksum = true;
  config2.verify_checksum_on_read = true;
  config2.ram_cache_size = 0;
  config2.multi_process.enabled = true;
  config2.multi_process.process_index = 0;
  config2.multi_process.total_processes = 1;

  auto cache2_result = PageSpeedCache::Create(config2);
  ASSERT_TRUE(cache2_result.has_value());
  auto cache2 = std::move(*cache2_result);

  auto variant_masks = BuildVariantMasks();

  // Phase 1: Instance 1 (nginx) writes all originals.
  std::vector<std::string> urls;
  for (int i = 0; i < kNumUrls; ++i) {
    urls.push_back("/img/burst_" + std::to_string(i) + ".jpg");
    std::string data =
        MakeRandomData(static_cast<size_t>(15 * 1024), /*seed=*/3000 + i);
    WriteOriginal(urls[i], hostname, data);
  }

  // Phase 2: Instance 2 (worker) writes variants for all URLs concurrently,
  // while a reader thread on instance 2 verifies originals stay readable.
  std::atomic<bool> done{false};
  std::atomic<int> read_failures{0};
  std::atomic<int> total_reads{0};

  // Reader on instance 2.
  std::thread reader([&]() {
    CapabilityMask default_mask;
    while (!done.load(std::memory_order_acquire)) {
      for (int i = 0; i < kNumUrls; ++i) {
        total_reads.fetch_add(1, std::memory_order_relaxed);
        auto r =
            ReadBestRetryingBusy(cache2.get(), urls[i], hostname, default_mask);
        if (!r.has_value()) {
          read_failures.fetch_add(1, std::memory_order_relaxed);
        }
      }
    }
  });

  // Writers on instance 2 (4 threads, round-robin across URLs and variants).
  constexpr int kNumWriterThreads = 4;
  std::vector<std::thread> writers;
  writers.reserve(kNumWriterThreads);
  std::atomic<int> write_ok{0};

  for (int t = 0; t < kNumWriterThreads; ++t) {
    writers.emplace_back([&, t]() {
      for (int u = t; u < kNumUrls; u += kNumWriterThreads) {
        for (size_t v = 0; v < variant_masks.size(); ++v) {
          std::string data = MakeRandomData(static_cast<size_t>(8 * 1024),
                                            /*seed=*/4000 + u * 100 + v);
          if (WriteVariant(cache2.get(), urls[u], hostname, variant_masks[v],
                           data)) {
            write_ok.fetch_add(1, std::memory_order_relaxed);
          }
        }
      }
    });
  }

  for (auto& w : writers) w.join();
  done.store(true, std::memory_order_release);
  reader.join();

  int failures = read_failures.load();
  int total = total_reads.load();

  printf("Cross-instance multi-URL: %d reads (%d FAILED), %d URLs, %d writes\n",
         total, failures, kNumUrls, write_ok.load());

  EXPECT_EQ(failures, 0)
      << "Cross-instance: original became unreadable during burst! " << failures
      << "/" << total;
}

// Test 6: Cross-instance concurrent writes to DIFFERENT keys — reproduces
// the shared_write_pos TOCTOU race.  Two cache instances write to the same
// volume file simultaneously: instance 1 simulates nginx writing originals,
// instance 2 simulates the worker writing originals for different URLs.
// Both processes allocate write space from the same shared_write_pos without
// cross-process synchronization, causing overlapping writes → CRC corruption.
TEST_F(CacheBurstTest, CrossInstanceWriteOverlapCorruption) {
  CreateCache();

  const std::string hostname = "example.com";

  // Open second cache instance (simulates worker process).
  PageSpeedCacheConfig config2;
  config2.volume_path = cache_path_;
  config2.volume_size = 2ULL * 1024 * 1024 * 1024;
  config2.enable_checksum = true;
  config2.verify_checksum_on_read = true;
  config2.ram_cache_size = 0;
  config2.multi_process.enabled = true;
  config2.multi_process.process_index = 0;
  config2.multi_process.total_processes = 1;

  auto cache2_result = PageSpeedCache::Create(config2);
  ASSERT_TRUE(cache2_result.has_value());
  auto cache2 = std::move(*cache2_result);

  // Each instance writes a set of unique URLs concurrently.
  // Using larger payloads increases the chance of overlapping writes.
  constexpr int kUrlsPerInstance = 50;
  constexpr size_t kPayloadSize =
      static_cast<const size_t>(32 * 1024);  // 32KB each

  std::vector<std::string> urls1, urls2;
  std::vector<std::string> data1, data2;
  for (int i = 0; i < kUrlsPerInstance; ++i) {
    urls1.push_back("/nginx/img_" + std::to_string(i) + ".jpg");
    urls2.push_back("/worker/img_" + std::to_string(i) + ".jpg");
    data1.push_back(MakeRandomData(kPayloadSize, /*seed=*/5000 + i));
    data2.push_back(MakeRandomData(kPayloadSize, /*seed=*/6000 + i));
  }

  // Write concurrently from both instances.
  std::atomic<int> write_ok_1{0}, write_ok_2{0};
  std::atomic<int> write_fail_1{0}, write_fail_2{0};

  auto writer_fn = [&](PageSpeedCache* cache,
                       const std::vector<std::string>& urls,
                       const std::vector<std::string>& data,
                       std::atomic<int>& ok, std::atomic<int>& fail) {
    CapabilityMask default_mask;
    auto id = MaskToId(default_mask);
    AlternateMetadata meta;
    meta.full_mask = default_mask.Encode();
    meta.content_type = ContentType::kImage;
    meta.origin_content_type = "image/jpeg";

    for (int i = 0; i < kUrlsPerInstance; ++i) {
      auto wh = cache->WriteAlternate(urls[i], hostname, "https", id,
                                      data[i].size(), meta);
      if (!wh) {
        fail.fetch_add(1, std::memory_order_relaxed);
        continue;
      }
      auto bytes = std::as_bytes(std::span(data[i].data(), data[i].size()));
      auto written = wh->write_sync(bytes);
      if (!written) {
        fail.fetch_add(1, std::memory_order_relaxed);
        continue;
      }
      auto closed = wh->close_sync();
      if (!closed) {
        fail.fetch_add(1, std::memory_order_relaxed);
        continue;
      }
      ok.fetch_add(1, std::memory_order_relaxed);
    }
  };

  std::thread t1(writer_fn, cache_.get(), std::cref(urls1), std::cref(data1),
                 std::ref(write_ok_1), std::ref(write_fail_1));
  std::thread t2(writer_fn, cache2.get(), std::cref(urls2), std::cref(data2),
                 std::ref(write_ok_2), std::ref(write_fail_2));
  t1.join();
  t2.join();

  printf("Cross-instance writes: inst1=%d ok/%d fail, inst2=%d ok/%d fail\n",
         write_ok_1.load(), write_fail_1.load(), write_ok_2.load(),
         write_fail_2.load());

  // Now read back ALL entries from BOTH instances and check for corruption.
  // Use a fresh third instance to avoid any caching artifacts.
  PageSpeedCacheConfig config3;
  config3.volume_path = cache_path_;
  config3.volume_size = 2ULL * 1024 * 1024 * 1024;
  config3.enable_checksum = true;
  config3.verify_checksum_on_read = true;
  config3.ram_cache_size = 0;
  config3.multi_process.enabled = true;
  config3.multi_process.process_index = 0;
  config3.multi_process.total_processes = 1;

  auto cache3_result = PageSpeedCache::Create(config3);
  ASSERT_TRUE(cache3_result.has_value());
  auto cache3 = std::move(*cache3_result);

  int read_ok = 0, read_corrupt = 0, read_missing = 0;
  int content_mismatch = 0;
  std::map<int, int> error_counts;
  CapabilityMask default_mask;

  auto check_read = [&](const std::string& url, const std::string& expected) {
    auto r = cache3->ReadBestAlternate(url, hostname, "https", default_mask);
    if (!r.has_value()) {
      error_counts[static_cast<int>(r.error())]++;
      if (r.error() == cyclone::CacheError::Corrupted ||
          r.error() == cyclone::CacheError::ChainCorrupted) {
        ++read_corrupt;
      } else {
        ++read_missing;
      }
      return;
    }
    ++read_ok;
    auto content = r->content();
    if (content.size() != expected.size() ||
        std::memcmp(content.data(), expected.data(), expected.size()) != 0) {
      ++content_mismatch;
    }
  };

  for (int i = 0; i < kUrlsPerInstance; ++i) {
    check_read(urls1[i], data1[i]);
  }
  for (int i = 0; i < kUrlsPerInstance; ++i) {
    check_read(urls2[i], data2[i]);
  }

  printf(
      "Readback: %d ok, %d corrupt, %d missing, %d content-mismatch "
      "(of %d total)\n",
      read_ok, read_corrupt, read_missing, content_mismatch,
      kUrlsPerInstance * 2);
  for (auto& [err, cnt] : error_counts) {
    printf("  Error %d: %d entries\n", err, cnt);
  }

  EXPECT_EQ(read_corrupt, 0)
      << "CRC/checksum corruption detected! " << read_corrupt
      << " entries corrupted out of " << (kUrlsPerInstance * 2);
  EXPECT_EQ(content_mismatch, 0)
      << "Content mismatch (silent corruption)! " << content_mismatch
      << " entries had wrong data";
  EXPECT_EQ(read_missing, 0) << "Entries missing: " << read_missing;
}

// Test 7: Same as Test 6 but with variant writes — instance 1 writes
// originals while instance 2 writes variants for the SAME URLs.
// This is the exact production pattern.
TEST_F(CacheBurstTest, CrossInstanceOriginalAndVariantWriteCorruption) {
  CreateCache();

  const std::string hostname = "example.com";

  // Open second cache instance.
  PageSpeedCacheConfig config2;
  config2.volume_path = cache_path_;
  config2.volume_size = 2ULL * 1024 * 1024 * 1024;
  config2.enable_checksum = true;
  config2.verify_checksum_on_read = true;
  config2.ram_cache_size = 0;
  config2.multi_process.enabled = true;
  config2.multi_process.process_index = 0;
  config2.multi_process.total_processes = 1;

  auto cache2_result = PageSpeedCache::Create(config2);
  ASSERT_TRUE(cache2_result.has_value());
  auto cache2 = std::move(*cache2_result);

  constexpr int kNumUrls = 20;
  constexpr size_t kPayloadSize = static_cast<const size_t>(16 * 1024);

  std::vector<std::string> urls;
  std::vector<std::string> original_data;
  for (int i = 0; i < kNumUrls; ++i) {
    urls.push_back("/shared/img_" + std::to_string(i) + ".jpg");
    original_data.push_back(MakeRandomData(kPayloadSize, /*seed=*/7000 + i));
  }

  auto variant_masks = BuildVariantMasks();

  // Instance 1 (nginx) writes originals while instance 2 (worker) writes
  // variants for URLs that already have originals.
  // First write half the originals so the worker has something to build on.
  for (int i = 0; i < kNumUrls / 2; ++i) {
    WriteOriginal(urls[i], hostname, original_data[i]);
  }

  std::atomic<int> nginx_writes{0}, worker_writes{0};
  std::atomic<int> nginx_fails{0}, worker_fails{0};

  // Instance 1: continue writing originals for the remaining URLs.
  std::thread nginx_writer([&]() {
    CapabilityMask default_mask;
    auto id = MaskToId(default_mask);
    AlternateMetadata meta;
    meta.full_mask = default_mask.Encode();
    meta.content_type = ContentType::kImage;
    meta.origin_content_type = "image/jpeg";

    for (int i = kNumUrls / 2; i < kNumUrls; ++i) {
      auto wh = cache_->WriteAlternate(urls[i], hostname, "https", id,
                                       original_data[i].size(), meta);
      if (!wh) {
        nginx_fails.fetch_add(1, std::memory_order_relaxed);
        continue;
      }
      auto bytes = std::as_bytes(
          std::span(original_data[i].data(), original_data[i].size()));
      auto written = wh->write_sync(bytes);
      if (!written || !wh->close_sync()) {
        nginx_fails.fetch_add(1, std::memory_order_relaxed);
        continue;
      }
      nginx_writes.fetch_add(1, std::memory_order_relaxed);
    }
  });

  // Instance 2: write variants for the first half of URLs (already have
  // originals).
  constexpr int kWorkerThreads = 4;
  std::vector<std::thread> worker_threads;
  worker_threads.reserve(kWorkerThreads);
  for (int t = 0; t < kWorkerThreads; ++t) {
    worker_threads.emplace_back([&, t]() {
      for (int u = t; u < kNumUrls / 2; u += kWorkerThreads) {
        for (size_t v = 0; v < variant_masks.size(); ++v) {
          std::string data = MakeRandomData(static_cast<size_t>(8 * 1024),
                                            /*seed=*/8000 + u * 100 + v);
          if (WriteVariant(cache2.get(), urls[u], hostname, variant_masks[v],
                           data)) {
            worker_writes.fetch_add(1, std::memory_order_relaxed);
          } else {
            worker_fails.fetch_add(1, std::memory_order_relaxed);
          }
        }
      }
    });
  }

  nginx_writer.join();
  for (auto& t : worker_threads) t.join();

  printf("Concurrent writes: nginx=%d ok/%d fail, worker=%d ok/%d fail\n",
         nginx_writes.load(), nginx_fails.load(), worker_writes.load(),
         worker_fails.load());

  // Read back with a fresh instance.
  PageSpeedCacheConfig config3;
  config3.volume_path = cache_path_;
  config3.volume_size = 2ULL * 1024 * 1024 * 1024;
  config3.enable_checksum = true;
  config3.verify_checksum_on_read = true;
  config3.ram_cache_size = 0;
  config3.multi_process.enabled = true;
  config3.multi_process.process_index = 0;
  config3.multi_process.total_processes = 1;

  auto cache3_result = PageSpeedCache::Create(config3);
  ASSERT_TRUE(cache3_result.has_value());
  auto cache3 = std::move(*cache3_result);

  int read_ok = 0, read_corrupt = 0, read_missing = 0;
  int content_mismatch = 0;
  CapabilityMask default_mask;

  for (int i = 0; i < kNumUrls; ++i) {
    auto r =
        cache3->ReadBestAlternate(urls[i], hostname, "https", default_mask);
    if (!r.has_value()) {
      if (r.error() == cyclone::CacheError::Corrupted ||
          r.error() == cyclone::CacheError::ChainCorrupted) {
        ++read_corrupt;
      } else {
        ++read_missing;
      }
      continue;
    }
    ++read_ok;
    auto content = r->content();
    if (content.size() != original_data[i].size() ||
        std::memcmp(content.data(), original_data[i].data(),
                    original_data[i].size()) != 0) {
      ++content_mismatch;
    }
  }

  printf(
      "Readback: %d ok, %d corrupt, %d missing, %d content-mismatch "
      "(of %d)\n",
      read_ok, read_corrupt, read_missing, content_mismatch, kNumUrls);

  EXPECT_EQ(read_corrupt, 0) << "CRC corruption from cross-instance writes!";
  EXPECT_EQ(content_mismatch, 0) << "Silent data corruption!";
}

}  // namespace
}  // namespace pagespeed
