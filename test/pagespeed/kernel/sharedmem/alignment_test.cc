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

// Unit tests for alignment utilities and shared memory structure layout.

#include "pagespeed/kernel/base/alignment_util.h"

#include "gtest/gtest.h"
#include "pagespeed/kernel/sharedmem/shared_mem_cache_data.h"
#ifdef _WIN32
#include "pagespeed/kernel/sharedmem/windows_shared_mem.h"
#endif

namespace net_instaweb {
namespace {

using SharedMemCacheData::CacheEntry;
using SharedMemCacheData::SectorHeader;
using SharedMemCacheData::SectorStats;

TEST(AlignmentUtilTest, MutexAlignment) {
#ifdef _WIN32
  EXPECT_EQ(16u, MutexAlignment());
#else
  EXPECT_EQ(8u, MutexAlignment());
#endif
}

TEST(AlignmentUtilTest, SharedMemMaxAlignment) {
  EXPECT_EQ(MutexAlignment(), SharedMemMaxAlignment());
}

TEST(AlignmentUtilTest, AlignOffset_Zero) {
  EXPECT_EQ(0u, AlignOffset(0, 8));
  EXPECT_EQ(0u, AlignOffset(0, 16));
}

TEST(AlignmentUtilTest, AlignOffset_AlreadyAligned) {
  EXPECT_EQ(8u, AlignOffset(8, 8));
  EXPECT_EQ(16u, AlignOffset(16, 8));
  EXPECT_EQ(16u, AlignOffset(16, 16));
  EXPECT_EQ(32u, AlignOffset(32, 16));
}

TEST(AlignmentUtilTest, AlignOffset_NeedsAlignment) {
  EXPECT_EQ(8u, AlignOffset(1, 8));
  EXPECT_EQ(8u, AlignOffset(7, 8));
  EXPECT_EQ(16u, AlignOffset(9, 8));
  EXPECT_EQ(16u, AlignOffset(15, 8));

  EXPECT_EQ(16u, AlignOffset(1, 16));
  EXPECT_EQ(16u, AlignOffset(15, 16));
  EXPECT_EQ(32u, AlignOffset(17, 16));
  EXPECT_EQ(32u, AlignOffset(31, 16));
}

TEST(AlignmentUtilTest, AlignTo8) {
  EXPECT_EQ(0u, AlignTo8(0));
  EXPECT_EQ(8u, AlignTo8(1));
  EXPECT_EQ(8u, AlignTo8(8));
  EXPECT_EQ(16u, AlignTo8(9));
}

TEST(AlignmentUtilTest, AlignForMutex) {
  size_t alignment = MutexAlignment();
  EXPECT_EQ(0u, AlignForMutex(0) % alignment);
  EXPECT_EQ(0u, AlignForMutex(1) % alignment);
  EXPECT_EQ(0u, AlignForMutex(alignment - 1) % alignment);
  EXPECT_EQ(0u, AlignForMutex(alignment) % alignment);
  EXPECT_EQ(0u, AlignForMutex(alignment + 1) % alignment);
}

TEST(AlignmentUtilTest, IsMutexAligned) {
  // Test with stack-allocated aligned buffer.
  alignas(16) char buffer[64];

  EXPECT_TRUE(IsMutexAligned(buffer));
  EXPECT_TRUE(IsMutexAligned(buffer + 16));
  EXPECT_TRUE(IsMutexAligned(buffer + 32));

#ifdef _WIN32
  // On Windows, 8-byte aligned but not 16-byte aligned should fail.
  if (reinterpret_cast<uintptr_t>(buffer + 8) % 16 != 0) {
    EXPECT_FALSE(IsMutexAligned(buffer + 8));
  }
#endif
}

TEST(AlignmentUtilTest, IsAligned) {
  alignas(16) char buffer[64];

  EXPECT_TRUE(IsAligned(buffer, 1));
  EXPECT_TRUE(IsAligned(buffer, 2));
  EXPECT_TRUE(IsAligned(buffer, 4));
  EXPECT_TRUE(IsAligned(buffer, 8));
  EXPECT_TRUE(IsAligned(buffer, 16));

  EXPECT_TRUE(IsAligned(buffer + 8, 8));
  EXPECT_FALSE(IsAligned(buffer + 1, 8));
}

// Tests for shared memory structure layout.

TEST(SharedMemStructuresTest, SectorStatsSize) {
  // SectorStats has 11 int64 fields = 88 bytes minimum.
  // With 8-byte packing, should be exactly 88 bytes.
  EXPECT_GE(sizeof(SectorStats), 80u);
  EXPECT_LE(sizeof(SectorStats), 96u);
  EXPECT_EQ(0u, sizeof(SectorStats) % 8);  // Must be 8-byte aligned.
}

TEST(SharedMemStructuresTest, SectorHeaderSize) {
  // SectorHeader has:
  // - 3x int32 (12 bytes) + 1x int32 padding (4 bytes) = 16 bytes
  // - SectorStats (88 bytes)
  // Total: 104 bytes
  EXPECT_GE(sizeof(SectorHeader), 96u);
  EXPECT_LE(sizeof(SectorHeader), 112u);
  EXPECT_EQ(0u, sizeof(SectorHeader) % 8);  // Must be 8-byte aligned.
}

TEST(SharedMemStructuresTest, CacheEntrySize) {
  // CacheEntry has:
  // - hash_bytes[16] (16 bytes)
  // - int64 last_use_timestamp_ms (8 bytes)
  // - int32 byte_size (4 bytes)
  // - 2x int32 lru_prev/next (8 bytes)
  // - int32 first_block (4 bytes)
  // - uint32 flags (4 bytes)
  // - uint32 padding (4 bytes)
  // Total: 48 bytes
  EXPECT_GE(sizeof(CacheEntry), 48u);
  EXPECT_LE(sizeof(CacheEntry), 64u);
  EXPECT_EQ(0u, sizeof(CacheEntry) % 8);  // Must be 8-byte aligned.
}

TEST(SharedMemStructuresTest, SectorHeaderMutexOffset) {
  // The mutex should be placed at an aligned offset after SectorHeader.
  size_t header_size = sizeof(SectorHeader);
  size_t mutex_offset = AlignForMutex(header_size);

  // The offset must be properly aligned.
#ifdef _WIN32
  EXPECT_EQ(0u, mutex_offset % 16);
#else
  EXPECT_EQ(0u, mutex_offset % 8);
#endif

  // The offset should be >= header size.
  EXPECT_GE(mutex_offset, header_size);
}

TEST(SharedMemStructuresTest, CacheEntryAlignment) {
  // CacheEntry alignment should not exceed 8 bytes.
  EXPECT_LE(alignof(CacheEntry), 8u);
}

// Test CacheEntry flag accessors.

TEST(CacheEntryTest, FlagsInitiallyZero) {
  CacheEntry entry;
  entry.flags = 0;
  EXPECT_FALSE(entry.creating());
  EXPECT_EQ(0u, entry.open_count());
}

TEST(CacheEntryTest, SetCreating) {
  CacheEntry entry;
  entry.flags = 0;

  entry.set_creating(true);
  EXPECT_TRUE(entry.creating());
  EXPECT_EQ(0u, entry.open_count());

  entry.set_creating(false);
  EXPECT_FALSE(entry.creating());
  EXPECT_EQ(0u, entry.open_count());
}

TEST(CacheEntryTest, SetOpenCount) {
  CacheEntry entry;
  entry.flags = 0;

  entry.set_open_count(1);
  EXPECT_EQ(1u, entry.open_count());
  EXPECT_FALSE(entry.creating());

  entry.set_open_count(100);
  EXPECT_EQ(100u, entry.open_count());
  EXPECT_FALSE(entry.creating());

  entry.set_open_count(0x7FFFFFFFu);  // Max value (31 bits).
  EXPECT_EQ(0x7FFFFFFFu, entry.open_count());
  EXPECT_FALSE(entry.creating());
}

TEST(CacheEntryTest, IncrementDecrementOpenCount) {
  CacheEntry entry;
  entry.flags = 0;

  entry.increment_open_count();
  EXPECT_EQ(1u, entry.open_count());

  entry.increment_open_count();
  EXPECT_EQ(2u, entry.open_count());

  entry.decrement_open_count();
  EXPECT_EQ(1u, entry.open_count());

  entry.decrement_open_count();
  EXPECT_EQ(0u, entry.open_count());
}

TEST(CacheEntryTest, FlagsIndependent) {
  CacheEntry entry;
  entry.flags = 0;

  // Set creating, then open_count - they should be independent.
  entry.set_creating(true);
  entry.set_open_count(42);
  EXPECT_TRUE(entry.creating());
  EXPECT_EQ(42u, entry.open_count());

  // Modify open_count, creating should remain.
  entry.set_open_count(100);
  EXPECT_TRUE(entry.creating());
  EXPECT_EQ(100u, entry.open_count());

  // Clear creating, open_count should remain.
  entry.set_creating(false);
  EXPECT_FALSE(entry.creating());
  EXPECT_EQ(100u, entry.open_count());

  // Increment/decrement with creating set.
  entry.set_creating(true);
  entry.increment_open_count();
  EXPECT_TRUE(entry.creating());
  EXPECT_EQ(101u, entry.open_count());

  entry.decrement_open_count();
  EXPECT_TRUE(entry.creating());
  EXPECT_EQ(100u, entry.open_count());
}

#ifdef _WIN32
// Windows-specific tests for WindowsMutexSlot structure.

TEST(WindowsMutexSlotTest, Size) {
  // WindowsMutexSlot must be 16 bytes for proper alignment.
  EXPECT_EQ(16u, sizeof(WindowsMutexSlot));
}

TEST(WindowsMutexSlotTest, Alignment) {
  // WindowsMutexSlot alignment should not exceed 8 bytes.
  EXPECT_LE(alignof(WindowsMutexSlot), 8u);
}

TEST(WindowsMutexSlotTest, FieldLayout) {
  WindowsMutexSlot slot;
  slot.magic = 0xDEADBEEF;
  slot.mutex_id = 42;
  slot.padding = 0;

  EXPECT_EQ(0xDEADBEEF, slot.magic);
  EXPECT_EQ(42u, slot.mutex_id);
  EXPECT_EQ(0u, slot.padding);
}
#endif  // _WIN32

}  // namespace
}  // namespace net_instaweb
