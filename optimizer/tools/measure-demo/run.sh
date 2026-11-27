#!/usr/bin/env bash

# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2024-2026 We-Amp B.V.

# Run the demo site measurement pipeline.
#
# Prerequisites:
#   - Docker with pagespeed2-dev image built (./tools/docker-build.sh)
#   - Node.js (for Lighthouse, optional with --skip-lighthouse)
#   - The site tree: WEBSITE_DIR (default <repo root>/website)
#
# Usage:
#   ./tools/measure-demo/run.sh                  # Full measurement
#   ./tools/measure-demo/run.sh --skip-lighthouse # Skip Lighthouse (faster)
#   WEBSITE_DIR=/path/to/website ./tools/measure-demo/run.sh

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
COMPOSE_FILE="$SCRIPT_DIR/docker-compose.yml"

# WEBSITE_DIR names the modpagespeed.com site tree, which lives in the public
# repository (We-Amp/pagespeed-optimizer, website/). Default: <repo root>/website. Exported so the
# tools this script runs resolve the same tree.
WEBSITE_DIR="${WEBSITE_DIR:-$REPO_ROOT/website}"
if [ ! -d "$WEBSITE_DIR" ]; then
    echo "Error: website tree not found at $WEBSITE_DIR" >&2
    echo "Set WEBSITE_DIR to the public repository's website/ checkout (We-Amp/pagespeed-optimizer)." >&2
    exit 1
fi
export WEBSITE_DIR

echo "=== PageSpeed 2.0 Demo Measurement ==="
echo ""

# Check prerequisites
echo "Checking prerequisites..."

if ! command -v docker &>/dev/null; then
    echo "Error: docker not found" >&2
    exit 1
fi
echo "  Docker: OK"

if ! docker compose version &>/dev/null; then
    echo "Error: docker compose not found" >&2
    exit 1
fi
echo "  Docker Compose: OK"

# Check if demo images exist
if [ ! -f "$WEBSITE_DIR/public/demos/ecommerce/images/headphones.jpg" ]; then
    echo ""
    echo "Demo images not found. Generating..."
    "$SCRIPT_DIR/generate_images.sh"
fi

# Check if demo sites have been restructured (external CSS/JS)
if [ ! -f "$WEBSITE_DIR/public/demos/ecommerce/style.css" ]; then
    echo "Error: Demo sites not restructured (missing style.css)." >&2
    echo "Ensure demos have external CSS/JS files." >&2
    exit 1
fi
echo "  Demo content: OK"

# Set up Python venv
VENV_DIR="$SCRIPT_DIR/.venv"
if [ ! -d "$VENV_DIR" ]; then
    echo ""
    echo "Creating Python virtual environment..."
    python3 -m venv "$VENV_DIR"
fi
# Ensure deps (idempotent - pip skips already-installed).
"$VENV_DIR/bin/pip" install --quiet requests
echo "  Python venv: OK"

# Build and start measurement stack
echo ""
echo "Starting measurement stack..."
docker compose -f "$COMPOSE_FILE" build --quiet
docker compose -f "$COMPOSE_FILE" up -d

# Wait for services
echo "Waiting for services to be ready..."
for i in $(seq 1 30); do
    if curl -sf http://localhost:8081/demos/ecommerce/index.html >/dev/null 2>&1 && \
       curl -sf http://localhost:8080/demos/ecommerce/index.html >/dev/null 2>&1; then
        echo "  Services ready after ${i}s"
        break
    fi
    if [ "$i" -eq 30 ]; then
        echo "Error: Services did not start within 30s" >&2
        docker compose -f "$COMPOSE_FILE" logs
        docker compose -f "$COMPOSE_FILE" down -v
        exit 1
    fi
    sleep 1
done

# Give worker a moment to start listening on socket
sleep 2

# Run measurement
echo ""
echo "Running measurements..."
"$VENV_DIR/bin/python3" "$SCRIPT_DIR/measure.py" "$@"
EXIT_CODE=$?

# Tear down
echo ""
echo "Tearing down measurement stack..."
docker compose -f "$COMPOSE_FILE" down -v

exit $EXIT_CODE
