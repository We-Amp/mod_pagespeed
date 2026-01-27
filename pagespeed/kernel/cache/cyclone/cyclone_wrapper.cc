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

// C++23 implementation of the Cyclone cache wrapper.
//
// This file bridges the C ABI defined in cyclone_wrapper.h to the
// C++23 Cyclone library. It is compiled with -std=c++23 while the
// rest of mod_pagespeed uses C++20.

#include "pagespeed/kernel/cache/cyclone/cyclone_wrapper.h"

#include <cstring>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "cyclone/cache.hpp"
#include "cyclone/config.hpp"
#include "cyclone/key.hpp"

// Thread-local storage for error messages.
// This allows each thread to have its own error message without synchronization.
thread_local std::string g_last_error;

// Internal structure that holds the actual Cyclone cache object.
// This is opaque to C code - they only see a CycloneCacheHandle*.
struct CycloneCacheHandle {
  std::unique_ptr<cyclone::Cache> impl;
  std::string cache_path;
  bool running = false;
};

// Internal structure that holds a read handle and the data copy.
// We copy the data to provide a stable pointer that remains valid
// until the handle is closed.
struct CycloneReadHandle {
  cyclone::ReadHandle handle;
  std::vector<char> data_copy;
};

// Helper to set the thread-local error message.
static void SetLastError(const std::string& msg) {
  g_last_error = msg;
}

// Helper to clear the thread-local error message.
static void ClearLastError() {
  g_last_error.clear();
}

extern "C" {

CycloneCacheHandle* cyclone_cache_create(const CycloneCacheConfig* config) {
  ClearLastError();

  if (!config || !config->cache_path) {
    SetLastError("Invalid configuration: cache_path is required");
    return nullptr;
  }

  auto handle = std::make_unique<CycloneCacheHandle>();
  handle->cache_path = config->cache_path;

  // Configure the cache
  cyclone::CacheConfig cc;
  cc.ram_cache_size = config->ram_cache_size_bytes;
  cc.enable_checksum = config->enable_checksum != 0;
  if (config->num_segments > 0) {
    cc.num_segments = config->num_segments;
  }

  // Create the cache instance
  auto result = cyclone::Cache::create(cc);
  if (!result) {
    SetLastError("Failed to create cache instance");
    return nullptr;
  }

  handle->impl = std::move(*result);

  // Add the volume (disk storage)
  cyclone::VolumeConfig vc;
  vc.path = config->cache_path;
  vc.size = config->cache_size_bytes;

  auto add_result = handle->impl->add_volume(vc);
  if (!add_result) {
    SetLastError("Failed to add volume to cache");
    return nullptr;
  }

  return handle.release();
}

void cyclone_cache_destroy(CycloneCacheHandle* cache) {
  if (cache) {
    if (cache->impl && cache->running) {
      cache->impl->stop();
    }
    delete cache;
  }
}

CycloneError cyclone_cache_start(CycloneCacheHandle* cache) {
  ClearLastError();

  if (!cache || !cache->impl) {
    SetLastError("Invalid cache handle");
    return CYCLONE_NOT_INITIALIZED;
  }

  if (cache->running) {
    return CYCLONE_OK;  // Already running
  }

  auto result = cache->impl->start();
  if (!result) {
    SetLastError("Failed to start cache");
    return CYCLONE_INTERNAL_ERROR;
  }

  cache->running = true;
  return CYCLONE_OK;
}

void cyclone_cache_stop(CycloneCacheHandle* cache) {
  if (cache && cache->impl && cache->running) {
    cache->impl->stop();
    cache->running = false;
  }
}

int cyclone_cache_is_running(const CycloneCacheHandle* cache) {
  return (cache && cache->impl && cache->running) ? 1 : 0;
}

CycloneError cyclone_cache_read(CycloneCacheHandle* cache,
                                const char* key, size_t key_len,
                                CycloneReadHandle** out_handle) {
  ClearLastError();

  if (!cache || !cache->impl || !cache->running) {
    SetLastError("Cache not initialized or not running");
    return CYCLONE_NOT_INITIALIZED;
  }

  if (!key || !out_handle) {
    SetLastError("Invalid arguments");
    return CYCLONE_INVALID_ARGUMENT;
  }

  // Create a cache key from the provided data
  cyclone::CacheKey ckey(std::string_view(key, key_len));

  // Perform synchronous read
  auto result = cache->impl->read_sync(ckey);

  if (!result) {
    // Key not found - this is not an error condition, just a miss
    return CYCLONE_NOT_FOUND;
  }

  // Create a read handle to return to the caller
  auto* handle = new CycloneReadHandle();
  handle->handle = std::move(*result);

  // Get content length and content span
  uint64_t len = handle->handle.content_length();
  auto content = handle->handle.content();

  // Use content_length() as the authoritative size, not content.size()
  // content.size() might return the internal buffer size, not the actual data size
  size_t actual_size = static_cast<size_t>(len);
  if (actual_size > content.size()) {
    actual_size = content.size();  // Safety check
  }

  // Copy the data to provide a stable pointer.
  handle->data_copy.assign(
      reinterpret_cast<const char*>(content.data()),
      reinterpret_cast<const char*>(content.data()) + actual_size);

  *out_handle = handle;
  return CYCLONE_OK;
}

const char* cyclone_read_handle_data(const CycloneReadHandle* handle) {
  return handle ? handle->data_copy.data() : nullptr;
}

size_t cyclone_read_handle_size(const CycloneReadHandle* handle) {
  return handle ? handle->data_copy.size() : 0;
}

void cyclone_read_handle_close(CycloneReadHandle* handle) {
  delete handle;
}

CycloneError cyclone_cache_write(CycloneCacheHandle* cache,
                                 const char* key, size_t key_len,
                                 const char* data, size_t data_len) {
  ClearLastError();

  if (!cache || !cache->impl || !cache->running) {
    SetLastError("Cache not initialized or not running");
    return CYCLONE_NOT_INITIALIZED;
  }

  if (!key) {
    SetLastError("Invalid key");
    return CYCLONE_INVALID_ARGUMENT;
  }

  // Create a cache key
  cyclone::CacheKey ckey(std::string_view(key, key_len));

  // Always delete existing entry first - Cyclone doesn't properly handle overwrites
  // (with RAM cache enabled, overwrites cause content_length to become 0)
  auto exists_result = cache->impl->exists_sync(ckey);
  if (exists_result && *exists_result) {
    cache->impl->remove_sync(ckey);
  }

  // Now write to the (empty) slot - use pre-allocated version with size
  auto handle_result = cache->impl->write_sync(ckey, data_len);

  if (!handle_result) {
    SetLastError("Failed to allocate space for write - cache may be full");
    return CYCLONE_RESOURCE_EXHAUSTED;
  }

  // Convert the data to bytes and write it
  std::vector<std::byte> bytes(data_len);
  if (data && data_len > 0) {
    std::memcpy(bytes.data(), data, data_len);
  }

  auto write_result = handle_result->write_sync(std::span<const std::byte>(bytes));
  if (!write_result) {
    // On write failure, abort the handle to prevent destructor issues
    handle_result->abort();
    SetLastError("Failed to write data");
    return CYCLONE_IO_ERROR;
  }

  // Check that all bytes were written
  size_t bytes_written = *write_result;
  if (bytes_written != data_len) {
    handle_result->abort();
    SetLastError("Incomplete write - not all bytes written");
    return CYCLONE_IO_ERROR;
  }

  // Finalize the write
  auto close_result = handle_result->close_sync();
  if (!close_result) {
    // On close failure, abort the handle
    handle_result->abort();
    SetLastError("Failed to finalize write");
    return CYCLONE_IO_ERROR;
  }

  return CYCLONE_OK;
}

CycloneError cyclone_cache_delete(CycloneCacheHandle* cache,
                                  const char* key, size_t key_len) {
  ClearLastError();

  if (!cache || !cache->impl || !cache->running) {
    SetLastError("Cache not initialized or not running");
    return CYCLONE_NOT_INITIALIZED;
  }

  if (!key) {
    SetLastError("Invalid key");
    return CYCLONE_INVALID_ARGUMENT;
  }

  cyclone::CacheKey ckey(std::string_view(key, key_len));
  auto result = cache->impl->remove_sync(ckey);

  if (!result) {
    // Key not found - return NOT_FOUND but don't set error message
    // since this is a normal condition
    return CYCLONE_NOT_FOUND;
  }

  return CYCLONE_OK;
}

CycloneError cyclone_cache_exists(CycloneCacheHandle* cache,
                                  const char* key, size_t key_len,
                                  int* exists) {
  ClearLastError();

  if (!cache || !cache->impl || !cache->running) {
    SetLastError("Cache not initialized or not running");
    return CYCLONE_NOT_INITIALIZED;
  }

  if (!key || !exists) {
    SetLastError("Invalid arguments");
    return CYCLONE_INVALID_ARGUMENT;
  }

  cyclone::CacheKey ckey(std::string_view(key, key_len));
  auto result = cache->impl->exists_sync(ckey);

  if (!result) {
    SetLastError("Failed to check existence");
    return CYCLONE_IO_ERROR;
  }

  *exists = *result ? 1 : 0;
  return CYCLONE_OK;
}

void cyclone_cache_get_stats(const CycloneCacheHandle* cache,
                             CycloneCacheStats* stats) {
  if (!cache || !cache->impl || !stats) {
    return;
  }

  auto s = cache->impl->stats();
  stats->ram_cache_hits = s.ram_cache_hits;
  stats->ram_cache_misses = s.ram_cache_misses;
  stats->disk_cache_hits = s.disk_cache_hits;
  stats->disk_cache_misses = s.disk_cache_misses;
  stats->bytes_read = s.bytes_read;
  stats->bytes_written = s.bytes_written;
  stats->evictions = s.evictions;
  stats->current_size_bytes = s.current_bytes;
  stats->current_entries = s.current_entries;
}

const char* cyclone_get_last_error(void) {
  return g_last_error.empty() ? nullptr : g_last_error.c_str();
}

}  // extern "C"
