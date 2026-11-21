// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

// Tests for ServeStats shared mmap create/open/validate/atomic increment.

#include "src/worker/serve_stats.h"

#include <algorithm>
#include <atomic>
#include <cstdarg>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <iterator>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include "gtest/gtest.h"
#include "lib/base/message_handler.h"
#include "lib/classify/content_type.h"

namespace pagespeed {
namespace {

class ServeStatsTest : public ::testing::Test {
 protected:
  void SetUp() override {
    path_ = std::string(::testing::TempDir()) + "/test-serve-stats";
    // Clean up any leftover file.
    std::remove(path_.c_str());
  }

  void TearDown() override { std::remove(path_.c_str()); }

  std::string path_;
};

TEST_F(ServeStatsTest, CreateAndOpen) {
  ServeStats* created = CreateServeStats(path_);
  ASSERT_NE(created, nullptr);
  EXPECT_EQ(created->magic, ServeStats::kMagic);
  EXPECT_EQ(created->version, ServeStats::kVersion);
  EXPECT_EQ(created->html_original_bytes, 0u);
  EXPECT_EQ(created->image_optimized_hits, 0u);
  CloseServeStats(created);

  // Re-open.
  ServeStats* opened = OpenServeStats(path_);
  ASSERT_NE(opened, nullptr);
  EXPECT_EQ(opened->magic, ServeStats::kMagic);
  EXPECT_EQ(opened->version, ServeStats::kVersion);
  CloseServeStats(opened);
}

TEST_F(ServeStatsTest, OpenNonexistent) {
  ServeStats* stats = OpenServeStats("/tmp/nonexistent-serve-stats-xyz");
  EXPECT_EQ(stats, nullptr);
}

TEST_F(ServeStatsTest, OpenWrongMagic) {
  ServeStats* created = CreateServeStats(path_);
  ASSERT_NE(created, nullptr);
  created->magic = 0xBADBAD;
  CloseServeStats(created);

  // Open should fail because magic doesn't match.
  ServeStats* opened = OpenServeStats(path_);
  EXPECT_EQ(opened, nullptr);
}

TEST_F(ServeStatsTest, OpenWrongVersion) {
  ServeStats* created = CreateServeStats(path_);
  ASSERT_NE(created, nullptr);
  created->version = 99;
  CloseServeStats(created);

  ServeStats* opened = OpenServeStats(path_);
  EXPECT_EQ(opened, nullptr);
}

TEST_F(ServeStatsTest, CountersPersistAcrossReopen) {
  ServeStats* stats = CreateServeStats(path_);
  ASSERT_NE(stats, nullptr);

  stats->html_original_bytes = 1000;
  stats->html_optimized_bytes = 700;
  stats->html_optimized_hits = 5;
  stats->image_original_bytes = 50000;
  stats->image_optimized_bytes = 10000;
  stats->image_optimized_hits = 20;
  CloseServeStats(stats);

  ServeStats* reopened = OpenServeStats(path_);
  ASSERT_NE(reopened, nullptr);
  EXPECT_EQ(reopened->html_original_bytes, 1000u);
  EXPECT_EQ(reopened->html_optimized_bytes, 700u);
  EXPECT_EQ(reopened->html_optimized_hits, 5u);
  EXPECT_EQ(reopened->image_original_bytes, 50000u);
  EXPECT_EQ(reopened->image_optimized_bytes, 10000u);
  EXPECT_EQ(reopened->image_optimized_hits, 20u);
  CloseServeStats(reopened);
}

// CreateServeStats on an existing valid file must reuse the inode (so nginx's
// mmap stays valid) and zero every counter, preserving magic+version. The
// zeroing is done with atomic stores (matching concurrent nginx increments),
// not a racy memset.
TEST_F(ServeStatsTest, CreateReuseZeroesAllCounters) {
  ServeStats* stats = CreateServeStats(path_);
  ASSERT_NE(stats, nullptr);
  stats->html_original_bytes = 1000;
  stats->html_optimized_bytes = 700;
  stats->css_original_bytes = 11;
  stats->css_optimized_bytes = 22;
  stats->js_original_bytes = 33;
  stats->js_optimized_bytes = 44;
  stats->image_original_bytes = 50000;
  stats->image_optimized_bytes = 10000;
  stats->html_optimized_hits = 5;
  stats->css_optimized_hits = 6;
  stats->js_optimized_hits = 7;
  stats->image_optimized_hits = 20;
  stats->svg_optimized_hits = 8;
  stats->webbotauth_signed_verified = 9;
  stats->webbotauth_signed_invalid = 10;
  stats->zerocopy_torn_aborts = 11;
  stats->zerocopy_proactive_copyouts = 12;
  stats->zerocopy_copy_then_verify_discards = 13;
  stats->swr_coalesced_serves = 14;
  stats->stale_if_error_serves = 15;
  stats->headers_sidecar_fall_through = 16;
  stats->headers_sidecar_never_optimized = 17;
  stats->headers_sidecar_replayed = 18;
  stats->headers_sidecar_missing = 19;
  stats->headers_sidecar_write_failed = 20;
  stats->headers_sidecar_stored = 21;
  CloseServeStats(stats);

  ServeStats* reused = CreateServeStats(path_);
  ASSERT_NE(reused, nullptr);
  EXPECT_EQ(reused->magic, ServeStats::kMagic);
  EXPECT_EQ(reused->version, ServeStats::kVersion);
  for (uint64_t value : {reused->html_original_bytes,
                         reused->html_optimized_bytes,
                         reused->css_original_bytes,
                         reused->css_optimized_bytes,
                         reused->js_original_bytes,
                         reused->js_optimized_bytes,
                         reused->image_original_bytes,
                         reused->image_optimized_bytes,
                         reused->html_optimized_hits,
                         reused->css_optimized_hits,
                         reused->js_optimized_hits,
                         reused->image_optimized_hits,
                         reused->svg_optimized_hits,
                         reused->webbotauth_signed_verified,
                         reused->webbotauth_signed_invalid,
                         reused->zerocopy_torn_aborts,
                         reused->zerocopy_proactive_copyouts,
                         reused->zerocopy_copy_then_verify_discards,
                         reused->swr_coalesced_serves,
                         reused->stale_if_error_serves,
                         reused->headers_sidecar_fall_through,
                         reused->headers_sidecar_never_optimized,
                         reused->headers_sidecar_replayed,
                         reused->headers_sidecar_missing,
                         reused->headers_sidecar_write_failed,
                         reused->headers_sidecar_stored}) {
    EXPECT_EQ(value, 0u);
  }
  CloseServeStats(reused);
}

// Zero-copy serve-barrier + bounded-stale counters (v7): the Record*
// helpers increment the shared mmap fields and the values persist across a
// reopen. Also covers the nullptr no-op contract.
TEST_F(ServeStatsTest, ZerocopyAndSwrCountersRecordAndPersist) {
  ServeStats* stats = CreateServeStats(path_);
  ASSERT_NE(stats, nullptr);
  EXPECT_EQ(stats->zerocopy_torn_aborts, 0u);

  RecordZerocopyTornAbort(stats);
  RecordZerocopyTornAbort(stats);
  RecordZerocopyProactiveCopyout(stats);
  RecordZerocopyCopyThenVerifyDiscard(stats);
  RecordSwrCoalescedServe(stats);
  RecordStaleIfErrorServe(stats);
  RecordStaleIfErrorServe(stats);

  EXPECT_EQ(stats->zerocopy_torn_aborts, 2u);
  EXPECT_EQ(stats->zerocopy_proactive_copyouts, 1u);
  EXPECT_EQ(stats->zerocopy_copy_then_verify_discards, 1u);
  EXPECT_EQ(stats->swr_coalesced_serves, 1u);
  EXPECT_EQ(stats->stale_if_error_serves, 2u);
  CloseServeStats(stats);

  // Values survive a reopen (they live in the shared mmap).
  ServeStats* reopened = OpenServeStats(path_);
  ASSERT_NE(reopened, nullptr);
  EXPECT_EQ(reopened->zerocopy_torn_aborts, 2u);
  EXPECT_EQ(reopened->zerocopy_proactive_copyouts, 1u);
  EXPECT_EQ(reopened->zerocopy_copy_then_verify_discards, 1u);
  EXPECT_EQ(reopened->swr_coalesced_serves, 1u);
  EXPECT_EQ(reopened->stale_if_error_serves, 2u);
  CloseServeStats(reopened);
}

TEST_F(ServeStatsTest, ZerocopyAndSwrRecordNullSafe) {
  // nullptr is a no-op (front-end may not have mapped the file yet).
  RecordZerocopyTornAbort(nullptr);
  RecordZerocopyProactiveCopyout(nullptr);
  RecordZerocopyCopyThenVerifyDiscard(nullptr);
  RecordSwrCoalescedServe(nullptr);
  RecordStaleIfErrorServe(nullptr);
}

// v11 response-header sidecar counters: each Record* helper increments its
// own field, the values persist across a reopen, and nullptr is a no-op.
TEST_F(ServeStatsTest, HeadersSidecarCountersRecordAndPersist) {
  ServeStats* stats = CreateServeStats(path_);
  ASSERT_NE(stats, nullptr);
  RecordHeadersSidecarFallThrough(stats);
  RecordHeadersSidecarFallThrough(stats);
  RecordHeadersSidecarNeverOptimized(stats);
  RecordHeadersSidecarReplayed(stats);
  RecordHeadersSidecarReplayed(stats);
  RecordHeadersSidecarReplayed(stats);
  RecordHeadersSidecarMissing(stats);
  RecordHeadersSidecarWriteFailed(stats);
  RecordHeadersSidecarStored(stats);
  RecordHeadersSidecarStored(stats);
  CloseServeStats(stats);

  ServeStats* reopened = OpenServeStats(path_);
  ASSERT_NE(reopened, nullptr);
  EXPECT_EQ(reopened->headers_sidecar_fall_through, 2u);
  EXPECT_EQ(reopened->headers_sidecar_never_optimized, 1u);
  EXPECT_EQ(reopened->headers_sidecar_replayed, 3u);
  EXPECT_EQ(reopened->headers_sidecar_missing, 1u);
  EXPECT_EQ(reopened->headers_sidecar_write_failed, 1u);
  EXPECT_EQ(reopened->headers_sidecar_stored, 2u);
  CloseServeStats(reopened);

  RecordHeadersSidecarFallThrough(nullptr);
  RecordHeadersSidecarNeverOptimized(nullptr);
  RecordHeadersSidecarReplayed(nullptr);
  RecordHeadersSidecarMissing(nullptr);
  RecordHeadersSidecarWriteFailed(nullptr);
  RecordHeadersSidecarStored(nullptr);
}

// The busy-read counters: RecordCacheBusyRead always counts the read, counts
// the retry hit only when told, persists across a reopen, is zeroed on
// same-version reuse, and is a no-op on nullptr.
TEST_F(ServeStatsTest, CacheBusyReadCountersRecordPersistAndReset) {
  ServeStats* stats = CreateServeStats(path_);
  ASSERT_NE(stats, nullptr);
  RecordCacheBusyRead(stats, /*retry_hit=*/true);
  RecordCacheBusyRead(stats, /*retry_hit=*/true);
  RecordCacheBusyRead(stats, /*retry_hit=*/false);
  CloseServeStats(stats);

  ServeStats* reopened = OpenServeStats(path_);
  ASSERT_NE(reopened, nullptr);
  EXPECT_EQ(reopened->cache_busy_reads, 3u);
  EXPECT_EQ(reopened->cache_busy_retry_hits, 2u);
  CloseServeStats(reopened);

  stats = CreateServeStats(path_);
  ASSERT_NE(stats, nullptr);
  EXPECT_EQ(stats->cache_busy_reads, 0u);
  EXPECT_EQ(stats->cache_busy_retry_hits, 0u);
  CloseServeStats(stats);

  RecordCacheBusyRead(nullptr, true);
}

// The v11 block sits after the v10 block and before the identity/epoch pair.
TEST_F(ServeStatsTest, V11LayoutOrdering) {
  EXPECT_LT(offsetof(ServeStats, serve_hosts_other_optimized_bytes),
            offsetof(ServeStats, headers_sidecar_fall_through));
  EXPECT_LT(offsetof(ServeStats, headers_sidecar_stored),
            offsetof(ServeStats, counting_since_unix_day));
  EXPECT_EQ(offsetof(ServeStats, headers_sidecar_fall_through) % 8, 0u);
}

TEST_F(ServeStatsTest, SplitBlockFollowsTheIdentityPairAndMovesNothing) {
  // The offsets a peer built before the split block knows.
  EXPECT_EQ(offsetof(ServeStats, boot_id), 11480u);
  EXPECT_EQ(offsetof(ServeStats, split_html), 11496u);
  // The busy-read block follows the split block, moving nothing either.
  EXPECT_EQ(offsetof(ServeStats, cache_busy_reads), 11592u);
  EXPECT_EQ(sizeof(ServeStats), 11608u);
  EXPECT_LE(sizeof(ServeStats), ServeStats::kFileSize);
}

TEST_F(ServeStatsTest, SplitCountsOnlyServesWithAnEncodedSize) {
  ServeStats* stats = CreateServeStats(path_);
  ASSERT_NE(stats, nullptr);
  constexpr uint32_t kGzipMask = 1u << 6;
  // 10000 raw, 3000 as gzip, 2400 served: encoding 7000, rewrite 600.
  RecordServeHitSplit(stats, ContentType::kCss, 10000, 3000, 2400, kGzipMask,
                      "a.example");
  // Identity: nothing for the encoding to save.
  RecordServeHitSplit(stats, ContentType::kCss, 8000, 8000, 5000, 0,
                      "a.example");
  // Served bytes above the encoded origin: the rewrite part is negative.
  RecordServeHitSplit(stats, ContentType::kHtml, 6000, 1500, 2100, kGzipMask,
                      "");
  // No encoded size known: totals only.
  RecordServeHit(stats, ContentType::kCss, 4000, 1000, kGzipMask);
  RecordServeHitSplit(stats, ContentType::kOther, 1, 1, 1, 0, "");

  const ServeStats::SplitCounters css =
      ReadServeSplit(stats, ContentType::kCss);
  EXPECT_EQ(css.original_bytes, 18000u);
  EXPECT_EQ(css.compressed_original_bytes, 11000u);
  EXPECT_EQ(css.optimized_bytes, 7400u);
  EXPECT_EQ(stats->css_original_bytes, 22000u);
  EXPECT_EQ(stats->css_optimized_hits, 3u);
  // Encoding saved: 18000 - 11000 = 7000.  Rewrite saved: 11000 - 7400 = 3600.
  EXPECT_EQ(css.original_bytes - css.compressed_original_bytes, 7000u);
  EXPECT_EQ(css.compressed_original_bytes - css.optimized_bytes, 3600u);

  const ServeStats::SplitCounters html =
      ReadServeSplit(stats, ContentType::kHtml);
  EXPECT_EQ(html.compressed_original_bytes, 1500u);
  EXPECT_LT(static_cast<int64_t>(html.compressed_original_bytes),
            static_cast<int64_t>(html.optimized_bytes));
  EXPECT_EQ(ReadServeSplit(stats, ContentType::kOther).original_bytes, 0u);
  EXPECT_EQ(ReadServeSplit(nullptr, ContentType::kCss).original_bytes, 0u);
  RecordServeHitSplit(nullptr, ContentType::kCss, 1, 1, 1, 0, "");
  CloseServeStats(stats);
}

// A writer that never records the split (a front end built before it) leaves
// the block at zero while the totals grow: coverage reads 0, not a measured 0.
TEST_F(ServeStatsTest, ServesRecordedWithoutTheSplitLeaveItAtZero) {
  ServeStats* stats = CreateServeStats(path_);
  ASSERT_NE(stats, nullptr);
  RecordServeHit(stats, ContentType::kCss, 9000, 3000, 1u << 6);
  RecordServeHit(stats, ContentType::kImage, 9000, 3000, 0);
  EXPECT_EQ(stats->css_original_bytes, 9000u);
  for (const ContentType type : {ContentType::kHtml, ContentType::kCss,
                                 ContentType::kJs, ContentType::kImage}) {
    const ServeStats::SplitCounters s = ReadServeSplit(stats, type);
    EXPECT_EQ(s.original_bytes, 0u);
    EXPECT_EQ(s.compressed_original_bytes, 0u);
    EXPECT_EQ(s.optimized_bytes, 0u);
  }
  CloseServeStats(stats);
}

TEST_F(ServeStatsTest, SplitBlockIsZeroedWithTheCountersOnReuse) {
  ServeStats* stats = CreateServeStats(path_);
  ASSERT_NE(stats, nullptr);
  RecordServeHitSplit(stats, ContentType::kJs, 900, 400, 300, 0, "");
  RecordServeHitSplit(stats, ContentType::kImage, 900, 900, 300, 0, "");
  EXPECT_EQ(ReadServeSplit(stats, ContentType::kJs).original_bytes, 900u);
  CloseServeStats(stats);
  // Same-version reuse zeroes every counter, the split block included.
  stats = CreateServeStats(path_);
  ASSERT_NE(stats, nullptr);
  for (const ContentType type : {ContentType::kHtml, ContentType::kCss,
                                 ContentType::kJs, ContentType::kImage}) {
    const ServeStats::SplitCounters s = ReadServeSplit(stats, type);
    EXPECT_EQ(s.original_bytes, 0u);
    EXPECT_EQ(s.compressed_original_bytes, 0u);
    EXPECT_EQ(s.optimized_bytes, 0u);
  }
  CloseServeStats(stats);
}

TEST_F(ServeStatsTest, AtomicIncrement) {
  ServeStats* stats = CreateServeStats(path_);
  ASSERT_NE(stats, nullptr);

  // Simulate what nginx does: atomic fetch-add on mmap'd plain integers.
  std::atomic_ref(stats->css_original_bytes)
      .fetch_add(2000, std::memory_order_relaxed);
  std::atomic_ref(stats->css_optimized_bytes)
      .fetch_add(1500, std::memory_order_relaxed);
  std::atomic_ref(stats->css_optimized_hits)
      .fetch_add(1, std::memory_order_relaxed);
  std::atomic_ref(stats->css_original_bytes)
      .fetch_add(3000, std::memory_order_relaxed);
  std::atomic_ref(stats->css_optimized_bytes)
      .fetch_add(2200, std::memory_order_relaxed);
  std::atomic_ref(stats->css_optimized_hits)
      .fetch_add(1, std::memory_order_relaxed);

  // Simulate what the worker reads: atomic load.
  uint64_t orig = std::atomic_ref(stats->css_original_bytes)
                      .load(std::memory_order_relaxed);
  uint64_t opt = std::atomic_ref(stats->css_optimized_bytes)
                     .load(std::memory_order_relaxed);
  uint64_t hits = std::atomic_ref(stats->css_optimized_hits)
                      .load(std::memory_order_relaxed);

  EXPECT_EQ(orig, 5000u);
  EXPECT_EQ(opt, 3700u);
  EXPECT_EQ(hits, 2u);

  CloseServeStats(stats);
}

TEST_F(ServeStatsTest, AllCounterFields) {
  ServeStats* stats = CreateServeStats(path_);
  ASSERT_NE(stats, nullptr);

  // Verify all counter fields are independently writable and readable.
  stats->html_original_bytes = 1;
  stats->html_optimized_bytes = 2;
  stats->css_original_bytes = 3;
  stats->css_optimized_bytes = 4;
  stats->js_original_bytes = 5;
  stats->js_optimized_bytes = 6;
  stats->image_original_bytes = 7;
  stats->image_optimized_bytes = 8;
  stats->html_optimized_hits = 9;
  stats->css_optimized_hits = 10;
  stats->js_optimized_hits = 11;
  stats->image_optimized_hits = 12;

  EXPECT_EQ(stats->html_original_bytes, 1u);
  EXPECT_EQ(stats->html_optimized_bytes, 2u);
  EXPECT_EQ(stats->css_original_bytes, 3u);
  EXPECT_EQ(stats->css_optimized_bytes, 4u);
  EXPECT_EQ(stats->js_original_bytes, 5u);
  EXPECT_EQ(stats->js_optimized_bytes, 6u);
  EXPECT_EQ(stats->image_original_bytes, 7u);
  EXPECT_EQ(stats->image_optimized_bytes, 8u);
  EXPECT_EQ(stats->html_optimized_hits, 9u);
  EXPECT_EQ(stats->css_optimized_hits, 10u);
  EXPECT_EQ(stats->js_optimized_hits, 11u);
  EXPECT_EQ(stats->image_optimized_hits, 12u);

  CloseServeStats(stats);
}

// Observe-only telemetry: RecordWebBotAuthSigned splits signed
// requests into verified vs invalid and never touches any other counter.
TEST_F(ServeStatsTest, RecordWebBotAuthSignedSplitsByResult) {
  ServeStats* stats = CreateServeStats(path_);
  ASSERT_NE(stats, nullptr);

  RecordWebBotAuthSigned(stats, /*verified=*/true);
  RecordWebBotAuthSigned(stats, /*verified=*/true);
  RecordWebBotAuthSigned(stats, /*verified=*/false);

  EXPECT_EQ(stats->webbotauth_signed_verified, 2u);
  EXPECT_EQ(stats->webbotauth_signed_invalid, 1u);
  // No bleed into the serve-hit counters.
  EXPECT_EQ(stats->html_optimized_hits, 0u);
  EXPECT_EQ(stats->image_optimized_hits, 0u);
  CloseServeStats(stats);
}

TEST_F(ServeStatsTest, RecordWebBotAuthSignedNullStatsNoCrash) {
  RecordWebBotAuthSigned(nullptr, true);
  RecordWebBotAuthSigned(nullptr, false);
}

// Non-web-bot-auth signature material (no member tagged "web-bot-auth") is
// counted separately — never as verified or invalid.
TEST_F(ServeStatsTest, RecordWebBotAuthOtherSignatureCountsSeparately) {
  ServeStats* stats = CreateServeStats(path_);
  ASSERT_NE(stats, nullptr);

  RecordWebBotAuthOtherSignature(stats);
  RecordWebBotAuthOtherSignature(stats);

  EXPECT_EQ(stats->webbotauth_other_signature, 2u);
  EXPECT_EQ(stats->webbotauth_signed_verified, 0u);
  EXPECT_EQ(stats->webbotauth_signed_invalid, 0u);
  CloseServeStats(stats);
}

TEST_F(ServeStatsTest, RecordWebBotAuthOtherSignatureNullStatsNoCrash) {
  RecordWebBotAuthOtherSignature(nullptr);
}

// The real v3 -> v4 migration artifact: a legacy 128-byte file with correct
// magic and version=3 on disk. Open must reject it (the SIZE gate fires --
// 128 < kFileSize -- before the version check even runs) and Create must
// unlink + recreate a fresh v4 file at the new size with zeroed counters.
TEST_F(ServeStatsTest, LegacyV3FileIsRejectedAndRecreated) {
  std::string legacy(128, '\0');
  uint32_t magic = ServeStats::kMagic;
  uint32_t version = 3;
  std::memcpy(&legacy[0], &magic, sizeof(magic));
  std::memcpy(&legacy[4], &version, sizeof(version));
  legacy[16] = 0x2A;  // non-zero counter junk that must NOT survive
  FILE* f = fopen(path_.c_str(), "wb");
  ASSERT_NE(f, nullptr);
  ASSERT_EQ(legacy.size(), fwrite(legacy.data(), 1, legacy.size(), f));
  fclose(f);

  // The nginx-side open path fails closed on the legacy file.
  EXPECT_EQ(nullptr, OpenServeStats(path_));

  // The worker-side create path self-heals: unlink + fresh v4 file.
  ServeStats* fresh = CreateServeStats(path_);
  ASSERT_NE(fresh, nullptr);
  EXPECT_EQ(fresh->magic, ServeStats::kMagic);
  EXPECT_EQ(fresh->version, ServeStats::kVersion);
  EXPECT_EQ(fresh->html_original_bytes, 0u);
  EXPECT_EQ(fresh->webbotauth_other_signature, 0u);
  CloseServeStats(fresh);

  // On-disk size is the new kFileSize, so a subsequent open succeeds.
  ServeStats* reopened = OpenServeStats(path_);
  ASSERT_NE(reopened, nullptr);
  EXPECT_EQ(reopened->version, ServeStats::kVersion);
  CloseServeStats(reopened);
}

TEST_F(ServeStatsTest, CreateTruncatesExisting) {
  // Create, write some data, close.
  ServeStats* stats = CreateServeStats(path_);
  ASSERT_NE(stats, nullptr);
  stats->html_original_bytes = 99999;
  CloseServeStats(stats);

  // Create again — should truncate and zero.
  ServeStats* fresh = CreateServeStats(path_);
  ASSERT_NE(fresh, nullptr);
  EXPECT_EQ(fresh->html_original_bytes, 0u);
  CloseServeStats(fresh);
}

TEST_F(ServeStatsTest, ServeStatsPath) {
  EXPECT_EQ(ServeStatsPath("/var/cache/pagespeed"),
            "/var/cache/.pagespeed-serve-stats");
  EXPECT_EQ(ServeStatsPath("/data/ps/cache"),
            "/data/ps/.pagespeed-serve-stats");
}

TEST_F(ServeStatsTest, CloseNull) {
  // CloseServeStats(nullptr) should be safe (no-op).
  CloseServeStats(nullptr);
}

TEST_F(ServeStatsTest, StructSize) {
  EXPECT_LE(sizeof(ServeStats), ServeStats::kFileSize);
  // v6 grew the file to 512 bytes (the Bar-A opt-in counter apparatus —
  // per-signer slots, latency buckets, boot identity — overflowed the 256-byte
  // headroom); the version+size bump self-heals existing files.  v7 adds the
  // zero-copy serve-barrier + bounded-stale counters (40 bytes), still
  // inside 512.  v8 adds the serve-class + saturation block (88 bytes).  v9
  // adds the by-encoding rows and grows the file to 1024.  v10 adds the 64
  // host slots and the other triple (10,776 bytes) and grows it to 16384.
  // v11 adds the six response-header sidecar counters (48 bytes).
  EXPECT_EQ(ServeStats::kFileSize, 16384u);
  EXPECT_EQ(ServeStats::kVersion, 11u);
  // The exact size is pinned in the header too; asserted here so the failure
  // names the surface rather than a translation unit.
  // 11496 through v11, plus the 96-byte split block appended after boot_id
  // and the 16-byte busy-read block after it, both without a version bump.
  EXPECT_EQ(sizeof(ServeStats), 11608u);
  EXPECT_EQ(sizeof(ServeStats::HostSlot), 168u);
  EXPECT_EQ(ServeStats::kFileSize - sizeof(ServeStats), 4776u)
      << "headroom shrank: the next append may need a kFileSize bump";
}

TEST_F(ServeStatsTest, OpenFileTooSmall) {
  // Create a file smaller than kFileSize.
  FILE* f = fopen(path_.c_str(), "w");
  ASSERT_NE(f, nullptr);
  (void)fwrite("short", 1, 5, f);
  fclose(f);

  ServeStats* stats = OpenServeStats(path_);
  EXPECT_EQ(stats, nullptr);
}

TEST_F(ServeStatsTest, DualMmapSharedAccess) {
  // Validate the cross-process sharing contract: two independent mmaps of
  // the same file see each other's writes via atomics.
  ServeStats* writer = CreateServeStats(path_);
  ASSERT_NE(writer, nullptr);

  ServeStats* reader = OpenServeStats(path_);
  ASSERT_NE(reader, nullptr);

  // Writer increments counters (simulating nginx).
  std::atomic_ref(writer->image_original_bytes)
      .fetch_add(10000, std::memory_order_relaxed);
  std::atomic_ref(writer->image_optimized_bytes)
      .fetch_add(3000, std::memory_order_relaxed);
  std::atomic_ref(writer->image_optimized_hits)
      .fetch_add(1, std::memory_order_relaxed);

  // Reader sees the updates (simulating worker stats API).
  uint64_t orig = std::atomic_ref(reader->image_original_bytes)
                      .load(std::memory_order_relaxed);
  uint64_t opt = std::atomic_ref(reader->image_optimized_bytes)
                     .load(std::memory_order_relaxed);
  uint64_t hits = std::atomic_ref(reader->image_optimized_hits)
                      .load(std::memory_order_relaxed);

  EXPECT_EQ(orig, 10000u);
  EXPECT_EQ(opt, 3000u);
  EXPECT_EQ(hits, 1u);

  CloseServeStats(reader);
  CloseServeStats(writer);
}

// ---------------------------------------------------------------------------
// RecordServeHit — the shared helper (single source of truth for nginx + the
// C API). The CALLER gates; the helper only does per-type atomic increments.
// ---------------------------------------------------------------------------

TEST_F(ServeStatsTest, RecordServeHitPerTypeIncrements) {
  ServeStats* stats = CreateServeStats(path_);
  ASSERT_NE(stats, nullptr);

  // Distinct original/optimized values per type so cross-bucket leakage is
  // detectable.
  RecordServeHit(stats, ContentType::kHtml, 1000, 700);
  RecordServeHit(stats, ContentType::kCss, 2000, 1500);
  RecordServeHit(stats, ContentType::kJs, 3000, 2200);
  RecordServeHit(stats, ContentType::kImage, 50000, 10000);

  EXPECT_EQ(stats->html_original_bytes, 1000u);
  EXPECT_EQ(stats->html_optimized_bytes, 700u);
  EXPECT_EQ(stats->html_optimized_hits, 1u);

  EXPECT_EQ(stats->css_original_bytes, 2000u);
  EXPECT_EQ(stats->css_optimized_bytes, 1500u);
  EXPECT_EQ(stats->css_optimized_hits, 1u);

  EXPECT_EQ(stats->js_original_bytes, 3000u);
  EXPECT_EQ(stats->js_optimized_bytes, 2200u);
  EXPECT_EQ(stats->js_optimized_hits, 1u);

  EXPECT_EQ(stats->image_original_bytes, 50000u);
  EXPECT_EQ(stats->image_optimized_bytes, 10000u);
  EXPECT_EQ(stats->image_optimized_hits, 1u);

  // The default (no-mask) image call must not touch the SVG counter.
  EXPECT_EQ(stats->svg_optimized_hits, 0u);

  CloseServeStats(stats);
}

// --- v9: serves by transfer encoding ----------------------------------------

TEST_F(ServeStatsTest, RecordServeHitSplitsByTransferEncoding) {
  ServeStats* stats = CreateServeStats(path_);
  ASSERT_NE(stats, nullptr);

  // Bits 6-7 of the mask: identity (0), gzip (1), brotli (2).
  constexpr uint32_t kGzipMask = 1u << 6;
  constexpr uint32_t kBrotliMask = 2u << 6;
  const size_t css = static_cast<size_t>(ContentType::kCss);
  const size_t image = static_cast<size_t>(ContentType::kImage);
  const size_t js = static_cast<size_t>(ContentType::kJs);

  RecordServeHit(stats, ContentType::kCss, 1000, 800, 0);
  RecordServeHit(stats, ContentType::kCss, 1000, 200, kGzipMask);
  RecordServeHit(stats, ContentType::kCss, 1000, 160, kBrotliMask);
  RecordServeHit(stats, ContentType::kImage, 5000, 5000, kGzipMask);

  // The per-type totals are unchanged by the split.
  EXPECT_EQ(3u, stats->css_optimized_hits);
  EXPECT_EQ(1u, stats->image_optimized_hits);
  // ...and the split itself:
  EXPECT_EQ(1u, stats->serve_hits_by_encoding[css][0]);
  EXPECT_EQ(800u, stats->serve_bytes_by_encoding[css][0]);
  EXPECT_EQ(1u, stats->serve_hits_by_encoding[css][1]);
  EXPECT_EQ(200u, stats->serve_bytes_by_encoding[css][1]);
  EXPECT_EQ(1u, stats->serve_hits_by_encoding[css][2]);
  EXPECT_EQ(160u, stats->serve_bytes_by_encoding[css][2]);
  EXPECT_EQ(0u, stats->serve_hits_by_encoding[image][0]);
  EXPECT_EQ(1u, stats->serve_hits_by_encoding[image][1]);
  EXPECT_EQ(5000u, stats->serve_bytes_by_encoding[image][1]);

  // The reserved encoding (3) writes no row; the totals still count it.
  RecordServeHit(stats, ContentType::kJs, 100, 90, 3u << 6);
  EXPECT_EQ(1u, stats->js_optimized_hits);
  EXPECT_EQ(0u, stats->serve_hits_by_encoding[js][0]);
  EXPECT_EQ(0u, stats->serve_hits_by_encoding[js][1]);
  EXPECT_EQ(0u, stats->serve_hits_by_encoding[js][2]);
  CloseServeStats(stats);
}

TEST_F(ServeStatsTest, ReusedFileZeroesTheEncodingBlock) {
  ServeStats* stats = CreateServeStats(path_);
  ASSERT_NE(stats, nullptr);
  RecordServeHit(stats, ContentType::kCss, 1000, 200, 1u << 6);
  CloseServeStats(stats);

  // A second create over the same path reuses the file and zeroes every
  // counter, the v9 block included.
  ServeStats* reused = CreateServeStats(path_);
  ASSERT_NE(reused, nullptr);
  EXPECT_EQ(reused->version, ServeStats::kVersion);
  EXPECT_EQ(0u, reused->css_optimized_hits);
  EXPECT_EQ(0u, reused->serve_hits_by_encoding[1][1]);
  EXPECT_EQ(0u, reused->serve_bytes_by_encoding[1][1]);
  CloseServeStats(reused);
}

// The real v8 -> v9 upgrade artifact: a 512-byte file with correct magic and
// version=8, junk past the v8 struct. Open must reject it (the SIZE gate
// fires first) and Create must replace it with a fresh zeroed v9 file -- an
// older layout must read as zeros, never as garbage counters.
TEST_F(ServeStatsTest, LegacyV8FileIsRejectedAndRecreatedZeroed) {
  std::string legacy(512, '\0');
  uint32_t magic = ServeStats::kMagic;
  uint32_t version = 8;
  std::memcpy(&legacy[0], &magic, sizeof(magic));
  std::memcpy(&legacy[4], &version, sizeof(version));
  legacy[480] = 0x2A;  // non-zero junk in the v8 tail
  legacy[500] = 0xFF;
  FILE* f = fopen(path_.c_str(), "wb");
  ASSERT_NE(f, nullptr);
  ASSERT_EQ(legacy.size(), fwrite(legacy.data(), 1, legacy.size(), f));
  fclose(f);

  EXPECT_EQ(nullptr, OpenServeStats(path_));

  ServeStats* fresh = CreateServeStats(path_);
  ASSERT_NE(fresh, nullptr);
  EXPECT_EQ(fresh->magic, ServeStats::kMagic);
  EXPECT_EQ(fresh->version, ServeStats::kVersion);
  EXPECT_EQ(0u, fresh->css_optimized_hits);
  EXPECT_EQ(0u, fresh->serve_hits_by_encoding[1][1]);
  EXPECT_EQ(0u, fresh->serve_bytes_by_encoding[3][2]);
  CloseServeStats(fresh);

  // On-disk size is the new kFileSize, so a subsequent open succeeds.
  ServeStats* reopened = OpenServeStats(path_);
  ASSERT_NE(reopened, nullptr);
  EXPECT_EQ(reopened->version, ServeStats::kVersion);
  CloseServeStats(reopened);
}

TEST_F(ServeStatsTest, RecordServeHitOnlyTouchesTargetBucket) {
  ServeStats* stats = CreateServeStats(path_);
  ASSERT_NE(stats, nullptr);

  // Record a single image hit; every other bucket must stay 0.
  RecordServeHit(stats, ContentType::kImage, 8000, 2000);

  EXPECT_EQ(stats->image_original_bytes, 8000u);
  EXPECT_EQ(stats->image_optimized_bytes, 2000u);
  EXPECT_EQ(stats->image_optimized_hits, 1u);

  EXPECT_EQ(stats->html_original_bytes, 0u);
  EXPECT_EQ(stats->html_optimized_bytes, 0u);
  EXPECT_EQ(stats->html_optimized_hits, 0u);
  EXPECT_EQ(stats->css_original_bytes, 0u);
  EXPECT_EQ(stats->css_optimized_bytes, 0u);
  EXPECT_EQ(stats->css_optimized_hits, 0u);
  EXPECT_EQ(stats->js_original_bytes, 0u);
  EXPECT_EQ(stats->js_optimized_bytes, 0u);
  EXPECT_EQ(stats->js_optimized_hits, 0u);

  CloseServeStats(stats);
}

TEST_F(ServeStatsTest, RecordServeHitAccumulates) {
  ServeStats* stats = CreateServeStats(path_);
  ASSERT_NE(stats, nullptr);

  // N calls accumulate bytes and tick hits by N.
  constexpr int kN = 4;
  for (int i = 0; i < kN; ++i) {
    RecordServeHit(stats, ContentType::kCss, 1000, 600);
  }

  EXPECT_EQ(stats->css_original_bytes, 4000u);
  EXPECT_EQ(stats->css_optimized_bytes, 2400u);
  EXPECT_EQ(stats->css_optimized_hits, static_cast<uint64_t>(kN));

  CloseServeStats(stats);
}

TEST_F(ServeStatsTest, RecordServeHitOtherIsNoOp) {
  ServeStats* stats = CreateServeStats(path_);
  ASSERT_NE(stats, nullptr);

  // kOther maps to no bucket — must change nothing.
  RecordServeHit(stats, ContentType::kOther, 9999, 8888);

  EXPECT_EQ(stats->html_original_bytes, 0u);
  EXPECT_EQ(stats->css_original_bytes, 0u);
  EXPECT_EQ(stats->js_original_bytes, 0u);
  EXPECT_EQ(stats->image_original_bytes, 0u);
  EXPECT_EQ(stats->html_optimized_hits, 0u);
  EXPECT_EQ(stats->css_optimized_hits, 0u);
  EXPECT_EQ(stats->js_optimized_hits, 0u);
  EXPECT_EQ(stats->image_optimized_hits, 0u);

  CloseServeStats(stats);
}

TEST_F(ServeStatsTest, RecordServeHitNullStatsNoCrash) {
  // nullptr stats is a documented no-op; must not crash.
  RecordServeHit(nullptr, ContentType::kImage, 1234, 567);
  SUCCEED();
}

// SVG variants serve as ContentType::kImage with image-format bits == kSvg
// (0x03) in the capability mask. RecordServeHit must bump BOTH the image bucket
// and svg_optimized_hits for those (follow-up #455), while non-SVG
// image HITs (and the default mask=0) leave svg_optimized_hits untouched.
TEST_F(ServeStatsTest, RecordServeHitSvgVariantBumpsSvgAndImage) {
  ServeStats* stats = CreateServeStats(path_);
  ASSERT_NE(stats, nullptr);

  // SVG hit: mask=3 (image-format bits == kSvg).
  RecordServeHit(stats, ContentType::kImage, 50000, 10000, /*mask=*/3);
  EXPECT_EQ(stats->image_original_bytes, 50000u);
  EXPECT_EQ(stats->image_optimized_bytes, 10000u);
  EXPECT_EQ(stats->image_optimized_hits, 1u);
  EXPECT_EQ(stats->svg_optimized_hits, 1u);

  CloseServeStats(stats);
}

TEST_F(ServeStatsTest, RecordServeHitNonSvgImageLeavesSvgZero) {
  ServeStats* stats = CreateServeStats(path_);
  ASSERT_NE(stats, nullptr);

  // Default mask (0) → plain image, no SVG accounting.
  RecordServeHit(stats, ContentType::kImage, 8000, 2000);
  // WebP variant (format bits == 1) is an image but not SVG.
  RecordServeHit(stats, ContentType::kImage, 9000, 3000, /*mask=*/1);
  // AVIF variant (format bits == 2) is an image but not SVG.
  RecordServeHit(stats, ContentType::kImage, 7000, 1500, /*mask=*/2);

  EXPECT_EQ(stats->image_optimized_hits, 3u);
  EXPECT_EQ(stats->svg_optimized_hits, 0u);

  CloseServeStats(stats);
}

// A non-image content type with the SVG mask bits set must NOT bump
// svg_optimized_hits — SVG accounting is gated on the kImage arm.
TEST_F(ServeStatsTest, RecordServeHitSvgMaskOnNonImageIsIgnored) {
  ServeStats* stats = CreateServeStats(path_);
  ASSERT_NE(stats, nullptr);

  RecordServeHit(stats, ContentType::kCss, 2000, 1500, /*mask=*/3);
  EXPECT_EQ(stats->css_optimized_hits, 1u);
  EXPECT_EQ(stats->svg_optimized_hits, 0u);

  CloseServeStats(stats);
}

// ---------------------------------------------------------------------------
// v6 Web Bot Auth opt-in counter (experimental).
// ---------------------------------------------------------------------------

// A fresh create mints a non-zero boot_id and a plausible counting-since day,
// and every v6 counter starts at zero.
TEST_F(ServeStatsTest, V6FreshCreateMintsIdentityAndZeroCounters) {
  ServeStats* stats = CreateServeStats(path_);
  ASSERT_NE(stats, nullptr);

  bool any_nonzero = false;
  for (uint8_t b : stats->boot_id) {
    if (b != 0) any_nonzero = true;
  }
  EXPECT_TRUE(any_nonzero) << "boot_id must be randomly minted";
  // RFC-4122 v4 variant bits.
  EXPECT_EQ(stats->boot_id[6] & 0xF0, 0x40);
  EXPECT_EQ(stats->boot_id[8] & 0xC0, 0x80);
  // Counting-since is a day count well past 2020 (18262 = 2020-01-01).
  EXPECT_GT(stats->counting_since_unix_day, 18262u);

  EXPECT_EQ(stats->webbotauth_other_verified_bots, 0u);
  EXPECT_EQ(stats->webbotauth_verify_latency_lt100us, 0u);
  EXPECT_EQ(stats->webbotauth_verify_latency_ge10ms, 0u);
  for (const auto& slot : stats->webbotauth_signers) {
    EXPECT_EQ(slot.kid_hash, 0u);
    EXPECT_EQ(slot.count, 0u);
  }
  CloseServeStats(stats);
}

// v6 round-trip: counters + identity survive a plain reopen.
TEST_F(ServeStatsTest, V6RoundTrip) {
  ServeStats* stats = CreateServeStats(path_);
  ASSERT_NE(stats, nullptr);
  RecordWebBotAuthVerifiedSigner(stats, "kid-A", /*latency_us=*/50);
  RecordWebBotAuthVerifiedSigner(stats, "kid-A", /*latency_us=*/5000);
  RecordWebBotAuthVerifiedSigner(stats, "kid-B", /*latency_us=*/20000);
  uint8_t boot_copy[16];
  std::memcpy(boot_copy, stats->boot_id, 16);
  uint64_t since = stats->counting_since_unix_day;
  CloseServeStats(stats);

  ServeStats* re = OpenServeStats(path_);
  ASSERT_NE(re, nullptr);
  EXPECT_EQ(0, std::memcmp(re->boot_id, boot_copy, 16));
  EXPECT_EQ(re->counting_since_unix_day, since);
  // kid-A twice, kid-B once, across two distinct slots.
  uint64_t total = 0;
  int used = 0;
  for (const auto& slot : re->webbotauth_signers) {
    if (slot.kid_hash != 0) {
      ++used;
      total += slot.count;
    }
  }
  EXPECT_EQ(used, 2);
  EXPECT_EQ(total, 3u);
  // Latency buckets: 50us, 5ms, 20ms.
  EXPECT_EQ(re->webbotauth_verify_latency_lt100us, 1u);
  EXPECT_EQ(re->webbotauth_verify_latency_lt1ms, 0u);
  EXPECT_EQ(re->webbotauth_verify_latency_lt10ms, 1u);
  EXPECT_EQ(re->webbotauth_verify_latency_ge10ms, 1u);
  CloseServeStats(re);
}

// ZeroCounters (the create-reuse path) zeroes the new v6 counters but PRESERVES
// boot_id + counting_since_unix_day (identity/epoch).
TEST_F(ServeStatsTest, V6ReuseZeroesCountersPreservesIdentity) {
  ServeStats* stats = CreateServeStats(path_);
  ASSERT_NE(stats, nullptr);
  RecordWebBotAuthVerifiedSigner(stats, "kid-A", /*latency_us=*/50);
  RecordWebBotAuthVerifiedSigner(stats, "kid-B", /*latency_us=*/20000);
  uint8_t boot_copy[16];
  std::memcpy(boot_copy, stats->boot_id, 16);
  uint64_t since = stats->counting_since_unix_day;
  CloseServeStats(stats);

  // Reuse (same inode) — CreateServeStats zeroes counters.
  ServeStats* reused = CreateServeStats(path_);
  ASSERT_NE(reused, nullptr);
  // Identity PRESERVED.
  EXPECT_EQ(0, std::memcmp(reused->boot_id, boot_copy, 16));
  EXPECT_EQ(reused->counting_since_unix_day, since);
  // Counters ZEROED.
  EXPECT_EQ(reused->webbotauth_other_verified_bots, 0u);
  EXPECT_EQ(reused->webbotauth_verify_latency_lt100us, 0u);
  EXPECT_EQ(reused->webbotauth_verify_latency_ge10ms, 0u);
  for (const auto& slot : reused->webbotauth_signers) {
    EXPECT_EQ(slot.kid_hash, 0u);
    EXPECT_EQ(slot.count, 0u);
  }
  CloseServeStats(reused);
}

// boot_id regenerates on a genuine create-fresh (not a reuse).
TEST_F(ServeStatsTest, V6BootIdRegeneratesOnCreateFresh) {
  ServeStats* a = CreateServeStats(path_);
  ASSERT_NE(a, nullptr);
  uint8_t boot_a[16];
  std::memcpy(boot_a, a->boot_id, 16);
  CloseServeStats(a);
  std::remove(path_.c_str());  // force a fresh create

  ServeStats* b = CreateServeStats(path_);
  ASSERT_NE(b, nullptr);
  // Overwhelmingly likely to differ (128 random bits).
  EXPECT_NE(0, std::memcmp(b->boot_id, boot_a, 16));
  CloseServeStats(b);
}

// Slot find-or-claim: the same keyid always lands in the same slot; distinct
// keyids claim distinct slots; the 9th distinct keyid overflows.
TEST_F(ServeStatsTest, V6SlotClaimAndOverflow) {
  ServeStats* stats = CreateServeStats(path_);
  ASSERT_NE(stats, nullptr);

  // Fill all 8 slots with distinct keyids, twice each.
  for (int rep = 0; rep < 2; ++rep) {
    for (int i = 0; i < static_cast<int>(ServeStats::kMaxCountedBots); ++i) {
      RecordWebBotAuthVerifiedSigner(stats, "kid-" + std::to_string(i), 10);
    }
  }
  int used = 0;
  for (const auto& slot : stats->webbotauth_signers) {
    if (slot.kid_hash != 0) {
      ++used;
      EXPECT_EQ(slot.count, 2u);
    }
  }
  EXPECT_EQ(used, static_cast<int>(ServeStats::kMaxCountedBots));
  EXPECT_EQ(stats->webbotauth_other_verified_bots, 0u);

  // A 9th distinct keyid overflows.
  RecordWebBotAuthVerifiedSigner(stats, "kid-overflow", 10);
  RecordWebBotAuthVerifiedSigner(stats, "kid-overflow-2", 10);
  EXPECT_EQ(stats->webbotauth_other_verified_bots, 2u);
  CloseServeStats(stats);
}

// The keyid hash is stable, non-zero, and salts the 0 sentinel away.
TEST_F(ServeStatsTest, V6HashKeyidNeverZeroAndStable) {
  EXPECT_EQ(HashWebBotAuthKeyid("kid-A"), HashWebBotAuthKeyid("kid-A"));
  EXPECT_NE(HashWebBotAuthKeyid("kid-A"), HashWebBotAuthKeyid("kid-B"));
  // No input should hash to the empty-slot sentinel.
  EXPECT_NE(HashWebBotAuthKeyid(""), 0u);
}

TEST_F(ServeStatsTest, V6RecordVerifiedSignerNullStatsNoCrash) {
  RecordWebBotAuthVerifiedSigner(nullptr, "kid", 100);
  SUCCEED();
}

// Concurrency: many threads hammering the lock-free slot claim with a keyid
// alphabet larger than kMaxCountedBots (forcing the overflow path AND the
// CAS-loss branch where a slot is claimed by another thread between our load
// and our compare_exchange).  The conservation invariant must hold exactly:
//   Σ slot.count + other_verified_bots == total number of Record calls.
// No count may be lost or double-counted regardless of interleaving.  This is
// the highest-risk code in the feature and is ThreadSanitizer-friendly (every
// shared access goes through std::atomic_ref).
TEST_F(ServeStatsTest, V6SlotClaimIsRaceFreeUnderContention) {
  ServeStats* stats = CreateServeStats(path_);
  ASSERT_NE(stats, nullptr);

  constexpr int kThreads = 8;
  constexpr int kPerThread = 20000;
  // 12 distinct kids > kMaxCountedBots (8) so 8 slots fill and the rest spill
  // into webbotauth_other_verified_bots.
  constexpr int kDistinctKids = 12;
  static_assert(kDistinctKids > static_cast<int>(ServeStats::kMaxCountedBots),
                "alphabet must exceed slot count to exercise overflow");

  std::vector<std::thread> threads;
  threads.reserve(kThreads);
  for (int t = 0; t < kThreads; ++t) {
    threads.emplace_back([stats, t]() {
      for (int i = 0; i < kPerThread; ++i) {
        // Interleave the kid choice with the thread id so different threads
        // race to claim the same kid concurrently.
        std::string kid = "kid-" + std::to_string((i + t) % kDistinctKids);
        RecordWebBotAuthVerifiedSigner(stats, kid, /*latency_us=*/(i % 20000));
      }
    });
  }
  for (auto& th : threads) th.join();

  uint64_t slot_total = 0;
  int used = 0;
  for (const auto& slot : stats->webbotauth_signers) {
    if (slot.kid_hash != 0) {
      ++used;
      slot_total += slot.count;
    }
  }
  const uint64_t grand_total =
      slot_total + stats->webbotauth_other_verified_bots;
  EXPECT_EQ(grand_total, static_cast<uint64_t>(kThreads) * kPerThread);
  // All 8 slots should have been claimed (12 kids, all seen many times).
  EXPECT_EQ(used, static_cast<int>(ServeStats::kMaxCountedBots));
  EXPECT_GT(stats->webbotauth_other_verified_bots, 0u);

  // Latency buckets also conserve the total (each call lands in exactly one).
  const uint64_t lat_total = stats->webbotauth_verify_latency_lt100us +
                             stats->webbotauth_verify_latency_lt1ms +
                             stats->webbotauth_verify_latency_lt10ms +
                             stats->webbotauth_verify_latency_ge10ms;
  EXPECT_EQ(lat_total, static_cast<uint64_t>(kThreads) * kPerThread);

  CloseServeStats(stats);
}

// A legacy v5 (256-byte) file must be rejected by the open path and recreated
// as a fresh v6 (512-byte) file by the create path.
TEST_F(ServeStatsTest, LegacyV5FileIsRejectedAndRecreated) {
  std::string legacy(256, '\0');
  uint32_t magic = ServeStats::kMagic;
  uint32_t version = 5;
  std::memcpy(&legacy[0], &magic, sizeof(magic));
  std::memcpy(&legacy[4], &version, sizeof(version));
  FILE* f = fopen(path_.c_str(), "wb");
  ASSERT_NE(f, nullptr);
  ASSERT_EQ(legacy.size(), fwrite(legacy.data(), 1, legacy.size(), f));
  fclose(f);

  // Open fails closed (size < 512 AND version != 6).
  EXPECT_EQ(nullptr, OpenServeStats(path_));

  // Create self-heals to a fresh v6 file.
  ServeStats* fresh = CreateServeStats(path_);
  ASSERT_NE(fresh, nullptr);
  EXPECT_EQ(fresh->version, ServeStats::kVersion);
  EXPECT_GT(fresh->counting_since_unix_day, 18262u);
  CloseServeStats(fresh);

  ServeStats* reopened = OpenServeStats(path_);
  ASSERT_NE(reopened, nullptr);
  CloseServeStats(reopened);
}

// =================================================================
// v8: serve-class counters + saturation sampling
// =================================================================

// Captures emitted messages so a test can assert on a warning's TEXT.  A
// NullMessageHandler would make every assertion below vacuously pass, which is
// exactly how a dropped warning goes unnoticed.
class RecordingHandler : public MessageHandler {
 public:
  struct Entry {
    MessageType type;
    std::string message;
  };

  void Message(MessageType type, const char* format, ...) override {
    va_list args;
    va_start(args, format);
    MessageV(type, format, args);
    va_end(args);
  }

  [[nodiscard]] const std::vector<Entry>& entries() const { return entries_; }

  [[nodiscard]] std::vector<std::string> warnings() const {
    std::vector<std::string> out;
    for (const auto& e : entries_) {
      if (e.type == MessageType::kWarning) out.push_back(e.message);
    }
    return out;
  }

 protected:
  void MessageV(MessageType type, const char* format, va_list args) override {
    entries_.push_back({type, FormatMessage(format, args)});
  }

 private:
  std::vector<Entry> entries_;
};

// Write a header-only file carrying `version` at the current file size, as a
// peer built against that version would have left it.
void WriteVersionedFile(const std::string& path, uint32_t version) {
  std::string raw(ServeStats::kFileSize, '\0');
  uint32_t magic = ServeStats::kMagic;
  std::memcpy(&raw[0], &magic, sizeof(magic));
  std::memcpy(&raw[4], &version, sizeof(version));
  FILE* f = fopen(path.c_str(), "wb");
  ASSERT_NE(f, nullptr);
  ASSERT_EQ(raw.size(), fwrite(raw.data(), 1, raw.size(), f));
  fclose(f);
}

TEST_F(ServeStatsTest, V8LayoutOrderingAndAlignment) {
  // The in-tree ordering rule, made executable: the v8 block sits after the
  // v7 counters and before the identity/epoch pair, which stay last.
  EXPECT_LT(offsetof(ServeStats, stale_if_error_serves),
            offsetof(ServeStats, serve_optimized_total));
  EXPECT_LT(offsetof(ServeStats, saturation_hwm),
            offsetof(ServeStats, counting_since_unix_day));
  EXPECT_LT(offsetof(ServeStats, counting_since_unix_day),
            offsetof(ServeStats, boot_id));

  // Every u64 in the block is 8-byte aligned, which is what makes the
  // cross-process atomic_ref updates legal.
  for (size_t off : {offsetof(ServeStats, serve_optimized_total),
                     offsetof(ServeStats, serve_original_cold_total),
                     offsetof(ServeStats, serve_original_pending_total),
                     offsetof(ServeStats, serve_original_declined_total),
                     offsetof(ServeStats, serve_original_skew_total),
                     offsetof(ServeStats, notify_suppressed_total),
                     offsetof(ServeStats, serve_class_unrecognized_total),
                     offsetof(ServeStats, serve_flags_unrecognized_total),
                     offsetof(ServeStats, saturation_sample_accum),
                     offsetof(ServeStats, saturation_sample_count)}) {
    EXPECT_EQ(off % 8, 0u);
  }
  // The u32 pair shares one aligned 8-byte slot.
  EXPECT_EQ(offsetof(ServeStats, worker_pool_threads) % 8, 0u);
  EXPECT_EQ(offsetof(ServeStats, saturation_hwm),
            offsetof(ServeStats, worker_pool_threads) + 4);
}

TEST_F(ServeStatsTest, LegacyV7FileIsRejectedAndRecreatedWithWarning) {
  WriteVersionedFile(path_, 7);

  // The reader side fails closed on the older file.
  EXPECT_EQ(nullptr, OpenServeStats(path_));

  RecordingHandler handler;
  ServeStats* fresh = CreateServeStats(path_, &handler);
  ASSERT_NE(fresh, nullptr);
  EXPECT_EQ(fresh->version, ServeStats::kVersion);
  EXPECT_EQ(fresh->serve_original_pending_total, 0u);
  CloseServeStats(fresh);

  // The skew must be announced, naming BOTH versions: flat counters and a
  // silently-stopped peer are the only other symptoms.
  const std::vector<std::string> warnings = handler.warnings();
  ASSERT_EQ(warnings.size(), 1u) << "expected exactly one skew warning";
  EXPECT_NE(warnings[0].find("version 7"), std::string::npos) << warnings[0];
  EXPECT_NE(warnings[0].find("version 11"), std::string::npos) << warnings[0];

  ServeStats* reopened = OpenServeStats(path_);
  ASSERT_NE(reopened, nullptr);
  CloseServeStats(reopened);
}

TEST_F(ServeStatsTest, CreateWarnsOnlyOnVersionSkew) {
  // Fresh create of a missing file: nothing to warn about.
  RecordingHandler first;
  ServeStats* created = CreateServeStats(path_, &first);
  ASSERT_NE(created, nullptr);
  CloseServeStats(created);
  EXPECT_TRUE(first.warnings().empty());

  // Same-version reuse: still nothing to warn about.
  RecordingHandler second;
  ServeStats* reused = CreateServeStats(path_, &second);
  ASSERT_NE(reused, nullptr);
  CloseServeStats(reused);
  EXPECT_TRUE(second.warnings().empty());

  // A version-skewed file: exactly one warning.
  WriteVersionedFile(path_, 6);
  RecordingHandler third;
  ServeStats* healed = CreateServeStats(path_, &third);
  ASSERT_NE(healed, nullptr);
  CloseServeStats(healed);
  EXPECT_EQ(third.warnings().size(), 1u);
}

// Returns the five serve-class counters in enum order.
std::vector<uint64_t> ServeClassCounters(const ServeStats* s) {
  return {s->serve_optimized_total, s->serve_original_cold_total,
          s->serve_original_pending_total, s->serve_original_declined_total,
          s->serve_original_skew_total};
}

TEST_F(ServeStatsTest, RecordServeClassMovesExactlyOneCounter) {
  const ServeClass classes[] = {
      ServeClass::kOptimized, ServeClass::kOriginalCold,
      ServeClass::kOriginalPending, ServeClass::kOriginalDeclined,
      ServeClass::kOriginalSkew};

  for (size_t i = 0; i < std::size(classes); ++i) {
    ServeStats* stats = CreateServeStats(path_);
    ASSERT_NE(stats, nullptr);

    RecordServeClass(stats, classes[i]);

    const std::vector<uint64_t> counters = ServeClassCounters(stats);
    for (size_t j = 0; j < counters.size(); ++j) {
      EXPECT_EQ(counters[j], i == j ? 1u : 0u)
          << "class index " << i << " moved counter index " << j;
    }
    // The suppressed-notify counter is orthogonal and must stay put, and an
    // accepted write must never look like a dropped or misflagged one.
    EXPECT_EQ(stats->notify_suppressed_total, 0u);
    EXPECT_EQ(stats->serve_class_unrecognized_total, 0u);
    EXPECT_EQ(stats->serve_flags_unrecognized_total, 0u);
    CloseServeStats(stats);
    std::remove(path_.c_str());
  }
}

TEST_F(ServeStatsTest, RecordServeClassRejectsTwoClassesAtOnce) {
  // The classes are disjoint bits, so any combination of two is not a class.
  // Recording one must count NOTHING rather than land on a third class — this
  // is the property that keeps the five counters a partition.
  const ServeClass classes[] = {
      ServeClass::kOptimized, ServeClass::kOriginalCold,
      ServeClass::kOriginalPending, ServeClass::kOriginalDeclined,
      ServeClass::kOriginalSkew};

  ServeStats* stats = CreateServeStats(path_);
  ASSERT_NE(stats, nullptr);

  int pairs = 0;
  for (size_t i = 0; i < std::size(classes); ++i) {
    for (size_t j = i + 1; j < std::size(classes); ++j) {
      const auto combined =
          static_cast<ServeClass>(static_cast<uint32_t>(classes[i]) |
                                  static_cast<uint32_t>(classes[j]));
      RecordServeClass(stats, combined, kServeFlagNotifySuppressed);
      ++pairs;
    }
  }
  EXPECT_EQ(pairs, 10);

  for (uint64_t counter : ServeClassCounters(stats)) {
    EXPECT_EQ(counter, 0u) << "a two-class value must count no serve class";
  }
  // Not even the orthogonal flag: an unrecognised class means the caller told
  // us nothing reliable about the serve.
  EXPECT_EQ(stats->notify_suppressed_total, 0u);
  // But the drop must not be silent.  Every rejected write is accounted for,
  // so a reader can see that the five counters are short and by how much —
  // otherwise a broken partition is indistinguishable from a healthy one.
  EXPECT_EQ(stats->serve_class_unrecognized_total, static_cast<uint64_t>(pairs))
      << "each rejected write must leave a trace";
  // A dropped write is counted once, as a drop: its flags were never
  // interpreted either, and reporting both would double-report one bad call.
  EXPECT_EQ(stats->serve_flags_unrecognized_total, 0u);
  CloseServeStats(stats);
}

TEST_F(ServeStatsTest, RecordServeClassIgnoresUnknownValues) {
  ServeStats* stats = CreateServeStats(path_);
  ASSERT_NE(stats, nullptr);

  const uint32_t unknown[] = {0u, 3u, 7u, 32u, 0xFFFFFFFFu};
  for (uint32_t raw : unknown) {
    RecordServeClass(stats, static_cast<ServeClass>(raw));
  }
  for (uint64_t counter : ServeClassCounters(stats)) {
    EXPECT_EQ(counter, 0u);
  }
  // Includes 32 — a value a NEWER peer could legitimately mean as a sixth
  // class.  This build cannot honour it, and the count is how that shows up.
  EXPECT_EQ(stats->serve_class_unrecognized_total, std::size(unknown));
  CloseServeStats(stats);
}

TEST_F(ServeStatsTest, RecordServeClassSuppressedFlagIsOrthogonal) {
  ServeStats* stats = CreateServeStats(path_);
  ASSERT_NE(stats, nullptr);

  RecordServeClass(stats, ServeClass::kOriginalCold,
                   kServeFlagNotifySuppressed);
  EXPECT_EQ(stats->serve_original_cold_total, 1u);
  EXPECT_EQ(stats->notify_suppressed_total, 1u);

  // A known flag alone is not "unrecognised".
  EXPECT_EQ(stats->serve_flags_unrecognized_total, 0u);

  // Unknown flag bits are ignored, and the class still lands — the write is
  // accepted, so this must NOT be counted as a dropped class.
  RecordServeClass(stats, ServeClass::kOriginalCold, 0xFFFFFFFEu);
  EXPECT_EQ(stats->serve_original_cold_total, 2u);
  EXPECT_EQ(stats->notify_suppressed_total, 1u);
  EXPECT_EQ(stats->serve_flags_unrecognized_total, 1u)
      << "an ignored flag bit must be visible to a reader";
  EXPECT_EQ(stats->serve_class_unrecognized_total, 0u)
      << "an accepted write must never be reported as a dropped one";

  // Known and unknown bits together: still one accepted write, one ignored-bit
  // report, and the known bit still honoured.
  RecordServeClass(stats, ServeClass::kOriginalCold,
                   kServeFlagNotifySuppressed | 0x80000000u);
  EXPECT_EQ(stats->serve_original_cold_total, 3u);
  EXPECT_EQ(stats->notify_suppressed_total, 2u);
  EXPECT_EQ(stats->serve_flags_unrecognized_total, 2u);
  EXPECT_EQ(stats->serve_class_unrecognized_total, 0u);

  CloseServeStats(stats);
}

TEST_F(ServeStatsTest, RecordServeClassNullStatsNoCrash) {
  RecordServeClass(nullptr, ServeClass::kOptimized);
  RecordServeClass(nullptr, ServeClass::kOriginalSkew,
                   kServeFlagNotifySuppressed);
  RecordSaturationSample(nullptr, 5);
  SetWorkerPoolThreads(nullptr, 8);
}

TEST_F(ServeStatsTest, SaturationSamplesAccumulateAndTrackTheMax) {
  ServeStats* stats = CreateServeStats(path_);
  ASSERT_NE(stats, nullptr);

  // A peak in the middle: the high-water mark must survive the samples that
  // follow it, which is what separates a max from a last-value gauge.
  const uint32_t samples[] = {1, 4, 9, 2, 0};
  uint64_t expected_sum = 0;
  for (uint32_t sample : samples) {
    RecordSaturationSample(stats, sample);
    expected_sum += sample;
  }

  EXPECT_EQ(stats->saturation_sample_count, std::size(samples));
  EXPECT_EQ(stats->saturation_sample_accum, expected_sum);
  EXPECT_EQ(stats->saturation_hwm, 9u);
  // The mean a reader computes from the pair.
  EXPECT_EQ(stats->saturation_sample_accum / stats->saturation_sample_count,
            expected_sum / std::size(samples));

  // A zero sample still advances the count: an idle interval is data, and a
  // count that stalls is how a reader detects a worker that stopped sampling.
  const uint64_t count_before = stats->saturation_sample_count;
  RecordSaturationSample(stats, 0);
  EXPECT_EQ(stats->saturation_sample_count, count_before + 1);
  EXPECT_EQ(stats->saturation_hwm, 9u);

  CloseServeStats(stats);
}

TEST_F(ServeStatsTest, WorkerPoolThreadsIsStampedAndSurvivesReuse) {
  ServeStats* stats = CreateServeStats(path_);
  ASSERT_NE(stats, nullptr);
  EXPECT_EQ(stats->worker_pool_threads, 0u) << "unstamped means uninstrumented";

  SetWorkerPoolThreads(stats, 12);
  RecordSaturationSample(stats, 7);
  RecordServeClass(stats, ServeClass::kOptimized);
  EXPECT_EQ(stats->worker_pool_threads, 12u);
  CloseServeStats(stats);

  // Reuse resets the counters but not the configuration field: a reader that
  // polls across a worker restart must never see the block go "uninstrumented".
  ServeStats* reused = CreateServeStats(path_);
  ASSERT_NE(reused, nullptr);
  EXPECT_EQ(reused->worker_pool_threads, 12u);
  EXPECT_EQ(reused->saturation_sample_accum, 0u);
  EXPECT_EQ(reused->saturation_sample_count, 0u);
  EXPECT_EQ(reused->saturation_hwm, 0u);
  EXPECT_EQ(reused->serve_optimized_total, 0u);
  CloseServeStats(reused);
}

TEST_F(ServeStatsTest, V8CountersZeroOnReuseAndIdentitySurvives) {
  ServeStats* stats = CreateServeStats(path_);
  ASSERT_NE(stats, nullptr);
  RecordServeClass(stats, ServeClass::kOptimized);
  RecordServeClass(stats, ServeClass::kOriginalCold,
                   kServeFlagNotifySuppressed);
  RecordServeClass(stats, ServeClass::kOriginalPending);
  RecordServeClass(stats, ServeClass::kOriginalDeclined);
  RecordServeClass(stats, ServeClass::kOriginalSkew);
  RecordServeClass(stats, static_cast<ServeClass>(99));
  RecordServeClass(stats, ServeClass::kOptimized, 0x40000000u);
  RecordSaturationSample(stats, 3);
  SetWorkerPoolThreads(stats, 6);
  const uint64_t since = stats->counting_since_unix_day;
  std::vector<uint8_t> boot(std::begin(stats->boot_id),
                            std::end(stats->boot_id));
  CloseServeStats(stats);

  ServeStats* reused = CreateServeStats(path_);
  ASSERT_NE(reused, nullptr);
  for (uint64_t counter : ServeClassCounters(reused)) EXPECT_EQ(counter, 0u);
  EXPECT_EQ(reused->notify_suppressed_total, 0u);
  EXPECT_EQ(reused->serve_class_unrecognized_total, 0u);
  EXPECT_EQ(reused->serve_flags_unrecognized_total, 0u);
  EXPECT_EQ(reused->saturation_sample_accum, 0u);
  EXPECT_EQ(reused->saturation_sample_count, 0u);
  EXPECT_EQ(reused->saturation_hwm, 0u);
  // Identity/epoch and configuration survive the reset.
  EXPECT_EQ(reused->counting_since_unix_day, since);
  EXPECT_TRUE(
      std::equal(boot.begin(), boot.end(), std::begin(reused->boot_id)));
  EXPECT_EQ(reused->worker_pool_threads, 6u);
  CloseServeStats(reused);
}

TEST_F(ServeStatsTest, V8CountersAreVisibleAcrossMappings) {
  // The cross-process contract for the new block: a second mapping of the same
  // file observes the first mapping's relaxed atomic writes.
  ServeStats* writer = CreateServeStats(path_);
  ASSERT_NE(writer, nullptr);
  ServeStats* reader = OpenServeStats(path_);
  ASSERT_NE(reader, nullptr);
  ASSERT_NE(writer, reader);

  RecordServeClass(writer, ServeClass::kOriginalPending);
  RecordSaturationSample(writer, 11);
  SetWorkerPoolThreads(writer, 4);

  EXPECT_EQ(reader->serve_original_pending_total, 1u);
  EXPECT_EQ(reader->saturation_sample_accum, 11u);
  EXPECT_EQ(reader->saturation_sample_count, 1u);
  EXPECT_EQ(reader->saturation_hwm, 11u);
  EXPECT_EQ(reader->worker_pool_threads, 4u);

  CloseServeStats(reader);
  CloseServeStats(writer);
}

TEST_F(ServeStatsTest, SaturationHwmIsRaceFreeUnderContention) {
  ServeStats* stats = CreateServeStats(path_);
  ASSERT_NE(stats, nullptr);

  constexpr int kThreads = 4;
  constexpr int kPerThread = 2000;
  std::vector<std::thread> threads;
  threads.reserve(kThreads);
  for (int t = 0; t < kThreads; ++t) {
    threads.emplace_back([stats, t] {
      for (int i = 0; i < kPerThread; ++i) {
        RecordSaturationSample(stats, static_cast<uint32_t>((i % 50) + t));
      }
    });
  }
  for (auto& th : threads) th.join();

  EXPECT_EQ(stats->saturation_sample_count,
            static_cast<uint64_t>(kThreads) * kPerThread);
  // Highest value any thread submitted: 49 + (kThreads - 1).
  EXPECT_EQ(stats->saturation_hwm, 49u + kThreads - 1);
  CloseServeStats(stats);
}

// ---- v10: serve savings by host ----

const ServeHostRow* FindHost(const ServeSavingsByHost& by_host,
                             const std::string& host) {
  for (const ServeHostRow& row : by_host.hosts) {
    if (row.host == host) return &row;
  }
  return nullptr;
}

TEST_F(ServeStatsTest, ServeHostsRecordPerHostAndReadBack) {
  ServeStats* stats = CreateServeStats(path_);
  ASSERT_NE(stats, nullptr);
  RecordServeHit(stats, ContentType::kCss, 1000, 400, 0, "www.example.com");
  RecordServeHit(stats, ContentType::kCss, 1000, 400, 0, "www.example.com");
  RecordServeHit(stats, ContentType::kImage, 5000, 1000, 0,
                 "static.example.com");

  const ServeSavingsByHost by_host =
      ReadServeSavingsByHost(stats, kServeHostReportLimit);
  ASSERT_EQ(by_host.hosts.size(), 2u);
  EXPECT_EQ(by_host.hosts[0].host, "www.example.com");
  EXPECT_EQ(by_host.hosts[0].hits, 2u);
  EXPECT_EQ(by_host.hosts[0].original_bytes, 2000u);
  EXPECT_EQ(by_host.hosts[0].optimized_bytes, 800u);
  EXPECT_EQ(by_host.hosts[1].host, "static.example.com");
  EXPECT_EQ(by_host.hosts[1].hits, 1u);
  EXPECT_EQ(by_host.hosts[1].original_bytes, 5000u);
  EXPECT_EQ(by_host.hosts[1].optimized_bytes, 1000u);
  EXPECT_EQ(by_host.other.hits, 0u);
  // The per-type totals are counted exactly as before.
  EXPECT_EQ(stats->css_optimized_hits, 2u);
  EXPECT_EQ(stats->css_original_bytes, 2000u);
  EXPECT_EQ(stats->image_optimized_hits, 1u);
  CloseServeStats(stats);
}

TEST_F(ServeStatsTest, ServeHostsNormaliseCaseAndPort) {
  ServeStats* stats = CreateServeStats(path_);
  ASSERT_NE(stats, nullptr);
  RecordServeHostHit(stats, "WWW.Example.COM", 10, 5);
  RecordServeHostHit(stats, "www.example.com:8443", 10, 5);
  RecordServeHostHit(stats, "www.example.com.", 10, 5);
  RecordServeHostHit(stats, "[2001:DB8::1]:443", 10, 5);
  RecordServeHostHit(stats, "[2001:db8::1]", 10, 5);

  const ServeSavingsByHost by_host =
      ReadServeSavingsByHost(stats, kServeHostReportLimit);
  ASSERT_EQ(by_host.hosts.size(), 2u);
  const ServeHostRow* www = FindHost(by_host, "www.example.com");
  ASSERT_NE(www, nullptr);
  EXPECT_EQ(www->hits, 3u);
  const ServeHostRow* v6 = FindHost(by_host, "[2001:db8::1]");
  ASSERT_NE(v6, nullptr);
  EXPECT_EQ(v6->hits, 2u);
  EXPECT_EQ(by_host.other.hits, 0u);

  std::string out;
  EXPECT_TRUE(NormalizeServeHost("Shop.Example.org:80", &out));
  EXPECT_EQ(out, "shop.example.org");
  CloseServeStats(stats);
}

TEST_F(ServeStatsTest, ServeHostsOutsideTheGrammarCountUnderOther) {
  ServeStats* stats = CreateServeStats(path_);
  ASSERT_NE(stats, nullptr);
  const std::string long_host(200, 'a');
  const std::string with_nul("a\0b.example", 11);
  const std::vector<std::string> bad = {
      "",
      "bad host",
      "a\"b.example",
      "x\ny",
      "<img src=x>",
      "caf\xc3\xa9.example",
      "example.com:99999999",
      "[::1",
      "a/b",
      long_host,
      with_nul,
      ".",
      "..",
      "-",
      "[:]",
  };
  for (const std::string& host : bad) {
    std::string out;
    EXPECT_FALSE(NormalizeServeHost(host, &out)) << host;
    RecordServeHostHit(stats, host, 100, 50);
  }
  const ServeSavingsByHost by_host =
      ReadServeSavingsByHost(stats, kServeHostReportLimit);
  EXPECT_TRUE(by_host.hosts.empty());
  EXPECT_EQ(by_host.other.hits, bad.size());
  EXPECT_EQ(by_host.other.original_bytes, 100u * bad.size());
  EXPECT_EQ(by_host.other.optimized_bytes, 50u * bad.size());
  CloseServeStats(stats);
}

// The host-name rule (serve_stats.h, NormalizeServeHost) with the shapes a
// real server meets.  Every accepted spelling becomes one name that the
// reader's second pass accepts unchanged -- the write path and the read
// path agree -- and every refused value only ever counts under "other".
TEST_F(ServeStatsTest, ServeHostGrammarAgreesOnWriteAndRead) {
  struct Case {
    std::string input;
    std::string name;  // empty: refused
  };
  const std::vector<Case> cases = {
      {"www.example.com", "www.example.com"},
      {"WWW.Example.COM", "www.example.com"},
      {"www.example.com.", "www.example.com"},
      {"www.example.com:8080", "www.example.com"},
      {"www.example.com.:443", "www.example.com"},
      {"a_b.test", "a_b.test"},
      {"10.1.2.3", "10.1.2.3"},
      {"[::1]", "[::1]"},
      {"[2001:db8::1]", "[2001:db8::1]"},
      {"[2001:DB8::1]:80", "[2001:db8::1]"},
      {std::string(127, 'a'), std::string(127, 'a')},
      {"www.example.com..", ""},
      {std::string(128, 'a'), ""},
      {std::string(300, 'a'), ""},
      {"", ""},
      {"_", ""},
      {".", ""},
      {"-", ""},
      {"[]", ""},
      {"[:]", ""},
      {"[::1]x", ""},
      {"[zz::1]", ""},
      {"[::1", ""},
      {"[::1].", ""},
      {"a[b].test", ""},
      {"::1", ""},
      {"2001:db8::1", ""},
      {"example.com:", ""},
      {"example.com:123456", ""},
      {"*.example.com", ""},
      {"(other)", ""},
  };
  for (const Case& c : cases) {
    std::string out;
    const bool accepted = NormalizeServeHost(c.input, &out);
    EXPECT_EQ(accepted, !c.name.empty()) << c.input;
    if (!accepted) continue;
    EXPECT_EQ(out, c.name) << c.input;
    std::string again;
    EXPECT_TRUE(NormalizeServeHost(out, &again)) << c.input;
    EXPECT_EQ(again, out) << c.input;
  }

  ServeStats* stats = CreateServeStats(path_);
  ASSERT_NE(stats, nullptr);
  uint64_t refused = 0;
  std::set<std::string> expected;
  for (const Case& c : cases) {
    RecordServeHostHit(stats, c.input, 10, 4);
    if (c.name.empty()) {
      ++refused;
    } else {
      expected.insert(c.name);
    }
  }
  const ServeSavingsByHost by_host = ReadServeSavingsByHost(stats, 1000);
  std::set<std::string> reported;
  for (const ServeHostRow& row : by_host.hosts) reported.insert(row.host);
  EXPECT_EQ(reported, expected);
  EXPECT_EQ(by_host.other.hits, refused);
  CloseServeStats(stats);
}

TEST_F(ServeStatsTest, RecordServeHitWithoutAHostCountsUnderOther) {
  ServeStats* stats = CreateServeStats(path_);
  ASSERT_NE(stats, nullptr);
  RecordServeHit(stats, ContentType::kJs, 300, 200);  // no host
  // Not a counted content type: neither the totals nor the hosts move.
  RecordServeHit(stats, ContentType::kOther, 300, 200, 0, "www.example.com");

  const ServeSavingsByHost by_host =
      ReadServeSavingsByHost(stats, kServeHostReportLimit);
  EXPECT_TRUE(by_host.hosts.empty());
  EXPECT_EQ(by_host.other.hits, 1u);
  EXPECT_EQ(by_host.other.original_bytes, 300u);
  EXPECT_EQ(by_host.other.optimized_bytes, 200u);
  EXPECT_EQ(stats->js_optimized_hits, 1u);
  CloseServeStats(stats);
}

TEST_F(ServeStatsTest, ServeHostsPast64CountUnderOther) {
  ServeStats* stats = CreateServeStats(path_);
  ASSERT_NE(stats, nullptr);
  for (int i = 0; i < 70; ++i) {
    RecordServeHostHit(stats, "host" + std::to_string(i) + ".example", 10, 5);
  }
  const ServeSavingsByHost all = ReadServeSavingsByHost(stats, 1000);
  EXPECT_EQ(all.hosts.size(), ServeStats::kHostSlots);
  EXPECT_EQ(all.other.hits, 70u - ServeStats::kHostSlots);
  uint64_t total = all.other.hits;
  for (const ServeHostRow& row : all.hosts) total += row.hits;
  EXPECT_EQ(total, 70u);
  CloseServeStats(stats);
}

TEST_F(ServeStatsTest, ReadTopHostsFoldsTheRestIntoOther) {
  ServeStats* stats = CreateServeStats(path_);
  ASSERT_NE(stats, nullptr);
  // Host i is served i + 1 times: 40 hosts, 820 serves.
  for (int i = 0; i < 40; ++i) {
    for (int n = 0; n <= i; ++n) {
      RecordServeHostHit(stats, "h" + std::to_string(i) + ".example", 2, 1);
    }
  }
  const ServeSavingsByHost top =
      ReadServeSavingsByHost(stats, kServeHostReportLimit);
  ASSERT_EQ(top.hosts.size(), kServeHostReportLimit);
  EXPECT_EQ(top.hosts.front().host, "h39.example");
  EXPECT_EQ(top.hosts.front().hits, 40u);
  EXPECT_EQ(top.hosts.back().host, "h8.example");
  EXPECT_EQ(top.hosts.back().hits, 9u);
  EXPECT_EQ(top.other.hits, 36u);  // 1 + 2 + ... + 8
  EXPECT_EQ(top.other.original_bytes, 72u);
  CloseServeStats(stats);
}

TEST_F(ServeStatsTest, ReusedFileZeroesTheHostBlock) {
  ServeStats* stats = CreateServeStats(path_);
  ASSERT_NE(stats, nullptr);
  RecordServeHostHit(stats, "www.example.com", 10, 5);
  RecordServeHostHit(stats, "", 10, 5);
  ASSERT_EQ(ReadServeSavingsByHost(stats, kServeHostReportLimit).hosts.size(),
            1u);
  CloseServeStats(stats);

  ServeStats* reused = CreateServeStats(path_);
  ASSERT_NE(reused, nullptr);
  const ServeSavingsByHost by_host =
      ReadServeSavingsByHost(reused, kServeHostReportLimit);
  EXPECT_TRUE(by_host.hosts.empty());
  EXPECT_EQ(by_host.other.hits, 0u);
  for (const ServeStats::HostSlot& slot : reused->serve_hosts) {
    EXPECT_EQ(slot.key_hash, 0u);
    EXPECT_EQ(slot.ready, 0u);
    EXPECT_EQ(slot.name[0], '\0');
  }
  CloseServeStats(reused);
}

// The real v9 -> v10 upgrade artifact: a 1024-byte file with the right magic
// and version 9, junk past the v9 struct.  Open must reject it, Create must
// replace it with a zeroed v10 file and say so once, naming both versions --
// an older layout must read as zeros, never as garbage host rows.
TEST_F(ServeStatsTest, LegacyV9FileIsRejectedAndRecreatedZeroedWithWarning) {
  std::string legacy(1024, '\0');
  const uint32_t magic = ServeStats::kMagic;
  const uint32_t version = 9;
  std::memcpy(&legacy[0], &magic, sizeof(magic));
  std::memcpy(&legacy[4], &version, sizeof(version));
  legacy[700] = 0x2A;
  legacy[1000] = static_cast<char>(0xFF);
  FILE* f = fopen(path_.c_str(), "wb");
  ASSERT_NE(f, nullptr);
  ASSERT_EQ(legacy.size(), fwrite(legacy.data(), 1, legacy.size(), f));
  fclose(f);

  EXPECT_EQ(nullptr, OpenServeStats(path_));

  RecordingHandler handler;
  ServeStats* fresh = CreateServeStats(path_, &handler);
  ASSERT_NE(fresh, nullptr);
  EXPECT_EQ(fresh->version, ServeStats::kVersion);
  const ServeSavingsByHost by_host =
      ReadServeSavingsByHost(fresh, kServeHostReportLimit);
  EXPECT_TRUE(by_host.hosts.empty());
  EXPECT_EQ(by_host.other.hits, 0u);
  CloseServeStats(fresh);

  const std::vector<std::string> warnings = handler.warnings();
  ASSERT_EQ(warnings.size(), 1u);
  EXPECT_NE(warnings[0].find("version 9"), std::string::npos) << warnings[0];
  EXPECT_NE(warnings[0].find("version 11"), std::string::npos) << warnings[0];

  ServeStats* reopened = OpenServeStats(path_);
  ASSERT_NE(reopened, nullptr);
  CloseServeStats(reopened);
}

TEST_F(ServeStatsTest, ServeHostClaimIsRaceFreeUnderContention) {
  ServeStats* stats = CreateServeStats(path_);
  ASSERT_NE(stats, nullptr);
  constexpr int kThreads = 8;
  constexpr int kPerThread = 2000;
  std::vector<std::thread> threads;
  threads.reserve(kThreads);
  for (int t = 0; t < kThreads; ++t) {
    threads.emplace_back([stats] {
      for (int j = 0; j < kPerThread; ++j) {
        RecordServeHostHit(stats, (j % 2 == 0) ? "a.example" : "b.example", 3,
                           1);
      }
    });
  }
  for (std::thread& t : threads) t.join();
  const ServeSavingsByHost by_host =
      ReadServeSavingsByHost(stats, kServeHostReportLimit);
  ASSERT_EQ(by_host.hosts.size(), 2u);
  for (const ServeHostRow& row : by_host.hosts) {
    EXPECT_EQ(row.hits, static_cast<uint64_t>(kThreads * kPerThread / 2));
    EXPECT_EQ(row.original_bytes,
              static_cast<uint64_t>(3 * kThreads * kPerThread / 2));
  }
  EXPECT_EQ(by_host.other.hits, 0u);
  CloseServeStats(stats);
}

// A ZeroCounters racing a claim can leave a slot whose name was torn across
// the restart -- still grammar-valid ("www.exa"), but not the name the slot's
// key_hash was claimed for.  Such a slot is never a host row (so never a JSON
// key or a metrics label); its serves count under other.
TEST_F(ServeStatsTest, ATornHostNameIsNeverReported) {
  ServeStats* stats = CreateServeStats(path_);
  ASSERT_NE(stats, nullptr);
  RecordServeHostHit(stats, "www.example.com", 10, 5);
  ServeStats::HostSlot* claimed = nullptr;
  for (ServeStats::HostSlot& slot : stats->serve_hosts) {
    if (slot.key_hash != 0) claimed = &slot;
  }
  ASSERT_NE(claimed, nullptr);
  ASSERT_EQ(claimed->ready, 1u);
  std::memset(claimed->name, 0, sizeof(claimed->name));
  std::memcpy(claimed->name, "www.exa", 7);
  // The torn name is itself grammar-valid: only the hash check rejects it.
  std::string torn;
  ASSERT_TRUE(NormalizeServeHost("www.exa", &torn) && torn == "www.exa");

  const ServeSavingsByHost by_host =
      ReadServeSavingsByHost(stats, kServeHostReportLimit);
  EXPECT_TRUE(by_host.hosts.empty());
  EXPECT_EQ(by_host.other.hits, 1u);
  EXPECT_EQ(by_host.other.original_bytes, 10u);
  EXPECT_EQ(by_host.other.optimized_bytes, 5u);
  CloseServeStats(stats);
}

// A ZeroCounters racing a probe across a restart can leave one host in two
// slots.  The host is still reported once, with both slots' serves summed, so
// /v1/stats never lists it twice and /v1/metrics never repeats a series.
TEST_F(ServeStatsTest, AHostInTwoSlotsIsReportedOnceWithSummedCounters) {
  ServeStats* stats = CreateServeStats(path_);
  ASSERT_NE(stats, nullptr);
  RecordServeHostHit(stats, "www.example.com", 10, 5);
  RecordServeHostHit(stats, "static.example.com", 7, 3);
  ServeStats::HostSlot* claimed = nullptr;
  for (ServeStats::HostSlot& slot : stats->serve_hosts) {
    if (slot.ready == 1 && std::string(slot.name) == "www.example.com") {
      claimed = &slot;
    }
  }
  ASSERT_NE(claimed, nullptr);
  ServeStats::HostSlot* twin = nullptr;
  for (ServeStats::HostSlot& slot : stats->serve_hosts) {
    if (slot.key_hash == 0) {
      twin = &slot;
      break;
    }
  }
  ASSERT_NE(twin, nullptr);
  std::memcpy(twin, claimed, sizeof(*twin));
  twin->hits = 2;
  twin->original_bytes = 20;
  twin->optimized_bytes = 8;

  const ServeSavingsByHost by_host =
      ReadServeSavingsByHost(stats, kServeHostReportLimit);
  ASSERT_EQ(by_host.hosts.size(), 2u);
  EXPECT_EQ(by_host.hosts[0].host, "www.example.com");
  EXPECT_EQ(by_host.hosts[0].hits, 3u);
  EXPECT_EQ(by_host.hosts[0].original_bytes, 30u);
  EXPECT_EQ(by_host.hosts[0].optimized_bytes, 13u);
  EXPECT_EQ(by_host.hosts[1].host, "static.example.com");
  EXPECT_EQ(by_host.hosts[1].hits, 1u);
  EXPECT_EQ(by_host.other.hits, 0u);

  // The merge happens before the reporting limit: one row, not two.
  const ServeSavingsByHost top_one = ReadServeSavingsByHost(stats, 1);
  ASSERT_EQ(top_one.hosts.size(), 1u);
  EXPECT_EQ(top_one.hosts[0].host, "www.example.com");
  EXPECT_EQ(top_one.hosts[0].hits, 3u);
  EXPECT_EQ(top_one.other.hits, 1u);
  EXPECT_EQ(top_one.other.original_bytes, 7u);
  CloseServeStats(stats);
}

// A name the reader would never report (it does not survive a second
// normalisation) must not take one of the 64 slots: its serves go straight
// to other.
TEST_F(ServeStatsTest, AHostThatCanNeverBeReportedClaimsNoSlot) {
  ServeStats* stats = CreateServeStats(path_);
  ASSERT_NE(stats, nullptr);
  const std::vector<std::string> refused = {"www.example.com..", "a..:8080"};
  for (const std::string& host : refused) {
    std::string out;
    EXPECT_FALSE(NormalizeServeHost(host, &out)) << host << " -> " << out;
    RecordServeHostHit(stats, host, 100, 50);
  }
  for (const ServeStats::HostSlot& slot : stats->serve_hosts) {
    EXPECT_EQ(slot.key_hash, 0u) << slot.name;
  }
  const ServeSavingsByHost by_host =
      ReadServeSavingsByHost(stats, kServeHostReportLimit);
  EXPECT_TRUE(by_host.hosts.empty());
  EXPECT_EQ(by_host.other.hits, refused.size());
  EXPECT_EQ(by_host.other.original_bytes, 100u * refused.size());
  EXPECT_EQ(by_host.other.optimized_bytes, 50u * refused.size());
  CloseServeStats(stats);
}

TEST_F(ServeStatsTest, ServeHostsNullStatsAreNoOps) {
  RecordServeHostHit(nullptr, "www.example.com", 1, 1);
  RecordServeHit(nullptr, ContentType::kCss, 1, 1, 0, "www.example.com");
  const ServeSavingsByHost by_host = ReadServeSavingsByHost(nullptr, 32);
  EXPECT_TRUE(by_host.hosts.empty());
  EXPECT_EQ(by_host.other.hits, 0u);
}

}  // namespace
}  // namespace pagespeed
