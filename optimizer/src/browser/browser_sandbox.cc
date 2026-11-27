// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

#include "src/browser/browser_sandbox.h"

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <string>
#include <string_view>

#include "absl/strings/str_cat.h"

#ifdef __linux__
#include <sched.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include <csignal>
#include <ctime>
#endif

namespace pagespeed {

const char* BrowserSandboxStateName(BrowserSandboxState state) {
  switch (state) {
    case BrowserSandboxState::kDisabled:
      return "disabled";
    case BrowserSandboxState::kOn:
      return "on";
    case BrowserSandboxState::kUnavailable:
      return "unavailable";
    case BrowserSandboxState::kOff:
      return "off";
  }
  return "disabled";
}

bool ParseBrowserSandboxMode(std::string_view text, BrowserSandboxMode* out) {
  if (out == nullptr) return false;
  if (text == "require") {
    *out = BrowserSandboxMode::kRequire;
    return true;
  }
  if (text == "off") {
    *out = BrowserSandboxMode::kOff;
    return true;
  }
  return false;
}

std::string BrowserSandboxProbeReason(BrowserSandboxProbeOutcome outcome,
                                      int signo) {
  switch (outcome) {
    case BrowserSandboxProbeOutcome::kAvailable:
      return {};
    case BrowserSandboxProbeOutcome::kDenied:
      return "unshare(CLONE_NEWUSER) was denied \u2014 unprivileged user "
             "namespaces are unavailable to this process";
    case BrowserSandboxProbeOutcome::kFilterKilled:
      // The one an operator can act on in a single step.  It deliberately
      // names the CLASS of cause and the two remedies rather than specific
      // directive or file names: the same probe runs under a systemd unit,
      // in a container under the runtime's own profile, and embedded in a
      // host process, and a message that named a systemd drop-in would be
      // wrong in two of those three.  The systemd-specific spelling lives
      // where it is actually true -- in the shipped unit and the packaged
      // browser-analysis drop-in example.
      return "the unshare(CLONE_NEWUSER) probe was killed by SIGSYS \u2014 a "
             "syscall filter denied it, not the kernel's userns policy; relax "
             "the syscall filter / namespace restriction this service runs "
             "under for the browser-analysis feature (see the "
             "browser-analysis documentation), or in a container use a "
             "Chrome-compatible seccomp profile";
    case BrowserSandboxProbeOutcome::kSignalled:
      return absl::StrCat(
          "the unshare(CLONE_NEWUSER) probe was killed by signal ", signo,
          " before it could answer");
    case BrowserSandboxProbeOutcome::kTimedOut:
      return "the unshare(CLONE_NEWUSER) probe did not answer within the "
             "bounded wait";
    case BrowserSandboxProbeOutcome::kSpawnFailed:
      return "the unshare(CLONE_NEWUSER) probe child could not be run";
  }
  return "the unshare(CLONE_NEWUSER) probe did not complete";
}

#ifdef __linux__
namespace {

// Reads a one-line /proc file; returns "?" when it cannot be read, so the
// diagnostic string always has a value for every key it names.
std::string ReadProcLine(const char* path) {
  FILE* f = std::fopen(path, "re");
  if (f == nullptr) return "?";
  char buf[64] = {0};
  const char* got = std::fgets(buf, sizeof(buf), f);
  std::fclose(f);
  if (got == nullptr) return "?";
  std::string s(buf);
  while (!s.empty() &&
         (s.back() == '\n' || s.back() == '\r' || s.back() == ' ')) {
    s.pop_back();
  }
  return s.empty() ? "?" : s;
}

// Restores a saved SIGCHLD disposition on scope exit, so every early return
// out of the probe puts the host's handler back.
class RestoreSigchld {
 public:
  explicit RestoreSigchld(const struct sigaction* saved) : saved_(saved) {}
  ~RestoreSigchld() {
    if (saved_ != nullptr) ::sigaction(SIGCHLD, saved_, nullptr);
  }
  RestoreSigchld(const RestoreSigchld&) = delete;
  RestoreSigchld& operator=(const RestoreSigchld&) = delete;

 private:
  const struct sigaction* saved_;
};

// What the reap saw.  H4 returned a bare int and folded "killed by a signal"
// into the same -1 as "timed out", which is precisely the distinction an
// operator needs once a syscall filter is in play: SIGSYS
// means a filter denied the call, and no amount of waiting will change it.
struct ReapResult {
  BrowserSandboxProbeOutcome outcome = BrowserSandboxProbeOutcome::kTimedOut;
  int signo = 0;
};

// waitpid(WNOHANG) with a bounded poll.  A probe that will not answer is not
// a probe that succeeded, but it is also not the same failure as a probe the
// kernel shot.
ReapResult ReapWithTimeout(pid_t pid, int timeout_ms) {
  const int kSliceMs = 5;
  for (int waited = 0; waited <= timeout_ms; waited += kSliceMs) {
    int status = 0;
    pid_t r = ::waitpid(pid, &status, WNOHANG);
    if (r == pid) {
      if (WIFEXITED(status)) {
        return {WEXITSTATUS(status) == 0
                    ? BrowserSandboxProbeOutcome::kAvailable
                    : BrowserSandboxProbeOutcome::kDenied,
                0};
      }
      if (WIFSIGNALED(status)) {
        const int signo = WTERMSIG(status);
        // SIGSYS from a child whose only syscall of interest was unshare(2)
        // is a seccomp denial: SECCOMP_RET_KILL_THREAD (systemd's default
        // action for SystemCallFilter=) and an unhandled SECCOMP_RET_TRAP
        // both surface here.
        return {signo == SIGSYS ? BrowserSandboxProbeOutcome::kFilterKilled
                                : BrowserSandboxProbeOutcome::kSignalled,
                signo};
      }
      return {BrowserSandboxProbeOutcome::kSpawnFailed, 0};
    }
    if (r < 0 && errno != EINTR) {
      return {BrowserSandboxProbeOutcome::kSpawnFailed, 0};
    }
    struct timespec ts = {0, static_cast<long>(kSliceMs) * 1000L * 1000L};
    ::nanosleep(&ts, nullptr);
  }
  // Out of budget.  The child may nevertheless have died in the last poll
  // window -- including of SIGSYS -- so inspect the status we reap instead of
  // discarding it and blaming a timeout for a filter denial.  SIGKILL is
  // idempotent here: if it already exited, the signal goes nowhere and
  // waitpid returns the real status.
  ::kill(pid, SIGKILL);
  int status = 0;
  // Blocking reap, so EINTR is reachable: any signal delivered to this
  // process (SIGCHLD is pinned to SIG_DFL above, but SIGTERM/SIGWINCH/a
  // profiler's timer are not) would otherwise abandon a child we have
  // already SIGKILLed -- leaving a zombie and reporting kTimedOut for a
  // status we were one retry away from reading.
  pid_t reaped = -1;
  do {
    reaped = ::waitpid(pid, &status, 0);
  } while (reaped < 0 && errno == EINTR);
  if (reaped == pid && WIFSIGNALED(status)) {
    const int signo = WTERMSIG(status);
    if (signo == SIGSYS) {
      return {BrowserSandboxProbeOutcome::kFilterKilled, signo};
    }
    // SIGKILL is the one we just sent, so it carries no information beyond
    // "it did not answer in time"; anything else is a real signal.
    if (signo != SIGKILL) {
      return {BrowserSandboxProbeOutcome::kSignalled, signo};
    }
  }
  return {BrowserSandboxProbeOutcome::kTimedOut, 0};
}

}  // namespace
#endif  // __linux__

BrowserSandboxProbe ProbeBrowserSandbox() {
  BrowserSandboxProbe result;

#ifndef __linux__
  result.available = true;
  result.diagnostics =
      "platform=non-linux (Chrome uses the platform sandbox; no user "
      "namespace required)";
  return result;
#else
  const std::string unpriv_clone =
      ReadProcLine("/proc/sys/kernel/unprivileged_userns_clone");
  const std::string apparmor_restrict =
      ReadProcLine("/proc/sys/kernel/apparmor_restrict_unprivileged_userns");
  const std::string max_userns =
      ReadProcLine("/proc/sys/user/max_user_namespaces");
  result.diagnostics = absl::StrCat(
      "euid=", static_cast<long>(::geteuid()),
      " kernel.unprivileged_userns_clone=", unpriv_clone,
      " kernel.apparmor_restrict_unprivileged_userns=", apparmor_restrict,
      " user.max_user_namespaces=", max_userns);

  // Chrome refuses to run its layer-1 sandbox as uid 0 and exits telling you
  // to pass --no-sandbox.  Checking it here turns that into a named startup
  // refusal instead of a Chrome crash on the first analysis.
  if (::geteuid() == 0) {
    result.available = false;
    result.outcome = BrowserSandboxProbeOutcome::kDenied;
    result.reason =
        "the daemon is running as root; Chrome refuses to sandbox itself as "
        "uid 0";
    return result;
  }

  // Reap deterministically whatever the process did to SIGCHLD.  Under
  // SIG_IGN a child is auto-reaped and waitpid never sees it, so the probe
  // would time out and report a false "unavailable" — an embedding host that
  // ignores SIGCHLD would silently lose browser analysis.  A host handler
  // could equally consume this child's status.  Pin SIG_DFL across the
  // fork+reap and restore whatever was there on the way out.
  struct sigaction probe_action{};
  struct sigaction saved_action{};
  probe_action.sa_handler = SIG_DFL;
  ::sigemptyset(&probe_action.sa_mask);
  probe_action.sa_flags = 0;
  const bool sigchld_swapped =
      (::sigaction(SIGCHLD, &probe_action, &saved_action) == 0);
  const RestoreSigchld restore_sigchld(sigchld_swapped ? &saved_action
                                                       : nullptr);

  // The only decision input: a direct test of the capability that matters.
  // Correct on every distro without reading a distro-specific knob.
  //
  // A probe that does not answer is not evidence about the capability: under
  // transient scheduling or memory pressure the child may never run inside
  // the budget (or the fork itself may fail), and reporting that as
  // "unavailable" would switch browser analysis off for the daemon's
  // lifetime over a momentary stall -- and hand the operator a remedy for a
  // cause that does not exist.  The definitive outcomes (answered, denied,
  // killed, signalled) settle the probe on the first attempt; only the
  // transient pair -- a timeout and a failed fork -- is retried, with the
  // budget doubling each attempt (2s, 4s, 8s), so even a machine stalled
  // for several seconds converges on the real cause (under a syscall filter
  // that is the SIGSYS kill, named as such) while a healthy machine still
  // answers in milliseconds on attempt one.
  ReapResult reaped;
  int fork_errno = 0;
  for (int attempt = 0;; ++attempt) {
    fork_errno = 0;
    const pid_t pid = ::fork();
    if (pid == 0) {
      // Child: attempt the namespace the Chrome zygote needs, report, exit.
      // _exit (not exit) — no atexit handlers, no flushed parent buffers.
      ::_exit(::unshare(CLONE_NEWUSER) == 0 ? 0 : 1);
    }
    if (pid < 0) {
      fork_errno = errno;
      reaped = {BrowserSandboxProbeOutcome::kSpawnFailed, 0};
    } else {
      reaped = ReapWithTimeout(pid, 2000 << attempt);
    }
    const bool transient =
        (reaped.outcome == BrowserSandboxProbeOutcome::kTimedOut ||
         reaped.outcome == BrowserSandboxProbeOutcome::kSpawnFailed);
    if (!transient || attempt == 2) {
      break;
    }
    const struct timespec pause = {0, 50L * 1000L * 1000L};
    ::nanosleep(&pause, nullptr);
  }

  result.outcome = reaped.outcome;
  result.available = (reaped.outcome == BrowserSandboxProbeOutcome::kAvailable);
  if (!result.available) {
    if (reaped.outcome == BrowserSandboxProbeOutcome::kSpawnFailed &&
        fork_errno != 0) {
      result.reason = absl::StrCat("could not fork the probe child: ",
                                   std::strerror(fork_errno));
    } else {
      result.reason = BrowserSandboxProbeReason(reaped.outcome, reaped.signo);
    }
  }
  return result;
#endif  // __linux__
}

}  // namespace pagespeed
