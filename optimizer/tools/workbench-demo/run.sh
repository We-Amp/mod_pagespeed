#!/usr/bin/env bash

# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2024-2026 We-Amp B.V.

# Launch the workbench demo: PageSpeed optimization pipeline + web console.
#
# Prerequisites:
#   - Docker (with compose plugin)
#   - pnpm (for the workbench dev server)
#   - pagespeed2-dev Docker image (./tools/docker-build.sh)
#   - Demo images generated (./tools/measure-demo/generate_images.sh)
#   - The site tree: WEBSITE_DIR (default <repo root>/website)
#
# Usage:
#   ./tools/workbench-demo/run.sh          # Start everything
#   ./tools/workbench-demo/run.sh --down   # Tear down Docker stack
#   WEBSITE_DIR=/path/to/website ./tools/workbench-demo/run.sh

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
COMPOSE_FILE="$SCRIPT_DIR/docker-compose.yml"
WORKBENCH_DIR="$REPO_ROOT/tools/workbench"

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

# Handle --down flag
if [ "${1:-}" = "--down" ]; then
    echo "Tearing down workbench demo stack..."
    docker compose -f "$COMPOSE_FILE" down -v
    exit 0
fi

echo "=== PageSpeed 2.0 Workbench Demo ==="
echo ""

# --- Prerequisites ---
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

if ! command -v pnpm &>/dev/null; then
    echo "Error: pnpm not found (needed for workbench dev server)" >&2
    echo "  Install: npm install -g pnpm" >&2
    exit 1
fi
echo "  pnpm: OK"

# Check demo images
if [ ! -f "$WEBSITE_DIR/public/demos/ecommerce/images/headphones.jpg" ]; then
    echo ""
    echo "Demo images not found. Generating..."
    "$REPO_ROOT/tools/measure-demo/generate_images.sh"
fi

if [ ! -f "$WEBSITE_DIR/public/demos/ecommerce/style.css" ]; then
    echo "Error: Demo sites not restructured (missing style.css)." >&2
    exit 1
fi
echo "  Demo content: OK"

# --- Docker Stack ---
echo ""
echo "Building and starting Docker stack (first build takes several minutes)..."
docker compose -f "$COMPOSE_FILE" build
docker compose -f "$COMPOSE_FILE" up -d

# Wait for services (60s timeout)
echo "Waiting for services..."
for i in $(seq 1 60); do
    origin_ok=false
    nginx_ok=false
    worker_ok=false

    curl -sf http://localhost:8081/demos/ecommerce/index.html >/dev/null 2>&1 && origin_ok=true
    curl -sf http://localhost:8080/demos/ecommerce/index.html >/dev/null 2>&1 && nginx_ok=true
    # /v1/health is the one endpoint the daemon serves without a credential;
    # /v1/stats now answers 401 unless the harness token is presented.
    curl -sf http://localhost:9880/v1/health >/dev/null 2>&1 && worker_ok=true

    if $origin_ok && $nginx_ok && $worker_ok; then
        echo "  All services ready after ${i}s"
        break
    fi

    if [ "$i" -eq 60 ]; then
        echo "Error: Services did not start within 60s" >&2
        echo "  origin: $origin_ok  nginx: $nginx_ok  worker: $worker_ok" >&2
        echo ""
        docker compose -f "$COMPOSE_FILE" logs
        docker compose -f "$COMPOSE_FILE" down -v
        exit 1
    fi
    sleep 1
done

# --- Seed Traffic ---
echo ""
echo "Seeding traffic through nginx to populate cache..."

DEMOS="ecommerce blog news portfolio"
BASE="http://localhost:8080/demos"

# Fetch HTML, CSS, JS for each demo
for demo in $DEMOS; do
    curl -sf "$BASE/$demo/index.html" >/dev/null &
    curl -sf "$BASE/$demo/style.css" >/dev/null &
    curl -sf "$BASE/$demo/app.js" >/dev/null &
done
wait

# Fetch images in parallel
for img in \
    ecommerce/images/headphones.jpg \
    ecommerce/images/jacket.jpg \
    ecommerce/images/plantpot.jpg \
    ecommerce/images/powerbank.jpg \
    ecommerce/images/tshirt.jpg \
    ecommerce/images/wallet.jpg \
    blog/images/ai-assistants.jpg \
    blog/images/container-security.jpg \
    blog/images/edge-computing.jpg \
    blog/images/rust-programming.jpg \
    blog/images/webassembly.jpg \
    news/images/autonomous-vehicles.jpg \
    news/images/cancer-research.jpg \
    news/images/central-banks.jpg \
    news/images/championship.jpg \
    news/images/infrastructure.jpg \
    news/images/ocean-conservation.jpg \
    news/images/open-source-ai.jpg \
    news/images/peace-agreement.jpg \
    news/images/quantum-hero.jpg \
    news/images/space-telescope.jpg \
    portfolio/images/forest-cathedral.jpg \
    portfolio/images/golden-hour.jpg \
    portfolio/images/hero-landscape.jpg \
    portfolio/images/morning-mist.jpg \
    portfolio/images/ocean-whisper.jpg \
    portfolio/images/still-waters.jpg \
; do
    curl -sf "$BASE/$img" >/dev/null &
done
wait

echo "  Done - worker is now processing optimizations in the background."

# --- Workbench ---
echo ""

# Install workbench dependencies if needed
if [ ! -d "$WORKBENCH_DIR/node_modules" ]; then
    echo "Installing workbench dependencies..."
    (cd "$WORKBENCH_DIR" && pnpm install)
fi

# Build api-client (web-shell imports from its dist/)
echo "Building api-client..."
(cd "$WORKBENCH_DIR" && pnpm -C packages/api-client build)

echo "============================================"
echo ""
echo "  Workbench:    http://localhost:5173"
echo "  Optimized:    http://localhost:8080/demos/ecommerce/index.html"
echo "  Unoptimized:  http://localhost:8081/demos/ecommerce/index.html"
echo "  Worker API:   http://localhost:9880/v1/stats"
echo "                (send: Authorization: Bearer workbench-demo-token)"
echo ""
echo "  Press Ctrl+C to stop the workbench dev server."
echo "  Then run: ./tools/workbench-demo/run.sh --down"
echo ""
echo "============================================"
echo ""

# Run workbench dev server in foreground
cd "$WORKBENCH_DIR"
exec pnpm --filter @pagespeed/web-shell dev
