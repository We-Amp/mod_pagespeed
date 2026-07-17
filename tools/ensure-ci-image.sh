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
#
# Daemon reachability: every step below goes through the docker CLI, so an
# unreachable daemon makes `docker image inspect` read as a cache miss and the
# subsequent `docker pull` read as a registry failure -- reporting "Docker Hub
# unreachable or rate-limited" for what is really a local socket that is not
# there. Probe the daemon once, up front, and name the real cause.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
DOCKER_DIR="${1:-${SCRIPT_DIR}/../docker}"
DOCKER_DIR="$(cd "$DOCKER_DIR" && pwd)"

BASE_PULL_TIMEOUT="${BASE_PULL_TIMEOUT:-300}"      # per docker-pull attempt (s)
IMAGE_BUILD_TIMEOUT="${IMAGE_BUILD_TIMEOUT:-4200}" # whole docker-build (s)
DAEMON_PING_TIMEOUT="${DAEMON_PING_TIMEOUT:-30}"   # daemon reachability probe (s)

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

# Fail fast, and accurately, when the docker daemon is not reachable. Bounded so
# a hung socket cannot stall the job. A WSL runner was seen with a healthy
# native dockerd whose /run/docker.sock had been unlinked out from under it by
# Docker Desktop's WSL integration: every docker call failed, and the pre-pull
# blamed Docker Hub, which sent the investigation at the registry instead of the
# host. The distinguishing tell was in the timing -- the pulls failed instantly
# rather than burning their 300s cap -- which is far too subtle to rely on.
require_docker_daemon() {
  local out rc=0 attempt
  # Poll before declaring it down. A single probe turns a transient blip (host
  # resuming, a daemon restart, a socket re-bind) into a hard failure -- the
  # recurring "Docker preflight" flake that a re-run then turns green. Same
  # 5x/3s shape as the 2.0 optimizer line tools/ci/docker-preflight.sh (its), which hit
  # this class first; unlike its bare `docker info`, each probe here is bounded,
  # so a HUNG daemon still cannot wedge the job.
  for attempt in 1 2 3 4 5; do
    rc=0
    out="$(_timeout "$DAEMON_PING_TIMEOUT" docker version --format "{{.Server.Version}}" 2>&1)" || rc=$?
    [ "$rc" -eq 0 ] && break
    [ "$attempt" -lt 5 ] && sleep 3
  done
  # Flatten to a single line: a ::error:: annotation stops at the first newline,
  # and docker prefixes its error with the failed template's empty line. The
  # annotation is the record that MATTERS here -- the per-step log blob on these
  # runners can vanish within minutes, while annotations persist -- so a
  # multi-line $out silently truncates the cause to "docker said: " and nothing.
  out="$(printf '%s' "$out" | tr '\n\r\t' '   ' | sed -e 's/  */ /g' -e 's/^ //' -e 's/ $//')"
  if [ "$rc" -eq 0 ]; then
    if [ "$attempt" -gt 1 ]; then
      echo "ensure-ci-image: docker daemon reachable (server ${out}) after ${attempt} attempt(s) — the daemon was briefly unreachable"
    else
      echo "ensure-ci-image: docker daemon reachable (server ${out})"
    fi
    return 0
  fi
  echo "::error::ensure-ci-image: docker daemon is NOT reachable — this is a HOST problem, not a registry problem; nothing below can work. Check that the daemon is running and that its socket exists (on a WSL runner, Docker Desktop's WSL integration can unlink a native dockerd's socket out from under it). DOCKER_HOST=${DOCKER_HOST:-<unset: using the active docker context>}. docker said: ${out}" >&2
  return 1
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

# Some self-hosted runners (notably headless macOS) have a docker `credsStore`
# (e.g. "desktop" / "osxkeychain") whose credential helper HANGS when invoked
# without an interactive session -- so even an ANONYMOUS pull of a PUBLIC base
# image stalls until the timeout kills it:
#   error getting credentials - err: signal: terminated, out: ``
# (observed repeatedly on mac-x64-auxiliary -> Apache System Tests "Ensure
# Docker image"; Docker Desktop's GUI exits seconds after launch on that box, so
# docker-credential-desktop has no backend to talk to and blocks). Point
# DOCKER_CONFIG at an ephemeral, credsStore-free config so docker never consults
# the host keychain for anonymous pulls; a DOCKERHUB_TOKEN login (if any) then
# writes its auth into THIS clean config as base64, which works headless.
# No-op when the active config has no credsStore (e.g. the Linux runners).
neutralize_hanging_credstore() {
  local active_cfg="${DOCKER_CONFIG:-$HOME/.docker}/config.json"
  grep -q '"credsStore"' "$active_cfg" 2>/dev/null || return 0
  local clean
  clean="$(mktemp -d)"
  printf '{}\n' > "${clean}/config.json"
  export DOCKER_CONFIG="$clean"
  echo "ensure-ci-image: active docker config has a credsStore; using clean DOCKER_CONFIG=${DOCKER_CONFIG} to avoid a headless credential-helper hang"
}

# Build the dev image: dodge a hanging host credsStore, authenticate, pre-pull
# bases (fail fast on a registry stall), then build under a bounded timeout so
# nothing can hang the job.
build_image() {
  neutralize_hanging_credstore
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

# Before the cache probe: a dead daemon would otherwise look like a cache miss
# and send us into a build that cannot possibly succeed.
require_docker_daemon

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
