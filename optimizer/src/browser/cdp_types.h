// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

// PageSpeed 2.0 - Chrome DevTools Protocol Types
//
// Core types for CDP JSON-RPC communication over pipe transport.

#ifndef PAGESPEED_SRC_BROWSER_CDP_TYPES_H_
#define PAGESPEED_SRC_BROWSER_CDP_TYPES_H_

#include <cstdint>
#include <functional>
#include <string>

#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "nlohmann/json.hpp"

namespace pagespeed {

// CDP command to send to Chrome.
struct CdpCommand {
  std::string method;      // e.g. "Page.enable"
  nlohmann::json params;   // Method parameters (default: empty object)
  std::string session_id;  // Target session (empty = browser session)
  int timeout_ms = 30000;  // Per-command timeout (B2, M2)
};

// CDP response from Chrome.
struct CdpResponse {
  int id = 0;                 // Matching command ID
  nlohmann::json result;      // Success result
  std::string error_message;  // Non-empty on error
  int error_code = 0;         // CDP error code
  bool is_error() const { return error_code != 0 || !error_message.empty(); }
};

// CDP event received from Chrome.
struct CdpEvent {
  std::string method;      // e.g. "Page.loadEventFired"
  nlohmann::json params;   // Event parameters
  std::string session_id;  // Target session (empty = browser session)
};

// Callback types.
using CdpResponseCallback = std::function<void(absl::StatusOr<CdpResponse>)>;
using CdpEventCallback = std::function<void(const CdpEvent&)>;

// Configuration for Chrome process management.
struct ChromeProcessConfig {
  std::string chrome_path = "/usr/bin/google-chrome";
  int max_rss_mb = 512;            // Kill + restart above this RSS
  int startup_timeout_ms = 10000;  // Wait for DevTools ready
  int recycle_after_pages = 100;   // Restart after N pages analyzed
  bool headless = true;
  std::string user_data_dir;  // Chrome profile (empty = temp)
  // Emit --no-sandbox.  FALSE by default — the flag now
  // rides the resolved sandbox mode instead of being pushed unconditionally,
  // and it is only ever true when the operator passed --browser-sandbox=off.
  bool no_sandbox = false;
};

}  // namespace pagespeed

#endif  // PAGESPEED_SRC_BROWSER_CDP_TYPES_H_
