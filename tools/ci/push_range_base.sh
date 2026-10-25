#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2024-2026 We-Amp B.V.
#
# Base of the cumulative range a push to the default branch covers
# (docs/ci/pipelines.md, "The sidecar native pair"). The full tier's path
# filter diffs <base>..GITHUB_SHA. The base is the head of the most recent
# push `CI` run on the default branch that completed with a conclusion other
# than `cancelled` and whose head is an ancestor of GITHUB_SHA: that run's own
# range was evaluated, so everything after it is still to be examined. A
# cancelled run (a newer push superseded it) never evaluated its range, which
# is why `github.event.before` is not enough: it names the previous push even
# when that push's run was cancelled.
#
# Env: GITHUB_REPOSITORY, GITHUB_SHA, GH_TOKEN (actions: read); optional
# DEFAULT_BRANCH (default master). Run from a checkout with full history.
#
# Stdout: the base SHA, or nothing when no usable base was found (the API
# failed, the answer was unusable, or no candidate is an ancestor). The caller
# treats nothing as "everything changed". Diagnostics go to stderr. Always
# exits 0.

set -uo pipefail

log() { echo "push range base: $*" >&2; }

repo="${GITHUB_REPOSITORY:-}"
sha="${GITHUB_SHA:-}"
branch="${DEFAULT_BRANCH:-master}"

if [ -z "${repo}" ] || [ -z "${sha}" ]; then
  log "GITHUB_REPOSITORY or GITHUB_SHA missing"
  exit 0
fi

if ! body="$(gh api "repos/${repo}/actions/runs?event=push&branch=${branch}&status=completed&per_page=30")"; then
  log "the runs API call failed"
  exit 0
fi

if ! candidates="$(printf '%s' "${body}" | jq -r --arg sha "${sha}" '
    [ (.workflow_runs? // [])[]?
      | select(.name == "CI" and .conclusion != "cancelled" and .head_sha != $sha
               and ((.head_sha // "") | test("^[0-9a-f]{40}$"))) ]
    | sort_by(.created_at) | reverse | .[].head_sha' 2>/dev/null)"; then
  log "the runs API answer is unusable"
  exit 0
fi

if [ -z "${candidates}" ]; then
  log "no completed, uncancelled push CI run on ${branch} other than this commit"
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
