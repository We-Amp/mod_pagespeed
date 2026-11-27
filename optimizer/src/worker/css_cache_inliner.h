// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

// PageSpeed 2.0 - CSS Cache Inliner for Browser Analysis
//
// Inlines cached stylesheets into HTML before passing to Chrome's CSS
// Coverage API.  Without this, Chrome cannot fetch external stylesheets
// in its sandboxed (network-offline) environment and reports 0% coverage.

#ifndef PAGESPEED_SRC_WORKER_CSS_CACHE_INLINER_H_
#define PAGESPEED_SRC_WORKER_CSS_CACHE_INLINER_H_

#include <string>
#include <string_view>

#include "lib/css/css_import_flattener.h"  // css::CssLookupFn

namespace pagespeed {

struct CssInliningStats {
  size_t stylesheets_found = 0;
  size_t stylesheets_cached = 0;
  size_t bytes_inlined = 0;
};

// Inlines cached stylesheets into HTML for browser CSS coverage analysis.
// Parses <link rel="stylesheet"> tags, looks up each in cache via lookup,
// flattens @import chains, sanitizes for XSS, and injects <style> blocks.
// Returns enriched HTML (or original if nothing to inline). Each href is
// resolved against the document base (the page URL or its <base href>,
// DocumentBaseOf in html_scanner.h), exactly as the serve path's
// combined-stylesheet gather resolves it; `page_host` and `page_scheme` are
// the page's own when `page_url` is a cache-normalized path without them.
std::string InlineCachedStylesheets(std::string_view html,
                                    std::string_view page_url,
                                    const css::CssLookupFn& lookup,
                                    CssInliningStats* stats = nullptr,
                                    std::string_view page_host = {},
                                    std::string_view page_scheme = {});

}  // namespace pagespeed

#endif  // PAGESPEED_SRC_WORKER_CSS_CACHE_INLINER_H_
