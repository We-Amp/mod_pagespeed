#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2024-2026 We-Amp B.V.
#
# Test for tools/ci/ci_release_gate.sh with a fake gh and a fake sleep. The
# scenario selects what the fake serves; a counter file lets a call be empty or
# report "in flight" the first N times. The job list the fake serves is read
# from the two tier workflows, the same set the gate's REQUIRED list must
# match. No network, no token.
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

# tier_jobs <workflow> <mode>: one "<job id>|<name>|<runs-on>" line per job.
tier_jobs() {
  awk -v q="'" '
    /^jobs:/ { in_jobs = 1; next }
    in_jobs && /^[^ #]/ { in_jobs = 0 }
    !in_jobs { next }
    /^  [A-Za-z0-9_-]+:[ ]*$/ {
      if (id != "") print id "|" name "|" ro
      id = $1; sub(/:$/, "", id); name = ""; ro = ""; next
    }
    /^    name: / { name = $0; sub(/^    name: /, "", name); gsub("^[\"" q "]|[\"" q "]$", "", name); next }
    /^    runs-on: / { ro = $0; sub(/^    runs-on: /, "", ro); next }
    END { if (id != "") print id "|" name "|" ro }
  ' "$1"
}

# The release-relevant jobs of the two tiers, as the gate must require them:
# every PR-tier job that runs on a fleet- class, every full-tier job but the
# bookkeeping ones.
{
  tier_jobs "${WF}/pipeline-pr.yml" | while IFS='|' read -r id name ro; do
    case "$ro" in *fleet-*) echo "pr / ${name}" ;; esac
  done
  tier_jobs "${WF}/pipeline-full.yml" | while IFS='|' read -r id name ro; do
    case "$id" in event-guard|changes|fleet-off-notice) continue ;; esac
    echo "full / ${name}"
  done
} | LC_ALL=C sort > "${TMP}/wf-required"

cat > "${TMP}/bin/sleep" <<'SL'
#!/usr/bin/env bash
echo "sleep $*" >> "${FAKE_LOG}.sleep"
SL

cat > "${TMP}/bin/gh" <<'GH'
#!/usr/bin/env bash
# Fake gh api. Case table (SCENARIO -> what is served for the dispatch query;
# every job of WF_REQUIRED is success unless the scenario says otherwise):
#   dispatch-full-green       run 21 completed success
#   push-only-green           nothing for dispatch; run 22 green for push
#   dispatch-pr-only          run 21, only the pr / jobs
#   tsan-skipped              run 21, full / Linux TSan skipped
#   arm64-skipped             run 21, full / Linux arm64 Build skipped
#   iis-skipped               run 21, both IIS jobs skipped
#   asan-runtime-missing      run 21, Windows ASan success, its test step skipped
#   other-red                 run 21 completed failure, pr / Guards failure
#   flaky-empty-then-green    first query empty, then run 21
#   inflight-then-green       run 21 in_progress twice, then completed
#   api-500                   every call fails with HTTP 500
set -uo pipefail
path="$2"
echo "$path" >> "${FAKE_LOG}"
bump() { local n; n=$(cat "${FAKE_STATE}/$1" 2>/dev/null || echo 0); echo $((n + 1)) > "${FAKE_STATE}/$1"; echo "$n"; }
asan_step='STEP full / Windows ASan/Run the unit tests under ASan'
jobs_of() { # jobs_of <jobs filter sed> <step conclusion>
  { cat "${WF_REQUIRED}"; echo 'pr / Guards (security gates)'; echo 'CI gate'; } |
    sed 's/$/=success/' | sed "$1"
  echo "${asan_step}=$2"
}
ev=push
case "$path" in *event=workflow_dispatch*) ev=dispatch ;; esac
case "$path" in
  */workflows/ci.yml/runs*)
    [ "$SCENARIO" = api-500 ] && { echo "gh: Internal Server Error (HTTP 500)" >&2; exit 1; }
    n=$(bump "q_${ev}")
    case "$SCENARIO:$ev" in
      push-only-green:push) echo "22 completed success" ;;
      push-only-green:dispatch) : ;;
      flaky-empty-then-green:dispatch)
        if [ "$n" -lt 1 ]; then : ; else echo "21 completed success"; fi ;;
      inflight-then-green:dispatch)
        if [ "$n" -lt 2 ]; then echo "21 in_progress null"; else echo "21 completed success"; fi ;;
      other-red:dispatch) echo "21 completed failure" ;;
      *:dispatch) echo "21 completed success" ;;
      *) : ;;
    esac ;;
  */actions/runs/21/jobs*)
    case "$SCENARIO" in
      dispatch-pr-only) jobs_of '/^full \/ /d' skipped | grep -v '^STEP' ;;
      tsan-skipped) jobs_of 's#^full / Linux TSan=success#full / Linux TSan=skipped#' success ;;
      arm64-skipped) jobs_of 's#^full / Linux arm64 Build=success#full / Linux arm64 Build=skipped#' success ;;
      iis-skipped) jobs_of 's#^pr / IIS System Tests=success#pr / IIS System Tests=skipped#;s#^full / Windows AppVerif=success#full / Windows AppVerif=skipped#' success ;;
      asan-runtime-missing) jobs_of 's/^$//' skipped ;;
      other-red) jobs_of 's#^pr / Guards (security gates)=success#pr / Guards (security gates)=failure#' success ;;
      *) jobs_of 's/^$//' success ;;
    esac ;;
  */actions/runs/22/jobs*) jobs_of 's/^$//' success ;;
  *) echo "fake gh: unexpected call: $path" >&2; exit 9 ;;
esac
GH
chmod +x "${TMP}/bin/gh" "${TMP}/bin/sleep"

# run_gate <scenario> [VAR=value ...]: sets RC and OUT, resets the fake state.
run_gate() {
  local scenario="$1"; shift
  rm -rf "${TMP}/state"; mkdir -p "${TMP}/state"; : > "${TMP}/log"; rm -f "${TMP}/log.sleep"
  RC=0
  OUT="$(env PATH="${TMP}/bin:${PATH}" SCENARIO="$scenario" FAKE_STATE="${TMP}/state" FAKE_LOG="${TMP}/log" \
    WF_REQUIRED="${TMP}/wf-required" GITHUB_REPOSITORY=o/r RELEASE_SHA=abc FLEET_IIS_ENABLED=true \
    POLL_SECONDS=0 RETRY_BASE_SECONDS=0 MAX_WAIT_SECONDS=3 \
    "$@" bash "${GATE}" 2>&1)" || RC=$?
}

# required-list-matches-workflows
gate_list="$(CI_RELEASE_GATE_PRINT_REQUIRED=1 bash "${GATE}" 2>/dev/null | LC_ALL=C sort || true)"
if [ -s "${TMP}/wf-required" ] && [ "$gate_list" = "$(cat "${TMP}/wf-required")" ]; then
  pass "required-list-matches-workflows"
else
  fail "required-list-matches-workflows"
  diff <(printf '%s\n' "$gate_list") "${TMP}/wf-required" || true
fi

run_gate dispatch-full-green
if [ "$RC" -eq 0 ] && grep -q 'ci gate passed in ci.yml run 21 (workflow_dispatch)' <<<"$OUT" && ! grep -q '::warning::' <<<"$OUT"; then
  pass "dispatch-full-green-passes"
else fail "dispatch-full-green-passes (rc=$RC)"; echo "$OUT"; fi

# The log of the run above: the runs query is pinned to ci.yml, master, the
# dispatch event and the release commit; the jobs call reads the latest
# attempt, all pages.
q_ok=1; q_n=0
while IFS= read -r line; do
  case "$line" in */actions/runs/*) continue ;; esac
  q_n=$((q_n + 1))
  case "$line" in */workflows/ci.yml/runs\?*) : ;; *) q_ok=0 ;; esac
  case "$line" in *event=workflow_dispatch*) : ;; *) q_ok=0 ;; esac
  case "$line" in *branch=master*) : ;; *) q_ok=0 ;; esac
  case "$line" in *head_sha=abc*) : ;; *) q_ok=0 ;; esac
done < "${TMP}/log"
if [ "$q_ok" -eq 1 ] && [ "$q_n" -gt 0 ] && grep -q 'runs/21/jobs?filter=latest&per_page=100' "${TMP}/log"; then
  pass "query-pins-dispatch-master-and-latest-attempt"
else fail "query-pins-dispatch-master-and-latest-attempt (n=$q_n)"; cat "${TMP}/log"; fi

run_gate push-only-green MAX_WAIT_SECONDS=0 POLL_SECONDS=1
if [ "$RC" -eq 1 ] && ! grep -q 'event=push' "${TMP}/log" && ! grep -q 'runs/22/' "${TMP}/log" \
   && grep -q 'gave up waiting' <<<"$OUT" && grep -q 'ci gate refuses this release' <<<"$OUT"; then
  pass "push-run-is-never-read"
else fail "push-run-is-never-read (rc=$RC)"; echo "$OUT"; cat "${TMP}/log"; fi

run_gate dispatch-pr-only
if [ "$RC" -eq 1 ] && grep -q 'ci gate refuses this release: ci.yml run 21: .*full / Linux Build is not success' <<<"$OUT"; then
  pass "dispatch-without-full-tier-refuses"
else fail "dispatch-without-full-tier-refuses (rc=$RC)"; echo "$OUT"; fi

run_gate tsan-skipped
if [ "$RC" -eq 1 ] && grep -q 'ci gate refuses this release: ci.yml run 21: full / Linux TSan is not success' <<<"$OUT"; then
  pass "required-skipped-refuses"
else fail "required-skipped-refuses (rc=$RC)"; echo "$OUT"; fi

run_gate arm64-skipped FLEET_IIS_ENABLED=false
if [ "$RC" -eq 1 ] && grep -q 'ci gate refuses this release: ci.yml run 21: full / Linux arm64 Build is not success' <<<"$OUT"; then
  pass "arm64-skipped-refuses"
else fail "arm64-skipped-refuses (rc=$RC)"; echo "$OUT"; fi

run_gate iis-skipped FLEET_IIS_ENABLED=false
if [ "$RC" -eq 0 ] && grep -q 'ci gate passed in ci.yml run 21 (workflow_dispatch)' <<<"$OUT" \
   && grep -q "::warning::FLEET_IIS_ENABLED is 'false': pr / IIS System Tests skipped and accepted" <<<"$OUT" \
   && grep -q "::warning::FLEET_IIS_ENABLED is 'false': full / Windows AppVerif skipped and accepted" <<<"$OUT"; then
  pass "iis-skipped-with-iis-off-passes-with-warning"
else fail "iis-skipped-with-iis-off-passes-with-warning (rc=$RC)"; echo "$OUT"; fi

run_gate iis-skipped FLEET_IIS_ENABLED=
if [ "$RC" -eq 0 ] && grep -q "::warning::FLEET_IIS_ENABLED is '': pr / IIS System Tests skipped and accepted" <<<"$OUT"; then
  pass "iis-skipped-with-iis-unset-passes-with-warning"
else fail "iis-skipped-with-iis-unset-passes-with-warning (rc=$RC)"; echo "$OUT"; fi

run_gate iis-skipped
if [ "$RC" -eq 1 ] && grep -q 'ci gate refuses this release: ci.yml run 21: pr / IIS System Tests is not success; full / Windows AppVerif is not success' <<<"$OUT" \
   && ! grep -q 'skipped and accepted' <<<"$OUT"; then
  pass "iis-skipped-with-iis-on-refuses"
else fail "iis-skipped-with-iis-on-refuses (rc=$RC)"; echo "$OUT"; fi

run_gate asan-runtime-missing
if [ "$RC" -eq 1 ] && grep -q 'ci gate refuses this release: ci.yml run 21: full / Windows ASan step Run the unit tests under ASan is not success' <<<"$OUT"; then
  pass "win-asan-runtime-missing-refuses"
else fail "win-asan-runtime-missing-refuses (rc=$RC)"; echo "$OUT"; fi

run_gate other-red
if [ "$RC" -eq 1 ] && grep -q 'ci gate refuses this release: ci.yml run 21: .*pr / Guards (security gates) is failure' <<<"$OUT"; then
  pass "other-red-refuses"
else fail "other-red-refuses (rc=$RC)"; echo "$OUT"; fi

run_gate inflight-then-green
if [ "$RC" -eq 0 ] && grep -q 'still in flight' <<<"$OUT" && grep -q 'ci gate passed in ci.yml run 21' <<<"$OUT"; then
  pass "inflight-then-green"
else fail "inflight-then-green (rc=$RC)"; echo "$OUT"; fi

run_gate flaky-empty-then-green
if [ "$RC" -eq 0 ] && [ "$(grep -c 'waiting' <<<"$OUT")" -eq 1 ] && grep -q 'ci gate passed in ci.yml run 21' <<<"$OUT"; then
  pass "flaky-empty-then-green"
else fail "flaky-empty-then-green (rc=$RC)"; echo "$OUT"; fi

run_gate api-500
if [ "$RC" -eq 1 ] && [ "$(wc -l < "${TMP}/log")" -eq 5 ] && grep -q 'failed after 5 tries' <<<"$OUT" \
   && grep -q 'ci gate refuses this release: the GitHub API failed' <<<"$OUT"; then
  pass "api-500-refuses"
else fail "api-500-refuses (rc=$RC)"; echo "$OUT"; fi

run_gate tsan-skipped DRY_RUN=true
if [ "$RC" -eq 0 ] && grep -q '::warning::ci gate refuses this release: ci.yml run 21: full / Linux TSan is not success (dry run, not enforced)' <<<"$OUT"; then
  pass "dry-run-refusal-is-warning"
else fail "dry-run-refusal-is-warning (rc=$RC)"; echo "$OUT"; fi

run_gate dispatch-full-green RELEASE_SHA=
if [ "$RC" -eq 1 ] && grep -q 'ci gate refuses this release: RELEASE_SHA is empty' <<<"$OUT" && [ ! -s "${TMP}/log" ]; then
  pass "empty-release-sha-refuses"
else fail "empty-release-sha-refuses (rc=$RC)"; echo "$OUT"; fi

run_gate dispatch-full-green RELEASE_SHA= DRY_RUN=true
if [ "$RC" -eq 0 ] && grep -q '::warning::ci gate refuses this release: RELEASE_SHA is empty' <<<"$OUT" && [ ! -s "${TMP}/log" ]; then
  pass "empty-release-sha-dry-run-warns"
else fail "empty-release-sha-dry-run-warns (rc=$RC)"; echo "$OUT"; fi

[ "$FAILS" -eq 0 ] || { echo "$FAILS failure(s)"; exit 1; }
echo "all ok"
