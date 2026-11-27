// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

#ifndef PAGESPEED_SRC_WORKER_UNSAFE_FORCE_ASYNC_CSS_H_
#define PAGESPEED_SRC_WORKER_UNSAFE_FORCE_ASYNC_CSS_H_

#include <string_view>

namespace pagespeed {

// Diagnostic switch: defer stylesheets even when the critical-CSS sufficiency
// gate refuses. It exists so tools/async-css-probe/ can exercise the DEFERRED
// markup on realistic input while the gate is — correctly — refusing to defer
// it, and it has no other purpose.
//
// CLI-ONLY, and deliberately absent from config_file.cc. A key parsed there
// reaches ApplyConfigJson and the RCU live_config_ hot-reload unless it is
// listed in IsNonReloadable, which would make this flippable on a RUNNING
// worker through the management API — with the startup warning below, the
// entire mitigation, never printing. It is also absent from PrintUsage and from
// the user documentation.
inline constexpr std::string_view kUnsafeForceAsyncCssFlag =
    "--unsafe-force-async-css";

// Printed once at startup when the switch is on. Tests pin both the flag name
// and the FOUC risk, so the warning cannot silently lose either half.
inline constexpr std::string_view kUnsafeForceAsyncCssWarning =
    "Warning: --unsafe-force-async-css is set. Stylesheets will be deferred "
    "even when the critical-CSS sufficiency gate refuses, which can flash "
    "unstyled content. Diagnostic use only: never serve production traffic "
    "with this flag.";

// Recognizes the flag. Returns true and sets *force when `arg` is exactly it.
// Leaves *force untouched otherwise, so no other argument — and no absent
// argument — can turn it on.
inline bool ParseUnsafeForceAsyncCssFlag(std::string_view arg, bool* force) {
  if (arg != kUnsafeForceAsyncCssFlag) return false;
  *force = true;
  return true;
}

// The async-CSS DEFERRAL decision with the diagnostic override applied. The
// override bypasses the sufficiency verdict for this decision alone: critical
// CSS is still extracted, injected, and coverage-measured exactly as before.
inline bool AsyncCssDeferralAllowed(bool sufficiency_verdict,
                                    bool unsafe_force) {
  return sufficiency_verdict || unsafe_force;
}

}  // namespace pagespeed

#endif  // PAGESPEED_SRC_WORKER_UNSAFE_FORCE_ASYNC_CSS_H_
