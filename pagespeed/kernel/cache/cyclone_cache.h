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

#include <memory>

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

    // Small-object tier carve-out percentage (0 = disabled).
    // When > 0, that percentage of cache_size_bytes is carved out into a
    // physically separate small-object volume at "<cache_path>.small" that
    // only small_tier_view() operations route to, so payload churn on the
    // default volume can never evict small-tier entries.  If the total size
    // cannot host both volumes' sizing floors (~256 MB single-process), the
    // tier silently disables itself and small-tier operations fall back to
    // default routing -- check small_tier_active().
    int small_tier_percent;

    Config()
        : cache_size_bytes(100 * 1024 * 1024),  // 100 MB default
          ram_cache_size_bytes(0),              // No RAM cache by default
          enable_checksum(true),
          num_segments(0),
          persist_directory(true),
          small_tier_percent(0) {}
  };

  // Statistics variable names.  kRamHits/kDiskHits split kHits by serving
  // tier.  Like the other CycloneCache variables, they are only incremented
  // on the global statistics object, so they are visible in the global view
  // but stay 0 in per-vhost split statistics.
  static const char kHits[];
  static const char kMisses[];
  static const char kRamHits[];
  static const char kDiskHits[];
  static const char kInserts[];
  static const char kDeletes[];
  static const char kFailures[];
  static const char kBytesRead[];
  static const char kBytesWritten[];

  // Creates a CycloneCache with the given configuration.
  // The cache is started during construction.
  CycloneCache(const Config& config, Statistics* statistics,
               MessageHandler* handler);

  ~CycloneCache() override;

  // Registers statistics variables used by this cache.
  // Must be called once during server initialization.
  static void InitStats(Statistics* statistics);

  // Returns a formatted name for this cache type.
  static GoogleString FormatName() { return "CycloneCache"; }

  // Name used by the small-tier view returned from small_tier_view().
  static GoogleString FormatSmallTierName() {
    return "CycloneCache(small_tier)";
  }

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

  // Appends Cyclone's internal counters (tier hits, sizes, eviction,
  // write-buffer wrap and the design record lease telemetry) to *out as
  // "Label: value" lines.  No-op when the cache is unavailable.
  void PrintStats(GoogleString* out) const;

  // Returns the configuration used to create this cache.
  const Config& config() const { return config_; }

  // True when the dedicated small-object volume exists.  False either
  // because config().small_tier_percent is 0 or because cache_size_bytes was
  // below the sizing floor and the tier was silently disabled (small-tier
  // operations then fall back to default routing).
  bool small_tier_active() const;

  // Returns a lightweight CacheInterface view over this same cache whose
  // Get/Put/Delete route to the small-object tier.  The view is owned by
  // this CycloneCache and shares its handle, statistics, and lifetime;
  // Backend() on the view returns this CycloneCache.  Safe to use even when
  // the small tier is disabled or inactive (operations fall back to default
  // routing inside Cyclone).
  CacheInterface* small_tier_view();

 private:
  class SmallTierView;

  // Shared implementations for both tiers.  |small_tier| selects
  // CYCLONE_TIER_SMALL routing.
  void GetWithTier(const GoogleString& key, bool small_tier,
                   Callback* callback);
  void PutWithTier(const GoogleString& key, const SharedString& value,
                   bool small_tier);
  void DeleteWithTier(const GoogleString& key, bool small_tier);

  Config config_;
  CycloneCacheHandle* cache_;  // C handle from wrapper
  MessageHandler* handler_;
  bool is_shut_down_;

  // Statistics variables
  Variable* hits_;
  Variable* misses_;
  Variable* ram_hits_;
  Variable* disk_hits_;
  Variable* inserts_;
  Variable* deletes_;
  Variable* failures_;
  Variable* bytes_read_;
  Variable* bytes_written_;

  std::unique_ptr<CacheInterface> small_tier_view_;

  CycloneCache(const CycloneCache&) = delete;
  CycloneCache& operator=(const CycloneCache&) = delete;
};

}  // namespace net_instaweb

#endif  // PAGESPEED_KERNEL_CACHE_CYCLONE_CACHE_H_
