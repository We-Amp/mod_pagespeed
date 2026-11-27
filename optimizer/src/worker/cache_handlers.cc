// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

// PageSpeed 2.0 - Cache API Route Handlers Implementation

#include "src/worker/cache_handlers.h"

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>

#include "absl/strings/numbers.h"
#include "absl/strings/str_cat.h"
#include "lib/classify/alternate_id.h"
#include "lib/classify/alternate_metadata.h"
#include "lib/classify/capability_mask.h"
#include "lib/classify/content_type.h"
#include "lib/classify/pagespeed_selector.h"
#include "nlohmann/json.hpp"
#include "src/proto/worker_ipc.h"
#include "src/worker/json_dump.h"

namespace pagespeed {

using json = nlohmann::json;

// Parse an unsigned query parameter via std::stoul, preserving stoul's exact
// acceptance/overflow semantics (leading whitespace/+/-, base-10, throws
// std::invalid_argument / std::out_of_range on bad input).  Returns the parsed
// value, or std::nullopt when stoul throws — callers own the error response and
// any range check so each call site keeps its distinct message and behavior.
//
// NOTE: This deliberately does NOT replace the absl::SimpleAtoi call sites
// (offset/limit), which have different acceptance semantics.
static std::optional<unsigned long> ParseQueryParamStoul(std::string_view s) {
  try {
    return std::stoul(std::string(s));
  } catch (...) {
    return std::nullopt;
  }
}

// Normalize a hostname using the full cache normalization pipeline
// (NormalizeCacheHostname, which resolves host aliases) when a config
// is available.  Falls back to NormalizeCacheHostname with an empty
// config (equivalent to NormalizeHostname) when no config is set.
static std::string NormalizeHostnameForApi(std::string_view hostname,
                                           const UrlNormalizationConfig* cfg) {
  if (cfg != nullptr) {
    return NormalizeCacheHostname(hostname, *cfg);
  }
  static const UrlNormalizationConfig kEmpty;
  return NormalizeCacheHostname(hostname, kEmpty);
}

// Helper: describe a CapabilityMask as JSON.
static json MaskToJson(uint32_t mask) {
  auto decoded = CapabilityMask::Decode(mask);
  json j;
  j["raw"] = mask;

  const char* fmt = "original";
  switch (decoded.image_format()) {
    case CapabilityMask::ImageFormat::kWebP:
      fmt = "webp";
      break;
    case CapabilityMask::ImageFormat::kAvif:
      fmt = "avif";
      break;
    case CapabilityMask::ImageFormat::kSvg:
      fmt = "svg";
      break;
    default:
      break;
  }
  j["format"] = fmt;

  const char* vp = "mobile";
  switch (decoded.viewport()) {
    case CapabilityMask::Viewport::kTablet:
      vp = "tablet";
      break;
    case CapabilityMask::Viewport::kDesktop:
      vp = "desktop";
      break;
    default:
      break;
  }
  j["viewport"] = vp;

  j["density"] =
      decoded.pixel_density() == CapabilityMask::PixelDensity::k2xPlus ? "2x+"
                                                                       : "1x";
  j["save_data"] = decoded.save_data() == CapabilityMask::SaveData::kOn;

  const char* enc = "identity";
  switch (decoded.transfer_encoding()) {
    case CapabilityMask::TransferEncoding::kGzip:
      enc = "gzip";
      break;
    case CapabilityMask::TransferEncoding::kBrotli:
      enc = "brotli";
      break;
    default:
      break;
  }
  j["encoding"] = enc;

  return j;
}

// Helper: describe a content type as a string.
static const char* ContentTypeString(ContentType ct) {
  switch (ct) {
    case ContentType::kHtml:
      return "html";
    case ContentType::kCss:
      return "css";
    case ContentType::kJs:
      return "js";
    case ContentType::kImage:
      return "image";
    default:
      return "other";
  }
}

// Helper: describe a sentinel ID as a string.  Names come from the sentinel
// registry (lib/classify/alternate_id.h) rather than a second switch here —
// a diagnostic that disagrees with the registry about what an id IS is worse
// than no diagnostic, and a registry with two copies grows a disagreement.
static std::string SentinelNameFor(uint8_t id) {
  return std::string(pagespeed::SentinelName(static_cast<AlternateId>(id)));
}

// The optional site of a request: absent, "" (the unsited keyspace), or a
// site id this instance has stored work for.  Returns the error response for
// a malformed (400) or unknown (403) site, or nothing when `site` is usable.
static std::optional<HttpResponse> CheckSiteParam(const CacheApiContext& ctx,
                                                  std::string_view site) {
  if (site.empty()) return std::nullopt;
  if (!IsSiteId(site)) {
    return HttpResponse::Error(
        ApiErrorCode::kBadRequest,
        "Invalid 'site': must be 64 lowercase hex characters");
  }
  if (!ctx.url_registry.HasSite(site)) {
    return HttpResponse::Error(ApiErrorCode::kForbidden,
                               "Site was never served through this instance.");
  }
  return std::nullopt;
}

// The same for a JSON body.  `*present` says whether the body named a site.
static std::optional<HttpResponse> ReadSiteField(const CacheApiContext& ctx,
                                                 const json& body,
                                                 std::string* site,
                                                 bool* present) {
  site->clear();
  *present = body.contains("site");
  if (!*present) return std::nullopt;
  if (!body["site"].is_string()) {
    return HttpResponse::Error(ApiErrorCode::kBadRequest,
                               "Invalid 'site': must be a string");
  }
  *site = body["site"].get<std::string>();
  return CheckSiteParam(ctx, *site);
}

// ---------------------------------------------------------------------------
// GET /v1/cache/alternates?url=...&hostname=...
// ---------------------------------------------------------------------------

static HttpResponse HandleListAlternates(CacheApiContext& ctx,
                                         const HttpRequest& request) {
  auto url = request.QueryParam("url");
  auto hostname = request.QueryParam("hostname");
  auto scheme_param = request.QueryParam("scheme");
  std::string scheme =
      scheme_param.empty() ? "https" : std::string(scheme_param);
  if (!scheme_param.empty() && scheme != "http" && scheme != "https") {
    return HttpResponse::Error(ApiErrorCode::kBadRequest,
                               "Invalid 'scheme': must be 'http' or 'https'");
  }
  if (url.empty()) {
    return HttpResponse::Error(ApiErrorCode::kBadRequest,
                               "Missing 'url' query parameter");
  }

  auto site = request.QueryParam("site");
  if (auto err = CheckSiteParam(ctx, site)) return std::move(*err);

  auto result = ctx.cache->ListAlternates(url, hostname, scheme, site);
  if (!result) {
    return HttpResponse::Error(ApiErrorCode::kNotFound,
                               "No alternates found for URL");
  }

  // Deduplicate by alternate_id (keep first occurrence — newest in chain).
  // Cyclone prepends new alternates, so the chain traversal returns newest
  // entries first.  When a variant is re-written, the old entry remains in
  // the chain but appears later in traversal order.
  std::unordered_map<uint8_t, size_t> newest_index;
  for (size_t i = 0; i < result->size(); ++i) {
    auto id = static_cast<uint8_t>((*result)[i].id);
    if (newest_index.find(id) == newest_index.end()) {
      newest_index[id] = i;
    }
  }

  json alternates = json::array();
  for (size_t i = 0; i < result->size(); ++i) {
    auto id = static_cast<uint8_t>((*result)[i].id);
    if (newest_index[id] != i) continue;  // Skip stale duplicate.
    const auto& alt = (*result)[i];
    json j;
    auto alt_byte = static_cast<uint8_t>(alt.id);
    j["alternate_id"] = alt_byte;
    j["size"] = alt.content_length;
    j["hit_count"] = alt.hit_count;

    bool sentinel = IsSentinel(alt_byte);
    j["is_sentinel"] = sentinel;

    // last_access as epoch milliseconds.
    j["last_access"] = std::chrono::duration_cast<std::chrono::milliseconds>(
                           alt.last_access.time_since_epoch())
                           .count();

    if (sentinel) {
      j["sentinel_name"] = SentinelNameFor(alt_byte);
    } else {
      // Try to deserialize metadata from the Cyclone document header first
      // (populated by set_header() since the write-path change).  This avoids
      // N+1 chain traversals — list_alternates_sync already traversed the
      // chain once and returned the header data for each alternate.
      std::optional<AlternateMetadata> meta;
      if (!alt.header.empty()) {
        meta = AlternateMetadata::Deserialize(alt.header);
        if (meta) {
          // Report content size excluding the metadata prefix.
          size_t prefix_size = meta->WireSize();
          if (alt.content_length > prefix_size) {
            j["size"] = alt.content_length - prefix_size;
          } else {
            j["size"] = 0;  // Metadata-only entry (no content payload).
          }
        }
      }
      // Fall back to reading the alternate individually for entries written
      // before the set_header() change (header will be empty).
      if (!meta) {
        auto read_result = ctx.cache->ReadAlternate(
            url, hostname, scheme, static_cast<AlternateId>(alt_byte), site);
        if (read_result) {
          j["size"] = read_result->content_length();
          auto raw = read_result->handle.content();
          meta = AlternateMetadata::Deserialize(raw);
        }
      }

      if (meta) {
        json mask = MaskToJson(meta->full_mask);
        j["mask"] = mask;
        j["format"] = mask["format"];
        // For worker-processed variants with "original" format, show the
        // actual codec (e.g., "jpeg", "png") instead of generic "original".
        if (j["format"] == "original" &&
            (meta->flags & AlternateMetadata::kFlagWorkerProcessed) != 0 &&
            !meta->origin_content_type.empty()) {
          auto ct = meta->origin_content_type;
          if (ct.find("jpeg") != std::string::npos ||
              ct.find("jpg") != std::string::npos) {
            j["format"] = "jpeg";
          } else if (ct.find("png") != std::string::npos) {
            j["format"] = "png";
          } else if (ct.find("gif") != std::string::npos) {
            j["format"] = "gif";
          }
        }
        j["viewport"] = mask["viewport"];
        j["density"] = mask["density"];
        j["save_data"] = mask["save_data"];
        j["encoding"] = mask["encoding"];
        j["content_type"] = ContentTypeString(meta->content_type);
        j["origin_content_type"] = meta->origin_content_type;
        j["flags"] = meta->flags;
        j["needs_revalidation"] =
            (meta->flags & AlternateMetadata::kFlagNeedsRevalidation) != 0;
        j["cache_inserted_at"] = meta->cache_inserted_at;
        j["origin_max_age"] = meta->origin_max_age;
        j["origin_s_maxage"] = meta->origin_s_maxage;
        j["origin_cc_flags"] = meta->origin_cc_flags;
        j["version"] = meta->version;
        if (const auto score = meta->QualityScoreX100(); score.has_value()) {
          j["ssimulacra2_score"] = static_cast<double>(*score) / 100.0;
        }
        if (meta->content_class < AlternateMetadata::kContentClassUnknown) {
          static constexpr const char* kClassNames[] = {
              "photo", "screenshot", "illustration", "noisy"};
          j["content_class"] = kClassNames[meta->content_class];
        }
        if (meta->origin_content_length > 0) {
          j["original_size"] = meta->origin_content_length;
        }
      } else {
        // Fallback: derive from the low 8-bit AlternateId.
        json mask = MaskToJson(alt_byte);
        j["mask"] = mask;
        j["format"] = mask["format"];
        j["viewport"] = mask["viewport"];
        j["density"] = mask["density"];
        j["save_data"] = mask["save_data"];
        j["encoding"] = mask["encoding"];
        j["content_type"] = "";
        j["origin_content_type"] = "";
        j["flags"] = 0;
        j["needs_revalidation"] = false;
      }
    }

    alternates.push_back(std::move(j));
  }

  json resp;
  resp["url"] = url;
  resp["hostname"] = hostname;
  resp["scheme"] = scheme;

  // Add the composed cache key (matches ComposeKey output).
  std::string normalized_host =
      NormalizeHostnameForApi(hostname, ctx.url_norm_config);
  std::string cache_key_str = absl::StrCat(scheme, "://", normalized_host, url);
  if (!site.empty()) {
    cache_key_str = absl::StrCat("st1:", site, "@", cache_key_str);
    resp["site"] = site;
  }
  resp["cache_key"] =
      absl::StrCat(PageSpeedCache::kKeyNamespace, cache_key_str);

  resp["alternates"] = std::move(alternates);
  resp["count"] = resp["alternates"].size();
  resp["chain_length"] = result->size();  // Raw chain entries (including stale)

  // Add cooldown info if callback is available and URL is in cooldown.
  if (ctx.get_cooldown) {
    auto cd = ctx.get_cooldown(std::string(url), std::string(hostname), scheme,
                               std::string(site));
    if (cd) {
      resp["cooldown"] = {{"reason", cd->reason},
                          {"remaining_seconds", cd->remaining_seconds},
                          {"duration_seconds", cd->duration_seconds}};
    }
  }

  return HttpResponse().Json(DumpJson(resp));
}

// ---------------------------------------------------------------------------
// GET /v1/cache/urls?offset=0&limit=100
// ---------------------------------------------------------------------------

static HttpResponse HandleListUrls(CacheApiContext& ctx,
                                   const HttpRequest& request) {
  auto offset_str = request.QueryParam("offset");
  auto limit_str = request.QueryParam("limit");
  auto hostname = request.QueryParam("hostname");
  auto site = request.QueryParam("site");
  if (auto err = CheckSiteParam(ctx, site)) return std::move(*err);

  size_t offset = 0;
  size_t limit = 100;
  if (!offset_str.empty()) {
    if (!absl::SimpleAtoi(offset_str, &offset)) {
      return HttpResponse::Error(ApiErrorCode::kBadRequest,
                                 "Invalid 'offset' parameter");
    }
  }
  if (!limit_str.empty()) {
    if (!absl::SimpleAtoi(limit_str, &limit)) {
      return HttpResponse::Error(ApiErrorCode::kBadRequest,
                                 "Invalid 'limit' parameter");
    }
  }
  if (limit > 1000) limit = 1000;
  if (limit == 0) limit = 100;

  auto page = ctx.url_registry.List(offset, limit, hostname, site);

  json urls = json::array();
  for (const auto& entry : page.entries) {
    json j = {{"url", entry.url},
              {"hostname", entry.hostname},
              {"scheme", entry.scheme},
              {"site", entry.site}};

    // Alternate count is maintained in the registry by the worker on the
    // notification thread, so listing never touches the cache here.  This is
    // the fix for the /console/urls timeout: previously this loop did one
    // ListAlternates() cache chain traversal PER URL on the event-loop
    // thread (~0.3-0.7s each), which also blocked /v1/health.
    j["alternate_count"] = entry.alternate_count;

    // Add the composed cache key (matches ComposeKey output).
    std::string normalized_host =
        NormalizeHostnameForApi(entry.hostname, ctx.url_norm_config);
    std::string cache_key_str =
        absl::StrCat(entry.scheme, "://", normalized_host, entry.url);
    if (!entry.site.empty()) {
      cache_key_str = absl::StrCat("st1:", entry.site, "@", cache_key_str);
    }
    cache_key_str = absl::StrCat(PageSpeedCache::kKeyNamespace, cache_key_str);
    j["cache_key"] = cache_key_str;

    urls.push_back(std::move(j));
  }

  json resp;
  resp["urls"] = std::move(urls);
  resp["offset"] = offset;
  resp["limit"] = limit;
  resp["next_offset"] = page.next_offset;
  resp["has_more"] = page.has_more;
  resp["total"] = page.total;

  return HttpResponse().Json(DumpJson(resp));
}

// ---------------------------------------------------------------------------
// GET /v1/cache/select?url=...&hostname=...&mask=...[&site=...]
// ---------------------------------------------------------------------------

static HttpResponse HandleSelect(CacheApiContext& ctx,
                                 const HttpRequest& request) {
  auto url = request.QueryParam("url");
  auto hostname = request.QueryParam("hostname");
  auto scheme_param = request.QueryParam("scheme");
  std::string scheme =
      scheme_param.empty() ? "https" : std::string(scheme_param);
  if (!scheme_param.empty() && scheme != "http" && scheme != "https") {
    return HttpResponse::Error(ApiErrorCode::kBadRequest,
                               "Invalid 'scheme': must be 'http' or 'https'");
  }
  auto mask_str = request.QueryParam("mask");

  if (url.empty()) {
    return HttpResponse::Error(ApiErrorCode::kBadRequest,
                               "Missing 'url' query parameter");
  }
  if (mask_str.empty()) {
    return HttpResponse::Error(ApiErrorCode::kBadRequest,
                               "Missing 'mask' query parameter");
  }

  auto mask_val = ParseQueryParamStoul(mask_str);
  if (!mask_val) {
    return HttpResponse::Error(ApiErrorCode::kBadRequest,
                               "Invalid 'mask' parameter");
  }
  uint32_t client_mask = static_cast<uint32_t>(*mask_val);

  auto site = request.QueryParam("site");
  if (auto err = CheckSiteParam(ctx, site)) return std::move(*err);

  auto result = ctx.cache->ListAlternates(url, hostname, scheme, site);
  if (!result) {
    return HttpResponse::Error(ApiErrorCode::kNotFound,
                               "No alternates found for URL");
  }

  json scores = json::array();
  int best_score = 0;
  int best_idx = -1;

  for (size_t i = 0; i < result->size(); ++i) {
    const auto& alt = (*result)[i];
    auto alt_byte = static_cast<uint8_t>(alt.id);
    bool sentinel = IsSentinel(alt_byte);

    json entry;
    entry["alternate_id"] = alt_byte;
    entry["is_sentinel"] = sentinel;
    entry["size"] = alt.content_length;

    if (sentinel) {
      entry["score"] = 0;
      entry["sentinel_name"] = SentinelNameFor(alt_byte);
    } else {
      // Read the alternate to extract metadata (stored as content prefix).
      auto read_result =
          ctx.cache->ReadAlternate(url, hostname, scheme, alt_byte, site);
      if (read_result) {
        entry["size"] = read_result->content_length();
        uint32_t stored_mask = read_result->metadata.full_mask;
        entry["mask"] = MaskToJson(stored_mask);
        entry["content_type"] =
            ContentTypeString(read_result->metadata.content_type);
        int score = ScoreAlternate(client_mask, stored_mask);
        entry["score"] = score;
        if (score > best_score) {
          best_score = score;
          best_idx = static_cast<int>(i);
        }
      } else {
        entry["score"] = 0;
        entry["error"] = "metadata_read_failed";
      }
    }

    scores.push_back(std::move(entry));
  }

  json resp;
  resp["url"] = url;
  resp["hostname"] = hostname;
  resp["scheme"] = scheme;
  resp["client_mask"] = MaskToJson(client_mask);
  resp["alternates"] = std::move(scores);
  resp["best_index"] = best_idx;
  resp["best_score"] = best_score;

  return HttpResponse().Json(DumpJson(resp));
}

// ---------------------------------------------------------------------------
// GET /v1/cache/content?url=...&hostname=...&alternate_id=...
// ---------------------------------------------------------------------------

static HttpResponse HandleContent(CacheApiContext& ctx,
                                  const HttpRequest& request) {
  auto url = request.QueryParam("url");
  auto hostname = request.QueryParam("hostname");
  auto scheme_param = request.QueryParam("scheme");
  std::string scheme =
      scheme_param.empty() ? "https" : std::string(scheme_param);
  if (!scheme_param.empty() && scheme != "http" && scheme != "https") {
    return HttpResponse::Error(ApiErrorCode::kBadRequest,
                               "Invalid 'scheme': must be 'http' or 'https'");
  }
  auto id_str = request.QueryParam("alternate_id");

  if (url.empty()) {
    return HttpResponse::Error(ApiErrorCode::kBadRequest,
                               "Missing 'url' query parameter");
  }
  if (id_str.empty()) {
    return HttpResponse::Error(ApiErrorCode::kBadRequest,
                               "Missing 'alternate_id' query parameter");
  }

  auto id_val = ParseQueryParamStoul(id_str);
  if (!id_val) {
    return HttpResponse::Error(ApiErrorCode::kBadRequest,
                               "Invalid 'alternate_id' parameter");
  }
  if (*id_val > 255) {
    return HttpResponse::Error(ApiErrorCode::kBadRequest,
                               "alternate_id must be 0-255");
  }
  uint8_t alt_id = static_cast<uint8_t>(*id_val);

  auto site = request.QueryParam("site");
  if (auto err = CheckSiteParam(ctx, site)) return std::move(*err);

  auto result = ctx.cache->ReadAlternate(url, hostname, scheme, alt_id, site);
  if (!result) {
    return HttpResponse::Error(ApiErrorCode::kNotFound, "Alternate not found");
  }

  auto content = result->content();
  std::string body(reinterpret_cast<const char*>(content.data()),
                   content.size());

  // Determine content type. For transcoded images, use the variant's
  // actual format. For SVG variants (format bits = 3), use image/svg+xml.
  std::string content_type = "application/octet-stream";
  bool is_svg = false;
  if (result->metadata.content_type == ContentType::kImage) {
    uint8_t format_bits =
        static_cast<uint8_t>(result->metadata.full_mask) & 0x03;
    if (format_bits == 3) {
      content_type = "image/svg+xml";
      is_svg = true;
    } else if (format_bits == 1) {
      content_type = "image/webp";
    } else if (format_bits == 2) {
      content_type = "image/avif";
    } else if (!result->metadata.origin_content_type.empty()) {
      content_type = result->metadata.origin_content_type;
    }
  } else if (!result->metadata.origin_content_type.empty()) {
    content_type = result->metadata.origin_content_type;
  }

  auto resp = HttpResponse()
                  .ContentType(content_type)
                  .SetHeader("X-Content-Type-Options", "nosniff")
                  .SetHeader("Content-Disposition", "attachment")
                  .SetHeader("X-Alternate-Id", std::to_string(alt_id))
                  .SetHeader("X-Capability-Mask",
                             std::to_string(result->metadata.full_mask));

  // SVG security headers: restrict execution context to mitigate
  // XSS via inline SVG scripts.
  if (is_svg) {
    resp.SetHeader("Content-Security-Policy",
                   "default-src 'none'; style-src 'unsafe-inline'");
  }

  return resp.Body(std::move(body));
}

// Extract hostname from a URL string (scheme://host[:port]/path).
// Strips the port if present.  Returns empty string if no hostname can
// be extracted.
static std::string ExtractHostnameFromUrl(std::string_view url) {
  auto scheme_end = url.find("://");
  if (scheme_end == std::string_view::npos) return {};
  auto authority_start = scheme_end + 3;
  auto authority_end = url.find('/', authority_start);
  std::string hostname;
  if (authority_end != std::string_view::npos) {
    hostname = std::string(
        url.substr(authority_start, authority_end - authority_start));
  } else {
    hostname = std::string(url.substr(authority_start));
  }
  // Strip port if present (e.g., "host:8080" -> "host")
  auto colon = hostname.find(':');
  if (colon != std::string::npos) {
    hostname = hostname.substr(0, colon);
  }
  return hostname;
}

// ---------------------------------------------------------------------------
// POST /v1/cache/purge
// ---------------------------------------------------------------------------

static HttpResponse HandlePurge(CacheApiContext& ctx,
                                const HttpRequest& request) {
  if (JsonNestingTooDeep(request.body)) {
    return HttpResponse::Error(ApiErrorCode::kBadRequest,
                               "JSON nesting too deep");
  }
  json body;
  try {
    body = json::parse(request.body);
  } catch (const json::exception&) {
    return HttpResponse::Error(ApiErrorCode::kBadRequest, "Invalid JSON body");
  }

  // Full cache purge.
  if (body.contains("scope") && body["scope"] == "all") {
    if (!body.contains("confirm") || body["confirm"] != "purge-all") {
      return HttpResponse::Error(
          ApiErrorCode::kBadRequest,
          R"(Full purge requires {"confirm": "purge-all"})");
    }
    // True full purge: stop the cache, delete the volume file, and
    // recreate from scratch.  The shared_mutex in PageSpeedCache
    // ensures all in-flight operations complete before the reset.
    size_t urls_cleared = ctx.url_registry.ClearAll().size();
    // The volume goes, and every site's entries with it: drop the known
    // sites too (ClearAll keeps them, as it keeps the known hostnames).
    ctx.url_registry.ClearKnownSites();

    std::string err = ctx.reset_cache();
    if (!err.empty()) {
      return HttpResponse::Error(ApiErrorCode::kInternalError, err);
    }

    json resp;
    resp["scope"] = "all";
    resp["urls_cleared"] = urls_cleared;
    resp["volume_reset"] = true;
    return HttpResponse().Json(DumpJson(resp));
  }

  // Single-URL purge.
  if (!body.contains("url") || !body["url"].is_string()) {
    return HttpResponse::Error(ApiErrorCode::kBadRequest,
                               "Missing 'url' in request body");
  }

  std::string url = body["url"].get<std::string>();
  std::string hostname;
  bool hostname_explicit = false;
  if (body.contains("hostname") && body["hostname"].is_string()) {
    hostname = body["hostname"].get<std::string>();
    hostname_explicit = true;
  } else {
    // Extract hostname from the URL if not provided in the body.
    // nginx stores cache entries with (path, hostname) keys; the hostname
    // must match for the purge to find the entry.
    hostname = ExtractHostnameFromUrl(url);
  }

  // Reject explicitly empty hostname (CWE-345 bypass prevention).
  if (hostname_explicit && hostname.empty()) {
    return HttpResponse::Error(ApiErrorCode::kBadRequest,
                               "Field 'hostname' must not be empty.");
  }

  // Normalize hostname using the full cache normalization pipeline
  // (including host alias resolution) so aliased hostnames match
  // what nginx recorded via NormalizeCacheHostname().
  hostname = NormalizeHostnameForApi(hostname, ctx.url_norm_config);

  // Validate hostname against known hostnames (CWE-345).
  // Applies to both explicit and URL-extracted hostnames.
  // Normalize before lookup: known_hostnames_ stores normalized forms.
  if (!hostname.empty() && !ctx.url_registry.HasHostname(hostname)) {
    return HttpResponse::Error(
        ApiErrorCode::kForbidden,
        "Hostname was never served through this instance.");
  }

  std::string site;
  bool site_given = false;
  if (auto err = ReadSiteField(ctx, body, &site, &site_given)) {
    return std::move(*err);
  }

  std::string scheme;
  bool purge_both_schemes = true;
  if (body.contains("scheme") && body["scheme"].is_string()) {
    scheme = body["scheme"].get<std::string>();
    if (scheme != "http" && scheme != "https") {
      return HttpResponse::Error(ApiErrorCode::kBadRequest,
                                 "Invalid 'scheme': must be 'http' or 'https'");
    }
    purge_both_schemes = false;
  }

  // One purge acts on one keyspace.  A request naming a site purges that
  // one; without one, the purge runs over every known site and the unsited
  // keyspace.
  std::vector<std::string> sites;
  if (site_given) {
    sites.push_back(site);
  } else {
    sites = ctx.url_registry.KnownSites();
    sites.emplace_back();
  }
  std::vector<std::string> schemes;
  if (purge_both_schemes) {
    schemes = {"https", "http"};
    scheme = "both";
  } else {
    schemes = {scheme};
  }

  int deleted = 0;
  json per_site = json::array();
  for (const std::string& s : sites) {
    int site_deleted = 0;
    for (const std::string& sch : schemes) {
      site_deleted += ctx.invalidate_url(url, hostname, sch, s);
      ctx.url_registry.Remove(url, hostname, sch, s);
    }
    if (site_deleted > 0) {
      per_site.push_back({{"site", s}, {"deleted", site_deleted}});
    }
    deleted += site_deleted;
  }

  json resp;
  resp["url"] = url;
  resp["hostname"] = hostname;
  // scheme is "http", "https", or "both" (when no scheme was specified
  // in the request, both HTTP and HTTPS entries are purged).
  resp["scheme"] = scheme;
  resp["deleted"] = deleted;
  resp["sites"] = std::move(per_site);
  resp["sites_checked"] = sites.size();
  resp["known_sites_incomplete"] =
      ctx.known_sites_incomplete ? ctx.known_sites_incomplete() : false;
  if (deleted == 0) {
    resp["note"] =
        "URL not found in cache. It may have been evicted under cache "
        "pressure or was never cached.";
  }

  return HttpResponse().Json(DumpJson(resp));
}

// ---------------------------------------------------------------------------
// POST /v1/cache/reprocess
// ---------------------------------------------------------------------------

static HttpResponse HandleReprocess(CacheApiContext& ctx,
                                    const HttpRequest& request) {
  if (JsonNestingTooDeep(request.body)) {
    return HttpResponse::Error(ApiErrorCode::kBadRequest,
                               "JSON nesting too deep");
  }
  json body;
  try {
    body = json::parse(request.body);
  } catch (const json::exception&) {
    return HttpResponse::Error(ApiErrorCode::kBadRequest, "Invalid JSON body");
  }

  if (!body.contains("url") || !body["url"].is_string()) {
    return HttpResponse::Error(ApiErrorCode::kBadRequest,
                               "Missing 'url' in request body");
  }

  std::string url = body["url"].get<std::string>();
  std::string hostname;
  bool hostname_explicit = false;
  if (body.contains("hostname") && body["hostname"].is_string()) {
    hostname = body["hostname"].get<std::string>();
    hostname_explicit = true;
  } else {
    // Extract hostname from the URL when not provided in the body.
    hostname = ExtractHostnameFromUrl(url);
  }

  // Reject explicitly empty hostname (CWE-345 bypass prevention).
  if (hostname_explicit && hostname.empty()) {
    return HttpResponse::Error(ApiErrorCode::kBadRequest,
                               "Field 'hostname' must not be empty.");
  }

  std::string scheme = "https";
  if (body.contains("scheme") && body["scheme"].is_string()) {
    scheme = body["scheme"].get<std::string>();
  }
  if (scheme != "http" && scheme != "https") {
    return HttpResponse::Error(ApiErrorCode::kBadRequest,
                               "Invalid 'scheme': must be 'http' or 'https'");
  }

  // Normalize hostname using the full cache normalization pipeline
  // (including host alias resolution) so aliased hostnames match
  // what nginx recorded via NormalizeCacheHostname().
  hostname = NormalizeHostnameForApi(hostname, ctx.url_norm_config);

  // Validate hostname against known hostnames (CWE-345).
  // Applies to both explicit and URL-extracted hostnames — omitting the
  // hostname field must not bypass the validation gate.
  if (!hostname.empty() && !ctx.url_registry.HasHostname(hostname)) {
    return HttpResponse::Error(
        ApiErrorCode::kForbidden,
        "Hostname was never served through this instance.");
  }

  std::string site;
  bool site_given = false;
  if (auto err = ReadSiteField(ctx, body, &site, &site_given)) {
    return std::move(*err);
  }

  // Read content type from the original alternate's metadata.
  ContentType ct = ContentType::kOther;
  AlternateId default_id =
      MaskToAlternateId(static_cast<uint8_t>(CapabilityMask().Encode() & 0xFF));
  auto read = ctx.cache->ReadAlternate(url, hostname, scheme, default_id, site);
  if (read) {
    auto meta = AlternateMetadata::Deserialize(read->handle.content());
    if (meta) {
      ct = meta->content_type;
    }
  }

  // Clear dedup set and cooldown so the worker re-processes on
  // notification.  Don't remove cached content — the worker needs the
  // original to optimize from, and overwrites variants on write.
  if (ctx.clear_dedup) {
    ctx.clear_dedup(url, hostname, scheme, site);
  }

  // Enqueue reprocessing notification.
  if (ctx.enqueue_reprocess) {
    ctx.enqueue_reprocess(url, hostname, scheme, site, ct);
  }

  json resp;
  resp["url"] = url;
  resp["hostname"] = hostname;
  resp["scheme"] = scheme;
  resp["site"] = site;
  resp["reprocess_enqueued"] = ctx.enqueue_reprocess != nullptr;

  return HttpResponse().Json(DumpJson(resp));
}

// ---------------------------------------------------------------------------
// Cooldown listing
// ---------------------------------------------------------------------------

static HttpResponse HandleListCooldowns(CacheApiContext& ctx,
                                        const HttpRequest& request) {
  json resp;
  if (!ctx.list_cooldowns) {
    resp["cooldowns"] = json::array();
    resp["count"] = 0;
    resp["enabled"] = false;
    return HttpResponse()
        .SetHeader("Cache-Control", "no-store")
        .Json(DumpJson(resp));
  }

  auto entries = ctx.list_cooldowns();

  // Optional hostname filter.
  auto hostname_filter = request.QueryParam("hostname");

  json cooldowns = json::array();
  for (const auto& e : entries) {
    if (!hostname_filter.empty() && e.hostname != hostname_filter) continue;
    cooldowns.push_back({{"url", e.url},
                         {"hostname", e.hostname},
                         {"scheme", e.scheme},
                         {"reason", e.reason},
                         {"remaining_seconds", e.remaining_seconds},
                         {"duration_seconds", e.duration_seconds}});
  }

  resp["cooldowns"] = std::move(cooldowns);
  resp["count"] = resp["cooldowns"].size();
  resp["enabled"] = true;
  return HttpResponse()
      .SetHeader("Cache-Control", "no-store")
      .Json(DumpJson(resp));
}

// ---------------------------------------------------------------------------
// Route registration
// ---------------------------------------------------------------------------

void RegisterCacheRoutes(HttpServer& server, CacheApiContext& ctx) {
  // These five GET routes are open under --api-read-open today --
  // RouteAuth::kReadOpenOk is the explicit allow-list entry preserving
  // that.  Together they are the cached-URL inventory, the per-URL variant
  // detail, the origin-host/content-type data, and the cooldown table: the
  // read-only surface --api-read-open exists to publish.  The two mutating
  // routes (purge, reprocess) stay at the RouteAuth::kToken default --
  // --api-read-open only ever bypasses GET/HEAD, so it is a no-op for them
  // either way.
  server.AddRoute(
      "GET", "/v1/cache/alternates",
      [&ctx](const HttpRequest& req) { return HandleListAlternates(ctx, req); },
      RouteAuth::kReadOpenOk);

  server.AddRoute(
      "GET", "/v1/cache/urls",
      [&ctx](const HttpRequest& req) { return HandleListUrls(ctx, req); },
      RouteAuth::kReadOpenOk);

  server.AddRoute(
      "GET", "/v1/cache/select",
      [&ctx](const HttpRequest& req) { return HandleSelect(ctx, req); },
      RouteAuth::kReadOpenOk);

  server.AddRoute(
      "GET", "/v1/cache/content",
      [&ctx](const HttpRequest& req) { return HandleContent(ctx, req); },
      RouteAuth::kReadOpenOk);

  server.AddRoute("POST", "/v1/cache/purge", [&ctx](const HttpRequest& req) {
    return HandlePurge(ctx, req);
  });

  server.AddRoute(
      "POST", "/v1/cache/reprocess",
      [&ctx](const HttpRequest& req) { return HandleReprocess(ctx, req); });

  server.AddRoute(
      "GET", "/v1/cache/cooldowns",
      [&ctx](const HttpRequest& req) { return HandleListCooldowns(ctx, req); },
      RouteAuth::kReadOpenOk);
}

}  // namespace pagespeed
