// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

#include "pagespeed/kernel/packs/kinds/canonical_kind.h"

#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "absl/strings/ascii.h"
#include "absl/strings/match.h"
#include "absl/strings/str_split.h"
#include "pagespeed/kernel/html/html_element.h"
#include "pagespeed/kernel/html/html_parse.h"
#include "pagespeed/kernel/packs/element_fact.h"
#include "pagespeed/kernel/packs/planner_common.h"
#include "pagespeed/kernel/packs/rule_value_parser.h"

namespace pagespeed::packs {
namespace {

absl::Status CheckCanonicalTableValue(std::string_view value) {
  return CheckUrlValue(value);
}

}  // namespace

absl::Status CanonicalKind::ParseValue(const Json::Value& value,
                                       const LoadContext& context,
                                       RuleValue* out) const {
  ValueSpec spec;
  spec.check_table_value = &CheckCanonicalTableValue;
  return ParseRuleValue(value, context, spec, out);
}

bool CanonicalKind::Observe(const ElementView& view, ElementFact* fact) const {
  if (view.keyword != net_instaweb::HtmlName::kLink) return false;
  bool has_rel = false;
  const std::string rel =
      AttrValue(view.element, net_instaweb::HtmlName::kRel, &has_rel);
  if (!has_rel) return false;
  bool canonical = false;
  std::vector<std::string> others;
  for (std::string_view tok :
       absl::StrSplit(rel, absl::ByAnyChar(" \t\n\r\f"), absl::SkipEmpty())) {
    if (absl::EqualsIgnoreCase(tok, "canonical")) {
      canonical = true;
    } else {
      others.emplace_back(tok);
    }
  }
  if (!canonical) return false;
  fact->element = view.element;
  fact->in_head = view.in_head;
  fact->value = AttrValue(view.element, net_instaweb::HtmlName::kHref,
                          &fact->has_value_attr);
  fact->other_rel_tokens = std::move(others);
  return true;
}

net_instaweb::HtmlElement* CanonicalKind::CreateElement(
    net_instaweb::HtmlParse* parser, net_instaweb::HtmlElement* parent) const {
  net_instaweb::HtmlElement* link =
      parser->NewElement(parent, net_instaweb::HtmlName::kLink);
  parser->AddEscapedAttribute(link, net_instaweb::HtmlName::kRel, "canonical");
  return link;
}

}  // namespace pagespeed::packs
