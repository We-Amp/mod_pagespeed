#!/bin/bash

# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2024-2026 We-Amp B.V.

# mod_pagespeed - Headless Browser E2E Test Runner (Playwright)
#
# Runs Playwright-based browser tests against the full stack
# (nginx + worker + origin) in Docker, using Chromium, Firefox, and WebKit.
#
# Prerequisites:
#   - Docker and Docker Compose
#   - pagespeed2-dev image (run ./tools/docker-build.sh first)
#   - Python 3 with pip
#
# Usage:
#   ./tools/e2e/run_browser_tests.sh                     # Run all browser tests
#   ./tools/e2e/run_browser_tests.sh -k "Negotiation"    # Run content negotiation tests
#   ./tools/e2e/run_browser_tests.sh -v --tb=long        # Verbose with full tracebacks

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

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

# Set up virtual environment (shared with run_e2e.sh)
VENV_DIR="$SCRIPT_DIR/.venv"
if [ ! -d "$VENV_DIR" ]; then
    python3 -m venv "$VENV_DIR"
fi
# shellcheck disable=SC1091
source "$VENV_DIR/bin/activate"
pip install -q -r "$SCRIPT_DIR/requirements.txt"

# Install Playwright browsers
echo "Installing Playwright browsers..."
playwright install --with-deps chromium firefox webkit

# Run browser tests from the e2e directory
cd "$SCRIPT_DIR"
exec pytest test_browser.py -v --tb=short "$@"
