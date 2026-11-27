#!/bin/bash

# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2024-2026 We-Amp B.V.

# Generate test coverage report (LCOV + HTML).
# Usage: ./tools/coverage.sh [targets...]
# Examples:
#   ./tools/coverage.sh                          # All tests
#   ./tools/coverage.sh //test/lib/base/...      # Specific tests
#   ./tools/coverage.sh //test/lib/html/...      # Specific tests

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(dirname "$SCRIPT_DIR")"

TARGETS=("${@:-//...}")
OUTPUT_DIR="$PROJECT_ROOT/coverage-html"

echo "=== Running coverage: ${TARGETS[*]} ==="
cd "$PROJECT_ROOT"

# Some tests may fail (e.g. browser tests needing Chrome); we still
# generate the report from whatever coverage data was collected.
bazel coverage "${TARGETS[@]}" || true

LCOV_REPORT="$(bazel info output_path)/_coverage/_coverage_report.dat"

if [ ! -f "$LCOV_REPORT" ] || [ ! -s "$LCOV_REPORT" ]; then
    echo "ERROR: No coverage data collected at $LCOV_REPORT"
    exit 1
fi

echo ""
echo "LCOV report: $LCOV_REPORT"

if command -v genhtml &>/dev/null; then
    # Strip ML-generated predictor code (287K lines, not meaningful to cover).
    # lcov v2 returns non-zero on inconsistency warnings even with --ignore-errors,
    # so we ignore exit code and fall back only if the output file is empty/missing.
    #
    # The 'inconsistent' and 'unmapped' error names exist only in lcov 2.x and
    # later; on lcov 1.x genhtml exits with "unknown argument for
    # --ignore-errors" when it sees them. Detect the major version once and
    # pass these options only when supported.
    LCOV_MAJOR="$(lcov --version 2>/dev/null | head -1 | awk '{print $NF}' | cut -d. -f1)"
    LCOV_IGNORE=()
    GENHTML_IGNORE=()
    if [ "${LCOV_MAJOR:-0}" -ge 2 ] 2>/dev/null; then
        LCOV_IGNORE=(--ignore-errors 'inconsistent,inconsistent')
        GENHTML_IGNORE=(--ignore-errors 'inconsistent,inconsistent'
                        --ignore-errors 'unmapped,unmapped')
    fi
    FILTERED_REPORT="$(mktemp)"
    lcov --remove "$LCOV_REPORT" '*/lib/image/generated/*' 'lib/image/generated/*' \
        --output-file "$FILTERED_REPORT" --quiet \
        "${LCOV_IGNORE[@]}" 2>/dev/null || true
    if [ ! -s "$FILTERED_REPORT" ]; then
        cp "$LCOV_REPORT" "$FILTERED_REPORT"
    fi
    echo "Generating HTML report in $OUTPUT_DIR ..."
    rm -rf "$OUTPUT_DIR"
    genhtml "$FILTERED_REPORT" --output-directory "$OUTPUT_DIR" --quiet \
        "${GENHTML_IGNORE[@]}"
    rm -f "$FILTERED_REPORT"
    echo ""
    echo "Coverage report: $OUTPUT_DIR/index.html"
else
    echo ""
    echo "genhtml not found. Install lcov for HTML reports:"
    echo "  macOS:  brew install lcov"
    echo "  Linux:  apt-get install lcov"
    echo ""
    echo "You can still inspect the raw LCOV data at:"
    echo "  $LCOV_REPORT"
fi
