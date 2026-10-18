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

#include <atomic>
#include <cstring>
#include <limits>
#include <memory>
#include <mutex>
#include <new>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "cyclone/cache.hpp"
#include "cyclone/config.hpp"
#include "cyclone/error.hpp"
#include "cyclone/key.hpp"

// Thread cancellation on glibc unwinds the stack with abi::__forced_unwind,
// which must not be swallowed: the runtime aborts the process if it is.  The
// barriers below rethrow it, which is also why they are not noexcept.
#if defined(__GLIBCXX__) && __has_include(<cxxabi.h>)
#include <cxxabi.h>
#define CYCLONE_WRAPPER_FORCED_UNWIND 1
#else
#define CYCLONE_WRAPPER_FORCED_UNWIND 0
#endif

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

// Internal structure that holds a read handle with reference counting.
// Supports both zero-copy (mmap'd) access and a lazy data copy for compatibility.
struct CycloneReadHandle {
  cyclone::ReadHandle handle;
  mutable std::vector<char> data_copy;  // Lazy copy - only populated on demand
  mutable std::once_flag data_copy_flag;  // Thread-safe one-time initialization
  const char* mapped_ptr =
      nullptr;                   // Direct pointer to mmap'd data (zero-copy)
  size_t data_size = 0;          // Size of the data
  std::atomic<int> refcount{1};  // Reference count, starts at 1

  // Lazily initialize data_copy from the mmap'd data.
  // Thread-safe: uses std::call_once to ensure initialization happens exactly once.
  const char* get_data_copy() const {
    std::call_once(data_copy_flag, [this]() {
      if (mapped_ptr != nullptr && data_size > 0) {
        data_copy.assign(mapped_ptr, mapped_ptr + data_size);
      }
    });
    return data_copy.data();
  }
};

// Convert a CacheError to a human-readable string.
static std::string CacheErrorMessage(cyclone::CacheError err) {
  return cyclone::make_error_code(err).message();
}

// Helper to set the thread-local error message.  May throw (it copies the
// message); every exported function runs behind the barrier below.
static void SetLastError(const std::string& msg) { g_last_error = msg; }

// Helper to clear the thread-local error message.
static void ClearLastError() noexcept { g_last_error.clear(); }

// Sets the message from inside an exception handler, where a second
// exception must not escape: if even this copy fails, the message is left
// empty rather than stale.
static void SetLastErrorNoThrow(const char* msg) noexcept {
  try {
    g_last_error = msg;
  } catch (...) {
    g_last_error.clear();
  }
}

// Exception barrier for the C ABI.  The callers are C-style frames that
// cannot handle a C++ exception, and anything in this file may throw --
// allocations in the library, in the handles below and in the error
// messages -- so every exported function that can reach one runs its body
// through one of these.  An exception becomes the given fallback result
// (CYCLONE_INTERNAL_ERROR for functions returning a CycloneError), with the
// reason in the thread's last-error message.  Each body leaves its handles
// in a consistent state before an exception can leave it (see
// CycloneCacheWriteInternal), so the barrier only translates.  A forced
// unwind (thread cancellation) passes through, see above.
template <typename Result, typename Body>
static Result Guarded(Result on_exception, Body&& body) {
  try {
    return body();
#if CYCLONE_WRAPPER_FORCED_UNWIND
  } catch (abi::__forced_unwind&) {
    throw;
#endif
  } catch (const std::bad_alloc&) {
    SetLastErrorNoThrow("Out of memory in the cache library - not completed");
  } catch (...) {
    SetLastErrorNoThrow("Internal error in the cache library - not completed");
  }
  return on_exception;
}

// The same for functions without a result: an exception is dropped.
template <typename Body>
static void GuardedVoid(Body&& body) {
  try {
    body();
#if CYCLONE_WRAPPER_FORCED_UNWIND
  } catch (abi::__forced_unwind&) {
    throw;
#endif
  } catch (const std::bad_alloc&) {
    SetLastErrorNoThrow("Out of memory in the cache library - not completed");
  } catch (...) {
    SetLastErrorNoThrow("Internal error in the cache library - not completed");
  }
}

// Map a failed lookup to the C ABI.  A plain miss stays CYCLONE_NOT_FOUND;
// everything else gets its own code so the caller can tell a miss from
// contention, a damaged entry or a failing disk.  All of them are misses
// for serving.
static CycloneError ReadErrorToCode(cyclone::CacheError err) {
  switch (err) {
    case cyclone::CacheError::NotFound:
    case cyclone::CacheError::AlternateNotFound:
      return CYCLONE_NOT_FOUND;
    case cyclone::CacheError::Busy:
    case cyclone::CacheError::Timeout:
      return CYCLONE_BUSY;
    case cyclone::CacheError::Corrupted:
    case cyclone::CacheError::ChainCorrupted:
      return CYCLONE_CORRUPTED;
    case cyclone::CacheError::IoError:
      return CYCLONE_IO_ERROR;
    case cyclone::CacheError::NotInitialized:
    case cyclone::CacheError::Closed:
      return CYCLONE_NOT_INITIALIZED;
    case cyclone::CacheError::InvalidKey:
    case cyclone::CacheError::InvalidArgument:
      return CYCLONE_INVALID_ARGUMENT;
    default:
      return CYCLONE_INTERNAL_ERROR;
  }
}

// Map a failed commit (close_sync) to the C ABI.  Busy is contention, not a
// broken cache: the caller skips such a write instead of reporting it.
static CycloneError CommitErrorToCode(cyclone::CacheError err) {
  switch (err) {
    case cyclone::CacheError::Busy:
    case cyclone::CacheError::Timeout:
      return CYCLONE_BUSY;
    case cyclone::CacheError::NoSpace:
    case cyclone::CacheError::ObjectTooLarge:
      return CYCLONE_RESOURCE_EXHAUSTED;
    case cyclone::CacheError::NotInitialized:
    case cyclone::CacheError::Closed:
      return CYCLONE_NOT_INITIALIZED;
    default:
      return CYCLONE_IO_ERROR;
  }
}

// Test-only seams for the error mapping above: when armed, the next read
// (or the next commit of a write) fails with the selected library error
// without touching the volume.  Kinds: 1 = Busy, 2 = IoError,
// 3 = Corrupted.  Armed by cyclone_cache_test_fail_next_read() /
// cyclone_cache_test_fail_next_commit(), consumed by the next call that
// reaches that step.  Not declared in cyclone_wrapper.h, like the
// pre-write seams further down.
static std::atomic<int> g_test_fail_next_read{0};
static std::atomic<int> g_test_fail_next_commit{0};

static cyclone::CacheError TestSeamError(int kind) {
  switch (kind) {
    case 1:
      return cyclone::CacheError::Busy;
    case 2:
      return cyclone::CacheError::IoError;
    default:
      return cyclone::CacheError::Corrupted;
  }
}

// Map the C ABI tier to Cyclone's C++ tier.  Anything other than
// CYCLONE_TIER_SMALL routes to the default tier.
static cyclone::Tier ToCycloneTier(CycloneTier tier) {
  return tier == CYCLONE_TIER_SMALL ? cyclone::Tier::kSmall
                                    : cyclone::Tier::kDefault;
}

extern "C" {

CycloneCacheHandle* cyclone_cache_create(const CycloneCacheConfig* config) {
  return Guarded<CycloneCacheHandle*>(nullptr, [&]() -> CycloneCacheHandle* {
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
    // Small-object tier carve-out; Cyclone clamps nonzero values to [1, 50]
    // and silently disables the tier when cache_size_bytes is below the
    // sizing floor (see cyclone_cache_small_tier_active()).
    cc.small_tier_percent = config->small_tier_percent;
    if (config->num_segments > 0) {
      cc.num_segments = config->num_segments;
    }

    // Enable persistent directory so cached data survives process restarts.
    // Single-process mode (process_index=0, total=1) — no partitioning,
    // all stripes writable, but the directory is mmap'd into the file.
    if (config->persist_directory) {
      cc.multi_process_config.enabled = true;
      cc.multi_process_config.process_index = 0;
      cc.multi_process_config.total_processes = 1;
    }

    // Create the cache instance
    auto result = cyclone::Cache::create(cc);
    if (!result) {
      SetLastError("Failed to create cache instance: " +
                   CacheErrorMessage(result.error()));
      return nullptr;
    }

    handle->impl = std::move(*result);

    // Add the volume (disk storage)
    cyclone::VolumeConfig vc;
    vc.path = config->cache_path;
    vc.size = config->cache_size_bytes;

    auto add_result = handle->impl->add_volume(vc);
    if (!add_result) {
      SetLastError("Failed to add volume at '" +
                   std::string(config->cache_path) +
                   "': " + CacheErrorMessage(add_result.error()));
      return nullptr;
    }

    return handle.release();
  });
}

void cyclone_cache_destroy(CycloneCacheHandle* cache) {
  if (cache) {
    if (cache->impl && cache->running) {
      // The handle is freed whatever stop() does.
      GuardedVoid([&] { cache->impl->stop(); });
      cache->running = false;
    }
    delete cache;
  }
}

CycloneError cyclone_cache_start(CycloneCacheHandle* cache) {
  return Guarded(CYCLONE_INTERNAL_ERROR, [&]() -> CycloneError {
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
      SetLastError("Failed to start cache at '" + cache->cache_path +
                   "': " + CacheErrorMessage(result.error()));
      return CYCLONE_INTERNAL_ERROR;
    }

    cache->running = true;
    return CYCLONE_OK;
  });
}

void cyclone_cache_stop(CycloneCacheHandle* cache) {
  if (cache && cache->impl && cache->running) {
    GuardedVoid([&] { cache->impl->stop(); });
    // Not running from here on, also when stop() threw: callers check this
    // flag before every operation.
    cache->running = false;
  }
}

int cyclone_cache_is_running(const CycloneCacheHandle* cache) {
  return (cache && cache->impl && cache->running) ? 1 : 0;
}

static CycloneError CycloneCacheReadInternal(CycloneCacheHandle* cache,
                                             const char* key, size_t key_len,
                                             cyclone::Tier tier,
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
  // Load first: the exchange writes the shared line, which production
  // reads must not do on every call.
  int seam = g_test_fail_next_read.load(std::memory_order_relaxed)
                 ? g_test_fail_next_read.exchange(0, std::memory_order_relaxed)
                 : 0;
  auto result = seam != 0
                    ? std::expected<cyclone::ReadHandle, cyclone::CacheError>(
                          std::unexpected(TestSeamError(seam)))
                    : cache->impl->read_sync(ckey, tier);

  if (!result) {
    CycloneError code = ReadErrorToCode(result.error());
    if (code != CYCLONE_NOT_FOUND) {
      // A plain miss carries no message; anything else says what it was.
      SetLastError("Read failed: " + CacheErrorMessage(result.error()));
    }
    return code;
  }

  // Create a read handle to return to the caller.  Owned here until it is
  // handed out, so an exception below cannot leak it.
  auto handle = std::make_unique<CycloneReadHandle>();
  handle->handle = std::move(*result);

  // Get content length and content span
  uint64_t len = handle->handle.content_length();
  auto content = handle->handle.content();

  // Check for integer overflow on 32-bit systems where size_t is 32 bits
  if (len > std::numeric_limits<size_t>::max()) {
    SetLastError("Content length exceeds maximum addressable size");
    return CYCLONE_INVALID_ARGUMENT;
  }

  // Use content_length() as the authoritative size, not content.size()
  // content.size() might return the internal buffer size, not the actual data size
  size_t actual_size = static_cast<size_t>(len);
  if (actual_size > content.size()) {
    actual_size = content.size();  // Safety check
  }

  handle->data_size = actual_size;

  // Store the mmap'd pointer for zero-copy access.
  // This pointer is valid as long as the ReadHandle is open.
  if (!content.empty()) {
    handle->mapped_ptr = reinterpret_cast<const char*>(content.data());
  }

  // data_copy is lazily initialized only when cyclone_read_handle_data()
  // is called. This avoids unnecessary copying on the zero-copy path.

  *out_handle = handle.release();
  return CYCLONE_OK;
}

CycloneError cyclone_cache_read(CycloneCacheHandle* cache, const char* key,
                                size_t key_len,
                                CycloneReadHandle** out_handle) {
  return Guarded(CYCLONE_INTERNAL_ERROR, [&] {
    return CycloneCacheReadInternal(cache, key, key_len,
                                    cyclone::Tier::kDefault, out_handle);
  });
}

CycloneError cyclone_cache_read_tier(CycloneCacheHandle* cache, const char* key,
                                     size_t key_len, CycloneTier tier,
                                     CycloneReadHandle** out_handle) {
  return Guarded(CYCLONE_INTERNAL_ERROR, [&] {
    return CycloneCacheReadInternal(cache, key, key_len, ToCycloneTier(tier),
                                    out_handle);
  });
}

const char* cyclone_read_handle_data(const CycloneReadHandle* handle) {
  // The copy is made on first use and may fail to allocate: then NULL, and
  // a later call tries again (std::call_once does not latch on a throw).
  return Guarded<const char*>(nullptr, [&]() -> const char* {
    return handle ? handle->get_data_copy() : nullptr;
  });
}

const char* cyclone_read_handle_mapped_data(const CycloneReadHandle* handle) {
  return handle ? handle->mapped_ptr : nullptr;
}

int cyclone_read_handle_has_mapped_data(const CycloneReadHandle* handle) {
  return (handle && handle->mapped_ptr != nullptr) ? 1 : 0;
}

size_t cyclone_read_handle_size(const CycloneReadHandle* handle) {
  return handle ? handle->data_size : 0;
}

int cyclone_read_handle_is_ram_hit(const CycloneReadHandle* handle) {
  return Guarded(0, [&] {
    return (handle != nullptr && handle->handle.is_ram_cache_hit()) ? 1 : 0;
  });
}

void cyclone_read_handle_ref(CycloneReadHandle* handle) {
  if (handle) {
    handle->refcount.fetch_add(1, std::memory_order_relaxed);
  }
}

void cyclone_read_handle_unref(CycloneReadHandle* handle) {
  if (handle) {
    // Use acq_rel to ensure all accesses to handle data happen-before deletion
    if (handle->refcount.fetch_sub(1, std::memory_order_acq_rel) == 1) {
      GuardedVoid([&] { delete handle; });
    }
  }
}

int cyclone_read_handle_refcount(const CycloneReadHandle* handle) {
  return handle ? handle->refcount.load(std::memory_order_relaxed) : 0;
}

void cyclone_read_handle_close(CycloneReadHandle* handle) {
  // Legacy API - equivalent to unref
  cyclone_read_handle_unref(handle);
}

int cyclone_read_handle_renew_lease(CycloneReadHandle* handle) {
  // On an exception the lease is reported as not renewed.
  return Guarded(0, [&] {
    return (handle != nullptr && handle->handle.renew_lease()) ? 1 : 0;
  });
}

int cyclone_read_handle_renew_lease_strict(CycloneReadHandle* handle) {
  if (handle == nullptr) {
    return static_cast<int>(cyclone::LeaseRenewal::kLeasesOff);
  }
  // On an exception the region may no longer be intact: report it torn, so
  // the caller stops serving it.
  return Guarded(static_cast<int>(cyclone::LeaseRenewal::kTorn), [&] {
    return static_cast<int>(handle->handle.renew_lease_strict());
  });
}

uint64_t cyclone_read_handle_ns_until_forced_wrap(
    const CycloneReadHandle* handle) {
  // On an exception: no time left.
  return Guarded(uint64_t{0}, [&] {
    return handle != nullptr ? handle->handle.ns_until_forced_wrap()
                             : UINT64_MAX;
  });
}

// Test-only seam for the pre-write remove in CycloneCacheWriteInternal:
// when armed, the next write onto an existing key is judged as if the
// library's remove had returned Busy, without touching the volume --
// simulating a cross-process lock held by a live peer past the library's
// wait cap.  Armed by cyclone_cache_test_busy_next_pre_write_remove(),
// consumed by the write that performs the pre-write remove.  Not declared
// in cyclone_wrapper.h: a unit-test hook is not part of the vendored C ABI.
static bool g_test_busy_next_pre_write_remove = false;

// The pre-write remove, with the seam above able to force its outcome so
// tests can drive the branch that must drop the write.
static std::expected<void, cyclone::CacheError> PreWriteRemove(
    CycloneCacheHandle* cache, const cyclone::CacheKey& ckey,
    cyclone::Tier tier) {
  if (g_test_busy_next_pre_write_remove) {
    g_test_busy_next_pre_write_remove = false;
    return std::unexpected(cyclone::CacheError::Busy);
  }
  return cache->impl->remove_sync(ckey, tier);
}

// Test-only seam for the pre-write presence check in the same write path:
// when armed, the next write's exists lookup is judged as if the library
// had returned Busy, without touching the volume -- simulating a writer
// that held the key's directory bucket for the whole seqlock wait budget.
// Armed by cyclone_cache_test_busy_next_pre_write_exists(), consumed by
// the write that performs the presence check.  Like the remove seam, not
// declared in cyclone_wrapper.h (not part of the vendored C ABI).
static bool g_test_busy_next_pre_write_exists = false;

// The pre-write presence check, with the seam above able to force its
// outcome so tests can drive the branch that must drop the write.
static std::expected<bool, cyclone::CacheError> PreWriteExists(
    CycloneCacheHandle* cache, const cyclone::CacheKey& ckey,
    cyclone::Tier tier) {
  if (g_test_busy_next_pre_write_exists) {
    g_test_busy_next_pre_write_exists = false;
    return std::unexpected(cyclone::CacheError::Busy);
  }
  return cache->impl->exists_sync(ckey, tier);
}

static CycloneError CommitWrite(cyclone::WriteHandle* handle, const char* data,
                                size_t data_len);

static CycloneError CycloneCacheWriteInternal(CycloneCacheHandle* cache,
                                              const char* key, size_t key_len,
                                              const char* data, size_t data_len,
                                              cyclone::Tier tier) {
  ClearLastError();

  if (!cache || !cache->impl || !cache->running) {
    SetLastError("Cache not initialized or not running");
    return CYCLONE_NOT_INITIALIZED;
  }

  if (!key || (data == nullptr && data_len > 0)) {
    SetLastError("Invalid arguments");
    return CYCLONE_INVALID_ARGUMENT;
  }

  // Create a cache key
  cyclone::CacheKey ckey(std::string_view(key, key_len));

  // Always delete existing entry first - Cyclone doesn't properly handle overwrites
  // (with RAM cache enabled, overwrites cause content_length to become 0)
  auto exists_result = PreWriteExists(cache, ckey, tier);
  if (!exists_result) {
    // Presence could not be determined -- at this pin an exists lookup
    // that exhausts its seqlock wait returns Busy with the answer unknown
    // (deliberately not NotFound).  Writing without knowing the slot is
    // empty risks the in-place overwrite the delete-first step exists to
    // prevent, so drop this write and report it like a busy write_sync.
    SetLastError("Cache busy (lock held by another process) - not stored");
    return CYCLONE_BUSY;
  }
  if (*exists_result) {
    auto remove_result = PreWriteRemove(cache, ckey, tier);
    if (!remove_result &&
        remove_result.error() != cyclone::CacheError::NotFound) {
      // The entry was NOT removed -- at this pin the remove gives up with
      // Busy after ~250 ms when another process of this server holds the
      // cross-process locks.  Writing now would overwrite the live entry,
      // exactly what the delete-first step exists to prevent, so drop this
      // write and report it like a busy write_sync.  NotFound is fine: a
      // peer removed the entry between the exists check and here, so the
      // slot is empty.
      SetLastError("Cache busy (lock held by another process) - not stored");
      return CYCLONE_BUSY;
    }
  }

  // Now write to the (empty) slot - use pre-allocated version with size
  auto handle_result = cache->impl->write_sync(ckey, data_len, tier);

  if (!handle_result) {
    // Two very different conditions arrive here and they used to be reported
    // identically.  Since the cache library began enforcing its per-object
    // ceiling, an over-size put fails at this same call -- before anything is
    // buffered -- and reporting it as "cache may be full" points the operator
    // at the one remedy that cannot work: enlarging the cache does not make a
    // 65 MB object fit under a 64 MB per-object bound, and nothing else in
    // the product ever says the words "size limit".  Name the real cause.
    //
    // The status code is deliberately unchanged.  CycloneError is the
    // wrapper's ABI and this is a diagnostic fix, not a behaviour change; a
    // caller that treats a failed write as a failed write keeps working, and
    // a distinct code can be minted later if one is wanted.
    if (handle_result.error() == cyclone::CacheError::ObjectTooLarge) {
      SetLastError(
          "Object exceeds the cache's per-object size limit - not stored "
          "(enlarging the cache does not raise this limit)");
    } else if (handle_result.error() == cyclone::CacheError::Busy) {
      // Another process of this server kept the cache's cross-process locks
      // for longer than the library waits; nothing was stored.  Not a full
      // cache, so do not say so.
      SetLastError("Cache busy (lock held by another process) - not stored");
      return CYCLONE_BUSY;
    } else {
      SetLastError("Failed to allocate space for write - cache may be full");
    }
    return CYCLONE_RESOURCE_EXHAUSTED;
  }

  // From here on the write handle is open.  Whatever throws below, abort it
  // before the exception reaches the barrier, so nothing half-written can
  // be committed later.
  try {
    return CommitWrite(&*handle_result, data, data_len);
  } catch (...) {
    handle_result->abort();
    throw;
  }
}

// Fills and commits an open write handle (see CycloneCacheWriteInternal).
static CycloneError CommitWrite(cyclone::WriteHandle* handle_result,
                                const char* data, size_t data_len) {
  // The handle buffers the content itself, so write straight from the
  // caller's bytes; no intermediate copy is needed.
  std::span<const std::byte> bytes(reinterpret_cast<const std::byte*>(data),
                                   data_len);
  auto write_result = handle_result->write_sync(bytes);
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
  int seam =
      g_test_fail_next_commit.load(std::memory_order_relaxed)
          ? g_test_fail_next_commit.exchange(0, std::memory_order_relaxed)
          : 0;
  auto close_result = seam != 0 ? std::expected<void, cyclone::CacheError>(
                                      std::unexpected(TestSeamError(seam)))
                                : handle_result->close_sync();
  if (!close_result) {
    // On close failure, abort the handle
    handle_result->abort();
    CycloneError code = CommitErrorToCode(close_result.error());
    if (code == CYCLONE_BUSY) {
      // Contention outlasted the library's retries (other writers of the
      // same entry, or a lock held by another process); nothing was
      // stored.  Not a cache fault.
      SetLastError("Cache busy (contended write) - not stored");
    } else {
      SetLastError("Failed to finalize write: " +
                   CacheErrorMessage(close_result.error()));
    }
    return code;
  }

  return CYCLONE_OK;
}

CycloneError cyclone_cache_write(CycloneCacheHandle* cache, const char* key,
                                 size_t key_len, const char* data,
                                 size_t data_len) {
  return Guarded(CYCLONE_INTERNAL_ERROR, [&] {
    return CycloneCacheWriteInternal(cache, key, key_len, data, data_len,
                                     cyclone::Tier::kDefault);
  });
}

CycloneError cyclone_cache_write_tier(CycloneCacheHandle* cache,
                                      const char* key, size_t key_len,
                                      const char* data, size_t data_len,
                                      CycloneTier tier) {
  return Guarded(CYCLONE_INTERNAL_ERROR, [&] {
    return CycloneCacheWriteInternal(cache, key, key_len, data, data_len,
                                     ToCycloneTier(tier));
  });
}

// Arm the pre-write remove test seam (see PreWriteRemove above): the next
// write onto an existing key behaves as if its pre-write remove had
// returned Busy -- the remove is not performed.  Unit tests only.
void cyclone_cache_test_busy_next_pre_write_remove(void) {
  g_test_busy_next_pre_write_remove = true;
}

// Arm the pre-write presence-check test seam (see PreWriteExists above):
// the next write behaves as if its pre-write exists had returned Busy
// (presence unknown) -- the lookup is not performed.  Unit tests only.
void cyclone_cache_test_busy_next_pre_write_exists(void) {
  g_test_busy_next_pre_write_exists = true;
}

// Arm the read and commit error seams (see TestSeamError above).  Unit
// tests only.
void cyclone_cache_test_fail_next_read(int kind) {
  g_test_fail_next_read.store(kind, std::memory_order_relaxed);
}

void cyclone_cache_test_fail_next_commit(int kind) {
  g_test_fail_next_commit.store(kind, std::memory_order_relaxed);
}

static CycloneError CycloneCacheDeleteInternal(CycloneCacheHandle* cache,
                                               const char* key, size_t key_len,
                                               cyclone::Tier tier) {
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
  auto result = cache->impl->remove_sync(ckey, tier);

  if (!result) {
    if (result.error() == cyclone::CacheError::Busy) {
      // Another process kept the cross-process locks past the library's
      // wait cap and the entry was NOT removed.  Reporting that as
      // NOT_FOUND would count it as a delete that happened.
      SetLastError("Cache busy (lock held by another process) - not deleted");
      return CYCLONE_BUSY;
    }
    // Key not found - return NOT_FOUND but don't set error message
    // since this is a normal condition
    return CYCLONE_NOT_FOUND;
  }

  return CYCLONE_OK;
}

CycloneError cyclone_cache_delete(CycloneCacheHandle* cache, const char* key,
                                  size_t key_len) {
  return Guarded(CYCLONE_INTERNAL_ERROR, [&] {
    return CycloneCacheDeleteInternal(cache, key, key_len,
                                      cyclone::Tier::kDefault);
  });
}

CycloneError cyclone_cache_delete_tier(CycloneCacheHandle* cache,
                                       const char* key, size_t key_len,
                                       CycloneTier tier) {
  return Guarded(CYCLONE_INTERNAL_ERROR, [&] {
    return CycloneCacheDeleteInternal(cache, key, key_len, ToCycloneTier(tier));
  });
}

static CycloneError CycloneCacheExistsInternal(CycloneCacheHandle* cache,
                                               const char* key, size_t key_len,
                                               cyclone::Tier tier,
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
  auto result = cache->impl->exists_sync(ckey, tier);

  if (!result) {
    SetLastError("Failed to check existence");
    return CYCLONE_IO_ERROR;
  }

  *exists = *result ? 1 : 0;
  return CYCLONE_OK;
}

CycloneError cyclone_cache_exists(CycloneCacheHandle* cache, const char* key,
                                  size_t key_len, int* exists) {
  return Guarded(CYCLONE_INTERNAL_ERROR, [&] {
    return CycloneCacheExistsInternal(cache, key, key_len,
                                      cyclone::Tier::kDefault, exists);
  });
}

CycloneError cyclone_cache_exists_tier(CycloneCacheHandle* cache,
                                       const char* key, size_t key_len,
                                       CycloneTier tier, int* exists) {
  return Guarded(CYCLONE_INTERNAL_ERROR, [&] {
    return CycloneCacheExistsInternal(cache, key, key_len, ToCycloneTier(tier),
                                      exists);
  });
}

int cyclone_cache_small_tier_active(const CycloneCacheHandle* cache) {
  return Guarded(0, [&] {
    return (cache && cache->impl && cache->impl->small_tier_active()) ? 1 : 0;
  });
}

void cyclone_cache_get_stats(const CycloneCacheHandle* cache,
                             CycloneCacheStats* stats) {
  if (!cache || !cache->impl || !stats) {
    return;
  }
  // On an exception *stats is left as the caller initialized it.
  GuardedVoid([&] {
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
    stats->ram_cache_bytes = s.ram_cache_bytes;
    stats->write_buffer_wraps = s.write_buffer_wraps;
    stats->wraps_deferred_by_lease = s.wraps_deferred_by_lease;
    stats->writes_dropped_by_lease = s.writes_dropped_by_lease;
    stats->wraps_forced_past_lease = s.wraps_forced_past_lease;
    stats->tag_collision_evictions = s.tag_collision_evictions;
    stats->bucket_full_evictions = s.bucket_full_evictions;
    stats->resets_under_degraded_gate = s.resets_under_degraded_gate;
    stats->resets_gate_verified = s.resets_gate_verified;
  });
}

const char* cyclone_get_last_error(void) {
  return g_last_error.empty() ? nullptr : g_last_error.c_str();
}

}  // extern "C"
