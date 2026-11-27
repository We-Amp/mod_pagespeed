// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

#ifndef PAGESPEED_LIB_CLASSIFY_CONTENT_TYPE_H_
#define PAGESPEED_LIB_CLASSIFY_CONTENT_TYPE_H_

#include <cstdint>
#include <string_view>

namespace pagespeed {

// Broad content categories used for optimization dispatch.
enum class ContentType : uint8_t {
  kHtml = 0,
  kCss = 1,
  kJs = 2,
  kImage = 3,
  kOther = 4,
};

// Classify a Content-Type header value into a broad category.
// Strips parameters (charset, boundary, etc.) before matching.
// Case-insensitive matching on the MIME type.
//
// Examples:
//   "text/html; charset=utf-8"     -> kHtml
//   "text/css"                     -> kCss
//   "application/javascript"       -> kJs
//   "text/javascript"              -> kJs
//   "image/jpeg"                   -> kImage
//   "image/webp"                   -> kImage
//   "application/octet-stream"     -> kOther
//   ""                             -> kOther
ContentType ClassifyContentType(std::string_view content_type_header);

// True only when the media type of a Content-Type header value is exactly
// `text/html` (ASCII case-insensitive, parameters such as charset ignored).
// Unlike ClassifyContentType, which files application/xhtml+xml under kHtml
// too, this tells an HTML document from an XHTML one. "" -> false.
bool IsTextHtmlMediaType(std::string_view content_type_header);

// Return the canonical MIME string for a ContentType.
// Returns "application/octet-stream" for kOther.
std::string_view ContentTypeMime(ContentType type);

}  // namespace pagespeed

#endif  // PAGESPEED_LIB_CLASSIFY_CONTENT_TYPE_H_
