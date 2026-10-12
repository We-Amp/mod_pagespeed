// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

#ifndef PAGESPEED_KERNEL_PACKS_KINDS_HREFLANG_KIND_H_
#define PAGESPEED_KERNEL_PACKS_KINDS_HREFLANG_KIND_H_

#include <string_view>
#include <vector>

#include "absl/status/status.h"
#include "pagespeed/kernel/base/json.h"
#include "pagespeed/kernel/packs/pack.h"
#include "pagespeed/kernel/packs/rule_kind.h"

namespace pagespeed::packs {

// <link rel="alternate" hreflang>: a cluster of alternates, decided when the
// head closes.
//
//   keep     insert the whole cluster when the page has no alternate link at
//            all; remove exact duplicates; otherwise only observe (an invalid
//            entry, no entry for the page, two hrefs for one code);
//   repair   drop entries with an invalid code, a relative or empty href or
//            outside <head>, and a repeated code; add the entries that are
//            missing, the page's own among them; when every code of the page
//            points elsewhere, point the first at the page;
//   replace  the document ends with exactly the rule's entries.
//
// Never emits a cluster without an entry for the page, a code the language
// code check rejects, or an href that is not an absolute URL.
class HreflangKind final : public RuleKind {
 public:
  std::string_view name() const override { return "hreflang"; }
  DecisionPoint decision_point() const override {
    return DecisionPoint::kHeadEnd;
  }
  std::string_view lens_defect_prefix() const override { return "hreflang"; }
  absl::Status ParseValue(const Json::Value& value, const LoadContext& context,
                          RuleValue* out) const override;

  // An alternate link with an hreflang attribute and no other rel token
  // (such a link also means something else and is never touched).
  bool Observe(const ElementView& element, ElementFact* fact) const override;
  void Plan(const PlanInput& input, RulePlan* out) const override;
  // repair and replace remove an alternate link that arrives after the head
  // closed, as the ladder removes one outside <head>. Under keep, when the
  // pack inserted the cluster, a late link for a code of the cluster is
  // removed (a second answer for that code would leave the page worse than
  // the input); any other late link is left alone.
  LateAction OnLateElement(const PlanState& state,
                           const ElementFact& late) const override;
  void BuildInsert(net_instaweb::HtmlParse* parser,
                   net_instaweb::HtmlElement* parent, const PlanOp& op,
                   std::vector<net_instaweb::HtmlNode*>* out) const override;
  // The kind writes alternate-language <link href> elements, but it never
  // rewrites the URL of a resource the page loads, which is what the flag is
  // for (a filter that reports it makes the chain drop the origin's preload
  // and subresource hints).
  bool ModifiesUrls(OnPresent on_present) const override { return false; }
};

}  // namespace pagespeed::packs

#endif  // PAGESPEED_KERNEL_PACKS_KINDS_HREFLANG_KIND_H_
