#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2024-2026 We-Amp B.V.
#
# Test for tools/ci/check-workflow-shape.sh against the fixtures in
# tools/ci/testdata/workflow-shape/ (see the README.md there).
#
# Run: tools/ci/test_check_workflow_shape.sh

set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
CHECK="${HERE}/check-workflow-shape.sh"
FIX="${HERE}/testdata/workflow-shape"

FAILS=0
pass() { echo "PASS: $*"; }
fail() { echo "FAIL: $*"; FAILS=$((FAILS + 1)); }

# The good tree passes.
if out=$(bash "$CHECK" "${FIX}/good" 2>&1); then
  pass "good tree passes"
else
  fail "good tree rejected: $out"
fi

expect_bad() { # <fixture-dir> <message substring>
  local dir="$1" want="$2" out
  if out=$(bash "$CHECK" "${FIX}/${dir}" 2>&1); then
    fail "${dir}: passed unexpectedly"
  elif printf '%s' "$out" | grep -qF "$want"; then
    pass "${dir} rejected (${want})"
  else
    fail "${dir}: failed but without '${want}': $out"
  fi
}

expect_bad bad-pr-target 'rule a'
expect_bad bad-caller-steps 'rule b'
expect_bad bad-caller-fleet 'rule b'
expect_bad bad-caller-expression 'rule b'
expect_bad bad-caller-foreign-uses 'rule b'
expect_bad bad-local-uses 'rule c'
expect_bad bad-fleet-literal 'rule d'
expect_bad bad-routing-drift 'rule d'
expect_bad bad-hosted-label 'rule d'
expect_bad bad-guard-missing 'fleet guard'
expect_bad bad-guard-drift 'fleet guard'
expect_bad bad-guard-path-drift 'fleet guard'
expect_bad bad-guard-only-drift 'fleet guard'
expect_bad bad-write-fleet 'rule f'
expect_bad bad-write-workflow 'rule f'
expect_bad bad-no-top-perms 'no top-level permissions'
expect_bad bad-write-quoted 'rule f'
expect_bad bad-write-single-quoted 'rule f'
expect_bad bad-write-flow 'rule f'
expect_bad bad-multiline-runson 'multi-line runs-on'
expect_bad bad-token 'rule e'
expect_bad bad-ip 'rule e'
expect_bad bad-home 'rule e'
expect_bad bad-port 'rule e'

# The repository itself passes (self-test).
REPO_ROOT="$(cd "${HERE}/../.." && pwd)"
if out=$(bash "$CHECK" "${REPO_ROOT}" 2>&1); then
  pass "repository tree passes"
else
  fail "repository tree rejected: $out"
fi

if [ "$FAILS" -ne 0 ]; then
  echo "$FAILS case(s) failed"
  exit 1
fi
echo "All workflow-shape gate cases passed"
