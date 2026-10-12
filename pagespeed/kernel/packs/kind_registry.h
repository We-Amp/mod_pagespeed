// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

#ifndef PAGESPEED_KERNEL_PACKS_KIND_REGISTRY_H_
#define PAGESPEED_KERNEL_PACKS_KIND_REGISTRY_H_

#include <array>
#include <cstddef>
#include <string>
#include <string_view>

#include "pagespeed/kernel/packs/rule_kind.h"

namespace pagespeed::packs {

// Number of kinds in kind_table.inc.
inline constexpr size_t kKindCount = 0
#define RULE_KIND(name, cls, header) +1
#include "pagespeed/kernel/packs/kind_table.inc"
#undef RULE_KIND
    ;

// The kinds in application order.
const std::array<const RuleKind*, kKindCount>& AllKinds();

// The kind named `name` in a pack file, or null.
const RuleKind* FindKind(std::string_view name);

// Position of `kind` in AllKinds(). `kind` must come from this registry; a
// null or foreign kind is a fatal error (CHECK).
size_t KindIndex(const RuleKind* kind);
// Same, by name; kKindCount when there is no such kind.
size_t KindIndexOf(std::string_view name);

// "canonical, title, description, hreflang, jsonld" -- for messages.
std::string KindNameList();

}  // namespace pagespeed::packs

#endif  // PAGESPEED_KERNEL_PACKS_KIND_REGISTRY_H_
