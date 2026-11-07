#!/bin/sh
# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2024-2026 We-Amp B.V.
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
# DOCKER_SOCK     host path of the daemon socket: the unix:// path in
#                 DOCKER_HOST when set (a rootless daemon listens under
#                 $XDG_RUNTIME_DIR), else /var/run/docker.sock.
# DOCKER_GID      group of DOCKER_SOCK, for --group-add; 0 when unknown and
#                 on a rootless daemon.
set -eu

uid="$(id -u)"
gid="$(id -g)"
rootless=0
case "$(docker info -f '{{.SecurityOptions}}' 2>/dev/null || true)" in
  *rootless*) uid=0; gid=0; rootless=1 ;;
esac

sock=/var/run/docker.sock
case "${DOCKER_HOST:-}" in
  unix://*) sock="${DOCKER_HOST#unix://}" ;;
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

printf "CONTAINER_USER='%s'\nDOCKER_SOCK='%s'\nDOCKER_GID='%s'\n" "$uid:$gid" "$sock" "$sgid"
