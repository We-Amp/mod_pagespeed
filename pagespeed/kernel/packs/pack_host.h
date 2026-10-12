// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

#ifndef PAGESPEED_KERNEL_PACKS_PACK_HOST_H_
#define PAGESPEED_KERNEL_PACKS_PACK_HOST_H_

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "pagespeed/kernel/packs/decision.h"
#include "pagespeed/kernel/packs/template.h"

namespace pagespeed::packs {

// The fixed counters the filter and the pack store move. A host maps them
// onto its statistics; per-rule numbers live in the decision log only.
enum class Counter : std::uint8_t {
  kLoads,
  kLoadErrors,
  kPagesEvaluated,
  kPagesModified,
  kPagesWouldModify,
  kPagesSkippedHostNotListed,
  kPagesSkippedNoHead,
  kPagesSkippedMalformedHead,
  kPagesSkippedNotRewritable,
  kPagesSkippedSizeLimit,
  // Not moved yet: planning is a pure function that cannot fail, so the
  // filter has no error skip. Kept so the statistics names stay stable.
  kPagesSkippedError,
  kDecisionsApplied,
  kDecisionsWouldApply,
  kLateRemoved,
  kLateUnremovable,
  kHeadersSet,
  kHeadersFlushed,
  kDecisionLogLines,
  kDecisionLogDropped,
  kLoadsSigned,
  kRefusedUnsigned,
  kRefusedSignatureUnreadable,
  kRefusedPublisherMismatch,
  kRefusedUnknownKey,
  kRefusedBadSignature,
};
inline constexpr int kCounterCount =
    static_cast<int>(Counter::kRefusedBadSignature) + 1;

// The statistics name of a counter ("rule_pack_pages_evaluated").
std::string_view CounterName(Counter counter);

enum class LogLevel : std::uint8_t { kInfo, kWarning, kError };

// What the rule-pack filter needs from the server it runs in. Keeps the
// engine free of the rewriter, HTTP headers, statistics and the file system.
class PackHost {
 public:
  virtual ~PackHost() = default;

  // Response headers. Names are ASCII case-insensitive.
  class Headers {
   public:
    virtual ~Headers() = default;
    virtual std::vector<std::string> Values(std::string_view name) const = 0;
    virtual void Add(std::string_view name, std::string_view value) = 0;
    virtual void Replace(std::string_view name,
                         const std::vector<std::string>& values) = 0;
  };
  // The response headers, or null when the host has none or they were sent
  // already (after the first flush).
  virtual Headers* response_headers() = 0;
  // A request header, for rules that look at the request. Unused by the
  // current kinds.
  virtual std::optional<std::string> RequestHeader(
      std::string_view name) const = 0;
  virtual void Log(LogLevel level, std::string_view message) = 0;
  virtual void Count(Counter counter, int64_t delta = 1) = 0;
  // One decision of one rule on one page, for the decision log. The host
  // knows which pack (and which version of it) it handed to the filter.
  virtual void Decision(const PageUrl& url, const PackDecision& decision) = 0;
  // Milliseconds since the epoch, for decision-log timestamps only; never
  // reaches the HTML.
  virtual int64_t NowMs() const = 0;
  // Put an HTML comment before every element the pack inserted or changed.
  virtual bool debug_comments() const = 0;
};

}  // namespace pagespeed::packs

#endif  // PAGESPEED_KERNEL_PACKS_PACK_HOST_H_
