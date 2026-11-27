#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2024-2026 We-Amp B.V.
#
# Expand a PR's changed files into the set of translation units clang-tidy must
# lint.
#
# A changed .cc is its own TU. A changed header is not a TU at all, so it is
# resolved to the .cc files that include it, transitively, since a header may
# reach a .cc only through other headers. Without the transitive step a change
# to a widely-included base header would lint almost nothing.
#
# Run inside the lint container with cwd = the source tree. Prints one path per
# line on stdout, sorted and de-duplicated; prints nothing when the scope is
# empty. The caller must treat no output as "skip clang-tidy", never as "lint
# everything".
#
# Usage: lint-scope-expand.sh <changed-cc-list> <changed-header-list> [compdb]
#
# Includes in this tree are repo-relative and quoted
# (#include "lib/net/ssrf_guard.h"), so a fixed-string grep for the quoted
# path is exact: no basename ambiguity between same-named headers in different
# directories. A repo header spelled with angle brackets is matched too. Bare
# quoted includes (#include "png.h") name third-party headers and never match
# a repo-relative path.
#
# When a compilation database path is given, the result is intersected with its
# "file" entries. A TU absent from the database would lint nothing and exit 0,
# a false green, so it is dropped here rather than handed to run-clang-tidy;
# the dropped paths are listed on stderr. The caller decides what a dropped
# changed .cc means (the lint job fails on it). Exit status 2 means the given
# database holds no lib/, src/ or test/ entries at all (an unreadable or
# drifted format), which must never be read as an empty scope.
#
# No mapfile and no `declare -A`: this must also run under the macOS system
# bash 3.2 that the local reproduction path uses.
set -uo pipefail

CC_LIST="${1:?changed .cc list required}"
HDR_LIST="${2:?changed header list required}"
COMPDB="${3:-}"
ROOTS="lib src test"

WORK=$(mktemp -d)
trap 'rm -rf "${WORK}"' EXIT

# grep exits 1 on "no match", which is a normal result here, not an error.
# Never let it terminate the script or poison the pipeline status.
includers() {
  # shellcheck disable=SC2086  # ROOTS is a deliberate word list
  grep -rlF --include="$2" -e "\"$1\"" -e "<$1>" ${ROOTS} 2>/dev/null || true
}

: > "${WORK}/closure"
: > "${WORK}/frontier"
[ -s "${HDR_LIST}" ] && sort -u "${HDR_LIST}" > "${WORK}/frontier"

# Fixpoint over headers: a changed header pulls in the headers that include it,
# which pull in theirs, until nothing new appears. Bounded by the header count.
while [ -s "${WORK}/frontier" ]; do
  : > "${WORK}/next"
  while read -r hdr; do
    [ -z "${hdr}" ] && continue
    grep -qxF "${hdr}" "${WORK}/closure" 2>/dev/null && continue
    printf '%s\n' "${hdr}" >> "${WORK}/closure"
    includers "${hdr}" "*.h"   >> "${WORK}/next"
    includers "${hdr}" "*.hpp" >> "${WORK}/next"
  done < "${WORK}/frontier"
  sort -u "${WORK}/next" -o "${WORK}/next"
  mv "${WORK}/next" "${WORK}/frontier"
done

# Seed with the changed .cc files (each is its own TU), then add every .cc that
# includes any header in the closure.
: > "${WORK}/targets"
[ -s "${CC_LIST}" ] && cat "${CC_LIST}" >> "${WORK}/targets"
while read -r hdr; do
  [ -z "${hdr}" ] && continue
  includers "${hdr}" "*.cc" >> "${WORK}/targets"
done < "${WORK}/closure"

# Carve-outs that mirror the lint job's changed-.cc filter: the vendored 1.15
# JS and HTML kernel TUs are style-lint exceptions and are never tidied, and
# the nginx and Windows-only TUs never reach the Linux compilation database.
# Keep this list in lockstep with the "Compute lint scope" step.
#
# An empty scope must emit NOTHING, not a blank line: a blank line would reach
# clang-tidy as an empty path argument and silently widen the lint.
grep -v '^[[:space:]]*$' "${WORK}/targets" 2>/dev/null \
  | grep -vE '^lib/js/(js_[a-z_0-9]+\.(h|cc)|source_map\.h|corpus/)' \
  | grep -vE '^lib/html/(content_type|doctype|empty_html_filter|html_element|html_event|html_filter|html_keywords|html_lexer|html_name|html_node|html_parse|html_parse_probe|html_parse_recorder|html_writer_filter)\.(h|cc|gperf)$' \
  | grep -vE '^src/nginx/(early_hints|ngx_pagespeed_module)\.cc$|^src/proto/notification_sender_win\.cc$' \
  | sort -u > "${WORK}/candidates" || true

if [ -n "${COMPDB}" ] && [ -s "${COMPDB}" ]; then
  # Workspace-relative "file" values; tolerate a ./ or /workspace/ prefix.
  grep -oE '"file": *"[^"]+"' "${COMPDB}" 2>/dev/null \
    | sed -E 's/^"file": *"//; s/"$//; s#^\./##; s#^/workspace/##' \
    | sort -u > "${WORK}/compdb-files" || true
  if ! grep -qE '^(lib|src|test)/' "${WORK}/compdb-files"; then
    echo "lint-scope-expand: ${COMPDB} has no lib/, src/ or test/ entries" >&2
    exit 2
  fi
  comm -23 "${WORK}/candidates" "${WORK}/compdb-files" > "${WORK}/dropped" || true
  if [ -s "${WORK}/dropped" ]; then
    echo "lint-scope-expand: $(wc -l < "${WORK}/dropped" | tr -d ' ') TU(s) not in the compilation database, not linted:" >&2
    sed 's/^/  /' "${WORK}/dropped" >&2
  fi
  comm -12 "${WORK}/candidates" "${WORK}/compdb-files" || true
elif [ -n "${COMPDB}" ]; then
  echo "lint-scope-expand: ${COMPDB} is missing or empty" >&2
  exit 2
else
  cat "${WORK}/candidates"
fi
