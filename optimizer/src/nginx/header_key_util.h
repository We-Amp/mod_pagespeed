// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

#ifndef PAGESPEED_SRC_NGINX_HEADER_KEY_UTIL_H_
#define PAGESPEED_SRC_NGINX_HEADER_KEY_UTIL_H_

// Lowercase key and hash for request headers the module adds at runtime.
// Pure C++ (no nginx headers) so it is unit-testable without an nginx build.
//
// nginx stores every request header with a lowercase copy of its name
// (ngx_table_elt_t::lowcase_key) and a hash of that lowercase name
// (::hash), and later request processing reads both.  The hash is
// ngx_hash_key_lc: h = h * 31 + lowercase(c), starting from 0, in the
// width of ngx_uint_t (uintptr_t).

#include <cstddef>
#include <cstdint>
#include <string_view>

namespace pagespeed {

// Writes the ASCII-lowercased `name` to `out` (name.size() bytes, no NUL)
// and returns the nginx key hash of the lowercased name.
std::uintptr_t LowercaseHeaderKey(std::string_view name, unsigned char* out);

}  // namespace pagespeed

#endif  // PAGESPEED_SRC_NGINX_HEADER_KEY_UTIL_H_
