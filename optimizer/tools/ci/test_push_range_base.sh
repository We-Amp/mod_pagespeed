#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2024-2026 We-Amp B.V.
#
# Test for tools/ci/push_range_base.sh: a fake `gh` on PATH and a temporary
# git repository with a linear history c1..c4 and one side commit.
#
# Run: tools/ci/test_push_range_base.sh

set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SCRIPT="${HERE}/push_range_base.sh"
# The cases set DEFAULT_BRANCH themselves; a value from the caller must not
# leak into the default-branch case.
unset DEFAULT_BRANCH

WORK="$(mktemp -d)"
trap 'rm -rf "${WORK}"' EXIT

BIN="${WORK}/bin"
mkdir -p "${BIN}"
# Fake gh: logs every call to FAKE_LOG. A jobs call (.../runs/<id>/jobs...)
# fails with HTTP 500 when FAKE_JOBS_FAIL=1 and otherwise prints FAKE_JOBS_<id>
# (empty when unset). Any other call fails with HTTP 500 when FAKE_GH_FAIL=1
# and otherwise prints FAKE_GH_BODY (which may be empty); both exit 0.
cat > "${BIN}/gh" <<'GH'
#!/usr/bin/env bash
echo "gh $*" >> "${FAKE_LOG}"
case "$*" in
  */actions/runs/*/jobs*)
    if [ "${FAKE_JOBS_FAIL:-0}" = "1" ]; then
      echo "HTTP 500" >&2
      exit 1
    fi
    id="$*"
    id="${id#*/actions/runs/}"
    id="${id%%/*}"
    var="FAKE_JOBS_${id}"
    printf '%s' "${!var-}"
    exit 0
    ;;
esac
if [ "${FAKE_GH_FAIL:-0}" = "1" ]; then
  echo "HTTP 500" >&2
  exit 1
fi
printf '%s' "${FAKE_GH_BODY-}"
GH
chmod +x "${BIN}/gh"

FAILS=0
pass() { echo "PASS: $*"; }
fail() { echo "FAIL: $*"; FAILS=$((FAILS + 1)); }

REPO="${WORK}/repo"
git init -q "${REPO}"
git -C "${REPO}" config user.email t@example.invalid
git -C "${REPO}" config user.name t
git -C "${REPO}" commit -q --allow-empty -m c1
C1="$(git -C "${REPO}" rev-parse HEAD)"
git -C "${REPO}" commit -q --allow-empty -m c2
C2="$(git -C "${REPO}" rev-parse HEAD)"
git -C "${REPO}" commit -q --allow-empty -m c3
C3="$(git -C "${REPO}" rev-parse HEAD)"
git -C "${REPO}" commit -q --allow-empty -m c4
C4="$(git -C "${REPO}" rev-parse HEAD)"
git -C "${REPO}" checkout -q -b side "${C1}"
git -C "${REPO}" commit -q --allow-empty -m side
SIDE="$(git -C "${REPO}" rev-parse HEAD)"
git -C "${REPO}" checkout -q -
NOT_THERE=0123456789abcdef0123456789abcdef01234567

run() { # <fake body> [env assignments...]
  local fb="$1"
  shift
  : > "${WORK}/gh.log"
  set +e
  (cd "${REPO}" && env PATH="${BIN}:${PATH}" FAKE_LOG="${WORK}/gh.log" \
    GITHUB_REPOSITORY=We-Amp/example GITHUB_SHA="${C4}" FAKE_GH_BODY="${fb}" \
    "$@" bash "${SCRIPT}" > "${WORK}/out" 2> "${WORK}/err")
  RC=$?
  set -e
}

# runs <name:sha:conclusion:created>...  ->  the API body
runs() {
  local out="" item name sha concl created
  for item in "$@"; do
    IFS=: read -r name sha concl created <<< "${item}"
    out="${out}${out:+,}{\"name\":\"${name}\",\"head_sha\":\"${sha}\",\"conclusion\":\"${concl}\",\"created_at\":\"${created}\"}"
  done
  printf '{"total_count":%d,"workflow_runs":[%s]}' "$#" "${out}"
}

# jruns <id:sha:conclusion:created>...  ->  the API body, runs with an id
jruns() {
  local out="" item id sha concl created
  for item in "$@"; do
    IFS=: read -r id sha concl created <<< "${item}"
    out="${out}${out:+,}{\"id\":${id},\"name\":\"CI\",\"head_sha\":\"${sha}\",\"conclusion\":\"${concl}\",\"created_at\":\"${created}\"}"
  done
  printf '{"total_count":%d,"workflow_runs":[%s]}' "$#" "${out}"
}

# jobs <name=conclusion>...  ->  a jobs API body
jobs() {
  local out="" item
  for item in "$@"; do
    out="${out}${out:+,}{\"name\":\"${item%=*}\",\"conclusion\":\"${item##*=}\"}"
  done
  printf '{"total_count":%d,"jobs":[%s]}' "$#" "${out}"
}

# check <name> <want stdout, may be empty> [stderr substring...]
check() {
  local name="$1" want="$2" s ok=1
  shift 2
  if [ "${RC}" -ne 0 ]; then
    fail "${name}: exit ${RC}"
    return
  fi
  if [ "$(cat "${WORK}/out")" != "${want}" ]; then
    fail "${name}: stdout '$(cat "${WORK}/out")', want '${want}'"
    return
  fi
  for s in "$@"; do
    if ! grep -qF -- "${s}" "${WORK}/err"; then
      fail "${name}: stderr lacks '${s}': $(cat "${WORK}/err")"
      ok=0
    fi
  done
  [ "${ok}" = 0 ] || pass "${name}"
}

run "$(runs "CI:${C3}:success:2026-10-25T03:00:00Z" "CI:${C2}:success:2026-10-25T02:00:00Z")"
check "newest-completed-run-is-the-base" "${C3}"
if grep -qF 'repos/We-Amp/example/actions/workflows/ci.yml/runs?event=push&branch=master&status=completed&per_page=30' "${WORK}/gh.log"; then
  pass "queried the completed push runs of master"
else
  fail "gh log: $(cat "${WORK}/gh.log")"
fi

# Order in the answer does not matter: created_at decides.
run "$(runs "CI:${C2}:success:2026-10-25T02:00:00Z" "CI:${C3}:failure:2026-10-25T03:00:00Z")"
check "failed-run-is-passed-over" "${C2}"
for c in startup_failure timed_out skipped neutral stale action_required; do
  run "$(runs "CI:${C3}:${c}:2026-10-25T03:00:00Z" "CI:${C2}:success:2026-10-25T02:00:00Z")"
  check "${c}-run-is-passed-over" "${C2}"
done
run "{\"workflow_runs\":[{\"head_sha\":\"${C3}\",\"conclusion\":null,\"created_at\":\"2026-10-25T03:00:00Z\"},{\"head_sha\":\"${C2}\",\"conclusion\":\"success\",\"created_at\":\"2026-10-25T02:00:00Z\"}]}"
check "null-conclusion-is-passed-over" "${C2}"

run "$(runs "CI:${C3}:cancelled:2026-10-25T03:00:00Z" "CI:${C2}:cancelled:2026-10-25T02:00:00Z" "CI:${C1}:success:2026-10-25T01:00:00Z")"
check "cancelled-runs-are-skipped" "${C1}" "base ${C1}"

run "$(runs "CI:${SIDE}:success:2026-10-25T03:00:00Z" "CI:${C2}:success:2026-10-25T02:00:00Z")"
check "non-ancestor-head-is-skipped" "${C2}" "skipping ${SIDE}: not an ancestor"

run "$(runs "CI:${NOT_THERE}:success:2026-10-25T03:00:00Z" "CI:${C2}:success:2026-10-25T02:00:00Z")"
check "unknown-head-is-skipped" "${C2}"

run "$(runs "CI:${C4}:success:2026-10-25T04:00:00Z" "CI:${C3}:success:2026-10-25T03:00:00Z")"
check "current-sha-is-excluded" "${C3}"

run "$(runs "CI:${C4}:success:2026-10-25T04:00:00Z")"
check "only-current-sha-gives-nothing" "" "no successful"

run "$(runs "CI:${SIDE}:success:2026-10-25T03:00:00Z" "CI:${NOT_THERE}:success:2026-10-25T02:00:00Z")"
check "no-ancestor-gives-nothing" "" "no candidate is an ancestor"

run "" FAKE_GH_FAIL=1
check "api-failure-gives-nothing" "" "API call failed"

run ""
check "empty-body-gives-nothing" "" "no successful"

run "<html>"
check "unparseable-body-gives-nothing" "" "unusable"

run '{"message":"Not Found"}'
check "error-object-gives-nothing" "" "no successful"

run "$(runs "CI:${C3}:success:2026-10-25T03:00:00Z")" DEFAULT_BRANCH=release
if grep -qF 'branch=release&' "${WORK}/gh.log"; then pass "DEFAULT_BRANCH is honoured"; else fail "DEFAULT_BRANCH: $(cat "${WORK}/gh.log")"; fi

# --require-job: the base is the newest completed (success or failure) push
# run whose job of that exact name succeeded.
JOB='Full tier scope'
GREEN="$(jobs "Lint=success" "${JOB}=success" "pr / Build=failure")"

rj() { # <fake body> [env assignments...]: run with --require-job "${JOB}"
  local fb="$1"
  shift
  : > "${WORK}/gh.log"
  set +e
  (cd "${REPO}" && env PATH="${BIN}:${PATH}" FAKE_LOG="${WORK}/gh.log" \
    GITHUB_REPOSITORY=We-Amp/example GITHUB_SHA="${C4}" FAKE_GH_BODY="${fb}" \
    "$@" bash "${SCRIPT}" --require-job "${JOB}" > "${WORK}/out" 2> "${WORK}/err")
  RC=$?
  set -e
}
rj "$(jruns "3:${C3}:success:2026-10-25T03:00:00Z" "2:${C2}:success:2026-10-25T02:00:00Z")" \
  FAKE_JOBS_3="${GREEN}" FAKE_JOBS_2="${GREEN}"
check "require-job-success-run-with-green-scope-is-base" "${C3}" "base ${C3}"
if grep -qF 'repos/We-Amp/example/actions/runs/3/jobs?filter=latest&per_page=100' "${WORK}/gh.log"; then
  pass "require-job queried the latest jobs of run 3"
else
  fail "require-job jobs call: $(cat "${WORK}/gh.log")"
fi

jobs_calls() { grep -c '/jobs?' "${WORK}/gh.log" || true; }

rj "$(jruns "3:${C3}:failure:2026-10-25T03:00:00Z" "2:${C2}:success:2026-10-25T02:00:00Z")" \
  FAKE_JOBS_3="${GREEN}" FAKE_JOBS_2="${GREEN}"
check "require-job-failed-run-with-green-scope-is-base" "${C3}"

rj "$(jruns "3:${C3}:cancelled:2026-10-25T03:00:00Z" "2:${C2}:success:2026-10-25T02:00:00Z")" \
  FAKE_JOBS_3="${GREEN}" FAKE_JOBS_2="${GREEN}"
check "require-job-cancelled-run-is-skipped" "${C2}"
if grep -qF '/runs/3/jobs' "${WORK}/gh.log"; then
  fail "require-job-cancelled-run-is-skipped: run 3's jobs were read: $(cat "${WORK}/gh.log")"
else
  pass "require-job-cancelled-run-is-skipped: no jobs call for run 3"
fi
for c in skipped startup_failure timed_out; do
  rj "$(jruns "3:${C3}:${c}:2026-10-25T03:00:00Z" "2:${C2}:success:2026-10-25T02:00:00Z")" \
    FAKE_JOBS_3="${GREEN}" FAKE_JOBS_2="${GREEN}"
  check "require-job-${c}-run-is-skipped" "${C2}"
done
rj "{\"workflow_runs\":[{\"id\":3,\"head_sha\":\"${C3}\",\"conclusion\":null,\"created_at\":\"2026-10-25T03:00:00Z\"},{\"id\":2,\"head_sha\":\"${C2}\",\"conclusion\":\"success\",\"created_at\":\"2026-10-25T02:00:00Z\"}]}" \
  FAKE_JOBS_3="${GREEN}" FAKE_JOBS_2="${GREEN}"
check "require-job-null-conclusion-is-skipped" "${C2}"

rj "$(jruns "4:${C3}:success:2026-10-25T04:00:00Z" "3:${C3}:success:2026-10-25T03:00:00Z" "2:${C2}:success:2026-10-25T02:00:00Z")" \
  FAKE_JOBS_4="$(jobs "Lint=success")" FAKE_JOBS_3="$(jobs "Lint=success" "${JOB}=skipped")" FAKE_JOBS_2="${GREEN}"
check "require-job-scope-skipped-or-absent-is-passed-over" "${C2}"

rj "$(jruns "3:${C3}:success:2026-10-25T03:00:00Z" "2:${C2}:success:2026-10-25T02:00:00Z")" \
  FAKE_JOBS_3="$(jobs "${JOB} (x64)=success" "pr / ${JOB}=success")" FAKE_JOBS_2="${GREEN}"
check "require-job-name-matches-exactly" "${C2}"

rj "$(jruns "3:${SIDE}:success:2026-10-25T03:00:00Z" "2:${C2}:success:2026-10-25T02:00:00Z")" \
  FAKE_JOBS_3="${GREEN}" FAKE_JOBS_2="${GREEN}"
check "require-job-non-ancestor-is-skipped" "${C2}" "skipping ${SIDE}: not an ancestor"
if grep -qF '/runs/3/jobs' "${WORK}/gh.log"; then
  fail "require-job-non-ancestor-is-skipped: run 3's jobs were read"
else
  pass "require-job-non-ancestor-is-skipped: no jobs call for run 3"
fi

NOSCOPE="$(jobs "Lint=success" "${JOB}=failure")"
rj "$(jruns "16:${C3}:success:2026-10-25T06:00:00Z" "15:${C2}:failure:2026-10-25T05:00:00Z" \
            "14:${C1}:success:2026-10-25T04:00:00Z" "13:${C3}:success:2026-10-25T03:00:00Z" \
            "12:${C2}:success:2026-10-25T02:00:00Z" "11:${C1}:success:2026-10-25T01:00:00Z")" \
  PUSH_RANGE_MAX_JOB_LOOKUPS=2 FAKE_JOBS_16="${NOSCOPE}" FAKE_JOBS_15="${NOSCOPE}" \
  FAKE_JOBS_14="${GREEN}" FAKE_JOBS_13="${GREEN}" FAKE_JOBS_12="${GREEN}" FAKE_JOBS_11="${GREEN}"
check "require-job-lookup-cap" "" "no run within 2 lookups"
if [ "$(jobs_calls)" = 2 ]; then
  pass "require-job-lookup-cap: exactly 2 jobs calls"
else
  fail "require-job-lookup-cap: $(jobs_calls) jobs calls: $(cat "${WORK}/gh.log")"
fi

rj "$(jruns "16:${C3}:success:2026-10-25T06:00:00Z" "15:${C2}:success:2026-10-25T05:00:00Z" \
            "14:${C1}:success:2026-10-25T04:00:00Z" "13:${C3}:success:2026-10-25T03:00:00Z" \
            "12:${C2}:success:2026-10-25T02:00:00Z" "11:${C1}:success:2026-10-25T01:00:00Z")" \
  FAKE_JOBS_11="${GREEN}"
check "require-job-default-cap-is-5" "" "no run within 5 lookups"

rj "$(jruns "3:${C3}:success:2026-10-25T03:00:00Z" "2:${C2}:success:2026-10-25T02:00:00Z")" \
  FAKE_JOBS_FAIL=1 FAKE_JOBS_3="${GREEN}" FAKE_JOBS_2="${GREEN}"
check "require-job-jobs-api-failure-gives-nothing" "" "jobs API call"

rj "$(jruns "3:${C3}:success:2026-10-25T03:00:00Z" "2:${C2}:success:2026-10-25T02:00:00Z")" \
  FAKE_JOBS_3="<html>" FAKE_JOBS_2="${GREEN}"
check "require-job-unusable-jobs-answer-gives-nothing" "" "unusable"

rj "$(jruns "4:${C4}:success:2026-10-25T04:00:00Z")" FAKE_JOBS_4="${GREEN}"
check "require-job-only-current-sha-gives-nothing" "" "no completed push run"

rj "" FAKE_GH_FAIL=1
check "require-job-runs-api-failure-gives-nothing" "" "API call failed"

: > "${WORK}/gh.log"
set +e
(cd "${REPO}" && env PATH="${BIN}:${PATH}" FAKE_LOG="${WORK}/gh.log" \
  GITHUB_REPOSITORY=We-Amp/example GITHUB_SHA="${C4}" bash "${SCRIPT}" --require-job > "${WORK}/out" 2> "${WORK}/err")
RC=$?
set -e
check "require-job-without-a-name-gives-nothing" "" "--require-job needs a job name"
if [ -s "${WORK}/gh.log" ]; then fail "require-job without a name called gh"; else pass "require-job without a name calls no API"; fi

# sidecar-filter-uses-require-job: where the full tier's path filter uses
# this script (the sidecar pair), it keys its push range on its own
# change-detection job, never on any green run. A repository whose full tier
# has no such filter passes trivially.
FULL_YML="${HERE}/../../.github/workflows/pipeline-full.yml"
if ! grep -qF 'push_range_base.sh' "${FULL_YML}" 2>/dev/null; then
  pass "sidecar-filter-uses-require-job (no path filter uses the script)"
elif calls="$(grep -F 'push_range_base.sh' "${FULL_YML}")" &&
     calls="$(grep -v '^[[:space:]]*#' <<< "${calls}")" &&
     grep -vqF "base=\"\$(bash tools/ci/push_range_base.sh --require-job 'full / Detect changed paths (full tier)')\"" <<< "${calls}"; then
  fail "sidecar-filter-uses-require-job: pipeline-full.yml calls push_range_base.sh without --require-job 'full / Detect changed paths (full tier)'"
else
  pass "sidecar-filter-uses-require-job"
fi

if [ "${FAILS}" -ne 0 ]; then
  echo "${FAILS} case(s) failed"
  exit 1
fi
echo "All push range base cases passed"
