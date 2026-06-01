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

#ifndef PAGESPEED_SYSTEM_SYSTEM_CACHE_PATH_H_
#define PAGESPEED_SYSTEM_SYSTEM_CACHE_PATH_H_

#include <memory>
#include <set>

#include "pagespeed/kernel/base/basictypes.h"
#include "pagespeed/kernel/base/string.h"
#include "pagespeed/kernel/base/string_util.h"
#include "pagespeed/kernel/base/thread_annotations.h"
#include "pagespeed/kernel/util/copy_on_write.h"

namespace net_instaweb {

class AbstractMutex;
class AbstractSharedMem;
class CacheInterface;
class CycloneCache;
class LRUCache;
class MessageHandler;
class NamedLockManager;
class PurgeContext;
class PurgeSet;
class RewriteDriverFactory;
class SharedMemLockManager;
class ThreadSafeLockManager;
class SystemServerContext;
class SystemRewriteOptions;

// The SystemCachePath encapsulates a cache-sharing model where a user specifies
// a file-cache path per virtual-host.  With each file-cache object we keep
// a locking mechanism and an optional per-process LRUCache.
class SystemCachePath {
 public:
  // CacheStats prefixes.
  static const char kFileCache[];
  static const char kLruCache[];

  SystemCachePath(const StringPiece& path, const SystemRewriteOptions* config,
                  RewriteDriverFactory* factory,
                  AbstractSharedMem* shm_runtime);
  ~SystemCachePath();

  // Computes a key suitable for building a map to help share common cache
  // objects between vhosts.  This key is given to the constructor as 'path'.
  static GoogleString CachePath(SystemRewriteOptions* config);

  // Per-process in-memory LRU, with any stats/thread safety wrappers, or NULL.
  CacheInterface* lru_cache() { return lru_cache_; }

  // Per-machine file cache with any stats wrappers.
  CacheInterface* file_cache() { return file_cache_; }

  // Access to backend for testing.  Do not use this directly in production
  // as it lacks statistics wrappers, etc.
  // Returns the underlying cache (CycloneCache or fallback LRU).
  CacheInterface* cache_backend() { return cache_backend_; }
  NamedLockManager* lock_manager() { return lock_manager_; }

  // See comments in SystemCaches for calling conventions on these.
  void RootInit();
  void ChildInit();
  void GlobalCleanup(MessageHandler* handler);  // only called in root process

  // Associates a ServerContext with this CachePath, enabling cache purges
  // to propagate into the ServerContext's global options.
  void AddServerContext(SystemServerContext* server_context);

  // Disassociates a server context with this CachePath -- used on shutdown.
  void RemoveServerContext(SystemServerContext* server_context);

  // Entry-point for flushing the cache, either via the legacy method
  // of "touch .../cache.flush" or the newer method of purging via
  // /pagespeed_admin/cache?purge=... or a PURGE method, depending on whether
  // the EnableCachePurge method is set.
  void FlushCacheIfNecessary();

  PurgeContext* purge_context() { return purge_context_.get(); }

 private:
  using ServerContextSet = std::set<SystemServerContext*>;

  GoogleString LockManagerSegmentName() const;

  // Transmits cache-purge-set updates to all live server contexts.
  void UpdateCachePurgeSet(const CopyOnWrite<PurgeSet>& purge_set);

  GoogleString path_;

  RewriteDriverFactory* factory_;
  AbstractSharedMem* shm_runtime_;
  std::unique_ptr<SharedMemLockManager> shared_mem_lock_manager_;
  std::unique_ptr<ThreadSafeLockManager> fallback_lock_manager_;
  NamedLockManager* lock_manager_;
  CacheInterface* cache_backend_;  // Cyclone or fallback LRU, owned by factory
  CacheInterface* lru_cache_;
  CacheInterface* file_cache_;
  std::unique_ptr<LRUCache>
      fallback_lru_cache_;  // Used when CycloneCache fails
  GoogleString cache_flush_filename_;
  bool unplugged_;
  bool enable_cache_purge_;

  std::unique_ptr<PurgeContext> purge_context_;

  std::unique_ptr<AbstractMutex> mutex_;
  ServerContextSet server_context_set_ GUARDED_BY(mutex_);
};

}  // namespace net_instaweb

#endif  // PAGESPEED_SYSTEM_SYSTEM_CACHE_PATH_H_
