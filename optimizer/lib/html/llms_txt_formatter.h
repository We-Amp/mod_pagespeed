// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

#ifndef PAGESPEED_LIB_HTML_LLMS_TXT_FORMATTER_H_
#define PAGESPEED_LIB_HTML_LLMS_TXT_FORMATTER_H_

#include <string>
#include <string_view>
#include <vector>

namespace net_instaweb {

// One link entry in a /llms.txt section. `summary` is optional.
struct LlmsTxtLink {
  std::string title;
  std::string url;
  std::string summary;
};

// A group of links under a "## heading".
struct LlmsTxtSection {
  std::string heading;
  std::vector<LlmsTxtLink> links;
};

// Assemble a /llms.txt document in the llmstxt.org format:
//
//   # <site_title>
//
//   > <site_summary>
//
//   ## <section heading>
//
//   - [<link title>](<link url>): <link summary>
//   - [<link title>](<link url>)
//
// PURE (string -> string). SECURITY: every field (titles/summaries/urls) is
// attacker-influenced (derived from a customer's page content) and the output
// is consumed by an LLM agent, so the formatter SANITIZES so a page cannot
// forge markdown STRUCTURE (extra headings, list items, link/blockquote
// injection) into the feed: text fields are collapsed to a single line with
// control bytes stripped and length-capped; link text escapes the markdown
// link delimiters; urls are validated http(s) and have markdown-breaking bytes
// percent-encoded. An empty section/link set yields a minimal-but-valid file.
std::string FormatLlmsTxt(std::string_view site_title,
                          std::string_view site_summary,
                          const std::vector<LlmsTxtSection>& sections);

}  // namespace net_instaweb

#endif  // PAGESPEED_LIB_HTML_LLMS_TXT_FORMATTER_H_
