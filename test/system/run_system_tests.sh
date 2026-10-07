#!/bin/bash
# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2024-2026 We-Amp B.V.

#
# Builds mod_pagespeed, sets up Apache, and runs Python system tests.
#
# Usage (inside Docker container):
#   ./test/system/run_system_tests.sh [options] [pytest-args...]
#
# Options:
#   --build-only    Only build the module, don't run tests
#   --skip-build    Skip building, assume module is already built
#   --keep-running  Keep Apache running after tests
#   --help          Show this help message
#
# Examples:
#   ./test/system/run_system_tests.sh                    # Run all tests
#   ./test/system/run_system_tests.sh -k sanity          # Run only sanity tests
#   ./test/system/run_system_tests.sh automatic/         # Run tests in automatic/
#   ./test/system/run_system_tests.sh --build-only       # Just build the module

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

Builds mod_pagespeed, sets up Apache, and runs Python system tests.

Options:
  --build-only    Only build the module, don't run tests
  --skip-build    Skip building, assume module is already built
  --keep-running  Keep Apache running after tests (for debugging)
  --gcc           Use GCC 13 for building (recommended for Cyclone cache)
  --help          Show this help message

Pytest arguments:
  Any additional arguments are passed to pytest.

Examples:
  $0                                    # Run all tests
  $0 -k sanity                          # Run only sanity tests
  $0 automatic/test_extend_cache.py     # Run specific test file
  $0 -v --tb=long                       # Verbose output with long tracebacks
  $0 --build-only                       # Just build the module
  $0 --skip-build -k "not slow"         # Skip build, run non-slow tests

Environment Variables:
  PAGESPEED_HOST    Server hostname (default: localhost)
  PAGESPEED_PORT    Server port (default: 80)
  BAZEL_CONFIG      Additional bazel config (default: --config=gcc)
EOF
}

# Parse arguments
BUILD_ONLY=false
SKIP_BUILD=false
KEEP_RUNNING=false
BAZEL_CONFIG="${BAZEL_CONFIG:---config=gcc}"
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
        --gcc)
            BAZEL_CONFIG="--config=gcc"
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

# Build the module
build_module() {
    log_step "Building mod_pagespeed module..."
    cd "$PROJECT_ROOT"

    local build_opts="$BAZEL_CONFIG"

    if ! bazel build $build_opts //:libmod_pagespeed.so; then
        log_error "Failed to build mod_pagespeed module"
        exit 1
    fi

    log_info "Module built successfully: bazel-bin/libmod_pagespeed.so"
}

# Setup and start Apache
setup_apache() {
    log_step "Setting up Apache with mod_pagespeed..."
    cd "$PROJECT_ROOT"

    if ! "$SCRIPT_DIR/setup_apache_test.sh" start; then
        log_error "Failed to setup Apache"
        exit 1
    fi

    log_info "Apache is running with mod_pagespeed"
}

# Stop Apache
stop_apache() {
    log_step "Stopping Apache..."
    "$SCRIPT_DIR/setup_apache_test.sh" stop 2>/dev/null || true
}

# Run Python tests
run_tests() {
    log_step "Running Python system tests..."
    cd "$SCRIPT_DIR"

    # Set default environment variables
    export PAGESPEED_HOST="${PAGESPEED_HOST:-localhost}"
    export PAGESPEED_PORT="${PAGESPEED_PORT:-80}"

    # HTTPS configuration for TLS tests
    export PAGESPEED_HTTPS_HOST="${PAGESPEED_HTTPS_HOST:-localhost}"
    export PAGESPEED_HTTPS_PORT="${PAGESPEED_HTTPS_PORT:-8443}"

    # Secondary server for IPRO caching tests
    export PAGESPEED_SECONDARY_HOST="${PAGESPEED_SECONDARY_HOST:-localhost}"
    export PAGESPEED_SECONDARY_PORT="${PAGESPEED_SECONDARY_PORT:-8081}"

    # The remote-configuration server (setup_apache_test.sh RCPORT) for
    # system/test_remote_config.py, which arms its failure case directly.
    # Falls back to RCPORT itself so an RCPORT override reaches the test too.
    export PAGESPEED_REMOTE_CONFIG_PORT="${PAGESPEED_REMOTE_CONFIG_PORT:-${RCPORT:-8090}}"

    # Apache control + error-log paths for the graceful-restart thread-leak
    # regression test (test_graceful_restart_no_thread_leak). The test issues
    # `sudo $PAGESPEED_APACHE_CTL graceful` and greps the error log for AH03490.
    export PAGESPEED_SERVER_TYPE="${PAGESPEED_SERVER_TYPE:-apache}"
    export PAGESPEED_APACHE_CTL="${PAGESPEED_APACHE_CTL:-apache2ctl}"
    export PAGESPEED_APACHE_ERROR_LOG="${PAGESPEED_APACHE_ERROR_LOG:-/var/log/apache2/error.log}"

    # Lane fixtures (test/system/conftest.py KNOWN_LANE_FIXTURES): the names
    # setup_apache_test.sh provisioned, unless the caller pins a list.
    if [ -z "${PAGESPEED_LANE_FIXTURES:-}" ] && [ -f /tmp/apache_pagespeed_test/lane_fixtures ]; then
        PAGESPEED_LANE_FIXTURES="$(sort -u /tmp/apache_pagespeed_test/lane_fixtures | paste -sd, -)"
    fi
    export PAGESPEED_LANE_FIXTURES="${PAGESPEED_LANE_FIXTURES:-}"
    export PAGESPEED_CACHE_DIR="${PAGESPEED_CACHE_DIR:-/var/cache/mod_pagespeed}"
    export PAGESPEED_SECONDARY_CACHE_DIR="${PAGESPEED_SECONDARY_CACHE_DIR:-/var/cache/mod_pagespeed_secondary}"
    export PAGESPEED_DOC_ROOT="${PAGESPEED_DOC_ROOT:-/var/www/html}"
    export PAGESPEED_STATS_LOG="${PAGESPEED_STATS_LOG:-/var/log/pagespeed/stats_log_global}"
    log_info "Lane fixtures: ${PAGESPEED_LANE_FIXTURES:-<none>}"

    log_info "Test server: http://$PAGESPEED_HOST:$PAGESPEED_PORT"
    log_info "HTTPS server: https://$PAGESPEED_HTTPS_HOST:$PAGESPEED_HTTPS_PORT"
    log_info "Secondary server: http://$PAGESPEED_SECONDARY_HOST:$PAGESPEED_SECONDARY_PORT"

    # Default to running all automatic tests if no specific tests specified
    if [ ${#PYTEST_ARGS[@]} -eq 0 ]; then
        PYTEST_ARGS=("automatic/" "system/" "-v")
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
        stop_apache
    else
        log_info "Apache left running (--keep-running specified)"
        log_info "Stop it manually with: ./test/system/setup_apache_test.sh stop"
    fi
}

# Main
main() {
    cd "$PROJECT_ROOT"

    # Build module
    if [ "$SKIP_BUILD" = false ]; then
        build_module
    else
        log_info "Skipping build (--skip-build specified)"
    fi

    # Exit early if build-only
    if [ "$BUILD_ONLY" = true ]; then
        log_info "Build complete (--build-only specified)"
        exit 0
    fi

    # Statistics-log timestamps must be from this run (statistics_logging.sh
    # START_TIME); setup_apache_test.sh empties the log before Apache starts.
    export PAGESPEED_LANE_START_MS="${PAGESPEED_LANE_START_MS:-$(date +%s)000}"

    # Setup cleanup trap
    trap cleanup EXIT

    # Setup Apache
    setup_apache

    # Run tests
    run_tests

    log_info "System tests completed!"
}

main
