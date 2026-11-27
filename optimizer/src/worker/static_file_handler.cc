// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

// PageSpeed 2.0 - Static File Handler Implementation

#include "src/worker/static_file_handler.h"

#include <cstdio>
#include <filesystem>
#include <string>

#include "absl/strings/str_cat.h"
#include "absl/strings/str_format.h"
#include "lib/base/message_handler.h"

namespace pagespeed {

// ---------------------------------------------------------------------------
// Content-Type detection
// ---------------------------------------------------------------------------

std::string_view ContentTypeForExtension(std::string_view path) {
  auto dot = path.rfind('.');
  if (dot == std::string_view::npos) return "application/octet-stream";

  std::string_view ext = path.substr(dot);

  if (ext == ".html" || ext == ".htm") return "text/html; charset=utf-8";
  if (ext == ".js" || ext == ".mjs") return "application/javascript";
  if (ext == ".css") return "text/css";
  if (ext == ".json") return "application/json";
  if (ext == ".svg") return "image/svg+xml";
  if (ext == ".png") return "image/png";
  if (ext == ".jpg" || ext == ".jpeg") return "image/jpeg";
  if (ext == ".gif") return "image/gif";
  if (ext == ".ico") return "image/x-icon";
  if (ext == ".webp") return "image/webp";
  if (ext == ".avif") return "image/avif";
  if (ext == ".woff") return "font/woff";
  if (ext == ".woff2") return "font/woff2";
  if (ext == ".ttf") return "font/ttf";
  if (ext == ".map") return "application/json";
  if (ext == ".txt") return "text/plain";
  if (ext == ".xml") return "application/xml";
  if (ext == ".webmanifest") return "application/manifest+json";

  return "application/octet-stream";
}

// ---------------------------------------------------------------------------
// ETag computation (simple FNV-1a hash, hex-encoded)
// ---------------------------------------------------------------------------

static std::string ComputeEtag(std::string_view content) {
  // FNV-1a 64-bit.
  uint64_t hash = 0xcbf29ce484222325ULL;
  for (char c : content) {
    hash ^= static_cast<uint8_t>(c);
    hash *= 0x100000001b3ULL;
  }
  return absl::StrFormat("\"%016x\"", hash);
}

// ---------------------------------------------------------------------------
// StaticFileCache
// ---------------------------------------------------------------------------

bool StaticFileCache::Load(std::string_view directory, size_t size_limit,
                           MessageHandler* handler) {
  namespace fs = std::filesystem;

  std::error_code ec;
  fs::path dir_path(directory);

  if (!fs::is_directory(dir_path, ec)) {
    handler->Error("Console dir '%.*s' is not a directory",
                   static_cast<int>(directory.size()), directory.data());
    return false;
  }

  size_t total = 0;
  size_t count = 0;

  for (auto it = fs::recursive_directory_iterator(dir_path, ec);
       it != fs::recursive_directory_iterator(); ++it) {
    if (ec) {
      handler->Error("Error iterating console dir: %s", ec.message().c_str());
      return false;
    }

    if (!it->is_regular_file(ec)) continue;
    if (ec) continue;

    auto file_size = it->file_size(ec);
    if (ec) continue;

    total += file_size;
    if (total > size_limit) {
      handler->Error("Console dir exceeds size limit (%zu > %zu bytes)", total,
                     size_limit);
      return false;
    }

    // Read file content.
    std::string content;
    content.resize(file_size);
    FILE* f = fopen(it->path().string().c_str(), "rb");
    if (f == nullptr) continue;
    size_t read = fread(content.data(), 1, file_size, f);
    if (fclose(f) != 0 || read != file_size) continue;

    // Compute relative path from directory root.
    fs::path rel = fs::relative(it->path(), dir_path, ec);
    if (ec) continue;
    std::string key = rel.generic_string();

    FileEntry entry;
    entry.content_type = std::string(ContentTypeForExtension(key));
    entry.etag = ComputeEtag(content);
    entry.content = std::move(content);

    files_[std::move(key)] = std::move(entry);
    ++count;
  }

  total_bytes_ = total;
  handler->Info("Console: loaded %zu files (%zu bytes) from %.*s", count, total,
                static_cast<int>(directory.size()), directory.data());
  return true;
}

const FileEntry* StaticFileCache::Lookup(std::string_view path) const {
  auto it = files_.find(std::string(path));
  if (it != files_.end()) return &it->second;
  return nullptr;
}

const FileEntry* StaticFileCache::IndexHtml() const {
  return Lookup("index.html");
}

// ---------------------------------------------------------------------------
// Route handler
// ---------------------------------------------------------------------------

static constexpr std::string_view kConsolePrefix = "/console/";
static constexpr std::string_view kCspHeader =
    "default-src 'self'; "
    "script-src 'self' 'unsafe-inline'; "
    "style-src 'self' 'unsafe-inline'; "
    "connect-src 'self' ws: wss:; "
    "img-src 'self' data: blob:; "
    "frame-src https://modpagespeed.com; "
    "frame-ancestors 'self'";

void RegisterConsoleRoute(HttpServer& server, const StaticFileCache& cache,
                          bool security_headers) {
  server.AddRoute(
      "GET", "/console/*",
      [&cache, security_headers](const HttpRequest& request) -> HttpResponse {
        // Strip the /console/ prefix.
        std::string_view path = request.path;
        if (path.starts_with(kConsolePrefix)) {
          path.remove_prefix(kConsolePrefix.size());
        }

        // Empty path or trailing slash → index.html.
        if (path.empty()) {
          path = "index.html";
        }

        const FileEntry* entry = cache.Lookup(path);
        bool is_spa_fallback = false;

        if (!entry) {
          // SPA fallback: serve index.html for unknown paths.
          entry = cache.IndexHtml();
          is_spa_fallback = true;
          if (!entry) {
            return HttpResponse::Error(ApiErrorCode::kNotFound,
                                       "Console not configured");
          }
        }

        // Check If-None-Match for 304 responses.
        std::string_view if_none_match = request.Header("if-none-match");
        if (!if_none_match.empty() && if_none_match == entry->etag) {
          return HttpResponse().Status(304).SetHeader("ETag", entry->etag);
        }

        // Determine Cache-Control policy.
        // index.html and SPA fallback: no-cache (revalidate every time).
        // Everything else: immutable (hashed by build tools).
        bool is_index = (path == "index.html") || is_spa_fallback;
        std::string cache_control =
            is_index ? "no-cache" : "public, max-age=31536000, immutable";

        auto resp = HttpResponse()
                        .ContentType(entry->content_type)
                        .SetHeader("ETag", entry->etag)
                        .SetHeader("Cache-Control", std::move(cache_control));
        if (security_headers) {
          resp.SetHeader("Content-Security-Policy", std::string(kCspHeader))
              .SetHeader("X-Content-Type-Options", "nosniff")
              .SetHeader("X-Frame-Options", "SAMEORIGIN");
        }
        return resp.Body(entry->content);
      });
}

}  // namespace pagespeed
