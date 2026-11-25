#!/bin/bash

# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2024-2026 We-Amp B.V.

# Canonical entrypoint for the mod_pagespeed factory worker.
#
# Sets up permissions for cross-process cache sharing, then starts
# the worker daemon. ALL behavior is controlled via environment variables
# so the same script works for production, staging, testing, and demos.
#
# Environment variables (all optional, sensible defaults):
#   DATA_DIR                          - Shared data directory (default: /data)
#   CACHE_SIZE                        - Cache size in bytes (default: 1GB)
#   PAGESPEED_MAX_CONNECTIONS         - Max concurrent connections (default: 128)
#   PAGESPEED_LOG_FORMAT              - Log format: json|text (default: json)
#   PAGESPEED_LOG_LEVEL               - Log level: debug|info|warning|error (default: info)
#   PAGESPEED_API_PORT                - HTTP API TCP port (omit to disable).
#                                       MUTUALLY EXCLUSIVE with PAGESPEED_API_SOCKET.
#   PAGESPEED_API_BIND                - API bind address (default: 127.0.0.1).
#                                       CHANGED in 2.1: was 0.0.0.0. Publishing the
#                                       API off-host now needs PAGESPEED_API_ALLOW_REMOTE=true
#                                       AND a token; see the migration note in CHANGELOG.md.
#   PAGESPEED_API_SOCKET              - Serve the API over a unix socket at this path
#                                       ("1"/"true"/"default" = the packaged path,
#                                       "0"/"false"/"off"/"no" = disabled). Mode 0660;
#                                       reaching the socket IS the credential, so no
#                                       token is needed (mutating requests still send
#                                       X-Requested-With: XMLHttpRequest).
#                                       MUTUALLY EXCLUSIVE with PAGESPEED_API_PORT --
#                                       setting both is a startup error.
#   PAGESPEED_API_ALLOW_REMOTE        - Confirm a deliberate non-loopback bind (default: false)
#   PAGESPEED_API_NO_AUTH             - Confirm a deliberate tokenless local API (default: false)
#   PAGESPEED_API_TOKEN               - API bearer token. When the API is enabled and no
#                                       token is supplied, one is GENERATED per container
#                                       start and written to $DATA_DIR/generated-tokens
#                                       (mode 0600); the log names the file, not the value.
#   PAGESPEED_PURGE_TOKEN             - Token for PURGE on the .mgmt socket. Generated per
#                                       container start when unset and written to the same
#                                       file, so cache invalidation works out of the box.
#   PAGESPEED_API_READ_OPEN           - Allow unauthenticated reads (default: false)
#   PAGESPEED_BROWSER_SANDBOX         - Headless Chrome sandbox: require|off (default: require).
#                                       The optimizer runs as the unprivileged `pagespeed` user,
#                                       so Chrome CAN sandbox itself here and `require` works out
#                                       of the box. Leave it alone unless your container runtime
#                                       blocks user namespaces, in which case /v1/health reports
#                                       browser_sandbox: "unavailable" and names the cause.
#   PAGESPEED_ADOPT_VOLUME            - Whether the entrypoint may take ownership of cache
#                                       content left by a pre-2.1 release on a shared volume:
#                                       auto (default) | off. See docker/lib-cache-perms.sh.
#   PAGESPEED_CONSOLE_DIR             - Path to web console SPA (omit to disable)
#   PAGESPEED_ENABLE_BROWSER_ANALYSIS - Enable headless browser (default: false)
#   PAGESPEED_AGENT_OPTIMIZE          - Enable agent_optimize markdown render
#                                       (default: false). Also requires
#                                       --enable-browser-analysis. The flag alone enables
#                                       it -- no token or entitlement.
#   PAGESPEED_AGENT_OPTIMIZE_LLMS_TXT - Enable the synthesized /llms.txt site-index
#                                       (default: false). Requires PAGESPEED_AGENT_OPTIMIZE.
#   PAGESPEED_AGENT_RENDER_PRIVATE_ORIGINS - Own origins the agent render may fetch at a
#                                       loopback or private address (comma-separated
#                                       scheme://host[:port] or host[:port]; default: none).
#   PAGESPEED_WEB_BOT_AUTH            - Enable the observe-only Web Bot Auth classifier
#                                       (default: false). The verdict never
#                                       changes request handling.
#   PAGESPEED_WEB_BOT_AUTH_KEY_DIRECTORIES - Comma-separated https:// JWKS key-directory
#                                       URLs, no spaces (each becomes a
#                                       --web-bot-auth-key-directory flag)
#   PAGESPEED_RSL_CAP_ENFORCEMENT     - Enable the experimental RSL-CAP capability-token
#                                       enforcement handler (default: false). Validates
#                                       Authorization: License tokens -> 401/402/pass.
#   PAGESPEED_RSL_CAP_KEY_DIRECTORIES - Comma-separated https:// JWKS key-directory URLs
#                                       for the token issuers (repeated --rsl-cap-key-directory)
#   PAGESPEED_RSL_CAP_REQUESTED_LICENSE - License id a token must grant to be authorized
#   PAGESPEED_RSL_CAP_REQUESTED_SCOPE - Scope a token must grant to be authorized
#   PAGESPEED_RSL_CAP_ISSUER          - Optional issuer pin (reject tokens with other iss)
#   PAGESPEED_WEB_BOT_AUTH_VERIFIED_BOTS - "keyid=name,keyid2=name2" operator-curated
#                                       map promoting verified key ids to bot names
#   PAGESPEED_ALLOW_PRIVATE_URLS      - Allow RFC 1918 URLs (default: false)
#   PAGESPEED_NO_ASYNC_CSS            - Disable async CSS loading (default: false)
#   PAGESPEED_ASYNC_CSS_MIN_COVERAGE  - Min critical/total CSS coverage to defer a
#                                       stylesheet, 0-1 (default: 0.10; 0 = off)
#   PAGESPEED_ASYNC_CSS_MIN_DEFERRED_BYTES - Always defer sheets below this size
#                                       regardless of coverage (default: 15000)
#   PAGESPEED_PROACTIVE_IMAGE_VARIANTS - Generate all image formats eagerly (default: false)
#   PAGESPEED_SVG_MODE                - SVG optimization mode: off|auto (omit for default)

set -euo pipefail

DATA_DIR="${DATA_DIR:-/data}"

# Cross-process cache sharing is a GROUP relationship, not a world-writable
# one: the daemon runs as `pagespeed` and creates its volume and sockets 0660,
# its shared config 0640.  The nginx image puts its worker user in the same
# fixed group (GID 918), so 0660 means "shared with the peer".  This call
# establishes that on $DATA_DIR, adopts a volume left by a pre-2.1 release
# where that is safe, and refuses early and actionably where it is not.
#
# The library sits beside this script in the release images.  The test
# harnesses under tools/ COPY only the entrypoint into their own images and
# exec it; there the library is absent and the pre-2.1 world-writable sharing
# is kept, because those images have no `pagespeed` identity to share WITH.
_PS_LIB="$(dirname "$(readlink -f "${BASH_SOURCE[0]}")")/lib-cache-perms.sh"
if [ -f "$_PS_LIB" ]; then
  # shellcheck source=docker/lib-cache-perms.sh
  . "$_PS_LIB"
else
  ps_prepare_data_dir() {
    echo "NOTE: no cache-perms library beside this entrypoint; keeping pre-2.1"
    echo "NOTE: world-writable cache sharing. Development images only."
    chmod 777 "$1"; umask 0000; touch "$2"; chmod 666 "$2"
  }
  ps_check_api_port() { :; }
  ps_exec_worker() { exec "$@"; }
fi
mkdir -p "$DATA_DIR"
ps_prepare_data_dir "$DATA_DIR" "$DATA_DIR/cache.vol"
ps_check_api_port "${PAGESPEED_API_PORT:-}"

# Build command line arguments.
ARGS=(
    --socket "$DATA_DIR/pagespeed.sock"
    --cache-path "$DATA_DIR/cache.vol"
    --cache-size "${CACHE_SIZE:-1073741824}"
    --max-connections "${PAGESPEED_MAX_CONNECTIONS:-128}"
    --log-format "${PAGESPEED_LOG_FORMAT:-json}"
    --log-level "${PAGESPEED_LOG_LEVEL:-info}"
)

# HTTP Management API.
#
# BREAKING in 2.1: the bind default flipped from 0.0.0.0 to 127.0.0.1, and the
# daemon now REFUSES TO START on a non-loopback bind unless BOTH a token is set
# and PAGESPEED_API_ALLOW_REMOTE=true.  The flagship image used to publish an
# unauthenticated management API -- including cache purge -- to anything that
# could reach the container.
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
[ -n "${PAGESPEED_API_PORT:-}" ] && ARGS+=(--api-port "$PAGESPEED_API_PORT" --api-bind "${PAGESPEED_API_BIND:-127.0.0.1}")
[ -n "${PAGESPEED_API_SOCKET:-}" ] && ARGS+=(--api-socket "$PAGESPEED_API_SOCKET")
[ "${PAGESPEED_API_ALLOW_REMOTE:-false}" = "true" ] && ARGS+=(--api-allow-remote)
[ "${PAGESPEED_API_NO_AUTH:-false}" = "true" ] && ARGS+=(--api-no-auth)

# Tokens: install-time generation is meaningless in a container (no persistent
# /etc), so generate PER START when the operator supplied none -- and make
# them READABLE (see the token file below). A generated credential nobody can
# read is not a credential: it just turns "no token configured" into
# "authentication required" and leaves the operator with no way in.
ps_gen_token() { head -c 32 /dev/urandom | base64 | tr '+/' '-_' | tr -d '=\n'; }
PS_GENERATED_API=""
PS_GENERATED_PURGE=""
if { [ -n "${PAGESPEED_API_PORT:-}" ] || [ -n "${PAGESPEED_API_SOCKET:-}" ]; } \
   && [ -z "${PAGESPEED_API_TOKEN:-}" ] \
   && [ "${PAGESPEED_API_NO_AUTH:-false}" != "true" ]; then
  PAGESPEED_API_TOKEN="$(ps_gen_token)"
  PS_GENERATED_API="yes"
fi
# PURGE over the management socket is fail-closed without a token; generate one
# per start too, so a stock container can actually invalidate its cache.
if [ -z "${PAGESPEED_PURGE_TOKEN:-}" ]; then
  PAGESPEED_PURGE_TOKEN="$(ps_gen_token)"
  PS_GENERATED_PURGE="yes"
fi
# The generated values go to an owner-only file in the data directory, NOT to
# this log: container logs are routinely shipped off-host and kept, and a
# credential in them outlives the container. The log names the file and how
# to read it. The file is rewritten on every start; when nothing was generated
# (the operator set both), a stale one from an earlier start is removed.
# printf is a builtin, so the values never appear in any argv.
PS_TOKEN_FILE="$DATA_DIR/generated-tokens"
ps_write_generated_tokens() {
  local tmp
  tmp="$(mktemp "$DATA_DIR/.generated-tokens.XXXXXX")" || return 1
  if {
       if [ -n "$PS_GENERATED_API" ]; then
         printf 'PAGESPEED_API_TOKEN=%s\n' "$PAGESPEED_API_TOKEN"
       fi
       if [ -n "$PS_GENERATED_PURGE" ]; then
         printf 'PAGESPEED_PURGE_TOKEN=%s\n' "$PAGESPEED_PURGE_TOKEN"
       fi
     } > "$tmp" && chmod 0600 "$tmp" && mv -f "$tmp" "$PS_TOKEN_FILE"; then
    return 0
  fi
  rm -f "$tmp"
  return 1
}
if [ -n "$PS_GENERATED_API" ] || [ -n "$PS_GENERATED_PURGE" ]; then
  echo "=============================================================================="
  echo "PAGESPEED CREDENTIALS generated for this container start:"
  [ -n "$PS_GENERATED_API" ] && \
    echo "  PAGESPEED_API_TOKEN     (Authorization: Bearer <token>)"
  [ -n "$PS_GENERATED_PURGE" ] && \
    echo "  PAGESPEED_PURGE_TOKEN   (AUTH <token> on the .mgmt socket)"
  if ps_write_generated_tokens; then
    echo "Written to $PS_TOKEN_FILE (mode 0600, one KEY=value per line). Read them with:"
    echo "  docker exec <container> cat $PS_TOKEN_FILE"
  else
    echo "WARNING: could not write them to $PS_TOKEN_FILE; set PAGESPEED_API_TOKEN" >&2
    echo "WARNING: and PAGESPEED_PURGE_TOKEN yourself to know their values." >&2
  fi
  echo "They change on every restart -- set them yourself for stable values."
  echo "=============================================================================="
else
  rm -f "$PS_TOKEN_FILE"
fi
# Deliberately NOT --api-token: the daemon reads both from the ENVIRONMENT,
# which keeps them out of /proc/<pid>/cmdline. That matters beyond this
# container: the shipped Helm pod and compose files put the worker in a PID
# namespace shared with nginx, so every process there can read the argv.
[ -n "${PAGESPEED_API_TOKEN:-}" ] && export PAGESPEED_API_TOKEN
export PAGESPEED_PURGE_TOKEN

[ "${PAGESPEED_API_READ_OPEN:-false}" = "true" ] && ARGS+=(--api-read-open)
[ -n "${PAGESPEED_CONSOLE_DIR:-}" ] && ARGS+=(--console-dir "$PAGESPEED_CONSOLE_DIR")

# Browser analysis.
[ "${PAGESPEED_ENABLE_BROWSER_ANALYSIS:-false}" = "true" ] && ARGS+=(--enable-browser-analysis)
[ "${PAGESPEED_ALLOW_PRIVATE_URLS:-false}" = "true" ] && ARGS+=(--allow-private-urls)

# Agent optimize: markdown content-negotiation render + /llms.txt index.
# Gated additionally by --enable-browser-analysis; the operator flags alone enable it.
[ "${PAGESPEED_AGENT_OPTIMIZE:-false}" = "true" ] && ARGS+=(--agent-optimize)
[ "${PAGESPEED_AGENT_OPTIMIZE_LLMS_TXT:-false}" = "true" ] && ARGS+=(--agent-optimize-llms-txt)
[ -n "${PAGESPEED_AGENT_RENDER_PRIVATE_ORIGINS:-}" ] && ARGS+=(--agent-render-private-origins "$PAGESPEED_AGENT_RENDER_PRIVATE_ORIGINS")

# Web Bot Auth: observe-only RFC 9421 crawler verification.
# The worker rejects non-https key-directory URLs at startup.
[ "${PAGESPEED_WEB_BOT_AUTH:-false}" = "true" ] && ARGS+=(--web-bot-auth)
if [ -n "${PAGESPEED_WEB_BOT_AUTH_KEY_DIRECTORIES:-}" ]; then
  IFS=',' read -r -a WBA_DIRS <<< "$PAGESPEED_WEB_BOT_AUTH_KEY_DIRECTORIES"
  for wba_dir in "${WBA_DIRS[@]}"; do
    if [ -n "$wba_dir" ]; then
      ARGS+=(--web-bot-auth-key-directory "$wba_dir")
    fi
  done
fi
[ -n "${PAGESPEED_WEB_BOT_AUTH_VERIFIED_BOTS:-}" ] && ARGS+=(--web-bot-auth-verified-bots "$PAGESPEED_WEB_BOT_AUTH_VERIFIED_BOTS")

# RSL-CAP enforcement (experimental): validate Authorization:License
# capability tokens and return 401/402/pass (status codes only).  Off by
# default.  The worker rejects non-https key-directory URLs at startup.
[ "${PAGESPEED_RSL_CAP_ENFORCEMENT:-false}" = "true" ] && ARGS+=(--rsl-cap-enforcement)
if [ -n "${PAGESPEED_RSL_CAP_KEY_DIRECTORIES:-}" ]; then
  IFS=',' read -r -a RSL_DIRS <<< "$PAGESPEED_RSL_CAP_KEY_DIRECTORIES"
  for rsl_dir in "${RSL_DIRS[@]}"; do
    if [ -n "$rsl_dir" ]; then
      ARGS+=(--rsl-cap-key-directory "$rsl_dir")
    fi
  done
fi
[ -n "${PAGESPEED_RSL_CAP_REQUESTED_LICENSE:-}" ] && ARGS+=(--rsl-cap-requested-license "$PAGESPEED_RSL_CAP_REQUESTED_LICENSE")
[ -n "${PAGESPEED_RSL_CAP_REQUESTED_SCOPE:-}" ] && ARGS+=(--rsl-cap-requested-scope "$PAGESPEED_RSL_CAP_REQUESTED_SCOPE")
[ -n "${PAGESPEED_RSL_CAP_ISSUER:-}" ] && ARGS+=(--rsl-cap-issuer "$PAGESPEED_RSL_CAP_ISSUER")

# Async CSS / FOUC sufficiency gate.
[ "${PAGESPEED_NO_ASYNC_CSS:-false}" = "true" ] && ARGS+=(--no-async-css)
[ -n "${PAGESPEED_ASYNC_CSS_MIN_COVERAGE:-}" ] && ARGS+=(--async-css-min-coverage "$PAGESPEED_ASYNC_CSS_MIN_COVERAGE")
[ -n "${PAGESPEED_ASYNC_CSS_MIN_DEFERRED_BYTES:-}" ] && ARGS+=(--async-css-min-deferred-bytes "$PAGESPEED_ASYNC_CSS_MIN_DEFERRED_BYTES")

# Image/SVG processing.
[ "${PAGESPEED_PROACTIVE_IMAGE_VARIANTS:-false}" = "true" ] && ARGS+=(--proactive-image-variants)
[ -n "${PAGESPEED_SVG_MODE:-}" ] && ARGS+=(--svg-mode "$PAGESPEED_SVG_MODE")

ps_exec_worker factory_worker "${ARGS[@]}"
