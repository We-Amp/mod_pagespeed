// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

#ifndef PAGESPEED_SRC_NGINX_COOKIE_CACHE_GATE_H_
#define PAGESPEED_SRC_NGINX_COOKIE_CACHE_GATE_H_

// Shared-cache gate for requests that carry a Cookie header.  Pure C++ (no
// nginx headers) so the decision is unit-testable without an nginx build
// (same pattern as authz_cache_gate).
//
// The rule is the HTML cache policy's (lib/cache/html_cache_policy.h, "THE
// COOKIE RULE"), called through CookieRequestMayStore / CookieRequestMayServe
// so the port and the policy cannot drift.  In short, for a cookie-bearing
// request:
//   - a page (anything that is not css, JavaScript or an image, including
//     XHTML, JSON, plain text, the Markdown copy and an unknown type) is
//     recorded, and a stored page served, only when the origin marked it
//     `public` or gave it an s-maxage above zero;
//   - a static asset is recorded only when the origin stated a freshness
//     lifetime (no class default applies); a stored asset is served as
//     before.
// A page fetched for one visitor may depend on that visitor's cookie, so
// without the mark it is not shared with anyone else.
//
// The record path passes the live response's Content-Type and parsed
// Cache-Control (Expires folded into max-age where no Cache-Control line was
// sent, exactly as the record path stores it); the HIT path passes the STORED
// entry's.  The Markdown copy, the Early Hints set and /llms.txt store no
// Cache-Control of their own: the caller passes the recorded page's flags
// (or none, which fails closed for a cookie-bearing request).
//
// Independent of the Authorization gate (authz_cache_gate.h): a request
// carrying both must pass both.

#include <cstdint>
#include <string_view>

#include "lib/cache/html_cache_policy.h"
#include "lib/classify/content_type.h"

namespace pagespeed {

// The response class of a stored entry: its stored origin Content-Type when
// it has one, else the class it was stored under.
CookieShareClass StoredEntryCookieClass(std::string_view origin_content_type,
                                        ContentType stored_type);

// Record path.
bool CookieCacheGateAllowsStore(bool request_has_cookie,
                                std::string_view content_type,
                                uint16_t origin_cc_flags,
                                uint32_t origin_max_age,
                                uint32_t origin_s_maxage);

// HIT path, for an entry of the given class.
bool CookieCacheGateAllowsServe(bool request_has_cookie,
                                CookieShareClass entry_class,
                                uint16_t origin_cc_flags,
                                uint32_t origin_s_maxage);

}  // namespace pagespeed

#endif  // PAGESPEED_SRC_NGINX_COOKIE_CACHE_GATE_H_
