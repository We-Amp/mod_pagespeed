#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2024-2026 We-Amp B.V.
#
# Test for tools/ci/prune-bazel-repository-cache.sh. Hermetic (temp dirs, fake
# files with set mtimes).
#   1. the age policy removes only files older than the limit.
#   2. the size policy removes oldest first until the cache fits.
#   3. only content_addressable/ is touched, however old or large the rest.
#   4. a missing or empty directory is not an error.
#   5. a symlink is removed as a link, its target is never followed; a
#      symlinked content_addressable/ is left alone.
#   6. hash directories left empty are removed, others are kept.
#   7. id-* markers survive while their `file` does, and go with it.
#   8. malformed age or size arguments are refused (status 2), nothing
#      deleted; a size with more than six digits is refused, six digits pass.
#   9. a candidate name containing a newline is removed as one file and never
#      yields a delete path outside the cache.
#  10. numbers with leading zeros are read base 10 (08d is 8 days, 02500 bytes).
#
# Run: tools/ci/test_prune_bazel_repository_cache.sh

set -uo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PRUNE="${SCRIPT_DIR}/prune-bazel-repository-cache.sh"
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
CA=content_addressable/sha256

# 1. age policy
D="$T/age"
mk "$D/$CA/aa/file" 100 40; mk "$D/$CA/bb/file" 100 31; mk "$D/$CA/cc/file" 100 1
"$PRUNE" "$D" 30d 1G >/dev/null
if ! exists "$D/$CA/aa/file" && ! exists "$D/$CA/bb/file" && exists "$D/$CA/cc/file"; then
  pass "age policy removes only files older than the limit"; else fail "age policy"; fi

# 2. size policy, oldest first (all younger than the age limit)
D="$T/size"
mk "$D/$CA/h1/file" 1000 5; mk "$D/$CA/h2/file" 1000 4; mk "$D/$CA/h3/file" 1000 3; mk "$D/$CA/h4/file" 1000 2
"$PRUNE" "$D" 30d 2500 >/dev/null
if ! exists "$D/$CA/h1/file" && ! exists "$D/$CA/h2/file" && exists "$D/$CA/h3/file" && exists "$D/$CA/h4/file"; then
  pass "size policy removes oldest first until the cache fits"; else fail "size policy"; fi

# 3. only content_addressable/
D="$T/scope"
mk "$D/$CA/aa/file" 100 90
mk "$D/registry/x" 5000 90; mk "$D/tmp/y" 5000 90; mk "$D/top" 5000 90
"$PRUNE" "$D" 30d 1 >/dev/null
if ! exists "$D/$CA/aa/file" && exists "$D/registry/x" && exists "$D/tmp/y" && exists "$D/top"; then
  pass "only content_addressable/ is touched"; else fail "scope guarantee"; fi

# 4. missing and empty directories
if "$PRUNE" "$T/nope" 30d 8G >/dev/null && mkdir -p "$T/empty" && "$PRUNE" "$T/empty" 30d 8G >/dev/null; then
  pass "a missing or empty directory is not an error"; else fail "missing directory"; fi

# 5. symlinks are never followed
D="$T/link"; mkdir -p "$T/outside/dir"
mk "$T/outside/file" 100 90; mk "$T/outside/dir/inner" 100 90
mk "$D/$CA/real/file" 100 1
ln -s "$T/outside/file" "$D/$CA/flink"; ln -s "$T/outside/dir" "$D/$CA/dlink"
touch -h -d "@$(( NOW - 90 * 86400 ))" "$D/$CA/flink" "$D/$CA/dlink" 2>/dev/null \
  || touch -h -t "$(date -r $(( NOW - 90 * 86400 )) +%Y%m%d%H%M.%S)" "$D/$CA/flink" "$D/$CA/dlink"
"$PRUNE" "$D" 30d 1G >/dev/null
if [ ! -L "$D/$CA/flink" ] && [ ! -L "$D/$CA/dlink" ] && exists "$D/$CA/real/file" \
   && exists "$T/outside/file" && exists "$T/outside/dir/inner"; then
  pass "a symlink is removed as a link and its target is never followed"; else fail "symlink fixture"; fi
D="$T/calink"; mkdir -p "$D" "$T/elsewhere/sha256/zz"
mk "$T/elsewhere/sha256/zz/file" 100 90
ln -s "$T/elsewhere" "$D/content_addressable"
"$PRUNE" "$D" 30d 1 >/dev/null
if exists "$T/elsewhere/sha256/zz/file"; then
  pass "a symlinked content_addressable/ is not followed"; else fail "symlinked content_addressable"; fi

# 6. empty hash directories are removed, non-empty ones kept
D="$T/dirs"
mk "$D/$CA/old/file" 100 90; mk "$D/$CA/new/file" 100 1
"$PRUNE" "$D" 30d 1G >/dev/null
if [ ! -d "$D/$CA/old" ] && exists "$D/$CA/new/file" && [ -d "$D/content_addressable" ]; then
  pass "emptied hash directories are removed, the rest kept"; else fail "empty directory cleanup"; fi

# 7. id-* markers: kept while the archive is kept (however old), removed with it
D="$T/marker"
mk "$D/$CA/keep/file" 100 1; mk "$D/$CA/keep/id-abc" 10 90
mk "$D/$CA/gone/file" 100 90; mk "$D/$CA/gone/id-abc" 10 90
"$PRUNE" "$D" 30d 1G >/dev/null
if exists "$D/$CA/keep/file" && exists "$D/$CA/keep/id-abc" \
   && ! exists "$D/$CA/gone/file" && ! exists "$D/$CA/gone/id-abc" && [ ! -d "$D/$CA/gone" ]; then
  pass "an id-* marker is kept with its archive and removed with it"; else fail "id marker"; fi

# 8. malformed arguments are refused with status 2 and delete nothing
D="$T/bad"; mk "$D/$CA/x/file" 100 90
for args in "30 8G" "30d 8g" "30d 8X" "30d 8GB" "1xd 8G" "30d 1.5G" "30d G" "30d 1000000" "30d 1000000G"; do
  # shellcheck disable=SC2086
  "$PRUNE" "$D" $args >/dev/null 2>&1; rc=$?
  if [ "$rc" -eq 2 ] && exists "$D/$CA/x/file"; then pass "arguments '$args' refused"; else fail "arguments '$args' (rc=$rc)"; fi
done
# the digit cap rejects the seventh digit, not the sixth
"$PRUNE" "$D" 30d 999999 >/dev/null 2>&1; rc=$?
if [ "$rc" -eq 0 ] && ! exists "$D/$CA/x/file"; then
  pass "a six-digit size is accepted"; else fail "six-digit size accepted (rc=$rc)"; fi

# 9. a name containing a newline: the name's second line must not be parsed
#    as its own list record and delete a path outside the cache
D="$T/newline"; OUT="$T/newline-outside"; mkdir -p "$D/$CA" "$OUT"
mk "$OUT/innocent" 100 1; mk "$D/$CA/keep/file" 100 1
EVIL="$D/$CA/hx/file
1000000000 0 $OUT/innocent"
mk "$EVIL" 100 40
"$PRUNE" "$D" 30d 1G >/dev/null
if ! exists "$EVIL" && exists "$D/$CA/keep/file" && exists "$OUT/innocent"; then
  pass "a name with a newline is removed whole, nothing outside is touched"; else fail "newline in file name"; fi

# 10. leading zeros are decimal: 08d means eight days, 02500 means 2500 bytes
D="$T/zeros"
mk "$D/$CA/h1/file" 1000 5; mk "$D/$CA/h2/file" 1000 4; mk "$D/$CA/h3/file" 1000 3; mk "$D/$CA/h4/file" 1000 2
"$PRUNE" "$D" 08d 02500 >/dev/null 2>&1; rc=$?
if [ "$rc" -eq 0 ] && ! exists "$D/$CA/h1/file" && ! exists "$D/$CA/h2/file" \
   && exists "$D/$CA/h3/file" && exists "$D/$CA/h4/file"; then
  pass "leading zeros are read base 10 for age and size"; else fail "leading zeros (rc=$rc)"; fi

if [ "$FAILS" -ne 0 ]; then echo "$FAILS failure(s)"; exit 1; fi
echo "All prune-bazel-repository-cache cases passed"
