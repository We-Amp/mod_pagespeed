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

#ifndef PAGESPEED_KERNEL_BASE_SCOPED_MEMORY_POOL_H_
#define PAGESPEED_KERNEL_BASE_SCOPED_MEMORY_POOL_H_

#include <cstddef>
#include <cstring>
#include <memory>
#include <vector>

#include "pagespeed/kernel/base/basictypes.h"
#include "pagespeed/kernel/base/string_util.h"

namespace net_instaweb {

// Cross-platform memory pool that provides fast allocation with bulk
// deallocation. All memory allocated from the pool is freed when the pool
// is destroyed. This is similar to APR pools but uses standard C++.
//
// Use this for allocations that have the same lifetime as some scope or
// operation, where individual deallocation is not needed.
//
// Example usage:
//   ScopedMemoryPool pool;
//   char* str1 = pool.Strdup("hello");
//   char* str2 = pool.Strdup("world");
//   void* data = pool.Alloc(1024);
//   // All memory is freed when pool goes out of scope
class ScopedMemoryPool {
 public:
  // Create a pool with the specified initial block size.
  explicit ScopedMemoryPool(size_t initial_block_size = 8192);

  // Destroying the pool frees all allocated memory.
  ~ScopedMemoryPool();

  // Allocate 'size' bytes from the pool. The returned memory is not
  // initialized. Returns nullptr if size is 0.
  void* Alloc(size_t size);

  // Allocate 'size' bytes and zero-initialize them.
  void* Calloc(size_t size);

  // Duplicate a null-terminated C string.
  char* Strdup(const char* src);

  // Duplicate a StringPiece (copies the data, adds null terminator).
  char* Strdup(StringPiece src);

  // Returns the total number of bytes allocated from this pool.
  size_t bytes_allocated() const { return bytes_allocated_; }

 private:
  // Internal block structure for managing memory chunks.
  struct Block {
    std::unique_ptr<char[]> data;
    size_t size;
    size_t used;

    explicit Block(size_t block_size)
        : data(new char[block_size]), size(block_size), used(0) {}
  };

  // Allocate a new block if the current one doesn't have enough space.
  void AllocateNewBlock(size_t min_size);

  std::vector<Block> blocks_;
  size_t default_block_size_;
  size_t bytes_allocated_;

  DISALLOW_COPY_AND_ASSIGN(ScopedMemoryPool);
};

}  // namespace net_instaweb

#endif  // PAGESPEED_KERNEL_BASE_SCOPED_MEMORY_POOL_H_
