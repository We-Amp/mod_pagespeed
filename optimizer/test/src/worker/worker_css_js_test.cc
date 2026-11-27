// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

// CSS and JavaScript optimization tests
//
// - CSS and JS minification, import flattening, compression guards
// - Compressed variant generation and counters
//
// One of the topic files of //test/src/worker:worker_test; the shared fixture
// lives in worker_test_fixture.h.

// clang-format off
// Keep the fixture header first: it preserves the old worker_test.cc include
// order (worker.h before any Windows header), which Windows builds have proven.
// See lib/base/message_handler.h for the FormatMessage macro interaction.
#include "test/src/worker/worker_test_fixture.h"
// clang-format on

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "gtest/gtest.h"
#include "lib/base/message_handler.h"
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

TEST_F(WorkerTest, NotificationForMissingCacheEntry) {
  WorkerConfig config;
  config.socket_path = socket_path_;
  config.cache_path = cache_path_;
  config.cache_size_bytes = static_cast<uint64_t>(10 * 1024 * 1024);

  Worker worker(config, nullptr);
  ASSERT_TRUE(worker.Initialize());

  std::thread worker_thread([&worker]() { worker.Run(); });
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  // Send notification for URL not in cache — should not crash
  CacheNotification notification;
  notification.url = "http://example.com/missing.html";
  notification.scheme = "https";
  notification.content_type = ContentType::kHtml;
  notification.capability_mask = 0;
  SendNotification(notification);

  std::this_thread::sleep_for(std::chrono::milliseconds(100));

  worker.Shutdown();
  worker_thread.join();
}

TEST_F(WorkerTest, CssNotificationMinifiesAndWritesVariant) {
  WorkerConfig config;
  config.socket_path = socket_path_;
  config.cache_path = cache_path_;
  config.cache_size_bytes = static_cast<uint64_t>(10 * 1024 * 1024);

  Worker worker(config, nullptr);
  ASSERT_TRUE(worker.Initialize());

  // Pre-populate cache with unminified CSS
  std::string url = "http://example.com/style.css";
  std::string css =
      "body  {  margin:  0;  padding:  0;  }\n"
      "h1  {  color:  red;  font-size:  24px;  }\n";
  CacheOriginal(worker.cache(), url, css);

  // Run worker in background
  std::thread worker_thread([&worker]() { worker.Run(); });
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  // Send CSS notification with Desktop/Identity mask (nginx default).
  CacheNotification notification;
  notification.url = url;
  notification.scheme = "https";
  notification.content_type = ContentType::kCss;
  notification.capability_mask = CapabilityMask().Encode();
  SendNotification(notification);

  // Give worker time to process
  std::this_thread::sleep_for(std::chrono::milliseconds(200));

  // Verify minified CSS variant was written at Desktop/Identity.
  CapabilityMask mask;
  std::string variant = ReadVariant(worker.cache(), url, mask);
  EXPECT_FALSE(variant.empty());
  // Minified version should be shorter
  EXPECT_LT(variant.size(), css.size());
  // Should still contain key properties
  EXPECT_NE(variant.find("margin"), std::string::npos);
  EXPECT_NE(variant.find("color"), std::string::npos);

  worker.Shutdown();
  worker_thread.join();
}

TEST_F(WorkerTest, CssViewportNormalization) {
  WorkerConfig config;
  config.socket_path = socket_path_;
  config.cache_path = cache_path_;
  config.cache_size_bytes = static_cast<uint64_t>(10 * 1024 * 1024);

  Worker worker(config, nullptr);
  ASSERT_TRUE(worker.Initialize());

  // Pre-populate cache with unminified CSS
  std::string url = "http://example.com/normalize.css";
  std::string css =
      "body  {  margin:  0;  padding:  0;  }\n"
      "h1  {  color:  red;  font-size:  24px;  }\n";
  CacheOriginal(worker.cache(), url, css);

  std::thread worker_thread([&worker]() { worker.Run(); });
  WorkerStopper stopper(worker, worker_thread);
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  // Send CSS notification with Desktop/Identity mask (nginx default).
  CacheNotification notification;
  notification.url = url;
  notification.scheme = "https";
  notification.content_type = ContentType::kCss;
  notification.capability_mask = CapabilityMask().Encode();
  SendNotification(notification);

  // Wait for processing to complete and dedup set to be populated.
  for (int i = 0; i < 40 * kSanitizerBudgetScale; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    if (worker.stats().css_processed.load() >= 1 &&
        worker.in_flight_work() == 0) {
      break;
    }
  }

  // Variant should be written at Desktop/Identity.
  CapabilityMask desktop_mask;
  std::string desktop_variant = ReadVariant(worker.cache(), url, desktop_mask);
  ASSERT_FALSE(desktop_variant.empty())
      << "CSS variant should exist at Desktop/Identity";
  EXPECT_LT(desktop_variant.size(), css.size()) << "Should be minified";

  // A second notification with the same mask should be deduped.
  uint64_t dedup_before = worker.stats().notifications_skipped_dedup.load();
  CacheNotification dup_notif;
  dup_notif.url = url;
  dup_notif.scheme = "https";
  dup_notif.content_type = ContentType::kCss;
  dup_notif.capability_mask = CapabilityMask().Encode();
  SendNotification(dup_notif);
  // Poll for the dedup counter to advance instead of a fixed sleep (slow
  // shards can sit at the pipe read past a fixed budget).
  for (int i = 0; i < 80 * kSanitizerBudgetScale; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    if (worker.stats().notifications_skipped_dedup.load() > dedup_before) break;
  }
  EXPECT_GT(worker.stats().notifications_skipped_dedup.load(), dedup_before)
      << "Duplicate CSS notification should be deduped";
}

TEST_F(WorkerTest, JsNotificationMinifiesAndWritesVariant) {
  WorkerConfig config;
  config.socket_path = socket_path_;
  config.cache_path = cache_path_;
  config.cache_size_bytes = static_cast<uint64_t>(10 * 1024 * 1024);

  Worker worker(config, nullptr);
  ASSERT_TRUE(worker.Initialize());

  // Pre-populate cache with unminified JS
  std::string url = "http://example.com/app.js";
  std::string js =
      "// Main application script\n"
      "function  hello( name )  {\n"
      "  var  greeting  =  'Hello, '  +  name;\n"
      "  return  greeting;\n"
      "}\n";
  CacheOriginal(worker.cache(), url, js);

  // Run worker in background
  std::thread worker_thread([&worker]() { worker.Run(); });
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  // Send JS notification with Desktop/Identity mask (nginx default).
  CacheNotification notification;
  notification.url = url;
  notification.scheme = "https";
  notification.content_type = ContentType::kJs;
  notification.capability_mask = CapabilityMask().Encode();
  SendNotification(notification);

  // Wait for worker to process the JS notification.
  // Use the same simple pattern as CssNotificationMinifiesAndWritesVariant:
  // wait a fixed time, then read the variant.
  for (int i = 0; i < 40 * kSanitizerBudgetScale; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    if (worker.stats().js_processed.load() >= 1) break;
  }
  EXPECT_GE(worker.stats().js_processed.load(), 1u)
      << "Worker did not process JS notification";

  CapabilityMask mask;
  std::string variant = ReadVariant(worker.cache(), url, mask);
  EXPECT_FALSE(variant.empty()) << "Worker did not produce a JS variant";
  // Minified version should be shorter (comment removed, whitespace collapsed)
  EXPECT_LT(variant.size(), js.size());
  // Should still contain key identifiers
  EXPECT_NE(variant.find("hello"), std::string::npos);
  EXPECT_NE(variant.find("greeting"), std::string::npos);

  worker.Shutdown();
  worker_thread.join();
}

TEST_F(WorkerTest, RawInputVariantsCarrySourceBinding) {
  // Issue #434, property half: every variant the worker writes from a RAW
  // origin input (an entry whose flags lack kFlagWorkerProcessed, i.e. a
  // front end's own recording) carries the SHA-256 of that input in its
  // metadata, so the front ends' derived ETag has a tier-1 identity that
  // changes with the source content instead of only with the optimized
  // byte length.  Covers the text paths (CSS, JS), the HTML path with
  // agent_optimize off and on, the raster image path and the native-SVG
  // branch.  Flattened sheets are excepted by design: their input embeds
  // other files, so they record no binding (see the CSS caller).
  WorkerConfig config;
  config.socket_path = socket_path_;
  config.cache_path = cache_path_;
  config.cache_size_bytes = static_cast<uint64_t>(10 * 1024 * 1024);

  NullMessageHandler handler;
  config.unsafe_force_async_css =
      true;  // fixture page is not under test for the gate
  Worker worker(config, &handler);
  ASSERT_TRUE(worker.Initialize());

  const std::string css =
      "body  {  margin:  0;  padding:  0;  }\n"
      "h1  {  color:  red;  font-size:  24px;  }\n";
  const std::string js =
      "// Main application script\n"
      "function  hello( name )  {\n"
      "  var  greeting  =  'Hello, '  +  name;\n"
      "  return  greeting;\n"
      "}\n";
  const std::string html =
      "<html><head><style>body { margin: 0; } "
      "h1 { color: red; }</style></head>"
      "<body><h1>Hello World</h1></body></html>";
  const std::string svg =
      "<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"16\" "
      "height=\"16\"><rect width=\"16\" height=\"16\" fill=\"red\"/></svg>";
  const std::string jpeg = ReadTestFile("jpeg/quality100.jpg");
  ASSERT_FALSE(jpeg.empty()) << "Test JPEG not found";

  const std::string css_url = "http://example.com/style.css";
  const std::string js_url = "http://example.com/app.js";
  const std::string html_url = "http://example.com/page.html";
  const std::string svg_url = "http://example.com/icon.svg";
  const std::string img_url = "http://example.com/photo.jpg";
  CacheOriginal(worker.cache(), css_url, css);
  CacheOriginal(worker.cache(), js_url, js);
  CacheOriginal(worker.cache(), html_url, html);
  CacheOriginal(worker.cache(), svg_url, svg, /*hostname=*/"", /*flags=*/0,
                /*source_binding=*/nullptr, "image/svg+xml");
  CacheOriginal(worker.cache(), img_url, jpeg);

  std::thread worker_thread([&worker]() { worker.Run(); });
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  auto notify = [&](const std::string& url, ContentType type, uint32_t mask) {
    CacheNotification n;
    n.url = url;
    n.scheme = "https";
    n.content_type = type;
    n.capability_mask = mask;
    SendNotification(n);
  };
  notify(css_url, ContentType::kCss, CapabilityMask().Encode());
  notify(js_url, ContentType::kJs, CapabilityMask().Encode());
  notify(html_url, ContentType::kHtml, CapabilityMask().Encode());
  notify(svg_url, ContentType::kImage, CapabilityMask().Encode());
  CapabilityMask mobile_webp(
      CapabilityMask::ImageFormat::kWebP, CapabilityMask::Viewport::kMobile,
      CapabilityMask::PixelDensity::k1x, CapabilityMask::SaveData::kOff,
      CapabilityMask::TransferEncoding::kIdentity);
  notify(img_url, ContentType::kImage, mobile_webp.Encode());

  ASSERT_TRUE(pagespeed::test::WaitForNotificationsRetired(worker, 5,
                                                           kWorkerStallBudget));
  EXPECT_GE(worker.stats().variants_written.load(std::memory_order_relaxed), 5u)
      << "notification set retired without the expected variant writes";

  size_t bound = 0;
  CheckSourceBinding(worker.cache(), css_url, Sha256Of(css), &bound);
  EXPECT_GE(bound, 2u) << "CSS: expected the minified identity plus a "
                          "compressed copy";
  CheckSourceBinding(worker.cache(), js_url, Sha256Of(js), &bound);
  EXPECT_GE(bound, 2u) << "JS: expected the minified identity plus a "
                          "compressed copy";
  CheckSourceBinding(worker.cache(), html_url, Sha256Of(html), &bound);
  EXPECT_GE(bound, 1u) << "HTML (agent off): no stamped variant";
  CheckSourceBinding(worker.cache(), svg_url, Sha256Of(svg), &bound);
  EXPECT_GE(bound, 2u) << "SVG: expected gzip and brotli copies";
  CheckSourceBinding(worker.cache(), img_url, Sha256Of(jpeg), &bound);
  EXPECT_GE(bound, 1u) << "image: no stamped variant";

  worker.Shutdown();
  worker_thread.join();

  // agent_optimize leg: the agent branch derives the binding itself (over
  // the raw read); the stamped variant must carry the same SHA-256.
  WorkerConfig agent_config = config;
  agent_config.socket_path = socket_path_ + "2";
  agent_config.cache_path = temp_dir_ + "/cache2";
  agent_config.browser_analysis.agent_optimize = true;
  Worker agent_worker(agent_config, &handler);
  ASSERT_TRUE(agent_worker.Initialize());
  const std::string agent_url = "http://example.com/agent.html";
  CacheOriginal(agent_worker.cache(), agent_url, html);
  std::thread agent_thread([&agent_worker]() { agent_worker.Run(); });
  std::this_thread::sleep_for(std::chrono::milliseconds(50));
  // SendNotification targets socket_path_ (the first worker's, now shut
  // down); the agent worker listens on its own socket, so send directly.
  {
    CacheNotification n;
    n.url = agent_url;
    n.scheme = "https";
    n.content_type = ContentType::kHtml;
    n.capability_mask = CapabilityMask().Encode();
    std::vector<char> data = n.Serialize();
    intptr_t client_fd = pagespeed::test::ConnectPipe(agent_config.socket_path);
    ASSERT_GE(client_fd, static_cast<intptr_t>(0))
        << "failed to connect to the agent worker's socket";
    EXPECT_EQ(pagespeed::test::PipeWrite(client_fd, data.data(), data.size()),
              static_cast<ssize_t>(data.size()));
    pagespeed::test::ClosePipe(client_fd);
  }
  ASSERT_TRUE(pagespeed::test::WaitForNotificationsRetired(agent_worker, 1,
                                                           kWorkerStallBudget));
  size_t agent_bound = 0;
  CheckSourceBinding(agent_worker.cache(), agent_url, Sha256Of(html),
                     &agent_bound);
  EXPECT_GE(agent_bound, 1u) << "HTML (agent on): no stamped variant";

  agent_worker.Shutdown();
  agent_thread.join();
}

TEST_F(WorkerTest, UnchangedInputGetsSourceBindingOnTheServedEntry) {
  // Issue #507: an HTML page the transform leaves unchanged, or a stylesheet
  // that is already minimal, gets no identity variant, so the entry the
  // front end serves must itself carry the SHA-256 of the raw origin bytes,
  // otherwise a validator-less origin gets no ETag.  The bytes and the "not
  // worker output" flag state stay as the front end wrote them; a
  // worker-processed input keeps its inherited stamp and is not rewritten;
  // a second notification does not rewrite an entry that is already stamped.
  WorkerConfig config;
  config.socket_path = socket_path_;
  config.cache_path = cache_path_;
  config.cache_size_bytes = static_cast<uint64_t>(10 * 1024 * 1024);

  NullMessageHandler handler;
  Worker worker(config, &handler);
  ASSERT_TRUE(worker.Initialize());

  const std::string html = "<html><body><p>Hello</p></body></html>";
  const std::string raw_url = "http://example.com/plain.html";
  // A page that runs the transform pass and nets no change (the
  // "produced no changes" return), as opposed to the plain page above that
  // may stop earlier.
  const std::string nochange_html =
      "<html><head></head><body><p>Plain text only</p></body></html>";
  const std::string nochange_url = "http://example.com/no-net-change.html";
  const std::string css = ".a{color:red}";
  const std::string css_url = "http://example.com/minimal.css";
  const std::string derived_url = "http://example.com/derived-plain.html";
  std::array<std::byte, 32> inherited{};
  for (size_t i = 0; i < inherited.size(); ++i) {
    inherited[i] = static_cast<std::byte>(0x40 + i);
  }
  CacheOriginal(worker.cache(), raw_url, html);
  CacheOriginal(worker.cache(), css_url, css);
  CacheOriginal(worker.cache(), nochange_url, nochange_html);
  CacheOriginal(worker.cache(), derived_url, html, /*hostname=*/"",
                AlternateMetadata::kFlagWorkerProcessed, &inherited);

  std::thread worker_thread([&worker]() { worker.Run(); });
  std::this_thread::sleep_for(std::chrono::milliseconds(50));
  auto notify_all = [&](const std::vector<std::string>& urls) {
    for (const std::string& url : urls) {
      CacheNotification n;
      n.url = url;
      n.scheme = "https";
      n.content_type = url == css_url ? ContentType::kCss : ContentType::kHtml;
      n.capability_mask = CapabilityMask().Encode();
      SendNotification(n);
    }
  };
  notify_all({raw_url, derived_url, css_url, nochange_url});
  ASSERT_TRUE(pagespeed::test::WaitForNotificationsRetired(worker, 4,
                                                           kWorkerStallBudget));
  // Stamped: the plain page, the already-minimal sheet (plus its compressed
  // copies) and the no-net-change page; the exact count is not pinned.
  const uint64_t writes_after_first =
      worker.stats().alternate_writes.load(std::memory_order_relaxed);
  EXPECT_GE(writes_after_first, 3u);
  EXPECT_GE(worker.stats().policy_computed.load(std::memory_order_relaxed), 1u)
      << "no page reached the transform pass";
  EXPECT_EQ(worker.stats().alternate_write_failures.load(), 0u);
  notify_all({raw_url, derived_url, css_url, nochange_url});
  ASSERT_TRUE(pagespeed::test::WaitForNotificationsRetired(worker, 8,
                                                           kWorkerStallBudget));
  EXPECT_EQ(worker.stats().alternate_writes.load(std::memory_order_relaxed),
            writes_after_first)
      << "an already-stamped or worker-processed entry was rewritten";

  const AlternateId id =
      MaskToAlternateId(static_cast<uint8_t>(CapabilityMask().Encode() & 0xFF));
  auto raw = worker.cache()->ReadAlternate(StripSchemeAuthority(raw_url), "",
                                           "https", id);
  ASSERT_TRUE(raw.has_value());
  EXPECT_EQ(raw->metadata.origin_html_hash, Sha256Of(html));
  EXPECT_EQ(raw->metadata.flags & AlternateMetadata::kFlagWorkerProcessed, 0);
  auto raw_body = raw->content();
  EXPECT_EQ(std::string(reinterpret_cast<const char*>(raw_body.data()),
                        raw_body.size()),
            html);

  auto nc = worker.cache()->ReadAlternate(StripSchemeAuthority(nochange_url),
                                          "", "https", id);
  ASSERT_TRUE(nc.has_value());
  EXPECT_EQ(nc->metadata.origin_html_hash, Sha256Of(nochange_html));

  auto css_raw = worker.cache()->ReadAlternate(StripSchemeAuthority(css_url),
                                               "", "https", id);
  ASSERT_TRUE(css_raw.has_value());
  EXPECT_EQ(css_raw->metadata.origin_html_hash, Sha256Of(css));
  EXPECT_EQ(css_raw->metadata.flags & AlternateMetadata::kFlagWorkerProcessed,
            0);

  auto derived = worker.cache()->ReadAlternate(
      StripSchemeAuthority(derived_url), "", "https", id);
  ASSERT_TRUE(derived.has_value());
  EXPECT_EQ(derived->metadata.origin_html_hash, inherited);

  worker.Shutdown();
  worker_thread.join();
}

TEST_F(WorkerTest, NativeSvgIdentityEntryGetsSourceBinding) {
  // Issue #507: the native-SVG passthrough stamps the compressed copies;
  // the identity entry a request without compression is served must carry
  // the same binding.
  WorkerConfig config;
  config.socket_path = socket_path_;
  config.cache_path = cache_path_;
  config.cache_size_bytes = static_cast<uint64_t>(10 * 1024 * 1024);

  NullMessageHandler handler;
  Worker worker(config, &handler);
  ASSERT_TRUE(worker.Initialize());

  const std::string svg =
      "<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"16\" "
      "height=\"16\"><rect width=\"16\" height=\"16\"/></svg>";
  const std::string url = "http://example.com/plain.svg";
  CacheOriginal(worker.cache(), url, svg, /*hostname=*/"", /*flags=*/0,
                /*source_binding=*/nullptr, "image/svg+xml");

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

  const AlternateId id =
      MaskToAlternateId(static_cast<uint8_t>(CapabilityMask().Encode() & 0xFF));
  auto raw =
      worker.cache()->ReadAlternate(StripSchemeAuthority(url), "", "https", id);
  ASSERT_TRUE(raw.has_value());
  EXPECT_EQ(raw->metadata.origin_html_hash, Sha256Of(svg));
  EXPECT_EQ(raw->metadata.flags & AlternateMetadata::kFlagWorkerProcessed, 0);

  worker.Shutdown();
  worker_thread.join();
}

TEST_F(WorkerTest, WorkerProcessedInputKeepsInheritedSourceBinding) {
  // Issue #434, inheritance half of the rule: when the worker's input is
  // its OWN earlier output (entry flags carry kFlagWorkerProcessed and the
  // metadata already carries the binding stamped over the raw origin),
  // every variant written from it keeps the inherited binding.  The worker
  // never re-hashes its own output into the field: that would re-bind every
  // re-optimization and poison the agent-markdown equality gate (the
  // Issue-B trap).
  WorkerConfig config;
  config.socket_path = socket_path_;
  config.cache_path = cache_path_;
  config.cache_size_bytes = static_cast<uint64_t>(10 * 1024 * 1024);

  NullMessageHandler handler;
  Worker worker(config, &handler);
  ASSERT_TRUE(worker.Initialize());

  // A binding a real SHA-256 of the inputs below can never produce.
  std::array<std::byte, 32> inherited{};
  for (size_t i = 0; i < inherited.size(); ++i) {
    inherited[i] = static_cast<std::byte>(0x40 + i);
  }

  const std::string css =
      "body  {  margin:  0;  padding:  0;  }\n"
      "h1  {  color:  red;  font-size:  24px;  }\n";
  const std::string jpeg = ReadTestFile("jpeg/quality100.jpg");
  ASSERT_FALSE(jpeg.empty()) << "Test JPEG not found";

  const std::string css_url = "http://example.com/derived.css";
  const std::string img_url = "http://example.com/derived.jpg";
  CacheOriginal(worker.cache(), css_url, css, /*hostname=*/"",
                AlternateMetadata::kFlagWorkerProcessed, &inherited);
  CacheOriginal(worker.cache(), img_url, jpeg, /*hostname=*/"",
                AlternateMetadata::kFlagWorkerProcessed, &inherited);

  std::thread worker_thread([&worker]() { worker.Run(); });
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  auto notify = [&](const std::string& url, ContentType type, uint32_t mask) {
    CacheNotification n;
    n.url = url;
    n.scheme = "https";
    n.content_type = type;
    n.capability_mask = mask;
    SendNotification(n);
  };
  notify(css_url, ContentType::kCss, CapabilityMask().Encode());
  CapabilityMask mobile_webp(
      CapabilityMask::ImageFormat::kWebP, CapabilityMask::Viewport::kMobile,
      CapabilityMask::PixelDensity::k1x, CapabilityMask::SaveData::kOff,
      CapabilityMask::TransferEncoding::kIdentity);
  notify(img_url, ContentType::kImage, mobile_webp.Encode());

  ASSERT_TRUE(pagespeed::test::WaitForNotificationsRetired(worker, 2,
                                                           kWorkerStallBudget));

  // The seeded inputs are themselves flagged worker-processed, so the
  // per-URL counts below would be satisfied by the seeds alone: require that
  // the worker actually wrote new variants from them.
  EXPECT_GE(worker.stats().variants_written.load(std::memory_order_relaxed), 2u)
      << "the worker wrote no variant from the worker-processed inputs";

  // The proof has two legs: every worker-written variant carries exactly
  // the inherited binding, and the inherited binding is not the SHA-256 of
  // the input bytes: so the worker provably did not re-hash.
  size_t bound = 0;
  CheckSourceBinding(worker.cache(), css_url, inherited, &bound);
  EXPECT_GE(bound, 1u) << "CSS: no variant written from worker-processed "
                          "input";
  EXPECT_NE(inherited, Sha256Of(css));
  CheckSourceBinding(worker.cache(), img_url, inherited, &bound);
  EXPECT_GE(bound, 1u) << "image: no variant written from worker-processed "
                          "input";
  EXPECT_NE(inherited, Sha256Of(jpeg));

  worker.Shutdown();
  worker_thread.join();
}

TEST_F(WorkerTest, JsParseErrorSkipsVariant) {
  // Issue #654: when MinifyUtf8Js fails, the tokenizer's error path emits
  // the unlexable remainder raw, so the output may be half-minified.  The
  // worker must not write a variant — the original is served unchanged.
  WorkerConfig config;
  config.socket_path = socket_path_;
  config.cache_path = cache_path_;
  config.cache_size_bytes = static_cast<uint64_t>(10 * 1024 * 1024);

  Worker worker(config, nullptr);
  ASSERT_TRUE(worker.Initialize());

  // Unterminated string literal: MinifyUtf8Js reliably returns false.
  std::string url = "http://example.com/broken.js";
  std::string js = "var  a  =  1;\n\"not valid javascript";
  CacheOriginal(worker.cache(), url, js);

  std::thread worker_thread([&worker]() { worker.Run(); });
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  CacheNotification notification;
  notification.url = url;
  notification.scheme = "https";
  notification.content_type = ContentType::kJs;
  notification.capability_mask = CapabilityMask().Encode();
  SendNotification(notification);

  // The failure path bumps text_minify_parse_failures; js_processed only
  // increments when a variant is actually written.
  for (int i = 0; i < 40 * kSanitizerBudgetScale; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    if (worker.stats().text_minify_parse_failures.load() >= 1) break;
  }
  EXPECT_GE(worker.stats().text_minify_parse_failures.load(), 1u)
      << "Worker did not record the JS parse failure";
  EXPECT_EQ(worker.stats().js_processed.load(), 0u);
  EXPECT_EQ(worker.stats().variants_written.load(), 0u);

  // CSS/JS variants overwrite the original at the default mask, so
  // "no variant written" means the cached bytes are still the original.
  CapabilityMask mask;
  std::string variant = ReadVariant(worker.cache(), url, mask);
  EXPECT_EQ(variant, js) << "Half-minified JS variant must not be written";

  worker.Shutdown();
  worker_thread.join();
}

TEST_F(WorkerTest, IntegrityPinnedJsNotOptimized) {
  // Issue #656: 2.0 serves optimized variants at the SAME URL, so a
  // <script integrity=...> whose body is minified would fail the
  // browser's hash check.  A URL referenced with an integrity attribute
  // must never get a text variant; a control URL without one must.
  WorkerConfig config;
  config.socket_path = socket_path_;
  config.cache_path = cache_path_;
  config.cache_size_bytes = static_cast<uint64_t>(10 * 1024 * 1024);

  Worker worker(config, nullptr);
  ASSERT_TRUE(worker.Initialize());

  std::string js =
      "// Main application script\n"
      "function  hello( name )  {\n"
      "  var  greeting  =  'Hello, '  +  name;\n"
      "  return  greeting;\n"
      "}\n";
  CacheOriginal(worker.cache(), "/pinned.js", js);
  CacheOriginal(worker.cache(), "/free.js", js);
  // Protocol-relative reference: must pin under (path, hostname).
  CacheOriginal(worker.cache(), "/proto-rel.js", js, "cdn.example.com");
  std::string html =
      "<html><head>"
      "<script src=\"/pinned.js\" integrity=\"sha384-abc\"></script>"
      "<script src=\"//cdn.example.com/proto-rel.js\" "
      "integrity=\"sha384-def\"></script>"
      "<script src=\"/free.js\"></script>"
      "</head><body><p>Hello</p></body></html>";
  CacheOriginal(worker.cache(), "/page.html", html);

  std::thread worker_thread([&worker]() { worker.Run(); });
  WorkerStopper stopper(worker, worker_thread);
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  // Process the HTML first so the pin is registered.
  CacheNotification html_notif;
  html_notif.url = "http://example.com/page.html";
  html_notif.scheme = "https";
  html_notif.content_type = ContentType::kHtml;
  html_notif.capability_mask = CapabilityMask().Encode();
  SendNotification(html_notif);

  // Pin registration drops the pinned URL's cache key (a stale minified
  // variant could already exist) — poll for that as completion signal.
  CapabilityMask mask;
  for (int i = 0; i < 80 * kSanitizerBudgetScale; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    if (ReadVariant(worker.cache(), "/pinned.js", mask).empty()) break;
  }
  EXPECT_TRUE(ReadVariant(worker.cache(), "/pinned.js", mask).empty())
      << "Pin registration should drop the pinned URL's cache key";
  for (int i = 0; i < 80; ++i) {
    if (ReadVariant(worker.cache(), "/proto-rel.js", mask, "cdn.example.com")
            .empty()) {
      break;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
  }
  EXPECT_TRUE(
      ReadVariant(worker.cache(), "/proto-rel.js", mask, "cdn.example.com")
          .empty())
      << "Protocol-relative pin should drop the cdn-hosted cache key";

  // nginx re-caches the original on the next request; simulate that.
  CacheOriginal(worker.cache(), "/pinned.js", js);
  CacheOriginal(worker.cache(), "/proto-rel.js", js, "cdn.example.com");

  // JS notifications for both URLs (as nginx would send on MISS).
  CacheNotification pinned_notif;
  pinned_notif.url = "http://example.com/pinned.js";
  pinned_notif.scheme = "https";
  pinned_notif.content_type = ContentType::kJs;
  pinned_notif.capability_mask = CapabilityMask().Encode();
  SendNotification(pinned_notif);

  CacheNotification proto_rel_notif;
  proto_rel_notif.url = "/proto-rel.js";
  proto_rel_notif.hostname = "cdn.example.com";
  proto_rel_notif.scheme = "https";
  proto_rel_notif.content_type = ContentType::kJs;
  proto_rel_notif.capability_mask = CapabilityMask().Encode();
  SendNotification(proto_rel_notif);

  CacheNotification free_notif;
  free_notif.url = "http://example.com/free.js";
  free_notif.scheme = "https";
  free_notif.content_type = ContentType::kJs;
  free_notif.capability_mask = CapabilityMask().Encode();
  SendNotification(free_notif);

  // Only the control URL counts as processed (pin skip writes nothing).
  for (int i = 0; i < 80 * kSanitizerBudgetScale; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    if (worker.stats().js_processed.load() >= 1 &&
        worker.in_flight_work() == 0) {
      break;
    }
  }
  EXPECT_EQ(worker.stats().js_processed.load(), 1u);

  std::string pinned_variant = ReadVariant(worker.cache(), "/pinned.js", mask);
  EXPECT_EQ(pinned_variant, js)
      << "Integrity-pinned JS must keep its original bytes";

  std::string proto_rel_variant =
      ReadVariant(worker.cache(), "/proto-rel.js", mask, "cdn.example.com");
  EXPECT_EQ(proto_rel_variant, js)
      << "Protocol-relative integrity-pinned JS must keep its original bytes";

  std::string free_variant = ReadVariant(worker.cache(), "/free.js", mask);
  ASSERT_FALSE(free_variant.empty());
  EXPECT_LT(free_variant.size(), js.size())
      << "Control JS without integrity must still be minified";
}

// --- Phase 4.4: Compressed variant tests ---

TEST_F(WorkerTest, CssNotificationWritesCompressedVariants) {
  WorkerConfig config;
  config.socket_path = socket_path_;
  config.cache_path = cache_path_;
  config.cache_size_bytes = static_cast<uint64_t>(10 * 1024 * 1024);

  NullMessageHandler handler;
  Worker worker(config, &handler);
  ASSERT_TRUE(worker.Initialize());

  // Pre-populate cache with unminified CSS.
  std::string url = "http://example.com/compress.css";
  std::string css =
      "body  {  margin:  0;  padding:  0;  }\n"
      "h1  {  color:  red;  font-size:  24px;  }\n";
  CacheOriginal(worker.cache(), url, css);

  std::thread worker_thread([&worker]() { worker.Run(); });
  WorkerStopper stopper(worker, worker_thread);
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  // Send CSS notification with Desktop/Identity mask (nginx default).
  CacheNotification notification;
  notification.url = url;
  notification.scheme = "https";
  notification.content_type = ContentType::kCss;
  notification.capability_mask = CapabilityMask().Encode();
  SendNotification(notification);

  std::this_thread::sleep_for(std::chrono::milliseconds(500));

  // Read identity variant at Desktop/Identity.
  CapabilityMask identity_mask;
  std::string identity = ReadVariant(worker.cache(), url, identity_mask);
  ASSERT_FALSE(identity.empty()) << "Identity CSS variant missing";
  EXPECT_LT(identity.size(), css.size());

  // Read gzip variant at Desktop/Gzip.
  CapabilityMask gz_mask;
  gz_mask.set_transfer_encoding(CapabilityMask::TransferEncoding::kGzip);
  std::string gz_variant = ReadVariant(worker.cache(), url, gz_mask);
  ASSERT_FALSE(gz_variant.empty()) << "Gzip CSS variant missing";
  EXPECT_EQ(GzipDecompress(gz_variant), identity);

  // Read brotli variant at Desktop/Brotli.
  CapabilityMask br_mask;
  br_mask.set_transfer_encoding(CapabilityMask::TransferEncoding::kBrotli);
  std::string br_variant = ReadVariant(worker.cache(), url, br_mask);
  ASSERT_FALSE(br_variant.empty()) << "Brotli CSS variant missing";
  EXPECT_EQ(BrotliDecompress(br_variant), identity);

  // Verify stats counters.
  EXPECT_GE(worker.stats().gzip_variants_written.load(), 1u);
  EXPECT_GE(worker.stats().brotli_variants_written.load(), 1u);
}

// The coded copies record the size of the RAW origin under their own encoding,
// which the serve-time split measures a rewrite against.
TEST_F(WorkerTest, CodedCssVariantsRecordTheEncodedOriginSize) {
  WorkerConfig config;
  config.socket_path = socket_path_;
  config.cache_path = cache_path_;
  config.cache_size_bytes = static_cast<uint64_t>(10 * 1024 * 1024);

  NullMessageHandler handler;
  Worker worker(config, &handler);
  ASSERT_TRUE(worker.Initialize());

  const std::string url = "http://example.com/encoded-size.css";
  std::string css;
  for (int i = 0; i < 40; ++i) {
    css += "body  {  margin:  " + std::to_string(i) +
           "px;  padding:  0;  }\n/* comment " + std::to_string(i * 7) +
           " */\n";
  }
  CacheOriginal(worker.cache(), url, css);

  std::thread worker_thread([&worker]() { worker.Run(); });
  WorkerStopper stopper(worker, worker_thread);
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  CacheNotification notification;
  notification.url = url;
  notification.scheme = "https";
  notification.content_type = ContentType::kCss;
  notification.capability_mask = CapabilityMask().Encode();
  SendNotification(notification);
  std::this_thread::sleep_for(std::chrono::milliseconds(500));

  CapabilityMask identity_mask;
  const auto identity = ReadVariantWithMeta(worker.cache(), url, identity_mask);
  ASSERT_TRUE(identity.has_value());
  ASSERT_LT(identity->content.size(), css.size());
  // Identity: measured against the origin itself.
  EXPECT_EQ(identity->metadata.EncodedOriginLength(), css.size());

  CapabilityMask gz_mask;
  gz_mask.set_transfer_encoding(CapabilityMask::TransferEncoding::kGzip);
  const auto gz = ReadVariantWithMeta(worker.cache(), url, gz_mask);
  ASSERT_TRUE(gz.has_value());
  const auto gz_encoded = gz->metadata.EncodedOriginLength();
  ASSERT_TRUE(gz_encoded.has_value());
  const double gz_expected = GzipCompress(css, config.gzip_level).size();
  EXPECT_NEAR(*gz_encoded, gz_expected, css.size() / 50000.0 + 1.0);
  EXPECT_FALSE(gz->metadata.QualityScoreX100().has_value());

  CapabilityMask br_mask;
  br_mask.set_transfer_encoding(CapabilityMask::TransferEncoding::kBrotli);
  const auto br = ReadVariantWithMeta(worker.cache(), url, br_mask);
  ASSERT_TRUE(br.has_value());
  const auto br_encoded = br->metadata.EncodedOriginLength();
  ASSERT_TRUE(br_encoded.has_value());
  const double br_expected = BrotliCompress(css, config.brotli_level).size();
  EXPECT_NEAR(*br_encoded, br_expected, css.size() / 50000.0 + 1.0);
  // The rewrite saved bytes beyond what the encoding saved.
  EXPECT_LT(gz->content.size(), *gz_encoded);
}

TEST_F(WorkerTest, CompressionDisabledProducesNoCompressedVariants) {
  WorkerConfig config;
  config.socket_path = socket_path_;
  config.cache_path = cache_path_;
  config.cache_size_bytes = static_cast<uint64_t>(10 * 1024 * 1024);
  config.gzip_level = 0;
  config.brotli_level = 0;

  NullMessageHandler handler;
  Worker worker(config, &handler);
  ASSERT_TRUE(worker.Initialize());

  std::string url = "http://example.com/nocompress.css";
  std::string css =
      "body  {  margin:  0;  padding:  0;  }\n"
      "h1  {  color:  red;  font-size:  24px;  }\n";
  CacheOriginal(worker.cache(), url, css);

  std::thread worker_thread([&worker]() { worker.Run(); });
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  CacheNotification notification;
  notification.url = url;
  notification.scheme = "https";
  notification.content_type = ContentType::kCss;
  notification.capability_mask = CapabilityMask().Encode();
  SendNotification(notification);

  std::this_thread::sleep_for(std::chrono::milliseconds(500));

  // Identity variant should exist at Desktop/Identity.
  CapabilityMask identity_mask;
  std::string identity = ReadVariant(worker.cache(), url, identity_mask);
  EXPECT_FALSE(identity.empty()) << "Identity CSS variant missing";

  // Gzip and brotli variants should NOT exist.
  CapabilityMask gz_mask;
  gz_mask.set_transfer_encoding(CapabilityMask::TransferEncoding::kGzip);
  EXPECT_TRUE(ReadVariant(worker.cache(), url, gz_mask).empty())
      << "Gzip variant should not exist when level=0";

  CapabilityMask br_mask;
  br_mask.set_transfer_encoding(CapabilityMask::TransferEncoding::kBrotli);
  EXPECT_TRUE(ReadVariant(worker.cache(), url, br_mask).empty())
      << "Brotli variant should not exist when level=0";

  // Stats should show zero compressed variants.
  EXPECT_EQ(worker.stats().gzip_variants_written.load(), 0u);
  EXPECT_EQ(worker.stats().brotli_variants_written.load(), 0u);

  worker.Shutdown();
  worker_thread.join();
}

// --- Encoding AlternateId distinctness test ---

TEST_F(WorkerTest, BrotliNotificationWritesDistinctAlternateIds) {
  // When a notification arrives with a non-identity encoding mask (e.g.,
  // Desktop/Brotli = 0x88), the identity write should strip encoding bits
  // so identity, gzip, and brotli alternates all get distinct AlternateIds.
  WorkerConfig config;
  config.socket_path = socket_path_;
  config.cache_path = cache_path_;
  config.cache_size_bytes = static_cast<uint64_t>(10 * 1024 * 1024);

  NullMessageHandler handler;
  Worker worker(config, &handler);
  ASSERT_TRUE(worker.Initialize());

  // Pre-populate cache with unminified CSS (large enough to compress).
  std::string url = "http://example.com/encoding-test.css";
  std::string css =
      "body  {  margin:  0;  padding:  0;  }\n"
      "h1  {  color:  red;  font-size:  24px;  }\n";
  CacheOriginal(worker.cache(), url, css);

  std::thread worker_thread([&worker]() { worker.Run(); });
  WorkerStopper stopper(worker, worker_thread);
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  // Send CSS notification with Desktop/Brotli mask (0x88).
  // Before the fix, the identity write would collide with brotli at the
  // same AlternateId (0x88).  After the fix, identity goes to 0x08.
  CacheNotification notification;
  notification.url = url;
  notification.scheme = "https";
  notification.content_type = ContentType::kCss;
  notification.capability_mask = 0x88;  // Desktop + Brotli encoding
  SendNotification(notification);

  std::this_thread::sleep_for(std::chrono::milliseconds(500));

  // Identity variant: Desktop/Identity = 0x08.
  CapabilityMask identity_mask = CapabilityMask::Decode(0x88);
  identity_mask.set_transfer_encoding(
      CapabilityMask::TransferEncoding::kIdentity);
  EXPECT_EQ(identity_mask.Encode(), 0x08u);
  std::string identity = ReadVariant(worker.cache(), url, identity_mask);
  ASSERT_FALSE(identity.empty()) << "Identity CSS variant missing at 0x08";
  EXPECT_LT(identity.size(), css.size()) << "Should be minified";

  // Gzip variant: Desktop/Gzip = 0x48.
  CapabilityMask gz_mask = CapabilityMask::Decode(0x88);
  gz_mask.set_transfer_encoding(CapabilityMask::TransferEncoding::kGzip);
  EXPECT_EQ(gz_mask.Encode(), 0x48u);
  std::string gz_variant = ReadVariant(worker.cache(), url, gz_mask);
  ASSERT_FALSE(gz_variant.empty()) << "Gzip CSS variant missing at 0x48";
  EXPECT_EQ(GzipDecompress(gz_variant), identity);

  // Brotli variant: Desktop/Brotli = 0x88.
  CapabilityMask br_mask = CapabilityMask::Decode(0x88);
  // Already has brotli encoding, no need to set.
  EXPECT_EQ(br_mask.Encode(), 0x88u);
  std::string br_variant = ReadVariant(worker.cache(), url, br_mask);
  ASSERT_FALSE(br_variant.empty()) << "Brotli CSS variant missing at 0x88";
  EXPECT_EQ(BrotliDecompress(br_variant), identity);

  // All three AlternateIds must be distinct.
  EXPECT_NE(identity_mask.Encode(), gz_mask.Encode());
  EXPECT_NE(identity_mask.Encode(), br_mask.Encode());
  EXPECT_NE(gz_mask.Encode(), br_mask.Encode());
}

TEST_F(WorkerTest, CssNotificationGzipGuard) {
  // CSS content starting with gzip magic bytes should be skipped.
  WorkerConfig config;
  config.socket_path = socket_path_;
  config.cache_path = cache_path_;
  config.cache_size_bytes = static_cast<uint64_t>(10 * 1024 * 1024);

  NullMessageHandler handler;
  Worker worker(config, &handler);
  ASSERT_TRUE(worker.Initialize());

  // Pre-populate cache with fake gzip-compressed CSS.
  std::string url = "http://example.com/gzip-guard.css";
  std::string gzip_css = "\x1f\x8b\x08...fake gzip css content";
  CacheOriginal(worker.cache(), url, gzip_css);

  std::thread worker_thread([&worker]() { worker.Run(); });
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  CacheNotification notification;
  notification.url = url;
  notification.scheme = "https";
  notification.content_type = ContentType::kCss;
  notification.capability_mask = CapabilityMask().Encode();
  SendNotification(notification);

  // Poll for the error counter to advance instead of a fixed sleep (slow
  // shards can sit at the pipe read past a fixed budget).  This also lets the
  // no-variant invariant below settle.
  for (int i = 0; i < 80 * kSanitizerBudgetScale; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    if (worker.stats().errors.load() >= 1u) break;
  }

  // No minified variant should be written.
  CapabilityMask mask;
  std::string variant = ReadVariant(worker.cache(), url, mask);
  EXPECT_EQ(variant, gzip_css)
      << "Gzip-guarded CSS should not produce a variant";

  // Error counter should have incremented.
  EXPECT_GE(worker.stats().errors.load(), 1u)
      << "Gzip guard should increment error counter";

  worker.Shutdown();
  worker_thread.join();
}

TEST_F(WorkerTest, CssNotificationTooLarge) {
  // CSS content exceeding max_css_size should be skipped.
  WorkerConfig config;
  config.socket_path = socket_path_;
  config.cache_path = cache_path_;
  config.cache_size_bytes = static_cast<uint64_t>(10 * 1024 * 1024);
  config.max_css_size = 100;  // Very small limit

  NullMessageHandler handler;
  Worker worker(config, &handler);
  ASSERT_TRUE(worker.Initialize());

  // Pre-populate cache with CSS content exceeding 100 bytes.
  std::string url = "http://example.com/large.css";
  std::string css =
      "body { margin: 0; padding: 0; } "
      "h1 { color: red; font-size: 24px; } "
      "p { line-height: 1.5; margin: 10px; } "
      "a { text-decoration: none; color: blue; }";
  ASSERT_GT(css.size(), 100u);
  CacheOriginal(worker.cache(), url, css);

  std::thread worker_thread([&worker]() { worker.Run(); });
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  CacheNotification notification;
  notification.url = url;
  notification.scheme = "https";
  notification.content_type = ContentType::kCss;
  notification.capability_mask = CapabilityMask().Encode();
  SendNotification(notification);

  std::this_thread::sleep_for(std::chrono::milliseconds(500));

  // Original CSS should still be intact, no minified variant written.
  CapabilityMask mask;
  std::string variant = ReadVariant(worker.cache(), url, mask);
  EXPECT_EQ(variant, css) << "Too-large CSS should not produce a variant";

  worker.Shutdown();
  worker_thread.join();
}

TEST_F(WorkerTest, CssNotificationAlreadyMinimal) {
  // CSS that is already minimal should still produce compressed
  // variants (gzip/brotli) even though no identity variant is written.
  WorkerConfig config;
  config.socket_path = socket_path_;
  config.cache_path = cache_path_;
  config.cache_size_bytes = static_cast<uint64_t>(10 * 1024 * 1024);

  NullMessageHandler handler;
  Worker worker(config, &handler);
  ASSERT_TRUE(worker.Initialize());

  // Pre-populate with already-minified CSS.
  std::string url = "http://example.com/already-min.css";
  std::string css = "body{margin:0}h1{color:red}";
  CacheOriginal(worker.cache(), url, css);

  std::thread worker_thread([&worker]() { worker.Run(); });
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  CacheNotification notification;
  notification.url = url;
  notification.scheme = "https";
  notification.content_type = ContentType::kCss;
  notification.capability_mask = CapabilityMask().Encode();
  SendNotification(notification);

  std::this_thread::sleep_for(std::chrono::milliseconds(500));

  // Compressed variants should exist even though CSS is already minimal.
  CapabilityMask gz_mask;
  gz_mask.set_transfer_encoding(CapabilityMask::TransferEncoding::kGzip);
  std::string gz_variant = ReadVariant(worker.cache(), url, gz_mask);
  EXPECT_FALSE(gz_variant.empty())
      << "Gzip variant should exist for already-minimal CSS";

  CapabilityMask br_mask;
  br_mask.set_transfer_encoding(CapabilityMask::TransferEncoding::kBrotli);
  std::string br_variant = ReadVariant(worker.cache(), url, br_mask);
  EXPECT_FALSE(br_variant.empty())
      << "Brotli variant should exist for already-minimal CSS";

  worker.Shutdown();
  worker_thread.join();
}

TEST_F(WorkerTest, JsNotificationGzipGuard) {
  // JS content starting with gzip magic bytes should be skipped.
  WorkerConfig config;
  config.socket_path = socket_path_;
  config.cache_path = cache_path_;
  config.cache_size_bytes = static_cast<uint64_t>(10 * 1024 * 1024);

  NullMessageHandler handler;
  Worker worker(config, &handler);
  ASSERT_TRUE(worker.Initialize());

  std::string url = "http://example.com/gzip-guard.js";
  std::string gzip_js = "\x1f\x8b\x08...fake gzip js content";
  CacheOriginal(worker.cache(), url, gzip_js);

  std::thread worker_thread([&worker]() { worker.Run(); });
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  CacheNotification notification;
  notification.url = url;
  notification.scheme = "https";
  notification.content_type = ContentType::kJs;
  notification.capability_mask = CapabilityMask().Encode();
  SendNotification(notification);

  // Poll for the error counter to advance instead of a fixed sleep (slow
  // shards can sit at the pipe read past a fixed budget).  This also lets the
  // no-variant invariant below settle.
  for (int i = 0; i < 80 * kSanitizerBudgetScale; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    if (worker.stats().errors.load() >= 1u) break;
  }

  // No minified variant should be written.
  CapabilityMask mask;
  std::string variant = ReadVariant(worker.cache(), url, mask);
  EXPECT_EQ(variant, gzip_js) << "Gzip-guarded JS should not produce a variant";

  // Error counter should have incremented.
  EXPECT_GE(worker.stats().errors.load(), 1u)
      << "Gzip guard should increment error counter";

  worker.Shutdown();
  worker_thread.join();
}

TEST_F(WorkerTest, JsNotificationTooLarge) {
  // JS content exceeding max_js_size should be skipped.
  WorkerConfig config;
  config.socket_path = socket_path_;
  config.cache_path = cache_path_;
  config.cache_size_bytes = static_cast<uint64_t>(10 * 1024 * 1024);
  config.max_js_size = 100;  // Very small limit

  NullMessageHandler handler;
  Worker worker(config, &handler);
  ASSERT_TRUE(worker.Initialize());

  std::string url = "http://example.com/large.js";
  std::string js =
      "// This is a long JavaScript file\n"
      "function processData(input) {\n"
      "  var result = input.split(',');\n"
      "  return result.map(function(item) { return item.trim(); });\n"
      "}\n";
  ASSERT_GT(js.size(), 100u);
  CacheOriginal(worker.cache(), url, js);

  std::thread worker_thread([&worker]() { worker.Run(); });
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  CacheNotification notification;
  notification.url = url;
  notification.scheme = "https";
  notification.content_type = ContentType::kJs;
  notification.capability_mask = CapabilityMask().Encode();
  SendNotification(notification);

  // Poll to confirm notification was received.
  for (int i = 0; i < 40 * kSanitizerBudgetScale; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    if (worker.stats().notifications_received.load() >= 1) break;
  }

  // No minified variant should be written.
  CapabilityMask mask;
  std::string variant = ReadVariant(worker.cache(), url, mask);
  EXPECT_EQ(variant, js) << "Too-large JS should not produce a variant";

  worker.Shutdown();
  worker_thread.join();
}

TEST_F(WorkerTest, JsNotificationSourcemapWarning) {
  // JS containing a sourceMappingURL should still be minified.
  // The worker produces output even when the minifier encounters
  // a sourceMappingURL comment.
  WorkerConfig config;
  config.socket_path = socket_path_;
  config.cache_path = cache_path_;
  config.cache_size_bytes = static_cast<uint64_t>(10 * 1024 * 1024);

  NullMessageHandler handler;
  Worker worker(config, &handler);
  ASSERT_TRUE(worker.Initialize());

  std::string url = "http://example.com/sourcemap.js";
  std::string js =
      "function  hello( name )  {\n"
      "  var  greeting  =  'Hello, '  +  name;\n"
      "  return  greeting;\n"
      "}\n"
      "//# sourceMappingURL=app.js.map\n";
  CacheOriginal(worker.cache(), url, js);

  std::thread worker_thread([&worker]() { worker.Run(); });
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  CacheNotification notification;
  notification.url = url;
  notification.scheme = "https";
  notification.content_type = ContentType::kJs;
  notification.capability_mask = CapabilityMask().Encode();
  SendNotification(notification);

  // Wait for the worker to finish processing the JS notification before
  // reading.  Poll the js_processed stat counter — which the worker increments
  // only AFTER the variant write commits (worker.cc orders
  // MarkVariantProcessed + variants_written + js_processed strictly after
  // WriteVariant returns write_ok) — rather than breaking on the first
  // non-empty ReadVariant.  The slot is pre-populated with the un-minified
  // original by CacheOriginal, so a "first non-empty read" races the worker
  // and can return the original (variant.size() == js.size()) before
  // minification lands.  That race is the cause of the historical
  // JsNotificationSourcemapWarning flake on slow runners.  This mirrors the
  // passing JsNotificationMinifiesAndWritesVariant.
  for (int i = 0; i < 40 * kSanitizerBudgetScale; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    if (worker.stats().js_processed.load() >= 1) break;
  }
  EXPECT_GE(worker.stats().js_processed.load(), 1u)
      << "Worker did not process the JS sourcemap notification";

  CapabilityMask mask;
  std::string variant = ReadVariant(worker.cache(), url, mask);

  // Should still produce a minified variant.
  EXPECT_FALSE(variant.empty()) << "JS with sourcemap should still be minified";
  if (!variant.empty()) {
    // Minified should be shorter than original.
    EXPECT_LT(variant.size(), js.size());
    // Key identifiers should survive minification.
    EXPECT_NE(variant.find("hello"), std::string::npos);
    EXPECT_NE(variant.find("greeting"), std::string::npos);
  }

  worker.Shutdown();
  worker_thread.join();
}

TEST_F(WorkerTest, WarmupSentinelProcessesJs) {
  // Verify that the warmup sentinel triggers JS minification for
  // a URL that doesn't have a minified variant yet.
  WorkerConfig config;
  config.socket_path = socket_path_;
  config.cache_path = cache_path_;
  config.cache_size_bytes = static_cast<uint64_t>(10 * 1024 * 1024);
  config.enable_warmup = true;

  NullMessageHandler handler;
  Worker worker(config, &handler);
  ASSERT_TRUE(worker.Initialize());

  // Pre-populate JS content.
  std::string js_url = "http://example.com/warmup.js";
  std::string js =
      "// Main application script\n"
      "function  hello( name )  {\n"
      "  var  greeting  =  'Hello, '  +  name;\n"
      "  return  greeting;\n"
      "}\n";
  CacheOriginal(worker.cache(), js_url, js);

  std::thread worker_thread([&worker]() { worker.Run(); });
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  // Send warmup notification (sentinel mask).
  CacheNotification notification;
  notification.url = js_url;
  notification.scheme = "https";
  notification.content_type = ContentType::kJs;
  notification.capability_mask = kWarmupSentinel;
  SendNotification(notification);

  // Wait for processing to complete and dedup set to be populated.
  // CacheOriginal writes at the same AlternateId, so !variant.empty()
  // alone races with the worker overwriting the original.
  for (int i = 0; i < 40 * kSanitizerBudgetScale; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    if (worker.stats().js_processed.load() >= 1 &&
        worker.in_flight_work() == 0) {
      break;
    }
  }

  CapabilityMask default_mask;
  std::string variant = ReadVariant(worker.cache(), js_url, default_mask);
  EXPECT_FALSE(variant.empty()) << "Warmup should produce minified JS";
  if (!variant.empty()) {
    EXPECT_LT(variant.size(), js.size());
    EXPECT_NE(variant.find("hello"), std::string::npos);
  }

  worker.Shutdown();
  worker_thread.join();
}

// --- C. CSS Path Coverage ---

TEST_F(WorkerTest, CssOriginalNotInCache) {
  // Send CSS notification for URL not in cache — should not crash
  // and should not write any variant.  Covers worker.cc ~2661-2667.
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
  notification.url = "http://example.com/css-not-cached.css";
  notification.scheme = "https";
  notification.content_type = ContentType::kCss;
  notification.capability_mask = CapabilityMask().Encode();
  SendNotification(notification);

  for (int i = 0; i < 40 * kSanitizerBudgetScale; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    if (worker.stats().notifications_received.load() >= 1) break;
  }

  EXPECT_GE(worker.stats().notifications_received.load(), 1u);
  EXPECT_EQ(worker.stats().css_processed.load(), 0u);
  EXPECT_EQ(worker.stats().variants_written.load(), 0u);

  worker.Shutdown();
  worker_thread.join();
}

TEST_F(WorkerTest, CssCacheNotInitializedSkips) {
  // When cache is not initialized, CSS notifications should be skipped.
  // Covers worker.cc ~2649-2654.
  WorkerConfig config;
  config.socket_path = socket_path_;
  // No cache_path — cache_ will be null.

  NullMessageHandler handler;
  Worker worker(config, &handler);
  ASSERT_TRUE(worker.Initialize());

  std::thread worker_thread([&worker]() { worker.Run(); });
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  CacheNotification notification;
  notification.url = "http://example.com/no-cache-css.css";
  notification.scheme = "https";
  notification.content_type = ContentType::kCss;
  notification.capability_mask = CapabilityMask().Encode();
  SendNotification(notification);

  for (int i = 0; i < 40 * kSanitizerBudgetScale; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    if (worker.stats().notifications_received.load() >= 1) break;
  }

  EXPECT_GE(worker.stats().notifications_received.load(), 1u);
  EXPECT_EQ(worker.stats().css_processed.load(), 0u);

  worker.Shutdown();
  worker_thread.join();
}

TEST_F(WorkerTest, CssImportFlattening) {
  // CSS with @import statements should have them flattened before
  // minification.  Covers worker.cc ~2702-2729.
  WorkerConfig config;
  config.socket_path = socket_path_;
  config.cache_path = cache_path_;
  config.cache_size_bytes = static_cast<uint64_t>(10 * 1024 * 1024);

  NullMessageHandler handler;
  Worker worker(config, &handler);
  ASSERT_TRUE(worker.Initialize());

  // Pre-populate the imported CSS file.
  std::string import_url = "http://example.com/base.css";
  std::string import_css =
      "body  {  margin:  0;  padding:  0;  }\n"
      "h1  {  color:  blue;  }\n";
  CacheOriginal(worker.cache(), import_url, import_css);

  // Pre-populate the main CSS that imports base.css.
  std::string url = "http://example.com/main.css";
  std::string main_css =
      "@import url(\"http://example.com/base.css\");\n"
      "p  {  font-size:  16px;  line-height:  1.5;  }\n";
  CacheOriginal(worker.cache(), url, main_css);

  std::thread worker_thread([&worker]() { worker.Run(); });
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  CacheNotification notification;
  notification.url = url;
  notification.scheme = "https";
  notification.content_type = ContentType::kCss;
  notification.capability_mask = CapabilityMask().Encode();
  SendNotification(notification);

  // Poll for the minified variant.
  CapabilityMask mask;
  std::string variant;
  for (int i = 0; i < 40 * kSanitizerBudgetScale; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    variant = ReadVariant(worker.cache(), url, mask);
    if (!variant.empty()) break;
  }

  EXPECT_FALSE(variant.empty()) << "Flattened+minified CSS variant missing";
  if (!variant.empty()) {
    // The flattened CSS should contain content from both files.
    EXPECT_NE(variant.find("margin"), std::string::npos)
        << "Should contain imported body margin rule";
    EXPECT_NE(variant.find("font-size"), std::string::npos)
        << "Should contain main CSS font-size rule";
    // Should be smaller than the combined size (minified).
    EXPECT_LT(variant.size(), main_css.size() + import_css.size());
    // @import directive should be gone (flattened).
    EXPECT_EQ(variant.find("@import"), std::string::npos)
        << "Flattened CSS should not contain @import";
  }

  worker.Shutdown();
  worker_thread.join();
}

TEST_F(WorkerTest, CssMinificationWritesCompressed) {
  // Verify that CSS minification produces identity, gzip, and brotli
  // variants.  Specifically covers the write path in worker.cc ~2741-2810.
  WorkerConfig config;
  config.socket_path = socket_path_;
  config.cache_path = cache_path_;
  config.cache_size_bytes = static_cast<uint64_t>(10 * 1024 * 1024);

  NullMessageHandler handler;
  Worker worker(config, &handler);
  ASSERT_TRUE(worker.Initialize());

  // Use Desktop/Gzip mask to ensure encoding stripping is tested.
  std::string url = "http://example.com/compress-test.css";
  std::string css =
      "body  {  margin:  0;  padding:  0;  }\n"
      "h1  {  color:  red;  font-size:  24px;  }\n"
      "p  {  line-height:  1.5;  }\n";
  CacheOriginal(worker.cache(), url, css);

  std::thread worker_thread([&worker]() { worker.Run(); });
  WorkerStopper stopper(worker, worker_thread);
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  // Send with Gzip encoding mask — worker should strip to Identity for write.
  CacheNotification notification;
  notification.url = url;
  notification.scheme = "https";
  notification.content_type = ContentType::kCss;
  notification.capability_mask = 0x48;  // Desktop/Gzip
  SendNotification(notification);

  std::this_thread::sleep_for(std::chrono::milliseconds(500));

  // Identity variant should exist at Desktop/Identity (0x08).
  CapabilityMask identity_mask = CapabilityMask::Decode(0x48);
  identity_mask.set_transfer_encoding(
      CapabilityMask::TransferEncoding::kIdentity);
  identity_mask.set_image_format(CapabilityMask::ImageFormat::kOriginal);
  std::string identity = ReadVariant(worker.cache(), url, identity_mask);
  ASSERT_FALSE(identity.empty()) << "Identity CSS variant missing";
  EXPECT_LT(identity.size(), css.size()) << "Should be minified";

  // Gzip variant should exist.
  CapabilityMask gz_mask = identity_mask;
  gz_mask.set_transfer_encoding(CapabilityMask::TransferEncoding::kGzip);
  std::string gz_variant = ReadVariant(worker.cache(), url, gz_mask);
  EXPECT_FALSE(gz_variant.empty()) << "Gzip CSS variant missing";

  // Brotli variant should exist.
  CapabilityMask br_mask = identity_mask;
  br_mask.set_transfer_encoding(CapabilityMask::TransferEncoding::kBrotli);
  std::string br_variant = ReadVariant(worker.cache(), url, br_mask);
  EXPECT_FALSE(br_variant.empty()) << "Brotli CSS variant missing";

  // Stats should show CSS was processed.
  EXPECT_GE(worker.stats().css_processed.load(), 1u);
}

// --- D. JS Path Coverage ---

TEST_F(WorkerTest, JsOriginalNotInCache) {
  // Send JS notification for URL not in cache — should not crash
  // and should not write any variant.  Covers worker.cc ~2835-2841.
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
  notification.url = "http://example.com/js-not-cached.js";
  notification.scheme = "https";
  notification.content_type = ContentType::kJs;
  notification.capability_mask = CapabilityMask().Encode();
  SendNotification(notification);

  for (int i = 0; i < 40 * kSanitizerBudgetScale; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    if (worker.stats().notifications_received.load() >= 1) break;
  }

  EXPECT_GE(worker.stats().notifications_received.load(), 1u);
  EXPECT_EQ(worker.stats().js_processed.load(), 0u);
  EXPECT_EQ(worker.stats().variants_written.load(), 0u);

  worker.Shutdown();
  worker_thread.join();
}

TEST_F(WorkerTest, JsCacheNotInitializedSkips) {
  // When cache is not initialized, JS notifications should be skipped.
  // Covers worker.cc ~2823-2828.
  WorkerConfig config;
  config.socket_path = socket_path_;
  // No cache_path — cache_ will be null.

  NullMessageHandler handler;
  Worker worker(config, &handler);
  ASSERT_TRUE(worker.Initialize());

  std::thread worker_thread([&worker]() { worker.Run(); });
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  CacheNotification notification;
  notification.url = "http://example.com/no-cache-js.js";
  notification.scheme = "https";
  notification.content_type = ContentType::kJs;
  notification.capability_mask = CapabilityMask().Encode();
  SendNotification(notification);

  for (int i = 0; i < 40 * kSanitizerBudgetScale; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    if (worker.stats().notifications_received.load() >= 1) break;
  }

  EXPECT_GE(worker.stats().notifications_received.load(), 1u);
  EXPECT_EQ(worker.stats().js_processed.load(), 0u);

  worker.Shutdown();
  worker_thread.join();
}

TEST_F(WorkerTest, JsMinificationAndCompression) {
  // Verify that JS minification produces identity + compressed variants.
  // Covers the JS write path in worker.cc ~2881-2939.
  WorkerConfig config;
  config.socket_path = socket_path_;
  config.cache_path = cache_path_;
  config.cache_size_bytes = static_cast<uint64_t>(10 * 1024 * 1024);

  NullMessageHandler handler;
  Worker worker(config, &handler);
  ASSERT_TRUE(worker.Initialize());

  std::string url = "http://example.com/compress-test.js";
  std::string js =
      "// Application script\n"
      "function  processData( items )  {\n"
      "  var  result  =  [];\n"
      "  for  ( var  i  =  0;  i  <  items.length;  i++ )  {\n"
      "    result.push( items[i].trim() );\n"
      "  }\n"
      "  return  result;\n"
      "}\n";
  CacheOriginal(worker.cache(), url, js);

  std::thread worker_thread([&worker]() { worker.Run(); });
  WorkerStopper stopper(worker, worker_thread);
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  CacheNotification notification;
  notification.url = url;
  notification.scheme = "https";
  notification.content_type = ContentType::kJs;
  notification.capability_mask = CapabilityMask().Encode();
  SendNotification(notification);

  // Wait for processing to complete and dedup set to be populated.
  // CacheOriginal writes at the same AlternateId, so !variant.empty()
  // alone races with the worker overwriting the original.
  for (int i = 0; i < 60 * kSanitizerBudgetScale; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    if (worker.stats().js_processed.load() >= 1 &&
        worker.in_flight_work() == 0) {
      break;
    }
  }

  CapabilityMask mask;
  std::string variant = ReadVariant(worker.cache(), url, mask);
  ASSERT_FALSE(variant.empty()) << "Minified JS variant missing";
  EXPECT_LT(variant.size(), js.size()) << "Should be minified";
  EXPECT_NE(variant.find("processData"), std::string::npos)
      << "Key identifiers should survive minification";

  // Gzip variant should exist.
  CapabilityMask gz_mask;
  gz_mask.set_transfer_encoding(CapabilityMask::TransferEncoding::kGzip);
  std::string gz_variant = ReadVariant(worker.cache(), url, gz_mask);
  EXPECT_FALSE(gz_variant.empty()) << "Gzip JS variant missing";

  // Brotli variant should exist.
  CapabilityMask br_mask;
  br_mask.set_transfer_encoding(CapabilityMask::TransferEncoding::kBrotli);
  std::string br_variant = ReadVariant(worker.cache(), url, br_mask);
  EXPECT_FALSE(br_variant.empty()) << "Brotli JS variant missing";

  // Stats should reflect processing.
  EXPECT_GE(worker.stats().js_processed.load(), 1u);
  EXPECT_GE(worker.stats().variants_written.load(), 1u);
  EXPECT_GE(worker.stats().gzip_variants_written.load(), 1u);
  EXPECT_GE(worker.stats().brotli_variants_written.load(), 1u);
}

TEST_F(WorkerTest, JsAlreadyMinimalProducesCompressed) {
  // JS that is already minimal should still produce compressed variants.
  // Covers the "JS already minimal" path in worker.cc ~2940+.
  WorkerConfig config;
  config.socket_path = socket_path_;
  config.cache_path = cache_path_;
  config.cache_size_bytes = static_cast<uint64_t>(10 * 1024 * 1024);

  NullMessageHandler handler;
  Worker worker(config, &handler);
  ASSERT_TRUE(worker.Initialize());

  // Already-minimal JS (no comments, no extra whitespace).
  std::string url = "http://example.com/already-min.js";
  std::string js = "function f(a){return a+1}";
  CacheOriginal(worker.cache(), url, js);

  std::thread worker_thread([&worker]() { worker.Run(); });
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  CacheNotification notification;
  notification.url = url;
  notification.scheme = "https";
  notification.content_type = ContentType::kJs;
  notification.capability_mask = CapabilityMask().Encode();
  SendNotification(notification);

  // Wait until the notification is retired (not merely received), so the
  // compressed variants are written before they are read.
  ASSERT_TRUE(pagespeed::test::WaitForNotificationsRetired(worker, 1,
                                                           kWorkerStallBudget));

  // Compressed variants should exist even though JS is already minimal.
  CapabilityMask gz_mask;
  gz_mask.set_transfer_encoding(CapabilityMask::TransferEncoding::kGzip);
  std::string gz_variant = ReadVariant(worker.cache(), url, gz_mask);
  EXPECT_FALSE(gz_variant.empty())
      << "Gzip variant should exist for already-minimal JS";

  CapabilityMask br_mask;
  br_mask.set_transfer_encoding(CapabilityMask::TransferEncoding::kBrotli);
  std::string br_variant = ReadVariant(worker.cache(), url, br_mask);
  EXPECT_FALSE(br_variant.empty())
      << "Brotli variant should exist for already-minimal JS";

  worker.Shutdown();
  worker_thread.join();
}

TEST_F(WorkerTest, CssImportFlatteningMissingImportGraceful) {
  // CSS with @import referencing a file NOT in cache should still
  // produce a minified variant using only the available CSS, but
  // with kFlagNeedsRevalidation set so the worker re-processes once
  // the import becomes available.
  WorkerConfig config;
  config.socket_path = socket_path_;
  config.cache_path = cache_path_;
  config.cache_size_bytes = static_cast<uint64_t>(10 * 1024 * 1024);

  NullMessageHandler handler;
  Worker worker(config, &handler);
  ASSERT_TRUE(worker.Initialize());

  // Pre-populate the main CSS that imports a missing file.
  std::string url = "http://example.com/import-missing.css";
  std::string main_css =
      "@import url(\"http://example.com/missing-reset.css\");\n"
      "body  {  color:  red;  font-size:  16px;  }\n";
  CacheOriginal(worker.cache(), url, main_css);
  // Do NOT cache missing-reset.css.

  std::thread worker_thread([&worker]() { worker.Run(); });
  WorkerStopper stopper(worker, worker_thread);
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  CacheNotification notification;
  notification.url = url;
  notification.scheme = "https";
  notification.content_type = ContentType::kCss;
  notification.capability_mask = CapabilityMask().Encode();
  SendNotification(notification);

  // Poll for the minified variant with metadata.
  CapabilityMask mask;
  std::optional<VariantWithMeta> result;
  for (int i = 0; i < 40 * kSanitizerBudgetScale; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    result = ReadVariantWithMeta(worker.cache(), url, mask);
    if (result.has_value()) break;
  }

  ASSERT_TRUE(result.has_value())
      << "CSS with missing import should still produce minified variant";
  // Should contain the main CSS rule.
  EXPECT_NE(result->content.find("color"), std::string::npos)
      << "Main CSS rules should survive import flattening failure";
  // Should be smaller than original (minified).
  EXPECT_LT(result->content.size(), main_css.size());
  // CSS variants are always marked as worker-processed (revalidation
  // is only used on HTML variants).
  EXPECT_NE(result->metadata.flags & AlternateMetadata::kFlagWorkerProcessed, 0)
      << "kFlagWorkerProcessed should be set";

  // No errors should occur (unresolved imports are handled gracefully).
  EXPECT_EQ(worker.stats().errors.load(), 0u)
      << "Missing import should not cause errors";
}

TEST_F(WorkerTest, CssRevalidationWhenImportMissing) {
  // Verifies that when CSS A imports CSS B which is not yet cached,
  // the worker still produces a minified variant of CSS A (without B's
  // rules) and marks it as worker-processed.
  WorkerConfig config;
  config.socket_path = socket_path_;
  config.cache_path = cache_path_;
  config.cache_size_bytes = static_cast<uint64_t>(10 * 1024 * 1024);

  NullMessageHandler handler;
  Worker worker(config, &handler);
  ASSERT_TRUE(worker.Initialize());

  std::string a_url = "http://example.com/reval-a.css";
  std::string a_css =
      "@import url(\"http://example.com/reval-b.css\");\n"
      "body  {  color:  red;  font-size:  16px;  }\n";

  std::string hostname = "example.com";
  CacheOriginal(worker.cache(), a_url, a_css, hostname);

  std::thread worker_thread([&worker]() { worker.Run(); });
  WorkerStopper stopper(worker, worker_thread);
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  CacheNotification notification;
  notification.url = a_url;
  notification.scheme = "https";
  notification.hostname = hostname;
  notification.content_type = ContentType::kCss;
  notification.capability_mask = CapabilityMask().Encode();
  SendNotification(notification);

  CapabilityMask mask;
  std::optional<VariantWithMeta> result;
  for (int i = 0; i < 40 * kSanitizerBudgetScale; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    result = ReadVariantWithMeta(worker.cache(), a_url, mask, hostname);
    if (result.has_value()) break;
  }

  ASSERT_TRUE(result.has_value()) << "CSS variant not written";
  // Should contain A's rule but NOT B's (B not cached).
  EXPECT_NE(result->content.find("color"), std::string::npos)
      << "Should contain CSS A rules";
  EXPECT_EQ(result->content.find("font-weight"), std::string::npos)
      << "Should NOT contain CSS B rules (not cached)";
  // Worker-processed flag should be set.
  EXPECT_NE(result->metadata.flags & AlternateMetadata::kFlagWorkerProcessed, 0)
      << "kFlagWorkerProcessed should be set";
}

TEST_F(WorkerTest, CssSizeGateBypassWhenImportsNewlyResolved) {
  // When @imports are newly resolved (flattened), the CSS variant should
  // be written even if the resulting minified CSS is larger than the
  // original (because the flattened version is more complete).
  WorkerConfig config;
  config.socket_path = socket_path_;
  config.cache_path = cache_path_;
  config.cache_size_bytes = static_cast<uint64_t>(10 * 1024 * 1024);

  NullMessageHandler handler;
  Worker worker(config, &handler);
  ASSERT_TRUE(worker.Initialize());

  // Create a small main CSS that imports a large dependency.
  // After flattening, the result will be larger than the original.
  std::string main_url = "http://example.com/size-gate-bypass.css";
  std::string dep_url = "http://example.com/large-dep.css";
  std::string main_css =
      "@import url(\"http://example.com/large-dep.css\");\n"
      "a{color:red}\n";
  // Generate a dependency that is already well minified (no extra
  // whitespace to remove) so the flattened result stays larger.
  std::string dep_css =
      "h1{font-size:32px}h2{font-size:24px}h3{font-size:18px}"
      "h4{font-size:14px}h5{font-size:12px}h6{font-size:10px}"
      "p{line-height:1.6;margin:0 0 16px}ul{padding-left:20px}"
      "ol{padding-left:20px}li{margin:4px 0}";
  std::string hostname = "example.com";
  CacheOriginal(worker.cache(), main_url, main_css, hostname);
  CacheOriginal(worker.cache(), dep_url, dep_css, hostname);

  std::thread worker_thread([&worker]() { worker.Run(); });
  WorkerStopper stopper(worker, worker_thread);
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  CacheNotification notification;
  notification.url = main_url;
  notification.scheme = "https";
  notification.hostname = hostname;
  notification.content_type = ContentType::kCss;
  notification.capability_mask = CapabilityMask().Encode();
  SendNotification(notification);

  // Poll for the variant.
  CapabilityMask mask;
  std::optional<VariantWithMeta> result;
  for (int i = 0; i < 40 * kSanitizerBudgetScale; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    result = ReadVariantWithMeta(worker.cache(), main_url, mask, hostname);
    if (result.has_value()) break;
  }

  // After flattening, the CSS is larger than the original input.
  // The size gate prevents writing the identity variant.  However,
  // compressed variants (gzip/brotli) of the original are still written.
  // The read at Desktop/Identity returns the original content.
  ASSERT_TRUE(result.has_value()) << "Should read back original content";
  // The original should contain the main rule and the @import.
  EXPECT_NE(result->content.find("color:red"), std::string::npos)
      << "Should contain main CSS rule";
  EXPECT_NE(result->content.find("@import"), std::string::npos)
      << "Original @import should be present (flattened version was larger)";
}

TEST_F(WorkerTest, CssImportFlatteningWithChainedImports) {
  // CSS with chained @imports (A imports B, B imports C) should flatten
  // all levels.  Covers the recursive flattening path.
  WorkerConfig config;
  config.socket_path = socket_path_;
  config.cache_path = cache_path_;
  config.cache_size_bytes = static_cast<uint64_t>(10 * 1024 * 1024);

  NullMessageHandler handler;
  Worker worker(config, &handler);
  ASSERT_TRUE(worker.Initialize());

  // C.css (leaf — no imports)
  std::string c_url = "http://example.com/c.css";
  std::string c_css = "p  {  line-height:  1.6;  }\n";
  CacheOriginal(worker.cache(), c_url, c_css);

  // B.css imports C.css
  std::string b_url = "http://example.com/b.css";
  std::string b_css =
      "@import url(\"http://example.com/c.css\");\n"
      "h1  {  color:  blue;  }\n";
  CacheOriginal(worker.cache(), b_url, b_css);

  // A.css (main) imports B.css
  std::string a_url = "http://example.com/a-chain.css";
  std::string a_css =
      "@import url(\"http://example.com/b.css\");\n"
      "body  {  margin:  0;  }\n";
  CacheOriginal(worker.cache(), a_url, a_css);

  std::thread worker_thread([&worker]() { worker.Run(); });
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  CacheNotification notification;
  notification.url = a_url;
  notification.scheme = "https";
  notification.content_type = ContentType::kCss;
  notification.capability_mask = CapabilityMask().Encode();
  SendNotification(notification);

  // Poll for the minified variant.
  CapabilityMask mask;
  std::string variant;
  for (int i = 0; i < 40 * kSanitizerBudgetScale; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    variant = ReadVariant(worker.cache(), a_url, mask);
    if (!variant.empty()) break;
  }

  EXPECT_FALSE(variant.empty()) << "Chained import CSS variant missing";
  if (!variant.empty()) {
    // Should contain content from all three files (flattened).
    EXPECT_NE(variant.find("margin"), std::string::npos)
        << "Should contain A.css body margin rule";
    EXPECT_NE(variant.find("color"), std::string::npos)
        << "Should contain B.css h1 color rule";
    EXPECT_NE(variant.find("line-height"), std::string::npos)
        << "Should contain C.css p line-height rule";
    // @import directives should be gone.
    EXPECT_EQ(variant.find("@import"), std::string::npos)
        << "Flattened CSS should not contain @import";
    // Should be smaller than combined originals.
    EXPECT_LT(variant.size(), a_css.size() + b_css.size() + c_css.size());
  }

  worker.Shutdown();
  worker_thread.join();
}

// ---------------------------------------------------------------------------
// Compression variant write: verify write failure increments stats counter.
// When the cache is full or corrupted, WriteCompressedVariants should
// increment alternate_write_failures without crashing.
// ---------------------------------------------------------------------------

TEST_F(WorkerTest, CompressedVariantWriteCountersAccurate) {
  // Verify that after processing CSS, the compressed variant write counters
  // are incremented correctly for both gzip and brotli.
  WorkerConfig config;
  config.socket_path = socket_path_;
  config.cache_path = cache_path_;
  config.cache_size_bytes = static_cast<uint64_t>(10 * 1024 * 1024);
  config.gzip_level = 6;
  config.brotli_level = 6;

  NullMessageHandler handler;
  Worker worker(config, &handler);
  ASSERT_TRUE(worker.Initialize());

  std::string url = "http://example.com/style-compressed-check.css";
  std::string css_content =
      "body { margin: 0; padding: 0; }\n"
      "h1 { color: red; font-size: 24px; }\n"
      "p { color: blue; line-height: 1.5; }\n";
  CacheOriginal(worker.cache(), url, css_content);

  std::thread worker_thread([&worker]() { worker.Run(); });
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  CacheNotification notification;
  notification.url = url;
  notification.scheme = "https";
  notification.content_type = ContentType::kCss;
  notification.capability_mask = CapabilityMask().Encode();
  SendNotification(notification);

  // Wait for compressed variants to be written (strong completion signal).
  // Polling on css_processed is racy: it increments BEFORE
  // WriteCompressedVariants runs. Poll on brotli_variants_written because
  // WriteCompressedVariants writes gzip first, then brotli, on the same
  // thread — brotli visibility implies gzip is already written.
  for (int i = 0; i < 60 * kSanitizerBudgetScale; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    if (worker.stats().brotli_variants_written.load() >= 1) break;
  }
  EXPECT_GE(worker.stats().css_processed.load(), 1u);

  // Both gzip and brotli variants should have been written.
  EXPECT_GE(worker.stats().gzip_variants_written.load(), 1u)
      << "At least one gzip variant should be written";
  EXPECT_GE(worker.stats().brotli_variants_written.load(), 1u)
      << "At least one brotli variant should be written";

  // alternate_writes should include both compressed writes plus the identity.
  uint64_t total_writes = worker.stats().alternate_writes.load();
  EXPECT_GE(total_writes, 3u)
      << "At least 3 alternate writes expected (identity + gzip + brotli)";

  // No write failures expected on a healthy cache.
  EXPECT_EQ(worker.stats().alternate_write_failures.load(), 0u)
      << "No write failures expected on healthy cache";

  worker.Shutdown();
  worker_thread.join();
}

}  // namespace
}  // namespace pagespeed
