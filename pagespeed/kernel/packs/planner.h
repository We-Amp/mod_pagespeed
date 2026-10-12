// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

#ifndef PAGESPEED_KERNEL_PACKS_PLANNER_H_
#define PAGESPEED_KERNEL_PACKS_PLANNER_H_

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "pagespeed/kernel/packs/matcher.h"
#include "pagespeed/kernel/packs/pack.h"
#include "pagespeed/kernel/packs/page_facts.h"
#include "pagespeed/kernel/packs/planner_common.h"
#include "pagespeed/kernel/packs/rule_kind.h"
#include "pagespeed/kernel/packs/template.h"

namespace pagespeed::packs {

struct PagePlan;

// What is known about the page being rewritten, besides its HTML.
struct PageContext {
  PageUrl url;
  Mode global_mode = Mode::kReport;
  // For the kinds that decide at the end of the document: the plan made when
  // the head closed (null when none was made), and whether it was applied.
  // They quote the values the document ends with, so a plan that was dropped
  // (the head could not be edited) counts for nothing.
  const PagePlan* head_plan = nullptr;
  bool head_applied = true;
};

// The plans of one decision point of one page.
struct PagePlan {
  SkipReason skip = SkipReason::kNone;
  std::vector<RulePlan> rules;  // in registry order; one per applicable rule
  size_t added_bytes = 0;
};

// Splits an absolute http(s) page URL into scheme, host (lowercased, port
// stripped), path (starts with '/') and query (without '?'). The fragment is
// dropped. Returns nullopt for anything else.
std::optional<PageUrl> ParsePageUrl(std::string_view url);

// "scheme://host/path?query".
std::string PageUrlString(const PageUrl& url);

// What relative URLs in the page resolve against: the page's first
// <base href> when it resolves to a usable URL, else the page URL.
std::string BaseUrl(const PageUrl& url, const PageFacts& facts);

// Pure: decides, from the pack, the page URL, the rules selected for it and
// the facts collected from the HTML so far, what each rule whose kind decides
// at `point` does on this page. Nothing here touches HTML.
//
// The point is skipped whole (no rule plans) when the host is not listed,
// when no explicit <head> exists, when the head cannot be edited safely, or
// when the bytes to add exceed `max_added_bytes`.
//
// `extra_bytes_per_change` is added to the byte count of every insert or
// value change (room for the debug comment the filter may write).
PagePlan BuildPlan(const Pack& pack, const PageContext& ctx,
                   const Selection& selection, const PageFacts& facts,
                   DecisionPoint point,
                   size_t max_added_bytes = kMaxAddedBytesPerPage,
                   size_t extra_bytes_per_change = 0);

// Same, selecting the rules for the page first.
PagePlan BuildPlan(const Pack& pack, const PageContext& ctx,
                   const PageFacts& facts,
                   DecisionPoint point = DecisionPoint::kHeadEnd,
                   size_t max_added_bytes = kMaxAddedBytesPerPage,
                   size_t extra_bytes_per_change = 0);

}  // namespace pagespeed::packs

#endif  // PAGESPEED_KERNEL_PACKS_PLANNER_H_
