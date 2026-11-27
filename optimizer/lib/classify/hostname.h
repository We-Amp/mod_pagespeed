// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

#ifndef PAGESPEED_LIB_CLASSIFY_HOSTNAME_H_
#define PAGESPEED_LIB_CLASSIFY_HOSTNAME_H_

#include <string>
#include <string_view>

namespace pagespeed {

// Normalize a hostname for cache key composition.
// - Lowercase (ASCII only, no IDN/punycode)
// - Strip default port (:80, :443)
// - Strip trailing dot
// - Preserve non-default ports (e.g., :8080)
// - Empty input returns empty string
std::string NormalizeHostname(std::string_view hostname);

}  // namespace pagespeed

#endif  // PAGESPEED_LIB_CLASSIFY_HOSTNAME_H_
