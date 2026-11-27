// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

// PageSpeed 2.0 - MIME type detection from URL extensions
//
// Pure C++ utility for mapping URL extensions to MIME types.
// Used by both the nginx module and tests.

#ifndef PAGESPEED_SRC_NGINX_MIME_UTIL_H_
#define PAGESPEED_SRC_NGINX_MIME_UTIL_H_

#include <string_view>

namespace pagespeed {

// Determine Content-Type MIME string from URL extension.
// Strips query strings before checking extension.
// Returns nullptr if no recognized extension is found.
const char* MimeFromUrl(std::string_view url);

}  // namespace pagespeed

#endif  // PAGESPEED_SRC_NGINX_MIME_UTIL_H_
