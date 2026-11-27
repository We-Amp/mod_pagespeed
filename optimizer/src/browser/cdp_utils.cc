// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

// PageSpeed 2.0 - CDP Utility Functions Implementation

#include "src/browser/cdp_utils.h"

#include <cstdint>
#include <string>
#include <string_view>

namespace pagespeed::cdp_utils {

std::string Base64Encode(const std::string& data) {
  static const char kChars[] =
      "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz"
      "0123456789+/";
  std::string result;
  result.reserve((data.size() * 4 + 2) / 3 + 4);

  // Process full 3-byte groups.
  size_t i = 0;
  for (; i + 2 < data.size(); i += 3) {
    uint32_t triple = (static_cast<uint8_t>(data[i]) << 16) |
                      (static_cast<uint8_t>(data[i + 1]) << 8) |
                      static_cast<uint8_t>(data[i + 2]);
    result += kChars[(triple >> 18) & 0x3F];
    result += kChars[(triple >> 12) & 0x3F];
    result += kChars[(triple >> 6) & 0x3F];
    result += kChars[triple & 0x3F];
  }

  // Handle 1-byte remainder: encode 2 chars + "==".
  if (i + 1 == data.size()) {
    uint32_t val = static_cast<uint8_t>(data[i]);
    result += kChars[(val >> 2) & 0x3F];
    result += kChars[(val << 4) & 0x3F];
    result += "==";
  }
  // Handle 2-byte remainder: encode 3 chars + "=".
  else if (i + 2 == data.size()) {
    uint32_t val = (static_cast<uint8_t>(data[i]) << 8) |
                   static_cast<uint8_t>(data[i + 1]);
    result += kChars[(val >> 10) & 0x3F];
    result += kChars[(val >> 4) & 0x3F];
    result += kChars[(val << 2) & 0x3F];
    result += '=';
  }

  return result;
}

std::string_view GuessContentType(std::string_view url) {
  auto dot = url.rfind('.');
  if (dot == std::string_view::npos) {
    return "application/octet-stream";
  }
  // Strip query string if present.
  auto query = url.find('?', dot);
  auto ext =
      url.substr(dot, query != std::string_view::npos ? query - dot
                                                      : std::string_view::npos);
  if (ext == ".css") return "text/css";
  if (ext == ".js") return "application/javascript";
  if (ext == ".png") return "image/png";
  if (ext == ".jpg" || ext == ".jpeg") return "image/jpeg";
  if (ext == ".gif") return "image/gif";
  if (ext == ".svg") return "image/svg+xml";
  if (ext == ".webp") return "image/webp";
  if (ext == ".avif") return "image/avif";
  if (ext == ".woff2") return "font/woff2";
  if (ext == ".woff") return "font/woff";
  if (ext == ".html") return "text/html";
  return "application/octet-stream";
}

}  // namespace pagespeed::cdp_utils
