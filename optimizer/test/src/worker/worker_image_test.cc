// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

// Image optimization tests
//
// - Unsupported and oversized image types, SVG candidacy and vectorization
// - Proactive multi-format creation, decline tombstones and verification
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
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "avif/avif.h"
#include "gtest/gtest.h"
#include "lib/base/message_handler.h"
#include "lib/cache/freshness.h"
#include "lib/classify/alternate_id.h"
#include "lib/classify/alternate_metadata.h"
#include "lib/classify/capability_mask.h"
#include "src/nginx/etag_util.h"
#include "src/proto/worker_ipc.h"
#include "src/worker/browser_analysis_manager.h"
#include "src/worker/critical_css_extractor.h"
#include "src/worker/worker.h"
#include "test/test_util/pipe_client.h"
#include "test/test_util/tcp_client.h"
#include "test/test_util/worker_quiescence.h"

namespace pagespeed {
namespace {

TEST_F(WorkerTest, ImageNotificationDeferred) {
  WorkerConfig config;
  config.socket_path = socket_path_;
  config.cache_path = cache_path_;
  config.cache_size_bytes = static_cast<uint64_t>(10 * 1024 * 1024);

  Worker worker(config, nullptr);
  ASSERT_TRUE(worker.Initialize());

  std::thread worker_thread([&worker]() { worker.Run(); });
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  // Send image notification — should be accepted without error
  CacheNotification notification;
  notification.url = "http://example.com/photo.jpg";
  notification.scheme = "https";
  notification.content_type = ContentType::kImage;
  notification.capability_mask = 0x00000001;
  SendNotification(notification);

  std::this_thread::sleep_for(std::chrono::milliseconds(100));

  worker.Shutdown();
  worker_thread.join();
}

// =============================================================================
// Additional coverage tests for previously untested paths
// =============================================================================

// Synthetic AVIF authoring via libavif (the test links @libavif), same
// helper as the transcoder suite: noise content is the reliable
// constructor for a below-floor verify verdict, and AVIF is the one input
// format whose original-format arm has no optimizer.
enum class SyntheticPattern : uint8_t { kFlat, kNoise };

std::string MakeSyntheticAvif(int width, int height, bool grayscale,
                              SyntheticPattern pattern, int value, int quality,
                              uint32_t seed) {
  avifImage* image = avifImageCreate(
      width, height, 8,
      grayscale ? AVIF_PIXEL_FORMAT_YUV400 : AVIF_PIXEL_FORMAT_YUV420);
  if (image == nullptr) return {};

  std::string rgb(static_cast<size_t>(width) * height * 3, '\0');
  uint32_t s = seed != 0 ? seed : 1;
  for (char& byte : rgb) {
    if (pattern == SyntheticPattern::kFlat) {
      byte = static_cast<char>(value);
    } else {
      s = s * 1664525u + 1013904223u;
      byte = static_cast<char>((s >> 16) & 0xFF);
    }
  }

  avifRGBImage rgb_image;
  avifRGBImageSetDefaults(&rgb_image, image);
  rgb_image.depth = 8;
  rgb_image.format = AVIF_RGB_FORMAT_RGB;
  rgb_image.pixels = reinterpret_cast<uint8_t*>(rgb.data());
  rgb_image.rowBytes = static_cast<size_t>(width) * 3;
  avifResult r = avifImageRGBToYUV(image, &rgb_image);
  if (r != AVIF_RESULT_OK) {
    avifImageDestroy(image);
    return {};
  }

  avifEncoder* encoder = avifEncoderCreate();
  if (encoder == nullptr) {
    avifImageDestroy(image);
    return {};
  }
  encoder->quality = quality;
  encoder->speed = 8;  // Authoring speed; determinism is what matters.
  avifRWData out = AVIF_DATA_EMPTY;
  r = avifEncoderWrite(encoder, image, &out);
  avifEncoderDestroy(encoder);
  avifImageDestroy(image);
  if (r != AVIF_RESULT_OK) {
    avifRWDataFree(&out);
    return {};
  }
  std::string bytes(reinterpret_cast<const char*>(out.data), out.size);
  avifRWDataFree(&out);
  return bytes;
}

TEST_F(WorkerTest, ImageNotificationUnsupportedType) {
  // An image with unsupported origin_content_type (e.g., image/x-icon)
  // should have only compressed variants written, not transcoded.
  WorkerConfig config;
  config.socket_path = socket_path_;
  config.cache_path = cache_path_;
  config.cache_size_bytes = static_cast<uint64_t>(10 * 1024 * 1024);

  NullMessageHandler handler;
  Worker worker(config, &handler);
  ASSERT_TRUE(worker.Initialize());

  // Create fake ICO data with metadata indicating image/x-icon.
  std::string url = "http://example.com/favicon.ico";
  std::string ico_data = "fake ico content for testing purposes";
  AlternateMetadata meta;
  meta.content_type = ContentType::kOther;
  meta.origin_content_type = "image/x-icon";
  CacheOriginalWithMeta(worker.cache(), url, ico_data, meta);

  std::thread worker_thread([&worker]() { worker.Run(); });
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  CacheNotification notification;
  notification.url = url;
  notification.scheme = "https";
  notification.content_type = ContentType::kImage;
  notification.capability_mask = CapabilityMask().Encode();
  SendNotification(notification);

  std::this_thread::sleep_for(std::chrono::milliseconds(500));

  // No WebP/AVIF transcoded variant should be written.
  CapabilityMask webp_mask;
  webp_mask.set_image_format(CapabilityMask::ImageFormat::kWebP);
  std::string webp_variant = ReadVariant(worker.cache(), url, webp_mask);
  EXPECT_TRUE(webp_variant.empty())
      << "Unsupported image type should not produce WebP variant";

  // Compressed variants (gzip/brotli) of the original should exist.
  CapabilityMask gz_mask;
  gz_mask.set_transfer_encoding(CapabilityMask::TransferEncoding::kGzip);
  std::string gz_variant = ReadVariant(worker.cache(), url, gz_mask);
  EXPECT_FALSE(gz_variant.empty())
      << "Unsupported image type should still have gzip variant";

  worker.Shutdown();
  worker_thread.join();
}

TEST_F(WorkerTest, ImageNotificationTooLarge) {
  // Image content exceeding max_image_size should be skipped.
  WorkerConfig config;
  config.socket_path = socket_path_;
  config.cache_path = cache_path_;
  config.cache_size_bytes = static_cast<uint64_t>(10 * 1024 * 1024);
  config.max_image_size = 100;  // Very small limit

  NullMessageHandler handler;
  Worker worker(config, &handler);
  ASSERT_TRUE(worker.Initialize());

  std::string url = "http://example.com/huge-image.jpg";
  // Use a real JPEG file that exceeds 100 bytes.
  std::string image_data = ReadTestFile("jpeg/quality100.jpg");
  ASSERT_FALSE(image_data.empty()) << "Test JPEG not found";
  ASSERT_GT(image_data.size(), 100u);
  CacheOriginal(worker.cache(), url, image_data);

  std::thread worker_thread([&worker]() { worker.Run(); });
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  CacheNotification notification;
  notification.url = url;
  notification.scheme = "https";
  notification.content_type = ContentType::kImage;
  notification.capability_mask = CapabilityMask().Encode();
  SendNotification(notification);

  // Wait for notification to be received.
  for (int i = 0; i < 40 * kSanitizerBudgetScale; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    if (worker.stats().notifications_received.load() >= 1) break;
  }

  // No transcoded variant should be written.
  CapabilityMask webp_mask;
  webp_mask.set_image_format(CapabilityMask::ImageFormat::kWebP);
  std::string variant = ReadVariant(worker.cache(), url, webp_mask);
  EXPECT_TRUE(variant.empty())
      << "Too-large image should not produce a variant";

  // Image processing counter should not have incremented.
  EXPECT_EQ(worker.stats().images_processed.load(), 0u);

  worker.Shutdown();
  worker_thread.join();
}

TEST_F(WorkerTest, ImageNotificationSvgPassthrough) {
  // SVG content with origin_content_type "image/svg+xml" should
  // produce only compressed variants without raster processing.
  WorkerConfig config;
  config.socket_path = socket_path_;
  config.cache_path = cache_path_;
  config.cache_size_bytes = static_cast<uint64_t>(10 * 1024 * 1024);

  NullMessageHandler handler;
  Worker worker(config, &handler);
  ASSERT_TRUE(worker.Initialize());

  std::string url = "http://example.com/logo.svg";
  std::string svg_data =
      "<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"100\" "
      "height=\"100\"><circle cx=\"50\" cy=\"50\" r=\"40\"/></svg>";
  AlternateMetadata meta;
  meta.content_type = ContentType::kOther;
  meta.origin_content_type = "image/svg+xml";
  CacheOriginalWithMeta(worker.cache(), url, svg_data, meta);

  std::thread worker_thread([&worker]() { worker.Run(); });
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  CacheNotification notification;
  notification.url = url;
  notification.scheme = "https";
  notification.content_type = ContentType::kImage;
  notification.capability_mask = CapabilityMask().Encode();
  SendNotification(notification);

  std::this_thread::sleep_for(std::chrono::milliseconds(500));

  // No WebP variant should be produced (SVG is already vector format).
  CapabilityMask webp_mask;
  webp_mask.set_image_format(CapabilityMask::ImageFormat::kWebP);
  std::string webp_variant = ReadVariant(worker.cache(), url, webp_mask);
  EXPECT_TRUE(webp_variant.empty()) << "SVG should not produce a WebP variant";

  // Compressed variants of the SVG should exist.
  CapabilityMask gz_mask;
  gz_mask.set_transfer_encoding(CapabilityMask::TransferEncoding::kGzip);
  std::string gz_variant = ReadVariant(worker.cache(), url, gz_mask);
  EXPECT_FALSE(gz_variant.empty())
      << "SVG should have a gzip compressed variant";

  worker.Shutdown();
  worker_thread.join();
}

// --- B. Image Path Coverage ---

TEST_F(WorkerTest, ImageOriginalNotInCache) {
  // Send image notification for URL not in cache — should not crash
  // and should not write any variant.  Covers worker.cc ~1916-1921.
  WorkerConfig config;
  config.socket_path = socket_path_;
  config.cache_path = cache_path_;
  config.cache_size_bytes = static_cast<uint64_t>(10 * 1024 * 1024);

  NullMessageHandler handler;
  Worker worker(config, &handler);
  ASSERT_TRUE(worker.Initialize());

  std::thread worker_thread([&worker]() { worker.Run(); });
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  CacheNotification notification;
  notification.url = "http://example.com/image-not-cached.jpg";
  notification.scheme = "https";
  notification.content_type = ContentType::kImage;
  notification.capability_mask = CapabilityMask().Encode();
  SendNotification(notification);

  for (int i = 0; i < 40 * kSanitizerBudgetScale; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    if (worker.stats().notifications_received.load() >= 1) break;
  }

  EXPECT_GE(worker.stats().notifications_received.load(), 1u);
  EXPECT_EQ(worker.stats().images_processed.load(), 0u);
  EXPECT_EQ(worker.stats().variants_written.load(), 0u);

  worker.Shutdown();
  worker_thread.join();
}

TEST_F(WorkerTest, ImageUnsupportedFormatBmp) {
  // BMP images should not be transcoded but should get compressed variants.
  // Covers the unsupported image type path (worker.cc ~1953-1992).
  WorkerConfig config;
  config.socket_path = socket_path_;
  config.cache_path = cache_path_;
  config.cache_size_bytes = static_cast<uint64_t>(10 * 1024 * 1024);

  NullMessageHandler handler;
  Worker worker(config, &handler);
  ASSERT_TRUE(worker.Initialize());

  std::string url = "http://example.com/image.bmp";
  std::string bmp_data = "fake bmp content for testing purposes";
  AlternateMetadata meta;
  meta.content_type = ContentType::kOther;
  meta.origin_content_type = "image/bmp";
  CacheOriginalWithMeta(worker.cache(), url, bmp_data, meta);

  std::thread worker_thread([&worker]() { worker.Run(); });
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  CacheNotification notification;
  notification.url = url;
  notification.scheme = "https";
  notification.content_type = ContentType::kImage;
  notification.capability_mask = CapabilityMask().Encode();
  SendNotification(notification);

  std::this_thread::sleep_for(std::chrono::milliseconds(500));

  // No WebP/AVIF transcoded variant should be written.
  CapabilityMask webp_mask;
  webp_mask.set_image_format(CapabilityMask::ImageFormat::kWebP);
  std::string webp_variant = ReadVariant(worker.cache(), url, webp_mask);
  EXPECT_TRUE(webp_variant.empty()) << "BMP should not produce WebP variant";

  // Compressed variants of the original should exist.
  CapabilityMask gz_mask;
  gz_mask.set_transfer_encoding(CapabilityMask::TransferEncoding::kGzip);
  std::string gz_variant = ReadVariant(worker.cache(), url, gz_mask);
  EXPECT_FALSE(gz_variant.empty())
      << "BMP should still have gzip compressed variant";

  worker.Shutdown();
  worker_thread.join();
}

TEST_F(WorkerTest, ImageUnsupportedFormatTiff) {
  // TIFF images should not be transcoded but should get compressed variants.
  // Covers the unsupported image type path (worker.cc ~1953-1992).
  WorkerConfig config;
  config.socket_path = socket_path_;
  config.cache_path = cache_path_;
  config.cache_size_bytes = static_cast<uint64_t>(10 * 1024 * 1024);

  NullMessageHandler handler;
  Worker worker(config, &handler);
  ASSERT_TRUE(worker.Initialize());

  std::string url = "http://example.com/image.tiff";
  std::string tiff_data = "fake tiff content for testing";
  AlternateMetadata meta;
  meta.content_type = ContentType::kOther;
  meta.origin_content_type = "image/tiff";
  CacheOriginalWithMeta(worker.cache(), url, tiff_data, meta);

  std::thread worker_thread([&worker]() { worker.Run(); });
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  CacheNotification notification;
  notification.url = url;
  notification.scheme = "https";
  notification.content_type = ContentType::kImage;
  notification.capability_mask = CapabilityMask().Encode();
  SendNotification(notification);

  std::this_thread::sleep_for(std::chrono::milliseconds(500));

  // No WebP/AVIF transcoded variant should be written.
  CapabilityMask webp_mask;
  webp_mask.set_image_format(CapabilityMask::ImageFormat::kWebP);
  std::string webp_variant = ReadVariant(worker.cache(), url, webp_mask);
  EXPECT_TRUE(webp_variant.empty()) << "TIFF should not produce WebP variant";

  // Compressed variants of the original should exist.
  CapabilityMask br_mask;
  br_mask.set_transfer_encoding(CapabilityMask::TransferEncoding::kBrotli);
  std::string br_variant = ReadVariant(worker.cache(), url, br_mask);
  EXPECT_FALSE(br_variant.empty())
      << "TIFF should still have brotli compressed variant";

  worker.Shutdown();
  worker_thread.join();
}

TEST_F(WorkerTest, ImageProactiveAllSiblingsExistSkips) {
  // When all format/viewport siblings already exist in cache, proactive
  // generation should be skipped entirely.  Covers worker.cc ~2457-2462.
  WorkerConfig config;
  config.socket_path = socket_path_;
  config.cache_path = cache_path_;
  config.cache_size_bytes = static_cast<uint64_t>(10 * 1024 * 1024);
  config.proactive_image_variants = true;
  config.proactive_viewport_variants = false;
  config.proactive_savedata_variants = false;
  config.proactive_density_variants = false;

  NullMessageHandler handler;
  Worker worker(config, &handler);
  ASSERT_TRUE(worker.Initialize());

  std::string url = "http://example.com/all-siblings.jpg";
  std::string jpeg = ReadTestFile("jpeg/quality100.jpg");
  ASSERT_FALSE(jpeg.empty()) << "Test JPEG not found";
  CacheOriginal(worker.cache(), url, jpeg);

  std::thread worker_thread([&worker]() { worker.Run(); });
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  // Send first notification to generate all format siblings at Desktop.
  CapabilityMask notif_mask(
      CapabilityMask::ImageFormat::kWebP, CapabilityMask::Viewport::kDesktop,
      CapabilityMask::PixelDensity::k1x, CapabilityMask::SaveData::kOff,
      CapabilityMask::TransferEncoding::kIdentity);
  CacheNotification notification;
  notification.url = url;
  notification.scheme = "https";
  notification.content_type = ContentType::kImage;
  notification.capability_mask = notif_mask.Encode();
  SendNotification(notification);

  // Wait for all proactive variants (WebP, AVIF, optimized original).
  for (int i = 0; i < 60 * kSanitizerBudgetScale; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    if (worker.stats().images_processed.load() >= 2) break;
  }
  EXPECT_GE(worker.stats().images_processed.load(), 2u)
      << "At least WebP + AVIF should be written";

  // images_processed moves before the in-flight entry is erased.  Drain first
  // so the re-send below reaches the all-siblings check instead of being
  // dropped by the in-flight guard.
  WaitForWorkerIdle(worker);

  uint64_t variants_before = worker.stats().variants_written.load();

  // Clear dedup to allow re-processing.
  worker.ClearDedupAndCooldown(url, "");

  // Send same notification again — all siblings already exist in cache,
  // so no new variants should be written.
  SendNotification(notification);
  std::this_thread::sleep_for(std::chrono::milliseconds(1000));

  uint64_t variants_after = worker.stats().variants_written.load();
  EXPECT_EQ(variants_after, variants_before)
      << "No new variants should be written when all siblings already exist";

  worker.Shutdown();
  worker_thread.join();
}

TEST_F(WorkerTest, ImageNonProactiveGeneratesAllFormatsAtRequestedDimension) {
  // When proactive_image_variants is disabled, all 3 formats (WebP, AVIF,
  // optimized original) should be generated at the requested viewport/density/
  // save-data, but NOT at other viewports/densities/save-data combinations.
  WorkerConfig config;
  config.socket_path = socket_path_;
  config.cache_path = cache_path_;
  config.cache_size_bytes = static_cast<uint64_t>(10 * 1024 * 1024);
  config.proactive_image_variants = false;
  config.proactive_viewport_variants = false;
  config.proactive_savedata_variants = false;
  config.proactive_density_variants = false;

  NullMessageHandler handler;
  Worker worker(config, &handler);
  ASSERT_TRUE(worker.Initialize());

  std::string url = "http://example.com/non-proactive-formats.jpg";
  std::string jpeg = ReadTestFile("jpeg/quality100.jpg");
  ASSERT_FALSE(jpeg.empty()) << "Test JPEG not found";
  CacheOriginal(worker.cache(), url, jpeg);

  std::thread worker_thread([&worker]() { worker.Run(); });
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  // Request WebP/Desktop specifically with non-proactive mode.
  CapabilityMask webp_mask(
      CapabilityMask::ImageFormat::kWebP, CapabilityMask::Viewport::kDesktop,
      CapabilityMask::PixelDensity::k1x, CapabilityMask::SaveData::kOff,
      CapabilityMask::TransferEncoding::kIdentity);
  CacheNotification notification;
  notification.url = url;
  notification.scheme = "https";
  notification.content_type = ContentType::kImage;
  notification.capability_mask = webp_mask.Encode();
  SendNotification(notification);

  // Poll for the WebP variant.
  std::string webp_variant;
  for (int i = 0; i < 60 * kSanitizerBudgetScale; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    webp_variant = ReadVariant(worker.cache(), url, webp_mask);
    if (!webp_variant.empty()) break;
  }
  EXPECT_FALSE(webp_variant.empty()) << "WebP variant should be written";

  // AVIF at Desktop should also be generated (all formats at requested dim).
  CapabilityMask avif_mask(
      CapabilityMask::ImageFormat::kAvif, CapabilityMask::Viewport::kDesktop,
      CapabilityMask::PixelDensity::k1x, CapabilityMask::SaveData::kOff,
      CapabilityMask::TransferEncoding::kIdentity);
  std::string avif_variant;
  for (int i = 0; i < 20 * kSanitizerBudgetScale; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    avif_variant = ReadVariant(worker.cache(), url, avif_mask);
    if (!avif_variant.empty()) break;
  }
  EXPECT_FALSE(avif_variant.empty())
      << "AVIF should be generated at the requested dimension";

  // Optimized original at Desktop should also be generated.
  CapabilityMask orig_mask(CapabilityMask::ImageFormat::kOriginal,
                           CapabilityMask::Viewport::kDesktop,
                           CapabilityMask::PixelDensity::k1x,
                           CapabilityMask::SaveData::kOff,
                           CapabilityMask::TransferEncoding::kIdentity);
  std::string orig_variant;
  for (int i = 0; i < 20 * kSanitizerBudgetScale; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    orig_variant = ReadVariant(worker.cache(), url, orig_mask);
    if (!orig_variant.empty()) break;
  }
  // Note: optimized_original may be absent if re-encoding yields no savings,
  // which is an acceptable outcome for high-quality source images.

  // Mobile viewport should NOT be generated (non-proactive).
  CapabilityMask mobile_mask(
      CapabilityMask::ImageFormat::kWebP, CapabilityMask::Viewport::kMobile,
      CapabilityMask::PixelDensity::k1x, CapabilityMask::SaveData::kOff,
      CapabilityMask::TransferEncoding::kIdentity);
  std::string mobile_variant = ReadVariant(worker.cache(), url, mobile_mask);
  EXPECT_TRUE(mobile_variant.empty())
      << "Mobile WebP should not be generated in non-proactive mode";

  // Tablet viewport should NOT be generated (non-proactive).
  CapabilityMask tablet_mask(
      CapabilityMask::ImageFormat::kWebP, CapabilityMask::Viewport::kTablet,
      CapabilityMask::PixelDensity::k1x, CapabilityMask::SaveData::kOff,
      CapabilityMask::TransferEncoding::kIdentity);
  std::string tablet_variant = ReadVariant(worker.cache(), url, tablet_mask);
  EXPECT_TRUE(tablet_variant.empty())
      << "Tablet WebP should not be generated in non-proactive mode";

  // Save-Data kOn should NOT be generated (non-proactive).
  CapabilityMask sd_mask(
      CapabilityMask::ImageFormat::kWebP, CapabilityMask::Viewport::kDesktop,
      CapabilityMask::PixelDensity::k1x, CapabilityMask::SaveData::kOn,
      CapabilityMask::TransferEncoding::kIdentity);
  std::string sd_variant = ReadVariant(worker.cache(), url, sd_mask);
  EXPECT_TRUE(sd_variant.empty())
      << "Save-Data WebP should not be generated in non-proactive mode";

  // 2x density should NOT be generated (non-proactive).
  CapabilityMask den_mask(
      CapabilityMask::ImageFormat::kWebP, CapabilityMask::Viewport::kDesktop,
      CapabilityMask::PixelDensity::k2xPlus, CapabilityMask::SaveData::kOff,
      CapabilityMask::TransferEncoding::kIdentity);
  std::string den_variant = ReadVariant(worker.cache(), url, den_mask);
  EXPECT_TRUE(den_variant.empty())
      << "2x density WebP should not be generated in non-proactive mode";

  // Verify the WebP variant is valid WebP (starts with "RIFF").
  if (!webp_variant.empty()) {
    EXPECT_GE(webp_variant.size(), 4u);
    EXPECT_EQ(webp_variant.substr(0, 4), "RIFF")
        << "WebP variant should start with RIFF header";
  }

  worker.Shutdown();
  worker_thread.join();
}

TEST_F(WorkerTest, ImageCacheNotInitializedSkips) {
  // When cache is not initialized, image notifications should be skipped.
  // Covers worker.cc ~1902-1908.
  WorkerConfig config;
  config.socket_path = socket_path_;
  // No cache_path — cache_ will be null.

  NullMessageHandler handler;
  Worker worker(config, &handler);
  ASSERT_TRUE(worker.Initialize());

  std::thread worker_thread([&worker]() { worker.Run(); });
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  CacheNotification notification;
  notification.url = "http://example.com/no-cache-image.jpg";
  notification.scheme = "https";
  notification.content_type = ContentType::kImage;
  notification.capability_mask = CapabilityMask().Encode();
  SendNotification(notification);

  for (int i = 0; i < 40 * kSanitizerBudgetScale; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    if (worker.stats().notifications_received.load() >= 1) break;
  }

  EXPECT_GE(worker.stats().notifications_received.load(), 1u);
  EXPECT_EQ(worker.stats().images_processed.load(), 0u);

  worker.Shutdown();
  worker_thread.join();
}

TEST_F(WorkerTest, ImageSvgCandidacyEvaluationDetectMode) {
  // In detect mode, SVG candidacy should be evaluated and stats
  // updated, but no SVG variant should be written.
  // Covers lines 2025-2089 (SVG candidacy evaluation).
  WorkerConfig config;
  config.socket_path = socket_path_;
  config.cache_path = cache_path_;
  config.cache_size_bytes = static_cast<uint64_t>(10 * 1024 * 1024);
  config.svg_mode = SvgMode::kDetect;
  config.proactive_image_variants = false;
  config.proactive_viewport_variants = false;
  config.proactive_savedata_variants = false;
  config.proactive_density_variants = false;

  NullMessageHandler handler;
  Worker worker(config, &handler);
  ASSERT_TRUE(worker.Initialize());

  // Use a small PNG (icon-like, good SVG candidate).
  std::string url = "http://example.com/icon-detect.png";
  std::string png = ReadTestFile("png/pagespeed-128.png");
  ASSERT_FALSE(png.empty()) << "Test PNG not found";

  AlternateMetadata meta;
  meta.content_type = ContentType::kImage;
  meta.origin_content_type = "image/png";
  CacheOriginalWithMeta(worker.cache(), url, png, meta);

  std::thread worker_thread([&worker]() { worker.Run(); });
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  CapabilityMask webp_mask(
      CapabilityMask::ImageFormat::kWebP, CapabilityMask::Viewport::kDesktop,
      CapabilityMask::PixelDensity::k1x, CapabilityMask::SaveData::kOff,
      CapabilityMask::TransferEncoding::kIdentity);
  CacheNotification notification;
  notification.url = url;
  notification.scheme = "https";
  notification.content_type = ContentType::kImage;
  notification.capability_mask = webp_mask.Encode();
  SendNotification(notification);

  // Wait for processing.
  for (int i = 0; i < 60 * kSanitizerBudgetScale; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    if (worker.stats().images_processed.load() >= 1) break;
  }
  EXPECT_GE(worker.stats().images_processed.load(), 1u);

  // SVG candidacy should have been evaluated.
  EXPECT_GE(worker.stats().svg_candidates_evaluated.load(), 1u)
      << "SVG candidacy should be evaluated in detect mode";

  // No SVG variant should be written in detect mode.
  CapabilityMask svg_mask(
      CapabilityMask::ImageFormat::kSvg, CapabilityMask::Viewport::kDesktop,
      CapabilityMask::PixelDensity::k1x, CapabilityMask::SaveData::kOff,
      CapabilityMask::TransferEncoding::kIdentity);
  std::string svg_variant = ReadVariant(worker.cache(), url, svg_mask);
  EXPECT_TRUE(svg_variant.empty())
      << "No SVG variant should be written in detect mode";

  worker.Shutdown();
  worker_thread.join();
}

TEST_F(WorkerTest, ImageSvgAutoModeVectorizesAndWrites) {
  // In auto mode with a good SVG candidate, the full pipeline should run:
  // evaluate -> vectorize -> size gate -> cache write.
  // Covers lines 2107-2190 (vectorization) and 2537-2614 (size gate + write).
  WorkerConfig config;
  config.socket_path = socket_path_;
  config.cache_path = cache_path_;
  config.cache_size_bytes = static_cast<uint64_t>(10 * 1024 * 1024);
  config.svg_mode = SvgMode::kAuto;
  config.svg_candidacy_threshold = 1;  // Very low threshold to pass.
  config.svg_max_pixels = 1000000;
  config.svg_max_paths = 10000;
  config.proactive_image_variants = true;
  config.proactive_viewport_variants = false;
  config.proactive_savedata_variants = false;
  config.proactive_density_variants = false;

  NullMessageHandler handler;
  Worker worker(config, &handler);
  ASSERT_TRUE(worker.Initialize());

  // Use the pagespeed icon PNG — likely a good SVG candidate (logo/icon).
  std::string url = "http://example.com/icon-auto.png";
  std::string png = ReadTestFile("png/pagespeed-128.png");
  ASSERT_FALSE(png.empty()) << "Test PNG not found";

  AlternateMetadata meta;
  meta.content_type = ContentType::kImage;
  meta.origin_content_type = "image/png";
  CacheOriginalWithMeta(worker.cache(), url, png, meta);

  std::thread worker_thread([&worker]() { worker.Run(); });
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  CapabilityMask webp_mask(
      CapabilityMask::ImageFormat::kWebP, CapabilityMask::Viewport::kDesktop,
      CapabilityMask::PixelDensity::k1x, CapabilityMask::SaveData::kOff,
      CapabilityMask::TransferEncoding::kIdentity);
  CacheNotification notification;
  notification.url = url;
  notification.scheme = "https";
  notification.content_type = ContentType::kImage;
  notification.capability_mask = webp_mask.Encode();
  SendNotification(notification);

  // Wait for image processing (SVG pipeline may take a moment).
  for (int i = 0; i < 80 * kSanitizerBudgetScale; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    if (worker.stats().images_processed.load() >= 1) break;
  }
  EXPECT_GE(worker.stats().images_processed.load(), 1u);

  // SVG candidacy should have been evaluated.
  EXPECT_GE(worker.stats().svg_candidates_evaluated.load(), 1u)
      << "SVG candidacy should be evaluated in auto mode";

  // Check if vectorization was attempted (candidate may or may not pass).
  // The vectorize time counter increments only when vectorization runs.
  bool vectorization_attempted =
      worker.stats().svg_vectorized.load() > 0 ||
      worker.stats().svg_candidates_rejected.load() > 0 ||
      worker.stats().svg_path_count_rejected.load() > 0 ||
      worker.stats().svg_fidelity_rejected.load() > 0 ||
      worker.stats().svg_timeout_exceeded.load() > 0;
  EXPECT_TRUE(vectorization_attempted ||
              worker.stats().svg_candidates_rejected.load() > 0)
      << "SVG pipeline should have either vectorized or rejected";

  // If vectorization succeeded, check for SVG variant.
  if (worker.stats().svg_written.load() > 0) {
    CapabilityMask svg_mask(
        CapabilityMask::ImageFormat::kSvg, CapabilityMask::Viewport::kDesktop,
        CapabilityMask::PixelDensity::k1x, CapabilityMask::SaveData::kOff,
        CapabilityMask::TransferEncoding::kIdentity);
    std::string svg_variant = ReadVariant(worker.cache(), url, svg_mask);
    EXPECT_FALSE(svg_variant.empty())
        << "SVG variant should exist when svg_written > 0";
    if (!svg_variant.empty()) {
      // Should be valid SVG (starts with <svg or similar).
      EXPECT_NE(svg_variant.find("<svg"), std::string::npos)
          << "SVG variant should contain SVG markup";
    }
    // Bytes saved should be tracked.
    EXPECT_GT(worker.stats().svg_bytes_saved.load(), 0u)
        << "SVG bytes saved should be tracked";
  }

  worker.Shutdown();
  worker_thread.join();
}

TEST_F(WorkerTest, ImageSvgPreviewModeVectorizesButDoesNotWrite) {
  // In preview mode, SVG should be vectorized but NOT written to cache
  // for serving.  Covers lines 2615-2626 (preview mode branch).
  WorkerConfig config;
  config.socket_path = socket_path_;
  config.cache_path = cache_path_;
  config.cache_size_bytes = static_cast<uint64_t>(10 * 1024 * 1024);
  config.svg_mode = SvgMode::kPreview;
  config.svg_candidacy_threshold = 1;
  config.svg_max_pixels = 1000000;
  config.svg_max_paths = 10000;
  config.proactive_image_variants = false;
  config.proactive_viewport_variants = false;
  config.proactive_savedata_variants = false;
  config.proactive_density_variants = false;

  NullMessageHandler handler;
  Worker worker(config, &handler);
  ASSERT_TRUE(worker.Initialize());

  std::string url = "http://example.com/icon-preview.png";
  std::string png = ReadTestFile("png/pagespeed-128.png");
  ASSERT_FALSE(png.empty()) << "Test PNG not found";

  AlternateMetadata meta;
  meta.content_type = ContentType::kImage;
  meta.origin_content_type = "image/png";
  CacheOriginalWithMeta(worker.cache(), url, png, meta);

  std::thread worker_thread([&worker]() { worker.Run(); });
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  CapabilityMask webp_mask(
      CapabilityMask::ImageFormat::kWebP, CapabilityMask::Viewport::kDesktop,
      CapabilityMask::PixelDensity::k1x, CapabilityMask::SaveData::kOff,
      CapabilityMask::TransferEncoding::kIdentity);
  CacheNotification notification;
  notification.url = url;
  notification.scheme = "https";
  notification.content_type = ContentType::kImage;
  notification.capability_mask = webp_mask.Encode();
  SendNotification(notification);

  // Wait for processing.
  for (int i = 0; i < 80 * kSanitizerBudgetScale; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    if (worker.stats().images_processed.load() >= 1) break;
  }

  // SVG candidacy should have been evaluated.
  EXPECT_GE(worker.stats().svg_candidates_evaluated.load(), 1u);

  // In preview mode, svg_written should NOT increment even if
  // vectorization succeeded.
  EXPECT_EQ(worker.stats().svg_written.load(), 0u)
      << "SVG should not be written in preview mode";

  // No SVG variant should exist in cache.
  CapabilityMask svg_mask(
      CapabilityMask::ImageFormat::kSvg, CapabilityMask::Viewport::kDesktop,
      CapabilityMask::PixelDensity::k1x, CapabilityMask::SaveData::kOff,
      CapabilityMask::TransferEncoding::kIdentity);
  std::string svg_variant = ReadVariant(worker.cache(), url, svg_mask);
  EXPECT_TRUE(svg_variant.empty())
      << "SVG variant should not be in cache in preview mode";

  worker.Shutdown();
  worker_thread.join();
}

TEST_F(WorkerTest, ImageProactiveWithContentClassCounting) {
  // Proactive image transcode with content analysis enabled should
  // update content class stats counters.
  // Covers lines 2265-2301 (proactive transcode loop) and
  // 2376-2427 (content class counting and learned quality stats).
  //
  // Content analysis only runs in TranscodeMultiResized (pixel decode path),
  // which requires target_width > 0 AND actual resize to occur (otherwise
  // the no-resize optimization short-circuits to TranscodeMulti).
  // quality100.jpg is 200x200, so we set mobile_width to 100 to force resize.
  WorkerConfig config;
  config.socket_path = socket_path_;
  config.cache_path = cache_path_;
  config.cache_size_bytes = static_cast<uint64_t>(10 * 1024 * 1024);
  config.proactive_image_variants = true;
  config.proactive_viewport_variants = false;
  config.proactive_savedata_variants = false;
  config.proactive_density_variants = false;
  config.content_analysis = true;
  config.mobile_width =
      100;  // Smaller than quality100.jpg (200px) to force resize
  // This test is about content-class COUNTING, not verification: turn the
  // verifier off so the tiny (100px) resized variants store unconditionally.
  // On images this small SSIMULACRA2 scores structurally low, so under the
  // binding verify verdict both cross-format variants decline below the
  // floor — decline mechanics are pinned in the transcoder suite.
  config.quality_verify = false;

  NullMessageHandler handler;
  Worker worker(config, &handler);
  ASSERT_TRUE(worker.Initialize());

  // Use a real JPEG photo -- should classify as kPhoto.
  std::string url = "http://example.com/content-class.jpg";
  std::string jpeg = ReadTestFile("jpeg/quality100.jpg");
  ASSERT_FALSE(jpeg.empty()) << "Test JPEG not found";

  AlternateMetadata meta;
  meta.content_type = ContentType::kImage;
  meta.origin_content_type = "image/jpeg";
  CacheOriginalWithMeta(worker.cache(), url, jpeg, meta);

  std::thread worker_thread([&worker]() { worker.Run(); });
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  // Use Mobile viewport so target_width > 0 (default 480px), which
  // forces the pixel decode + resize path in TranscodeMultiResized,
  // where content analysis and content class counting occur.
  CapabilityMask webp_mask(
      CapabilityMask::ImageFormat::kWebP, CapabilityMask::Viewport::kMobile,
      CapabilityMask::PixelDensity::k1x, CapabilityMask::SaveData::kOff,
      CapabilityMask::TransferEncoding::kIdentity);
  CacheNotification notification;
  notification.url = url;
  notification.scheme = "https";
  notification.content_type = ContentType::kImage;
  notification.capability_mask = webp_mask.Encode();
  SendNotification(notification);

  // Wait for processing (proactive generates WebP + AVIF).
  for (int i = 0; i < 60 * kSanitizerBudgetScale; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    if (worker.stats().images_processed.load() >= 2) break;
  }
  EXPECT_GE(worker.stats().images_processed.load(), 1u)
      << "At least one image format should be processed";

  // Content class counting: at least one class should have incremented.
  uint64_t photo = worker.stats().content_photo.load();
  uint64_t screenshot = worker.stats().content_screenshot.load();
  uint64_t illustration = worker.stats().content_illustration.load();
  uint64_t noisy = worker.stats().content_noisy.load();
  EXPECT_GE(photo + screenshot + illustration + noisy, 1u)
      << "Content class should be counted at least once";

  // WebP variant should exist at Mobile viewport.
  std::string webp_variant = ReadVariant(worker.cache(), url, webp_mask);
  EXPECT_FALSE(webp_variant.empty()) << "WebP variant should be written";

  // AVIF variant should also exist (proactive format sibling).
  CapabilityMask avif_mask(
      CapabilityMask::ImageFormat::kAvif, CapabilityMask::Viewport::kMobile,
      CapabilityMask::PixelDensity::k1x, CapabilityMask::SaveData::kOff,
      CapabilityMask::TransferEncoding::kIdentity);
  std::string avif_variant;
  for (int i = 0; i < 60 * kSanitizerBudgetScale; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    avif_variant = ReadVariant(worker.cache(), url, avif_mask);
    if (!avif_variant.empty()) break;
  }
  EXPECT_FALSE(avif_variant.empty()) << "AVIF variant should be written";

  worker.Shutdown();
  worker_thread.join();
}

// The verify verdict must ROUTE to the declines counter (#1381):
// the stats-contract test seeds the atomic directly, so the increment path
// in WriteImageVariants was previously undriven — a mutant deleting the
// declined branch survived, and the declined score silently polluted the
// checks count and the score average. This drives a real decline
// end-to-end: same construction as the content-class test above, but with
// the verifier ON, so the tiny (100px) resized cross-format variants score
// structurally below the floor and decline.
TEST_F(WorkerTest, ImageProactiveDeclineRoutesToDeclinesCounterOnly) {
  WorkerConfig config;
  config.socket_path = socket_path_;
  config.cache_path = cache_path_;
  config.cache_size_bytes = static_cast<uint64_t>(10 * 1024 * 1024);
  config.proactive_image_variants = true;
  config.proactive_viewport_variants = false;
  config.proactive_savedata_variants = false;
  config.proactive_density_variants = false;
  config.content_analysis = false;
  config.mobile_width = 100;  // force the resize path (200px source)
  config.quality_verify = true;

  NullMessageHandler handler;
  Worker worker(config, &handler);
  ASSERT_TRUE(worker.Initialize());

  std::string url = "http://example.com/decline-counting.jpg";
  std::string jpeg = ReadTestFile("jpeg/quality100.jpg");
  ASSERT_FALSE(jpeg.empty()) << "Test JPEG not found";

  AlternateMetadata meta;
  meta.content_type = ContentType::kImage;
  meta.origin_content_type = "image/jpeg";
  CacheOriginalWithMeta(worker.cache(), url, jpeg, meta);

  std::thread worker_thread([&worker]() { worker.Run(); });
  WorkerStopper stopper(worker, worker_thread);
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  CapabilityMask webp_mask(
      CapabilityMask::ImageFormat::kWebP, CapabilityMask::Viewport::kMobile,
      CapabilityMask::PixelDensity::k1x, CapabilityMask::SaveData::kOff,
      CapabilityMask::TransferEncoding::kIdentity);
  CacheNotification notification;
  notification.url = url;
  notification.scheme = "https";
  notification.content_type = ContentType::kImage;
  notification.capability_mask = webp_mask.Encode();
  SendNotification(notification);

  // Both cross-format arms decline; wait on the counter itself, not on
  // images_processed (a declined variant is not a processed image).
  for (int i = 0; i < 120 * kSanitizerBudgetScale; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    if (worker.stats().ssimulacra2_declines.load() >= 2) break;
  }
  EXPECT_EQ(2u, worker.stats().ssimulacra2_declines.load())
      << "both cross-format variants of a 100px resize decline below the "
         "floor";

  // Routing, not just counting: a decline must NOT feed the checks count
  // or the score average. Only the same-format JPEG arm (ship-below-floor
  // policy) may register an ordinary check.
  const uint64_t checks = worker.stats().ssimulacra2_checks.load();
  EXPECT_LE(checks, 1u)
      << "declined arms leaked into ssimulacra2_checks — the declined "
         "branch in WriteImageVariants is not routing";
  const uint64_t total_x100 =
      worker.stats().ssimulacra2_total_score_x100.load();
  if (checks == 0) {
    EXPECT_EQ(0u, total_x100);
  } else {
    EXPECT_LE(total_x100, 100u * 100u)
        << "score average carries more than the single JPEG check";
  }
  EXPECT_EQ(0u, worker.stats().errors.load())
      << "a decline is a decision, not an error";

  // The mgmt STATS surface carries the decision too (hand-built JSON,
  // distinct from BuildStatsJson): read the WHOLE response.
  std::string mgmt_path = worker.mgmt_socket_path();
  intptr_t fd = ConnectToSocket(mgmt_path);
  ASSERT_GE(fd, 0) << "Failed to connect to management socket";
  std::string cmd = "STATS\n";
  ssize_t written = pagespeed::test::PipeWrite(fd, cmd.data(), cmd.size());
  ASSERT_EQ(written, static_cast<ssize_t>(cmd.size()));
  std::string response = ReadMgmtResponseToEof(fd);
  pagespeed::test::ClosePipe(fd);
  EXPECT_NE(response.find("\"declines\":2"), std::string::npos)
      << "mgmt STATS response: " << response;
}

// A declined original-format slot must not ALSO count as a no-savings skip
// (#1382): both counters describe decisions, and one refusal wearing two
// labels makes each of them over-report. The classification helper is
// tested directly because the state is not constructible end-to-end -- an
// unmeasurable-declined optimized_original needs a failed decode-back of
// our own JPEG re-encode, which the encoders do not produce.
TEST_F(WorkerTest, DeclinedOriginalIsNotANoSavingsSkip) {
  MultiTranscodeResult declined;
  declined.optimized_original = {
      false,
      {},
      {},
      "JPEG quality verification failed (no verdict: output could not be "
      "measured)"};
  declined.ssimulacra2_declined = true;
  EXPECT_FALSE(pagespeed::OriginalSlotCountsAsNoSavingsSkip(declined))
      << "a verify decline must not double-count as image_no_savings_skipped";

  // The plain case still counts: an original the optimizer could not beat.
  MultiTranscodeResult no_savings;
  no_savings.optimized_original = {
      false, {}, {}, "JPEG optimization produced no size savings"};
  EXPECT_TRUE(pagespeed::OriginalSlotCountsAsNoSavingsSkip(no_savings));

  // A shipped original never counts, whatever else the call did.
  MultiTranscodeResult shipped;
  shipped.optimized_original = {true, "bytes", "image/jpeg", {}};
  EXPECT_FALSE(pagespeed::OriginalSlotCountsAsNoSavingsSkip(shipped));

  // A CROSS-FORMAT decline does not reclassify the original slot: only the
  // original arm's decline flag belongs to this classification.
  MultiTranscodeResult webp_declined_only;
  webp_declined_only.webp_ssimulacra2_declined = true;
  webp_declined_only.optimized_original = {
      false, {}, {}, "JPEG optimization produced no size savings"};
  EXPECT_TRUE(pagespeed::OriginalSlotCountsAsNoSavingsSkip(webp_declined_only));
}

// ---------------------------------------------------------------------------
// Decline tombstone (#1382): codec contract. The blob is worker-written
// (kDeclineTombstone sentinel) and worker-read; the parse must be fail-open
// (anything unrecognized reads as "no tombstone" and the ladder re-runs)
// and MUST key on the source hash (a changed source retries its slots).
// ---------------------------------------------------------------------------

TEST(DeclineTombstoneCodec, RoundTrip) {
  std::array<std::byte, 32> hash{};
  hash[0] = static_cast<std::byte>(0xAB);
  std::vector<pagespeed::DeclineTombstoneRecord> records = {
      {0x11, 60}, {0x22, 0xFF}, {0x33, 45}};
  std::string blob = pagespeed::EncodeDeclineTombstone(hash, records);
  auto back = pagespeed::ParseDeclineTombstone(blob, hash);
  ASSERT_EQ(back.size(), records.size());
  for (size_t i = 0; i < records.size(); ++i) {
    EXPECT_EQ(back[i].slot, records[i].slot);
    EXPECT_EQ(back[i].quality, records[i].quality);
  }
}

TEST(DeclineTombstoneCodec, DifferentSourceHashReadsAsNoTombstone) {
  std::array<std::byte, 32> hash_a{};
  std::array<std::byte, 32> hash_b{};
  hash_b[31] = static_cast<std::byte>(1);
  auto blob = pagespeed::EncodeDeclineTombstone(hash_a, {{0x11, 60}});
  EXPECT_TRUE(pagespeed::ParseDeclineTombstone(blob, hash_b).empty())
      << "a tombstone for another source must not stop this source's "
         "recompute";
  // Non-vacuity: the same hash reads it back.
  EXPECT_EQ(pagespeed::ParseDeclineTombstone(blob, hash_a).size(), 1u);
}

TEST(DeclineTombstoneCodec, APayloadFromAnEarlierLayoutReadsAsNoTombstone) {
  // A refusal recorded under the previous payload layout must not survive
  // into a build whose quality search has a different reach: the variant
  // that search could not find may be reachable now.
  std::array<std::byte, 32> hash{};
  std::string legacy;
  legacy.push_back(static_cast<char>(1));  // the pre-generation layout
  legacy.append(reinterpret_cast<const char*>(hash.data()), 32);
  legacy.push_back(static_cast<char>(1));     // one record
  legacy.push_back(static_cast<char>(0x11));  // slot
  legacy.push_back(static_cast<char>(60));    // quality
  EXPECT_TRUE(pagespeed::ParseDeclineTombstone(legacy, hash).empty());
  // Non-vacuity: the same slot, written by this build, is honored.
  EXPECT_EQ(pagespeed::ParseDeclineTombstone(
                pagespeed::EncodeDeclineTombstone(hash, {{0x11, 60}}), hash)
                .size(),
            1u);
}

TEST(DeclineTombstoneCodec, ARefusalFromAnotherQualitySearchIsIgnored) {
  // The generation byte is what makes a refusal expire when the search
  // that produced it is replaced -- without it a warm cache would keep
  // serving the old verdict forever.
  std::array<std::byte, 32> hash{};
  std::string blob = pagespeed::EncodeDeclineTombstone(hash, {{0x11, 60}});
  ASSERT_EQ(blob[1], static_cast<char>(pagespeed::kVerifyQualitySearchVersion));
  blob[1] = static_cast<char>(pagespeed::kVerifyQualitySearchVersion + 1);
  EXPECT_TRUE(pagespeed::ParseDeclineTombstone(blob, hash).empty());
}

TEST(DeclineTombstoneCodec, MalformedBlobReadsAsNoTombstone) {
  std::array<std::byte, 32> hash{};
  // Truncated header.
  EXPECT_TRUE(
      pagespeed::ParseDeclineTombstone(std::string(8, '\0'), hash).empty());
  // Unknown payload version: a future writer's blob must not be
  // half-understood.
  std::string bad = pagespeed::EncodeDeclineTombstone(hash, {{0x11, 60}});
  bad[0] = static_cast<char>(99);
  EXPECT_TRUE(pagespeed::ParseDeclineTombstone(bad, hash).empty());
  // Count disagrees with the payload length (truncated record).
  std::string truncated = pagespeed::EncodeDeclineTombstone(hash, {{0x11, 60}});
  truncated.pop_back();
  EXPECT_TRUE(pagespeed::ParseDeclineTombstone(truncated, hash).empty());
}

// The decline tombstone, end-to-end (#1382): a declined variant is never
// stored, so a notification that asks for it again re-pays the full attempt
// ladder (measured in seconds per AVIF arm). The production shape of "asks
// again" is a re-notify under a DIFFERENT client mask: the notification
// dedup keys on the normalized (viewport x density x save-data) mask of the
// original request, so a second visitor class reaches the variant loops
// with every previously declined slot still missing.
//
// Fixture: a synthetic 33x34 noise AVIF stored with an EMPTY origin content
// type. Both cross-format arms of a noise image score far below the floor
// and decline (the same constructor the transcoder suite uses for its
// negative-verdict test, same pinned qualities), and -- the reason this
// input is load-bearing here -- the original-format arm of an AVIF source
// has no optimizer, so no variant is ever written for the URL and the
// original-format slot stays missing on every pass. The empty content type
// is deliberate: a stored "image/avif" type hits the pipeline's
// unsupported-type branch (only jpeg/png/gif/webp pass) and never reaches
// the variant loops, while an empty type routes by magic bytes and does.
TEST_F(WorkerTest, DeclineTombstoneStopsTheSecondNotificationsLadder) {
  WorkerConfig config;
  config.socket_path = socket_path_;
  config.cache_path = cache_path_;
  config.cache_size_bytes = static_cast<uint64_t>(10 * 1024 * 1024);
  config.proactive_image_variants = true;
  config.proactive_viewport_variants = true;  // all three viewports per pass
  config.proactive_savedata_variants = false;
  config.proactive_density_variants = false;
  config.content_analysis = false;
  config.learned_quality = false;
  // Mirror the transcoder suite's negative-verdict fixture: low fixed
  // qualities, so the noise re-encodes land far below the floor
  // deterministically instead of depending on a predictor.
  config.webp_quality = 20;
  config.avif_quality = 20;
  config.quality_verify = true;

  NullMessageHandler handler;
  Worker worker(config, &handler);
  ASSERT_TRUE(worker.Initialize());

  std::string url = "http://example.com/decline-tombstone.avif";
  std::string noise = MakeSyntheticAvif(33, 34, /*grayscale=*/false,
                                        SyntheticPattern::kNoise, 0,
                                        /*quality=*/90, /*seed=*/12345);
  ASSERT_FALSE(noise.empty()) << "synthetic AVIF authoring failed";

  AlternateMetadata meta;
  meta.content_type = ContentType::kImage;
  meta.origin_content_type = "";  // Route by magic bytes; see the comment.
  CacheOriginalWithMeta(worker.cache(), url, noise, meta);

  std::thread worker_thread([&worker]() { worker.Run(); });
  WorkerStopper stopper(worker, worker_thread);
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  auto notify = [&](CapabilityMask::Viewport vp) {
    CacheNotification notification;
    notification.url = url;
    notification.scheme = "https";
    notification.content_type = ContentType::kImage;
    CapabilityMask mask(CapabilityMask::ImageFormat::kWebP, vp,
                        CapabilityMask::PixelDensity::k1x,
                        CapabilityMask::SaveData::kOff,
                        CapabilityMask::TransferEncoding::kIdentity);
    notification.capability_mask = mask.Encode();
    SendNotification(notification);
  };

  // First notification (mobile visitor): all three viewport combinations
  // process, and both cross-format arms decline in each -- 6 declines, and
  // six tombstone records.
  notify(CapabilityMask::Viewport::kMobile);
  // Wait for the work item to retire, not for the decline counter to reach
  // six: the sixth decline is counted while the item is still running (it
  // has yet to store the tombstone and leave the in-flight set), and a
  // notification for the same URL that arrives in that window is rejected by
  // the in-flight guard -- the second ladder below would then never start,
  // however long the test waited for it.
  ASSERT_TRUE(pagespeed::test::WaitForNotificationsRetired(worker, 1,
                                                           kWorkerStallBudget));
  ASSERT_EQ(6u, worker.stats().ssimulacra2_declines.load())
      << "first notification must decline both cross-format arms at all "
         "three viewports";
  EXPECT_EQ(0u, worker.stats().ssimulacra2_decline_tombstone_hits.load())
      << "the first run has no tombstone to hit yet";

  // Second notification (desktop visitor): a different normalized mask, so
  // the dedup does not absorb it, and the same six slots are missing again
  // (declined variants are never stored). The tombstone must stop every
  // ladder. Once this work item has retired too, the counters are final.
  notify(CapabilityMask::Viewport::kDesktop);
  ASSERT_TRUE(pagespeed::test::WaitForNotificationsRetired(worker, 2,
                                                           kWorkerStallBudget));
  EXPECT_EQ(0u, worker.stats().notifications_skipped_inflight.load())
      << "the re-notify was rejected as in-flight and never reached the "
         "variant loops";
  EXPECT_EQ(6u, worker.stats().ssimulacra2_decline_tombstone_hits.load())
      << "every declined slot must hit the tombstone on the re-notify";
  EXPECT_EQ(6u, worker.stats().ssimulacra2_declines.load())
      << "the ladder re-ran for a tombstoned slot: the decline counter grew";
  EXPECT_EQ(0u, worker.stats().errors.load());

  // The mgmt STATS surface carries the decision (hand-built JSON, distinct
  // from BuildStatsJson): read the WHOLE response.
  std::string mgmt_path = worker.mgmt_socket_path();
  intptr_t fd = ConnectToSocket(mgmt_path);
  ASSERT_GE(fd, 0) << "Failed to connect to management socket";
  std::string cmd = "STATS\n";
  ssize_t written = pagespeed::test::PipeWrite(fd, cmd.data(), cmd.size());
  ASSERT_EQ(written, static_cast<ssize_t>(cmd.size()));
  std::string response = ReadMgmtResponseToEof(fd);
  pagespeed::test::ClosePipe(fd);
  EXPECT_NE(response.find("\"tombstone_hits\":6"), std::string::npos)
      << "mgmt STATS response: " << response;
}

// The tombstone consult is gated on the verifier toggle (#1382): a
// tombstone stored while verification was on must not keep suppressing
// its slot after verification is turned off -- the slot would now fill.
// Two workers share one cache: the first (verifier on) declines the
// cross-format arms and stores the tombstone; the second (verifier off)
// receives the same notification and must RE-ATTEMPT the slots -- the
// consult is skipped, no tombstone hit is counted, and variants are
// written. Deleting the gate makes the second worker read the stored
// tombstone, skip every ladder, and write nothing.
TEST_F(WorkerTest, DeclineTombstoneIgnoredWhenVerifierDisabled) {
  const std::string noise = MakeSyntheticAvif(33, 34, /*grayscale=*/false,
                                              SyntheticPattern::kNoise, 0,
                                              /*quality=*/90, /*seed=*/12345);
  ASSERT_FALSE(noise.empty()) << "synthetic AVIF authoring failed";
  AlternateMetadata meta;
  meta.content_type = ContentType::kImage;
  meta.origin_content_type = "";  // Route by magic bytes; see the e2e test.
  const std::string url = "http://example.com/decline-tombstone-off.avif";

  // Phase 1: verifier ON -- declines both cross-format arms at all three
  // viewports and stores the tombstone (same construction as the e2e
  // test above).
  {
    WorkerConfig config;
    config.socket_path = socket_path_;
    config.cache_path = cache_path_;
    config.cache_size_bytes = static_cast<uint64_t>(10 * 1024 * 1024);
    config.proactive_image_variants = true;
    config.proactive_viewport_variants = true;
    config.proactive_savedata_variants = false;
    config.proactive_density_variants = false;
    config.content_analysis = false;
    config.learned_quality = false;
    config.webp_quality = 20;
    config.avif_quality = 20;
    config.quality_verify = true;

    NullMessageHandler handler;
    Worker worker(config, &handler);
    ASSERT_TRUE(worker.Initialize());
    CacheOriginalWithMeta(worker.cache(), url, noise, meta);

    std::thread worker_thread([&worker]() { worker.Run(); });
    WorkerStopper stopper(worker, worker_thread);
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    CacheNotification notification;
    notification.url = url;
    notification.scheme = "https";
    notification.content_type = ContentType::kImage;
    CapabilityMask mask(
        CapabilityMask::ImageFormat::kWebP, CapabilityMask::Viewport::kMobile,
        CapabilityMask::PixelDensity::k1x, CapabilityMask::SaveData::kOff,
        CapabilityMask::TransferEncoding::kIdentity);
    notification.capability_mask = mask.Encode();
    SendNotification(notification);
    for (int i = 0; i < 120 * kSanitizerBudgetScale; ++i) {
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
      if (worker.stats().ssimulacra2_declines.load() >= 6) break;
    }
    ASSERT_EQ(6u, worker.stats().ssimulacra2_declines.load())
        << "phase 1 must decline both cross-format arms at all three "
           "viewports and store the tombstone";
  }

  // Phase 2: verifier OFF, same cache -- the stored tombstone must be
  // ignored (consult gated) and the slots re-attempted.
  {
    WorkerConfig config;
    config.socket_path = socket_path_ + "-off";
    config.cache_path = cache_path_;
    config.cache_size_bytes = static_cast<uint64_t>(10 * 1024 * 1024);
    config.proactive_image_variants = true;
    config.proactive_viewport_variants = true;
    config.proactive_savedata_variants = false;
    config.proactive_density_variants = false;
    config.content_analysis = false;
    config.learned_quality = false;
    config.webp_quality = 20;
    config.avif_quality = 20;
    config.quality_verify = false;

    NullMessageHandler handler;
    Worker worker(config, &handler);
    ASSERT_TRUE(worker.Initialize());

    std::thread worker_thread([&worker]() { worker.Run(); });
    WorkerStopper stopper(worker, worker_thread);
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    CacheNotification notification;
    notification.url = url;
    notification.scheme = "https";
    notification.content_type = ContentType::kImage;
    CapabilityMask mask(
        CapabilityMask::ImageFormat::kWebP, CapabilityMask::Viewport::kMobile,
        CapabilityMask::PixelDensity::k1x, CapabilityMask::SaveData::kOff,
        CapabilityMask::TransferEncoding::kIdentity);
    notification.capability_mask = mask.Encode();
    // This worker listens on its own socket; the fixture's SendNotification
    // helper hardcodes the first worker's path.
    auto notify_b = [&](const CacheNotification& n) {
      intptr_t fd = pagespeed::test::ConnectPipe(socket_path_ + "-off");
      ASSERT_GE(fd, static_cast<intptr_t>(0)) << "worker B socket";
      std::vector<char> data = n.Serialize();
      ssize_t written =
          pagespeed::test::PipeWrite(fd, data.data(), data.size());
      EXPECT_EQ(written, static_cast<ssize_t>(data.size()));
      pagespeed::test::ClosePipe(fd);
    };
    notify_b(notification);

    // With the verifier off nothing declines; the slots the first worker
    // tombstoned are re-attempted and written. Wait on the write.
    for (int i = 0; i < 120 * kSanitizerBudgetScale; ++i) {
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
      if (worker.stats().variants_written.load() >= 1) break;
    }
    EXPECT_GE(worker.stats().variants_written.load(), 1u)
        << "the tombstoned slot must be re-attempted with the verifier "
           "disabled";
    EXPECT_EQ(0u, worker.stats().ssimulacra2_decline_tombstone_hits.load())
        << "the consult must be skipped entirely with the verifier "
           "disabled";
    EXPECT_EQ(0u, worker.stats().ssimulacra2_declines.load());
  }
}

TEST_F(WorkerTest, ImageProactiveMultiFormatCreation) {
  // Proactive image variants enabled with multiple formats should
  // create WebP and AVIF variants at the requested viewport, plus
  // track variants_written and proactive_variants_written stats.
  // Covers lines 2199-2304 (proactive format loop).
  WorkerConfig config;
  config.socket_path = socket_path_;
  config.cache_path = cache_path_;
  config.cache_size_bytes = static_cast<uint64_t>(10 * 1024 * 1024);
  config.proactive_image_variants = true;
  config.proactive_viewport_variants = true;
  config.proactive_savedata_variants = false;
  config.proactive_density_variants = false;

  NullMessageHandler handler;
  Worker worker(config, &handler);
  ASSERT_TRUE(worker.Initialize());

  std::string url = "http://example.com/multi-format.jpg";
  std::string jpeg = ReadTestFile("jpeg/quality100.jpg");
  ASSERT_FALSE(jpeg.empty()) << "Test JPEG not found";

  AlternateMetadata meta;
  meta.content_type = ContentType::kImage;
  meta.origin_content_type = "image/jpeg";
  CacheOriginalWithMeta(worker.cache(), url, jpeg, meta);

  std::thread worker_thread([&worker]() { worker.Run(); });
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  // Send notification with Desktop/WebP mask.
  CapabilityMask desktop_webp(
      CapabilityMask::ImageFormat::kWebP, CapabilityMask::Viewport::kDesktop,
      CapabilityMask::PixelDensity::k1x, CapabilityMask::SaveData::kOff,
      CapabilityMask::TransferEncoding::kIdentity);
  CacheNotification notification;
  notification.url = url;
  notification.scheme = "https";
  notification.content_type = ContentType::kImage;
  notification.capability_mask = desktop_webp.Encode();
  SendNotification(notification);

  // Wait for processing (proactive generates at all viewports x formats).
  // Desktop/WebP is requested directly; proactive generates AVIF at
  // Desktop, plus both formats at Mobile and Tablet.
  // Use selector fallback: content-deduped variants still servable.
  auto check_variant = [&](CapabilityMask::Viewport vp,
                           CapabilityMask::ImageFormat fmt) {
    CapabilityMask m(fmt, vp, CapabilityMask::PixelDensity::k1x,
                     CapabilityMask::SaveData::kOff,
                     CapabilityMask::TransferEncoding::kIdentity);
    return !ReadBestVariant(worker.cache(), url, m).empty();
  };

  // Poll for Desktop variants (directly requested + proactive AVIF).
  // AVIF encoding is slow; under CPU contention allow up to 20s
  // (sanitizer-scaled: TSan multiplies the transcode cost).
  bool has_desktop_webp = false;
  bool has_desktop_avif = false;
  for (int i = 0; i < 200 * kSanitizerBudgetScale; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    if (!has_desktop_webp)
      has_desktop_webp = check_variant(CapabilityMask::Viewport::kDesktop,
                                       CapabilityMask::ImageFormat::kWebP);
    if (!has_desktop_avif)
      has_desktop_avif = check_variant(CapabilityMask::Viewport::kDesktop,
                                       CapabilityMask::ImageFormat::kAvif);
    if (has_desktop_webp && has_desktop_avif) break;
  }

  EXPECT_TRUE(has_desktop_webp) << "Desktop/WebP should be servable";
  EXPECT_TRUE(has_desktop_avif)
      << "Desktop/AVIF should be servable (proactive)";

  // Mobile and Tablet should also be servable due to proactive_viewport_variants
  // (either written directly or via selector fallback when content-identical).
  bool mobile_webp = false;
  bool mobile_avif = false;
  for (int i = 0; i < 200 * kSanitizerBudgetScale; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    if (!mobile_webp)
      mobile_webp = check_variant(CapabilityMask::Viewport::kMobile,
                                  CapabilityMask::ImageFormat::kWebP);
    if (!mobile_avif)
      mobile_avif = check_variant(CapabilityMask::Viewport::kMobile,
                                  CapabilityMask::ImageFormat::kAvif);
    if (mobile_webp && mobile_avif) break;
  }
  EXPECT_TRUE(mobile_webp)
      << "Mobile/WebP should be servable (proactive viewport)";
  EXPECT_TRUE(mobile_avif)
      << "Mobile/AVIF should be servable (proactive viewport)";

  // Wait for the proactive matrix to fully complete (ReadBestVariant may
  // find variants via selector fallback before all writes finish).  The
  // old "counter stable for 3s" heuristic was a slowdown trap: under TSan
  // a single AVIF transcode leaves the counter flat well past 3s
  // mid-matrix, so the wait declared completion at 3 of the expected >=4
  // variants.  Retiring the work item is the true completion signal.
  ASSERT_NO_FATAL_FAILURE(WaitForNotificationRetiredWithVariant(worker));

  // Stats should show variants written + dedup skips cover the expected matrix.
  uint64_t written = worker.stats().variants_written.load();
  uint64_t dedup_skipped = worker.stats().dedup_writes_skipped.load();
  EXPECT_GE(written + dedup_skipped, 4u)
      << "At least 4 variants should be generated (2 formats x 2+ viewports)";
  EXPECT_GE(worker.stats().proactive_variants_written.load() + dedup_skipped,
            1u)
      << "Proactive variants counter should increment";

  worker.Shutdown();
  worker_thread.join();
}

TEST_F(WorkerTest, ImageProactivePerFormatSsimulacra2InMetadata) {
  // Verify that per-format SSIMULACRA2 scores are written to cache
  // metadata (not just the JPEG score for all formats).
  // Uses Mobile viewport so the resize path runs (Desktop target_width=0
  // takes a fast path that skips SSIMULACRA2 verification).
  WorkerConfig config;
  config.socket_path = socket_path_;
  config.cache_path = cache_path_;
  config.cache_size_bytes = static_cast<uint64_t>(10 * 1024 * 1024);
  config.proactive_image_variants = true;
  config.proactive_viewport_variants = false;
  config.proactive_savedata_variants = false;
  config.proactive_density_variants = false;
  config.avif_speed = 10;  // Fastest — MSVC-compiled libaom is slower

  NullMessageHandler handler;
  Worker worker(config, &handler);
  ASSERT_TRUE(worker.Initialize());

  std::string url = "http://example.com/ssim-meta-test.jpg";
  // q80_512x512.jpg is 512x512, wider than mobile_width=480, so resize occurs.
  // q80 source quality ensures the resized WebP/AVIF output beats the size
  // gate (sjpeg6.jpg was too well-compressed, causing WebP to be larger).
  std::string jpeg = ReadTestFile("jpeg/q80_512x512.jpg");
  ASSERT_FALSE(jpeg.empty()) << "Test JPEG not found";

  AlternateMetadata meta;
  meta.content_type = ContentType::kImage;
  meta.origin_content_type = "image/jpeg";
  CacheOriginalWithMeta(worker.cache(), url, jpeg, meta);

  std::thread worker_thread([&worker]() { worker.Run(); });
  WorkerStopper stopper(worker, worker_thread);
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  // Send notification with Mobile/WebP mask to trigger the resize path
  // (q80_512x512.jpg is wider than mobile_width=480).
  CapabilityMask mobile_webp(
      CapabilityMask::ImageFormat::kWebP, CapabilityMask::Viewport::kMobile,
      CapabilityMask::PixelDensity::k1x, CapabilityMask::SaveData::kOff,
      CapabilityMask::TransferEncoding::kIdentity);
  CacheNotification notification;
  notification.url = url;
  notification.scheme = "https";
  notification.content_type = ContentType::kImage;
  notification.capability_mask = mobile_webp.Encode();
  SendNotification(notification);

  // WebP and Original variants at Mobile viewport.
  CapabilityMask mobile_original(
      CapabilityMask::ImageFormat::kOriginal, CapabilityMask::Viewport::kMobile,
      CapabilityMask::PixelDensity::k1x, CapabilityMask::SaveData::kOff,
      CapabilityMask::TransferEncoding::kIdentity);

  // Wait for the notification's whole transcode to retire (the verify loop
  // runs its full attempt budget and every variant is written when the
  // multi-format transcode returns), then read both variants.  A fixed
  // poll budget expired on a loaded Windows runner (issue #1756) with the
  // pipeline still making progress; the stall-based wait only gives up when
  // the worker stops moving.
  ASSERT_TRUE(pagespeed::test::WaitForNotificationsRetired(worker, 1,
                                                           kWorkerStallBudget));
  std::optional<VariantWithMeta> webp_meta =
      ReadVariantWithMeta(worker.cache(), url, mobile_webp);
  std::optional<VariantWithMeta> orig_meta =
      ReadVariantWithMeta(worker.cache(), url, mobile_original);

  // At least the Original variant should always be written (resized JPEG).
  // WebP may not be written if its output exceeds the resized JPEG size gate.
  ASSERT_TRUE(orig_meta.has_value())
      << "Mobile/Original variant should be written";

  // Original variant SSIMULACRA2: valid when pixel-based resize path runs,
  // N/A (0xFFFF) on stream-based fast path fallback.
  if (orig_meta->metadata.ssimulacra2_score_x100 != 0xFFFF) {
    EXPECT_LE(orig_meta->metadata.ssimulacra2_score_x100, 10000u);
  }

  // WebP variant may not exist (size gate rejects when output >= resized JPEG).
  // When it does exist, verify its SSIMULACRA2 score is valid.
  if (webp_meta.has_value() &&
      webp_meta->metadata.ssimulacra2_score_x100 != 0xFFFF) {
    EXPECT_LE(webp_meta->metadata.ssimulacra2_score_x100, 10000u);
  }
}

// ---------------------------------------------------------------------------
// ToSvgImageType coverage: GIF source format through SVG pipeline.
// Exercises the IMAGE_GIF -> kGif branch of ToSvgImageType().
// ---------------------------------------------------------------------------

TEST_F(WorkerTest, ImageSvgCandidacyEvaluatesGifFormat) {
  // GIF images should be evaluated for SVG candidacy with src_format=kGif.
  // This exercises the ToSvgImageType() IMAGE_GIF -> kGif mapping.
  WorkerConfig config;
  config.socket_path = socket_path_;
  config.cache_path = cache_path_;
  config.cache_size_bytes = static_cast<uint64_t>(10 * 1024 * 1024);
  config.svg_mode = SvgMode::kDetect;
  config.proactive_image_variants = false;
  config.proactive_viewport_variants = false;
  config.proactive_savedata_variants = false;
  config.proactive_density_variants = false;

  NullMessageHandler handler;
  Worker worker(config, &handler);
  ASSERT_TRUE(worker.Initialize());

  // Use a small GIF test file.
  std::string url = "http://example.com/icon.gif";
  std::string gif = ReadTestFile("gif/o.gif");
  ASSERT_FALSE(gif.empty()) << "Test GIF not found";

  AlternateMetadata meta;
  meta.content_type = ContentType::kImage;
  meta.origin_content_type = "image/gif";
  CacheOriginalWithMeta(worker.cache(), url, gif, meta);

  std::thread worker_thread([&worker]() { worker.Run(); });
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  CacheNotification notification;
  notification.url = url;
  notification.scheme = "https";
  notification.content_type = ContentType::kImage;
  notification.capability_mask = CapabilityMask().Encode();
  SendNotification(notification);

  // Wait for processing.
  for (int i = 0; i < 60 * kSanitizerBudgetScale; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    if (worker.stats().images_processed.load() >= 1) break;
  }
  EXPECT_GE(worker.stats().images_processed.load(), 1u);

  // SVG candidacy should have been evaluated (even if rejected for GIF).
  EXPECT_GE(worker.stats().svg_candidates_evaluated.load(), 1u)
      << "SVG candidacy should be evaluated for GIF images";

  worker.Shutdown();
  worker_thread.join();
}

// ---------------------------------------------------------------------------
// ToSvgImageType coverage: WebP source format through SVG pipeline.
// Exercises the IMAGE_WEBP -> kWebP branch of ToSvgImageType().
// ---------------------------------------------------------------------------

TEST_F(WorkerTest, ImageSvgCandidacyEvaluatesWebPFormat) {
  // WebP images should be evaluated for SVG candidacy with src_format=kWebP.
  // This exercises the ToSvgImageType() IMAGE_WEBP -> kWebP mapping.
  WorkerConfig config;
  config.socket_path = socket_path_;
  config.cache_path = cache_path_;
  config.cache_size_bytes = static_cast<uint64_t>(10 * 1024 * 1024);
  config.svg_mode = SvgMode::kDetect;
  config.proactive_image_variants = false;
  config.proactive_viewport_variants = false;
  config.proactive_savedata_variants = false;
  config.proactive_density_variants = false;

  NullMessageHandler handler;
  Worker worker(config, &handler);
  ASSERT_TRUE(worker.Initialize());

  // Use a WebP test file.
  std::string url = "http://example.com/icon.webp";
  std::string webp = ReadTestFile("opaque_32x20.webp");
  ASSERT_FALSE(webp.empty()) << "Test WebP not found";

  AlternateMetadata meta;
  meta.content_type = ContentType::kImage;
  meta.origin_content_type = "image/webp";
  CacheOriginalWithMeta(worker.cache(), url, webp, meta);

  std::thread worker_thread([&worker]() { worker.Run(); });
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  CacheNotification notification;
  notification.url = url;
  notification.scheme = "https";
  notification.content_type = ContentType::kImage;
  notification.capability_mask = CapabilityMask().Encode();
  SendNotification(notification);

  // Wait for processing.
  for (int i = 0; i < 60 * kSanitizerBudgetScale; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    if (worker.stats().images_processed.load() >= 1) break;
  }
  EXPECT_GE(worker.stats().images_processed.load(), 1u);

  // SVG candidacy should have been evaluated.
  EXPECT_GE(worker.stats().svg_candidates_evaluated.load(), 1u)
      << "SVG candidacy should be evaluated for WebP images";

  worker.Shutdown();
  worker_thread.join();
}

// ---------------------------------------------------------------------------
// ToSvgImageType coverage: JPEG source format through SVG pipeline.
// Exercises the IMAGE_JPEG -> kJpeg branch of ToSvgImageType().
// ---------------------------------------------------------------------------

TEST_F(WorkerTest, ImageSvgCandidacyEvaluatesJpegFormat) {
  // JPEG images should be evaluated for SVG candidacy with src_format=kJpeg.
  // This exercises the ToSvgImageType() IMAGE_JPEG -> kJpeg mapping.
  WorkerConfig config;
  config.socket_path = socket_path_;
  config.cache_path = cache_path_;
  config.cache_size_bytes = static_cast<uint64_t>(10 * 1024 * 1024);
  config.svg_mode = SvgMode::kDetect;
  config.proactive_image_variants = false;
  config.proactive_viewport_variants = false;
  config.proactive_savedata_variants = false;
  config.proactive_density_variants = false;

  NullMessageHandler handler;
  Worker worker(config, &handler);
  ASSERT_TRUE(worker.Initialize());

  // Use a JPEG test file.
  std::string url = "http://example.com/photo-svg-eval.jpg";
  std::string jpeg = ReadTestFile("jpeg/quality100.jpg");
  ASSERT_FALSE(jpeg.empty()) << "Test JPEG not found";

  AlternateMetadata meta;
  meta.content_type = ContentType::kImage;
  meta.origin_content_type = "image/jpeg";
  CacheOriginalWithMeta(worker.cache(), url, jpeg, meta);

  std::thread worker_thread([&worker]() { worker.Run(); });
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  CacheNotification notification;
  notification.url = url;
  notification.scheme = "https";
  notification.content_type = ContentType::kImage;
  notification.capability_mask = CapabilityMask().Encode();
  SendNotification(notification);

  // Wait for processing.
  for (int i = 0; i < 60 * kSanitizerBudgetScale; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    if (worker.stats().images_processed.load() >= 1) break;
  }
  EXPECT_GE(worker.stats().images_processed.load(), 1u);

  // SVG candidacy should have been evaluated for the JPEG.
  EXPECT_GE(worker.stats().svg_candidates_evaluated.load(), 1u)
      << "SVG candidacy should be evaluated for JPEG images";

  worker.Shutdown();
  worker_thread.join();
}

// ---------------------------------------------------------------------------
// Content identity for images the worker leaves as-is.  An origin that sends
// no validators gets its ETag from the stamp on the identity entry, so every
// path that writes no optimized original-format variant must stamp it.
// ---------------------------------------------------------------------------

namespace {

// Reads the identity entry (desktop, 1x, identity encoding) of `url`.
std::optional<ReadResult> ReadIdentityEntry(PageSpeedCache* cache,
                                            const std::string& url) {
  const AlternateId id =
      MaskToAlternateId(static_cast<uint8_t>(CapabilityMask().Encode() & 0xFF));
  auto rr = cache->ReadAlternate(WorkerTest::StripSchemeAuthority(url), "",
                                 "https", id);
  if (!rr.has_value()) return std::nullopt;
  return std::move(*rr);
}

}  // namespace

TEST_F(WorkerTest, KeptAsIsImageIdentityEntryGetsContentIdentity) {
  // An already-optimal JPEG: the original-format re-encode is not smaller,
  // so no optimized original is written and the identity entry keeps the
  // front end's own bytes.  It must carry the hash of those bytes, which is
  // what gives an origin without validators an ETag and a 304.
  WorkerConfig config;
  config.socket_path = socket_path_;
  config.cache_path = cache_path_;
  config.cache_size_bytes = static_cast<uint64_t>(10 * 1024 * 1024);
  config.proactive_image_variants = false;
  config.proactive_viewport_variants = false;
  config.proactive_savedata_variants = false;
  config.proactive_density_variants = false;

  NullMessageHandler handler;
  Worker worker(config, &handler);
  ASSERT_TRUE(worker.Initialize());

  const std::string url = "http://example.com/already-optimal.jpg";
  const std::string jpeg = ReadTestFile("jpeg/already_optimized.jpg");
  ASSERT_FALSE(jpeg.empty()) << "Test JPEG not found";
  AlternateMetadata meta;
  meta.content_type = ContentType::kOther;
  meta.origin_content_type = "image/jpeg";
  CacheOriginalWithMeta(worker.cache(), url, jpeg, meta);

  std::thread worker_thread([&worker]() { worker.Run(); });
  std::this_thread::sleep_for(std::chrono::milliseconds(50));
  CacheNotification n;
  n.url = url;
  n.scheme = "https";
  n.content_type = ContentType::kImage;
  n.capability_mask = CapabilityMask().Encode();
  SendNotification(n);
  ASSERT_TRUE(pagespeed::test::WaitForNotificationsRetired(worker, 1,
                                                           kWorkerStallBudget));

  // Whether the encoder keeps this fixture as-is depends on the platform's
  // codec build; the identity must carry the origin's hash either way (a
  // re-encoded entry inherits the stamp).
  const bool kept_as_is = worker.stats().image_no_savings_skipped.load() >= 1u;
  auto raw = ReadIdentityEntry(worker.cache(), url);
  ASSERT_TRUE(raw.has_value());
  EXPECT_EQ(raw->metadata.origin_html_hash, Sha256Of(jpeg));
  EXPECT_TRUE(pagespeed::ContentIdentity64(raw->metadata).has_value())
      << "no content identity: the front end would emit no ETag";
  if (kept_as_is) {
    // The path under test: the identity entry stays the origin's own bytes.
    EXPECT_EQ(raw->metadata.flags & AlternateMetadata::kFlagWorkerProcessed, 0)
        << "the identity entry must stay the origin's own bytes";
    auto body = raw->content();
    EXPECT_EQ(
        std::string(reinterpret_cast<const char*>(body.data()), body.size()),
        jpeg);
  }

  worker.Shutdown();
  worker_thread.join();
}

TEST_F(WorkerTest, KeptAsIsPngIdentityEntryGetsContentIdentity) {
  // Same rule for PNG: a small palette image the optimizer cannot shrink.
  WorkerConfig config;
  config.socket_path = socket_path_;
  config.cache_path = cache_path_;
  config.cache_size_bytes = static_cast<uint64_t>(10 * 1024 * 1024);
  config.proactive_image_variants = false;
  config.proactive_viewport_variants = false;
  config.proactive_savedata_variants = false;
  config.proactive_density_variants = false;

  NullMessageHandler handler;
  Worker worker(config, &handler);
  ASSERT_TRUE(worker.Initialize());

  const std::string url = "http://example.com/already-optimal.png";
  const std::string png = ReadTestFile("png/pagespeed-33x34.png");
  ASSERT_FALSE(png.empty()) << "Test PNG not found";
  AlternateMetadata meta;
  meta.content_type = ContentType::kOther;
  meta.origin_content_type = "image/png";
  CacheOriginalWithMeta(worker.cache(), url, png, meta);

  std::thread worker_thread([&worker]() { worker.Run(); });
  std::this_thread::sleep_for(std::chrono::milliseconds(50));
  CacheNotification n;
  n.url = url;
  n.scheme = "https";
  n.content_type = ContentType::kImage;
  n.capability_mask = CapabilityMask().Encode();
  SendNotification(n);
  ASSERT_TRUE(pagespeed::test::WaitForNotificationsRetired(worker, 1,
                                                           kWorkerStallBudget));

  auto entry = ReadIdentityEntry(worker.cache(), url);
  ASSERT_TRUE(entry.has_value());
  ASSERT_GE(worker.stats().image_no_savings_skipped.load(), 1u)
      << "the fixture was optimized after all; pick an already-optimal image";
  EXPECT_EQ(entry->metadata.flags & AlternateMetadata::kFlagWorkerProcessed, 0)
      << "the identity entry must stay the origin's own bytes";
  EXPECT_EQ(entry->metadata.origin_html_hash, Sha256Of(png));
  EXPECT_TRUE(pagespeed::ContentIdentity64(entry->metadata).has_value());

  worker.Shutdown();
  worker_thread.join();
}

TEST_F(WorkerTest, PassthroughImageWithCompressedCopiesKeepsIdentity) {
  // An unsupported image type whose gzip and brotli copies already exist (for
  // example written before the identity stamp existed): the early return for
  // stored copies must not skip the identity stamp.
  WorkerConfig config;
  config.socket_path = socket_path_;
  config.cache_path = cache_path_;
  config.cache_size_bytes = static_cast<uint64_t>(10 * 1024 * 1024);

  NullMessageHandler handler;
  Worker worker(config, &handler);
  ASSERT_TRUE(worker.Initialize());

  const std::string url = "http://example.com/favicon.ico";
  const std::string ico = "fake ico content for testing purposes";
  AlternateMetadata meta;
  meta.content_type = ContentType::kOther;
  meta.origin_content_type = "image/x-icon";
  CacheOriginalWithMeta(worker.cache(), url, ico, meta);

  // Pre-existing compressed copies, written without a stamp.
  for (auto enc : {CapabilityMask::TransferEncoding::kGzip,
                   CapabilityMask::TransferEncoding::kBrotli}) {
    CapabilityMask mask;
    mask.set_transfer_encoding(enc);
    AlternateMetadata copy_meta;
    copy_meta.full_mask = mask.Encode();
    copy_meta.content_type = ContentType::kImage;
    copy_meta.origin_content_type = "image/x-icon";
    copy_meta.flags = AlternateMetadata::kFlagWorkerProcessed;
    const AlternateId cid =
        MaskToAlternateId(static_cast<uint8_t>(mask.Encode() & 0xFF));
    auto wh = worker.cache()->WriteAlternate(
        StripSchemeAuthority(url), "", "https", cid, ico.size(), copy_meta);
    ASSERT_TRUE(wh.has_value());
    ASSERT_TRUE(wh->write_sync(std::as_bytes(std::span(ico.data(), ico.size())))
                    .has_value());
    ASSERT_TRUE(wh->close_sync().has_value());
  }

  std::thread worker_thread([&worker]() { worker.Run(); });
  std::this_thread::sleep_for(std::chrono::milliseconds(50));
  CacheNotification n;
  n.url = url;
  n.scheme = "https";
  n.content_type = ContentType::kImage;
  n.capability_mask = CapabilityMask().Encode();
  SendNotification(n);
  ASSERT_TRUE(pagespeed::test::WaitForNotificationsRetired(worker, 1,
                                                           kWorkerStallBudget));

  auto raw = ReadIdentityEntry(worker.cache(), url);
  ASSERT_TRUE(raw.has_value());
  EXPECT_EQ(raw->metadata.flags & AlternateMetadata::kFlagWorkerProcessed, 0);
  EXPECT_EQ(raw->metadata.origin_html_hash, Sha256Of(ico));
  EXPECT_TRUE(pagespeed::ContentIdentity64(raw->metadata).has_value());
  EXPECT_EQ(worker.stats().alternate_write_failures.load(), 0u);

  worker.Shutdown();
  worker_thread.join();
}

}  // namespace
}  // namespace pagespeed
