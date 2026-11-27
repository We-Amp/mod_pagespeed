// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

#ifndef PAGESPEED_LIB_HTML_SITEMAP_PARSER_H_
#define PAGESPEED_LIB_HTML_SITEMAP_PARSER_H_

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace net_instaweb {

// Result of parsing a sitemap.xml (the /llms.txt builder).
//
// A sitemap document is EITHER a <urlset> (a list of page <loc>s) OR a
// <sitemapindex> (a list of nested-sitemap <loc>s) per the sitemaps.org
// protocol — never both. We surface the two kinds separately so the builder
// can do a BOUNDED one-level fan-out over nested sitemaps (deep nested trees
// are a documented non-goal).
//
// Parsing is PURE (no I/O) and FAIL-SAFE: it never crashes on malformed input
// and never emits a partial/garbage <loc>. It does NOT filter by origin — the
// builder is responsible for dropping off-origin <loc>s (separation of concerns
// keeps this lib hermetic and testable). Entries are capped at kSitemapMaxEntries.
struct ParsedSitemap {
  std::vector<std::string> page_locs;  // from <urlset>/<url>/<loc>
  std::vector<std::string>
      nested_sitemap_locs;  // from <sitemapindex>/<sitemap>/<loc>
  bool truncated = false;   // hit the kSitemapMaxEntries cap
};

// Self-bound: do NOT trust the input size. A conformant sitemap holds at most
// 50,000 URLs / 50 MiB uncompressed (sitemaps.org); we mirror those ceilings.
inline constexpr std::size_t kSitemapMaxEntries = 50000;
inline constexpr std::size_t kSitemapMaxInputBytes = 64u << 20;  // 64 MiB

// Parse a sitemap XML document. Input beyond kSitemapMaxInputBytes is ignored.
ParsedSitemap ParseSitemap(std::string_view xml);

}  // namespace net_instaweb

#endif  // PAGESPEED_LIB_HTML_SITEMAP_PARSER_H_
