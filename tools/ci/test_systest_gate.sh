#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2024-2026 We-Amp B.V.
#
# Test for tools/ci/systest_gate.sh with a fake gh. The scenario selects what
# the fake serves; a counter file lets a call fail or report "in flight" the
# first N times. No network, no token.
#
# Run: tools/ci/test_systest_gate.sh
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
GATE="${HERE}/systest_gate.sh"
TMP="$(mktemp -d)"
trap 'rm -rf "${TMP}"' EXIT
mkdir -p "${TMP}/bin"

FAILS=0
pass() { echo "PASS: $*"; }
fail() { echo "FAIL: $*"; FAILS=$((FAILS + 1)); }

cat > "${TMP}/bin/gh" <<'GH'
#!/usr/bin/env bash
# Fake gh api. SCENARIO: 502-then-ok | 502-always | inflight-then-red | inflight-then-ok | red | none | 404
set -uo pipefail
path="$2"
echo "$path" >> "${FAKE_LOG}"
bump() { local n; n=$(cat "${FAKE_STATE}/$1" 2>/dev/null || echo 0); echo $((n + 1)) > "${FAKE_STATE}/$1"; echo "$n"; }
green='Apache System Tests=success
Nginx System Tests=success'
red='Apache System Tests=success
Nginx System Tests=failure'
case "$path" in
  */workflows/ci.yml/runs*status=completed*)
    case "$SCENARIO" in
      502-then-ok)
        if [ "$(bump completed)" -lt 2 ]; then echo "gh: Bad Gateway (HTTP 502)" >&2; exit 1; fi
        echo 11 ;;
      502-always) echo "gh: Bad Gateway (HTTP 502)" >&2; exit 1 ;;
      inflight-then-red)
        if [ "$(bump completed)" -lt 2 ]; then : ; else echo 12; fi ;;
      inflight-then-ok)
        if [ "$(bump completed)" -lt 2 ]; then : ; else echo 11; fi ;;
      red) echo 12 ;;
      404) echo "gh: Not Found (HTTP 404)" >&2; exit 1 ;;
      none) : ;;
    esac ;;
  */workflows/ci.yml/runs*status=in_progress*)
    case "$SCENARIO" in
      inflight-then-ok|inflight-then-red) [ "$(bump inflight)" -lt 2 ] && echo 10 || true ;;
    esac ;;
  */workflows/ci.yml/runs*status=*) : ;;
  */actions/runs/11/jobs*) echo "$green" ;;
  */actions/runs/12/jobs*) echo "$red" ;;
  *) echo "fake gh: unexpected call: $path" >&2; exit 9 ;;
esac
GH
chmod +x "${TMP}/bin/gh"

# run_gate <scenario> [VAR=value ...]: sets RC and OUT, resets the fake state.
run_gate() {
  local scenario="$1"; shift
  rm -rf "${TMP}/state"; mkdir -p "${TMP}/state"; : > "${TMP}/log"
  RC=0
  OUT="$(env PATH="${TMP}/bin:${PATH}" SCENARIO="$scenario" FAKE_STATE="${TMP}/state" FAKE_LOG="${TMP}/log" \
    GITHUB_REPOSITORY=o/r RELEASE_SHA=abc POLL_SECONDS=0 RETRY_BASE_SECONDS=0 MAX_WAIT_SECONDS=3 \
    "$@" bash "${GATE}" 2>&1)" || RC=$?
}

run_gate 502-then-ok
if [ "$RC" -eq 0 ] && grep -q 'retrying' <<<"$OUT" && grep -q 'passed in ci.yml run 11' <<<"$OUT"; then
  pass "a 502 is retried, then the gate passes"
else fail "502-then-success (rc=$RC)"; echo "$OUT"; fi

run_gate inflight-then-ok
if [ "$RC" -eq 0 ] && grep -q 'still in flight' <<<"$OUT" && grep -q 'waiting' <<<"$OUT" && grep -q 'passed in ci.yml run 11' <<<"$OUT"; then
  pass "an in-progress run is waited for, then the gate passes"
else fail "in-progress-then-success (rc=$RC)"; echo "$OUT"; fi

run_gate red
if [ "$RC" -eq 1 ] && grep -q 'refuses this release' <<<"$OUT" && ! grep -q 'waiting' <<<"$OUT"; then
  pass "a completed red run is refused at once"
else fail "completed-red (rc=$RC)"; echo "$OUT"; fi

run_gate red DRY_RUN=true
if [ "$RC" -eq 0 ] && grep -q 'dry run' <<<"$OUT"; then
  pass "a refusal on a dry run is a warning"
else fail "completed-red dry run (rc=$RC)"; echo "$OUT"; fi

run_gate none MAX_WAIT_SECONDS=2 POLL_SECONDS=1
if [ "$RC" -eq 1 ] && grep -q 'gave up waiting' <<<"$OUT"; then
  pass "no run after the timeout is refused"
else fail "no-run-after-timeout (rc=$RC)"; echo "$OUT"; fi

run_gate 404
if [ "$RC" -eq 1 ] && [ "$(wc -l < "${TMP}/log")" -eq 1 ]; then
  pass "a 404 is not retried and is refused"
else fail "404 (rc=$RC)"; echo "$OUT"; fi

run_gate 502-always
if [ "$RC" -eq 1 ] && [ "$(wc -l < "${TMP}/log")" -eq 5 ] && grep -q 'failed after 5 tries' <<<"$OUT" && grep -q 'refuses this release' <<<"$OUT"; then
  pass "five transient failures in a row are refused"
else fail "502-always (rc=$RC)"; echo "$OUT"; fi

run_gate inflight-then-red
if [ "$RC" -eq 1 ] && grep -q 'still in flight' <<<"$OUT" && grep -q 'refuses this release' <<<"$OUT"; then
  pass "a run that finishes red while waited for is refused"
else fail "inflight-then-red (rc=$RC)"; echo "$OUT"; fi

run_gate 502-always DRY_RUN=true
if [ "$RC" -eq 0 ] && grep -q 'dry run' <<<"$OUT"; then
  pass "an API failure on a dry run is a warning"
else fail "502-always dry run (rc=$RC)"; echo "$OUT"; fi

run_gate red
if grep -q 'event=push&branch=master' "${TMP}/log"; then pass "only master push runs are queried"; else fail "runs query lacks event=push&branch=master"; fi

[ "$FAILS" -eq 0 ] || { echo "$FAILS failure(s)"; exit 1; }
echo "all ok"
