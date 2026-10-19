#!/bin/bash

# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2024-2026 We-Amp B.V.

# Version -> optimizer cache directory generation, as the two upgrade
# rehearsals derive it (cache_gen_for_version in
# install/upgrade_test/run_upgrade_test.sh and its copy in
# test/system/el9-selinux/el9-selinux-rehearsal.sh).
#
# The rehearsals install a published baseline, upgrade to the version under
# test and look for the optimizer's cache under /var/cache/pagespeed-optimizer/
# v1, v2 or v3, so a wrong mapping fails a release rehearsal (or, worse, checks
# the wrong directory). The module line moved from 1.19.0 to 2.5.0, so the
# comparison has to be numeric across the major: 2.5.0 is the first release
# with generation 3, the published 1.15 to 1.19.0 keep theirs, and the
# 1.19.1-pre.N dry-run tags do not move to 3.
#
# Hermetic: the functions and thresholds are lifted out of the two scripts
# (both start containers when run, so they are not sourced whole), the table
# runs against each copy, and the two copies must agree line for line.
#
# Run: bash tools/ci/test_cache_gen_for_version.sh

set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
SCRIPTS=(
  install/upgrade_test/run_upgrade_test.sh
  test/system/el9-selinux/el9-selinux-rehearsal.sh
)

# version  expected generation
TABLE='
1.15.0          v1
1.15.0-beta.1   v1
1.16.0-rc.14    v1
1.16.0          v1
1.17.0-rc.1     v2
1.17.0          v2
1.18.0          v2
1.19.0          v2
1.19.1-pre.1    v2
1.19.1-pre.2    v2
1.19.1          v2
1.100.0         v2
2.4.0           v2
2.5.0-rc.1      v3
2.5.0           v3
2.5.1           v3
2.10.0          v3
3.0.0           v3
10.0.0          v3
'

# extract FILE: the threshold assignments and the two functions, verbatim.
extract() {
  awk '
    /^CACHE_GEN[0-9]+_SINCE=/ { print; next }
    /^(semver_ge|cache_gen_for_version)\(\) \{/ { infn = 1 }
    infn { print }
    infn && /^\}/ { infn = 0 }
  ' "$REPO_ROOT/$1" | grep -v '^[[:space:]]*#'
}

FAILS=0
WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT

for s in "${SCRIPTS[@]}"; do
  extract "$s" > "$WORK/$(basename "$s").fns"
  for need in CACHE_GEN2_SINCE= CACHE_GEN3_SINCE= 'semver_ge()' 'cache_gen_for_version()'; do
    grep -qF "$need" "$WORK/$(basename "$s").fns" \
      || { echo "FAIL  $s: could not find $need"; FAILS=$((FAILS + 1)); }
  done
done

if ! diff -u "$WORK/run_upgrade_test.sh.fns" "$WORK/el9-selinux-rehearsal.sh.fns"; then
  echo "FAIL  the two copies of cache_gen_for_version / semver_ge / CACHE_GEN*_SINCE differ"
  FAILS=$((FAILS + 1))
fi

for s in "${SCRIPTS[@]}"; do
  echo "== $s"
  printf '  %-15s %-8s %s\n' version want got
  while read -r ver want; do
    [ -n "$ver" ] || continue
    got="$(bash -c "set -euo pipefail; $(cat "$WORK/$(basename "$s").fns"); cache_gen_for_version '$ver'")"
    if [ "$got" = "$want" ]; then
      printf '  %-15s %-8s %s\n' "$ver" "$want" "$got"
    else
      printf '  %-15s %-8s %s   FAIL\n' "$ver" "$want" "$got"
      FAILS=$((FAILS + 1))
    fi
  done <<<"$TABLE"
done

# The baseline search ignores -pre tags (GA only) and must order 2.x above
# 1.x: semver_ge on the full version, as the rehearsals use it.
for case in "2.5.0 1.19.1 0" "1.19.1 2.5.0 1" "1.19.1-pre.2 1.19.1 1" \
            "1.19.1 1.19.1-pre.2 0" "1.19.1-pre.2 1.19.1-pre.1 0" "2.10.0 2.9.0 0" \
            "1.100.0 2.5.0 1"; do
  read -r a b want <<<"$case"
  got=0
  bash -c "$(cat "$WORK/run_upgrade_test.sh.fns"); semver_ge '$a' '$b'" || got=1
  if [ "$got" != "$want" ]; then
    echo "FAIL  semver_ge $a $b: exit $got, want $want"
    FAILS=$((FAILS + 1))
  fi
done

if [ "$FAILS" -ne 0 ]; then
  echo "cache_gen_for_version: $FAILS failure(s)"
  exit 1
fi
echo "cache_gen_for_version: ok"
