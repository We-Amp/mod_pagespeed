#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2024-2026 We-Amp B.V.
#
# Test for tools/ci/failing_targets.sh over the fixture logs in
# tools/ci/testdata/failing-targets/: gh-prefixed and raw lines, with ANSI
# colour codes as a job log carries them. They are `.txt` files because the
# tree ignores `*.log`.
#
# Run: tools/ci/test_failing_targets.sh

set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SCRIPT="${HERE}/failing_targets.sh"
DATA="${HERE}/testdata/failing-targets"

WORK="$(mktemp -d)"
trap 'rm -rf "${WORK}"' EXIT

FAILS=0
pass() { echo "PASS: $*"; }
fail() { echo "FAIL: $*"; FAILS=$((FAILS + 1)); }

TAB="$(printf '\t')"

# check <name> <want stdout, lines joined by \n> <args...>
check() {
  local name="$1" want="$2" rc
  shift 2
  set +e
  bash "${SCRIPT}" "$@" > "${WORK}/out" 2> "${WORK}/err"
  rc=$?
  set -e
  if [ "${rc}" -ne 0 ]; then
    fail "${name}: exit ${rc}: $(cat "${WORK}/err")"
    return
  fi
  if [ "$(cat "${WORK}/out")" != "$(printf '%b' "${want}")" ]; then
    fail "${name}: stdout '$(cat "${WORK}/out")', want '$(printf '%b' "${want}")'"
    return
  fi
  pass "${name}"
}

check "test-failed-and-timeout" \
  "//a:b_test${TAB}test-failed\n//a:c_test${TAB}test-timeout" \
  --mode test "${DATA}/test-failed-and-timeout.txt"
check "flaky-passed" "" --mode test "${DATA}/flaky-passed.txt"
check "build-error-test-mode" \
  "//src:lib${TAB}build-failed\n//src:lib_test${TAB}build-failed\n//src:other_test${TAB}build-failed" \
  --mode test "${DATA}/build-error.txt"
check "build-error-build-mode" \
  "//src:lib${TAB}build-failed\n//src:lib_test${TAB}build-failed\n//src:other_test${TAB}build-failed" \
  --mode build "${DATA}/build-error.txt"
check "test-mode-build-only" "" --mode build "${DATA}/test-failed-and-timeout.txt"
check "duplicates" \
  "//a:b_test${TAB}test-failed\n//a:e_test${TAB}test-failed\n//src:lib${TAB}build-failed" \
  --mode test "${DATA}/duplicates.txt"
check "empty" "" --mode test "${DATA}/empty.txt"

# stdin when no file is named.
set +e
bash "${SCRIPT}" --mode test < "${DATA}/test-failed-and-timeout.txt" > "${WORK}/out" 2>&1
rc=$?
set -e
if [ "${rc}" -eq 0 ] && [ "$(cat "${WORK}/out")" = "$(printf '//a:b_test\ttest-failed\n//a:c_test\ttest-timeout')" ]; then
  pass "reads-stdin"
else
  fail "reads-stdin: exit ${rc}, stdout '$(cat "${WORK}/out")'"
fi

# A Windows job log ends its lines with CR LF (kept out of the fixtures,
# which git normalizes to LF).
printf '//w:x_test    FAILED\r\n//w:y_test    TIMEOUT in 60.0s\r\n' > "${WORK}/crlf.txt"
check "crlf-line-endings" "//w:x_test${TAB}test-failed\n//w:y_test${TAB}test-timeout" \
  --mode test "${WORK}/crlf.txt"

# A usage error is not a silent empty answer.
for args in "--mode" "--mode other" "--bogus"; do
  set +e
  # shellcheck disable=SC2086 # the args are split on purpose
  bash "${SCRIPT}" ${args} < /dev/null > "${WORK}/out" 2>&1
  rc=$?
  set -e
  if [ "${rc}" -eq 2 ]; then
    pass "usage-error '${args}'"
  else
    fail "usage-error '${args}': exit ${rc}"
  fi
done

if [ "${FAILS}" -ne 0 ]; then
  echo "${FAILS} check(s) failed"
  exit 1
fi
echo "all checks passed"
