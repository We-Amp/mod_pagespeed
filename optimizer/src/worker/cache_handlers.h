// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

// PageSpeed 2.0 - Cache Inspection & Mutation API Route Handlers
//
// Factory functions that create RouteHandler lambdas for cache-related
// endpoints: /v1/cache/alternates, /v1/cache/urls, /v1/cache/select,
// /v1/cache/content, /v1/cache/purge, /v1/cache/reprocess,
// /v1/cache/cooldowns.

#ifndef PAGESPEED_SRC_WORKER_CACHE_HANDLERS_H_
#define PAGESPEED_SRC_WORKER_CACHE_HANDLERS_H_

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <unordered_set>
#include <vector>

#include "lib/cache/cache.h"
#include "lib/classify/content_type.h"
#include "lib/classify/url_normalizer.h"
#include "src/worker/http_server.h"
#include "src/worker/url_registry.h"

namespace pagespeed {

// Count unique alternate IDs, ignoring stale append-only chain duplicates.
//
// Shared by the /v1/cache/urls listing handler and the worker's
// notification-time count refresh, so the cached alternate_count matches
// exactly what the serve path reports (unique alt.id over the whole chain,
// sentinels included).  Keep these two callers using this single function;
// if the counting semantics ever change, both must change together.
inline size_t CountUniqueAlternates(
    const std::vector<cyclone::AlternateInfo>& alts) {
  std::unordered_set<uint8_t> seen;
  for (const auto& alt : alts) {
    seen.insert(static_cast<uint8_t>(alt.id));
  }
  return seen.size();
}

// Runtime context for cache API handlers.
struct CacheApiContext {
  // Cache for direct read/list/remove operations.
  PageSpeedCache* cache;

  // URL registry for paginated URL listing.
  UrlRegistry& url_registry;

  // Invalidate all cached variants for a URL in one site's keyspace (empty
  // site: the unsited one).  Returns count deleted.
  std::function<int(const std::string& url, const std::string& hostname,
                    const std::string& scheme, const std::string& site)>
      invalidate_url;

  // Clear dedup set and cooldown for a URL in one site's keyspace so the next
  // notification triggers re-processing, without removing cached content.
  std::function<void(const std::string& url, const std::string& hostname,
                     const std::string& scheme, const std::string& site)>
      clear_dedup;

  // Enqueue a reprocessing notification for a URL in one site's keyspace.
  // The notification vouches for no host.
  std::function<void(const std::string& url, const std::string& hostname,
                     const std::string& scheme, const std::string& site,
                     ContentType content_type)>
      enqueue_reprocess;

  // Reset the entire cache (stop, delete volume, recreate).
  // Returns empty string on success, error message on failure.
  std::function<std::string()> reset_cache;

  // URL normalization config (host aliases, query stripping, etc.).
  // When non-null, NormalizeCacheHostname() is used instead of plain
  // NormalizeHostname(), ensuring alias-resolved hostnames match what
  // nginx recorded via NormalizeCacheHostname().
  const UrlNormalizationConfig* url_norm_config = nullptr;

  // Optional cooldown query callbacks (nullptr = cooldown tracking unavailable).
  // Added with defaults so existing designated-initializer sites compile
  // unchanged.
  struct CooldownEntry {
    std::string url;
    std::string hostname;
    std::string scheme;
    std::string reason;
    int remaining_seconds;
    int duration_seconds;
  };

  // Query cooldown for a specific URL.  Returns (reason, remaining, duration)
  // or nullopt if no active cooldown.
  std::function<std::optional<CooldownEntry>(
      const std::string& url, const std::string& hostname,
      const std::string& scheme, const std::string& site)>
      get_cooldown = nullptr;

  // List all active cooldowns.
  std::function<std::vector<CooldownEntry>()> list_cooldowns = nullptr;

  // True when the known sites may not name every site with entries in the
  // volume: sites were seen past the cap since start, or the known-sites
  // file could not be read at start.  nullptr reads as false.
  std::function<bool()> known_sites_incomplete = nullptr;
};

// Register all cache routes on the server.
//   GET  /v1/cache/alternates  - List alternates for a URL
//   GET  /v1/cache/urls        - Paginated URL listing
//   GET  /v1/cache/select      - Selector scoring breakdown
//   GET  /v1/cache/content     - Raw alternate content
//   POST /v1/cache/purge       - Purge URL or entire cache
//   POST /v1/cache/reprocess   - Purge non-original + requeue
//   GET  /v1/cache/cooldowns   - List active HTML processing cooldowns
void RegisterCacheRoutes(HttpServer& server, CacheApiContext& ctx);

}  // namespace pagespeed

#endif  // PAGESPEED_SRC_WORKER_CACHE_HANDLERS_H_
