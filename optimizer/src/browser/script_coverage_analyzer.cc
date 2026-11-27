// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

// 4. Fetch.enable({patterns: [{urlPattern: "*"}]})
//    + Fetch.requestPaused handler to serve from cache
// 5. Emulation.setDeviceMetricsOverride + setTouchEmulationEnabled
//    [+ setUserAgentOverride] (EmulateDevice, src/browser/device_emulation.h)
// 6. Profiler.enable
// 7. Profiler.startPreciseCoverage({callCount: false, detailed: true})
// 8. Page.enable, Page.navigate(about:blank) for a fresh window
//    (LoadFreshBlankDocument), Page.setLifecycleEventsEnabled
// 9. Page.getFrameTree -> frameId
// 10. Page.setDocumentContent({frameId, html})
// 11. Wait for Page.lifecycleEvent("firstContentfulPaint")
// 12. Profiler.takePreciseCoverage -> scripts_at_fcp
// 13. Wait for Page.lifecycleEvent("networkIdle")
// 14. Profiler.takePreciseCoverage -> scripts_at_load
// 15. Runtime.evaluate -> script element info (async/defer/module,
//     document.write detection)
// 16. Profiler.stopPreciseCoverage
// 17. Target.closeTarget({targetId})

#include "src/browser/script_coverage_analyzer.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "absl/container/flat_hash_map.h"
#include "absl/container/flat_hash_set.h"
#include "absl/status/statusor.h"
#include "absl/strings/str_cat.h"
#include "nlohmann/json.hpp"
#include "src/browser/cdp_client.h"
#include "src/browser/cdp_types.h"
#include "src/browser/cdp_utils.h"
#include "src/browser/device_emulation.h"
#include "src/browser/render_support.h"
#include "uv.h"

namespace pagespeed {

using json = nlohmann::json;

// JavaScript to collect script element information from the DOM.
// Detects async/defer/module attributes and document.write usage.
static constexpr std::string_view kCollectScriptsScript = R"JS(
(function() {
  var stringify = window.__PS_stringify || JSON.stringify;
  var scripts = document.querySelectorAll('script');
  var result = [];
  for (var i = 0; i < scripts.length; i++) {
    var s = scripts[i];
    var url = s.src || '';
    var isInline = false;
    if (!url) {
      if (!s.textContent) continue;
      isInline = true;
    }
    var sel = 'script';
    if (s.id) sel += '#' + s.id;
    if (s.className && typeof s.className === 'string') {
      sel += '.' + s.className.trim().split(/\s+/).join('.');
    }
    var hasDocWrite = false;
    if (s.textContent) {
      hasDocWrite = /document\.write/.test(s.textContent);
    }
    result.push({
      url: url,
      selector: sel,
      is_inline: isInline,
      is_async: s.async || false,
      is_defer: s.defer || false,
      is_module: (s.type === 'module'),
      has_document_write: hasDocWrite
    });
  }
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

namespace {

// Per-script coverage data with properly merged used ranges.
struct ScriptCov {
  size_t total_bytes = 0;
  size_t used_bytes = 0;
  // Collected used ranges (start, end) before merging.
  std::vector<std::pair<size_t, size_t>> used_ranges;
};

// Sort and merge overlapping ranges, return total used bytes.
size_t MergeRangesAndComputeUsed(
    std::vector<std::pair<size_t, size_t>> ranges) {
  if (ranges.empty()) return 0;
  std::sort(ranges.begin(), ranges.end());

  size_t used = 0;
  size_t merge_start = ranges[0].first;
  size_t merge_end = ranges[0].second;
  for (size_t i = 1; i < ranges.size(); ++i) {
    if (ranges[i].first > merge_end) {
      used += merge_end - merge_start;
      merge_start = ranges[i].first;
      merge_end = ranges[i].second;
    } else {
      merge_end = std::max(merge_end, ranges[i].second);
    }
  }
  return used + merge_end - merge_start;
}

// Compute coverage per script from Profiler coverage data.
// Coverage ranges can overlap (function ranges contain inner block
// ranges), so we sort/merge used ranges to avoid double-counting.
// Returns map of script key -> ScriptCov with merged used_bytes.
absl::flat_hash_map<std::string, ScriptCov> ComputeCoverageByScript(
    const json& coverage_result) {
  absl::flat_hash_map<std::string, ScriptCov> result;
  if (!coverage_result.is_array()) return result;

  for (const auto& script : coverage_result) {
    std::string script_id = script.value("scriptId", "");
    std::string url = script.value("url", "");
    // Use URL as key when available; scriptId otherwise.
    std::string key = url.empty() ? script_id : url;
    if (key.empty()) continue;

    auto& cov = result[key];
    if (script.contains("functions") && script["functions"].is_array()) {
      for (const auto& func : script["functions"]) {
        if (!func.contains("ranges") || !func["ranges"].is_array()) {
          continue;
        }
        for (const auto& range : func["ranges"]) {
          size_t start = range.value("startOffset", 0u);
          size_t end = range.value("endOffset", 0u);
          int count = range.value("count", 0);
          cov.total_bytes = std::max(cov.total_bytes, end);
          if (count > 0 && end > start) {
            cov.used_ranges.emplace_back(start, end);
          }
        }
      }
    }
  }

  for (auto& [key, cov] : result) {
    cov.used_bytes = MergeRangesAndComputeUsed(std::move(cov.used_ranges));
  }
  return result;
}

// Merge two coverage maps into a cumulative result.
// For each script present in either map, takes the maximum total_bytes
// and unions the used ranges (re-merging overlaps).
absl::flat_hash_map<std::string, ScriptCov> MergeCoverageMaps(
    const absl::flat_hash_map<std::string, ScriptCov>& a,
    const absl::flat_hash_map<std::string, ScriptCov>& b) {
  // We need the raw ranges for proper merging, but
  // ComputeCoverageByScript already cleared them. Instead, treat
  // each snapshot's used_bytes as a single [0, used_bytes) range
  // approximation. For exact merging we'd need to keep the raw
  // ranges, but since FCP and post-FCP ranges are non-overlapping
  // (delta coverage resets counters), simple addition is correct.
  absl::flat_hash_map<std::string, ScriptCov> merged;
  for (const auto& [key, cov] : a) {
    auto& m = merged[key];
    m.total_bytes = cov.total_bytes;
    m.used_bytes = cov.used_bytes;
  }
  for (const auto& [key, cov] : b) {
    auto& m = merged[key];
    m.total_bytes = std::max(m.total_bytes, cov.total_bytes);
    // Delta coverage: FCP snapshot resets counters, so post-FCP
    // coverage is disjoint. Sum is the correct cumulative total.
    m.used_bytes += cov.used_bytes;
    // Clamp to total in case of any edge-case overflow.
    if (m.used_bytes > m.total_bytes) {
      m.used_bytes = m.total_bytes;
    }
  }
  return merged;
}

}  // namespace

struct ScriptCoverageAnalyzer::Session
    : public std::enable_shared_from_this<Session> {
  CdpClient* client;
  Callback callback;
  std::string html_content;
  std::shared_ptr<const PageAnalyzer::ResourceMap> cached_resources;
  uint32_t viewport_width;
  uint32_t viewport_height;
  uint32_t timeout_ms;

  std::string target_id;
  std::string session_id;
  std::string frame_id;
  bool got_fcp = false;
  bool got_network_idle = false;
  bool fcp_coverage_done = false;
  bool completed = false;
  // A refused Fetch.failRequest was reported already (FailPausedRequest).
  bool fetch_error_reported = false;

  // Coverage data captured at FCP and at load.
  json fcp_coverage;
  json load_coverage;

  // Fetch-interception outcomes (copied into the result).
  size_t fetches_served = 0;
  size_t fetches_blocked = 0;

  // URLs of the scripts the browser actually received (served from the
  // resource map). Coverage is joined to a script element only through
  // this set: a coverage entry keyed by URL does not by itself show that
  // the script at that URL was loaded.
  absl::flat_hash_set<std::string> served_urls;

  // Overall session timeout timer.
  uv_timer_t* timeout_timer = nullptr;

  void StartTimeout() {
    if (timeout_ms == 0) return;
    timeout_timer = new uv_timer_t;
    uv_timer_init(client->loop(), timeout_timer);
    auto weak = std::weak_ptr<Session>(shared_from_this());
    timeout_timer->data = new std::weak_ptr<Session>(weak);
    uv_timer_start(
        timeout_timer,
        [](uv_timer_t* t) {
          auto* wp = static_cast<std::weak_ptr<Session>*>(t->data);
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

  bool Send(const CdpCommand& cmd, CdpResponseCallback cb) {
    auto status = client->SendCommand(cmd, std::move(cb));
    if (!status.ok()) {
      FinishError(
          absl::StrCat("SendCommand failed: ", status.status().message()));
      return false;
    }
    return true;
  }

  void Finish(absl::StatusOr<ScriptCoverageResult> result) {
    if (completed) return;
    completed = true;

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
                              [](auto) {});
      (void)status;
    }

    client->SetEventCallback(nullptr);

    if (callback) {
      callback(std::move(result));
    }
  }

  void FinishError(std::string_view msg) { Finish(absl::InternalError(msg)); }

  // Handle Fetch.requestPaused events — serve from cache or fail.
  void HandleFetchRequest(const json& params) {
    std::string request_id = params.value("requestId", "");
    std::string url = params.value("request", json::object()).value("url", "");

    // Defense-in-depth: block 3xx redirect responses.  Fetch.enable
    // defaults to request-stage interception so responseStatusCode is
    // normally absent (defaults to 0), but if Chrome ever delivers a
    // response-stage event the redirect must not be followed — it would
    // generate new requestPaused events for uncached redirect targets.
    int response_status = params.value("responseStatusCode", 0);
    if (response_status >= 300 && response_status < 400) {
      ++fetches_blocked;
      FailPausedRequest(shared_from_this(), request_id, "script coverage");
      return;
    }

    auto it = cached_resources->find(url);
    if (it != cached_resources->end()) {
      ++fetches_served;
      served_urls.insert(url);
      CdpCommand fulfill_cmd;
      fulfill_cmd.method = "Fetch.fulfillRequest";
      std::string encoded = cdp_utils::Base64Encode(it->second);
      std::string content_type(cdp_utils::GuessContentType(url));
      fulfill_cmd.params = {
          {"requestId", request_id},
          {"responseCode", 200},
          {"responseHeaders",
           json::array({{{"name", "Content-Type"}, {"value", content_type}}})},
          {"body", encoded},
      };
      fulfill_cmd.session_id = session_id;
      Send(fulfill_cmd, [](auto) {});
    } else {
      ++fetches_blocked;
      FailPausedRequest(shared_from_this(), request_id, "script coverage");
    }
  }

  // Called when BOTH fcp_coverage_done AND got_network_idle are true.
  void MaybeTakeLoadCoverage() {
    if (!fcp_coverage_done || !got_network_idle || completed) return;

    // Take coverage at load.
    CdpCommand take_cmd;
    take_cmd.method = "Profiler.takePreciseCoverage";
    take_cmd.session_id = session_id;

    auto self = shared_from_this();
    Send(take_cmd, [self](absl::StatusOr<CdpResponse> result) {
      if (!result.ok() || result->is_error()) {
        self->FinishError("Profiler.takePreciseCoverage (load) failed");
        return;
      }
      self->load_coverage = result->result.value("result", json::array());
      self->CollectScriptInfo();
    });
  }

  // Collect script element info from the DOM.
  void CollectScriptInfo() {
    CdpCommand eval_cmd;
    eval_cmd.method = "Runtime.evaluate";
    eval_cmd.params = {
        {"expression", std::string(kCollectScriptsScript)},
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

      json scripts_data;
      try {
        scripts_data = json::parse(value_str);
      } catch (...) {
        self->FinishError("Failed to parse script info");
        return;
      }

      // Compute coverage maps.
      auto fcp_cov = ComputeCoverageByScript(self->fcp_coverage);
      auto load_cov = ComputeCoverageByScript(self->load_coverage);

      // Merge FCP and load coverage for cumulative load coverage.
      // takePreciseCoverage returns delta-only (resets counters),
      // so load_cov only has post-FCP data. The merged map gives
      // the true cumulative coverage at load time.
      auto merged_load_cov = MergeCoverageMaps(fcp_cov, load_cov);

      ScriptCoverageResult cov_result;
      cov_result.fetches_served = self->fetches_served;
      cov_result.fetches_blocked = self->fetches_blocked;

      if (scripts_data.is_array()) {
        for (const auto& s : scripts_data) {
          ScriptInfo info;
          info.url = s.value("url", "");
          info.selector = s.value("selector", "");
          info.is_inline = s.value("is_inline", false);
          info.is_async = s.value("is_async", false);
          info.is_defer = s.value("is_defer", false);
          info.is_module = s.value("is_module", false);
          info.has_document_write = s.value("has_document_write", false);

          // Join coverage only to scripts the browser actually received;
          // a script that was never served has no coverage data. The
          // request URL carries no fragment, so the element's is dropped
          // for this check only.
          const bool received = self->served_urls.contains(
              info.url.substr(0, info.url.find('#')));

          // Look up FCP coverage.  A join with total_bytes > 0 is the only
          // execution evidence — V8 reports every COMPILED script (including
          // ones with zero executed ranges), so absence means the browser
          // never got the script's content.
          auto fcp_it = received ? fcp_cov.find(info.url) : fcp_cov.end();
          if (fcp_it != fcp_cov.end() && fcp_it->second.total_bytes > 0) {
            info.has_coverage_data = true;
            info.coverage_at_fcp =
                static_cast<float>(fcp_it->second.used_bytes) /
                static_cast<float>(fcp_it->second.total_bytes);
            cov_result.total_script_bytes += fcp_it->second.total_bytes;
            cov_result.fcp_used_bytes += fcp_it->second.used_bytes;
          }

          // Look up cumulative load coverage (FCP + post-FCP).
          auto load_it =
              received ? merged_load_cov.find(info.url) : merged_load_cov.end();
          if (load_it != merged_load_cov.end() &&
              load_it->second.total_bytes > 0) {
            info.has_coverage_data = true;
            info.coverage_at_load =
                static_cast<float>(load_it->second.used_bytes) /
                static_cast<float>(load_it->second.total_bytes);
            // Update total if not already counted from FCP.
            if (fcp_it == fcp_cov.end()) {
              cov_result.total_script_bytes += load_it->second.total_bytes;
            }
          }

          // Inline scripts carry no production-actionable verdict
          // (ApplyScriptDeferral requires a src) and V8 collapses all inline
          // coverage onto the document URL, so no per-element join exists;
          // they are reported in `scripts` only.
          if (!info.is_inline && !info.url.empty()) {
            DeferralAdvice advice = ScriptCoverageAnalyzer::Classify(info);
            cov_result.recommendations.emplace_back(info.url, advice);
          }
          cov_result.scripts.push_back(std::move(info));
        }
      }

      if (cov_result.total_script_bytes > 0) {
        cov_result.fcp_coverage_ratio =
            static_cast<float>(cov_result.fcp_used_bytes) /
            static_cast<float>(cov_result.total_script_bytes);
      }

      // Stop profiler coverage before finishing.
      CdpCommand stop_cmd;
      stop_cmd.method = "Profiler.stopPreciseCoverage";
      stop_cmd.session_id = self->session_id;

      self->Send(stop_cmd, [self, result = std::move(cov_result)](
                               auto) mutable { self->Finish(result); });
    });
  }
};

ScriptCoverageAnalyzer::ScriptCoverageAnalyzer(CdpClient* client)
    : client_(client) {}

// static
DeferralAdvice ScriptCoverageAnalyzer::Classify(const ScriptInfo& info) {
  if (info.is_async || info.is_defer || info.is_module) {
    return DeferralAdvice::kAlreadyAsync;
  }
  if (info.has_document_write || info.modifies_dom_before_paint) {
    return DeferralAdvice::kKeepSynchronous;
  }
  // Fail-safe: without profiler evidence the 0.0 coverage default is
  // indistinguishable from "genuinely idle" — a script the analysis browser
  // could not observe (blocked fetch, unresolved src, profiler gap) must
  // never classify as deferrable.
  if (!info.has_coverage_data) {
    return DeferralAdvice::kNoCoverageData;
  }
  if (info.coverage_at_fcp == 0.0f) {
    return DeferralAdvice::kSafeToDefer;
  }
  if (info.coverage_at_fcp < 0.10f) {
    return DeferralAdvice::kCandidateForAsync;
  }
  return DeferralAdvice::kKeepSynchronous;
}

void ScriptCoverageAnalyzer::Analyze(
    std::string_view html_content,
    std::shared_ptr<const PageAnalyzer::ResourceMap> cached_resources,
    uint32_t viewport_width, uint32_t viewport_height, Callback callback,
    uint32_t timeout_ms) {
  auto session = std::make_shared<Session>();
  session->client = client_;
  session->callback = std::move(callback);
  session->html_content = std::string(html_content);
  session->cached_resources = std::move(cached_resources);
  session->viewport_width = viewport_width;
  session->viewport_height = viewport_height;
  session->timeout_ms = timeout_ms;

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

          // Step 3: Network offline (SSRF defense).
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
            // Step 4: Fetch interception.
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
                // Step 6: Profiler.enable.
                CdpCommand prof_cmd;
                prof_cmd.method = "Profiler.enable";
                prof_cmd.session_id = s->session_id;

                s->Send(prof_cmd, [s](auto result) {
                  if (!result.ok() || result->is_error()) {
                    s->FinishError("Profiler.enable failed");
                    return;
                  }
                  // Step 7: Start precise coverage.
                  CdpCommand cov_cmd;
                  cov_cmd.method =
                      "Profiler."
                      "startPreciseCoverage";
                  cov_cmd.params = {
                      {"callCount", false},
                      {"detailed", true},
                  };
                  cov_cmd.session_id = s->session_id;

                  s->Send(cov_cmd, [s](auto result) {
                    if (!result.ok() || result->is_error()) {
                      s->FinishError("startPreciseCoverage failed");
                      return;
                    }
                    // Step 8: Inject capture script.
                    CdpCommand script_cmd;
                    script_cmd.method =
                        "Page."
                        "addScriptToEvaluateOnNewDocument";
                    script_cmd.params = {
                        {"source", std::string(kCaptureScript)}};
                    script_cmd.session_id = s->session_id;

                    s->Send(script_cmd, [s](auto result) {
                      if (!result.ok() || result->is_error()) {
                        s->FinishError("addScript setup failed");
                        return;
                      }
                      // Step 9: Page.enable + lifecycle.
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
                              s->FinishError(
                                  "lifecycle enable "
                                  "failed");
                              return;
                            }
                            // Step 10: Get frame + set
                            // content.
                            CdpCommand tree_cmd;
                            tree_cmd.method = "Page.getFrameTree";
                            tree_cmd.session_id = s->session_id;

                            s->Send(tree_cmd,
                                    [s](absl::StatusOr<CdpResponse> result) {
                                      if (!result.ok() || result->is_error()) {
                                        s->FinishError(
                                            "getFrameTree "
                                            "failed");
                                        return;
                                      }
                                      s->frame_id =
                                          result->result
                                              .value("frameTree",
                                                     json::object())
                                              .value("frame", json::object())
                                              .value("id", "");

                                      CdpCommand content_cmd;
                                      content_cmd.method =
                                          "Page."
                                          "setDocumentContent";
                                      content_cmd.params = {
                                          {"frameId", s->frame_id},
                                          {"html", s->html_content},
                                      };
                                      content_cmd.session_id = s->session_id;
                                      content_cmd.timeout_ms = 15000;

                                      s->Send(content_cmd, [s](auto r) {
                                        s->html_content.clear();
                                        s->html_content.shrink_to_fit();
                                        if (!r.ok() || r->is_error()) {
                                          s->FinishError(
                                              "setDocument"
                                              "Content "
                                              "failed");
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

    if (event.method == "Fetch.requestPaused") {
      session->HandleFetchRequest(event.params);
      return;
    }

    if (event.method == "Page.lifecycleEvent") {
      std::string name = event.params.value("name", "");

      if (name == "firstContentfulPaint" && !session->got_fcp) {
        session->got_fcp = true;

        // Take coverage at FCP.
        CdpCommand take_cmd;
        take_cmd.method = "Profiler.takePreciseCoverage";
        take_cmd.session_id = session->session_id;

        session->Send(take_cmd, [session](absl::StatusOr<CdpResponse> result) {
          if (!result.ok() || result->is_error()) {
            session->FinishError("Profiler.takePreciseCoverage (fcp) failed");
            return;
          }
          session->fcp_coverage = result->result.value("result", json::array());
          session->fcp_coverage_done = true;
          session->MaybeTakeLoadCoverage();
        });
      } else if (name == "networkIdle" && !session->got_network_idle) {
        session->got_network_idle = true;
        session->MaybeTakeLoadCoverage();
      }
    }
  });
}

}  // namespace pagespeed
