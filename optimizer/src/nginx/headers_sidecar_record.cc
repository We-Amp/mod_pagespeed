// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

#include "src/nginx/headers_sidecar_record.h"

#include <cstddef>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

#include "lib/cache/headers_sidecar.h"
#include "lib/classify/alternate_metadata.h"

namespace pagespeed {

namespace {

constexpr std::string_view kUnreproduciblePrefix = "unreconstructible-header:";

char AsciiLower(char c) {
  return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
}

}  // namespace

bool HeaderNameEquals(std::string_view a, std::string_view b) {
  if (a.size() != b.size()) return false;
  for (size_t i = 0; i < a.size(); ++i) {
    if (AsciiLower(a[i]) != AsciiLower(b[i])) return false;
  }
  return true;
}

std::string_view FirstUnreproducibleHeader(
    const SidecarClassification& classification) {
  for (const auto& reason : classification.reasons) {
    std::string_view r(reason);
    if (r.starts_with(kUnreproduciblePrefix)) {
      return r.substr(kUnreproduciblePrefix.size());
    }
  }
  return {};
}

RestampSidecarPlan PlanRestampSidecar(
    uint8_t stored_flags, std::optional<std::string_view> stored_payload,
    std::span<const HeaderField> response_304) {
  RestampSidecarPlan plan;
  if ((stored_flags & AlternateMetadata::kFlagHeadersClassified) == 0) {
    plan.action = RestampSidecarAction::kLegacy;
    return plan;
  }
  const bool stored_required =
      (stored_flags & AlternateMetadata::kFlagHeadersSidecarRequired) != 0;

  std::vector<HeaderField> stored_fields;
  if (stored_required) {
    std::optional<std::vector<HeaderField>> parsed;
    if (stored_payload.has_value()) {
      parsed = ParseHeadersSidecar(*stored_payload);
    }
    if (!parsed.has_value()) {
      // The entry says a block is required and there is none to merge with:
      // the merge cannot be computed, so nothing may be restamped.  Dropping
      // the URL lets the next request re-record it with a block.
      plan.action = RestampSidecarAction::kRemove;
      return plan;
    }
    stored_fields = std::move(*parsed);
  }

  // RFC 9111 §4.3.4: a stored field is replaced by the 304's lines of the
  // same name; a name the 304 does not carry keeps its stored lines.
  for (const auto& field : stored_fields) {
    bool overridden = false;
    for (const auto& line : response_304) {
      if (HeaderNameEquals(field.name, line.name)) {
        overridden = true;
        break;
      }
    }
    if (!overridden) {
      plan.merged.push_back(field);
      plan.stored_not_overridden.push_back(field);
    }
  }
  plan.merged.insert(plan.merged.end(), response_304.begin(),
                     response_304.end());

  plan.classification = ClassifyForHeadersSidecar(plan.merged);
  if (!plan.classification.swap_eligible()) {
    plan.action = RestampSidecarAction::kRemove;
    return plan;
  }
  const bool carry = !plan.classification.payload.empty();
  if (carry != stored_required) {
    // A carry-state switch cannot be merged: the 304 restamps only the
    // stashed variant, and its siblings would keep the old bits.
    plan.action = RestampSidecarAction::kRemove;
    return plan;
  }
  if (!carry || plan.classification.payload == *stored_payload) {
    plan.action = RestampSidecarAction::kRestampOnly;
    return plan;
  }
  plan.action = RestampSidecarAction::kRewriteThenRestamp;
  return plan;
}

}  // namespace pagespeed
