// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

#include "pagespeed/kernel/packs/kinds/description_kind.h"

#include <string>

#include "absl/strings/ascii.h"
#include "absl/strings/match.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/str_split.h"
#include "pagespeed/kernel/html/html_element.h"
#include "pagespeed/kernel/html/html_parse.h"
#include "pagespeed/kernel/packs/element_fact.h"
#include "pagespeed/kernel/packs/planner_common.h"
#include "pagespeed/kernel/packs/rule_value_parser.h"

namespace pagespeed::packs {
namespace {

absl::Status CheckDescriptionTableValue(std::string_view value) {
  if (HasControlChars(value)) {
    return absl::InvalidArgumentError(
        "a description cannot contain NUL or other control characters");
  }
  if (value.empty() || Utf8Length(value) > kMaxDescriptionChars) {
    return absl::InvalidArgumentError(absl::StrCat(
        "a description must be 1 to ", kMaxDescriptionChars, " characters"));
  }
  return absl::OkStatus();
}

}  // namespace

absl::Status DescriptionKind::ParseValue(const Json::Value& value,
                                         const LoadContext& context,
                                         RuleValue* out) const {
  ValueSpec spec;
  spec.forbid_control_chars = true;
  spec.check_table_value = &CheckDescriptionTableValue;
  return ParseRuleValue(value, context, spec, out);
}

bool DescriptionKind::Observe(const ElementView& view,
                              ElementFact* fact) const {
  if (view.keyword != net_instaweb::HtmlName::kMeta) return false;
  const std::string name =
      AttrValue(view.element, net_instaweb::HtmlName::kName);
  if (!absl::EqualsIgnoreCase(name, "description")) return false;
  fact->element = view.element;
  fact->in_head = view.in_head;
  fact->value = AttrValue(view.element, net_instaweb::HtmlName::kContent,
                          &fact->has_value_attr);
  return true;
}

net_instaweb::HtmlElement* DescriptionKind::CreateElement(
    net_instaweb::HtmlParse* parser, net_instaweb::HtmlElement* parent) const {
  net_instaweb::HtmlElement* meta =
      parser->NewElement(parent, net_instaweb::HtmlName::kMeta);
  parser->AddEscapedAttribute(meta, net_instaweb::HtmlName::kName,
                              "description");
  return meta;
}

}  // namespace pagespeed::packs
