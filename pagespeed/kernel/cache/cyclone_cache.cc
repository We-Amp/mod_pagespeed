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

#include <cstdio>
#include <sys/stat.h>

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

}  // namespace

// Statistics variable names
const char CycloneCache::kHits[] = "cyclone_cache_hits";
const char CycloneCache::kMisses[] = "cyclone_cache_misses";
const char CycloneCache::kInserts[] = "cyclone_cache_inserts";
const char CycloneCache::kDeletes[] = "cyclone_cache_deletes";
const char CycloneCache::kFailures[] = "cyclone_cache_failures";
const char CycloneCache::kBytesRead[] = "cyclone_cache_bytes_read";
const char CycloneCache::kBytesWritten[] = "cyclone_cache_bytes_written";

void CycloneCache::InitStats(Statistics* statistics) {
  statistics->AddVariable(kHits);
  statistics->AddVariable(kMisses);
  statistics->AddVariable(kInserts);
  statistics->AddVariable(kDeletes);
  statistics->AddVariable(kFailures);
  statistics->AddVariable(kBytesRead);
  statistics->AddVariable(kBytesWritten);
}

CycloneCache::CycloneCache(const Config& config,
                           Statistics* statistics,
                           MessageHandler* handler)
    : config_(config),
      cache_(nullptr),
      handler_(handler),
      is_shut_down_(false) {
  // Initialize statistics variables
  hits_ = statistics->GetVariable(kHits);
  misses_ = statistics->GetVariable(kMisses);
  inserts_ = statistics->GetVariable(kInserts);
  deletes_ = statistics->GetVariable(kDeletes);
  failures_ = statistics->GetVariable(kFailures);
  bytes_read_ = statistics->GetVariable(kBytesRead);
  bytes_written_ = statistics->GetVariable(kBytesWritten);

  // Remove stale 0-byte cache file from a previous failed initialization.
  // A 0-byte file is not a valid Cyclone cache and will cause start() to fail.
  struct stat st;
  if (stat(config_.cache_path.c_str(), &st) == 0 && st.st_size == 0) {
    handler_->Message(kInfo, "CycloneCache: Removing stale 0-byte cache file %s",
                      config_.cache_path.c_str());
    if (std::remove(config_.cache_path.c_str()) != 0) {
      handler_->Message(kWarning,
                        "CycloneCache: Failed to remove stale cache file %s",
                        config_.cache_path.c_str());
    }
  }

  // Create the Cyclone cache configuration
  CycloneCacheConfig c_config;
  c_config.cache_path = config_.cache_path.c_str();
  c_config.cache_size_bytes = config_.cache_size_bytes;
  c_config.ram_cache_size_bytes = config_.ram_cache_size_bytes;
  c_config.enable_checksum = config_.enable_checksum ? 1 : 0;
  c_config.num_segments = config_.num_segments;

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

  handler_->Message(kInfo, "CycloneCache: Started cache at %s "
                    "(size=%lld bytes, ram_cache=%lld bytes)",
                    config_.cache_path.c_str(),
                    static_cast<long long>(config_.cache_size_bytes),
                    static_cast<long long>(config_.ram_cache_size_bytes));
}

CycloneCache::~CycloneCache() {
  ShutDown();
}

void CycloneCache::Get(const GoogleString& key, Callback* callback) {
  if (is_shut_down_ || cache_ == nullptr) {
    ValidateAndReportResult(key, kNotFound, callback);
    return;
  }

  CycloneReadHandle* read_handle = nullptr;
  CycloneError err = cyclone_cache_read(
      cache_, key.data(), key.size(), &read_handle);

  if (err == CYCLONE_OK && read_handle != nullptr) {
    // Cache hit - get the data
    size_t size = cyclone_read_handle_size(read_handle);

    // Check if zero-copy mmap'd path is available
    if (cyclone_read_handle_has_mapped_data(read_handle)) {
      // Zero-copy path: create a MappedSharedString that holds a reference
      // to the read handle. The handle will be released when all references
      // to the MappedSharedString are gone.
      const char* mapped_data = cyclone_read_handle_mapped_data(read_handle);

      // Increment refcount since MappedSharedString will take ownership
      cyclone_read_handle_ref(read_handle);

      MappedSharedString mapped_value = MappedSharedString::FromMappedView(
          mapped_data, size, ReleaseReadHandle, read_handle);
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
    bytes_read_->Add(static_cast<int64>(size));

    ValidateAndReportResult(key, kAvailable, callback);
  } else {
    // Cache miss
    misses_->Add(1);
    ValidateAndReportResult(key, kNotFound, callback);
  }
}

void CycloneCache::Put(const GoogleString& key, const SharedString& value) {
  if (is_shut_down_ || cache_ == nullptr) {
    failures_->Add(1);
    return;
  }

  StringPiece data = value.Value();
  CycloneError err = cyclone_cache_write(
      cache_, key.data(), key.size(), data.data(), data.size());

  if (err == CYCLONE_OK) {
    inserts_->Add(1);
    bytes_written_->Add(static_cast<int64>(data.size()));
  } else {
    failures_->Add(1);
    const char* error = cyclone_get_last_error();
    handler_->Message(kWarning, "CycloneCache: Write failed for key %s: %s",
                      key.c_str(),
                      error ? error : "unknown error");
  }
}

void CycloneCache::Delete(const GoogleString& key) {
  if (is_shut_down_ || cache_ == nullptr) {
    return;
  }

  CycloneError err = cyclone_cache_delete(cache_, key.data(), key.size());
  if (err == CYCLONE_OK || err == CYCLONE_NOT_FOUND) {
    // Both success and not-found are considered successful deletes
    deletes_->Add(1);
  }
  // Note: We don't log failures for delete operations as they are
  // typically best-effort.
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
