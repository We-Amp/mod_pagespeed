// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

// PageSpeed 2.0 - pieces every browser-analysis render shares.
//
// FailPausedRequest: the one Fetch.failRequest of the renders. Each render
// blocks the requests it does not serve itself; until the read-the-page fix six
// copies of
// that call sent `reason` instead of Chromium's `errorReason` and dropped
// the reply, so Chromium's "Invalid parameters" answer went unseen while the
// request stayed paused forever. The helper sends the right parameter and
// checks the reply: the first error reply of a render is reported to the
// CdpClient's diagnostic handler (the worker logs it as a warning).
//
// OneShotTimer: a libuv timer for a render's settle wait (page analysis
// collects at the page's networkIdle or kSettleAfterLoadMs after its load
// event, whichever comes first).

#ifndef PAGESPEED_SRC_BROWSER_RENDER_SUPPORT_H_
#define PAGESPEED_SRC_BROWSER_RENDER_SUPPORT_H_

#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <utility>

#include "absl/strings/str_cat.h"
#include "src/browser/cdp_client.h"
#include "src/browser/cdp_types.h"
#include "uv.h"

namespace pagespeed {

// How long a render that waits for the page to settle waits after the
// page's (post-write) load event before it collects anyway. A page that
// keeps a request in flight at least every 500 ms (polling, beacons) never
// reaches networkIdle; without this bound such a render ran into its 60 s
// session timeout.
inline constexpr uint64_t kSettleAfterLoadMs = 2000;

// The Fetch.failRequest command that blocks paused request `request_id` on
// CDP session `session_id`.
CdpCommand FailRequestCommand(const std::string& request_id,
                              const std::string& session_id);

// Block paused request `request_id` (FailRequestCommand) on the render
// `session`, and check the reply: the first error reply of the render is
// reported through `session->client->Diagnose()`, naming `render` (a
// transport failure, such as the pipe closing, is not a refusal). Later
// error replies of the same render are not reported again
// (`session->fetch_error_reported`). A reply that arrives after the render
// finished is ignored: Chromium refuses commands for a closed target.
//
// `session` is the render's session object, held by shared_ptr, with
//   bool Send(const CdpCommand&, CdpResponseCallback);
//   CdpClient* client;
//   std::string session_id;
//   bool completed;
//   bool fetch_error_reported;
template <typename SessionPtr>
void FailPausedRequest(const SessionPtr& session, const std::string& request_id,
                       std::string_view render) {
  CdpCommand cmd = FailRequestCommand(request_id, session->session_id);
  session->Send(cmd, [session, render](auto result) {
    if (session->completed || session->fetch_error_reported) return;
    // Only Chromium's own error reply is a refusal. A transport status (the
    // pipe closed, Chrome exited or was stopped, a command timeout) ends or
    // fails the render through its own path and is not reported here.
    if (!result.ok() || !result->is_error()) return;
    session->fetch_error_reported = true;
    session->client->Diagnose(absl::StrCat(
        render, ": Fetch.failRequest was refused (", result->error_message,
        "); the blocked request stays paused"));
  });
}

// A one-shot timer on a libuv loop. Start() arms it once; the callback runs
// on the loop after the delay unless Cancel() ran first. The owner must
// Cancel() (or let it fire) while the loop is still running; the destructor
// cancels as a last resort.
class OneShotTimer {
 public:
  OneShotTimer() = default;
  ~OneShotTimer() { Cancel(); }
  OneShotTimer(const OneShotTimer&) = delete;
  OneShotTimer& operator=(const OneShotTimer&) = delete;

  // Arms the timer. A second Start() while armed does nothing.
  void Start(uv_loop_t* loop, uint64_t delay_ms, std::function<void()> fire);
  // Disarms the timer; the callback never runs. Safe when not armed.
  void Cancel();
  bool armed() const { return handle_ != nullptr; }

 private:
  struct Handle {
    uv_timer_t timer;
    std::function<void()> fire;
    OneShotTimer* owner = nullptr;
  };
  static void Close(Handle* handle);
  Handle* handle_ = nullptr;
};

}  // namespace pagespeed

#endif  // PAGESPEED_SRC_BROWSER_RENDER_SUPPORT_H_
