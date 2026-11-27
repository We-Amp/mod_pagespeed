#!/bin/bash

# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2024-2026 We-Amp B.V.

# Run test coverage inside Docker and generate HTML report.
# Usage: ./tools/docker-coverage.sh [targets...]
# Examples:
#   ./tools/docker-coverage.sh                      # All tests
#   ./tools/docker-coverage.sh //test/lib/base/...  # Specific tests

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(dirname "$SCRIPT_DIR")"

TARGETS=("${@:-//...}")

echo "=== Running coverage in Docker: ${TARGETS[*]} ==="

# Source mounted read-write so the HTML report lands on the host.
# Some tests may fail (e.g. browser tests needing Chrome); we still
# generate the report from whatever coverage data was collected.
"$PROJECT_ROOT/docker/run-bazel.sh" --rw --no-ssh -- \
    bash -c "
        bazel coverage --config=docker --symlink_prefix=/dev/null/ \"\$@\" || true
        LCOV_REPORT=\"\$(bazel info output_path)/_coverage/_coverage_report.dat\"
        if [ ! -f \"\$LCOV_REPORT\" ] || [ ! -s \"\$LCOV_REPORT\" ]; then
            echo 'ERROR: No coverage data collected'
            exit 1
        fi
        # Strip ML-generated predictor code (287K lines, not meaningful to cover).
        # 'inconsistent' and 'unmapped' are lcov 2.x-only error names; on lcov
        # 1.x genhtml exits with \"unknown argument for --ignore-errors\" when it
        # sees them. Detect the major version once and pass these options only
        # when supported.
        LCOV_MAJOR=\$(lcov --version 2>/dev/null | head -1 | awk '{print \$NF}' | cut -d. -f1)
        LCOV_IGNORE=()
        GENHTML_IGNORE=()
        if [ \"\${LCOV_MAJOR:-0}\" -ge 2 ] 2>/dev/null; then
            LCOV_IGNORE=(--ignore-errors 'inconsistent,inconsistent')
            GENHTML_IGNORE=(--ignore-errors 'inconsistent,inconsistent'
                            --ignore-errors 'unmapped,unmapped')
        fi
        FILTERED=\"\$(mktemp)\"
        lcov --remove \"\$LCOV_REPORT\" '*/lib/image/generated/*' 'lib/image/generated/*' \
            --output-file \"\$FILTERED\" --quiet \
            \"\${LCOV_IGNORE[@]}\" 2>/dev/null || true
        if [ ! -s \"\$FILTERED\" ]; then
            cp \"\$LCOV_REPORT\" \"\$FILTERED\"
        fi
        rm -rf coverage-html
        genhtml \"\$FILTERED\" --output-directory coverage-html --quiet \
            \"\${GENHTML_IGNORE[@]}\"
        rm -f \"\$FILTERED\"
        echo ''
        echo 'Coverage report generated.'
    " _ "${TARGETS[@]}"

echo ""
echo "Coverage report: $PROJECT_ROOT/coverage-html/index.html"
