#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2024-2026 We-Amp B.V.
#
# The release's system-test gate: a run of ci.yml for the release commit must
# have passed both the Apache and the Nginx System Tests. This script checks
# those two lanes only; it does not assert the full tier (that is checked by
# the /release-readiness R2 step against the full=true dispatch run).
#
#   * Every API call is retried on a transient failure (HTTP 5xx, timeouts,
#     connection errors) with backoff 10/20/40/80 s; a 4xx is not retried.
#     When the retries run out the gate refuses (fail closed).
#   * Push runs and workflow_dispatch runs of ci.yml on master count (a push
#     run can be cancelled while pending; the release-candidate dispatch with
#     full=true is a run of its own).
#   * When no run of ci.yml for the commit has passed yet but one is still
#     queued or in progress, the gate polls until it finishes, up to
#     MAX_WAIT_SECONDS. The tag can therefore be pushed right after the merge
#     and the release builds overlap the CI run of the same commit.
#   * A completed run that lacks green Apache and Nginx System Tests, with no
#     run left in flight, is refused only when two consecutive checks agree
#     (one extra POLL_SECONDS before a red refusal, so a flaky empty page
#     cannot cause one). A pass always needs a run actually seen green. With
#     no run at all the gate keeps polling (the run may not exist yet) and
#     refuses at the deadline.
#   * On a dry run a refusal is reported as a warning and the exit is 0.
#
# Environment:
#   GITHUB_REPOSITORY, RELEASE_SHA   required
#   DRY_RUN                          "true" turns a refusal into a warning
#   MAX_WAIT_SECONDS                 default 3600 (see the budget below)
#   POLL_SECONDS                     default 60
#   RETRY_BASE_SECONDS               default 10 (doubles per try, 5 tries)
#   GH_TOKEN                         read-only token (set by the workflow)
#
# Needs gh.
set -uo pipefail

: "${GITHUB_REPOSITORY:?}" "${RELEASE_SHA:?}"
DRY_RUN="${DRY_RUN:-false}"
# Time budget against the 75 minute job timeout: polling stops after 60
# minutes. A check that hits a transient failure gives up on its first
# exhausted call (5 tries of at most 60 s each plus 150 s of backoff, 7.5
# minutes), so the verdict is written by about 68 minutes, with the checkout
# and setup well inside the remaining margin.
MAX_WAIT_SECONDS="${MAX_WAIT_SECONDS:-3600}"
POLL_SECONDS="${POLL_SECONDS:-60}"
RETRY_BASE_SECONDS="${RETRY_BASE_SECONDS:-10}"
# Each API call is bounded to 60 s (exit 124 counts as transient).
TIMEOUT=""
command -v timeout >/dev/null 2>&1 && TIMEOUT="timeout 60"
TRANSIENT_RE='HTTP 5[0-9][0-9]|Bad Gateway|Service Unavailable|Gateway Time-?out|timed? ?out|timeout|connection (reset|refused)|EOF|temporar'

# gh_api <path> <jq>: prints the jq result; 0 on success, 2 after the retries.
gh_api() {
  local path="$1" jq="$2" try delay out err errf rc
  errf="$(mktemp)"
  delay="$RETRY_BASE_SECONDS"
  for try in 1 2 3 4 5; do
    rc=0
    out="$($TIMEOUT gh api "$path" --jq "$jq" 2>"$errf")" || rc=$?
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
# Both events count: push runs, and the release-candidate dispatch (full=true).
EVENTS="push workflow_dispatch"

# Consecutive checks that found completed runs, none passing, nothing in flight.
REFUSE_STREAK=0

# runs_url <event>: one unfiltered query per event (no status filter), so a
# check costs two list calls plus the job queries of the completed runs. This
# keeps the polling well inside the 1000 requests/hour limit of github.token
# (a rate-limit 403 is not retried and would refuse the release).
runs_url() { echo "${RUNS_PATH}?${RUNS_Q}&event=$1&branch=master"; }

# check: 0 = passed, 1 = refuse, 2 = API failure (refuse), 3 = wait.
check() {
  local ids="" inflight="" runs rid st concl entry run ev jobs
  for ev in $EVENTS; do
    runs="$(gh_api "$(runs_url "$ev")" '.workflow_runs[] | "\(.id) \(.status) \(.conclusion)"')" || return 2
    while read -r rid st concl; do
      [ -n "$rid" ] || continue
      if [ "$st" = completed ]; then ids="${ids} ${rid}:${ev}"; else inflight="${inflight} ${rid}"; fi
    done <<<"$runs"
  done
  for entry in $ids; do
    run="${entry%%:*}"; ev="${entry#*:}"
    jobs="$(gh_api "repos/${GITHUB_REPOSITORY}/actions/runs/${run}/jobs?per_page=100" '.jobs[] | "\(.name)=\(.conclusion)"')" || return 2
    echo "ci.yml run ${run}:"
    echo "$jobs" | grep -E '(^| / )(Apache|Nginx) System Tests=' || echo "  (no system-test jobs)"
    if echo "$jobs" | grep -Eq '(^|/ )Apache System Tests=success$' \
       && echo "$jobs" | grep -Eq '(^|/ )Nginx System Tests=success$'; then
      echo "System-test lanes passed in ci.yml run ${run} (${ev})"
      return 0
    fi
  done
  if [ -n "$inflight" ]; then
    REFUSE_STREAK=0
    echo "ci.yml run(s) of ${RELEASE_SHA} still in flight:${inflight}"
    return 3
  fi
  if [ -n "$ids" ]; then
    REFUSE_STREAK=$((REFUSE_STREAK + 1))
    echo "no ci.yml run of ${RELEASE_SHA} passed both Apache and Nginx System Tests"
    if [ "$REFUSE_STREAK" -ge 2 ]; then
      return 1
    fi
    echo "one more check before refusing"
    return 3
  fi
  REFUSE_STREAK=0
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
  echo "::warning::dry run: the system-test gate would refuse a real run of this tag (see above)"
  exit 0
fi
echo "::error::the system-test gate refuses this release (see above)"
exit 1
