// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

// Tests for HTTP 103 Early Hints utilities.

#include <algorithm>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "gtest/gtest.h"
#include "src/nginx/early_hints_util.h"

namespace pagespeed {
namespace {

TEST(FormatLinkHeaderTest, SingleStylesheet) {
  std::vector<PreloadResource> resources = {
      {"/style.css", "style", "preload", ""}};
  EXPECT_EQ(FormatLinkHeader(resources), "</style.css>; rel=preload; as=style");
}

TEST(FormatLinkHeaderTest, MultipleResources) {
  std::vector<PreloadResource> resources = {
      {"/style.css", "style", "preload", ""},
      {"/app.js", "script", "preload", ""},
      {"/hero.png", "image", "preload", ""},
  };
  EXPECT_EQ(FormatLinkHeader(resources),
            "</style.css>; rel=preload; as=style, "
            "</app.js>; rel=preload; as=script, "
            "</hero.png>; rel=preload; as=image");
}

TEST(FormatLinkHeaderTest, EmptyResources) {
  std::vector<PreloadResource> resources;
  EXPECT_EQ(FormatLinkHeader(resources), "");
}

TEST(FormatLinkHeaderTest, AbsoluteUrl) {
  std::vector<PreloadResource> resources = {
      {"https://cdn.example.com/style.css", "style", "preload", ""},
  };
  EXPECT_EQ(FormatLinkHeader(resources),
            "<https://cdn.example.com/style.css>; rel=preload; as=style");
}

TEST(ExtractStylesheetPreloadsTest, SingleStylesheet) {
  auto preloads = ExtractStylesheetPreloads(
      "<html><head>"
      "<link rel=\"stylesheet\" href=\"/style.css\">"
      "</head></html>");

  ASSERT_EQ(preloads.size(), 1u);
  EXPECT_EQ(preloads[0].url, "/style.css");
  EXPECT_EQ(preloads[0].as_type, "style");
}

TEST(ExtractStylesheetPreloadsTest, MultipleStylesheets) {
  auto preloads = ExtractStylesheetPreloads(
      "<html><head>"
      "<link rel=\"stylesheet\" href=\"/base.css\">"
      "<link rel=\"stylesheet\" href=\"/theme.css\">"
      "<link rel=\"stylesheet\" href=\"/page.css\">"
      "</head></html>");

  ASSERT_EQ(preloads.size(), 3u);
  EXPECT_EQ(preloads[0].url, "/base.css");
  EXPECT_EQ(preloads[1].url, "/theme.css");
  EXPECT_EQ(preloads[2].url, "/page.css");
}

TEST(ExtractStylesheetPreloadsTest, IgnoresNonStylesheetLinks) {
  auto preloads = ExtractStylesheetPreloads(
      "<html><head>"
      "<link rel=\"icon\" href=\"/favicon.ico\">"
      "<link rel=\"stylesheet\" href=\"/style.css\">"
      "<link rel=\"preconnect\" href=\"https://cdn.example.com\">"
      "</head></html>");

  ASSERT_EQ(preloads.size(), 1u);
  EXPECT_EQ(preloads[0].url, "/style.css");
}

TEST(ExtractStylesheetPreloadsTest, SingleQuotedHref) {
  auto preloads =
      ExtractStylesheetPreloads("<link rel='stylesheet' href='/style.css'>");

  ASSERT_EQ(preloads.size(), 1u);
  EXPECT_EQ(preloads[0].url, "/style.css");
}

TEST(ExtractStylesheetPreloadsTest, NoStylesheets) {
  auto preloads = ExtractStylesheetPreloads(
      "<html><head><title>Test</title></head>"
      "<body><p>No stylesheets</p></body></html>");

  EXPECT_TRUE(preloads.empty());
}

TEST(ExtractStylesheetPreloadsTest, EmptyHtml) {
  auto preloads = ExtractStylesheetPreloads("");
  EXPECT_TRUE(preloads.empty());
}

TEST(ExtractStylesheetPreloadsTest, HrefBeforeRel) {
  // href= appears before rel= in the tag.
  auto preloads = ExtractStylesheetPreloads(
      "<link href=\"/style.css\" rel=\"stylesheet\">");

  ASSERT_EQ(preloads.size(), 1u);
  EXPECT_EQ(preloads[0].url, "/style.css");
}

TEST(ExtractStylesheetPreloadsTest, AbsoluteUrl) {
  auto preloads = ExtractStylesheetPreloads(
      "<link rel=\"stylesheet\" "
      "href=\"https://cdn.example.com/bootstrap.min.css\">");

  ASSERT_EQ(preloads.size(), 1u);
  EXPECT_EQ(preloads[0].url, "https://cdn.example.com/bootstrap.min.css");
}

TEST(ExtractStylesheetPreloadsTest, UrlWithQueryString) {
  auto preloads = ExtractStylesheetPreloads(
      "<link rel=\"stylesheet\" href=\"/style.css?v=1.2.3\">");

  ASSERT_EQ(preloads.size(), 1u);
  EXPECT_EQ(preloads[0].url, "/style.css?v=1.2.3");
}

TEST(ExtractStylesheetPreloadsTest, MixedContent) {
  auto preloads = ExtractStylesheetPreloads(
      "<html><head>"
      "<meta charset=\"utf-8\">"
      "<link rel=\"stylesheet\" href=\"/a.css\">"
      "<script src=\"/app.js\"></script>"
      "<link rel=\"stylesheet\" href=\"/b.css\">"
      "</head><body>"
      "<link rel=\"stylesheet\" href=\"/c.css\">"
      "</body></html>");

  ASSERT_EQ(preloads.size(), 3u);
  EXPECT_EQ(preloads[0].url, "/a.css");
  EXPECT_EQ(preloads[1].url, "/b.css");
  EXPECT_EQ(preloads[2].url, "/c.css");
}

// Tests for SanitizeLinkUrl (0d: URL sanitization for Link headers).

TEST(SanitizeLinkUrlTest, CleanUrlPassesThrough) {
  EXPECT_EQ(SanitizeLinkUrl("/style.css"), "/style.css");
  EXPECT_EQ(SanitizeLinkUrl("https://cdn.example.com/style.css"),
            "https://cdn.example.com/style.css");
}

TEST(SanitizeLinkUrlTest, StripsCrLfAndNull) {
  EXPECT_EQ(SanitizeLinkUrl("/style.css\r\nX-Injected: evil"),
            "/style.cssX-Injected: evil");
}

TEST(SanitizeLinkUrlTest, StripsAngleBrackets) {
  EXPECT_EQ(SanitizeLinkUrl("/style.css<script>"), "/style.cssscript");
}

TEST(SanitizeLinkUrlTest, RejectsPercentEncodedCr) {
  EXPECT_EQ(SanitizeLinkUrl("/style.css%0D%0AX-Injected:%20evil"), "");
}

TEST(SanitizeLinkUrlTest, RejectsPercentEncodedLf) {
  EXPECT_EQ(SanitizeLinkUrl("/style.css%0a"), "");
}

TEST(SanitizeLinkUrlTest, RejectsPercentEncodedNull) {
  EXPECT_EQ(SanitizeLinkUrl("/style.css%00"), "");
}

TEST(SanitizeLinkUrlTest, PercentEncodedCaseInsensitive) {
  EXPECT_EQ(SanitizeLinkUrl("/style.css%0D"), "");
  EXPECT_EQ(SanitizeLinkUrl("/style.css%0d"), "");
}

TEST(SanitizeLinkUrlTest, EmptyUrlReturnsEmpty) {
  EXPECT_EQ(SanitizeLinkUrl(""), "");
}

// Verify FormatLinkHeader skips URLs that fail sanitization.
TEST(FormatLinkHeaderTest, SkipsDangerousUrls) {
  std::vector<PreloadResource> resources = {
      {"/safe.css", "style", "preload", ""},
      {"/evil%0D%0A.css", "style", "preload", ""},
      {"/also-safe.css", "style", "preload", ""},
  };
  EXPECT_EQ(FormatLinkHeader(resources),
            "</safe.css>; rel=preload; as=style, "
            "</also-safe.css>; rel=preload; as=style");
}

// --- ParseHintLine tests ---

TEST(ParseHintLineTest, StylesheetNoPrefix) {
  auto res = ParseHintLine("/style.css");
  EXPECT_EQ(res.url, "/style.css");
  EXPECT_EQ(res.as_type, "style");
  EXPECT_EQ(res.rel, "preload");
  EXPECT_TRUE(res.extra.empty());
}

TEST(ParseHintLineTest, ImagePrefix) {
  auto res = ParseHintLine("image:/hero.jpg");
  EXPECT_EQ(res.url, "/hero.jpg");
  EXPECT_EQ(res.as_type, "image");
  EXPECT_EQ(res.rel, "preload");
  EXPECT_EQ(res.extra, "fetchpriority=high");
}

TEST(ParseHintLineTest, FontPrefix) {
  auto res = ParseHintLine("font:/fonts/inter.woff2");
  EXPECT_EQ(res.url, "/fonts/inter.woff2");
  EXPECT_EQ(res.as_type, "font");
  EXPECT_EQ(res.rel, "preload");
  EXPECT_EQ(res.extra, "crossorigin");
}

TEST(ParseHintLineTest, PreconnectPrefix) {
  auto res = ParseHintLine("preconnect:https://cdn.example.com");
  EXPECT_EQ(res.url, "https://cdn.example.com");
  EXPECT_TRUE(res.as_type.empty());
  EXPECT_EQ(res.rel, "preconnect");
  EXPECT_TRUE(res.extra.empty());
}

TEST(ParseHintLineTest, EmptyLineYieldsNoUsableHint) {
  // An empty or zero-length sentinel means "no hints".  The module already
  // skips empty content and empty lines before parsing; even if an empty
  // line reached the parser, the empty URL sanitizes to an empty string,
  // which the Link-header builders drop.
  auto res = ParseHintLine("");
  EXPECT_TRUE(res.url.empty());
  EXPECT_TRUE(SanitizeLinkUrl(res.url).empty());
}

TEST(ParseHintLineTest, PreconnectCorsPrefix) {
  // CORS-mode origins carry crossorigin so the 103 preconnect warms the
  // same connection pool as the injected HTML preconnect.
  auto res = ParseHintLine("preconnect-cors:https://fonts.gstatic.com");
  EXPECT_EQ(res.url, "https://fonts.gstatic.com");
  EXPECT_TRUE(res.as_type.empty());
  EXPECT_EQ(res.rel, "preconnect");
  EXPECT_EQ(res.extra, "crossorigin");
}

TEST(FormatLinkHeaderTest, PreconnectResource) {
  std::vector<PreloadResource> resources = {
      {"https://cdn.example.com", "", "preconnect", ""},
  };
  EXPECT_EQ(FormatLinkHeader(resources),
            "<https://cdn.example.com>; rel=preconnect");
}

TEST(FormatLinkHeaderTest, PreconnectCorsResource) {
  std::vector<PreloadResource> resources = {
      {"https://fonts.gstatic.com", "", "preconnect", "crossorigin"},
  };
  EXPECT_EQ(FormatLinkHeader(resources),
            "<https://fonts.gstatic.com>; rel=preconnect; crossorigin");
}

TEST(FormatLinkHeaderTest, ImageWithFetchPriority) {
  std::vector<PreloadResource> resources = {
      {"/hero.jpg", "image", "preload", "fetchpriority=high"},
  };
  EXPECT_EQ(FormatLinkHeader(resources),
            "</hero.jpg>; rel=preload; as=image; fetchpriority=high");
}

TEST(FormatLinkHeaderTest, MixedPreloadAndPreconnect) {
  std::vector<PreloadResource> resources = {
      {"/style.css", "style", "preload", ""},
      {"https://cdn.example.com", "", "preconnect", ""},
      {"/hero.jpg", "image", "preload", "fetchpriority=high"},
  };
  EXPECT_EQ(FormatLinkHeader(resources),
            "</style.css>; rel=preload; as=style, "
            "<https://cdn.example.com>; rel=preconnect, "
            "</hero.jpg>; rel=preload; as=image; fetchpriority=high");
}

TEST(SanitizeLinkUrlTest, SafePercentEncodedCharacter) {
  // %20 is space (safe) — exercises the % branch without returning empty
  EXPECT_EQ(SanitizeLinkUrl("/path%20name.css"), "/path%20name.css");
  EXPECT_EQ(SanitizeLinkUrl("/path%2F.css"), "/path%2F.css");
}

TEST(ExtractStylesheetPreloadsTest, UnquotedHref) {
  // Exercises the unquoted href extraction branch (find_first_of " >")
  auto preloads =
      ExtractStylesheetPreloads("<link rel=\"stylesheet\" href=/style.css >");

  ASSERT_EQ(preloads.size(), 1u);
  EXPECT_EQ(preloads[0].url, "/style.css");
}

TEST(ExtractStylesheetPreloadsTest, UnquotedHrefEndedByAngleBracket) {
  auto preloads =
      ExtractStylesheetPreloads("<link rel=\"stylesheet\" href=/style.css>");

  ASSERT_EQ(preloads.size(), 1u);
  EXPECT_EQ(preloads[0].url, "/style.css");
}

// --- FlushWithRetry tests ---
//
// FlushWithRetry models the partial-write drain loop used by SendEarlyHints
// when writing the raw 103 response through nginx's c->send_chain. The writer
// callback returns the number of bytes STILL PENDING after each attempt
// (0 == fully flushed, >0 == partial write, <0 == hard error). The loop must
// keep retrying the remainder until the buffer is drained — the original code
// fired send_chain once and ignored a non-NULL (partial-write) return, which
// truncates the 103 and corrupts the response stream when the 200 follows.

// Helper: a writer driven by a scripted sequence of pending-byte counts.
namespace {
std::function<long()> ScriptedWriter(std::vector<long> pending,
                                     int* call_count = nullptr) {
  auto idx = std::make_shared<size_t>(0);
  auto seq = std::make_shared<std::vector<long>>(std::move(pending));
  auto counter = call_count;
  return [idx, seq, counter]() -> long {
    if (counter != nullptr) ++*counter;
    // Clamp to the last scripted value once exhausted (models a wedged socket
    // that keeps reporting the same pending count).
    size_t i = *idx < seq->size() ? *idx : seq->size() - 1;
    ++*idx;
    return (*seq)[i];
  };
}
}  // namespace

TEST(FlushWithRetryTest, CompletesWhenWriterDrainsImmediately) {
  int calls = 0;
  EXPECT_EQ(FlushWithRetry(ScriptedWriter({0}, &calls)),
            FlushStatus::kComplete);
  EXPECT_EQ(calls, 1);  // one call, fully sent.
}

TEST(FlushWithRetryTest, RetriesUntilFullyFlushed) {
  // Partial writes: 300 pending -> 120 pending -> 0. Must retry, not give up.
  int calls = 0;
  EXPECT_EQ(FlushWithRetry(ScriptedWriter({300, 120, 0}, &calls)),
            FlushStatus::kComplete);
  EXPECT_EQ(calls, 3);
}

TEST(FlushWithRetryTest, PropagatesWriterError) {
  // Negative pending signals NGX_CHAIN_ERROR.
  EXPECT_EQ(FlushWithRetry(ScriptedWriter({300, -1})), FlushStatus::kError);
}

TEST(FlushWithRetryTest, ReportsWouldBlockWhenNoProgress) {
  // Writer never makes progress (socket buffer wedged). The loop must bail
  // with kWouldBlock and must NOT spin forever.
  int calls = 0;
  EXPECT_EQ(FlushWithRetry(ScriptedWriter({200}, &calls), /*max_stalls=*/4),
            FlushStatus::kWouldBlock);
  // First call establishes the baseline, then max_stalls no-progress calls.
  EXPECT_LE(calls, 6);
  EXPECT_GE(calls, 4);
}

TEST(FlushWithRetryTest, StallCounterResetsOnProgress) {
  // One stall, then progress, then completion — the intermittent stall must
  // not cause an early kWouldBlock.
  EXPECT_EQ(
      FlushWithRetry(ScriptedWriter({200, 200, 100, 0}), /*max_stalls=*/2),
      FlushStatus::kComplete);
}

// --- Link-header injection containment (claimed "0d delimiter" issue) ---
//
// These assert that an attacker-controlled URL cannot break out of the
// <...>-delimited URI-Reference to inject extra Link parameters or a second
// link-value. SanitizeLinkUrl strips '<' and '>' (and CR/LF/NUL), so the
// injected ';'/',' stay inside the angle brackets and parse as part of one
// (malformed) URI per RFC 8288 — not as new parameters.

TEST(SanitizeLinkUrlTest, ContainsParameterInjectionViaAngleBracket) {
  // Attempt to close the URI early and append a forged "as=script" param.
  EXPECT_EQ(SanitizeLinkUrl("/x.css>; rel=preload; as=script"),
            "/x.css; rel=preload; as=script");  // '>' stripped; no breakout.
}

TEST(FormatLinkHeaderTest, ParameterInjectionStaysInsideUri) {
  std::vector<PreloadResource> resources = {
      {"/x.css>; rel=preload; as=script", "style", "preload", ""},
  };
  // The forged params remain inside <...>; exactly one '>' (the one we emit)
  // is present, so no real parameter/link breakout occurs.
  const std::string out = FormatLinkHeader(resources);
  EXPECT_EQ(out, "</x.css; rel=preload; as=script>; rel=preload; as=style");
  EXPECT_EQ(std::count(out.begin(), out.end(), '>'), 1);
}

TEST(FormatLinkHeaderTest, CommaInUrlDoesNotInjectSecondLink) {
  // A comma separates link-values in RFC 8288. With '<'/'>' stripped it stays
  // inside the URI-Reference, so it cannot smuggle in a second link target.
  std::vector<PreloadResource> resources = {
      {"/a.css, <https://evil.example/x.js", "style", "preload", ""},
  };
  const std::string out = FormatLinkHeader(resources);
  // Only one '<' and one '>' — a single link-value, no injected second link.
  EXPECT_EQ(std::count(out.begin(), out.end(), '<'), 1);
  EXPECT_EQ(std::count(out.begin(), out.end(), '>'), 1);
}

TEST(ShouldSendHintsOnMissTest, HtmlExtensionSendsRegardlessOfAccept) {
  EXPECT_TRUE(ShouldSendHintsOnMiss("text/html", ""));
  EXPECT_TRUE(ShouldSendHintsOnMiss("text/html", "*/*"));
}

TEST(ShouldSendHintsOnMissTest, OtherKnownTypesNeverSend) {
  EXPECT_FALSE(ShouldSendHintsOnMiss("text/css", "text/html"));
  EXPECT_FALSE(ShouldSendHintsOnMiss("image/png", "text/html,*/*"));
  EXPECT_FALSE(ShouldSendHintsOnMiss("application/json", "text/html"));
}

TEST(ShouldSendHintsOnMissTest, ExtensionlessKeysOnAcceptingHtml) {
  EXPECT_TRUE(ShouldSendHintsOnMiss(
      "", "text/html,application/xhtml+xml,application/xml;q=0.9,*/*;q=0.8"));
  EXPECT_TRUE(ShouldSendHintsOnMiss("", "Text/HTML"));
  EXPECT_FALSE(ShouldSendHintsOnMiss("", ""));
  EXPECT_FALSE(ShouldSendHintsOnMiss("", "*/*"));
  EXPECT_FALSE(ShouldSendHintsOnMiss("", "application/json"));
  EXPECT_FALSE(ShouldSendHintsOnMiss("", "text/htm"));
}

}  // namespace
}  // namespace pagespeed
