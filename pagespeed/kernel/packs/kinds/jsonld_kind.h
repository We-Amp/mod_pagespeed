// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

#ifndef PAGESPEED_KERNEL_PACKS_KINDS_JSONLD_KIND_H_
#define PAGESPEED_KERNEL_PACKS_KINDS_JSONLD_KIND_H_

#include <string_view>
#include <vector>

#include "absl/status/status.h"
#include "pagespeed/kernel/base/json.h"
#include "pagespeed/kernel/packs/pack.h"
#include "pagespeed/kernel/packs/rule_kind.h"

namespace pagespeed::packs {

// <script type="application/ld+json">: structured data anywhere in the
// document, decided when the document ends with every block in view and the
// head values the document ends with ({canonical}, {title}, {description}).
//
// A block belongs to the rule when its top-level @type values (the members of
// a top-level @graph included) overlap the template's.
//
//   keep     insert when no block of the type exists; stand down when a block
//            does not parse; remove exact duplicates;
//   repair   remove blocks that do not parse and exact duplicates; insert
//            when no block of the type remains;
//   replace  exactly one block with the rule's content, rewritten in place;
//            only blocks whose types are all among the rule's are rewritten
//            or removed, and a block that shares a type but carries others
//            makes the rule stand down (conflict).
//
// A block is removed or rewritten through its own events, so one that an
// earlier flush window already sent cannot be: the rule records
// not_rewritable instead. A new block is appended to <body> (before </body>),
// else to <html>.
class JsonLdKind final : public RuleKind {
 public:
  std::string_view name() const override { return "jsonld"; }
  DecisionPoint decision_point() const override {
    return DecisionPoint::kDocumentEnd;
  }
  std::string_view lens_defect_prefix() const override { return "jsonld"; }
  absl::Status ParseValue(const Json::Value& value, const LoadContext& context,
                          RuleValue* out) const override;

  bool Observe(const ElementView& element, ElementFact* fact) const override;
  void Plan(const PlanInput& input, RulePlan* out) const override;
  // The kind decides at the end of the document, after every block: there
  // are no late elements.
  LateAction OnLateElement(const PlanState& state,
                           const ElementFact& late) const override {
    return LateAction();
  }
  void BuildInsert(net_instaweb::HtmlParse* parser,
                   net_instaweb::HtmlElement* parent, const PlanOp& op,
                   std::vector<net_instaweb::HtmlNode*>* out) const override;
  // The rule adds a <script type="application/ld+json"> element (a data
  // block, but a script element all the same), so a chain that treats script
  // injection specially must be told.
  bool MayInjectScripts() const override { return true; }
};

}  // namespace pagespeed::packs

#endif  // PAGESPEED_KERNEL_PACKS_KINDS_JSONLD_KIND_H_
