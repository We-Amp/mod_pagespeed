// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

#ifndef PAGESPEED_KERNEL_PACKS_DECISION_H_
#define PAGESPEED_KERNEL_PACKS_DECISION_H_

#include <cstddef>
#include <string>
#include <string_view>

#include "pagespeed/kernel/packs/pack.h"

namespace pagespeed::packs {

// What a rule did (enforce) or would do (report) on one page.
enum class Action { kNone, kInsert, kFill, kReplace, kRemove, kDedupe };

// Why. The same names the decision log uses.
enum class Reason {
  kAbsent,
  kPresent,
  kDuplicate,
  kUnusable,
  kOutOfHead,
  kEmpty,
  kConflict,
  kEqual,
  kNoValue,
  kInvalidValue,
  kClusterWithoutSelf,
  kShadowed,
  // The page's head was not safely editable, so the rule's plan was dropped.
  kNotRewritable,
  // An element of the rule's kind appeared after the decision was made and
  // contradicts it.
  kLateConflict,
  // The response headers already carry a conflicting canonical link.
  kHeaderConflict,
  // The response headers were already sent.
  kHeadersFlushed,
};

std::string_view ActionName(Action action);
std::string_view ReasonName(Reason reason);

// One rule's decision on one page. The value that was on the page is recorded
// as a hash only, never as text; the value the rule puts there comes from the
// pack, never from the page, and is recorded as text.
struct PackDecision {
  std::string rule_id;
  const RuleKind* kind = nullptr;
  // The effective mode of the rule on this page: `report` decisions are what
  // the rule WOULD do and changed nothing.
  Mode mode = Mode::kReport;
  Action action = Action::kNone;
  Reason reason = Reason::kPresent;
  // The defect id (as an SEO-defects checker names it) this decision clears or
  // observes; empty when the situation is not a checker defect.
  std::string defect;
  // Hash of the value that was on the page; empty when there was none.
  std::string before_hash;
  // The value the rule sets (pack-derived), at most kMaxAfterBytes bytes;
  // empty when the decision sets nothing.
  std::string after;
  // What the rule took out of the page, as "code=href (reason)" entries for
  // hreflang and "@types (reason)" for jsonld, separated by "; ", at most
  // kMaxAfterBytes bytes. This is page data, not pack data. Left out of the
  // log line when empty.
  std::string removed;
  // The decision is about the `Link: rel=canonical` response header, not the
  // document. Written to the log line as `"header":true`.
  bool header = false;

  bool operator==(const PackDecision&) const = default;
};

// Stable 64-bit FNV-1a of `value`, as "fnv1a64:" plus 16 lowercase hex
// digits. Deterministic across runs and platforms (no per-process seed).
// Cuts `value` to at most `max_bytes` bytes without splitting a UTF-8
// sequence.
inline constexpr size_t kMaxAfterBytes = 200;
std::string TruncateUtf8(std::string_view value, size_t max_bytes);

std::string HashValue(std::string_view value);

// One JSON object, no trailing newline, keys in a fixed order:
// host, path, rule, kind, mode, action, reason, defect, before, after and,
// only when something was removed, removed and, only for a response header
// decision, header. The path
// is cut to 300 bytes. Contains no timestamp or pack identity: the sink that
// writes the line adds them.
std::string DecisionToJsonLine(std::string_view host, std::string_view path,
                               const PackDecision& decision);

}  // namespace pagespeed::packs

#endif  // PAGESPEED_KERNEL_PACKS_DECISION_H_
