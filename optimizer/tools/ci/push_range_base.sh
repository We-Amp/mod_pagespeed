#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2024-2026 We-Amp B.V.
#
# Base of the cumulative range a push to the default branch covers
# (docs/ci/pipelines.md, "The sidecar native pair" and "The full tier"). The
# caller diffs <base>..GITHUB_SHA. The base is the head of a push run of CI
# (ci.yml) on the default branch whose head is an ancestor of GITHUB_SHA and
# whose range was evaluated, so everything after it is still to be examined.
# `github.event.before` is not enough: it names the previous push even when
# that push's run was cancelled before it evaluated anything.
#
# Two modes:
#   push_range_base.sh
#     The newest push run that SUCCEEDED. Any other conclusion (cancelled,
#     failure, startup_failure, timed_out, skipped, none) may never have
#     evaluated its range. The cost: after a red push to the default branch,
#     the next push covers the red push's range again. Kept for
#     compatibility; it has no in-tree caller today.
#   push_range_base.sh --require-job <job name>
#     The newest push run that completed with success or failure (never
#     cancelled, skipped or none) and whose job named exactly <job name>
#     succeeded in its latest attempt: the run in which that job evaluated
#     the range. One jobs API call per ancestor candidate, at most
#     PUSH_RANGE_MAX_JOB_LOOKUPS (default 5) of them. Used by the full tier's
#     path filter (`full / Detect changed paths (full tier)`,
#     pipeline-full.yml) and by tools/ci/full_tier_scope.sh (`Full tier
#     scope`, ci.yml).
#
# Env: GITHUB_REPOSITORY, GITHUB_SHA, GH_TOKEN (actions: read); optional
# DEFAULT_BRANCH (default master), PUSH_RANGE_MAX_JOB_LOOKUPS. Run from a
# checkout with full history.
#
# Stdout: the base SHA, or nothing when no usable base was found (an API call
# failed, an answer was unusable, no candidate is an ancestor, or no run
# within the lookup cap ran the job green). The caller treats nothing as
# "everything changed". Diagnostics go to stderr. Always exits 0.

set -uo pipefail

log() { echo "push range base: $*" >&2; }

require_job=""
if [ "${1:-}" = "--require-job" ]; then
  if [ -z "${2:-}" ]; then
    log "--require-job needs a job name"
    exit 0
  fi
  require_job="$2"
fi

repo="${GITHUB_REPOSITORY:-}"
sha="${GITHUB_SHA:-}"
branch="${DEFAULT_BRANCH:-master}"

if [ -z "${repo}" ] || [ -z "${sha}" ]; then
  log "GITHUB_REPOSITORY or GITHUB_SHA missing"
  exit 0
fi

if ! body="$(gh api "repos/${repo}/actions/workflows/ci.yml/runs?event=push&branch=${branch}&status=completed&per_page=30")"; then
  log "the runs API call failed"
  exit 0
fi

if [ -n "${require_job}" ]; then
  max="${PUSH_RANGE_MAX_JOB_LOOKUPS:-5}"
  case "${max}" in ''|*[!0-9]*) max=5 ;; esac
  if ! candidates="$(printf '%s' "${body}" | jq -r --arg sha "${sha}" '
      [ (.workflow_runs? // [])[]?
        | select((.conclusion == "success" or .conclusion == "failure")
                 and .head_sha != $sha
                 and ((.head_sha // "") | test("^[0-9a-f]{40}$"))
                 and ((.id // "") | tostring | test("^[0-9]+$"))) ]
      | sort_by(.created_at) | reverse | .[] | "\(.id) \(.head_sha)"' 2>/dev/null)"; then
    log "the runs API answer is unusable"
    exit 0
  fi
  if [ -z "${candidates}" ]; then
    log "no completed push run of CI on ${branch} other than this commit"
    exit 0
  fi
  lookups=0
  while read -r id cand; do
    [ -n "${cand}" ] || continue
    if ! git merge-base --is-ancestor "${cand}" "${sha}" 2>/dev/null; then
      log "skipping ${cand}: not an ancestor of ${sha}"
      continue
    fi
    if [ "${lookups}" -ge "${max}" ]; then
      break
    fi
    lookups=$((lookups + 1))
    if ! jobs_body="$(gh api "repos/${repo}/actions/runs/${id}/jobs?filter=latest&per_page=100")"; then
      log "the jobs API call for run ${id} failed"
      exit 0
    fi
    if ! concl="$(printf '%s' "${jobs_body}" | jq -r --arg name "${require_job}" '
        [ (.jobs // error("no jobs"))[] | select(.name == $name) | .conclusion ]
        | first // "absent"' 2>/dev/null)"; then
      log "the jobs API answer for run ${id} is unusable"
      exit 0
    fi
    if [ "${concl}" = "success" ]; then
      log "base ${cand} (run ${id}, job '${require_job}' succeeded)"
      printf '%s\n' "${cand}"
      exit 0
    fi
    log "skipping ${cand}: job '${require_job}' of run ${id} is ${concl}"
  done <<< "${candidates}"
  if [ "${lookups}" -ge "${max}" ]; then
    log "no run within ${max} lookups ran '${require_job}' green"
  else
    log "no ancestor run ran '${require_job}' green"
  fi
  exit 0
fi

if ! candidates="$(printf '%s' "${body}" | jq -r --arg sha "${sha}" '
    [ (.workflow_runs? // [])[]?
      | select(.conclusion == "success" and .head_sha != $sha
               and ((.head_sha // "") | test("^[0-9a-f]{40}$"))) ]
    | sort_by(.created_at) | reverse | .[].head_sha' 2>/dev/null)"; then
  log "the runs API answer is unusable"
  exit 0
fi

if [ -z "${candidates}" ]; then
  log "no successful push run of CI on ${branch} other than this commit"
  exit 0
fi

while IFS= read -r cand; do
  [ -n "${cand}" ] || continue
  if git merge-base --is-ancestor "${cand}" "${sha}" 2>/dev/null; then
    log "base ${cand}"
    printf '%s\n' "${cand}"
    exit 0
  fi
  log "skipping ${cand}: not an ancestor of ${sha}"
done <<< "${candidates}"

log "no candidate is an ancestor of ${sha}"
exit 0
