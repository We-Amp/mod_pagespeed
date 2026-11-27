// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

// 2. Fetch.enable + Fetch.failRequest for all requests -- intercept layer
// 3. Emulation.setScriptExecutionDisabled({value:true}) -- no JS
// Content is loaded via Page.setDocumentContent (no navigation).

#ifndef PAGESPEED_SRC_BROWSER_VISUAL_REGRESSION_GATE_H_
#define PAGESPEED_SRC_BROWSER_VISUAL_REGRESSION_GATE_H_

#include <cstdint>
#include <functional>
#include <memory>
#include <string_view>
#include <vector>

#include "absl/status/statusor.h"

namespace pagespeed {

class CdpClient;

// Screenshot data captured from a rendered page.
struct ScreenshotResult {
  std::vector<uint8_t> png_data;
  uint32_t width = 0;
  uint32_t height = 0;
};

// Result of a visual regression comparison.
struct RegressionResult {
  bool passed = false;      // true if diff <= threshold
  float diff_ratio = 0.0f;  // ratio of differing pixels
  uint32_t diff_pixels = 0;
  uint32_t total_pixels = 0;
  uint32_t above_fold_height = 0;  // viewport height used
  // Every compared pixel of the ORIGINAL (reference) screenshot is the same
  // colour, within kChannelTolerance of the first: a blank render. Two blank
  // renders compare equal, so a caller that confirms something on a match
  // must refuse such a comparison (the critical-CSS validator does).
  // `passed` is not changed by it.
  bool reference_uniform = false;
};

// Captures screenshots and compares them for visual regression.
//
// LIFETIME. A capture outlives the call that started it: it is a chain of CDP
// round trips plus a libuv timeout timer, and both hold a raw `CdpClient*`.
// The gate therefore OWNS its in-flight captures and cancels them when it is
// destroyed, so the rule for an owner is simply:
//
//     destroy the gate BEFORE the CdpClient it was built on.
//
// Get that order wrong and the timeout timer is the last thing standing — it
// fires after the client is gone and dereferences it. That is not the rare
// path: once the client dies, no response ever arrives, so the timer becomes
// the ONLY way a capture can finish. Owners hold the gate as a member next to
// their other CDP components and tear it down with them.
class VisualRegressionGate {
 public:
  explicit VisualRegressionGate(CdpClient* client);
  // Cancels every in-flight capture, failing its callback. The client must
  // still be alive here — see the lifetime note above.
  ~VisualRegressionGate();

  VisualRegressionGate(const VisualRegressionGate&) = delete;
  VisualRegressionGate& operator=(const VisualRegressionGate&) = delete;

  using Callback = std::function<void(absl::StatusOr<RegressionResult>)>;

  // Default pixel difference threshold (0.5% of pixels).
  static constexpr float kDefaultThreshold = 0.005f;

  // Default overall session timeout (60 seconds).
  static constexpr uint32_t kDefaultTimeoutMs = 60000;

  // Per-channel tolerance for pixel comparison (anti-aliasing).
  static constexpr int kChannelTolerance = 2;

  // Compare original and optimized HTML visually.
  // Renders both at the given viewport, captures above-fold
  // screenshots, and computes pixel difference.
  void Compare(std::string_view original_html, std::string_view optimized_html,
               uint32_t viewport_width, uint32_t viewport_height,
               Callback callback, float threshold = kDefaultThreshold);

  // Capture a screenshot of HTML content at the given viewport.
  void CaptureScreenshot(
      std::string_view html_content, uint32_t viewport_width,
      uint32_t viewport_height,
      std::function<void(absl::StatusOr<ScreenshotResult>)> callback);

  // Compare two PNG screenshots (static, no Chrome needed).
  // Compares only the above-fold region (viewport_height pixels).
  static RegressionResult CompareScreenshots(
      const std::vector<uint8_t>& original_png,
      const std::vector<uint8_t>& optimized_png, uint32_t viewport_height,
      float threshold = kDefaultThreshold);

 private:
  struct Session;

  // Forget sessions that already completed, so a long-lived gate does not
  // accumulate expired weak references.
  void PruneSessions();

  CdpClient* client_;
  // In-flight captures. Weak, because a session stays alive on its own while
  // the CDP round trip is outstanding; this is a cancellation handle, not
  // ownership of the memory.
  std::vector<std::weak_ptr<Session>> sessions_;
  // Set for the duration of ~VisualRegressionGate. A cancelled capture's
  // callback can ask for another one (Compare's handoff does exactly that when
  // the first capture succeeds), and starting one from inside the destructor
  // would leave it running against a gate that no longer exists.
  bool destroying_ = false;
};

}  // namespace pagespeed

#endif  // PAGESPEED_SRC_BROWSER_VISUAL_REGRESSION_GATE_H_
