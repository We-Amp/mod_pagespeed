#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2024-2026 We-Amp B.V.
#
# Cap one Bazel repository cache directory by age and size.
#
#   prune-bazel-repository-cache.sh <cache-dir> <max-age> <max-size>
#   e.g. prune-bazel-repository-cache.sh "$REPO_CACHE_DIR" 30d 8G
#
# Bazel's --repository_cache has no eviction for content_addressable/. Bazel 9
# also keeps the repo contents cache under it (contents/), which has an idle-time
# GC of its own (--repo_contents_cache_gc_max_age); that GC runs only once the
# Bazel server has idled, which the throwaway container of the nightly Envoy
# filter job never reaches. That job keeps one cache per runner so Envoy's
# large dependency archives are not downloaded on every run; this prunes it at
# job start, when nothing holds the directory.
#
# It touches ONLY <cache-dir>/content_addressable (the layout is
# content_addressable/sha256/<hash>/file) and, when present, <cache-dir>/contents
# (Bazel 9), never anything else under <cache-dir>, and it never follows a
# symlink. Both share one size cap; either may be absent (Bazel 8 has no
# contents/).
#
# content_addressable/ is pruned per file. Bazel refreshes the mtime of an
# archive it reads from the cache, so mtime order is least-recently-used order.
# A hash directory may also hold an id-* marker file; Bazel never refreshes its
# mtime, so markers are not candidates by age or size and are removed only
# together with the sibling `file` they belong to. Hash directories left empty
# are removed.
#
# contents/ holds whole unpacked repository trees that Bazel reuses as they are,
# so it is pruned by WHOLE top-level entry only, never file by file, and the
# script never descends into an entry to delete anything. An entry's age is the
# newest mtime among its own *.recorded_inputs files (the files Bazel keeps
# beside each cached tree and refreshes when it reuses the tree); an entry
# without any is aged by the entry directory's own mtime. That layout is from
# knowledge of Bazel, not verified against a Bazel 9 run: the file mtimes
# inside a tree say nothing about its last use and are not consulted. Entries
# older than <max-age> (Nd) go first; if what remains still exceeds <max-size>
# (N[K|M|G|T], counting the bytes of every file in both directories), the
# oldest candidates by that age, files and whole entries alike, go until it
# fits. Plain files and symlinks directly under contents/ are left alone. A
# missing directory is not an error, and the script always exits 0 on that
# path.
#
# Sizing: the vendored repository cache is about 0.5 GB with Envoy's module
# archive; the Envoy filter job fetches much more (its toolchains and build
# dependencies). The caller passes 8G: each nightly reuses its whole archive
# set, so the age limit does the real clearing and the cap is a ceiling; the
# log line below shows the real size to lower it from. 30d keeps the nightly
# cadence warm across a few missed weeks.
#
# GNU find/stat/sort/xargs (the Linux runners); `gstat` stands in for stat on
# a Mac.

set -uo pipefail

if [ "$#" -ne 3 ]; then
  echo "usage: $0 <cache-dir> <max-age Nd> <max-size N[K|M|G|T]>" >&2
  exit 2
fi
DIR="$1"; AGE="$2"; SIZE="$3"

# Five digits cap the age: 99999d * 86400s fits int64, a longer number
# could wrap it and empty the cache.
[[ $AGE =~ ^[0-9]{1,5}d$ ]] || { echo "max-age must look like 30d (at most five digits), got: $AGE" >&2; exit 2; }
[[ $SIZE =~ ^[0-9]{1,6}[KMGT]?$ ]] || { echo "max-size must look like 8G (at most six digits), got: $SIZE" >&2; exit 2; }
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
  echo "repository cache prune: ${DIR} does not exist, nothing to do"
  exit 0
fi

# A symlink is never followed out of the cache directory.
CA_ROOT="${DIR}/content_addressable"
CT_ROOT="${DIR}/contents"
ROOTS=()
for r in "$CA_ROOT" "$CT_ROOT"; do
  if [ -d "$r" ] && [ ! -L "$r" ]; then ROOTS+=("$r"); fi
done
if [ "${#ROOTS[@]}" -eq 0 ]; then
  echo "repository cache prune: no content_addressable/ or contents/ under ${DIR}, nothing to do"
  exit 0
fi

STAT=stat
if ! stat -c %Y / >/dev/null 2>&1; then
  if command -v gstat >/dev/null 2>&1; then STAT=gstat; else
    echo "repository cache prune: no GNU stat, skipping" >&2; exit 0
  fi
fi

export LC_ALL=C
T="$(mktemp -d)" || exit 0
trap 'rm -rf "$T"' EXIT
NOW="$(date +%s)"

# "<mtime> <size> <path>" records, NUL-terminated (a symlink is listed as the
# link itself, never followed), oldest first. NUL separation holds a name
# containing a newline together end to end, so it can never yield a delete
# path outside the cache.
# A contents/ record is a whole entry: its newest *.recorded_inputs mtime (else
# the entry directory's own mtime), the sum of its file sizes, its path.
# id-* markers are excluded under content_addressable/ only.
{
  if [ -d "$CA_ROOT" ] && [ ! -L "$CA_ROOT" ]; then
    find "$CA_ROOT" \( -type f -o -type l \) ! -name 'id-*' -exec "$STAT" --printf '%Y %s %n\0' {} + 2>/dev/null
  fi
  if [ -d "$CT_ROOT" ] && [ ! -L "$CT_ROOT" ]; then
    while IFS= read -r -d '' e; do
      em="$(find "$e" -mindepth 1 -maxdepth 1 -type f -name '*.recorded_inputs' \
            -exec "$STAT" --printf '%Y\n' {} + 2>/dev/null | sort -n | tail -n 1)"
      [ -n "$em" ] || em="$("$STAT" --printf '%Y' "$e")"
      es="$(find "$e" \( -type f -o -type l \) -exec "$STAT" --printf '%s\n' {} + 2>/dev/null \
            | awk '{ t += $1 } END { print t + 0 }')"
      printf '%s %s %s\0' "$em" "$es" "$e"
    done < <(find "$CT_ROOT" -mindepth 1 -maxdepth 1 -type d -print0)
  fi
} | sort -z -t ' ' -k1,1n -k3 > "$T/list"

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
  while IFS= read -r -d '' f; do
    case "$f" in
      "$CT_ROOT"/*) rm -rf -- "$f" ;;  # a whole contents/ entry
      "$CA_ROOT"/*)
        rm -f -- "$f"
        # An id-* marker goes with the archive it belongs to, and only then.
        [ "$(basename "$f")" = file ] && rm -f -- "$(dirname "$f")"/id-* ;;
    esac
  done < "$T/delete"
  # Drop the content_addressable/ hash directories that are now empty (a real,
  # empty directory only; find does not follow symlinks here).
  if [ -d "$CA_ROOT" ] && [ ! -L "$CA_ROOT" ]; then
    find "$CA_ROOT" -mindepth 1 -type d -empty -delete 2>/dev/null
  fi
fi
echo "repository cache prune: ${DIR}: removed ${n} items, freed ${freed} bytes, ${total} bytes kept (limits ${AGE}, ${SIZE})"
exit 0
