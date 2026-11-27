// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

// PageSpeed 2.0 - Analysis Resource Map for Browser Analysis
//
// Builds the URL -> content map the offline analysis browser serves via
// Fetch interception (page analysis + script coverage).  Only same-origin
// scripts already present in the cache are mapped; everything else stays
// blocked, so a script the analyzer could not observe never earns coverage
// evidence (kNoCoverageData -> keep synchronous).

#ifndef PAGESPEED_SRC_WORKER_ANALYSIS_RESOURCE_MAP_H_
#define PAGESPEED_SRC_WORKER_ANALYSIS_RESOURCE_MAP_H_

#include <cstddef>
#include <functional>
#include <optional>
#include <string>
#include <string_view>

#include "absl/container/flat_hash_map.h"
#include "lib/classify/url_normalizer.h"

namespace pagespeed {

struct AnalysisResourceMapStats {
  size_t scripts_found = 0;
  size_t scripts_cached = 0;
  // Same-origin, mappable-class, cache-cold — the ONLY miss class a
  // re-analysis can heal (the script lands in cache once fetched through the
  // proxy), so this is what the shortened profile TTL keys on.  Unique
  // resolved URLs are counted once.
  size_t scripts_uncached_same_origin = 0;
  // Same-origin, cached, but with an empty body (stubs, feature-flag
  // placeholders).  Nothing to execute -> nothing to map, and a re-analysis
  // re-reads the same zero bytes, so like the over-cap class this is
  // excluded from the TTL-heal trigger — counting it as uncached would pin
  // the template on hourly re-analysis forever.  A stub that later gains
  // content still heals, at the configured TTL expiry instead of the clamp.
  size_t scripts_empty_same_origin = 0;
  size_t scripts_cross_origin = 0;
  // Cached but over the per-entry or total cap.  Re-reading the same bytes
  // can never heal these, so they are excluded from the TTL-heal trigger.
  size_t scripts_oversize = 0;
  size_t bytes_mapped = 0;
};

// Cache lookup, same shape as the CSS inlining lambda: receives a
// cache-keyable URL (path-only for same-origin resources — PageSpeedCache
// keys are scheme://hostname + path, composed by the cache itself).  The
// builder passes the NormalizeCacheUrl form of the path+query, matching how
// the worker keys stored resources (a query-versioned src would otherwise
// permanently miss).
using ResourceLookupFn =
    std::function<std::optional<std::string>(std::string_view url)>;

// Per-entry / total caps: each mapped body crosses the CDP pipe
// base64-encoded in Fetch.fulfillRequest, and one map lives across all four
// analysis sessions (3 viewports + script pass).  Oversize entries are left
// unmapped (blocked -> keep-sync), never truncated.
inline constexpr size_t kMaxMappedScriptBytes = 1UL * 1024 * 1024;  // 1 MiB
inline constexpr size_t kMaxMappedTotalBytes = 8UL * 1024 * 1024;   // 8 MiB

// Scans `html` for <script src> elements and maps each SAME-ORIGIN script's
// cached bytes under its absolute URL — the URL Chrome requests once the
// document carries a usable base (the author <base href> when present, else
// the injected one; see InjectBaseHrefIfAbsent).  Map keys are
// request-normalized: host lowercased, fragment stripped.  page_url may be
// cache-normalized (path-only); the absolute form is composed from
// page_hostname + scheme.  Cache lookups go through NormalizeCacheUrl with
// `url_norm` (the worker's store-key normalization).  Cross-origin scripts
// are never mapped (the analysis browser has no egress; they classify
// kNoCoverageData downstream).
// Return type must stay identical to PageAnalyzer::ResourceMap.
absl::flat_hash_map<std::string, std::string> BuildAnalysisResourceMap(
    std::string_view html, std::string_view page_url,
    std::string_view page_hostname, std::string_view scheme,
    const ResourceLookupFn& lookup, const UrlNormalizationConfig& url_norm,
    AnalysisResourceMapStats* stats = nullptr);

// Injects <base href="{absolute page url}"> at the start of <head> unless the
// document carries an author <base href> (or the scan fails — no injection is
// the conservative fallback).  The analysis document is loaded into
// about:blank via Page.setDocumentContent, so without a base element relative
// subresource URLs cannot resolve and DOM .src reads stay relative — breaking
// the analysis-side joins (resource map + profiler coverage).  Production is
// a separate matter: the deferral suffix-matcher only ever matches
// absolute-authored srcs, so verdicts earned by relative-authored scripts are
// analysis-only for now.
std::string InjectBaseHrefIfAbsent(std::string_view html,
                                   std::string_view page_url,
                                   std::string_view page_hostname,
                                   std::string_view scheme);

}  // namespace pagespeed

#endif  // PAGESPEED_SRC_WORKER_ANALYSIS_RESOURCE_MAP_H_
