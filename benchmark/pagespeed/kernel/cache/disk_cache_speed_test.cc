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

// Throughput benchmark for CycloneCache.
//
// Run with:
//   bazel run --config=clang-libstdcxx13 //benchmark/pagespeed/kernel/cache:cache
//
// Each benchmark pre-generates random keys and payloads, then times the
// cache operations. Results are printed as ops/sec and MB/s.

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <vector>
#include <memory>

#include "base/logging.h"
#include "pagespeed/kernel/base/basictypes.h"
#include "pagespeed/kernel/base/cache_interface.h"
#include "pagespeed/kernel/base/null_message_handler.h"
#include "pagespeed/kernel/base/null_mutex.h"
#include "pagespeed/kernel/base/shared_string.h"
#include "pagespeed/kernel/base/string.h"
#include "pagespeed/kernel/base/string_util.h"
#include "pagespeed/kernel/cache/cyclone_cache.h"
#include "pagespeed/kernel/cache/lru_cache.h"
#include "pagespeed/kernel/cache/write_through_cache.h"
#include "pagespeed/kernel/thread/pthread_thread_system.h"
#include "pagespeed/kernel/util/simple_random.h"
#include "pagespeed/kernel/util/simple_stats.h"
#include "test/pagespeed/kernel/base/gtest.h"

namespace net_instaweb {
namespace {

const int kNumKeys = 10000;  // Reduced for faster benchmarks
const int kKeySize = 50;
const int kSmallPayloadSize = 100;
const int kLargePayloadSize = 10240;

class SilentCallback : public CacheInterface::Callback {
 public:
  void Done(CacheInterface::KeyState state) override {}
};

// RAII temp directory.
class TempDir {
 public:
  TempDir() {
    char tmpl[] = "/tmp/disk_cache_bench_XXXXXX";
    char* dir = mkdtemp(tmpl);
    CHECK(dir != nullptr);
    path_ = dir;
  }
  ~TempDir() {
    // Clean up temp directory. Use system() as std::filesystem can cause issues.
    std::string cmd = "rm -rf " + path_;
    (void)system(cmd.c_str());
  }
  const GoogleString& path() const { return path_; }

 private:
  GoogleString path_;
};

// Pre-generated random keys and payloads.
struct TestData {
  std::vector<GoogleString> keys;
  std::vector<SharedString> values;
  int payload_size;

  TestData(int num_keys, int key_size, int psize) : payload_size(psize) {
    SimpleRandom random(new NullMutex);
    keys.resize(num_keys);
    values.resize(num_keys);
    GoogleString key_prefix = random.GenerateHighEntropyString(key_size);
    GoogleString value_prefix = random.GenerateHighEntropyString(psize);
    for (int k = 0; k < num_keys; ++k) {
      keys[k] = StrCat(key_prefix, "_", IntegerToString(k));
      GoogleString val = StrCat(value_prefix, "_", IntegerToString(k));
      values[k].SwapWithString(&val);
    }
  }

  int64 total_bytes() const {
    return static_cast<int64>(keys.size()) * payload_size;
  }
};

// Helpers for cache operations.
void DoPuts(CacheInterface* cache, const TestData& data) {
  for (size_t k = 0; k < data.keys.size(); ++k) {
    cache->Put(data.keys[k], data.values[k]);
  }
}

void DoGets(CacheInterface* cache, const TestData& data) {
  SilentCallback cb;
  for (size_t k = 0; k < data.keys.size(); ++k) {
    cache->Get(data.keys[k], &cb);
  }
}

void DoDeletes(CacheInterface* cache, const TestData& data) {
  for (size_t k = 0; k < data.keys.size(); ++k) {
    cache->Delete(data.keys[k]);
  }
}

// Timing helper. Returns elapsed seconds.
double TimeOp(std::function<void()> fn) {
  auto start = std::chrono::steady_clock::now();
  fn();
  auto end = std::chrono::steady_clock::now();
  return std::chrono::duration<double>(end - start).count();
}

void PrintResult(const char* label, int num_ops, int64 bytes, double secs) {
  double ops_per_sec = num_ops / secs;
  double mb_per_sec = (bytes / (1024.0 * 1024.0)) / secs;
  printf("  %-30s %8.0f ops/s  %8.1f MB/s  (%.3fs)\n", label, ops_per_sec,
         mb_per_sec, secs);
}

struct CycloneCacheFactory {
  TempDir dir;
  PthreadThreadSystem thread_system;
  SimpleStats stats;
  NullMessageHandler handler;
  std::unique_ptr<CycloneCache> cache;

  CycloneCacheFactory() : stats(&thread_system) {
    CycloneCache::InitStats(&stats);
    CycloneCache::Config config;
    config.cache_path = StrCat(dir.path(), "/cyclone.dat");
    config.cache_size_bytes = int64{1} << 30;
    config.ram_cache_size_bytes = 0;
    config.enable_checksum = true;
    config.num_segments = 0;
    cache = std::make_unique<CycloneCache>(config, &stats, &handler);
    CHECK(cache->IsHealthy()) << "CycloneCache failed to start";
  }
  CacheInterface* get() { return cache.get(); }
  static const char* name() { return "CycloneCache"; }
};

// CycloneCache with RAM cache layer enabled.
struct CycloneCacheWithRamFactory {
  TempDir dir;
  PthreadThreadSystem thread_system;
  SimpleStats stats;
  NullMessageHandler handler;
  std::unique_ptr<CycloneCache> cache;

  CycloneCacheWithRamFactory() : stats(&thread_system) {
    CycloneCache::InitStats(&stats);
    CycloneCache::Config config;
    config.cache_path = StrCat(dir.path(), "/cyclone.dat");
    config.cache_size_bytes = int64{1} << 30;
    config.ram_cache_size_bytes = int64{64} << 20;  // 64MB RAM cache
    config.enable_checksum = true;
    config.num_segments = 0;
    cache = std::make_unique<CycloneCache>(config, &stats, &handler);
    CHECK(cache->IsHealthy()) << "CycloneCache failed to start";
  }
  CacheInterface* get() { return cache.get(); }
  static const char* name() { return "CycloneCache+RAM"; }
};

template <typename CacheFactoryT>
void RunBenchmarkSuite(int payload_size) {
  TestData data(kNumKeys, kKeySize, payload_size);
  TestData miss_data(kNumKeys, kKeySize, payload_size);
  const int n = kNumKeys;
  const int64 bytes = data.total_bytes();
  const char* cache_name = CacheFactoryT::name();

  printf("\n--- %s (payload=%d bytes, keys=%d) ---\n", cache_name,
         payload_size, n);

  // Puts into a fresh cache
  {
    CacheFactoryT factory;
    double secs = TimeOp([&] { DoPuts(factory.get(), data); });
    PrintResult("Puts", n, bytes, secs);
  }

  // Gets (all hits)
  {
    CacheFactoryT factory;
    DoPuts(factory.get(), data);
    double secs = TimeOp([&] { DoGets(factory.get(), data); });
    PrintResult("Gets (hits)", n, bytes, secs);
  }

  // Gets (all misses)
  {
    CacheFactoryT factory;
    DoPuts(factory.get(), data);
    double secs = TimeOp([&] { DoGets(factory.get(), miss_data); });
    PrintResult("Gets (misses)", n, 0, secs);
  }

  // Overwrites
  {
    CacheFactoryT factory;
    DoPuts(factory.get(), data);
    double secs = TimeOp([&] { DoPuts(factory.get(), data); });
    PrintResult("Overwrites", n, bytes, secs);
  }

  // Deletes
  {
    CacheFactoryT factory;
    DoPuts(factory.get(), data);
    double secs = TimeOp([&] { DoDeletes(factory.get(), data); });
    PrintResult("Deletes", n, 0, secs);
  }
}

TEST(DiskCacheBenchmark, SmallPayload) {
  printf("\n========== Disk Cache Benchmark (small payload) ==========\n");
  RunBenchmarkSuite<CycloneCacheFactory>(kSmallPayloadSize);
  RunBenchmarkSuite<CycloneCacheWithRamFactory>(kSmallPayloadSize);
}

TEST(DiskCacheBenchmark, LargePayload) {
  printf("\n========== Disk Cache Benchmark (large payload) ==========\n");
  RunBenchmarkSuite<CycloneCacheFactory>(kLargePayloadSize);
  RunBenchmarkSuite<CycloneCacheWithRamFactory>(kLargePayloadSize);
}

}  // namespace
}  // namespace net_instaweb
