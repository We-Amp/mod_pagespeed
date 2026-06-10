#!/usr/bin/env bash
# SPDX-License-Identifier: BUSL-1.1
# Copyright (c) 2024-2026 We-Amp B.V.
#
# extract-vendor-tarball.sh - Robust cross-runner fetch + integrity-checked
# extraction of the hermetic vendor tarball produced by the `vendor` job.
#
# Why this script exists (recurring):
#   The vendor job writes the tarball to per-machine local storage
#   (both x64 builders carry the
#   `ci-builder,dedicated` label) and rsyncs a copy to the cache-host shared dir.
#   On `gh run rerun --failed`, GitHub does NOT re-run the successful vendor
#   job; retried consumer jobs land on whichever machine has capacity -- often
#   the OTHER one -- where the local tarball is missing or stale. The previous
#   cache-host fallback (a) only ran when the local file was missing, so a *partial*
#   local file sailed straight into `tar` ("zstd: premature end" /
#   "tar: Unexpected EOF"); and (b) had the integrity check inline-duplicated
#   into every consumer job, so fixes drifted.
#
# This script is the single, DRY, hardened path every Linux consumer job calls.
# Defense-in-depth:
#   1. INTEGRITY ALWAYS, EVEN ON THE LOCAL HIT. Before any `tar` runs we verify
#      non-empty + `zstd -t` (and a `.sha256` sidecar when present). A partial
#      NEVER reaches tar.
#   2. SELF-HEALING FALLBACK. If the local copy is missing OR fails integrity,
#      we re-fetch from the authoritative cache-host shared dir with retries +
#      backoff, re-verifying each attempt. A flaky/partial local file heals
#      from the authoritative source instead of cascading downstream failures.
#   3. IN-FLIGHT TTL EXTENSION. On every successful cache-host fetch we `touch` the
#      remote tarball so the design record's 3-day cleanup measures "days since last
#      consumed" rather than "days since vendored". A workflow rerun within the
#      window keeps its artifact alive without weakening cleanup for idle repos.
#   4. FAIL LOUD. When the artifact is genuinely gone from both the local disk
#      and cache-host, we exit non-zero with precise, actionable guidance
#      ("rerun the full workflow, not just failed jobs") instead of a silent
#      partial or a swallowed warning.
#
# Usage:
#   extract-vendor-tarball.sh --sha <short_sha> --dest <workspace_dir> \
#       [--local <local_tarball_path>] [--stream]
#
#   --sha    <s>  Short SHA the vendor job stamped into the tarball name
#                 (mod_pagespeed-<sha>.tar.zst). Required.
#   --dest   <d>  Directory to extract the workspace into. Required; created if
#                 absent. Extraction is `tar ... -C <dest>`.
#   --local  <p>  Candidate local tarball path to try before falling back to
#                 cache-host. Optional; if omitted only
#                 the cache-host path is used (e.g. auxiliary runners with no D:).
#   --stream      Extract by streaming `zstd -d | tar` instead of
#                 `tar --zstd -xf`. Used by jobs whose tar lacks --zstd.
#
# Environment overrides (all optional, sane LAN defaults):
#   CI_HUB_HOST          ssh host/IP for the shared dir (default: resolve
#                       cache-host, else 192.0.2.20).
#   CI_HUB_USER          ssh user (default: oschaaf).
#   CI_HUB_VENDOR_DIR    remote shared vendor dir
#                       (default: /Users/builder/ci/shared/vendor).
#   FETCH_RETRIES       cache-host fetch attempts (default: 4).
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
STREAM=0

die_usage() {
  echo "::error::extract-vendor-tarball.sh: $1" >&2
  echo "usage: extract-vendor-tarball.sh --sha <short_sha> --dest <dir> [--local <path>] [--stream]" >&2
  exit 2
}

while [ $# -gt 0 ]; do
  case "$1" in
    --sha)    SHORT_SHA="${2:-}"; shift 2 ;;
    --dest)   DEST="${2:-}"; shift 2 ;;
    --local)  LOCAL_TARBALL="${2:-}"; shift 2 ;;
    --stream) STREAM=1; shift ;;
    *)        die_usage "unknown argument: $1" ;;
  esac
done

[ -n "$SHORT_SHA" ] || die_usage "--sha is required"
[ -n "$DEST" ] || die_usage "--dest is required"

# --- config -----------------------------------------------------------------
BASENAME="mod_pagespeed-${SHORT_SHA}.tar.zst"
CI_HUB_USER="${CI_HUB_USER:-oschaaf}"
CI_HUB_VENDOR_DIR="${CI_HUB_VENDOR_DIR:-/Users/builder/ci/shared/vendor}"
FETCH_RETRIES="${FETCH_RETRIES:-4}"
FETCH_BACKOFF="${FETCH_BACKOFF:-3}"
SSH_OPTS="${SSH_OPTS:--o StrictHostKeyChecking=accept-new -o BatchMode=yes}"

if [ -z "${CI_HUB_HOST:-}" ]; then
  # `exit` after the first line: cache-host resolves to MULTIPLE IPs on some
  # runners (ci-pool sees both the live and a stale address -- cache-host has two
  # interfaces). Without it `awk '{print $1}'` emits two newline-joined IPs, so
  # the ssh/rsync host becomes "ip1\nip2" -> "hostname contains invalid
  # characters" and EVERY cache-host fetch fails (observed: burst linux-build
  # Extract tarball). Any one resolved IP reaches the host.
  CI_HUB_HOST="$(getent hosts cache-host 2>/dev/null | awk '{print $1; exit}' || true)"
  : "${CI_HUB_HOST:=192.0.2.20}"
fi

REMOTE_PATH="${CI_HUB_USER}@${CI_HUB_HOST}:${CI_HUB_VENDOR_DIR}/${BASENAME}"

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

# --- cache-host fetch with retry + backoff + TTL touch --------------------------
# fetch_from_ci_hub <dest_path>
#   Pulls the tarball (and its .sha256 sidecar if present) from the
#   authoritative shared dir, verifying after each attempt. On success, touches
#   the remote file's mtime so the 3-day TTL measures last-consumed.
fetch_from_ci_hub() {
  local dest_path="$1"
  local attempt backoff
  backoff="$FETCH_BACKOFF"

  for attempt in $(seq 1 "$FETCH_RETRIES"); do
    log "Fetching ${BASENAME} from cache-host (${CI_HUB_HOST}), attempt ${attempt}/${FETCH_RETRIES}..."
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
        ssh ${SSH_OPTS} "${CI_HUB_USER}@${CI_HUB_HOST}" \
          "touch ${CI_HUB_VENDOR_DIR}/${BASENAME} ${CI_HUB_VENDOR_DIR}/${BASENAME}.sha256 2>/dev/null || true" \
          >/dev/null 2>&1 || true
        log "Fetched and verified ${BASENAME} from cache-host."
        return 0
      fi
      warn "Fetched ${BASENAME} from cache-host but it failed integrity (attempt ${attempt}); retrying."
    else
      warn "rsync of ${BASENAME} from cache-host failed (attempt ${attempt}/${FETCH_RETRIES})."
    fi

    if [ "$attempt" -lt "$FETCH_RETRIES" ]; then
      sleep "$backoff"
      backoff=$((backoff * 2))
    fi
  done
  return 1
}

# --- main -------------------------------------------------------------------
mkdir -p "$DEST"

CHOSEN=""

# 1. Try the local candidate (same-machine vendor hit). Use -r, not -f: on
#    ci-pool a Windows-written tarball under /mnt/d can be stat-present
#    but EACCES for the WSL2 uid (DrvFs/NTFS ACL mismatch).
if [ -n "$LOCAL_TARBALL" ] && [ -r "$LOCAL_TARBALL" ]; then
  if verify_tarball "$LOCAL_TARBALL"; then
    log "Local tarball present and verified: $LOCAL_TARBALL"
    CHOSEN="$LOCAL_TARBALL"
  else
    # CRITICAL FIX vs old behaviour: a present-but-partial local file no longer
    # reaches tar. We fall through and self-heal from cache-host.
    warn "Local tarball failed integrity on $(hostname): $LOCAL_TARBALL -- self-healing from cache-host."
  fi
elif [ -n "$LOCAL_TARBALL" ]; then
  warn "Local tarball missing/unreadable on $(hostname): $LOCAL_TARBALL -- fetching from cache-host."
else
  log "No local candidate provided (auxiliary runner) -- fetching from cache-host."
fi

# 2. Fall back to the authoritative cache-host shared dir.
if [ -z "$CHOSEN" ]; then
  FETCHED="/tmp/${BASENAME}"
  if fetch_from_ci_hub "$FETCHED"; then
    CHOSEN="$FETCHED"
  else
    err "Vendor tarball ${BASENAME} is unavailable on $(hostname) AND could not be"
    err "fetched/verified from cache-host after ${FETCH_RETRIES} attempts."
    err "Most likely the producing vendor job's artifact was cleaned up by the"
    err "3-day TTL because only failed jobs were rerun -- GitHub does"
    err "NOT re-run the successful 'Vendor Dependencies' job on 'gh run rerun"
    err "--failed'. RECOVERY: rerun the FULL workflow ('gh run rerun <run-id>'"
    err "without --failed, or push an empty commit) so the vendor job"
    err "regenerates the tarball."
    exit 1
  fi
fi

# 3. Extract the verified tarball. (Verification already happened above, so
#    tar only ever sees a complete archive.)
log "Extracting verified workspace from ${CHOSEN} into ${DEST}"
if [ "$STREAM" -eq 1 ]; then
  zstd -d -T0 "$CHOSEN" -c | tar xf - -C "$DEST"
else
  tar --zstd -xf "$CHOSEN" -C "$DEST"
fi
log "Workspace extracted into ${DEST}"
