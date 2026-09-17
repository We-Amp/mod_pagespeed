#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2024-2026 We-Amp B.V.
#
# Surface report-only dependency-scan findings as a single, idempotent GitHub
# issue. Meant to run ONLY on scheduled / manual cron runs —
# the per-PR gates are elsewhere; this turns the otherwise-invisible daily
# report into an actionable, tracked signal.
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
# Needs `gh` + GH_TOKEN with `issues: write`. Set DRY_RUN=1 to echo, not act.
# Optional knobs (defaults keep the original dep-scan wording):
#   [resolve-hint] 4th arg — trailer telling the reader how to resolve.
#   LABEL_DESC  env — description used when the marker label is created.
#   CLEAN_NOTE  env — phrase used in the auto-close comment.
set -uo pipefail

LABEL="${1:?label}"; TITLE="${2:?title}"; BODY_FILE="${3:?findings md file}"
HINT="${4:-Fix the dep or record a justified suppression (\`sbom/*.vex.json\` / \`cve-ignore.yaml\`); this issue auto-closes when the scan is clean.}"
REPO="${GITHUB_REPOSITORY:?GITHUB_REPOSITORY unset}"
RUN_URL="${GITHUB_SERVER_URL:-https://github.com}/${REPO}/actions/runs/${GITHUB_RUN_ID:-}"
NOW="$(date -u +%Y-%m-%dT%H:%M:%SZ)"

gh_() { if [ "${DRY_RUN:-0}" = "1" ]; then echo "DRY: gh $*" >&2; else gh "$@"; fi; }

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
    echo "updated issue #$existing" >&2
  else
    gh_ issue create --repo "$REPO" --title "$TITLE" --label "$LABEL" --body "$body"
    echo "created tracking issue" >&2
  fi
else
  if [ -n "$existing" ]; then
    gh_ issue comment "$existing" --repo "$REPO" --body "✅ ${CLEAN_NOTE:-No medium+ findings} as of ${NOW} ([run](${RUN_URL})). Auto-closing."
    gh_ issue close "$existing" --repo "$REPO"
    echo "closed issue #$existing (clean)" >&2
  else
    echo "clean, no open issue — nothing to do" >&2
  fi
fi
