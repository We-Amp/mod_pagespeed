// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

#ifndef PAGESPEED_KERNEL_PACKS_KINDS_CANONICAL_KIND_H_
#define PAGESPEED_KERNEL_PACKS_KINDS_CANONICAL_KIND_H_

#include <cstddef>
#include <string_view>

#include "absl/status/status.h"
#include "pagespeed/kernel/base/json.h"
#include "pagespeed/kernel/html/html_name.h"
#include "pagespeed/kernel/packs/kinds/head_singleton_kind.h"
#include "pagespeed/kernel/packs/pack.h"
#include "pagespeed/kernel/packs/rule_kind.h"

namespace pagespeed::packs {

// <link rel="canonical">: one URL per page, decided when the head closes.
class CanonicalKind final : public HeadSingletonKind {
 public:
  std::string_view name() const override { return "canonical"; }
  std::string_view lens_defect_prefix() const override { return "canonical"; }
  absl::Status ParseValue(const Json::Value& value, const LoadContext& context,
                          RuleValue* out) const override;
  bool Observe(const ElementView& element, ElementFact* fact) const override;
  HeadSlot head_slot() const override { return HeadSlot::kCanonical; }
  // Repair and replace rewrite the canonical <link href>, never the URL of a
  // resource the page loads, which is what the flag is for (a filter that
  // reports it makes the chain drop the origin's preload and subresource
  // hints).
  bool ModifiesUrls(OnPresent on_present) const override { return false; }
  bool SupportsLinkHeader() const override { return true; }

 protected:
  bool has_unusable_defect() const override { return true; }

  ValueType value_type() const override { return ValueType::kUrl; }
  net_instaweb::HtmlName::Keyword value_attribute() const override {
    return net_instaweb::HtmlName::kHref;
  }
  size_t max_value_length() const override { return kMaxUrlBytes; }
  net_instaweb::HtmlElement* CreateElement(
      net_instaweb::HtmlParse* parser,
      net_instaweb::HtmlElement* parent) const override;
};

}  // namespace pagespeed::packs

#endif  // PAGESPEED_KERNEL_PACKS_KINDS_CANONICAL_KIND_H_
