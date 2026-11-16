// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

// Tests for the Analysis Resource Map builder and the <base href> injector.

#include "src/worker/analysis_resource_map.h"

#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "gtest/gtest.h"
#include "lib/classify/url_normalizer.h"

namespace pagespeed {
namespace {

// Lookup that returns preset content for specific (cache-keyable) URLs and
// records every URL it was asked for.
struct RecordingLookup {
  std::unordered_map<std::string, std::string> entries;
  mutable std::vector<std::string> asked;

  ResourceLookupFn fn() {
    return [this](std::string_view url) -> std::optional<std::string> {
      asked.emplace_back(url);
      auto it = entries.find(std::string(url));
      if (it != entries.end()) return it->second;
      return std::nullopt;
    };
  }
};

// Flag-free normalization config (the worker's default): NormalizeCacheUrl
// still percent-normalizes and sorts/dedups query params.
const UrlNormalizationConfig& NoNorm() {
  static const UrlNormalizationConfig* config = new UrlNormalizationConfig();
  return *config;
}

// --- BuildAnalysisResourceMap: URL resolution + keying ---

// Production page URLs are cache-normalized (path-only).  A document-relative
// src must be looked up under its path form and keyed under the absolute URL
// Chrome requests once the absolute <base> is present.
TEST(BuildAnalysisResourceMap, RelativeSrcPathOnlyPageUrl) {
  RecordingLookup lookup;
  lookup.entries["/blog/js/app.js"] = "console.log(1);";

  AnalysisResourceMapStats stats;
  auto map = BuildAnalysisResourceMap(
      "<html><head><script src=\"js/app.js\"></script></head></html>",
      "/blog/post.html", "example.com", "https", lookup.fn(), NoNorm(), &stats);

  ASSERT_EQ(1u, map.size());
  ASSERT_TRUE(map.contains("https://example.com/blog/js/app.js"));
  EXPECT_EQ("console.log(1);", map["https://example.com/blog/js/app.js"]);
  ASSERT_EQ(1u, lookup.asked.size());
  EXPECT_EQ("/blog/js/app.js", lookup.asked[0]);
  EXPECT_EQ(1u, stats.scripts_found);
  EXPECT_EQ(1u, stats.scripts_cached);
  EXPECT_EQ(0u, stats.scripts_uncached_same_origin);
  EXPECT_EQ(0u, stats.scripts_cross_origin);
  EXPECT_EQ(0u, stats.scripts_oversize);
  EXPECT_EQ(std::string("console.log(1);").size(), stats.bytes_mapped);
}

TEST(BuildAnalysisResourceMap, RootRelativeSrc) {
  RecordingLookup lookup;
  lookup.entries["/js/app.js"] = "x";

  auto map = BuildAnalysisResourceMap(
      "<html><head><script src=\"/js/app.js\"></script></head></html>",
      "/blog/post.html", "example.com", "https", lookup.fn(), NoNorm());

  ASSERT_EQ(1u, map.size());
  EXPECT_TRUE(map.contains("https://example.com/js/app.js"));
  ASSERT_EQ(1u, lookup.asked.size());
  EXPECT_EQ("/js/app.js", lookup.asked[0]);
}

TEST(BuildAnalysisResourceMap, AbsoluteSameOriginSrc) {
  RecordingLookup lookup;
  lookup.entries["/js/app.js"] = "x";

  auto map = BuildAnalysisResourceMap(
      "<html><head>"
      "<script src=\"https://example.com/js/app.js\"></script>"
      "</head></html>",
      "/index.html", "example.com", "https", lookup.fn(), NoNorm());

  ASSERT_EQ(1u, map.size());
  EXPECT_TRUE(map.contains("https://example.com/js/app.js"));
  ASSERT_EQ(1u, lookup.asked.size());
  EXPECT_EQ("/js/app.js", lookup.asked[0]);
}

TEST(BuildAnalysisResourceMap, ProtocolRelativeSameOrigin) {
  RecordingLookup lookup;
  lookup.entries["/a.js"] = "x";

  auto map = BuildAnalysisResourceMap(
      "<html><head><script src=\"//example.com/a.js\"></script></head></html>",
      "/index.html", "example.com", "http", lookup.fn(), NoNorm());

  ASSERT_EQ(1u, map.size());
  EXPECT_TRUE(map.contains("http://example.com/a.js"));
}

TEST(BuildAnalysisResourceMap, AbsolutePageUrlAlsoWorks) {
  RecordingLookup lookup;
  lookup.entries["/dir/js/app.js"] = "x";

  auto map = BuildAnalysisResourceMap(
      "<html><head><script src=\"js/app.js\"></script></head></html>",
      "https://example.com/dir/page.html", "example.com", "https", lookup.fn(),
      NoNorm());

  ASSERT_EQ(1u, map.size());
  EXPECT_TRUE(map.contains("https://example.com/dir/js/app.js"));
}

// The lookup URL is the worker's store-key form: a query stripped for
// configured static extensions must be stripped here too, or a
// query-versioned src permanently misses.  The MAP KEY keeps the raw
// request-form URL Chrome asks for.
TEST(BuildAnalysisResourceMap, QueryVersionedSrcLookupNormalized) {
  UrlNormalizationConfig norm;
  norm.strip_query_extensions.insert(".js");
  RecordingLookup lookup;
  lookup.entries["/app.js"] = "x";

  AnalysisResourceMapStats stats;
  auto map = BuildAnalysisResourceMap(
      "<html><head><script src=\"/app.js?v=123\"></script></head></html>",
      "/index.html", "example.com", "https", lookup.fn(), norm, &stats);

  ASSERT_EQ(1u, map.size());
  EXPECT_TRUE(map.contains("https://example.com/app.js?v=123"));
  ASSERT_EQ(1u, lookup.asked.size());
  EXPECT_EQ("/app.js", lookup.asked[0]);
  EXPECT_EQ(1u, stats.scripts_cached);
  EXPECT_EQ(0u, stats.scripts_uncached_same_origin);
}

// Multi-param queries: the lookup sees the normalized (param-sorted) form,
// the key preserves the authored query string.
TEST(BuildAnalysisResourceMap, MultiParamQueryLookupSortedKeyPreserved) {
  RecordingLookup lookup;
  lookup.entries["/w.js?a=1&b=2"] = "x";

  auto map = BuildAnalysisResourceMap(
      "<html><head><script src=\"/w.js?b=2&a=1\"></script></head></html>",
      "/index.html", "example.com", "https", lookup.fn(), NoNorm());

  ASSERT_EQ(1u, map.size());
  EXPECT_TRUE(map.contains("https://example.com/w.js?b=2&a=1"));
  ASSERT_EQ(1u, lookup.asked.size());
  EXPECT_EQ("/w.js?a=1&b=2", lookup.asked[0]);
}

// Chrome strips fragments from requests: the key and the lookup are both
// fragment-free, so a "#init"-suffixed src stays mappable and hittable.
TEST(BuildAnalysisResourceMap, FragmentStrippedFromKeyAndLookup) {
  RecordingLookup lookup;
  lookup.entries["/app.js"] = "x";

  auto map = BuildAnalysisResourceMap(
      "<html><head><script src=\"/app.js#init\"></script></head></html>",
      "/index.html", "example.com", "https", lookup.fn(), NoNorm());

  ASSERT_EQ(1u, map.size());
  EXPECT_TRUE(map.contains("https://example.com/app.js"));
  ASSERT_EQ(1u, lookup.asked.size());
  EXPECT_EQ("/app.js", lookup.asked[0]);
}

// "../" traversal resolves against the page directory (css template analog).
TEST(BuildAnalysisResourceMap, ParentTraversalResolved) {
  RecordingLookup lookup;
  lookup.entries["/blog/lib/a.js"] = "x";

  auto map = BuildAnalysisResourceMap(
      "<html><head><script src=\"../lib/a.js\"></script></head></html>",
      "/blog/post/index.html", "example.com", "https", lookup.fn(), NoNorm());

  ASSERT_EQ(1u, map.size());
  EXPECT_TRUE(map.contains("https://example.com/blog/lib/a.js"));
  ASSERT_EQ(1u, lookup.asked.size());
  EXPECT_EQ("/blog/lib/a.js", lookup.asked[0]);
}

// A "://" inside the query must not promote a relative src to absolute.
TEST(BuildAnalysisResourceMap, SchemeInQuerySrcIsRelative) {
  RecordingLookup lookup;
  lookup.entries["/dir/a.js?next=https://x"] = "x";

  AnalysisResourceMapStats stats;
  auto map = BuildAnalysisResourceMap(
      "<html><head><script src=\"a.js?next=https://x\"></script></head>"
      "</html>",
      "/dir/page.html", "example.com", "https", lookup.fn(), NoNorm(), &stats);

  ASSERT_EQ(1u, map.size());
  EXPECT_TRUE(map.contains("https://example.com/dir/a.js?next=https://x"));
  EXPECT_EQ(0u, stats.scripts_cross_origin);
  ASSERT_EQ(1u, lookup.asked.size());
  EXPECT_EQ("/dir/a.js?next=https://x", lookup.asked[0]);
}

// --- Author <base href>: relative srcs resolve against it ---

TEST(BuildAnalysisResourceMap, AuthorBaseResolvesRelativeSrcs) {
  RecordingLookup lookup;
  lookup.entries["/assets/js/app.js"] = "x";

  auto map = BuildAnalysisResourceMap(
      "<html><head><base href=\"https://example.com/assets/\">"
      "<script src=\"js/app.js\"></script></head></html>",
      "/index.html", "example.com", "https", lookup.fn(), NoNorm());

  ASSERT_EQ(1u, map.size());
  EXPECT_TRUE(map.contains("https://example.com/assets/js/app.js"));
  ASSERT_EQ(1u, lookup.asked.size());
  EXPECT_EQ("/assets/js/app.js", lookup.asked[0]);
}

// A relative base href is itself resolved against the page URL first.
TEST(BuildAnalysisResourceMap, RelativeAuthorBaseResolvedAgainstPageUrl) {
  RecordingLookup lookup;
  lookup.entries["/blog/assets/a.js"] = "x";

  auto map = BuildAnalysisResourceMap(
      "<html><head><base href=\"assets/\">"
      "<script src=\"a.js\"></script></head></html>",
      "/blog/post.html", "example.com", "https", lookup.fn(), NoNorm());

  ASSERT_EQ(1u, map.size());
  EXPECT_TRUE(map.contains("https://example.com/blog/assets/a.js"));
  ASSERT_EQ(1u, lookup.asked.size());
  EXPECT_EQ("/blog/assets/a.js", lookup.asked[0]);
}

// A cross-origin author base sends relative AND root-relative srcs to the
// other origin — structurally unmappable, counted cross-origin, never a
// TTL-heal trigger.
TEST(BuildAnalysisResourceMap, CrossOriginAuthorBaseMakesSrcsCrossOrigin) {
  RecordingLookup lookup;
  AnalysisResourceMapStats stats;
  auto map = BuildAnalysisResourceMap(
      "<html><head><base href=\"https://cdn.other.com/lib/\">"
      "<script src=\"a.js\"></script>"
      "<script src=\"/root.js\"></script></head></html>",
      "/index.html", "example.com", "https", lookup.fn(), NoNorm(), &stats);

  EXPECT_TRUE(map.empty());
  EXPECT_TRUE(lookup.asked.empty());
  EXPECT_EQ(2u, stats.scripts_cross_origin);
  EXPECT_EQ(0u, stats.scripts_uncached_same_origin);
}

// --- Cross-origin: never mapped, never looked up ---

TEST(BuildAnalysisResourceMap, CrossOriginSkipped) {
  RecordingLookup lookup;
  lookup.entries["/lib.js"] = "x";

  AnalysisResourceMapStats stats;
  auto map = BuildAnalysisResourceMap(
      "<html><head>"
      "<script src=\"https://cdn.example.net/lib.js\"></script>"
      "</head></html>",
      "/index.html", "example.com", "https", lookup.fn(), NoNorm(), &stats);

  EXPECT_TRUE(map.empty());
  EXPECT_TRUE(lookup.asked.empty());
  EXPECT_EQ(1u, stats.scripts_found);
  EXPECT_EQ(1u, stats.scripts_cross_origin);
  EXPECT_EQ(0u, stats.scripts_cached);
  EXPECT_EQ(0u, stats.scripts_uncached_same_origin);
}

TEST(BuildAnalysisResourceMap, ProtocolRelativeCrossOriginSkipped) {
  RecordingLookup lookup;
  AnalysisResourceMapStats stats;
  auto map = BuildAnalysisResourceMap(
      "<html><head><script src=\"//cdn.other.com/x.js\"></script></head>"
      "</html>",
      "/index.html", "example.com", "https", lookup.fn(), NoNorm(), &stats);
  EXPECT_TRUE(map.empty());
  EXPECT_EQ(1u, stats.scripts_cross_origin);
}

// Hostname comparison is ASCII-case-insensitive (a case variant must not be
// misclassified as cross-origin), and the key carries the Chrome-normalized
// (lowercased-host) request form so the fetch-time join still matches.
TEST(BuildAnalysisResourceMap, HostCaseVariantIsSameOrigin) {
  RecordingLookup lookup;
  lookup.entries["/a.js"] = "x";
  AnalysisResourceMapStats stats;
  auto map = BuildAnalysisResourceMap(
      "<html><head>"
      "<script src=\"https://EXAMPLE.com/a.js\"></script>"
      "</head></html>",
      "/index.html", "example.com", "https", lookup.fn(), NoNorm(), &stats);
  ASSERT_EQ(1u, map.size());
  EXPECT_TRUE(map.contains("https://example.com/a.js"));
  EXPECT_EQ(0u, stats.scripts_cross_origin);
  EXPECT_EQ(1u, stats.scripts_cached);
}

// --- Misses, caps, dedup ---

TEST(BuildAnalysisResourceMap, UncachedSameOriginCounted) {
  RecordingLookup lookup;  // empty: every lookup misses
  AnalysisResourceMapStats stats;
  auto map = BuildAnalysisResourceMap(
      "<html><head><script src=\"/missing.js\"></script></head></html>",
      "/index.html", "example.com", "https", lookup.fn(), NoNorm(), &stats);
  EXPECT_TRUE(map.empty());
  EXPECT_EQ(1u, stats.scripts_uncached_same_origin);
}

// Duplicate uncached srcs count once — the TTL-heal trigger keys on unique
// healable misses, not on how often a page repeats a src.
TEST(BuildAnalysisResourceMap, DuplicateUncachedSrcCountedOnce) {
  RecordingLookup lookup;  // empty: every lookup misses
  AnalysisResourceMapStats stats;
  auto map = BuildAnalysisResourceMap(
      "<html><head>"
      "<script src=\"/missing.js\"></script>"
      "<script src=\"/missing.js\"></script>"
      "</head></html>",
      "/index.html", "example.com", "https", lookup.fn(), NoNorm(), &stats);
  EXPECT_TRUE(map.empty());
  EXPECT_EQ(1u, lookup.asked.size());
  EXPECT_EQ(1u, stats.scripts_uncached_same_origin);
}

// A script that is cached but EMPTY (stubs, feature-flag placeholders) is
// unmapped like a miss (blocked -> keep-sync, kNoCoverageData) but is NOT a
// healable miss: a re-analysis re-reads the same zero bytes.  Counting it as
// uncached would trip the TTL-heal trigger on every analysis and pin the
// template on hourly re-analysis forever.
TEST(BuildAnalysisResourceMap, CachedEmptyScriptNotAHealableMiss) {
  RecordingLookup lookup;
  lookup.entries["/stub.js"] = "";

  AnalysisResourceMapStats stats;
  auto map = BuildAnalysisResourceMap(
      "<html><head><script src=\"/stub.js\"></script></head></html>",
      "/index.html", "example.com", "https", lookup.fn(), NoNorm(), &stats);

  EXPECT_TRUE(map.empty());
  EXPECT_EQ(1u, stats.scripts_empty_same_origin);
  // The TTL-clamp decision keys on scripts_uncached_same_origin > 0
  // (BrowserAnalysisManager -> ProfileExpiryFor), so this stat staying zero
  // is what keeps the profile on its configured TTL.
  EXPECT_EQ(0u, stats.scripts_uncached_same_origin);
  EXPECT_EQ(0u, stats.scripts_cached);
  EXPECT_EQ(0u, stats.scripts_oversize);
}

// Duplicate cross-origin srcs count once — the dedup runs before
// classification, so repeating a third-party src does not inflate the
// cross-origin observability stat.
TEST(BuildAnalysisResourceMap, DuplicateCrossOriginSrcCountedOnce) {
  RecordingLookup lookup;
  AnalysisResourceMapStats stats;
  auto map = BuildAnalysisResourceMap(
      "<html><head>"
      "<script src=\"https://cdn.example.net/lib.js\"></script>"
      "<script src=\"https://cdn.example.net/lib.js\"></script>"
      "</head></html>",
      "/index.html", "example.com", "https", lookup.fn(), NoNorm(), &stats);
  EXPECT_TRUE(map.empty());
  EXPECT_TRUE(lookup.asked.empty());
  EXPECT_EQ(2u, stats.scripts_found);
  EXPECT_EQ(1u, stats.scripts_cross_origin);
}

TEST(BuildAnalysisResourceMap, PerEntryCapSkipsOversize) {
  RecordingLookup lookup;
  lookup.entries["/huge.js"] = std::string(kMaxMappedScriptBytes + 1, 'x');
  lookup.entries["/small.js"] = "y";

  AnalysisResourceMapStats stats;
  auto map = BuildAnalysisResourceMap(
      "<html><head>"
      "<script src=\"/huge.js\"></script>"
      "<script src=\"/small.js\"></script>"
      "</head></html>",
      "/index.html", "example.com", "https", lookup.fn(), NoNorm(), &stats);

  ASSERT_EQ(1u, map.size());
  EXPECT_TRUE(map.contains("https://example.com/small.js"));
  // Over-cap degrades like a miss at serve time (blocked -> keep-sync) but is
  // NOT a healable miss: it must not count toward the TTL-heal trigger.
  EXPECT_EQ(1u, stats.scripts_oversize);
  EXPECT_EQ(0u, stats.scripts_uncached_same_origin);
  EXPECT_EQ(1u, stats.scripts_cached);
}

TEST(BuildAnalysisResourceMap, PerEntryCapExactBoundaryMapped) {
  RecordingLookup lookup;
  lookup.entries["/exact.js"] = std::string(kMaxMappedScriptBytes, 'x');
  auto map = BuildAnalysisResourceMap(
      "<html><head><script src=\"/exact.js\"></script></head></html>",
      "/index.html", "example.com", "https", lookup.fn(), NoNorm());
  EXPECT_EQ(1u, map.size());
}

TEST(BuildAnalysisResourceMap, TotalCapBoundsMap) {
  RecordingLookup lookup;
  std::string html = "<html><head>";
  // 8 entries of exactly 1 MiB fill the 8 MiB budget; the 9th is skipped.
  for (int i = 0; i < 9; ++i) {
    std::string path = "/s" + std::to_string(i) + ".js";
    lookup.entries[path] = std::string(kMaxMappedScriptBytes, 'a' + i);
    html += "<script src=\"" + path + "\"></script>";
  }
  html += "</head></html>";

  AnalysisResourceMapStats stats;
  auto map = BuildAnalysisResourceMap(html, "/index.html", "example.com",
                                      "https", lookup.fn(), NoNorm(), &stats);

  EXPECT_EQ(8u, map.size());
  EXPECT_EQ(8u, stats.scripts_cached);
  // Budget overflow is unhealable: counted oversize, never uncached.
  EXPECT_EQ(1u, stats.scripts_oversize);
  EXPECT_EQ(0u, stats.scripts_uncached_same_origin);
  EXPECT_EQ(kMaxMappedTotalBytes, stats.bytes_mapped);
}

TEST(BuildAnalysisResourceMap, DuplicateSrcMappedOnce) {
  RecordingLookup lookup;
  lookup.entries["/a.js"] = "x";
  AnalysisResourceMapStats stats;
  auto map = BuildAnalysisResourceMap(
      "<html><head>"
      "<script src=\"/a.js\"></script>"
      "<script src=\"/a.js\"></script>"
      "</head></html>",
      "/index.html", "example.com", "https", lookup.fn(), NoNorm(), &stats);
  EXPECT_EQ(1u, map.size());
  EXPECT_EQ(1u, lookup.asked.size());
  EXPECT_EQ(2u, stats.scripts_found);
  EXPECT_EQ(1u, stats.scripts_cached);
}

TEST(BuildAnalysisResourceMap, DataAndJavascriptUrisSkipped) {
  RecordingLookup lookup;
  auto map = BuildAnalysisResourceMap(
      "<html><head>"
      "<script src=\"data:text/javascript,1\"></script>"
      "<script src=\"javascript:void(0)\"></script>"
      "</head></html>",
      "/index.html", "example.com", "https", lookup.fn(), NoNorm());
  EXPECT_TRUE(map.empty());
  EXPECT_TRUE(lookup.asked.empty());
}

TEST(BuildAnalysisResourceMap, NoScriptsYieldsEmptyMap) {
  RecordingLookup lookup;
  AnalysisResourceMapStats stats;
  auto map = BuildAnalysisResourceMap(
      "<html><head></head><body>hi</body></html>", "/index.html", "example.com",
      "https", lookup.fn(), NoNorm(), &stats);
  EXPECT_TRUE(map.empty());
  EXPECT_EQ(0u, stats.scripts_found);
}

TEST(BuildAnalysisResourceMap, InlineScriptsNotCollected) {
  RecordingLookup lookup;
  AnalysisResourceMapStats stats;
  auto map = BuildAnalysisResourceMap(
      "<html><head><script>var a = 1;</script></head></html>", "/index.html",
      "example.com", "https", lookup.fn(), NoNorm(), &stats);
  EXPECT_TRUE(map.empty());
  EXPECT_EQ(0u, stats.scripts_found);
}

TEST(BuildAnalysisResourceMap, NullStatsPointerSafe) {
  RecordingLookup lookup;
  lookup.entries["/a.js"] = "x";
  auto map = BuildAnalysisResourceMap(
      "<html><head><script src=\"/a.js\"></script></head></html>",
      "/index.html", "example.com", "https", lookup.fn(), NoNorm(), nullptr);
  EXPECT_EQ(1u, map.size());
}

// --- InjectBaseHrefIfAbsent ---

TEST(InjectBaseHrefIfAbsent, InjectsAbsoluteBaseAtHeadStart) {
  std::string html = "<html><head><script src=\"a.js\"></script></head></html>";
  std::string out =
      InjectBaseHrefIfAbsent(html, "/blog/post.html", "example.com", "https");

  const std::string expected =
      "<base href=\"https://example.com/blog/post.html\">";
  size_t pos = out.find(expected);
  ASSERT_NE(std::string::npos, pos);
  // Injected immediately after the opening <head>, before the first script.
  EXPECT_EQ(out.find("<head>") + 6, pos);
  // Injected exactly once.
  EXPECT_EQ(std::string::npos, out.find(expected, pos + 1));
}

TEST(InjectBaseHrefIfAbsent, AuthorBasePresentLeftUntouched) {
  std::string html =
      "<html><head><base href=\"/other/\">"
      "<script src=\"a.js\"></script></head></html>";
  std::string out =
      InjectBaseHrefIfAbsent(html, "/blog/post.html", "example.com", "https");
  EXPECT_EQ(html, out);
}

TEST(InjectBaseHrefIfAbsent, HrefLessBaseDoesNotSuppressInjection) {
  std::string html =
      "<html><head><base target=\"_blank\"></head><body></body></html>";
  std::string out =
      InjectBaseHrefIfAbsent(html, "/p.html", "example.com", "https");
  EXPECT_NE(std::string::npos,
            out.find("<base href=\"https://example.com/p.html\">"));
}

TEST(InjectBaseHrefIfAbsent, HeadWithAttributes) {
  std::string html = "<html><head lang=\"en\"><title>t</title></head></html>";
  std::string out =
      InjectBaseHrefIfAbsent(html, "/p.html", "example.com", "https");
  size_t head_close =
      out.find("<head lang=\"en\">") + std::string("<head lang=\"en\">").size();
  EXPECT_EQ(head_close, out.find("<base href="));
}

TEST(InjectBaseHrefIfAbsent, NoHeadPrepends) {
  std::string html = "<html><body><script src=\"a.js\"></script></body></html>";
  std::string out =
      InjectBaseHrefIfAbsent(html, "/p.html", "example.com", "https");
  EXPECT_TRUE(out.starts_with("<base href=\"https://example.com/p.html\">"));
}

// "<header" must not be mistaken for "<head".
TEST(InjectBaseHrefIfAbsent, HeaderElementNotConfusedWithHead) {
  std::string html = "<html><body><header>x</header></body></html>";
  std::string out =
      InjectBaseHrefIfAbsent(html, "/p.html", "example.com", "https");
  EXPECT_TRUE(out.starts_with("<base "));
  EXPECT_NE(std::string::npos, out.find("<header>x</header>"));
}

// A "<head>" inside an HTML comment is not an insertion point — the base must
// land after the REAL <head>.
TEST(InjectBaseHrefIfAbsent, CommentedHeadNotAnInsertionPoint) {
  std::string html =
      "<!-- legacy <head> markup -->"
      "<html><head><title>t</title></head></html>";
  std::string out =
      InjectBaseHrefIfAbsent(html, "/p.html", "example.com", "https");
  const std::string expected = "<base href=\"https://example.com/p.html\">";
  size_t pos = out.find(expected);
  ASSERT_NE(std::string::npos, pos);
  // After the real <head>, not inside/before the comment.
  EXPECT_EQ(out.find("<html><head>") + std::string("<html><head>").size(), pos);
  // The comment is untouched.
  EXPECT_TRUE(out.starts_with("<!-- legacy <head> markup -->"));
}

// `<!-->` is a whole comment, as a browser reads it: the <head> after it is
// the real one.
TEST(InjectBaseHrefIfAbsent, ShortCommentEndsBeforeTheHead) {
  std::string html =
      "<!--><html><head><title>t</title></head><body><!-- x --></body></html>";
  std::string out =
      InjectBaseHrefIfAbsent(html, "/p.html", "example.com", "https");
  const std::string expected = "<base href=\"https://example.com/p.html\">";
  EXPECT_EQ(out.find(expected), out.find("<html><head>") + 12) << out;
}

TEST(InjectBaseHrefIfAbsent, EscapesHrefAttributeSpecials) {
  std::string out = InjectBaseHrefIfAbsent(
      "<html><head></head></html>", "/p?a=1&b=\"x\"", "example.com", "https");
  EXPECT_NE(
      std::string::npos,
      out.find(
          "<base href=\"https://example.com/p?a=1&amp;b=&quot;x&quot;\">"));
}

TEST(InjectBaseHrefIfAbsent, AbsolutePageUrlUsedVerbatim) {
  std::string out = InjectBaseHrefIfAbsent("<html><head></head></html>",
                                           "https://example.com/dir/page.html",
                                           "example.com", "https");
  EXPECT_NE(std::string::npos,
            out.find("<base href=\"https://example.com/dir/page.html\">"));
}

}  // namespace
}  // namespace pagespeed
