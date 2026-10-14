// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

#include "pagespeed/kernel/packs/pack_overlap.h"

#include <cstddef>
#include <optional>
#include <string>
#include <vector>

#include "absl/strings/str_cat.h"
#include "pagespeed/kernel/packs/kind_registry.h"
#include "pagespeed/kernel/packs/pack.h"
#include "pagespeed/kernel/packs/rule_kind.h"

namespace pagespeed::packs {

std::optional<Mode> KindMode(const Pack& pack, const RuleKind* kind,
                             Mode directive) {
  std::optional<Mode> best;
  for (const Rule& rule : pack.rules) {
    if (!rule.enabled || rule.kind != kind) continue;
    for (const Site& site : pack.sites) {
      const Mode m = EffectiveMode(directive, site.mode, rule.enforce);
      if (!best.has_value() || m == Mode::kEnforce) best = m;
    }
  }
  return best;
}

std::vector<PackOverlap> FindOverlaps(const std::vector<const Pack*>& packs,
                                      Mode directive) {
  std::vector<PackOverlap> overlaps;
  for (size_t a = 0; a < packs.size(); ++a) {
    for (size_t b = a + 1; b < packs.size(); ++b) {
      for (const RuleKind* kind : AllKinds()) {
        const auto first_mode = KindMode(*packs[a], kind, directive);
        const auto later_mode = KindMode(*packs[b], kind, directive);
        if (!first_mode.has_value() || !later_mode.has_value() ||
            *first_mode == *later_mode) {
          continue;
        }
        overlaps.push_back({a, b, kind, *first_mode, *later_mode});
      }
    }
  }
  return overlaps;
}

std::string OverlapMessage(const Pack& first, const Pack& later,
                           const PackOverlap& overlap) {
  return absl::StrCat(
      "rule pack overlap: ", overlap.kind->name(), " owned by ", first.info.id,
      " (", ModeName(overlap.first_mode), ") before ", later.info.id, " (",
      ModeName(overlap.later_mode), "); the later pack will not act on it");
}

}  // namespace pagespeed::packs
