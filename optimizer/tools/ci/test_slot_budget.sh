#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2024-2026 We-Amp B.V.
#
# Self-test for tools/ci/slot-budget.sh: unset keeps the legacy values, valid
# values are used exactly, nonsense fails with a clear message.
set -uo pipefail
HELPER="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/slot-budget.sh"
fail=0
check() { # label expected actual
  if [ "$2" != "$3" ]; then echo "FAIL: $1: expected [$2] got [$3]" >&2; fail=1; else echo "ok: $1"; fi
}
run() { env -u CI_SLOT_CPUS -u CI_SLOT_MEM_GB "$@" bash "${HELPER}" 55g 2>&1 | tr '\n' ' '; }

check "unset"        "JOBS=HOST_CPUS*.75 MEM=55g CPUS= " "$(run)"
check "no legacy"    "JOBS=HOST_CPUS*.75 MEM= CPUS= " "$(env -u CI_SLOT_CPUS -u CI_SLOT_MEM_GB bash "${HELPER}" 2>&1 | tr '\n' ' ')"
check "both set"     "JOBS=12 MEM=20g CPUS=12 " "$(run CI_SLOT_CPUS=12 CI_SLOT_MEM_GB=20)"
check "cpus only"    "JOBS=16 MEM=55g CPUS=16 " "$(run CI_SLOT_CPUS=16)"
check "mem only"     "JOBS=HOST_CPUS*.75 MEM=10g CPUS= " "$(run CI_SLOT_MEM_GB=10)"
check "empty = unset" "JOBS=HOST_CPUS*.75 MEM=55g CPUS= " "$(run CI_SLOT_CPUS= CI_SLOT_MEM_GB=)"
for bad in abc 0 -3 1.5 012 99999 "4 5"; do
  out="$(run CI_SLOT_CPUS="${bad}")"
  case "${out}" in *"CI_SLOT_CPUS='${bad}'"*) echo "ok: reject cpus [${bad}]";; *) echo "FAIL: cpus [${bad}] -> ${out}" >&2; fail=1;; esac
  out="$(run CI_SLOT_MEM_GB="${bad}")"
  case "${out}" in *"CI_SLOT_MEM_GB='${bad}'"*) echo "ok: reject mem [${bad}]";; *) echo "FAIL: mem [${bad}] -> ${out}" >&2; fail=1;; esac
done
env CI_SLOT_CPUS=zz bash "${HELPER}" >/dev/null 2>&1 && { echo "FAIL: invalid value exited 0" >&2; fail=1; }
# Sourced use: function sets the variables and returns nonzero on bad input.
got="$(env -u CI_SLOT_CPUS CI_SLOT_CPUS=8 CI_SLOT_MEM_GB=24 bash -c 'source "$1"; slot_budget 50g; echo "$SLOT_JOBS $SLOT_MEM $SLOT_CPUS"' _ "${HELPER}")"
check "sourced" "8 24g 8" "${got}"
env CI_SLOT_MEM_GB=x bash -c 'source "$1"; slot_budget 50g' _ "${HELPER}" >/dev/null 2>&1 && { echo "FAIL: sourced bad value returned 0" >&2; fail=1; }
exit "${fail}"
