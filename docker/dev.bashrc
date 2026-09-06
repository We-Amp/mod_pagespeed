# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2024-2026 We-Amp B.V.

# Development environment shell configuration for mod_pagespeed
# Sourced automatically when entering the container

# Color prompt with container indicator
export PS1='\[\033[01;32m\][pagespeed-dev]\[\033[00m\] \[\033[01;34m\]\w\[\033[00m\]\$ '

# Test environment for cache backends
export REDIS_HOST="${REDIS_HOST:-redis}"
export MEMCACHED_HOST="${MEMCACHED_HOST:-memcached}"
export REDIS_PORT="${REDIS_PORT:-6379}"
export MEMCACHED_PORT="${MEMCACHED_PORT:-11211}"

# Bazel cache configuration
export BAZEL_CACHE_DIR="${BAZEL_CACHE_DIR:-/bazel-cache}"

# Convenience aliases for common operations
alias build='bazel build //...'
alias test='bazel test //test/...'
alias test-debug='bazel test --test_env=REDIS_PORT=${REDIS_PORT} --test_env=MEMCACHED_PORT=${MEMCACHED_PORT} -c dbg --test_output=streamed //test/...'

# Build specific target
alias build-kernel='bazel build //pagespeed/kernel/...'
alias build-apache='bazel build //pagespeed/apache/...'
alias build-envoy='bazel build //pagespeed/envoy/...'

# Test specific areas
alias test-kernel='bazel test //test/pagespeed/kernel/...'
alias test-apache='bazel test //test/pagespeed/apache/...'
alias test-envoy='bazel test //test/pagespeed/envoy/...'

# Quick single test run with verbose output
run-test() {
    if [ -z "$1" ]; then
        echo "Usage: run-test //test/path/to:test_name"
        return 1
    fi
    bazel test --test_output=streamed "$1"
}

# Build with clang config (default in container)
alias build-clang='bazel build --config=clang //...'

# Build with sanitizers
alias build-asan='bazel build --config=clang-asan //...'
alias build-tsan='bazel build --config=clang-tsan //...'

# Check if cache services are available
check-services() {
    echo "Checking Redis..."
    redis-cli -h "${REDIS_HOST}" -p "${REDIS_PORT}" ping 2>/dev/null && echo "Redis: OK" || echo "Redis: FAILED"

    echo "Checking Memcached..."
    echo "stats" | nc -q1 "${MEMCACHED_HOST}" "${MEMCACHED_PORT}" 2>/dev/null | head -1 && echo "Memcached: OK" || echo "Memcached: FAILED"
}

# Clean bazel cache
alias clean='bazel clean'
alias clean-all='bazel clean --expunge'

# Print help on container start
echo ""
echo "=== mod_pagespeed Development Environment ==="
echo ""
echo "Common commands:"
echo "  build          - Build all targets"
echo "  test           - Run all tests"
echo "  test-debug     - Run tests with debug output"
echo "  check-services - Verify Redis/Memcached connectivity"
echo ""
echo "Quick builds:"
echo "  build-kernel, build-apache, build-envoy"
echo ""
echo "Quick tests:"
echo "  test-kernel, test-apache, test-envoy"
echo "  run-test //test/path/to:test_name"
echo ""
