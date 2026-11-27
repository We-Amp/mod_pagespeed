// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

// 2. Fetch.enable + Fetch interception — serve from cached_resources
// 3. --host-resolver-rules="MAP * ~NOTFOUND" on Chrome — blocks DNS
// JavaScript is enabled (required for coverage analysis). Content is
// served via Fetch API interception from the worker's cache.

#ifndef PAGESPEED_SRC_BROWSER_SCRIPT_COVERAGE_ANALYZER_H_
#define PAGESPEED_SRC_BROWSER_SCRIPT_COVERAGE_ANALYZER_H_

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "absl/status/statusor.h"
#include "src/browser/page_analysis.h"

namespace pagespeed {

class CdpClient;

// Information about a single script element.
struct ScriptInfo {
  std::string url;         // Script URL; empty for inline scripts
  std::string selector;    // CSS selector of the <script> element
  bool is_inline = false;  // No src attribute (never in recommendations)
  bool is_async = false;
  bool is_defer = false;
  bool is_module = false;                  // type="module"
  float coverage_at_fcp = 0.0f;            // Fraction of bytes executed at FCP
  float coverage_at_load = 0.0f;           // Fraction executed at load
  bool has_document_write = false;         // Uses document.write()
  bool modifies_dom_before_paint = false;  // Not yet implemented (always false)
  // The V8 profiler reported this script with total_bytes > 0.  A script the
  // browser never fetched/compiled (blocked fetch, resolution failure) stays
  // false and MUST NOT be classified from the 0.0 coverage default.
  bool has_coverage_data = false;
};

// Deferral recommendation for a script.  Never serialized — profiles persist
// URL strings only — so adding values is compatibility-safe.
enum class DeferralAdvice : std::uint8_t {
  kKeepSynchronous,    // Must stay sync (document.write, DOM mutation)
  kSafeToDefer,        // Zero coverage at FCP, no DOM writes
  kCandidateForAsync,  // <10% coverage at FCP, no document.write
  kAlreadyAsync,       // Already has async/defer/module
  // No execution evidence — never defer.  Distinct from kKeepSynchronous so
  // evidence-free verdicts stay countable (scripts_no_coverage).
  kNoCoverageData,
};

// Complete script coverage analysis result.
struct ScriptCoverageResult {
  std::vector<ScriptInfo> scripts;
  std::vector<std::pair<std::string, DeferralAdvice>> recommendations;
  size_t total_script_bytes = 0;
  size_t fcp_used_bytes = 0;
  float fcp_coverage_ratio = 0.0f;
  // Fetch-interception outcomes for this session (map hit vs blocked).
  size_t fetches_served = 0;
  size_t fetches_blocked = 0;
};

// Analyzes JavaScript coverage in headless Chrome to identify scripts
// that are safe to defer or make async.
//
// Usage:
//   ScriptCoverageAnalyzer analyzer(cdp_client);
//   analyzer.Analyze(html, cached_resources, 1440, 900,
//       [](absl::StatusOr<ScriptCoverageResult> result) {
//         if (result.ok()) { /* use result->recommendations */ }
//       });
//
// Each Analyze() call creates a new browser tab, runs the analysis,
// and closes the tab. Only one analysis may run at a time per
// CdpClient (the event callback is single-subscriber).
class ScriptCoverageAnalyzer {
 public:
  using Callback = std::function<void(absl::StatusOr<ScriptCoverageResult>)>;

  explicit ScriptCoverageAnalyzer(CdpClient* client);
  ~ScriptCoverageAnalyzer() = default;

  ScriptCoverageAnalyzer(const ScriptCoverageAnalyzer&) = delete;
  ScriptCoverageAnalyzer& operator=(const ScriptCoverageAnalyzer&) = delete;

  // Default overall session timeout (60 seconds).
  static constexpr uint32_t kDefaultTimeoutMs = 60000;

  // Analyze script coverage at a specific viewport.
  // html_content: full HTML to render.
  // cached_resources: URL -> content map for Fetch interception.
  // The callback is invoked when analysis completes or fails.
  // timeout_ms is an overall session timeout (0 = no timeout).
  void Analyze(
      std::string_view html_content,
      std::shared_ptr<const PageAnalyzer::ResourceMap> cached_resources,
      uint32_t viewport_width, uint32_t viewport_height, Callback callback,
      uint32_t timeout_ms = kDefaultTimeoutMs);

  // Classify a script's deferral advice based on its properties.
  // Exposed as a static method for unit testing without CDP.
  static DeferralAdvice Classify(const ScriptInfo& info);

 private:
  struct Session;
  CdpClient* client_;
};

}  // namespace pagespeed

#endif  // PAGESPEED_SRC_BROWSER_SCRIPT_COVERAGE_ANALYZER_H_
