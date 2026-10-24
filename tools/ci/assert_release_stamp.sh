#!/bin/bash

# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2024-2026 We-Amp B.V.

# Release assertion: every shipped Linux module carries the release commit.
#
#   assert_release_stamp.sh [--require-families [--prerelease]] <assets-dir> <release-sha>
#
# For each Linux module asset in <assets-dir> (the Apache module .deb and .rpm,
# the el10 Apache rpm, the standalone Apache module .so files, the nginx module
# tarballs, .deb and .rpm, and the cPanel (ea4) rpms, x64 and arm64) every *.so
# is extracted (a standalone .so is read as it is) and
# must contain the full 40-character <release-sha> (GIT_COMMIT_FULL_STRING from
# version.h.in). Prints "stamp ok: <asset> <so>" per module; exits 1 with
# "not stamped: <asset> <so>" on a miss, and also when no module .so was found
# at all (fail closed). Self-test: tools/ci/test_assert_release_stamp.sh

set -euo pipefail

REQUIRE=0
PRERELEASE=0
while [ "$#" -gt 0 ]; do
  case "$1" in
    --require-families) REQUIRE=1; shift ;;
    --prerelease) PRERELEASE=1; shift ;;
    *) break ;;
  esac
done
if [ "$#" -ne 2 ]; then
  echo "usage: $0 [--require-families [--prerelease]] <assets-dir> <release-sha>" >&2
  exit 2
fi
DIR="$1"
SHA="$2"

if ! printf '%s' "$SHA" | grep -qE '^[0-9a-f]{40}$'; then
  echo "error: release sha must be 40 lowercase hex characters, got '${SHA}'" >&2
  exit 1
fi
[ -d "$DIR" ] || { echo "error: no such directory: ${DIR}" >&2; exit 1; }

TMPS=()
cleanup() {
  local t
  for t in "${TMPS[@]+"${TMPS[@]}"}"; do rm -rf "$t"; done
}
trap cleanup EXIT

need() {
  command -v "$1" >/dev/null 2>&1 || { echo "error: $1 is required for $2" >&2; exit 1; }
}

modules=0
missed=0

# Module asset families, one glob per family, as tools/ci/release_manifest.py
# names them in required() and UNIQUE_PREFIX (the self-test checks the two agree). Globs are
# anchored at the start of the file name, so the optimizer packages
# (pagespeed-optimizer_*.deb, pagespeed-optimizer-*.rpm) never match.
# FAMILY|glob|required-on-prerelease
FAMILIES=(
  "apache deb|mod-pagespeed_*.deb|1"
  "apache rpm|mod-pagespeed-*.rpm|1"
  "apache rpm el10|el10-mod-pagespeed-*.rpm|1"
  "apache module so|libmod_pagespeed-linux-*.so|1"
  "nginx tarball|ngx_pagespeed-*.tar.gz|1"
  "nginx deb|nginx-module-pagespeed_*.deb|1"
  "nginx rpm|nginx-module-pagespeed-*.rpm|1"
  "ea4 rpm|ea-apache24-mod_pagespeed-*.rpm|0"
)

shopt -s nullglob
assets=()
nofamily=0
for fam in "${FAMILIES[@]}"; do
  IFS='|' read -r fname fglob freq <<<"$fam"
  cnt=0
  for f in "$DIR"/$fglob; do
    assets+=("$f")
    cnt=$((cnt + 1))
  done
  if [ "$REQUIRE" -eq 1 ] && [ "$cnt" -eq 0 ] && { [ "$freq" -eq 1 ] || [ "$PRERELEASE" -eq 0 ]; }; then
    echo "not stamped: no ${fname} asset in ${DIR} (family missing)"
    nofamily=1
  fi
done

for asset in "${assets[@]+"${assets[@]}"}"; do
  name="$(basename "$asset")"
  t="$(mktemp -d)"
  TMPS+=("$t")
  abs="$(cd "$(dirname "$asset")" && pwd)/${name}"
  case "$name" in
    *.deb) need dpkg-deb "$name"; dpkg-deb -x "$abs" "$t" ;;
    *.rpm) need rpm2cpio "$name"; need cpio "$name"; (cd "$t" && rpm2cpio "$abs" | cpio -id --quiet) ;;
    *.tar.gz) tar -xzf "$abs" -C "$t" ;;
    *.so) cp "$abs" "$t/$name" ;;
  esac
  found=0
  while IFS= read -r so; do
    found=1
    modules=$((modules + 1))
    rel="${so#"$t"/}"
    if [ "$(grep -caF -- "$SHA" "$so" || true)" -gt 0 ]; then
      echo "stamp ok: ${name} ${rel}"
    else
      echo "not stamped: ${name} ${rel}"
      missed=1
    fi
  done < <(find "$t" -type f -name '*.so' | sort)
  if [ "$found" -eq 0 ]; then
    echo "not stamped: ${name} (no .so inside)"
    missed=1
  fi
done

if [ "$nofamily" -ne 0 ]; then
  echo "error: a required module asset family is missing" >&2
  exit 1
fi
if [ "$modules" -eq 0 ]; then
  echo "error: no Linux module .so found in ${DIR} (no Linux module asset?)" >&2
  exit 1
fi
if [ "$missed" -ne 0 ]; then
  echo "error: a shipped module does not carry the release commit ${SHA}" >&2
  exit 1
fi
echo "every Linux module carries ${SHA}"
