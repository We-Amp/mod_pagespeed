// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

#ifndef PAGESPEED_KERNEL_PACKS_KINDS_TITLE_KIND_H_
#define PAGESPEED_KERNEL_PACKS_KINDS_TITLE_KIND_H_

#include <cstddef>
#include <string_view>

#include "absl/status/status.h"
#include "pagespeed/kernel/base/json.h"
#include "pagespeed/kernel/html/html_name.h"
#include "pagespeed/kernel/packs/kinds/head_singleton_kind.h"
#include "pagespeed/kernel/packs/pack.h"
#include "pagespeed/kernel/packs/rule_kind.h"

namespace pagespeed::packs {

// <title>: one per page, in the head, decided when the head closes.
class TitleKind final : public HeadSingletonKind {
 public:
  std::string_view name() const override { return "title"; }
  std::string_view lens_defect_prefix() const override { return "title"; }
  absl::Status ParseValue(const Json::Value& value, const LoadContext& context,
                          RuleValue* out) const override;
  bool Observe(const ElementView& element, ElementFact* fact) const override;
  HeadSlot head_slot() const override { return HeadSlot::kTitle; }

 protected:
  ValueType value_type() const override { return ValueType::kHtmlText; }
  net_instaweb::HtmlName::Keyword value_attribute() const override {
    return net_instaweb::HtmlName::kNotAKeyword;
  }
  size_t max_value_length() const override { return kMaxTitleChars; }
  net_instaweb::HtmlElement* CreateElement(
      net_instaweb::HtmlParse* parser,
      net_instaweb::HtmlElement* parent) const override;
};

}  // namespace pagespeed::packs

#endif  // PAGESPEED_KERNEL_PACKS_KINDS_TITLE_KIND_H_
