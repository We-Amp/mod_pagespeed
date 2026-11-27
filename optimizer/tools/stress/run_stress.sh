#!/bin/bash

# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2024-2026 We-Amp B.V.

# mod_pagespeed Stress Test Runner
#
# Builds Docker images, generates test data, starts the stack, runs pytest,
# saves logs, and tears down.
#
# Usage:
#   ./tools/stress/run_stress.sh                     # Run all stress tests
#   ./tools/stress/run_stress.sh -k "test_http_load" # Run one category
#   ./tools/stress/run_stress.sh -k "test_chaos"     # Run chaos tests only

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
COMPOSE_FILE="${SCRIPT_DIR}/docker-compose.yml"
LOG_FILE="${SCRIPT_DIR}/stress_test_logs.txt"
TESTDATA_DIR="${SCRIPT_DIR}/testdata/generated"

# Pass remaining args to pytest
PYTEST_ARGS=("$@")

cleanup() {
    echo "=== Saving Docker logs ==="
    docker compose -f "${COMPOSE_FILE}" logs --no-color > "${LOG_FILE}" 2>&1 || true
    echo "Logs saved to ${LOG_FILE}"

    echo "=== Tearing down ==="
    docker compose -f "${COMPOSE_FILE}" down --remove-orphans -v 2>/dev/null || true

    # Remove generated testdata to avoid leaving root-owned files on CI runners
    # (when this script runs inside a Docker container, files are created as root)
    echo "=== Cleaning generated testdata ==="
    rm -rf "${TESTDATA_DIR}"
}
trap cleanup EXIT

# Step 1: Generate test data if needed
if [ ! -d "${TESTDATA_DIR}" ] || [ -z "$(ls -A "${TESTDATA_DIR}" 2>/dev/null)" ]; then
    echo "=== Generating test data ==="
    python3 "${SCRIPT_DIR}/testdata/generate_testdata.py"
fi

# Step 2: Build Docker images (skip if STRESS_NO_BUILD=1, e.g., when CI pre-builds on host)
if [ "${STRESS_NO_BUILD:-0}" != "1" ]; then
    echo "=== Building Docker images ==="
    DOCKER_BUILDKIT=1 docker compose -f "${COMPOSE_FILE}" build --ssh default
else
    echo "=== Skipping Docker build (STRESS_NO_BUILD=1) ==="
fi

# Step 3: Start services
echo "=== Starting services ==="
docker compose -f "${COMPOSE_FILE}" up -d

# Step 4: Wait for nginx to be ready
echo "=== Waiting for services ==="
TIMEOUT=60
ELAPSED=0
while [ $ELAPSED -lt $TIMEOUT ]; do
    if curl -sf "http://localhost:${STRESS_NGINX_PORT:-8190}/" > /dev/null 2>&1; then
        echo "Nginx ready after ${ELAPSED}s"
        break
    fi
    sleep 1
    ELAPSED=$((ELAPSED + 1))
done
if [ $ELAPSED -ge $TIMEOUT ]; then
    echo "ERROR: Nginx not ready after ${TIMEOUT}s"
    docker compose -f "${COMPOSE_FILE}" logs
    exit 1
fi

# Give worker a moment to start
sleep 2

# Step 5: Run pytest
echo "=== Running stress tests ==="
cd "${SCRIPT_DIR}"
python3 -m pytest . -v --tb=short "${PYTEST_ARGS[@]+"${PYTEST_ARGS[@]}"}"
RESULT=$?

echo "=== Stress tests completed (exit code: ${RESULT}) ==="
exit ${RESULT}
