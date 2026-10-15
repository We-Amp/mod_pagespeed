#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2024-2026 We-Amp B.V.
#
# publish-vendor-tarball.sh - Publish a vendor tarball and its digest
# sidecar into the shared vendor dir without ever replacing a tarball that
# is already there.
#
# Usage: tools/ci/publish-vendor-tarball.sh <final-tarball> [<temp-tarball>]
#   With <temp-tarball>: publish it as <final-tarball> if that name is free,
#   otherwise discard it and adopt the tarball already there. Then (and with
#   only <final-tarball>) write <final-tarball>.sha256 for whatever tarball
#   is at the final name.
#
# Several writers can produce the same SHA's tarball at once: two runner
# instances share one host's vendor dir, and a push-to-master run, the
# nightly and a release tag often tar the same commit. Their bytes differ
# (file mtimes), so a writer that renamed its tarball over another's could
# leave one writer's digest next to the other's tarball, and an rm-then-
# rename removed a tarball a consumer had just verified. Publication
# is therefore create-if-absent: `ln` of the temp file to the final name is
# atomic and fails if the name exists, in which case the existing tarball is
# adopted. Once published, a tarball is never replaced, so the sidecar every
# writer computes from it is the same.
#
# DrvFs (a Windows drive mounted into WSL2) can report EACCES for a rename or link
# that actually landed, and can show a just-created name as briefly absent.
# Every publish step is therefore judged by its result (the file is there
# and complete, the sidecar holds the digest), never by the exit status of
# the command alone. Where hard links are unsupported, a plain rename onto
# the still-free name is the fallback.

set -euo pipefail

usage() { sed -n '9,13p' "$0" | sed 's/^# \{0,1\}//' >&2; }
log() { echo "publish-vendor-tarball: $*" >&2; }

[ $# -ge 1 ] && [ $# -le 2 ] || { usage; exit 2; }
FINAL="$1"
TMP="${2:-}"
RUN_TAG="${GITHUB_RUN_ID:-local}-${GITHUB_RUN_ATTEMPT:-$$}"

# settle <test-command...>: retry a check while DrvFs catches up.
settle() {
  local i
  for i in 1 2 3 4 5; do
    if "$@"; then return 0; fi
    if [ "$i" -lt 5 ]; then sleep 2; fi
  done
  return 1
}
size_of() { wc -c < "$1" 2>/dev/null || echo 0; }
is_complete() { [ -f "$FINAL" ] && [ "$(size_of "$FINAL")" -ge "$1" ]; }

if [ -n "$TMP" ]; then
  [ -s "$TMP" ] || { echo "::error::temp tarball missing or empty: $TMP" >&2; exit 1; }
  TMP_SIZE=$(size_of "$TMP")
  if ln "$TMP" "$FINAL" 2>/dev/null; then
    log "published $(basename "$FINAL")"
  elif [ -s "$FINAL" ]; then
    # Either another writer published this SHA first (adopt it), or our
    # link landed despite a DrvFs false error (the same thing, then).
    log "$(basename "$FINAL") already present; adopting it and discarding this run's copy"
  else
    # No hard links here: rename onto the name, which is still free.
    mv "$TMP" "$FINAL" 2>/dev/null || true
    if ! settle is_complete "$TMP_SIZE"; then
      echo "::error::could not publish $FINAL (size $(size_of "$FINAL") < $TMP_SIZE)" >&2
      exit 1
    fi
    log "published $(basename "$FINAL") by rename"
  fi
  rm -f "$TMP" 2>/dev/null || true
fi

if ! settle test -s "$FINAL"; then
  echo "::error::no tarball at $FINAL" >&2
  exit 1
fi

DIR="$(dirname "$FINAL")"
BN="$(basename "$FINAL")"
SIDECAR="${FINAL}.sha256"
SIDECAR_TMP="${FINAL}.sha256.${RUN_TAG}.tmp"
DIGEST="$(cd "$DIR" && sha256sum "$BN" | cut -d' ' -f1)"
printf '%s  %s\n' "$DIGEST" "$BN" > "$SIDECAR_TMP"
mv -f "$SIDECAR_TMP" "$SIDECAR" 2>/dev/null || true
sidecar_ok() { [ "$(cut -d' ' -f1 "$SIDECAR" 2>/dev/null)" = "$DIGEST" ]; }
if ! settle sidecar_ok; then
  echo "::error::sidecar $SIDECAR does not hold the digest of $BN" >&2
  exit 1
fi
rm -f "$SIDECAR_TMP" 2>/dev/null || true
log "sidecar ${BN}.sha256 = ${DIGEST}"
