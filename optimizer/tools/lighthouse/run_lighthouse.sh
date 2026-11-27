#!/bin/bash

# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2024-2026 We-Amp B.V.

# mod_pagespeed - Lighthouse A/B Validation Runner
#
# Runs Lighthouse-based A/B tests comparing origin (unoptimized) against
# nginx (optimized) to validate that PageSpeed optimizations improve
# performance scores and audit results.
#
# Prerequisites:
#   - Node.js (for Lighthouse CLI)
#   - npm install -g lighthouse (or npx)
#   - Docker and Docker Compose (for the E2E stack)
#   - Python 3 with pip
#
# Usage:
#   ./tools/lighthouse/run_lighthouse.sh                    # Run all tests
#   ./tools/lighthouse/run_lighthouse.sh -k "scoring"       # Run scoring tests only
#   ./tools/lighthouse/run_lighthouse.sh -v --tb=long       # Verbose output
#   ./tools/lighthouse/run_lighthouse.sh --no-stack         # Skip stack management

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
E2E_DIR="${REPO_ROOT}/tools/e2e"
E2E_COMPOSE_FILE="${E2E_DIR}/docker-compose.yml"

MANAGE_STACK=true
PYTEST_ARGS=()

# Parse our flags, pass the rest to pytest.
for arg in "$@"; do
    case "$arg" in
        --no-stack)
            MANAGE_STACK=false
            ;;
        *)
            PYTEST_ARGS+=("$arg")
            ;;
    esac
done

# --- Prerequisite checks ---

echo "=== Checking prerequisites ==="

if ! command -v node &>/dev/null; then
    echo "Error: node not found. Install Node.js first."
    exit 1
fi
echo "  Node.js: $(node --version)"

if command -v lighthouse &>/dev/null; then
    echo "  Lighthouse: $(lighthouse --version 2>/dev/null || echo 'available')"
elif npx --yes lighthouse --version &>/dev/null 2>&1; then
    echo "  Lighthouse: available via npx"
else
    echo "Error: lighthouse not found."
    echo "Install with: npm install -g lighthouse"
    exit 1
fi

if command -v lhci &>/dev/null; then
    echo "  LHCI: $(lhci --version 2>/dev/null || echo 'available')"
else
    echo "  LHCI: not found (optional, only needed for 'lhci autorun')"
fi

if ! command -v python3 &>/dev/null; then
    echo "Error: python3 not found."
    exit 1
fi
echo "  Python: $(python3 --version)"

# --- Stack management ---

STARTED_STACK=false

cleanup() {
    if [ "$STARTED_STACK" = true ]; then
        echo "=== Tearing down E2E stack ==="
        docker compose -f "${E2E_COMPOSE_FILE}" down --remove-orphans -v 2>/dev/null || true
    fi
}
trap cleanup EXIT

check_services() {
    # Returns 0 if both origin (8081) and nginx (8080) respond.
    curl -sf http://localhost:8080/ >/dev/null 2>&1 && \
    curl -sf http://localhost:8081/ >/dev/null 2>&1
}

if [ "$MANAGE_STACK" = true ]; then
    if check_services; then
        echo "=== E2E stack already running ==="
    else
        echo "=== Starting E2E stack ==="
        if ! command -v docker &>/dev/null; then
            echo "Error: docker not found. Install Docker first."
            exit 1
        fi
        if [ ! -f "${E2E_COMPOSE_FILE}" ]; then
            echo "Error: E2E compose file not found: ${E2E_COMPOSE_FILE}"
            exit 1
        fi

        docker compose -f "${E2E_COMPOSE_FILE}" build
        docker compose -f "${E2E_COMPOSE_FILE}" up -d
        STARTED_STACK=true

        echo "=== Waiting for services ==="
        TIMEOUT=60
        ELAPSED=0
        while [ $ELAPSED -lt $TIMEOUT ]; do
            if check_services; then
                echo "  Services ready after ${ELAPSED}s"
                break
            fi
            sleep 1
            ELAPSED=$((ELAPSED + 1))
        done
        if [ $ELAPSED -ge $TIMEOUT ]; then
            echo "Error: Services not ready after ${TIMEOUT}s"
            docker compose -f "${E2E_COMPOSE_FILE}" logs --tail=50
            exit 1
        fi

        # Give worker time to start listening.
        sleep 2
    fi
else
    echo "=== Stack management disabled (--no-stack) ==="
    if ! check_services; then
        echo "Warning: Services not reachable. Tests may fail."
    fi
fi

# --- Python virtual environment ---

VENV_DIR="${SCRIPT_DIR}/.venv"
if [ ! -d "$VENV_DIR" ]; then
    echo "=== Creating Python venv ==="
    python3 -m venv "$VENV_DIR"
fi
# shellcheck disable=SC1091
source "${VENV_DIR}/bin/activate"
pip install -q -r "${SCRIPT_DIR}/requirements.txt"

# --- Run tests ---

echo "=== Running Lighthouse A/B validation tests ==="
cd "${SCRIPT_DIR}"
set +e
pytest test_lighthouse.py -v -m lighthouse --tb=short "${PYTEST_ARGS[@]+"${PYTEST_ARGS[@]}"}"
RESULT=$?
set -e

# --- Summary ---

echo ""
echo "========================================="
if [ $RESULT -eq 0 ]; then
    echo "  Lighthouse validation: ALL PASSED"
elif [ $RESULT -eq 5 ]; then
    # pytest exit code 5 = no tests collected (all skipped)
    echo "  Lighthouse validation: ALL SKIPPED"
    echo "  (lighthouse CLI not installed or services not running)"
    RESULT=0
else
    echo "  Lighthouse validation: FAILURES DETECTED"
fi
echo "========================================="

exit ${RESULT}
