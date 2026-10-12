#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2024-2026 We-Amp B.V.
#
# Test for tools/sbom/install-pinned-tool.sh (issue #859): a sha256 mismatch
# must abort non-zero, with a clear message, before anything is extracted or
# installed. No network: PINNED_TOOL_BASE points at a local file:// tree.
#
# Run: tools/ci/test_install_pinned_tool.sh
set -euo pipefail
here="$(cd "$(dirname "$0")" && pwd)"
script="$here/../sbom/install-pinned-tool.sh"
work="$(mktemp -d)"; trap 'rm -rf "$work"' EXIT
fail() { echo "FAIL: $*" >&2; exit 1; }

ver=v9.9.9
rel="$work/release/syft/releases/download/$ver"
mkdir -p "$rel" "$work/pkg"
printf '#!/bin/sh\necho "Version: 9.9.9"\n' > "$work/pkg/syft"; chmod +x "$work/pkg/syft"
tarball=syft_9.9.9_linux_amd64.tar.gz
tar -czf "$rel/$tarball" -C "$work/pkg" syft
sha() { if command -v sha256sum >/dev/null; then sha256sum "$1"; else shasum -a 256 "$1"; fi | awk '{print $1}'; }
run() { # bindir -> sets $out, returns script status
  PINNED_TOOL_BASE="file://$work/release" PINNED_TOOL_OS=linux PINNED_TOOL_ARCH=amd64 \
    INSTALL_RETRIES=2 INSTALL_RETRY_SLEEP=0 bash "$script" syft "$ver" "$1" 2>&1
}

# 1. correct checksum installs a runnable binary.
echo "$(sha "$rel/$tarball")  $tarball" > "$rel/syft_9.9.9_checksums.txt"
out="$(run "$work/ok")" || fail "good checksum should install: $out"
[ "$("$work/ok/syft")" = "Version: 9.9.9" ] || fail "installed binary does not run"

# 2. wrong checksum aborts non-zero, says so, installs nothing.
echo "$(printf 0%.0s $(seq 64))  $tarball" > "$rel/syft_9.9.9_checksums.txt"
rc=0; out="$(run "$work/bad")" || rc=$?
[ "$rc" -ne 0 ] || fail "wrong checksum must exit non-zero: $out"
echo "$out" | grep -q "sha256 mismatch" || fail "missing mismatch message: $out"
echo "$out" | grep -q "ABORTING" || fail "missing ABORTING message: $out"
[ ! -e "$work/bad/syft" ] || fail "binary installed despite checksum mismatch"

# 3. checksum file lacking the tarball entry also aborts.
echo "deadbeef  other.tar.gz" > "$rel/syft_9.9.9_checksums.txt"
rc=0; out="$(run "$work/missing")" || rc=$?
[ "$rc" -ne 0 ] || fail "missing checksum entry must exit non-zero"
echo "$out" | grep -q "no checksum entry" || fail "missing no-checksum-entry message: $out"
[ ! -e "$work/missing/syft" ] || fail "binary installed without a checksum entry"

echo "PASS: install-pinned-tool aborts on checksum mismatch"
