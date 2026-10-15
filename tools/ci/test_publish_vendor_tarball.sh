#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2024-2026 We-Amp B.V.
#
# Test for tools/ci/publish-vendor-tarball.sh. Hermetic (plain files in a
# temp dir). Cases:
#
#   1. publish     a free name: the temp file becomes the tarball, no temp
#                  file is left, and the sidecar holds its digest.
#   2. adopt       the name is taken (a same-SHA writer was first): the
#                  existing bytes are kept, this run's temp file is
#                  discarded, and the sidecar describes the kept bytes.
#   3. stale-sidecar a sidecar describing other bytes is corrected.
#   4. sidecar-only no temp argument: only the sidecar is (re)written.
#   5. race        eight writers publish the same name at once: the tarball
#                  is one writer's bytes and the sidecar matches it.
#   6. bad-input   a missing temp file fails loud.
#   7. no-hardlinks `ln` fails (a filesystem without hard links): the temp
#                  file is renamed onto the free name instead.
#
# Run: tools/ci/test_publish_vendor_tarball.sh

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PUBLISH="${SCRIPT_DIR}/publish-vendor-tarball.sh"

T="$(mktemp -d)"
trap 'rm -rf "$T"' EXIT

FAILS=0
pass() { echo "PASS: $*"; }
fail() { echo "FAIL: $*"; FAILS=$((FAILS + 1)); }
digest() { sha256sum "$1" | cut -d' ' -f1; }
sidecar_matches() {
  [ "$(cut -d' ' -f1 "$1.sha256")" = "$(digest "$1")" ] \
    && [ "$(cut -d' ' -f3 "$1.sha256")" = "$(basename "$1")" ]
}
leftovers() { find "$T" -name '*.tmp' | wc -l | tr -d ' '; }

# 1. publish
F="$T/vendor-aaaaaaa.tar.zst"
printf 'writer-1\n' > "$F.1.tmp"
bash "$PUBLISH" "$F" "$F.1.tmp" 2>/dev/null
if [ "$(cat "$F")" = "writer-1" ] && sidecar_matches "$F" && [ "$(leftovers)" = 0 ]; then
  pass "published; sidecar matches; no temp left"
else
  fail "publish: content='$(cat "$F" 2>/dev/null)' leftovers=$(leftovers)"
fi

# 2. adopt
printf 'writer-2-different-bytes\n' > "$F.2.tmp"
bash "$PUBLISH" "$F" "$F.2.tmp" 2>/dev/null
if [ "$(cat "$F")" = "writer-1" ] && sidecar_matches "$F" && [ "$(leftovers)" = 0 ]; then
  pass "existing tarball adopted, never replaced; temp discarded"
else
  fail "adopt: content='$(cat "$F")' leftovers=$(leftovers)"
fi

# 3. stale-sidecar
printf '%s  %s\n' "$(printf x | sha256sum | cut -d' ' -f1)" "$(basename "$F")" > "$F.sha256"
bash "$PUBLISH" "$F" 2>/dev/null
if sidecar_matches "$F"; then
  pass "stale sidecar corrected"
else
  fail "stale sidecar kept"
fi

# 4. sidecar-only
G="$T/vendor-bbbbbbb.tar.zst"
printf 'reused\n' > "$G"
bash "$PUBLISH" "$G" 2>/dev/null
if [ "$(cat "$G")" = "reused" ] && sidecar_matches "$G"; then
  pass "sidecar-only mode"
else
  fail "sidecar-only mode"
fi

# 5. race
H="$T/vendor-ccccccc.tar.zst"
PIDS=()
for n in 1 2 3 4 5 6 7 8; do
  head -c 200000 /dev/urandom > "$H.$n.tmp"
  GITHUB_RUN_ID="$n" GITHUB_RUN_ATTEMPT=1 bash "$PUBLISH" "$H" "$H.$n.tmp" 2>/dev/null &
  PIDS+=("$!")
done
RC=0
for p in "${PIDS[@]}"; do wait "$p" || RC=1; done
if [ "$RC" = 0 ] && sidecar_matches "$H" && [ "$(leftovers)" = 0 ]; then
  pass "eight concurrent writers: one tarball, matching sidecar"
else
  fail "race: rc=$RC leftovers=$(leftovers)"
fi

# 6. bad-input
if OUT=$(bash "$PUBLISH" "$T/vendor-ddddddd.tar.zst" "$T/nope.tmp" 2>&1); then
  fail "missing temp did not fail"
elif printf '%s' "$OUT" | grep -q '::error::'; then
  pass "missing temp fails loud"
else
  fail "missing temp failed without ::error:: ($OUT)"
fi

# 7. no-hardlinks
mkdir -p "$T/noln"
printf '#!/bin/sh\nexit 1\n' > "$T/noln/ln"; chmod +x "$T/noln/ln"
K="$T/vendor-eeeeeee.tar.zst"
printf 'renamed\n' > "$K.1.tmp"
PATH="$T/noln:$PATH" bash "$PUBLISH" "$K" "$K.1.tmp" 2>/dev/null
if [ "$(cat "$K")" = "renamed" ] && sidecar_matches "$K" && [ "$(leftovers)" = 0 ]; then
  pass "rename fallback without hard links"
else
  fail "rename fallback: content='$(cat "$K" 2>/dev/null)' leftovers=$(leftovers)"
fi

if [ "$FAILS" -ne 0 ]; then
  echo "$FAILS case(s) failed"
  exit 1
fi
echo "All publish-vendor-tarball cases passed"
