// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

// Worker IPC protocol and notification-handling tests
//
// - IPC protocol serialization/deserialization and the config extractor check
// - Notification validation: version skew, option contexts, per-URL policy,
//   size limits, dedup and content-type handling
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
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <optional>
#include <span>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "absl/strings/str_cat.h"
#include "gtest/gtest.h"
#include "lib/base/message_handler.h"
#include "lib/cache/freshness.h"
#include "lib/classify/alternate_id.h"
#include "lib/classify/alternate_metadata.h"
#include "lib/classify/capability_mask.h"
#include "lib/classify/option_context.h"
#include "src/proto/worker_ipc.h"
#include "src/worker/browser_analysis_manager.h"
#include "src/worker/browser_analysis_manager_internal.h"
#include "src/worker/critical_css_extractor.h"
#include "src/worker/known_sites.h"
#include "src/worker/url_registry.h"
#include "src/worker/worker.h"
#include "test/test_util/pipe_client.h"
#include "test/test_util/tcp_client.h"
#include "test/test_util/temp_dir.h"
#include "test/test_util/worker_quiescence.h"

namespace pagespeed {
namespace {

// =============================================================================
// IPC Protocol Tests
// =============================================================================

TEST(WorkerIpcTest, NotificationSerialization) {
  CacheNotification notification;
  notification.url = "http://example.com/page.html";
  notification.scheme = "https";
  notification.content_type = ContentType::kHtml;
  notification.capability_mask = 0x12345678;

  std::vector<char> data = notification.Serialize();
  EXPECT_FALSE(data.empty());

  CacheNotification parsed;
  EXPECT_TRUE(CacheNotification::Deserialize(
      std::string_view(data.data(), data.size()), &parsed));
  EXPECT_EQ(parsed.url, notification.url);
  EXPECT_EQ(parsed.content_type, notification.content_type);
  EXPECT_EQ(parsed.capability_mask, notification.capability_mask);
}

TEST(WorkerIpcTest, NotificationWithEmptyUrl) {
  CacheNotification notification;
  notification.url = "";
  notification.scheme = "https";
  notification.content_type = ContentType::kOther;
  notification.capability_mask = 0;

  std::vector<char> data = notification.Serialize();
  EXPECT_FALSE(data.empty());

  CacheNotification parsed;
  EXPECT_TRUE(CacheNotification::Deserialize(
      std::string_view(data.data(), data.size()), &parsed));
  EXPECT_EQ(parsed.url, "");
  EXPECT_EQ(parsed.capability_mask, 0u);
}

TEST(WorkerIpcTest, NotificationWithLongUrl) {
  CacheNotification notification;
  notification.url = std::string(4096, 'x');
  notification.scheme = "https";
  notification.content_type = ContentType::kImage;
  notification.capability_mask = 0xFFFFFFFF;

  std::vector<char> data = notification.Serialize();

  CacheNotification parsed;
  EXPECT_TRUE(CacheNotification::Deserialize(
      std::string_view(data.data(), data.size()), &parsed));
  EXPECT_EQ(parsed.url, notification.url);
  EXPECT_EQ(parsed.content_type, ContentType::kImage);
  EXPECT_EQ(parsed.capability_mask, 0xFFFFFFFFu);
}

TEST(WorkerIpcTest, DeserializeTruncatedData) {
  CacheNotification parsed;
  // Too short to contain length header
  EXPECT_FALSE(CacheNotification::Deserialize("ab", &parsed));
  // Length says more data than available
  EXPECT_FALSE(
      CacheNotification::Deserialize(std::string_view("\x00\x00\x00\xFF"
                                                      "abcd",
                                                      8),
                                     &parsed));
}

// Verify that embedded null bytes in the URL field do not cause a crash
// or data corruption during serialization and deserialization.
TEST(WorkerIpcTest, MalformedIpcNullByteInUrl) {
  // Build a URL with embedded null bytes.
  std::string url_with_nulls = "http://example.com/";
  url_with_nulls.push_back('\0');
  url_with_nulls += "page.html";

  CacheNotification notification;
  notification.url = url_with_nulls;
  notification.scheme = "https";
  notification.hostname = "example.com";
  notification.content_type = ContentType::kHtml;
  notification.capability_mask = 0x08;

  // Roundtrip through Serialize/Deserialize.
  std::vector<char> data = notification.Serialize();
  EXPECT_FALSE(data.empty());

  CacheNotification parsed;
  EXPECT_TRUE(CacheNotification::Deserialize(
      std::string_view(data.data(), data.size()), &parsed));
  // The URL must survive the roundtrip byte-for-byte, including the null.
  EXPECT_EQ(parsed.url.size(), url_with_nulls.size());
  EXPECT_EQ(parsed.url, url_with_nulls);
  EXPECT_EQ(parsed.hostname, "example.com");
  EXPECT_EQ(parsed.content_type, ContentType::kHtml);
  EXPECT_EQ(parsed.capability_mask, 0x08u);
}

// Verify that out-of-range capability mask values and invalid content-type
// bytes are handled gracefully (no crash, correct accept/reject).
TEST(WorkerIpcTest, MalformedIpcInvalidMask) {
  // An all-bits-set mask (0xFFFFFFFF) has no valid encoding but must not
  // crash.  Deserialize accepts any uint32_t mask value.
  {
    CacheNotification notification;
    notification.url = "http://example.com/test";
    notification.scheme = "https";
    notification.hostname = "example.com";
    notification.content_type = ContentType::kImage;
    notification.capability_mask = 0xFFFFFFFF;

    std::vector<char> data = notification.Serialize();
    CacheNotification parsed;
    EXPECT_TRUE(CacheNotification::Deserialize(
        std::string_view(data.data(), data.size()), &parsed));
    EXPECT_EQ(parsed.capability_mask, 0xFFFFFFFFu);
  }

  // Hand-craft a message with an invalid content_type byte (0xFF, which
  // is well above ContentType::kOther = 4).  Deserialize must reject it.
  {
    CacheNotification valid;
    valid.url = "http://x.com/a";
    valid.scheme = "https";
    valid.hostname = "";
    valid.content_type = ContentType::kOther;
    valid.capability_mask = 0;

    std::vector<char> data = valid.Serialize();
    ASSERT_GT(data.size(), 4u);

    // The content_type byte sits right after the hostname field.
    // Wire format (v3): [4B total_len] [1B version] [4B url_len] [url] [4B host_len] [host] [1B ct] [4B mask] [1B scheme]
    // Find the ct byte: offset = 4 + 1 + 4 + url_len + 4 + host_len
    size_t ct_offset = 4 + 1 + 4 + valid.url.size() + 4 + valid.hostname.size();
    ASSERT_LT(ct_offset, data.size());
    data[ct_offset] = static_cast<char>(0xFF);  // Invalid content type

    CacheNotification parsed;
    EXPECT_FALSE(CacheNotification::Deserialize(
        std::string_view(data.data(), data.size()), &parsed))
        << "Deserialize should reject content_type byte 0xFF";
  }
}

// Verify that non-UTF-8 byte sequences in the URL do not cause a crash.
// The IPC protocol is encoding-agnostic (length-prefixed binary), so
// arbitrary bytes should survive the roundtrip.
TEST(WorkerIpcTest, MalformedIpcNonUtf8Url) {
  // Construct a URL containing invalid UTF-8 sequences:
  // 0xFE and 0xFF are never valid in UTF-8; 0xC0 0x01 is an overlong
  // encoding; 0x80 is a continuation byte without a leading byte.
  std::string bad_url = "http://example.com/";
  bad_url += std::string("\xFE\xFF\xC0\x01\x80\xED\xBF\xBF", 8);
  bad_url += "/end";

  CacheNotification notification;
  notification.url = bad_url;
  notification.scheme = "https";
  notification.hostname = std::string("\xC0\xAF", 2);  // Overlong slash
  notification.content_type = ContentType::kCss;
  notification.capability_mask = 0xDEADBEEF;

  std::vector<char> data = notification.Serialize();
  EXPECT_FALSE(data.empty());

  CacheNotification parsed;
  EXPECT_TRUE(CacheNotification::Deserialize(
      std::string_view(data.data(), data.size()), &parsed));
  EXPECT_EQ(parsed.url.size(), bad_url.size());
  EXPECT_EQ(parsed.url, bad_url);
  EXPECT_EQ(parsed.hostname, notification.hostname);
  EXPECT_EQ(parsed.content_type, ContentType::kCss);
  EXPECT_EQ(parsed.capability_mask, 0xDEADBEEFu);
}

// =============================================================================
// Per-URL Policy and Security Hardening Tests
// =============================================================================

TEST_F(WorkerTest, DisableHtmlSkipsProcessing) {
  WorkerConfig config;
  config.socket_path = socket_path_;
  config.cache_path = cache_path_;
  config.cache_size_bytes = static_cast<uint64_t>(10 * 1024 * 1024);
  config.html_mode = HtmlMode::kOff;  // the old disable_html=true

  Worker worker(config, nullptr);
  ASSERT_TRUE(worker.Initialize());

  // Pre-populate cache with HTML
  std::string url = "http://example.com/disabled.html";
  std::string html =
      "<html><head><style>body { margin: 0; }</style></head>"
      "<body><h1>Test</h1></body></html>";
  CacheOriginal(worker.cache(), url, html);

  std::thread worker_thread([&worker]() { worker.Run(); });
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  CacheNotification notification;
  notification.url = url;
  notification.scheme = "https";
  notification.content_type = ContentType::kHtml;
  notification.capability_mask = 0x00000001;
  SendNotification(notification);

  std::this_thread::sleep_for(std::chrono::milliseconds(300));

  // No variant should be written
  CapabilityMask mask = CapabilityMask::Decode(0x00000001);
  std::string variant = ReadVariant(worker.cache(), url, mask);
  EXPECT_TRUE(variant.empty()) << "HTML variant written despite html_mode off";

  worker.Shutdown();
  worker_thread.join();
}

TEST_F(WorkerTest, DisableCssSkipsProcessing) {
  WorkerConfig config;
  config.socket_path = socket_path_;
  config.cache_path = cache_path_;
  config.cache_size_bytes = static_cast<uint64_t>(10 * 1024 * 1024);
  config.disable_css = true;

  Worker worker(config, nullptr);
  ASSERT_TRUE(worker.Initialize());

  std::string url = "http://example.com/disabled.css";
  std::string css = "body  {  margin:  0;  padding:  0;  }\n";
  CacheOriginal(worker.cache(), url, css);

  std::thread worker_thread([&worker]() { worker.Run(); });
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  CacheNotification notification;
  notification.url = url;
  notification.scheme = "https";
  notification.content_type = ContentType::kCss;
  notification.capability_mask = 0x00000001;
  SendNotification(notification);

  std::this_thread::sleep_for(std::chrono::milliseconds(300));

  CapabilityMask mask = CapabilityMask::Decode(0x00000001);
  std::string variant = ReadVariant(worker.cache(), url, mask);
  EXPECT_TRUE(variant.empty()) << "CSS variant written despite disable_css";

  worker.Shutdown();
  worker_thread.join();
}

TEST_F(WorkerTest, SentinelViewportMaskRejected) {
  // Notifications with viewport bits = 3 (sentinel range) should be
  // rejected — they would overwrite reserved sentinel AlternateIds.
  WorkerConfig config;
  config.socket_path = socket_path_;
  config.cache_path = cache_path_;
  config.cache_size_bytes = static_cast<uint64_t>(10 * 1024 * 1024);

  NullMessageHandler handler;
  Worker worker(config, &handler);
  ASSERT_TRUE(worker.Initialize());

  // Pre-populate cache with CSS
  std::string url = "http://example.com/sentinel.css";
  std::string css = "body  {  margin:  0;  padding:  0;  }\n";
  CacheOriginal(worker.cache(), url, css);

  std::thread worker_thread([&worker]() { worker.Run(); });
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  // Send notification with viewport=3 in the mask (bits 2-3 = 0b11).
  // This is NOT the warmup sentinel (0xFFFFFFFE), just a mask with
  // the sentinel viewport bits set.
  uint32_t sentinel_mask = 0x0C;  // viewport bits 2-3 = 3
  CacheNotification notification;
  notification.url = url;
  notification.scheme = "https";
  notification.content_type = ContentType::kCss;
  notification.capability_mask = sentinel_mask;
  SendNotification(notification);

  // Poll for the error counter to advance instead of a fixed sleep (slow
  // shards can sit at the pipe read past a fixed budget).
  for (int i = 0; i < 80 * kSanitizerBudgetScale; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    if (worker.stats().errors.load() >= 1u) break;
  }

  // Should not have processed (error counter should increment)
  EXPECT_GE(worker.stats().errors.load(), 1u);
  // No variant should be written
  CapabilityMask mask = CapabilityMask::Decode(sentinel_mask);
  std::string variant = ReadVariant(worker.cache(), url, mask);
  EXPECT_TRUE(variant.empty()) << "Variant written for sentinel viewport mask";

  worker.Shutdown();
  worker_thread.join();
}

// A notification stamped with a wire version this build does not speak is
// REFUSED WHOLE — not parsed, not partially applied — and the refusal is
// counted and logged as version skew specifically.
//
// This is the receive path, not the codec: worker_ipc_test pins that
// Deserialize says "version mismatch", and this pins that the socket path
// acts on it.  Both directions of skew are driven, because the interesting
// one is the peer AHEAD of this build — a notify sender shipping on its own
// cadence — and that is precisely the frame a length-tolerant reader would be
// happy to decode with the wrong field meanings.
//
// The three assertions are the whole fail-direction contract:
//   1. notifications_received stays 0        (never dispatched)
//   2. rejected_version counts every frame   (visible without reading logs)
//   3. the warning names BOTH versions       (actionable without a debugger)
TEST_F(WorkerTest, NotifyFromSkewedPeerRefusedCountedAndNamed) {
  WorkerConfig config;
  config.socket_path = socket_path_;
  config.cache_path = cache_path_;
  config.cache_size_bytes = static_cast<uint64_t>(10 * 1024 * 1024);

  WarningRecordingHandler handler;
  Worker worker(config, &handler);
  ASSERT_TRUE(worker.Initialize());

  // Real, minifiable content at the key the skewed frames name: if the worker
  // ever did parse one of them, it would find something to optimize at that
  // key, so "no variant written" is evidence and not a tautology.
  const std::string url = "http://example.com/skewed-peer.css";
  const std::string css = "body  {  margin:  0;  }\n";
  CacheOriginal(worker.cache(), url, css);

  std::thread worker_thread([&worker]() { worker.Run(); });
  WorkerStopper stopper(worker, worker_thread);
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  // Build one valid frame, then restamp its version byte.  Everything else
  // about the frame is well-formed, which is what isolates the version as the
  // only reason it is refused.
  CacheNotification notification;
  notification.url = url;
  // NO hostname, deliberately: CacheOriginal above stores under the empty
  // hostname, and a notification naming a different one composes a different
  // cache key -- the worker would find nothing to optimize and "no variant was
  // written" would hold for a reason that has nothing to do with the version
  // gate. Matching the key is what makes the assertion below mean "the
  // refusal stopped it" rather than "there was never anything to do".
  notification.scheme = "https";
  notification.content_type = ContentType::kCss;
  notification.capability_mask = CapabilityMask().Encode();
  const std::vector<char> valid = notification.Serialize();
  ASSERT_FALSE(valid.empty());
  ASSERT_EQ(static_cast<uint8_t>(valid[4]), kIpcVersion);

  // Older peer, newer peer, and a byte that is no version at all.  Distinct
  // versions on purpose: the log is rate-limited per peer version, so this
  // also pins that a second differently-skewed peer is not swallowed by the
  // rate limit the first one armed.
  const uint8_t peers[] = {static_cast<uint8_t>(kIpcVersion - 1),
                           static_cast<uint8_t>(kIpcVersion + 1), 0xFF};
  for (uint8_t peer : peers) {
    SCOPED_TRACE(static_cast<unsigned>(peer));
    const uint64_t rejected_before =
        worker.stats().notifications_rejected_version.load();
    const size_t warnings_before = handler.warnings().size();

    std::vector<char> wire = valid;
    wire[4] = static_cast<char>(peer);
    SendRawNotificationBytes(wire);

    bool counted = false;
    for (int i = 0; i < 80 * kSanitizerBudgetScale; ++i) {
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
      if (worker.stats().notifications_rejected_version.load() >
          rejected_before) {
        counted = true;
        break;
      }
    }
    EXPECT_TRUE(counted)
        << "a version-skewed notification must be counted as version skew, "
           "not lost in a generic parse-failure bucket";

    // The refusal is loud AND specific: an operator must be able to read the
    // peer's version and this build's version straight off the line.
    auto warnings = handler.warnings();
    ASSERT_GT(warnings.size(), warnings_before)
        << "the refusal must be visible in the log, not silent";
    bool named_both = false;
    for (size_t i = warnings_before; i < warnings.size(); ++i) {
      const std::string& w = warnings[i];
      if (w.find("wire version " + std::to_string(peer)) != std::string::npos &&
          w.find("version " + std::to_string(kIpcVersion)) !=
              std::string::npos) {
        named_both = true;
      }
    }
    EXPECT_TRUE(named_both)
        << "the warning must name the peer's version AND this build's";
  }

  // Nothing was ever handed to the pipeline, and nothing was written.
  EXPECT_EQ(worker.stats().notifications_received.load(), 0u)
      << "a refused frame must never reach dispatch";
  EXPECT_EQ(worker.stats().notifications_rejected_version.load(),
            static_cast<uint64_t>(std::size(peers)));
  EXPECT_EQ(worker.stats().notifications_rejected_malformed.load(), 0u)
      << "skew must not be miscounted as corruption";
  EXPECT_EQ(worker.stats().variants_written.load(), 0u)
      << "a refused frame must produce no cache write at all";
  // The pre-seeded original is byte-for-byte what it was: refusing costs cache
  // warmth, it does not touch what is already stored.
  EXPECT_EQ(ReadVariant(worker.cache(), url, CapabilityMask()), css);
}

// A notification whose per-request options context FAILS VALIDATION is REFUSED
// — dropped before any source is read — and the refusal is counted and logged
// as an options-context refusal specifically.
//
// The four refusal statuses do not all reach this gate, and this says which is
// which rather than implying otherwise:
//
//   kUnknownFormat, kMalformedSignature and kSignatureMismatch arrive over the
//   socket and are refused here. The last matters most: it means the two
//   implementations of the context format have drifted, and it must surface on
//   the first message rather than as a cache that never quite warms.
//
//   kTooLarge cannot reach this gate. The frame reader bounds the payload with
//   the same constant the validator does, so an oversized context is refused a
//   layer earlier as a malformed frame, and the sending side refuses to emit
//   one at all. Both halves are asserted below, because "every invalid context
//   is refused before a source read" is only true if that earlier refusal is
//   real.
//
// A VALID context — default or not — is NOT refused: it is accepted and its
// work is filed under the default context. That is the test after this one.
//
// As with the version-skew case above, the URL names real minifiable content,
// so "no variant written" is evidence rather than a tautology.
TEST_F(WorkerTest, NotifyWithAnInvalidOptionContextIsRefusedAndCounted) {
  WorkerConfig config;
  config.socket_path = socket_path_;
  config.cache_path = cache_path_;
  config.cache_size_bytes = static_cast<uint64_t>(10 * 1024 * 1024);

  WarningRecordingHandler handler;
  Worker worker(config, &handler);
  ASSERT_TRUE(worker.Initialize());

  const std::string url = "http://example.com/option-context.css";
  const std::string css = "body  {  margin:  0;  }\n";
  CacheOriginal(worker.cache(), url, css);

  std::thread worker_thread([&worker]() { worker.Run(); });
  WorkerStopper stopper(worker, worker_thread);
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  CacheNotification notification;
  notification.url = url;
  // NO hostname, deliberately: CacheOriginal above stores under the empty
  // hostname, and a notification naming a different one composes a different
  // cache key -- the worker would find nothing to optimize and "no variant was
  // written" would hold for a reason that has nothing to do with the options
  // context. Matching the key is what makes the assertion below mean "the
  // refusal stopped it" rather than "there was never anything to do".
  notification.scheme = "https";
  notification.content_type = ContentType::kCss;
  notification.capability_mask = CapabilityMask().Encode();

  const std::string good_payload = "psoc1\nf:hw\no:ImageInlineMaxBytes=3072\n";

  // Each arm is (payload, signature, expected status).  Every one of them is a
  // context a peer could actually put on the socket.
  struct Arm {
    const char* what;
    std::string payload;
    std::string signature;
    OptionContextStatus status;
  };
  const std::string unknown_format_payload = "psoc2\no:ImageQuality=80\n";
  const std::vector<Arm> arms = {
      {"a payload from a format this build does not read",
       unknown_format_payload, OptionContextSignature(unknown_format_payload),
       OptionContextStatus::kUnknownFormat},
      {"a signature that is 64 characters of something other than lowercase "
       "hex",
       good_payload, std::string(kOptionContextSignatureChars, 'z'),
       OptionContextStatus::kMalformedSignature},
      {"a signature that is of a DIFFERENT payload -- the two implementations "
       "of this format have drifted",
       good_payload,
       OptionContextSignature("psoc1\nf:hw\no:ImageInlineMaxBytes=4096\n"),
       OptionContextStatus::kSignatureMismatch},
  };

  uint64_t expected_refusals = 0;
  for (const Arm& arm : arms) {
    SCOPED_TRACE(arm.what);
    notification.option_context = arm.payload;
    notification.option_signature = arm.signature;
    ASSERT_EQ(arm.status, ValidateOptionContext(notification.option_context,
                                                notification.option_signature))
        << "this arm no longer produces the status it says it does";
    const std::vector<char> wire = notification.Serialize();
    ASSERT_FALSE(wire.empty())
        << "the sending side must be able to emit this frame, or the arm is "
           "testing the sender rather than the gate";
    SendRawNotificationBytes(wire);

    ++expected_refusals;
    bool counted = false;
    for (int i = 0; i < 80 * kSanitizerBudgetScale; ++i) {
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
      if (worker.stats().notifications_rejected_option_context.load() >=
          expected_refusals) {
        counted = true;
        break;
      }
    }
    EXPECT_TRUE(counted)
        << "an options context that fails validation must be refused and "
           "counted";
  }
  EXPECT_EQ(worker.stats().notifications_rejected_option_context.load(),
            expected_refusals);

  // kTooLarge, the fourth status: it never reaches the gate, and this pins the
  // two refusals that stand in front of it.  Without this the claim "an
  // oversized context is refused before a source read" would rest on nothing.
  {
    const std::string oversized =
        "psoc1\n" + std::string(kMaxOptionContextBytes, 'x');
    EXPECT_EQ(OptionContextStatus::kTooLarge,
              ValidateOptionContext(oversized, OptionContextSignature("")))
        << "the validator must still name an oversized payload for what it is";
    CacheNotification too_big = notification;
    too_big.option_context = oversized;
    too_big.option_signature = OptionContextSignature(oversized);
    EXPECT_TRUE(too_big.Serialize().empty())
        << "the sending side must refuse to emit an oversized context rather "
           "than put one on the socket";
  }

  // The refusal is loud, and it says it was the context that was refused.
  bool named_refusal = false;
  for (const std::string& w : handler.warnings()) {
    if (w.find("options context was refused") != std::string::npos) {
      named_refusal = true;
    }
  }
  EXPECT_TRUE(named_refusal)
      << "a context that failed validation must be reported as such";
  // The retired refusal rationale must not come back: this line described
  // version skew, and none of these arms is version skew.
  for (const std::string& w : handler.warnings()) {
    EXPECT_EQ(w.find("Align the versions of the components sending"),
              std::string::npos)
        << "an options-context refusal is not version skew: " << w;
  }

  // No frame here was version-skewed or corrupt, and none was accepted: those
  // buckets stay clean, so the refusal counter is not soaking up other causes
  // and the accept path is not being credited for a refusal.
  EXPECT_EQ(worker.stats().notifications_rejected_version.load(), 0u);
  EXPECT_EQ(worker.stats().notifications_rejected_malformed.load(), 0u);
  EXPECT_EQ(
      worker.stats().notifications_accepted_non_default_option_context.load(),
      0u)
      << "a refused context must never be counted as an accepted one";

  // Nothing reached the pipeline and nothing was written. The pre-seeded
  // original is byte-for-byte what it was.
  EXPECT_EQ(worker.stats().variants_written.load(), 0u)
      << "a refused context must produce no cache write at all";
  EXPECT_EQ(worker.stats().css_processed.load(), 0u)
      << "a refused context must be dropped before any source is read";
  EXPECT_EQ(ReadVariant(worker.cache(), url, CapabilityMask()), css);
}

// A notification carrying a VALID NON-DEFAULT per-request options context is
// ACCEPTED, optimized end-to-end, and its work stored under the DEFAULT cache
// key — the same key a notification carrying no context at all would use.
//
// That is the whole shape of the contract on this surface. Optimized output
// here is a function of the resource, of the request (capability mask and
// content type, plus the agent-request bit — the request's own declared intent,
// gated by an operator flag the daemon itself publishes), and of the daemon's own
// configuration, which a notification cannot reach; no value a sender resolves
// per request reaches a rewriter. So two contexts cannot produce two different
// artifacts, one stored
// artifact is correct for both, and refusing the notification would cost an
// optimization for nothing.
//
// The read-back is deliberately through the ORDINARY default-key path
// (ReadVariant, which composes the key with no context at all): if the work had
// been filed anywhere else, this read would come back with the unoptimized
// original.
TEST_F(WorkerTest, NotifyWithValidNonDefaultOptionContextIsStoredUnderDefault) {
  WorkerConfig config;
  config.socket_path = socket_path_;
  config.cache_path = cache_path_;
  config.cache_size_bytes = static_cast<uint64_t>(10 * 1024 * 1024);

  WarningRecordingHandler handler;
  Worker worker(config, &handler);
  ASSERT_TRUE(worker.Initialize());

  const std::string url = "http://example.com/accepted-context.css";
  const std::string css = "body  {  margin:  0;  }\n";
  CacheOriginal(worker.cache(), url, css);

  std::thread worker_thread([&worker]() { worker.Run(); });
  WorkerStopper stopper(worker, worker_thread);
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  CacheNotification notification;
  notification.url = url;
  // No hostname, matching CacheOriginal's key -- see the refusal test above.
  notification.scheme = "https";
  notification.content_type = ContentType::kCss;
  notification.capability_mask = CapabilityMask().Encode();

  const std::string payload = "psoc1\nf:hw\no:ImageInlineMaxBytes=3072\n";
  notification.option_context = payload;
  notification.option_signature = OptionContextSignature(payload);
  ASSERT_FALSE(IsDefaultOptionContext(notification.option_signature))
      << "this test is about a context that is NOT the default one";
  ASSERT_EQ(OptionContextStatus::kOk,
            ValidateOptionContext(notification.option_context,
                                  notification.option_signature));
  {
    const std::vector<char> wire = notification.Serialize();
    ASSERT_FALSE(wire.empty());
    SendRawNotificationBytes(wire);
  }

  // Wait for the optimizer to have actually run and the work item to have
  // drained -- css_processed moves before the in-flight entry is erased, so
  // both conditions are needed (the pre-existing CSS tests use this pairing).
  for (int i = 0; i < 80 * kSanitizerBudgetScale; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    if (worker.stats().css_processed.load() >= 1 &&
        worker.in_flight_work() == 0) {
      break;
    }
  }
  EXPECT_GE(worker.stats().css_processed.load(), 1u)
      << "a valid non-default options context must reach the optimizer, not be "
         "dropped before the source is read";

  // POSITIVE proof, and under the default key specifically.
  const std::string optimized =
      ReadVariant(worker.cache(), url, CapabilityMask());
  ASSERT_FALSE(optimized.empty())
      << "the work must be readable through the ordinary default-context key";
  EXPECT_LT(optimized.size(), css.size())
      << "the variant should be minified, which is what proves the optimizer "
         "ran rather than that something was merely stored";

  // The counters split the two outcomes cleanly: accepted-non-default moves,
  // the refusal counter does not.
  EXPECT_EQ(
      worker.stats().notifications_accepted_non_default_option_context.load(),
      1u)
      << "an accepted non-default context must be counted, or a deployment "
         "cannot see that this path is flowing";
  EXPECT_EQ(worker.stats().notifications_rejected_option_context.load(), 0u)
      << "accepting must not also count as a refusal";
  EXPECT_EQ(worker.stats().notifications_rejected_version.load(), 0u);
  EXPECT_EQ(worker.stats().notifications_rejected_malformed.load(), 0u);

  // Said once, and said accurately: the operator-facing half of the new path.
  bool named_accept = false;
  for (const std::string& info : handler.infos()) {
    if (info.find("non-default per-request options context") !=
            std::string::npos &&
        info.find("stored under the default context") != std::string::npos) {
      named_accept = true;
    }
  }
  EXPECT_TRUE(named_accept)
      << "the first accepted non-default context must say where the work "
         "landed";
  // And it is not a warning: this is normal operation, not a problem.
  for (const std::string& w : handler.warnings()) {
    EXPECT_EQ(w.find("options context"), std::string::npos)
        << "accepting a valid context must not warn: " << w;
  }
}

// A notification that carries an options context starts no fetch from its
// hostname: the /llms.txt build does not run for it.  The control, the same
// request without a context, runs the build, which stamps its freshness
// record even when the fetch fails.
TEST_F(WorkerTest, NotifyWithOptionContextBuildsNoLlmsTxt) {
  WorkerConfig config;
  config.socket_path = socket_path_;
  config.cache_path = cache_path_;
  config.cache_size_bytes = static_cast<uint64_t>(10 * 1024 * 1024);
  config.browser_analysis.agent_optimize = true;
  config.browser_analysis.agent_optimize_llms_txt = true;

  Worker worker(config, nullptr);
  ASSERT_TRUE(worker.Initialize());

  std::thread worker_thread([&worker]() { worker.Run(); });
  WorkerStopper stopper(worker, worker_thread);
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  // Closed loopback ports: any fetch fails at once.
  const std::string with_context_host = "127.0.0.1:1";
  const std::string control_host = "127.0.0.2:1";
  // Both vouched, so the options context is the only difference.
  auto llms_notification = [](const std::string& host) {
    CacheNotification n;
    n.url = "/llms.txt";
    n.hostname = host;
    n.scheme = "http";
    n.content_type = ContentType::kHtml;
    n.capability_mask = kLlmsTxtSentinel;
    n.host_vouched = true;
    return n;
  };

  CacheNotification with_context = llms_notification(with_context_host);
  const std::string payload = "psoc1\nf:hw\no:ImageInlineMaxBytes=3072\n";
  with_context.option_context = payload;
  with_context.option_signature = OptionContextSignature(payload);
  ASSERT_EQ(OptionContextStatus::kOk,
            ValidateOptionContext(with_context.option_context,
                                  with_context.option_signature));
  SendNotification(with_context);
  SendNotification(llms_notification(control_host));

  auto has_meta = [&worker](const std::string& host) {
    return worker.cache()
        ->ReadAlternate("/llms.txt", host, "http",
                        static_cast<AlternateId>(SentinelId::kLlmsTxtMeta))
        .has_value();
  };
  for (int i = 0; i < 100 * kSanitizerBudgetScale; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    if (has_meta(control_host) && worker.in_flight_work() == 0) break;
  }
  ASSERT_TRUE(has_meta(control_host))
      << "the control build did not run, so this test proves nothing";
  EXPECT_FALSE(has_meta(with_context_host))
      << "a notification with an options context must not start the build";
}

// The same rule for browser analysis, which the agent render follows: an HTML
// notification with an options context queues no analysis, while the same
// notification without one does.
TEST_F(WorkerTest, NotifyWithOptionContextQueuesNoBrowserAnalysis) {
  WorkerConfig config;
  config.socket_path = socket_path_;
  config.cache_path = cache_path_;
  config.cache_size_bytes = static_cast<uint64_t>(10 * 1024 * 1024);
  // As in the critical-CSS tests: a manager without a live Chrome, sandbox
  // opt-out for the build container.
  config.browser_analysis.sandbox_mode = BrowserSandboxMode::kOff;
  config.browser_analysis.enabled = true;
  config.browser_analysis.chrome_binary = "/nonexistent/chrome";
  config.browser_analysis.agent_optimize = true;

  Worker worker(config, nullptr);
  ASSERT_TRUE(worker.Initialize());
  BrowserAnalysisManager* mgr = worker.TestBrowserManager();
  ASSERT_NE(mgr, nullptr);

  CacheOriginal(worker.cache(), "/with-context.html",
                "<html><head><title>a</title></head><body><p>a</p>"
                "<img src=\"/a1.png\"><img src=\"/a2.png\"></body></html>");
  CacheOriginal(worker.cache(), "/control.html",
                "<html><head><title>b</title></head><body><div><ul><li>b"
                "</li></ul></div><img src=\"/b1.png\"><img src=\"/b2.png\">"
                "</body></html>");

  std::thread worker_thread([&worker]() { worker.Run(); });
  WorkerStopper stopper(worker, worker_thread);
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  auto html_notification = [](const std::string& url) {
    CacheNotification n;
    n.url = url;
    n.scheme = "https";
    n.content_type = ContentType::kHtml;
    n.capability_mask = CapabilityMask().Encode();
    return n;
  };
  auto wait_html = [&worker](uint64_t count) {
    for (int i = 0; i < 80 * kSanitizerBudgetScale; ++i) {
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
      if (worker.stats().html_processed.load() >= count &&
          worker.in_flight_work() == 0) {
        return;
      }
    }
  };

  CacheNotification with_context =
      html_notification("http://example.com/with-context.html");
  const std::string payload = "psoc1\nf:hw\no:ImageInlineMaxBytes=3072\n";
  with_context.option_context = payload;
  with_context.option_signature = OptionContextSignature(payload);
  SendNotification(with_context);
  wait_html(1);
  ASSERT_GE(worker.stats().html_processed.load(), 1u);
  EXPECT_EQ(mgr->stats().queue_enqueued.load(), 0u)
      << "a notification with an options context must not queue analysis";

  SendNotification(html_notification("http://example.com/control.html"));
  wait_html(2);
  ASSERT_GE(worker.stats().html_processed.load(), 2u);
  EXPECT_GE(mgr->stats().queue_enqueued.load(), 1u)
      << "the control was not queued, so this test proves nothing";
}

// Two notifications for the SAME URL carrying DIFFERENT valid contexts produce
// ONE OPTIMIZATION, not two: both are filed under the default context, so the
// second is deduplicated exactly as a repeat of the first would be.
//
// Be precise about what is and is not asserted, because the difference matters
// if this ever has to be re-read:
//
//   * ONE OPTIMIZATION is the claim, and css_processed is the oracle for it.
//     Both notifications are ADMITTED (the accepted counter reads 2) and the
//     second does reach HandleNotification — it is stopped at the
//     already-processed set, which is where "same work, named twice" is
//     supposed to be caught. "One work item" would overstate it.
//   * This is the counterpart of the storage assertion above: it shows the two
//     contexts converging on one stored result rather than on two.
//   * It is NOT a tripwire for context keying being switched on later. The
//     dedup key is composed without a signature parameter, so a change made
//     purely in the cache layer would not be visible here. What would be
//     visible is the gate or the dedup key itself growing a context
//     dimension.
TEST_F(WorkerTest, NotifyWithTwoDifferentValidContextsIsOneOptimization) {
  WorkerConfig config;
  config.socket_path = socket_path_;
  config.cache_path = cache_path_;
  config.cache_size_bytes = static_cast<uint64_t>(10 * 1024 * 1024);

  NullMessageHandler handler;
  Worker worker(config, &handler);
  ASSERT_TRUE(worker.Initialize());

  const std::string url = "http://example.com/two-contexts.css";
  const std::string css = "body  {  margin:  0;  }\n";
  CacheOriginal(worker.cache(), url, css);

  std::thread worker_thread([&worker]() { worker.Run(); });
  WorkerStopper stopper(worker, worker_thread);
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  CacheNotification notification;
  notification.url = url;
  notification.scheme = "https";
  notification.content_type = ContentType::kCss;
  notification.capability_mask = CapabilityMask().Encode();

  const std::string first = "psoc1\nf:hw\no:ImageInlineMaxBytes=3072\n";
  notification.option_context = first;
  notification.option_signature = OptionContextSignature(first);
  ASSERT_FALSE(IsDefaultOptionContext(notification.option_signature));
  {
    const std::vector<char> wire = notification.Serialize();
    ASSERT_FALSE(wire.empty());
    SendRawNotificationBytes(wire);
  }

  // Drain the first completely BEFORE sending the second, so that what the
  // second hits is the processed-set dedup and not the in-flight guard -- the
  // in-flight guard would also fire, but only by a timing accident, and a test
  // that depended on it would be racy.
  for (int i = 0; i < 80 * kSanitizerBudgetScale; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    if (worker.stats().css_processed.load() >= 1 &&
        worker.in_flight_work() == 0) {
      break;
    }
  }
  ASSERT_GE(worker.stats().css_processed.load(), 1u)
      << "the first notification must have been processed for the dedup "
         "assertion below to mean anything";
  const uint64_t processed_after_first = worker.stats().css_processed.load();
  const uint64_t skipped_before =
      worker.stats().notifications_skipped_dedup.load();

  // A DIFFERENT valid non-default context, same URL, same mask.
  const std::string second = "psoc1\nf:hw\no:ImageInlineMaxBytes=9999\n";
  notification.option_context = second;
  notification.option_signature = OptionContextSignature(second);
  ASSERT_NE(OptionContextSignature(first), OptionContextSignature(second))
      << "the two contexts must actually differ";
  ASSERT_FALSE(IsDefaultOptionContext(notification.option_signature));
  {
    const std::vector<char> wire = notification.Serialize();
    ASSERT_FALSE(wire.empty());
    SendRawNotificationBytes(wire);
  }

  bool skipped = false;
  for (int i = 0; i < 80 * kSanitizerBudgetScale; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    if (worker.stats().notifications_skipped_dedup.load() > skipped_before) {
      skipped = true;
      break;
    }
  }
  EXPECT_TRUE(skipped)
      << "a second context for the same URL must be recognised as naming work "
         "already done, because both are filed under the default context";
  EXPECT_EQ(worker.stats().css_processed.load(), processed_after_first)
      << "the same work must not be optimized twice just because it was named "
         "twice";

  // Both were ADMITTED, and both were counted -- the counter tracks
  // notifications carrying a non-default context, not optimizations performed.
  EXPECT_EQ(
      worker.stats().notifications_accepted_non_default_option_context.load(),
      2u);
  EXPECT_EQ(worker.stats().notifications_rejected_option_context.load(), 0u);
}

// The control for the cases above: a notification with NO options context, and
// one carrying the DEFAULT context explicitly, are both processed normally —
// and, being the default, neither is counted as a non-default acceptance.
// Without this, the counter assertions above would also pass if the gate had
// simply stopped discriminating.
TEST_F(WorkerTest, NotifyWithNoOrDefaultOptionContextIsProcessedNormally) {
  WorkerConfig config;
  config.socket_path = socket_path_;
  config.cache_path = cache_path_;
  config.cache_size_bytes = static_cast<uint64_t>(10 * 1024 * 1024);

  NullMessageHandler handler;
  Worker worker(config, &handler);
  ASSERT_TRUE(worker.Initialize());

  const std::string url = "http://example.com/default-context.css";
  const std::string css = "body  {  margin:  0;  }\n";
  CacheOriginal(worker.cache(), url, css);

  std::thread worker_thread([&worker]() { worker.Run(); });
  WorkerStopper stopper(worker, worker_thread);
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  CacheNotification notification;
  notification.url = url;
  // No hostname, matching CacheOriginal's key (see the declined-context test
  // above) and matching the pre-existing CSS tests. The two tests are otherwise
  // identical, so the options context is the only variable between them.
  notification.scheme = "https";
  notification.content_type = ContentType::kCss;
  notification.capability_mask = CapabilityMask().Encode();

  // No context at all.
  {
    const std::vector<char> wire = notification.Serialize();
    ASSERT_FALSE(wire.empty());
    SendRawNotificationBytes(wire);
  }
  bool dispatched = false;
  for (int i = 0; i < 80 * kSanitizerBudgetScale; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    if (worker.stats().notifications_received.load() > 0) {
      dispatched = true;
      break;
    }
  }
  EXPECT_TRUE(dispatched) << "a notification with no options context must be "
                             "dispatched exactly as before";
  EXPECT_EQ(worker.stats().notifications_rejected_option_context.load(), 0u);

  // The DEFAULT context stated explicitly means the same thing as saying
  // nothing, and must be treated identically.
  notification.option_context = "psoc1\n";
  notification.option_signature =
      OptionContextSignature(notification.option_context);
  ASSERT_EQ(notification.option_signature,
            std::string(kDefaultOptionContextSignature));
  notification.url = "http://example.com/default-context-explicit.css";
  CacheOriginal(worker.cache(), notification.url, css);
  {
    const std::vector<char> wire = notification.Serialize();
    ASSERT_FALSE(wire.empty());
    SendRawNotificationBytes(wire);
  }
  for (int i = 0; i < 80 * kSanitizerBudgetScale; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    if (worker.stats().notifications_received.load() > 1) break;
  }
  EXPECT_GE(worker.stats().notifications_received.load(), 2u);
  EXPECT_EQ(worker.stats().notifications_rejected_option_context.load(), 0u)
      << "stating the default context explicitly must not be refused";
  EXPECT_EQ(
      worker.stats().notifications_accepted_non_default_option_context.load(),
      0u)
      << "the default context, stated or omitted, is not a non-default one and "
         "must leave that counter alone";

  // POSITIVE proof, not just the absence of a refusal.
  //
  // notifications_received is incremented BEFORE the option-context gate, so it
  // moves whether or not the notification was declined. On its own it cannot
  // tell "processed" from "counted and dropped", which would make this control
  // resemble the declined case rather than contrast with it. So this waits for
  // the optimizer to have actually run and then reads the result back.
  //
  // css_processed is the counter to wait on, not variants_written: the CSS path
  // reports through the per-content-type counter, and the pre-existing CSS
  // tests use exactly this pairing (poll css_processed AND drain in-flight
  // work, because css_processed moves before the in-flight entry is erased).
  for (int i = 0; i < 80 * kSanitizerBudgetScale; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    if (worker.stats().css_processed.load() >= 2 &&
        worker.in_flight_work() == 0) {
      break;
    }
  }
  EXPECT_GE(worker.stats().css_processed.load(), 2u)
      << "both notifications -- the one with no options context and the one "
         "stating the default context explicitly -- must reach the optimizer";

  const std::string optimized =
      ReadVariant(worker.cache(), url, CapabilityMask());
  ASSERT_FALSE(optimized.empty())
      << "a notification with no options context must be optimized, not merely "
         "counted; without this the declined case and this one would look "
         "alike";
  EXPECT_LT(optimized.size(), css.size())
      << "the variant should be minified, which is what proves the optimizer "
         "ran rather than that something was merely stored";
}

// The other half of the split: a frame this build's version DOES cover, but
// whose body is damaged, must land in the malformed bucket.  Without this,
// "rejected_version == 0" would be uninformative — it could just mean the
// receive path routes every failure to the other counter.
TEST_F(WorkerTest, MalformedNotifyCountedSeparatelyFromVersionSkew) {
  WorkerConfig config;
  config.socket_path = socket_path_;
  config.cache_path = cache_path_;
  config.cache_size_bytes = static_cast<uint64_t>(10 * 1024 * 1024);

  NullMessageHandler handler;
  Worker worker(config, &handler);
  ASSERT_TRUE(worker.Initialize());

  std::thread worker_thread([&worker]() { worker.Run(); });
  WorkerStopper stopper(worker, worker_thread);
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  CacheNotification notification;
  notification.url = "http://example.com/corrupt-frame.css";
  notification.hostname = "example.com";
  notification.scheme = "https";
  notification.content_type = ContentType::kCss;
  notification.capability_mask = CapabilityMask().Encode();
  std::vector<char> wire = notification.Serialize();
  ASSERT_FALSE(wire.empty());
  // Current version, invalid scheme byte.
  //
  // The offset moved when the wire gained the v5 option-context tail: the
  // scheme byte is now followed by agent_request(1) + option_context_length(4)
  // + option_signature_length(1), and since v6 by site_length(1) + flags(1),
  // so it sits nine from the end rather than second. Left at an old offset
  // this test still passed -- corrupting a length field also yields a
  // malformed frame -- but it would have stopped testing what it says it
  // tests, which is the quiet way a regression test turns into a tautology.
  const size_t scheme_index = wire.size() - 9;
  ASSERT_EQ(0x02u, static_cast<unsigned char>(wire[scheme_index]))
      << "scheme byte is not where this test thinks it is";
  wire[scheme_index] = static_cast<char>(0x7E);
  SendRawNotificationBytes(wire);

  bool counted = false;
  for (int i = 0; i < 80 * kSanitizerBudgetScale; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    if (worker.stats().notifications_rejected_malformed.load() >= 1) {
      counted = true;
      break;
    }
  }
  EXPECT_TRUE(counted);
  EXPECT_EQ(worker.stats().notifications_rejected_version.load(), 0u)
      << "a corrupt frame from a version-matched peer is not version skew";
  EXPECT_EQ(worker.stats().notifications_received.load(), 0u);
}

// A sentinel-shaped notify is DROPPED and SAID OUT LOUD — and the drop is
// REGISTRATION-BLIND, which is what this test pins.
//
// Two halves, and it is worth being exact about which one is new.  The drop is
// pre-existing behaviour (SentinelViewportMaskRejected above covers it for one
// id).  What had no coverage is that the drop is *audible*: a mixed fleet where
// one side speaks a namespace the other does not looks exactly like a cache
// that never warms, and the log line is the only thing that tells them apart.
//
// The registration-blindness of the DECISION is asserted rather than assumed,
// because it is easy to read the sentinel registry as if it governed this
// path. It does not: the guard in worker.cc is viewport-bits-shaped and never
// consults IsRegisteredSentinel, so a REGISTERED id and an UNREGISTERED one
// are dropped identically — deliberately, since what a notification may
// trigger is a fixed set of whole-mask specials and not "whatever this build
// recognises". Driving both through the same assertions documents that
// contract, and this test is the thing that will fail — correctly, demanding
// an update to it and to lib/classify/CLAUDE.md — on the day a lane wires the
// registry into this path as a DECISION and the two stop being equivalent.
//
// The registry does reach the LOG LINE: the message names the class, so a
// peer naming a class this build has never heard of (unknown_sentinel — a
// namespace-skewed peer) reads differently from a peer misusing a class this
// build knows. Naming is not gating, and the two cases are asserted below to
// be identical in treatment and different only in what the log says.
TEST_F(WorkerTest, SentinelShapedNotifyDroppedAndLoggedRegardlessOfRegistry) {
  // A recording handler, because "and logged" is the half with no coverage.
  using RecordingHandler = WarningRecordingHandler;

  // 0x0C is a REGISTERED sentinel (reserved for durable original content);
  // 0xFC is sentinel-shaped and names no class at all.  Both must be handled
  // identically today.
  struct Case {
    const char* label;
    uint32_t mask;
    bool registered;
  };
  const Case cases[] = {
      {"registered-0x0C", 0x0C, true},
      {"unregistered-0xFC", 0xFC, false},
  };
  for (const auto& c : cases) {
    ASSERT_TRUE(IsSentinel(static_cast<AlternateId>(c.mask))) << c.label;
    ASSERT_EQ(IsRegisteredSentinel(static_cast<AlternateId>(c.mask)),
              c.registered)
        << c.label
        << ": the premise of this test is that these two differ in "
           "registration and NOT in treatment";
  }

  WorkerConfig config;
  config.socket_path = socket_path_;
  config.cache_path = cache_path_;
  config.cache_size_bytes = static_cast<uint64_t>(10 * 1024 * 1024);

  RecordingHandler handler;
  Worker worker(config, &handler);
  ASSERT_TRUE(worker.Initialize());

  std::string css = "body  {  margin:  0;  }\n";
  for (const auto& c : cases) {
    CacheOriginal(
        worker.cache(),
        std::string("http://example.com/sentinel-notify-") + c.label + ".css",
        css);
  }

  std::thread worker_thread([&worker]() { worker.Run(); });
  WorkerStopper stopper(worker, worker_thread);
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  uint64_t errors_before = 0;
  uint64_t rejected_before = 0;
  size_t warnings_before = 0;
  for (const auto& c : cases) {
    SCOPED_TRACE(c.label);
    errors_before = worker.stats().errors.load();
    rejected_before = worker.stats().notifications_rejected_sentinel.load();
    warnings_before = handler.warnings().size();

    std::string url =
        std::string("http://example.com/sentinel-notify-") + c.label + ".css";
    CacheNotification notification;
    notification.url = url;
    notification.scheme = "https";
    notification.content_type = ContentType::kCss;
    notification.capability_mask = c.mask;
    SendNotification(notification);

    for (int i = 0; i < 80 * kSanitizerBudgetScale; ++i) {
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
      if (worker.stats().errors.load() > errors_before) break;
    }

    EXPECT_GT(worker.stats().errors.load(), errors_before)
        << "the notify must be dropped";
    // Dropped for a NAMED reason, not folded into the generic error total:
    // "a peer is talking about entry classes" and "an optimization failed"
    // are different operational facts.
    EXPECT_GT(worker.stats().notifications_rejected_sentinel.load(),
              rejected_before)
        << "the drop must be attributable on the stats surface";

    auto warnings = handler.warnings();
    ASSERT_GT(warnings.size(), warnings_before)
        << "the drop must be visible in the log, not silent";
    bool logged = false;
    bool named_class = false;
    const std::string expected_name(
        SentinelName(static_cast<AlternateId>(c.mask)));
    for (size_t i = warnings_before; i < warnings.size(); ++i) {
      if (warnings[i].find("sentinel") != std::string::npos) logged = true;
      if (warnings[i].find(expected_name) != std::string::npos) {
        named_class = true;
      }
    }
    EXPECT_TRUE(logged) << "the log line must name what was rejected";
    // Registered ids log their class name; an unregistered one logs
    // "unknown_sentinel" — which is the only thing in the whole drop that
    // tells an operator a peer is speaking a namespace this build lacks.
    EXPECT_TRUE(named_class)
        << "the log line must name the entry class (" << expected_name << ")";

    CapabilityMask mask = CapabilityMask::Decode(c.mask);
    EXPECT_TRUE(ReadVariant(worker.cache(), url, mask).empty());
  }
}

TEST_F(WorkerTest, UrlTooLongRejected) {
  WorkerConfig config;
  config.socket_path = socket_path_;
  config.cache_path = cache_path_;
  config.cache_size_bytes = static_cast<uint64_t>(10 * 1024 * 1024);
  config.max_url_length = 100;  // Very short limit

  NullMessageHandler handler;
  Worker worker(config, &handler);
  ASSERT_TRUE(worker.Initialize());

  // Pre-populate with content at a very long URL
  std::string url = "http://example.com/" + std::string(200, 'x');
  std::string css = "body { margin: 0; }";
  CacheOriginal(worker.cache(), url, css);

  std::thread worker_thread([&worker]() { worker.Run(); });
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  CacheNotification notification;
  notification.url = url;
  notification.scheme = "https";
  notification.content_type = ContentType::kCss;
  notification.capability_mask = 0x00000001;
  SendNotification(notification);

  std::this_thread::sleep_for(std::chrono::milliseconds(300));

  // No processing should happen
  CapabilityMask mask = CapabilityMask::Decode(0x00000001);
  std::string variant = ReadVariant(worker.cache(), url, mask);
  EXPECT_TRUE(variant.empty());

  worker.Shutdown();
  worker_thread.join();
}

TEST_F(WorkerTest, HtmlTooLargeSkipped) {
  WorkerConfig config;
  config.socket_path = socket_path_;
  config.cache_path = cache_path_;
  config.cache_size_bytes = static_cast<uint64_t>(10 * 1024 * 1024);
  config.max_html_size = 100;  // Very small limit

  NullMessageHandler handler;
  Worker worker(config, &handler);
  ASSERT_TRUE(worker.Initialize());

  // Pre-populate with oversized HTML
  std::string url = "http://example.com/huge.html";
  std::string html = "<html><head><style>" + std::string(200, 'x') +
                     "</style></head><body></body></html>";
  CacheOriginal(worker.cache(), url, html);

  std::thread worker_thread([&worker]() { worker.Run(); });
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  CacheNotification notification;
  notification.url = url;
  notification.scheme = "https";
  notification.content_type = ContentType::kHtml;
  notification.capability_mask = 0x00000001;
  SendNotification(notification);

  std::this_thread::sleep_for(std::chrono::milliseconds(300));

  CapabilityMask mask = CapabilityMask::Decode(0x00000001);
  std::string variant = ReadVariant(worker.cache(), url, mask);
  EXPECT_TRUE(variant.empty()) << "HTML variant written despite size limit";

  worker.Shutdown();
  worker_thread.join();
}

TEST_F(WorkerTest, DuplicateNotificationSkipped) {
  WorkerConfig config;
  config.socket_path = socket_path_;
  config.cache_path = cache_path_;
  config.cache_size_bytes = static_cast<uint64_t>(10 * 1024 * 1024);

  NullMessageHandler handler;
  Worker worker(config, &handler);
  ASSERT_TRUE(worker.Initialize());

  // Pre-populate cache with CSS
  std::string url = "http://example.com/dedup.css";
  std::string css = "body  {  margin:  0;  padding:  0;  }\n";
  CacheOriginal(worker.cache(), url, css);

  std::thread worker_thread([&worker]() { worker.Run(); });
  WorkerStopper stopper(worker, worker_thread);
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  // Use a non-default notification mask — worker normalizes CSS to
  // Desktop/Identity regardless of the notification viewport.
  uint32_t mask_val = 0x00000001;  // WebP/Mobile

  // Send first notification
  CacheNotification notification;
  notification.url = url;
  notification.scheme = "https";
  notification.content_type = ContentType::kCss;
  notification.capability_mask = mask_val;
  SendNotification(notification);

  // Wait for first processing to complete AND in-flight work to drain.
  // The variant may appear in cache before MarkVariantProcessed populates
  // the dedup set, so also check in_flight_work==0.
  CapabilityMask mask;  // Desktop/Identity
  std::string variant;
  for (int i = 0; i < 40 * kSanitizerBudgetScale; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    variant = ReadVariant(worker.cache(), url, mask);
    if (!variant.empty() && worker.in_flight_work() == 0) break;
  }
  ASSERT_FALSE(variant.empty()) << "First CSS variant not written";

  uint64_t dedup_before = worker.stats().notifications_skipped_dedup.load();

  // Send duplicate notification - should be skipped (variant exists)
  SendNotification(notification);
  // Poll for the dedup counter to advance instead of a fixed sleep (slow
  // shards can sit at the pipe read past a fixed budget).  This also lets
  // the variant settle for the unchanged-variant invariant below.
  for (int i = 0; i < 80 * kSanitizerBudgetScale; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    if (worker.stats().notifications_skipped_dedup.load() > dedup_before) break;
  }

  // The variant should be unchanged (dedup skipped reprocessing)
  std::string variant2 = ReadVariant(worker.cache(), url, mask);
  EXPECT_EQ(variant, variant2);
  EXPECT_GT(worker.stats().notifications_skipped_dedup.load(), dedup_before)
      << "Second notification should have been skipped by dedup";
}

TEST_F(WorkerTest, StatsCountersIncrement) {
  // Verify that processing stats counters are incremented correctly
  // when notifications are handled.
  WorkerConfig config;
  config.socket_path = socket_path_;
  config.cache_path = cache_path_;
  config.cache_size_bytes = static_cast<uint64_t>(10 * 1024 * 1024);

  NullMessageHandler handler;
  Worker worker(config, &handler);
  ASSERT_TRUE(worker.Initialize());

  // Stats should start at zero
  EXPECT_EQ(worker.stats().notifications_received.load(), 0u);
  EXPECT_EQ(worker.stats().variants_written.load(), 0u);

  // Pre-populate CSS content
  std::string css_url = "http://example.com/stats.css";
  std::string css = "body { margin: 0; } h1 { color: red; }";
  CacheOriginal(worker.cache(), css_url, css);

  // Run worker in background
  std::thread worker_thread([&worker]() { worker.Run(); });
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  // Send CSS notification
  CacheNotification notification;
  notification.url = css_url;
  notification.scheme = "https";
  notification.content_type = ContentType::kCss;
  notification.capability_mask = CapabilityMask().Encode();
  SendNotification(notification);

  // Wait for processing — poll until every asserted counter has advanced
  // instead of a fixed sleep (slow shards can sit at the pipe read past a
  // fixed budget).
  for (int i = 0; i < 80 * kSanitizerBudgetScale; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    if (worker.stats().notifications_received.load() >= 1u &&
        worker.stats().css_processed.load() >= 1u &&
        worker.stats().variants_written.load() >= 1u) {
      break;
    }
  }

  // Verify counters incremented
  EXPECT_GE(worker.stats().notifications_received.load(), 1u);
  EXPECT_GE(worker.stats().css_processed.load(), 1u);
  EXPECT_GE(worker.stats().variants_written.load(), 1u);

  worker.Shutdown();
  worker_thread.join();
}

TEST_F(WorkerTest, HealthEndpointIncludesStats) {
  // Verify the health endpoint response includes stats fields.
  WorkerConfig config;
  config.socket_path = socket_path_;
  config.cache_path = cache_path_;
  config.cache_size_bytes = static_cast<uint64_t>(10 * 1024 * 1024);

  NullMessageHandler handler;
  Worker worker(config, &handler);
  ASSERT_TRUE(worker.Initialize());

  std::thread worker_thread([&worker]() { worker.Run(); });
  WorkerStopper stopper(worker, worker_thread);
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  // Connect to health socket
  std::string health_path = worker.health_socket_path();
  intptr_t fd = ConnectToSocket(health_path);
  ASSERT_GE(fd, static_cast<intptr_t>(0));

  char buf[512];
  ssize_t n = pagespeed::test::PipeRead(fd, buf, sizeof(buf) - 1);
  ASSERT_GT(n, 0);
  buf[n] = '\0';
  pagespeed::test::ClosePipe(fd);

  std::string response(buf, n);
  // Response should contain the extended stats fields
  EXPECT_NE(response.find("OK "), std::string::npos);
  EXPECT_NE(response.find("notifs="), std::string::npos);
  EXPECT_NE(response.find("variants="), std::string::npos);
  EXPECT_NE(response.find("proactive="), std::string::npos);
  EXPECT_NE(response.find("errors="), std::string::npos);
  EXPECT_NE(response.find("cache_entries="), std::string::npos);
}

TEST_F(WorkerTest, MaxRequestSizeEnforced) {
  WorkerConfig config;
  config.socket_path = socket_path_;
  config.max_request_size = 32;  // Very small limit
  config.connection_timeout_ms = 10000;

  NullMessageHandler handler;
  Worker worker(config, &handler);
  ASSERT_TRUE(worker.Initialize());

  std::thread worker_thread([&worker]() { worker.Run(); });
  WorkerStopper stopper(worker, worker_thread);
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  // Connect and send a message with large length header
  intptr_t fd = ConnectToSocket(socket_path_);
  ASSERT_GE(fd, static_cast<intptr_t>(0));

  // Create a message header claiming 1MB of data
  uint32_t fake_len = 1024 * 1024;
  char header[4];
  header[0] = (fake_len >> 24) & 0xFF;
  header[1] = (fake_len >> 16) & 0xFF;
  header[2] = (fake_len >> 8) & 0xFF;
  header[3] = fake_len & 0xFF;
  pagespeed::test::PipeWrite(fd, header, 4);

  // Worker should disconnect due to oversized message
  std::this_thread::sleep_for(std::chrono::milliseconds(200));
  char tmp[16];
  ssize_t n = pagespeed::test::PipeRead(fd, tmp, sizeof(tmp));
  EXPECT_LE(n, 0) << "Connection should be closed after oversized message";

  pagespeed::test::ClosePipe(fd);
}

TEST_F(WorkerTest, WarmupSentinelProcessesCss) {
  // Verify that the warmup sentinel triggers CSS minification for
  // a URL that doesn't have a minified variant yet.
  WorkerConfig config;
  config.socket_path = socket_path_;
  config.cache_path = cache_path_;
  config.cache_size_bytes = static_cast<uint64_t>(10 * 1024 * 1024);
  config.enable_warmup = true;

  NullMessageHandler handler;
  Worker worker(config, &handler);
  ASSERT_TRUE(worker.Initialize());

  // Pre-populate CSS content
  std::string css_url = "http://example.com/warmup.css";
  std::string css = "body { margin: 0; } h1 { color: red; }";
  CacheOriginal(worker.cache(), css_url, css);

  std::thread worker_thread([&worker]() { worker.Run(); });
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  // Send warmup notification (sentinel mask)
  CacheNotification notification;
  notification.url = css_url;
  notification.scheme = "https";
  notification.content_type = ContentType::kCss;
  notification.capability_mask = kWarmupSentinel;
  SendNotification(notification);

  // Poll for minified variant at default mask
  CapabilityMask default_mask;
  std::string variant;
  for (int i = 0; i < 40 * kSanitizerBudgetScale; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    variant = ReadVariant(worker.cache(), css_url, default_mask);
    if (!variant.empty()) break;
  }

  EXPECT_FALSE(variant.empty()) << "Warmup should produce minified CSS";
  if (!variant.empty()) {
    EXPECT_LT(variant.size(), css.size());
  }

  worker.Shutdown();
  worker_thread.join();
}

TEST_F(WorkerTest, WarmupDisabledIgnoresSentinel) {
  // Verify that warmup is skipped when disabled — original CSS is
  // NOT replaced with minified version.
  WorkerConfig config;
  config.socket_path = socket_path_;
  config.cache_path = cache_path_;
  config.cache_size_bytes = static_cast<uint64_t>(10 * 1024 * 1024);
  config.enable_warmup = false;

  NullMessageHandler handler;
  Worker worker(config, &handler);
  ASSERT_TRUE(worker.Initialize());

  std::string css_url = "http://example.com/no-warmup.css";
  std::string css = "body { margin: 0; } h1 { color: red; }";
  CacheOriginal(worker.cache(), css_url, css);

  std::thread worker_thread([&worker]() { worker.Run(); });
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  CacheNotification notification;
  notification.url = css_url;
  notification.scheme = "https";
  notification.content_type = ContentType::kCss;
  notification.capability_mask = kWarmupSentinel;
  SendNotification(notification);

  std::this_thread::sleep_for(std::chrono::milliseconds(300));

  // Original CSS should be unchanged (not minified)
  CapabilityMask default_mask;
  std::string variant = ReadVariant(worker.cache(), css_url, default_mask);
  EXPECT_EQ(variant, css) << "Warmup disabled: original should be unchanged";

  worker.Shutdown();
  worker_thread.join();
}

// =============================================================================
// Disable toggles
// =============================================================================

TEST_F(WorkerTest, DisableJsSkipsProcessing) {
  // When disable_js is set, JS notifications are skipped.
  WorkerConfig config;
  config.socket_path = socket_path_;
  config.cache_path = cache_path_;
  config.cache_size_bytes = static_cast<uint64_t>(10 * 1024 * 1024);
  config.disable_js = true;

  NullMessageHandler handler;
  Worker worker(config, &handler);
  ASSERT_TRUE(worker.Initialize());

  std::string url = "http://example.com/disabled-js.js";
  std::string js = "var foo = 1; var bar = 2;";
  CacheOriginal(worker.cache(), url, js);

  std::thread worker_thread([&worker]() { worker.Run(); });
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  CacheNotification notification;
  notification.url = url;
  notification.scheme = "https";
  notification.content_type = ContentType::kJs;
  notification.capability_mask = CapabilityMask().Encode();
  uint64_t before = worker.stats().notifications_received.load();
  SendNotification(notification);

  // Poll until the notification has been received by the worker.
  for (int i = 0; i < 40; ++i) {
    if (worker.stats().notifications_received.load() > before) break;
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
  }
  EXPECT_GT(worker.stats().notifications_received.load(), before)
      << "Notification was not received by worker";

  // JS should not be processed (disabled).
  EXPECT_EQ(worker.stats().js_processed.load(), 0u);

  worker.Shutdown();
  worker_thread.join();
}

TEST_F(WorkerTest, DisableImageSkipsProcessing) {
  // When disable_image is set, image notifications are skipped.
  WorkerConfig config;
  config.socket_path = socket_path_;
  config.cache_path = cache_path_;
  config.cache_size_bytes = static_cast<uint64_t>(10 * 1024 * 1024);
  config.disable_image = true;

  NullMessageHandler handler;
  Worker worker(config, &handler);
  ASSERT_TRUE(worker.Initialize());

  // Pre-populate with image data (use a real JPEG to be safe).
  std::string image_data = ReadTestFile("jpeg/sjpeg1.jpg");
  ASSERT_FALSE(image_data.empty());
  std::string url = "http://example.com/disabled-image.jpg";
  CacheOriginal(worker.cache(), url, image_data);

  std::thread worker_thread([&worker]() { worker.Run(); });
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  uint64_t before = worker.stats().notifications_received.load();
  CacheNotification notification;
  notification.url = url;
  notification.scheme = "https";
  notification.content_type = ContentType::kImage;
  notification.capability_mask = CapabilityMask().Encode();
  SendNotification(notification);

  // Poll until the notification has been received by the worker.
  for (int i = 0; i < 40; ++i) {
    if (worker.stats().notifications_received.load() > before) break;
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
  }
  EXPECT_GT(worker.stats().notifications_received.load(), before)
      << "Notification was not received by worker";

  // Image should not be processed (disabled).
  EXPECT_EQ(worker.stats().images_processed.load(), 0u);

  worker.Shutdown();
  worker_thread.join();
}

TEST_F(WorkerTest, ContentTypeOtherNotification) {
  // A notification with ContentType::kOther should be handled
  // gracefully (logged as unknown, no processing).
  WorkerConfig config;
  config.socket_path = socket_path_;
  config.cache_path = cache_path_;
  config.cache_size_bytes = static_cast<uint64_t>(10 * 1024 * 1024);

  NullMessageHandler handler;
  Worker worker(config, &handler);
  ASSERT_TRUE(worker.Initialize());

  std::string url = "http://example.com/data.json";
  std::string content = R"({"key": "value"})";
  CacheOriginal(worker.cache(), url, content);

  std::thread worker_thread([&worker]() { worker.Run(); });
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  CacheNotification notification;
  notification.url = url;
  notification.scheme = "https";
  notification.content_type = ContentType::kOther;
  notification.capability_mask = CapabilityMask().Encode();
  SendNotification(notification);

  // Wait for notification to be received.
  for (int i = 0; i < 40 * kSanitizerBudgetScale; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    if (worker.stats().notifications_received.load() >= 1) break;
  }

  EXPECT_GE(worker.stats().notifications_received.load(), 1u);
  // No variants should be written for unknown content types.
  EXPECT_EQ(worker.stats().variants_written.load(), 0u);
  // No processing counters should increment.
  EXPECT_EQ(worker.stats().html_processed.load(), 0u);
  EXPECT_EQ(worker.stats().css_processed.load(), 0u);
  EXPECT_EQ(worker.stats().js_processed.load(), 0u);
  EXPECT_EQ(worker.stats().images_processed.load(), 0u);

  worker.Shutdown();
  worker_thread.join();
}

// =============================================================================
// Additional coverage tests: config-driven skipping, dedup, HTTP API, browser
// =============================================================================

TEST_F(WorkerTest, HtmlDisabledSkipsProcessing) {
  // With html_mode off and a non-null handler, the HTML disable branch
  // (worker.cc ~1486-1491) is exercised including the Info log call.
  // Verifies notifications_received increments but html_processed does not.
  WorkerConfig config;
  config.socket_path = socket_path_;
  config.cache_path = cache_path_;
  config.cache_size_bytes = static_cast<uint64_t>(10 * 1024 * 1024);
  config.html_mode = HtmlMode::kOff;

  NullMessageHandler handler;
  Worker worker(config, &handler);
  ASSERT_TRUE(worker.Initialize());

  // Pre-populate cache with HTML that has CSS (would normally produce a
  // variant).
  std::string url = "http://example.com/html-disabled-stats.html";
  std::string html =
      "<html><head><style>body { margin: 0; }</style></head>"
      "<body><h1>Disabled</h1></body></html>";
  CacheOriginal(worker.cache(), url, html);

  std::thread worker_thread([&worker]() { worker.Run(); });
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  uint64_t before = worker.stats().notifications_received.load();
  CacheNotification notification;
  notification.url = url;
  notification.scheme = "https";
  notification.content_type = ContentType::kHtml;
  notification.capability_mask = CapabilityMask().Encode();
  SendNotification(notification);

  // Poll until the notification has been received by the worker.
  for (int i = 0; i < 40; ++i) {
    if (worker.stats().notifications_received.load() > before) break;
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
  }
  EXPECT_GT(worker.stats().notifications_received.load(), before)
      << "Notification was not received by worker";

  // HTML should not be processed (disabled).
  EXPECT_EQ(worker.stats().html_processed.load(), 0u);
  // No variant should be written.
  EXPECT_EQ(worker.stats().variants_written.load(), 0u);

  worker.Shutdown();
  worker_thread.join();
}

TEST_F(WorkerTest, CssDisabledSkipsProcessing) {
  // With disable_css and a non-null handler, the CSS disable branch
  // (worker.cc ~2641-2646) is exercised including the Info log call.
  WorkerConfig config;
  config.socket_path = socket_path_;
  config.cache_path = cache_path_;
  config.cache_size_bytes = static_cast<uint64_t>(10 * 1024 * 1024);
  config.disable_css = true;

  NullMessageHandler handler;
  Worker worker(config, &handler);
  ASSERT_TRUE(worker.Initialize());

  std::string url = "http://example.com/css-disabled-stats.css";
  std::string css = "body  {  margin:  0;  padding:  0;  }\n";
  CacheOriginal(worker.cache(), url, css);

  std::thread worker_thread([&worker]() { worker.Run(); });
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  uint64_t before = worker.stats().notifications_received.load();
  CacheNotification notification;
  notification.url = url;
  notification.scheme = "https";
  notification.content_type = ContentType::kCss;
  notification.capability_mask = CapabilityMask().Encode();
  SendNotification(notification);

  // Poll until the notification has been received.
  for (int i = 0; i < 40; ++i) {
    if (worker.stats().notifications_received.load() > before) break;
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
  }
  EXPECT_GT(worker.stats().notifications_received.load(), before)
      << "Notification was not received by worker";

  EXPECT_EQ(worker.stats().css_processed.load(), 0u);
  EXPECT_EQ(worker.stats().variants_written.load(), 0u);

  worker.Shutdown();
  worker_thread.join();
}

TEST_F(WorkerTest, JsDisabledSkipsProcessing) {
  // With disable_js and a non-null handler, the JS disable branch
  // (worker.cc ~2814-2820) is exercised including the Info log call.
  WorkerConfig config;
  config.socket_path = socket_path_;
  config.cache_path = cache_path_;
  config.cache_size_bytes = static_cast<uint64_t>(10 * 1024 * 1024);
  config.disable_js = true;

  NullMessageHandler handler;
  Worker worker(config, &handler);
  ASSERT_TRUE(worker.Initialize());

  std::string url = "http://example.com/js-disabled-stats.js";
  std::string js = "function  hello()  { return 'world'; }\n";
  CacheOriginal(worker.cache(), url, js);

  std::thread worker_thread([&worker]() { worker.Run(); });
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  uint64_t before = worker.stats().notifications_received.load();
  CacheNotification notification;
  notification.url = url;
  notification.scheme = "https";
  notification.content_type = ContentType::kJs;
  notification.capability_mask = CapabilityMask().Encode();
  SendNotification(notification);

  // Poll until the notification has been received.
  for (int i = 0; i < 40; ++i) {
    if (worker.stats().notifications_received.load() > before) break;
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
  }
  EXPECT_GT(worker.stats().notifications_received.load(), before)
      << "Notification was not received by worker";

  EXPECT_EQ(worker.stats().js_processed.load(), 0u);
  EXPECT_EQ(worker.stats().variants_written.load(), 0u);

  worker.Shutdown();
  worker_thread.join();
}

TEST_F(WorkerTest, ImageDisabledSkipsProcessing) {
  // With disable_image and a non-null handler, the image disable branch
  // (worker.cc ~1894-1899) is exercised including the Info log call.
  WorkerConfig config;
  config.socket_path = socket_path_;
  config.cache_path = cache_path_;
  config.cache_size_bytes = static_cast<uint64_t>(10 * 1024 * 1024);
  config.disable_image = true;

  NullMessageHandler handler;
  Worker worker(config, &handler);
  ASSERT_TRUE(worker.Initialize());

  // Use real JPEG data so the cache entry is realistic.
  std::string image_data = ReadTestFile("jpeg/sjpeg1.jpg");
  ASSERT_FALSE(image_data.empty());
  std::string url = "http://example.com/img-disabled-stats.jpg";
  CacheOriginal(worker.cache(), url, image_data);

  std::thread worker_thread([&worker]() { worker.Run(); });
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  uint64_t before = worker.stats().notifications_received.load();
  CacheNotification notification;
  notification.url = url;
  notification.scheme = "https";
  notification.content_type = ContentType::kImage;
  notification.capability_mask = CapabilityMask().Encode();
  SendNotification(notification);

  // Poll until the notification has been received.
  for (int i = 0; i < 40; ++i) {
    if (worker.stats().notifications_received.load() > before) break;
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
  }
  EXPECT_GT(worker.stats().notifications_received.load(), before)
      << "Notification was not received by worker";

  EXPECT_EQ(worker.stats().images_processed.load(), 0u);
  EXPECT_EQ(worker.stats().variants_written.load(), 0u);

  worker.Shutdown();
  worker_thread.join();
}

TEST_F(WorkerTest, NotificationDeduplication) {
  // Send the same CSS notification twice; the second should be deduped
  // and stats.notifications_skipped_dedup should increment.
  // Covers the dedup path in HandleNotification (worker.cc ~1416-1444)
  // and MarkVariantProcessed (worker.cc ~1347-1356).
  WorkerConfig config;
  config.socket_path = socket_path_;
  config.cache_path = cache_path_;
  config.cache_size_bytes = static_cast<uint64_t>(10 * 1024 * 1024);

  NullMessageHandler handler;
  Worker worker(config, &handler);
  ASSERT_TRUE(worker.Initialize());

  std::string url = "http://example.com/dedup-counter.css";
  std::string css = "body  {  margin:  0;  padding:  0;  }\n";
  CacheOriginal(worker.cache(), url, css);

  std::thread worker_thread([&worker]() { worker.Run(); });
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  // Send first notification (empty hostname matches CacheOriginal's "").
  CacheNotification notification;
  notification.url = url;
  notification.scheme = "https";
  notification.content_type = ContentType::kCss;
  notification.capability_mask = CapabilityMask().Encode();
  SendNotification(notification);

  // Wait for first processing to complete AND in-flight work to drain.
  // css_processed is incremented before MarkVariantProcessed populates
  // the dedup set, so polling css_processed alone is insufficient --
  // in_flight_work==0 guarantees MarkVariantProcessed has returned.
  for (int i = 0; i < 40 * kSanitizerBudgetScale; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    if (worker.stats().css_processed.load() >= 1 &&
        worker.in_flight_work() == 0) {
      break;
    }
  }
  EXPECT_GE(worker.stats().css_processed.load(), 1u)
      << "First CSS notification was not processed";

  // css_processed moves before the in-flight entry is erased.  Drain first, or
  // the duplicate below is rejected by the in-flight guard and bumps
  // skipped_inflight instead of skipped_dedup.
  WaitForWorkerIdle(worker);

  uint64_t dedup_before = worker.stats().notifications_skipped_dedup.load();

  // Send the same notification again -- should be deduped.
  SendNotification(notification);

  // Poll until the dedup counter increments.
  for (int i = 0; i < 40 * kSanitizerBudgetScale; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    if (worker.stats().notifications_skipped_dedup.load() > dedup_before) break;
  }
  EXPECT_GT(worker.stats().notifications_skipped_dedup.load(), dedup_before)
      << "Second notification should be deduped";

  // CSS processed counter should still be 1 (not reprocessed).
  EXPECT_EQ(worker.stats().css_processed.load(), 1u);

  worker.Shutdown();
  worker_thread.join();
}

TEST_F(WorkerTest, UnknownContentTypeNotification) {
  // Send a notification with ContentType::kOther and a non-null handler.
  // Exercises the default case in the switch statement (worker.cc ~2965-2971)
  // including the Warning log call.  Also verifies no processing counters
  // increment and the timing accumulation still runs.
  WorkerConfig config;
  config.socket_path = socket_path_;
  config.cache_path = cache_path_;
  config.cache_size_bytes = static_cast<uint64_t>(10 * 1024 * 1024);

  NullMessageHandler handler;
  Worker worker(config, &handler);
  ASSERT_TRUE(worker.Initialize());

  std::string url = "http://example.com/unknown-type.bin";
  std::string content = "binary data here";
  CacheOriginal(worker.cache(), url, content);

  std::thread worker_thread([&worker]() { worker.Run(); });
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  uint64_t before = worker.stats().notifications_received.load();
  CacheNotification notification;
  notification.url = url;
  notification.scheme = "https";
  notification.content_type = ContentType::kOther;
  notification.capability_mask = CapabilityMask().Encode();
  SendNotification(notification);

  // Wait for notification to be received.
  for (int i = 0; i < 40 * kSanitizerBudgetScale; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    if (worker.stats().notifications_received.load() > before) break;
  }

  EXPECT_GT(worker.stats().notifications_received.load(), before);
  // No processing counters should increment.
  EXPECT_EQ(worker.stats().html_processed.load(), 0u);
  EXPECT_EQ(worker.stats().css_processed.load(), 0u);
  EXPECT_EQ(worker.stats().js_processed.load(), 0u);
  EXPECT_EQ(worker.stats().images_processed.load(), 0u);
  EXPECT_EQ(worker.stats().variants_written.load(), 0u);
  // Total processing time should still accumulate (timing code runs
  // regardless of content type).
  // We can't assert exact values but the counter should be accessible.
  (void)worker.stats().total_processing_time_us.load();

  worker.Shutdown();
  worker_thread.join();
}

// The extractor's default small-sheet threshold for the coverage gate is the
// worker's default (the worker also passes its configured value): rules kept
// for wide replaced elements are bounded by the gate the block is judged by.
TEST(WorkerConfigTest, ExtractorSmallSheetThresholdMatchesTheWorkers) {
  EXPECT_EQ(CriticalCssConfig{}.inline_limit_min_sheet_bytes,
            WorkerConfig{}.async_css_min_deferred_bytes);
}

// ============================================================================
// Site identity (wire v6): every keyspace is its own, and the module's
// notifications never start page work.
// ============================================================================

namespace {

const std::string kSiteOne(kSiteIdChars, 'a');
const std::string kSiteTwo(kSiteIdChars, 'b');

// Writes `content` as the identity entry of `path` in `site`'s keyspace.
void CacheSitedOriginal(PageSpeedCache* cache, const std::string& path,
                        const std::string& hostname, const std::string& site,
                        const std::string& content,
                        ContentType type = ContentType::kOther) {
  CapabilityMask identity;
  AlternateMetadata meta;
  meta.full_mask = identity.Encode();
  meta.content_type = type;
  auto wh = cache->WriteAlternate(
      path, hostname, "https",
      MaskToAlternateId(static_cast<uint8_t>(identity.Encode() & 0xFF)),
      content.size(), meta, site);
  ASSERT_TRUE(wh.has_value());
  ASSERT_TRUE(
      wh->write_sync(std::as_bytes(std::span(content.data(), content.size())))
          .has_value());
  ASSERT_TRUE(wh->close_sync().has_value());
}

// The identity entry of `path` in `site`'s keyspace, with its flags.
std::optional<std::pair<std::string, uint8_t>> ReadSitedIdentity(
    PageSpeedCache* cache, const std::string& path, const std::string& hostname,
    const std::string& site) {
  CapabilityMask identity;
  auto r = cache->ReadAlternate(
      path, hostname, "https",
      MaskToAlternateId(static_cast<uint8_t>(identity.Encode() & 0xFF)), site);
  if (!r.has_value()) return std::nullopt;
  auto c = r->content();
  return std::make_pair(
      std::string(reinterpret_cast<const char*>(c.data()), c.size()),
      r->metadata.flags);
}

CacheNotification ModuleCss(const std::string& url, const std::string& site) {
  CacheNotification n;
  n.url = url;
  n.hostname = "example.com";
  n.scheme = "https";
  n.content_type = ContentType::kCss;
  n.capability_mask = CapabilityMask().Encode();
  n.site = site;
  n.from_module = true;
  return n;
}

WorkerConfig SiteTestConfig(const std::string& socket_path,
                            const std::string& cache_path) {
  WorkerConfig config;
  config.socket_path = socket_path;
  config.cache_path = cache_path;
  config.cache_size_bytes = static_cast<uint64_t>(10 * 1024 * 1024);
  return config;
}

WorkerConfig SiteAnalysisConfig(const std::string& socket_path,
                                const std::string& cache_path) {
  WorkerConfig config = SiteTestConfig(socket_path, cache_path);
  // No Chrome runs: the manager exists so its queue can be inspected.
  config.browser_analysis.sandbox_mode = BrowserSandboxMode::kOff;
  config.browser_analysis.enabled = true;
  config.browser_analysis.chrome_binary = "/nonexistent/chrome";
  config.browser_analysis.agent_optimize = true;
  return config;
}

}  // namespace

TEST_F(WorkerTest, ModuleNotificationWithoutASiteIsRefused) {
  Worker worker(SiteTestConfig(socket_path_, cache_path_), nullptr);
  ASSERT_TRUE(worker.Initialize());
  const std::string css = "body  {  margin:  0;  }\n";
  CacheSitedOriginal(worker.cache(), "/style.css", "example.com", "", css);

  std::thread worker_thread([&worker]() { worker.Run(); });
  WorkerStopper stopper(worker, worker_thread);
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  SendNotification(ModuleCss("http://example.com/style.css", ""));
  ASSERT_TRUE(pagespeed::test::WaitForNotificationsRetired(worker, 1,
                                                           kWorkerStallBudget));
  EXPECT_EQ(worker.stats().notifications_rejected_empty_site.load(), 1u);
  EXPECT_EQ(worker.stats().variants_written.load(), 0u);
  auto unsited =
      ReadSitedIdentity(worker.cache(), "/style.css", "example.com", "");
  ASSERT_TRUE(unsited.has_value());
  EXPECT_EQ(unsited->first, css) << "the unsited entry was not touched";
}

TEST_F(WorkerTest, ModuleNotificationAskingForPageWorkIsRefused) {
  Worker worker(SiteAnalysisConfig(socket_path_, cache_path_), nullptr);
  ASSERT_TRUE(worker.Initialize());
  std::thread worker_thread([&worker]() { worker.Run(); });
  WorkerStopper stopper(worker, worker_thread);
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  CacheNotification html = ModuleCss("http://example.com/p.html", kSiteOne);
  html.content_type = ContentType::kHtml;
  CacheNotification llms = ModuleCss("http://example.com/llms.txt", kSiteOne);
  llms.capability_mask = kLlmsTxtSentinel;
  CacheNotification agent = ModuleCss("http://example.com/a.css", kSiteOne);
  agent.agent_request = true;
  CacheNotification vouched = ModuleCss("http://example.com/b.css", kSiteOne);
  vouched.host_vouched = true;
  for (const CacheNotification& n : {html, llms, agent, vouched}) {
    SendNotification(n);
  }
  ASSERT_TRUE(pagespeed::test::WaitForNotificationsRetired(worker, 4,
                                                           kWorkerStallBudget));
  EXPECT_EQ(worker.stats().notifications_rejected_module_render.load(), 4u);
  EXPECT_FALSE(worker.TestBrowserManager()->TestDequeue().has_value())
      << "no analysis was queued";
}

TEST_F(WorkerTest, DefaultHtmlModeIsFullAndRefusesModuleHtml) {
  // No html_mode anywhere: the mode is full (today's behaviour), and a
  // module HTML frame is refused even when vouched.
  WorkerConfig config = SiteAnalysisConfig(socket_path_, cache_path_);
  ASSERT_EQ(config.html_mode, HtmlMode::kFull);
  Worker worker(config, nullptr);
  ASSERT_TRUE(worker.Initialize());
  std::thread worker_thread([&worker]() { worker.Run(); });
  WorkerStopper stopper(worker, worker_thread);
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  CacheNotification html = ModuleCss("http://example.com/p.html", kSiteOne);
  html.content_type = ContentType::kHtml;
  html.host_vouched = true;
  SendNotification(html);
  ASSERT_TRUE(pagespeed::test::WaitForNotificationsRetired(worker, 1,
                                                           kWorkerStallBudget));
  EXPECT_EQ(worker.stats().notifications_rejected_module_render.load(), 1u);
  EXPECT_FALSE(worker.TestBrowserManager()->TestDequeue().has_value());
}

TEST_F(WorkerTest, MarkdownHtmlModeStillRefusesModuleHtml) {
  // A markdown-* mode accepts module HTML only for a page the module's HTML
  // store wrote (the html_stored flag); vouching for the host is not enough.
  WorkerConfig config = SiteAnalysisConfig(socket_path_, cache_path_);
  config.html_mode = HtmlMode::kMarkdownStatic;
  Worker worker(config, nullptr);
  ASSERT_TRUE(worker.Initialize());
  std::thread worker_thread([&worker]() { worker.Run(); });
  WorkerStopper stopper(worker, worker_thread);
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  CacheNotification html = ModuleCss("http://example.com/p.html", kSiteOne);
  html.content_type = ContentType::kHtml;
  html.host_vouched = true;
  SendNotification(html);
  ASSERT_TRUE(pagespeed::test::WaitForNotificationsRetired(worker, 1,
                                                           kWorkerStallBudget));
  EXPECT_EQ(worker.stats().notifications_rejected_module_render.load(), 1u);
  EXPECT_FALSE(worker.TestBrowserManager()->TestDequeue().has_value());
}

TEST_F(WorkerTest, TwoSitesOnOneUrlGetIndependentVariants) {
  Worker worker(SiteTestConfig(socket_path_, cache_path_), nullptr);
  ASSERT_TRUE(worker.Initialize());
  const std::string css_one = "h1  {  color:  red;  }\n";
  const std::string css_two = "h2  {  color:  blue;  }\n";
  const std::string css_unsited = "h3  {  color:  green;  }\n";
  CacheSitedOriginal(worker.cache(), "/style.css", "example.com", kSiteOne,
                     css_one);
  CacheSitedOriginal(worker.cache(), "/style.css", "example.com", kSiteTwo,
                     css_two);
  CacheSitedOriginal(worker.cache(), "/style.css", "example.com", "",
                     css_unsited);

  std::thread worker_thread([&worker]() { worker.Run(); });
  WorkerStopper stopper(worker, worker_thread);
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  SendNotification(ModuleCss("http://example.com/style.css", kSiteOne));
  ASSERT_TRUE(pagespeed::test::WaitForNotificationsRetired(worker, 1,
                                                           kWorkerStallBudget));
  auto one =
      ReadSitedIdentity(worker.cache(), "/style.css", "example.com", kSiteOne);
  auto two =
      ReadSitedIdentity(worker.cache(), "/style.css", "example.com", kSiteTwo);
  auto unsited =
      ReadSitedIdentity(worker.cache(), "/style.css", "example.com", "");
  ASSERT_TRUE(one && two && unsited);
  EXPECT_NE(one->first.find("h1"), std::string::npos);
  EXPECT_LT(one->first.size(), css_one.size()) << "site one was optimized";
  EXPECT_EQ(two->first, css_two) << "site two was not touched";
  EXPECT_EQ(unsited->first, css_unsited) << "the unsited entry was not touched";

  // A repeat for site one is already processed; site two is not.
  SendNotification(ModuleCss("http://example.com/style.css", kSiteOne));
  SendNotification(ModuleCss("http://example.com/style.css", kSiteTwo));
  ASSERT_TRUE(pagespeed::test::WaitForNotificationsRetired(worker, 3,
                                                           kWorkerStallBudget));
  EXPECT_EQ(worker.stats().notifications_skipped_dedup.load(), 1u);
  two =
      ReadSitedIdentity(worker.cache(), "/style.css", "example.com", kSiteTwo);
  ASSERT_TRUE(two.has_value());
  EXPECT_NE(two->first.find("h2"), std::string::npos);
  EXPECT_LT(two->first.size(), css_two.size()) << "site two was optimized";
  unsited = ReadSitedIdentity(worker.cache(), "/style.css", "example.com", "");
  ASSERT_TRUE(unsited.has_value());
  EXPECT_EQ(unsited->first, css_unsited);

  // Both sites are now known, and the registry lists each entry under its
  // own site.
  EXPECT_TRUE(worker.url_registry()->HasSite(kSiteOne));
  EXPECT_TRUE(worker.url_registry()->HasSite(kSiteTwo));
  EXPECT_EQ(worker.url_registry()->List(0, 10, {}, kSiteOne).total, 1u);
}

TEST_F(WorkerTest, PurgeGenerationAndCooldownStateArePerSite) {
  Worker worker(SiteTestConfig(socket_path_, cache_path_), nullptr);
  ASSERT_TRUE(worker.Initialize());
  const std::string url = "http://example.com/style.css";
  CacheSitedOriginal(worker.cache(), "/style.css", "example.com", kSiteOne,
                     "a{}");
  CacheSitedOriginal(worker.cache(), "/style.css", "example.com", kSiteTwo,
                     "b{}");

  EXPECT_EQ(1, worker.InvalidateUrl(url, "example.com", "https", kSiteOne));
  EXPECT_EQ(
      worker.CapturePurgeGen(url, "example.com", "https", kSiteOne).url_gen,
      1u);
  EXPECT_EQ(
      worker.CapturePurgeGen(url, "example.com", "https", kSiteTwo).url_gen,
      0u);
  EXPECT_EQ(worker.CapturePurgeGen(url, "example.com", "https").url_gen, 0u);
  // One purge acts on one keyspace.
  EXPECT_FALSE(
      ReadSitedIdentity(worker.cache(), "/style.css", "example.com", kSiteOne)
          .has_value());
  EXPECT_TRUE(
      ReadSitedIdentity(worker.cache(), "/style.css", "example.com", kSiteTwo)
          .has_value());
  EXPECT_FALSE(worker.GetCooldownForUrl(url, "example.com", "https", kSiteTwo)
                   .has_value());
}

TEST_F(WorkerTest, RevalidationAndRetryKeepTheSiteAndBits) {
  Worker worker(SiteAnalysisConfig(socket_path_, cache_path_), nullptr);
  ASSERT_TRUE(worker.Initialize());
  BrowserAnalysisManager* mgr = worker.TestBrowserManager();
  ASSERT_NE(mgr, nullptr);

  ASSERT_TRUE(mgr->RequestRevalidation(
      "http://example.com/p.html", "example.com", "https", kSiteOne,
      /*host_vouched=*/true, /*from_module=*/false, 0x08, 42, "<html></html>"));
  auto item = mgr->TestDequeue();
  ASSERT_TRUE(item.has_value());
  EXPECT_EQ(item->site, kSiteOne);
  EXPECT_TRUE(item->host_vouched);
  EXPECT_FALSE(item->from_module);

  auto retry = browser_internal::BuildReanalysisItem(*item, 1);
  EXPECT_EQ(retry.site, kSiteOne);
  EXPECT_TRUE(retry.host_vouched);
  EXPECT_FALSE(retry.from_module);
  EXPECT_EQ(retry.retry_count, 1);
}

TEST_F(WorkerTest, TwoSitesOnOneUrlQueueSeparateAgentRenders) {
  Worker worker(SiteAnalysisConfig(socket_path_, cache_path_), nullptr);
  ASSERT_TRUE(worker.Initialize());
  const std::string url = "http://example.com/page.html";
  const std::string html =
      "<html><head><style>h1 { color: red; }</style></head>"
      "<body><h1>Hello</h1></body></html>";
  HtmlScanner scanner;
  HtmlScanResult scan = scanner.Scan(url, html);
  ASSERT_TRUE(scan.success);
  const uint64_t template_hash = TemplateDetector::HashStructure(scan);
  OptimizationProfile profile;
  profile.template_hash_hex = "feed";
  profile.analyzed_url = url;
  profile.expires_at = 0;
  for (const std::string& site : {kSiteOne, kSiteTwo}) {
    CacheSitedOriginal(worker.cache(), "/page.html", "example.com", site, html,
                       ContentType::kHtml);
    worker.TestBrowserManager()->TestStoreProfile(template_hash, profile, site);
  }

  std::thread worker_thread([&worker]() { worker.Run(); });
  WorkerStopper stopper(worker, worker_thread);
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  for (const std::string& site : {kSiteOne, kSiteTwo}) {
    CacheNotification n;
    n.url = url;
    n.hostname = "example.com";
    n.scheme = "https";
    n.content_type = ContentType::kHtml;
    n.capability_mask = CapabilityMask().Encode();
    n.agent_request = true;
    n.site = site;
    n.host_vouched = true;
    SendNotification(n);
  }
  ASSERT_TRUE(pagespeed::test::WaitForNotificationsRetired(worker, 2,
                                                           kWorkerStallBudget));
  std::vector<AnalysisQueue::Item> items;
  while (auto item = worker.TestBrowserManager()->TestDequeue()) {
    items.push_back(std::move(*item));
  }
  ASSERT_EQ(items.size(), 2u) << "one render per site, not one for both";
  EXPECT_NE(items[0].site, items[1].site);
  EXPECT_NE(items[0].template_hash, items[1].template_hash);
  for (const auto& item : items) {
    EXPECT_TRUE(item.force_agent_render);
    EXPECT_TRUE(item.host_vouched);
  }
}

TEST_F(WorkerTest, CombinedCssIsReadFromThePagesOwnSite) {
  Worker worker(SiteAnalysisConfig(socket_path_, cache_path_), nullptr);
  ASSERT_TRUE(worker.Initialize());
  CacheSitedOriginal(worker.cache(), "/s.css", "example.com", kSiteOne,
                     ".one{color:red}", ContentType::kCss);
  CacheSitedOriginal(worker.cache(), "/s.css", "example.com", kSiteTwo,
                     ".two{color:blue}", ContentType::kCss);
  const std::string html =
      "<html><head><link rel=\"stylesheet\" href=\"/s.css\"></head>"
      "<body><p class=one>x</p></body></html>";
  HtmlScanner scanner;
  HtmlScanResult scan = scanner.Scan("/page.html", html);
  ASSERT_TRUE(scan.success);
  const auto& builder = worker.TestBrowserManager()->TestCombinedCssBuilder();
  ASSERT_TRUE(builder);
  auto one = builder(scan, "/page.html", "example.com", "https", kSiteOne,
                     /*host_vouched=*/false, /*from_module=*/false);
  auto two = builder(scan, "/page.html", "example.com", "https", kSiteTwo,
                     /*host_vouched=*/false, /*from_module=*/false);
  EXPECT_NE(one.css.find(".one"), std::string::npos);
  EXPECT_EQ(one.css.find(".two"), std::string::npos);
  EXPECT_NE(two.css.find(".two"), std::string::npos);
  EXPECT_EQ(two.css.find(".one"), std::string::npos);
}

TEST_F(WorkerTest, ReprocessCarriesItsSiteAndVouchesForNoHost) {
  WorkerConfig config = SiteAnalysisConfig(socket_path_, cache_path_);
  config.api_port = -1;
  config.api_no_auth = true;
  NullMessageHandler handler;
  Worker worker(config, &handler);
  ASSERT_TRUE(worker.Initialize());
  const std::string html = "<html><body><h1>Hi</h1></body></html>";
  CacheSitedOriginal(worker.cache(), "/page.html", "example.com", kSiteOne,
                     html, ContentType::kHtml);

  std::thread worker_thread([&worker]() { worker.Run(); });
  WorkerStopper stopper(worker, worker_thread);
  for (int i = 0; i < 40 && worker.api_port() <= 0; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
  }
  ASSERT_GT(worker.api_port(), 0);

  // The site and host must have been served before: record a notification.
  SendNotification(ModuleCss("http://example.com/x.css", kSiteOne));
  ASSERT_TRUE(pagespeed::test::WaitForNotificationsRetired(worker, 1,
                                                           kWorkerStallBudget));

  const std::string body =
      absl::StrCat(R"({"url":"/page.html","hostname":"example.com","site":")",
                   kSiteOne, R"("})");
  const std::string req = absl::StrCat(
      "POST /v1/cache/reprocess HTTP/1.1\r\nHost: localhost\r\n"
      "Content-Type: application/json\r\nConnection: close\r\n"
      "Content-Length: ",
      body.size(), "\r\n\r\n", body);
  const std::string resp = pagespeed::test::SendRequest(
      worker.api_port(), pagespeed::test::InjectCsrfHeaderForV1Mutations(req));
  ASSERT_NE(resp.find("200 OK"), std::string::npos) << resp;
  ASSERT_TRUE(pagespeed::test::WaitForNotificationsRetired(worker, 2,
                                                           kWorkerStallBudget));

  auto item = worker.TestBrowserManager()->TestDequeue();
  ASSERT_TRUE(item.has_value()) << "the reprocessed page was analysed";
  EXPECT_EQ(item->site, kSiteOne);
  EXPECT_FALSE(item->host_vouched);
  EXPECT_FALSE(item->from_module);
  EXPECT_FALSE(item->force_agent_render);
}

TEST_F(WorkerTest, KnownSitesSurviveARestartOfTheWorker) {
  {
    Worker worker(SiteTestConfig(socket_path_, cache_path_), nullptr);
    ASSERT_TRUE(worker.Initialize());
    CacheSitedOriginal(worker.cache(), "/style.css", "example.com", kSiteOne,
                       "h1  {  color:  red;  }\n");
    std::thread worker_thread([&worker]() { worker.Run(); });
    WorkerStopper stopper(worker, worker_thread);
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    SendNotification(ModuleCss("http://example.com/style.css", kSiteOne));
    ASSERT_TRUE(pagespeed::test::WaitForNotificationsRetired(
        worker, 1, kWorkerStallBudget));
  }
  Worker restarted(SiteTestConfig(socket_path_, cache_path_), nullptr);
  ASSERT_TRUE(restarted.Initialize());
  EXPECT_TRUE(restarted.url_registry()->HasSite(kSiteOne));
  EXPECT_FALSE(restarted.KnownSitesIncomplete());
}

TEST_F(WorkerTest, KnownSitesAreIncompleteAfterAnUnreadableFile) {
  std::filesystem::create_directories(
      std::filesystem::path(cache_path_).parent_path());
  {
    std::ofstream f(KnownSitesFilePath(cache_path_), std::ios::trunc);
    f << "# pagespeed-sites v9\n" << kSiteOne << "\n";
  }
  Worker worker(SiteTestConfig(socket_path_, cache_path_), nullptr);
  ASSERT_TRUE(worker.Initialize());
  EXPECT_TRUE(worker.KnownSitesIncomplete());
  EXPECT_FALSE(worker.url_registry()->HasSite(kSiteOne));
}

TEST_F(WorkerTest, KnownSitesAreIncompleteAfterTheCap) {
  std::filesystem::create_directories(
      std::filesystem::path(cache_path_).parent_path());
  std::vector<std::string> full;
  full.reserve(kMaxKnownSites);
  char buf[65];
  for (size_t i = 0; i < kMaxKnownSites; ++i) {
    std::snprintf(buf, sizeof(buf), "%064zx", i + 1);
    full.emplace_back(buf);
  }
  ASSERT_TRUE(WriteKnownSitesFile(KnownSitesFilePath(cache_path_), full));
  Worker worker(SiteTestConfig(socket_path_, cache_path_), nullptr);
  ASSERT_TRUE(worker.Initialize());
  EXPECT_FALSE(worker.KnownSitesIncomplete());
  CacheSitedOriginal(worker.cache(), "/style.css", "example.com", kSiteOne,
                     "h1  {  color:  red;  }\n");
  std::thread worker_thread([&worker]() { worker.Run(); });
  WorkerStopper stopper(worker, worker_thread);
  std::this_thread::sleep_for(std::chrono::milliseconds(50));
  SendNotification(ModuleCss("http://example.com/style.css", kSiteOne));
  ASSERT_TRUE(pagespeed::test::WaitForNotificationsRetired(worker, 1,
                                                           kWorkerStallBudget));
  EXPECT_TRUE(worker.KnownSitesIncomplete());
  EXPECT_GE(worker.stats().variants_written.load(), 1u)
      << "the site past the cap still works";
}

}  // namespace
}  // namespace pagespeed
