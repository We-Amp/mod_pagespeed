// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

// PageSpeed 2.0 - device emulation for the browser-analysis renders
//
// Which device every analysis render emulates at a given viewport width: the
// CSS coverage render, page analysis, the validation render, script coverage,
// the font glyph scan and the capture endpoint all set up their emulation
// through EmulateDevice, so they cannot disagree.
//
// ONE SOURCE, TWO READERS. The values live in device_emulation.json next to
// this header. The async-css probe (tools/async-css-probe/probe_rendered.mjs)
// reads that file to emulate each viewport, and device_emulation_test fails
// when the constants below differ from it, so the probe renders what the
// product renders.
//
// THE RULE. A viewport at most kMobileEmulationMaxWidth
// CSS px wide renders as a mobile device: the analysis viewports at 375 px
// (mobile) and 768 px (tablet). Real Android tablets and iPads in portrait lay
// pages out as mobile devices do: the meta viewport applies, the page itself
// stays at device width, and content that overflows it widens the layout
// viewport (which overflow and `position: fixed` elements anchor to) instead
// of adding a scrollbar outside the measured fold. Before the tablet-as-mobile
// change the tablet
// viewport rendered as a desktop window (`mobile: width < 768`). Wider
// viewports render as a desktop window.
//
// A mobile device is also a TOUCH device (kMobileEmulatesTouch):
// `Emulation.setDeviceMetricsOverride {mobile: true}` alone leaves
// `(hover: hover)` and `(pointer: fine)` matching, so until touch emulation
// every
// phone and tablet render evaluated hover/pointer media queries the way a
// desktop does, the opposite of what a phone visitor's browser does. Touch
// emulation (`Emulation.setTouchEmulationEnabled {enabled: true,
// maxTouchPoints: kMaxTouchPoints}`) flips them in every render:
// `(hover: none)`, `(pointer: coarse)`, `(any-hover: none)` and
// `(any-pointer: coarse)` match, their `hover`/`fine` counterparts do not,
// and `navigator.maxTouchPoints` is kMaxTouchPoints. `'ontouchstart' in
// window` is set only for a Window created after the call: the capture
// endpoint navigates to the page, and since User-Agent emulation the five
// renders that
// write their document with Page.setDocumentContent write it into a fresh
// about:blank window (LoadFreshBlankDocument below; until then they wrote it
// into the window the target started with, where ontouchstart stayed false).
// The desktop viewport keeps `hover: hover` / `pointer: fine` and no touch
// points, so every render is sent the call (enabled or not) and no target
// inherits a state. The critical-CSS extractor's static media evaluation
// reads the same rule for the phone and tablet classes (RetentionPointer in
// src/worker/critical_css_extractor.cc), so the block it derives and the
// render that validates it agree.
//
// A phone or a tablet also sends a phone's or a tablet's USER AGENT:
// `Emulation.setUserAgentOverride` with the UA string, the
// navigator.platform and the client hints (`userAgentMetadata`) of Chrome on
// Android, from device_emulation.json. A viewport at most kPhoneMaxWidth CSS
// px wide (the phone class, CapabilityMask::ViewportWidthRange(kMobile))
// sends Chrome on an Android phone ("Android" + "Mobile", client hint
// `mobile: true`); a wider mobile viewport (the tablet) sends Chrome on an
// Android tablet ("Android", no "Mobile", `mobile: false`, which is what an
// Android tablet sends). Those are the strings the front end's ParseViewport
// (lib/classify/capability_mask.cc) classifies as kMobile and kTablet, so the
// UA a render reports belongs to the class whose variant that render
// analyses; device_emulation_test feeds both through the classifier. The
// tablet is an Android tablet, not an iPad: iPadOS Safari sends a desktop
// "Macintosh" UA, which the front end serves the DESKTOP variant, and an iPad
// UA on a Blink engine with Chromium client hints is no device that exists.
// The desktop viewport is sent no override at all: it keeps the browser's
// own (headless desktop) UA, byte for byte as before User-Agent emulation.
//
// What a page sees of the client hints depends on the document:
// `navigator.userAgentData` exists only in a secure context. The capture
// endpoint navigates to the page's URL and its requests carry the
// Sec-CH-UA-* headers; the five renders that write the document into an
// about:blank window have no secure context, so page scripts there see the
// UA string and navigator.platform, and no navigator.userAgentData (measured
// in Chromium 145).
//
// A FRESH WINDOW (LoadFreshBlankDocument). Until User-Agent emulation those
// five renders wrote the document into the window the target was created
// with, and that window predates the emulation: `'ontouchstart' in window`
// stayed false on the phone and the tablet, and the
// Page.addScriptToEvaluateOnNewDocument scripts (page analysis's LCP/CLS
// observers, the scanners' pristine JSON.stringify) never ran in it with
// --headless=new. Each of them now navigates the target to about:blank once
// Page is enabled and its scripts are registered, and writes the document
// into THAT window: ontouchstart is true on the phone and the tablet (false
// on the desktop), the scripts run, and the document URL stays about:blank
// as before. Measured on the pinned Chromium 145 (50 renders per viewport):
// the navigation costs about 13 ms with --headless=new and under 1 ms with
// chrome-headless-shell, against a render of about 550 ms; the desktop
// renders that wait for networkIdle take about 1.06 s instead of 2.05 s in
// the fresh window. The navigation happens before
// Page.setLifecycleEventsEnabled, so the lifecycle events the render sees
// from the blank document are only the ones enabling them replays, as
// before; a render must still not act on that replayed networkIdle (the
// `content_set` guard of each render). Measured end to end with
// the production analyzers, page analysis now reports the page's LCP element
// with --headless=new too, its first-paint time changes, and the desktop
// viewport's layout shifts are counted (the phone's and tablet's stay 0:
// mobile emulation marks their shifts hadRecentInput, which the observer
// drops). None of it decides anything: CLS and FCP are not
// stored, the LCP element only feeds the telemetry-only
// OptimizationPolicy::Compute.

#ifndef PAGESPEED_SRC_BROWSER_DEVICE_EMULATION_H_
#define PAGESPEED_SRC_BROWSER_DEVICE_EMULATION_H_

#include <chrono>
#include <cstdint>
#include <initializer_list>
#include <memory>
#include <optional>
#include <string>
#include <utility>

#include "lib/classify/capability_mask.h"
#include "nlohmann/json.hpp"
#include "src/browser/cdp_types.h"
#include "uv.h"

namespace pagespeed {

// device_emulation.json "mobile_max_width".
inline constexpr uint32_t kMobileEmulationMaxWidth = 768;
// device_emulation.json "mobile_has_touch": a mobile device is a touch device.
inline constexpr bool kMobileEmulatesTouch = true;
// device_emulation.json "max_touch_points": what the emulated touch screen
// reports as navigator.maxTouchPoints (phones report 5).
inline constexpr uint32_t kMaxTouchPoints = 5;
// device_emulation.json "phone_max_width": a mobile viewport at most this
// wide is a phone and sends the phone's user agent, a wider one the
// tablet's. The phone class's width range
// (CapabilityMask::ViewportWidthRange(kMobile)).
inline constexpr uint32_t kPhoneMaxWidth = 479;

// device_emulation.json "user_agent_overrides": the
// Emulation.setUserAgentOverride params of the phone ("phone") and the
// tablet ("tablet"). Chrome on Android with its reduced UA string ("Android
// 10; K", what Chrome on Android has sent since Chrome 110) at the major
// version of the pinned Chromium, and the client hints that Chromium's own
// brands report (Chromium, plus its GREASE brand for that version).
inline constexpr const char* kPhoneUserAgent =
    "Mozilla/5.0 (Linux; Android 10; K) AppleWebKit/537.36 (KHTML, like "
    "Gecko) Chrome/145.0.0.0 Mobile Safari/537.36";
inline constexpr const char* kTabletUserAgent =
    "Mozilla/5.0 (Linux; Android 10; K) AppleWebKit/537.36 (KHTML, like "
    "Gecko) Chrome/145.0.0.0 Safari/537.36";
// navigator.platform of Chrome on Android.
inline constexpr const char* kAndroidNavigatorPlatform = "Linux armv81";
// userAgentMetadata.platform (Sec-CH-UA-Platform).
inline constexpr const char* kAndroidClientHintsPlatform = "Android";
// userAgentMetadata.brands (Sec-CH-UA).
inline constexpr const char* kClientHintsBrand = "Chromium";
inline constexpr const char* kClientHintsBrandVersion = "145";
inline constexpr const char* kClientHintsGreaseBrand = "Not:A-Brand";
inline constexpr const char* kClientHintsGreaseBrandVersion = "99";

// The analysis viewports, one per device class in CapabilityMask::Viewport
// order (mobile, tablet, desktop). BrowserAnalysisManager renders at these.
inline constexpr uint32_t kAnalysisViewportWidths[] = {375, 768, 1440};
inline constexpr uint32_t kAnalysisViewportHeights[] = {667, 1024, 900};

struct DeviceEmulation {
  // Emulation.setDeviceMetricsOverride `mobile`.
  bool mobile = false;
  // Emulation.setTouchEmulationEnabled `enabled`: on for a mobile device
  // (kMobileEmulatesTouch), off for a desktop window.
  bool has_touch = false;
  // The user agent the render reports (Emulation.setUserAgentOverride).
  enum class UserAgent : std::uint8_t {
    kBrowserDefault,  // no override: the desktop window
    kAndroidPhone,    // kPhoneUserAgent
    kAndroidTablet,   // kTabletUserAgent
  };
  UserAgent user_agent = UserAgent::kBrowserDefault;
};

// The emulation for a viewport `width` CSS px wide.
DeviceEmulation EmulationForViewportWidth(uint32_t width);

// The emulation of the analysis render for a device class: that of its
// analysis viewport (kAnalysisViewportWidths).
DeviceEmulation EmulationForViewport(CapabilityMask::Viewport viewport);

// The Emulation.setDeviceMetricsOverride params for a `width` x `height`
// viewport at device scale factor 1.
nlohmann::json DeviceMetricsOverrideParams(uint32_t width, uint32_t height);

// The Emulation.setTouchEmulationEnabled params for a `width` CSS px wide
// viewport: enabled for a mobile device, disabled for a desktop window.
nlohmann::json TouchEmulationParams(uint32_t width);

// The Emulation.setUserAgentOverride params for a `width` CSS px wide
// viewport: the phone's or the tablet's (device_emulation.json
// "user_agent_overrides"), or nullopt for a desktop window, which is sent no
// override.
std::optional<nlohmann::json> UserAgentOverrideParams(uint32_t width);

inline constexpr const char* kDeviceMetricsOverrideMethod =
    "Emulation.setDeviceMetricsOverride";
inline constexpr const char* kTouchEmulationMethod =
    "Emulation.setTouchEmulationEnabled";
inline constexpr const char* kUserAgentOverrideMethod =
    "Emulation.setUserAgentOverride";
inline constexpr const char* kNavigateMethod = "Page.navigate";
inline constexpr const char* kGetFrameTreeMethod = "Page.getFrameTree";
inline constexpr const char* kBlankDocumentUrl = "about:blank";
// How long LoadFreshBlankDocument waits for the blank document to commit
// before it gives up. Measured on the pinned Chromium 145, the first
// Page.getFrameTree after Page.navigate already reports the new document
// (30 of 30 renders per viewport, --headless=new and chrome-headless-shell);
// the budget is for a loaded browser.
inline constexpr std::chrono::milliseconds kBlankDocumentCommitTimeout{1000};
// The pause between two of those frame-tree reads, so a slow commit is
// waited for, not polled for in a tight loop of CDP round trips.
inline constexpr uint64_t kBlankDocumentPollIntervalMs = 5;

// Emulate the device for a `width` x `height` viewport on a CDP session, then
// run `then`. The one setup step every analysis render shares: the device
// metrics (DeviceMetricsOverrideParams), then touch (TouchEmulationParams),
// then, for a phone or a tablet, the user agent (UserAgentOverrideParams;
// a desktop window is sent none), each on the session's own `session_id`. A
// failed command ends the session with FinishError and `then` never runs.
//
// `session` is the render's session object (held by shared_ptr at every call
// site, so the callbacks keep it alive) with
//   bool Send(const CdpCommand&, CdpResponseCallback);
//   void FinishError(const std::string&);
//   std::string session_id;
template <typename SessionPtr, typename Then>
void EmulateDevice(SessionPtr session, uint32_t width, uint32_t height,
                   Then then) {
  CdpCommand metrics_cmd;
  metrics_cmd.method = kDeviceMetricsOverrideMethod;
  metrics_cmd.params = DeviceMetricsOverrideParams(width, height);
  metrics_cmd.session_id = session->session_id;
  session->Send(metrics_cmd, [session, width,
                              then = std::move(then)](auto result) mutable {
    if (!result.ok() || result->is_error()) {
      session->FinishError("viewport setup failed");
      return;
    }
    CdpCommand touch_cmd;
    touch_cmd.method = kTouchEmulationMethod;
    touch_cmd.params = TouchEmulationParams(width);
    touch_cmd.session_id = session->session_id;
    session->Send(touch_cmd, [session, width,
                              then = std::move(then)](auto result) mutable {
      if (!result.ok() || result->is_error()) {
        session->FinishError("touch emulation setup failed");
        return;
      }
      std::optional<nlohmann::json> ua_params = UserAgentOverrideParams(width);
      if (!ua_params.has_value()) {
        then();  // a desktop window keeps the browser's own user agent
        return;
      }
      CdpCommand ua_cmd;
      ua_cmd.method = kUserAgentOverrideMethod;
      ua_cmd.params = *std::move(ua_params);
      ua_cmd.session_id = session->session_id;
      session->Send(ua_cmd,
                    [session, then = std::move(then)](auto result) mutable {
                      if (!result.ok() || result->is_error()) {
                        session->FinishError("user agent setup failed");
                        return;
                      }
                      then();
                    });
    });
  });
}

namespace device_emulation_internal {

// The string at `path` in `json`, or "" when any step is missing or not of
// the expected type (never throws, whatever a reply carries).
std::string StringField(const nlohmann::json& json,
                        std::initializer_list<const char*> path);

// Run `fn` once on `loop` after `delay_ms`, counted from now (the loop's
// cached time is refreshed first, so the pause is never cut short by a stale
// loop time). The timer owns `fn` until it fires. Like every handle on the
// loop, a timer still pending when the loop is torn down without being run
// is not freed; the pause is kBlankDocumentPollIntervalMs, so that only
// happens if the loop stops within those milliseconds.
template <typename Fn>
void RunAfter(uv_loop_t* loop, uint64_t delay_ms, Fn fn) {
  struct Pending {
    uv_timer_t timer;
    Fn fn;
  };
  auto* pending = new Pending{{}, std::move(fn)};
  uv_timer_init(loop, &pending->timer);
  pending->timer.data = pending;
  uv_update_time(loop);
  uv_timer_start(
      &pending->timer,
      [](uv_timer_t* t) {
        auto* p = static_cast<Pending*>(t->data);
        Fn run = std::move(p->fn);
        uv_close(reinterpret_cast<uv_handle_t*>(t),
                 [](uv_handle_t* h) { delete static_cast<Pending*>(h->data); });
        run();
      },
      delay_ms, 0);
}

// One Page.getFrameTree round trip of LoadFreshBlankDocument: `then` runs
// once the main frame's loader is `loader_id`, the blank document's; until
// `deadline` (on `Clock`) it asks again, after it the session ends.
template <typename Clock, typename SessionPtr, typename Then>
void AwaitBlankDocumentCommit(SessionPtr session, std::string loader_id,
                              typename Clock::time_point deadline, Then then) {
  CdpCommand tree_cmd;
  tree_cmd.method = kGetFrameTreeMethod;
  tree_cmd.session_id = session->session_id;
  session->Send(tree_cmd, [session, loader_id = std::move(loader_id), deadline,
                           then = std::move(then)](auto result) mutable {
    if (!result.ok() || result->is_error()) {
      session->FinishError("blank document: getFrameTree failed");
      return;
    }
    const std::string current =
        StringField(result->result, {"frameTree", "frame", "loaderId"});
    if (current == loader_id) {
      then();
      return;
    }
    if (Clock::now() >= deadline) {
      session->FinishError("blank document did not commit");
      return;
    }
    // The session by weak reference across the pause: a render that ends
    // meanwhile (timeout, Chrome exit) skips the read.
    std::weak_ptr<typename SessionPtr::element_type> weak = session;
    RunAfter(session->client->loop(), kBlankDocumentPollIntervalMs,
             [weak, loader_id = std::move(loader_id), deadline,
              then = std::move(then)]() mutable {
               SessionPtr session = weak.lock();
               if (!session || session->completed) return;
               AwaitBlankDocumentCommit<Clock>(session, std::move(loader_id),
                                               deadline, std::move(then));
             });
  });
}

}  // namespace device_emulation_internal

// Give a render that writes its document with Page.setDocumentContent a
// fresh window (see "A FRESH WINDOW" above): navigate the target to
// about:blank, wait until that document has committed (the main frame's
// loader is the navigation's; Page.navigate can answer before the commit),
// then run `then`. Call it after EmulateDevice, Page.enable and any
// Page.addScriptToEvaluateOnNewDocument (so the new window gets the
// emulation and runs the scripts), and before
// Page.setLifecycleEventsEnabled (so no lifecycle event of the blank
// document is mistaken for the page's). A failed navigation, or a document
// that has not committed kBlankDocumentCommitTimeout after the navigation was
// answered, ends the session with FinishError, and `then` never runs. The
// frame-tree reads are kBlankDocumentPollIntervalMs apart, on the loop of
// `session->client`; `Clock` is for tests. Same `session` contract as
// EmulateDevice, plus `session->client->loop()` and `session->completed`.
template <typename Clock = std::chrono::steady_clock, typename SessionPtr,
          typename Then>
void LoadFreshBlankDocument(SessionPtr session, Then then) {
  CdpCommand nav_cmd;
  nav_cmd.method = kNavigateMethod;
  nav_cmd.params = {{"url", kBlankDocumentUrl}};
  nav_cmd.session_id = session->session_id;
  session->Send(nav_cmd, [session,
                          then = std::move(then)](auto result) mutable {
    if (!result.ok() || result->is_error() ||
        !device_emulation_internal::StringField(result->result, {"errorText"})
             .empty()) {
      session->FinishError("blank document navigation failed");
      return;
    }
    std::string loader_id =
        device_emulation_internal::StringField(result->result, {"loaderId"});
    if (loader_id.empty()) {
      // No new document to wait for (Chromium names the loader
      // of every cross-document navigation).
      then();
      return;
    }
    device_emulation_internal::AwaitBlankDocumentCommit<Clock>(
        session, std::move(loader_id),
        Clock::now() + kBlankDocumentCommitTimeout, std::move(then));
  });
}

}  // namespace pagespeed

#endif  // PAGESPEED_SRC_BROWSER_DEVICE_EMULATION_H_
