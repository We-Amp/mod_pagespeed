#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2024-2026 We-Amp B.V.
#
# Test for tools/ci/prune-bazel-disk-cache.sh. Hermetic (temp dirs, fake files
# with set mtimes).
#   1. the age policy removes only files older than the limit.
#   2. the size policy removes oldest first until ac + cas fit.
#   3. on equal mtime an ac/ entry goes before a cas/ blob.
#   4. nothing outside ac/ and cas/ is touched, however old or large.
#   5. a missing or empty directory is not an error.
#
# Run: tools/ci/test_prune_bazel_disk_cache.sh

set -uo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PRUNE="${SCRIPT_DIR}/prune-bazel-disk-cache.sh"
T="$(mktemp -d)"; trap 'rm -rf "$T"' EXIT
FAILS=0
pass() { echo "PASS: $*"; }
fail() { echo "FAIL: $*"; FAILS=$((FAILS + 1)); }
NOW="$(date +%s)"
# mk <file> <bytes> <age in days>
mk() {
  mkdir -p "$(dirname "$1")"
  head -c "$2" /dev/zero > "$1"
  local when=$(( NOW - $3 * 86400 ))
  touch -d "@${when}" "$1" 2>/dev/null || touch -t "$(date -r "$when" +%Y%m%d%H%M.%S)" "$1"
}
exists() { [ -e "$1" ]; }

# 1. age policy
D="$T/age"
mk "$D/ac/aa/old" 100 10; mk "$D/cas/bb/old" 100 8; mk "$D/cas/bb/new" 100 1
"$PRUNE" "$D" 7d 1G >/dev/null
if ! exists "$D/ac/aa/old" && ! exists "$D/cas/bb/old" && exists "$D/cas/bb/new"; then
  pass "age policy removes only files older than the limit"; else fail "age policy"; fi

# 2. size policy, oldest first (all younger than the age limit)
D="$T/size"
mk "$D/cas/c1" 1000 5; mk "$D/cas/c2" 1000 4; mk "$D/cas/c3" 1000 3; mk "$D/ac/a1" 1000 2
"$PRUNE" "$D" 7d 2500 >/dev/null
if ! exists "$D/cas/c1" && ! exists "$D/cas/c2" && exists "$D/cas/c3" && exists "$D/ac/a1"; then
  pass "size policy removes oldest first until the cache fits"; else fail "size policy"; fi

# 3. tie on mtime: ac before cas
D="$T/tie"
mk "$D/ac/a" 1000 3; mk "$D/cas/c" 1000 3; touch -r "$D/ac/a" "$D/cas/c"
"$PRUNE" "$D" 7d 1500 >/dev/null
if ! exists "$D/ac/a" && exists "$D/cas/c"; then
  pass "an ac/ entry goes before a cas/ blob of the same age"; else fail "ac before cas tie"; fi

# 4. only ac/ and cas/
D="$T/scope"
mk "$D/ac/a" 100 30; mk "$D/cas/c" 100 30
mk "$D/disk/ac/x" 5000 30; mk "$D/tmp/y" 5000 30; mk "$D/gc/z" 5000 30; mk "$D/top" 5000 30
"$PRUNE" "$D" 7d 1 >/dev/null
if ! exists "$D/ac/a" && ! exists "$D/cas/c" && exists "$D/disk/ac/x" && exists "$D/tmp/y" \
   && exists "$D/gc/z" && exists "$D/top"; then
  pass "only ac/ and cas/ are touched"; else fail "scope guarantee"; fi

# 5. missing and empty directories
if "$PRUNE" "$T/nope" 7d 12G >/dev/null && mkdir -p "$T/empty" && "$PRUNE" "$T/empty" 7d 12G >/dev/null; then
  pass "a missing or empty directory is not an error"; else fail "missing directory"; fi

# bad arguments are refused
if ! "$PRUNE" "$T/empty" 7 12G >/dev/null 2>&1; then pass "a malformed age is refused"; else fail "bad age"; fi

if [ "$FAILS" -ne 0 ]; then echo "$FAILS failure(s)"; exit 1; fi
echo "All prune-bazel-disk-cache cases passed"
