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
# variable), SHA (the commit under test), GH_TOKEN (checks: read),
# FORK_OR_BOT, PR_FULL, BEFORE_SHA (the push's previous tip, optional). Run
# from a checkout of SHA with its parent commit; with BEFORE_SHA, also with
# that commit.
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
# branch for the same SHA, and the pull request of every commit of the queue
# group, looked up by the last "(#N)" of its subject, is same-repo and not from
# dependabot[bot])
# skips the fleet jobs, unless the push changes the pipeline itself. A group
# with a fork or Dependabot pull request ran on hosted runners, so it is not
# proof. It is on only when CI_SCOPE is push, pr-build or pr; any
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

# pipeline_change: the push touches the PR tier's own definition, or there is
# no sound range to compare. With BEFORE_SHA set (a push carries up to a few
# squashed commits), the range is BEFORE_SHA..HEAD; an all-zero or unreachable
# BEFORE_SHA is a change. Without it, the range is HEAD^..HEAD. Renames are not
# detected: git lists only the destination of a rename by default, which would
# hide a move out of a pipeline path.
pipeline_change() {
  local changed base
  git rev-parse -q --verify "HEAD^{commit}" >/dev/null 2>&1 || return 0
  if [ -n "${BEFORE_SHA:-}" ]; then
    case "${BEFORE_SHA}" in
      *[!0]*) ;;
      *) return 0 ;;
    esac
    git rev-parse -q --verify "${BEFORE_SHA}^{commit}" >/dev/null 2>&1 || return 0
    base="${BEFORE_SHA}"
  else
    git rev-parse -q --verify "HEAD^^{commit}" >/dev/null 2>&1 || return 0
    base="HEAD^"
  fi
  changed="$(git diff --no-renames --name-only "${base}" HEAD 2>/dev/null)" || return 0
  # A here-string, not a pipe: grep -q exits at the first match, and under
  # pipefail a writer killed by SIGPIPE would turn a match into a miss.
  grep -qE '^(\.github/workflows/(pipeline-pr|ci)\.yml$|\.github/actions/|tools/ci/)' <<< "${changed}"
}

# pr_status <n>: prints true, false or error for one pull request: true when
# it is same-repo, not a fork and not authored by dependabot[bot]; false when
# it is a fork, from another repository or Dependabot; error for anything
# unknown (failed lookup, missing head repository).
pr_status() {
  local n="$1" tries=0 delay=2 out fork login full want_lc full_lc
  while :; do
    tries=$((tries + 1))
    if out="$(gh api "repos/${GITHUB_REPOSITORY}/pulls/${n}" \
      --jq '[(.head.repo.fork | tostring), (.user.login // ""), (.head.repo.full_name // "")] | @tsv' 2>/dev/null)" &&
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
  fork="${out%%$'\t'*}"
  out="${out#*$'\t'}"
  login="${out%%$'\t'*}"
  full=""
  case "${out}" in *$'\t'*) full="${out#*$'\t'}" ;; esac
  want_lc="$(printf '%s' "${GITHUB_REPOSITORY}" | tr '[:upper:]' '[:lower:]')"
  full_lc="$(printf '%s' "${full}" | tr '[:upper:]' '[:lower:]')"
  if [ "${fork}" != "false" ] && [ "${fork}" != "true" ]; then
    echo error
  elif [ "${fork}" = "true" ] || [ "${full_lc}" != "${want_lc}" ] || [ "${login}" = "dependabot[bot]" ] || [ -z "${login}" ] || [ -z "${full}" ]; then
    echo false
  else
    echo true
  fi
}

# pr_trusted: prints true, false or error for the push. The proof is the
# queue group's check suite, and the group is every commit between the default
# branch and the group head. A stacked group's base is the previous entry's
# queue commit, so the group cannot be read from the suite's branch name.
# Instead the pull requests of the union of BEFORE_SHA..SHA and the last 10
# first-parent commits ending at SHA (a group has been seen up to 5 commits deep) must all
# be trusted: same-repo and not authored by dependabot[bot], each commit mapped
# by the last "(#N)" of its subject. A queue check suite alone is not proof for
# a fork or Dependabot group, which ran on hosted runners. Anything unknown
# (BEFORE_SHA missing, all-zero, unreachable or not an ancestor of SHA; an
# empty or unreadable range; a merge commit; more than 10 commits in
# BEFORE_SHA..SHA; a subject without a number; a failed lookup) is error, which
# runs the full tier.
pr_trusted() {
  local push_shas window shas sha subject n nums="" count=0 st result=true
  case "${BEFORE_SHA:-}" in
    ""|*[!0]*) ;;
    *) echo error; return ;;
  esac
  [ -n "${BEFORE_SHA:-}" ] || { echo error; return; }
  git rev-parse -q --verify "${BEFORE_SHA}^{commit}" >/dev/null 2>&1 || { echo error; return; }
  git merge-base --is-ancestor "${BEFORE_SHA}" "${SHA:-HEAD}" 2>/dev/null || { echo error; return; }
  push_shas="$(git rev-list "${BEFORE_SHA}..${SHA:-HEAD}" 2>/dev/null)" || { echo error; return; }
  [ -n "${push_shas}" ] || { echo error; return; }
  window="$(git rev-list --first-parent -n 10 "${SHA:-HEAD}" 2>/dev/null)" || { echo error; return; }
  [ -n "${window}" ] || { echo error; return; }
  for sha in ${push_shas}; do
    count=$((count + 1))
  done
  [ "${count}" -le 10 ] || { echo error; return; }
  shas="$(printf '%s\n%s\n' "${push_shas}" "${window}" | sort -u)"
  for sha in ${shas}; do
    [ "$(git rev-list --no-walk --min-parents=2 "${sha}" 2>/dev/null | wc -l | tr -dc '0-9')" = 0 ] || { echo error; return; }
    subject="$(git log -1 --format=%s "${sha}" 2>/dev/null)" || { echo error; return; }
    n="$(printf '%s\n' "${subject}" | grep -oE '\(#[0-9]+\)' | tail -n 1 | tr -dc '0-9' || true)"
    [ -n "${n}" ] || { echo error; return; }
    nums="${nums} ${n}"
  done
  for n in $(printf '%s\n' ${nums} | sort -un); do
    st="$(pr_status "${n}")"
    case "${st}" in
      true) ;;
      false) result=false ;;
      *) echo error; return ;;
    esac
  done
  echo "${result}"
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
                   and .status == "completed" and .conclusion == "success")
          | .head_branch ] | if length == 0 then "none" else "some" end' 2>/dev/null)" &&
      { [ "${result}" = none ] || [ "${result}" = some ]; }; then
      if [ "${result}" = some ]; then
        pr_trusted
      else
        echo false
      fi
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
