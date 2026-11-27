// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

// PageSpeed 2.0 - MIME type detection from URL extensions

#include "src/nginx/mime_util.h"

namespace pagespeed {

const char* MimeFromUrl(std::string_view url) {
  // Find the last '.' before any query string.
  auto query_pos = url.find('?');
  std::string_view path =
      (query_pos != std::string_view::npos) ? url.substr(0, query_pos) : url;

  // URLs ending with '/' are HTML by universal web convention.
  if (!path.empty() && path.back() == '/') {
    return "text/html";
  }

  auto dot_pos = path.rfind('.');
  if (dot_pos == std::string_view::npos) {
    return nullptr;
  }

  std::string_view ext = path.substr(dot_pos);

  // Match common extensions (case-sensitive for performance; URLs are
  // typically lowercase).
  if (ext == ".html" || ext == ".htm") return "text/html";
  if (ext == ".css") return "text/css";
  if (ext == ".js") return "application/javascript";
  if (ext == ".jpg" || ext == ".jpeg") return "image/jpeg";
  if (ext == ".png") return "image/png";
  if (ext == ".webp") return "image/webp";
  if (ext == ".avif") return "image/avif";
  if (ext == ".gif") return "image/gif";
  if (ext == ".svg") return "image/svg+xml";
  if (ext == ".json") return "application/json";
  if (ext == ".xml") return "application/xml";
  if (ext == ".woff2") return "font/woff2";
  if (ext == ".woff") return "font/woff";

  return nullptr;
}

}  // namespace pagespeed
