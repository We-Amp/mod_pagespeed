// Copyright 2026 We-Amp B.V.
// Licensed under the Apache License, Version 2.0 (the "License").
//
// Unit tests for the opt-in Web-Bot-Auth counter store: the FNV-1a-64 keyid
// hash (asserted cross-engine compatible with ModPageSpeed 2.0), the lock-free
// slot find-or-claim + overflow (single-threaded and under contention), the
// verify-latency bucketing, the boot-identity minting, and the open/create/
// self-heal round-trip. Mirrors the intent of the 2.0 optimizer line's serve_stats_test.cc v6
// cases.

#include "pagespeed/kernel/webbotauth/webbotauth_counter_store.h"

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#include "gtest/gtest.h"

namespace net_instaweb {
namespace webbotauth {
namespace {

class WebBotAuthCounterStoreTest : public ::testing::Test {
 protected:
  void SetUp() override {
    // A unique temp path per test process; removed in TearDown.
    std::string dir = ::testing::TempDir();
    if (dir.empty()) dir = "/tmp";
    path_ = WebBotAuthCounterPath(dir) + "." +
            std::to_string(
                static_cast<long long>(reinterpret_cast<uintptr_t>(this)));
    std::remove(path_.c_str());
  }
  void TearDown() override { std::remove(path_.c_str()); }

  std::string path_;
};

// The FNV-1a-64 keyid hash MUST match ModPageSpeed 2.0's byte-for-byte so the
// two engines emit hash-compatible documents the same aggregator consumes. The
// probe kid is the aggregator's cross-engine exclusion anchor.
TEST_F(WebBotAuthCounterStoreTest, FnvHashMatchesTwoPointOhProbeAnchor) {
  EXPECT_EQ(HashWebBotAuthKeyid("GBxqWscB-GOkwNXEhmWQA4SY2gWRH2Hz4dAvzRcK1gw"),
            0xd7a140a27d88bea6ULL);
}

TEST_F(WebBotAuthCounterStoreTest, HashKeyidNeverZeroAndStable) {
  EXPECT_EQ(HashWebBotAuthKeyid("kid-A"), HashWebBotAuthKeyid("kid-A"));
  EXPECT_NE(HashWebBotAuthKeyid("kid-A"), HashWebBotAuthKeyid("kid-B"));
  // No input should hash to the empty-slot sentinel 0.
  EXPECT_NE(HashWebBotAuthKeyid(""), 0u);
}

// A fresh create mints a non-zero, RFC-4122-v4 boot_id and a plausible
// counting-since day, with every counter at zero.
TEST_F(WebBotAuthCounterStoreTest, FreshCreateMintsIdentityAndZeroCounters) {
  WebBotAuthCounterStats* stats = OpenOrCreateWebBotAuthCounter(path_);
  ASSERT_NE(stats, nullptr);
  EXPECT_EQ(stats->magic, WebBotAuthCounterStats::kMagic);
  EXPECT_EQ(stats->version, WebBotAuthCounterStats::kVersion);

  bool any_nonzero = false;
  for (uint8_t b : stats->boot_id) {
    if (b != 0) any_nonzero = true;
  }
  EXPECT_TRUE(any_nonzero) << "boot_id must be randomly minted";
  EXPECT_EQ(stats->boot_id[6] & 0xF0, 0x40);
  EXPECT_EQ(stats->boot_id[8] & 0xC0, 0x80);
  // Counting-since is a day count well past 2020 (18262 = 2020-01-01).
  EXPECT_GT(stats->counting_since_unix_day, 18262u);

  EXPECT_EQ(stats->verified_total, 0u);
  EXPECT_EQ(stats->invalid_total, 0u);
  EXPECT_EQ(stats->other_total, 0u);
  EXPECT_EQ(stats->other_verified_bots, 0u);
  EXPECT_EQ(stats->verify_latency_lt100us, 0u);
  EXPECT_EQ(stats->verify_latency_ge10ms, 0u);
  for (const auto& slot : stats->signers) {
    EXPECT_EQ(slot.kid_hash, 0u);
    EXPECT_EQ(slot.count, 0u);
  }
  CloseWebBotAuthCounter(stats);
}

TEST_F(WebBotAuthCounterStoreTest, OpenNonexistentReturnsNull) {
  EXPECT_EQ(nullptr, OpenWebBotAuthCounter(path_));
}

TEST_F(WebBotAuthCounterStoreTest, OpenWrongMagicRejected) {
  std::string bad(WebBotAuthCounterStats::kFileSize, '\0');
  uint32_t magic = 0xdeadbeef;
  std::memcpy(&bad[0], &magic, sizeof(magic));
  FILE* f = fopen(path_.c_str(), "wb");
  ASSERT_NE(f, nullptr);
  ASSERT_EQ(bad.size(), fwrite(bad.data(), 1, bad.size(), f));
  fclose(f);
  EXPECT_EQ(nullptr, OpenWebBotAuthCounter(path_));
}

TEST_F(WebBotAuthCounterStoreTest, OpenTooSmallRejected) {
  std::string tiny(16, '\0');
  FILE* f = fopen(path_.c_str(), "wb");
  ASSERT_NE(f, nullptr);
  ASSERT_EQ(tiny.size(), fwrite(tiny.data(), 1, tiny.size(), f));
  fclose(f);
  EXPECT_EQ(nullptr, OpenWebBotAuthCounter(path_));
}

// Counters + identity survive a plain reopen; latency lands in the right bucket.
TEST_F(WebBotAuthCounterStoreTest, RoundTripPersistsCountersAndIdentity) {
  WebBotAuthCounterStats* stats = OpenOrCreateWebBotAuthCounter(path_);
  ASSERT_NE(stats, nullptr);
  RecordWebBotAuthSigned(stats, true);
  RecordWebBotAuthSigned(stats, false);
  RecordWebBotAuthOtherSignature(stats);
  RecordWebBotAuthVerifiedSigner(stats, "kid-A", /*latency_us=*/50);
  RecordWebBotAuthVerifiedSigner(stats, "kid-A", /*latency_us=*/5000);
  RecordWebBotAuthVerifiedSigner(stats, "kid-B", /*latency_us=*/20000);
  uint8_t boot_copy[16];
  std::memcpy(boot_copy, stats->boot_id, 16);
  uint64_t since = stats->counting_since_unix_day;
  CloseWebBotAuthCounter(stats);

  WebBotAuthCounterStats* re = OpenWebBotAuthCounter(path_);
  ASSERT_NE(re, nullptr);
  EXPECT_EQ(0, std::memcmp(re->boot_id, boot_copy, 16));
  EXPECT_EQ(re->counting_since_unix_day, since);
  EXPECT_EQ(re->verified_total, 1u);
  EXPECT_EQ(re->invalid_total, 1u);
  EXPECT_EQ(re->other_total, 1u);
  uint64_t total = 0;
  int used = 0;
  for (const auto& slot : re->signers) {
    if (slot.kid_hash != 0) {
      ++used;
      total += slot.count;
    }
  }
  EXPECT_EQ(used, 2);  // kid-A, kid-B
  EXPECT_EQ(total, 3u);
  EXPECT_EQ(re->verify_latency_lt100us, 1u);  // 50us
  EXPECT_EQ(re->verify_latency_lt1ms, 0u);
  EXPECT_EQ(re->verify_latency_lt10ms, 1u);  // 5ms
  EXPECT_EQ(re->verify_latency_ge10ms, 1u);  // 20ms
  CloseWebBotAuthCounter(re);
}

// OpenOrCreate on a valid existing file PRESERVES its identity AND counters
// (they persist across reopen; unlike 2.0 they are not zeroed on reuse).
TEST_F(WebBotAuthCounterStoreTest, OpenOrCreateReusePreservesCounters) {
  WebBotAuthCounterStats* stats = OpenOrCreateWebBotAuthCounter(path_);
  ASSERT_NE(stats, nullptr);
  RecordWebBotAuthVerifiedSigner(stats, "kid-A", /*latency_us=*/50);
  uint8_t boot_copy[16];
  std::memcpy(boot_copy, stats->boot_id, 16);
  CloseWebBotAuthCounter(stats);

  WebBotAuthCounterStats* reused = OpenOrCreateWebBotAuthCounter(path_);
  ASSERT_NE(reused, nullptr);
  EXPECT_EQ(0, std::memcmp(reused->boot_id, boot_copy, 16));
  EXPECT_EQ(reused->verified_total, 0u);  // RecordSigned not called above
  uint64_t total = 0;
  for (const auto& slot : reused->signers) total += slot.count;
  EXPECT_EQ(total, 1u);  // the earlier kid-A count survives
  CloseWebBotAuthCounter(reused);
}

// A present-but-invalid file (right magic, WRONG version -- e.g. after a version
// bump) must be self-healed by OpenOrCreate: unlinked and recreated fresh, so
// recording works again rather than becoming a silent no-op forever.
TEST_F(WebBotAuthCounterStoreTest, OpenOrCreateSelfHealsWrongVersionFile) {
  std::string bad(WebBotAuthCounterStats::kFileSize, '\0');
  uint32_t magic = WebBotAuthCounterStats::kMagic;
  uint32_t version = WebBotAuthCounterStats::kVersion + 99;  // future/unknown
  std::memcpy(&bad[0], &magic, sizeof(magic));
  std::memcpy(&bad[4], &version, sizeof(version));
  FILE* f = fopen(path_.c_str(), "wb");
  ASSERT_NE(f, nullptr);
  ASSERT_EQ(bad.size(), fwrite(bad.data(), 1, bad.size(), f));
  fclose(f);

  // Open fails closed on the version mismatch...
  EXPECT_EQ(nullptr, OpenWebBotAuthCounter(path_));
  // ...but OpenOrCreate self-heals to a fresh, current-version file.
  WebBotAuthCounterStats* fresh = OpenOrCreateWebBotAuthCounter(path_);
  ASSERT_NE(fresh, nullptr);
  EXPECT_EQ(fresh->version, WebBotAuthCounterStats::kVersion);
  EXPECT_GT(fresh->counting_since_unix_day, 18262u);   // freshly minted
  RecordWebBotAuthVerifiedSigner(fresh, "kid-A", 10);  // recording works again
  uint64_t total = 0;
  for (const auto& slot : fresh->signers) total += slot.count;
  EXPECT_EQ(total, 1u);
  CloseWebBotAuthCounter(fresh);
}

// boot_id regenerates on a genuine create-fresh (file deleted between opens).
TEST_F(WebBotAuthCounterStoreTest, BootIdRegeneratesOnCreateFresh) {
  WebBotAuthCounterStats* a = OpenOrCreateWebBotAuthCounter(path_);
  ASSERT_NE(a, nullptr);
  uint8_t boot_a[16];
  std::memcpy(boot_a, a->boot_id, 16);
  CloseWebBotAuthCounter(a);
  std::remove(path_.c_str());

  WebBotAuthCounterStats* b = OpenOrCreateWebBotAuthCounter(path_);
  ASSERT_NE(b, nullptr);
  EXPECT_NE(0, std::memcmp(b->boot_id, boot_a, 16));  // 128 random bits
  CloseWebBotAuthCounter(b);
}

// Slot find-or-claim: same keyid -> same slot; distinct keyids -> distinct
// slots; the 9th distinct keyid overflows into other_verified_bots.
TEST_F(WebBotAuthCounterStoreTest, SlotClaimAndOverflow) {
  WebBotAuthCounterStats* stats = OpenOrCreateWebBotAuthCounter(path_);
  ASSERT_NE(stats, nullptr);

  for (int rep = 0; rep < 2; ++rep) {
    for (int i = 0;
         i < static_cast<int>(WebBotAuthCounterStats::kMaxCountedBots); ++i) {
      RecordWebBotAuthVerifiedSigner(stats, "kid-" + std::to_string(i), 10);
    }
  }
  int used = 0;
  for (const auto& slot : stats->signers) {
    if (slot.kid_hash != 0) {
      ++used;
      EXPECT_EQ(slot.count, 2u);
    }
  }
  EXPECT_EQ(used, static_cast<int>(WebBotAuthCounterStats::kMaxCountedBots));
  EXPECT_EQ(stats->other_verified_bots, 0u);

  RecordWebBotAuthVerifiedSigner(stats, "kid-overflow", 10);
  RecordWebBotAuthVerifiedSigner(stats, "kid-overflow-2", 10);
  EXPECT_EQ(stats->other_verified_bots, 2u);
  CloseWebBotAuthCounter(stats);
}

TEST_F(WebBotAuthCounterStoreTest, NullStatsNoCrash) {
  RecordWebBotAuthSigned(nullptr, true);
  RecordWebBotAuthOtherSignature(nullptr);
  RecordWebBotAuthVerifiedSigner(nullptr, "kid", 100);
  CloseWebBotAuthCounter(nullptr);
  SUCCEED();
}

// Concurrency: many threads hammer the lock-free slot claim with a keyid
// alphabet larger than kMaxCountedBots (forcing the overflow path AND the
// CAS-loss branch). The conservation invariant must hold EXACTLY:
//   Sum(slot.count) + other_verified_bots == total number of Record calls.
// TSan-friendly: every shared access goes through std::atomic_ref.
TEST_F(WebBotAuthCounterStoreTest, SlotClaimIsRaceFreeUnderContention) {
  WebBotAuthCounterStats* stats = OpenOrCreateWebBotAuthCounter(path_);
  ASSERT_NE(stats, nullptr);

  constexpr int kThreads = 8;
  constexpr int kPerThread = 20000;
  constexpr int kDistinctKids = 12;  // > kMaxCountedBots (8) to force overflow
  static_assert(
      kDistinctKids > static_cast<int>(WebBotAuthCounterStats::kMaxCountedBots),
      "alphabet must exceed slot count to exercise overflow");

  std::vector<std::thread> threads;
  threads.reserve(kThreads);
  for (int t = 0; t < kThreads; ++t) {
    threads.emplace_back([stats, t]() {
      for (int i = 0; i < kPerThread; ++i) {
        std::string kid = "kid-" + std::to_string((i + t) % kDistinctKids);
        RecordWebBotAuthVerifiedSigner(stats, kid, /*latency_us=*/(i % 20000));
      }
    });
  }
  for (auto& th : threads) th.join();

  uint64_t slot_total = 0;
  int used = 0;
  for (const auto& slot : stats->signers) {
    if (slot.kid_hash != 0) {
      ++used;
      slot_total += slot.count;
    }
  }
  const uint64_t grand_total = slot_total + stats->other_verified_bots;
  EXPECT_EQ(grand_total, static_cast<uint64_t>(kThreads) * kPerThread);
  EXPECT_EQ(used, static_cast<int>(WebBotAuthCounterStats::kMaxCountedBots));
  EXPECT_GT(stats->other_verified_bots, 0u);

  const uint64_t lat_total =
      stats->verify_latency_lt100us + stats->verify_latency_lt1ms +
      stats->verify_latency_lt10ms + stats->verify_latency_ge10ms;
  EXPECT_EQ(lat_total, static_cast<uint64_t>(kThreads) * kPerThread);
  CloseWebBotAuthCounter(stats);
}

}  // namespace
}  // namespace webbotauth
}  // namespace net_instaweb
