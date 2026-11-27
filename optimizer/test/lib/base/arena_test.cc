// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.
//
// Ported from mod_pagespeed's test/pagespeed/kernel/base/arena_test.cc
// (Apache License 2.0) and adapted to PageSpeed 2.0 conventions.

// Unit tests for the destructor-tracking arena.

#include "lib/base/arena.h"

#include <cstddef>
#include <memory>
#include <set>

#include "gtest/gtest.h"

namespace net_instaweb {

class ArenaTest : public testing::Test {
 public:
  ArenaTest() { ClearStats(); }

 protected:
  friend class KidA;
  friend class KidB;
  friend class KidBig;

  class Base {
   public:
    explicit Base(ArenaTest* owner) : owner_(owner) {}
    virtual ~Base() {
      // Watch out for double-delete
      EXPECT_TRUE(owner_ != nullptr);
      owner_ = nullptr;
    }

    // When testing creation, we invoke this method.
    virtual void Made() = 0;

    void* operator new(size_t size, Arena<Base>* arena) {
      return arena->Allocate(size);
    }

   protected:
    ArenaTest* owner_;
  };

  // We expect KidA to have size of 2 pointers: the vtable and owner,
  // (just like base). On 32-bit this will be 8-bytes, so with 8-byte
  // alignment area it will divide the block size
  class KidA : public Base {
   public:
    explicit KidA(ArenaTest* o) : Base(o) {}

    ~KidA() override { ++owner_->destroyed_a_; }

    void Made() override { ++owner_->made_a_; }
  };

  // KidB is 3 pointers long, so on 64-bit along with the next pointer
  // we will be using 32 bytes per allocation and it will divide
  // the block size.
  //
  // The difference in size between A and B lets us test mixed combinations of
  // different sizes.
  class KidB : public Base {
   public:
    explicit KidB(ArenaTest* o) : Base(o) {}

    ~KidB() override { ++owner_->destroyed_b_; }

    void Made() override { ++owner_->made_b_; }
  };

  // KidBig exceeds the default 64 KB chunk size, so allocating one forces
  // AddChunk() to create a dedicated oversized chunk for it.
  class KidBig : public Base {
   public:
    explicit KidBig(ArenaTest* o) : Base(o) {}

    ~KidBig() override { ++owner_->destroyed_big_; }

    void Made() override {
      payload_[0] = 42;  // touch it so the buffer is not optimized away
      ++owner_->made_big_;
    }

   private:
    char payload_[100 * 1024];
  };

  // HtmlNode-like: owns a heap payload via unique_ptr that can be
  // released explicitly before the arena runs destructors (the
  // FreeData() pattern). The destructor must then be a safe no-op
  // with respect to the payload.
  class KidPayload : public Base {
   public:
    explicit KidPayload(ArenaTest* o) : Base(o), payload_(new int(42)) {}

    ~KidPayload() override {
      ++owner_->destroyed_payload_;
      // unique_ptr destructor frees payload_ if still held; a payload
      // already released via FreePayload() must not be touched again.
    }

    void Made() override { ++owner_->made_payload_; }

    void FreePayload() { payload_.reset(); }

    bool HasPayload() const { return payload_ != nullptr; }

   private:
    std::unique_ptr<int> payload_;
  };

  // Tests a given mixture of allocations of KidA and KidB --
  // making sure we get sane pointers and delete things.
  void TestCombo(int num_a, int num_b) {
    for (int a = 0; a < num_a; ++a) {
      CheckPtr(new (&arena_) KidA(this));
    }

    for (int b = 0; b < num_b; ++b) {
      CheckPtr(new (&arena_) KidB(this));
    }

    arena_.DestroyObjects();

    EXPECT_EQ(num_a, made_a_);
    EXPECT_EQ(num_b, made_b_);
    EXPECT_EQ(num_a, destroyed_a_);
    EXPECT_EQ(num_b, destroyed_b_);
  }

  // Checks to make sure the pointer is sane, and calls Made on it.
  void CheckPtr(Base* p) {
    EXPECT_TRUE(seen_ptrs_.find(p) == seen_ptrs_.end());
    seen_ptrs_.insert(p);
    p->Made();
  }

  void ClearStats() {
    made_a_ = 0;
    made_b_ = 0;
    made_big_ = 0;
    made_payload_ = 0;
    destroyed_a_ = 0;
    destroyed_b_ = 0;
    destroyed_big_ = 0;
    destroyed_payload_ = 0;
    seen_ptrs_.clear();
  }

  int made_a_;
  int made_b_;
  int made_big_;
  int made_payload_;
  int destroyed_a_;
  int destroyed_b_;
  int destroyed_big_;
  int destroyed_payload_;
  Arena<Base> arena_;
  std::set<void*> seen_ptrs_;
};

// Empty arena should be OK without a Destroy
TEST_F(ArenaTest, TestEmpty) {}

// calling Destroy on empty is fine.
TEST_F(ArenaTest, TestEmptyDestroy) { arena_.DestroyObjects(); }

TEST_F(ArenaTest, TestJustA) { TestCombo(10000, 0); }

TEST_F(ArenaTest, TestJustA2) {
  // On 32-bit this should perfectly fill all the blocks it uses
  TestCombo(2048, 0);
}

TEST_F(ArenaTest, TestJustB) { TestCombo(0, 10000); }

TEST_F(ArenaTest, TestJustB2) {
  // On 64-bit this should perfectly fill all the blocks it uses
  TestCombo(0, 2048);
}

// An object larger than the default 64 KB chunk gets a dedicated
// oversized chunk; allocation works and the destructor runs exactly
// once at DestroyObjects().
TEST_F(ArenaTest, TestOversizeAllocation) {
  CheckPtr(new (&arena_) KidBig(this));
  EXPECT_EQ(1u, arena_.block_count());

  arena_.DestroyObjects();
  EXPECT_EQ(1, made_big_);
  EXPECT_EQ(1, destroyed_big_);
  EXPECT_EQ(0u, arena_.block_count());

  // Mix oversize with normal allocations: the small object before the
  // big one lives in its own chunk, the big one forces a dedicated
  // chunk, and every destructor still runs exactly once.
  ClearStats();
  CheckPtr(new (&arena_) KidA(this));
  CheckPtr(new (&arena_) KidBig(this));
  CheckPtr(new (&arena_) KidB(this));
  EXPECT_EQ(3u, arena_.block_count());

  arena_.DestroyObjects();
  EXPECT_EQ(1, made_a_);
  EXPECT_EQ(1, made_big_);
  EXPECT_EQ(1, made_b_);
  EXPECT_EQ(1, destroyed_a_);
  EXPECT_EQ(1, destroyed_big_);
  EXPECT_EQ(1, destroyed_b_);
}

TEST_F(ArenaTest, TestMix) { TestCombo(10000, 20000); }

// Make sure we work again after a clear
TEST_F(ArenaTest, TestReuse) {
  TestCombo(10000, 20000);
  ClearStats();
  TestCombo(20000, 10000);
}

// Reuse after DestroyObjects with a small block size, so several chunks
// are built, torn down, and rebuilt.
TEST_F(ArenaTest, TestReuseSmallBlocks) {
  Arena<Base> small_arena(256);
  for (int i = 0; i < 100; ++i) {
    Base* p = new (&small_arena) KidA(this);
    p->Made();
  }
  small_arena.DestroyObjects();
  EXPECT_EQ(100, made_a_);
  EXPECT_EQ(100, destroyed_a_);
  EXPECT_EQ(0u, small_arena.bytes_allocated());
  EXPECT_EQ(0u, small_arena.block_count());

  ClearStats();
  for (int i = 0; i < 100; ++i) {
    Base* p = new (&small_arena) KidB(this);
    p->Made();
  }
  small_arena.DestroyObjects();
  EXPECT_EQ(100, made_b_);
  EXPECT_EQ(100, destroyed_b_);
}

// Clear() frees chunks without running destructors, and the arena is
// reusable afterwards.
TEST_F(ArenaTest, TestClearSkipsDestructors) {
  Base* p = new (&arena_) KidA(this);
  p->Made();
  arena_.Clear();
  EXPECT_EQ(1, made_a_);
  EXPECT_EQ(0, destroyed_a_);
  EXPECT_EQ(0u, arena_.bytes_allocated());
  EXPECT_EQ(0u, arena_.block_count());

  ClearStats();
  TestCombo(10, 10);
}

// HtmlNode-like lifetime: payload released explicitly before
// DestroyObjects(); the destructor must run exactly once and not
// double-free the payload.
TEST_F(ArenaTest, TestExplicitlyFreedPayload) {
  KidPayload* p = new (&arena_) KidPayload(this);
  p->Made();
  ASSERT_TRUE(p->HasPayload());
  p->FreePayload();
  ASSERT_FALSE(p->HasPayload());

  KidPayload* q = new (&arena_) KidPayload(this);
  q->Made();
  ASSERT_TRUE(q->HasPayload());  // still owned at destroy time

  arena_.DestroyObjects();
  EXPECT_EQ(2, made_payload_);
  EXPECT_EQ(2, destroyed_payload_);
}

// The arena must be emptied (Clear() or DestroyObjects()) before it is
// destroyed. With asserts enabled, destroying a non-empty arena aborts.
#ifndef NDEBUG
TEST_F(ArenaTest, TestNonEmptyArenaDeath) {
  EXPECT_DEATH(
      {
        Arena<Base> arena;
        Base* p = new (&arena) KidA(this);
        p->Made();
      },
      "");
}
#endif

// Tests for alignment helper.
TEST_F(ArenaTest, TestAlign) {
  // A few that work regardless of arch, to sanity-check
  // the more through loop below
  EXPECT_EQ(8u, arena_.ExpandToAlign(8));
  EXPECT_EQ(16u, arena_.ExpandToAlign(15));
  EXPECT_EQ(16u, arena_.ExpandToAlign(14));
  EXPECT_EQ(16u, arena_.ExpandToAlign(13));

  for (size_t t = 0; t < 1000u; ++t) {
    size_t expanded = arena_.ExpandToAlign(t);
    if ((t % arena_.kAlign) == 0) {
      // Well-aligned case.
      EXPECT_EQ(t, expanded);
    } else {
      // Otherwise should be next multiple
      EXPECT_EQ(((t / arena_.kAlign) + 1) * arena_.kAlign, expanded);
    }
  }
}

}  // namespace net_instaweb
