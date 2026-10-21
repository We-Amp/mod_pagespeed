#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2024-2026 We-Amp B.V.
#
# Per-slot resource budget for fleet jobs. A build host runs several heavy
# jobs at once; each runner advertises its share through its environment:
#
#   CI_SLOT_CPUS    Bazel/clang-tidy parallelism for one job (positive integer)
#   CI_SLOT_MEM_GB  container memory limit for one job in GiB (positive integer)
#
# Unset (hosted runners, or a host not yet configured) keeps the legacy
# behaviour: Bazel sizes itself from the host CPU count and the caller's
# historical container cap applies.
#
# Sourced:   source tools/ci/slot-budget.sh; slot_budget [LEGACY_MEM]
#            sets SLOT_JOBS  (value for --jobs=)
#                 SLOT_MEM   (value for --memory=/--memory-swap=, e.g. 50g;
#                             empty = no cap)
#                 SLOT_MEM_GB (SLOT_MEM without the g suffix)
#                 SLOT_CPUS  (plain integer, empty when CI_SLOT_CPUS is unset)
# Executed:  tools/ci/slot-budget.sh [LEGACY_MEM]
#            prints JOBS=, MEM= and CPUS= lines.
#
# LEGACY_MEM is the cap used when CI_SLOT_MEM_GB is unset (e.g. 55g); omit it
# for a step that never had a cap. A set-but-invalid value is an error.

# shellcheck disable=SC2034  # SLOT_MEM_GB is part of the interface for callers
SLOT_LEGACY_JOBS='HOST_CPUS*.75'

_slot_check() { # name value max
  case "$2" in
    ''|*[!0-9]*|0|0[0-9]*)
      echo "::error::$1='$2' is not a positive integer" >&2; return 1 ;;
  esac
  if [ "${#2}" -gt 4 ] || [ "$2" -gt "$3" ]; then
    echo "::error::$1='$2' is out of range (max $3)" >&2; return 1
  fi
}

slot_budget() {
  SLOT_JOBS="${SLOT_LEGACY_JOBS}"
  SLOT_CPUS=""
  SLOT_MEM="${1:-}"
  if [ -n "${CI_SLOT_CPUS:-}" ]; then
    _slot_check CI_SLOT_CPUS "${CI_SLOT_CPUS}" 1024 || return 1
    SLOT_CPUS="${CI_SLOT_CPUS}"
    SLOT_JOBS="${CI_SLOT_CPUS}"
  fi
  if [ -n "${CI_SLOT_MEM_GB:-}" ]; then
    _slot_check CI_SLOT_MEM_GB "${CI_SLOT_MEM_GB}" 4096 || return 1
    SLOT_MEM="${CI_SLOT_MEM_GB}g"
  fi
  SLOT_MEM_GB="${SLOT_MEM%g}"
  return 0
}

if [ "${BASH_SOURCE[0]}" = "$0" ]; then
  slot_budget "$@" || exit 1
  echo "JOBS=${SLOT_JOBS}"
  echo "MEM=${SLOT_MEM}"
  echo "CPUS=${SLOT_CPUS}"
fi
