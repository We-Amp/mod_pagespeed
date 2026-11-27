// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.
//
// Compat header for the vendored 1.15 JS kernel: stands in for
// the canonical pagespeed/kernel/base/string_util.h header, supplying the exact
// surface the vendored files consume — StringPiece and strings::{StartsWith,
// EndsWith} — as aliases over the 2.0-native std types. StringPiece is a
// true alias of std::string_view, so the vendored kernel's public API
// (js_minify.h, js_tokenizer.h) is byte-compatible with existing 2.0
// consumers without any call-site churn. See lib/js/CLAUDE.md.

#ifndef PAGESPEED_LIB_JS_COMPAT_STRING_UTIL_H_
#define PAGESPEED_LIB_JS_COMPAT_STRING_UTIL_H_

#include <cstddef>
#include <string_view>

// The 2.0-native string utilities: supplies net_instaweb::IsAsciiAlphaNumeric
// (and friends) that the vendored kernel calls; deliberately included rather
// than redefined here so there is exactly one definition repo-wide.
#include "lib/base/string_util.h"
#include "lib/js/compat/string.h"

using StringPiece = std::string_view;

// Canonical string_util.h declares this as size_t too (unsigned; compared
// against StringPiece::npos at the call sites).
using stringpiece_ssize_type = size_t;

namespace strings {

inline bool StartsWith(StringPiece text, StringPiece prefix) {
  return text.starts_with(prefix);
}

inline bool EndsWith(StringPiece text, StringPiece suffix) {
  return text.ends_with(suffix);
}

}  // namespace strings

#endif  // PAGESPEED_LIB_JS_COMPAT_STRING_UTIL_H_
