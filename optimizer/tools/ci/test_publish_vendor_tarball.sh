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
#   8. partial-temp   an empty temp file is re-checked, then rejected; a
#                     truncated zstd frame is rejected before it can be
#                     published; no final name appears either way.
#   9. digest-mismatch a temp file whose digest is not the expected one is
#                     rejected and nothing is published; the right digest
#                     publishes.
#  10. unsound-final  an existing tarball that is empty, truncated or
#                     disagrees with its sidecar is replaced by a rename
#                     (new inode), the sidecar follows; a sound one keeps its
#                     inode (never rewritten in place).
#  11. private-temp   a temp file outside the shared dir is copied in under a
#                     unique name, compared with its source, then linked;
#                     the source is removed.
#  12. atomic-rename  a reader polling the final name while a large tarball is
#                     published never sees a short file.
#  13. same-run-sibling two jobs of one workflow run (same run id and
#                     attempt) publish into one shared dir: this job's stage
#                     and sidecar temp names differ from the sibling's, so it
#                     never removes or overwrites the sibling's files.
#
# Run: tools/ci/test_publish_vendor_tarball.sh

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PUBLISH="${SCRIPT_DIR}/publish-vendor-tarball.sh"
export PUBLISH_SETTLE_SECS=0

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


HAVE_ZSTD=1; command -v zstd >/dev/null 2>&1 || HAVE_ZSTD=0
inode_of() { ls -i "$1" | awk '{print $1}'; }
mkzst() { head -c "${2:-300000}" /dev/urandom | zstd -q > "$1"; }

# 8. partial-temp
P="$T/vendor-fffffff.tar.zst"
: > "$P.1.tmp"
if OUT=$(bash "$PUBLISH" "$P" "$P.1.tmp" 2>&1); then
  fail "empty temp was published"
elif [ ! -e "$P" ] && printf '%s' "$OUT" | grep -q 'missing or empty'; then
  pass "empty temp rejected after the re-checks; nothing published"
else
  fail "empty temp: final present=$([ -e "$P" ] && echo yes || echo no) ($OUT)"
fi
if [ "$HAVE_ZSTD" = 1 ]; then
  mkzst "$T/whole.zst"; head -c "$(( $(wc -c < "$T/whole.zst") / 2 ))" "$T/whole.zst" > "$P.2.tmp"
  if OUT=$(VENDOR_VERIFY_ZSTD=1 bash "$PUBLISH" "$P" "$P.2.tmp" 2>&1); then
    fail "truncated zstd temp was published"
  elif [ ! -e "$P" ] && [ ! -e "$P.2.tmp" ] && printf '%s' "$OUT" | grep -q 'not a complete zstd frame'; then
    pass "truncated zstd temp rejected and removed; nothing published"
  else
    fail "truncated zstd temp: ($OUT)"
  fi
  rm -f "$P.2.tmp"
fi

# 9. digest-mismatch
D="$T/vendor-1111111.tar.zst"
printf 'bytes\n' > "$D.1.tmp"
if OUT=$(VENDOR_EXPECT_SHA256=$(printf x | sha256sum | cut -d' ' -f1) bash "$PUBLISH" "$D" "$D.1.tmp" 2>&1); then
  fail "temp with the wrong digest was published"
elif [ ! -e "$D" ] && [ ! -e "$D.1.tmp" ] && printf '%s' "$OUT" | grep -q 'not the expected'; then
  pass "digest mismatch rejected and temp removed; nothing published"
else
  fail "digest mismatch: ($OUT)"
fi
printf 'bytes\n' > "$D.1.tmp"
if VENDOR_EXPECT_SHA256="$(digest "$D.1.tmp")" bash "$PUBLISH" "$D" "$D.1.tmp" 2>/dev/null \
   && [ "$(cat "$D")" = bytes ] && sidecar_matches "$D"; then
  pass "expected digest publishes"
else
  fail "expected digest did not publish"
fi

# 10. unsound-final
U="$T/vendor-2222222.tar.zst"
printf 'sound\n' > "$U.1.tmp"; bash "$PUBLISH" "$U" "$U.1.tmp" 2>/dev/null
ino="$(inode_of "$U")"
printf 'other\n' > "$U.2.tmp"; bash "$PUBLISH" "$U" "$U.2.tmp" 2>/dev/null
if [ "$(inode_of "$U")" = "$ino" ] && [ "$(cat "$U")" = sound ]; then
  pass "sound final keeps its inode and bytes"
else
  fail "sound final was touched"
fi
: > "$U"                                  # truncated in place by something else
printf 'fresh\n' > "$U.3.tmp"; bash "$PUBLISH" "$U" "$U.3.tmp" 2>/dev/null
if [ "$(cat "$U")" = fresh ] && sidecar_matches "$U" && [ "$(inode_of "$U")" != "$ino" ]; then
  pass "empty final replaced by rename; sidecar follows"
else
  fail "empty final: content='$(cat "$U")'"
fi
printf 'tampered\n' > "$U.4.tmp"; cp "$U" "$U.keep"; printf 'xx' >> "$U"   # disagrees with its sidecar
bash "$PUBLISH" "$U" "$U.4.tmp" 2>/dev/null
if [ "$(cat "$U")" = tampered ] && sidecar_matches "$U"; then
  pass "final that disagrees with its sidecar replaced"
else
  fail "digest-disagreeing final: content='$(cat "$U")'"
fi
rm -f "$U.keep"

# 11. private-temp
mkdir -p "$T/private" "$T/shared"
V="$T/shared/vendor-3333333.tar.zst"
head -c 500000 /dev/urandom > "$T/private/work.tmp"
want="$(digest "$T/private/work.tmp")"
bash "$PUBLISH" "$V" "$T/private/work.tmp" 2>/dev/null
if [ "$(digest "$V")" = "$want" ] && sidecar_matches "$V" && [ ! -e "$T/private/work.tmp" ] \
   && [ "$(find "$T/shared" -name '*.tmp' | wc -l | tr -d ' ')" = 0 ]; then
  pass "private temp copied in, verified, linked; source and stage removed"
else
  fail "private temp: digest ok=$([ "$(digest "$V")" = "$want" ] && echo y || echo n)"
fi

# 12. atomic-rename
A="$T/shared/vendor-4444444.tar.zst"
head -c 30000000 /dev/urandom > "$T/private/big.tmp"
BIG="$(wc -c < "$T/private/big.tmp" | tr -d ' ')"
( bad=0; for _ in $(seq 1 4000); do
    if [ -e "$A" ]; then
      sz=$(wc -c < "$A" 2>/dev/null | tr -d ' ' || echo 0)
      [ "$sz" = "$BIG" ] || { bad=1; break; }
    fi
  done; echo "$bad" > "$T/reader.out" ) &
reader=$!
bash "$PUBLISH" "$A" "$T/private/big.tmp" 2>/dev/null
wait "$reader"
if [ "$(cat "$T/reader.out")" = 0 ] && [ "$(wc -c < "$A" | tr -d ' ')" = "$BIG" ]; then
  pass "a polling reader never saw a short final file"
else
  fail "a reader saw a short file while publishing"
fi

# 13. same-run-sibling
B="$T/shared/vendor-5555555.tar.zst"
printf 'sibling-stage\n' > "$B.777-1.1.tmp"
printf 'sibling-sidecar\n' > "$B.sha256.777-1.tmp"
head -c 200000 /dev/urandom > "$T/private/sib.tmp"
want="$(digest "$T/private/sib.tmp")"
GITHUB_RUN_ID=777 GITHUB_RUN_ATTEMPT=1 bash "$PUBLISH" "$B" "$T/private/sib.tmp" 2>/dev/null
if [ "$(digest "$B")" = "$want" ] && sidecar_matches "$B" \
   && [ "$(cat "$B.777-1.1.tmp" 2>/dev/null)" = "sibling-stage" ] \
   && [ "$(cat "$B.sha256.777-1.tmp" 2>/dev/null)" = "sibling-sidecar" ]; then
  pass "a same-run sibling's stage and sidecar temp files are left alone"
else
  fail "same-run sibling: stage='$(cat "$B.777-1.1.tmp" 2>/dev/null)' sidecar-tmp='$(cat "$B.sha256.777-1.tmp" 2>/dev/null)'"
fi
rm -f "$B.777-1.1.tmp" "$B.sha256.777-1.tmp"

if [ "$FAILS" -ne 0 ]; then
  echo "$FAILS case(s) failed"
  exit 1
fi
echo "All publish-vendor-tarball cases passed"
