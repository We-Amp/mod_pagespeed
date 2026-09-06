#!/bin/bash

# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2024-2026 We-Amp B.V.

# CI test-placement guard (BLOCKING): every *_test.cc must live under test/.
#
# The CI build/test target globs are
# //test/pagespeed/kernel/... //test/net/... //test/pagespeed/opt/...
# //test/pagespeed/system/... -- i.e. the test/ tree ONLY. A unit test placed
# ALONGSIDE its source (e.g. pagespeed/kernel/<pkg>/foo_test.cc) is linted but
# NEVER compiled or run by any CI job, so it silently rots: a regression it would
# catch ships green. This guard fails the build when a *_test.cc exists outside
# test/, forcing the canonical //test/<mirror>/ placement -- unless the file is
# explicitly allowlisted with a reason.
#
# Fast + hermetic: a single `git ls-files` over the tracked tree, no Bazel
# analysis, no network, no Docker. Mirrors tools/ci/check_doc_commands.sh, kept
# portable to macOS stock bash 3.2 (no mapfile / associative arrays).
#
# Run locally: bash tools/ci/check_tests_in_test_tree.sh

set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "$REPO_ROOT"

ALLOWLIST="tools/ci/tests-outside-test-tree.allow"

WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT

# All tracked *_test.cc NOT under test/. git ls-files is deterministic and
# ignores build outputs / bazel-* symlinks. Sorted for `comm`.
git ls-files '*_test.cc' | grep -v '^test/' | LC_ALL=C sort > "$WORK/offenders" || true

# Normalize the allowlist: strip trailing '#' comments, trim whitespace, drop
# blank lines. One repo-relative path per surviving line. Sorted for `comm`.
if [ -f "$ALLOWLIST" ]; then
  sed -e 's/#.*$//' -e 's/[[:space:]]//g' "$ALLOWLIST" \
    | grep -v '^$' | LC_ALL=C sort -u > "$WORK/allow"
else
  : > "$WORK/allow"
fi

# Violations = offenders not present in the allowlist.
LC_ALL=C comm -23 "$WORK/offenders" "$WORK/allow" > "$WORK/violations"

# Stale allowlist entries = allowlisted paths git no longer tracks outside test/
# (moved into test/, deleted, or renamed). Keeps the allowlist from accruing cruft.
: > "$WORK/stale"
while IFS= read -r path; do
  [ -z "$path" ] && continue
  if ! git ls-files --error-unmatch "$path" >/dev/null 2>&1; then
    printf '%s\n' "$path" >> "$WORK/stale"
  fi
done < "$WORK/allow"

FATAL=0

if [ -s "$WORK/violations" ]; then
  echo "ERROR: the following *_test.cc files live OUTSIDE the test/ tree and are" >&2
  echo "       therefore NEVER compiled or run by CI (the build/test globs cover" >&2
  echo "       //test/... only):" >&2
  sed 's/^/  - /' "$WORK/violations" >&2
  echo "" >&2
  echo "Fix: relocate each to test/<mirror-of-its-package>/ with a pagespeed_cc_test" >&2
  echo "target (load //bazel:pagespeed_test.bzl), leaving the library under its" >&2
  echo "source package. If a test is INTENTIONALLY not under test/ (e.g. a" >&2
  echo "pagespeed_cc_benchmark, or vendored upstream source), add its path to" >&2
  echo "$ALLOWLIST with a one-line reason." >&2
  FATAL=1
fi

if [ -s "$WORK/stale" ]; then
  echo "ERROR: $ALLOWLIST lists path(s) git no longer tracks outside test/ -- prune them:" >&2
  sed 's/^/  - /' "$WORK/stale" >&2
  FATAL=1
fi

if [ "$FATAL" -ne 0 ]; then
  echo "" >&2
  echo "FAILED: test-placement guard (tools/ci/check_tests_in_test_tree.sh)." >&2
  exit 1
fi

echo "OK: every tracked *_test.cc is under test/ or explicitly allowlisted."
exit 0
