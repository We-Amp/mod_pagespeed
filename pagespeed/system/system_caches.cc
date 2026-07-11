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

#include "pagespeed/system/system_caches.h"

#include <cstddef>
#include <cstdlib>
#include <memory>
#include <tuple>
#include <utility>

#include "base/logging.h"
#include "net/instaweb/http/public/http_cache.h"
#include "net/instaweb/rewriter/public/rewrite_driver.h"
#include "net/instaweb/rewriter/public/rewrite_driver_factory.h"
#include "net/instaweb/rewriter/public/rewrite_options.h"
#include "net/instaweb/rewriter/public/server_context.h"
#include "net/instaweb/util/public/property_cache.h"
#include "pagespeed/kernel/base/abstract_shared_mem.h"
#include "pagespeed/kernel/base/md5_hasher.h"
#include "pagespeed/kernel/base/message_handler.h"
#include "pagespeed/kernel/base/statistics.h"
#include "pagespeed/kernel/base/string.h"
#include "pagespeed/kernel/base/string_writer.h"
#include "pagespeed/kernel/base/thread_system.h"
#include "pagespeed/kernel/cache/async_cache.h"
#include "pagespeed/kernel/cache/cache_batcher.h"
#include "pagespeed/kernel/cache/cache_stats.h"
#include "pagespeed/kernel/cache/compressed_cache.h"
#include "pagespeed/kernel/cache/cyclone_cache.h"
#include "pagespeed/kernel/cache/fallback_cache.h"
#include "pagespeed/kernel/cache/purge_context.h"
#include "pagespeed/kernel/cache/write_through_cache.h"
#include "pagespeed/kernel/thread/queued_worker_pool.h"
#if PAGESPEED_ENABLE_MEMCACHED
#include "pagespeed/system/memcached_cache.h"
#endif
#include "pagespeed/system/external_server_spec.h"
#include "pagespeed/system/redis_cache.h"
#include "pagespeed/system/system_cache_path.h"
#include "pagespeed/system/system_rewrite_options.h"
#include "pagespeed/system/system_server_context.h"

namespace net_instaweb {

const char SystemCaches::kMemcachedAsync[] = "memcached_async";
const char SystemCaches::kMemcachedBlocking[] = "memcached_blocking";
const char SystemCaches::kRedisAsync[] = "redis_async";
const char SystemCaches::kRedisBlocking[] = "redis_blocking";
const char SystemCaches::kShmCache[] = "shm_cache";
const char SystemCaches::kDefaultSharedMemoryPath[] = "pagespeed_default_shm";

SystemCaches::SystemCaches(RewriteDriverFactory* factory,
                           AbstractSharedMem* shm_runtime, int thread_limit)
    : factory_(factory),
      shared_mem_runtime_(shm_runtime),
      thread_limit_(thread_limit),
      is_root_process_(true),
      was_shut_down_(false),
      cache_hasher_(20),
      default_shm_metadata_cache_creation_failed_(false) {}

SystemCaches::~SystemCaches() { DCHECK(was_shut_down_); }

void SystemCaches::ShutDown(MessageHandler* message_handler) {
  DCHECK(!was_shut_down_);
  if (was_shut_down_) {
    return;
  }

  was_shut_down_ = true;

  // Take down any threads serving external caches, then wait for shut down to
  // complete and free memory. Note that this will block waiting for any
  // operations currently in progress to terminate. It's the reason we initiate
  // shutdown and wait for it instead of simple calling ShutDown().
  //
  // In case of memcached it can possibly require kill -9 to restart Apache if
  // memcached is permanently hung. In practice, the libmemcached-backed
  // MemcachedCache I/O timeouts make that very unlikely.
  //
  // The alternative scenario of exiting with pending I/O will often
  // crash and always leak memory. Note that if memcached crashes, as
  // opposed to hanging, it will probably not appear wedged.
#if PAGESPEED_ENABLE_MEMCACHED
  if (memcached_pool_) {
    memcached_pool_->InitiateShutDown();
  }
#endif
  if (redis_pool_) {
    redis_pool_->InitiateShutDown();
  }
#if PAGESPEED_ENABLE_MEMCACHED
  if (memcached_pool_) {
    memcached_pool_->WaitForShutDownComplete();
    memcached_pool_.reset(nullptr);
  }
#endif
  if (redis_pool_) {
    redis_pool_->WaitForShutDownComplete();
    redis_pool_.reset(nullptr);
  }

  if (is_root_process_) {
    // Cleanup per-path shm resources.
    for (PathCacheMap::iterator p = path_cache_map_.begin(),
                                e = path_cache_map_.end();
         p != e; ++p) {
      p->second->GlobalCleanup(message_handler);
    }

    // And all the SHM caches.
    for (MetadataShmCacheMap::iterator p = metadata_shm_caches_.begin(),
                                       e = metadata_shm_caches_.end();
         p != e; ++p) {
      if (p->second->cache_backend != nullptr && p->second->initialized) {
        MetadataShmCache::GlobalCleanup(shared_mem_runtime_, p->second->segment,
                                        message_handler);
      }
    }
  }
}

SystemCachePath* SystemCaches::GetCache(SystemRewriteOptions* config) {
  GoogleString path = SystemCachePath::CachePath(config);
  SystemCachePath* system_cache_path = nullptr;
  std::pair<PathCacheMap::iterator, bool> result =
      path_cache_map_.insert(PathCacheMap::value_type(path, system_cache_path));
  PathCacheMap::iterator iter = result.first;
  if (result.second) {
    iter->second = system_cache_path =
        new SystemCachePath(path, config, factory_, shared_mem_runtime_);
    factory_->TakeOwnership(system_cache_path);
  } else {
    system_cache_path = iter->second;
  }
  return system_cache_path;
}

SystemCaches::ExternalCacheInterfaces
SystemCaches::ConstructExternalCacheInterfacesFromBlocking(
    CacheInterface* backend, QueuedWorkerPool* pool,
    int batcher_max_parallel_lookups, const char* async_stats_name,
    const char* blocking_stats_name) {
  ExternalCacheInterfaces result;

  if (pool == nullptr) {
    result.async = backend;
  } else {
    result.async = new AsyncCache(backend, pool);
    factory_->TakeOwnership(result.async);
  }

  // Put the batcher above the stats so that the stats sees the MultiGets
  // and can show us the histogram of how they are sized.
  result.async = new CacheStats(async_stats_name, result.async,
                                factory_->timer(), factory_->statistics());
  factory_->TakeOwnership(result.async);

  CacheBatcher::Options options;
  if (batcher_max_parallel_lookups != -1) {
    options.max_parallel_lookups = batcher_max_parallel_lookups;
  }
  CacheBatcher* batcher = new CacheBatcher(
      options, result.async, factory_->thread_system()->NewMutex(),
      factory_->statistics());
  factory_->TakeOwnership(batcher);
  result.async = batcher;

  // Populate the blocking interface, giving it its own
  // statistics wrapper.
  result.blocking = new CacheStats(blocking_stats_name, backend,
                                   factory_->timer(), factory_->statistics());
  factory_->TakeOwnership(result.blocking);
  return result;
}

#if PAGESPEED_ENABLE_MEMCACHED
SystemCaches::ExternalCacheInterfaces SystemCaches::NewMemcached(
    SystemRewriteOptions* config) {
  const ExternalClusterSpec& servers_specs = config->memcached_servers();
  MemcachedCache* mem_cache = new MemcachedCache(
      servers_specs, thread_limit_, &cache_hasher_, factory_->statistics(),
      factory_->timer(), factory_->message_handler());
  factory_->TakeOwnership(mem_cache);
  mem_cache->set_timeout_us(config->memcached_timeout_us());
  memcache_servers_.push_back(mem_cache);

  int num_threads = config->memcached_threads();
  if (num_threads != 0) {
    if (num_threads != 1) {
      factory_->message_handler()->Message(
          kWarning,
          "ModPagespeedMemcachedThreads support for >1 thread "
          "is not supported yet; changing to 1 thread (was %d)",
          num_threads);
      num_threads = 1;
    }

    if (memcached_pool_.get() == nullptr) {
      // Note -- we will use the first value of ModPagespeedMemCacheThreads
      // that we see in a VirtualHost, ignoring later ones.
      memcached_pool_ = std::make_unique<QueuedWorkerPool>(
          num_threads, "memcached", factory_->thread_system());
    }
    return ConstructExternalCacheInterfacesFromBlocking(
        mem_cache, memcached_pool_.get(), num_threads, kMemcachedAsync,
        kMemcachedBlocking);
  } else {
    return ConstructExternalCacheInterfacesFromBlocking(
        mem_cache,
        nullptr,  // No worker pool.
        -1,       // Do not change batcher's max_parallel_lookups.
        kMemcachedAsync, kMemcachedBlocking);
  }
}
#endif  // PAGESPEED_ENABLE_MEMCACHED

SystemCaches::ExternalCacheInterfaces SystemCaches::NewRedis(
    SystemRewriteOptions* config) {
  const ExternalServerSpec& server_spec = config->redis_server();
  // using database index -1 when property not specified in config
  const int redis_database_index =
      config->has_redis_database_index() ? config->redis_database_index() : -1;
  const int redis_ttl_sec =
      config->has_redis_ttl_sec() ? config->redis_ttl_sec() : -1;
  RedisCache* redis_server = new RedisCache(
      server_spec.host, server_spec.port, factory_->thread_system(),
      factory_->message_handler(), factory_->timer(),
      config->redis_reconnection_delay_ms(), config->redis_timeout_us(),
      factory_->statistics(), redis_database_index, redis_ttl_sec);
  factory_->TakeOwnership(redis_server);
  redis_servers_.push_back(redis_server);
  if (redis_pool_.get() == nullptr) {
    // TODO(yeputons): consider using more than one thread and making the amount
    // configurable. For memcached using more than one thread was not boosting
    // performance and that opportunity was eventually disabled, but that could
    // be caused by some bug which is possibly already fixed.
    //
    // Looks like adding more threads won't be a win until RedisCache is async,
    // because all queries will still be queued as they require exclusive access
    // to RedisCache. Creating a separate single-threaded pool for different
    // RedisCaches could be a good idea, though.
    redis_pool_ = std::make_unique<QueuedWorkerPool>(1, "redis",
                                                     factory_->thread_system());
  }
  return ConstructExternalCacheInterfacesFromBlocking(
      redis_server, redis_pool_.get(), 1, kRedisAsync, kRedisBlocking);
}

SystemCaches::ExternalCacheInterfaces SystemCaches::NewExternalCache(
    SystemRewriteOptions* config) {
  bool use_redis = !config->redis_server().empty();
#if PAGESPEED_ENABLE_MEMCACHED
  bool use_memcached = !config->memcached_servers().empty();

  if (use_redis && use_memcached) {
    factory_->message_handler()->Message(
        kWarning,
        "Redis and Memcached are enabled simultaneously, will use "
        "Redis and ignore Memcached");
    use_memcached = false;
  }
#else
  // Memcached is not available on this platform.
  [[maybe_unused]] bool use_memcached = false;
#endif

  // Some unique signature to distinguish server configurations.
  GoogleString spec_signature;
  if (use_redis) {
    spec_signature =
        StrCat("r;", config->redis_server().ToString(), ";",
               IntegerToString(config->redis_database_index()), ";",
               IntegerToString(config->redis_reconnection_delay_ms()), ";",
               IntegerToString(config->redis_timeout_us()), ";",
               IntegerToString(config->redis_ttl_sec()));
#if PAGESPEED_ENABLE_MEMCACHED
  } else if (use_memcached) {
    spec_signature = StrCat("m;", config->memcached_servers().ToString(), ";",
                            IntegerToString(config->memcached_threads()), ";",
                            IntegerToString(config->memcached_timeout_us()));
#endif
  } else {
    return ExternalCacheInterfaces();
  }

  ExternalCachesMap::iterator iterator;
  bool inserted;
  std::tie(iterator, inserted) = external_caches_map_.insert(
      ExternalCachesMap::value_type(spec_signature, ExternalCacheInterfaces()));
  if (inserted) {
    // Was not in the map, should construct new one and store it.
    if (use_redis) {
      iterator->second = NewRedis(config);
#if PAGESPEED_ENABLE_MEMCACHED
    } else if (use_memcached) {
      iterator->second = NewMemcached(config);
#endif
    }
  }

  // Some per-VirtualHost modifications follow, we do not want to store them in
  // map.
  ExternalCacheInterfaces result = iterator->second;
#if PAGESPEED_ENABLE_MEMCACHED
  if (use_memcached) {
    // Note that a distinct FallbackCache gets created for every VirtualHost
    // that employs memcached, even if the memcached and file-cache
    // specifications are identical.  This does no harm, because there
    // is no data in the cache object itself; just configuration.  Sharing
    // FallbackCache* objects would require making a map using the
    // memcache & file-cache specs as a key (and possibly duplicating memcache
    // objects as a result), so it's simpler to make a new small FallbackCache
    // object for each VirtualHost.

    CacheInterface* file_cache = GetCache(config)->file_cache();

    result.async = new FallbackCache(result.async, file_cache,
                                     MemcachedCache::kValueSizeThreshold,
                                     factory_->message_handler());
    factory_->TakeOwnership(result.async);

    result.blocking = new FallbackCache(result.blocking, file_cache,
                                        MemcachedCache::kValueSizeThreshold,
                                        factory_->message_handler());
    factory_->TakeOwnership(result.blocking);
  }
#endif
  return result;
}

bool SystemCaches::CreateShmMetadataCache(StringPiece name, int64 size_kb,
                                          GoogleString* error_msg) {
  MetadataShmCacheInfo* cache_info = nullptr;
  std::pair<MetadataShmCacheMap::iterator, bool> result =
      metadata_shm_caches_.insert(
          MetadataShmCacheMap::value_type(name.as_string(), cache_info));
  if (result.second) {
    int entries, blocks;
    int64 size_cap;
    const int kSectors = 128;
    MetadataShmCache::ComputeDimensions(
        size_kb, 2 /* block/entry ratio, based empirically off load tests */,
        kSectors, &entries, &blocks, &size_cap);

    // Make sure the size cap is not unusably low. In particular, with 2K
    // inlining thresholds, something like 3K is needed. (As of time of writing,
    // that required about 4.3MiB).
    if (size_cap < static_cast<int64>(3 * 1024)) {
      metadata_shm_caches_.erase(result.first);
      *error_msg = "Shared memory cache unusably small.";
      return false;
    } else {
      cache_info = new MetadataShmCacheInfo;
      factory_->TakeOwnership(cache_info);
      cache_info->segment = StrCat(name, "/metadata_cache");
      cache_info->cache_backend = new SharedMemCache<64>(
          shared_mem_runtime_, cache_info->segment, factory_->timer(),
          factory_->hasher(), kSectors, entries, /* entries per sector */
          blocks /* blocks per sector*/, factory_->message_handler());
      factory_->TakeOwnership(cache_info->cache_backend);
      // We can't set cache_info->cache_to_use yet since statistics aren't ready
      // yet. It will happen in ::RootInit().
      result.first->second = cache_info;
      return true;
    }
  } else if (name == kDefaultSharedMemoryPath) {
    // If the default shared memory cache already exists, and we try to create
    // it again, that's not a problem.  This happens because when we check if
    // the cache exists yet we look at MetadataShmCacheInfo->cache_to_use which
    // isn't actually set until RootInit().
    return true;
  } else {
    *error_msg = StrCat("Cache named ", name, " already exists.");
    return false;
  }
}

NamedLockManager* SystemCaches::GetLockManager(SystemRewriteOptions* config) {
  return GetCache(config)->lock_manager();
}

SystemCaches::MetadataShmCacheInfo* SystemCaches::LookupShmMetadataCache(
    const GoogleString& name) {
  if (name.empty()) {
    return nullptr;
  }
  MetadataShmCacheMap::iterator i = metadata_shm_caches_.find(name);
  if (i != metadata_shm_caches_.end()) {
    return i->second;
  }
  return nullptr;
}

SystemCaches::MetadataShmCacheInfo* SystemCaches::GetShmMetadataCacheOrDefault(
    SystemRewriteOptions* config) {
  MetadataShmCacheInfo* shm_cache =
      LookupShmMetadataCache(config->file_cache_path());
  if (shm_cache != nullptr) {
    return shm_cache;  // Explicitly configured.
  }
  if (shared_mem_runtime_->IsDummy()) {
    // We're on a system that doesn't actually support shared memory.
    return nullptr;
  }
  if (config->default_shared_memory_cache_kb() == 0) {
    return nullptr;  // User has disabled the default shm cache.
  }
  shm_cache = LookupShmMetadataCache(kDefaultSharedMemoryPath);
  if (shm_cache != nullptr) {
    return shm_cache;  // Using the default shm cache, which already exists.
  }
  if (default_shm_metadata_cache_creation_failed_) {
    return nullptr;  // Already tried to create the default shm cache and
                     // failed.
  }
  // This config is for the first server context to need the default cache;
  // create it.
  GoogleString error_msg;
  bool ok = CreateShmMetadataCache(kDefaultSharedMemoryPath,
                                   config->default_shared_memory_cache_kb(),
                                   &error_msg);
  if (!ok) {
    factory_->message_handler()->Message(
        kWarning, "Default shared memory cache: %s", error_msg.c_str());
    default_shm_metadata_cache_creation_failed_ = true;
    return nullptr;
  }
  return LookupShmMetadataCache(kDefaultSharedMemoryPath);
}

void SystemCaches::SetupPcacheCohorts(ServerContext* server_context,
                                      bool enable_property_cache) {
  server_context->set_enable_property_cache(enable_property_cache);
  PropertyCache* pcache = server_context->page_property_cache();
  server_context->set_beacon_cohort(
      server_context->AddCohort(RewriteDriver::kBeaconCohort, pcache));
  server_context->set_dom_cohort(
      server_context->AddCohort(RewriteDriver::kDomCohort, pcache));
  server_context->set_dependencies_cohort(
      server_context->AddCohort(RewriteDriver::kDependenciesCohort, pcache));
}

void SystemCaches::SetupCaches(ServerContext* server_context,
                               bool enable_property_cache) {
  SystemRewriteOptions* config =
      dynamic_cast<SystemRewriteOptions*>(server_context->global_options());
  DCHECK(config != nullptr);
  SystemCachePath* caches_for_path = GetCache(config);
  CacheInterface* lru_cache = caches_for_path->lru_cache();
  CacheInterface* file_cache = caches_for_path->file_cache();
  MetadataShmCacheInfo* shm_metadata_cache_info =
      GetShmMetadataCacheOrDefault(config);
  CacheInterface* shm_metadata_cache =
      (shm_metadata_cache_info != nullptr)
          ? shm_metadata_cache_info->cache_to_use
          : nullptr;
  CacheInterface* property_store_cache = nullptr;
  CacheInterface* http_l2 = file_cache;
  Statistics* stats = server_context->statistics();

  ExternalCacheInterfaces external_cache = NewExternalCache(config);
  if (external_cache.async != nullptr) {
    CHECK(external_cache.blocking != nullptr);

    http_l2 = external_cache.async;

    // Use the blocking version of our external cache for the filesystem
    // metadata cache AND the property store cache.  Note that if there is
    // a shared-memory cache, then we will override this setting and use it
    // for the filesystem metadata cache below.
    server_context->set_filesystem_metadata_cache(external_cache.blocking);
    property_store_cache = external_cache.blocking;
  }

  // Figure out our L1/L2 hierarchy for http cache.
  // TODO(jmarantz): consider moving ownership of the LRU cache into the
  // factory, rather than having one per vhost.
  //
  // Note that a user can disable the LRU cache by setting its byte-count
  // to 0, and in fact this is the default setting.
  int64 max_content_length = config->max_cacheable_response_content_length();
  HTTPCache* http_cache = nullptr;
  if (lru_cache == nullptr) {
    // No L1, and so backend is just the L2.
    http_cache =
        new HTTPCache(http_l2, factory_->timer(), factory_->hasher(), stats);
    http_cache->SetCompressionLevel(config->http_cache_compression_level());
  } else {
    // L1 is LRU, with the L2 as computed above.
    WriteThroughCache* write_through_http_cache =
        new WriteThroughCache(lru_cache, http_l2);
    server_context->DeleteCacheOnDestruction(write_through_http_cache);
    write_through_http_cache->set_cache1_limit(config->lru_cache_byte_limit());
    http_cache = new HTTPCache(write_through_http_cache, factory_->timer(),
                               factory_->hasher(), stats);
    http_cache->set_cache_levels(2);
    http_cache->SetCompressionLevel(config->http_cache_compression_level());
  }

  http_cache->set_max_cacheable_response_content_length(max_content_length);
  // Zero-copy serving of memory-mapped (Cyclone) cache hits, default off.
  // Applies to both cache topologies above: with an LRU L1 the
  // WriteThroughCache forwards the backend's MappedSharedString by
  // reference, so mapped-ness survives the L1/L2 stack.
  http_cache->set_cyclone_zero_copy_enabled(config->cyclone_zero_copy());
  http_cache->set_cyclone_zero_copy_serve_enabled(
      config->cyclone_zero_copy_serve());
  server_context->set_http_cache(http_cache);

  // And now the metadata cache. If we only have one level, it will be in
  // metadata_l2, with metadata_l1 set to NULL.
  CacheInterface* metadata_l1 = nullptr;
  CacheInterface* metadata_l2 = nullptr;
  size_t l1_size_limit = WriteThroughCache::kUnlimited;
  if (shm_metadata_cache != nullptr) {
    if (external_cache.async != nullptr) {
      // If we have both a local SHM cache and a cache on an external server
      // we should go L1/L2 because there are likely to be other machines
      // that would like to use our metadata.
      metadata_l1 = shm_metadata_cache;
      metadata_l2 = external_cache.async;

      // Because external cache share the metadata cache across machines,
      // we need a filesystem metadata cache to validate LoadFromFile
      // entries.  We default to using external cache for that, even though
      // the LoadFromFile metadata is usually local to the machine,
      // unless the user specifies an NFS directory in LoadFromFile.
      // This is OK because it is keyed to the machine name.  But if
      // we have a shm cache, then use it instead for the metadata
      // cache.
      //
      // Note that we don't need to use a writethrough or fallback
      // strategy as the data is reasonably inexpensive to recompute
      // on a restart, unlike the metadata_cache which has
      // optimization results, and the payloads are all small (message
      // InputInfo in ../rewriter/cached_result.proto).
      server_context->set_filesystem_metadata_cache(shm_metadata_cache);

      // Durability here comes from the external L2 cache itself: shared memory
      // is a volatile L1 in front of it, and any entry not resident in shm
      // after a restart is re-fetched from the external cache on demand.
    } else {
      // Persist metadata across restarts by writing every entry through to the
      // Cyclone-backed disk cache, while serving hot reads from shared memory.
      // The shared-memory cache is the fast L1; Cyclone is the durable L2 and
      // source of truth, so a restart (deploy, crash, cache re-root) does not
      // discard optimization decisions and force re-derivation on first hit.
      // Entries larger than the shm value cap skip L1 and live in Cyclone only;
      // small entries land in both. The WriteThroughCache is assembled below,
      // from metadata_l1/metadata_l2, once l1_size_limit is set.
      //
      // Metadata routes to the small-object tier of the Cyclone cache
      // (FileCacheSmallTierPercent): rname/ entries are tiny compared to the
      // HTTP payloads they co-mingled with, and volume eviction churn from
      // payload traffic used to drop them wholesale, defeating the
      // restart-warmth this write-through exists to provide. The small tier
      // is a physically separate volume, so payload churn can no longer
      // evict metadata. When the tier is disabled or inactive this is the
      // same cache as file_cache().
      metadata_l1 = shm_metadata_cache;
      metadata_l2 = caches_for_path->small_tier_file_cache();
      l1_size_limit = shm_metadata_cache_info->cache_backend->MaxValueSize();

      // Give the property store the same arrangement: shared memory is the
      // fast L1 for property lookups; Cyclone is the durable L2 so
      // beacon-derived page properties (critical images/selectors, dom stats)
      // survive restarts instead of waiting for beacons to re-arrive over live
      // traffic. This is a separate WriteThroughCache instance from the
      // metadata one assembled below, so each gets its own CompressedCache
      // wrap when compress_metadata_cache is on. Values over the shm cap skip
      // L1 and live in Cyclone only. Unlike FallbackCache there is no
      // account_for_key_size(false) equivalent, so the key counts against the
      // L1 limit too; SharedMemCache uses hash-produced fixed-size keys
      // internally, so the slight over-counting only steers a few
      // borderline-sized values to Cyclone alone -- same as the metadata
      // cache above, and harmless.
      WriteThroughCache* pcache_write_through = new WriteThroughCache(
          shm_metadata_cache, caches_for_path->small_tier_file_cache());
      pcache_write_through->set_cache1_limit(
          shm_metadata_cache_info->cache_backend->MaxValueSize());
      server_context->DeleteCacheOnDestruction(pcache_write_through);
      property_store_cache = pcache_write_through;

      // TODO(jmarantz): do we really want to use the shm-cache as a
      // pcache?  The potential for inconsistent data across a
      // multi-server setup seems like it could give confusing results.
    }
  } else {
    l1_size_limit = config->lru_cache_byte_limit();
    metadata_l1 = lru_cache;  // may be NULL
    metadata_l2 = http_l2;    // external or file cache.
  }

  CacheInterface* metadata_cache;

  if (metadata_l1 != nullptr) {
    WriteThroughCache* write_through_cache =
        new WriteThroughCache(metadata_l1, metadata_l2);
    server_context->DeleteCacheOnDestruction(write_through_cache);
    write_through_cache->set_cache1_limit(l1_size_limit);
    metadata_cache = write_through_cache;
  } else {
    metadata_cache = metadata_l2;
  }

  // TODO(jmarantz): We probably want to store HTTP cache compressed
  // even without this flag, but we should do it differently, storing
  // only the content compressed and putting in content-encoding:gzip
  // so that mod_gzip doesn't have to recompress on every request.
  if (property_store_cache == nullptr) {
    property_store_cache = metadata_l2;
  }
  if (config->compress_metadata_cache()) {
    metadata_cache = new CompressedCache(metadata_cache, stats);
    server_context->DeleteCacheOnDestruction(metadata_cache);
    property_store_cache = new CompressedCache(property_store_cache, stats);
    server_context->DeleteCacheOnDestruction(property_store_cache);
  }
  DCHECK(property_store_cache->IsBlocking());
  server_context->MakePagePropertyCache(
      server_context->CreatePropertyStore(property_store_cache));
  server_context->set_metadata_cache(metadata_cache);
  SetupPcacheCohorts(server_context, enable_property_cache);
  SystemServerContext* system_server_context =
      dynamic_cast<SystemServerContext*>(server_context);
  system_server_context->SetCachePath(caches_for_path);
}

void SystemCaches::RegisterConfig(SystemRewriteOptions* config) {
  // Should fill in path_cache_map_.
  GetCache(config);
  // Should fill in external_caches_map_, memcache_servers_, and
  // redis_servers_.
  NewExternalCache(config);

  // GetShmMetadataCacheOrDefault will create a default cache if one is needed
  // and doesn't exist yet.
  GetShmMetadataCacheOrDefault(config);
}

void SystemCaches::RootInit() {
  for (MetadataShmCacheMap::iterator p = metadata_shm_caches_.begin(),
                                     e = metadata_shm_caches_.end();
       p != e; ++p) {
    MetadataShmCacheInfo* cache_info = p->second;

    if (cache_info->cache_backend->Initialize()) {
      cache_info->initialized = true;
      cache_info->cache_to_use =
          new CacheStats(kShmCache, cache_info->cache_backend,
                         factory_->timer(), factory_->statistics());
      factory_->TakeOwnership(cache_info->cache_to_use);
    } else {
      factory_->message_handler()->Message(
          kWarning, "Unable to initialize shared memory cache: %s.",
          p->first.c_str());
      cache_info->cache_backend = nullptr;
      cache_info->cache_to_use = nullptr;
    }
  }

  for (PathCacheMap::iterator p = path_cache_map_.begin(),
                              e = path_cache_map_.end();
       p != e; ++p) {
    SystemCachePath* cache = p->second;
    cache->RootInit();
  }
}

void SystemCaches::ChildInit() {
  is_root_process_ = false;

  for (MetadataShmCacheMap::iterator p = metadata_shm_caches_.begin(),
                                     e = metadata_shm_caches_.end();
       p != e; ++p) {
    MetadataShmCacheInfo* cache_info = p->second;
    if ((cache_info->cache_backend != nullptr) &&
        !cache_info->cache_backend->Attach()) {
      factory_->message_handler()->Message(
          kWarning, "Unable to attach to shared memory cache: %s.",
          p->first.c_str());
      delete cache_info->cache_backend;
      cache_info->cache_backend = nullptr;
      cache_info->cache_to_use = nullptr;
    }
  }

  for (PathCacheMap::iterator p = path_cache_map_.begin(),
                              e = path_cache_map_.end();
       p != e; ++p) {
    SystemCachePath* cache = p->second;
    cache->ChildInit();
  }

  // TODO(yeputons): think about moving StartUp() to some base class of
  // RedisCache and MemcachedCache and collapsing these two loops into one.
#if PAGESPEED_ENABLE_MEMCACHED
  for (int i = 0, n = memcache_servers_.size(); i < n; ++i) {
    MemcachedCache* mem_cache = memcache_servers_[i];
    // TODO(yeputons): looks like this line does not really "connect", but just
    // loads list of servers into apr_memcached and connects later. Maybe the
    // name should be fixed.
    if (!mem_cache->Connect()) {
      factory_->message_handler()->MessageS(kError, "Memory cache failed");
      abort();  // TODO(jmarantz): is there a better way to exit?
    }
  }
#endif

  for (RedisCache* redis_cache : redis_servers_) {
    redis_cache->StartUp();
  }
}

void SystemCaches::StopCacheActivity() {
  if (is_root_process_) {
    // No caches used in root process, so nothing to shutdown.  We could run the
    // shutdown code anyway, except that starts a thread which is unsafe to do
    // in a forking server like Nginx.
    return;
  }

  // Iterate through the map of ExternalCacheInterface objects constructed and
  // try to stop pending operations on async caches. Note that these are not
  // typically MemcachedCache* or RedisCache* objects, but instead are a hierarchy
  // of CacheStats*, CacheBatcher*, AsyncCache*, all of which must be stopped.
  for (const auto& item : external_caches_map_) {
    ExternalCacheInterfaces cache = item.second;
    cache.async->ShutDown();
  }

  // TODO(morlovich): Also shutdown shm caches
}

void SystemCaches::InitStats(Statistics* statistics) {
#if PAGESPEED_ENABLE_MEMCACHED
  MemcachedCache::InitStats(statistics);
#endif
  CycloneCache::InitStats(statistics);
  CacheStats::InitStats(SystemCachePath::kFileCache, statistics);
  CacheStats::InitStats(SystemCachePath::kFileCacheSmall, statistics);
  CacheStats::InitStats(kShmCache, statistics);
#if PAGESPEED_ENABLE_MEMCACHED
  CacheStats::InitStats(kMemcachedAsync, statistics);
  CacheStats::InitStats(kMemcachedBlocking, statistics);
#endif
  CacheStats::InitStats(kRedisAsync, statistics);
  CacheStats::InitStats(kRedisBlocking, statistics);
  CompressedCache::InitStats(statistics);
  PurgeContext::InitStats(statistics);
  RedisCache::InitStats(statistics);
}

void SystemCaches::PrintCacheStats(StatFlags flags, GoogleString* out) {
  // We don't want to print this in per-vhost info since it would leak
  // all the declared caches.
  if (flags & kGlobalView) {
    for (MetadataShmCacheMap::iterator p = metadata_shm_caches_.begin(),
                                       e = metadata_shm_caches_.end();
         p != e; ++p) {
      MetadataShmCacheInfo* cache_info = p->second;
      if (cache_info->cache_backend != nullptr) {
        StrAppend(out, "\nShared memory metadata cache '", p->first,
                  "' statistics:\n");
        StringWriter writer(out);
        writer.Write(cache_info->cache_backend->DumpStats(),
                     factory_->message_handler());
      }
    }

    for (PathCacheMap::iterator p = path_cache_map_.begin(),
                                e = path_cache_map_.end();
         p != e; ++p) {
      CycloneCache* cyclone_cache = p->second->cyclone_cache();
      if (cyclone_cache != nullptr && cyclone_cache->IsHealthy()) {
        StrAppend(out, "\nCyclone cache '", cyclone_cache->config().cache_path,
                  "' statistics:\n");
        cyclone_cache->PrintStats(out);
      }
    }
  }

#if PAGESPEED_ENABLE_MEMCACHED
  if (flags & kIncludeMemcached) {
    for (int i = 0, n = memcache_servers_.size(); i < n; ++i) {
      MemcachedCache* mem_cache = memcache_servers_[i];
      if (!mem_cache->GetStatus(out)) {
        StrAppend(out, "\nError getting memcached server status for ",
                  mem_cache->cluster_spec().ToString());
      }
    }
  }
#endif

  if (flags & kIncludeRedis) {
    for (RedisCache* redis : redis_servers_) {
      // We can have partial failures, where some cluster machines give errors
      // and others don't, so have GetStatus handle error reporting.
      redis->GetStatus(out);
    }
  }
}

}  // namespace net_instaweb
