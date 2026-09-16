#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2024-2026 We-Amp B.V.
#
# Provision the PINNED grype vulnerability DB for the blocking dep-scan path,
# and report how stale that pin is. Reads
# tools/sbom/grype-db-pin.json and `grype db import`s that exact dated archive
# (checksum-verified) into GRYPE_DB_CACHE_DIR. The caller then runs grype with
# GRYPE_DB_AUTO_UPDATE=false so the gate is deterministic: a newly-published
# CVE cannot surprise-break an in-flight PR or release. Only the blocking job
# uses this; report-only / scheduled runs keep the live DB.
#
# The cost of a pin is staleness, so this script also makes the pin's age
# VISIBLE on every run (stdout + the job Summary tab) and, with --max-age /
# --enforce-age, ENFORCES it. The workflow enforces only on the scheduled job,
# never on the PR job: a forgotten pin must redden the schedule, not someone's
# PR. (The 2.0.41 readiness audit found this repo had NO age enforcement at
# all — a pin nobody bumps is a gate that has quietly stopped working.)
#
# Ported from pagespeed-optimizer's tools/sbom/provision-grype-db.sh so the 1.1
# blocking npm gate has the same determinism guarantee. Kept in sync with
# corp + pagespeed-optimizer; the pin JSON is the same shape.
#
# Usage:
#   GRYPE_DB_CACHE_DIR=<dir> tools/sbom/provision-grype-db.sh [--max-age N | --enforce-age]
#
# Exit codes: 0 ok · 1 pin older than the limit · 3 environment/tooling fault.
#
# Idempotent: skips the import if the cache already holds the pinned build.
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
PIN="${REPO_ROOT}/tools/sbom/grype-db-pin.json"

MAX_AGE=""
while [ "$#" -gt 0 ]; do
  case "$1" in
    --max-age) shift; MAX_AGE="${1:-}" ;;
    --max-age=*) MAX_AGE="${1#*=}" ;;
    # --enforce-age means "use max_age_days from the pin file".
    --enforce-age) MAX_AGE="pin" ;;
    *) echo "::error::unknown arg: $1" >&2; exit 3 ;;
  esac
  shift
done

command -v grype >/dev/null 2>&1 || { echo "::error::grype not on PATH" >&2; exit 3; }
command -v jq    >/dev/null 2>&1 || { echo "::error::jq required" >&2; exit 3; }
[ -f "$PIN" ] || { echo "::error::missing $PIN" >&2; exit 3; }

URL_BASE="$(jq -r '.url_base' "$PIN")"
DB_PATH="$(jq -r '.path' "$PIN")"
CHECKSUM="$(jq -r '.checksum' "$PIN")"
BUILT="$(jq -r '.built' "$PIN")"
PIN_MAX_AGE="$(jq -r '.max_age_days // 14' "$PIN")"
[ "$MAX_AGE" = "pin" ] && MAX_AGE="$PIN_MAX_AGE"

: "${GRYPE_DB_CACHE_DIR:?set GRYPE_DB_CACHE_DIR to an isolated cache dir}"
export GRYPE_DB_AUTO_UPDATE=false GRYPE_DB_VALIDATE_AGE=false

# --- import (idempotent; validate-age off so an old pin still loads — pinning
# is the whole point; its age is reported and enforced below, by us) ----------
if grype db status 2>/dev/null | grep -q "$BUILT"; then
  echo "[provision-grype-db] pinned DB already present (built $BUILT)"
else
  echo "[provision-grype-db] importing pinned DB: $DB_PATH (built $BUILT)"
  # Download with curl (resume + retries) and verify the sha256 ourselves, then
  # import the LOCAL file. grype's own Go downloader has no resume/retry and
  # dies on transient TLS corruption ("tls: bad record MAC" — root-caused
  # 2026-08-26 to the CI runners' USB-ethernet / virtualized-NIC
  # receive paths), turning a flaky link into a red gate. The import itself is
  # offline: `grype db import <file>` never touches the network. Mirrors the
  # same fix in the sibling provisioning script — keep the two in sync.
  TMP_DB="$(mktemp -t grype-db.XXXXXX.tar.zst)"
  trap 'rm -f "$TMP_DB"' EXIT
  curl -fSL --retry 5 --retry-all-errors -C - -o "$TMP_DB" "${URL_BASE}/${DB_PATH}"
  if command -v sha256sum >/dev/null 2>&1; then
    ACTUAL="$(sha256sum "$TMP_DB" | awk '{print $1}')"
  else
    ACTUAL="$(shasum -a 256 "$TMP_DB" | awk '{print $1}')"
  fi
  [ "$ACTUAL" = "${CHECKSUM#sha256:}" ] \
    || { echo "::error::checksum mismatch on downloaded DB (want $CHECKSUM, got sha256:$ACTUAL)" >&2; exit 3; }
  echo "[provision-grype-db] checksum verified ($CHECKSUM)"
  grype db import "$TMP_DB"
  rm -f "$TMP_DB"
  trap - EXIT
  grype db status 2>/dev/null | grep -E 'Built|Status|Schema' || true
fi

# --- staleness (always reported; enforced only when a limit is given) --------
# `date -d` is GNU, `date -j -f` is BSD/macOS — the runners are Linux but this
# script is also run by hand on a Mac, so handle both.
epoch_of() {
  date -u -d "$1" +%s 2>/dev/null || date -u -j -f '%Y-%m-%dT%H:%M:%SZ' "$1" +%s 2>/dev/null
}
BUILT_EPOCH="$(epoch_of "$BUILT" || true)"
if [ -z "${BUILT_EPOCH:-}" ]; then
  echo "::warning::could not parse pin built timestamp '$BUILT'; skipping age check" >&2
  exit 0
fi
AGE_DAYS=$(( ( $(date -u +%s) - BUILT_EPOCH ) / 86400 ))

LINE="grype DB pin: schema \`$(jq -r '.schema' "$PIN")\` · built \`${BUILT}\` · **${AGE_DAYS} day(s) old**"
echo "[provision-grype-db] ${AGE_DAYS} day(s) since pinned DB build ($BUILT)"
if [ -n "${GITHUB_STEP_SUMMARY:-}" ]; then
  printf '%s\n\n' "$LINE" >>"$GITHUB_STEP_SUMMARY"
fi

if [ -n "$MAX_AGE" ] && [ "$AGE_DAYS" -gt "$MAX_AGE" ]; then
  echo "::error::grype DB pin is ${AGE_DAYS} days old (max ${MAX_AGE}). Refresh tools/sbom/grype-db-pin.json from ${URL_BASE}/latest.json and re-run — a pin nobody bumps is a gate that has quietly stopped working." >&2
  # shellcheck disable=SC2016  # backticks here are Markdown code spans, not a subshell
  [ -n "${GITHUB_STEP_SUMMARY:-}" ] && \
    printf '> **STALE PIN** — %s days old (max %s). Refresh `tools/sbom/grype-db-pin.json`.\n\n' "$AGE_DAYS" "$MAX_AGE" >>"$GITHUB_STEP_SUMMARY"
  exit 1
fi
exit 0
