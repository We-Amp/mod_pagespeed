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
# Environment:
#   VENDOR_EXPECT_SHA256  digest the temp tarball must have (an end-to-end
#                         check of a transfer that produced it).
#   VENDOR_VERIFY_ZSTD=1  also require the temp tarball (and an existing
#                         final one) to pass `zstd -t`.
#
# Several writers can produce the same SHA's tarball at once: two runner
# instances share one host's vendor dir (#1752), and a push-to-main run, the
# nightly and a release tag often tar the same commit. Their bytes differ
# (file mtimes), so a writer that renamed its tarball over another's could
# leave one writer's digest next to the other's tarball, and an rm-then-
# rename removed a tarball a consumer had just verified (#1766). Publication
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
# the still-free name is the fallback. That fallback is not exclusive: on a
# filesystem without hard links two publishers can both see the name as free
# and the second rename replaces the first, so "a sound tarball is never
# rewritten" holds only where `ln` works. Sidecars still converge on the
# tarball that ends up there, and readers retry until tarball and digest agree.

set -euo pipefail

usage() { sed -n '9,13p' "$0" | sed 's/^# \{0,1\}//' >&2; }
log() { echo "publish-vendor-tarball: $*" >&2; }

[ $# -ge 1 ] && [ $# -le 2 ] || { usage; exit 2; }
FINAL="$1"
TMP="${2:-}"
# Temp names must be unique per publishing process, not per workflow run:
# one run can hold several jobs that publish the same tarball into the same
# shared dir at once, and with a run-only tag each would remove or overwrite
# the other's stage and sidecar temp files mid-check.
RUN_TAG="${GITHUB_RUN_ID:-local}-${GITHUB_RUN_ATTEMPT:-0}-$$-${RANDOM}"
EXPECT="${VENDOR_EXPECT_SHA256:-}"
VERIFY_ZSTD="${VENDOR_VERIFY_ZSTD:-0}"
DIR="$(dirname "$FINAL")"
BN="$(basename "$FINAL")"
SIDECAR="${FINAL}.sha256"
SIDECAR_TMP="${FINAL}.sha256.${RUN_TAG}.tmp"
STAGE="${FINAL}.${RUN_TAG}.tmp"

# settle <test-command...>: retry a check while DrvFs catches up.
settle() {
  local i
  for i in 1 2 3 4 5; do
    if "$@"; then return 0; fi
    if [ "$i" -lt 5 ]; then sleep "${PUBLISH_SETTLE_SECS:-2}"; fi
  done
  return 1
}
size_of() { wc -c < "$1" 2>/dev/null | tr -d ' ' || echo 0; }
hash_of() {
  if command -v sha256sum >/dev/null 2>&1; then sha256sum "$1" | cut -d' ' -f1
  else shasum -a 256 "$1" | cut -d' ' -f1; fi
}
# Flush a file's data to disk: `sync FILE` (coreutils 8.24+); older or BSD
# sync ignores or rejects the argument, which is only a missed hardening.
flush() { sync "$1" 2>/dev/null || sync 2>/dev/null || true; }
zstd_ok() { [ "$VERIFY_ZSTD" != 1 ] || ! command -v zstd >/dev/null 2>&1 || zstd -t -q "$1" >/dev/null 2>&1; }

# sound <file>: non-empty, a complete zstd frame (when asked), and agreeing
# with its sidecar when one is there.
sound() {
  local f="$1" want
  [ -s "$f" ] || return 1
  zstd_ok "$f" || return 1
  if [ "$f" = "$FINAL" ] && [ -s "$SIDECAR" ]; then
    want="$(cut -d' ' -f1 "$SIDECAR" 2>/dev/null)"
    [ -z "$want" ] || [ "$(hash_of "$f")" = "$want" ] || return 1
  fi
  return 0
}

if [ -n "$TMP" ]; then
  settle test -s "$TMP" || { echo "::error::temp tarball missing or empty: $TMP" >&2; exit 1; }
  TMP_SIZE=$(size_of "$TMP")
  if [ "$(cd "$(dirname "$TMP")" && pwd)" = "$(cd "$DIR" && pwd)" ]; then
    STAGE="$TMP"
  else
    # The temp file lives elsewhere (a runner-private path): stage a copy in
    # the shared directory under a unique name and prove it equals the source.
    # DrvFs can hand back stale or short bytes right after a copy: prove the
    # copy with the same patience as every other step, and copy again a few
    # times before calling it a bad copy. Each attempt stages under its own
    # name (a just-removed name can misbehave on DrvFs), and a failed cp is
    # a failed attempt, not the end of the script.
    TMP_HASH="$(hash_of "$TMP")"
    copy_matches() {
      [ "$(size_of "$STAGE")" = "$TMP_SIZE" ] && [ "$(hash_of "$STAGE")" = "$TMP_HASH" ]
    }
    copied=0
    for i in 1 2 3; do
      rm -f "$STAGE" 2>/dev/null || true
      STAGE="${FINAL}.${RUN_TAG}.${i}.tmp"
      rm -f "$STAGE" 2>/dev/null || true
      cp "$TMP" "$STAGE" || log "cp into $DIR failed (attempt $i of 3)"
      flush "$STAGE"
      if settle copy_matches; then copied=1; break; fi
      log "copy of $(basename "$TMP") into $DIR differs from its source (attempt $i of 3)"
    done
    if [ "$copied" != 1 ]; then
      rm -f "$STAGE"
      echo "::error::copy of $(basename "$TMP") into $DIR differs from its source after 3 attempts" >&2
      exit 1
    fi
  fi
  if ! zstd_ok "$STAGE"; then
    rm -f "$STAGE"
    echo "::error::temp tarball is not a complete zstd frame: $TMP" >&2
    exit 1
  fi
  STAGE_DIGEST="$(hash_of "$STAGE")"
  if [ -n "$EXPECT" ] && [ "$STAGE_DIGEST" != "$EXPECT" ]; then
    rm -f "$STAGE"
    echo "::error::temp tarball digest ${STAGE_DIGEST} is not the expected ${EXPECT}" >&2
    exit 1
  fi
  flush "$STAGE"

  published=0
  for _ in 1 2 3; do
    if [ -e "$FINAL" ]; then
      if sound "$FINAL"; then
        log "$BN already present and sound; adopting it and discarding this run's copy"
        published=1; break
      fi
      # Replace by rename, never in place; drop the stale digest first.
      log "$BN present but unsound (empty, truncated or disagreeing with its digest); replacing it"
      rm -f "$SIDECAR"
      if mv -f "$STAGE" "$FINAL" 2>/dev/null || [ "$(size_of "$FINAL")" = "$TMP_SIZE" ]; then
        published=1; break
      fi
    elif ln "$STAGE" "$FINAL" 2>/dev/null; then
      # The name was free, so any sidecar is stale; drop it only now that
      # this writer owns the name (a losing writer must not remove the
      # winner's fresh one).
      rm -f "$SIDECAR"
      log "published $BN"
      published=1; break
    elif [ ! -e "$FINAL" ]; then
      # No hard links here: rename onto the name, which is still free.
      if mv "$STAGE" "$FINAL" 2>/dev/null || settle test -s "$FINAL"; then
        rm -f "$SIDECAR"
        log "published $BN by rename"
        published=1; break
      fi
    fi
  done
  if [ "$published" != 1 ] || ! settle test -s "$FINAL"; then
    echo "::error::could not publish $FINAL (size $(size_of "$FINAL") < $TMP_SIZE)" >&2
    exit 1
  fi
  rm -f "$STAGE" 2>/dev/null || true
  [ "$TMP" = "$STAGE" ] || rm -f "$TMP" 2>/dev/null || true
fi

if ! settle test -s "$FINAL"; then
  echo "::error::no tarball at $FINAL" >&2
  exit 1
fi

# Sidecar: temp name, flush, rename; then re-read against the digest of the
# final tarball until they agree (another repairing writer may have renamed
# new bytes in between).
for _ in 1 2 3; do
  DIGEST="$(hash_of "$FINAL")"
  printf '%s  %s\n' "$DIGEST" "$BN" > "$SIDECAR_TMP"
  flush "$SIDECAR_TMP"
  mv -f "$SIDECAR_TMP" "$SIDECAR" 2>/dev/null || true
  sidecar_ok() { [ "$(cut -d' ' -f1 "$SIDECAR" 2>/dev/null)" = "$DIGEST" ]; }
  if settle sidecar_ok && [ "$(hash_of "$FINAL")" = "$DIGEST" ]; then
    rm -f "$SIDECAR_TMP" 2>/dev/null || true
    log "sidecar ${BN}.sha256 = ${DIGEST}"
    exit 0
  fi
done
echo "::error::sidecar $SIDECAR does not hold the digest of $BN" >&2
exit 1
