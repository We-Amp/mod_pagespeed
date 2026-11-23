// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

// PageSpeed 2.0 - Page Analysis via Headless Browser
//
// Uses Chrome's Performance Observer and DOM APIs to detect LCP
// elements, fold position, image dimensions, and CLS sources.
// This provides ground-truth data that replaces heuristic detection.
//
// Content is served via Fetch interception from the worker's cache,
// so images load and have meaningful dimensions. SSRF is prevented
// by blocking all requests not matched in cache.

#ifndef PAGESPEED_SRC_BROWSER_PAGE_ANALYSIS_H_
#define PAGESPEED_SRC_BROWSER_PAGE_ANALYSIS_H_

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "absl/container/flat_hash_map.h"
#include "absl/status/statusor.h"
#include "src/browser/agent_fetcher.h"

namespace pagespeed {

class CdpClient;

// Information about an image element detected by browser analysis.
struct ImageInfo {
  std::string selector;
  std::string src;
  bool above_fold = false;
  uint32_t rendered_width = 0;
  uint32_t rendered_height = 0;
  uint32_t natural_width = 0;
  uint32_t natural_height = 0;
};

// One element the render walked, described the way a later static-HTML scan of
// the same page can recognize it again.
//
// `index` is Chrome's `document.querySelectorAll('*')` ordinal. It is NOT
// HtmlScanner's `element_index` (src/worker/html_scanner.h) and the two must
// never be compared or subtracted: they are different walks of different
// documents (rendered vs. as-served) and agree only loosely. Consumers match on
// the id/class/tag description; the ordinal is reportable context only.
struct ElementDescriptor {
  std::string tag;  // lowercased tag name
  std::string id;
  std::vector<std::string> classes;
  uint32_t index = 0;
  bool above_fold = false;
};

// LCP element information.
struct LcpInfo {
  std::string selector;
  std::string url;  // For image LCP elements
  std::string element_tag;
  uint32_t size = 0;  // LCP size metric
};

// Layout shift source.
struct ClsSource {
  std::string selector;
  float shift_value = 0.0f;
};

// Complete page analysis result.
struct PageAnalysisResult {
  LcpInfo lcp;
  std::vector<ImageInfo> images;
  std::vector<ClsSource> cls_sources;
  std::vector<std::string> preconnect_origins;
  float fcp_ms = 0.0f;
  float lcp_ms = 0.0f;
  float cls_total = 0.0f;

  // agent_optimize: populated ONLY on the agent-content path
  // (else empty). rendered_html is the hydrated rendered DOM read post-networkIdle;
  // agent_markdown is the SAX-extracted markdown. On this path the perf fields above
  // are left default (the agent render extracts content, not a perf profile).
  std::string rendered_html;
  std::string agent_markdown;

  // Hard cap on `elements`. A profile carries three viewports and is cached, so
  // the descriptor list is bounded rather than proportional to page size.
  static constexpr size_t kMaxElementDescriptors = 2000;

  // The elements this render walked, in document order, truncated to
  // kMaxElementDescriptors. Includes below-fold elements, flagged: dropping
  // them would make "measured nothing" indistinguishable from "measured, and
  // nothing was visible".
  std::vector<ElementDescriptor> elements;

  // True when `elements` is a truncated prefix. Never silent: a consumer has to
  // be able to tell a short page from a clipped one, because a clipped list
  // under-describes the fold.
  bool elements_truncated = false;
};

// Analyzes a page in headless Chrome to detect LCP, fold position,
// image dimensions, and layout shift sources.
//
// Usage:
//   PageAnalyzer analyzer(cdp_client);
//   analyzer.Analyze(html, css_map, viewport_w, viewport_h,
//       [](auto result) { ... });
//
// Each Analyze() call creates a new browser tab. Cached resources
// are served via Fetch API interception.
class PageAnalyzer {
 public:
  using Callback = std::function<void(absl::StatusOr<PageAnalysisResult>)>;

  // Map of URL -> content for serving cached resources via Fetch.
  using ResourceMap = absl::flat_hash_map<std::string, std::string>;

  explicit PageAnalyzer(CdpClient* client);
  ~PageAnalyzer() = default;

  PageAnalyzer(const PageAnalyzer&) = delete;
  PageAnalyzer& operator=(const PageAnalyzer&) = delete;

  // Default overall session timeout (60 seconds).
  static constexpr uint32_t kDefaultTimeoutMs = 60000;

  // Analyze a page at a specific viewport.
  // html_content: full HTML to render.
  // cached_resources: URL -> content map for Fetch interception.
  //   Pass a shared_ptr to avoid deep-copying multi-MB resource maps.
  // The callback is invoked when analysis completes or fails.
  // timeout_ms is an overall session timeout (0 = no timeout).
  // agent: agent_optimize render options. nullptr (the default) keeps the
  //   legacy offline/cache-only behavior; when set with a non-null policy, paused
  //   subresource requests are fetched out-of-Chrome via the IP-pinned fetcher.
  void Analyze(std::string_view html_content,
               std::shared_ptr<const ResourceMap> cached_resources,
               uint32_t viewport_width, uint32_t viewport_height,
               Callback callback, uint32_t timeout_ms = kDefaultTimeoutMs,
               std::shared_ptr<const AgentRenderOptions> agent = nullptr);

  // How long after the page's load event the render collects when the page
  // never reaches networkIdle (kSettleAfterLoadMs). For tests.
  void set_settle_after_load_ms_for_testing(uint64_t ms) {
    settle_after_load_ms_ = ms;
  }

 private:
  struct Session;
  // Step 1 onwards: create the target (in the session's browser context,
  // if it has one), attach, and drive the render.
  static void StartTarget(const std::shared_ptr<Session>& session);
  CdpClient* client_;
  uint64_t settle_after_load_ms_;
};

}  // namespace pagespeed

#endif  // PAGESPEED_SRC_BROWSER_PAGE_ANALYSIS_H_
