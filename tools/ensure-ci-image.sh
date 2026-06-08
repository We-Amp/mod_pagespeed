#!/usr/bin/env bash
# Ensure the pagespeed1.1-dev Docker image is available.
# Content-hashes docker/ → pagespeed1.1-dev:{arch}-{hash}
# Builds only when the hash changes.
#
# Usage:
#   tools/ensure-ci-image.sh [docker-context-dir]
#
# In GitHub Actions, exports DOCKER_IMAGE to $GITHUB_ENV.
#
# Registry resilience (a CI postmortem): the Dockerfile's FROM base lives on
# Docker Hub. On the macOS CI runners an anonymous, un-pre-pulled
# metadata fetch was observed to HANG for 3+ hours on BuildKit's
# "load metadata for docker.io/..." step (not a single layer was ever pulled),
# burning the whole job timeout. To make that impossible:
#   - pre-pull each external FROM base so BuildKit resolves metadata from the
#     LOCAL cache instead of a (possibly hanging) live docker.io fetch;
#   - bound every registry operation with a timeout so a stall fails FAST
#     (minutes, with a clear error) rather than hanging the job;
#   - authenticate to Docker Hub when DOCKERHUB_TOKEN is present to dodge
#     anonymous pull rate limits.
# All three are best-effort and degrade to the previous behaviour.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
DOCKER_DIR="${1:-${SCRIPT_DIR}/../docker}"
DOCKER_DIR="$(cd "$DOCKER_DIR" && pwd)"

BASE_PULL_TIMEOUT="${BASE_PULL_TIMEOUT:-300}"      # per docker-pull attempt (s)
IMAGE_BUILD_TIMEOUT="${IMAGE_BUILD_TIMEOUT:-4200}" # whole docker-build (s)

# Determine architecture tag
case "$(uname -m)" in
  x86_64)        ARCH_TAG="x64"  ;;
  aarch64|arm64) ARCH_TAG="arm64" ;;
  *) echo "Unsupported architecture: $(uname -m)" >&2; exit 1 ;;
esac

# Portable bounded run: GNU `timeout` (Linux), `gtimeout` (macOS + coreutils),
# else a background+kill fallback (macOS ships no `timeout` by default). Returns
# the command's exit code, or non-zero if it was killed for exceeding <secs>.
_timeout() {
  local secs="$1"; shift
  local rc=0
  if command -v timeout >/dev/null 2>&1; then
    timeout "$secs" "$@" || rc=$?
  elif command -v gtimeout >/dev/null 2>&1; then
    gtimeout "$secs" "$@" || rc=$?
  else
    "$@" &
    local pid=$!
    ( sleep "$secs"; kill -TERM "$pid" 2>/dev/null ) >/dev/null 2>&1 &
    local watcher=$!
    if wait "$pid" 2>/dev/null; then rc=0; else rc=$?; fi
    kill "$watcher" 2>/dev/null || true
    wait "$watcher" 2>/dev/null || true
  fi
  return "$rc"
}

# Authenticate to Docker Hub if creds are present (best-effort). Skipped when
# DOCKERHUB_TOKEN is unset — identical to the previous anonymous behaviour.
docker_login_if_creds() {
  [ -n "${DOCKERHUB_TOKEN:-}" ] || return 0
  if printf '%s' "$DOCKERHUB_TOKEN" \
       | docker login -u "${DOCKERHUB_USERNAME:-}" --password-stdin >/dev/null 2>&1; then
    echo "ensure-ci-image: authenticated to Docker Hub"
  else
    echo "ensure-ci-image: WARNING docker login failed; continuing unauthenticated" >&2
  fi
}

# Pre-pull every external FROM base (skipping multi-stage aliases and 'scratch')
# so BuildKit never has to do a live metadata fetch mid-build. Bounded:
# BASE_PULL_TIMEOUT per attempt, 3 attempts with backoff.
prepull_base_images() {
  local aliases img i ok rc=0
  aliases=" $(awk 'toupper($1)=="FROM" && toupper($3)=="AS"{print $4}' "${DOCKER_DIR}/Dockerfile" | tr '\n' ' ') "
  for img in $(awk 'toupper($1)=="FROM"{print $2}' "${DOCKER_DIR}/Dockerfile"); do
    case "$aliases" in *" $img "*) continue ;; esac   # skip stage aliases
    [ "$img" = "scratch" ] && continue
    if docker image inspect "$img" >/dev/null 2>&1; then
      echo "ensure-ci-image: base present locally: $img"
      continue
    fi
    ok=0
    for i in 1 2 3; do
      echo "ensure-ci-image: pulling base $img (attempt $i, ${BASE_PULL_TIMEOUT}s cap)..."
      if _timeout "$BASE_PULL_TIMEOUT" docker pull "$img"; then ok=1; break; fi
      sleep $((i * 10))
    done
    if [ "$ok" != 1 ]; then
      echo "::error::ensure-ci-image: could not pull base image '$img' within ${BASE_PULL_TIMEOUT}s x3 (Docker Hub unreachable or rate-limited). Set DOCKERHUB_TOKEN, mirror the image, or check egress to docker.io." >&2
      rc=1
    fi
  done
  return "$rc"
}

# Build the dev image: authenticate, pre-pull bases (fail fast on a registry
# stall), then build under a bounded timeout so nothing can hang the job.
build_image() {
  docker_login_if_creds
  prepull_base_images || return 1
  if ! _timeout "$IMAGE_BUILD_TIMEOUT" docker build -t "$IMAGE" "$DOCKER_DIR"; then
    echo "::error::ensure-ci-image: docker build exceeded ${IMAGE_BUILD_TIMEOUT}s or failed for $IMAGE" >&2
    return 1
  fi
}

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
    build_image
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
      build_image
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
