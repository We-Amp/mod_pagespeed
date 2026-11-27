// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

// Headless-Chrome sandbox resolution.
//
// Chrome's Linux sandbox has two layers.  Layer 2 (the seccomp-bpf renderer
// filter) is installed by Chrome itself and needs nothing from us.  Layer 1
// (the namespace/zygote sandbox) needs EITHER unprivileged user namespaces OR
// a setuid `chrome-sandbox` helper.  The daemon's unit sets
// NoNewPrivileges=yes, under which execve of a setuid binary does not
// elevate, so the setuid path is structurally unavailable and We-Amp never
// ships a setuid-root binary: user namespaces or nothing.
//
// This header exposes a direct probe of that one capability plus the mode
// the operator selected.  There is deliberately no `auto` mode: an `auto`
// that silently falls back to `--no-sandbox` is the defect being fixed.

#ifndef PAGESPEED_SRC_BROWSER_BROWSER_SANDBOX_H_
#define PAGESPEED_SRC_BROWSER_BROWSER_SANDBOX_H_

#include <cstdint>
#include <string>
#include <string_view>

namespace pagespeed {

// What the operator asked for.  `require` is the default and never degrades.
enum class BrowserSandboxMode : std::uint8_t {
  kRequire,  // sandbox or refuse (default)
  kOff,      // deliberate opt-out: --no-sandbox is re-added, loudly
};

// What the daemon resolved at startup.  Reported by GET /v1/health and
// /v1/stats so a console or monitoring check can see it without reading logs.
enum class BrowserSandboxState : std::uint8_t {
  kDisabled,     // browser analysis is off; nothing was probed
  kOn,           // probe succeeded, Chrome runs sandboxed
  kUnavailable,  // probe failed in `require` mode: analysis REFUSED
  kOff,          // operator opted out; Chrome runs unsandboxed
};

// Stable wire strings for the health/stats surfaces:
// "disabled" | "on" | "unavailable" | "off".
const char* BrowserSandboxStateName(BrowserSandboxState state);

// Parses `require` / `off` (case-sensitive, as the flag documents them).
// Returns false for anything else — including `auto`, which does not exist.
bool ParseBrowserSandboxMode(std::string_view text, BrowserSandboxMode* out);

// How the probe child ended.  Kept separate from `available` because the
// three not-available outcomes need three different remedies, and H4 shipped
// with two of them collapsed into one string: a child killed by the kernel
// and a child that simply took too long both reported "did not complete".
// The daemon runs inside a systemd-installed syscall
// filter, so "the filter killed the probe" becomes the single most likely
// cause of an unavailable verdict on a correctly configured host -- and the
// one an operator can fix in one step, if we name it.
enum class BrowserSandboxProbeOutcome : std::uint8_t {
  kAvailable,     // the child created a user namespace
  kDenied,        // the child ran and unshare(CLONE_NEWUSER) returned an error
  kFilterKilled,  // the child was killed by SIGSYS: a syscall filter denied it
  kSignalled,     // the child died on some other signal
  kTimedOut,      // the child did not answer within the bounded wait
  kSpawnFailed,   // fork() or the reap itself failed
};

// The reason clause for a non-available outcome, as it appears in the startup
// refusal.  Pure and platform-independent so the exact wording -- which the
// packaging smoke greps for -- is unit-testable without arranging a real
// kernel denial.  Returns the empty string for kAvailable.  `signo` is only
// read for kSignalled.
std::string BrowserSandboxProbeReason(BrowserSandboxProbeOutcome outcome,
                                      int signo);

struct BrowserSandboxProbe {
  // The single decision input: can this process create an unprivileged user
  // namespace, and is it an identity Chrome will accept a sandbox for?
  bool available = false;

  // How the probe ended.  `available` is exactly `outcome == kAvailable`;
  // this field says which of the ways it failed, so a caller (and a test)
  // can distinguish "a syscall filter killed the probe" from "the machine
  // was loaded and the child was slow" without matching on prose.
  BrowserSandboxProbeOutcome outcome = BrowserSandboxProbeOutcome::kAvailable;

  // Why not, when !available — one clause, suitable for a log line.
  std::string reason;

  // Observed kernel/LSM state.  DIAGNOSTICS ONLY: recorded in the log line so
  // an operator can act, never consulted to make the decision (a sysctl says
  // what a distro intends; the probe says what the kernel does).
  std::string diagnostics;
};

// Forks a child that attempts unshare(CLONE_NEWUSER) and reports whether it
// succeeded, plus the identity check Chrome itself performs (it refuses to
// run sandboxed as uid 0).  Reaps with a bounded wait; a probe that does not
// answer counts as unavailable.  Safe to call once at startup, before any
// Chrome spawn.
//
// On non-Linux platforms Chrome uses the platform sandbox and needs nothing
// from us, so the probe reports available with a diagnostic saying so.
BrowserSandboxProbe ProbeBrowserSandbox();

}  // namespace pagespeed

#endif  // PAGESPEED_SRC_BROWSER_BROWSER_SANDBOX_H_
