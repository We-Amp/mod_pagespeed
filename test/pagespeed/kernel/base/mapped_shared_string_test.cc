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

#include "pagespeed/kernel/base/mapped_shared_string.h"

#include <atomic>
#include <thread>
#include <vector>

#include "pagespeed/kernel/base/shared_string.h"
#include "pagespeed/kernel/base/string.h"
#include "test/pagespeed/kernel/base/gtest.h"

namespace net_instaweb {

class MappedSharedStringTest : public testing::Test {
 protected:
  // Test data for mapped views
  static constexpr const char* kTestData = "Hello, World!";
  static constexpr size_t kTestDataSize = 13;

  // Counter for tracking release callback invocations
  std::atomic<int> release_count_{0};

  static void ReleaseCallback(void* user_data) {
    auto* counter = static_cast<std::atomic<int>*>(user_data);
    counter->fetch_add(1);
  }
};

// ============================================================================
// Construction from SharedString
// ============================================================================

TEST_F(MappedSharedStringTest, DefaultConstruction) {
  MappedSharedString mss;
  EXPECT_TRUE(mss.empty());
  EXPECT_EQ(0u, mss.size());
  EXPECT_FALSE(mss.is_mapped());
}

TEST_F(MappedSharedStringTest, ConstructFromSharedString) {
  SharedString ss("hello");
  MappedSharedString mss(ss);

  EXPECT_EQ("hello", mss.Value());
  EXPECT_EQ(5u, mss.size());
  EXPECT_FALSE(mss.is_mapped());
  EXPECT_FALSE(mss.empty());

  // AsSharedString should return non-null for owned strings
  const SharedString* ptr = mss.AsSharedString();
  ASSERT_NE(nullptr, ptr);
  EXPECT_EQ("hello", ptr->Value());
}

TEST_F(MappedSharedStringTest, ConstructFromEmptySharedString) {
  SharedString ss;
  MappedSharedString mss(ss);

  EXPECT_TRUE(mss.empty());
  EXPECT_EQ(0u, mss.size());
  EXPECT_FALSE(mss.is_mapped());
}

// ============================================================================
// Construction from Mapped View
// ============================================================================

TEST_F(MappedSharedStringTest, ConstructFromMappedView) {
  MappedSharedString mss = MappedSharedString::FromMappedView(
      kTestData, kTestDataSize, ReleaseCallback, &release_count_);

  EXPECT_EQ("Hello, World!", mss.Value());
  EXPECT_EQ(kTestDataSize, mss.size());
  EXPECT_TRUE(mss.is_mapped());
  EXPECT_FALSE(mss.empty());

  // AsSharedString should return null for mapped strings
  EXPECT_EQ(nullptr, mss.AsSharedString());

  // data() should point to the original mapped data
  EXPECT_EQ(kTestData, mss.data());

  // Release callback not called yet
  EXPECT_EQ(0, release_count_.load());
}

TEST_F(MappedSharedStringTest, MappedViewReleaseOnDestruction) {
  {
    MappedSharedString mss = MappedSharedString::FromMappedView(
        kTestData, kTestDataSize, ReleaseCallback, &release_count_);
    EXPECT_EQ(0, release_count_.load());
  }
  // After destruction, release callback should be called
  EXPECT_EQ(1, release_count_.load());
}

TEST_F(MappedSharedStringTest, MappedViewNullCallback) {
  // Should work with null callback
  MappedSharedString mss = MappedSharedString::FromMappedView(
      kTestData, kTestDataSize, nullptr, nullptr);

  EXPECT_EQ("Hello, World!", mss.Value());
  EXPECT_TRUE(mss.is_mapped());
  // Destructor should not crash
}

TEST_F(MappedSharedStringTest, MappedViewEmptyData) {
  MappedSharedString mss = MappedSharedString::FromMappedView(
      "", 0, ReleaseCallback, &release_count_);

  EXPECT_TRUE(mss.empty());
  EXPECT_EQ(0u, mss.size());
  EXPECT_TRUE(mss.is_mapped());
}

// ============================================================================
// Copy Semantics
// ============================================================================

TEST_F(MappedSharedStringTest, CopyConstructorOwned) {
  SharedString ss("hello");
  MappedSharedString mss1(ss);
  MappedSharedString mss2(mss1);

  EXPECT_EQ("hello", mss1.Value());
  EXPECT_EQ("hello", mss2.Value());
  EXPECT_FALSE(mss1.is_mapped());
  EXPECT_FALSE(mss2.is_mapped());

  // Both should refer to SharedStrings that share storage
  const SharedString* ss1 = mss1.AsSharedString();
  const SharedString* ss2 = mss2.AsSharedString();
  ASSERT_NE(nullptr, ss1);
  ASSERT_NE(nullptr, ss2);
  EXPECT_TRUE(ss1->SharesStorage(*ss2));
}

TEST_F(MappedSharedStringTest, CopyConstructorMapped) {
  MappedSharedString mss1 = MappedSharedString::FromMappedView(
      kTestData, kTestDataSize, ReleaseCallback, &release_count_);
  MappedSharedString mss2(mss1);

  EXPECT_EQ("Hello, World!", mss1.Value());
  EXPECT_EQ("Hello, World!", mss2.Value());
  EXPECT_TRUE(mss1.is_mapped());
  EXPECT_TRUE(mss2.is_mapped());

  // Both should point to the same data
  EXPECT_EQ(mss1.data(), mss2.data());

  // Neither is unique
  EXPECT_FALSE(mss1.unique());
  EXPECT_FALSE(mss2.unique());

  // Release callback not called yet
  EXPECT_EQ(0, release_count_.load());
}

TEST_F(MappedSharedStringTest, CopyAssignmentOwned) {
  SharedString ss("hello");
  MappedSharedString mss1(ss);
  MappedSharedString mss2;

  mss2 = mss1;

  EXPECT_EQ("hello", mss1.Value());
  EXPECT_EQ("hello", mss2.Value());
}

TEST_F(MappedSharedStringTest, CopyAssignmentMapped) {
  MappedSharedString mss1 = MappedSharedString::FromMappedView(
      kTestData, kTestDataSize, ReleaseCallback, &release_count_);
  MappedSharedString mss2;

  mss2 = mss1;

  EXPECT_EQ("Hello, World!", mss2.Value());
  EXPECT_TRUE(mss2.is_mapped());
  EXPECT_EQ(0, release_count_.load());
}

TEST_F(MappedSharedStringTest, MappedViewSharedReleaseOnLastCopy) {
  {
    MappedSharedString mss1 = MappedSharedString::FromMappedView(
        kTestData, kTestDataSize, ReleaseCallback, &release_count_);
    {
      MappedSharedString mss2(mss1);
      EXPECT_EQ(0, release_count_.load());
    }
    // mss2 destroyed, but mss1 still holds a reference
    EXPECT_EQ(0, release_count_.load());
  }
  // mss1 destroyed, release callback should be called now
  EXPECT_EQ(1, release_count_.load());
}

// ============================================================================
// Move Semantics
// ============================================================================

TEST_F(MappedSharedStringTest, MoveConstructorOwned) {
  SharedString ss("hello");
  MappedSharedString mss1(ss);
  MappedSharedString mss2(std::move(mss1));

  EXPECT_EQ("hello", mss2.Value());
  EXPECT_FALSE(mss2.is_mapped());

  // mss1 should be in valid empty state
  EXPECT_TRUE(mss1.empty());
}

TEST_F(MappedSharedStringTest, MoveConstructorMapped) {
  MappedSharedString mss1 = MappedSharedString::FromMappedView(
      kTestData, kTestDataSize, ReleaseCallback, &release_count_);
  MappedSharedString mss2(std::move(mss1));

  EXPECT_EQ("Hello, World!", mss2.Value());
  EXPECT_TRUE(mss2.is_mapped());
  EXPECT_TRUE(mss1.empty());

  // Release callback not called - ownership transferred
  EXPECT_EQ(0, release_count_.load());
}

TEST_F(MappedSharedStringTest, MoveAssignmentOwned) {
  SharedString ss("hello");
  MappedSharedString mss1(ss);
  MappedSharedString mss2;

  mss2 = std::move(mss1);

  EXPECT_EQ("hello", mss2.Value());
  EXPECT_TRUE(mss1.empty());
}

TEST_F(MappedSharedStringTest, MoveAssignmentMapped) {
  MappedSharedString mss1 = MappedSharedString::FromMappedView(
      kTestData, kTestDataSize, ReleaseCallback, &release_count_);
  MappedSharedString mss2;

  mss2 = std::move(mss1);

  EXPECT_EQ("Hello, World!", mss2.Value());
  EXPECT_TRUE(mss2.is_mapped());
  EXPECT_TRUE(mss1.empty());
  EXPECT_EQ(0, release_count_.load());
}

// ============================================================================
// ToOwned Conversion
// ============================================================================

TEST_F(MappedSharedStringTest, ToOwnedFromOwned) {
  SharedString original("hello");
  MappedSharedString mss(original);

  SharedString owned = mss.ToOwned();

  EXPECT_EQ("hello", owned.Value());
  // Should share storage since no copy was needed
  EXPECT_TRUE(original.SharesStorage(owned));
}

TEST_F(MappedSharedStringTest, ToOwnedFromMapped) {
  MappedSharedString mss = MappedSharedString::FromMappedView(
      kTestData, kTestDataSize, ReleaseCallback, &release_count_);

  SharedString owned = mss.ToOwned();

  EXPECT_EQ("Hello, World!", owned.Value());
  // Data should be copied, so pointer should be different
  EXPECT_NE(kTestData, owned.data());

  // Original MappedSharedString should still be valid
  EXPECT_EQ("Hello, World!", mss.Value());
}

TEST_F(MappedSharedStringTest, ToOwnedFromMappedReleasesIndependently) {
  SharedString owned;
  {
    MappedSharedString mss = MappedSharedString::FromMappedView(
        kTestData, kTestDataSize, ReleaseCallback, &release_count_);
    owned = mss.ToOwned();
    EXPECT_EQ(0, release_count_.load());
  }
  // MappedSharedString destroyed, but owned SharedString still valid
  EXPECT_EQ(1, release_count_.load());
  EXPECT_EQ("Hello, World!", owned.Value());
}

// ============================================================================
// Unique Check
// ============================================================================

TEST_F(MappedSharedStringTest, UniqueOwned) {
  SharedString ss("hello");
  MappedSharedString mss(ss);

  // The MappedSharedString's SharedString shares storage with ss
  EXPECT_FALSE(mss.unique());

  SharedString ss_copy = ss;
  EXPECT_FALSE(mss.unique());
}

TEST_F(MappedSharedStringTest, UniqueMapped) {
  MappedSharedString mss = MappedSharedString::FromMappedView(
      kTestData, kTestDataSize, nullptr, nullptr);

  EXPECT_TRUE(mss.unique());

  MappedSharedString mss2(mss);
  EXPECT_FALSE(mss.unique());
  EXPECT_FALSE(mss2.unique());
}

// ============================================================================
// Self-Assignment
// ============================================================================

TEST_F(MappedSharedStringTest, SelfCopyAssignment) {
  MappedSharedString mss = MappedSharedString::FromMappedView(
      kTestData, kTestDataSize, ReleaseCallback, &release_count_);

#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wself-assign-overloaded"
  mss = mss;  // Self-assignment
#pragma clang diagnostic pop

  EXPECT_EQ("Hello, World!", mss.Value());
  EXPECT_EQ(0, release_count_.load());
}

TEST_F(MappedSharedStringTest, SelfMoveAssignment) {
  MappedSharedString mss = MappedSharedString::FromMappedView(
      kTestData, kTestDataSize, ReleaseCallback, &release_count_);

#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wself-move"
  mss = std::move(mss);  // Self-move
#pragma clang diagnostic pop

  // Self-move is a no-op due to self-assignment check (this != &other).
  // The object remains unchanged and valid.
  EXPECT_FALSE(mss.empty());
  EXPECT_TRUE(mss.is_mapped());
  EXPECT_EQ("Hello, World!", mss.Value());

  // Release callback should NOT have been called yet
  EXPECT_EQ(0, release_count_.load());
}

// ============================================================================
// Concurrent Access Tests
// ============================================================================

TEST_F(MappedSharedStringTest, ConcurrentCopyAndDestroy) {
  // Test that multiple threads can safely copy and destroy the same
  // MappedSharedString concurrently.
  constexpr int kNumThreads = 8;
  constexpr int kIterationsPerThread = 1000;

  MappedSharedString mss = MappedSharedString::FromMappedView(
      kTestData, kTestDataSize, ReleaseCallback, &release_count_);

  std::vector<std::thread> threads;
  for (int i = 0; i < kNumThreads; ++i) {
    threads.emplace_back([&mss]() {
      for (int j = 0; j < kIterationsPerThread; ++j) {
        // Copy, use, and destroy
        MappedSharedString copy = mss;
        EXPECT_EQ("Hello, World!", copy.Value());
      }
    });
  }

  for (auto& t : threads) {
    t.join();
  }

  // The original should still be valid
  EXPECT_EQ("Hello, World!", mss.Value());
  EXPECT_EQ(0, release_count_.load());
}

TEST_F(MappedSharedStringTest, ConcurrentRefCountStress) {
  // Stress test the reference counting with many threads creating and
  // destroying copies of a mapped string.
  constexpr int kNumThreads = 16;
  constexpr int kIterationsPerThread = 500;

  {
    MappedSharedString mss = MappedSharedString::FromMappedView(
        kTestData, kTestDataSize, ReleaseCallback, &release_count_);

    std::atomic<int> active_copies{0};
    std::vector<std::thread> threads;

    for (int i = 0; i < kNumThreads; ++i) {
      threads.emplace_back([&mss, &active_copies]() {
        for (int j = 0; j < kIterationsPerThread; ++j) {
          MappedSharedString copy = mss;
          active_copies.fetch_add(1);
          // Brief use
          (void)copy.Value();
          active_copies.fetch_sub(1);
        }
      });
    }

    for (auto& t : threads) {
      t.join();
    }

    // All thread-local copies should be destroyed
    EXPECT_EQ(0, active_copies.load());
    // But original still holds a reference
    EXPECT_EQ(0, release_count_.load());
  }

  // Now original is destroyed, release should be called exactly once
  EXPECT_EQ(1, release_count_.load());
}

// ============================================================================
// Edge Case Tests
// ============================================================================

TEST_F(MappedSharedStringTest, NullDataPointerWithZeroSize) {
  // Test creating a mapped view with null data pointer and zero size.
  // This is a valid edge case (empty mapped data).
  MappedSharedString mss = MappedSharedString::FromMappedView(
      nullptr, 0, ReleaseCallback, &release_count_);

  EXPECT_TRUE(mss.empty());
  EXPECT_EQ(0u, mss.size());
  EXPECT_TRUE(mss.is_mapped());
  // Value() should return an empty StringPiece
  EXPECT_EQ("", mss.Value());
  EXPECT_EQ(0, release_count_.load());
}

TEST_F(MappedSharedStringTest, NullDataPointerReleaseCallback) {
  // Verify the release callback is still called for null data pointer.
  {
    MappedSharedString mss = MappedSharedString::FromMappedView(
        nullptr, 0, ReleaseCallback, &release_count_);
    EXPECT_EQ(0, release_count_.load());
  }
  // Release callback should be called on destruction
  EXPECT_EQ(1, release_count_.load());
}

TEST_F(MappedSharedStringTest, ZeroCopyPointerEquality) {
  // Verify that the zero-copy path actually uses the same pointer.
  // This confirms no copy is made for mapped data.
  MappedSharedString mss = MappedSharedString::FromMappedView(
      kTestData, kTestDataSize, nullptr, nullptr);

  // The data pointer should be exactly the same as the input
  EXPECT_EQ(kTestData, mss.data());

  // Value().data() should also point to the same location
  EXPECT_EQ(kTestData, mss.Value().data());

  // Copy should share the same pointer
  MappedSharedString copy = mss;
  EXPECT_EQ(kTestData, copy.data());
}

TEST_F(MappedSharedStringTest, OwnedDoesNotSharePointerWithOriginal) {
  // Verify that ToOwned() creates a copy with a different pointer.
  MappedSharedString mss = MappedSharedString::FromMappedView(
      kTestData, kTestDataSize, nullptr, nullptr);

  SharedString owned = mss.ToOwned();

  // The owned copy should have different storage
  EXPECT_NE(kTestData, owned.data());

  // But the content should be the same
  EXPECT_EQ(mss.Value(), owned.Value());
}

TEST_F(MappedSharedStringTest, OwnedSharedStringSharesPointer) {
  // For owned MappedSharedString, verify SharedString semantics are preserved.
  SharedString original("test data");
  MappedSharedString mss(original);

  // AsSharedString should return a pointer to internal storage
  const SharedString* internal = mss.AsSharedString();
  ASSERT_NE(nullptr, internal);

  // The internal SharedString should share storage with original
  EXPECT_TRUE(original.SharesStorage(*internal));

  // ToOwned should also share storage (no copy for already-owned data)
  SharedString owned = mss.ToOwned();
  EXPECT_TRUE(original.SharesStorage(owned));
}

// ============================================================================
// CopyMappedVerified
// ============================================================================

namespace {

int g_strict_verdict = 0;  // LeaseRenewal as int; set per test.
int StrictVerdictHook(void* /*user_data*/) { return g_strict_verdict; }
int RenewHook(void* /*user_data*/) { return 1; }
uint64_t NsUntilHook(void* /*user_data*/) { return ~static_cast<uint64_t>(0); }

}  // namespace

TEST_F(MappedSharedStringTest, CopyMappedVerifiedClean) {
  g_strict_verdict = static_cast<int>(LeaseRenewal::kOk);
  MappedSharedString mss = MappedSharedString::FromMappedView(
      kTestData, kTestDataSize, ReleaseCallback, RenewHook, StrictVerdictHook,
      NsUntilHook, &release_count_);
  GoogleString out;
  EXPECT_TRUE(CopyMappedVerified(mss.Value(), mss, &out));
  EXPECT_EQ("Hello, World!", out);
  EXPECT_NE(kTestData, out.data());  // A real copy, not an alias.
}

TEST_F(MappedSharedStringTest, CopyMappedVerifiedCopyNowStillServes) {
  g_strict_verdict = static_cast<int>(LeaseRenewal::kCopyNow);
  MappedSharedString mss = MappedSharedString::FromMappedView(
      kTestData, kTestDataSize, ReleaseCallback, RenewHook, StrictVerdictHook,
      NsUntilHook, &release_count_);
  GoogleString out;
  // A wrap in flight leaves the region intact: the copy is servable.
  EXPECT_TRUE(CopyMappedVerified(mss.Value(), mss, &out));
  EXPECT_EQ("Hello, World!", out);
}

TEST_F(MappedSharedStringTest, CopyMappedVerifiedTornFails) {
  g_strict_verdict = static_cast<int>(LeaseRenewal::kTorn);
  MappedSharedString mss = MappedSharedString::FromMappedView(
      kTestData, kTestDataSize, ReleaseCallback, RenewHook, StrictVerdictHook,
      NsUntilHook, &release_count_);
  GoogleString out;
  EXPECT_FALSE(CopyMappedVerified(mss.Value(), mss, &out));
}

TEST_F(MappedSharedStringTest, CopyMappedVerifiedNoHooksAlwaysVerifies) {
  // Hook-less mapped view (legacy) and plain owned storage: kLeasesOff,
  // copy without failing.
  MappedSharedString mapped = MappedSharedString::FromMappedView(
      kTestData, kTestDataSize, ReleaseCallback, &release_count_);
  GoogleString out;
  EXPECT_TRUE(CopyMappedVerified(mapped.Value(), mapped, &out));
  EXPECT_EQ("Hello, World!", out);

  MappedSharedString owned(SharedString("owned-bytes"));
  EXPECT_TRUE(CopyMappedVerified(owned.Value(), owned, &out));
  EXPECT_EQ("owned-bytes", out);
}

}  // namespace net_instaweb
