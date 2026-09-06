#!/bin/bash
# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2024-2026 We-Amp B.V.

#
# Builds Envoy with PageSpeed, sets up the environment, and runs Python system tests.
#
# Usage (inside Docker container):
#   ./test/system/run_envoy_tests.sh [options] [pytest-args...]
#
# Options:
#   --build-only    Only build Envoy, don't run tests
#   --skip-build    Skip building, assume Envoy is already built
#   --keep-running  Keep Envoy running after tests
#   --help          Show this help message
#
# Examples:
#   ./test/system/run_envoy_tests.sh                    # Run all tests (automatic/ + envoy/)
#   ./test/system/run_envoy_tests.sh -k sanity          # Run only sanity tests
#   ./test/system/run_envoy_tests.sh automatic/         # Run only automatic/ tests
#   ./test/system/run_envoy_tests.sh envoy/             # Run only envoy/ tests
#   ./test/system/run_envoy_tests.sh --build-only       # Just build Envoy
#
# Test Directories:
#   automatic/ - Golden standard tests shared with Apache (core PageSpeed functionality)
#   envoy/     - Envoy-specific tests for filter-unique features

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"

# Colors for output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m' # No Color

log_info() {
    echo -e "${GREEN}[INFO]${NC} $1"
}

log_warn() {
    echo -e "${YELLOW}[WARN]${NC} $1"
}

log_error() {
    echo -e "${RED}[ERROR]${NC} $1"
}

log_step() {
    echo -e "${BLUE}[STEP]${NC} $1"
}

show_help() {
    cat << EOF
Usage: $0 [options] [pytest-args...]

Builds Envoy with PageSpeed filter, sets up the environment, and runs Python system tests.

Options:
  --build-only    Only build Envoy, don't run tests
  --skip-build    Skip building, assume Envoy is already built
  --keep-running  Keep Envoy running after tests (for debugging)
  --jobs=N        Number of parallel build jobs (default: auto)
  --help          Show this help message

Pytest arguments:
  Any additional arguments are passed to pytest.

Examples:
  $0                                    # Run all tests (automatic/ + envoy/)
  $0 -k sanity                          # Run only sanity tests
  $0 automatic/                         # Run only automatic/ tests
  $0 envoy/                             # Run only envoy/ tests
  $0 automatic/test_extend_cache.py     # Run specific test file
  $0 -v --tb=long                       # Verbose output with long tracebacks
  $0 --build-only                       # Just build Envoy
  $0 --skip-build -k "not slow"         # Skip build, run non-slow tests

Test Directories:
  automatic/  Golden standard tests shared with Apache. These test core PageSpeed
              functionality (cache extension, image optimization, CSS/JS minification)
              and should pass on all server types.
  envoy/      Envoy-specific tests for features unique to the Envoy filter.

Environment Variables:
  PAGESPEED_HOST          Server hostname (default: localhost)
  PAGESPEED_PORT          Server port (default: 8080)
  BAZEL_CONFIG            Bazel config (default: --config=clang-libstdcxx13)
  BAZEL_JOBS              Parallel build jobs (default: auto)

Note: Building Envoy requires significant memory (16GB+ recommended).
      Use --jobs=1 if you experience out-of-memory issues.
EOF
}

# Parse arguments
BUILD_ONLY=false
SKIP_BUILD=false
KEEP_RUNNING=false
BAZEL_CONFIG="${BAZEL_CONFIG:---config=clang-libstdcxx13}"
BAZEL_JOBS="${BAZEL_JOBS:-}"
PYTEST_ARGS=()

while [[ $# -gt 0 ]]; do
    case $1 in
        --build-only)
            BUILD_ONLY=true
            shift
            ;;
        --skip-build)
            SKIP_BUILD=true
            shift
            ;;
        --keep-running)
            KEEP_RUNNING=true
            shift
            ;;
        --jobs=*)
            BAZEL_JOBS="${1#*=}"
            shift
            ;;
        --help|-h)
            show_help
            exit 0
            ;;
        *)
            PYTEST_ARGS+=("$1")
            shift
            ;;
    esac
done

# Build Envoy with PageSpeed filter
build_envoy() {
    log_step "Building Envoy with PageSpeed filter..."
    log_info "This may take a while and requires significant memory."
    cd "$PROJECT_ROOT"

    # Build jobs argument (empty = let bazel auto-detect)
    JOBS_ARG=""
    if [ -n "$BAZEL_JOBS" ]; then
        JOBS_ARG="--jobs=$BAZEL_JOBS"
        log_info "Using --jobs=$BAZEL_JOBS (adjust with --jobs=N if needed)"
    else
        log_info "Using bazel default parallelism (override with --jobs=N)"
    fi

    if ! bazel build $BAZEL_CONFIG $JOBS_ARG //pagespeed/envoy:envoy_pagespeed; then
        log_error "Failed to build Envoy with PageSpeed filter"
        log_info ""
        log_info "If you ran out of memory, try:"
        log_info "  $0 --jobs=1 ..."
        exit 1
    fi

    log_info "Envoy built successfully: bazel-bin/pagespeed/envoy/envoy_pagespeed"
}

# Setup and start Envoy
setup_envoy() {
    log_step "Setting up Envoy with PageSpeed..."
    cd "$PROJECT_ROOT"

    # Source the setup script to get access to its functions.
    # This keeps Envoy running in the same process group, preventing Docker
    # from killing it when subprocesses exit.
    source "$SCRIPT_DIR/setup_envoy_test.sh"

    # Run the setup steps
    if ! check_binary; then
        log_error "Envoy binary not found"
        exit 1
    fi
    setup_directories
    setup_test_content
    if ! generate_tls_certs; then
        log_error "Failed to generate TLS certificates"
        exit 1
    fi
    generate_envoy_config
    if ! start_envoy; then
        log_error "Failed to start Envoy"
        exit 1
    fi

    log_info "Envoy is running with PageSpeed filter"
}

# Stop Envoy
# Note: This uses the stop_envoy_process and stop_static_server functions
# from the sourced setup_envoy_test.sh script
_stop_envoy() {
    log_step "Stopping Envoy..."
    stop_envoy_process 2>/dev/null || true
    stop_static_server 2>/dev/null || true
    log_info "Envoy stopped"
}

# Run Python tests
run_tests() {
    log_step "Running Python system tests against Envoy..."
    cd "$SCRIPT_DIR"

    # Set environment variables for Envoy testing
    # Use ENVOY_PORT if set, otherwise PAGESPEED_PORT, otherwise default to 8080
    export PAGESPEED_HOST="${PAGESPEED_HOST:-localhost}"
    export PAGESPEED_PORT="${ENVOY_PORT:-${PAGESPEED_PORT:-8080}}"
    export PAGESPEED_SERVER_TYPE="envoy"
    # Envoy uses /pagespeed_statistics (without "mod_" prefix used by Apache)
    export PAGESPEED_STATS_PATH="${PAGESPEED_STATS_PATH:-/pagespeed_statistics}"

    # HTTPS configuration for TLS tests
    export PAGESPEED_HTTPS_HOST="${PAGESPEED_HTTPS_HOST:-localhost}"
    export PAGESPEED_HTTPS_PORT="${ENVOY_HTTPS_PORT:-${PAGESPEED_HTTPS_PORT:-8443}}"

    log_info "Test server: http://$PAGESPEED_HOST:$PAGESPEED_PORT (Envoy)"
    log_info "HTTPS server: https://$PAGESPEED_HTTPS_HOST:$PAGESPEED_HTTPS_PORT"

    # Default to running both automatic/ and envoy/ test directories:
    # - automatic/ contains the golden standard tests shared with Apache. These test
    #   core PageSpeed functionality (cache extension, image optimization, CSS/JS
    #   minification, etc.) and should pass on all server types.
    # - envoy/ contains Envoy-specific tests for features unique to the Envoy filter.
    if [ ${#PYTEST_ARGS[@]} -eq 0 ]; then
        PYTEST_ARGS=("automatic/" "system/" "envoy/" "-v")
    fi

    # Install pytest if needed
    if ! python3 -c "import pytest" 2>/dev/null; then
        log_info "Installing pytest..."
        pip3 install --user pytest requests
    fi

    # Run pytest
    log_info "Running: python3 -m pytest ${PYTEST_ARGS[*]}"
    python3 -m pytest "${PYTEST_ARGS[@]}"
}

# Cleanup on exit
cleanup() {
    if [ "$KEEP_RUNNING" = false ]; then
        _stop_envoy
    else
        log_info "Envoy left running (--keep-running specified)"
        log_info "Stop it manually with: ./test/system/setup_envoy_test.sh stop"
    fi
}

# Main
main() {
    cd "$PROJECT_ROOT"

    # Build Envoy
    if [ "$SKIP_BUILD" = false ]; then
        build_envoy
    else
        log_info "Skipping build (--skip-build specified)"
    fi

    # Exit early if build-only
    if [ "$BUILD_ONLY" = true ]; then
        log_info "Build complete (--build-only specified)"
        exit 0
    fi

    # Setup cleanup trap
    trap cleanup EXIT

    # Setup Envoy
    setup_envoy

    # Run tests
    run_tests

    log_info "Envoy system tests completed!"
}

main
