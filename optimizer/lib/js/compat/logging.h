// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.
//
// Compat header for the vendored 1.15 JS kernel: stands in for
// the canonical base/logging.h header, supplying the exact macro surface the
// vendored files consume — the DCHECK family and LOG(DFATAL), both
// stream-accepting.
//
// Semantics mirror the canonical debug/release split:
//   * DCHECK* — live assert in !NDEBUG builds, no-op in NDEBUG (same as the
//     asserts the pre-vendoring 2.0 kernel used).
//   * LOG(DFATAL) — asserts in !NDEBUG builds (canonical DFATAL is fatal in
//     debug); in NDEBUG builds execution continues and the message is
//     discarded (canonical logs at ERROR; this library has no logging sink,
//     and every DFATAL site is an unreachable-by-contract state).
//
// These macros are include-guarded per name so a TU that already defines
// them (a real logging library) wins; within lib/js only the vendored .cc
// files include this header. See lib/js/CLAUDE.md.

#ifndef PAGESPEED_LIB_JS_COMPAT_LOGGING_H_
#define PAGESPEED_LIB_JS_COMPAT_LOGGING_H_

#include <cassert>

namespace pagespeed::js_compat {

// Discards everything streamed into it.
struct LogSink {
  template <typename T>
  LogSink& operator<<(const T&) {
    return *this;
  }
};

inline LogSink CheckedSink(bool ok) {
  assert(ok);
  (void)ok;
  return {};
}

}  // namespace pagespeed::js_compat

#ifndef DCHECK
#define DCHECK(cond) \
  ::pagespeed::js_compat::CheckedSink(static_cast<bool>(cond))
#define DCHECK_EQ(a, b) DCHECK((a) == (b))
#define DCHECK_NE(a, b) DCHECK((a) != (b))
#define DCHECK_GE(a, b) DCHECK((a) >= (b))
#define DCHECK_GT(a, b) DCHECK((a) > (b))
#define DCHECK_LE(a, b) DCHECK((a) <= (b))
#define DCHECK_LT(a, b) DCHECK((a) < (b))
#endif  // DCHECK

#ifndef LOG
// Only LOG(DFATAL) is used by the vendored kernel; the severity token is
// expanded so an unexpected severity fails the build instead of silently
// no-opping.
#define LOG(severity) PAGESPEED_JS_COMPAT_LOG_##severity
#define PAGESPEED_JS_COMPAT_LOG_DFATAL \
  ::pagespeed::js_compat::CheckedSink(false)
#endif  // LOG

#endif  // PAGESPEED_LIB_JS_COMPAT_LOGGING_H_
