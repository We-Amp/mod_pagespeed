// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.
//
// Compat header for the vendored 1.15 HTML kernel (#1130): stands in
// for the canonical pagespeed/kernel/base/string_hash.h header, supplying the
// case-folding hash functors the vendored html_keywords.h names in its
// lookup-map types. See lib/html/CLAUDE.md.
//
// The hash algorithm mirrors canonical HashString (result * 131 + folded
// char) exactly. Hash VALUES are process-internal (bucket placement only) —
// no parse event can observe them — so even a divergence here could not
// affect output; matching canonical anyway keeps future ports trivial.
// CaseFoldStringEqual is expressed over lib/base's StringCaseEqual, which is
// MemCaseEqual's exact contract (size equality + case-insensitive compare).
//
// The functors pass the HashString result through absl::HashOf, as canonical
// does: absl's Swiss tables do not mix a custom hasher's output, and unmixed
// short keys would all share one probe tag.

#ifndef PAGESPEED_LIB_HTML_COMPAT_STRING_HASH_H_
#define PAGESPEED_LIB_HTML_COMPAT_STRING_HASH_H_

#include <cstddef>

#include "absl/hash/hash.h"
#include "lib/base/string_util.h"
#include "lib/html/compat/string.h"

namespace net_instaweb {

// Canonical HashString (pagespeed/kernel/base/string_hash.h): the Chromium
// hash_tables.h recurrence, result * 131 + Normalize(c).
template <class CharTransform, typename IntType>
inline IntType HashString(const char* s, size_t len) {
  IntType result = 0;
  for (const char* end = s + len; s != end; ++s) {
    result = (result * 131) + CharTransform::Normalize(*s);
  }
  return result;
}

// Case-sensitive folding: identity. Unsigned-char return so the hash
// arithmetic is signedness-independent (canonical comment).
struct CasePreserve {
  static unsigned char Normalize(char c) { return c; }
};

// Case-insensitive folding to lowercase.
struct CaseFold {
  static unsigned char Normalize(char c) { return LowerChar(c); }
};

struct CasePreserveStringHash {
  size_t operator()(const GoogleString& str) const {
    return absl::HashOf(
        HashString<CasePreserve, size_t>(str.data(), str.size()));
  }
};

struct CaseFoldStringHash {
  size_t operator()(const GoogleString& str) const {
    return absl::HashOf(HashString<CaseFold, size_t>(str.data(), str.size()));
  }
};

struct CaseFoldStringEqual {
  bool operator()(const GoogleString& a, const GoogleString& b) const {
    return StringCaseEqual(a, b);
  }
};

}  // namespace net_instaweb

#endif  // PAGESPEED_LIB_HTML_COMPAT_STRING_HASH_H_
