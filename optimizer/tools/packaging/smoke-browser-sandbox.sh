#!/bin/bash
# SPDX-License-Identifier: Apache-2.0
#
# The headless-Chrome sandbox is on, and when it cannot be,
# browser analysis REFUSES rather than degrading.
#
#   smoke-browser-sandbox.sh [--binary PATH]
#
# Legs (design doc §7.1):
#   1  --browser-sandbox rejects anything but require|off -- in particular
#      `auto`, the silently-degrading mode this gate exists to remove
#   2  probe available    -> browser_sandbox="on" on /v1/health
#   3  probe unavailable  -> the daemon STARTS and SERVES, logs one named
#      ERROR, reports browser_sandbox="unavailable", and spawns no Chrome
#   4  --browser-sandbox=off -> browser_sandbox="off", WARNING banner present
#   5  no `--no-sandbox` appears in any spawned argv unless mode is `off`
#   6  /v1/health and /v1/stats carry `syscall_filter`, and
#      under a real kernel syscall filter that denies unshare(2) the daemon
#      still serves and the refusal names SIGSYS and a remedy -- not the
#      "did not complete" that used to cover a timeout too.  The
#      none -> filtered transition is asserted only when this host starts
#      unfiltered; otherwise it says so instead of banking a vacuous check.
#      Same rule for an unanswered probe: on a host stalled past the
#      probe's bounded wait the cause-naming checks report INCONCLUSIVE
#      (and the final banner says the filter path went unverified) rather
#      than failing as if the daemon had named the wrong cause
#
# Leg 3 needs a process for which unshare(CLONE_NEWUSER) is unavailable.  The
# probe treats uid 0 as unavailable (Chrome refuses to sandbox as root), so a
# root run IS the negative leg -- which is also the posture the container
# images are in today.  A non-root run on a userns-capable kernel is the
# positive leg.  CI runs both because its images differ; each leg skips when
# the host cannot produce its precondition, and the run asserts that at least
# one of the two fired.
#
# Self-contained: a scratch directory, no systemd, no packaging, no Chrome.

set -uo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO="$(cd "$HERE/../.." && pwd)"
BINARY="$REPO/bazel-bin/src/worker/factory_worker"
while [[ $# -gt 0 ]]; do
  case "$1" in
    --binary) BINARY="$2"; shift 2 ;;
    *) echo "usage: smoke-browser-sandbox.sh [--binary PATH]" >&2; exit 2 ;;
  esac
done
[[ -x "$BINARY" ]] || { echo "error: binary not found: $BINARY" >&2; exit 1; }
[[ "$(uname -s)" == "Linux" ]] || { echo "skip: Linux-only smoke" >&2; exit 0; }
command -v curl >/dev/null 2>&1 || { echo "error: curl is required" >&2; exit 1; }

# A refused flag exits at once; the bound only matters if the daemon starts.
TIMEOUT=()
command -v timeout >/dev/null 2>&1 && TIMEOUT=(timeout -k 5 30)
fails=0
legs_fired=0
check() { # label expected actual
  if [[ "$2" == "$3" ]]; then echo "ok: $1"; else
    echo "FAIL: $1 -- expected [$2], got [$3]" >&2; fails=$((fails+1)); fi
}

# Bounded stop: SIGTERM, up to 10 s for exit, then SIGKILL and up to 5 s more.
# Never a bare `wait` -- a worker that ignores SIGTERM must not hold the job
# until its timeout.  Polls `kill -0`; reaps only once the pid is gone.
stop_pid() { # pid
  local pid="${1:-}" i
  [[ -n "$pid" ]] || return 0
  kill "$pid" 2>/dev/null || true
  for ((i = 0; i < 100; i++)); do
    kill -0 "$pid" 2>/dev/null || break
    sleep 0.1
  done
  if kill -0 "$pid" 2>/dev/null; then
    echo "note: pid $pid ignored SIGTERM for 10 s; sending SIGKILL" >&2
    kill -9 "$pid" 2>/dev/null || true
    for ((i = 0; i < 50; i++)); do
      kill -0 "$pid" 2>/dev/null || break
      sleep 0.1
    done
  fi
  kill -0 "$pid" 2>/dev/null || wait "$pid" 2>/dev/null || true
  return 0
}

TMP="$(mktemp -d)"
WORKER_PID=""
cleanup() {
  stop_pid "$WORKER_PID"
  WORKER_PID=""
  rm -rf "$TMP" 2>/dev/null
  return 0
}
trap cleanup EXIT
trap 'exit 130' INT
trap 'exit 143' TERM

CACHE_DIR="$TMP/cache/v2"
RUN_DIR="$TMP/run"
mkdir -p "$CACHE_DIR" "$RUN_DIR"

# ---------------------------------------------------------------------------
# Leg 1: the flag surface.  `auto` must not exist.
# ---------------------------------------------------------------------------
${TIMEOUT[@]+"${TIMEOUT[@]}"} "$BINARY" --cache-dir "$CACHE_DIR" \
  --socket "$RUN_DIR/x.sock" --browser-sandbox auto >"$TMP/auto.log" 2>&1
check "--browser-sandbox=auto is refused" "1" "$?"
check "...and the refusal says there is deliberately no 'auto'" "1" \
  "$(grep -c "no 'auto'" "$TMP/auto.log")"

# ---------------------------------------------------------------------------
# Shared: start a daemon with browser analysis on and read /v1/health.
# The Chrome binary deliberately does not exist: no leg here needs a live
# browser, and every leg must be able to tell "refused" from "crashed".
# ---------------------------------------------------------------------------
# WRAP, when non-empty, is a command prefix that execs the daemon -- used by
# leg 6 to put a real kernel syscall filter around it.
WRAP=()
start_and_probe() { # logfile [extra args...]
  local log="$1"; shift
  local sock="$RUN_DIR/api-$RANDOM.sock"
  ${WRAP[@]+"${WRAP[@]}"} \
    "$BINARY" --cache-dir "$CACHE_DIR" --socket "$RUN_DIR/n-$RANDOM.sock" \
    --cache-size 16777216 --api-socket "$sock" \
    --enable-browser-analysis --chrome-binary /nonexistent/chrome \
    --browser-user-data-dir "$RUN_DIR/chrome" \
    "$@" >"$log" 2>&1 &
  WORKER_PID=$!
  local i
  for i in $(seq 1 600); do
    [[ -S "$sock" ]] && break
    sleep 0.2
  done
  [[ -S "$sock" ]] || echo "error: daemon did not start within 120 s ($log)" >&2
  HEALTH="$(curl -s --max-time 60 --unix-socket "$sock" http://localhost/v1/health 2>/dev/null)"
  STATS="$(curl -s --max-time 60 --unix-socket "$sock" http://localhost/v1/stats 2>/dev/null)"
  SANDBOX="$(printf '%s' "$HEALTH" \
    | sed -n 's/.*"browser_sandbox":"\([a-z]*\)".*/\1/p')"
  FILTER="$(printf '%s' "$HEALTH" \
    | sed -n 's/.*"syscall_filter":"\([a-z]*\)".*/\1/p')"
  FILTER_STATS="$(printf '%s' "$STATS" \
    | sed -n 's/.*"syscall_filter":"\([a-z]*\)".*/\1/p')"
}

stop_worker() {
  stop_pid "$WORKER_PID"
  WORKER_PID=""
  return 0
}

# ---------------------------------------------------------------------------
# Legs 2/3: `require` -- what the probe says, said out loud.
# ---------------------------------------------------------------------------
start_and_probe "$TMP/require.log"
check "the daemon serves in require mode whatever the probe says" "1" \
  "$([[ -n "$HEALTH" ]] && echo 1 || echo 0)"

# The field is always present and never empty -- a missing key
# means an old daemon, never "we did not look".  Its value here depends on the
# runner (a bare host says none; inside a container runtime profile, filtered),
# so the value is only asserted to be one of the three wire strings.  Leg 6
# below asserts the transition to "filtered" against a filter we install.
UNFILTERED_STATE="$FILTER"
check "/v1/health reports a syscall_filter wire string" "1" \
  "$(case "$FILTER" in none|filtered|unknown) echo 1 ;; *) echo 0 ;; esac)"
check "/v1/stats reports the same syscall_filter as /v1/health" "$FILTER" \
  "$FILTER_STATS"
echo "note: unfiltered syscall_filter=[$UNFILTERED_STATE] (euid=$(id -u))"

if [[ "$SANDBOX" == "unavailable" ]]; then
  legs_fired=1
  echo "leg: probe UNAVAILABLE (euid=$(id -u))"
  check "browser analysis refuses LOUDLY, naming the probe" "1" \
    "$(grep -c 'REFUSING to start browser analysis' "$TMP/require.log")"
  check "...and the refusal names the opt-out flag" "1" \
    "$(grep -c -- '--browser-sandbox=off' "$TMP/require.log")"
  check "...and records the observed kernel state as a diagnostic" "1" \
    "$(grep -c 'apparmor_restrict_unprivileged_userns' "$TMP/require.log")"
  check "...and never falls back to an unsandboxed browser" "0" \
    "$(grep -c -- '--no-sandbox' "$TMP/require.log")"
  check "...and no Chrome profile directory is created" "0" \
    "$([[ -d "$RUN_DIR/chrome" ]] && echo 1 || echo 0)"
elif [[ "$SANDBOX" == "on" ]]; then
  legs_fired=1
  echo "leg: probe AVAILABLE (euid=$(id -u))"
  check "the sandbox is reported on" "on" "$SANDBOX"
  check "the startup line records the observed kernel state" "1" \
    "$(grep -c 'Browser sandbox available' "$TMP/require.log")"
  check "no unsandboxed-browser warning is logged" "0" \
    "$(grep -c 'browser sandbox is OFF' "$TMP/require.log")"
else
  echo "FAIL: require mode reported browser_sandbox=[$SANDBOX]" >&2
  fails=$((fails+1))
fi
stop_worker

# ---------------------------------------------------------------------------
# Leg 4: the escape hatch is one flag, and it is loud.
# ---------------------------------------------------------------------------
start_and_probe "$TMP/off.log" --browser-sandbox off
check "--browser-sandbox=off reports off on /v1/health" "off" "$SANDBOX"
check "...and logs a SECURITY warning at startup" "1" \
  "$(grep -c 'browser sandbox DISABLED' "$TMP/off.log")"
check "...and does NOT log the refusal" "0" \
  "$(grep -c 'REFUSING to start browser analysis' "$TMP/off.log")"
stop_worker

# ---------------------------------------------------------------------------
# Leg 5: with browser analysis off entirely, the surface still answers.
# ---------------------------------------------------------------------------
API_SOCK="$RUN_DIR/plain.sock"
"$BINARY" --cache-dir "$CACHE_DIR" --socket "$RUN_DIR/plain-notify.sock" \
  --cache-size 16777216 --api-socket "$API_SOCK" \
  >"$TMP/disabled.log" 2>&1 &
WORKER_PID=$!
for _ in $(seq 1 600); do [[ -S "$API_SOCK" ]] && break; sleep 0.2; done
[[ -S "$API_SOCK" ]] || echo "error: daemon did not start within 120 s ($TMP/disabled.log)" >&2
check "browser analysis off reports disabled" "disabled" \
  "$(curl -s --max-time 60 --unix-socket "$API_SOCK" http://localhost/v1/health \
      | sed -n 's/.*"browser_sandbox":"\([a-z]*\)".*/\1/p')"
stop_worker

# ---------------------------------------------------------------------------
# Leg 6: under a real kernel syscall filter that denies
# unshare(2), the daemon must keep serving and say WHY browser analysis
# refused -- naming SIGSYS and a remedy.  Before this gate the same refusal
# read "the probe did not complete", which is what a loaded machine also
# looks like.
#
# Self-imposed, so the leg needs no systemd and no privileges: a ~40-line
# wrapper installs a seccomp filter over its own process and execs the daemon,
# which inherits it exactly as it would inherit the unit's filter.
# ---------------------------------------------------------------------------
# Leg 6 is allowed to skip on a host that genuinely cannot run it, but a skip
# must never be silent: nothing else in this script would notice that the one
# leg exercising the new code path did not run.  The counter below is asserted
# at the end, and only PAGESPEED_SMOKE_ALLOW_FILTER_LEG_SKIP=1 waives it --
# deliberately NOT set in CI, so a runner that loses its compiler turns red
# instead of quietly testing nothing.
filter_leg_fired=0
filter_leg_inconclusive=""
skip_filter_leg() { echo "SKIP leg 6: $1"; }

run_filter_leg() {
  if [[ "$(id -u)" == "0" ]]; then
    skip_filter_leg "running as uid 0, where the probe refuses before it forks"
    return 0
  fi
  local cc=""
  for candidate in "${CC:-}" cc gcc clang; do
    [[ -n "$candidate" ]] && command -v "$candidate" >/dev/null 2>&1 && {
      cc="$candidate"; break; }
  done
  if [[ -z "$cc" ]]; then
    skip_filter_leg "no C compiler to build the filter wrapper"
    return 0
  fi

  cat >"$TMP/denyunshare.c" <<'CEOF'
/* Installs a seccomp filter that kills the caller with SIGSYS on unshare(2),
 * then execs argv[1..].  Mirrors what systemd's SystemCallFilter= does to the
 * daemon (default action SCMP_ACT_KILL), without needing systemd.  Exits 70
 * when the kernel will not take the filter, so the caller can skip honestly.
 */
#include <linux/audit.h>
#include <linux/filter.h>
#include <linux/seccomp.h>
#include <stddef.h>
#include <stdio.h>
#include <sys/prctl.h>
#include <sys/syscall.h>
#include <unistd.h>
#ifndef SECCOMP_RET_KILL_THREAD
#define SECCOMP_RET_KILL_THREAD 0x00000000U
#endif
#if defined(__x86_64__)
#define ARCH_NR AUDIT_ARCH_X86_64
#elif defined(__aarch64__)
#define ARCH_NR AUDIT_ARCH_AARCH64
#else
#define ARCH_NR 0
#endif
int main(int argc, char **argv) {
  if (argc < 2) return 2;
  if (ARCH_NR == 0) { fprintf(stderr, "unsupported arch\n"); return 70; }
  struct sock_filter prog[] = {
      BPF_STMT(BPF_LD | BPF_W | BPF_ABS, offsetof(struct seccomp_data, arch)),
      BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, ARCH_NR, 1, 0),
      BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ALLOW),
      BPF_STMT(BPF_LD | BPF_W | BPF_ABS, offsetof(struct seccomp_data, nr)),
      BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, __NR_unshare, 0, 1),
      BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_KILL_THREAD),
      BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ALLOW),
  };
  struct sock_fprog fprog = {sizeof(prog) / sizeof(prog[0]), prog};
  if (prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0) != 0) return 70;
  if (syscall(SYS_seccomp, SECCOMP_SET_MODE_FILTER, 0, &fprog) != 0 &&
      prctl(PR_SET_SECCOMP, SECCOMP_MODE_FILTER, &fprog) != 0) {
    return 70;
  }
  execvp(argv[1], &argv[1]);
  return 70;
}
CEOF
  if ! "$cc" -O0 -o "$TMP/denyunshare" "$TMP/denyunshare.c" \
        >"$TMP/denyunshare.build.log" 2>&1; then
    skip_filter_leg "filter wrapper did not build"
    sed -n '1,10p' "$TMP/denyunshare.build.log" >&2
    return 0
  fi
  if ! "$TMP/denyunshare" /bin/true; then
    skip_filter_leg "this kernel would not install the filter"
    return 0
  fi

  echo "leg 6: daemon under a self-imposed filter denying unshare(2)"
  WRAP=("$TMP/denyunshare")
  start_and_probe "$TMP/filtered.log"
  WRAP=()

  filter_leg_fired=1

  check "the daemon still serves under a syscall filter" "1" \
    "$([[ -n "$HEALTH" ]] && echo 1 || echo 0)"

  # The none -> filtered transition is only evidence when we STARTED at none.
  # Container runtimes apply a default seccomp profile and many CI hosts run
  # filtered already, in which case asserting "filtered" here would pass no
  # matter what the code did.  Say so out loud rather than bank a vacuous ok.
  if [[ "$UNFILTERED_STATE" == "none" ]]; then
    check "syscall_filter flips none -> filtered under the wrapper" \
      "filtered" "$FILTER"
    check "/v1/stats agrees" "filtered" "$FILTER_STATS"
  elif [[ "$UNFILTERED_STATE" == "filtered" ]]; then
    echo "note: host already filtered (pre-state=[filtered]) --" \
         "transition not provable here; asserting the refusal reason only"
  else
    # "unknown" is not "filtered": it means the daemon could not READ
    # /proc/self/status Seccomp: at all (a kernel without it, a /proc the
    # process cannot see).  Reporting it as "already filtered" would claim
    # an observation that was never made.
    echo "note: pre-state=[$UNFILTERED_STATE] -- the daemon could not read" \
         "its own filter state, so the none -> filtered transition is not" \
         "observable here; asserting the refusal reason only"
  fi
  check "browser analysis refuses (the filter denied the probe)" \
    "unavailable" "$SANDBOX"

  # A probe that never answered is not evidence that the daemon named the
  # wrong cause -- it is evidence that nothing was observed.  On a host
  # stalled past the probe's whole bounded wait the refusal honestly says
  # "did not answer", and failing the cause-naming checks there reports a
  # load incident as a daemon defect.  Same honesty as the pre-state note
  # above: say "inconclusive" out loud rather than bank a vacuous result in
  # EITHER direction.  Every other case -- a refusal that names a wrong
  # cause, or no refusal at all -- stays a hard failure, and the leg still
  # has to have run at all (asserted below), so an inconclusive leg can
  # never be mistaken for a pass: the final banner says which of the two
  # happened.
  if grep -q 'did not answer within the bounded wait' "$TMP/filtered.log"; then
    filter_leg_inconclusive="the probe did not answer within the bounded wait (nothing was observed), so the refusal's cause-naming could not be verified"
    echo "INCONCLUSIVE leg 6: $filter_leg_inconclusive"
    echo "inconclusive: ...and the refusal names SIGSYS"
    echo "inconclusive: ...and names the syscall filter as the cause"
    echo "inconclusive: ...and points at the browser-analysis remedy"
  else
    check "...and the refusal names SIGSYS" "1" \
      "$(grep -c 'SIGSYS' "$TMP/filtered.log")"
    check "...and names the syscall filter as the cause" "1" \
      "$(grep -c 'syscall filter denied it' "$TMP/filtered.log")"
    check "...and points at the browser-analysis remedy" "1" \
      "$(grep -c 'browser-analysis documentation' "$TMP/filtered.log")"
  fi
  # Outcome-INDEPENDENT, so it stays hard even when the probe said nothing:
  # this sentence lives in the refusal's outer text, not in the probe's
  # reason clause, so the daemon emits it for every refusal including a
  # timeout.  Making it conditional would drop a check that was already
  # passing on exactly the path this branch exists to handle.
  check "...and offers the container remedy" "1" \
    "$(grep -c 'Chrome-compatible seccomp profile' "$TMP/filtered.log")"
  check "...and no longer reports it as a probe that 'did not complete'" "0" \
    "$(grep -c 'did not complete' "$TMP/filtered.log")"
  check "...and still never falls back to an unsandboxed browser" "0" \
    "$(grep -c -- '--no-sandbox' "$TMP/filtered.log")"
  stop_worker
}
run_filter_leg

if [[ "${PAGESPEED_SMOKE_ALLOW_FILTER_LEG_SKIP:-0}" == "1" ]]; then
  echo "note: leg 6 skip waived by PAGESPEED_SMOKE_ALLOW_FILTER_LEG_SKIP=1"
else
  check "leg 6 (syscall filter) actually ran" "1" "$filter_leg_fired"
fi

check "at least one probe leg fired" "1" "$legs_fired"

if [[ "$fails" -gt 0 ]]; then
  echo "smoke: $fails FAILURE(S)" >&2; exit 1
fi
if [[ -n "$filter_leg_inconclusive" ]]; then
  # Not "all checks passed": the one leg exercising the filter path ran but
  # observed nothing, and a green banner that does not say so is how a
  # stalled host quietly becomes an untested gate.  The run is not red --
  # nothing the daemon did failed -- but it did not verify the filter path,
  # and it says so.
  echo "smoke: all executed checks passed, but leg 6 was INCONCLUSIVE:"
  echo "smoke: $filter_leg_inconclusive"
  echo "smoke: the syscall-filter path was NOT verified by this run"
else
  echo "smoke: all checks passed"
fi
exit 0
