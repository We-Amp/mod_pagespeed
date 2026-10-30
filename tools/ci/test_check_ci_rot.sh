#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2024-2026 We-Amp B.V.
#
# Fixture test for tools/ci/check_ci_rot.py: each case builds a tiny tree
# under a temporary directory and states whether the check must pass or fail.
#
# Run: tools/ci/test_check_ci_rot.sh

set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
CHECK="${HERE}/check_ci_rot.py"
WORK="$(mktemp -d)"
trap 'rm -rf "${WORK}"' EXIT
FAILS=0

# case <name> <expect: pass|fail> <workflow body (the jobs: section)> [extra test script name]
case_run() {
  local name="$1" want="$2" body="$3" extra="${4:-}" root="${WORK}/${1}" rc=0
  mkdir -p "${root}/.github/workflows" "${root}/tools/ci"
  printf 'name: t\non: push\njobs:\n%s\n' "${body}" > "${root}/.github/workflows/t.yml"
  : > "${root}/tools/ci/test_a.sh"
  [ -z "${extra}" ] || : > "${root}/tools/ci/${extra}"
  python3 "${CHECK}" --root "${root}" > "${root}/out" 2>&1 || rc=$?
  if { [ "${want}" = pass ] && [ "${rc}" = 0 ]; } || { [ "${want}" = fail ] && [ "${rc}" = 1 ]; }; then
    echo "PASS: ${name}"
  else
    echo "FAIL: ${name} (want ${want}, rc=${rc})"; cat "${root}/out"; FAILS=$((FAILS + 1))
  fi
}

J='  t:
    runs-on: ubuntu-latest
    steps:
      - name: Self-test
'
case_run plain-step-passes pass "${J}        run: tools/ci/test_a.sh"
case_run env-prefix-passes pass "${J}        run: DEFAULT_BRANCH=main bash tools/ci/test_a.sh"
case_run and-chain-passes pass "${J}        run: tools/ci/test_a.sh --selftest && tools/ci/test_a.sh"
case_run or-true-fails fail "${J}        run: tools/ci/test_a.sh || true"
case_run pipe-fails fail "${J}        run: tools/ci/test_a.sh | tee out.log"
case_run condition-fails fail "${J}        run: if tools/ci/test_a.sh; then echo ok; fi"
case_run set-plus-e-fails fail "${J}        run: |
          set +e
          tools/ci/test_a.sh"
case_run step-continue-on-error-fails fail "${J}        continue-on-error: true
        run: tools/ci/test_a.sh"
case_run job-continue-on-error-fails fail "  t:
    runs-on: ubuntu-latest
    continue-on-error: true
    steps:
      - run: tools/ci/test_a.sh"
case_run unregistered-script-fails fail "${J}        run: tools/ci/test_a.sh" test_b.sh
case_run compose-without-name-fails fail "${J}        run: docker compose -f x.yml up -d"
case_run compose-with-run-unique-name-passes pass "${J}        run: echo \"COMPOSE_PROJECT_NAME=x-\${GITHUB_RUN_ID}\" >> \"\$GITHUB_ENV\"
      - name: Stack
        run: docker compose -f x.yml up -d
      - run: tools/ci/test_a.sh"
case_run compose-fixed-name-fails fail "${J}        run: echo \"COMPOSE_PROJECT_NAME=x\" >> \"\$GITHUB_ENV\"
      - run: docker compose -f x.yml up -d"
case_run compose-name-after-use-fails fail "${J}        run: docker compose -f x.yml up -d
      - run: echo \"COMPOSE_PROJECT_NAME=x-\${GITHUB_RUN_ID}\" >> \"\$GITHUB_ENV\""

if [ "${FAILS}" -ne 0 ]; then
  echo "${FAILS} case(s) failed"
  exit 1
fi
echo "All check_ci_rot cases passed"
