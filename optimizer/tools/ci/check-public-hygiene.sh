#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2024-2026 We-Amp B.V.
#
# Public-tree hygiene gate (BLOCKING) -- the "Public-tree hygiene gate" CI
# job. This repository is public, so the tracked tree must stay free of
# private-infrastructure and private-process vocabulary: internal
# decision-record references, the former private CI runner label scheme,
# pre-publication development repository slugs, internal work-item
# identifiers and private build-fleet topology words. Direct commits to
# this repository never pass through the private source repos' export-time
# filters, so the classes are re-checked here, against public-safe patterns
# only.
#
# The rules live in .github/hygiene-rules.txt (one "<flags><TAB><ERE>" per
# line; its header documents each class). Every rule is run over the
# tracked tree with git grep; ANY hit fails the job and prints path:line.
#
# Excluded from the scan:
#   * .git -- git grep visits tracked files only, so it is never scanned.
#   * the rules file itself -- it necessarily contains every blocked
#     pattern.
#   * tools/async-css-probe/fixtures/modpagespeed-com/index.html -- a
#     byte-frozen capture of the modpagespeed.com home page (sha256-pinned
#     per file in its CAPTURE.md, so it can be neither edited nor
#     re-captured without breaking the pin). Its recorded marketing copy
#     uses one of the blocked topology words in the product-positioning
#     sense ("runs on your servers"), not the CI-fleet sense the rule
#     targets. This is the ONLY content exclusion; any other hit anywhere
#     in the tree fails.
#
# Usage: tools/ci/check-public-hygiene.sh
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "$REPO_ROOT"

RULES=".github/hygiene-rules.txt"
[[ -f "$RULES" ]] || { echo "::error::${RULES} not found" >&2; exit 1; }

EXCLUDES=(
  ":(exclude)${RULES}"
  # Byte-frozen modpagespeed.com capture; see the header comment.
  ":(exclude)tools/async-css-probe/fixtures/modpagespeed-com/index.html"
)

status=0
lineno=0
while IFS=$'\t' read -r flags pattern || [[ -n "$flags" ]]; do
  lineno=$((lineno + 1))
  case "$flags" in
    ''|\#*) continue ;;
  esac
  if [[ "$flags" != *E* ]]; then
    echo "::error::${RULES}:${lineno}: only extended-regex (E) rules are supported" >&2
    exit 1
  fi
  if [[ -z "$pattern" ]]; then
    echo "::error::${RULES}:${lineno}: rule has flags but no pattern" >&2
    exit 1
  fi
  grep_args=(-n -I -E)
  [[ "$flags" == *i* ]] && grep_args+=(-i)
  set +e
  out="$(git grep "${grep_args[@]}" -e "$pattern" -- . "${EXCLUDES[@]}")"
  rc=$?
  set -e
  if [[ $rc -gt 1 ]]; then
    echo "::error::${RULES}:${lineno}: git grep failed (status ${rc}): ${out}" >&2
    exit 1
  fi
  if [[ $rc -eq 0 ]]; then
    echo "::error::hygiene rule ${lineno} matched (${pattern}):" >&2
    echo "$out" >&2
    status=1
  fi
done < "$RULES"

# Hashed-token rules and the relative-link check (see the .py header).
if ! python3 -I tools/ci/check-public-hygiene.py; then
  status=1
fi

if [[ $status -ne 0 ]]; then
  echo "::error::public-tree hygiene gate FAILED -- see the matched paths above" >&2
  exit 1
fi

echo "Public-tree hygiene gate passed: no rule matched the tracked tree."
