#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2024-2026 We-Amp B.V.
#
# prune-vendor-dir.sh - age-based prune of a vendor directory that several
# runner instances (and, through a drive mount, Windows jobs) share.
#
# Usage: prune-vendor-dir.sh <dir> <prefix> <keep-sha|-> [<max-age-minutes>]
#   Removes <prefix>-*.tar.zst, their .sha256 sidecars and *.tmp files in
#   <dir> (not recursive) once older than <max-age-minutes> (default 120),
#   except <prefix>-<keep-sha>.tar.zst and its sidecar ("-" keeps nothing).
#
# Age comes from file mtimes, which a mounted Windows drive stamps with the
# other side's clock. A clock that disagrees with this host's by more than a
# couple of minutes makes every fresh file look old, and the prune would then
# delete a sibling runner's tarball while it is being written or read. The
# prune therefore stamps a probe file in <dir> and measures ages against the
# probe's own timestamp (through a reference file set that many minutes
# earlier), never against this host's clock. When the two disagree it says
# so and prunes anyway, so a permanent skew cannot fill the shared drive.
#
# bash 3.2 compatible (macOS runners); ASCII only.

set -u

DIR="${1:-}"
PREFIX="${2:-}"
KEEP="${3:--}"
MAXMIN="${4:-120}"
MAX_SKEW_SECS="${PRUNE_MAX_SKEW_SECS:-120}"

[ -n "$DIR" ] && [ -n "$PREFIX" ] || { echo "usage: prune-vendor-dir.sh <dir> <prefix> <keep-sha|-> [<minutes>]" >&2; exit 2; }
[ -d "$DIR" ] || { echo "prune-vendor-dir: no directory at the given path; nothing to prune"; exit 0; }

mtime_of() { stat -c %Y "$1" 2>/dev/null || stat -f %m "$1" 2>/dev/null; }

PROBE="$DIR/.clock-probe.$$"
: > "$PROBE" 2>/dev/null || { echo "::warning::prune-vendor-dir: directory is not writable; not pruning"; exit 0; }
STAMP="$(mtime_of "$PROBE")"
NOW="$(date +%s)"
rm -f "$PROBE"
case "$STAMP" in ''|*[!0-9]*) echo "::warning::prune-vendor-dir: cannot read file times; not pruning"; exit 0 ;; esac
SKEW=$((NOW - STAMP)); [ "$SKEW" -ge 0 ] || SKEW=$((-SKEW))
if [ "$SKEW" -gt "$MAX_SKEW_SECS" ]; then
  echo "::warning::prune-vendor-dir: file times differ from this host's clock by ${SKEW}s; measuring ages against the directory's own clock"
fi

# Reference file: the cutoff, on the directory's clock.
REF="$DIR/.prune-ref.$$"
trap 'rm -f "$REF"' EXIT
CUTOFF=$((STAMP - MAXMIN * 60))
if ! touch -d "@${CUTOFF}" "$REF" 2>/dev/null; then
  # BSD touch has no -d @epoch.
  touch -t "$(date -r "$CUTOFF" +%Y%m%d%H%M.%S 2>/dev/null)" "$REF" 2>/dev/null \
    || { echo "::warning::prune-vendor-dir: cannot stamp a reference file; not pruning"; exit 0; }
fi

if [ "$KEEP" = "-" ]; then
  find "$DIR" -maxdepth 1 -type f \
    \( -name "${PREFIX}-*.tar.zst" -o -name "${PREFIX}-*.tar.zst.sha256" -o -name "${PREFIX}-*.tar.zst.*.tmp" -o -name "${PREFIX}-*.tar.zst.sha256.*.tmp" \) \
    ! -newer "$REF" -delete 2>/dev/null || true
else
  find "$DIR" -maxdepth 1 -type f \
    \( -name "${PREFIX}-*.tar.zst" -o -name "${PREFIX}-*.tar.zst.sha256" -o -name "${PREFIX}-*.tar.zst.*.tmp" -o -name "${PREFIX}-*.tar.zst.sha256.*.tmp" \) \
    ! -name "${PREFIX}-${KEEP}.tar.zst" \
    ! -name "${PREFIX}-${KEEP}.tar.zst.sha256" \
    ! -newer "$REF" -delete 2>/dev/null || true
fi
exit 0
