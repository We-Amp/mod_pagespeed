#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2024-2026 We-Amp B.V.
#
# Surface report-only dependency-scan findings as a single, idempotent GitHub
# issue (phase A). Meant to act ONLY for runs on the default branch (push /
# schedule / dispatch on main) — the per-PR gates are elsewhere; this turns
# the otherwise-invisible daily report into an actionable, tracked signal.
#
#   post-findings-issue.sh <label> <title> <findings-md-file> [resolve-hint]
#
# Behavior (find-or-create, one open issue per <label>):
#   - findings present  → create the issue (or update its body) with the
#                         findings + a timestamp; the issue's OPEN state means
#                         "unresolved findings exist".
#   - findings empty    → comment "clean" on and CLOSE any existing open issue.
# Idempotent: never opens a second issue, never spams when nothing changed body.
#
# Optional knobs (defaults keep the original dep-scan wording):
#   [resolve-hint] 4th arg — trailer telling the reader how to resolve.
#   LABEL_DESC  env — description used when the marker label is created.
#   CLEAN_NOTE  env — phrase used in the auto-close comment.
#
# Needs `gh` + GH_TOKEN with `issues: write`. DRY_RUN=1 makes every write a
# no-op: the gh calls are echoed instead of made, and the verdict (what the
# live run would have done to which issue) is written to the log and, in
# Actions, to the step summary. The callers set it off the default branch —
# pull_request runs and branch dispatches — so a PR's evidence can never
# close or re-file the tracker. The open-issue lookup is read-only
# and still runs, so the dry-run verdict names the real issue number.
set -uo pipefail

LABEL="${1:?label}"; TITLE="${2:?title}"; BODY_FILE="${3:?findings md file}"
HINT="${4:-Fix the dep or record a justified suppression (\`sbom/*.vex.json\` / \`cve-ignore.yaml\`); this issue auto-closes when the scan is clean.}"
REPO="${GITHUB_REPOSITORY:?GITHUB_REPOSITORY unset}"
RUN_URL="${GITHUB_SERVER_URL:-https://github.com}/${REPO}/actions/runs/${GITHUB_RUN_ID:-}"
NOW="$(date -u +%Y-%m-%dT%H:%M:%SZ)"

dry() { [ "${DRY_RUN:-0}" = "1" ]; }
gh_() { if dry; then echo "DRY: gh $*" >&2; else gh "$@"; fi; }

# verdict <what happened>: the one-line outcome, on the log. In a dry run it
# reads "would <what>" and also lands on the step summary, so a PR run shows
# what the live run on the default branch would have done without doing it.
verdict() {
  if dry; then
    echo "DRY RUN: would $*" >&2
    if [ -n "${GITHUB_STEP_SUMMARY:-}" ]; then
      printf -- '**Findings issue (dry run — not the default branch, nothing touched):** would %s.\n\n' "$*" >>"$GITHUB_STEP_SUMMARY"
    fi
  else
    echo "$*" >&2
  fi
}

# Ensure the marker label exists (idempotent); harmless if it already does.
gh_ label create "$LABEL" --repo "$REPO" --color B60205 \
  --description "${LABEL_DESC:-Automated dependency CVE findings}" --force >/dev/null 2>&1 || true

# At most one open tracking issue per label.
existing="$(gh issue list --repo "$REPO" --label "$LABEL" --state open \
  --json number --jq '.[0].number // empty' 2>/dev/null || true)"

has_findings=0
[ -s "$BODY_FILE" ] && grep -q '[^[:space:]]' "$BODY_FILE" && has_findings=1

if [ "$has_findings" = "1" ]; then
  body="$(cat "$BODY_FILE")

---
_Automated by \`${GITHUB_WORKFLOW:-dep-scan}\` at ${NOW} — [run](${RUN_URL}). ${HINT}_"
  if [ -n "$existing" ]; then
    gh_ issue edit "$existing" --repo "$REPO" --body "$body"
    verdict "update open issue #$existing with the current findings"
  else
    gh_ issue create --repo "$REPO" --title "$TITLE" --label "$LABEL" --body "$body"
    verdict "create the \`$LABEL\` tracking issue (findings present, none open)"
  fi
else
  if [ -n "$existing" ]; then
    gh_ issue comment "$existing" --repo "$REPO" --body "✅ ${CLEAN_NOTE:-No medium+ findings} as of ${NOW} ([run](${RUN_URL})). Auto-closing."
    gh_ issue close "$existing" --repo "$REPO"
    verdict "close issue #$existing (clean)"
  else
    verdict "do nothing (clean, no open \`$LABEL\` issue)"
  fi
fi
