#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2024-2026 We-Amp B.V.
#
# The release lane's CI gate: a green full-tier run of ci.yml for the release
# commit must exist before the release lane publishes. tools/ci/systest_gate.sh
# stays next to it as the system-test half; this script checks the full tier.
#
# Accepted run: a ci.yml run on master with head_sha = RELEASE_SHA and event
# workflow_dispatch (the candidate dispatch with full=true), conclusion
# success, in which every job of REQUIRED below is success and no other job is
# failure, cancelled or timed_out.
#
#   * Only the candidate's full=true dispatch counts. A push run is never
#     queried: it may not carry the full tier, and a repository variable's
#     value today says nothing about an earlier run. A scheduled nightly, or a
#     re-run of one, is never evidence either. A dispatch without full=true
#     has no full / jobs and is refused by the REQUIRED list.
#   * skipped is a failure. The one allow-list: pr / IIS System Tests and
#     full / Windows AppVerif need the IIS runner class and are skipped by
#     their workflows while the repository variable FLEET_IIS_ENABLED is not
#     true. At gate time, and only then, a skipped conclusion of those two is
#     accepted and printed as a ::warning:: in the release evidence. The
#     arm64 legs have no allow-list entry: if the hosted arm64 class is
#     unavailable, the release waits or is waived explicitly.
#   * With FLEET_ENABLED=false the IIS jobs are skipped too; with
#     FLEET_IIS_ENABLED=true the gate then refuses (fail closed).
#   * No job of the two tiers is continue-on-error today. Such a job would be
#     advisory: reported, not in REQUIRED. Making one required is a separate
#     change.
#   * full / Windows ASan concludes success even when the ASan runtime is
#     missing (its test step is then skipped), so the gate also requires that
#     job's step "Run the unit tests under ASan" to be success.
#   * Every API call is retried on a transient failure (HTTP 5xx, timeouts,
#     connection errors) with backoff 10/20/40/80 s; a 4xx is not retried.
#     When the retries run out the gate refuses (fail closed).
#   * Any green full=true dispatch at the SHA suffices: the first one that
#     passes wins, and a newer red dispatch of the same SHA does not override
#     it (a red re-dispatch of one commit is a flake signal, not a
#     regression).
#   * A run still queued or in progress is waited for, up to MAX_WAIT_SECONDS,
#     so the gate can run while the candidate dispatch is still running.
#   * Two consecutive checks that find completed runs and no passing one
#     refuse; an empty page or an in-flight run resets that streak. With no
#     run at all the gate keeps polling and refuses at the deadline.
#   * On DRY_RUN=true a refusal is a ::warning:: and the exit code is 0.
#
# Exit: 0 "ci gate passed in ci.yml run <id> (workflow_dispatch)";
#       1 "ci gate refuses this release: <reason>".
#
# Environment:
#   GITHUB_REPOSITORY, RELEASE_SHA   required
#   DRY_RUN                          "true" turns a refusal into a warning
#   FLEET_IIS_ENABLED                the repository variable, passed by the
#                                    workflow (the token cannot read variables)
#   MAX_WAIT_SECONDS                 default 3600
#   POLL_SECONDS                     default 60
#   RETRY_BASE_SECONDS               default 10 (doubles per try, 5 tries)
#   GH_TOKEN                         read-only token (set by the workflow)
#   CI_RELEASE_GATE_PRINT_REQUIRED   "1" prints REQUIRED and exits 0 (tests)
#
# Needs gh.
set -uo pipefail

# Every fleet job of the PR tier (pipeline-pr.yml) and every job of the full
# tier (pipeline-full.yml) but its bookkeeping jobs (event guard, path filter,
# fleet-off notice), under their caller prefixes. tools/ci/test_ci_release_gate.sh
# compares this list with the two workflows. The path-gated PR-tier jobs (the
# two changed-port system tests) are listed too: the candidate dispatch sets
# every path filter true, so they run there, and skipped still fails.
REQUIRED=(
  'pr / Vendor Dependencies'
  'pr / Linux Build'
  'pr / Linux Unit Tests'
  'pr / Lint (clang-tidy + clang-format)'
  'pr / JS Minify Corpus'
  'pr / HTML Parse Corpus Goldens'
  'pr / Fuzz smoke (deterministic replay plus 60s libFuzzer)'
  'pr / Windows Build'
  'pr / Windows Unit Tests'
  'pr / IIS System Tests'
  'pr / Apache System Tests (changed port)'
  'pr / nginx System Tests (changed port)'
  'full / Vendor Dependencies'
  'full / Linux Build'
  'full / Linux ASan + UBSan'
  'full / Linux TSan'
  'full / Apache System Tests'
  'full / Nginx System Tests'
  'full / Windows Build'
  'full / Windows ASan'
  'full / Windows AppVerif'
  'full / Linux arm64 Build'
  'full / Linux arm64 Unit Tests'
  'full / Linux arm64 Apache System Tests'
  'full / Linux arm64 Nginx System Tests'
  'full / Sidecar pair and live tests (x64)'
  'full / Sidecar pair and live tests (arm64)'
)
# The IIS allow-list: skipped is accepted only while FLEET_IIS_ENABLED is not
# true at gate time (see the header).
IIS_JOBS=('pr / IIS System Tests' 'full / Windows AppVerif')
# The runtime check: this step of this job must be success.
ASAN_JOB='full / Windows ASan'
ASAN_STEP='Run the unit tests under ASan'

if [ "${CI_RELEASE_GATE_PRINT_REQUIRED:-}" = 1 ]; then
  printf '%s\n' "${REQUIRED[@]}"
  exit 0
fi

: "${GITHUB_REPOSITORY:?}"
DRY_RUN="${DRY_RUN:-false}"
FLEET_IIS_ENABLED="${FLEET_IIS_ENABLED:-}"
# Time budget against the 75 minute job timeout: polling stops after 60
# minutes. A check that hits a transient failure gives up on its first
# exhausted call (5 tries of at most 60 s each plus 150 s of backoff, 7.5
# minutes), so the verdict is written by about 68 minutes, with the checkout
# and setup well inside the remaining margin.
# An empty RELEASE_SHA is a refusal like any other (a warning on a dry run).
if [ -z "${RELEASE_SHA:-}" ]; then
  if [ "$DRY_RUN" = true ]; then
    echo "::warning::ci gate refuses this release: RELEASE_SHA is empty (dry run, not enforced)"
    exit 0
  fi
  echo "::error::ci gate refuses this release: RELEASE_SHA is empty"
  exit 1
fi
MAX_WAIT_SECONDS="${MAX_WAIT_SECONDS:-3600}"
POLL_SECONDS="${POLL_SECONDS:-60}"
RETRY_BASE_SECONDS="${RETRY_BASE_SECONDS:-10}"
# Each API call is bounded to 60 s (exit 124 counts as transient).
TIMEOUT=""
command -v timeout >/dev/null 2>&1 && TIMEOUT="timeout 60"
TRANSIENT_RE='HTTP 5[0-9][0-9]|Bad Gateway|Service Unavailable|Gateway Time-?out|timed? ?out|timeout|connection (reset|refused)|EOF|temporar'

# gh_api <path> <jq> [extra flag]: prints the jq result; 0 on success, 2 after
# the retries.
gh_api() {
  local path="$1" jq="$2" extra="${3:-}" try delay out err errf rc
  errf="$(mktemp)"
  delay="$RETRY_BASE_SECONDS"
  for try in 1 2 3 4 5; do
    rc=0
    out="$($TIMEOUT gh api "$path" ${extra:+"$extra"} --jq "$jq" 2>"$errf")" || rc=$?
    if [ "$rc" -eq 0 ]; then
      rm -f "$errf"; printf '%s' "$out"; return 0
    fi
    err="$(cat "$errf")"
    [ "$rc" -eq 124 ] && err="${err} timed out after 60s"
    if ! printf '%s' "$err" | grep -Eiq "$TRANSIENT_RE"; then
      echo "gh api ${path%%\?*} failed (not retried): ${err}" >&2
      rm -f "$errf"; return 2
    fi
    if [ "$try" -lt 5 ]; then
      echo "gh api ${path%%\?*} failed (try ${try}/5), retrying in ${delay}s: ${err}" >&2
      sleep "$delay"; delay=$((delay * 2))
    else
      echo "gh api ${path%%\?*} failed after 5 tries: ${err}" >&2
    fi
  done
  rm -f "$errf"; return 2
}

# Only the dispatch event on master: push runs are never read. One query per
# check, unfiltered by status, keeps the polling well inside the hourly
# request limit of github.token.
RUNS_URL="repos/${GITHUB_REPOSITORY}/actions/workflows/ci.yml/runs?head_sha=${RELEASE_SHA}&per_page=50&event=workflow_dispatch&branch=master"
# One "<job name>=<conclusion>" line per job, plus
# "STEP <job>/<step>=<conclusion>" for the runtime check step.
JOBS_JQ='.jobs[] | ("\(.name)=\(.conclusion)"), (select(.name == "'"${ASAN_JOB}"'") | .steps[]? | select(.name == "'"${ASAN_STEP}"'") | "STEP '"${ASAN_JOB}"'/\(.name)=\(.conclusion)")'

# Consecutive checks that found completed runs, none passing, nothing in flight.
REFUSE_STREAK=0
REASON=""

# iis_allowed <job>: 0 when <job> is on the IIS allow-list and IIS is off.
iis_allowed() {
  local j
  [ "$FLEET_IIS_ENABLED" = true ] && return 1
  for j in "${IIS_JOBS[@]}"; do [ "$j" = "$1" ] && return 0; done
  return 1
}

# check: 0 = passed, 1 = refuse, 2 = API failure (refuse), 3 = wait.
check() {
  local ids="" inflight="" completed=0 runs rid st concl entry run jobs req bad warn line
  runs="$(gh_api "$RUNS_URL" '.workflow_runs[] | "\(.id) \(.status) \(.conclusion)"')" || { REASON="the GitHub API failed"; return 2; }
  while read -r rid st concl; do
    [ -n "$rid" ] || continue
    if [ "$st" = completed ]; then
      completed=1
      ids="${ids} ${rid}:${concl}"
    else
      inflight="${inflight} ${rid}"
    fi
  done <<<"$runs"
  for entry in $ids; do
    run="${entry%%:*}"; concl="${entry#*:}"
    jobs="$(gh_api "repos/${GITHUB_REPOSITORY}/actions/runs/${run}/jobs?filter=latest&per_page=100" "$JOBS_JQ" --paginate)" || { REASON="the GitHub API failed"; return 2; }
    bad=""; warn=""
    [ "$concl" = success ] || bad="the run concluded ${concl}"
    for req in "${REQUIRED[@]}"; do
      printf '%s\n' "$jobs" | grep -Fxq -- "${req}=success" && continue
      if iis_allowed "$req" && printf '%s\n' "$jobs" | grep -Fxq -- "${req}=skipped"; then
        warn="${warn}::warning::FLEET_IIS_ENABLED is '${FLEET_IIS_ENABLED}': ${req} skipped and accepted"$'\n'
        continue
      fi
      bad="${bad}${bad:+; }${req} is not success"
    done
    printf '%s\n' "$jobs" | grep -Fxq -- "STEP ${ASAN_JOB}/${ASAN_STEP}=success" \
      || bad="${bad}${bad:+; }${ASAN_JOB} step ${ASAN_STEP} is not success"
    while IFS= read -r line; do
      case "$line" in
        "STEP "*) continue ;;
        *=failure|*=cancelled|*=timed_out) bad="${bad}${bad:+; }${line%%=*} is ${line##*=}" ;;
      esac
    done <<<"$jobs"
    echo "ci.yml run ${run} (workflow_dispatch): ${bad:-all required jobs success}"
    if [ -z "$bad" ]; then
      printf '%s' "$warn"
      echo "ci gate passed in ci.yml run ${run} (workflow_dispatch)"
      return 0
    fi
    REASON="ci.yml run ${run}: ${bad}"
  done
  if [ -n "$inflight" ]; then
    REFUSE_STREAK=0
    [ -n "$REASON" ] || REASON="the full=true ci.yml dispatch run of ${RELEASE_SHA} did not finish in time"
    echo "ci.yml dispatch run(s) of ${RELEASE_SHA} still in flight:${inflight}"
    return 3
  fi
  if [ "$completed" -eq 1 ]; then
    REFUSE_STREAK=$((REFUSE_STREAK + 1))
    echo "no ci.yml dispatch run of ${RELEASE_SHA} passed the gate"
    if [ "$REFUSE_STREAK" -ge 2 ]; then
      return 1
    fi
    echo "one more check before refusing"
    return 3
  fi
  REFUSE_STREAK=0
  REASON="no full=true ci.yml dispatch run for ${RELEASE_SHA} on master"
  echo "no ci.yml dispatch run for ${RELEASE_SHA} yet"
  return 3
}

start=$SECONDS
while :; do
  rc=0
  check || rc=$?
  [ "$rc" -eq 3 ] || break
  if [ $((SECONDS - start + POLL_SECONDS)) -gt "$MAX_WAIT_SECONDS" ]; then
    echo "gave up waiting for a passing ci.yml dispatch run of ${RELEASE_SHA} after $((SECONDS - start))s"
    rc=1
    break
  fi
  echo "waiting ${POLL_SECONDS}s for the full=true CI dispatch of the release commit"
  sleep "$POLL_SECONDS"
done

[ "$rc" -eq 0 ] && exit 0
if [ "$DRY_RUN" = true ]; then
  echo "::warning::ci gate refuses this release: ${REASON} (dry run, not enforced)"
  exit 0
fi
echo "::error::ci gate refuses this release: ${REASON}"
exit 1
