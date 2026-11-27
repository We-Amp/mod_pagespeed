// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.
//
// Compat header for the vendored 1.15 JS kernel: stands in for
// the canonical pagespeed/kernel/util/re2.h header. Supplies the RE2 alias
// surface the vendored files consume: Re2StringPiece, the
// StringPieceToRe2/Re2ToStringPiece converters, and re2::posix_syntax. With
// the 2.0 toolchain both StringPiece (lib/js/compat/string_util.h) and
// RE2's string-piece type are std::string_view, so the converters are
// identity functions kept purely so the vendored call sites compile
// unmodified. See lib/js/CLAUDE.md.

#ifndef PAGESPEED_LIB_JS_COMPAT_RE2_H_
#define PAGESPEED_LIB_JS_COMPAT_RE2_H_

#include <string_view>

#include "lib/js/compat/string_util.h"
#include "re2/re2.h"

using re2::RE2;

namespace re2 {
inline constexpr RE2::CannedOptions posix_syntax = RE2::POSIX;
}  // namespace re2

using Re2StringPiece = std::string_view;

inline Re2StringPiece StringPieceToRe2(StringPiece sp) { return sp; }

inline StringPiece Re2ToStringPiece(Re2StringPiece sp) { return sp; }

#endif  // PAGESPEED_LIB_JS_COMPAT_RE2_H_
