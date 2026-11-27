#!/bin/bash

# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2024-2026 We-Amp B.V.

# mod_pagespeed - Real-World Website Proxy Test Runner
#
# Brings up the Docker stack, runs pytest, collects logs, and tears down.
#
# Prerequisites:
#   - Docker and Docker Compose
#   - pagespeed2-dev image (run ./tools/docker-build.sh first)
#   - Python 3 with pytest and requests
#
# Usage:
#   ./tools/realworld-tests/run_realworld.sh              # Run all tests
#   ./tools/realworld-tests/run_realworld.sh -k privacy   # Run only privacy tests
#   ./tools/realworld-tests/run_realworld.sh -v --tb=long # Verbose with full tracebacks

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
COMPOSE_FILE="$SCRIPT_DIR/docker-compose.yml"

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

if ! python3 -c "import pytest, requests" &>/dev/null; then
    echo "Error: pytest and requests required."
    echo "Install with: pip install pytest requests"
    exit 1
fi

cleanup() {
    echo ""
    echo "=== Collecting logs ==="
    docker compose -f "$COMPOSE_FILE" logs --tail=200 worker 2>/dev/null || true
    docker compose -f "$COMPOSE_FILE" logs --tail=200 nginx 2>/dev/null || true
    echo ""
    echo "=== Tearing down ==="
    docker compose -f "$COMPOSE_FILE" down -v --remove-orphans 2>/dev/null || true
}
trap cleanup EXIT

echo "=== Cleaning up previous runs ==="
docker compose -f "$COMPOSE_FILE" down -v --remove-orphans 2>/dev/null || true

echo "=== Starting stack ==="
if [ "${NO_BUILD:-0}" = "1" ]; then
    docker compose -f "$COMPOSE_FILE" up -d --no-build
else
    docker compose -f "$COMPOSE_FILE" up -d --build
fi

echo "=== Waiting for services ==="
# Wait up to 30s for nginx to respond (use local /health endpoint)
nginx_ready=false
deadline=$((SECONDS + 30))
while [ $SECONDS -lt $deadline ]; do
    if curl -sf http://localhost:${REALWORLD_NGINX_PORT:-8083}/health >/dev/null 2>&1; then
        echo "Nginx is ready."
        nginx_ready=true
        break
    fi
    sleep 1
done
if [ "$nginx_ready" = false ]; then
    echo "Error: Nginx not ready after 30s"
    exit 1
fi

# Wait up to 30s for worker API to respond
worker_ready=false
deadline=$((SECONDS + 30))
while [ $SECONDS -lt $deadline ]; do
    if curl -sf http://localhost:${REALWORLD_WORKER_PORT:-9882}/v1/health >/dev/null 2>&1; then
        echo "Worker API is ready."
        worker_ready=true
        break
    fi
    sleep 1
done
if [ "$worker_ready" = false ]; then
    echo "Error: Worker API not ready after 30s"
    exit 1
fi

echo "=== Running tests ==="
cd "$SCRIPT_DIR"
export NGINX_URL="${NGINX_URL:-http://localhost:${REALWORLD_NGINX_PORT:-8083}}"
export WORKER_API_URL="${WORKER_API_URL:-http://localhost:${REALWORLD_WORKER_PORT:-9882}}"
pytest -v --tb=short "$@"
