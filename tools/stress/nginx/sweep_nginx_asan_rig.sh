#!/bin/bash
# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2024-2026 We-Amp B.V.

# Adjudicate ASan / crash evidence after a run. Exit non-zero on any
# hit so CI goes red. ASan writes reports to <RIG_DIR>/logs/asan.<pid> (log_path
# set in run-nginx.sh) AND may emit into error.log; check both, plus the cores dir.
set -uo pipefail

RIG_DIR="${RIG_DIR:-$HOME/nginx-asan-rig}"
HARNESS_RC="${1:-0}"
hits=0

echo "==== harness rc ===="
echo "$HARNESS_RC"
if [[ "$HARNESS_RC" != "0" ]]; then echo "harness reported non-zero -> FAIL"; hits=$((hits+1)); fi

TOKENS='AddressSanitizer|LeakSanitizer|ThreadSanitizer|heap-use-after-free|heap-buffer-overflow|use-after-poison|stack-use-after-return|stack-use-after-scope|SEGV on|runtime error:|SUMMARY: (Address|Undefined|Thread)Sanitizer'

echo "==== ASan report files (<rig>/logs/asan.*) ===="
shopt -s nullglob
asan_files=("$RIG_DIR"/logs/asan.*)
if (( ${#asan_files[@]} )); then
  printf '%s\n' "${asan_files[@]}"
  echo "---- first report (head) ----"; head -120 "${asan_files[0]}"
  echo "ASan report file(s) present -> FAIL"; hits=$((hits+1))
else echo "NONE"; fi

echo "==== ASan/crash tokens in error.log ===="
if grep -aE "$TOKENS" "$RIG_DIR/logs/error.log" 2>/dev/null | tail -20; then
  echo "sanitizer/crash token(s) in error.log -> FAIL"; hits=$((hits+1))
else echo "NONE"; fi

echo "==== core dumps ===="
cores=("$RIG_DIR"/cores/*)
if (( ${#cores[@]} )) && [[ -e "${cores[0]}" ]]; then
  ls -la "$RIG_DIR"/cores/
  echo "core dump(s) present -> FAIL"; hits=$((hits+1))
else echo "NONE"; fi

echo "==== SWEEP DONE: $hits failure signal(s) ===="
[[ $hits -eq 0 ]] || exit 1
