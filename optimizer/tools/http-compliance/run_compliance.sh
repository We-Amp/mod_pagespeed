#!/bin/bash

# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2024-2026 We-Amp B.V.

# mod_pagespeed - HTTP Compliance Test Runner
#
# Runs HTTP compliance tests against the full stack in Docker.
#
# Prerequisites:
#   - Docker and Docker Compose
#   - pagespeed2-dev image (run ./tools/docker-build.sh first)
#   - Python 3 with pip
#   - hurl (optional, for Hurl tests: brew install hurl)
#
# Usage:
#   ./tools/http-compliance/run_compliance.sh              # All tests
#   ./tools/http-compliance/run_compliance.sh --pytest      # Only pytest
#   ./tools/http-compliance/run_compliance.sh --hurl        # Only hurl
#   ./tools/http-compliance/run_compliance.sh --cache-tests # Only cache-tests
#   ./tools/http-compliance/run_compliance.sh -k content_length  # Specific tests

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
COMPOSE_FILE="$SCRIPT_DIR/docker-compose.yml"

# Generate large test fixtures if missing
"$SCRIPT_DIR/../generate-test-fixtures.sh"

RUN_PYTEST=false
RUN_HURL=false
RUN_CACHE_TESTS=false
PYTEST_ARGS=()
RUN_ALL=true

# Parse arguments
while [[ $# -gt 0 ]]; do
    case "$1" in
        --pytest)
            RUN_PYTEST=true
            RUN_ALL=false
            shift
            ;;
        --hurl)
            RUN_HURL=true
            RUN_ALL=false
            shift
            ;;
        --cache-tests)
            RUN_CACHE_TESTS=true
            RUN_ALL=false
            shift
            ;;
        *)
            PYTEST_ARGS+=("$1")
            shift
            ;;
    esac
done

if $RUN_ALL; then
    RUN_PYTEST=true
    RUN_HURL=true
    RUN_CACHE_TESTS=true
fi

# Check prerequisites
if ! command -v docker &>/dev/null; then
    echo "Error: docker not found. Install Docker first."
    exit 1
fi

if ! docker image inspect pagespeed2-dev &>/dev/null; then
    echo "Error: pagespeed2-dev image not found."
    echo "Build it with: ./tools/docker-build.sh"
    exit 1
fi

# Set up Python virtual environment
VENV_DIR="$SCRIPT_DIR/.venv"
if [ ! -d "$VENV_DIR" ]; then
    python3 -m venv "$VENV_DIR"
fi
# shellcheck disable=SC1091
source "$VENV_DIR/bin/activate"
pip install -q -r "$SCRIPT_DIR/requirements.txt"

EXIT_CODE=0

# --- Pytest ---
if $RUN_PYTEST; then
    echo "=== Running pytest compliance tests ==="
    cd "$SCRIPT_DIR"
    if ! pytest -v --tb=short "${PYTEST_ARGS[@]+"${PYTEST_ARGS[@]}"}"; then
        EXIT_CODE=1
    fi
fi

# --- Hurl ---
if $RUN_HURL; then
    if command -v hurl &>/dev/null; then
        echo ""
        echo "=== Running Hurl compliance tests ==="
        HURL_DIR="$SCRIPT_DIR/hurl"
        if [ -d "$HURL_DIR" ] && compgen -G "$HURL_DIR/*.hurl" > /dev/null; then
            # Ensure services are running (pytest may have started them)
            NGINX_PORT="${COMPLIANCE_NGINX_PORT:-8180}"
            ORIGIN_PORT="${COMPLIANCE_ORIGIN_PORT:-8181}"
            if ! curl -sf "http://localhost:${NGINX_PORT}/small.html" &>/dev/null; then
                echo "Starting Docker services for Hurl tests..."
                docker compose -f "$COMPOSE_FILE" up -d --build
                for i in $(seq 1 15); do
                    if curl -sf "http://localhost:${NGINX_PORT}/small.html" &>/dev/null; then
                        break
                    fi
                    sleep 1
                done
            fi
            if ! hurl --test --variable "host=http://localhost:${NGINX_PORT}" \
                 --variable "origin=http://localhost:${ORIGIN_PORT}" \
                 "$HURL_DIR"/*.hurl; then
                EXIT_CODE=1
            fi
        else
            echo "No .hurl files found in $HURL_DIR"
        fi
    else
        echo ""
        echo "=== Skipping Hurl tests (hurl not installed) ==="
        echo "Install with: brew install hurl"
    fi
fi

# --- Cache Tests (IETF) ---
if $RUN_CACHE_TESTS; then
    CACHE_TESTS_SCRIPT="$SCRIPT_DIR/cache-tests/run_cache_tests.sh"
    if [ -x "$CACHE_TESTS_SCRIPT" ]; then
        echo ""
        echo "=== Running IETF cache-tests ==="
        if ! "$CACHE_TESTS_SCRIPT"; then
            EXIT_CODE=1
        fi
    else
        echo ""
        echo "=== Skipping IETF cache-tests (run_cache_tests.sh not found) ==="
    fi
fi

# Tear down services if we started them
if docker compose -f "$COMPOSE_FILE" ps --quiet 2>/dev/null | head -1 | grep -q .; then
    echo ""
    echo "=== Tearing down services ==="
    docker compose -f "$COMPOSE_FILE" down --remove-orphans -v
fi

exit $EXIT_CODE
