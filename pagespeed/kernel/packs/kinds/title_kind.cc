// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

#include "pagespeed/kernel/packs/kinds/title_kind.h"

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

absl::Status CheckTitleTableValue(std::string_view value) {
  if (HasControlChars(value)) {
    return absl::InvalidArgumentError(
        "a title cannot contain NUL or other control characters");
  }
  if (value.empty() || Utf8Length(value) > kMaxTitleChars) {
    return absl::InvalidArgumentError(
        absl::StrCat("a title must be 1 to ", kMaxTitleChars, " characters"));
  }
  return absl::OkStatus();
}

}  // namespace

absl::Status TitleKind::ParseValue(const Json::Value& value,
                                   const LoadContext& context,
                                   RuleValue* out) const {
  ValueSpec spec;
  spec.forbid_control_chars = true;
  spec.check_table_value = &CheckTitleTableValue;
  return ParseRuleValue(value, context, spec, out);
}

bool TitleKind::Observe(const ElementView& view, ElementFact* fact) const {
  if (view.keyword != net_instaweb::HtmlName::kTitle) return false;
  fact->element = view.element;
  fact->in_head = view.in_head;
  fact->text_mode = TextMode::kDecoded;
  return true;
}

net_instaweb::HtmlElement* TitleKind::CreateElement(
    net_instaweb::HtmlParse* parser, net_instaweb::HtmlElement* parent) const {
  return parser->NewElement(parent, net_instaweb::HtmlName::kTitle);
}

}  // namespace pagespeed::packs
