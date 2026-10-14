// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

#include "pagespeed/kernel/packs/pack_host.h"

namespace pagespeed::packs {

std::string_view CounterName(Counter counter) {
  switch (counter) {
    case Counter::kLoads:
      return "rule_pack_loads";
    case Counter::kLoadErrors:
      return "rule_pack_load_errors";
    case Counter::kPagesEvaluated:
      return "rule_pack_pages_evaluated";
    case Counter::kPagesModified:
      return "rule_pack_pages_modified";
    case Counter::kPagesWouldModify:
      return "rule_pack_pages_would_modify";
    case Counter::kPagesSkippedHostNotListed:
      return "rule_pack_pages_skipped_host_not_listed";
    case Counter::kPagesSkippedNoHead:
      return "rule_pack_pages_skipped_no_head";
    case Counter::kPagesSkippedMalformedHead:
      return "rule_pack_pages_skipped_malformed_head";
    case Counter::kPagesSkippedNotRewritable:
      return "rule_pack_pages_skipped_not_rewritable";
    case Counter::kPagesSkippedSizeLimit:
      return "rule_pack_pages_skipped_size_limit";
    case Counter::kPagesSkippedError:
      return "rule_pack_pages_skipped_error";
    case Counter::kDecisionsApplied:
      return "rule_pack_decisions_applied";
    case Counter::kDecisionsWouldApply:
      return "rule_pack_decisions_would_apply";
    case Counter::kLateRemoved:
      return "rule_pack_late_removed";
    case Counter::kLateUnremovable:
      return "rule_pack_late_unremovable";
    case Counter::kHeadersSet:
      return "rule_pack_headers_set";
    case Counter::kHeadersFlushed:
      return "rule_pack_headers_flushed";
    case Counter::kDecisionLogLines:
      return "rule_pack_decision_log_lines";
    case Counter::kDecisionLogDropped:
      return "rule_pack_decision_log_dropped";
    case Counter::kLoadsSigned:
      return "rule_pack_loads_signed";
    case Counter::kRefusedUnsigned:
      return "rule_pack_refused_unsigned";
    case Counter::kRefusedSignatureUnreadable:
      return "rule_pack_refused_signature_unreadable";
    case Counter::kRefusedPublisherMismatch:
      return "rule_pack_refused_publisher_mismatch";
    case Counter::kRefusedUnknownKey:
      return "rule_pack_refused_unknown_key";
    case Counter::kRefusedBadSignature:
      return "rule_pack_refused_bad_signature";
    case Counter::kDecisionsClaimed:
      return "rule_pack_decisions_claimed";
    case Counter::kRefusedDuplicateId:
      return "rule_pack_refused_duplicate_id";
  }
  return "rule_pack_unknown";
}

const std::string* ClaimSet::Claim(std::string_view target,
                                   std::string_view pack_id) {
  for (const auto& [claimed, holder] : claims_) {
    if (claimed != target) continue;
    return holder == pack_id ? nullptr : &holder;
  }
  claims_.emplace_back(std::string(target), std::string(pack_id));
  return nullptr;
}

}  // namespace pagespeed::packs
