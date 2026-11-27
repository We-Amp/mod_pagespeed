// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

// Critical CSS and async-CSS tests
//
// - Critical CSS injection from HTML notifications
// - The async-CSS empirical gate and revalidation
// - Missing external stylesheets and the measured fold
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
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <thread>
#include <unordered_set>
#include <vector>

#include "absl/strings/str_cat.h"
#include "gtest/gtest.h"
#include "lib/cache/cache.h"
#include "lib/classify/alternate_id.h"
#include "lib/classify/alternate_metadata.h"
#include "lib/classify/capability_mask.h"
#include "src/browser/optimization_profile.h"
#include "src/browser/template_detector.h"
#include "src/proto/worker_ipc.h"
#include "src/worker/browser_analysis_manager.h"
#include "src/worker/critical_css_extractor.h"
#include "src/worker/html_scanner.h"
#include "src/worker/url_registry.h"
#include "src/worker/worker.h"
#include "test/test_util/worker_quiescence.h"

namespace pagespeed {
namespace {

TEST_F(WorkerTest, HtmlNotificationInjectsCriticalCss) {
  // HTML critical CSS injection: the worker reads original HTML,
  // extracts critical CSS, injects it as a <style> tag, and writes
  // the modified HTML as a variant.
  WorkerConfig config;
  config.socket_path = socket_path_;
  config.cache_path = cache_path_;
  config.cache_size_bytes = static_cast<uint64_t>(10 * 1024 * 1024);

  config.unsafe_force_async_css =
      true;  // fixture page is not under test for the gate
  Worker worker(config, nullptr);
  ASSERT_TRUE(worker.Initialize());

  // Pre-populate cache with HTML content containing inline CSS.
  // Use path-only URL for cache operations (the worker normalizes URLs
  // by stripping scheme+authority via NormalizeCacheUrl).
  std::string html =
      "<html><head><style>body { margin: 0; } "
      "h1 { color: red; }</style></head>"
      "<body><h1>Hello World</h1></body></html>";
  std::string cache_url = "/test.html";
  std::string notification_url = "http://example.com/test.html";
  CacheOriginal(worker.cache(), cache_url, html);

  // Run worker in background
  std::thread worker_thread([&worker]() { worker.Run(); });
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  // Send notification
  CacheNotification notification;
  notification.url = notification_url;
  notification.scheme = "https";
  notification.content_type = ContentType::kHtml;
  notification.capability_mask = 0x00000001;  // WebP capability
  SendNotification(notification);

  // Poll for variant to appear.  The worker strips image-format bits
  // for text content types, so the variant is written at the
  // format-stripped mask (Original) regardless of the notification's
  // WebP format bits.
  CapabilityMask mask = CapabilityMask::Decode(0x00000001);
  mask.set_image_format(CapabilityMask::ImageFormat::kOriginal);
  std::string variant;
  for (int i = 0; i < 40 * kSanitizerBudgetScale; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    variant = ReadVariant(worker.cache(), cache_url, mask);
    if (!variant.empty()) break;
  }

  // Verify a variant was written with injected critical CSS
  EXPECT_FALSE(variant.empty()) << "HTML variant not written";
  if (!variant.empty()) {
    // The variant should contain the pagespeed-critical style tag
    EXPECT_NE(variant.find("data-pagespeed-critical"), std::string::npos);
    // The variant should still be valid HTML with original content
    EXPECT_NE(variant.find("Hello World"), std::string::npos);
    EXPECT_NE(variant.find("</head>"), std::string::npos);
    // The variant should be larger than the original (CSS injected)
    EXPECT_GT(variant.size(), html.size());
  }

  // Verify original HTML is still intact at the default mask
  CapabilityMask default_mask;
  std::string cached = ReadVariant(worker.cache(), cache_url, default_mask);
  EXPECT_EQ(cached, html);

  worker.Shutdown();
  worker_thread.join();
}

// A Tailwind CSS v4 page keeps its critical CSS on the worker path: its
// `@property` syntax strings such as "<color>" and `content` strings holding
// `<` are written with the CSS escape `\3c `, and the block holds no `<` that
// opens markup.
TEST_F(WorkerTest, TailwindV4CriticalCssIsInlined) {
  WorkerConfig config;
  config.socket_path = socket_path_;
  config.cache_path = cache_path_;
  config.cache_size_bytes = static_cast<uint64_t>(10 * 1024 * 1024);
  config.unsafe_force_async_css = true;
  Worker worker(config, nullptr);
  ASSERT_TRUE(worker.Initialize());

  const std::string html =
      "<html><head><style>"
      "@layer theme,base,components,utilities;"
      "@layer utilities{"
      ".content-\\[\\'\\<\\'\\]{--tw-content:'<';"
      "content:var(--tw-content)}"
      ".text-red-500{color:red;&:hover{color:blue}}"
      ".bg-linear-to-r{background-image:linear-gradient("
      "var(--tw-gradient-from),red)}"
      ".max-md\\:hidden{@media (width<48rem){display:none}}}"
      "@property --tw-gradient-from{syntax:\"<color>\";inherits:false;"
      "initial-value:#0000}"
      "@property --tw-content{syntax:\"*\";inherits:false;"
      "initial-value:\"\"}</style></head>"
      "<body><h1 class=\"text-red-500 bg-linear-to-r\">Hello World</h1>"
      "<p class=\"content-['<']\">x</p></body></html>";
  const std::string cache_url = "/tailwind.html";
  CacheOriginal(worker.cache(), cache_url, html);

  std::thread worker_thread([&worker]() { worker.Run(); });
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  CacheNotification notification;
  notification.url = "http://example.com" + cache_url;
  notification.scheme = "https";
  notification.content_type = ContentType::kHtml;
  notification.capability_mask = 0x00000001;
  SendNotification(notification);

  CapabilityMask mask = CapabilityMask::Decode(0x00000001);
  mask.set_image_format(CapabilityMask::ImageFormat::kOriginal);
  std::string variant;
  for (int i = 0; i < 40 * kSanitizerBudgetScale; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    variant = ReadVariant(worker.cache(), cache_url, mask);
    if (!variant.empty()) break;
  }
  ASSERT_NE(variant.find("data-pagespeed-critical"), std::string::npos)
      << variant;
  const size_t open = variant.find("data-pagespeed-critical");
  const size_t text = variant.find('>', open);
  const size_t end = variant.find("</style>", text);
  ASSERT_NE(end, std::string::npos) << variant;
  const std::string block = variant.substr(text + 1, end - text - 1);
  EXPECT_NE(block.find("syntax:\"\\3c color>\""), std::string::npos) << block;
  EXPECT_NE(block.find(".text-red-500"), std::string::npos) << block;
  for (size_t i = 0; i + 1 < block.size(); ++i) {
    if (block[i] != '<') continue;
    const char next = block[i + 1];
    EXPECT_FALSE((next >= 'a' && next <= 'z') || (next >= 'A' && next <= 'Z') ||
                 next == '/' || next == '!' || next == '?')
        << block;
  }

  worker.Shutdown();
  worker_thread.join();
}

// The worker inlines critical CSS only into a page the origin served as
// text/html: an XHTML page (application/xhtml+xml, which the front ends file
// as HTML too) or one recorded without a Content-Type gets none, and its
// stylesheet stays render-blocking.
TEST_F(WorkerTest, CriticalCssOnlyForPagesServedAsTextHtml) {
  const std::string page =
      "<html><head><style>body { margin: 0; } "
      "h1 { color: red; }</style></head>"
      "<body><h1>Hello World</h1></body></html>";
  // The fixture records text/html for a body that opens with <html> when the
  // test names no type; a leading comment keeps the empty type empty.
  const std::string untyped_page = "<!-- untyped -->" + page;
  struct Case {
    const char* origin_content_type;
    const std::string* html;
    bool expect_block;
  };
  const Case cases[] = {
      {"text/html; charset=utf-8", &page, true},
      {"application/xhtml+xml", &page, false},
      {"application/xhtml+xml; charset=utf-8", &page, false},
      {"", &untyped_page, false},
  };
  WorkerConfig config;
  config.socket_path = socket_path_;
  config.cache_path = cache_path_;
  config.cache_size_bytes = static_cast<uint64_t>(10 * 1024 * 1024);
  config.unsafe_force_async_css = true;
  Worker worker(config, nullptr);
  ASSERT_TRUE(worker.Initialize());
  for (size_t n = 0; n < std::size(cases); ++n) {
    CacheOriginal(worker.cache(), "/page" + std::to_string(n) + ".html",
                  *cases[n].html, "", 0, nullptr, cases[n].origin_content_type);
  }

  std::thread worker_thread([&worker]() { worker.Run(); });
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  CapabilityMask mask = CapabilityMask::Decode(0x00000001);
  mask.set_image_format(CapabilityMask::ImageFormat::kOriginal);
  for (size_t n = 0; n < std::size(cases); ++n) {
    const Case& c = cases[n];
    SCOPED_TRACE(c.origin_content_type);
    const std::string cache_url = "/page" + std::to_string(n) + ".html";
    CacheNotification notification;
    notification.url = "http://example.com" + cache_url;
    notification.scheme = "https";
    notification.content_type = ContentType::kHtml;
    notification.capability_mask = 0x00000001;
    SendNotification(notification);

    std::string variant;
    for (int i = 0; i < 40 * kSanitizerBudgetScale; ++i) {
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
      variant = ReadVariant(worker.cache(), cache_url, mask);
      if (!variant.empty()) break;
      if (!c.expect_block && i >= 20) break;  // may write nothing at all
    }
    if (c.expect_block) {
      ASSERT_FALSE(variant.empty()) << "HTML variant not written";
      EXPECT_NE(variant.find("data-pagespeed-critical"), std::string::npos)
          << variant;
    } else {
      EXPECT_EQ(variant.find("data-pagespeed-critical"), std::string::npos)
          << variant;
    }
  }

  worker.Shutdown();
  worker_thread.join();
}

// A rule inside <noscript> applies only to clients without
// scripts, so it must never reach the inlined critical block, which every
// browser is served. Before the fix the scanner folded <noscript> <style>
// bodies (and <noscript> sheets) into the combined stylesheet, and the
// heuristic extractor kept .nsonly because it matches the fold's <h1>.
TEST_F(WorkerTest, NoscriptOnlyRuleNeverReachesTheInlinedBlock) {
  WorkerConfig config;
  config.socket_path = socket_path_;
  config.cache_path = cache_path_;
  config.cache_size_bytes = static_cast<uint64_t>(10 * 1024 * 1024);

  config.unsafe_force_async_css =
      true;  // fixture page is not under test for the gate
  Worker worker(config, nullptr);
  ASSERT_TRUE(worker.Initialize());

  const std::string html =
      "<html><head><style>h1 { color: red; }</style>"
      "<noscript><style>.nsonly { display: block !important; }</style>"
      "<link rel=\"stylesheet\" href=\"/noscript.css\"></noscript>"
      "</head><body><h1 class=\"nsonly nslink\">Hello World</h1></body></html>";
  const std::string cache_url = "/noscript.html";
  CacheOriginal(worker.cache(), cache_url, html);
  CacheOriginal(worker.cache(), "/noscript.css",
                ".nslink { outline: 1px solid; }");

  std::thread worker_thread([&worker]() { worker.Run(); });
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  CacheNotification notification;
  notification.url = "http://example.com/noscript.html";
  notification.scheme = "https";
  notification.content_type = ContentType::kHtml;
  notification.capability_mask = 0;
  SendNotification(notification);

  CapabilityMask mask = CapabilityMask::Decode(0);
  std::string variant;
  for (int i = 0; i < 40 * kSanitizerBudgetScale; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    variant = ReadVariant(worker.cache(), cache_url, mask);
    if (!variant.empty()) break;
  }

  worker.Shutdown();
  worker_thread.join();

  ASSERT_FALSE(variant.empty()) << "HTML variant not written";
  const size_t block_start = variant.find("<style data-pagespeed-critical");
  ASSERT_NE(block_start, std::string::npos) << variant;
  const size_t block_end = variant.find("</style>", block_start);
  ASSERT_NE(block_end, std::string::npos) << variant;
  const std::string block =
      variant.substr(block_start, block_end - block_start);
  // The page's own rule is inlined; neither <noscript> source is.
  EXPECT_NE(block.find("color: red"), std::string::npos) << block;
  EXPECT_EQ(block.find("nsonly"), std::string::npos) << block;
  EXPECT_EQ(block.find("nslink"), std::string::npos) << block;
  // The author's <noscript> is still there for clients without scripts.
  EXPECT_NE(variant.find("<noscript><style>.nsonly"), std::string::npos)
      << variant;
}

// Regression (the /console/urls timeout fix): /v1/cache/urls must report a
// NON-ZERO alternate_count for HTML pages.  The count is cached in the URL
// registry by a scope guard in HandleNotification that fires for EVERY content
// type — crucially the HTML write path goes through WriteVariant /
// WriteCompressedVariants, NOT WriteTextVariant, so a refresh wired only into
// the text/image writers (the naive fix) would leave every HTML row at 0.  We
// drive a real HTML notification end-to-end and assert the cached count equals
// the live unique-alternate-id count, computed independently below.
TEST_F(WorkerTest, HtmlNotificationCachesAlternateCount) {
  WorkerConfig config;
  config.socket_path = socket_path_;
  config.cache_path = cache_path_;
  config.cache_size_bytes = static_cast<uint64_t>(10 * 1024 * 1024);

  Worker worker(config, nullptr);
  ASSERT_TRUE(worker.Initialize());

  std::string html =
      "<html><head><style>body { margin: 0; }</style></head>"
      "<body><h1>Hello</h1></body></html>";
  std::string cache_url = "/count.html";
  CacheOriginal(worker.cache(), cache_url, html);

  std::thread worker_thread([&worker]() { worker.Run(); });
  WorkerStopper stopper(worker, worker_thread);
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  CacheNotification notification;
  notification.url = "http://example.com/count.html";
  notification.scheme = "https";
  notification.content_type = ContentType::kHtml;
  notification.capability_mask = 0x00000001;  // WebP capability
  SendNotification(notification);

  // Poll until the registry has cached a non-zero count for the page.  The
  // count guard fires at HandleNotification exit (just after the variant
  // write), so wait on the count itself rather than on the variant.
  auto cached_count = [&]() -> std::optional<uint32_t> {
    const UrlRegistry* reg = worker.url_registry();
    if (reg == nullptr) return std::nullopt;
    UrlRegistry::Page page = reg->List(0, 100);
    for (const auto& e : page.entries) {
      if (e.url == cache_url) return e.alternate_count;
    }
    return std::nullopt;
  };
  std::optional<uint32_t> count;
  for (int i = 0; i < 50 * kSanitizerBudgetScale; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    count = cached_count();
    if (count.has_value() && *count > 0) break;
  }

  ASSERT_TRUE(count.has_value()) << "page URL not recorded in registry";
  EXPECT_GT(*count, 0u)
      << "HTML alternate_count was not cached — the count guard missed the "
         "HTML (WriteVariant) write path";

  // The cached count must match the live unique-alternate-id count, computed
  // here independently of the production CountUniqueAlternates so a bug in that
  // helper would also be caught.
  auto alts = worker.cache()->ListAlternates(cache_url, "", "https");
  ASSERT_TRUE(alts.has_value());
  std::unordered_set<uint8_t> unique_ids;
  for (const auto& a : *alts) {
    unique_ids.insert(static_cast<uint8_t>(a.id));
  }
  EXPECT_EQ(static_cast<size_t>(*count), unique_ids.size());
}

// Issue A end-to-end wiring pin.  Sibling to
// HtmlNotificationInjectsCriticalCss above (which is the LOW-coverage inline
// path): here we seed a HIGH-coverage browser profile (coverage 0.85, ~93 KB
// critical of a ~110 KB sheet — Chrome's pathological Tailwind over-report) and
// drive the SAME HandleNotification path.  This pins the whole suppression
// wiring in worker.cc that the pure ShouldInlineCriticalCss helper test cannot
// see: skip_critical_css computed at profile-selection time, the
// critical_css.clear() applied AFTER the heuristic fallback, the resulting
// has_async_css suppression, and the critical_css_skipped_high_coverage stat.
// A regression (flag computed-but-never-applied, a clear() moved before the
// heuristic fallback so it re-extracts, async deferral not suppressed, or a
// miswired stat) flips one of the four asserts below.
TEST_F(WorkerTest, HtmlNotificationSuppressesHighCoverageBrowserCriticalCss) {
  WorkerConfig config;
  config.socket_path = socket_path_;
  config.cache_path = cache_path_;
  config.cache_size_bytes = static_cast<uint64_t>(10 * 1024 * 1024);
  // Construct the BrowserAnalysisManager so LookupProfile can return our seeded
  // profile.  A non-existent Chrome binary makes StartChrome fail gracefully
  // (non-fatal) — no live render is needed; we seed the profile directly.
  // The test binary runs as root in the build container,
  // where the Chrome sandbox is (correctly) unavailable and `require`
  // would refuse to construct the manager.  These tests seed profiles
  // directly and never spawn Chrome, so take the documented opt-out.
  config.browser_analysis.sandbox_mode = BrowserSandboxMode::kOff;
  config.browser_analysis.enabled = true;
  config.browser_analysis.chrome_binary = "/nonexistent/chrome";

  Worker worker(config, nullptr);
  ASSERT_TRUE(worker.Initialize());
  ASSERT_NE(worker.TestBrowserManager(), nullptr)
      << "browser manager must be constructed to seed a profile";

  // HTML with a render-blocking external <link rel="stylesheet"> and a body
  // <img> (so lazy-load still produces a variant even when critical CSS is
  // suppressed — proving the suppression does NOT short-circuit the whole HTML
  // pass).  Path-only URL for cache ops; full URL for the notification.
  std::string cache_url = "/tailwind.html";
  std::string notification_url = "http://example.com/tailwind.html";
  std::string html =
      "<html><head>"
      "<link rel=\"stylesheet\" href=\"http://example.com/tailwind.css\">"
      "</head><body><h1>Hello World</h1>"
      "<img src=\"/hero.png\"><img src=\"/below.png\"></body></html>";
  CacheOriginal(worker.cache(), cache_url, html);

  // Compute the template hash the worker will compute from this HTML, then seed
  // a HIGH-coverage profile under it on the MOBILE viewport (the notification
  // mask 0x1 decodes to Viewport::kMobile, so the worker selects ->mobile).
  HtmlScanner scanner;
  HtmlScanResult scan_result = scanner.Scan(notification_url, html);
  ASSERT_TRUE(scan_result.success);
  uint64_t template_hash = TemplateDetector::HashStructure(scan_result);

  OptimizationProfile profile;
  profile.template_hash_hex = "deadbeef";
  profile.analyzed_url = notification_url;
  // ~93 KB "critical" CSS of a ~110 KB sheet at 0.85 coverage — the bug case
  // ShouldInlineCriticalCss must suppress (0.85 >= 0.60 floor).
  profile.mobile.critical_css = std::string(93363, 'a');
  profile.mobile.css_coverage_ratio = 0.85f;
  profile.mobile.total_css_bytes = 110388;
  profile.mobile.unused_css_bytes = 110388 - 93363;
  profile.created_at = 0;
  profile.expires_at = 0;  // no TTL — LookupProfile keeps it
  worker.TestBrowserManager()->TestStoreProfile(template_hash, profile);

  std::thread worker_thread([&worker]() { worker.Run(); });
  WorkerStopper stopper(worker, worker_thread);
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  CacheNotification notification;
  notification.url = notification_url;
  notification.scheme = "https";
  notification.content_type = ContentType::kHtml;
  notification.capability_mask = 0x00000001;  // viewport bits -> Mobile
  SendNotification(notification);

  // The variant is written at the format-stripped mask (Mobile/Original).
  CapabilityMask mask = CapabilityMask::Decode(0x00000001);
  mask.set_image_format(CapabilityMask::ImageFormat::kOriginal);
  std::string variant;
  for (int i = 0; i < 40 * kSanitizerBudgetScale; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    variant = ReadVariant(worker.cache(), cache_url, mask);
    if (!variant.empty()) break;
  }

  ASSERT_FALSE(variant.empty())
      << "HTML variant not written (lazy-load on the <img> should still apply)";

  // (a) Inline critical CSS suppressed: no injected <style data-pagespeed-...>.
  EXPECT_EQ(variant.find("data-pagespeed-critical"), std::string::npos)
      << "high-coverage browser critical CSS must NOT be inlined";
  // (b) Async deferral also suppressed: no preload swap, no async marker.
  EXPECT_EQ(variant.find("data-pagespeed-async"), std::string::npos)
      << "with critical CSS suppressed, the sheet must NOT be async-deferred";
  EXPECT_EQ(variant.find("as=\"style\""), std::string::npos)
      << "async preload swap must not be applied";
  // (c) The original render-blocking external <link rel=stylesheet> survives.
  EXPECT_NE(variant.find("rel=\"stylesheet\""), std::string::npos)
      << "original render-blocking external <link> must be preserved";
  EXPECT_NE(variant.find("tailwind.css"), std::string::npos);
  // The variant still exists because an image transform ran (a non-CSS
  // transform DID apply) — i.e. suppressing critical CSS did NOT short-circuit
  // the whole HTML pass.  The first body <img> always gets fetchpriority="high"
  // (LCP-candidate match or first-img heuristic).
  EXPECT_NE(variant.find("fetchpriority=\"high\""), std::string::npos)
      << "image transform should still apply (variant is not CSS-only)";

  // (d) The suppression stat incremented exactly once.
  EXPECT_EQ(worker.stats().critical_css_skipped_high_coverage.load(), 1u)
      << "critical_css_skipped_high_coverage must increment on suppression";
}

// =============================================================================
// Async-CSS empirical gate: deferral requires a validated profile that is still
// bound to the stylesheet being served.
// =============================================================================

namespace async_css_gate {

// One page, one stylesheet, across the whole group: the ONLY thing that varies
// between these tests is the validation record, so any difference in the served
// markup is the gate's doing and nothing else's.
constexpr const char* kCssUrl = "http://example.com/site.css";
constexpr const char* kCss = ".hero{color:red;font-size:24px}p{margin:0}";
// What the browser profile claims as its critical block. It is used as the
// coverage-identity source for the re-derivation, not inlined verbatim.
constexpr const char* kProfileCriticalCss = ".hero{color:red;font-size:24px}";

std::string PageHtml() {
  return std::string("<html><head><link rel=\"stylesheet\" href=\"") + kCssUrl +
         "\"></head><body><div class=\"hero\">Hello</div></body></html>";
}

}  // namespace async_css_gate

// D2, heuristic half: with no browser profile there is no validation record and
// none can be produced, so the heuristic extractor's output must never
// authorize deferral — however favourable the byte ratio looks. The sheet here
// is well under async_css_min_deferred_bytes, so the small-sheet escape hatch
// makes the byte gate say "sufficient"; that is exactly the free pass this
// closes.
TEST_F(WorkerTest, HeuristicPathNeverDefersAsyncCss) {
  using namespace async_css_gate;
  WorkerConfig config;
  config.socket_path = socket_path_;
  config.cache_path = cache_path_;
  config.cache_size_bytes = static_cast<uint64_t>(10 * 1024 * 1024);

  Worker worker(config, nullptr);
  ASSERT_TRUE(worker.Initialize());

  const std::string url = "http://example.com/heuristic.html";
  const std::string html = PageHtml();
  CacheOriginal(worker.cache(), url, html);
  CacheOriginal(worker.cache(), kCssUrl, kCss, "example.com");

  std::thread worker_thread([&worker]() { worker.Run(); });
  WorkerStopper stopper(worker, worker_thread);
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  CacheNotification notification;
  notification.url = url;
  notification.scheme = "https";
  notification.content_type = ContentType::kHtml;
  notification.capability_mask = 0;
  SendNotification(notification);

  CapabilityMask mask = CapabilityMask::Decode(0);
  std::string variant;
  for (int i = 0; i < 60 * kSanitizerBudgetScale; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    variant = ReadVariant(worker.cache(), url, mask);
    if (!variant.empty()) break;
  }
  ASSERT_FALSE(variant.empty()) << "HTML variant not written";

  EXPECT_EQ(variant.find("data-pagespeed-async"), std::string::npos)
      << "the heuristic path has no validation record and must not defer";
  EXPECT_EQ(variant.find("as=\"style\""), std::string::npos);
  EXPECT_NE(variant.find("rel=\"stylesheet\""), std::string::npos)
      << "the render-blocking <link> must survive";
  // Without deferral the block would only duplicate bytes next to the
  // render-blocking sheet, so it is not inlined either.
  EXPECT_EQ(variant.find("data-pagespeed-critical"), std::string::npos)
      << "the sheet stays render-blocking, so the block must not be inlined";
}

// A profile exists but carries no validation record: the same refusal, now on
// the path that CAN one day produce one.
TEST_F(WorkerTest, AsyncCssRequiresValidatedProfile) {
  using namespace async_css_gate;
  WorkerConfig config = BrowserProfileConfig();

  Worker worker(config, nullptr);
  ASSERT_TRUE(worker.Initialize());

  const std::string url = "http://example.com/unvalidated.html";
  const std::string html = PageHtml();
  CacheOriginal(worker.cache(), url, html);
  CacheOriginal(worker.cache(), kCssUrl, kCss, "example.com");
  StoreBrowserProfile(worker, url, html, kProfileCriticalCss,
                      /*validated=*/false, /*validated_against_css=*/"");

  std::thread worker_thread([&worker]() { worker.Run(); });
  WorkerStopper stopper(worker, worker_thread);
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  CacheNotification notification;
  notification.url = url;
  notification.scheme = "https";
  notification.content_type = ContentType::kHtml;
  notification.capability_mask = 0;
  SendNotification(notification);

  CapabilityMask mask = CapabilityMask::Decode(0);
  std::string variant;
  for (int i = 0; i < 60 * kSanitizerBudgetScale; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    variant = ReadVariant(worker.cache(), url, mask);
    if (!variant.empty()) break;
  }
  ASSERT_FALSE(variant.empty()) << "HTML variant not written";

  EXPECT_EQ(variant.find("data-pagespeed-async"), std::string::npos)
      << "an unvalidated profile must not authorize deferral";
  EXPECT_EQ(variant.find("as=\"style\""), std::string::npos);
  EXPECT_EQ(variant.find("data-pagespeed-critical"), std::string::npos);
}

// The positive control: the gate does not simply suppress everything.
TEST_F(WorkerTest, AsyncCssDefersWhenProfileValidatedAndHashMatches) {
  using namespace async_css_gate;
  WorkerConfig config = BrowserProfileConfig();

  Worker worker(config, nullptr);
  ASSERT_TRUE(worker.Initialize());

  const std::string url = "http://example.com/validated.html";
  const std::string html = PageHtml();
  CacheOriginal(worker.cache(), url, html);
  CacheOriginal(worker.cache(), kCssUrl, kCss, "example.com");
  // The page's only stylesheet and no inline <style>, so the combined sheet the
  // worker assembles IS these bytes.
  StoreBrowserProfile(worker, url, html, kProfileCriticalCss,
                      /*validated=*/true, /*validated_against_css=*/kCss);

  std::thread worker_thread([&worker]() { worker.Run(); });
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  CacheNotification notification;
  notification.url = url;
  notification.scheme = "https";
  notification.content_type = ContentType::kHtml;
  notification.capability_mask = 0;
  SendNotification(notification);

  CapabilityMask mask = CapabilityMask::Decode(0);
  std::string variant;
  for (int i = 0; i < 80 * kSanitizerBudgetScale; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    variant = ReadVariant(worker.cache(), url, mask);
    if (variant.find("as=\"style\"") != std::string::npos) break;
  }

  EXPECT_NE(variant.find("data-pagespeed-async"), std::string::npos)
      << "a validated profile bound to the served sheet must defer";
  EXPECT_NE(variant.find("as=\"style\""), std::string::npos);
  EXPECT_NE(variant.find("data-pagespeed-critical"), std::string::npos);
  EXPECT_GE(worker.stats().policy_async_css_enabled.load(), 1u);
  // The #1216 guard is not in the way of the happy path: the derivation DID
  // produce the block, so the record stands.
  EXPECT_EQ(worker.stats().async_css_record_dropped_empty_derivation.load(),
            0u);

  worker.Shutdown();
  worker_thread.join();
}

// The 64 KiB inline cap applies while the sheet stays render-blocking. A
// validated deferral is never vetoed by it: there the block REPLACES the
// render-blocking fetch, and the validator rendered exactly those bytes. A
// site with a big sheet, a validated profile and a >64 KiB derived block must
// keep both the inline and the deferral.
TEST_F(WorkerTest, ValidatedDeferralIsNotVetoedByTheInlineByteCap) {
  using namespace async_css_gate;
  WorkerConfig config = BrowserProfileConfig();

  Worker worker(config, nullptr);
  ASSERT_TRUE(worker.Initialize());

  // One matched element and ~72 KB of rules that all apply to it, so the
  // derived block is the whole sheet and well over the cap.
  const std::string big_css_url = "http://example.com/big.css";
  std::string big_css;
  for (int i = 0; i < 1600; ++i) {
    absl::StrAppend(&big_css, ".a{padding-top:.0001px;margin-top:.0001px}\n");
  }
  // kInlineCriticalCssMaxBytes (browser_analysis_manager_internal.h).
  constexpr size_t kInlineCap = 64 * 1024;
  ASSERT_GT(big_css.size(), kInlineCap);
  const std::string url = "http://example.com/big.html";
  const std::string html =
      std::string("<html><head><link rel=\"stylesheet\" href=\"") +
      big_css_url + "\"></head><body><div class=\"a\">x</div></body></html>";
  CacheOriginal(worker.cache(), url, html);
  CacheOriginal(worker.cache(), big_css_url, big_css, "example.com");
  StoreBrowserProfile(worker, url, html, /*profile_critical_css=*/".a{}",
                      /*validated=*/true, /*validated_against_css=*/big_css);

  std::thread worker_thread([&worker]() { worker.Run(); });
  WorkerStopper stopper(worker, worker_thread);
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  CacheNotification notification;
  notification.url = url;
  notification.scheme = "https";
  notification.content_type = ContentType::kHtml;
  notification.capability_mask = 0;
  SendNotification(notification);

  CapabilityMask mask = CapabilityMask::Decode(0);
  std::string variant;
  for (int i = 0; i < 80 * kSanitizerBudgetScale; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    variant = ReadVariant(worker.cache(), url, mask);
    if (variant.find("as=\"style\"") != std::string::npos) break;
  }
  ASSERT_FALSE(variant.empty()) << "HTML variant not written";

  const size_t block_start = variant.find("<style data-pagespeed-critical");
  ASSERT_NE(block_start, std::string::npos)
      << "the validated block must still be inlined";
  const size_t block_end = variant.find("</style>", block_start);
  ASSERT_NE(block_end, std::string::npos);
  EXPECT_GT(block_end - block_start, kInlineCap)
      << "and it is the over-cap block, not a truncated one";
  EXPECT_NE(variant.find("data-pagespeed-async"), std::string::npos)
      << "the validated deferral must still happen";
  EXPECT_NE(variant.find("as=\"style\""), std::string::npos);
  EXPECT_EQ(worker.stats().critical_css_skipped_byte_cap.load(), 0u);
  // A record that matches is never sent back for validation.
  EXPECT_EQ(worker.TestBrowserManager()->stats().revalidations_queued.load(),
            0u);
}

// #1216: the record is dropped the moment the derivation comes back empty for
// THIS page, before anything else can take the derived block's place.
//
// Setup: a healthy record — validated bit set, hash still bound to the served
// stylesheet — on a profile whose critical block names a selector the current
// sheet no longer contains, served to a page whose fold matches nothing in that
// sheet either. So the DOM-matched derivation has nothing to emit and nothing
// to force-include, and what the serve path inlines from here on is not what
// the record was produced about. The sheet is far under
// async_css_min_deferred_bytes, so the byte gate's small-sheet escape hatch
// says "sufficient" whatever the coverage is: the record is the only thing left
// between a substituted block and a deferred stylesheet, which is why it must
// not still be standing.
//
// The counter is the assertion that pins WHERE the record is dropped. Today the
// heuristic fallback finds nothing here either (it sees the same DOM through a
// strictly less inclusive matcher), so the served markup alone cannot tell a
// dropped record apart from an empty one; make the fallback more inclusive than
// the derivation and only the drop point stands between its block and a
// deferral.
TEST_F(WorkerTest, AsyncCssRecordDroppedWhenNoBlockDerivedForPage) {
  WorkerConfig config = BrowserProfileConfig();

  Worker worker(config, nullptr);
  ASSERT_TRUE(worker.Initialize());

  constexpr const char* kCssUrl = "http://example.com/nomatch.css";
  constexpr const char* kCss = ".hero{color:red;font-size:24px}p{margin:0}";
  // A block from an earlier build of the sheet: its selector is not in the
  // served sheet, so it force-includes nothing.
  constexpr const char* kStaleProfileCss = ".legacy-hero{color:red}";

  const std::string url = "http://example.com/nomatch.html";
  // Neither `.hero` nor `p` occurs in the fold. The <img> elements are here so
  // an image transform still applies and a variant is written at all.
  const std::string html =
      std::string("<html><head><link rel=\"stylesheet\" href=\"") + kCssUrl +
      "\"></head><body><div class=\"banner\">Hello</div>"
      "<img src=\"/one.png\"><img src=\"/two.png\"></body></html>";
  CacheOriginal(worker.cache(), url, html);
  CacheOriginal(worker.cache(), kCssUrl, kCss, "example.com");
  StoreBrowserProfile(worker, url, html, kStaleProfileCss,
                      /*validated=*/true, /*validated_against_css=*/kCss);

  std::thread worker_thread([&worker]() { worker.Run(); });
  WorkerStopper stopper(worker, worker_thread);
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  CacheNotification notification;
  notification.url = url;
  notification.scheme = "https";
  notification.content_type = ContentType::kHtml;
  notification.capability_mask = 0;
  SendNotification(notification);

  CapabilityMask mask = CapabilityMask::Decode(0);
  std::string variant;
  for (int i = 0; i < 60 * kSanitizerBudgetScale; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    variant = ReadVariant(worker.cache(), url, mask);
    if (!variant.empty()) break;
  }
  ASSERT_FALSE(variant.empty())
      << "HTML variant not written (the image transform should still apply)";

  EXPECT_EQ(worker.stats().async_css_record_dropped_empty_derivation.load(), 1u)
      << "the record must be dropped where the derivation comes back empty, "
         "not left standing for whatever is inlined next";
  EXPECT_EQ(variant.find("data-pagespeed-async"), std::string::npos)
      << "no block was derived for this page, so nothing may be deferred";
  EXPECT_EQ(variant.find("as=\"style\""), std::string::npos);
  EXPECT_NE(variant.find("rel=\"stylesheet\""), std::string::npos)
      << "the render-blocking <link> must survive";
  EXPECT_EQ(variant.find("data-pagespeed-critical"), std::string::npos)
      << "precondition: no critical block was produced for this page at all";
}

// C2, the scenario that actually occurs: a stylesheet-only redeploy. The tag
// tree is unchanged, so the template hash still resolves the same profile and
// its validated bit is still alive — but the bytes it was validated against are
// gone. Honouring the bit here re-derives a never-validated critical block from
// the NEW sheet and defers on the OLD sheet's verdict.
TEST_F(WorkerTest, AsyncCssRefusedWhenCombinedCssHashDiverges) {
  using namespace async_css_gate;
  WorkerConfig config = BrowserProfileConfig();

  Worker worker(config, nullptr);
  ASSERT_TRUE(worker.Initialize());

  const std::string url = "http://example.com/redeployed.html";
  const std::string html = PageHtml();
  CacheOriginal(worker.cache(), url, html);
  CacheOriginal(worker.cache(), kCssUrl, kCss, "example.com");
  // Validated — against a DIFFERENT build of the stylesheet.
  StoreBrowserProfile(
      worker, url, html, kProfileCriticalCss, /*validated=*/true,
      /*validated_against_css=*/".hero{color:blue;font-size:24px}p{margin:0}");

  std::thread worker_thread([&worker]() { worker.Run(); });
  WorkerStopper stopper(worker, worker_thread);
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  CacheNotification notification;
  notification.url = url;
  notification.scheme = "https";
  notification.content_type = ContentType::kHtml;
  notification.capability_mask = 0;
  SendNotification(notification);

  CapabilityMask mask = CapabilityMask::Decode(0);
  std::string variant;
  for (int i = 0; i < 60 * kSanitizerBudgetScale; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    variant = ReadVariant(worker.cache(), url, mask);
    if (!variant.empty()) break;
  }
  ASSERT_FALSE(variant.empty()) << "HTML variant not written";

  EXPECT_EQ(variant.find("data-pagespeed-async"), std::string::npos)
      << "a validation made against different stylesheet bytes must not carry "
         "over to the redeployed sheet";
  EXPECT_EQ(variant.find("as=\"style\""), std::string::npos);
  EXPECT_EQ(variant.find("data-pagespeed-critical"), std::string::npos);

  // And the template is queued for validation again now, rather
  // than the page waiting out the profile's TTL with deferral off.
  BrowserAnalysisManager* mgr = worker.TestBrowserManager();
  ASSERT_NE(mgr, nullptr);
  EXPECT_EQ(mgr->stats().revalidations_queued.load(), 1u);
  auto item = mgr->TestDequeue();
  ASSERT_TRUE(item.has_value());
  HtmlScanner scanner;
  EXPECT_EQ(item->template_hash,
            TemplateDetector::HashStructure(scanner.Scan(url, html)));
  EXPECT_EQ(item->original_html, html);
}

// The record's hash is of the ANALYSED page's combined sheet,
// which includes that page's own inline <style>. A sibling page of the same
// template with different inline CSS always mismatches; if it could ask for a
// revalidation, the record would move from page to page and every such
// template would spend its budget daily. Only the analysed page asks.
TEST_F(WorkerTest, OnlyTheAnalyzedPageRequestsRevalidation) {
  using namespace async_css_gate;
  WorkerConfig config = BrowserProfileConfig();

  config.unsafe_force_async_css =
      true;  // fixture page is not under test for the gate
  Worker worker(config, nullptr);
  ASSERT_TRUE(worker.Initialize());

  auto page = [](std::string_view inline_css) {
    return absl::StrCat("<html><head><style>", inline_css,
                        "</style><link rel=\"stylesheet\" href=\"", kCssUrl,
                        "\"></head><body><div class=\"hero\">Hello</div>"
                        "</body></html>");
  };
  const std::string analyzed_url = "http://example.com/analyzed.html";
  const std::string sibling_url = "http://example.com/sibling.html";
  const std::string analyzed_html = page(".a{color:red}");
  const std::string sibling_html = page(".b{color:blue}");
  HtmlScanner scanner;
  ASSERT_EQ(
      TemplateDetector::HashStructure(
          scanner.Scan(analyzed_url, analyzed_html)),
      TemplateDetector::HashStructure(scanner.Scan(sibling_url, sibling_html)))
      << "precondition: one template";
  CacheOriginal(worker.cache(), analyzed_url, analyzed_html);
  CacheOriginal(worker.cache(), sibling_url, sibling_html);
  CacheOriginal(worker.cache(), kCssUrl, kCss, "example.com");
  // Validated on the analysed page, against a stylesheet since redeployed.
  StoreBrowserProfile(worker, analyzed_url, analyzed_html, kProfileCriticalCss,
                      /*validated=*/true,
                      /*validated_against_css=*/".a{color:red}\n.old{}");

  std::thread worker_thread([&worker]() { worker.Run(); });
  WorkerStopper stopper(worker, worker_thread);
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  auto serve = [&](const std::string& url) {
    CacheNotification notification;
    notification.url = url;
    notification.scheme = "https";
    notification.content_type = ContentType::kHtml;
    notification.capability_mask = 0;
    SendNotification(notification);
    CapabilityMask mask = CapabilityMask::Decode(0);
    std::string variant;
    for (int i = 0; i < 60 * kSanitizerBudgetScale; ++i) {
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
      variant = ReadVariant(worker.cache(), url, mask);
      if (!variant.empty()) break;
    }
    return variant;
  };
  BrowserAnalysisManager* mgr = worker.TestBrowserManager();
  ASSERT_NE(mgr, nullptr);

  const std::string sibling_variant = serve(sibling_url);
  ASSERT_FALSE(sibling_variant.empty()) << "sibling variant not written";
  EXPECT_NE(sibling_variant.find("data-pagespeed-critical"), std::string::npos)
      << "precondition: the profile path served the sibling";
  EXPECT_EQ(mgr->stats().revalidations_queued.load(), 0u)
      << "a sibling page's own inline CSS is not a reason to revalidate";
  EXPECT_EQ(mgr->stats().revalidations_rate_limited.load(), 0u);

  ASSERT_FALSE(serve(analyzed_url).empty()) << "analysed variant not written";
  EXPECT_EQ(mgr->stats().revalidations_queued.load(), 1u)
      << "the analysed page's own stale record is";
  auto item = mgr->TestDequeue();
  ASSERT_TRUE(item.has_value());
  // Queued in the serve path's normalized form, which is also what the new
  // record's analyzed_url will be.
  EXPECT_EQ(item->url, "/analyzed.html");
}

// The analysis renders now leave <noscript> content out, so a
// record made while they rendered it is about another fold, but only on a page
// whose <noscript> content shows something with scripts off. Such a page's
// record no longer matches (the binding is salted) and the analysed page asks
// for a new validation once; a record made since, with the salt, matches. A
// page whose <noscript> holds only the GTM snippet's hidden iframe keeps its
// record and its deferral, and asks for nothing.
TEST_F(WorkerTest, NoscriptContentSaltsTheValidationBinding) {
  using namespace async_css_gate;
  WorkerConfig config = BrowserProfileConfig();

  Worker worker(config, nullptr);
  ASSERT_TRUE(worker.Initialize());

  auto page = [](std::string_view noscript, std::string_view extra) {
    return absl::StrCat("<html><head><link rel=\"stylesheet\" href=\"", kCssUrl,
                        "\"></head><body>", noscript,
                        "<div class=\"hero\">Hello</div>", extra,
                        "</body></html>");
  };
  const std::string gtm_url = "http://example.com/gtm.html";
  const std::string gtm_html = page(
      "<noscript><iframe "
      "src=\"https://www.googletagmanager.com/ns.html?id=GTM-X\" "
      "height=\"0\" width=\"0\" "
      "style=\"display:none;visibility:hidden\"></iframe></noscript>",
      "");
  const std::string banner_url = "http://example.com/banner.html";
  const std::string banner_html =
      page("<noscript><div class=\"nojs\">Enable JS</div></noscript>", "");
  const std::string revalidated_url = "http://example.com/revalidated.html";
  const std::string revalidated_html = page(
      "<noscript><div class=\"nojs\">Enable JS</div></noscript>", "<p>x</p>");
  HtmlScanner scanner;
  ASSERT_FALSE(scanner.Scan(gtm_url, gtm_html).noscript_affects_render);
  ASSERT_TRUE(scanner.Scan(banner_url, banner_html).noscript_affects_render);
  CacheOriginal(worker.cache(), gtm_url, gtm_html);
  CacheOriginal(worker.cache(), banner_url, banner_html);
  CacheOriginal(worker.cache(), revalidated_url, revalidated_html);
  CacheOriginal(worker.cache(), kCssUrl, kCss, "example.com");
  // Records made before the <noscript> removal on the first two pages, one made
  // since on the
  // third. All against the served sheet.
  StoreBrowserProfile(worker, gtm_url, gtm_html, kProfileCriticalCss,
                      /*validated=*/true, /*validated_against_css=*/kCss);
  StoreBrowserProfile(worker, banner_url, banner_html, kProfileCriticalCss,
                      /*validated=*/true, /*validated_against_css=*/kCss);
  StoreBrowserProfile(worker, revalidated_url, revalidated_html,
                      kProfileCriticalCss, /*validated=*/true,
                      /*validated_against_css=*/kCss, std::nullopt,
                      /*made_without_noscript=*/true);

  std::thread worker_thread([&worker]() { worker.Run(); });
  WorkerStopper stopper(worker, worker_thread);
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  auto serve = [&](const std::string& url, bool expect_deferral) {
    CacheNotification notification;
    notification.url = url;
    notification.scheme = "https";
    notification.content_type = ContentType::kHtml;
    notification.capability_mask = 0;
    SendNotification(notification);
    CapabilityMask mask = CapabilityMask::Decode(0);
    std::string variant;
    for (int i = 0; i < 80 * kSanitizerBudgetScale; ++i) {
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
      variant = ReadVariant(worker.cache(), url, mask);
      if (!variant.empty() &&
          (!expect_deferral ||
           variant.find("as=\"style\"") != std::string::npos)) {
        break;
      }
    }
    return variant;
  };
  BrowserAnalysisManager* mgr = worker.TestBrowserManager();
  ASSERT_NE(mgr, nullptr);

  const std::string gtm = serve(gtm_url, /*expect_deferral=*/true);
  EXPECT_NE(gtm.find("data-pagespeed-async"), std::string::npos)
      << "a hidden tag-manager iframe changes no render: the record stands";
  EXPECT_EQ(mgr->stats().revalidations_queued.load(), 0u);

  const std::string banner = serve(banner_url, /*expect_deferral=*/false);
  ASSERT_FALSE(banner.empty()) << "banner variant not written";
  EXPECT_EQ(banner.find("data-pagespeed-async"), std::string::npos)
      << "a record made while the banner was rendered is about another fold";
  EXPECT_EQ(banner.find("data-pagespeed-critical"), std::string::npos);
  EXPECT_EQ(mgr->stats().revalidations_queued.load(), 1u)
      << "so the page asks to be validated again";

  const std::string revalidated =
      serve(revalidated_url, /*expect_deferral=*/true);
  EXPECT_NE(revalidated.find("data-pagespeed-async"), std::string::npos)
      << "a record made with the salt matches";
  EXPECT_EQ(mgr->stats().revalidations_queued.load(), 1u);
}

// The record binds to the page's cascade-layer order as well as to its sheet,
// through the same combined-sheet hash, so a change of order
// takes the path a changed sheet takes: the analysed page asks for a
// new validation. A record of the same sheet made with the served order does
// not ask. The notification's mask 0 is the mobile class, whose records carry
// the "touch" and "ua" salts since touch emulation and User-Agent emulation
// (ValidationBindingFor), so a record "made since" a fix below is one made
// with those salts; a mobile record made before either is made again once, as
// those changes intend.
TEST_F(WorkerTest, ALayerOrderChangeRequestsRevalidation) {
  using namespace async_css_gate;
  const std::string url = "http://example.com/layered.html";
  auto page = [](std::string_view inline_css) {
    return absl::StrCat("<html><head><style>", inline_css,
                        "</style><link rel=\"stylesheet\" href=\"", kCssUrl,
                        "\"></head><body><div class=\"hero\">Hello</div>"
                        "</body></html>");
  };
  // Serves `html` with a validation record of hash `record_hash` and returns
  // how many revalidations the serve queued.
  auto revalidations = [&](const std::string& html,
                           const std::string& record_hash) -> uint64_t {
    WorkerConfig config = BrowserProfileConfig();
    config.unsafe_force_async_css =
        true;  // fixture page is not under test for the gate
    Worker worker(config, nullptr);
    EXPECT_TRUE(worker.Initialize());
    CacheOriginal(worker.cache(), url, html);
    CacheOriginal(worker.cache(), kCssUrl, kCss, "example.com");
    StoreBrowserProfile(worker, url, html, kProfileCriticalCss,
                        /*validated=*/true, /*validated_against_css=*/kCss,
                        record_hash);
    std::thread worker_thread([&worker]() { worker.Run(); });
    WorkerStopper stopper(worker, worker_thread);
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    CacheNotification notification;
    notification.url = url;
    notification.scheme = "https";
    notification.content_type = ContentType::kHtml;
    notification.capability_mask = 0;
    SendNotification(notification);
    CapabilityMask mask = CapabilityMask::Decode(0);
    std::string variant;
    for (int i = 0; i < 60 * kSanitizerBudgetScale; ++i) {
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
      variant = ReadVariant(worker.cache(), url, mask);
      if (!variant.empty()) break;
    }
    EXPECT_FALSE(variant.empty()) << "variant not written";
    EXPECT_NE(variant.find("data-pagespeed-critical"), std::string::npos)
        << "precondition: the profile path served the page";
    return worker.TestBrowserManager()->stats().revalidations_queued.load();
  };

  // A layered page: the order is proven, `@layer base;`.
  const std::string layered_inline = "@layer base{.a{color:red}}";
  const std::string layered_sheet = absl::StrCat(layered_inline, "\n", kCss);
  EXPECT_EQ(
      revalidations(page(layered_inline),
                    CombinedCssValidationHash(
                        layered_sheet, "proven r2 @layer base; touch ua")),
      0u)
      << "a record of the served sheet and order still matches";
  EXPECT_EQ(revalidations(page(layered_inline),
                          CombinedCssValidationHash(layered_sheet,
                                                    "proven r2 @layer base;")),
            1u)
      << "a mobile record made before touch emulation is made "
         "again once";
  EXPECT_EQ(revalidations(page(layered_inline),
                          CombinedCssValidationHash(
                              layered_sheet, "proven r2 @layer base; touch")),
            1u)
      << "a mobile record made before the user agent override "
         "is made again once";
  EXPECT_EQ(revalidations(page(layered_inline),
                          CombinedCssValidationHash(layered_sheet,
                                                    "proven @layer base;")),
            1u)
      << "a record made before @media retention narrowed on layered pages "
         "describes another block and is made again";
  EXPECT_EQ(revalidations(page(layered_inline),
                          CombinedCssValidationHash(layered_sheet, "unproven")),
            1u)
      << "the same sheet validated under another order no longer matches";
  EXPECT_EQ(revalidations(page(layered_inline),
                          CombinedCssValidationHash(layered_sheet)),
            1u)
      << "a record made before the binding, on a layered page, is made again";

  // A sheet with an !important in an anonymous layer binds with
  // "a2" (the block leaves those declarations out), so a record made before
  // is made again.
  const std::string anon_inline =
      "@layer base{.a{color:red}} @layer{.b{color:blue!important}}";
  const std::string anon_sheet = absl::StrCat(anon_inline, "\n", kCss);
  EXPECT_EQ(revalidations(page(anon_inline),
                          CombinedCssValidationHash(
                              anon_sheet, "proven a2 @layer base; touch ua")),
            0u)
      << "a record made with the a2 binding matches";
  EXPECT_EQ(
      revalidations(page(anon_inline), CombinedCssValidationHash(
                                           anon_sheet, "proven @layer base;")),
      1u)
      << "a record made before the !important rule is made again";

  // The anonymous layer is written as `@l\61yer {`,
  // which a browser reads as `@layer {`. The optimizer does not decode it, so
  // the order is not proven and the block goes after the sheets, where it
  // carries no anonymous-layer rule: the sheet binds with "u2", and the record
  // with the serve path's own derivation on top (anon-layers-dropped), the
  // same salt the validator's derivation adds.
  const std::string escaped_layer_inline =
      "@layer base{.a{color:red}} @l\\61yer{.b{color:blue}}";
  const std::string escaped_layer_sheet =
      absl::StrCat(escaped_layer_inline, "\n", kCss);
  EXPECT_EQ(revalidations(page(escaped_layer_inline),
                          CombinedCssValidationHash(
                              escaped_layer_sheet,
                              "unproven u2 touch ua anon-layers-dropped")),
            0u)
      << "a record made with the u2 binding and the derivation's salt matches";
  EXPECT_EQ(
      revalidations(page(escaped_layer_inline),
                    CombinedCssValidationHash(escaped_layer_sheet, "unproven")),
      1u)
      << "a record made before the anonymous-layer rule is made again";

  // A layered page whose `@import … layer()` is not cached yet: the order is
  // unproven for now and will change once the import caches, so the stale
  // record does not trigger a revalidation that would bind to `unproven`.
  const std::string import_inline =
      "@import url(/lib-not-cached.css) layer(lib);@layer base{.a{color:red}}";
  EXPECT_EQ(revalidations(page(import_inline),
                          CombinedCssValidationHash(
                              absl::StrCat(import_inline, "\n", kCss),
                              "proven r2 @layer lib,base;")),
            0u)
      << "a missing layer() import holds the revalidation back";

  // A page without layers: the layer binding is empty, so a record of the
  // sheet alone (plus the class's salt) matches; the layer binding itself
  // revalidates no page without layers.
  const std::string plain_inline = ".a{color:red}";
  EXPECT_EQ(
      revalidations(page(plain_inline),
                    CombinedCssValidationHash(
                        absl::StrCat(plain_inline, "\n", kCss), "touch ua")),
      0u)
      << "the binding must not revalidate every page without layers";

  // A sheet that escapes a quote outside a string was misread
  // until that fix, so a record made before it (no salt) is made again once,
  // and one made since (salted) stands. The plain page above shows that a
  // sheet without such an escape keeps its record.
  const std::string escaped_inline = ".a\\'b{color:red}";
  const std::string escaped_sheet = absl::StrCat(escaped_inline, "\n", kCss);
  ASSERT_TRUE(CombinedCssHasStructuralEscape(escaped_sheet));
  ASSERT_FALSE(
      CombinedCssHasStructuralEscape(absl::StrCat(plain_inline, "\n", kCss)));
  EXPECT_EQ(revalidations(page(escaped_inline),
                          CombinedCssValidationHash(escaped_sheet)),
            1u)
      << "a record made while the sheet was misread is made again";
  EXPECT_EQ(revalidations(page(escaped_inline),
                          CombinedCssValidationHash(escaped_sheet,
                                                    "css-escape touch ua")),
            0u)
      << "a record made since the fix matches";
}

// M6(a): the stylesheet resolves from cache but its body is empty, so the
// combined sheet is empty while external_css_missing stays FALSE. The profile
// path then inlines the browser block verbatim at coverage -1, and the byte
// gate's `deferred_css_bytes == 0` free pass waves the deferral through —
// deferring a real <link> on a decision made against no stylesheet at all.
// "Validated against nothing" must never satisfy the gate, so this holds even
// with the validated bit set and a hash on file.
TEST_F(WorkerTest, EmptyCombinedCssNeverDefers) {
  using namespace async_css_gate;
  WorkerConfig config = BrowserProfileConfig();

  Worker worker(config, nullptr);
  ASSERT_TRUE(worker.Initialize());

  const std::string url = "http://example.com/emptysheet.html";
  const std::string html = PageHtml();
  CacheOriginal(worker.cache(), url, html);
  CacheOriginal(worker.cache(), kCssUrl, "", "example.com");
  StoreBrowserProfile(worker, url, html, kProfileCriticalCss,
                      /*validated=*/true, /*validated_against_css=*/kCss);

  std::thread worker_thread([&worker]() { worker.Run(); });
  WorkerStopper stopper(worker, worker_thread);
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  CacheNotification notification;
  notification.url = url;
  notification.scheme = "https";
  notification.content_type = ContentType::kHtml;
  notification.capability_mask = 0;
  SendNotification(notification);

  CapabilityMask mask = CapabilityMask::Decode(0);
  std::string variant;
  for (int i = 0; i < 60 * kSanitizerBudgetScale; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    variant = ReadVariant(worker.cache(), url, mask);
    if (!variant.empty()) break;
  }
  ASSERT_FALSE(variant.empty()) << "HTML variant not written";

  EXPECT_EQ(variant.find("data-pagespeed-async"), std::string::npos)
      << "no stylesheet was gathered, so nothing was measured and nothing may "
         "be deferred";
  EXPECT_EQ(variant.find("as=\"style\""), std::string::npos);
  EXPECT_NE(variant.find("rel=\"stylesheet\""), std::string::npos);
}

// The "validated against nothing" escape, half one: a record that says
// validated but names no stylesheet. Two empty strings compare equal, so an
// unguarded gate reads this as a match against the equally empty combined sheet
// and defers. A record is only meaningful when it says WHAT it was made
// against.
TEST_F(WorkerTest, ValidatedRecordWithNoStylesheetHashNeverDefers) {
  using namespace async_css_gate;
  WorkerConfig config = BrowserProfileConfig();

  Worker worker(config, nullptr);
  ASSERT_TRUE(worker.Initialize());

  const std::string url = "http://example.com/nohash.html";
  const std::string html = PageHtml();
  CacheOriginal(worker.cache(), url, html);
  // Cached but EMPTY, so the combined sheet is empty too — both sides of the
  // comparison are "" unless the gate refuses an unbound record outright.
  CacheOriginal(worker.cache(), kCssUrl, "", "example.com");
  StoreBrowserProfile(worker, url, html, kProfileCriticalCss,
                      /*validated=*/true, /*validated_against_css=*/kCss,
                      /*validated_hash_override=*/std::string());

  std::thread worker_thread([&worker]() { worker.Run(); });
  WorkerStopper stopper(worker, worker_thread);
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  CacheNotification notification;
  notification.url = url;
  notification.scheme = "https";
  notification.content_type = ContentType::kHtml;
  notification.capability_mask = 0;
  SendNotification(notification);

  CapabilityMask mask = CapabilityMask::Decode(0);
  std::string variant;
  for (int i = 0; i < 60 * kSanitizerBudgetScale; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    variant = ReadVariant(worker.cache(), url, mask);
    if (!variant.empty()) break;
  }
  ASSERT_FALSE(variant.empty()) << "HTML variant not written";

  EXPECT_EQ(variant.find("data-pagespeed-async"), std::string::npos)
      << "a validation record naming no stylesheet must not authorize deferral";
  EXPECT_EQ(variant.find("as=\"style\""), std::string::npos);
  EXPECT_NE(variant.find("rel=\"stylesheet\""), std::string::npos);
}

// Half two: a record that DOES name a stylesheet — the digest of an empty one.
// Only reachable through the override, because the hash function refuses to
// produce it; that refusal is the guard under test. Drop it and this record
// matches an empty combined sheet, deferring a real <link> against a validation
// performed on no CSS whatsoever.
TEST_F(WorkerTest, ValidationAgainstAnEmptySheetNeverDefers) {
  using namespace async_css_gate;
  WorkerConfig config = BrowserProfileConfig();

  Worker worker(config, nullptr);
  ASSERT_TRUE(worker.Initialize());

  const std::string url = "http://example.com/emptyhash.html";
  const std::string html = PageHtml();
  CacheOriginal(worker.cache(), url, html);
  CacheOriginal(worker.cache(), kCssUrl, "", "example.com");
  // SHA-256 of the empty string — what hashing an empty sheet WOULD give if
  // CombinedCssValidationHash did not special-case it.
  StoreBrowserProfile(
      worker, url, html, kProfileCriticalCss, /*validated=*/true,
      /*validated_against_css=*/kCss,
      /*validated_hash_override=*/
      std::string(
          "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"));

  std::thread worker_thread([&worker]() { worker.Run(); });
  WorkerStopper stopper(worker, worker_thread);
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  CacheNotification notification;
  notification.url = url;
  notification.scheme = "https";
  notification.content_type = ContentType::kHtml;
  notification.capability_mask = 0;
  SendNotification(notification);

  CapabilityMask mask = CapabilityMask::Decode(0);
  std::string variant;
  for (int i = 0; i < 60 * kSanitizerBudgetScale; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    variant = ReadVariant(worker.cache(), url, mask);
    if (!variant.empty()) break;
  }
  ASSERT_FALSE(variant.empty()) << "HTML variant not written";

  EXPECT_EQ(variant.find("data-pagespeed-async"), std::string::npos)
      << "a validation performed against an empty stylesheet must not "
         "authorize deferring a real one";
  EXPECT_EQ(variant.find("as=\"style\""), std::string::npos);
}

// D6: the decision must not depend on how warm the cache happened to be when a
// notification arrived. Same URL, repeated notifications, warmer cache each
// time — the served markup keeps the same verdict.
TEST_F(WorkerTest, AsyncCssDecisionStableAcrossRepeatedNotifications) {
  using namespace async_css_gate;
  WorkerConfig config = BrowserProfileConfig();

  Worker worker(config, nullptr);
  ASSERT_TRUE(worker.Initialize());

  const std::string url = "http://example.com/stable.html";
  const std::string html = PageHtml();
  CacheOriginal(worker.cache(), url, html);
  CacheOriginal(worker.cache(), kCssUrl, kCss, "example.com");
  StoreBrowserProfile(worker, url, html, kProfileCriticalCss,
                      /*validated=*/true, /*validated_against_css=*/kCss);

  std::thread worker_thread([&worker]() { worker.Run(); });
  WorkerStopper stopper(worker, worker_thread);
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  CacheNotification notification;
  notification.url = url;
  notification.scheme = "https";
  notification.content_type = ContentType::kHtml;
  notification.capability_mask = 0;
  SendNotification(notification);

  CapabilityMask mask = CapabilityMask::Decode(0);
  std::string first;
  for (int i = 0; i < 80 * kSanitizerBudgetScale; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    first = ReadVariant(worker.cache(), url, mask);
    if (first.find("as=\"style\"") != std::string::npos) break;
  }
  const bool deferred_first =
      first.find("data-pagespeed-async") != std::string::npos;
  ASSERT_TRUE(deferred_first)
      << "precondition: the validated profile should defer on the first pass";

  // Re-notify against a fully warm cache several times.
  for (int i = 0; i < 5; ++i) {
    SendNotification(notification);
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    std::string again = ReadVariant(worker.cache(), url, mask);
    ASSERT_FALSE(again.empty());
    EXPECT_EQ(again.find("data-pagespeed-async") != std::string::npos,
              deferred_first)
        << "the deferral decision changed on repeat notification " << i;
  }
}

TEST_F(WorkerTest, HtmlNotificationWithCachedStylesheet) {
  // HTML with external stylesheet cached: the worker gathers CSS from
  // both inline and external sources, extracts critical CSS, and
  // injects it into the HTML variant.
  WorkerConfig config;
  config.socket_path = socket_path_;
  config.cache_path = cache_path_;
  config.cache_size_bytes = static_cast<uint64_t>(10 * 1024 * 1024);

  config.unsafe_force_async_css =
      true;  // fixture page is not under test for the gate
  Worker worker(config, nullptr);
  ASSERT_TRUE(worker.Initialize());

  // Pre-populate HTML that references an external stylesheet.
  // Path-only URL for cache operations; full URL for notification.
  std::string cache_url = "/page.html";
  std::string notification_url = "http://example.com/page.html";
  std::string html =
      "<html><head>"
      "<link rel=\"stylesheet\" href=\"http://example.com/style.css\">"
      "</head><body><div class=\"hero\">Hello</div></body></html>";
  CacheOriginal(worker.cache(), cache_url, html);

  // Pre-populate the external stylesheet in cache.  The href is absolute,
  // so the worker resolves it as-is and uses UrlHostname to extract
  // "example.com" for the cache lookup.  Use raw_url=true to skip
  // normalization (matching the worker's lookup path for absolute hrefs).
  std::string css_url = "http://example.com/style.css";
  std::string css = ".hero { font-size: 24px; } .footer { display: none; }";
  CacheOriginal(worker.cache(), css_url, css, "example.com");

  // Run worker in background
  std::thread worker_thread([&worker]() { worker.Run(); });
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  // Send notification
  CacheNotification notification;
  notification.url = notification_url;
  notification.scheme = "https";
  notification.content_type = ContentType::kHtml;
  notification.capability_mask = 0;
  SendNotification(notification);

  // Poll for variant
  CapabilityMask mask = CapabilityMask::Decode(0);
  std::string variant;
  for (int i = 0; i < 40 * kSanitizerBudgetScale; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    variant = ReadVariant(worker.cache(), cache_url, mask);
    if (!variant.empty()) break;
  }

  // Verify variant was written with injected critical CSS
  EXPECT_FALSE(variant.empty()) << "HTML variant not written";
  if (!variant.empty()) {
    EXPECT_NE(variant.find("data-pagespeed-critical"), std::string::npos);
    // The hero class CSS should be included (it's in the external sheet)
    EXPECT_NE(variant.find("hero"), std::string::npos);
    // Original HTML structure should be preserved
    EXPECT_NE(variant.find("Hello"), std::string::npos);
  }

  // Verify original HTML is still intact at the default mask.
  CapabilityMask default_mask;
  std::string cached = ReadVariant(worker.cache(), cache_url, default_mask);
  EXPECT_EQ(cached, html);

  worker.Shutdown();
  worker_thread.join();
}

TEST_F(WorkerTest, HtmlNotificationResolvesRelativeStylesheetHref) {
  // HTML with a relative stylesheet href: the worker resolves the href
  // against the page URL before looking up the CSS in cache.
  WorkerConfig config;
  config.socket_path = socket_path_;
  config.cache_path = cache_path_;
  config.cache_size_bytes = static_cast<uint64_t>(10 * 1024 * 1024);
  // Async CSS off, so the sheet stays a render-blocking link and keeps its
  // preload hint; this test asserts the RESOLVED href appears in the hints.
  config.disable_async_css = true;

  Worker worker(config, nullptr);
  ASSERT_TRUE(worker.Initialize());

  // Pre-populate HTML at a deep path that references a relative stylesheet.
  // Path-only URL for cache operations; full URL for notification.
  std::string cache_url = "/demos/ecommerce/index.html";
  std::string notification_url =
      "http://example.com/demos/ecommerce/index.html";
  std::string html =
      "<html><head>"
      "<link rel=\"stylesheet\" href=\"style.css\">"
      "</head><body><div class=\"hero\">Shop</div></body></html>";
  CacheOriginal(worker.cache(), cache_url, html);

  // Pre-populate CSS at the resolved path-only URL.  The href is relative,
  // so the worker resolves it against the page directory (/demos/ecommerce/)
  // to get /demos/ecommerce/style.css, with empty hostname (falls back to
  // notification.hostname which is also empty).
  std::string css_cache_url = "/demos/ecommerce/style.css";
  std::string css = ".hero { font-size: 24px; } .footer { display: none; }";
  CacheOriginal(worker.cache(), css_cache_url, css);

  // Run worker in background
  std::thread worker_thread([&worker]() { worker.Run(); });
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  // Send notification
  CacheNotification notification;
  notification.url = notification_url;
  notification.scheme = "https";
  notification.content_type = ContentType::kHtml;
  notification.capability_mask = 0;
  SendNotification(notification);

  // The sheet stays render-blocking, so there is no block to inline and
  // nothing else to change: the pass is skipped and no variant is written.
  CapabilityMask mask = CapabilityMask::Decode(0);
  for (int i = 0; i < 40 * kSanitizerBudgetScale; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    if (worker.stats().html_assembly_skipped.load() >= 1u) break;
  }
  EXPECT_GE(worker.stats().html_assembly_skipped.load(), 1u);
  EXPECT_TRUE(ReadVariant(worker.cache(), cache_url, mask).empty())
      << "no variant should carry a critical block next to a blocking sheet";

  // Verify early hints contain the resolved path-only URL (not relative).
  std::string hints =
      ReadSentinel(worker.cache(), cache_url, SentinelId::kEarlyHints);
  EXPECT_FALSE(hints.empty()) << "Preload hints not stored";
  if (!hints.empty()) {
    EXPECT_NE(hints.find("/demos/ecommerce/style.css"), std::string::npos)
        << "Hints should contain resolved URL, got: " << hints;
    EXPECT_EQ(hints.find("style.css\n"), std::string::npos)
        << "Hints should not contain raw relative href";
  }

  worker.Shutdown();
  worker_thread.join();
}

TEST_F(WorkerTest, RevalidationDoesNotDuplicateCriticalCss) {
  // On revalidation the worker re-processes its own output (because the
  // variant overwrites the original at the same AlternateId).  Previously-
  // injected <style data-pagespeed-critical> must not accumulate.
  WorkerConfig config;
  config.socket_path = socket_path_;
  config.cache_path = cache_path_;
  config.cache_size_bytes = static_cast<uint64_t>(10 * 1024 * 1024);

  config.unsafe_force_async_css =
      true;  // fixture page is not under test for the gate
  Worker worker(config, nullptr);
  ASSERT_TRUE(worker.Initialize());

  // Path-only URL for cache operations; full URL for notification.
  std::string cache_url = "/page.html";
  std::string notification_url = "http://example.com/page.html";
  std::string html =
      "<html><head>"
      "<link rel=\"stylesheet\" href=\"http://example.com/style.css\">"
      "</head><body><div class=\"hero\">Hello</div></body></html>";
  CacheOriginal(worker.cache(), cache_url, html);

  // CSS has absolute href — worker resolves as-is with UrlHostname.
  // raw_url=true skips normalization for external resource lookups.
  std::string css_url = "http://example.com/style.css";
  std::string css = ".hero { font-size: 24px; } .footer { display: none; }";
  CacheOriginal(worker.cache(), css_url, css, "example.com");

  std::thread worker_thread([&worker]() { worker.Run(); });
  WorkerStopper stopper(worker, worker_thread);
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  // First notification — produces variant with critical CSS.
  CacheNotification notification;
  notification.url = notification_url;
  notification.scheme = "https";
  notification.content_type = ContentType::kHtml;
  notification.capability_mask = 0;
  SendNotification(notification);

  CapabilityMask mask = CapabilityMask::Decode(0);
  std::string first_variant;
  for (int i = 0; i < 40 * kSanitizerBudgetScale; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    first_variant = ReadVariant(worker.cache(), cache_url, mask);
    if (!first_variant.empty()) break;
  }
  ASSERT_FALSE(first_variant.empty()) << "First variant not written";

  // Second notification — simulates revalidation.  The worker now reads
  // the variant (which already contains critical CSS) from cache.
  SendNotification(notification);

  // Wait for the second processing pass to complete.
  std::this_thread::sleep_for(std::chrono::milliseconds(2000));
  std::string second_variant = ReadVariant(worker.cache(), cache_url, mask);
  ASSERT_FALSE(second_variant.empty()) << "Second variant not written";

  // Count occurrences of the critical CSS marker.
  size_t count = 0;
  size_t pos = 0;
  while ((pos = second_variant.find("data-pagespeed-critical", pos)) !=
         std::string::npos) {
    ++count;
    pos += 23;
  }
  EXPECT_EQ(count, 1u) << "Expected exactly one data-pagespeed-critical tag, "
                          "found "
                       << count << " in:\n"
                       << second_variant;

  // The variant should not be substantially larger than the first pass
  // (small differences from re-minification are acceptable).
  EXPECT_LE(second_variant.size(), first_variant.size() + 50)
      << "Second variant grew unexpectedly — possible CSS duplication";
}

TEST_F(WorkerTest, RevalidationOnADefaultConfigCarriesNoCriticalCss) {
  // On revalidation the worker re-processes its own output (because the
  // variant overwrites the original at the same AlternateId).  Previously-
  // injected <style data-pagespeed-critical> must not accumulate.
  WorkerConfig config;
  config.socket_path = socket_path_;
  config.cache_path = cache_path_;
  config.cache_size_bytes = static_cast<uint64_t>(10 * 1024 * 1024);

  Worker worker(config, nullptr);
  ASSERT_TRUE(worker.Initialize());

  // Path-only URL for cache operations; full URL for notification.
  std::string cache_url = "/page.html";
  std::string notification_url = "http://example.com/page.html";
  std::string html =
      "<html><head>"
      "<link rel=\"stylesheet\" href=\"http://example.com/style.css\">"
      "</head><body><div class=\"hero\">Hello</div></body></html>";
  CacheOriginal(worker.cache(), cache_url, html);

  // CSS has absolute href - worker resolves as-is with UrlHostname.
  // raw_url=true skips normalization for external resource lookups.
  std::string css_url = "http://example.com/style.css";
  std::string css = ".hero { font-size: 24px; } .footer { display: none; }";
  CacheOriginal(worker.cache(), css_url, css, "example.com");

  std::thread worker_thread([&worker]() { worker.Run(); });
  WorkerStopper stopper(worker, worker_thread);
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  // First notification - produces variant with critical CSS.
  CacheNotification notification;
  notification.url = notification_url;
  notification.scheme = "https";
  notification.content_type = ContentType::kHtml;
  notification.capability_mask = 0;
  SendNotification(notification);

  CapabilityMask mask = CapabilityMask::Decode(0);
  std::string first_variant;
  for (int i = 0; i < 40 * kSanitizerBudgetScale; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    first_variant = ReadVariant(worker.cache(), cache_url, mask);
    if (!first_variant.empty()) break;
  }
  ASSERT_FALSE(first_variant.empty()) << "First variant not written";

  // Second notification - simulates revalidation.  The worker now reads
  // the variant (which already contains critical CSS) from cache.
  SendNotification(notification);

  // Wait for the second processing pass to complete.
  std::this_thread::sleep_for(std::chrono::milliseconds(2000));
  std::string second_variant = ReadVariant(worker.cache(), cache_url, mask);
  ASSERT_FALSE(second_variant.empty()) << "Second variant not written";

  // Count occurrences of the critical CSS marker.
  size_t count = 0;
  size_t pos = 0;
  while ((pos = second_variant.find("data-pagespeed-critical", pos)) !=
         std::string::npos) {
    ++count;
    pos += 23;
  }
  EXPECT_EQ(count, 0u) << "Expected no data-pagespeed-critical tag, "
                          "found "
                       << count << " in:\n"
                       << second_variant;

  // The variant should not be substantially larger than the first pass
  // (small differences from re-minification are acceptable).
  EXPECT_LE(second_variant.size(), first_variant.size() + 50)
      << "Second variant grew unexpectedly - possible CSS duplication";
}

TEST_F(WorkerTest, HtmlNotificationNoCssNoVariant) {
  // HTML with no CSS at all — no variant should be written.
  WorkerConfig config;
  config.socket_path = socket_path_;
  config.cache_path = cache_path_;
  config.cache_size_bytes = static_cast<uint64_t>(10 * 1024 * 1024);

  Worker worker(config, nullptr);
  ASSERT_TRUE(worker.Initialize());

  // Pre-populate cache with HTML that has no CSS
  std::string url = "http://example.com/no-css.html";
  std::string html = "<html><head></head><body><p>Hello</p></body></html>";
  CacheOriginal(worker.cache(), url, html);

  // Run worker in background
  std::thread worker_thread([&worker]() { worker.Run(); });
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  // Send notification
  CacheNotification notification;
  notification.url = url;
  notification.scheme = "https";
  notification.content_type = ContentType::kHtml;
  notification.capability_mask = 0;
  SendNotification(notification);

  // Give worker time to process
  std::this_thread::sleep_for(std::chrono::milliseconds(200));

  // Verify no variant was written (no CSS to inject)
  CapabilityMask mask = CapabilityMask::Decode(0);
  std::string variant = ReadVariant(worker.cache(), url, mask);
  EXPECT_TRUE(variant.empty());

  worker.Shutdown();
  worker_thread.join();
}

TEST_F(WorkerTest, HtmlRevalidationWhenExternalCssMissing) {
  // Verifies the critical CSS revalidation flow:
  //   1. HTML references an external stylesheet not yet in cache.
  //   2. Worker writes HTML variant with kFlagNeedsRevalidation set
  //      (other transforms applied, but no critical CSS).
  //   3. CSS is added to cache.
  //   4. Second notification (simulating nginx re-notify on HIT).
  //   5. Worker writes updated variant WITH critical CSS, flag cleared.
  WorkerConfig config;
  config.socket_path = socket_path_;
  config.cache_path = cache_path_;
  config.cache_size_bytes = static_cast<uint64_t>(10 * 1024 * 1024);
  // Disable RAM cache: this single-process test writes CSS via
  // CacheOriginal() and expects the worker to see it on the next
  // disk read.  Write-around RAM cache can return stale state for
  // same-process writes.  In production, CSS is written by nginx
  // (different process / different RAM cache), so this is safe.
  config.ram_cache_size = 0;
  config.unsafe_force_async_css =
      true;  // fixture page is not under test for the gate

  Worker worker(config, nullptr);
  ASSERT_TRUE(worker.Initialize());

  std::string url = "http://example.com/page.html";
  std::string css_url = "http://example.com/style.css";
  // Use multiple images so lazy-load applies to non-LCP images.
  std::string html =
      "<html><head>"
      "<link rel=\"stylesheet\" href=\"" +
      css_url +
      "\">"
      "</head><body>"
      "<div class=\"hero\">Hello</div>"
      "<img src=\"hero.jpg\">"
      "<img src=\"photo1.jpg\">"
      "<img src=\"photo2.jpg\">"
      "<img src=\"photo3.jpg\">"
      "<img src=\"photo4.jpg\">"
      "</body></html>";

  // Only cache the HTML — CSS is NOT in cache yet.
  CacheOriginal(worker.cache(), url, html);

  // Run worker in background.
  std::thread worker_thread([&worker]() { worker.Run(); });
  WorkerStopper stopper(worker, worker_thread);
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  // --- Phase 1: Send notification with CSS missing ---
  CacheNotification notification;
  notification.url = url;
  notification.scheme = "https";
  notification.content_type = ContentType::kHtml;
  notification.capability_mask = 0;  // Mobile/Identity
  SendNotification(notification);

  // Poll for variant to appear.
  CapabilityMask mask = CapabilityMask::Decode(0);
  std::optional<VariantWithMeta> phase1;
  for (int i = 0; i < 40 * kSanitizerBudgetScale; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    phase1 = ReadVariantWithMeta(worker.cache(), url, mask);
    if (phase1.has_value()) break;
  }

  // Variant should exist (fetchpriority + lazy-load still applies).
  ASSERT_TRUE(phase1.has_value()) << "Phase 1: HTML variant not written";
  // Should NOT contain critical CSS (external stylesheet was missing).
  EXPECT_EQ(phase1->content.find("data-pagespeed-critical"), std::string::npos)
      << "Phase 1: critical CSS should not be injected without stylesheet";
  // First img gets fetchpriority="high" (LCP heuristic), later imgs
  // get lazy-loaded.
  EXPECT_NE(phase1->content.find("fetchpriority=\"high\""), std::string::npos)
      << "Phase 1: fetchpriority should be applied to first image";
  EXPECT_NE(phase1->content.find("loading=\"lazy\""), std::string::npos)
      << "Phase 1: lazy-load should be applied to later images";
  // kFlagNeedsRevalidation should be SET.
  EXPECT_NE(phase1->metadata.flags & AlternateMetadata::kFlagNeedsRevalidation,
            0)
      << "Phase 1: kFlagNeedsRevalidation should be set";

  // --- Phase 2: Add CSS to cache, re-notify ---
  std::string css =
      ".hero { font-size: 24px; color: red; } "
      ".footer { display: none; margin: 10px; }";
  CacheOriginal(worker.cache(), css_url, css, "example.com");

  // Poll until the variant is updated.  Re-send notifications
  // periodically (every 1s) to simulate nginx re-notifying on each
  // revalidation HIT.  The worker's revalidation cooldown (3s) suppresses
  // some of these, matching production behavior where CSS convergence
  // takes a few seconds after the stylesheet becomes available.
  // With RAM cache disabled, all reads go directly to disk (mmap).
  std::optional<VariantWithMeta> phase2;
  for (int i = 0; i < 80 * kSanitizerBudgetScale; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    // Re-notify every 1s (simulates nginx re-notification on HIT).
    if (i % 10 == 0) SendNotification(notification);
    phase2 = ReadVariantWithMeta(worker.cache(), url, mask);
    if (phase2.has_value() &&
        phase2->content.find("data-pagespeed-critical") != std::string::npos) {
      break;
    }
  }

  // Variant should now contain critical CSS.
  ASSERT_TRUE(phase2.has_value()) << "Phase 2: HTML variant not written";
  EXPECT_NE(phase2->content.find("data-pagespeed-critical"), std::string::npos)
      << "Phase 2: critical CSS should be injected after stylesheet cached";
  // The hero class should be in the critical CSS.
  EXPECT_NE(phase2->content.find("hero"), std::string::npos)
      << "Phase 2: hero class should be in critical CSS";
  // kFlagNeedsRevalidation should be CLEARED.
  EXPECT_EQ(phase2->metadata.flags & AlternateMetadata::kFlagNeedsRevalidation,
            0)
      << "Phase 2: kFlagNeedsRevalidation should be cleared";
  // Other transforms should still be applied.
  EXPECT_NE(phase2->content.find("fetchpriority=\"high\""), std::string::npos)
      << "Phase 2: fetchpriority should still be applied";
  EXPECT_NE(phase2->content.find("loading=\"lazy\""), std::string::npos)
      << "Phase 2: lazy-load should still be applied";
}

TEST_F(WorkerTest, HtmlRevalidationWhenExternalCssMissingOnADefaultConfig) {
  // Verifies the critical CSS revalidation flow:
  //   1. HTML references an external stylesheet not yet in cache.
  //   2. Worker writes HTML variant with kFlagNeedsRevalidation set
  //      (other transforms applied, but no critical CSS).
  //   3. CSS is added to cache.
  //   4. Second notification (simulating nginx re-notify on HIT).
  //   5. Worker writes updated variant WITHOUT critical CSS (the sheet stays
  //      render-blocking on a default config), flag cleared.
  WorkerConfig config;
  config.socket_path = socket_path_;
  config.cache_path = cache_path_;
  config.cache_size_bytes = static_cast<uint64_t>(10 * 1024 * 1024);
  // Disable RAM cache: this single-process test writes CSS via
  // CacheOriginal() and expects the worker to see it on the next
  // disk read.  Write-around RAM cache can return stale state for
  // same-process writes.  In production, CSS is written by nginx
  // (different process / different RAM cache), so this is safe.
  config.ram_cache_size = 0;

  Worker worker(config, nullptr);
  ASSERT_TRUE(worker.Initialize());

  std::string url = "http://example.com/page.html";
  std::string css_url = "http://example.com/style.css";
  // Use multiple images so lazy-load applies to non-LCP images.
  std::string html =
      "<html><head>"
      "<link rel=\"stylesheet\" href=\"" +
      css_url +
      "\">"
      "</head><body>"
      "<div class=\"hero\">Hello</div>"
      "<img src=\"hero.jpg\">"
      "<img src=\"photo1.jpg\">"
      "<img src=\"photo2.jpg\">"
      "<img src=\"photo3.jpg\">"
      "<img src=\"photo4.jpg\">"
      "</body></html>";

  // Only cache the HTML - CSS is NOT in cache yet.
  CacheOriginal(worker.cache(), url, html);

  // Run worker in background.
  std::thread worker_thread([&worker]() { worker.Run(); });
  WorkerStopper stopper(worker, worker_thread);
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  // --- Phase 1: Send notification with CSS missing ---
  CacheNotification notification;
  notification.url = url;
  notification.scheme = "https";
  notification.content_type = ContentType::kHtml;
  notification.capability_mask = 0;  // Mobile/Identity
  SendNotification(notification);

  // Poll for variant to appear.
  CapabilityMask mask = CapabilityMask::Decode(0);
  std::optional<VariantWithMeta> phase1;
  for (int i = 0; i < 40 * kSanitizerBudgetScale; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    phase1 = ReadVariantWithMeta(worker.cache(), url, mask);
    if (phase1.has_value()) break;
  }

  // Variant should exist (fetchpriority + lazy-load still applies).
  ASSERT_TRUE(phase1.has_value()) << "Phase 1: HTML variant not written";
  // Should NOT contain critical CSS (external stylesheet was missing).
  EXPECT_EQ(phase1->content.find("data-pagespeed-critical"), std::string::npos)
      << "Phase 1: critical CSS should not be injected without stylesheet";
  // First img gets fetchpriority="high" (LCP heuristic), later imgs
  // get lazy-loaded.
  EXPECT_NE(phase1->content.find("fetchpriority=\"high\""), std::string::npos)
      << "Phase 1: fetchpriority should be applied to first image";
  EXPECT_NE(phase1->content.find("loading=\"lazy\""), std::string::npos)
      << "Phase 1: lazy-load should be applied to later images";
  // kFlagNeedsRevalidation should be SET.
  EXPECT_NE(phase1->metadata.flags & AlternateMetadata::kFlagNeedsRevalidation,
            0)
      << "Phase 1: kFlagNeedsRevalidation should be set";

  // --- Phase 2: Add CSS to cache, re-notify ---
  std::string css =
      ".hero { font-size: 24px; color: red; } "
      ".footer { display: none; margin: 10px; }";
  CacheOriginal(worker.cache(), css_url, css, "example.com");

  // Poll until the variant is updated.  Re-send notifications
  // periodically (every 1s) to simulate nginx re-notifying on each
  // revalidation HIT.  The worker's revalidation cooldown (3s) suppresses
  // some of these, matching production behavior where CSS convergence
  // takes a few seconds after the stylesheet becomes available.
  // With RAM cache disabled, all reads go directly to disk (mmap).
  std::optional<VariantWithMeta> phase2;
  for (int i = 0; i < 80 * kSanitizerBudgetScale; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    // Re-notify every 1s (simulates nginx re-notification on HIT).
    if (i % 10 == 0) SendNotification(notification);
    phase2 = ReadVariantWithMeta(worker.cache(), url, mask);
    if (phase2.has_value() &&
        (phase2->metadata.flags & AlternateMetadata::kFlagNeedsRevalidation) ==
            0) {
      break;
    }
  }

  // With the sheet cached and no confirmation for the page, the sheet stays
  // render-blocking and the block that the cold pass carried is gone.
  ASSERT_TRUE(phase2.has_value()) << "Phase 2: HTML variant not written";
  EXPECT_EQ(phase2->content.find("data-pagespeed-critical"), std::string::npos)
      << "Phase 2: no critical block next to a render-blocking sheet";
  // kFlagNeedsRevalidation should be CLEARED.
  EXPECT_EQ(phase2->metadata.flags & AlternateMetadata::kFlagNeedsRevalidation,
            0)
      << "Phase 2: kFlagNeedsRevalidation should be cleared";
  // Other transforms should still be applied.
  EXPECT_NE(phase2->content.find("fetchpriority=\"high\""), std::string::npos)
      << "Phase 2: fetchpriority should still be applied";
  EXPECT_NE(phase2->content.find("loading=\"lazy\""), std::string::npos)
      << "Phase 2: lazy-load should still be applied";
}

TEST_F(WorkerTest, AsyncCssSuppressedWhenExternalCssMissing) {
  // Cold-cache FOUC fail-safe (the iispeed.com regression): the page declares
  // an external <link rel=stylesheet> that is NOT yet cached AND carries a small
  // inline <style> (e.g. a dark-mode override). The heuristic extractor echoes
  // the inline rules as "critical CSS", so critical CSS IS injected — but the
  // deferred-byte denominator collapsed to the inline-only blob. Without the
  // guard, the small-sheet escape hatch would fire and async-defer the
  // unmeasured external sheet (preload swap) -> the page renders unstyled
  // until it loads. The guard must keep the <link> render-blocking AND mark the
  // variant for revalidation (even though critical CSS was injected) so it
  // self-heals once the sheet caches.
  //
  // A validated browser profile is seeded so phase 2 CAN defer: without one the
  // empirical gate refuses regardless, and the test could no longer tell the
  // cold-cache veto apart from a page that simply never defers. The profile's
  // validation is bound to the WARM combined sheet, which is what phase 2
  // assembles.
  WorkerConfig config = BrowserProfileConfig();
  config.ram_cache_size = 0;  // single-process test; see sibling test above.

  Worker worker(config, nullptr);
  ASSERT_TRUE(worker.Initialize());

  std::string url = "http://example.com/cold.html";
  std::string css_url = "http://example.com/big.css";
  // Inline <style> overrides (echoed as critical) + a .hero rule that matches an
  // above-the-fold element, guaranteeing the heuristic injects critical CSS.
  std::string html =
      "<html><head>"
      "<style>html.dark,html.dark body{background:#0c0a09}"
      ".hero{color:red}</style>"
      "<link rel=\"stylesheet\" href=\"" +
      css_url +
      "\">"
      "</head><body>"
      "<div class=\"hero\">Hello</div>"
      "<img src=\"hero.jpg\">"
      "</body></html>";

  // Only cache the HTML — the external CSS is NOT in cache yet (cold).
  CacheOriginal(worker.cache(), url, html);

  std::string big_css =
      ".hero{color:red;font-size:24px}.footer{display:none}"
      "body{margin:0;font-family:system-ui}a{color:blue}";
  // Two combined sheets, because the two phases assemble different bytes: cold
  // is the page's inline CSS alone (the external sheet was never gathered),
  // warm is that plus the external sheet, newline-joined.
  HtmlScanner profile_scanner;
  HtmlScanResult profile_scan = profile_scanner.Scan(url, html);
  ASSERT_TRUE(profile_scan.success);
  const std::string cold_combined_css = profile_scan.inline_css;
  const std::string warm_combined_css =
      profile_scan.inline_css + "\n" + big_css;

  // Phase 1 seeds against the COLD bytes on purpose. It makes the empirical
  // gate SATISFIED while the sheet is missing, so external_css_missing is the
  // only thing left that can refuse — which is the fail-safe under test. Seed
  // the warm bytes here instead and the empirical gate would refuse too, and
  // phase 1 would pass even with the cold-cache guard deleted.
  StoreBrowserProfile(worker, url, html, ".hero{color:red}",
                      /*validated=*/true,
                      /*validated_against_css=*/cold_combined_css);

  std::thread worker_thread([&worker]() { worker.Run(); });
  WorkerStopper stopper(worker, worker_thread);
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  // --- Phase 1: external CSS cold ---
  CacheNotification notification;
  notification.url = url;
  notification.scheme = "https";
  notification.content_type = ContentType::kHtml;
  notification.capability_mask = 0;
  SendNotification(notification);

  CapabilityMask mask = CapabilityMask::Decode(0);
  std::optional<VariantWithMeta> phase1;
  for (int i = 0; i < 40 * kSanitizerBudgetScale; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    phase1 = ReadVariantWithMeta(worker.cache(), url, mask);
    if (phase1.has_value()) break;
  }
  ASSERT_TRUE(phase1.has_value()) << "Phase 1: HTML variant not written";

  // The inline override IS echoed as critical CSS (this is the echo that fooled
  // the old gate into thinking the deferred sheet was tiny).
  EXPECT_NE(phase1->content.find("data-pagespeed-critical"), std::string::npos)
      << "Phase 1: heuristic echo should still inline critical CSS";
  // But the external sheet must NOT be async-deferred (the fail-safe).
  EXPECT_EQ(phase1->content.find("as=\"style\""), std::string::npos)
      << "Phase 1: cold external sheet must NOT be async-deferred (FOUC guard)";
  EXPECT_EQ(phase1->content.find("data-pagespeed-async"), std::string::npos)
      << "Phase 1: no async markers when the deferred sheet is unmeasured";
  // The original render-blocking <link> survives so the page stays styled.
  EXPECT_NE(phase1->content.find("rel=\"stylesheet\""), std::string::npos)
      << "Phase 1: render-blocking external <link> must be preserved";
  // Self-heal: revalidation MUST be set even though critical CSS was injected.
  // (The pre-fix gate cleared it here, making the cold result sticky.)
  EXPECT_NE(phase1->metadata.flags & AlternateMetadata::kFlagNeedsRevalidation,
            0)
      << "Phase 1: kFlagNeedsRevalidation must be set despite injected "
         "critical "
         "CSS, so the cold-cache decision self-heals";

  // --- Phase 2: cache the external CSS, re-notify -> converges ---
  CacheOriginal(worker.cache(), css_url, big_css, "example.com");
  // Re-seed against the WARM bytes: the combined sheet genuinely changed when
  // the external sheet joined it, so the phase-1 record no longer describes
  // what is being served. Re-validating is exactly what a real analysis would
  // do, and it keeps phase 2 a test of the cold-cache veto lifting rather than
  // of the hash gate.
  StoreBrowserProfile(worker, url, html, ".hero{color:red}",
                      /*validated=*/true,
                      /*validated_against_css=*/warm_combined_css);

  std::optional<VariantWithMeta> phase2;
  for (int i = 0; i < 80 * kSanitizerBudgetScale; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    if (i % 10 == 0) SendNotification(notification);
    phase2 = ReadVariantWithMeta(worker.cache(), url, mask);
    // Wait for the self-heal: async-CSS re-fires (preload swap) once the
    // sheet is cached.
    if (phase2.has_value() &&
        phase2->content.find("as=\"style\"") != std::string::npos) {
      break;
    }
  }
  ASSERT_TRUE(phase2.has_value()) << "Phase 2: HTML variant not written";
  // Self-heal proof: with the SAME small byte sizes as phase 1, the only thing
  // that changed is that the sheet is now cached (external_css_missing flipped
  // false). That alone re-enables async-CSS — proving the cold-cache veto, not
  // some other factor, gated phase 1.
  EXPECT_NE(phase2->content.find("as=\"style\""), std::string::npos)
      << "Phase 2: async-CSS should re-fire once the external sheet caches";
  // And the revalidation flag clears (the veto lifted).
  EXPECT_EQ(phase2->metadata.flags & AlternateMetadata::kFlagNeedsRevalidation,
            0)
      << "Phase 2: kFlagNeedsRevalidation should clear once the sheet caches";
}

TEST_F(WorkerTest, AsyncCssGathersTheSheetFromTheBaseHrefHost) {
  // A page whose <base href> points at another host declares its
  // stylesheet with a root-relative href. The browser fetches it from the
  // <base> host; the combined-stylesheet gather used to resolve it against the
  // page directory and read it from the page host, where it is not, so the
  // sheet counted as missing on both the serve and the analysis side, and the
  // page never deferred. Now both sides resolve it through the scan's
  // DocumentBase and read the CDN host's sheet.
  //
  // End to end, with the serve/analysis parity made explicit: the record is
  // seeded against the bytes the ANALYSIS side's own builder (the worker's
  // BuildCombinedCss as wired into the browser manager) assembles, and the
  // SERVE path has to accept it, which it only does when it gathers the same
  // sheet from the same host and hashes the same bytes.
  WorkerConfig config = BrowserProfileConfig();
  config.ram_cache_size = 0;  // single-process test; see the sibling tests.

  Worker worker(config, nullptr);
  ASSERT_TRUE(worker.Initialize());

  const std::string hostname = "example.com";
  const std::string url = "https://example.com/base.html";
  const std::string html =
      "<html><head>"
      "<base href=\"https://cdn.example.com/assets/\">"
      "<style>.hero{color:red}</style>"
      "<link rel=\"stylesheet\" href=\"/css/site.css\">"
      "</head><body>"
      "<div class=\"hero\">Hello</div>"
      "<img src=\"hero.jpg\">"
      "</body></html>";
  const std::string sheet_css =
      ".hero{color:red;font-size:24px}.footer{display:none}"
      "body{margin:0;font-family:system-ui}a{color:blue}";

  CacheOriginal(worker.cache(), url, html, hostname);
  // The sheet lives on the CDN host, at the root-relative path. Nothing is
  // cached for the page host's /css/site.css.
  CacheOriginal(worker.cache(), "https://cdn.example.com/css/site.css",
                sheet_css, "cdn.example.com");

  // The analysis side: the manager's builder is the worker's own assembly,
  // handed the item as the manager holds it (the cache-normalized path, with
  // the hostname and scheme beside it), as BuildValidationInputs hands it.
  const std::string item_url = StripSchemeAuthority(url);
  HtmlScanner scanner;
  HtmlScanResult scan = scanner.Scan(item_url, html);
  ASSERT_TRUE(scan.success);
  ASSERT_NE(worker.TestBrowserManager(), nullptr);
  const auto& builder = worker.TestBrowserManager()->TestCombinedCssBuilder();
  ASSERT_TRUE(builder) << "the worker wires BuildCombinedCss into the manager";
  BrowserAnalysisManager::CombinedCss analysis_side =
      builder(scan, item_url, hostname, "https", /*site=*/"",
              /*host_vouched=*/false, /*from_module=*/false);
  EXPECT_FALSE(analysis_side.external_css_missing)
      << "the CDN host's sheet is in cache";
  EXPECT_EQ(scan.inline_css + "\n" + sheet_css, analysis_side.css);

  StoreBrowserProfile(worker, url, html, ".hero{color:red}",
                      /*validated=*/true,
                      /*validated_against_css=*/analysis_side.css);

  std::thread worker_thread([&worker]() { worker.Run(); });
  WorkerStopper stopper(worker, worker_thread);
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  CacheNotification notification;
  notification.url = url;
  notification.hostname = hostname;
  notification.scheme = "https";
  notification.content_type = ContentType::kHtml;
  notification.capability_mask = 0;
  SendNotification(notification);

  CapabilityMask mask = CapabilityMask::Decode(0);
  std::optional<VariantWithMeta> variant;
  for (int i = 0; i < 40 * kSanitizerBudgetScale; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    variant = ReadVariantWithMeta(worker.cache(), url, mask, hostname);
    if (variant.has_value()) break;
  }
  ASSERT_TRUE(variant.has_value()) << "HTML variant not written";

  // The serve path gathered the CDN host's sheet, hashed the same bytes the
  // analysis side did, and so accepted the record: the sheet is deferred.
  EXPECT_NE(variant->content.find("data-pagespeed-critical"), std::string::npos)
      << variant->content;
  EXPECT_NE(variant->content.find("as=\"style\""), std::string::npos)
      << "the stylesheet should be async-deferred: " << variant->content;
  EXPECT_NE(variant->content.find("data-pagespeed-async"), std::string::npos)
      << variant->content;
  // No declared sheet was missing, so nothing to revalidate.
  EXPECT_EQ(variant->metadata.flags & AlternateMetadata::kFlagNeedsRevalidation,
            0)
      << "the sheet was gathered; the variant must not be marked for "
         "revalidation";
}

TEST_F(WorkerTest, AsyncCssSuppressedWhenOneOfMultipleSheetsMissing) {
  // Multi-sheet partial miss: one sheet cached, one missing. external_css_missing
  // accumulates across the whole gather loop, so async must stay suppressed and
  // the (same-origin) miss marks the variant for revalidation. Locks the
  // OR-accumulate semantics against a future refactor.
  WorkerConfig config;
  config.socket_path = socket_path_;
  config.cache_path = cache_path_;
  config.cache_size_bytes = static_cast<uint64_t>(10 * 1024 * 1024);
  config.ram_cache_size = 0;

  Worker worker(config, nullptr);
  ASSERT_TRUE(worker.Initialize());

  std::string url = "http://example.com/multi.html";
  std::string found_url = "http://example.com/found.css";
  std::string missing_url = "http://example.com/missing.css";
  std::string html =
      "<html><head>"
      "<style>.hero{color:red}</style>"
      "<link rel=\"stylesheet\" href=\"" +
      found_url + "\">" + "<link rel=\"stylesheet\" href=\"" + missing_url +
      "\">"
      "</head><body><div class=\"hero\">Hello</div></body></html>";
  CacheOriginal(worker.cache(), url, html);
  // Cache ONLY the first sheet.
  CacheOriginal(worker.cache(), found_url, ".hero{color:red;font-size:20px}",
                "example.com");

  std::thread worker_thread([&worker]() { worker.Run(); });
  WorkerStopper stopper(worker, worker_thread);
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  CacheNotification notification;
  notification.url = url;
  notification.scheme = "https";
  notification.content_type = ContentType::kHtml;
  notification.capability_mask = 0;
  SendNotification(notification);

  CapabilityMask mask = CapabilityMask::Decode(0);
  std::optional<VariantWithMeta> v;
  for (int i = 0; i < 40 * kSanitizerBudgetScale; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    v = ReadVariantWithMeta(worker.cache(), url, mask);
    if (v.has_value()) break;
  }
  ASSERT_TRUE(v.has_value()) << "HTML variant not written";
  // One sheet still missing -> async suppressed (no preload primitive) ...
  EXPECT_EQ(v->content.find("as=\"style\""), std::string::npos)
      << "async must stay suppressed while ANY declared sheet is uncached";
  // ... and revalidation set (the miss is same-origin, so it can self-heal).
  EXPECT_NE(v->metadata.flags & AlternateMetadata::kFlagNeedsRevalidation, 0)
      << "same-origin partial miss should mark the variant for revalidation";
}

TEST_F(WorkerTest,
       CrossOriginMissingSheetSuppressesAsyncWithoutRevalidationChurn) {
  // A cross-origin stylesheet (e.g. Google Fonts) can NEVER enter the worker's
  // cache. async-CSS must stay suppressed (we cannot measure it -> no FOUC),
  // but the variant must NOT be marked for revalidation — otherwise it would
  // re-process the HTML on every request forever without ever resolving.
  WorkerConfig config;
  config.socket_path = socket_path_;
  config.cache_path = cache_path_;
  config.cache_size_bytes = static_cast<uint64_t>(10 * 1024 * 1024);
  config.ram_cache_size = 0;

  Worker worker(config, nullptr);
  ASSERT_TRUE(worker.Initialize());

  std::string url = "http://example.com/fonts.html";
  std::string html =
      "<html><head>"
      "<style>.hero{color:red}</style>"
      "<link rel=\"stylesheet\" "
      "href=\"https://fonts.googleapis.com/css?family=Roboto\">"
      "</head><body><div class=\"hero\">Hello</div>"
      "<img src=\"hero.jpg\"></body></html>";
  // Set the page host explicitly so the cross-origin sheet is actually detected
  // as cross-origin (a missing same-origin sheet would, by design, revalidate).
  std::string hostname = "example.com";
  CacheOriginal(worker.cache(), url, html, hostname);

  std::thread worker_thread([&worker]() { worker.Run(); });
  WorkerStopper stopper(worker, worker_thread);
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  CacheNotification notification;
  notification.url = url;
  notification.scheme = "https";
  notification.hostname = hostname;
  notification.content_type = ContentType::kHtml;
  notification.capability_mask = 0;
  SendNotification(notification);

  CapabilityMask mask = CapabilityMask::Decode(0);
  std::optional<VariantWithMeta> v;
  for (int i = 0; i < 40 * kSanitizerBudgetScale; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    v = ReadVariantWithMeta(worker.cache(), url, mask, hostname);
    if (v.has_value()) break;
  }
  ASSERT_TRUE(v.has_value()) << "HTML variant not written";
  // Fail-safe: the unmeasured cross-origin sheet is NOT async-deferred.
  EXPECT_EQ(v->content.find("as=\"style\""), std::string::npos)
      << "cross-origin unmeasured sheet must not be async-deferred";
  // No churn: a sheet that can never cache must NOT trigger revalidation.
  EXPECT_EQ(v->metadata.flags & AlternateMetadata::kFlagNeedsRevalidation, 0)
      << "cross-origin (uncacheable) miss must NOT mark the variant for "
         "revalidation (would re-process forever)";
}

TEST_F(WorkerTest, RevalidationDoesNotDuplicatePreconnectLinks) {
  // On revalidation, previously-injected <link rel="preconnect"
  // data-pagespeed-hint> must not accumulate.
  WorkerConfig config;
  config.socket_path = socket_path_;
  config.cache_path = cache_path_;
  config.cache_size_bytes = static_cast<uint64_t>(10 * 1024 * 1024);

  Worker worker(config, nullptr);
  ASSERT_TRUE(worker.Initialize());

  // HTML with an external stylesheet from a third-party origin.
  std::string url = "http://example.com/page.html";
  std::string html =
      "<html><head>"
      "<link rel=\"stylesheet\" "
      "href=\"https://fonts.googleapis.com/css?family=Inter\">"
      "</head><body><div class=\"hero\">Hello</div></body></html>";
  CacheOriginal(worker.cache(), url, html);

  // Cache the external CSS so processing succeeds.
  std::string css_url = "https://fonts.googleapis.com/css?family=Inter";
  std::string css = ".hero { font-size: 24px; }";
  CacheOriginal(worker.cache(), css_url, css, "fonts.googleapis.com");

  std::thread worker_thread([&worker]() { worker.Run(); });
  WorkerStopper stopper(worker, worker_thread);
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  // First notification.
  CacheNotification notification;
  notification.url = url;
  notification.scheme = "https";
  notification.content_type = ContentType::kHtml;
  notification.capability_mask = 0;
  SendNotification(notification);

  CapabilityMask mask = CapabilityMask::Decode(0);
  std::string first_variant;
  for (int i = 0; i < 40 * kSanitizerBudgetScale; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    first_variant = ReadVariant(worker.cache(), url, mask);
    if (!first_variant.empty()) break;
  }
  ASSERT_FALSE(first_variant.empty()) << "First variant not written";

  // Second notification — simulates revalidation.
  SendNotification(notification);

  std::this_thread::sleep_for(std::chrono::milliseconds(2000));
  std::string second_variant = ReadVariant(worker.cache(), url, mask);
  ASSERT_FALSE(second_variant.empty()) << "Second variant not written";

  // Count preconnect occurrences — should be at most 1 per origin.
  size_t count = 0;
  size_t pos = 0;
  while ((pos = second_variant.find("rel=\"preconnect\"", pos)) !=
         std::string::npos) {
    ++count;
    pos += 16;
  }
  // Expect at most 1 preconnect for fonts.googleapis.com.
  EXPECT_LE(count, 1u) << "Expected at most 1 preconnect link, found " << count
                       << " in:\n"
                       << second_variant;
}

// =============================================================================
// Issue E: wire the dead policy counters + split benign purge-fences from hard
// write failures.
// =============================================================================

TEST_F(WorkerTest, PolicyCountersIncrementWhenAsyncCssApplied) {
  // The policy.* counters (computed/async_css_enabled/script_deferral_enabled)
  // are read by the dashboard but had ZERO production increment sites — they
  // read 0 on prod even though async CSS is applied + served.  When the worker
  // applies a transform with critical CSS AND the empirical gate authorizes
  // deferral, policy_computed and policy_async_css_enabled must increment.
  WorkerConfig config = BrowserProfileConfig();

  Worker worker(config, nullptr);
  ASSERT_TRUE(worker.Initialize());

  // Inline CSS so the extractor yields critical CSS; a validated profile bound
  // to that same inline CSS (the whole combined sheet here) is what makes
  // has_async_css true.
  std::string html =
      "<html><head><style>body { margin: 0; } "
      "h1 { color: red; }</style></head>"
      "<body><h1>Hello World</h1></body></html>";
  std::string cache_url = "/policy.html";
  std::string notification_url = "http://example.com/policy.html";
  CacheOriginal(worker.cache(), cache_url, html);

  HtmlScanner policy_scanner;
  HtmlScanResult policy_scan = policy_scanner.Scan(notification_url, html);
  ASSERT_TRUE(policy_scan.success);
  StoreBrowserProfile(worker, notification_url, html, "h1{color:red}",
                      /*validated=*/true,
                      /*validated_against_css=*/policy_scan.inline_css);

  std::thread worker_thread([&worker]() { worker.Run(); });
  WorkerStopper stopper(worker, worker_thread);
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  CacheNotification notification;
  notification.url = notification_url;
  notification.scheme = "https";
  notification.content_type = ContentType::kHtml;
  notification.capability_mask = CapabilityMask().Encode();
  SendNotification(notification);

  CapabilityMask mask;  // Desktop/Identity
  std::string variant;
  for (int i = 0; i < 60 * kSanitizerBudgetScale; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    variant = ReadVariant(worker.cache(), cache_url, mask);
    if (!variant.empty()) break;
  }
  ASSERT_FALSE(variant.empty()) << "HTML variant not written (transform must "
                                   "run for the policy counters to fire)";

  EXPECT_GE(worker.stats().policy_computed.load(), 1u)
      << "policy_computed must increment once a policy decision is made";
  EXPECT_GE(worker.stats().policy_async_css_enabled.load(), 1u)
      << "policy_async_css_enabled must increment when async CSS is applied";
}

// =============================================================================
// Measured fold: the browser's per-viewport render replaces the element-count
// estimate, end to end through the serve path.
// =============================================================================

namespace measured_fold {

constexpr const char* kCssUrl = "http://example.com/measured.css";
// `.flex` is what a real browser measured above the fold; `.mt-96` is the
// control — same document, same distance past the estimate, NOT measured.
constexpr const char* kCss =
    ".flex{display:flex}.mt-96{margin:24rem}p{margin:0}";
// The profile's raw coverage blob. It deliberately does NOT mention `.flex`:
// coverage identities are a separate force-include channel, and if this named
// `.flex` the rule would be retained whether or not the measured fold works.
constexpr const char* kProfileCriticalCss = "p{margin:0}";

// A long page. Three hundred paragraphs exhaust the extractor's body-element
// estimate before the two divs of interest, so both are judged below the fold
// by document position alone. (The original live shape was a head-heavy page,
// thirty <meta> elements exhausting a 25-element estimate counted from <html>;
// the estimate is now counted from <body>, so the fillers moved there.)
std::string PageHtml() {
  std::string html = "<html><head><link rel=\"stylesheet\" href=\"";
  html += kCssUrl;
  html += "\"></head><body>";
  for (int i = 0; i < 300; ++i) html += "<p>x</p>";
  html +=
      "<div class=\"flex\">a</div>"
      "<div class=\"mt-96\">b</div></body></html>";
  return html;
}

// Seed a profile for `html`'s template with the given measured fold on every
// viewport.
void StoreProfileWithMeasuredFold(
    Worker& worker, const std::string& url, const std::string& html,
    const std::vector<std::string>& above_fold_selectors) {
  HtmlScanner scanner;
  HtmlScanResult scan = scanner.Scan(url, html);
  ASSERT_TRUE(scan.success);

  ViewportProfile vp;
  vp.critical_css = kProfileCriticalCss;
  // Under the 0.60 inline budget, so the block is kept rather than suppressed.
  vp.css_coverage_ratio = 0.2f;
  vp.total_css_bytes = 5000;
  vp.unused_css_bytes = 4000;
  vp.above_fold_selectors = above_fold_selectors;

  OptimizationProfile profile;
  profile.template_hash_hex = "measured";
  profile.analyzed_url = url;
  profile.mobile = vp;
  profile.tablet = vp;
  profile.desktop = vp;
  profile.created_at = 0;
  profile.expires_at = 0;  // no TTL — LookupProfile keeps it

  ASSERT_NE(worker.TestBrowserManager(), nullptr);
  worker.TestBrowserManager()->TestStoreProfile(
      TemplateDetector::HashStructure(scan), profile);
}

}  // namespace measured_fold

// A regression shape, end to end with the DEFAULT
// configuration and no browser profile: a <head> holding more elements than
// the old 25-element budget, then a body element whose rule the fold needs.
// Counted from <html> the budget was spent inside <head> and `.flex` never
// reached the inlined block; counted from <body> it does.
TEST_F(WorkerTest, HeadHeavyPageStillGetsItsBodyRulesInlined) {
  WorkerConfig config;
  config.socket_path = socket_path_;
  config.cache_path = cache_path_;
  config.cache_size_bytes = static_cast<uint64_t>(10 * 1024 * 1024);

  config.unsafe_force_async_css =
      true;  // fixture page is not under test for the gate
  Worker worker(config, nullptr);
  ASSERT_TRUE(worker.Initialize());

  std::string html =
      "<html><head><style>.flex{display:flex}.footer-x{color:red}"
      "</style>";
  for (int i = 0; i < 40; ++i) html += "<meta name=\"m\" content=\"v\">";
  html += "</head><body><div class=\"flex\">a</div></body></html>";
  const std::string cache_url = "/head-heavy.html";
  CacheOriginal(worker.cache(), cache_url, html);

  std::thread worker_thread([&worker]() { worker.Run(); });
  WorkerStopper stopper(worker, worker_thread);
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  CacheNotification notification;
  notification.url = "http://example.com/head-heavy.html";
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
  ASSERT_FALSE(variant.empty()) << "HTML variant not written";

  const size_t block_start = variant.find("<style data-pagespeed-critical");
  ASSERT_NE(block_start, std::string::npos);
  const size_t block_end = variant.find("</style>", block_start);
  ASSERT_NE(block_end, std::string::npos);
  const std::string block =
      variant.substr(block_start, block_end - block_start);
  EXPECT_NE(block.find("display:flex"), std::string::npos)
      << "the first body element's rule must be in the inlined block";
  EXPECT_EQ(block.find("color:red"), std::string::npos)
      << "a rule no fold element uses stays out of the block";
}

// The negative half of the A/B. A profile with NO measured fold reproduces the
// old behaviour exactly: both body divs sit past the estimate and neither
// rule is inlined. Without this, the positive test below could pass for the
// wrong reason (e.g. an extractor that suddenly admits everything).
TEST_F(WorkerTest, UnmeasuredFoldLeavesLateElementsOutOfCriticalCss) {
  using namespace measured_fold;
  WorkerConfig config = BrowserProfileConfig();

  Worker worker(config, nullptr);
  ASSERT_TRUE(worker.Initialize());

  const std::string url = "http://example.com/unmeasured.html";
  const std::string html = PageHtml();
  CacheOriginal(worker.cache(), url, html);
  CacheOriginal(worker.cache(), kCssUrl, kCss, "example.com");
  StoreProfileWithMeasuredFold(worker, url, html,
                               /*above_fold_selectors=*/{});

  std::thread worker_thread([&worker]() { worker.Run(); });
  WorkerStopper stopper(worker, worker_thread);
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  CacheNotification notification;
  notification.url = url;
  notification.scheme = "https";
  notification.content_type = ContentType::kHtml;
  notification.capability_mask = 0;
  SendNotification(notification);

  CapabilityMask mask = CapabilityMask::Decode(0);
  std::string variant;
  for (int i = 0; i < 60 * kSanitizerBudgetScale; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    variant = ReadVariant(worker.cache(), url, mask);
    if (!variant.empty()) break;
  }
  ASSERT_FALSE(variant.empty()) << "HTML variant not written";

  EXPECT_EQ(variant.find("display:flex"), std::string::npos)
      << "with nothing measured, an element past the estimate stays out";
  EXPECT_EQ(variant.find("24rem"), std::string::npos);
}

// The whole change, end to end: the profile says a browser measured `.flex`
// above the fold, and the block the visitor receives contains its rule. This is
// the only test that pins the serve-path plumbing — a profile field that is
// read but never passed on, or passed to a derivation that ignores it, leaves
// every unit test in this change green while production behaves exactly as
// before.
TEST_F(WorkerTest, MeasuredFoldAdmitsLateElementsIntoCriticalCss) {
  using namespace measured_fold;
  WorkerConfig config = BrowserProfileConfig();

  config.unsafe_force_async_css =
      true;  // fixture page is not under test for the gate
  Worker worker(config, nullptr);
  ASSERT_TRUE(worker.Initialize());

  const std::string url = "http://example.com/measured.html";
  const std::string html = PageHtml();
  CacheOriginal(worker.cache(), url, html);
  CacheOriginal(worker.cache(), kCssUrl, kCss, "example.com");
  StoreProfileWithMeasuredFold(worker, url, html,
                               /*above_fold_selectors=*/{".flex"});

  std::thread worker_thread([&worker]() { worker.Run(); });
  WorkerStopper stopper(worker, worker_thread);
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  CacheNotification notification;
  notification.url = url;
  notification.scheme = "https";
  notification.content_type = ContentType::kHtml;
  notification.capability_mask = 0;
  SendNotification(notification);

  CapabilityMask mask = CapabilityMask::Decode(0);
  std::string variant;
  for (int i = 0; i < 60 * kSanitizerBudgetScale; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    variant = ReadVariant(worker.cache(), url, mask);
    if (!variant.empty()) break;
  }
  ASSERT_FALSE(variant.empty()) << "HTML variant not written";

  EXPECT_NE(variant.find("data-pagespeed-critical"), std::string::npos)
      << "the critical block must still be inlined";
  EXPECT_NE(variant.find("display:flex"), std::string::npos)
      << "the measured above-the-fold element's rule must be inlined";
  // Selective, not a switch that admits the document: the sibling div is the
  // same distance past the estimate and was NOT measured above the fold.
  EXPECT_EQ(variant.find("24rem"), std::string::npos)
      << "an unmeasured element must stay out of the critical block";
}

// End to end: on a layered page whose layer order is proven, the
// served block goes first behind the page's `@layer` statement, and its
// @media blocks are kept for the device class's windows, so the mobile block
// drops Tailwind's `lg:` block while the desktop block keeps it. When a
// parser-blocking script comes before the sheet, the order is not proven: the
// block goes before </head>, after the sheet, and keeps every width. Both the
// heuristic path (no browser profile) and the profile path are served.
TEST_F(WorkerTest, ProvenLayerOrderNarrowsTheServedMobileBlock) {
  static constexpr const char* kCssUrl = "http://example.com/tw.css";
  const std::string css =
      "@layer theme,base,utilities;\n"
      "@layer base{nav{margin:0}}\n"
      "@layer utilities{.hidden{display:none}"
      "@media (width>=64rem){.lg\\:block{display:block}}}\n";
  auto page = [&](bool with_script) {
    return absl::StrCat("<html><head>",
                        with_script ? "<script src=\"/s.js\"></script>" : "",
                        "<link rel=\"stylesheet\" href=\"", kCssUrl,
                        "\"></head><body><nav class=\"hidden lg:block\">x</nav>"
                        "<img src=\"/a.png\"></body></html>");
  };

  struct Served {
    std::string variant;
    std::string block;  // the inlined <style> body
    size_t block_at = std::string::npos;
    size_t link_at = std::string::npos;
  };
  auto serve = [&](bool profile_path, bool with_script,
                   CapabilityMask::Viewport viewport) {
    Served out;
    WorkerConfig config;
    if (profile_path) {
      config = BrowserProfileConfig();
    } else {
      config.socket_path = socket_path_;
      config.cache_path = cache_path_;
      config.cache_size_bytes = static_cast<uint64_t>(10 * 1024 * 1024);
    }
    config.unsafe_force_async_css =
        true;  // fixture page is not under test for the gate
    Worker worker(config, nullptr);
    EXPECT_TRUE(worker.Initialize());
    // A URL per serve: the workers share one cache directory.
    static int serial = 0;
    const std::string url =
        absl::StrCat("http://example.com/tw", ++serial, ".html");
    const std::string html = page(with_script);
    CacheOriginal(worker.cache(), url, html);
    CacheOriginal(worker.cache(), kCssUrl, css, "example.com");
    if (profile_path) {
      HtmlScanner scanner;
      HtmlScanResult scan = scanner.Scan(url, html);
      EXPECT_TRUE(scan.success);
      ViewportProfile vp;
      vp.critical_css = ".hidden{display:none}";
      vp.css_coverage_ratio = 0.2f;
      vp.total_css_bytes = 5000;
      vp.unused_css_bytes = 4000;
      OptimizationProfile profile;
      profile.template_hash_hex = "layered";
      profile.analyzed_url = url;
      profile.mobile = vp;
      profile.tablet = vp;
      profile.desktop = vp;
      profile.created_at = 0;
      profile.expires_at = 0;
      EXPECT_NE(worker.TestBrowserManager(), nullptr);
      worker.TestBrowserManager()->TestStoreProfile(
          TemplateDetector::HashStructure(scan), profile);
    }
    std::thread worker_thread([&worker]() { worker.Run(); });
    WorkerStopper stopper(worker, worker_thread);
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    CapabilityMask mask;
    mask.set_viewport(viewport);
    CacheNotification notification;
    notification.url = url;
    notification.scheme = "https";
    notification.content_type = ContentType::kHtml;
    notification.capability_mask = mask.Encode();
    SendNotification(notification);
    for (int i = 0; i < 60 * kSanitizerBudgetScale; ++i) {
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
      out.variant = ReadVariant(worker.cache(), url, mask);
      if (!out.variant.empty()) break;
    }
    out.block_at = out.variant.find("<style data-pagespeed-critical");
    if (out.block_at != std::string::npos) {
      const size_t body = out.variant.find('>', out.block_at) + 1;
      out.block =
          out.variant.substr(body, out.variant.find("</style>", body) - body);
    }
    out.link_at = out.variant.find(kCssUrl);
    return out;
  };

  for (bool profile_path : {false, true}) {
    SCOPED_TRACE(profile_path ? "profile path" : "heuristic path");
    // Proven: first, with the statement; mobile drops lg:, desktop keeps it.
    Served mobile = serve(profile_path, /*with_script=*/false,
                          CapabilityMask::Viewport::kMobile);
    ASSERT_FALSE(mobile.variant.empty()) << "variant not written";
    ASSERT_NE(mobile.block_at, std::string::npos) << mobile.variant;
    EXPECT_LT(mobile.block_at, mobile.link_at) << "the block goes first";
    EXPECT_EQ(mobile.block.rfind("@layer theme,base,utilities;", 0), 0u)
        << mobile.block;
    EXPECT_NE(mobile.block.find("display:none"), std::string::npos);
    EXPECT_EQ(mobile.block.find("64rem"), std::string::npos)
        << "a proven page's mobile block drops the lg: block: " << mobile.block;

    Served desktop = serve(profile_path, /*with_script=*/false,
                           CapabilityMask::Viewport::kDesktop);
    ASSERT_NE(desktop.block_at, std::string::npos) << desktop.variant;
    EXPECT_NE(desktop.block.find("64rem"), std::string::npos) << desktop.block;

    // Not proven: after the sheet, every width kept on mobile too.
    Served unproven = serve(profile_path, /*with_script=*/true,
                            CapabilityMask::Viewport::kMobile);
    ASSERT_NE(unproven.block_at, std::string::npos) << unproven.variant;
    EXPECT_GT(unproven.block_at, unproven.link_at)
        << "the fallback block goes after the sheet";
    EXPECT_NE(unproven.block.find("64rem"), std::string::npos)
        << "a page whose order is not proven keeps the lg: block: "
        << unproven.block;
  }
}

// While the full stylesheet stays render-blocking the critical block is not
// inlined: it would duplicate bytes next to the sheet with no paint benefit.
// With a validated record the block and the deferral arrive together, and a
// page that moves from the first state to the second gets both on its next
// variant write, stable under repeated notifications.
TEST_F(WorkerTest, CriticalCssFollowsTheDeferralDecision) {
  using namespace async_css_gate;
  WorkerConfig config = BrowserProfileConfig();

  Worker worker(config, nullptr);
  ASSERT_TRUE(worker.Initialize());

  const std::string url = "http://example.com/moves.html";
  const std::string html = PageHtml();
  CacheOriginal(worker.cache(), url, html);
  CacheOriginal(worker.cache(), kCssUrl, kCss, "example.com");
  StoreBrowserProfile(worker, url, html, kProfileCriticalCss,
                      /*validated=*/false, /*validated_against_css=*/"");

  std::thread worker_thread([&worker]() { worker.Run(); });
  WorkerStopper stopper(worker, worker_thread);
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  CacheNotification notification;
  notification.url = url;
  notification.scheme = "https";
  notification.content_type = ContentType::kHtml;
  notification.capability_mask = 0;
  SendNotification(notification);

  CapabilityMask mask = CapabilityMask::Decode(0);
  // Unvalidated: nothing is inlined and the page keeps its blocking <link>.
  for (int i = 0; i < 60 * kSanitizerBudgetScale; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    if (worker.stats().async_css_suppressed_unvalidated.load() >= 1u) break;
  }
  ASSERT_GE(worker.stats().async_css_suppressed_unvalidated.load(), 1u);
  std::string before = ReadVariant(worker.cache(), url, mask);
  EXPECT_EQ(before.find("data-pagespeed-critical"), std::string::npos);
  EXPECT_EQ(before.find("data-pagespeed-async"), std::string::npos);

  // The record arrives; the next variant write carries block and deferral.
  // A purge (as on a redeploy or a reprocess request) makes the page eligible
  // for that write.
  WaitForWorkerIdle(worker);
  worker.InvalidateUrl(url, "");
  CacheOriginal(worker.cache(), url, html);
  StoreBrowserProfile(worker, url, html, kProfileCriticalCss,
                      /*validated=*/true, /*validated_against_css=*/kCss);
  std::string after;
  for (int i = 0; i < 80 * kSanitizerBudgetScale; ++i) {
    SendNotification(notification);
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    after = ReadVariant(worker.cache(), url, mask);
    if (after.find("as=\"style\"") != std::string::npos) break;
  }
  EXPECT_NE(after.find("data-pagespeed-async"), std::string::npos);
  EXPECT_NE(after.find("data-pagespeed-critical"), std::string::npos);

  // No half state and no rewrite loop on repeat notifications.
  for (int i = 0; i < 3; ++i) {
    SendNotification(notification);
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    std::string again = ReadVariant(worker.cache(), url, mask);
    size_t blocks = 0;
    for (size_t pos = 0; (pos = again.find("<style data-pagespeed-critical",
                                           pos)) != std::string::npos;
         ++pos) {
      ++blocks;
    }
    EXPECT_EQ(blocks, 1u);
    EXPECT_NE(again.find("data-pagespeed-async"), std::string::npos);
  }
}

// A cold page whose only change would be the critical block must still be
// written flagged for revalidation: dropping the block there must not drop
// the self-heal for a sheet that is not cached yet.
TEST_F(WorkerTest, ColdPageWithNoOtherChangeStillRevalidates) {
  WorkerConfig config;
  config.socket_path = socket_path_;
  config.cache_path = cache_path_;
  config.cache_size_bytes = static_cast<uint64_t>(10 * 1024 * 1024);
  Worker worker(config, nullptr);
  ASSERT_TRUE(worker.Initialize());

  const std::string url = "http://example.com/cold-only.html";
  const std::string html =
      "<html><head><style>.hero{color:red}</style>"
      "<link rel=\"stylesheet\" href=\"http://example.com/cold-only.css\">"
      "</head><body><div class=\"hero\">Hello</div></body></html>";
  CacheOriginal(worker.cache(), url, html);

  std::thread worker_thread([&worker]() { worker.Run(); });
  WorkerStopper stopper(worker, worker_thread);
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  CacheNotification notification;
  notification.url = url;
  notification.scheme = "https";
  notification.content_type = ContentType::kHtml;
  notification.capability_mask = 0;
  SendNotification(notification);

  CapabilityMask mask = CapabilityMask::Decode(0);
  std::optional<VariantWithMeta> v;
  for (int i = 0; i < 40 * kSanitizerBudgetScale; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    v = ReadVariantWithMeta(worker.cache(), url, mask);
    if (v.has_value() &&
        (v->metadata.flags & AlternateMetadata::kFlagWorkerProcessed) != 0) {
      break;
    }
  }
  ASSERT_TRUE(v.has_value());
  EXPECT_NE(v->metadata.flags & AlternateMetadata::kFlagNeedsRevalidation, 0)
      << "the cold page must be revisited once its sheet caches";
}

// The default image has no browser analysis, so a page is served by the
// heuristic path. For a small stylesheet nearly all of it is derived (the
// coverage gate does not apply below the small-sheet size), which used to
// duplicate the whole sheet inline next to the render-blocking link.
TEST_F(WorkerTest, HeuristicPathDoesNotDuplicateASmallSheetInline) {
  WorkerConfig config;  // defaults: no browser analysis
  config.socket_path = socket_path_;
  config.cache_path = cache_path_;
  config.cache_size_bytes = static_cast<uint64_t>(10 * 1024 * 1024);
  Worker worker(config, nullptr);
  ASSERT_TRUE(worker.Initialize());

  const std::string url = "http://example.com/demos/small/index.html";
  const std::string css_url = "http://example.com/demos/small/style.css";
  std::string css;
  for (int i = 0; i < 30; ++i) {
    css += ".c" + std::to_string(i) + "{color:#" + std::to_string(100 + i) +
           ";margin:0 auto;padding:4px 8px}\n";
  }
  std::string body;
  for (int i = 0; i < 30; ++i) {
    body += "<div class=\"c" + std::to_string(i) + "\">x</div>";
  }
  const std::string html =
      "<html><head><link rel=\"stylesheet\" href=\"style.css\"></head><body>" +
      body + "<img src=\"a.jpg\" width=\"10\" height=\"10\"></body></html>";
  CacheOriginal(worker.cache(), url, html);
  CacheOriginal(worker.cache(), css_url, css, "example.com");

  std::thread worker_thread([&worker]() { worker.Run(); });
  WorkerStopper stopper(worker, worker_thread);
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  CacheNotification notification;
  notification.url = url;
  notification.scheme = "https";
  notification.content_type = ContentType::kHtml;
  notification.capability_mask = 0;
  SendNotification(notification);

  CapabilityMask mask = CapabilityMask::Decode(0);
  std::optional<VariantWithMeta> v;
  for (int i = 0; i < 60 * kSanitizerBudgetScale; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    v = ReadVariantWithMeta(worker.cache(), url, mask);
    if (v.has_value() &&
        (v->metadata.flags & AlternateMetadata::kFlagWorkerProcessed) != 0) {
      break;
    }
  }
  ASSERT_TRUE(v.has_value());
  EXPECT_EQ(v->content.find("data-pagespeed-critical"), std::string::npos);
  EXPECT_NE(v->content.find("rel=\"stylesheet\""), std::string::npos);
  EXPECT_LT(v->content.size(), html.size() + 400u)
      << "the page must not carry a copy of the sheet";
}

namespace block_drop {

constexpr const char* kSheetBase = "http://example.com/bd/";

std::string Rules() {
  std::string css;
  for (int i = 0; i < 30; ++i) {
    css += ".c" + std::to_string(i) + "{color:#" + std::to_string(100 + i) +
           ";margin:0 auto;padding:4px 8px}\n";
  }
  return css;
}

std::string Divs() {
  std::string body;
  for (int i = 0; i < 30; ++i) {
    body += "<div class=\"c" + std::to_string(i) + "\">x</div>";
  }
  return body;
}

}  // namespace block_drop

// A sheet that first paint does not wait for (applied by script, or standing
// after body content) leaves the inlined block as the fold's only styling, so
// the block stays.
TEST_F(WorkerTest, SheetThatMayNotBlockPaintKeepsTheBlock) {
  using namespace block_drop;
  WorkerConfig config;
  config.socket_path = socket_path_;
  config.cache_path = cache_path_;
  config.cache_size_bytes = static_cast<uint64_t>(10 * 1024 * 1024);
  Worker worker(config, nullptr);
  ASSERT_TRUE(worker.Initialize());

  struct Case {
    const char* name;
    std::string head;
    std::string tail;
  };
  const std::string sheet = std::string(kSheetBase);
  std::vector<Case> cases = {
      {"preload",
       "<link rel=\"preload\" as=\"style\" href=\"" + sheet +
           "preload.css\" onload=\"this.rel='stylesheet'\"><noscript><link "
           "rel=\"stylesheet\" href=\"" +
           sheet + "preload.css\"></noscript>",
       ""},
      {"printswap",
       "<link rel=\"stylesheet\" href=\"" + sheet +
           "printswap.css\" media=\"print\" onload=\"this.media='all'\">",
       ""},
      {"inbody", "",
       "<link rel=\"stylesheet\" href=\"" + sheet + "inbody.css\">"},
  };
  for (const Case& c : cases) {
    CacheOriginal(worker.cache(), std::string(kSheetBase) + c.name + ".css",
                  Rules(), "example.com");
    CacheOriginal(worker.cache(),
                  std::string("http://example.com/bd/") + c.name + ".html",
                  "<html><head>" + c.head + "</head><body>" + Divs() + c.tail +
                      "<img src=\"a.jpg\" width=\"10\" height=\"10\">"
                      "</body></html>");
  }

  std::thread worker_thread([&worker]() { worker.Run(); });
  WorkerStopper stopper(worker, worker_thread);
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  for (const Case& c : cases) {
    const std::string url =
        std::string("http://example.com/bd/") + c.name + ".html";
    CacheNotification notification;
    notification.url = url;
    notification.scheme = "https";
    notification.content_type = ContentType::kHtml;
    notification.capability_mask = 0;
    SendNotification(notification);
    CapabilityMask mask = CapabilityMask::Decode(0);
    std::optional<VariantWithMeta> v;
    for (int i = 0; i < 60 * kSanitizerBudgetScale; ++i) {
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
      v = ReadVariantWithMeta(worker.cache(), url, mask);
      if (v.has_value() &&
          (v->metadata.flags & AlternateMetadata::kFlagWorkerProcessed) != 0) {
        break;
      }
    }
    ASSERT_TRUE(v.has_value()) << c.name;
    EXPECT_NE(v->content.find("data-pagespeed-critical"), std::string::npos)
        << c.name << ": the block is the fold's only styling";
  }
}

// The block-only page that was left unchanged is held for a short while, so a
// front end that re-notifies on every hit does not rerun the pipeline, and the
// hold is short enough for a later validated record to take effect.
TEST_F(WorkerTest, UnchangedPageWithDroppedBlockIsHeldBriefly) {
  using namespace async_css_gate;
  WorkerConfig config = BrowserProfileConfig();
  config.disable_lazy_load = true;
  config.disable_image_dimensions = true;
  config.disable_lcp_preload = true;
  config.disable_preconnect_injection = true;
  config.disable_script_deferral = true;
  config.enable_speculation_rules = false;
  WarningRecordingHandler handler;
  Worker worker(config, &handler);
  ASSERT_TRUE(worker.Initialize());

  const std::string url = "http://example.com/held.html";
  const std::string html = PageHtml();
  CacheOriginal(worker.cache(), url, html);
  CacheOriginal(worker.cache(), kCssUrl, kCss, "example.com");
  StoreBrowserProfile(worker, url, html, kProfileCriticalCss,
                      /*validated=*/false, /*validated_against_css=*/"");

  std::thread worker_thread([&worker]() { worker.Run(); });
  WorkerStopper stopper(worker, worker_thread);
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  CacheNotification notification;
  notification.url = url;
  notification.scheme = "https";
  notification.content_type = ContentType::kHtml;
  notification.capability_mask = 0;
  SendNotification(notification);
  for (int i = 0; i < 60 * kSanitizerBudgetScale; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    if (worker.stats().html_assembly_skipped.load() >= 1u) break;
  }
  ASSERT_EQ(worker.stats().html_assembly_skipped.load(), 1u)
      << ::testing::PrintToString(handler.infos());
  WaitForWorkerIdle(worker);

  const uint64_t dedup_before =
      worker.stats().notifications_skipped_dedup.load();
  for (int i = 0; i < 3; ++i) {
    SendNotification(notification);
    for (int j = 0; j < 30 * kSanitizerBudgetScale; ++j) {
      std::this_thread::sleep_for(std::chrono::milliseconds(50));
      if (worker.stats().notifications_skipped_dedup.load() >=
          dedup_before + i + 1) {
        break;
      }
    }
  }
  EXPECT_EQ(worker.stats().html_assembly_skipped.load(), 1u)
      << "the held page must not rerun the pipeline";
  EXPECT_GE(worker.stats().notifications_skipped_dedup.load(),
            dedup_before + 3);

  // A record arrives; once the hold has run out the next notification picks it
  // up, with no purge.
  StoreBrowserProfile(worker, url, html, kProfileCriticalCss,
                      /*validated=*/true, /*validated_against_css=*/kCss);
  // The hold is 5 seconds.
  std::this_thread::sleep_for(std::chrono::seconds(6));
  CapabilityMask mask = CapabilityMask::Decode(0);
  std::string after;
  for (int i = 0; i < 40 * kSanitizerBudgetScale; ++i) {
    SendNotification(notification);
    std::this_thread::sleep_for(std::chrono::milliseconds(250));
    after = ReadVariant(worker.cache(), url, mask);
    if (after.find("as=\"style\"") != std::string::npos) break;
  }
  EXPECT_NE(after.find("data-pagespeed-async"), std::string::npos)
      << "a validated record must take effect once the hold has run out";
  EXPECT_NE(after.find("data-pagespeed-critical"), std::string::npos);
}

// With the other transforms off, a page whose previous output carries a block
// must lose it when the sheet is no longer deferred, rather than be left alone.
TEST_F(WorkerTest, StaleBlockIsRemovedWhenNothingElseTransforms) {
  using namespace async_css_gate;
  WorkerConfig config = BrowserProfileConfig();
  config.disable_lazy_load = true;
  config.disable_image_dimensions = true;
  config.disable_lcp_preload = true;
  config.disable_preconnect_injection = true;
  config.disable_script_deferral = true;
  config.enable_speculation_rules = false;
  WarningRecordingHandler handler;
  Worker worker(config, &handler);
  ASSERT_TRUE(worker.Initialize());

  const std::string url = "http://example.com/stale.html";
  const std::string html = PageHtml();
  CacheOriginal(worker.cache(), url, html);
  CacheOriginal(worker.cache(), kCssUrl, kCss, "example.com");
  StoreBrowserProfile(worker, url, html, kProfileCriticalCss,
                      /*validated=*/true, /*validated_against_css=*/kCss);

  std::thread worker_thread([&worker]() { worker.Run(); });
  WorkerStopper stopper(worker, worker_thread);
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  CacheNotification notification;
  notification.url = url;
  notification.scheme = "https";
  notification.content_type = ContentType::kHtml;
  notification.capability_mask = 0;
  SendNotification(notification);
  CapabilityMask mask = CapabilityMask::Decode(0);
  std::string first;
  for (int i = 0; i < 80 * kSanitizerBudgetScale; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    first = ReadVariant(worker.cache(), url, mask);
    if (first.find("data-pagespeed-critical") != std::string::npos) break;
  }
  ASSERT_NE(first.find("data-pagespeed-critical"), std::string::npos);
  WaitForWorkerIdle(worker);

  // The record is withdrawn; the front end re-notifies against the worker's
  // own output.
  StoreBrowserProfile(worker, url, html, kProfileCriticalCss,
                      /*validated=*/false, /*validated_against_css=*/"");
  worker.ClearDedupAndCooldown(url, "", "https");
  // The front end re-notifies against the worker's own output, which is what
  // the original entry holds once a variant has been written over it.
  std::array<std::byte, 32> inherited{};
  for (size_t i = 0; i < inherited.size(); ++i) {
    inherited[i] = static_cast<std::byte>(0x40 + i);
  }
  CacheOriginal(worker.cache(), url, first, /*hostname=*/"",
                AlternateMetadata::kFlagWorkerProcessed, &inherited);
  SendNotification(notification);
  std::string second = first;
  for (int i = 0; i < 80 * kSanitizerBudgetScale; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    second = ReadVariant(worker.cache(), url, mask);
    if (second.find("data-pagespeed-critical") == std::string::npos) break;
  }
  EXPECT_EQ(second.find("data-pagespeed-critical"), std::string::npos)
      << second << ::testing::PrintToString(handler.infos());
  EXPECT_EQ(second.find("data-pagespeed-async"), std::string::npos);
  EXPECT_NE(second.find("rel=\"stylesheet\""), std::string::npos);
}

// --no-async-css leaves the sheet render-blocking, so a warm page gets no
// block and no async markup.
TEST_F(WorkerTest, DisabledAsyncCssGivesNoBlockOnAWarmSheet) {
  using namespace block_drop;
  WorkerConfig config;
  config.socket_path = socket_path_;
  config.cache_path = cache_path_;
  config.cache_size_bytes = static_cast<uint64_t>(10 * 1024 * 1024);
  config.disable_async_css = true;
  Worker worker(config, nullptr);
  ASSERT_TRUE(worker.Initialize());

  const std::string url = "http://example.com/bd/noasync.html";
  CacheOriginal(worker.cache(), "http://example.com/bd/noasync.css", Rules(),
                "example.com");
  CacheOriginal(worker.cache(), url,
                "<html><head><link rel=\"stylesheet\" "
                "href=\"noasync.css\"></head><body>" +
                    Divs() +
                    "<img src=\"a.jpg\" width=\"10\" height=\"10\"></body>"
                    "</html>");
  std::thread worker_thread([&worker]() { worker.Run(); });
  WorkerStopper stopper(worker, worker_thread);
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  CacheNotification notification;
  notification.url = url;
  notification.scheme = "https";
  notification.content_type = ContentType::kHtml;
  notification.capability_mask = 0;
  SendNotification(notification);
  CapabilityMask mask = CapabilityMask::Decode(0);
  std::optional<VariantWithMeta> v;
  for (int i = 0; i < 60 * kSanitizerBudgetScale; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    v = ReadVariantWithMeta(worker.cache(), url, mask);
    if (v.has_value() &&
        (v->metadata.flags & AlternateMetadata::kFlagWorkerProcessed) != 0) {
      break;
    }
  }
  ASSERT_TRUE(v.has_value());
  EXPECT_EQ(v->content.find("data-pagespeed-critical"), std::string::npos);
  EXPECT_EQ(v->content.find("data-pagespeed-async"), std::string::npos);
  EXPECT_NE(v->content.find("rel=\"stylesheet\""), std::string::npos);
}

// The cold page, carried through the sheet caching: no block, flag clear,
// and the variant stays the same on repeat notifications.
TEST_F(WorkerTest, ColdPageConvergesWhenItsSheetCaches) {
  using namespace block_drop;
  WorkerConfig config;
  config.socket_path = socket_path_;
  config.cache_path = cache_path_;
  config.cache_size_bytes = static_cast<uint64_t>(10 * 1024 * 1024);
  Worker worker(config, nullptr);
  ASSERT_TRUE(worker.Initialize());

  const std::string url = "http://example.com/bd/converge.html";
  const std::string css_url = "http://example.com/bd/converge.css";
  CacheOriginal(worker.cache(), url,
                "<html><head><style>.hero{color:red}</style><link "
                "rel=\"stylesheet\" href=\"" +
                    css_url + "\"></head><body><div class=\"hero\">x</div>" +
                    "</body></html>");
  std::thread worker_thread([&worker]() { worker.Run(); });
  WorkerStopper stopper(worker, worker_thread);
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  CacheNotification notification;
  notification.url = url;
  notification.scheme = "https";
  notification.content_type = ContentType::kHtml;
  notification.capability_mask = 0;
  SendNotification(notification);
  CapabilityMask mask = CapabilityMask::Decode(0);
  std::optional<VariantWithMeta> cold;
  for (int i = 0; i < 40 * kSanitizerBudgetScale; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    cold = ReadVariantWithMeta(worker.cache(), url, mask);
    if (cold.has_value() &&
        (cold->metadata.flags & AlternateMetadata::kFlagWorkerProcessed) != 0) {
      break;
    }
  }
  ASSERT_TRUE(cold.has_value());
  EXPECT_NE(cold->metadata.flags & AlternateMetadata::kFlagNeedsRevalidation,
            0);

  CacheOriginal(worker.cache(), css_url, ".hero{color:red}", "example.com");
  std::optional<VariantWithMeta> warm;
  for (int i = 0; i < 80 * kSanitizerBudgetScale; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    if (i % 10 == 0) SendNotification(notification);
    warm = ReadVariantWithMeta(worker.cache(), url, mask);
    if (warm.has_value() && (warm->metadata.flags &
                             AlternateMetadata::kFlagNeedsRevalidation) == 0) {
      break;
    }
  }
  ASSERT_TRUE(warm.has_value());
  EXPECT_EQ(warm->metadata.flags & AlternateMetadata::kFlagNeedsRevalidation,
            0);
  EXPECT_EQ(warm->content.find("data-pagespeed-critical"), std::string::npos)
      << "the stale cold-pass block must be gone";
  EXPECT_NE(warm->content.find("rel=\"stylesheet\""), std::string::npos);

  for (int i = 0; i < 3; ++i) {
    SendNotification(notification);
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    auto again = ReadVariantWithMeta(worker.cache(), url, mask);
    ASSERT_TRUE(again.has_value());
    EXPECT_EQ(again->content, warm->content);
  }
}

// A print stylesheet's rules never style the screen. The combined sheet the
// critical block is derived from used to append a print <link>'s sheet and a
// print <style>'s body as unconditional rules, so a block inlined from it
// carried `header,footer{display:none}` and hid the header and footer on
// screen. Here the in-body screen sheet keeps the block (the fold's only
// styling until it loads); the print rules must not reach the block outside
// `@media print`.
TEST_F(WorkerTest, PrintSheetRulesNeverReachTheScreenBlock) {
  WorkerConfig config;
  config.socket_path = socket_path_;
  config.cache_path = cache_path_;
  config.cache_size_bytes = static_cast<uint64_t>(10 * 1024 * 1024);
  Worker worker(config, nullptr);
  ASSERT_TRUE(worker.Initialize());

  struct Case {
    const char* name;
    std::string head;
  };
  const std::vector<Case> cases = {
      {"printlink",
       "<link rel=\"stylesheet\" href=\"http://example.com/pr/print.css\" "
       "media=\"print\">"},
      {"printstyle",
       "<style media=\"print\">header,footer{display:none}</style>"},
  };
  CacheOriginal(worker.cache(), "http://example.com/pr/print.css",
                "header,footer{display:none}", "example.com");
  CacheOriginal(worker.cache(), "http://example.com/pr/screen.css",
                "header{display:block;background:#eee}"
                "footer{display:block;color:#333}p{margin:0}",
                "example.com");
  for (const Case& c : cases) {
    CacheOriginal(worker.cache(),
                  std::string("http://example.com/pr/") + c.name + ".html",
                  "<html><head>" + c.head +
                      "</head><body><header>Top</header><p>Hello</p>"
                      "<footer>Bottom</footer>"
                      "<link rel=\"stylesheet\" "
                      "href=\"http://example.com/pr/screen.css\">"
                      "</body></html>");
  }

  std::thread worker_thread([&worker]() { worker.Run(); });
  WorkerStopper stopper(worker, worker_thread);
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  for (const Case& c : cases) {
    const std::string url =
        std::string("http://example.com/pr/") + c.name + ".html";
    CacheNotification notification;
    notification.url = url;
    notification.scheme = "https";
    notification.content_type = ContentType::kHtml;
    notification.capability_mask = 0;
    SendNotification(notification);
    CapabilityMask mask = CapabilityMask::Decode(0);
    std::optional<VariantWithMeta> v;
    for (int i = 0; i < 60 * kSanitizerBudgetScale; ++i) {
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
      v = ReadVariantWithMeta(worker.cache(), url, mask);
      if (v.has_value() &&
          (v->metadata.flags & AlternateMetadata::kFlagWorkerProcessed) != 0) {
        break;
      }
    }
    ASSERT_TRUE(v.has_value()) << c.name;
    const size_t tag = v->content.find("data-pagespeed-critical");
    ASSERT_NE(tag, std::string::npos) << c.name << ": " << v->content;
    const size_t open_end = v->content.find('>', tag);
    const size_t close = v->content.find("</style>", open_end);
    ASSERT_NE(close, std::string::npos) << c.name;
    std::string block = v->content.substr(open_end + 1, close - open_end - 1);
    // Cut every `@media print{...}` block: what remains applies on screen.
    std::string screen;
    for (size_t i = 0; i < block.size();) {
      if (block.compare(i, 12, "@media print") == 0) {
        int depth = 0;
        size_t j = block.find('{', i);
        for (; j < block.size(); ++j) {
          if (block[j] == '{') ++depth;
          if (block[j] == '}' && --depth == 0) break;
        }
        i = j + 1;
        continue;
      }
      screen += block[i++];
    }
    EXPECT_NE(screen.find("display:block"), std::string::npos)
        << c.name << ": " << block;
    EXPECT_EQ(screen.find("display:none"), std::string::npos)
        << c.name << ": print rules on screen: " << block;
  }
}

// The analysis side assembles the combined sheet with the same builder, so
// the bytes it renders and validates scope a print sheet the same way, and
// a sheet under the print-media trick (its onload handler applies it to
// every medium) stays unscoped as before.
TEST_F(WorkerTest, CombinedSheetScopesEachSheetToItsMedia) {
  WorkerConfig config = BrowserProfileConfig();
  config.ram_cache_size = 0;
  Worker worker(config, nullptr);
  ASSERT_TRUE(worker.Initialize());

  const std::string hostname = "example.com";
  const std::string html =
      "<html><head>"
      "<link rel=\"stylesheet\" href=\"/s.css\">"
      "<link rel=\"stylesheet\" href=\"/p.css\" media=\"print\">"
      "<link rel=\"stylesheet\" href=\"/n.css\" media=\"(max-width: 600px)\">"
      "<link rel=\"stylesheet\" href=\"/a.css\" media=\"all\">"
      "<link rel=\"stylesheet\" href=\"/w.css\" media=\"print\" "
      "onload=\"this.media='all'\">"
      "<style media=\"print\">footer{display:none}</style>"
      "</head><body><p>x</p></body></html>";
  CacheOriginal(worker.cache(), "https://example.com/s.css", "p{margin:0}",
                hostname);
  CacheOriginal(worker.cache(), "https://example.com/p.css",
                "header{display:none}", hostname);
  CacheOriginal(worker.cache(), "https://example.com/n.css", "p{margin:1px}",
                hostname);
  CacheOriginal(worker.cache(), "https://example.com/a.css", "p{color:red}",
                hostname);
  CacheOriginal(worker.cache(), "https://example.com/w.css", "p{color:blue}",
                hostname);

  HtmlScanner scanner;
  HtmlScanResult scan = scanner.Scan("/page.html", html);
  ASSERT_TRUE(scan.success);
  EXPECT_EQ(scan.inline_css, "@media print{footer{display:none}}");
  ASSERT_NE(worker.TestBrowserManager(), nullptr);
  const auto& builder = worker.TestBrowserManager()->TestCombinedCssBuilder();
  ASSERT_TRUE(builder);
  BrowserAnalysisManager::CombinedCss combined =
      builder(scan, "/page.html", hostname, "https", /*site=*/"",
              /*host_vouched=*/false, /*from_module=*/false);
  EXPECT_FALSE(combined.external_css_missing);
  EXPECT_EQ(combined.css,
            "@media print{footer{display:none}}\n"
            "p{margin:0}\n"
            "@media print{header{display:none}}\n"
            "@media (max-width: 600px){p{margin:1px}}\n"
            "p{color:red}\n"
            "p{color:blue}");
}

// A <link> whose media value cannot be written as an @media prelude safely
// contributes nothing to the combined sheet and counts as missing, so the
// page's stylesheets stay render-blocking.
TEST_F(WorkerTest, UnusableMediaSheetIsMissingAndLeftOut) {
  WorkerConfig config = BrowserProfileConfig();
  config.ram_cache_size = 0;
  Worker worker(config, nullptr);
  ASSERT_TRUE(worker.Initialize());

  const std::string hostname = "example.com";
  CacheOriginal(worker.cache(), "https://example.com/s.css", "p{margin:0}",
                hostname);
  CacheOriginal(worker.cache(), "https://example.com/h.css",
                ".hostile{display:none}", hostname);
  ASSERT_NE(worker.TestBrowserManager(), nullptr);
  const auto& builder = worker.TestBrowserManager()->TestCombinedCssBuilder();
  ASSERT_TRUE(builder);

  const char* const kMedia[] = {
      "print}body{display:none}",
      "screen;x",
      "print{",
      "print\\7d b",
      "print/*",
      "(min-width:1px",
      "screen)",
      "print&quot;x",
      "@import",
  };
  for (const char* media : kMedia) {
    const std::string html =
        "<html><head><link rel=\"stylesheet\" href=\"/s.css\">"
        "<link rel=\"stylesheet\" href=\"/h.css\" media=\"" +
        std::string(media) + "\"></head><body><p>x</p></body></html>";
    HtmlScanner scanner;
    HtmlScanResult scan = scanner.Scan("/page.html", html);
    ASSERT_TRUE(scan.success) << media;
    BrowserAnalysisManager::CombinedCss combined =
        builder(scan, "/page.html", hostname, "https", /*site=*/"",
                /*host_vouched=*/false, /*from_module=*/false);
    EXPECT_TRUE(combined.external_css_missing) << media;
    EXPECT_EQ(combined.css, "p{margin:0}") << media;
  }
}

// A print sheet or <style> whose braces do not balance cannot be wrapped in
// its @media block without a stray `}` closing the block early, so it adds
// nothing, and a sheet counts as missing. Balanced nesting is wrapped.
TEST_F(WorkerTest, UnbalancedPrintSheetIsMissingAndLeftOut) {
  WorkerConfig config = BrowserProfileConfig();
  config.ram_cache_size = 0;
  Worker worker(config, nullptr);
  ASSERT_TRUE(worker.Initialize());

  const std::string hostname = "example.com";
  CacheOriginal(worker.cache(), "https://example.com/s.css", "p{margin:0}",
                hostname);
  CacheOriginal(worker.cache(), "https://example.com/stray.css",
                "a{b:c}}header{display:none}", hostname);
  CacheOriginal(worker.cache(), "https://example.com/open.css",
                "header{display:none", hostname);
  CacheOriginal(worker.cache(), "https://example.com/nested.css",
                "@supports (a:b){header{display:none}}"
                "i::before{content:\"}\"}",
                hostname);
  ASSERT_NE(worker.TestBrowserManager(), nullptr);
  const auto& builder = worker.TestBrowserManager()->TestCombinedCssBuilder();
  ASSERT_TRUE(builder);

  const auto build = [&](const std::string& head) {
    const std::string html =
        "<html><head>" + head + "</head><body><p>x</p></body></html>";
    HtmlScanner scanner;
    HtmlScanResult scan = scanner.Scan("/page.html", html);
    EXPECT_TRUE(scan.success);
    return std::make_pair(
        scan.inline_css,
        builder(scan, "/page.html", hostname, "https", /*site=*/"",
                /*host_vouched=*/false, /*from_module=*/false));
  };

  for (const char* sheet : {"/stray.css", "/open.css"}) {
    const auto [inline_css, combined] = build(
        "<link rel=\"stylesheet\" href=\"/s.css\">"
        "<link rel=\"stylesheet\" href=\"" +
        std::string(sheet) + "\" media=\"print\">");
    EXPECT_TRUE(combined.external_css_missing) << sheet;
    EXPECT_EQ(combined.css, "p{margin:0}") << sheet;
  }
  {
    const auto [inline_css, combined] = build(
        "<link rel=\"stylesheet\" href=\"/s.css\">"
        "<link rel=\"stylesheet\" href=\"/nested.css\" media=\"print\">");
    EXPECT_FALSE(combined.external_css_missing);
    EXPECT_EQ(combined.css,
              "p{margin:0}\n"
              "@media print{@supports (a:b){header{display:none}}"
              "i::before{content:\"}\"}}");
  }
  {
    const auto [inline_css, combined] = build(
        "<style media=\"print\">a{b:c}}header{display:none}</style>"
        "<link rel=\"stylesheet\" href=\"/s.css\">");
    EXPECT_EQ(inline_css, "");
    EXPECT_EQ(combined.css, "p{margin:0}");
  }
  {
    const auto [inline_css, combined] =
        build("<style media=\"print\">a{b:c}header{display:none}</style>");
    EXPECT_EQ(inline_css, "@media print{a{b:c}header{display:none}}");
  }
}

// The print-media exception applies only when the onload handler text changes
// the media. Any other handler leaves the sheet a print sheet.
TEST_F(WorkerTest, OnlyAMediaChangingOnloadMakesAPrintSheetAScreenSheet) {
  WorkerConfig config = BrowserProfileConfig();
  config.ram_cache_size = 0;
  Worker worker(config, nullptr);
  ASSERT_TRUE(worker.Initialize());

  const std::string hostname = "example.com";
  CacheOriginal(worker.cache(), "https://example.com/w.css", "p{color:blue}",
                hostname);
  ASSERT_NE(worker.TestBrowserManager(), nullptr);
  const auto& builder = worker.TestBrowserManager()->TestCombinedCssBuilder();
  ASSERT_TRUE(builder);

  const auto combined_for = [&](const std::string& handler) {
    const std::string html =
        "<html><head><link rel=\"stylesheet\" href=\"/w.css\" "
        "media=\"print\" onload=\"" +
        handler + "\"></head><body><p>x</p></body></html>";
    HtmlScanner scanner;
    HtmlScanResult scan = scanner.Scan("/page.html", html);
    EXPECT_TRUE(scan.success);
    return builder(scan, "/page.html", hostname, "https", /*site=*/"",
                   /*host_vouched=*/false, /*from_module=*/false)
        .css;
  };

  for (const char* handler :
       {"this.media='all'", "this.onload=null;this.media = 'all'",
        "this['media']='all'", "this.setAttribute('media', 'all')",
        "this.removeAttribute('media')"}) {
    EXPECT_EQ(combined_for(handler), "p{color:blue}") << handler;
  }
  for (const char* handler :
       {"track()", "this.rel='stylesheet'", "console.log(this.media)",
        "this.media=='print'", "this.onload=null", "this.xmedia='all'", ""}) {
    EXPECT_EQ(combined_for(handler), "@media print{p{color:blue}}") << handler;
  }
}

}  // namespace
}  // namespace pagespeed
