#!/bin/sh
# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2024-2026 We-Amp B.V.
#
# stage-docker-cli.sh -- stage a Linux docker CLI + compose plugin where the
# Docker DAEMON can bind-mount them, and print shell assignments for
# DOCKER_BIN and COMPOSE_PLUGIN. Usage in a workflow step:
#
#   eval "$(tools/ci/stage-docker-cli.sh)"
#   docker run ... -v "$DOCKER_BIN":/usr/bin/docker:ro \
#                  -v "$COMPOSE_PLUGIN":/usr/local/lib/docker/cli-plugins/docker-compose:ro ...
#
# WHY THIS EXISTS
#   Compose-based lanes run pytest inside a throwaway container and hand it the
#   host's docker CLI by bind mount. A bind-mount source path is resolved by the
#   DAEMON, not by the shell that runs `docker run`. When the runner is a Linux
#   container (or a macOS shell) whose daemon is Docker Desktop, the daemon
#   host is the Desktop VM, which has no /usr/bin/docker: Docker then creates
#   an EMPTY DIRECTORY at the mount source and the test process fails with
#   `PermissionError: [Errno 13] Permission denied: 'docker'` (exec of a
#   directory). On a bare-metal Linux runner the runner host is the daemon host
#   and the mount just works -- which is why the defect only shows on Desktop-
#   backed runners.
#
#   The one path every runner shape shares with its daemon is the runner's own
#   work directory (the repo checkout is bind-mounted from it on every lane
#   already), so the CLI and plugin are copied under $RUNNER_TEMP and mounted
#   from there. Linux runners copy their host binaries; macOS runners (darwin
#   host binaries cannot run in a Linux container) download the Linux static
#   builds for the daemon's architecture, as the periodic lanes already did.
set -eu

STAGE_DIR="${RUNNER_TEMP:-${TMPDIR:-/tmp}}/ci-docker-cli"
mkdir -p "$STAGE_DIR"
DOCKER_BIN="$STAGE_DIR/docker"
COMPOSE_PLUGIN="$STAGE_DIR/docker-compose"

RETRY="$(cd "$(dirname "$0")" && pwd)/fetch_retry.sh"
DOCKER_VERSION="${STAGE_DOCKER_VERSION:-27.5.1}"
COMPOSE_VERSION="${STAGE_COMPOSE_VERSION:-v2.32.4}"

# Architecture of the DAEMON (what the test container will run as), not of
# the shell running this script.
daemon_arch() {
  a="$(docker version --format '{{.Server.Arch}}' 2>/dev/null || true)"
  case "$a" in
    amd64) echo x86_64 ;;
    arm64) echo aarch64 ;;
    "")    case "$(uname -m)" in arm64|aarch64) echo aarch64 ;; *) echo x86_64 ;; esac ;;
    *)     echo "$a" ;;
  esac
}

# Locate the host's compose plugin. `docker info` lists client plugins with
# their paths; fall back to the conventional install locations.
find_compose_plugin() {
  p="$(docker info --format '{{range .ClientInfo.Plugins}}{{.Name}} {{.Path}}{{"\n"}}{{end}}' 2>/dev/null \
       | awk '$1=="compose"{print $2; exit}')"
  [ -n "$p" ] && [ -f "$p" ] && { echo "$p"; return 0; }
  for c in /usr/local/lib/docker/cli-plugins/docker-compose \
           /usr/libexec/docker/cli-plugins/docker-compose \
           /usr/lib/docker/cli-plugins/docker-compose \
           "$HOME/.docker/cli-plugins/docker-compose"; do
    [ -f "$c" ] && { echo "$c"; return 0; }
  done
  return 1
}

download_linux_builds() {
  arch="$(daemon_arch)"
  if [ ! -x "$DOCKER_BIN" ]; then
    bash "$RETRY" "https://download.docker.com/linux/static/stable/${arch}/docker-${DOCKER_VERSION}.tgz" \
      "$STAGE_DIR/docker-cli.tgz"
    tar xz -C "$STAGE_DIR" --strip-components=1 -f "$STAGE_DIR/docker-cli.tgz" docker/docker
    rm -f "$STAGE_DIR/docker-cli.tgz"
    chmod 755 "$DOCKER_BIN"
  fi
  if [ ! -x "$COMPOSE_PLUGIN" ]; then
    bash "$RETRY" \
      "https://github.com/docker/compose/releases/download/${COMPOSE_VERSION}/docker-compose-linux-${arch}" \
      "$COMPOSE_PLUGIN"
    chmod 755 "$COMPOSE_PLUGIN"
  fi
}

if [ "$(uname)" = "Linux" ] && host_cli="$(command -v docker 2>/dev/null)" && [ -n "$host_cli" ]; then
  cp -f "$(readlink -f "$host_cli")" "$DOCKER_BIN"
  chmod 755 "$DOCKER_BIN"
  if plugin="$(find_compose_plugin)"; then
    cp -f "$(readlink -f "$plugin")" "$COMPOSE_PLUGIN"
    chmod 755 "$COMPOSE_PLUGIN"
  else
    echo "stage-docker-cli: no compose plugin on this runner; downloading ${COMPOSE_VERSION}" >&2
    download_linux_builds
  fi
else
  download_linux_builds
fi

for f in "$DOCKER_BIN" "$COMPOSE_PLUGIN"; do
  if [ ! -f "$f" ] || [ ! -x "$f" ]; then
    echo "stage-docker-cli: failed to stage $f" >&2
    exit 1
  fi
done

printf 'DOCKER_BIN=%s\nCOMPOSE_PLUGIN=%s\n' "$DOCKER_BIN" "$COMPOSE_PLUGIN"
