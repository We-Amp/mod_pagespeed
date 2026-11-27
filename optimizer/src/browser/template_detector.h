// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

// PageSpeed 2.0 - Template Detector
//
// Hashes DOM structure from HtmlScanResult to group similar pages.
// Two pages sharing the same template (e.g., product pages, blog posts)
// produce the same hash, allowing a single browser analysis to cover
// all pages with that template.

#ifndef PAGESPEED_SRC_BROWSER_TEMPLATE_DETECTOR_H_
#define PAGESPEED_SRC_BROWSER_TEMPLATE_DETECTOR_H_

#include <cstdint>
#include <deque>
#include <mutex>
#include <string>
#include <string_view>

#include "absl/container/flat_hash_map.h"

namespace pagespeed {

struct HtmlScanResult;

class TemplateDetector {
 public:
  static constexpr size_t kMaxTemplates = 1000;

  // Hash the DOM structure (tag names + nesting depth, ignoring
  // content, ids, classes, and attribute values).
  // Returns a 64-bit FNV-1a hash that groups structurally similar
  // pages.
  static uint64_t HashStructure(const HtmlScanResult& scan_result);

  // Check if we already have a profile for this template.
  bool HasProfile(uint64_t template_hash) const;

  // Store association: template_hash -> analyzed_url.
  // If the map is full (kMaxTemplates), the oldest entry is evicted.
  void RecordProfile(uint64_t template_hash, std::string_view analyzed_url);

  // Remove a profile entry (e.g., after cache eviction).
  // Returns true if the entry existed and was removed.
  bool RemoveProfile(uint64_t template_hash);

  // Get the analyzed URL for a template hash, or empty string.
  std::string GetAnalyzedUrl(uint64_t template_hash) const;

  // Number of tracked templates.
  size_t size() const;

 private:
  mutable std::mutex mu_;
  absl::flat_hash_map<uint64_t, std::string> profiles_;
  std::deque<uint64_t> insertion_order_;  // For FIFO eviction
};

}  // namespace pagespeed

#endif  // PAGESPEED_SRC_BROWSER_TEMPLATE_DETECTOR_H_
