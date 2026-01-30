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
#include "pagespeed/kernel/cache/purge_context.h"
#include "pagespeed/kernel/cache/purge_set.h"
#include "pagespeed/kernel/sharedmem/shared_mem_lock_manager.h"
#include "pagespeed/kernel/util/file_system_lock_manager.h"
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
  if (cache_flush_filename_[0] != '/') {
    // Implementations must ensure the file cache path is an absolute path.
    // mod_pagespeed checks in mod_instaweb.cc:pagespeed_post_config while
    // ngx_pagespeed checks in ngx_pagespeed.cc:ps_merge_srv_conf.
    // There is at least one example where this check is violated in
    // ngx_pagespeed. Example:
    // server {
    //   pagespeed off;
    //   pagespeed FileCachePath "/tmp";
    //   location / {
    //     pagespeed on;
    //   }
    // }
    //
    // Fixing this would require knowing if pagespeed is ever switched on within
    // a deeper level of the block. When this is parsed, we just have knowledge
    // of the higher-level server block.
    StringPiece path(config->file_cache_path());
    cache_flush_filename_ = StrCat(
        path, (path.size() > 0 && strings::EndsWith(path, "/")) ? "" : "/",
        cache_flush_filename_);
  }

  if (config->use_shared_mem_locking()) {
    shared_mem_lock_manager_ = std::make_unique<SharedMemLockManager>(
        shm_runtime, LockManagerSegmentName(), factory->scheduler(),
        factory->hasher(), factory->message_handler());
    lock_manager_ = shared_mem_lock_manager_.get();
  } else {
    FallBackToFileBasedLocking();
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

  cache_backend_ = new CycloneCache(
      cyclone_config, factory->statistics(), factory->message_handler());
  factory->TakeOwnership(cache_backend_);
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
    FallBackToFileBasedLocking();
  }
}

void SystemCachePath::ChildInit(SlowWorker* cache_clean_worker) {
  if (unplugged_) {
    return;
  }
  factory_->message_handler()->Message(
      kInfo, "Reusing shared memory for path: %s.", path_.c_str());
  if ((shared_mem_lock_manager_.get() != nullptr) &&
      !shared_mem_lock_manager_->Attach()) {
    FallBackToFileBasedLocking();
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

void SystemCachePath::FallBackToFileBasedLocking() {
  if ((shared_mem_lock_manager_.get() != nullptr) ||
      (lock_manager_ == nullptr)) {
    shared_mem_lock_manager_.reset(nullptr);
    file_system_lock_manager_ = std::make_unique<FileSystemLockManager>(
        factory_->file_system(), path_, factory_->scheduler(),
        factory_->message_handler());
    lock_manager_ = file_system_lock_manager_.get();
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
