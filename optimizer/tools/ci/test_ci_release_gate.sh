#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2024-2026 We-Amp B.V.
#
# Test for tools/ci/ci_release_gate.sh with a fake gh. The scenario selects
# what the fake serves; a counter file lets a call be empty, fail or report
# "in flight" the first N times. The gate's REQUIRED list is compared with
# the two tier workflows (required-list-matches-workflows). No network, no
# token.
#
# Run: tools/ci/test_ci_release_gate.sh
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
GATE="${HERE}/ci_release_gate.sh"
WF="${HERE}/../../.github/workflows"
TMP="$(mktemp -d)"
trap 'rm -rf "${TMP}"' EXIT
mkdir -p "${TMP}/bin"

FAILS=0
pass() { echo "PASS: $*"; }
fail() { echo "FAIL: $*"; FAILS=$((FAILS + 1)); }

cat > "${TMP}/bin/gh" <<'GH'
#!/usr/bin/env bash
# Fake gh api. Case table (SCENARIO -> what is served):
#   dispatch-full-green       dispatch run 21, every required job success
#   push-pr-skipped           push run 22, green, the five pr / jobs skipped
#   required-skipped          dispatch run 21, full / Lint (whole tree) skipped
#   rig-leg-skipped           dispatch run 21, full / Daemon Privilege Rig (alma9) skipped
#   http-skipped              dispatch run 21, full / HTTP Compliance skipped
#   fleet-off                 dispatch run 21, every fleet-only full / job skipped
#   dispatch-pr-only          dispatch run 21, only the pr / jobs (no full=true)
#   coe-red                   dispatch run 21, arm64 rig failure, rest green
#   other-red                 dispatch run 21 (success), pr / JS/TS Unit Tests failure
#   flaky-empty-then-green    first dispatch query empty, then run 21
#   inflight-then-green       dispatch run 21 in_progress, then completed
#   api-500                   every call fails with HTTP 500
set -uo pipefail
path="$2"
echo "$path" >> "${FAKE_LOG}"
bump() { local n; n=$(cat "${FAKE_STATE}/$1" 2>/dev/null || echo 0); echo $((n + 1)) > "${FAKE_STATE}/$1"; echo "$n"; }
pr='pr / Linux Build
pr / Linux Unit Tests
pr / Windows Build
pr / Windows Unit Tests
pr / Lint (clang-tidy + clang-format)'
full='full / Vendor Dependencies
full / Linux Build
full / Linux ASan + UBSan
full / Linux TSan
full / Daemon Privilege Rig (debian12)
full / Daemon Privilege Rig (ubuntu2404)
full / Daemon Privilege Rig (alma9)
full / Linux Pkg Smoke
full / HTTP Compliance
full / Async-CSS Probe
full / Container Cache Sharing
full / Windows Build
full / Windows Pkg Smoke
full / Linux arm64 Build
full / Linux arm64 Unit Tests
full / Linux arm64 Pkg Smoke
full / Lint (whole tree)'
all_with() { # all_with <conclusion for pr jobs>
  while IFS= read -r j; do echo "${j}=$1"; done <<<"$pr"
  while IFS= read -r j; do echo "${j}=success"; done <<<"$full"
}
ev=push
case "$path" in *event=workflow_dispatch*) ev=dispatch ;; esac
case "$path" in
  */workflows/ci.yml/runs*)
    n=$(bump "q_${ev}")
    case "$SCENARIO:$ev" in
      api-500:*) echo "gh: Internal Server Error (HTTP 500)" >&2; exit 1 ;;
      push-pr-skipped:push) echo "22 completed success" ;;
      flaky-empty-then-green:dispatch)
        if [ "$n" -lt 1 ]; then : ; else echo "21 completed success"; fi ;;
      inflight-then-green:dispatch)
        if [ "$n" -lt 2 ]; then echo "21 in_progress null"; else echo "21 completed success"; fi ;;
      dispatch-full-green:dispatch|required-skipped:dispatch|rig-leg-skipped:dispatch|http-skipped:dispatch|fleet-off:dispatch|dispatch-pr-only:dispatch|coe-red:dispatch|other-red:dispatch) echo "21 completed success" ;;
      *) : ;;
    esac ;;
  */actions/runs/21/jobs*)
    case "$SCENARIO" in
      required-skipped) all_with success | sed 's#^full / Lint (whole tree)=success#full / Lint (whole tree)=skipped#' ;;
      rig-leg-skipped) all_with success | sed 's#^full / Daemon Privilege Rig (alma9)=success#full / Daemon Privilege Rig (alma9)=skipped#' ;;
      http-skipped) all_with success | sed 's#^full / HTTP Compliance=success#full / HTTP Compliance=skipped#' ;;
      fleet-off)
        all_with success | sed -E 's#^(full / (Daemon Privilege Rig \((debian12|ubuntu2404|alma9)\)|HTTP Compliance|Async-CSS Probe))=success#\1=skipped#'
        echo 'full / Shared build runners are switched off=success' ;;
      dispatch-pr-only) while IFS= read -r j; do echo "${j}=success"; done <<<"$pr"; echo 'pr / JS/TS Unit Tests=success' ;;
      coe-red) all_with success; echo 'full / Daemon Privilege Rig (arm64)=failure' ;;
      other-red) all_with success; echo 'pr / JS/TS Unit Tests=failure' ;;
      *) all_with success; echo 'pr / JS/TS Unit Tests=success'; echo 'full / Daemon Privilege Rig (arm64)=success' ;;
    esac ;;
  */actions/runs/22/jobs*) all_with skipped ;;
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

# tier_jobs <workflow>: one "<job id>US<name>US<runs-on>US<coe>US<matrix list>"
# line per job, US being the ASCII unit separator (runs-on holds "||"). Assumes the shapes the tier workflows use today: name:,
# runs-on: and continue-on-error: are single-line job keys, and a matrix axis
# is a one-line flow list ("        distro: [a, b, c]").
tier_jobs() {
  awk -v q="'" -v us=$'\037' '
    function emit() { if (id != "") print id us name us ro us coe us ml }
    /^jobs:/ { in_jobs = 1; next }
    in_jobs && /^[^ #]/ { in_jobs = 0 }
    !in_jobs { next }
    /^  [A-Za-z0-9_-]+:[ ]*$/ {
      emit(); id = $1; sub(/:$/, "", id); name = ""; ro = ""; coe = ""; ml = ""; next
    }
    /^    name: / { name = $0; sub(/^    name: /, "", name); gsub("^[\"" q "]|[\"" q "]$", "", name); next }
    /^    runs-on: / { ro = $0; sub(/^    runs-on: /, "", ro); next }
    /^    continue-on-error: true/ { coe = "coe"; next }
    /^        [A-Za-z0-9_-]+: \[.*\][ ]*$/ { ml = $0; sub(/^[^[]*\[/, "", ml); sub(/\].*$/, "", ml); gsub(/[ ]/, "", ml); next }
    END { emit() }
  ' "$1"
}

# The jobs the gate must require: every full-tier job but the bookkeeping ones
# (event guard, fleet-off notice) and the continue-on-error one, with a matrix
# job expanded to one name per leg; the five pr / jobs must each still be a
# fleet job of the PR tier (the PR-tier fuzz smokes skip outside pull requests
# and stay out of the list).
tier_jobs "${WF}/pipeline-full.yml" | while IFS=$'\037' read -r id name ro coe ml; do
  case "$id" in event-guard|fleet-off-notice) continue ;; esac
  [ "$coe" = coe ] && continue
  case "$name" in
    *'${{ matrix.'*'}}'*)
      for leg in ${ml//,/ }; do
        echo "full / $(printf '%s' "$name" | sed -E "s/\\$\\{\\{ matrix\\.[A-Za-z0-9_-]+ \\}\\}/${leg}/")"
      done ;;
    *) echo "full / ${name}" ;;
  esac
done | LC_ALL=C sort > "${TMP}/wf-full"
tier_jobs "${WF}/pipeline-pr.yml" | while IFS=$'\037' read -r id name ro coe ml; do
  case "$ro" in *fleet-*) echo "pr / ${name}" ;; esac
done | LC_ALL=C sort > "${TMP}/wf-pr-fleet"

# required-list-matches-workflows
gate_list="$(CI_RELEASE_GATE_PRINT_REQUIRED=1 bash "${GATE}" 2>/dev/null || true)"
gate_full="$(printf '%s\n' "$gate_list" | grep '^full / ' | LC_ALL=C sort || true)"
gate_pr="$(printf '%s\n' "$gate_list" | grep '^pr / ' | LC_ALL=C sort || true)"
pr_missing="$(LC_ALL=C comm -23 <(printf '%s\n' "$gate_pr") "${TMP}/wf-pr-fleet")"
if [ -s "${TMP}/wf-full" ] && [ "$gate_full" = "$(cat "${TMP}/wf-full")" ] &&
   [ "$(printf '%s\n' "$gate_pr" | grep -c .)" -eq 5 ] && [ -z "$pr_missing" ] &&
   [ -z "$(printf '%s\n' "$gate_list" | grep -v -e '^full / ' -e '^pr / ' || true)" ]; then
  pass "required-list-matches-workflows"
else
  fail "required-list-matches-workflows"
  diff <(printf '%s\n' "$gate_full") "${TMP}/wf-full" || true
  [ -z "$pr_missing" ] || echo "pr / entries not in the PR tier's fleet jobs: ${pr_missing}"
fi

run_gate dispatch-full-green
if [ "$RC" -eq 0 ] && grep -q 'passed in ci.yml run 21 (workflow_dispatch)' <<<"$OUT"; then
  pass "dispatch-full-green"
else fail "dispatch-full-green (rc=$RC)"; echo "$OUT"; fi

# query-pins-main-and-ci-yml: reuses the log of the run above.
q_ok=1; q_n=0
while IFS= read -r line; do
  case "$line" in */actions/runs/*) continue ;; esac
  q_n=$((q_n + 1))
  case "$line" in */workflows/ci.yml/runs*) : ;; *) q_ok=0 ;; esac
  case "$line" in *branch=main*) : ;; *) q_ok=0 ;; esac
  case "$line" in *head_sha=*) : ;; *) q_ok=0 ;; esac
done < "${TMP}/log"
if [ "$q_ok" -eq 1 ] && [ "$q_n" -gt 0 ]; then pass "query-pins-main-and-ci-yml"; else fail "query-pins-main-and-ci-yml (n=$q_n)"; cat "${TMP}/log"; fi

run_gate push-pr-skipped
if [ "$RC" -eq 1 ] && grep -q 'refuses this release: ci.yml run 22: pr / Linux Build is not success' <<<"$OUT" && [ "$(grep -c 'waiting' <<<"$OUT")" -eq 1 ]; then
  pass "push-pr-tier-skipped-refused"
else fail "push-pr-tier-skipped-refused (rc=$RC)"; echo "$OUT"; fi

run_gate required-skipped
if [ "$RC" -eq 1 ] && grep -q 'refuses this release: ci.yml run 21: full / Lint (whole tree) is not success' <<<"$OUT"; then
  pass "required-job-skipped-refused"
else fail "required-job-skipped-refused (rc=$RC)"; echo "$OUT"; fi

run_gate rig-leg-skipped
if [ "$RC" -eq 1 ] && grep -q 'refuses this release: ci.yml run 21: full / Daemon Privilege Rig (alma9) is not success$' <<<"$OUT"; then
  pass "rig-matrix-leg-skipped-refuses"
else fail "rig-matrix-leg-skipped-refuses (rc=$RC)"; echo "$OUT"; fi

run_gate http-skipped
if [ "$RC" -eq 1 ] && grep -q 'refuses this release: ci.yml run 21: full / HTTP Compliance is not success$' <<<"$OUT"; then
  pass "http-compliance-skipped-refuses"
else fail "http-compliance-skipped-refuses (rc=$RC)"; echo "$OUT"; fi

# FLEET_ENABLED=false: the fleet-only jobs skip, so the gate refuses (fail closed).
run_gate fleet-off
fo_ok=1
for j in 'Daemon Privilege Rig (debian12)' 'Daemon Privilege Rig (ubuntu2404)' 'Daemon Privilege Rig (alma9)' 'HTTP Compliance' 'Async-CSS Probe'; do
  grep -qF "full / ${j} is not success" <<<"$OUT" || fo_ok=0
done
if [ "$RC" -eq 1 ] && [ "$fo_ok" -eq 1 ] && grep -q '::error::ci gate refuses this release' <<<"$OUT"; then
  pass "fleet-off-refuses"
else fail "fleet-off-refuses (rc=$RC)"; echo "$OUT"; fi

run_gate dispatch-pr-only
if [ "$RC" -eq 1 ] && grep -q 'refuses this release: ci.yml run 21: full / Vendor Dependencies is not success' <<<"$OUT" &&
   grep -q 'full / Lint (whole tree) is not success$' <<<"$OUT"; then
  pass "dispatch-without-full-tier-refuses"
else fail "dispatch-without-full-tier-refuses (rc=$RC)"; echo "$OUT"; fi

run_gate coe-red
if [ "$RC" -eq 0 ] && grep -q 'passed in ci.yml run 21' <<<"$OUT"; then
  pass "coe-rig-red-still-passes"
else fail "coe-rig-red-still-passes (rc=$RC)"; echo "$OUT"; fi

run_gate other-red
if [ "$RC" -eq 1 ] && grep -q 'refuses this release: ci.yml run 21: pr / JS/TS Unit Tests is failure' <<<"$OUT"; then
  pass "other-job-red-refused"
else fail "other-job-red-refused (rc=$RC)"; echo "$OUT"; fi

run_gate flaky-empty-then-green
if [ "$RC" -eq 0 ] && [ "$(grep -c 'waiting' <<<"$OUT")" -eq 1 ] && grep -q 'passed in ci.yml run 21' <<<"$OUT"; then
  pass "flaky-empty-then-green"
else fail "flaky-empty-then-green (rc=$RC)"; echo "$OUT"; fi

run_gate inflight-then-green
if [ "$RC" -eq 0 ] && grep -q 'still in flight' <<<"$OUT" && grep -q 'passed in ci.yml run 21' <<<"$OUT"; then
  pass "inflight-then-green"
else fail "inflight-then-green (rc=$RC)"; echo "$OUT"; fi

run_gate api-500
if [ "$RC" -eq 1 ] && [ "$(wc -l < "${TMP}/log")" -eq 5 ] && grep -q 'failed after 5 tries' <<<"$OUT" && grep -q 'refuses' <<<"$OUT"; then
  pass "api-500-exhausted"
else fail "api-500-exhausted (rc=$RC)"; echo "$OUT"; fi

run_gate required-skipped DRY_RUN=true
if [ "$RC" -eq 0 ] && grep -q '::warning::ci gate refuses this release: ci.yml run 21: full / Lint (whole tree) is not success' <<<"$OUT"; then
  pass "dry-run-refusal-warns"
else fail "dry-run-refusal-warns (rc=$RC)"; echo "$OUT"; fi

# The optimizer workflow passes tag_sha || github.sha; a pre-tag dry run has an
# empty tag_sha, so the fallback commit is what the script queries.
run_gate dispatch-full-green RELEASE_SHA=fallbacksha DRY_RUN=true
if [ "$RC" -eq 0 ] && grep -q 'passed in ci.yml run 21' <<<"$OUT" && grep -q 'head_sha=fallbacksha' "${TMP}/log"; then
  pass "dry-run-empty-sha-uses-fallback"
else fail "dry-run-empty-sha-uses-fallback (rc=$RC)"; echo "$OUT"; fi

run_gate dispatch-full-green RELEASE_SHA= DRY_RUN=true
if [ "$RC" -eq 0 ] && grep -q '::warning::.*RELEASE_SHA is empty' <<<"$OUT" && [ ! -s "${TMP}/log" ]; then
  pass "empty-sha-dry-run-warns"
else fail "empty-sha-dry-run-warns (rc=$RC)"; echo "$OUT"; fi

run_gate dispatch-full-green RELEASE_SHA=
if [ "$RC" -eq 1 ] && grep -q 'refuses this release: RELEASE_SHA is empty' <<<"$OUT"; then
  pass "empty-sha-real-refused"
else fail "empty-sha-real-refused (rc=$RC)"; echo "$OUT"; fi

# The jobs call reads the latest attempt, all pages.
run_gate dispatch-full-green
if grep -q 'runs/21/jobs?filter=latest&per_page=100' "${TMP}/log"; then pass "jobs-call-latest-attempt"; else fail "jobs call lacks filter=latest"; cat "${TMP}/log"; fi

[ "$FAILS" -eq 0 ] || { echo "$FAILS failure(s)"; exit 1; }
echo "all ok"
