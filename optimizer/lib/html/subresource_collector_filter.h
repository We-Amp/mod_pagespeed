// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

// PageSpeed 2.0 - SubresourceCollectorFilter
//
// Shared HtmlParse filter that collects subresource metadata from HTML.
// Used by both the nginx module (miss-path scanning) and the worker
// (critical CSS extraction). Collects <link rel="stylesheet">,
// <script src>, <base href>, and CSP detection from <meta> tags.

#ifndef PAGESPEED_LIB_HTML_SUBRESOURCE_COLLECTOR_FILTER_H_
#define PAGESPEED_LIB_HTML_SUBRESOURCE_COLLECTOR_FILTER_H_

#include <cstdint>
#include <string>
#include <vector>

#include "lib/html/empty_html_filter.h"
#include "lib/html/html_name.h"

namespace net_instaweb {

class HtmlElement;

// Result of scanning HTML for subresource information.
struct NginxScanResult {
  struct SubresourceInfo {
    std::string url;          // resolved href/src
    std::string media;        // media attribute (stylesheets only)
    std::string crossorigin;  // crossorigin attribute value (if present)
    enum Type : std::uint8_t { kStylesheet, kScript } type;
  };

  std::vector<SubresourceInfo> subresources;

  // CSP detection flags (from <meta http-equiv="Content-Security-Policy">)
  bool has_nonce_csp = false;
  bool has_hash_csp = false;
  bool has_restrictive_style_csp = false;

  // Base URL from <base href="..."> (empty if none found)
  std::string base_url;
};

// HTML filter that collects subresource metadata.
//
// Handles:
// - <link rel="stylesheet" href="..."> with media, crossorigin attributes
// - <script src="...">
// - <base href="..."> URL resolution
// - <template> depth tracking (subresources inside <template> are skipped)
// - CSP detection from <meta http-equiv="Content-Security-Policy">
// - rel attribute parsing using HTML5 whitespace (IsHtmlSpace)
//
// Safety limits:
// - Maximum 500 subresources collected (kMaxSubresources)
class SubresourceCollectorFilter : public EmptyHtmlFilter {
 public:
  static constexpr int kMaxSubresources = 500;

  explicit SubresourceCollectorFilter(NginxScanResult* result);
  ~SubresourceCollectorFilter() override;

  const char* Name() const override { return "SubresourceCollector"; }

  void StartDocument() override;
  void EndDocument() override;
  void StartElement(HtmlElement* element) override;
  void EndElement(HtmlElement* element) override;

  // Returns true if the maximum subresource limit was reached.
  bool limit_reached() const { return limit_reached_; }

 private:
  // Check if a rel attribute value contains "stylesheet" as a token.
  // Uses HTML5 whitespace splitting (IsHtmlSpace).
  static bool RelContainsStylesheet(const char* rel_value);

  // Parse a CSP meta tag content for nonce/hash/restrictive directives.
  void ParseCspContent(const char* content);

  // Parse a single CSP directive value for nonces, hashes, and
  // unsafe-inline.
  void ParseCspDirectiveValue(std::string_view value);

  // Resolve a URL against the base URL (simple concatenation for
  // relative URLs starting with /).
  std::string ResolveUrl(const char* url) const;

  NginxScanResult* result_;
  int template_depth_;
  bool limit_reached_;
};

}  // namespace net_instaweb

#endif  // PAGESPEED_LIB_HTML_SUBRESOURCE_COLLECTOR_FILTER_H_
