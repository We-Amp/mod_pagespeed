#!/bin/bash
# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2024-2026 We-Amp B.V.

# Expand a PR's changed files into the set of translation units clang-tidy must
# lint.
#
# A changed .cc is its own TU. A changed header is not a TU at all, so it is
# resolved to the .cc files that include it -- transitively, since a header may
# reach a .cc only through other headers. Without the transitive step a change
# to a widely-included base header would lint almost nothing.
#
# Run inside the lint container with cwd = the source tree. Prints one path per
# line on stdout, sorted and de-duplicated; prints nothing when the scope is
# empty (the caller must treat no-output as "skip clang-tidy", never as "lint
# everything").
#
# Usage: lint-scope-expand.sh <changed-cc-list> <changed-header-list>
#
# Includes in this tree are always repo-relative and quoted
# (#include "pagespeed/kernel/base/basictypes.h"), so a fixed-string grep for
# the quoted path is exact -- no basename ambiguity between same-named headers
# in different directories.
#
# No mapfile / no `declare -A`: this must also run under the macOS system bash
# 3.2 that the local reproduction path uses.
set -uo pipefail

CC_LIST="${1:?changed .cc list required}"
HDR_LIST="${2:?changed header list required}"
ROOTS="pagespeed net"

WORK=$(mktemp -d)
trap 'rm -rf "${WORK}"' EXIT

# grep exits 1 on "no match", which is a normal result here, not an error.
# Never let it terminate the script or poison the pipeline status.
includers() {
  grep -rlF --include="$2" "\"$1\"" ${ROOTS} 2>/dev/null || true
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

# Drop TUs that are absent from the compilation database. compdb is generated
# for //pagespeed/... //net/... with --deleted_packages=pagespeed/envoy,
# pagespeed/iis,pagespeed/windows, so a target under those roots matches no
# compdb entry, lints nothing, and exits 0 -- a false green. Keep this list in
# lockstep with BAZEL_BUILD_OPTIONS in the CI workflow's lint job.
#
# An empty scope must emit NOTHING, not a blank line: a blank line would reach
# clang-tidy as an empty path argument and silently widen the lint to the cwd.
grep -v '^[[:space:]]*$' "${WORK}/targets" 2>/dev/null \
  | grep -vE '^pagespeed/(envoy|iis|windows)/' \
  | sort -u || true
