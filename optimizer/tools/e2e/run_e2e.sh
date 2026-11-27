#!/bin/bash

# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2024-2026 We-Amp B.V.

# mod_pagespeed - End-to-End Test Runner
#
# Runs pytest-based E2E user story tests against the full stack
# (nginx + worker + origin) in Docker.
#
# Prerequisites:
#   - Docker and Docker Compose
#   - pagespeed2-dev image (run ./tools/docker-build.sh first)
#   - Python 3 with pip
#
# Usage:
#   ./tools/e2e/run_e2e.sh              # Run all tests
#   ./tools/e2e/run_e2e.sh -k css       # Run only CSS tests
#   ./tools/e2e/run_e2e.sh -v --tb=long # Verbose with full tracebacks

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

# Generate large test fixtures if missing
"$SCRIPT_DIR/../generate-test-fixtures.sh"

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

# Set up virtual environment for Python dependencies
VENV_DIR="$SCRIPT_DIR/.venv"
if [ ! -d "$VENV_DIR" ]; then
    python3 -m venv "$VENV_DIR"
fi
# shellcheck disable=SC1091
source "$VENV_DIR/bin/activate"
pip install -q -r "$SCRIPT_DIR/requirements.txt"

# Run tests from the e2e directory (so docker-compose.yml is found)
cd "$SCRIPT_DIR"
exec pytest test_user_stories.py test_configuration.py -v --tb=short "$@"
