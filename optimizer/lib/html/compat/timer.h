// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.
//
// Compat header for the vendored 1.15 HTML kernel (#1130): stands in
// for the canonical pagespeed/kernel/base/timer.h header.
//
// The vendored kernel's entire Timer surface is `timer_->NowUs()` in
// html_parse.cc's ShowProgress/debug-timing path, reached only when a
// consumer installs a timer via HtmlParse::set_timer AND enables
// log_rewrite_timing — no 2.0 consumer does either (the pre-vendoring 2.0
// kernel forward-declared Timer and null-guarded the same calls). This
// header supplies the minimal abstract interface so the vendored code
// compiles unchanged; canonical's static time-unit constants and NowMs()
// are omitted because the vendored set never references them. If a future
// re-sync consumes them, extend this header in that change (fail-closed by
// compile error, like stl_util.h).

#ifndef PAGESPEED_LIB_HTML_COMPAT_TIMER_H_
#define PAGESPEED_LIB_HTML_COMPAT_TIMER_H_

#include <cstdint>

namespace net_instaweb {

// Timer interface, made virtual so it can be mocked for tests (canonical
// contract). Only the member the vendored kernel calls is declared.
class Timer {
 public:
  virtual ~Timer() = default;

  // Returns number of microseconds since 1970.
  virtual int64_t NowUs() const = 0;
};

}  // namespace net_instaweb

#endif  // PAGESPEED_LIB_HTML_COMPAT_TIMER_H_
