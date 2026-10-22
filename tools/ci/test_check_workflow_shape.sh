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

expect_good() { # <fixture-dir>
  local dir="$1" out
  if out=$(bash "$CHECK" "${FIX}/${dir}" 2>&1); then
    pass "${dir} passes"
  else
    fail "${dir} rejected: $out"
  fi
}

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
expect_bad bad-caller-full-ref 'rule b'
expect_bad bad-local-uses 'rule c'
expect_bad bad-fleet-literal 'rule d'
expect_bad bad-routing-drift 'rule d'
expect_bad bad-hosted-label 'rule d'
expect_bad bad-guard-missing 'fleet guard'
expect_bad bad-guard-drift 'fleet guard'
expect_bad bad-guard-path-drift 'fleet guard'
expect_bad bad-guard-only-drift 'fleet guard'
expect_bad bad-guard-iis-only-term 'fleet-iis guard'
expect_bad bad-guard-iis-no-fork 'fleet-iis guard'
expect_bad bad-guard-iis-class 'fleet-iis'
expect_bad bad-guard-iis-path-drift 'fleet-iis guard'
expect_bad bad-guard-hyperv-class 'fleet-hyperv job'
expect_bad bad-switch-guard 'switch-routed job'
expect_bad bad-switch-drift 'rule d'
expect_bad bad-switch-iis-class 'rule d'
expect_bad bad-switch-path-drift 'switch-routed job'
expect_bad bad-guard-only-switch-drift 'fleet guard'
expect_bad bad-guard-status-drift 'fleet guard'
expect_bad bad-guard-status-missing 'fleet guard'
expect_bad bad-write-fleet 'rule f'
expect_bad bad-write-workflow 'rule f'
expect_bad bad-no-top-perms 'no top-level permissions'
expect_bad bad-write-quoted 'rule f'
expect_bad bad-write-single-quoted 'rule f'
expect_bad bad-write-flow 'rule f'
expect_bad bad-multiline-runson 'multi-line runs-on'
expect_bad bad-env-fleet 'rule g'
expect_bad bad-secret-fleet 'rule g'
expect_bad bad-secret-toplevel-env 'rule g'
expect_bad bad-secret-index 'rule g'
expect_bad bad-secret-tojson 'rule g'
expect_bad bad-secret-inherit 'rule g'
expect_bad bad-secret-uses-job 'rule g'
expect_good good-queue-guard
expect_bad bad-queue-guard-from-tree 'queue-guard-reads-default-branch'
expect_bad bad-queue-guard-extra-command 'queue-guard-reads-default-branch'
expect_bad bad-queue-guard-trailing-text 'queue-guard-reads-default-branch'
expect_bad bad-queue-guard-trailing-comment 'queue-guard-reads-default-branch'
expect_bad bad-queue-guard-interpolated 'queue-guard-reads-default-branch'
expect_bad bad-queue-guard-other-read 'queue-guard-reads-default-branch'
expect_bad bad-queue-guard-truncated-name 'queue-guard-reads-default-branch'
expect_bad bad-queue-guard-glob-read 'queue-guard-reads-default-branch'
expect_bad bad-queue-guard-brace-read 'queue-guard-reads-default-branch'
expect_bad bad-queue-guard-find-read 'queue-guard-reads-default-branch'
expect_bad bad-queue-guard-dot-source 'queue-guard-reads-default-branch'
expect_bad bad-queue-guard-github-env 'queue-guard-reads-default-branch'
expect_bad bad-queue-guard-absent 'queue-guard-reads-default-branch'
expect_bad bad-queue-guard-renamed 'queue-guard-reads-default-branch'
expect_bad bad-queue-guard-wrong-job 'queue-guard-reads-default-branch'
expect_bad bad-queue-guard-continue-on-error 'queue-guard-reads-default-branch'
expect_bad bad-queue-guard-quoted-key 'queue-guard-reads-default-branch'
expect_bad bad-queue-guard-quoted-shell 'queue-guard-reads-default-branch'
expect_bad bad-queue-guard-wrong-if 'queue-guard-reads-default-branch'
expect_bad bad-queue-guard-not-after-checkout 'queue-guard-reads-default-branch'
expect_bad bad-queue-guard-checkout-env 'queue-guard-reads-default-branch'
expect_bad bad-queue-guard-job-env 'queue-guard-reads-default-branch'
expect_bad bad-queue-guard-step-env-extra 'queue-guard-reads-default-branch'
expect_bad bad-queue-guard-path-env 'queue-guard-reads-default-branch'
expect_bad bad-queue-guard-bash-env-top 'queue-guard-reads-default-branch'
expect_bad bad-queue-guard-git-config-job 'queue-guard-reads-default-branch'
expect_bad bad-queue-guard-vendor-no-needs 'queue-guard-reads-default-branch'
expect_bad bad-queue-guard-vendor-if 'queue-guard-reads-default-branch'
expect_bad bad-queue-guard-vendor-missing 'queue-guard-reads-default-branch'
expect_bad bad-queue-guard-push-step-missing 'queue-guard-reads-default-branch'
expect_bad bad-queue-guard-push-step-renamed 'queue-guard-reads-default-branch'
expect_bad bad-queue-guard-push-step-extra-command 'queue-guard-reads-default-branch'
expect_bad bad-queue-guard-push-step-wide 'queue-guard-reads-default-branch'
expect_bad bad-queue-guard-dispatch-step-missing 'queue-guard-reads-default-branch'
expect_bad bad-queue-guard-dispatch-step-extra-command 'queue-guard-reads-default-branch'
expect_bad bad-queue-guard-steps-misordered 'queue-guard-reads-default-branch'
expect_bad bad-event-guard-missing 'event-guard'
expect_bad bad-event-guard-unreached 'event-guard'
expect_bad bad-event-guard-extra-command 'event-guard'
expect_bad bad-event-guard-allowed 'event-guard'
expect_bad bad-event-guard-always 'event-guard'
expect_bad bad-event-guard-cancelled 'event-guard'
expect_bad bad-event-guard-refs-widened 'event-guard'
expect_bad bad-event-guard-actor-check-removed 'event-guard'
expect_bad bad-event-guard-queue-ref-check-removed 'event-guard'
expect_bad bad-event-guard-fork-check-removed 'event-guard'
expect_bad bad-event-guard-relay-failure 'event-guard'
expect_bad bad-event-guard-relay-or 'event-guard'
expect_bad bad-event-guard-relay-multiline 'event-guard'
expect_bad bad-token 'rule e'
expect_bad bad-ip 'rule e'
expect_bad bad-home 'rule e'
expect_bad bad-port 'rule e'

# ci-dev-full-input: the good tree's ci-dev caller has a full job that uses
# pipeline-full.yml@ci-dev (accepted by the good-tree case above); the same
# call at a feature ref is rejected (bad-caller-full-ref above).

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
