// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

// 4. Fetch.enable({patterns: [{urlPattern: "*"}]})
//    + Fetch.requestPaused handler to block all requests
// 5. Emulation.setDeviceMetricsOverride + setTouchEmulationEnabled
//    [+ setUserAgentOverride] (EmulateDevice, src/browser/device_emulation.h)
// 6. Page.enable, Page.navigate(about:blank) for a fresh window
//    (LoadFreshBlankDocument), Page.setLifecycleEventsEnabled
// 7. Page.getFrameTree -> frameId
// 8. Page.setDocumentContent({frameId, html})
// 9. Wait for Page.lifecycleEvent("networkIdle")
// 10. Runtime.evaluate to collect:
//     - All text node code points via TreeWalker
//     - All @font-face rules from document.styleSheets
// 11. Target.closeTarget({targetId})

#include "src/browser/font_glyph_scanner.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "absl/status/statusor.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/str_format.h"
#include "nlohmann/json.hpp"
#include "src/browser/cdp_client.h"
#include "src/browser/cdp_types.h"
#include "src/browser/device_emulation.h"
#include "src/browser/render_support.h"
#include "uv.h"

namespace pagespeed {

using json = nlohmann::json;

// JavaScript to collect font usage and text node code points.
// Uses captured JSON.stringify to prevent page script tampering.
static constexpr std::string_view kFontGlyphScript = R"JS(
(function() {
  var stringify = window.__PS_stringify || JSON.stringify;
  var result = {fonts: [], codepoints: [], text_nodes: 0};

  // 1. Walk all text nodes and collect unique code points.
  var cpSet = {};
  try {
    var walker = document.createTreeWalker(
        document.body || document.documentElement,
        NodeFilter.SHOW_TEXT, null);
    var node;
    while ((node = walker.nextNode())) {
      result.text_nodes++;
      var text = node.textContent || '';
      for (var i = 0; i < text.length; i++) {
        var cp = text.codePointAt(i);
        if (cp !== undefined && cp > 0) {
          cpSet[cp] = true;
          // Skip low surrogate for astral code points.
          if (cp > 0xFFFF) i++;
        }
      }
    }
  } catch(e) {}

  // Convert set to sorted array.
  var cpArr = [];
  for (var k in cpSet) {
    cpArr.push(parseInt(k, 10));
  }
  cpArr.sort(function(a, b) { return a - b; });
  result.codepoints = cpArr;

  // 2. Read @font-face rules from all stylesheets.
  try {
    var sheets = document.styleSheets;
    for (var s = 0; s < sheets.length; s++) {
      var rules;
      try { rules = sheets[s].cssRules || sheets[s].rules; }
      catch(e) { continue; }
      if (!rules) continue;
      for (var r = 0; r < rules.length; r++) {
        var rule = rules[r];
        if (rule.type !== CSSRule.FONT_FACE_RULE) continue;
        var style = rule.style;
        var family = style.getPropertyValue('font-family') || '';
        family = family.replace(/^['"]|['"]$/g, '');
        var src = style.getPropertyValue('src') || '';
        var weight = style.getPropertyValue('font-weight') || 'normal';
        var fstyle = style.getPropertyValue('font-style') || 'normal';

        // Parse src to extract URL and format.
        var srcUrl = '';
        var format = '';
        var urlMatch = src.match(/url\(['"]?([^'")]+)['"]?\)/);
        if (urlMatch) srcUrl = urlMatch[1];
        var fmtMatch = src.match(/format\(['"]?([^'")]+)['"]?\)/);
        if (fmtMatch) format = fmtMatch[1];

        result.fonts.push({
          family: family,
          src_url: srcUrl,
          format: format,
          weight: weight,
          style: fstyle
        });
      }
    }
  } catch(e) {}

  return stringify(result);
})();
)JS";

// JavaScript injected via Page.addScriptToEvaluateOnNewDocument to
// capture JSON.stringify before page scripts can override it.
static constexpr std::string_view kCaptureScript = R"JS(
(function() {
  window.__PS_stringify = JSON.stringify;
})();
)JS";

struct FontGlyphScanner::Session
    : public std::enable_shared_from_this<Session> {
  CdpClient* client;
  Callback callback;
  std::string html_content;
  uint32_t viewport_width;
  uint32_t viewport_height;
  uint32_t timeout_ms;

  std::string target_id;
  std::string session_id;
  std::string frame_id;
  bool got_network_idle = false;
  // The requested document is in the frame (Page.setDocumentContent
  // answered). The target starts at about:blank, and enabling lifecycle
  // events makes Chromium replay that blank document's lifecycle, its
  // networkIdle included, before the document is written: a networkIdle that
  // arrives before this is set describes the blank tab, and collecting on it
  // reads about:blank instead of the page.
  bool content_set = false;
  // Page.setDocumentContent was sent. Chromium replays the blank tab's
  // lifecycle while lifecycle events are enabled, before this is sent, so a
  // load event after it is the page's own. On a page with nothing left to
  // fetch that load arrives BEFORE the reply to setDocumentContent.
  bool content_sent = false;
  // The settle bound ran out before the reply (content_set): collect at the
  // reply.
  bool settled_before_reply = false;
  // kSettleAfterLoadMs, or the analyzer's test override.
  uint64_t settle_after_load_ms = kSettleAfterLoadMs;
  bool completed = false;
  // A refused Fetch.failRequest was reported already (FailPausedRequest).
  bool fetch_error_reported = false;

  // Overall session timeout timer.
  uv_timer_t* timeout_timer = nullptr;

  // Collect at the page's networkIdle or kSettleAfterLoadMs after its load
  // event, whichever comes first (OnPageSettled): a page that keeps a request
  // in flight at least every 500 ms never reaches networkIdle.
  OneShotTimer load_settle_timer;

  // Start the overall session timeout. Must be called after
  // the shared_ptr is fully constructed.
  void StartTimeout() {
    if (timeout_ms == 0) return;
    timeout_timer = new uv_timer_t;
    uv_timer_init(client->loop(), timeout_timer);
    // Use weak_ptr to avoid preventing Session cleanup.
    auto weak = std::weak_ptr<Session>(shared_from_this());
    timeout_timer->data = new std::weak_ptr<Session>(weak);
    uv_timer_start(
        timeout_timer,
        [](uv_timer_t* t) {
          auto* wp = static_cast<std::weak_ptr<Session>*>(t->data);
          // Lock before cleanup so Finish() sees data==nullptr and
          // skips its own timer teardown (avoids double-free + double
          // uv_close when Finish is called from this callback).
          auto s = wp->lock();
          delete wp;
          t->data = nullptr;
          if (s) {
            s->FinishError("session timeout");
          }
          uv_close(reinterpret_cast<uv_handle_t*>(t),
                   [](uv_handle_t* h) { delete (uv_timer_t*)h; });
        },
        timeout_ms, 0);
  }

  // Send a CDP command, aborting the session if the pipe is
  // disconnected. Returns false if the send failed.
  bool Send(const CdpCommand& cmd, CdpResponseCallback cb) {
    auto status = client->SendCommand(cmd, std::move(cb));
    if (!status.ok()) {
      FinishError(
          absl::StrCat("SendCommand failed: ", status.status().message()));
      return false;
    }
    return true;
  }

  void Finish(absl::StatusOr<FontGlyphResult> result) {
    if (completed) return;
    completed = true;

    load_settle_timer.Cancel();

    // Cancel the overall session timeout.
    if (timeout_timer != nullptr && timeout_timer->data != nullptr) {
      auto* wp = static_cast<std::weak_ptr<Session>*>(timeout_timer->data);
      delete wp;
      timeout_timer->data = nullptr;
      uv_timer_stop(timeout_timer);
      uv_close(reinterpret_cast<uv_handle_t*>(timeout_timer),
               [](uv_handle_t* h) { delete (uv_timer_t*)h; });
      timeout_timer = nullptr;
    }

    // Close the target (best effort).
    if (!target_id.empty() && client->connected()) {
      auto status =
          client->SendCommand(CdpCommand{.method = "Target.closeTarget",
                                         .params = {{"targetId", target_id}},
                                         .session_id = {},
                                         .timeout_ms = 5000},
                              [](const auto&) {});
      (void)status;  // Best effort cleanup.
    }

    // Release the event callback's strong reference to this session.
    client->SetEventCallback(nullptr);

    if (callback) {
      callback(std::move(result));
    }
  }

  void FinishError(std::string_view msg) { Finish(absl::InternalError(msg)); }

  // Collect results after networkIdle.
  // The page settled (networkIdle, or the settle bound after load): collect,
  // once.
  void OnPageSettled() {
    if (completed || got_network_idle) return;
    if (!content_set) {
      // Not before the document is written (the reply to setDocumentContent
      // collects then).
      settled_before_reply = true;
      return;
    }
    got_network_idle = true;
    load_settle_timer.Cancel();
    CollectResults();
  }

  void CollectResults() {
    CdpCommand eval_cmd;
    eval_cmd.method = "Runtime.evaluate";
    eval_cmd.params = {
        {"expression", std::string(kFontGlyphScript)},
        {"returnByValue", true},
    };
    eval_cmd.session_id = session_id;
    eval_cmd.timeout_ms = 10000;

    auto self = shared_from_this();
    Send(eval_cmd, [self](absl::StatusOr<CdpResponse> result) {
      if (!result.ok() || result->is_error()) {
        self->FinishError("Runtime.evaluate failed");
        return;
      }

      std::string value_str =
          result->result.value("result", json::object()).value("value", "");
      if (value_str.empty()) {
        self->FinishError("evaluate returned empty");
        return;
      }

      json data;
      try {
        data = json::parse(value_str);
      } catch (...) {
        self->FinishError("Failed to parse eval result");
        return;
      }

      FontGlyphResult glyph_result;
      glyph_result.total_text_nodes = data.value("text_nodes", 0u);

      // Parse code points.
      std::vector<uint32_t> all_codepoints;
      if (data.contains("codepoints") && data["codepoints"].is_array()) {
        for (const auto& cp : data["codepoints"]) {
          if (cp.is_number_unsigned()) {
            all_codepoints.push_back(cp.get<uint32_t>());
          }
        }
      }
      glyph_result.all_used_codepoints =
          CodePointsToUnicodeRange(all_codepoints);

      // Parse fonts.
      if (data.contains("fonts") && data["fonts"].is_array()) {
        for (const auto& f : data["fonts"]) {
          FontUsage usage;
          usage.family = f.value("family", "");
          usage.src_url = f.value("src_url", "");
          usage.format = f.value("format", "");
          usage.weight = f.value("weight", "");
          usage.style = f.value("style", "");
          // Assign the full codepoint range for now — subsetting
          // requires knowing which glyphs each font covers, which
          // would need font file parsing. As a practical estimate,
          // assign all used codepoints to each font.
          usage.unicode_range = glyph_result.all_used_codepoints;
          usage.used_glyphs = all_codepoints.size();
          glyph_result.fonts.push_back(std::move(usage));
        }
      }

      self->Finish(glyph_result);
    });
  }
};

FontGlyphScanner::FontGlyphScanner(CdpClient* client)
    : client_(client), settle_after_load_ms_(kSettleAfterLoadMs) {}

void FontGlyphScanner::Scan(std::string_view html_content,
                            uint32_t viewport_width, uint32_t viewport_height,
                            Callback callback, uint32_t timeout_ms) {
  auto session = std::make_shared<Session>();
  session->client = client_;
  session->callback = std::move(callback);
  session->html_content = std::string(html_content);
  session->viewport_width = viewport_width;
  session->viewport_height = viewport_height;
  session->timeout_ms = timeout_ms;
  session->settle_after_load_ms = settle_after_load_ms_;

  // Start overall session timeout.
  session->StartTimeout();

  // Step 1: Create target.
  CdpCommand create_cmd;
  create_cmd.method = "Target.createTarget";
  create_cmd.params = {{"url", "about:blank"}};
  create_cmd.timeout_ms = 10000;

  auto send_result = client_->SendCommand(
      create_cmd, [session](absl::StatusOr<CdpResponse> result) {
        if (!result.ok() || result->is_error()) {
          session->FinishError("createTarget failed");
          return;
        }
        session->target_id = result->result.value("targetId", "");
        if (session->target_id.empty()) {
          session->FinishError("createTarget: no targetId");
          return;
        }

        // Step 2: Attach to target.
        CdpCommand attach_cmd;
        attach_cmd.method = "Target.attachToTarget";
        attach_cmd.params = {
            {"targetId", session->target_id},
            {"flatten", true},
        };
        attach_cmd.timeout_ms = 10000;

        session->Send(attach_cmd, [session](
                                      absl::StatusOr<CdpResponse> result) {
          if (!result.ok() || result->is_error()) {
            session->FinishError("attachToTarget failed");
            return;
          }
          session->session_id = result->result.value("sessionId", "");
          if (session->session_id.empty()) {
            session->FinishError("no sessionId");
            return;
          }

          const auto& s = session;

          // Step 3: Network offline (SSRF defense —
          // MUST succeed).
          CdpCommand offline_cmd;
          offline_cmd.method = "Network.emulateNetworkConditions";
          offline_cmd.params = {
              {"offline", true},
              {"latency", 0},
              {"downloadThroughput", -1},
              {"uploadThroughput", -1},
          };
          offline_cmd.session_id = s->session_id;

          s->Send(offline_cmd, [s](auto result) {
            if (!result.ok() || result->is_error()) {
              s->FinishError("Network offline setup failed");
              return;
            }
            // Step 4: Enable Fetch interception (SSRF
            // defense — MUST succeed).
            CdpCommand fetch_cmd;
            fetch_cmd.method = "Fetch.enable";
            fetch_cmd.params = {
                {"patterns", json::array({{{"urlPattern", "*"}}})}};
            fetch_cmd.session_id = s->session_id;

            s->Send(fetch_cmd, [s](auto result) {
              if (!result.ok() || result->is_error()) {
                s->FinishError("Fetch.enable setup failed");
                return;
              }
              // Step 5: Emulate the device (metrics, touch, UA).
              EmulateDevice(s, s->viewport_width, s->viewport_height, [s]() {
                // Step 6: Inject capture script.
                CdpCommand script_cmd;
                script_cmd.method =
                    "Page."
                    "addScriptToEvaluateOnNewDocument";
                script_cmd.params = {{"source", std::string(kCaptureScript)}};
                script_cmd.session_id = s->session_id;

                s->Send(script_cmd, [s](auto result) {
                  if (!result.ok() || result->is_error()) {
                    s->FinishError("addScript setup failed");
                    return;
                  }
                  // Step 7: Enable Page + lifecycle.
                  CdpCommand page_cmd;
                  page_cmd.method = "Page.enable";
                  page_cmd.session_id = s->session_id;

                  s->Send(page_cmd, [s](auto result) {
                    if (!result.ok() || result->is_error()) {
                      s->FinishError("Page.enable failed");
                      return;
                    }
                    // A fresh window for the document (LoadFreshBlankDocument).
                    LoadFreshBlankDocument(s, [s]() {
                      CdpCommand lc_cmd;
                      lc_cmd.method =
                          "Page."
                          "setLifecycleEventsEnabled";
                      lc_cmd.params = {{"enabled", true}};
                      lc_cmd.session_id = s->session_id;

                      s->Send(lc_cmd, [s](auto result) {
                        if (!result.ok() || result->is_error()) {
                          s->FinishError("lifecycle enable failed");
                          return;
                        }
                        // Step 8: Get frame + set content.
                        CdpCommand tree_cmd;
                        tree_cmd.method = "Page.getFrameTree";
                        tree_cmd.session_id = s->session_id;

                        s->Send(
                            tree_cmd, [s](absl::StatusOr<CdpResponse> result) {
                              if (!result.ok() || result->is_error()) {
                                s->FinishError("getFrameTree failed");
                                return;
                              }
                              s->frame_id =
                                  result->result
                                      .value("frameTree", json::object())
                                      .value("frame", json::object())
                                      .value("id", "");

                              CdpCommand content_cmd;
                              content_cmd.method = "Page.setDocumentContent";
                              content_cmd.params = {
                                  {"frameId", s->frame_id},
                                  {"html", s->html_content},
                              };
                              content_cmd.session_id = s->session_id;
                              content_cmd.timeout_ms = 15000;

                              s->content_sent = true;
                              s->Send(content_cmd, [s](auto r) {
                                // Release HTML memory.
                                s->html_content.clear();
                                s->html_content.shrink_to_fit();
                                if (!r.ok() || r->is_error()) {
                                  s->FinishError(
                                      "setDocumentContent "
                                      "failed");
                                  return;
                                }
                                // From here a networkIdle describes the
                                // page.
                                s->content_set = true;
                                if (s->settled_before_reply) {
                                  s->OnPageSettled();
                                }
                              });
                            });
                      });
                    });
                  });
                });
              });
            });
          });
        });
      });

  if (!send_result.ok()) {
    session->FinishError(
        absl::StrCat("SendCommand failed: ", send_result.status().message()));
    return;
  }

  // Set event callback to handle lifecycle + Fetch events.
  client_->SetEventCallback([session](const CdpEvent& event) {
    if (session->completed) return;
    if (event.session_id != session->session_id) return;

    // Fetch interception: block all requests (SSRF defense).
    if (event.method == "Fetch.requestPaused") {
      FailPausedRequest(session, event.params.value("requestId", ""),
                        "font scan");
      return;
    }

    if (event.method == "Page.lifecycleEvent") {
      std::string name = event.params.value("name", "");
      // The replayed lifecycle of the blank tab is not the page's:
      // it all arrives before setDocumentContent is sent. The page
      // settles at its networkIdle (after the reply), or settle_after_load_ms
      // after its own load event if networkIdle never comes.
      if (name == "networkIdle" && session->content_set) {
        session->OnPageSettled();
      } else if (name == "load" && session->content_sent) {
        std::weak_ptr<Session> weak = session;
        session->load_settle_timer.Start(
            session->client->loop(), session->settle_after_load_ms, [weak]() {
              if (auto s = weak.lock()) s->OnPageSettled();
            });
      }
    }
  });
}

// static
std::string FontGlyphScanner::CodePointsToUnicodeRange(
    const std::vector<uint32_t>& codepoints) {
  if (codepoints.empty()) return "";

  // Sort a copy.
  std::vector<uint32_t> sorted = codepoints;
  std::sort(sorted.begin(), sorted.end());

  // Remove duplicates.
  sorted.erase(std::unique(sorted.begin(), sorted.end()), sorted.end());

  std::string result;
  size_t i = 0;
  while (i < sorted.size()) {
    uint32_t start = sorted[i];
    uint32_t end = start;

    // Extend range while consecutive.
    while (i + 1 < sorted.size() && sorted[i + 1] == end + 1) {
      end = sorted[++i];
    }

    if (!result.empty()) result += ',';

    if (start == end) {
      absl::StrAppendFormat(&result, "U+%04X", start);
    } else {
      absl::StrAppendFormat(&result, "U+%04X-%04X", start, end);
    }
    ++i;
  }
  return result;
}

}  // namespace pagespeed
