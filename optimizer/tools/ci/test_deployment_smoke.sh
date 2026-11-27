#!/bin/bash

# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2024-2026 We-Amp B.V.

# CI smoke test: Verify deploy/docker-compose.yml starts and serves traffic.
# Tests the "Try it in 60 seconds" promise.
#
# Prerequisites: docker, docker compose, curl
# Usage: ./tools/ci/test_deployment_smoke.sh

set -euo pipefail

COMPOSE_FILE="deploy/docker-compose.yml"
PROJECT_NAME="pagespeed-smoke-$$"
TIMEOUT=60
NGINX_PORT=8080

cleanup() {
  echo "Cleaning up..."
  docker compose -f "$COMPOSE_FILE" -p "$PROJECT_NAME" down -v --remove-orphans 2>/dev/null || true
}
trap cleanup EXIT

if [ ! -f "$COMPOSE_FILE" ]; then
  echo "ERROR: $COMPOSE_FILE not found"
  exit 1
fi

echo "=== Starting services ==="
docker compose -f "$COMPOSE_FILE" -p "$PROJECT_NAME" up -d

echo "=== Waiting for nginx to be healthy (up to ${TIMEOUT}s) ==="
ELAPSED=0
while [ $ELAPSED -lt $TIMEOUT ]; do
  if curl -sf -o /dev/null "http://localhost:${NGINX_PORT}/" 2>/dev/null; then
    echo "Nginx responding after ${ELAPSED}s"
    break
  fi
  sleep 2
  ELAPSED=$((ELAPSED + 2))
done

if [ $ELAPSED -ge $TIMEOUT ]; then
  echo "ERROR: Nginx did not become healthy within ${TIMEOUT}s"
  docker compose -f "$COMPOSE_FILE" -p "$PROJECT_NAME" logs
  exit 1
fi

echo ""
echo "=== First request (expect MISS) ==="
HEADERS=$(curl -sI "http://localhost:${NGINX_PORT}/")
echo "$HEADERS" | head -10

if echo "$HEADERS" | grep -qi "X-PageSpeed: MISS"; then
  echo "OK: First request returned X-PageSpeed: MISS"
elif echo "$HEADERS" | grep -qi "X-PageSpeed:"; then
  echo "OK: X-PageSpeed header present"
else
  echo "WARNING: X-PageSpeed header not found (origin may not be configured)"
fi

echo ""
echo "=== Second request (may be HIT) ==="
sleep 2
HEADERS2=$(curl -sI "http://localhost:${NGINX_PORT}/")
if echo "$HEADERS2" | grep -qi "X-PageSpeed: HIT"; then
  echo "OK: Second request returned X-PageSpeed: HIT"
else
  echo "INFO: Second request not yet a HIT (worker may still be processing)"
fi

echo ""
echo "=== Deployment smoke test passed ==="
