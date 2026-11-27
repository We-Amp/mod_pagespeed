#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2024-2026 We-Amp B.V.
#
# The release lanes' CI gate: a green run of ci.yml for the release commit
# must exist before a release lane builds or publishes. Accepted run: a
# ci.yml run on main with head_sha = RELEASE_SHA, event push or
# workflow_dispatch, conclusion success, in which every job of the list in
# REQUIRED below is success and no other job is failure, cancelled or
# timed_out (the arm64 daemon privilege rig is the one job allowed to be red;
# it is continue-on-error in pipeline-full.yml). REQUIRED holds every job of
# the full tier but its bookkeeping jobs, so a release needs the full tier
# green. A full=true dispatch satisfies this. A push run does not: it skips
# full / Lint (whole tree), by design.
#
#   * A REQUIRED job that is skipped refuses, like a red one: only success
#     counts. With the repository variable FLEET_ENABLED set to 'false' the
#     fleet-only jobs (the daemon privilege rig legs, HTTP Compliance,
#     Async-CSS Probe) skip, so the gate refuses every release until the
#     fleet is back (fail closed).
#
#   * Every API call is retried on a transient failure (HTTP 5xx, timeouts,
#     connection errors) with backoff 10/20/40/80 s; a 4xx is not retried.
#     When the retries run out the gate refuses (fail closed).
#   * A run still queued or in progress is waited for, up to MAX_WAIT_SECONDS.
#   * Two consecutive checks that find completed runs and no passing one
#     refuse; an empty page or an in-flight run resets that streak.
#   * On DRY_RUN=true a refusal is a ::warning:: and the exit code is 0.
#
# Exit: 0 "ci gate passed in ci.yml run <id> (<event>)";
#       1 "ci gate refuses this release: <reason>".
#
# Environment:
#   GITHUB_REPOSITORY, RELEASE_SHA   required
#   DRY_RUN                          "true" turns a refusal into a warning
#   MAX_WAIT_SECONDS                 default 3600
#   POLL_SECONDS                     default 60
#   RETRY_BASE_SECONDS               default 10 (doubles per try, 5 tries)
#   GH_TOKEN                         read-only token (set by the workflow)
#   CI_RELEASE_GATE_PRINT_REQUIRED   "1" prints REQUIRED and exits 0 (tests)
#
# Needs gh.
set -uo pipefail

# Every PR-tier job the release relies on and every job of the full tier
# (pipeline-full.yml) but its bookkeeping jobs (event guard, fleet-off
# notice) and the continue-on-error one, under their caller prefixes; a
# matrix job is one entry per leg. The PR-tier fuzz smokes skip outside pull
# requests by design and stay out. tools/ci/test_ci_release_gate.sh compares
# this list with the workflows.
REQUIRED=(
  'pr / Linux Build'
  'pr / Linux Unit Tests'
  'pr / Windows Build'
  'pr / Windows Unit Tests'
  'pr / Lint (clang-tidy + clang-format)'
  'full / Vendor Dependencies'
  'full / Linux Build'
  'full / Linux ASan + UBSan'
  'full / Linux TSan'
  'full / Daemon Privilege Rig (debian12)'
  'full / Daemon Privilege Rig (ubuntu2404)'
  'full / Daemon Privilege Rig (alma9)'
  'full / Linux Pkg Smoke'
  'full / HTTP Compliance'
  'full / Async-CSS Probe'
  'full / Container Cache Sharing'
  'full / Windows Build'
  'full / Windows Pkg Smoke'
  'full / Linux arm64 Build'
  'full / Linux arm64 Unit Tests'
  'full / Linux arm64 Pkg Smoke'
  'full / Lint (whole tree)'
)
# Job-level continue-on-error in pipeline-full.yml: may be red without refusing.
COE='full / Daemon Privilege Rig (arm64)'

if [ "${CI_RELEASE_GATE_PRINT_REQUIRED:-}" = 1 ]; then
  printf '%s\n' "${REQUIRED[@]}"
  exit 0
fi

: "${GITHUB_REPOSITORY:?}"
DRY_RUN="${DRY_RUN:-false}"
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

# gh_api <path> <jq>: prints the jq result; 0 on success, 2 after the retries.
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

RUNS_PATH="repos/${GITHUB_REPOSITORY}/actions/workflows/ci.yml/runs"
RUNS_Q="head_sha=${RELEASE_SHA}&per_page=50"
EVENTS="push workflow_dispatch"

# Consecutive checks that found completed runs, none passing, nothing in flight.
REFUSE_STREAK=0
REASON=""

# One unfiltered-by-status query per event keeps the polling well inside the
# hourly request limit of github.token.
runs_url() { echo "${RUNS_PATH}?${RUNS_Q}&event=$1&branch=main"; }

# check: 0 = passed, 1 = refuse, 2 = API failure (refuse), 3 = wait.
check() {
  local ids="" inflight="" completed=0 runs rid st concl entry run ev jobs req bad
  for ev in $EVENTS; do
    runs="$(gh_api "$(runs_url "$ev")" '.workflow_runs[] | "\(.id) \(.status) \(.conclusion)"')" || { REASON="the GitHub API failed"; return 2; }
    while read -r rid st concl; do
      [ -n "$rid" ] || continue
      if [ "$st" = completed ]; then
        completed=1
        [ "$concl" = success ] && ids="${ids} ${rid}:${ev}"
      else
        inflight="${inflight} ${rid}"
      fi
    done <<<"$runs"
  done
  for entry in $ids; do
    run="${entry%%:*}"; ev="${entry#*:}"
    jobs="$(gh_api "repos/${GITHUB_REPOSITORY}/actions/runs/${run}/jobs?filter=latest&per_page=100" '.jobs[] | "\(.name)=\(.conclusion)"' --paginate)" || { REASON="the GitHub API failed"; return 2; }
    bad=""
    for req in "${REQUIRED[@]}"; do
      printf '%s\n' "$jobs" | grep -Fxq -- "${req}=success" || bad="${bad}${bad:+; }${req} is not success"
    done
    while IFS= read -r line; do
      case "$line" in
        "${COE}="*) continue ;;
        *=failure|*=cancelled|*=timed_out) bad="${bad}${bad:+; }${line%%=*} is ${line##*=}" ;;
      esac
    done <<<"$jobs"
    echo "ci.yml run ${run} (${ev}): ${bad:-all required jobs success}"
    if [ -z "$bad" ]; then
      echo "ci gate passed in ci.yml run ${run} (${ev})"
      return 0
    fi
    REASON="ci.yml run ${run}: ${bad}"
  done
  if [ -n "$inflight" ]; then
    REFUSE_STREAK=0
    echo "ci.yml run(s) of ${RELEASE_SHA} still in flight:${inflight}"
    return 3
  fi
  if [ "$completed" -eq 1 ]; then
    REFUSE_STREAK=$((REFUSE_STREAK + 1))
    [ -n "$REASON" ] || REASON="no successful ci.yml run of ${RELEASE_SHA} on main"
    echo "no ci.yml run of ${RELEASE_SHA} passed the gate"
    if [ "$REFUSE_STREAK" -ge 2 ]; then
      return 1
    fi
    echo "one more check before refusing"
    return 3
  fi
  REFUSE_STREAK=0
  REASON="no ci.yml run for ${RELEASE_SHA} on main"
  echo "no ci.yml run for ${RELEASE_SHA} yet"
  return 3
}

start=$SECONDS
while :; do
  rc=0
  check || rc=$?
  [ "$rc" -eq 3 ] || break
  if [ $((SECONDS - start + POLL_SECONDS)) -gt "$MAX_WAIT_SECONDS" ]; then
    echo "gave up waiting for a passing ci.yml run of ${RELEASE_SHA} after $((SECONDS - start))s"
    rc=1
    break
  fi
  echo "waiting ${POLL_SECONDS}s for the CI run of the release commit"
  sleep "$POLL_SECONDS"
done

[ "$rc" -eq 0 ] && exit 0
if [ "$DRY_RUN" = true ]; then
  echo "::warning::ci gate refuses this release: ${REASON} (dry run, not enforced)"
  exit 0
fi
echo "::error::ci gate refuses this release: ${REASON}"
exit 1
