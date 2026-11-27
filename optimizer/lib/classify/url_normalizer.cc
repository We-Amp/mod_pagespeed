// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

#include "lib/classify/url_normalizer.h"

#include <algorithm>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "lib/base/string_util.h"
#include "lib/classify/hostname.h"

namespace pagespeed {
namespace {

// RFC 3986 unreserved characters: A-Za-z0-9 - . _ ~
bool IsUnreserved(char c) {
  return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
         (c >= '0' && c <= '9') || c == '-' || c == '.' || c == '_' || c == '~';
}

char UpperHexDigit(int v) {
  return static_cast<char>(v < 10 ? '0' + v : 'A' + v - 10);
}

// Normalize percent-encoding: decode unreserved chars, uppercase remaining hex.
std::string NormalizePercentEncoding(std::string_view input) {
  using net_instaweb::TransformPercentEncoded;
  return TransformPercentEncoded(
      input,
      [](std::string& out, char decoded, int hi, int lo) {
        if (IsUnreserved(decoded)) {
          out.push_back(decoded);
        } else {
          out.push_back('%');
          out.push_back(UpperHexDigit(hi));
          out.push_back(UpperHexDigit(lo));
        }
      },
      [](std::string& out, char c) { out.push_back(c); });
}

struct QueryParam {
  std::string_view key;
  std::string_view value;
  bool has_equals;
};

// Parse query string into key=value pairs.
std::vector<QueryParam> ParseQueryParams(std::string_view query) {
  std::vector<QueryParam> params;
  if (query.empty()) return params;

  size_t pos = 0;
  while (pos < query.size()) {
    size_t amp = query.find('&', pos);
    if (amp == std::string_view::npos) amp = query.size();

    std::string_view param = query.substr(pos, amp - pos);
    pos = amp + 1;

    if (param.empty()) continue;

    size_t eq = param.find('=');
    if (eq == std::string_view::npos) {
      params.push_back({param, {}, false});
    } else {
      params.push_back({param.substr(0, eq), param.substr(eq + 1), true});
    }
  }
  return params;
}

// Strip scheme+authority if present (e.g., "https://host/path" → "/path").
// nginx stores cache entries with path-only keys; the purge API may receive
// full URLs from users.  Stripping here ensures consistent keys everywhere.
// Only match "://" that appears before the first "/" to avoid false matches
// on path-only URLs with "://" in the query string (e.g., "/page?url=https://x").
std::string_view ExtractSchemeAndAuthority(std::string_view url) {
  auto first_slash = url.find('/');
  if (auto scheme_end = url.find("://");
      scheme_end != std::string_view::npos &&
      (first_slash == std::string_view::npos || scheme_end < first_slash)) {
    auto authority_end = url.find('/', scheme_end + 3);
    if (authority_end != std::string_view::npos) {
      return url.substr(authority_end);  // "/path?query#frag"
    }
    return "/";  // "https://host" with no path
  }
  return url;
}

// Strip the fragment and split the remainder into path and query.
// has_query is false when there is no '?' (the query view is then empty).
struct PathAndQuery {
  std::string_view path;
  std::string_view query;
  bool has_query;
};

PathAndQuery ExtractPathAndQuery(std::string_view url) {
  // Strip fragment (should not appear in cache URLs, but defense-in-depth).
  auto hash = url.find('#');
  if (hash != std::string_view::npos) {
    url = url.substr(0, hash);
  }

  auto qmark = url.find('?');
  if (qmark == std::string_view::npos) {
    return {url, {}, false};
  }
  return {url.substr(0, qmark), url.substr(qmark + 1), true};
}

}  // namespace

std::string ExtractLowercaseExtension(std::string_view url) {
  // Strip query and fragment.
  std::string_view path = url;
  auto q = path.find('?');
  if (q != std::string_view::npos) path = path.substr(0, q);
  auto h = path.find('#');
  if (h != std::string_view::npos) path = path.substr(0, h);

  // Find the last path segment (after final '/').
  auto slash = path.rfind('/');
  std::string_view segment =
      (slash != std::string_view::npos) ? path.substr(slash + 1) : path;

  // Find the last dot in the segment.
  auto dot = segment.rfind('.');
  if (dot == std::string_view::npos || dot == segment.size() - 1) {
    return {};
  }

  std::string ext;
  ext.reserve(segment.size() - dot);
  for (size_t i = dot; i < segment.size(); ++i) {
    ext.push_back(net_instaweb::LowerChar(segment[i]));
  }
  return ext;
}

std::vector<std::string> ExpandExtensionGroup(std::string_view group) {
  if (group == "images") {
    return {".jpg",  ".jpeg", ".png", ".gif", ".webp",
            ".avif", ".svg",  ".ico", ".bmp", ".tiff"};
  }
  if (group == "static") {
    return {".css", ".js", ".woff", ".woff2", ".ttf", ".eot"};
  }
  return {};
}

std::string NormalizeCacheUrl(std::string_view url,
                              const UrlNormalizationConfig& config) {
  // Strip scheme+authority (full URL → path-only), then split path and query.
  // ExtractPathAndQuery operates on the scheme-stripped view, not the original
  // url, and also strips any fragment.
  std::string_view effective_url = ExtractSchemeAndAuthority(url);
  PathAndQuery split = ExtractPathAndQuery(effective_url);
  if (!split.has_query) {
    // No query string — still normalize percent-encoding on path.
    return NormalizePercentEncoding(split.path);
  }

  std::string_view path = split.path;
  std::string_view query = split.query;

  // Normalize percent-encoding on path and query separately.
  std::string norm_path = NormalizePercentEncoding(path);
  std::string norm_query = NormalizePercentEncoding(query);

  // Extension check: strip query entirely for static assets.
  if (!config.strip_query_extensions.empty()) {
    std::string ext = ExtractLowercaseExtension(norm_path);
    if (!ext.empty() && (config.strip_query_extensions.count(ext) != 0u)) {
      return norm_path;
    }
  }

  // Parse, filter, sort, dedup query params.
  auto params = ParseQueryParams(norm_query);

  // Remove stripped params.
  if (!config.strip_query_params.empty()) {
    std::erase_if(params, [&config](const auto& p) {
      return config.strip_query_params.count(std::string(p.key));
    });
  }

  // Sort by key, then by value for same key.
  std::sort(params.begin(), params.end(), [](const auto& a, const auto& b) {
    if (a.key != b.key) return a.key < b.key;
    return a.value < b.value;
  });

  // Dedup identical key=value pairs.
  params.erase(std::unique(params.begin(), params.end(),
                           [](const auto& a, const auto& b) {
                             return a.key == b.key && a.value == b.value;
                           }),
               params.end());

  // Reassemble.
  if (params.empty()) {
    return norm_path;
  }

  std::string result = std::move(norm_path);
  result.push_back('?');
  for (size_t i = 0; i < params.size(); ++i) {
    if (i > 0) result.push_back('&');
    result.append(params[i].key);
    if (params[i].has_equals) {
      result.push_back('=');
      result.append(params[i].value);
    }
  }
  return result;
}

std::string NormalizeCacheHostname(std::string_view hostname,
                                   const UrlNormalizationConfig& config) {
  std::string normalized = NormalizeHostname(hostname);
  if (!config.host_aliases.empty()) {
    auto it = config.host_aliases.find(normalized);
    if (it != config.host_aliases.end()) {
      return it->second;
    }
  }
  return normalized;
}

}  // namespace pagespeed
