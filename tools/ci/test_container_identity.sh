#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2024-2026 We-Amp B.V.
#
# Hermetic test of container-identity.sh: the container user and the daemon
# socket a job's test container gets, for a rootful and a rootless daemon.
# docker is a shim; the "socket" is a temp file.
set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
SUT="$HERE/container-identity.sh"
ROOT="$(mktemp -d "${TMPDIR:-/tmp}/container-identity-test.XXXXXX")"
trap 'rm -rf "$ROOT"' EXIT
fail() { echo "FAIL: $1" >&2; exit 1; }
pass() { echo "ok: $1"; }

mkdir -p "$ROOT/bin"
cat > "$ROOT/bin/docker" <<'SH'
#!/bin/sh
[ "$1" = info ] || exit 1
case "${FAKE_DAEMON:-rootful}" in
  rootful)  echo '[name=apparmor,profile=default name=seccomp,profile=builtin name=cgroupns]' ;;
  rootless) echo '[name=seccomp,profile=builtin name=rootless name=cgroupns]' ;;
  down)     echo 'Cannot connect to the Docker daemon' >&2; exit 1 ;;
esac
SH
chmod +x "$ROOT/bin/docker"
SOCK="$ROOT/run user/docker.sock"
mkdir -p "$(dirname "$SOCK")"; : > "$SOCK"
ME="$(id -u):$(id -g)"
MYGID="$(id -g)"

# Run the helper the way the workflows do (eval its output) and print one var.
get() { # var, then env assignments
  local var="$1"; shift
  # shellcheck disable=SC2016 # expanded by the inner sh, not here
  env -u DOCKER_HOST PATH="$ROOT/bin:$PATH" "$@" sh -c 'eval "$("$0")" && eval "printf %s \"\${$1}\""' "$SUT" "$var"
}

[ "$(get CONTAINER_USER FAKE_DAEMON=rootful)" = "$ME" ] || fail "rootful daemon: container user is the runner's uid:gid"
pass "rootful daemon: container user is the runner's uid:gid"
[ "$(get CONTAINER_USER FAKE_DAEMON=rootless)" = "0:0" ] || fail "rootless daemon: container user is 0:0"
pass "rootless daemon: container user is 0:0 (the runner user on the host)"
[ "$(get CONTAINER_USER FAKE_DAEMON=down)" = "$ME" ] || fail "unreachable daemon: falls back to the runner's uid:gid"
pass "unreachable daemon: falls back to the runner's uid:gid"

[ "$(get DOCKER_SOCK)" = "/var/run/docker.sock" ] || fail "no DOCKER_HOST: default socket"
pass "no DOCKER_HOST: /var/run/docker.sock"
[ "$(get DOCKER_SOCK DOCKER_HOST="unix://$SOCK")" = "$SOCK" ] || fail "unix DOCKER_HOST: socket taken from it"
pass "unix:// DOCKER_HOST: socket path taken from it (a path with a space survives eval)"
[ "$(get DOCKER_SOCK DOCKER_HOST=tcp://127.0.0.1:2375)" = "/var/run/docker.sock" ] || fail "tcp DOCKER_HOST: default socket"
pass "tcp:// DOCKER_HOST: default socket"

[ "$(get DOCKER_GID DOCKER_HOST="unix://$SOCK")" = "$MYGID" ] || fail "socket gid read from the socket file"
pass "DOCKER_GID is the socket file's group"
[ "$(get DOCKER_GID DOCKER_HOST="unix://$ROOT/missing.sock")" = "0" ] || fail "missing socket: DOCKER_GID 0"
pass "missing socket: DOCKER_GID falls back to 0"

if env -u DOCKER_HOST PATH="$ROOT/bin:$PATH" DOCKER_HOST="unix://$ROOT/it's.sock" "$SUT" >/dev/null 2>&1; then
  fail "a socket path with a single quote is refused"
fi
pass "a socket path with a single quote is refused"
echo "all container-identity tests passed"
