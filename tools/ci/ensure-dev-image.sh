#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2024-2026 We-Amp B.V.
#
# ensure-dev-image.sh - Name, and if missing build, the content-hashed
# pagespeed1.1-dev image for a docker/ tree. Prints the image reference
# (pagespeed1.1-dev:<arch>-<hash>) on stdout; every log line goes to stderr.
#
# Usage: tools/ci/ensure-dev-image.sh [options] <arch> <docker-dir>
#   --name-only          compute and print the name; never build
#   --tag-latest         also tag the image pagespeed1.1-dev:latest
#   --lock-timeout SECS  bound on waiting for a concurrent build of the same
#                        tag (default ${CI_IMAGE_LOCK_TIMEOUT:-1200})
#   --derived REPO FILE  then ensure REPO:<arch>-<hash>, built from
#                        <docker-dir>/FILE with --build-arg BASE=<dev image>
#                        under the same lock scheme, and print THAT name
#                        (e.g. --derived pagespeed1.1-lint Dockerfile.lint)
#
# The ONE definition of the image tag. Every workflow and every
# arch computes it here, so the same Dockerfile bytes get the same tag
# whatever the workspace path: the hash covers each file's content and its
# path RELATIVE to <docker-dir> (sha256sum prints "<hash>  <path>", so an
# absolute path would fold the workspace location into the tag and a host
# would keep one duplicate image per workflow), in C-locale order (sort is
# locale-sensitive: en_US puts build-release.sh among the Dockerfile.*).
#
# Concurrent builds of one tag on one host (two runner instances share the
# Docker daemon) are serialised by flock(1) on a lock file in
# ${CI_DEV_IMAGE_LOCK_DIR:-/tmp}, created (mode 1777) when missing. The
# kernel drops the lock when the holder dies, so a cancelled (SIGKILLed)
# build never strands a waiter; the wait is bounded anyway, and on timeout
# this builds unlocked, since a duplicate build of one tag is wasted
# minutes, not a correctness problem. A host without flock builds
# unlocked. A build that fails while the image exists afterwards counts as
# success: a concurrent unlocked build of the same content-hashed tag won
# the race. A derived image (--derived: the lint image) shares the dev
# image's tag hash and takes its own lock file,
# ${CI_DEV_IMAGE_LOCK_DIR:-/tmp}/<repo>-build-<arch>-<hash>.lock, through the
# same code.
#
# Before building, this also removes the mkdir lock directories of the
# legacy scheme (/tmp/docker-build-<arch>-<hash>.lock). Nothing
# reads them any more, but a SIGKILLed holder stranded one, and runs of
# branches whose workflow predates the current scheme wait on such a directory with no
# bound. Only EMPTY directories (rmdir; the old scheme never put a file in
# one) older than ${CI_OLD_LOCK_MAX_AGE_MIN:-150} minutes go: every job
# that ever created one (Linux Build and Linux arm64 Build in ci.yml and
# release.yml) had timeout-minutes of at most 90 and created it inside the
# job, so a directory past 150 minutes has no live holder.

set -euo pipefail

NAME_ONLY=0
TAG_LATEST=0
LOCK_TIMEOUT="${CI_IMAGE_LOCK_TIMEOUT:-1200}"
DERIVED_REPO=""
DERIVED_FILE=""

usage() { sed -n '5,17p' "$0" | sed 's/^# \{0,1\}//' >&2; }
log() { echo "ensure-dev-image: $*" >&2; }

while [ $# -gt 0 ]; do
  case "$1" in
    --name-only) NAME_ONLY=1; shift ;;
    --tag-latest) TAG_LATEST=1; shift ;;
    --lock-timeout) LOCK_TIMEOUT="${2:-}"; shift 2 ;;
    --derived)
      [ $# -ge 3 ] || { echo "--derived needs REPO and FILE" >&2; exit 2; }
      DERIVED_REPO="$2"; DERIVED_FILE="$3"; shift 3 ;;
    -h|--help) usage; exit 0 ;;
    --) shift; break ;;
    -*) echo "unknown option: $1" >&2; usage; exit 2 ;;
    *) break ;;
  esac
done
[ $# -eq 2 ] || { usage; exit 2; }
ARCH="$1"
DOCKER_DIR="$2"
case "$LOCK_TIMEOUT" in
  ''|*[!0-9]*) echo "--lock-timeout needs whole seconds, got '${LOCK_TIMEOUT}'" >&2; exit 2 ;;
esac
if [ -n "$DERIVED_REPO" ]; then
  case "$DERIVED_REPO" in
    pagespeed1.1-dev|*[!a-z0-9._-]*|[!a-z0-9]*)
      echo "--derived needs an image repository name other than pagespeed1.1-dev, got '${DERIVED_REPO}'" >&2; exit 2 ;;
  esac
  case "$DERIVED_FILE" in
    ''|/*|*..*) echo "--derived needs a Dockerfile path relative to <docker-dir>, got '${DERIVED_FILE}'" >&2; exit 2 ;;
  esac
fi

hash_cmd() {
  if command -v sha256sum >/dev/null 2>&1; then
    sha256sum "$@"
  else
    shasum -a 256 "$@"
  fi
}

# Loud failure on a missing or empty tree: hashing nothing still yields a
# well-formed tag (the empty-input digest), which would name an image no
# Dockerfile describes.
if [ ! -d "$DOCKER_DIR" ] \
   || [ -z "$(cd "$DOCKER_DIR" && find . -type f | head -n 1)" ]; then
  echo "::error::no docker/ tree at ${DOCKER_DIR} -- cannot name the image" >&2
  exit 1
fi
DIR_HASH=$(cd "$DOCKER_DIR" \
  && find . -type f | LC_ALL=C sort \
  | while IFS= read -r f; do hash_cmd "$f"; done \
  | hash_cmd | cut -c1-12)
case "$DIR_HASH" in
  [0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f]) ;;
  *) echo "::error::could not hash ${DOCKER_DIR} (got '${DIR_HASH}')" >&2; exit 1 ;;
esac
IMAGE="pagespeed1.1-dev:${ARCH}-${DIR_HASH}"
if [ -n "$DERIVED_REPO" ] && [ ! -f "${DOCKER_DIR}/${DERIVED_FILE}" ]; then
  echo "::error::no ${DERIVED_FILE} in ${DOCKER_DIR} -- cannot build ${DERIVED_REPO}" >&2
  exit 1
fi
DERIVED_IMAGE=""
[ -z "$DERIVED_REPO" ] || DERIVED_IMAGE="${DERIVED_REPO}:${ARCH}-${DIR_HASH}"

if [ "$NAME_ONLY" -eq 1 ]; then
  echo "${DERIVED_IMAGE:-$IMAGE}"
  exit 0
fi

LOCK_DIR="${CI_DEV_IMAGE_LOCK_DIR:-/tmp}"

# Legacy lock directories (see the header). Best effort: a directory
# another user owns, or one that is not empty, stays.
sweep_old_lock_dirs() {
  local dir="${CI_OLD_LOCK_DIR:-/tmp}" age="${CI_OLD_LOCK_MAX_AGE_MIN:-150}" d
  case "$age" in ''|*[!0-9]*) age=150 ;; esac
  while IFS= read -r d; do
    if rmdir "$d" 2>/dev/null; then
      log "removed stale legacy lock directory ${d}"
    fi
  done < <(find "$dir" -mindepth 1 -maxdepth 1 -type d \
             -name 'docker-build-*.lock' -mmin +"$age" 2>/dev/null || true)
}

image_exists() { docker image inspect "$1" >/dev/null 2>&1; }

# build_if_missing <image> <docker build args...>
build_if_missing() {
  local image="$1"
  shift
  if image_exists "$image"; then
    log "image cache hit: ${image}"
  else
    log "building ${image}"
    local rc=0
    docker build -t "$image" "$@" >&2 || rc=$?
    if [ "$rc" -ne 0 ]; then
      # A build of the same tag that ran unlocked (no flock, an unusable
      # lock file, or a timed-out wait) can lose the race to another job
      # on this daemon ("already exists"). The tag is content-hashed, so
      # an image present now is the image this build would have made.
      if image_exists "$image"; then
        log "docker build of ${image} failed, but the image exists now (a concurrent build won); using it"
      else
        return "$rc"
      fi
    fi
  fi
}

# ensure_image <image> <docker build args...>: build <image> if missing,
# with concurrent builds of that tag on this host serialised by flock on
# ${LOCK_DIR}/<repo>-build-<arch>-<hash>.lock (see the header).
ensure_image() {
  local image="$1" lock_file start waited
  lock_file="${LOCK_DIR}/${image%%:*}-build-${ARCH}-${DIR_HASH}.lock"
  if image_exists "$image"; then
    log "image cache hit: ${image}"
    return 0
  fi
  if ! command -v flock >/dev/null 2>&1; then
    log "flock not available; building ${image} without the per-host lock"
    build_if_missing "$@"
    return 0
  fi
  # The lock directory may not exist yet (a cleared /tmp). Create it best
  # effort and, when this call made it, open it to every runner user: it
  # holds only empty lock files.
  if [ ! -d "$LOCK_DIR" ] && mkdir -p "$LOCK_DIR" 2>/dev/null; then
    chmod 1777 "$LOCK_DIR" 2>/dev/null || true
  fi
  # Read-write if we can, read-only if another user created the file:
  # flock(2) needs an open descriptor, not write access.
  if ! { exec 9>>"$lock_file"; } 2>/dev/null \
     && ! { exec 9<"$lock_file"; } 2>/dev/null; then
    log "cannot open ${lock_file}; building ${image} without the per-host lock"
    build_if_missing "$@"
    return 0
  fi
  # Poll with -n rather than -w: busybox flock has no -w, and polling
  # lets a waiter leave as soon as the holder's tag appears.
  start=$SECONDS
  waited=0
  until flock -n 9; do
    if [ "$waited" -eq 0 ]; then
      log "waiting for a concurrent build of ${image} (lock ${lock_file}, up to ${LOCK_TIMEOUT}s)"
      waited=1
    fi
    if image_exists "$image"; then
      break
    fi
    if [ $((SECONDS - start)) -ge "$LOCK_TIMEOUT" ]; then
      echo "::warning::waited ${LOCK_TIMEOUT}s for the build lock on ${image}; building without it (a duplicate build of one tag is harmless)" >&2
      break
    fi
    sleep 2
  done
  build_if_missing "$@"
  # Closing the descriptor drops the lock before the next image's turn.
  exec 9>&-
}

sweep_old_lock_dirs
ensure_image "$IMAGE" "$DOCKER_DIR"
if [ "$TAG_LATEST" -eq 1 ]; then
  docker tag "$IMAGE" pagespeed1.1-dev:latest
fi
if [ -n "$DERIVED_IMAGE" ]; then
  ensure_image "$DERIVED_IMAGE" --build-arg BASE="$IMAGE" \
    -f "${DOCKER_DIR}/${DERIVED_FILE}" "$DOCKER_DIR"
fi
echo "${DERIVED_IMAGE:-$IMAGE}"
