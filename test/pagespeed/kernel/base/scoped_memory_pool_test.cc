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

// Unit tests for ScopedMemoryPool

#include "pagespeed/kernel/base/scoped_memory_pool.h"

#include <cstring>

#include "pagespeed/kernel/base/string_util.h"
#include "test/pagespeed/kernel/base/gtest.h"

namespace net_instaweb {

class ScopedMemoryPoolTest : public testing::Test {};

TEST_F(ScopedMemoryPoolTest, BasicAllocation) {
  ScopedMemoryPool pool;
  EXPECT_EQ(0u, pool.bytes_allocated());

  void* p1 = pool.Alloc(100);
  EXPECT_NE(nullptr, p1);
  EXPECT_EQ(100u, pool.bytes_allocated());

  void* p2 = pool.Alloc(200);
  EXPECT_NE(nullptr, p2);
  EXPECT_NE(p1, p2);
  EXPECT_EQ(300u, pool.bytes_allocated());
}

TEST_F(ScopedMemoryPoolTest, ZeroSizeAllocation) {
  ScopedMemoryPool pool;
  void* p = pool.Alloc(0);
  EXPECT_EQ(nullptr, p);
  EXPECT_EQ(0u, pool.bytes_allocated());
}

TEST_F(ScopedMemoryPoolTest, Calloc) {
  ScopedMemoryPool pool;
  unsigned char* p = static_cast<unsigned char*>(pool.Calloc(100));
  EXPECT_NE(nullptr, p);
  EXPECT_EQ(100u, pool.bytes_allocated());

  // Verify all bytes are zero
  for (int i = 0; i < 100; ++i) {
    EXPECT_EQ(0, p[i]) << "Byte at index " << i << " is not zero";
  }
}

TEST_F(ScopedMemoryPoolTest, CallocZeroSize) {
  ScopedMemoryPool pool;
  void* p = pool.Calloc(0);
  EXPECT_EQ(nullptr, p);
}

TEST_F(ScopedMemoryPoolTest, StrdupCString) {
  ScopedMemoryPool pool;
  const char* original = "hello world";
  char* copy = pool.Strdup(original);

  EXPECT_NE(nullptr, copy);
  EXPECT_NE(original, copy);  // Different pointers
  EXPECT_STREQ(original, copy);
  EXPECT_EQ(strlen(original) + 1, pool.bytes_allocated());
}

TEST_F(ScopedMemoryPoolTest, StrdupNullCString) {
  ScopedMemoryPool pool;
  const char* null_str = nullptr;
  char* copy = pool.Strdup(null_str);
  EXPECT_EQ(nullptr, copy);
  EXPECT_EQ(0u, pool.bytes_allocated());
}

TEST_F(ScopedMemoryPoolTest, StrdupEmptyCString) {
  ScopedMemoryPool pool;
  const char* empty = "";
  char* copy = pool.Strdup(empty);

  EXPECT_NE(nullptr, copy);
  EXPECT_STREQ("", copy);
  EXPECT_EQ(1u, pool.bytes_allocated());  // Just the null terminator
}

TEST_F(ScopedMemoryPoolTest, StrdupStringPiece) {
  ScopedMemoryPool pool;
  StringPiece original("hello world");
  char* copy = pool.Strdup(original);

  EXPECT_NE(nullptr, copy);
  EXPECT_STREQ("hello world", copy);
  EXPECT_EQ(original.size() + 1, pool.bytes_allocated());
}

TEST_F(ScopedMemoryPoolTest, StrdupStringPieceWithEmbeddedNull) {
  ScopedMemoryPool pool;
  // Create a StringPiece with embedded null
  const char data[] = "hel\0lo";
  StringPiece original(data, 6);  // "hel\0lo"
  char* copy = pool.Strdup(original);

  EXPECT_NE(nullptr, copy);
  EXPECT_EQ(7u, pool.bytes_allocated());  // 6 chars + null terminator
  EXPECT_EQ(0, memcmp(copy, data, 6));    // First 6 bytes match
  EXPECT_EQ('\0', copy[6]);               // Null terminator added
}

TEST_F(ScopedMemoryPoolTest, StrdupNullDataStringPiece) {
  ScopedMemoryPool pool;
  StringPiece null_piece(nullptr, 0);
  char* copy = pool.Strdup(null_piece);
  EXPECT_EQ(nullptr, copy);
  EXPECT_EQ(0u, pool.bytes_allocated());
}

TEST_F(ScopedMemoryPoolTest, LargeAllocation) {
  ScopedMemoryPool pool(1024);  // Small initial block
  // Allocate more than the block size
  void* p = pool.Alloc(4096);
  EXPECT_NE(nullptr, p);
  EXPECT_EQ(4096u, pool.bytes_allocated());
}

TEST_F(ScopedMemoryPoolTest, ManySmallAllocations) {
  ScopedMemoryPool pool(8192);
  const int kNumAllocations = 1000;
  const int kAllocationSize = 16;

  for (int i = 0; i < kNumAllocations; ++i) {
    void* p = pool.Alloc(kAllocationSize);
    EXPECT_NE(nullptr, p);
  }

  EXPECT_EQ(static_cast<size_t>(kNumAllocations * kAllocationSize),
            pool.bytes_allocated());
}

TEST_F(ScopedMemoryPoolTest, BlockGrowth) {
  ScopedMemoryPool pool(64);  // Very small initial block

  // Allocate multiple chunks to force multiple blocks
  void* p1 = pool.Alloc(32);
  void* p2 = pool.Alloc(32);
  void* p3 = pool.Alloc(32);
  void* p4 = pool.Alloc(32);

  EXPECT_NE(nullptr, p1);
  EXPECT_NE(nullptr, p2);
  EXPECT_NE(nullptr, p3);
  EXPECT_NE(nullptr, p4);

  // All pointers should be different
  EXPECT_NE(p1, p2);
  EXPECT_NE(p2, p3);
  EXPECT_NE(p3, p4);

  EXPECT_EQ(128u, pool.bytes_allocated());
}

TEST_F(ScopedMemoryPoolTest, MultipleStrings) {
  ScopedMemoryPool pool;
  const char* strings[] = {"one", "two", "three", "four", "five"};
  char* copies[5];

  size_t total_size = 0;
  for (int i = 0; i < 5; ++i) {
    copies[i] = pool.Strdup(strings[i]);
    total_size += strlen(strings[i]) + 1;
  }

  // Verify all copies are correct
  for (int i = 0; i < 5; ++i) {
    EXPECT_STREQ(strings[i], copies[i]);
  }

  EXPECT_EQ(total_size, pool.bytes_allocated());
}

TEST_F(ScopedMemoryPoolTest, MixedAllocations) {
  ScopedMemoryPool pool;

  char* s1 = pool.Strdup("hello");
  void* p1 = pool.Alloc(100);
  char* s2 = pool.Strdup("world");
  void* p2 = pool.Calloc(50);

  EXPECT_NE(nullptr, s1);
  EXPECT_NE(nullptr, p1);
  EXPECT_NE(nullptr, s2);
  EXPECT_NE(nullptr, p2);

  EXPECT_STREQ("hello", s1);
  EXPECT_STREQ("world", s2);

  // 6 + 100 + 6 + 50 = 162
  EXPECT_EQ(162u, pool.bytes_allocated());
}

TEST_F(ScopedMemoryPoolTest, CustomBlockSize) {
  ScopedMemoryPool pool(256);

  // Allocate less than the block size
  void* p1 = pool.Alloc(100);
  void* p2 = pool.Alloc(100);

  EXPECT_NE(nullptr, p1);
  EXPECT_NE(nullptr, p2);
  EXPECT_EQ(200u, pool.bytes_allocated());
}

TEST_F(ScopedMemoryPoolTest, AllocatedMemoryIsWritable) {
  ScopedMemoryPool pool;
  char* data = static_cast<char*>(pool.Alloc(100));
  ASSERT_NE(nullptr, data);

  // Write to the memory
  for (int i = 0; i < 100; ++i) {
    data[i] = static_cast<char>(i);
  }

  // Read it back and verify
  for (int i = 0; i < 100; ++i) {
    EXPECT_EQ(static_cast<char>(i), data[i]);
  }
}

}  // namespace net_instaweb
