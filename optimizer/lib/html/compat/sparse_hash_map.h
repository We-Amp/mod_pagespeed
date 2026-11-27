// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.
//
// Compat header for the vendored 1.15 HTML kernel (#1130): stands in
// for the canonical pagespeed/kernel/base/sparse_hash_map.h header (the
// gperftools sparse_hash_map). The vendored html_keywords.{h,cc} uses it for
// the entity escape/unescape lookup maps.
//
// std::unordered_map is the backing store (the pre-vendoring 2.0 kernel made
// the same substitution). Two semantic notes:
//   * The maps are lookup-only after construction — no iteration order ever
//     reaches a parse event, so the sparse->unordered container swap cannot
//     change observable output (D21: cross-repo goldens must never compare
//     hash-derived ordering; nothing here produces any).
//   * set_deleted_key/set_empty_key are gperftools marker APIs with NO
//     std::unordered_map equivalent; they exist to reserve a key value the
//     container uses internally. Canonical html_keywords.cc calls
//     set_deleted_key("") at init, and the escape-sequence keys it stores
//     are never empty, so the no-op form here is behavior-identical. A
//     subclass (not an alias) is used precisely so these calls still
//     compile — the sync tool makes no semantic rewrites.
//
// Global namespace, matching gperftools' own declaration.

#ifndef PAGESPEED_LIB_HTML_COMPAT_SPARSE_HASH_MAP_H_
#define PAGESPEED_LIB_HTML_COMPAT_SPARSE_HASH_MAP_H_

#include <unordered_map>

template <class K, class V, class Hash, class Eq = std::equal_to<K>>
class sparse_hash_map : public std::unordered_map<K, V, Hash, Eq> {
 public:
  using Base = std::unordered_map<K, V, Hash, Eq>;
  using Base::Base;

  // gperftools deleted/empty-key markers; meaningless for
  // std::unordered_map. See the file-top comment for why the no-op is safe.
  void set_deleted_key(const K&) {}
  void set_empty_key(const K&) {}
};

#endif  // PAGESPEED_LIB_HTML_COMPAT_SPARSE_HASH_MAP_H_
