#!/usr/bin/env bash
# SPDX-License-Identifier: BUSL-1.1
# Copyright (c) 2024-2026 We-Amp B.V.
#
# Provision the PINNED grype vulnerability DB for the blocking dep-scan path
#. Reads tools/sbom/grype-db-pin.json and `grype db import`s that
# exact dated archive (checksum-verified) into GRYPE_DB_CACHE_DIR. The caller
# then runs grype with GRYPE_DB_AUTO_UPDATE=false so the gate is deterministic:
# a newly-published CVE cannot surprise-break an in-flight PR or release. Only
# the blocking job uses this; report-only / scheduled runs keep the live DB.
#
# Idempotent: skips the import if the cache already holds the pinned build.
#
# Ported from pagespeed-optimizer's tools/sbom/provision-grype-db.sh so the 1.1
# blocking npm gate has the same determinism guarantee. Keep the two in sync.
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
PIN="${REPO_ROOT}/tools/sbom/grype-db-pin.json"

command -v grype >/dev/null 2>&1 || { echo "::error::grype not on PATH" >&2; exit 3; }
command -v jq    >/dev/null 2>&1 || { echo "::error::jq required" >&2; exit 3; }
[ -f "$PIN" ] || { echo "::error::missing $PIN" >&2; exit 3; }

URL_BASE="$(jq -r '.url_base' "$PIN")"
DB_PATH="$(jq -r '.path' "$PIN")"
CHECKSUM="$(jq -r '.checksum' "$PIN")"
BUILT="$(jq -r '.built' "$PIN")"

: "${GRYPE_DB_CACHE_DIR:?set GRYPE_DB_CACHE_DIR to an isolated cache dir}"
export GRYPE_DB_AUTO_UPDATE=false GRYPE_DB_VALIDATE_AGE=false

# Already provisioned with the pinned build? (validate-age off so an old pin
# still loads — pinning is the whole point.)
if grype db status 2>/dev/null | grep -q "$BUILT"; then
  echo "[provision-grype-db] pinned DB already present (built $BUILT)"
  exit 0
fi

echo "[provision-grype-db] importing pinned DB: $DB_PATH (built $BUILT)"
grype db import "${URL_BASE}/${DB_PATH}?checksum=${CHECKSUM}"
grype db status 2>/dev/null | grep -E 'Built|Status|Schema' || true
