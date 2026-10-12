// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

#include "pagespeed/kernel/packs/kind_registry.h"

#include <cstddef>

#include "absl/strings/str_cat.h"
#include "base/logging.h"
// One include per row of kind_table.inc (a missing one fails to compile).
#include "pagespeed/kernel/packs/kinds/canonical_kind.h"
#include "pagespeed/kernel/packs/kinds/description_kind.h"
#include "pagespeed/kernel/packs/kinds/hreflang_kind.h"
#include "pagespeed/kernel/packs/kinds/jsonld_kind.h"
#include "pagespeed/kernel/packs/kinds/title_kind.h"

namespace pagespeed::packs {
namespace {

// One explicit table, not self-registration: static initializers would need
// alwayslink on every consumer and their order across linkers is not
// something to depend on.
const std::array<const RuleKind*, kKindCount>& Table() {
  // Function-local statics: constructed on first use, never destroyed.
  static const std::array<const RuleKind*, kKindCount> table = {
#define RULE_KIND(name, cls, header) new cls(),
#include "pagespeed/kernel/packs/kind_table.inc"
#undef RULE_KIND
  };
  return table;
}

}  // namespace

const std::array<const RuleKind*, kKindCount>& AllKinds() { return Table(); }

const RuleKind* FindKind(std::string_view name) {
  for (const RuleKind* kind : Table()) {
    if (kind->name() == name) return kind;
  }
  return nullptr;
}

size_t KindIndex(const RuleKind* kind) {
  const auto& table = Table();
  for (size_t i = 0; i < table.size(); ++i) {
    if (table[i] == kind) return i;
  }
  CHECK(false) << "KindIndex() of a null or foreign RuleKind";
  return kKindCount;
}

size_t KindIndexOf(std::string_view name) {
  const auto& table = Table();
  for (size_t i = 0; i < table.size(); ++i) {
    if (table[i]->name() == name) return i;
  }
  return kKindCount;
}

std::string KindNameList() {
  std::string out;
  for (const RuleKind* kind : Table()) {
    absl::StrAppend(&out, out.empty() ? "" : ", ", kind->name());
  }
  return out;
}

}  // namespace pagespeed::packs
