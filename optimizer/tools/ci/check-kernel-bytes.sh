#!/bin/bash

# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2024-2026 We-Amp B.V.
#
# check-kernel-bytes.sh - byte-compare a vendored kernel against the commit its
# pin file names, by running the kernel's sync tool in --check mode against the
# canonical repository.
#
# The required differential jobs otherwise verify only the pin's lineage (the
# floor ratchet), the file-name membership and parse behavior. None of those
# reads the vendored bytes, so a hand edit that leaves parsing unchanged would
# pass them. This script closes that gap for either kernel.
#
# Usage:
#   check-kernel-bytes.sh --sync-tool tools/sync-html-kernel.sh \
#       --pin-file lib/html/HTML_KERNEL_PIN --event "$GITHUB_EVENT_NAME" \
#       --base-repo "$GITHUB_WORKSPACE" \
#       [--canonical-url URL | --canonical GITDIR]
#
# What it does:
#   1. Resolves the canonical repository: --canonical GITDIR as given, or a
#      complete bare clone of --canonical-url into a temp dir (all objects up
#      front, so the compare makes no further network calls), with bounded
#      retries and a per-attempt timeout. The clone's stderr is kept in the log.
#   2. Runs the sync tool with --check (the tool compares against the pinned
#      commit, never the canonical's head, so a change landing in the other
#      repository cannot fail this).
#   3. Outcomes:
#        sync --check fails with its own reason (drift, a planted or missing
#        file, pin or floor not found, ancestry, ...) -> FAIL, always; the
#        tool's reason lines are repeated in the error
#        canonical unreachable (clone failed after the retries, or the
#        compare against a --canonical source died with exit 128; after our own
#        full clone, exit 128 is a real failure and fails):
#          pull_request and the pin equals the pin on origin/main
#                                                  -> WARN, exit 0
#          a pin bump, or any other event, or the
#          origin/main pin cannot be read, or the
#          pin is missing or malformed              -> FAIL (fail-closed)
#
# Test knobs: KERNEL_BYTES_CLONE_ATTEMPTS (default 3), KERNEL_BYTES_CLONE_TIMEOUT
# (seconds per attempt, default 300), KERNEL_BYTES_RETRY_SLEEP (default 4).
#
# Exit codes: 0 pass or warn, 1 fail, 2 bad usage.

set -euo pipefail

SYNC_TOOL=""
PIN_FILE=""
EVENT=""
BASE_REPO=""
CANONICAL=""
CANONICAL_URL=""

need_value() {
  [[ "$2" -ge 2 ]] || { echo "Missing value for $1" >&2; exit 2; }
}
while [[ $# -gt 0 ]]; do
  case "$1" in
    --sync-tool)     need_value "$1" $#; SYNC_TOOL="$2"; shift 2 ;;
    --pin-file)      need_value "$1" $#; PIN_FILE="$2"; shift 2 ;;
    --event)         need_value "$1" $#; EVENT="$2"; shift 2 ;;
    --base-repo)     need_value "$1" $#; BASE_REPO="$2"; shift 2 ;;
    --canonical)     need_value "$1" $#; CANONICAL="$2"; shift 2 ;;
    --canonical-url) need_value "$1" $#; CANONICAL_URL="$2"; shift 2 ;;
    *) echo "Unknown argument: $1" >&2; exit 2 ;;
  esac
done
if [[ -z "$SYNC_TOOL" || -z "$PIN_FILE" || -z "$EVENT" || -z "$BASE_REPO" ]]; then
  echo "Usage: $0 --sync-tool PATH --pin-file PATH --event EVENT --base-repo GITDIR [--canonical-url URL | --canonical GITDIR]" >&2
  exit 2
fi
if [[ -z "$CANONICAL" && -z "$CANONICAL_URL" ]]; then
  echo "need --canonical or --canonical-url" >&2
  exit 2
fi
[[ -f "$SYNC_TOOL" && -f "$PIN_FILE" ]] || { echo "sync tool or pin file not found" >&2; exit 2; }

pin_of() {  # stdin: pin file body; first token of the first non-comment line
  awk '!/^[[:space:]]*(#|$)/ {print $1; exit}'
}

# True when the canonical is out of reach AND this run may only warn.
may_warn() {
  [[ "$EVENT" == "pull_request" ]] || return 1
  local head_pin main_pin rel
  head_pin="$(pin_of < "$PIN_FILE")"
  [[ "$head_pin" =~ ^[0-9a-f]{40}$ ]] || return 1
  rel="$(git -C "$BASE_REPO" ls-files --full-name -- "$PIN_FILE" 2>/dev/null | head -n1 || true)"
  [[ -n "$rel" ]] || rel="$PIN_FILE"
  git -C "$BASE_REPO" fetch --no-tags --depth=1 origin main >/dev/null 2>&1 || return 1
  main_pin="$(git -C "$BASE_REPO" show "origin/main:${rel}" 2>/dev/null | pin_of || true)"
  [[ "$main_pin" =~ ^[0-9a-f]{40}$ && "$head_pin" == "$main_pin" ]]
}

# The canonical could not be read: warn where allowed, otherwise fail closed.
unreachable() {  # $1 = what happened
  echo "kernel byte check: $1" >&2
  if may_warn; then
    echo "::warning::kernel byte check: canonical repository unreachable; the vendored bytes of ${PIN_FILE} were NOT compared. Allowed only because this is a pull_request that does not change the pin."
    exit 0
  fi
  echo "::error::kernel byte check: canonical repository unreachable and this run may not skip (pin bump, non-pull_request event, unreadable origin/main pin, or a missing or malformed pin); failing closed." >&2
  exit 1
}

TMP="$(mktemp -d "${TMPDIR:-/tmp}/kernel-bytes.XXXXXX")"
trap 'rm -rf "$TMP"' EXIT

clone_canonical() {
  local attempts="${KERNEL_BYTES_CLONE_ATTEMPTS:-3}" tmo="${KERNEL_BYTES_CLONE_TIMEOUT:-300}" nap="${KERNEL_BYTES_RETRY_SLEEP:-4}" i=1 pre=()
  command -v timeout >/dev/null 2>&1 && pre=(timeout "$tmo")
  while :; do
    rm -rf "${TMP}/canonical"
    if ${pre[@]+"${pre[@]}"} git clone -q --bare "$CANONICAL_URL" "${TMP}/canonical" 2>>"${TMP}/clone.log"; then
      return 0
    fi
    echo "kernel byte check: clone attempt ${i}/${attempts} failed" >> "${TMP}/clone.log"
    [[ "$i" -lt "$attempts" ]] || return 1
    sleep $((nap * i))
    i=$((i + 1))
  done
}

SRC=""
if [[ -n "$CANONICAL" ]]; then
  SRC="$CANONICAL"
elif clone_canonical; then
  SRC="${TMP}/canonical"
  [[ ! -s "${TMP}/clone.log" ]] || sed 's/^/clone: /' "${TMP}/clone.log" >&2
else
  [[ ! -s "${TMP}/clone.log" ]] || sed 's/^/clone: /' "${TMP}/clone.log" >&2
  unreachable "cloning ${CANONICAL_URL} failed"
fi
[[ -d "$SRC" ]] || unreachable "canonical directory ${SRC} does not exist"

rc=0
out="$(bash "$SYNC_TOOL" --source "$SRC" --check 2>&1)" || rc=$?
printf '%s\n' "$out"
case "$rc" in
  0) exit 0 ;;
  128)
    # With --canonical the source may be a partial local repository whose
    # objects are fetched lazily, so a git fatal error can be a network failure.
    # After our own full clone the compare makes no network call: exit 128 is
    # then a real failure (for example a bad path in the tool's file list).
    if [[ -n "$CANONICAL" ]]; then
      unreachable "${SYNC_TOOL} --check died with exit 128 (git fatal error on the supplied canonical)"
    fi ;;
esac
reason="$(printf '%s\n' "$out" | grep -E '^(DRIFT|RED|ERROR):' | head -n 6 | tr '\n' ';' || true)"
echo "::error::kernel byte check: ${SYNC_TOOL} --check failed with exit ${rc}: ${reason:-no reason line printed (see output above)} The vendored files are written only by the sync tool; never hand-edit them." >&2
exit 1
