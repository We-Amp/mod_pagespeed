// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

// PageSpeed 2.0 - CDP Utility Functions
//
// Shared helpers used by browser analysis sessions (page_analysis,
// script_coverage_analyzer, etc.) for Fetch interception.

#ifndef PAGESPEED_SRC_BROWSER_CDP_UTILS_H_
#define PAGESPEED_SRC_BROWSER_CDP_UTILS_H_

#include <string>
#include <string_view>

namespace pagespeed {
namespace cdp_utils {

// Base64-encode data using standard alphabet with padding.
// Used by Fetch.fulfillRequest to serve cached resources.
std::string Base64Encode(const std::string& data);

// Infer Content-Type from URL file extension.
// Returns "application/octet-stream" for unknown extensions.
std::string_view GuessContentType(std::string_view url);

}  // namespace cdp_utils
}  // namespace pagespeed

#endif  // PAGESPEED_SRC_BROWSER_CDP_UTILS_H_
