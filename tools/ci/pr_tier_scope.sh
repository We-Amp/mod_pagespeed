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
# variable), SHA (the commit under test), BEFORE_SHA (the push's previous
# tip), GH_TOKEN (checks: read),
# FORK_OR_BOT, PR_FULL. Run from a checkout of SHA with its parent commit.
#
# Stdout: exactly two lines, `fleet=<true|false>` and `heavy=<true|false>`.
# Everything else goes to stderr: one `fleet scope:` line and, on a push, one
# `shadow:` line (what the push rule would decide, whatever CI_SCOPE says).
#
# The push rule: a push whose commit the merge queue already proved (a
# completed, successful check suite of GitHub Actions on a gh-readonly-queue/
# branch for the same SHA) skips the fleet jobs, unless the push changes the
# pipeline itself or the pull request of ANY commit of the merge group (the
# last `(#N)` of each subject in base..SHA, where the base is the suffix of the
# queue suite's branch name `gh-readonly-queue/<branch>/pr-N-<base sha>`, looked
# up through the API) is from a fork, from another repository or from
# dependabot[bot]: such a group ran on hosted runners. An unreadable or
# missing base, pull request or lookup means no skip. It is on only when CI_SCOPE is push, pr-build or pr; any
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
  # The whole push when its range is usable (a merge group lands up to 3
  # commits at once), else the tip commit alone.
  if range_commits >/dev/null 2>&1; then
    changed="$(git diff --name-only "${BEFORE_SHA}" HEAD 2>/dev/null)" || return 0
  else
    changed="$(git diff --name-only HEAD^ HEAD 2>/dev/null)" || return 0
  fi
  # A here-string, not a pipe: grep -q exits at the first match, and under
  # pipefail a writer killed by SIGPIPE would turn a match into a miss.
  grep -qE '^(\.github/workflows/(pipeline-pr|ci)\.yml$|\.github/actions/|tools/ci/)' <<< "${changed}"
}

# range_commits: prints the commits of the push (BEFORE_SHA..HEAD), newest
# first, and succeeds only for a usable range: BEFORE_SHA set, not all zeros,
# a commit of this repository, no merge commit in the range, and between 1 and
# MAX_PUSH_COMMITS commits (a merge group holds at most 3 entries). The base is
# the first argument (default BEFORE_SHA) and must be an ancestor of HEAD.
MAX_PUSH_COMMITS=3
range_commits() {
  local before="${1-${BEFORE_SHA:-}}" list n
  [ -n "${before}" ] || return 1
  [ -n "$(printf '%s' "${before}" | tr -d '0')" ] || return 1
  git rev-parse -q --verify "${before}^{commit}" >/dev/null 2>&1 || return 1
  git merge-base --is-ancestor "${before}" HEAD >/dev/null 2>&1 || return 1
  [ -z "$(git rev-list --min-parents=2 "${before}..HEAD" 2>/dev/null)" ] || return 1
  list="$(git rev-list "${before}..HEAD" 2>/dev/null)" || return 1
  [ -n "${list}" ] || return 1
  n="$(printf '%s\n' "${list}" | wc -l | tr -d ' ')"
  [ "${n}" -le "${MAX_PUSH_COMMITS}" ] || return 1
  printf '%s\n' "${list}"
}

# pr_lookup_trusted <number>: prints true, false or error. The pull request
# must be from this repository (head repository not null, not a fork) and not
# authored by dependabot[bot].
pr_lookup_trusted() {
  local n="$1" out="" tries=0 delay=2 fork login full
  while :; do
    tries=$((tries + 1))
    if out="$(gh api "repos/${GITHUB_REPOSITORY}/pulls/${n}" \
      --jq '[(.head.repo.fork | tostring), (.user.login // ""), (.head.repo.full_name // "")] | @tsv')" &&
      [ -n "${out}" ]; then
      break
    fi
    if [ "${tries}" -ge 3 ]; then
      echo error
      return
    fi
    sleep "${delay}"
    delay=$((delay * 2))
  done
  IFS=$'\t' read -r fork login full <<< "${out}"
  if [ "${fork}" = false ] && [ -n "${login}" ] && [ "${login}" != "dependabot[bot]" ] &&
    [ -n "${full}" ] &&
    [ "$(printf '%s' "${full}" | tr '[:upper:]' '[:lower:]')" = "$(printf '%s' "${GITHUB_REPOSITORY}" | tr '[:upper:]' '[:lower:]')" ]; then
    echo true
  else
    echo false
  fi
}

# pr_trusted <group base>: prints true, false or error. EVERY commit of the
# merge group, base..HEAD, where the base is the commit the successful queue
# suite was built on (a merge group lands up to 3 entries in one push, and the
# proof covers the whole group, not just this push's range), must name, as the
# last `(#N)` of its subject, a pull request that passes pr_lookup_trusted. The
# push's own range (BEFORE_SHA) must be usable too. A group that held
# any other pull request ran on hosted runners, so its proof does not cover the
# fleet. An unusable range, a missing number or an unknown lookup is not
# trusted.
pr_trusted() {
  local commits sha subject n r
  if [ -z "${GITHUB_REPOSITORY:-}" ]; then
    echo false
    return
  fi
  range_commits >/dev/null || { echo false; return; }
  commits="$(range_commits "${1-}")" || { echo false; return; }
  for sha in ${commits}; do
    subject="$(git log -1 --format=%s "${sha}" 2>/dev/null)" || { echo error; return; }
    n="$(printf '%s\n' "${subject}" | sed -nE 's/.*\(#([0-9]+)\).*/\1/p' | head -n 1)"
    if [ -z "${n}" ]; then
      echo false
      return
    fi
    r="$(pr_lookup_trusted "${n}")"
    if [ "${r}" != true ]; then
      echo "${r}"
      return
    fi
  done
  echo true
}

# queue_group_trusted <suite branch>...: the group base is the suffix of the
# queue suite's head_branch, `gh-readonly-queue/<branch>/pr-N-<base sha>`. Prints
# true, false or error. Several differing suite branches, a missing or
# unparseable base, or a base that is not an ancestor of HEAD mean no proof.
queue_group_trusted() {
  local base
  if [ "$(printf '%s' "${1-}" | wc -w | tr -d ' ')" != 1 ]; then
    echo false
    return
  fi
  base="$(printf '%s\n' "$1" | sed -nE 's#^gh-readonly-queue/.+/pr-[0-9]+-([0-9a-f]{40})$#\1#p')"
  if [ -z "${base}" ]; then
    echo false
    return
  fi
  pr_trusted "${base}"
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
        | [ .[].head_branch ] | unique
        | if length > 0 then "true " + join(" ") else "false" end' 2>/dev/null)" &&
      { [ "${result}" = false ] || [ "${result%% *}" = true ]; }; then
      if [ "${result}" != false ]; then
        result="$(queue_group_trusted "${result#true }")"
      fi
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
