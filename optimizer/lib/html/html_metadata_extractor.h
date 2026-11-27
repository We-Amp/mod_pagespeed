// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

#ifndef PAGESPEED_LIB_HTML_HTML_METADATA_EXTRACTOR_H_
#define PAGESPEED_LIB_HTML_HTML_METADATA_EXTRACTOR_H_

#include <string>
#include <string_view>

namespace net_instaweb {

// Cheap page metadata for the /llms.txt builder's per-page summary.
// Populated by a single SAX pass over a page's raw HTML — NO Chrome
// render. Fields are entity-decoded (the HTML parser decodes attribute/text
// entities) and empty when absent.
struct HtmlMetadata {
  std::string title;             // <title> text content
  std::string meta_description;  // <meta name="description" content="...">
  std::string
      meta_robots;        // <meta name="robots"|"ai" content="..."> (for §2.6)
  std::string canonical;  // <link rel="canonical" href="..."> (Issue F)
};

// Parse `html` and extract <title> + <meta name=description|robots>. `url` only
// satisfies the parser. PURE, fail-soft (malformed HTML never crashes; missing
// fields stay empty), self-bounded (per-field byte caps).
HtmlMetadata ExtractHtmlMetadata(std::string_view html, std::string_view url);

}  // namespace net_instaweb

#endif  // PAGESPEED_LIB_HTML_HTML_METADATA_EXTRACTOR_H_
