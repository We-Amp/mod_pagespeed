// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

#ifndef PAGESPEED_KERNEL_PACKS_URL_NORM_H_
#define PAGESPEED_KERNEL_PACKS_URL_NORM_H_

#include <optional>
#include <string>
#include <string_view>

namespace pagespeed::packs {

// Normalizes an absolute http(s) URL for equality comparison:
//   - scheme and host are lowercased;
//   - the default port (80 for http, 443 for https) is dropped;
//   - the fragment is dropped;
//   - path and query are kept as written, except that a backslash before the
//     query counts as '/' and a bare trailing '?' (empty query) is dropped;
//   - surrounding whitespace is trimmed;
//   - a single trailing '/' on the path is ignored (so "https://a.test/" and
//     "https://a.test" are equal, and "https://a.test/x/" equals
//     "https://a.test/x").
// Returns nullopt when the input is not an absolute http(s) URL with a host
// (user-info is not supported and counts as invalid).
std::optional<std::string> NormUrl(std::string_view url);

// Lowercases an hreflang code, maps '_' to '-', and validates the result
// against ^(x-default|[a-z]{2,3}(-[a-z]{4})?(-([a-z]{2}|\d{3}))?)$.
// Returns the normalized code, or nullopt when it is not valid.
std::optional<std::string> NormalizeHreflangCode(std::string_view code);

}  // namespace pagespeed::packs

#endif  // PAGESPEED_KERNEL_PACKS_URL_NORM_H_
