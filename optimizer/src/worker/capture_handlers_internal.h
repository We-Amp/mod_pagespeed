// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

// Internal helpers for capture_handlers.cc, exposed for unit testing.
//
// NOT part of the public API. Do not include from outside test code.

#ifndef PAGESPEED_SRC_WORKER_CAPTURE_HANDLERS_INTERNAL_H_
#define PAGESPEED_SRC_WORKER_CAPTURE_HANDLERS_INTERNAL_H_

#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

#include "nlohmann/json.hpp"
#include "src/worker/http_server.h"

namespace pagespeed {

class CdpClient;

namespace capture_internal {

using json = nlohmann::json;

// Resource timing data collected during CDP network capture.
struct ResourceEntry {
  std::string url;
  std::string method;
  std::string resource_type;
  std::string mime_type;
  int status = 0;
  double request_time = 0.0;   // Monotonic timestamp (seconds)
  double response_time = 0.0;  // When response headers arrived
  double end_time = 0.0;       // When loading finished
  int64_t encoded_data_length = 0;
  int64_t data_length = 0;

  // Detailed timing from Network.Response.timing (in ms relative
  // to requestTime).
  double dns_start = -1;
  double dns_end = -1;
  double connect_start = -1;
  double connect_end = -1;
  double ssl_start = -1;
  double ssl_end = -1;
  double send_start = -1;
  double send_end = -1;
  double receive_headers_end = -1;

  std::string request_id;
  bool finished = false;
};

// Serialize resource timing data to a JSON waterfall object.
json BuildWaterfallJson(const std::vector<ResourceEntry>& entries,
                        double page_load_time_ms);

// URL validation: requires http:// or https://.
bool IsValidCaptureUrl(std::string_view url);

// Extract hostname from URL (handles IPv6 brackets, userinfo, etc.).
std::string_view ExtractHostname(std::string_view url);

// Parse dotted IPv4 (supports decimal and octal octets).
bool ParseDottedIPv4(std::string_view s, uint32_t* out);

// Check if a 32-bit IPv4 address is private/loopback/link-local.
bool IsPrivateIPv4(uint32_t ip);

// Full SSRF check: returns true if the URL's host is private/loopback.
bool IsPrivateHost(std::string_view url);

// SSRF check for a CDP Network.responseReceived remoteIPAddress: returns true
// if the actual connection peer is a private/loopback/link-local address (IPv4
// or IPv6). Used to catch redirect and DNS-rebinding bypasses of the
// literal-URL IsPrivateHost() pre-check. Empty/unparseable -> false (e.g.
// data:/blob:/cached responses with no network peer).
bool IsPrivateRemoteIp(std::string_view remote_ip);

// The outcome of one capture, as the route handlers read it off their state.
struct CaptureOutcome {
  bool success = false;
  bool renderer_crashed = false;  // 502 rather than 504
  std::string error_message;
  json result_json;
};

// The error code a failed capture answers with: 502 when the renderer
// crashed, 504 for every other failure (the navigation timeout included).
inline ApiErrorCode CaptureFailureCode(const CaptureOutcome& outcome) {
  return outcome.renderer_crashed ? ApiErrorCode::kBadGateway
                                  : ApiErrorCode::kGatewayTimeout;
}

// Test seam: starts the CDP pipeline the route handlers run (createTarget,
// attachToTarget, the device emulation of src/browser/device_emulation.h,
// Network.enable, Page.enable, Page.navigate, then the events) on `client`,
// and calls `done` when it finishes. Unlike the handlers it does not spin
// the loop, so a test drives its own scripted peer.
void StartCaptureForTest(CdpClient* client, std::string url,
                         uint32_t viewport_width, uint32_t viewport_height,
                         bool is_waterfall, int timeout_ms,
                         std::function<void(const CaptureOutcome&)> done);

}  // namespace capture_internal
}  // namespace pagespeed

#endif  // PAGESPEED_SRC_WORKER_CAPTURE_HANDLERS_INTERNAL_H_
