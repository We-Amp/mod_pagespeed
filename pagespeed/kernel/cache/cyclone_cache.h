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

#ifndef PAGESPEED_KERNEL_CACHE_CYCLONE_CACHE_H_
#define PAGESPEED_KERNEL_CACHE_CYCLONE_CACHE_H_

#include "pagespeed/kernel/base/basictypes.h"
#include "pagespeed/kernel/base/cache_interface.h"
#include "pagespeed/kernel/base/string.h"

// Forward declare the C handle type from the wrapper
struct CycloneCacheHandle;

namespace net_instaweb {

class MessageHandler;
class Statistics;
class Variable;

// CycloneCache implements CacheInterface using the Cyclone disk cache library.
//
// Cyclone is a high-performance disk cache that uses the scan-resistant
// CLFUS (Clock with Low Inter-reference Recency First Unset Second chance)
// algorithm. It provides memory-mapped I/O for zero-copy reads and includes
// an integrated RAM cache for hot data.
//
// This implementation uses a C wrapper to bridge the C++23 Cyclone library
// to the C++20 PageSpeed codebase.
//
// Features:
//   - Scan-resistant eviction algorithm
//   - Memory-mapped I/O for efficient reads
//   - Built-in RAM cache (optional)
//   - CRC32 checksums for data integrity (optional)
//   - Concurrent access support
//
// Usage:
//   CycloneCache::Config config;
//   config.cache_path = "/var/cache/pagespeed/cyclone.dat";
//   config.cache_size_bytes = 100 * 1024 * 1024;  // 100 MB
//   config.ram_cache_size_bytes = 10 * 1024 * 1024;  // 10 MB
//   config.enable_checksum = true;
//
//   CycloneCache cache(config, statistics, message_handler);
//   if (cache.IsHealthy()) {
//     // Cache is ready to use
//   }
class CycloneCache : public CacheInterface {
 public:
  // Configuration for the Cyclone cache.
  struct Config {
    // Path to the cache data file (required).
    // The parent directory must exist and be writable.
    GoogleString cache_path;

    // Maximum size of the disk cache in bytes.
    // The cache will evict entries to stay within this limit.
    int64 cache_size_bytes;

    // Size of the in-memory RAM cache in bytes.
    // Set to 0 to disable the RAM cache layer.
    // The RAM cache provides faster access for frequently accessed data.
    int64 ram_cache_size_bytes;

    // Enable CRC32 checksums for data integrity verification.
    // This adds some overhead but catches data corruption.
    bool enable_checksum;

    // Number of cache segments for concurrency.
    // More segments allow more concurrent operations but use more memory.
    // Set to 0 for the default value.
    int num_segments;

    // Enable persistent directory for cache data to survive process restarts.
    // When true, the directory (index) is mmap'd into the data file so that
    // previously written entries are still reachable after a restart.
    bool persist_directory;

    Config()
        : cache_size_bytes(100 * 1024 * 1024),  // 100 MB default
          ram_cache_size_bytes(0),               // No RAM cache by default
          enable_checksum(true),
          num_segments(0),
          persist_directory(true) {}
  };

  // Statistics variable names.
  static const char kHits[];
  static const char kMisses[];
  static const char kInserts[];
  static const char kDeletes[];
  static const char kFailures[];
  static const char kBytesRead[];
  static const char kBytesWritten[];

  // Creates a CycloneCache with the given configuration.
  // The cache is started during construction.
  CycloneCache(const Config& config,
               Statistics* statistics,
               MessageHandler* handler);

  ~CycloneCache() override;

  // Registers statistics variables used by this cache.
  // Must be called once during server initialization.
  static void InitStats(Statistics* statistics);

  // Returns a formatted name for this cache type.
  static GoogleString FormatName() { return "CycloneCache"; }

  // CacheInterface implementation.
  void Get(const GoogleString& key, Callback* callback) override;
  void Put(const GoogleString& key, const SharedString& value) override;
  void Delete(const GoogleString& key) override;

  GoogleString Name() const override { return FormatName(); }

  // CycloneCache is a blocking cache - callbacks are invoked before
  // Get/Put/Delete return.
  bool IsBlocking() const override { return true; }

  // Returns true if the cache was successfully initialized and is running.
  bool IsHealthy() const override;

  // Stops the cache. After this call, all operations will fail.
  void ShutDown() override;

  // Returns the configuration used to create this cache.
  const Config& config() const { return config_; }

 private:
  Config config_;
  CycloneCacheHandle* cache_;  // C handle from wrapper
  MessageHandler* handler_;
  bool is_shut_down_;

  // Statistics variables
  Variable* hits_;
  Variable* misses_;
  Variable* inserts_;
  Variable* deletes_;
  Variable* failures_;
  Variable* bytes_read_;
  Variable* bytes_written_;

  CycloneCache(const CycloneCache&) = delete;
  CycloneCache& operator=(const CycloneCache&) = delete;
};

}  // namespace net_instaweb

#endif  // PAGESPEED_KERNEL_CACHE_CYCLONE_CACHE_H_
