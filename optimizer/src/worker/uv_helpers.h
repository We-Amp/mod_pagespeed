// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

// Header-only libuv handle safety helpers.
//
// libuv handles must not be closed twice, and calling uv_close on a handle
// that uv_walk already closed during force shutdown causes undefined
// behavior.  These helpers encapsulate the uv_is_closing guard and the
// reinterpret_cast boilerplate that otherwise repeats at every call site.

#ifndef PAGESPEED_WORKER_UV_HELPERS_H_
#define PAGESPEED_WORKER_UV_HELPERS_H_

#include <uv.h>

namespace pagespeed {

// Close a handle if it is not already closing.  Returns true if close
// was initiated, false if the handle was already closing.
inline bool SafeClose(uv_handle_t* h, uv_close_cb cb = nullptr) {
  if (!uv_is_closing(h)) {
    uv_close(h, cb);
    return true;
  }
  return false;
}

template <typename T>
inline bool SafeClose(T* h, uv_close_cb cb = nullptr) {
  return SafeClose(reinterpret_cast<uv_handle_t*>(h), cb);
}

// Stop and close a timer if it is not already closing.
inline bool SafeTimerClose(uv_timer_t* timer, uv_close_cb cb = nullptr) {
  if (!uv_is_closing(reinterpret_cast<uv_handle_t*>(timer))) {
    uv_timer_stop(timer);
    uv_close(reinterpret_cast<uv_handle_t*>(timer), cb);
    return true;
  }
  return false;
}

// Check whether a handle is closing, without the cast boilerplate.
template <typename T>
inline bool IsClosing(T* h) {
  return uv_is_closing(reinterpret_cast<uv_handle_t*>(h));
}

}  // namespace pagespeed

#endif  // PAGESPEED_WORKER_UV_HELPERS_H_
