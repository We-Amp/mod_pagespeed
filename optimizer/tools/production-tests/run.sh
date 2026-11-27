#!/bin/bash

# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2024-2026 We-Amp B.V.

# Production Readiness Test Runner
#
# Usage:
#   ./run.sh                          # Start stack + run all tests
#   ./run.sh -k "test_status"         # Run specific tests
#   ./run.sh -m p0                    # P0 tests only
#   ./run.sh --no-teardown            # Leave stack running after tests
#   ./run.sh --stack-only             # Only start stack, don't run tests

set -e
cd "$(dirname "$0")"

NO_TEARDOWN=0
NO_BUILD=0
STACK_ONLY=0
PYTEST_ARGS=()

# Parse our flags, pass everything else to pytest
while [[ $# -gt 0 ]]; do
    case "$1" in
        --no-teardown)
            NO_TEARDOWN=1
            shift
            ;;
        --no-build)
            NO_BUILD=1
            shift
            ;;
        --stack-only)
            STACK_ONLY=1
            shift
            ;;
        *)
            PYTEST_ARGS+=("$1")
            shift
            ;;
    esac
done

cleanup() {
    if [ "$NO_TEARDOWN" = "0" ] && [ "$STACK_ONLY" = "0" ]; then
        echo "Collecting logs..."
        docker compose logs > test-logs.txt 2>&1 || true
        echo "Tearing down stack..."
        docker compose down -v 2>/dev/null || true
    fi
}
trap cleanup EXIT

echo "=== Building and starting production test stack ==="
if [ "$NO_BUILD" = "0" ]; then
    docker compose build
fi
docker compose up -d

echo "=== Waiting for services to be healthy ==="
for i in $(seq 1 60); do
    if docker compose ps --format json | python3 -c "
import sys, json
services = [json.loads(line) for line in sys.stdin if line.strip()]
all_healthy = all(s.get('Health','') == 'healthy' for s in services if s.get('Health'))
running = all(s.get('State','') == 'running' for s in services)
sys.exit(0 if (all_healthy and running) else 1)
" 2>/dev/null; then
        echo "All services healthy after ${i}s"
        break
    fi
    if [ "$i" = "60" ]; then
        echo "ERROR: Services not healthy after 60s"
        docker compose ps
        docker compose logs
        exit 1
    fi
    sleep 1
done

# Extra sleep for worker to fully initialize
sleep 3

if [ "$STACK_ONLY" = "1" ]; then
    echo "=== Stack is running. Use 'docker compose down -v' to stop. ==="
    trap - EXIT  # Don't cleanup on exit
    exit 0
fi

echo "=== Running production readiness tests ==="
RESULT=0
pytest tests/ -v --tb=short "${PYTEST_ARGS[@]}" || RESULT=$?

if [ "$RESULT" -ne 0 ]; then
    echo ""
    echo "=== TEST FAILURES ==="
    echo "Logs saved to test-logs.txt"
    echo "Stack left running for debugging (use 'docker compose down -v' to stop)"
    NO_TEARDOWN=1
fi

exit $RESULT
