// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

// PageSpeed 2.0 - Unused CSS Removal
//
// Removes browser-detected unused CSS rules from stylesheets.
// Uses CSS Coverage API data from BrowserCssExtractor to identify
// rules that never matched any element across all analyzed viewports.
//
// This goes beyond critical CSS (above-fold only) -- it removes rules
// that are truly dead across the entire page lifecycle.

#ifndef PAGESPEED_SRC_BROWSER_UNUSED_CSS_REMOVER_H_
#define PAGESPEED_SRC_BROWSER_UNUSED_CSS_REMOVER_H_

#include <cstddef>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace pagespeed {

// Result of unused CSS removal.
struct UnusedCssRemovalResult {
  std::string cleaned_css;  // CSS with unused rules removed
  size_t original_bytes = 0;
  size_t cleaned_bytes = 0;
  size_t rules_removed = 0;
  float removal_ratio = 0.0f;  // removed / original
};

// Removes unused CSS rules identified by browser analysis.
//
// Usage:
//   auto result = UnusedCssRemover::Remove(stylesheet_text, used_ranges);
//   // result.cleaned_css contains the stylesheet with unused rules removed.
//   // result.removal_ratio indicates the fraction of bytes removed.
//
// Preservation: @charset, @import, @font-face, and @keyframes rules are
// always preserved regardless of coverage data, since they may be needed
// for correct rendering even if not directly matched.
class UnusedCssRemover {
 public:
  // Remove unused rules from a stylesheet based on coverage data.
  // used_ranges: byte ranges that were active during full page load
  //   (from CSS.stopRuleUsageTracking, NOT just FCP).
  // full_css: the complete stylesheet text.
  static UnusedCssRemovalResult Remove(
      std::string_view full_css,
      const std::vector<std::pair<size_t, size_t>>& used_ranges);

  // Merge coverage data from multiple viewports.
  // Union of all used ranges across mobile/tablet/desktop.
  static std::vector<std::pair<size_t, size_t>> MergeViewportCoverage(
      const std::vector<std::vector<std::pair<size_t, size_t>>>&
          per_viewport_ranges);
};

}  // namespace pagespeed

#endif  // PAGESPEED_SRC_BROWSER_UNUSED_CSS_REMOVER_H_
