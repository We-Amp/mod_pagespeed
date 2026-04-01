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

// C-style wrapper for Cyclone Cache library.
//
// This header provides a C ABI interface to the Cyclone cache library,
// which is implemented in C++23. By using a C interface, we can link
// C++20 code (like mod_pagespeed) against the C++23 Cyclone library
// without requiring the entire codebase to use C++23.
//
// Usage:
//   1. Create a cache with cyclone_cache_create()
//   2. Start the cache with cyclone_cache_start()
//   3. Perform read/write/delete operations
//   4. Stop with cyclone_cache_stop() and destroy with cyclone_cache_destroy()

#ifndef PAGESPEED_KERNEL_CACHE_CYCLONE_CYCLONE_WRAPPER_H_
#define PAGESPEED_KERNEL_CACHE_CYCLONE_CYCLONE_WRAPPER_H_

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Opaque handle types - actual structures are defined in the C++23 implementation
typedef struct CycloneCacheHandle CycloneCacheHandle;
typedef struct CycloneReadHandle CycloneReadHandle;

// Error codes returned by Cyclone operations.
// These map to the internal Cyclone error codes.
typedef enum CycloneError {
  CYCLONE_OK = 0,                  // Operation succeeded
  CYCLONE_NOT_FOUND = 1,           // Key not found in cache
  CYCLONE_ALREADY_EXISTS = 2,      // Key already exists (for create operations)
  CYCLONE_INVALID_ARGUMENT = 3,    // Invalid argument provided
  CYCLONE_PERMISSION_DENIED = 4,   // Permission denied
  CYCLONE_RESOURCE_EXHAUSTED = 5,  // Cache is full, cannot allocate
  CYCLONE_IO_ERROR = 6,            // I/O error during operation
  CYCLONE_INTERNAL_ERROR = 7,      // Internal error
  CYCLONE_UNAVAILABLE = 8,         // Service unavailable
  CYCLONE_NOT_INITIALIZED = 9      // Cache not initialized or not running
} CycloneError;

// Configuration for creating a Cyclone cache instance.
typedef struct CycloneCacheConfig {
  // Path to the cache data file (required).
  // This is where the cache will store its data on disk.
  const char* cache_path;

  // Maximum size of the disk cache in bytes.
  // The cache will evict entries to stay within this limit.
  uint64_t cache_size_bytes;

  // Size of the in-memory RAM cache in bytes.
  // Set to 0 to disable the RAM cache layer.
  // The RAM cache provides faster access for hot data.
  uint64_t ram_cache_size_bytes;

  // Enable CRC32 checksums for data integrity verification.
  // Set to non-zero to enable checksums.
  int enable_checksum;

  // Number of cache segments (affects concurrency).
  // Set to 0 for the default value.
  // More segments allow more concurrent operations but use more memory.
  int num_segments;

  // Enable persistent directory for cache data to survive process restarts.
  // Set to non-zero to enable.
  // When enabled, the cache directory is stored in the data file via mmap.
  int persist_directory;
} CycloneCacheConfig;

// Statistics from the cache.
typedef struct CycloneCacheStats {
  uint64_t ram_cache_hits;      // Hits served from RAM cache
  uint64_t ram_cache_misses;    // Misses in RAM cache
  uint64_t disk_cache_hits;     // Hits served from disk cache
  uint64_t disk_cache_misses;   // Misses in disk cache (key not found)
  uint64_t bytes_read;          // Total bytes read from cache
  uint64_t bytes_written;       // Total bytes written to cache
  uint64_t evictions;           // Number of entries evicted
  uint64_t current_size_bytes;  // Current size of cache on disk
  uint64_t current_entries;     // Current number of entries in cache
} CycloneCacheStats;

// ============================================================================
// Cache Lifecycle Management
// ============================================================================

// Create a new Cyclone cache instance with the given configuration.
//
// Returns a cache handle on success, or NULL on failure.
// Call cyclone_get_last_error() to get the error message on failure.
// The returned handle must be destroyed with cyclone_cache_destroy().
//
// The cache is not started after creation - call cyclone_cache_start()
// before performing any operations.
CycloneCacheHandle* cyclone_cache_create(const CycloneCacheConfig* config);

// Destroy a Cyclone cache instance and release all resources.
//
// This will stop the cache if it is running. After this call, the
// handle is invalid and must not be used.
//
// It is safe to call this with a NULL handle.
void cyclone_cache_destroy(CycloneCacheHandle* cache);

// Start the cache.
//
// Must be called before any read/write operations.
// Returns CYCLONE_OK on success.
CycloneError cyclone_cache_start(CycloneCacheHandle* cache);

// Stop the cache.
//
// Flushes pending writes and releases resources.
// The cache can be restarted with cyclone_cache_start().
void cyclone_cache_stop(CycloneCacheHandle* cache);

// Check if the cache is currently running.
//
// Returns 1 if running, 0 if stopped or invalid handle.
int cyclone_cache_is_running(const CycloneCacheHandle* cache);

// ============================================================================
// Cache Read Operations
// ============================================================================

// Read a value from the cache.
//
// On success (CYCLONE_OK), *out_handle will be set to a read handle that
// provides access to the cached data. The handle is created with a reference
// count of 1. Use cyclone_read_handle_ref() to increment and
// cyclone_read_handle_unref() to decrement. The handle is automatically
// closed when the reference count reaches zero.
//
// Returns CYCLONE_NOT_FOUND if the key does not exist.
// Returns CYCLONE_NOT_INITIALIZED if the cache is not running.
CycloneError cyclone_cache_read(CycloneCacheHandle* cache, const char* key,
                                size_t key_len, CycloneReadHandle** out_handle);

// Get a pointer to the data in a read handle.
//
// The pointer is valid until the handle's reference count reaches zero.
// This returns a pointer to a copy of the data (for backward compatibility).
// For zero-copy access, use cyclone_read_handle_mapped_data().
//
// Returns NULL if the handle is NULL.
const char* cyclone_read_handle_data(const CycloneReadHandle* handle);

// Get a pointer to the memory-mapped data in a read handle (zero-copy).
//
// This returns a direct pointer to the mmap'd data, avoiding a copy.
// The pointer is valid as long as the handle's reference count is > 0.
// May return NULL if the data is not available via mmap (e.g., RAM cache hit).
// In that case, fall back to cyclone_read_handle_data().
//
// Returns NULL if the handle is NULL or mmap is not available.
const char* cyclone_read_handle_mapped_data(const CycloneReadHandle* handle);

// Check if the read handle has memory-mapped data available.
//
// Returns 1 if cyclone_read_handle_mapped_data() will return non-NULL.
// Returns 0 otherwise.
int cyclone_read_handle_has_mapped_data(const CycloneReadHandle* handle);

// Get the size of the data in a read handle.
//
// Returns 0 if the handle is NULL.
size_t cyclone_read_handle_size(const CycloneReadHandle* handle);

// Increment the reference count on a read handle.
//
// This allows multiple holders to share a read handle. The handle remains
// valid until all references are released via cyclone_read_handle_unref().
void cyclone_read_handle_ref(CycloneReadHandle* handle);

// Decrement the reference count on a read handle.
//
// When the reference count reaches zero, the handle is automatically closed
// and its resources are released. After this call, if the refcount reached
// zero, the handle is invalid and must not be used.
//
// It is safe to call this with a NULL handle.
void cyclone_read_handle_unref(CycloneReadHandle* handle);

// Get the current reference count of a read handle (for debugging).
//
// Returns 0 if the handle is NULL.
int cyclone_read_handle_refcount(const CycloneReadHandle* handle);

// Close a read handle and release its resources (legacy API).
//
// This is equivalent to cyclone_read_handle_unref() and is provided for
// backward compatibility. New code should use the ref/unref API.
//
// After this call, the handle is invalid and must not be used.
// It is safe to call this with a NULL handle.
void cyclone_read_handle_close(CycloneReadHandle* handle);

// ============================================================================
// Cache Write Operations
// ============================================================================

// Write a value to the cache.
//
// If the key already exists, the value is replaced.
//
// Returns CYCLONE_OK on success.
// Returns CYCLONE_RESOURCE_EXHAUSTED if the cache is full and cannot
// evict enough space.
// Returns CYCLONE_NOT_INITIALIZED if the cache is not running.
CycloneError cyclone_cache_write(CycloneCacheHandle* cache, const char* key,
                                 size_t key_len, const char* data,
                                 size_t data_len);

// Delete a key from the cache.
//
// Returns CYCLONE_OK on success.
// Returns CYCLONE_NOT_FOUND if the key does not exist.
// Returns CYCLONE_NOT_INITIALIZED if the cache is not running.
CycloneError cyclone_cache_delete(CycloneCacheHandle* cache, const char* key,
                                  size_t key_len);

// Check if a key exists in the cache.
//
// On success (CYCLONE_OK), *exists will be set to 1 if the key exists,
// or 0 if it does not.
//
// Returns CYCLONE_NOT_INITIALIZED if the cache is not running.
CycloneError cyclone_cache_exists(CycloneCacheHandle* cache, const char* key,
                                  size_t key_len, int* exists);

// ============================================================================
// Statistics and Diagnostics
// ============================================================================

// Get statistics from the cache.
//
// Fills in the provided stats structure with current cache statistics.
// Does nothing if cache or stats is NULL.
void cyclone_cache_get_stats(const CycloneCacheHandle* cache,
                             CycloneCacheStats* stats);

// Get the last error message (thread-local).
//
// Returns NULL if no error has occurred in the current thread.
// The returned string is valid until the next Cyclone call in this thread.
const char* cyclone_get_last_error(void);

#ifdef __cplusplus
}  // extern "C"
#endif

#endif  // PAGESPEED_KERNEL_CACHE_CYCLONE_CYCLONE_WRAPPER_H_
