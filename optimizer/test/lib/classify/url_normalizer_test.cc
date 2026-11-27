// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

#include "lib/classify/url_normalizer.h"

#include <algorithm>
#include <string>

#include "gtest/gtest.h"

namespace pagespeed {
namespace {

// ---------------------------------------------------------------------------
// ExtractLowercaseExtension
// ---------------------------------------------------------------------------

TEST(ExtractLowercaseExtensionTest, BasicJpg) {
  EXPECT_EQ(ExtractLowercaseExtension("/img/photo.jpg"), ".jpg");
}

TEST(ExtractLowercaseExtensionTest, UppercaseJpg) {
  EXPECT_EQ(ExtractLowercaseExtension("/img/photo.JPG"), ".jpg");
}

TEST(ExtractLowercaseExtensionTest, MixedCaseExtension) {
  EXPECT_EQ(ExtractLowercaseExtension("/img/photo.JpG"), ".jpg");
}

TEST(ExtractLowercaseExtensionTest, DotInDirectoryNotExtension) {
  EXPECT_EQ(ExtractLowercaseExtension("/path.d/file"), "");
}

TEST(ExtractLowercaseExtensionTest, NoExtension) {
  EXPECT_EQ(ExtractLowercaseExtension("/path"), "");
}

TEST(ExtractLowercaseExtensionTest, TrailingDotOnly) {
  EXPECT_EQ(ExtractLowercaseExtension("/path/file."), "");
}

TEST(ExtractLowercaseExtensionTest, LastDotWins) {
  EXPECT_EQ(ExtractLowercaseExtension("/path/file.tar.gz"), ".gz");
}

TEST(ExtractLowercaseExtensionTest, QueryStripped) {
  EXPECT_EQ(ExtractLowercaseExtension("/img/photo.jpg?v=123"), ".jpg");
}

TEST(ExtractLowercaseExtensionTest, FragmentStripped) {
  EXPECT_EQ(ExtractLowercaseExtension("/img/photo.png#section"), ".png");
}

TEST(ExtractLowercaseExtensionTest, EmptyString) {
  EXPECT_EQ(ExtractLowercaseExtension(""), "");
}

TEST(ExtractLowercaseExtensionTest, NoSlash) {
  EXPECT_EQ(ExtractLowercaseExtension("photo.webp"), ".webp");
}

TEST(ExtractLowercaseExtensionTest, DotFile) {
  EXPECT_EQ(ExtractLowercaseExtension("/path/.htaccess"), ".htaccess");
}

// ---------------------------------------------------------------------------
// ExpandExtensionGroup
// ---------------------------------------------------------------------------

TEST(ExpandExtensionGroupTest, ImagesGroup) {
  auto exts = ExpandExtensionGroup("images");
  EXPECT_EQ(exts.size(), 10u);
  // Verify key members.
  EXPECT_NE(std::find(exts.begin(), exts.end(), ".jpg"), exts.end());
  EXPECT_NE(std::find(exts.begin(), exts.end(), ".jpeg"), exts.end());
  EXPECT_NE(std::find(exts.begin(), exts.end(), ".png"), exts.end());
  EXPECT_NE(std::find(exts.begin(), exts.end(), ".gif"), exts.end());
  EXPECT_NE(std::find(exts.begin(), exts.end(), ".webp"), exts.end());
  EXPECT_NE(std::find(exts.begin(), exts.end(), ".avif"), exts.end());
  EXPECT_NE(std::find(exts.begin(), exts.end(), ".svg"), exts.end());
  EXPECT_NE(std::find(exts.begin(), exts.end(), ".ico"), exts.end());
  EXPECT_NE(std::find(exts.begin(), exts.end(), ".bmp"), exts.end());
  EXPECT_NE(std::find(exts.begin(), exts.end(), ".tiff"), exts.end());
}

TEST(ExpandExtensionGroupTest, StaticGroup) {
  auto exts = ExpandExtensionGroup("static");
  EXPECT_EQ(exts.size(), 6u);
  EXPECT_NE(std::find(exts.begin(), exts.end(), ".css"), exts.end());
  EXPECT_NE(std::find(exts.begin(), exts.end(), ".js"), exts.end());
  EXPECT_NE(std::find(exts.begin(), exts.end(), ".woff"), exts.end());
  EXPECT_NE(std::find(exts.begin(), exts.end(), ".woff2"), exts.end());
  EXPECT_NE(std::find(exts.begin(), exts.end(), ".ttf"), exts.end());
  EXPECT_NE(std::find(exts.begin(), exts.end(), ".eot"), exts.end());
}

TEST(ExpandExtensionGroupTest, UnknownGroup) {
  EXPECT_TRUE(ExpandExtensionGroup("unknown").empty());
}

TEST(ExpandExtensionGroupTest, EmptyGroup) {
  EXPECT_TRUE(ExpandExtensionGroup("").empty());
}

TEST(ExpandExtensionGroupTest, CaseSensitive) {
  // "Images" is not "images".
  EXPECT_TRUE(ExpandExtensionGroup("Images").empty());
}

// ---------------------------------------------------------------------------
// NormalizeCacheUrl -- empty config (sorting / dedup / percent-encoding)
// ---------------------------------------------------------------------------

TEST(NormalizeCacheUrlTest, NoQuery) {
  UrlNormalizationConfig cfg;
  EXPECT_EQ(NormalizeCacheUrl("/page", cfg), "/page");
}

TEST(NormalizeCacheUrlTest, NoQueryPercentNormalized) {
  UrlNormalizationConfig cfg;
  // Even without a query string, percent-encoding must be normalized
  // to prevent cache splitting: /p%41ge and /pAge must produce the same key.
  EXPECT_EQ(NormalizeCacheUrl("/p%41ge", cfg), "/pAge");
}

TEST(NormalizeCacheUrlTest, FragmentOnlyNoQuery) {
  UrlNormalizationConfig cfg;
  // Fragment must be stripped even when there is no query string.
  EXPECT_EQ(NormalizeCacheUrl("/page#fragment", cfg), "/page");
}

TEST(NormalizeCacheUrlTest, FragmentOnlyPercentNormalized) {
  UrlNormalizationConfig cfg;
  // Both fragment stripping and percent normalization on the no-query path.
  EXPECT_EQ(NormalizeCacheUrl("/p%41ge#frag", cfg), "/pAge");
}

TEST(NormalizeCacheUrlTest, QuerySorting) {
  UrlNormalizationConfig cfg;
  EXPECT_EQ(NormalizeCacheUrl("/page?b=2&a=1", cfg), "/page?a=1&b=2");
}

TEST(NormalizeCacheUrlTest, AlreadySorted) {
  UrlNormalizationConfig cfg;
  EXPECT_EQ(NormalizeCacheUrl("/page?a=1&b=2", cfg), "/page?a=1&b=2");
}

TEST(NormalizeCacheUrlTest, DedupExact) {
  UrlNormalizationConfig cfg;
  EXPECT_EQ(NormalizeCacheUrl("/page?a=1&a=1", cfg), "/page?a=1");
}

TEST(NormalizeCacheUrlTest, DedupKeyOnly) {
  UrlNormalizationConfig cfg;
  // Key-only params (no '=') should be deduplicated.
  EXPECT_EQ(NormalizeCacheUrl("/page?debug&debug", cfg), "/page?debug");
}

TEST(NormalizeCacheUrlTest, KeepDistinctValues) {
  UrlNormalizationConfig cfg;
  EXPECT_EQ(NormalizeCacheUrl("/page?a=1&a=2", cfg), "/page?a=1&a=2");
}

TEST(NormalizeCacheUrlTest, EmptyQueryStripped) {
  UrlNormalizationConfig cfg;
  EXPECT_EQ(NormalizeCacheUrl("/page?", cfg), "/page");
}

TEST(NormalizeCacheUrlTest, EmptyParamsStripped) {
  UrlNormalizationConfig cfg;
  EXPECT_EQ(NormalizeCacheUrl("/page?&", cfg), "/page");
}

TEST(NormalizeCacheUrlTest, PercentUnreservedDecoded) {
  UrlNormalizationConfig cfg;
  // %41 = 'A', which is unreserved.
  EXPECT_EQ(NormalizeCacheUrl("/p%41ge?k%41=v%41", cfg), "/pAge?kA=vA");
}

TEST(NormalizeCacheUrlTest, PercentReservedPreserved) {
  UrlNormalizationConfig cfg;
  // %2F = '/', which is reserved.
  EXPECT_EQ(NormalizeCacheUrl("/page?k=%2F", cfg), "/page?k=%2F");
}

TEST(NormalizeCacheUrlTest, PercentLowercaseToUpper) {
  UrlNormalizationConfig cfg;
  EXPECT_EQ(NormalizeCacheUrl("/page?k=%2f", cfg), "/page?k=%2F");
}

TEST(NormalizeCacheUrlTest, FragmentStripped) {
  UrlNormalizationConfig cfg;
  EXPECT_EQ(NormalizeCacheUrl("/page?a=1#frag", cfg), "/page?a=1");
}

TEST(NormalizeCacheUrlTest, FragmentBeforeQueryMalformed) {
  UrlNormalizationConfig cfg;
  // Fragment appears before query: everything after '#' is stripped first,
  // leaving no query string.
  EXPECT_EQ(NormalizeCacheUrl("/page#frag?a=1", cfg), "/page");
}

TEST(NormalizeCacheUrlTest, KeyOnlyParam) {
  UrlNormalizationConfig cfg;
  // Key-only params sorted, no '=' appended.
  EXPECT_EQ(NormalizeCacheUrl("/page?foo&bar", cfg), "/page?bar&foo");
}

TEST(NormalizeCacheUrlTest, MixedKeyOnlyAndKeyValue) {
  UrlNormalizationConfig cfg;
  EXPECT_EQ(NormalizeCacheUrl("/page?foo&bar=1", cfg), "/page?bar=1&foo");
}

TEST(NormalizeCacheUrlTest, PlusPreserved) {
  UrlNormalizationConfig cfg;
  // '+' is NOT decoded to space; it is preserved literally.
  EXPECT_EQ(NormalizeCacheUrl("/page?q=hello+world", cfg),
            "/page?q=hello+world");
}

TEST(NormalizeCacheUrlTest, InvalidPercentSequence) {
  UrlNormalizationConfig cfg;
  // %GG is not valid hex, pass through unchanged.
  EXPECT_EQ(NormalizeCacheUrl("/page?k=%GG", cfg), "/page?k=%GG");
}

TEST(NormalizeCacheUrlTest, TrailingPercent) {
  UrlNormalizationConfig cfg;
  // Trailing '%' with no following hex digits — passed through.
  EXPECT_EQ(NormalizeCacheUrl("/page?k=val%", cfg), "/page?k=val%");
}

TEST(NormalizeCacheUrlTest, SinglePercentInMiddle) {
  UrlNormalizationConfig cfg;
  // '%' followed by only one char — not enough for a percent-encoded triplet.
  EXPECT_EQ(NormalizeCacheUrl("/page?k=a%2", cfg), "/page?k=a%2");
}

TEST(NormalizeCacheUrlTest, VeryLongUrlSortedCorrectly) {
  UrlNormalizationConfig cfg;
  std::string url = "/page?";
  for (int i = 0; i < 1000; ++i) {
    if (i > 0) url.push_back('&');
    url += "k" + std::to_string(i) + "=v" + std::to_string(i);
  }
  auto result = NormalizeCacheUrl(url, cfg);
  EXPECT_FALSE(result.empty());
  EXPECT_EQ(result.substr(0, 6), "/page?");
  // Verify sorting actually occurred: lexicographic sort puts k0 < k1 < k10.
  // k100 should come before k2 in lexicographic order.
  auto k100_pos = result.find("k100=");
  auto k2_pos = result.find("k2=");
  EXPECT_NE(k100_pos, std::string::npos);
  EXPECT_NE(k2_pos, std::string::npos);
  EXPECT_LT(k100_pos, k2_pos);
}

TEST(NormalizeCacheUrlTest, EmptyKeyEmptyValue) {
  UrlNormalizationConfig cfg;
  // "?=" is a param with empty key and empty value.
  EXPECT_EQ(NormalizeCacheUrl("/page?=", cfg), "/page?=");
}

TEST(NormalizeCacheUrlTest, MultipleAmpersands) {
  UrlNormalizationConfig cfg;
  // Empty params between '&' are skipped.
  EXPECT_EQ(NormalizeCacheUrl("/page?a=1&&b=2", cfg), "/page?a=1&b=2");
}

TEST(NormalizeCacheUrlTest, SortStability) {
  UrlNormalizationConfig cfg;
  // Same key, different values — sorted by value.
  EXPECT_EQ(NormalizeCacheUrl("/page?z=b&z=a", cfg), "/page?z=a&z=b");
}

TEST(NormalizeCacheUrlTest, PercentEncodedTilde) {
  UrlNormalizationConfig cfg;
  // %7E = '~', which is unreserved — should be decoded.
  EXPECT_EQ(NormalizeCacheUrl("/page?k=%7E", cfg), "/page?k=~");
}

TEST(NormalizeCacheUrlTest, PercentEncodedSpace) {
  UrlNormalizationConfig cfg;
  // %20 = ' ' (space), which is reserved — stays encoded.
  EXPECT_EQ(NormalizeCacheUrl("/page?k=%20", cfg), "/page?k=%20");
}

// ---------------------------------------------------------------------------
// NormalizeCacheUrl -- extension stripping
// ---------------------------------------------------------------------------

TEST(NormalizeCacheUrlTest, ExtensionStripJpg) {
  UrlNormalizationConfig cfg;
  cfg.strip_query_extensions.insert(".jpg");
  EXPECT_EQ(NormalizeCacheUrl("/img/photo.jpg?v=123", cfg), "/img/photo.jpg");
}

TEST(NormalizeCacheUrlTest, ExtensionStripUppercasePath) {
  UrlNormalizationConfig cfg;
  cfg.strip_query_extensions.insert(".jpg");
  // Path has .JPG, ExtractLowercaseExtension lowercases to .jpg → matches.
  // Path itself is preserved as-is (uppercase).
  EXPECT_EQ(NormalizeCacheUrl("/img/photo.JPG?v=123", cfg), "/img/photo.JPG");
}

TEST(NormalizeCacheUrlTest, ExtensionNotInConfig) {
  UrlNormalizationConfig cfg;
  cfg.strip_query_extensions.insert(".jpg");
  // .html is not in the strip set → query preserved.
  EXPECT_EQ(NormalizeCacheUrl("/page.html?a=1", cfg), "/page.html?a=1");
}

TEST(NormalizeCacheUrlTest, ExtensionGroupIntegration) {
  UrlNormalizationConfig cfg;
  // Simulate the worker's ExpandExtensionGroup → insert pipeline.
  for (const auto& ext : ExpandExtensionGroup("images")) {
    cfg.strip_query_extensions.insert(ext);
  }
  EXPECT_EQ(NormalizeCacheUrl("/img/photo.jpg?v=1", cfg), "/img/photo.jpg");
  EXPECT_EQ(NormalizeCacheUrl("/img/bg.png?t=2", cfg), "/img/bg.png");
  EXPECT_EQ(NormalizeCacheUrl("/img/icon.webp?h=abc", cfg), "/img/icon.webp");
  // Non-image: query preserved.
  EXPECT_EQ(NormalizeCacheUrl("/page.html?v=1", cfg), "/page.html?v=1");
}

TEST(NormalizeCacheUrlTest, ExtensionPercentEncodedInPath) {
  UrlNormalizationConfig cfg;
  cfg.strip_query_extensions.insert(".jpg");
  // %4A%50%47 → JPG (unreserved letters decoded), then ExtractLowercaseExtension
  // lowercases to .jpg → matches the strip set.
  EXPECT_EQ(NormalizeCacheUrl("/img/photo.%4A%50%47?v=1", cfg),
            "/img/photo.JPG");
}

TEST(NormalizeCacheUrlTest, ExtensionStripNoQuery) {
  UrlNormalizationConfig cfg;
  cfg.strip_query_extensions.insert(".jpg");
  // No query string → fast path, returned as-is.
  EXPECT_EQ(NormalizeCacheUrl("/img/photo.jpg", cfg), "/img/photo.jpg");
}

TEST(NormalizeCacheUrlTest, ExtensionStripWithFragment) {
  UrlNormalizationConfig cfg;
  cfg.strip_query_extensions.insert(".png");
  EXPECT_EQ(NormalizeCacheUrl("/img/bg.png?v=1#top", cfg), "/img/bg.png");
}

// ---------------------------------------------------------------------------
// NormalizeCacheUrl -- param stripping
// ---------------------------------------------------------------------------

TEST(NormalizeCacheUrlTest, ParamStripUtmSource) {
  UrlNormalizationConfig cfg;
  cfg.strip_query_params.insert("utm_source");
  EXPECT_EQ(NormalizeCacheUrl("/page?a=1&utm_source=google&b=2", cfg),
            "/page?a=1&b=2");
}

TEST(NormalizeCacheUrlTest, ParamStripAll) {
  UrlNormalizationConfig cfg;
  cfg.strip_query_params.insert("utm_source");
  // All params stripped → no query string.
  EXPECT_EQ(NormalizeCacheUrl("/page?utm_source=google", cfg), "/page");
}

TEST(NormalizeCacheUrlTest, ParamStripMultiple) {
  UrlNormalizationConfig cfg;
  cfg.strip_query_params.insert("utm_source");
  cfg.strip_query_params.insert("utm_medium");
  EXPECT_EQ(NormalizeCacheUrl("/page?utm_source=g&keep=1&utm_medium=cpc", cfg),
            "/page?keep=1");
}

TEST(NormalizeCacheUrlTest, ParamStripKeyOnly) {
  UrlNormalizationConfig cfg;
  cfg.strip_query_params.insert("debug");
  // Key-only param stripped.
  EXPECT_EQ(NormalizeCacheUrl("/page?a=1&debug", cfg), "/page?a=1");
}

TEST(NormalizeCacheUrlTest, ParamStripAndSort) {
  UrlNormalizationConfig cfg;
  cfg.strip_query_params.insert("fbclid");
  EXPECT_EQ(NormalizeCacheUrl("/page?z=1&fbclid=abc&a=2", cfg),
            "/page?a=2&z=1");
}

TEST(NormalizeCacheUrlTest, ParamStripIsCaseSensitive) {
  UrlNormalizationConfig cfg;
  cfg.strip_query_params.insert("utm_source");
  // UTM_SOURCE (uppercase) does NOT match utm_source — param preserved.
  EXPECT_EQ(NormalizeCacheUrl("/page?UTM_SOURCE=google", cfg),
            "/page?UTM_SOURCE=google");
}

// ---------------------------------------------------------------------------
// NormalizeCacheUrl -- combined extension + param config
// ---------------------------------------------------------------------------

TEST(NormalizeCacheUrlTest, ExtensionTakesPrecedenceOverParamStrip) {
  UrlNormalizationConfig cfg;
  cfg.strip_query_extensions.insert(".css");
  cfg.strip_query_params.insert("utm_source");
  // Extension matches → entire query stripped (param strip never reached).
  EXPECT_EQ(NormalizeCacheUrl("/style.css?utm_source=x&v=2", cfg),
            "/style.css");
}

TEST(NormalizeCacheUrlTest, ParamStripFallsThrough) {
  UrlNormalizationConfig cfg;
  cfg.strip_query_extensions.insert(".css");
  cfg.strip_query_params.insert("utm_source");
  // Extension does NOT match (.html) → param stripping applies.
  EXPECT_EQ(NormalizeCacheUrl("/page.html?utm_source=x&v=2", cfg),
            "/page.html?v=2");
}

// ---------------------------------------------------------------------------
// NormalizeCacheUrl -- percent-encoding security (Chromium-inspired)
// ---------------------------------------------------------------------------

TEST(NormalizeCacheUrlTest, NullByteStaysEncoded) {
  UrlNormalizationConfig cfg;
  // %00 = NUL, which is NOT unreserved — must stay encoded.
  EXPECT_EQ(NormalizeCacheUrl("/page%00test?a=1", cfg), "/page%00test?a=1");
}

TEST(NormalizeCacheUrlTest, NullByteInQueryStaysEncoded) {
  UrlNormalizationConfig cfg;
  EXPECT_EQ(NormalizeCacheUrl("/page?key=val%00ue", cfg), "/page?key=val%00ue");
}

TEST(NormalizeCacheUrlTest, DoubleEncodedPercentNotDoubleDecoded) {
  UrlNormalizationConfig cfg;
  // %25 = '%'. The result '%' should NOT then consume the following '41'.
  // %2541 → first pass decodes %25 to '%' (reserved, stays %25), then '41'
  // is literal. Actually: %25 → '%' is NOT unreserved, so stays as %25.
  EXPECT_EQ(NormalizeCacheUrl("/page?k=%2541", cfg), "/page?k=%2541");
}

TEST(NormalizeCacheUrlTest, NestedEscapeSequence) {
  UrlNormalizationConfig cfg;
  // %%41: first '%' + '%4' → invalid hex ('%' not hex digit) → '%' literal.
  // Then %41 → 'A' (unreserved). Result: %A.
  EXPECT_EQ(NormalizeCacheUrl("/page?k=%%41", cfg), "/page?k=%A");
}

TEST(NormalizeCacheUrlTest, EncodedDotDecodes) {
  UrlNormalizationConfig cfg;
  // %2E = '.', which IS unreserved per RFC 3986 — decoded to '.'.
  // This is safe because nginx resolves path traversal before we see r->uri.
  EXPECT_EQ(NormalizeCacheUrl("/foo/%2E/bar?a=1", cfg), "/foo/./bar?a=1");
}

TEST(NormalizeCacheUrlTest, EncodedDoubleDotDecodes) {
  UrlNormalizationConfig cfg;
  // %2E%2E decodes to '..' — ensures /foo/../bar and /foo/%2E%2E/bar
  // produce the same cache key (correct normalization).
  EXPECT_EQ(NormalizeCacheUrl("/foo/%2e%2e/bar?a=1", cfg), "/foo/../bar?a=1");
}

TEST(NormalizeCacheUrlTest, EncodedSlashPreserved) {
  UrlNormalizationConfig cfg;
  // %2F = '/', which is reserved — must stay encoded.
  EXPECT_EQ(NormalizeCacheUrl("/foo%2Fbar?a=1", cfg), "/foo%2Fbar?a=1");
}

TEST(NormalizeCacheUrlTest, EncodedQuestionMarkPreserved) {
  UrlNormalizationConfig cfg;
  // %3F = '?', reserved — stays encoded. Must not split the URL again.
  EXPECT_EQ(NormalizeCacheUrl("/page?k=%3Fv", cfg), "/page?k=%3Fv");
}

TEST(NormalizeCacheUrlTest, EncodedAmpersandPreserved) {
  UrlNormalizationConfig cfg;
  // %26 = '&', reserved — stays encoded. Must not split params.
  EXPECT_EQ(NormalizeCacheUrl("/page?k=a%26b", cfg), "/page?k=a%26b");
}

TEST(NormalizeCacheUrlTest, EncodedHashPreserved) {
  UrlNormalizationConfig cfg;
  // %23 = '#', reserved — stays encoded. Must not truncate as fragment.
  EXPECT_EQ(NormalizeCacheUrl("/page?k=val%23ue", cfg), "/page?k=val%23ue");
}

// ---------------------------------------------------------------------------
// NormalizeCacheUrl -- query string robustness (Chromium-inspired)
// ---------------------------------------------------------------------------

TEST(NormalizeCacheUrlTest, HashInsideQueryTruncates) {
  UrlNormalizationConfig cfg;
  // Raw '#' inside query acts as fragment delimiter.
  EXPECT_EQ(NormalizeCacheUrl("/page?a=val#ue&b=2", cfg), "/page?a=val");
}

TEST(NormalizeCacheUrlTest, EqualsInValue) {
  UrlNormalizationConfig cfg;
  // Only first '=' splits key from value. "a=b=c" → key="a", value="b=c".
  EXPECT_EQ(NormalizeCacheUrl("/page?a=b=c", cfg), "/page?a=b=c");
}

TEST(NormalizeCacheUrlTest, MultipleEqualsInValue) {
  UrlNormalizationConfig cfg;
  EXPECT_EQ(NormalizeCacheUrl("/page?token=a==&z=1", cfg),
            "/page?token=a==&z=1");
}

TEST(NormalizeCacheUrlTest, QueryOnlyNoPath) {
  UrlNormalizationConfig cfg;
  // Edge case: URL is just a query string with no path.
  EXPECT_EQ(NormalizeCacheUrl("?b=2&a=1", cfg), "?a=1&b=2");
}

TEST(NormalizeCacheUrlTest, EmptyUrl) {
  UrlNormalizationConfig cfg;
  EXPECT_EQ(NormalizeCacheUrl("", cfg), "");
}

TEST(NormalizeCacheUrlTest, JustQuestionMark) {
  UrlNormalizationConfig cfg;
  EXPECT_EQ(NormalizeCacheUrl("?", cfg), "");
}

TEST(NormalizeCacheUrlTest, QuestionMarkInQueryValue) {
  UrlNormalizationConfig cfg;
  // '?' after the first one is part of the query, not a delimiter.
  EXPECT_EQ(NormalizeCacheUrl("/page?q=what?&a=1", cfg), "/page?a=1&q=what?");
}

TEST(NormalizeCacheUrlTest, LargeSingleParamValue) {
  UrlNormalizationConfig cfg;
  // Single param with a very large value — no crash, no excessive allocation.
  std::string big_val(static_cast<size_t>(64) * 1024, 'x');
  std::string url = "/page?k=" + big_val;
  auto result = NormalizeCacheUrl(url, cfg);
  EXPECT_EQ(result, url);
}

// ---------------------------------------------------------------------------
// NormalizeCacheUrl -- hostile input (Chromium-inspired)
// ---------------------------------------------------------------------------

TEST(NormalizeCacheUrlTest, RawNullByteInUrl) {
  UrlNormalizationConfig cfg;
  // Raw NUL byte via string_view — passes through unchanged.
  // std::string is length-aware, so the '?' after NUL is still found.
  std::string url = "/page";
  url.push_back('\0');
  url += "?a=1";
  auto result = NormalizeCacheUrl(url, cfg);
  // The NUL is in the path portion, query still parsed.
  EXPECT_NE(result.find("a=1"), std::string::npos);
  // Path portion preserved (including the NUL byte).
  std::string expected_path = "/page";
  expected_path.push_back('\0');
  EXPECT_EQ(result.substr(0, expected_path.size()), expected_path);
}

TEST(NormalizeCacheUrlTest, RawControlCharsPassThrough) {
  UrlNormalizationConfig cfg;
  // Control characters (\x01, \x7f) pass through — they aren't percent sequences.
  EXPECT_EQ(NormalizeCacheUrl("/page?\x01=\x7f", cfg), "/page?\x01=\x7f");
}

TEST(NormalizeCacheUrlTest, TabAndNewlineInUrl) {
  UrlNormalizationConfig cfg;
  EXPECT_EQ(NormalizeCacheUrl("/page?a=1\t&b=2\n", cfg), "/page?a=1\t&b=2\n");
}

TEST(NormalizeCacheUrlTest, AllReservedCharsStayEncoded) {
  UrlNormalizationConfig cfg;
  // All RFC 3986 reserved characters: :/?#[]@!$&'()*+,;=
  // When percent-encoded, they must stay encoded.
  EXPECT_EQ(NormalizeCacheUrl("/page?k=%3A%2F%3F%23%5B%5D%40", cfg),
            "/page?k=%3A%2F%3F%23%5B%5D%40");
  EXPECT_EQ(NormalizeCacheUrl("/page?k=%21%24%26%27%28%29%2A%2B%2C%3B%3D", cfg),
            "/page?k=%21%24%26%27%28%29%2A%2B%2C%3B%3D");
}

TEST(NormalizeCacheUrlTest, AllUnreservedCharsDecoded) {
  UrlNormalizationConfig cfg;
  // Unreserved: A-Z, a-z, 0-9, -, ., _, ~
  // %2D=-, %2E=., %5F=_, %7E=~
  EXPECT_EQ(NormalizeCacheUrl("/page?k=%2D%2E%5F%7E", cfg), "/page?k=-._~");
}

// Scheme+authority stripping: full URLs are reduced to path+query so that
// the purge API (which receives full URLs) matches the path-only keys
// that nginx stores.
TEST(NormalizeCacheUrlTest, FullUrlStripsSchemeAndAuthority) {
  UrlNormalizationConfig cfg;
  EXPECT_EQ(NormalizeCacheUrl("https://example.com/page", cfg), "/page");
}

TEST(NormalizeCacheUrlTest, FullUrlWithQuery) {
  UrlNormalizationConfig cfg;
  EXPECT_EQ(NormalizeCacheUrl("https://example.com/page?a=1&b=2", cfg),
            "/page?a=1&b=2");
}

TEST(NormalizeCacheUrlTest, FullUrlRootPath) {
  UrlNormalizationConfig cfg;
  EXPECT_EQ(NormalizeCacheUrl("https://we-amp.com/", cfg), "/");
}

TEST(NormalizeCacheUrlTest, FullUrlNoPath) {
  UrlNormalizationConfig cfg;
  EXPECT_EQ(NormalizeCacheUrl("https://example.com", cfg), "/");
}

TEST(NormalizeCacheUrlTest, FullUrlWithPort) {
  UrlNormalizationConfig cfg;
  EXPECT_EQ(NormalizeCacheUrl("http://example.com:8080/path", cfg), "/path");
}

TEST(NormalizeCacheUrlTest, FullUrlWithFragment) {
  UrlNormalizationConfig cfg;
  EXPECT_EQ(NormalizeCacheUrl("https://example.com/page#frag", cfg), "/page");
}

TEST(NormalizeCacheUrlTest, FullUrlHttpScheme) {
  UrlNormalizationConfig cfg;
  EXPECT_EQ(NormalizeCacheUrl("http://example.com/path", cfg), "/path");
}

TEST(NormalizeCacheUrlTest, PathOnlyUnchanged) {
  UrlNormalizationConfig cfg;
  // Path-only URLs (from nginx) must pass through unchanged.
  EXPECT_EQ(NormalizeCacheUrl("/path?q=1", cfg), "/path?q=1");
}

TEST(NormalizeCacheUrlTest, FullUrlWithFragmentAndQuery) {
  UrlNormalizationConfig cfg;
  // Fragment must be stripped AFTER scheme stripping, not before.
  EXPECT_EQ(NormalizeCacheUrl("https://example.com/page?q=1#frag", cfg),
            "/page?q=1");
}

TEST(NormalizeCacheUrlTest, PathWithSchemeInQuery) {
  UrlNormalizationConfig cfg;
  // "://" in query string must NOT trigger scheme stripping.
  // The path-only URL passes through with its query intact.
  EXPECT_EQ(NormalizeCacheUrl("/redirect?url=https://other.com/foo", cfg),
            "/redirect?url=https://other.com/foo");
}

TEST(NormalizeCacheUrlTest, FullUrlWithUserinfo) {
  UrlNormalizationConfig cfg;
  // Userinfo in authority is stripped along with scheme+authority.
  EXPECT_EQ(NormalizeCacheUrl("https://user:pass@host.com/path", cfg), "/path");
}

// ---------------------------------------------------------------------------
// NormalizeCacheHostname
// ---------------------------------------------------------------------------

TEST(NormalizeCacheHostnameTest, EmptyConfig) {
  UrlNormalizationConfig cfg;
  // Delegates to NormalizeHostname: lowercase, strip trailing dot, strip :80.
  EXPECT_EQ(NormalizeCacheHostname("WWW.Example.COM.:80", cfg),
            "www.example.com");
}

TEST(NormalizeCacheHostnameTest, WithAlias) {
  UrlNormalizationConfig cfg;
  cfg.host_aliases["www.example.com"] = "example.com";
  EXPECT_EQ(NormalizeCacheHostname("www.example.com", cfg), "example.com");
}

TEST(NormalizeCacheHostnameTest, AliasAfterNormalization) {
  UrlNormalizationConfig cfg;
  cfg.host_aliases["www.example.com"] = "example.com";
  // Input needs normalization first, then alias lookup.
  EXPECT_EQ(NormalizeCacheHostname("WWW.Example.COM.:80", cfg), "example.com");
}

TEST(NormalizeCacheHostnameTest, UnmappedHostname) {
  UrlNormalizationConfig cfg;
  cfg.host_aliases["www.other.com"] = "other.com";
  // No alias for example.com → returns normalized hostname.
  EXPECT_EQ(NormalizeCacheHostname("example.com", cfg), "example.com");
}

TEST(NormalizeCacheHostnameTest, EmptyHostname) {
  UrlNormalizationConfig cfg;
  EXPECT_EQ(NormalizeCacheHostname("", cfg), "");
}

TEST(NormalizeCacheHostnameTest, EmptyHostnameWithAliases) {
  UrlNormalizationConfig cfg;
  cfg.host_aliases["www.example.com"] = "example.com";
  EXPECT_EQ(NormalizeCacheHostname("", cfg), "");
}

TEST(NormalizeCacheHostnameTest, NonDefaultPortPreserved) {
  UrlNormalizationConfig cfg;
  EXPECT_EQ(NormalizeCacheHostname("example.com:8080", cfg),
            "example.com:8080");
}

TEST(NormalizeCacheHostnameTest, AliasWithPort) {
  UrlNormalizationConfig cfg;
  cfg.host_aliases["example.com:8080"] = "example.com";
  EXPECT_EQ(NormalizeCacheHostname("example.com:8080", cfg), "example.com");
}

TEST(NormalizeCacheHostnameTest, Port443Stripped) {
  UrlNormalizationConfig cfg;
  // NormalizeHostname strips both :80 and :443.
  EXPECT_EQ(NormalizeCacheHostname("example.com:443", cfg), "example.com");
}

TEST(NormalizeCacheHostnameTest, IPv6Loopback) {
  UrlNormalizationConfig cfg;
  // IPv6 with default port stripped.
  EXPECT_EQ(NormalizeCacheHostname("[::1]:80", cfg), "[::1]");
}

TEST(NormalizeCacheHostnameTest, IPv6NonDefaultPort) {
  UrlNormalizationConfig cfg;
  EXPECT_EQ(NormalizeCacheHostname("[::1]:8080", cfg), "[::1]:8080");
}

TEST(NormalizeCacheHostnameTest, IPv6WithAlias) {
  UrlNormalizationConfig cfg;
  cfg.host_aliases["[::1]"] = "localhost";
  EXPECT_EQ(NormalizeCacheHostname("[::1]:80", cfg), "localhost");
}

TEST(NormalizeCacheHostnameTest, TrailingDotThenAlias) {
  UrlNormalizationConfig cfg;
  cfg.host_aliases["www.example.com"] = "example.com";
  // Trailing dot stripped first, then alias lookup.
  EXPECT_EQ(NormalizeCacheHostname("www.example.com.", cfg), "example.com");
}

}  // namespace
}  // namespace pagespeed
