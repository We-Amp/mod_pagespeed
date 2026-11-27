// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

// PageSpeed 2.0 - Static File Handler for Web Console SPA
//
// Serves the PageSpeed Workbench web console from an in-memory
// file cache.  All files are loaded at startup from --console-dir
// and served immutably from the event loop (no disk I/O per request).
//
// Routing:
//   GET /console/         → index.html
//   GET /console/<path>   → file if exists, else index.html (SPA fallback)
//
// Features: ETag, If-None-Match → 304, Cache-Control, CSP header.

#ifndef PAGESPEED_SRC_WORKER_STATIC_FILE_HANDLER_H_
#define PAGESPEED_SRC_WORKER_STATIC_FILE_HANDLER_H_

#include <cstddef>
#include <string>
#include <string_view>
#include <unordered_map>

#include "src/worker/http_server.h"

namespace pagespeed {

class MessageHandler;

// A single file loaded into memory.
struct FileEntry {
  std::string content;
  std::string content_type;
  std::string etag;  // Hex-encoded hash of content.
};

// In-memory file cache for the web console SPA.
class StaticFileCache {
 public:
  // Load all files recursively from `directory`.
  // Returns false if directory doesn't exist, exceeds size_limit,
  // or encounters I/O errors.
  bool Load(std::string_view directory, size_t size_limit,
            MessageHandler* handler);

  // Look up a file by path relative to the console root.
  // Returns nullptr if not found.
  const FileEntry* Lookup(std::string_view path) const;

  // Get the index.html entry (SPA fallback).
  const FileEntry* IndexHtml() const;

  // Number of loaded files.
  size_t file_count() const { return files_.size(); }

  // Total bytes loaded.
  size_t total_bytes() const { return total_bytes_; }

 private:
  std::unordered_map<std::string, FileEntry> files_;
  size_t total_bytes_ = 0;
};

// Derive Content-Type from file extension.
std::string_view ContentTypeForExtension(std::string_view path);

// Register the /console/* route on the server using the given cache.
// The cache must outlive the server.
void RegisterConsoleRoute(HttpServer& server, const StaticFileCache& cache,
                          bool security_headers = true);

}  // namespace pagespeed

#endif  // PAGESPEED_SRC_WORKER_STATIC_FILE_HANDLER_H_
