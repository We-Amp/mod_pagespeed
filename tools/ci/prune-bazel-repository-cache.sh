#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2024-2026 We-Amp B.V.
#
# Cap one Bazel repository cache directory by age and size.
#
#   prune-bazel-repository-cache.sh <cache-dir> <max-age> <max-size>
#   e.g. prune-bazel-repository-cache.sh "$REPO_CACHE_DIR" 30d 8G
#
# Bazel's --repository_cache has no eviction of its own. The nightly Envoy
# filter job keeps one per runner so Envoy's large dependency archives are not
# downloaded on every run; this prunes it at job start, when nothing holds the
# directory.
#
# It touches ONLY files under <cache-dir>/content_addressable (the layout is
# content_addressable/sha256/<hash>/file), never anything else under
# <cache-dir>, and it never follows a symlink. Files older than <max-age>
# (Nd) go first; if what remains still exceeds <max-size> (N[K|M|G|T]), the
# oldest files by mtime go until it fits. Bazel refreshes the mtime of an
# archive it reads from the cache, so mtime order is least-recently-used
# order. A hash directory may also hold an id-* marker file; Bazel never
# refreshes its mtime, so markers are not candidates by age or size and are
# removed only together with the sibling `file` they belong to. Hash
# directories left empty are removed. A missing directory is not
# an error, and the script always exits 0 on that path.
#
# Sizing: the vendored repository cache is about 0.5 GB with Envoy's module
# archive; the Envoy filter job fetches much more (its toolchains and build
# dependencies). The caller passes 8G: each nightly reuses its whole archive
# set, so the age limit does the real clearing and the cap is a ceiling; the
# log line below shows the real size to lower it from. 30d keeps the nightly
# cadence warm across a few missed weeks.
#
# Bazel 9 adds a contents/ directory next to content_addressable/; this script
# does not prune it.
#
# GNU find/stat/sort/xargs (the Linux runners); `gstat` stands in for stat on
# a Mac.

set -uo pipefail

if [ "$#" -ne 3 ]; then
  echo "usage: $0 <cache-dir> <max-age Nd> <max-size N[K|M|G|T]>" >&2
  exit 2
fi
DIR="$1"; AGE="$2"; SIZE="$3"

[[ $AGE =~ ^[0-9]+d$ ]] || { echo "max-age must look like 30d, got: $AGE" >&2; exit 2; }
[[ $SIZE =~ ^[0-9]+[KMGT]?$ ]] || { echo "max-size must look like 8G, got: $SIZE" >&2; exit 2; }
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
  echo "repository cache prune: ${DIR} does not exist, nothing to do"
  exit 0
fi

ROOT="${DIR}/content_addressable"
# A symlink is never followed out of the cache directory.
if [ ! -d "$ROOT" ] || [ -L "$ROOT" ]; then
  echo "repository cache prune: no content_addressable/ under ${DIR}, nothing to do"
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

# "<mtime> <size> <path>" (a symlink is listed as the link itself, never
# followed), oldest first.
find "$ROOT" \( -type f -o -type l \) ! -name 'id-*' -exec "$STAT" -c '%Y %s %n' {} + 2>/dev/null \
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
  # An id-* marker goes with the archive it belongs to, and only then.
  while IFS= read -r f; do
    [ "$(basename "$f")" = file ] && rm -f -- "$(dirname "$f")"/id-*
  done < "$T/delete"
  # Drop the hash directories that are now empty (a real, empty directory
  # only; find does not follow symlinks here).
  find "$ROOT" -mindepth 1 -type d -empty -delete 2>/dev/null
fi
echo "repository cache prune: ${DIR}: removed ${N:-0} files, freed ${FREED:-0} bytes, ${LEFT:-0} bytes kept (limits ${AGE}, ${SIZE})"
exit 0
