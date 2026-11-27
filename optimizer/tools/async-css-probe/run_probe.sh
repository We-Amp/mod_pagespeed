#!/bin/bash

# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2024-2026 We-Amp B.V.

# mod_pagespeed - Async-CSS probe runner
#
# Runs the markup lane against the full stack in Docker, in each of its three
# modes (see conftest.py): gated, forced, shipped. One pass per mode because
# both knobs are startup flags — the worker cannot change its mind without a
# restart, by design.
#
# Prerequisites:
#   - Docker and Docker Compose
#   - pagespeed2-dev image (run ./tools/docker-build.sh first), or --prebuilt
#     with .ci-artifacts/ already staged
#   - Python 3
#
# Usage:
#   ./tools/async-css-probe/run_probe.sh                 # all three modes
#   ./tools/async-css-probe/run_probe.sh --mode forced   # one mode only
#   ./tools/async-css-probe/run_probe.sh --prebuilt      # use .ci-artifacts/
#   ./tools/async-css-probe/run_probe.sh -k loader       # pass through to pytest

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
COMPOSE_FILE="$SCRIPT_DIR/docker-compose.yml"
PREBUILT_FILE="$SCRIPT_DIR/docker-compose.prebuilt.yml"

# Two probe runs can share a docker host — the CI pool has more than one runner
# on the same machine. Without a unique project name they would share
# containers, a shared volume, and the host ports below, and each run's
# `down -v` would tear down the other's stack mid-assertion. Overridable so CI
# can key it on the run id; the pid keeps concurrent local runs apart.
export COMPOSE_PROJECT_NAME="${COMPOSE_PROJECT_NAME:-async-css-probe-$$}"
export PROBE_NGINX_PORT="${PROBE_NGINX_PORT:-8280}"
export PROBE_ORIGIN_PORT="${PROBE_ORIGIN_PORT:-8281}"

MODES=()
PREBUILT=false
PYTEST_ARGS=()

while [[ $# -gt 0 ]]; do
    case "$1" in
        --mode)     MODES+=("$2"); shift 2 ;;
        --prebuilt) PREBUILT=true; shift ;;
        *) PYTEST_ARGS+=("$1"); shift ;;
    esac
done

if [ ${#MODES[@]} -eq 0 ]; then
    MODES=(gated forced shipped)
fi

# The floor the gated/forced pair raises so the sufficiency gate refuses this
# page. Well above the ~0.38 coverage the fixture actually achieves, so the
# refusal is unambiguous and does not depend on extractor tuning.
REFUSING_COVERAGE_FLOOR=0.95

if ! command -v docker &>/dev/null; then
    echo "Error: docker not found. Install Docker first."
    exit 1
fi

COMPOSE=(docker compose -f "$COMPOSE_FILE")
if $PREBUILT; then
    COMPOSE+=(-f "$PREBUILT_FILE")
    if [ ! -f "$SCRIPT_DIR/../../.ci-artifacts/factory_worker" ]; then
        echo "Error: --prebuilt needs .ci-artifacts/ staged (tools/ci/stage-artifacts.sh)."
        exit 1
    fi
elif ! docker image inspect pagespeed2-dev &>/dev/null; then
    echo "Error: pagespeed2-dev image not found."
    echo "Build it with: ./tools/docker-build.sh   (or run with --prebuilt)"
    exit 1
fi

VENV_DIR="$SCRIPT_DIR/.venv"
if [ ! -d "$VENV_DIR" ]; then
    python3 -m venv "$VENV_DIR"
fi
# shellcheck disable=SC1091
source "$VENV_DIR/bin/activate"
pip install -q -r "$SCRIPT_DIR/requirements.txt"

EXIT_CODE=0

run_pass() {
    local mode="$1"
    local force=0 floor=""
    case "$mode" in
        gated)   floor="$REFUSING_COVERAGE_FLOOR" ;;
        forced)  floor="$REFUSING_COVERAGE_FLOOR"; force=1 ;;
        shipped) ;;
        *) echo "Unknown mode: $mode" >&2; exit 2 ;;
    esac

    echo ""
    echo "=== Async-CSS probe: ${mode} mode (force=${force} floor=${floor:-default}) ==="
    export ASYNC_CSS_PROBE_FORCE="$force"
    export PAGESPEED_ASYNC_CSS_MIN_COVERAGE="$floor"
    export ASYNC_CSS_PROBE_MODE="$mode"

    # A fresh volume per pass: the decision is cached with the page, so a pass
    # that inherited another pass's cache would assert against markup produced
    # under a different worker argv.
    "${COMPOSE[@]}" down --remove-orphans -v 2>/dev/null || true
    "${COMPOSE[@]}" up -d --build
    (
        cd "$SCRIPT_DIR"
        PROBE_NO_LIFECYCLE=1 pytest -v --tb=short "${PYTEST_ARGS[@]+"${PYTEST_ARGS[@]}"}"
    ) || EXIT_CODE=1
    "${COMPOSE[@]}" logs --no-color --tail=200 worker \
        > "$SCRIPT_DIR/.worker-${mode}.log" 2>&1 || true
    "${COMPOSE[@]}" down --remove-orphans -v 2>/dev/null || true
}

for mode in "${MODES[@]}"; do
    run_pass "$mode"
done

exit $EXIT_CODE
