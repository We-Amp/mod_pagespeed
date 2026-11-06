#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2024-2026 We-Amp B.V.
#
# Test for tools/ci/rehearsal_fulltier_gate.sh: a fake `gh` on PATH answering
# from a per-case scenario (environment variables). No network, no token.
#
# Run: tools/ci/test_rehearsal_fulltier_gate.sh
#      tools/ci/rehearsal_fulltier_gate.sh --self-test

set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
GATE="${HERE}/rehearsal_fulltier_gate.sh"

WORK="$(mktemp -d)"
trap 'rm -rf "${WORK}"' EXIT
BIN="${WORK}/bin"
mkdir -p "${BIN}"

# Fake gh. Every call is logged to FAKE_LOG. Answers:
#   api .../workflows/ci.yml/runs...      FAKE_RUNS_CI (HTTP 500 while
#                                          FAKE_RUNS_CI_FAIL is a positive count)
#   api .../workflows/nightly.yml/runs... FAKE_RUNS_NIGHTLY
#   api [--paginate] .../runs/<id>/jobs.. FAKE_JOBS_<id> (HTTP 500 when
#                                          FAKE_JOBS_FAIL_<id>=1)
# FAKE_RUNS_CI_FAIL counts down in the file FAKE_COUNTER, so a transient
# failure can be followed by an answer.
cat > "${BIN}/gh" <<'GH'
#!/usr/bin/env bash
echo "gh $*" >> "${FAKE_LOG}"
path=""
for a in "$@"; do case "${a}" in repos/*) path="${a}" ;; esac; done
case "${path}" in
  */actions/runs/*/jobs*)
    id="${path#*/actions/runs/}"; id="${id%%/*}"
    fail="FAKE_JOBS_FAIL_${id}"
    if [ "${!fail-0}" = "1" ]; then echo "HTTP 404" >&2; exit 1; fi
    var="FAKE_JOBS_${id}"
    printf '%s' "${!var-}"
    ;;
  */actions/workflows/ci.yml/runs*)
    n="$(cat "${FAKE_COUNTER}" 2> /dev/null || echo 0)"
    if [ "${n}" -lt "${FAKE_RUNS_CI_FAIL:-0}" ]; then
      echo $((n + 1)) > "${FAKE_COUNTER}"
      echo "HTTP 502 Bad Gateway" >&2; exit 1
    fi
    if [ "${FAKE_RUNS_CI_HARD:-0}" = "1" ]; then echo "HTTP 403" >&2; exit 1; fi
    printf '%s' "${FAKE_RUNS_CI-}"
    ;;
  */actions/workflows/nightly.yml/runs*) printf '%s' "${FAKE_RUNS_NIGHTLY-}" ;;
  *) echo "unexpected api call: $*" >&2; exit 1 ;;
esac
GH
chmod +x "${BIN}/gh"

FAILS=0
pass() { echo "PASS: $*"; }
fail() { echo "FAIL: $*"; FAILS=$((FAILS + 1)); }

REQUIRED="$(CI_RELEASE_GATE_PRINT_REQUIRED=1 bash "${HERE}/ci_release_gate.sh" | grep '^full / ')"
[ -n "${REQUIRED}" ] && pass "the release gate lists full-tier jobs" || fail "the release gate lists no full-tier job"
printf '%s\n' "${REQUIRED}" | grep -Fxq 'full / Windows ASan' \
  && pass "the ASan job is in the list the gate reads" || fail "the ASan job moved: update the gate"
printf '%s\n' "${REQUIRED}" | grep -Fxq 'full / Windows AppVerif' \
  && pass "the IIS job is in the list the gate reads" || fail "the IIS job moved: update the gate"

SHA_A="aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
SHA_B="bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb"
NOW=1793836800 # 2026-11-05T00:00:00Z
SINCE="2026-10-29T00:00:00Z"

# runs <id:event:created>...  ->  a runs page (the sha follows the id)
runs() {
  local out="" item id ev cr sha
  for item in "$@"; do
    IFS=: read -r id ev cr sha <<< "${item}"
    # created carries colons: rejoin
    cr="2026-11-0${cr}T04:17:00Z"
    sha="${sha:-${SHA_A}}"
    out="${out}${out:+,}{\"id\":${id},\"event\":\"${ev}\",\"created_at\":\"${cr}\",\"head_sha\":\"${sha}\"}"
  done
  printf '{"total_count":%s,"workflow_runs":[%s]}' "$#" "${out}"
}

# jobs [name=conclusion]... [asan=<conclusion>]  ->  a jobs page: every
# required job success, the pr / jobs success, the ASan step success, with the
# given overrides.
jobs() {
  local over="" asan=success item
  for item in "$@"; do
    case "${item}" in
      asan=*) asan="${item#asan=}" ;;
      *) over="${over}${item}"$'\n' ;;
    esac
  done
  { printf '%s\n' "${REQUIRED}"; echo 'pr / Linux Build'; echo 'nightly / Fuzz'; } | jq -R -s -c \
    --arg over "${over}" --arg asan "${asan}" '
      ($over | split("\n") | map(select(length > 0) | {key: (.[: index("=")]), value: (.[index("=") + 1:])}) | from_entries) as $o
      | { jobs: [ split("\n")[] | select(length > 0) | . as $n
          | { name: $n, conclusion: ($o[$n] // "success"),
              steps: (if $n == "full / Windows ASan"
                      then [ {name: "Run the unit tests under ASan", conclusion: $asan} ] else [] end) } ] }'
}

# case <name> <want exit> <want output substring> -- run the gate with the
# scenario in the environment.
run_case() {
  local name="$1" want="$2" needle="$3" rc=0 out
  : > "${WORK}/log"
  rm -f "${WORK}/counter"
  out="$(PATH="${BIN}:${PATH}" FAKE_LOG="${WORK}/log" FAKE_COUNTER="${WORK}/counter" \
    GITHUB_REPOSITORY=o/r NOW_EPOCH="${NOW}" RETRY_BASE_SECONDS=0 bash "${GATE}" 2>&1)" || rc=$?
  if [ "${rc}" = "${want}" ] && printf '%s' "${out}" | grep -Fq -- "${needle}"; then
    pass "${name}"
  else
    fail "${name} (exit ${rc}, wanted ${want}; wanted output '${needle}')"
    printf '%s\n' "${out}" | sed 's/^/    | /'
  fi
}

export FAKE_RUNS_CI="" FAKE_RUNS_NIGHTLY="" FAKE_RUNS_CI_FAIL=0 FAKE_RUNS_CI_HARD=0

# A green full=true dispatch passes.
FAKE_RUNS_CI="$(runs 101:workflow_dispatch:3:)" FAKE_RUNS_NIGHTLY="$(runs)"
export FAKE_RUNS_CI FAKE_RUNS_NIGHTLY
FAKE_JOBS_101="$(jobs)"; export FAKE_JOBS_101
run_case "a green ci.yml dispatch passes" 0 "gate passed: ci.yml run 101 (workflow_dispatch) at ${SHA_A}"
# The query names the window, master, completed runs and the dispatch event; a
# push run is never asked for.
grep -Fq "created=%3E%3D${SINCE}" "${WORK}/log" && pass "the window starts 7 days before now" || fail "no window in the query: $(cat "${WORK}/log")"
grep -Fq 'branch=master' "${WORK}/log" && pass "only master is asked for" || fail "master not in the query"
grep -Fq 'event=workflow_dispatch' "${WORK}/log" && pass "ci.yml is asked for its dispatches" || fail "no dispatch filter on ci.yml"
grep -Fq 'event=push' "${WORK}/log" && fail "a push run was asked for" || pass "a push run is never asked for"

# A green scheduled nightly passes, whatever the other nightly jobs did.
FAKE_RUNS_CI="$(runs)" FAKE_RUNS_NIGHTLY="$(runs 201:schedule:4:${SHA_B})"
FAKE_JOBS_201="$(jobs 'nightly / Fuzz=failure')"; export FAKE_RUNS_CI FAKE_RUNS_NIGHTLY FAKE_JOBS_201
run_case "a green full-tier nightly passes with a red nightly job" 0 "nightly.yml run 201 (schedule) at ${SHA_B}"

# The newest run is red, an older one is green: the older one counts.
FAKE_RUNS_CI="$(runs 102:workflow_dispatch:5: 101:workflow_dispatch:2:)" FAKE_RUNS_NIGHTLY="$(runs)"
FAKE_JOBS_102="$(jobs 'full / Linux arm64 Build=failure')"; export FAKE_RUNS_CI FAKE_RUNS_NIGHTLY FAKE_JOBS_102
run_case "a red newest run does not hide an older green one" 0 "ci.yml run 101"

# No run at all.
FAKE_RUNS_CI="$(runs)" FAKE_RUNS_NIGHTLY="$(runs)"; export FAKE_RUNS_CI FAKE_RUNS_NIGHTLY
run_case "no run in the window fails" 1 "no completed ci.yml full=true dispatch or nightly run"

# Only red runs.
FAKE_RUNS_CI="$(runs 102:workflow_dispatch:5:)" FAKE_RUNS_NIGHTLY="$(runs 201:schedule:4:)"
FAKE_JOBS_102="$(jobs 'full / Linux arm64 Build=failure')"
FAKE_JOBS_201="$(jobs 'full / Linux TSan=cancelled')"
export FAKE_RUNS_CI FAKE_RUNS_NIGHTLY FAKE_JOBS_102 FAKE_JOBS_201
run_case "only red runs fail" 1 "no green full-tier run on master since ${SINCE}"
run_case "the failing job is named" 1 "full / Linux arm64 Build is failure"

# A skipped full-tier job is not green.
FAKE_RUNS_CI="$(runs 103:workflow_dispatch:5:)" FAKE_RUNS_NIGHTLY="$(runs)"
FAKE_JOBS_103="$(jobs 'full / Linux arm64 Build=skipped')"; export FAKE_RUNS_CI FAKE_RUNS_NIGHTLY FAKE_JOBS_103
run_case "a skipped full-tier job fails" 1 "full / Linux arm64 Build is not success"

# The IIS exception: skipped is accepted only while FLEET_IIS_ENABLED is not true.
FAKE_JOBS_103="$(jobs 'full / Windows AppVerif=skipped')"; export FAKE_JOBS_103
run_case "a skipped IIS job passes while IIS is off" 0 "ci.yml run 103"
FLEET_IIS_ENABLED=true run_case "a skipped IIS job fails while IIS is on" 1 "full / Windows AppVerif is not success"

# ASan: success with the runtime step not success is red.
FAKE_JOBS_103="$(jobs asan=skipped)"; export FAKE_JOBS_103
run_case "ASan without its runtime step fails" 1 "step Run the unit tests under ASan is not success"

# A required job missing from the list.
FAKE_JOBS_103="$(jobs | jq -c '.jobs |= map(select(.name != "full / Linux TSan"))')"; export FAKE_JOBS_103
run_case "a missing required job fails" 1 "full / Linux TSan is not success"

# Failed or unusable lookups never pass.
FAKE_JOBS_103="$(jobs)"; export FAKE_JOBS_103
FAKE_RUNS_CI_HARD=1 run_case "a failed runs call fails" 1 "the runs API call for ci.yml failed"
FAKE_RUNS_CI_FAIL=9 run_case "a runs call that keeps failing (HTTP 502) fails" 1 "the runs API call for ci.yml failed"
FAKE_RUNS_CI_FAIL=2 run_case "a transient runs failure is retried" 0 "ci.yml run 103"
FAKE_JOBS_FAIL_103=1 run_case "a failed jobs call fails" 1 "the jobs API call for run 103 failed"
FAKE_RUNS_CI="not json" run_case "an unusable runs answer fails" 1 "the runs API answer for ci.yml is unusable"
FAKE_RUNS_CI="$(runs 103:workflow_dispatch:5:)" FAKE_JOBS_103="not json" run_case "an unusable jobs answer fails" 1 "the jobs API answer for run 103 is unusable"
FAKE_JOBS_103='{"jobs":[]}' run_case "an empty job list fails" 1 "lists no job"
FAKE_JOBS_103="" run_case "an empty jobs answer fails" 1 "lists no job"
# The failed lookup of an older run does not let a newer green one pass later:
# the gate stops at the first failed call.
FAKE_RUNS_CI="$(runs 104:workflow_dispatch:5: 103:workflow_dispatch:4:)" FAKE_JOBS_104="$(jobs 'full / Linux TSan=failure')" FAKE_JOBS_FAIL_104=1 \
  run_case "a failed lookup stops the search" 1 "the jobs API call for run 104 failed"

# Bad input.
GITHUB_REPOSITORY="" PATH="${BIN}:${PATH}" bash "${GATE}" > /dev/null 2>&1 && fail "no repository passed" || pass "no repository fails"

if [ "${FAILS}" -ne 0 ]; then
  echo "${FAILS} case(s) failed"
  exit 1
fi
echo "rehearsal_fulltier_gate self-test: all passed"
