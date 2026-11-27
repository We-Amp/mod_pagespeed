// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

// PageSpeed 2.0 - HTTP 103 Early Hints (nginx-specific)
//
// Sends HTTP 103 Early Hints informational responses before the main
// response for HTML pages that have known critical CSS resources.
// This allows browsers to start preloading stylesheets while the
// server is still preparing the full response.

#ifndef PAGESPEED_SRC_NGINX_EARLY_HINTS_H_
#define PAGESPEED_SRC_NGINX_EARLY_HINTS_H_

extern "C" {
#include <ngx_config.h>
#include <ngx_core.h>
#include <ngx_http.h>
}

#include <vector>

#include "src/nginx/early_hints_util.h"

namespace pagespeed {

// Send an HTTP 103 Early Hints response with Link preload headers.
// Returns NGX_OK on success, NGX_ERROR on failure.
//
// This must be called BEFORE ngx_http_send_header() for the main
// response.  Not all nginx versions support 103; if the version
// doesn't support it, this function returns NGX_OK (no-op).
ngx_int_t SendEarlyHints(ngx_http_request_t* r,
                         const std::vector<PreloadResource>& resources);

}  // namespace pagespeed

#endif  // PAGESPEED_SRC_NGINX_EARLY_HINTS_H_
