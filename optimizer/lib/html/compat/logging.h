// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.
//
// Compat header for the vendored 1.15 HTML kernel (#1130): stands in
// for the canonical base/logging.h header, supplying the macro surface the
// vendored files consume — the DCHECK family, the CHECK family (added for
// the HTML kernel; canonical html_parse.cc/html_keywords.cc/content_type.cc
// have live CHECK sites), LOG(DFATAL|WARNING|FATAL), and VLOG — all
// stream-accepting.
//
// Semantics mirror the canonical debug/release split:
//   * CHECK(cond) — ALWAYS-ON, matching canonical: on failure it prints the
//     source location to stderr and aborts (streamed values are discarded;
//     this library has no logging sink). html_parse.cc's
//     `CHECK(ok) << url` in SetUrlForTesting is a real validity gate that
//     must fire in release builds too.
//   * DCHECK* — live assert in !NDEBUG builds, no-op in NDEBUG (same as the
//     asserts the pre-vendoring 2.0 kernel used).
//   * LOG(DFATAL) — asserts in !NDEBUG (canonical DFATAL is fatal in debug);
//     in NDEBUG execution continues and the message is discarded (canonical
//     logs at ERROR; 2.0 has no logging sink and every DFATAL site is an
//     unreachable-by-contract state — same ruling as D1).
//   * LOG(WARNING) — discarded. Canonical logs; the pre-vendoring 2.0 kernel
//     never emitted these either, so this preserves 2.0 behavior.
//   * LOG(FATAL) — like CHECK(false): stderr location + abort. Canonical
//     FATAL aborts; the pre-vendoring 2.0 html_node.h used std::abort() at
//     the same sites (S9).
//   * VLOG(n) — discarded (verbose-only diagnostics).
//
// These macros are include-guarded per name so a TU that already defines
// them wins; within lib/html only the vendored files include this header.
// Both orders against lib/js/compat/logging.h are safe: D1's macros are
// likewise per-name-guarded. Note D1's LOG only maps DFATAL — a TU that
// takes D1's LOG first and then uses LOG(WARNING) itself fails to compile
// (fail-closed, never silent); no such TU exists (the two kernels' .cc
// files never share a TU). See lib/html/CLAUDE.md.

#ifndef PAGESPEED_LIB_HTML_COMPAT_LOGGING_H_
#define PAGESPEED_LIB_HTML_COMPAT_LOGGING_H_

#include <cassert>
#include <cstdio>
#include <cstdlib>

namespace pagespeed::html_compat {

// Discards everything streamed into it.
struct LogSink {
  template <typename T>
  LogSink& operator<<(const T&) {
    return *this;
  }
};

// Debug-check sink: asserts the condition (no-op under NDEBUG).
inline LogSink CheckedSink(bool ok) {
  assert(ok);
  (void)ok;
  return {};
}

// Fatal sink: prints the source location on construction and aborts on
// destruction (i.e. after any streamed values are discarded), so
// `CHECK(cond) << detail` enforces `cond` exactly like canonical CHECK.
class CheckSink {
 public:
  CheckSink(bool ok, const char* file, int line) : ok_(ok) {
    if (!ok_) {
      std::fprintf(stderr, "%s:%d: Check failed\n", file, line);
    }
  }
  template <typename T>
  CheckSink& operator<<(const T&) {
    return *this;
  }
  ~CheckSink() {
    if (!ok_) {
      std::abort();
    }
  }

 private:
  bool ok_;
};

}  // namespace pagespeed::html_compat

#ifndef DCHECK
#define DCHECK(cond) \
  ::pagespeed::html_compat::CheckedSink(static_cast<bool>(cond))
#define DCHECK_EQ(a, b) DCHECK((a) == (b))
#define DCHECK_NE(a, b) DCHECK((a) != (b))
#define DCHECK_GE(a, b) DCHECK((a) >= (b))
#define DCHECK_GT(a, b) DCHECK((a) > (b))
#define DCHECK_LE(a, b) DCHECK((a) <= (b))
#define DCHECK_LT(a, b) DCHECK((a) < (b))
#endif  // DCHECK

#ifndef CHECK
#define CHECK(cond)                                                      \
  ::pagespeed::html_compat::CheckSink(static_cast<bool>(cond), __FILE__, \
                                      __LINE__)
#define CHECK_EQ(a, b) CHECK((a) == (b))
#define CHECK_NE(a, b) CHECK((a) != (b))
#define CHECK_GE(a, b) CHECK((a) >= (b))
#define CHECK_GT(a, b) CHECK((a) > (b))
#define CHECK_LE(a, b) CHECK((a) <= (b))
#define CHECK_LT(a, b) CHECK((a) < (b))
#endif  // CHECK

#ifndef LOG
// The severity token is expanded so an unexpected severity fails the build
// instead of silently no-opping (D1 pattern). WARNING is a discard sink (2.0
// has no logging sink; the pre-vendoring kernel was equally silent), FATAL is
// the aborting CheckSink, DFATAL asserts in debug and discards in release.
#define LOG(severity) PAGESPEED_HTML_COMPAT_LOG_##severity
#define PAGESPEED_HTML_COMPAT_LOG_DFATAL \
  ::pagespeed::html_compat::CheckedSink(false)
#define PAGESPEED_HTML_COMPAT_LOG_WARNING ::pagespeed::html_compat::LogSink()
#define PAGESPEED_HTML_COMPAT_LOG_FATAL \
  ::pagespeed::html_compat::CheckSink(false, __FILE__, __LINE__)
#endif  // LOG

#ifndef VLOG
#define VLOG(n) ::pagespeed::html_compat::LogSink()
#endif  // VLOG

#endif  // PAGESPEED_LIB_HTML_COMPAT_LOGGING_H_
