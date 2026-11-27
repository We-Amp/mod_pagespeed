#!/bin/bash
# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2024-2026 We-Amp B.V.
#
# Async-CSS probe worker entrypoint — delegates to the canonical entrypoint.
#
# API posture contract: docker/entrypoint-worker.sh. A non-loopback bind needs
# PAGESPEED_API_ALLOW_REMOTE=true AND a token of at least 16 characters;
# tools/ci/check-entrypoint-api-posture.sh enforces that across every copy.

set -euo pipefail

export DATA_DIR=/shared

# Both extra worker flags are CLI-only (--unsafe-force-async-css by design, see
# src/worker/unsafe_force_async_css.h; --host-alias is what the worker writes
# out as pagespeed-hosts.conf), and the canonical entrypoint owns the argv
# (EULA, socket paths) and resolves `factory_worker` through PATH.  A PATH shim
# appends the flags without forking that argv construction and without adding a
# generic extra-args hook to the shipped image.
EXTRA=""

# Host name confirmation.  The optimizer uses a cached stylesheet for critical
# CSS only on a host name the web server has confirmed, and the probe's server
# block is a catch-all, which confirms nothing.  Visitors reach nginx on the
# published port (PROBE_NGINX_PORT -> 8080), so, as the docs tell an operator
# behind a port mapping, name the host and port visitors use in the host alias
# file, mapped to the name and port the worker itself can reach (the nginx
# service on the compose network).
if [ -n "${PROBE_VISITOR_HOST:-}" ]; then
    EXTRA="$EXTRA --host-alias ${PROBE_VISITOR_HOST}=${PROBE_WORKER_HOST:-nginx:8080}"
    echo "[probe] host alias ${PROBE_VISITOR_HOST} -> ${PROBE_WORKER_HOST:-nginx:8080}" >&2
fi

if [ "${ASYNC_CSS_PROBE_FORCE:-0}" = "1" ]; then
    EXTRA="$EXTRA --unsafe-force-async-css"
    echo "[probe] forcing async-CSS deferral (--unsafe-force-async-css)" >&2
fi

if [ -n "$EXTRA" ]; then
    mkdir -p /tmp/probe-bin
    cat > /tmp/probe-bin/factory_worker <<SHIM
#!/bin/sh
exec /usr/local/bin/factory_worker "\$@" $EXTRA
SHIM
    chmod +x /tmp/probe-bin/factory_worker
    export PATH="/tmp/probe-bin:$PATH"
fi

exec /docker/entrypoint-worker.sh
