#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
# Smoke tests for invalidate-stale-artifacts.sh.
set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
SCRIPT="$HERE/invalidate-stale-artifacts.sh"

mkroot() { mktemp -d "${TMPDIR:-/tmp}/invalidate-test.XXXXXX"; }
write_meta() { mkdir -p "$1"; printf '{"commit":"%s","branch":"main","run":"x"}\n' "$2" > "$1/metadata.json"; }

pass() { echo "ok: $1"; }
fail() { echo "FAIL: $1" >&2; exit 1; }

# 1: matching SHA — staging survives
ROOT=$(mkroot); STAGING="$ROOT/a"
write_meta "$STAGING" "abc1234567890"
touch "$STAGING/libpagespeed.so"
"$SCRIPT" "$STAGING" "abc1234567890" >/dev/null
[ -f "$STAGING/libpagespeed.so" ] || fail "1: staging wiped when SHAs match"
pass "1: matching SHA preserves staging"
rm -rf "$ROOT"

# 2: mismatched SHA — staging wiped
ROOT=$(mkroot); STAGING="$ROOT/a"
write_meta "$STAGING" "abc1234567890"
touch "$STAGING/libpagespeed.so"
"$SCRIPT" "$STAGING" "deadbeef1234" >/dev/null 2>&1
[ ! -e "$STAGING" ] || fail "2: staging survived wipe when SHAs mismatch"
pass "2: mismatched SHA invalidates staging"
rm -rf "$ROOT"

# 3: missing metadata — no-op, staging untouched
ROOT=$(mkroot); STAGING="$ROOT/a"
mkdir -p "$STAGING"; touch "$STAGING/libpagespeed.so"
"$SCRIPT" "$STAGING" "abc1234567890" >/dev/null
[ -f "$STAGING/libpagespeed.so" ] || fail "3: staging without metadata was wiped"
pass "3: missing metadata is a no-op"
rm -rf "$ROOT"

# 4: absent staging dir — no-op, exits 0
ROOT=$(mkroot); STAGING="$ROOT/missing"
"$SCRIPT" "$STAGING" "abc1234567890" >/dev/null
pass "4: absent staging dir is a no-op"
rm -rf "$ROOT"

# 5: malformed metadata (no commit key) — treated as mismatch, wiped
ROOT=$(mkroot); STAGING="$ROOT/a"
mkdir -p "$STAGING"; echo '{"branch":"main"}' > "$STAGING/metadata.json"
touch "$STAGING/libpagespeed.so"
"$SCRIPT" "$STAGING" "abc1234567890" >/dev/null 2>&1
[ ! -e "$STAGING" ] || fail "5: malformed metadata wasn't wiped"
pass "5: malformed metadata invalidates staging"
rm -rf "$ROOT"

# 6: argument check — exits non-zero without args
set +e
"$SCRIPT" >/dev/null 2>&1; rc=$?
set -e
[ "$rc" != "0" ] || fail "6: missing args should exit non-zero"
pass "6: missing args rejected"

# 7: pretty-printed JSON (whitespace around ':') — commit extracts correctly,
#    matching SHA preserves staging
ROOT=$(mkroot); STAGING="$ROOT/a"
mkdir -p "$STAGING"
cat > "$STAGING/metadata.json" <<'EOF'
{
  "commit": "abc1234567890",
  "branch": "main",
  "run": "x"
}
EOF
touch "$STAGING/libpagespeed.so"
"$SCRIPT" "$STAGING" "abc1234567890" >/dev/null
[ -f "$STAGING/libpagespeed.so" ] || fail "7: pretty-printed matching SHA wiped staging"
pass "7: pretty-printed JSON parsed (matching SHA preserves)"
rm -rf "$ROOT"

# 8: nested "commit" key — the FIRST top-level "commit":"..." occurrence wins
#    (guards against greedy-regex regression where a later nested match won).
ROOT=$(mkroot); STAGING="$ROOT/a"
mkdir -p "$STAGING"
printf '{"commit":"abc1234567890","nested":{"commit":"deadbeef1234"}}\n' > "$STAGING/metadata.json"
touch "$STAGING/libpagespeed.so"
"$SCRIPT" "$STAGING" "abc1234567890" >/dev/null
[ -f "$STAGING/libpagespeed.so" ] || fail "8: nested key made extractor pick the wrong commit"
pass "8: nested commit key does not mask top-level"
rm -rf "$ROOT"

echo "ALL PASS"
