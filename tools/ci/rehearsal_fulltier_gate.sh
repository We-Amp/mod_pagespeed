#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2024-2026 We-Amp B.V.
#
# The 7-day full-tier gate of the weekly lane rehearsal (docs/ci/release.md,
# "The weekly rehearsal"). The rehearsal builds the master tip, which rarely
# has a full-tier run of its own, so tools/ci/ci_release_gate.sh only warns
# there. This gate asks a different question: has the full tier been green on
# master at all in the last WINDOW_DAYS days? A rehearsal that finds no such
# run fails, so a full tier that is red or never run cannot hide for a week.
#
# A green full-tier run is a completed run on master of
#   * ci.yml, event workflow_dispatch (a dispatch with full=true), or
#   * nightly.yml (the scheduled full-tier nightly, or a full_tier dispatch),
# created in the window, in which every `full / ` job of the release gate's
# REQUIRED list (tools/ci/ci_release_gate.sh) is success and no `full / ` job
# is failure, cancelled or timed_out. The run's own conclusion does not
# matter: a nightly is red when any of its other jobs is, and a ci.yml
# dispatch carries the PR tier too; only the full tier is judged here. The
# same two exceptions as in the release gate apply: the IIS job of the list
# (`full / Windows AppVerif`) may be skipped while FLEET_IIS_ENABLED is not
# true, and `full / Windows ASan` needs its step "Run the unit tests under
# ASan" to be success. A push run never counts: whether it carries the full
# tier depends on a repository variable that says nothing about an earlier
# run.
#
# Runs are found by their head commit: the runs API lists them with head_sha,
# and the job list of each is read by run id, newest first, until one is
# green. Every API call is retried on a transient failure (HTTP 5xx, timeouts,
# connection errors). A call that still fails, or whose answer cannot be
# read, fails the gate: an unanswered question never passes.
#
# Exit: 0 "rehearsal full-tier gate passed: ...";
#       1 "rehearsal full-tier gate failed: <reason>".
#
# Environment:
#   GITHUB_REPOSITORY   required
#   GH_TOKEN            read-only token (set by the workflow)
#   WINDOW_DAYS         default 7
#   FLEET_IIS_ENABLED   the repository variable, passed by the workflow
#   NOW_EPOCH           the current time in seconds, for tests
#   RETRY_BASE_SECONDS  default 10 (doubles per try, 5 tries)
#   RUNS_PER_PAGE       default 100
#   MAX_RUN_LOOKUPS     default 30: the most job lists read before giving up
#
# Needs gh and jq. Usage: rehearsal_fulltier_gate.sh | --self-test
set -uo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

if [ "${1:-}" = "--self-test" ]; then
  exec bash "${HERE}/test_rehearsal_fulltier_gate.sh"
fi

fail() { echo "::error::rehearsal full-tier gate failed: $*"; exit 1; }

: "${GITHUB_REPOSITORY:?}"
WINDOW_DAYS="${WINDOW_DAYS:-7}"
case "${WINDOW_DAYS}" in ''|*[!0-9]*) fail "WINDOW_DAYS '${WINDOW_DAYS}' is not a number" ;; esac
RETRY_BASE_SECONDS="${RETRY_BASE_SECONDS:-10}"
RUNS_PER_PAGE="${RUNS_PER_PAGE:-100}"
MAX_RUN_LOOKUPS="${MAX_RUN_LOOKUPS:-30}"
FLEET_IIS_ENABLED="${FLEET_IIS_ENABLED:-}"
NOW_EPOCH="${NOW_EPOCH:-$(date -u +%s)}"
TRANSIENT_RE='HTTP 5[0-9][0-9]|Bad Gateway|Service Unavailable|Gateway Time-?out|timed? ?out|timeout|connection (reset|refused)|EOF|temporar'

# The full-tier jobs, from the release gate itself, so the two cannot drift.
REQUIRED="$(CI_RELEASE_GATE_PRINT_REQUIRED=1 bash "${HERE}/ci_release_gate.sh" 2> /dev/null | grep '^full / ')" \
  || fail "cannot read the required full-tier jobs from ci_release_gate.sh"
[ -n "${REQUIRED}" ] || fail "ci_release_gate.sh lists no full-tier job"
IIS_JOBS=('full / Windows AppVerif')
ASAN_JOB='full / Windows ASan'
ASAN_STEP='Run the unit tests under ASan'

# since: the start of the window as an ISO time (GNU date, then BSD date).
since="$(date -u -d "@$((NOW_EPOCH - WINDOW_DAYS * 86400))" +%Y-%m-%dT%H:%M:%SZ 2> /dev/null \
  || date -u -r "$((NOW_EPOCH - WINDOW_DAYS * 86400))" +%Y-%m-%dT%H:%M:%SZ 2> /dev/null)" \
  || fail "cannot compute the start of the window"
[ -n "${since}" ] || fail "cannot compute the start of the window"

TMP="$(mktemp -d)"
trap 'rm -rf "${TMP}"' EXIT

# gh_get <path> [--paginate]: the answer on stdout; 0 on success, 2 after the
# retries.
gh_get() {
  local path="$1" extra="${2:-}" try delay rc err
  delay="${RETRY_BASE_SECONDS}"
  for try in 1 2 3 4 5; do
    rc=0
    gh api ${extra:+"${extra}"} "${path}" < /dev/null > "${TMP}/out" 2> "${TMP}/err" || rc=$?
    if [ "${rc}" -eq 0 ]; then cat "${TMP}/out"; return 0; fi
    err="$(cat "${TMP}/err")"
    if ! printf '%s' "${err}" | grep -Eiq "${TRANSIENT_RE}"; then
      echo "gh api ${path%%\?*} failed (not retried): ${err}" >&2
      return 2
    fi
    if [ "${try}" -lt 5 ]; then
      echo "gh api ${path%%\?*} failed (try ${try}/5), retrying in ${delay}s: ${err}" >&2
      sleep "${delay}"; delay=$((delay * 2))
    else
      echo "gh api ${path%%\?*} failed after 5 tries: ${err}" >&2
    fi
  done
  return 2
}

# The completed runs of both workflows in the window, newest first, one line
# "<created_at> <workflow> <id> <event> <head_sha>" each.
: > "${TMP}/runs"
for src in "ci.yml|&event=workflow_dispatch" "nightly.yml|"; do
  wf="${src%%|*}"
  raw="$(gh_get "repos/${GITHUB_REPOSITORY}/actions/workflows/${wf}/runs?branch=master&status=completed${src#*|}&created=%3E%3D${since}&per_page=${RUNS_PER_PAGE}")" \
    || fail "the runs API call for ${wf} failed"
  printf '%s' "${raw}" | jq -r --arg wf "${wf}" '
      (.workflow_runs // error("no workflow_runs"))[]
      | select((.id | type) == "number" and ((.head_sha // "") | test("^[0-9a-f]{40}$")))
      | "\(.created_at) \($wf) \(.id) \(.event) \(.head_sha)"' >> "${TMP}/runs" 2> "${TMP}/err" \
    || fail "the runs API answer for ${wf} is unusable: $(head -c 300 "${TMP}/err")"
done
sort -r "${TMP}/runs" -o "${TMP}/runs"

# The job lines of one run: "<name>=<conclusion>", and for the ASan job the
# step line "STEP <job>/<step>=<conclusion>".
# shellcheck disable=SC2016 # jq source
JOBS_JQ='(.jobs // error("no jobs"))[]
  | ("\(.name)=\(.conclusion)"),
    (select(.name == $asan) | (.steps // [])[] | select(.name == $step) | "STEP \($asan)/\(.name)=\(.conclusion)")'

# verdict <job lines>: prints why the full tier of the run is not green, or
# nothing when it is.
verdict() {
  local jobs="$1" req bad="" line
  while IFS= read -r req; do
    [ -n "${req}" ] || continue
    printf '%s\n' "${jobs}" | grep -Fxq -- "${req}=success" && continue
    if [ "${FLEET_IIS_ENABLED}" != true ]; then
      local j
      for j in "${IIS_JOBS[@]}"; do
        if [ "${j}" = "${req}" ] && printf '%s\n' "${jobs}" | grep -Fxq -- "${req}=skipped"; then continue 2; fi
      done
    fi
    bad="${bad}${bad:+; }${req} is not success"
  done <<< "${REQUIRED}"
  printf '%s\n' "${jobs}" | grep -Fxq -- "STEP ${ASAN_JOB}/${ASAN_STEP}=success" \
    || bad="${bad}${bad:+; }${ASAN_JOB} step ${ASAN_STEP} is not success"
  while IFS= read -r line; do
    case "${line}" in
      "STEP "*) continue ;;
      "full / "*=failure|"full / "*=cancelled|"full / "*=timed_out) bad="${bad}${bad:+; }${line%%=*} is ${line##*=}" ;;
    esac
  done <<< "${jobs}"
  printf '%s' "${bad}"
}

lookups=0
total="$(wc -l < "${TMP}/runs" | tr -d ' ')"
echo "${total} completed run(s) of ci.yml dispatches and nightly.yml on master since ${since}"
last_reason="no completed ci.yml full=true dispatch or nightly run on master since ${since}"
while read -r created wf id event sha; do
  [ -n "${id}" ] || continue
  if [ "${lookups}" -ge "${MAX_RUN_LOOKUPS}" ]; then
    last_reason="${last_reason}; stopped after ${MAX_RUN_LOOKUPS} job lookups"
    break
  fi
  lookups=$((lookups + 1))
  raw="$(gh_get "repos/${GITHUB_REPOSITORY}/actions/runs/${id}/jobs?filter=latest&per_page=100" --paginate)" \
    || fail "the jobs API call for run ${id} failed"
  jobs="$(printf '%s' "${raw}" | jq -s -r --arg asan "${ASAN_JOB}" --arg step "${ASAN_STEP}" '.[] | '"${JOBS_JQ}" 2> "${TMP}/err")" \
    || fail "the jobs API answer for run ${id} is unusable: $(head -c 300 "${TMP}/err")"
  [ -n "${jobs}" ] || fail "the jobs API answer for run ${id} lists no job"
  bad="$(verdict "${jobs}")"
  if [ -z "${bad}" ]; then
    echo "rehearsal full-tier gate passed: ${wf} run ${id} (${event}) at ${sha}, created ${created}"
    exit 0
  fi
  echo "${wf} run ${id} (${event}, ${sha}): ${bad}"
  [ "${lookups}" -gt 1 ] || last_reason="no green full-tier run on master since ${since}; newest: ${wf} run ${id}: ${bad}"
done < "${TMP}/runs"

fail "${last_reason}"
