// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

#ifndef TEST_SRC_WORKER_WORKER_TEST_FIXTURE_H_
#define TEST_SRC_WORKER_WORKER_TEST_FIXTURE_H_

// Shared fixture and helpers for the worker tests. The tests themselves live in
// the worker_*_test.cc topic files, which are all sources of the single
// //test/src/worker:worker_test target.

#include "src/worker/worker.h"

#ifdef _WIN32
#include <cstdlib>
// windows.h arrives transitively through worker.h above and defines the
// OBJECT-like macro FormatMessage -> FormatMessageA, which rewrites the token
// before name lookup runs — so an unqualified call to the inherited
// MessageHandler::FormatMessage becomes a call to the Win32 API. Qualifying or
// parenthesising the name does NOT suppress an object-like macro; only #undef
// does. Placed after the worker.h include, which is what makes it effective.
#undef FormatMessage
inline int setenv(const char* name, const char* value, int) {
  return _putenv_s(name, value);
}
inline int unsetenv(const char* name) { return _putenv_s(name, ""); }
#else
#include <fcntl.h>
#include <unistd.h>
#endif

#include <array>
#include <chrono>
#include <cstdarg>
#include <cstdint>
#include <filesystem>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "gtest/gtest.h"
#include "lib/base/message_handler.h"
#include "lib/cache/cache.h"
#include "lib/cache/freshness.h"
#include "lib/classify/alternate_id.h"
#include "lib/classify/alternate_metadata.h"
#include "lib/classify/capability_mask.h"
#include "sha256.hpp"
#include "src/browser/optimization_profile.h"
#include "src/browser/template_detector.h"
#include "src/proto/worker_ipc.h"
#include "src/worker/browser_analysis_manager.h"
#include "src/worker/cascade_layer_order.h"
#include "src/worker/critical_css_extractor.h"
#include "src/worker/html_scanner.h"
#include "src/worker/layer_order_sources.h"
#include "src/worker/shared_config.h"
#include "src/worker/subresource_gate.h"
#include "src/worker/text_compressor.h"
#include "test/test_util/pipe_client.h"
#include "test/test_util/tcp_client.h"
#include "test/test_util/temp_dir.h"
#include "test/test_util/worker_quiescence.h"

namespace pagespeed {

// Sanitizer builds run the instrumented binary far slower than a release
// build; ThreadSanitizer in particular is roughly an order of magnitude
// slower, and image transcodes (AVIF via libaom, plus the SSIMULACRA2
// verification path) sit at the worst of that curve.  The suite's wall-clock
// wait ceilings scale by this factor.  They are *ceilings* for condition
// polls that return the instant their condition holds, so a larger ceiling
// never slows a passing test -- it only keeps a genuinely slow sanitizer run
// from tripping a deadline that was sized for a release build.
// The detection lives in worker_quiescence.h (shared with the e2e tests).
inline constexpr int kSanitizerBudgetScale =
    pagespeed::test::kSanitizerBuild ? 12 : 1;

// Poll ceiling for the proactive viewport-sibling tests.  Those variants are
// produced asynchronously behind the image encoder queue, so the window has
// to be sized for the slowest lane rather than the median: on an unoptimized
// Windows fastbuild sharing a loaded CI runner, a test whose polls waited on
// the full warmup encode matrix exhausted its previous 6 s window twice in a
// row (issue #1296), while a sibling test in the same shard needed its whole
// ~30 s aggregate window to pass.  90 s matches the flat window adopted for
// the same class of flake in the e2e AVIF test (issues #276 / #307).  The
// loops sleep 100 ms per iteration, so iterations x 100 ms = wall clock:
// 900 x 100 ms = 90 s.  Deliberately NOT multiplied by kSanitizerBudgetScale
// (matching the e2e precedent, which is flat): 90 s already covers the
// sanitizer lanes' slowdown, and scaling it would let a genuinely-broken
// run burn 18 min per exhausted loop under TSan — several sequential loops
// would then hit the bazel/job timeouts and report a bare timeout instead of
// the assertion message.  This is a ceiling on a condition poll that returns
// the instant its variant appears, so widening it costs a passing run
// nothing.
inline constexpr int kViewportSiblingPollIterations = 900;

// Silence limit for WaitForNotificationsRetired (worker_quiescence.h): how
// long the worker may go without moving any progress counter before the wait
// gives up.  It is not a deadline on the work -- a notification may take as
// long as the host needs, provided the worker keeps reporting progress -- so
// it has to cover one step only: the encodes of a single variant combination
// (WebP, AVIF and the original format, each with its quality verification),
// after which a write, a quality check or a decline is counted.  The slowest
// such step measured here is the cache-full test's 150 KB JPEG: about 25 s on
// an idle machine in an unoptimized build, and about two minutes on a host
// oversubscribed four to five times.  Ten minutes leaves a factor of five
// over that.  Sanitizer builds are roughly an order of magnitude slower on
// the encoders and get 20 minutes.  The target's 60-minute timeout is per
// shard, and a shard runs about an eighth of this file's tests, so the budget
// has to leave room for the rest of the shard; at 20 minutes a stalled worker
// still reports through the wait's message rather than as a bare timeout
// (and CI's retry of a failed test doubles whatever a stall costs).
inline constexpr std::chrono::minutes kWorkerStallBudget{
    kSanitizerBudgetScale > 1 ? 20 : 10};

// Helper to read a test image file into a string.
std::string ReadTestFile(const std::string& filename);

// SHA-256 of a byte string, the same value the worker stamps into
// AlternateMetadata::origin_html_hash (issue #434).
inline std::array<std::byte, 32> Sha256Of(std::string_view content) {
  return cyclone::crypto::SHA256::hash(std::span<const std::byte>(
      reinterpret_cast<const std::byte*>(content.data()), content.size()));
}

// Helper: open a cross-platform IPC pipe connection to the worker
intptr_t ConnectToSocket(const std::string& path);

// =============================================================================
// Worker Integration Tests
// =============================================================================

// The Content-Type a front end records with an original it stores, for a test
// that names none: text/html for an HTML document (one that opens with
// `<!doctype html` or `<html`, ASCII case-insensitive, after whitespace),
// otherwise none. The worker inlines critical CSS only into a page recorded
// as text/html.
inline std::string DefaultOriginContentType(std::string_view content) {
  size_t i = 0;
  while (i < content.size() &&
         (content[i] == ' ' || content[i] == '\t' || content[i] == '\n' ||
          content[i] == '\r' || content[i] == '\f')) {
    ++i;
  }
  auto opens_with = [&](std::string_view prefix) {
    if (content.size() - i < prefix.size()) return false;
    for (size_t k = 0; k < prefix.size(); ++k) {
      char c = content[i + k];
      if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
      if (c != prefix[k]) return false;
    }
    return true;
  };
  return opens_with("<!doctype html") || opens_with("<html")
             ? "text/html; charset=utf-8"
             : "";
}

class WorkerTest : public ::testing::Test {
 protected:
  void SetUp() override {
    // Create unique temp dir for each test
    temp_dir_ = pagespeed::test::MakeTempDir();
    cache_path_ = temp_dir_ + "/cache";
#ifdef _WIN32
    // Windows Named Pipes need simple identifiers, not full filesystem paths.
    // Worker::Initialize() prepends \\.\pipe\ on Windows. Include the prefix
    // in socket_path_ so test assertions comparing against socket_path() match.
    socket_path_ = "\\\\.\\pipe\\ps_worker_" +
                   std::to_string(std::hash<std::string>{}(temp_dir_));
#else
    socket_path_ = temp_dir_ + "/worker.sock";
#endif
    // No front end answers the worker's sub-resource checks here: every
    // cached sub-resource counts as served to any client, as before the gate.
    // Tests of the gate itself install their own verdicts.
    SubresourceGate::SetVerdictOverrideForTest(
        [](const SubresourceRead&) -> std::optional<SubresourceVerdict> {
          return SubresourceVerdict::kAdmitted;
        });
  }

  void TearDown() override {
    SubresourceGate::SetVerdictOverrideForTest(nullptr);
    if (!temp_dir_.empty()) {
      std::error_code ec;
      std::filesystem::remove_all(temp_dir_, ec);
    }
  }

  // Helper: send a notification to the worker via IPC pipe.
  // Scheme must be set explicitly (v3 wire format requires "http" or "https").
  void SendNotification(const CacheNotification& notification) {
    ASSERT_FALSE(notification.scheme.empty())
        << R"(Notification scheme must be set ("http" or "https"))";
    intptr_t client_fd = pagespeed::test::ConnectPipe(socket_path_);
    ASSERT_GE(client_fd, static_cast<intptr_t>(0))
        << "Failed to connect to worker socket";

    std::vector<char> data = notification.Serialize();
    ssize_t written =
        pagespeed::test::PipeWrite(client_fd, data.data(), data.size());
    EXPECT_EQ(written, static_cast<ssize_t>(data.size()));

    pagespeed::test::ClosePipe(client_fd);
  }

  // Helper: put raw bytes on the notify socket, bypassing Serialize().
  //
  // Required for skew tests: the point of those is to present a frame no
  // in-tree writer can produce — one stamped with another peer's wire version
  // — and Serialize() by construction always stamps the current one.  Going
  // through the real socket rather than calling Deserialize directly is what
  // makes them cover the RECEIVE PATH's handling of the refusal (counters,
  // logging, non-dispatch) instead of only the codec's verdict.
  void SendRawNotificationBytes(const std::vector<char>& bytes) {
    intptr_t client_fd = pagespeed::test::ConnectPipe(socket_path_);
    ASSERT_GE(client_fd, static_cast<intptr_t>(0))
        << "Failed to connect to worker socket";
    ssize_t written =
        pagespeed::test::PipeWrite(client_fd, bytes.data(), bytes.size());
    EXPECT_EQ(written, static_cast<ssize_t>(bytes.size()));
    pagespeed::test::ClosePipe(client_fd);
  }

  // Barrier: wait until every dispatched work item has retired.
  //
  // A processing counter (js_processed / html_processed) is incremented inside
  // OnNotificationWork, but the URL is only removed from the in-flight set later,
  // by OnNotificationDone on the event loop.  Between those two points a repeat
  // notification for the same URL is rejected by the in-flight guard
  // (notifications_skipped_inflight) rather than reaching the processed-variant
  // dedup or the cooldown check.  A test that acts the moment a counter moves is
  // racing the event loop, and loses that race on a loaded machine.
  //
  // OnNotificationDone erases the in-flight entry BEFORE decrementing
  // in_flight_work_, so observing zero here means the entry is already gone.
  void WaitForWorkerIdle(Worker& worker) {
    // Generous cap: this returns the instant the worker is idle, but image
    // tests with proactive variants can legitimately keep the pool busy for
    // tens of seconds on a loaded runner -- and a sanitizer build stretches
    // that by kSanitizerBudgetScale (TSan's ~10x instrumentation cost lands
    // squarely on the image transcode path), so the cap scales with it.
    const int kMaxPolls = 6000 * kSanitizerBudgetScale;
    for (int i = 0; i < kMaxPolls; ++i) {
      if (worker.in_flight_work() == 0) return;
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    ADD_FAILURE() << "worker did not drain in-flight work within "
                  << (kMaxPolls / 100) << "s";
  }

  // Barrier: wait for the notification this test sent to retire (the
  // worker has consumed it and every work item it dispatched has finished),
  // then check that at least one variant was written.
  //
  // A bare WaitForWorkerIdle races worker STARTUP: between SendNotification's
  // pipe write and DispatchNotification registering the work item on the
  // event loop, in_flight_work() still reads 0, so it can return before the
  // notification was even consumed.  WaitForNotificationsRetired also
  // requires notifications_received >= 1, which closes that window.
  //
  // The wait is progress-based, not a wall-clock budget: it gives up only
  // after kWorkerStallBudget without any worker counter moving, so a slow
  // host that keeps making progress is never cut off (issue #1756).
  //
  // Only usable in tests whose expected outcome includes at least one
  // successful variant write; a pipeline that writes nothing fails the
  // EXPECT_GE below.  Callers wrap it in ASSERT_NO_FATAL_FAILURE.
  void WaitForNotificationRetiredWithVariant(Worker& worker) {
    ASSERT_TRUE(pagespeed::test::WaitForNotificationsRetired(
        worker, 1, kWorkerStallBudget));
    EXPECT_GE(worker.stats().variants_written.load(std::memory_order_relaxed),
              1u)
        << "notification retired without a variant write";
  }

  // Strip scheme+authority from a URL, matching what the worker's
  // NormalizeCacheUrl does.  "http://example.com/path" → "/path".
  // When hostname is empty (page-level operations), the worker uses
  // path-only keys.  When hostname is set (external CSS refs), the
  // worker resolves absolute hrefs as-is.
 public:
  static std::string StripSchemeAuthority(const std::string& url) {
    auto scheme_end = url.find("://");
    if (scheme_end == std::string::npos) return url;
    auto first_slash = url.find('/');
    if (first_slash != std::string::npos && scheme_end < first_slash) {
      auto authority_end = url.find('/', scheme_end + 3);
      if (authority_end != std::string::npos) return url.substr(authority_end);
      return "/";
    }
    return url;
  }

 protected:
  // Helper: pre-populate cache with content using the PageSpeedCache API.
  // Writes as a regular alternate at the default mask (Desktop/Identity = 0x08)
  // so ReadBestAlternate can find it via the PageSpeedSelector.
  // URL is normalized to path-only (matching worker's NormalizeCacheUrl).
  void CacheOriginal(PageSpeedCache* cache, const std::string& url,
                     const std::string& content,
                     const std::string& hostname = "", uint8_t flags = 0,
                     const std::array<std::byte, 32>* source_binding = nullptr,
                     const std::string& origin_content_type = "") {
    std::string effective_url = StripSchemeAuthority(url);
    CapabilityMask default_mask;  // Desktop/Identity = 0x08
    AlternateId id =
        MaskToAlternateId(static_cast<uint8_t>(default_mask.Encode() & 0xFF));
    AlternateMetadata meta;
    meta.full_mask = default_mask.Encode();
    meta.content_type = ContentType::kOther;
    meta.origin_content_type = origin_content_type.empty()
                                   ? DefaultOriginContentType(content)
                                   : origin_content_type;
    meta.flags = flags;
    if (source_binding != nullptr) {
      meta.origin_html_hash = *source_binding;
    }
    auto wh = cache->WriteAlternate(effective_url, hostname, "https", id,
                                    content.size(), meta);
    ASSERT_TRUE(wh.has_value()) << "Failed to open write handle";
    auto written = wh->write_sync(
        std::as_bytes(std::span(content.data(), content.size())));
    ASSERT_TRUE(written.has_value()) << "Failed to write content";
    auto closed = wh->close_sync();
    ASSERT_TRUE(closed.has_value()) << "Failed to close write handle";
  }

  // Helper: leave the URL holding a durable ORIGINAL and NOTHING ELSE — the
  // cache shape an external writer produces, and the shape the source fallback
  // exists for.  Deliberately not CacheOriginal's default-mask alternate: this
  // writes the durable-original class (SentinelId::kOriginalContent, 0x0C),
  // which ordinary selection can never return.
  //
  // Calls the C++ entry point, NOT the published C one, and the distinction is
  // worth being exact about rather than glossed: ps_cache_write_original is a
  // thin shim over this same WriteOriginalAlternate — it converts the caller's
  // params (flags included, verbatim) and stamps the class id, then calls it
  // (lib/pagespeed/pagespeed.cc).  So what this produces is byte-identical to
  // what the ABI produces, but that is an argument, not a proof.  The proof is
  // split deliberately and neither half is redundant:
  //
  //   * lib/pagespeed/pagespeed_test.cc drives the REAL ABI end-to-end and
  //     asserts the marker survives into the stored entry;
  //   * these tests assert what the worker does with such an entry;
  //   * a static_assert in pagespeed.cc pins PS_FLAG_ORIGIN_VARIES_ACCEPT to
  //     AlternateMetadata::kFlagOriginVariesAccept, so the two halves cannot
  //     be talking about different bits.
  //
  // The worker suite cannot close that loop by itself: lib/pagespeed's test
  // has no worker dependency (and inverting that to give it one would point
  // lib/ at src/worker/).
  //
  // Origin state is stamped the way a front end that just fetched from the
  // origin stamps it, because the class's lifetime is bound to it.
  void CacheDurableOriginal(PageSpeedCache* cache, const std::string& url,
                            const std::string& content,
                            const std::string& origin_content_type,
                            ContentType content_type = ContentType::kOther,
                            uint32_t cache_inserted_at = 1'700'000'000,
                            uint32_t origin_max_age = 600,
                            const std::string& hostname = "",
                            uint8_t flags = 0) {
    std::string effective_url = StripSchemeAuthority(url);
    AlternateMetadata meta;
    meta.full_mask = static_cast<uint32_t>(SentinelId::kOriginalContent);
    meta.content_type = content_type;
    meta.origin_content_type = origin_content_type;
    meta.cache_inserted_at = cache_inserted_at;
    meta.origin_max_age = origin_max_age;
    meta.origin_cc_flags = AlternateMetadata::kCCOriginHeaderPresent |
                           AlternateMetadata::kCCOriginPublic;
    meta.flags = flags;
    auto wh = cache->WriteOriginalAlternate(effective_url, hostname, "https",
                                            content.size(), meta);
    ASSERT_TRUE(wh.has_value()) << "Failed to open durable-original handle";
    auto written = wh->write_sync(
        std::as_bytes(std::span(content.data(), content.size())));
    ASSERT_TRUE(written.has_value()) << "Failed to write durable original";
    ASSERT_TRUE(wh->close_sync().has_value())
        << "Failed to close durable original";
  }

  // Helper: read a specific variant from cache by its exact mask ID.
  // URL is normalized to path-only to match worker's NormalizeCacheUrl.
  std::string ReadVariant(PageSpeedCache* cache, const std::string& url,
                          const CapabilityMask& mask,
                          const std::string& hostname = "") {
    std::string effective_url = StripSchemeAuthority(url);
    AlternateId id =
        MaskToAlternateId(static_cast<uint8_t>(mask.Encode() & 0xFF));
    auto result = cache->ReadAlternate(effective_url, hostname, "https", id);
    if (!result.has_value()) return "";
    auto content = result->content();
    return std::string(reinterpret_cast<const char*>(content.data()),
                       content.size());
  }

  // Helper: read the best variant for a mask using selector fallback.
  // Unlike ReadVariant (exact ID), this finds the best-matching alternate
  // via PageSpeedSelector scoring, handling deduped variants gracefully.
  // URL is normalized to path-only to match worker's NormalizeCacheUrl.
  std::string ReadBestVariant(PageSpeedCache* cache, const std::string& url,
                              const CapabilityMask& mask,
                              const std::string& hostname = "") {
    std::string effective_url = StripSchemeAuthority(url);
    auto result =
        cache->ReadBestAlternate(effective_url, hostname, "https", mask);
    if (!result.has_value()) return "";
    auto content = result->content();
    return std::string(reinterpret_cast<const char*>(content.data()),
                       content.size());
  }

  // Helper: read a specific variant and its metadata.
  struct VariantWithMeta {
    std::string content;
    AlternateMetadata metadata;
  };
  std::optional<VariantWithMeta> ReadVariantWithMeta(
      PageSpeedCache* cache, const std::string& url, const CapabilityMask& mask,
      const std::string& hostname = "") {
    std::string effective_url = StripSchemeAuthority(url);
    AlternateId id =
        MaskToAlternateId(static_cast<uint8_t>(mask.Encode() & 0xFF));
    auto result = cache->ReadAlternate(effective_url, hostname, "https", id);
    if (!result.has_value()) return std::nullopt;
    auto content = result->content();
    return VariantWithMeta{
        std::string(reinterpret_cast<const char*>(content.data()),
                    content.size()),
        result->metadata};
  }

  // Issue #434 source-binding property checker: walk the URL's whole
  // alternate family and assert that every WORKER-WRITTEN variant (flags
  // carry kFlagWorkerProcessed) carries |expected| in
  // metadata.origin_html_hash.  The front end's own recording (flag clear)
  // keeps its all-zero binding by design: it is not a worker variant, and
  // the serve path falls to the origin validators or emits no ETag for it.
  // Sentinels (IsSentinel) are not variants and are skipped.  Counts the
  // worker-written variants into *worker_written so callers can assert the
  // check was non-vacuous.
  void CheckSourceBinding(PageSpeedCache* cache, const std::string& url,
                          const std::array<std::byte, 32>& expected,
                          size_t* worker_written,
                          const std::string& hostname = "") {
    *worker_written = 0;
    std::string effective_url = StripSchemeAuthority(url);
    auto alts = cache->ListAlternates(effective_url, hostname, "https");
    ASSERT_TRUE(alts.has_value()) << "no alternates listed for " << url;
    for (const auto& alt : *alts) {
      if (IsSentinel(static_cast<AlternateId>(alt.id))) continue;
      auto rr = cache->ReadAlternate(effective_url, hostname, "https",
                                     static_cast<AlternateId>(alt.id));
      ASSERT_TRUE(rr.has_value())
          << "alternate 0x" << std::hex << static_cast<int>(alt.id)
          << " listed but unreadable";
      if ((rr->metadata.flags & AlternateMetadata::kFlagWorkerProcessed) == 0) {
        continue;  // the front end's own recording, not a worker variant
      }
      ++*worker_written;
      EXPECT_EQ(rr->metadata.origin_html_hash, expected)
          << "worker variant 0x" << std::hex << static_cast<int>(alt.id)
          << " of " << url << " does not carry the expected source binding";
    }
  }

  // Helper: read a sentinel entry from cache.
  // URL is normalized to path-only to match worker's NormalizeCacheUrl.
  std::string ReadSentinel(PageSpeedCache* cache, const std::string& url,
                           SentinelId sentinel,
                           const std::string& hostname = "") {
    std::string effective_url = StripSchemeAuthority(url);
    auto result = cache->ReadAlternate(effective_url, hostname, "https",
                                       static_cast<AlternateId>(sentinel));
    if (!result.has_value()) return "";
    auto content = result->content();
    return std::string(reinterpret_cast<const char*>(content.data()),
                       content.size());
  }

  // Seed a browser profile for `html`'s template, carrying (or withholding) the
  // validation record the async-CSS deferral gate requires.
  //
  // All three viewports get a record, so a caller does not have to mirror
  // the capability-mask -> viewport mapping to know which one the worker will
  // select. Each record's binding carries its own class's salts
  // (ValidationBindingFor: "tablet-mobile" for the tablet since the
  // tablet-as-mobile change,
  // "touch" and "ua" for the mobile and tablet since touch emulation and
  // User-Agent emulation), so it
  // matches whichever class the test requests with. `validated_against_css`
  // is the COMBINED stylesheet the validation is claimed to have been made
  // against; pass the bytes the worker will actually assemble to authorize
  // deferral, or different bytes to model a stylesheet-only redeploy.
  //
  // `validated_hash_override`, when engaged, writes that string into
  // validated_combined_css_hash instead of hashing anything. It exists to reach
  // records the hash function CANNOT produce — an empty stored hash, or the
  // digest of an empty sheet — which is exactly where a dropped guard would let
  // "validated against nothing" satisfy the gate.
  void StoreBrowserProfile(
      Worker& worker, const std::string& notification_url,
      const std::string& html, std::string_view profile_critical_css,
      bool validated, std::string_view validated_against_css,
      std::optional<std::string> validated_hash_override = std::nullopt,
      bool made_without_noscript = false) {
    HtmlScanner scanner;
    HtmlScanResult scan = scanner.Scan(notification_url, html);
    ASSERT_TRUE(scan.success);

    auto record_for = [&](CapabilityMask::Viewport viewport) {
      ViewportProfile vp;
      vp.critical_css = std::string(profile_critical_css);
      // Under the 0.60 inline budget, so ShouldInlineCriticalCss keeps the
      // block.
      vp.css_coverage_ratio = 0.2f;
      vp.total_css_bytes = profile_critical_css.size() * 5;
      vp.unused_css_bytes = vp.total_css_bytes - profile_critical_css.size();
      vp.critical_css_validated = validated;
      vp.validation_diff_ratio = validated ? 0.001f : -1.0f;
      if (validated) {
        vp.validated_critical_css_hash =
            CombinedCssValidationHash(profile_critical_css);
        // Bound to the page's layer order, as a real record is.
        // The seeded sheets declare no layers, so the order only
        // depends on which sources the page has: compute it with every
        // gathered sheet empty.
        std::vector<std::optional<GatheredSheet>> gathered(
            scan.stylesheets.size(), GatheredSheet{});
        const CascadeLayerOrder order = ComputeCascadeLayerOrder(
            BuildLayerOrderSources(scan.stylesheet_sources, notification_url,
                                   gathered),
            {});
        // `made_without_noscript`: a record made since the <noscript> removal,
        // by
        // renders that leave <noscript> content out, carries that page's
        // salt. Either way the record carries its class's salts (an empty
        // sheet adds no other): it is a record for THIS viewport.
        const std::string layer_binding =
            order.ValidationBinding(validated_against_css);
        vp.validated_combined_css_hash = CombinedCssValidationHash(
            validated_against_css,
            made_without_noscript
                ? ValidationBindingFor(layer_binding,
                                       scan.noscript_affects_render,
                                       validated_against_css, viewport,
                                       /*anonymous_layers_dropped=*/false)
                : ValidationBindingFor(layer_binding,
                                       /*noscript_affects_render=*/false,
                                       /*combined_css=*/"", viewport,
                                       /*anonymous_layers_dropped=*/false));
      }
      if (validated_hash_override.has_value()) {
        vp.validated_combined_css_hash = *validated_hash_override;
      }
      return vp;
    };

    OptimizationProfile profile;
    profile.template_hash_hex = "seeded";
    profile.analyzed_url = notification_url;
    profile.mobile = record_for(CapabilityMask::Viewport::kMobile);
    profile.tablet = record_for(CapabilityMask::Viewport::kTablet);
    profile.desktop = record_for(CapabilityMask::Viewport::kDesktop);
    profile.created_at = 0;
    profile.expires_at = 0;  // no TTL — LookupProfile keeps it

    ASSERT_NE(worker.TestBrowserManager(), nullptr)
        << "browser manager must be constructed to seed a profile";
    worker.TestBrowserManager()->TestStoreProfile(
        TemplateDetector::HashStructure(scan), profile);
  }

  // A worker config that constructs the BrowserAnalysisManager (so a profile
  // can be seeded) without ever starting Chrome.
  WorkerConfig BrowserProfileConfig() {
    WorkerConfig config;
    config.socket_path = socket_path_;
    config.cache_path = cache_path_;
    config.cache_size_bytes = static_cast<uint64_t>(10 * 1024 * 1024);
    // The test binary runs as root in the build container,
    // where the Chrome sandbox is (correctly) unavailable and `require` would
    // refuse to construct the manager.  These tests seed a profile directly
    // and never spawn Chrome, so take the documented opt-out explicitly.
    config.browser_analysis.sandbox_mode = BrowserSandboxMode::kOff;
    config.browser_analysis.enabled = true;
    config.browser_analysis.chrome_binary = "/nonexistent/chrome";
    return config;
  }

  std::string temp_dir_;
  std::string socket_path_;
  std::string cache_path_;
};

// Stops the worker and joins its thread on scope exit.
//
// Load-bearing wherever a test uses a FATAL assertion (ASSERT_*) after Run()
// has been started: a fatal assertion returns from the test body immediately,
// so a trailing Shutdown()/join() never executes and a still-joinable
// std::thread is destroyed — which calls std::terminate and takes the whole
// binary, and every test after it, down with one failed expectation. With this,
// a failing assertion reports as a failing assertion.
//
// This is the file's single guard idiom (#1352, consolidated in #1354): every
// test that starts a worker thread and can hit a fatal assert afterwards
// declares one immediately after the thread start, and new tests do the same —
// there is deliberately no second guard class, so the absence of a
// WorkerStopper at a post-start fatal assert means something. The one
// sanctioned exception is GracefulShutdownDrainsConnections, whose teardown is
// the behavior under test; it carries an explicit pre-assert cleanup instead.
class WorkerStopper {
 public:
  WorkerStopper(Worker& worker, std::thread& thread)
      : worker_(worker), thread_(thread) {}
  WorkerStopper(const WorkerStopper&) = delete;
  WorkerStopper& operator=(const WorkerStopper&) = delete;
  ~WorkerStopper() {
    worker_.Shutdown();
    thread_.join();
  }

 private:
  Worker& worker_;
  std::thread& thread_;
};

// A MessageHandler that keeps the WARNINGs (and INFOs) it was given.  Shared by
// the skew tests below, all of which assert on the "and it was said out loud"
// half rather than only on the behaviour.
class WarningRecordingHandler : public MessageHandler {
 public:
  void Message(MessageType type, const char* format, ...) override {
    va_list args;
    va_start(args, format);
    MessageV(type, format, args);
    va_end(args);
  }
  std::vector<std::string> warnings() const {
    std::lock_guard<std::mutex> lock(mu_);
    return warnings_;
  }
  std::vector<std::string> infos() const {
    std::lock_guard<std::mutex> lock(mu_);
    return infos_;
  }

 protected:
  void MessageV(MessageType type, const char* format, va_list args) override {
    std::string formatted = FormatMessage(format, args);
    std::lock_guard<std::mutex> lock(mu_);
    if (type == MessageType::kWarning) warnings_.push_back(formatted);
    if (type == MessageType::kInfo) infos_.push_back(formatted);
  }

 private:
  mutable std::mutex mu_;
  std::vector<std::string> warnings_;
  std::vector<std::string> infos_;
};

// Reads a one-shot mgmt response to EOF (the server closes the connection
// after writing non-AUTH responses).  A single PipeRead can return just the
// first pipe-buffer chunk, which silently truncates the merged Prometheus
// document (#877) and makes find()-based assertions position-dependent.
//
// Defined here, above the first mgmt test, because that truncation is no
// longer a large-response edge case: the Prometheus document is past 4 KiB and
// grows by three lines with every counter the build gains, so a fixed-buffer
// single read now cuts it in the ordinary case — and the failure names
// whichever metric happened to fall past the cut rather than the real cause.
std::string ReadMgmtResponseToEof(intptr_t fd);

// Helper: pre-populate cache with content and custom metadata.
// Unlike CacheOriginal(), this lets callers set origin_content_type_str
// and other metadata fields for testing image type detection.
// URL is normalized to path-only to match worker's NormalizeCacheUrl.
void CacheOriginalWithMeta(PageSpeedCache* cache, const std::string& url,
                           const std::string& content,
                           const AlternateMetadata& meta_in);

// =============================================================================
// An origin file rewritten in place, recorded by a PEER process, must replace
// the worker's stored gzip/brotli copies.
//
// The front end is a SECOND cache handle on the same volume: a same-handle
// write evicts its own RAM copy and would hide the worker's stale RAM read of
// the durable original.  The worker keeps its RAM tier on (the default).
// =============================================================================

class WorkerPeerRewriteTest : public WorkerTest {
 protected:
  static constexpr uint32_t kStamp1 = 1'700'000'000;
  static constexpr uint32_t kStamp2 = 1'700'000'005;

  // Waits for the worker to drain every notification sent so far.
  void NotifySvgAndDrain(Worker& worker, const std::string& url) {
    CacheNotification notification;
    notification.url = url;
    notification.scheme = "https";
    notification.content_type = ContentType::kImage;
    notification.capability_mask = CapabilityMask().Encode();
    SendNotification(notification);
    WaitForWorkerIdle(worker);
  }

  // Polls (bounded) until `counter` reaches `target`.
  static bool WaitForAtLeast(const std::atomic<uint64_t>& counter,
                             uint64_t target) {
    for (int i = 0; i < 60 * kSanitizerBudgetScale; ++i) {
      if (counter.load() >= target) return true;
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    return counter.load() >= target;
  }

  std::string SvgBody(const std::string& color) {
    return R"(<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 100 100">)"
           R"(<circle cx="50" cy="50" r="40" fill=")" +
           color + R"("/></svg>)";
  }

  // `rotate_dedup_set`: shrink the processed set to one entry and notify other
  // SVGs between the rewrite and the second notification, so the dedup key is
  // gone and only the stale source read can serve the old bytes.
  void RunScenario(bool rotate_dedup_set, bool same_bytes = false) {
    WorkerConfig config;
    config.socket_path = socket_path_;
    config.cache_path = cache_path_;
    config.cache_size_bytes = static_cast<uint64_t>(10 * 1024 * 1024);
    config.proactive_image_variants = false;
    config.proactive_viewport_variants = false;
    config.proactive_savedata_variants = false;
    config.proactive_density_variants = false;
    if (rotate_dedup_set) config.max_processed_entries = 1;

    NullMessageHandler handler;
    Worker worker(config, &handler);
    ASSERT_TRUE(worker.Initialize());

    // The front end: a second handle onto the worker's volume.
    PageSpeedCacheConfig fe_config;
    fe_config.volume_path = cache_path_;
    fe_config.volume_size = static_cast<uint64_t>(10 * 1024 * 1024);
    auto front_end = PageSpeedCache::Create(fe_config);
    ASSERT_TRUE(front_end.has_value());

    const std::string url = "http://example.com/images/logo.svg";
    const std::string v1 = SvgBody("blue");
    const std::string v2 = same_bytes ? v1 : SvgBody("red");
    ASSERT_EQ(v1 == v2, same_bytes);
    CacheDurableOriginal(front_end->get(), url, v1, "image/svg+xml",
                         ContentType::kImage, kStamp1, 600);

    std::thread worker_thread([&worker]() { worker.Run(); });
    WorkerStopper stopper(worker, worker_thread);
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    NotifySvgAndDrain(worker, url);
    for (int i = 0; i < 60 * kSanitizerBudgetScale; ++i) {
      if (worker.stats().gzip_variants_written.load() >= 1) break;
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    ASSERT_GE(worker.stats().gzip_variants_written.load(), 1u);
    WaitForWorkerIdle(worker);

    // A reader with no RAM tier sees what is on disk.
    PageSpeedCacheConfig reader_config = fe_config;
    reader_config.ram_cache_size = 0;
    auto reader = PageSpeedCache::Create(reader_config);
    ASSERT_TRUE(reader.has_value());
    CapabilityMask gz_mask;
    gz_mask.set_transfer_encoding(CapabilityMask::TransferEncoding::kGzip);
    CapabilityMask br_mask;
    br_mask.set_transfer_encoding(CapabilityMask::TransferEncoding::kBrotli);
    ASSERT_EQ(GzipDecompress(ReadVariant(reader->get(), url, gz_mask)), v1);
    ASSERT_EQ(BrotliDecompress(ReadVariant(reader->get(), url, br_mask)), v1);

    // The front end re-records the origin file with NEW bytes and a newer
    // stamp, then (as every re-record does) sends the plain notification.
    CacheDurableOriginal(front_end->get(), url, v2, "image/svg+xml",
                         ContentType::kImage, kStamp2, 600);
    if (rotate_dedup_set) {
      for (int i = 0; i < 3; ++i) {
        const std::string other =
            "http://example.com/images/other" + std::to_string(i) + ".svg";
        CacheDurableOriginal(front_end->get(), other, SvgBody("green"),
                             "image/svg+xml", ContentType::kImage, kStamp1,
                             600);
        const uint64_t before = worker.stats().gzip_variants_written.load();
        NotifySvgAndDrain(worker, other);
        // Processed for real, so the one-entry set has rotated.
        ASSERT_TRUE(
            WaitForAtLeast(worker.stats().gzip_variants_written, before + 1));
      }
    }
    const uint64_t gz_before = worker.stats().gzip_variants_written.load();
    NotifySvgAndDrain(worker, url);
    if (same_bytes) {
      for (int i = 0; i < 40 * kSanitizerBudgetScale; ++i) {
        if (worker.stats().origin_unchanged_restamped.load() >= 1) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
      }
      WaitForWorkerIdle(worker);
      // Same bytes re-recorded: freshness is adopted, nothing is recompressed.
      EXPECT_EQ(worker.stats().gzip_variants_written.load(), gz_before);
      EXPECT_EQ(worker.stats().notifications_origin_rechecked.load(), 1u);
      EXPECT_EQ(worker.stats().origin_unchanged_restamped.load(), 1u);
      EXPECT_EQ(worker.stats().origin_change_rebuilt.load(), 0u);
      auto gz = reader->get()->ReadAlternate(
          "/images/logo.svg", "", "https",
          MaskToAlternateId(static_cast<uint8_t>(gz_mask.Encode() & 0xFF)));
      ASSERT_TRUE(gz.has_value());
      EXPECT_EQ(gz->metadata.cache_inserted_at, kStamp2);
      return;
    }
    // Give an asynchronous rebuild time to land before judging.
    const std::string& want_gz = v2;
    for (int i = 0; i < 20 * kSanitizerBudgetScale; ++i) {
      if (GzipDecompress(ReadVariant(reader->get(), url, gz_mask)) == want_gz &&
          BrotliDecompress(ReadVariant(reader->get(), url, br_mask)) == v2) {
        break;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    if (!rotate_dedup_set) {
      EXPECT_EQ(worker.stats().origin_change_rebuilt.load(), 1u);
      // The rebuild's own writes were not fenced by the targeted purge's
      // generation bump (the generation is re-read after the purge).
      EXPECT_EQ(worker.stats().alternate_writes_fenced.load(), 0u);
      EXPECT_GT(worker.stats().gzip_variants_written.load(), gz_before);
    } else {
      // The processed-set entry was gone: the ordinary pass rebuilt from a
      // freshly read original, with no recheck involved.
      EXPECT_EQ(worker.stats().notifications_origin_rechecked.load(), 0u);
    }
    EXPECT_EQ(GzipDecompress(ReadVariant(reader->get(), url, gz_mask)), v2)
        << "the gzip copy still holds the previous origin bytes";
    EXPECT_EQ(BrotliDecompress(ReadVariant(reader->get(), url, br_mask)), v2)
        << "the brotli copy still holds the previous origin bytes";
  }
};

// Shared rig for the recheck-policy tests: one SVG built from v1, a front end
// on a second handle, and a RAM-less reader.
struct PeerRig {
  NullMessageHandler handler;
  std::unique_ptr<Worker> worker;
  std::unique_ptr<PageSpeedCache> front_end;
  std::unique_ptr<PageSpeedCache> reader;
  std::thread thread;
  std::string url = "http://example.com/images/logo.svg";
  ~PeerRig() {
    if (worker) worker->Shutdown();
    if (thread.joinable()) thread.join();
  }
};

}  // namespace pagespeed

#endif  // TEST_SRC_WORKER_WORKER_TEST_FIXTURE_H_
