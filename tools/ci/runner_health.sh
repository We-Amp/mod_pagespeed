#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2024-2026 We-Amp B.V.
#
# The runner-health issues (docs/ci/pipelines.md, "IIS infrastructure
# failures"). Runs hourly in the hosted `runner-health` job of
# pipeline-sweep.yml. pipeline-pr.yml runs on merge groups and holds no write
# token, so the IIS job cannot open an issue itself; this sweep reads what it
# left behind instead.
#
# An infrastructure failure of `IIS System Tests` (a failure of the host, not
# of the change; tools/ci/iis_failure_class.py) leaves:
#   * the first attempt green, with a warning annotation naming the reason,
#     and `IIS System Tests (retry 1|2)` jobs in the same run attempt;
#   * a retry leg that failed the same way: an error annotation saying "the
#     retry failed for a reason of the host too (<reason>)".
# Each such job is one record against the runner it ran on. The ci.yml runs
# created in the last LOOKBACK_HOURS (default 6; the sweep runs hourly, so
# every run is seen several times) are read, newest first, at most MAX_RUNS
# (default 100) of them.
#
# One open issue per runner, labelled runner-health and titled
#   Runner health: <runner> (IIS System Tests)
# holds the records, one per job, each behind a marker line
# `<!-- runner-health-record job=<job id> -->` (a job already recorded is
# skipped, so overlapping sweeps count it once). On the second record while
# the issue is open, the title gets the prefix `[drain] ` and a comment asks
# for the runner to be taken out of the pool: that is the drain signal. This
# script never changes a runner, its labels or its registration; whoever
# repairs the runner closes the issue, which starts the count again.
#
# Env: GITHUB_REPOSITORY, GH_TOKEN (actions: read, checks: read, issues:
# write); optional DRY_RUN (`true`: reads only, every write is printed as
# `dry run: would <verb> ...`), RUNNER_HEALTH_LOOKBACK_HOURS,
# RUNNER_HEALTH_MAX_RUNS, RUNNER_HEALTH_NOW_EPOCH (tests).
#
# Exit 0 when done, 1 when an API call failed or its answer is unusable.
# Needs gh and jq.

set -uo pipefail

log() { echo "runner health: $*" >&2; }
die() { echo "::error::runner health: $*" >&2; exit 1; }

repo="${GITHUB_REPOSITORY:-}"
dry=false
[ "${DRY_RUN:-false}" = "true" ] && dry=true
hours="${RUNNER_HEALTH_LOOKBACK_HOURS:-6}"
case "${hours}" in '' | *[!0-9]*) hours=6 ;; esac
max_runs="${RUNNER_HEALTH_MAX_RUNS:-100}"
case "${max_runs}" in '' | *[!0-9]*) max_runs=100 ;; esac
now="${RUNNER_HEALTH_NOW_EPOCH:-$(date -u +%s)}"

[ -n "${repo}" ] || die "GITHUB_REPOSITORY is required"
export GH_REPO="${repo}"

JOB="IIS System Tests"
LABEL="runner-health"
DRAIN="[drain] "

TMP="$(mktemp -d)"
trap 'rm -rf "${TMP}"' EXIT

write() { # <what it does, for the dry-run line> <gh args...>
  local what="$1"
  shift
  if [ "${dry}" = "true" ]; then
    echo "dry run: would ${what}"
    return 0
  fi
  gh "$@" < /dev/null > /dev/null || die "gh $1 $2 failed"
}

since="$(jq -nr --argjson t "$((now - hours * 3600))" '$t | todate')" || die "cannot compute the lookback start"

# 1. The records: "<job id>\t<runner>\t<reason>\t<run url>" lines.
if ! runs="$(gh api -X GET "repos/${repo}/actions/workflows/ci.yml/runs" \
  -f "created=>=${since}" -f status=completed -f per_page=100 < /dev/null)"; then
  die "the runs list call failed"
fi
run_ids="$(printf '%s' "${runs}" | jq -r --argjson n "${max_runs}" \
  '.workflow_runs | sort_by(.created_at) | reverse | .[:$n][] | .id' 2> /dev/null)" \
  || die "the runs list answer is unusable"

# reason_of <job id> <pattern>: the reason token from the job's annotations.
reason_of() {
  local ann
  if ! ann="$(gh api "repos/${repo}/check-runs/$1/annotations?per_page=50" < /dev/null)"; then
    die "the annotations call for job $1 failed"
  fi
  printf '%s' "${ann}" | jq -r --arg p "$2" \
    '[.[] | .message | capture($p) | .r] | .[0] // empty' 2> /dev/null \
    || die "the annotations answer for job $1 is unusable"
}

: > "${TMP}/records"
for run in ${run_ids}; do
  case "${run}" in '' | *[!0-9]*) die "the runs list answer is unusable" ;; esac
  if ! jobs="$(gh api "repos/${repo}/actions/runs/${run}/jobs?filter=all&per_page=100" < /dev/null)"; then
    die "the jobs call for run ${run} failed"
  fi
  # Run attempts that retried the IIS job, and per attempt the first try
  # (green) and the failed retry legs.
  if ! picked="$(printf '%s' "${jobs}" | jq -r --arg j "${JOB}" '
      [.jobs[] | select(.name | test("(^|/ )" + $j + " \\(retry [12]\\)$"))] as $legs
      | ($legs | map(.run_attempt) | unique) as $att
      | .jobs[]
      | select(.run_attempt as $a | $att | index($a))
      | select(.runner_name != null and .runner_name != "")
      | if (.name | test("(^|/ )" + $j + "$")) and .conclusion == "success" then
          "first\t\(.id)\t\(.runner_name)\t\(.html_url)"
        elif (.name | test("(^|/ )" + $j + " \\(retry [12]\\)$")) and .conclusion == "failure" then
          "retry\t\(.id)\t\(.runner_name)\t\(.html_url)"
        else empty end' 2> /dev/null)"; then
    die "the jobs answer for run ${run} is unusable"
  fi
  [ -n "${picked}" ] || continue
  while IFS=$'\t' read -r kind id runner url; do
    [[ "${id}" =~ ^[0-9]+$ ]] || die "job id '${id}' in run ${run} is not a number"
    [[ "${runner}" =~ ^[A-Za-z0-9][A-Za-z0-9._-]{0,63}$ ]] || { log "job ${id}: runner name '${runner}' skipped"; continue; }
    if [ "${kind}" = first ]; then
      reason="$(reason_of "${id}" 'a failure of the host \((?<r>[a-z0-9-]+)\), not of the change')"
      [ -n "${reason}" ] || reason="unknown"
    else
      reason="$(reason_of "${id}" 'failed for a reason of the host too \((?<r>[a-z0-9-]+)\)')"
      [ -n "${reason}" ] || continue # a test failure of the retry: not the host's
    fi
    printf '%s\t%s\t%s\t%s\n' "${id}" "${runner}" "${reason}" "${url}" >> "${TMP}/records"
  done <<< "${picked}"
done

if [ ! -s "${TMP}/records" ]; then
  log "no infrastructure failure of ${JOB} since ${since}"
  exit 0
fi

# 2. Per runner, the issue.
if ! issues="$(gh issue list --label "${LABEL}" --state open --json number,title --limit 100 < /dev/null)"; then
  die "the issue list call failed"
fi
label_ready=false

mapfile -t runners < <(cut -f2 "${TMP}/records" | LC_ALL=C sort -u)
for runner in "${runners[@]}"; do
  title="Runner health: ${runner} (${JOB})"
  issue="$(printf '%s' "${issues}" | jq -r --arg t "${title}" --arg d "${DRAIN}${title}" \
    '[.[] | select(.title == $t or .title == $d)] | sort_by(.number) | .[0].number // empty' 2> /dev/null)" \
    || die "the issue list answer is unusable"
  seen=""
  cur_title="${title}"
  if [ -n "${issue}" ]; then
    if ! view="$(gh issue view "${issue}" --json title,body,comments < /dev/null)"; then
      die "the issue view call for #${issue} failed"
    fi
    seen="$(printf '%s' "${view}" | jq -r \
      '([.body] + [.comments[].body]) | map(select(. != null)) | .[]' 2> /dev/null \
      | sed -n 's/^<!-- runner-health-record job=\([0-9]*\) -->$/\1/p' | LC_ALL=C sort -u)" \
      || die "the issue view answer for #${issue} is unusable"
    cur_title="$(printf '%s' "${view}" | jq -r '.title')"
  fi
  count="$(printf '%s\n' "${seen}" | grep -c '^[0-9]')"
  new=0
  while IFS=$'\t' read -r id r reason url; do
    [ "${r}" = "${runner}" ] || continue
    if printf '%s\n' "${seen}" | grep -qx "${id}"; then continue; fi
    seen="$(printf '%s\n%s' "${seen}" "${id}")"
    count=$((count + 1))
    new=$((new + 1))
    {
      echo "<!-- runner-health-record job=${id} -->"
      echo "Infrastructure failure ${count} of \`${JOB}\` on runner \`${runner}\`: \`${reason}\`."
      echo "Job: ${url}"
    } > "${TMP}/record"
    if [ -z "${issue}" ]; then
      {
        cat "${TMP}/record"
        echo
        echo "This runner failed \`${JOB}\` for a reason of the host, not of the change under test; the tests ran again on another runner."
        echo "A second such failure while this issue is open marks the runner for draining (title prefix \`[drain]\`)."
        echo "Close this issue once the runner is repaired; the count starts again."
      } > "${TMP}/body"
      if [ "${label_ready}" = false ]; then
        if [ "${dry}" = "true" ]; then
          echo "dry run: would create label ${LABEL}"
        else
          gh label create "${LABEL}" --color FBCA04 --description 'A CI runner fails jobs for reasons of the host' \
            < /dev/null > /dev/null 2>&1 || log "label ${LABEL} not created (it may exist already)"
        fi
        label_ready=true
      fi
      if [ "${dry}" = "true" ]; then
        echo "dry run: would create issue '${title}' labelled ${LABEL}"
        issue="(new)"
      else
        url_new="$(gh issue create --title "${title}" --label "${LABEL}" --body-file "${TMP}/body" < /dev/null)" \
          || die "gh issue create failed"
        issue="${url_new##*/}"
        [[ "${issue}" =~ ^[0-9]+$ ]] || die "gh issue create answered '${url_new}'"
      fi
    else
      write "comment on issue #${issue}" issue comment "${issue}" --body-file "${TMP}/record"
    fi
  done < "${TMP}/records"
  [ "${new}" -gt 0 ] || { log "${runner}: nothing new"; continue; }
  if [ "${count}" -ge 2 ] && [ "${cur_title#"${DRAIN}"}" = "${cur_title}" ]; then
    {
      echo "**Drain requested.** \`${runner}\` failed \`${JOB}\` for reasons of the host ${count} times while this issue was open."
      echo "Take ${runner} out of the fleet-iis pool (docs/ci/runner-environment.md, \"Draining a runner\"), repair it, then close this issue."
    } > "${TMP}/drain"
    write "comment on issue #${issue}" issue comment "${issue}" --body-file "${TMP}/drain"
    write "edit issue #${issue}" issue edit "${issue}" --title "${DRAIN}${title}"
    echo "::warning::${runner} failed ${JOB} for reasons of the host ${count} times: drain requested in issue #${issue}"
  else
    log "${runner}: ${new} new record(s), ${count} in issue #${issue}"
  fi
done
exit 0
