#!/bin/bash

# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2024-2026 We-Amp B.V.

# Build the Docker image for sanitizer testing.

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(dirname "$SCRIPT_DIR")"

IMAGE_NAME="pagespeed2-dev"
BASE_RUNTIME="pagespeed2-base-runtime"

echo "Building Docker image: $BASE_RUNTIME"
docker build -t "$BASE_RUNTIME" -f "$PROJECT_ROOT/docker/Dockerfile.base-runtime" "$PROJECT_ROOT"

echo "Building Docker image: $IMAGE_NAME"
docker build -t "$IMAGE_NAME" "$PROJECT_ROOT/docker"

echo ""
echo "Docker images built successfully: $BASE_RUNTIME, $IMAGE_NAME"
echo "Run tests with: ./tools/docker-test.sh //..."
