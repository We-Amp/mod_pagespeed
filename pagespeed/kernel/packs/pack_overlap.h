// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

#ifndef PAGESPEED_KERNEL_PACKS_PACK_OVERLAP_H_
#define PAGESPEED_KERNEL_PACKS_PACK_OVERLAP_H_

#include <cstddef>
#include <optional>
#include <string>
#include <vector>

#include "pagespeed/kernel/packs/pack.h"
#include "pagespeed/kernel/packs/rule_kind.h"

namespace pagespeed::packs {

// The mode `pack` can run `kind` in on a host with the directive mode
// `directive` (site mode, the rule's own opt-in); nullopt when the pack has
// no enabled rule of the kind.
std::optional<Mode> KindMode(const Pack& pack, const RuleKind* kind,
                             Mode directive);

// One kind that two packs of a host both carry, in different modes.
struct PackOverlap {
  size_t first = 0;  // index into the list given to FindOverlaps()
  size_t later = 0;
  const RuleKind* kind = nullptr;
  Mode first_mode = Mode::kReport;
  Mode later_mode = Mode::kReport;
};

// The kinds that an earlier pack owns (first pack wins) while a later pack
// carries rules of it that run in another mode, ordered by (first, later,
// kind application order). Packs that share a kind in the same mode are not
// reported.
std::vector<PackOverlap> FindOverlaps(const std::vector<const Pack*>& packs,
                                      Mode directive);

// The warning text for `overlap` between `first` and `later`.
std::string OverlapMessage(const Pack& first, const Pack& later,
                           const PackOverlap& overlap);

}  // namespace pagespeed::packs

#endif  // PAGESPEED_KERNEL_PACKS_PACK_OVERLAP_H_
