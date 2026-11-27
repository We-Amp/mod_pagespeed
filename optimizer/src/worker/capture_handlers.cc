// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

// PageSpeed 2.0 - Capture API Route Handlers Implementation
//
// CDP workflows:
//
// Waterfall:
//   1. Target.createTarget({url: "about:blank"})
//   2. Target.attachToTarget({targetId, flatten: true})
//   3. Emulation.setDeviceMetricsOverride + setTouchEmulationEnabled
//      [+ setUserAgentOverride] (EmulateDevice,
//      src/browser/device_emulation.h); the navigation of step 6 makes the
//      page's window, so no blank document is loaded first
//   4. Network.enable
//   5. Page.enable + Page.setLifecycleEventsEnabled
//   6. Page.navigate({url})
//   7. Collect Network.requestWillBeSent, responseReceived, loadingFinished
//   8. Wait for Page.loadEventFired or timeout
//   9. Return HAR-like JSON
//   10. Target.closeTarget
//
// Screenshot:
//   1-6. Same as waterfall
//   7. Wait for Page.lifecycleEvent("networkIdle") or loadEventFired
//   8. Page.captureScreenshot
//   9. Return base64 PNG
//   10. Target.closeTarget

#include "src/worker/capture_handlers.h"

#include <cctype>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "absl/status/statusor.h"
#include "absl/strings/str_cat.h"
#include "lib/base/string_util.h"
#include "nlohmann/json.hpp"
#include "src/browser/cdp_client.h"
#include "src/browser/cdp_types.h"
#include "src/browser/device_emulation.h"
#include "src/worker/browser_analysis_manager.h"
#include "src/worker/capture_handlers_internal.h"
#include "src/worker/json_dump.h"
#include "uv.h"

namespace pagespeed {

using json = nlohmann::json;
using net_instaweb::AsciiToLowerInPlace;

// ---------------------------------------------------------------------------
// Testable helper functions (capture_internal namespace)
// ---------------------------------------------------------------------------

namespace capture_internal {

bool IsValidCaptureUrl(std::string_view url) {
  return url.starts_with("http://") || url.starts_with("https://");
}

std::string_view ExtractHostname(std::string_view url) {
  // Skip scheme.
  size_t pos = url.find("://");
  if (pos == std::string_view::npos) return {};
  pos += 3;
  if (pos >= url.size()) return {};

  // Skip userinfo@ if present.
  size_t at = url.find('@', pos);
  size_t slash = url.find('/', pos);
  if (at != std::string_view::npos &&
      (slash == std::string_view::npos || at < slash)) {
    pos = at + 1;
  }

  // Find end of host (port or path).
  size_t end = url.find_first_of(":/?#", pos);
  if (end == std::string_view::npos) end = url.size();

  // Handle IPv6 bracket notation [::1].
  if (pos < url.size() && url[pos] == '[') {
    size_t bracket = url.find(']', pos);
    if (bracket == std::string_view::npos) return {};
    return url.substr(pos, bracket + 1 - pos);
  }

  return url.substr(pos, end - pos);
}

bool IsPrivateIPv4(uint32_t ip) {
  // 127.0.0.0/8 (loopback)
  if ((ip >> 24) == 127) return true;
  // 10.0.0.0/8
  if ((ip >> 24) == 10) return true;
  // 172.16.0.0/12
  if ((ip >> 16 & 0xFFF0) == 0xAC10) return true;
  // 192.168.0.0/16
  if ((ip >> 16) == 0xC0A8) return true;
  // 169.254.0.0/16 (link-local)
  if ((ip >> 16) == 0xA9FE) return true;
  // 0.0.0.0
  if (ip == 0) return true;
  return false;
}

bool ParseDottedIPv4(std::string_view s, uint32_t* out) {
  uint32_t result = 0;
  int octets = 0;
  size_t pos = 0;
  while (pos < s.size() && octets < 4) {
    if (octets > 0) {
      if (s[pos] != '.') return false;
      ++pos;
    }
    if (pos >= s.size()) return false;
    // Find end of this octet.
    size_t end = s.find('.', pos);
    if (end == std::string_view::npos) end = s.size();
    std::string_view octet_str = s.substr(pos, end - pos);
    if (octet_str.empty()) return false;
    // Detect base: leading '0' followed by more digits means octal.
    int base = 10;
    if (octet_str.size() > 1 && octet_str[0] == '0' &&
        (std::isdigit(static_cast<unsigned char>(octet_str[1])) != 0)) {
      base = 8;
    }
    char* endp = nullptr;
    std::string tmp(octet_str);
    unsigned long val = strtoul(tmp.c_str(), &endp, base);
    if (endp != tmp.c_str() + tmp.size() || val > 255) return false;
    result = (result << 8) | static_cast<uint32_t>(val);
    pos = end;
    ++octets;
  }
  if (octets != 4 || pos != s.size()) return false;
  *out = result;
  return true;
}

bool IsPrivateRemoteIp(std::string_view remote_ip) {
  if (remote_ip.empty()) {
    return false;  // No network peer (data:/blob:/cached) — not an SSRF vector.
  }
  // Strip surrounding brackets from an IPv6 literal, if present.
  std::string_view ip = remote_ip;
  if (ip.size() >= 2 && ip.front() == '[' && ip.back() == ']') {
    ip = ip.substr(1, ip.size() - 2);
  }
  uint32_t v4 = 0;
  if (ParseDottedIPv4(ip, &v4)) {
    return IsPrivateIPv4(v4);
  }
  // IPv6: loopback (::1), unique-local (fc00::/7), link-local (fe80::/10).
  std::string h(ip);
  AsciiToLowerInPlace(h);
  if (h == "::1") return true;
  if (h.starts_with("fc") || h.starts_with("fd")) return true;  // fc00::/7
  if (h.starts_with("fe8") || h.starts_with("fe9") || h.starts_with("fea") ||
      h.starts_with("feb")) {  // fe80::/10
    return true;
  }
  return false;
}

bool IsPrivateHost(std::string_view url) {
  std::string_view host = ExtractHostname(url);
  if (host.empty()) return true;  // Unparseable -> reject.

  // Lowercase for comparison (hostname is case-insensitive).
  std::string h(host);
  AsciiToLowerInPlace(h);

  // Loopback.
  if (h == "localhost" || h.ends_with(".localhost") || h.starts_with("127.") ||
      h == "[::1]") {
    return true;
  }
  // All-zeros.
  if (h == "0.0.0.0" || h == "[::]") return true;
  // RFC 1918 private ranges.
  if (h.starts_with("10.")) return true;
  if (h.starts_with("192.168.")) return true;
  // 172.16.0.0/12 = 172.16.* through 172.31.*
  if (h.starts_with("172.")) {
    size_t dot = h.find('.', 4);
    if (dot != std::string::npos) {
      int octet = 0;
      auto sub = h.substr(4, dot - 4);
      for (char c : sub) {
        if (c < '0' || c > '9') break;
        octet = octet * 10 + (c - '0');
      }
      if (octet >= 16 && octet <= 31) return true;
    }
  }
  // Link-local.
  if (h.starts_with("169.254.")) return true;
  // IPv6 private/link-local.
  if (h.starts_with("[fc") || h.starts_with("[fd") || h.starts_with("[fe80:")) {
    return true;
  }
  // Metadata endpoints (cloud providers).
  if (h == "metadata.google.internal") return true;

  // IPv4-mapped IPv6: [::ffff:A.B.C.D] or hex form [::ffff:XXXX:YYYY]
  if (h.starts_with("[::ffff:") && h.back() == ']') {
    std::string_view inner = std::string_view(h).substr(8, h.size() - 9);
    uint32_t ip = 0;
    if (ParseDottedIPv4(inner, &ip) && IsPrivateIPv4(ip)) return true;
    // Hex form: [::ffff:7f00:1] -- two 16-bit groups separated by colon.
    auto colon = inner.find(':');
    if (colon != std::string_view::npos) {
      std::string hi_s(inner.substr(0, colon));
      std::string lo_s(inner.substr(colon + 1));
      char* endp1 = nullptr;
      char* endp2 = nullptr;
      unsigned long hi = strtoul(hi_s.c_str(), &endp1, 16);
      unsigned long lo = strtoul(lo_s.c_str(), &endp2, 16);
      if (endp1 == hi_s.c_str() + hi_s.size() &&
          endp2 == lo_s.c_str() + lo_s.size() && hi <= 0xFFFF && lo <= 0xFFFF) {
        auto mapped = static_cast<uint32_t>((hi << 16) | lo);
        if (IsPrivateIPv4(mapped)) return true;
      }
    }
  }

  // Hex IP: 0x7f000001 -> 127.0.0.1
  if (h.starts_with("0x") && h.size() > 2) {
    bool all_hex = true;
    for (size_t i = 2; i < h.size(); ++i) {
      if (std::isxdigit(static_cast<unsigned char>(h[i])) == 0) {
        all_hex = false;
        break;
      }
    }
    if (all_hex) {
      char* endp = nullptr;
      unsigned long val = strtoul(h.c_str(), &endp, 16);
      if (endp == h.c_str() + h.size() && val <= 0xFFFFFFFFUL) {
        if (IsPrivateIPv4(static_cast<uint32_t>(val))) return true;
      }
    }
  }

  // Decimal IP: all digits -> single 32-bit integer.
  {
    bool all_digits = !h.empty();
    for (char c : h) {
      if (!std::isdigit(static_cast<unsigned char>(c))) {
        all_digits = false;
        break;
      }
    }
    if (all_digits) {
      char* endp = nullptr;
      unsigned long val = strtoul(h.c_str(), &endp, 10);
      if (endp == h.c_str() + h.size() && val <= 0xFFFFFFFFUL) {
        if (IsPrivateIPv4(static_cast<uint32_t>(val))) return true;
      }
    }
  }

  // Dotted octal/mixed: e.g. 0177.0.0.1 -> 127.0.0.1
  if (h.find('.') != std::string::npos) {
    uint32_t ip = 0;
    if (ParseDottedIPv4(h, &ip) && IsPrivateIPv4(ip)) return true;
  }

  return false;
}

json BuildWaterfallJson(const std::vector<ResourceEntry>& entries,
                        double page_load_time_ms) {
  json result;
  result["page_load_time_ms"] = page_load_time_ms;

  json resources = json::array();
  for (const auto& e : entries) {
    json r;
    r["url"] = e.url;
    r["method"] = e.method;
    r["resource_type"] = e.resource_type;
    r["mime_type"] = e.mime_type;
    r["status"] = e.status;
    r["encoded_data_length"] = e.encoded_data_length;
    r["data_length"] = e.data_length;

    // Compute phase durations in ms.
    json timing = json::object();
    if (e.dns_start >= 0 && e.dns_end >= 0) {
      timing["dns_ms"] = e.dns_end - e.dns_start;
    }
    if (e.connect_start >= 0 && e.connect_end >= 0) {
      timing["connect_ms"] = e.connect_end - e.connect_start;
    }
    if (e.ssl_start >= 0 && e.ssl_end >= 0) {
      timing["tls_ms"] = e.ssl_end - e.ssl_start;
    }
    if (e.send_start >= 0 && e.send_end >= 0) {
      timing["send_ms"] = e.send_end - e.send_start;
    }
    if (e.send_end >= 0 && e.receive_headers_end >= 0) {
      timing["ttfb_ms"] = e.receive_headers_end - e.send_end;
    }

    // Total duration.
    if (e.request_time > 0 && e.end_time > 0) {
      double duration_ms = (e.end_time - e.request_time) * 1000.0;
      timing["total_ms"] = duration_ms;

      // Download time = total - time to headers.
      if (e.receive_headers_end >= 0) {
        double download = duration_ms - e.receive_headers_end;
        if (download > 0) timing["download_ms"] = download;
      }
    }

    // Start offset relative to navigation (for waterfall chart).
    r["timing"] = timing;
    resources.push_back(std::move(r));
  }

  result["resources"] = std::move(resources);
  result["resource_count"] = entries.size();
  return result;
}

}  // namespace capture_internal

// Bring capture_internal names into local scope for use by anonymous namespace.
using capture_internal::BuildWaterfallJson;
using capture_internal::CaptureFailureCode;
using capture_internal::CaptureOutcome;
using capture_internal::ExtractHostname;
using capture_internal::IsPrivateHost;
using capture_internal::IsPrivateIPv4;
using capture_internal::IsPrivateRemoteIp;
using capture_internal::IsValidCaptureUrl;
using capture_internal::ParseDottedIPv4;
using capture_internal::ResourceEntry;

namespace {

// Maximum body size for capture requests (16KB).
constexpr size_t kMaxCaptureBodySize = static_cast<size_t>(16 * 1024);

// ---------------------------------------------------------------------------
// Synchronous-looking capture via uv_async + condition variable.
//
// The HTTP server route handlers run on the libuv event loop thread.
// CDP operations are also on the event loop. We cannot block the event
// loop waiting for CDP results. Instead, we dispatch the CDP session
// via uv_queue_work's "after" callback pattern:
//
// Actually, since RouteHandler returns HttpResponse synchronously and
// CDP is async, we need a bridge.  We use a condition variable to
// block the route handler thread while the CDP work runs on the loop
// thread (the route handler is called from the loop thread in the
// current HttpServer design, so we need to pump the loop).
//
// HOWEVER: Looking at the HttpServer implementation, route handlers
// ARE called on the event loop thread (OnMessageComplete -> Dispatch).
// So we cannot block. Instead, we run the CDP flow synchronously by
// spinning the event loop with UV_RUN_ONCE until the result is ready.
// ---------------------------------------------------------------------------

// Shared state for an async capture operation.
struct CaptureState : public std::enable_shared_from_this<CaptureState> {
  CdpClient* client = nullptr;
  std::string url;
  uint32_t viewport_width = 1280;
  uint32_t viewport_height = 800;
  bool full_page = false;
  int timeout_ms = 30000;

  // CDP session state.
  std::string target_id;
  std::string session_id;
  bool completed = false;

  // Waterfall collection.
  std::unordered_map<std::string, ResourceEntry> resources;
  double navigation_start_time = 0.0;
  double load_event_time = 0.0;
  bool got_load_event = false;
  bool is_waterfall = false;
  // When false, abort if any response arrives from a private/loopback peer IP
  // (network-layer SSRF defense). Copied from CaptureContext::allow_private_urls.
  bool allow_private_urls = false;

  // Result.
  json result_json;
  std::string error_message;
  bool success = false;
  // Why a failed capture failed. A crashed renderer is a bad gateway (502);
  // everything else, including the navigation timeout, stays a gateway
  // timeout (504).
  bool renderer_crashed = false;
  // Called once, after Finish/FinishError's cleanup (StartCaptureForTest).
  std::function<void()> on_complete;

  // Timeout timer.
  uv_timer_t* timeout_timer = nullptr;

  void StartTimeout() {
    if (timeout_ms <= 0) return;
    timeout_timer = new uv_timer_t;
    int init_rc = uv_timer_init(client->loop(), timeout_timer);
    if (init_rc != 0) {
      fprintf(stderr, "[pagespeed] uv_timer_init failed: %s\n",
              uv_strerror(init_rc));
      delete timeout_timer;
      timeout_timer = nullptr;
      return;
    }
    auto weak = std::weak_ptr<CaptureState>(shared_from_this());
    timeout_timer->data = new std::weak_ptr<CaptureState>(weak);
    int rc = uv_timer_start(
        timeout_timer,
        [](uv_timer_t* t) {
          auto* wp = static_cast<std::weak_ptr<CaptureState>*>(t->data);
          auto s = wp->lock();
          delete wp;
          t->data = nullptr;
          if (s) {
            // For waterfall, a timeout after load event is fine --
            // return what we have.
            if (s->is_waterfall && s->got_load_event) {
              s->FinishWaterfall();
            } else {
              s->FinishError("navigation timeout");  // 504
            }
          }
          uv_close(reinterpret_cast<uv_handle_t*>(t),
                   [](uv_handle_t* h) { delete (uv_timer_t*)h; });
        },
        timeout_ms, 0);
    if (rc != 0) {
      fprintf(stderr, "[pagespeed] uv_timer_start failed: %s\n",
              uv_strerror(rc));
      auto* wp = static_cast<std::weak_ptr<CaptureState>*>(timeout_timer->data);
      delete wp;
      timeout_timer->data = nullptr;
      uv_close(reinterpret_cast<uv_handle_t*>(timeout_timer),
               [](uv_handle_t* h) { delete (uv_timer_t*)h; });
      timeout_timer = nullptr;
    }
  }

  bool Send(const CdpCommand& cmd, CdpResponseCallback cb) {
    auto status = client->SendCommand(cmd, std::move(cb));
    if (!status.ok()) {
      FinishError(
          absl::StrCat("SendCommand failed: ", status.status().message()));
      return false;
    }
    return true;
  }

  void Finish(json result) {
    if (completed) return;
    completed = true;
    success = true;
    result_json = std::move(result);
    Cleanup();
    if (on_complete) on_complete();
  }

  // `crashed` marks a renderer crash (502); every other failure is a 504.
  void FinishError(std::string_view msg, bool crashed = false) {
    if (completed) return;
    completed = true;
    success = false;
    renderer_crashed = crashed;
    error_message = std::string(msg);
    Cleanup();
    if (on_complete) on_complete();
  }

  void FinishWaterfall() {
    if (completed) return;

    // Build the result from collected resources.
    std::vector<ResourceEntry> entries;
    entries.reserve(resources.size());
    for (auto& [id, entry] : resources) {
      entries.push_back(std::move(entry));
    }

    double page_load_ms = 0.0;
    if (load_event_time > 0 && navigation_start_time > 0) {
      page_load_ms = (load_event_time - navigation_start_time) * 1000.0;
    }

    Finish(BuildWaterfallJson(entries, page_load_ms));
  }

  void FinishScreenshot(std::string_view base64_data) {
    if (completed) return;
    json result;
    result["data"] = base64_data;
    result["format"] = "png";
    result["viewport_width"] = viewport_width;
    result["viewport_height"] = viewport_height;
    Finish(std::move(result));
  }

  void Cleanup() {
    // Cancel timeout timer.
    if (timeout_timer != nullptr && timeout_timer->data != nullptr) {
      auto* wp = static_cast<std::weak_ptr<CaptureState>*>(timeout_timer->data);
      delete wp;
      timeout_timer->data = nullptr;
      uv_timer_stop(timeout_timer);
      uv_close(reinterpret_cast<uv_handle_t*>(timeout_timer),
               [](uv_handle_t* h) { delete (uv_timer_t*)h; });
      timeout_timer = nullptr;
    }

    // Close the target (best effort).
    if (!target_id.empty() && client->connected()) {
      CdpCommand close_cmd;
      close_cmd.method = "Target.closeTarget";
      close_cmd.params = {{"targetId", target_id}};
      close_cmd.timeout_ms = 5000;
      auto status = client->SendCommand(close_cmd, [](auto) {});
      (void)status;
    }

    // Release the event callback.
    client->SetEventCallback(nullptr);
  }

  // True for a CDP event saying this capture's target died or was torn down
  // under it.
  bool IsRendererCrashEvent(const CdpEvent& event) const {
    if (event.method == "Inspector.targetCrashed") {
      return !session_id.empty() && event.session_id == session_id;
    }
    if (event.method == "Target.targetCrashed") {
      return !target_id.empty() &&
             event.params.value("targetId", "") == target_id;
    }
    if (event.method == "Target.detachedFromTarget") {
      const std::string sid = event.params.value("sessionId", "");
      const std::string tid = event.params.value("targetId", "");
      return (!session_id.empty() && sid == session_id) ||
             (!target_id.empty() && tid == target_id);
    }
    return false;
  }

  // Handle a Network event for waterfall collection.
  void HandleNetworkEvent(const CdpEvent& event) {
    if (event.method == "Network.requestWillBeSent") {
      std::string request_id = event.params.value("requestId", "");
      if (request_id.empty()) return;

      ResourceEntry entry;
      entry.request_id = request_id;
      entry.url =
          event.params.value("request", json::object()).value("url", "");
      entry.method =
          event.params.value("request", json::object()).value("method", "GET");
      entry.resource_type = event.params.value("type", "Other");

      double timestamp = event.params.value("timestamp", 0.0);
      entry.request_time = timestamp;

      // Track navigation start time for page load calculation.
      if (navigation_start_time == 0.0 || timestamp < navigation_start_time) {
        navigation_start_time = timestamp;
      }

      resources[request_id] = std::move(entry);
    } else if (event.method == "Network.responseReceived") {
      std::string request_id = event.params.value("requestId", "");
      auto it = resources.find(request_id);
      if (it == resources.end()) return;

      auto& entry = it->second;
      auto resp = event.params.value("response", json::object());
      entry.status = resp.value("status", 0);
      entry.mime_type = resp.value("mimeType", "");
      entry.response_time = event.params.value("timestamp", 0.0);
      entry.encoded_data_length = resp.value("encodedDataLength", 0);

      // Extract detailed timing from response.timing.
      auto timing = resp.value("timing", json::object());
      if (!timing.empty()) {
        entry.dns_start = timing.value("dnsStart", -1.0);
        entry.dns_end = timing.value("dnsEnd", -1.0);
        entry.connect_start = timing.value("connectStart", -1.0);
        entry.connect_end = timing.value("connectEnd", -1.0);
        entry.ssl_start = timing.value("sslStart", -1.0);
        entry.ssl_end = timing.value("sslEnd", -1.0);
        entry.send_start = timing.value("sendStart", -1.0);
        entry.send_end = timing.value("sendEnd", -1.0);
        entry.receive_headers_end = timing.value("receiveHeadersEnd", -1.0);

        // requestTime from the timing object is more accurate.
        double req_time = resp.value("requestTime", 0.0);
        if (req_time > 0.0) entry.request_time = req_time;
      }
    } else if (event.method == "Network.loadingFinished") {
      std::string request_id = event.params.value("requestId", "");
      auto it = resources.find(request_id);
      if (it == resources.end()) return;

      it->second.finished = true;
      it->second.end_time = event.params.value("timestamp", 0.0);
      it->second.encoded_data_length = event.params.value(
          "encodedDataLength", it->second.encoded_data_length);
    } else if (event.method == "Network.loadingFailed") {
      std::string request_id = event.params.value("requestId", "");
      auto it = resources.find(request_id);
      if (it == resources.end()) return;
      it->second.finished = true;
      it->second.end_time = event.params.value("timestamp", 0.0);
    }
  }

  // Take screenshot once the page is idle.
  void TakeScreenshot() {
    json clip = {{"x", 0},
                 {"y", 0},
                 {"width", viewport_width},
                 {"height", viewport_height},
                 {"scale", 1}};

    CdpCommand screenshot_cmd;
    screenshot_cmd.method = "Page.captureScreenshot";
    screenshot_cmd.params = {{"format", "png"}};
    if (!full_page) {
      screenshot_cmd.params["clip"] = clip;
    } else {
      screenshot_cmd.params["captureBeyondViewport"] = true;
    }
    screenshot_cmd.session_id = session_id;
    screenshot_cmd.timeout_ms = 15000;

    auto self = shared_from_this();
    Send(screenshot_cmd, [self](absl::StatusOr<CdpResponse> result) {
      if (!result.ok() || result->is_error()) {
        self->FinishError("captureScreenshot failed");
        return;
      }

      std::string b64_data = result->result.value("data", "");
      if (b64_data.empty()) {
        self->FinishError("captureScreenshot: empty data");
        return;
      }

      self->FinishScreenshot(b64_data);
    });
  }
};

// Start the CDP session pipeline (shared by waterfall + screenshot).
// Steps: createTarget -> attachToTarget -> setDeviceMetrics ->
//         Network.enable -> Page.enable -> Page.navigate
void StartCdpSession(std::shared_ptr<CaptureState> state) {
  state->StartTimeout();

  // Step 1: Create target.
  CdpCommand create_cmd;
  create_cmd.method = "Target.createTarget";
  create_cmd.params = {{"url", "about:blank"}};
  create_cmd.timeout_ms = 10000;

  auto self = state;  // NOLINT(performance-unnecessary-copy-initialization)
  auto status = state->client->SendCommand(
      create_cmd, [self](absl::StatusOr<CdpResponse> result) {
        if (!result.ok() || result->is_error()) {
          self->FinishError("createTarget failed");
          return;
        }
        self->target_id = result->result.value("targetId", "");
        if (self->target_id.empty()) {
          self->FinishError("createTarget: no targetId");
          return;
        }

        // Step 2: Attach to target.
        CdpCommand attach_cmd;
        attach_cmd.method = "Target.attachToTarget";
        attach_cmd.params = {
            {"targetId", self->target_id},
            {"flatten", true},
        };
        attach_cmd.timeout_ms = 10000;

        self->Send(attach_cmd, [self](absl::StatusOr<CdpResponse> result) {
          if (!result.ok() || result->is_error()) {
            self->FinishError("attachToTarget failed");
            return;
          }
          self->session_id = result->result.value("sessionId", "");
          if (self->session_id.empty()) {
            self->FinishError("no sessionId");
            return;
          }

          // Step 3: Emulate the device (metrics, touch, UA).
          const uint32_t width = self->viewport_width;
          const uint32_t height = self->viewport_height;
          EmulateDevice(self, width, height, [self]() {
            // Step 4: Enable Network domain.
            CdpCommand net_cmd;
            net_cmd.method = "Network.enable";
            net_cmd.session_id = self->session_id;

            self->Send(net_cmd, [self](auto result) {
              if (!result.ok() || result->is_error()) {
                self->FinishError("Network.enable failed");
                return;
              }

              // Step 5: Enable Page + lifecycle events.
              CdpCommand page_cmd;
              page_cmd.method = "Page.enable";
              page_cmd.session_id = self->session_id;

              self->Send(page_cmd, [self](auto result) {
                if (!result.ok() || result->is_error()) {
                  self->FinishError("Page.enable failed");
                  return;
                }
                CdpCommand lc_cmd;
                lc_cmd.method = "Page.setLifecycleEventsEnabled";
                lc_cmd.params = {{"enabled", true}};
                lc_cmd.session_id = self->session_id;

                self->Send(lc_cmd, [self](auto result) {
                  if (!result.ok() || result->is_error()) {
                    self->FinishError("lifecycle enable failed");
                    return;
                  }

                  // Step 6: Navigate to the URL.
                  CdpCommand nav_cmd;
                  nav_cmd.method = "Page.navigate";
                  nav_cmd.params = {{"url", self->url}};
                  nav_cmd.session_id = self->session_id;
                  nav_cmd.timeout_ms = self->timeout_ms;

                  self->Send(nav_cmd, [self](auto result) {
                    if (!result.ok() || result->is_error()) {
                      self->FinishError("Page.navigate failed");
                      return;
                    }
                    // Navigation started.
                    // Events will drive completion.
                  });
                });
              });
            });
          });
        });
      });

  if (!status.ok()) {
    self->FinishError(
        absl::StrCat("SendCommand failed: ", status.status().message()));
    return;
  }

  // Set event callback for page lifecycle + network events.
  state->client->SetEventCallback([self](const CdpEvent& event) {
    if (self->completed) return;

    // A crashed or detached renderer is not a timeout: Chrome reports the
    // target's death with Inspector.targetCrashed (on the target's session),
    // Target.targetCrashed (browser session, carries targetId) or
    // Target.detachedFromTarget (carries sessionId and targetId). Fail the
    // capture at once and name the target.
    if (self->IsRendererCrashEvent(event)) {
      self->FinishError(
          absl::StrCat("browser renderer crashed (target ",
                       self->target_id.empty() ? "unknown" : self->target_id,
                       ", event ", event.method, ")"),
          /*crashed=*/true);
      return;
    }

    if (event.session_id != self->session_id) return;

    // SSRF defense (network layer): the literal-URL IsPrivateHost() pre-check
    // is bypassable via HTTP redirects and DNS rebinding, because Chrome
    // resolves DNS and follows 3xx responses itself. Re-check the *actual*
    // connection's remote IP on every response and abort the capture if it is
    // a private/loopback/link-local address — unless the caller explicitly
    // opted into private URLs. This catches both redirect-to-private and
    // public-name-to-private-IP, before any private content is returned.
    if (!self->allow_private_urls &&
        event.method == "Network.responseReceived") {
      std::string remote_ip = event.params.value("response", json::object())
                                  .value("remoteIPAddress", "");
      if (IsPrivateRemoteIp(remote_ip)) {
        self->FinishError("blocked: response from a private network address");
        return;
      }
    }

    // Collect network events for waterfall.
    if (self->is_waterfall) {
      self->HandleNetworkEvent(event);
    }

    // Page load events.
    if (event.method == "Page.loadEventFired") {
      self->got_load_event = true;
      self->load_event_time = event.params.value("timestamp", 0.0);

      if (self->is_waterfall) {
        // For waterfall, wait a bit more for late resources.
        // The timeout timer will finish collection.
        // But if most resources are done, finish now.
        bool all_done = true;
        for (const auto& [id, entry] : self->resources) {
          if (!entry.finished) {
            all_done = false;
            break;
          }
        }
        if (all_done) {
          self->FinishWaterfall();
        }
        // Otherwise wait for timeout or remaining resources.
      }
    }

    if (event.method == "Page.lifecycleEvent") {
      std::string name = event.params.value("name", "");
      if (name == "networkIdle") {
        if (self->is_waterfall) {
          self->FinishWaterfall();
        } else {
          // Screenshot: capture once idle.
          self->TakeScreenshot();
        }
      }
    }
  });
}

// Single-flight guard for captures. RunCapture spins the libuv loop, and the
// HTTP route handlers run on that same (single) loop thread, so a second
// capture dispatched while one is in flight would run reentrantly and clobber
// the CdpClient's single-slot event callback (and its Cleanup() would clear
// event delivery for the still-running capture). Serialize: allow only one
// capture at a time. Safe as a plain bool — all access is on the loop thread.
bool g_capture_in_flight = false;

class CaptureInFlightGuard {
 public:
  CaptureInFlightGuard() {
    if (!g_capture_in_flight) {
      g_capture_in_flight = true;
      acquired_ = true;
    }
  }
  ~CaptureInFlightGuard() {
    if (acquired_) g_capture_in_flight = false;
  }
  CaptureInFlightGuard(const CaptureInFlightGuard&) = delete;
  CaptureInFlightGuard& operator=(const CaptureInFlightGuard&) = delete;
  bool acquired() const { return acquired_; }

 private:
  bool acquired_ = false;
};

// Run a capture operation, spinning the event loop until complete.
// Must be called from the event loop thread.
void RunCapture(std::shared_ptr<CaptureState> state) {
  uv_loop_t* loop = state->client->loop();
  StartCdpSession(state);

  // Spin the event loop until the capture completes.
  while (!state->completed) {
    uv_run(loop, UV_RUN_ONCE);
  }
}

}  // namespace

namespace capture_internal {

void StartCaptureForTest(CdpClient* client, std::string url,
                         uint32_t viewport_width, uint32_t viewport_height,
                         bool is_waterfall, int timeout_ms,
                         std::function<void(const CaptureOutcome&)> done) {
  auto state = std::make_shared<CaptureState>();
  state->client = client;
  state->url = std::move(url);
  state->viewport_width = viewport_width;
  state->viewport_height = viewport_height;
  state->timeout_ms = timeout_ms;
  state->is_waterfall = is_waterfall;
  // The state owns its continuation; a weak self-reference keeps the two
  // from holding each other alive.
  std::weak_ptr<CaptureState> weak = state;
  state->on_complete = [weak, done = std::move(done)]() {
    auto s = weak.lock();
    if (!s || !done) return;
    CaptureOutcome outcome;
    outcome.success = s->success;
    outcome.renderer_crashed = s->renderer_crashed;
    outcome.error_message = s->error_message;
    outcome.result_json = s->result_json;
    done(outcome);
  };
  StartCdpSession(state);
}

}  // namespace capture_internal

// ---------------------------------------------------------------------------
// POST /v1/capture/waterfall
// ---------------------------------------------------------------------------

static HttpResponse HandleWaterfall(CaptureContext& ctx,
                                    const HttpRequest& request) {
  // Validate request body first (return 400 before checking Chrome).
  if (request.body.size() > kMaxCaptureBodySize) {
    return HttpResponse::Error(ApiErrorCode::kPayloadTooLarge,
                               "Request body too large");
  }
  if (JsonNestingTooDeep(request.body)) {
    return HttpResponse::Error(ApiErrorCode::kBadRequest,
                               "JSON nesting too deep");
  }

  json body;
  try {
    body = json::parse(request.body);
  } catch (const json::exception&) {
    return HttpResponse::Error(ApiErrorCode::kBadRequest, "Invalid JSON body");
  }

  if (!body.is_object()) {
    return HttpResponse::Error(ApiErrorCode::kBadRequest,
                               "Request body must be a JSON object");
  }

  std::string url = body.value("url", "");
  if (url.empty()) {
    return HttpResponse::Error(ApiErrorCode::kBadRequest,
                               "Missing required field: url");
  }
  if (!IsValidCaptureUrl(url)) {
    return HttpResponse::Error(ApiErrorCode::kBadRequest,
                               "URL must use http or https scheme");
  }
  if (!ctx.allow_private_urls && IsPrivateHost(url)) {
    return HttpResponse::Error(ApiErrorCode::kBadRequest,
                               "URLs targeting private/loopback "
                               "addresses are not allowed");
  }

  uint32_t viewport_width = body.value("viewport_width", 1280);
  if (viewport_width < 320 || viewport_width > 3840) {
    return HttpResponse::Error(ApiErrorCode::kBadRequest,
                               "viewport_width must be 320-3840");
  }

  // Check Chrome availability (after validation).
  if (ctx.browser_manager == nullptr) {
    return HttpResponse::Error(
        ApiErrorCode::kServiceUnavailable,
        "Browser analysis not enabled. Start worker with "
        "--enable-browser-analysis");
  }

  CdpClient* cdp = ctx.browser_manager->cdp_client();
  if (cdp == nullptr) {
    return HttpResponse::Error(ApiErrorCode::kServiceUnavailable,
                               "Chrome is not running. Browser analysis may be "
                               "initializing or Chrome crashed.");
  }

  // Create capture state.
  auto state = std::make_shared<CaptureState>();
  state->client = cdp;
  state->url = url;
  state->viewport_width = viewport_width;
  state->viewport_height = 800;  // Fixed for waterfall
  state->timeout_ms = ctx.navigation_timeout_ms;
  state->is_waterfall = true;
  state->allow_private_urls = ctx.allow_private_urls;

  // Only one capture may run at a time (it spins the loop reentrantly).
  CaptureInFlightGuard capture_guard;
  if (!capture_guard.acquired()) {
    return HttpResponse::Error(
        ApiErrorCode::kServiceUnavailable,
        "A capture is already in progress; retry shortly");
  }

  // Run the capture (spins event loop).
  RunCapture(state);

  if (!state->success) {
    CaptureOutcome outcome;
    outcome.renderer_crashed = state->renderer_crashed;
    return HttpResponse::Error(CaptureFailureCode(outcome),
                               state->error_message);
  }

  return HttpResponse().Json(DumpJson(state->result_json));
}

// ---------------------------------------------------------------------------
// POST /v1/capture/screenshot
// ---------------------------------------------------------------------------

static HttpResponse HandleScreenshot(CaptureContext& ctx,
                                     const HttpRequest& request) {
  // Validate request body first (return 400 before checking Chrome).
  if (request.body.size() > kMaxCaptureBodySize) {
    return HttpResponse::Error(ApiErrorCode::kPayloadTooLarge,
                               "Request body too large");
  }
  if (JsonNestingTooDeep(request.body)) {
    return HttpResponse::Error(ApiErrorCode::kBadRequest,
                               "JSON nesting too deep");
  }

  json body;
  try {
    body = json::parse(request.body);
  } catch (const json::exception&) {
    return HttpResponse::Error(ApiErrorCode::kBadRequest, "Invalid JSON body");
  }

  if (!body.is_object()) {
    return HttpResponse::Error(ApiErrorCode::kBadRequest,
                               "Request body must be a JSON object");
  }

  std::string url = body.value("url", "");
  if (url.empty()) {
    return HttpResponse::Error(ApiErrorCode::kBadRequest,
                               "Missing required field: url");
  }
  if (!IsValidCaptureUrl(url)) {
    return HttpResponse::Error(ApiErrorCode::kBadRequest,
                               "URL must use http or https scheme");
  }
  if (!ctx.allow_private_urls && IsPrivateHost(url)) {
    return HttpResponse::Error(ApiErrorCode::kBadRequest,
                               "URLs targeting private/loopback "
                               "addresses are not allowed");
  }

  uint32_t viewport_width = body.value("viewport_width", 1280);
  if (viewport_width < 320 || viewport_width > 3840) {
    return HttpResponse::Error(ApiErrorCode::kBadRequest,
                               "viewport_width must be 320-3840");
  }

  uint32_t viewport_height =
      body.value("viewport_height", static_cast<uint32_t>(800));
  if (viewport_height < 200 || viewport_height > 10000) {
    return HttpResponse::Error(ApiErrorCode::kBadRequest,
                               "viewport_height must be 200-10000");
  }

  bool full_page = body.value("full_page", false);

  // Check Chrome availability (after validation).
  if (ctx.browser_manager == nullptr) {
    return HttpResponse::Error(
        ApiErrorCode::kServiceUnavailable,
        "Browser analysis not enabled. Start worker with "
        "--enable-browser-analysis");
  }

  CdpClient* cdp = ctx.browser_manager->cdp_client();
  if (cdp == nullptr) {
    return HttpResponse::Error(ApiErrorCode::kServiceUnavailable,
                               "Chrome is not running. Browser analysis may be "
                               "initializing or Chrome crashed.");
  }

  // Create capture state.
  auto state = std::make_shared<CaptureState>();
  state->client = cdp;
  state->url = url;
  state->viewport_width = viewport_width;
  state->viewport_height = viewport_height;
  state->full_page = full_page;
  state->timeout_ms = ctx.navigation_timeout_ms;
  state->is_waterfall = false;
  state->allow_private_urls = ctx.allow_private_urls;

  // Only one capture may run at a time (it spins the loop reentrantly).
  CaptureInFlightGuard capture_guard;
  if (!capture_guard.acquired()) {
    return HttpResponse::Error(
        ApiErrorCode::kServiceUnavailable,
        "A capture is already in progress; retry shortly");
  }

  // Run the capture (spins event loop).
  RunCapture(state);

  if (!state->success) {
    CaptureOutcome outcome;
    outcome.renderer_crashed = state->renderer_crashed;
    return HttpResponse::Error(CaptureFailureCode(outcome),
                               state->error_message);
  }

  return HttpResponse().Json(DumpJson(state->result_json));
}

// ---------------------------------------------------------------------------
// Route registration
// ---------------------------------------------------------------------------

void RegisterCaptureRoutes(HttpServer& server, CaptureContext& ctx) {
  server.AddRoute(
      "POST", "/v1/capture/waterfall",
      [&ctx](const HttpRequest& req) { return HandleWaterfall(ctx, req); });

  server.AddRoute(
      "POST", "/v1/capture/screenshot",
      [&ctx](const HttpRequest& req) { return HandleScreenshot(ctx, req); });
}

}  // namespace pagespeed
