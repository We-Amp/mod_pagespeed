#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2024-2026 We-Amp B.V.
#
# The nightly-red issue (docs/ci/pipelines.md, "The nightly-red issue"). Runs
# in the hosted `nightly-red` job at the end of nightly.yml, after the full
# tier (job `full`). The caller sees one result for `full`, and a
# continue-on-error job never turns it red, so this script reads the run's own
# job list instead.
#
# A `full / <job>` job is
#   - skipped  when it concluded skipped: listed on its own line, never as
#              failed and never as green;
#   - green    when it concluded success and every RUNTIME_STEP_CHECKS step
#              named for it concluded success too;
#   - failed   otherwise (failure, cancelled, timed_out, a success whose
#              checked step did not succeed, ...).
#
# Any failed job: one open issue titled "Nightly full tier red", labelled
# nightly-red, is created, or the open one gets a comment and its body
# replaced. Per failed job the body carries the job name, the SHA, the
# failing targets from its log (tools/ci/failing_targets.sh), the last green
# run of that job and the commit count since, and a marker line
# `<!-- nightly-red-job: <job name> -->`. The last green run is searched in
# the completed runs on the default branch of nightly.yml, of ci.yml's
# workflow_dispatch and of ci.yml's push (a schedule, a full_tier or full=true
# dispatch, or a push), created before this run, newest first, one jobs call
# per run. REPORT_MAX_LOOKUPS counts only the runs whose job list has a
# `full / ` job, so push runs without the full tier do not use it up;
# REPORT_MAX_API_CALLS caps the jobs calls of the search as a whole.
#
# No failed job: on attempt 1, when every job named by a marker line of the
# open issue is green in this run, the issue gets a comment and is closed. A
# re-run attempt (GITHUB_RUN_ATTEMPT > 1) only comments: a re-run that goes
# green is a flake signal, never evidence of a fix.
#
# Env: GITHUB_REPOSITORY, GITHUB_RUN_ID, GITHUB_RUN_ATTEMPT, GITHUB_SHA,
# DEFAULT_BRANCH, GH_TOKEN (actions: read, issues: write); optional DRY_RUN
# (`true`: reads only, every write is printed as `dry run: would <verb> ...`),
# REPORT_MAX_LOOKUPS (default 12), REPORT_MAX_API_CALLS (default 40),
# RUNTIME_STEP_CHECKS (newline separated
# `<job name>=<step name>`), GITHUB_SERVER_URL. Run from a checkout with full
# history (for the commit count).
#
# Exit 0 when done, 1 when an API call failed or its answer is unusable (the
# job then shows red).

set -uo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

log() { echo "nightly red report: $*" >&2; }
die() { echo "::error::nightly red report: $*" >&2; exit 1; }

repo="${GITHUB_REPOSITORY:-}"
run="${GITHUB_RUN_ID:-}"
attempt="${GITHUB_RUN_ATTEMPT:-1}"
sha="${GITHUB_SHA:-}"
branch="${DEFAULT_BRANCH:-}"
server="${GITHUB_SERVER_URL:-https://github.com}"
dry=false
[ "${DRY_RUN:-false}" = "true" ] && dry=true
max="${REPORT_MAX_LOOKUPS:-12}"
case "${max}" in ''|*[!0-9]*) max=12 ;; esac
ceiling="${REPORT_MAX_API_CALLS:-40}"
case "${ceiling}" in ''|*[!0-9]*) ceiling=40 ;; esac
case "${attempt}" in ''|*[!0-9]*) attempt=1 ;; esac

if [ -z "${repo}" ] || [ -z "${run}" ] || [ -z "${sha}" ] || [ -z "${branch}" ]; then
  die "GITHUB_REPOSITORY, GITHUB_RUN_ID, GITHUB_SHA and DEFAULT_BRANCH are required"
fi
export GH_REPO="${repo}"

TITLE="Nightly full tier red"
LABEL="nightly-red"
run_url="${server}/${repo}/actions/runs/${run}"

TMP="$(mktemp -d)"
trap 'rm -rf "${TMP}"' EXIT

checks="$(printf '%s\n' "${RUNTIME_STEP_CHECKS:-}" | jq -R -s -c '
  [ split("\n")[] | sub("\r$"; "") | index("=") as $i | select($i != null)
    | { job: (.[:$i] | gsub("^\\s+|\\s+$"; "")),
        step: (.[$i + 1:] | gsub("^\\s+|\\s+$"; "")) } ]')" \
  || die "RUNTIME_STEP_CHECKS is unusable"

# verdict: {v: green|failed|skipped, d: detail} of one job object.
# shellcheck disable=SC2016 # jq source, expanded by jq
JQ_DEFS='
def verdict($checks):
  . as $j
  | if .conclusion == "skipped" then { v: "skipped", d: "skipped" }
    elif .conclusion == "success" then
      [ $checks[] | select(.job == $j.name) | .step as $s
        | ([ ($j.steps // [])[] | select(.name == $s) | .conclusion ] | .[0] // "none") as $c
        | select($c != "success")
        | "step \"\($s)\" concluded \($c)" ] as $bad
      | if ($bad | length) > 0
        then { v: "failed", d: ("success, but " + ($bad | join("; "))) }
        else { v: "green", d: "success" } end
    else { v: "failed", d: (.conclusion // "no conclusion") } end;
'

# run_jobs <run id>: the run's latest-attempt jobs as one JSON array.
run_jobs() {
  local raw
  if ! raw="$(gh api --paginate "repos/${repo}/actions/runs/$1/jobs?filter=latest&per_page=100" < /dev/null)"; then
    die "the jobs API call for run $1 failed"
  fi
  [ -n "${raw}" ] || die "the jobs API answer for run $1 is empty"
  printf '%s' "${raw}" | jq -s -c '[ .[] | (.jobs // error("no jobs"))[] ]' 2> /dev/null \
    || die "the jobs API answer for run $1 is unusable"
}

# green_names <jobs json>: the names of its green `full / ` jobs, one a line.
green_names() {
  printf '%s' "$1" | jq -r --argjson checks "${checks}" "${JQ_DEFS}"'
    .[] | select((.name // "") | startswith("full / "))
    | select(verdict($checks).v == "green") | .name'
}

jobs="$(run_jobs "${run}")" || exit 1
if ! rows="$(printf '%s' "${jobs}" | jq -r --argjson checks "${checks}" "${JQ_DEFS}"'
    [ .[] | select((.name // "") | startswith("full / ")) ] | sort_by(.name) | .[]
    | verdict($checks) as $v
    | [ (.id | tostring), .name, $v.v, $v.d, (.html_url // "") ] | @tsv')"; then
  die "the jobs API answer for run ${run} is unusable"
fi
if [ -z "${rows}" ]; then
  log "run ${run} has no 'full / ' jobs: nothing to report"
  exit 0
fi
greens="$(green_names "${jobs}")"

nfailed=0
skipped=""
while IFS="$(printf '\t')" read -r id name verdict detail url; do
  case "${verdict}" in
    failed)
      f_id[nfailed]="${id}"
      f_name[nfailed]="${name}"
      f_detail[nfailed]="${detail}"
      f_url[nfailed]="${url}"
      g_run[nfailed]=""
      nfailed=$((nfailed + 1))
      ;;
    skipped) skipped="${skipped}${skipped:+, }\`${name}\`" ;;
  esac
done <<< "${rows}"
log "run ${run} attempt ${attempt}: ${nfailed} failed 'full / ' job(s)"

# Writes: printed instead of sent in a dry run.
write() { # <what it does, for the dry-run line> <gh args...>
  local what="$1"
  shift
  if [ "${dry}" = "true" ]; then
    echo "dry run: would ${what}"
    return 0
  fi
  gh "$@" < /dev/null || die "gh $1 $2 failed"
}
show_body() { # <file>: the text a dry run would have written
  if [ "${dry}" = "true" ]; then
    sed 's/^/  | /' "$1"
  fi
}

# The open nightly-red issue (the oldest, should there be more than one):
# sets issue (empty when none) and issue_body.
load_issue() {
  local issues
  if ! issues="$(gh issue list --label "${LABEL}" --state open --json number,body --limit 5 < /dev/null)"; then
    die "the issue list call failed"
  fi
  issue="$(printf '%s' "${issues}" | jq -r 'sort_by(.number) | .[0].number // empty' 2> /dev/null)" \
    || die "the issue list answer is unusable"
  issue_body="$(printf '%s' "${issues}" | jq -r 'sort_by(.number) | .[0].body // ""')"
}

if [ "${nfailed}" -eq 0 ]; then
  load_issue
  if [ -z "${issue}" ]; then
    log "every 'full / ' job is green or skipped and no ${LABEL} issue is open"
    exit 0
  fi
  listed="$(printf '%s\n' "${issue_body}" | sed -n 's/^<!-- nightly-red-job: \(.*\) -->$/\1/p')"
  if [ -z "${listed}" ]; then
    log "issue #${issue} names no job (no marker line): left alone"
    exit 0
  fi
  not_green=""
  while IFS= read -r name; do
    [ -n "${name}" ] || continue
    if ! printf '%s\n' "${greens}" | grep -Fxq -- "${name}"; then
      not_green="${not_green}${not_green:+, }\`${name}\`"
    fi
  done <<< "${listed}"
  note="${TMP}/comment.md"
  if [ "${attempt}" -gt 1 ]; then
    {
      echo "Run [${run}](${run_url}), re-run attempt ${attempt}, has no failed \`full / \` job at \`${sha}\`."
      echo "A re-run that goes green is a flake signal, not evidence of a fix, so this issue stays open."
      [ -z "${not_green}" ] || echo "Not green in this attempt: ${not_green}."
    } > "${note}"
    write "comment on issue #${issue}" issue comment "${issue}" --body-file "${note}"
    show_body "${note}"
    exit 0
  fi
  if [ -n "${not_green}" ]; then
    log "issue #${issue} stays open: not green in run ${run}: ${not_green}"
    exit 0
  fi
  echo "Every listed job is green in run [${run}](${run_url}) at \`${sha}\`: closing." > "${note}"
  write "comment on issue #${issue}" issue comment "${issue}" --body-file "${note}"
  show_body "${note}"
  write "close issue #${issue}" issue close "${issue}"
  exit 0
fi

# Failing targets, one log fetch per failed job.
for ((i = 0; i < nfailed; i++)); do
  logf="${TMP}/log-${i}"
  f_note[i]=""
  f_whole[i]=""
  if ! gh run view "${run}" --job "${f_id[i]}" --log-failed \
      < /dev/null > "${logf}" 2> "${TMP}/err"; then
    log "gh run view failed for job ${f_id[i]}: $(head -c 300 "${TMP}/err"); trying the job log API"
    # A job log carries ANSI colour codes, which gh api refuses to print
    # without --allow-escape-sequences; its short refusal is no log either.
    if ! gh api --allow-escape-sequences "repos/${repo}/actions/jobs/${f_id[i]}/logs" \
        < /dev/null > "${logf}" 2> "${TMP}/err" \
        || { [ "$(wc -c < "${logf}")" -lt 512 ] \
             && grep -q 'pass --allow-escape-sequences' "${logf}" "${TMP}/err"; }; then
      echo "::warning::no log for job ${f_id[i]} (${f_name[i]}): $(head -c 300 "${TMP}/err")" >&2
      : > "${logf}"
      f_note[i]=" (log not available)"
    else
      f_whole[i]="from the whole job log"
    fi
  fi
  bash "${HERE}/failing_targets.sh" --mode test "${logf}" > "${TMP}/targets-${i}"
done

# Last green per failed job: completed runs of both callers of the full tier,
# newest first, without this run and anything created after it. The cutoff is
# the run's own creation time, which every re-run attempt keeps (its jobs get
# new times).
if ! cutoff="$(gh api "repos/${repo}/actions/runs/${run}" --jq .created_at < /dev/null)" \
    || [ -z "${cutoff}" ] || [ "${cutoff}" = "null" ]; then
  die "the run API call for run ${run} failed or named no created_at"
fi
# A runs answer is a few hundred KB, more than one argument may carry, so
# the answers stay in files: each is cut down to the fields read below and
# appended to ${TMP}/hist as one array a line.
hist="${TMP}/hist"
: > "${hist}"
for src in "nightly.yml|" "ci.yml|&event=workflow_dispatch" "ci.yml|&event=push"; do
  wf="${src%%|*}"
  if ! gh api "repos/${repo}/actions/workflows/${wf}/runs?branch=${branch}&status=completed${src#*|}&per_page=20" \
      < /dev/null > "${TMP}/runs"; then
    die "the runs API call for ${wf}${src#*|} failed"
  fi
  jq -c -n '[ (input.workflow_runs // error("no runs"))[]
              | { id, event, head_sha, html_url, created_at } ]' "${TMP}/runs" \
      >> "${hist}" 2> "${TMP}/err" \
    || die "the runs API answer for ${wf}${src#*|} is unusable: $(head -c 300 "${TMP}/err")"
done
candidates="$(jq -r -s --arg run "${run}" --arg cut "${cutoff}" '
  [ add[] | select(((.id // "") | tostring | test("^[0-9]+$")) and (.id | tostring) != $run
                 and (.created_at // "") < $cut) ]
  | unique_by(.id) | sort_by(.created_at) | reverse | .[]
  | [ (.id | tostring), (.event // "unknown"), (.head_sha // ""), (.html_url // "") ] | @tsv' "${hist}")"

remaining="${nfailed}"
lookups=0
calls=0
ceiling_hit=0
while IFS="$(printf '\t')" read -r hid hevent hsha hurl; do
  [ -n "${hid}" ] || continue
  [ "${remaining}" -gt 0 ] || break
  [ "${lookups}" -lt "${max}" ] || break
  if [ "${calls}" -ge "${ceiling}" ]; then
    ceiling_hit=1
    log "API call ceiling (${ceiling}) reached after ${lookups} full-tier run(s)"
    break
  fi
  calls=$((calls + 1))
  hjobs="$(run_jobs "${hid}")" || exit 1
  has_full="$(printf '%s' "${hjobs}" | jq -r 'any(.[]; (.name // "") | startswith("full / "))')"
  if [ "${has_full}" != "true" ]; then
    continue
  fi
  lookups=$((lookups + 1))
  hgreens="$(green_names "${hjobs}")"
  for ((i = 0; i < nfailed; i++)); do
    [ -z "${g_run[i]}" ] || continue
    if printf '%s\n' "${hgreens}" | grep -Fxq -- "${f_name[i]}"; then
      g_run[i]="${hid}"
      g_event[i]="${hevent}"
      g_sha[i]="${hsha}"
      g_url[i]="${hurl:-${server}/${repo}/actions/runs/${hid}}"
      remaining=$((remaining - 1))
      log "last green of '${f_name[i]}': run ${hid} (${hevent}, ${hsha})"
    fi
  done
done <<< "${candidates}"

# The report.
report="${TMP}/report.md"
{
  echo "The scheduled full tier is red in run [${run}](${run_url}) (attempt ${attempt}) at \`${sha}\`."
  for ((i = 0; i < nfailed; i++)); do
    echo
    echo "### ${f_name[i]}"
    echo "<!-- nightly-red-job: ${f_name[i]} -->"
    if [ -n "${f_url[i]}" ]; then
      echo "- Job: [${f_name[i]}](${f_url[i]}), ${f_detail[i]}"
    else
      echo "- Job: ${f_name[i]}, ${f_detail[i]}"
    fi
    echo "- SHA: \`${sha}\`"
    tf="${TMP}/targets-${i}"
    if [ ! -s "${tf}" ]; then
      echo "- Failing targets: none extracted${f_note[i]}${f_whole[i]:+ (${f_whole[i]})}"
    else
      tags="${f_whole[i]}"
      if ! cut -f2 "${tf}" | grep -vqx 'test-timeout'; then
        tags="timeout-only${tags:+, ${tags}}"
      fi
      echo "- Failing targets${tags:+ (${tags})}:"
      while IFS="$(printf '\t')" read -r label kind; do
        echo "  - \`${label}\` (${kind})"
      done < "${tf}"
    fi
    if [ -z "${g_run[i]}" ]; then
      if [ "${lookups}" -eq 1 ]; then
        why="no green within 1 completed full-tier run"
      else
        why="no green within ${lookups} completed full-tier runs"
      fi
      [ "${ceiling_hit}" = 0 ] || why="${why} (API call ceiling reached)"
      echo "- Last green: ${why}"
    else
      if git merge-base --is-ancestor "${g_sha[i]}" "${sha}" 2> /dev/null \
          && count="$(git rev-list --count "${g_sha[i]}..${sha}" 2> /dev/null)"; then
        if [ "${count}" = "1" ]; then
          since="1 commit since"
        else
          since="${count} commits since"
        fi
      else
        since="commit count unknown (not an ancestor in this checkout)"
      fi
      echo "- Last green: [run ${g_run[i]}](${g_url[i]}) (${g_event[i]}, \`${g_sha[i]}\`), ${since}"
    fi
  done
  echo
  echo "Skipped (neither failed nor green): ${skipped:-none}"
} > "${report}"

load_issue
if [ -n "${issue}" ]; then
  write "comment on issue #${issue}" issue comment "${issue}" --body-file "${report}"
  write "edit issue #${issue}" issue edit "${issue}" --body-file "${report}"
else
  if [ "${dry}" = "true" ]; then
    echo "dry run: would create label ${LABEL}"
  else
    gh label create "${LABEL}" --color B60205 --description 'The scheduled full tier is red' \
      < /dev/null > /dev/null 2>&1 || log "label ${LABEL} not created (it may exist already)"
  fi
  write "create issue '${TITLE}' labelled ${LABEL}" issue create --title "${TITLE}" --label "${LABEL}" --body-file "${report}"
fi
show_body "${report}"
exit 0
