// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

// PageSpeed 2.0 - HTTP 103 Early Hints Utilities
//
// Pure C++ utilities for Early Hints functionality, testable without
// nginx headers.

#ifndef PAGESPEED_SRC_NGINX_EARLY_HINTS_UTIL_H_
#define PAGESPEED_SRC_NGINX_EARLY_HINTS_UTIL_H_

#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace pagespeed {

// Outcome of attempting to fully flush a pending byte buffer through a writer
// that may accept only part of it per call (e.g. nginx's c->send_chain under
// socket back-pressure).
enum class FlushStatus : std::uint8_t {
  kComplete,    // all bytes flushed
  kWouldBlock,  // writer stopped making progress before finishing
  kError,       // writer reported a hard error
};

// Drives a partial-write-aware flush loop.
//
// `send_remaining` attempts to send the not-yet-sent bytes and returns the
// number of bytes STILL PENDING afterwards: 0 means fully flushed, a positive
// value means a partial write occurred (retry the remainder), and a negative
// value signals a hard error (e.g. NGX_CHAIN_ERROR). The loop retries while the
// pending count keeps shrinking; after `max_stalls` consecutive calls that make
// no progress it returns kWouldBlock instead of spinning forever on a wedged
// socket. It is guaranteed to terminate: every call either makes progress,
// errors, completes, or counts toward the stall budget.
//
// This exists because a 103 Early Hints response is written as raw bytes before
// the main response; a silently-dropped partial write truncates the 103 and
// corrupts the stream once the 200 headers follow.
FlushStatus FlushWithRetry(const std::function<long()>& send_remaining,
                           int max_stalls = 16);

// Whether a cache MISS should look up and send Early Hints for a request.
// `url_mime` is the Content-Type implied by the URL extension ("" when the URL
// has no recognized extension, e.g. a clean URL such as "/about"); `accept` is
// the request's Accept header.  The hint set is only ever stored for HTML, so:
//   - a URL that maps to text/html: yes;
//   - a URL that maps to any other type (css, js, images, ...): no;
//   - a URL with no recognized extension: yes when the client asks for HTML
//     (its Accept header names text/html, as every browser navigation does).
bool ShouldSendHintsOnMiss(std::string_view url_mime, std::string_view accept);

// A resource to preload via Link header in a 103 Early Hints response.
struct PreloadResource {
  std::string url;
  std::string as_type;          // "style", "script", "image", etc.
  std::string rel = "preload";  // "preload" or "preconnect"
  std::string extra;            // Extra attributes, e.g. "fetchpriority=high"
};

// Sanitize a URL for safe inclusion in an HTTP Link header.
// Strips \r, \n, \0, <, > to prevent header injection and tag breakout.
// Returns empty string if the URL contains percent-encoded dangerous
// characters (%0D, %0A, %00) — these indicate an injection attempt.
std::string SanitizeLinkUrl(std::string_view url);

// Construct a Link header value for preloading.
// Example: </style.css>; rel=preload; as=style
// URLs are sanitized to prevent header injection.
std::string FormatLinkHeader(const std::vector<PreloadResource>& resources);

// Parse a single Early Hints line (from the sentinel) into a
// PreloadResource.  Recognizes prefixes:
//   "image:"           → rel=preload, as=image, fetchpriority=high
//   "font:"            → rel=preload, as=font, crossorigin
//   "preconnect:"      → rel=preconnect, no as (no-cors connection pool)
//   "preconnect-cors:" → rel=preconnect, crossorigin (CORS connection pool;
//                        matches the crossorigin on the injected HTML
//                        preconnect so both paths warm the same pool)
//   (none)             → rel=preload, as=style
//
// Compatibility: an unknown-prefix line falls back to a bare stylesheet
// preload, so a NEW sentinel read by an OLD consumer produces a broken Link
// header.  When adding a prefix, update every sentinel consumer (this parser
// AND the .NET middleware's BuildLinkHeader) in the same release, and deploy
// consumers together with — or before — the emitting worker.  Rolling back
// across a sentinel-format change requires a cache purge: sentinels persist
// in the cache.
PreloadResource ParseHintLine(std::string_view line);

// Extract stylesheet URLs from HTML content by scanning for
// <link rel="stylesheet" href="..."> tags.
// This is a lightweight scan, not a full HTML parser.
std::vector<PreloadResource> ExtractStylesheetPreloads(std::string_view html);

}  // namespace pagespeed

#endif  // PAGESPEED_SRC_NGINX_EARLY_HINTS_UTIL_H_
