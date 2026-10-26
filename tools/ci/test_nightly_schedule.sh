#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2024-2026 We-Amp B.V.
#
# Self-test for the nightly schedule: no job of pipeline-nightly.yml reads
# the cron string (the weekly sidecar step is gone; the full tier's sidecar
# pair runs every night), the crons never fire the same minute, none lands in
# the morning peak, and nothing reads the run-time weekday.
set -uo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
NIGHTLY="${ROOT}/.github/workflows/nightly.yml"
PIPELINE="${ROOT}/.github/workflows/pipeline-nightly.yml"
fail=0
ok() { echo "ok: $1"; }
bad() { echo "FAIL: $1" >&2; fail=1; }

crons="$(sed -n 's/^[[:space:]]*-[[:space:]]*cron:[[:space:]]*"\(.*\)"[[:space:]]*$/\1/p' "${NIGHTLY}")"

if grep -q 'github\.event\.schedule' "${PIPELINE}"; then
  bad "no job of pipeline-nightly.yml reads github.event.schedule"
else
  ok "no job of pipeline-nightly.yml reads github.event.schedule"
fi
if grep -q 'sidecar-pair-weekly' "${PIPELINE}"; then
  bad "no weekly sidecar pair job left"
else
  ok "no weekly sidecar pair job left"
fi

# Expand each day-of-week field to a set of digits (supports N, N-M, lists).
days() {
  local f="$1" part a b out=""
  [ "${f}" = "*" ] && f="0-6"
  for part in ${f//,/ }; do
    case "${part}" in
      *-*) a="${part%-*}"; b="${part#*-}"; while [ "${a}" -le "${b}" ]; do out="${out} ${a}"; a=$((a + 1)); done ;;
      *) out="${out} ${part}" ;;
    esac
  done
  echo "${out}"
}
overlap=""
seen=" "
while read -r m h dom mon dow; do
  [ -n "${dow}" ] || continue
  for d in $(days "${dow}"); do
    case "${seen}" in *" ${m}:${h}:${d} "*) overlap="${overlap} ${m}:${h}:${d}" ;; esac
    seen="${seen}${m}:${h}:${d} "
  done
done <<< "${crons}"
if [ -z "${overlap}" ] && [ "$(printf '%s\n' "${crons}" | grep -c .)" -ge 1 ]; then
  ok "no two crons fire the same minute"
else
  bad "no two crons fire the same minute: overlap [${overlap}]"
fi

peak=""
while read -r m h rest; do
  [ -n "${h}" ] || continue
  case "${h}" in
    ''|*[!0-9]*) peak="${peak} non-integer-hour:${h}"; continue ;;
  esac
  hh=$((10#${h}))
  if [ "${hh}" -ge 6 ] && [ "${hh}" -le 14 ]; then peak="${peak} ${h}"; fi
done <<< "${crons}"
if [ -z "${peak}" ] && [ -n "${crons}" ]; then
  ok "no cron hour between 06 and 14"
else
  bad "no cron hour between 06 and 14: [${peak}]"
fi

if grep -q 'date -u +%u' "${PIPELINE}"; then
  bad "no date -u +%u left in the nightly pipeline"
else
  ok "no date -u +%u left in the nightly pipeline"
fi
exit "${fail}"
