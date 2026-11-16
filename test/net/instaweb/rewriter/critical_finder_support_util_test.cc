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

#include "net/instaweb/rewriter/public/critical_finder_support_util.h"

#include "net/instaweb/rewriter/critical_keys.pb.h"
#include "net/instaweb/rewriter/public/rewrite_options.h"
#include "pagespeed/kernel/base/string.h"
#include "pagespeed/kernel/base/string_util.h"
#include "test/net/instaweb/rewriter/rewrite_test_base.h"
#include "test/net/instaweb/rewriter/test_rewrite_driver_factory.h"
#include "test/pagespeed/kernel/base/gtest.h"
#include "test/pagespeed/kernel/base/mock_timer.h"

namespace net_instaweb {
namespace {

class CriticalFinderSupportUtilTest : public RewriteTestBase {
 protected:
  CriticalFinderSupportUtilTest() {}

  ~CriticalFinderSupportUtilTest() override {}

  void SetUp() override {
    RewriteTestBase::SetUp();
    critical_keys_proto_.Clear();
  }

  void TestPrepareForBeaconInsertionHelper(const StringSet& keys,
                                           BeaconStatus expected_status) {
    BeaconMetadata result;
    UpdateCandidateKeys(keys, &critical_keys_proto_, true);
    PrepareForBeaconInsertionHelper(
        &critical_keys_proto_, factory()->nonce_generator(), rewrite_driver(),
        true /* using_candidate_key_detection */, &result);
    EXPECT_EQ(expected_status, result.status);
    // Clear the count of expired nonces. We aren't actually sending back the
    // nonces in our test, so they can expire and put us into low frequency
    // beaconing mode when we aren't expecting it to.
    critical_keys_proto_.set_nonces_recently_expired(0);
  }

  CriticalKeys critical_keys_proto_;
};

TEST_F(CriticalFinderSupportUtilTest,
       PrepareForBeaconInsertionHelperWithCandidateKeys) {
  StringSet keys;

  keys.insert("a");
  TestPrepareForBeaconInsertionHelper(keys, kBeaconWithNonce);

  // We shouldn't get another beacon until we either change the keys, or time
  // advances to high frequency beaconing amount.
  TestPrepareForBeaconInsertionHelper(keys, kDoNotBeacon);
  keys.insert("b");
  TestPrepareForBeaconInsertionHelper(keys, kBeaconWithNonce);
  TestPrepareForBeaconInsertionHelper(keys, kDoNotBeacon);
  factory()->mock_timer()->AdvanceMs(options()->beacon_reinstrument_time_sec() *
                                     Timer::kSecondMs);
  TestPrepareForBeaconInsertionHelper(keys, kBeaconWithNonce);

  // Verify that if the candidate keys don't change for kHighFreqBeaconCount
  // then we transition into low frequency beaconing.
  keys.insert("c");
  for (int i = 0; i < kHighFreqBeaconCount; ++i) {
    TestPrepareForBeaconInsertionHelper(keys, kBeaconWithNonce);
    factory()->mock_timer()->AdvanceMs(
        options()->beacon_reinstrument_time_sec() * Timer::kSecondMs);
    // Normally the beacon_received field would be updated upon beacon response
    // by UpdateCriticalKeys.
    critical_keys_proto_.set_valid_beacons_received(
        critical_keys_proto_.valid_beacons_received() + 1);
  }
  // Now critical_keys_proto_.valid_beacons_received() == kHighFreqBeaconCount,
  // so after the next call to PrepareForBeaconInsertionHelper the next beacon
  // should occur at the low frequency time.
  TestPrepareForBeaconInsertionHelper(keys, kBeaconWithNonce);
  factory()->mock_timer()->AdvanceMs(options()->beacon_reinstrument_time_sec() *
                                     Timer::kSecondMs);
  TestPrepareForBeaconInsertionHelper(keys, kDoNotBeacon);
  factory()->mock_timer()->AdvanceMs(options()->beacon_reinstrument_time_sec() *
                                     Timer::kSecondMs * kLowFreqBeaconMult);
  TestPrepareForBeaconInsertionHelper(keys, kBeaconWithNonce);

  // And changing the keys again should put us back into high frequency
  // beaconing.
  keys.insert("d");
  TestPrepareForBeaconInsertionHelper(keys, kBeaconWithNonce);
  EXPECT_EQ(0, critical_keys_proto_.valid_beacons_received());
  factory()->mock_timer()->AdvanceMs(options()->beacon_reinstrument_time_sec() *
                                     Timer::kSecondMs);
  TestPrepareForBeaconInsertionHelper(keys, kBeaconWithNonce);
}

// A page with a key that differs on every view keeps a bounded number of
// the keys it had before.
TEST_F(CriticalFinderSupportUtilTest, UpdateCandidateKeysForgetsStaleKeys) {
  for (int i = 0; i < 10 * kMaxStaleCandidateKeys; ++i) {
    StringSet keys;
    keys.insert("fixed");
    keys.insert(StrCat("changing", IntegerToString(i)));
    EXPECT_TRUE(UpdateCandidateKeys(keys, &critical_keys_proto_, true));
    EXPECT_GE(kMaxStaleCandidateKeys + 2,
              critical_keys_proto_.key_evidence_size());
    // The keys on the page are still registered candidates.
    EXPECT_FALSE(UpdateCandidateKeys(keys, &critical_keys_proto_, true));
  }
}

// A page that alternates between a few variants settles on the union of their
// keys instead of looking changed on every view.
TEST_F(CriticalFinderSupportUtilTest, UpdateCandidateKeysKeepsAFewStaleKeys) {
  StringSet variant_a, variant_b;
  variant_a.insert("a");
  variant_b.insert("b");
  EXPECT_TRUE(UpdateCandidateKeys(variant_a, &critical_keys_proto_, true));
  EXPECT_TRUE(UpdateCandidateKeys(variant_b, &critical_keys_proto_, true));
  EXPECT_FALSE(UpdateCandidateKeys(variant_a, &critical_keys_proto_, true));
  EXPECT_FALSE(UpdateCandidateKeys(variant_b, &critical_keys_proto_, true));
}

// The same holds for variants with more keys than kMaxStaleCandidateKeys.
TEST_F(CriticalFinderSupportUtilTest, UpdateCandidateKeysLargeVariantsSettle) {
  StringSet variant_a, variant_b;
  for (int i = 0; i < kMaxStaleCandidateKeys + 50; ++i) {
    variant_a.insert(StrCat("a", IntegerToString(i)));
    variant_b.insert(StrCat("b", IntegerToString(i)));
  }
  EXPECT_TRUE(UpdateCandidateKeys(variant_a, &critical_keys_proto_, true));
  EXPECT_TRUE(UpdateCandidateKeys(variant_b, &critical_keys_proto_, true));
  EXPECT_FALSE(UpdateCandidateKeys(variant_a, &critical_keys_proto_, true));
  EXPECT_FALSE(UpdateCandidateKeys(variant_b, &critical_keys_proto_, true));
}

// Support is only ever removed by decay, also for a key that left the page.
TEST_F(CriticalFinderSupportUtilTest, UpdateCandidateKeysKeepsSupportedKeys) {
  StringSet supported;
  supported.insert("supported");
  EXPECT_TRUE(UpdateCandidateKeys(supported, &critical_keys_proto_, true));
  UpdateCriticalKeys(true /* require_prior_support */, supported, 10,
                     &critical_keys_proto_);
  for (int i = 0; i < 2 * kMaxStaleCandidateKeys; ++i) {
    StringSet keys;
    keys.insert(StrCat("changing", IntegerToString(i)));
    EXPECT_TRUE(UpdateCandidateKeys(keys, &critical_keys_proto_, true));
  }
  StringSet critical_keys;
  GetCriticalKeysFromProto(0 /* support_percentage */, critical_keys_proto_,
                           &critical_keys);
  EXPECT_EQ(supported, critical_keys);
}

// A page keeps a bounded number of pending nonces however often it is
// instrumented; the newest nonce handed out stays valid.
TEST_F(CriticalFinderSupportUtilTest, PendingNonceListIsBounded) {
  StringSet keys;
  keys.insert("a");
  UpdateCandidateKeys(keys, &critical_keys_proto_, true);
  GoogleString first_nonce;
  GoogleString last_nonce;
  for (int i = 0; i < kMaxPendingNonces + 50; ++i) {
    factory()->mock_timer()->AdvanceMs(10);
    critical_keys_proto_.clear_next_beacon_timestamp_ms();
    BeaconMetadata result;
    PrepareForBeaconInsertionHelper(
        &critical_keys_proto_, factory()->nonce_generator(), rewrite_driver(),
        true /* using_candidate_key_detection */, &result);
    ASSERT_EQ(kBeaconWithNonce, result.status);
    if (i == 0) {
      first_nonce = result.nonce;
    }
    last_nonce = result.nonce;
    EXPECT_GE(kMaxPendingNonces, critical_keys_proto_.pending_nonce_size());
  }
  const int64 now_ms = timer()->NowMs();
  EXPECT_TRUE(
      ValidateAndExpireNonce(now_ms, last_nonce, &critical_keys_proto_));
  // The oldest nonces made room for the newer ones.
  EXPECT_FALSE(
      ValidateAndExpireNonce(now_ms, first_nonce, &critical_keys_proto_));
}

}  // namespace

TEST_F(CriticalFinderSupportUtilTest, ValidateAndExpireNonceExactMatchOnly) {
  const int64 now_ms = 1000;
  CriticalKeys keys;
  CriticalKeys::PendingNonce* entry = keys.add_pending_nonce();
  entry->set_timestamp_ms(now_ms);
  entry->set_nonce("nonce-abc");

  // A near-miss nonce (differing content, prefix, or longer) does not
  // validate and leaves the pending entry intact.
  EXPECT_FALSE(ValidateAndExpireNonce(now_ms, "nonce-abd", &keys));
  EXPECT_FALSE(ValidateAndExpireNonce(now_ms, "nonce-ab", &keys));
  EXPECT_FALSE(ValidateAndExpireNonce(now_ms, "nonce-abcd", &keys));
  ASSERT_TRUE(keys.pending_nonce(0).has_nonce());

  // The exact nonce validates and consumes the entry.
  EXPECT_TRUE(ValidateAndExpireNonce(now_ms, "nonce-abc", &keys));
  EXPECT_FALSE(keys.pending_nonce(0).has_nonce());
}

TEST_F(CriticalFinderSupportUtilTest,
       ValidateAndExpireNonceRefusesLongerThanIssued) {
  // A beacon nonce longer than the ones the server hands out is refused before
  // it is compared with any pending entry, so the sender does not choose how
  // much work each comparison does. The server never stores an entry of that
  // length; one is planted here so that the refusal is observable.
  StringSet keys;
  keys.insert("a");
  UpdateCandidateKeys(keys, &critical_keys_proto_, true);
  BeaconMetadata result;
  PrepareForBeaconInsertionHelper(
      &critical_keys_proto_, factory()->nonce_generator(), rewrite_driver(),
      true /* using_candidate_key_detection */, &result);
  ASSERT_EQ(kBeaconWithNonce, result.status);
  const GoogleString issued = result.nonce;
  const GoogleString longer = StrCat(issued, "A");
  const int64 now_ms = timer()->NowMs();

  CriticalKeys::PendingNonce* planted =
      critical_keys_proto_.add_pending_nonce();
  planted->set_timestamp_ms(now_ms);
  planted->set_nonce(longer);
  const int planted_index = critical_keys_proto_.pending_nonce_size() - 1;

  EXPECT_FALSE(ValidateAndExpireNonce(now_ms, longer, &critical_keys_proto_));
  EXPECT_TRUE(critical_keys_proto_.pending_nonce(planted_index).has_nonce());

  // The nonce that was issued still validates and consumes its entry.
  EXPECT_TRUE(ValidateAndExpireNonce(now_ms, issued, &critical_keys_proto_));
  EXPECT_FALSE(ValidateAndExpireNonce(now_ms, issued, &critical_keys_proto_));
}
}  // namespace net_instaweb
