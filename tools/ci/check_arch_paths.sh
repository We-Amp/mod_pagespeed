#!/bin/bash
# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2024-2026 We-Amp B.V.

# Lint: no hard-coded CPU-arch toolchain paths in the container build scripts.
#
#   check_arch_paths.sh [file...]
#
# The scripts that build inside a distribution container
# (install/**/*_in_container.sh, the default set) run on x86_64 and aarch64
# alike. A literal triplet such as x86_64-redhat-linux or aarch64-linux-gnu in
# a path wires one arch only, and the other arch then fails far from the
# cause. Derive the triplet instead (rpm --eval '%{_arch}', uname -m,
# dpkg-architecture).
#
# A line is flagged when it names <arch>-linux-gnu or <arch>-redhat-linux
# (arch x86_64 or aarch64) and is none of:
#   * a comment line;
#   * a case arm whose pattern names an arch (amd64|x86_64) ...), which is the
#     arch branch itself;
#   * a line carrying the marker "arch-path-ok" (say why next to it).
# Prints path:line per hit and exits 1 when there is any. Self-test:
# tools/ci/test_check_arch_paths.sh

set -euo pipefail

if [ "$#" -eq 0 ]; then
  ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
  cd "$ROOT"
  mapfile -t FILES < <(git ls-files -- 'install/**/*_in_container.sh' 'install/*_in_container.sh')
  if [ "${#FILES[@]}" -eq 0 ]; then
    echo "check_arch_paths: no install/**/*_in_container.sh files found" >&2
    exit 1
  fi
else
  FILES=("$@")
fi

TRIPLET_RE='(x86_64|aarch64)-(linux-gnu|redhat-linux)'
COMMENT_RE='^[[:space:]]*#'
ARCH_ARM_RE='^[[:space:]]*[A-Za-z0-9_|*"-]*(x86_64|aarch64|amd64|arm64)[A-Za-z0-9_|*"-]*\)'

hits=0
for f in "${FILES[@]}"; do
  [ -f "$f" ] || { echo "check_arch_paths: no such file: $f" >&2; exit 1; }
  n=0
  while IFS= read -r line || [ -n "$line" ]; do
    n=$((n + 1))
    [[ "$line" =~ $TRIPLET_RE ]] || continue
    [[ "$line" =~ $COMMENT_RE ]] && continue
    [[ "$line" =~ $ARCH_ARM_RE ]] && continue
    [[ "$line" == *arch-path-ok* ]] && continue
    echo "${f}:${n}: hard-coded arch path: ${line#"${line%%[![:space:]]*}"}"
    hits=$((hits + 1))
  done < "$f"
done

if [ "$hits" -gt 0 ]; then
  echo "check_arch_paths: ${hits} hard-coded arch path(s); derive the triplet from the" \
    "container's arch, or mark an intended one with 'arch-path-ok'" >&2
  exit 1
fi
echo "check_arch_paths: ok (${#FILES[@]} file(s))"
