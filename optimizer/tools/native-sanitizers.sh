#!/bin/bash

# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2024-2026 We-Amp B.V.

# Run sanitizer tests natively (outside Docker).
# Sets up absolute suppressions paths (Bazel sandbox prevents relative paths).
#
# Usage:
#   ./tools/native-sanitizers.sh              # Run all (ASan+UBSan+LSan, TSan)
#   ./tools/native-sanitizers.sh asan         # ASan+UBSan+LSan only
#   ./tools/native-sanitizers.sh tsan         # TSan only

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(dirname "$SCRIPT_DIR")"

LSAN_SUPP="$PROJECT_ROOT/tools/lsan_suppressions.txt"
TSAN_SUPP="$PROJECT_ROOT/tools/tsan_suppressions.txt"

EXCLUDES=(
  -//test/src/worker:image_transcoder_test
  -//test/src/worker:worker_test
  -//test/e2e:pipeline_test
  -//test/lib/image:gif_reader_test
)

run_asan() {
  echo "=== Running ASan + UBSan + LeakSanitizer (native) ==="
  bazel test --config=asan \
    --test_env="ASAN_OPTIONS=check_initialization_order=1:detect_leaks=1" \
    --test_env="LSAN_OPTIONS=suppressions=$LSAN_SUPP" \
    --keep_going --test_output=errors \
    -- //... "${EXCLUDES[@]}"
}

run_tsan() {
  echo "=== Running TSan (native) ==="
  # Exclude static_file_handler_test: UV loop timing test, not thread-safety
  # relevant. Flakes under TSan due to server-restart race. Tested under ASan.
  bazel test --config=tsan \
    --test_env="TSAN_OPTIONS=halt_on_error=1:suppressions=$TSAN_SUPP" \
    --keep_going --test_output=errors \
    -- //... "${EXCLUDES[@]}" \
    -//test/src/worker:static_file_handler_test
}

MODE="${1:-all}"
case "$MODE" in
  asan) run_asan ;;
  tsan) run_tsan ;;
  all)  run_asan; echo; run_tsan ;;
  *)    echo "Usage: $0 [asan|tsan|all]"; exit 1 ;;
esac
echo
echo "All sanitizer tests passed."
