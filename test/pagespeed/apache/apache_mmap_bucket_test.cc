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

// Unit tests for the PAGESPEED_MMAP aliasing bucket (the Apache zero-copy
// sink), against the real APR bucket API.  The Cyclone lease is scripted
// through the MappedSharedString hook callbacks, so every barrier verdict --
// alias, de-alias-by-copy, torn -- is driven deterministically, and the
// "poison" pattern (overwrite the fake mapped region after a copy-out)
// proves no aliased read survives a de-alias.

#include "pagespeed/apache/apache_mmap_bucket.h"

#include <cstring>
#include <vector>

#include "apr_buckets.h"  // NOLINT
#include "apr_pools.h"    // NOLINT
#include "pagespeed/kernel/base/mapped_shared_string.h"
#include "pagespeed/kernel/base/string.h"
#include "pagespeed/kernel/base/string_util.h"
#include "test/pagespeed/apache/mock_apache.h"
#include "test/pagespeed/kernel/base/gtest.h"

namespace net_instaweb {

namespace {

const char kPayload[] = "0123456789abcdefghijklmnopqrstuvwxyz";

// Scripted Cyclone-lease stand-in wired through the MappedSharedString
// hooks.  strict_results is consumed one verdict per RenewLeaseStrict call
// (the last entry repeats), mirroring a lease whose state changes between
// barrier checks.
struct HookScript {
  std::vector<LeaseRenewal> strict_results;
  size_t strict_calls = 0;
  uint64_t ns_until_forced_wrap = ~static_cast<uint64_t>(0);
  int released = 0;
};

void ScriptRelease(void* user_data) {
  static_cast<HookScript*>(user_data)->released++;
}

int ScriptRenew(void* user_data) {
  return 1;  // Epoch-only renew: unused by the bucket, always valid.
}

int ScriptRenewStrict(void* user_data) {
  HookScript* script = static_cast<HookScript*>(user_data);
  size_t i = script->strict_calls;
  if (i >= script->strict_results.size()) {
    i = script->strict_results.size() - 1;
  }
  script->strict_calls++;
  return static_cast<int>(script->strict_results[i]);
}

uint64_t ScriptNsUntilForcedWrap(void* user_data) {
  return static_cast<HookScript*>(user_data)->ns_until_forced_wrap;
}

class ApacheMmapBucketTest : public testing::Test {
 protected:
  void SetUp() override {
    MockApache::Initialize();  // apr_initialize underneath.
    apr_pool_create(&pool_, nullptr);
    alloc_ = apr_bucket_alloc_create(pool_);
    brigade_ = apr_brigade_create(pool_, alloc_);
    memcpy(region_, kPayload, sizeof(kPayload));
    script_.strict_results.assign(1, LeaseRenewal::kOk);
  }

  void TearDown() override {
    apr_brigade_destroy(brigade_);
    apr_pool_destroy(pool_);
    MockApache::Terminate();
  }

  MappedSharedString MakePin() {
    return MappedSharedString::FromMappedView(
        region_, sizeof(kPayload) - 1, ScriptRelease, ScriptRenew,
        ScriptRenewStrict, ScriptNsUntilForcedWrap, &script_);
  }

  // Creates the bucket over the whole payload and parks it in the brigade
  // (whose destruction destroys leftover buckets, like a request teardown).
  apr_bucket* MakeBucket(const MappedSharedString& pin) {
    apr_bucket* b =
        CreateMmapAliasBucket(StringPiece(region_, sizeof(kPayload) - 1), pin,
                              nullptr, nullptr, alloc_);
    APR_BRIGADE_INSERT_TAIL(brigade_, b);
    return b;
  }

  apr_pool_t* pool_ = nullptr;
  apr_bucket_alloc_t* alloc_ = nullptr;
  apr_bucket_brigade* brigade_ = nullptr;
  char region_[sizeof(kPayload)];
  HookScript script_;
};

TEST_F(ApacheMmapBucketTest, ReadAliasesWhileLeaseOk) {
  MappedSharedString pin = MakePin();
  apr_bucket* b = MakeBucket(pin);
  const char* data = nullptr;
  apr_size_t len = 0;
  ASSERT_EQ(APR_SUCCESS, apr_bucket_read(b, &data, &len, APR_BLOCK_READ));
  // Zero copy: the returned pointer IS the mapped region.
  EXPECT_EQ(region_, data);
  EXPECT_EQ(sizeof(kPayload) - 1, len);
  EXPECT_TRUE(IsMmapAliasBucket(b));
  EXPECT_EQ(1u, script_.strict_calls);  // One barrier run per read.
}

TEST_F(ApacheMmapBucketTest, SplitSiblingsAliasTheirWindows) {
  MappedSharedString pin = MakePin();
  apr_bucket* b = MakeBucket(pin);
  ASSERT_EQ(APR_SUCCESS, apr_bucket_split(b, 10));
  apr_bucket* tail = APR_BUCKET_NEXT(b);
  const char* data = nullptr;
  apr_size_t len = 0;
  ASSERT_EQ(APR_SUCCESS, apr_bucket_read(b, &data, &len, APR_BLOCK_READ));
  EXPECT_EQ(region_, data);
  EXPECT_EQ(10u, len);
  ASSERT_EQ(APR_SUCCESS, apr_bucket_read(tail, &data, &len, APR_BLOCK_READ));
  EXPECT_EQ(region_ + 10, data);
  EXPECT_EQ(sizeof(kPayload) - 1 - 10, len);
  // The pin is shared: destroying one sibling must not release it.
  apr_bucket_delete(b);
  EXPECT_EQ(0, script_.released);
  pin = MappedSharedString();  // Drop the test's own reference too.
  EXPECT_EQ(0, script_.released);
  apr_bucket_delete(tail);
  EXPECT_EQ(1, script_.released);
}

TEST_F(ApacheMmapBucketTest, DestroyWithoutReadReleasesPin) {
  MappedSharedString pin = MakePin();
  apr_bucket* b = MakeBucket(pin);
  apr_bucket_delete(b);
  pin = MappedSharedString();
  EXPECT_EQ(1, script_.released);
  EXPECT_EQ(0u, script_.strict_calls);
}

TEST_F(ApacheMmapBucketTest, CopyOutOnWrapInFlight) {
  script_.strict_results = {LeaseRenewal::kCopyNow, LeaseRenewal::kOk};
  MappedSharedString pin = MakePin();
  apr_bucket* b = MakeBucket(pin);
  const char* data = nullptr;
  apr_size_t len = 0;
  ASSERT_EQ(APR_SUCCESS, apr_bucket_read(b, &data, &len, APR_BLOCK_READ));
  // De-aliased: owned heap bytes, byte-identical, bucket morphed.
  EXPECT_NE(static_cast<const char*>(region_), data);
  ASSERT_EQ(sizeof(kPayload) - 1, len);
  EXPECT_EQ(0, memcmp(kPayload, data, len));
  EXPECT_FALSE(IsMmapAliasBucket(b));
  // Barrier + post-copy verify.
  EXPECT_EQ(2u, script_.strict_calls);
  // The morph dropped the bucket's pin reference immediately.
  pin = MappedSharedString();
  EXPECT_EQ(1, script_.released);
  // Poison the region: the copy must not see it.
  memset(region_, 'X', sizeof(region_));
  ASSERT_EQ(APR_SUCCESS, apr_bucket_read(b, &data, &len, APR_BLOCK_READ));
  EXPECT_EQ(0, memcmp(kPayload, data, len));
}

TEST_F(ApacheMmapBucketTest, CopyOutOnForcedWrapImminent) {
  script_.ns_until_forced_wrap = 100ULL * 1000 * 1000;  // 100ms < margin.
  MappedSharedString pin = MakePin();
  apr_bucket* b = MakeBucket(pin);
  const char* data = nullptr;
  apr_size_t len = 0;
  ASSERT_EQ(APR_SUCCESS, apr_bucket_read(b, &data, &len, APR_BLOCK_READ));
  EXPECT_NE(static_cast<const char*>(region_), data);
  EXPECT_EQ(0, memcmp(kPayload, data, len));
  EXPECT_FALSE(IsMmapAliasBucket(b));
}

TEST_F(ApacheMmapBucketTest, CopyOutOnLeasesOff) {
  script_.strict_results = {LeaseRenewal::kLeasesOff};
  MappedSharedString pin = MakePin();
  apr_bucket* b = MakeBucket(pin);
  const char* data = nullptr;
  apr_size_t len = 0;
  // Leases-off must degrade to a copy, never fail the serve.
  ASSERT_EQ(APR_SUCCESS, apr_bucket_read(b, &data, &len, APR_BLOCK_READ));
  EXPECT_NE(static_cast<const char*>(region_), data);
  EXPECT_EQ(0, memcmp(kPayload, data, len));
}

TEST_F(ApacheMmapBucketTest, TornAtBarrierFailsRead) {
  script_.strict_results = {LeaseRenewal::kTorn};
  MappedSharedString pin = MakePin();
  apr_bucket* b = MakeBucket(pin);
  const char* data = nullptr;
  apr_size_t len = 0;
  EXPECT_NE(APR_SUCCESS, apr_bucket_read(b, &data, &len, APR_BLOCK_READ));
  // Still our type; the pin is released by destroy (brigade teardown).
  EXPECT_TRUE(IsMmapAliasBucket(b));
}

TEST_F(ApacheMmapBucketTest, CopyThenVerifyTornFailsRead) {
  // Barrier says "wrap in flight, copy now"; the post-memcpy verify then
  // discovers the wrap committed over the borrow.  The copy may be torn,
  // so the read must fail -- copy-then-verify, never check-then-copy.
  script_.strict_results = {LeaseRenewal::kCopyNow, LeaseRenewal::kTorn};
  MappedSharedString pin = MakePin();
  apr_bucket* b = MakeBucket(pin);
  const char* data = nullptr;
  apr_size_t len = 0;
  EXPECT_NE(APR_SUCCESS, apr_bucket_read(b, &data, &len, APR_BLOCK_READ));
  EXPECT_EQ(2u, script_.strict_calls);
}

TEST_F(ApacheMmapBucketTest, SetasideCopiesOutAndVerifies) {
  MappedSharedString pin = MakePin();
  apr_bucket* b = MakeBucket(pin);
  ASSERT_EQ(APR_SUCCESS, apr_bucket_setaside(b, pool_));
  EXPECT_FALSE(IsMmapAliasBucket(b));
  // Retain-by-copy: one verify ran after the memcpy.
  EXPECT_EQ(1u, script_.strict_calls);
  pin = MappedSharedString();
  EXPECT_EQ(1, script_.released);
  memset(region_, 'X', sizeof(region_));  // Poison after park.
  const char* data = nullptr;
  apr_size_t len = 0;
  ASSERT_EQ(APR_SUCCESS, apr_bucket_read(b, &data, &len, APR_BLOCK_READ));
  ASSERT_EQ(sizeof(kPayload) - 1, len);
  EXPECT_EQ(0, memcmp(kPayload, data, len));
}

// httpd's deferred-write path DISCARDS setaside errors, so a torn borrow
// discovered while parking must NOT surface as a (swallowed) error return:
// setaside reports success but poisons the bucket, and the failure fires on
// the next read -- the point the core output filter honors errors.
TEST_F(ApacheMmapBucketTest, SetasideTornPoisonsBucket) {
  script_.strict_results = {LeaseRenewal::kTorn};
  MappedSharedString pin = MakePin();
  apr_bucket* b = MakeBucket(pin);
  ASSERT_EQ(APR_SUCCESS, apr_bucket_setaside(b, pool_));
  EXPECT_TRUE(IsTornAliasBucket(b));
  EXPECT_FALSE(IsMmapAliasBucket(b));
  // The poison morph released the bucket's pin reference.
  pin = MappedSharedString();
  EXPECT_EQ(1, script_.released);
  // Every subsequent read fails; a repeated read stays failed.
  const char* data = nullptr;
  apr_size_t len = 0;
  EXPECT_NE(APR_SUCCESS, apr_bucket_read(b, &data, &len, APR_BLOCK_READ));
  EXPECT_NE(APR_SUCCESS, apr_bucket_read(b, &data, &len, APR_BLOCK_READ));
  // A second park attempt is a no-op success, never an error to swallow.
  EXPECT_EQ(APR_SUCCESS, apr_bucket_setaside(b, pool_));
}

}  // namespace

}  // namespace net_instaweb
