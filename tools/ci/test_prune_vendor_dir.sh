#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2024-2026 We-Amp B.V.
#
# Test for tools/ci/prune-vendor-dir.sh. Hermetic (plain files in a temp dir).
#   1. aged files go, fresh ones and the kept SHA stay; a sidecar goes with
#      its tarball; an aged temp file goes, a fresh one stays.
#   2. a clock that disagrees with the files' times disables the prune.
#   3. a missing directory is not an error.
#
# Run: tools/ci/test_prune_vendor_dir.sh

set -euo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PRUNE="${SCRIPT_DIR}/prune-vendor-dir.sh"
T="$(mktemp -d)"; trap 'rm -rf "$T"' EXIT
FAILS=0
pass() { echo "PASS: $*"; }
fail() { echo "FAIL: $*"; FAILS=$((FAILS + 1)); }
old() { touch -t 202001010000 "$@"; }

D="$T/vendor"; mkdir -p "$D"
for f in p-aaaaaaa.tar.zst p-aaaaaaa.tar.zst.sha256 p-bbbbbbb.tar.zst p-bbbbbbb.tar.zst.sha256 \
         p-ccccccc.tar.zst p-ccccccc.tar.zst.sha256 p-ddddddd.tar.zst.5-1.tmp p-eeeeeee.tar.zst.5-1.tmp other.txt; do
  echo x > "$D/$f"
done
old "$D/p-aaaaaaa.tar.zst" "$D/p-aaaaaaa.tar.zst.sha256" "$D/p-bbbbbbb.tar.zst" "$D/p-bbbbbbb.tar.zst.sha256" \
    "$D/p-ddddddd.tar.zst.5-1.tmp" "$D/other.txt"
bash "$PRUNE" "$D" p aaaaaaa 120 >/dev/null
left="$(cd "$D" && ls | tr '\n' ' ')"
if [ "$left" = "other.txt p-aaaaaaa.tar.zst p-aaaaaaa.tar.zst.sha256 p-ccccccc.tar.zst p-ccccccc.tar.zst.sha256 p-eeeeeee.tar.zst.5-1.tmp " ]; then
  pass "aged files pruned; kept SHA, fresh files and unrelated files stay"
else
  fail "prune result: $left"
fi

# 2. skew: a stat that reports a time an hour in the past
mkdir -p "$T/shim"
cat > "$T/shim/stat" <<SH
#!/bin/sh
echo \$(( \$(date +%s) - 3600 ))
SH
chmod +x "$T/shim/stat"
old "$D/p-ccccccc.tar.zst"
out="$(PATH="$T/shim:$PATH" bash "$PRUNE" "$D" p - 120 2>&1)"
if [ -e "$D/p-ccccccc.tar.zst" ] && printf '%s' "$out" | grep -q 'not pruning'; then
  pass "a skewed clock disables the prune"
else
  fail "skew guard: $out"
fi
bash "$PRUNE" "$D" p - 120 >/dev/null
[ ! -e "$D/p-ccccccc.tar.zst" ] && pass "keep '-' prunes every aged tarball" || fail "keep '-' left an aged tarball"

# 3. missing dir
bash "$PRUNE" "$T/absent" p - 120 >/dev/null && pass "missing directory is fine" || fail "missing directory failed"

[ "$FAILS" -eq 0 ] || { echo "$FAILS case(s) failed"; exit 1; }
echo "All prune-vendor-dir cases passed"
