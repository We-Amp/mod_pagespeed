#!/bin/bash

# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2024-2026 We-Amp B.V.

# Build and run the PageSpeed ASP.NET Core demo.
#
# Usage:
#   ./run-demo.sh              # Full stack (ASP.NET + worker) via Compose
#   ./run-demo.sh --standalone # Single container (no worker, no caching)
#   ./run-demo.sh --build-only # Build images without running
#
# Prerequisites:
#   - Docker (with Compose plugin)
#   - reference/cyclone/ must exist in the repo root

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/../../../.." && pwd)"
COMPOSE_FILE="$SCRIPT_DIR/docker-compose.yml"
IMAGE_NAME="pagespeed-aspnetcore-demo"

# Verify reference/cyclone exists
if [ ! -d "$REPO_ROOT/reference/cyclone/src" ]; then
    echo "ERROR: reference/cyclone/ not found."
    echo "This directory is required for building libpagespeed.so."
    echo "Clone the Cyclone source into reference/cyclone/ first."
    exit 1
fi

if [ "$1" = "--build-only" ]; then
    echo "Building images from $REPO_ROOT ..."
    echo "(This may take several minutes on first build — libaom alone is ~90s)"
    echo ""
    docker compose -f "$COMPOSE_FILE" build
    echo ""
    echo "Images built. Run with: docker compose -f $COMPOSE_FILE up"
    exit 0
fi

if [ "$1" = "--standalone" ]; then
    echo "Building standalone image (no worker) ..."
    echo "(This may take several minutes on first build — libaom alone is ~90s)"
    echo ""
    docker build \
        -f "$SCRIPT_DIR/Dockerfile" \
        --target aspnet-runtime \
        -t "$IMAGE_NAME" \
        "$REPO_ROOT"
    echo ""
    echo "Starting standalone demo on http://localhost:5100"
    echo "  (No worker — HTML processing only, no caching)"
    echo ""
    echo "  Append ?bypass=1 to any page to skip the middleware."
    echo "  Press Ctrl+C to stop."
    echo ""
    docker run --rm -p 5100:8080 "$IMAGE_NAME"
    exit 0
fi

# Default: full stack via Docker Compose
echo "Building full stack from $REPO_ROOT ..."
echo "(This may take several minutes on first build — libaom alone is ~90s)"
echo ""

docker compose -f "$COMPOSE_FILE" up --build -d

echo ""
echo "Full stack running:"
echo ""
echo "  Pages:"
echo "    http://localhost:5200/            Home (hero + features)"
echo "    http://localhost:5200/gallery     Gallery (lazy loading)"
echo "    http://localhost:5200/blog        Blog (preconnect)"
echo "    http://localhost:5200/about       About"
echo "    http://localhost:5200/api/status  API (JSON pass-through)"
echo ""
echo "  Worker API & Console:"
echo "    http://localhost:9980/v1/stats    Cache & worker statistics"
echo "    http://localhost:9980/console/    Web console (workbench)"
echo ""
echo "  Append ?bypass=1 to any page to skip the middleware."
echo ""
echo "  Logs:   docker compose -f $COMPOSE_FILE logs -f"
echo "  Stop:   docker compose -f $COMPOSE_FILE down -v"
