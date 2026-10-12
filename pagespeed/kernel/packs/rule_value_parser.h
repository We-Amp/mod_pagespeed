// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

#ifndef PAGESPEED_KERNEL_PACKS_RULE_VALUE_PARSER_H_
#define PAGESPEED_KERNEL_PACKS_RULE_VALUE_PARSER_H_

#include <string_view>

#include "absl/status/status.h"
#include "pagespeed/kernel/base/json.h"
#include "pagespeed/kernel/packs/pack.h"
#include "pagespeed/kernel/packs/rule_kind.h"

namespace pagespeed::packs {

// How a kind reads its `value` object: which of "template", "table" and
// "pattern" it accepts, and what it demands of the text. The kind handlers
// fill one in and call ParseRuleValue(), which owns the shared shape checks
// and their messages.
struct ValueSpec {
  bool allow_template = true;
  std::string_view template_denied = {};  // message when !allow_template

  bool allow_pattern = false;  // code -> template map (hreflang)

  bool allow_table = true;
  std::string_view table_denied = {};  // message when !allow_table
  // The table is the hreflang cluster table, and nothing else may be named.
  bool hreflang_table = false;

  // Templates (and table values) may not hold NUL or other control characters.
  bool forbid_control_chars = false;
  // The template is a JSON document that may use page values (jsonld).
  bool json_template = false;

  // Checks one entry of a user table that this kind reads. The returned
  // message is completed by the caller with the user of the table. Null when
  // entries need no check.
  absl::Status (*check_table_value)(std::string_view value) = nullptr;
};

absl::Status ParseRuleValue(const Json::Value& value,
                            const LoadContext& context, const ValueSpec& spec,
                            RuleValue* out);

}  // namespace pagespeed::packs

#endif  // PAGESPEED_KERNEL_PACKS_RULE_VALUE_PARSER_H_
