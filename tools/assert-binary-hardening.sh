#!/bin/bash
# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2024-2026 We-Amp B.V.

#
# assert-binary-hardening.sh — verify a shipped binary carries the
# binary-hardening flags from the .bazelrc :hardening config. Run at release time
# on each built .so, BEFORE stripping/packaging, so a shipped artifact can never
# silently regress to un-hardened. Needs only readelf (binutils).
#
# Asserts the four properties verified to land on the linked output:
#   1. Full RELRO   — a GNU_RELRO segment is present, and
#   2. BIND_NOW     — DT_FLAGS BIND_NOW / DT_FLAGS_1 NOW (GOT resolved + RO at load)
#   3. Stack canary — __stack_chk_fail is referenced (-fstack-protector-strong)
#   4. FORTIFY      — fortified *_chk symbols are referenced (_FORTIFY_SOURCE=2)
#
# NOT asserted: CET/BTI GNU-property notes. Branch-protection (-fcf-protection /
# -mbranch-protection) instruments our code but the binary-level enforcement note
# is only set when every statically linked object is branch-protection-compiled,
# which the vendored/prebuilt libs are not. See the :hardening comment in .bazelrc.
#
# Usage: assert-binary-hardening.sh <path-to-binary>
set -euo pipefail

BIN="${1:?usage: assert-binary-hardening.sh <binary>}"
[ -f "${BIN}" ] || { echo "FAIL: ${BIN} not found" >&2; exit 1; }

echo "=== binary-hardening assertion: ${BIN} ==="
RC=0

# Capture readelf output ONCE into variables, then match in-memory. Piping a
# large `readelf -sW` into `grep -q` under `set -o pipefail` is a trap: grep
# closes the pipe on first match, readelf gets SIGPIPE (nonzero), and pipefail
# makes the `if` see failure even though the symbol WAS found. Matching against
# captured strings avoids the pipe entirely.
RELF_L=$(readelf -lW "${BIN}" 2>/dev/null || true)   # program headers (RELRO)
RELF_D=$(readelf -dW "${BIN}" 2>/dev/null || true)   # dynamic section (BIND_NOW)
RELF_S=$(readelf -sW "${BIN}" 2>/dev/null || true)   # symbols (canary, FORTIFY)

# 1. Full RELRO: GNU_RELRO program header.
if [[ "${RELF_L}" == *GNU_RELRO* ]]; then
  echo "OK: GNU_RELRO segment present (RELRO)."
else
  echo "FAIL: no GNU_RELRO segment — missing -Wl,-z,relro." >&2
  RC=1
fi

# 2. BIND_NOW: DT_FLAGS BIND_NOW or DT_FLAGS_1 NOW.
if [[ "${RELF_D}" == *BIND_NOW* || "${RELF_D}" == *"Flags: NOW"* ]]; then
  echo "OK: BIND_NOW set (full RELRO — GOT resolved + read-only at load)."
else
  echo "FAIL: BIND_NOW not set — missing -Wl,-z,now (RELRO is partial)." >&2
  RC=1
fi

# 3. Stack canary: __stack_chk_fail reference.
if [[ "${RELF_S}" == *__stack_chk_fail* ]]; then
  echo "OK: stack canaries present (__stack_chk_fail)."
else
  echo "FAIL: no __stack_chk_fail — missing -fstack-protector-strong." >&2
  RC=1
fi

# 4. FORTIFY: at least one fortified *_chk symbol. grep -c reads the whole
# here-string (no early pipe close), so this is pipefail-safe.
NCHK=$(grep -coE '__[a-z]+_chk' <<<"${RELF_S}" || true)
if [ "${NCHK:-0}" -gt 0 ]; then
  echo "OK: ${NCHK} fortified *_chk symbol(s) (_FORTIFY_SOURCE=2)."
else
  echo "FAIL: no fortified *_chk symbols — _FORTIFY_SOURCE=2 not in effect." >&2
  RC=1
fi

if [ "${RC}" -ne 0 ]; then
  echo "=== binary-hardening assertion FAILED for ${BIN} ===" >&2
  exit 1
fi
echo "=== binary-hardening assertion PASSED ==="
