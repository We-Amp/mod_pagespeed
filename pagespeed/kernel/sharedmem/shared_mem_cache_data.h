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

// Data structure operation helpers for SharedMemCache. See the top of
// shared_mem_cache.cc for data format descriptions.
//
// ALIGNMENT NOTES:
// These structures are placed in shared memory and must have consistent
// layout across different compilers and platforms. We use #pragma pack
// to ensure consistent packing, and avoid bitfields which have
// compiler-specific layout.
//
// Mutexes are placed at aligned offsets AFTER these structures (not inline)
// to satisfy platform-specific alignment requirements:
//   - Linux/macOS: pthread_mutex_t requires 8-byte alignment
//   - Windows x64: CRITICAL_SECTION requires 16-byte alignment

#ifndef PAGESPEED_KERNEL_SHAREDMEM_SHARED_MEM_CACHE_DATA_H_
#define PAGESPEED_KERNEL_SHAREDMEM_SHARED_MEM_CACHE_DATA_H_

#include <cstddef>  // for size_t
#include <memory>
#include <vector>

#include "base/logging.h"
#include "pagespeed/kernel/base/basictypes.h"
#include "pagespeed/kernel/base/string.h"
#include "pagespeed/kernel/base/thread_annotations.h"

namespace net_instaweb {

class AbstractMutex;
class AbstractSharedMem;
class AbstractSharedMemSegment;
class MessageHandler;

namespace SharedMemCacheData {

using EntryNum = int32;
using BlockNum = int32;
using BlockVector = std::vector<BlockNum>;

const BlockNum kInvalidBlock = -1;
const EntryNum kInvalidEntry = -1;
const size_t kHashSize = 16;

// Ensure consistent structure layout across compilers (MSVC, GCC, Clang).
// We use 8-byte packing which matches the natural alignment of int64 members.
#if defined(_MSC_VER)
#pragma pack(push, 8)
#elif defined(__GNUC__) || defined(__clang__)
#pragma pack(push, 8)
#endif

struct SectorStats {
  SectorStats();

  // FS operation stats --- updated by SharedMemCache. We do it
  // this way rather than using the normal Statistics interface
  // to avoid having to worry about extra synchronization inside
  // critical sections --- since we already hold sector locks
  // when doing this stuff, it's easy to update per-sector data.
  // TODO(morlovich): Consider periodically pushing these to
  // normal Statistics.
  int64 num_put;
  int64 num_put_update;   // update of the same key
  int64 num_put_replace;  // replacement of different key
  int64 num_put_concurrent_create;
  int64 num_put_concurrent_full_set;
  int64 num_put_spins;  // # of times writers had to sleep behind readers
  int64 num_get;        // # of calls to get
  int64 num_get_hit;
  int64 last_checkpoint_ms;  // When this sector was last checkpointed to disk.

  // Current state stats --- updated by SharedMemCacheData
  int64 used_entries;
  int64 used_blocks;

  // Adds number to this object's. No concurrency control is done.
  void Add(const SectorStats& other);

  // Text dump of the statistics. No concurrency control is done.
  GoogleString Dump(size_t total_entries, size_t total_blocks) const;
};

struct SectorHeader {
  BlockNum free_list_front;
  EntryNum lru_list_front;
  EntryNum lru_list_rear;
  int32 padding;

  SectorStats stats;

  // Note: The mutex is stored at an aligned offset AFTER this structure,
  // not inline. The offset is computed using AlignForMutex() to satisfy
  // platform-specific alignment requirements (16-byte on Windows x64).
};

struct CacheEntry {
  char hash_bytes[kHashSize];
  int64 last_use_timestamp_ms;
  int32 byte_size;

  // For LRU list, prev/next are kInvalidEntry to denote 'none', which
  // can apply both at endpoints and for entries not in the LRU at all,
  // due to being free.
  EntryNum lru_prev;
  EntryNum lru_next;

  BlockNum first_block;

  // Flags field replaces bitfields for cross-platform consistency.
  // Bitfield layout is compiler-specific, but this uint32 with accessor
  // methods works identically on all platforms.
  //
  // Bit 0: creating - When true, someone is trying to overwrite this entry.
  // Bits 1-31: open_count - Number of readers currently accessing the data.
  uint32 flags;

  uint32 padding;  // ensures we're 8-aligned.

  // Accessor methods for the flags field.
  bool creating() const { return (flags & 1u) != 0; }
  void set_creating(bool value) {
    if (value) {
      flags |= 1u;
    } else {
      flags &= ~1u;
    }
  }

  uint32 open_count() const { return flags >> 1; }
  void set_open_count(uint32 count) { flags = (flags & 1u) | (count << 1); }

  void increment_open_count() {
    // Increment by 2 since open_count is stored in bits 1-31
    flags += 2u;
  }

  void decrement_open_count() {
    // Decrement by 2 since open_count is stored in bits 1-31
    flags -= 2u;
  }
};

#if defined(_MSC_VER) || defined(__GNUC__) || defined(__clang__)
#pragma pack(pop)
#endif

// Compile-time verification of structure sizes.
// We use range checks rather than exact values since padding may vary slightly,
// but the structures must remain within expected bounds for correct operation.
static_assert(sizeof(SectorStats) >= 80 && sizeof(SectorStats) <= 96,
              "SectorStats size outside expected range");
static_assert(sizeof(SectorHeader) >= 96 && sizeof(SectorHeader) <= 112,
              "SectorHeader size outside expected range");
static_assert(sizeof(CacheEntry) >= 48 && sizeof(CacheEntry) <= 64,
              "CacheEntry size outside expected range");

// Verify alignment requirements
static_assert(alignof(SectorStats) <= 8,
              "SectorStats alignment exceeds 8 bytes");
static_assert(alignof(SectorHeader) <= 8,
              "SectorHeader alignment exceeds 8 bytes");
static_assert(alignof(CacheEntry) <= 8, "CacheEntry alignment exceeds 8 bytes");

// Helper for operating on a given sector's data structures; helping
// access them, lay them out in memory, and initialize them. It does not
// implement the actual cache operations, however. In particular, its
// methods affect only a single data structure at the time and do not
// do anything to preserve cross-structure invariants.
template <size_t kBlockSize>
class Sector {
 public:
  // Creates a wrapper to help operate on cache sectors in a given region of
  // memory with given geometry.  The sector should have had as much memory
  // allocated for it as returned by a call to RequiredSize with the same
  // arguments.
  //
  // Note that this doesn't do any imperative initialization; you must
  // call Initialize() in the parent process, and Attach() in child processes,
  // and check their results as well. Also, segment is assumed to be owned
  // separately, with lifetime longer than ours.
  Sector(AbstractSharedMemSegment* segment, size_t sector_offset,
         size_t cache_entries, size_t data_blocks);
  ~Sector();

  // This should be called from child processes to initialize client
  // state for the cache already formatted by a call to Initialize() in
  // the parent.
  //
  // Returns if successful (which it should be if the parent process
  // successfully create the memory and Initialize()'d it).
  bool Attach(MessageHandler* handler);

  // This should be called from the initial/parent process before the children
  // start. It initializes the data structures in this sector, including
  // mutexes. Returns true on success.
  bool Initialize(MessageHandler* handler);

  // Computes how much memory a sector will need for given number of entries.
  // Also makes sure it's padded to proper alignment.
  static size_t RequiredSize(AbstractSharedMem* shmem_runtime,
                             size_t cache_entries, size_t data_blocks);

  // Mutex ops.

  // The sector lock should be held while doing any metadata accesses.
  AbstractMutex* mutex() const LOCK_RETURNED(mutex_) { return mutex_.get(); }

  // Block successor list ops.
  // ------------------------------------------------------------
  BlockNum GetBlockSuccessor(BlockNum block) EXCLUSIVE_LOCKS_REQUIRED(mutex()) {
    DCHECK_GE(block, 0);
    DCHECK_LT(block, static_cast<BlockNum>(data_blocks_));
    return block_successors_[block];
  }

  void SetBlockSuccessor(BlockNum block, BlockNum next)
      EXCLUSIVE_LOCKS_REQUIRED(mutex()) {
    DCHECK_GE(block, 0);
    DCHECK_LT(block, static_cast<BlockNum>(data_blocks_));

    DCHECK_GE(next, kInvalidBlock);
    DCHECK_LT(next, static_cast<BlockNum>(data_blocks_));

    block_successors_[block] = next;
  }

  // Links blocks in the vector in order, with later blocks being
  // marked as successors of later ones.
  void LinkBlockSuccessors(const BlockVector& blocks)
      EXCLUSIVE_LOCKS_REQUIRED(mutex()) {
    for (size_t pos = 0; pos < blocks.size(); ++pos) {
      if (pos == (blocks.size() - 1)) {
        SetBlockSuccessor(blocks[pos], kInvalidBlock);
      } else {
        SetBlockSuccessor(blocks[pos], blocks[pos + 1]);
      }
    }
  }

  // Freelist ops.
  // ------------------------------------------------------------

  // Allocates as close to the goal blocks from freelist as it can, and
  // appends their numbers to blocks. Returns how much it allocated.
  // Does not adjust block successor lists.
  //
  // Note that this doesn't attempt to free blocks that are in use by some
  // entries.
  int AllocBlocksFromFreeList(int goal, BlockVector* blocks)
      EXCLUSIVE_LOCKS_REQUIRED(mutex());

  // Puts all the passed in blocks onto this sector's freelist.
  // Does not read successors for passed in blocks, but does set them
  // for freelist membership.
  void ReturnBlocksToFreeList(const BlockVector& blocks)
      EXCLUSIVE_LOCKS_REQUIRED(mutex());

  // Cache directory ops.
  // ------------------------------------------------------------

  // Returns the given # entry.
  CacheEntry* EntryAt(EntryNum slot) {
    return reinterpret_cast<CacheEntry*>(directory_base_) + slot;
  }

  // Inserts the given entry into the LRU, at front.
  // Precondition: must not be in LRU.
  void InsertEntryIntoLRU(EntryNum entry_num);

  // Removes from the LRU. Safe to call if not in the LRU already.
  void UnlinkEntryFromLRU(EntryNum entry_num);

  EntryNum OldestEntryNum() { return sector_header_->lru_list_rear; }

  // Block ops.
  // ------------------------------------------------------------

  char* BlockBytes(BlockNum block_num) {
    return blocks_base_ + kBlockSize * block_num;
  }

  // Ops for lists of blocks corresponding to each directory entry,
  // and related size computations
  // ------------------------------------------------------------

  // Number of blocks of data needed for size blocks.
  static size_t DataBlocksForSize(size_t size) {
    return NeededPieces(size, kBlockSize);
  }

  // The # of bytes stored in block 'b' out of 'total' blocks for file of size
  // 'total_bytes'.
  // Precondition: 'total' is appropriate for 'total_bytes'.
  static size_t BytesInPortion(size_t total_bytes, size_t b, size_t total);

  // Appends the list of blocks used by the entry to 'blocks'.
  // Returns the number of items appended.
  int BlockListForEntry(CacheEntry* entry, BlockVector* out_blocks)
      EXCLUSIVE_LOCKS_REQUIRED(mutex());

  // Statistics stuff
  // ------------------------------------------------------------

  SectorStats* sector_stats() { return &sector_header_->stats; }

  // Prints out all statistics in the header (some of which are maintained
  // by the higher-level)
  void DumpStats(MessageHandler* handler);

 private:
  // Helper for doing sizing/memory layout computations.
  struct MemLayout;

  // How many piece_size pieces suffice to fit total
  static size_t NeededPieces(size_t total, size_t piece_size) {
    return (total + piece_size - 1) / piece_size;
  }

  // Configured geometry
  size_t cache_entries_;
  size_t data_blocks_;

  // Pointers to where various things are, and our sizes
  AbstractSharedMemSegment* segment_;
  std::unique_ptr<AbstractMutex> mutex_;
  SectorHeader* sector_header_;
  BlockNum* block_successors_ PT_GUARDED_BY(mutex());
  char* directory_base_;
  char* blocks_base_;
  size_t sector_offset_;  // offset of the sector within the SHM segment

  Sector(const Sector&) = delete;
  Sector& operator=(const Sector&) = delete;
};

}  // namespace SharedMemCacheData

}  // namespace net_instaweb

#endif  // PAGESPEED_KERNEL_SHAREDMEM_SHARED_MEM_CACHE_DATA_H_
