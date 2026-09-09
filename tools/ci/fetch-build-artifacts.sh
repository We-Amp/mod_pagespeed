#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2024-2026 We-Amp B.V.
#
# fetch-build-artifacts.sh - Retried, integrity-checked fetch of the linux-x64
# build-artifacts tarball staged on the CI hub by the `linux-build` job.
#
# Why this exists (companion to extract-vendor-tarball.sh):
#   The vendor tarball fetch is retried, verified, and TTL-extended. The
#   build-artifacts fetch sitting next to it in the CI workflow was a bare single-shot
#   rsync: one transient LAN hiccup failed the job, a truncated transfer went
#   straight into `tar`, and -- because nothing ever touched the remote file --
#   the hub's cleanup measured "days since staged" instead of "days since last
#   consumed", so a rerun outside that window died on a file that had been
#   silently reaped. This script closes all three gaps.
#
# Usage:
#   fetch-build-artifacts.sh --sha <full_sha> --out <dest_tarball_path>
#
# Environment (hub coordinates come from GitHub repository variables; NOTHING
# environment-specific is hardcoded here):
#   CI_HUB_ADDR            ssh host/IP (default: resolved by
#                          resolve-cache-host.sh from CI_HUB_HOST / CI_HUB_IP).
#   CI_HUB_USER            ssh user (required).
#   CI_HUB_ARTIFACTS_ROOT  remote per-repo artifact root (required).
#   FETCH_RETRIES          attempts (default: 4).
#   FETCH_BACKOFF   initial backoff seconds, doubled each retry (default: 3).
#
# Exit codes:
#   0  tarball fetched and integrity-verified.
#   1  artifact unavailable/corrupt after all attempts (loud).
#   2  bad usage.
set -euo pipefail

SHA=""
OUT=""
die_usage() {
  echo "::error::fetch-build-artifacts.sh: $1" >&2
  echo "usage: fetch-build-artifacts.sh --sha <full_sha> --out <path>" >&2
  exit 2
}
while [ $# -gt 0 ]; do
  case "$1" in
    --sha) SHA="${2:-}"; shift 2 ;;
    --out) OUT="${2:-}"; shift 2 ;;
    *) die_usage "unknown argument: $1" ;;
  esac
done
[ -n "$SHA" ] || die_usage "--sha is required (got empty; on a rerun-failed-jobs the vendor job does not re-run and its outputs can come back empty -- rerun the full workflow)"
[ -n "$OUT" ] || die_usage "--out is required"

CI_HUB_USER="${CI_HUB_USER:?CI_HUB_USER must be set (GitHub repository variable)}"
CI_HUB_ARTIFACTS_ROOT="${CI_HUB_ARTIFACTS_ROOT:?CI_HUB_ARTIFACTS_ROOT must be set (GitHub repository variable)}"
if [ -z "${CI_HUB_ADDR:-}" ]; then
  CI_HUB_ADDR="$(bash "$(dirname "${BASH_SOURCE[0]}")/resolve-cache-host.sh" 2>/dev/null || true)"
fi
: "${CI_HUB_ADDR:=${CI_HUB_IP:-}}"
RETRIES="${FETCH_RETRIES:-4}"
BACKOFF="${FETCH_BACKOFF:-3}"
SSH_OPTS="${SSH_OPTS:--o StrictHostKeyChecking=accept-new -o BatchMode=yes -o ConnectTimeout=10}"
REMOTE="${CI_HUB_ARTIFACTS_ROOT}/${SHA}/linux-x64/build-artifacts.tar.zst"

echo "[fetch-build-artifacts] host=${CI_HUB_ADDR} sha=${SHA}"

verify() {
  # A truncated transfer must NEVER reach tar. zstd -t walks the whole frame.
  [ -s "$1" ] || { echo "[fetch-build-artifacts] empty file"; return 1; }
  zstd -t "$1" >/dev/null 2>&1 || { echo "[fetch-build-artifacts] zstd integrity check failed"; return 1; }
}

attempt=1
while [ "$attempt" -le "$RETRIES" ]; do
  rm -f "$OUT"
  if rsync -a --partial --inplace -e "ssh ${SSH_OPTS}" \
       "${CI_HUB_USER}@${CI_HUB_ADDR}:${REMOTE}" "$OUT" 2>&1 && verify "$OUT"; then
    # TTL extension: the hub's cleanup is mtime-based, so a consumed artifact
    # must look recently used, not recently staged. Best-effort; never fatal.
    ssh ${SSH_OPTS} "${CI_HUB_USER}@${CI_HUB_ADDR}" "touch '${REMOTE}'" 2>/dev/null || true
    echo "[fetch-build-artifacts] ok ($(wc -c < "$OUT") bytes, attempt ${attempt})"
    exit 0
  fi
  echo "::warning::build-artifacts fetch attempt ${attempt}/${RETRIES} failed"
  attempt=$((attempt + 1))
  [ "$attempt" -le "$RETRIES" ] && { sleep "$BACKOFF"; BACKOFF=$((BACKOFF * 2)); }
done

rm -f "$OUT"
echo "::error::build-artifacts unavailable or corrupt after ${RETRIES} attempts: ${CI_HUB_USER}@${CI_HUB_ADDR}:${REMOTE}"
echo "::error::the artifact may have been cleaned up -- rerun the FULL workflow, not just the failed jobs"
exit 1
