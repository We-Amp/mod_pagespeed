#!/usr/bin/env bash
# Ensure the pagespeed1.1-dev Docker image is available.
# Content-hashes docker/ → pagespeed1.1-dev:{arch}-{hash}
# Builds only when the hash changes.
#
# Usage:
#   tools/ensure-ci-image.sh [docker-context-dir]
#
# In GitHub Actions, exports DOCKER_IMAGE to $GITHUB_ENV.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
DOCKER_DIR="${1:-${SCRIPT_DIR}/../docker}"
DOCKER_DIR="$(cd "$DOCKER_DIR" && pwd)"

# Determine architecture tag
case "$(uname -m)" in
  x86_64)        ARCH_TAG="x64"  ;;
  aarch64|arm64) ARCH_TAG="arm64" ;;
  *) echo "Unsupported architecture: $(uname -m)" >&2; exit 1 ;;
esac

# Content hash of docker directory (filenames + contents for rename detection).
# Excludes .DS_Store to avoid macOS/Linux hash divergence.
hash_docker_dir() {
  cd "$DOCKER_DIR"
  # Hash sorted filenames, then sorted file contents
  {
    find . -type f ! -name '.DS_Store' | LC_ALL=C sort
    find . -type f ! -name '.DS_Store' -print0 | LC_ALL=C sort -z | xargs -0 cat
  }
}

if command -v sha256sum >/dev/null 2>&1; then
  HASH=$(hash_docker_dir | sha256sum | cut -c1-12)
else
  HASH=$(hash_docker_dir | shasum -a 256 | cut -c1-12)
fi

IMAGE="pagespeed1.1-dev:${ARCH_TAG}-${HASH}"

if docker image inspect "$IMAGE" >/dev/null 2>&1; then
  echo "Image cache hit: $IMAGE"
else
  echo "Building image $IMAGE ..."

  # Concurrent build protection via lockdir (atomic on all filesystems)
  LOCKDIR="/tmp/pagespeed1.1-dev-build-${ARCH_TAG}-${HASH}.lock"
  MAX_WAIT=1800  # 30 minutes
  if mkdir "$LOCKDIR" 2>/dev/null; then
    trap 'rmdir "$LOCKDIR" 2>/dev/null || true' EXIT
    docker build -t "$IMAGE" "$DOCKER_DIR"
  else
    echo "Another build in progress, waiting (max ${MAX_WAIT}s) ..."
    elapsed=0
    while [ -d "$LOCKDIR" ]; do
      sleep 5
      elapsed=$((elapsed + 5))
      if [ "$elapsed" -ge "$MAX_WAIT" ]; then
        echo "Lock wait timed out after ${MAX_WAIT}s — removing stale lock"
        # Use rmdir (safe, won't follow symlinks or delete contents)
        rmdir "$LOCKDIR" 2>/dev/null || true
        break
      fi
    done
    if ! docker image inspect "$IMAGE" >/dev/null 2>&1; then
      echo "Image not available after wait, building ..."
      # Must acquire lock before building; fail if we can't
      if ! mkdir "$LOCKDIR" 2>/dev/null; then
        echo "ERROR: Could not acquire build lock" >&2
        exit 1
      fi
      trap 'rmdir "$LOCKDIR" 2>/dev/null || true' EXIT
      docker build -t "$IMAGE" "$DOCKER_DIR"
    fi
  fi

  echo "Image built: $IMAGE"
fi

# Also tag as latest for backward compatibility
docker tag "$IMAGE" "pagespeed1.1-dev:latest"

# Export for GitHub Actions
if [ -n "${GITHUB_ENV:-}" ]; then
  echo "DOCKER_IMAGE=$IMAGE" >> "$GITHUB_ENV"
fi

echo "$IMAGE"
