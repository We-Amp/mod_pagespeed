// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.
//
// This file is derived from mod_pagespeed (pagespeed/kernel/base/arena.h)
// and has been substantially modified. Originally licensed under Apache
// License, Version 2.0.
// Copyright (c) 2010-2017 Google Inc.
// Copyright (c) 2018 The Apache Software Foundation.

#ifndef PAGESPEED_LIB_BASE_ARENA_H_
#define PAGESPEED_LIB_BASE_ARENA_H_

#include <algorithm>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <new>
#include <utility>
#include <vector>

namespace net_instaweb {

// Arena/bump allocator that tracks every allocated object via an 8-byte
// link header so that DestroyObjects() can invoke (virtual) destructors
// before bulk-freeing the backing chunks.
//
// Ported from mod_pagespeed's pagespeed/kernel/base/arena.h. Objects are
// allocated from fixed-size chunks; each object is preceded by a link
// pointer forming a per-chunk singly-linked list:
//
//                  /-----------------|
//                  |                \|/
// -----------------|---------------|----|-----------|
// |   | object 1 | | | object 2    | NU | object 3  |
// | | |          |   |             | LL |           |
// |_|_|----------|---|-------------|----|-----------|
//   |              ^
//   \--------------/
//
// The link is needed because objects may have different sizes, and we
// need to find every object to call its destructor.
//
// API-compatible with the previous bump-only arena (AllocateBytes,
// Allocate, AllocateObject, AllocateArray, Clear, bytes_allocated,
// block_count) and adds DestroyObjects(). Clear() frees all chunks
// WITHOUT calling destructors (previous behavior); DestroyObjects()
// calls virtual ~T() on every object first.
//
// The template parameter T is the common base type of all allocated
// objects and must have a virtual destructor.
//
// Usage:
//   Arena<HtmlNode> arena;
//   HtmlNode* node = new (&arena) HtmlNode(args...);
//   // ... use nodes ...
//   arena.DestroyObjects();  // ~T() run on all nodes, chunks freed
//
template <typename T>
class Arena {
 public:
  // All allocations are aligned to this. We also reserve this much room
  // per object for the link header, as it keeps things simple.
  static const size_t kAlign = 8;

  // Default chunk size (64KB).
  static constexpr size_t kDefaultBlockSize = 64 * 1024;

  explicit Arena(size_t block_size = kDefaultBlockSize)
      : block_size_(block_size) {
    InitEmpty();
  }

  // The arena must be emptied (Clear() or DestroyObjects()) before it is
  // destroyed, so that no live object is silently abandoned. The chunks
  // themselves are freed by the chunks_ member either way.
  ~Arena() { assert(chunks_.empty()); }

  // Non-copyable, non-movable (to keep allocated pointers stable)
  Arena(const Arena&) = delete;
  Arena& operator=(const Arena&) = delete;
  Arena(Arena&&) = delete;
  Arena& operator=(Arena&&) = delete;

  // Allocate raw memory with given size and alignment. The destructor
  // tracking requires that all objects are kAlign-aligned, which is
  // sufficient for the node types this arena is used with.
  void* AllocateBytes(size_t size, size_t alignment = kAlign) {
    assert(alignment <= kAlign);
    static_cast<void>(alignment);
    size += kAlign;  // Need room to link the next object.
    size = ExpandToAlign(size);

    assert(sizeof(void*) <= kAlign);

    if (next_alloc_ == nullptr || next_alloc_ + size > chunk_end_) {
      AddChunk(size);
    }

    char* base = next_alloc_;

    // Update the links -- the previous object should point to our
    // chunk's base, our base should point to nullptr, and last_link_
    // should point to our base.
    char** our_last_link_field = reinterpret_cast<char**>(base);
    *last_link_ = base;
    *our_last_link_field = nullptr;
    last_link_ = our_last_link_field;

    next_alloc_ += size;
    bytes_allocated_ += size;

    char* out = base + kAlign;
    assert((reinterpret_cast<uintptr_t>(out) & (kAlign - 1)) == 0);
    return out;
  }

  // Simple Allocate(size) for placement new operator - used by HtmlNode
  void* Allocate(size_t size) { return AllocateBytes(size, kAlign); }

  // Allocate and construct an object of type U. Like AllocateBytes, this
  // only guarantees kAlign (8-byte) alignment: alignof(U) > kAlign trips
  // the debug assert in AllocateBytes and is silently assumed not to
  // happen in release builds.
  template <typename U, typename... Args>
  U* AllocateObject(Args&&... args) {
    void* memory = AllocateBytes(sizeof(U), alignof(U));
    return new (memory) U(std::forward<Args>(args)...);
  }

  // Allocate an array of objects (default constructed). Same kAlign
  // alignment guarantee/assumption as AllocateObject.
  template <typename U>
  U* AllocateArray(size_t count) {
    if (count == 0) return nullptr;
    if (count > SIZE_MAX / sizeof(U)) return nullptr;
    void* memory = AllocateBytes(sizeof(U) * count, alignof(U));
    U* array = static_cast<U*>(memory);
    for (size_t i = 0; i < count; ++i) {
      new (array + i) U();
    }
    return array;
  }

  // Clear all allocations (does NOT call destructors)
  void Clear() { ReleaseChunks(); }

  // Calls the (virtual) destructor of every object in the arena and
  // frees all chunks. You must call this explicitly to run destructors.
  // Destructors must not allocate from this arena during the walk: a new
  // object would extend (or add a chunk to) the list being traversed.
  void DestroyObjects() {
    for (const auto& chunk : chunks_) {
      // Walk through objects in this chunk.
      char* base = chunk.get();
      while (base != nullptr) {
        reinterpret_cast<T*>(base + kAlign)->~T();
        base = *reinterpret_cast<char**>(base);
      }
    }
    ReleaseChunks();
  }

  // Rounds block size up to 8; we always align to it, even on 32-bit.
  static size_t ExpandToAlign(size_t in) {
    return (in + kAlign - 1) & ~(kAlign - 1);
  }

  // Get total bytes allocated (including per-object link headers and
  // alignment padding)
  size_t bytes_allocated() const { return bytes_allocated_; }

  // Get number of chunks allocated
  size_t block_count() const { return chunks_.size(); }

 private:
  // Adds in a new chunk and initializes all the fields below to refer
  // to it. The chunk is sized to fit at least min_size bytes.
  void AddChunk(size_t min_size) {
    size_t chunk_size = std::max(block_size_, min_size);
    auto chunk = std::make_unique<char[]>(chunk_size);
    next_alloc_ = chunk.get();
    chunk_end_ = next_alloc_ + chunk_size;
    chunks_.push_back(std::move(chunk));
    last_link_ = &scratch_;
  }

  // Frees all chunks without calling destructors and resets to empty.
  void ReleaseChunks() {
    chunks_.clear();
    bytes_allocated_ = 0;
    InitEmpty();
  }

  // Sets up all the pointers below to denote us being empty.
  void InitEmpty() {
    // The way this is initialized ensures that the next call to allocate
    // will call AddChunk(). Doing it this way rather than calling
    // AddChunk() from here establishes the invariant that all the chunks
    // are non-empty, which helps in DestroyObjects().
    next_alloc_ = nullptr;
    last_link_ = nullptr;
    chunk_end_ = nullptr;
  }

  size_t block_size_;

  // First free byte of the current chunk
  char* next_alloc_;

  // The point where to link in a new object we allocate.
  // We need this so that the last object in a chunk has
  // a null 'next' link.
  char** last_link_;

  // First address after the last byte of the currently active chunk
  char* chunk_end_;

  // Scratch location in case we want a write to go nowhere.
  // Does not point anywhere, just bitbuckets writes
  char* scratch_;

  std::vector<std::unique_ptr<char[]>> chunks_;
  size_t bytes_allocated_ = 0;
};

}  // namespace net_instaweb

#endif  // PAGESPEED_LIB_BASE_ARENA_H_
