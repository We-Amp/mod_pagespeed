#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2024-2026 We-Amp B.V.
#
# Fleet scope of the pull request tier (docs/ci/pipelines.md, "CI_SCOPE").
# Decides whether the fleet jobs of pipeline-pr.yml run for this event.
#
# The workflow reads this file from the default branch and runs it from the
# runner's temp directory, never from the tree under test. workflow_dispatch
# never reaches it: the workflow decides that inline (always the full tier).
# A merge group runs the full tier unless the docs lane below applies.
#
# Env: GITHUB_EVENT_NAME, GITHUB_REPOSITORY, CI_SCOPE (the repository
# variable), SHA (the commit under test), BEFORE_SHA (the push's previous
# tip), GH_TOKEN (checks: read),
# FORK_OR_BOT, PR_FULL. Run from a checkout of SHA with its parent commit.
#
# Stdout: exactly two lines, `fleet=<true|false>` and `heavy=<true|false>`.
# Everything else goes to stderr: one `fleet scope:` line and, on a push, one
# `shadow:` line (what the push rule would decide, whatever CI_SCOPE says).
# A pull request or merge group also gets `docs lane:` lines.
#
# The docs lane (docs/ci/pipelines.md, "DOCS_LANE"): a pull request or merge
# group whose every changed path is on the allowlist of tools/ci/docs_only.sh
# skips the fleet jobs, but only when the repository variable DOCS_LANE is
# `on`. Unset or any other value is shadow mode: the `docs lane:` line says
# what the lane would decide and the output stays the full tier. The
# classifier is read from origin/DEFAULT_BRANCH, like this file. The ci:pr-full
# label (PR_FULL) always runs the full tier. A merge group is docs-only only
# while the default branch's merge queue groups ALLGREEN (read from the rules
# API; anything else, or a failed read, is the full tier): under any other
# grouping a fast docs entry could merge a code entry queued ahead of it
# without that entry's fleet result. Extra env: DOCS_LANE, DEFAULT_BRANCH,
# BASE (pull_request.base.sha or merge_group.base_sha).
#
# The push rule: a push whose commit the merge queue already proved (a
# completed, successful check suite of GitHub Actions on a gh-readonly-queue/
# branch for the same SHA) skips the fleet jobs, unless the push changes the
# pipeline itself or the pull request of ANY commit of the merge group (the
# last `(#N)` of each subject, looked up through the API; the group is the
# union of the push range and the last 10 first-parent commits, because a
# stacked group's base is the previous entry) is from a fork, from another
# repository or from dependabot[bot]: such a group ran on hosted runners. An
# unreadable range, pull request or lookup means no skip. It is on only when CI_SCOPE is push, pr-build or pr; any
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
  # The whole push when its range is usable (a merge group lands up to 10
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
# MAX_PUSH_COMMITS commits (a merge group holds at most 10 entries). The base is
# the first argument (default BEFORE_SHA) and must be an ancestor of HEAD.
MAX_PUSH_COMMITS=10
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

# pr_trusted: prints true, false or error. The proof covers the whole merge
# group, and in a stacked group the suite's own base is the previous entry's
# commit, so no base is taken from the proof. EVERY commit of the union of the
# push range (BEFORE_SHA..HEAD, which must be usable) and the last 10
# first-parent commits ending at HEAD (a group holds at most 10 entries) must
# name, as the last `(#N)` of its subject, a pull request that passes
# pr_lookup_trusted. A group that held any other pull request ran on hosted
# runners, so its proof does not cover the fleet. An unusable range, a missing
# number or an unknown lookup is not trusted.
pr_trusted() {
  local commits tail sha subject n r
  if [ -z "${GITHUB_REPOSITORY:-}" ]; then
    echo false
    return
  fi
  commits="$(range_commits)" || { echo false; return; }
  tail="$(git rev-list --first-parent -n "${MAX_PUSH_COMMITS}" HEAD 2>/dev/null)" || { echo false; return; }
  [ -n "${tail}" ] || { echo false; return; }
  commits="$(printf '%s\n%s\n' "${commits}" "${tail}" | awk 'NF && !seen[$0]++')"
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
      if [ "${result}" = true ]; then
        result="$(pr_trusted)"
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

# --- The docs lane -------------------------------------------------------------
lane="$(printf '%s' "${DOCS_LANE:-}" | tr '[:upper:]' '[:lower:]')"
[ "${lane}" = on ] || lane=shadow

# docs_only <base> <head>: prints true or false. The classifier is read from
# origin/DEFAULT_BRANCH; an unreadable classifier or any output other than
# `docs_only=true` is false. Its own `docs lane:` reason line goes to stderr.
docs_only() {
  local script res
  script="$(mktemp "${TMPDIR:-/tmp}/docs_only.XXXXXX")" || { echo false; return; }
  if [ -z "${DEFAULT_BRANCH:-}" ] ||
    ! git show "origin/${DEFAULT_BRANCH}:tools/ci/docs_only.sh" > "${script}" 2>/dev/null; then
    rm -f "${script}"
    log "docs lane: classifier unreadable on origin/${DEFAULT_BRANCH:-<unset>}"
    echo false
    return
  fi
  res="$(bash "${script}" "$1" "$2")" || res=""
  rm -f "${script}"
  if [ "${res}" = docs_only=true ]; then echo true; else echo false; fi
}

# queue_mode: prints the grouping strategy of the default branch's merge queue
# (ALLGREEN, HEADGREEN, ...), `none` without a queue rule, `mixed` for more
# than one, or `error` when the rules cannot be read.
queue_mode() {
  local tries=0 delay=2 body mode
  if [ -z "${GITHUB_REPOSITORY:-}" ] || [ -z "${DEFAULT_BRANCH:-}" ]; then
    echo error
    return
  fi
  while :; do
    tries=$((tries + 1))
    if body="$(gh api "repos/${GITHUB_REPOSITORY}/rules/branches/${DEFAULT_BRANCH}?per_page=100")" &&
      mode="$(printf '%s' "${body}" | jq -r '
        if type != "array" then "error" else
        [ .[] | select(.type == "merge_queue") | (.parameters.grouping_strategy // "unknown") ] | unique
        | if length == 0 then "none" elif length == 1 then .[0] else "mixed" end end' 2>/dev/null)" &&
      [ -n "${mode}" ] && [ "${mode}" != error ]; then
      echo "${mode}"
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
  # Burst guard of the docs lane: once a docs-only queue entry can pass
  # without the fleet, its proof covers only itself. A push whose tip commit
  # is docs-only but whose range holds code is not proven by the tip's suite.
  if [ "${proof}" = true ] && [ "$(docs_only HEAD^ HEAD 2>/dev/null)" = true ] &&
    [ "$(docs_only "${BEFORE_SHA:-}" HEAD 2>/dev/null)" = false ]; then
    if [ "${lane}" = on ]; then
      proof=false
      log "docs lane: burst guard: docs-only tip with code in the push range; the queue proof does not cover it"
    else
      log "docs lane: shadow: the burst guard would drop the queue proof (docs-only tip, code in the push range)"
    fi
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

if [ "${event}" = pull_request ] || [ "${event}" = merge_group ]; then
  docs="$(docs_only "${BASE:-}" HEAD)"
  qm=n/a
  if [ "${event}" = merge_group ] && [ "${docs}" = true ]; then
    qm="$(queue_mode)"
    [ "${qm}" = ALLGREEN ] || docs=false
  fi
  docs_skip="${docs}"
  [ "${PR_FULL:-false}" != true ] || docs_skip=false
  log "docs lane: mode=${lane} would_skip=${docs_skip} docs_only=${docs} queue=${qm} pr_full=${PR_FULL:-false}"
  if [ "${docs_skip}" = true ]; then
    if [ "${lane}" = on ]; then
      fleet=false
      heavy=false
      reason="docs only"
      log "docs lane: skipping vendor and every fleet job that needs it"
    else
      log "docs lane: shadow: would skip vendor and every fleet job that needs it; this run keeps the full tier (DOCS_LANE is not on)"
    fi
  fi
fi

log "fleet scope: event=${event:-none} scope=${scope} fleet=${fleet} heavy=${heavy} reason=${reason}"
printf 'fleet=%s\nheavy=%s\n' "${fleet}" "${heavy}"
