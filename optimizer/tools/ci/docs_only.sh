#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2024-2026 We-Amp B.V.
#
# Docs-only classifier of the docs lane (docs/ci/pipelines.md, "DOCS_LANE").
#
# Usage: docs_only.sh BASE [HEAD]   (HEAD defaults to HEAD)
#
# Stdout: exactly one line, `docs_only=true` or `docs_only=false`. One
# `docs lane:` line with the reason goes to stderr. The exit code is always 0.
#
# A change is docs-only when EVERY path in `git diff BASE...HEAD` matches
# DOCS_ALLOW below. The list is an allowlist: a path it does not name, a
# missing or unknown BASE or HEAD, a failed diff and an empty diff all print
# false. Renames are not detected, so a move out of a code path still lists
# the code path it left.
#
# pr_tier_scope.sh reads this file from the default branch, never from the
# tree under test, so a pull request cannot widen its own lane. Every path
# here is one that no build, test or fleet job reads; a change to the list is
# a pipeline change (tools/ci/ is never docs-only).

set -uo pipefail

# The one line that differs between the product repositories.
DOCS_ALLOW='^(website/|docs/|[^/]+\.md$|\.github/(ISSUE_TEMPLATE/|pull_request_template\.md$))'

out() {
  echo "docs_only=$1"
  echo "docs lane: docs_only=$1 ($2)" >&2
  exit 0
}

base="${1:-}"
head="${2:-HEAD}"
[ -n "${base}" ] || out false "no base commit"
git rev-parse -q --verify "${base}^{commit}" >/dev/null 2>&1 || out false "base ${base} unknown"
git rev-parse -q --verify "${head}^{commit}" >/dev/null 2>&1 || out false "head ${head} unknown"
changed="$(git diff --no-renames --name-only "${base}...${head}" 2>/dev/null)" || out false "diff failed"
[ -n "${changed}" ] || out false "empty diff"
n="$(printf '%s\n' "${changed}" | wc -l | tr -d ' ')"
# A here-string, not a pipe: under pipefail a writer killed by SIGPIPE would
# turn the result around.
bad="$(grep -vE "${DOCS_ALLOW}" <<< "${changed}" | head -n 1)"
[ -z "${bad}" ] || out false "non-docs path ${bad}, ${n} path(s) changed"
out true "${n} docs path(s)"
