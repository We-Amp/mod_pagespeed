#!/bin/bash

# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2024-2026 We-Amp B.V.

# Build mod_pagespeed production Docker images.
#
# Usage:
#   docker/build-release.sh [VERSION] [--push-ar]
#
# Examples:
#   docker/build-release.sh                    # tags as :latest
#   docker/build-release.sh 2.0.0              # tags as :2.0.0
#   docker/build-release.sh 2.0.0 --push-ar    # build + push to Artifact Registry
#   docker/build-release.sh --no-cache         # force full rebuild (no Docker layer cache)
#   docker/build-release.sh --no-smoke         # skip the post-build cache-sharing gate
#
# Environment:
#   BUILD_PLATFORM  Target platform (default: linux/amd64)
#   AR_REPO         Artifact Registry repo to push to (required with --push-ar)

set -euo pipefail

VERSION="${1:-latest}"
PUSH_AR=false
NO_CACHE=""
SMOKE=true
for arg in "$@"; do
    [ "$arg" = "--push-ar" ] && PUSH_AR=true
    [ "$arg" = "--no-smoke" ] && SMOKE=false
    [ "$arg" = "--no-cache" ] && NO_CACHE="--no-cache"
done
# Don't treat flags as version
[[ "$VERSION" == --* ]] && VERSION="latest"
# Stamped into the OCI labels (org.opencontainers.image.version / .created);
# without these every release image reported version "dev".
BUILD_DATE="$(date -u +%Y-%m-%dT%H:%M:%SZ)"

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(dirname "$SCRIPT_DIR")"
BUILD_PLATFORM="${BUILD_PLATFORM:-linux/amd64}"
AR_REPO="${AR_REPO:-}"
if [ "$PUSH_AR" = true ] && [ -z "$AR_REPO" ]; then
    echo "error: --push-ar requires AR_REPO (the Artifact Registry repo to push to)" >&2
    exit 2
fi
# Bust the apt dist-upgrade layer on every build so a warm Docker layer cache
# can't re-freeze OS-package CVEs. Today's UTC date is a
# stable-within-a-day cache key; passing it fresh each run pulls current security
# updates even when the base digest and the rest of the Dockerfile are unchanged.
APT_REFRESH="$(date -u +%F)"

echo "Building mod_pagespeed release images (version: $VERSION)"
echo "Project directory: $PROJECT_DIR"
echo "Platform: $BUILD_PLATFORM"
if [ "$PUSH_AR" = true ]; then
    echo "Artifact Registry: $AR_REPO"
fi
echo

if [ "$(uname -m)" = "arm64" ] && [ "$BUILD_PLATFORM" = "linux/amd64" ]; then
    echo "Note: Cross-compiling for amd64 on arm64 (Bazel stage will be slow)"
    echo
fi

# Stamp GIT_COMMIT before any image build reads it.
# The checked-in GIT_COMMIT contains literal "dev"; the worker embeds it
# via //src/product_version:build_commit and exposes
# the first 7 chars at /v1/health. Without this step, release images carry
# git_commit="dev" in production (issue #256). Restore on exit so a dev's
# working tree isn't left dirty.
if ! git -C "$PROJECT_DIR" rev-parse HEAD >/dev/null 2>&1; then
    echo "ERROR: $PROJECT_DIR is not a git working tree; cannot stamp GIT_COMMIT." >&2
    exit 1
fi
GIT_COMMIT_FILE="$PROJECT_DIR/GIT_COMMIT"
GIT_SHA=$(git -C "$PROJECT_DIR" rev-parse HEAD)
_orig_git_commit=""
if [ -f "$GIT_COMMIT_FILE" ]; then
    _orig_git_commit=$(cat "$GIT_COMMIT_FILE")
fi
restore_git_commit() {
    printf '%s' "$_orig_git_commit" > "$GIT_COMMIT_FILE"
}
trap restore_git_commit EXIT
printf '%s' "$GIT_SHA" > "$GIT_COMMIT_FILE"
echo "Stamped GIT_COMMIT = $GIT_SHA"
echo

# Ensure the dev image exists for the target platform (needed as builder base).
# A simple `docker image inspect` doesn't check platform, so rebuild if cross-compiling.
_need_dev=false
if ! docker image inspect pagespeed2-dev >/dev/null 2>&1; then
    _need_dev=true
elif [ "$(uname -m)" = "arm64" ] && [ "$BUILD_PLATFORM" = "linux/amd64" ]; then
    _need_dev=true
fi
if [ "$_need_dev" = true ]; then
    echo "Building dev image for $BUILD_PLATFORM..."
    docker build --platform "$BUILD_PLATFORM" $NO_CACHE \
        -t pagespeed2-dev "$SCRIPT_DIR"
    echo
fi

# Build the base-runtime image (required by Dockerfile.worker).
echo "=== Building base-runtime image ==="
docker build --platform "$BUILD_PLATFORM" $NO_CACHE \
    --build-arg APT_REFRESH="$APT_REFRESH" \
    -f "$SCRIPT_DIR/Dockerfile.base-runtime" \
    -t pagespeed2-base-runtime \
    "$PROJECT_DIR"
echo

echo "=== Building worker image ==="
docker build --ssh default \
    --platform "$BUILD_PLATFORM" \
    $NO_CACHE \
    --build-arg APT_REFRESH="$APT_REFRESH" \
    --build-arg VERSION="$VERSION" \
    --build-arg BUILD_DATE="$BUILD_DATE" \
    -f "$SCRIPT_DIR/Dockerfile.worker" \
    -t "modpagespeed/worker:$VERSION" \
    "$PROJECT_DIR"

echo
echo "=== Building nginx image ==="
docker build --ssh default \
    --platform "$BUILD_PLATFORM" \
    $NO_CACHE \
    --build-arg APT_REFRESH="$APT_REFRESH" \
    --build-arg VERSION="$VERSION" \
    --build-arg BUILD_DATE="$BUILD_DATE" \
    -f "$SCRIPT_DIR/Dockerfile.nginx" \
    -t "modpagespeed/nginx:$VERSION" \
    "$PROJECT_DIR"

# Keep :latest pointing at the newest build. The image CVE gate
# (tools/sbom/dep-scan.sh PRODUCT_IMAGES) scans the :latest tags, so without
# this a versioned build leaves the gate auditing whatever was last built with
# no VERSION argument -- an image nobody ships. That is exactly how :latest came
# to be ten days and several releases stale while the gate reported on it.
if [ "$VERSION" != "latest" ]; then
    echo
    echo "=== Retagging :latest -> $VERSION ==="
    for IMAGE in worker nginx; do
        docker tag "modpagespeed/$IMAGE:$VERSION" "modpagespeed/$IMAGE:latest"
        echo "  modpagespeed/$IMAGE:latest -> $VERSION"
    done
fi

# The shared cache-volume contract is an IMAGE property (fixed GID in both
# images, unprivileged optimizer, nothing world-writable, an adoptable
# pre-2.1 volume), so it is only assertable once both images exist. Run it
# here rather than leaving the pair's central claim -- that the nginx peer
# can actually open the optimizer's cache -- unasserted until an operator
# finds out in production. --no-smoke skips it.
if [ "$SMOKE" = true ]; then
    echo
    echo "=== Cache-sharing gate ==="
    if ! "$SCRIPT_DIR/../tools/ci/test-image-cache-sharing.sh" \
            "modpagespeed/worker:$VERSION" "modpagespeed/nginx:$VERSION"; then
        echo "ERROR: the cache-sharing gate failed; images are NOT fit to publish." >&2
        exit 1
    fi
fi

# Tag and push to Artifact Registry if requested
if [ "$PUSH_AR" = true ]; then
    echo
    echo "=== Pushing to Artifact Registry ==="
    for IMAGE in worker nginx; do
        AR_TAG="$AR_REPO/$IMAGE:$VERSION"
        docker tag "modpagespeed/$IMAGE:$VERSION" "$AR_TAG"
        docker push "$AR_TAG"
        echo "  Pushed: $AR_TAG"
    done
fi

echo
echo "Done! Images built:"
echo "  modpagespeed/worker:$VERSION"
echo "  modpagespeed/nginx:$VERSION"
if [ "$PUSH_AR" = true ]; then
    echo
    echo "Pushed to Artifact Registry:"
    echo "  $AR_REPO/worker:$VERSION"
    echo "  $AR_REPO/nginx:$VERSION"
fi
echo
echo "To push manually:"
echo "  docker push modpagespeed/worker:$VERSION"
echo "  docker push modpagespeed/nginx:$VERSION"
