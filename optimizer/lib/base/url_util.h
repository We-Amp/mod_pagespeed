// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

// PageSpeed 2.0 - URL utility functions

#ifndef LIB_BASE_URL_UTIL_H_
#define LIB_BASE_URL_UTIL_H_

#include <string>
#include <string_view>

namespace pagespeed {

// Get the directory portion of a URL path.
// "http://example.com/css/style.css" -> "http://example.com/css/"
// "/css/style.css" -> "/css/"
// "/style.css" -> "/"
// "style.css" -> ""
std::string_view UrlDirectory(std::string_view url);

// Resolve a relative path against a base directory.
// Handles "../" and "./" normalization.  Absolute paths and full URLs
// (containing "://") are returned as-is.
std::string ResolvePath(std::string_view base_dir, std::string_view relative);

// Extract the hostname (with port if non-default) from a URL.
// "http://cdn.example.com:8080/path" -> "cdn.example.com:8080"
// "https://example.com/path" -> "example.com"
// "/relative/path" -> "" (empty for relative URLs)
std::string_view UrlHostname(std::string_view url);

}  // namespace pagespeed

#endif  // LIB_BASE_URL_UTIL_H_
