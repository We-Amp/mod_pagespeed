// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

#include "pagespeed/kernel/packs/decision.h"

#include <cstdint>
#include <string>
#include <string_view>

#include "absl/strings/str_cat.h"
#include "pagespeed/kernel/packs/rule_kind.h"
#include "pagespeed/kernel/packs/template.h"

namespace pagespeed::packs {

std::string_view ActionName(Action action) {
  switch (action) {
    case Action::kNone:
      return "none";
    case Action::kInsert:
      return "insert";
    case Action::kFill:
      return "fill";
    case Action::kReplace:
      return "replace";
    case Action::kRemove:
      return "remove";
    case Action::kDedupe:
      return "dedupe";
  }
  return "none";
}

std::string_view ReasonName(Reason reason) {
  switch (reason) {
    case Reason::kAbsent:
      return "absent";
    case Reason::kPresent:
      return "present";
    case Reason::kDuplicate:
      return "duplicate";
    case Reason::kUnusable:
      return "unusable";
    case Reason::kOutOfHead:
      return "out_of_head";
    case Reason::kEmpty:
      return "empty";
    case Reason::kConflict:
      return "conflict";
    case Reason::kEqual:
      return "equal";
    case Reason::kNoValue:
      return "no_value";
    case Reason::kInvalidValue:
      return "invalid_value";
    case Reason::kClusterWithoutSelf:
      return "cluster_without_self";
    case Reason::kShadowed:
      return "shadowed";
    case Reason::kNotRewritable:
      return "not_rewritable";
    case Reason::kLateConflict:
      return "late_conflict";
    case Reason::kHeaderConflict:
      return "header_conflict";
    case Reason::kHeadersFlushed:
      return "headers_flushed";
    case Reason::kClaimed:
      return "claimed";
  }
  return "present";
}

std::string HashValue(std::string_view value) {
  uint64_t h = 14695981039346656037ULL;
  for (unsigned char c : value) {
    h ^= c;
    h *= 1099511628211ULL;
  }
  static constexpr char kHex[] = "0123456789abcdef";
  std::string out = "fnv1a64:";
  for (int shift = 60; shift >= 0; shift -= 4) {
    out.push_back(kHex[(h >> shift) & 0xF]);
  }
  return out;
}

std::string TruncateUtf8(std::string_view value, size_t max_bytes) {
  if (value.size() <= max_bytes) return std::string(value);
  size_t n = max_bytes;
  while (n > 0 && (static_cast<unsigned char>(value[n]) & 0xC0) == 0x80) --n;
  return std::string(value.substr(0, n));
}

namespace {
std::string Quote(std::string_view s) {
  return absl::StrCat("\"", EscapeJsonString(s), "\"");
}
}  // namespace

std::string DecisionToJsonLine(std::string_view host, std::string_view path,
                               const PackDecision& d) {
  constexpr size_t kMaxPathBytes = 300;
  // Never splits a UTF-8 sequence, so the line stays valid JSON text.
  const std::string cut_path = TruncateUtf8(path, kMaxPathBytes);
  return absl::StrCat(
      "{\"host\":", Quote(host), ",\"path\":", Quote(cut_path),
      ",\"rule\":", Quote(d.rule_id), ",\"kind\":",
      Quote(d.kind == nullptr ? std::string_view() : d.kind->name()),
      ",\"mode\":", Quote(ModeName(d.mode)),
      ",\"action\":", Quote(ActionName(d.action)),
      ",\"reason\":", Quote(ReasonName(d.reason)),
      ",\"defect\":", Quote(d.defect), ",\"before\":", Quote(d.before_hash),
      ",\"after\":", Quote(d.after),
      d.removed.empty() ? std::string()
                        : absl::StrCat(",\"removed\":", Quote(d.removed)),
      d.header ? ",\"header\":true" : "",
      d.claimed_by.empty()
          ? std::string()
          : absl::StrCat(",\"claimed_by\":", Quote(d.claimed_by)),
      "}");
}

}  // namespace pagespeed::packs
