// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.
//
// Compat header for the vendored 1.15 HTML kernel (#1130): stands in for
// the canonical pagespeed/kernel/html/html_page_limit.h header.
//
// The vendored lexer passes the rest of a page through unparsed and
// unchanged once it has read this many bytes of it, which bounds the
// parser's memory for one page. The value is the canonical one, so both
// products optimize the same prefix of a long page; change it only
// together with the canonical header.

#ifndef PAGESPEED_LIB_HTML_COMPAT_HTML_PAGE_LIMIT_H_
#define PAGESPEED_LIB_HTML_COMPAT_HTML_PAGE_LIMIT_H_

#include <cstdint>

namespace net_instaweb {

// How much of one HTML page is optimized (canonical: 64 MiB).
constexpr int64_t kHtmlPageLimitBytes = int64_t{64} << 20;

}  // namespace net_instaweb

#endif  // PAGESPEED_LIB_HTML_COMPAT_HTML_PAGE_LIMIT_H_
