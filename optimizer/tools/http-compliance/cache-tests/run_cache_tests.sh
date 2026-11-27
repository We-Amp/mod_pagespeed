#!/bin/bash

# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2024-2026 We-Amp B.V.

# Run IETF HTTP cache-tests (mnot/proxy-cache-tests) against the
# pagespeed nginx proxy.
#
# Prerequisites:
#   - Docker and Docker Compose
#   - pagespeed2-dev image
#
# Usage:
#   ./tools/http-compliance/cache-tests/run_cache_tests.sh
#
# The script:
#   1. Starts the proxy and cache-tests containers
#   2. Runs the test suite
#   3. Compares results against known_failures.json
#   4. Fails if any NEW failures appear

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
COMPOSE_FILE="$SCRIPT_DIR/docker-compose.cache-tests.yml"
KNOWN_FAILURES="$SCRIPT_DIR/known_failures.json"
RESULTS_DIR="$SCRIPT_DIR/results"

mkdir -p "$RESULTS_DIR"

echo "=== Starting IETF cache-tests ==="

# Build and start services
docker compose -f "$COMPOSE_FILE" build
docker compose -f "$COMPOSE_FILE" up --exit-code-from cache-tests 2>&1 | tee "$RESULTS_DIR/output.log"
EXIT_CODE=${PIPESTATUS[0]}

# Capture results
echo ""
echo "=== Cache-tests complete (exit code: $EXIT_CODE) ==="

# Compare against known failures if results file exists
if [ -f "$RESULTS_DIR/results.json" ] && [ -f "$KNOWN_FAILURES" ]; then
    echo "=== Checking for new failures ==="
    python3 -c "
import json, sys

with open('$KNOWN_FAILURES') as f:
    known = set(json.load(f).get('expected_failures', []))

with open('$RESULTS_DIR/results.json') as f:
    results = json.load(f)

failures = set()
for test in results.get('tests', []):
    if test.get('result') == 'fail':
        failures.add(test.get('name', ''))

new_failures = failures - known
resolved = known - failures

if new_failures:
    print(f'NEW FAILURES ({len(new_failures)}):')
    for f in sorted(new_failures):
        print(f'  - {f}')
    sys.exit(1)

if resolved:
    print(f'RESOLVED (can remove from known_failures.json):')
    for f in sorted(resolved):
        print(f'  + {f}')

print(f'Total failures: {len(failures)} (all known)')
"
else
    echo "No results file or known_failures.json found, skipping comparison"
fi

# Cleanup
docker compose -f "$COMPOSE_FILE" down --remove-orphans -v

exit $EXIT_CODE
