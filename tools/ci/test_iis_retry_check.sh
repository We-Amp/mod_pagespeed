#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2024-2026 We-Amp B.V.
#
# Test for the step "Check the IIS retry after a host failure" of the
# fleet-only-skip-check job in .github/workflows/pipeline-pr.yml: the step's
# script is read from the workflow and run with each combination of the first
# attempt's failure class and the retry's result.
#
# Run: tools/ci/test_iis_retry_check.sh

set -uo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
WF="${HERE}/../../.github/workflows/pipeline-pr.yml"
WORK="$(mktemp -d)"
trap 'rm -rf "${WORK}"' EXIT

FAILS=0
pass() { echo "PASS: $*"; }
fail() { echo "FAIL: $*"; FAILS=$((FAILS + 1)); }

# The run: | block of the step, de-indented.
awk '
  /^      - name: Check the IIS retry after a host failure$/ { found = 1; next }
  found && /^        run: \|$/ { body = 1; next }
  body && /^          / { sub(/^          /, ""); print; next }
  body && /^$/ { print; next }
  body { exit }
' "${WF}" > "${WORK}/step.sh"
# shellcheck disable=SC2016 # literal workflow expressions
if grep -q 'RETRY_RESULT' "${WORK}/step.sh" \
   && awk '/^  fleet-only-skip-check:/{j=1} j && /^    needs:/{print; exit}' "${WF}" | grep -q 'iis-sys-tests-retry' \
   && grep -q 'RETRY_RESULT: ${{ needs.iis-sys-tests-retry.result }}' "${WF}" \
   && grep -q 'IIS_CLASS: ${{ needs.iis-sys-tests.outputs.failure_class }}' "${WF}"; then
  pass "the step exists and reads the retry result and the failure class"
else
  fail "the step or its inputs are missing from fleet-only-skip-check"
fi

case_() { # <class> <retry result> <expected rc>
  local rc=0
  IIS_CLASS="$1" RETRY_RESULT="$2" bash -e "${WORK}/step.sh" > "${WORK}/out" 2>&1 || rc=$?
  if [ "${rc}" -eq "$3" ]; then pass "class='$1' retry=$2 -> exit $3"
  else fail "class='$1' retry=$2: exit ${rc}, want $3: $(cat "${WORK}/out")"; fi
}
case_ infra success 0
case_ infra skipped 1
case_ infra failure 1
case_ infra cancelled 1
case_ '' skipped 0
case_ test skipped 0
case_ '' success 0

if [ "${FAILS}" -ne 0 ]; then
  echo "${FAILS} case(s) failed"
  exit 1
fi
echo "all cases passed"
