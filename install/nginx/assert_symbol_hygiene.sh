#!/bin/bash
#
# assert_symbol_hygiene.sh — deterministic symbol-leak check
# on the built ngx_pagespeed_module.so. Run in BOTH build scripts and reused as
# the Phase C CI gate. Needs only binutils (readelf + nm); no nginx required.
#
# Two assertions, both FAIL (exit non-zero) on violation:
#   1. readelf -d: NO libssl / libcrypto in NEEDED. A leaked OpenSSL symbol
#      colliding with nginx's system OpenSSL segfaults the worker on load —
#      the highest-severity crash mode (R3).
#   2. nm -D --defined-only: the ONLY exported dynamic symbols are the nginx
#      module trio (ngx_modules / ngx_module_names / ngx_module_order). Anything
#      else (spdlog/protobuf/re2/...) means the version script (.lds) did
#      not take effect.
#
# Usage: assert_symbol_hygiene.sh <path-to-module.so>
set -euo pipefail

SO="${1:?usage: assert_symbol_hygiene.sh <module.so>}"
[ -f "${SO}" ] || { echo "FAIL: $SO not found" >&2; exit 1; }

echo "=== D4 symbol-leak assertion: ${SO} ==="
RC=0

# --- 1. NEEDED must not include libssl/libcrypto ----------------------------
NEEDED=$(readelf -d "${SO}" 2>/dev/null | awk '/\(NEEDED\)/{gsub(/[][]/,"",$NF);print $NF}')
echo "-- NEEDED --"; echo "${NEEDED}" | sed 's/^/   /'
if echo "${NEEDED}" | grep -qiE '^lib(ssl|crypto)'; then
  echo "FAIL: module links libssl/libcrypto dynamically — would collide with nginx's system OpenSSL." >&2
  RC=1
else
  echo "OK: no libssl/libcrypto in NEEDED."
fi

# --- 2. exported dynamic symbols must be only the ngx_module trio -----------
# nm -D --defined-only lists exported defined symbols. The version script keeps
# only ngx_modules / ngx_module_names / ngx_module_order global.
EXPORTED=$(nm -D --defined-only "${SO}" 2>/dev/null | awk '{print $3}' | grep -v '^$' || true)
echo "-- exported dynamic symbols --"; echo "${EXPORTED}" | sed 's/^/   /'
UNEXPECTED=$(echo "${EXPORTED}" | grep -vE '^ngx_module' || true)
if [ -n "${UNEXPECTED}" ]; then
  echo "FAIL: unexpected exported symbol(s) (version script did not hide internals):" >&2
  echo "${UNEXPECTED}" | sed 's/^/   /' >&2
  RC=1
else
  echo "OK: only ngx_module* symbols are exported."
fi

if [ "${RC}" -ne 0 ]; then
  echo "=== D4 ASSERTION FAILED for ${SO} ===" >&2
  exit 1
fi
echo "=== D4 assertion PASSED ==="
