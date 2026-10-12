// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

#include "pagespeed/kernel/packs/planner.h"

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "absl/strings/ascii.h"
#include "absl/strings/str_cat.h"
#include "pagespeed/kernel/packs/kind_registry.h"
#include "pagespeed/kernel/packs/url_norm.h"

namespace pagespeed::packs {

std::optional<PageUrl> ParsePageUrl(std::string_view url) {
  PageUrl out;
  const size_t scheme_end = url.find("://");
  if (scheme_end == std::string_view::npos) return std::nullopt;
  out.scheme = absl::AsciiStrToLower(url.substr(0, scheme_end));
  if (out.scheme != "http" && out.scheme != "https") return std::nullopt;
  url.remove_prefix(scheme_end + 3);
  const size_t frag = url.find('#');
  if (frag != std::string_view::npos) url = url.substr(0, frag);
  const size_t auth_end = url.find_first_of("/?");
  const std::string_view authority = url.substr(0, auth_end);
  std::string_view rest = auth_end == std::string_view::npos
                              ? std::string_view()
                              : url.substr(auth_end);
  if (authority.find('@') != std::string_view::npos) return std::nullopt;
  out.host = NormalizeHost(authority);
  if (out.host.empty()) return std::nullopt;
  const size_t q = rest.find('?');
  std::string_view path = rest.substr(0, q);
  if (q != std::string_view::npos) out.query = std::string(rest.substr(q + 1));
  out.path = path.empty() ? "/" : std::string(path);
  return out;
}

std::string PageUrlString(const PageUrl& url) {
  return absl::StrCat(url.scheme, "://", url.host, url.path,
                      url.query.empty() ? "" : "?", url.query);
}

std::string BaseUrl(const PageUrl& url, const PageFacts& facts) {
  std::string page_url = PageUrlString(url);
  if (!facts.base_href.empty()) {
    std::string b = ResolveUrl(page_url, facts.base_href);
    if (!b.empty() && NormUrl(b).has_value()) return b;
  }
  return page_url;
}

PagePlan BuildPlan(const Pack& pack, const PageContext& ctx,
                   const Selection& sel, const PageFacts& facts,
                   DecisionPoint point, size_t max_added_bytes,
                   size_t extra_bytes_per_change) {
  PagePlan plan;
  if (sel.site == nullptr) {
    plan.skip = SkipReason::kHostNotListed;
    return plan;
  }

  bool any = false;
  for (const RuleKind* kind : AllKinds()) {
    if (kind->decision_point() == point &&
        sel.by_kind[KindIndex(kind)].has_value()) {
      any = true;
    }
  }
  if (!any) return plan;
  if (facts.malformed_head) {
    plan.skip = SkipReason::kMalformedHead;
    return plan;
  }
  if (facts.head == nullptr) {
    plan.skip = SkipReason::kNoHead;
    return plan;
  }

  const std::string base = BaseUrl(ctx.url, facts);

  // The head values the page ends with, for the kinds that quote them:
  // `planned` as if every head rule had been applied, `actual` with only the
  // rules that really are enforcing and were applied. A rule that only
  // reports answers "what would the whole pack do"; an enforcing one must not
  // quote a value that is not in the document.
  struct HeadValues {
    std::string canonical, title, description;
  };
  HeadValues planned, actual;
  if (point == DecisionPoint::kDocumentEnd) {
    for (const RuleKind* kind : AllKinds()) {
      const HeadSlot slot = kind->head_slot();
      if (slot == HeadSlot::kNone) continue;
      const RulePlan* head_rule = nullptr;
      if (ctx.head_plan != nullptr) {
        for (const RulePlan& r : ctx.head_plan->rules) {
          if (r.decision.kind == kind) head_rule = &r;
        }
      }
      const bool applied = head_rule != nullptr && ctx.head_applied &&
                           head_rule->decision.mode == Mode::kEnforce;
      const std::string pv = kind->FinalValue(head_rule, facts.For(kind), base);
      const std::string av =
          applied ? pv : kind->FinalValue(nullptr, facts.For(kind), base);
      HeadValues* both[2] = {&planned, &actual};
      const std::string* values[2] = {&pv, &av};
      for (int i = 0; i < 2; ++i) {
        switch (slot) {
          case HeadSlot::kCanonical:
            both[i]->canonical = *values[i];
            break;
          case HeadSlot::kTitle:
            both[i]->title = *values[i];
            break;
          case HeadSlot::kDescription:
            both[i]->description = *values[i];
            break;
          case HeadSlot::kNone:
            break;
        }
      }
    }
  }

  for (const RuleKind* kind : AllKinds()) {
    if (kind->decision_point() != point) continue;
    const std::optional<RuleHit>& hit = sel.by_kind[KindIndex(kind)];
    if (!hit.has_value()) continue;
    const Rule& rule = *hit->rule;

    RulePlan rp;
    rp.rule = &rule;
    rp.decision.rule_id = rule.id;
    rp.decision.kind = kind;
    rp.decision.mode =
        EffectiveMode(ctx.global_mode, sel.site->mode, rule.enforce);

    PlanInput in;
    in.pack = &pack;
    in.rule = &rule;
    in.url = ctx.url;
    in.captures = hit->captures;
    in.facts = &facts.For(kind);
    in.base = base;
    if (rule.value.source == ValueSource::kTable) {
      if (rule.value.table == kHreflangTableName) {
        in.cluster = FindCluster(pack, PageUrlString(ctx.url));
      } else {
        auto table = pack.tables.find(rule.value.table);
        if (table != pack.tables.end()) {
          in.table_value =
              LookupTableValue(table->second, ctx.url.path, ctx.url.query);
        }
      }
    }
    if (point == DecisionPoint::kDocumentEnd) {
      const HeadValues& fin =
          rp.decision.mode == Mode::kEnforce ? actual : planned;
      in.final_canonical = fin.canonical;
      in.final_title = fin.title;
      in.final_description = fin.description;
    }
    kind->Plan(in, &rp);
    if (!rp.planned) continue;

    size_t removals = 0;
    for (const PlanOp& op : rp.ops) removals += op.type == OpType::kRemove;
    CapRemoved(&rp, removals);
    plan.added_bytes += AddedBytes(rp, extra_bytes_per_change);
    plan.rules.push_back(std::move(rp));
  }

  if (plan.added_bytes > max_added_bytes) {
    plan.skip = SkipReason::kSizeLimit;
    plan.rules.clear();
  }
  return plan;
}

PagePlan BuildPlan(const Pack& pack, const PageContext& ctx,
                   const PageFacts& facts, DecisionPoint point,
                   size_t max_added_bytes, size_t extra_bytes_per_change) {
  const Selection sel = SelectRules(pack, ctx.url.host, ctx.url.path);
  return BuildPlan(pack, ctx, sel, facts, point, max_added_bytes,
                   extra_bytes_per_change);
}

}  // namespace pagespeed::packs
