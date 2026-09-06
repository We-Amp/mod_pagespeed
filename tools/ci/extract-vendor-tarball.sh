#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2024-2026 We-Amp B.V.
#
# extract-vendor-tarball.sh - Robust cross-runner fetch + integrity-checked
# extraction of the hermetic vendor tarball produced by the `vendor` job.
#
# Why this script exists (recurring):
#   The vendor job writes the tarball to per-machine local storage
#   (a machine-local path on each x64 builder, all of which share one runner
#   label) and rsyncs a copy to the CI hub shared dir.
#   On `gh run rerun --failed`, GitHub does NOT re-run the successful vendor
#   job; retried consumer jobs land on whichever machine has capacity -- often
#   the OTHER one -- where the local tarball is missing or stale. The previous
#   hub fallback (a) only ran when the local file was missing, so a *partial*
#   local file sailed straight into `tar` ("zstd: premature end" /
#   "tar: Unexpected EOF"); and (b) had the integrity check inline-duplicated
#   into every consumer job, so fixes drifted.
#
# This script is the single, DRY, hardened path every Linux consumer job calls.
# Defense-in-depth:
#   1. INTEGRITY ALWAYS, EVEN ON THE LOCAL HIT. Before any `tar` runs we verify
#      non-empty + `zstd -t` (and a `.sha256` sidecar when present). A partial
#      NEVER reaches tar.
#   2. SELF-HEALING FALLBACK. If the local copy is missing, fails integrity,
#      OR FAILS EXTRACTION (a verified file can still vanish -- or its DrvFs
#      mount flap -- between `zstd -t` and `tar`: Linux Build lane, run
#      29756918487, "Local tarball present and verified" then seconds later
#      "Cannot open: No such file or directory"), we re-fetch from the
#      authoritative CI hub shared dir with retries + backoff, re-verifying
#      each attempt. A flaky/partial/vanished local file heals from the
#      authoritative source instead of cascading downstream failures.
#   3. IN-FLIGHT TTL EXTENSION. On every successful hub fetch we `touch` the
#      remote tarball so the design record's 3-day cleanup measures "days since last
#      consumed" rather than "days since vendored". A workflow rerun within the
#      window keeps its artifact alive without weakening cleanup for idle repos.
#   4. FAIL LOUD. When the artifact is genuinely gone from both the local disk
#      and the CI hub, we exit non-zero with precise, actionable guidance
#      ("rerun the full workflow, not just failed jobs") instead of a silent
#      partial or a swallowed warning.
#
# Usage:
#   extract-vendor-tarball.sh --sha <short_sha> --dest <workspace_dir> \
#       [--local <local_tarball_path>] [--prefix <name>] [--stream]
#
#   --sha    <s>  Short SHA the vendor job stamped into the tarball name
#                 (<prefix>-<sha>.tar.zst). Required.
#   --dest   <d>  Directory to extract the workspace into. Required; created if
#                 absent. Extraction is `tar ... -C <dest>`.
#   --local  <p>  Candidate local tarball path to try before falling back to
#                 the hub (e.g. a machine-local vendor dir). Optional; if
#                 omitted only the hub path is used (e.g. auxiliary runners
#                 with no local vendor drive).
#   --prefix <n>  Tarball basename prefix (default: mod_pagespeed).
#   --stream      Extract by streaming `zstd -d | tar` instead of
#                 `tar --zstd -xf`. Used by jobs whose tar lacks --zstd.
#
# Environment (hub coordinates come from GitHub repository variables; NOTHING
# environment-specific is hardcoded here):
#   CI_HUB_ADDR         ssh host/IP for the shared dir (default: resolved by
#                       resolve-cache-host.sh from CI_HUB_HOST / CI_HUB_IP).
#   CI_HUB_USER         ssh user (required only when the hub is contacted).
#   CI_HUB_VENDOR_DIR   remote shared vendor dir (same).
#   FETCH_RETRIES       hub fetch attempts (default: 4).
#   FETCH_BACKOFF       initial backoff seconds, doubled each retry (default: 3).
#   SSH_OPTS            ssh options (default: StrictHostKeyChecking=accept-new
#                       + BatchMode=yes).
#
# Exit codes:
#   0  workspace extracted and integrity-verified.
#   1  artifact genuinely unavailable / corrupt after all fallbacks (loud).
#   2  bad usage.

set -euo pipefail

# --- argument parsing -------------------------------------------------------
SHORT_SHA=""
DEST=""
LOCAL_TARBALL=""
PREFIX="mod_pagespeed"
STREAM=0

die_usage() {
  echo "::error::extract-vendor-tarball.sh: $1" >&2
  echo "usage: extract-vendor-tarball.sh --sha <short_sha> --dest <dir> [--local <path>] [--prefix <name>] [--stream]" >&2
  exit 2
}

while [ $# -gt 0 ]; do
  case "$1" in
    --sha)    SHORT_SHA="${2:-}"; shift 2 ;;
    --dest)   DEST="${2:-}"; shift 2 ;;
    --local)  LOCAL_TARBALL="${2:-}"; shift 2 ;;
    --prefix) PREFIX="${2:-}"; shift 2 ;;
    --stream) STREAM=1; shift ;;
    *)        die_usage "unknown argument: $1" ;;
  esac
done

[ -n "$SHORT_SHA" ] || die_usage "--sha is required"
[ -n "$DEST" ] || die_usage "--dest is required"
[ -n "$PREFIX" ] || die_usage "--prefix must be non-empty"

# --- config -----------------------------------------------------------------
BASENAME="${PREFIX}-${SHORT_SHA}.tar.zst"
# Hub coordinates are only needed on the fetch path. Do NOT require them here:
# a --local run with a good tarball never touches the hub, and aborting on an
# unset variable would fail a job that had everything it needed. The hard
# requirement (and the address resolution, which costs TCP probes) is scoped to
# resolve_hub() below, called from fetch_from_hub().
CI_HUB_USER="${CI_HUB_USER:-}"
CI_HUB_VENDOR_DIR="${CI_HUB_VENDOR_DIR:-}"
FETCH_RETRIES="${FETCH_RETRIES:-4}"
FETCH_BACKOFF="${FETCH_BACKOFF:-3}"
SSH_OPTS="${SSH_OPTS:--o StrictHostKeyChecking=accept-new -o BatchMode=yes}"
REMOTE_PATH=""

log()  { echo "$*"; }
warn() { echo "::warning::$*"; }
err()  { echo "::error::$*" >&2; }

# --- integrity --------------------------------------------------------------
# verify_tarball <path> [<expected_sha_file>]
#   Returns 0 only if the file is non-empty, passes `zstd -t`, and (when a
#   .sha256 sidecar exists) matches it. Never extracts; pure verification so a
#   partial can be rejected before it ever reaches `tar`.
verify_tarball() {
  local path="$1"
  local sha_file="${2:-${path}.sha256}"

  if [ ! -s "$path" ]; then
    warn "tarball missing or empty: $path"
    return 1
  fi

  # zstd -t (== --test): validates the full frame, catching truncation
  # ("premature end") and corruption before tar ever sees the stream.
  if ! zstd -t "$path" >/dev/null 2>&1; then
    warn "tarball failed 'zstd -t' (partial/corrupt frame): $path"
    return 1
  fi

  # Optional stronger check: a committed-size-and-digest sidecar written by the
  # vendor job. zstd -t alone can pass a tarball truncated exactly on a frame
  # boundary; the sha256 closes that gap. Absent sidecar => zstd -t is the floor.
  if [ -f "$sha_file" ]; then
    local want got
    want="$(awk '{print $1}' "$sha_file" 2>/dev/null | head -1)"
    if [ -n "$want" ]; then
      if command -v sha256sum >/dev/null 2>&1; then
        got="$(sha256sum "$path" | awk '{print $1}')"
      elif command -v shasum >/dev/null 2>&1; then
        got="$(shasum -a 256 "$path" | awk '{print $1}')"
      else
        got=""
      fi
      if [ -n "$got" ] && [ "$got" != "$want" ]; then
        warn "tarball sha256 mismatch (want ${want:0:12}.. got ${got:0:12}..): $path"
        return 1
      fi
    fi
  fi
  return 0
}

# --- hub fetch with retry + backoff + TTL touch -----------------------------
# fetch_from_hub <dest_path>
#   Pulls the tarball (and its .sha256 sidecar if present) from the
#   authoritative shared dir, verifying after each attempt. On success, touches
#   the remote file's mtime so the 3-day TTL measures last-consumed.
# resolve_hub -- assert the hub coordinates and assemble REMOTE_PATH.
#   Called once, lazily, from fetch_from_hub(): everything it needs is only
#   needed when we actually reach over the network.
resolve_hub() {
  [ -n "$REMOTE_PATH" ] && return 0
  : "${CI_HUB_USER:?CI_HUB_USER must be set (GitHub repository variable) to fetch from the hub}"
  : "${CI_HUB_VENDOR_DIR:?CI_HUB_VENDOR_DIR must be set (GitHub repository variable) to fetch from the hub}"

  if [ -z "${CI_HUB_ADDR:-}" ]; then
    # resolve-cache-host.sh probes TCP/22 on every resolved address and prints
    # the first that answers -- the bare first-resolved-address idiom this
    # replaces pre-collapsed to whatever stale A-record DNS listed first and
    # died with "No route to host" (see resolve-cache-host.sh's header).
    CI_HUB_ADDR="$(bash "$(dirname "${BASH_SOURCE[0]}")/resolve-cache-host.sh" 2>/dev/null || true)"
    : "${CI_HUB_ADDR:=${CI_HUB_IP:-}}"
  fi
  : "${CI_HUB_ADDR:?no hub address: set CI_HUB_ADDR, or CI_HUB_HOST/CI_HUB_IP for resolve-cache-host.sh}"

  REMOTE_PATH="${CI_HUB_USER}@${CI_HUB_ADDR}:${CI_HUB_VENDOR_DIR}/${BASENAME}"
}

fetch_from_hub() {
  local dest_path="$1"
  local attempt backoff
  resolve_hub
  backoff="$FETCH_BACKOFF"

  for attempt in $(seq 1 "$FETCH_RETRIES"); do
    log "Fetching ${BASENAME} from the CI hub (${CI_HUB_ADDR}), attempt ${attempt}/${FETCH_RETRIES}..."
    # Best-effort sidecar fetch first (don't fail the attempt if it's absent --
    # older vendor runs predate the sidecar). --ignore-missing-args keeps rsync
    # from erroring when the sidecar doesn't exist on the source.
    rsync -a --ignore-missing-args -e "ssh ${SSH_OPTS}" \
      "${REMOTE_PATH}.sha256" "${dest_path}.sha256" >/dev/null 2>&1 || true

    if rsync -a -e "ssh ${SSH_OPTS}" "$REMOTE_PATH" "$dest_path"; then
      if verify_tarball "$dest_path"; then
        # Extend in-flight TTL: re-stamp mtime so the design record cleanup counts from
        # last consumption, not from vendoring. Best-effort; never fatal.
        # SSH_OPTS is an intentional multi-token option list -> must word-split.
        # shellcheck disable=SC2029,SC2086
        ssh ${SSH_OPTS} "${CI_HUB_USER}@${CI_HUB_ADDR}" \
          "touch ${CI_HUB_VENDOR_DIR}/${BASENAME} ${CI_HUB_VENDOR_DIR}/${BASENAME}.sha256 2>/dev/null || true" \
          >/dev/null 2>&1 || true
        log "Fetched and verified ${BASENAME} from the CI hub."
        return 0
      fi
      warn "Fetched ${BASENAME} from the CI hub but it failed integrity (attempt ${attempt}); retrying."
    else
      warn "rsync of ${BASENAME} from the CI hub failed (attempt ${attempt}/${FETCH_RETRIES})."
    fi

    if [ "$attempt" -lt "$FETCH_RETRIES" ]; then
      sleep "$backoff"
      backoff=$((backoff * 2))
    fi
  done
  return 1
}

# --- extraction ---------------------------------------------------------------
# extract_workspace <path>
#   Extracts a tarball that has ALREADY passed verify_tarball into $DEST.
#   Returns tar's (or the pipeline's, under pipefail) exit status so the
#   caller can self-heal: a verified file can still vanish -- or its DrvFs
#   mount flap -- between verification and extraction (Linux Build lane, run
#   29756918487: "Local tarball present and verified" followed seconds later
#   by tar's "Cannot open: No such file or directory"). Called under `if`, so
#   `set -e` is intentionally suppressed inside; the last command's status is
#   the function's return value (pipefail keeps a zstd-side failure in the
#   --stream pipeline from being masked by tar's exit code).
extract_workspace() {
  local path="$1"
  log "Extracting verified workspace from ${path} into ${DEST}"
  if [ "$STREAM" -eq 1 ]; then
    zstd -d -T0 "$path" -c | tar xf - -C "$DEST"
  else
    tar --zstd -xf "$path" -C "$DEST"
  fi
}

# --- main -------------------------------------------------------------------
mkdir -p "$DEST"

CHOSEN=""

# 1. Try the local candidate (same-machine vendor hit). Use -r, not -f: on
#    a WSL2 builder a Windows-written tarball under a DrvFs mount can be
#    stat-present but EACCES for the WSL2 uid (DrvFs/NTFS ACL mismatch).
if [ -n "$LOCAL_TARBALL" ] && [ -r "$LOCAL_TARBALL" ]; then
  if verify_tarball "$LOCAL_TARBALL"; then
    log "Local tarball present and verified: $LOCAL_TARBALL"
    CHOSEN="$LOCAL_TARBALL"
  else
    # CRITICAL FIX vs old behaviour: a present-but-partial local file no longer
    # reaches tar. We fall through and self-heal from the CI hub.
    warn "Local tarball failed integrity on $(hostname): $LOCAL_TARBALL -- self-healing from the CI hub."
  fi
elif [ -n "$LOCAL_TARBALL" ]; then
  warn "Local tarball missing/unreadable on $(hostname): $LOCAL_TARBALL -- fetching from the CI hub."
else
  log "No local candidate provided (auxiliary runner) -- fetching from the CI hub."
fi

# 2. Extract the verified local candidate. Verification passing is NOT the end
#    of the story: the file can still vanish, or the DrvFs mount flap, between
#    `zstd -t` and `tar`. Treat a failed extraction exactly
#    like a failed integrity check -- heal from the CI hub below.
if [ -n "$CHOSEN" ]; then
  if extract_workspace "$CHOSEN"; then
    log "Workspace extracted into ${DEST}"
    exit 0
  fi
  warn "Local tarball verified but extraction failed on $(hostname): $CHOSEN -- self-healing from the CI hub."
  CHOSEN=""
fi

# 3. Fall back to the authoritative CI hub shared dir.
FETCHED="/tmp/${BASENAME}"
if fetch_from_hub "$FETCHED"; then
  # The fetched copy was verified by fetch_from_hub a moment ago, so an
  # extraction failure here is not a fetch/integrity problem; there is no
  # further fallback -- fail loud.
  if extract_workspace "$FETCHED"; then
    log "Workspace extracted into ${DEST}"
    exit 0
  fi
  err "Vendor tarball ${BASENAME} was fetched from the CI hub and passed integrity,"
  err "but extraction into ${DEST} still failed on $(hostname). Investigate the"
  err "runner's tar/zstd toolchain and the destination filesystem; re-running"
  err "the job as-is will hit the same failure."
  exit 1
fi

err "Vendor tarball ${BASENAME} is unavailable on $(hostname) AND could not be"
err "fetched/verified from the CI hub after ${FETCH_RETRIES} attempts."
err "Most likely the producing vendor job's artifact was cleaned up by the"
err "3-day TTL because only failed jobs were rerun -- GitHub does"
err "NOT re-run the successful 'Vendor Dependencies' job on 'gh run rerun"
err "--failed'. RECOVERY: rerun the FULL workflow ('gh run rerun <run-id>'"
err "without --failed, or push an empty commit) so the vendor job"
err "regenerates the tarball."
exit 1
