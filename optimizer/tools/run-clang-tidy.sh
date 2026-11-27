#!/bin/bash
# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2024-2026 We-Amp B.V.
#
# Run clang-tidy via Docker to ensure correct clang-20 toolchain version.
# Usage: tools/run-clang-tidy.sh [--fix]
#
# Options:
#   --fix    Apply auto-fixes in-place
#
# The script builds the lint Docker image if needed (based on pagespeed2-dev:latest),
# generates compile_commands.json with hedron, then runs clang-tidy inside the
# container.

# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2024-2026 We-Amp B.V.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(dirname "$SCRIPT_DIR")"
IMAGE="pagespeed2-lint:latest"
FIX_FLAG=""

for arg in "$@"; do
  case "$arg" in
    --fix) FIX_FLAG="-fix" ;;
    *) echo "Unknown argument: $arg" >&2; exit 1 ;;
  esac
done

# Build the lint image if needed
if ! docker image inspect "$IMAGE" >/dev/null 2>&1; then
  echo "Building lint Docker image..."
  docker build -t "$IMAGE" -f "$REPO_ROOT/docker/Dockerfile.lint" "$REPO_ROOT/docker"
fi

echo "Running clang-tidy (fix=$([[ -n "$FIX_FLAG" ]] && echo yes || echo no))..."

# Map the bazel remote-cache alias only when BAZEL_CACHE_HOST is exported
# (see .bazelrc); no environment-specific address is stored in this tree.
CACHE_DOCKER_ARGS=()
# shellcheck source=ci/remote-cache-docker-args.sh
source "$SCRIPT_DIR/ci/remote-cache-docker-args.sh"
remote_cache_docker_args CACHE_DOCKER_ARGS
docker run --rm \
  -v "$REPO_ROOT:/workspace" \
  -w /workspace \
  ${CACHE_DOCKER_ARGS[@]+"${CACHE_DOCKER_ARGS[@]}"} \
  "$IMAGE" \
  bash -c '
    set -e

    # Generate compile_commands.json (requires network for first run).
    # //:refresh_all (root BUILD) wraps the hedron default with explicit
    # entries for the `manual`-tagged fuzz harnesses, which `//...` excludes.
    echo "Generating compile_commands.json..."
    bazel run //:refresh_all \
      --config=docker > /tmp/compdb.log 2>&1 || { cat /tmp/compdb.log; exit 1; }

    # Run clang-tidy (exclude external/ and generated code)
    echo "Running clang-tidy..."
    run-clang-tidy-20 '"$FIX_FLAG"' \
      -clang-tidy-binary=clang-tidy-20 \
      -p /workspace \
      -header-filter="^/workspace/(lib|src|test)/.*\.(h|hpp)$" \
      "/workspace/(lib|src|test)/.*\.cc"
  '
