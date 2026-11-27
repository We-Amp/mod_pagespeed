// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

// Test for the pagespeed nginx module C++ APIs
//
// Tests the C++ APIs used directly by the nginx module:
// CapabilityMask, PageSpeedCache, and MimeFromUrl.

#include <array>
#include <chrono>
#include <cstddef>
#include <cstring>
#include <filesystem>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

// POSIX fd APIs (open/dup/fstat) for the request-owned sendfile-fd lifetime
// test below. The nginx module is POSIX-only, so these are guarded for the
// Windows build of this otherwise cross-platform test.
#ifndef _WIN32
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

#include "gtest/gtest.h"
#include "lib/cache/cache.h"
#include "lib/classify/alternate_id.h"
#include "lib/classify/alternate_metadata.h"
#include "lib/classify/capability_mask.h"
#include "src/nginx/mime_util.h"
#include "src/nginx/zerocopy_barrier.h"
#include "test/test_util/temp_dir.h"

using pagespeed::AlternateId;
using pagespeed::AlternateMetadata;
using pagespeed::CapabilityMask;
using pagespeed::ContentType;
using pagespeed::MaskToAlternateId;
using pagespeed::PageSpeedCache;
using pagespeed::PageSpeedCacheConfig;
using pagespeed::SentinelId;

namespace {

// =============================================================================
// CapabilityMask Tests
// =============================================================================

TEST(PagespeedTest, CapabilityMaskFromHeaders) {
  // Test with WebP support and desktop viewport
  CapabilityMask mask = CapabilityMask::FromHeaders(
      "image/webp,image/png,*/*",                   // Accept
      "Mozilla/5.0 (Windows NT 10.0; Win64; x64)",  // User-Agent
      "",                                           // Save-Data
      "");                                          // Accept-Encoding

  uint32_t encoded = mask.Encode();
  // Should have WebP format bit set
  EXPECT_NE(encoded & 0x03, 0u);  // Image format bits
}

TEST(PagespeedTest, CapabilityMaskEmptyHeaders) {
  // Test with empty headers - should not crash
  CapabilityMask mask = CapabilityMask::FromHeaders("", "", "", "");

  // Should produce a valid (default) encoding
  uint32_t encoded = mask.Encode();
  EXPECT_EQ(encoded, mask.Encode());  // Consistent
}

// =============================================================================
// MimeFromUrl Tests
// =============================================================================

using pagespeed::MimeFromUrl;

TEST(ContentTypeFromUrlTest, HtmlExtensions) {
  EXPECT_STREQ(MimeFromUrl("/page.html"), "text/html");
  EXPECT_STREQ(MimeFromUrl("/page.htm"), "text/html");
  EXPECT_STREQ(MimeFromUrl("/dir/index.html"), "text/html");
}

TEST(ContentTypeFromUrlTest, CssAndJs) {
  EXPECT_STREQ(MimeFromUrl("/style.css"), "text/css");
  EXPECT_STREQ(MimeFromUrl("/app.js"), "application/javascript");
}

TEST(ContentTypeFromUrlTest, ImageFormats) {
  EXPECT_STREQ(MimeFromUrl("/photo.jpg"), "image/jpeg");
  EXPECT_STREQ(MimeFromUrl("/photo.jpeg"), "image/jpeg");
  EXPECT_STREQ(MimeFromUrl("/icon.png"), "image/png");
  EXPECT_STREQ(MimeFromUrl("/hero.webp"), "image/webp");
  EXPECT_STREQ(MimeFromUrl("/hero.avif"), "image/avif");
  EXPECT_STREQ(MimeFromUrl("/anim.gif"), "image/gif");
  EXPECT_STREQ(MimeFromUrl("/logo.svg"), "image/svg+xml");
}

TEST(ContentTypeFromUrlTest, OtherFormats) {
  EXPECT_STREQ(MimeFromUrl("/data.json"), "application/json");
  EXPECT_STREQ(MimeFromUrl("/feed.xml"), "application/xml");
  EXPECT_STREQ(MimeFromUrl("/font.woff2"), "font/woff2");
  EXPECT_STREQ(MimeFromUrl("/font.woff"), "font/woff");
}

TEST(ContentTypeFromUrlTest, QueryStringIgnored) {
  EXPECT_STREQ(MimeFromUrl("/page.html?v=123"), "text/html");
  EXPECT_STREQ(MimeFromUrl("/style.css?hash=abc"), "text/css");
}

TEST(ContentTypeFromUrlTest, NoExtension) {
  EXPECT_EQ(MimeFromUrl("/page"), nullptr);
  EXPECT_EQ(MimeFromUrl(""), nullptr);
}

TEST(ContentTypeFromUrlTest, TrailingSlashIsHtml) {
  EXPECT_STREQ(MimeFromUrl("/"), "text/html");
  EXPECT_STREQ(MimeFromUrl("/features/"), "text/html");
  EXPECT_STREQ(MimeFromUrl("/pricing/plans/"), "text/html");
}

TEST(ContentTypeFromUrlTest, UnknownExtension) {
  EXPECT_EQ(MimeFromUrl("/file.xyz"), nullptr);
  EXPECT_EQ(MimeFromUrl("/data.bin"), nullptr);
}

// =============================================================================
// PageSpeedCache API Tests (nginx module patterns)
// =============================================================================

class CacheTest : public ::testing::Test {
 protected:
  void SetUp() override {
    cache_dir_ = pagespeed::test::MakeTempDir();
    cache_path_ = cache_dir_ + "/cache.vol";
  }

  void TearDown() override {
    cache_.reset();
    if (!cache_dir_.empty()) {
      std::error_code ec;
      std::filesystem::remove_all(cache_dir_, ec);
    }
  }

  void CreateCache() {
    PageSpeedCacheConfig config;
    config.volume_path = cache_path_;
    config.volume_size = static_cast<uint64_t>(10 * 1024 * 1024);

    auto result = PageSpeedCache::Create(config);
    ASSERT_TRUE(result.has_value()) << "Failed to create cache";
    cache_ = std::move(*result);
  }

  // Helper: write content using the nginx record-on-miss pattern.
  void WriteOriginal(std::string_view url, std::string_view data,
                     ContentType ct = ContentType::kImage) {
    CapabilityMask default_mask;
    AlternateId id =
        MaskToAlternateId(static_cast<uint8_t>(default_mask.Encode() & 0xFF));
    AlternateMetadata meta;
    meta.full_mask = default_mask.Encode();
    meta.content_type = ct;

    auto wh = cache_->WriteAlternate(url, "", "", id, data.size(), meta);
    ASSERT_TRUE(wh.has_value()) << "WriteAlternate failed";
    auto bytes = std::as_bytes(std::span(data));
    ASSERT_TRUE(wh->write_sync(bytes).has_value());
    ASSERT_TRUE(wh->close_sync().has_value());
  }

  // Helper: write a content alternate at a specific mask.
  void WriteVariant(std::string_view url, const CapabilityMask& mask,
                    std::string_view data,
                    ContentType ct = ContentType::kImage) {
    AlternateId id =
        MaskToAlternateId(static_cast<uint8_t>(mask.Encode() & 0xFF));
    AlternateMetadata meta;
    meta.full_mask = mask.Encode();
    meta.content_type = ct;

    auto wh = cache_->WriteAlternate(url, "", "", id, data.size(), meta);
    ASSERT_TRUE(wh.has_value()) << "WriteAlternate failed";
    auto bytes = std::as_bytes(std::span(data));
    ASSERT_TRUE(wh->write_sync(bytes).has_value());
    ASSERT_TRUE(wh->close_sync().has_value());
  }

  std::string cache_dir_;
  std::string cache_path_;
  std::unique_ptr<PageSpeedCache> cache_;
};

TEST_F(CacheTest, OpenAndClose) {
  CreateCache();
  // Cache is automatically closed by unique_ptr destructor
}

TEST_F(CacheTest, WriteAndReadBack) {
  CreateCache();

  std::string body = "<html><body>Hello</body></html>";
  WriteOriginal("/index.html", body, ContentType::kHtml);

  // Read back with default mask (same as what was written)
  CapabilityMask default_mask;
  auto result = cache_->ReadBestAlternate("/index.html", "", "", default_mask);
  ASSERT_TRUE(result.has_value());
  EXPECT_TRUE(result->is_valid());

  auto content = result->content();
  std::string_view read_data(reinterpret_cast<const char*>(content.data()),
                             content.size());
  EXPECT_EQ(read_data, body);
}

TEST_F(CacheTest, ReadMissReturnsError) {
  CreateCache();

  CapabilityMask mask;
  auto result = cache_->ReadBestAlternate("/nonexistent", "", "", mask);
  EXPECT_FALSE(result.has_value());
}

// ReadBestAlternate finds the original when no optimized variant exists.
TEST_F(CacheTest, FallbackToOriginal) {
  CreateCache();

  std::string body = "body { color: red; }";
  WriteOriginal("/style.css", body, ContentType::kCss);

  // Request with WebP+Desktop+4G capabilities — not an exact match
  // for the original (which has default mask), but should still find it.
  CapabilityMask webp_mask(
      CapabilityMask::ImageFormat::kWebP, CapabilityMask::Viewport::kDesktop,
      CapabilityMask::PixelDensity::k1x, CapabilityMask::SaveData::kOff,
      CapabilityMask::TransferEncoding::kIdentity);

  auto result = cache_->ReadBestAlternate("/style.css", "", "", webp_mask);
  ASSERT_TRUE(result.has_value());

  auto content = result->content();
  std::string_view read_data(reinterpret_cast<const char*>(content.data()),
                             content.size());
  EXPECT_EQ(read_data, body);

  // Metadata should show original's mask, not the requested mask.
  EXPECT_EQ(result->metadata.full_mask, CapabilityMask().Encode());
}

// ReadBestAlternate picks the optimized variant over the original.
TEST_F(CacheTest, PrefersOptimizedVariant) {
  CreateCache();

  std::string original_body = "original PNG data";
  std::string webp_body = "optimized WebP data";

  // Write original at default mask
  WriteOriginal("/hero.png", original_body);

  // Write WebP variant
  CapabilityMask webp_mask(
      CapabilityMask::ImageFormat::kWebP, CapabilityMask::Viewport::kDesktop,
      CapabilityMask::PixelDensity::k1x, CapabilityMask::SaveData::kOff,
      CapabilityMask::TransferEncoding::kIdentity);
  WriteVariant("/hero.png", webp_mask, webp_body);

  // Request with WebP capabilities should get WebP variant
  auto result = cache_->ReadBestAlternate("/hero.png", "", "", webp_mask);
  ASSERT_TRUE(result.has_value());

  auto content = result->content();
  std::string_view read_data(reinterpret_cast<const char*>(content.data()),
                             content.size());
  EXPECT_EQ(read_data, webp_body);
  EXPECT_EQ(result->metadata.full_mask, webp_mask.Encode());
}

// ReadResult move semantics keep data valid (zero-copy serving pattern).
TEST_F(CacheTest, ReadResultMoveKeepsDataValid) {
  CreateCache();

  std::string data = "Data that must survive move";
  WriteOriginal("/move.html", data, ContentType::kHtml);

  auto result =
      cache_->ReadBestAlternate("/move.html", "", "", CapabilityMask());
  ASSERT_TRUE(result.has_value());

  // Move (simulates what the handler does: moves to request context)
  auto moved = std::make_unique<pagespeed::ReadResult>(std::move(*result));

  auto content = moved->content();
  std::string_view read_data(reinterpret_cast<const char*>(content.data()),
                             content.size());
  EXPECT_EQ(read_data, data);
}

// Verify exact-match detection via metadata (nginx uses this to decide
// whether to notify the worker for variant generation).
TEST_F(CacheTest, ExactMatchDetection) {
  CreateCache();

  CapabilityMask default_mask;
  WriteOriginal("/img.jpg", "jpeg-data");

  // Read with default mask → exact match
  auto result = cache_->ReadBestAlternate("/img.jpg", "", "", default_mask);
  ASSERT_TRUE(result.has_value());
  EXPECT_EQ(result->metadata.full_mask, default_mask.Encode());

  // Read with WebP mask → fallback (not exact)
  CapabilityMask webp_mask(
      CapabilityMask::ImageFormat::kWebP, CapabilityMask::Viewport::kDesktop,
      CapabilityMask::PixelDensity::k1x, CapabilityMask::SaveData::kOff,
      CapabilityMask::TransferEncoding::kIdentity);
  auto result2 = cache_->ReadBestAlternate("/img.jpg", "", "", webp_mask);
  ASSERT_TRUE(result2.has_value());
  // Should return original (different mask), triggering worker notification
  EXPECT_NE(result2->metadata.full_mask, webp_mask.Encode());
}

// Sentinel read/write for Early Hints (nginx pattern).
TEST_F(CacheTest, EarlyHintsSentinel) {
  CreateCache();

  std::string hints = "/style.css\n/fonts.css\n";

  auto wh = cache_->WriteSentinel("/page.html", "", "", SentinelId::kEarlyHints,
                                  hints.size());
  ASSERT_TRUE(wh.has_value());
  auto bytes = std::as_bytes(std::span(hints));
  ASSERT_TRUE(wh->write_sync(bytes).has_value());
  ASSERT_TRUE(wh->close_sync().has_value());

  // Read back via ReadAlternate with sentinel ID
  auto result = cache_->ReadAlternate(
      "/page.html", "", "", static_cast<AlternateId>(SentinelId::kEarlyHints));
  ASSERT_TRUE(result.has_value());
  auto content = result->content();
  std::string_view read_data(reinterpret_cast<const char*>(content.data()),
                             content.size());
  EXPECT_EQ(read_data, hints);
}

// =============================================================================
// agent_optimize same-URL Accept negotiation (spec §8.4)
//
// These exercise the SAME production C++ APIs the nginx serve path uses, so the
// same-URL negotiation contract is proven without standing up a full
// nginx+worker+Chrome stack.  The module's serve decision
// (ngx_pagespeed_module.cc ~:2407-2439) is exactly:
//     entitled = WantsAgentMarkdown(Accept) && shared.agent_optimize_entitled
//     read     = ReadBestAlternate(url, host, scheme, mask, entitled)
//     served_agent_markdown = (read.metadata.full_mask & 0xFF) == kAgentMarkdown
// (the entitled agent flag rides BESIDE the capability mask, never in it).
// The end-to-end curl-against-real-nginx demo is the manual recipe in
// tools/run-test-nginx.sh.  Cache-layer read-back is covered separately in
// test/lib/cache/cache_test.cc (WriteAgentAlternateReadBack).
// =============================================================================

class AgentOptimizeServeTest : public CacheTest {
 protected:
  // A 32-byte content hash filled with `fill` (distinct hashes for the
  // bound/advanced cases).
  static std::array<std::byte, 32> HashOf(uint8_t fill) {
    std::array<std::byte, 32> h{};
    for (auto& b : h) b = static_cast<std::byte>(fill);
    return h;
  }

  // Write the kContentHash sentinel binding for a key (the D8 oracle the
  // serve-time equality gate reads).
  void WriteContentHash(std::string_view url,
                        const std::array<std::byte, 32>& hash) {
    auto wh = cache_->WriteSentinel(url, "", "", SentinelId::kContentHash,
                                    hash.size());
    ASSERT_TRUE(wh.has_value()) << "WriteSentinel(kContentHash) failed";
    ASSERT_TRUE(wh->write_sync(std::span<const std::byte>(hash)).has_value());
    ASSERT_TRUE(wh->close_sync().has_value());
  }

  // Write the rendered-DOM markdown as the kAgentMarkdown (0x7C) variant stamped
  // with origin_html_hash=HashOf(0x11), exactly as
  // BrowserAnalysisManager::OnAgentRenderDone does.  When `bind` (default), also
  // writes a matching kContentHash sentinel so the serve-time equality gate
  // (P2.5) passes.
  void WriteAgentMarkdown(std::string_view url, std::string_view md,
                          bool bind = true) {
    AlternateMetadata meta;
    meta.full_mask = static_cast<uint32_t>(SentinelId::kAgentMarkdown);
    meta.content_type = ContentType::kOther;
    meta.origin_content_type = "text/markdown";
    meta.flags |= AlternateMetadata::kFlagWorkerProcessed;
    meta.origin_html_hash = HashOf(0x11);
    auto wh = cache_->WriteAgentAlternate(url, "", "", md.size(), meta);
    ASSERT_TRUE(wh.has_value()) << "WriteAgentAlternate failed";
    auto bytes = std::as_bytes(std::span(md));
    ASSERT_TRUE(wh->write_sync(bytes).has_value());
    ASSERT_TRUE(wh->close_sync().has_value());
    if (bind) WriteContentHash(url, HashOf(0x11));
  }

  struct ServeOutcome {
    std::string body;
    bool served_agent_markdown = false;
    ContentType content_type = ContentType::kHtml;
    std::string origin_content_type;
  };

  // Drive the REAL shared serve gate (P3.1: PageSpeedCache::
  // ReadBestAlternateAgentByKey) — the same primitive the nginx handler now
  // calls — so this test exercises production code, not a replica of it.
  // `agent_optimize_entitled` is the operator/shared-config flag; the helper
  // computes the real AND with the Accept-header predicate, just like the
  // module.  The primitive returns the markdown only when entitled AND the
  // content-hash binding is fresh; otherwise it returns the best non-agent
  // variant, or NotFound (refused stale markdown).  On NotFound the serve path
  // falls back to the best non-agent variant — the cc stand-in for nginx's
  // `goto cache_miss -> origin` (covered end-to-end by tools/run-test-nginx.sh).
  ServeOutcome Serve(std::string_view url, const CapabilityMask& mask,
                     std::string_view accept, bool agent_optimize_entitled) {
    bool entitled =
        pagespeed::WantsAgentMarkdown(accept) && agent_optimize_entitled;
    auto key = pagespeed::PageSpeedCache::ComposeKeyPreNormalized(url, "", "");
    auto result = cache_->ReadBestAlternateAgentByKey(key, mask, entitled);
    ServeOutcome out;
    if (!result.has_value()) {
      // Refused (stale/unbound markdown) -> serve the best non-agent variant.
      auto html = cache_->ReadBestAlternate(url, "", "", mask, false);
      EXPECT_TRUE(html.has_value());
      if (!html.has_value()) return out;
      auto hc = html->content();
      out.body.assign(reinterpret_cast<const char*>(hc.data()), hc.size());
      out.served_agent_markdown = false;
      out.content_type = html->metadata.content_type;
      out.origin_content_type = html->metadata.origin_content_type;
      return out;
    }
    auto content = result->content();
    out.body.assign(reinterpret_cast<const char*>(content.data()),
                    content.size());
    out.served_agent_markdown =
        (result->metadata.full_mask & 0xFF) ==
        static_cast<uint32_t>(SentinelId::kAgentMarkdown);
    out.content_type = result->metadata.content_type;
    out.origin_content_type = result->metadata.origin_content_type;
    return out;
  }
};

// Entitled agent (Accept: text/markdown + flag on) → the markdown variant at
// the SAME url, with the kOther/text-markdown metadata the serve path emits.
TEST_F(AgentOptimizeServeTest, EntitledMarkdownRequestGetsMarkdownAtSameUrl) {
  CreateCache();
  WriteOriginal("/page.html", "<html><body><h1>Hi</h1></body></html>",
                ContentType::kHtml);
  WriteAgentMarkdown("/page.html", "# Hi\n");

  auto out = Serve("/page.html", CapabilityMask(), "text/markdown",
                   /*agent_optimize_entitled=*/true);
  EXPECT_TRUE(out.served_agent_markdown);
  EXPECT_EQ(out.body, "# Hi\n");
  EXPECT_EQ(out.content_type, ContentType::kOther);
  EXPECT_EQ(out.origin_content_type, "text/markdown");
}

// A browser (no text/markdown in Accept) → unchanged HTML, even when the flag
// is on and a markdown variant exists for the url.  (Keystone.)
TEST_F(AgentOptimizeServeTest, BrowserGetsHtmlNotMarkdown) {
  CreateCache();
  WriteOriginal("/page.html", "<html>HI</html>", ContentType::kHtml);
  WriteAgentMarkdown("/page.html", "# Hi\n");

  auto out = Serve("/page.html", CapabilityMask(),
                   "text/html,application/xhtml+xml,*/*",
                   /*agent_optimize_entitled=*/true);
  EXPECT_FALSE(out.served_agent_markdown);
  EXPECT_EQ(out.body, "<html>HI</html>");
  EXPECT_EQ(out.content_type, ContentType::kHtml);
}

// Even a `*/*` (wildcard) browser request must not match — WantsAgentMarkdown is
// never matched by `*/*`.
TEST_F(AgentOptimizeServeTest, WildcardAcceptGetsHtmlNotMarkdown) {
  CreateCache();
  WriteOriginal("/page.html", "<html>HI</html>", ContentType::kHtml);
  WriteAgentMarkdown("/page.html", "# Hi\n");

  auto out = Serve("/page.html", CapabilityMask(), "*/*",
                   /*agent_optimize_entitled=*/true);
  EXPECT_FALSE(out.served_agent_markdown);
  EXPECT_EQ(out.body, "<html>HI</html>");
}

// An agent that lists text/markdown but ranks it below HTML gets the HTML:
// q=0, a lower q, and an explicit q=0 that */* does not override.
TEST_F(AgentOptimizeServeTest, MarkdownNotPreferredGetsHtml) {
  CreateCache();
  WriteOriginal("/page.html", "<html>HI</html>", ContentType::kHtml);
  WriteAgentMarkdown("/page.html", "# Hi\n");

  for (std::string_view accept :
       {"text/markdown;q=0", "text/markdown;q=0.5, text/html",
        "text/markdown;q=0, */*"}) {
    auto out = Serve("/page.html", CapabilityMask(), accept,
                     /*agent_optimize_entitled=*/true);
    EXPECT_FALSE(out.served_agent_markdown) << accept;
    EXPECT_EQ(out.body, "<html>HI</html>") << accept;
  }
}

// Agents that list text/markdown first, tied with text/html or above it, get
// the Markdown copy (Claude Code and an MCP fetch tool's headers).
TEST_F(AgentOptimizeServeTest, MarkdownTiedOrPreferredGetsMarkdown) {
  CreateCache();
  WriteOriginal("/page.html", "<html>HI</html>", ContentType::kHtml);
  WriteAgentMarkdown("/page.html", "# Hi\n");

  for (std::string_view accept :
       {"text/markdown, text/html, */*",
        "text/markdown, text/html;q=0.9, */*;q=0.8"}) {
    auto out = Serve("/page.html", CapabilityMask(), accept,
                     /*agent_optimize_entitled=*/true);
    EXPECT_TRUE(out.served_agent_markdown) << accept;
    EXPECT_EQ(out.body, "# Hi\n") << accept;
  }
}

// Flag OFF (operator did not enable agent_optimize): even a text/markdown
// request gets the browser HTML — the AND is false.
TEST_F(AgentOptimizeServeTest, MarkdownRequestUnentitledGetsHtml) {
  CreateCache();
  WriteOriginal("/page.html", "<html>HI</html>", ContentType::kHtml);
  WriteAgentMarkdown("/page.html", "# Hi\n");

  auto out = Serve("/page.html", CapabilityMask(), "text/markdown",
                   /*agent_optimize_entitled=*/false);
  EXPECT_FALSE(out.served_agent_markdown);
  EXPECT_EQ(out.body, "<html>HI</html>");
}

// No markdown variant for the url → an entitled markdown request falls back to
// the best content variant (never a hard decline).
TEST_F(AgentOptimizeServeTest,
       EntitledMarkdownRequestNoVariantFallsBackToHtml) {
  CreateCache();
  WriteOriginal("/page.html", "<html>HI</html>", ContentType::kHtml);

  auto out = Serve("/page.html", CapabilityMask(), "text/markdown",
                   /*agent_optimize_entitled=*/true);
  EXPECT_FALSE(out.served_agent_markdown);
  EXPECT_EQ(out.body, "<html>HI</html>");
}

// The Accept-header predicate the module gates on: `text/markdown` named
// explicitly with a q above 0 and at least the q `text/html` resolves to; never
// matched by `*/*`; equal q selects Markdown.
TEST_F(AgentOptimizeServeTest, WantsAgentMarkdownDecisionMatrix) {
  EXPECT_TRUE(pagespeed::WantsAgentMarkdown("text/markdown"));
  EXPECT_TRUE(pagespeed::WantsAgentMarkdown("text/html, text/markdown"));
  EXPECT_TRUE(pagespeed::WantsAgentMarkdown("text/html;q=0.9, text/markdown"));
  EXPECT_TRUE(pagespeed::WantsAgentMarkdown("text/markdown;q=0.9"));
  EXPECT_FALSE(pagespeed::WantsAgentMarkdown("*/*"));
  EXPECT_FALSE(
      pagespeed::WantsAgentMarkdown("text/html,application/xhtml+xml,*/*"));
  EXPECT_FALSE(pagespeed::WantsAgentMarkdown("application/x-markdown"));
  EXPECT_FALSE(pagespeed::WantsAgentMarkdown(""));
}

// D8 Gap#2 (P2.5): a markdown variant whose stamped origin_html_hash no longer
// matches the LIVE kContentHash (origin advanced) is REFUSED — the entitled
// agent gets the (non-stale) HTML, never the superseded markdown.
TEST_F(AgentOptimizeServeTest, StaleBindingRefusesMarkdownServesHtml) {
  CreateCache();
  WriteOriginal("/page.html", "<html>FRESH</html>", ContentType::kHtml);
  WriteAgentMarkdown("/page.html", "# stale\n");  // stamped + bound to 0x11
  // Origin advanced: the live kContentHash is now a DIFFERENT hash.
  WriteContentHash("/page.html", HashOf(0x22));

  auto out = Serve("/page.html", CapabilityMask(), "text/markdown",
                   /*agent_optimize_entitled=*/true);
  EXPECT_FALSE(out.served_agent_markdown);
  EXPECT_EQ(out.body, "<html>FRESH</html>");
}

// D8 Gap#2 (P2.5): a markdown variant with NO kContentHash sentinel (unbound —
// e.g. the worker's fail-closed path left the binding absent) is REFUSED.
TEST_F(AgentOptimizeServeTest, UnboundMarkdownRefusedServesHtml) {
  CreateCache();
  WriteOriginal("/page.html", "<html>HI</html>", ContentType::kHtml);
  WriteAgentMarkdown("/page.html", "# unbound\n", /*bind=*/false);  // no oracle

  auto out = Serve("/page.html", CapabilityMask(), "text/markdown",
                   /*agent_optimize_entitled=*/true);
  EXPECT_FALSE(out.served_agent_markdown);
  EXPECT_EQ(out.body, "<html>HI</html>");
}

// =============================================================================
// /llms.txt serve tests — mirror the nginx handler decision over
// the REAL cache primitives (ReadAlternateByKey on the kLlmsTxt/kLlmsTxtMeta
// sentinels), exactly as ngx_http_pagespeed_handler now does.
// =============================================================================

class LlmsTxtServeTest : public CacheTest {
 protected:
  void WriteLlms(std::string_view url, std::string_view body) {
    auto wh =
        cache_->WriteSentinel(url, "", "", SentinelId::kLlmsTxt, body.size());
    ASSERT_TRUE(wh.has_value()) << "WriteSentinel(kLlmsTxt) failed";
    ASSERT_TRUE(wh->write_sync(std::as_bytes(std::span(body))).has_value());
    ASSERT_TRUE(wh->close_sync().has_value());
  }

  // Freshness record: [32B hash of `hash_fill`][8B big-endian next_refresh].
  void WriteLlmsMeta(std::string_view url, uint8_t hash_fill,
                     int64_t next_refresh) {
    std::array<std::byte, 40> rec{};
    for (int i = 0; i < 32; ++i) rec[i] = static_cast<std::byte>(hash_fill);
    for (int i = 0; i < 8; ++i) {
      rec[32 + i] =
          static_cast<std::byte>((next_refresh >> (8 * (7 - i))) & 0xFF);
    }
    auto wh = cache_->WriteSentinel(url, "", "", SentinelId::kLlmsTxtMeta,
                                    rec.size());
    ASSERT_TRUE(wh.has_value()) << "WriteSentinel(kLlmsTxtMeta) failed";
    ASSERT_TRUE(wh->write_sync(std::span<const std::byte>(rec)).has_value());
    ASSERT_TRUE(wh->close_sync().has_value());
  }

  enum class Decision : unsigned char { kDeclined, kServed };
  struct Outcome {
    Decision decision = Decision::kDeclined;
    std::string body;
    std::string content_type;
    bool noindex = false;
    bool cache_private = false;
    bool would_notify = false;
  };

  // Mirror of the ngx_http_pagespeed_handler /llms.txt branch.
  Outcome Serve(std::string_view url, bool enabled, int64_t now) {
    Outcome out;
    if (!enabled) return out;  // off / not entitled -> NGX_DECLINED
    auto key = pagespeed::PageSpeedCache::ComposeKeyPreNormalized(url, "", "");
    auto llms = cache_->ReadAlternateByKey(
        key, static_cast<AlternateId>(SentinelId::kLlmsTxt));
    const bool hit = llms.has_value() && llms->is_valid();
    // Build-notify is decided from kLlmsTxtMeta next-refresh for BOTH hit & miss.
    out.would_notify = true;
    {
      auto fresh = cache_->ReadAlternateByKey(
          key, static_cast<AlternateId>(SentinelId::kLlmsTxtMeta));
      if (fresh.has_value() && fresh->is_valid() &&
          fresh->content().size() >= 40) {
        auto mc = fresh->content();
        const auto* p = reinterpret_cast<const unsigned char*>(mc.data());
        int64_t next_refresh = 0;
        for (int i = 0; i < 8; ++i)
          next_refresh = (next_refresh << 8) | p[32 + i];
        out.would_notify = (now >= next_refresh);
      }
    }
    if (!hit)
      return out;  // miss -> NGX_DECLINED (origin serves), notify queued
    auto c = llms->content();
    out.body.assign(reinterpret_cast<const char*>(c.data()), c.size());
    out.decision = Decision::kServed;
    out.content_type = "text/markdown; charset=utf-8";
    out.noindex = true;
    out.cache_private = true;
    return out;
  }
};

TEST_F(LlmsTxtServeTest, EntitledPresentServesMarkdown) {
  CreateCache();
  WriteLlms("/llms.txt", "# example.com\n\n> index\n");
  WriteLlmsMeta("/llms.txt", 0x22, /*next_refresh=*/2'000'000'000);
  auto out = Serve("/llms.txt", /*enabled=*/true, /*now=*/1'000'000'000);
  EXPECT_EQ(out.decision, Decision::kServed);
  EXPECT_EQ(out.body, "# example.com\n\n> index\n");
  EXPECT_EQ(out.content_type, "text/markdown; charset=utf-8");
  EXPECT_TRUE(out.noindex);
  EXPECT_TRUE(out.cache_private);
  EXPECT_FALSE(out.would_notify);  // within TTL -> no rebuild
}

TEST_F(LlmsTxtServeTest, NotEnabledDeclines) {
  CreateCache();
  WriteLlms("/llms.txt", "# present\n");
  auto out = Serve("/llms.txt", /*enabled=*/false, /*now=*/1'000'000'000);
  EXPECT_EQ(out.decision, Decision::kDeclined);
  EXPECT_TRUE(out.body.empty());
  EXPECT_FALSE(out.would_notify);  // off => not even a build request
}

TEST_F(LlmsTxtServeTest, EnabledAbsentDeclinesAndNotifies) {
  CreateCache();
  auto out = Serve("/llms.txt", /*enabled=*/true, /*now=*/1'000'000'000);
  EXPECT_EQ(out.decision, Decision::kDeclined);  // miss -> origin (never 5xx)
  EXPECT_TRUE(out.would_notify);                 // cold miss triggers a build
}

TEST_F(LlmsTxtServeTest, PastTtlServesAndNotifies) {
  CreateCache();
  WriteLlms("/llms.txt", "# stale-but-valid\n");
  WriteLlmsMeta("/llms.txt", 0x22, /*next_refresh=*/1'000);
  auto out = Serve("/llms.txt", /*enabled=*/true, /*now=*/2'000'000'000);
  EXPECT_EQ(out.decision, Decision::kServed);  // serve the cached copy
  EXPECT_TRUE(out.would_notify);               // past TTL -> trigger rebuild
}

TEST_F(LlmsTxtServeTest, HitWithoutMetaServesAndNotifies) {
  CreateCache();
  WriteLlms("/llms.txt", "# no-meta\n");
  auto out = Serve("/llms.txt", /*enabled=*/true, /*now=*/1'000'000'000);
  EXPECT_EQ(out.decision, Decision::kServed);
  EXPECT_TRUE(out.would_notify);  // missing freshness record -> refresh
}

TEST_F(LlmsTxtServeTest, MissWithinBackoffDoesNotRenotify) {
  CreateCache();
  // No body (cold/unbuildable) but a recent backoff meta the worker stamped on a
  // failed build: nginx must NOT re-notify on every request (abuse control).
  WriteLlmsMeta("/llms.txt", 0x00, /*next_refresh=*/2'000'000'000);
  auto out = Serve("/llms.txt", /*enabled=*/true, /*now=*/1'000'000'000);
  EXPECT_EQ(out.decision, Decision::kDeclined);  // miss -> origin
  EXPECT_FALSE(out.would_notify);  // within backoff -> no rebuild storm
}

// =============================================================================
// Pre-Compressed Variant Tests (Phase 4.5)
// =============================================================================

// Write a CSS variant with gzip encoding, verify metadata round-trips.
TEST_F(CacheTest, GzipVariantMetadata) {
  CreateCache();

  std::string body = "compressed-css-data";
  CapabilityMask gzip_mask(
      CapabilityMask::ImageFormat::kOriginal,
      CapabilityMask::Viewport::kDesktop, CapabilityMask::PixelDensity::k1x,
      CapabilityMask::SaveData::kOff, CapabilityMask::TransferEncoding::kGzip);
  WriteVariant("/style.css", gzip_mask, body, ContentType::kCss);

  auto result = cache_->ReadBestAlternate("/style.css", "", "", gzip_mask);
  ASSERT_TRUE(result.has_value());
  EXPECT_TRUE(result->is_valid());

  // Verify metadata carries the gzip encoding.
  auto decoded = CapabilityMask::Decode(result->metadata.full_mask);
  EXPECT_EQ(decoded.transfer_encoding(),
            CapabilityMask::TransferEncoding::kGzip);
  EXPECT_EQ(result->metadata.content_type, ContentType::kCss);

  auto content = result->content();
  std::string_view read_data(reinterpret_cast<const char*>(content.data()),
                             content.size());
  EXPECT_EQ(read_data, body);
}

// Write a CSS variant with brotli encoding, verify metadata round-trips.
TEST_F(CacheTest, BrotliVariantMetadata) {
  CreateCache();

  std::string body = "brotli-compressed-css";
  CapabilityMask br_mask(CapabilityMask::ImageFormat::kOriginal,
                         CapabilityMask::Viewport::kDesktop,
                         CapabilityMask::PixelDensity::k1x,
                         CapabilityMask::SaveData::kOff,
                         CapabilityMask::TransferEncoding::kBrotli);
  WriteVariant("/style.css", br_mask, body, ContentType::kCss);

  auto result = cache_->ReadBestAlternate("/style.css", "", "", br_mask);
  ASSERT_TRUE(result.has_value());
  EXPECT_TRUE(result->is_valid());

  auto decoded = CapabilityMask::Decode(result->metadata.full_mask);
  EXPECT_EQ(decoded.transfer_encoding(),
            CapabilityMask::TransferEncoding::kBrotli);
  EXPECT_EQ(result->metadata.content_type, ContentType::kCss);

  auto content = result->content();
  std::string_view read_data(reinterpret_cast<const char*>(content.data()),
                             content.size());
  EXPECT_EQ(read_data, body);
}

// Write identity + gzip + brotli variants for same URL. Reading with a gzip
// mask should return the gzip variant (exact match scores +60).
TEST_F(CacheTest, EncodingExactMatchPreferred) {
  CreateCache();

  std::string identity_body = "identity-css";
  std::string gzip_body = "gzip-css";
  std::string brotli_body = "brotli-css";

  CapabilityMask identity_mask(CapabilityMask::ImageFormat::kOriginal,
                               CapabilityMask::Viewport::kDesktop,
                               CapabilityMask::PixelDensity::k1x,
                               CapabilityMask::SaveData::kOff,
                               CapabilityMask::TransferEncoding::kIdentity);
  CapabilityMask gzip_mask(
      CapabilityMask::ImageFormat::kOriginal,
      CapabilityMask::Viewport::kDesktop, CapabilityMask::PixelDensity::k1x,
      CapabilityMask::SaveData::kOff, CapabilityMask::TransferEncoding::kGzip);
  CapabilityMask brotli_mask(CapabilityMask::ImageFormat::kOriginal,
                             CapabilityMask::Viewport::kDesktop,
                             CapabilityMask::PixelDensity::k1x,
                             CapabilityMask::SaveData::kOff,
                             CapabilityMask::TransferEncoding::kBrotli);

  // Write all three encoding variants for the same URL.
  WriteVariant("/app.js", identity_mask, identity_body, ContentType::kJs);
  WriteVariant("/app.js", gzip_mask, gzip_body, ContentType::kJs);
  WriteVariant("/app.js", brotli_mask, brotli_body, ContentType::kJs);

  // Read with gzip mask → should get gzip variant.
  {
    auto result = cache_->ReadBestAlternate("/app.js", "", "", gzip_mask);
    ASSERT_TRUE(result.has_value());

    auto decoded = CapabilityMask::Decode(result->metadata.full_mask);
    EXPECT_EQ(decoded.transfer_encoding(),
              CapabilityMask::TransferEncoding::kGzip);

    auto content = result->content();
    std::string_view read_data(reinterpret_cast<const char*>(content.data()),
                               content.size());
    EXPECT_EQ(read_data, gzip_body);
  }

  // Read with brotli mask → should get brotli variant (not gzip).
  {
    auto result = cache_->ReadBestAlternate("/app.js", "", "", brotli_mask);
    ASSERT_TRUE(result.has_value());

    auto decoded = CapabilityMask::Decode(result->metadata.full_mask);
    EXPECT_EQ(decoded.transfer_encoding(),
              CapabilityMask::TransferEncoding::kBrotli);

    auto content = result->content();
    std::string_view read_data(reinterpret_cast<const char*>(content.data()),
                               content.size());
    EXPECT_EQ(read_data, brotli_body);
  }
}

// Write only identity variant, read with gzip mask → identity returned
// as fallback (selector gives +5 for identity fallback).
TEST_F(CacheTest, IdentityFallbackWhenNoCompressedVariant) {
  CreateCache();

  std::string body = "uncompressed-css";
  WriteOriginal("/fallback.css", body, ContentType::kCss);

  // Request with gzip encoding — no gzip variant exists.
  CapabilityMask gzip_mask(
      CapabilityMask::ImageFormat::kOriginal,
      CapabilityMask::Viewport::kDesktop, CapabilityMask::PixelDensity::k1x,
      CapabilityMask::SaveData::kOff, CapabilityMask::TransferEncoding::kGzip);

  auto result = cache_->ReadBestAlternate("/fallback.css", "", "", gzip_mask);
  ASSERT_TRUE(result.has_value());

  // Should return the original/identity variant as fallback.
  auto decoded = CapabilityMask::Decode(result->metadata.full_mask);
  EXPECT_EQ(decoded.transfer_encoding(),
            CapabilityMask::TransferEncoding::kIdentity);

  auto content = result->content();
  std::string_view read_data(reinterpret_cast<const char*>(content.data()),
                             content.size());
  EXPECT_EQ(read_data, body);
}

// =============================================================================
// Cache-generation lifetime tests (deferred slow-client write safety)
// =============================================================================
//
// These model the nginx module's cache-generation anchoring: on a cache HIT
// the response buffer points INTO the mmap-backed ReadResult span owned by a
// specific PageSpeedCache instance.  ngx_http_output_filter may DEFER the
// socket write for a slow client, so nginx re-reads that span across later
// event cycles AFTER the handler returns.  A worker purge/reset replaces the
// global cache with a fresh instance; the old instance must NOT be destroyed
// while an in-flight response still references its mmap (use-after-free /
// cross-tenant disclosure).  The module solves this by stashing a
// shared_ptr<PageSpeedCache> (cache_anchor) in the per-request ctx.  The
// tests below exercise that exact ownership pattern at the cache API level.

// A ReadResult/ReadHandle keeps the cache's mmap readable after the global
// owning reference has moved on to a *new* generation, as long as a second
// shared_ptr anchor (the per-request ctx) is held.
TEST(CacheGenerationLifetimeTest, AnchorKeepsMappedDataValidAcrossReopen) {
  std::string cache_dir = pagespeed::test::MakeTempDir();
  std::string cache_path = cache_dir + "/cache.vol";

  auto make_cache = [&]() -> std::shared_ptr<PageSpeedCache> {
    PageSpeedCacheConfig config;
    config.volume_path = cache_path;
    config.volume_size = static_cast<uint64_t>(10 * 1024 * 1024);
    auto result = PageSpeedCache::Create(config);
    EXPECT_TRUE(result.has_value()) << "Failed to create cache";
    return result.has_value()
               ? std::shared_ptr<PageSpeedCache>(std::move(*result))
               : nullptr;
  };

  // Models the worker-global g_cache.
  std::shared_ptr<PageSpeedCache> g_cache = make_cache();
  ASSERT_NE(g_cache, nullptr);

  const std::string body = "Tenant A secret content that must not be freed";
  {
    CapabilityMask default_mask;
    AlternateId id =
        MaskToAlternateId(static_cast<uint8_t>(default_mask.Encode() & 0xFF));
    AlternateMetadata meta;
    meta.full_mask = default_mask.Encode();
    meta.content_type = ContentType::kHtml;
    auto wh =
        g_cache->WriteAlternate("/secret.html", "", "", id, body.size(), meta);
    ASSERT_TRUE(wh.has_value());
    ASSERT_TRUE(wh->write_sync(std::as_bytes(std::span(body))).has_value());
    ASSERT_TRUE(wh->close_sync().has_value());
  }

  // Simulate a cache HIT: take a ReadResult and anchor the generation it
  // points into (this is ctx->read_result + ctx->cache_anchor).
  auto read =
      g_cache->ReadBestAlternate("/secret.html", "", "", CapabilityMask());
  ASSERT_TRUE(read.has_value());
  auto* req_read_result = new pagespeed::ReadResult(std::move(*read));
  auto* req_cache_anchor = new std::shared_ptr<PageSpeedCache>(g_cache);

  // Two back-to-back generation changes (worker purge/reset).  Each replaces
  // the global owning reference with a fresh instance.  Under the OLD fixed
  // one-deep retirement, the second change would destroy the instance the
  // in-flight response still references.  With the anchor, it survives.
  g_cache = make_cache();  // generation N -> N+1
  ASSERT_NE(g_cache, nullptr);
  g_cache = make_cache();  // generation N+1 -> N+2
  ASSERT_NE(g_cache, nullptr);

  // Deferred slow-client write: nginx re-reads the mmap span here, long
  // after the "handler" returned.  Must still be the original content.
  auto content = req_read_result->content();
  std::string_view served(reinterpret_cast<const char*>(content.data()),
                          content.size());
  EXPECT_EQ(served, body);

  // Cleanup ORDER (ngx_http_pagespeed_cleanup): the ReadHandle MUST be
  // destroyed before the anchor that may own the last reference to the cache
  // (cache.h invariant: all ReadHandles destroyed before the cache).
  delete req_read_result;
  delete req_cache_anchor;

  std::error_code ec;
  std::filesystem::remove_all(cache_dir, ec);
}

// The anchor is what keeps the data alive: dropping all owning references in
// the wrong order is the bug this fix prevents.  Here we confirm that holding
// only the global reference (no per-request anchor) and then reopening would
// drop the generation — i.e. the anchor is load-bearing.  We verify the
// positive case: with the anchor, content is still valid even after the
// global reference is fully released.
TEST(CacheGenerationLifetimeTest, AnchorAloneKeepsDataValidAfterGlobalDropped) {
  std::string cache_dir = pagespeed::test::MakeTempDir();
  std::string cache_path = cache_dir + "/cache.vol";

  PageSpeedCacheConfig config;
  config.volume_path = cache_path;
  config.volume_size = static_cast<uint64_t>(10 * 1024 * 1024);
  auto created = PageSpeedCache::Create(config);
  ASSERT_TRUE(created.has_value());
  std::shared_ptr<PageSpeedCache> g_cache(std::move(*created));

  const std::string body = "content anchored solely by the in-flight request";
  {
    CapabilityMask default_mask;
    AlternateId id =
        MaskToAlternateId(static_cast<uint8_t>(default_mask.Encode() & 0xFF));
    AlternateMetadata meta;
    meta.full_mask = default_mask.Encode();
    meta.content_type = ContentType::kHtml;
    auto wh = g_cache->WriteAlternate("/a.html", "", "", id, body.size(), meta);
    ASSERT_TRUE(wh.has_value());
    ASSERT_TRUE(wh->write_sync(std::as_bytes(std::span(body))).has_value());
    ASSERT_TRUE(wh->close_sync().has_value());
  }

  auto read = g_cache->ReadBestAlternate("/a.html", "", "", CapabilityMask());
  ASSERT_TRUE(read.has_value());
  auto req_read_result =
      std::make_unique<pagespeed::ReadResult>(std::move(*read));
  // The per-request ctx anchor (ctx->cache_anchor) holds a second reference.
  std::shared_ptr<PageSpeedCache> anchor = g_cache;

  // Release the worker-global reference entirely (extreme case: the worker
  // dropped every newer generation too).  The anchor alone keeps the
  // instance — and its mmap — alive.
  g_cache.reset();
  EXPECT_EQ(anchor.use_count(), 1);  // only the request holds it now

  auto content = req_read_result->content();
  std::string_view served(reinterpret_cast<const char*>(content.data()),
                          content.size());
  EXPECT_EQ(served, body);

  // Correct teardown order.
  req_read_result.reset();
  anchor.reset();

  std::error_code ec;
  std::filesystem::remove_all(cache_dir, ec);
}

#ifndef _WIN32
// The fix has a second half besides the mmap anchor: when a HIT is served via
// sendfile, the module dup()s the worker-global cache sendfile fd into the
// per-request ctx (ctx->sendfile_fd in ngx_pagespeed_module.cc), and the
// cleanup handler closes that request-owned fd before releasing the cache
// anchor.  This means a generation change that closes the *global* fd cannot
// pull the descriptor out from under a slow client still streaming the
// response.  This test models that fd-ownership invariant at the syscall level:
// a request-owned dup() of the cache-volume fd stays a live descriptor that
// still references the generation it was opened against, across two
// back-to-back generation changes that each close + reopen the global fd.  A
// regression that dropped the per-request dup() (serving straight from the
// global fd) would observe EBADF here once the global fd was closed.
TEST(CacheGenerationLifetimeTest,
     RequestOwnedSendfileFdSurvivesBackToBackPurge) {
  std::string cache_dir = pagespeed::test::MakeTempDir();
  std::string cache_path = cache_dir + "/cache.vol";

  auto make_cache = [&]() -> std::shared_ptr<PageSpeedCache> {
    PageSpeedCacheConfig config;
    config.volume_path = cache_path;
    config.volume_size = static_cast<uint64_t>(10 * 1024 * 1024);
    auto result = PageSpeedCache::Create(config);
    EXPECT_TRUE(result.has_value()) << "Failed to create cache";
    return result.has_value()
               ? std::shared_ptr<PageSpeedCache>(std::move(*result))
               : nullptr;
  };

  std::shared_ptr<PageSpeedCache> g_cache = make_cache();
  ASSERT_NE(g_cache, nullptr);

  // Worker-global sendfile fd over the cache volume (models g_cache_sendfile_fd,
  // opened once per generation via ngx_open_file on the actual
  // fingerprint-named volume file).
  int g_sendfile_fd = ::open(g_cache->VolumeFilePath().c_str(), O_RDONLY);
  ASSERT_GE(g_sendfile_fd, 0) << "open(cache volume) failed";
  struct stat orig{};
  ASSERT_EQ(::fstat(g_sendfile_fd, &orig), 0);

  // Cache HIT served via sendfile: the request captures its OWN dup of the fd
  // (this is ctx->sendfile_fd) so a later global close can't affect it.
  int req_sendfile_fd = ::dup(g_sendfile_fd);
  ASSERT_GE(req_sendfile_fd, 0) << "dup() failed";

  // Two back-to-back generation changes (worker purge/reset).  Each one closes
  // the previous worker-global fd and reopens a fresh one, and rotates the
  // cache shared_ptr -- exactly the sequence that, before the fix, would have
  // invalidated an in-flight sendfile.
  for (int i = 0; i < 2; ++i) {
    ASSERT_EQ(::close(g_sendfile_fd), 0);
    g_sendfile_fd = ::open(g_cache->VolumeFilePath().c_str(), O_RDONLY);
    ASSERT_GE(g_sendfile_fd, 0);
    g_cache = make_cache();
    ASSERT_NE(g_cache, nullptr);
  }

  // The request-owned dup must still be a live descriptor referencing the same
  // volume it was opened against.  fstat would fail with EBADF if a regression
  // let the generation change close the descriptor the request was using.
  struct stat cur{};
  ASSERT_EQ(::fstat(req_sendfile_fd, &cur), 0)
      << "request-owned sendfile fd was closed by a generation change (EBADF)";
  EXPECT_EQ(cur.st_ino, orig.st_ino);
  EXPECT_EQ(cur.st_size, orig.st_size);

  // Teardown order mirrors ngx_http_pagespeed_cleanup: close the request-owned
  // fd first, then drop the cache anchor.
  EXPECT_EQ(::close(req_sendfile_fd), 0);
  EXPECT_EQ(::close(g_sendfile_fd), 0);
  g_cache.reset();

  std::error_code ec;
  std::filesystem::remove_all(cache_dir, ec);
}
#endif  // _WIN32

// =============================================================================
// Zero-copy serve barrier decision (ps_barrier::ClassifyRenewal)
// =============================================================================
//
// Pure keep/copy/abort logic shared by the emit-time barrier and the
// lease-renewal timer in the nginx module.  Exercised here without nginx
// headers or a live cache; the buffer surgery and E2E torn serve are covered
// as noted in the deliverable (E2E needs a live wrap during a slow drain).

namespace psb = pagespeed::ps_barrier;
using pagespeed::LeaseRenewal;

TEST(ZeroCopyBarrierTest, TornAlwaysAborts) {
  // A committed wrap (epoch moved) aborts regardless of the deadline.
  EXPECT_EQ(psb::ClassifyRenewal(LeaseRenewal::kTorn, psb::kBarrierMarginNs / 2,
                                 psb::kBarrierMarginNs),
            psb::BarrierAction::kAbort);
  EXPECT_EQ(psb::ClassifyRenewal(LeaseRenewal::kTorn,
                                 pagespeed::ReadResult::kNoForcedWrap,
                                 psb::kBarrierMarginNs),
            psb::BarrierAction::kAbort);
}

TEST(ZeroCopyBarrierTest, CopyNowCopiesOut) {
  // A wrap in flight (region intact) copies out, never aborts.
  EXPECT_EQ(psb::ClassifyRenewal(LeaseRenewal::kCopyNow,
                                 pagespeed::ReadResult::kNoForcedWrap,
                                 psb::kBarrierMarginNs),
            psb::BarrierAction::kCopyOut);
}

TEST(ZeroCopyBarrierTest, LeasesOffCopiesWithoutAbort) {
  // Legacy semantics: no lease protection -> copy, but never RST a valid serve.
  EXPECT_EQ(psb::ClassifyRenewal(LeaseRenewal::kLeasesOff,
                                 pagespeed::ReadResult::kNoForcedWrap,
                                 psb::kBarrierMarginNs),
            psb::BarrierAction::kCopyOut);
}

TEST(ZeroCopyBarrierTest, OkFarDeadlineKeepsAlias) {
  // Lease live, no wrap deferred: the fast, common case keeps aliasing.
  EXPECT_EQ(psb::ClassifyRenewal(LeaseRenewal::kOk,
                                 pagespeed::ReadResult::kNoForcedWrap,
                                 psb::kBarrierMarginNs),
            psb::BarrierAction::kAlias);
  // Deadline comfortably beyond the margin: still aliases.
  EXPECT_EQ(psb::ClassifyRenewal(LeaseRenewal::kOk, psb::kBarrierMarginNs * 4,
                                 psb::kBarrierMarginNs),
            psb::BarrierAction::kAlias);
}

TEST(ZeroCopyBarrierTest, OkNearDeadlineCopiesOut) {
  // A ceiling-forced wrap reachable within the margin overrides a live lease:
  // copy out proactively before it can overwrite bytes still on the wire.
  EXPECT_EQ(psb::ClassifyRenewal(LeaseRenewal::kOk, psb::kBarrierMarginNs / 2,
                                 psb::kBarrierMarginNs),
            psb::BarrierAction::kCopyOut);
}

TEST(ZeroCopyBarrierTest, OkDeadlineBoundaryIsInclusive) {
  // At exactly the margin the wrap is treated as reachable (<=): copy out.
  EXPECT_EQ(psb::ClassifyRenewal(LeaseRenewal::kOk, psb::kBarrierMarginNs,
                                 psb::kBarrierMarginNs),
            psb::BarrierAction::kCopyOut);
  // One nanosecond beyond the margin: keep aliasing.
  EXPECT_EQ(psb::ClassifyRenewal(LeaseRenewal::kOk, psb::kBarrierMarginNs + 1,
                                 psb::kBarrierMarginNs),
            psb::BarrierAction::kAlias);
}

TEST(ZeroCopyBarrierTest, MarginExceedsTimerRecheckInterval) {
  // Rationale guard: the 2.0 barrier re-checks an aliased borrow only on the
  // 3 s lease-renewal timer, so the margin must exceed that interval (plus a
  // burst/jitter cushion) or a forced wrap could fall between two re-checks
  // unseen.  Pin the invariant so a future timer-interval change is caught.
  constexpr uint64_t kLeaseRenewIntervalNs = 3'000ULL * 1000 * 1000;
  EXPECT_GT(psb::kBarrierMarginNs, kLeaseRenewIntervalNs);
}

// =============================================================================
// Buffer surgery: sendfile tail mapping (ps_barrier::MapSendfileTail)
// =============================================================================
//
// The timer-driven tail de-alias maps a sendfile buffer's unsent volume span
// [file_pos, file_last) back into the borrowed content() view.  Content sits
// at content_file_offset within the volume; nginx advances file_pos as the
// socket accepts bytes.

TEST(ZeroCopySurgeryTest, SendfileTailMidDrain) {
  // Content at volume offset 1000, 500 bytes; 200 sent, 300 unsent.
  const auto s = psb::MapSendfileTail(/*file_pos=*/1200, /*file_last=*/1500,
                                      /*content_file_offset=*/1000,
                                      /*content_size=*/500);
  ASSERT_TRUE(s.valid);
  EXPECT_EQ(s.offset, 200u);
  EXPECT_EQ(s.length, 300u);
}

TEST(ZeroCopySurgeryTest, SendfileTailNothingSentYet) {
  const auto s = psb::MapSendfileTail(1000, 1500, 1000, 500);
  ASSERT_TRUE(s.valid);
  EXPECT_EQ(s.offset, 0u);
  EXPECT_EQ(s.length, 500u);
}

TEST(ZeroCopySurgeryTest, SendfileTailFullyDrainedIsValidEmpty) {
  // pos == last: everything accepted by the socket — valid, zero-length tail.
  const auto s = psb::MapSendfileTail(1500, 1500, 1000, 500);
  ASSERT_TRUE(s.valid);
  EXPECT_EQ(s.offset, 500u);
  EXPECT_EQ(s.length, 0u);
}

TEST(ZeroCopySurgeryTest, SendfileTailExactWholeSpanBoundary) {
  // offset + length == content_size is in-bounds (subspan to the last byte).
  const auto s = psb::MapSendfileTail(1499, 1500, 1000, 500);
  ASSERT_TRUE(s.valid);
  EXPECT_EQ(s.offset, 499u);
  EXPECT_EQ(s.length, 1u);
}

TEST(ZeroCopySurgeryTest, SendfileTailRejectsCursorBeforeContent) {
  // file_pos before the content start cannot map into the borrow.
  EXPECT_FALSE(psb::MapSendfileTail(999, 1500, 1000, 500).valid);
}

TEST(ZeroCopySurgeryTest, SendfileTailRejectsSpanPastContentEnd) {
  // One byte past the end of the borrow: fail closed.
  EXPECT_FALSE(psb::MapSendfileTail(1200, 1501, 1000, 500).valid);
  // Cursor itself past the end.
  EXPECT_FALSE(psb::MapSendfileTail(1501, 1501, 1000, 500).valid);
}

TEST(ZeroCopySurgeryTest, SendfileTailRejectsBackwardsSpan) {
  EXPECT_FALSE(psb::MapSendfileTail(1400, 1300, 1000, 500).valid);
}

TEST(ZeroCopySurgeryTest, SendfileTailRejectsNoFileOffsetSentinel) {
  // A RAM-cache hit / no persistent mapping has no volume offset: never map.
  EXPECT_FALSE(
      psb::MapSendfileTail(0, 100, pagespeed::ReadResult::kNoFileOffset, 500)
          .valid);
}

TEST(ZeroCopySurgeryTest, SendfileTailNoOverflowOnHugeOffsets) {
  // offset + length is computed without wrapping (guard is subtraction-form).
  EXPECT_FALSE(
      psb::MapSendfileTail(UINT64_MAX - 10, UINT64_MAX - 1, 0, 500).valid);
}

// =============================================================================
// Buffer surgery: owned-buffer swap (ps_barrier::RepointToOwned)
// =============================================================================

// Mirrors the ngx_buf_t fields the swap touches (RepointToOwned is templated
// over the buffer type precisely so this nginx-free mirror can exercise it).
struct MockNgxBuf {
  unsigned char* start = nullptr;
  unsigned char* pos = nullptr;
  unsigned char* last = nullptr;
  unsigned char* end = nullptr;
  void* file = nullptr;
  long file_pos = 0;
  long file_last = 0;
  unsigned memory : 1;
  unsigned temporary : 1;
  unsigned in_file : 1;
  MockNgxBuf() : memory(0), temporary(0), in_file(0) {}
};

TEST(ZeroCopySurgeryTest, RepointToOwnedFromMmapAlias) {
  unsigned char mmap_bytes[64] = {0};
  unsigned char owned[16] = {0};
  MockNgxBuf b;
  b.start = b.pos = mmap_bytes;
  b.last = b.end = mmap_bytes + 64;
  b.memory = 1;  // aliased, immutable

  psb::RepointToOwned(&b, owned, sizeof(owned));

  EXPECT_EQ(b.start, owned);
  EXPECT_EQ(b.pos, owned);
  EXPECT_EQ(b.last, owned + sizeof(owned));
  EXPECT_EQ(b.end, owned + sizeof(owned));
  // Pool-owned mutable bytes: temporary, NOT memory (memory asserts an
  // immutable alias downstream filters may retain).
  EXPECT_EQ(b.temporary, 1u);
  EXPECT_EQ(b.memory, 0u);
  EXPECT_EQ(b.in_file, 0u);
  EXPECT_EQ(b.file, nullptr);
}

TEST(ZeroCopySurgeryTest, RepointToOwnedFromSendfileClearsFileFields) {
  int fake_file = 42;
  unsigned char owned[8] = {0};
  MockNgxBuf b;
  b.in_file = 1;
  b.file = &fake_file;
  b.file_pos = 1200;
  b.file_last = 1500;

  psb::RepointToOwned(&b, owned, sizeof(owned));

  // The write filter must never re-issue a sendfile from the released borrow.
  EXPECT_EQ(b.in_file, 0u);
  EXPECT_EQ(b.file, nullptr);
  EXPECT_EQ(b.file_pos, 0);
  EXPECT_EQ(b.file_last, 0);
  EXPECT_EQ(b.pos, owned);
  EXPECT_EQ(b.last, owned + sizeof(owned));
  EXPECT_EQ(b.temporary, 1u);
  EXPECT_EQ(b.memory, 0u);
}

TEST(ZeroCopySurgeryTest, RepointToOwnedZeroLength) {
  unsigned char owned[1] = {0};
  MockNgxBuf b;
  psb::RepointToOwned(&b, owned, 0u);
  EXPECT_EQ(b.pos, owned);
  EXPECT_EQ(b.last, owned);  // empty span: pos == last (drained/no bytes)
  EXPECT_EQ(b.temporary, 1u);
}

// ===========================================================================
// Torn-serve E2E + h2 emit-gate
//
// The ZeroCopyBarrierTest cases above feed SYNTHETIC LeaseRenewal verdicts into
// ClassifyRenewal. The tests below force a REAL Cyclone forced-past-lease wrap
// under a held aliased borrow and drive the barrier with the REAL
// renew_lease_strict() verdict + ns_until_forced_wrap() — the live-wrap E2E the
// #954 barrier tests deferred. Recipe mirrors Cyclone's own lease-pinning
// forced-wrap integration test (short wrap ceiling, never-lapsing lease,
// re-read to keep the lease live while flooding the single small volume).
// ===========================================================================
class ZeroCopyLiveWrapTest : public ::testing::Test {
 protected:
  void SetUp() override {
    cache_dir_ = pagespeed::test::MakeTempDir();
    cache_path_ = cache_dir_ + "/cache.vol";
  }
  void TearDown() override {
    cache_.reset();
    if (!cache_dir_.empty()) {
      std::error_code ec;
      std::filesystem::remove_all(cache_dir_, ec);
    }
  }
  // Small single-writer volume: leases never lapse (30s) but the wrap ceiling
  // is short (300ms), so a sustained borrow forces a wrap within a few hundred
  // ms. ram_cache_size=0 makes every read a disk mmap borrow; checksum off
  // because the wrap deliberately overwrites the borrowed region.
  void CreateWrapProneCache() {
    PageSpeedCacheConfig config;
    config.volume_path = cache_path_;
    config.volume_size = static_cast<uint64_t>(10 * 1024 * 1024);
    config.ram_cache_size = 0;
    config.verify_checksum_on_read = false;  // torn region is read raw
    config.read_lease_duration = std::chrono::milliseconds(30000);
    config.lease_wrap_ceiling = std::chrono::milliseconds(300);
    auto result = PageSpeedCache::Create(config);
    ASSERT_TRUE(result.has_value()) << "Failed to create wrap-prone cache: "
                                    << static_cast<int>(result.error());
    cache_ = std::move(*result);
  }
  static CapabilityMask Mask() { return CapabilityMask(); }  // 0x08 original
  bool WriteFiller(const std::string& url, const std::string& data) {
    CapabilityMask mask = Mask();
    AlternateId id =
        MaskToAlternateId(static_cast<uint8_t>(mask.Encode() & 0xFF));
    AlternateMetadata meta;
    meta.full_mask = mask.Encode();
    meta.content_type = ContentType::kImage;
    auto wh = cache_->WriteAlternate(url, "example.com", "https", id,
                                     data.size(), meta);
    if (!wh.has_value()) return false;
    if (!wh->write_sync(std::as_bytes(std::span(data))).has_value()) {
      return false;
    }
    return wh->close_sync().has_value();
  }
  // Drive Cyclone to a forced-past-lease wrap while the victim's stripe lease
  // is kept live by re-reading it: every 8 warm writes AND in the deadline
  // loop (mirrors the module's timer re-stamp). Without the warm re-reads the
  // 30s read lease can lapse on a slow (sanitizer) runner, and the wrap over
  // the victim is then an ordinary wrap that never counts as forced. Returns
  // true once Cyclone reports >= 1 forced wrap.
  bool ForceWrap(const std::string& victim_url) {
    const std::string filler(64 * 1024, 'x');
    // Warm with ONE volume's worth of fills (+ slack): that is enough to
    // bring the write cursor into contention with the victim's stripe.  On an
    // unpinned volume WriteFiller keeps succeeding forever (circular wrap), so
    // a success-gated loop would burn ~64MB of I/O per call; on a pinned one
    // fills start dropping (lease-deferred wrap), so also stop on failure.
    constexpr int kWarmWrites =
        static_cast<int>((10 * 1024 * 1024) / (64 * 1024)) + 8;
    const auto warm_start = std::chrono::steady_clock::now();
    for (int warm = 0; warm < kWarmWrites; ++warm) {
      // Re-read the victim so its read lease cannot lapse mid-warm however
      // slow the runner is (see the comment above).
      if (warm % 8 == 0) {
        (void)cache_->ReadBestAlternate(victim_url, "example.com", "https",
                                        Mask());  // keep the lease live
      }
      if (!WriteFiller("/warm/" + std::to_string(warm) + ".bin", filler)) {
        break;
      }
    }
    warm_elapsed_ms_ = std::chrono::duration_cast<std::chrono::milliseconds>(
                           std::chrono::steady_clock::now() - warm_start)
                           .count();
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(15);
    int attempt = 0;
    while (std::chrono::steady_clock::now() < deadline) {
      (void)cache_->ReadBestAlternate(victim_url, "example.com", "https",
                                      Mask());  // keep the lease live
      (void)WriteFiller("/c/" + std::to_string(attempt++) + ".bin", filler);
      if (cache_->Stats().wraps_forced_past_lease >= 1) break;
      std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    return cache_->Stats().wraps_forced_past_lease >= 1;
  }

  // Failure diagnostic for the ForceWrap() asserts: warm-phase duration and
  // the forced-wrap counter, so a future flake explains itself.
  std::string WrapFailDiag() const {
    uint64_t forced = 0;
    if (cache_) {
      forced = cache_->Stats().wraps_forced_past_lease;
    }
    return "warm " + std::to_string(warm_elapsed_ms_) +
           " ms, wraps_forced_past_lease " + std::to_string(forced);
  }
  std::string cache_dir_;
  std::string cache_path_;
  std::unique_ptr<PageSpeedCache> cache_;
  // Warm-phase duration of the last ForceWrap() call (failure diagnostics).
  long long warm_elapsed_ms_ = 0;
};

// F3.1: a live forced wrap under a held alias must drive the barrier to kAbort.
TEST_F(ZeroCopyLiveWrapTest, ForcedWrapDrivesBarrierToAbort) {
  CreateWrapProneCache();
  const std::string victim_bytes(4096, 'V');
  ASSERT_TRUE(WriteFiller("/victim.bin", victim_bytes));

  auto borrow =
      cache_->ReadBestAlternate("/victim.bin", "example.com", "https", Mask());
  ASSERT_TRUE(borrow.has_value());
  ASSERT_TRUE(borrow->is_valid());

  // Fresh, uncontended borrow: the real barrier keeps aliasing (fast path).
  EXPECT_EQ(borrow->renew_lease_strict(), LeaseRenewal::kOk);
  EXPECT_EQ(psb::ClassifyRenewal(borrow->renew_lease_strict(),
                                 borrow->ns_until_forced_wrap(),
                                 psb::kBarrierMarginNs),
            psb::BarrierAction::kAlias);

  ASSERT_TRUE(ForceWrap("/victim.bin"))
      << "Cyclone never forced a wrap (" << WrapFailDiag() << ")";

  // The forced wrap moved the stripe epoch: the REAL strict renew now reports
  // kTorn, and the barrier maps that to kAbort — the aliased serve fails closed
  // (RST), never streaming the overwritten bytes.
  const LeaseRenewal verdict = borrow->renew_lease_strict();
  EXPECT_EQ(verdict, LeaseRenewal::kTorn);
  EXPECT_EQ(psb::ClassifyRenewal(verdict, borrow->ns_until_forced_wrap(),
                                 psb::kBarrierMarginNs),
            psb::BarrierAction::kAbort)
      << "a torn borrow must ABORT (no foreign bytes on the wire)";
}

// F3.1: copy-then-verify, driven through the ps_barrier::CopyThenVerify seam
// that ps_zerocopy_copy_out / ps_zerocopy_dealias_tail share. The tear is
// INJECTED by the verify callback instead of raced for against a live wrap:
// driving a real Cyclone wrap made the old case fail in its setup on a slow
// (sanitizer) runner, where the victim's read lease lapsed during the warm
// phase and the wrap was an ordinary one that never counted as forced.
// Torn verdict: the callback reports the wrap landing mid-copy (overwriting
// the source from under the copy); the copy is discarded, never served.
TEST(ZeroCopyCopyThenVerifyTest, TornVerdictDiscards) {
  std::string src(4096, 'V');
  std::string dst(src.size(), '\0');
  const bool ok =
      psb::CopyThenVerify(src.data(), src.size(), dst.data(), [&src] {
        src.assign(src.size(), 'X');
        return LeaseRenewal::kTorn;
      });
  EXPECT_FALSE(ok);
}

// Clean path: the copy happens BEFORE verify (the property the protocol
// depends on), verify returns kOk, and the intact copy is kept.
TEST(ZeroCopyCopyThenVerifyTest, CleanCopyVerifiesAndKeepsBytes) {
  const std::string src(4096, 'V');
  std::string dst(src.size(), '\0');
  bool verified_after_copy = false;
  const bool ok = psb::CopyThenVerify(src.data(), src.size(), dst.data(), [&] {
    verified_after_copy = (dst == src);
    return LeaseRenewal::kOk;
  });
  EXPECT_TRUE(ok);
  EXPECT_EQ(dst, src);
  EXPECT_TRUE(verified_after_copy);
}

// Only kTorn discards: the wrap-in-flight and leases-off verdicts keep the
// (verified) copy, matching the module's copy sites.
TEST(ZeroCopyCopyThenVerifyTest, NonTornVerdictsKeepTheCopy) {
  for (const LeaseRenewal verdict :
       {LeaseRenewal::kCopyNow, LeaseRenewal::kLeasesOff}) {
    const std::string src(64, 'V');
    std::string dst(src.size(), '\0');
    EXPECT_TRUE(psb::CopyThenVerify(src.data(), src.size(), dst.data(),
                                    [verdict] { return verdict; }))
        << "verdict " << static_cast<int>(verdict);
  }
}

// F3.2: the emit-time copy gate forces copy for h2/h3, Range, subrequests, and
// in-memory/temporary filters; a plain HTTP/1.1 GET stays alias-eligible.
TEST(ZeroCopyGateTest, Http2AndHttp3ForceCopy) {
  EXPECT_TRUE(psb::MustCopyForEmit(/*http_version_gt_11=*/true, false, false,
                                   false, false, false));
}
TEST(ZeroCopyGateTest, PlainHttp11StaysAliasEligible) {
  EXPECT_FALSE(psb::MustCopyForEmit(false, false, false, false, false, false));
}
TEST(ZeroCopyGateTest, RangeForcesCopy) {
  EXPECT_TRUE(psb::MustCopyForEmit(false, /*has_range=*/true, false, false,
                                   false, false));
}
TEST(ZeroCopyGateTest, SubrequestForcesCopy) {
  EXPECT_TRUE(psb::MustCopyForEmit(false, false, /*is_subrequest=*/true, false,
                                   false, false));
}
TEST(ZeroCopyGateTest, InMemoryAndTemporaryFiltersForceCopy) {
  EXPECT_TRUE(psb::MustCopyForEmit(false, false, false,
                                   /*filter_need_in_memory=*/true, false,
                                   false));
  EXPECT_TRUE(psb::MustCopyForEmit(false, false, false, false,
                                   /*filter_need_temporary=*/true, false));
  EXPECT_TRUE(psb::MustCopyForEmit(false, false, false, false, false,
                                   /*main_filter_need_in_memory=*/true));
}

// F3.2: over HTTP/2 the forced copy path serves CORRECT bytes under wrap
// pressure — the whole body is copied+verified BEFORE any drain, so the served
// bytes are process-owned and immune to a later wrap (contrast the alias path
// above, which must abort).  Load-bearing shape: the borrow is deliberately
// HELD (not released, unlike the module's copy path) purely as a tear-detector
// probe, so after the forced wrap the test can assert BOTH halves of the
// property the gate buys us — the aliased view genuinely went kTorn (serving
// it would have been foreign bytes) WHILE the owned copy stayed correct.
// Covered here: gate predicate + copy-then-verify + copy immunity to a real
// wrap.  NOT covered: nginx's socket-level h2 per-frame drain (the Docker e2e
// follow-up).
TEST_F(ZeroCopyLiveWrapTest, H2CopyGateServesCorrectBytesUnderWrap) {
  CreateWrapProneCache();
  const std::string victim_bytes(4096, 'H');
  ASSERT_TRUE(WriteFiller("/h2.bin", victim_bytes));

  auto borrow =
      cache_->ReadBestAlternate("/h2.bin", "example.com", "https", Mask());
  ASSERT_TRUE(borrow.has_value());
  ASSERT_TRUE(borrow->is_valid());

  // The gate forces copy on h2 (http_version > 1.1).
  ASSERT_TRUE(psb::MustCopyForEmit(/*http_version_gt_11=*/true, false, false,
                                   false, false, false));

  // Copy-then-verify while the fresh lease is live: verify kOk, copy trusted.
  auto content = borrow->content();
  const std::string owned(reinterpret_cast<const char*>(content.data()),
                          content.size());
  ASSERT_EQ(borrow->renew_lease_strict(), LeaseRenewal::kOk);

  // Force a REAL wrap over the borrowed region (the held lease defers it only
  // until the 300ms ceiling).
  ASSERT_TRUE(ForceWrap("/h2.bin"))
      << "Cyclone never forced a wrap (" << WrapFailDiag() << ")";

  // The wrap genuinely invalidated the aliased view: had we served the alias
  // (the non-gated path), these would be foreign bytes.
  EXPECT_EQ(borrow->renew_lease_strict(), LeaseRenewal::kTorn)
      << "the forced wrap must tear the held aliased borrow";

  // ... while the owned copy — what the h2 copy path actually serves — is
  // separate process-owned memory and stays CORRECT.
  EXPECT_EQ(owned, victim_bytes);
}

}  // namespace
