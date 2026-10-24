#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2024-2026 We-Amp B.V.
#
# Fleet scope of the pull request tier (docs/ci/pipelines.md, "CI_SCOPE").
# Decides whether the fleet jobs of pipeline-pr.yml run for this event.
#
# The workflow reads this file from the default branch and runs it from the
# runner's temp directory, never from the tree under test. merge_group and
# workflow_dispatch never reach it: the workflow decides those inline (always
# the full tier).
#
# Env: GITHUB_EVENT_NAME, GITHUB_REPOSITORY, CI_SCOPE (the repository
# variable), SHA (the commit under test), GH_TOKEN (checks: read),
# FORK_OR_BOT, PR_FULL. Run from a checkout of SHA with its parent commit.
#
# Stdout: exactly two lines, `fleet=<true|false>` and `heavy=<true|false>`.
# Everything else goes to stderr: one `fleet scope:` line and, on a push, one
# `shadow:` line (what the push rule would decide, whatever CI_SCOPE says).
#
# The push rule: a push whose commit the merge queue already proved (a
# completed, successful check suite of GitHub Actions on a gh-readonly-queue/
# branch for the same SHA) skips the fleet jobs, unless the push changes the
# pipeline itself. It is on only when CI_SCOPE is push, pr-build or pr; any
# other value, or none, is `full` and runs everything. Every other event runs
# everything. Anything unexpected fails safe to the full tier.

set -uo pipefail

log() { echo "$*" >&2; }

event="${GITHUB_EVENT_NAME:-}"
scope="$(printf '%s' "${CI_SCOPE:-}" | tr '[:upper:]' '[:lower:]')"
case "${scope}" in
  push|pr-build|pr) ;;
  *) scope=full ;;
esac

fleet=true
heavy=true
reason="event ${event:-none}"

# pipeline_change: the pushed commit touches the PR tier's own definition,
# or it has no parent to compare with.
pipeline_change() {
  local changed
  git rev-parse -q --verify "HEAD^{commit}" >/dev/null 2>&1 || return 0
  git rev-parse -q --verify "HEAD^^{commit}" >/dev/null 2>&1 || return 0
  changed="$(git diff --name-only HEAD^ HEAD 2>/dev/null)" || return 0
  # A here-string, not a pipe: grep -q exits at the first match, and under
  # pipefail a writer killed by SIGPIPE would turn a match into a miss.
  grep -qE '^(\.github/workflows/(pipeline-pr|ci)\.yml$|\.github/actions/|tools/ci/)' <<< "${changed}"
}

# queue_proof: prints true, false or error.
queue_proof() {
  local path="repos/${GITHUB_REPOSITORY:-}/commits/${SHA:-}/check-suites?app_id=15368&per_page=100"
  local tries=0 delay=2 body result
  if [ -z "${GITHUB_REPOSITORY:-}" ] || [ -z "${SHA:-}" ]; then
    echo error
    return
  fi
  while :; do
    tries=$((tries + 1))
    if body="$(gh api "${path}")" && [ -n "$(printf '%s' "${body}" | tr -d '[:space:]')" ] &&
      result="$(printf '%s' "${body}" | jq -r '
        [ (.check_suites? // [])[]?
          | select(((.head_branch // "") | startswith("gh-readonly-queue/"))
                   and .status == "completed" and .conclusion == "success") ]
        | if length > 0 then "true" else "false" end' 2>/dev/null)" &&
      { [ "${result}" = true ] || [ "${result}" = false ]; }; then
      echo "${result}"
      return
    fi
    if [ "${tries}" -ge 3 ]; then
      echo error
      return
    fi
    sleep "${delay}"
    delay=$((delay * 2))
  done
}

if [ "${event}" = push ]; then
  if pipeline_change; then
    pc=true
    proof=false
  else
    pc=false
    proof="$(queue_proof)"
  fi
  would_skip=false
  if [ "${pc}" = false ] && [ "${proof}" = true ]; then
    would_skip=true
  fi
  if [ "${scope}" = full ]; then
    reason="scope full"
  elif [ "${pc}" = true ]; then
    reason="pipeline change"
  elif [ "${proof}" = true ]; then
    fleet=false
    heavy=false
    reason="queue proof"
  elif [ "${proof}" = error ]; then
    reason="queue proof unreadable"
  else
    reason="no queue proof"
  fi
  log "shadow: would_skip=${would_skip} proof=${proof} pipeline_change=${pc}"
fi

log "fleet scope: event=${event:-none} scope=${scope} fleet=${fleet} heavy=${heavy} reason=${reason}"
printf 'fleet=%s\nheavy=%s\n' "${fleet}" "${heavy}"
