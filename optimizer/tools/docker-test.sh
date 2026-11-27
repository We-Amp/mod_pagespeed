#!/bin/bash

# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2024-2026 We-Amp B.V.

# Run bazel commands inside Docker container.
# Usage: ./tools/docker-test.sh <subcommand> [bazel args...]
# Examples:
#   ./tools/docker-test.sh test //...
#   ./tools/docker-test.sh test --config=asan-leaks //...
#   ./tools/docker-test.sh build //lib/base:base

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(dirname "$SCRIPT_DIR")"

SUBCMD="$1"
shift

exec "$PROJECT_ROOT/docker/run-bazel.sh" -- \
    bazel "$SUBCMD" --config=docker --symlink_prefix=/dev/null/ "$@"
