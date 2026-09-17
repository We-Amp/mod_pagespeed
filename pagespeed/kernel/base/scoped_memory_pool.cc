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

#include "pagespeed/kernel/base/scoped_memory_pool.h"

#include <algorithm>
#include <cstring>

#include "pagespeed/kernel/base/string_util.h"

namespace net_instaweb {

ScopedMemoryPool::ScopedMemoryPool(size_t initial_block_size)
    : default_block_size_(initial_block_size), bytes_allocated_(0) {
  // Pre-allocate the first block
  blocks_.emplace_back(default_block_size_);
}

ScopedMemoryPool::~ScopedMemoryPool() {
  // All memory is automatically freed when blocks_ is destroyed
}

void ScopedMemoryPool::AllocateNewBlock(size_t min_size) {
  // Allocate at least the default block size, or more if needed
  size_t block_size = std::max(default_block_size_, min_size);
  blocks_.emplace_back(block_size);
}

void* ScopedMemoryPool::Alloc(size_t size) {
  if (size == 0) {
    return nullptr;
  }

  // Check if the current block has enough space
  Block& current = blocks_.back();
  if (current.used + size > current.size) {
    // Need a new block
    AllocateNewBlock(size);
  }

  Block& block = blocks_.back();
  void* result = block.data.get() + block.used;
  block.used += size;
  bytes_allocated_ += size;
  return result;
}

void* ScopedMemoryPool::Calloc(size_t size) {
  void* result = Alloc(size);
  if (result != nullptr) {
    std::memset(result, 0, size);
  }
  return result;
}

char* ScopedMemoryPool::Strdup(const char* src) {
  if (src == nullptr) {
    return nullptr;
  }
  size_t len = std::strlen(src);
  char* result = static_cast<char*>(Alloc(len + 1));
  if (result != nullptr) {
    std::memcpy(result, src, len + 1);
  }
  return result;
}

char* ScopedMemoryPool::Strdup(StringPiece src) {
  if (src.data() == nullptr) {
    return nullptr;
  }
  size_t len = src.size();
  char* result = static_cast<char*>(Alloc(len + 1));
  if (result != nullptr) {
    std::memcpy(result, src.data(), len);
    result[len] = '\0';
  }
  return result;
}

}  // namespace net_instaweb
