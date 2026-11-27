#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2024-2026 We-Amp B.V.
#
# Cancel workflow runs that have been waiting for a runner for too long.
#
# A job that asks for a runner label nothing serves (a fleet class while the
# fleet is off, a retired label) queues until GitHub gives up, after days.
# This sweep finds such runs and cancels them, so the checks they hold do not
# sit pending forever.
#
# A run is cancelled when ALL of these hold:
#   * its status is "queued" or "waiting";
#   * its latest attempt started more than MAX_AGE minutes ago (default 30;
#     "run_started_at", which a re-run resets, falling back to "created_at",
#     which a re-run keeps, only when run_started_at is empty). Measuring from
#     created_at would cancel a legitimate re-run of an old run before any
#     runner could pick it up;
#   * none of its jobs has a runner assigned;
#   * its workflow name is not listed in SWEEP_SKIP_WORKFLOWS (a
#     space-separated list of names, default "Release"): a run that waits on
#     a manual approval looks the same from the outside and must be left to
#     the person who owns the approval.
#
# Usage: tools/ci/sweep-queued-runs.sh [--dry-run] [--max-age MINUTES]
#
# Environment:
#   SWEEP_MAX_AGE_MINUTES default for --max-age (the workflow passes the
#                        repository variable of the same name; default 30).
#                        Raise it when a busy runner class legitimately keeps
#                        whole runs queued for longer.
#   GH_TOKEN             token with actions: write (read-only with --dry-run)
#   GITHUB_REPOSITORY    owner/name (set by Actions)
#   SWEEP_SKIP_WORKFLOWS see above
#   SWEEP_NOW_EPOCH      override "now" (seconds since the epoch; for tests)
#
# One log line per run examined that matters: "cancelled run <id> ..." (or
# "would cancel run <id> ..." with --dry-run). Only the first 100 runs of each
# status are examined; the sweep runs hourly, so a longer queue drains over
# several passes.
#
# Needs gh and jq.
set -euo pipefail

DRY_RUN=0
MAX_AGE="${SWEEP_MAX_AGE_MINUTES:-30}"
while [ $# -gt 0 ]; do
  case "$1" in
    --dry-run) DRY_RUN=1 ;;
    --max-age)
      shift
      MAX_AGE="${1:-}"
      ;;
    -h|--help)
      sed -n '5,42p' "$0" | sed 's/^# \{0,1\}//'
      exit 0
      ;;
    *)
      echo "usage: $0 [--dry-run] [--max-age MINUTES]" >&2
      exit 2
      ;;
  esac
  shift
done

case "$MAX_AGE" in
  ''|*[!0-9]*) echo "::error::the max age (--max-age or SWEEP_MAX_AGE_MINUTES) needs a whole number of minutes" >&2; exit 2 ;;
esac
REPO="${GITHUB_REPOSITORY:?GITHUB_REPOSITORY must be set}"
SKIP="${SWEEP_SKIP_WORKFLOWS-Release}"
NOW="${SWEEP_NOW_EPOCH:-$(date -u +%s)}"

cancelled=0
examined=0

for status in queued waiting; do
  runs="$(gh api "repos/${REPO}/actions/runs?status=${status}&per_page=100")"
  # id, workflow name, age in minutes, one per line, oldest first.
  rows="$(printf '%s' "$runs" | jq -r --argjson now "$NOW" --argjson max "$MAX_AGE" '
    [.workflow_runs[]?
      | {id, name, age: (($now - ((if (.run_started_at // "") != "" then .run_started_at else .created_at end) | fromdateiso8601)) / 60 | floor)}
      | select(.age > $max)]
    | sort_by(-.age)[]
    | [.id, .age, .name] | @tsv')"
  [ -n "$rows" ] || continue
  while IFS=$'\t' read -r id age name; do
    [ -n "$id" ] || continue
    examined=$((examined + 1))
    skipped=0
    for s in $SKIP; do
      if [ "$name" = "$s" ]; then skipped=1; fi
    done
    if [ "$skipped" -eq 1 ]; then
      echo "kept run ${id} (${name}, ${status} ${age} min): workflow is on the skip list"
      continue
    fi
    jobs="$(gh api "repos/${REPO}/actions/runs/${id}/jobs?per_page=100")"
    assigned="$(printf '%s' "$jobs" | jq '[.jobs[]? | select(.runner_id != null)] | length')"
    if [ "$assigned" -gt 0 ]; then
      echo "kept run ${id} (${name}, ${status} ${age} min): a job has a runner"
      continue
    fi
    if [ "$DRY_RUN" -eq 1 ]; then
      echo "would cancel run ${id} (${name}, ${status} ${age} min, no runner assigned)"
    else
      # The run can finish between the listing and the cancel. GitHub then
      # answers "Cannot cancel a workflow run that is completed"; that is not a
      # failure of the sweep, so skip the run. Any other error stays fatal.
      if cancel_out="$(gh run cancel "$id" --repo "$REPO" 2>&1)"; then
        echo "cancelled run ${id} (${name}, ${status} ${age} min, no runner assigned)"
      elif printf '%s' "$cancel_out" | grep -qi 'cannot cancel a workflow run that is completed'; then
        echo "kept run ${id} (${name}, ${status} ${age} min): already completed, skipped"
        continue
      else
        printf '%s\n' "$cancel_out" >&2
        exit 1
      fi
    fi
    cancelled=$((cancelled + 1))
  done <<EOT
$rows
EOT
done

verb="cancelled"
if [ "$DRY_RUN" -eq 1 ]; then verb="would cancel"; fi
echo "sweep done: examined ${examined} stale run(s), ${verb} ${cancelled}"
