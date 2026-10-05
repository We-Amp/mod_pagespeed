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

#include "test/pagespeed/kernel/sharedmem/shared_mem_cache_test_base.h"

#ifdef _WIN32
#include <windows.h>
#define usleep(us) Sleep((us) / 1000)
#else
#include <unistd.h>
#endif

#include <cstddef>  // for size_t
#include <memory>
#include <vector>

#include "base/logging.h"  // for Check_EQImpl, CHECK_EQ
#include "pagespeed/kernel/base/abstract_mutex.h"
#include "pagespeed/kernel/base/abstract_shared_mem.h"
#include "pagespeed/kernel/base/function.h"
#include "pagespeed/kernel/base/shared_string.h"
#include "pagespeed/kernel/base/string_util.h"
#include "pagespeed/kernel/base/thread_annotations.h"
#include "pagespeed/kernel/base/thread_system.h"
#include "pagespeed/kernel/cache/cache_interface.h"
#include "pagespeed/kernel/sharedmem/shared_mem_cache.h"
#include "pagespeed/kernel/util/platform.h"

namespace net_instaweb {

namespace {

const char kSegment[] = "cache";
const char kAltSegment[] = "alt_cache";
const int kSectors = 2;
const int kSectorBlocks = 2000;
const int kSectorEntries = 256;
const int kSpinRuns = 100;

// In some tests we have tight consumer/producer spinloops assuming they'll get
// preempted to let other end proceed. Valgrind does not actually do that
// sometimes.
void YieldToThread() { usleep(1); }

}  // namespace

SharedMemCacheTestBase::SharedMemCacheTestBase(SharedMemTestEnv* env)
    : test_env_(env),
      shmem_runtime_(env->CreateSharedMemRuntime()),
      thread_system_(Platform::CreateThreadSystem()),
      handler_(thread_system_->NewMutex()),
      timer_(thread_system_->NewMutex(), 0),
      sanity_checks_enabled_(true) {
  cache_.reset(MakeCache());
  EXPECT_TRUE(cache_->Initialize());

  // Compute a large string for tests --- one large enough to take multiple
  // blocks (2 complete blocks, plus 43 bytes in a 3rd one, where 43
  // is a completely arbitrary small integer smaller than the block size)
  for (size_t c = 0; c < kBlockSize * 2 + 43; ++c) {
    large_.push_back('A' + (c % 26));
  }

  // Now a gigantic one, which goes close to the size limit, which is 1/32nd
  // of sector size.
  for (size_t c = 0; c < kBlockSize * kSectorBlocks / 40 + 43; ++c) {
    gigantic_.push_back('a' + (c % 26));
  }
}

SharedMemCache<SharedMemCacheTestBase::kBlockSize>*
SharedMemCacheTestBase::MakeCache() {
  return new SharedMemCache<kBlockSize>(shmem_runtime_.get(), kSegment, &timer_,
                                        &hasher_, kSectors, kSectorEntries,
                                        kSectorBlocks, &handler_);
}

void SharedMemCacheTestBase::TearDown() {
  cache_->GlobalCleanup(shmem_runtime_.get(), kSegment, &handler_);
  CacheTestBase::TearDown();
}

void SharedMemCacheTestBase::ResetCache() {
  cache_->GlobalCleanup(shmem_runtime_.get(), kSegment, &handler_);
  cache_.reset(MakeCache());
  EXPECT_TRUE(cache_->Initialize());
}

bool SharedMemCacheTestBase::CreateChild(TestMethod method) {
  Function* fn = new MemberFunction0<SharedMemCacheTestBase>(method, this);
  return test_env_->CreateChild(fn);
}

void SharedMemCacheTestBase::SanityCheck() {
  if (sanity_checks_enabled_) {
    cache_->SanityCheck();
  }
}

void SharedMemCacheTestBase::TestBasic() {
  CheckNotFound("404");
  CheckDelete("404");
  CheckNotFound("404");

  CheckPut("200", "OK");
  CheckGet("200", "OK");
  CheckNotFound("404");

  CheckPut("002", "KO!");
  CheckGet("002", "KO!");
  CheckGet("200", "OK");
  CheckNotFound("404");

  CheckDelete("002");
  CheckNotFound("002");
  CheckNotFound("404");
  CheckGet("200", "OK");

  CheckPut("big", large_);
  CheckGet("big", large_);

  // Make sure this at least doesn't blow up.
  cache_->DumpStats();
}

void SharedMemCacheTestBase::TestReinsert() {
  CheckPut("key", "val");
  CheckGet("key", "val");

  // Insert the same size.
  CheckPut("key", "alv");
  CheckGet("key", "alv");

  // Insert larger one..
  CheckPut("key", large_);
  CheckGet("key", large_);

  // Now shrink it down again.
  CheckPut("key", "small");
  CheckGet("key", "small");

  // ... And make it huge.
  CheckPut("key", gigantic_);
  CheckGet("key", gigantic_);

  // Now try with empty value
  CheckPut("key", "");
  CheckGet("key", "");
}

void SharedMemCacheTestBase::TestReplacement() {
  sanity_checks_enabled_ = false;  // To expensive for that much work.

  // Make sure we can allocate spec from replacement, too, but that it doesn't
  // affect very recent files (barring collisions). All 3 entries below should
  // fit into one sector.
  // Now throw in tons of small files, to make sure we load the
  // directory heavily.
  for (int n = 0; n < kSectorEntries * 4; ++n) {
    GoogleString key1 = IntegerToString(n);
    CheckPut(key1, key1);
    timer_.AdvanceMs(1);
    CheckPut(key1, key1);
    CheckGet(key1, key1);
  }

  cache_->SanityCheck();

  for (int n = 0; n < 100; n += 3) {
    GoogleString key1 = IntegerToString(n);
    GoogleString key2 = IntegerToString(n + 1);
    GoogleString key3 = IntegerToString(n + 2);

    CheckPut(key1, large_);
    timer_.AdvanceMs(1);
    CheckPut(key2, key2);
    timer_.AdvanceMs(1);
    CheckPut(key3, gigantic_);
    timer_.AdvanceMs(1);

    CheckGet(key1, large_);
    CheckGet(key2, key2);
    CheckGet(key3, gigantic_);
    CheckDelete(key2.c_str());
    timer_.AdvanceMs(1);
    CheckNotFound(key2.c_str());
  }

  cache_->SanityCheck();
}

void SharedMemCacheTestBase::TestReaderWriter() {
  CreateChild(&SharedMemCacheTestBase::TestReaderWriterChild);

  for (int i = 0; i < kSpinRuns; ++i) {
    // Wait until the child puts in a proper value for 'key'
    CacheTestBase::Callback callback;
    while (callback.state() != CacheInterface::kAvailable) {
      cache_->Get("key", callback.Reset());
      ASSERT_EQ(true, callback.called());
      YieldToThread();
    }
    EXPECT_EQ(large_, callback.value().Value());
    CheckDelete("key");

    CheckPut("key2", "val2");
  }

  test_env_->WaitForChildren();
}

void SharedMemCacheTestBase::TestReaderWriterChild() {
  std::unique_ptr<SharedMemCache<kBlockSize>> child_cache(MakeCache());
  if (!child_cache->Attach()) {
    test_env_->ChildFailed();
  }
  SharedString val(large_);

  for (int i = 0; i < kSpinRuns; ++i) {
    child_cache->Put("key", val);

    // Now wait until the parent puts in what we expect for 'key2'
    CacheTestBase::Callback callback;
    while (callback.state() != CacheInterface::kAvailable) {
      child_cache->Get("key2", callback.Reset());
      ASSERT_EQ(true, callback.called());
      YieldToThread();
    }

    if (callback.value().Value() != "val2") {
      test_env_->ChildFailed();
    }
    child_cache->Delete("key2");
  }
}

void SharedMemCacheTestBase::TestConflict() {
  const int kAssociativity = SharedMemCache<kBlockSize>::kAssociativity;

  // We create a cache with 1 sector, and kAssociativity entries, since it
  // makes it easy to get a conflict and replacement.
  std::unique_ptr<SharedMemCache<kBlockSize>> small_cache(
      new SharedMemCache<kBlockSize>(
          shmem_runtime_.get(), kAltSegment, &timer_, &hasher_, 1 /* sectors*/,
          kAssociativity /* entries / sector */, kSectorBlocks, &handler_));
  ASSERT_TRUE(small_cache->Initialize());

  // Insert kAssociativity + 1 entries.
  for (int c = 0; c <= kAssociativity; ++c) {
    GoogleString key = IntegerToString(c);
    CheckPut(small_cache.get(), key, key);
  }

  // Now make sure the final one is available.
  // It would seem like one could predict replacement order exactly, but
  // with us only having kAssoc possible key values, it's quite likely
  // that the constructed key set will not have full associativity.
  GoogleString last(IntegerToString(kAssociativity));
  CheckGet(small_cache.get(), last, last);
  small_cache->GlobalCleanup(shmem_runtime_.get(), kAltSegment, &handler_);
}

void SharedMemCacheTestBase::TestEvict() {
  // We create a cache with 1 sector as it makes it easier to reason
  // about how much room is left.
  std::unique_ptr<SharedMemCache<kBlockSize>> small_cache(
      new SharedMemCache<kBlockSize>(
          shmem_runtime_.get(), kAltSegment, &timer_, &hasher_, 1 /* sectors*/,
          kSectorBlocks * 4 /* entries / sector */, kSectorBlocks, &handler_));
  ASSERT_TRUE(small_cache->Initialize());

  // Insert large_ kSectorBlocks times. Since large_ is ~3 blocks in size,
  // we will need to evict older entries eventually.
  for (int c = 0; c < kSectorBlocks; ++c) {
    GoogleString key = IntegerToString(c);
    CheckPut(small_cache.get(), key, large_);
    CheckGet(small_cache.get(), key, large_);
  }

  small_cache->GlobalCleanup(shmem_runtime_.get(), kAltSegment, &handler_);
}

void SharedMemCacheTestBase::TestAbandonedSectorLocks() {
  if (!test_env_->RecoversAbandonedMutexes()) {
    GTEST_SKIP() << "This runtime leaves an abandoned mutex locked.";
  }
  CheckPut("200", "OK");
  CheckGet("200", "OK");

  ASSERT_TRUE(CreateChild(&SharedMemCacheTestBase::AbandonSectorLocksChild));
  test_env_->WaitForChildren();

  // The child terminated holding every sector's lock, so none of these may
  // block. What the sectors hold can no longer be trusted, so from now on
  // they act as empty: the old entry is gone and new ones are dropped.
  CheckNotFound("200");
  CheckPut("200", "OK");
  CheckNotFound("200");
  CheckDelete("200");

  // Each sector reports being retired once, when its lock is first taken
  // over. SanityCheck() in CheckDelete() has taken every sector's lock by
  // now, and so does DumpStats().
  cache_->DumpStats();
  EXPECT_EQ(kSectors, handler_.MessagesOfType(kWarning));
}

void SharedMemCacheTestBase::TestAbandonedLockWhileWriterWaits() {
  if (!test_env_->RecoversAbandonedMutexes()) {
    GTEST_SKIP() << "This runtime leaves an abandoned mutex locked.";
  }
  CheckPut("200", "OK");

  // Pretend that a reader is inside every entry, so that the next Put of
  // "200" waits for it in EnsureReadyForWriting().
  std::unique_ptr<AbstractSharedMemSegment> segment;
  SectorVector sectors;
  ASSERT_TRUE(AttachSectors(&segment, &sectors));
  for (const auto& sector : sectors) {
    ScopedMutex lock(sector->mutex());
    for (int e = 0; e < kSectorEntries; ++e) {
      sector->EntryAt(e)->increment_open_count();
    }
  }

  // While the writer sleeps without the lock, a child takes every sector's
  // lock and terminates. The writer must stop waiting and drop the write.
  timer_.set_sleep_hook(
      MakeFunction(this, &SharedMemCacheTestBase::AbandonSectorLocksAndWait));
  CheckPut("200", "NEW");
  CheckNotFound("200");
}

void SharedMemCacheTestBase::TestAbandonedReader() {
  CheckPut("200", "OK");

  // A reader that terminated while copying a value out leaves the entry's
  // open count raised for good. Leave every entry in that state.
  std::unique_ptr<AbstractSharedMemSegment> segment;
  SectorVector sectors;
  ASSERT_TRUE(AttachSectors(&segment, &sectors));
  for (const auto& sector : sectors) {
    ScopedMutex lock(sector->mutex());
    for (int e = 0; e < kSectorEntries; ++e) {
      sector->EntryAt(e)->increment_open_count();
    }
  }

  // A write of "200" waits for that reader only for a while, then gives up
  // on the entry. From then on the entry is skipped without waiting.
  CheckPut("200", "NEW");
  CheckNotFound("200");
  CheckPut("200", "NEWER");
  CheckNotFound("200");
  CheckDelete("200");
  EXPECT_EQ(1, handler_.MessagesOfType(kWarning));

  // A read of the stuck entry misses and is not counted as a hit.
  auto get_hits = [&sectors]() {
    int64 hits = 0;
    for (const auto& sector : sectors) {
      ScopedMutex lock(sector->mutex());
      hits += sector->sector_stats()->num_get_hit;
    }
    return hits;
  };
  int64 hits = get_hits();
  CheckNotFound("200");
  EXPECT_EQ(hits, get_hits());

  // If the reader leaves after all, the entry stays out of use: it keeps its
  // block, and writes that need room evict everything around it but not it.
  for (const auto& sector : sectors) {
    ScopedMutex lock(sector->mutex());
    for (int e = 0; e < kSectorEntries; ++e) {
      sector->EntryAt(e)->decrement_open_count();
    }
  }
  // 2000 values of 3 blocks each need about 3000 blocks in each of the two
  // 2000-block sectors, and far more entries than a sector's 256, so the
  // writes have to evict around the stuck entry.
  for (int i = 0; i < 2000; ++i) {
    cache_->Put(StrCat("evict", IntegerToString(i)), SharedString(large_));
  }
  SanityCheck();
  CheckNotFound("200");
  int stuck = 0;
  for (const auto& sector : sectors) {
    ScopedMutex lock(sector->mutex());
    for (int e = 0; e < kSectorEntries; ++e) {
      const SharedMemCacheData::CacheEntry* entry = sector->EntryAt(e);
      if (entry->creating()) {
        ++stuck;
        EXPECT_NE(SharedMemCacheData::kInvalidBlock, entry->first_block);
      }
    }
  }
  EXPECT_EQ(1, stuck);
}

void SharedMemCacheTestBase::AbandonSectorLocksAndWait() {
  EXPECT_TRUE(CreateChild(&SharedMemCacheTestBase::AbandonSectorLocksChild));
  test_env_->WaitForChildren();
}

bool SharedMemCacheTestBase::AttachSectors(
    std::unique_ptr<AbstractSharedMemSegment>* segment, SectorVector* sectors) {
  using SharedMemCacheData::Sector;
  size_t sector_size = Sector<kBlockSize>::RequiredSize(
      shmem_runtime_.get(), kSectorEntries, kSectorBlocks);
  segment->reset(shmem_runtime_->AttachToSegment(
      kSegment, kSectors * sector_size, &handler_));
  if (*segment == nullptr) {
    return false;
  }
  for (int s = 0; s < kSectors; ++s) {
    sectors->push_back(std::make_unique<Sector<kBlockSize>>(
        segment->get(), s * sector_size, kSectorEntries, kSectorBlocks));
    if (!sectors->back()->Attach(&handler_)) {
      return false;
    }
  }
  return true;
}

void SharedMemCacheTestBase::AbandonSectorLocksChild()
    NO_THREAD_SAFETY_ANALYSIS {
  std::unique_ptr<AbstractSharedMemSegment> segment;
  SectorVector sectors;
  if (!AttachSectors(&segment, &sectors)) {
    test_env_->ChildFailed();
    return;
  }
  for (const auto& sector : sectors) {
    sector->mutex()->Lock();
    // Leave the sector as garbled as a holder killed half-way through an
    // update might: every block list loops and every entry points out of
    // range. A sector taken over this way must never be walked again.
    for (int b = 0; b < kSectorBlocks; ++b) {
      sector->SetBlockSuccessor(b, b);
    }
    for (int e = 0; e < kSectorEntries; ++e) {
      SharedMemCacheData::CacheEntry* entry = sector->EntryAt(e);
      entry->lru_prev = 0x7fffff00;
      entry->lru_next = 0x7fffff00;
      entry->first_block = 0x7fffff00;
      entry->byte_size = 0x7fffff00;
    }
  }
  // Return without unlocking anything.
}

void SharedMemCacheTestBase::CheckDelete(const char* key) {
  cache_->Delete(key);
  SanityCheck();
}

}  // namespace net_instaweb
