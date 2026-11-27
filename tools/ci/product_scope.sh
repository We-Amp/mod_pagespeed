#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2024-2026 We-Amp B.V.
#
# Product scope of a change in a repository that holds two Bazel workspaces:
# the module at the root and the optimizer daemon under optimizer/.
# Experiment only.
#
# Usage: product_scope.sh BASE [HEAD]   (HEAD defaults to HEAD)
#
# Stdout: exactly three lines, `module=<true|false>`, `daemon=<true|false>`
# and `website=<true|false>`. One `product scope:` line with the reason goes
# to stderr. The exit code is always 0.
#
# Every path of `git diff BASE...HEAD` falls in exactly one class:
#   website  optimizer/website/
#   docs     the docs allowlists of both trees (no pipeline)
#   daemon   anything else under optimizer/
#   shared   .github/ and this script (both pipelines)
#   module   everything else
# A missing or unknown BASE or HEAD, a failed diff and an empty diff print
# true three times: anything unexpected runs everything.

set -uo pipefail

out() {
  printf 'module=%s\ndaemon=%s\nwebsite=%s\n' "$1" "$2" "$3"
  echo "product scope: module=$1 daemon=$2 website=$3 ($4)" >&2
  exit 0
}

base="${1:-}"
head="${2:-HEAD}"
[ -n "${base}" ] || out true true true "no base commit"
git rev-parse -q --verify "${base}^{commit}" >/dev/null 2>&1 || out true true true "base ${base} unknown"
git rev-parse -q --verify "${head}^{commit}" >/dev/null 2>&1 || out true true true "head ${head} unknown"
changed="$(git diff --no-renames --name-only "${base}...${head}" 2>/dev/null)" || out true true true "diff failed"
[ -n "${changed}" ] || out true true true "empty diff"

module=false
daemon=false
website=false
docs=0
n=0
while IFS= read -r path; do
  n=$((n + 1))
  case "${path}" in
    optimizer/website/*) website=true ;;
    optimizer/docs/*|optimizer/changelog.d/*) docs=$((docs + 1)) ;;
    optimizer/*/*) daemon=true ;;
    optimizer/*.md) docs=$((docs + 1)) ;;
    optimizer/*) daemon=true ;;
    .github/ISSUE_TEMPLATE/*|.github/pull_request_template.md) docs=$((docs + 1)) ;;
    .github/*|tools/ci/product_scope.sh) module=true; daemon=true ;;
    docs/*|changelog.d/*) docs=$((docs + 1)) ;;
    */*) module=true ;;
    *.md) docs=$((docs + 1)) ;;
    *) module=true ;;
  esac
done <<< "${changed}"
out "${module}" "${daemon}" "${website}" "${n} path(s) changed, ${docs} docs-only"
