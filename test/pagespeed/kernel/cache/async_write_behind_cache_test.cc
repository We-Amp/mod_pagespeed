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

// Unit-test AsyncWriteBehindCache, using a ThreadsafeCache over LRUCache as a
// stand-in for a blocking L2 backend.

#include "pagespeed/kernel/cache/async_write_behind_cache.h"

#include <cstddef>
#include <memory>

#include "base/logging.h"
#include "pagespeed/kernel/base/abstract_mutex.h"
#include "pagespeed/kernel/base/basictypes.h"
#include "pagespeed/kernel/base/shared_string.h"
#include "pagespeed/kernel/base/string.h"
#include "pagespeed/kernel/base/string_util.h"
#include "pagespeed/kernel/base/thread_annotations.h"
#include "pagespeed/kernel/base/thread_system.h"
#include "pagespeed/kernel/base/timer.h"
#include "pagespeed/kernel/cache/lru_cache.h"
#include "pagespeed/kernel/cache/threadsafe_cache.h"
#include "pagespeed/kernel/thread/queued_worker_pool.h"
#include "pagespeed/kernel/util/platform.h"
#include "test/pagespeed/kernel/base/gtest.h"
#include "test/pagespeed/kernel/cache/cache_test_base.h"
#include "test/pagespeed/kernel/thread/worker_test_base.h"

namespace net_instaweb {

namespace {
const size_t kMaxBytes = 10000;
// Size threshold below which a Put is deferred; a key+value at or above this
// "skips L1" and must be written synchronously.
const size_t kSyncThreshold = 8;
}  // namespace

class AsyncWriteBehindCacheTest : public CacheTestBase {
 protected:
  // A blocking backend whose Put/Delete can be parked on a sync-point so the
  // test can deterministically observe the deferred-write path.  Reads are
  // never gated (they pass straight through, as the shim relies on).
  class GatedCache : public ThreadsafeCache {
   public:
    GatedCache(LRUCache* lru, AbstractMutex* cache_mutex,
               AbstractMutex* gate_mutex)
        : ThreadsafeCache(lru, cache_mutex),
          gate_mutex_(gate_mutex),
          entered_(nullptr),
          release_(nullptr) {}

    // 'entered' is notified when a mutation begins on the worker thread;
    // the worker then blocks until 'release' is notified.
    void SetGate(WorkerTestBase::SyncPoint* entered,
                 WorkerTestBase::SyncPoint* release) {
      ScopedMutex lock(gate_mutex_.get());
      entered_ = entered;
      release_ = release;
    }

    void ClearGate() {
      ScopedMutex lock(gate_mutex_.get());
      entered_ = nullptr;
      release_ = nullptr;
    }

    void Put(const GoogleString& key, const SharedString& value) override {
      PassGate();
      ThreadsafeCache::Put(key, value);
    }

    void Delete(const GoogleString& key) override {
      PassGate();
      ThreadsafeCache::Delete(key);
    }

   private:
    void PassGate() {
      WorkerTestBase::SyncPoint* entered;
      WorkerTestBase::SyncPoint* release;
      {
        ScopedMutex lock(gate_mutex_.get());
        entered = entered_;
        release = release_;
      }
      if (entered != nullptr) {
        entered->Notify();
      }
      if (release != nullptr) {
        release->Wait();
      }
    }

    std::unique_ptr<AbstractMutex> gate_mutex_;
    WorkerTestBase::SyncPoint* entered_ GUARDED_BY(gate_mutex_);
    WorkerTestBase::SyncPoint* release_ GUARDED_BY(gate_mutex_);
  };

  AsyncWriteBehindCacheTest()
      : lru_cache_(kMaxBytes),
        thread_system_(Platform::CreateThreadSystem()),
        timer_(thread_system_->NewTimer()),
        gated_cache_(&lru_cache_, thread_system_->NewMutex(),
                     thread_system_->NewMutex()),
        pool_shut_down_(false) {
    set_mutex(thread_system_->NewMutex());
    pool_ = std::make_unique<QueuedWorkerPool>(1, "write_behind",
                                               thread_system_.get());
  }

  ~AsyncWriteBehindCacheTest() override { ShutDownPool(); }

  void ResetShim(size_t sync_threshold) {
    shim_ = std::make_unique<AsyncWriteBehindCache>(&gated_cache_, pool_.get(),
                                                    sync_threshold);
  }

  void ShutDownPool() {
    if (!pool_shut_down_) {
      pool_shut_down_ = true;
      pool_->ShutDown();  // quiesce before destructing the shim.
    }
  }

  // Blocks until all deferred mutations have retired into the backend.
  void Drain() {
    while (shim_->outstanding_operations() > 0) {
      timer_->SleepMs(1);
    }
  }

  CacheInterface* Cache() override { return shim_.get(); }

  LRUCache lru_cache_;
  std::unique_ptr<ThreadSystem> thread_system_;
  std::unique_ptr<Timer> timer_;
  GatedCache gated_cache_;
  std::unique_ptr<QueuedWorkerPool> pool_;
  std::unique_ptr<AsyncWriteBehindCache> shim_;
  bool pool_shut_down_;
};

// Reads are synchronous pass-through, so the wrapper honestly reports the
// backend's blocking guarantee.
TEST_F(AsyncWriteBehindCacheTest, IsBlockingReportsBackend) {
  ResetShim(AsyncWriteBehindCache::kUnlimited);
  EXPECT_TRUE(shim_->IsBlocking());
}

// A deferred Put becomes visible once the queue drains.
TEST_F(AsyncWriteBehindCacheTest, PutThenGetAfterDrain) {
  ResetShim(AsyncWriteBehindCache::kUnlimited);
  CheckPut("Name", "Value");
  Drain();
  CheckGet("Name", "Value");
  CheckNotFound("Absent");
}

// With no size threshold, a normal Put is deferred: while the worker is parked
// the value is not yet in the backend, and outstanding_operations() reflects
// the queued write.  After release+drain the value lands.
TEST_F(AsyncWriteBehindCacheTest, SmallPutIsDeferred) {
  ResetShim(kSyncThreshold);
  WorkerTestBase::SyncPoint entered(thread_system_.get());
  WorkerTestBase::SyncPoint release(thread_system_.get());
  gated_cache_.SetGate(&entered, &release);

  shim_->Put("k", SharedString("v"));  // 1+1 < threshold -> deferred.
  EXPECT_EQ(1, shim_->outstanding_operations());
  entered.Wait();  // The write is executing on the worker thread, not inline.

  // The worker is parked before touching the backend, so the value is absent
  // and a same-machine read (pass-through) misses -- which is why L1 must front
  // this cache for keys handled on the deferred path.
  CheckNotFound("k");
  EXPECT_EQ(1, shim_->outstanding_operations());

  gated_cache_.ClearGate();
  release.Notify();
  Drain();
  CheckGet("k", "v");
}

// An entry that would skip L1 (key+value >= threshold) is written synchronously
// on the caller's thread: it never enters the queue and is visible immediately
// without draining.  This preserves read-your-writes for L1-bypassing entries.
TEST_F(AsyncWriteBehindCacheTest, OversizedEntryIsSynchronous) {
  ResetShim(kSyncThreshold);
  const GoogleString big_value(50, 'x');  // key+value well over threshold.
  shim_->Put("k", SharedString(big_value));

  // Never queued: the counter was untouched and the value is already present.
  EXPECT_EQ(0, shim_->outstanding_operations());
  CheckGet("k", big_value);
}

// A Put followed by a Delete of the same key retires in FIFO order: the final
// state is deleted, never resurrected by a reordered Put.
TEST_F(AsyncWriteBehindCacheTest, PutThenDeleteFifoResolvesDeleted) {
  ResetShim(AsyncWriteBehindCache::kUnlimited);
  shim_->Put("k", SharedString("v1"));
  shim_->Put("k", SharedString("v2"));
  shim_->Delete("k");
  Drain();
  CheckNotFound("k");
}

// The mirror ordering: Delete then Put leaves the value present.
TEST_F(AsyncWriteBehindCacheTest, DeleteThenPutFifoResolvesPresent) {
  ResetShim(AsyncWriteBehindCache::kUnlimited);
  shim_->Put("k", SharedString("stale"));
  shim_->Delete("k");
  shim_->Put("k", SharedString("fresh"));
  Drain();
  CheckGet("k", "fresh");
}

// Shutting the worker pool down drains the queue with no outstanding work left
// behind (and, under a sanitizer build, no leak/UAF).  This mirrors the
// SystemCaches shutdown path that quiesces the pool before the backing store
// is destroyed.
TEST_F(AsyncWriteBehindCacheTest, ShutdownDrainsQueue) {
  ResetShim(AsyncWriteBehindCache::kUnlimited);
  for (int i = 0; i < 50; ++i) {
    shim_->Put(absl::StrFormat("k%d", i), SharedString("v"));
  }
  ShutDownPool();
  EXPECT_EQ(0, shim_->outstanding_operations());
}

// The wrapper's own ShutDown() stops accepting new writes and drops pending
// ones without touching the backend after quiescence.
TEST_F(AsyncWriteBehindCacheTest, ShutDownStopsAcceptingWrites) {
  ResetShim(AsyncWriteBehindCache::kUnlimited);
  shim_->ShutDown();
  EXPECT_FALSE(shim_->IsHealthy());
  shim_->Put("k", SharedString("v"));  // dropped, not queued.
  EXPECT_EQ(0, shim_->outstanding_operations());
  ShutDownPool();
  EXPECT_EQ(0, shim_->outstanding_operations());
}

}  // namespace net_instaweb
