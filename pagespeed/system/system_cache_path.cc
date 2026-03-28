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

#include "pagespeed/system/system_cache_path.h"

#include <memory>

#include "base/logging.h"
#include "net/instaweb/rewriter/public/rewrite_driver_factory.h"
#include "net/instaweb/rewriter/public/rewrite_options.h"
//#include "strings/stringpiece_utils.h"
#include "pagespeed/kernel/base/abstract_mutex.h"
#include "pagespeed/kernel/base/basictypes.h"
#include "pagespeed/kernel/base/callback.h"
#include "pagespeed/kernel/base/message_handler.h"
#include "pagespeed/kernel/base/thread_system.h"
#include "pagespeed/kernel/cache/cache_interface.h"
#include "pagespeed/kernel/cache/cache_stats.h"
#include "pagespeed/kernel/cache/cyclone_cache.h"
#include "pagespeed/kernel/cache/lru_cache.h"
#include "pagespeed/kernel/cache/purge_context.h"
#include "pagespeed/kernel/cache/purge_set.h"
#include "pagespeed/kernel/sharedmem/shared_mem_lock_manager.h"
#include "pagespeed/kernel/util/threadsafe_lock_manager.h"
#include "pagespeed/system/system_rewrite_options.h"
#include "pagespeed/system/system_server_context.h"

namespace net_instaweb {

const char SystemCachePath::kFileCache[] = "file_cache";
const char SystemCachePath::kLruCache[] = "lru_cache";

// The SystemCachePath encapsulates a cache-sharing model where a user specifies
// a file-cache path per virtual-host.  With each file-cache object we keep
// a locking mechanism and an optional per-process LRUCache.
SystemCachePath::SystemCachePath(const StringPiece& path,
                                 const SystemRewriteOptions* config,
                                 RewriteDriverFactory* factory,
                                 AbstractSharedMem* shm_runtime)
    : path_(path.data(), path.size()),
      factory_(factory),
      shm_runtime_(shm_runtime),
      lock_manager_(nullptr),
      cache_backend_(nullptr),
      lru_cache_(nullptr),
      file_cache_(nullptr),
      cache_flush_filename_(config->cache_flush_filename()),
      unplugged_(config->unplugged()),
      enable_cache_purge_(config->enable_cache_purge()),
      mutex_(factory->thread_system()->NewMutex()) {
  if (cache_flush_filename_.empty()) {
    if (enable_cache_purge_) {
      cache_flush_filename_ = "cache.purge";
    } else {
      cache_flush_filename_ = "cache.flush";
    }
  }
  if (cache_flush_filename_[0] != '/'
#ifdef _WIN32
      && !(cache_flush_filename_.size() >= 3 &&
           cache_flush_filename_[1] == ':' &&
           (cache_flush_filename_[2] == '\\' ||
            cache_flush_filename_[2] == '/'))
#endif
  ) {
    // cache_flush_filename_ is relative — prepend file_cache_path.
    // There is at least one example where the file cache path check is
    // violated in ngx_pagespeed (server-level pagespeed off with
    // FileCachePath set, then pagespeed on in a location block).
    StringPiece path(config->file_cache_path());
    cache_flush_filename_ = StrCat(
        path, (path.size() > 0 && (strings::EndsWith(path, "/")
#ifdef _WIN32
               || strings::EndsWith(path, "\\")
#endif
               )) ? "" : "/",
        cache_flush_filename_);
  }

  if (config->use_shared_mem_locking()) {
    shared_mem_lock_manager_ = std::make_unique<SharedMemLockManager>(
        shm_runtime, LockManagerSegmentName(), factory->scheduler(),
        factory->hasher(), factory->message_handler());
    lock_manager_ = shared_mem_lock_manager_.get();
  } else {
    factory->message_handler()->Message(
        kWarning, "Shared memory locking disabled; inter-process coordination "
                  "will not be available for path: %s", path_.c_str());
    // Use ThreadSafeLockManager for single-process locking when shared memory
    // is not available. This provides thread-safety but not inter-process
    // coordination.
    fallback_lock_manager_ =
        std::make_unique<ThreadSafeLockManager>(factory->scheduler());
    lock_manager_ = fallback_lock_manager_.get();
  }

  // Create the CycloneCache disk cache backend.
  // CycloneCache handles its own eviction using scan-resistant CLFUS algorithm
  // and does not need external cleaning workers.
  CycloneCache::Config cyclone_config;
  cyclone_config.cache_path = StrCat(config->file_cache_path(), "/cyclone.dat");
  cyclone_config.cache_size_bytes = config->file_cache_clean_size_kb() * 1024;
  // Use the LRU cache size setting for the RAM cache layer
  cyclone_config.ram_cache_size_bytes = config->lru_cache_kb_per_process() * 1024;
  cyclone_config.enable_checksum = true;
  cyclone_config.num_segments = 0;  // Use default

  CycloneCache* cyclone_cache = new CycloneCache(
      cyclone_config, factory->statistics(), factory->message_handler());
  factory->TakeOwnership(cyclone_cache);

  // Check if CycloneCache started successfully
  if (cyclone_cache->IsHealthy()) {
    cache_backend_ = cyclone_cache;
    // Register cyclone.dat so Apache's post_config chown sweep fixes ownership.
    factory->AddCreatedDirectory(cyclone_config.cache_path);
    file_cache_ = new CacheStats(kFileCache, cache_backend_,
                                 factory->timer(), factory->statistics());
    factory->TakeOwnership(file_cache_);

    if (cyclone_config.ram_cache_size_bytes > 0) {
      factory->message_handler()->Message(
          kInfo, "CycloneCache enabled with %lld byte RAM cache at %s",
          static_cast<long long>(cyclone_config.ram_cache_size_bytes),
          cyclone_config.cache_path.c_str());
    } else {
      factory->message_handler()->Message(
          kInfo, "CycloneCache enabled at %s",
          cyclone_config.cache_path.c_str());
    }
  } else {
    // CycloneCache failed to start - fall back to LRU-only cache.
    // This can happen on platforms where CycloneCache isn't fully supported
    // (e.g., Windows) or when the cache directory isn't writable.
    factory->message_handler()->Message(
        kWarning, "CycloneCache failed to start at %s. "
                  "Falling back to LRU-only cache.",
        cyclone_config.cache_path.c_str());

    // Create an LRU cache to use as the file_cache fallback.
    // This means we won't have persistent disk caching, but rewriting
    // will still work using the in-memory LRU cache.
    int64 lru_size = config->lru_cache_kb_per_process() * 1024;
    if (lru_size == 0) {
      // Ensure at least some cache space if LRU was configured to 0
      lru_size = 50 * 1024 * 1024;  // 50 MB default
    }
    fallback_lru_cache_ = std::make_unique<LRUCache>(lru_size);
    cache_backend_ = fallback_lru_cache_.get();
    file_cache_ = new CacheStats(kFileCache, cache_backend_,
                                 factory->timer(), factory->statistics());
    factory->TakeOwnership(file_cache_);
  }
}

SystemCachePath::~SystemCachePath() {}

// static
GoogleString SystemCachePath::CachePath(SystemRewriteOptions* config) {
  return (config->unplugged()
              ? "<unplugged>"
              : StrCat(config->file_cache_path(),
                       config->enable_cache_purge() ? " purge " : " flush ",
                       config->cache_flush_filename()));
}

void SystemCachePath::RootInit() {
  factory_->message_handler()->Message(
      kInfo, "Initializing shared memory for path: %s.", path_.c_str());
  if ((shared_mem_lock_manager_.get() != nullptr) &&
      !shared_mem_lock_manager_->Initialize()) {
    factory_->message_handler()->Message(
        kError, "Failed to initialize shared memory lock manager for path: %s. "
                "Falling back to in-process locking.",
        path_.c_str());
    shared_mem_lock_manager_.reset(nullptr);
    // Fall back to ThreadSafeLockManager
    fallback_lock_manager_ =
        std::make_unique<ThreadSafeLockManager>(factory_->scheduler());
    lock_manager_ = fallback_lock_manager_.get();
  }
}

void SystemCachePath::ChildInit() {
  if (unplugged_) {
    return;
  }
  factory_->message_handler()->Message(
      kInfo, "Reusing shared memory for path: %s.", path_.c_str());
  if ((shared_mem_lock_manager_.get() != nullptr) &&
      !shared_mem_lock_manager_->Attach()) {
    factory_->message_handler()->Message(
        kError, "Failed to attach to shared memory lock manager for path: %s. "
                "Falling back to in-process locking.",
        path_.c_str());
    shared_mem_lock_manager_.reset(nullptr);
    // Fall back to ThreadSafeLockManager
    fallback_lock_manager_ =
        std::make_unique<ThreadSafeLockManager>(factory_->scheduler());
    lock_manager_ = fallback_lock_manager_.get();
  }
  purge_context_ = std::make_unique<PurgeContext>(
      cache_flush_filename_, factory_->file_system(), factory_->timer(),
      RewriteOptions::kCachePurgeBytes, factory_->thread_system(),
      lock_manager_, factory_->scheduler(), factory_->statistics(),
      factory_->message_handler());
  purge_context_->set_enable_purge(enable_cache_purge_);
  purge_context_->SetUpdateCallback(
      NewPermanentCallback(this, &SystemCachePath::UpdateCachePurgeSet));
}

void SystemCachePath::GlobalCleanup(MessageHandler* handler) {
  if (shared_mem_lock_manager_.get() != nullptr) {
    shared_mem_lock_manager_->GlobalCleanup(shm_runtime_,
                                            LockManagerSegmentName(), handler);
  }
}

GoogleString SystemCachePath::LockManagerSegmentName() const {
  return StrCat(path_, "/named_locks");
}

void SystemCachePath::FlushCacheIfNecessary() {
  if (!unplugged_) {
    purge_context_->PollFileSystem();
  }
}

void SystemCachePath::AddServerContext(SystemServerContext* server_context) {
  ScopedMutex lock(mutex_.get());
  server_context_set_.insert(server_context);
}

void SystemCachePath::RemoveServerContext(SystemServerContext* server_context) {
  ScopedMutex lock(mutex_.get());
  server_context_set_.erase(server_context);
}

void SystemCachePath::UpdateCachePurgeSet(
    const CopyOnWrite<PurgeSet>& purge_set) {
  ScopedMutex lock(mutex_.get());
  for (ServerContextSet::iterator p = server_context_set_.begin(),
                                  e = server_context_set_.end();
       p != e; ++p) {
    SystemServerContext* server_context = *p;
    server_context->UpdateCachePurgeSet(purge_set);
  }
}

}  // namespace net_instaweb
