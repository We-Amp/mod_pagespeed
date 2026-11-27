// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

#include "lib/classify/content_type.h"

#include <algorithm>
#include <string>

#include "lib/base/string_util.h"

namespace pagespeed {

namespace {

// Extract just the MIME type portion, stripping parameters.
// "text/html; charset=utf-8" -> "text/html"
std::string_view StripParameters(std::string_view header) {
  auto pos = header.find(';');
  if (pos != std::string_view::npos) {
    header = header.substr(0, pos);
  }
  // Trim whitespace
  while (!header.empty() && header.front() == ' ') {
    header.remove_prefix(1);
  }
  while (!header.empty() && header.back() == ' ') {
    header.remove_suffix(1);
  }
  return header;
}

}  // namespace

bool IsTextHtmlMediaType(std::string_view content_type_header) {
  return net_instaweb::StringCaseEqual(StripParameters(content_type_header),
                                       "text/html");
}

ContentType ClassifyContentType(std::string_view content_type_header) {
  std::string_view mime = StripParameters(content_type_header);

  if (mime.empty()) {
    return ContentType::kOther;
  }

  // HTML
  if (net_instaweb::StringCaseEqual(mime, "text/html") ||
      net_instaweb::StringCaseEqual(mime, "application/xhtml+xml")) {
    return ContentType::kHtml;
  }

  // CSS
  if (net_instaweb::StringCaseEqual(mime, "text/css")) {
    return ContentType::kCss;
  }

  // JavaScript
  if (net_instaweb::StringCaseEqual(mime, "application/javascript") ||
      net_instaweb::StringCaseEqual(mime, "text/javascript") ||
      net_instaweb::StringCaseEqual(mime, "application/x-javascript") ||
      net_instaweb::StringCaseEqual(mime, "application/ecmascript") ||
      net_instaweb::StringCaseEqual(mime, "text/ecmascript")) {
    return ContentType::kJs;
  }

  // Images — any image/* MIME type
  if (net_instaweb::StringCaseStartsWith(mime, "image/")) {
    return ContentType::kImage;
  }

  return ContentType::kOther;
}

std::string_view ContentTypeMime(ContentType type) {
  switch (type) {
    case ContentType::kHtml:
      return "text/html";
    case ContentType::kCss:
      return "text/css";
    case ContentType::kJs:
      return "application/javascript";
    case ContentType::kImage:
      return "image/jpeg";  // Generic; actual type from variant metadata
    case ContentType::kOther:
      return "application/octet-stream";
  }
  return "application/octet-stream";
}

}  // namespace pagespeed
