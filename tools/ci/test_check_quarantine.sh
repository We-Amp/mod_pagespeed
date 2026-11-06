#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2024-2026 We-Amp B.V.
#
# Test for tools/ci/check_quarantine.py against small workflow trees in a
# temporary directory. No network.
#
# Run: tools/ci/test_check_quarantine.sh

set -uo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SCRIPT="${HERE}/check_quarantine.py"
WORK="$(mktemp -d)"
trap 'rm -rf "${WORK}"' EXIT

FAILS=0
pass() { echo "PASS: $*"; }
fail() { echo "FAIL: $*"; FAILS=$((FAILS + 1)); }

# tree <name> <continue-on-error value or "none"> <registry lines...>
tree() {
  local d="${WORK}/$1" coe="$2"
  shift 2
  mkdir -p "${d}/.github/workflows" "${d}/tools/ci"
  {
    echo "name: pipeline-full"
    echo "on:"
    echo "  workflow_call:"
    echo "jobs:"
    echo "  build:"
    echo "    name: Build"
    echo "    runs-on: ubuntu-latest"
    echo "    steps:"
    echo "      - run: true"
    echo "        continue-on-error: true"
    echo "  win-appverif:"
    echo "    name: Windows AppVerif"
    [ "${coe}" = none ] || echo "    continue-on-error: ${coe}"
    echo "    runs-on: ubuntu-latest"
    echo "    steps:"
    echo "      - run: true"
  } > "${d}/.github/workflows/pipeline-full.yml"
  {
    echo "# workflow|job id|job name|issue|expires"
    for l in "$@"; do echo "${l}"; done
  } > "${d}/tools/ci/quarantine.txt"
  echo "${d}"
}

run() { # <root> <today>
  OUT="$(QUARANTINE_TODAY="$2" "${SCRIPT}" --root "$1" 2>&1)"
  RC=$?
}

ENTRY='pipeline-full.yml|win-appverif|Windows AppVerif|12|2026-11-20'

d="$(tree ok true "${ENTRY}")"
run "${d}" 2026-11-06
if [ "${RC}" -eq 0 ] && grep -q 'Windows AppVerif is report-only until 2026-11-20 (issue #12)' <<<"${OUT}"; then
  pass "a registered report-only job before its expiry passes"
else fail "registered job before expiry: rc=${RC} ${OUT}"; fi

run "${d}" 2026-11-20
if [ "${RC}" -eq 0 ]; then pass "the expiry day itself still passes"
else fail "expiry day: rc=${RC} ${OUT}"; fi

run "${d}" 2026-11-18
if [ "${RC}" -eq 0 ] && grep -q '::warning::' <<<"${OUT}"; then pass "three days before expiry warns"
else fail "near expiry: rc=${RC} ${OUT}"; fi

run "${d}" 2026-11-21
if [ "${RC}" -eq 1 ] && grep -q '::error::.*Windows AppVerif.*expired on 2026-11-20.*#12' <<<"${OUT}"; then
  pass "an expired quarantine fails loudly"
else fail "expired: rc=${RC} ${OUT}"; fi

# shellcheck disable=SC2016 # a literal workflow expression
d="$(tree expr '${{ true }}' "${ENTRY}")"
run "${d}" 2026-11-06
if [ "${RC}" -eq 0 ]; then pass "an expression value counts as report-only"
else fail "expression value: rc=${RC} ${OUT}"; fi

d="$(tree unregistered true)"
run "${d}" 2026-11-06
if [ "${RC}" -eq 1 ] && grep -q '::error::.*win-appverif.*not in tools/ci/quarantine.txt' <<<"${OUT}"; then
  pass "a report-only job without a registry entry fails"
else fail "unregistered: rc=${RC} ${OUT}"; fi

d="$(tree false-ok false)"
run "${d}" 2026-11-06
if [ "${RC}" -eq 0 ]; then pass "continue-on-error: false needs no entry"
else fail "false: rc=${RC} ${OUT}"; fi

d="$(tree stale none "${ENTRY}")"
run "${d}" 2026-11-06
if [ "${RC}" -eq 1 ] && grep -q '::error::.*win-appverif.*is not report-only' <<<"${OUT}"; then
  pass "an entry whose job is no longer report-only fails (remove the entry)"
else fail "stale entry: rc=${RC} ${OUT}"; fi

d="$(tree nojob true "${ENTRY}" 'pipeline-full.yml|gone|Gone|13|2026-11-20')"
run "${d}" 2026-11-06
if [ "${RC}" -eq 1 ] && grep -q '::error::.*job gone not found' <<<"${OUT}"; then
  pass "an entry naming a missing job fails"
else fail "missing job: rc=${RC} ${OUT}"; fi

d="$(tree badname true 'pipeline-full.yml|win-appverif|Windows ASan|12|2026-11-20')"
run "${d}" 2026-11-06
if [ "${RC}" -eq 1 ] && grep -q "::error::.*is named 'Windows AppVerif'" <<<"${OUT}"; then
  pass "an entry whose name does not match the job fails"
else fail "name mismatch: rc=${RC} ${OUT}"; fi

for bad in 'pipeline-full.yml|win-appverif|Windows AppVerif|12|2026-13-40' \
           'pipeline-full.yml|win-appverif|Windows AppVerif|x|2026-11-20' \
           'pipeline-full.yml|win-appverif|Windows AppVerif|12' ; do
  d="$(tree "malformed-${RANDOM}" true "${bad}")"
  run "${d}" 2026-11-06
  if [ "${RC}" -eq 1 ] && grep -q '::error::' <<<"${OUT}"; then pass "malformed entry refused: ${bad}"
  else fail "malformed entry ${bad}: rc=${RC} ${OUT}"; fi
done

d="$(tree toolong true 'pipeline-full.yml|win-appverif|Windows AppVerif|12|2026-12-31')"
run "${d}" 2026-11-06
if [ "${RC}" -eq 1 ] && grep -q '::error::.*more than 28 days' <<<"${OUT}"; then
  pass "an expiry more than 28 days out is refused"
else fail "too long: rc=${RC} ${OUT}"; fi

# The real tree: whatever it registers must hold today.
OUT="$("${SCRIPT}" --root "${HERE}/../.." 2>&1)"; RC=$?
if [ "${RC}" -eq 0 ]; then pass "the repository's own registry holds"
else fail "repository registry: rc=${RC} ${OUT}"; fi

if [ "${FAILS}" -ne 0 ]; then
  echo "${FAILS} case(s) failed"
  exit 1
fi
echo "all cases passed"
