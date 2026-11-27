#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2024-2026 We-Amp B.V.
#
# Hermetic tests for validate-local-bundle.sh (#1825).
set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
SCRIPT="$HERE/validate-local-bundle.sh"
ROOT="$(mktemp -d "${TMPDIR:-/tmp}/validate-bundle-test.XXXXXX")"
trap 'rm -rf "$ROOT"' EXIT

fail() { echo "FAIL: $1" >&2; exit 1; }
pass() { echo "ok: $1"; }
mk() { rm -rf "$ROOT/b"; mkdir -p "$ROOT/b"; }
full() { mk; echo w > "$ROOT/b/factory_worker"; echo m > "$ROOT/b/ngx_pagespeed_module.so"; }
meta() { printf '{"commit":"abc","platform":"%s"}\n' "$1" > "$ROOT/b/metadata.json"; }
ok()  { "$SCRIPT" "$ROOT/b" "${1:-linux-x64}" 2>/dev/null; }

full; meta linux-x64
ok || fail "1: complete matching bundle rejected"; pass "1: complete + matching platform accepted"

full
ok || fail "2: complete bundle without metadata rejected"; pass "2: no metadata (local build) accepted"

mk; echo w > "$ROOT/b/factory_worker"; meta linux-x64
! ok || fail "3: Release-style set (no module .so) accepted"; pass "3: missing ngx_pagespeed_module.so rejected"

mk; echo m > "$ROOT/b/ngx_pagespeed_module.so"
! ok || fail "4: bundle without factory_worker accepted"; pass "4: missing factory_worker rejected"

full; : > "$ROOT/b/ngx_pagespeed_module.so"
! ok || fail "5: zero-byte module accepted"; pass "5: empty file rejected"

full; meta linux-x64-periodic
! ok linux-x64 || fail "6: periodic bundle accepted for linux-x64"; pass "6: platform mismatch rejected"
ok linux-x64-periodic || fail "7: periodic bundle rejected for periodic"; pass "7: periodic bundle accepted for periodic"

full; printf '{\n  "platform" : "linux-x64"\n}\n' > "$ROOT/b/metadata.json"
ok || fail "8: pretty-printed metadata rejected"; pass "8: pretty-printed JSON parsed"

full
if command -v sha256sum >/dev/null 2>&1; then
  (cd "$ROOT/b" && sha256sum factory_worker ngx_pagespeed_module.so > checksums.sha256)
else
  (cd "$ROOT/b" && shasum -a 256 factory_worker ngx_pagespeed_module.so > checksums.sha256)
fi
ok || fail "11: bundle with valid checksums rejected"; pass "11: valid checksums accepted"
echo extra >> "$ROOT/b/factory_worker"
! ok || fail "12: modified/truncated file with stale checksums accepted"; pass "12: checksum mismatch (truncated file) rejected"

rm -rf "$ROOT/b"
! ok || fail "9: absent dir accepted"; pass "9: absent dir rejected"

set +e; "$SCRIPT" >/dev/null 2>&1; rc=$?; set -e
[ "$rc" = 2 ] || fail "10: missing args should exit 2 (got $rc)"; pass "10: missing args rejected"
echo "ALL PASS"
