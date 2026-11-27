// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

// 4. Emulation.setScriptExecutionDisabled({value: true})
// 5. Emulation.setDeviceMetricsOverride + setTouchEmulationEnabled
//    [+ setUserAgentOverride] (EmulateDevice, src/browser/device_emulation.h)
// 6. Page.enable, CSS.enable, Page.navigate(about:blank) for a fresh window
//    (LoadFreshBlankDocument), Page.setLifecycleEventsEnabled
// 7. CSS.startRuleUsageTracking
// 8. Page.setDocumentContent({frameId, html})
// 9. Wait for Page.lifecycleEvent("firstContentfulPaint")
// 10. CSS.takeCoverageDelta -> critical rule byte-ranges at FCP
// 11. Wait for Page.lifecycleEvent("networkIdle")
// 12. CSS.stopRuleUsageTracking -> all used rule byte-ranges
// 13. For each styleSheetId: CSS.getStyleSheetText -> full text
// 14. Reconstruct critical/deferred/unused CSS from byte ranges
// 15. Target.closeTarget({targetId})

#include "src/browser/browser_css_extractor.h"

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "absl/container/flat_hash_set.h"
#include "absl/status/statusor.h"
#include "absl/strings/ascii.h"
#include "absl/strings/match.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/str_join.h"
#include "nlohmann/json.hpp"
#include "src/browser/cdp_client.h"
#include "src/browser/cdp_types.h"
#include "src/browser/device_emulation.h"
#include "src/browser/render_support.h"
#include "src/worker/critical_css_extractor.h"

namespace pagespeed {

using json = nlohmann::json;

// A coverage range within a stylesheet.
struct CoverageRange {
  std::string style_sheet_id;
  size_t start_offset = 0;
  size_t end_offset = 0;
};

// Per-extraction session state. Created for each Extract() call and
// destroyed when the extraction completes or fails.
struct BrowserCssExtractor::Session
    : public std::enable_shared_from_this<Session> {
  CdpClient* client;
  Callback callback;
  std::string html_content;
  uint32_t viewport_width;
  uint32_t viewport_height;
  uint32_t timeout_ms;

  // State accumulated during extraction.
  std::string target_id;
  std::string session_id;
  std::string frame_id;
  std::vector<CoverageRange> fcp_ranges;
  std::vector<CoverageRange> all_ranges;

  // Overall session timeout timer.
  uv_timer_t* timeout_timer = nullptr;

  // Track whether we've received lifecycle events.
  bool got_fcp = false;
  bool fcp_delta_done = false;  // FCP coverage delta response received
  bool got_network_idle = false;
  bool stop_requested = false;  // CSS.stopRuleUsageTracking already sent
  bool completed = false;
  // A refused Fetch.failRequest was reported already (FailPausedRequest).
  bool fetch_error_reported = false;

  // Start the overall session timeout. Must be called after
  // the shared_ptr is fully constructed (i.e., from Extract(),
  // not from the constructor).
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
  // disconnected. Returns false if the send failed (session
  // already finished with an error).
  bool Send(const CdpCommand& cmd, CdpResponseCallback cb) {
    auto status = client->SendCommand(cmd, std::move(cb));
    if (!status.ok()) {
      FinishError(
          absl::StrCat("SendCommand failed: ", status.status().message()));
      return false;
    }
    return true;
  }

  // Called when BOTH fcp_delta_done AND got_network_idle are true.
  void MaybeProceedToStopTracking() {
    if (!fcp_delta_done || !got_network_idle || completed || stop_requested) {
      return;
    }
    stop_requested = true;

    // Step 12: Stop tracking -> all used ranges.
    CdpCommand stop_cmd;
    stop_cmd.method = "CSS.stopRuleUsageTracking";
    stop_cmd.session_id = session_id;

    auto self = shared_from_this();
    Send(stop_cmd,
         [self](auto result) { self->HandleStopTracking(std::move(result)); });
  }

  void HandleStopTracking(absl::StatusOr<CdpResponse> result);

  void Finish(absl::StatusOr<BrowserCssResult> result) {
    if (std::exchange(completed, true)) return;

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

    // Close the target (best effort — session is already done,
    // so failure to send is not actionable).
    if (!target_id.empty() && client->connected()) {
      CdpCommand close_cmd;
      close_cmd.method = "Target.closeTarget";
      close_cmd.params = {{"targetId", target_id}};
      close_cmd.timeout_ms = 5000;
      auto status = client->SendCommand(close_cmd, [](auto) {});
      (void)status;  // Best effort cleanup.
    }

    // Release the event callback's strong reference to this session,
    // so the Session is freed promptly instead of being held alive
    // until the next SetEventCallback() or CdpClient destruction.
    client->SetEventCallback(nullptr);

    if (callback) {
      callback(std::move(result));
    }
  }

  void FinishError(std::string_view msg) { Finish(absl::InternalError(msg)); }
};

namespace {

// Parse coverage ranges from a JSON array returned by
// CSS.takeCoverageDelta or CSS.stopRuleUsageTracking.
std::vector<CoverageRange> ParseCoverageRanges(const json& coverage) {
  std::vector<CoverageRange> ranges;
  if (!coverage.is_array()) return ranges;
  for (const auto& entry : coverage) {
    if (!entry.value("used", false)) {
      continue;
    }
    CoverageRange range;
    range.style_sheet_id = entry.value("styleSheetId", "");
    range.start_offset = entry.value("startOffset", 0u);
    range.end_offset = entry.value("endOffset", 0u);
    if (!range.style_sheet_id.empty() &&
        range.end_offset > range.start_offset) {
      ranges.push_back(std::move(range));
    }
  }
  return ranges;
}

// Merge overlapping ranges for the same stylesheet.
void MergeRanges(std::vector<CoverageRange>& ranges) {
  if (ranges.size() < 2) return;

  std::sort(ranges.begin(), ranges.end(),
            [](const CoverageRange& a, const CoverageRange& b) {
              if (a.style_sheet_id != b.style_sheet_id) {
                return a.style_sheet_id < b.style_sheet_id;
              }
              return a.start_offset < b.start_offset;
            });

  std::vector<CoverageRange> merged;
  merged.push_back(ranges[0]);

  for (size_t i = 1; i < ranges.size(); ++i) {
    auto& last = merged.back();
    if (ranges[i].style_sheet_id == last.style_sheet_id &&
        ranges[i].start_offset <= last.end_offset) {
      last.end_offset = std::max(last.end_offset, ranges[i].end_offset);
    } else {
      merged.push_back(ranges[i]);
    }
  }
  ranges = std::move(merged);
}

// Extract CSS text at the given byte ranges from stylesheet text.
std::string ExtractRanges(const std::string& sheet_text,
                          const std::vector<CoverageRange>& ranges,
                          const std::string& sheet_id) {
  std::string result;
  for (const auto& range : ranges) {
    if (range.style_sheet_id != sheet_id) continue;
    if (range.start_offset >= sheet_text.size()) continue;
    size_t end = std::min(range.end_offset, sheet_text.size());
    if (end > range.start_offset) {
      if (!result.empty()) result += '\n';
      result.append(sheet_text, range.start_offset, end - range.start_offset);
    }
  }
  return result;
}

// Compute CSS text NOT covered by any range.
std::string ExtractUnusedRanges(const std::string& sheet_text,
                                const std::vector<CoverageRange>& used_ranges,
                                const std::string& sheet_id) {
  // Build a bitmap of used bytes (for correctness; simpler than
  // gap calculation with overlapping ranges).
  std::vector<bool> used(sheet_text.size(), false);
  for (const auto& range : used_ranges) {
    if (range.style_sheet_id != sheet_id) continue;
    size_t end = std::min(range.end_offset, sheet_text.size());
    for (size_t i = range.start_offset; i < end; ++i) {
      used[i] = true;
    }
  }

  // Extract contiguous unused regions.
  std::string result;
  size_t i = 0;
  while (i < sheet_text.size()) {
    if (!used[i]) {
      size_t start = i;
      while (i < sheet_text.size() && !used[i]) ++i;
      // Only include non-whitespace-only regions.
      std::string_view region(sheet_text.data() + start, i - start);
      bool all_whitespace = true;
      for (char c : region) {
        if (c != ' ' && c != '\t' && c != '\n' && c != '\r') {
          all_whitespace = false;
          break;
        }
      }
      if (!all_whitespace) {
        if (!result.empty()) result += '\n';
        result.append(region);
      }
    } else {
      ++i;
    }
  }
  return result;
}

// Compute deferred ranges: ranges in `all` that are NOT in `fcp`.
// Both inputs must be sorted and merged.
std::vector<CoverageRange> ComputeDeferredRanges(
    const std::vector<CoverageRange>& all,
    const std::vector<CoverageRange>& fcp, const std::string& sheet_id) {
  std::vector<CoverageRange> result;
  // Collect FCP ranges for this sheet.
  std::vector<std::pair<size_t, size_t>> fcp_intervals;
  for (const auto& r : fcp) {
    if (r.style_sheet_id == sheet_id) {
      fcp_intervals.emplace_back(r.start_offset, r.end_offset);
    }
  }
  // For each range in `all` for this sheet, subtract FCP intervals.
  for (const auto& r : all) {
    if (r.style_sheet_id != sheet_id) continue;
    size_t start = r.start_offset;
    size_t end = r.end_offset;
    for (const auto& [fs, fe] : fcp_intervals) {
      if (fs >= end || fe <= start) continue;  // No overlap.
      // There is overlap — emit the part before the FCP range.
      if (start < fs) {
        CoverageRange cr;
        cr.style_sheet_id = sheet_id;
        cr.start_offset = start;
        cr.end_offset = fs;
        result.push_back(std::move(cr));
      }
      start = std::max(start, fe);
    }
    if (start < end) {
      CoverageRange cr;
      cr.style_sheet_id = sheet_id;
      cr.start_offset = start;
      cr.end_offset = end;
      result.push_back(std::move(cr));
    }
  }
  return result;
}

// Layer name from a `@layer <name>` prelude (trimmed; anonymous -> "").
// Must format identically to the worker-side ExtractLayerName so identities
// key-match across the two parsers.
std::string CoverageLayerName(std::string_view prelude) {
  return std::string(
      absl::StripAsciiWhitespace(prelude.substr(6)));  // "@layer"
}

// Normalized @media condition from a `@media <cond>` prelude: whitespace-
// collapsed and lowercased, identical to the worker-side ExtractMediaCondition.
std::string CoverageMediaCondition(std::string_view prelude) {
  std::string_view rest =
      absl::StripAsciiWhitespace(prelude.substr(6));  // media
  std::string out;
  out.reserve(rest.size());
  bool in_ws = false;
  for (char c : rest) {
    if (std::isspace(static_cast<unsigned char>(c)) != 0) {
      if (!in_ws) {
        out += ' ';
        in_ws = true;
      }
    } else {
      out += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
      in_ws = false;
    }
  }
  return out;
}

}  // namespace

CriticalCssResult DeriveDomMatchedCriticalCss(
    const std::vector<CollectedElement>& elements,
    std::string_view combined_css, std::string_view profile_critical_css,
    CapabilityMask::Viewport viewport,
    const std::vector<std::string>& measured_above_fold_selectors,
    const CascadeLayerOrder& layer_order) {
  absl::flat_hash_set<RuleIdentity> coverage_identities =
      BuildCoverageIdentities(profile_critical_css);
  CriticalCssConfig config;
  config.measured_above_fold_selectors = measured_above_fold_selectors;
  CriticalCssExtractor extractor(std::move(config));
  return extractor.Extract(elements, combined_css, viewport,
                           &coverage_identities, &layer_order);
}

absl::flat_hash_set<RuleIdentity> BuildCoverageIdentities(
    std::string_view css) {
  absl::flat_hash_set<RuleIdentity> out;

  // Frame types for the open-block stack. LAYER/MEDIA contribute context; STYLE
  // and OTHER (@supports/@font-face/@keyframes/...) do not.
  enum class Kind : std::uint8_t { kLayer, kMedia, kStyle, kOther };
  std::vector<Kind> stack;
  std::vector<std::string> layer_stack;
  std::vector<std::string> media_stack;

  const size_t n = css.size();
  size_t prelude_start = 0;
  for (size_t i = 0; i < n; ++i) {
    char c = css[i];

    // An escape is part of an ident: an escaped quote opens no string and an
    // escaped brace no block (`.a\'b`, Tailwind v4's `[class*=\'size-\']`;
    // CSS Syntax 3 §4.3.7).
    if (c == '\\') {
      ++i;
      continue;
    }
    // Skip CSS strings so braces/semicolons inside them do not corrupt nesting.
    if (c == '"' || c == '\'') {
      char quote = c;
      ++i;
      while (i < n) {
        if (css[i] == '\\') {
          // An escaped newline continues the string; CRLF is one newline.
          i += (i + 2 < n && css[i + 1] == '\r' && css[i + 2] == '\n') ? 3 : 2;
          continue;
        }
        if (css[i] == quote) break;
        // An unescaped newline ends a bad string (CSS Syntax 3 §4.3.5); the
        // newline itself is not part of it.
        if (css[i] == '\n' || css[i] == '\r' || css[i] == '\f') {
          --i;
          break;
        }
        ++i;
      }
      continue;
    }
    // Skip comments.
    if (c == '/' && i + 1 < n && css[i + 1] == '*') {
      i += 2;
      while (i + 1 < n && !(css[i] == '*' && css[i + 1] == '/')) ++i;
      ++i;  // land on '/', loop ++i moves past
      continue;
    }

    if (c == '{') {
      std::string_view prelude = absl::StripAsciiWhitespace(
          css.substr(prelude_start, i - prelude_start));
      if (absl::StartsWithIgnoreCase(prelude, "@layer")) {
        stack.push_back(Kind::kLayer);
        layer_stack.push_back(CoverageLayerName(prelude));
      } else if (absl::StartsWithIgnoreCase(prelude, "@media")) {
        stack.push_back(Kind::kMedia);
        media_stack.push_back(CoverageMediaCondition(prelude));
      } else if (!prelude.empty() && prelude.front() == '@') {
        // @supports / @font-face / @keyframes / ... — transparent, no selector.
        stack.push_back(Kind::kOther);
      } else {
        // Plain style rule: emit an identity under the current context.
        if (!prelude.empty()) {
          out.insert(RuleIdentity{absl::StrJoin(layer_stack, ">"),
                                  absl::StrJoin(media_stack, " and "),
                                  NormalizeSelector(prelude)});
        }
        stack.push_back(Kind::kStyle);
      }
      prelude_start = i + 1;
      continue;
    }

    if (c == '}') {
      // Tolerate unbalanced input: an extra '}' with an empty stack is ignored.
      if (!stack.empty()) {
        Kind k = stack.back();
        stack.pop_back();
        if (k == Kind::kLayer && !layer_stack.empty()) layer_stack.pop_back();
        if (k == Kind::kMedia && !media_stack.empty()) media_stack.pop_back();
      }
      prelude_start = i + 1;
      continue;
    }

    if (c == ';') {
      // Statement at-rule with no block (`@layer a,b,c;`, `@import ...;`) or a
      // stray declaration fragment from a partial slice: reset the prelude.
      prelude_start = i + 1;
      continue;
    }
  }

  return out;
}

BrowserCssExtractor::BrowserCssExtractor(CdpClient* client) : client_(client) {}

void BrowserCssExtractor::Extract(std::string_view html_content,
                                  uint32_t viewport_width,
                                  uint32_t viewport_height, Callback callback,
                                  uint32_t timeout_ms) {
  auto session = std::make_shared<Session>();
  session->client = client_;
  session->callback = std::move(callback);
  session->html_content = std::string(html_content);
  session->viewport_width = viewport_width;
  session->viewport_height = viewport_height;
  session->timeout_ms = timeout_ms;

  // Start overall session timeout.
  session->StartTimeout();

  // Step 1: Create a new browser target.
  CdpCommand create_cmd;
  create_cmd.method = "Target.createTarget";
  create_cmd.params = {{"url", "about:blank"}};
  create_cmd.timeout_ms = 10000;

  auto send_result = client_->SendCommand(
      create_cmd, [session](absl::StatusOr<CdpResponse> result) {
        if (!result.ok()) {
          session->FinishError(
              absl::StrCat("createTarget failed: ", result.status().message()));
          return;
        }
        if (result->is_error()) {
          session->FinishError(
              absl::StrCat("createTarget error: ", result->error_message));
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
            session->FinishError("attachToTarget: no sessionId");
            return;
          }

          // Step 3-7: Setup and start tracking.
          const auto& s = session;

          // 3a. Network offline (SSRF defense — MUST
          // succeed)
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
            // 3b. Disable JavaScript (SSRF defense)
            CdpCommand js_cmd;
            js_cmd.method = "Emulation.setScriptExecutionDisabled";
            js_cmd.params = {{"value", true}};
            js_cmd.session_id = s->session_id;

            s->Send(js_cmd, [s](auto result) {
              if (!result.ok() || result->is_error()) {
                s->FinishError("JS disable setup failed");
                return;
              }
              // 3c. Fetch interception (defense
              // in depth — fail all requests)
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
                // 4. Emulate the device (metrics, touch, UA)
                EmulateDevice(s, s->viewport_width, s->viewport_height, [s]() {
                  // 5. Enable domains
                  CdpCommand page_cmd;
                  page_cmd.method = "Page.enable";
                  page_cmd.session_id = s->session_id;

                  s->Send(page_cmd, [s](auto result) {
                    if (!result.ok() || result->is_error()) {
                      s->FinishError("Page.enable failed");
                      return;
                    }
                    // 5b. DOM.enable (required before CSS.enable)
                    CdpCommand dom_cmd;
                    dom_cmd.method = "DOM.enable";
                    dom_cmd.session_id = s->session_id;

                    s->Send(dom_cmd, [s](auto result) {
                      if (!result.ok() || result->is_error()) {
                        s->FinishError("DOM.enable failed");
                        return;
                      }
                      CdpCommand css_cmd;
                      css_cmd.method = "CSS.enable";
                      css_cmd.session_id = s->session_id;

                      s->Send(css_cmd, [s](auto result) {
                        if (!result.ok() || result->is_error()) {
                          s->FinishError("CSS.enable failed");
                          return;
                        }
                        // 6. A fresh window for the document
                        // (LoadFreshBlankDocument), then lifecycle events.
                        LoadFreshBlankDocument(s, [s]() {
                          CdpCommand lifecycle_cmd;
                          lifecycle_cmd.method =
                              "Page.setLifecycleEventsEnabled";
                          lifecycle_cmd.params = {{"enabled", true}};
                          lifecycle_cmd.session_id = s->session_id;

                          s->Send(lifecycle_cmd, [s](auto result) {
                            if (!result.ok() || result->is_error()) {
                              s->FinishError("lifecycle enable failed");
                              return;
                            }
                            // 7. Start rule usage tracking
                            CdpCommand track_cmd;
                            track_cmd.method = "CSS.startRuleUsageTracking";
                            track_cmd.session_id = s->session_id;

                            s->Send(track_cmd, [s](auto result) {
                              if (!result.ok() || result->is_error()) {
                                s->FinishError("startRuleUsageTracking failed");
                                return;
                              }

                              // 8. Get the main frame ID and set content.
                              CdpCommand tree_cmd;
                              tree_cmd.method = "Page.getFrameTree";
                              tree_cmd.session_id = s->session_id;

                              s->Send(
                                  tree_cmd,
                                  [s](absl::StatusOr<CdpResponse> result) {
                                    if (!result.ok() || result->is_error()) {
                                      s->FinishError("getFrameTree failed");
                                      return;
                                    }

                                    // Extract frameId from the tree.
                                    s->frame_id =
                                        result->result
                                            .value("frameTree", json::object())
                                            .value("frame", json::object())
                                            .value("id", "");

                                    if (s->frame_id.empty()) {
                                      s->FinishError(
                                          "getFrameTree: no frameId");
                                      return;
                                    }

                                    CdpCommand set_content_cmd;
                                    set_content_cmd.method =
                                        "Page.setDocumentContent";
                                    set_content_cmd.params = {
                                        {"frameId", s->frame_id},
                                        {"html", s->html_content},
                                    };
                                    set_content_cmd.session_id = s->session_id;
                                    set_content_cmd.timeout_ms = 15000;

                                    s->Send(set_content_cmd, [s](auto result) {
                                      if (!result.ok() || result->is_error()) {
                                        s->FinishError(
                                            "setDocumentContent failed");
                                        return;
                                      }
                                      // Content is set. Now we wait for
                                      // lifecycle events via the event
                                      // callback. The event handler is
                                      // set up before Extract() returns.
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
          });
        });
      });

  if (!send_result.ok()) {
    session->FinishError(
        absl::StrCat("SendCommand failed: ", send_result.status().message()));
    return;
  }

  // Set up the event callback to watch for lifecycle events.
  // Note: this overwrites any previous event callback. In practice,
  // only one extraction runs at a time per CdpClient.
  client_->SetEventCallback([session](const CdpEvent& event) {
    if (session->completed) return;
    if (event.session_id != session->session_id) return;

    // Fetch interception: fail all requests (defense in
    // depth alongside offline mode for CSS-only extraction).
    if (event.method == "Fetch.requestPaused") {
      FailPausedRequest(session, event.params.value("requestId", ""),
                        "css coverage render");
      return;
    }

    if (event.method == "Page.lifecycleEvent") {
      std::string name = event.params.value("name", "");

      if (name == "firstContentfulPaint" && !session->got_fcp) {
        session->got_fcp = true;

        // Take coverage delta at FCP.
        CdpCommand delta_cmd;
        delta_cmd.method = "CSS.takeCoverageDelta";
        delta_cmd.session_id = session->session_id;

        session->Send(delta_cmd, [session](absl::StatusOr<CdpResponse> result) {
          if (!result.ok() || result->is_error()) {
            session->FinishError("takeCoverageDelta failed");
            return;
          }
          session->fcp_ranges = ParseCoverageRanges(
              result->result.value("coverage", json::array()));
          MergeRanges(session->fcp_ranges);
          session->fcp_delta_done = true;
          // If networkIdle already fired, proceed.
          session->MaybeProceedToStopTracking();
        });
      } else if (name == "networkIdle" && !session->got_network_idle) {
        session->got_network_idle = true;
        // Wait for FCP delta to complete before stopping.
        session->MaybeProceedToStopTracking();
      }
    }
  });
}

// HandleStopTracking processes the CSS.stopRuleUsageTracking
// response: collects stylesheet texts and reconstructs CSS.
void BrowserCssExtractor::Session::HandleStopTracking(
    absl::StatusOr<CdpResponse> result) {
  if (!result.ok() || result->is_error()) {
    FinishError("stopRuleUsageTracking failed");
    return;
  }
  all_ranges =
      ParseCoverageRanges(result->result.value("ruleUsage", json::array()));
  MergeRanges(all_ranges);

  // Release html_content — no longer needed.
  html_content.clear();
  html_content.shrink_to_fit();

  // Collect unique stylesheet IDs from both range sets.
  absl::flat_hash_set<std::string> id_set;
  for (const auto& r : all_ranges) {
    id_set.insert(r.style_sheet_id);
  }
  for (const auto& r : fcp_ranges) {
    id_set.insert(r.style_sheet_id);
  }

  if (id_set.empty()) {
    BrowserCssResult css_result;
    Finish(css_result);
    return;
  }

  std::vector<std::string> sheet_ids(id_set.begin(), id_set.end());

  // Fetch all stylesheet texts concurrently.
  auto texts =
      std::make_shared<std::vector<std::pair<std::string, std::string>>>();
  auto remaining = std::make_shared<int>(static_cast<int>(sheet_ids.size()));
  auto self = shared_from_this();

  for (const auto& sheet_id : sheet_ids) {
    CdpCommand text_cmd;
    text_cmd.method = "CSS.getStyleSheetText";
    text_cmd.params = {{"styleSheetId", sheet_id}};
    text_cmd.session_id = session_id;

    if (!Send(text_cmd, [self, texts, remaining,
                         sheet_id](absl::StatusOr<CdpResponse> result) {
          if (result.ok() && !result->is_error()) {
            std::string text = result->result.value("text", "");
            texts->emplace_back(sheet_id, std::move(text));
          }

          (*remaining)--;
          if (*remaining > 0) return;

          // All stylesheet texts collected — reconstruct CSS.
          BrowserCssResult css_result;

          // Count each unique stylesheet text once. Identical sheets surface as
          // distinct Chrome stylesheet ids when the analysis HTML carries both
          // an external <link> and an inlined <style> copy of the same sheet
          // (css_cache_inliner is append-only and leaves the original <link>).
          // Without this guard the critical/deferred/unused blocks AND
          // total_css_bytes are emitted twice — the prod critical-CSS
          // double-ship (13.2KB = 2x the 6.5KB critical subset).
          //
          // Dedup is byte-exact: it collapses the dominant case (a render-
          // blocking / media-"all" sheet, no @import — the inlined copy is byte-
          // identical to the external sheet). It does NOT collapse a media-
          // scoped <link> (css_cache_inliner wraps the copy in @media{...}) or an
          // @import sheet (the inliner flattens), where inline != external. The
          // structural root fix is to drop/neutralize the original <link> in
          // css_cache_inliner so Chrome loads each logical sheet exactly once;
          // this guard then becomes belt-and-braces. See follow-up.
          absl::flat_hash_set<std::string_view> seen_sheet_texts;
          for (const auto& [id, text] : *texts) {
            if (!seen_sheet_texts.insert(text).second) continue;
            css_result.total_css_bytes += text.size();

            std::string critical = ExtractRanges(text, self->fcp_ranges, id);
            if (!critical.empty()) {
              if (!css_result.critical_css.empty()) {
                css_result.critical_css += '\n';
              }
              css_result.critical_css += critical;
            }

            // Deferred = ranges used at load but NOT at FCP.
            auto deferred_ranges =
                ComputeDeferredRanges(self->all_ranges, self->fcp_ranges, id);
            std::string deferred = ExtractRanges(text, deferred_ranges, id);
            if (!deferred.empty()) {
              if (!css_result.deferred_css.empty()) {
                css_result.deferred_css += '\n';
              }
              css_result.deferred_css += deferred;
            }

            std::string unused =
                ExtractUnusedRanges(text, self->all_ranges, id);
            if (!unused.empty()) {
              if (!css_result.unused_css.empty()) {
                css_result.unused_css += '\n';
              }
              css_result.unused_css += unused;
            }
          }

          css_result.critical_css_bytes = css_result.critical_css.size();
          if (css_result.total_css_bytes > 0) {
            css_result.coverage_ratio =
                static_cast<float>(css_result.critical_css_bytes) /
                static_cast<float>(css_result.total_css_bytes);
          }

          self->Finish(css_result);
        }))
      return;
  }
}

}  // namespace pagespeed
