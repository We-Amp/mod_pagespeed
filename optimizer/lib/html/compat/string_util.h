// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.
//
// Compat header for the vendored 1.15 HTML kernel (#1130): stands in
// for the canonical pagespeed/kernel/base/string_util.h header, supplying the
// exact surface the vendored files consume. See lib/html/CLAUDE.md.
//
// SHARED PRIMITIVES COME FROM lib/js/compat/string_util.h — included, not
// redefined. StringPiece, stringpiece_ssize_type, and strings::{StartsWith,
// EndsWith} must have exactly ONE definition repo-wide: src/worker/worker.cc
// includes both kernels' public headers (js_minify.h and html_parse.h), so
// both compat sets appear in one TU there. Identical inline-function
// redefinitions would be a hard error, and `using absl::StartsWith;` here
// (the canonical form) ILL-FORMEDLY conflicts with the D1 inline functions
// (verified with clang: "target of using declaration conflicts with
// declaration already in scope"). Including the D1 header is the only
// order-independent resolution; lib/js is pinned/stable and this is a
// read-only dependency (lib/js/** is never modified from here).
//
// The rest mirrors the canonical declarations the synced set consumes:
//   * `using absl::StrAppend; using absl::StrCat;` at global scope, exactly
//     as canonical string_util.h has them (mpp adopted absl upstream);
//   * net_instaweb aliases/helpers: StringVector, StringPieceVector,
//     StringSetInsensitive (+ StringCompareInsensitive),
//     SplitStringPieceToVector, TrimWhitespace, LowerString/UpperString.
// TrimWhitespace delegates to lib/base's TrimHtmlWhitespace — verified
// line-by-line identical semantics to canonical TrimWhitespace (both trim
// IsHtmlSpace leading+trailing and report whether anything was trimmed).

#ifndef PAGESPEED_LIB_HTML_COMPAT_STRING_UTIL_H_
#define PAGESPEED_LIB_HTML_COMPAT_STRING_UTIL_H_

#include <set>
#include <string>
#include <string_view>
#include <vector>

#include "absl/strings/str_cat.h"
#include "absl/strings/str_format.h"
#include "lib/base/string_util.h"
#include "lib/html/compat/string.h"
// Single-definition source for StringPiece / stringpiece_ssize_type /
// strings::{StartsWith,EndsWith} (see the file-top comment).
#include "lib/js/compat/string_util.h"

// Canonical string_util.h declares these exact using-declarations at global
// scope (mpp #654 adopted absl). Repeated using-declarations of the SAME
// entity are legal, so a TU that also pulls absl directly is unaffected.
using absl::StrAppend;
using absl::StrCat;

// Canonical string_util.h also defines this macro (guarded, "Match Envoy's
// definition exactly to avoid redefinition warnings") — the vendored
// html_lexer.cc uses it for its literal-tag tables.
#ifndef STATIC_STRLEN
#define STATIC_STRLEN(X) (sizeof(X) - 1)
#endif

namespace net_instaweb {

using StringVector = std::vector<GoogleString>;
using StringPieceVector = std::vector<StringPiece>;

// Canonical string_util.h declares this functor (definition lives in the
// canonical .cc); StringSetInsensitive is std::set with it, NOT an unordered
// set — the canonical ordering guarantee is kept so nothing downstream can
// ever observe a hash-order difference.
struct StringCompareInsensitive {
  bool operator()(const GoogleString& a, const GoogleString& b) const {
    return StringCaseCompare(a, b) < 0;
  }
};
using StringSetInsensitive = std::set<GoogleString, StringCompareInsensitive>;

// Split sp into pieces separated by any character in |separators|, in order,
// onto |components|. Mirrors the canonical implementation statement-for-
// statement (including the final-tail push, where the canonical
// `prev_pos - sp.size()` underflow clamps to the tail — keep it identical).
inline void SplitStringPieceToVector(StringPiece sp, StringPiece separators,
                                     StringPieceVector* components,
                                     bool omit_empty_strings) {
  size_t prev_pos = 0;
  size_t pos = 0;
  while ((pos = sp.find_first_of(separators, pos)) != StringPiece::npos) {
    if (!omit_empty_strings || (pos > prev_pos)) {
      components->push_back(sp.substr(prev_pos, pos - prev_pos));
    }
    ++pos;
    prev_pos = pos;
  }
  if (!omit_empty_strings || (prev_pos < sp.size())) {
    components->push_back(sp.substr(prev_pos, prev_pos - sp.size()));
  }
}

// Canonical TrimWhitespace trims IsHtmlSpace from both ends and reports
// whether anything was trimmed — exactly lib/base's TrimHtmlWhitespace.
inline bool TrimWhitespace(StringPiece* str) { return TrimHtmlWhitespace(str); }

// Protobuf-derived helpers, as in canonical string_util.cc.
inline void LowerString(GoogleString* s) {
  for (char& c : *s) {
    c = LowerChar(c);
  }
}

inline void UpperString(GoogleString* s) {
  for (char& c : *s) {
    c = UpperChar(c);
  }
}

}  // namespace net_instaweb

#endif  // PAGESPEED_LIB_HTML_COMPAT_STRING_UTIL_H_
