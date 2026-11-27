// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

// Peer-process origin rewrite tests
//
// - An origin file rewritten in place and recorded by a peer process must
//   replace the worker's stored copies, per content type
//
// One of the topic files of //test/src/worker:worker_test; the shared fixture
// lives in worker_test_fixture.h.

// clang-format off
// Keep the fixture header first: it preserves the old worker_test.cc include
// order (worker.h before any Windows header), which Windows builds have proven.
// See lib/base/message_handler.h for the FormatMessage macro interaction.
#include "test/src/worker/worker_test_fixture.h"
// clang-format on

#include <cstdint>
#include <span>
#include <string>
#include <thread>

#include "gtest/gtest.h"
#include "lib/base/message_handler.h"
#include "lib/cache/cache.h"
#include "lib/classify/alternate_id.h"
#include "lib/classify/alternate_metadata.h"
#include "lib/classify/capability_mask.h"
#include "src/proto/worker_ipc.h"
#include "src/worker/browser_analysis_manager.h"
#include "src/worker/text_compressor.h"
#include "src/worker/worker.h"
#include "test/test_util/worker_quiescence.h"

namespace pagespeed {
namespace {

TEST_F(WorkerPeerRewriteTest,
       NativeSvgPeerRewrittenOriginalRebuildsCodedCopies) {
  RunScenario(/*rotate_dedup_set=*/false);
}

TEST_F(WorkerPeerRewriteTest,
       NativeSvgPeerRewriteUnchangedBytesRestampsWithoutRecompress) {
  RunScenario(/*rotate_dedup_set=*/false, /*same_bytes=*/true);
}

TEST_F(WorkerPeerRewriteTest,
       NativeSvgPeerRewriteAfterDedupRotationReadsFreshOriginal) {
  RunScenario(/*rotate_dedup_set=*/true);
}

TEST_F(WorkerPeerRewriteTest, DedupHitWithUnchangedOriginalDoesNotRecheck) {
  WorkerConfig config;
  config.socket_path = socket_path_;
  config.cache_path = cache_path_;
  config.cache_size_bytes = static_cast<uint64_t>(10 * 1024 * 1024);
  config.proactive_image_variants = false;
  config.proactive_viewport_variants = false;
  config.proactive_savedata_variants = false;
  config.proactive_density_variants = false;
  PeerRig rig;
  rig.worker = std::make_unique<Worker>(config, &rig.handler);
  Worker& worker = *rig.worker;
  ASSERT_TRUE(worker.Initialize());
  PageSpeedCacheConfig fe_config;
  fe_config.volume_path = cache_path_;
  fe_config.volume_size = static_cast<uint64_t>(10 * 1024 * 1024);
  auto fe = PageSpeedCache::Create(fe_config);
  ASSERT_TRUE(fe.has_value());
  CacheDurableOriginal(fe->get(), rig.url, SvgBody("blue"), "image/svg+xml",
                       ContentType::kImage, kStamp1, 600);
  rig.thread = std::thread([&worker]() { worker.Run(); });
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  NotifySvgAndDrain(worker, rig.url);
  ASSERT_TRUE(WaitForAtLeast(worker.stats().gzip_variants_written, 1));
  WaitForWorkerIdle(worker);

  // No re-record: the second notification is the ordinary dedup skip and
  // must not read or hash the original.
  const uint64_t skipped = worker.stats().notifications_skipped_dedup.load();
  NotifySvgAndDrain(worker, rig.url);
  ASSERT_TRUE(
      WaitForAtLeast(worker.stats().notifications_skipped_dedup, skipped + 1));
  EXPECT_EQ(worker.stats().notifications_origin_rechecked.load(), 0u);
  EXPECT_EQ(worker.stats().origin_change_rebuilt.load(), 0u);
}

TEST_F(WorkerPeerRewriteTest, RecheckIsRateLimitedButAChangeIsNeverDeferred) {
  WorkerConfig config;
  config.socket_path = socket_path_;
  config.cache_path = cache_path_;
  config.cache_size_bytes = static_cast<uint64_t>(10 * 1024 * 1024);
  config.proactive_image_variants = false;
  config.proactive_viewport_variants = false;
  config.proactive_savedata_variants = false;
  config.proactive_density_variants = false;
  PeerRig rig;
  rig.worker = std::make_unique<Worker>(config, &rig.handler);
  Worker& worker = *rig.worker;
  ASSERT_TRUE(worker.Initialize());
  PageSpeedCacheConfig fe_config;
  fe_config.volume_path = cache_path_;
  fe_config.volume_size = static_cast<uint64_t>(10 * 1024 * 1024);
  auto fe = PageSpeedCache::Create(fe_config);
  ASSERT_TRUE(fe.has_value());
  PageSpeedCacheConfig reader_config = fe_config;
  reader_config.ram_cache_size = 0;
  auto reader = PageSpeedCache::Create(reader_config);
  ASSERT_TRUE(reader.has_value());
  const std::string v1 = SvgBody("blue");
  CacheDurableOriginal(fe->get(), rig.url, v1, "image/svg+xml",
                       ContentType::kImage, kStamp1, 600);
  rig.thread = std::thread([&worker]() { worker.Run(); });
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  NotifySvgAndDrain(worker, rig.url);
  ASSERT_TRUE(WaitForAtLeast(worker.stats().gzip_variants_written, 1));
  WaitForWorkerIdle(worker);

  // Re-record 1: same bytes, newer stamp -> restamped (opens the window).
  CacheDurableOriginal(fe->get(), rig.url, v1, "image/svg+xml",
                       ContentType::kImage, kStamp1 + 5, 600);
  NotifySvgAndDrain(worker, rig.url);
  ASSERT_TRUE(WaitForAtLeast(worker.stats().origin_unchanged_restamped, 1));
  WaitForWorkerIdle(worker);

  // Re-record 2, inside the window, same bytes: looked at, not rewritten.
  CacheDurableOriginal(fe->get(), rig.url, v1, "image/svg+xml",
                       ContentType::kImage, kStamp1 + 10, 600);
  NotifySvgAndDrain(worker, rig.url);
  ASSERT_TRUE(WaitForAtLeast(worker.stats().notifications_origin_rechecked, 2));
  WaitForWorkerIdle(worker);
  EXPECT_EQ(worker.stats().origin_unchanged_restamped.load(), 1u);

  // Re-record 3, still inside the window, NEW bytes: rebuilt regardless.
  const std::string v2 = SvgBody("red");
  CacheDurableOriginal(fe->get(), rig.url, v2, "image/svg+xml",
                       ContentType::kImage, kStamp1 + 15, 600);
  NotifySvgAndDrain(worker, rig.url);
  ASSERT_TRUE(WaitForAtLeast(worker.stats().origin_change_rebuilt, 1));
  CapabilityMask gz_mask;
  gz_mask.set_transfer_encoding(CapabilityMask::TransferEncoding::kGzip);
  for (int i = 0; i < 40 * kSanitizerBudgetScale; ++i) {
    if (GzipDecompress(ReadVariant(reader->get(), rig.url, gz_mask)) == v2) {
      break;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
  }
  EXPECT_EQ(GzipDecompress(ReadVariant(reader->get(), rig.url, gz_mask)), v2);
}

TEST_F(WorkerPeerRewriteTest, SvgOracleMakesTheAgeExpirySentinelDefer) {
  WorkerConfig config;
  config.socket_path = socket_path_;
  config.cache_path = cache_path_;
  config.cache_size_bytes = static_cast<uint64_t>(10 * 1024 * 1024);
  config.proactive_image_variants = false;
  config.proactive_viewport_variants = false;
  config.proactive_savedata_variants = false;
  config.proactive_density_variants = false;
  PeerRig rig;
  rig.worker = std::make_unique<Worker>(config, &rig.handler);
  Worker& worker = *rig.worker;
  ASSERT_TRUE(worker.Initialize());
  PageSpeedCacheConfig fe_config;
  fe_config.volume_path = cache_path_;
  fe_config.volume_size = static_cast<uint64_t>(10 * 1024 * 1024);
  auto fe = PageSpeedCache::Create(fe_config);
  ASSERT_TRUE(fe.has_value());
  CacheDurableOriginal(fe->get(), rig.url, SvgBody("blue"), "image/svg+xml",
                       ContentType::kImage, kStamp1, 600);
  rig.thread = std::thread([&worker]() { worker.Run(); });
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  NotifySvgAndDrain(worker, rig.url);
  ASSERT_TRUE(WaitForAtLeast(worker.stats().gzip_variants_written, 1));
  WaitForWorkerIdle(worker);

  // The sentinel arrives before any re-record (the age-expiry shape).  The
  // SVG now has a content-hash record, so it defers instead of purging.
  CacheNotification refresh;
  refresh.url = rig.url;
  refresh.scheme = "https";
  refresh.content_type = ContentType::kImage;
  refresh.capability_mask = kOriginRefreshedSentinel;
  SendNotification(refresh);
  ASSERT_TRUE(WaitForAtLeast(worker.stats().origin_refresh_deferred, 1));
  WaitForWorkerIdle(worker);
  EXPECT_EQ(worker.stats().origin_refresh_purges.load(), 0u);
}

// The same peer-rewrite scenarios for the other types that record a content
// hash of their source: CSS, JS, unsupported image types, and raster images.
class WorkerPeerRewriteTypesTest : public WorkerPeerRewriteTest {
 protected:
  struct Rig {
    NullMessageHandler handler;
    std::unique_ptr<Worker> worker;
    std::unique_ptr<PageSpeedCache> front_end;
    std::unique_ptr<PageSpeedCache> reader;
    std::thread thread;
    ~Rig() {
      if (worker) worker->Shutdown();
      if (thread.joinable()) thread.join();
    }
  };

  void StartRig(Rig& rig, int gzip_level = 6, int brotli_level = 6) {
    WorkerConfig config;
    config.gzip_level = gzip_level;
    config.brotli_level = brotli_level;
    config.socket_path = socket_path_;
    config.cache_path = cache_path_;
    config.cache_size_bytes = static_cast<uint64_t>(10 * 1024 * 1024);
    config.proactive_image_variants = false;
    config.proactive_viewport_variants = false;
    config.proactive_savedata_variants = false;
    config.proactive_density_variants = false;
    rig.worker = std::make_unique<Worker>(config, &rig.handler);
    ASSERT_TRUE(rig.worker->Initialize());
    PageSpeedCacheConfig fe_config;
    fe_config.volume_path = cache_path_;
    fe_config.volume_size = static_cast<uint64_t>(10 * 1024 * 1024);
    auto fe = PageSpeedCache::Create(fe_config);
    ASSERT_TRUE(fe.has_value());
    rig.front_end = std::move(*fe);
    PageSpeedCacheConfig reader_config = fe_config;
    reader_config.ram_cache_size = 0;
    auto reader = PageSpeedCache::Create(reader_config);
    ASSERT_TRUE(reader.has_value());
    rig.reader = std::move(*reader);
  }

  void StartWorker(Rig& rig) {
    Worker* worker = rig.worker.get();
    rig.thread = std::thread([worker]() { worker->Run(); });
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
  }

  void Notify(Worker& worker, const std::string& url, ContentType type) {
    CacheNotification notification;
    notification.url = url;
    notification.scheme = "https";
    notification.content_type = type;
    notification.capability_mask = CapabilityMask().Encode();
    SendNotification(notification);
    WaitForWorkerIdle(worker);
  }

  // The content-hash record, read from disk ("" when absent).
  std::string Oracle(Rig& rig, const std::string& url) {
    return ReadSentinel(rig.reader.get(), url, SentinelId::kContentHash);
  }

  // Polls until the on-disk content-hash record differs from `previous`.
  std::string WaitForOracleChange(Rig& rig, const std::string& url,
                                  const std::string& previous) {
    std::string now = Oracle(rig, url);
    for (int i = 0; i < 60 * kSanitizerBudgetScale && now == previous; ++i) {
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
      now = Oracle(rig, url);
    }
    return now;
  }

  // Builds `url` from `v1`, re-records it as `v2` through the second handle,
  // and notifies again.  Returns once the recheck rebuilt and the record
  // moved; the caller judges the stored copies.
  void BuildThenRewrite(Rig& rig, const std::string& url,
                        const std::string& mime, ContentType type,
                        const std::string& v1, const std::string& v2) {
    CacheDurableOriginal(rig.front_end.get(), url, v1, mime, type, kStamp1,
                         600);
    StartWorker(rig);
    Notify(*rig.worker, url, type);
    std::string first = WaitForOracleChange(rig, url, "");
    ASSERT_EQ(first.size(), 32u)
        << "the build must record a hash of the source";
    WaitForWorkerIdle(*rig.worker);

    CacheDurableOriginal(rig.front_end.get(), url, v2, mime, type, kStamp2,
                         600);
    Notify(*rig.worker, url, type);
    ASSERT_TRUE(WaitForAtLeast(rig.worker->stats().origin_change_rebuilt, 1));
    WaitForWorkerIdle(*rig.worker);
    const std::string second = WaitForOracleChange(rig, url, first);
    EXPECT_EQ(second.size(), 32u);
    EXPECT_NE(second, first) << "the record still binds the previous bytes";
    EXPECT_EQ(rig.worker->stats().origin_change_rebuilt.load(), 1u);
    EXPECT_EQ(rig.worker->stats().alternate_writes_fenced.load(), 0u);
  }

  // The same bytes re-recorded with a newer stamp: the copies' freshness is
  // refreshed, and nothing is rebuilt or recompressed.
  void ExpectUnchangedBytesRestamp(const std::string& url,
                                   const std::string& mime, ContentType type,
                                   const std::string& body) {
    Rig rig;
    ASSERT_NO_FATAL_FAILURE(StartRig(rig));
    CacheDurableOriginal(rig.front_end.get(), url, body, mime, type, kStamp1,
                         600);
    StartWorker(rig);
    Notify(*rig.worker, url, type);
    ASSERT_EQ(WaitForOracleChange(rig, url, "").size(), 32u);
    WaitForWorkerIdle(*rig.worker);
    const uint64_t gz_before = rig.worker->stats().gzip_variants_written.load();
    const uint64_t written_before = rig.worker->stats().variants_written.load();

    CacheDurableOriginal(rig.front_end.get(), url, body, mime, type, kStamp2,
                         600);
    Notify(*rig.worker, url, type);
    ASSERT_TRUE(
        WaitForAtLeast(rig.worker->stats().origin_unchanged_restamped, 1));
    WaitForWorkerIdle(*rig.worker);
    EXPECT_EQ(rig.worker->stats().origin_change_rebuilt.load(), 0u);
    EXPECT_EQ(rig.worker->stats().gzip_variants_written.load(), gz_before);
    EXPECT_EQ(rig.worker->stats().variants_written.load(), written_before);
    CapabilityMask gz_mask;
    gz_mask.set_transfer_encoding(CapabilityMask::TransferEncoding::kGzip);
    auto gz = rig.reader->ReadAlternate(
        StripSchemeAuthority(url), "", "https",
        MaskToAlternateId(static_cast<uint8_t>(gz_mask.Encode() & 0xFF)));
    ASSERT_TRUE(gz.has_value());
    EXPECT_EQ(gz->metadata.cache_inserted_at, kStamp2);
  }

  std::string Coded(Rig& rig, const std::string& url,
                    CapabilityMask::TransferEncoding encoding) {
    CapabilityMask mask;
    mask.set_transfer_encoding(encoding);
    std::string body = ReadVariant(rig.reader.get(), url, mask);
    return encoding == CapabilityMask::TransferEncoding::kGzip
               ? GzipDecompress(body)
               : BrotliDecompress(body);
  }

  // Polls (bounded) until both coded copies decode to something containing
  // `needle`.
  bool WaitForCodedContaining(Rig& rig, const std::string& url,
                              const std::string& needle) {
    for (int i = 0; i < 40 * kSanitizerBudgetScale; ++i) {
      if (Coded(rig, url, CapabilityMask::TransferEncoding::kGzip)
                  .find(needle) != std::string::npos &&
          Coded(rig, url, CapabilityMask::TransferEncoding::kBrotli)
                  .find(needle) != std::string::npos) {
        return true;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    return false;
  }
};

TEST_F(WorkerPeerRewriteTypesTest,
       CssPeerRewriteRebuildsMinifiedAndCodedCopies) {
  Rig rig;
  ASSERT_NO_FATAL_FAILURE(StartRig(rig));
  const std::string url = "http://example.com/css/site.css";
  ASSERT_NO_FATAL_FAILURE(
      BuildThenRewrite(rig, url, "text/css", ContentType::kCss,
                       "a {\n  color : red ;\n}\n\n/* first */\n",
                       "a {\n  color : blue ;\n}\n\n/* second */\n"));
  ASSERT_TRUE(WaitForCodedContaining(rig, url, "blue"));
  for (auto enc : {CapabilityMask::TransferEncoding::kGzip,
                   CapabilityMask::TransferEncoding::kBrotli}) {
    const std::string body = Coded(rig, url, enc);
    EXPECT_EQ(body.find("red"), std::string::npos) << body;
    EXPECT_EQ(body.find("second"), std::string::npos)
        << "the coded copy of a minified sheet must not carry comments";
  }
  EXPECT_NE(ReadVariant(rig.reader.get(), url, CapabilityMask()).find("blue"),
            std::string::npos);
}

TEST_F(WorkerPeerRewriteTypesTest,
       JsPeerRewriteRebuildsMinifiedAndCodedCopies) {
  Rig rig;
  ASSERT_NO_FATAL_FAILURE(StartRig(rig));
  const std::string url = "http://example.com/js/app.js";
  ASSERT_NO_FATAL_FAILURE(BuildThenRewrite(
      rig, url, "application/javascript", ContentType::kJs,
      "function first ( value ) {\n  return value + 1 ;\n}\n",
      "function second ( value ) {\n  return value + 2 ;\n}\n"));
  ASSERT_TRUE(WaitForCodedContaining(rig, url, "second"));
  for (auto enc : {CapabilityMask::TransferEncoding::kGzip,
                   CapabilityMask::TransferEncoding::kBrotli}) {
    EXPECT_EQ(Coded(rig, url, enc).find("first"), std::string::npos);
  }
  EXPECT_NE(ReadVariant(rig.reader.get(), url, CapabilityMask()).find("second"),
            std::string::npos);
}

TEST_F(WorkerPeerRewriteTypesTest,
       CssRewrittenToAlreadyMinimalRemovesTheOldMinifiedCopy) {
  Rig rig;
  ASSERT_NO_FATAL_FAILURE(StartRig(rig));
  const std::string url = "http://example.com/css/minimal.css";
  const std::string v2 = "a{color:blue}";
  CacheDurableOriginal(rig.front_end.get(), url,
                       "a {\n  color : red ;\n}\n\n/* first */\n", "text/css",
                       ContentType::kCss, kStamp1, 600);
  StartWorker(rig);
  Notify(*rig.worker, url, ContentType::kCss);
  const std::string first = WaitForOracleChange(rig, url, "");
  ASSERT_EQ(first.size(), 32u);
  WaitForWorkerIdle(*rig.worker);
  // The first build stored a worker-minified copy in the identity slot.
  ASSERT_FALSE(ReadVariant(rig.reader.get(), url, CapabilityMask()).empty());

  CacheDurableOriginal(rig.front_end.get(), url, v2, "text/css",
                       ContentType::kCss, kStamp2, 600);
  Notify(*rig.worker, url, ContentType::kCss);
  ASSERT_TRUE(WaitForAtLeast(rig.worker->stats().origin_change_rebuilt, 1));
  WaitForWorkerIdle(*rig.worker);
  ASSERT_NE(WaitForOracleChange(rig, url, first), first);
  WaitForWorkerIdle(*rig.worker);

  // The new bytes are already minimal: no minified copy is written, and the
  // old one (the identity slot) must be gone, not left to be served.
  EXPECT_EQ(ReadVariant(rig.reader.get(), url, CapabilityMask()), "")
      << "the previous minified copy survived the rebuild";
  ASSERT_TRUE(WaitForCodedContaining(rig, url, v2));
  EXPECT_EQ(Coded(rig, url, CapabilityMask::TransferEncoding::kGzip), v2);
  EXPECT_EQ(Coded(rig, url, CapabilityMask::TransferEncoding::kBrotli), v2);
}

TEST_F(WorkerPeerRewriteTypesTest,
       UnsupportedImageTypePeerRewriteRebuildsCodedCopies) {
  Rig rig;
  ASSERT_NO_FATAL_FAILURE(StartRig(rig));
  const std::string url = "http://example.com/favicon.ico";
  const std::string v1 = std::string("icon-one-") + std::string(300, 'a');
  const std::string v2 = std::string("icon-two-") + std::string(300, 'b');
  ASSERT_NO_FATAL_FAILURE(
      BuildThenRewrite(rig, url, "image/x-icon", ContentType::kImage, v1, v2));
  ASSERT_TRUE(WaitForCodedContaining(rig, url, "icon-two-"));
  EXPECT_EQ(Coded(rig, url, CapabilityMask::TransferEncoding::kGzip), v2);
  EXPECT_EQ(Coded(rig, url, CapabilityMask::TransferEncoding::kBrotli), v2);
}

TEST_F(WorkerPeerRewriteTypesTest, UnchangedCssBytesRestampWithoutRebuild) {
  ExpectUnchangedBytesRestamp("http://example.com/css/same.css", "text/css",
                              ContentType::kCss,
                              "a {\n  color : red ;\n}\n\n/* note */\n");
}

TEST_F(WorkerPeerRewriteTypesTest, UnchangedJsBytesRestampWithoutRebuild) {
  ExpectUnchangedBytesRestamp(
      "http://example.com/js/same.js", "application/javascript",
      ContentType::kJs, "function same ( value ) {\n  return value + 1 ;\n}\n");
}

TEST_F(WorkerPeerRewriteTypesTest,
       UnchangedUnsupportedImageBytesRestampWithoutRebuild) {
  ExpectUnchangedBytesRestamp("http://example.com/favicon.ico", "image/x-icon",
                              ContentType::kImage,
                              std::string("icon-") + std::string(300, 'a'));
}

// A disabled encoding is never written, so it must not keep the source hash
// from being recorded: without the hash every origin change purges and
// rebuilds instead of being told apart from a mere re-record.
TEST_F(WorkerPeerRewriteTypesTest, HashIsRecordedWithGzipDisabled) {
  Rig rig;
  ASSERT_NO_FATAL_FAILURE(StartRig(rig, /*gzip_level=*/0, /*brotli_level=*/6));
  const std::string url = "http://example.com/css/nogzip.css";
  ASSERT_NO_FATAL_FAILURE(
      BuildThenRewrite(rig, url, "text/css", ContentType::kCss,
                       "a {\n  color : red ;\n}\n\n/* first */\n",
                       "a {\n  color : blue ;\n}\n\n/* second */\n"));
  EXPECT_EQ(Coded(rig, url, CapabilityMask::TransferEncoding::kGzip), "");
  ASSERT_TRUE(WaitForAtLeast(rig.worker->stats().brotli_variants_written, 1));
}

TEST_F(WorkerPeerRewriteTypesTest, HashIsRecordedWithBrotliDisabled) {
  Rig rig;
  ASSERT_NO_FATAL_FAILURE(StartRig(rig, /*gzip_level=*/6, /*brotli_level=*/0));
  const std::string url = "http://example.com/js/nobrotli.js";
  ASSERT_NO_FATAL_FAILURE(BuildThenRewrite(
      rig, url, "application/javascript", ContentType::kJs,
      "function first ( value ) {\n  return value + 1 ;\n}\n",
      "function second ( value ) {\n  return value + 2 ;\n}\n"));
  EXPECT_EQ(rig.worker->stats().brotli_variants_written.load(), 0u);
  ASSERT_TRUE(WaitForAtLeast(rig.worker->stats().gzip_variants_written, 1));
}

TEST_F(WorkerPeerRewriteTypesTest, HashIsRecordedWithBothEncodingsDisabled) {
  // The minified identity copy is the only stored copy left to keep honest.
  Rig rig;
  ASSERT_NO_FATAL_FAILURE(StartRig(rig, /*gzip_level=*/0, /*brotli_level=*/0));
  const std::string url = "http://example.com/css/nocoding.css";
  ASSERT_NO_FATAL_FAILURE(
      BuildThenRewrite(rig, url, "text/css", ContentType::kCss,
                       "a {\n  color : red ;\n}\n\n/* first */\n",
                       "a {\n  color : blue ;\n}\n\n/* second */\n"));
  EXPECT_EQ(rig.worker->stats().gzip_variants_written.load(), 0u);
  EXPECT_EQ(rig.worker->stats().brotli_variants_written.load(), 0u);
  EXPECT_NE(ReadVariant(rig.reader.get(), url, CapabilityMask()).find("blue"),
            std::string::npos);
}

TEST_F(WorkerPeerRewriteTypesTest, UnchangedBytesRestampWithGzipDisabled) {
  // Same bytes, newer stamp, no gzip copy: still told apart from a change.
  Rig rig;
  ASSERT_NO_FATAL_FAILURE(StartRig(rig, /*gzip_level=*/0, /*brotli_level=*/6));
  const std::string url = "http://example.com/css/nogzip-same.css";
  const std::string body = "a {\n  color : red ;\n}\n\n/* note */\n";
  CacheDurableOriginal(rig.front_end.get(), url, body, "text/css",
                       ContentType::kCss, kStamp1, 600);
  StartWorker(rig);
  Notify(*rig.worker, url, ContentType::kCss);
  ASSERT_EQ(WaitForOracleChange(rig, url, "").size(), 32u);
  WaitForWorkerIdle(*rig.worker);
  CacheDurableOriginal(rig.front_end.get(), url, body, "text/css",
                       ContentType::kCss, kStamp2, 600);
  Notify(*rig.worker, url, ContentType::kCss);
  ASSERT_TRUE(
      WaitForAtLeast(rig.worker->stats().origin_unchanged_restamped, 1));
  WaitForWorkerIdle(*rig.worker);
  EXPECT_EQ(rig.worker->stats().origin_change_rebuilt.load(), 0u);
}

// The unsupported-image "already stored" check expects only the coded copies
// the configuration enables.  Each test starts a fresh worker, so its
// in-memory dedup set is empty (as after a restart), stores the coded copies
// beforehand and sends one notification.
void WriteStoredCodedCopy(Worker& worker, const std::string& url,
                          CapabilityMask::TransferEncoding encoding) {
  CapabilityMask mask;
  mask.set_transfer_encoding(encoding);
  AlternateId id =
      MaskToAlternateId(static_cast<uint8_t>(mask.Encode() & 0xFF));
  AlternateMetadata meta;
  meta.content_type = ContentType::kImage;
  meta.flags = AlternateMetadata::kFlagWorkerProcessed;
  meta.full_mask = mask.Encode();
  const std::string content = "fake coded ico data";
  auto wh =
      worker.cache()->WriteAlternate(WorkerTest::StripSchemeAuthority(url), "",
                                     "https", id, content.size(), meta);
  ASSERT_TRUE(wh.has_value()) << "Failed to write the coded copy";
  auto written =
      wh->write_sync(std::as_bytes(std::span(content.data(), content.size())));
  ASSERT_TRUE(written.has_value()) << "Failed to write the coded content";
  auto closed = wh->close_sync();
  ASSERT_TRUE(closed.has_value()) << "Failed to close the coded write handle";
}

TEST_F(WorkerPeerRewriteTypesTest,
       UnsupportedImageStoredWithGzipDisabledSkips) {
  Rig rig;
  ASSERT_NO_FATAL_FAILURE(StartRig(rig, /*gzip_level=*/0, /*brotli_level=*/6));
  const std::string url = "http://example.com/nogzip.ico";
  CacheDurableOriginal(rig.front_end.get(), url,
                       std::string("icon-") + std::string(300, 'a'),
                       "image/x-icon", ContentType::kImage, kStamp1, 600);
  ASSERT_NO_FATAL_FAILURE(WriteStoredCodedCopy(
      *rig.worker, url, CapabilityMask::TransferEncoding::kBrotli));
  StartWorker(rig);
  Notify(*rig.worker, url, ContentType::kImage);
  ASSERT_TRUE(pagespeed::test::WaitForNotificationsRetired(*rig.worker, 1,
                                                           kWorkerStallBudget));
  EXPECT_EQ(rig.worker->stats().alternate_writes.load(), 0u)
      << "the stored brotli copy must end the work when gzip is disabled";
  EXPECT_EQ(rig.worker->stats().brotli_variants_written.load(), 0u);
  EXPECT_EQ(rig.worker->stats().gzip_variants_written.load(), 0u);
}

TEST_F(WorkerPeerRewriteTypesTest,
       UnsupportedImageStoredWithBrotliDisabledSkips) {
  Rig rig;
  ASSERT_NO_FATAL_FAILURE(StartRig(rig, /*gzip_level=*/6, /*brotli_level=*/0));
  const std::string url = "http://example.com/nobrotli.ico";
  CacheDurableOriginal(rig.front_end.get(), url,
                       std::string("icon-") + std::string(300, 'a'),
                       "image/x-icon", ContentType::kImage, kStamp1, 600);
  ASSERT_NO_FATAL_FAILURE(WriteStoredCodedCopy(
      *rig.worker, url, CapabilityMask::TransferEncoding::kGzip));
  StartWorker(rig);
  Notify(*rig.worker, url, ContentType::kImage);
  ASSERT_TRUE(pagespeed::test::WaitForNotificationsRetired(*rig.worker, 1,
                                                           kWorkerStallBudget));
  EXPECT_EQ(rig.worker->stats().alternate_writes.load(), 0u)
      << "the stored gzip copy must end the work when brotli is disabled";
  EXPECT_EQ(rig.worker->stats().gzip_variants_written.load(), 0u);
  EXPECT_EQ(rig.worker->stats().brotli_variants_written.load(), 0u);
}

TEST_F(WorkerPeerRewriteTypesTest,
       UnsupportedImageWithBothEnabledNeedsBothCopies) {
  Rig rig;
  ASSERT_NO_FATAL_FAILURE(StartRig(rig));
  const std::string url = "http://example.com/both.ico";
  const std::string body = std::string("icon-") + std::string(300, 'a');
  CacheDurableOriginal(rig.front_end.get(), url, body, "image/x-icon",
                       ContentType::kImage, kStamp1, 600);
  // Only the brotli copy is stored: with both enabled the gzip copy is still
  // missing, so the notification must write it.
  ASSERT_NO_FATAL_FAILURE(WriteStoredCodedCopy(
      *rig.worker, url, CapabilityMask::TransferEncoding::kBrotli));
  StartWorker(rig);
  Notify(*rig.worker, url, ContentType::kImage);
  ASSERT_TRUE(pagespeed::test::WaitForNotificationsRetired(*rig.worker, 1,
                                                           kWorkerStallBudget));
  ASSERT_TRUE(WaitForAtLeast(rig.worker->stats().gzip_variants_written, 1));
  EXPECT_EQ(Coded(rig, url, CapabilityMask::TransferEncoding::kGzip), body);
}

TEST_F(WorkerPeerRewriteTypesTest,
       StylesheetsEmbeddingImportsKeepNoHashAndHealAtTheSentinel) {
  // A sheet whose output includes other files (a resolved @import, or an
  // import that was skipped as unresolved) is not bound to its own bytes: an
  // unchanged main sheet must not restamp over a changed child, so the
  // sentinel purges as it always did.
  struct Case {
    const char* url;
    bool child_cached;
  };
  const Case cases[] = {{"http://example.com/css/flat.css", true},
                        {"http://example.com/css/skipped.css", false}};
  for (const Case& c : cases) {
    SCOPED_TRACE(c.url);
    Rig rig;
    ASSERT_NO_FATAL_FAILURE(StartRig(rig));
    if (c.child_cached) {
      CacheOriginal(rig.front_end.get(), "http://example.com/css/child.css",
                    "h1  {  color:  blue;  }\n");
    }
    CacheDurableOriginal(
        rig.front_end.get(), c.url,
        c.child_cached ? "@import url(\"http://example.com/css/child.css\");\n"
                         "p  {  font-size:  16px;  }\n"
                       : "@import url(\"http://example.com/css/absent.css\");\n"
                         "p  {  font-size:  16px;  }\n",
        "text/css", ContentType::kCss, kStamp1, 600);
    StartWorker(rig);
    Notify(*rig.worker, c.url, ContentType::kCss);
    ASSERT_TRUE(WaitForAtLeast(rig.worker->stats().gzip_variants_written, 1));
    ASSERT_TRUE(WaitForAtLeast(rig.worker->stats().brotli_variants_written, 1));
    WaitForWorkerIdle(*rig.worker);
    EXPECT_EQ(Oracle(rig, c.url), "");

    // Same main sheet re-recorded, then the sentinel: purged, not restamped.
    CacheDurableOriginal(
        rig.front_end.get(), c.url,
        c.child_cached ? "@import url(\"http://example.com/css/child.css\");\n"
                         "p  {  font-size:  16px;  }\n"
                       : "@import url(\"http://example.com/css/absent.css\");\n"
                         "p  {  font-size:  16px;  }\n",
        "text/css", ContentType::kCss, kStamp2, 600);
    CacheNotification refresh;
    refresh.url = c.url;
    refresh.scheme = "https";
    refresh.content_type = ContentType::kCss;
    refresh.capability_mask = kOriginRefreshedSentinel;
    SendNotification(refresh);
    ASSERT_TRUE(WaitForAtLeast(rig.worker->stats().origin_refresh_purges, 1));
    WaitForWorkerIdle(*rig.worker);
    EXPECT_EQ(rig.worker->stats().origin_refresh_purges.load(), 1u);
    EXPECT_EQ(rig.worker->stats().origin_refresh_unchanged.load(), 0u);
  }
}

TEST_F(WorkerPeerRewriteTypesTest,
       RasterPeerRewriteReachesTheContentHashRedecide) {
  Rig rig;
  ASSERT_NO_FATAL_FAILURE(StartRig(rig));
  const std::string url = "http://example.com/images/photo.jpg";
  const std::string v1 = ReadTestFile("jpeg/quality100.jpg");
  const std::string v2 = ReadTestFile("jpeg/progressive.jpg");
  ASSERT_FALSE(v1.empty());
  ASSERT_FALSE(v2.empty());
  ASSERT_NE(v1, v2);
  ASSERT_NO_FATAL_FAILURE(
      BuildThenRewrite(rig, url, "image/jpeg", ContentType::kImage, v1, v2));
}

TEST_F(WorkerPeerRewriteTypesTest, NoOracleIsWrittenWithoutAPristineReference) {
  // A source that is not a front end's own recording (it carries the worker's
  // flag) must not seed the change record: it could not prove "same bytes"
  // later.  Both a text type and an unsupported image type.
  struct Case {
    const char* url;
    const char* mime;
    ContentType type;
    std::string body;
  };
  const Case cases[] = {
      {"http://example.com/css/derived.css", "text/css", ContentType::kCss,
       "a {\n  color : red ;\n}\n"},
      {"http://example.com/derived.ico", "image/x-icon", ContentType::kImage,
       std::string("icon-") + std::string(300, 'a')},
  };
  for (const Case& c : cases) {
    SCOPED_TRACE(c.url);
    Rig rig;
    ASSERT_NO_FATAL_FAILURE(StartRig(rig));
    CacheDurableOriginal(rig.front_end.get(), c.url, c.body, c.mime, c.type,
                         kStamp1, 600, "",
                         AlternateMetadata::kFlagWorkerProcessed);
    StartWorker(rig);
    Notify(*rig.worker, c.url, c.type);
    // Both coded copies are written, so the record would be due by now.
    ASSERT_TRUE(WaitForAtLeast(rig.worker->stats().gzip_variants_written, 1));
    ASSERT_TRUE(WaitForAtLeast(rig.worker->stats().brotli_variants_written, 1));
    WaitForWorkerIdle(*rig.worker);
    EXPECT_EQ(Oracle(rig, c.url), "");
  }
}

TEST_F(WorkerPeerRewriteTypesTest,
       OriginRefreshedSentinelAfterAChangedSvgRebuildsOnce) {
  Rig rig;
  ASSERT_NO_FATAL_FAILURE(StartRig(rig));
  const std::string url = "http://example.com/images/logo.svg";
  const std::string v1 = SvgBody("blue");
  const std::string v2 = SvgBody("red");
  CacheDurableOriginal(rig.front_end.get(), url, v1, "image/svg+xml",
                       ContentType::kImage, kStamp1, 600);
  StartWorker(rig);
  Notify(*rig.worker, url, ContentType::kImage);
  const std::string first = WaitForOracleChange(rig, url, "");
  ASSERT_EQ(first.size(), 32u);
  WaitForWorkerIdle(*rig.worker);

  // Re-record with new bytes, then deliver the origin-refreshed sentinel
  // instead of the plain notification.
  CacheDurableOriginal(rig.front_end.get(), url, v2, "image/svg+xml",
                       ContentType::kImage, kStamp2, 600);
  CacheNotification refresh;
  refresh.url = url;
  refresh.scheme = "https";
  refresh.content_type = ContentType::kImage;
  refresh.capability_mask = kOriginRefreshedSentinel;
  SendNotification(refresh);
  ASSERT_TRUE(WaitForAtLeast(rig.worker->stats().origin_refresh_purges, 1));
  WaitForWorkerIdle(*rig.worker);
  EXPECT_EQ(rig.worker->stats().origin_refresh_purges.load(), 1u);

  // The sentinel purge also dropped the recorded original, so the front end
  // records again and sends the plain notification, which builds from the new
  // bytes.  The old set is already gone, so the recheck does not fire.
  CacheDurableOriginal(rig.front_end.get(), url, v2, "image/svg+xml",
                       ContentType::kImage, kStamp2 + 5, 600);
  Notify(*rig.worker, url, ContentType::kImage);
  const std::string second = WaitForOracleChange(rig, url, "");
  ASSERT_EQ(second.size(), 32u);
  EXPECT_NE(second, first);
  ASSERT_TRUE(WaitForCodedContaining(rig, url, "red"));
  EXPECT_EQ(Coded(rig, url, CapabilityMask::TransferEncoding::kGzip), v2);
  EXPECT_EQ(Coded(rig, url, CapabilityMask::TransferEncoding::kBrotli), v2);
  EXPECT_EQ(rig.worker->stats().origin_change_rebuilt.load(), 0u);
}

}  // namespace
}  // namespace pagespeed
