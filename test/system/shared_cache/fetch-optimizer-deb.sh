#!/bin/bash
# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2024-2026 We-Amp B.V.
#
# Fetch the pinned optimizer's Debian package for PACKAGE_DIR mode of run.sh.
#
#   test/system/shared_cache/fetch-optimizer-deb.sh <out-dir> [amd64|arm64]
#
# Reads install/debian/optimizer-pin.json, downloads the pinned release's
# SHA256SUMS, requires it to hash to the pin's sha256sums_sha256, then
# downloads pagespeed-optimizer_<version>_<arch>.deb and requires it to match
# the verified list.  Any mismatch, or a package the list does not name, is an
# error and leaves no package in <out-dir>.
# OPTIMIZER_PIN and OPTIMIZER_RELEASE_BASE override the pin file and the
# download base (default: the optimizer repository's release downloads).
set -euo pipefail
OUT=${1:?usage: fetch-optimizer-deb.sh <out-dir> [amd64|arm64]}
ARCH=${2:-amd64}
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PIN=${OPTIMIZER_PIN:-$HERE/../../../install/debian/optimizer-pin.json}
BASE=${OPTIMIZER_RELEASE_BASE:-https://github.com/We-Amp/pagespeed-optimizer/releases/download}
sha() { if command -v sha256sum >/dev/null; then sha256sum "$1"; else shasum -a 256 "$1"; fi | cut -d' ' -f1; }

TAG=$(python3 -c "import json,sys;print(json.load(open(sys.argv[1]))['tag'])" "$PIN")
WANT_SUMS=$(python3 -c "import json,sys;print(json.load(open(sys.argv[1]))['sha256sums_sha256'])" "$PIN")
case $TAG in optimizer-v[0-9]*) ;; *) echo "fetch-optimizer-deb: pin tag '$TAG' is not a release" >&2; exit 1 ;; esac
VER=${TAG#optimizer-v}
tilde='~'
DEB="pagespeed-optimizer_${VER/-/$tilde}_${ARCH}.deb"

TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT
curl -fsSL --retry 5 --retry-all-errors -o "$TMP/SHA256SUMS" "$BASE/$TAG/SHA256SUMS"
if [ "$(sha "$TMP/SHA256SUMS")" != "$WANT_SUMS" ]; then
  echo "fetch-optimizer-deb: SHA256SUMS of $TAG does not match the pin" >&2
  exit 1
fi
want=$(awk -v n="$DEB" '{ f = $2; sub(/^\*/, "", f); if (f == n) print $1 }' "$TMP/SHA256SUMS")
if [ -z "$want" ]; then
  echo "fetch-optimizer-deb: the verified list does not name $DEB" >&2
  exit 1
fi
curl -fsSL --retry 5 --retry-all-errors -o "$TMP/$DEB" "$BASE/$TAG/$DEB"
if [ "$(sha "$TMP/$DEB")" != "$want" ]; then
  echo "fetch-optimizer-deb: $DEB does not match the verified list" >&2
  exit 1
fi
mkdir -p "$OUT"
mv "$TMP/$DEB" "$OUT/$DEB"
echo "fetched $DEB (pin $TAG)"
