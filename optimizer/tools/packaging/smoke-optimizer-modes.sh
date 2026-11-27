#!/bin/bash
# SPDX-License-Identifier: Apache-2.0
#
# Local smoke test for the daemon permission model:
# run factory_worker NON-root against a scratch cache dir and assert every
# mode the design pins, plus the loud refusal-to-start legs.
#
#   smoke-optimizer-modes.sh [--binary PATH]
#
# Asserts (design doc §4 table):
#   notify/health/mgmt sockets  0660
#   volume file(s)              0660
#   pagespeed-shared.conf       0640, and carries cache_dir_generation=3
#   .pagespeed-serve-stats      0660
#   no world-writable bit anywhere under the cache or socket dirs
#   missing cache dir           -> refuses to start, log names "does not exist"
#   unwritable cache dir        -> refuses, log says "permission denied"
#   foreign-owned VOLUME file   -> refuses, log says "cannot own" (root only)
#   foreign-owned bystander     -> starts anyway: the cache dir is
#                                  group-writable by design, so only the
#                                  daemon's OWN files carry the ownership
#                                  rule (root only)
#
# POSIX-only (modes are a POSIX concept here). Cheap and local: no
# systemd, no packaging, no web server. The full multi-distro install
# rig (design §9) is a follow-up arc; this catches mode regressions in
# every local/CI run.

set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO="$(cd "$HERE/../.." && pwd)"
BINARY="$REPO/bazel-bin/src/worker/factory_worker"
while [[ $# -gt 0 ]]; do
  case "$1" in
    --binary) BINARY="$2"; shift 2 ;;
    *) echo "usage: smoke-optimizer-modes.sh [--binary PATH]" >&2; exit 2 ;;
  esac
done
[[ -x "$BINARY" ]] || { echo "error: binary not found: $BINARY" >&2; exit 1; }
[[ "$(uname -s)" == "Linux" || "$(uname -s)" == "Darwin" ]] || {
  echo "skip: POSIX-only smoke" >&2; exit 0; }

fails=0
check() { # label expected actual
  if [[ "$2" == "$3" ]]; then echo "ok: $1"; else
    echo "FAIL: $1 -- expected [$2], got [$3]" >&2; fails=$((fails+1)); fi
}

mode_of() { stat -c '%a' "$1" 2>/dev/null || stat -f '%Lp' "$1"; }

TMP="$(mktemp -d)"
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

# Every step here must be failure-proof: `set -e` is still in force while the
# EXIT trap runs, so a single non-zero command inside it aborts the trap AND
# becomes the script's exit status -- which would report a fully passing run
# as a failure. (It did: `kill ""` after the pid file is removed.)
cleanup() {
  local pid=""
  if [[ -f "$TMP/worker.pid" ]]; then
    pid="$(cat "$TMP/worker.pid" 2>/dev/null || true)"
  fi
  stop_pid "$pid"
  rm -rf "$TMP" || true
}
trap cleanup EXIT
CACHE_DIR="$TMP/cache/v2"
RUN_DIR="$TMP/run"
mkdir -p "$CACHE_DIR" "$RUN_DIR"

# --- happy path -----------------------------------------------------------
"$BINARY" --cache-dir "$CACHE_DIR" --socket "$RUN_DIR/notify.sock" \
  --cache-size 16777216 --log-level warning >"$TMP/worker.log" 2>&1 &
echo $! > "$TMP/worker.pid"

# Wait for ALL THREE sockets, not just the notification one.
#
# The daemon binds them in sequence -- notify, then health, then mgmt -- so
# the notification socket appearing says nothing about the other two, and
# waiting on pagespeed-shared.conf says even less: the daemon writes it, and
# the serve-stats mmap, well before it binds anything (worker.cc). Stat-ing
# all three after waiting on notify alone is a race the mgmt socket loses
# whenever the daemon is descheduled between the binds, which on a loaded
# machine it is: `FAIL: mgmt socket mode 0660 -- expected [660], got []` with
# the socket simply not there yet. Reproduced 1 run in 12 under load, against
# a binary whose sources had not changed at all.
#
# The budget is 120 s, not 10: on a busy build host the daemon needs well over
# 10 s to open its cache volume and start listening, and a quiet host still
# breaks out of the loop in a fraction of a second. If the daemon has already
# exited there is nothing left to wait for, so stop polling early.
for _ in $(seq 1 1200); do
  [[ -S "$RUN_DIR/notify.sock" && -S "$RUN_DIR/notify.sock.health" && \
     -S "$RUN_DIR/notify.sock.mgmt" ]] && break
  kill -0 "$(cat "$TMP/worker.pid")" 2>/dev/null || break
  sleep 0.1
done
# Name the socket that never showed up: a bare "daemon did not start" sends
# the reader after the daemon rather than after the one bind that lagged, and
# the health socket was not checked here at all.
for sock in notify.sock notify.sock.health notify.sock.mgmt; do
  [[ -S "$RUN_DIR/$sock" ]] || {
    echo "FAIL: daemon did not start ($sock never appeared)" >&2
    if kill -0 "$(cat "$TMP/worker.pid")" 2>/dev/null; then
      echo "daemon is still running after 120 s" >&2
    else
      status=0
      wait "$(cat "$TMP/worker.pid")" 2>/dev/null || status=$?
      echo "daemon exited with status $status" >&2
    fi
    echo "--- daemon output (last 50 lines of $TMP/worker.log) ---" >&2
    tail -n 50 "$TMP/worker.log" >&2 || true
    exit 1; }
done
# The volume, shared config and serve-stats are all created before the sockets
# are bound, so by here they exist. Kept as a cheap explicit guard.
for _ in $(seq 1 100); do
  [[ -f "$CACHE_DIR/pagespeed-shared.conf" && \
     -f "$CACHE_DIR/.pagespeed-serve-stats" ]] && break
  sleep 0.1
done

check "notify socket mode 0660" "660" "$(mode_of "$RUN_DIR/notify.sock")"
check "health socket mode 0660" "660" "$(mode_of "$RUN_DIR/notify.sock.health")"
check "mgmt socket mode 0660" "660" "$(mode_of "$RUN_DIR/notify.sock.mgmt")"
check "shared config mode 0640" "640" "$(mode_of "$CACHE_DIR/pagespeed-shared.conf")"
check "serve-stats mode 0660" "660" "$(mode_of "$CACHE_DIR/.pagespeed-serve-stats")"
volume="$(find "$CACHE_DIR" -maxdepth 1 -name 'cache-*' -type f | head -1)"
[[ -n "$volume" ]] || { echo "FAIL: no volume file in $CACHE_DIR" >&2; exit 1; }
check "volume file mode 0660" "660" "$(mode_of "$volume")"
check "shared config publishes cache_dir_generation=3" "1" \
  "$(grep -c '^cache_dir_generation=3$' "$CACHE_DIR/pagespeed-shared.conf")"
check "no world-writable file under cache dir" "0" \
  "$(find "$CACHE_DIR" -perm -o+w | wc -l | tr -d ' ')"
check "no world-writable entry under socket dir" "0" \
  "$(find "$RUN_DIR" -perm -o+w | wc -l | tr -d ' ')"

stop_pid "$(cat "$TMP/worker.pid")"
rm -f "$TMP/worker.pid"

# --- refusal legs ---------------------------------------------------------
check "missing cache dir refuses (exit)" "1" \
  "$("$BINARY" --cache-dir "$TMP/absent/v2" --socket "$RUN_DIR/x.sock" \
      >"$TMP/missing.log" 2>&1; echo $?)"
check "missing cache dir log says 'does not exist'" "1" \
  "$(grep -c 'does not exist' "$TMP/missing.log")"

LOCKED="$TMP/locked/v2"
mkdir -p "$LOCKED"
chmod 0500 "$LOCKED"
if [[ "$(id -u)" -ne 0 ]]; then
  check "unwritable cache dir refuses (exit)" "1" \
    "$("$BINARY" --cache-dir "$LOCKED" --socket "$RUN_DIR/y.sock" \
        >"$TMP/locked.log" 2>&1; echo $?)"
  check "unwritable cache dir log says 'permission denied'" "1" \
    "$(grep -ci 'permission denied' "$TMP/locked.log")"
else
  echo "skip: unwritable-dir leg needs a non-root run"
fi
chmod 0700 "$LOCKED"

if [[ "$(id -u)" -eq 0 ]]; then
  # A foreign-owned file the daemon WOULD rewrite (named after the volume
  # stem) must refuse.
  FOREIGN="$TMP/foreign/v2"
  mkdir -p "$FOREIGN"
  touch "$FOREIGN/cache-6-0123456789abcdef"
  chown 1:1 "$FOREIGN/cache-6-0123456789abcdef"
  check "foreign-owned volume file refuses (exit)" "1" \
    "$("$BINARY" --cache-dir "$FOREIGN" --socket "$RUN_DIR/z.sock" \
        >"$TMP/foreign.log" 2>&1; echo $?)"
  check "foreign-owned log says 'cannot own'" "1" \
    "$(grep -c 'cannot own' "$TMP/foreign.log")"

  # A foreign-owned file the daemon never touches must NOT refuse: the cache
  # directory is group-writable by design, so a directory-wide rule would let
  # any group member (or a filesystem's own lost+found) deny service.
  BYSTANDER="$TMP/bystander/v2"
  mkdir -p "$BYSTANDER/lost+found"
  touch "$BYSTANDER/someone-elses-file"
  chown 1:1 "$BYSTANDER/someone-elses-file"
  "$BINARY" --cache-dir "$BYSTANDER" --socket "$RUN_DIR/b.sock" \
    --cache-size 16777216 --log-level warning >"$TMP/bystander.log" 2>&1 &
  bystander_pid=$!
  # Same start budget as the happy path: 120 s at 0.1 s polling.
  for _ in $(seq 1 1200); do
    [[ -S "$RUN_DIR/b.sock" ]] && break
    kill -0 "$bystander_pid" 2>/dev/null || break
    sleep 0.1
  done
  check "foreign-owned bystander does not refuse" "1" \
    "$([[ -S "$RUN_DIR/b.sock" ]] && echo 1 || echo 0)"
  stop_pid "$bystander_pid"
else
  echo "skip: foreign-owned legs need root (to chown the fixtures)"
fi

if [[ "$fails" -gt 0 ]]; then
  echo "--- daemon output (last 50 lines of $TMP/worker.log) ---" >&2
  tail -n 50 "$TMP/worker.log" >&2 || true
  echo "smoke: $fails FAILURE(S)" >&2; exit 1
fi
echo "smoke: all checks passed"
# Explicit: the status of a passing run must not depend on whatever the last
# statement above happened to return.
exit 0
