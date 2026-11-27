#!/bin/sh
# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2024-2026 We-Amp B.V.
#
# Keep in sync with the mod_pagespeed copy (tools/ci/container-identity.sh).
#
# container-identity.sh -- print shell assignments for the user a job's test
# container runs as and for the daemon socket it is handed. Usage in a
# workflow step:
#
#   eval "$(tools/ci/container-identity.sh)"
#   docker run --user "$CONTAINER_USER" --group-add "$DOCKER_GID" \
#     -v "$DOCKER_SOCK":/var/run/docker.sock ...
#
# CONTAINER_USER  "<uid>:<gid>" of the runner user on a rootful daemon, so
#                 files the container writes into a bind-mounted workspace
#                 stay the runner's. On a ROOTLESS daemon the runner's uid
#                 inside the container maps to a subordinate id on the host,
#                 which cannot write the workspace; container root is the
#                 runner user there, so this is "0:0".
# DOCKER_SOCK     host path of the daemon socket: the unix:// endpoint of the
#                 active Docker context (honours DOCKER_HOST, DOCKER_CONTEXT
#                 and the current context; a rootless daemon listens under
#                 $XDG_RUNTIME_DIR), else DOCKER_HOST, else
#                 /var/run/docker.sock.
# DOCKER_GID      group of DOCKER_SOCK, for --group-add; 0 when unknown and
#                 on a rootless daemon.
# DOCKER_ROOTLESS 1 under a rootless daemon, else 0.
set -eu

uid="$(id -u)"
gid="$(id -g)"
rootless=0
# Bound the daemon query so a wedged daemon cannot hang the job.
tmo=""
command -v timeout >/dev/null 2>&1 && tmo="timeout 10"
# shellcheck disable=SC2086 # $tmo is empty or two plain words
case "$($tmo docker info -f '{{.SecurityOptions}}' 2>/dev/null || true)" in
  *name=rootless*) uid=0; gid=0; rootless=1 ;;
esac

sock=/var/run/docker.sock
host="$($tmo docker context inspect -f '{{.Endpoints.docker.Host}}' 2>/dev/null || true)"
case "$host" in
  unix://*) ;;
  *) host="${DOCKER_HOST:-}" ;;
esac
case "$host" in
  unix://*) sock="${host#unix://}" ;;
esac
case "$sock" in
  *"'"*) echo "container-identity: refusing a socket path with a single quote: $sock" >&2; exit 1 ;;
esac

sgid="$(stat -c '%g' "$sock" 2>/dev/null || stat -f '%g' "$sock" 2>/dev/null || echo 0)"
case "$sgid" in ''|*[!0-9]*) sgid=0 ;; esac
# A rootless daemon's socket belongs to a subordinate group outside the
# container's id map (--group-add of it fails); container root owns the
# socket there anyway.
[ "$rootless" = 1 ] && sgid=0

printf "CONTAINER_USER='%s'\nDOCKER_SOCK='%s'\nDOCKER_GID='%s'\nDOCKER_ROOTLESS='%s'\n" "$uid:$gid" "$sock" "$sgid" "$rootless"
