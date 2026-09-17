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

#include "pagespeed/kernel/cache/cyclone_cache.h"

#include <sys/stat.h>

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <memory>

#include "pagespeed/kernel/base/mapped_shared_string.h"
#include "pagespeed/kernel/base/message_handler.h"
#include "pagespeed/kernel/base/statistics.h"
#include "pagespeed/kernel/base/string_util.h"
#include "pagespeed/kernel/cache/cyclone/cyclone_wrapper.h"

namespace net_instaweb {

namespace {

// Release callback for MappedSharedString that unrefs the Cyclone read handle.
// This is called when the last reference to the mapped value is released.
void ReleaseReadHandle(void* user_data) {
  CycloneReadHandle* handle = static_cast<CycloneReadHandle*>(user_data);
  cyclone_read_handle_unref(handle);
}

// Lease hooks threaded to the zero-copy embedder via
// MappedSharedString (user_data is the same CycloneReadHandle*).
int RenewReadHandleLease(void* user_data) {
  return cyclone_read_handle_renew_lease(
      static_cast<CycloneReadHandle*>(user_data));
}
int RenewReadHandleLeaseStrict(void* user_data) {
  return cyclone_read_handle_renew_lease_strict(
      static_cast<CycloneReadHandle*>(user_data));
}
uint64_t ReadHandleNsUntilForcedWrap(void* user_data) {
  return cyclone_read_handle_ns_until_forced_wrap(
      static_cast<CycloneReadHandle*>(user_data));
}

}  // namespace

// Lightweight CacheInterface view over an owning CycloneCache that routes
// every operation to the small-object tier.  Shares the owner's C handle and
// statistics; holds no state of its own.  When the small tier is disabled or
// inactive, Cyclone falls back to default routing, so the view is always
// safe to use.
class CycloneCache::SmallTierView : public CacheInterface {
 public:
  explicit SmallTierView(CycloneCache* owner) : owner_(owner) {}

  void Get(const GoogleString& key, Callback* callback) override {
    owner_->GetWithTier(key, true /* small_tier */, callback);
  }
  void Put(const GoogleString& key, const SharedString& value) override {
    owner_->PutWithTier(key, value, true /* small_tier */);
  }
  void Delete(const GoogleString& key) override {
    owner_->DeleteWithTier(key, true /* small_tier */);
  }

  GoogleString Name() const override {
    return CycloneCache::FormatSmallTierName();
  }
  CacheInterface* Backend() override { return owner_; }
  bool IsBlocking() const override { return true; }
  bool IsHealthy() const override { return owner_->IsHealthy(); }
  void ShutDown() override { owner_->ShutDown(); }

 private:
  CycloneCache* owner_;

  SmallTierView(const SmallTierView&) = delete;
  SmallTierView& operator=(const SmallTierView&) = delete;
};

// Statistics variable names
const char CycloneCache::kHits[] = "cyclone_cache_hits";
const char CycloneCache::kMisses[] = "cyclone_cache_misses";
const char CycloneCache::kRamHits[] = "cyclone_cache_ram_hits";
const char CycloneCache::kDiskHits[] = "cyclone_cache_disk_hits";
const char CycloneCache::kInserts[] = "cyclone_cache_inserts";
const char CycloneCache::kDeletes[] = "cyclone_cache_deletes";
const char CycloneCache::kFailures[] = "cyclone_cache_failures";
const char CycloneCache::kBytesRead[] = "cyclone_cache_bytes_read";
const char CycloneCache::kBytesWritten[] = "cyclone_cache_bytes_written";

void CycloneCache::InitStats(Statistics* statistics) {
  statistics->AddVariable(kHits);
  statistics->AddVariable(kMisses);
  statistics->AddVariable(kRamHits);
  statistics->AddVariable(kDiskHits);
  statistics->AddVariable(kInserts);
  statistics->AddVariable(kDeletes);
  statistics->AddVariable(kFailures);
  statistics->AddVariable(kBytesRead);
  statistics->AddVariable(kBytesWritten);
}

CycloneCache::CycloneCache(const Config& config, Statistics* statistics,
                           MessageHandler* handler)
    : config_(config),
      cache_(nullptr),
      handler_(handler),
      is_shut_down_(false) {
  // Initialize statistics variables
  hits_ = statistics->GetVariable(kHits);
  misses_ = statistics->GetVariable(kMisses);
  ram_hits_ = statistics->GetVariable(kRamHits);
  disk_hits_ = statistics->GetVariable(kDiskHits);
  inserts_ = statistics->GetVariable(kInserts);
  deletes_ = statistics->GetVariable(kDeletes);
  failures_ = statistics->GetVariable(kFailures);
  bytes_read_ = statistics->GetVariable(kBytesRead);
  bytes_written_ = statistics->GetVariable(kBytesWritten);

  // Construct the small-tier view eagerly: small_tier_view() may be called
  // from multiple threads, so there must be no lazy (unsynchronized) init.
  // The view is safe even when cache creation fails below -- its operations
  // check cache_ like the primary interface does.
  small_tier_view_ = std::make_unique<SmallTierView>(this);

  // Remove stale 0-byte cache files from a previous failed initialization.
  // A 0-byte file is not a valid Cyclone cache and will cause start() to
  // fail.  Check the small-object tier's sidecar volume ("<path>.small")
  // too: a crash during first-init could leave a 0-byte sidecar that would
  // otherwise wedge startup into the LRU fallback until manually cleared.
  for (const GoogleString& stale_candidate :
       {config_.cache_path, StrCat(config_.cache_path, ".small")}) {
    struct stat st;
    if (stat(stale_candidate.c_str(), &st) == 0 && st.st_size == 0) {
      handler_->Message(kInfo,
                        "CycloneCache: Removing stale 0-byte cache file %s",
                        stale_candidate.c_str());
      if (std::remove(stale_candidate.c_str()) != 0) {
        handler_->Message(kWarning,
                          "CycloneCache: Failed to remove stale cache file %s",
                          stale_candidate.c_str());
      }
    }
  }

  // Create the Cyclone cache configuration
  CycloneCacheConfig c_config;
  c_config.cache_path = config_.cache_path.c_str();
  c_config.cache_size_bytes = config_.cache_size_bytes;
  c_config.ram_cache_size_bytes = config_.ram_cache_size_bytes;
  c_config.enable_checksum = config_.enable_checksum ? 1 : 0;
  c_config.num_segments = config_.num_segments;
  c_config.persist_directory = config_.persist_directory ? 1 : 0;
  c_config.small_tier_percent =
      config_.small_tier_percent > 0
          ? static_cast<uint32_t>(config_.small_tier_percent)
          : 0;

  // Create the cache
  cache_ = cyclone_cache_create(&c_config);
  if (cache_ == nullptr) {
    const char* error = cyclone_get_last_error();
    handler_->Message(kError, "CycloneCache: Failed to create cache at %s: %s",
                      config_.cache_path.c_str(),
                      error ? error : "unknown error");
    return;
  }

  // Start the cache
  CycloneError err = cyclone_cache_start(cache_);
  if (err != CYCLONE_OK) {
    const char* error = cyclone_get_last_error();
    handler_->Message(kError, "CycloneCache: Failed to start cache at %s: %s",
                      config_.cache_path.c_str(),
                      error ? error : "unknown error");
    cyclone_cache_destroy(cache_);
    cache_ = nullptr;
    return;
  }

  handler_->Message(kInfo,
                    "CycloneCache: Started cache at %s "
                    "(size=%lld bytes, ram_cache=%lld bytes)",
                    config_.cache_path.c_str(),
                    static_cast<long long>(config_.cache_size_bytes),
                    static_cast<long long>(config_.ram_cache_size_bytes));
}

CycloneCache::~CycloneCache() { ShutDown(); }

void CycloneCache::Get(const GoogleString& key, Callback* callback) {
  GetWithTier(key, false /* small_tier */, callback);
}

void CycloneCache::GetWithTier(const GoogleString& key, bool small_tier,
                               Callback* callback) {
  if (is_shut_down_ || cache_ == nullptr) {
    ValidateAndReportResult(key, kNotFound, callback);
    return;
  }

  CycloneReadHandle* read_handle = nullptr;
  CycloneError err = cyclone_cache_read_tier(
      cache_, key.data(), key.size(),
      small_tier ? CYCLONE_TIER_SMALL : CYCLONE_TIER_DEFAULT, &read_handle);

  if (err == CYCLONE_OK && read_handle != nullptr) {
    // Cache hit - get the data
    size_t size = cyclone_read_handle_size(read_handle);
    bool ram_hit = cyclone_read_handle_is_ram_hit(read_handle) != 0;

    // Check if zero-copy mmap'd path is available
    if (cyclone_read_handle_has_mapped_data(read_handle)) {
      // Zero-copy path: create a MappedSharedString that holds a reference
      // to the read handle. The handle will be released when all references
      // to the MappedSharedString are gone.
      const char* mapped_data = cyclone_read_handle_mapped_data(read_handle);

      // Increment refcount since MappedSharedString will take ownership
      cyclone_read_handle_ref(read_handle);

      MappedSharedString mapped_value = MappedSharedString::FromMappedView(
          mapped_data, size, ReleaseReadHandle, RenewReadHandleLease,
          RenewReadHandleLeaseStrict, ReadHandleNsUntilForcedWrap, read_handle);
      callback->set_value(mapped_value);

      // Close our reference to the read handle (decrements refcount).
      // The MappedSharedString still holds a reference.
      cyclone_read_handle_close(read_handle);
    } else {
      // Fall back to the copied data (e.g., RAM cache hits may not have mmap)
      const char* data = cyclone_read_handle_data(read_handle);
      SharedString value;
      value.Assign(StringPiece(data, size));
      callback->set_value(value);

      // Close the read handle
      cyclone_read_handle_close(read_handle);
    }

    // Update statistics
    hits_->Add(1);
    if (ram_hit) {
      ram_hits_->Add(1);
    } else {
      disk_hits_->Add(1);
    }
    bytes_read_->Add(static_cast<int64>(size));

    ValidateAndReportResult(key, kAvailable, callback);
  } else {
    // Cache miss
    misses_->Add(1);
    ValidateAndReportResult(key, kNotFound, callback);
  }
}

void CycloneCache::Put(const GoogleString& key, const SharedString& value) {
  PutWithTier(key, value, false /* small_tier */);
}

void CycloneCache::PutWithTier(const GoogleString& key,
                               const SharedString& value, bool small_tier) {
  if (is_shut_down_ || cache_ == nullptr) {
    failures_->Add(1);
    return;
  }

  StringPiece data = value.Value();
  CycloneError err = cyclone_cache_write_tier(
      cache_, key.data(), key.size(), data.data(), data.size(),
      small_tier ? CYCLONE_TIER_SMALL : CYCLONE_TIER_DEFAULT);

  if (err == CYCLONE_OK) {
    inserts_->Add(1);
    bytes_written_->Add(static_cast<int64>(data.size()));
  } else {
    failures_->Add(1);
    // A full or unwritable cache fails every write, so a per-failure warning
    // floods the log at request rate without adding signal.  Log the first
    // failure and every 1024th after that; the cyclone_cache_failures
    // statistic still counts each one.
    uint64_t n =
        write_failure_log_count_.fetch_add(1, std::memory_order_relaxed);
    if ((n & 1023) == 0) {
      const char* error = cyclone_get_last_error();
      handler_->Message(
          kWarning,
          "CycloneCache: Write failed for key %s: %s (failure #%llu on this "
          "cache; every failure is counted in cyclone_cache_failures, this "
          "warning is logged 1-in-1024)",
          key.c_str(), error ? error : "unknown error",
          static_cast<unsigned long long>(n) + 1);
    }
  }
}

void CycloneCache::Delete(const GoogleString& key) {
  DeleteWithTier(key, false /* small_tier */);
}

void CycloneCache::DeleteWithTier(const GoogleString& key, bool small_tier) {
  if (is_shut_down_ || cache_ == nullptr) {
    return;
  }

  CycloneError err = cyclone_cache_delete_tier(
      cache_, key.data(), key.size(),
      small_tier ? CYCLONE_TIER_SMALL : CYCLONE_TIER_DEFAULT);
  if (err == CYCLONE_OK || err == CYCLONE_NOT_FOUND) {
    // Both success and not-found are considered successful deletes
    deletes_->Add(1);
  }
  // Note: We don't log failures for delete operations as they are
  // typically best-effort.
}

bool CycloneCache::small_tier_active() const {
  if (is_shut_down_ || cache_ == nullptr) {
    return false;
  }
  return cyclone_cache_small_tier_active(cache_) != 0;
}

CacheInterface* CycloneCache::small_tier_view() {
  return small_tier_view_.get();
}

void CycloneCache::PrintStats(GoogleString* out) const {
  if (is_shut_down_ || cache_ == nullptr) {
    return;
  }
  // Zero-initialize: the struct is append-only across the vendored C ABI,
  // so fields a wrapper built against an older layout does not fill must
  // read 0, not garbage.
  CycloneCacheStats stats = {};
  cyclone_cache_get_stats(cache_, &stats);
  StrAppend(out, "Current entries: ",
            Integer64ToString(static_cast<int64>(stats.current_entries)), "\n");
  StrAppend(out, "Current size bytes: ",
            Integer64ToString(static_cast<int64>(stats.current_size_bytes)),
            "\n");
  StrAppend(out, "RAM cache bytes: ",
            Integer64ToString(static_cast<int64>(stats.ram_cache_bytes)), "\n");
  StrAppend(out, "RAM cache hits: ",
            Integer64ToString(static_cast<int64>(stats.ram_cache_hits)), "\n");
  StrAppend(out, "RAM cache misses: ",
            Integer64ToString(static_cast<int64>(stats.ram_cache_misses)),
            "\n");
  StrAppend(out, "Disk cache hits: ",
            Integer64ToString(static_cast<int64>(stats.disk_cache_hits)), "\n");
  StrAppend(out, "Disk cache misses: ",
            Integer64ToString(static_cast<int64>(stats.disk_cache_misses)),
            "\n");
  StrAppend(out, "Evictions: ",
            Integer64ToString(static_cast<int64>(stats.evictions)), "\n");
  StrAppend(out, "Write buffer wraps: ",
            Integer64ToString(static_cast<int64>(stats.write_buffer_wraps)),
            "\n");
  StrAppend(
      out, "Wraps deferred by lease: ",
      Integer64ToString(static_cast<int64>(stats.wraps_deferred_by_lease)),
      "\n");
  StrAppend(
      out, "Writes dropped by lease: ",
      Integer64ToString(static_cast<int64>(stats.writes_dropped_by_lease)),
      "\n");
  StrAppend(
      out, "Wraps forced past lease: ",
      Integer64ToString(static_cast<int64>(stats.wraps_forced_past_lease)),
      "\n");
  StrAppend(
      out, "Tag collision evictions: ",
      Integer64ToString(static_cast<int64>(stats.tag_collision_evictions)),
      "\n");
  StrAppend(out, "Bucket full evictions: ",
            Integer64ToString(static_cast<int64>(stats.bucket_full_evictions)),
            "\n");
  StrAppend(
      out, "Resets under degraded gate: ",
      Integer64ToString(static_cast<int64>(stats.resets_under_degraded_gate)),
      "\n");
  StrAppend(out, "Resets gate verified: ",
            Integer64ToString(static_cast<int64>(stats.resets_gate_verified)),
            "\n");
}

bool CycloneCache::IsHealthy() const {
  if (is_shut_down_ || cache_ == nullptr) {
    return false;
  }
  return cyclone_cache_is_running(cache_) != 0;
}

void CycloneCache::ShutDown() {
  if (is_shut_down_) {
    return;
  }
  is_shut_down_ = true;

  if (cache_ != nullptr) {
    cyclone_cache_stop(cache_);
    cyclone_cache_destroy(cache_);
    cache_ = nullptr;
    handler_->Message(kInfo, "CycloneCache: Shut down cache at %s",
                      config_.cache_path.c_str());
  }
}

}  // namespace net_instaweb
