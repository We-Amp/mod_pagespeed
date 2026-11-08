#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2024-2026 We-Amp B.V.
#
# Test for tools/ci/runner_health.sh: a fake `gh` on PATH answering
# from environment variables. No network, no token.
#
# Run: tools/ci/test_runner_health.sh

set -uo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SCRIPT="${HERE}/runner_health.sh"
WORK="$(mktemp -d)"
trap 'rm -rf "${WORK}"' EXIT
BIN="${WORK}/bin"
mkdir -p "${BIN}"

# Fake gh. Every call is logged to FAKE_LOG; a --body-file's text is appended
# to FAKE_BODIES under a `==== <subcommand> <verb>` header. Answers:
#   api -X GET .../workflows/ci.yml/runs ...   FAKE_RUNS (HTTP 500 when FAKE_RUNS_FAIL=1)
#   api .../actions/runs/<id>/jobs...          FAKE_JOBS_<id>
#   api .../check-runs/<id>/annotations...     FAKE_ANN_<id> (default [])
#   issue list ...                             FAKE_ISSUES
#   issue view <n> ...                         FAKE_VIEW
#   issue create                               prints .../issues/77
#   issue edit|comment, label create           nothing, exit 0
cat > "${BIN}/gh" <<'GH'
#!/usr/bin/env bash
echo "gh $*" >> "${FAKE_LOG}"
prev=""
for a in "$@"; do
  if [ "${prev}" = "--body-file" ]; then
    { echo "==== $1 $2"; cat "${a}"; } >> "${FAKE_BODIES}"
  fi
  prev="${a}"
done
all="$*"
case "${all}" in
  "api -X GET "*"/actions/workflows/ci.yml/runs"*)
    if [ "${FAKE_RUNS_FAIL:-0}" = "1" ]; then echo "HTTP 500" >&2; exit 1; fi
    printf '%s' "${FAKE_RUNS:-{\"workflow_runs\":[]\}}" ;;
  "api "*"/actions/runs/"*"/jobs"*)
    id="${all#*/actions/runs/}"; id="${id%%/*}"; v="FAKE_JOBS_${id}"; printf '%s' "${!v}" ;;
  "api "*"/check-runs/"*"/annotations"*)
    id="${all#*/check-runs/}"; id="${id%%/*}"; v="FAKE_ANN_${id}"; printf '%s' "${!v:-[]}" ;;
  "issue list"*) printf '%s' "${FAKE_ISSUES:-[]}" ;;
  "issue view"*) printf '%s' "${FAKE_VIEW}" ;;
  "issue create"*) echo "https://example.invalid/o/r/issues/77" ;;
  "issue edit"*|"issue comment"*|"label create"*) ;;
  *) echo "unexpected gh call: $*" >&2; exit 1 ;;
esac
GH
chmod +x "${BIN}/gh"

FAILS=0
pass() { echo "PASS: $*"; }
fail() { echo "FAIL: $*"; FAILS=$((FAILS + 1)); }

run() {
  : > "${WORK}/log"
  : > "${WORK}/bodies"
  OUT="$(env PATH="${BIN}:${PATH}" FAKE_LOG="${WORK}/log" FAKE_BODIES="${WORK}/bodies" \
    GITHUB_REPOSITORY=o/r RUNNER_HEALTH_NOW_EPOCH=1793966400 "$@" "${SCRIPT}" 2>&1)"
  RC=$?
  LOG="$(cat "${WORK}/log")"
  BODIES="$(cat "${WORK}/bodies")"
}

job() { # <id> <name> <conclusion> <runner> [attempt]
  printf '{"id":%s,"name":"%s","conclusion":"%s","runner_name":"%s","run_attempt":%s,"html_url":"https://example.invalid/job/%s"}' \
    "$1" "$2" "$3" "$4" "${5:-1}" "$1"
}
RUNS='{"workflow_runs":[{"id":7,"created_at":"2026-11-06T11:00:00Z"},{"id":8,"created_at":"2026-11-06T11:30:00Z"}]}'
WARN='[{"message":"IIS System Tests: a failure of the host (iis-site-start), not of the change, on fleet-windows-01; the tests run again on another runner"}]'
RETRY_ERR='[{"message":"IIS System Tests: the retry failed for a reason of the host too (httpsys-ipv6-wedge) on fleet-windows-02"}]'
TEST_ERR='[{"message":"the IIS system tests failed"}]'
TITLE='Runner health: fleet-windows-01 (IIS System Tests)'

# Run 7: first attempt infra on 01, retry legs (hold on 01, test on 02, green).
JOBS7="{\"jobs\":[$(job 701 'pr / IIS System Tests' success fleet-windows-01),$(job 702 'pr / IIS System Tests (retry 1)' success fleet-windows-01),$(job 703 'pr / IIS System Tests (retry 2)' success fleet-windows-02),$(job 704 'pr / Linux Build' success fleet-heavy-01)]}"
# Run 8: no retry: a plain green IIS job is never read.
JOBS8="{\"jobs\":[$(job 801 'pr / IIS System Tests' success fleet-windows-02)]}"

# 1. First record: an issue is opened, no drain.
run FAKE_RUNS="${RUNS}" FAKE_JOBS_7="${JOBS7}" FAKE_JOBS_8="${JOBS8}" FAKE_ANN_701="${WARN}"
if [ "${RC}" -eq 0 ] && grep -q "gh issue create --title ${TITLE} --label runner-health" <<<"${LOG}" \
   && grep -q '<!-- runner-health-record job=701 -->' <<<"${BODIES}" && grep -q 'iis-site-start' <<<"${BODIES}" \
   && ! grep -q 'check-runs/801' <<<"${LOG}" && ! grep -q 'check-runs/70[234]' <<<"${LOG}" \
   && ! grep -q 'issue edit' <<<"${LOG}" && ! grep -q 'drain requested' <<<"${OUT}" \
   && ! grep -q 'fleet-windows-02' <<<"${BODIES}"; then
  pass "a first infrastructure failure opens the runner's issue; green retry legs are not records"
else fail "first record: rc=${RC} out=${OUT} log=${LOG}"; fi

# 2. The same job seen again (overlapping sweep): nothing new.
ISSUES="[{\"number\":41,\"title\":\"${TITLE}\"}]"
VIEW='{"title":"'"${TITLE}"'","body":"<!-- runner-health-record job=701 -->\nx","comments":[]}'
run FAKE_RUNS="${RUNS}" FAKE_JOBS_7="${JOBS7}" FAKE_JOBS_8="${JOBS8}" FAKE_ANN_701="${WARN}" \
  FAKE_ISSUES="${ISSUES}" FAKE_VIEW="${VIEW}"
if [ "${RC}" -eq 0 ] && ! grep -qE 'issue (create|comment|edit)' <<<"${LOG}"; then
  pass "a job already recorded is not counted twice"
else fail "dedupe: rc=${RC} log=${LOG}"; fi

# 3. A second failure on the same runner: record, retitle, drain warning.
JOBS8="{\"jobs\":[$(job 811 'pr / IIS System Tests' success fleet-windows-01 2),$(job 812 'pr / IIS System Tests (retry 1)' success fleet-windows-01 2),$(job 813 'pr / IIS System Tests (retry 2)' success fleet-windows-02 2),$(job 801 'pr / IIS System Tests' failure fleet-windows-01 1)]}"
run FAKE_RUNS="${RUNS}" FAKE_JOBS_7="${JOBS7}" FAKE_JOBS_8="${JOBS8}" FAKE_ANN_701="${WARN}" FAKE_ANN_811="${WARN}" \
  FAKE_ISSUES="${ISSUES}" FAKE_VIEW="${VIEW}"
if [ "${RC}" -eq 0 ] && grep -q 'gh issue comment 41' <<<"${LOG}" \
   && grep -q '<!-- runner-health-record job=811 -->' <<<"${BODIES}" \
   && grep -q "gh issue edit 41 --title \[drain\] ${TITLE}" <<<"${LOG}" \
   && grep -q '::warning::fleet-windows-01 failed .*drain requested in issue #41' <<<"${OUT}" \
   && grep -q 'Take fleet-windows-01 out of the fleet-iis pool' <<<"${BODIES}" \
   && ! grep -q 'check-runs/801' <<<"${LOG}"; then
  pass "a second infrastructure failure on the same runner requests a drain (only the retried attempt is read)"
else fail "second record: rc=${RC} out=${OUT} log=${LOG}"; fi

# 4. A retry leg that failed for the host is a record; one that failed a test is not.
JOBS7="{\"jobs\":[$(job 701 'pr / IIS System Tests' success fleet-windows-01),$(job 702 'pr / IIS System Tests (retry 1)' success fleet-windows-01),$(job 703 'pr / IIS System Tests (retry 2)' failure fleet-windows-02)]}"
JOBS8="{\"jobs\":[$(job 811 'pr / IIS System Tests' success fleet-windows-01),$(job 813 'pr / IIS System Tests (retry 2)' failure fleet-windows-03)]}"
run FAKE_RUNS="${RUNS}" FAKE_JOBS_7="${JOBS7}" FAKE_JOBS_8="${JOBS8}" FAKE_ANN_701="${WARN}" FAKE_ANN_811="${WARN}" \
  FAKE_ANN_703="${RETRY_ERR}" FAKE_ANN_813="${TEST_ERR}"
if [ "${RC}" -eq 0 ] && grep -q 'gh issue create --title Runner health: fleet-windows-02 (IIS System Tests)' <<<"${LOG}" \
   && grep -q 'httpsys-ipv6-wedge' <<<"${BODIES}" && ! grep -q 'fleet-windows-03' <<<"${LOG}"; then
  pass "a retry leg's host failure is a record against its own runner; a test failure is not"
else fail "retry legs: rc=${RC} out=${OUT} log=${LOG}"; fi

# 5. Already marked for draining: record only.
VIEW='{"title":"[drain] '"${TITLE}"'","body":"<!-- runner-health-record job=1 -->","comments":[{"body":"<!-- runner-health-record job=2 -->"}]}'
ISSUES="[{\"number\":41,\"title\":\"[drain] ${TITLE}\"}]"
JOBS7="{\"jobs\":[$(job 701 'pr / IIS System Tests' success fleet-windows-01),$(job 702 'pr / IIS System Tests (retry 1)' success fleet-windows-02)]}"
run FAKE_RUNS='{"workflow_runs":[{"id":7,"created_at":"2026-11-06T11:00:00Z"}]}' FAKE_JOBS_7="${JOBS7}" \
  FAKE_ANN_701="${WARN}" FAKE_ISSUES="${ISSUES}" FAKE_VIEW="${VIEW}"
if [ "${RC}" -eq 0 ] && grep -q 'gh issue comment 41' <<<"${LOG}" && ! grep -q 'issue edit' <<<"${LOG}" \
   && grep -q 'Infrastructure failure 3 ' <<<"${BODIES}"; then
  pass "a runner already marked for draining only gets the record"
else fail "already drained: rc=${RC} out=${OUT} log=${LOG}"; fi

# 6. No retried run: no issue call at all.
run FAKE_RUNS='{"workflow_runs":[{"id":8,"created_at":"2026-11-06T11:00:00Z"}]}' \
  FAKE_JOBS_8="{\"jobs\":[$(job 801 'pr / IIS System Tests' failure fleet-windows-01)]}"
if [ "${RC}" -eq 0 ] && ! grep -q 'issue' <<<"${LOG}" && grep -q 'no infrastructure failure' <<<"${OUT}"; then
  pass "a plain failure (a test failure) is never a record"
else fail "plain failure: rc=${RC} log=${LOG}"; fi

# 7. The lookback window is in the runs query.
if grep -q "created=>=2026-11-06T06:00:00Z" <<<"${LOG}"; then
  pass "the runs query is bounded by the lookback window"
else fail "lookback: log=${LOG}"; fi

# 8. An API failure is an error.
run FAKE_RUNS_FAIL=1
if [ "${RC}" -eq 1 ] && grep -q '::error::' <<<"${OUT}"; then pass "an API failure is an error"
else fail "api failure: rc=${RC} out=${OUT}"; fi

# 9. Dry run: reads only.
JOBS7="{\"jobs\":[$(job 701 'pr / IIS System Tests' success fleet-windows-01),$(job 702 'pr / IIS System Tests (retry 1)' success fleet-windows-02)]}"
run FAKE_RUNS='{"workflow_runs":[{"id":7,"created_at":"2026-11-06T11:00:00Z"}]}' FAKE_JOBS_7="${JOBS7}" \
  FAKE_ANN_701="${WARN}" DRY_RUN=true
if [ "${RC}" -eq 0 ] && ! grep -qE 'issue (create|edit|comment)|label create' <<<"${LOG}" \
   && grep -q 'dry run: would create issue' <<<"${OUT}"; then
  pass "a dry run writes nothing"
else fail "dry run: rc=${RC} out=${OUT} log=${LOG}"; fi

if [ "${FAILS}" -ne 0 ]; then
  echo "${FAILS} case(s) failed"
  exit 1
fi
echo "all cases passed"
