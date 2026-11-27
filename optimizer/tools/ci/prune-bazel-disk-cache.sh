#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2024-2026 We-Amp B.V.
#
# Cap one Bazel disk cache directory by age and size.
#
#   prune-bazel-disk-cache.sh <cache-dir> <max-age> <max-size>
#   e.g. prune-bazel-disk-cache.sh "${HOME}/.cache/optimizer-bazel-disk-arm64" 7d 1G
#
# Bazel collects a disk cache only after its server has been idle for a
# while, and a CI step's container ends seconds after its last command, so
# Bazel's own collection never runs here. The hosted jobs that keep a disk
# cache in actions/cache call this after their last Bazel command and before
# the cache is saved, when nothing holds the directory.
#
# It touches ONLY <cache-dir>/ac and <cache-dir>/cas (what Bazel's disk cache
# writes), never anything else under <cache-dir>. Files older than <max-age>
# (Nd) go first; if ac + cas still exceed <max-size> (N[K|M|G|T]), the oldest
# files by mtime go until they fit; an action-cache entry goes before a
# content blob of the same mtime. Bazel refreshes the mtime of an entry it
# hits, so mtime order is least-recently-used order. A missing directory is
# not an error, and the script always exits 0 on that path.
#
# Sizing: the caps are chosen per job so that every weekly disk cache entry,
# two weeks' worth of them, and the other actions/cache entries fit the
# repository's 10 GB cache quota (see the callers in .github/workflows/).
#
# GNU find/stat/sort/xargs (the Linux runners); `gstat` stands in for stat on
# a Mac, and without either perl lists the same records (the hosted macOS
# image has perl but no GNU coreutils). Without any of them the script
# skips the prune and still exits 0.

set -uo pipefail

if [ "$#" -ne 3 ]; then
  echo "usage: $0 <cache-dir> <max-age Nd> <max-size N[K|M|G|T]>" >&2
  exit 2
fi
DIR="$1"; AGE="$2"; SIZE="$3"

# Five digits cap the age: 99999d * 86400s fits int64, a longer number
# could wrap it and empty the cache.
[[ $AGE =~ ^[0-9]{1,5}d$ ]] || { echo "max-age must look like 7d (at most five digits), got: $AGE" >&2; exit 2; }
[[ $SIZE =~ ^[0-9]{1,6}[KMGT]?$ ]] || { echo "max-size must look like 12G (at most six digits), got: $SIZE" >&2; exit 2; }
# 10# reads the numbers base 10: a leading zero would be octal otherwise.
AGE_S=$(( 10#${AGE%d} * 86400 ))
UNIT="${SIZE: -1}"; NUM=$((10#${SIZE%[KMGT]}))
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

if stat -c %Y / >/dev/null 2>&1; then STAT=stat
elif command -v gstat >/dev/null 2>&1; then STAT=gstat
elif command -v perl >/dev/null 2>&1; then STAT=perl
else
  echo "disk cache prune: no GNU stat and no perl, skipping" >&2; exit 0
fi

export LC_ALL=C
T="$(mktemp -d)" || exit 0
trap 'rm -rf "$T"' EXIT
NOW="$(date +%s)"

# "<mtime> <size> <path>" records, NUL-terminated (a symlink is listed as the
# link itself, never followed), oldest first, ac/ before cas/ on equal mtime.
# NUL separation holds a name containing a newline together end to end, so it
# can never yield a delete path outside the cache. The perl lister writes
# the same records from lstat (mtime, size, path).
list_records() {
  if [ "$STAT" = perl ]; then
    # shellcheck disable=SC2016
    find "${ROOTS[@]}" \( -type f -o -type l \) -exec perl -e \
      'for (@ARGV) { my @s = lstat($_) or next; printf "%d %d %s\0", $s[9], $s[7], $_ }' {} +
  else
    find "${ROOTS[@]}" \( -type f -o -type l \) -exec "$STAT" --printf '%Y %s %n\0' {} +
  fi
}
list_records 2>/dev/null | sort -z -t ' ' -k1,1n -k3 > "$T/list"

# Age first, then oldest-first until what is left fits MAX.
M=() S=() P=() K=() nrec=0 total=0
while IFS= read -r -d '' rec; do
  rest="${rec#* }"
  M+=("${rec%% *}"); S+=("${rest%% *}"); P+=("${rest#* }")
  total=$((total + ${rest%% *}))
  nrec=$((nrec + 1))
done < "$T/list"
for ((i = 0; i < nrec; i++)); do
  if (( NOW - M[i] > AGE_S )); then K[i]=1; total=$((total - S[i])); fi
done
for ((i = 0; i < nrec; i++)); do
  [ "$total" -le "$MAX" ] && break
  if [ -z "${K[i]:-}" ]; then K[i]=1; total=$((total - S[i])); fi
done
: > "$T/delete"
n=0 freed=0
for ((i = 0; i < nrec; i++)); do
  if [ -n "${K[i]:-}" ]; then
    printf '%s\0' "${P[i]}"
    freed=$((freed + S[i])); n=$((n + 1))
  fi
done >> "$T/delete"
if [ "$n" -gt 0 ]; then
  xargs -0 rm -f -- < "$T/delete"
fi
echo "disk cache prune: ${DIR}: removed ${n} files, freed ${freed} bytes, ${total} bytes kept (limits ${AGE}, ${SIZE})"
exit 0
