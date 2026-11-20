#!/bin/bash

# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2024-2026 We-Amp B.V.

# Combined entrypoint: runs the factory worker AND nginx in one
# container (single `docker run`). The worker starts in the background; nginx
# runs in the foreground and proxies to the operator's backend. If either
# process exits, the container exits.

set -euo pipefail

DATA_DIR="${DATA_DIR:-/data}"

# Cross-process cache sharing is a GROUP relationship (fixed GID 918), not a
# world-writable one -- the optimizer runs as `pagespeed` and creates its
# volume and sockets 0660.  nginx's worker processes are in the same group.
# Mirrors docker/entrypoint-worker.sh; see docker/lib-cache-perms.sh.
_PS_LIB="$(dirname "$(readlink -f "${BASH_SOURCE[0]}")")/lib-cache-perms.sh"
[ -f "$_PS_LIB" ] || _PS_LIB=/docker/lib-cache-perms.sh
# shellcheck source=docker/lib-cache-perms.sh
. "$_PS_LIB"
mkdir -p "$DATA_DIR"
ps_prepare_data_dir "$DATA_DIR" "$DATA_DIR/cache.vol"
ps_check_api_port "${PAGESPEED_API_PORT:-}"

# --- Worker arguments (mirrors docker/entrypoint-worker.sh).
WARGS=(
  --socket "$DATA_DIR/pagespeed.sock"
  --cache-path "$DATA_DIR/cache.vol"
  --cache-size "${CACHE_SIZE:-1073741824}"
  --max-connections "${PAGESPEED_MAX_CONNECTIONS:-128}"
  --log-format "${PAGESPEED_LOG_FORMAT:-json}"
  --log-level "${PAGESPEED_LOG_LEVEL:-info}"
)
# BREAKING in 2.1: the bind default flipped from 0.0.0.0 to
# loopback, and a non-loopback bind now needs BOTH a token and
# PAGESPEED_API_ALLOW_REMOTE=true or the daemon refuses to start.  Mirrors
# docker/entrypoint-worker.sh.
# Exactly one transport. The daemon refuses this combination
# at startup, but a container that exits from deep inside the worker's config
# parse is much harder to read than one that says so here -- and a compose
# file setting both is a mistake worth naming before anything else runs.
if [ -n "${PAGESPEED_API_PORT:-}" ] && [ -n "${PAGESPEED_API_SOCKET:-}" ]; then
  echo "ERROR: PAGESPEED_API_PORT and PAGESPEED_API_SOCKET are both set." >&2
  echo "ERROR: The management API serves exactly one transport. Keep" >&2
  echo "ERROR: PAGESPEED_API_SOCKET for local access (no credential needed)," >&2
  echo "ERROR: or PAGESPEED_API_PORT for a TCP listener -- not both." >&2
  exit 1
fi
[ -n "${PAGESPEED_API_PORT:-}" ] && WARGS+=(--api-port "$PAGESPEED_API_PORT" --api-bind "${PAGESPEED_API_BIND:-127.0.0.1}")
[ -n "${PAGESPEED_API_SOCKET:-}" ] && WARGS+=(--api-socket "$PAGESPEED_API_SOCKET")
[ "${PAGESPEED_API_ALLOW_REMOTE:-false}" = "true" ] && WARGS+=(--api-allow-remote)
[ "${PAGESPEED_API_NO_AUTH:-false}" = "true" ] && WARGS+=(--api-no-auth)
# Per-start credentials when the operator supplied none -- and PRINTED, once.
# A generated credential nobody can read is not a credential: it only turns
# "no token configured" into "authentication required" and leaves the operator
# with no way in.  Mirrors docker/entrypoint-worker.sh.  Passed by
# ENVIRONMENT, never argv (/proc/<pid>/cmdline).
ps_gen_token() { head -c 32 /dev/urandom | base64 | tr '+/' '-_' | tr -d '=\n'; }
PS_GENERATED_API=""
PS_GENERATED_PURGE=""
if { [ -n "${PAGESPEED_API_PORT:-}" ] || [ -n "${PAGESPEED_API_SOCKET:-}" ]; } \
   && [ -z "${PAGESPEED_API_TOKEN:-}" ] \
   && [ "${PAGESPEED_API_NO_AUTH:-false}" != "true" ]; then
  PAGESPEED_API_TOKEN="$(ps_gen_token)"
  PS_GENERATED_API="yes"
fi
if [ -z "${PAGESPEED_PURGE_TOKEN:-}" ]; then
  PAGESPEED_PURGE_TOKEN="$(ps_gen_token)"
  PS_GENERATED_PURGE="yes"
fi
if [ -n "$PS_GENERATED_API" ] || [ -n "$PS_GENERATED_PURGE" ]; then
  echo "=============================================================================="
  echo "PAGESPEED CREDENTIALS generated for this container start."
  echo "They change on every restart -- set them yourself for stable values."
  [ -n "$PS_GENERATED_API" ] && \
    echo "  PAGESPEED_API_TOKEN=$PAGESPEED_API_TOKEN   (Authorization: Bearer <token>)"
  [ -n "$PS_GENERATED_PURGE" ] && \
    echo "  PAGESPEED_PURGE_TOKEN=$PAGESPEED_PURGE_TOKEN   (AUTH <token> on the .mgmt socket)"
  echo "=============================================================================="
fi
[ -n "${PAGESPEED_API_TOKEN:-}" ] && export PAGESPEED_API_TOKEN
export PAGESPEED_PURGE_TOKEN
[ "${PAGESPEED_API_READ_OPEN:-false}" = "true" ] && WARGS+=(--api-read-open)
[ -n "${PAGESPEED_CONSOLE_DIR:-}" ] && WARGS+=(--console-dir "$PAGESPEED_CONSOLE_DIR")
[ "${PAGESPEED_ENABLE_BROWSER_ANALYSIS:-false}" = "true" ] && WARGS+=(--enable-browser-analysis)
[ "${PAGESPEED_ALLOW_PRIVATE_URLS:-false}" = "true" ] && WARGS+=(--allow-private-urls)

# nginx's master keeps root (it binds :80/:443 and drops its own workers to
# `nginx`, which is in group `pagespeed`); only the optimizer drops here.
(ps_exec_worker factory_worker "${WARGS[@]}") &
WORKER_PID=$!

# Wait for the worker socket (up to ~30s); bail if the worker dies first.
for _ in $(seq 1 60); do
  [ -S "$DATA_DIR/pagespeed.sock" ] && break
  kill -0 "$WORKER_PID" 2>/dev/null || { echo "worker exited before its socket was ready" >&2; exit 1; }
  sleep 0.5
done
[ -S "$DATA_DIR/pagespeed.sock" ] || { echo "worker socket not ready after 30s" >&2; exit 1; }

# Render the nginx config (BACKEND only; everything else nginx needs is
# auto-discovered from the worker's shared config).
if [ -f /etc/nginx/nginx.conf.template ] && [ -w /etc/nginx/nginx.conf ]; then
  envsubst '${BACKEND_HOST} ${BACKEND_PORT}' < /etc/nginx/nginx.conf.template > /etc/nginx/nginx.conf
fi
nginx -t

trap 'kill -TERM "$WORKER_PID" 2>/dev/null || true; nginx -s quit 2>/dev/null || true' TERM INT
nginx -g 'daemon off;' &
NGINX_PID=$!
wait -n "$WORKER_PID" "$NGINX_PID"
EXIT=$?
echo "a managed process exited (status $EXIT); stopping container" >&2
# Stop the surviving process and reap both so neither lingers as a zombie
# (this script is PID 1). The wait is bounded by the processes' own shutdown.
kill -TERM "$WORKER_PID" "$NGINX_PID" 2>/dev/null || true
wait "$WORKER_PID" "$NGINX_PID" 2>/dev/null || true
exit "$EXIT"
