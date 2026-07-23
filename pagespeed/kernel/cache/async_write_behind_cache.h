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

#ifndef PAGESPEED_KERNEL_CACHE_ASYNC_WRITE_BEHIND_CACHE_H_
#define PAGESPEED_KERNEL_CACHE_ASYNC_WRITE_BEHIND_CACHE_H_

#include <cstddef>

#include "pagespeed/kernel/base/atomic_bool.h"
#include "pagespeed/kernel/base/atomic_int32.h"
#include "pagespeed/kernel/base/basictypes.h"
#include "pagespeed/kernel/base/shared_string.h"
#include "pagespeed/kernel/base/string.h"
#include "pagespeed/kernel/base/string_util.h"
#include "pagespeed/kernel/cache/cache_interface.h"
#include "pagespeed/kernel/thread/queued_worker_pool.h"

namespace net_instaweb {

// A put-only asynchronous write-behind wrapper around a blocking cache.
//
// Unlike AsyncCache, which pushes *every* operation (including Gets) onto a
// worker pool and reports IsBlocking()==false, this wrapper defers only the
// mutating operations (Put/Delete) while leaving reads synchronous and
// pass-through.  It is intended to sit between a WriteThroughCache and a slow
// blocking L2 tier (e.g. the Cyclone small-object disk tier used for metadata),
// so that a slow L2 write no longer holds the calling thread on the rewrite
// critical path.
//
// Correctness contract (this wrapper is only safe under these assumptions):
//   * There is a synchronous L1 in front of this cache that provides
//     read-your-writes for the same machine (the enclosing WriteThroughCache's
//     shm/LRU L1).  Reads consult L1 first, so a deferred L2 write is invisible
//     to a subsequent read only for entries that are ALSO absent from L1.
//   * Entries that skip L1 (key.size()+value.size() >= sync_put_size_threshold,
//     mirroring WriteThroughCache::set_cache1_limit) are therefore written
//     SYNCHRONOUSLY here, so read-your-writes still holds for every key.
//   * All deferred mutations run on a single QueuedWorkerPool sequence (one
//     thread, strict FIFO), so a Put and a later Delete of the same key can
//     never reorder relative to each other.
//
// Because only Put/Delete are deferred and Get/MultiGet remain synchronous,
// IsBlocking() honestly reflects the wrapped cache's read guarantee (true for a
// blocking backend).  The lock-free read path is preserved unchanged; mapped
// (zero-copy) values returned by the backend flow straight through the callback.
//
// The wrapper owns neither the backend cache nor the worker pool.  The pool
// MUST be quiesced (InitiateShutDown + WaitForShutDownComplete, or ShutDown())
// before the backend cache is destroyed, so that no deferred task outlives the
// backing store.
class AsyncWriteBehindCache : public CacheInterface {
 public:
  // Sentinel threshold meaning "never force a synchronous Put based on size"
  // (matches WriteThroughCache::kUnlimited).
  static const size_t kUnlimited;

  // Bounds the number of deferred mutations that can queue up while the backend
  // is slow.  When exceeded, the oldest queued Put/Delete is dropped (its
  // Cancel runs); metadata is re-derivable so a dropped L2 write is recoverable.
  static const int64 kMaxQueueSize = 100000;

  // Does not take ownership of 'cache' or 'pool'.  A dedicated single-thread
  // pool (or a pool used only by this wrapper) is required to preserve the
  // strict-FIFO ordering guarantee.  'sync_put_size_threshold' must equal the
  // enclosing WriteThroughCache's cache1 size limit so this wrapper makes the
  // exact same L1-skip decision (pass kUnlimited if the L1 is unbounded).
  AsyncWriteBehindCache(CacheInterface* cache, QueuedWorkerPool* pool,
                        size_t sync_put_size_threshold);
  ~AsyncWriteBehindCache() override;

  static GoogleString FormatName(StringPiece name);
  GoogleString Name() const override { return FormatName(cache_->Name()); }

  // Reads pass straight through to the blocking backend (synchronous).
  void Get(const GoogleString& key, Callback* callback) override;
  void MultiGet(MultiGetRequest* request) override;

  // Writes are deferred FIFO, except entries that skip L1 (see class comment),
  // which are written synchronously to preserve read-your-writes.
  void Put(const GoogleString& key, const SharedString& value) override;
  void Delete(const GoogleString& key) override;

  // Only Put/Delete are deferred, so the read guarantee is the backend's.
  bool IsBlocking() const override { return cache_->IsBlocking(); }

  bool IsHealthy() const override {
    return !stopped_.value() && cache_->IsHealthy();
  }

  void ShutDown() override;

  CacheInterface* Backend() override { return cache_->Backend(); }

  // Number of deferred mutations not yet retired.  Meaningful only when the
  // wrapper is quiescent (used by tests and the destructor DCHECK).
  int32 outstanding_operations() { return outstanding_operations_.value(); }

 private:
  // Returns true iff a value of this size would be excluded from the L1 by the
  // enclosing WriteThroughCache, and therefore must be written synchronously.
  bool SkipsL1(const GoogleString& key, const SharedString& value) const {
    return (sync_put_size_threshold_ != kUnlimited) &&
           (key.size() + value.size() >= sync_put_size_threshold_);
  }

  // Encode-aware synchronous store into the backend.  'value' has already had
  // the key encoded into it (when the backend requires it) by the caller.
  void PutEncoded(const GoogleString& key, const SharedString& value);

  // Deferred-task bodies executed on sequence_.
  void DoPut(GoogleString* key, const SharedString value);
  void CancelPut(GoogleString* key, const SharedString value);
  void DoDelete(GoogleString* key);
  void CancelDelete(GoogleString* key);

  CacheInterface* cache_;
  QueuedWorkerPool::Sequence* sequence_;
  size_t sync_put_size_threshold_;
  AtomicBool stopped_;
  AtomicInt32 outstanding_operations_;

  AsyncWriteBehindCache(const AsyncWriteBehindCache&) = delete;
  AsyncWriteBehindCache& operator=(const AsyncWriteBehindCache&) = delete;
};

}  // namespace net_instaweb

#endif  // PAGESPEED_KERNEL_CACHE_ASYNC_WRITE_BEHIND_CACHE_H_
