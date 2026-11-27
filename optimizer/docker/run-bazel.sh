#!/bin/bash

# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2024-2026 We-Amp B.V.

# Run a command inside the pagespeed2-dev Docker container with Bazel cache
# and source code mounted. Handles platform detection, SSH agent forwarding,
# and image auto-build.
#
# Usage: docker/run-bazel.sh [OPTIONS] -- <command...>
#   Options:
#     --rw           Mount workspace read-write (default: read-only)
#     --no-ssh       Skip SSH agent forwarding
#     --image NAME   Override image name (default: pagespeed2-dev)
#
# Examples:
#   docker/run-bazel.sh -- bazel test //...
#   docker/run-bazel.sh --rw -- bash -c "bazel coverage //... && genhtml ..."

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(dirname "$SCRIPT_DIR")"

IMAGE_NAME="pagespeed2-dev"
MOUNT_MODE=":ro"
FORWARD_SSH=true

# Parse options.
while [[ $# -gt 0 ]]; do
    case "$1" in
        --rw) MOUNT_MODE=""; shift ;;
        --no-ssh) FORWARD_SSH=false; shift ;;
        --image) IMAGE_NAME="$2"; shift 2 ;;
        --) shift; break ;;
        *) break ;;
    esac
done

if [[ $# -eq 0 ]]; then
    echo "Usage: docker/run-bazel.sh [OPTIONS] -- <command...>" >&2
    exit 1
fi

# Check if image exists, build if not.
if ! docker image inspect "$IMAGE_NAME" &>/dev/null; then
    echo "Docker image $IMAGE_NAME not found. Building..."
    "$PROJECT_ROOT/tools/docker-build.sh" 2>&1 || {
        echo "ERROR: Failed to build $IMAGE_NAME" >&2
        exit 1
    }
fi

# Platform matching host architecture.
PLATFORM="linux/$(uname -m | sed 's/x86_64/amd64/;s/aarch64/arm64/')"

# Forward SSH agent if available (needed when a dependency remote requires
# authentication; harmless when every fetch is anonymous).
SSH_ARGS=()
if [[ "$FORWARD_SSH" = true ]] && [[ -n "${SSH_AUTH_SOCK:-}" ]]; then
    SSH_ARGS+=(
        -v "$SSH_AUTH_SOCK:/ssh-agent"
        -e "SSH_AUTH_SOCK=/ssh-agent"
    )
fi

exec docker run --rm \
    --platform "$PLATFORM" \
    -v "$PROJECT_ROOT:/workspace${MOUNT_MODE}" \
    -v "pagespeed2-bazel-cache:/root/.cache/bazel" \
    "${SSH_ARGS[@]}" \
    -w /workspace \
    "$IMAGE_NAME" \
    "$@"
