// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

// PageSpeed 2.0 - pieces every browser-analysis render shares. See the
// header.

#include "src/browser/render_support.h"

#include <cstdint>
#include <functional>
#include <string>
#include <utility>

#include "src/browser/cdp_types.h"
#include "uv.h"

namespace pagespeed {

CdpCommand FailRequestCommand(const std::string& request_id,
                              const std::string& session_id) {
  CdpCommand cmd;
  cmd.method = "Fetch.failRequest";
  cmd.params = {
      {"requestId", request_id},
      // `errorReason`, not `reason`: Chromium rejects the latter with
      // "Invalid parameters" and leaves the request paused.
      {"errorReason", "BlockedByClient"},
  };
  cmd.session_id = session_id;
  return cmd;
}

void OneShotTimer::Start(uv_loop_t* loop, uint64_t delay_ms,
                         std::function<void()> fire) {
  if (handle_ != nullptr) return;
  handle_ = new Handle;
  handle_->fire = std::move(fire);
  handle_->owner = this;
  uv_timer_init(loop, &handle_->timer);
  handle_->timer.data = handle_;
  uv_timer_start(
      &handle_->timer,
      [](uv_timer_t* t) {
        auto* h = static_cast<Handle*>(t->data);
        std::function<void()> fire = std::move(h->fire);
        if (h->owner != nullptr) h->owner->handle_ = nullptr;
        Close(h);
        // Last: `fire` may destroy the owner.
        if (fire) fire();
      },
      delay_ms, 0);
}

void OneShotTimer::Cancel() {
  if (handle_ == nullptr) return;
  Handle* h = std::exchange(handle_, nullptr);
  h->owner = nullptr;
  h->fire = nullptr;
  uv_timer_stop(&h->timer);
  Close(h);
}

void OneShotTimer::Close(Handle* handle) {
  uv_close(reinterpret_cast<uv_handle_t*>(&handle->timer),
           [](uv_handle_t* t) { delete static_cast<Handle*>(t->data); });
}

}  // namespace pagespeed
