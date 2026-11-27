// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

// 4. Fetch.enable({patterns: [{urlPattern: "*"}]})
//    + Fetch.requestPaused handler to serve from cache
// 5. Emulation.setDeviceMetricsOverride + setTouchEmulationEnabled
//    [+ setUserAgentOverride] (EmulateDevice, src/browser/device_emulation.h)
// 6. Page.addScriptToEvaluateOnNewDocument() for LCP + CLS observers
// 7. Page.enable, Page.navigate(about:blank) for a fresh window
//    (LoadFreshBlankDocument, which also makes the observers of step 6
//    run in the window the document is written into),
//    Page.setLifecycleEventsEnabled
// 8. Page.setDocumentContent({frameId, html})
// 9. Wait for Page.lifecycleEvent("networkIdle") + 2s stability
// 10. Runtime.evaluate to collect:
//     - window.__PS_LCP, window.__PS_CLS
//     - img getBoundingClientRect for each image
// 11. Target.closeTarget({targetId})
//
// An agent_optimize render (network-enabled) runs in a browser context of its
// own: Target.createBrowserContext before step 1, the target created in it,
// and Target.disposeBrowserContext after step 11, so no storage, cache or
// service worker state carries from one render to the next.  Service worker
// registration is hidden from its pages as well.

#include "src/browser/page_analysis.h"

#include <algorithm>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "absl/status/statusor.h"
#include "absl/strings/str_cat.h"
#include "lib/html/markdown_extractor_filter.h"
#include "nlohmann/json.hpp"
#include "src/browser/agent_fetcher.h"
#include "src/browser/cdp_client.h"
#include "src/browser/cdp_types.h"
#include "src/browser/cdp_utils.h"
#include "src/browser/device_emulation.h"
#include "src/browser/render_support.h"
#include "uv.h"

namespace pagespeed {

using json = nlohmann::json;

// JavaScript to inject via Page.addScriptToEvaluateOnNewDocument.
// This creates PerformanceObservers for LCP and CLS that survive
// navigation (unlike Runtime.evaluate which is destroyed).
static constexpr std::string_view kObserverScript = R"JS(
(function() {
  // Capture built-in functions before page scripts can override them.
  // This prevents attacker-controlled pages from tampering with our
  // data collection (e.g., overriding JSON.stringify to inject data).
  window.__PS_stringify = JSON.stringify;
  window.__PS_LCP = {selector: '', url: '', tag: '', size: 0, time: 0};
  window.__PS_CLS = {total: 0, sources: []};

  // LCP Observer
  try {
    new PerformanceObserver(function(list) {
      var entries = list.getEntries();
      for (var i = 0; i < entries.length; i++) {
        var e = entries[i];
        var info = {
          selector: '',
          url: e.url || '',
          tag: '',
          size: e.size || 0,
          time: e.startTime || 0
        };
        if (e.element) {
          info.tag = e.element.tagName || '';
          info.selector = e.element.tagName;
          if (e.element.id) info.selector += '#' + e.element.id;
          if (e.element.className && typeof e.element.className === 'string') {
            info.selector += '.' + e.element.className.trim().split(/\s+/).join('.');
          }
        }
        window.__PS_LCP = info;
      }
    }).observe({type: 'largest-contentful-paint', buffered: true});
  } catch(e) {}

  // CLS Observer
  try {
    new PerformanceObserver(function(list) {
      var entries = list.getEntries();
      for (var i = 0; i < entries.length; i++) {
        var e = entries[i];
        if (!e.hadRecentInput) {
          window.__PS_CLS.total += e.value;
          if (e.sources) {
            for (var j = 0; j < e.sources.length; j++) {
              var src = e.sources[j];
              var sel = '';
              if (src.node) {
                sel = src.node.tagName || '';
                if (src.node.id) sel += '#' + src.node.id;
              }
              window.__PS_CLS.sources.push({
                selector: sel,
                value: e.value
              });
            }
          }
        }
      }
    }).observe({type: 'layout-shift', buffered: true});
  } catch(e) {}
})();
)JS";

// Prepended to the observer script in an agent_optimize render: removes the
// service worker API from every document of the render (frames included,
// as Page.addScriptToEvaluateOnNewDocument runs in each), so a page cannot
// install a worker that answers or fetches outside the Fetch interception.
// A page sees what it sees in a browser without service workers.  Defence in
// depth: the browser's switches already keep the render's own network use
// to the Fetch interception (tools/packaging/smoke-chrome-egress.sh).
static constexpr std::string_view kNoServiceWorkerScript = R"JS(
(() => {
  try { delete Navigator.prototype.serviceWorker; } catch (e) {}
  try {
    if ('serviceWorker' in navigator) {
      Object.defineProperty(Navigator.prototype, 'serviceWorker', {
        get: () => undefined, configurable: false });
    }
  } catch (e) {}
})();
)JS";

// JavaScript to collect image information and above-the-fold element
// descriptors from the rendered page.
static constexpr std::string_view kCollectImagesScript = R"JS(
(function() {
  var result = {
    lcp: window.__PS_LCP || {},
    cls: window.__PS_CLS || {total: 0, sources: []},
    images: [],
    elements: [],
    elements_truncated: false,
    fcp: 0
  };

  // Get FCP timing
  try {
    var paintEntries = performance.getEntriesByType('paint');
    for (var i = 0; i < paintEntries.length; i++) {
      if (paintEntries[i].name === 'first-contentful-paint') {
        result.fcp = paintEntries[i].startTime;
      }
    }
  } catch(e) {}

  // Collect image info
  var imgs = document.querySelectorAll('img');
  var viewportHeight = window.innerHeight;
  for (var i = 0; i < imgs.length; i++) {
    var img = imgs[i];
    var rect = img.getBoundingClientRect();
    var sel = 'img';
    if (img.id) sel += '#' + img.id;
    if (img.className && typeof img.className === 'string') {
      sel += '.' + img.className.trim().split(/\s+/).join('.');
    }
    result.images.push({
      selector: sel,
      src: img.src || '',
      above_fold: rect.top < viewportHeight,
      rendered_width: Math.round(rect.width),
      rendered_height: Math.round(rect.height),
      natural_width: img.naturalWidth,
      natural_height: img.naturalHeight
    });
  }

  // Describe every element the browser laid out, flagging the ones that are
  // actually above the fold.  This is the measured replacement for guessing the
  // fold from "the first N elements of the document": a page whose <head> is
  // large exhausts such an estimate long before its first visible body element,
  // so the classes that lay out the header are never seen.
  //
  // Same fold predicate as the image walk above (rect.top < viewportHeight), so
  // the two channels cannot disagree about what "above the fold" means.
  //
  // NOTE on the predicate: an element with no layout box (anything in <head>,
  // anything in a `display:none` subtree) gets an all-zero rect, so `top < h`
  // is true for it and it is reported above the fold.  That is tolerable ONLY
  // because a descriptor is consumed by matching it against an element — a
  // <head> element carries no class a body rule matches, and a hidden element's
  // classes were going to be needed at first paint or not regardless.  It is
  // NOT safe to count these; an earlier revision reported a COUNT here and used
  // it as an element-index budget, which promoted whole documents to critical.
  var all = document.querySelectorAll('*');
  var cap = 2000;
  for (var k = 0; k < all.length; k++) {
    var el = all[k];
    var box = el.getBoundingClientRect();
    var visible = box.top < viewportHeight;
    if (result.elements.length >= cap) continue;
    // getAttribute, not .className/.id: className is an SVGAnimatedString on
    // SVG elements, and a <form> element's named-control access can shadow .id.
    var classAttr = el.getAttribute('class');
    var classes = [];
    if (typeof classAttr === 'string') {
      var parts = classAttr.trim().split(/\s+/);
      for (var m = 0; m < parts.length; m++) {
        if (parts[m]) classes.push(parts[m]);
      }
    }
    var idAttr = el.getAttribute('id');
    result.elements.push({
      tag: (el.tagName || '').toLowerCase(),
      id: typeof idAttr === 'string' ? idAttr : '',
      classes: classes,
      index: k,
      above_fold: visible
    });
  }
  result.elements_truncated = all.length > cap;

  // Use captured stringify to prevent page script tampering.
  var stringify = window.__PS_stringify || JSON.stringify;
  return stringify(result);
})();
)JS";

namespace {

// Type-safe field reads for the in-page collector's element descriptors.
//
// nlohmann's `value(key, default)` THROWS json::type_error when the stored
// value is of a different type than the default — it is a convenience for
// trusted JSON, and this JSON is not trusted: the collector runs inside the
// page, and a page that replaced window.__PS_stringify chooses every byte.
//
// Severity, stated precisely: in production this callback is reached from a
// libuv C frame, and an exception unwinding through C has no handler to reach,
// so the process aborts (std::terminate) — a hostile or merely broken page
// takes the worker down. Under test the loop is driven from C++ and the same
// throw merely leaves the analysis never completing, which is why the harness
// observes a hang rather than a crash; do not read the test symptom as the
// production one.
std::string JsonStringOr(const json& obj, const char* key) {
  auto it = obj.find(key);
  if (it == obj.end() || !it->is_string()) return {};
  return it->get<std::string>();
}

uint32_t JsonUintOr(const json& obj, const char* key, uint32_t fallback) {
  auto it = obj.find(key);
  if (it == obj.end() || !it->is_number_unsigned()) return fallback;
  return it->get<uint32_t>();
}

bool JsonBoolOr(const json& obj, const char* key, bool fallback) {
  auto it = obj.find(key);
  if (it == obj.end() || !it->is_boolean()) return fallback;
  return it->get<bool>();
}

}  // namespace

struct PageAnalyzer::Session : public std::enable_shared_from_this<Session> {
  CdpClient* client;
  Callback callback;
  std::string html_content;
  std::shared_ptr<const ResourceMap> cached_resources;
  uint32_t viewport_width;
  uint32_t viewport_height;
  uint32_t timeout_ms;

  // agent_optimize: when agent->policy is set, a paused request not
  // served from cache is fetched out-of-Chrome via the IP-pinned fetcher instead
  // of being blocked. nullptr => legacy offline/cache-only behavior.
  std::shared_ptr<const AgentRenderOptions> agent;
  int agent_fetches_in_flight = 0;

  // The render's own browser context (agent_optimize renders only; empty
  // otherwise).  Disposed in Finish().
  std::string browser_context_id;
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

  // agent_optimize settle timer (between networkIdle and the rendered-DOM read).
  // Owned like timeout_timer: self-cleaned on fire, or cleaned by Finish().
  uv_timer_t* settle_timer = nullptr;

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

  void Finish(absl::StatusOr<PageAnalysisResult> result) {
    if (std::exchange(completed, true)) return;

    // Keep ourselves alive across our own teardown: SetEventCallback(nullptr)
    // below drops the event callback's strong ref, which may be the LAST ref to
    // this Session (e.g. Finish reached re-entrantly from inside the event
    // callback via a Send failure). Without this, the rest of Finish() would run
    // on a freed `this`.
    auto keep_alive = shared_from_this();

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

    // Cancel the agent_optimize settle timer if it is still pending (e.g. Finish
    // reached via timeout/error between networkIdle and the settle firing).
    if (settle_timer != nullptr) {
      if (settle_timer->data != nullptr) {
        delete static_cast<std::weak_ptr<Session>*>(settle_timer->data);
        settle_timer->data = nullptr;
      }
      uv_timer_stop(settle_timer);
      uv_close(reinterpret_cast<uv_handle_t*>(settle_timer),
               [](uv_handle_t* h) { delete (uv_timer_t*)h; });
      settle_timer = nullptr;
    }

    // Close the target (best effort — session is already done,
    // so failure to send is not actionable).
    if (!target_id.empty() && client->connected()) {
      auto status =
          client->SendCommand(CdpCommand{.method = "Target.closeTarget",
                                         .params = {{"targetId", target_id}},
                                         .session_id = {},
                                         .timeout_ms = 5000},
                              [](auto) {});
      (void)status;  // Best effort cleanup.
    }
    // Dispose of the render's browser context (closes anything left in it).
    if (!browser_context_id.empty() && client->connected()) {
      DisposeBrowserContext(client, browser_context_id);
    }

    // Release the event callback's strong reference to this session.
    client->SetEventCallback(nullptr);

    if (callback) {
      callback(std::move(result));
    }
  }

  void FinishError(std::string_view msg) { Finish(absl::InternalError(msg)); }

  // Whether this render gets a browser context of its own: an agent_optimize
  // render, the one that egresses.
  bool IsolatedRender() const { return agent != nullptr && agent->policy; }

  static void DisposeBrowserContext(CdpClient* c, const std::string& id) {
    auto status =
        c->SendCommand(CdpCommand{.method = "Target.disposeBrowserContext",
                                  .params = {{"browserContextId", id}},
                                  .session_id = {},
                                  .timeout_ms = 5000},
                       [](auto) {});
    (void)status;  // Best effort cleanup.
  }

  // Bound on concurrent out-of-Chrome agent_optimize fetches per render. A hostile
  // page must not be able to spawn unbounded curl processes / starve the libuv work
  // pool. (The manager serializes to one render at a time, so this also caps global
  // concurrency.) Excess fetchable requests fail closed.
  static constexpr int kMaxConcurrentAgentFetches = 32;

  // Heap context for one out-of-Chrome fetch on the libuv work pool. Owns its own
  // uv_work_t (first member). Holds a weak_ptr<Session> (the Session may finish or
  // be destroyed while curl runs) + a shared_ptr to the render options (keeps the
  // policy + spawn/resolve deps alive across the work).
  struct AgentFetchWork {
    uv_work_t req{};
    std::weak_ptr<Session> session;
    std::string request_id;
    std::string url;
    ResourceClass rc = ResourceClass::kOther;
    std::shared_ptr<const AgentRenderOptions> agent;
    AgentFetchOutcome outcome;
  };

  // Work callback — runs OFF the loop thread (libuv threadpool). Performs the
  // blocking IP-pinned fetch. Must never let an exception cross the libuv (C)
  // boundary, and touches ONLY ctx-owned fields (no Session/CdpClient access).
  static void AgentFetchWorkCb(uv_work_t* req) {
    auto* ctx = static_cast<AgentFetchWork*>(req->data);
    try {
      ctx->outcome = FetchSubresource(ctx->url, ctx->rc, *ctx->agent->policy,
                                      ctx->agent->deps);
    } catch (...) {
      ctx->outcome = AgentFetchOutcome{};  // default => deny BlockedByClient
    }
  }

  // After-work callback — runs ON the loop thread. The page may have finished or
  // been torn down during the curl: lock the weak Session and re-check `completed`
  // before touching CDP. (Tearing down the CdpClient drops the event-callback's
  // strong ref to the Session, so a dead client implies a null lock here — UAF-safe.)
  static void AgentFetchAfterCb(uv_work_t* req, int status) {
    std::unique_ptr<AgentFetchWork> ctx(
        static_cast<AgentFetchWork*>(req->data));
    if (status != 0) return;  // cancelled at shutdown — drop (ctx auto-freed)
    auto s = ctx->session.lock();
    if (!s || s->completed) return;  // Session gone or already finished
    if (s->agent_fetches_in_flight > 0) --s->agent_fetches_in_flight;
    if (ctx->outcome.fulfilled) {
      s->SendAgentFulfill(ctx->request_id, ctx->outcome);
    } else {
      s->FailRequest(ctx->request_id);
    }
  }

  // Fetch.failRequest(BlockedByClient), reply checked (FailPausedRequest).
  void FailRequest(const std::string& request_id) {
    FailPausedRequest(shared_from_this(), request_id, "page analysis");
  }

  // Fetch.fulfillRequest from an in-memory cached resource (legacy path).
  void FulfillFromCache(const std::string& request_id, const std::string& url,
                        const std::string& body) {
    CdpCommand fulfill_cmd;
    fulfill_cmd.method = "Fetch.fulfillRequest";
    std::string encoded = cdp_utils::Base64Encode(body);
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
  }

  // Fetch.fulfillRequest from a real out-of-Chrome fetch (agent_optimize path).
  // outcome.headers are already hygiene'd (Set-Cookie / hop-by-hop / Content-Length
  // stripped) by the fetcher; Chrome re-derives framing from the body we hand it.
  void SendAgentFulfill(const std::string& request_id,
                        const AgentFetchOutcome& outcome) {
    CdpCommand fulfill_cmd;
    fulfill_cmd.method = "Fetch.fulfillRequest";
    json headers = json::array();
    for (const HttpHeader& h : outcome.headers) {
      headers.push_back({{"name", h.name}, {"value", h.value}});
    }
    fulfill_cmd.params = {
        {"requestId", request_id},
        {"responseCode", outcome.status},
        {"responseHeaders", headers},
        {"body", cdp_utils::Base64Encode(outcome.body)},
    };
    fulfill_cmd.session_id = session_id;
    Send(fulfill_cmd, [](auto) {});
  }

  // Schedule the out-of-Chrome fetch for a paused request on the libuv work pool.
  void ScheduleAgentFetch(const std::string& request_id, const std::string& url,
                          const json& params) {
    if (agent_fetches_in_flight >= kMaxConcurrentAgentFetches) {
      FailRequest(request_id);  // fail closed over the concurrency cap
      return;
    }
    auto* ctx = new AgentFetchWork;
    ctx->req.data = ctx;
    ctx->session = weak_from_this();
    ctx->request_id = request_id;
    ctx->url = url;
    ctx->rc = ClassifyResource(params.value("resourceType", ""));
    ctx->agent = agent;  // keeps policy + deps alive across the async work
    int rc =
        uv_queue_work(client->loop(), &ctx->req, &Session::AgentFetchWorkCb,
                      &Session::AgentFetchAfterCb);
    if (rc != 0) {
      delete ctx;
      FailRequest(request_id);
      return;
    }
    ++agent_fetches_in_flight;
  }

  // Handle Fetch.requestPaused events — serve from cache, agent-fetch, or block.
  void HandleFetchRequest(const json& params) {
    std::string request_id = params.value("requestId", "");
    std::string url = params.value("request", json::object()).value("url", "");

    // Defense-in-depth: block 3xx redirect responses. Fetch.enable defaults to
    // request-stage interception so responseStatusCode is normally absent
    // (defaults to 0), but if Chrome ever delivers a response-stage event the
    // redirect must not be followed — it would generate new requestPaused events
    // for redirect targets. (The agent fetcher follows redirects itself, manually
    // re-adjudicated per hop, so Chrome must never follow them.)
    int response_status = params.value("responseStatusCode", 0);
    if (response_status >= 300 && response_status < 400) {
      FailRequest(request_id);
      return;
    }

    // Cache hit (legacy path) — serve the worker-supplied bytes.
    auto it = cached_resources->find(url);
    if (it != cached_resources->end()) {
      FulfillFromCache(request_id, url, it->second);
      return;
    }

    // agent_optimize: out-of-Chrome IP-pinned fetch (async). Only when a policy is
    // active; the policy + SSRF guard decide allow/deny inside the fetcher.
    if (agent && agent->policy) {
      ScheduleAgentFetch(request_id, url, params);
      return;
    }

    // Legacy default: block everything not in cache (SSRF defense).
    FailRequest(request_id);
  }

  // agent_optimize: after networkIdle, wait a settle delay for
  // hydration to flush, then read the rendered DOM. The "2s stability" the legacy
  // path only comments about is actually implemented here (one-shot uv_timer,
  // weak_ptr-guarded like StartTimeout). settle_ms==0 reads immediately (tests).
  void ScheduleRenderedHtmlRead() {
    int settle = (agent && agent->settle_ms > 0) ? agent->settle_ms : 0;
    if (settle <= 0) {
      CollectRenderedHtml();
      return;
    }
    settle_timer = new uv_timer_t;
    uv_timer_init(client->loop(), settle_timer);
    settle_timer->data = new std::weak_ptr<Session>(shared_from_this());
    uv_timer_start(
        settle_timer,
        [](uv_timer_t* t) {
          auto* wp = static_cast<std::weak_ptr<Session>*>(t->data);
          auto s = wp->lock();
          delete wp;
          t->data = nullptr;
          if (s)
            s->settle_timer = nullptr;  // detach: Finish must not re-close it
          uv_timer_stop(t);
          uv_close(reinterpret_cast<uv_handle_t*>(t), [](uv_handle_t* h) {
            delete reinterpret_cast<uv_timer_t*>(h);
          });
          if (s && !s->completed) s->CollectRenderedHtml();
        },
        settle, 0);
  }

  // Read the hydrated rendered DOM (document.documentElement.outerHTML, JS-capped
  // at 4 MiB) and SAX-extract markdown. Terminal: feeds Finish().
  void CollectRenderedHtml() {
    CdpCommand eval_cmd;
    eval_cmd.method = "Runtime.evaluate";
    eval_cmd.params = {
        {"expression",
         "(function(){var h=document.documentElement.outerHTML;var CAP=4194304;"
         "return h.length>CAP?h.slice(0,CAP):h;})()"},
        {"returnByValue", true},
    };
    eval_cmd.session_id = session_id;
    eval_cmd.timeout_ms = 10000;

    auto self = shared_from_this();
    Send(eval_cmd, [self](absl::StatusOr<CdpResponse> result) {
      if (!result.ok() || result->is_error()) {
        self->FinishError("rendered-DOM evaluate failed");
        return;
      }
      std::string html =
          result->result.value("result", json::object()).value("value", "");
      PageAnalysisResult analysis;
      analysis.agent_markdown = net_instaweb::ExtractAgentMarkdown(html, "");
      analysis.rendered_html = std::move(html);
      self->Finish(analysis);
    });
  }

  // Collect results from the page after networkIdle.
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
    if (agent && agent->policy) {
      // agent_optimize: settle, then read the rendered DOM -> markdown.
      ScheduleRenderedHtmlRead();
    } else {
      // Legacy perf path: collect LCP/CLS/image metrics now.
      CollectResults();
    }
  }

  void CollectResults() {
    CdpCommand eval_cmd;
    eval_cmd.method = "Runtime.evaluate";
    eval_cmd.params = {
        {"expression", std::string(kCollectImagesScript)},
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

      // Parse the JSON result.
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

      PageAnalysisResult analysis;

      // Everything below reads page-controlled JSON. The descriptor loop is
      // type-safe field by field (see JsonStringOr and friends) so one bad
      // element degrades to a default instead of discarding the render; the
      // older LCP/CLS/image reads still use nlohmann's throwing `value()`, so
      // this catch is what stops a type_error from unwinding out of the CDP
      // response callback through libuv's C frames, where it would abort the
      // process rather than fail one analysis.
      try {
        // LCP info.
        if (data.contains("lcp")) {
          auto& lcp = data["lcp"];
          analysis.lcp.selector = lcp.value("selector", "");
          analysis.lcp.url = lcp.value("url", "");
          analysis.lcp.element_tag = lcp.value("tag", "");
          analysis.lcp.size = lcp.value("size", 0u);
          analysis.lcp_ms = static_cast<float>(lcp.value("time", 0.0));
        }

        // CLS info.
        if (data.contains("cls")) {
          auto& cls = data["cls"];
          analysis.cls_total = static_cast<float>(cls.value("total", 0.0));
          if (cls.contains("sources")) {
            for (const auto& src : cls["sources"]) {
              ClsSource cs;
              cs.selector = src.value("selector", "");
              cs.shift_value = static_cast<float>(src.value("value", 0.0));
              analysis.cls_sources.push_back(std::move(cs));
            }
          }
        }

        // FCP timing.
        analysis.fcp_ms = static_cast<float>(data.value("fcp", 0.0));

        // Images.
        if (data.contains("images")) {
          for (const auto& img : data["images"]) {
            ImageInfo info;
            info.selector = img.value("selector", "");
            info.src = img.value("src", "");
            info.above_fold = img.value("above_fold", false);
            info.rendered_width = img.value("rendered_width", 0u);
            info.rendered_height = img.value("rendered_height", 0u);
            info.natural_width = img.value("natural_width", 0u);
            info.natural_height = img.value("natural_height", 0u);
            analysis.images.push_back(std::move(info));
          }
        }

        // Measured above-the-fold elements.  The cap is re-enforced HERE rather
        // than trusted from the page: the collector runs inside a document that
        // may have tampered with anything, and an unbounded descriptor list would
        // be copied into a cached profile.  Truncation is recorded either way —
        // an over-long list that arrives claiming not to be truncated is still
        // truncated once we clip it.
        if (data.contains("elements") && data["elements"].is_array()) {
          const auto& els = data["elements"];
          for (const auto& el : els) {
            // The page controls these bytes end to end: the collector runs inside
            // the document, and a page that replaced __PS_stringify chooses the
            // whole payload. The try/catch upstream only covers json::parse, so a
            // well-formed array of the WRONG SHAPE reaches here — el.value() on a
            // non-object throws nlohmann::type_error, which nothing below catches.
            if (!el.is_object()) continue;
            if (analysis.elements.size() >=
                PageAnalysisResult::kMaxElementDescriptors) {
              analysis.elements_truncated = true;
              break;
            }
            ElementDescriptor d;
            d.tag = JsonStringOr(el, "tag");
            d.id = JsonStringOr(el, "id");
            auto classes = el.find("classes");
            if (classes != el.end() && classes->is_array()) {
              for (const auto& c : *classes) {
                if (c.is_string()) d.classes.push_back(c.get<std::string>());
              }
            }
            d.index = JsonUintOr(el, "index", 0);
            d.above_fold = JsonBoolOr(el, "above_fold", false);
            analysis.elements.push_back(std::move(d));
          }
        }
        if (JsonBoolOr(data, "elements_truncated", false)) {
          analysis.elements_truncated = true;
        }
      } catch (const json::exception& e) {
        self->FinishError(
            absl::StrCat("Unexpected eval result shape: ", e.what()));
        return;
      }

      self->Finish(analysis);
    });
  }
};

PageAnalyzer::PageAnalyzer(CdpClient* client)
    : client_(client), settle_after_load_ms_(kSettleAfterLoadMs) {}

void PageAnalyzer::Analyze(std::string_view html_content,
                           std::shared_ptr<const ResourceMap> cached_resources,
                           uint32_t viewport_width, uint32_t viewport_height,
                           Callback callback, uint32_t timeout_ms,
                           std::shared_ptr<const AgentRenderOptions> agent) {
  auto session = std::make_shared<Session>();
  session->client = client_;
  session->callback = std::move(callback);
  session->html_content = std::string(html_content);
  // Share the resource map — no deep copy.
  session->cached_resources = std::move(cached_resources);
  session->viewport_width = viewport_width;
  session->viewport_height = viewport_height;
  session->timeout_ms = timeout_ms;
  session->settle_after_load_ms = settle_after_load_ms_;
  session->agent = std::move(agent);

  // Start overall session timeout.
  session->StartTimeout();

  // Set event callback to handle lifecycle + Fetch events.
  client_->SetEventCallback([session](const CdpEvent& event) {
    if (session->completed) return;
    if (event.session_id != session->session_id) return;

    if (event.method == "Fetch.requestPaused") {
      session->HandleFetchRequest(event.params);
    } else if (event.method == "Page.lifecycleEvent") {
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

  if (!session->IsolatedRender()) {
    StartTarget(session);
    return;
  }
  // Step 0 (agent_optimize): a browser context of the render's own (MUST
  // succeed: the render never runs in the shared default context).
  CdpCommand context_cmd;
  context_cmd.method = "Target.createBrowserContext";
  context_cmd.params = {{"disposeOnDetach", true}};
  context_cmd.timeout_ms = 10000;
  session->Send(context_cmd, [session](absl::StatusOr<CdpResponse> result) {
    std::string id;
    if (result.ok() && !result->is_error()) {
      id = result->result.value("browserContextId", "");
    }
    if (session->completed) {
      // Ended (timeout) while the context was being made: drop it.
      if (!id.empty() && session->client->connected()) {
        Session::DisposeBrowserContext(session->client, id);
      }
      return;
    }
    if (id.empty()) {
      session->FinishError("createBrowserContext failed");
      return;
    }
    session->browser_context_id = std::move(id);
    StartTarget(session);
  });
}

void PageAnalyzer::StartTarget(const std::shared_ptr<Session>& session) {
  // Step 1: Create target.
  CdpCommand create_cmd;
  create_cmd.method = "Target.createTarget";
  create_cmd.params = {{"url", "about:blank"}};
  if (!session->browser_context_id.empty()) {
    create_cmd.params["browserContextId"] = session->browser_context_id;
  }
  create_cmd.timeout_ms = 10000;

  session->Send(create_cmd, [session](absl::StatusOr<CdpResponse> result) {
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

    session->Send(attach_cmd, [session](absl::StatusOr<CdpResponse> result) {
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
        fetch_cmd.params = {{"patterns", json::array({{{"urlPattern", "*"}}})}};
        fetch_cmd.session_id = s->session_id;

        s->Send(fetch_cmd, [s](auto result) {
          if (!result.ok() || result->is_error()) {
            s->FinishError("Fetch.enable setup failed");
            return;
          }
          // Step 5: Emulate the device (metrics, touch, UA).
          EmulateDevice(s, s->viewport_width, s->viewport_height, [s]() {
            // Step 6: Inject observer script.
            CdpCommand script_cmd;
            script_cmd.method =
                "Page."
                "addScriptToEvaluateOnNewDocument";
            script_cmd.params = {
                {"source",
                 s->IsolatedRender()
                     ? absl::StrCat(kNoServiceWorkerScript, kObserverScript)
                     : std::string(kObserverScript)}};
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

                    s->Send(tree_cmd, [s](absl::StatusOr<CdpResponse> result) {
                      if (!result.ok() || result->is_error()) {
                        s->FinishError("getFrameTree failed");
                        return;
                      }
                      s->frame_id =
                          result->result.value("frameTree", json::object())
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
                        // Release HTML memory — no
                        // longer needed after loading.
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
}

}  // namespace pagespeed
