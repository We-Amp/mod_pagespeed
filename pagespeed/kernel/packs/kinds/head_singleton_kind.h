// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

#ifndef PAGESPEED_KERNEL_PACKS_KINDS_HEAD_SINGLETON_KIND_H_
#define PAGESPEED_KERNEL_PACKS_KINDS_HEAD_SINGLETON_KIND_H_

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "pagespeed/kernel/html/html_name.h"
#include "pagespeed/kernel/packs/pack.h"
#include "pagespeed/kernel/packs/planner_common.h"
#include "pagespeed/kernel/packs/rule_kind.h"

namespace pagespeed::packs {

// The base of the kinds a page should carry exactly once in <head>
// (canonical, title, description). They share the keep / repair / replace
// ladder:
//
//   keep     act only when the kind is absent; otherwise remove exact
//            duplicates and nothing else;
//   repair   make sure one usable element exists in <head>; leave a usable
//            one alone, whatever its value;
//   replace  the document ends with exactly the rule's value.
//
// A subclass says how its elements look (Observe, CreateElement) and how its
// value is written and compared (the traits below).
class HeadSingletonKind : public RuleKind {
 public:
  DecisionPoint decision_point() const final { return DecisionPoint::kHeadEnd; }

  void Plan(const PlanInput& input, RulePlan* out) const final;
  LateAction OnLateElement(const PlanState& state,
                           const ElementFact& late) const final;
  void BuildInsert(net_instaweb::HtmlParse* parser,
                   net_instaweb::HtmlElement* parent, const PlanOp& op,
                   std::vector<net_instaweb::HtmlNode*>* out) const final;

  std::string FinalValue(const RulePlan* plan,
                         const std::vector<ElementFact>& facts,
                         std::string_view base) const final;

  // How an element's value compares: NormUrl() of the resolved href for a
  // URL, the whitespace-collapsed decoded text otherwise. Empty when blank or
  // unusable.
  std::string NormOf(const ElementFact& fact, std::string_view base) const;

 protected:
  enum class ValueType : std::uint8_t {
    kUrl,       // an absolute URL in an attribute (canonical)
    kText,      // text in an attribute (description)
    kHtmlText,  // the element's text content (title)
  };
  virtual ValueType value_type() const = 0;
  // The attribute that carries the value; kNotAKeyword for text content.
  virtual net_instaweb::HtmlName::Keyword value_attribute() const = 0;
  // Bound on the expanded value: bytes for a URL, characters for text.
  virtual size_t max_value_length() const = 0;
  // An element without a usable value has its own defect id ("-unusable");
  // otherwise it counts as missing.
  virtual bool has_unusable_defect() const { return false; }
  // A new element of the kind, without its value.
  virtual net_instaweb::HtmlElement* CreateElement(
      net_instaweb::HtmlParse* parser,
      net_instaweb::HtmlElement* parent) const = 0;
};

}  // namespace pagespeed::packs

#endif  // PAGESPEED_KERNEL_PACKS_KINDS_HEAD_SINGLETON_KIND_H_
