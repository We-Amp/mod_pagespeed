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
#   6. a symlink in ac/ is removed as a link, its target is never followed.
#   7. malformed age or size arguments are refused (status 2), nothing
#      deleted; an age with more than five digits or a size with more than
#      six digits is refused, a six-digit size cap is applied.
#   8. a candidate name containing a newline is removed as one file and never
#      yields a delete path outside the cache.
#   9. numbers with leading zeros are read base 10 (08d is 8 days, 02500 bytes).
#  10. with no GNU stat on PATH the perl lister enforces the same age and
#      size policy, and a name containing a newline is still removed whole.
#  11. with neither GNU stat nor perl on PATH the prune skips, exits 0 and
#      deletes nothing.
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

# 6. a symlink inside ac/ pointing outside is removed as a link, never followed
D="$T/link"; mkdir -p "$T/outside/dir"
mk "$T/outside/file" 100 30; mk "$T/outside/dir/inner" 100 30
mk "$D/ac/real" 100 1
ln -s "$T/outside/file" "$D/ac/flink"; ln -s "$T/outside/dir" "$D/ac/dlink"
touch -h -d "@$(( NOW - 30 * 86400 ))" "$D/ac/flink" "$D/ac/dlink" 2>/dev/null \
  || touch -h -t "$(date -r $(( NOW - 30 * 86400 )) +%Y%m%d%H%M.%S)" "$D/ac/flink" "$D/ac/dlink"
"$PRUNE" "$D" 7d 1G >/dev/null
if [ ! -L "$D/ac/flink" ] && [ ! -L "$D/ac/dlink" ] && exists "$D/ac/real" \
   && exists "$T/outside/file" && exists "$T/outside/dir/inner"; then
  pass "a symlink is removed as a link and its target is never followed"; else fail "symlink fixture"; fi

# 7. malformed arguments are refused with status 2 and delete nothing
D="$T/bad"; mk "$D/cas/x" 100 30
for args in "7 12G" "7d 12g" "7d 12X" "7d 12GB" "1xd 12G" "7d 1.5G" "7d G" "7d 1000000" \
            "7d 1000000G" "1000000d 12G" "9223372036854775807d 12G"; do
  # shellcheck disable=SC2086
  "$PRUNE" "$D" $args >/dev/null 2>&1; rc=$?
  if [ "$rc" -eq 2 ] && exists "$D/cas/x"; then pass "arguments '$args' refused"; else fail "arguments '$args' (rc=$rc)"; fi
done
# the size digit cap rejects the seventh digit, not the sixth; the files are
# recent, so only the size cap can remove the one over it
D="$T/six"; mk "$D/cas/big" 300000 1; mk "$D/cas/small" 100 1
"$PRUNE" "$D" 7d 250000 >/dev/null 2>&1; rc=$?
if [ "$rc" -eq 0 ] && ! exists "$D/cas/big" && exists "$D/cas/small"; then
  pass "a six-digit size cap is applied"; else fail "six-digit size cap (rc=$rc)"; fi

# 8. a name containing a newline: the name's second line must not be parsed
#    as its own list record and delete a path outside the cache
D="$T/newline"; OUT="$T/newline-outside"; mkdir -p "$D/cas" "$OUT"
mk "$OUT/innocent" 100 1; mk "$D/cas/good" 100 1
EVIL="$D/cas/evil
1000000000 0 $OUT/innocent"
mk "$EVIL" 100 30
"$PRUNE" "$D" 7d 1G >/dev/null
if ! exists "$EVIL" && exists "$D/cas/good" && exists "$OUT/innocent"; then
  pass "a name with a newline is removed whole, nothing outside is touched"; else fail "newline in file name"; fi

# 9. leading zeros are decimal: 08d means eight days, 02500 means 2500 bytes
D="$T/zeros"
mk "$D/cas/c1" 1000 5; mk "$D/cas/c2" 1000 4; mk "$D/cas/c3" 1000 3; mk "$D/cas/c4" 1000 2
"$PRUNE" "$D" 08d 02500 >/dev/null 2>&1; rc=$?
if [ "$rc" -eq 0 ] && ! exists "$D/cas/c1" && ! exists "$D/cas/c2" && exists "$D/cas/c3" && exists "$D/cas/c4"; then
  pass "leading zeros are read base 10 for age and size"; else fail "leading zeros (rc=$rc)"; fi

# 10/11. hide GNU stat (and gstat): PATH holds only the tools the prune needs
BIN="$T/bin"; mkdir -p "$BIN"
for tool in find sort xargs rm mktemp date; do
  ln -s "$(command -v "$tool")" "$BIN/$tool"
done
BASH_BIN="$(command -v bash)"
D="$T/perl"; OUT="$T/perl-outside"; mkdir -p "$OUT"
mk "$OUT/innocent" 100 1
mk "$D/ac/old" 100 10; mk "$D/cas/c1" 1000 5; mk "$D/cas/c2" 1000 4
mk "$D/cas/c3" 1000 3; mk "$D/cas/c4" 1000 2
EVIL="$D/cas/evil
1000000000 0 $OUT/innocent"
mk "$EVIL" 100 6
ln -s "$(command -v perl)" "$BIN/perl"
LOG="$(PATH="$BIN" "$BASH_BIN" "$PRUNE" "$D" 7d 2500 2>&1)"; rc=$?
if [ "$rc" -eq 0 ] && ! exists "$D/ac/old" && ! exists "$EVIL" && ! exists "$D/cas/c1" \
   && ! exists "$D/cas/c2" && exists "$D/cas/c3" && exists "$D/cas/c4" && exists "$OUT/innocent" \
   && [[ $LOG == *"removed 4 files"* ]]; then
  pass "without GNU stat the perl lister enforces the age and size caps"
else fail "perl lister (rc=$rc): $LOG"; fi
rm -f "$BIN/perl"
D="$T/noperl"; mk "$D/cas/old" 1000 30
LOG="$(PATH="$BIN" "$BASH_BIN" "$PRUNE" "$D" 7d 1 2>&1)"; rc=$?
if [ "$rc" -eq 0 ] && exists "$D/cas/old" && [[ $LOG == *skipping* ]]; then
  pass "without GNU stat and perl the prune skips and exits 0"
else fail "no lister (rc=$rc): $LOG"; fi

if [ "$FAILS" -ne 0 ]; then echo "$FAILS failure(s)"; exit 1; fi
echo "All prune-bazel-disk-cache cases passed"
