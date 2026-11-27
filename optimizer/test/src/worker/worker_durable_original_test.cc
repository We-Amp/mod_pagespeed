// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

// Durable original tests
//
// - The durable original as an optimization source
// - Variant sizes per client against the selection and size gates
//
// One of the topic files of //test/src/worker:worker_test; the shared fixture
// lives in worker_test_fixture.h.

// clang-format off
// Keep the fixture header first: it preserves the old worker_test.cc include
// order (worker.h before any Windows header), which Windows builds have proven.
// See lib/base/message_handler.h for the FormatMessage macro interaction.
#include "test/src/worker/worker_test_fixture.h"
// clang-format on

#include <cstddef>
#include <cstdint>
#include <ctime>
#include <iomanip>
#include <iostream>
#include <map>
#include <optional>
#include <string>
#include <thread>

#include "avif/avif.h"
#include "gtest/gtest.h"
#include "lib/base/message_handler.h"
#include "lib/cache/cache.h"
#include "lib/cache/freshness.h"
#include "lib/classify/alternate_id.h"
#include "lib/classify/alternate_metadata.h"
#include "lib/classify/capability_mask.h"
#include "src/proto/worker_ipc.h"
#include "src/worker/browser_analysis_manager.h"
#include "src/worker/critical_css_extractor.h"
#include "src/worker/worker.h"
#include "test/test_util/worker_quiescence.h"

namespace pagespeed {
namespace {

// =============================================================================
// The durable original as an OPTIMIZATION SOURCE (issue #1323).
//
// The durable-original class (SentinelId::kOriginalContent, 0x0C) is never
// SELECTABLE: PageSpeedSelector::select skips it unconditionally so that "the
// original" can never compete with the optimized variants at serve time.  The
// worker's optimization-source read went through that same selector, so a URL
// whose only entry was a durable original produced nothing — an external
// writer using the published C ABI (ps_cache_write_original) that recorded the
// origin's bytes into the class and notified correctly got a cache that filled
// and an optimizer that never ran, silently.
//
// The worker's SOURCE read now falls back to an exact-id read of the class.
// These tests pin both halves: the fallback works for every content-type
// branch, and the serving guarantee it falls back from is untouched.
// =============================================================================

// THE regression test.  Only entry for the URL is a durable original; the
// worker must still produce an optimized variant.  Red before the fallback:
// the selector skips the class, so the source read missed and the CSS branch
// returned at "Original CSS not found in cache".
TEST_F(WorkerTest, CssNotificationOptimizesFromADurableOriginalAlone) {
  WorkerConfig config;
  config.socket_path = socket_path_;
  config.cache_path = cache_path_;
  config.cache_size_bytes = static_cast<uint64_t>(10 * 1024 * 1024);

  NullMessageHandler handler;
  Worker worker(config, &handler);
  ASSERT_TRUE(worker.Initialize());

  const std::string url = "http://example.com/original-only.css";
  const std::string css =
      "body  {  margin:  0;  padding:  0;  }\n"
      "h1  {  color:  red;  font-size:  24px;  }\n";
  CacheDurableOriginal(worker.cache(), url, css, "text/css", ContentType::kCss);

  // Precondition, and the reason the fix is needed: ordinary selection cannot
  // see this URL at all.  If this ever starts returning content, the serving
  // guarantee has been broken and the rest of the test is meaningless.
  ASSERT_TRUE(ReadBestVariant(worker.cache(), url, CapabilityMask()).empty())
      << "the durable original must not be reachable by ordinary selection";

  std::thread worker_thread([&worker]() { worker.Run(); });
  WorkerStopper stopper(worker, worker_thread);
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  CacheNotification notification;
  notification.url = url;
  notification.scheme = "https";
  notification.content_type = ContentType::kCss;
  notification.capability_mask = CapabilityMask().Encode();
  SendNotification(notification);

  std::string variant;
  for (int i = 0; i < 60 * kSanitizerBudgetScale; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    variant = ReadVariant(worker.cache(), url, CapabilityMask());
    if (!variant.empty()) break;
  }

  EXPECT_FALSE(variant.empty())
      << "a URL whose only entry is a durable original yielded no variant";
  EXPECT_LT(variant.size(), css.size());
  EXPECT_NE(variant.find("margin"), std::string::npos);
  EXPECT_NE(variant.find("color"), std::string::npos);
  EXPECT_GE(worker.stats().css_processed.load(), 1u);
  EXPECT_GE(worker.stats().source_reads_from_durable_original.load(), 1u)
      << "the fallback counter must make this path observable";
}

TEST_F(WorkerTest, JsNotificationOptimizesFromADurableOriginalAlone) {
  WorkerConfig config;
  config.socket_path = socket_path_;
  config.cache_path = cache_path_;
  config.cache_size_bytes = static_cast<uint64_t>(10 * 1024 * 1024);

  NullMessageHandler handler;
  Worker worker(config, &handler);
  ASSERT_TRUE(worker.Initialize());

  const std::string url = "http://example.com/original-only.js";
  const std::string js =
      "// Main application script\n"
      "function  hello( name )  {\n"
      "  var  greeting  =  'Hello, '  +  name;\n"
      "  return  greeting;\n"
      "}\n";
  CacheDurableOriginal(worker.cache(), url, js, "application/javascript",
                       ContentType::kJs);

  std::thread worker_thread([&worker]() { worker.Run(); });
  WorkerStopper stopper(worker, worker_thread);
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  CacheNotification notification;
  notification.url = url;
  notification.scheme = "https";
  notification.content_type = ContentType::kJs;
  notification.capability_mask = CapabilityMask().Encode();
  SendNotification(notification);

  for (int i = 0; i < 60 * kSanitizerBudgetScale; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    if (worker.stats().js_processed.load() >= 1) break;
  }
  EXPECT_GE(worker.stats().js_processed.load(), 1u)
      << "JS notification for a durable-original-only URL produced nothing";
  EXPECT_FALSE(ReadVariant(worker.cache(), url, CapabilityMask()).empty());
}

TEST_F(WorkerTest, HtmlNotificationOptimizesFromADurableOriginalAlone) {
  WorkerConfig config;
  config.socket_path = socket_path_;
  config.cache_path = cache_path_;
  config.cache_size_bytes = static_cast<uint64_t>(10 * 1024 * 1024);

  NullMessageHandler handler;
  config.unsafe_force_async_css =
      true;  // fixture page is not under test for the gate
  Worker worker(config, &handler);
  ASSERT_TRUE(worker.Initialize());

  const std::string cache_url = "/original-only.html";
  const std::string notification_url = "http://example.com/original-only.html";
  const std::string html =
      "<html><head><style>body { margin: 0; } "
      "h1 { color: red; }</style></head>"
      "<body><h1>Hello World</h1></body></html>";
  CacheDurableOriginal(worker.cache(), cache_url, html, "text/html",
                       ContentType::kHtml);

  std::thread worker_thread([&worker]() { worker.Run(); });
  WorkerStopper stopper(worker, worker_thread);
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  CacheNotification notification;
  notification.url = notification_url;
  notification.scheme = "https";
  notification.content_type = ContentType::kHtml;
  notification.capability_mask = 0;
  SendNotification(notification);

  CapabilityMask mask = CapabilityMask::Decode(0);
  std::string variant;
  for (int i = 0; i < 60 * kSanitizerBudgetScale; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    variant = ReadVariant(worker.cache(), cache_url, mask);
    if (!variant.empty()) break;
  }

  ASSERT_FALSE(variant.empty())
      << "HTML notification for a durable-original-only URL produced nothing";
  EXPECT_NE(variant.find("data-pagespeed-critical"), std::string::npos);
  EXPECT_NE(variant.find("Hello World"), std::string::npos);
}

// An unchanged page read from the durable original cannot be rewritten
// through the alternate path (its id is a sentinel): the worker skips the
// source stamp quietly, with no write attempt and no failure counted.
TEST_F(WorkerTest, UnchangedHtmlFromADurableOriginalIsNotRestamped) {
  WorkerConfig config;
  config.socket_path = socket_path_;
  config.cache_path = cache_path_;
  config.cache_size_bytes = static_cast<uint64_t>(10 * 1024 * 1024);

  NullMessageHandler handler;
  Worker worker(config, &handler);
  ASSERT_TRUE(worker.Initialize());

  const std::string cache_url = "/original-plain.html";
  CacheDurableOriginal(worker.cache(), cache_url,
                       "<html><head></head><body><p>Plain</p></body></html>",
                       "text/html", ContentType::kHtml);

  std::thread worker_thread([&worker]() { worker.Run(); });
  WorkerStopper stopper(worker, worker_thread);
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  CacheNotification notification;
  notification.url = "http://example.com/original-plain.html";
  notification.scheme = "https";
  notification.content_type = ContentType::kHtml;
  notification.capability_mask = 0;
  SendNotification(notification);
  ASSERT_TRUE(pagespeed::test::WaitForNotificationsRetired(worker, 1,
                                                           kWorkerStallBudget));

  EXPECT_EQ(worker.stats().alternate_writes.load(), 0u);
  EXPECT_EQ(worker.stats().alternate_write_failures.load(), 0u);
}

// The image branch, via the SVG passthrough — it exercises the same source
// read without standing up a raster transcode, and its "did the source read
// succeed" signal (compressed variants exist) is unambiguous.
TEST_F(WorkerTest, ImageNotificationOptimizesFromADurableOriginalAlone) {
  WorkerConfig config;
  config.socket_path = socket_path_;
  config.cache_path = cache_path_;
  config.cache_size_bytes = static_cast<uint64_t>(10 * 1024 * 1024);

  NullMessageHandler handler;
  Worker worker(config, &handler);
  ASSERT_TRUE(worker.Initialize());

  const std::string url = "http://example.com/original-only.svg";
  const std::string svg_data =
      "<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"100\" "
      "height=\"100\"><circle cx=\"50\" cy=\"50\" r=\"40\"/></svg>";
  CacheDurableOriginal(worker.cache(), url, svg_data, "image/svg+xml",
                       ContentType::kOther);

  std::thread worker_thread([&worker]() { worker.Run(); });
  WorkerStopper stopper(worker, worker_thread);
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  CacheNotification notification;
  notification.url = url;
  notification.scheme = "https";
  notification.content_type = ContentType::kImage;
  notification.capability_mask = CapabilityMask().Encode();
  SendNotification(notification);

  CapabilityMask gz_mask;
  gz_mask.set_transfer_encoding(CapabilityMask::TransferEncoding::kGzip);
  std::string gz_variant;
  for (int i = 0; i < 60 * kSanitizerBudgetScale; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    gz_variant = ReadVariant(worker.cache(), url, gz_mask);
    if (!gz_variant.empty()) break;
  }
  EXPECT_FALSE(gz_variant.empty())
      << "image notification for a durable-original-only URL produced nothing";
}

// ORDERING (no preference inversion).  With BOTH a selectable alternate and a
// durable original present, the selector's answer still wins — the fallback is
// reached only when the selector found nothing usable, so nothing this build
// preferred as a source before it prefers any less now.
TEST_F(WorkerTest, ASelectableAlternateStillOutranksTheDurableOriginal) {
  WorkerConfig config;
  config.socket_path = socket_path_;
  config.cache_path = cache_path_;
  config.cache_size_bytes = static_cast<uint64_t>(10 * 1024 * 1024);

  NullMessageHandler handler;
  Worker worker(config, &handler);
  ASSERT_TRUE(worker.Initialize());

  const std::string url = "http://example.com/both-present.css";
  // Distinguishable by a declaration that survives minification.
  const std::string selectable_css = "body  {  outline:  1px  solid  red;  }\n";
  const std::string original_css = "body  {  zoom:  2;  }\n";
  CacheOriginal(worker.cache(), url, selectable_css);
  CacheDurableOriginal(worker.cache(), url, original_css, "text/css",
                       ContentType::kCss);

  std::thread worker_thread([&worker]() { worker.Run(); });
  WorkerStopper stopper(worker, worker_thread);
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  CacheNotification notification;
  notification.url = url;
  notification.scheme = "https";
  notification.content_type = ContentType::kCss;
  notification.capability_mask = CapabilityMask().Encode();
  SendNotification(notification);

  for (int i = 0; i < 60 * kSanitizerBudgetScale; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    if (worker.stats().css_processed.load() >= 1) break;
  }
  ASSERT_GE(worker.stats().css_processed.load(), 1u);

  const std::string variant =
      ReadVariant(worker.cache(), url, CapabilityMask());
  ASSERT_FALSE(variant.empty());
  EXPECT_NE(variant.find("outline"), std::string::npos)
      << "the selectable alternate must remain the preferred source";
  EXPECT_EQ(variant.find("zoom"), std::string::npos)
      << "the fallback must not displace a source the selector found";
  EXPECT_EQ(worker.stats().source_reads_from_durable_original.load(), 0u)
      << "the fallback must not run when the selector answered";
}

// THE SERVING BOUNDARY, pinned against the fallback.  Reading the class as a
// SOURCE must not make it reachable by SELECTION — before or after the worker
// has used it.  The selector itself is unchanged; this asserts the property
// end-to-end through the worker rather than only in the selector's own tests.
TEST_F(WorkerTest, UsingTheOriginalAsASourceLeavesItUnselectable) {
  WorkerConfig config;
  config.socket_path = socket_path_;
  config.cache_path = cache_path_;
  config.cache_size_bytes = static_cast<uint64_t>(10 * 1024 * 1024);

  NullMessageHandler handler;
  Worker worker(config, &handler);
  ASSERT_TRUE(worker.Initialize());

  const std::string url = "http://example.com/unselectable.css";
  const std::string css = "body  {  zoom:  2;  }\n";
  CacheDurableOriginal(worker.cache(), url, css, "text/css", ContentType::kCss);

  // Every request shape reaches the same verdict: not selectable.
  const CapabilityMask shapes[] = {
      CapabilityMask(),
      CapabilityMask(
          CapabilityMask::ImageFormat::kWebP, CapabilityMask::Viewport::kMobile,
          CapabilityMask::PixelDensity::k1x, CapabilityMask::SaveData::kOff,
          CapabilityMask::TransferEncoding::kGzip),
      CapabilityMask(
          CapabilityMask::ImageFormat::kAvif, CapabilityMask::Viewport::kTablet,
          CapabilityMask::PixelDensity::k2xPlus, CapabilityMask::SaveData::kOn,
          CapabilityMask::TransferEncoding::kBrotli),
  };
  for (const auto& shape : shapes) {
    EXPECT_TRUE(ReadBestVariant(worker.cache(), url, shape).empty())
        << "selection reached the durable original before the worker ran";
  }

  std::thread worker_thread([&worker]() { worker.Run(); });
  WorkerStopper stopper(worker, worker_thread);
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  CacheNotification notification;
  notification.url = url;
  notification.scheme = "https";
  notification.content_type = ContentType::kCss;
  notification.capability_mask = CapabilityMask().Encode();
  SendNotification(notification);

  for (int i = 0; i < 60 * kSanitizerBudgetScale; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    if (worker.stats().css_processed.load() >= 1) break;
  }
  ASSERT_GE(worker.stats().css_processed.load(), 1u)
      << "the source read must have succeeded for this test to say anything";

  // After the worker used it as a source, selection answers with the
  // OPTIMIZED bytes — never the original's, at any request shape.
  for (const auto& shape : shapes) {
    const std::string selected = ReadBestVariant(worker.cache(), url, shape);
    EXPECT_NE(selected, css)
        << "selection returned the durable original's raw bytes";
  }
  // And the class is still there, reachable only by exact id.
  auto by_id = worker.cache()->ReadOriginalAlternate(StripSchemeAuthority(url),
                                                     "", "https");
  ASSERT_TRUE(by_id.has_value())
      << "the durable original must survive being used as a source";
  auto body = by_id->content();
  EXPECT_EQ(
      std::string(reinterpret_cast<const char*>(body.data()), body.size()),
      css);
}

// FRESHNESS.  The class's lifetime is origin-freshness-bound, and that binding
// is a property of the STAMPED ORIGIN STATE evaluated by whoever reads the
// entry — the deliberate-read API refuses only a missing/mis-stamped metadata
// prefix, never a past-its-window entry (PageSpeedCache::ReadOriginalAlternate
// ByKey).  So the worker does not re-decide it, and it cannot launder it: a
// variant built from a durable original carries that original's origin state
// verbatim, so it reaches the same serve-side freshness verdict the original
// would have.  A source read whose result was stamped fresh must not produce a
// variant that looks fresher than the origin allowed.
TEST_F(WorkerTest, AVariantBuiltFromTheOriginalInheritsItsOriginFreshness) {
  WorkerConfig config;
  config.socket_path = socket_path_;
  config.cache_path = cache_path_;
  config.cache_size_bytes = static_cast<uint64_t>(10 * 1024 * 1024);

  NullMessageHandler handler;
  Worker worker(config, &handler);
  ASSERT_TRUE(worker.Initialize());

  const std::string url = "http://example.com/freshness.css";
  const std::string css = "body  {  margin:  0;  padding:  0;  }\n";
  // Stamped long past any plausible window: as an origin state this reads
  // stale at every evaluation time this test could run at.
  constexpr uint32_t kInsertedAt = 1'000'000'000;
  constexpr uint32_t kMaxAge = 600;
  CacheDurableOriginal(worker.cache(), url, css, "text/css", ContentType::kCss,
                       kInsertedAt, kMaxAge);

  std::thread worker_thread([&worker]() { worker.Run(); });
  WorkerStopper stopper(worker, worker_thread);
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  CacheNotification notification;
  notification.url = url;
  notification.scheme = "https";
  notification.content_type = ContentType::kCss;
  notification.capability_mask = CapabilityMask().Encode();
  SendNotification(notification);

  std::optional<VariantWithMeta> got;
  for (int i = 0; i < 60 * kSanitizerBudgetScale; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    got = ReadVariantWithMeta(worker.cache(), url, CapabilityMask());
    if (got.has_value()) break;
  }
  ASSERT_TRUE(got.has_value());

  // The origin state came across verbatim — this is what makes a separate
  // freshness gate on the source read unnecessary rather than merely omitted.
  EXPECT_EQ(got->metadata.cache_inserted_at, kInsertedAt);
  EXPECT_EQ(got->metadata.origin_max_age, kMaxAge);

  // And it evaluates the same way the original does: stale by age, at the
  // same evaluation instant, through the same shared evaluator.
  auto original = worker.cache()->ReadOriginalAlternate(
      StripSchemeAuthority(url), "", "https");
  ASSERT_TRUE(original.has_value());
  const uint32_t now = kInsertedAt + kMaxAge + 100;
  FreshnessConfig fc;
  const auto original_verdict = EvaluateFreshness(
      FreshnessInputFromMetadata(original->metadata, now), fc);
  const auto variant_verdict =
      EvaluateFreshness(FreshnessInputFromMetadata(got->metadata, now), fc);
  EXPECT_TRUE(original_verdict.expired_by_age);
  EXPECT_EQ(variant_verdict.verdict, original_verdict.verdict);
  EXPECT_EQ(variant_verdict.expired_by_age, original_verdict.expired_by_age);
  EXPECT_EQ(variant_verdict.remaining_ttl, original_verdict.remaining_ttl);

  // The variant is an ordinary capability-keyed entry, not the class: the
  // sentinel id must never leak into what the worker writes.
  EXPECT_EQ(got->metadata.full_mask & 0xFFu, CapabilityMask().Encode() & 0xFFu);
  EXPECT_NE(got->metadata.full_mask & 0xFFu,
            static_cast<uint32_t>(SentinelId::kOriginalContent));
}

// The Accept-negotiation refusal, which this change would otherwise open.
//
// A URL whose ORIGIN negotiates on Accept is stored as sent and never
// optimized — deriving variants from the one representation captured would
// layer a second negotiation on the origin's own.  That guard reads the flag
// off the stored original and, when there is none, fails OPEN, on the
// reasoning that no stored original means nothing to optimize from.  The
// source fallback removes that reasoning: with a durable original there IS
// something to optimize from.  So the guard consults it, and a marked durable
// original is refused exactly like a marked identity alternate.
TEST_F(WorkerTest, AMarkedDurableOriginalIsStillRefusedAsAnAcceptNegotiator) {
  WorkerConfig config;
  config.socket_path = socket_path_;
  config.cache_path = cache_path_;
  config.cache_size_bytes = static_cast<uint64_t>(10 * 1024 * 1024);

  NullMessageHandler handler;
  Worker worker(config, &handler);
  ASSERT_TRUE(worker.Initialize());

  const std::string url = "http://example.com/negotiated.css";
  const std::string css = "body  {  margin:  0;  padding:  0;  }\n";
  CacheDurableOriginal(worker.cache(), url, css, "text/css", ContentType::kCss,
                       /*cache_inserted_at=*/1'700'000'000,
                       /*origin_max_age=*/600, /*hostname=*/"",
                       AlternateMetadata::kFlagOriginVariesAccept);

  std::thread worker_thread([&worker]() { worker.Run(); });
  WorkerStopper stopper(worker, worker_thread);
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  CacheNotification notification;
  notification.url = url;
  notification.scheme = "https";
  notification.content_type = ContentType::kCss;
  notification.capability_mask = CapabilityMask().Encode();
  SendNotification(notification);

  // Long enough that the sibling tests' variant would have appeared.
  for (int i = 0; i < 20 * kSanitizerBudgetScale; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    if (worker.stats().notifications_skipped_dedup.load() >= 1) break;
  }
  EXPECT_GE(worker.stats().notifications_skipped_dedup.load(), 1u)
      << "the notification must be refused, not optimized";
  EXPECT_EQ(worker.stats().css_processed.load(), 0u);
  EXPECT_TRUE(ReadVariant(worker.cache(), url, CapabilityMask()).empty())
      << "a variant was derived for an origin that negotiates on Accept";
}

// The header-fidelity marker is advisory to serve paths, and the optimizer is
// not one of them.
//
// kFlagOriginHeadersNotReproducible says the origin sent a header no later
// serve can reproduce.  That is a statement about the RESPONSE a serve path
// would assemble, not about the bytes, so it must not reach optimization: a
// marked original is optimized exactly as an unmarked one is.  The obvious
// wrong implementation is the one next door — the Accept-negotiation refusal —
// and it would be silent, costing every marked URL its optimization with the
// same "nothing errored" signature that bug had.
//
// The second half is the flags byte the worker WRITES.  Variant metadata is
// built from the original's and the flags byte is assigned, not inherited, so
// the marker does not travel into derived variants.  That is the behaviour the
// contract states ("stamped by the writer at record time"), and it is asserted
// rather than left to the assignment staying an assignment.
TEST_F(WorkerTest, AnOriginalMarkedHeadersNotReproducibleIsStillOptimized) {
  WorkerConfig config;
  config.socket_path = socket_path_;
  config.cache_path = cache_path_;
  config.cache_size_bytes = static_cast<uint64_t>(10 * 1024 * 1024);

  NullMessageHandler handler;
  Worker worker(config, &handler);
  ASSERT_TRUE(worker.Initialize());

  const std::string url = "http://example.com/unreproducible-headers.css";
  const std::string css =
      "body  {  margin:  0;  padding:  0;  }\n"
      "h1  {  color:  red;  font-size:  24px;  }\n";
  CacheDurableOriginal(worker.cache(), url, css, "text/css", ContentType::kCss,
                       /*cache_inserted_at=*/1'700'000'000,
                       /*origin_max_age=*/600, /*hostname=*/"",
                       AlternateMetadata::kFlagOriginHeadersNotReproducible);

  std::thread worker_thread([&worker]() { worker.Run(); });
  WorkerStopper stopper(worker, worker_thread);
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  CacheNotification notification;
  notification.url = url;
  notification.scheme = "https";
  notification.content_type = ContentType::kCss;
  notification.capability_mask = CapabilityMask().Encode();
  SendNotification(notification);

  std::optional<VariantWithMeta> got;
  for (int i = 0; i < 60 * kSanitizerBudgetScale; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    got = ReadVariantWithMeta(worker.cache(), url, CapabilityMask());
    if (got.has_value()) break;
  }
  ASSERT_TRUE(got.has_value())
      << "the marker suppressed optimization, which it must not gate";
  EXPECT_LT(got->content.size(), css.size());
  EXPECT_GE(worker.stats().css_processed.load(), 1u);

  // The variant is the worker's own write, carrying the worker-processed bit
  // and not the writer's marker.
  EXPECT_NE(got->metadata.flags & AlternateMetadata::kFlagWorkerProcessed, 0);
  EXPECT_EQ(got->metadata.flags &
                AlternateMetadata::kFlagOriginHeadersNotReproducible,
            0)
      << "a record-time marker leaked into a variant the worker produced";

  // The compressed-variant leg: that write goes through the same shared
  // helper, which copies the flags byte it is handed — so non-propagation
  // holds there only because the caller composed the byte, a convention this
  // read pins as an invariant (#1342: derived variants do not inherit the
  // header-fidelity marker).
  CapabilityMask gz_mask;
  gz_mask.set_transfer_encoding(CapabilityMask::TransferEncoding::kGzip);
  std::optional<VariantWithMeta> gz;
  for (int i = 0; i < 60 * kSanitizerBudgetScale && !gz.has_value(); ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    gz = ReadVariantWithMeta(worker.cache(), url, gz_mask);
  }
  ASSERT_TRUE(gz.has_value()) << "gzip variant missing";
  EXPECT_EQ(
      gz->metadata.flags & AlternateMetadata::kFlagOriginHeadersNotReproducible,
      0)
      << "a record-time marker leaked into a compressed variant";

  // Non-vacuity: the marker really is on the entry the worker read from, so
  // the assertions above are about a marked input, not an unmarked one.
  auto original = worker.cache()->ReadOriginalAlternate(
      StripSchemeAuthority(url), "", "https");
  ASSERT_TRUE(original.has_value());
  EXPECT_NE(original->metadata.flags &
                AlternateMetadata::kFlagOriginHeadersNotReproducible,
            0);
}

// The transcode fall-through and the alternate id the origin's bytes land at
// (#1374), end to end through the real write path, the real content dedup and
// the real selector.
//
// An image whose pixel decode fails takes the per-format fall-through, which
// answers a conversion request with the origin's own bytes in the origin's own
// format.  Those bytes belong at the ORIGINAL-format id: ScoreAlternate gives a
// stored original-format alternate a fallback bonus for every client mask,
// while a stored WebP/AVIF whose format differs from what the client negotiated
// is a hard disqualification.  So the original-format id is reachable from
// every request shape and a converted id is reachable from exactly one.
//
// Pre-fix the WebP slot came back "successful" holding the GIF, the worker
// wrote it under the WebP id first, and the dedup then dropped the identical
// original-format twin -- so the family's only copy of the origin bytes sat at
// an id no other client could select.  Reintroducing that ordering or that slot
// flips the id assertions AND the two cross-format reachability reads below.
TEST_F(WorkerTest, DecodeFailingImageKeepsOriginBytesAtTheOriginalFormatId) {
  WorkerConfig config;
  config.socket_path = socket_path_;
  config.cache_path = cache_path_;
  config.cache_size_bytes = static_cast<uint64_t>(10 * 1024 * 1024);
  // Format siblings only, at the notification's exact combination: the family
  // under test is the three format slots and nothing else.
  config.proactive_image_variants = true;
  config.proactive_viewport_variants = false;
  config.proactive_density_variants = false;
  config.proactive_savedata_variants = false;

  NullMessageHandler handler;
  Worker worker(config, &handler);
  ASSERT_TRUE(worker.Initialize());

  // Valid GIF magic, nothing readable behind it: the decode fails, so every
  // conversion falls through.  Seeded as the durable original so all three
  // capability ids start free (a default-mask seed would already occupy the
  // original-format id and take that slot out of the family).
  const std::string gif("GIF89a\x01\x00\x01\x00", 10);
  const std::string url = "http://example.com/fallthrough.gif";
  CacheDurableOriginal(worker.cache(), url, gif, "image/gif",
                       ContentType::kImage);

  std::thread worker_thread([&worker]() { worker.Run(); });
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  const CapabilityMask desktop_original(
      CapabilityMask::ImageFormat::kOriginal,
      CapabilityMask::Viewport::kDesktop, CapabilityMask::PixelDensity::k1x,
      CapabilityMask::SaveData::kOff,
      CapabilityMask::TransferEncoding::kIdentity);
  const CapabilityMask desktop_webp(
      CapabilityMask::ImageFormat::kWebP, CapabilityMask::Viewport::kDesktop,
      CapabilityMask::PixelDensity::k1x, CapabilityMask::SaveData::kOff,
      CapabilityMask::TransferEncoding::kIdentity);
  const CapabilityMask desktop_avif(
      CapabilityMask::ImageFormat::kAvif, CapabilityMask::Viewport::kDesktop,
      CapabilityMask::PixelDensity::k1x, CapabilityMask::SaveData::kOff,
      CapabilityMask::TransferEncoding::kIdentity);

  CacheNotification notification;
  notification.url = url;
  notification.scheme = "https";
  notification.content_type = ContentType::kImage;
  notification.capability_mask = desktop_webp.Encode();
  SendNotification(notification);

  std::string at_original;
  for (int i = 0; i < 60 * kSanitizerBudgetScale; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    at_original = ReadVariant(worker.cache(), url, desktop_original);
    if (!at_original.empty()) break;
  }

  // The origin's bytes are at the original-format id ...
  EXPECT_EQ(gif, at_original)
      << "origin bytes are not at the original-format id";
  // ... and at neither converted id.
  EXPECT_TRUE(ReadVariant(worker.cache(), url, desktop_webp).empty())
      << "unconverted bytes were written under the WebP id";
  EXPECT_TRUE(ReadVariant(worker.cache(), url, desktop_avif).empty())
      << "unconverted bytes were written under the AVIF id";

  // The refusal is visible to an operator, not only to this test.
  // Both converted slots were refused for this input, and the counter that
  // says so is distinct from the no-savings one -- "there was nothing to
  // convert" and "the optimizer could not win" are different operator
  // situations, and telling them apart is exactly what was missing when #1374
  // went unnoticed.
  EXPECT_GE(worker.stats().image_unconverted_fallthrough.load(), 2u)
      << "the fall-through refusals were not counted";

  // The reachability that placement buys, read through the production
  // selector: all three client shapes resolve to those bytes.  Pre-fix the
  // original-format and AVIF reads came back empty -- the only stored
  // alternate was at the WebP id, and a client that negotiated anything else
  // is hard-disqualified from it.
  EXPECT_EQ(gif, ReadBestVariant(worker.cache(), url, desktop_original))
      << "an original-format client cannot reach the family";
  EXPECT_EQ(gif, ReadBestVariant(worker.cache(), url, desktop_webp))
      << "a WebP client cannot reach the family";
  EXPECT_EQ(gif, ReadBestVariant(worker.cache(), url, desktop_avif))
      << "an AVIF client cannot reach the family";

  worker.Shutdown();
  worker_thread.join();
}

// =============================================================================
// Variant sizes per client: the selection and the size gates together.
// =============================================================================

// Width x height of a JPEG, WebP or AVIF variant, read from its own bytes
// ({0, 0} when the bytes are none of those).
std::pair<uint32_t, uint32_t> VariantDimensions(const std::string& b) {
  auto u8 = [&](size_t i) {
    return static_cast<uint32_t>(static_cast<unsigned char>(b[i]));
  };
  if (b.size() > 4 && u8(0) == 0xFF && u8(1) == 0xD8) {
    size_t i = 2;
    while (i + 9 < b.size() && u8(i) == 0xFF) {
      const uint32_t marker = u8(i + 1);
      const size_t len = (u8(i + 2) << 8) | u8(i + 3);
      if (marker == 0xC0 || marker == 0xC1 || marker == 0xC2) {
        return {(u8(i + 7) << 8) | u8(i + 8), (u8(i + 5) << 8) | u8(i + 6)};
      }
      i += 2 + len;
    }
    return {0, 0};
  }
  if (b.size() > 30 && b.compare(0, 4, "RIFF") == 0 &&
      b.compare(8, 4, "WEBP") == 0) {
    if (b.compare(12, 4, "VP8 ") == 0) {
      return {((u8(27) << 8) | u8(26)) & 0x3FFF,
              ((u8(29) << 8) | u8(28)) & 0x3FFF};
    }
    if (b.compare(12, 4, "VP8L") == 0) {
      const uint32_t bits =
          u8(21) | (u8(22) << 8) | (u8(23) << 16) | (u8(24) << 24);
      return {(bits & 0x3FFF) + 1, ((bits >> 14) & 0x3FFF) + 1};
    }
    if (b.compare(12, 4, "VP8X") == 0) {
      return {(u8(24) | (u8(25) << 8) | (u8(26) << 16)) + 1,
              (u8(27) | (u8(28) << 8) | (u8(29) << 16)) + 1};
    }
    return {0, 0};
  }
  avifDecoder* decoder = avifDecoderCreate();
  std::pair<uint32_t, uint32_t> dims{0, 0};
  if (decoder != nullptr &&
      avifDecoderSetIOMemory(decoder,
                             reinterpret_cast<const uint8_t*>(b.data()),
                             b.size()) == AVIF_RESULT_OK &&
      avifDecoderParse(decoder) == AVIF_RESULT_OK) {
    dims = {decoder->image->width, decoder->image->height};
  }
  if (decoder != nullptr) avifDecoderDestroy(decoder);
  return dims;
}

// End-to-end reproduction of the per-client variant sizes: the request
// sequence of the bug reports (desktop and phone, Accept image/jpeg vs
// WebP-only vs AVIF, Save-Data on and off) replayed as the notifications a
// front end sends for it, against the worker's stock image configuration
// and a stored 1200x800 high-quality noisy photo, then read back through the
// production selector for each client.  The fixture is a seeded
// diamond-square plasma (three channels, roughness 0.9) saved at JPEG
// quality 95 -- the same class of content as the reports' fractal plasma,
// at half their size, which still resizes for a phone (960 px target).
//
// Invariants, for every client:
//   * a phone is never served the desktop-size image when a phone-size one
//     exists (here: the phone JPEG), whatever format it negotiated;
//   * a converted (WebP/AVIF) variant is never larger than the
//     original-format variant it replaces for that client;
//   * a Save-Data variant is never larger than the variant the same client
//     gets without Save-Data.
// The byte table is printed so a run documents the sizes it saw.
// Shared by the variant-size tests below: the reported request mix, the
// storage layouts of the two front-end families, and the per-slot size check.
class VariantSizeTest : public WorkerTest {
 protected:
  // How a front end records the origin's bytes: the Apache module and the
  // .NET middleware keep them as the durable original (0x0C); the nginx
  // module records them at the identity slot (0x08), the same id the
  // optimized desktop original-format variant lives at.
  enum class Layout : std::uint8_t { kDurableOriginal, kIdentitySlot };

  void StoreOriginal(Worker& worker, const std::string& url,
                     const std::string& bytes, Layout layout) {
    if (layout == Layout::kIdentitySlot) {
      CacheOriginal(worker.cache(), url, bytes);
    } else {
      CacheDurableOriginal(worker.cache(), url, bytes, "image/jpeg",
                           ContentType::kImage,
                           static_cast<uint32_t>(std::time(nullptr)), 3600);
    }
  }

  // Every stored converted variant is smaller than the original-format
  // variant of its slot, and every stored Save-Data variant is smaller than
  // the same format and size without Save-Data.
  void ExpectStoredSlotsSmallerThanFallback(Worker& worker,
                                            const std::string& url) {
    auto alts =
        worker.cache()->ListAlternates(StripSchemeAuthority(url), "", "https");
    ASSERT_TRUE(alts.has_value());
    std::map<uint8_t, uint64_t> sizes;
    for (const auto& a : *alts) {
      if (!IsSentinel(static_cast<uint8_t>(a.id))) {
        sizes[static_cast<uint8_t>(a.id)] = a.content_length;
      }
    }
    ASSERT_FALSE(sizes.empty());
    for (const auto& [id, size] : sizes) {
      if ((id & 0x03) != 0) {  // WebP or AVIF: compare with the JPEG slot
        auto orig = sizes.find(static_cast<uint8_t>(id & ~0x03));
        if (orig != sizes.end()) {
          EXPECT_LT(size, orig->second) << "slot 0x" << std::hex << int{id};
        }
      }
      if ((id & 0x20) != 0) {  // Save-Data: compare with the normal slot
        auto normal = sizes.find(static_cast<uint8_t>(id & ~0x20));
        if (normal != sizes.end()) {
          EXPECT_LT(size, normal->second) << "slot 0x" << std::hex << int{id};
        }
      }
    }
  }

  void ReplayReportedClients(Layout layout);
  void ReplayWarmupMatrix(Layout layout);
};

void VariantSizeTest::ReplayReportedClients(Layout layout) {
  if (pagespeed::test::kThreadSanitizerBuild) {
    GTEST_SKIP() << "checks the encoder byte-size rules on a 1200x800 "
                    "plasma, not concurrency; too slow under TSan (issue "
                    "#436) and still covered by the plain and ASan lanes";
  }
  WorkerConfig config;
  config.socket_path = socket_path_;
  config.cache_path = cache_path_;
  config.cache_size_bytes = static_cast<uint64_t>(64 * 1024 * 1024);

  NullMessageHandler handler;
  Worker worker(config, &handler);
  ASSERT_TRUE(worker.Initialize());

  const std::string url = "http://example.com/big.jpg";
  const std::string jpeg = ReadTestFile("jpeg/plasma_1200x800_q95.jpg");
  ASSERT_FALSE(jpeg.empty());
  StoreOriginal(worker, url, jpeg, layout);

  std::thread worker_thread([&worker]() { worker.Run(); });
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  struct Client {
    const char* name;
    uint32_t mask;
  };
  // Masks as CapabilityMask::FromHeaders derives them from the reports'
  // requests: desktop Chrome is Desktop/1x (0x08), the Pixel 8 is Mobile/2x
  // (0x10); Accept picks the format, Save-Data sets 0x20.
  const Client clients[] = {
      {"desktop  image/jpeg       ", 0x08},
      {"phone    image/jpeg       ", 0x10},
      {"desktop  image/jpeg  SD   ", 0x28},
      {"desktop  AVIF+WebP        ", 0x0A},
      {"desktop  AVIF+WebP   SD   ", 0x2A},
      {"desktop  WebP, no AVIF    ", 0x09},
      {"phone    WebP, no AVIF    ", 0x11},
  };
  uint64_t sent = 0;
  for (const Client& c : clients) {
    CacheNotification n;
    n.url = url;
    n.scheme = "https";
    n.content_type = ContentType::kImage;
    n.capability_mask = c.mask;
    SendNotification(n);
    ASSERT_TRUE(pagespeed::test::WaitForNotificationsRetired(
        worker, ++sent, kWorkerStallBudget));
  }

  std::map<uint32_t, std::string> served;
  std::cout << "\nclient                       bytes      size\n";
  for (const Client& c : clients) {
    served[c.mask] =
        ReadBestVariant(worker.cache(), url, CapabilityMask::Decode(c.mask));
    const auto [w, h] = VariantDimensions(served[c.mask]);
    std::cout << c.name << std::setw(9) << served[c.mask].size() << "  " << w
              << "x" << h << "\n";
    ASSERT_FALSE(served[c.mask].empty()) << c.name;
  }

  // A phone gets a phone-size image, whatever format it negotiated.
  EXPECT_EQ(VariantDimensions(served[0x11]), VariantDimensions(served[0x10]))
      << "the WebP-only phone was served another size than the phone JPEG";
  EXPECT_LE(VariantDimensions(served[0x11]).first, 960u);
  EXPECT_LE(served[0x11].size(), served[0x10].size());
  // A converted format never costs more than the JPEG it replaces.
  EXPECT_LE(served[0x09].size(), served[0x08].size());
  EXPECT_LE(served[0x0A].size(), served[0x08].size());
  // Save-Data never costs more than the same client without it.
  EXPECT_LE(served[0x28].size(), served[0x08].size());
  EXPECT_LE(served[0x2A].size(), served[0x0A].size());
  // The desktop original-format client gets the optimized encode, never
  // the upload, on either layout.
  EXPECT_LT(served[0x08].size(), jpeg.size());

  ExpectStoredSlotsSmallerThanFallback(worker, url);

  if (layout == Layout::kIdentitySlot) {
    // The upload replaced at the identity slot is kept as the durable
    // original, so change detection keeps reading the origin's bytes: a
    // second pass does not mistake the optimized encode for a new origin.
    auto durable = worker.cache()->ReadOriginalAlternate(
        StripSchemeAuthority(url), "", "https");
    EXPECT_TRUE(durable.has_value()) << "the upload was not kept";
    if (durable.has_value()) {
      auto span = durable->content();
      EXPECT_EQ(
          std::string(reinterpret_cast<const char*>(span.data()), span.size()),
          jpeg);
      durable->release();
    }
    CacheNotification again;
    again.url = url;
    again.scheme = "https";
    again.content_type = ContentType::kImage;
    again.capability_mask = 0x14;  // tablet: a slot not built yet
    SendNotification(again);
    ASSERT_TRUE(pagespeed::test::WaitForNotificationsRetired(
        worker, ++sent, kWorkerStallBudget));
    EXPECT_EQ(worker.stats().content_hash_stale.load(), 0u)
        << "the worker's own encode was read as an origin change";
  }

  worker.Shutdown();
  worker_thread.join();
}

TEST_F(VariantSizeTest, ReportedClientsDurableOriginalLayout) {
  ReplayReportedClients(Layout::kDurableOriginal);
}

// The nginx layout: the upload sits at the identity slot.  The desktop
// original-format variant must still be built over it (it used to be served
// raw), and the size gates must compare with that optimized encode.
TEST_F(VariantSizeTest, ReportedClientsIdentitySlotLayout) {
  ReplayReportedClients(Layout::kIdentitySlot);
}

// The warmup (hot URL) matrix writes variants through its own path; it
// keeps the same size rules.  One test per storage layout: the two replays
// are independent and each is slow, so separate test ids let gtest sharding
// place them on different shards.
void VariantSizeTest::ReplayWarmupMatrix(Layout layout) {
  if (pagespeed::test::kThreadSanitizerBuild) {
    GTEST_SKIP() << "checks the encoder byte-size rules on a 1200x800 "
                    "plasma, not concurrency; too slow under TSan (issue "
                    "#436) and still covered by the plain and ASan lanes";
  }
  WorkerConfig config;
  config.socket_path = socket_path_;
  config.cache_path = cache_path_;
  config.cache_size_bytes = static_cast<uint64_t>(64 * 1024 * 1024);
  config.enable_warmup = true;
  // Desktop 1x with and without Save-Data: enough to exercise both gates
  // without the full viewport matrix.
  config.proactive_viewport_variants = false;
  config.proactive_density_variants = false;
  config.proactive_savedata_variants = true;

  NullMessageHandler handler;
  Worker worker(config, &handler);
  ASSERT_TRUE(worker.Initialize());
  const std::string url = "http://example.com/warm.jpg";
  const std::string jpeg = ReadTestFile("jpeg/plasma_1200x800_q95.jpg");
  ASSERT_FALSE(jpeg.empty());
  StoreOriginal(worker, url, jpeg, layout);

  std::thread worker_thread([&worker]() { worker.Run(); });
  std::this_thread::sleep_for(std::chrono::milliseconds(50));
  CacheNotification warm;
  warm.url = url;
  warm.scheme = "https";
  warm.content_type = ContentType::kImage;
  warm.capability_mask = kWarmupSentinel;
  SendNotification(warm);
  ASSERT_TRUE(pagespeed::test::WaitForNotificationsRetired(worker, 1,
                                                           kWorkerStallBudget));
  EXPECT_GT(worker.stats().variants_written.load(), 0u);

  ExpectStoredSlotsSmallerThanFallback(worker, url);
  const std::string desktop_jpeg =
      ReadBestVariant(worker.cache(), url, CapabilityMask::Decode(0x08));
  EXPECT_LT(desktop_jpeg.size(), jpeg.size());
  EXPECT_LE(
      ReadBestVariant(worker.cache(), url, CapabilityMask::Decode(0x0A)).size(),
      desktop_jpeg.size());
  EXPECT_LE(
      ReadBestVariant(worker.cache(), url, CapabilityMask::Decode(0x28)).size(),
      desktop_jpeg.size());

  worker.Shutdown();
  worker_thread.join();
}

TEST_F(VariantSizeTest, WarmupMatrixKeepsTheSizeRulesDurableOriginalLayout) {
  ReplayWarmupMatrix(Layout::kDurableOriginal);
}

TEST_F(VariantSizeTest, WarmupMatrixKeepsTheSizeRulesIdentitySlotLayout) {
  ReplayWarmupMatrix(Layout::kIdentitySlot);
}

// An identity slot that already holds a worker variant is not the origin's
// bytes: it is never copied into the durable original.
TEST_F(VariantSizeTest, WorkerVariantAtIdentitySlotIsNotKeptAsTheOriginal) {
  WorkerConfig config;
  config.socket_path = socket_path_;
  config.cache_path = cache_path_;
  config.cache_size_bytes = static_cast<uint64_t>(16 * 1024 * 1024);

  NullMessageHandler handler;
  Worker worker(config, &handler);
  ASSERT_TRUE(worker.Initialize());

  const std::string url = "http://example.com/processed.jpg";
  const std::string jpeg = ReadTestFile("jpeg/sjpeg4.jpg");
  ASSERT_FALSE(jpeg.empty());
  CacheOriginal(worker.cache(), url, jpeg, "",
                AlternateMetadata::kFlagWorkerProcessed);

  std::thread worker_thread([&worker]() { worker.Run(); });
  std::this_thread::sleep_for(std::chrono::milliseconds(50));
  CacheNotification n;
  n.url = url;
  n.scheme = "https";
  n.content_type = ContentType::kImage;
  n.capability_mask = 0x08;
  SendNotification(n);
  ASSERT_TRUE(pagespeed::test::WaitForNotificationsRetired(worker, 1,
                                                           kWorkerStallBudget));

  auto durable = worker.cache()->ReadOriginalAlternate(
      StripSchemeAuthority(url), "", "https");
  EXPECT_FALSE(durable.has_value())
      << "a worker variant was kept as the durable original";

  worker.Shutdown();
  worker_thread.join();
}

// Proactive variants for an image narrower than the tablet width: the
// tablet and desktop encodes come out byte-identical (neither resizes).  The
// pass used to skip the desktop copies as content duplicates of the tablet
// ones, but the selector ranks a viewport match above format, so a desktop
// client is never handed the tablet copy: it got the front end's raw
// recording at the identity slot instead, whatever format it negotiated.
// Every desktop client must get an optimized variant, never the upload.
TEST_F(VariantSizeTest, ProactiveDesktopSlotIsBuiltForASmallImage) {
  WorkerConfig config;
  config.socket_path = socket_path_;
  config.cache_path = cache_path_;
  config.cache_size_bytes = static_cast<uint64_t>(32 * 1024 * 1024);
  config.proactive_image_variants = true;
  // One density and no Save-Data keep the matrix to the three viewports.
  config.proactive_density_variants = false;
  config.proactive_savedata_variants = false;

  NullMessageHandler handler;
  Worker worker(config, &handler);
  ASSERT_TRUE(worker.Initialize());

  // 512x512, below the tablet width (768): no viewport resizes it.
  const std::string url = "http://example.com/small.jpg";
  const std::string jpeg = ReadTestFile("jpeg/sjpeg6.jpg");
  ASSERT_FALSE(jpeg.empty());
  StoreOriginal(worker, url, jpeg, Layout::kIdentitySlot);

  std::thread worker_thread([&worker]() { worker.Run(); });
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  CacheNotification n;
  n.url = url;
  n.scheme = "https";
  n.content_type = ContentType::kImage;
  n.capability_mask = 0x09;  // desktop, 1x, WebP: the E2E suite's request
  SendNotification(n);
  ASSERT_TRUE(pagespeed::test::WaitForNotificationsRetired(worker, 1,
                                                           kWorkerStallBudget));

  // Desktop and tablet, 1x, original/WebP/AVIF.
  for (uint32_t mask : {0x08u, 0x09u, 0x0Au, 0x04u, 0x05u, 0x06u}) {
    const std::string served =
        ReadBestVariant(worker.cache(), url, CapabilityMask::Decode(mask));
    ASSERT_FALSE(served.empty()) << "mask 0x" << std::hex << mask;
    EXPECT_TRUE(served != jpeg) << "mask 0x" << std::hex << mask
                                << " was served the unoptimized upload";
    EXPECT_LT(served.size(), jpeg.size()) << "mask 0x" << std::hex << mask;
  }
  // A desktop client gets the format a tablet client with the same Accept
  // gets: the same encode, so the same size-gate decision.
  for (uint32_t fmt : {0x1u, 0x2u}) {
    const std::string desktop = ReadBestVariant(
        worker.cache(), url, CapabilityMask::Decode(0x08 | fmt));
    const std::string tablet = ReadBestVariant(
        worker.cache(), url, CapabilityMask::Decode(0x04 | fmt));
    EXPECT_TRUE(desktop.substr(0, 4) == tablet.substr(0, 4))
        << "format " << fmt;
  }
  ExpectStoredSlotsSmallerThanFallback(worker, url);

  worker.Shutdown();
  worker_thread.join();
}

// The image dedup key is the full (format, viewport, density, save-data)
// slot.  One pass decides every format of its slot, so a notification for
// another format of the same slot is answered by that decision (a refused
// format is not re-run on every request), while a different size or a
// different Save-Data setting is new work.
TEST_F(WorkerTest, ImageDedupKeyIsTheFullSlot) {
  WorkerConfig config;
  config.socket_path = socket_path_;
  config.cache_path = cache_path_;
  config.cache_size_bytes = static_cast<uint64_t>(16 * 1024 * 1024);
  config.mobile_width = 160;  // sjpeg4 is 512 wide: the phone slot resizes

  NullMessageHandler handler;
  Worker worker(config, &handler);
  ASSERT_TRUE(worker.Initialize());

  const std::string url = "http://example.com/slot.jpg";
  const std::string jpeg = ReadTestFile("jpeg/sjpeg4.jpg");
  ASSERT_FALSE(jpeg.empty());
  CacheOriginal(worker.cache(), url, jpeg);

  std::thread worker_thread([&worker]() { worker.Run(); });
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  uint64_t sent = 0;
  auto notify = [&](uint32_t mask) {
    CacheNotification n;
    n.url = url;
    n.scheme = "https";
    n.content_type = ContentType::kImage;
    n.capability_mask = mask;
    SendNotification(n);
    ASSERT_TRUE(pagespeed::test::WaitForNotificationsRetired(
        worker, ++sent, kWorkerStallBudget));
  };
  auto skipped = [&] {
    return worker.stats().notifications_skipped_dedup.load();
  };

  ASSERT_NO_FATAL_FAILURE(notify(0x10));  // phone, original format
  const uint64_t s0 = skipped();
  ASSERT_NO_FATAL_FAILURE(notify(0x11));  // same slot, WebP
  ASSERT_NO_FATAL_FAILURE(notify(0x12));  // same slot, AVIF
  ASSERT_NO_FATAL_FAILURE(notify(0x10));  // same slot, original again
  EXPECT_EQ(skipped(), s0 + 3)
      << "every format of a decided slot is answered by its decision";

  const uint64_t written = worker.stats().variants_written.load();
  ASSERT_NO_FATAL_FAILURE(notify(0x31));  // phone + Save-Data, WebP
  ASSERT_NO_FATAL_FAILURE(notify(0x01));  // phone 1x, WebP
  ASSERT_NO_FATAL_FAILURE(notify(0x09));  // desktop, WebP
  EXPECT_EQ(skipped(), s0 + 3)
      << "another Save-Data setting, density or viewport is a new slot";
  EXPECT_GT(worker.stats().variants_written.load(), written);

  worker.Shutdown();
  worker_thread.join();
}

}  // namespace
}  // namespace pagespeed
