#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2024-2026 We-Amp B.V.
#
# Cap one Bazel disk cache directory by age and size.
#
#   prune-bazel-disk-cache.sh <cache-dir> <max-age> <max-size>
#   e.g. prune-bazel-disk-cache.sh "$BAZEL_CACHE_DIR/disk" 7d 12G
#
# Bazel collects a disk cache only after its server has been idle for a
# while, and a CI step's container ends seconds after its last command, so
# Bazel's own collection never runs here. The jobs call this at their start
# instead, when nothing holds the directory.
#
# It touches ONLY <cache-dir>/ac and <cache-dir>/cas (what Bazel's disk cache
# writes), never anything else under <cache-dir>. Files older than <max-age>
# (Nd) go first; if ac + cas still exceed <max-size> (N[K|M|G|T]), the oldest
# files by mtime go until they fit; an action-cache entry goes before a
# content blob of the same mtime. Bazel refreshes the mtime of an entry it
# hits, so mtime order is least-recently-used order. A missing directory is
# not an error, and the script always exits 0 on that path.
#
# Sizing: the jobs pass 7d and 12G. Several per-runner caches share one host
# with the dev images and the runners' work directories, and they had grown
# to 19-26 GB each; 12G keeps a warm working set (the remote cache serves the
# rest) while leaving the system drive room.
#
# GNU find/stat/sort/xargs (the Linux runners); `gstat` stands in for stat on
# a Mac.

set -uo pipefail

if [ "$#" -ne 3 ]; then
  echo "usage: $0 <cache-dir> <max-age Nd> <max-size N[K|M|G|T]>" >&2
  exit 2
fi
DIR="$1"; AGE="$2"; SIZE="$3"

[[ $AGE =~ ^[0-9]+d$ ]] || { echo "max-age must look like 7d, got: $AGE" >&2; exit 2; }
[[ $SIZE =~ ^[0-9]+[KMGT]?$ ]] || { echo "max-size must look like 12G, got: $SIZE" >&2; exit 2; }
AGE_S=$(( ${AGE%d} * 86400 ))
UNIT="${SIZE: -1}"; NUM="${SIZE%[KMGT]}"
case "$UNIT" in
  K) MAX=$(( NUM * 1024 )) ;;
  M) MAX=$(( NUM * 1024 * 1024 )) ;;
  G) MAX=$(( NUM * 1024 * 1024 * 1024 )) ;;
  T) MAX=$(( NUM * 1024 * 1024 * 1024 * 1024 )) ;;
  *) MAX="$NUM" ;;
esac

if [ ! -d "$DIR" ]; then
  echo "disk cache prune: ${DIR} does not exist, nothing to do"
  exit 0
fi

ROOTS=()
for sub in ac cas; do
  # A symlink is never followed out of the cache directory.
  if [ -d "${DIR}/${sub}" ] && [ ! -L "${DIR}/${sub}" ]; then ROOTS+=("${DIR}/${sub}"); fi
done
if [ "${#ROOTS[@]}" -eq 0 ]; then
  echo "disk cache prune: no ac/ or cas/ under ${DIR}, nothing to do"
  exit 0
fi

STAT=stat
if ! stat -c %Y / >/dev/null 2>&1; then
  if command -v gstat >/dev/null 2>&1; then STAT=gstat; else
    echo "disk cache prune: no GNU stat, skipping" >&2; exit 0
  fi
fi

export LC_ALL=C
T="$(mktemp -d)" || exit 0
trap 'rm -rf "$T"' EXIT
NOW="$(date +%s)"

# "<mtime> <size> <path>" (a symlink is listed as the link itself, never
# followed), oldest first, ac/ before cas/ on equal mtime.
find "${ROOTS[@]}" \( -type f -o -type l \) -exec "$STAT" -c '%Y %s %n' {} + 2>/dev/null \
  | sort -t ' ' -k1,1n -k3 > "$T/list"

awk -v dlist="$T/delete" -v now="$NOW" -v age="$AGE_S" -v max="$MAX" '
  { m[NR] = $1; s[NR] = $2; p = $0; sub(/^[0-9]+ [0-9]+ /, "", p); path[NR] = p; total += $2 }
  END {
    for (i = 1; i <= NR; i++) {
      kill[i] = (now - m[i] > age)
      if (kill[i]) total -= s[i]
    }
    for (i = 1; i <= NR && total > max; i++) {
      if (!kill[i]) { kill[i] = 1; total -= s[i] }
    }
    for (i = 1; i <= NR; i++) if (kill[i]) { print path[i] > dlist; fb += s[i]; n++ }
    close(dlist)
    printf "%d %.0f %.0f\n", n, fb, total
  }' "$T/list" > "$T/summary"

read -r N FREED LEFT < "$T/summary"
if [ "${N:-0}" -gt 0 ]; then
  tr '\n' '\0' < "$T/delete" | xargs -0 rm -f --
fi
echo "disk cache prune: ${DIR}: removed ${N:-0} files, freed ${FREED:-0} bytes, ${LEFT:-0} bytes kept (limits ${AGE}, ${SIZE})"
exit 0
