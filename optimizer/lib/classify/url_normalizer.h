// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

#ifndef PAGESPEED_LIB_CLASSIFY_URL_NORMALIZER_H_
#define PAGESPEED_LIB_CLASSIFY_URL_NORMALIZER_H_

#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace pagespeed {

struct UrlNormalizationConfig {
  // Extensions whose query strings are stripped entirely (lowercase, with dot).
  std::unordered_set<std::string> strip_query_extensions;

  // Query param keys to always remove (e.g., tracking params).
  std::unordered_set<std::string> strip_query_params;

  // Hostname → canonical hostname mapping (pre-normalized keys).
  std::unordered_map<std::string, std::string> host_aliases;
};

// Normalize a path+query URL for cache key composition.
// Steps: percent-encoding normalize → extension strip OR (param strip →
//        sort → dedup) → reassemble.
// No query string → returns URL as-is (zero-copy fast path).
std::string NormalizeCacheUrl(std::string_view url,
                              const UrlNormalizationConfig& config);

// NormalizeHostname() + host alias map lookup.
std::string NormalizeCacheHostname(std::string_view hostname,
                                   const UrlNormalizationConfig& config);

// Extract lowercase extension from the path portion of a URL. Returns "" if none.
// Only considers the last path segment (after the final '/').
std::string ExtractLowercaseExtension(std::string_view url);

// Expand group name to extension list.
// "images" → {".jpg", ".jpeg", ".png", ".gif", ".webp", ".avif", ".svg", ".ico", ".bmp", ".tiff"}
// "static" → {".css", ".js", ".woff", ".woff2", ".ttf", ".eot"}
// Unknown group → empty.
std::vector<std::string> ExpandExtensionGroup(std::string_view group);

}  // namespace pagespeed

#endif  // PAGESPEED_LIB_CLASSIFY_URL_NORMALIZER_H_
