// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

// Pipeline Integration Tests
//
// Tests cross-component flows through the full optimization pipeline:
// notification sender → worker socket → content processing → cache write.
//
// Unlike unit tests, these exercise the complete stack including IPC
// serialization, libuv event loop, and Cyclone cache.

#include <chrono>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <span>
#include <string>
#include <thread>

#include "gtest/gtest.h"
#include "lib/base/message_handler.h"
#include "lib/cache/cache.h"
#include "lib/classify/alternate_id.h"
#include "lib/classify/alternate_metadata.h"
#include "lib/classify/capability_mask.h"
#include "src/proto/notification_sender.h"
#include "src/proto/worker_ipc.h"
#include "src/worker/worker.h"
#include "test/test_util/pipe_client.h"
#include "test/test_util/scoped_thread_join.h"
#include "test/test_util/temp_dir.h"
#include "test/test_util/worker_quiescence.h"

namespace pagespeed {
namespace {

// Helper to read a test file into a string.
std::string ReadTestFile(const std::string& filename) {
  std::string path = "test/lib/image/testdata/" + filename;
  std::ifstream file(path, std::ios::binary);
  if (!file.is_open()) {
    return "";
  }
  return std::string((std::istreambuf_iterator<char>(file)),
                     std::istreambuf_iterator<char>());
}

class PipelineTest : public ::testing::Test {
 protected:
  void SetUp() override {
    temp_dir_ = pagespeed::test::MakeTempDir();
    socket_path_ = temp_dir_ + "/worker.sock";
    cache_path_ = temp_dir_ + "/cache";
  }

  void TearDown() override {
    if (!temp_dir_.empty()) {
      std::error_code ec;
      std::filesystem::remove_all(temp_dir_, ec);
    }
  }

  // Start worker and return thread. Caller must call Shutdown + join.
  std::thread StartWorker(Worker& worker) {
    EXPECT_TRUE(worker.Initialize());
    std::thread t([&worker]() { worker.Run(); });
    // Wait for pipe endpoint to appear.
    for (int i = 0; i < 50; ++i) {
      if (pagespeed::test::PipeEndpointExists(socket_path_)) {
        break;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return t;
  }

  // Pre-populate cache with content at default mask (Desktop/Identity = 0x08).
  void CacheOriginal(PageSpeedCache* cache, const std::string& url,
                     const std::string& content,
                     const std::string& origin_content_type = "") {
    CapabilityMask default_mask;  // Desktop/Identity = 0x08
    AlternateId id =
        MaskToAlternateId(static_cast<uint8_t>(default_mask.Encode() & 0xFF));
    AlternateMetadata meta;
    meta.full_mask = default_mask.Encode();
    meta.content_type = ContentType::kOther;
    // The front end records the origin's Content-Type; the worker inlines
    // critical CSS only into a page recorded as text/html.
    meta.origin_content_type = origin_content_type;
    auto wh = cache->WriteAlternate(url, "", "https", id, content.size(), meta);
    ASSERT_TRUE(wh.has_value()) << "Failed to open write handle";
    auto written = wh->write_sync(
        std::as_bytes(std::span(content.data(), content.size())));
    ASSERT_TRUE(written.has_value()) << "Failed to write content";
    auto closed = wh->close_sync();
    ASSERT_TRUE(closed.has_value()) << "Failed to close write handle";
  }

  // Read a variant from cache with a specific mask.
  std::string ReadVariant(PageSpeedCache* cache, const std::string& url,
                          const CapabilityMask& mask) {
    AlternateId id =
        MaskToAlternateId(static_cast<uint8_t>(mask.Encode() & 0xFF));
    auto result = cache->ReadAlternate(url, "", "https", id);
    if (!result.has_value()) return "";
    auto content = result->content();
    return std::string(reinterpret_cast<const char*>(content.data()),
                       content.size());
  }

  // Read a sentinel entry from cache.
  std::string ReadSentinel(PageSpeedCache* cache, const std::string& url,
                           SentinelId sentinel) {
    auto result = cache->ReadAlternate(url, "", "https",
                                       static_cast<AlternateId>(sentinel));
    if (!result.has_value()) return "";
    auto content = result->content();
    return std::string(reinterpret_cast<const char*>(content.data()),
                       content.size());
  }

  // Test-to-worker sync cadence. Each PollVariant / PollSentinel iteration
  // sleeps kPollIntervalMs between cache reads; iteration counts are
  // derived from a wall-clock timeout in ms so editing kPollIntervalMs
  // preserves the per-site window math at each call site.
  static constexpr int kPollIntervalMs = 100;
  static constexpr int kDefaultPollIterations =
      4'000 / kPollIntervalMs;  // 4 s — fast path
  static constexpr int kJsMinifyPollIterations =
      30'000 / kPollIntervalMs;  // 30 s — unoptimized Windows fastbuild on a
                                 // loaded shared runner exceeds 6 s (issue
                                 // #1122: 0/3 on a loaded machine against a
                                 // pass on an idle one, same tree).
                                 // Same class as the AVIF window below.
  static constexpr int kCssMinifyPollIterations =
      30'000 / kPollIntervalMs;  // 30 s — same runner class: the default 4 s
                                 // window already produced a miss on a loaded
                                 // machine (variant.size() == original.size(),
                                 // run 30286431914, 2026-07-27).
  static constexpr int kWebPPollIterations =
      6'000 / kPollIntervalMs;  // 6 s — WebP encode
  static constexpr int kGifAnimationPollIterations =
      2'000 / kPollIntervalMs;  // 2 s — best-effort, variant may not be written

  // Silence limit for WaitForNotificationsRetired: how long the worker may go
  // without moving any progress counter before the wait gives up.  Not a
  // deadline on the work; see test/test_util/worker_quiescence.h.  The
  // slowest step here is one combination of a 150 KB JPEG, about two minutes
  // on a heavily oversubscribed host, so 5 minutes in a plain build.
  // Sanitizer builds run the encoders roughly an order of magnitude slower
  // and get 20 minutes (the worker_test value).  No sanitizer config sets a
  // test timeout (.bazelrc, CI), so the target's own timeout applies to every
  // build; it is "eternal" (3600 s) in test/e2e/BUILD, which holds this
  // binary's tests (unsharded) plus a 20-minute stall, so a stall reports
  // through the wait's message rather than as a bare timeout.
  static constexpr std::chrono::minutes kWorkerStallBudget{
      pagespeed::test::kSanitizerBuild ? 20 : 5};

  // Poll for a variant to appear in cache (up to timeout).
  std::string PollVariant(PageSpeedCache* cache, const std::string& url,
                          const CapabilityMask& mask,
                          int max_attempts = kDefaultPollIterations) {
    std::string variant;
    for (int i = 0; i < max_attempts; ++i) {
      std::this_thread::sleep_for(std::chrono::milliseconds(kPollIntervalMs));
      variant = ReadVariant(cache, url, mask);
      if (!variant.empty()) break;
    }
    return variant;
  }

  // Poll for a transformed variant (up to timeout).  CSS/JS minify writes
  // the variant at the same alternate key where CacheOriginal pre-populated
  // the original (Desktop/Identity), so a plain non-empty poll can observe
  // the original bytes before the worker overwrites them — seen on loaded
  // Windows CI runners as CssPipelineEndToEnd reading back the unminified
  // original (variant.size() == original.size()).  Keep polling until the
  // content differs from the original; on timeout the original is returned
  // and the caller's assertions fail as before.
  std::string PollTransformedVariant(
      PageSpeedCache* cache, const std::string& url, const CapabilityMask& mask,
      const std::string& original, int max_attempts = kDefaultPollIterations) {
    std::string variant;
    for (int i = 0; i < max_attempts; ++i) {
      std::this_thread::sleep_for(std::chrono::milliseconds(kPollIntervalMs));
      variant = ReadVariant(cache, url, mask);
      if (!variant.empty() && variant != original) break;
    }
    return variant;
  }

  // Poll for a sentinel entry to appear in cache.
  std::string PollSentinel(PageSpeedCache* cache, const std::string& url,
                           SentinelId sentinel,
                           int max_attempts = kDefaultPollIterations) {
    std::string value;
    for (int i = 0; i < max_attempts; ++i) {
      std::this_thread::sleep_for(std::chrono::milliseconds(kPollIntervalMs));
      value = ReadSentinel(cache, url, sentinel);
      if (!value.empty()) break;
    }
    return value;
  }

  std::string temp_dir_;
  std::string socket_path_;
  std::string cache_path_;
  NullMessageHandler handler_;
};

// =============================================================================
// HTML Pipeline: notification → worker → scan → extract → inject → cache
// =============================================================================

TEST_F(PipelineTest, HtmlPipelineEndToEnd) {
  // Full HTML critical CSS injection pipeline: the worker reads
  // original HTML, scans for elements and CSS, extracts critical rules,
  // injects them as a <style> tag, and writes the variant.
  WorkerConfig config;
  config.socket_path = socket_path_;
  config.cache_path = cache_path_;
  config.cache_size_bytes = static_cast<uint64_t>(10 * 1024 * 1024);

  config.unsafe_force_async_css =
      true;  // fixture page is not under test for the gate
  Worker worker(config, &handler_);
  auto thread = StartWorker(worker);
  // Pre-populate cache with HTML containing inline CSS.
  // Use path-only URL for cache operations (the worker normalizes URLs
  // by stripping scheme+authority via NormalizeCacheUrl).
  std::string cache_url = "/index.html";
  std::string notification_url = "http://example.com/index.html";
  std::string html =
      "<html><head><style>"
      "body { margin: 0; padding: 0; }"
      "h1 { color: red; font-size: 32px; }"
      ".hero { background: blue; }"
      "</style></head>"
      "<body><h1>Hello</h1><div class=\"hero\">World</div></body></html>";
  CacheOriginal(worker.cache(), cache_url, html, "text/html; charset=utf-8");

  // Send notification via the notification sender API.
  CacheNotification notification;
  notification.url = notification_url;
  notification.scheme = "https";
  notification.content_type = ContentType::kHtml;
  notification.capability_mask = 0;

  auto send_result = SendNotification(socket_path_, notification);
  EXPECT_TRUE(send_result.success) << send_result.error_message;

  // Poll for the HTML variant.
  CapabilityMask mask = CapabilityMask::Decode(0);
  std::string variant = PollVariant(worker.cache(), cache_url, mask);
  EXPECT_FALSE(variant.empty()) << "HTML variant not written";
  if (!variant.empty()) {
    // The variant should contain critical CSS injected inline.
    EXPECT_NE(variant.find("data-pagespeed-critical"), std::string::npos);
    // Original HTML structure should be preserved.
    EXPECT_NE(variant.find("Hello"), std::string::npos);
    EXPECT_NE(variant.find("World"), std::string::npos);
    // The variant should be larger (CSS duplicated as critical).
    EXPECT_GT(variant.size(), html.size());
  }

  // Verify original HTML is preserved at the default mask.
  CapabilityMask default_mask;
  std::string cached = ReadVariant(worker.cache(), cache_url, default_mask);
  EXPECT_EQ(cached, html);

  worker.Shutdown();
  thread.join();
}

// =============================================================================
// CSS Pipeline: notification → worker → minify → cache
// =============================================================================

TEST_F(PipelineTest, CssPipelineEndToEnd) {
  WorkerConfig config;
  config.socket_path = socket_path_;
  config.cache_path = cache_path_;
  config.cache_size_bytes = static_cast<uint64_t>(10 * 1024 * 1024);

  Worker worker(config, &handler_);
  auto thread = StartWorker(worker);
  // Pre-populate with unminified CSS.
  // Path-only URL for cache ops; full URL for notification.
  std::string cache_url = "/styles.css";
  std::string notification_url = "http://example.com/styles.css";
  std::string css =
      "body  {\n"
      "  margin:  0;\n"
      "  padding:  0;\n"
      "}\n"
      "h1  {\n"
      "  color:  red;\n"
      "  font-size:  24px;\n"
      "}\n";
  CacheOriginal(worker.cache(), cache_url, css);

  // Send notification.
  CacheNotification notification;
  notification.url = notification_url;
  notification.scheme = "https";
  notification.content_type = ContentType::kCss;
  notification.capability_mask = CapabilityMask().Encode();

  auto send_result = SendNotification(socket_path_, notification);
  EXPECT_TRUE(send_result.success) << send_result.error_message;

  // Poll for minified variant (CSS is always written at Desktop/Identity,
  // overwriting the pre-populated original at the same key — poll past it).
  CapabilityMask mask;
  std::string variant = PollTransformedVariant(worker.cache(), cache_url, mask,
                                               css, kCssMinifyPollIterations);
  EXPECT_FALSE(variant.empty()) << "Minified CSS not written";
  EXPECT_NE(variant, css) << "Minified CSS not written (still original)";
  if (!variant.empty() && variant != css) {
    EXPECT_LT(variant.size(), css.size());
    EXPECT_NE(variant.find("margin"), std::string::npos);
  }

  worker.Shutdown();
  thread.join();
}

// =============================================================================
// JS Pipeline: notification → worker → minify → cache
// =============================================================================

TEST_F(PipelineTest, JsPipelineEndToEnd) {
  WorkerConfig config;
  config.socket_path = socket_path_;
  config.cache_path = cache_path_;
  config.cache_size_bytes = static_cast<uint64_t>(10 * 1024 * 1024);

  Worker worker(config, &handler_);
  auto thread = StartWorker(worker);
  // Pre-populate with unminified JS.
  std::string cache_url = "/app.js";
  std::string notification_url = "http://example.com/app.js";
  std::string js =
      "// Main application\n"
      "function  greet( name )  {\n"
      "  var  msg  =  'Hello, '  +  name;\n"
      "  return  msg;\n"
      "}\n";
  CacheOriginal(worker.cache(), cache_url, js);

  // Send notification.
  CacheNotification notification;
  notification.url = notification_url;
  notification.scheme = "https";
  notification.content_type = ContentType::kJs;
  notification.capability_mask = CapabilityMask().Encode();

  auto send_result = SendNotification(socket_path_, notification);
  EXPECT_TRUE(send_result.success) << send_result.error_message;

  // Poll for minified JS (longer timeout: RE2 pattern compilation is slow
  // under sanitizers). JS is always written at Desktop/Identity, overwriting
  // the pre-populated original at the same key — poll past it.
  CapabilityMask mask;
  std::string variant = PollTransformedVariant(worker.cache(), cache_url, mask,
                                               js, kJsMinifyPollIterations);
  EXPECT_FALSE(variant.empty()) << "Minified JS not written";
  EXPECT_NE(variant, js) << "Minified JS not written (still original)";
  if (!variant.empty() && variant != js) {
    EXPECT_LE(variant.size(), js.size());
    EXPECT_NE(variant.find("greet"), std::string::npos);
  }

  worker.Shutdown();
  thread.join();
}

// =============================================================================
// Image Pipeline: JPEG → WebP through full worker
// =============================================================================

TEST_F(PipelineTest, ImagePipelineEndToEnd) {
  WorkerConfig config;
  config.socket_path = socket_path_;
  config.cache_path = cache_path_;
  config.cache_size_bytes = static_cast<uint64_t>(10 * 1024 * 1024);
  // Disable proactive variants for predictable single-variant output.
  // Proactive generation is tested separately in worker_image_test.cc.
  config.proactive_image_variants = false;
  // Disable SSIMULACRA2 verification: the re-encode loop can push WebP
  // quality above the original JPEG size, triggering the size gate.
  // This test validates the e2e pipeline, not quality targets.
  config.quality_verify = false;

  Worker worker(config, &handler_);
  auto thread = StartWorker(worker);
  // Pre-populate with a real JPEG test image.
  std::string jpeg = ReadTestFile("jpeg/sjpeg6.jpg");
  ASSERT_FALSE(jpeg.empty()) << "Test JPEG not found";

  std::string cache_url = "/photo.jpg";
  std::string notification_url = "http://example.com/photo.jpg";
  CacheOriginal(worker.cache(), cache_url, jpeg);

  // Send image notification with WebP target.
  CapabilityMask webp_mask;
  webp_mask.set_image_format(CapabilityMask::ImageFormat::kWebP);

  CacheNotification notification;
  notification.url = notification_url;
  notification.scheme = "https";
  notification.content_type = ContentType::kImage;
  notification.capability_mask = webp_mask.Encode();

  auto send_result = SendNotification(socket_path_, notification);
  EXPECT_TRUE(send_result.success) << send_result.error_message;

  // Poll for WebP variant.
  std::string variant =
      PollVariant(worker.cache(), cache_url, webp_mask, kWebPPollIterations);
  EXPECT_FALSE(variant.empty()) << "WebP variant not written";
  if (variant.size() >= 4) {
    // Verify RIFF/WebP magic bytes.
    EXPECT_EQ('R', variant[0]);
    EXPECT_EQ('I', variant[1]);
    EXPECT_EQ('F', variant[2]);
    EXPECT_EQ('F', variant[3]);
  }

  worker.Shutdown();
  thread.join();
}

// =============================================================================
// GIF Animation Pipeline: animated GIF → WebP through full worker
// =============================================================================

TEST_F(PipelineTest, GifAnimationPipelineEndToEnd) {
  WorkerConfig config;
  config.socket_path = socket_path_;
  config.cache_path = cache_path_;
  config.cache_size_bytes = static_cast<uint64_t>(10 * 1024 * 1024);
  config.proactive_image_variants = false;

  Worker worker(config, &handler_);
  auto thread = StartWorker(worker);
  // Pre-populate with an animated GIF.
  std::string gif = ReadTestFile("gif/animated.gif");
  ASSERT_FALSE(gif.empty()) << "Test GIF not found";

  std::string cache_url = "/animation.gif";
  std::string notification_url = "http://example.com/animation.gif";
  CacheOriginal(worker.cache(), cache_url, gif);

  // Send image notification with WebP target.
  CapabilityMask webp_mask;
  webp_mask.set_image_format(CapabilityMask::ImageFormat::kWebP);

  CacheNotification notification;
  notification.url = notification_url;
  notification.scheme = "https";
  notification.content_type = ContentType::kImage;
  notification.capability_mask = webp_mask.Encode();

  auto send_result = SendNotification(socket_path_, notification);
  EXPECT_TRUE(send_result.success) << send_result.error_message;

  // Poll for variant. For small animated GIFs, the WebP output may be
  // larger, in which case the worker won't write a variant. We verify
  // the pipeline runs without crashing.
  CapabilityMask target = CapabilityMask::Decode(webp_mask.Encode());
  std::string variant = PollVariant(worker.cache(), cache_url, target,
                                    kGifAnimationPollIterations);
  // If a variant was written, verify it's WebP.
  if (variant.size() >= 4) {
    EXPECT_EQ('R', variant[0]);
    EXPECT_EQ('I', variant[1]);
    EXPECT_EQ('F', variant[2]);
    EXPECT_EQ('F', variant[3]);
  }

  worker.Shutdown();
  thread.join();
}

// =============================================================================
// Image Pipeline: JPEG → AVIF through full worker
// =============================================================================

TEST_F(PipelineTest, ImagePipelineAvifEndToEnd) {
  WorkerConfig config;
  config.socket_path = socket_path_;
  config.cache_path = cache_path_;
  config.cache_size_bytes = static_cast<uint64_t>(10 * 1024 * 1024);
  config.proactive_image_variants = false;
  config.avif_speed = 10;  // Fastest — MSVC-compiled libaom is slower

  Worker worker(config, &handler_);
  auto thread = StartWorker(worker);
  pagespeed::test::ScopedThreadJoin stop_worker(
      thread, [&worker] { worker.Shutdown(); });
  // Pre-populate with a real JPEG test image (use large one for AVIF win).
  std::string jpeg = ReadTestFile("jpeg/sjpeg6.jpg");
  ASSERT_FALSE(jpeg.empty()) << "Test JPEG not found";

  std::string cache_url = "/photo-avif.jpg";
  std::string notification_url = "http://example.com/photo-avif.jpg";
  CacheOriginal(worker.cache(), cache_url, jpeg);

  // Send image notification with AVIF target.
  CapabilityMask avif_mask;
  avif_mask.set_image_format(CapabilityMask::ImageFormat::kAvif);

  CacheNotification notification;
  notification.url = notification_url;
  notification.scheme = "https";
  notification.content_type = ContentType::kImage;
  notification.capability_mask = avif_mask.Encode();

  auto send_result = SendNotification(socket_path_, notification);
  EXPECT_TRUE(send_result.success) << send_result.error_message;

  // Wait for the worker to finish with the notification, then read the
  // variant once.  The AVIF is written only after every missing format of the
  // requested combination has been encoded, which takes tens of seconds in an
  // unoptimized build on an idle machine and several times that on a host
  // whose cores are shared with other jobs -- a fixed polling window measured
  // the host, and timed out with the worker still encoding.
  auto wait_start = std::chrono::steady_clock::now();
  ASSERT_TRUE(pagespeed::test::WaitForNotificationsRetired(worker, 1,
                                                           kWorkerStallBudget));
  auto wait_elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                             std::chrono::steady_clock::now() - wait_start)
                             .count();
  // Surface the observed latency so a creep stays visible in the logs.
  GTEST_LOG_(INFO) << "AVIF notification retired in " << wait_elapsed_ms
                   << " ms";
  // The upload sits at the identity slot here (the nginx layout), so the
  // pass also builds the optimized desktop JPEG over it, and an AVIF that is
  // not smaller than that JPEG is refused: the AVIF client is then served
  // the JPEG.  Either way it never gets more bytes than the JPEG client.
  CapabilityMask jpeg_mask;  // Desktop/Original/Identity = 0x08
  std::string jpeg_variant = ReadVariant(worker.cache(), cache_url, jpeg_mask);
  ASSERT_FALSE(jpeg_variant.empty()) << "optimized JPEG not written";
  EXPECT_LT(jpeg_variant.size(), jpeg.size())
      << "the identity slot still holds the upload";
  std::string variant = ReadVariant(worker.cache(), cache_url, avif_mask);
  if (!variant.empty()) {
    EXPECT_LT(variant.size(), jpeg_variant.size());
  }
  if (variant.size() >= 12) {
    // Verify AVIF magic bytes: "ftyp" at offset 4.
    EXPECT_EQ('f', variant[4]);
    EXPECT_EQ('t', variant[5]);
    EXPECT_EQ('y', variant[6]);
    EXPECT_EQ('p', variant[7]);
  }
}

// =============================================================================
// Cache Miss: worker handles missing entries gracefully
// =============================================================================

TEST_F(PipelineTest, CacheMissSilentlyIgnored) {
  WorkerConfig config;
  config.socket_path = socket_path_;
  config.cache_path = cache_path_;
  config.cache_size_bytes = static_cast<uint64_t>(10 * 1024 * 1024);

  Worker worker(config, &handler_);
  auto thread = StartWorker(worker);
  // Send notification for a URL that is NOT in cache.
  CacheNotification notification;
  notification.url = "http://example.com/does-not-exist.html";
  notification.scheme = "https";
  notification.content_type = ContentType::kHtml;
  notification.capability_mask = 0;

  auto send_result = SendNotification(socket_path_, notification);
  EXPECT_TRUE(send_result.success) << send_result.error_message;

  // Give the worker time to process (and not crash).
  std::this_thread::sleep_for(std::chrono::milliseconds(200));

  // Verify no variant was written (use path-only URL to match worker
  // normalization).
  CapabilityMask mask = CapabilityMask::Decode(0);
  std::string variant =
      ReadVariant(worker.cache(), "/does-not-exist.html", mask);
  EXPECT_TRUE(variant.empty());

  worker.Shutdown();
  thread.join();
}

// =============================================================================
// HTML Pipeline: preload hints stored for Early Hints (103)
// =============================================================================

TEST_F(PipelineTest, HtmlPipelineStoresHints) {
  // After the worker processes HTML, it stores preload hints at
  // mask=0xFFFFFFFF so nginx can send 103 Early Hints on future MISS
  // requests.  The hint list reflects the POST-transform reality: a sheet the
  // transform DEFERRED gets no hint (the 103 would re-promote the very
  // download it demoted), and a sheet left render-blocking DOES.
  //
  // This fixture is the render-blocking case, and by default: nothing here has
  // a browser analysis, so the empirical gate refuses to defer and both
  // stylesheets stay render-blocking — hinted, alongside the third-party
  // script's preconnect.  The deferred half of the same gate is covered by
  // WorkerTest.EarlyHintsOmitAsyncDeferredStylesheets, which seeds the
  // validated profile deferral requires.
  WorkerConfig config;
  config.socket_path = socket_path_;
  config.cache_path = cache_path_;
  config.cache_size_bytes = static_cast<uint64_t>(10 * 1024 * 1024);

  Worker worker(config, &handler_);
  auto thread = StartWorker(worker);
  // Pre-populate cache with HTML referencing external stylesheets and a
  // render-blocking third-party script (preconnect hint source).
  std::string cache_url = "/early-hints.html";
  std::string notification_url = "http://example.com/early-hints.html";
  std::string html =
      "<html><head>"
      "<link rel=\"stylesheet\" href=\"/reset.css\">"
      "<link rel=\"stylesheet\" href=\"/main.css\">"
      "<script src=\"https://cdn.example.net/app.js\"></script>"
      "<style>.hero { color: red; }</style>"
      "</head>"
      "<body><div class=\"hero\">Hello</div></body></html>";
  CacheOriginal(worker.cache(), cache_url, html, "text/html; charset=utf-8");

  // Pre-populate external stylesheets so the worker can complete
  // CSS gathering (otherwise it returns early on empty combined CSS).
  std::string css1 = ".hero { color: red; }";
  CacheOriginal(worker.cache(), "/reset.css", css1);
  std::string css2 = "body { margin: 0; }";
  CacheOriginal(worker.cache(), "/main.css", css2);

  // Send notification via the notification sender API.
  CacheNotification notification;
  notification.url = notification_url;
  notification.scheme = "https";
  notification.content_type = ContentType::kHtml;
  notification.capability_mask = 0;

  auto send_result = SendNotification(socket_path_, notification);
  EXPECT_TRUE(send_result.success) << send_result.error_message;

  // Poll for the hints sentinel entry.
  std::string hints =
      PollSentinel(worker.cache(), cache_url, SentinelId::kEarlyHints);
  EXPECT_FALSE(hints.empty()) << "Preload hints not stored";
  if (!hints.empty()) {
    // The third-party script origin is hinted for preconnect (bare, not
    // CORS: a plain <script src> fetches in no-cors mode).
    EXPECT_NE(hints.find("preconnect:https://cdn.example.net"),
              std::string::npos);
    // Both stylesheets stayed render-blocking, so both are hinted.
    EXPECT_NE(hints.find("/reset.css"), std::string::npos);
    EXPECT_NE(hints.find("/main.css"), std::string::npos);
  }

  // Also verify the HTML variant was still written normally.
  CapabilityMask mask = CapabilityMask::Decode(0);
  std::string variant = PollVariant(worker.cache(), cache_url, mask);
  EXPECT_FALSE(variant.empty()) << "HTML variant not written";
  if (!variant.empty()) {
    // Refusing to defer leaves the sheets blocking, and no block is inlined
    // next to them.
    EXPECT_EQ(variant.find("data-pagespeed-critical"), std::string::npos);
    EXPECT_EQ(variant.find("data-pagespeed-async"), std::string::npos);
  }

  worker.Shutdown();
  thread.join();
}

}  // namespace
}  // namespace pagespeed
